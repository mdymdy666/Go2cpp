/**
 * @file test_hook_sleep.cpp
 * @brief 验证 `nanosleep` / `usleep` / `clock_nanosleep` 的 hook 行为。
 *
 * ## 判定方法：心跳增量
 *
 * 单 worker（`processor_count = 1`）+ 心跳 Fiber（每 10ms 加一）：
 * - 心跳增量 **> 8 / 200ms** → 调用 **park 了 Fiber**（worker 没被占住）
 * - 心跳增量 **< 3 / 200ms** → 调用 **阻塞了 worker**
 *
 * ## 关键教训（这个文件里刻意遵守）
 *
 * **必须先有探针断言**：证明心跳 Fiber 确实在跑（`before >= 3`），
 * 否则 `delta == 0` 无法区分「心跳没跑」与「心跳被阻塞」。
 *
 * ## 三个函数各自的语义差异（实现里已处理，测试要覆盖）
 *
 * | 函数 | 返回值 | 剩余时间 | 特殊点 |
 * |---|---|---|---|
 * `sleep` | 剩余**秒数**（0 = 睡满） | 无 | |
 * `nanosleep` | 0 / -1 + `errno` | 写回 `rem` | 被信号打断是 EINTR |
 * `usleep` | 0 / -1 + `errno` | 无 | POSIX 已废弃 |
 * `clock_nanosleep` | **0 / errno 本身** | 相对时间才写回 | `TIMER_ABSTIME` |
 */

#include <go2cpp/network/hook.hpp>
#include <go2cpp/network/io.hpp>
#include <go2cpp/runtime.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <thread>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[hook-sleep] FAILED: %s\n", what);
        ++g_failures;
    }
}

using namespace std::chrono_literals;

/// 在单 worker 上：先确认心跳在跑，再执行 `action`，返回心跳增量与探针值。
///
/// `action` 在**另一个 Fiber** 里执行，所以它的阻塞会占住唯一的 worker。
template <typename Action>
int MeasureHeartbeatDelta(const char* label, Action action, int* probe_before,
                          std::size_t* action_elapsed_ms) {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    go2cpp::IOManager manager(config);
    if (!manager.Start()) {
        Check(false, "IOManager::Start failed");
        return -1;
    }
    go2cpp::hook::ScopedIOManagerBinding binding(&manager);

    std::atomic<int> heartbeats{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};

    manager.Go([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            heartbeats.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(10ms);
        }
    });

    // 探针：先让心跳跑 80ms，确认它真的在动。
    std::this_thread::sleep_for(80ms);
    const int before = heartbeats.load();
    *probe_before = before;

    manager.Go([&] {
        const auto t0 = std::chrono::steady_clock::now();
        action();
        *action_elapsed_ms = static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        done.store(true, std::memory_order_release);
    });

    // 观察窗 200ms。
    std::this_thread::sleep_for(200ms);
    const int after = heartbeats.load();
    const int delta = after - before;

    stop.store(true, std::memory_order_release);
    std::this_thread::sleep_for(30ms);

    std::fprintf(stderr,
                 "[hook-sleep] %s: before=%d, after=%d, delta=%d, 动作耗时=%zums\n",
                 label, before, after, delta, *action_elapsed_ms);
    return delta;
}

// ---------------------------------------------------------------------------
// 场景 1：Fiber 里 nanosleep 应该 park（心跳继续）
// ---------------------------------------------------------------------------
void ScenarioNanosleepParks() {
    std::fprintf(stderr, "[hook-sleep][场景1] Fiber 里 nanosleep(100ms) 应 park\n");
    int probe = 0;
    std::size_t elapsed = 0;
    const int delta = MeasureHeartbeatDelta("nanosleep(100ms)", [] {
        const timespec request{0, 100000000L};  // 100ms
        timespec remaining{};
        const int rc = ::nanosleep(&request, &remaining);
        Check(rc == 0, "nanosleep should succeed");
    }, &probe, &elapsed);

    // 探针：心跳必须先跑起来，否则 delta 无意义。
    Check(probe >= 3, "probe: heartbeat must be running before measuring");
    Check(delta > 8, "nanosleep must park the Fiber (heartbeats should continue)");
    Check(elapsed >= 90, "nanosleep should actually wait ~100ms");
}

