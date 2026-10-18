/**
 * @file gmp_vs_thread_epoll_benchmark.cpp
 * @brief Go2Cpp GMP 协程模型与传统 thread/epoll 模型的多场景网络压测。
 *
 * @details
 * 本文件实现**同一套协议、同一套业务处理**的三种服务端模型：
 *
 * - `gmp`   ：1 个 `IOManager` + 每连接一个 Fiber（G），阻塞式 `Socket::recv/send`
 *             在 Fiber 里 park，M 被复用；线程数固定 ≈ processor_count。
 * - `thread`：每连接一个 `pthread`（默认 8MB 栈）+ 阻塞 `read/write`，
 *             即最朴素的每连接一线程模型。
 * - `epoll` ：1 个 epoll IO 线程 + 固定工作线程池（默认 8）+ 每连接状态机，
 *             写路径走用户态发送队列，慢客户端不会占住工作线程。
 *
 * 客户端也实现为单线程 epoll 事件循环，支持五种负载场景，逐请求记录时间戳，
 * 输出 QPS / P50 / P95 / P99 / 吞吐。服务端进程指标（线程数、RSS、上下文切换、
 * CPU 时间）由外部驱动脚本读 `/proc/<pid>` 采样。
 *
 * 协议（简单长度前缀帧，避免 HTTP 解析开销干扰模型对比）：
 *
 *     请求： [u32 length][u8 opcode][body...]
 *     响应： [u32 length][body...]
 *
 * opcode 语义见 `process_request`。所有模型共用 `process_request`，因此差异
 * 只来自并发模型本身。
 *
 * ## 为什么这样设计
 *
 * - **协议极简**：目的是比较「阻塞/调度/线程」的代价，不是比较 HTTP 实现；
 * - **写路径非阻塞**：epoll 模型用发送队列 + EPOLLOUT，保证慢客户端不阻塞
 *   工作线程，这是该模型的标准做法；否则对比会变成「谁有 bug」而不是「谁更快」；
 * - **客户端也 epoll**：单线程即可驱动 1 万连接，不会让客户端成为瓶颈；
 * - **每请求时间戳**：延迟分位数由客户端完整样本计算，不用平均值糊弄。
 *
 * 用法：
 *   gmp_vs_thread_epoll_benchmark server --model gmp|thread|epoll [--port 0]
 *                                       [--workers 8] [--sndbuf 262144]
 *                                       [--stack-kb 8192]
 *   gmp_vs_thread_epoll_benchmark client --scenario s1..s5 --port N
 *                                       [--conns 64] [--duration-ms 5000]
 *                                       [--warmup-ms 1000] [--interval-ms 1000]
 *
 * 服务端监听成功后向 stdout 打印一行
 *   `READY pid=<pid> port=<port>`，
 * 客户端结束后打印一行 `RESULT {json}`。
 *
 * @author DSH agent（deepseek）
 * @date 2026-10-18
 * @license 与项目一致（见根目录 LICENSE）
 */

#include "go2cpp/network/address.hpp"
#include "go2cpp/network/io.hpp"
#include "go2cpp/network/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::Socket;
using go2cpp::net::IPv4Address;

// ===========================================================================
// 全局停止标志与信号处理
// ===========================================================================

std::atomic<bool> g_stop{false};

/**
 * @brief 安装 SIGTERM/SIGINT 处理器。
 * @details 不设置 `SA_RESTART`：驱动脚本用 SIGTERM 结束服务端时，阻塞中的
 *          `accept`/`read` 会被 EINTR 打断，模型 B1 才能退出循环。
 */
void install_stop_handlers() {
    struct sigaction action {};
    action.sa_handler = [](int) { g_stop.store(true); };
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    ::sigaction(SIGTERM, &action, nullptr);
    ::sigaction(SIGINT, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
}

/**
 * @brief 启动一个兜底看门狗：3 秒后仍未正常退出就强制结束。
 * @details 目的是避免压测驱动在收尾阶段被卡住（例如仍有 Fiber 阻塞在
 *          已断开的 fd 上），不是掩盖错误：正常路径会先打印 STOPPED。
 */
void arm_shutdown_watchdog() {
    std::thread([] {
        ::sleep(3);
        std::fputs("[server] 看门狗强制退出\n", stderr);
        ::_exit(0);
    }).detach();
}

// ===========================================================================
// 协议与业务处理（三种模型共用）
// ===========================================================================

constexpr std::uint32_t kMaxFrame = 8U << 20;   ///< 单帧上限 8MB
constexpr std::size_t kReadBody = 4096;         ///< 读场景上行负载
constexpr std::size_t kWriteBody = 4096;        ///< 写场景下行负载
constexpr std::uint32_t kBigBody = 4U << 20;    ///< 慢客户端场景下行 4MB
constexpr std::size_t kBigChunk = 64U << 10;    ///< 大响应分块 64KB
constexpr std::size_t kServerInLimit = 1U << 20;

// opcode：
//   1 ECHO  —— 原样回显 body（长/短连接 echo）
//   2 READ  —— 上行 4KB，下行 1 字节 ack（混合读）
//   3 WRITE —— 上行 1 字节，下行 4KB（混合写）
//   4 BIG   —— 上行 1 字节，下行 4MB（慢客户端/背压）
//   5 PING  —— 上行 1 字节，下行 1 字节（海量空闲连接）
enum Opcode : std::uint8_t {
    kOpEcho = 1,
    kOpRead = 2,
    kOpWrite = 3,
    kOpBig = 4,
    kOpPing = 5,
};

inline void put_u32(char* dst, std::uint32_t value) {
    std::memcpy(dst, &value, sizeof(value));
}

inline std::uint32_t get_u32(const char* src) {
    std::uint32_t value = 0;
    std::memcpy(&value, src, sizeof(value));
    return value;
}

/** @brief 一次业务处理的结果；`big_bytes > 0` 表示流式大响应。 */
struct Response {
    std::uint8_t opcode{0};
    std::string body;
    std::uint32_t big_bytes{0};
};

/**
 * @brief 纯 CPU 业务处理，三种模型完全一致。
 * @details 刻意保持“便宜”，让测量聚焦于并发/IO 模型成本；ECHO 会复制
 *          请求体，READ 回 1 字节，WRITE 回 4KB，BIG 只标记大小由发送层
 *          分块写出（避免服务端额外 4MB 分配影响内存对比）。
 */
/// @brief 模拟真实 IO 延迟的上限（微秒）。0 表示关闭。
/// @details 用 usleep 而不是忙等：在 GMP 模型里 usleep 被 hook 成 park（让出 M），
///          在 thread 模型里是真正的线程睡眠 —— 这正是要对比的差异。
constexpr std::uint32_t kIoSleepDefaultUs = 5000;

/// 实际使用的 IO 延迟上限，可用 GO2CPP_BENCH_IO_SLEEP_US 覆盖（诊断用）。
/// 注意：压测结果必须报告这个值；默认 5000 与文档基线一致。
std::uint32_t io_sleep_max_us() {
    static const std::uint32_t value = [] {
        const char* env = std::getenv("GO2CPP_BENCH_IO_SLEEP_US");
        if (env == nullptr || *env == 0) {
            return kIoSleepDefaultUs;
        }
        return static_cast<std::uint32_t>(std::strtoul(env, nullptr, 10));
    }();
    return value;
}

Response process_request(std::uint8_t opcode, std::string_view body) {
    const std::uint32_t kIoSleepMaxUs = io_sleep_max_us();
    if (kIoSleepMaxUs > 0) {
        // xorshift32：每线程独立，避免锁；延迟在 [0, kIoSleepMaxUs] 均匀分布
        static thread_local std::uint32_t seed = 0x9e3779b9u;
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        const std::uint32_t delay_us = seed % (kIoSleepMaxUs + 1u);
        if (delay_us > 0) {
            ::usleep(delay_us);
        }
    }
    switch (opcode) {
        case kOpEcho:
            return {kOpEcho, std::string(body), 0};
        case kOpRead:
            return {kOpRead, std::string(1, 'A'), 0};
        case kOpWrite:
            return {kOpWrite, std::string(kWriteBody, 'W'), 0};
        case kOpPing:
            return {kOpPing, std::string(1, 'P'), 0};
        case kOpBig:
            return {kOpBig, std::string(), kBigBody};
        default:
            return {0, std::string(), 0};
    }
}

/** @brief 大响应用的 64KB 填充块（静态，避免每次分配）。 */
const std::string& big_pattern() {
    static const std::string pattern(kBigChunk, 'B');
    return pattern;
}

// ===========================================================================
// 原始 socket 工具（模型 B1/B2、客户端使用）
// ===========================================================================

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void tune_accepted(int fd, int sndbuf) {
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (sndbuf > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    }
}

/** @brief 创建监听 socket（非阻塞 + SO_REUSEADDR），port=0 时由内核分配。 */
int make_listener(std::uint16_t port, int backlog, std::uint16_t* actual_port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, backlog) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &len) == 0 &&
        actual_port != nullptr) {
        *actual_port = ntohs(address.sin_port);
    }
    // 监听 fd 用阻塞 accept（由模型自己决定是否 epoll 注册），因此不设非阻塞。
    return fd;
}

