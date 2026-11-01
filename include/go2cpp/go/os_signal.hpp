/**
 * @file os_signal.hpp
 * @brief Go `os/signal` 包的 C++ 对应物（Notify / Stop / Ignore / Reset）。
 *
 * @author DeepSeek Harness
 * @date 2026-11-01
 * @copyright BSD-3-Clause（语义对照 Go `os/signal`，实现借鉴 Go runtime
 *            的「信号进入队列 → 分发到 channel」结构，未复制上游代码）
 *
 * @details
 * Go 的 `os/signal` 把「进程收到的异步信号」变成「channel 上的值」：
 *
 * ```go
 * c := make(chan os.Signal, 1)
 * signal.Notify(c, os.Interrupt, syscall.SIGTERM)
 * defer signal.Stop(c)
 *
 * for {
 *     switch sig := <-c; sig {
 *     case os.Interrupt: return
 *     case syscall.SIGTERM: shutdown()
 *     }
 * }
 * ```
 *
 * 对应 C++：
 *
 * ```cpp
 * go::Chan<go::Signal> c = go::Chan<go::Signal>::Make(1);
 * go::signal::Notify(c, {go::Interrupt, go::Signal(SIGTERM)});
 * // ... 结束时 go::signal::Stop(c);
 *
 * go::Signal sig;
 * while (c.Recv(sig)) {
 *     if (sig == go::Interrupt) return;
 * }
 * ```
 *
 * ## 为什么用 sigaction 而不是 signalfd
 *
 * 任务书给了 `signalfd` 与 `sigaction` 两个候选，这里选**后者**，原因是
 * `signalfd` 的语义要求调用线程先 `pthread_sigmask(SIG_BLOCK)`，而
 * `pthread_sigmask` **只影响当前线程**：Go2Cpp 的进程里同时有 fiber 调度线程、
 * timer 线程、IO 线程，只要其中任何一个没有屏蔽该信号，信号就会被内核投递给
 * 它并按默认处置（多数是直接杀进程），`signalfd` 永远收不到。
 * 想做到「全进程屏蔽」就得到每个线程创建处插钩子，改动面远大于本任务边界。
 *
 * `sigaction` 安装的是**进程级**处理器，任何线程收到信号都会走同一个 handler。
 * handler 里只做三件 async-signal-safe 的事：
 *   1. 读原子掩码判断该信号有没有监听者；
 *   2. 有则 `write(2)` 一个字节到自建 pipe（pipe 满则丢弃，等价 Go 的
 *      「channel 满时信号被丢弃」）；
 *   3. 没有监听者也没有 `Ignore` 则恢复 `SIG_DFL` 并 `raise(2)` 重新投递，
 *      让信号的默认处置（终止/忽略/核心转储）照常发生——这正是 Go 的
 *      `dieFromSignal` 做的事。
 *
 * 独立的**分发线程**阻塞读 pipe，把信号编号转成 `Signal` 值后对每个注册的
 * channel 做**非阻塞** `TrySend`（与 Go 的 `sig.send` 一致：channel 满时丢弃，
 * 绝不阻塞信号处理路径）。
 *
 * ## 与 Go 的对应关系
 *
 * | Go | 本文件 | 备注 |
 * |---|---|---|
 * | `signal.Notify(c, sigs...)` | `signal::Notify(c, {sigs...})` | 空列表 = 全部可捕获信号 |
 * | `signal.Stop(c)` | `signal::Stop(c)` | 移除该 channel 的全部注册 |
 * | `signal.Ignore(sigs...)` | `signal::Ignore({sigs...})` | 信号被吞掉，不投递 |
 * | `signal.Reset(sigs...)`（Go 1.18） | `signal::Reset({sigs...})` | 恢复默认处置 |
 * | `os.Interrupt` | `go::Interrupt` | 与 Go 一样是 SIGINT |
 * | `os.Kill` | `go::Kill` | SIGKILL，**不可捕获**（Notify 无效） |
 * | `syscall.Signal(n)` | `go::Signal(n)` | 构造不校验，越界信号被静默忽略 |
 * | `sig.String()` | `sig.String()` | 与 Go 的 syscall 名字表一致（见实现） |
 * | `signal.NotifyContext(ctx, sigs...)` | **未提供** | 需要 context 依赖，见下方边界 |
 *
 * ## 语义边界（必须知道）
 *
 * - **channel 满即丢**：与 Go 相同——`Notify` 不做阻塞发送，未及时消费的
 *   信号会被丢弃。需要不丢信号就给 channel 足够容量（Go 的惯例是容量 1）。
 * - **向已关闭的 channel 发送**：Go 会 panic；本实现静默丢弃（C++ 无 panic，
 *   且 `Channel::TrySend` 对已关闭通道返回失败）。文档在此标注为**行为差异**。
 * - **多次 Notify 是并集**：与 Go 1.16+ 一致（"each call expands the set"）。
 *   要换集合只能先 `Stop`（或对单个信号 `Reset`）。
 * - **Ignore 与 Notify 的优先级**：`Notify` 覆盖同一信号的 `Ignore`
 *   （与 Go 的「后来者生效」一致）；`Ignore` 只在该信号没有监听者时才真正
 *   改变处置（吞掉而非默认动作）。
 * - **`SA_RESTART`**：本实现安装的 handler 带 `SA_RESTART`，因此被这些信号
 *   打断的 `read`/`write` 会自动重启。Go 的 runtime 自己处理 `EINTR`，行为
 *   对用户代码等价。
 * - **信号编号范围 1..63**：Linux 常规信号（1..31）与实时信号 32..63 的
 *   低段都覆盖；64 及以上的实时信号不注册（`Signal(64)` 会被静默忽略）。
 *   这是 64 位原子掩码的取舍，文档在此说明。
 * - **不可捕获信号**：`SIGKILL`/`SIGSTOP` 无法安装 handler，`Notify` 对它们
 *   是 no-op（Go 同样如此）。
 * - **Windows**：当前只提供接口与 `Supported() == false` 的退化实现
 *   （`std::signal` 无法表达「多通道 + 默认处置恢复」的语义）。POSIX 上
 *   `Supported()` 为 true。
 * - **进程级接管**：`Notify` 会覆盖宿主进程已有的 `sigaction`（Go 亦然）。
 *   不想影响宿主时请只 `Notify` 自己关心的信号。
 *
 * ## 线程模型
 *
 * - `Notify`/`Stop`/`Ignore`/`Reset` 互斥，可在任意线程调用；
 * - 分发线程在第一次 `Notify`/`Ignore` 时惰性创建，进程生命周期内常驻
 *   （Go 的 signal goroutine 同样常驻）；
 * - 分发时对 channel 只做 `TrySend`，因此**不会**在信号路径上分配内存或阻塞；
 * - 用户 channel 用 `shared_ptr` 持有（`Chan` 内部即 `shared_ptr`），因此
 *   即使调用方先销毁 `Chan` 对象，注册表仍然安全（但 `Stop` 就无从调用了，
 *   这是 Go 里「忘记 Stop 导致泄漏」的等价物）。
 *
 * @warning `Signal` 只承载信号编号，不携带 `siginfo_t`（Go 的 `os.Signal`
 *          也不保证携带；需要 `si_code`/`si_pid` 时请直接用 `sigaction`）。
 */

