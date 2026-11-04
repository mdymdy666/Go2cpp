#pragma once

/**
 * @file http.hpp
 * @brief HTTP/1.x 消息、增量解析器、客户端和轻量服务端接口。
 * @details 解析器按字节流工作，支持半包、粘包、Content-Length、chunked 和
 *          trailer，并保留一条消息之后的剩余字节。HTTP/2、HTTP/3、升级协议
 *          不在本模块范围内；超过限制或冲突的 CL/TE 会明确返回错误。
 *
 * ## 模块定位
 * HTTP/1.x 的应用层实现：消息模型（`Request`/`Response`/`Headers`）、增量解析器、
 * 最小客户端与轻量服务端。只依赖 `net::Socket`，不依赖调度器 —— 普通线程或
 * `IOManager` 的 Fiber 里都能用。
 *
 * ## 本文件定义什么
 * - 协议常量与名字转换：`Method`、`Status`、`method_name()`/`parse_method()`/`status_text()`；
 * - 消息模型：`Headers`（保留重复值、大小写不敏感）、`Request`、`Response`、
 *   `FileBody`（文件体，由 `Server` 用 `sendfile(2)` 发送）；
 * - 解析：`ParserLimits`、`ParseState`、`ParseResult`、`Parser`；
 * - 客户端：`Result`、`Client` 与 `Get`/`Post`/`Head` 便捷入口；
 * - 服务端：`Handler`、`ResponseWriter`（分块流式响应）、`Server`；
 * - 路由：`Router`（精确哈希 + 参数/尾部通配 trie）；
 * - 静态文件：`StaticFiles`（sendfile + Range/304/HEAD）。
 *
 * ## 阅读建议
 * 先看 `Parser` 的三态与 `feed()` 的步骤（半包/粘包/Content-Length/chunked 都在这里），
 * 再看 `Client::do_request()` 如何串「连接 -> 写请求 -> 读响应」，
 * 最后看 `Server::serve_connection()` 的读侧闭环与 `ResponseWriter` 的写侧。
 *
 * ## 边界与陷阱
 * - 解析器**有状态**：一次 `feed()` 可以只喂半包，必须持续喂到 Complete；
 * - 收到 `Error` 后应丢弃该 Parser（`reset()` 或重建），不要继续复用；
 * - 响应若无 Content-Length 且非 chunked，body 由连接 EOF 定界，需要 `finish()`；
 * - `Client` 的 socket 不跨请求复用；`Server::serve_connection()` 不设 recv 超时；
 * - `Response::file` 非空时 body 不参与序列化：`to_string()` 只回头，文件体由
 *   Server 发送；`ResponseWriter`（chunked 流）会忽略文件体；
 * - HEAD 未注册时路由回退到同路径 GET（`StaticFiles` 因此可直接用于 HEAD）。
 */

#include "go2cpp/network/socket.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace go2cpp::http {

enum class Method { Delete, Get, Head, Post, Put, Connect, Options, Trace, Patch, Unknown };
enum class Status : int { Continue = 100, SwitchingProtocols = 101, OK = 200, Created = 201,
                          NoContent = 204, PartialContent = 206, Found = 302, NotModified = 304,
                          BadRequest = 400, Unauthorized = 401, Forbidden = 403,
                          NotFound = 404, MethodNotAllowed = 405, RequestTimeout = 408,
                          Conflict = 409, PayloadTooLarge = 413, RangeNotSatisfiable = 416,
                          InternalError = 500, NotImplemented = 501, BadGateway = 502,
                          ServiceUnavailable = 503, GatewayTimeout = 504 };
std::string method_name(Method method);
Method parse_method(std::string_view value);
std::string status_text(Status status);

/** @brief 保留重复值且键大小写不敏感的 HTTP 头。 */
/**
 * @brief 保留重复值且键大小写不敏感的 HTTP 头。
 *
 * ## 定位
 * 被 `Request` / `Response` 持有；`websocket.hpp` 与 `sse.hpp` 也用它读取/设置
 * Upgrade、Sec-WebSocket-*、Content-Type 等头。
 *
 * ## 不变量
 * - 名字比较大小写不敏感，但存储保留调用方写入的原始大小写；
 * - 重复头不合并（`values()` 返回全部），`get()` 只返回第一个；
 * - `entries()` 的顺序就是写入顺序。
 */
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
    /// 预留头容量：解析器按 CRLF 计数一次性预留，避免逐头扩容搬移。
    void reserve(std::size_t count) { m_entries.reserve(count); }

