/**
 * @file douyin_http.hpp
 * @brief 骨架用的最小 HTTP/1.1 服务器：基于 go2cpp::net::Socket，无第三方库。
 *
 * @details 支持「读完整请求（请求行 + 头 + 按 Content-Length 的 body）→ 写一个
 *          响应 → 关闭连接」。请求头键名统一转小写，便于查 Authorization。
 *          业务实现阶段应替换为 go2cpp::bento::HttpServer。
 *
 *          明确的能力边界（都是硬限制，不是「尽力而为」）：
 *          - 只支持 `Content-Length` 定界；`Transfer-Encoding`（含 chunked）
 *            一律拒绝并回 400/501，且不读 body、不落盘；
 *          - 请求头 ≤ 64 KiB、body ≤ 32 MiB，超出分别回 431/400；
 *          - 单请求总耗时 ≤ 30 s（含读头与读 body），超时回 408 并关闭；
 *          - 不支持 HTTP pipeline：读完一个请求后若缓冲仍有字节（即对端
 *            pipeline 了后续请求），响应显式带 `Connection: close` 并记日志，
 *            后续请求不会被处理。
 *
 * @warning 本服务器是**串行**架构：单线程 accept 循环一次只服务一个连接。
 *          因此 30 s 总时限同时意味着「一个慢客户端最多可独占服务器 30 s」，
 *          其余连接在内核 backlog 中排队。这是骨架级实现的已知限制，生产环境
 *          必须换成并发服务器（bento::HttpServer）。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#pragma once

#include "douyin_shutdown.hpp"
#include "go2cpp/network/socket.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
    /// @brief 额外响应头（如幂等中间件的 `Idempotency-Replayed`）。
    /// @note 键按调用方给的大小写原样输出；Serialize 会跳过含 CR/LF 的字段
    ///       以防止响应头注入（跳过不影响 body，只丢这一条头）。
    std::map<std::string, std::string> headers;
    /// @brief 是否在响应里显式声明 `Connection: close`。
    ///        默认 true：本服务器每连接只处理一个请求；检测到 pipeline 残留
    ///        字节时会再次置 true 并记日志，让对端明确知道后续请求未被处理。
    bool connection_close{true};
};

/// @brief 命中路由返回 true 并填好 response；返回 false 由服务器回 404。
using HttpHandler = std::function<bool(const HttpRequest&, HttpResponse*)>;

/// @brief 单次请求的字节上限（头 64 KiB + body 32 MiB，容纳视频上传）。
///        超限在读取层直接拒绝（431/400），不会 clamp 成截断 body。
inline constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
inline constexpr std::size_t kMaxBodyBytes = 32 * 1024 * 1024;
/// @brief 单请求总时限（含读请求头与读 body）；到点回 408 并关闭连接。
inline constexpr std::chrono::milliseconds kRequestTimeout{30000};
/// @brief 单次 recv 等待切片；切片超时只回到总预算里继续等，不直接判失败。
inline constexpr std::chrono::milliseconds kRecvSlice{2000};

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
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 431: return "Request Header Fields Too Large";
        case 501: return "Not Implemented";
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
    for (const auto& header : response.headers) {
        // 只跳过非法字段（防 CRLF 注入）：只读请求头可能带有攻击者构造的值，
        // 不能原样回显到响应头里。
        if (header.first.empty() ||
            header.first.find_first_of("\r\n") != std::string::npos ||
            header.second.find_first_of("\r\n") != std::string::npos) {
            continue;
        }
        head += header.first + ": " + header.second + "\r\n";
    }
    if (response.connection_close) {
        head += "Connection: close\r\n";
    }
    head += "\r\n";
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
 * @brief 解析 Content-Length：用 std::from_chars 严格解析非负十进制整数。
 *
 * @param text [in] 头部原值（首尾空白已由 ParseHeaders 去掉）；不得为空。
 * @param out [out] 成功时写入字节数；失败时**保持原值**（调用方不应读取）。
 * @return
 * - true：整个 text 恰好是一个可放入 size_t 的十进制整数——调用方**必须**
 *   用它做长度判断；
 * - false：空串、前导 `+`/`-`、含非数字字符、尾随垃圾或溢出——调用方**必须**
 *   回 400，不能像 atoll 那样静默按 0 处理（否则 abc 会变成「无 body」）。
 *
 * ## 定位
 * 消息定界的唯一解析点：这里放松一个字符，就会出现请求走私/截断上传。
 */
