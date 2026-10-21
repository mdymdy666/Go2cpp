/**
 * @file test_park_wake_pair.cpp
 * @brief 受管 Fiber 的 park/wake 配对测试（GMP 调度器 + Channel/ParkingCondition）。
 *
 * ## 为什么需要这个文件
 *
 * park/wake 是 Go2Cpp 调度器的地基：一个 Fiber 在 Channel 或
 * `ParkingCondition` 上 park 后，必须由**恰好一次** wake 把它放回运行队列。
 * 这类缺陷的典型表现不是崩溃，而是「测试进程看起来还在跑」——某个 G 永久
 * 挂起，`Scheduler::shutdown()` 永远等不到它进入终态。功能测试通常只验
 * 值对不对，不会检查「每一个 park 是否都有对应的 wake」。
 *
 * 本文件用**配对表**做检查：每个 Fiber 只写自己那一格（woken/timed_out），
 * 全部结束后逐格断言——任何一格既没 woken 也没 timed_out，就说明它既没有
 * 被唤醒也没有超时，即**丢唤醒导致的 stuck**。
 *
 * ## 场景
 *
 * 1. **Channel 全配对**：8 个 Fiber 各自 park 在专属无缓冲 Channel 的
 *    `RecvFor` 上，主线程逐个 `Send` → 8 格全部 woken，收到值等于槽位号；
 * 2. **ParkingCondition 全配对**：8 个 Fiber park 在同一个 PC 上（各自
 *    谓词），主线程逐轮置位 + `notify_all` → 每轮 woken 计数递增，最终 8 格；
 * 3. **Channel 丢唤醒注入**：4 个 Fiber，故意跳过 1 次 Send → 该 Fiber 的
 *    `RecvFor(200ms)` 必须超时返回，配对表出现且仅出现 1 个 timed_out；
 * 4. **PC 丢唤醒注入**：4 个 Fiber，故意不置位其中 1 个谓词 → 该 Fiber 的
 *    `wait_for(200ms)` 必须超时返回 false，其余 3 个正常唤醒；
 * 5. **重复配对**：10 轮 × 4 Fiber 的 Channel 交接，验证唤醒状态不跨轮残留。
 *
 * ## 断言口径
 *
 * - 每个 Fiber 的 `Task::wait_for` 都有 watchdog 预算，stuck 时以 `[watchdog]`
 *   失败而不是让 ctest 挂死；
 * - 「丢唤醒注入」场景的 stuck 必须被**超时检测到**（timed_out=1），而不是
 *   永远 park；
 * - 正常场景要求 `woken == N` 且 `timed_out == 0`。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

using go2cpp::MakeChannel;
using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;
using go2cpp::Task;
using go2cpp::GState;
using go2cpp::core::ParkingCondition;

/// 场景 1/2 的 Fiber 数。
constexpr int kFiberCount = 8;
/// 丢唤醒注入场景的 Fiber 数。
constexpr int kInjectedCount = 4;
/// 等待 park 的超时：超过则测试失败，但不阻塞后续 Send/Notify。
constexpr auto kParkDeadline = 5s;

SchedulerConfig MakeConfig() {
    SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.idle_wait = 1ms;
    return config;
}

/// 等待所有任务进入 `kWaiting`（park）；返回是否在超时前全部 park。
bool WaitAllParked(const std::vector<std::shared_ptr<Task>>& tasks,
                   std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        bool all = true;
        for (const auto& task : tasks) {
            if (task->state() != GState::kWaiting) {
                all = false;
                break;
            }
        }
        if (all) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

/// 等待 ParkingCondition 的受管等待者达到 target。
bool WaitFiberWaiters(ParkingCondition& condition, std::size_t target,
                      std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (condition.DebugSnapshot().fiber_waiters >= target) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

/// 统计配对表中 woken 的格数。
int CountWoken(const std::vector<std::atomic<int>>& table) {
    int count = 0;
    for (const auto& slot : table) {
        count += slot.load(std::memory_order_acquire);
    }
    return count;
}

std::vector<std::atomic<int>> MakeTable(std::size_t size) {
    std::vector<std::atomic<int>> table(size);
    for (auto& slot : table) {
        slot.store(0, std::memory_order_relaxed);
    }
    return table;
}

// ---------------------------------------------------------------------------
// 1. Channel：N 个 Fiber park 在各自 Channel 上，逐个唤醒
// ---------------------------------------------------------------------------
void TestChannelParkWakePairing() {
    go2cpp_tests::announce(
        "park/wake: 8 fibers park on channels, wake all");
    const auto started = Clock::now();

    Scheduler scheduler(MakeConfig());
    scheduler.start();

    std::vector<std::shared_ptr<go2cpp::Channel<int>>> channels;
    std::vector<std::shared_ptr<Task>> tasks;
    auto woken = MakeTable(kFiberCount);
    auto timed_out = MakeTable(kFiberCount);
    auto values = MakeTable(kFiberCount);

    for (int index = 0; index < kFiberCount; ++index) {
        auto channel = MakeChannel<int>(0);
        channels.push_back(channel);
        tasks.push_back(scheduler.spawn([&, channel, index] {
            const auto result = channel->RecvFor(kParkDeadline);
            if (result.Ok()) {
                woken[static_cast<std::size_t>(index)].store(
                    1, std::memory_order_release);
                values[static_cast<std::size_t>(index)].store(
                    result.ValueOrDefault(), std::memory_order_release);
            } else if (result.status == go2cpp::ChannelStatus::kTimedOut) {
                timed_out[static_cast<std::size_t>(index)].store(
                    1, std::memory_order_release);
            }
        }));
    }

    GO2CPP_CHECK(WaitAllParked(tasks, kParkDeadline));
    for (int index = 0; index < kFiberCount; ++index) {
        GO2CPP_CHECK(channels[static_cast<std::size_t>(index)]
                         ->SendFor(index, kParkDeadline)
                         .Ok());
    }
    for (auto& task : tasks) {
        GO2CPP_REQUIRE(task->wait_for(10s));
    }

    GO2CPP_CHECK(CountWoken(woken) == kFiberCount);
    GO2CPP_CHECK(CountWoken(timed_out) == 0);
    for (int index = 0; index < kFiberCount; ++index) {
        GO2CPP_CHECK(values[static_cast<std::size_t>(index)].load(
                         std::memory_order_acquire) == index);
    }
    scheduler.shutdown();
    std::printf("[park] channel pairing: woken=%d timed_out=%d elapsed=%.3fs\n",
                CountWoken(woken), CountWoken(timed_out),
                std::chrono::duration<double>(Clock::now() - started).count());
}

// ---------------------------------------------------------------------------
// 2. ParkingCondition：N 个 Fiber park 在同一个 PC 上，逐轮唤醒
// ---------------------------------------------------------------------------
void TestParkingConditionParkWakePairing() {
    go2cpp_tests::announce(
        "park/wake: 8 fibers park on ParkingCondition, wake each");
    const auto started = Clock::now();

    Scheduler scheduler(MakeConfig());
    scheduler.start();

    ParkingCondition condition;
    std::mutex mutex;
    std::vector<bool> ready(static_cast<std::size_t>(kFiberCount), false);
    auto woken = MakeTable(kFiberCount);
    auto timed_out = MakeTable(kFiberCount);
    std::vector<std::shared_ptr<Task>> tasks;

    for (int index = 0; index < kFiberCount; ++index) {
        tasks.push_back(scheduler.spawn([&, index] {
            std::unique_lock<std::mutex> lock(mutex);
            const bool notified = condition.wait_for(
                lock, kParkDeadline, [&] {
                    return ready[static_cast<std::size_t>(index)];
                });
            if (notified) {
                woken[static_cast<std::size_t>(index)].store(
                    1, std::memory_order_release);
            } else {
                timed_out[static_cast<std::size_t>(index)].store(
                    1, std::memory_order_release);
            }
        }));
    }

    GO2CPP_CHECK(
        WaitFiberWaiters(condition, kFiberCount, kParkDeadline));
    // 逐轮置位 + notify_all：被唤醒但谓词未满足的 Fiber 必须重新 park，
    // 下一轮继续；这同时验证了虚假唤醒后的谓词重查。
    for (int index = 0; index < kFiberCount; ++index) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ready[static_cast<std::size_t>(index)] = true;
        }
        condition.notify_all();
        GO2CPP_REQUIRE_EVENTUALLY(
            CountWoken(woken) >= index + 1, 5s);
    }
    for (auto& task : tasks) {
        GO2CPP_REQUIRE(task->wait_for(10s));
    }

    GO2CPP_CHECK(CountWoken(woken) == kFiberCount);
    GO2CPP_CHECK(CountWoken(timed_out) == 0);
    scheduler.shutdown();
    std::printf("[park] pc pairing: woken=%d timed_out=%d elapsed=%.3fs\n",
                CountWoken(woken), CountWoken(timed_out),
                std::chrono::duration<double>(Clock::now() - started).count());
}

// ---------------------------------------------------------------------------
// 3. 丢唤醒注入（Channel）：跳过 1 次 Send，必须由超时检测到 stuck
// ---------------------------------------------------------------------------
void TestChannelDroppedWake() {
    go2cpp_tests::announce(
        "park/wake: channel dropped-wake injection (1 of 4 stuck)");
    const auto started = Clock::now();

    Scheduler scheduler(MakeConfig());
    scheduler.start();

    std::vector<std::shared_ptr<go2cpp::Channel<int>>> channels;
    std::vector<std::shared_ptr<Task>> tasks;
    auto woken = MakeTable(kInjectedCount);
    auto timed_out = MakeTable(kInjectedCount);

    for (int index = 0; index < kInjectedCount; ++index) {
        auto channel = MakeChannel<int>(0);
        channels.push_back(channel);
        tasks.push_back(scheduler.spawn([&, channel, index] {
            const auto result = channel->RecvFor(200ms);
            if (result.Ok()) {
                woken[static_cast<std::size_t>(index)].store(
                    1, std::memory_order_release);
            } else if (result.status == go2cpp::ChannelStatus::kTimedOut) {
                timed_out[static_cast<std::size_t>(index)].store(
                    1, std::memory_order_release);
            }
        }));
    }

    GO2CPP_CHECK(WaitAllParked(tasks, kParkDeadline));
    // 故意跳过 index=0：模拟「wake 丢失」。
    for (int index = 1; index < kInjectedCount; ++index) {
        GO2CPP_CHECK(channels[static_cast<std::size_t>(index)]
                         ->SendFor(index, kParkDeadline)
                         .Ok());
    }
    for (auto& task : tasks) {
        GO2CPP_REQUIRE(task->wait_for(10s));
    }

    GO2CPP_CHECK(woken[0].load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(timed_out[0].load(std::memory_order_acquire) == 1);
    GO2CPP_CHECK(CountWoken(woken) == kInjectedCount - 1);
    GO2CPP_CHECK(CountWoken(timed_out) == 1);
    scheduler.shutdown();
    std::printf("[park] channel dropped-wake: woken=%d timed_out=%d "
                "elapsed=%.3fs\n",
                CountWoken(woken), CountWoken(timed_out),
                std::chrono::duration<double>(Clock::now() - started).count());
}

// ---------------------------------------------------------------------------
// 4. 丢唤醒注入（ParkingCondition）：不置位 1 个谓词
// ---------------------------------------------------------------------------
void TestParkingConditionDroppedWake() {
    go2cpp_tests::announce(
        "park/wake: pc dropped-wake injection (1 of 4 stuck)");
    const auto started = Clock::now();

    Scheduler scheduler(MakeConfig());
    scheduler.start();

    ParkingCondition condition;
    std::mutex mutex;
    std::vector<bool> ready(static_cast<std::size_t>(kInjectedCount), false);
    auto woken = MakeTable(kInjectedCount);
    auto timed_out = MakeTable(kInjectedCount);
    std::vector<std::shared_ptr<Task>> tasks;

    for (int index = 0; index < kInjectedCount; ++index) {
        tasks.push_back(scheduler.spawn([&, index] {
            std::unique_lock<std::mutex> lock(mutex);
            const bool notified = condition.wait_for(
                lock, 200ms, [&] {
                    return ready[static_cast<std::size_t>(index)];
                });
            if (notified) {
                woken[static_cast<std::size_t>(index)].store(
                    1, std::memory_order_release);
            } else {
                timed_out[static_cast<std::size_t>(index)].store(
                    1, std::memory_order_release);
            }
        }));
    }

    GO2CPP_CHECK(
        WaitFiberWaiters(condition, kInjectedCount, kParkDeadline));
    // 故意不置位 index=0：它只能靠 200ms 超时退出。
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (int index = 1; index < kInjectedCount; ++index) {
            ready[static_cast<std::size_t>(index)] = true;
        }
    }
    condition.notify_all();
    for (auto& task : tasks) {
        GO2CPP_REQUIRE(task->wait_for(10s));
    }

    GO2CPP_CHECK(woken[0].load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(timed_out[0].load(std::memory_order_acquire) == 1);
    GO2CPP_CHECK(CountWoken(woken) == kInjectedCount - 1);
    GO2CPP_CHECK(CountWoken(timed_out) == 1);
    scheduler.shutdown();
    std::printf("[park] pc dropped-wake: woken=%d timed_out=%d elapsed=%.3fs\n",
                CountWoken(woken), CountWoken(timed_out),
                std::chrono::duration<double>(Clock::now() - started).count());
}

// ---------------------------------------------------------------------------
// 5. 重复配对：10 轮 × 4 Fiber，验证唤醒状态不跨轮残留
// ---------------------------------------------------------------------------
void TestRepeatedPairing() {
    go2cpp_tests::announce("park/wake: 10 rounds x 4 fibers repeated pairing");
    const auto started = Clock::now();

    constexpr int kRounds = 10;
    constexpr int kPerRound = 4;

    Scheduler scheduler(MakeConfig());
    scheduler.start();
    int paired = 0;
    int failed = 0;

    for (int round = 0; round < kRounds; ++round) {
        std::vector<std::shared_ptr<Task>> tasks;
        std::vector<std::shared_ptr<go2cpp::Channel<int>>> channels;
        auto woken = MakeTable(kPerRound);
        auto values = MakeTable(kPerRound);
        for (int index = 0; index < kPerRound; ++index) {
            auto channel = MakeChannel<int>(0);
            channels.push_back(channel);
            tasks.push_back(scheduler.spawn([&, channel, index] {
                const auto result = channel->RecvFor(2s);
                if (result.Ok()) {
                    woken[static_cast<std::size_t>(index)].store(
                        1, std::memory_order_release);
                    values[static_cast<std::size_t>(index)].store(
                        result.ValueOrDefault(), std::memory_order_release);
                }
            }));
        }
        GO2CPP_CHECK(WaitAllParked(tasks, kParkDeadline));
        for (int index = 0; index < kPerRound; ++index) {
            const int expected = round * 100 + index;
            GO2CPP_CHECK(channels[static_cast<std::size_t>(index)]
                             ->SendFor(expected, 2s)
                             .Ok());
        }
        for (auto& task : tasks) {
            GO2CPP_CHECK(task->wait_for(5s));
        }
        for (int index = 0; index < kPerRound; ++index) {
            if (woken[static_cast<std::size_t>(index)].load(
                    std::memory_order_acquire) == 1 &&
                values[static_cast<std::size_t>(index)].load(
                    std::memory_order_acquire) == round * 100 + index) {
                ++paired;
            } else {
                ++failed;
            }
        }
    }

    GO2CPP_CHECK(failed == 0);
    GO2CPP_CHECK(paired == kRounds * kPerRound);
    scheduler.shutdown();
    std::printf("[park] repeated pairing: paired=%d failed=%d elapsed=%.3fs\n",
                paired, failed,
                std::chrono::duration<double>(Clock::now() - started).count());
}

}  // namespace

int main() {
    std::printf("=== Fiber park/wake 配对测试（GMP + Channel/ParkingCondition）===\n");
    const auto total_started = Clock::now();

    TestChannelParkWakePairing();
    TestParkingConditionParkWakePairing();
    TestChannelDroppedWake();
    TestParkingConditionDroppedWake();
    TestRepeatedPairing();

    const int failures = go2cpp_tests::g_failures.load();
    const double seconds =
        std::chrono::duration<double>(Clock::now() - total_started).count();
    std::printf("[park-wake-pair] failures=%d elapsed=%.3fs\n", failures,
                seconds);
    return failures == 0 ? 0 : 1;
}
