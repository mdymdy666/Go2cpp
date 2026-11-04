/**
 * @file gateway.cpp
 * @brief 通用 HTTP 网关实现：注册式路由、中间件链、可插拔 Resolver、JSON 配置。
 *
 * @details 依赖只到 `go2cpp::http`（Parser/Client）与 `go2cpp::net::Socket`。
 *          请求解析复用 `http::Parser`，上游转发复用 `http::Client`；连接循环与
 *          响应序列化在本文件实现：`http::Server::serve_connection()` 经
 *          `Response::to_string()` 输出 reason phrase，429 等不在 `http::Status`
 *          枚举内的状态码会得到空 reason，导致客户端解析失败。不使用
 *          `bento::HttpServer`（依赖 bento/io/sse/scheduler 整链）。
 *
 * @author go2cpp contributors
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "go2cpp/app/gateway.hpp"

#include "go2cpp/logging/log.hpp"

#include "go2cpp/network/socket.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace go2cpp::app::gateway {
namespace {

/// @brief 内部鉴权标记头：客户端传入的同名头在入口处清除，防止伪造。
constexpr const char* kAuthHeader = "X-Go2Cpp-Gateway-Auth";
/// @brief 鉴权成功后注入的转发头。
constexpr const char* kUserHeader = "X-User-Id";
/// @brief 内建健康检查路径。
constexpr const char* kHealthPath = "/health";
/// @brief 内建聚合健康路径。
constexpr const char* kHealthAllPath = "/api/health/all";

/// @brief 把字符串按 `/` 切段（忽略前导 `/`，保留空段）。
std::vector<std::string_view> SplitPath(std::string_view path) {
    std::vector<std::string_view> segments;
    std::size_t index = 0;
    if (!path.empty() && path.front() == '/') {
        index = 1;
    }
    while (index <= path.size()) {
        const std::size_t slash = path.find('/', index);
        if (slash == std::string_view::npos) {
            if (index < path.size()) {
                segments.push_back(path.substr(index));
            }
            break;
        }
        segments.push_back(path.substr(index, slash - index));
        index = slash + 1;
    }
    return segments;
}

/// @brief 大小写不敏感的 ASCII 比较（头名用）。
bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

/// @brief 去掉首尾的 HTTP 空白（空格/制表符），用于头名与头值。
std::string_view TrimOws(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/**
 * @brief 判断已收齐的请求头是否需要在读 body 前先回 `100 Continue`（G4）。
 * @param head [in] 本请求已收到的原始字节——借用，仅本次调用期间有效；不可为空，
 *        且必须包含完整头部块（含结尾 `\r\n\r\n`）。
 * @param header_end [in] 头部终止符 `\r\n\r\n` 在 `head` 中的起始偏移——由调用方
 *        用 `find()` 得到，必须小于 `head.size()`。
 * @return
 * - true：调用方**必须**先写出 `HTTP/1.1 100 Continue\r\n\r\n`，否则客户端会在
 *   等 100 时白等到超时；
 * - false：直接解析，别发 100，否则无 body 的请求会多收到一个状态行。
 *
 * ## 定位
 * 100-continue 的唯一判据（ServeConnection 解析前的探测）。判断错会导致客户端
 * 等待 100 超时（漏发）或把 100 误当最终响应（白发）。
 *
 * ## 步骤
 * step1. 起始行必须声明 HTTP/1.1——HTTP/1.0 没有 100-continue 语义；
 * step2. 逐行扫描头部：`Expect: 100-continue` 置位，`Content-Length > 0` 或存在
 *        非空 `Transfer-Encoding` 置「有 body」；
 * step3. 两者同时成立才发；无 body 的 Expect 客户端本就不会等 body。
 *
 * ## 并发/失败语义
 * - 调用约束：纯函数，无共享状态；可在任意线程调用。
 * - 失败语义：不抛异常；头部畸形时返回 false，交由 Parser 报 Error（400）。
 */
bool ShouldSendContinue(std::string_view head, std::size_t header_end) {
    // step1: HTTP/1.0 客户端不支持 100-continue，按普通请求处理。
    const std::size_t request_line_end = head.find("\r\n");
    if (request_line_end == std::string_view::npos || request_line_end > header_end) {
        return false;
    }
    if (head.substr(0, request_line_end).find("HTTP/1.1") == std::string_view::npos) {
        return false;
    }
    // step2: 逐行提取 Expect 与 body 定界信息。
    bool expect_continue = false;
    bool has_body = false;
    std::size_t line = request_line_end + 2;
    while (line <= header_end) {
        const std::size_t line_end = head.find("\r\n", line);
        const std::size_t stop =
            (line_end == std::string_view::npos || line_end > header_end) ? header_end
                                                                         : line_end;
        const std::string_view field = head.substr(line, stop - line);
        const std::size_t colon = field.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view name = TrimOws(field.substr(0, colon));
            const std::string_view value = TrimOws(field.substr(colon + 1));
            if (EqualsIgnoreCase(name, "Expect") &&
                EqualsIgnoreCase(value, "100-continue")) {
                expect_continue = true;
            } else if (EqualsIgnoreCase(name, "Content-Length")) {
                std::uint64_t length = 0;
                for (const char c : value) {
                    if (c < '0' || c > '9') {
                        break;
                    }
                    length = length * 10 + static_cast<std::uint64_t>(c - '0');
                }
                has_body = length > 0;
            } else if (EqualsIgnoreCase(name, "Transfer-Encoding") && !value.empty()) {
                has_body = true;
            }
        }
        if (stop == header_end) {
            break;
        }
        line = stop + 2;
    }
    // step3: 只有「客户端确实在等，而且后面有 body」时才预应答。
    return expect_continue && has_body;
}

/// @brief JSON 字符串转义（用于错误体与聚合健康）。
std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

/// @brief 统一错误体：`{"ok":false,"error":"...","code":N}`。
std::string ErrorBody(int code, std::string_view message) {
    return "{\"ok\":false,\"error\":\"" + JsonEscape(message) +
           "\",\"code\":" + std::to_string(code) + "}";
}

/// @brief 构造统一错误响应。
Response ErrorResponse(int code, std::string_view message) {
    Response response;
    response.status = static_cast<http::Status>(code);
    response.header.set("Content-Type", "application/json; charset=utf-8");
    response.body = ErrorBody(code, message);
    return response;
}

/// @brief 构造成功 JSON 响应。
Response JsonResponse(std::string body) {
    Response response;
    response.status = http::Status::OK;
    response.header.set("Content-Type", "application/json; charset=utf-8");
    response.body = std::move(body);
    return response;
}

/// @brief 状态码 → reason phrase；未知码补标准短语（库的 status_text 只覆盖枚举值）。
std::string ReasonFor(int code) {
    std::string reason = http::status_text(static_cast<http::Status>(code));
    if (!reason.empty()) {
        return reason;
    }
    switch (code) {
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 451: return "Unavailable For Legal Reasons";
        default: return "Unknown";
    }
}

