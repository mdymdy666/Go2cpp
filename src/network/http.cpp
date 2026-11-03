/**
 * @file http.cpp
 * @brief 实现 HTTP/1.x 消息、增量解析、查询编码和基础客户端/服务端。
 * @details Parser 保留流水线剩余字节并限制头体大小；网络读写通过 Socket，
 *          不在任何用户锁内 park Fiber 或执行回调。
 */
#include "go2cpp/network/http.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <exception>
#include <sstream>
#include <limits>
#include <ctime>
#include <iomanip>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace go2cpp::http {

namespace {
bool equal_ci(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) return false;
    return true;
}
bool valid_header_name(std::string_view value) noexcept {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        const bool token = std::isalnum(c) || c == '!' || c == '#' || c == '$' ||
            c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' || c == '-' ||
            c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
        if (!token) return false;
    }
    return true;
}
bool valid_header_value(std::string_view value) noexcept {
    for (unsigned char c : value) if (c < 0x20U && c != '\t') return false;
    return true;
}
bool valid_http_version_token(std::string_view value) noexcept {
    if (value.size() < 8 || value.substr(0, 5) != "HTTP/") return false;
    const auto dot = value.find('.', 5);
    if (dot == std::string_view::npos || dot == 5 || dot + 1 == value.size()) return false;
    for (std::size_t i = 5; i < value.size(); ++i)
        if (i != dot && (value[i] < '0' || value[i] > '9')) return false;
    return true;
}
bool valid_request_target(std::string_view value) noexcept {
    if (value.empty()) return false;
    for (const unsigned char character : value)
        if (character <= 0x20U || character == 0x7fU) return false;
    return true;
}
using SteadyClock = std::chrono::steady_clock;
SteadyClock::time_point make_deadline(std::chrono::milliseconds timeout) noexcept {
    const auto now = SteadyClock::now();
    if (timeout == std::chrono::milliseconds::max()) return SteadyClock::time_point::max();
    if (timeout <= std::chrono::milliseconds::zero()) return now;
    const auto available = std::chrono::duration<long double>(SteadyClock::time_point::max() - now);
    if (std::chrono::duration<long double>(timeout) >= available)
        return SteadyClock::time_point::max();
    return now + std::chrono::duration_cast<SteadyClock::duration>(timeout);
}
std::chrono::milliseconds remaining_time(SteadyClock::time_point deadline) noexcept {
    if (deadline == SteadyClock::time_point::max()) return std::chrono::milliseconds::max();
    const auto now = SteadyClock::now();
    if (now >= deadline) return std::chrono::milliseconds::zero();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return remaining == std::chrono::milliseconds::zero()
        ? std::chrono::milliseconds(1) : remaining;
}
bool has_connection_token(const Headers& headers, std::string_view wanted) {
    for (const auto& value : headers.values("Connection")) {
        std::string_view remaining(value);
        while (true) {
            const auto comma = remaining.find(',');
            auto token = remaining.substr(0, comma);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.remove_prefix(1);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.remove_suffix(1);
            if (equal_ci(token, wanted)) return true;
            if (comma == std::string_view::npos) break;
            remaining.remove_prefix(comma + 1);
        }
    }
    return false;
}
/// 统计 \n 个数（glibc memchr 走向量化路径，~32 B/cycle）。
std::size_t count_newlines(const char* data, std::size_t size) noexcept {
    std::size_t count = 0;
    const char* cursor = data;
    const char* const end = data + size;
    while (cursor < end) {
        const void* found = std::memchr(cursor, '\n', static_cast<std::size_t>(end - cursor));
        if (found == nullptr) break;
        ++count;
        cursor = static_cast<const char*>(found) + 1;
    }
    return count;
}
std::string_view trim_view(std::string_view value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}
std::string trim(std::string_view value) { return std::string(trim_view(value)); }
bool parse_size(std::string_view value, std::size_t& output) {
    value = std::string_view(value.data(), value.size());
    if (value.empty()) return false;
    std::size_t result = 0;
    for (char c : value) {
        if (c < '0' || c > '9' || result > (std::numeric_limits<std::size_t>::max() - (c - '0')) / 10) return false;
        result = result * 10 + static_cast<std::size_t>(c - '0');
    }
    output = result; return true;
}
bool parse_transfer_encoding(std::string_view value, bool& chunked) {
    chunked = false;
    bool found = false;
    while (!value.empty()) {
        const auto comma = value.find(',');
        auto token = value.substr(0, comma);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.remove_prefix(1);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.remove_suffix(1);
        if (token.empty() || !std::all_of(token.begin(), token.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '_';
            })) return false;
        // 当前实现只解码 chunked；任何未知编码都不能被当作明文 body。
        if (!equal_ci(token, "chunked") || found) return false;
        found = true;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    chunked = found;
    return found;
}
bool parse_chunk_size(std::string_view line, std::size_t& size) {
    const auto semicolon = line.find(';');
    auto digits = line.substr(0, semicolon);
    while (!digits.empty() && std::isspace(static_cast<unsigned char>(digits.front()))) digits.remove_prefix(1);
    while (!digits.empty() && std::isspace(static_cast<unsigned char>(digits.back()))) digits.remove_suffix(1);
    if (digits.empty()) return false;
    size = 0;
    for (const unsigned char c : digits) {
        unsigned int value = 0;
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
        else return false;
        if (size > (std::numeric_limits<std::size_t>::max() - value) / 16) return false;
        size = size * 16 + value;
    }
    if (semicolon != std::string_view::npos) {
        const auto extension = line.substr(semicolon + 1);
        for (const unsigned char c : extension)
            if (c < 0x20U && c != '\t') return false;
    }
    return true;
}
bool split_url(const std::string& url, std::string& host, std::string& target,
               std::uint16_t& port) {
    const std::string prefix = "http://";
    if (url.rfind(prefix, 0) != 0) return false;
    const auto path_start = url.find_first_of("/?#", prefix.size());
    const auto authority = url.substr(prefix.size(), path_start == std::string::npos
        ? std::string::npos : path_start - prefix.size());
    if (authority.empty()) return false;
    std::string service;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos || close == 1) return false;
        host = authority.substr(0, close + 1);
        if (authority.size() == close + 1) {
            service = "80";
        } else if (authority[close + 1] == ':') {
            service = authority.substr(close + 2);
        } else {
            return false;
        }
    } else {
        const auto first_colon = authority.find(':');
        const auto last_colon = authority.rfind(':');
        if (first_colon == std::string::npos) {
            host = authority;
            service = "80";
        } else if (first_colon == last_colon) {
            host = authority.substr(0, first_colon);
            service = authority.substr(first_colon + 1);
        } else {
            // URL authority 中的 IPv6 必须用方括号包围，以免冒号与端口混淆。
            return false;
        }
    }
    if (host.empty() || service.empty()) return false;
    unsigned int parsed_port = 0;
    if (!std::all_of(service.begin(), service.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
        return false;
    const auto parsed = std::from_chars(service.data(), service.data() + service.size(), parsed_port);
    if (parsed.ec != std::errc{} || parsed.ptr != service.data() + service.size() ||
        parsed_port == 0 || parsed_port > 65535U) return false;
    port = static_cast<std::uint16_t>(parsed_port);
    if (path_start == std::string::npos || url[path_start] == '#') {
        target = "/";
    } else if (url[path_start] == '?') {
        target = "/" + url.substr(path_start, url.find('#', path_start) - path_start);
    } else {
        const auto fragment = url.find('#', path_start);
        target = url.substr(path_start, fragment == std::string::npos ? fragment : fragment - path_start);
    }
    return true;
}
std::size_t find_header_end(const std::string& buffer) { return buffer.find("\r\n\r\n"); }
// 方法名的静态字面量视图：序列化热路径不再每请求构造 std::string。
std::string_view method_token(Method method) noexcept {
    switch (method) {
    case Method::Delete: return "DELETE";
    case Method::Get: return "GET";
    case Method::Head: return "HEAD";
    case Method::Post: return "POST";
    case Method::Put: return "PUT";
    case Method::Connect: return "CONNECT";
    case Method::Options: return "OPTIONS";
    case Method::Trace: return "TRACE";
    case Method::Patch: return "PATCH";
    default: return {};
    }
}
}

