/**
 * @file gmp_vs_epoll_fair_benchmark.cpp
 * @brief 公平对比用的 GMP / epoll 压测程序（协议与业务与既有 benchmark 完全一致）。
 *
 * ## 为什么要单独写一份
 *
 * 既有 `gmp_vs_thread_epoll_benchmark` 的 epoll 池模型有两个会**夸大 GMP 优势**
 * 的实现细节：
 *   1. 默认 256 个工作线程（12 核上严重超配），全部抢同一把 `m_mutex` + cv
 *      投递任务、同一把 `m_mutex` 收集完成、每个完成再写一次 eventfd；
 *   2. 每请求两次「IO 线程 ↔ 工作线程」跨线程交接。
 *
 * 本程序把 epoll 模型实现成和 GMP 同构的形态：**K 个 IO 线程，各自持有独立
 * epoll 实例，就绪事件在本线程内联完成读/解析/业务/写，无跨线程投递、无全局
 * 锁**（K 默认 = 核数）。两边跑同一个协议、同一份业务函数、同一个客户端。
 *
 * ## 客户端
 *
 * 请求到达按指数分布（真实流量模型），body 大小在 [min,max] 内随机，
 * 支持 pipeline depth 与「不限速冲击上限」。warmup 之后才计入统计，避免把
 * 连接建立阶段摊进每请求 CPU。输出 RESULT JSON 与 MEASURE_START/MEASURE_END
 * 标记，供驱动脚本按稳态窗口采样服务端 CPU。
 *
 * 用法：
 *   server --model gmp|epoll [--port 0] [--workers 12]
 *   client --port N [--conns 2000] [--depth 1] [--rate 0]
 *          [--body-min 64] [--body-max 4096] [--mixed]
 *          [--warmup-ms 2000] [--duration-ms 10000] [--seed 1]
 */
#include "go2cpp/network/address.hpp"
#include "go2cpp/network/io.hpp"
#include "go2cpp/network/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t kMaxFrame = 8U << 20;
std::atomic<bool> g_stop{false};

std::size_t bench_stack_kb() {
    const char* env = std::getenv("GO2CPP_BENCH_STACK_KB");
    if (env == nullptr || *env == 0) {
        return 128;
    }
    return static_cast<std::size_t>(std::strtoul(env, nullptr, 10));
}
int g_downstream_us = 0;  // 模拟下游阻塞等待（微秒），0 = 关闭

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void put_u32(char* dst, std::uint32_t value) { std::memcpy(dst, &value, 4); }
std::uint32_t get_u32(const char* src) {
    std::uint32_t value = 0;
    std::memcpy(&value, src, 4);
    return value;
}

