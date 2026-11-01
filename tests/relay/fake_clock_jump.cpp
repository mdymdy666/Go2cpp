/**
 * @file fake_clock_jump.cpp
 * @brief 测试用 LD_PRELOAD 垫片：按需把 `CLOCK_REALTIME` 拨快/拨慢 1 小时。
 *
 * ## 为什么需要它
 *
 * 「Lease 用单调时钟、不受墙钟跳变影响」不能只靠读代码断言。这里用
 * `LD_PRELOAD` 拦截 `clock_gettime`：只改写 `CLOCK_REALTIME` 的返回值，
 * 完全不碰 `CLOCK_MONOTONIC`。测试进程里的 `std::chrono::system_clock`
 * 因此会看到 ±1 小时的跳变，而 `steady_clock`（以及 glibc 条件变量的
 * `pthread_cond_clockwait(CLOCK_MONOTONIC)`）保持原样——这正是我们要区分的
 * 两种情况。
 *
 * ## 触发方式
 *
 * 不按时间自动跳，而是导出三个函数由测试主动调用（`dlsym(RTLD_DEFAULT, ...)`）：
 * - `go2cpp_fake_clock_jump_forward()`：之后墙钟 = 真实墙钟 + 3600s；
 * - `go2cpp_fake_clock_jump_backward()`：之后墙钟 = 真实墙钟 - 3600s；
 * - `go2cpp_fake_clock_reset()`：恢复正常。
 *
 * 这样「跳变发生在租约计时中间」是确定性的，不依赖进程启动时序。
 *
 * ## 实现注意
 *
 * - 通过 `dlsym(RTLD_NEXT, ...)` 找真正的 `clock_gettime`；解析期间禁止重入，
 *   极端情况下 dlsym 内部会调用它；
 * - 解析失败时退回 `syscall(SYS_clock_gettime, ...)`，保证垫片自身不会把
 *   进程带进「返回 -1」的异常路径；
 * - 本文件只被 `relay_lease_clockjump` 用例使用，不进入任何库目标。
 */
#include <dlfcn.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>

namespace {

using ClockGettimeFn = int (*)(clockid_t, struct timespec *);

constexpr std::int64_t kJumpSeconds = 3600;

std::atomic<ClockGettimeFn> g_real_clock_gettime{nullptr};
std::atomic<std::int64_t> g_offset_seconds{0};
/// dlsym 解析期间的重入保护（解析本身可能间接调用 clock_gettime）。
thread_local bool g_resolving = false;

/// @brief 返回真正的 clock_gettime；解析失败时返回 nullptr。
ClockGettimeFn ResolveReal() {
    ClockGettimeFn real = g_real_clock_gettime.load(std::memory_order_acquire);
    if (real != nullptr || g_resolving) {
        return real;
    }
    g_resolving = true;
    real = reinterpret_cast<ClockGettimeFn>(dlsym(RTLD_NEXT, "clock_gettime"));
    g_resolving = false;
    g_real_clock_gettime.store(real, std::memory_order_release);
    return real;
}

} // namespace

extern "C" void go2cpp_fake_clock_jump_forward(void) {
    g_offset_seconds.store(kJumpSeconds, std::memory_order_release);
}

extern "C" void go2cpp_fake_clock_jump_backward(void) {
    g_offset_seconds.store(-kJumpSeconds, std::memory_order_release);
}

extern "C" void go2cpp_fake_clock_reset(void) {
    g_offset_seconds.store(0, std::memory_order_release);
}

extern "C" int clock_gettime(clockid_t clock_id, struct timespec *ts) {
    const ClockGettimeFn real = ResolveReal();
    if (real == nullptr) {
        // 兜底：直接系统调用，至少保证时钟仍然可读。
        return static_cast<int>(syscall(SYS_clock_gettime, clock_id, ts));
    }
    const int result = real(clock_id, ts);
    if (result != 0 || clock_id != CLOCK_REALTIME || g_resolving) {
        return result;
    }
    const std::int64_t offset = g_offset_seconds.load(std::memory_order_acquire);
    if (offset != 0) {
        ts->tv_sec += static_cast<time_t>(offset);
    }
    return result;
}