private:
    static bool equal_name(std::string_view left, std::string_view right) noexcept;
    /// 头字段存储（保序、可重复）；add/set/erase/clear 写，entries() 只读。
    std::vector<Entry> m_entries;
};

/** @brief HTTP 请求消息，body 为已经读取的有限缓冲。 */
/**
 * @brief HTTP 请求消息，body 为已经读取的有限缓冲。
 *
 * ## 定位
 * `Parser::feed()` 的产出、`Client` / `Server` 的输入输出、WebSocket 握手校验的输入。
 *
 * ## 不变量
 * - `target` 是 origin-form（以 '/' 开头）；absolute-form 由 `Client` 解析后再填入；
 * - `body` 是**已完整读取**的有限缓冲（不是流）；`close` 表示对端要求关闭连接。
 */
class Request final {
public:
    /// 请求方法；Parser 写入，业务/路由读取。
    Method method{Method::Get};
    /// 请求目标（origin-form）；Parser 写入，path()/query() 解析它。
    std::string target{"/"};
    /// 协议版本；Parser 写入，to_string() 原样回写。
    std::string version{"HTTP/1.1"};
    /// 头部；Parser 写入、业务读取（含 WebSocket Upgrade 校验）。
    Headers header;
    /// 尾部（chunked 的 trailer 段）；Parser 在消息结束时填充。
    Headers trailer;
    /// 已完整读取的 body；Parser 写入，Handler/业务读取。
    std::string body;
    /// 路由参数占位（供上层填充）；`path_value()` 读取。
    std::map<std::string, std::string> path_values;
    /// 是否应在本次消息后关闭连接；Parser 依据 Connection 头设置。
    bool close{false};

    std::string path() const;
    /// 路径视图（截到 '?' 之前）：指向 target，生命周期与 Request 相同。
    /// 路由匹配用它可避免每个候选路由都分配一次 std::string。
    std::string_view path_view() const noexcept {
        const auto pos = target.find('?');
        return pos == std::string::npos ? std::string_view(target)
                                        : std::string_view(target.data(), pos);
    }
    std::string query() const;
    void add_cookie(std::string name, std::string value);
    std::string path_value(std::string_view name) const;
    std::string to_string() const;
};

/**
 * @brief 响应体为文件时的描述：`Server` 用 `sendfile(2)` 零拷贝发送。
 *
 * ## 定位
 * 静态文件服务的产出；与 `Response::body` 互斥，`body` 非空时以 `file` 为准。
 *
 * ## 不变量
 * - `length` 是**要发送的字节数**，不是文件大小（Range 时小于文件大小）；
 * - `offset + length` 必须落在文件范围内，发送前不再做边界修正；
 * - `Server` 每次发送时 `open()` 一次，不缓存 fd（响应可拷贝、可跨线程）。
 */
struct FileBody {
    /// 文件路径（绝对路径或相对工作目录）；发送时 `open(O_RDONLY)`。
    std::string path;
    /// 起始偏移（Range 请求的 first 字节）。
    std::uint64_t offset{0};
    /// 发送字节数；0 表示空文件体（只发头）。
    std::uint64_t length{0};
};

/** @brief HTTP 响应消息。 */
/**
 * @brief HTTP 响应消息。
 *
 * ## 定位
 * `Parser::feed()` 的产出、`Client::do_request()` 的结果体、`ResponseWriter::start()` 的输入。
 *
 * ## 不变量
 * `to_string()` 的输出与 `close` 一致：`close == true` 时会带 `Connection: close`。
 * `file` 非空时 `body` 不参与序列化，`Content-Length` 由 `file->length` 补齐。
 */