#pragma once

#include "go2cpp/go/chan.go.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace go2cpp::go {

/**
 * @brief 对应 Go 的 `os.Signal` / `syscall.Signal`（只承载信号编号）。
 *
 * @details 值语义、可平凡复制，因此可以安全地经 `Chan` 跨线程传递。
 *          `String()` 的名字表与 Go `syscall.Signal.String()` 一致。
 */
class Signal final {
public:
    /// 空信号（编号 0，不对应任何真实信号）。
    constexpr Signal() noexcept = default;

    /// 由信号编号构造；**不校验**合法性（与 `syscall.Signal(n)` 一致）。
    constexpr Signal(int number) noexcept : m_number(number) {}

    /// 对应 Go 的 `int(sig)`。
    constexpr int Number() const noexcept { return m_number; }

    /// 是否对应一个真实信号（编号 > 0）。
    constexpr bool Valid() const noexcept { return m_number > 0; }

    /// 对应 Go 的 `sig.String()`（如 `"interrupt"`、`"terminated"`）。
    std::string String() const;

    constexpr bool operator==(Signal other) const noexcept {
        return m_number == other.m_number;
    }
    constexpr bool operator!=(Signal other) const noexcept {
        return m_number != other.m_number;
    }
    constexpr bool operator<(Signal other) const noexcept {
        return m_number < other.m_number;
    }

private:
    int m_number{0};  ///< 信号编号；默认 0 = 空信号
};