/// @brief 序列化响应；显式写 Content-Length，支持 429 等不在 http::Status 枚举内的码。
std::string SerializeResponse(http::Method method, const Response& response) {
    const int code = static_cast<int>(response.status);
    std::string wire = "HTTP/1.1 " + std::to_string(code) + " " + ReasonFor(code) + "\r\n";
    for (const auto& entry : response.header.entries()) {
        if (EqualsIgnoreCase(entry.first, "Content-Length") ||
            EqualsIgnoreCase(entry.first, "Transfer-Encoding") ||
            EqualsIgnoreCase(entry.first, "Connection")) {
            continue;
        }
        wire += entry.first;
        wire += ": ";
        wire += entry.second;
        wire += "\r\n";
    }
    const bool body_forbidden =
        (code >= 100 && code < 200) || code == 204 || code == 304;
    if (!body_forbidden) {
        wire += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    }
    // 连接去留由调用方按「是否还有流水线请求/对端是否要求关闭」决定，
    // 序列化器只忠实反映该决定，避免响应头与实际关闭行为不一致。
    wire += response.close ? "Connection: close\r\n\r\n"
                           : "Connection: keep-alive\r\n\r\n";
    if (!body_forbidden && method != http::Method::Head) {
        wire += response.body;
    }
    return wire;
}

/// @brief 循环写完整缓冲；对端关闭或超时返回 false。
bool WriteAll(const net::Socket::Ptr& socket, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto result = socket->send(data.data() + sent, data.size() - sent,
                                         MSG_NOSIGNAL);
        if (!result.ok() || result.transferred <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result.transferred);
    }
    return true;
}

/// @brief 判断方法掩码是否包含给定方法。
bool MethodMatches(MethodMask mask, http::Method method) {
    return (mask & method_mask::Of(method)) != 0;
}

/// @brief 解析 `host:port`（IPv6 用 `[::1]:80`）；失败返回 false。
bool ParseUpstreamText(std::string_view text, Upstream* upstream) {
    if (text.empty() || upstream == nullptr) {
        return false;
    }
    std::string_view host;
    std::string_view port_text;
    if (text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos || close + 2 > text.size() ||
            text[close + 1] != ':') {
            return false;
        }
        host = text.substr(1, close - 1);
        port_text = text.substr(close + 2);
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
    }
    if (host.empty() || port_text.empty()) {
        return false;
    }
    long port = 0;
    for (const char c : port_text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        port = port * 10 + (c - '0');
        if (port > 65535) {
            return false;
        }
    }
    if (port <= 0) {
        return false;
    }
    upstream->host = std::string(host);
    upstream->port = static_cast<std::uint16_t>(port);
    return true;
}

/// @brief hop-by-hop 头（转发出站与响应回写都要剔除）。
bool IsHopByHop(std::string_view name) {
    static constexpr const char* kNames[] = {
        "connection",       "keep-alive",         "proxy-authenticate",
        "proxy-authorization", "proxy-connection", "te",
        "trailer",          "transfer-encoding",  "upgrade",
        "content-length",
    };
    for (const char* candidate : kNames) {
        if (EqualsIgnoreCase(name, candidate)) {
            return true;
        }
    }
    return false;
}

/// @brief 极简 JSON 值（只覆盖网关配置需要的结构）。
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type{Type::Null};
    bool boolean{false};
    double number{0.0};
    std::string text;
    std::vector<JsonValue> items;
    std::map<std::string, JsonValue> fields;

    /// @brief 取对象字段；不存在或不是对象返回 nullptr。
    const JsonValue* Find(std::string_view key) const {
        if (type != Type::Object) {
            return nullptr;
        }
        const auto found = fields.find(std::string(key));
        return found == fields.end() ? nullptr : &found->second;
    }
};

/// @brief 递归下降 JSON 解析器（深度上限 32，拒绝尾随字符）。
class JsonParser final {
public:
    explicit JsonParser(std::string_view text) : m_text(text) {}

    bool Parse(JsonValue* value, std::string* error) {
        SkipSpace();
        if (!ParseValue(value, 0)) {
            if (error != nullptr) {
                *error = m_error;
            }
            return false;
        }
        SkipSpace();
        if (m_pos != m_text.size()) {
            if (error != nullptr) {
                *error = "trailing characters at offset " + std::to_string(m_pos);
            }
            return false;
        }
        return true;
    }

private:
    bool Fail(std::string message) {
        if (m_error.empty()) {
            m_error = std::move(message) + " at offset " + std::to_string(m_pos);
        }
        return false;
    }

    void SkipSpace() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_pos;
            } else {
                break;
            }
        }
    }

    bool ParseValue(JsonValue* value, int depth) {
        if (depth > 32) {
            return Fail("nesting too deep");
        }
        SkipSpace();
        if (m_pos >= m_text.size()) {
            return Fail("unexpected end of input");
        }
        const char c = m_text[m_pos];
        if (c == '{') {
            return ParseObject(value, depth);
        }
        if (c == '[') {
            return ParseArray(value, depth);
        }
        if (c == '"') {
            value->type = JsonValue::Type::String;
            return ParseString(&value->text);
        }
        if (c == 't') {
            if (m_text.substr(m_pos, 4) != "true") {
                return Fail("invalid literal");
            }
            m_pos += 4;
            value->type = JsonValue::Type::Bool;
            value->boolean = true;
            return true;
        }
        if (c == 'f') {
            if (m_text.substr(m_pos, 5) != "false") {
                return Fail("invalid literal");
            }
            m_pos += 5;
            value->type = JsonValue::Type::Bool;
            value->boolean = false;
            return true;
        }
        if (c == 'n') {
            if (m_text.substr(m_pos, 4) != "null") {
                return Fail("invalid literal");
            }
            m_pos += 4;
            value->type = JsonValue::Type::Null;
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return ParseNumber(value);
        }
        return Fail("unexpected character");
    }

    static int HexValue(char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    static void AppendUtf8(unsigned code, std::string* out) {
        if (code < 0x80) {
            *out += static_cast<char>(code);
        } else if (code < 0x800) {
            *out += static_cast<char>(0xC0 | (code >> 6));
            *out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            *out += static_cast<char>(0xE0 | (code >> 12));
            *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool ParseString(std::string* out) {
        if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
            return Fail("expected string");
        }
        ++m_pos;
        out->clear();
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                *out += c;
                continue;
            }
            if (m_pos >= m_text.size()) {
                return Fail("unterminated escape");
            }
            const char escape = m_text[m_pos++];
            switch (escape) {
                case '"': *out += '"'; break;
                case '\\': *out += '\\'; break;
                case '/': *out += '/'; break;
                case 'b': *out += '\b'; break;
                case 'f': *out += '\f'; break;
                case 'n': *out += '\n'; break;
                case 'r': *out += '\r'; break;
                case 't': *out += '\t'; break;
                case 'u': {
                    if (m_pos + 4 > m_text.size()) {
                        return Fail("truncated unicode escape");
                    }
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int digit = HexValue(m_text[m_pos + static_cast<std::size_t>(i)]);
                        if (digit < 0) {
                            return Fail("invalid unicode escape");
                        }
                        code = code * 16 + static_cast<unsigned>(digit);
                    }
                    m_pos += 4;
                    AppendUtf8(code, out);
                    break;
                }
                default:
                    return Fail("invalid escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseNumber(JsonValue* value) {
        const std::size_t start = m_pos;
        if (m_text[m_pos] == '-') {
            ++m_pos;
        }
        while (m_pos < m_text.size() &&
               std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) {
            ++m_pos;
        }
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            ++m_pos;
            while (m_pos < m_text.size() &&
                   std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) {
                ++m_pos;
            }
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_text.size() &&
                (m_text[m_pos] == '+' || m_text[m_pos] == '-')) {
                ++m_pos;
            }
            while (m_pos < m_text.size() &&
                   std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) {
                ++m_pos;
            }
        }
        const std::string token(m_text.substr(start, m_pos - start));
        char* end = nullptr;
        value->number = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || *end != '\0') {
            return Fail("invalid number");
        }
        value->type = JsonValue::Type::Number;
        return true;
    }

    bool ParseArray(JsonValue* value, int depth) {
        value->type = JsonValue::Type::Array;
        ++m_pos;  // '['
        SkipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        for (;;) {
            JsonValue item;
            if (!ParseValue(&item, depth + 1)) {
                return false;
            }
            value->items.push_back(std::move(item));
            SkipSpace();
            if (m_pos >= m_text.size()) {
                return Fail("unterminated array");
            }
            if (m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            if (m_text[m_pos] != ',') {
                return Fail("expected ',' or ']'");
            }
            ++m_pos;
        }
    }

    bool ParseObject(JsonValue* value, int depth) {
        value->type = JsonValue::Type::Object;
        ++m_pos;  // '{'
        SkipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        for (;;) {
            SkipSpace();
            std::string key;
            if (!ParseString(&key)) {
                return false;
            }
            SkipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
                return Fail("expected ':'");
            }
            ++m_pos;
            JsonValue child;
            if (!ParseValue(&child, depth + 1)) {
                return false;
            }
            value->fields[std::move(key)] = std::move(child);
            SkipSpace();
            if (m_pos >= m_text.size()) {
                return Fail("unterminated object");
            }
            if (m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            if (m_text[m_pos] != ',') {
                return Fail("expected ',' or '}'");
            }
            ++m_pos;
        }
    }

    std::string_view m_text;
    std::size_t m_pos{0};
    std::string m_error;
};

