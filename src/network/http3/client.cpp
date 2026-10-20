/**
 * @file client.cpp
 * @brief `Client` 的实现 —— 唯一 include `picohttp`（h3zero/QPACK）头的地方。
 *
 * ## 分层
 *
 * ```
 *   Client (本文件)              HTTP/3：HEADERS/DATA 帧、:status、body
 *      |  picoquic_set_callback（换成 HTTP/3 回调）
 *   PicoquicConnection           QUIC：握手、ACK、重传、流控
 *      |  picoquic_prepare_next_packet / picoquic_incoming_packet
 *   Transport (UDP / 内存)        字节通道，datagram 语义
 * ```
 *
 * ## 三个关键实现点
 *
 * ### 1. 回调是"换掉"而不是"再包一层"
 *
 * `PicoquicConnection` 在创建连接时挂了自己的回调（把每个流的字节攒起来）。
 * 本类拿到 `picoquic_cnx_t*` 后用 `picoquic_set_callback` 把回调换成
 * `Impl::Callback`。因此：
 *
 * - `PicoquicConnection::HandshakeComplete()` 不再更新，**本类自己用
 *   `ready` 标志跟踪**，不读 wrapper 的握手状态；
 * - `PicoquicConnection::ReadStream()` 不再有数据，也不需要。
 *
 * ### 2. `h3zero_protocol_init` 必须发，而且要在发请求之前
 *
 * HTTP/3 连接上必须先发 SETTINGS（控制流）、QPACK encoder/decoder 流。
 * `h3zero_protocol_init(cnx)` 一次做完这三件事（内部会分配 3 个单向流）。
 * 我们在 `picoquic_callback_ready` 里调用它，而 `Send` 会先驱动握手到
 * `ready`，所以顺序天然正确。
 *
 * ### 3. 帧头长度用 QUIC varint 自己写，QPACK 一律用 picohttp
 *
 * HEADERS/DATA 的**帧头**（类型 + 长度）是 QUIC varint（RFC 9000 §16），
 * 不属于 QPACK，所以这里用 5 行的 `AppendVarint` 写。**字段段本身**
 * （`:method`/`:scheme`/`:path`/`:authority`/UA/自定义头）全部由
 * `h3zero_create_request_header_frame_ex` /
 * `h3zero_create_post_header_frame_ex` / `h3zero_qpack_literal_plus_name_encode`
 * 编码 —— 没有一行自写 QPACK。
 *
 * 注意 picohttp 的请求构造函数会在字段段最前面写两个 0 字节
 * （Required Insert Count = 0，Delta Base = 0），这是 QPACK 字段段前缀，
 * 必须原样保留在该字段段的最前面。
 */

#include "go2cpp/network/http3/client.hpp"

#include "go2cpp/network/http2/transport.hpp"
#include "go2cpp/network/quic/picoquic_connection.hpp"

#include <h3zero.h>
#include <h3zero_common.h>
#include <picoquic.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <utility>

namespace go2cpp::net::http3 {
namespace {

using Clock = std::chrono::steady_clock;

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::string ToUpperAscii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string HexCode(std::uint64_t code) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(code));
    return std::string(buffer);
}

/**
 * @brief 追加一个 QUIC varint（RFC 9000 §16）。
 *
 * 只用于 HTTP/3 的**帧头**（帧类型与帧长度），不涉及 QPACK。
 */
void AppendVarint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    if (value < 0x40ULL) {
        out.push_back(static_cast<std::uint8_t>(value));
    } else if (value < 0x4000ULL) {
        out.push_back(static_cast<std::uint8_t>(0x40U | (value >> 8)));
        out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    } else if (value < 0x40000000ULL) {
        out.push_back(static_cast<std::uint8_t>(0x80U | (value >> 24)));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    } else {
        out.push_back(static_cast<std::uint8_t>(0xC0U | (value >> 56)));
        out.push_back(static_cast<std::uint8_t>((value >> 48) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 40) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 32) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    }
}

/**
 * @brief `h3zero_content_type_enum` 到 MIME 字符串。
 *
 * picohttp 只提供 "path -> enum" 与 "enum -> 静态表 code" 两个方向，
 * 没有 "enum -> MIME 字符串"。响应头里要还原 `content-type` 文本，
 * 只能在本地做这一张小表 —— 这不是 QPACK 解码，enum 已经在
 * `h3zero_header_parts_t` 里解好了。
 */
