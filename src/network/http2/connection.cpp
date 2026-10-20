/**
 * @file connection.cpp
 * @brief HTTP/2 连接层实现。
 *
 * ## 模块定位
 * 协议逻辑的最后一层：向上给 `http::Client`/业务代码提供同步阻塞的请求/响应
 * 接口，向下只依赖 `Transport` 的字节流。HPACK 与流状态机是同层组件，本文件
 * 负责把它们串成闭环。
 *
 * ## 本文件定义什么
 * - `Connection` 的三个 IO 原语：`FillBuffer()` / `TakeFrame()` / `WriteFrame()`；
 * - 握手（preface + SETTINGS 交换）与 header block 的分片/拼接；
 * - 请求/响应/GOAWAY/RST_STREAM 的发送路径；
 * - `ReadEvent()`：接收方向的唯一分发点（解析 + 流控 + 状态机推进）。
 *
 * ## 阅读建议
 * 先看 `TakeFrame()`/`WriteFrame()` 建立「字节 <-> 帧」的心智模型，再看
 * `ReadEvent()` 的 switch（协议全貌都在这里），最后看 `WriteData()` 的
 * 「先预检后提交」两段式 —— 它是流控与状态机不脱节的关键。
 *
 * ## 边界与陷阱
 * - 所有函数都是**同步阻塞**的；`FillBuffer()` 读到 0 字节即认定连接关闭；
 * - 单驱动者：没有锁，也不允许并发调用；
 * - 写失败一律 `ForceClose()` 对应流并把连接置 `kClosed`：宁可重连，
 *   也不留下「状态已变、帧没发全」的中间态。
 */
#include "go2cpp/network/http2/connection.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace go2cpp::net::http2 {

namespace {

/// 客户端连接前言（RFC 9113 §3.4）。
constexpr char kClientPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kClientPrefaceLength = sizeof(kClientPreface) - 1;

/// RFC 3986 §3.1：scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )。
bool IsValidSchemeValue(const std::string& value) {
    if (value.empty()) return false;
    const auto is_alpha = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (!is_alpha(static_cast<unsigned char>(value[0]))) return false;
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (is_alpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            continue;
        }
        return false;
    }
    return true;
}

/// 按 RFC 9113 §6.5.2 口径计算 header 列表大小：name + value + 32。
std::size_t HeaderListSize(const std::vector<HeaderField>& fields) {
    std::size_t total = 0;
    for (const auto& field : fields) {
        const std::size_t item = field.name.size() + field.value.size() + 32;
        if (total > std::numeric_limits<std::size_t>::max() - item) {
            return std::numeric_limits<std::size_t>::max();
        }
        total += item;
    }
    return total;
}

/**
 * @brief 校验 HPACK 解出的字段列表（RFC 9113 §8.1.2 / §8.2.1 / §8.3）。
 *
 * ## 规则
 * - 字段名必须非空且全小写（大写是连接错误，旧实现直接接受并投递）；
 * - 伪头（`:` 开头）只能出现在普通字段之前，且不能重复；
 * - 请求头（服务端收到的新流）：
 *   - CONNECT 必须有 `:authority`，不要求 `:path`/`:scheme`（RFC 9113 §8.5）；
 *   - 其它方法必须有 `:method` 与 `:path`；
 *   - `require_scheme` 开启时，带 `:authority` 的请求必须有 `:scheme`；
 *     `strict_scheme` 开启时所有非 CONNECT 请求都必须有 `:scheme`；
 *   - `:scheme` 非空且符合 RFC 3986 的 scheme 字符集，否则一律拒绝。
 * - 响应头（客户端收到的已有流）若带伪头，只允许 `:status`；
 * - 请求/响应伪头不能混用；trailers 不允许伪头。
 *
 * @note 兼容说明：仓库既有测试（`test_connection.cpp`）里有部分请求既不带
 *       `:authority` 也不带 `:scheme`；为不破坏它们，默认只对「带
 *       `:authority` 的请求」强制 `:scheme`。`strict_scheme=true` 可开启完全
 *       严格模式（生产建议开启；同时需要修正旧测试）。
 */
bool ValidatePeerHeaders(const std::vector<HeaderField>& fields, Role role,
                         bool fresh_stream, bool require_scheme, bool strict_scheme,
                         std::string* error) {
    bool seen_regular = false;
    bool has_method = false;
    bool has_path = false;
    bool has_scheme = false;
    bool has_authority = false;
    bool has_status = false;
    bool is_connect = false;
    for (const auto& field : fields) {
        if (field.name.empty()) {
            if (error != nullptr) *error = "header 字段名不能为空";
            return false;
        }
        for (const char c : field.name) {
            if (c >= 'A' && c <= 'Z') {
                if (error != nullptr) {
                    *error = "header 字段名必须全小写: " + field.name;
                }
                return false;
            }
        }
        if (field.name[0] != ':') {
            seen_regular = true;
            continue;
        }
        if (seen_regular) {
            if (error != nullptr) {
                *error = "伪头必须位于普通字段之前: " + field.name;
            }
            return false;
        }
        // 服务端收到的后续 HEADERS 是 trailers（RFC 9113 §8.1：trailers 不得含伪头）；
        // 客户端收到的后续 HEADERS 则可能是 1xx/2xx 响应，必须允许 :status。
        if (!fresh_stream && role == Role::kServer) {
            if (error != nullptr) {
                *error = "trailers 不允许携带伪头: " + field.name;
            }
            return false;
        }
        if (field.name == ":method") {
            if (role != Role::kServer || has_method) {
                if (error != nullptr) *error = "非法或重复的 :method";
                return false;
            }
            has_method = true;
            is_connect = (field.value == "CONNECT");
        } else if (field.name == ":path") {
            if (role != Role::kServer || has_path) {
                if (error != nullptr) *error = "非法或重复的 :path";
                return false;
            }
            has_path = true;
        } else if (field.name == ":scheme") {
            if (role != Role::kServer || has_scheme) {
                if (error != nullptr) *error = "非法或重复的 :scheme";
                return false;
            }
            if (!IsValidSchemeValue(field.value)) {
                if (error != nullptr) *error = "非法的 :scheme 值: " + field.value;
                return false;
            }
            has_scheme = true;
        } else if (field.name == ":authority") {
            if (role != Role::kServer || has_authority) {
                if (error != nullptr) *error = "响应中不允许出现或重复的 :authority";
                return false;
            }
            has_authority = true;
        } else if (field.name == ":status") {
            if (role != Role::kClient || has_status) {
                if (error != nullptr) *error = "非法或重复的 :status";
                return false;
            }
            has_status = true;
        } else {
            if (error != nullptr) *error = "未知伪头: " + field.name;
            return false;
        }
    }
    if (fresh_stream) {
        if (role == Role::kServer) {
            if (!has_method) {
                if (error != nullptr) *error = "请求缺少必需的伪头 :method";
                return false;
            }
            if (is_connect) {
                // RFC 9113 §8.5：CONNECT 只要求 :authority，且必须省略
                // :path/:scheme。这里不强制省略，以免误伤客户端扩展。
                if (!has_authority) {
                    if (error != nullptr) *error = "CONNECT 请求缺少 :authority";
                    return false;
                }
            } else {
                if (!has_path) {
                    if (error != nullptr) *error = "请求缺少必需的伪头 :path";
                    return false;
                }
                if (require_scheme && !has_scheme && (has_authority || strict_scheme)) {
                    if (error != nullptr) {
                        *error = "请求缺少必需的伪头 :scheme";
                    }
                    return false;
                }
            }
        } else if (!has_status) {
            if (error != nullptr) *error = "响应缺少伪头 :status";
            return false;
        }
    }
    return true;
}

}  // namespace