class Response final {
public:
    /// 响应状态码；业务/Handler 设置，to_string() 读取。
    Status status{Status::OK};
    /// 响应的协议版本；业务可改，to_string() 原样输出。
    std::string version{"HTTP/1.1"};
    /// 响应头；业务/Handler 写入，to_string() 序列化。
    Headers header;
    /// 响应尾（chunked trailer）；to_string() 在终止块前输出。
    Headers trailer;
    /// 响应 body；`head_request` 为真时 to_string() 仍会算 Content-Length 但不输出。
    /// 与 `file` 互斥：`file` 非空时本字段被忽略（文件体不进入内存）。
    std::string body;
    /// 文件体（静态文件/Range）：非空时 `Server` 走 `sendfile(2)`，不读进内存。
    std::shared_ptr<FileBody> file;
    /// 是否在响应后关闭连接；to_string() 据此输出 Connection: close。
    bool close{false};

    /// 是否携带文件体；`Server` 的发送路径据此选择 sendfile 或普通 write。
    bool has_file() const noexcept { return static_cast<bool>(file); }
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
/**
 * @brief 一次 `feed()` 的结果。
 *
 * ## 定位
 * 被 `Parser::feed()` / `Parser::finish()` 返回；`Client`、`Server` 与 WebSocket 握手
 * 都靠它判断「还要更多字节 / 完成 / 失败」。
 *
 * ## 不变量
 * `state == Complete` 时 `consumed > 0`；`state == Error` 时 `error` 非空。
 * `operator bool()` 只排除 Error（NeedMore 仍为 true）。
 */
struct ParseResult {
    ParseState state{ParseState::NeedMore};
    std::size_t consumed{0};
    std::string error;
    explicit operator bool() const noexcept { return state != ParseState::Error; }
};

/**
 * @brief 可重复使用的 HTTP 请求/响应增量解析器。
 * @note 解析失败返回 `ParseState::Error`，但 Parser **不会**因此进入不可变
 *       终态：继续 feed() 会重新解析缓冲区。调用方一旦收到 Error 应丢弃
 *       该 Parser（或调用 reset()），不要复用（R11）。
 *
 * ## 定位
 * 被 `Client`（解析响应）与 `Server::serve_connection()`（解析请求）使用；
 * `net::WebSocket::Connect()` 也用它解析 101 响应。
 *
 * ## 状态机
 * `ParseState`：`NeedMore` -(feed)-> `Complete` 或 `Error`；`Error` 不是终态，
 * 但调用方应 `reset()` 或丢弃 Parser（继续 feed 会从缓冲区重新解析）。
 *
 * ## 不变量
 * - 缓冲区只追加不丢字节：`consumed` 之前的字节已交付，之后的留给下一条消息；
 * - 超过 `ParserLimits` 立即 Error，不做「尽力解析」；
 * - `m_expected_body` / `m_chunked` / `m_response_close_delimited` 互斥地决定 body 定界方式。
 */
class Parser final {
public:
    explicit Parser(ParserLimits limits = {});
    void reset();
    /**
     * @brief 输入一段网络字节并尝试解析出一条完整消息。
     *
     * ## 步骤
     * step1. 追加字节到内部缓冲并按 `ParserLimits` 检查总量；
     * step2. 解析起始行（请求行或状态行），不完整则返回 `NeedMore`；
     * step3. 解析头部直到空行，处理 Content-Length / Transfer-Encoding / trailer 标记；
     * step4. 按定界方式收 body（定长 / chunked / EOF），凑齐后返回 `Complete`；
     * step5. 保留多余字节，供下一条消息或 `take_remaining()` 使用。
     *
     * ## 失败语义
     * 冲突的 CL/TE、超限、非法行都返回 `Error`；`error` 字段带中文原因。
     */
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
    /// 解析上限快照；构造时固定，feed_impl() 用它做所有超限判断。
    ParserLimits m_limits;
    /// 未消费字节；feed() 追加、feed_impl() 按 consumed 擦除。
    std::string m_buffer;
    /// 头部是否已解析（true 时后续 feed 跳过头部重解析，直接进 body 阶段）。
    bool m_header_parsed{false};
    /// 头部解析结果快照：body 起始偏移 / Content-Length / chunked / 是否无 body。
    std::size_t m_body_start_offset{0};
    std::size_t m_content_length{0};
    bool m_has_length{false};
    bool m_no_body{false};
    /// 是否 chunked 传输；由 Transfer-Encoding 头设置，决定 body 解析路径。
    bool m_chunked{false};
    /// 当前消息是否已完整交付；`finished()` 只读，reset() 清零。
    bool m_finished{false};
    /// chunked 已解码 body；跨 feed 累积，完成时 move 给 Response/Request。
    /// 与 m_chunk_scan_pos 一起把增量解析从 O(n²) 降到 O(n)。
    std::string m_chunked_body;
    /// 下一个 chunk-size 行在 m_buffer 中的绝对偏移；0 表示尚未开始。
    /// NeedMore 不清空缓冲，因此该偏移在后续 feed 中仍然有效。
    std::size_t m_chunk_scan_pos{0};
    /// 当前响应对应 HEAD 请求；`set_response_to_head()` 设置，解析时跳过 body。
    bool m_response_to_head{false};
    // 响应没有长度/分块标记时，body 由连接 EOF 定界；保存头后偏移，
    // 让 finish() 在 EOF 到达后一次性交付 body。
    bool m_response_close_delimited{false};
    /// EOF 定界响应的 body 起点偏移；finish() 用它一次性交付 body。
    std::size_t m_body_start{0};
    /// 最近一次解析错误；Error 时由 ParseResult.error 带出。
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

/**
 * @brief 把一次 IO 失败的 errno 归类为 `Result::Error`。
 * @param normal [in] 阶段默认错误（Connect/Send/Receive）。
 * @param error [in] 失败 errno 快照；0 表示无错误。
 * @return 归类结果；`Closed` 表示连接已结束，调用方应重建连接而不是重试。
 */
Result::Error classify_io_error(Result::Error normal, int error) noexcept;

/** @brief 基于 Socket 的最小 HTTP/1.1 客户端，连接不跨协程共享。 */
/**
 * @brief 基于 Socket 的最小 HTTP/1.1 客户端，连接不跨协程共享。
 *
 * ## 定位
 * 被 `go2cpp::Get/Post/Head` 等自由函数与业务代码使用；每次 `do_request()`
 * 自行建连（不复用连接池）。
 *
 * ## 不变量
 * - 错误通过 `Result::error` 返回，不抛异常（可安全跨 Fiber/线程传递结果）；
 * - `m_timeout` 覆盖 connect/send/recv 的绝对 deadline。
 */
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
    /// 超时；构造时固定，do_request() 用它做 connect/send/recv 的 deadline。
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
/**
 * @brief HTTP/1.1 分块流式响应写入器（Idle/HeadersCommitted/Closed/Failed）。
 *
 * ## 定位
 * 被 `Server::serve_connection()` 与 `sse::Stream` 使用（SSE 复用它的 chunked 写入）。
 *
 * ## 状态机
 * `Idle` -(start)-> `HeadersCommitted` -(write)-> `HeadersCommitted` -(close)-> `Closed`；
 * 任一写失败 -> `Failed`（不可复用）。
 *
 * ## 不变量
 * - 只有 `HeadersCommitted` 才允许 `write()`；`close()` 幂等；
 * - 所有写入处理 partial write，且不在用户锁内做阻塞 IO。
 */
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
    /// 底层已连接 socket（所有权在本类）；start/write/close 都经它写。
    net::Socket::Ptr m_socket;
    /// 写入超时；构造时固定，ResponseWriter 的每次写都用它。
    std::chrono::milliseconds m_timeout;
    /// 当前写入状态；start/write/close 迁移，state() 只读。
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

/// 路由参数表：键为 `:name` 中的 name，值是路径段原文（未做百分号解码）。
using RouteParams = std::map<std::string, std::string>;

/**
 * @brief 精确哈希 + 段 trie 的路由表：注册期可写，匹配期只读。
 *
 * ## 定位
 * `http::Server::route()` 与 `bento::HttpServer::Route()` 的底层匹配结构，
 * 也供业务在 `OnRequest` 之上自行做二级路由。注册集中在启动期，匹配在每请求
 * 热路径，因此匹配路径**不加锁**；运行期改路由需调用方自行同步（`bento::HttpServer`
 * 用路由锁包住注册与匹配）。
 *
 * ## 匹配优先级
 * 1. **静态精确**（`/api/items`）：每方法一张 `unordered_map<string_view, Handler>`，
 *    平均 O(1)，替代原实现的线性扫描（64 条路由时平均 32 次字符串比较）；
 * 2. **参数段**（`/api/items/:id`）：按 `/` 切段的 trie，逐段下降；
 * 3. **尾部通配**（`/static/` 后接 `*` 或 `*path`，或整条路由就是 `*`）：
 *    trie 的叶子节点。
 * 同一节点上静态子节点优先于参数子节点；同一段位置允许并存多个不同参数名
 * （`/users/:id` 与 `/users/:uid/posts/:pid` 可同时注册），按注册顺序回溯。
 *
 * ## 不变量
 * - 同一 `(method, pattern)` 重复注册保留第一次的 handler（与原线性匹配一致）；
 * - 参数段必须为非空路径段；`*` 通配匹配剩余全部（含空串）路径；
 * - `match()` 返回的 `Handler*` 指向表内存储，生命周期与 Router 相同。
 */
class Router final {
public:
    Router();
    ~Router();
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    /**
     * @brief 注册一条路由。
     * @param method 请求方法；`Method::Unknown` 不参与匹配。
     * @param pattern 以 '/' 开头的模式：静态段、`:name` 参数段、尾部 `*`/`*name`
     *        通配；单独的 `*` 表示任意路径。
     * @param handler 命中后的处理器；空 handler 不注册（与「未命中」等价）。
     */
    void add(Method method, std::string pattern, Handler handler);

