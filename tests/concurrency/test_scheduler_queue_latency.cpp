/**
 * @file test_scheduler_queue_latency.cpp
 * @brief 量化「新提交的任务要等多久才开始执行」—— 用来判断是否需要 `runnext`。
 *
 * ## 背景
 *
 * Go 的每个 P 有一个 `runnext` 槽位：刚被 spawn 的 G 会插队到该槽，优先于
 * 队列中已有的 G 执行。这让"提交后立即需要结果"的短任务不被前面的积压拖住。
 *
 * 本项目**没有** `runnext`（`grep runnext` 零命中），新任务一律 `push_back`
 * 到 P 队列尾部。本测试用来量化这个差异的实际影响：
 *
 * - 先在 P 队列里塞满 N 个"慢任务"；
 * - 再 spawn 一个"标记任务"，测量它从提交到开始执行的延迟；
 * - 如果延迟 ≈ N 个慢任务的执行时间，说明新任务被完全排队；
 * - 如果延迟很小，说明现有机制（本地队列快速路径/窃取）已经足够，
 *   `runnext` 的收益有限，不值得为它改动调度器核心。
 *
 * ## 这是纯观测测试
 *
 * 不修改调度器任何行为，只测量。因此它可以在任何版本上跑，作为后续优化的
 * 基线。
 */
#include "go2cpp/scheduler.hpp"

#include <algorithm>
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
        std::fprintf(stderr, "[latency] FAILED: %s\n", what);
        ++g_failures;
    }
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

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = p * static_cast<double>(v.size() - 1);
    const auto idx = static_cast<std::size_t>(pos);
    const double frac = pos - static_cast<double>(idx);
    if (idx + 1 >= v.size()) return v.back();
    return v[idx] * (1.0 - frac) + v[idx + 1] * frac;
}

// ---------------------------------------------------------------------------
// 场景 1：单 P，队列积压时新任务的排队延迟
// ---------------------------------------------------------------------------
void ScenarioBacklogLatency() {
    constexpr int kBacklog = 2000;
    constexpr int kSamples = 20;

    go2cpp::SchedulerConfig config;
    config.processor_count = 1;   // 单 P：所有任务共享一个队列，最能暴露排队
    config.min_workers = 1;
    config.max_workers = 1;
    config.collect_metrics = false;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    // 让主线程先稳定下来，避免把启动开销算进延迟
    std::this_thread::sleep_for(50ms);

    std::vector<double> latencies_us;
    latencies_us.reserve(kSamples);

    for (int sample = 0; sample < kSamples; ++sample) {
        // 1) 先把队列塞满慢任务
        std::atomic<int> backlog_done{0};
        for (int i = 0; i < kBacklog; ++i) {
            (void)scheduler.spawn([&backlog_done] {
                // 极短但非空的工作，保证它们真的占住队列
                for (volatile int k = 0; k < 200; ++k) {
                }
                backlog_done.fetch_add(1, std::memory_order_relaxed);
            });
        }

        // 2) 在积压之上再提交一个标记任务，测它多久开始执行
        const auto submitted = Clock::now();
        std::atomic<bool> started{false};
        double latency_us = -1.0;
        (void)scheduler.spawn([&, submitted] {
            const auto ran = Clock::now();
            latency_us = std::chrono::duration<double, std::micro>(ran - submitted).count();
            started.store(true, std::memory_order_release);
        });

        Check(WaitUntil([&] { return started.load(std::memory_order_acquire); }, 60s),
              "marker task never started");
        Check(WaitUntil([&] { return backlog_done.load() >= kBacklog; }, 60s),
              "backlog did not drain");
        if (latency_us >= 0.0) {
            latencies_us.push_back(latency_us);
        }
    }

    (void)scheduler.shutdown_for(60s);

    if (latencies_us.empty()) {
        Check(false, "no latency samples collected");
        return;
    }
    std::printf(
        "[latency] 单 P，积压 %d 任务时新任务排队延迟: p50=%.1fus p90=%.1fus "
        "max=%.1fus（%zu 个样本）\n",
        kBacklog, Percentile(latencies_us, 0.50), Percentile(latencies_us, 0.90),
        *std::max_element(latencies_us.begin(), latencies_us.end()),
        latencies_us.size());
}

// ---------------------------------------------------------------------------
// 场景 2：空队列时新任务的启动延迟（基线：不含排队）
// ---------------------------------------------------------------------------
void ScenarioIdleLatency() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    config.collect_metrics = false;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    std::this_thread::sleep_for(50ms);

    std::vector<double> latencies_us;
    constexpr int kSamples = 200;
    latencies_us.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const auto submitted = Clock::now();
        std::atomic<bool> done{false};
        double latency_us = -1.0;
        (void)scheduler.spawn([&, submitted] {
            latency_us = std::chrono::duration<double, std::micro>(
                             Clock::now() - submitted).count();
            done.store(true, std::memory_order_release);
        });
        Check(WaitUntil([&] { return done.load(std::memory_order_acquire); }, 10s),
              "idle task did not start");
        if (latency_us >= 0.0) {
            latencies_us.push_back(latency_us);
        }
    }
    (void)scheduler.shutdown_for(30s);

    if (latencies_us.empty()) {
        Check(false, "no idle samples");
        return;
    }
    std::printf(
        "[latency] 空队列时新任务启动延迟: p50=%.1fus p90=%.1fus max=%.1fus（%zu 个样本）\n",
        Percentile(latencies_us, 0.50), Percentile(latencies_us, 0.90),
        *std::max_element(latencies_us.begin(), latencies_us.end()),
        latencies_us.size());
}

}  // namespace

int main() {
    std::printf("=== 调度队列延迟测试（runnext 必要性评估）===\n");
    std::printf("[latency][场景1] 空队列启动延迟（基线）\n");
    ScenarioIdleLatency();
    std::printf("[latency][场景2] 单 P 积压下的排队延迟\n");
    ScenarioBacklogLatency();

    if (g_failures == 0) {
        std::printf("\n[latency] failures=0\n");
    } else {
        std::printf("\n[latency] failures=%d\n", g_failures);
    }
    std::printf("\n[latency] 判读: 若场景2 的 p50 远大于场景1，说明新任务确实被\n"
                "[latency] 队列积压拖住，`runnext` 类的插队机制有价值；若两者接近，\n"
                "[latency] 说明现有机制已足够，不值得为它改动调度器核心。\n");
    return g_failures == 0 ? 0 : 1;
}
