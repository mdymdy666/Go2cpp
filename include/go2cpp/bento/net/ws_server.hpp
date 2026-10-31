#pragma once

/**
 * @file ws_server.hpp
 * @brief 基于 base `net::WebSocket` 的最小 WebSocket 服务端骨架。
 * @details 把 base 的 RFC 6455 协议对象（`net::WebSocket`）与 `net::Socket`
 *          组装成一个可 Listen/Start/Stop 的服务端：
 *          - 握手：先用 `http::Parser` 读一个 HTTP Upgrade 请求，再用
 *            `WebSocket::ValidateUpgradeRequest()` 校验；合法请求交给
 *            `WebSocket::AcceptWithPending()` 写 101 并接管连接（Parser 多读
 *            的首帧字节通过 pending 注入，不会丢消息）；
 *          - 非法请求：回 `400 Bad Request` 并关闭连接，不进入 WebSocket 状态；
 *          - 数据面：`OnConnection` 在握手完成后回调一次，`OnMessage` 对每条
 *            Text/Binary 消息回调；Ping/Pong 由 base 的 `read_message()` 自动
 *            处理，Close 帧走 base 的关闭握手后终止读循环；
 *          - 主动发送：handler 拿到的 `WsConnection::Ptr` 可跨线程
 *            `SendText/SendBinary/Ping/Pong/Close`（base 的写锁串行化并发写）。
 *
 * ## 并发模型
 *          一个 accept 线程 + 固定数量 worker 线程；每条连接由一个 worker
 *          独占读方向并串行回调 `OnConnection` / `OnMessage`。不同连接的
 *          handler 可并发执行，业务状态需自行同步。`WsConnection` 的发送
 *          接口线程安全（复用 base 的读锁/写锁），可以在任意线程调用。
 *
 * ## 已实现 / 未实现（骨架边界）
 *          已实现：Upgrade 校验与 101 响应、非法请求 400、首帧 pending 注入、
 *          文本/二进制消息、双向 Ping/Pong、Close 握手、优雅 Stop、并发连接、
 *          运行期替换 handler、基础统计。
 *          未实现：TLS（wss）、permessage-deflate、子协议协商、路由与鉴权、
 *          服务端广播表、连接级限流、分片消息的逐片回调（base 已重组后交付）。
 *
 * ## 停止语义
 *          `Stop()` 依次：置停止标记、关闭待处理队列、shutdown 所有活动连接
 *          （唤醒阻塞在握手 recv 与帧 recv 上的 worker）、join accept 线程、
 *          关闭监听 socket、join worker。因此空闲连接的读循环与握手阶段的
 *          阻塞读都会被唤醒并退出，`Stop()` 返回后端口已释放。
 *          `Stop()` 与析构幂等；不要在 handler 内调用 `Stop()`（实现会
 *          detach 当前 worker 以防 terminate，但 server 对象可能在 handler
 *          返回前被调用方析构）。
 *
 * ## 典型用法
 * @code
 * go2cpp::bento::WsServer server;
 * server.OnConnection([](const go2cpp::bento::WsConnection::Ptr& connection) {
 *     std::printf("ws %llu connected: %s\n",
 *                 static_cast<unsigned long long>(connection->id()),
 *                 connection->path().c_str());
 * });
 * server.OnMessage([](const go2cpp::bento::WsConnection::Ptr& connection,
 *                      const go2cpp::net::WebSocketMessage& message) {
 *     if (message.opcode == go2cpp::net::WebSocketOpcode::Text) {
 *         (void)connection->SendText(message.payload);  // echo
 *     } else {
 *         (void)connection->SendBinary(message.payload);
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
#include "go2cpp/network/io.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/websocket.hpp"

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
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace go2cpp::bento {

/// @brief `WsServer` 操作失败的分类。
enum class WsServerError {
    None,              ///< 成功。
    InvalidAddress,    ///< 监听地址无法解析。
    BindFailed,        ///< bind(2) 失败（端口被占用、权限不足等）。
    ListenFailed,      ///< listen(2) 失败。
    NotListening,      ///< 尚未 `Listen()` 就调用 `Start()`。
    AlreadyListening,  ///< 已经绑定过监听 socket。
    AlreadyRunning,    ///< 已经 `Start()`。
    ThreadFailure,     ///< 创建 accept/worker 线程失败。
};

/// @brief 错误分类的可读名称（英文标识，便于日志与断言）。
const char* WsServerErrorName(WsServerError error) noexcept;

/// @brief 不跨线程抛异常的操作结果。
struct WsServerResult {
    WsServerError error{WsServerError::None};
    /// 系统错误码（errno）；仅 Bind/Listen 等系统调用失败时非 0。
    int system_error{0};
    std::string message;

    bool ok() const noexcept { return error == WsServerError::None; }
    static WsServerResult Success() { return {}; }
    static WsServerResult Failure(WsServerError error, std::string message,
                                  int system_error = 0);
};

/// @brief 服务端配置；字段在 `Listen()` 时快照，运行期不再改变。
struct WsServerConfig {
    /// worker 线程数；0 按 1 处理。
    std::size_t worker_threads{4};
    /// accept 后待处理连接队列上限；超过时直接关闭新连接（骨架级背压）。
    std::size_t max_queued_connections{128};
    /// HTTP Upgrade 请求头上限（含起始行）。
    std::size_t max_header_bytes{16 * 1024};
    /// listen(2) backlog；<=0 按 64 处理。
    int backlog{128};
    /// accept(2) 的轮询超时，决定 Stop() 等待 accept 线程退出的上限；
    /// 内部钳制到 [10ms, 1000ms]，避免配置过大导致 Stop 卡住。
    std::chrono::milliseconds accept_timeout{100};
    /// 握手阶段的 recv/send 超时；内部钳制到 [100ms, 120s]，<=0 按 10s 处理。
    /// 握手完成后恢复为不限时，帧层超时由 `websocket.io_timeout` 控制。
    std::chrono::milliseconds handshake_timeout{10000};
    /// 帧/消息上限、帧层 IO 超时等，原样透传给 base WebSocket。
    net::WebSocketConfig websocket{};
};

/// @brief 服务端统计快照（原子读取，不保证各项严格同一时刻）。
struct WsServerStats {
    /// 正在被 worker 服务的连接数（含尚未完成握手的连接）。
    std::size_t active_connections{0};
    /// 累计成功完成的握手数。
    std::uint64_t handshakes{0};
    /// 累计被拒绝的握手数（非法 Upgrade 或 101 写入失败）。
    std::uint64_t rejected_handshakes{0};
    /// 累计交付给业务的 Text/Binary 消息数。
    std::uint64_t received_messages{0};
};

/**
 * @brief 一条已经完成握手的 WebSocket 服务端连接。
 *
 * ## 生命周期
 * 由 `WsServer` 在握手成功后创建并通过 `OnConnection` 交付 `Ptr`；连接结束
 * （对端关闭、读错误、Stop）后 `IsOpen()` 变为 false，但对象仍可被业务持有
 * （发送接口返回 `WebSocketError::Closed`）。`WsServer` 内部持有同一 socket，
 * 连接结束后由 worker 关闭底层 socket。
 *
 * ## 线程安全
 * `SendText/SendBinary/Ping/Pong/Close` 可从任意线程并发调用：base WebSocket
 * 用写锁保证帧不交错。读方向只由所属 worker 消费，业务不要自行读 socket。
 * `id/path/peer/request` 在构造后只读，可无锁访问。
 *
 * ## 关闭语义
 * `Close()` 发送 Close 帧后立即把连接标记为关闭；重复调用幂等（base 的
 * `close()` 幂等）。对端发起的关闭由读循环处理，`close_code()` 返回对端
 * 携带的 code（无 payload 时为 0）。
 */