/// @brief 执行中间件链（洋葱模型由递归自然形成）。
Result<void> InvokeChain(
    const std::vector<Middleware>& middlewares, std::size_t index, Request& request,
    Response& response,
    const std::function<Result<void>(Request&, Response&)>& tail) {
    if (index >= middlewares.size()) {
        return tail(request, response);
    }
    Next next = [&middlewares, index, &request, &response, &tail]() {
        return InvokeChain(middlewares, index + 1, request, response, tail);
    };
    if (!middlewares[index]) {
        return next();
    }
    return middlewares[index](request, response, next);
}

}  // namespace

struct Gateway::Impl {
    /// @brief 路由种类；枚举值顺序就是匹配优先级（Custom 最高）。
    enum class Kind { Custom = 0, Static = 1, Param = 2, Wildcard = 3 };

    /// @brief 一条已注册路由（运行期不可变；热更新用替换整个 shared_ptr）。
    struct RouteEntry {
        Kind kind{Kind::Static};
        MethodMask method_mask{method_mask::kAll};
        /// 原始模式（静态/参数/通配都用它做同名覆盖判断）。
        std::string pattern;
        /// 通配前缀（如 `/api/**` 的 `/api`；`/**` 为空）。
        std::string prefix;
        /// 参数/静态路由的分段；参数段存参数名。
        std::vector<std::string> segments;
        /// 与 segments 对齐：true 表示该段是参数。
        std::vector<bool> is_param;
        /// 原始 upstream 文本（健康聚合按它去重）。
        std::string upstream_text;
        /// 解析后的默认上游。
        Upstream upstream;
        bool require_auth{false};
        std::chrono::milliseconds timeout{500};
        int priority{0};
        std::uint64_t order{0};
        std::shared_ptr<Matcher> matcher;
        std::shared_ptr<Handler> handler;
        std::string name;
    };

    /// @brief 匹配结果：命中的路由与参数。
    struct RouteMatch {
        std::shared_ptr<RouteEntry> entry;
        PathParams params;
    };

    explicit Impl(Config value) : config(std::move(value)) {}

    ~Impl() { Stop(); }

    // ---- 注册 -------------------------------------------------------------
    Result<void> AddRoute(RouteSpec spec) {
        if (spec.path_pattern.empty() || spec.path_pattern.front() != '/') {
            return Result<void>::Err("path_pattern must start with '/'");
        }
        auto entry = std::make_shared<RouteEntry>();
        entry->method_mask =
            spec.method_mask == method_mask::kNone ? method_mask::kAll : spec.method_mask;
        entry->pattern = spec.path_pattern;
        entry->require_auth = spec.require_auth;
        entry->timeout = spec.timeout;
        if (entry->timeout.count() <= 0) {
            entry->timeout = std::chrono::milliseconds(500);
        }
        entry->priority = spec.priority;
        entry->name = std::move(spec.name);

        const auto raw_segments = SplitPath(entry->pattern);
        bool has_param = false;
        bool has_wildcard = false;
        for (const auto segment : raw_segments) {
            if (segment == "**") {
                has_wildcard = true;
            } else if (!segment.empty() && segment.front() == ':') {
                has_param = true;
            }
        }
        if (has_wildcard) {
            if (has_param) {
                return Result<void>::Err("wildcard pattern cannot contain ':param' segments");
            }
            if (raw_segments.empty() || raw_segments.back() != "**") {
                return Result<void>::Err("'**' must be the last path segment");
            }
            entry->kind = Kind::Wildcard;
            std::string prefix = entry->pattern.substr(0, entry->pattern.size() - 2);
            if (!prefix.empty() && prefix.back() == '/') {
                prefix.pop_back();
            }
            entry->prefix = std::move(prefix);
        } else if (has_param) {
            entry->kind = Kind::Param;
            for (const auto segment : raw_segments) {
                if (!segment.empty() && segment.front() == ':') {
                    const std::string name(segment.substr(1));
                    if (name.empty()) {
                        return Result<void>::Err("empty parameter name in path_pattern");
                    }
                    entry->segments.push_back(name);
                    entry->is_param.push_back(true);
                } else {
                    entry->segments.emplace_back(segment);
                    entry->is_param.push_back(false);
                }
            }
        } else {
            entry->kind = Kind::Static;
        }

        if (spec.upstream.empty()) {
            return Result<void>::Err("upstream is required for AddRoute(RouteSpec)");
        }
        Upstream upstream;
        if (!ParseUpstreamText(spec.upstream, &upstream)) {
            return Result<void>::Err("invalid upstream (expected host:port): " + spec.upstream);
        }
        entry->upstream_text = spec.upstream;
        entry->upstream = upstream;

        const std::weak_ptr<RouteEntry> weak_entry = entry;
        entry->handler = std::make_shared<Handler>(
            [this, weak_entry](Request& request, Response& response) {
                const auto locked = weak_entry.lock();
                if (!locked) {
                    response = ErrorResponse(500, "route is no longer available");
                    return Result<void>::Ok();
                }
                return Forward(*locked, request, response);
            });

        {
            std::unique_lock<std::shared_mutex> lock(routes_mutex);
            for (auto& existing : routes) {
                if (existing->kind != Kind::Custom && existing->kind == entry->kind &&
                    existing->pattern == entry->pattern &&
                    existing->method_mask == entry->method_mask) {
                    entry->order = existing->order;
                    existing = std::move(entry);
                    return Result<void>::Ok();
                }
            }
            entry->order = next_order++;
            routes.push_back(std::move(entry));
        }
        return Result<void>::Ok();
    }

    Result<void> AddCustomRoute(std::shared_ptr<Matcher> matcher,
                                std::shared_ptr<Handler> handler) {
        if (!matcher) {
            return Result<void>::Err("matcher must not be null");
        }
        if (!handler) {
            return Result<void>::Err("handler must not be null");
        }
        auto entry = std::make_shared<RouteEntry>();
        entry->kind = Kind::Custom;
        entry->matcher = std::move(matcher);
        entry->handler = std::move(handler);
        entry->timeout = std::chrono::milliseconds(500);
        std::unique_lock<std::shared_mutex> lock(routes_mutex);
        entry->order = next_order++;
        routes.push_back(std::move(entry));
        return Result<void>::Ok();
    }

