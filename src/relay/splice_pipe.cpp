/**
 * @file splice_pipe.cpp
 * @brief `SplicePipe` 实现：socket -> pipe -> socket 双向零拷贝转发。
 *
 * @details 每方向一条线程，循环为「源端 `splice` 进管道 -> 目的端 `splice`
 *          出管道」。数据全程留在内核页缓存，用户态只搬长度；没有队列、
 *          没有 `memcpy`、没有 chunk 分配。
 *
 * ## 关键不变量
 * - 一个方向内，管道中未完成的字节数恒等于「已从源端读入、尚未推给目的端」
 *   的字节数；写侧必须把 `moved` 字节全部推完才能回到读侧（否则字节顺序会乱）；
 * - 只有 EOF 才 `shutdown(目的端, SHUT_WR)`；失败路径直接判定方向失败并
 *   `shutdown` 目的端（慢 peer 隔离）；
 * - `timeouts` 每方向最多记一次；`a_to_b`/`b_to_a` 只统计实际推入目的端的字节。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#include "go2cpp/relay/splice_pipe.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace go2cpp {
namespace relay {
namespace {

using Clock = std::chrono::steady_clock;

/// 默认参数（与 `SpliceConfig` 成员初值保持一致）。
constexpr std::size_t kDefaultPipeBytes = 1U * 1024U * 1024U;
constexpr std::size_t kDefaultChunk = 256U * 1024U;
constexpr std::chrono::milliseconds kDefaultTimeout{30000};

/// 读侧无数据时的 poll 分片；取消/关闭能在这个粒度内打断。
constexpr int kIdlePollMs = 500;

/// 归一化配置：零值回落默认。
SpliceConfig NormalizeConfig(SpliceConfig cfg) {
    if (cfg.pipe_bytes == 0) {
        cfg.pipe_bytes = kDefaultPipeBytes;
    }
    if (cfg.splice_chunk == 0) {
        cfg.splice_chunk = kDefaultChunk;
    }
    if (cfg.write_timeout <= std::chrono::milliseconds::zero()) {
        cfg.write_timeout = kDefaultTimeout;
    }
    return cfg;
}

/// 环境变量真值解析（1/true/on/yes，大小写不敏感）。
bool ParseTruthy(const char* raw) {
    if (raw == nullptr) {
        return false;
    }
    std::string value(raw);
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value == "1" || value == "true" || value == "on" || value == "yes";
}

/// 失败文本；冷段，避免 `strerror` 相关代码进热循环。
[[gnu::noinline]] std::string ErrnoText(int code) {
    return std::strerror(code);
}

}  // namespace

struct SplicePipe::State final
    : std::enable_shared_from_this<SplicePipe::State> {
    State(int a, int b, SpliceConfig in)
        : a_fd(a), b_fd(b), cfg(NormalizeConfig(in)) {}

    /// A 端 fd（不拥有）。
    int a_fd;
    /// B 端 fd（不拥有）。
    int b_fd;
    /// 归一化配置。
    SpliceConfig cfg;

    /// 已推入目的端的字节数。
    std::atomic<std::uint64_t> a_to_b{0};
    std::atomic<std::uint64_t> b_to_a{0};
    /// deadline 事件数；每方向最多一次。
    std::atomic<std::uint64_t> timeouts{0};
    /// 取消标志；由 `Run` 的取消回调置位。
    std::atomic<bool> cancel{false};
    /// `Run` 是否已进入；只允许一次。
    std::atomic<bool> run_started{false};

    /// 保护 `done_count` 与等待通知。
    std::mutex done_mutex;
    /// 两个方向都结束时（或取消时）唤醒 `Run`。
    std::condition_variable done_cv;
    /// 已结束的方向数。
    int done_count{0};

    /// 保护 `error`。
    std::mutex error_mutex;
    /// 首个错误。
    ErrorPtr error;

    /// 每方向是否干净结束；线程 join 后读取。
    bool clean[2] = {true, true};

    /// 方向线程。
    std::thread workers[2];

    /// 记一个方向结束。
    void NoteDone() {
        std::lock_guard<std::mutex> lock(done_mutex);
        ++done_count;
        done_cv.notify_all();
    }

    /// 只记录首个错误。
    void SetError(ErrorPtr err) {
        if (!err) {
            return;
        }
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!error) {
            error = std::move(err);
        }
    }

    /// 累加交付字节。
    void AddDelivered(int index, std::size_t bytes) {
        if (index == 0) {
            a_to_b.fetch_add(bytes, std::memory_order_relaxed);
        } else {
            b_to_a.fetch_add(bytes, std::memory_order_relaxed);
        }
    }

    /// 记一次 timeout（每方向由调用方保证只调一次）。
    void NoteTimeout() { timeouts.fetch_add(1, std::memory_order_relaxed); }

    /// 取消：置标志并 shutdown 两端，打断阻塞中的 `poll`/`splice`。
    void Cancel() {
        cancel.store(true, std::memory_order_release);
#if defined(__linux__)
        // 非 Linux 平台没有 splice 路径（Run 因 Supported() 为假直接返回），
        // 这里也必须隔离 ::shutdown，否则非 Linux 编译缺少声明。
        if (a_fd >= 0) {
            (void)::shutdown(a_fd, SHUT_RDWR);
        }
        if (b_fd >= 0) {
            (void)::shutdown(b_fd, SHUT_RDWR);
        }
#endif
        std::lock_guard<std::mutex> lock(done_mutex);
        done_cv.notify_all();
    }

    /**
     * @brief 等待 fd 就绪。
     *
     * @param fd [in] 目标 fd。
     * @param events [in] `POLLIN` / `POLLOUT`。
     * @param budget [in] 等待上限（毫秒）；0 表示立即返回。
     * @return 就绪（含 `POLLHUP`，交给上层按 EOF/EPIPE 处理）返回 true；
     *         超时、`POLLERR`/`POLLNVAL` 或系统错误返回 false。
     */
    static bool WaitFd(int fd, short events, int budget_ms) {
        for (;;) {
            struct pollfd entry{};
            entry.fd = fd;
            entry.events = events;
            const int ready = ::poll(&entry, 1, budget_ms);
            if (ready > 0) {
                return (entry.revents & (POLLERR | POLLNVAL)) == 0;
            }
            if (ready == 0) {
                return false;
            }
            if (errno != EINTR) {
                return false;
            }
        }
    }

    /**
     * @brief 单个方向的转发循环：`in_fd` -> 管道 -> `out_fd`。
     *
     * @param index [in] 0 = A->B，1 = B->A；决定统计字段与 `clean[]` 槽位。
     * @param in_fd [in] 源端 fd（socket）。
     * @param out_fd [in] 目的端 fd（socket）。
     *
     * ## 并发/失败语义
     * - 只在 `Run` 创建的方向线程里调用；
     * - 取消回调可能并发 `shutdown` 两端，使 `poll` 立即返回、`splice` 报错退出；
     * - 失败后本方向 `clean[index] = false`，另一方向不受影响。
     */
    void Loop(int index, int in_fd, int out_fd);

    /// 起两个方向线程并等待结束。
    bool Run(const ContextPtr& ctx, ErrorPtr* err);
};