/** @brief 阻塞读满 len 字节；EOF/错误返回 false。 */
bool recv_exact_fd(int fd, void* dst, std::size_t len) {
    char* bytes = static_cast<char*>(dst);
    std::size_t got = 0;
    while (got < len) {
        const ssize_t n = ::recv(fd, bytes + got, len - got, 0);
        if (n > 0) {
            got += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool send_all_fd(int fd, const void* src, std::size_t len) {
    const char* bytes = static_cast<const char*>(src);
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, bytes + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

/** @brief Go2Cpp Socket 版本：受管 Fiber 内会自动 park 而非阻塞 M。 */
bool recv_exact_socket(Socket& socket, void* dst, std::size_t len) {
    char* bytes = static_cast<char*>(dst);
    std::size_t got = 0;
    while (got < len) {
        const auto result = socket.recv(bytes + got, len - got);
        if (result.transferred > 0) {
            got += static_cast<std::size_t>(result.transferred);
            continue;
        }
        if (result.transferred == 0 && result.error == 0) {
            return false;  // 对端关闭
        }
        if (result.error == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool send_all_socket(Socket& socket, const void* src, std::size_t len) {
    const char* bytes = static_cast<const char*>(src);
    std::size_t sent = 0;
    while (sent < len) {
        const auto result = socket.send(bytes + sent, len - sent);
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

/** @brief 写出一个完整响应（小响应一次性写，大响应按 64KB 分块）。 */
bool write_response_fd(int fd, const Response& response) {
    char header[4];
    if (response.big_bytes > 0) {
        put_u32(header, response.big_bytes);
        if (!send_all_fd(fd, header, 4)) {
            return false;
        }
        const std::string& chunk = big_pattern();
        for (std::uint32_t left = response.big_bytes; left > 0;) {
            const std::size_t n = std::min<std::size_t>(left, chunk.size());
            if (!send_all_fd(fd, chunk.data(), n)) {
                return false;
            }
            left -= static_cast<std::uint32_t>(n);
        }
        return true;
    }
    put_u32(header, static_cast<std::uint32_t>(response.body.size()));
    return send_all_fd(fd, header, 4) &&
           send_all_fd(fd, response.body.data(), response.body.size());
}

/** @brief 阻塞模型的读-处理-写循环（B1 的每个线程、GMP 的每个 Fiber 共用）。 */
template <typename RecvFn, typename SendFn>
void blocking_serve_loop(RecvFn&& recv_exact, SendFn&& send_response) {
    char header[4];
    std::string body;
    for (;;) {
        if (!recv_exact(header, sizeof(header))) {
            return;
        }
        const std::uint32_t length = get_u32(header);
        if (length == 0 || length > kMaxFrame) {
            return;
        }
        body.resize(length);
        if (!recv_exact(body.data(), body.size())) {
            return;
        }
        const Response response =
            process_request(static_cast<std::uint8_t>(body[0]), std::string_view(body).substr(1));
        if (response.opcode == 0) {
            return;
        }
        if (!send_response(response)) {
            return;
        }
    }
}

// ===========================================================================
// 模型 A：Go2Cpp GMP —— IOManager + 每连接一个 Fiber
// ===========================================================================

/**
 * @brief 使用 hook 拦截的原生 recv/send 服务一个连接。
 * @details 与 `Socket::recv` 的差别只是绕开 Socket 包装层（mutex、关闭状态机），
 *          仍然是「在 Fiber 里同步阻塞」的写法；用于量化包装层成本。
 */
void serve_gmp_connection_hook(Socket::Ptr socket) {
    const int fd = socket->fd();
    blocking_serve_loop(
        [&](void* dst, std::size_t len) { return recv_exact_fd(fd, dst, len); },
        [&](const Response& response) { return write_response_fd(fd, response); });
    socket->close();
}

/**
 * @brief 服务一个连接：阻塞式 recv/send 在 Fiber 内 park，M 去跑别的 G。
 * @details 这是项目推荐的“同步写法、异步执行”形态：代码里看不到回调或
 *          状态机，但 1 万连接只占 1 万份 KB 级栈 + 固定数量的 M。
 */
void serve_gmp_connection(Socket::Ptr socket) {
    blocking_serve_loop(
        [&](void* dst, std::size_t len) { return recv_exact_socket(*socket, dst, len); },
        [&](const Response& response) {
            char header[4];
            if (response.big_bytes > 0) {
                put_u32(header, response.big_bytes);
                if (!send_all_socket(*socket, header, 4)) {
                    return false;
                }
                const std::string& chunk = big_pattern();
                for (std::uint32_t left = response.big_bytes; left > 0;) {
                    const std::size_t n = std::min<std::size_t>(left, chunk.size());
                    if (!send_all_socket(*socket, chunk.data(), n)) {
                        return false;
                    }
                    left -= static_cast<std::uint32_t>(n);
                }
                return true;
            }
            put_u32(header, static_cast<std::uint32_t>(response.body.size()));
            return send_all_socket(*socket, header, 4) &&
                   send_all_socket(*socket, response.body.data(), response.body.size());
        });
    socket->close();
}

int run_gmp_server(std::uint16_t port, int sndbuf, bool raw_io, bool pin) {
    go2cpp::SchedulerConfig config;
    config.processor_count = 0;      // 0 = 按硬件线程数（本机 12）
    config.collect_metrics = false;  // 关闭诊断计时，压测时不付额外成本
    config.pin_workers_to_cpu = pin;
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
    std::uint16_t actual_port = 0;
    if (auto address = std::dynamic_pointer_cast<IPv4Address>(listener->local_address())) {
        actual_port = address->port();
    }
    std::printf("READY pid=%d port=%u\n", static_cast<int>(::getpid()),
                static_cast<unsigned>(actual_port));
    std::fflush(stdout);

    while (!g_stop.load(std::memory_order_relaxed)) {
        auto socket = listener->accept(std::chrono::milliseconds(200));
        if (!socket) {
            continue;
        }
        tune_accepted(socket->fd(), sndbuf);
        if (raw_io) {
            manager.go([socket] { serve_gmp_connection_hook(socket); });
        } else {
            manager.go([socket] { serve_gmp_connection(socket); });
        }
    }
    listener->close();
    arm_shutdown_watchdog();
    manager.shutdown();
    std::fputs("STOPPED\n", stderr);
    return 0;
}

// ===========================================================================
// 模型 B1：每连接一个线程 + 阻塞 IO
// ===========================================================================

std::atomic<std::uint64_t> g_thread_create_failures{0};
std::atomic<std::uint64_t> g_thread_active{0};

int run_thread_server(std::uint16_t port, int sndbuf, std::size_t stack_kb) {
    std::uint16_t actual_port = 0;
    const int listener = make_listener(port, 8192, &actual_port);
    if (listener < 0) {
        std::fputs("[server] 监听失败\n", stderr);
        return 1;
    }
    std::printf("READY pid=%d port=%u\n", static_cast<int>(::getpid()),
                static_cast<unsigned>(actual_port));
    std::fflush(stdout);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_kb > 0) {
        // 默认 (size_t)-1 由 glibc 决定（通常 8MB）；这里只在显式指定时覆盖。
        pthread_attr_setstacksize(&attr, stack_kb * 1024);
    } else {
        pthread_attr_setstacksize(&attr, 8U * 1024U * 1024U);
    }

    while (!g_stop.load(std::memory_order_relaxed)) {
        sockaddr_in peer {};
        socklen_t peer_len = sizeof(peer);
        const int fd = ::accept4(listener, reinterpret_cast<sockaddr*>(&peer), &peer_len,
                                 SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            continue;
        }
        tune_accepted(fd, sndbuf);
        struct Context {
            int fd;
        };
        auto* context = new Context{fd};
        pthread_t thread;
        const int rc = pthread_create(
            &thread, &attr,
            [](void* arg) -> void* {
                std::unique_ptr<Context> context(static_cast<Context*>(arg));
                g_thread_active.fetch_add(1, std::memory_order_relaxed);
                blocking_serve_loop(
                    [&](void* dst, std::size_t len) {
                        return recv_exact_fd(context->fd, dst, len);
                    },
                    [&](const Response& response) {
                        return write_response_fd(context->fd, response);
                    });
                ::close(context->fd);
                g_thread_active.fetch_sub(1, std::memory_order_relaxed);
                return nullptr;
            },
            context);
        if (rc != 0) {
            // 线程创建失败（默认 8MB 栈时很常见）：关闭连接并计数，客户端会
            // 看到连接被重置；这是“每连接一线程”模型的真实容量边界。
            g_thread_create_failures.fetch_add(1, std::memory_order_relaxed);
            ::close(fd);
            delete context;
            continue;
        }
        pthread_detach(thread);
    }
    pthread_attr_destroy(&attr);
    ::close(listener);
    std::fprintf(stderr, "STOPPED thread_create_failures=%llu\n",
                 static_cast<unsigned long long>(
                     g_thread_create_failures.load(std::memory_order_relaxed)));
    return 0;
}

// ===========================================================================
// 模型 B2：1 个 epoll IO 线程 + 固定工作线程池
// ===========================================================================

namespace b2 {

constexpr std::size_t kOutHighWater = 8U << 20;  ///< 输出堆积阈值（背压）
constexpr std::size_t kOutLowWater = 1U << 20;   ///< 恢复读阈值

/** @brief 每连接状态：输入解析缓冲 + 用户态发送队列。 */
struct Conn {
    int fd{-1};
    std::string in;
    std::size_t in_off{0};
    std::string out;
    std::size_t out_off{0};
    bool paused{false};      ///< 输出堆积时暂停 EPOLLIN
    bool want_out{false};    ///< 已注册 EPOLLOUT
    bool out_queued{false};  ///< 已在待刷队列中，避免重复入队
};

struct Job {
    std::uint64_t id{0};
    std::string frame;
};

struct Completion {
    std::uint64_t id{0};
    std::uint8_t opcode{0};
    std::string body;
    std::uint32_t big_bytes{0};
};

class EpollPoolServer final {
public:
    EpollPoolServer(std::uint16_t port, std::size_t workers, int sndbuf)
        : m_workers(workers), m_sndbuf(sndbuf), m_port(port) {}

    int run() {
        std::uint16_t actual_port = 0;
        m_listener = make_listener(m_port, 8192, &actual_port);
        if (m_listener < 0) {
            std::fputs("[server] 监听失败\n", stderr);
            return 1;
        }
        set_nonblocking(m_listener);
        m_epoll = ::epoll_create1(EPOLL_CLOEXEC);
        m_wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (m_epoll < 0 || m_wake < 0) {
            std::fputs("[server] epoll/eventfd 创建失败\n", stderr);
            return 1;
        }
        add_interest(m_listener, 0, EPOLLIN);
        add_interest(m_wake, kSpecialWake, EPOLLIN);

        for (std::size_t i = 0; i < m_workers; ++i) {
            m_pool.emplace_back([this] { worker_loop(); });
        }

        std::printf("READY pid=%d port=%u\n", static_cast<int>(::getpid()),
                    static_cast<unsigned>(actual_port));
        std::fflush(stdout);

        epoll_event events[512];
        std::vector<std::uint64_t> pending;
        while (!g_stop.load(std::memory_order_relaxed)) {
            drain_completions();
            flush_pending();
            const int ready = ::epoll_wait(m_epoll, events, 512, 1);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            for (int i = 0; i < ready; ++i) {
                const std::uint64_t id = events[i].data.u64;
                const std::uint32_t mask = events[i].events;
                if (id == 0) {
                    accept_all();
                } else if (id == kSpecialWake) {
                    std::uint64_t value = 0;
                    const ssize_t drained = ::read(m_wake, &value, sizeof(value));
                    (void)drained;
                } else {
                    handle_event(id, mask);
                    if (g_stop.load(std::memory_order_relaxed)) {
                        break;
                    }
                }
            }
        }

        // 收尾：先停工作线程，再关闭连接与 epoll。
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        for (auto& worker : m_pool) {
            worker.join();
        }
        for (auto& entry : m_conns) {
            ::epoll_ctl(m_epoll, EPOLL_CTL_DEL, entry.second->fd, nullptr);
            ::close(entry.second->fd);
        }
        m_conns.clear();
        ::close(m_wake);
        ::close(m_epoll);
        ::close(m_listener);
        std::fputs("STOPPED\n", stderr);
        return 0;
    }

private:
    static constexpr std::uint64_t kSpecialWake = 1;  ///< 0 = 监听 fd

    void add_interest(int fd, std::uint64_t id, std::uint32_t events) {
        epoll_event event {};
        event.events = events;
        event.data.u64 = id;
        ::epoll_ctl(m_epoll, EPOLL_CTL_ADD, fd, &event);
    }

    void update_interest(Conn& conn, std::uint64_t id) {
        std::uint32_t events = 0;
        if (!conn.paused) {
            events |= EPOLLIN;
        }
        if (conn.want_out) {
            events |= EPOLLOUT;
        }
        epoll_event event {};
        event.events = events;
        event.data.u64 = id;
        ::epoll_ctl(m_epoll, EPOLL_CTL_MOD, conn.fd, &event);
    }

    void accept_all() {
        for (;;) {
            sockaddr_in peer {};
            socklen_t len = sizeof(peer);
            const int fd = ::accept4(m_listener, reinterpret_cast<sockaddr*>(&peer), &len,
                                     SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            tune_accepted(fd, m_sndbuf);
            const std::uint64_t id = m_next_id++;
            auto conn = std::make_unique<Conn>();
            conn->fd = fd;
            m_conns.emplace(id, std::move(conn));
            add_interest(fd, id, EPOLLIN);
        }
    }

    void handle_event(std::uint64_t id, std::uint32_t mask) {
        auto it = m_conns.find(id);
        if (it == m_conns.end()) {
            return;
        }
        if ((mask & EPOLLERR) != 0 || (mask & EPOLLHUP) != 0) {
            close_conn(id);
            return;
        }
        if ((mask & EPOLLOUT) != 0) {
            try_flush(id);
            if (m_conns.find(id) == m_conns.end()) {
                return;
            }
        }
        if ((mask & EPOLLIN) != 0) {
            on_readable(id);
        }
    }

    void on_readable(std::uint64_t id) {
        auto it = m_conns.find(id);
        if (it == m_conns.end()) {
            return;
        }
        Conn& conn = *it->second;
        char buffer[64 * 1024];
        for (;;) {
            const ssize_t n = ::recv(conn.fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                conn.in.append(buffer, static_cast<std::size_t>(n));
                if (conn.in.size() - conn.in_off > kServerInLimit) {
                    break;
                }
                continue;
            }
            if (n == 0) {
                close_conn(id);
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            close_conn(id);
            return;
        }
        parse_frames(id);
        auto after = m_conns.find(id);
        if (after == m_conns.end()) {
            return;
        }
        Conn& current = *after->second;
        if (current.out.size() - current.out_off > kOutHighWater && !current.paused) {
            current.paused = true;
            update_interest(current, id);
        }
    }

    void parse_frames(std::uint64_t id) {
        auto it = m_conns.find(id);
        if (it == m_conns.end()) {
            return;
        }
        Conn& conn = *it->second;
        for (;;) {
            const std::size_t available = conn.in.size() - conn.in_off;
            if (available < 4) {
                break;
            }
            const std::uint32_t length = get_u32(conn.in.data() + conn.in_off);
            if (length == 0 || length > kMaxFrame) {
                close_conn(id);
                return;
            }
            if (available < static_cast<std::size_t>(length) + 4) {
                break;
            }
            Job job;
            job.id = id;
            job.frame.assign(conn.in, conn.in_off + 4, length);
            conn.in_off += static_cast<std::size_t>(length) + 4;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_jobs.push_back(std::move(job));
            }
            m_cv.notify_one();
        }
        if (conn.in_off == conn.in.size()) {
            conn.in.clear();
            conn.in_off = 0;
        } else if (conn.in_off > (1U << 20)) {
            conn.in.erase(0, conn.in_off);
            conn.in_off = 0;
        }
    }

    void worker_loop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
                if (m_jobs.empty()) {
                    return;
                }
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            const std::uint8_t opcode = static_cast<std::uint8_t>(job.frame[0]);
            const Response response =
                process_request(opcode, std::string_view(job.frame).substr(1));
            if (response.opcode == 0) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_completions.push_back(
                    Completion{job.id, response.opcode, std::move(response.body),
                               response.big_bytes});
            }
            const std::uint64_t one = 1;
            const ssize_t woke = ::write(m_wake, &one, sizeof(one));
            (void)woke;
        }
    }

    void drain_completions() {
        std::vector<Completion> completed;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            completed.swap(m_completions);
        }
        for (auto& completion : completed) {
            auto it = m_conns.find(completion.id);
            if (it == m_conns.end()) {
                continue;  // 连接已关闭，丢弃
            }
            Conn& conn = *it->second;
            char header[4];
            if (completion.big_bytes > 0) {
                put_u32(header, completion.big_bytes);
                conn.out.append(header, 4);
                const std::string& chunk = big_pattern();
                for (std::uint32_t left = completion.big_bytes; left > 0;) {
                    const std::size_t n = std::min<std::size_t>(left, chunk.size());
                    conn.out.append(chunk.data(), n);
                    left -= static_cast<std::uint32_t>(n);
                }
            } else {
                put_u32(header, static_cast<std::uint32_t>(completion.body.size()));
                conn.out.append(header, 4);
                conn.out.append(completion.body);
            }
            if (!conn.out_queued) {
                conn.out_queued = true;
                m_out_pending.push_back(completion.id);
            }
        }
    }

    void flush_pending() {
        while (!m_out_pending.empty()) {
            const std::uint64_t id = m_out_pending.front();
            m_out_pending.pop_front();
            auto it = m_conns.find(id);
            if (it == m_conns.end()) {
                continue;
            }
            it->second->out_queued = false;
            try_flush(id);
        }
    }

    void try_flush(std::uint64_t id) {
        auto it = m_conns.find(id);
        if (it == m_conns.end()) {
            return;
        }
        Conn& conn = *it->second;
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
                    update_interest(conn, id);
                }
                return;
            }
            close_conn(id);
            return;
        }
        conn.out.clear();
        conn.out_off = 0;
        if (conn.want_out) {
            conn.want_out = false;
        }
        if (conn.paused && conn.out.size() < kOutLowWater) {
            conn.paused = false;
        }
        update_interest(conn, id);
    }

    void close_conn(std::uint64_t id) {
        auto it = m_conns.find(id);
        if (it == m_conns.end()) {
            return;
        }
        ::epoll_ctl(m_epoll, EPOLL_CTL_DEL, it->second->fd, nullptr);
        ::close(it->second->fd);
        m_conns.erase(it);
    }

    std::size_t m_workers;
    int m_sndbuf;
    std::uint16_t m_port;
    int m_listener{-1};
    int m_epoll{-1};
    int m_wake{-1};
    std::uint64_t m_next_id{2};  // 0 = 监听 fd，1 = eventfd，连接 id 从 2 开始

    std::unordered_map<std::uint64_t, std::unique_ptr<Conn>> m_conns;
    std::deque<std::uint64_t> m_out_pending;
    std::vector<std::thread> m_pool;
    std::deque<Job> m_jobs;
    std::vector<Completion> m_completions;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stopping{false};
};

}  // namespace b2

// ===========================================================================
// 客户端：单线程 epoll 事件循环 + 五种场景
// ===========================================================================

enum class Scenario {
    kShort,   ///< s1 短连接：连接→1 请求→关闭→重连
    kEcho,    ///< s2 长连接 echo：连接存活，连续 ping-pong
    kIdle,    ///< s3 海量空闲连接：1 万连接，每秒 1 次 PING
    kMixed,   ///< s4 混合读写：READ(4KB 上/1B 下) 与 WRITE(1B 上/4KB 下) 各半
    kSlow,    ///< s5 慢客户端：1 个慢读大响应连接 + N 个正常 echo 连接
};

const char* scenario_name(Scenario scenario) {
    switch (scenario) {
        case Scenario::kShort:
            return "s1-short";
        case Scenario::kEcho:
            return "s2-echo";
        case Scenario::kIdle:
            return "s3-idle";
        case Scenario::kMixed:
            return "s4-mixed";
        case Scenario::kSlow:
            return "s5-slow";
    }
    return "?";
}

struct ClientOptions {
    Scenario scenario{Scenario::kEcho};
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    std::size_t conns{64};
    std::int64_t duration_ms{30000};
    std::int64_t warmup_ms{5000};
    std::int64_t interval_ms{1000};
    std::size_t slow_conns{1};
    /// @brief s2 每连接同时在途请求数；>1 时事件会在服务端批量到达。
    std::size_t depth{1};
};

struct CConn {
    int fd{-1};
    int state{0};  ///< 0 空闲 / 1 connecting / 2 established
    std::string in;
    std::size_t in_off{0};
    std::string out;
    std::size_t out_off{0};
    bool want_write{false};
    bool read_paused{false};
    bool in_epoll{false};
    std::uint64_t generation{0};
    std::uint64_t t_send{0};
    std::uint64_t t_connect{0};
    std::uint64_t next_read_at{0};
    bool slow{false};
    std::uint8_t pending_op{0};
    // 流水线模式（--depth N）下同一条连接可能有 N 个请求在途，响应按 TCP
    // 顺序返回，因此用 FIFO 保存每个请求的发送时间。
    std::deque<std::uint64_t> send_times;
};

struct Timer {
    std::uint64_t at{0};
    std::size_t index{0};
    std::uint64_t generation{0};
    int kind{0};  ///< 1 新建连接 / 2 空闲连接下次 PING / 3 慢客户端恢复读 / 4 错峰连接
};

struct TimerGreater {
    bool operator()(const Timer& lhs, const Timer& rhs) const { return lhs.at > rhs.at; }
};

class LoadClient final {
public:
    explicit LoadClient(ClientOptions options) : m_options(std::move(options)) {}

    int run() {
        m_epoll = ::epoll_create1(EPOLL_CLOEXEC);
        if (m_epoll < 0) {
            std::fputs("[client] epoll_create1 失败\n", stderr);
            return 1;
        }
        const std::size_t total_slots =
            m_options.scenario == Scenario::kSlow ? m_options.conns + m_options.slow_conns
                                                  : m_options.conns;
        m_conns.resize(total_slots);
        for (std::size_t i = 0; i < total_slots; ++i) {
            m_conns[i].slow = (m_options.scenario == Scenario::kSlow && i >= m_options.conns);
        }

        const std::uint64_t start = now_ns();
        m_warmup_end = start + static_cast<std::uint64_t>(m_options.warmup_ms) * 1000000ULL;
        m_run_end = m_warmup_end + static_cast<std::uint64_t>(m_options.duration_ms) * 1000000ULL;

        schedule_initial_connections();

        epoll_event events[4096];
        while (now_ns() < m_run_end) {
            process_timers();
            if (now_ns() >= m_run_end) {
                break;
            }
            const int timeout_ms = compute_timeout_ms();
            const int ready = ::epoll_wait(m_epoll, events, 4096, timeout_ms);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            for (int i = 0; i < ready; ++i) {
                const std::uint64_t id = events[i].data.u64;
                const std::uint32_t mask = events[i].events;
                const std::size_t index = static_cast<std::size_t>(id);
                if (index >= m_conns.size() || m_conns[index].fd < 0) {
                    continue;
                }
                handle_event(index, mask);
            }
            if (m_warmed_up == false && now_ns() >= m_warmup_end) {
                begin_measurement();
            }
        }
        close_all();
        report();
        ::close(m_epoll);
        return 0;
    }

private:
    static std::uint64_t now_ns() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now().time_since_epoch())
                .count());
    }

    sockaddr_in server_address() const {
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(m_options.port);
        ::inet_pton(AF_INET, m_options.host.c_str(), &address.sin_addr);
        return address;
    }

    void schedule_initial_connections() {
        const std::uint64_t now = now_ns();
        const std::size_t total = m_conns.size();
        for (std::size_t i = 0; i < total; ++i) {
            if (m_conns[i].slow) {
                // 慢客户端先连上，避免它的大响应与正常连接建立竞争。
                m_timers.push(Timer{now, i, m_conns[i].generation, 4});
            } else if (m_options.scenario == Scenario::kIdle) {
                // 1 万连接错峰建立，避免瞬时 SYN 洪水导致 listen backlog 溢出。
                const std::uint64_t stagger =
                    static_cast<std::uint64_t>(i) * 150000ULL;  // 10k × 150µs ≈ 1.5s
                m_timers.push(Timer{now + stagger, i, m_conns[i].generation, 4});
            } else {
                m_timers.push(Timer{now, i, m_conns[i].generation, 4});
            }
        }
    }

    int compute_timeout_ms() const {
        const std::uint64_t now = now_ns();
        int timeout = 5;
        const std::uint64_t run_end = m_run_end;
        if (run_end > now) {
            timeout = static_cast<int>(std::min<std::uint64_t>((run_end - now) / 1000000ULL + 1, 5));
        }
        if (!m_timers.empty()) {
            const std::uint64_t at = m_timers.top().at;
            if (at <= now) {
                return 0;
            }
            const int timer_ms = static_cast<int>((at - now) / 1000000ULL + 1);
            timeout = std::min(timeout, timer_ms);
        }
        return std::max(timeout, 0);
    }

    void process_timers() {
        const std::uint64_t now = now_ns();
        while (!m_timers.empty() && m_timers.top().at <= now) {
            const Timer timer = m_timers.top();
            m_timers.pop();
            if (timer.index >= m_conns.size() ||
                m_conns[timer.index].generation != timer.generation) {
                continue;
            }
            dispatch_timer(timer);
        }
    }

    void dispatch_timer(const Timer& timer) {
        switch (timer.kind) {
            case 1:
            case 4:
                start_connect(timer.index);
                break;
            case 2:
                if (m_conns[timer.index].state == 2) {
                    queue_request(timer.index, kOpPing, std::string());
                }
                break;
            case 3:
                resume_read(timer.index);
                break;
            default:
                break;
        }
    }

    void start_connect(std::size_t index) {
        CConn& conn = m_conns[index];
        conn.generation += 1;  // 该槽位的旧定时器立即失效
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            ++m_errors;
            schedule_reconnect(index);
            return;
        }
        {
            // 客户端也关 Nagle：请求帧很小（1–4KB），不关会让“小帧 + 延迟 ACK”
            // 的交互把 RTT 抬到 20ms 量级，污染混合读写场景。
            const int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
        const sockaddr_in address = server_address();
        const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (rc != 0 && errno != EINPROGRESS) {
            ::close(fd);
            ++m_errors;
            schedule_reconnect(index);
            return;
        }
        conn.fd = fd;
        conn.state = 1;
        conn.in.clear();
        conn.in_off = 0;
        conn.out.clear();
        conn.out_off = 0;
        conn.send_times.clear();
        conn.want_write = false;
        conn.read_paused = false;
        conn.t_send = now_ns();
        conn.t_connect = conn.t_send;
        conn.pending_op = kOpEcho;
        if (!conn.in_epoll) {
            epoll_event event {};
            event.events = EPOLLIN | EPOLLOUT;
            event.data.u64 = index;
            ::epoll_ctl(m_epoll, EPOLL_CTL_ADD, fd, &event);
            conn.in_epoll = true;
        } else {
            epoll_event event {};
            event.events = EPOLLIN | EPOLLOUT;
            event.data.u64 = index;
            ::epoll_ctl(m_epoll, EPOLL_CTL_MOD, fd, &event);
        }
    }

    void schedule_reconnect(std::size_t index) {
        m_conns[index].generation += 1;
        m_timers.push(Timer{now_ns(), index, m_conns[index].generation, 1});
    }

    void close_conn(std::size_t index) {
        CConn& conn = m_conns[index];
        if (conn.fd >= 0) {
            if (m_options.scenario == Scenario::kShort) {
                // 短连接场景会建立数万条连接；用 RST 关闭避免 TIME_WAIT
                // 耗尽客户端临时端口（压测客户端的标准做法）。
                linger option {};
                option.l_onoff = 1;
                option.l_linger = 0;
                ::setsockopt(conn.fd, SOL_SOCKET, SO_LINGER, &option, sizeof(option));
            }
            if (conn.in_epoll) {
                ::epoll_ctl(m_epoll, EPOLL_CTL_DEL, conn.fd, nullptr);
                conn.in_epoll = false;
            }
            ::close(conn.fd);
        }
        conn.fd = -1;
        conn.state = 0;
        conn.in.clear();
        conn.in_off = 0;
        conn.out.clear();
        conn.out_off = 0;
        conn.send_times.clear();
        conn.want_write = false;
        conn.read_paused = false;
        conn.generation += 1;
    }

    void handle_event(std::size_t index, std::uint32_t mask) {
        CConn& conn = m_conns[index];
        if (conn.state == 1) {
            const int error = [&] {
                int value = 0;
                socklen_t len = sizeof(value);
                ::getsockopt(conn.fd, SOL_SOCKET, SO_ERROR, &value, &len);
                return value;
            }();
            if (error != 0) {
                ++m_errors;
                close_conn(index);
                schedule_reconnect(index);
                return;
            }
            conn.state = 2;
            ++m_established;
            if (m_measuring) {
                ++m_established_in_window;
            }
            on_connected(index);
            return;
        }
        if ((mask & (EPOLLERR | EPOLLHUP)) != 0) {
            ++m_errors;
            if (m_options.scenario == Scenario::kShort) {
                close_conn(index);
                schedule_reconnect(index);
            } else {
                close_conn(index);
            }
            return;
        }
        if ((mask & EPOLLOUT) != 0) {
            flush_output(index);
            if (m_conns[index].fd < 0) {
                return;
            }
        }
        if ((mask & EPOLLIN) != 0) {
            on_readable(index);
        }
    }

    void on_connected(std::size_t index) {
        CConn& conn = m_conns[index];
        switch (m_options.scenario) {
            case Scenario::kShort: {
                conn.t_send = now_ns();
                queue_request(index, kOpEcho, std::string(64, 'e'));
                break;
            }
            case Scenario::kEcho: {
                for (std::size_t i = 0; i < std::max<std::size_t>(m_options.depth, 1); ++i) {
                    queue_request(index, kOpEcho, std::string(64, 'e'));
                }
                break;
            }
            case Scenario::kIdle: {
                const std::uint64_t now = now_ns();
                const std::uint64_t stagger =
                    static_cast<std::uint64_t>(index % 1000) *
                    static_cast<std::uint64_t>(m_options.interval_ms) * 1000000ULL / 1000ULL;
                m_timers.push(
                    Timer{now + stagger, index, conn.generation, 2});
                break;
            }
            case Scenario::kMixed: {
                queue_request(index, kOpRead, std::string(kReadBody, 'r'));
                break;
            }
            case Scenario::kSlow: {
                if (conn.slow) {
                    queue_request(index, kOpBig, std::string(1, 'b'));
                } else {
                    queue_request(index, kOpEcho, std::string(64, 'e'));
                }
                break;
            }
        }
    }

    void queue_request(std::size_t index, std::uint8_t opcode, const std::string& body) {
        CConn& conn = m_conns[index];
        const std::size_t payload = body.size() + 1;
        std::string frame;
        frame.resize(4 + payload);
        put_u32(frame.data(), static_cast<std::uint32_t>(payload));
        frame[4] = static_cast<char>(opcode);
        if (!body.empty()) {
            std::memcpy(frame.data() + 5, body.data(), body.size());
        }
        conn.out.append(frame);
        conn.t_send = now_ns();
        conn.send_times.push_back(conn.t_send);
        conn.pending_op = opcode;
        flush_output(index);
    }

    void flush_output(std::size_t index) {
        CConn& conn = m_conns[index];
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
                conn.want_write = true;
                break;
            }
            ++m_errors;
            if (m_options.scenario == Scenario::kShort) {
                close_conn(index);
                schedule_reconnect(index);
            } else {
                close_conn(index);
            }
            return;
        }
        if (conn.out_off >= conn.out.size()) {
            conn.out.clear();
            conn.out_off = 0;
            conn.want_write = false;
        }
        update_interest(index);
    }

    void update_interest(std::size_t index) {
        CConn& conn = m_conns[index];
        if (conn.fd < 0 || !conn.in_epoll) {
            return;
        }
        std::uint32_t events = 0;
        if (!conn.read_paused) {
            events |= EPOLLIN;
        }
        if (conn.want_write || conn.state == 1) {
            events |= EPOLLOUT;
        }
        epoll_event event {};
        event.events = events;
        event.data.u64 = index;
        ::epoll_ctl(m_epoll, EPOLL_CTL_MOD, conn.fd, &event);
    }

    void on_readable(std::size_t index) {
        CConn& conn = m_conns[index];
        if (conn.read_paused) {
            return;
        }
        char buffer[64 * 1024];
        for (;;) {
            const std::size_t limit = conn.slow ? 32 * 1024 : sizeof(buffer);
            const ssize_t n = ::recv(conn.fd, buffer, limit, 0);
            if (n > 0) {
                conn.in.append(buffer, static_cast<std::size_t>(n));
                m_bytes_rx += static_cast<std::uint64_t>(n);
                if (conn.slow) {
                    // 慢客户端：每次只读 32KB，然后停 20ms（≈1.6MB/s），
                    // 制造真实背压。
                    conn.read_paused = true;
                    conn.next_read_at = now_ns() + 20ULL * 1000000ULL;
                    m_timers.push(Timer{conn.next_read_at, index, conn.generation, 3});
                    update_interest(index);
                    break;
                }
                continue;
            }
            if (n == 0) {
                if (m_options.scenario == Scenario::kShort) {
                    close_conn(index);
                    schedule_reconnect(index);
                } else {
                    close_conn(index);
                }
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            ++m_errors;
            close_conn(index);
            return;
        }
        parse_responses(index);
    }

    void resume_read(std::size_t index) {
        CConn& conn = m_conns[index];
        if (conn.fd < 0) {
            return;
        }
        conn.read_paused = false;
        update_interest(index);
    }

    void parse_responses(std::size_t index) {
        CConn& conn = m_conns[index];
        for (;;) {
            const std::size_t available = conn.in.size() - conn.in_off;
            if (conn.pending_op == 0) {
                break;
            }
            if (available < 4) {
                break;
            }
            const std::uint32_t length = get_u32(conn.in.data() + conn.in_off);
            if (length > kMaxFrame) {
                ++m_errors;
                close_conn(index);
                return;
            }
            if (available < static_cast<std::size_t>(length) + 4) {
                break;
            }
            conn.in_off += static_cast<std::size_t>(length) + 4;
            on_response(index, length);
            if (m_conns[index].fd < 0) {
                return;
            }
            if (conn.in_off == conn.in.size()) {
                conn.in.clear();
                conn.in_off = 0;
            } else if (conn.in_off > (1U << 20)) {
                conn.in.erase(0, conn.in_off);
                conn.in_off = 0;
            }
        }
    }

    void on_response(std::size_t index, std::uint32_t length) {
        CConn& conn = m_conns[index];
        const std::uint8_t finished_op = conn.pending_op;
        conn.pending_op = 0;
        const std::uint64_t now = now_ns();
        std::uint64_t sent_at = conn.t_send;
        if (!conn.send_times.empty()) {
            sent_at = conn.send_times.front();
            conn.send_times.pop_front();
        }
        if (m_measuring) {
            ++m_completed;
            const std::uint64_t base =
                (m_options.scenario == Scenario::kShort) ? conn.t_connect : sent_at;
            if (base != 0 && now >= base) {
                m_latencies.push_back(now - base);
            }
        }
        m_resp_bytes += length;
        switch (m_options.scenario) {
            case Scenario::kShort: {
                if (m_measuring) {
                    ++m_short_completed;
                }
                close_conn(index);
                // 立即发起下一次连接；用定时器避免递归过深。
                m_timers.push(Timer{now, index, conn.generation, 1});
                break;
            }
            case Scenario::kEcho: {
                queue_request(index, kOpEcho, std::string(64, 'e'));
                break;
            }
            case Scenario::kIdle: {
                const std::uint64_t at =
                    now + static_cast<std::uint64_t>(m_options.interval_ms) * 1000000ULL;
                m_timers.push(Timer{at, index, conn.generation, 2});
                break;
            }
            case Scenario::kMixed: {
                if (finished_op == kOpRead) {
                    queue_request(index, kOpWrite, std::string());
                } else {
                    queue_request(index, kOpRead, std::string(kReadBody, 'r'));
                }
                break;
            }
            case Scenario::kSlow: {
                if (conn.slow) {
                    queue_request(index, kOpBig, std::string(1, 'b'));
                } else {
                    queue_request(index, kOpEcho, std::string(64, 'e'));
                }
                break;
            }
        }
    }

    void begin_measurement() {
        m_warmed_up = true;
        m_measuring = true;
        m_t0 = now_ns();
        m_completed = 0;
        m_errors = 0;
        m_bytes_rx = 0;
        m_resp_bytes = 0;
        m_latencies.clear();
        m_established_in_window = 0;
        m_short_completed = 0;
        for (auto& conn : m_conns) {
            if (conn.pending_op != 0) {
                conn.t_send = m_t0;
            }
            // 在途请求跨过预热边界时，把它们的时间基准统一挪到 m_t0，
            // 避免把预热期的等待算进测量窗口。
            for (auto& sent_at : conn.send_times) {
                sent_at = m_t0;
            }
        }
    }

    void close_all() {
        for (std::size_t i = 0; i < m_conns.size(); ++i) {
            close_conn(i);
        }
    }

    void report() {
        const std::uint64_t end = now_ns();
        const std::uint64_t elapsed = end > m_t0 ? end - m_t0 : 1;
        std::sort(m_latencies.begin(), m_latencies.end());
        const std::uint64_t count = m_completed;
        const double elapsed_s = static_cast<double>(elapsed) / 1e9;
        const double qps = elapsed_s > 0 ? static_cast<double>(count) / elapsed_s : 0.0;
        const double mbps =
            (static_cast<double>(m_resp_bytes) / (1024.0 * 1024.0)) / elapsed_s;
        const std::size_t samples = m_latencies.size();
        auto pick = [&](double p) -> std::uint64_t {
            if (samples == 0) {
                return 0;
            }
            std::size_t index = static_cast<std::size_t>(p * static_cast<double>(samples - 1));
            index = std::min(index, samples - 1);
            return m_latencies[index];
        };
        const std::uint64_t sum = [&] {
            std::uint64_t total = 0;
            for (auto value : m_latencies) {
                total += value;
            }
            return total;
        }();
        std::printf(
            "RESULT {\"scenario\":\"%s\",\"completed\":%llu,\"errors\":%llu,"
            "\"elapsed_ms\":%.3f,\"qps\":%.3f,\"resp_mbps\":%.3f,"
            "\"p50_ns\":%llu,\"p95_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,"
            "\"avg_ns\":%llu,\"samples\":%llu,\"established\":%llu,"
            "\"resp_bytes\":%llu,\"short_completed\":%llu}\n",
            scenario_name(m_options.scenario),
            static_cast<unsigned long long>(count),
            static_cast<unsigned long long>(m_errors),
            static_cast<double>(elapsed) / 1e6,
            qps,
            mbps,
            static_cast<unsigned long long>(pick(0.50)),
            static_cast<unsigned long long>(pick(0.95)),
            static_cast<unsigned long long>(pick(0.99)),
            static_cast<unsigned long long>(pick(0.999)),
            static_cast<unsigned long long>(samples > 0 ? sum / samples : 0),
            static_cast<unsigned long long>(samples),
            static_cast<unsigned long long>(m_established),
            static_cast<unsigned long long>(m_resp_bytes),
            static_cast<unsigned long long>(m_short_completed));
        std::fflush(stdout);
    }

    ClientOptions m_options;
    int m_epoll{-1};
    std::vector<CConn> m_conns;
    std::priority_queue<Timer, std::vector<Timer>, TimerGreater> m_timers;

    std::uint64_t m_warmup_end{0};
    std::uint64_t m_run_end{0};
    std::uint64_t m_t0{0};
    bool m_warmed_up{false};
    bool m_measuring{false};

    std::uint64_t m_completed{0};
    std::uint64_t m_errors{0};
    std::uint64_t m_bytes_rx{0};
    std::uint64_t m_resp_bytes{0};
    std::uint64_t m_established{0};
    std::uint64_t m_established_in_window{0};
    std::uint64_t m_short_completed{0};
    std::vector<std::uint64_t> m_latencies;
};

