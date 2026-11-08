/**
 * @file http_server.cpp
 * @brief `bento::HttpServer` 的实现：accept/worker 线程、请求循环与 SSE 桥接。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/bento/net/http_server.hpp"

#include "go2cpp/network/bufio_socket.hpp"
#include "go2cpp/platform/socket_option.hpp"

#include <sys/socket.h>

#include <netinet/tcp.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
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

/// TCP_CORK 只对「头 + 大文件」划算：合包省下的一个 TCP 段抵不过两次
/// setsockopt 的开销（实测 1 KiB/10 KiB 档因此回退 6-10%）。16 KiB 以上
/// 才启用，与 nginx 在大响应上收益更明显的实测一致。
constexpr std::uint64_t kCorkMinFileBytes = 16 * 1024;
/// 小于等于此长度的文件体走「header + body 一次写」而不是 sendfile：
/// 1 KiB 档的 sendfile 需要「头一次 write + 文件一次 sendfile」两个段，
/// 读进用户态反而能用一次 send 发完（网络往返次数不变，但段数减半）。
/// 上限与 CORK 阈值一致，再大就进入「sendfile + TCP_CORK」路径。
constexpr std::uint64_t kInlineFileBytes = 16 * 1024;

/// @brief 响应带文件体时用 sendfile(2) 发送（静态文件/Range 零拷贝）。
/// @details 头由调用方先写出；本函数只负责文件体，`HEAD` 不调用。
///          `tcp_cork` 为真时按 nginx `ngx_linux_sendfile_chain` 的做法用
///          TCP_CORK 把「响应头 + 文件体」括起来：头先进入内核发送队列但
///          不立即出包，第一次 sendfile 之后内核把两者合并成尽量少的 TCP
///          段（1 KiB 静态响应从 2 段降到 1 段）。nginx 在
///          `header.count != 0 && cl->buf->in_file` 时无条件打开 nopush
///          （即 TCP_CORK），并在文件发完后关闭，这里同构。
bool SendFileBody(const net::Socket::Ptr& connection, const http::FileBody& file,
                  bool tcp_cork) {
    if (file.length == 0) {
        return true;
    }
    const int fd = ::open(file.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    bool corked = false;
#if defined(TCP_CORK)
    // TCP_CORK 与 TCP_NODELAY 互斥：打开 CORK 后内核会等积累到 MSS 或
    // uncork 才发包，这正是「头 + 文件」合包需要的语义。
    int enable = 1;
    if (tcp_cork &&
        connection->set_option(IPPROTO_TCP, TCP_CORK, &enable, sizeof(enable))) {
        corked = true;
    }
#else
    (void)tcp_cork;
#endif
    off_t offset = static_cast<off_t>(file.offset);
    std::uint64_t remaining = file.length;
    bool ok = true;
    while (remaining > 0) {
        const auto written = connection->send_file(
            fd, &offset, static_cast<std::size_t>(remaining));
        if (written.transferred <= 0) {
            ok = false;
            break;
        }
        remaining -= static_cast<std::uint64_t>(written.transferred);
    }
    (void)::close(fd);
#if defined(TCP_CORK)
    if (corked) {
        int disable = 0;
        // uncork 失败不影响已发送的数据；连接会由调用方按写错误处理。
        (void)connection->set_option(IPPROTO_TCP, TCP_CORK, &disable,
                                     sizeof(disable));
    }
#endif
    return ok;
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
    // Windows 的 SO_REUSEADDR 允许端口劫持：platform 封装在 Windows 上改用
    // SO_EXCLUSIVEADDRUSE + SO_LINGER{1,0}。原实现忽略失败，保持同样语义。
    (void)platform::SetReuseAddr(listener->fd());
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
    std::unique_lock<std::shared_mutex> lock(m_route_mutex);
    m_request_handler = std::move(handler);
}

/**
 * @brief 向路由表注册一条「方法 + 路径模式」规则，运行期可安全并发注册。
 * @param method [in] HTTP 方法——按值传入，精确匹配；HEAD 未单独注册时由 MatchRoute 回退到同路径 GET。
 * @param pattern [in] 路由模式——按值移交（内部 move），支持路径参数语法；调用后调用方不得再引用。
 * @param handler [in] 请求处理函数——按值移交，需可拷贝；不可为空，空 handler 会产生「命中但不可调用」的路由。
 * @return 无返回值；注册总是成功（重复模式由路由器按注册顺序决定优先级）。
 * ## 定位
 * 路由表唯一写入口，与 MatchRoute（读锁）构成读写锁对。
 * 若漏锁或与匹配并发写，会出现迭代器失效/数据竞争，表现为随机 404 或崩溃。
 * ## 步骤
 * step1. 取 m_route_mutex 写锁，阻塞所有匹配侧读者；
 * step2. 把模式与 handler 移交给 m_router，由路由器维护匹配顺序。
 * ## 并发/失败语义
 * - 调用约束：任意线程可调用；不要在请求处理函数内部对本 server 做阻塞式大规模注册。
 * - 原子性：单条路由的插入在写锁内完成，读者要么看到整条路由，要么看不到。
 * - 失败语义：不抛异常（容器分配失败会 terminate）；注册后立即对后续请求生效。
 * - 不变量：返回后该 (method, pattern) 一定能被 MatchRoute 命中（除非被更新的同模式规则覆盖）。
 */