void SplicePipe::State::Loop(int index, int in_fd, int out_fd) {
#if !defined(__linux__)
    (void)index;
    (void)in_fd;
    (void)out_fd;
    SetError(NewError("splice: unsupported platform"));
    clean[index] = false;
    NoteDone();
#else
    int pipe_fds[2] = {-1, -1};
    if (::pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        SetError(NewError("splice: pipe2: " + ErrnoText(errno)));
        clean[index] = false;
        NoteDone();
        return;
    }
    // 管道容量尽力调大：内核默认 64 KiB 时每次 splice 只能搬 64 KiB，
    // 256 KiB chunk 会被拆成多次 syscall。设置失败不影响正确性。
    (void)::fcntl(pipe_fds[1], F_SETPIPE_SZ,
                  static_cast<int>(std::min<std::size_t>(
                      cfg.pipe_bytes, static_cast<std::size_t>(0x7fffffff))));

    bool ok = true;
    while (!cancel.load(std::memory_order_acquire)) {
        // step1: 源端 -> 管道。EAGAIN 表示管道满或源端暂无数据，poll 等待。
        const ssize_t moved = ::splice(in_fd, nullptr, pipe_fds[1], nullptr,
                                       cfg.splice_chunk,
                                       SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
        if (moved < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!WaitFd(in_fd, POLLIN, kIdlePollMs) &&
                    cancel.load(std::memory_order_acquire)) {
                    ok = false;
                }
                continue;
            }
            SetError(NewError("splice: read side: " + ErrnoText(errno)));
            ok = false;
            break;
        }
        if (moved == 0) {
            // step2: 源端 EOF。只关闭目的端写方向，让对端看到 FIN；
            // 反方向（另一条线程）继续，直到它也 EOF——与 RelayPipe 的
            // 「正常 EOF 不关闭两端」语义一致。
            (void)::shutdown(out_fd, SHUT_WR);
            break;
        }
        // step3: 管道 -> 目的端，必须把 moved 字节全部推完（部分写推进）。
        std::size_t left = static_cast<std::size_t>(moved);
        const auto deadline = Clock::now() + cfg.write_timeout;
        while (left > 0) {
            const ssize_t pushed = ::splice(pipe_fds[0], nullptr, out_fd, nullptr,
                                            left,
                                            SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
            if (pushed > 0) {
                left -= static_cast<std::size_t>(pushed);
                AddDelivered(index, static_cast<std::size_t>(pushed));
                continue;
            }
            if (pushed < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    const auto now = Clock::now();
                    if (now >= deadline) {
                        NoteTimeout();
                        SetError(NewError("splice: write deadline exceeded"));
                        ok = false;
                        break;
                    }
                    const auto remaining =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - now);
                    if (!WaitFd(out_fd, POLLOUT,
                                static_cast<int>(std::max<std::int64_t>(
                                    1, remaining.count()))) &&
                        Clock::now() >= deadline) {
                        NoteTimeout();
                        SetError(NewError("splice: write deadline exceeded"));
                        ok = false;
                        break;
                    }
                    continue;
                }
                SetError(NewError("splice: write side: " + ErrnoText(errno)));
                ok = false;
                break;
            }
            // pushed == 0 且 left > 0：管道里没有数据了，说明状态被破坏。
            SetError(NewError("splice: write side made no progress"));
            ok = false;
            break;
        }
        if (!ok) {
            break;
        }
    }
    clean[index] = ok && !cancel.load(std::memory_order_acquire);
    (void)::close(pipe_fds[0]);
    (void)::close(pipe_fds[1]);
    NoteDone();
