/**
 * @file steady_clock.cpp
 * @brief 跨平台单调高精度时钟与睡眠实现。
 *
 * ## Windows 关键坑
 * `Sleep(1)` 实际睡 **15.6ms**（默认定时器分辨率）。本实现优先用
 * `CreateWaitableTimerExW` + `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`（Win10 1803+），
 * 失败才退化 `Sleep`。
 */
#include "go2cpp/platform/steady_clock.hpp"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <ctime>
#include <cerrno>
#endif

namespace go2cpp::platform {

std::int64_t SteadyNanos() noexcept {
#if defined(_WIN32)
  LARGE_INTEGER counter{};
  LARGE_INTEGER freq{};
  ::QueryPerformanceCounter(&counter);
  ::QueryPerformanceFrequency(&freq);
  if (freq.QuadPart == 0) {
    return 0;
  }
  // (counter * 1e9) / freq，避免溢出：先转 double 再取整（纳秒级足够）
  return static_cast<std::int64_t>(
      static_cast<double>(counter.QuadPart) * 1e9 /
      static_cast<double>(freq.QuadPart));
#else
  struct timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL +
         static_cast<std::int64_t>(ts.tv_nsec);
#endif
}

void SleepNanos(std::int64_t ns) noexcept {
  if (ns <= 0) {
    return;
  }
#if defined(_WIN32)
  // 优先高精度可等待定时器（Win10 1803+）
  static HANDLE timer = ::CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
      TIMER_ALL_ACCESS);
  if (timer != nullptr) {
    LARGE_INTEGER due{};
    // 负值 = 相对时间，单位 100ns
    due.QuadPart = -(ns / 100);
    if (::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      ::WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
  // 退化：Sleep 精度 15.6ms
  ::Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
#else
  struct timespec req{};
  req.tv_sec = static_cast<time_t>(ns / 1000000000LL);
  req.tv_nsec = static_cast<long>(ns % 1000000000LL);
  struct timespec rem{};
  // 处理 EINTR：剩余时间写 rem，重试
  while (::nanosleep(&req, &rem) == -1 && errno == EINTR) {
    req = rem;
  }
#endif
}

}  // namespace go2cpp::platform
