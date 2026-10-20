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

/// `:authority` 的最大长度（防止把巨大字符串塞进 HTTP/3 头）。
constexpr std::size_t kMaxAuthorityLength = 1024;
/// 服务端控制流缓冲上限：SETTINGS/GOAWAY 都很小，1 MiB 足够且能防 DoS。
constexpr std::size_t kMaxControlStreamBuffer = 1U << 20;
/// 单个控制流帧长度上限。
constexpr std::size_t kMaxControlFrameLength = 1U << 20;
/// QPACK 单向流累计消费上限（不使用动态表时正常只有几十字节）。
constexpr std::uint64_t kMaxQpackStreamBytes = 1U << 20;

/**
 * @brief 校验 authority / SNI 文本：拒绝控制字符、空白和会破坏 URL 的字符。
 *
 * 只做“不能注入”的最小校验，不做 DNS 语法校验（SNI 允许任意合法域名）。
 */
bool IsValidAuthorityText(const std::string& text) {
    if (text.empty() || text.size() > kMaxAuthorityLength) {
        return false;
    }
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc == 0x7f) {
            return false;
        }
        switch (c) {
            case '/':
            case '?':
            case '#':
            case '\\':
            case '"':
            case '<':
            case '>':
                return false;
            default:
                break;
        }
    }
    return true;
}

/**
 * @brief 从 `host[:port]` 里拆出 host 与端口（D15）。
 *
 * 支持 `example.com`、`example.com:8443`、`[::1]`、`[::1]:8443` 与裸 IPv6
 * `::1`。不解析 DNS 合法性；只做结构拆分与端口范围校验。
 */
bool SplitHostPort(const std::string& input, std::string* host, std::uint16_t* port,
                   std::string* error) {
    if (input.empty()) {
        SetError(error, "host must not be empty");
        return false;
    }
    std::string parsed_host;
    std::string parsed_port;
    if (input.front() == '[') {
        const std::size_t close = input.find(']');
        if (close == std::string::npos) {
            SetError(error, "invalid IPv6 host '" + input + "' (missing ']')");
            return false;
        }
        parsed_host = input.substr(1, close - 1);
        if (close + 1 < input.size()) {
            if (input[close + 1] != ':') {
                SetError(error, "invalid host '" + input + "' (expected ':port' after ']')");
                return false;
            }
            parsed_port = input.substr(close + 2);
        }
    } else {
        const std::size_t first = input.find(':');
        const std::size_t last = input.rfind(':');
        if (first != std::string::npos && first == last) {
            parsed_host = input.substr(0, first);
            parsed_port = input.substr(first + 1);
        } else {
            parsed_host = input;  // 无端口，或裸 IPv6 字面量（多个冒号）
        }
    }
    if (parsed_host.empty()) {
        SetError(error, "host must not be empty in '" + input + "'");
        return false;
    }
    std::uint32_t value = 0;
    for (char c : parsed_port) {
        if (c < '0' || c > '9') {
            SetError(error, "invalid port in '" + input + "'");
            return false;
        }
        value = value * 10U + static_cast<std::uint32_t>(c - '0');
        if (value > 65535U) {
            SetError(error, "port out of range in '" + input + "'");
            return false;
        }
    }
    if (!parsed_port.empty() && value == 0) {
        SetError(error, "port must be 1..65535 in '" + input + "'");
        return false;
    }
    *host = parsed_host;
    *port = static_cast<std::uint16_t>(value);
    return true;
}

/// 生成 `host[:port]`；IPv6 字面量自动加方括号。
std::string FormatAuthority(const std::string& host, std::uint16_t port) {
    std::string out;
    if (host.find(':') != std::string::npos) {
        out += '[';
        out += host;
        out += ']';
    } else {
        out = host;
    }
    if (port != 0) {
        out += ':';
        out += std::to_string(port);
    }
    return out;
}