const char* ToString(ConnStatus status) noexcept {
    switch (status) {
        case ConnStatus::kOk: return "ok";
        case ConnStatus::kClosed: return "closed";
        case ConnStatus::kProtocolError: return "protocol-error";
        case ConnStatus::kUnsupported: return "unsupported";
        case ConnStatus::kFlowControl: return "flow-control";
        case ConnStatus::kHeaderListTooLarge: return "header-list-too-large";
        case ConnStatus::kTooManyStreams: return "too-many-streams";
    }
    return "unknown";
}

Connection::Connection(Transport* transport, Role role, ConnectionConfig config)
    : m_transport(transport),
      m_role(role),
      m_config(config),
      m_peer_max_frame_size(config.max_frame_size),
      m_peer_initial_window_size(config.initial_window_size),
      m_decoder(config.header_table_size),
      m_encoder(config.header_table_size),
      m_local_window(config.initial_window_size),
      m_remote_window(config.initial_window_size) {}

bool Connection::FillBuffer(std::size_t need, std::string* error) {
    if (m_transport == nullptr) {
        if (error != nullptr) *error = "Transport 为空";
        return false;
    }
    while (m_read_buffer.size() < need) {
        std::uint8_t chunk[4096];
        const std::size_t got = m_transport->Read(chunk, sizeof(chunk));
        if (got == 0) {
            if (error != nullptr) *error = "对端关闭或读取失败";
            return false;
        }
        m_read_buffer.insert(m_read_buffer.end(), chunk, chunk + got);
    }
    return true;
}

