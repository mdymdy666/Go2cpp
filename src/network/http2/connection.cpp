/**
 * @file connection.cpp
 * @brief HTTP/2 连接层实现。
 */
#include "go2cpp/network/http2/connection.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace go2cpp::net::http2 {

namespace {

/// 客户端连接前言（RFC 9113 §3.4）。
constexpr char kClientPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kClientPrefaceLength = sizeof(kClientPreface) - 1;

}  // namespace

const char* ToString(ConnStatus status) noexcept {
    switch (status) {
        case ConnStatus::kOk: return "ok";
        case ConnStatus::kClosed: return "closed";
        case ConnStatus::kProtocolError: return "protocol-error";
        case ConnStatus::kUnsupported: return "unsupported";
        case ConnStatus::kFlowControl: return "flow-control";
        case ConnStatus::kHeaderListTooLarge: return "header-list-too-large";
    }
    return "unknown";
}

Connection::Connection(Transport* transport, Role role, ConnectionConfig config)
    : m_transport(transport),
      m_role(role),
      m_config(config),
      m_peer_max_frame_size(config.max_frame_size),
      m_local_window(config.initial_window_size),
      m_remote_window(config.initial_window_size),
      m_decoder(config.header_table_size),
      m_encoder(config.header_table_size) {}

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
        return status == FrameStatus::kFrameSizeError ? ConnStatus::kProtocolError
                                                      : ConnStatus::kProtocolError;
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
    const std::vector<SettingsEntry> local_settings = {
        {static_cast<std::uint16_t>(SettingsId::kHeaderTableSize), m_config.header_table_size},
        {static_cast<std::uint16_t>(SettingsId::kMaxFrameSize), m_config.max_frame_size},
        {static_cast<std::uint16_t>(SettingsId::kInitialWindowSize),
         m_config.initial_window_size},
        {static_cast<std::uint16_t>(SettingsId::kMaxConcurrentStreams),
         m_config.max_concurrent_streams},
    };
    auto status = WriteFrame(MakeSettings(local_settings), error);
    if (status != ConnStatus::kOk) {
        return status;
    }
    m_local_settings_sent = true;

    // 3) 等对端 SETTINGS 并发 ACK
    return FinishSettingsHandshake(error);
}

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
        if (frame.header.type == FrameType::kSettings) {
            SettingsFrame settings;
            if (ParseSettings(frame, &settings, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            if (settings.ack) {
                // 对端确认了我们的 SETTINGS
                continue;
            }
            // 应用对端 SETTINGS 中的关键值
            for (const auto& entry : settings.entries) {
                switch (static_cast<SettingsId>(entry.id)) {
                    case SettingsId::kHeaderTableSize:
                        m_encoder.SetTableSize(entry.value);
                        break;
                    case SettingsId::kMaxFrameSize:
                        if (entry.value >= kMinMaxFrameSize && entry.value <= kMaxMaxFrameSize) {
                            m_peer_max_frame_size = entry.value;
                        }
                        break;
                    case SettingsId::kInitialWindowSize:
                        // 对端声明的初始流窗口：重置所有已存在流的发送窗口
                        m_remote_window = entry.value;
                        for (auto& [sid, win] : m_stream_remote_window) {
                            (void)sid;
                            win = entry.value;
                        }
                        break;
                    default:
                        break;
                }
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
    const std::uint32_t stream_id = m_next_stream_id;
    m_next_stream_id += 2;   // 客户端流 id 为奇数且递增

    std::vector<std::uint8_t> block;
    if (m_encoder.EncodeBlock(headers, &block, error) != HpackStatus::kOk) {
        if (error != nullptr) *error = "HPACK 编码失败";
        return ConnStatus::kProtocolError;
    }
    // 用 try_emplace 而不是 operator[]：StreamStateMachine 没有默认构造，
    // 而且流 id 总是确定的，无需"先默认构造再赋值"。
    auto& machine = m_streams.try_emplace(stream_id, stream_id).first->second;
    if (machine.SendHeaders(end_stream) != StreamEventResult::kOk) {
        if (error != nullptr) *error = "流状态机拒绝发送 HEADERS";
        return ConnStatus::kProtocolError;
    }
    ResetStreamWindows(stream_id);
    // 超过一帧时自动拆成 HEADERS + CONTINUATION
    const auto status = WriteHeaderBlock(stream_id, block, end_stream, error);
    if (status != ConnStatus::kOk) {
        return status;
    }
    if (out_stream_id != nullptr) {
        *out_stream_id = stream_id;
    }
    return ConnStatus::kOk;
}

ConnStatus Connection::WriteData(std::uint32_t stream_id,
                                 const std::vector<std::uint8_t>& data,
                                 bool end_stream,
                                 std::string* error) {
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
    if (!machine->CanSendData()) {
        if (error != nullptr) *error = "流状态机拒绝发送 DATA";
        return ConnStatus::kProtocolError;
    }
    if (data.size() + kFrameHeaderSize > m_peer_max_frame_size) {
        if (error != nullptr) *error = "DATA 超过帧上限（未实现分片）";
        return ConnStatus::kUnsupported;
    }
    // 流控：窗口不足时拒绝（不阻塞等待，调用方等 WINDOW_UPDATE 后重试）
    if (!HasSendWindow(stream_id, data.size())) {
        if (error != nullptr) *error = "流控窗口不足";
        return ConnStatus::kFlowControl;
    }
    // 第二步才提交：状态机迁移 + 扣额度 + 写帧。
    if (machine->SendData(end_stream) != StreamEventResult::kOk) {
        if (error != nullptr) *error = "流状态机拒绝发送 DATA";
        return ConnStatus::kProtocolError;
    }
    if (!ConsumeSendWindow(stream_id, data.size())) {
        // 单驱动者下不应发生（预检与提交之间没有其他写者）。一旦发生，
        // 显式关闭该流，绝不留"状态已变、帧没发"的中间态。
        machine->ForceClose();
        if (error != nullptr) *error = "流控额度提交失败";
        return ConnStatus::kFlowControl;
    }
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
    if (m_state == ConnState::kClosed) {
        if (error != nullptr) *error = "连接已关闭";
        return ConnStatus::kClosed;
    }
    auto* machine = StreamFor(stream_id);
    if (machine == nullptr) {
        if (error != nullptr) *error = "未知流";
        return ConnStatus::kProtocolError;
    }
    if (machine->SendHeaders(end_stream) != StreamEventResult::kOk) {
        if (error != nullptr) *error = "流状态机拒绝发送响应 HEADERS";
        return ConnStatus::kProtocolError;
    }
    std::vector<std::uint8_t> block;
    if (m_encoder.EncodeBlock(headers, &block, error) != HpackStatus::kOk) {
        if (error != nullptr) *error = "HPACK 编码失败";
        return ConnStatus::kProtocolError;
    }
    return WriteHeaderBlock(stream_id, block, end_stream, error);
}

ConnStatus Connection::ResetStream(std::uint32_t stream_id, ErrorCode code,
                                   std::string* error) {
    if (m_state == ConnState::kClosed) {
        if (error != nullptr) *error = "连接已关闭";
        return ConnStatus::kClosed;
    }
    auto* machine = StreamFor(stream_id);
    if (machine == nullptr) {
        if (error != nullptr) *error = "未知流";
        return ConnStatus::kProtocolError;
    }
    machine->SendRstStream();
    return WriteFrame(MakeRstStream(stream_id, code), error);
}


void Connection::ResetStreamWindows(std::uint32_t stream_id) {
    m_stream_local_window[stream_id] = m_config.initial_window_size;
    m_stream_remote_window[stream_id] = m_config.initial_window_size;
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

ConnStatus Connection::WriteHeaderBlock(std::uint32_t stream_id,
                                        const std::vector<std::uint8_t>& block,
                                        bool end_stream, std::string* error) {
    if (m_peer_max_frame_size <= kFrameHeaderSize) {
        if (error != nullptr) *error = "对端 max_frame_size 过小";
        return ConnStatus::kProtocolError;
    }
    const std::size_t chunk = m_peer_max_frame_size - kFrameHeaderSize;
    // 能一帧装下就走快路径（也是绝大多数情况）
    if (block.size() <= chunk) {
        return WriteFrame(MakeHeaders(stream_id, end_stream, true, block), error);
    }
    // 分片：HEADERS(无 END_HEADERS) + CONTINUATION... 最后一片带 END_HEADERS
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

ConnStatus Connection::AssembleHeaderBlock(std::uint32_t stream_id,
                                           const std::vector<std::uint8_t>& first_block,
                                           std::vector<std::uint8_t>* out,
                                           std::string* error) {
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
    for (;;) {
        Frame frame;
        const auto status = TakeFrame(&frame, error);
        if (status != ConnStatus::kOk) {
            return status;
        }
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

ConnStatus Connection::SendGoAway(ErrorCode code, std::string* error) {
    if (m_state == ConnState::kClosed) {
        if (error != nullptr) *error = "连接已关闭";
        return ConnStatus::kClosed;
    }
    std::uint32_t last = 0;
    // 最后一个已处理的流 id。
    // 已知限制（R7）：本实现没有服务端推送 API，m_streams 里只有本端
    // 发起的流，所以 client 角色下这个值与 RFC 语义（对端发起的流）有偏差。
    for (const auto& [id, machine] : m_streams) {
        (void)machine;
        if (id > last) {
            last = id;
        }
    }
    const auto status = WriteFrame(MakeGoAway(last, code), error);
    if (status == ConnStatus::kOk) {
        // 已发出 GOAWAY：进入 draining，不再接受新流（已有流仍可完成）。
        m_state = ConnState::kDraining;
    }
    return status;
}

ConnStatus Connection::ReadEvent(IncomingEvent* out, std::string* error) {
    if (out == nullptr) {
        return ConnStatus::kProtocolError;
    }
    if (m_state == ConnState::kClosed) {
        if (error != nullptr) *error = "连接已关闭";
        return ConnStatus::kClosed;
    }
    Frame frame;
    auto status = TakeFrame(&frame, error);
    if (status != ConnStatus::kOk) {
        return status;
    }
    *out = IncomingEvent{};

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
            const bool fresh_stream =
                m_streams.find(headers.stream_id) == m_streams.end();
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
                if (error != nullptr) *error = "DATA 落在未知流上";
                return ConnStatus::kProtocolError;
            }
            const std::size_t received = data.data.size();
            const auto received_amount = static_cast<std::int64_t>(received);
            auto lit = m_stream_local_window.find(data.stream_id);
            // 接收侧流控（RFC 9113 §6.9.1）：对端 DATA 不得超过当前可用窗口。
            // 必须在 ReceiveData/扣窗口之前检查 —— 否则窗口会被推到负值，
            // 既无法回报 FLOW_CONTROL_ERROR，也让补窗增量失真。
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
            if (auto* machine = StreamFor(rst.stream_id)) {
                machine->ReceiveRstStream();
            }
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
                if (it != m_stream_remote_window.end()) {
                    it->second += static_cast<std::int64_t>(update.increment);
                }
            }
            out->kind = IncomingEvent::Kind::kWindowUpdate;
            out->stream_id = update.stream_id;
            return ConnStatus::kOk;
        }
        default:
            out->kind = IncomingEvent::Kind::kUnknown;
            return ConnStatus::kOk;
    }
}

}  // namespace go2cpp::net::http2
