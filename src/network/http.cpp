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
std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}
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
std::string status_text(Status status) {
    switch (status) { case Status::Continue: return "Continue"; case Status::SwitchingProtocols: return "Switching Protocols"; case Status::OK: return "OK"; case Status::Created: return "Created"; case Status::Found: return "Found";
    case Status::NoContent: return "No Content"; case Status::BadRequest: return "Bad Request"; case Status::Unauthorized: return "Unauthorized";
    case Status::Forbidden: return "Forbidden"; case Status::NotFound: return "Not Found"; case Status::MethodNotAllowed: return "Method Not Allowed";
    case Status::RequestTimeout: return "Request Timeout"; case Status::Conflict: return "Conflict"; case Status::PayloadTooLarge: return "Payload Too Large";
    case Status::InternalError: return "Internal Server Error"; case Status::NotImplemented: return "Not Implemented";
    case Status::BadGateway: return "Bad Gateway"; case Status::ServiceUnavailable: return "Service Unavailable";
    case Status::GatewayTimeout: return "Gateway Timeout"; default: return ""; }
}

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
    if (method_name(method).empty() || !valid_request_target(target) ||
        !valid_http_version_token(version)) return {};
    std::ostringstream stream; stream << method_name(method) << " " << target << " " << version << "\r\n";
    for (const auto& item : header.entries()) stream << item.first << ": " << item.second << "\r\n";
    if (close && !header.has("Connection")) stream << "Connection: close\r\n";
    if (!body.empty() && !header.has("Content-Length")) stream << "Content-Length: " << body.size() << "\r\n";
    stream << "\r\n" << body; return stream.str();
}
void Response::set_cookie(std::string name, std::string value, std::string attributes) { std::string result = std::move(name) + "=" + std::move(value); if (!attributes.empty()) result += "; " + attributes; header.add("Set-Cookie", std::move(result)); }
void Response::redirect(std::string location, Status code) { status = code; header.set("Location", std::move(location)); }
std::string Response::to_string(bool head_request) const {
    if (!valid_http_version_token(version)) return {};
    const int code = static_cast<int>(status);
    const bool body_forbidden = (code >= 100 && code < 200) || code == 204 || code == 304;
    std::ostringstream stream; stream << version << " " << static_cast<int>(status) << " " << status_text(status) << "\r\n";
    for (const auto& item : header.entries()) {
        // 1xx/204/304 不允许实体 framing；过滤用户手工设置的 CL/TE，
        // 避免下游继续等待一个永远不会出现的 body。
        if (body_forbidden && (equal_ci(item.first, "Content-Length") ||
                               equal_ci(item.first, "Transfer-Encoding"))) {
            continue;
        }
        stream << item.first << ": " << item.second << "\r\n";
    }
    if (close && !header.has("Connection")) stream << "Connection: close\r\n";
    if (!body.empty() && !body_forbidden && !header.has("Content-Length"))
        stream << "Content-Length: " << body.size() << "\r\n";
    stream << "\r\n";
    if (!head_request && !body_forbidden) stream << body;
    return stream.str();
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
ParseResult Parser::feed_impl(std::string_view bytes, bool response, Request* request, Response* result) {
    if (m_finished) return {ParseState::Complete, 0, {}};
    if (m_buffer.size() > std::numeric_limits<std::size_t>::max() - bytes.size() ||
        (m_buffer.size() + bytes.size() > m_limits.max_header_bytes &&
         m_buffer.size() + bytes.size() - m_limits.max_header_bytes > m_limits.max_body_bytes))
        return {ParseState::Error, 0, "input too large"};
    try {
        m_buffer.append(bytes.data(), bytes.size());
    } catch (const std::exception&) {
        m_error = "input allocation failed";
        return {ParseState::Error, 0, m_error};
    }
    if (response && m_response_close_delimited) {
        if (m_buffer.size() - m_body_start > m_limits.max_body_bytes)
            return {ParseState::Error, bytes.size(), "body too large"};
        return {ParseState::NeedMore, bytes.size(), {}};
    }
    const auto header_end = find_header_end(m_buffer);
    if (header_end == std::string::npos) {
        if (m_buffer.size() > m_limits.max_header_bytes) { m_error = "header too large"; return {ParseState::Error, bytes.size(), m_error}; }
        return {ParseState::NeedMore, bytes.size(), {}};
    }
    if (header_end + 4 > m_limits.max_header_bytes) { m_error = "header too large"; return {ParseState::Error, bytes.size(), m_error}; }
    const std::string header_block = m_buffer.substr(0, header_end);
    std::size_t line_end = header_block.find("\r\n");
    if (line_end == std::string::npos || line_end > m_limits.max_line_bytes) return {ParseState::Error, bytes.size(), "invalid start line"};
    const std::string start_line = header_block.substr(0, line_end);
    std::istringstream start(start_line); std::string first, second, third, extra; start >> first >> second >> third;
    if (first.empty() || second.empty() || third.empty() || (!response && (start >> extra)))
        return {ParseState::Error, bytes.size(), "invalid start line"};
    if (response) {
        if (!valid_http_version_token(first) || second.empty()) return {ParseState::Error, bytes.size(), "invalid response"};
        if (second.size() != 3 || !std::all_of(second.begin(), second.end(),
                                                [](unsigned char c) { return std::isdigit(c); }))
            return {ParseState::Error, bytes.size(), "invalid status"};
        int code = 0; try { code = std::stoi(second); } catch (...) { return {ParseState::Error, bytes.size(), "invalid status"}; }
        result->version = first; result->status = static_cast<Status>(code);
    } else {
        request->method = parse_method(first); if (request->method == Method::Unknown || !valid_request_target(second) || !valid_http_version_token(third)) return {ParseState::Error, bytes.size(), "invalid request"};
        request->target = second; request->version = third;
    }
    std::size_t content_length = 0; bool has_length = false; bool chunked = false; bool has_transfer_encoding = false;
    for (std::size_t offset = line_end + 2; offset < header_block.size();) {
        const auto found_end = header_block.find("\r\n", offset);
        const auto end = found_end == std::string::npos ? header_block.size() : found_end;
        const auto colon = header_block.find(':', offset); if (colon == std::string::npos || colon >= end || colon == offset) return {ParseState::Error, bytes.size(), "invalid header"};
        const auto name = header_block.substr(offset, colon - offset); const auto value = trim(std::string_view(header_block).substr(colon + 1, end - colon - 1));
        if (name.find_first_of("\r\n\0 \t") != std::string::npos) return {ParseState::Error, bytes.size(), "invalid header name"};
        for (const unsigned char c : value) if (c < 0x20U && c != '\t') return {ParseState::Error, bytes.size(), "invalid header value"};
        if (equal_ci(name, "Content-Length")) { std::size_t parsed = 0; if (!parse_size(value, parsed) || (has_length && parsed != content_length)) return {ParseState::Error, bytes.size(), "invalid content length"}; content_length = parsed; has_length = true; }
        if (equal_ci(name, "Transfer-Encoding")) {
            if (has_transfer_encoding || !parse_transfer_encoding(value, chunked))
                return {ParseState::Error, bytes.size(), "unsupported transfer encoding"};
            has_transfer_encoding = true;
        }
        if (response) result->header.add(name, value); else request->header.add(name, value);
        if (found_end == std::string::npos) break;
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
bool ResponseWriter::start(Response response) {
    if (!m_socket || m_state != StreamState::Idle) return false;
    response.header.erase("Content-Length");
    response.header.set("Transfer-Encoding", "chunked");
    response.body.clear();
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
    std::ostringstream prefix;
    prefix << std::hex << chunk.size() << "\r\n";
    const auto prefix_wire = prefix.str();
    net::SocketStream stream(m_socket, false);
    if (!stream.write_all(prefix_wire.data(), prefix_wire.size())) {
        m_state = StreamState::Failed;
        return false;
    }
    if (!stream.write_all(chunk.data(), chunk.size())) {
        m_state = StreamState::Failed;
        return false;
    }
    static constexpr char suffix[] = "\r\n";
    if (!stream.write_all(suffix, 2)) {
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

Server::Server(net::Socket::Ptr listener) : m_listener(std::move(listener)) {}
void Server::route(Method method, std::string path, Handler handler) { m_routes.emplace_back(method, std::move(path), std::move(handler)); }
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
 * step4. 路由：按 (method, path) 精确匹配 `m_routes`，未命中用 `m_fallback`；
 *        handler 抛异常时降级成 500 且**强制 close**；
 * step5. 序列化响应并 `write_all`；写失败直接返回；
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
    // step2: keep-alive 主循环，单连接串行处理，上限 max_requests。
    for (std::size_t i = 0; i < max_requests && !m_stopped.load(std::memory_order_acquire); ++i) {
        Request request; std::array<char, 8192> buffer{}; ParseResult parsed;
        // 读出一个完整请求：NeedMore 时继续 recv。
        do {
            if (!pending.empty()) { parsed = parser.feed(pending, request); pending.clear(); }
            else { auto count = connection->recv(buffer.data(), buffer.size()); if (count.transferred <= 0) return; parsed = parser.feed(std::string_view(buffer.data(), static_cast<std::size_t>(count.transferred)), request); }
            if (parsed.state == ParseState::Error) return;
        } while (parsed.state == ParseState::NeedMore);
        // step3: 同一 recv 里可能还有下一个请求，留给下一轮。
        pending = parser.take_remaining();
        // step4: 精确匹配路由，未命中用 fallback；handler 异常转 500 并强制 close。
        Response response; Handler selected = m_fallback;
        for (const auto& [method, path, handler] : m_routes) if (method == request.method && path == request.path()) { selected = handler; break; }
        if (selected) {
            try { response = selected(request); }
            catch (...) { response = Response{}; response.status = Status::InternalError; response.close = true; }
        } else response.status = Status::NotFound;
        // step5: 写响应；close 标志决定是否结束本连接。
        const auto wire = response.to_string(request.method == Method::Head);
        net::SocketStream stream(connection, false);
        if (!stream.write_all(wire.data(), wire.size())) return;
        if (request.close || response.close) return;
    }
}

}  // namespace go2cpp::http