/// @brief 业务函数：与既有 benchmark 的 process_request **逐行一致**，
///        保证 GMP（旧 server）与 epoll（本程序）做完全相同的工作量。
std::string process_body(std::uint8_t opcode, std::string_view body) {
    static thread_local std::uint32_t seed = 0x9e3779b9u;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    const std::uint32_t count = seed % 4097u;
    if (count > 1) {
        static thread_local std::vector<std::int32_t> scratch;
        if (scratch.size() < count) {
            scratch.resize(count);
        }
        std::uint32_t r = seed | 1u;
        for (std::uint32_t i = 0; i < count; ++i) {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            scratch[i] = static_cast<std::int32_t>(r);
        }
        std::sort(scratch.begin(), scratch.begin() + count);
        if (scratch[0] == std::numeric_limits<std::int32_t>::min()) {
            return {};
        }
    }
    switch (opcode) {
        case 1: return std::string(body);              // echo
        case 2: return std::string(1, 'A');            // read → 1B
        case 3: return std::string(4096, 'W');         // write → 4KB
        case 5: return std::string(1, 'P');            // ping → 1B
        default: return {};
    }
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// ===========================================================================
// GMP 服务端：IOManager + 每连接一个 Fiber（同步写法）
// ===========================================================================

bool send_all(go2cpp::net::Socket& socket, const void* src, std::size_t length) {
    const char* bytes = static_cast<const char*>(src);
    std::size_t sent = 0;
    while (sent < length) {
        const auto result = socket.send(bytes + sent, length - sent);
        if (result.transferred > 0) {
            sent += static_cast<std::size_t>(result.transferred);
            continue;
        }
        if (result.error == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void serve_gmp_connection(go2cpp::net::Socket::Ptr socket) {
    // 与 epoll 模型同构的读取方式：一次 recv 读满 64KB 缓冲，再按帧解析。
    // 旧写法“精确读 4 字节 header、再读 body”每请求要 2 次 recvmsg，而
    // epoll 模型 1 次 recvfrom 就能读完——这是测试写法差异，不是模型差异。
    char buffer[8 * 1024];
    std::string in;
    in.reserve(16 * 1024);
    std::size_t in_off = 0;
    std::string out;
    out.reserve(16 * 1024);
    for (;;) {
        out.clear();
        for (;;) {
            const std::size_t available = in.size() - in_off;
            if (available < 4) {
                break;
            }
            const std::uint32_t length = get_u32(in.data() + in_off);
            if (length == 0 || length > kMaxFrame) {
                socket->close();
                return;
            }
            if (available < static_cast<std::size_t>(length) + 4) {
                break;
            }
            const char* body = in.data() + in_off + 4;
            if (g_downstream_us > 0) {
                ::usleep(static_cast<unsigned>(g_downstream_us));
            }
            const std::string response = process_body(
                static_cast<std::uint8_t>(body[0]),
                std::string_view(body + 1, length - 1));
            char header[4];
            put_u32(header, static_cast<std::uint32_t>(response.size()));
            out.append(header, sizeof(header));
            out.append(response);
            in_off += static_cast<std::size_t>(length) + 4;
        }
        // 批量发送：一次 recv 解析出的所有帧合并成一次 send_all，
        // 减少 pipeline 场景下每帧一次 sendmsg 的系统调用。
        if (!out.empty() && !send_all(*socket, out.data(), out.size())) {
            socket->close();
            return;
        }
        if (in_off == in.size()) {
            in.clear();
            in_off = 0;
        } else if (in_off > (1U << 20)) {
            in.erase(0, in_off);
            in_off = 0;
        }
        const auto result = socket->recv(buffer, sizeof(buffer));
        if (result.transferred > 0) {
            in.append(buffer, static_cast<std::size_t>(result.transferred));
            continue;
        }
        if (result.transferred == 0 && result.error == 0) {
            socket->close();
            return;
        }
        if (result.error == EINTR) {
            continue;
        }
        socket->close();
        return;
    }
}

int run_gmp_server(std::uint16_t port, std::size_t workers) {
    go2cpp::SchedulerConfig config;
    config.processor_count = static_cast<std::uint32_t>(workers);
    config.min_workers = workers;
    config.collect_metrics = false;
    config.fiber_stack_size = bench_stack_kb() * 1024;
    go2cpp::IOManager manager(config);
    if (!manager.start()) {
        std::fputs("[server] IOManager 启动失败\n", stderr);
        return 1;
    }
    auto listener = go2cpp::net::ListenTCP("127.0.0.1", port, 8192);
    if (!listener) {
        std::fputs("[server] 监听失败\n", stderr);
        return 1;
    }
    std::uint16_t actual = 0;
    if (auto address = std::dynamic_pointer_cast<go2cpp::net::IPv4Address>(
            listener->local_address())) {
        actual = address->port();
    }
    std::printf("READY pid=%d port=%u\n", static_cast<int>(::getpid()),
                static_cast<unsigned>(actual));
    std::fflush(stdout);
    while (!g_stop.load(std::memory_order_relaxed)) {
        auto socket = listener->accept(std::chrono::milliseconds(200));
        if (!socket) {
            continue;
        }
        manager.go([socket] { serve_gmp_connection(socket); });
    }
    manager.shutdown();
    return 0;
}

// ===========================================================================
// epoll 服务端：K 个独立 IO 线程，各自 epoll + 内联处理
// ===========================================================================

struct EConn {
    int fd{-1};
    std::string in;
    std::size_t in_off{0};
    std::string out;
    std::size_t out_off{0};
    bool want_out{false};
};

struct EWorker {
    int epfd{-1};
    int wakefd{-1};
    std::mutex mutex;
    std::vector<int> incoming;
    std::unordered_map<int, std::unique_ptr<EConn>> conns;
    std::thread thread;
};

void e_close(EConn& conn) {
    if (conn.fd >= 0) {
        ::close(conn.fd);
        conn.fd = -1;
    }
}

bool e_flush(EWorker& worker, EConn& conn) {
    while (conn.out_off < conn.out.size()) {
        const ssize_t n = ::send(conn.fd, conn.out.data() + conn.out_off,
                                 conn.out.size() - conn.out_off, MSG_NOSIGNAL);
        if (n > 0) {
            conn.out_off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!conn.want_out) {
                conn.want_out = true;
                epoll_event event{};
                event.events = EPOLLIN | EPOLLOUT;
                event.data.fd = conn.fd;
                (void)::epoll_ctl(worker.epfd, EPOLL_CTL_MOD, conn.fd, &event);
            }
            return true;
        }
        return false;
    }
    conn.out.clear();
    conn.out_off = 0;
    if (conn.want_out) {
        conn.want_out = false;
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = conn.fd;
        (void)::epoll_ctl(worker.epfd, EPOLL_CTL_MOD, conn.fd, &event);
    }
    return true;
}

void e_handle(EWorker& worker, int fd, std::uint32_t mask) {
    const auto found = worker.conns.find(fd);
    if (found == worker.conns.end()) {
        return;
    }
    EConn& conn = *found->second;
    const bool closed = (mask & (EPOLLERR | EPOLLHUP)) != 0;
    if ((mask & EPOLLOUT) != 0 && !e_flush(worker, conn)) {
        e_close(conn);
    }
    if ((mask & EPOLLIN) != 0 || closed) {
        char buffer[8 * 1024];
        for (;;) {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                conn.in.append(buffer, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                e_close(conn);
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            e_close(conn);
            break;
        }
    }
    if (conn.fd < 0) {
        worker.conns.erase(found);
        return;
    }
    // 解析并内联处理所有完整帧。
    for (;;) {
        const std::size_t available = conn.in.size() - conn.in_off;
        if (available < 4) {
            break;
        }
        const std::uint32_t length = get_u32(conn.in.data() + conn.in_off);
        if (length == 0 || length > kMaxFrame) {
            e_close(conn);
            worker.conns.erase(found);
            return;
        }
        if (available < static_cast<std::size_t>(length) + 4) {
            break;
        }
        const char* body = conn.in.data() + conn.in_off + 4;
        if (g_downstream_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(g_downstream_us));
        }
        const std::string response = process_body(
            static_cast<std::uint8_t>(body[0]), std::string_view(body + 1, length - 1));
        char header[4];
        put_u32(header, static_cast<std::uint32_t>(response.size()));
        conn.out.append(header, sizeof(header));
        conn.out.append(response);
        conn.in_off += static_cast<std::size_t>(length) + 4;
    }
    if (conn.in_off == conn.in.size()) {
        conn.in.clear();
        conn.in_off = 0;
    } else if (conn.in_off > (1U << 20)) {
        conn.in.erase(0, conn.in_off);
        conn.in_off = 0;
    }
    if (!e_flush(worker, conn)) {
        e_close(conn);
        worker.conns.erase(found);
    }
}

void e_worker_loop(EWorker& worker) {
    epoll_event events[256];
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::vector<int> incoming;
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            incoming.swap(worker.incoming);
        }
        for (int fd : incoming) {
            auto conn = std::make_unique<EConn>();
            conn->fd = fd;
            const int fd_value = fd;
            worker.conns.emplace(fd_value, std::move(conn));
            epoll_event event{};
            event.events = EPOLLIN;
            event.data.fd = fd;
            (void)::epoll_ctl(worker.epfd, EPOLL_CTL_ADD, fd, &event);
        }
        const int ready = ::epoll_wait(worker.epfd, events, 256, 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (int i = 0; i < ready; ++i) {
            const int fd = events[i].data.fd;
            if (fd == worker.wakefd) {
                std::uint64_t value = 0;
                (void)::read(worker.wakefd, &value, sizeof(value));
                continue;
            }
            e_handle(worker, fd, events[i].events);
        }
    }
    for (auto& entry : worker.conns) {
        e_close(*entry.second);
    }
    worker.conns.clear();
}

int run_epoll_server(std::uint16_t port, std::size_t workers) {
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        return 1;
    }
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 8192) != 0) {
        ::close(listener);
        return 1;
    }
    socklen_t length = sizeof(address);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    const std::uint16_t actual = ntohs(address.sin_port);
    set_nonblocking(listener);

    std::vector<std::unique_ptr<EWorker>> pool;
    pool.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        auto worker = std::make_unique<EWorker>();
        worker->epfd = ::epoll_create1(EPOLL_CLOEXEC);
        worker->wakefd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = worker->wakefd;
        (void)::epoll_ctl(worker->epfd, EPOLL_CTL_ADD, worker->wakefd, &event);
        pool.push_back(std::move(worker));
    }
    for (auto& worker : pool) {
        EWorker* raw = worker.get();
        worker->thread = std::thread([raw] { e_worker_loop(*raw); });
    }

    std::printf("READY pid=%d port=%u\n", static_cast<int>(::getpid()),
                static_cast<unsigned>(actual));
    std::fflush(stdout);

    std::size_t next_worker = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        // 用 poll 等待监听 fd（100ms 超时），而不是 usleep 忙轮询：
        // strace 里旧实现每个 accept 失败都产生一次 clock_nanosleep。
        pollfd wait{};
        wait.fd = listener;
        wait.events = POLLIN;
        const int ready = ::poll(&wait, 1, 100);
        if (ready <= 0) {
            continue;
        }
        for (;;) {
            sockaddr_in peer{};
            socklen_t peer_length = sizeof(peer);
            const int fd = ::accept4(listener, reinterpret_cast<sockaddr*>(&peer),
                                     &peer_length, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd < 0) {
                break;
            }
            EWorker& worker = *pool[next_worker % pool.size()];
            ++next_worker;
            {
                std::lock_guard<std::mutex> lock(worker.mutex);
                worker.incoming.push_back(fd);
            }
            const std::uint64_t one = 1;
            (void)::write(worker.wakefd, &one, sizeof(one));
        }
    }
    ::close(listener);
    g_stop.store(true);
    for (auto& worker : pool) {
        const std::uint64_t one = 1;
        (void)::write(worker->wakefd, &one, sizeof(one));
    }
    for (auto& worker : pool) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
        ::close(worker->epfd);
        ::close(worker->wakefd);
    }
    return 0;
}

