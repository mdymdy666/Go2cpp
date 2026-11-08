/**
 * @file hook_win32_stub.cpp
 * @brief Windows 上的 hook 空实现（**非功能等价**）。
 *
 * ## 为什么需要这个文件
 * Go2Cpp 的 hook 层依赖 `LD_PRELOAD` + `dlsym(RTLD_NEXT)` 拦截 libc 调用，
 * **Windows 没有等价机制**（最接近的 API hooking 需要每进程注入 + trampoline，
 * 工程量和稳定性都不可接受）。
 *
 * Go 的 goroutine 是**编译器插桩**（`morestack` + 抢占点），不需要 hook；
 * Go2Cpp 用 hook 在用户层模拟——**Windows 上这条路走不通**。
 *
 * ## 所以 Windows 上
 * - `enabled()` **恒 false**（永不接管）；
 * - 所有系统调用**直通**（无 park、无 fiber 化）；
 * - 依赖 hook 的「同步代码自动并发」在 Windows 上**不生效**——调用方需用
 *   显式异步 API（`AsyncRead`/`AsyncWrite`，见 `docs/reports/go-cross-platform-study.md`）。
 *
 * 详见 `docs/plans/cross-platform-plan.md` §1.3（hook 是 TOP1 风险）与
 * `docs/reports/cross-platform-syscall-audit.md`（硬阻断 1）。
 */
#if !defined(__linux__)

#include "go2cpp/network/hook.hpp"

#include <chrono>

namespace go2cpp::hook {

void set_enabled(bool /*enabled*/) noexcept {}
bool enabled() noexcept { return false; }

void bind_io_manager(IOManager* /*manager*/) noexcept {}
IOManager* bound_io_manager() noexcept { return nullptr; }

void mark_system_nonblocking(std::intptr_t /*fd*/) noexcept {}

void set_connect_timeout(std::chrono::milliseconds /*timeout*/) noexcept {}
std::chrono::milliseconds connect_timeout() noexcept {
  return std::chrono::milliseconds(0);
}

bool ShouldRetryAfterEintr(int /*retries_used*/) noexcept { return false; }

void reset_error_counters() noexcept {}

bool OpFromString(const std::string& /*name*/, HookOp* /*out*/) noexcept {
  // Windows 上所有 op 都不可控；不写 out（调用方看到 false 就不该读它）。
  return false;
}

bool OpEnabled(HookOp /*op*/) noexcept { return false; }
bool SetOpEnabled(HookOp /*op*/, bool /*enabled*/) noexcept { return false; }

}  // namespace go2cpp::hook

#endif  // !defined(__linux__)