std::string method_name(Method method) {
    switch (method) { case Method::Delete: return "DELETE"; case Method::Get: return "GET";
    case Method::Head: return "HEAD"; case Method::Post: return "POST"; case Method::Put: return "PUT";
    case Method::Connect: return "CONNECT"; case Method::Options: return "OPTIONS"; case Method::Trace: return "TRACE";
    case Method::Patch: return "PATCH"; default: return ""; }
}
Method parse_method(std::string_view value) {
    if (value == "DELETE") return Method::Delete;
    if (value == "GET") return Method::Get;
    if (value == "HEAD") return Method::Head;
    if (value == "POST") return Method::Post;
    if (value == "PUT") return Method::Put;
    if (value == "CONNECT") return Method::Connect;
    if (value == "OPTIONS") return Method::Options;
    if (value == "TRACE") return Method::Trace;
    if (value == "PATCH") return Method::Patch;
    return Method::Unknown;
}
std::string_view status_text_view(Status status) noexcept {
    switch (status) { case Status::Continue: return "Continue"; case Status::SwitchingProtocols: return "Switching Protocols"; case Status::OK: return "OK"; case Status::Created: return "Created"; case Status::Found: return "Found";
    case Status::NoContent: return "No Content"; case Status::PartialContent: return "Partial Content"; case Status::NotModified: return "Not Modified"; case Status::BadRequest: return "Bad Request"; case Status::Unauthorized: return "Unauthorized";
    case Status::Forbidden: return "Forbidden"; case Status::NotFound: return "Not Found"; case Status::MethodNotAllowed: return "Method Not Allowed";
    case Status::RequestTimeout: return "Request Timeout"; case Status::Conflict: return "Conflict"; case Status::PayloadTooLarge: return "Payload Too Large"; case Status::RangeNotSatisfiable: return "Range Not Satisfiable";
    case Status::InternalError: return "Internal Server Error"; case Status::NotImplemented: return "Not Implemented";
    case Status::BadGateway: return "Bad Gateway"; case Status::ServiceUnavailable: return "Service Unavailable";
    case Status::GatewayTimeout: return "Gateway Timeout"; default: return {}; }
}
std::string status_text(Status status) { return std::string(status_text_view(status)); }

bool Headers::equal_name(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) return false;
    return true;
}
void Headers::add(std::string name, std::string value) {
    // 出站头也必须执行 RFC token/field-value 校验，不能只依赖解析器校验；
    // 否则业务字符串中的 CRLF 会造成响应拆分或请求走私。
    if (!valid_header_name(name) || !valid_header_value(value)) return;
    m_entries.emplace_back(std::move(name), std::move(value));
}
void Headers::set(std::string name, std::string value) { erase(name); add(std::move(name), std::move(value)); }
bool Headers::erase(std::string_view name) { const auto old = m_entries.size(); m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&](const Entry& item) { return equal_name(item.first, name); }), m_entries.end()); return old != m_entries.size(); }
bool Headers::has(std::string_view name) const { return std::any_of(m_entries.begin(), m_entries.end(), [&](const Entry& item) { return equal_name(item.first, name); }); }
std::string Headers::get(std::string_view name, std::string_view fallback) const { for (const auto& item : m_entries) if (equal_name(item.first, name)) return item.second; return std::string(fallback); }
std::vector<std::string> Headers::values(std::string_view name) const { std::vector<std::string> result; for (const auto& item : m_entries) if (equal_name(item.first, name)) result.push_back(item.second); return result; }

std::string Request::path() const { const auto pos = target.find('?'); return pos == std::string::npos ? target : target.substr(0, pos); }
std::string Request::query() const { const auto pos = target.find('?'); return pos == std::string::npos ? std::string{} : target.substr(pos + 1); }
void Request::add_cookie(std::string name, std::string value) { const auto old = header.get("Cookie"); header.set("Cookie", old.empty() ? name + "=" + value : old + "; " + name + "=" + value); }
std::string Request::path_value(std::string_view name) const { auto found = path_values.find(std::string(name)); return found == path_values.end() ? std::string{} : found->second; }
std::string Request::to_string() const {
    const std::string_view token = method_token(method);
    if (token.empty() || !valid_request_target(target) ||
        !valid_http_version_token(version)) return {};
    // Connection / Content-Length 只在同一次头表扫描里判断，替代两次 has() 线性扫描。
    bool has_connection = false;
    bool has_content_length = false;
    for (const auto& item : header.entries()) {
        if (!has_connection) has_connection = equal_ci(item.first, "Connection");
        if (!has_content_length) has_content_length = equal_ci(item.first, "Content-Length");
    }
    std::size_t total = token.size() + target.size() + version.size() + body.size() + 32;
    for (const auto& item : header.entries())
        total += item.first.size() + item.second.size() + 4;
    if (close && !has_connection) total += 19;
    if (!body.empty() && !has_content_length) total += 20;
    std::string wire;
    wire.reserve(total);
    wire.append(token).push_back(' ');
    wire.append(target).push_back(' ');
    wire.append(version).append("\r\n");
    for (const auto& item : header.entries())
        wire.append(item.first).append(": ").append(item.second).append("\r\n");
    if (close && !has_connection) wire.append("Connection: close\r\n");
    if (!body.empty() && !has_content_length) {
        char digits[24];
        const auto converted = std::to_chars(digits, digits + sizeof(digits), body.size());
        wire.append("Content-Length: ").append(digits, converted.ptr).append("\r\n");
    }
    wire.append("\r\n");
    wire.append(body);
    return wire;
}
void Response::set_cookie(std::string name, std::string value, std::string attributes) { std::string result = std::move(name) + "=" + std::move(value); if (!attributes.empty()) result += "; " + attributes; header.add("Set-Cookie", std::move(result)); }
void Response::redirect(std::string location, Status code) { status = code; header.set("Location", std::move(location)); }
std::string Response::to_string(bool head_request) const {
    if (!valid_http_version_token(version)) return {};
    const int code = static_cast<int>(status);
    const bool body_forbidden = (code >= 100 && code < 200) || code == 204 || code == 304;
    const auto& entries = header.entries();
    // 直接拼接到一个 string：原实现用 ostringstream，每个响应要构造流对象、
    // 逐字段格式化再 str() 复制一次，是 HTTP 服务端每请求最大的一笔固定开销。
    std::string out;
    out.reserve(version.size() + 48 + body.size() + entries.size() * 40);
    out.append(version);
    out.push_back(' ');
    {
        char code_buffer[8];
        const auto code_end =
            std::to_chars(code_buffer, code_buffer + sizeof(code_buffer), code).ptr;
        out.append(code_buffer, code_end);
    }
    out.push_back(' ');
    // status_text() 返回 std::string（一次分配）；这里只要视图。
    out.append(status_text_view(status));
    out.append("\r\n");
    // 三次 has() 线性扫描合并成一次：头越多省得越多。
    bool has_connection = false;
    bool has_content_length = false;
    bool has_transfer_encoding = false;
    for (const auto& item : entries) {
        if (!has_connection) has_connection = equal_ci(item.first, "Connection");
        if (!has_content_length) has_content_length = equal_ci(item.first, "Content-Length");
        if (!has_transfer_encoding) has_transfer_encoding = equal_ci(item.first, "Transfer-Encoding");
    }
    for (const auto& item : entries) {
        // 1xx/204/304 不允许实体 framing；过滤用户手工设置的 CL/TE，
        // 避免下游继续等待一个永远不会出现的 body。
        if (body_forbidden && (equal_ci(item.first, "Content-Length") ||
                               equal_ci(item.first, "Transfer-Encoding"))) {
            continue;
        }
        out.append(item.first);
        out.append(": ");
        out.append(item.second);
        out.append("\r\n");
    }
    if (close && !has_connection) out.append("Connection: close\r\n");
    // 空 body 也必须补 Content-Length: 0：keep-alive 下客户端无法区分
    // 「无 body」与「body 直到连接关闭」，双方会互等（http1.md §4 已记录）。
    // 有 Transfer-Encoding（chunked）时不补 CL（两种 framing 互斥）。
    if (!body_forbidden && !has_content_length && !has_transfer_encoding) {
        // 文件体不进入内存：Content-Length 取 Range 后的发送字节数。
        const std::size_t content_size =
            file ? static_cast<std::size_t>(file->length) : body.size();
        char length_buffer[24];
        const auto length_end = std::to_chars(length_buffer,
                                              length_buffer + sizeof(length_buffer),
                                              content_size).ptr;
        out.append("Content-Length: ");
        out.append(length_buffer, length_end);
        out.append("\r\n");
    }
    out.append("\r\n");
    // 文件体走 sendfile(2)，绝不拷贝进内存；HEAD 只回头。
    if (!head_request && !body_forbidden && !file) out.append(body);
    return out;
}