/**
 * @brief 计算 `:authority` 与实际使用的 SNI（D15：两者可分离）。
 *
 * 优先级：
 * 1. `options.authority` 非空 → 原样使用（只做注入校验）；
 * 2. `options.host` 非空 → 用它的 host（允许它自带端口）；
 * 3. 否则从 `sni_arg` 拆出 host（旧用法 `host:port` 也能正确拆开）；
 * 端口依次由 `options.port`、显式 host 的端口、`sni_arg` 的端口决定。
 * SNI 使用 `options.sni`（拆掉端口），否则用 `sni_arg` 的 host。
 */
bool BuildAuthority(const std::string& sni_arg, const Client::Options& options,
                    std::string* authority, std::string* effective_sni, std::string* error) {
    std::string sni_host;
    std::uint16_t sni_port = 0;
    if (!SplitHostPort(sni_arg, &sni_host, &sni_port, error)) {
        return false;
    }

    // D15：SNI 与 authority 的 host 分别演进——给 options.host 不会改变 SNI。
    std::string authority_host = sni_host;
    std::uint16_t port = sni_port;
    if (!options.host.empty()) {
        std::uint16_t host_port = 0;
        if (!SplitHostPort(options.host, &authority_host, &host_port, error)) {
            return false;
        }
        port = host_port;
    }
    if (options.port != 0) {
        port = options.port;
    }

    if (!options.sni.empty()) {
        std::uint16_t ignored_port = 0;
        if (!SplitHostPort(options.sni, &sni_host, &ignored_port, error)) {
            return false;
        }
    }
    if (!IsValidAuthorityText(sni_host)) {
        SetError(error, "invalid SNI '" + sni_host + "'");
        return false;
    }

    if (options.authority.empty()) {
        *authority = FormatAuthority(authority_host, port);
    } else {
        *authority = options.authority;
    }
    if (!IsValidAuthorityText(*authority)) {
        SetError(error, "invalid :authority '" + *authority + "'");
        return false;
    }
    *effective_sni = sni_host;
    return true;
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
    /// 实际发送的 `:authority`（host[:port]），Connect 时计算一次（D15）。
    std::string authority;
    std::string alpn;
    bool ready{false};
    bool closed{false};
    std::uint64_t last_error{0};
    std::string close_reason;
    Options options;
    std::uint64_t last_stream_id{Client::kInvalidStreamId};

    /// 回调中捕获了异常：置位后由 Poll/Send/Receive 转换为错误（D18）。
    bool callback_exception{false};

    /// 服务端控制流（stream 3）解析状态（D16）。
    std::vector<std::uint8_t> control_buffer;
    /// 单向流的首个 varint 是流类型（control=0x00 / QPACK=0x02/0x03）。
    bool control_type_seen{false};
    bool qpack_encoder_type_seen{false};
    bool qpack_decoder_type_seen{false};
    bool control_frame_seen{false};
    bool server_settings_received{false};
    h3zero_settings_t remote_settings{};
    bool goaway_received{false};
    std::uint64_t goaway_id{Client::kInvalidStreamId};
    bool max_push_id_seen{false};
    std::uint64_t max_push_id{0};
    /// QPACK encoder(7) / decoder(11) 流累计消费字节（D16：只读不解析）。
    std::uint64_t qpack_encoder_bytes{0};
    std::uint64_t qpack_decoder_bytes{0};

    std::map<std::uint64_t, StreamState> streams;
    std::deque<std::uint64_t> pending;

    ~Impl() {
        // D01：PicoquicConnection 析构会先发 CONNECTION_CLOSE 再 picoquic_free；
        // 该过程会回调本对象的 Callback（close 事件），此时 Impl 仍存活，安全。
        connection.reset();
    }

    static int Callback(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes,
                        std::size_t length, picoquic_call_back_event_t event,
                        void* callback_ctx, void* stream_ctx);

    /// 回调主体（异常由 `Callback` 的 try/catch 兜住，见 D18）。
    void HandleEvent(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes,
                     std::size_t length, picoquic_call_back_event_t event);

    /// 按流类型分流：0=请求流，1/2=非法服务端流，3=服务端单向流（D16）。
    void OnStreamBytes(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes,
                       std::size_t length, bool fin);

    /// 服务端单向流：控制流 3 / QPACK encoder 7 / decoder 11。
    void HandleServerUniStream(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes,
                               std::size_t length, bool fin);

    /// 解析控制流上的 SETTINGS / GOAWAY，未知帧按 RFC 9114 §9 忽略（D16）。
    void HandleControlStream(picoquic_cnx_t* cnx, std::uint8_t* bytes, std::size_t length,
                             bool fin);

    /// QPACK 单向流只消费不解析，避免流控窗口长期不打开（D16）。
    void ConsumeQpackStream(picoquic_cnx_t* cnx, bool encoder, std::uint8_t* bytes,
                            std::size_t length);

    /// 置连接级失败并发送 CONNECTION_CLOSE；幂等。
    void FailConnection(picoquic_cnx_t* cnx, std::uint64_t h3_error, const char* reason);

    /// PollFor 失败后对外暴露的原因（D18：回调异常优先于底层传输错误）。
    std::string PollReason(const std::string& poll_error) const;

    void HandleStreamData(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes,
                          std::size_t length, bool fin);
    void FailStream(std::uint64_t stream_id, const std::string& reason);
    void CaptureHeader(StreamState* stream);
};

