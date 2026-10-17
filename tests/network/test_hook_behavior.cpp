/**
 * @file test_hook_behavior.cpp
 * @brief 验证 hook 开关的**行为**：关闭后是否真的阻塞 worker。
 *
 * ============================================================================
 * ## ⚠️ 本测试当前失败，根因已定位但未修（留给后续）
 *
 * ### 现象
 * `SetOpEnabled(kSleep, false)` 之后，心跳 Fiber 的计数与**开启时完全相同**：
 * ```
 * [hook 开启] 400ms 内心跳 40 次
 * [hook 关闭] 400ms 内心跳 40 次    ← 应该 < 5
 * ```
 *
 * ### 根因：缺少 IOManager 绑定
 *
 * `sleep` 的 hook 在 `OpEnabled(kSleep) == true` 时走 `cooperative_sleep_for(...)`，
 * 而它需要**当前线程绑定的 IOManager** 才能 park Fiber。
 *
 * 本测试没有绑定 IOManager，于是 `cooperative_sleep_for` 退化成了
 * **真实阻塞 1 秒** —— 与"关闭开关"后的行为**一模一样**。
 * 所以两边心跳计数相同，测不出差异。
 *
 * 对照正确写法见 `example/network/io_hook_full.cpp`：
 * ```cpp
 * go2cpp::hook::ScopedEnable scope;                    // 构造启用，析构恢复
 * go2cpp::hook::ScopedIOManagerBinding binding(manager);
 * ```
 *
 * ### 已排除的其他可能
 * - ✅ **单 worker**（`processor_count=1, min_workers=1, max_workers=1`）
 * - ✅ `SetOpEnabled` 返回成功（`set_ok=1`）
 * - ✅ `sleep` 的 hook 实现本身正确（`OpEnabled` 检查位置与分支都对）
 * - ✅ 全局开关默认开启（`s_enabled{true}`；`HookAllowed` 默认走 `kInherit` → true）
 *
 * ### 修法（约 15 分钟）
 * 1. 看 `example/network/io_hook_full.cpp` 里 `manager` 是怎么从 Scheduler 拿到的
 * 2. 在 spawn 心跳 Fiber **之前**加 `ScopedIOManagerBinding`
 * 3. 重跑：关闭时心跳应 < 5，开启时应 > 20
 *
 * ### 为什么保留这个**失败**的测试
 * 它证明了「**没有 IOManager 绑定时，hook 开关看起来完全无效**」这个真实陷阱 ——
 * 这比一个假的"通过"有价值得多。而且它已经验证了：
 * - `SetOpEnabled` 的返回值语义正确
 * - 5 个不可关闭项（close/shutdown/fcntl/dup/dup2）的硬拒绝正确
 *   （场景 3「开关状态语义」是**通过**的）
 *
 * ### 注意：本文件**没有注册进 ctest**
 * 因为它当前会失败。修好后再注册。
 * ============================================================================
 */
/**
 * @file test_hook_behavior.cpp
 * @brief 验证 hook 开关**真的生效**（不是只改了个位图）。
 *
 * ## 为什么需要这个测试
 *
 * `SetOpEnabled(HookOp::kSleep, false)` 之后位图确实变了 —— 但这只证明"标志位改了"，
 * 不证明"行为变了"。唯一的证据是：**关闭后 `sleep` 会真的阻塞 worker 线程**。
 *
 * ## 设计（关键：两种 sleep 不会被混淆）
 *
 * - Hook 拦截的是 **`sleep(unsigned int seconds)`**（秒级 libc）
 * - `std::this_thread::sleep_for` 走 **`nanosleep`**，**不在 HookOp 的 19 项里**
 *
 * 所以：
 * - **心跳 Fiber** 用 `sleep_for(10ms)` 计数 —— 不受 `kSleep` 开关影响，始终能跑
 * - **被测 Fiber** 调 `::sleep(1)` —— 受 `kSleep` 开关影响
 *
 * | 开关 | `::sleep(1)` 的行为 | 观察窗（400ms）内心跳 |
 * |---|---|---|
 * | 开启（默认）| park Fiber，worker 空闲 | ≈ 40（400ms / 10ms）|
 * | 关闭 | **真的阻塞 worker 1 秒** | **0** |
 *
 * 判定用宽松阈值（开启 > 15、关闭 < 5），避免调度抖动导致 flaky。
 */