int run_epoll_server(std::uint16_t port, std::size_t workers, int sndbuf) {
    b2::EpollPoolServer server(port, workers, sndbuf);
    return server.run();
}

// ===========================================================================
// 参数解析与入口
// ===========================================================================

bool has_bool_flag(int argc, char** argv, const char* name) {
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

bool has_flag(int argc, char** argv, const char* name, std::string* value) {
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            *value = argv[i + 1];
            return true;
        }
    }
    return false;
}

std::string arg_string(int argc, char** argv, const char* name, const char* fallback) {
    std::string value;
    return has_flag(argc, argv, name, &value) ? value : std::string(fallback);
}

long long arg_int(int argc, char** argv, const char* name, long long fallback) {
    std::string value;
    if (!has_flag(argc, argv, name, &value)) {
        return fallback;
    }
    return std::strtoll(value.c_str(), nullptr, 10);
}

void usage() {
    std::fputs(
        "用法:\n"
        "  server --model gmp|thread|epoll [--port 0] [--workers 8]\n"
        "         [--sndbuf 0] [--stack-kb 8192]\n"
        "  client --scenario s1|s2|s3|s4|s5 --port N [--host 127.0.0.1]\n"
        "         [--conns 64] [--duration-ms 5000] [--warmup-ms 1000]\n"
        "         [--interval-ms 1000] [--depth 1]\n"
        "  run [--repeats 5] [--scenarios s1,s2,...] [--models gmp,thread,epoll]\n",
        stderr);
}