inline bool ParseContentLength(std::string_view text, std::size_t* out) {
    if (text.empty() || out == nullptr) {
        return false;
    }
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    std::size_t value = 0;
    // from_chars 不接受前导 '+'/空白、不做部分解析：ptr == end 才算全串合法。
    const auto result = std::from_chars(begin, end, value, 10);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
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

/// @brief 单请求的总截止时间（steady_clock，不受系统时间跳变影响）。
struct RequestDeadline {
    std::chrono::steady_clock::time_point at;  ///< 绝对截止时刻。

    /// @brief 是否已越过 kRequestTimeout 总预算。
    bool expired() const noexcept {
        return std::chrono::steady_clock::now() >= at;
    }

    /// @brief 下一次 recv 可用的等待时长：剩余预算与 kRecvSlice 取小，至少 1 ms。
    std::chrono::milliseconds Slice() const noexcept {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            at - std::chrono::steady_clock::now());
        if (left <= std::chrono::milliseconds::zero()) {
            return std::chrono::milliseconds(1);
        }
        return std::min(left, kRecvSlice);
    }
};

/// @brief 读取阶段的结果；把「对端关闭 / 超时 / 超限」分开，才能分别回 400/408/431。
enum class ReadStatus {
    kOk,        ///< 读到所需字节。
    kClosed,    ///< 对端关闭、EOF 或 socket 错误。
    kTimeout,   ///< 超过 kRequestTimeout 总预算。
    kTooLarge,  ///< 超出上限（头 64 KiB / body 32 MiB）。
};

/// @brief 头值里是否含 `chunked` 令牌（大小写不敏感，容忍逗号分隔与参数）。
inline bool HasChunkedTransferEncoding(std::string_view value) {
    std::string lower(value);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("chunked") != std::string::npos;
}

/**
 * @brief 读取请求头直到空行出现。
 *
 * @param connection [in,out] 已 accept 的连接；只读不写，所有权仍在调用方。
 * @param buffer [out] 累积读到的字节（可能已含部分 body 或被 pipeline 的字节）。
 * @param body_begin [out] 空行之后的 body 起始偏移；仅在返回 kOk 时有效。
 * @param deadline [in] 本次请求的总截止时间；每次 recv 只等剩余预算的切片。
 * @return
 * - kOk：头完整——调用方**必须**继续解析请求行与头；
 * - kTooLarge：头超过 64 KiB——调用方回 431，**不要**再读 body；
 * - kTimeout：越过 30 s 总预算——调用方回 408 并关闭；
 * - kClosed：对端关闭或 socket 错误——调用方回 400。
 *
 * ## 定位
 * 唯一的「读头」入口；这里若把超时当 EOF，慢连接会变成 400 而不是 408。
 */
inline ReadStatus ReadRequestHead(go2cpp::net::Socket& connection, std::string* buffer,
                                  std::size_t* body_begin,
                                  const RequestDeadline& deadline) {
    char chunk[64 * 1024];
    for (;;) {
        if (buffer->size() >= kMaxHeaderBytes) {
            return ReadStatus::kTooLarge;
        }
        if (deadline.expired()) {
            return ReadStatus::kTimeout;
        }
        const auto result =
            connection.recv(chunk, sizeof(chunk), 0, deadline.Slice());
        // recv 返回 0 且无错误是对端半关闭/EOF；errno 为 ETIMEDOUT 只是切片到期。
        if (result.transferred == 0 && result.error == 0) {
            return ReadStatus::kClosed;
        }
        if (!result.ok()) {
            if (result.error == ETIMEDOUT) {
                continue;  // 切片到期不判失败，回到循环顶检查总预算。
            }
            return ReadStatus::kClosed;
        }
        buffer->append(chunk, static_cast<std::size_t>(result.transferred));
        if (FindHeaderEnd(*buffer, body_begin)) {
            // 头结束位置也可能一次读超 64 KiB（上次读 60 KiB + 本次 20 KiB）。
            if (*body_begin > kMaxHeaderBytes) {
                return ReadStatus::kTooLarge;
            }
            return ReadStatus::kOk;
        }
    }
}