void HttpServer::Route(http::Method method, std::string pattern,
                       RequestHandler handler) {
    std::unique_lock<std::shared_mutex> lock(m_route_mutex);
    m_router.add(method, std::move(pattern), std::move(handler));
}

bool HttpServer::MatchRoute(http::Request& request, RequestHandler* handler) const {
    std::shared_lock<std::shared_mutex> lock(m_route_mutex);
    if (m_router.empty()) {
        return false;
    }
    const http::Handler* matched =
        m_router.match(request.method, request.path_view(), &request.path_values);
    // HEAD 是 GET 的无体版本：未单独注册 HEAD 时回退到同一路径的 GET 路由。
    if (matched == nullptr && request.method == http::Method::Head) {
        matched = m_router.match(http::Method::Get, request.path_view(),
                                 &request.path_values);
    }
    if (matched == nullptr) {
        return false;
    }
    if (handler != nullptr) {
        *handler = *matched;
    }
    return true;
}

void HttpServer::AsSse(std::string path, SseHandler handler) {
    if (path.empty()) {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(m_route_mutex);
    for (auto& route : m_sse_routes) {
        if (route.first == path) {
            route.second = std::move(handler);
            return;
        }
    }
    m_sse_routes.emplace_back(std::move(path), std::move(handler));
}

/**
 * @brief 校验监听与运行状态后启动 Fiber 调度器，并异步投递 AcceptLoop。
 * @return HttpServerResult：
 * - ok()==true：调度器已运行、accept 循环已投递——调用方**必须**在销毁前调用 Stop()；
 * - NotListening：未先调用 Listen()——调用方**必须**先 Listen 再 Start；
 * - AlreadyRunning：重复启动——调用方**不要**再次 Start，直接复用现有实例；
 * - UnsupportedModel/ThreadFailure：模型未实现或线程创建失败——调用方**必须**终止启动流程并销毁实例。
 * ## 定位
 * 服务启动的唯一闸门：running 标志与调度器生命周期在这里绑定。
 * 若先置 running 再建调度器且失败不回滚，Stop 会 join 一个不存在的 manager，
 * 或对外表现为「running 但永不 accept」。
 * ## 步骤
 * step1. 前置校验：listener 必须存在、不能已在运行、模型必须支持；
 * step2. 先复位 stopping 再发布 running，保证上一轮 Stop 的残留状态不阻塞新循环；
 * step3. 按 worker_threads 构造 IOManager（每连接一个 Fiber，连接数不受线程池限制）；
 * step4. 启动失败回滚 running 并返回 ThreadFailure；
 * step5. 保存 manager 并 go(AcceptLoop)，把 accept 与连接处理都放到 Fiber 上。
 * ## 并发/失败语义
 * - 调用约束：任意线程可调用，但应在 Listen 之后、对外提供服务之前调用；Stop 与 Start 不可并发。
 * - 原子性：running 的置位先于调度器启动；失败时显式回滚，不留下「假运行」状态。
 * - 失败语义：不抛异常，所有失败经 HttpServerResult 返回；失败后对象仍可重新 Listen/Start。
 * - 不变量：返回 ok 时 m_manager 非空且 AcceptLoop 已被调度；返回失败时 m_manager 为空、running==false。
 */
HttpServerResult HttpServer::Start() {
    if (!m_listener) {
        return HttpServerResult::Failure(HttpServerError::NotListening,
                                        "尚未调用 Listen()");
    }
    if (m_running.load(std::memory_order_acquire)) {
        return HttpServerResult::Failure(HttpServerError::AlreadyRunning,
                                        "服务已经启动");
    }
    if (m_config.model == HttpConcurrencyModel::NonBlocking) {
        return HttpServerResult::Failure(
            HttpServerError::UnsupportedModel,
            "NonBlocking 模型尚未实现，当前为每连接一 Fiber 的调度模型");
    }
    m_stopping.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);

    // 每连接一个 Fiber（与 Go net/http 的 goroutine-per-connection 对齐）：
    // worker_threads 只决定 OS 线程（P）数量，连接数不再受线程池大小限制，
    // 从而消除「4 条 keep-alive 连接 + 2 个 worker 时后两条连接饥饿」的问题。
    SchedulerConfig config;
    config.processor_count = m_config.worker_threads;
    config.max_workers = m_config.worker_threads;
    config.min_workers = m_config.worker_threads;
    config.collect_metrics = false;
    auto manager = std::make_shared<io::IOManager>(config);
    if (!manager->start()) {
        m_running.store(false, std::memory_order_release);
        return HttpServerResult::Failure(HttpServerError::ThreadFailure,
                                        "创建 Fiber 调度器失败");
    }
    m_manager = std::move(manager);
    m_manager->go([this]() { AcceptLoop(); });
    return HttpServerResult::Success();
}