    void AddMiddleware(Middleware middleware) {
        if (!middleware) {
            return;
        }
        std::unique_lock<std::shared_mutex> lock(routes_mutex);
        middlewares.push_back(std::move(middleware));
    }

    void SetResolver(Resolver value) {
        std::unique_lock<std::shared_mutex> lock(routes_mutex);
        resolver = std::move(value);
    }

    // ---- 匹配 -------------------------------------------------------------
    bool Match(const Request& request, RouteMatch* match) const {
        std::vector<std::shared_ptr<RouteEntry>> candidates;
        {
            std::shared_lock<std::shared_mutex> lock(routes_mutex);
            candidates.reserve(routes.size());
            for (const auto& route : routes) {
                if (MethodMatches(route->method_mask, request.method)) {
                    candidates.push_back(route);
                }
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const std::shared_ptr<RouteEntry>& left,
                            const std::shared_ptr<RouteEntry>& right) {
                             const int left_kind = static_cast<int>(left->kind);
                             const int right_kind = static_cast<int>(right->kind);
                             if (left_kind != right_kind) {
                                 return left_kind < right_kind;
                             }
                             if (left->priority != right->priority) {
                                 return left->priority > right->priority;
                             }
                             return left->order < right->order;
                         });
        const std::string_view path = request.path_view();
        for (const auto& route : candidates) {
            PathParams params;
            bool matched = false;
            if (route->kind == Kind::Custom) {
                matched = route->matcher && route->matcher->Match(request, &params);
            } else {
                matched = MatchPath(*route, path, &params);
            }
            if (matched) {
                match->entry = route;
                match->params = std::move(params);
                return true;
            }
        }
        return false;
    }

    static bool MatchPath(const RouteEntry& route, std::string_view path,
                          PathParams* params) {
        switch (route.kind) {
            case Kind::Static:
                return path == route.pattern;
            case Kind::Param: {
                const auto segments = SplitPath(path);
                if (segments.size() != route.segments.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < segments.size(); ++i) {
                    if (route.is_param[i]) {
                        if (segments[i].empty()) {
                            return false;
                        }
                        (*params)[route.segments[i]] = std::string(segments[i]);
                    } else if (segments[i] != route.segments[i]) {
                        return false;
                    }
                }
                return true;
            }
            case Kind::Wildcard: {
                if (route.prefix.empty()) {
                    (*params)["**"] = std::string(path);
                    return true;
                }
                if (path == route.prefix) {
                    (*params)["**"] = std::string();
                    return true;
                }
                if (path.size() > route.prefix.size() &&
                    path.compare(0, route.prefix.size(), route.prefix) == 0 &&
                    path[route.prefix.size()] == '/') {
                    (*params)["**"] = std::string(path.substr(route.prefix.size() + 1));
                    return true;
                }
                return false;
            }
            case Kind::Custom:
                return false;
        }
        return false;
    }

    // ---- 请求分发 ---------------------------------------------------------
    Response Dispatch(const Request& original) {
        Request request = original;
        Response response;
        requests.fetch_add(1, std::memory_order_relaxed);

        // 客户端传入的内部头一律失效，防止伪造鉴权标记/用户身份。
        request.header.erase(kAuthHeader);
        request.header.erase(kUserHeader);

        const std::string path = request.path();
        std::vector<Middleware> chain = SnapshotMiddlewares();

        std::function<Result<void>(Request&, Response&)> tail;
        if (request.method == http::Method::Get && path == kHealthPath) {
            tail = [](Request&, Response& output) {
                output = JsonResponse("{\"ok\":true,\"service\":\"gateway\"}");
                return Result<void>::Ok();
            };
        } else if (request.method == http::Method::Get && path == kHealthAllPath) {
            tail = [this](Request&, Response& output) {
                output = AggregateHealth();
                return Result<void>::Ok();
            };
        } else {
            RouteMatch match;
            if (!Match(request, &match)) {
                not_found.fetch_add(1, std::memory_order_relaxed);
                response = ErrorResponse(404, "route not found");
                ApplyDecorate(request, &response);
                return response;
            }
            if (match.entry->require_auth) {
                request.header.set(kAuthHeader, "1");
            }
            request.path_values = std::move(match.params);
            const auto handler = match.entry->handler;
            tail = [handler](Request& incoming, Response& output) {
                return (*handler)(incoming, output);
            };
        }

        const Result<void> result =
            InvokeChain(chain, 0, request, response, tail);
        if (!result.ok()) {
            response = ErrorResponse(500, result.error().empty() ? "gateway error"
                                                               : result.error());
        }
        const int status = static_cast<int>(response.status);
        if (status == 401) {
            unauthorized.fetch_add(1, std::memory_order_relaxed);
        } else if (status == 429) {
            rate_limited.fetch_add(1, std::memory_order_relaxed);
        }
        ApplyDecorate(request, &response);
        return response;
    }

    std::vector<Middleware> SnapshotMiddlewares() const {
        std::shared_lock<std::shared_mutex> lock(routes_mutex);
        return middlewares;
    }

    void ApplyDecorate(const Request& request, Response* response) {
        if (config.decorate) {
            config.decorate(request, response);
        }
        // framing 必须由序列化器决定；装饰器不得改写长度/分块头。
        response->header.erase("Content-Length");
        response->header.erase("Transfer-Encoding");
    }