int server_main(int argc, char** argv) {
    install_stop_handlers();
    const std::string model = arg_string(argc, argv, "--model", "");
    const auto port =
        static_cast<std::uint16_t>(arg_int(argc, argv, "--port", 0));
    const auto workers = static_cast<std::size_t>(arg_int(argc, argv, "--workers", 8));
    const auto sndbuf = static_cast<int>(arg_int(argc, argv, "--sndbuf", 0));
    const auto stack_kb = static_cast<std::size_t>(arg_int(argc, argv, "--stack-kb", 8192));
    const bool raw_io = has_bool_flag(argc, argv, "--raw-io");
    const bool pin = has_bool_flag(argc, argv, "--pin");
    if (model == "gmp") {
        return run_gmp_server(port, sndbuf, raw_io, pin);
    }
    if (model == "thread") {
        return run_thread_server(port, sndbuf, stack_kb);
    }
    if (model == "epoll") {
        return run_epoll_server(port, workers, sndbuf);
    }
    usage();
    return 2;
}

int client_main(int argc, char** argv) {
    ClientOptions options;
    const std::string scenario = arg_string(argc, argv, "--scenario", "s2");
    if (scenario == "s1") {
        options.scenario = Scenario::kShort;
    } else if (scenario == "s2") {
        options.scenario = Scenario::kEcho;
    } else if (scenario == "s3") {
        options.scenario = Scenario::kIdle;
    } else if (scenario == "s4") {
        options.scenario = Scenario::kMixed;
    } else if (scenario == "s5") {
        options.scenario = Scenario::kSlow;
    } else {
        usage();
        return 2;
    }
    options.port = static_cast<std::uint16_t>(arg_int(argc, argv, "--port", 0));
    options.conns = static_cast<std::size_t>(arg_int(argc, argv, "--conns", 64));
    options.duration_ms = arg_int(argc, argv, "--duration-ms", 5000);
    options.warmup_ms = arg_int(argc, argv, "--warmup-ms", 1000);
    options.interval_ms = arg_int(argc, argv, "--interval-ms", 1000);
    options.depth = static_cast<std::size_t>(arg_int(argc, argv, "--depth", 1));
    if (options.port == 0) {
        usage();
        return 2;
    }
    LoadClient client(options);
    return client.run();
}