/**
 * @brief picoquic 的全局事件回调：把 C 库的连接/流事件翻译成本对象的状态。
 *
 * ## 定位
 *
 * 这是**本项目与 picoquic 的唯一交互面**：所有 QUIC/HTTP3 协议事件都从这里
 * 进入 C++ 侧。文件头也强调「唯一 include `picohttp`（h3zero/QPACK）头的地方」。
 *
 * ## 步骤
 *
 * step1. **`ready`**：`h3zero_protocol_init` 发出 SETTINGS 与 QPACK
 *        encoder/decoder 流，成功后才置 `ready`（D14）；
 * step2. **`set_alpn`**：记录协商出的 ALPN；
 * step3. **流事件**：按 `stream_id & 0x3` 分流（D16）——
 *        0 = 客户端双向请求流；1/2 = 服务端非法流（连接错误）；
 *        3 = 服务端单向流（控制流 3 / QPACK 7/11）；
 *        `stream_reset`/`stop_sending` 交给 `FailStream` 标记失败；
 * step4. **关闭类事件**：置 `closed`，用 `CloseErrorCode` 取关闭码。
 *
 * ## 并发/失败语义
 *
 * - **必须返回 0**：picoquic 把非 0 返回值当作致命错误，会直接拆连接；
 * - **回调在调用 `Poll`/`Send`/`Receive` 的同一线程同步执行**（不是 picoquic
 *   自己的线程，picoquic 没有第二线程），不能阻塞、不能 park；
 * - **D18：整个回调体用 try/catch 包住**。`std::bad_alloc`、`length_error`
 *   等异常绝不能穿过 C ABI：catch 里只做不抛的赋值与 `picoquic_close`，
 *   并置 `callback_exception`，由 `Poll` 转成明确错误；
 * - **`self == nullptr` 时返回 0**：`~Impl` 里 `connection.reset()` 会触发
 *   一次 `close` 回调，此时 `callback_ctx` 仍有效，但防御性地判空；
 * - **`ready` 只处理一次**（`if (self->ready) break;`）：重复 init 会破坏控制流；
 * - **关闭原因只记第一次**（`if (close_reason.empty())`）。
 */
