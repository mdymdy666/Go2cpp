#pragma once

/**
 * @file http_server.hpp
 * @brief 基于 base `http`/`sse` 的最小 HTTP/SSE 服务端骨架。
 * @details 把 base 的 HTTP 解析器（`http::Parser`）、SSE 写入器（`sse::Stream`）
 *          与 `net::Socket` 组装成一个可 Listen/Start/Stop 的服务端：
 *          - 普通请求：`OnRequest` 收到已解析的 method/path/headers/body，
 *            返回 `http::Response`；
 *          - SSE 推流：`AsSse` 按 path 精确匹配，handler 直接持有 `sse::Stream`，
 *            可多次 `send()` 事件；
 *          - 生命周期：`Listen()` 只绑定端口，`Start()` 启动 accept/worker 线程，
 *            `Stop()` 优雅关闭（停 accept、关闭监听、唤醒并等待所有连接线程）。
 *
 * ## 并发模型
 *          默认 `Blocking`：一个 accept 线程 + 固定数量 worker 线程，每条连接
 *          由某个 worker 串行服务（HTTP/1.1 语义）。`NonBlocking` 只是预留枚举，
 *          当前 `Start()` 会返回 `HttpServerError::UnsupportedModel`，不会静默
 *          降级成阻塞模型。
 *
 * ## 已实现 / 未实现（骨架边界）
 *          已实现：GET/POST 等任意方法、请求头与 body、keep-alive、404、
 *          SSE 事件推流、优雅 Stop、并发 accept。
 *          未实现：TLS、HTTP/2、路由参数/通配符、请求体流式读取、每连接读超时、
 *          连接级限流；SSE handler 必须自己观察 `sse::Stream::send()` 的返回值
 *          （或 `HttpServer::IsStopping()`）来退出长循环，否则 Stop 只能靠
 *          socket shutdown 让写入失败。
 *
 * ## 线程安全
 *          `OnRequest/AsSse` 用路由锁保护，可在运行期从任意线程调用；`Start`
 *          与 `Stop` 不可并发调用（`Stop` 允许从另一个线程发起，但必须发生在
 *          `Start` 返回之后）。`Stop()` 与析构是幂等的。handler 在 worker 线程
 *          里执行，同一个 handler 可能被多个连接并发调用，业务状态需自行同步。
 *          不要在 handler 内调用 `Stop()`：实现会 detach 当前 worker 以防
 *          terminate，但 server 对象可能在 handler 返回前被调用方析构。
 *
 * ## 典型用法
 * @code
 * go2cpp::bento::HttpServer server;
 * server.OnRequest([](const go2cpp::http::Request& request) {
 *     go2cpp::http::Response response;
 *     response.body = "hello " + request.path();
 *     return response;
 * });
 * server.AsSse("/events", [](const go2cpp::http::Request&, go2cpp::sse::Stream& stream) {
 *     for (int i = 0; i < 3; ++i) {
 *         go2cpp::sse::Event event;
 *         event.event = "tick";
 *         event.data = std::to_string(i);
 *         if (!stream.send(event).ok()) break;
 *     }
 * });
 * auto listened = server.Listen("127.0.0.1:0");
 * if (!listened.ok()) { ... }
 * server.Start();
 * // ... server.Port() ...
 * server.Stop();
 * @endcode
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/io.hpp"
#include "go2cpp/network/sse.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace go2cpp::bento {

/// @brief `HttpServer` 操作失败的分类。
enum class HttpServerError {
    None,              ///< 成功。
    InvalidAddress,    ///< 监听地址无法解析。
    BindFailed,        ///< bind(2) 失败（端口被占用、权限不足等）。
    ListenFailed,      ///< listen(2) 失败。
    NotListening,      ///< 尚未 `Listen()` 就调用 `Start()`。
    AlreadyListening,  ///< 已经绑定过监听 socket。
    AlreadyRunning,    ///< 已经 `Start()`。
    UnsupportedModel,  ///< 请求了尚未实现的并发模型（NonBlocking）。
    ThreadFailure,     ///< 创建 accept/worker 线程失败。
};

/// @brief 错误分类的可读名称（英文标识，便于日志与断言）。
const char* HttpServerErrorName(HttpServerError error) noexcept;

/// @brief 不跨线程抛异常的操作结果。
struct HttpServerResult {
    HttpServerError error{HttpServerError::None};
    /// 系统错误码（errno）；仅 Bind/Listen 等系统调用失败时非 0。
    int system_error{0};
    std::string message;

    bool ok() const noexcept { return error == HttpServerError::None; }
    static HttpServerResult Success() { return {}; }
    static HttpServerResult Failure(HttpServerError error, std::string message,
                                    int system_error = 0);
};

/// @brief 并发模型；`NonBlocking` 目前只是预留值。
enum class HttpConcurrencyModel {
    Blocking,   ///< 一个 accept 线程 + 固定 worker 线程池（当前唯一实现）。
    NonBlocking ///< 预留：事件循环驱动，尚未实现。
};

/// @brief 服务端配置；字段在 `Listen()` 时快照，运行期不再改变。
struct HttpServerConfig {
    /// 并发模型；NonBlocking 会让 Start() 返回 UnsupportedModel。
    HttpConcurrencyModel model{HttpConcurrencyModel::Blocking};
    /// worker 线程数；0 按 1 处理。
    std::size_t worker_threads{4};
    /// accept 后待处理连接队列上限；超过时直接关闭新连接（骨架级背压）。
    std::size_t max_queued_connections{128};
    /// 单条连接最多服务多少个请求（keep-alive 公平性保护）；0 按 1 处理。
    std::size_t max_requests_per_connection{100};
    /// 请求头上限（含起始行）。
    std::size_t max_header_bytes{64 * 1024};
    /// 请求 body 上限。
    std::size_t max_body_bytes{8 * 1024 * 1024};
    /// listen(2) backlog；<=0 按 64 处理。
    int backlog{128};
    /// accept(2) 的轮询超时，决定 Stop() 等待 accept 线程退出的上限；
    /// 内部钳制到 [10ms, 1000ms]，避免配置过大导致 Stop 卡住。
    std::chrono::milliseconds accept_timeout{100};
    /// SSE 写入配置（事件大小上限、心跳间隔、写超时等）。
    sse::StreamOptions sse_options{};
    /// 是否用 `bufio::Writer` 批量写响应（B-06 示范接入）；默认关闭，便于 A/B。
    /// 开启后小响应（头 + 体总长 ≤ `bufio_capacity`）拆成「头、体」两段并
    /// `FlushBatch()` 一次 `writev`；超过容量的响应走单次 `send` 旁路（B-04）。
    bool use_bufio{false};
    /// bufio 缓冲容量（字节）；0 按 4096 处理，内部钳制到 [16, 1 MiB]。
    std::size_t bufio_capacity{4096};
};

/**
 * @brief 最小 HTTP/SSE 服务端。
 *
 * ## 生命周期
 * `Listen()` -> `OnRequest()`/`AsSse()` -> `Start()` -> `Stop()`。
 * `Listen()` 之后可以继续注册 handler；`Start()` 之后再修改路由只影响新连接
 * （旧连接的后续 keep-alive 请求也会看到新 handler）。
 *
 * ## 停止语义
 * `Stop()` 依次：置停止标记、关闭待处理队列、shutdown 所有活动连接
 * （唤醒阻塞的 recv/SSE 写入）、join accept 线程、关闭监听 socket、join worker。
 * 因此空闲 keep-alive 连接与正在推流的 SSE 连接都会被唤醒并退出；`Stop()`
 * 返回后端口已释放，可以立即用新的 `HttpServer` 重新绑定。
 *
 * ## 不变量
 * - 一条连接只由一个 worker 线程串行处理；
 * - SSE 路由优先于普通 handler，且 SSE 响应结束后连接直接关闭；
 * - handler 抛异常降级为 500 + `Connection: close`，不会穿透线程边界。
 */