/// 对应 `os.Interrupt`（SIGINT）。
inline constexpr Signal Interrupt{SIGINT};
/// 对应 `os.Kill`（SIGKILL，**不可捕获、不可忽略**）。
inline constexpr Signal Kill{SIGKILL};

/**
 * @brief `Signal::String()` 的映射表；与 Go `syscall` 的名字逐项一致。
 *
 * @details 用 switch 而不是查表数组，是为了在编译期就能发现重复编号；
 *          未列出的信号返回 `"signal N"`（Go 对未知名也是这个格式）。
 */
inline std::string Signal::String() const {
    switch (m_number) {
#if defined(SIGHUP)
        case SIGHUP: return "hangup";
#endif
#if defined(SIGINT)
        case SIGINT: return "interrupt";
#endif
#if defined(SIGQUIT)
        case SIGQUIT: return "quit";
#endif
#if defined(SIGILL)
        case SIGILL: return "illegal instruction";
#endif
#if defined(SIGTRAP)
        case SIGTRAP: return "trace/breakpoint trap";
#endif
#if defined(SIGABRT)
        case SIGABRT: return "aborted";
#endif
#if defined(SIGBUS)
        case SIGBUS: return "bus error";
#endif
#if defined(SIGFPE)
        case SIGFPE: return "floating point exception";
#endif
#if defined(SIGKILL)
        case SIGKILL: return "killed";
#endif
#if defined(SIGUSR1)
        case SIGUSR1: return "user defined signal 1";
#endif
#if defined(SIGSEGV)
        case SIGSEGV: return "segmentation fault";
#endif
#if defined(SIGUSR2)
        case SIGUSR2: return "user defined signal 2";
#endif
#if defined(SIGPIPE)
        case SIGPIPE: return "broken pipe";
#endif
#if defined(SIGALRM)
        case SIGALRM: return "alarm clock";
#endif
#if defined(SIGTERM)
        case SIGTERM: return "terminated";
#endif
#if defined(SIGCHLD)
        case SIGCHLD: return "child exited";
#endif
#if defined(SIGCONT)
        case SIGCONT: return "continued";
#endif
#if defined(SIGSTOP)
        case SIGSTOP: return "stopped";
#endif
#if defined(SIGTSTP)
        case SIGTSTP: return "stopped";
#endif
#if defined(SIGTTIN)
        case SIGTTIN: return "stopped";
#endif
#if defined(SIGTTOU)
        case SIGTTOU: return "stopped";
#endif
#if defined(SIGURG)
        case SIGURG: return "urgent I/O condition";
#endif
#if defined(SIGXCPU)
        case SIGXCPU: return "CPU time limit exceeded";
#endif
#if defined(SIGXFSZ)
        case SIGXFSZ: return "file size limit exceeded";
#endif
#if defined(SIGVTALRM)
        case SIGVTALRM: return "virtual timer expired";
#endif
#if defined(SIGPROF)
        case SIGPROF: return "profiling timer expired";
#endif
#if defined(SIGWINCH)
        case SIGWINCH: return "window changed";
#endif
#if defined(SIGIO)
        case SIGIO: return "I/O possible";
#endif
#if defined(SIGPWR)
        case SIGPWR: return "power failure";
#endif
#if defined(SIGSYS)
        case SIGSYS: return "bad system call";
#endif
        default: break;
    }
    return "signal " + std::to_string(m_number);
}