    /**
     * @brief 按 method + path 匹配。
     * @param path 请求路径（不含查询串），通常是 `Request::path_view()`。
     * @param params 命中时写入参数表；未命中不修改。
     * @return 命中返回表内 handler 指针；未命中返回 nullptr。
     */
    const Handler* match(Method method, std::string_view path,
                         RouteParams* params = nullptr) const;

    std::size_t size() const noexcept;
    bool empty() const noexcept;
    void clear();

private:
    struct Impl;
    /// 路由表实现（精确哈希 + trie）；头文件不暴露节点细节。
    std::unique_ptr<Impl> m_impl;
};

/// 静态文件服务配置；构造期快照，运行期不再改变。
struct StaticFilesOptions {
    /// 文档根目录（绝对路径或相对工作目录）；空表示禁用。
    std::string root;
    /// 挂载前缀（如 `/static`）：请求路径先剥离它再映射到 root；空表示原样映射。
    std::string prefix;
    /// 目录请求返回的默认文件；空表示目录请求不处理（返回 false）。
    std::string index{"index.html"};
    /// 单文件上限（字节）；超过返回 413。
    std::uint64_t max_bytes{8ull * 1024 * 1024};
    /// 是否支持 `Range: bytes=`（单区间）；关闭时忽略 Range 返回 200。
    bool enable_range{true};
    /// 是否生成 ETag / 处理 `If-None-Match`（304）。
    bool enable_etag{true};
    /// 是否输出 Last-Modified / 处理 `If-Modified-Since`（304）。
    bool enable_last_modified{true};
};

/**
 * @brief 静态文件服务：`open` + `sendfile(2)` 零拷贝 + Range/304/HEAD。
 *
 * ## 定位
 * 把目录挂到路由上的可复用组件（替代示例里 `read` 整个文件再 `body` 的做法）。
 * 返回的 `Response::file` 由 `Server` 用 `sendfile(2)` 发送，数据不经过用户态：
 * 每请求只 `stat`/`open` 一次元数据，文件内容不进进程地址空间。
 *
 * ## 安全边界
 * - 请求路径先做百分号解码，再按 `/` 切段；任一段为 `..` 直接 403；
 * - 拒绝空路径、NUL、非法百分号编码、非 origin-form（不以 '/' 开头）；
 * - 目标路径由 root + 相对路径拼成，且相对路径不含 `..`/绝对路径，
 *   因此无法越过 root；root 内的 symlink 原样跟随（与 nginx
 *   `disable_symlinks off` 的默认行为一致）。
 *
 * ## 返回语义
 * - true：已生成响应（200/206/304/400/403/413/416）；
 * - false：路径不存在或方法不是 GET/HEAD，交上层继续路由（上层可回 404）。
 */
class StaticFiles final {
public:
    explicit StaticFiles(StaticFilesOptions options);

