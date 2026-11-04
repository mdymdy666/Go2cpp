/**
 * @file douyin_http.hpp
 * @brief 骨架用的最小 HTTP/1.1 服务器：基于 go2cpp::net::Socket，无第三方库。
 *
 * @details 支持「读完整请求（请求行 + 头 + 按 Content-Length 的 body）→ 写一个
 *          响应 → 关闭连接」。请求头键名统一转小写，便于查 Authorization。
 *          业务实现阶段应替换为 go2cpp::bento::HttpServer。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#pragma once

#include "go2cpp/network/socket.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <sys/socket.h>

namespace douyin {

/// @brief 解析后的请求（请求行 + 头 + body）。
struct HttpRequest {
    std::string method;              ///< 方法（GET/POST/...）。
    std::string target;              ///< 原始目标（含 query）。
    std::string path;                ///< 不含 query 的路径。
    std::string query;               ///< `?` 之后的原始 query（不含 `?`）。
    std::string version;             ///< HTTP 版本。
    std::map<std::string, std::string> headers;  ///< 头，键为小写。
    std::string body;                ///< 请求体。

    /// @brief 取请求头（键需小写）；不存在返回空串。
    std::string Header(std::string_view name) const {
        std::string key(name);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto it = headers.find(key);
        return it == headers.end() ? std::string() : it->second;
    }
};

/// @brief 待发送的响应。
struct HttpResponse {
    int status{200};
    std::string content_type{"application/json; charset=utf-8"};
    std::string body;
};

/// @brief 命中路由返回 true 并填好 response；返回 false 由服务器回 404。
using HttpHandler = std::function<bool(const HttpRequest&, HttpResponse*)>;

/// @brief 单次请求的字节上限（头 64 KiB + body 32 MiB，容纳视频上传）。
inline constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
inline constexpr std::size_t kMaxBodyBytes = 32 * 1024 * 1024;

/// @brief 状态码 → reason phrase。
inline const char* StatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 504: return "Gateway Timeout";
        default: return "OK";
    }
}

/// @brief 序列化成完整 HTTP 响应字节流。
inline std::string Serialize(const HttpResponse& response) {
    std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " +
                       StatusText(response.status) + "\r\n";
    head += "Content-Type: " + response.content_type + "\r\n";
    head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";
    return head + response.body;
}

/// @brief 从缓冲区首行解析 method / target / version。
inline bool ParseRequestLine(const std::string& buffer, HttpRequest* request) {
    const std::size_t line_end = buffer.find('\n');
    if (line_end == std::string::npos) {
        return false;
    }
    std::string line = buffer.substr(0, line_end);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const std::size_t first = line.find(' ');
    if (first == std::string::npos) {
        return false;
    }
    const std::size_t second = line.find(' ', first + 1);
    if (second == std::string::npos) {
        return false;
    }
    request->method = line.substr(0, first);
    request->target = line.substr(first + 1, second - first - 1);
    request->version = line.substr(second + 1);

    const std::size_t question = request->target.find('?');
    if (question == std::string::npos) {
        request->path = request->target;
        request->query.clear();
    } else {
        request->path = request->target.substr(0, question);
        request->query = request->target.substr(question + 1);
    }
    return !request->method.empty() && !request->target.empty();
}

/// @brief 去掉首尾空白（空格 / 制表 / CR）。
inline std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

/**
 * @brief 解析请求头。
 * @param block 请求行之后、空行之前的文本（含各行行尾）。
 * @param request [out] 输出；键统一转小写，重复头以后出现者为准。
 * @return 头合法返回 true；重复的 `Content-Length` 取值不一致返回 false。
 *         重复且同值的 `Content-Length` 合法（RFC 7230 允许合并同值）。
 */