// ===========================================================================
// 编排器：进程内 fork/exec 自身，跑完 5 场景 × N 模型 × 5 次，打印中位数表
// ===========================================================================

struct ScenarioConfig {
    const char* key;
    const char* title;
    std::size_t conns;
    int warmup_ms;
    int duration_ms;
    int interval_ms;
    int slow_conns;
    int sndbuf;
};

const ScenarioConfig kScenarioTable[] = {
    {"s1", "短连接（1 请求即关）", 64, 1000, 5000, 0, 0, 0},
    {"s2", "长连接 echo", 64, 1000, 5000, 0, 0, 0},
    {"s3", "海量空闲连接（1 万，1 次/秒）", 10000, 3000, 10000, 1000, 0, 0},
    {"s4", "混合读写（50% READ / 50% WRITE）", 64, 1000, 5000, 0, 0, 0},
    {"s5", "慢客户端（4MB 响应，慢读）", 32, 2000, 10000, 0, 1, 262144},
};

struct ModelConfig {
    std::string key;
    std::string label;
    std::vector<std::string> server_args;
};

const std::vector<ModelConfig>& model_table() {
    static const std::vector<ModelConfig> models = {
        {"gmp", "GMP 协程", {"--model", "gmp", "--pin"}},
        {"thread", "每连接一线程", {"--model", "thread"}},
        {"epoll", "epoll+线程池", {"--model", "epoll", "--workers", "8"}},
    };
    return models;
}