Parser::Parser(ParserLimits limits) : m_limits(limits) {}
void Parser::reset() { m_buffer.clear(); m_expected_body = 0; m_chunk_remaining = 0; m_chunked = false; m_finished = false; m_in_chunk_trailer = false; m_response_close_delimited = false; m_body_start = 0; m_error.clear(); }
ParseResult Parser::feed(std::string_view bytes, Request& request) { return feed_impl(bytes, false, &request, nullptr); }
ParseResult Parser::feed(std::string_view bytes, Response& response) { return feed_impl(bytes, true, nullptr, &response); }
ParseResult Parser::finish(Response& response) {
    if (m_finished) return {ParseState::Complete, 0, {}};
    if (!m_response_close_delimited) {
        const auto parsed = feed_impl({}, true, nullptr, &response);
        if (parsed.state != ParseState::NeedMore) return parsed;
        return {ParseState::Error, 0, "response is not EOF-delimited"};
    }
    if (m_buffer.size() < m_body_start) return {ParseState::Error, 0, "invalid EOF body offset"};
    const auto body_size = m_buffer.size() - m_body_start;
    if (body_size > m_limits.max_body_bytes) return {ParseState::Error, 0, "body too large"};
    response.body.assign(m_buffer, m_body_start, body_size);
    response.close = true;
    const auto consumed = m_buffer.size();
    m_buffer.clear();
    m_finished = true;
    return {ParseState::Complete, consumed, {}};
}
std::string Parser::take_remaining() { std::string result; result.swap(m_buffer); reset(); return result; }
/**
 * @brief HTTP 增量解析核心：把新到的字节并入缓冲，推进请求行/头部/消息体状态机，产出解析结果。
 *
 * @param bytes [in] 本次新到的字节——借用，仅在本调用内读取；可空（finish 路径用它推进 EOF 语义）。
 * @param response [in] 解析方向——true 按响应报文解析，false 按请求报文解析；决定起始行与 body framing 规则。
 * @param request [out] 请求对象——response=false 时必填，成功解析出完整报文才写入；失败保持未完成状态。
 * @param result [out] 响应对象——response=true 时必填，同上。
 *
 * @return
 * - ParseResult{kComplete, consumed}：报文已完整——调用方**必须**按 consumed 处理流水线剩余字节，不要丢弃尾部；
 * - ParseResult{kNeedMore, consumed}：还缺数据——调用方**不要**重复喂同一段已消费字节，继续 recv 再 feed；
 * - ParseResult{kError}：协议/限额错误——调用方**必须**关闭连接，不要尝试跳过该报文继续解析。
 *
 * ## 定位
 * 唯一的报文状态推进点：请求与响应共用同一实现，所有 framing（CL/chunked/EOF 定界）判定都在这里。
 * 写错会造成半包误判（提前 Complete）或粘包丢字节（consumed 与缓冲不一致），两者都是数据面静默错误。
 *
 * ## 步骤
 * step1. 校验缓冲上限并按需追加新字节，分配失败转 ParseState::Error 而不是抛异常；
 * step2. 若已判定为「EOF 定界响应」则不再找头部，只做体积检查并继续 NeedMore，直到 finish() 收尾；
 * step3. 查找头部结束标记；未找到时按头部上限判定 NeedMore 或 Error；
 * step4. 从起始行切分版本/状态码或方法/目标，逐段做格式与长度校验；
 * step5. 解析头部字段并据 Content-Length/Transfer-Encoding 选择 framing，推进到消息体状态；
 * step6. 按 framing 判定 Complete 或 NeedMore，返回 consumed 供上层切分流水线。
 *
 * ## 并发/失败语义
 * - 调用约束：Parser 是单连接状态机，同一实例只能由一条执行流串行 feed；不内部加锁。
 * - 原子性：状态推进非事务性——一旦返回 Error，内部 m_finished/m_error 已落定，实例不可复用（除非 reset）。
 * - 失败语义：不抛异常；所有分配失败都转成 ParseState::Error 并写 m_error。
 * - 不变量：返回 Complete 时缓冲中 [0, consumed) 属于该报文；返回 NeedMore 时已消费字节不会在下次调用中重复计算。
 */
ParseResult Parser::feed_impl(std::string_view bytes, bool response, Request* request, Response* result) {
    if (m_finished) return {ParseState::Complete, 0, {}};
    if (m_buffer.size() > std::numeric_limits<std::size_t>::max() - bytes.size() ||
        (m_buffer.size() + bytes.size() > m_limits.max_header_bytes &&
         m_buffer.size() + bytes.size() - m_limits.max_header_bytes > m_limits.max_body_bytes))
        return {ParseState::Error, 0, "input too large"};
    try {
        // step1: 校验缓冲上限后追加新字节；分配失败转 ParseState::Error 并写 m_error，不抛异常。
        m_buffer.append(bytes.data(), bytes.size());
    } catch (const std::exception&) {
        m_error = "input allocation failed";
        return {ParseState::Error, 0, m_error};
    }
    // step2: EOF 定界响应不再找头部，只做体积检查并继续 NeedMore，直到 finish() 收尾。
    if (response && m_response_close_delimited) {
        if (m_buffer.size() - m_body_start > m_limits.max_body_bytes)
            return {ParseState::Error, bytes.size(), "body too large"};
        return {ParseState::NeedMore, bytes.size(), {}};
    }
    // step3: 查找头部结束标记；未找到按头部上限判定 NeedMore 或 Error，防止无界增长。
    const auto header_end = find_header_end(m_buffer);
    if (header_end == std::string::npos) {
        if (m_buffer.size() > m_limits.max_header_bytes) { m_error = "header too large"; return {ParseState::Error, bytes.size(), m_error}; }
        return {ParseState::NeedMore, bytes.size(), {}};
    }
    if (header_end + 4 > m_limits.max_header_bytes) { m_error = "header too large"; return {ParseState::Error, bytes.size(), m_error}; }
    const std::string_view header_block(m_buffer.data(), header_end);
    std::size_t line_end = header_block.find("\r\n");
    if (line_end == std::string::npos || line_end > m_limits.max_line_bytes)
        return {ParseState::Error, bytes.size(), "invalid start line"};
    // step4: 切分起始行并做格式/长度校验；请求行只允许三段，响应行第三段可含空格。
    const std::string_view start_line = header_block.substr(0, line_end);
    // 起始行手写切分：istringstream 每请求要构造流对象并分配，是解析热路径上
    // 最大的一笔固定开销。
    std::string_view first;
    std::string_view second;
    std::string_view third;
    {
        const auto first_space = start_line.find(' ');
        if (first_space == std::string_view::npos || first_space == 0)
            return {ParseState::Error, bytes.size(), "invalid start line"};
        first = start_line.substr(0, first_space);
        const auto second_space = start_line.find(' ', first_space + 1);
        if (second_space == std::string_view::npos || second_space == first_space + 1)
            return {ParseState::Error, bytes.size(), "invalid start line"};
        second = start_line.substr(first_space + 1, second_space - first_space - 1);
        third = start_line.substr(second_space + 1);
        if (third.empty()) return {ParseState::Error, bytes.size(), "invalid start line"};
        // 请求行只有三段；响应行的第三段是 reason-phrase，可以含空格。
        if (!response && third.find(' ') != std::string_view::npos)
            return {ParseState::Error, bytes.size(), "invalid start line"};
    }
    if (response) {
        if (!valid_http_version_token(first) || second.empty())
            return {ParseState::Error, bytes.size(), "invalid response"};
        if (second.size() != 3 || !std::all_of(second.begin(), second.end(),
                                                [](unsigned char c) { return std::isdigit(c); }))
            return {ParseState::Error, bytes.size(), "invalid status"};
        int code = 0;
        const auto parsed_code =
            std::from_chars(second.data(), second.data() + second.size(), code);
        if (parsed_code.ec != std::errc() ||
            parsed_code.ptr != second.data() + second.size())
            return {ParseState::Error, bytes.size(), "invalid status"};
        result->version.assign(first);
        result->status = static_cast<Status>(code);
    } else {
        request->method = parse_method(first);
        if (request->method == Method::Unknown || !valid_request_target(second) ||
            !valid_http_version_token(third))
            return {ParseState::Error, bytes.size(), "invalid request"};
        request->target.assign(second);
        request->version.assign(third);
    }
    std::size_t content_length = 0;
    bool has_length = false;
    bool chunked = false;
    bool has_transfer_encoding = false;
    // 每个头都要 emplace_back：先按 CRLF 计数预留，消除 vector 扩容与搬移。
    {
        const auto header_lines = count_newlines(header_block.data(), header_block.size());
        if (response) result->header.reserve(header_lines);
        else request->header.reserve(header_lines);
    }
    for (std::size_t offset = line_end + 2; offset < header_block.size();) {
        const auto found_end = header_block.find("\r\n", offset);
        const auto end = found_end == std::string_view::npos ? header_block.size() : found_end;
        const auto colon = header_block.find(':', offset);
        if (colon == std::string_view::npos || colon >= end || colon == offset)
            return {ParseState::Error, bytes.size(), "invalid header"};
        const std::string_view name = header_block.substr(offset, colon - offset);
        const std::string_view value = trim_view(header_block.substr(colon + 1, end - colon - 1));
        if (name.find_first_of("\r\n") != std::string_view::npos)
            return {ParseState::Error, bytes.size(), "invalid header name"};
        for (const unsigned char c : value)
            if (c < 0x20U && c != '\t')
                return {ParseState::Error, bytes.size(), "invalid header value"};
        if (equal_ci(name, "Content-Length")) {
            std::size_t parsed = 0;
            if (!parse_size(value, parsed) || (has_length && parsed != content_length))
                return {ParseState::Error, bytes.size(), "invalid content length"};
            content_length = parsed;
            has_length = true;
        }
        if (equal_ci(name, "Transfer-Encoding")) {
            if (has_transfer_encoding || !parse_transfer_encoding(value, chunked))
                return {ParseState::Error, bytes.size(), "unsupported transfer encoding"};
            has_transfer_encoding = true;
        }
        if (response) result->header.add(std::string(name), std::string(value));
        else request->header.add(std::string(name), std::string(value));
        if (found_end == std::string_view::npos) break;
        offset = end + 2;
    }
    if (chunked && has_length) return {ParseState::Error, bytes.size(), "content length with chunked"};
    const std::size_t body_start = header_end + 4;
    const bool no_body = response && (m_response_to_head ||
        (static_cast<int>(result->status) >= 100 && static_cast<int>(result->status) < 200) ||
        result->status == Status::NoContent || static_cast<int>(result->status) == 304);
    if (no_body) {
        if (chunked || (!m_response_to_head && has_length && content_length != 0))
            return {ParseState::Error, bytes.size(), "body framing forbidden for response"};
        result->body.clear();
    }
    if (response && !no_body && !chunked && !has_length) {
        // 响应没有 CL/TE 时，HTTP/1.x 以连接关闭定界；不能在收到头部
        // 的同一次 recv 中错误地把 body 当成空并宣布 Complete。
        if (m_buffer.size() - body_start > m_limits.max_body_bytes)
            return {ParseState::Error, bytes.size(), "body too large"};
        m_response_close_delimited = true;
        m_body_start = body_start;
        return {ParseState::NeedMore, bytes.size(), {}};
    }
    std::size_t body_end = body_start;
    if (no_body) {
        body_end = body_start;
    } else if (body_start > std::numeric_limits<std::size_t>::max() - content_length) {
        return {ParseState::Error, bytes.size(), "body length overflow"};
    } else if (chunked) {
        std::size_t cursor = body_start; std::string body;
        while (true) {
            const auto end = m_buffer.find("\r\n", cursor); if (end == std::string::npos) return {ParseState::NeedMore, bytes.size(), {}};
            const auto line = m_buffer.substr(cursor, end - cursor);
            if (line.size() > m_limits.max_line_bytes) return {ParseState::Error, bytes.size(), "chunk line too large"};
            std::size_t chunk_size = 0; if (!parse_chunk_size(line, chunk_size)) return {ParseState::Error, bytes.size(), "invalid chunk"};
            cursor = end + 2; if (chunk_size > m_limits.max_body_bytes || body.size() > m_limits.max_body_bytes - chunk_size) return {ParseState::Error, bytes.size(), "body too large"};
            if (chunk_size == 0) {
                const auto trailer_end = m_buffer.find("\r\n\r\n", cursor);
                if (trailer_end == std::string::npos) {
                    if (m_buffer.compare(cursor, 2, "\r\n") == 0) body_end = cursor + 2;
                    else return {ParseState::NeedMore, bytes.size(), {}};
                } else {
                    if (trailer_end - cursor > m_limits.max_trailer_bytes) return {ParseState::Error, bytes.size(), "trailers too large"};
                    const auto trailer_block = m_buffer.substr(cursor, trailer_end - cursor);
                    std::size_t trailer_offset = 0;
                    while (trailer_offset < trailer_block.size()) {
                        const auto trailer_line_end = trailer_block.find("\r\n", trailer_offset);
                        const auto trailer_end_line = trailer_line_end == std::string::npos ? trailer_block.size() : trailer_line_end;
                        const auto colon = trailer_block.find(':', trailer_offset);
                        if (colon == std::string::npos || colon >= trailer_end_line || colon == trailer_offset)
                            return {ParseState::Error, bytes.size(), "invalid trailer"};
                        const auto name = trailer_block.substr(trailer_offset, colon - trailer_offset);
                        const auto value = trim(std::string_view(trailer_block).substr(colon + 1, trailer_end_line - colon - 1));
                        if (name.find_first_of("\r\n\0 \t") != std::string::npos)
                            return {ParseState::Error, bytes.size(), "invalid trailer name"};
                        if (response) result->trailer.add(name, value); else request->trailer.add(name, value);
                        if (trailer_line_end == std::string::npos) break;
                        trailer_offset = trailer_end_line + 2;
                    }
                    body_end = trailer_end + 4;
                }
                if (response) result->body = std::move(body); else request->body = std::move(body); break;
            }
            if (cursor > std::numeric_limits<std::size_t>::max() - chunk_size - 2 || m_buffer.size() < cursor + chunk_size + 2) return {ParseState::NeedMore, bytes.size(), {}};
            body.append(m_buffer, cursor, chunk_size); cursor += chunk_size; if (m_buffer.compare(cursor, 2, "\r\n") != 0) return {ParseState::Error, bytes.size(), "chunk terminator missing"}; cursor += 2;
        }
    } else {
        if (content_length > m_limits.max_body_bytes) return {ParseState::Error, bytes.size(), "body too large"};
        if (body_start > std::numeric_limits<std::size_t>::max() - content_length) return {ParseState::Error, bytes.size(), "body length overflow"};
        body_end = body_start + content_length;
        if (m_buffer.size() < body_end) return {ParseState::NeedMore, bytes.size(), {}};
        if (response) result->body.assign(m_buffer, body_start, content_length); else request->body.assign(m_buffer, body_start, content_length);
    }
    const std::size_t consumed = body_end; m_buffer.erase(0, consumed); m_finished = true;
    if (response) result->close = has_connection_token(result->header, "close");
    else request->close = has_connection_token(request->header, "close") || request->version == "HTTP/1.0";
    return {ParseState::Complete, bytes.size(), {}};
}

