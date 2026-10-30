/**
 * @file ws_server.cpp
 * @brief `bento::WsServer` 的实现：accept/worker 线程、Upgrade 握手、帧读循环与优雅停止。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/bento/net/ws_server.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <utility>

namespace go2cpp::bento {

namespace {

using std::chrono::milliseconds;

/// 握手阶段单次 recv 的缓冲区；HTTP 请求头通常远小于 1KiB。
constexpr std::size_t kHandshakeReadChunk = 4096;

/// 把配置里的 0 / 负值归一成可工作的最小值，避免 "配置成 0 就静默不干活"。
WsServerConfig NormalizeConfig(WsServerConfig config) {
    config.worker_threads = std::max<std::size_t>(1, config.worker_threads);
    config.max_queued_connections = std::max<std::size_t>(1, config.max_queued_connections);
    if (config.backlog <= 0) {
        config.backlog = 64;
    }
    if (config.accept_timeout.count() <= 0) {
        config.accept_timeout = milliseconds(100);
    }
    // 上限保证 Stop() 最多等一个轮询周期就能收掉 accept 线程。
    config.accept_timeout = std::max(milliseconds(10),
                                     std::min(config.accept_timeout, milliseconds(1000)));
    if (config.handshake_timeout.count() <= 0) {
        config.handshake_timeout = milliseconds(10000);
    }
    // 下限防止对端连上不发言就把 worker 永久占住；上限防止 Stop() 被恶意连接拖住。
    config.handshake_timeout = std::max(milliseconds(100),
                                        std::min(config.handshake_timeout, milliseconds(120000)));
    if (config.max_header_bytes == 0) {
        config.max_header_bytes = 16 * 1024;
    }
    return config;
}

/// 把 errno 值格式化成可读文本；0 时返回空串。
std::string SystemErrorText(int error) {
    if (error == 0) {
        return {};
    }
    return std::strerror(error);
}

/// 连接已经不可发送时的统一错误。
net::WebSocketResult ClosedResult() {
    return {net::WebSocketError::Closed, 0, "ws connection is closed"};
}

/// 从 Close 帧 payload 中取出 16 位关闭码；payload 不足 2 字节时返回 0。
std::uint16_t CloseCodeFromPayload(const std::string& payload) noexcept {
    if (payload.size() < 2) {
        return 0;
    }
    const auto high = static_cast<unsigned>(static_cast<unsigned char>(payload[0]));
    const auto low = static_cast<unsigned>(static_cast<unsigned char>(payload[1]));
    return static_cast<std::uint16_t>((high << 8U) | low);
}

}  // namespace

const char* WsServerErrorName(WsServerError error) noexcept {
    switch (error) {
        case WsServerError::None: return "None";
        case WsServerError::InvalidAddress: return "InvalidAddress";
        case WsServerError::BindFailed: return "BindFailed";
        case WsServerError::ListenFailed: return "ListenFailed";
        case WsServerError::NotListening: return "NotListening";
        case WsServerError::AlreadyListening: return "AlreadyListening";
        case WsServerError::AlreadyRunning: return "AlreadyRunning";
        case WsServerError::ThreadFailure: return "ThreadFailure";
    }
    return "Unknown";
}

WsServerResult WsServerResult::Failure(WsServerError error, std::string message,
                                       int system_error) {
    WsServerResult result;
    result.error = error;
    result.system_error = system_error;
    result.message = std::move(message);
    return result;
}

// ---------------------------------------------------------------------------
// WsConnection
// ---------------------------------------------------------------------------

WsConnection::WsConnection(std::uint64_t id, net::WebSocket::Ptr websocket,
                           http::Request request, std::string peer)
    : m_id(id),
      m_websocket(std::move(websocket)),
      m_request(std::move(request)),
      m_peer(std::move(peer)),
      m_path(m_request.path()) {}

net::WebSocketResult WsConnection::SendText(std::string_view payload) {
    return SendData(net::WebSocketOpcode::Text, payload);
}

net::WebSocketResult WsConnection::SendBinary(std::string_view payload) {
    return SendData(net::WebSocketOpcode::Binary, payload);
}

net::WebSocketResult WsConnection::Ping(std::string_view payload) {
    return SendControl(net::WebSocketOpcode::Ping, payload);
}

net::WebSocketResult WsConnection::Pong(std::string_view payload) {
    return SendControl(net::WebSocketOpcode::Pong, payload);
}

net::WebSocketResult WsConnection::Close(std::uint16_t code, std::string_view reason) {
    // base 的 close() 幂等：重复调用不会重复写 Close 帧。
    const auto result = m_websocket->close(code, reason);
    if (result.ok()) {
        MarkClosed(code);
    }
    return result;
}

net::WebSocketResult WsConnection::SendData(net::WebSocketOpcode opcode,
                                            std::string_view payload) {
    if (!IsOpen()) {
        return ClosedResult();
    }
    const auto result = opcode == net::WebSocketOpcode::Text
                            ? m_websocket->send_text(payload)
                            : m_websocket->send_binary(payload);
    MarkClosedOnTransportError(result);
    return result;
}

net::WebSocketResult WsConnection::SendControl(net::WebSocketOpcode opcode,
                                               std::string_view payload) {
    if (!IsOpen()) {
        return ClosedResult();
    }
    const auto result = opcode == net::WebSocketOpcode::Ping ? m_websocket->ping(payload)
                                                             : m_websocket->pong(payload);
    MarkClosedOnTransportError(result);
    return result;
}

void WsConnection::MarkClosedOnTransportError(const net::WebSocketResult& result) noexcept {
    if (result.ok()) {
        return;
    }
    // Protocol/MessageTooLarge 只说明这条消息非法，连接仍可继续用；
    // Io/Timeout/Closed 表示字节流已经不可靠（可能半帧写出），必须标记关闭。
    if (result.error == net::WebSocketError::Io || result.error == net::WebSocketError::Timeout ||
        result.error == net::WebSocketError::Closed) {
        MarkClosed(0);
    }
}

void WsConnection::MarkClosed(std::uint16_t code) noexcept {
    m_open.store(false, std::memory_order_release);
    if (code != 0) {
        // 保留第一个有效关闭码：对端 Close 与本地 Close 竞争时以先到者为准。
        std::uint16_t expected = 0;
        (void)m_close_code.compare_exchange_strong(expected, code,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire);
    }
}

void WsConnection::ShutdownTransport() noexcept {
    if (const auto socket = m_websocket->socket()) {
        (void)socket->shutdown(SHUT_RDWR);
    }
}

// ---------------------------------------------------------------------------
// WsServer
// ---------------------------------------------------------------------------

WsServer::WsServer() = default;

WsServer::~WsServer() { Stop(); }

WsServerResult WsServer::Listen(std::string address, WsServerConfig config) {
    if (m_running.load(std::memory_order_acquire)) {
        return WsServerResult::Failure(WsServerError::AlreadyRunning, "服务已经启动");
    }
    if (m_listener) {
        return WsServerResult::Failure(WsServerError::AlreadyListening,
                                       "监听 socket 已经建立");
    }
    if (address.empty()) {
        return WsServerResult::Failure(WsServerError::InvalidAddress, "监听地址为空");
    }
    // AI_ADDRCONFIG 会按本机网卡过滤地址族：127.0.0.1 一定可用，IPv6 需本机启用。
    const auto resolved =
        net::Address::LookupAny(address, AF_UNSPEC, SOCK_STREAM, IPPROTO_TCP);
    if (!resolved) {
        return WsServerResult::Failure(WsServerError::InvalidAddress,
                                       "无法解析监听地址: " + address);
    }
    config = NormalizeConfig(std::move(config));
    auto listener = net::Socket::CreateTCP(resolved->family());
    if (!listener) {
        return WsServerResult::Failure(WsServerError::BindFailed,
                                       "创建监听 socket 失败", errno);
    }
    int reuse = 1;
    (void)listener->set_option(SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (!listener->bind(*resolved)) {
        const int error = listener->last_error();
        return WsServerResult::Failure(WsServerError::BindFailed,
                                       "bind(" + address + ") 失败: " + SystemErrorText(error),
                                       error);
    }
    if (!listener->listen(config.backlog)) {
        const int error = listener->last_error();
        return WsServerResult::Failure(WsServerError::ListenFailed,
                                       "listen() 失败: " + SystemErrorText(error), error);
    }

    m_config = std::move(config);
    m_local_address = address;
    m_port = 0;
    const auto local = listener->local_address();
    if (local) {
        m_local_address = local->to_string();
        if (const auto ip = std::dynamic_pointer_cast<net::IPAddress>(local)) {
            m_port = ip->port();
        }
    }
    m_listener = std::move(listener);
    m_stopping.store(false, std::memory_order_release);
    return WsServerResult::Success();
}

WsServerResult WsServer::Listen(const std::string& host, std::uint16_t port,
                                WsServerConfig config) {
    std::string address = host;
    // IPv6 字面量必须加方括号，否则 "::1:8080" 会被拆成 host="::1:8080"。
    if (address.find(':') != std::string::npos &&
        (address.empty() || address.front() != '[')) {
        address = "[" + address + "]";
    }
    address += ":" + std::to_string(port);
    return Listen(std::move(address), std::move(config));
}

void WsServer::OnConnection(ConnectionHandler handler) {
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_connection_handler = std::move(handler);
}

void WsServer::OnMessage(MessageHandler handler) {
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_message_handler = std::move(handler);
}

WsServerResult WsServer::Start() {
    if (!m_listener) {
        return WsServerResult::Failure(WsServerError::NotListening, "尚未调用 Listen()");
    }
    if (m_running.load(std::memory_order_acquire)) {
        return WsServerResult::Failure(WsServerError::AlreadyRunning, "服务已经启动");
    }
    m_stopping.store(false, std::memory_order_release);
    m_handshakes.store(0, std::memory_order_release);
    m_rejected.store(0, std::memory_order_release);
    m_messages.store(0, std::memory_order_release);
    m_next_connection_id.store(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_queue_closed = false;
        m_queue.clear();
    }
    m_running.store(true, std::memory_order_release);

    const std::size_t worker_count = m_config.worker_threads;
    try {
        m_workers.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            m_workers.emplace_back([this]() { WorkerLoop(); });
        }
        m_acceptor = std::thread([this]() { AcceptLoop(); });
    } catch (const std::exception& error) {
        // 线程创建失败：先让已启动的 worker 退出，再回滚运行标记。
        m_stopping.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_queue_closed = true;
        }
        m_queue_cv.notify_all();
        JoinWorkers();
        m_running.store(false, std::memory_order_release);
        return WsServerResult::Failure(WsServerError::ThreadFailure,
                                       std::string("创建服务线程失败: ") + error.what());
    }
    return WsServerResult::Success();
}

void WsServer::Stop() noexcept {
    const bool had_listener = static_cast<bool>(m_listener);
    const bool was_running = m_running.exchange(false, std::memory_order_acq_rel);
    if (!was_running && !had_listener) {
        return;
    }
    m_stopping.store(true, std::memory_order_release);

    // step1: 关闭队列并丢弃尚未处理的连接（锁外 close，避免持锁做系统调用）。
    std::deque<net::Socket::Ptr> dropped;
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_queue_closed = true;
        dropped.swap(m_queue);
    }
    m_queue_cv.notify_all();
    for (auto& connection : dropped) {
        if (connection) {
            (void)connection->close();
        }
    }

    // step2: shutdown 所有活动连接，唤醒阻塞在握手 recv / 帧 recv 上的 worker。
    CloseAllConnections();

    // step3: accept 线程按 accept_timeout 轮询停止标记，最多等一个超时周期。
    if (m_acceptor.joinable()) {
        try {
            m_acceptor.join();
        } catch (...) {
            // join 失败（例如线程已被 detach）不影响后续清理。
        }
    }

    // step4: 关闭监听 socket（join 之后关闭，避免 accept 使用已释放对象）。
    CloseListener();

    // step5: 等待所有 worker 收尾，并兜底再唤醒一次（覆盖注册竞态窗口）。
    JoinWorkers();
    CloseAllConnections();
}

std::string WsServer::LocalAddress() const {
    return m_local_address;
}

WsServerStats WsServer::Stats() const noexcept {
    WsServerStats stats;
    stats.active_connections = m_active_count.load(std::memory_order_acquire);
    stats.handshakes = m_handshakes.load(std::memory_order_acquire);
    stats.rejected_handshakes = m_rejected.load(std::memory_order_acquire);
    stats.received_messages = m_messages.load(std::memory_order_acquire);
    return stats;
}

void WsServer::AcceptLoop() {
    while (!m_stopping.load(std::memory_order_acquire)) {
        auto connection = m_listener->accept(m_config.accept_timeout);
        if (!connection) {
            if (m_stopping.load(std::memory_order_acquire)) {
                return;
            }
            continue;
        }
        if (m_stopping.load(std::memory_order_acquire)) {
            (void)connection->close();
            return;
        }
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (!m_queue_closed && m_queue.size() < m_config.max_queued_connections) {
                m_queue.push_back(std::move(connection));
                queued = true;
            }
        }
        if (queued) {
            m_queue_cv.notify_one();
        } else if (connection) {
            // 队列满：骨架级背压直接拒绝，让客户端看到连接被关闭。
            (void)connection->close();
        }
    }
}

void WsServer::WorkerLoop() {
    for (;;) {
        net::Socket::Ptr connection;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_cv.wait(lock, [this]() { return m_queue_closed || !m_queue.empty(); });
            if (m_queue.empty()) {
                return;
            }
            connection = std::move(m_queue.front());
            m_queue.pop_front();
        }
        if (!connection) {
            continue;
        }
        // 先登记再检查停止标记：Stop() 扫描活动表之后才登记的连接会在下面
        // 立即放弃，不会进入阻塞 recv，从而不会让 Stop() join 卡住。
        {
            std::lock_guard<std::mutex> lock(m_active_mutex);
            m_active.emplace(connection.get(), connection);
        }
        m_active_count.fetch_add(1, std::memory_order_acq_rel);
        if (!m_stopping.load(std::memory_order_acquire)) {
            ServeConnection(connection);
        }
        {
            std::lock_guard<std::mutex> lock(m_active_mutex);
            m_active.erase(connection.get());
        }
        m_active_count.fetch_sub(1, std::memory_order_acq_rel);
        (void)connection->close();
    }
}

bool WsServer::ReadHandshake(const net::Socket::Ptr& connection, http::Parser& parser,
                             http::Request& request) {
    std::array<char, kHandshakeReadChunk> buffer{};
    for (;;) {
        if (m_stopping.load(std::memory_order_acquire)) {
            return false;
        }
        const auto count = connection->recv(buffer.data(), buffer.size());
        if (count.transferred <= 0) {
            return false;
        }
        const auto parsed = parser.feed(
            std::string_view(buffer.data(), static_cast<std::size_t>(count.transferred)),
            request);
        if (parsed.state == http::ParseState::Error) {
            return false;
        }
        if (parsed.state == http::ParseState::Complete) {
            return true;
        }
    }
}

void WsServer::ServeConnection(const net::Socket::Ptr& connection) {
    (void)connection->set_recv_timeout(m_config.handshake_timeout);
    (void)connection->set_send_timeout(m_config.handshake_timeout);

    // 握手请求没有 body：max_body_bytes=0 让任何声明的 body 都直接被 Parser 拒绝。
    http::ParserLimits limits;
    limits.max_header_bytes = m_config.max_header_bytes;
    limits.max_body_bytes = 0;
    http::Parser parser(limits);

    http::Request request;
    if (!ReadHandshake(connection, parser, request)) {
        return;
    }
    // Parser 可能把紧随 Upgrade 请求之后的首个 WebSocket 帧一起读进缓冲区，
    // 必须通过 pending 注入 base WebSocket，否则首帧会被静默丢弃。
    std::string pending = parser.take_remaining();

    std::string key;
    std::string reason;
    if (!net::WebSocket::ValidateUpgradeRequest(request, key, &reason)) {
        m_rejected.fetch_add(1, std::memory_order_acq_rel);
        RejectHandshake(connection);
        return;
    }

    auto websocket = net::WebSocket::AcceptWithPending(connection, request, std::move(pending),
                                                       m_config.websocket, nullptr);
    if (!websocket) {
        m_rejected.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    // 握手完成后帧层超时由 base 的 io_timeout 逐次传入，socket 默认值恢复为不限时。
    (void)connection->set_recv_timeout(net::Socket::Duration::max());
    (void)connection->set_send_timeout(net::Socket::Duration::max());

    std::string peer;
    if (const auto remote = connection->remote_address()) {
        peer = remote->to_string();
    }
    const auto id = m_next_connection_id.fetch_add(1, std::memory_order_acq_rel);
    auto ws_connection = WsConnection::Ptr(
        new WsConnection(id, websocket, std::move(request), std::move(peer)));
    m_handshakes.fetch_add(1, std::memory_order_acq_rel);

    // OnConnection 可以保存 Ptr 做主动推送，也可以直接 Close 拒绝服务。
    if (const auto connection_handler = FindConnectionHandler()) {
        try {
            connection_handler(ws_connection);
        } catch (...) {
            // 回调异常不能穿透线程边界：关闭当前连接，server 继续服务其他连接。
            ws_connection->MarkClosed(0);
        }
    }

    while (!m_stopping.load(std::memory_order_acquire) && ws_connection->IsOpen()) {
        net::WebSocketMessage message;
        const auto result = websocket->read_message(message);
        if (!result.ok()) {
            break;
        }
        if (message.opcode == net::WebSocketOpcode::Close) {
            // base 已完成回 Close 帧；这里只记录对端关闭码并结束读循环。
            ws_connection->MarkClosed(CloseCodeFromPayload(message.payload));
            break;
        }
        if (message.opcode != net::WebSocketOpcode::Text &&
            message.opcode != net::WebSocketOpcode::Binary) {
            continue;
        }
        m_messages.fetch_add(1, std::memory_order_acq_rel);
        const auto message_handler = FindMessageHandler();
        if (!message_handler) {
            continue;
        }
        try {
            message_handler(ws_connection, message);
        } catch (...) {
            break;
        }
    }
    ws_connection->MarkClosed(0);
}

void WsServer::RejectHandshake(const net::Socket::Ptr& connection) noexcept {
    try {
        http::Response response;
        response.status = http::Status::BadRequest;
        response.header.set("Content-Type", "text/plain; charset=utf-8");
        response.body = "websocket upgrade required";
        response.close = true;
        const std::string wire = response.to_string();
        net::SocketStream stream(connection, false);
        (void)stream.write_all(wire.data(), wire.size());
    } catch (...) {
        // noexcept 路径：写 400 失败不影响调用方，连接随后由 worker 关闭。
    }
}

WsServer::ConnectionHandler WsServer::FindConnectionHandler() const {
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    return m_connection_handler;
}

WsServer::MessageHandler WsServer::FindMessageHandler() const {
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    return m_message_handler;
}

void WsServer::CloseAllConnections() noexcept {
    // shutdown(2) 是立即返回的系统调用，持锁遍历不会阻塞在慢连接上；
    // 不复制 shared_ptr 是为了让本函数在 Stop() 的 noexcept 路径里零分配。
    std::lock_guard<std::mutex> lock(m_active_mutex);
    for (const auto& entry : m_active) {
        if (entry.second) {
            (void)entry.second->shutdown(SHUT_RDWR);
        }
    }
}

void WsServer::JoinWorkers() noexcept {
    const auto self = std::this_thread::get_id();
    for (auto& worker : m_workers) {
        if (!worker.joinable()) {
            continue;
        }
        // handler 内调用 Stop() 时当前线程就是 worker：join 自己会抛
        // resource_deadlock_would_occur，而让 joinable thread 析构会 terminate，
        // 因此只能 detach（文档同时要求不要在 handler 内 Stop）。
        if (worker.get_id() == self) {
            worker.detach();
            continue;
        }
        try {
            worker.join();
        } catch (...) {
            // 同上：join 失败不阻止其余线程收尾。
        }
    }
    m_workers.clear();
}

void WsServer::CloseListener() noexcept {
    if (m_listener) {
        (void)m_listener->close();
        m_listener.reset();
    }
    m_local_address.clear();
    m_port = 0;
}

}  // namespace go2cpp::bento