struct ProcSample {
    int threads{0};
    long long rss_kb{0};
    long long voluntary{0};
    long long nonvoluntary{0};
    long long utime{0};
    long long stime{0};
};

ProcSample read_proc_sample(int pid) {
    ProcSample sample;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/status", pid);
    if (FILE* file = std::fopen(path, "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            if (std::strncmp(line, "Threads:", 8) == 0) {
                sample.threads = std::atoi(line + 8);
            } else if (std::strncmp(line, "VmRSS:", 6) == 0) {
                sample.rss_kb = std::atoll(line + 6);
            } else if (std::strncmp(line, "voluntary_ctxt_switches:", 24) == 0) {
                sample.voluntary = std::atoll(line + 24);
            } else if (std::strncmp(line, "nonvoluntary_ctxt_switches:", 27) == 0) {
                sample.nonvoluntary = std::atoll(line + 27);
            }
        }
        std::fclose(file);
    }
    std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (FILE* file = std::fopen(path, "r")) {
        char line[1024];
        if (std::fgets(line, sizeof(line), file) != nullptr) {
            if (char* close_paren = std::strrchr(line, ')')) {
                long long values[40] = {0};
                int count = 0;
                char* cursor = close_paren + 2;
                // 第一个 token 是 state（字母），先跳过；之后第 0 个数字
                // 对应原第 4 字段 ppid，因此 utime/stime（第 14/15 字段）
                // 是下标 10/11。
                if (char* space = std::strchr(cursor, ' ')) {
                    cursor = space + 1;
                }
                while (count < 40) {
                    char* end = nullptr;
                    const long long value = std::strtoll(cursor, &end, 10);
                    if (end == cursor) {
                        break;
                    }
                    values[count++] = value;
                    cursor = end;
                }
                if (count > 11) {
                    sample.utime = values[10];
                    sample.stime = values[11];
                }
            }
        }
        std::fclose(file);
    }
    return sample;
}