std::string query_escape(std::string_view value) { static const char* hex = "0123456789ABCDEF"; std::string result; for (unsigned char c : value) { if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c); else if (c == ' ') result += '+'; else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; } } return result; }
bool query_unescape(std::string_view value, std::string& output) { output.clear(); for (std::size_t i = 0; i < value.size(); ++i) { if (value[i] == '+') output += ' '; else if (value[i] == '%') { if (i + 2 >= value.size()) return false; auto hex = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; }; const int a = hex(value[i + 1]), b = hex(value[i + 2]); if (a < 0 || b < 0) return false; output += static_cast<char>((a << 4) | b); i += 2; } else output += value[i]; } return true; }
std::string canonical_header_key(std::string_view value) { std::string result(value); bool upper = true; for (char& c : result) { if (c == '-') upper = true; else if (upper) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); upper = false; } else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); } return result; }
bool parse_http_version(std::string_view value, int& major, int& minor) { if (value.rfind("HTTP/", 0) != 0) return false; const auto dot = value.find('.', 5); if (dot == std::string_view::npos) return false; try { major = std::stoi(std::string(value.substr(5, dot - 5))); minor = std::stoi(std::string(value.substr(dot + 1))); } catch (...) { return false; } return major >= 0 && minor >= 0; }
bool parse_http_time(std::string_view value, std::chrono::system_clock::time_point& output) { std::tm tm{}; std::istringstream stream{std::string(value)}; stream >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT"); if (stream.fail()) return false; const std::time_t time = timegm(&tm); if (time < 0) return false; output = std::chrono::system_clock::from_time_t(time); return true; }
std::string encode_query(const QueryValues& values) { std::string result; for (const auto& [key, list] : values) for (const auto& value : list) { if (!result.empty()) result += '&'; result += query_escape(key) + '=' + query_escape(value); } return result; }
bool decode_query(std::string_view value, QueryValues& output) { output.clear(); while (!value.empty()) { const auto amp = value.find('&'); const auto item = value.substr(0, amp); const auto equal = item.find('='); const auto key_part = item.substr(0, equal); const auto value_part = equal == std::string_view::npos ? std::string_view{} : item.substr(equal + 1); std::string key, decoded; if (!query_unescape(key_part, key) || !query_unescape(value_part, decoded)) return false; output[key].push_back(std::move(decoded)); if (amp == std::string_view::npos) break; value.remove_prefix(amp + 1); } return true; }

Client::Client(std::chrono::milliseconds timeout) : m_timeout(timeout) {}
/**
 * @brief 执行一次完整的 HTTP/1.x 请求：解析地址 → 连接 → 发送 → 读响应。
 * @param request 请求（方法、目标、头、体）；未设置 `Host`/`Connection` 时自动补。
 * @param host 主机名（用于 DNS 与 `Host` 头）。
 * @param port 端口。
 * @return `Result`：成功时 `response` 非空；失败时 `error` 说明阶段，
 *         `message` 来自 `strerror` 或解析器的诊断。
 *
 * ## 定位
 *
 * `Client::get/head/post` 的共同实现，也是 HTTP 客户端**唯一的请求生命周期**：
 * 一次调用内完成建连、发送、接收、协议解析，**不复用连接**（固定发
 * `Connection: close`）。
 *
 * ## 步骤
 *
 * step1. 定义 `classify_io`：把 errno 的 `ETIMEDOUT`/`ECANCELED` 提升为
 *        对应的 `Result::Error`，其余归到调用方指定的阶段错误（Connect/Send/Receive）；
 * step2. 计算绝对 deadline（`make_deadline`），做 DNS 解析；
 *        **每次 IO 前都用 `remaining_time(deadline)` 重算剩余时间**，
 *        而不是每步用一个固定超时——否则总耗时会被阶段数放大；
 * step3. 创建 TCP socket 并 connect；失败时按 `classify_io` 归类；
 * step4. 补全 `Host` 与 `Connection: close` 头，序列化成 wire 字节；
 *        然后**循环发送**直到 `sent == wire.size()`（部分写是合法的）；
 * step5. 创建 `Parser`（HEAD 请求要求「无响应体」），准备 8KiB 栈上缓冲与
 *        `pending`（跨 recv 的流水线剩余）；
 * step6. **接收 + 解析循环**：有待处理数据就先 `feed`，否则 `recv`；
 *        `transferred == 0` 说明对端关闭，用 `finish` 尝试收尾；
 *        `Complete` → 若状态码是 100~199（且非 101）则**跳过 informational
 *        响应**（保留剩余字节、重建 Response）继续等最终响应；否则返回；
 *        `Error` → 返回解析错误。
 *
 * ## 并发/失败语义
 *
 * - **超时是全局 deadline**，不是每阶段超时：DNS/connect/send/recv 共享它，
 *   因此慢 DNS 会压缩后续阶段的时间预算；
 * - **部分写必须循环**：`send` 返回的 `transferred` 可能小于请求长度；
 * - **`recv` 返回 0 不是错误**，是「对端优雅关闭」——要先用 `finish()` 判断
 *   是否恰好解析完（否则报 `Closed`）；
 * - **informational 响应（1xx）不是最终结果**：`pending` 保存同一 recv 里
 *   的后续字节，避免丢流水线数据；
 * - **HEAD 请求不允许响应体**：`set_response_to_head` 让解析器在头结束处
 *   直接判 `Complete`；
 * - `response` 用 `shared_ptr` 传出，因为 `Result` 会被复制而 `Response` 较大；
 * - 本函数**不做重定向跟随**，3xx 原样返回给调用方。
 */
Result Client::do_request(const Request& request, const std::string& host, std::uint16_t port) {
    // step1: 错误分类器——把超时/取消从普通阶段错误里分出来。
    const auto classify_io = [](Result::Error normal, int error) {
        if (error == ETIMEDOUT) return Result::Error::Timeout;
        if (error == ECANCELED) return Result::Error::Cancelled;
        return normal;
    };
    const auto deadline = make_deadline(m_timeout);
    // step2: 计算全局 deadline 后做 DNS 解析。
    auto address = net::Address::LookupAny(host + ":" + std::to_string(port), AF_UNSPEC, SOCK_STREAM);
    if (!address) return {Result::Error::Resolve, "resolve failed", {}};
    auto remaining = remaining_time(deadline);
    if (remaining == std::chrono::milliseconds::zero())
        return {Result::Error::Timeout, "request deadline exceeded", {}};
    // step3: 建 TCP 连接（超时用 deadline 的剩余时间）。
    auto socket = net::Socket::CreateTCP(address->family());
    if (!socket || !socket->connect(*address, remaining)) {
        const int error = errno == 0 ? EIO : errno;
        const auto category = classify_io(Result::Error::Connect, error);
        return {category, std::strerror(error), {}};
    }
    Request outgoing = request; if (!outgoing.header.has("Host")) outgoing.header.set("Host", host); if (!outgoing.header.has("Connection")) outgoing.header.set("Connection", "close");
    const auto wire = outgoing.to_string();
    if (wire.empty()) return {Result::Error::Parse, "invalid request line", {}};
    // step4: 循环发送——send 可能部分写，必须发完整个 wire。
    std::size_t sent = 0;
    while (sent < wire.size()) {
        remaining = remaining_time(deadline);
        if (remaining == std::chrono::milliseconds::zero())
            return {Result::Error::Timeout, "request deadline exceeded", {}};
        const auto written = socket->send(wire.data() + sent, wire.size() - sent, 0, remaining);
        if (!written.ok() || written.transferred == 0) {
            const int error = written.error == 0 ? (errno == 0 ? EIO : errno) : written.error;
            return {classify_io(Result::Error::Send, error), std::strerror(error), {}};
        }
        sent += static_cast<std::size_t>(written.transferred);
    }
    // step5: 准备响应解析器与栈上接收缓冲。
    Parser parser; parser.set_response_to_head(request.method == Method::Head);
    auto response = std::make_shared<Response>(); std::array<char, 8192> buffer{};
    std::string pending;
    // step6: 接收 + 解析循环（含 1xx informational 响应的处理）。
    for (;;) {
        ParseResult parsed;
        if (!pending.empty()) {
            parsed = parser.feed(pending, *response);
            pending.clear();
        } else {
            remaining = remaining_time(deadline);
            if (remaining == std::chrono::milliseconds::zero())
                return {Result::Error::Timeout, "request deadline exceeded", {}};
            auto received = socket->recv(buffer.data(), buffer.size(), 0, remaining);
            if (!received.ok()) return {classify_io(Result::Error::Receive, received.error), std::strerror(received.error), {}};
            if (received.transferred == 0) {
                parsed = parser.finish(*response);
                if (parsed.state == ParseState::Complete)
                    return {Result::Error::None, {}, std::move(response)};
                return {Result::Error::Closed, "peer closed before response completed", {}};
            }
            parsed = parser.feed(std::string_view(buffer.data(), static_cast<std::size_t>(received.transferred)), *response);
        }
        if (parsed.state == ParseState::Complete) {
            const int status = static_cast<int>(response->status);
            // 100 Continue 等 informational 响应不是最终结果；保留同一
            // recv 中的流水线剩余并重新建立响应对象，直到收到最终响应。
            if (status >= 100 && status < 200 && status != 101) {
                pending = parser.take_remaining();
                *response = Response{};
                parser.set_response_to_head(request.method == Method::Head);
                continue;
            }
            return {Result::Error::None, {}, std::move(response)};
        }
        if (parsed.state == ParseState::Error) return {Result::Error::Parse, parsed.error, {}};
    }
    return {Result::Error::Closed, "peer closed before response", {}};
}
Result Client::get(const std::string& url) { Request request; request.method = Method::Get; std::string host, target; std::uint16_t port = 0; if (!split_url(url, host, target, port)) return {Result::Error::InvalidUrl, "invalid http url", {}}; request.target = std::move(target); return do_request(request, host, port); }
Result Client::head(const std::string& url) { Request request; request.method = Method::Head; std::string host, target; std::uint16_t port = 0; if (!split_url(url, host, target, port)) return {Result::Error::InvalidUrl, "invalid http url", {}}; request.target = std::move(target); return do_request(request, host, port); }
Result Client::post(const std::string& url, std::string body, std::string content_type) {
    Request request; request.method = Method::Post; request.body = std::move(body); request.header.set("Content-Type", std::move(content_type));
    std::string host, target; std::uint16_t port = 0; if (!split_url(url, host, target, port)) return {Result::Error::InvalidUrl, "invalid http url", {}};
    request.target = std::move(target); return do_request(request, host, port);
}

Result Get(const std::string& url, std::chrono::milliseconds timeout) { return Client(timeout).get(url); }
Result Head(const std::string& url, std::chrono::milliseconds timeout) { return Client(timeout).head(url); }
Result Post(const std::string& url, std::string body, std::string content_type, std::chrono::milliseconds timeout) { return Client(timeout).post(url, std::move(body), std::move(content_type)); }
Request NewRequest(Method method, std::string target, std::string body) { Request request; request.method = method; request.target = std::move(target); request.body = std::move(body); return request; }

ResponseWriter::ResponseWriter(net::Socket::Ptr socket, std::chrono::milliseconds timeout)
    : m_socket(std::move(socket)), m_timeout(timeout) {}
// 把 iovec 数组完整写出：writev 允许只写出一部分，返回后要按已写字节
// 前进到下一个 iovec（必要时只推进当前段的基址）。
static bool write_all_vectors(net::SocketStream& stream, iovec* vectors, std::size_t count) {
    std::size_t first = 0;
    while (first < count) {
        const auto written = stream.writev(vectors + first, count - first);
        if (written <= 0) return false;
        auto remaining = static_cast<std::size_t>(written);
        while (first < count && remaining >= vectors[first].iov_len) {
            remaining -= vectors[first].iov_len;
            ++first;
        }
        if (first < count && remaining > 0) {
            vectors[first].iov_base =
                static_cast<char*>(vectors[first].iov_base) + remaining;
            vectors[first].iov_len -= remaining;
        }
    }
    return true;
}
bool ResponseWriter::start(Response response) {
    if (!m_socket || m_state != StreamState::Idle) return false;
    response.header.erase("Content-Length");
    response.header.set("Transfer-Encoding", "chunked");
    response.body.clear();
    // chunked 流没有 sendfile 路径：文件体必须丢弃，否则 to_string() 会
    // 因为 file->length 与 Transfer-Encoding 冲突而不补 Content-Length。
    response.file.reset();
    const auto wire = response.to_string(false);
    if (m_timeout != std::chrono::milliseconds::max() && !m_socket->set_send_timeout(m_timeout)) {
        m_state = StreamState::Failed;
        return false;
    }
    net::SocketStream stream(m_socket, false);
    if (!stream.write_all(wire.data(), wire.size())) {
        m_state = StreamState::Failed;
        return false;
    }
    m_state = StreamState::HeadersCommitted;
    return true;
}
bool ResponseWriter::write(std::string_view chunk) {
    if (!m_socket || m_state != StreamState::HeadersCommitted) return false;
    if (chunk.empty()) return true;
    // chunk 前缀（十六进制长度 + CRLF）直接写在栈上：原实现每块构造一次
    // std::ostringstream（locale/虚表/分配）再 str() 复制一次。
    char prefix[24];
    const auto converted = std::to_chars(prefix, prefix + 20, chunk.size(), 16);
    char* cursor = converted.ptr;
    *cursor++ = '\r';
    *cursor++ = '\n';
    static constexpr char kSuffix[] = "\r\n";
    // prefix / payload / CRLF 合成一次 writev：原实现是 3 次 send（3 次系统调用）。
    // SSE 每个事件一块，收益与事件数线性相关。
    iovec vectors[3]{};
    vectors[0].iov_base = prefix;
    vectors[0].iov_len = static_cast<std::size_t>(cursor - prefix);
    vectors[1].iov_base = const_cast<char*>(chunk.data());
    vectors[1].iov_len = chunk.size();
    vectors[2].iov_base = const_cast<char*>(kSuffix);
    vectors[2].iov_len = sizeof(kSuffix) - 1;
    net::SocketStream stream(m_socket, false);
    if (!write_all_vectors(stream, vectors, 3)) {
        m_state = StreamState::Failed;
        return false;
    }
    return true;
}
// 注意：ResponseWriter 没有用户态发送缓冲，每次 write 都直接落到
// SocketStream。因此 flush() 只用于查询"当前是否仍可写"，不执行任何
// 系统调用；叫 flush 是历史命名（R8）。
bool ResponseWriter::flush() noexcept { return m_socket && m_state == StreamState::HeadersCommitted; }
bool ResponseWriter::close() {
    if (!m_socket || m_state == StreamState::Closed || m_state == StreamState::Failed ||
        m_state == StreamState::Idle) return false;
    static constexpr char end[] = "0\r\n\r\n";
    net::SocketStream stream(m_socket, false);
    if (!stream.write_all(end, sizeof(end) - 1)) {
        m_state = StreamState::Failed;
        return false;
    }
    m_state = StreamState::Closed;
    return true;
}

namespace {

/// 文件体发送：open + sendfile 循环；返回 false 表示连接已不可继续。
/// @note `Socket::send_file` 内部已处理 EINTR/EAGAIN 与部分写，这里只累计进度。
bool send_file_body(const net::Socket::Ptr& socket, const FileBody& file) {
    if (file.length == 0) return true;
    const int fd = ::open(file.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    off_t offset = static_cast<off_t>(file.offset);
    std::uint64_t remaining = file.length;
    bool ok = true;
    while (remaining > 0) {
        const auto written =
            socket->send_file(fd, &offset, static_cast<std::size_t>(remaining));
        if (written.transferred <= 0) {
            ok = false;
            break;
        }
        remaining -= static_cast<std::uint64_t>(written.transferred);
    }
    (void)::close(fd);
    return ok;
}

/// 十六进制字符转数值；非法返回 -1。
int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

/// 百分号解码；出现非法转义或 NUL 返回 false（调用方按 400 处理）。
bool percent_decode(std::string_view input, std::string& output) {
    output.clear();
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        const char value = input[index];
        if (value == '%') {
            if (index + 2 >= input.size()) return false;
            const int high = hex_value(input[index + 1]);
            const int low = hex_value(input[index + 2]);
            if (high < 0 || low < 0) return false;
            const char decoded = static_cast<char>((high << 4) | low);
            if (decoded == '\0') return false;
            output.push_back(decoded);
            index += 2;
            continue;
        }
        if (value == '\0') return false;
        output.push_back(value);
    }
    return true;
}

/// 解码后的路径是否含 `..` 段；含则拒绝，避免目录穿越。
bool has_parent_segment(std::string_view path) noexcept {
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto end = path.find('/', begin);
        const auto length = (end == std::string_view::npos ? path.size() : end) - begin;
        if (path.substr(begin, length) == "..") return true;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return false;
}

/// 扩展名 → Content-Type；返回值指向静态字面量，未知类型回退 octet-stream。
const char* mime_type(std::string_view path) noexcept {
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return "application/octet-stream";
    const std::string_view extension = path.substr(dot);
    char lowered[16];
    if (extension.size() >= sizeof(lowered)) return "application/octet-stream";
    for (std::size_t index = 0; index < extension.size(); ++index) {
        lowered[index] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(extension[index])));
    }
    const std::string_view value(lowered, extension.size());
    if (value == ".html" || value == ".htm") return "text/html; charset=utf-8";
    if (value == ".css") return "text/css; charset=utf-8";
    if (value == ".js" || value == ".mjs") return "application/javascript; charset=utf-8";
    if (value == ".json") return "application/json; charset=utf-8";
    if (value == ".xml") return "application/xml; charset=utf-8";
    if (value == ".svg") return "image/svg+xml";
    if (value == ".png") return "image/png";
    if (value == ".jpg" || value == ".jpeg") return "image/jpeg";
    if (value == ".gif") return "image/gif";
    if (value == ".webp") return "image/webp";
    if (value == ".ico") return "image/x-icon";
    if (value == ".woff") return "font/woff";
    if (value == ".woff2") return "font/woff2";
    if (value == ".wasm") return "application/wasm";
    if (value == ".pdf") return "application/pdf";
    if (value == ".txt" || value == ".log") return "text/plain; charset=utf-8";
    if (value == ".mp4") return "video/mp4";
    if (value == ".mp3") return "audio/mpeg";
    return "application/octet-stream";
}

