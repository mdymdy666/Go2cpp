/**
 * @file test_sync_cancel_stress.cpp
 * @brief 高并发复现：ConditionVariable 等待被 cancel 后，cleanup 重锁路径丢唤醒。
 *
 * ## 为什么需要这个文件
 *
 * `tests/concurrency/test_sync.cpp` 的 `TestConditionCancellationAlwaysRelocks`
 * 在**单进程**下稳定通过；但把同一个模式以「多进程并发」压到 12 份时，实测
 * 约 1/12~1/36 的运行会出现：
 *
 * ```
 * test_sync.cpp:734 check failed: waiter->wait_for(2s)
 * test_sync.cpp:736 check failed: !wait_result...
 * test_sync.cpp:737 check failed: owns_mutex...
 * ```
 *
 * gdb attach 证明这不是调度延迟：卡死时所有 worker 都空闲、
 * 主线程停在 `Scheduler::shutdown_for()` 无限等待一个「永不进入终态」的 G。
 * 即：**丢失唤醒**（Fiber 永久挂起，没有崩溃、没有超时）。
 *
 * ## 判据
 *
 * 单轮流程与 test_sync 完全一致：
 *   1. waiter G 持锁 → `condition.Wait(mutex)` 发布等待并 park；
 *   2. holder G 等 waiter 就绪后抢锁并持锁自旋；
 *   3. 主线程 `waiter->cancel()`，随机 jitter 后放行 holder；
 *   4. waiter 必须被取消唤醒 → cleanup 重锁 → 以持锁状态返回 false。
 *
 * 任何一步超时（`wait_for(2s)`）即视为丢唤醒，打印 STUCK 并返回非零。
 *
 * ## 状态：**暂未注册进 ctest**
 *
 * 该复现器目前会命中已知缺陷（详见交付报告），注册进 ctest 会让并发 CI 随机
 * 变红；等 `src/concurrency/sync.cpp` 的丢唤醒修复落地后再加
 * `add_executable/add_test`，作为回归测试长期保留。
 *
 * 手动运行（Release 构建）：
 * ```
 * g++ -O2 -std=c++20 -pthread -Iinclude tests/concurrency/test_sync_cancel_stress.cpp \
 *     -Lbuild-quality -Wl,-rpath,$PWD/build-quality \
 *     -lgo2cpp_error -lgo2cpp_control_flow -lgo2cpp_context -lgo2cpp_channel \
 *     -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_sync -lgo2cpp_future \
 *     -lgo2cpp_io -lgo2cpp_network -lgo2cpp_log -lgo2cpp_config -ldl \
 *     -o /tmp/sync_cancel_stress
 * /tmp/sync_cancel_stress 5000
 * # 并发放大：同时跑 12 份同一命令，约 1/12~1/36 概率命中
 * ```
 */
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

using namespace std::chrono_literals;

namespace {

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

int Report(const char* what, int iteration, int code) {
    std::fprintf(stderr, "[sync-cancel-stress] FAILED: %s at iteration=%d\n",
                 what, iteration);
    std::fflush(stderr);
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 5000;

    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.idle_wait = 1ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();
    std::mt19937 random(0xC0A7E11U);

    for (int iteration = 0; iteration < iterations; ++iteration) {
        go2cpp::sync::Mutex mutex;
        go2cpp::sync::ConditionVariable condition;
        std::atomic<bool> waiter_ready{false};
        std::atomic<bool> holder_locked{false};
        std::atomic<bool> release_holder{false};
        std::atomic<bool> wait_result{true};
        std::atomic<bool> owns_mutex{false};

        auto waiter = scheduler.spawn([&] {
            mutex.Lock();
            waiter_ready.store(true, std::memory_order_release);
            // 无 Context：只能靠 Task 取消 + CV 注册表唤醒。
            wait_result.store(condition.Wait(mutex), std::memory_order_release);
            const bool probe = mutex.TryLock();
            owns_mutex.store(!probe, std::memory_order_release);
            if (probe) {
                mutex.Unlock();
            }
            mutex.Unlock();
        });
        auto holder = scheduler.spawn([&] {
            while (!waiter_ready.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            mutex.Lock();
            holder_locked.store(true, std::memory_order_release);
            while (!release_holder.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            mutex.Unlock();
        });

        if (!WaitUntil(
                [&] { return holder_locked.load(std::memory_order_acquire); },
                5s)) {
            return Report("holder never locked", iteration, 2);
        }
        if (!WaitUntil(
                [&] { return waiter->state() == go2cpp::GState::kWaiting; },
                5s)) {
            return Report("waiter never parked", iteration, 2);
        }
        if (!waiter->cancel()) {
            return Report("waiter cancel failed", iteration, 2);
        }
        // 让 cancel 与 holder 解锁/cleanup 重锁的交错随机化。
        const auto jitter = std::chrono::microseconds(random() % 200U);
        std::this_thread::sleep_for(jitter);
        release_holder.store(true, std::memory_order_release);

        if (!waiter->wait_for(2s)) {
            std::fprintf(
                stderr,
                "[sync-cancel-stress] STUCK: waiter->wait_for(2s) false at "
                "iteration=%d\n",
                iteration);
            std::fflush(stderr);
            return 3;
        }
        if (!holder->wait_for(2s)) {
            return Report("holder->wait_for(2s) false", iteration, 4);
        }
        if (wait_result.load(std::memory_order_acquire)) {
            return Report("wait_result still true", iteration, 5);
        }
        if (!owns_mutex.load(std::memory_order_acquire)) {
            return Report("waiter did not own mutex after relock", iteration, 6);
        }
    }

    scheduler.shutdown();
    std::printf("[sync-cancel-stress] OK iterations=%d\n", iterations);
    return 0;
}