class HttpServer final {
public:
    /// 普通请求处理器：输入已完整解析的请求，返回响应。
    using RequestHandler = std::function<http::Response(const http::Request&)>;
    /// SSE 处理器：直接向已提交响应头的 `sse::Stream` 写事件。
    using SseHandler = std::function<void(const http::Request&, sse::Stream&)>;

    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    /**
     * @brief 解析并绑定监听地址（不启动线程）。
     * @param address `host:port` 形式；host 可为空（通配）、IPv4、`[IPv6]`；
     *                port 为 0 时由内核分配，可用 `Port()` 查询实际端口。
     * @param config 配置快照；未给出的字段用默认值。
     * @return 成功返回 `ok()`；重复调用返回 `AlreadyListening`。
     */
    HttpServerResult Listen(std::string address, HttpServerConfig config = {});

    /// @brief `Listen(host + ":" + port)` 的便捷重载（自动为 IPv6 加方括号）。
    HttpServerResult Listen(const std::string& host, std::uint16_t port,
                            HttpServerConfig config = {});

    /// @brief 设置普通请求处理器；传空函数对象表示取消。
    void OnRequest(RequestHandler handler);

    /// @brief 注册/替换一个 SSE 路由（path 精确匹配）。
    void AsSse(std::string path, SseHandler handler);

    /**
     * @brief 启动 accept 线程与 worker 线程。
     * @return 未 Listen -> NotListening；已启动 -> AlreadyRunning；
     *         NonBlocking -> UnsupportedModel；线程创建失败 -> ThreadFailure。
     */
    HttpServerResult Start();

    /**
     * @brief 优雅关闭：唤醒所有连接、停止线程并释放监听 socket。
     * @note 幂等；未 Listen 时调用也不会失败。返回后可以重新 `Listen()`。
     */
    void Stop() noexcept;