#endif
}

bool SplicePipe::State::Run(const ContextPtr& context, ErrorPtr* err) {
    bool expected = false;
    if (!run_started.compare_exchange_strong(expected, true)) {
        if (err != nullptr) {
            *err = NewError("splice: Run already called");
        }
        return false;
    }
    const ContextPtr ctx = context ? context : Context::Background();
    if (ctx->IsDone()) {
        if (err != nullptr) {
            *err = NewError("splice: canceled before start");
        }
        return false;
    }

    // 取消回调只捕获 state：`Run` 返回后回调仍可安全执行（只碰 fd 与原子量）。
    const std::shared_ptr<State> keep = shared_from_this();
    const DoneSignal::CallbackId callback_id = ctx->Done().AddCallback([keep] {
        keep->Cancel();
    });

    try {
        workers[0] = std::thread([this] { Loop(0, a_fd, b_fd); });
        workers[1] = std::thread([this] { Loop(1, b_fd, a_fd); });
    } catch (...) {
        // 已启动的方向线程可能正阻塞在 poll/splice：先置取消并 shutdown 两端，
        // 再回收。否则 joinable 的 std::thread 在 State 析构时 std::terminate，
        // 且线程会继续访问即将销毁的 State（use-after-free）。
        cancel.store(true, std::memory_order_release);
#if defined(__linux__)
        if (a_fd >= 0) {
            (void)::shutdown(a_fd, SHUT_RDWR);
        }
        if (b_fd >= 0) {
            (void)::shutdown(b_fd, SHUT_RDWR);
        }
#endif
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        ctx->Done().RemoveCallback(callback_id);
        if (err != nullptr) {
            *err = NewError("splice: cannot start worker thread");
        }
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(done_mutex);
        done_cv.wait(lock, [this] {
            return done_count == 2 || cancel.load(std::memory_order_acquire);
        });
    }
    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    ctx->Done().RemoveCallback(callback_id);

    if (cancel.load(std::memory_order_acquire)) {
        if (err != nullptr) {
            const ErrorPtr cause = ctx->Err();
            *err = cause ? cause : NewError("splice: canceled");
        }
        return false;
    }
    if (clean[0] && clean[1]) {
        return true;
    }
    if (err != nullptr) {
        std::lock_guard<std::mutex> lock(error_mutex);
        *err = error ? error : NewError("splice: aborted");
    }
    return false;
}

SplicePipe::SplicePipe(int a_fd, int b_fd, SpliceConfig cfg)
    : m_state(std::make_shared<State>(a_fd, b_fd, cfg)) {}

SplicePipe::~SplicePipe() = default;

bool SplicePipe::Run(const ContextPtr& ctx, ErrorPtr* err) {
    if (err != nullptr) {
        *err = {};
    }
    if (!Supported()) {
        if (err != nullptr) {
            *err = NewError(UnsupportedReason());
        }
        return false;
    }
    if (m_state->a_fd < 0 || m_state->b_fd < 0) {
        if (err != nullptr) {
            *err = NewError("splice: invalid fd");
        }
        return false;
    }
    return m_state->Run(ctx, err);
}

SpliceStats SplicePipe::Stats() const {
    SpliceStats stats;
    stats.a_to_b = m_state->a_to_b.load(std::memory_order_relaxed);
    stats.b_to_a = m_state->b_to_a.load(std::memory_order_relaxed);
    stats.timeouts = m_state->timeouts.load(std::memory_order_relaxed);
    return stats;
}

const SpliceConfig& SplicePipe::Config() const noexcept { return m_state->cfg; }

bool SplicePipe::Supported() noexcept {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

std::string SplicePipe::UnsupportedReason() {
#if defined(__linux__)
    return {};
#else
    return "splice: unsupported platform (requires Linux)";
#endif
}

bool SpliceEnabledFromEnv() {
    return ParseTruthy(std::getenv("GO2CPP_TUNNEL_SPLICE"));
}

}  // namespace relay
}  // namespace go2cpp
