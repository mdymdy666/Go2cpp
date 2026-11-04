/**
 * @file douyin_shutdown.hpp
 * @brief 抖音 P0 骨架的优雅关闭协调器（header-only，gateway 与六个服务共用）。
 *
 * @details 语义（与 `setsid nohup` + `kill` 的运维方式配套）：
 *          - SIGTERM / SIGINT 只置位停止标志，不在信号上下文做任何 I/O；
 *          - 主循环（accept 循环）在普通上下文轮询该标志，收到后**停止 accept**，
 *            让在途请求自然处理完；
 *          - 在途请求有 5 s 上限：看门狗线程从信号到达起计时，超时打印日志并
 *            以退出码 1 强制结束；正常路径由主流程以退出码 0 结束。
 *
 * @note 进程内所有组件共享同一组标志（inline 变量），因此 gateway 的 watcher
 *       线程与服务端的 accept 循环看到的是同一个「已请求停止」状态。
 *
 * @author douyin demo
 * @date 2026-11-04
 * @copyright MIT License
 */

#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace douyin {

/// @brief 优雅关闭预算：停止 accept 后等待在途请求的硬上限。
inline constexpr std::chrono::milliseconds kGracefulShutdownTimeout{5000};

namespace shutdown_detail {

/// @brief 进程级停止标志：信号处理器只置位，普通上下文轮询读取。
inline std::atomic<bool>& StopFlag() {
    static std::atomic<bool> flag{false};
    return flag;
}

/// @brief 正常关闭完成标志：置位后看门狗不再强制退出。
inline std::atomic<bool>& FinishedFlag() {
    static std::atomic<bool> finished{false};
    return finished;
}

/// @brief 信号处理器只安装一次。
inline std::once_flag& InstallOnce() {
    static std::once_flag once;
    return once;
}

/// @brief 看门狗线程只启动一次。
inline std::once_flag& WatchdogOnce() {
    static std::once_flag once;
    return once;
}

/// @brief 信号处理器：只置位（async-signal-safe），不分配、不打印。
/// @param signal [in] 收到的信号编号；语义由调用方决定，本函数不区分。
inline void HandleSignal(int /*signal*/) {
    StopFlag().store(true, std::memory_order_relaxed);
}

/**
 * @brief 看门狗主循环：等待停止信号，然后在 5 s 内确认正常退出，否则强制 `_Exit(1)`。
 *
 * @details 放在独立线程的原因：串行 accept 循环可能正卡在单个请求的读写里，
 *          无法自己计时；看门狗是唯一能执行 5 s 硬上限的地方。
 */
inline void WatchdogLoop() {
    // step1: 信号到达前空转，不占用额外资源。
    while (!StopFlag().load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // step2: 从信号到达起算 5 s；正常路径会在这之前退出或置完成标志。
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < kGracefulShutdownTimeout) {
        if (FinishedFlag().load(std::memory_order_relaxed)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // step3: 超时——在途请求没在预算内完成，直接强制退出码 1。
    std::fprintf(stderr,
                 "[shutdown] 优雅关闭超时（%lldms）：在途请求未完成，强制退出（退出码 1）\n",
                 static_cast<long long>(kGracefulShutdownTimeout.count()));
    std::fflush(stderr);
    std::_Exit(1);
}

}  // namespace shutdown_detail

/**
 * @brief 安装 SIGTERM/SIGINT 处理器（幂等）。
 *
 * @details 使用 `SA_RESTART`：在途请求的 `recv` 不会被信号打断成 EINTR，
 *          这是「kill 不中断在途请求」的关键；accept 使用带超时的 poll，
 *          因此仍能在 200 ms 内感知停止标志。
 */
inline void InstallShutdownSignals() {
    std::call_once(shutdown_detail::InstallOnce(), [] {
        struct sigaction action {};
        action.sa_handler = &shutdown_detail::HandleSignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        ::sigaction(SIGTERM, &action, nullptr);
        ::sigaction(SIGINT, &action, nullptr);
    });
}

/// @brief 是否已收到停止信号（任意线程可读）。
/// @return true 表示应停止 accept 并进入关闭流程。
inline bool ShutdownRequested() {
    return shutdown_detail::StopFlag().load(std::memory_order_relaxed);
}

/// @brief 启动 5 s 看门狗（幂等；必须在进入 accept 循环前调用）。
inline void ArmShutdownWatchdog() {
    std::call_once(shutdown_detail::WatchdogOnce(), [] {
        std::thread(&shutdown_detail::WatchdogLoop).detach();
    });
}

/// @brief 标记正常关闭完成，让看门狗退出而不强制结束进程。
inline void MarkShutdownFinished() {
    shutdown_detail::FinishedFlag().store(true, std::memory_order_relaxed);
}

}  // namespace douyin