    // ---- 转发 -------------------------------------------------------------
    /**
     * @brief 把一次请求转发到路由对应的上游，并把上游结果（或 502/504 错误响应）写回 response。
     * @param route [in] 命中的路由条目——借用，含默认 upstream/timeout；不可为 nullptr（引用）。
     * @param request [in,out] 入站请求——转发前被复制并改写 target/头，原始对象不被修改。
     * @param response [out] 出站响应——总是被覆盖：成功时为上游响应，失败时为 502/504 错误体。
     * @return
     * - Ok：总是返回 Ok——调用方**必须**检查 response.status 而不是返回值，不要把 502 当成功转发；
     * - 本函数不会返回 Err，上游故障已降级为网关错误响应。
     * ## 定位
     * 网关的转发终点：所有「代理」语义在这里落地。写错会泄漏 hop-by-hop 头、
     * 丢失 base_path 前缀，或把上游超时误报成 200。
     * ## 步骤
     * step1. 读锁下快照 resolver；存在则解析出目标 upstream，失败回 502；
     * step2. 校验 host/port，并按 route.timeout → 500ms 顺序兜底超时；
     * step3. 复制请求并 PrepareOutgoing（剥 hop-by-hop 头、拼接 base_path）；
     * step4. 用 http::Client 同步请求上游；
     * step5. 失败分类：Timeout 计 timeouts 回 504，其余计 upstream_errors 回 502；
     * step6. 成功则拷状态/体，并过滤上游 hop-by-hop 头后合并。
     * ## 并发/失败语义
     * - 调用约束：worker 线程调用，禁止持 routes_mutex；resolver 在锁内只做拷贝。
     * - 原子性：response 在返回前整体赋值；转发计数用 relaxed 原子累加。
     * - 失败语义：不抛异常；上游错误不传播为 Result::Err，保证连接仍能写出错误响应。
     * - 不变量：返回后 response.status 一定是最终可下发状态（2xx/4xx/5xx 之一）。
     */
    Result<void> Forward(const RouteEntry& route, Request& request, Response& response) {
        // step1: 先快照 resolver，避免网络调用期间一直占着路由读锁。
        Upstream upstream = route.upstream;
        Resolver resolver_snapshot;
        {
            std::shared_lock<std::shared_mutex> lock(routes_mutex);
            resolver_snapshot = resolver;
        }
        if (resolver_snapshot) {
            const Result<Upstream> resolved = resolver_snapshot(request);
            if (!resolved.ok()) {
                upstream_errors.fetch_add(1, std::memory_order_relaxed);
                response = ErrorResponse(
                    502, resolved.error().empty() ? "upstream resolve failed"
                                                  : resolved.error());
                return Result<void>::Ok();
            }
            upstream = resolved.value();
        }
        // step2: upstream 非法或超时缺失：按路由配置与 500ms 兜底，绝不发起无效连接。
        if (upstream.port == 0 || upstream.host.empty()) {
            upstream_errors.fetch_add(1, std::memory_order_relaxed);
            response = ErrorResponse(502, "invalid upstream");
            return Result<void>::Ok();
        }
        if (upstream.timeout.count() <= 0) {
            upstream.timeout = route.timeout;
        }
        if (upstream.timeout.count() <= 0) {
            upstream.timeout = std::chrono::milliseconds(500);
        }

        // step3: 复制后再改写，保证中间件/日志看到的仍是原始入站请求。
        Request outgoing = request;
        PrepareOutgoing(&outgoing, upstream);
        http::Client client(upstream.timeout);
        // step4: 同步阻塞转发；超时由 client 自己的 deadline 控制，不受 worker 队列影响。
        const http::Result result =
            client.do_request(outgoing, upstream.host, upstream.port);
        if (!result.ok() || !result.response) {
            // 上游失败被折叠成 502/504 返回给客户端，服务端日志必须保留原因。
            GO2CPP_LOG_WARN(GO2CPP_LOG_NAME("app"))
                << "gateway forward failed: upstream=" << upstream.host << ":"
                << upstream.port
                << " kind=" << (result.error == http::Result::Error::Timeout
                                    ? "timeout"
                                    : "upstream_error")
                << " detail=" << result.message;
            if (result.error == http::Result::Error::Timeout) {
                timeouts.fetch_add(1, std::memory_order_relaxed);
                response = ErrorResponse(504, "upstream timeout");
            } else {
                upstream_errors.fetch_add(1, std::memory_order_relaxed);
                response = ErrorResponse(
                    502, result.message.empty() ? "upstream unavailable"
                                                : ("upstream unavailable: " + result.message));
            }
            return Result<void>::Ok();
        }
        // step6: 成功路径：状态/体原样透传，头逐条过滤 hop-by-hop 后合并。
        forwarded.fetch_add(1, std::memory_order_relaxed);
        response.status = result.response->status;
        response.version = "HTTP/1.1";
        response.body = result.response->body;
        for (const auto& entry : result.response->header.entries()) {
            if (IsHopByHop(entry.first)) {
                continue;
            }
            response.header.add(entry.first, entry.second);
        }
        return Result<void>::Ok();
    }

    static void PrepareOutgoing(Request* request, const Upstream& upstream) {
        request->header.erase("Host");
        request->header.erase("Connection");
        request->header.erase("Content-Length");
        request->header.erase("Transfer-Encoding");
        request->header.erase("TE");
        request->header.erase("Trailer");
        request->header.erase("Upgrade");
        request->header.erase("Keep-Alive");
        request->header.erase("Proxy-Connection");
        request->header.erase("Proxy-Authenticate");
        request->header.erase("Proxy-Authorization");
        request->header.erase(kAuthHeader);
        // G4：100-continue 已由网关预应答，转发时摘掉 Expect，避免上游再发一次
        // 100 被 http::Client 当成最终响应。
        request->header.erase("Expect");
        if (!upstream.base_path.empty() && upstream.base_path != "/") {
            std::string prefix = upstream.base_path;
            if (prefix.back() == '/') {
                prefix.pop_back();
            }
            std::string target = request->target;
            if (target.empty() || target.front() != '/') {
                target = "/" + target;
            }
            request->target = prefix + target;
        }
    }

    // ---- 聚合健康 ---------------------------------------------------------
    /**
     * @brief 聚合所有路由上游的 /health 探测结果，生成 `{ok, services}` JSON 响应。
     * @return 200 的 JSON 响应——body 中 ok 表示全部上游健康，services 为每个 upstream 文本到结果的映射。
     * ## 定位
     * 健康检查端点（/health）的唯一实现：串行探测所有去重后的上游。
     * 写错会让就绪探针误判（把不可达当健康，或把 4xx 当故障），触发错误的摘流/重启。
     * ## 步骤
     * step1. 读锁下遍历路由，跳过 Custom/空 upstream，按文本去重；
     * step2. 累加 health_checks，health_timeout 缺省 300ms；
     * step3. 逐个拼 `http://host:port/health` 并发起 GET（IPv6 字面量补方括号）；
     * step4. 只有 200 才算健康，其余记录错误或状态码；
     * step5. 拼装服务映射与总 ok，JsonResponse 返回。
     * ## 并发/失败语义
     * - 调用约束：可在任意连接线程调用；探测是同步串行，上游数量多时会拉长响应时间。
     * - 原子性：路由列表在读锁内一次性拷贝，探测期间新增路由不影响本次结果。
     * - 失败语义：不抛异常；单个上游失败只影响其条目与总 ok，不会中断整体聚合。
     * - 不变量：返回的 JSON 一定可被解析，services 的键为原始 upstream 文本。
     */
    Response AggregateHealth() {
        // step1: 读锁下收集去重后的 upstream 文本，锁外再做网络探测。
        std::vector<std::string> upstreams;
        {
            std::shared_lock<std::shared_mutex> lock(routes_mutex);
            std::set<std::string> seen;
            for (const auto& route : routes) {
                if (route->kind == Kind::Custom || route->upstream_text.empty()) {
                    continue;
                }
                if (seen.insert(route->upstream_text).second) {
                    upstreams.push_back(route->upstream_text);
                }
            }
        }
        // step2: 计一次健康检查；timeout 非正时用 300ms，避免永久阻塞探针。
        health_checks.fetch_add(1, std::memory_order_relaxed);
        std::chrono::milliseconds timeout = config.health_timeout;
        if (timeout.count() <= 0) {
            timeout = std::chrono::milliseconds(300);
        }
        bool all_ok = true;
        std::string services = "{";
        bool first = true;
        for (const auto& text : upstreams) {
            Upstream upstream;
            bool healthy = false;
            std::string error;
            if (!ParseUpstreamText(text, &upstream)) {
                error = "invalid upstream";
            } else {
                std::string host = upstream.host;
                if (host.find(':') != std::string::npos && host.front() != '[') {
                    host = "[" + host + "]";
                }
                const std::string url =
                    "http://" + host + ":" + std::to_string(upstream.port) + "/health";
                http::Client client(timeout);
                const http::Result result = client.get(url);
                if (!result.ok() || !result.response) {
                    error = result.message.empty() ? "unreachable" : result.message;
                } else if (static_cast<int>(result.response->status) != 200) {
                    error = "status " +
                            std::to_string(static_cast<int>(result.response->status));
                } else {
                    healthy = true;
                }
            }
            if (!healthy) {
                all_ok = false;
            }
            if (!first) {
                services += ",";
            }
            first = false;
            services += "\"" + JsonEscape(text) + "\":{\"ok\":" +
                        (healthy ? "true" : "false");
            if (healthy) {
                services += ",\"status\":200";
            } else {
                services += ",\"error\":\"" + JsonEscape(error) + "\"";
            }
            services += "}";
        }
        services += "}";
        return JsonResponse(std::string("{\"ok\":") + (all_ok ? "true" : "false") +
                            ",\"services\":" + services + "}");
    }

