#pragma once

/**
 * @file http.hpp
 * @brief HTTP/1.x 消息、增量解析器、客户端和轻量服务端接口。
 * @details 解析器按字节流工作，支持半包、粘包、Content-Length、chunked 和
 *          trailer，并保留一条消息之后的剩余字节。HTTP/2、HTTP/3、升级协议
 *          不在本模块范围内；超过限制或冲突的 CL/TE 会明确返回错误。
 */

#include "go2cpp/network/socket.hpp"

#include <chrono>
#include <cstddef>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::http {

enum class Method { Delete, Get, Head, Post, Put, Connect, Options, Trace, Patch, Unknown };
enum class Status : int { Continue = 100, SwitchingProtocols = 101, OK = 200, Created = 201, NoContent = 204, Found = 302,
                          BadRequest = 400, Unauthorized = 401, Forbidden = 403,
                          NotFound = 404, MethodNotAllowed = 405, RequestTimeout = 408,
                          Conflict = 409, PayloadTooLarge = 413, InternalError = 500,
                          NotImplemented = 501, BadGateway = 502, ServiceUnavailable = 503,
                          GatewayTimeout = 504 };
std::string method_name(Method method);
Method parse_method(std::string_view value);
std::string status_text(Status status);

/** @brief 保留重复值且键大小写不敏感的 HTTP 头。 */
class Headers final {
public:
    using Entry = std::pair<std::string, std::string>;
    void add(std::string name, std::string value);
    void set(std::string name, std::string value);
    bool erase(std::string_view name);
    bool has(std::string_view name) const;
    std::string get(std::string_view name, std::string_view fallback = {}) const;
    std::vector<std::string> values(std::string_view name) const;
    const std::vector<Entry>& entries() const noexcept { return m_entries; }
    void clear() noexcept { m_entries.clear(); }

private:
    static bool equal_name(std::string_view left, std::string_view right) noexcept;
    std::vector<Entry> m_entries;
};

/** @brief HTTP 请求消息，body 为已经读取的有限缓冲。 */
class Request final {
public:
    Method method{Method::Get};
    std::string target{"/"};
    std::string version{"HTTP/1.1"};
    Headers header;
    Headers trailer;
    std::string body;
    std::map<std::string, std::string> path_values;
    bool close{false};

    std::string path() const;
    std::string query() const;
    void add_cookie(std::string name, std::string value);
    std::string path_value(std::string_view name) const;
    std::string to_string() const;
};

/** @brief HTTP 响应消息。 */
class Response final {
public:
    Status status{Status::OK};
    std::string version{"HTTP/1.1"};
    Headers header;
    Headers trailer;
    std::string body;
    bool close{false};
    void set_cookie(std::string name, std::string value,
                    std::string attributes = {});
    void redirect(std::string location, Status code = Status::Found);
    std::string to_string(bool head_request = false) const;
};

/** @brief 解析器限制，防止恶意头和 body 耗尽内存。 */
struct ParserLimits {
    std::size_t max_header_bytes{64 * 1024};
    std::size_t max_body_bytes{64 * 1024 * 1024};
    std::size_t max_line_bytes{16 * 1024};
    std::size_t max_trailer_bytes{16 * 1024};
};

enum class ParseState { NeedMore, Complete, Error };
struct ParseResult {
    ParseState state{ParseState::NeedMore};
    std::size_t consumed{0};
    std::string error;
    explicit operator bool() const noexcept { return state != ParseState::Error; }
};

/** @brief 可重复使用的 HTTP 请求/响应增量解析器。 */
class Parser final {
public:
    explicit Parser(ParserLimits limits = {});
    void reset();
    ParseResult feed(std::string_view bytes, Request& request);
    ParseResult feed(std::string_view bytes, Response& response);
    /** @brief 通知响应输入流已到 EOF，完成仅以连接关闭定界的响应。 */
    ParseResult finish(Response& response);
    /** @brief 指明当前响应对应 HEAD 请求，因此响应头后不读取 body。 */
    void set_response_to_head(bool value) noexcept { m_response_to_head = value; }
    std::string take_remaining();
    bool finished() const noexcept { return m_finished; }
    const ParserLimits& limits() const noexcept { return m_limits; }

private:
    ParseResult feed_impl(std::string_view bytes, bool response,
                          Request* request, Response* result);
    ParserLimits m_limits;
    std::string m_buffer;
    std::size_t m_expected_body{0};
    std::size_t m_chunk_remaining{0};
    bool m_chunked{false};
    bool m_finished{false};
    bool m_in_chunk_trailer{false};
    bool m_response_to_head{false};
    // 响应没有长度/分块标记时，body 由连接 EOF 定界；保存头后偏移，
    // 让 finish() 在 EOF 到达后一次性交付 body。
    bool m_response_close_delimited{false};
    std::size_t m_body_start{0};
    std::string m_error;
};