// ---------------------------------------------------------------------------
// 场景 2：Fiber 里 usleep 应该 park
// ---------------------------------------------------------------------------
void ScenarioUsleepParks() {
    std::fprintf(stderr, "[hook-sleep][场景2] Fiber 里 usleep(100ms) 应 park\n");
    int probe = 0;
    std::size_t elapsed = 0;
    const int delta = MeasureHeartbeatDelta("usleep(100000)", [] {
        const int rc = ::usleep(100000);  // 100ms
        Check(rc == 0, "usleep should succeed");
    }, &probe, &elapsed);

    Check(probe >= 3, "probe: heartbeat must be running before measuring");
    Check(delta > 8, "usleep must park the Fiber (heartbeats should continue)");
    Check(elapsed >= 90, "usleep should actually wait ~100ms");
}

// ---------------------------------------------------------------------------
// 场景 3：Fiber 里 clock_nanosleep(相对) 应该 park
// ---------------------------------------------------------------------------
void ScenarioClockRelativeParks() {
    std::fprintf(stderr, "[hook-sleep][场景3] Fiber 里 clock_nanosleep(CLOCK_MONOTONIC, 100ms) 应 park\n");
    int probe = 0;
    std::size_t elapsed = 0;
    const int delta = MeasureHeartbeatDelta("clock_nanosleep(相对 100ms)", [] {
        const timespec request{0, 100000000L};
        // 注意：clock_nanosleep 成功返回 0，失败返回 errno 本身。
        const int rc = ::clock_nanosleep(CLOCK_MONOTONIC, 0, &request, nullptr);
        Check(rc == 0, "clock_nanosleep(relative) should return 0");
    }, &probe, &elapsed);

    Check(probe >= 3, "probe: heartbeat must be running before measuring");
    Check(delta > 8, "clock_nanosleep must park the Fiber (heartbeats should continue)");
    Check(elapsed >= 90, "clock_nanosleep should actually wait ~100ms");
}

// ---------------------------------------------------------------------------
// 场景 4：TIMER_ABSTIME 且目标时刻已过 → 立即返回 0（不 park、不睡）
// ---------------------------------------------------------------------------
void ScenarioClockAbsolutePast() {
    std::fprintf(stderr, "[hook-sleep][场景4] clock_nanosleep(TIMER_ABSTIME, 过去) 应立即返回\n");
    const timespec past{0, 0};  // 绝对时刻 0 = 1970，必然已过
    const auto t0 = std::chrono::steady_clock::now();

    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    go2cpp::IOManager manager(config);
    Check(manager.Start(), "IOManager::Start failed");
    go2cpp::hook::ScopedIOManagerBinding binding(&manager);

    std::atomic<int> rc_value{-99};
    manager.Go([&] {
        rc_value.store(::clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &past, nullptr),
                       std::memory_order_release);
    });
    // 给 Fiber 一点时间执行
    std::this_thread::sleep_for(80ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    Check(rc_value.load() == 0, "TIMER_ABSTIME with past target must return 0");
    Check(elapsed < 200, "TIMER_ABSTIME with past target must return immediately (no park)");
}

