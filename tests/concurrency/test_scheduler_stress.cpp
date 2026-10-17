/**
 * @file test_scheduler_stress.cpp
 * @brief 调度器并发压力测试 —— 作为修改 worker_loop / admission 事务的护栏。
 *
 * 设计意图：`worker_loop` 的找活路径同时涉及本地队列锁、全局 admission 锁、
 * stopping / draining 状态、emergency 队列与执行认领。历史上曾因「把本地队列
 * 检查移出全局锁但漏掉 stopping/draining 检查」导致 fiber_local_full 与
 * io_hook_demo/io_hook_full 卡死超时。
 *
 * 本测试专门盯住这些不变量，要求在 1~3 秒内暴露问题：
 *   1. 短任务突发 + 多 worker：任务全部完成，runnable 归零
 *   2. 频繁 spawn/shutdown 循环：每轮都能干净退出，不残留 worker
 *   3. park/resume 混合：不丢唤醒
 *   4. 长短任务混合：短任务不被饿死
 *   5. shutdown 期间继续提交：必须被拒绝或安全取消，不能挂起
 *   6. 多 worker 下 runnable 计数归零
 */

#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using namespace go2cpp;
using Clock = std::chrono::steady_clock;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

/// 轮询等待条件成立，最多 timeout；返回是否成立。
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::yield();
    }
    return pred();
}