/**
 * @brief 按 Content-Length 读满 body。
 *
 * @param connection [in,out] 已 accept 的连接；只读不写。
 * @param buffer [in,out] 已含完整头；返回时追加 body，**不截断**超出部分
 *        （超出部分是 pipeline 的后续字节，留给调用方判断并回 Connection: close）。
 * @param body_begin [in] body 起始偏移。
 * @param content_length [in] 声明长度（字节，0 表示无 body）。
 * @param deadline [in] 本次请求的总截止时间。
 * @return
 * - kOk：读满 content_length 字节——调用方**必须**按 content_length 截取 body；
 * - kTooLarge：content_length > 32 MiB——**没有读任何 body 字节**，调用方回 400；
 * - kTimeout：越过 30 s 总预算——调用方回 408 并关闭；
 * - kClosed：对端提前关闭——调用方回 400，**不要**把半个 body 交给 handler。
 *
 * ## 定位
 * 唯一的「读 body」入口，也是 32 MiB 上限的唯一执行点：越限必须在读之前拒绝，
 * 否则既浪费内存/带宽，又会让「声明 33 MiB、实发少量」的请求静默变成半截文件。
 */
inline ReadStatus ReadRequestBody(go2cpp::net::Socket& connection, std::string* buffer,
                                  std::size_t body_begin, std::size_t content_length,
                                  const RequestDeadline& deadline) {
    // 越限在读取前拒绝：不读一个字节，也不 clamp（截断后 handler 会当成完整请求）。
    if (content_length > kMaxBodyBytes) {
        return ReadStatus::kTooLarge;
    }
    char chunk[64 * 1024];
    const std::size_t wanted = body_begin + content_length;
    while (buffer->size() < wanted) {
        if (deadline.expired()) {
            return ReadStatus::kTimeout;
        }
        const auto result =
            connection.recv(chunk, sizeof(chunk), 0, deadline.Slice());
        if (result.transferred == 0 && result.error == 0) {
            return ReadStatus::kClosed;
        }
        if (!result.ok()) {
            if (result.error == ETIMEDOUT) {
                continue;  // 切片到期，总预算还在：继续等，最终由 30 s 兜底。
            }
            return ReadStatus::kClosed;
        }
        buffer->append(chunk, static_cast<std::size_t>(result.transferred));
    }
    return ReadStatus::kOk;
}

/**
 * @brief 单连接处理：读完整请求 → 交给 handler → 写响应。
 *
 * @param connection [in,out] 已 accept 的连接；只读写，不负责关闭（调用方 close）。
 * @param handler [in] 路由回调；返回 false 时本函数回 404。
 * @return 无返回值；所有错误都写进 response（400/408/431/501）。
 *
 * ## 定位
 * 唯一的「请求 → 响应」编排点：状态码优先级与「不落盘」保证都在这里。
 *
 * ## 步骤
 * step1. 读头：区分 431（超长）/408（30 s）/400（不完整）；
 * step2. 解析请求行与头：畸形或重复冲突的 Content-Length → 400；
 * step3. Transfer-Encoding：任何 TE 都不支持，chunked → 501 且不进 handler；
 * step4. Content-Length：from_chars 严格解析，非法 → 400；超 32 MiB → 400；
 * step5. 读 body：按总截止时间读满；短 body → 400，超时 → 408；
 * step6. 检出 pipeline 残留字节 → 响应显式 Connection: close 并记日志；
 * step7. 调 handler，写响应。
 *
 * ## 并发/失败语义
 * - 调用约束：单线程 accept 循环内串行调用，不要求同步；
 * - 失败语义：所有失败都走 4xx/5xx 响应，不抛异常、不写盘；
 * - 不变量：只有 kOk 且 body 完整时才调用 handler。
 */