int Client::Impl::Callback(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes,
                           std::size_t length, picoquic_call_back_event_t event,
                           void* callback_ctx, void* /*stream_ctx*/) {
    auto* self = static_cast<Impl*>(callback_ctx);
    if (self == nullptr) {
        return 0;
    }
    try {
        self->HandleEvent(cnx, stream_id, bytes, length, event);
    } catch (...) {
        // D18：异常不得穿透 C 回调边界。catch 内只做 noexcept 级操作：
        // 置标志 + picoquic_close（C 函数，不抛），字符串留到上层再拼。
        self->callback_exception = true;
        self->closed = true;
        self->last_error = H3ZERO_INTERNAL_ERROR;
        if (cnx != nullptr) {
            (void)picoquic_close(cnx, H3ZERO_INTERNAL_ERROR);
        }
    }
    return 0;
}

void Client::Impl::HandleEvent(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes,
                               std::size_t length, picoquic_call_back_event_t event) {
    switch (event) {
        case picoquic_callback_ready: {
            if (ready) {
                break;
            }
            // D14：先发 SETTINGS / QPACK streams，成功后才置 ready。
            const int rc = h3zero_protocol_init(cnx);
            if (rc != 0) {
                FailConnection(cnx, H3ZERO_INTERNAL_ERROR, "h3zero_protocol_init failed");
                break;
            }
            ready = true;
            break;
        }
        case picoquic_callback_set_alpn:
            if (bytes != nullptr && length > 0) {
                alpn.assign(reinterpret_cast<const char*>(bytes), length);
            }
            break;
        case picoquic_callback_stream_data:
            OnStreamBytes(cnx, stream_id, bytes, length, false);
            break;
        case picoquic_callback_stream_fin:
            OnStreamBytes(cnx, stream_id, bytes, length, true);
            break;
        case picoquic_callback_stream_reset:
            FailStream(stream_id, "stream reset by peer");
            break;
        case picoquic_callback_stop_sending:
            FailStream(stream_id, "peer requested stop sending");
            break;
        case picoquic_callback_close:
        case picoquic_callback_application_close:
        case picoquic_callback_stateless_reset:
            closed = true;
            // D05：bytes/len 恒为 NULL/0，关闭码从连接上下文取。
            last_error = CloseErrorCode(cnx);
            if (close_reason.empty()) {
                close_reason = (event == picoquic_callback_application_close)
                                   ? "connection closed by application"
                                   : "connection closed";
            }
            break;
        default:
            break;
    }
}

void Client::Impl::OnStreamBytes(picoquic_cnx_t* cnx, std::uint64_t stream_id, std::uint8_t* bytes,
                                 std::size_t length, bool fin) {
    if (closed) {
        return;
    }
    switch (stream_id & 0x3U) {
        case 0:  // 客户端发起的双向流：请求的响应
            HandleStreamData(cnx, stream_id, bytes, length, fin);
            break;
        case 1:  // 服务端发起的双向流：HTTP/3 不允许
            FailConnection(cnx, H3ZERO_STREAM_CREATION_ERROR,
                           "server-initiated bidirectional stream is not allowed");
            break;
        case 2:  // 服务端 push 流：本客户端未启用 push
            FailConnection(cnx, H3ZERO_STREAM_CREATION_ERROR,
                           "server push stream received but push is not enabled");
            break;
        default:  // 3：服务端发起的单向流
            HandleServerUniStream(cnx, stream_id, bytes, length, fin);
            break;
    }
}

void Client::Impl::HandleServerUniStream(picoquic_cnx_t* cnx, std::uint64_t stream_id,
                                         std::uint8_t* bytes, std::size_t length, bool fin) {
    if (stream_id == 3) {
        HandleControlStream(cnx, bytes, length, fin);
        return;
    }
    if (stream_id == 7) {
        ConsumeQpackStream(cnx, true, bytes, length);
    } else if (stream_id == 11) {
        ConsumeQpackStream(cnx, false, bytes, length);
    } else {
        FailConnection(cnx, H3ZERO_STREAM_CREATION_ERROR,
                       "unexpected server-initiated unidirectional stream");
        return;
    }
    if (fin && !closed) {
        FailConnection(cnx, H3ZERO_CLOSED_CRITICAL_STREAM, "QPACK critical stream closed");
    }
}