/// 秒级时间 → IMF-fixdate（Last-Modified 用）。
std::string http_date(std::time_t value) {
    std::tm tm{};
    if (::gmtime_r(&value, &tm) == nullptr) return {};
    char buffer[40];
    const std::size_t length =
        std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return std::string(buffer, length);
}

/// 文件元数据 → 强 ETag（mtime-size，十六进制）。
std::string make_etag(std::uint64_t size, std::time_t modified) {
    char buffer[32];
    std::string etag = "\"";
    const auto modified_end =
        std::to_chars(buffer, buffer + sizeof(buffer),
                      static_cast<unsigned long long>(modified), 16).ptr;
    etag.append(buffer, modified_end);
    etag.push_back('-');
    const auto size_end =
        std::to_chars(buffer, buffer + sizeof(buffer),
                      static_cast<unsigned long long>(size), 16).ptr;
    etag.append(buffer, size_end);
    etag.push_back('"');
    return etag;
}

/// `Range` 解析结果：不处理 / 单区间 / 不可满足（416）。
enum class RangeState { Ignore, Partial, Unsatisfiable };

RangeState parse_range(std::string_view value, std::uint64_t size,
                       std::uint64_t& first, std::uint64_t& length) {
    constexpr std::string_view prefix = "bytes=";
    if (value.rfind(prefix, 0) != 0) return RangeState::Ignore;
    value.remove_prefix(prefix.size());
    // 多区间（含逗号）不支持：按 RFC 允许的方式忽略 Range，返回 200 全量。
    if (value.empty() || value.find(',') != std::string_view::npos) {
        return RangeState::Ignore;
    }
    const auto dash = value.find('-');
    if (dash == std::string_view::npos) return RangeState::Ignore;
    const auto first_text = trim_view(value.substr(0, dash));
    const auto last_text = trim_view(value.substr(dash + 1));
    std::size_t parsed = 0;
    if (first_text.empty()) {
        // 后缀区间 `-N`：最后 N 字节。
        if (last_text.empty() || !parse_size(last_text, parsed)) return RangeState::Ignore;
        if (parsed == 0) return RangeState::Unsatisfiable;
        const std::uint64_t suffix = static_cast<std::uint64_t>(parsed);
        if (suffix >= size) {
            first = 0;
            length = size;
        } else {
            first = size - suffix;
            length = suffix;
        }
        return RangeState::Partial;
    }
    if (!parse_size(first_text, parsed)) return RangeState::Ignore;
    const std::uint64_t start = static_cast<std::uint64_t>(parsed);
    if (size == 0 || start >= size) return RangeState::Unsatisfiable;
    std::uint64_t end = size - 1;
    if (!last_text.empty()) {
        if (!parse_size(last_text, parsed)) return RangeState::Ignore;
        end = static_cast<std::uint64_t>(parsed);
        if (end < start) return RangeState::Ignore;
        if (end >= size) end = size - 1;
    }
    first = start;
    length = end - start + 1;
    return RangeState::Partial;
}

