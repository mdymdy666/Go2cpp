/**
 * @file steady_clock.hpp
 * @brief 跨平台单调高精度时钟 + 睡眠——统一 `clock_gettime` / `QueryPerformanceCounter`。
 *
 * ## 定位
 * Timer / Scheduler / 基准测量都依赖「单调、高精度、不受系统时间调整影响」的时钟。
 * Linux 用 `clock_gettime(CLOCK_MONOTONIC)`；Windows 用 `QueryPerformanceCounter`。
 *
 * ## 边界与陷阱（**Windows 最大坑**）
 * - **`Sleep(1)` 在 Windows 上实际睡 15.6ms**（默认定时器分辨率）——不能用它做毫秒级等待；
 * - 本模块的 `SleepNanos` 在 Windows 上用 **`CreateWaitableTimerExW` + `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`**（Win10 1803+），
 *   退化路径才用 `Sleep`；
 * - `SteadyNanos()` 的**零点是任意的**（各平台不同），**只能做差**，不能当绝对时间。
 */
#pragma once

#include <cstdint>

namespace go2cpp::platform {

/// @brief 单调时钟的纳秒计数（零点任意，只用于做差）。
std::int64_t SteadyNanos() noexcept;

/// @brief 睡眠指定纳秒（可被信号打断，返回后调用方应检查剩余）。
/// @param ns [in] 纳秒；<=0 立即返回。
/// @note Windows 上用高精度可等待定时器（Win10 1803+），否则退化 `Sleep`。
void SleepNanos(std::int64_t ns) noexcept;

}  // namespace go2cpp::platform