pid_t spawn_self(const std::vector<std::string>& arguments, int* stdout_fd) {
    int fds[2];
    if (::pipe(fds) != 0) {
        return -1;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>("gmp_vs_thread_epoll_benchmark"));
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv("/proc/self/exe", argv.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    *stdout_fd = fds[0];
    return pid;
}

std::string read_line_fd(int fd) {
    std::string line;
    char ch = 0;
    while (::read(fd, &ch, 1) == 1) {
        if (ch == '\n') {
            break;
        }
        line.push_back(ch);
    }
    return line;
}

std::string read_all_fd(int fd) {
    std::string data;
    char buffer[4096];
    ssize_t n = 0;
    while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
        data.append(buffer, static_cast<std::size_t>(n));
    }
    return data;
}

double json_value(const std::string& line, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const auto position = line.find(needle);
    if (position == std::string::npos) {
        return 0.0;
    }
    return std::strtod(line.c_str() + position + needle.size(), nullptr);
}

struct RunResult {
    bool ok{false};
    double qps{0};
    double p50{0};
    double p95{0};
    double p99{0};
    double mbps{0};
    double threads{0};
    double rss_kb{0};
    double cpu_pct{0};
    double cpu_us{0};
    double ctxt_per_req{0};
    double completed{0};
    double errors{0};
    double established{0};
};