    bool IsRunning() const noexcept { return m_running.load(std::memory_order_acquire); }

    /// 是否处于「已请求停止但尚未返回」的窗口；SSE handler 可据此提前退出。
    bool IsStopping() const noexcept { return m_stopping.load(std::memory_order_acquire); }

    /// 实际监听端口；未 Listen 时为 0。
    std::uint16_t Port() const noexcept { return m_port; }

    /// 实际监听地址（`host:port`）；未 Listen 时为空。
    std::string LocalAddress() const;

    /// 当前正在被 worker 处理的连接数（不含排队连接）。
    std::size_t ActiveConnections() const noexcept {
        return m_active_count.load(std::memory_order_acquire);
    }

    const HttpServerConfig& config() const noexcept { return m_config; }

    /// @brief 写路径累计统计（B-06 验证用）；未开启 `use_bufio` 时计数恒 0。
    struct WriteStats {
        std::uint64_t responses{0};     ///< 成功写出的响应数（含 HEAD，所有路径都计）。
        std::uint64_t writev_calls{0};  ///< 批量 flush（一次 writev）次数（仅 use_bufio）。
        std::uint64_t direct_writes{0}; ///< 大响应旁路单次 send 次数（仅 use_bufio）。
        std::uint64_t write_errors{0};  ///< 写失败次数（仅 use_bufio 路径计数）。
    };
    /// @brief 读一次写路径计数快照（原子读，无需加锁）。
    WriteStats GetWriteStats() const noexcept;

private:
    void AcceptLoop();
    void WorkerLoop();
    void ServeConnection(const net::Socket::Ptr& connection);
    void ServeSse(const net::Socket::Ptr& connection, const http::Request& request,
                  const SseHandler& handler);
    bool ReadRequest(const net::Socket::Ptr& connection, http::Parser& parser,
                     std::string& pending, http::Request& request, char* buffer,
                     std::size_t capacity);
    SseHandler FindSseHandler(const std::string& path) const;
    RequestHandler FindRequestHandler() const;
    void CloseAllConnections() noexcept;
    void JoinWorkers() noexcept;
    void CloseListener() noexcept;

    /// 配置快照；Listen() 写入，读取无需加锁（Start 后不再变更）。
    HttpServerConfig m_config;
    /// 监听 socket；Listen() 创建，Stop() 关闭并重置。
    net::Socket::Ptr m_listener;
    /// 每连接一 Fiber 的调度器（IOManager）；Start() 创建，Stop() 关闭。
    std::shared_ptr<io::IOManager> m_manager;
    /// 实际监听地址字符串；Port()/LocalAddress() 读取。
    std::string m_local_address;
    /// 实际监听端口；Listen() 写入，Port() 读取。
    std::uint16_t m_port{0};

    /// 路由锁：保护 m_request_handler 与 m_sse_routes（运行期可改）。
    mutable std::mutex m_route_mutex;
    /// 普通请求处理器；OnRequest() 写，FindRequestHandler() 拷贝读取。
    RequestHandler m_request_handler;
    /// SSE 路由表（path + handler）；AsSse() 写，FindSseHandler() 拷贝读取。
    std::vector<std::pair<std::string, SseHandler>> m_sse_routes;

    /// 运行标记；Start() 置位，Stop() 清零。
    std::atomic<bool> m_running{false};
    /// 停止标记；Stop() 置位，所有循环据此退出。
    std::atomic<bool> m_stopping{false};
    /// 活动连接计数；worker 注册/注销时增减。
    std::atomic<std::size_t> m_active_count{0};

    /// 写路径计数（B-06）：成功响应数 / writev 次数 / 直写次数 / 失败次数。
    std::atomic<std::uint64_t> m_write_responses{0};
    std::atomic<std::uint64_t> m_write_batches{0};
    std::atomic<std::uint64_t> m_write_direct{0};
    std::atomic<std::uint64_t> m_write_errors{0};

    /// accept 线程；Start() 创建，Stop() join。
    std::thread m_acceptor;
    /// worker 线程集合；Start() 创建，Stop() join。
    std::vector<std::thread> m_workers;

    /// 队列锁 + 条件变量：accept 线程入队，worker 出队。
    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;
    /// 待处理连接队列；受 m_queue_mutex 保护。
    std::deque<net::Socket::Ptr> m_queue;
    /// 队列关闭标记；受 m_queue_mutex 保护，Stop() 置位后 worker 退出。
    bool m_queue_closed{false};

    /// 活动连接表（fd 裸指针作为 key，值持有 shared_ptr）；受 m_active_mutex 保护。
    mutable std::mutex m_active_mutex;
    std::unordered_map<net::Socket*, net::Socket::Ptr> m_active;
};

}  // namespace go2cpp::bento