/// 场景 1：短任务突发 + 多 worker。
void scenario_short_burst(std::size_t workers) {
    std::printf("[stress][场景1] 短任务突发 workers=%zu\n", workers);
    SchedulerConfig config;
    config.processor_count = workers;
    config.max_workers = workers;
    config.min_workers = workers;
    config.local_queue_limit = 256;
    Scheduler scheduler(config);
    scheduler.start();

    constexpr int kTasks = 20000;
    std::atomic<int> done{0};
    for (int i = 0; i < kTasks; ++i) {
        scheduler.spawn([&done] {
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    const bool finished = wait_until(
        [&] { return done.load(std::memory_order_acquire) == kTasks; },
        std::chrono::milliseconds(3000));
    check(finished, "短任务全部完成");
    check(scheduler.runnable_count() == 0, "runnable 归零");
    scheduler.shutdown();
}

/// 场景 2：频繁 spawn/shutdown 循环。
void scenario_spawn_shutdown_loop() {
    std::printf("[stress][场景2] spawn/shutdown 循环 10 轮\n");
    for (int round = 0; round < 10; ++round) {
        SchedulerConfig config;
        config.processor_count = 3;
        config.max_workers = 3;
        config.min_workers = 3;
        Scheduler scheduler(config);
        scheduler.start();
        std::atomic<int> done{0};
        for (int i = 0; i < 500; ++i) {
            scheduler.spawn([&done] { done.fetch_add(1, std::memory_order_relaxed); });
        }
        scheduler.shutdown();
        check(scheduler.worker_count() == 0, "worker 全部退出");
        check(!scheduler.is_running(), "调度器已停止");
        // shutdown 之后提交必须安全失败，不能挂起。
        auto late = scheduler.spawn([] {});
        check(late != nullptr, "shutdown 后 spawn 返回非空");
        if (late) {
            check(late->state() == GState::kCancelled, "shutdown 后任务被取消");
        }
    }
}

/// 场景 3：park/resume 混合，验证不丢唤醒。
void scenario_park_resume() {
    std::printf("[stress][场景3] park/resume 混合\n");
    SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 4;
    config.min_workers = 4;
    Scheduler scheduler(config);
    scheduler.start();

    constexpr int kTasks = 2000;
    std::atomic<int> done{0};
    for (int i = 0; i < kTasks; ++i) {
        scheduler.spawn([&done, &scheduler] {
            // 主动让出若干次，逼出 resume 路径。
            for (int k = 0; k < 3; ++k) {
                (void)scheduler.yield_current();
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    const bool finished = wait_until(
        [&] { return done.load(std::memory_order_acquire) == kTasks; },
        std::chrono::milliseconds(3000));
    check(finished, "yield 混合任务全部完成");
    scheduler.shutdown();
}

/// 场景 4：长短任务混合，短任务不被饿死。
void scenario_mixed_lengths() {
    std::printf("[stress][场景4] 长短任务混合\n");
    SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 4;
    config.min_workers = 4;
    Scheduler scheduler(config);
    scheduler.start();

    std::atomic<int> longs{0};
    std::atomic<int> shorts{0};
    for (int i = 0; i < 8; ++i) {
        scheduler.spawn([&longs] {
            const auto deadline = Clock::now() + std::chrono::milliseconds(20);
            while (Clock::now() < deadline) {
                std::this_thread::yield();
            }
            longs.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (int i = 0; i < 5000; ++i) {
        scheduler.spawn([&shorts] { shorts.fetch_add(1, std::memory_order_relaxed); });
    }
    const bool finished = wait_until(
        [&] {
            return shorts.load(std::memory_order_acquire) == 5000 &&
                   longs.load(std::memory_order_acquire) == 8;
        },
        std::chrono::milliseconds(3000));
    check(finished, "长短任务都完成（短任务未被饿死）");
    scheduler.shutdown();
}

/// 场景 5：shutdown 期间继续提交 —— 复现历史上 stopping 检查缺失的问题。
void scenario_submit_during_shutdown() {
    std::printf("[stress][场景5] shutdown 期间继续提交\n");
    SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 4;
    config.min_workers = 4;
    Scheduler scheduler(config);
    scheduler.start();

    std::atomic<bool> stop{false};
    std::atomic<int> accepted{0};
    std::thread submitter([&] {
        while (!stop.load(std::memory_order_acquire)) {
            if (scheduler.spawn([] {})) {
                accepted.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // 让它提交一会儿，再在提交进行中 shutdown。
    wait_until([&] { return accepted.load() > 100; }, std::chrono::milliseconds(500));
    const auto t0 = Clock::now();
    scheduler.shutdown();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    stop.store(true, std::memory_order_release);
    submitter.join();

    check(elapsed < std::chrono::milliseconds(2000), "shutdown 在 2s 内完成（未挂起）");
    check(scheduler.worker_count() == 0, "shutdown 后 worker 归零");
    std::printf("  shutdown 耗时 %lld ms，期间接受 %d 次提交\n",
                static_cast<long long>(elapsed.count()),
                accepted.load(std::memory_order_relaxed));
}

/// 场景 6：多 worker 下 runnable 计数归零。
void scenario_runnable_accounting(std::size_t workers) {
    std::printf("[stress][场景6] runnable 计数 workers=%zu\n", workers);
    SchedulerConfig config;
    config.processor_count = workers;
    config.max_workers = workers;
    config.min_workers = workers;
    Scheduler scheduler(config);
    scheduler.start();

    std::atomic<int> done{0};
    for (int i = 0; i < 5000; ++i) {
        scheduler.spawn([&done] { done.fetch_add(1, std::memory_order_relaxed); });
    }
    (void)wait_until([&] { return done.load(std::memory_order_acquire) == 5000; },
                     std::chrono::milliseconds(3000));
    check(done.load() == 5000, "任务全部完成");
    // 完成之后 runnable 必须严格归零：既不能残留虚增（丢任务），也不能虚减。
    check(scheduler.runnable_count() == 0, "runnable 严格归零（无残留计数）");
    scheduler.shutdown();
}

}  // namespace

void run_scheduler_stress_tests() {
    g_failures = 0;
    std::printf("=== 调度器并发压力测试 ===\n\n");

    for (std::size_t w : {2U, 3U, 4U}) {
        scenario_short_burst(w);
    }
    scenario_spawn_shutdown_loop();
    scenario_park_resume();
    scenario_mixed_lengths();
    scenario_submit_during_shutdown();
    for (std::size_t w : {3U, 4U}) {
        scenario_runnable_accounting(w);
    }

    std::printf("\n[stress] failures=%d\n", g_failures);
    if (g_failures != 0) {
        std::printf("[stress] FAILED\n");
    }
}