class WsConnection final {
public:
    using Ptr = std::shared_ptr<WsConnection>;

    WsConnection(const WsConnection&) = delete;
    WsConnection& operator=(const WsConnection&) = delete;
    WsConnection(WsConnection&&) = delete;
    WsConnection& operator=(WsConnection&&) = delete;

    /// @brief 服务端分配的自增连接序号（从 1 开始，每次 Start 重置）。
    std::uint64_t id() const noexcept { return m_id; }

    /// @brief 握手请求的 target path（不含 query）。
    const std::string& path() const noexcept { return m_path; }

    /// @brief 对端地址文本（`ip:port`）；取不到时为空。
    const std::string& peer() const noexcept { return m_peer; }

    /// @brief 完整握手请求的只读副本（含 header/target/query）。
    const http::Request& request() const noexcept { return m_request; }

    /// @brief 连接是否仍可发送；Close 或传输错误后为 false。
    bool IsOpen() const noexcept { return m_open.load(std::memory_order_acquire); }

    /// @brief 关闭码；未关闭或对端未携带 code 时为 0。
    std::uint16_t close_code() const noexcept {
        return m_close_code.load(std::memory_order_acquire);
    }

    /// @brief 发送文本消息；payload 为空也是合法的空文本消息。
    net::WebSocketResult SendText(std::string_view payload);

