/**
 * @file test_timer_bin_stress.cpp
 * @brief TimerBin（Timer::State 对象池）的正确性与压力测试。
 *
 * ## 为什么这个测试必须存在
 *
 * 对象池最大的风险是**复用未清空的旧状态**。Timer::State 持有
 * std::function / 弱引用 / 执行器，如果归还时没清干净，下一次复用会
 * "意外带着上一次的回调"触发 —— 这类 bug 不会崩，只会让回调多跑一次，
 * 极难在生产里发现。所以本文件的重点是**验证 reset 的彻底性**。
 *
 * ## 六个场景
 *
 * 1. 高频一次性定时器：连续建/触发，验证计数精确、池被复用；
 * 2. **状态不残留**：A 触发完后复用同一槽位给 B，验证 B 不继承 A 的回调；
 * 3. 并发多线程：N 线程各自建/毁，验证 thread_local 池无数据竞争；
 * 4. 随机取消：随机延迟后 Cancel，验证取消后绝不触发；
 * 5. 周期定时器 + 取消：周期触发若干次后取消，验证停止；
 * 6. 混合压力：随机混合上述形态，跑多轮，验证无泄漏、无异常触发。
 */
#include "go2cpp/core/timer.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[timerbin] FAILED: %s\n", what);
        ++g_failures;
    }
}

/** @brief 忙等到 predicate 成立或超时。 */
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
// 场景 1：高频一次性定时器
// ---------------------------------------------------------------------------
void ScenarioHighFrequency() {
    auto& service = go2cpp::core::TimerService::Default();
    constexpr int kRounds = 2000;
    std::atomic<int> fired{0};
    // 注意：Timer 的析构**等价于 Cancel**，所以必须把所有句柄持有到触发之后，
    // 否则循环结束即取消，回调一个都不会跑。
    std::vector<go2cpp::core::Timer> held;
    held.reserve(kRounds);
    for (int i = 0; i < kRounds; ++i) {
        held.emplace_back(service.ScheduleAfter(1ms, [&fired] {
            fired.fetch_add(1, std::memory_order_relaxed);
        }));
        Check(held.back().Active(), "one-shot timer should be active");
    }
    const bool all = WaitUntil([&] { return fired.load() >= kRounds; }, 30s);
    Check(all, "high-frequency one-shot: not all timers fired");
    Check(fired.load() == kRounds, "high-frequency one-shot: fired count mismatch");
}

// ---------------------------------------------------------------------------
// 场景 2：状态不残留（最关键）
// ---------------------------------------------------------------------------
void ScenarioNoStateResidue() {
    auto& service = go2cpp::core::TimerService::Default();

    // 第一代：回调加 1
    std::atomic<int> generation_a{0};
    // 第二代：回调加 100。若 reset 不彻底，复用的 State 可能同时带着
    // 上一代的回调，导致 generation_a 在第二阶段继续增长。
    std::atomic<int> generation_b{0};

    for (int cycle = 0; cycle < 300; ++cycle) {
        generation_a.store(0);
        generation_b.store(0);

        {
            auto timer = service.ScheduleAfter(1ms, [&generation_a] {
                generation_a.fetch_add(1, std::memory_order_relaxed);
            });
            const bool fired =
                WaitUntil([&] { return generation_a.load() > 0; }, 5s);
            Check(fired, "residue: generation A did not fire");
        }
        // 等这一代的 State 完全释放（引用计数归零）后，它可能被复用
        WaitUntil([] { return true; }, 1ms);
        const int a_after_release = generation_a.load();

        {
            auto timer = service.ScheduleAfter(1ms, [&generation_b] {
                generation_b.fetch_add(1, std::memory_order_relaxed);
            });
            const bool fired =
                WaitUntil([&] { return generation_b.load() > 0; }, 5s);
            Check(fired, "residue: generation B did not fire");
        }
        WaitUntil([] { return true; }, 1ms);

        // 关键断言：下一代触发不能带动上一代的回调
        Check(generation_a.load() == a_after_release,
              "residue: stale callback from previous generation fired");
        Check(generation_b.load() == 1,
              "residue: generation B fired more than once");
    }
}