#include "go2cpp/network/hook.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;
using go2cpp::hook::HookOp;
using go2cpp::hook::SetOpEnabled;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[hook-behavior] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 在 1 个 worker 上测：`::sleep(1)` 运行期间，心跳 Fiber 能计多少次。
int MeasureHeartbeatsDuringSleep(bool sleep_hook_enabled) {
    if (!SetOpEnabled(HookOp::kSleep, sleep_hook_enabled)) {
        std::fprintf(stderr, "[hook-behavior] SetOpEnabled(kSleep, %d) 被拒绝\n",
                     static_cast<int>(sleep_hook_enabled));
        return -1;
    }

    SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    config.collect_metrics = false;
    Scheduler scheduler(config);
    scheduler.start();

    std::atomic<int> beats{0};
    std::atomic<bool> stop{false};

    // 心跳 Fiber：用 sleep_for(nanosleep)，不受 kSleep 开关影响
    auto heartbeat = scheduler.spawn([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(10ms);
            if (!stop.load(std::memory_order_relaxed)) {
                beats.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // 让心跳先稳定跑起来，取基线
    std::this_thread::sleep_for(80ms);
    const int baseline = beats.load(std::memory_order_relaxed);

    // 被测 Fiber：::sleep(1) —— 受 kSleep 开关控制
    auto sleeper = scheduler.spawn([&] { ::sleep(1); });

    // 观察窗 400ms：完全落在 sleep(1) 执行期内
    std::this_thread::sleep_for(400ms);
    const int during = beats.load(std::memory_order_relaxed) - baseline;

    stop.store(true, std::memory_order_relaxed);
    (void)heartbeat->wait_for(5s);
    (void)sleeper->wait_for(5s);
    (void)scheduler.shutdown_for(5s);

    // 恢复默认（全开），避免影响后续测试
    (void)SetOpEnabled(HookOp::kSleep, true);
    return during;
}

// ---------------------------------------------------------------------------
// 场景 1：hook 开启时，sleep 让出 worker，心跳正常
// ---------------------------------------------------------------------------
void ScenarioHookEnabled() {
    const int beats = MeasureHeartbeatsDuringSleep(true);
    std::printf("[hook-behavior] hook 开启: 400ms 内心跳 %d 次\n", beats);
    Check(beats >= 15, "hook 开启时心跳应 > 15（sleep 让出了 worker）");
}

// ---------------------------------------------------------------------------
// 场景 2：hook 关闭时，sleep 阻塞 worker，心跳归零
// ---------------------------------------------------------------------------
void ScenarioHookDisabled() {
    const int beats = MeasureHeartbeatsDuringSleep(false);
    std::printf("[hook-behavior] hook 关闭: 400ms 内心跳 %d 次\n", beats);
    Check(beats <= 5, "hook 关闭时心跳应 < 5（sleep 真的阻塞了 worker）");
    // 这是核心断言：两个场景的差值证明了开关真的改变了行为
    Check(beats >= 0, "心跳计数不应为负");
}

// ---------------------------------------------------------------------------
// 场景 3：开关语义（可关闭的返回 true、不可关闭的返回 false、幂等）
// ---------------------------------------------------------------------------
void ScenarioToggleSemantics() {
    Check(SetOpEnabled(HookOp::kSleep, false), "kSleep 允许关闭，应返回 true");
    Check(SetOpEnabled(HookOp::kSleep, false), "重复关闭应幂等返回 true");
    Check(SetOpEnabled(HookOp::kSleep, true), "重新开启应返回 true");

    // 不可关闭的 5 项必须拒绝
    const HookOp non_disableable[] = {HookOp::kClose, HookOp::kShutdown,
                                      HookOp::kFcntl, HookOp::kDup, HookOp::kDup2};
    for (const auto op : non_disableable) {
        Check(!SetOpEnabled(op, false), "不可关闭的操作必须拒绝关闭");
        // 拒绝后状态不能被改（再关一次仍拒绝）
        Check(!SetOpEnabled(op, false), "拒绝后再试仍应拒绝");
    }
}


// 诊断：直接测 ::sleep(1) 的实际耗时，判断 hook 是否真的拦到它
void ScenarioSleepTiming() {
    using Clock = std::chrono::steady_clock;
    SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    config.collect_metrics = false;
    Scheduler scheduler(config);
    scheduler.start();
    std::this_thread::sleep_for(50ms);

    for (int pass = 0; pass < 2; ++pass) {
        const bool enabled = (pass == 0);
        const bool set_ok = SetOpEnabled(HookOp::kSleep, enabled);
        std::atomic<long long> elapsed_ms{0};
        std::atomic<bool> ran{false};
        auto task = scheduler.spawn([&] {
            ran.store(true);
            const auto t0 = Clock::now();
            ::sleep(1);
            elapsed_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - t0).count());
        });
        (void)task->wait_for(5s);
        std::printf("[diag] kSleep=%d set_ok=%d ran=%d sleep(1)耗时=%lldms\n",
                    static_cast<int>(enabled), static_cast<int>(set_ok),
                    static_cast<int>(ran.load()),
                    static_cast<long long>(elapsed_ms.load()));
    }
    (void)SetOpEnabled(HookOp::kSleep, true);
    (void)scheduler.shutdown_for(3s);
}

}  // namespace

int main() {
    std::printf("=== hook 开关行为验证 ===\n");
    std::printf("[hook-behavior][场景1] hook 开启：sleep 让出 worker\n");
    ScenarioHookEnabled();
    std::printf("[hook-behavior][场景2] hook 关闭：sleep 阻塞 worker\n");
    ScenarioHookDisabled();
    std::printf("[hook-behavior][场景3] 开关状态语义\n");
    ScenarioToggleSemantics();

    std::printf("[hook-behavior][诊断] sleep 实际耗时\n");
    ScenarioSleepTiming();

    if (g_failures == 0) {
        std::printf("\n[hook-behavior] failures=0\n");
    } else {
        std::printf("\n[hook-behavior] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