    // ---- 生命周期 ---------------------------------------------------------
    void StartWorkers() {
        std::size_t count = config.worker_threads;
        if (count == 0) {
            count = 4;
        }
        workers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            workers.emplace_back([this]() { WorkerLoop(); });
        }
    }

    void WorkerLoop() {
        for (;;) {
            net::Socket::Ptr connection;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [this]() { return queue_closed || !queue.empty(); });
                if (queue.empty()) {
                    return;
                }
                connection = std::move(queue.front());
                queue.pop_front();
            }
            if (connection) {
                // 兜底：ServeConnection 已把 Dispatch 的异常转成 500；这里防的是
                // 序列化/写回等边缘路径漏出的异常，绝不允许穿过 worker 线程触发 terminate。
                try {
                    ServeConnection(connection);
                } catch (const std::exception& error) {
                    internal_errors.fetch_add(1, std::memory_order_relaxed);
                    GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("app"))
                        << "gateway connection handler threw: " << error.what();
                    WriteAll(connection,
                             "HTTP/1.1 500 Internal Server Error\r\n"
                             "Content-Length: 0\r\nConnection: close\r\n\r\n");
                } catch (...) {
                    internal_errors.fetch_add(1, std::memory_order_relaxed);
                    GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("app"))
                        << "gateway connection handler threw unknown exception";
                    WriteAll(connection,
                             "HTTP/1.1 500 Internal Server Error\r\n"
                             "Content-Length: 0\r\nConnection: close\r\n\r\n");
                }
                connection->close();
            }
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                active.erase(connection.get());
            }
        }
    }

    /// @brief 单连接循环：逐个处理已到达请求（含同包流水线）→ Dispatch → 写回。
    /**
     * @brief 在一条已 accept 的连接上按序处理所有已到达的请求；每个请求带读取总时限，Dispatch 异常兜底为 500。
     * @param connection [in] 已就绪的连接——借用，生命周期由 worker 持有；不可为 nullptr；函数返回后由调用方 close。
     *
     * @return 无返回值；任何 I/O、解析或总时限失败都直接 return（连接留给调用方关闭），不向调用方抛异常。
     *
     * ## 定位
     * 网关连接处理的唯一入口（WorkerLoop → ServeConnection）：请求解析、Dispatch、序列化、写回全在这里。
     * 写错会直接造成生产事故：① handler 异常穿过 worker 线程 → std::terminate 进程崩溃；
     * ② 单次 recv 超时被慢连接无限续期 → 少量半开连接占满 worker，全站拒绝服务。
     *
     * ## 步骤
     * step1. 为本请求建立 `request_timeout` 总 deadline（第一次 recv 前起算），
     *        每次 recv 只等「剩余时间」与 30s 的较小值，到期立即关闭；
     * step2. 头部先攒到 `\r\n\r\n` 再交给 Parser；`Expect: 100-continue` 且有 body
     *        时先回 100 再读 body；NeedMore 继续读，Error 回 400 后关闭；
     *        首个请求允许阻塞等数据，后续流水线请求只消费已缓冲字节（残包仍可续读）；
     * step3. 同一 recv 的剩余字节交给下一条请求（`take_remaining()`），不再丢弃；
     * step4. Dispatch 外包 try/catch：handler/中间件任何异常 → 500 + close + 计数；
     * step5. 按方法序列化（HEAD 去体），`Connection` 头与实际是否关闭保持一致；
     * step6. 写回；要求关闭或流水线已耗尽则返回，否则继续处理下一条。
     *
     * ## 并发/失败语义
     * - 调用约束：每个连接只由一个 worker 线程处理；parser/buffer 都是循环内局部对象，天然线程隔离。
     * - 原子性：响应在内存中完整构造后才写 socket，不会写出半个响应。
     * - 失败语义：不抛异常；I/O/解析失败静默返回，Dispatch 异常转 500；调用方统一 close 并回收 active 表项。
     * - 不变量：返回时该连接要么已写完整响应且确定不可继续，要么已无缓冲请求。
     */
    void ServeConnection(const net::Socket::Ptr& connection) {
        // step1: 缓冲跨请求复用；pending 是已收到但尚未解析的流水线字节。
        constexpr std::size_t kMaxRequestsPerConnection = 100;
        constexpr auto kMaxRecvSlice = std::chrono::milliseconds(30000);
        std::array<char, 8192> buffer{};
        std::string pending;

        for (std::size_t served = 0; served < kMaxRequestsPerConnection; ++served) {
            const auto deadline =
                std::chrono::steady_clock::now() + config.request_timeout;
            http::Parser parser;
            http::Request request;
            http::ParseResult parsed;
            // 首个请求允许等 socket 新数据；后续请求只有已缓冲字节时才继续读，
            // 防止空闲 keep-alive 连接把 worker 占在 recv 上。
            bool allow_recv = served == 0 || !pending.empty();
            // G4：头部先在 head 里攒齐再交给 Parser，这样才能赶在 body 之前判定
            // 是否需要回 100 Continue；已缓冲的流水线字节直接作为初值。
            std::string head = std::move(pending);
            pending.clear();
            bool header_done = false;
            for (;;) {
                if (!head.empty() && !header_done) {
                    const std::size_t header_end = head.find("\r\n\r\n");
                    // 头部收齐（或超限）后才进入解析；超限交给 Parser 判 Error。
                    if (header_end != std::string::npos ||
                        head.size() > parser.limits().max_header_bytes) {
                        header_done = true;
                        if (header_end != std::string::npos &&
                            ShouldSendContinue(head, header_end)) {
                            if (!WriteAll(connection,
                                          "HTTP/1.1 100 Continue\r\n\r\n")) {
                                return;
                            }
                        }
                    }
                }
                if (header_done) {
                    if (!head.empty()) {
                        parsed = parser.feed(head, request);
                        head.clear();
                        allow_recv = true;
                    }
                    if (parsed.state == http::ParseState::Error) {
                        // G7：畸形请求必须回 400 再关；静默断连会让客户端只看到
                        // reset，无法区分「网关拒绝」与「网络故障」。
                        Response bad = ErrorResponse(
                            400, parsed.error.empty() ? "bad request" : parsed.error);
                        bad.close = true;
                        WriteAll(connection, SerializeResponse(request.method, bad));
                        return;
                    }
                    if (parsed.state == http::ParseState::Complete) {
                        break;
                    }
                }
                if (!allow_recv) {
                    return;
                }
                // step1: 每次 recv 只看总 deadline 的剩余时间，慢连接无法靠
                // 「每次多等 30s」续期；到点直接放弃该连接。
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    request_timeouts.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - now);
                const auto slice = std::min(remaining, kMaxRecvSlice);
                const auto count = connection->recv(
                    buffer.data(), buffer.size(), 0,
                    slice.count() > 0 ? slice : std::chrono::milliseconds(1));
                if (!count.ok() || count.transferred <= 0) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        request_timeouts.fetch_add(1, std::memory_order_relaxed);
                    }
                    return;
                }
                head.append(buffer.data(),
                            static_cast<std::size_t>(count.transferred));
            }

            // step3: 同一 recv 里可能已带着下一条请求，留给下一轮而不是丢弃。
            pending = parser.take_remaining();

            // step4: Dispatch 兜底：自定义 handler/中间件 throw 不能穿出 worker 线程。
            Response response;
            try {
                response = Dispatch(request);
            } catch (const std::exception& error) {
                internal_errors.fetch_add(1, std::memory_order_relaxed);
                GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("app"))
                    << "gateway handler threw: " << error.what();
                response = ErrorResponse(500, "internal server error");
                response.close = true;
            } catch (...) {
                internal_errors.fetch_add(1, std::memory_order_relaxed);
                GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("app"))
                    << "gateway handler threw unknown exception";
                response = ErrorResponse(500, "internal server error");
                response.close = true;
            }

            // step5: 只有缓冲里确实还有下一条请求才保持连接，否则关闭；
            // 这样既服务了流水线，又不为「等下一次请求」白占 worker。
            const bool close_after = request.close || response.close ||
                                     pending.empty() ||
                                     served + 1 >= kMaxRequestsPerConnection;
            response.close = close_after;
            const std::string wire = SerializeResponse(request.method, response);

            // step6: 写失败说明对端已断；正常写完且需关闭则结束，否则继续流水线。
            if (!WriteAll(connection, wire)) {
                return;
            }
            if (close_after) {
                return;
            }
        }
    }

    void Stop() {
        if (stopping.exchange(true)) {
            return;
        }
        if (listener) {
            listener->close();
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue_closed = true;
            for (auto& entry : active) {
                entry.second->shutdown(SHUT_RDWR);
            }
        }
        queue_cv.notify_all();
    }

    // ---- 数据成员 ---------------------------------------------------------
    Config config;
    net::Socket::Ptr listener;

    mutable std::shared_mutex routes_mutex;
    std::vector<std::shared_ptr<RouteEntry>> routes;
    std::vector<Middleware> middlewares;
    Resolver resolver;
    std::uint64_t next_order{0};

    std::vector<std::thread> workers;
    std::deque<net::Socket::Ptr> queue;
    std::unordered_map<net::Socket*, net::Socket::Ptr> active;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    bool queue_closed{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};

    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> forwarded{0};
    std::atomic<std::uint64_t> unauthorized{0};
    std::atomic<std::uint64_t> rate_limited{0};
    std::atomic<std::uint64_t> not_found{0};
    std::atomic<std::uint64_t> upstream_errors{0};
    std::atomic<std::uint64_t> timeouts{0};
    std::atomic<std::uint64_t> health_checks{0};
    std::atomic<std::uint64_t> request_timeouts{0};
    std::atomic<std::uint64_t> internal_errors{0};
};