/// 生成纯文本错误响应（400/403/413/416）。
Response make_text_response(Status status, std::string_view message) {
    Response response;
    response.status = status;
    response.header.set("Content-Type", "text/plain; charset=utf-8");
    response.body.assign(message);
    return response;
}

}  // namespace

// ---------------------------------------------------------------------------
// Router：精确哈希 + 段 trie
// ---------------------------------------------------------------------------

struct Router::Impl {
    /// 段 trie 节点：静态子节点 + 至多一个参数子节点 + 当前节点的通配。
    struct Node {
        std::map<std::string, std::unique_ptr<Node>, std::less<>> children;
        /// 参数子节点：同一段位置允许并存多个不同参数名（按注册顺序回溯）。
        std::vector<std::pair<std::string, std::unique_ptr<Node>>> parameters;
        Handler handler;
        Handler wildcard;
        std::string wildcard_name;
        bool has_handler{false};
        bool has_wildcard{false};
    };

    /// 每个方法一棵 trie；数组下标 = Method 的枚举值（Delete..Patch = 0..8）。
    std::array<Node, 9> roots;
    /// 每个方法一张精确表；key 指向 `patterns` 中的稳定存储，查找零分配。
    std::array<std::unordered_map<std::string_view, Handler>, 9> exact;
    /// 精确路由 pattern 的稳定存储（deque 的引用在 push_back 后不变）。
    std::deque<std::string> patterns;
    std::size_t size{0};