inline bool ParseHeaders(std::string_view block, HttpRequest* request) {
    std::size_t pos = 0;
    while (pos < block.size()) {
        std::size_t line_end = block.find('\n', pos);
        if (line_end == std::string_view::npos) {
            line_end = block.size();
        }
        const std::string line = Trim(block.substr(pos, line_end - pos));
        pos = line_end + 1;
        if (line.empty()) {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = Trim(std::string_view(line).substr(0, colon));
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string value = Trim(std::string_view(line).substr(colon + 1));
        // 两个 Content-Length 指向不同长度时无法判断以哪个为准，按 RFC 7230
        // 视为消息定界错误直接拒绝，避免请求走私/截断。
        if (key == "content-length") {
            const auto existing = request->headers.find(key);
            if (existing != request->headers.end() && existing->second != value) {
                return false;
            }
        }
        request->headers[key] = value;
    }
    return true;
}

/// @brief 查请求头结束位置（`\r\n\r\n` 或 `\n\n`），返回 body 起始偏移。
inline bool FindHeaderEnd(const std::string& buffer, std::size_t* body_begin) {
    const std::size_t crlf = buffer.find("\r\n\r\n");
    if (crlf != std::string::npos) {
        *body_begin = crlf + 4;
        return true;
    }
    const std::size_t lf = buffer.find("\n\n");
    if (lf != std::string::npos) {
        *body_begin = lf + 2;
        return true;
    }
    return false;
}

/**
 * @brief 解析 Content-Length：只接受非负十进制整数。
 * @param text 头部原值（首尾空白已由 ParseHeaders 去掉）。
 * @param out [out] 成功时写入字节数；失败时保持原值。
 * @return 合法返回 true；空串、含非数字字符或超出 size_t 返回 false。
 */
inline bool ParseContentLength(std::string_view text, std::size_t* out) {
    if (text.empty() || out == nullptr) {
        return false;
    }
    std::size_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const auto digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

/**
 * @brief 只解析请求行与请求头（不解析 body）。
 * @param buffer 已读字节，至少包含完整头。
 * @param body_begin `FindHeaderEnd` 得到的 body 起始偏移。
 * @param request [out] 输出；重复且冲突的 Content-Length 会让本函数返回 false。
 * @return 请求行与头合法返回 true。
 */
inline bool ParseRequestHead(const std::string& buffer, std::size_t body_begin,
                             HttpRequest* request) {
    if (!ParseRequestLine(buffer, request)) {
        return false;
    }
    const std::size_t header_line_end = buffer.find('\n');
    if (header_line_end == std::string::npos || header_line_end + 1 > body_begin) {
        return true;
    }
    return ParseHeaders(std::string_view(buffer).substr(header_line_end + 1,
                                                        body_begin - header_line_end - 1),
                        request);
}

/**
 * @brief 从已读缓冲区解析完整请求（请求行 + 头 + body）。
 * @param buffer 已读字节（至少含完整头；body 长度已由调用方保证）。
 * @param body_begin `FindHeaderEnd` 得到的 body 起始偏移。
 * @return 请求行与头合法返回 true。
 */
inline bool ParseHttpRequest(const std::string& buffer, std::size_t body_begin,
                             HttpRequest* request) {
    if (!ParseRequestHead(buffer, body_begin, request)) {
        return false;
    }
    request->body = buffer.substr(body_begin);
    return true;
}

/// @brief 取 `Authorization: Bearer <token>` 的 token；缺失/格式不对返回空串。
inline std::string BearerToken(const HttpRequest& request) {
    const std::string value = request.Header("authorization");
    constexpr std::string_view kPrefix = "Bearer ";
    if (value.size() <= kPrefix.size()) {
        return std::string();
    }
    if (value.compare(0, kPrefix.size(), kPrefix.data(), kPrefix.size()) != 0) {
        return std::string();
    }
    return Trim(std::string_view(value).substr(kPrefix.size()));
}

/// @brief 取 query 参数；不存在返回空串（不做 URL 解码）。
inline std::string QueryParam(const HttpRequest& request, std::string_view name) {
    std::size_t pos = 0;
    while (pos <= request.query.size()) {
        std::size_t end = request.query.find('&', pos);
        if (end == std::string::npos) {
            end = request.query.size();
        }
        const std::string_view pair(request.query.data() + pos, end - pos);
        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == name) {
            return std::string(pair.substr(eq + 1));
        }
        pos = end + 1;
    }
    return std::string();
}

/// @brief 转义 JSON 字符串内容（引号 / 反斜杠 / 控制字符）。
inline std::string JsonEscape(std::string_view text) {
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
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

/// @brief 统一错误响应体：`{"ok":false,"error":"...","code":N}`。
inline std::string ErrorBody(int status, std::string_view message) {
    return "{\"ok\":false,\"error\":\"" + JsonEscape(message) +
           "\",\"code\":" + std::to_string(status) + "}";
}

/// @brief 从 JSON 文本里取字符串字段（只支持本项目固定结构，解析失败返回 false）。
inline bool JsonStringField(std::string_view body, std::string_view key, std::string* out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t pos = body.find(needle);
    if (pos == std::string_view::npos) {
        return false;
    }
    pos = body.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return false;
    }
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) {
        ++pos;
    }
    if (pos >= body.size() || body[pos] != '"') {
        return false;
    }
    ++pos;
    std::string value;
    while (pos < body.size()) {
        const char c = body[pos++];
        if (c == '"') {
            *out = std::move(value);
            return true;
        }
        if (c == '\\' && pos < body.size()) {
            const char escaped = body[pos++];
            switch (escaped) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                case '/': value += '/'; break;
                default: value += escaped; break;
            }
            continue;
        }
        value += c;
    }
    return false;
}

/**
 * @brief 读取请求头直到空行出现。
 * @param connection [in,out] 已 accept 的连接；只读不写。
 * @param buffer [out] 累积读到的字节（可能已含部分 body）。
 * @param body_begin [out] 空行之后的 body 起始偏移；仅在返回 true 时有效。
 * @param too_large [in,out] 调用方初始化为 false；头超过 kMaxHeaderBytes 时置 true。
 * @return 头完整返回 true；连接提前关闭返回 false。
 */