// ===========================================================================
// 客户端：指数到达 + 随机 payload + pipeline
// ===========================================================================

struct ClientOptions {
    std::uint16_t port{0};
    std::size_t conns{64};
    std::size_t depth{1};
    double rate{0.0};        ///< 每连接每秒请求数；0 = 响应驱动（不限速）
    std::size_t body_min{64};
    std::size_t body_max{4096};
    bool mixed{false};
    std::int64_t warmup_ms{2000};
    std::int64_t duration_ms{10000};
    std::uint64_t seed{1};
};

struct CConn {
    int fd{-1};
    bool connecting{false};
    std::string in;
    std::size_t in_off{0};
    std::string out;
    std::size_t out_off{0};
    bool want_out{false};
    std::size_t inflight{0};
    std::uint64_t next_send_ns{0};
    std::deque<std::uint64_t> send_times;
};

int run_client(const ClientOptions& options) {
    const int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        return 1;
    }
    std::vector<std::unique_ptr<CConn>> conns;
    conns.reserve(options.conns);
    std::mt19937_64 rng(options.seed * 7919 + 13);
    std::uniform_int_distribution<std::size_t> body_dist(options.body_min,
                                                         std::max(options.body_min, options.body_max));
    std::uniform_int_distribution<int> opcode_dist(1, 4);
    std::exponential_distribution<double> interval_dist(std::max(options.rate, 1e-9));

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server.sin_port = htons(options.port);

    // 建连：全部先发起非阻塞 connect，由 epoll 驱动完成。
    for (std::size_t i = 0; i < options.conns; ++i) {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) {
            return 1;
        }
        ::connect(fd, reinterpret_cast<sockaddr*>(&server), sizeof(server));
        auto conn = std::make_unique<CConn>();
        conn->fd = fd;
        conn->connecting = true;
        epoll_event event{};
        event.events = EPOLLIN | EPOLLOUT;
        event.data.u64 = i;
        (void)::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);
        conns.push_back(std::move(conn));
    }

    const std::uint64_t start = now_ns();
    const std::uint64_t measure_at = start + static_cast<std::uint64_t>(options.warmup_ms) * 1000000ULL;
    const std::uint64_t end_at = measure_at + static_cast<std::uint64_t>(options.duration_ms) * 1000000ULL;
    bool measuring = false;
    std::vector<std::uint64_t> latencies;
    latencies.reserve(1 << 20);
    std::uint64_t completed = 0;
    std::uint64_t errors = 0;
    std::uint64_t bytes = 0;

    auto make_request = [&](CConn& conn) {
        std::uint8_t opcode = 5;
        std::size_t body_size = 1;
        if (options.mixed) {
            const int pick = opcode_dist(rng);
            if (pick == 1) {
                opcode = 1;
                body_size = body_dist(rng);
            } else if (pick == 2) {
                opcode = 2;
                body_size = 4096;
            } else if (pick == 3) {
                opcode = 3;
                body_size = 1;
            } else {
                opcode = 5;
                body_size = 1;
            }
        } else {
            opcode = 1;
            body_size = body_dist(rng);
        }
        const std::uint32_t length = static_cast<std::uint32_t>(body_size + 1);
        char header[4];
        put_u32(header, length);
        conn.out.append(header, sizeof(header));
        conn.out.push_back(static_cast<char>(opcode));
        conn.out.append(body_size, 'x');
        conn.send_times.push_back(now_ns());
        ++conn.inflight;
        conn.next_send_ns = options.rate > 0.0
                                ? now_ns() + static_cast<std::uint64_t>(
                                                  interval_dist(rng) * 1e9)
                                : 0;
    };

    auto flush = [&](CConn& conn) -> bool {
        while (conn.out_off < conn.out.size()) {
            const ssize_t n = ::send(conn.fd, conn.out.data() + conn.out_off,
                                     conn.out.size() - conn.out_off, MSG_NOSIGNAL);
            if (n > 0) {
                conn.out_off += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                conn.want_out = true;
                epoll_event event{};
                event.events = EPOLLIN | EPOLLOUT;
                event.data.u64 = static_cast<std::uint64_t>(
                    &conn - conns.front().get());
                (void)::epoll_ctl(epfd, EPOLL_CTL_MOD, conn.fd, &event);
                return true;
            }
            return false;
        }
        conn.out.clear();
        conn.out_off = 0;
        if (conn.want_out) {
            conn.want_out = false;
            epoll_event event{};
            event.events = EPOLLIN;
            event.data.u64 = static_cast<std::uint64_t>(&conn - conns.front().get());
            (void)::epoll_ctl(epfd, EPOLL_CTL_MOD, conn.fd, &event);
        }
        return true;
    };

    epoll_event events[512];
    while (true) {
        const std::uint64_t now = now_ns();
        if (!measuring && now >= measure_at) {
            measuring = true;
            std::printf("MEASURE_START\n");
            std::fflush(stdout);
        }
        if (now >= end_at) {
            break;
        }
        const int ready = ::epoll_wait(epfd, events, 512, 1);
        if (ready < 0 && errno != EINTR) {
            break;
        }
        for (int i = 0; i < ready; ++i) {
            const std::size_t index = static_cast<std::size_t>(events[i].data.u64);
            if (index >= conns.size()) {
                continue;
            }
            CConn& conn = *conns[index];
            const std::uint32_t mask = events[i].events;
            if (conn.connecting && ((mask & EPOLLOUT) != 0 || (mask & EPOLLERR) != 0)) {
                conn.connecting = false;
                if ((mask & EPOLLERR) != 0) {
                    ++errors;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, conn.fd, nullptr);
                    ::close(conn.fd);
                    conn.fd = -1;
                    continue;
                }
                conn.next_send_ns = now;
                epoll_event event{};
                event.events = EPOLLIN;
                event.data.u64 = index;
                (void)::epoll_ctl(epfd, EPOLL_CTL_MOD, conn.fd, &event);
            }
            if (conn.fd < 0) {
                continue;
            }
            if ((mask & EPOLLOUT) != 0) {
                if (!flush(conn)) {
                    ++errors;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, conn.fd, nullptr);
                    ::close(conn.fd);
                    conn.fd = -1;
                    continue;
                }
            }
            if ((mask & EPOLLIN) != 0) {
                char buffer[64 * 1024];
                for (;;) {
                    const ssize_t n = ::recv(conn.fd, buffer, sizeof(buffer), 0);
                    if (n > 0) {
                        conn.in.append(buffer, static_cast<std::size_t>(n));
                        continue;
                    }
                    if (n == 0) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, conn.fd, nullptr);
                        ::close(conn.fd);
                        conn.fd = -1;
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    ++errors;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, conn.fd, nullptr);
                    ::close(conn.fd);
                    conn.fd = -1;
                    break;
                }
            }
            if (conn.fd < 0) {
                continue;
            }
            // 解析响应。
            for (;;) {
                const std::size_t available = conn.in.size() - conn.in_off;
                if (available < 4) {
                    break;
                }
                const std::uint32_t length = get_u32(conn.in.data() + conn.in_off);
                if (length > kMaxFrame) {
                    ++errors;
                    break;
                }
                if (available < static_cast<std::size_t>(length) + 4) {
                    break;
                }
                conn.in_off += static_cast<std::size_t>(length) + 4;
                bytes += length;
                if (conn.inflight > 0) {
                    --conn.inflight;
                    if (measuring) {
                        ++completed;
                        if (!conn.send_times.empty() &&
                            latencies.size() < (1U << 22)) {
                            latencies.push_back(now - conn.send_times.front());
                        }
                    }
                    if (!conn.send_times.empty()) {
                        conn.send_times.pop_front();
                    }
                }
            }
            if (conn.in_off == conn.in.size()) {
                conn.in.clear();
                conn.in_off = 0;
            } else if (conn.in_off > (1U << 20)) {
                conn.in.erase(0, conn.in_off);
                conn.in_off = 0;
            }
        }
        // 发送：不限速时响应驱动补发；限速时按指数间隔。
        for (auto& conn : conns) {
            if (conn->fd < 0 || conn->connecting) {
                continue;
            }
            while (conn->inflight < options.depth &&
                   (options.rate <= 0.0 || conn->next_send_ns <= now)) {
                make_request(*conn);
                if (!flush(*conn)) {
                    ++errors;
                    break;
                }
            }
        }
    }

    for (auto& conn : conns) {
        if (conn->fd >= 0) {
            ::close(conn->fd);
        }
    }
    ::close(epfd);

    std::sort(latencies.begin(), latencies.end());
    auto percentile = [&](double p) -> std::uint64_t {
        if (latencies.empty()) {
            return 0;
        }
        const std::size_t index = static_cast<std::size_t>(
            std::min<double>(latencies.size() - 1, p * latencies.size()));
        return latencies[index];
    };
    const double elapsed_s =
        static_cast<double>(options.duration_ms) / 1000.0;
    const double qps = static_cast<double>(completed) / elapsed_s;
    std::printf(
        "RESULT {\"qps\":%.0f,\"completed\":%llu,\"errors\":%llu,"
        "\"p50_ns\":%llu,\"p90_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,"
        "\"bytes\":%llu}\n",
        qps, static_cast<unsigned long long>(completed),
        static_cast<unsigned long long>(errors),
        static_cast<unsigned long long>(percentile(0.50)),
        static_cast<unsigned long long>(percentile(0.90)),
        static_cast<unsigned long long>(percentile(0.99)),
        static_cast<unsigned long long>(percentile(0.999)),
        static_cast<unsigned long long>(bytes));
    std::fflush(stdout);
    return 0;
}

