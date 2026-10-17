#pragma once

/**
 * @file debug_signal_dump.hpp
 * @brief 信号驱动的跨进程 dump（第 22 轮，任务1 最后一项）。
 *
 * ## 为什么这样设计
 *
 * 真正的"dump 一个**正在运行**的进程"必须由**被 dump 的进程自己**执行 ——
 * 外部无法安全地读取另一个进程的 C++ 堆与运行时状态。标准做法是：
 *   1. 进程内注册信号处理器（POSIX 上是 `SIGUSR1`）；
 *   2. 外部用 `kill -USR1 <pid>` 触发；
 *   3. 进程在信号到来后把快照写到文件，外部再读该文件。
 *
 * ## 硬约束：信号处理器里只能做 async-signal-safe 的事
 *
 * 处理器**绝不能**做 I/O、分配内存、加锁、调用 `printf` —— 那些函数不是
 * async-signal-safe，在信号上下文里调用可能死锁或崩溃。
 * 因此本模块的处理器**只做一件事**：把一个 `std::atomic<bool>` 置位。
 * 真正的 dump 由 `PollOnce()`（主循环调用）或后台 watcher 线程完成。
 *
 * ## 已安装/未安装的行为差异（如实说明）
 *   - 未调用 `Install` 时，`SIGUSR1` 走内核默认动作（**终止进程**）——
 *     这是 POSIX 默认行为，本模块不改。
 *   - 调用 `Install` 后，`SIGUSR1` 被接管，进程不会被终止。
 *   - 本模块不处理 `SIGUSR2`、`SIGQUIT` 等其它信号。
 */

#include "go2cpp/debug/debug_events.hpp"
#include "go2cpp/debug/debug_scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <unistd.h>
#endif

namespace go2cpp::debug {

/**
 * @brief 信号驱动的 dump 控制器（进程内单例语义，全局最多装一次）。
 *
 * 依赖：POSIX `signal()`；被 dump 的对象由 `Install` 时的参数决定，**生命周期由调用方保证**。
 * 对上层提供：`Install`/`Uninstall`、`PollOnce`（主循环）、`StartWatcher`/`StopWatcher`（后台线程）。
 */
class DebugSignalDumper final {
public:
    /// dump 输出文件名的前缀。
    static constexpr const char* kFileNamePrefix = "go2cpp-signal-dump-";

    /**
     * @brief 安装信号处理器并绑定被 dump 的对象。
     * @param scheduler 调度器（生命周期必须长于本控制器）。
     * @param collector 可选；非空时 dump 事件。
     * @param output_dir 输出目录（空表示当前目录）。
     * @return 成功返回 true；失败时 `error` 填原因。
     * @note 重复调用会**替换**绑定，不会叠加多个处理器。
     */
    static bool Install(Scheduler& scheduler, DebugEventCollector* collector,
                        std::string output_dir, std::string* error) {
        State& state = Get();
        state.scheduler = &scheduler;
        state.collector = collector;
        state.output_dir = std::move(output_dir);
        state.installed.store(true, std::memory_order_release);
#if defined(__unix__) || defined(__APPLE__)
        // 只用 signal()：处理器内部只做一次原子置位，满足 async-signal-safe。
        if (std::signal(kSignal, &OnSignal) == SIG_ERR) {
            state.installed.store(false, std::memory_order_release);
            if (error) *error = "signal() 安装失败";
            return false;
        }
        return true;
#else
        if (error) *error = "当前平台不支持信号驱动 dump";
        return false;
#endif
    }

    /// 卸载：恢复默认动作并解绑。
    static void Uninstall() {
        State& state = Get();
        state.installed.store(false, std::memory_order_release);
#if defined(__unix__) || defined(__APPLE__)
        (void)std::signal(kSignal, SIG_DFL);
#endif
        state.scheduler = nullptr;
        state.collector = nullptr;
        state.requested.store(false, std::memory_order_release);
    }

    /// 是否有待处理的信号请求（不消费）。
    static bool Pending() noexcept {
        return Get().requested.load(std::memory_order_acquire);
    }