namespace signal {

/**
 * @brief 当前平台是否实现了信号通知。
 * @return POSIX（Linux/macOS）为 true；Windows 为 false（接口保留）。
 */
inline bool Supported() noexcept {
#if defined(_WIN32)
    return false;
#else
    return true;
#endif
}

namespace detail {

/// 支持的信号编号上界（不含）。1..63 覆盖常规信号与低段实时信号。
constexpr int kMaxSignal = 64;

#if !defined(_WIN32)

/**
 * @brief 信号分发的进程级状态。
 *
 * @details 通过 `new` 创建并**永不销毁**：信号可能随时到达，若在静态析构
 *          阶段销毁状态会踩到 use-after-free。进程退出时内核回收，属于
 *          有意为之的「进程级单例」泄漏（Go 的 signal 状态同样常驻）。
 *
 * ## 不变量
 *
 * - `watched_mask` / `ignored_mask` / `armed_mask` 是**唯一**能在 handler 里
 *   读取的状态（无锁原子）；
 * - `watchers` 与 `mutex` 只允许普通线程访问，分发给 channel 时先持锁拷贝
 *   目标列表，再在锁外 `TrySend`（避免持锁进入 Channel 内部）；
 * - `pipe_write` 为非阻塞：pipe 满时 handler 丢弃字节，不阻塞信号路径。
 */
struct SignalHub {
    std::mutex mutex;                                        ///< 保护 watchers 与注册流程
    std::map<int, std::vector<ChannelPtr<Signal>>> watchers;  ///< 信号 → 目标 channel 列表
    std::atomic<std::uint64_t> watched_mask{0};               ///< 有 channel 监听的信号位图（handler 只读）
    std::atomic<std::uint64_t> ignored_mask{0};               ///< 被 Ignore 的信号位图（handler 只读）
    std::atomic<std::uint64_t> armed_mask{0};                 ///< 已安装本模块 handler 的信号位图
    int pipe_read{-1};                                        ///< 分发线程的读端（阻塞）
    int pipe_write{-1};                                       ///< handler 的写端（非阻塞）
    bool started{false};                                      ///< 分发线程是否已创建
};

/// 常量初始化的全局指针：handler 可以直接读，不触发任何函数内静态对象的初始化。
inline std::atomic<SignalHub*> g_hub{nullptr};

/**
 * @brief 取（首次调用时创建）信号状态。
 * @note 只能在普通线程调用；handler 通过 `g_hub` 访问。
 */
inline SignalHub& Hub() {
    static SignalHub* hub = [] {
        auto* state = new SignalHub();
        int fds[2] = {-1, -1};
        if (::pipe(fds) == 0) {
            state->pipe_read = fds[0];
            state->pipe_write = fds[1];
            // 读端保持阻塞（分发线程等数据），写端非阻塞（handler 永不卡住）。
            const int flags = ::fcntl(state->pipe_read, F_GETFD);
            if (flags >= 0) {
                ::fcntl(state->pipe_read, F_SETFD, flags | FD_CLOEXEC);
            }
            const int write_flags = ::fcntl(state->pipe_write, F_GETFL);
            if (write_flags >= 0) {
                ::fcntl(state->pipe_write, F_SETFL, write_flags | O_NONBLOCK);
            }
            const int write_fd_flags = ::fcntl(state->pipe_write, F_GETFD);
            if (write_fd_flags >= 0) {
                ::fcntl(state->pipe_write, F_SETFD, write_fd_flags | FD_CLOEXEC);
            }
        }
        g_hub.store(state, std::memory_order_release);
        return state;
    }();
    return *hub;
}

/// 把信号编号转成位图掩码；越界返回 0。
constexpr std::uint64_t Mask(int number) noexcept {
    return (number > 0 && number < kMaxSignal)
               ? (std::uint64_t{1} << static_cast<unsigned>(number))
               : 0;
}

/// 恢复信号的默认处置（`SIG_DFL`）。
inline void RestoreDefault(int number) noexcept {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    ::sigemptyset(&action.sa_mask);
    ::sigaction(number, &action, nullptr);
}

/// 安装本模块的 handler；`SIGKILL`/`SIGSTOP` 会失败并被静默跳过。
inline bool InstallHandler(int number) noexcept {
    struct sigaction action {};
    action.sa_handler = [](int signo) {
        SignalHub* hub = g_hub.load(std::memory_order_relaxed);
        if (hub == nullptr) {
            return;
        }
        const std::uint64_t bit = Mask(signo);
        if (bit == 0) {
            return;
        }
        if ((hub->watched_mask.load(std::memory_order_relaxed) & bit) != 0) {
            const auto byte = static_cast<unsigned char>(signo);
            const ssize_t written = ::write(hub->pipe_write, &byte, 1);
            (void)written;  // pipe 满即丢弃，与 Go 的 channel 满丢弃一致
            return;
        }
        if ((hub->ignored_mask.load(std::memory_order_relaxed) & bit) != 0) {
            return;  // Ignore：吞掉，不改变处置
        }
        // 没有监听者也没有 Ignore：恢复默认处置并重新投递（Go 的 dieFromSignal）。
        RestoreDefault(signo);
        hub->armed_mask.fetch_and(~bit, std::memory_order_relaxed);
        ::raise(signo);
    };
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (::sigaction(number, &action, nullptr) != 0) {
        return false;
    }
    g_hub.load(std::memory_order_relaxed)->armed_mask.fetch_or(Mask(number),
                                                               std::memory_order_relaxed);
    return true;
}

/// 分发线程主体：阻塞读 pipe，逐个信号交给 `Dispatch`。
inline void DispatchLoop(SignalHub* hub) noexcept {
    unsigned char buffer[64];
    for (;;) {
        const ssize_t count = ::read(hub->pipe_read, buffer, sizeof(buffer));
        if (count > 0) {
            for (ssize_t index = 0; index < count; ++index) {
                const int number = static_cast<int>(buffer[index]);
                std::vector<ChannelPtr<Signal>> targets;
                {
                    std::lock_guard<std::mutex> lock(hub->mutex);
                    const auto found = hub->watchers.find(number);
                    if (found == hub->watchers.end()) {
                        continue;
                    }
                    targets = found->second;
                }
                const Signal value(number);
                for (const auto& channel : targets) {
                    if (channel) {
                        channel->TrySend(value);  // 非阻塞：满了就丢
                    }
                }
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;  // pipe 关闭或不可恢复的错误
    }
}

/// 惰性启动分发线程（调用方必须已持有 hub.mutex）。
inline void StartDispatcherLocked(SignalHub& hub) {
    if (hub.started || hub.pipe_read < 0) {
        return;
    }
    hub.started = true;
    std::thread(DispatchLoop, &hub).detach();
}

/// 把 channel 注册到某个信号（调用方必须已持有 hub.mutex）。
inline void RegisterLocked(SignalHub& hub, const ChannelPtr<Signal>& channel,
                           int number) {
    const std::uint64_t bit = Mask(number);
    if (bit == 0 || !channel) {
        return;
    }
    auto& targets = hub.watchers[number];
    if (std::find(targets.begin(), targets.end(), channel) == targets.end()) {
        targets.push_back(channel);
    }
    hub.watched_mask.fetch_or(bit, std::memory_order_relaxed);
    // Notify 覆盖同一信号的 Ignore（Go 的「后来者生效」）。
    hub.ignored_mask.fetch_and(~bit, std::memory_order_relaxed);
    if ((hub.armed_mask.load(std::memory_order_relaxed) & bit) == 0) {
        InstallHandler(number);
    }
    StartDispatcherLocked(hub);
}

/// 某个信号已无监听者：撤销通知并（在未 Ignore 时）恢复默认处置。
inline void DisarmLocked(SignalHub& hub, int number) {
    const std::uint64_t bit = Mask(number);
    if (bit == 0) {
        return;
    }
    hub.watched_mask.fetch_and(~bit, std::memory_order_relaxed);
    if ((hub.ignored_mask.load(std::memory_order_relaxed) & bit) != 0) {
        return;  // 仍被 Ignore：handler 继续吞掉
    }
    hub.armed_mask.fetch_and(~bit, std::memory_order_relaxed);
    RestoreDefault(number);
}

/// 把信号列表登记到 channel（`signals` 为空表示全部可捕获信号）。
inline void NotifyList(Chan<Signal>& channel,
                       std::initializer_list<Signal> signals) {
    if (!channel.Valid()) {
        return;  // Go 的 nil channel 永久阻塞；这里选择忽略而不是挂死
    }
    const ChannelPtr<Signal> raw = channel.Raw();
    SignalHub& hub = Hub();
    std::lock_guard<std::mutex> lock(hub.mutex);
    if (signals.size() == 0) {
        for (int number = 1; number < kMaxSignal; ++number) {
#if defined(SIGKILL)
            if (number == SIGKILL) {
                continue;  // 不可捕获，InstallHandler 必然失败
            }
#endif
#if defined(SIGSTOP)
            if (number == SIGSTOP) {
                continue;
            }
#endif
            RegisterLocked(hub, raw, number);
        }
        return;
    }
    for (const Signal& value : signals) {
        RegisterLocked(hub, raw, value.Number());
    }
}

/// 从注册表移除 channel；某信号无人监听时恢复默认处置。
inline void StopChannel(Chan<Signal>& channel) {
    const ChannelPtr<Signal> raw = channel.Raw();
    if (!raw) {
        return;
    }
    SignalHub& hub = Hub();
    std::lock_guard<std::mutex> lock(hub.mutex);
    for (auto entry = hub.watchers.begin(); entry != hub.watchers.end();) {
        auto& targets = entry->second;
        targets.erase(std::remove(targets.begin(), targets.end(), raw),
                      targets.end());
        if (!targets.empty()) {
            ++entry;
            continue;
        }
        DisarmLocked(hub, entry->first);
        entry = hub.watchers.erase(entry);
    }
}

/// 忽略指定信号（该信号没有监听者时生效；有监听者时 Notify 优先）。
inline void IgnoreList(std::initializer_list<Signal> signals) {
    SignalHub& hub = Hub();
    std::lock_guard<std::mutex> lock(hub.mutex);
    for (const Signal& value : signals) {
        const int number = value.Number();
        const std::uint64_t bit = Mask(number);
        if (bit == 0) {
            continue;
        }
        hub.ignored_mask.fetch_or(bit, std::memory_order_relaxed);
        if ((hub.armed_mask.load(std::memory_order_relaxed) & bit) == 0) {
            InstallHandler(number);
        }
        StartDispatcherLocked(hub);
    }
}

/// 撤销 Notify/Ignore 对指定信号的影响，恢复默认处置。
inline void ResetList(std::initializer_list<Signal> signals) {
    SignalHub& hub = Hub();
    std::lock_guard<std::mutex> lock(hub.mutex);
    for (const Signal& value : signals) {
        const int number = value.Number();
        const std::uint64_t bit = Mask(number);
        if (bit == 0) {
            continue;
        }
        hub.ignored_mask.fetch_and(~bit, std::memory_order_relaxed);
        const auto found = hub.watchers.find(number);
        if (found != hub.watchers.end()) {
            hub.watchers.erase(found);
        }
        DisarmLocked(hub, number);
    }
}

#else  // _WIN32：接口保留，能力降级

struct SignalHub {
    std::mutex mutex;
};

inline SignalHub& Hub() {
    static SignalHub hub;
    return hub;
}
inline void NotifyList(Chan<Signal>&, std::initializer_list<Signal>) {}
inline void StopChannel(Chan<Signal>&) {}
inline void IgnoreList(std::initializer_list<Signal>) {}
inline void ResetList(std::initializer_list<Signal>) {}

#endif  // _WIN32

}  // namespace detail

/**
 * @brief 对应 `signal.Notify(c, sigs...)`：把信号转发到 `c`。
 * @param channel 目标通道（必须是有效通道；nil 通道被静默忽略）。
 * @param signals 要监听的信号；**省略表示全部可捕获信号**。
 *
 * @details 可以多次对同一通道调用：信号集合取**并集**（Go 1.16+ 语义）。
 *          投递是非阻塞的：通道满时信号被丢弃。
 */
inline void Notify(Chan<Signal>& channel,
                   std::initializer_list<Signal> signals = {}) {
    detail::NotifyList(channel, signals);
}

/// `Notify` 的单信号形式，便于 `Notify(c, go::Interrupt)` 直接写。
inline void Notify(Chan<Signal>& channel, Signal first) {
    detail::NotifyList(channel, {first});
}

/// `Notify` 的两信号形式（常见写法：SIGINT + SIGTERM）。
inline void Notify(Chan<Signal>& channel, Signal first, Signal second) {
    detail::NotifyList(channel, {first, second});
}

/**
 * @brief 对应 `signal.Stop(c)`：停止向 `c` 转发；无人监听的信号恢复默认处置。
 * @param channel 之前传给 `Notify` 的通道。
 */
inline void Stop(Chan<Signal>& channel) {
    detail::StopChannel(channel);
}

/**
 * @brief 对应 `signal.Ignore(sigs...)`：忽略这些信号（不投递、不触发默认动作）。
 * @param signals 要忽略的信号。
 */
inline void Ignore(std::initializer_list<Signal> signals) {
    detail::IgnoreList(signals);
}

/// `Ignore` 的单信号形式。
inline void Ignore(Signal first) {
    detail::IgnoreList({first});
}

/**
 * @brief 对应 `signal.Reset(sigs...)`（Go 1.18+）：恢复这些信号的默认处置。
 * @param signals 要重置的信号；移除其全部监听者并清除 Ignore 状态。
 */
inline void Reset(std::initializer_list<Signal> signals) {
    detail::ResetList(signals);
}

/// `Reset` 的单信号形式。
inline void Reset(Signal first) {
    detail::ResetList({first});
}

}  // namespace signal
}  // namespace go2cpp::go