    /// @brief 发送二进制消息。
    net::WebSocketResult SendBinary(std::string_view payload);

    /// @brief 发送 Ping 控制帧；payload 不得超过 125 字节。
    net::WebSocketResult Ping(std::string_view payload = {});

    /// @brief 发送 Pong 控制帧；payload 不得超过 125 字节。
    net::WebSocketResult Pong(std::string_view payload = {});

    /// @brief 发送 Close 帧并标记连接关闭；重复调用幂等。
    net::WebSocketResult Close(std::uint16_t code = 1000, std::string_view reason = {});

private:
    friend class WsServer;

    WsConnection(std::uint64_t id, net::WebSocket::Ptr websocket, http::Request request,
                 std::string peer);

    net::WebSocketResult SendData(net::WebSocketOpcode opcode, std::string_view payload);
    net::WebSocketResult SendControl(net::WebSocketOpcode opcode, std::string_view payload);
    /// 传输层错误（Io/Timeout/Closed）后标记连接不可再用；协议错误不标记。
    void MarkClosedOnTransportError(const net::WebSocketResult& result) noexcept;
    void MarkClosed(std::uint16_t code) noexcept;
    /// 半关闭底层 socket，唤醒阻塞中的读写；供 Stop() 使用。
    void ShutdownTransport() noexcept;

    /// 连接序号；构造时由 WsServer 分配，只读。
    std::uint64_t m_id{0};
    /// base 协议对象；握手成功后由本类独占持有（内含 socket）。
    net::WebSocket::Ptr m_websocket;
    /// 握手请求副本；构造时拷贝，之后只读。
    http::Request m_request;
    /// 对端地址文本；构造时解析，只读。
    std::string m_peer;
    /// 握手路径快照；构造时从 m_request 解析，只读。
    std::string m_path;
    /// 可发送标记；Close/传输错误/连接结束时置 false。
    std::atomic<bool> m_open{true};
    /// 关闭码；MarkClosed() 首次写入，读到 0 表示未知。
    std::atomic<std::uint16_t> m_close_code{0};
};

/**
 * @brief 最小 WebSocket 服务端。
 *
 * ## 生命周期
 * `Listen()` -> `OnConnection()`/`OnMessage()` -> `Start()` -> `Stop()`。
 * `Listen()` 之后可以继续注册 handler；运行期替换 handler 只影响之后的事件。
 *
 * ## 回调契约
 * - `OnConnection`：握手完成后、进入读循环前调用一次；参数连接已可发送。
 *   handler 内可以 `Close()` 拒绝服务，读循环会立即退出。
 * - `OnMessage`：每条 Text/Binary 消息调用一次；Ping/Pong 由 base 自动应答，
 *   不回调；Close 帧由内部处理并终止连接，不回调。
 * - 回调抛异常会关闭当前连接（异常被吞掉，不穿透线程边界），server 继续工作。
 *
 * ## 不变量
 * - 一条连接只由一个 worker 线程串行读；连接之间可并发；
 * - 同一连接的 `OnConnection` 一定早于该连接的 `OnMessage`；
 * - `Stop()` 返回后不再有回调执行，所有活动连接已 shutdown/关闭。
 */
class WsServer final {
public:
    /// 握手完成回调：连接已可发送，业务可保存 Ptr 用于后续主动推送。
    using ConnectionHandler = std::function<void(const WsConnection::Ptr&)>;
    /// 消息回调：只对 Text/Binary 调用，payload 已由 base 完成分片重组。
    using MessageHandler =
        std::function<void(const WsConnection::Ptr&, const net::WebSocketMessage&)>;

    WsServer();
    ~WsServer();

    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;
    WsServer(WsServer&&) = delete;
    WsServer& operator=(WsServer&&) = delete;

    /**
     * @brief 解析并绑定监听地址（不启动线程）。
     * @param address `host:port` 形式；host 可为空（通配）、IPv4、`[IPv6]`；
     *                port 为 0 时由内核分配，可用 `Port()` 查询实际端口。
     * @param config 配置快照；未给出的字段用默认值。
     * @return 成功返回 `ok()`；重复调用返回 `AlreadyListening`。
     */
    WsServerResult Listen(std::string address, WsServerConfig config = {});