// ---------------------------------------------------------------------------
// 场景 5：TIMER_ABSTIME 且目标在未来 → 应 park 到点
// ---------------------------------------------------------------------------
void ScenarioClockAbsoluteFuture() {
    std::fprintf(stderr, "[hook-sleep][场景5] clock_nanosleep(TIMER_ABSTIME, 未来 100ms) 应 park\n");
    int probe = 0;
    std::size_t elapsed = 0;
    const int delta = MeasureHeartbeatDelta("clock_nanosleep(绝对 +100ms)", [] {
        // CLOCK_MONOTONIC 的绝对时刻：now + 100ms
        timespec now{};
        ::clock_gettime(CLOCK_MONOTONIC, &now);
        timespec target = now;
        target.tv_nsec += 100000000L;
        if (target.tv_nsec >= 1000000000L) {
            target.tv_nsec -= 1000000000L;
            target.tv_sec += 1;
        }
        const int rc = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr);
        Check(rc == 0, "clock_nanosleep(absolute future) should return 0");
    }, &probe, &elapsed);

    Check(probe >= 3, "probe: heartbeat must be running before measuring");
    Check(delta > 8, "clock_nanosleep(absolute future) must park the Fiber");
    Check(elapsed >= 80, "clock_nanosleep(absolute future) should actually wait");
}

// ---------------------------------------------------------------------------
// 场景 6：普通线程里 nanosleep 真阻塞（说明 * _f 与 hook 版在非 Fiber 下都能用）
// ---------------------------------------------------------------------------
void ScenarioPlainThread() {
    std::fprintf(stderr, "[hook-sleep][场景6] 普通线程里 nanosleep / *_f 都可用\n");
    const timespec request{0, 50000000L};  // 50ms

    const auto t0 = std::chrono::steady_clock::now();
    const int rc1 = ::nanosleep(&request, nullptr);
    const auto e1 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    const auto t1 = std::chrono::steady_clock::now();
    const int rc2 = nanosleep_f(&request, nullptr);
    const auto e2 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t1).count();

    const auto t2 = std::chrono::steady_clock::now();
    const int rc3 = usleep_f(50000);
    const auto e3 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t2).count();

    std::fprintf(stderr,
                 "[hook-sleep][场景6] nanosleep=%d/%lldms, nanosleep_f=%d/%lldms, usleep_f=%d/%lldms\n",
                 rc1, static_cast<long long>(e1), rc2, static_cast<long long>(e2),
                 rc3, static_cast<long long>(e3));

    Check(rc1 == 0 && e1 >= 40, "nanosleep in plain thread should wait ~50ms");
    Check(rc2 == 0 && e2 >= 40, "nanosleep_f should wait ~50ms");
    Check(rc3 == 0 && e3 >= 40, "usleep_f should wait ~50ms");
}

// ---------------------------------------------------------------------------
// 场景 7：开关关闭后 nanosleep 仍可用（走原生，不 park）
// ---------------------------------------------------------------------------
void ScenarioDisabledStillWorks() {
    std::fprintf(stderr, "[hook-sleep][场景7] 关闭 kNanosleep 后仍能睡\n");
    const bool ok = go2cpp::hook::SetOpEnabled(go2cpp::hook::HookOp::kNanosleep, false);
    Check(ok, "SetOpEnabled(nanosleep, false) should be accepted (it is not non-disableable)");

    const timespec request{0, 30000000L};  // 30ms
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = ::nanosleep(&request, nullptr);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::fprintf(stderr, "[hook-sleep][场景7] 关闭后 nanosleep=%d/%lldms\n",
                 rc, static_cast<long long>(elapsed));
    Check(rc == 0 && elapsed >= 20, "nanosleep should still work when hook disabled");

    // 还原，避免影响其它用例
    go2cpp::hook::SetOpEnabled(go2cpp::hook::HookOp::kNanosleep, true);
}

}  // namespace

int main() {
    std::fprintf(stderr, "=== nanosleep / usleep / clock_nanosleep Hook 行为验证 ===\n");

    ScenarioNanosleepParks();
    ScenarioUsleepParks();
    ScenarioClockRelativeParks();
    ScenarioClockAbsolutePast();
    ScenarioClockAbsoluteFuture();
    ScenarioPlainThread();
    ScenarioDisabledStillWorks();

    std::fprintf(stderr, "\n[hook-sleep] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