void Client::Impl::HandleControlStream(picoquic_cnx_t* cnx, std::uint8_t* bytes, std::size_t length,
                                       bool fin) {
    if (closed) {
        return;
    }
    // 单向流以流类型 varint 开头（RFC 9114 §6.2），控制流类型必须是 0x00。
    if (!control_type_seen) {
        if (length == 0) {
            if (fin) {
                FailConnection(cnx, H3ZERO_CLOSED_CRITICAL_STREAM,
                               "control stream closed before its type");
            }
            return;
        }
        std::uint64_t stream_type = 0;
        const std::size_t prefix = h3zero_varint_decode(bytes, length, &stream_type);
        if (prefix == 0 || stream_type != h3zero_stream_type_control) {
            FailConnection(cnx, H3ZERO_STREAM_CREATION_ERROR, "invalid control stream type");
            return;
        }
        control_type_seen = true;
        bytes += prefix;
        length -= prefix;
    }
    if (length > kMaxControlStreamBuffer - std::min(control_buffer.size(), kMaxControlStreamBuffer)) {
        FailConnection(cnx, H3ZERO_EXCESSIVE_LOAD, "control stream buffer exceeds limit");
        return;
    }
    if (bytes != nullptr && length > 0) {
        control_buffer.insert(control_buffer.end(), bytes, bytes + length);
    }

    std::size_t pos = 0;
    while (pos < control_buffer.size()) {
        const std::uint8_t* base = control_buffer.data();
        const std::size_t available = control_buffer.size() - pos;
        std::uint64_t frame_type = 0;
        const std::size_t type_len = h3zero_varint_decode(base + pos, available, &frame_type);
        if (type_len == 0) {
            break;  // 帧类型还没收全
        }
        std::uint64_t frame_length = 0;
        const std::size_t length_len =
            h3zero_varint_decode(base + pos + type_len, available - type_len, &frame_length);
        if (length_len == 0) {
            break;  // 帧长度还没收全
        }
        const std::size_t header_len = type_len + length_len;
        if (frame_length > kMaxControlFrameLength) {
            FailConnection(cnx, H3ZERO_EXCESSIVE_LOAD, "control frame length exceeds limit");
            return;
        }
        if (available - header_len < frame_length) {
            break;  // 帧内容还没收全
        }
        const std::uint8_t* payload = base + pos + header_len;

        // RFC 9114 §6.2.1：控制流的第一帧必须是 SETTINGS。
        if (!control_frame_seen) {
            if (frame_type != h3zero_frame_settings) {
                FailConnection(cnx, H3ZERO_MISSING_SETTINGS,
                               "first control stream frame is not SETTINGS");
                return;
            }
            control_frame_seen = true;
        }

        switch (frame_type) {
            case h3zero_frame_settings: {
                if (server_settings_received) {
                    FailConnection(cnx, H3ZERO_FRAME_UNEXPECTED, "duplicate SETTINGS frame");
                    return;
                }
                h3zero_settings_t decoded{};
                const std::uint8_t* const end = payload + frame_length;
                const std::uint8_t* const parsed =
                    h3zero_settings_components_decode(payload, end, &decoded);
                if (parsed != end) {
                    FailConnection(cnx, H3ZERO_SETTINGS_ERROR, "invalid SETTINGS frame");
                    return;
                }
                remote_settings = decoded;
                server_settings_received = true;
                break;
            }
            case h3zero_frame_goaway: {
                std::uint64_t stream_id = 0;
                const std::size_t got = h3zero_varint_decode(payload, frame_length, &stream_id);
                if (got == 0 || got != frame_length) {
                    FailConnection(cnx, H3ZERO_FRAME_ERROR, "invalid GOAWAY frame");
                    return;
                }
                // RFC 9114 §5.2：后续 GOAWAY 的流号只能更小（不能“复活”流）。
                if (goaway_received && stream_id > goaway_id) {
                    FailConnection(cnx, H3ZERO_ID_ERROR, "GOAWAY stream ID increased");
                    return;
                }
                goaway_received = true;
                goaway_id = stream_id;
                break;
            }
            case h3zero_frame_max_push_id: {
                // RFC 9114 §7.2.7：MAX_PUSH_ID 只能由客户端发送；服务端发来属于
                // 协议违规。这里仍消费并记录（便于诊断），不主动断连，避免与
                // 非标准实现对撞。
                std::uint64_t push_id = 0;
                const std::size_t got = h3zero_varint_decode(payload, frame_length, &push_id);
                if (got == 0 || got != frame_length) {
                    FailConnection(cnx, H3ZERO_FRAME_ERROR, "invalid MAX_PUSH_ID frame");
                    return;
                }
                max_push_id_seen = true;
                max_push_id = push_id;
                break;
            }
            case h3zero_frame_data:
            case h3zero_frame_header:
            case h3zero_frame_push_promise:
            case 2:
            case 6:
            case 8:
            case 9:
                FailConnection(cnx, H3ZERO_FRAME_UNEXPECTED,
                               "request/response frame on the control stream");
                return;
            default:
                // RFC 9114 §9：未知帧必须被忽略。
                break;
        }
        pos += header_len + static_cast<std::size_t>(frame_length);
    }

    if (pos > 0) {
        control_buffer.erase(control_buffer.begin(),
                             control_buffer.begin() + static_cast<std::ptrdiff_t>(pos));
    }
    if (fin && !closed) {
        // RFC 9114 §6.2：关闭临界流是连接级错误；若还有半帧则先报 FRAME_ERROR。
        if (!control_buffer.empty()) {
            FailConnection(cnx, H3ZERO_FRAME_ERROR, "control stream ended mid-frame");
        } else {
            FailConnection(cnx, H3ZERO_CLOSED_CRITICAL_STREAM, "control stream closed by peer");
        }
    }
}

