/**
 * @file test_parking_condition_stress.cpp
 * @brief ParkingCondition 专项并发压力测试 —— futex 无状态锁重构的护栏。
 *
 * 历史背景：本会话曾把 ParkingCondition 的普通线程等待从
 * std::condition_variable_any 改为 futex 代际字，编译通过但 io_hook_demo /
 * io_hook_full 崩溃（Subprocess aborted），已回退。
 *
 * 本测试专盯 futex 最容易丢唤醒的模式：
 *   1. notify 早于 wait：唤醒必须"记住"，后续 wait 不能永久阻塞
 *   2. notify_one 只唤醒一个等待者（多个等待者时计数准确）
 *   3. notify_all 唤醒全部等待者
 *   4. 超时精度：wait_until 在无 notify 时按时返回 false
 *   5. 超时与 notify 竞态：两者同时发生时至少一方生效，不能挂死
 *   6. 反复 wait/notify 重入：同一 condition 上多轮等待不残留状态
 */

#include "go2cpp/core/parking_condition.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::core::ParkingCondition;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

/// 场景 1：notify 早于 wait（唤醒必须被记住）。
void scenario_notify_before_wait() {
    std::printf("[pc][场景1] notify 早于 wait\n");
    ParkingCondition cond;
    std::mutex m;
    bool flag = false;
    {
        std::lock_guard<std::mutex> lk(m);
        flag = true;
    }
    cond.notify_one();            // 此刻还没有等待者
    std::this_thread::yield();

    std::unique_lock<std::mutex> lk(m);
    // 谓词已为真，wait 必须立即返回而不是永久阻塞。
    const auto t0 = Clock::now();
    const bool ok = cond.wait_for(lk, std::chrono::milliseconds(500), [&] { return flag; });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    check(ok, "谓词已真时 wait_for 返回 true");
    check(ms < 100, "且几乎立即返回（未被唤醒丢失拖到超时）");
}

/// 场景 2：notify_one 只唤醒一个。
void scenario_notify_one_wakes_one() {
    std::printf("[pc][场景2] notify_one 只唤醒一个\n");
    ParkingCondition cond;
    std::mutex m;
    int pending = 0;
    std::atomic<int> woken{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i) {
        ts.emplace_back([&] {
            std::unique_lock<std::mutex> lk(m);
            (void)cond.wait(lk, [&] { return pending > 0 || stop.load(std::memory_order_acquire); });
            if (pending > 0) {
                --pending;
                woken.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // 等它们真的进入等待
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lk(m);
        pending = 1;
    }
    cond.notify_one();
    // 给足时间让不应被唤醒的线程保持睡眠
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int got = woken.load(std::memory_order_acquire);
    check(got == 1, "只有 1 个等待者被唤醒");

    {
        std::lock_guard<std::mutex> lk(m);
        stop.store(true, std::memory_order_release);
        pending = 4;
    }
    cond.notify_all();
    for (auto& t : ts) {
        t.join();
    }
}

/// 场景 3：notify_all 唤醒全部。
void scenario_notify_all() {
    std::printf("[pc][场景3] notify_all 唤醒全部\n");
    ParkingCondition cond;
    std::mutex m;
    bool go = false;
    std::atomic<int> woken{0};

    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([&] {
            std::unique_lock<std::mutex> lk(m);
            (void)cond.wait(lk, [&] { return go; });
            woken.fetch_add(1, std::memory_order_relaxed);
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lk(m);
        go = true;
    }
    cond.notify_all();
    for (auto& t : ts) {
        t.join();
    }
    check(woken.load(std::memory_order_acquire) == 8, "8 个等待者全部被唤醒");
}

/// 场景 4：超时精度。
void scenario_timeout() {
    std::printf("[pc][场景4] 超时精度\n");
    ParkingCondition cond;
    std::mutex m;
    std::unique_lock<std::mutex> lk(m);
    const auto deadline = Clock::now() + std::chrono::milliseconds(80);
    const auto t0 = Clock::now();
    const bool ok = cond.wait_until(lk, deadline, [] { return false; });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    check(!ok, "无 notify 时 wait_until 返回 false");
    check(ms >= 60 && ms < 400, "超时时长大致符合 deadline（60~400ms）");
}

/// 场景 5：超时与 notify 竞态（不能挂死）。
void scenario_timeout_vs_notify() {
    std::printf("[pc][场景5] 超时与 notify 竞态\n");
    for (int round = 0; round < 200; ++round) {
        ParkingCondition cond;
        std::mutex m;
        bool flag = false;
        std::thread notifier([&] {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            {
                std::lock_guard<std::mutex> lk(m);
                flag = true;
            }
            cond.notify_one();
        });
        {
            std::unique_lock<std::mutex> lk(m);
            const auto deadline = Clock::now() + std::chrono::milliseconds(20);
            (void)cond.wait_until(lk, deadline, [&] { return flag; });
        }
        notifier.join();
        // 无论谁先赢，最终 flag 必须为真（notifier 一定会置位）
        std::lock_guard<std::mutex> lk(m);
        if (!flag) {
            std::printf("  [FAIL] round %d: flag 未置位\n", round);
            ++g_failures;
            return;
        }
    }
    std::printf("  200 轮竞态均正常结束\n");
}

/// 场景 6：反复 wait/notify 重入。
void scenario_repeated() {
    std::printf("[pc][场景6] 反复 wait/notify 重入\n");
    ParkingCondition cond;
    std::mutex m;
    int turn = 0;
    constexpr int kRounds = 1000;
    std::atomic<int> consumed{0};

    std::thread consumer([&] {
        std::unique_lock<std::mutex> lk(m);
        for (int i = 0; i < kRounds; ++i) {
            (void)cond.wait(lk, [&] { return turn == 1; });
            turn = 0;
            consumed.fetch_add(1, std::memory_order_release);
        }
    });
    for (int i = 0; i < kRounds; ++i) {
        {
            std::lock_guard<std::mutex> lk(m);
            turn = 1;
        }
        cond.notify_one();
        // 等消费者处理完这一轮
        while (true) {
            std::lock_guard<std::mutex> lk(m);
            if (turn == 0) {
                break;
            }
            std::this_thread::yield();
        }
    }
    consumer.join();
    check(consumed.load(std::memory_order_acquire) == kRounds,
          "1000 轮重入完成（消费者每轮都被唤醒）");
}

}  // namespace

void run_parking_condition_stress_tests() {
    g_failures = 0;
    std::printf("=== ParkingCondition 并发压力测试 ===\n\n");

    scenario_notify_before_wait();
    scenario_notify_one_wakes_one();
    scenario_notify_all();
    scenario_timeout();
    scenario_timeout_vs_notify();
    scenario_repeated();

    std::printf("\n[pc] failures=%d\n", g_failures);
    if (g_failures != 0) {
        std::printf("[pc] FAILED\n");
    }
}

// 2026-10-21 测试固化：本文件原先只有 run_parking_condition_stress_tests()，
// 没有任何 target 引用（也从未被别的测试 include），等于 231 行从未编译。
// 这里补一个最小 main 让它能作为独立 ctest 运行；断言逻辑一行未改。
int main() {
    run_parking_condition_stress_tests();
    return g_failures == 0 ? 0 : 1;
}