    /// @brief `Listen(host + ":" + port)` 的便捷重载（自动为 IPv6 加方括号）。
    WsServerResult Listen(const std::string& host, std::uint16_t port,
                          WsServerConfig config = {});

    /// @brief 设置/替换握手完成回调；传空函数对象表示取消。
    void OnConnection(ConnectionHandler handler);

    /// @brief 设置/替换消息回调；传空函数对象表示取消。
    void OnMessage(MessageHandler handler);

    /**
     * @brief 启动 accept 线程与 worker 线程。
     * @return 未 Listen -> NotListening；已启动 -> AlreadyRunning；
     *         线程创建失败 -> ThreadFailure。
     */
    WsServerResult Start();

    /**
     * @brief 优雅关闭：唤醒所有连接、停止线程并释放监听 socket。
     * @note 幂等；未 Listen 时调用也不会失败。返回后可以重新 `Listen()`。
     */
    void Stop() noexcept;

    bool IsRunning() const noexcept { return m_running.load(std::memory_order_acquire); }

    /// 是否处于「已请求停止但尚未返回」的窗口；handler 可据此提前退出。
    bool IsStopping() const noexcept { return m_stopping.load(std::memory_order_acquire); }

    /// 实际监听端口；未 Listen 时为 0。
    std::uint16_t Port() const noexcept { return m_port; }

    /// 实际监听地址（`host:port`）；未 Listen 时为空。
    std::string LocalAddress() const;

    /// 当前正在被 worker 服务的连接数（含握手中的连接）。
    std::size_t ActiveConnections() const noexcept {
        return m_active_count.load(std::memory_order_acquire);
    }

    /// 统计快照；各项独立原子读取。
    WsServerStats Stats() const noexcept;

    const WsServerConfig& config() const noexcept { return m_config; }

private:
    void AcceptLoop();
    void WorkerLoop();
    void ServeConnection(const net::Socket::Ptr& connection);
    /// 读一个完整的 HTTP Upgrade 请求；成功时 parser 内部可能保留首帧字节。
    bool ReadHandshake(const net::Socket::Ptr& connection, http::Parser& parser,
                       http::Request& request, char* buffer, std::size_t capacity);
    /// 对非法 Upgrade 请求回 400 并关闭（只写响应，不进入 WebSocket 状态）。
    void RejectHandshake(const net::Socket::Ptr& connection) noexcept;
    ConnectionHandler FindConnectionHandler() const;
    MessageHandler FindMessageHandler() const;
    void CloseAllConnections() noexcept;
    void JoinWorkers() noexcept;
    void CloseListener() noexcept;

    /// 配置快照；Listen() 写入，Start 后不再变更。
    WsServerConfig m_config;
    /// 监听 socket；Listen() 创建，Stop() 关闭并重置。
    net::Socket::Ptr m_listener;
    /// 每连接一 Fiber 的调度器（IOManager）；Start() 创建，Stop() 关闭。
    std::shared_ptr<io::IOManager> m_manager;
    /// 实际监听地址字符串；Port()/LocalAddress() 读取。
    std::string m_local_address;
    /// 实际监听端口；Listen() 写入，Port() 读取。
    std::uint16_t m_port{0};

    /// 回调锁：保护 m_connection_handler 与 m_message_handler（运行期可改）。
    mutable std::mutex m_handler_mutex;
    /// 握手完成回调；OnConnection() 写，FindConnectionHandler() 拷贝读取。
    ConnectionHandler m_connection_handler;
    /// 消息回调；OnMessage() 写，FindMessageHandler() 拷贝读取。
    MessageHandler m_message_handler;

    /// 运行标记；Start() 置位，Stop() 清零。
    std::atomic<bool> m_running{false};
    /// 停止标记；Stop() 置位，所有循环据此退出。
    std::atomic<bool> m_stopping{false};
    /// 服务中连接计数；worker 注册/注销时增减。
    std::atomic<std::size_t> m_active_count{0};
    /// 累计握手成功数。
    std::atomic<std::uint64_t> m_handshakes{0};
    /// 累计握手拒绝数。
    std::atomic<std::uint64_t> m_rejected{0};
    /// 累计交付消息数。
    std::atomic<std::uint64_t> m_messages{0};
    /// 连接序号分配器；Start() 重置为 1。
    std::atomic<std::uint64_t> m_next_connection_id{1};

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