// ---- 中间件实现 -----------------------------------------------------------

Middleware AuthMiddleware(AuthFn auth) {
    return [auth = std::move(auth)](Request& request, Response& response,
                                    const Next& next) -> Result<void> {
        if (request.header.get(kAuthHeader) != "1") {
            return next();
        }
        if (!auth) {
            response = ErrorResponse(401, "auth is not configured");
            return Result<void>::Ok();
        }
        std::string user_id;
        std::string error;
        if (!auth(request, &user_id, &error)) {
            const std::string message = error.empty() ? "unauthorized" : error;
            if (message.front() == '{') {
                response = ErrorResponse(401, "unauthorized");
                response.body = message;
            } else {
                response = ErrorResponse(401, message);
            }
            return Result<void>::Ok();
        }
        request.header.erase(kUserHeader);
        if (!user_id.empty()) {
            request.header.set(kUserHeader, user_id);
        }
        return next();
    };
}

Middleware RateLimitMiddleware(RateLimitFn limiter) {
    return [limiter = std::move(limiter)](Request& request, Response& response,
                                          const Next& next) -> Result<void> {
        if (limiter && !limiter(request)) {
            response = ErrorResponse(429, "rate limited");
            return Result<void>::Ok();
        }
        return next();
    };
}

Middleware AccessLogMiddleware(AccessLogFn sink) {
    return [sink = std::move(sink)](Request& request, Response& response,
                                    const Next& next) -> Result<void> {
        const auto started = std::chrono::steady_clock::now();
        const Result<void> result = next();
        const auto cost = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started);
        if (sink) {
            sink(request, response, cost);
        } else {
            std::fprintf(stderr, "[gateway] %s %s -> %d (%lld us)\n",
                         http::method_name(request.method).c_str(),
                         request.target.c_str(), static_cast<int>(response.status),
                         static_cast<long long>(cost.count()));
        }
        return result;
    };
}

// ---- Gateway 外观 ---------------------------------------------------------