RunResult run_once(const ModelConfig& model, const ScenarioConfig& scenario) {
    RunResult result;
    std::vector<std::string> server_args = {"server"};
    server_args.insert(server_args.end(), model.server_args.begin(), model.server_args.end());
    server_args.push_back("--port");
    server_args.push_back("0");
    if (scenario.sndbuf > 0) {
        server_args.push_back("--sndbuf");
        server_args.push_back(std::to_string(scenario.sndbuf));
    }
    int server_out = -1;
    const pid_t server = spawn_self(server_args, &server_out);
    if (server < 0) {
        return result;
    }
    const std::string ready = read_line_fd(server_out);
    int server_pid = 0;
    unsigned server_port = 0;
    if (std::sscanf(ready.c_str(), "READY pid=%d port=%u", &server_pid, &server_port) != 2 ||
        server_port == 0) {
        ::kill(server, SIGKILL);
        ::waitpid(server, nullptr, 0);
        ::close(server_out);
        std::fprintf(stderr, "[run] %s/%s 服务端未就绪: %s\n", model.key.c_str(),
                     scenario.key, ready.c_str());
        return result;
    }
    ::close(server_out);

    std::vector<std::string> client_args = {
        "client", "--scenario", scenario.key, "--port", std::to_string(server_port),
        "--conns", std::to_string(scenario.conns),
        "--warmup-ms", std::to_string(scenario.warmup_ms),
        "--duration-ms", std::to_string(scenario.duration_ms)};
    if (scenario.interval_ms > 0) {
        client_args.push_back("--interval-ms");
        client_args.push_back(std::to_string(scenario.interval_ms));
    }
    int client_out = -1;
    const pid_t client = spawn_self(client_args, &client_out);
    if (client < 0) {
        ::kill(server, SIGKILL);
        ::waitpid(server, nullptr, 0);
        return result;
    }

    ProcSample base{};
    bool have_base = false;
    ProcSample last{};
    long long threads_peak = 0;
    long long rss_peak = 0;
    const auto start = Clock::now();
    int status = 0;
    for (;;) {
        if (::waitpid(client, &status, WNOHANG) == client) {
            break;
        }
        const ProcSample sample = read_proc_sample(server_pid);
        last = sample;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - start)
                                    .count();
        if (!have_base && elapsed_ms >= scenario.warmup_ms) {
            base = sample;
            have_base = true;
        }
        if (have_base) {
            threads_peak = std::max(threads_peak, static_cast<long long>(sample.threads));
            rss_peak = std::max(rss_peak, sample.rss_kb);
        }
        ::usleep(50000);
    }
    if (!have_base) {
        base = last;
    }
    const double window_s = static_cast<double>(scenario.duration_ms) / 1000.0;
    const std::string output = read_all_fd(client_out);
    ::close(client_out);
    std::string result_line;
    for (std::size_t position = output.find("RESULT "); position != std::string::npos;
         position = output.find("RESULT ", position + 1)) {
        const auto end = output.find('\n', position);
        result_line = output.substr(position, end == std::string::npos
                                                   ? std::string::npos
                                                   : end - position);
    }
    ::kill(server, SIGTERM);
    bool reaped = false;
    for (int i = 0; i < 50; ++i) {
        if (::waitpid(server, nullptr, WNOHANG) == server) {
            reaped = true;
            break;
        }
        ::usleep(100000);
    }
    if (!reaped) {
        ::kill(server, SIGKILL);
        ::waitpid(server, nullptr, 0);
    }

    if (result_line.empty()) {
        std::fprintf(stderr, "[run] %s/%s 客户端无结果\n", model.key.c_str(), scenario.key);
        return result;
    }
    result.qps = json_value(result_line, "qps");
    result.p50 = json_value(result_line, "p50_ns");
    result.p95 = json_value(result_line, "p95_ns");
    result.p99 = json_value(result_line, "p99_ns");
    result.mbps = json_value(result_line, "resp_mbps");
    result.completed = json_value(result_line, "completed");
    result.errors = json_value(result_line, "errors");
    result.established = json_value(result_line, "established");
    result.threads = static_cast<double>(threads_peak);
    result.rss_kb = static_cast<double>(rss_peak);
    const long long cpu_ticks = (last.utime + last.stime) - (base.utime + base.stime);
    const long long ctxt =
        (last.voluntary + last.nonvoluntary) - (base.voluntary + base.nonvoluntary);
    const long hz = ::sysconf(_SC_CLK_TCK);
    result.cpu_pct = static_cast<double>(cpu_ticks) / hz / window_s * 100.0;
    result.cpu_us = result.completed > 0
                        ? static_cast<double>(cpu_ticks) / hz * 1e6 / result.completed
                        : 0.0;
    result.ctxt_per_req =
        result.completed > 0 ? static_cast<double>(ctxt) / result.completed : 0.0;
    result.ok = true;
    return result;
}

double median_of(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 == 0) {
        return (values[middle - 1] + values[middle]) / 2.0;
    }
    return values[middle];
}

int run_orchestrator(int argc, char** argv) {
    const long long repeats = std::max(1LL, arg_int(argc, argv, "--repeats", 5));
    const std::string only = arg_string(argc, argv, "--scenarios", "");
    const std::string only_model = arg_string(argc, argv, "--models", "");
    std::printf("# GMP 协程 vs thread/epoll 压测（每格 %lld 次取中位数）\n\n", repeats);
    std::printf("| 场景 | 模型 | QPS | P50(ms) | P95(ms) | P99(ms) | MB/s | 线程峰值 | RSS峰值(MB) | CPU%% | CPU(µs/req) | 切换/请求 | 完成 | 错误 |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (const auto& scenario : kScenarioTable) {
        if (!only.empty() && only.find(scenario.key) == std::string::npos) {
            continue;
        }
        for (const auto& model : model_table()) {
            if (!only_model.empty() && only_model.find(model.key) == std::string::npos) {
                continue;
            }
            std::vector<double> qps, p50, p95, p99, mbps, threads, rss, cpu, cpu_us, ctxt,
                completed, errors;
            for (long long repeat = 0; repeat < repeats; ++repeat) {
                const RunResult run = run_once(model, scenario);
                if (!run.ok) {
                    continue;
                }
                qps.push_back(run.qps);
                p50.push_back(run.p50);
                p95.push_back(run.p95);
                p99.push_back(run.p99);
                mbps.push_back(run.mbps);
                threads.push_back(run.threads);
                rss.push_back(run.rss_kb);
                cpu.push_back(run.cpu_pct);
                cpu_us.push_back(run.cpu_us);
                ctxt.push_back(run.ctxt_per_req);
                completed.push_back(run.completed);
                errors.push_back(run.errors);
                std::fprintf(stderr,
                             "[%s/%s] rep%lld: qps=%.0f p50=%.2fms p99=%.2fms threads=%.0f "
                             "rss=%.1fMB completed=%.0f errors=%.0f\n",
                             model.key.c_str(), scenario.key, repeat + 1, run.qps,
                             run.p50 / 1e6, run.p99 / 1e6, run.threads,
                             run.rss_kb / 1024.0, run.completed, run.errors);
            }
            if (qps.empty()) {
                std::printf("| %s | %s | 失败 | - | - | - | - | - | - | - | - | - | - | - |\n",
                            scenario.key, model.label.c_str());
                continue;
            }
            std::printf("| %s | %s | %.0f | %.2f | %.2f | %.2f | %.2f | %.0f | %.1f | %.1f | %.1f | %.2f | %.0f | %.0f |\n",
                        scenario.key, model.label.c_str(), median_of(qps),
                        median_of(p50) / 1e6, median_of(p95) / 1e6, median_of(p99) / 1e6,
                        median_of(mbps), median_of(threads), median_of(rss) / 1024.0,
                        median_of(cpu), median_of(cpu_us), median_of(ctxt),
                        median_of(completed), median_of(errors));
            std::fflush(stdout);
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "server") {
        return server_main(argc, argv);
    }
    if (mode == "client") {
        return client_main(argc, argv);
    }
    if (mode == "run") {
        return run_orchestrator(argc, argv);
    }
    usage();
    return 2;
}