/**
 * @brief 幂等地停止 HTTP 服务：断开活动连接、关闭监听、停调度器并释放资源（noexcept）。
 * @return 无返回值；无论此前是否运行都保证返回后不再接受新连接、不再有该实例的 Fiber 在跑。
 * ## 定位
 * 服务生命周期的唯一收尾点，析构函数也会调用它。
 * 顺序写错会死锁（先 join 再 shutdown）或让 accept 线程永久阻塞在 listen socket 上；
 * 由于是 noexcept，任何异常都必须在这里吞掉，否则析构期间 terminate。
 * ## 步骤
 * step1. 交换 running/stopping 并判断是否无事可做，保证重复调用幂等；
 * step2. shutdown 所有活动连接，唤醒阻塞在 recv 上的连接 Fiber；
 * step3. shutdown 监听 socket，让 accept 的等待尽快返回；
 * step4. 停止 Fiber 调度器（AcceptLoop 最多一个 accept 超时周期内退出）；
 * step5. 兜底再扫一次活动表（覆盖「扫描后才登记」的竞态），最后释放监听 socket。
 * ## 并发/失败语义
 * - 调用约束：可与 Start 之外的任意线程调用；不要在连接 Fiber 内销毁 server（会 join 自身）。
 * - 原子性：running 的交换保证只有一个线程执行真正的收尾；其余调用直接返回。
 * - 失败语义：noexcept；shutdown/close 的错误被忽略，只为唤醒等待者。
 * - 不变量：返回后 m_manager 为空、listener 已释放、活动连接表为空。
 */