const char* ContentTypeName(h3zero_content_type_enum type) {
    switch (type) {
        case h3zero_content_type_text_html:
            return "text/html";
        case h3zero_content_type_text_plain:
            return "text/plain";
        case h3zero_content_type_image_gif:
            return "image/gif";
        case h3zero_content_type_image_jpeg:
            return "image/jpeg";
        case h3zero_content_type_image_png:
            return "image/png";
        case h3zero_content_type_dns_message:
            return "application/dns-message";
        case h3zero_content_type_javascript:
            return "application/javascript";
        case h3zero_content_type_json:
            return "application/json";
        case h3zero_content_type_www_form_urlencoded:
            return "application/x-www-form-urlencoded";
        case h3zero_content_type_text_css:
            return "text/css";
        case h3zero_content_type_not_supported:
            return "application/octet-stream";
        case h3zero_content_type_none:
        default:
            return "";
    }
}

/// HTTP header 名字的合法字符集（RFC 7230 `token`，已排除大写，因为我们先转小写）。
bool IsValidHeaderName(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) != 0) {
            continue;
        }
        switch (c) {
            case '!':
            case '#':
            case '$':
            case '%':
            case '&':
            case '\'':
            case '*':
            case '+':
            case '-':
            case '.':
            case '^':
            case '_':
            case '`':
            case '|':
            case '~':
                break;
            default:
                return false;
        }
    }
    return true;
}

/**
 * @brief 把请求编码成一条流上的完整字节序列：HEADERS [ + DATA ]。
 *
 * QPACK 字段段全部交给 picohttp；这里只负责：
 * 1. 选 GET/POST 的字段段构造函数；
 * 2. 把自定义头以 literal-with-name 追加到字段段（仍然是 picohttp 的编码器）；
 * 3. 用 QUIC varint 包 HEADERS/DATA 帧头。
 */
bool BuildRequestBytes(const Request& request, const std::string& authority,
                       std::vector<std::uint8_t>* bytes, std::string* error) {
    const std::string method = ToUpperAscii(request.method);
    if (method != "GET" && method != "POST") {
        SetError(error, "HTTP/3 client only supports GET and POST, got '" + request.method + "'");
        return false;
    }
    if (request.path.empty() || request.path[0] != '/') {
        SetError(error, "HTTP/3 request path must start with '/', got '" + request.path + "'");
        return false;
    }

    // 自定义头先规整：小写、拒绝伪头、抽出 user-agent。
    std::vector<std::pair<std::string, std::string>> extra_headers;
    std::string user_agent = H3ZERO_USER_AGENT_STRING;
    for (const auto& header : request.headers) {
        std::string name = ToLowerAscii(header.first);
        if (name.empty()) {
            SetError(error, "HTTP/3 request contains an empty header name");
            return false;
        }
        if (name[0] == ':') {
            SetError(error, "HTTP/3 pseudo header '" + name + "' must not be set by the caller");
            return false;
        }
        if (!IsValidHeaderName(name)) {
            SetError(error, "HTTP/3 request contains an invalid header name '" + header.first + "'");
            return false;
        }
        if (name == "user-agent") {
            user_agent = header.second;
        } else {
            extra_headers.emplace_back(std::move(name), header.second);
        }
    }

    // picohttp 的构造器按 "原始字节 + 少量固定字段" 估算大小；这里给足冗余。
    std::size_t capacity = 64 + (request.path.size() + authority.size() + user_agent.size()) * 2;
    for (const auto& header : extra_headers) {
        capacity += 8 + (header.first.size() + header.second.size()) * 2;
    }
    std::vector<std::uint8_t> fields(capacity);
    std::uint8_t* fields_max = fields.data() + fields.size();

    std::uint8_t* last = nullptr;
    if (method == "POST") {
        last = h3zero_create_post_header_frame_ex(
            fields.data(), fields_max, reinterpret_cast<const std::uint8_t*>(request.path.data()),
            request.path.size(), nullptr, 0, authority.c_str(), h3zero_content_type_text_plain,
            user_agent.empty() ? nullptr : user_agent.c_str());
    } else {
        last = h3zero_create_request_header_frame_ex(
            fields.data(), fields_max, reinterpret_cast<const std::uint8_t*>(request.path.data()),
            request.path.size(), nullptr, 0, authority.c_str(),
            user_agent.empty() ? nullptr : user_agent.c_str());
    }
    if (last == nullptr) {
        SetError(error, "picohttp failed to encode the HTTP/3 request header section");
        return false;
    }

    for (const auto& header : extra_headers) {
        last = h3zero_qpack_literal_plus_name_encode(
            last, fields_max, reinterpret_cast<const std::uint8_t*>(header.first.data()),
            header.first.size(), reinterpret_cast<const std::uint8_t*>(header.second.data()),
            header.second.size());
        if (last == nullptr) {
            SetError(error, "picohttp failed to encode HTTP/3 header '" + header.first + "'");
            return false;
        }
    }

    const std::size_t field_section_length = static_cast<std::size_t>(last - fields.data());
    bytes->clear();
    bytes->reserve(field_section_length + request.body.size() + 24);
    AppendVarint(*bytes, static_cast<std::uint64_t>(h3zero_frame_header));
    AppendVarint(*bytes, field_section_length);
    bytes->insert(bytes->end(), fields.begin(), fields.begin() + static_cast<std::ptrdiff_t>(field_section_length));

    // POST 的 body 用 DATA 帧；body 为空时不发 DATA 帧（H3 允许只有 HEADERS）。
    if (!request.body.empty()) {
        AppendVarint(*bytes, static_cast<std::uint64_t>(h3zero_frame_data));
        AppendVarint(*bytes, request.body.size());
        bytes->insert(bytes->end(), request.body.begin(), request.body.end());
    }

    return true;
}