inline bool ReadRequestHead(go2cpp::net::Socket& connection, std::string* buffer,
                            std::size_t* body_begin, bool* too_large) {
    char chunk[64 * 1024];
    for (;;) {
        if (buffer->size() >= kMaxHeaderBytes) {
            *too_large = true;
            return false;
        }
        const auto result =
            connection.recv(chunk, sizeof(chunk), 0, std::chrono::milliseconds(2000));
        if (!result.ok() || result.transferred <= 0) {
            return false;
        }
        buffer->append(chunk, static_cast<std::size_t>(result.transferred));
        if (FindHeaderEnd(*buffer, body_begin)) {
            // 头结束位置也可能一次读超 64 KiB（上次读 60 KiB + 本次 20 KiB）。
            if (*body_begin > kMaxHeaderBytes) {
                *too_large = true;
                return false;
            }
            return true;
        }
    }
}

/**
 * @brief 按 Content-Length 读满 body。
 * @param connection [in,out] 已 accept 的连接；只读不写。
 * @param buffer [in,out] 已含完整头；返回时可能追加 body，超长部分被截掉。
 * @param body_begin [in] body 起始偏移。
 * @param content_length [in] 声明长度（字节，已由调用方限制在 kMaxBodyBytes 内）。
 * @return 实际读到的 body 字节数；连接提前关闭/超时时小于 content_length。
 */
inline std::size_t ReadRequestBody(go2cpp::net::Socket& connection, std::string* buffer,
                                   std::size_t body_begin, std::size_t content_length) {
    char chunk[64 * 1024];
    const std::size_t wanted = body_begin + content_length;
    while (buffer->size() < wanted) {
        const auto result =
            connection.recv(chunk, sizeof(chunk), 0, std::chrono::milliseconds(2000));
        if (!result.ok() || result.transferred <= 0) {
            break;
        }
        buffer->append(chunk, static_cast<std::size_t>(result.transferred));
    }
    if (buffer->size() > wanted) {
        buffer->resize(wanted);
    }
    return buffer->size() - body_begin;
}

/// @brief 单连接处理：读完整请求 → 交给 handler → 写响应。
inline void ServeConnection(go2cpp::net::Socket& connection, const HttpHandler& handler) {
    std::string buffer;
    std::size_t body_begin = 0;
    bool header_too_large = false;
    HttpRequest request;
    HttpResponse response;
    const auto set_error = [&response](int status, std::string_view message) {
        response.status = status;
        response.body = ErrorBody(status, message);
    };

    if (!ReadRequestHead(connection, &buffer, &body_begin, &header_too_large)) {
        // 头没读完不能进路由：畸形请求行 / 超长头必须是 400/431，不能落成 404。
        if (header_too_large) {
            set_error(431, "请求头超过 64 KiB");
        } else {
            set_error(400, "请求头不完整或连接提前关闭");
        }
    } else if (!ParseRequestHead(buffer, body_begin, &request)) {
        set_error(400, "请求行或请求头非法");
    } else {
        std::size_t content_length = 0;
        bool length_valid = true;
        const auto length_header = request.headers.find("content-length");
        if (length_header != request.headers.end() &&
            !ParseContentLength(length_header->second, &content_length)) {
            length_valid = false;
        }
        if (!length_valid) {
            set_error(400, "Content-Length 非法");
        } else if (content_length > kMaxBodyBytes) {
            set_error(400, "请求体超过 32 MiB");
        } else {
            const std::size_t actual =
                ReadRequestBody(connection, &buffer, body_begin, content_length);
            if (actual != content_length) {
                // 短 body 绝不能当完整请求处理：上传会落盘半截文件并写 DB。
                set_error(400, "请求体不完整");
            } else {
                request.body.assign(buffer, body_begin, content_length);
                if (!handler(request, &response)) {
                    response.status = 404;
                    response.body = ErrorBody(404, "not found");
                }
            }
        }
    }

    const std::string payload = Serialize(response);
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const auto result =
            connection.send(payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (!result.ok() || result.transferred <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(result.transferred);
    }
}

/// @brief 监听端口并按请求顺序处理（骨架级：单线程 accept 循环）。
class HttpServer {
   public:
    HttpServer(std::string name, std::uint16_t port)
        : m_name(std::move(name)), m_port(port) {}

    /// @brief 绑定并监听 0.0.0.0:port。
    bool Start(std::string* error) {
        m_listener = go2cpp::net::ListenTCP("0.0.0.0", m_port);
        if (!m_listener) {
            if (error != nullptr) {
                *error = m_name + " 监听 0.0.0.0:" + std::to_string(m_port) + " 失败";
            }
            return false;
        }
        return true;
    }

    /// @brief 永久接受连接；进程被信号终止时退出。
    void ServeForever(const HttpHandler& handler) {
        for (;;) {
            auto connection = m_listener->accept();
            if (!connection) {
                continue;
            }
            ServeConnection(*connection, handler);
            connection->close();
        }
    }

    std::uint16_t Port() const noexcept { return m_port; }

   private:
    std::string m_name;
    std::uint16_t m_port;
    go2cpp::net::Socket::Ptr m_listener;
};

}  // namespace douyin