    /// 递归匹配：静态子节点优先，失败回退参数子节点，最后尾部通配。
    const Handler* match_node(const Node& node, std::string_view path,
                              RouteParams& params) const {
        if (path.empty()) {
            if (node.has_handler) return &node.handler;
            if (node.has_wildcard) {
                if (!node.wildcard_name.empty()) params[node.wildcard_name] = std::string{};
                return &node.wildcard;
            }
            return nullptr;
        }
        const auto slash = path.find('/');
        const std::string_view segment =
            path.substr(0, slash == std::string_view::npos ? path.size() : slash);
        const std::string_view rest =
            slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
        if (!segment.empty()) {
            const auto child = node.children.find(segment);
            if (child != node.children.end()) {
                if (const Handler* found = match_node(*child->second, rest, params)) {
                    return found;
                }
            }
            for (const auto& entry : node.parameters) {
                const std::string& name = entry.first;
                const auto existing = params.find(name);
                const bool had_value = existing != params.end();
                std::string saved;
                if (had_value) saved = existing->second;
                params[name] = std::string(segment);
                if (const Handler* found = match_node(*entry.second, rest, params)) {
                    return found;
                }
                if (had_value) params[name] = std::move(saved);
                else params.erase(name);
            }
        }
        if (node.has_wildcard) {
            if (!node.wildcard_name.empty()) params[node.wildcard_name] = std::string(path);
            return &node.wildcard;
        }
        return nullptr;
    }
};

Router::Router() : m_impl(std::make_unique<Impl>()) {}
Router::~Router() = default;

std::size_t Router::size() const noexcept { return m_impl ? m_impl->size : 0; }
bool Router::empty() const noexcept { return size() == 0; }
void Router::clear() { m_impl = std::make_unique<Impl>(); }