/// `Poll` 的单次默认等待上限（毫秒）。
constexpr int kClientPollWaitMs = 1000;

/// 距 deadline 的剩余毫秒数（已过期返回 0）。
std::int64_t RemainingMs(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return left.count() < 0 ? 0 : left.count();
}

/**
 * @brief 取关闭类事件的关闭码（审计 D05）。
 *
 * picoquic 1.1.53 的 `close`/`application_close`/`stateless_reset` 回调都是
 * `bytes = NULL, len = 0`，关闭码必须从连接上下文读，不能解 `bytes`。
 */
std::uint64_t CloseErrorCode(picoquic_cnx_t* cnx) {
    if (cnx == nullptr) {
        return 0;
    }
    // 注意 picoquic_get_application_error() 只返回对端的 application error；
    // 本地 close(code) 的码必须用 picoquic_get_close_reasons() 取（D05）。
    std::uint64_t local_transport = 0;
    std::uint64_t remote_transport = 0;
    std::uint64_t local_application = 0;
    std::uint64_t remote_application = 0;
    picoquic_get_close_reasons(cnx, &local_transport, &remote_transport, &local_application,
                               &remote_application);
    if (remote_application != 0) {
        return remote_application;
    }
    if (local_application != 0) {
        return local_application;
    }
    if (remote_transport != 0) {
        return remote_transport;
    }
    return local_transport;
}

}  // namespace

struct Client::Impl {
    /// 每个客户端双向流一个解析状态。析构自动释放 picohttp 内部缓冲。
    struct StreamState {
        h3zero_data_stream_state_t parser{};
        Response response;
        bool header_captured{false};
        bool fin{false};
        bool failed{false};
        bool flow_opened{false};
        std::string fail_reason;

        StreamState() = default;
        StreamState(const StreamState&) = delete;
        StreamState& operator=(const StreamState&) = delete;
        ~StreamState() { h3zero_delete_data_stream_state(&parser); }
    };

    net::http2::Transport* transport{nullptr};
    std::unique_ptr<quic::PicoquicConnection> connection;
    std::string sni;
    std::string alpn;
    bool ready{false};
    bool closed{false};
    std::uint64_t last_error{0};
    std::string close_reason;
    Options options;
    std::uint64_t last_stream_id{0};

    std::map<std::uint64_t, StreamState> streams;
    std::deque<std::uint64_t> pending;

    ~Impl() {
        // D01：PicoquicConnection 析构会先发 CONNECTION_CLOSE 再 picoquic_free；
        // 该过程会回调本对象的 Callback（close 事件），此时 Impl 仍存活，安全。
        connection.reset();
    }

    /// `:authority`：显式配置优先，否则退回 SNI（保持旧行为）。
    const std::string& Authority() const {
        return options.authority.empty() ? sni : options.authority;
    }

    static int Callback(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes,
                        std::size_t length, picoquic_call_back_event_t event,
                        void* callback_ctx, void* stream_ctx);