Gateway::Gateway(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
Gateway::~Gateway() = default;

Result<std::unique_ptr<Gateway>> Gateway::Create(Config config) {
    if (config.worker_threads == 0) {
        config.worker_threads = 4;
    }
    if (config.max_queued_connections == 0) {
        config.max_queued_connections = 256;
    }
    if (config.accept_timeout.count() <= 0) {
        config.accept_timeout = std::chrono::milliseconds(100);
    }
    if (config.request_timeout.count() <= 0) {
        config.request_timeout = std::chrono::milliseconds(10000);
    }
    const std::string host =
        config.listen_host.empty() ? std::string("0.0.0.0") : config.listen_host;
    net::Socket::Ptr listener = net::ListenTCP(host, config.port);
    if (!listener) {
        return Result<std::unique_ptr<Gateway>>::Err(
            "listen failed on " + host + ":" + std::to_string(config.port));
    }
    auto impl = std::make_unique<Impl>(std::move(config));
    impl->listener = listener;

    // 配置注入的内置中间件按「日志(最外) → 限流 → 鉴权 → 业务」的顺序安装。
    if (impl->config.access_log) {
        impl->AddMiddleware(AccessLogMiddleware());
    }
    if (impl->config.rate_limit) {
        impl->AddMiddleware(RateLimitMiddleware(impl->config.rate_limit));
    }
    if (impl->config.auth) {
        impl->AddMiddleware(AuthMiddleware(impl->config.auth));
    }
    if (impl->config.resolver) {
        impl->SetResolver(impl->config.resolver);
    }
    return Result<std::unique_ptr<Gateway>>::Ok(
        std::unique_ptr<Gateway>(new Gateway(std::move(impl))));
}

Result<void> Gateway::AddRoute(RouteSpec spec) {
    return m_impl->AddRoute(std::move(spec));
}

Result<void> Gateway::AddRoute(std::shared_ptr<Matcher> matcher,
                               std::shared_ptr<Handler> handler) {
    return m_impl->AddCustomRoute(std::move(matcher), std::move(handler));
}

void Gateway::Use(Middleware middleware) { m_impl->AddMiddleware(std::move(middleware)); }

void Gateway::SetResolver(Resolver resolver) { m_impl->SetResolver(std::move(resolver)); }

/**
 * @brief 从 JSON 文本加载并逐条注册路由；任一条非法则整体失败（已注册的前缀保留）。
 * @param json [in] 配置文本——借用，仅本函数调用期间有效；不可为空，支持 `{"routes":[...]}` 或直接数组。
 * @return
 * - Ok：全部路由已注册——调用方**必须**据此认为配置生效；
 * - Err：JSON 语法错误或首个非法条目的带下标原因——调用方**必须**把 error 原样上报，
 *   并注意此前已成功的条目已经生效（本函数不是事务性的）。
 * ## 定位
 * 启动期配置唯一入口（LoadConfigFile → LoadConfig）。写错会静默丢掉路由或把合法配置判死，
 * 直接影响网关能否转发；由于非事务，错误发生点必须在 error 文本中精确到 routes[i]。
 * ## 步骤
 * step1. 解析 JSON 根，语法错误直接返回；
 * step2. 定位 routes 数组：缺省时允许根本身就是数组；
 * step3. 逐条校验类型与必填字段（path/upstream），method 支持字符串/数组/通配符 *；
 * step4. 可选字段校验：require_auth 布尔、timeout_ms 正数、priority 数字、name 字符串；
 * step5. 通过后 AddRoute 注册，失败时把下标与原因拼进 error 返回。
 * ## 并发/失败语义
 * - 调用约束：通常在服务启动前由控制线程调用；运行期调用与请求处理并发是安全的（AddRoute 加写锁）。
 * - 原子性：单条路由注册原子；整体加载非事务，失败时保留前面已注册的条目。
 * - 失败语义：不抛异常，校验错误以 Err(std::string) 返回，对象状态保持可用。
 * - 不变量：返回 Ok 时 routes 中每条都已通过字段与路由表校验。
 */
Result<void> Gateway::LoadConfig(std::string_view json) {
    JsonValue root;
    std::string error;
    JsonParser parser(json);
    if (!parser.Parse(&root, &error)) {
        return Result<void>::Err("invalid json: " + error);
    }
    const JsonValue* routes = root.Find("routes");
    if (routes == nullptr) {
        if (root.type == JsonValue::Type::Array) {
            routes = &root;
        } else {
            return Result<void>::Err("config: missing 'routes' array");
        }
    }
    if (routes->type != JsonValue::Type::Array) {
        return Result<void>::Err("config: 'routes' must be an array");
    }
    // step3: 逐条校验必填字段；下标写进前缀，出错时调用方一眼定位到具体条目。
    for (std::size_t index = 0; index < routes->items.size(); ++index) {
        const JsonValue& item = routes->items[index];
        const std::string prefix = "routes[" + std::to_string(index) + "]: ";
        if (item.type != JsonValue::Type::Object) {
            return Result<void>::Err(prefix + "must be an object");
        }
        RouteSpec spec;
        const JsonValue* path = item.Find("path");
        if (path == nullptr) {
            path = item.Find("path_pattern");
        }
        if (path == nullptr || path->type != JsonValue::Type::String) {
            return Result<void>::Err(prefix + "missing string field 'path'");
        }
        spec.path_pattern = path->text;
        const JsonValue* upstream = item.Find("upstream");
        if (upstream == nullptr || upstream->type != JsonValue::Type::String) {
            return Result<void>::Err(prefix + "missing string field 'upstream'");
        }
        spec.upstream = upstream->text;
        const JsonValue* method = item.Find("method");
        if (method != nullptr) {
            if (method->type == JsonValue::Type::String) {
                if (method->text == "*") {
                    spec.method_mask = method_mask::kAll;
                } else {
                    const http::Method parsed = http::parse_method(method->text);
                    if (parsed == http::Method::Unknown) {
                        return Result<void>::Err(prefix + "unknown method: " + method->text);
                    }
                    spec.method_mask = method_mask::Of(parsed);
                }
            } else if (method->type == JsonValue::Type::Array) {
                spec.method_mask = method_mask::kNone;
                for (const auto& entry : method->items) {
                    if (entry.type != JsonValue::Type::String) {
                        return Result<void>::Err(prefix + "method array must contain strings");
                    }
                    const http::Method parsed = http::parse_method(entry.text);
                    if (parsed == http::Method::Unknown) {
                        return Result<void>::Err(prefix + "unknown method: " + entry.text);
                    }
                    spec.method_mask |= method_mask::Of(parsed);
                }
            } else {
                return Result<void>::Err(prefix + "'method' must be string or array");
            }
        }
        const JsonValue* require_auth = item.Find("require_auth");
        if (require_auth != nullptr) {
            if (require_auth->type != JsonValue::Type::Bool) {
                return Result<void>::Err(prefix + "'require_auth' must be boolean");
            }
            spec.require_auth = require_auth->boolean;
        }
        const JsonValue* timeout = item.Find("timeout_ms");
        if (timeout != nullptr) {
            if (timeout->type != JsonValue::Type::Number || timeout->number <= 0) {
                return Result<void>::Err(prefix + "'timeout_ms' must be a positive number");
            }
            spec.timeout = std::chrono::milliseconds(
                static_cast<std::int64_t>(timeout->number));
        }
        const JsonValue* priority = item.Find("priority");
        if (priority != nullptr) {
            if (priority->type != JsonValue::Type::Number) {
                return Result<void>::Err(prefix + "'priority' must be a number");
            }
            spec.priority = static_cast<int>(priority->number);
        }
        const JsonValue* name = item.Find("name");
        if (name != nullptr && name->type == JsonValue::Type::String) {
            spec.name = name->text;
        }
        // step5: 单条字段全部通过后才进路由表；失败只返回，不回滚已注册条目（非事务语义）。
        const Result<void> added = AddRoute(std::move(spec));
        if (!added.ok()) {
            return Result<void>::Err(prefix + added.error());
        }
    }
    return Result<void>::Ok();
}

Result<void> Gateway::LoadConfigFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<void>::Err("cannot open config file: " + path);
    }
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    return LoadConfig(content);
}

void Gateway::Run() {
    Impl& impl = *m_impl;
    bool expected = false;
    if (!impl.running.compare_exchange_strong(expected, true)) {
        return;
    }
    impl.StartWorkers();
    while (!impl.stopping.load(std::memory_order_acquire)) {
        net::Socket::Ptr connection = impl.listener->accept(impl.config.accept_timeout);
        if (!connection) {
            continue;
        }
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(impl.queue_mutex);
            if (!impl.queue_closed &&
                impl.queue.size() < impl.config.max_queued_connections) {
                impl.active[connection.get()] = connection;
                impl.queue.push_back(connection);
                accepted = true;
                impl.queue_cv.notify_one();
            }
        }
        if (!accepted) {
            // G6 背压：队列已满时先回 503 + Connection: close 再关，让客户端
            // 看到明确的「服务暂时不可用」，而不是被静默重置连接。
            Response busy = ErrorResponse(503, "server busy");
            busy.close = true;
            WriteAll(connection, SerializeResponse(http::Method::Get, busy));
            connection->close();
        }
    }
    impl.Stop();
    for (auto& worker : impl.workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    impl.workers.clear();
    impl.running.store(false, std::memory_order_release);
}

void Gateway::Stop() { m_impl->Stop(); }

Stats Gateway::stats() const {
    Stats snapshot;
    snapshot.requests = m_impl->requests.load(std::memory_order_relaxed);
    snapshot.forwarded = m_impl->forwarded.load(std::memory_order_relaxed);
    snapshot.unauthorized = m_impl->unauthorized.load(std::memory_order_relaxed);
    snapshot.rate_limited = m_impl->rate_limited.load(std::memory_order_relaxed);
    snapshot.not_found = m_impl->not_found.load(std::memory_order_relaxed);
    snapshot.upstream_errors = m_impl->upstream_errors.load(std::memory_order_relaxed);
    snapshot.timeouts = m_impl->timeouts.load(std::memory_order_relaxed);
    snapshot.health_checks = m_impl->health_checks.load(std::memory_order_relaxed);
    snapshot.request_timeouts =
        m_impl->request_timeouts.load(std::memory_order_relaxed);
    snapshot.internal_errors = m_impl->internal_errors.load(std::memory_order_relaxed);
    return snapshot;
}

std::uint16_t Gateway::port() const noexcept {
    if (!m_impl->listener) {
        return 0;
    }
    const net::AddressPtr address = m_impl->listener->local_address();
    const auto ip = std::dynamic_pointer_cast<net::IPAddress>(address);
    return ip ? ip->port() : 0;
}

bool Gateway::running() const noexcept {
    return m_impl->running.load(std::memory_order_acquire);
}

}  // namespace go2cpp::app::gateway