void HttpServer::Stop() noexcept {
    const bool had_listener = static_cast<bool>(m_listener);
    const bool was_running = m_running.exchange(false, std::memory_order_acq_rel);
    if (!was_running && !had_listener) {
        return;
    }
    m_stopping.store(true, std::memory_order_release);

    // step1: shutdown 所有活动连接，唤醒阻塞在 recv 上的连接 Fiber。
    CloseAllConnections();
    // step2: shutdown 监听 socket，让 accept 的等待尽快返回。
    if (m_listener) {
        (void)m_listener->shutdown(SHUT_RDWR);
    }
    // step3: 停止 Fiber 调度器。AcceptLoop 最多在一个 accept 超时周期内退出；
    // 连接 Fiber 在 step1 之后读到 EOF 自行结束。
    auto manager = m_manager;
    m_manager.reset();
    if (manager) {
        manager->shutdown();
    }
    // step4: 兜底再唤醒一次（覆盖扫描活动表之后才登记、且已被 shutdown 的竞态）。
    CloseAllConnections();
    // step5: 释放监听 socket。
    CloseListener();
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
        // 先登记再起 Fiber：Stop() 扫描活动表时不会漏掉正在启动的连接。
        {
            std::lock_guard<std::mutex> lock(m_active_mutex);
            m_active.emplace(connection.get(), connection);
        }
        m_active_count.fetch_add(1, std::memory_order_acq_rel);
        if (!m_manager) {
            (void)connection->close();
            m_active_count.fetch_sub(1, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> lock(m_active_mutex);
            m_active.erase(connection.get());
            return;
        }
        // 一条连接一个 Fiber：同连接内 keep-alive 请求串行，不同连接完全并行。
        m_manager->go([this, connection]() {
            if (!m_stopping.load(std::memory_order_acquire)) {
                ServeConnection(connection);
            }
            (void)connection->close();
            {
                std::lock_guard<std::mutex> lock(m_active_mutex);
                m_active.erase(connection.get());
            }
            m_active_count.fetch_sub(1, std::memory_order_acq_rel);
        });
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
                             std::string& pending, http::Request& request, char* buffer,
                             std::size_t capacity) {
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
        const auto count = connection->recv(buffer, capacity);
        if (count.transferred <= 0) {
            return false;
        }
        const auto parsed = parser.feed(
            std::string_view(buffer, static_cast<std::size_t>(count.transferred)),
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
    // 连接级读缓冲：一次 recv 16KB，避免每次都在 Fiber 栈上分配 8KB
    // （Fiber 初始栈只有 4KB，栈数组会触发栈增长甚至溢出）。
    std::vector<char> read_buffer(16 * 1024);
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
    // 偷自 nginx `ngx_http_write_filter` 的 postpone_output：同一次 recv 里
    // 连续解析出的流水线响应不逐条 write，先攒在用户态，攒到上限、必须阻塞
    // 读、或遇到文件体时再一次性写出。pipeline=N 时把 N 次 send / N 个 TCP
    // 段压成 1 次 send（nginx 在小响应上同样是用缓冲换系统调用）。
    constexpr std::size_t kWriteBatchBytes = 16 * 1024;
    std::string out;
    out.reserve(kWriteBatchBytes);
    // 小文件内联发送的复用缓冲：每个连接只分配一次，避免每请求 malloc。
    std::vector<char> file_buffer;
    // 把攒着的响应一次写出；返回 false 表示连接已不可写。失败也清空缓冲
    // （连接即将关闭，不会再重试）。
    const auto flush_out = [&]() -> bool {
        if (out.empty()) {
            return true;
        }
        const bool written = stream.write_all(out.data(), out.size());
        out.clear();
        return written;
    };

    for (std::size_t served = 0; served < m_config.max_requests_per_connection; ++served) {
        if (m_stopping.load(std::memory_order_acquire)) {
            return;
        }
        http::Request request;
        // 读出一个完整请求：pending 里还有字节就先解析；解析不出完整请求
        // 说明这一批流水线请求处理完了，再阻塞 recv。recv 之前必须先把攒着
        // 的响应发出去，否则对端等响应、我们等请求会互等。
        for (;;) {
            if (!pending.empty()) {
                const auto parsed = parser.feed(pending, request);
                pending.clear();
                if (parsed.state == http::ParseState::Error) {
                    (void)flush_out();
                    return;
                }
                if (parsed.state == http::ParseState::Complete) {
                    break;
                }
                continue;
            }
            if (!flush_out()) {
                return;
            }
            const auto count = connection->recv(read_buffer.data(), read_buffer.size());
            if (count.transferred <= 0) {
                (void)flush_out();
                return;
            }
            const auto parsed = parser.feed(
                std::string_view(read_buffer.data(),
                                 static_cast<std::size_t>(count.transferred)),
                request);
            if (parsed.state == http::ParseState::Error) {
                (void)flush_out();
                return;
            }
            if (parsed.state == http::ParseState::Complete) {
                break;
            }
        }
        // 同一 recv 里可能还有流水线的下一个请求，留给下一轮。
        pending = parser.take_remaining();

        const SseHandler sse_handler = FindSseHandler(request.path_view());
        if (sse_handler) {
            (void)flush_out();
            ServeSse(connection, request, sse_handler);
            return;
        }

        http::Response response;
        RequestHandler request_handler;
        if (!MatchRoute(request, &request_handler)) {
            request_handler = FindRequestHandler();
        }
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
        const bool head_request = request.method == http::Method::Head;
        const std::string wire = response.to_string(head_request);
        if (wire.empty()) {
            (void)flush_out();
            return;
        }
        const bool has_file = !head_request && response.has_file();
        if (m_config.use_bufio) {
            // 原有 bufio 路径：逐响应 flush，保持 writev_calls 语义不变。
            if (wire.size() <= m_config.bufio_capacity) {
                if (!WriteBufferedResponse(*buffered, wire)) {
                    m_write_errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                m_write_batches.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (!stream.write_all(wire.data(), wire.size())) {
                    m_write_errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                m_write_direct.fetch_add(1, std::memory_order_relaxed);
            }
            // 文件体在两条 bufio 路径之后统一发送（与原实现一致）：
            // 头已经 flush 到内核，sendfile 才能保证字节顺序。
            if (has_file &&
                !SendFileBody(connection, *response.file,
                              response.file->length >= kCorkMinFileBytes)) {
                m_write_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        } else if (has_file && response.file->length <= kInlineFileBytes) {
            // 小文件走「header + body 一次写出」：nginx `ngx_writev_chain`
            // 的核心是让头和内容处在同一次写里，这里把这一小段 pread 进
            // 用户态、与响应头拼进同一个批量缓冲，一次 send 发出 →
            // 1 个 TCP 段 + 1 次写系统调用（省掉头/体两段与 setsockopt）。
            const std::size_t body_size =
                static_cast<std::size_t>(response.file->length);
            file_buffer.resize(body_size);
            const int file_fd =
                ::open(response.file->path.c_str(), O_RDONLY | O_CLOEXEC);
            bool body_ok = file_fd >= 0;
            if (body_ok) {
                const ssize_t got = ::pread(
                    file_fd, file_buffer.data(), body_size,
                    static_cast<off_t>(response.file->offset));
                body_ok = got == static_cast<ssize_t>(body_size);
                (void)::close(file_fd);
            }
            if (!body_ok) {
                m_write_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            out.append(wire);
            out.append(file_buffer.data(), body_size);
            if (out.size() >= kWriteBatchBytes && !flush_out()) {
                m_write_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        } else if (has_file) {
            // 文件体必须严格按序：先把攒着的响应落地，再写「头 + 文件」。
            if (!flush_out() || !stream.write_all(wire.data(), wire.size())) {
                m_write_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (!SendFileBody(connection, *response.file,
                              response.file->length >= kCorkMinFileBytes)) {
                m_write_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        } else {
            out.append(wire);
            if (out.size() >= kWriteBatchBytes && !flush_out()) {
                m_write_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        m_write_responses.fetch_add(1, std::memory_order_relaxed);
        if (request.close || response.close) {
            (void)flush_out();
            return;
        }
    }
    (void)flush_out();
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

HttpServer::SseHandler HttpServer::FindSseHandler(std::string_view path) const {
    std::shared_lock<std::shared_mutex> lock(m_route_mutex);
    if (m_sse_routes.empty()) {
        return {};
    }
    for (const auto& route : m_sse_routes) {
        if (std::string_view(route.first) == path) {
            return route.second;
        }
    }
    return {};
}

HttpServer::RequestHandler HttpServer::FindRequestHandler() const {
    std::shared_lock<std::shared_mutex> lock(m_route_mutex);
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