    /// 尝试用静态文件响应请求；`response` 非空。
    bool serve(const Request& request, Response* response) const;

    /// 生成挂到 `Router`/`Server` 的 Handler：未命中时返回 404。
    Handler handler() const;

    const StaticFilesOptions& options() const noexcept { return m_options; }

private:
    /// 配置快照；构造时规范化 root（去掉尾部的 '/'）。
    StaticFilesOptions m_options;
};

/// 便捷入口：等价于 `StaticFiles(options).serve(request, response)`。
bool ServeStaticFile(const Request& request, const StaticFilesOptions& options,
                     Response* response);
/** @brief 可替换的线程/Fiber HTTP 服务端核心，监听与调度由调用方控制。 */
/**
 * @brief 可替换的线程/Fiber HTTP 服务端核心，监听与调度由调用方控制。
 *
 * ## 定位
 * 被示例/测试作为轻量服务端使用；监听、accept 与 Fiber 调度由调用方控制，
 * 本类只负责「一条连接上的请求-响应循环」。
 *
 * ## 不变量
 * - 每个连接只在一个 Fiber 中串行处理（`serve_connection()`）；
 * - 路由 = `Router`（精确哈希 + 参数/通配 trie）；未命中走 `m_fallback`；
 * - 响应带 `Response::file` 时走 `sendfile(2)`，不把文件读进内存。
 */
/// @brief HTTP 服务端连接级超时（H-05）。服务端默认必须有限，否则慢连接
///        会永久占用 Fiber + socket。
struct ServerOptions {
    /// 读取一个完整请求（含 keep-alive 空闲等待）的总预算，默认 10s。
    std::chrono::milliseconds read_timeout{std::chrono::seconds(10)};
    /// 写出一个完整响应（含 sendfile 文件体）的总预算，默认 10s。
    std::chrono::milliseconds write_timeout{std::chrono::seconds(10)};
};

class Server final {
public:
    /**
     * @param listener 监听 socket（不做 accept，由调用方负责）。
     * @param options 连接级超时；非法值（<=0）在 serve_connection 中回落到
     *        默认值并告警一次，不会被静默当成「立即超时」。
     */
    explicit Server(net::Socket::Ptr listener, ServerOptions options = {});