void Client::Impl::ConsumeQpackStream(picoquic_cnx_t* cnx, bool encoder, std::uint8_t* bytes,
                                      std::size_t length) {
    if (closed) {
        return;
    }
    bool& type_seen = encoder ? qpack_encoder_type_seen : qpack_decoder_type_seen;
    if (!type_seen) {
        if (length == 0) {
            return;
        }
        std::uint64_t stream_type = 0;
        const std::size_t prefix = h3zero_varint_decode(bytes, length, &stream_type);
        const std::uint64_t expected =
            encoder ? h3zero_stream_type_qpack_encoder : h3zero_stream_type_qpack_decoder;
        if (prefix == 0 || stream_type != expected) {
            FailConnection(cnx, H3ZERO_STREAM_CREATION_ERROR, "invalid QPACK stream type");
            return;
        }
        type_seen = true;
        bytes += prefix;
        length -= prefix;
    }
    std::uint64_t& total = encoder ? qpack_encoder_bytes : qpack_decoder_bytes;
    total += static_cast<std::uint64_t>(length);
    if (total > kMaxQpackStreamBytes) {
        FailConnection(cnx, H3ZERO_EXCESSIVE_LOAD, "QPACK stream exceeded the consume limit");
    }
    // 不用动态表：只消费字节以保持流控窗口打开，不解析插入/确认指令。
}