inline void ServeConnection(go2cpp::net::Socket& connection, const HttpHandler& handler) {
    std::string buffer;
    std::size_t body_begin = 0;
    HttpRequest request;
    HttpResponse response;
    const RequestDeadline deadline{std::chrono::steady_clock::now() + kRequestTimeout};
    const auto set_error = [&response](int status, std::string_view message) {
        response.status = status;
        response.body = ErrorBody(status, message);
    };

    // step1: 读头——超长/超时/断开分别映射 431/408/400，不能混成 400。
    const ReadStatus head_status =
        ReadRequestHead(connection, &buffer, &body_begin, deadline);
    if (head_status != ReadStatus::kOk) {
        if (head_status == ReadStatus::kTooLarge) {
            set_error(431, "请求头超过 64 KiB");
        } else if (head_status == ReadStatus::kTimeout) {
            set_error(408, "请求超时（30 秒）");
        } else {
            set_error(400, "请求头不完整或连接提前关闭");
        }
    } else if (!ParseRequestHead(buffer, body_begin, &request)) {
        // step2: 畸形请求行、重复且冲突的 Content-Length 都在这里拒绝。
        set_error(400, "请求行或请求头非法");
    } else if (!request.Header("transfer-encoding").empty()) {
        // step3: 不支持任何 Transfer-Encoding。chunked 尤其必须显式拒绝：若按
        // CL=0 处理，chunk 字节会被当成 pipeline 的后续请求，既不安全也不符合
        // RFC 7230 §3.3.3。这里不读 body、不进 handler，因此绝不会落盘。
        const std::string transfer_encoding = request.Header("transfer-encoding");
        if (HasChunkedTransferEncoding(transfer_encoding)) {
            set_error(501, "Transfer-Encoding: chunked 不支持");
        } else {
            set_error(400, "Transfer-Encoding 不支持（只接受 Content-Length）");
        }
    } else {
        std::size_t content_length = 0;
        bool length_valid = true;
        const auto length_header = request.headers.find("content-length");
        if (length_header != request.headers.end() &&
            !ParseContentLength(length_header->second, &content_length)) {
            length_valid = false;
        }
        if (!length_valid) {
            // step4: 非法 Content-Length 必须 400，不能像 atoll 那样静默按 0 处理。
            set_error(400, "Content-Length 非法");
        } else {
            // step5: 读满 body；32 MiB 上限在读取前执行，短读/超时都不进 handler。
            const ReadStatus body_status = ReadRequestBody(
                connection, &buffer, body_begin, content_length, deadline);
            if (body_status == ReadStatus::kTooLarge) {
                set_error(400, "请求体超过 32 MiB");
            } else if (body_status == ReadStatus::kTimeout) {
                set_error(408, "请求体读取超时（30 秒）");
            } else if (body_status == ReadStatus::kClosed) {
                set_error(400, "请求体不完整");
            } else {
                request.body.assign(buffer, body_begin, content_length);
                // step6: 读完一个请求后缓冲仍有字节 = 对端 pipeline 了后续请求。
                // 本服务器一次只处理一个请求，所以必须显式回 Connection: close；
                // 否则客户端会以为第二个请求也生效了（静默丢弃）。
                const std::size_t consumed = body_begin + content_length;
                if (buffer.size() > consumed) {
                    response.connection_close = true;
                    std::fprintf(stderr,
                                 "[http] 不支持 pipeline：请求后缓冲仍有 %zu 字节，"
                                 "已回 Connection: close 并关闭连接\n",
                                 buffer.size() - consumed);
                }
                // step7: 只有 body 完整时才进路由。
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
/// @warning 串行架构：一个连接处理完才 accept 下一个；配合 30 s 单请求总时限，
///          一个慢客户端最多可占用服务器 30 s，其余连接在 backlog 排队。
///          生产环境应替换为 bento::HttpServer。
class HttpServer {
   public:
    /// @brief 就绪探针：返回 false 表示依赖（数据库等）不可用。
    /// @details 在 accept 循环线程内串行调用，实现方无需同步。
    using ReadinessProbe = std::function<bool(std::string* detail)>;

    /// @brief accept 轮询超时：决定收到停止信号后多久停止 accept。
    static constexpr std::chrono::milliseconds kAcceptPoll{200};

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

    /// @brief 注册就绪探针（例如数据库 `SELECT 1`）；未注册时只看关闭状态。
    void SetReadinessProbe(ReadinessProbe probe) { m_probe = std::move(probe); }

    /**
     * @brief 接受连接直到收到 SIGTERM/SIGINT，然后优雅关闭。
     *
     * ## 步骤
     * step1. 安装信号处理器与 5 s 看门狗（幂等）；
     * step2. accept 前检查停止标志：命中即停止 accept，不再接受新连接；
     * step3. 已接受的连接按串行语义完整处理完（请求不被打断）；
     * step4. 回到循环顶部确认停止标志，打印「优雅关闭完成」并以退出码 0 返回。
     *
     * ## 并发/失败语义
     * - 调用约束：只能在主线程调用一次；串行处理，最多 1 个在途请求；
     * - 信号语义：handler 只置位，`SA_RESTART` 保证在途 `recv` 不被 EINTR 打断；
     * - 超时语义：看门狗从信号到达起 5 s 未退出则打印日志并 `_Exit(1)`。
     */
    void ServeForever(const HttpHandler& handler) {
        // step1: 信号只置位；看门狗负责 5 s 硬上限。
        InstallShutdownSignals();
        ArmShutdownWatchdog();
        const HttpHandler wrapped = [this, &handler](const HttpRequest& request,
                                                     HttpResponse* response) {
            return HandleHealth(request, response) || handler(request, response);
        };
        for (;;) {
            // step2: 收到停止信号就停止 accept；本轮已接受的连接不受影响。
            if (ShutdownRequested()) {
                std::printf("[%s] 优雅关闭：收到停止信号，停止 accept，等待在途请求完成（在途 %d）\n",
                            m_name.c_str(), m_inflight.load(std::memory_order_relaxed));
                std::fflush(stdout);
                break;
            }
            auto connection = m_listener->accept(kAcceptPoll);
            if (!connection) {
                continue;
            }
            // step3: 串行处理——请求完整走完才回到循环，kill 不会中断在途请求。
            m_inflight.fetch_add(1, std::memory_order_relaxed);
            ServeConnection(*connection, wrapped);
            m_inflight.fetch_sub(1, std::memory_order_relaxed);
            connection->close();
            // 关闭期间处理完的那一个在途请求：明确记录，便于验证「kill 不中断请求」。
            if (ShutdownRequested()) {
                std::printf("[%s] 优雅关闭：在途请求已完成\n", m_name.c_str());
                std::fflush(stdout);
            }
        }
        // step4: 关掉监听 fd，打印完成并交给 main 以 0 退出。
        m_listener->close();
        MarkShutdownFinished();
        std::printf("[%s] 优雅关闭完成：在途请求已处理完，退出码 0\n", m_name.c_str());
        std::fflush(stdout);
    }

    std::uint16_t Port() const noexcept { return m_port; }

   private:
    /// @brief 处理 `/health/live` 与 `/health/ready`；命中返回 true。
    /// @param request [in] 已解析请求——借用，调用期间有效。
    /// @param response [out] 命中时写入状态码与 JSON body。
    /// @return true 表示已处理，不再进入业务路由。
    bool HandleHealth(const HttpRequest& request, HttpResponse* response) const {
        // step1: 只响应 GET/HEAD；其余方法交给业务路由（通常回 405）。
        if (request.method != "GET" && request.method != "HEAD") {
            return false;
        }
        // step2: liveness 只看进程是否还能服务——收到停止信号后仍回 200，
        //        让编排侧把「关闭中」判定交给 readiness。
        if (request.path == "/health/live") {
            response->status = 200;
            response->content_type = "application/json; charset=utf-8";
            response->body = "{\"ok\":true,\"status\":\"live\",\"service\":\"" +
                             JsonEscape(m_name) + "\"}";
            return true;
        }
        // step3: readiness = 未在关闭中 && 依赖探针通过；任一不满足回 503。
        if (request.path == "/health/ready") {
            std::string detail;
            bool ready = !ShutdownRequested();
            if (!ready) {
                detail = "shutting down";
            } else if (m_probe && !m_probe(&detail)) {
                ready = false;
            }
            response->status = ready ? 200 : 503;
            response->content_type = "application/json; charset=utf-8";
            if (ready) {
                response->body = "{\"ok\":true,\"status\":\"ready\",\"service\":\"" +
                                 JsonEscape(m_name) + "\"}";
            } else {
                response->body = "{\"ok\":false,\"status\":\"not-ready\",\"service\":\"" +
                                 JsonEscape(m_name) + "\",\"reason\":\"" +
                                 JsonEscape(detail) + "\"}";
            }
            return true;
        }
        return false;
    }

    /// @brief 最小 JSON 字符串转义：足够承载探针错误文本，不引入额外依赖。
    /// @param text [in] 原始文本——借用，调用期间有效。
    /// @return 可安全嵌入 JSON 字符串的副本。
    static std::string JsonEscape(std::string_view text) {
        std::string out;
        out.reserve(text.size() + 8);
        for (const char raw : text) {
            const unsigned char c = static_cast<unsigned char>(raw);
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    out += (c < 0x20) ? '?' : raw;
                    break;
            }
        }
        return out;
    }

    std::string m_name;
    std::uint16_t m_port;
    go2cpp::net::Socket::Ptr m_listener;
    ReadinessProbe m_probe;
    std::atomic<int> m_inflight{0};
};


}  // namespace douyin