    void HandleStreamData(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes,
                          std::size_t length, bool fin);
    void FailStream(std::uint64_t stream_id, const std::string& reason);
    void CaptureHeader(StreamState* stream);
};

/**
 * @brief picoquic 的全局事件回调：把 C 库的连接/流事件翻译成本对象的状态。
 * @param cnx 连接句柄（只在 `ready` 分支用于 `h3zero_protocol_init`）。
 * @param stream_id 事件所属的 QUIC 流 id（流类事件有效）。
 * @param bytes/length 事件携带的数据（`stream_data`/`stream_fin` 为流字节，
 *        `close` 类事件为关闭码）。
 * @param event 事件类型。
 * @param callback_ctx 就是 `Impl*` 自身（picoquic 透传）。
 * @return 0——picoquic 要求回调返回 0；非 0 会被当成致命错误。
 *
 * ## 定位
 *
 * 这是**本项目与 picoquic 的唯一交互面**：所有 QUIC/HTTP3 协议事件都从这里
 * 进入 C++ 侧。文件头也强调「唯一 include `picohttp`（h3zero/QPACK）头的地方」。
 *
 * ## 步骤
 *
 * step1. **`ready`**：先 `h3zero_protocol_init` 发出 SETTINGS 与 QPACK
 *        encoder/decoder 流，**之后**才允许上层发请求（顺序不能反，否则对端
 *        会在控制流建立前收到 HEADERS）；初始化失败时置 `closed` 并记录原因；
 * step2. **`set_alpn`**：记录协商出的 ALPN（诊断用）；
 * step3. **流事件**：`stream_data`/`stream_fin` 交给 `HandleStreamData`
 *        （按 `fin` 区分是否收尾）；`stream_reset`/`stop_sending` 交给
 *        `FailStream` 标记失败；
 * step4. **关闭类事件**（`close`/`application_close`/`stateless_reset`）：
 *        置 `closed`，用 `DecodeCloseCode` 解析应用层关闭码，并记录首个原因。
 *
 * ## 并发/失败语义
 *
 * - **必须返回 0**：picoquic 把非 0 返回值当作致命错误，会直接拆连接；
 * - **回调在 picoquic 自己的线程上执行**，不能阻塞、不能 park；本函数只做
 *   状态更新与数据搬运，没有等待；
 * - **`self == nullptr` 时返回 0**：`~Impl` 里 `connection.reset()` 会触发
 *   一次 `close` 回调，此时 `callback_ctx` 仍有效，但防御性地判空；
 * - **`ready` 只处理一次**（`if (self->ready) break;`）：picoquic 可能重复
 *   触发，重复 init 会破坏控制流；
 * - **关闭原因只记第一次**（`if (close_reason.empty())`）：后续关闭事件
 *   （如先 `application_close` 再 `close`）不覆盖更具体的信息；
 * - **`stream_id` 的流类型过滤在 `HandleStreamData`/`FailStream` 里做**，
 *   本函数不做——因为它们各自需要不同的过滤理由（只关心双向流 vs 只标记已知流）；
 * - **不抛异常**：C 回调栈不能传播 C++ 异常，所有分支都只做不会抛的操作。
 */
int Client::Impl::Callback(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes,
                           std::size_t length, picoquic_call_back_event_t event,
                           void* callback_ctx, void* /*stream_ctx*/) {
    auto* self = static_cast<Impl*>(callback_ctx);
    if (self == nullptr) {
        return 0;
    }

    switch (event) {
        // step1: 连接就绪——先建控制流（SETTINGS/QPACK），之后才允许发请求。
        case picoquic_callback_ready: {
            if (self->ready) {
                break;
            }
            // D14：先发 SETTINGS / QPACK streams，成功后才置 ready，避免
            // HandshakeComplete() 在控制流初始化失败时「撒谎」。
            const int rc = h3zero_protocol_init(cnx);
            if (rc != 0) {
                self->closed = true;
                self->last_error = H3ZERO_INTERNAL_ERROR;
                self->close_reason = "h3zero_protocol_init failed";
                break;
            }
            self->ready = true;
            break;
        }
        case picoquic_callback_set_alpn:
            if (bytes != nullptr && length > 0) {
                self->alpn.assign(reinterpret_cast<const char*>(bytes), length);
            }
            break;
        // step3: 流事件——数据/收尾交给 HandleStreamData，reset 标记失败。
        case picoquic_callback_stream_data:
            self->HandleStreamData(cnx, stream_id, bytes, length, false);
            break;
        case picoquic_callback_stream_fin:
            self->HandleStreamData(cnx, stream_id, bytes, length, true);
            break;
        case picoquic_callback_stream_reset:
            self->FailStream(stream_id, "stream reset by peer");
            break;
        case picoquic_callback_stop_sending:
            self->FailStream(stream_id, "peer requested stop sending");
            break;
        // step4: 关闭类事件——置 closed、解析关闭码、只记首个原因。
        case picoquic_callback_close:
        case picoquic_callback_application_close:
        case picoquic_callback_stateless_reset:
            self->closed = true;
            // D05：bytes/len 恒为 NULL/0，关闭码从连接上下文取。
            self->last_error = CloseErrorCode(cnx);
            if (self->close_reason.empty()) {
                self->close_reason = (event == picoquic_callback_application_close)
                                         ? "connection closed by application"
                                         : "connection closed";
            }
            break;
        default:
            break;
    }
    return 0;
}