void Client::Impl::FailConnection(picoquic_cnx_t* cnx, std::uint64_t h3_error, const char* reason) {
    if (closed) {
        return;
    }
    closed = true;
    last_error = h3_error;
    if (close_reason.empty() && reason != nullptr) {
        close_reason = reason;
    }
    if (cnx != nullptr) {
        (void)picoquic_close(cnx, h3_error);
    }
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
    // 调用方（OnStreamBytes）已按流类型分流；这里只处理客户端双向流（0, 4, 8, ...）。
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
                // 与 picoquic demo 客户端一致：解析失败直接关连接，避免状态机继续跑。
                FailConnection(cnx, error_found, "HTTP/3 response parsing failed");
                return;
            }
            if (available_data > 0) {
                // D08：响应体上限，避免被对端声明的超长 DATA 帧拖成 OOM。
                const std::size_t max_body = options.max_response_bytes;
                if (max_body != 0 && stream.response.body.size() + available_data > max_body) {
                    stream.failed = true;
                    stream.fail_reason = "HTTP/3 response body exceeds limit (" +
                                         std::to_string(max_body) + " bytes)";
                    FailConnection(cnx, H3ZERO_INTERNAL_ERROR,
                                   "HTTP/3 response body exceeds the configured limit");
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

    // D15：把 `:authority` 与 TLS SNI 分开计算。旧用法 `Connect(transport,
    // "host:port")` 现在会得到 SNI=host、authority=host:port。
    std::string effective_sni;
    std::string authority;
    if (!BuildAuthority(sni, options, &authority, &effective_sni, error)) {
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->transport = transport;
    impl->sni = effective_sni;
    impl->authority = authority;
    impl->options = options;

    quic::PicoquicConfig config;
    config.alpn = "h3";
    config.sni = effective_sni;
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
        poll_error = state.PollReason(poll_error);
        if (state.close_reason.empty()) {
            state.close_reason = poll_error;
        }
        SetError(error, state.close_reason);
        return false;
    }
    if (state.closed) {
        if (state.close_reason.empty()) {
            state.close_reason = state.PollReason(std::string());
        }
        SetError(error, state.close_reason);
        return false;
    }
    // D21：成功路径清空上一次残留的错误，避免调用方误读旧超时字符串。
    if (error != nullptr) {
        error->clear();
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
    if (!BuildRequestBytes(req, state.authority, &bytes, error)) {
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
            poll_error = state.PollReason(poll_error);
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

    // D16：GOAWAY 之后拒绝创建号 >= goaway_id 的新流（RFC 9114 §5.2）。
    if (state.goaway_received && stream_id >= state.goaway_id) {
        SetError(error, "server sent GOAWAY (last accepted stream id " +
                            std::to_string(state.goaway_id) + "); refusing new stream " +
                            std::to_string(stream_id));
        return false;
    }

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
    // D21：成功路径清空错误。
    if (error != nullptr) {
        error->clear();
    }
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
            // D21：成功路径清空错误。
            if (error != nullptr) {
                error->clear();
            }
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
            poll_error = state.PollReason(poll_error);
            if (state.close_reason.empty()) {
                state.close_reason = poll_error;
            }
            SetError(error, poll_error);
            return false;
        }
    }
}

std::string Client::Impl::PollReason(const std::string& poll_error) const {
    if (callback_exception) {
        return "exception escaped HTTP/3 callback";
    }
    return poll_error.empty() ? "connection closed" : poll_error;
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
    return m_impl == nullptr ? kInvalidStreamId : m_impl->last_stream_id;
}

std::string Client::Authority() const {
    return m_impl == nullptr ? std::string() : m_impl->authority;
}

std::string Client::Sni() const {
    return m_impl == nullptr ? std::string() : m_impl->sni;
}

bool Client::GoAwayReceived() const noexcept {
    return m_impl != nullptr && m_impl->goaway_received;
}

std::uint64_t Client::GoAwayId() const noexcept {
    return m_impl == nullptr ? kInvalidStreamId : m_impl->goaway_id;
}

bool Client::ServerSettingsReceived() const noexcept {
    return m_impl != nullptr && m_impl->server_settings_received;
}

std::uint64_t Client::ConnectionError() const noexcept {
    return m_impl == nullptr ? 0 : m_impl->last_error;
}

}  // namespace go2cpp::net::http3
