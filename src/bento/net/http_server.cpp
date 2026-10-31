/**
 * @file http_server.cpp
 * @brief `bento::HttpServer` 的实现：accept/worker 线程、请求循环与 SSE 桥接。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/bento/net/http_server.hpp"

#include "go2cpp/network/bufio_socket.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <optional>
#include <utility>

namespace go2cpp::bento {

namespace {

/// 把配置里的 0 / 负值归一成可工作的最小值，避免 "配置成 0 就静默不干活"。
HttpServerConfig NormalizeConfig(HttpServerConfig config) {
    config.worker_threads = std::max<std::size_t>(1, config.worker_threads);
    config.max_queued_connections = std::max<std::size_t>(1, config.max_queued_connections);
    config.max_requests_per_connection =
        std::max<std::size_t>(1, config.max_requests_per_connection);
    if (config.backlog <= 0) {
        config.backlog = 64;
    }
    if (config.accept_timeout.count() <= 0) {
        config.accept_timeout = std::chrono::milliseconds(100);
    }
    // 上限保证 Stop() 最多等一个轮询周期就能收掉 accept 线程。
    config.accept_timeout =
        std::max(std::chrono::milliseconds(10),
                 std::min(config.accept_timeout, std::chrono::milliseconds(1000)));
    if (config.max_header_bytes == 0) {
        config.max_header_bytes = 64 * 1024;
    }
    if (config.max_body_bytes == 0) {
        config.max_body_bytes = 8 * 1024 * 1024;
    }
    if (config.bufio_capacity == 0) {
        config.bufio_capacity = 4096;
    }
    config.bufio_capacity = std::min<std::size_t>(
        1024 * 1024, std::max<std::size_t>(16, config.bufio_capacity));
    return config;
}

/// 把 errno 值格式化成可读文本；0 时返回空串。
std::string SystemErrorText(int error) {
    if (error == 0) {
        return {};
    }
    return std::strerror(error);
}

/// @brief 用 bufio 一次 writev 写出「响应头 + 响应体」两段（B-06）。
/// @details 头/体边界取第一个空行；两段都先进入 4 KB 缓冲，再由
///          `FlushBatch()` 合并成一次 `writev(2)`（段来自同一块缓冲，不需要
///          把头和体先拼成一个字符串）。
/// @return true 表示两段都写完且批量 flush 成功；失败时错误已粘在 writer 上。
bool WriteBufferedResponse(net::BufferedSocketWriter& writer,
                           std::string_view wire) {
    const std::size_t split = wire.find("\r\n\r\n");
    const std::size_t head_size =
        split == std::string_view::npos ? wire.size() : split + 4;
    const streams::WriteResult head = writer.Write(wire.data(), head_size);
    if (head.error || head.count != head_size) {
        return false;
    }
    const std::size_t body_size = wire.size() - head_size;
    if (body_size != 0) {
        const streams::WriteResult body =
            writer.Write(wire.data() + head_size, body_size);
        if (body.error || body.count != body_size) {
            return false;
        }
    }
    return writer.FlushBatch().ok();
}

}  // namespace

const char* HttpServerErrorName(HttpServerError error) noexcept {
    switch (error) {
        case HttpServerError::None: return "None";
        case HttpServerError::InvalidAddress: return "InvalidAddress";
        case HttpServerError::BindFailed: return "BindFailed";
        case HttpServerError::ListenFailed: return "ListenFailed";
        case HttpServerError::NotListening: return "NotListening";
        case HttpServerError::AlreadyListening: return "AlreadyListening";
        case HttpServerError::AlreadyRunning: return "AlreadyRunning";
        case HttpServerError::UnsupportedModel: return "UnsupportedModel";
        case HttpServerError::ThreadFailure: return "ThreadFailure";
    }
    return "Unknown";
}

HttpServerResult HttpServerResult::Failure(HttpServerError error, std::string message,
                                           int system_error) {
    HttpServerResult result;
    result.error = error;
    result.system_error = system_error;
    result.message = std::move(message);
    return result;
}

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() { Stop(); }

HttpServerResult HttpServer::Listen(std::string address, HttpServerConfig config) {
    if (m_running.load(std::memory_order_acquire)) {
        return HttpServerResult::Failure(HttpServerError::AlreadyRunning, "服务已经启动");
    }
    if (m_listener) {
        return HttpServerResult::Failure(HttpServerError::AlreadyListening,
                                        "监听 socket 已经建立");
    }
    if (address.empty()) {
        return HttpServerResult::Failure(HttpServerError::InvalidAddress, "监听地址为空");
    }
    // AI_ADDRCONFIG 会按本机网卡过滤地址族：127.0.0.1 一定可用，IPv6 需本机启用。
    const auto resolved =
        net::Address::LookupAny(address, AF_UNSPEC, SOCK_STREAM, IPPROTO_TCP);
    if (!resolved) {
        return HttpServerResult::Failure(HttpServerError::InvalidAddress,
                                        "无法解析监听地址: " + address);
    }
    config = NormalizeConfig(std::move(config));
    auto listener = net::Socket::CreateTCP(resolved->family());
    if (!listener) {
        return HttpServerResult::Failure(HttpServerError::BindFailed,
                                        "创建监听 socket 失败", errno);
    }
    int reuse = 1;
    (void)listener->set_option(SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (!listener->bind(*resolved)) {
        const int error = listener->last_error();
        return HttpServerResult::Failure(HttpServerError::BindFailed,
                                        "bind(" + address + ") 失败: " + SystemErrorText(error),
                                        error);
    }
    if (!listener->listen(config.backlog)) {
        const int error = listener->last_error();
        return HttpServerResult::Failure(HttpServerError::ListenFailed,
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
    return HttpServerResult::Success();
}

HttpServerResult HttpServer::Listen(const std::string& host, std::uint16_t port,
                                    HttpServerConfig config) {
    std::string address = host;
    // IPv6 字面量必须加方括号，否则 "::1:8080" 会被拆成 host="::1:8080"。
    if (address.find(':') != std::string::npos &&
        (address.empty() || address.front() != '[')) {
        address = "[" + address + "]";
    }
    address += ":" + std::to_string(port);
    return Listen(std::move(address), std::move(config));
}

void HttpServer::OnRequest(RequestHandler handler) {
    std::lock_guard<std::mutex> lock(m_route_mutex);
    m_request_handler = std::move(handler);
}

void HttpServer::AsSse(std::string path, SseHandler handler) {
    if (path.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_route_mutex);
    for (auto& route : m_sse_routes) {
        if (route.first == path) {
            route.second = std::move(handler);
            return;
        }
    }
    m_sse_routes.emplace_back(std::move(path), std::move(handler));
}

HttpServerResult HttpServer::Start() {
    if (!m_listener) {
        return HttpServerResult::Failure(HttpServerError::NotListening,
                                        "尚未调用 Listen()");
    }
    if (m_running.load(std::memory_order_acquire)) {
        return HttpServerResult::Failure(HttpServerError::AlreadyRunning, "服务已经启动");
    }
    if (m_config.model == HttpConcurrencyModel::NonBlocking) {
        return HttpServerResult::Failure(HttpServerError::UnsupportedModel,
                                        "NonBlocking 模型尚未实现，当前仅支持 Blocking 线程模型");
    }
    m_stopping.store(false, std::memory_order_release);
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
        return HttpServerResult::Failure(HttpServerError::ThreadFailure,
                                        std::string("创建服务线程失败: ") + error.what());
    }
    return HttpServerResult::Success();
}

void HttpServer::Stop() noexcept {
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

    // step2: shutdown 所有活动连接，唤醒阻塞在 recv / SSE 写入上的 worker。
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

std::string HttpServer::LocalAddress() const {
    return m_local_address;
}

HttpServer::WriteStats HttpServer::GetWriteStats() const noexcept {
    WriteStats stats;
    stats.responses = m_write_responses.load(std::memory_order_acquire);
    stats.writev_calls = m_write_batches.load(std::memory_order_acquire);
    stats.direct_writes = m_write_direct.load(std::memory_order_acquire);
    stats.write_errors = m_write_errors.load(std::memory_order_acquire);
    return stats;
}

void HttpServer::AcceptLoop() {
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

void HttpServer::WorkerLoop() {
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

bool HttpServer::ReadRequest(const net::Socket::Ptr& connection, http::Parser& parser,
                             std::string& pending, http::Request& request) {
    for (;;) {
        if (m_stopping.load(std::memory_order_acquire)) {
            return false;
        }
        if (!pending.empty()) {
            const auto parsed = parser.feed(pending, request);
            pending.clear();
            if (parsed.state == http::ParseState::Error) {
                return false;
            }
            if (parsed.state == http::ParseState::Complete) {
                return true;
            }
            continue;
        }
        std::array<char, 8192> buffer{};
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

void HttpServer::ServeConnection(const net::Socket::Ptr& connection) {
    http::ParserLimits limits;
    limits.max_header_bytes = m_config.max_header_bytes;
    limits.max_body_bytes = m_config.max_body_bytes;
    http::Parser parser(limits);
    std::string pending;
    net::SocketStream stream(connection, false);
    // B-06：可选的 bufio 批量写端。`bufio::Writer` 按引用持有
    // `SocketBatchWriter`，因此先声明 sink 再声明 writer，析构顺序反过来，
    // writer 先于 sink 销毁。
    std::optional<net::SocketBatchWriter> batch_sink;
    std::optional<net::BufferedSocketWriter> buffered;
    if (m_config.use_bufio) {
        batch_sink.emplace(connection);
        buffered.emplace(*batch_sink, m_config.bufio_capacity);
        net::ApplyProtocolBatchDefaults(*buffered);
    }

    for (std::size_t served = 0; served < m_config.max_requests_per_connection; ++served) {
        if (m_stopping.load(std::memory_order_acquire)) {
            return;
        }
        http::Request request;
        if (!ReadRequest(connection, parser, pending, request)) {
            return;
        }
        // 同一 recv 里可能还有流水线的下一个请求，留给下一轮。
        pending = parser.take_remaining();

        const SseHandler sse_handler = FindSseHandler(request.path());
        if (sse_handler) {
            ServeSse(connection, request, sse_handler);
            return;
        }

        http::Response response;
        const RequestHandler request_handler = FindRequestHandler();
        if (request_handler) {
            try {
                response = request_handler(request);
            } catch (...) {
                // handler 异常不能穿透线程边界：降级 500 并关闭连接。
                response = http::Response{};
                response.status = http::Status::InternalError;
                response.close = true;
            }
        } else {
            response.status = http::Status::NotFound;
        }
        if (m_stopping.load(std::memory_order_acquire)) {
            response.close = true;
        }
        const std::string wire =
            response.to_string(request.method == http::Method::Head);
        if (wire.empty()) {
            return;
        }
        const bool buffered_path =
            m_config.use_bufio && wire.size() <= m_config.bufio_capacity;
        if (buffered_path) {
            if (!WriteBufferedResponse(*buffered, wire)) {
                m_write_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            m_write_responses.fetch_add(1, std::memory_order_relaxed);
            m_write_batches.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (!stream.write_all(wire.data(), wire.size())) {
                if (m_config.use_bufio) {
                    m_write_errors.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
            m_write_responses.fetch_add(1, std::memory_order_relaxed);
            if (m_config.use_bufio) {
                m_write_direct.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (request.close || response.close) {
            return;
        }
    }
}

void HttpServer::ServeSse(const net::Socket::Ptr& connection,
                          const http::Request& request, const SseHandler& handler) {
    sse::Stream stream(connection, {}, m_config.sse_options);
    if (!stream.start().ok()) {
        return;
    }
    try {
        handler(request, stream);
    } catch (...) {
        // SSE handler 异常：不再尝试继续推流，走 close() 尽力写终止块。
    }
    (void)stream.close();
}

HttpServer::SseHandler HttpServer::FindSseHandler(const std::string& path) const {
    std::lock_guard<std::mutex> lock(m_route_mutex);
    for (const auto& route : m_sse_routes) {
        if (route.first == path) {
            return route.second;
        }
    }
    return {};
}

HttpServer::RequestHandler HttpServer::FindRequestHandler() const {
    std::lock_guard<std::mutex> lock(m_route_mutex);
    return m_request_handler;
}

void HttpServer::CloseAllConnections() noexcept {
    // shutdown(2) 是立即返回的系统调用，持锁遍历不会阻塞在慢连接上；
    // 不复制 shared_ptr 是为了让本函数在 Stop() 的 noexcept 路径里零分配。
    std::lock_guard<std::mutex> lock(m_active_mutex);
    for (const auto& entry : m_active) {
        if (entry.second) {
            (void)entry.second->shutdown(SHUT_RDWR);
        }
    }
}

void HttpServer::JoinWorkers() noexcept {
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

void HttpServer::CloseListener() noexcept {
    if (m_listener) {
        (void)m_listener->close();
        m_listener.reset();
    }
    m_local_address.clear();
    m_port = 0;
}

}  // namespace go2cpp::bento