void Client::Impl::FailStream(std::uint64_t stream_id, const std::string& reason) {
    // 只关心客户端主动发起的双向流（低 2 位为 00）。
    if ((stream_id & 0x3U) != 0) {
        return;
    }
    auto it = streams.find(stream_id);
    if (it == streams.end()) {
        return;
    }
    it->second.failed = true;
    if (it->second.fail_reason.empty()) {
        it->second.fail_reason = reason;
    }
}

void Client::Impl::CaptureHeader(StreamState* stream) {
    stream->header_captured = true;
    stream->response.status = stream->parser.header.status;
    const char* content_type = ContentTypeName(stream->parser.header.content_type);
    if (content_type[0] != '\0') {
        stream->response.headers.emplace_back("content-type", content_type);
    }
}

void Client::Impl::HandleStreamData(picoquic_cnx_t* cnx, std::uint64_t stream_id,
                                    std::uint8_t* bytes, std::size_t length, bool fin) {
    // 服务端控制流（3）、QPACK encoder/decoder（7/11）都是单向流，本实现不使用
    // 动态表，直接忽略。只解析客户端发起的双向流（0, 4, 8, ...）。
    if ((stream_id & 0x3U) != 0) {
        return;
    }

    auto it = streams.find(stream_id);
    if (it == streams.end()) {
        if (!fin) {
            return;
        }
        it = streams.try_emplace(stream_id).first;
        it->second.response.stream_id = stream_id;
    }
    StreamState& stream = it->second;
    if (stream.failed) {
        return;
    }

    if (bytes != nullptr && length > 0) {
        std::uint8_t* cursor = bytes;
        std::uint8_t* const end = bytes + length;
        while (cursor != nullptr && cursor < end) {
            std::size_t available_data = 0;
            std::uint64_t error_found = 0;
            cursor = h3zero_parse_data_stream(cursor, end, &stream.parser, &available_data,
                                              &error_found);
            if (cursor == nullptr) {
                stream.failed = true;
                stream.fail_reason = "HTTP/3 parse failed on stream " + std::to_string(stream_id) +
                                     ", error " + HexCode(error_found);
                last_error = error_found;
                // 与 picoquic demo 客户端一致：解析失败直接关连接，避免状态机继续跑。
                (void)picoquic_close(cnx, error_found);
                return;
            }
            if (available_data > 0) {
                // D08：响应体上限，避免被对端声明的超长 DATA 帧拖成 OOM。
                const std::size_t max_body = options.max_response_bytes;
                if (max_body != 0 && stream.response.body.size() + available_data > max_body) {
                    stream.failed = true;
                    stream.fail_reason = "HTTP/3 response body exceeds limit (" +
                                         std::to_string(max_body) + " bytes)";
                    last_error = H3ZERO_INTERNAL_ERROR;
                    (void)picoquic_close(cnx, H3ZERO_INTERNAL_ERROR);
                    return;
                }
                if (!stream.flow_opened) {
                    stream.flow_opened = true;
                    // 只按实际需要放开流控；对端声明超大长度时不照单全收（D08）。
                    if (stream.parser.current_frame_length > 0x100000ULL) {
                        const std::uint64_t window =
                            (max_body == 0)
                                ? stream.parser.current_frame_length
                                : std::min<std::uint64_t>(stream.parser.current_frame_length,
                                                          max_body);
                        (void)picoquic_open_flow_control(cnx, stream_id, window);
                    }
                }
                stream.response.body.append(reinterpret_cast<const char*>(cursor), available_data);
                cursor += available_data;
            }
        }
    }

    if (!stream.header_captured && stream.parser.header_found != 0) {
        CaptureHeader(&stream);
    }
    if (fin) {
        stream.fin = true;
    }
}