    /**
     * @brief 注册路由。
     * @param method 请求方法。
     * @param path 支持静态路径、`:name` 参数段与尾部 `*` 通配（见 `Router::add`）。
     * @param handler 命中后的处理器。
     */
    void route(Method method, std::string path, Handler handler);
    void set_fallback(Handler handler);
    /**
     * @brief 同步处理一个连接；每个连接只在一个 Fiber 中串行使用。
     * @note 本函数**不设置 recv/write 超时**（走无限等待），连接级超时需由
     *       调用方在 handler 内或上层负责；对比 `http::Client` 有覆盖
     *       connect/send/recv 的绝对 deadline。
     */
    void serve_connection(net::Socket::Ptr connection,
                          std::size_t max_requests = 100);
    /// @brief 替换连接级超时；任一字段 <=0 返回 false 且保持原配置。
    bool set_options(ServerOptions options);
    const ServerOptions& options() const noexcept { return m_options; }
    void stop() noexcept { m_stopped = true; }

private:
    /// 监听 socket；由构造注入，供调用方 accept 后配合 serve_connection()。
    net::Socket::Ptr m_listener;
    /// 路由表（精确哈希 + 参数/通配 trie）；`route()` 注册，服务时按优先级匹配。
    Router m_router;
    /// 兜底 handler；未命中路由时调用，`set_fallback()` 设置。
    Handler m_fallback;
    /// 连接级超时；`set_options()` 替换，默认读/写各 10s。
    ServerOptions m_options;
    /// 停止标记；`stop()` 置位，服务循环据此退出（原子量支持跨线程停止）。
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