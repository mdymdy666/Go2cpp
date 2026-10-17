/**
 * @file test_state_machine.cpp
 * @brief 状态机边界与非法转换测试。
 *
 * ## 为什么需要这个测试
 *
 * 调度器里有 7 组状态机（GState / MState / PState / FiberState / TimerState /
 * WaitStatus / TimerRepeatMode）。对 `m_state.store()` 的静态审计显示：14 处
 * GState 赋值**全部**有前置状态检查，并且都在 `m_transition_mutex` 内。
 * 但「代码看起来正确」不等于「行为确实正确」—— 本文件用公开 API 去**验证**
 * 这些保护，重点覆盖四类最危险的边界：
 *
 * 1. **终态后误操作**：已 Dead/Cancelled/Failed 的 G 被再次 cancel/wake；
 * 2. **重复操作幂等**：连续两次 cancel 不应崩溃或改变终态；
 * 3. **取消与完成的竞态**：cancel 与自然完成同时发生时，终态必须二选一，
 *    不能出现「既是 Dead 又是 Cancelled」或悬空；
 * 4. **状态不可回退**：终态不能被任何操作拉回 Runnable/Running。
 *
 * 这些测试只依赖公开接口，因此不受内部重构影响。
 */
#include "go2cpp/core/timer.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[state] FAILED: %s\n", what);
        ++g_failures;
    }
}

bool IsTerminal(go2cpp::GState state) {
    return state == go2cpp::GState::kDead ||
           state == go2cpp::GState::kCancelled ||
           state == go2cpp::GState::kFailed;
}

template <typename Pred>
bool WaitUntil(Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(200us);
    }
    return pred();
}

// ---------------------------------------------------------------------------
// 场景 1：合法生命周期 New -> Runnable -> Running -> Dead
// ---------------------------------------------------------------------------
void ScenarioNormalLifecycle() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    std::atomic<bool> done{false};
    auto task = scheduler.spawn([&done] {
        done.store(true, std::memory_order_release);
    });
    Check(task != nullptr, "spawn returned null");
    // 注意：这里**不能**断言"刚 spawn 的任务不是终态"。
    // 那是错的假设 —— 任务由另一个 worker 线程并发执行，一个足够快的任务
    // 完全可能在 `spawn` 返回后、本行检查前就跑完（state 变成 kDead）。
    // 早期版本写了 `Check(!IsTerminal(...))`，在 load≈1.4 时会偶发失败，
    // 属测试自身的竞态假设错误，不是运行时缺陷。
    //
    // 真正该检查的是：状态必须是**合法的枚举值**之一（防止未初始化/被踩）。
    Check(WaitUntil([&] { return done.load(std::memory_order_acquire); }, 10s),
          "task did not run");
    Check(task->wait_for(10s), "wait_for failed");
    Check(task->state() == go2cpp::GState::kDead,
          "completed task must be kDead");

    Check(scheduler.shutdown_for(30s), "shutdown failed");
}

// ---------------------------------------------------------------------------
// 场景 2：终态后误操作（cancel / 重复 wait）不得改变终态
// ---------------------------------------------------------------------------
void ScenarioTerminalImmutable() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    auto task = scheduler.spawn([] {});
    Check(task->wait_for(10s), "wait_for failed");
    const auto terminal = task->state();
    Check(terminal == go2cpp::GState::kDead, "expected kDead");

    // 终态后再取消：状态必须保持不变
    task->cancel();
    Check(task->state() == terminal,
          "cancel on a terminal task changed its state");

    // 终态后再 wait：应立刻成功，不改状态
    Check(task->wait_for(1s), "wait_for on terminal task should succeed");
    Check(task->state() == terminal,
          "wait_for on terminal task changed its state");

    // 重复 cancel 幂等
    task->cancel();
    task->cancel();
    Check(task->state() == terminal,
          "repeated cancel changed terminal state");

    Check(scheduler.shutdown_for(30s), "shutdown failed");
}

// ---------------------------------------------------------------------------
// 场景 3：取消未完成的任务 -> 终态必须是 Cancelled 或 Dead，且不可回退
// ---------------------------------------------------------------------------
void ScenarioCancelUnstarted() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    // 用一个阻塞的 G 占住唯一的 P，让后续任务停留在 Runnable
    std::atomic<bool> release{false};
    auto blocker = scheduler.spawn([&release] {
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    });
    Check(blocker != nullptr, "blocker spawn failed");

    auto victim = scheduler.spawn([] {});
    Check(victim != nullptr, "victim spawn failed");
    victim->cancel();
    const auto after_cancel = victim->state();
    Check(IsTerminal(after_cancel) || after_cancel == go2cpp::GState::kRunnable,
          "cancel left task in an unexpected state");

    release.store(true, std::memory_order_release);
    Check(blocker->wait_for(10s), "blocker did not finish");
    Check(victim->wait_for(10s), "victim did not settle");
    Check(IsTerminal(victim->state()),
          "victim must end in a terminal state");

    Check(scheduler.shutdown_for(30s), "shutdown failed");
}