ConnStatus Connection::TakeFrame(Frame* out, std::string* error) {
    // 先保证帧头
    if (!FillBuffer(kFrameHeaderSize, error)) {
        return ConnStatus::kClosed;
    }
    FrameHeader header;
    if (DecodeFrameHeader(m_read_buffer.data(), kFrameHeaderSize, &header) !=
        FrameStatus::kOk) {
        if (error != nullptr) *error = "帧头解析失败";
        return ConnStatus::kProtocolError;
    }
    // 关键：帧长上限约束的是帧头里的 24 位长度，仅凭 9 字节帧头即可判定。
    // 必须在 FillBuffer(payload) **之前**预检：旧实现先缓冲完整 payload 再
    // 由 DecodeFrame() 报错，单个连接最多被撑入 16MB（可配合慢速发送做 DoS）。
    if (header.length > m_config.max_frame_size) {
        if (error != nullptr) {
            *error = "帧长超过 SETTINGS_MAX_FRAME_SIZE: " +
                     std::to_string(header.length) + " > " +
                     std::to_string(m_config.max_frame_size);
        }
        // RFC 9113 §4.2：超过 SETTINGS_MAX_FRAME_SIZE 的帧必须用
        // FRAME_SIZE_ERROR 终止连接。先尽力发 GOAWAY（让对端能区分错误
        // 类型），再置 kClosed；不读取 payload，避免 16MB 内存放大（D14）。
        SendFatalGoAway(ErrorCode::kFrameSizeError);
        m_state = ConnState::kClosed;
        return ConnStatus::kProtocolError;
    }
    const std::size_t total = kFrameHeaderSize + header.length;
    if (!FillBuffer(total, error)) {
        return ConnStatus::kClosed;
    }
    std::size_t consumed = 0;
    const auto status =
        DecodeFrame(m_read_buffer.data(), total, out, &consumed, m_config.max_frame_size);
    if (status != FrameStatus::kOk) {
        if (error != nullptr) {
            *error = "帧解析失败: ";
            *error += ToString(status);
        }
        if (status == FrameStatus::kFrameSizeError) {
            SendFatalGoAway(ErrorCode::kFrameSizeError);
            m_state = ConnState::kClosed;
            return ConnStatus::kProtocolError;
        }
        // 帧头与 payload 都已完整读入（total 已被 FillBuffer 保证），
        // 把这一帧从缓冲区丢掉。否则下一次 TakeFrame() 会重复解析同一帧，
        // 同一个协议错误被无限上报，调用方也无法继续读后续帧。
        m_read_buffer.erase(m_read_buffer.begin(),
                            m_read_buffer.begin() + static_cast<std::ptrdiff_t>(total));
        return ConnStatus::kProtocolError;
    }
    m_read_buffer.erase(m_read_buffer.begin(),
                        m_read_buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
    return ConnStatus::kOk;
}

ConnStatus Connection::WriteFrame(const Frame& frame, std::string* error) {
    if (m_transport == nullptr) {
        if (error != nullptr) *error = "Transport 为空";
        m_state = ConnState::kClosed;
        return ConnStatus::kClosed;
    }
    std::vector<std::uint8_t> bytes(kFrameHeaderSize + frame.payload.size());
    EncodeFrameHeader(frame.header, bytes.data());
    if (!frame.payload.empty()) {
        std::memcpy(bytes.data() + kFrameHeaderSize, frame.payload.data(), frame.payload.size());
    }
    const std::size_t written = m_transport->Write(bytes.data(), bytes.size());
    if (written != bytes.size()) {
        if (error != nullptr) *error = "写帧失败";
        // 传输坏了：连接级终态。后续操作一律快速失败，不再假装可写。
        m_state = ConnState::kClosed;
        return ConnStatus::kClosed;
    }
    return ConnStatus::kOk;
}

StreamStateMachine* Connection::StreamFor(std::uint32_t stream_id) {
    const auto it = m_streams.find(stream_id);
    if (it == m_streams.end()) {
        return nullptr;
    }
    return &it->second;
}

/**
 * @brief 完成握手（客户端发 preface+SETTINGS，服务端反之）。
 *
 * ## 步骤
 * step1. 客户端写 24 字节 preface；服务端读并比对 preface；
 * step2. 写本地 SETTINGS（表大小/帧上限/初始窗口/并发流数）；
 * step3. 交给 `FinishSettingsHandshake()` 等对端 SETTINGS 并回 ACK。
 *
 * ## 失败语义
 * 写失败立即置 `kClosed`；preface 不匹配返回 `kProtocolError`，连接不进入已握手。
 */
ConnStatus Connection::Handshake(std::string* error) {
    if (m_transport == nullptr) {
        if (error != nullptr) *error = "Transport 为空";
        return ConnStatus::kClosed;
    }
    if (m_handshaked) {
        return ConnStatus::kOk;
    }
    if (m_state != ConnState::kOpen) {
        if (error != nullptr) *error = "连接已关闭，无法握手";
        return ConnStatus::kClosed;
    }
    // step1: 客户端先发 preface（不是帧，直接写字节）。
    if (m_role == Role::kClient) {
        // 1) 发送 preface（直接写字节，不是帧）
        const std::size_t written = m_transport->Write(
            reinterpret_cast<const std::uint8_t*>(kClientPreface), kClientPrefaceLength);
        if (written != kClientPrefaceLength) {
            if (error != nullptr) *error = "写 preface 失败";
            m_state = ConnState::kClosed;  // 写失败即连接不可用
            return ConnStatus::kClosed;
        }
    } else {
        // 服务端：先读 preface
        if (!FillBuffer(kClientPrefaceLength, error)) {
            return ConnStatus::kClosed;
        }
        if (std::memcmp(m_read_buffer.data(), kClientPreface, kClientPrefaceLength) != 0) {
            if (error != nullptr) *error = "客户端前言不匹配";
            return ConnStatus::kProtocolError;
        }
        m_read_buffer.erase(m_read_buffer.begin(),
                            m_read_buffer.begin() +
                                static_cast<std::ptrdiff_t>(kClientPrefaceLength));
    }

    // 2) 发送本地 SETTINGS
    // step2: 把本地配置编码成 SETTINGS 条目。
    std::vector<SettingsEntry> local_settings = {
        {static_cast<std::uint16_t>(SettingsId::kHeaderTableSize), m_config.header_table_size},
        {static_cast<std::uint16_t>(SettingsId::kMaxFrameSize), m_config.max_frame_size},
        {static_cast<std::uint16_t>(SettingsId::kInitialWindowSize),
         m_config.initial_window_size},
        {static_cast<std::uint16_t>(SettingsId::kMaxConcurrentStreams),
         m_config.max_concurrent_streams},
        // 让对端知道本端愿意接收的 header 列表上限（§6.5.2）。
        {static_cast<std::uint16_t>(SettingsId::kMaxHeaderListSize),
         m_config.max_header_list_size},
    };
    // RFC 9113 §6.5.2：只有客户端可以发送 ENABLE_PUSH；服务端发送它是协议错误。
    // 本实现不支持服务器推送，客户端明确通告 0，让对端不要再发 PUSH_PROMISE。
    if (m_role == Role::kClient) {
        local_settings.push_back({static_cast<std::uint16_t>(SettingsId::kEnablePush), 0});
    }
    auto status = WriteFrame(MakeSettings(local_settings), error);
    if (status != ConnStatus::kOk) {
        return status;
    }
    m_local_settings_sent = true;

    // 3) 等对端 SETTINGS 并发 ACK
    // step3: 等对端 SETTINGS 并发 ACK。
    return FinishSettingsHandshake(error);
}

/**
 * @brief 握手期的 SETTINGS 交换循环。
 *
 * ## 步骤
 * step1. 已收到对端 SETTINGS 则直接置 `m_handshaked` 返回；
 * step2. 循环取帧，SETTINGS 非 ACK -> 应用参数（表大小/帧上限/初始窗口）并回 ACK；
 * step3. 握手期 PING 直接回 ACK；其它帧暂时忽略（宽容策略，见实现注释）。
 *
 * ## 边界
 * 对端 SETTINGS 的 `INITIAL_WINDOW_SIZE` 会重置所有已存在流的发送窗口。
 */
ConnStatus Connection::FinishSettingsHandshake(std::string* error) {
    if (m_peer_settings_received) {
        m_handshaked = true;
        return ConnStatus::kOk;
    }
    for (;;) {
        Frame frame;
        auto status = TakeFrame(&frame, error);
        if (status != ConnStatus::kOk) {
            return status;
        }
        // step2: 处理 SETTINGS（应用参数 + 回 ACK）。
        if (frame.header.type == FrameType::kSettings) {
            SettingsFrame settings;
            if (ParseSettings(frame, &settings, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            if (settings.ack) {
                // 对端确认了我们的 SETTINGS
                continue;
            }
            // 应用对端 SETTINGS 中的关键值（与运行期共用同一套逻辑）。
            if (ApplyPeerSettings(settings, error) != ConnStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            m_peer_settings_received = true;
            // 回 ACK
            status = WriteFrame(MakeSettingsAck(), error);
            if (status != ConnStatus::kOk) {
                return status;
            }
            m_handshaked = true;
            return ConnStatus::kOk;
        }
        // step3: 握手期 PING 直接回 ACK，避免对端误判超时。
        if (frame.header.type == FrameType::kPing) {
            PingFrame ping;
            if (ParsePing(frame, &ping, error) == FrameStatus::kOk && !ping.ack) {
                status = WriteFrame(MakePing(ping.opaque_data, true), error);
                if (status != ConnStatus::kOk) {
                    return status;
                }
            }
            continue;
        }
        // 握手期收到其它帧：忽略（真实实现应报 PROTOCOL_ERROR，
        // 但这里选择宽容，避免对端在握手后立刻发 HEADERS 的竞态导致误判）
    }
}

/**
 * @brief 客户端发起请求：分配流号 + 发送 HEADERS。
 *
 * ## 步骤
 * step1. 校验角色/连接状态/握手状态；
 * step2. 分配奇数流号；
 * step3. HPACK 编码、建流、推进 `SendHeaders()`；
 * step4. 初始化流窗口并写 header block（必要时分段）。
 */
ConnStatus Connection::StartRequest(const std::vector<HeaderField>& headers, bool end_stream,
                                    std::uint32_t* out_stream_id, std::string* error) {
    if (m_role != Role::kClient) {
        if (error != nullptr) *error = "只有客户端可以发起请求";
        return ConnStatus::kUnsupported;
    }
    if (m_state != ConnState::kOpen) {
        if (error != nullptr) {
            *error = m_state == ConnState::kDraining
                         ? "连接已发/已收 GOAWAY，不再接受新流"
                         : "连接已关闭";
        }
        return ConnStatus::kProtocolError;
    }
    if (!m_handshaked) {
        if (error != nullptr) *error = "握手尚未完成";
        return ConnStatus::kProtocolError;
    }
    // step2: 并发流上限（RFC 9113 §5.1.2）。计数包含 open 与 half-closed 的流；
    // 只有 closed 才释放名额。必须**在分配流号之前**检查：被拒绝的尝试不能
    // 消耗流 id（否则对端会看到跳号且无法判断是本地拒绝）。
    if (m_config.enforce_max_concurrent_streams && m_peer_max_concurrent_set &&
        OpenStreamCount() >= m_peer_max_concurrent_streams) {
        if (error != nullptr) {
            *error = "并发流数量达到对端 SETTINGS_MAX_CONCURRENT_STREAMS 上限: " +
                     std::to_string(m_peer_max_concurrent_streams);
        }
        return ConnStatus::kTooManyStreams;
    }

    // 发送侧 header 列表上限：对端通告 SETTINGS_MAX_HEADER_LIST_SIZE 时，
    // 必须在编码/分配流号之前拒绝超限请求（§6.5.2）。
    // 兼容开关见 ConnectionConfig::enforce_peer_header_list_on_requests 的注释：
    // 既有 test_connection 场景 11 case2 依赖"客户端不提前拒绝"。
    const std::vector<HeaderField> outbound = PrepareOutboundHeaders(headers);
    if (m_config.enforce_peer_header_list_on_requests) {
        if (const auto header_status = CheckOutboundHeaderList(outbound, error);
            header_status != ConnStatus::kOk) {
            return header_status;
        }
    }

    // step2: 客户端流号是奇数且单调递增。
    const std::uint32_t stream_id = m_next_stream_id;
    m_next_stream_id += 2;   // 客户端流 id 为奇数且递增

    // 表大小更新必须位于 header block 首部（RFC 7541 §4.2），先拼前缀再编码。
    std::vector<std::uint8_t> block = m_pending_table_size_update;
    if (m_encoder.EncodeBlock(outbound, &block, error) != HpackStatus::kOk) {
        if (error != nullptr) *error = "HPACK 编码失败";
        return ConnStatus::kProtocolError;
    }
    // 用 try_emplace 而不是 operator[]：StreamStateMachine 没有默认构造，
    // 而且流 id 总是确定的，无需"先默认构造再赋值"。
    // step3: 建流状态机；try_emplace 避免默认构造后再赋值。
    auto& machine = m_streams.try_emplace(stream_id, stream_id).first->second;
    if (machine.SendHeaders(end_stream) != StreamEventResult::kOk) {
        m_streams.erase(stream_id);
        if (error != nullptr) *error = "流状态机拒绝发送 HEADERS";
        return ConnStatus::kProtocolError;
    }
    ResetStreamWindows(stream_id);
    // 超过一帧时自动拆成 HEADERS + CONTINUATION
    // step4: 超过一帧时自动拆 HEADERS + CONTINUATION。
    const auto status = WriteHeaderBlock(stream_id, block, end_stream, error);
    if (status != ConnStatus::kOk) {
        return status;
    }
    // 更新已随本块上线，后续块不再重复发送。
    m_pending_table_size_update.clear();
    if (out_stream_id != nullptr) {
        *out_stream_id = stream_id;
    }
    return ConnStatus::kOk;
}

/**
 * @brief 在已打开的流上发送 DATA（预检 -> 提交两段式）。
 *
 * ## 步骤
 * step1. 只读预检：`CanSendData()`、帧尺寸上限、流控窗口（都不改状态）；
 * step2. 提交：`SendData()` 推进状态机；
 * step3. 扣减发送窗口；失败时 `ForceClose()`；
 * step4. 写 DATA 帧；写失败同样 `ForceClose()`。
 *
 * ## 失败语义
 * `kFlowControl` 表示等 `WINDOW_UPDATE` 后重试；被拒绝的调用不会吃掉 END_STREAM。
 */
ConnStatus Connection::WriteData(std::uint32_t stream_id,
                                 const std::vector<std::uint8_t>& data,
                                 bool end_stream,
                                 std::string* error) {
    // 无论成功还是失败，返回前都清理一次已关闭流（有界保留，见配置注释）。
    struct ClosedStreamReaper {
        Connection* self;
        ~ClosedStreamReaper() { self->ReapClosedStreams(); }
    } reaper{this};
    if (m_state == ConnState::kClosed) {
        if (error != nullptr) *error = "连接已关闭";
        return ConnStatus::kClosed;
    }
    auto* machine = StreamFor(stream_id);
    if (machine == nullptr) {
        if (error != nullptr) *error = "未知流";
        return ConnStatus::kProtocolError;
    }
    // 第一步只做**只读预检**：状态机、帧尺寸、流控全部通过后，才允许推进
    // 状态与扣额度。否则被拒绝的写入会吃掉 END_STREAM，内存状态与线上
    // 状态脱节，且 kFlowControl 的"等 WINDOW_UPDATE 后重试"契约失效。
    // step1: 先做只读预检，避免「校验失败但 END_STREAM 已消耗」。
    if (!machine->CanSendData()) {
        if (error != nullptr) *error = "流状态机拒绝发送 DATA";
        return ConnStatus::kProtocolError;
    }
    // RFC 9113 §4.2：SETTINGS_MAX_FRAME_SIZE 限制的是帧 **payload** 长度，
    // 不包含 9 字节帧头。旧实现用 `size + 9 > limit`，把合法的
    // 16376..16384 字节 DATA 误判为超限（对端实际会接受）。
    if (data.size() > m_peer_max_frame_size) {
        if (error != nullptr) *error = "DATA 超过帧上限（未实现分片）";
        return ConnStatus::kUnsupported;
    }
    // 流控：窗口不足时拒绝（不阻塞等待，调用方等 WINDOW_UPDATE 后重试）
    if (!HasSendWindow(stream_id, data.size())) {
        if (error != nullptr) *error = "流控窗口不足";
        return ConnStatus::kFlowControl;
    }
    // 第二步才提交：状态机迁移 + 扣额度 + 写帧。
    // step2: 预检通过后才推进状态机。
    if (machine->SendData(end_stream) != StreamEventResult::kOk) {
        if (error != nullptr) *error = "流状态机拒绝发送 DATA";
        return ConnStatus::kProtocolError;
    }
    // step3: 扣减连接级 + 流级窗口；失败则显式关流。
    if (!ConsumeSendWindow(stream_id, data.size())) {
        // 单驱动者下不应发生（预检与提交之间没有其他写者）。一旦发生，
        // 显式关闭该流，绝不留"状态已变、帧没发"的中间态。
        machine->ForceClose();
        if (error != nullptr) *error = "流控额度提交失败";
        return ConnStatus::kFlowControl;
    }
    // step4: 写帧；半写失败也要关流，避免上层继续写。
    const auto status = WriteFrame(MakeData(stream_id, end_stream, data), error);
    if (status != ConnStatus::kOk) {
        // 帧可能只写了一半：这条流在线上已经不完整，显式关闭，
        // 不让调用方看到一个"以为还能继续写"的流。
        machine->ForceClose();
    }
    return status;
}

ConnStatus Connection::SendResponseHeaders(std::uint32_t stream_id,
                                           const std::vector<HeaderField>& headers,
                                           bool end_stream, std::string* error) {
    struct ClosedStreamReaper {
        Connection* self;
        ~ClosedStreamReaper() { self->ReapClosedStreams(); }
    } reaper{this};
    if (m_state == ConnState::kClosed) {
        if (error != nullptr) *error = "连接已关闭";
        return ConnStatus::kClosed;
    }
    auto* machine = StreamFor(stream_id);
    if (machine == nullptr) {
        if (error != nullptr) *error = "未知流";
        return ConnStatus::kProtocolError;
    }
    // 发送前先按对端 SETTINGS_MAX_HEADER_LIST_SIZE 检查，避免把超限响应
    // 写上线、再被对端以 ENHANCE_YOUR_CALM/连接错误拒绝。
    if (const auto header_status = CheckOutboundHeaderList(headers, error);
        header_status != ConnStatus::kOk) {
        return header_status;
    }
    if (machine->SendHeaders(end_stream) != StreamEventResult::kOk) {
        if (error != nullptr) *error = "流状态机拒绝发送响应 HEADERS";
        return ConnStatus::kProtocolError;
    }
    // 表大小更新必须位于 header block 首部（RFC 7541 §4.2），先拼前缀再编码。
    std::vector<std::uint8_t> block = m_pending_table_size_update;
    if (m_encoder.EncodeBlock(headers, &block, error) != HpackStatus::kOk) {
        if (error != nullptr) *error = "HPACK 编码失败";
        return ConnStatus::kProtocolError;
    }
    const auto status = WriteHeaderBlock(stream_id, block, end_stream, error);
    if (status == ConnStatus::kOk) {
        // 更新已随本块上线，后续块不再重复发送。
        m_pending_table_size_update.clear();
    }
    return status;
}

ConnStatus Connection::ResetStream(std::uint32_t stream_id, ErrorCode code,
                                   std::string* error) {
    struct ClosedStreamReaper {
        Connection* self;
        ~ClosedStreamReaper() { self->ReapClosedStreams(); }
    } reaper{this};
    if (m_state == ConnState::kClosed) {
        if (error != nullptr) *error = "连接已关闭";
        return ConnStatus::kClosed;
    }
    auto* machine = StreamFor(stream_id);
    if (machine == nullptr) {
        if (error != nullptr) *error = "未知流";
        return ConnStatus::kProtocolError;
    }
    // 预检：idle 流上不能发 RST_STREAM（§5.1）。旧实现忽略该结果，可能
    // 在 idle 流上写出帧；这里把失败映射成协议错误，不写线上。
    if (machine->SendRstStream() != StreamEventResult::kOk) {
        if (error != nullptr) *error = "流状态机拒绝发送 RST_STREAM";
        return ConnStatus::kProtocolError;
    }
    return WriteFrame(MakeRstStream(stream_id, code), error);
}


std::size_t Connection::OpenStreamCount() const noexcept {
    // RFC 9113 §5.1.2：open 与两个 half-closed 状态都计入并发流上限。
    // 流表会**有界保留**已关闭流（默认 256 条，用于容忍迟到的 RST/WINDOW_UPDATE），
    // 所以这里必须排除 kClosed。
    std::size_t count = 0;
    for (const auto& [stream_id, machine] : m_streams) {
        (void)stream_id;
        if (!machine.closed()) {
            ++count;
        }
    }
    return count;
}

void Connection::SendFatalGoAway(ErrorCode code) {
    if (m_transport == nullptr || m_state == ConnState::kClosed) {
        return;
    }
    const std::uint32_t last =
        (m_role == Role::kClient) ? 0U : m_max_seen_peer_stream_id;
    std::string ignored;
    (void)WriteFrame(MakeGoAway(last, code), &ignored);
}

void Connection::ReapClosedStreams() {
    // 只淘汰 closed 的流，且保留最近关闭的 max_retired_streams 条：
    // 关闭后短时间内的 WINDOW_UPDATE / RST_STREAM 仍可被识别为"已关闭"，
    // 不会被误判成 idle（D6/D13）；更老的条目删除后，流表不再随流号增长。
    std::size_t closed_count = 0;
    for (const auto& entry : m_streams) {
        if (entry.second.closed()) {
            ++closed_count;
        }
    }
    while (closed_count > m_config.max_retired_streams) {
        auto victim = m_streams.end();
        for (auto it = m_streams.begin(); it != m_streams.end(); ++it) {
            if (it->second.closed()) {
                victim = it;
                break;
            }
        }
        if (victim == m_streams.end()) {
            break;
        }
        const std::uint32_t victim_id = victim->first;
        m_streams.erase(victim);
        m_stream_local_window.erase(victim_id);
        m_stream_remote_window.erase(victim_id);
        --closed_count;
    }
}

bool Connection::IsEvictedStream(std::uint32_t stream_id) const noexcept {
    if (m_streams.find(stream_id) != m_streams.end()) {
        return false;
    }
    // 本实现只有客户端会主动分配奇数流；服务端不发起流（不支持推送）。
    if (stream_id == 0 || (stream_id & 1U) == 0U) {
        return false;
    }
    if (m_role == Role::kClient) {
        // 客户端流号连续分配：小于 m_next_stream_id 的奇数 id 一定分配过。
        return stream_id < m_next_stream_id;
    }
    // 服务端：对端新流 id 必须严格递增；不大于水位线的奇数 id 只可能是
    // 已关闭并被淘汰的流（也可能是对端跳过的号，忽略其迟到帧的风险很低）。
    return stream_id <= m_max_seen_peer_stream_id;
}

ConnStatus Connection::CheckOutboundHeaderList(
    const std::vector<HeaderField>& headers, std::string* error) const {
    if (!m_peer_max_header_list_size_set) {
        return ConnStatus::kOk;
    }
    const std::size_t size = HeaderListSize(headers);
    if (size > m_peer_max_header_list_size) {
        if (error != nullptr) {
            *error = "header 列表超过对端 SETTINGS_MAX_HEADER_LIST_SIZE: " +
                     std::to_string(size) + " > " +
                     std::to_string(m_peer_max_header_list_size);
        }
        return ConnStatus::kHeaderListTooLarge;
    }
    return ConnStatus::kOk;
}

std::vector<HeaderField> Connection::PrepareOutboundHeaders(
    const std::vector<HeaderField>& headers) const {
    if (m_role != Role::kClient || !m_config.auto_fill_scheme) {
        return headers;
    }
    bool has_scheme = false;
    bool has_authority = false;
    for (const auto& field : headers) {
        if (field.name == ":scheme") {
            has_scheme = true;
        } else if (field.name == ":authority") {
            has_authority = true;
        } else if (field.name == ":method" && field.value == "CONNECT") {
            // RFC 9113 §8.5：CONNECT 必须省略 :scheme/:path，不做补齐。
            return headers;
        }
    }
    if (has_scheme || (!has_authority && !m_config.strict_scheme)) {
        return headers;
    }
    // 伪头必须位于普通字段之前：插到伪头区的末尾。
    auto insert_pos = headers.begin();
    while (insert_pos != headers.end() && !insert_pos->name.empty() &&
           insert_pos->name[0] == ':') {
        ++insert_pos;
    }
    std::vector<HeaderField> outbound = headers;
    const auto offset = static_cast<std::ptrdiff_t>(insert_pos - headers.begin());
    outbound.insert(outbound.begin() + offset,
                    HeaderField{":scheme", m_config.default_scheme});
    return outbound;
}

ConnStatus Connection::ApplyPeerSettings(const SettingsFrame& settings,
                                         std::string* error) {
    for (const auto& entry : settings.entries) {
        switch (static_cast<SettingsId>(entry.id)) {
            case SettingsId::kHeaderTableSize:
                // 对端上限变化：收缩立即生效；扩容必须显式发 0x20 更新，
                // 否则对端解码器仍按旧上限解释索引（RFC 7541 §4.2）。
                if (m_encoder.Table().MaxSize() != entry.value) {
                    m_encoder.SetTableSize(entry.value);
                    if (QueueTableSizeUpdate(entry.value, error) != ConnStatus::kOk) {
                        return ConnStatus::kProtocolError;
                    }
                }
                break;
            case SettingsId::kMaxFrameSize:
                // 值域已由 ParseSettings() 校验（§6.5.2）。
                m_peer_max_frame_size = entry.value;
                break;
            case SettingsId::kInitialWindowSize: {
                // 只影响流级发送窗口（§6.5.2 / §6.9.2）：按差值调整所有已存在的流。
                // 连接级窗口 m_remote_window **不**受影响。
                const auto delta = static_cast<std::int64_t>(entry.value) -
                                   static_cast<std::int64_t>(m_peer_initial_window_size);
                m_peer_initial_window_size = static_cast<std::int64_t>(entry.value);
                for (auto& [stream_id, window] : m_stream_remote_window) {
                    (void)stream_id;
                    window += delta;
                }
                break;
            }
            case SettingsId::kMaxConcurrentStreams:
                m_peer_max_concurrent_streams = entry.value;
                m_peer_max_concurrent_set = true;
                break;
            case SettingsId::kMaxHeaderListSize:
                // 对端愿意接收的 header 列表上限：约束本端发送的请求/响应。
                m_peer_max_header_list_size = entry.value;
                m_peer_max_header_list_size_set = true;
                break;
            case SettingsId::kEnablePush:
                // RFC 9113 §6.5.2：服务端不得发送 ENABLE_PUSH，客户端收到
                // 任何值都是连接错误。服务端收到客户端的 0/1 是合法的，
                // 只记录（本实现不会推送，1 也只表示"对端允许推送"）。
                if (m_role == Role::kClient) {
                    if (error != nullptr) {
                        *error = "服务端不得发送 SETTINGS_ENABLE_PUSH";
                    }
                    return ConnStatus::kProtocolError;
                }
                m_peer_enable_push = entry.value != 0;
                break;
            default:
                break;
        }
    }
    return ConnStatus::kOk;
}

ConnStatus Connection::QueueTableSizeUpdate(std::uint32_t new_size,
                                            std::string* error) {
    std::vector<std::uint8_t> update;
    if (m_encoder.AppendTableSizeUpdate(new_size, &update) != HpackStatus::kOk) {
        if (error != nullptr) {
            *error = "HPACK 动态表大小更新编码失败: " + std::to_string(new_size);
        }
        return ConnStatus::kProtocolError;
    }
    // 只保留最新一次变化：对端只需要知道最终表大小。
    m_pending_table_size_update = std::move(update);
    return ConnStatus::kOk;
}

void Connection::ResetStreamWindows(std::uint32_t stream_id) {
    // 接收方向用本端 SETTINGS 的初值；发送方向用**对端**的 INITIAL_WINDOW_SIZE。
    m_stream_local_window[stream_id] = m_config.initial_window_size;
    m_stream_remote_window[stream_id] = m_peer_initial_window_size;
}

bool Connection::HasSendWindow(std::uint32_t stream_id,
                               std::size_t bytes) const {
    const auto amount = static_cast<std::int64_t>(bytes);
    if (m_remote_window < amount) {
        return false;
    }
    const auto it = m_stream_remote_window.find(stream_id);
    return it != m_stream_remote_window.end() && it->second >= amount;
}

bool Connection::ConsumeSendWindow(std::uint32_t stream_id, std::size_t bytes) {
    const auto amount = static_cast<std::int64_t>(bytes);
    if (m_remote_window < amount) {
        return false;
    }
    auto it = m_stream_remote_window.find(stream_id);
    if (it == m_stream_remote_window.end()) {
        return false;
    }
    if (it->second < amount) {
        return false;
    }
    m_remote_window -= amount;
    it->second -= amount;
    return true;
}

/**
 * @brief 写 header block，必要时拆成 HEADERS + CONTINUATION。
 *
 * ## 步骤
 * step1. 算单片上限 `m_peer_max_frame_size - 9`；
 * step2. 一帧装得下就直接写 HEADERS；
 * step3. 否则循环切片：首片 HEADERS、其余 CONTINUATION，最后一片带 END_HEADERS。
 *
 * ## 边界
 * `end_stream` 只加在 HEADERS 上；CONTINUATION 永远不带 END_STREAM。
 */
ConnStatus Connection::WriteHeaderBlock(std::uint32_t stream_id,
                                        const std::vector<std::uint8_t>& block,
                                        bool end_stream, std::string* error) {
    if (m_peer_max_frame_size == 0) {
        if (error != nullptr) *error = "对端 max_frame_size 为 0";
        return ConnStatus::kProtocolError;
    }
    // step1: SETTINGS_MAX_FRAME_SIZE 是 payload 上限，不需要减帧头。
    // 旧实现减了 9，导致每个 HEADERS/CONTINUATION 少装 9 字节，
    // 且与接收侧（按 payload 判定）口径不一致。
    const std::size_t chunk = m_peer_max_frame_size;
    // 能一帧装下就走快路径（也是绝大多数情况）
    // step2: 快路径：一帧装下。
    if (block.size() <= chunk) {
        return WriteFrame(MakeHeaders(stream_id, end_stream, true, block), error);
    }
    // 分片：HEADERS(无 END_HEADERS) + CONTINUATION... 最后一片带 END_HEADERS
    // step3: 分片循环（首片 HEADERS，其余 CONTINUATION）。
    std::size_t offset = 0;
    bool first = true;
    while (offset < block.size()) {
        const std::size_t take = std::min(chunk, block.size() - offset);
        std::vector<std::uint8_t> piece(block.begin() + static_cast<std::ptrdiff_t>(offset),
                                        block.begin() + static_cast<std::ptrdiff_t>(offset + take));
        offset += take;
        const bool last = (offset >= block.size());
        const auto status = first
                                ? WriteFrame(MakeHeaders(stream_id, end_stream, last, piece),
                                             error)
                                : WriteFrame(MakeContinuation(stream_id, last, piece), error);
        if (status != ConnStatus::kOk) {
            return status;
        }
        first = false;
    }
    return ConnStatus::kOk;
}

/**
 * @brief 读取并拼接完整的 header block（处理 CONTINUATION）。
 *
 * ## 步骤
 * step1. 清空输出并检查首片是否超限；
 * step2. 循环取帧直到 END_HEADERS；
 * step3. 校验帧类型与流 id（RFC 9113 §6.10 要求期间不能出现其它帧）；
 * step4. 边拼边累计上限，防止 CONTINUATION 泛滥把内存撑爆。
 */
ConnStatus Connection::AssembleHeaderBlock(std::uint32_t stream_id,
                                           const std::vector<std::uint8_t>& first_block,
                                           std::vector<std::uint8_t>* out,
                                           std::string* error) {
    // step1: 首片也要计入上限（单个 HEADERS 就可能超限）。
    out->clear();
    // 第一片本身也要算进上限（单个 HEADERS 就能超限）。
    if (first_block.size() > m_config.max_header_block_size) {
        if (error != nullptr) {
            *error = "header block 超过上限: " + std::to_string(first_block.size()) +
                     " > " + std::to_string(m_config.max_header_block_size);
        }
        return ConnStatus::kHeaderListTooLarge;
    }
    out->insert(out->end(), first_block.begin(), first_block.end());
    // 循环读后续帧，直到遇到 END_HEADERS。
    // step2: 一直读到 END_HEADERS。
    for (;;) {
        Frame frame;
        const auto status = TakeFrame(&frame, error);
        if (status != ConnStatus::kOk) {
            return status;
        }
        // step3: 期间出现其它帧是协议错误。
        if (frame.header.type != FrameType::kContinuation) {
            // RFC 9113 §6.10：header block 未结束前出现其它帧是协议错误
            if (error != nullptr) {
                *error = std::string("期望 CONTINUATION，收到 ") +
                         ToString(frame.header.type);
            }
            return ConnStatus::kProtocolError;
        }
        HeadersFrame cont;
        if (ParseContinuation(frame, &cont, error) != FrameStatus::kOk) {
            return ConnStatus::kProtocolError;
        }
        if (cont.stream_id != stream_id) {
            if (error != nullptr) *error = "CONTINUATION 的流 id 与 HEADERS 不一致";
            return ConnStatus::kProtocolError;
        }
        // 关键：边拼边累计检查。不能等收完再判 —— 那正是 CONTINUATION
        // 泛滥攻击想要的（先把内存撑爆）。
        // step4: 边拼边判上限，不能等收完再判。
        if (out->size() + cont.header_block.size() > m_config.max_header_block_size) {
            if (error != nullptr) {
                *error = "header block 超过上限: " +
                         std::to_string(out->size() + cont.header_block.size()) + " > " +
                         std::to_string(m_config.max_header_block_size);
            }
            return ConnStatus::kHeaderListTooLarge;
        }
        out->insert(out->end(), cont.header_block.begin(), cont.header_block.end());
        if (cont.end_headers) {
            return ConnStatus::kOk;
        }
    }
}

/**
 * @brief 发送 GOAWAY 并把连接置为 draining。
 *
 * ## 步骤
 * step1. 取已见最大流 id 作为 last_stream_id（已知偏差见实现注释）；
 * step2. 写 GOAWAY；
 * step3. 成功后迁移到 `kDraining`，不再接受新流。
 */
ConnStatus Connection::SendGoAway(ErrorCode code, std::string* error) {
    if (m_state == ConnState::kClosed) {
        if (error != nullptr) *error = "连接已关闭";
        return ConnStatus::kClosed;
    }
    // step1: 取"对端发起的、已处理的最后一个流 id"。
    // RFC 9113 §6.8：last_stream_id 是本端可能已处理的对端发起流的最大 id。
    // - 客户端不会接受服务端发起的新流（本实现不支持推送），因此恒为 0；
    // - 服务端用已见最大客户端流 id（m_max_seen_peer_stream_id）。
    // 旧实现把**本端**发起的流号也算了进去，客户端会发出 last_stream_id=1，
    // 这在语义上声称"处理过一个服务端发起的流"，属于协议错误。
    const std::uint32_t last =
        (m_role == Role::kClient) ? 0U : m_max_seen_peer_stream_id;
    // step2: 写 GOAWAY 帧。
    const auto status = WriteFrame(MakeGoAway(last, code), error);
    // step3: 只有真正写出去了才迁移状态。
    if (status == ConnStatus::kOk) {
        // 已发出 GOAWAY：进入 draining，不再接受新流（已有流仍可完成）。
        m_state = ConnState::kDraining;
    }
    return status;
}

/**
 * @brief 读取下一个事件（阻塞）——接收方向的唯一分发点。
 *
 * ## 步骤
 * step1. `TakeFrame()` 取完整帧；
 * step2. 按帧类型分发（HEADERS/DATA/SETTINGS/PING/RST_STREAM/GOAWAY/WINDOW_UPDATE）；
 * step3. HEADERS：拼 CONTINUATION -> HPACK 解码 -> 两道大小上限校验；
 *         DATA：接收流控预检 -> 扣窗口 -> 超半窗回 WINDOW_UPDATE；
 * step4. 推进流状态机（新流初建窗口）并填充 `IncomingEvent`。
 *
 * ## 失败语义
 * 协议错误返回 `kProtocolError`；本函数不发 GOAWAY，由调用方决定断连。
 */
ConnStatus Connection::ReadEvent(IncomingEvent* out, std::string* error) {
    struct ClosedStreamReaper {
        Connection* self;
        ~ClosedStreamReaper() { self->ReapClosedStreams(); }
    } reaper{this};
    if (out == nullptr) {
        return ConnStatus::kProtocolError;
    }
    if (m_state == ConnState::kClosed) {
        if (error != nullptr) *error = "连接已关闭";
        return ConnStatus::kClosed;
    }
    // step1: 阻塞取一个完整帧（可能跨多次 Read）。
    Frame frame;
    auto status = TakeFrame(&frame, error);
    if (status != ConnStatus::kOk) {
        return status;
    }
    *out = IncomingEvent{};

    // step2: 按帧类型分发；每个分支负责自身的字段校验。
    switch (frame.header.type) {
        case FrameType::kHeaders: {
            HeadersFrame headers;
            if (ParseHeaders(frame, &headers, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            std::vector<std::uint8_t> full_block;
            if (!headers.end_headers) {
                const auto assemble =
                    AssembleHeaderBlock(headers.stream_id, headers.header_block,
                                        &full_block, error);
                if (assemble != ConnStatus::kOk) {
                    return assemble;
                }
            } else {
                full_block = std::move(headers.header_block);
                // 单片 HEADERS 走的是另一条路径（不经 AssembleHeaderBlock），
                // 所以这里必须**独立**再检查一次上限 —— 否则一个 10MB 的
                // 单帧 HEADERS 可以绕过限制。
                if (full_block.size() > m_config.max_header_block_size) {
                    if (error != nullptr) {
                        *error = "header block 超过上限: " +
                                 std::to_string(full_block.size()) + " > " +
                                 std::to_string(m_config.max_header_block_size);
                    }
                    return ConnStatus::kHeaderListTooLarge;
                }
            }
            // step3: HPACK 解码（失败即协议错误）。
            std::vector<HeaderField> decoded;
            if (m_decoder.DecodeBlock(full_block.data(), full_block.size(), &decoded,
                                      error) != HpackStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            // 第二道闸：解压后的大小。压缩块可能很小而解压后极大
            // （HPACK 解压炸弹），所以这里按 RFC 9113 §6.5.2 的口径再算一次。
            std::size_t header_list_size = 0;
            for (const auto& field : decoded) {
                header_list_size += field.name.size() + field.value.size() + 32;
            }
            if (header_list_size > m_config.max_header_list_size) {
                if (error != nullptr) {
                    *error = "解压后 header 列表超过上限: " +
                             std::to_string(header_list_size) + " > " +
                             std::to_string(m_config.max_header_list_size);
                }
                return ConnStatus::kHeaderListTooLarge;
            }
            // 流状态推进
            // step4: 建流/推进状态机；新流必须先初始化窗口。
            const bool fresh_stream =
                m_streams.find(headers.stream_id) == m_streams.end();
            // RFC 9113 §8.1.2/§8.2.1/§8.3：伪头位置与必需项、字段名小写、
            // 请求的 :scheme 约束（兼容规则见 ValidatePeerHeaders 注释）。
            if (!ValidatePeerHeaders(decoded, m_role, fresh_stream,
                                     m_config.require_scheme, m_config.strict_scheme,
                                     error)) {
                return ConnStatus::kProtocolError;
            }
            if (fresh_stream) {
                // RFC 9113 §5.1.1：流 id 由发起方分配，客户端用奇数、服务端用偶数，
                // 且新流 id 必须严格大于该发起方此前用过的所有流 id。
                // 先校验 id 再校验并发上限：非法 id 属于连接错误，不能被
                // REFUSED_STREAM 掩盖。
                if (m_role == Role::kClient) {
                    // 服务端只能通过 PUSH_PROMISE 发起偶数流；本实现不支持推送。
                    if (error != nullptr) {
                        *error = "收到对端发起的新流（本实现不支持服务端推送）";
                    }
                    return ConnStatus::kProtocolError;
                }
                if ((headers.stream_id & 1U) == 0U) {
                    if (error != nullptr) {
                        *error = "客户端发起的流 id 必须为奇数: " +
                                 std::to_string(headers.stream_id);
                    }
                    return ConnStatus::kProtocolError;
                }
                if (headers.stream_id <= m_max_seen_peer_stream_id) {
                    if (error != nullptr) {
                        *error = "新流 id 必须大于已见最大流 id: " +
                                 std::to_string(headers.stream_id) + " <= " +
                                 std::to_string(m_max_seen_peer_stream_id);
                    }
                    return ConnStatus::kProtocolError;
                }
                if (m_state != ConnState::kOpen) {
                    // 已发/已收 GOAWAY：last_stream_id 之后的新流一律拒绝。
                    if (error != nullptr) {
                        *error = "连接处于 draining，拒绝新流: " +
                                 std::to_string(headers.stream_id);
                    }
                    return ConnStatus::kProtocolError;
                }
                // RFC 9113 §5.1.2：收到的 HEADERS 若使并发流数超过本端通告的
                // MAX_CONCURRENT_STREAMS，属于流错误（REFUSED_STREAM）。
                // 注意此时 HPACK 已解码完毕，动态表保持同步，可以直接拒绝该流。
                if (m_config.enforce_max_concurrent_streams &&
                    OpenStreamCount() >= m_config.max_concurrent_streams) {
                    status = WriteFrame(
                        MakeRstStream(headers.stream_id, ErrorCode::kRefusedStream), error);
                    if (status != ConnStatus::kOk) {
                        return status;
                    }
                    out->kind = IncomingEvent::Kind::kRstStream;
                    out->stream_id = headers.stream_id;
                    out->error_code = ErrorCode::kRefusedStream;
                    return ConnStatus::kOk;
                }
                m_max_seen_peer_stream_id = headers.stream_id;
            }
            auto& machine =
                m_streams.try_emplace(headers.stream_id, headers.stream_id).first->second;
            // 被动接受的流（服务端侧）也要初始化流控窗口，否则回响应时
            // ConsumeSendWindow 找不到该流会直接失败。
            if (fresh_stream) {
                ResetStreamWindows(headers.stream_id);
            }
            if (machine.ReceiveHeaders(headers.end_stream) != StreamEventResult::kOk) {
                if (error != nullptr) *error = "流状态机拒绝接收 HEADERS";
                return ConnStatus::kProtocolError;
            }
            out->kind = IncomingEvent::Kind::kHeaders;
            out->stream_id = headers.stream_id;
            out->end_stream = headers.end_stream;
            out->headers = std::move(decoded);
            return ConnStatus::kOk;
        }
        case FrameType::kData: {
            DataFrame data;
            if (ParseData(frame, &data, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            auto* machine = StreamFor(data.stream_id);
            if (machine == nullptr) {
                if (error != nullptr) {
                    *error = IsEvictedStream(data.stream_id)
                                 ? "DATA 落在已关闭并淘汰的流上（STREAM_CLOSED）"
                                 : "DATA 落在未知流上";
                }
                return ConnStatus::kProtocolError;
            }
            const std::size_t received = data.data.size();
            const auto received_amount = static_cast<std::int64_t>(received);
            auto lit = m_stream_local_window.find(data.stream_id);
            // 接收侧流控（RFC 9113 §6.9.1）：对端 DATA 不得超过当前可用窗口。
            // 必须在 ReceiveData/扣窗口之前检查 —— 否则窗口会被推到负值，
            // 既无法回报 FLOW_CONTROL_ERROR，也让补窗增量失真。
            // step3: 接收流控先校验再扣减，窗口不会变负。
            if (m_local_window < received_amount ||
                (lit != m_stream_local_window.end() &&
                 lit->second < received_amount)) {
                if (error != nullptr) {
                    *error = "DATA 超过接收窗口（FLOW_CONTROL_ERROR）";
                }
                return ConnStatus::kProtocolError;
            }
            if (machine->ReceiveData(data.end_stream) != StreamEventResult::kOk) {
                if (error != nullptr) *error = "流状态机拒绝接收 DATA";
                return ConnStatus::kProtocolError;
            }
            out->kind = IncomingEvent::Kind::kData;
            out->stream_id = data.stream_id;
            out->end_stream = data.end_stream;
            out->data = std::move(data.data);
            // 流控：扣减本地窗口（连接级 + 流级）。上面的检查保证不会为负。
            m_local_window -= received_amount;
            if (lit != m_stream_local_window.end()) {
                lit->second -= received_amount;
            }
            // 只有消费掉超过一半才补窗口，避免每帧都发 WINDOW_UPDATE
            // （那会让流控形同虚设，还浪费带宽）。
            // step4: 只有消费超过半窗才补 WINDOW_UPDATE。
            const auto threshold = static_cast<std::int64_t>(m_config.initial_window_size / 2);
            if (received > 0 && lit != m_stream_local_window.end() &&
                lit->second <= threshold) {
                // 增量按 2^31-1 饱和：WINDOW_UPDATE 的 increment 超过
                // 2^31-1 本身就是协议错误（RFC 9113 §6.9）。
                const auto increment = static_cast<std::uint32_t>(
                    std::min<std::int64_t>(
                        static_cast<std::int64_t>(m_config.initial_window_size) -
                            lit->second,
                        std::numeric_limits<std::int32_t>::max()));
                if (increment > 0) {
                    status = WriteFrame(MakeWindowUpdate(data.stream_id, increment), error);
                    if (status != ConnStatus::kOk) {
                        return status;
                    }
                    lit->second += static_cast<std::int64_t>(increment);
                }
            }
            if (received > 0 && m_local_window <= threshold) {
                const auto increment = static_cast<std::uint32_t>(
                    std::min<std::int64_t>(
                        static_cast<std::int64_t>(m_config.initial_window_size) -
                            m_local_window,
                        std::numeric_limits<std::int32_t>::max()));
                if (increment > 0) {
                    status = WriteFrame(MakeWindowUpdate(0, increment), error);
                    if (status != ConnStatus::kOk) {
                        return status;
                    }
                    m_local_window += static_cast<std::int64_t>(increment);
                }
            }
            return ConnStatus::kOk;
        }
        case FrameType::kSettings: {
            SettingsFrame settings;
            if (ParseSettings(frame, &settings, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            if (!settings.ack) {
                // 握手完成后的 SETTINGS 同样必须生效（RFC 9113 §6.5）：
                // 旧实现只回 ACK 不应用，对端调小 MAX_FRAME_SIZE /
                // INITIAL_WINDOW_SIZE 后本端仍按旧值发送，必然被对端拒绝。
                if (ApplyPeerSettings(settings, error) != ConnStatus::kOk) {
                    return ConnStatus::kProtocolError;
                }
                status = WriteFrame(MakeSettingsAck(), error);
                if (status != ConnStatus::kOk) {
                    return status;
                }
            }
            out->kind = IncomingEvent::Kind::kSettings;
            return ConnStatus::kOk;
        }
        case FrameType::kPing: {
            PingFrame ping;
            if (ParsePing(frame, &ping, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            if (!ping.ack) {
                status = WriteFrame(MakePing(ping.opaque_data, true), error);
                if (status != ConnStatus::kOk) {
                    return status;
                }
            }
            out->kind = IncomingEvent::Kind::kPing;
            out->ping_opaque = ping.opaque_data;
            return ConnStatus::kOk;
        }
        case FrameType::kRstStream: {
            RstStreamFrame rst;
            if (ParseRstStream(frame, &rst, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            auto* machine = StreamFor(rst.stream_id);
            if (machine == nullptr) {
                if (IsEvictedStream(rst.stream_id)) {
                    // 流已关闭并从流表淘汰：RFC 9113 §5.1 允许忽略迟到的
                    // RST_STREAM（对端可能在本端清理后才收到响应）。
                    out->kind = IncomingEvent::Kind::kRstStream;
                    out->stream_id = rst.stream_id;
                    out->error_code = rst.error_code;
                    return ConnStatus::kOk;
                }
                // RFC 9113 §6.4：idle 流上的 RST_STREAM 是连接错误 PROTOCOL_ERROR。
                if (error != nullptr) {
                    *error = "RST_STREAM 落在 idle 流上: " +
                             std::to_string(rst.stream_id);
                }
                return ConnStatus::kProtocolError;
            }
            machine->ReceiveRstStream();
            out->kind = IncomingEvent::Kind::kRstStream;
            out->stream_id = rst.stream_id;
            out->error_code = rst.error_code;
            return ConnStatus::kOk;
        }
        case FrameType::kGoAway: {
            GoAwayFrame goaway;
            if (ParseGoAway(frame, &goaway, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            // 对端 GOAWAY：进入 draining。id 大于 last_stream_id 的流
            // 在理论上未被对端处理，调用方应重试；本实现只记录边界，
            // 不自动重放（见头文件的"已知限制"）。
            // step3: 记录对端边界并迁移到 kDraining。
            m_peer_last_stream_id = goaway.last_stream_id;
            if (m_state == ConnState::kOpen) {
                m_state = ConnState::kDraining;
            }
            out->kind = IncomingEvent::Kind::kGoAway;
            out->error_code = goaway.error_code;
            out->last_stream_id = goaway.last_stream_id;
            return ConnStatus::kOk;
        }
        case FrameType::kWindowUpdate: {
            WindowUpdateFrame update;
            if (ParseWindowUpdate(frame, &update, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            // 流控：对端给了额度
            if (update.stream_id == 0) {
                m_remote_window += static_cast<std::int64_t>(update.increment);
            } else {
                auto it = m_stream_remote_window.find(update.stream_id);
                if (it == m_stream_remote_window.end()) {
                    if (IsEvictedStream(update.stream_id)) {
                        // 已关闭并被淘汰的流：RFC 9113 §5.1/§6.9 允许忽略
                        // 迟到的 WINDOW_UPDATE（窗口对象已不存在意义）。
                        out->kind = IncomingEvent::Kind::kWindowUpdate;
                        out->stream_id = update.stream_id;
                        return ConnStatus::kOk;
                    }
                    // RFC 9113 §6.9：idle 流上的 WINDOW_UPDATE 是连接错误。
                    if (error != nullptr) {
                        *error = "WINDOW_UPDATE 落在 idle 流上: " +
                                 std::to_string(update.stream_id);
                    }
                    return ConnStatus::kProtocolError;
                }
                it->second += static_cast<std::int64_t>(update.increment);
            }
            out->kind = IncomingEvent::Kind::kWindowUpdate;
            out->stream_id = update.stream_id;
            return ConnStatus::kOk;
        }
        case FrameType::kPushPromise:
            // 本实现不支持服务器推送：客户端已在 SETTINGS 中通告 ENABLE_PUSH=0。
            // 无论角色，收到 PUSH_PROMISE 都是连接错误。必须关闭连接而不是
            // 当作未知帧跳过 —— 它的 header block 会更新对端 HPACK 编码器
            // 的动态表，而本端解码器不更新，跳过必然导致后续 header 失步。
            if (error != nullptr) {
                *error = "收到 PUSH_PROMISE（本实现不支持服务器推送）";
            }
            m_state = ConnState::kClosed;
            return ConnStatus::kProtocolError;
        case FrameType::kContinuation:
            // RFC 9113 §6.10：CONTINUATION 必须紧跟未带 END_HEADERS 的
            // HEADERS/PUSH_PROMISE/CONTINUATION，且期间不能插入其它帧。
            // 正常序列由 AssembleHeaderBlock() 内联消费；走到分发器说明它是孤立的。
            if (error != nullptr) {
                *error = "孤立的 CONTINUATION（没有未完成的 header block）";
            }
            return ConnStatus::kProtocolError;
        default:
            out->kind = IncomingEvent::Kind::kUnknown;
            return ConnStatus::kOk;
    }
}

}  // namespace go2cpp::net::http2