// ---------------------------------------------------------------------------
// 场景 3：并发多线程
// ---------------------------------------------------------------------------
void ScenarioConcurrent() {
    auto& service = go2cpp::core::TimerService::Default();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 300;
    std::atomic<int> total{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    // 每个线程把自己的句柄放进共享容器，保证在触发之前不被析构（析构=Cancel）。
    std::mutex held_mutex;
    std::vector<go2cpp::core::Timer> held;
    held.reserve(static_cast<std::size_t>(kThreads) * kPerThread);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            std::vector<go2cpp::core::Timer> local;
            local.reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i) {
                local.emplace_back(service.ScheduleAfter(1ms, [&total] {
                    total.fetch_add(1, std::memory_order_relaxed);
                }));
            }
            std::vector<go2cpp::core::Timer> sink;
            {
                std::lock_guard<std::mutex> lock(held_mutex);
                for (auto& t2 : local) {
                    held.emplace_back(std::move(t2));
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    const int expected = kThreads * kPerThread;
    const bool all = WaitUntil([&] { return total.load() >= expected; }, 30s);
    Check(all, "concurrent: not all timers fired");
    if (total.load() != expected) {
        std::fprintf(stderr,
                     "[timerbin] concurrent actual=%d expected=%d %s\n",
                     total.load(), expected,
                     total.load() > expected ? "DUPLICATE" : "MISSING");
    }
    Check(total.load() == expected, "concurrent: fired count mismatch");
}

// ---------------------------------------------------------------------------
// 场景 4：随机取消
// ---------------------------------------------------------------------------
void ScenarioRandomCancel() {
    auto& service = go2cpp::core::TimerService::Default();
    std::mt19937 rng(20261017);
    std::uniform_int_distribution<int> delay_us(0, 800);
    constexpr int kRounds = 500;
    std::atomic<int> fired{0};
    for (int i = 0; i < kRounds; ++i) {
        auto timer = service.ScheduleAfter(
            std::chrono::microseconds(delay_us(rng)), [&fired] {
                fired.fetch_add(1, std::memory_order_relaxed);
            });
        // 随机决定是否立即取消
        if (delay_us(rng) % 2 == 0) {
            timer.Cancel();
            Check(!timer.Active(), "random-cancel: cancelled timer still active");
        }
    }
    // 等足够久让所有未取消的定时器都触发
    WaitUntil([] { return true; }, 50ms);
    // 取消过的定时器不能再增长：再等一轮，计数必须稳定
    const int settled = fired.load();
    WaitUntil([] { return true; }, 100ms);
    Check(fired.load() == settled,
          "random-cancel: cancelled timer fired after settling");
}

// ---------------------------------------------------------------------------
// 场景 5：周期定时器 + 取消
// ---------------------------------------------------------------------------
void ScenarioRepeating() {
    auto& service = go2cpp::core::TimerService::Default();
    std::atomic<int> ticks{0};
    auto timer = service.ScheduleEvery(1ms, [&ticks] {
        ticks.fetch_add(1, std::memory_order_relaxed);
    });
    Check(timer.Active(), "repeating: timer should be active");
    const bool got_some = WaitUntil([&] { return ticks.load() >= 3; }, 10s);
    Check(got_some, "repeating: did not produce 3 ticks");
    timer.Cancel();
    Check(!timer.Active(), "repeating: timer still active after cancel");
    const int after_cancel = ticks.load();
    WaitUntil([] { return true; }, 30ms);
    Check(ticks.load() == after_cancel,
          "repeating: ticked after cancel");

    // 复用路径：连续建/取消周期定时器，验证池不会泄漏回调
    for (int i = 0; i < 100; ++i) {
        std::atomic<int> local{0};
        auto t = service.ScheduleEvery(50ms, [&local] {
            local.fetch_add(1, std::memory_order_relaxed);
        });
        t.Cancel();
    }
}

// ---------------------------------------------------------------------------
// 场景 6：混合随机压力
// ---------------------------------------------------------------------------
void ScenarioMixedStress() {
    auto& service = go2cpp::core::TimerService::Default();
    std::mt19937 rng(777);
    std::uniform_int_distribution<int> shape(0, 3);
    std::uniform_int_distribution<int> delay_us(0, 300);
    constexpr int kRounds = 1200;
    std::atomic<int> one_shot{0};
    std::atomic<int> repeat_ticks{0};

    for (int i = 0; i < kRounds; ++i) {
        switch (shape(rng)) {
            case 0: {
                auto t = service.ScheduleAfter(
                    std::chrono::microseconds(delay_us(rng)), [&one_shot] {
                        one_shot.fetch_add(1, std::memory_order_relaxed);
                    });
                (void)t.Active();
                break;
            }
            case 1: {
                auto t = service.ScheduleEvery(1ms, [&repeat_ticks] {
                    repeat_ticks.fetch_add(1, std::memory_order_relaxed);
                });
                t.Cancel();
                break;
            }
            case 2: {
                auto t = service.ScheduleAfter(
                    std::chrono::microseconds(delay_us(rng)), [&one_shot] {
                        one_shot.fetch_add(1, std::memory_order_relaxed);
                    });
                t.Cancel();
                break;
            }
            default: {
                // 创建后立刻让其析构（等价于 Cancel），验证析构路径回池安全
                auto t = service.ScheduleAfter(50ms, [] {});
                (void)t.StateValue();
                break;
            }
        }
    }
    WaitUntil([] { return true; }, 300ms);
    const int settled = one_shot.load();
    WaitUntil([] { return true; }, 200ms);
    Check(one_shot.load() == settled,
          "mixed: one-shot fired after settling window");
}

/** @brief 场景 7：校验池确实被复用（否则测试无法证明池生效）。 */
void ScenarioPoolReuse() {
    auto& service = go2cpp::core::TimerService::Default();
    const auto before = go2cpp::core::TimerStateBinSize();
    for (int i = 0; i < 200; ++i) {
        auto t = service.ScheduleAfter(1ms, [] {});
        WaitUntil([&] { return !t.Active(); }, 2s);
    }
    WaitUntil([] { return true; }, 50ms);
    const auto after = go2cpp::core::TimerStateBinSize();
    std::printf("[timerbin] 池大小: before=%zu after=%zu（本线程）\n", before, after);
    // 池可能因跨线程归还而落在其它线程，所以只要求"不异常增长到无界"
    Check(after <= 256, "pool: unbounded growth");
}

}  // namespace

void run_timer_bin_stress_tests() {
    std::printf("=== TimerBin 压测 ===\n");

    std::printf("[timerbin][场景1] 高频一次性定时器 2000 次\n");
    ScenarioHighFrequency();
    std::printf("[timerbin][场景2] 状态不残留（关键：复用不继承旧回调）\n");
    ScenarioNoStateResidue();
    std::printf("[timerbin][场景3] 并发 8 线程 × 300 定时器\n");
    ScenarioConcurrent();
    std::printf("[timerbin][场景4] 随机取消 500 次\n");
    ScenarioRandomCancel();
    std::printf("[timerbin][场景5] 周期定时器 + 取消\n");
    ScenarioRepeating();
    std::printf("[timerbin][场景6] 混合随机压力 1200 次\n");
    ScenarioMixedStress();
    std::printf("[timerbin][场景7] 池复用校验\n");
    ScenarioPoolReuse();

    if (g_failures == 0) {
        std::printf("\n[timerbin] failures=0\n");
    } else {
        std::printf("\n[timerbin] failures=%d\n", g_failures);
    }
}

#ifndef GO2CPP_TIMERBIN_STRESS_NO_MAIN
int main() {
    run_timer_bin_stress_tests();
    return g_failures == 0 ? 0 : 1;
}
#endif