/** @brief RFC 3986 风格 URL 查询编码，保留重复键和空值。 */
std::string query_escape(std::string_view value);
bool query_unescape(std::string_view value, std::string& output);
/** @brief 将头名称规范化为首字母大写的 HTTP 形式。 */
std::string canonical_header_key(std::string_view value);
/** @brief 解析 HTTP/1.x 版本字符串。 */
bool parse_http_version(std::string_view value, int& major, int& minor);
/** @brief 解析 RFC 1123/RFC 850/ctime 风格时间，失败返回 false。 */
bool parse_http_time(std::string_view value, std::chrono::system_clock::time_point& output);
using QueryValues = std::map<std::string, std::vector<std::string>>;
std::string encode_query(const QueryValues& values);
bool decode_query(std::string_view value, QueryValues& output);

/** @brief HTTP 客户端结果，错误不会跨越 Fiber/线程边界抛出。 */
struct Result {
    enum class Error { None, InvalidUrl, Resolve, Connect, Timeout, Send, Receive,
                       Parse, Closed, Cancelled };
    Error error{Error::None};
    std::string message;
    std::shared_ptr<Response> response;
    bool ok() const noexcept { return error == Error::None && response != nullptr; }
};

/** @brief 基于 Socket 的最小 HTTP/1.1 客户端，连接不跨协程共享。 */
class Client final {
public:
    explicit Client(std::chrono::milliseconds timeout = std::chrono::seconds(30));
    Result do_request(const Request& request, const std::string& host,
                      std::uint16_t port = 80);
    Result get(const std::string& url);
    Result head(const std::string& url);
    Result post(const std::string& url, std::string body,
                std::string content_type = "application/octet-stream");
    std::chrono::milliseconds timeout() const noexcept { return m_timeout; }

private:
    std::chrono::milliseconds m_timeout;
};

/** @brief HTTP/1.1 分块流式响应写入器。
 * @details 只允许单线程/单 Fiber 持有；start 写响应头，write 写一个 chunk，
 *          close 写终止 chunk。所有底层写入都处理 partial write，并且不在
 *          用户锁内执行 Socket 阻塞操作。
 */
enum class StreamState {
    Idle,
    HeadersCommitted,
    Closed,
    /** @brief 底层写入失败，响应已不可继续复用。 */
    Failed
};
class ResponseWriter final {
public:
    explicit ResponseWriter(net::Socket::Ptr socket,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    bool start(Response response = {});
    bool write(std::string_view chunk);
    bool flush() noexcept;
    bool close();
    StreamState state() const noexcept { return m_state; }

private:
    net::Socket::Ptr m_socket;
    std::chrono::milliseconds m_timeout;
    StreamState m_state{StreamState::Idle};
};

/** @brief Go 风格的简单客户端入口，使用默认超时。 */
Result Get(const std::string& url, std::chrono::milliseconds timeout = std::chrono::seconds(30));
Result Head(const std::string& url, std::chrono::milliseconds timeout = std::chrono::seconds(30));
Result Post(const std::string& url, std::string body,
            std::string content_type = "application/octet-stream",
            std::chrono::milliseconds timeout = std::chrono::seconds(30));
Request NewRequest(Method method, std::string target, std::string body = {});

using Handler = std::function<Response(const Request&)>;
/** @brief 可替换的线程/Fiber HTTP 服务端核心，监听与调度由调用方控制。 */
class Server final {
public:
    explicit Server(net::Socket::Ptr listener);
    void route(Method method, std::string path, Handler handler);
    void set_fallback(Handler handler);
    /** @brief 同步处理一个连接；每个连接只在一个 Fiber 中串行使用。 */
    void serve_connection(net::Socket::Ptr connection,
                          std::size_t max_requests = 100);
    void stop() noexcept { m_stopped = true; }

private:
    net::Socket::Ptr m_listener;
    std::vector<std::tuple<Method, std::string, Handler>> m_routes;
    Handler m_fallback;
    std::atomic<bool> m_stopped{false};
};

}  // namespace go2cpp::http

namespace go2cpp {
using HttpRequest = http::Request;
using HttpResponse = http::Response;
using HttpHeaders = http::Headers;
using HttpParser = http::Parser;
using HttpClient = http::Client;
using HttpServer = http::Server;
using HttpResponseWriter = http::ResponseWriter;
}  // namespace go2cpp