void Router::add(Method method, std::string pattern, Handler handler) {
    if (!m_impl || method == Method::Unknown || pattern.empty() || !handler) return;
    const auto index = static_cast<std::size_t>(method);
    if (index >= m_impl->exact.size()) return;
    if (pattern.front() != '/') pattern.insert(pattern.begin(), '/');
    // 尾斜杠归一：`/api/items/` 与 `/api/items` 视为同一条路由（根路径除外）。
    if (pattern.size() > 1 && pattern.back() == '/') pattern.pop_back();

    std::vector<std::string> segments;
    if (pattern.size() > 1) {
        std::size_t begin = 1;
        for (;;) {
            const auto end = pattern.find('/', begin);
            const auto length = (end == std::string::npos ? pattern.size() : end) - begin;
            if (length > 0) segments.emplace_back(pattern, begin, length);
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }

    // 只有最后一段的 `*`/`*name` 是通配；中间段的 `*` 按字面量处理。
    bool wildcard = false;
    std::string wildcard_name;
    if (!segments.empty() && segments.back().front() == '*') {
        wildcard = true;
        wildcard_name = segments.back().substr(1);
        segments.pop_back();
    }
    bool dynamic = wildcard;
    for (const auto& segment : segments) {
        if (segment.size() > 1 && segment.front() == ':') {
            dynamic = true;
            break;
        }
    }

    if (!dynamic) {
        m_impl->patterns.push_back(std::move(pattern));
        const std::string_view key(m_impl->patterns.back());
        const auto inserted = m_impl->exact[index].emplace(key, std::move(handler));
        if (inserted.second) ++m_impl->size;
        return;
    }

    Impl::Node* node = &m_impl->roots[index];
    for (const auto& segment : segments) {
        if (segment.size() > 1 && segment.front() == ':') {
            const std::string name = segment.substr(1);
            Impl::Node* next = nullptr;
            for (auto& entry : node->parameters) {
                if (entry.first == name) {
                    next = entry.second.get();
                    break;
                }
            }
            if (next == nullptr) {
                node->parameters.emplace_back(name, std::make_unique<Impl::Node>());
                next = node->parameters.back().second.get();
            }
            node = next;
        } else {
            auto& child = node->children[segment];
            if (!child) child = std::make_unique<Impl::Node>();
            node = child.get();
        }
    }
    if (wildcard) {
        if (node->has_wildcard) return;
        node->has_wildcard = true;
        node->wildcard = std::move(handler);
        node->wildcard_name = std::move(wildcard_name);
        ++m_impl->size;
        return;
    }
    if (node->has_handler) return;
    node->has_handler = true;
    node->handler = std::move(handler);
    ++m_impl->size;
}

const Handler* Router::match(Method method, std::string_view path,
                             RouteParams* params) const {
    if (!m_impl || method == Method::Unknown) return nullptr;
    const auto index = static_cast<std::size_t>(method);
    if (index >= m_impl->exact.size()) return nullptr;
    const auto& table = m_impl->exact[index];
    const auto exact = table.find(path);
    if (exact != table.end()) return &exact->second;
    const auto& root = m_impl->roots[index];
    if (root.children.empty() && root.parameters.empty() && !root.has_wildcard &&
        !root.has_handler) {
        return nullptr;
    }
    std::string_view remaining = path;
    if (!remaining.empty() && remaining.front() == '/') remaining.remove_prefix(1);
    RouteParams collected;
    if (const Handler* handler = m_impl->match_node(root, remaining, collected)) {
        if (params) *params = std::move(collected);
        return handler;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// StaticFiles：open + sendfile + Range/304/HEAD
// ---------------------------------------------------------------------------

StaticFiles::StaticFiles(StaticFilesOptions options) : m_options(std::move(options)) {
    if (m_options.root.size() > 1 && m_options.root.back() == '/') {
        m_options.root.pop_back();
    }
    if (!m_options.prefix.empty() && m_options.prefix.front() != '/') {
        m_options.prefix.insert(m_options.prefix.begin(), '/');
    }
    if (m_options.prefix.size() > 1 && m_options.prefix.back() == '/') {
        m_options.prefix.pop_back();
    }
}

namespace {

/// 线程局部文件元数据缓存：同一工作线程重复请求同一文件时省掉 ETag 格式化
/// 与 strftime（静态文件吞吐的热路径）。size+mtime 相同即视为同一版本；
/// 缓存不改变语义（同样的输入本来就生成同样的 ETag/Last-Modified）。
struct CachedFileMeta {
    std::uint64_t size{0};
    std::time_t modified{0};
    std::string etag;
    std::string last_modified;
};

struct CachedFileMetaView {
    std::string_view etag;
    std::string_view last_modified;
};

CachedFileMetaView cached_file_meta(std::uint64_t size, std::time_t modified) {
    static constexpr std::size_t kSlots = 4;
    static thread_local std::array<CachedFileMeta, kSlots> slots;
    static thread_local std::size_t next = 0;
    for (const auto& slot : slots) {
        if (!slot.etag.empty() && slot.size == size && slot.modified == modified) {
            return {slot.etag, slot.last_modified};
        }
    }
    CachedFileMeta& slot = slots[next];
    next = (next + 1) % kSlots;
    slot.size = size;
    slot.modified = modified;
    slot.etag = make_etag(size, modified);
    slot.last_modified = http_date(modified);
    return {slot.etag, slot.last_modified};
}

}  // namespace

bool StaticFiles::serve(const Request& request, Response* response) const {
    if (response == nullptr || m_options.root.empty()) return false;
    if (request.method != Method::Get && request.method != Method::Head) return false;
    std::string_view raw_path = request.path_view();
    if (!m_options.prefix.empty()) {
        // 挂载前缀：`/static` 只匹配 `/static` 与 `/static/...`。
        if (raw_path.size() < m_options.prefix.size() ||
            raw_path.compare(0, m_options.prefix.size(), m_options.prefix) != 0) {
            return false;
        }
        raw_path.remove_prefix(m_options.prefix.size());
        if (raw_path.empty()) raw_path = "/";
        else if (raw_path.front() != '/') return false;
    }
    if (raw_path.empty() || raw_path.front() != '/') return false;

    // 快路径：没有 '%' 时不复制路径（静态文件请求绝大多数不需要解码）。
    std::string decoded_storage;
    std::string_view decoded;
    if (raw_path.find('%') == std::string_view::npos) {
        decoded = raw_path;
    } else {
        if (!percent_decode(raw_path, decoded_storage)) {
            *response = make_text_response(Status::BadRequest, "bad request\n");
            return true;
        }
        decoded = decoded_storage;
    }
    if (has_parent_segment(decoded)) {
        *response = make_text_response(Status::Forbidden, "forbidden\n");
        return true;
    }

    // 拼路径：一次 reserve，避免逐段 append 扩容。
    std::string full;
    const std::string_view relative =
        decoded == "/" ? std::string_view(m_options.index) : decoded.substr(1);
    full.reserve(m_options.root.size() + 1 + relative.size());
    full.append(m_options.root);
    if (full.empty() || full.back() != '/') full.push_back('/');
    if (decoded == "/") {
        if (m_options.index.empty()) return false;
        full.append(m_options.index);
    } else {
        full.append(decoded.substr(1));
    }

    struct stat status {};
    if (::stat(full.c_str(), &status) != 0) return false;
    if (S_ISDIR(status.st_mode)) {
        if (m_options.index.empty()) return false;
        if (!full.empty() && full.back() != '/') full.push_back('/');
        full.append(m_options.index);
        if (::stat(full.c_str(), &status) != 0) return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) return false;

    const auto file_size = static_cast<std::uint64_t>(status.st_size);
    if (file_size > m_options.max_bytes) {
        *response = make_text_response(Status::PayloadTooLarge, "file too large\n");
        return true;
    }

    const auto meta = cached_file_meta(file_size, status.st_mtime);
    const std::string_view etag = meta.etag;
    const std::string_view last_modified = meta.last_modified;

    // 条件请求：If-None-Match 优先于 If-Modified-Since（RFC 7232 §6）。
    if (m_options.enable_etag) {
        const std::string if_none_match = request.header.get("If-None-Match");
        if (!if_none_match.empty() &&
            (std::string_view(if_none_match) == etag || if_none_match == "*")) {
            *response = Response{};
            response->status = Status::NotModified;
            response->header.set("ETag", std::string(etag));
            if (m_options.enable_last_modified) {
                response->header.set("Last-Modified", std::string(last_modified));
            }
            return true;
        }
    }
    if (m_options.enable_last_modified) {
        const std::string if_modified_since = request.header.get("If-Modified-Since");
        std::chrono::system_clock::time_point since;
        if (!if_modified_since.empty() &&
            parse_http_time(if_modified_since, since) &&
            std::chrono::system_clock::to_time_t(since) >= status.st_mtime) {
            *response = Response{};
            response->status = Status::NotModified;
            if (m_options.enable_etag) response->header.set("ETag", std::string(etag));
            response->header.set("Last-Modified", std::string(last_modified));
            return true;
        }
    }

    std::uint64_t offset = 0;
    std::uint64_t length = file_size;
    Status code = Status::OK;
    if (m_options.enable_range) {
        const std::string range = request.header.get("Range");
        if (!range.empty()) {
            // If-Range：只支持 ETag 形式；不匹配时按 RFC 忽略 Range 返回全量。
            const std::string if_range = request.header.get("If-Range");
            const bool range_applies =
                if_range.empty() || std::string_view(if_range) == etag;
            if (range_applies) {
                std::uint64_t first = 0;
                std::uint64_t ranged_length = 0;
                const auto state = parse_range(range, file_size, first, ranged_length);
                if (state == RangeState::Unsatisfiable) {
                    *response = make_text_response(Status::RangeNotSatisfiable,
                                                   "range not satisfiable\n");
                    response->header.set("Content-Range",
                                         "bytes */" + std::to_string(file_size));
                    response->header.set("Accept-Ranges", "bytes");
                    return true;
                }
                if (state == RangeState::Partial) {
                    offset = first;
                    length = ranged_length;
                    code = Status::PartialContent;
                }
            }
        }
    }

    *response = Response{};
    response->status = code;
    response->header.reserve(8);
    response->header.set("Content-Type", mime_type(full));
    response->header.set("X-Content-Type-Options", "nosniff");
    response->header.set("Accept-Ranges",
                         m_options.enable_range ? "bytes" : "none");
    if (m_options.enable_etag) response->header.set("ETag", std::string(etag));
    if (m_options.enable_last_modified) {
        response->header.set("Last-Modified", std::string(last_modified));
    }
    if (code == Status::PartialContent) {
        response->header.set("Content-Range",
                             "bytes " + std::to_string(offset) + "-" +
                                 std::to_string(offset + length - 1) + "/" +
                                 std::to_string(file_size));
    }
    response->header.set("Content-Length", std::to_string(length));
    response->file = std::make_shared<FileBody>(FileBody{std::move(full), offset, length});
    return true;
}

Handler StaticFiles::handler() const {
    auto files = std::make_shared<StaticFiles>(*this);
    return [files](const Request& request) {
        Response response;
        if (!files->serve(request, &response)) {
            response.status = Status::NotFound;
            response.header.set("Content-Type", "text/plain; charset=utf-8");
            response.body = "not found\n";
        }
        return response;
    };
}

bool ServeStaticFile(const Request& request, const StaticFilesOptions& options,
                     Response* response) {
    return StaticFiles(options).serve(request, response);
}

Server::Server(net::Socket::Ptr listener) : m_listener(std::move(listener)) {}
void Server::route(Method method, std::string path, Handler handler) {
    m_router.add(method, std::move(path), std::move(handler));
}
void Server::set_fallback(Handler handler) { m_fallback = std::move(handler); }
/**
 * @brief 在一个已接受的连接上服务若干请求（keep-alive 循环）。
 * @param connection 已建立的连接（由 `Server` 的 accept 循环传入）。
 * @param max_requests 本连接最多服务的请求数（防单连接饿死其它连接）。
 *
 * ## 定位
 *
 * `Server` 的连接级入口，与客户端 `Client::do_request` 对称。它把
 * 「读请求 → 路由 → 调 handler → 写响应 → 决定是否继续」串成一个循环，
 * 是服务端**唯一的请求生命周期**。
 *
 * ## 步骤
 *
 * step1. 空连接直接返回；初始化 `Parser` 与 `pending`（跨 recv 的流水线剩余）；
 * step2. `for` 循环（上限 `max_requests`，且 `m_stopped` 为假）：
 *        用 `do { feed/recv } while (NeedMore)` 读出一个**完整**请求；
 *        收到 0 字节或解析错误就返回（连接结束）；
 * step3. `pending = take_remaining()`：同一 recv 里可能还有下一个请求的字节，
 *        留给下一轮循环；
 * step4. 路由：`Router` 按「精确哈希 → 参数段 → 尾部通配」匹配，未命中用
 *        `m_fallback`；命中时把参数写入 `request.path_values`；
 *        handler 抛异常时降级成 500 且**强制 close**；
 * step5. 序列化响应并 `write_all`；带 `Response::file` 时再用 `sendfile(2)`
 *        零拷贝发送文件体（HEAD 只回头）；写失败直接返回；
 *        `request.close || response.close` 为真则结束循环（关闭连接）。
 *
 * ## 并发/失败语义
 *
 * - **单连接单线程**：本函数在调用它的 Fiber/线程上顺序执行，不并发处理
 *   同一连接的多个请求（HTTP/1.1 的串行语义）；
 * - **`max_requests` 是公平性保护**：防止一个 keep-alive 连接长期占用 worker；
 * - **handler 异常必须转 500**：否则异常会穿过网络栈；
 *   同时设 `response.close = true`，因为此时协议状态可能已不可信；
 * - **`pending` 不能丢**：`take_remaining()` 返回的是下一次请求的开头，
 *   丢弃会导致流水线请求被静默吞掉；
 * - **写失败即返回**：对端已断，继续读只会浪费一次 syscall；
 * - `m_stopped` 是原子标志，`Server::stop()` 置位后本循环在下一轮退出。
 */
void Server::serve_connection(net::Socket::Ptr connection, std::size_t max_requests) {
    // step1: 空连接直接返回；准备解析器与流水线剩余缓冲。
    if (!connection) return;
    Parser parser; std::string pending;
    // 接收缓冲移出循环且不做零初始化：recv 会覆盖 [0, transferred)，
    // 只有这段会被读取。原来每请求一次 8 KB 栈清零（rep stos）。
    std::array<char, 8192> buffer;
    // step2: keep-alive 主循环，单连接串行处理，上限 max_requests。
    for (std::size_t i = 0; i < max_requests && !m_stopped.load(std::memory_order_acquire); ++i) {
        Request request; ParseResult parsed;
        // 读出一个完整请求：NeedMore 时继续 recv。
        do {
            if (!pending.empty()) { parsed = parser.feed(pending, request); pending.clear(); }
            else { auto count = connection->recv(buffer.data(), buffer.size()); if (count.transferred <= 0) return; parsed = parser.feed(std::string_view(buffer.data(), static_cast<std::size_t>(count.transferred)), request); }
            if (parsed.state == ParseState::Error) return;
        } while (parsed.state == ParseState::NeedMore);
        // step3: 同一 recv 里可能还有下一个请求，留给下一轮。
        pending = parser.take_remaining();
        // step4: Router 匹配（精确哈希 → 参数段 → 尾部通配），未命中用 fallback；
        // 命中时参数直接写入 request.path_values，handler 异常转 500 并强制 close。
        Response response;
        const Handler* selected = m_router.match(request.method, request.path_view(),
                                                 &request.path_values);
        // HEAD 是 GET 的无体版本：未单独注册 HEAD 时回退到同一路径的 GET 路由。
        if (selected == nullptr && request.method == Method::Head) {
            selected = m_router.match(Method::Get, request.path_view(),
                                      &request.path_values);
        }
        if (selected == nullptr) selected = &m_fallback;
        if (*selected) {
            try { response = (*selected)(request); }
            catch (...) { response = Response{}; response.status = Status::InternalError; response.close = true; }
        } else response.status = Status::NotFound;
        // step5: 写响应头；文件体走 sendfile(2)（HEAD 只回头）。
        const auto wire = response.to_string(request.method == Method::Head);
        net::SocketStream stream(connection, false);
        if (!stream.write_all(wire.data(), wire.size())) return;
        if (request.method != Method::Head && response.has_file()) {
            if (!send_file_body(connection, *response.file)) return;
        }
        if (request.close || response.close) return;
    }
}

}  // namespace go2cpp::http
