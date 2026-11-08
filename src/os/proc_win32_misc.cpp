/**
 * @file proc_win32_misc.cpp
 * @brief `proc.hpp` 里不依赖 `Process` 实现的公开函数（Windows 版）。
 *
 * ## 覆盖
 * | 函数 | Windows 实现 | 契约差异 |
 * |---|---|---|
 * | `DefaultProcessBackend()` | `RealBackend` | 与 Linux 相同 |
 * | `MakeRealClock()` | `steady_clock` | 相同 |
 * | `IgnoreSigPipe()` | **空操作** | Windows 无 SIGPIPE（写已关闭 socket 返回 `WSAECONNRESET`） |
 * | `Cleanup()` | **空操作** | 无 `waitpid` 残留可清理 |
 * | `CountOpenFds()` | `GetProcessHandleCount` | 统计的是**内核句柄**，不是 fd |
 *
 * 其余（`Spawn`/`Run`/`Process::*`）需 `ProcessImpl`，由后续轮次补到 `proc_win32.cpp`。
 */
#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>

#include <memory>

#include "go2cpp/os/proc.hpp"

namespace go2cpp::os {
namespace {

/// @brief 真实时钟（`steady_clock`）。
class RealClock final : public Clock {
 public:
  std::chrono::steady_clock::time_point Now() const noexcept override {
    return std::chrono::steady_clock::now();
  }
};

}  // namespace

std::shared_ptr<ProcessBackend> DefaultProcessBackend() {
  return std::make_shared<RealBackend>();
}

std::shared_ptr<Clock> MakeRealClock() { return std::make_shared<RealClock>(); }

void IgnoreSigPipe() {
  // Windows 无 SIGPIPE：写已关闭的 socket 返回 WSAECONNRESET/WSAENOTCONN。
  // 调用方按 `IsWouldBlock`/错误码处理即可。本函数保留为跨平台空操作。
}

void Cleanup() {
  // Windows 无 waitpid 残留（进程句柄由 Wait/Query 内部关闭）。
}

std::size_t CountOpenFds() {
  DWORD count = 0;
  if (::GetProcessHandleCount(::GetCurrentProcess(), &count)) {
    return static_cast<std::size_t>(count);
  }
  return 0;
}

}  // namespace go2cpp::os

#endif  // defined(_WIN32)