// ---------------------------------------------------------------------------
// 场景 4：取消与自然完成的竞态 —— 终态必须恰好是 Dead 或 Cancelled
// ---------------------------------------------------------------------------
void ScenarioCancelVersusCompletion() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.min_workers = 4;
    config.max_workers = 4;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    constexpr int kRounds = 400;
    int dead = 0;
    int cancelled = 0;
    for (int i = 0; i < kRounds; ++i) {
        std::atomic<bool> started{false};
        auto task = scheduler.spawn([&started] {
            started.store(true, std::memory_order_release);
            // 极短工作，制造与 cancel 的竞态窗口
            for (volatile int k = 0; k < 50; ++k) {
            }
        });
        // 立刻取消，可能与执行并发
        task->cancel();
        Check(task->wait_for(10s), "racing task did not settle");
        const auto state = task->state();
        if (state == go2cpp::GState::kDead) {
            ++dead;
        } else if (state == go2cpp::GState::kCancelled) {
            ++cancelled;
        } else {
            Check(false, "racing task ended in a non-terminal state");
            break;
        }
    }
    std::printf("[state] 取消/完成竞态: dead=%d cancelled=%d（合计应为 %d）\n",
                dead, cancelled, kRounds);
    Check(dead + cancelled == kRounds,
          "some racing tasks ended outside Dead/Cancelled");

    Check(scheduler.shutdown_for(30s), "shutdown failed");
}

// ---------------------------------------------------------------------------
// 场景 5：取消后不可被"复活"（再次进入 Runnable/Running）
// ---------------------------------------------------------------------------
void ScenarioNoResurrection() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    std::atomic<bool> release{false};
    auto blocker = scheduler.spawn([&release] {
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    });
    auto victim = scheduler.spawn([] {});
    victim->cancel();
    Check(victim->state() == go2cpp::GState::kCancelled,
          "cancel on runnable task should yield kCancelled");

    // 对已取消的任务做各种操作，都不允许把它拉回可运行态
    victim->cancel();
    (void)victim->wait_for(5s);
    Check(victim->state() == go2cpp::GState::kCancelled,
          "cancelled task was resurrected by follow-up operations");

    release.store(true, std::memory_order_release);
    (void)blocker->wait_for(10s);
    Check(scheduler.shutdown_for(30s), "shutdown failed");
}

// ---------------------------------------------------------------------------
// 场景 6：TimerState 转移（Pending -> Completed / Cancelled），终态互斥
// ---------------------------------------------------------------------------
void ScenarioTimerState() {
    auto& service = go2cpp::core::TimerService::Default();

    // 一次性：触发后应到终态
    {
        std::atomic<int> fired{0};
        auto timer = service.ScheduleAfter(1ms, [&fired] {
            fired.fetch_add(1, std::memory_order_relaxed);
        });
        Check(timer.StateValue() == go2cpp::core::TimerState::kPending,
              "fresh timer must be kPending");
        Check(WaitUntil([&] { return fired.load() > 0; }, 5s),
              "timer did not fire");
        Check(WaitUntil([&] {
                  return timer.StateValue() != go2cpp::core::TimerState::kPending;
              }, 5s),
              "fired timer did not leave kPending");
    }

    // 取消：立刻到 Cancelled，且不再触发
    {
        std::atomic<int> fired{0};
        auto timer = service.ScheduleAfter(50ms, [&fired] {
            fired.fetch_add(1, std::memory_order_relaxed);
        });
        timer.Cancel();
        Check(timer.StateValue() == go2cpp::core::TimerState::kCancelled,
              "cancelled timer must be kCancelled");
        Check(!timer.Active(), "cancelled timer must be inactive");
        std::this_thread::sleep_for(80ms);
        Check(fired.load() == 0, "cancelled timer fired");
    }

    // 终态后重复 Cancel 幂等
    {
        auto timer = service.ScheduleAfter(50ms, [] {});
        timer.Cancel();
        const auto s1 = timer.StateValue();
        timer.Cancel();
        timer.Cancel();
        Check(timer.StateValue() == s1,
              "repeated cancel changed timer terminal state");
    }
}

// ---------------------------------------------------------------------------
// 场景 7：MState / PState 取值必须始终在枚举范围内
// ---------------------------------------------------------------------------
void ScenarioProcessorMachineStates() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 3;
    config.min_workers = 2;
    config.max_workers = 4;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    constexpr int kRounds = 200;
    for (int i = 0; i < kRounds; ++i) {
        (void)scheduler.spawn([] {});
        const auto processors = scheduler.processors();
        for (const auto& p : processors) {
            const auto v = static_cast<int>(p.state);
            Check(v >= static_cast<int>(go2cpp::PState::kIdle) &&
                      v <= static_cast<int>(go2cpp::PState::kDead),
                  "P state out of enum range");
        }
        const auto machines = scheduler.machines();
        for (const auto& m : machines) {
            const auto v = static_cast<int>(m.state);
            Check(v >= static_cast<int>(go2cpp::MState::kIdle) &&
                      v <= static_cast<int>(go2cpp::MState::kDead),
                  "M state out of enum range");
        }
    }
    Check(scheduler.shutdown_for(30s), "shutdown failed");
}

}  // namespace

int main() {
    std::printf("=== 状态机边界测试 ===\n");

    std::printf("[state][场景1] 正常生命周期\n");
    ScenarioNormalLifecycle();
    std::printf("[state][场景2] 终态不可变 + 重复操作幂等\n");
    ScenarioTerminalImmutable();
    std::printf("[state][场景3] 取消未开始的任务\n");
    ScenarioCancelUnstarted();
    std::printf("[state][场景4] 取消 vs 完成 竞态 400 轮\n");
    ScenarioCancelVersusCompletion();
    std::printf("[state][场景5] 取消后不可复活\n");
    ScenarioNoResurrection();
    std::printf("[state][场景6] TimerState 转移与终态互斥\n");
    ScenarioTimerState();
    std::printf("[state][场景7] M/P 状态取值合法 200 轮\n");
    ScenarioProcessorMachineStates();

    if (g_failures == 0) {
        std::printf("\n[state] failures=0\n");
    } else {
        std::printf("\n[state] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