void usage() {
    std::puts(
        "server --model gmp|epoll [--port 0] [--workers 12]\n"
        "client --port N [--conns 2000] [--depth 1] [--rate 0]\n"
        "       [--body-min 64] [--body-max 4096] [--mixed]\n"
        "       [--warmup-ms 2000] [--duration-ms 10000] [--seed 1]");
}

std::string arg_value(int argc, char** argv, const char* name,
                      const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

bool has_flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "server") {
        g_downstream_us = std::stoi(arg_value(argc, argv, "--downstream-us", "0"));
        const std::string model = arg_value(argc, argv, "--model", "gmp");
        const std::size_t workers = static_cast<std::size_t>(
            std::stoul(arg_value(argc, argv, "--workers", "12")));
        const std::uint16_t port = static_cast<std::uint16_t>(
            std::stoul(arg_value(argc, argv, "--port", "0")));
        return model == "epoll" ? run_epoll_server(port, workers)
                                : run_gmp_server(port, workers);
    }
    if (mode == "client") {
        ClientOptions options;
        options.port = static_cast<std::uint16_t>(
            std::stoul(arg_value(argc, argv, "--port", "0")));
        options.conns = static_cast<std::size_t>(
            std::stoul(arg_value(argc, argv, "--conns", "64")));
        options.depth = static_cast<std::size_t>(
            std::max(1, std::stoi(arg_value(argc, argv, "--depth", "1"))));
        options.rate = std::stod(arg_value(argc, argv, "--rate", "0"));
        options.body_min = static_cast<std::size_t>(
            std::stoul(arg_value(argc, argv, "--body-min", "64")));
        options.body_max = static_cast<std::size_t>(
            std::stoul(arg_value(argc, argv, "--body-max", "4096")));
        options.warmup_ms = std::stoll(arg_value(argc, argv, "--warmup-ms", "2000"));
        options.duration_ms = std::stoll(arg_value(argc, argv, "--duration-ms", "10000"));
        options.seed = std::stoull(arg_value(argc, argv, "--seed", "1"));
        options.mixed = has_flag(argc, argv, "--mixed");
        return run_client(options);
    }
    usage();
    return 2;
}