std::unique_ptr<Client> Client::Connect(net::http2::Transport* transport, const std::string& sni,
                                        std::string* error) {
    return Connect(transport, sni, Options{}, error);
}

std::unique_ptr<Client> Client::Connect(net::http2::Transport* transport, const std::string& sni,
                                        const Options& options, std::string* error) {
    if (transport == nullptr) {
        SetError(error, "transport is null");
        return nullptr;
    }
    if (sni.empty()) {
        SetError(error, "SNI must not be empty");
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->transport = transport;
    impl->sni = sni;
    impl->options = options;

    quic::PicoquicConfig config;
    config.alpn = "h3";
    config.sni = sni;
    // D04：把证书校验开关与 CA 路径透传到 QUIC 层。默认仍是 false（本地自签），
    // 但调用方现在可以通过 Options 真正打开校验。
    config.verify_peer = options.verify_peer;
    config.cert_root_file_name = options.cert_root_file_name;

    impl->connection = quic::PicoquicConnection::Connect(transport, config, error);
    if (impl->connection == nullptr) {
        return nullptr;
    }

    auto* cnx = static_cast<picoquic_cnx_t*>(impl->connection->NativeHandle());
    if (cnx == nullptr) {
        SetError(error, "picoquic connection handle is null");
        return nullptr;
    }
    // 换掉 QUIC 封装的"攒字节"回调，改由本类解析 HTTP/3 帧。
    picoquic_set_callback(cnx, &Impl::Callback, impl.get());

    return std::unique_ptr<Client>(new Client(std::move(impl)));
}

Client::Client(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

Client::~Client() = default;

bool Client::Poll(std::string* error) {
    Impl& state = *m_impl;
    if (state.closed) {
        SetError(error, state.close_reason.empty() ? "connection closed" : state.close_reason);
        return false;
    }
    std::string poll_error;
    if (!state.connection->PollFor(&poll_error, kClientPollWaitMs)) {
        state.closed = true;
        if (state.close_reason.empty()) {
            state.close_reason = poll_error;
        }
        SetError(error, state.close_reason);
        return false;
    }
    if (state.closed) {
        SetError(error, state.close_reason.empty() ? "connection closed" : state.close_reason);
        return false;
    }
    return true;
}

bool Client::Send(const Request& req, std::string* error) {
    Impl& state = *m_impl;
    if (state.closed) {
        SetError(error, state.close_reason.empty() ? "connection closed" : state.close_reason);
        return false;
    }

    // D10：method/path/header 校验与编码都是纯本地操作，放在握手之前，
    // 参数错误立即返回，不会被 5s 握手超时掩盖。
    std::vector<std::uint8_t> bytes;
    if (!BuildRequestBytes(req, state.Authority(), &bytes, error)) {
        return false;
    }

    // 握手未完成时先推进握手（HTTP/3 的 SETTINGS 在 ready 回调里发）。
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(state.options.timeout_ms);
    while (!state.ready) {
        if (state.closed) {
            SetError(error, state.close_reason.empty() ? "connection closed" : state.close_reason);
            return false;
        }
        const std::int64_t remaining = RemainingMs(deadline);
        if (remaining <= 0) {
            SetError(error, "HTTP/3 handshake timeout after " +
                                std::to_string(state.options.timeout_ms) + " ms");
            return false;
        }
        // D02：把剩余预算交给 PollFor，单次阻塞读不会超出 deadline。
        std::string poll_error;
        if (!state.connection->PollFor(&poll_error, static_cast<int>(remaining))) {
            state.closed = true;
            if (state.close_reason.empty()) {
                state.close_reason = poll_error;
            }
            SetError(error, poll_error);
            return false;
        }
    }

    // D14：成功协商后必须是 h3；ALPN 回调尚未到达（空串）时不拦截。
    if (!state.alpn.empty() && state.alpn != "h3") {
        SetError(error, "HTTP/3 requires ALPN 'h3', negotiated '" + state.alpn + "'");
        return false;
    }

    auto* cnx = static_cast<picoquic_cnx_t*>(state.connection->NativeHandle());
    if (cnx == nullptr) {
        SetError(error, "picoquic connection handle is null");
        return false;
    }

    const std::uint64_t stream_id = picoquic_get_next_local_stream_id(cnx, 0);
    auto inserted = state.streams.try_emplace(stream_id);
    if (!inserted.second) {
        SetError(error, "QUIC stream " + std::to_string(stream_id) + " already exists");
        return false;
    }
    inserted.first->second.response.stream_id = stream_id;

    // GET 与 POST 都在这一批字节里带上 FIN：请求体已经完整，不需要等对端要数据。
    if (picoquic_add_to_stream(cnx, stream_id, bytes.data(), bytes.size(), 1) != 0) {
        state.streams.erase(stream_id);
        SetError(error, "picoquic_add_to_stream failed on stream " + std::to_string(stream_id));
        return false;
    }

    state.pending.push_back(stream_id);
    state.last_stream_id = stream_id;
    return true;
}

bool Client::Receive(Response* resp, std::string* error) {
    if (resp == nullptr) {
        SetError(error, "response is null");
        return false;
    }
    Impl& state = *m_impl;
    if (state.pending.empty()) {
        SetError(error, "no pending HTTP/3 request, call Send first");
        return false;
    }

    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(state.options.timeout_ms);
    for (;;) {
        const std::uint64_t stream_id = state.pending.front();
        auto it = state.streams.find(stream_id);
        if (it == state.streams.end()) {
            state.pending.pop_front();
            SetError(error, "internal error: stream " + std::to_string(stream_id) +
                                " has no parser state");
            return false;
        }
        Impl::StreamState& stream = it->second;

        if (stream.failed) {
            const std::string reason = stream.fail_reason;
            state.streams.erase(it);
            state.pending.pop_front();
            SetError(error, reason);
            return false;
        }
        if (stream.fin) {
            if (!stream.header_captured) {
                state.streams.erase(it);
                state.pending.pop_front();
                SetError(error, "HTTP/3 stream " + std::to_string(stream_id) +
                                    " ended without a HEADERS frame");
                return false;
            }
            *resp = stream.response;
            state.streams.erase(it);
            state.pending.pop_front();
            return true;
        }
        if (state.closed) {
            SetError(error, state.close_reason.empty() ? "connection closed" : state.close_reason);
            return false;
        }
        const std::int64_t remaining = RemainingMs(deadline);
        if (remaining <= 0) {
            SetError(error, "HTTP/3 response timeout after " +
                                std::to_string(state.options.timeout_ms) + " ms on stream " +
                                std::to_string(stream_id));
            return false;
        }
        // D02：按剩余预算驱动，阻塞读不会击穿 deadline。
        std::string poll_error;
        if (!state.connection->PollFor(&poll_error, static_cast<int>(remaining))) {
            state.closed = true;
            if (state.close_reason.empty()) {
                state.close_reason = poll_error;
            }
            SetError(error, poll_error);
            return false;
        }
    }
}

bool Client::Close(std::uint64_t application_error_code, std::string* error) {
    Impl& state = *m_impl;
    std::string close_error;
    bool ok = true;
    if (state.connection != nullptr) {
        ok = state.connection->Close(application_error_code, &close_error);
    }
    state.closed = true;
    if (state.close_reason.empty()) {
        state.close_reason = "connection closed locally";
    }
    if (error != nullptr) {
        *error = ok ? std::string() : close_error;
    }
    return ok;
}

bool Client::HandshakeComplete() const noexcept {
    return m_impl != nullptr && m_impl->ready;
}

bool Client::Closed() const noexcept {
    return m_impl == nullptr || m_impl->closed;
}

std::string Client::Alpn() const {
    if (m_impl == nullptr) {
        return {};
    }
    if (!m_impl->alpn.empty()) {
        return m_impl->alpn;
    }
    auto* cnx = static_cast<picoquic_cnx_t*>(m_impl->connection->NativeHandle());
    if (cnx == nullptr) {
        return {};
    }
    const char* negotiated = picoquic_tls_get_negotiated_alpn(cnx);
    return negotiated == nullptr ? std::string() : std::string(negotiated);
}

std::uint64_t Client::LastStreamId() const noexcept {
    return m_impl == nullptr ? 0 : m_impl->last_stream_id;
}

std::uint64_t Client::ConnectionError() const noexcept {
    return m_impl == nullptr ? 0 : m_impl->last_error;
}

}  // namespace go2cpp::net::http3
