/**
 * @file parking_condition_benchmark.cpp
 * @brief ParkingCondition 的原语成本基准（build-bench / Release）。
 *
 * 两个场景，都是**普通线程**路径（不绑定 Scheduler/Task），即当前走
 * std::condition_variable_any 的那条路 —— 也正是 futex 重构的目标。
 *
 *   A. notify 空转：无等待者时调用 notify_one/notify_all 的成本
 *   C. 2/4 消费者争用：N 个线程等同一个条件，每轮唤醒一个
 *
 * 用法：./go2cpp_parking_condition_benchmark [rounds]
 */

#include "go2cpp/core/parking_condition.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::core::ParkingCondition;

/// A：无等待者时 notify 的成本。
double BenchNotifyOnly(int rounds) {
    ParkingCondition cond;
    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        if ((i & 1) == 0) {
            cond.notify_one();
        } else {
            cond.notify_all();
        }
    }
    const auto t1 = Clock::now();
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
           rounds;
}

/// C：N 个消费者争用，每轮唤醒一个。
double BenchContended(int rounds, int consumers) {
    ParkingCondition cond;
    std::mutex m;
    int pending = 0;
    std::atomic<bool> stop{false};
    std::atomic<int> consumed{0};

    std::vector<std::thread> ts;
    ts.reserve(static_cast<std::size_t>(consumers));
    for (int i = 0; i < consumers; ++i) {
        ts.emplace_back([&] {
            std::unique_lock<std::mutex> lk(m);
            while (!stop.load(std::memory_order_acquire)) {
                (void)cond.wait(lk, [&] { return pending > 0 || stop.load(std::memory_order_acquire); });
                if (pending > 0) {
                    --pending;
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        {
            std::lock_guard<std::mutex> lk(m);
            ++pending;
        }
        cond.notify_one();
    }
    // 等全部消费完
    while (consumed.load(std::memory_order_acquire) < rounds) {
        std::this_thread::yield();
    }
    const auto t1 = Clock::now();

    {
        std::lock_guard<std::mutex> lk(m);
        stop.store(true, std::memory_order_release);
    }
    cond.notify_all();
    for (auto& th : ts) {
        th.join();
    }

    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
           rounds;
}

}  // namespace

int main(int argc, char** argv) {
    int rounds = 20000;
    if (argc > 1) {
        const int v = std::atoi(argv[1]);
        if (v > 0) rounds = v;
    }
    std::printf("=== ParkingCondition 原语基准（build-bench / Release）===\n");
    std::printf("rounds=%d\n\n", rounds);

    std::printf("A notify 空转（无等待者）: %.2f ns/次\n", BenchNotifyOnly(rounds));
    std::fflush(stdout);
    std::printf("C 争用（2 消费者）      : %.2f ns/次\n", BenchContended(rounds / 4, 2));
    std::printf("C 争用（4 消费者）      : %.2f ns/次\n", BenchContended(rounds / 4, 4));
    std::fflush(stdout);
    std::printf("\n说明：C 走的是普通线程路径（std::condition_variable_any），\n");
    std::printf("      即 futex 重构的目标路径。数字来自 build-bench，勿跨日期比较。\n");
    return 0;
}