    /**
     * @brief 若收到过信号则执行一次 dump（消费该请求）。
     * @return 本次执行的 dump 次数（0 或 1）。
     * @note 必须在**普通上下文**（主循环或 watcher 线程）调用，绝不能在信号处理器里调用。
     */
    static std::size_t PollOnce(std::string* last_path = nullptr, std::string* error = nullptr) {
        State& state = Get();
        if (!state.installed.load(std::memory_order_acquire)) {
            if (error) *error = "未安装信号处理器";
            return 0;
        }
        if (!state.requested.exchange(false, std::memory_order_acq_rel)) {
            return 0;
        }
        if (state.scheduler == nullptr) {
            if (error) *error = "未绑定调度器";
            return 0;
        }
        const std::string path = BuildPath(state.output_dir);
        DumpOptions options;
        options.include_fibers = true;   // 跨进程排障最需要看清"有哪些 G"
        const std::string text = DebugScheduler(*state.scheduler).DumpText(options);
        std::string write_error;
        if (!WriteDumpFile(path, text, false, &write_error)) {
            if (error) *error = write_error;
            return 0;
        }
        if (state.collector != nullptr) {
            const std::string events_path = path + ".events.ini";
            std::string events_error;
            (void)WriteDumpFile(events_path, state.collector->DumpIni(0), false, &events_error);
        }
        ++state.dump_count;
        if (last_path != nullptr) {
            *last_path = path;
        }
        return 1;
    }

    /// 已执行的 dump 次数。
    static std::size_t DumpCount() noexcept {
        return Get().dump_count.load(std::memory_order_relaxed);
    }

    /**
     * @brief 启动后台 watcher 线程，按 interval 轮询并执行 dump。
     * @note 用独立线程是为了让**没有主循环**的程序也能响应信号。
     */
    static bool StartWatcher(std::chrono::milliseconds interval, std::string* error) {
        State& state = Get();
        if (state.watcher.joinable()) {
            if (error) *error = "watcher 已在运行";
            return false;
        }
        state.stopping.store(false, std::memory_order_release);
        state.watcher = std::thread([&state, interval] {
            while (!state.stopping.load(std::memory_order_acquire)) {
                (void)PollOnce();
                std::this_thread::sleep_for(interval);
            }
        });
        return true;
    }

    /// 停止 watcher 线程。
    static void StopWatcher() {
        State& state = Get();
        state.stopping.store(true, std::memory_order_release);
        if (state.watcher.joinable()) {
            state.watcher.join();
        }
    }

    /// 本模块使用的信号编号（供文档与测试引用）。
    static int SignalNumber() noexcept {
#if defined(__unix__) || defined(__APPLE__)
        return kSignal;
#else
        return 0;
#endif
    }

private:
#if defined(__unix__) || defined(__APPLE__)
    static constexpr int kSignal = SIGUSR1;
#endif

    struct State {
        std::atomic<bool> installed{false};
        std::atomic<bool> requested{false};
        std::atomic<bool> stopping{false};
        std::atomic<std::size_t> dump_count{0};
        Scheduler* scheduler{nullptr};
        DebugEventCollector* collector{nullptr};
        std::string output_dir;
        std::thread watcher;
    };

    static State& Get() noexcept {
        static State state;
        return state;
    }

    /// **信号处理器**：只做一次原子置位，满足 async-signal-safe。
    static void OnSignal(int) noexcept {
        Get().requested.store(true, std::memory_order_release);
    }

    static std::string BuildPath(const std::string& dir) {
        char buffer[160];
        long pid = 0;
#if defined(__unix__) || defined(__APPLE__)
        pid = static_cast<long>(::getpid());
#endif
        const auto now = std::chrono::system_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            now.time_since_epoch()).count();
        std::snprintf(buffer, sizeof(buffer), "%s%s%ld-%lld.txt",
                      dir.empty() ? "" : (dir + "/").c_str(), kFileNamePrefix, pid,
                      static_cast<long long>(ns));
        return std::string(buffer);
    }

    DebugSignalDumper() = delete;
};

}  // namespace go2cpp::debug
