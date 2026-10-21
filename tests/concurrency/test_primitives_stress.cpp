/**
 * @file test_primitives_stress.cpp
 * @brief 10 类并发结构的「压力 / 竞争 / 计数守恒」测试。
 *
 * ## 为什么需要这个文件
 *
 * 现有测试按原语分散（test_sync / test_locks / test_spin_mutex /
 * test_futex_mutex / test_channel …），单点覆盖很细，但没有一个用例把
 * **10 类结构放到同一套多线程压力模型**下，用统一的「计数守恒 + 超时兜底」
 * 口径检查。并发回归最典型的形态不是断言失败，而是**死锁 / 丢唤醒 /
 * 计数漂移**；这三类都必须靠多线程 + 大迭代 + watchdog 才暴露。
 *
 * ## 覆盖矩阵（10 类）
 *
 * | 结构 | 线程数 | 迭代 | 检查点 |
 * |---|---|---|---|
 * | `sync::Mutex` | 8 | 10 万/线程 | 竞争下计数精确、无死锁 |
 * | `sync::RWMutex` | 4 读者 + 2 写者 | 10 万读 / 2 万写 | 读者快照不越界、读写计数 |
 * | `sync::RecursiveMutex` | 4 | 5 万 × 5 层 | 递归深度、计数守恒 |
 * | `sync::TimedMutex` | 4 | 5 万 | 无竞争成功、持锁超时返回 false |
 * | `sync::SpinMutex` | 4 | 10 万 | 普通线程退化路径计数 |
 * | `core::FutexMutex` | 4 | 10 万 | 跨线程门闩计数 |
 * | `sync::ConditionVariable` | 4 生产者 + 4 消费者 | 8000 值 | 每值恰好消费一次、超时返回 false |
 * | `sync::WaitGroup` | 16 | 20 波 × 100 Done | Wait/WaitFor 归零、Count 归零 |
 * | `core::ParkingCondition` | 4 | 2 轮 | notify_all 全醒、notify_one 只醒一个 |
 * | `Channel<T>` | 4 生产者 + 4 消费者 | 1 万（缓冲）/ 5 千（无缓冲） | 每值恰好一次、无丢值 |
 *
 * ## 断言口径
 *
 * 1. **无死锁**：所有 join 走 `GO2CPP_JOIN_WITH_WATCHDOG`，超时直接 abort
 *    并打印 `[watchdog]`，不会把 CI 挂成「一直跑」；
 * 2. **计数守恒**：每个场景的最终计数必须等于「线程数 × 迭代数」；
 * 3. **无崩溃**：场景之间互不共享状态，单个场景的失败不影响后续运行。
 *
 * ## 规模
 *
 * 线程数 4~8、总操作量约 300 万次（Release 构建本机约 5~10 秒）。规模写死
 * 在常量里，调大即可做 soak；默认值保证 ctest 单用例预算内可完成。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/core/futex_mutex.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/locks.hpp"
#include "go2cpp/concurrency/spin_mutex.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

using go2cpp::ChannelStatus;
using go2cpp::MakeChannel;
using go2cpp::core::FutexMutex;
using go2cpp::core::ParkingCondition;
using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;
using go2cpp::sync::RWMutex;
using go2cpp::sync::RecursiveMutex;
using go2cpp::sync::SpinMutex;
using go2cpp::sync::TimedMutex;
using go2cpp::sync::WaitGroup;

/// 锁竞争场景的线程数（任务要求 4~8）。
constexpr int kLockThreads = 8;
/// 每个线程的加解锁次数（任务要求 1e5）。
constexpr int kLockIterations = 100000;

/// 打印「场景名 + 操作量 + 耗时」，便于报告直接引用实测数据。
void Report(const char* name, Clock::time_point started, long long operations) {
    const double seconds =
        std::chrono::duration<double>(Clock::now() - started).count();
    std::printf("[stress] %-30s ops=%-10lld elapsed=%7.3fs\n", name,
                operations, seconds);
}

/// 启动闸门：让所有线程尽量同时开始竞争，避免「顺序执行」假压测。
class StartGate {
public:
    void Wait() {
        while (!m_open.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    void Open() { m_open.store(true, std::memory_order_release); }

private:
    std::atomic<bool> m_open{false};
};

// ---------------------------------------------------------------------------
// 1. sync::Mutex —— 8 线程 × 10 万次锁竞争
// ---------------------------------------------------------------------------
void TestMutexContention() {
    go2cpp_tests::announce("stress: sync::Mutex 8 threads x 100000 locks");
    const auto started = Clock::now();

    Mutex mutex;
    std::int64_t counter = 0;
    std::atomic<int> lock_failures{0};
    StartGate gate;
    std::vector<std::thread> threads;
    threads.reserve(kLockThreads);
    for (int index = 0; index < kLockThreads; ++index) {
        threads.emplace_back([&] {
            gate.Wait();
            for (int iteration = 0; iteration < kLockIterations; ++iteration) {
                if (!mutex.Lock()) {
                    lock_failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                ++counter;
                mutex.Unlock();
            }
        });
    }
    gate.Open();
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 120s);
    }

    GO2CPP_CHECK(lock_failures.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(counter ==
                 static_cast<std::int64_t>(kLockThreads) * kLockIterations);
    GO2CPP_CHECK(mutex.TryLock());  // 全部释放后锁必须空闲
    mutex.Unlock();
    Report("sync::Mutex", started,
           static_cast<long long>(kLockThreads) * kLockIterations);
}

// ---------------------------------------------------------------------------
// 2. sync::RWMutex —— 4 读者 + 2 写者（读多写少）
// ---------------------------------------------------------------------------
void TestRWMutexReadersWriters() {
    go2cpp_tests::announce(
        "stress: RWMutex 4 readers x 100000 / 2 writers x 20000");
    const auto started = Clock::now();

    constexpr int kReaders = 4;
    constexpr int kReadIterations = 100000;
    constexpr int kWriters = 2;
    constexpr int kWriteIterations = 20000;
    constexpr std::int64_t kMaxValue =
        static_cast<std::int64_t>(kWriters) * kWriteIterations;

    RWMutex mutex;
    std::int64_t value = 0;  // 写锁保护写、读锁保护读
    std::atomic<std::int64_t> read_iterations{0};
    std::atomic<std::int64_t> write_iterations{0};
    std::atomic<std::int64_t> read_sum{0};
    std::atomic<int> bad_snapshot{0};
    StartGate gate;
    std::vector<std::thread> threads;

    for (int index = 0; index < kReaders; ++index) {
        threads.emplace_back([&] {
            gate.Wait();
            for (int iteration = 0; iteration < kReadIterations; ++iteration) {
                mutex.RLock();
                const std::int64_t snapshot = value;
                if (snapshot < 0 || snapshot > kMaxValue) {
                    bad_snapshot.fetch_add(1, std::memory_order_relaxed);
                }
                read_sum.fetch_add(snapshot, std::memory_order_relaxed);
                read_iterations.fetch_add(1, std::memory_order_relaxed);
                mutex.RUnlock();
            }
        });
    }
    for (int index = 0; index < kWriters; ++index) {
        threads.emplace_back([&] {
            gate.Wait();
            for (int iteration = 0; iteration < kWriteIterations; ++iteration) {
                mutex.Lock();
                ++value;
                write_iterations.fetch_add(1, std::memory_order_relaxed);
                mutex.Unlock();
            }
        });
    }
    gate.Open();
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 180s);
    }

    GO2CPP_CHECK(bad_snapshot.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(read_iterations.load(std::memory_order_acquire) ==
                 static_cast<std::int64_t>(kReaders) * kReadIterations);
    GO2CPP_CHECK(write_iterations.load(std::memory_order_acquire) == kMaxValue);
    GO2CPP_CHECK(read_sum.load(std::memory_order_acquire) >= 0);
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    Report("sync::RWMutex", started,
           static_cast<long long>(kReaders) * kReadIterations +
               static_cast<long long>(kWriters) * kWriteIterations);
}

// ---------------------------------------------------------------------------
// 3. sync::RecursiveMutex —— 每轮 5 层递归
// ---------------------------------------------------------------------------
void TestRecursiveMutex() {
    go2cpp_tests::announce("stress: RecursiveMutex 4 threads x 50000 x depth 5");
    const auto started = Clock::now();

    constexpr int kThreads = 4;
    constexpr int kIterations = 50000;
    constexpr int kDepth = 5;

    RecursiveMutex mutex;
    std::int64_t counter = 0;
    std::atomic<int> depth_mismatch{0};
    StartGate gate;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&] {
            gate.Wait();
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                for (int depth = 0; depth < kDepth; ++depth) {
                    mutex.Lock();
                }
                if (mutex.RecursionCount() !=
                    static_cast<std::uint32_t>(kDepth)) {
                    depth_mismatch.fetch_add(1, std::memory_order_relaxed);
                }
                ++counter;
                for (int depth = 0; depth < kDepth; ++depth) {
                    mutex.Unlock();
                }
            }
        });
    }
    gate.Open();
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 120s);
    }

    GO2CPP_CHECK(depth_mismatch.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(counter == static_cast<std::int64_t>(kThreads) * kIterations);
    GO2CPP_CHECK(mutex.RecursionCount() == 0);
    Report("sync::RecursiveMutex", started,
           static_cast<long long>(kThreads) * kIterations * kDepth * 2);
}

// ---------------------------------------------------------------------------
// 4. sync::TimedMutex —— 无竞争成功 + 持锁超时
// ---------------------------------------------------------------------------
void TestTimedMutex() {
    go2cpp_tests::announce("stress: TimedMutex 4 x 50000 + timeout path");
    const auto started = Clock::now();

    constexpr int kThreads = 4;
    constexpr int kIterations = 50000;

    TimedMutex mutex;
    std::int64_t counter = 0;
    StartGate gate;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&] {
            gate.Wait();
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                GO2CPP_REQUIRE(mutex.TryLockFor(100ms));
                ++counter;
                mutex.Unlock();
            }
        });
    }
    gate.Open();
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 120s);
    }
    GO2CPP_CHECK(counter == static_cast<std::int64_t>(kThreads) * kIterations);

    // 持锁时 TryLockFor 必须在超时后返回 false，且不改变锁状态。
    TimedMutex contended;
    contended.Lock();
    std::atomic<bool> gate_open{false};
    std::atomic<bool> acquired{true};
    std::atomic<long long> elapsed_ms{0};
    std::thread waiter([&] {
        while (!gate_open.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const auto wait_started = Clock::now();
        acquired.store(contended.TryLockFor(60ms), std::memory_order_release);
        elapsed_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - wait_started)
                .count(),
            std::memory_order_release);
    });
    gate_open.store(true, std::memory_order_release);
    GO2CPP_JOIN_WITH_WATCHDOG(waiter, 30s);
    GO2CPP_CHECK(!acquired.load(std::memory_order_acquire));
    GO2CPP_CHECK(elapsed_ms.load(std::memory_order_acquire) >= 50);
    GO2CPP_CHECK(!contended.TryLock());  // 锁仍被主线程持有
    contended.Unlock();
    GO2CPP_CHECK(contended.TryLock());  // 状态干净，可再次获取
    contended.Unlock();

    Report("sync::TimedMutex", started,
           static_cast<long long>(kThreads) * kIterations);
}

// ---------------------------------------------------------------------------
// 5. sync::SpinMutex —— 普通线程退化为 Mutex 的路径
// ---------------------------------------------------------------------------
void TestSpinMutex() {
    go2cpp_tests::announce("stress: SpinMutex 4 threads x 100000 locks");
    const auto started = Clock::now();

    constexpr int kThreads = 4;
    constexpr int kIterations = 100000;

    SpinMutex mutex;
    std::int64_t counter = 0;
    StartGate gate;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&] {
            gate.Wait();
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                mutex.Lock();
                ++counter;
                mutex.Unlock();
            }
        });
    }
    gate.Open();
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 120s);
    }

    GO2CPP_CHECK(counter == static_cast<std::int64_t>(kThreads) * kIterations);
    GO2CPP_CHECK(!mutex.IsLocked());
    Report("sync::SpinMutex", started,
           static_cast<long long>(kThreads) * kIterations);
}

// ---------------------------------------------------------------------------
// 6. core::FutexMutex —— 最底层门闩，跨线程所有权
// ---------------------------------------------------------------------------
void TestFutexMutex() {
    go2cpp_tests::announce("stress: FutexMutex 4 threads x 100000 locks");
    const auto started = Clock::now();

    constexpr int kThreads = 4;
    constexpr int kIterations = 100000;

    FutexMutex mutex;
    std::int64_t counter = 0;
    StartGate gate;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&] {
            gate.Wait();
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                mutex.lock();
                ++counter;
                mutex.unlock();
            }
        });
    }
    gate.Open();
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 120s);
    }

    GO2CPP_CHECK(counter == static_cast<std::int64_t>(kThreads) * kIterations);
    GO2CPP_CHECK(mutex.try_lock());  // 全部释放后可用
    mutex.unlock();
    Report("core::FutexMutex", started,
           static_cast<long long>(kThreads) * kIterations);
}

// ---------------------------------------------------------------------------
// 7. sync::ConditionVariable —— 多生产者/多消费者 + 每值恰好一次
// ---------------------------------------------------------------------------
void TestConditionVariablePairing() {
    go2cpp_tests::announce(
        "stress: ConditionVariable 4 producers x 2000 / 4 consumers");
    const auto started = Clock::now();

    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 2000;
    constexpr int kTotal = kProducers * kPerProducer;

    Mutex mutex;
    ConditionVariable condition;
    std::deque<int> queue;
    bool closed = false;
    std::int64_t produced = 0;
    std::int64_t consumed = 0;
    std::vector<std::atomic<int>> seen(static_cast<std::size_t>(kTotal));
    for (auto& slot : seen) {
        slot.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> bad_values{0};

    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&, producer] {
            for (int index = 0; index < kPerProducer; ++index) {
                const int value = producer * kPerProducer + index;
                mutex.Lock();
                queue.push_back(value);
                ++produced;
                mutex.Unlock();
                condition.NotifyOne();
            }
        });
    }
    std::vector<std::thread> consumers;
    for (int consumer = 0; consumer < kConsumers; ++consumer) {
        consumers.emplace_back([&] {
            for (;;) {
                mutex.Lock();
                while (queue.empty() && !closed) {
                    if (!condition.Wait(mutex)) {
                        // 无 Context 的等待只在关闭阶段返回 false；继续
                        // 重查谓词即可，这里不把它当失败。
                        continue;
                    }
                }
                if (queue.empty() && closed) {
                    mutex.Unlock();
                    return;
                }
                const int value = queue.front();
                queue.pop_front();
                ++consumed;
                mutex.Unlock();
                if (value < 0 || value >= kTotal) {
                    bad_values.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                seen[static_cast<std::size_t>(value)].fetch_add(
                    1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& producer : producers) {
        GO2CPP_JOIN_WITH_WATCHDOG(producer, 120s);
    }
    mutex.Lock();
    closed = true;
    mutex.Unlock();
    condition.NotifyAll();
    for (auto& consumer : consumers) {
        GO2CPP_JOIN_WITH_WATCHDOG(consumer, 120s);
    }

    GO2CPP_CHECK(bad_values.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(produced == kTotal);
    GO2CPP_CHECK(consumed == kTotal);
    int missing = 0;
    int duplicated = 0;
    for (const auto& slot : seen) {
        const int count = slot.load(std::memory_order_relaxed);
        if (count == 0) {
            ++missing;
        } else if (count != 1) {
            ++duplicated;
        }
    }
    GO2CPP_CHECK(missing == 0);
    GO2CPP_CHECK(duplicated == 0);

    // 无通知时 WaitFor 必须按时返回 false，并且重新持有 mutex。
    Mutex timeout_mutex;
    ConditionVariable timeout_condition;
    timeout_mutex.Lock();
    const auto wait_started = Clock::now();
    const bool notified = timeout_condition.WaitFor(timeout_mutex, 30ms);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - wait_started)
                            .count();
    GO2CPP_CHECK(!notified);
    GO2CPP_CHECK(waited >= 20);
    GO2CPP_CHECK(!timeout_mutex.TryLock());  // WaitFor 返回前已重新持锁
    timeout_mutex.Unlock();
    GO2CPP_CHECK(timeout_mutex.TryLock());
    timeout_mutex.Unlock();

    Report("sync::ConditionVariable", started, kTotal * 2);
}

// ---------------------------------------------------------------------------
// 8. sync::WaitGroup —— 多波次归零 + Count 一致性
// ---------------------------------------------------------------------------
void TestWaitGroup() {
    go2cpp_tests::announce("stress: WaitGroup 20 waves x 16 workers x 100 Done");
    const auto started = Clock::now();

    constexpr int kWaves = 20;
    constexpr int kWorkers = 16;
    constexpr int kDonesPerWorker = 100;

    for (int wave = 0; wave < kWaves; ++wave) {
        WaitGroup group;
        group.Add(static_cast<std::int64_t>(kWorkers) * kDonesPerWorker);
        std::vector<std::thread> workers;
        workers.reserve(kWorkers);
        for (int worker = 0; worker < kWorkers; ++worker) {
            workers.emplace_back([&] {
                for (int done = 0; done < kDonesPerWorker; ++done) {
                    group.Done();
                }
            });
        }
        // WaitFor 在主线程与 worker 并发执行，验证的是「归零即醒」。
        GO2CPP_CHECK(group.WaitFor(30s));
        for (auto& worker : workers) {
            GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
        }
        GO2CPP_CHECK(group.Count() == 0);
    }

    // 已取消的 Context：Wait 立即返回 false（计数未归零）。
    WaitGroup cancelled_group;
    cancelled_group.Add(1);
    auto cancelled =
        go2cpp::Context::WithCancel(go2cpp::Context::Background());
    cancelled.second();
    GO2CPP_CHECK(!cancelled_group.Wait(cancelled.first));
    GO2CPP_CHECK(cancelled_group.Count() == 1);
    cancelled_group.Done();
    GO2CPP_CHECK(cancelled_group.WaitFor(5s));

    Report("sync::WaitGroup", started,
           static_cast<long long>(kWaves) * kWorkers * kDonesPerWorker);
}

// ---------------------------------------------------------------------------
// 9. core::ParkingCondition —— notify_all 全醒 + notify_one 只醒一个
// ---------------------------------------------------------------------------
void TestParkingConditionPairing() {
    go2cpp_tests::announce(
        "stress: ParkingCondition notify_all / notify_one pairing");
    const auto started = Clock::now();

    // 9.1 notify_all：4 个等待者全部被唤醒。
    {
        ParkingCondition condition;
        std::mutex mutex;
        bool ready = false;
        std::atomic<int> woken{0};
        std::vector<std::thread> waiters;
        for (int index = 0; index < 4; ++index) {
            waiters.emplace_back([&] {
                std::unique_lock<std::mutex> lock(mutex);
                const bool notified =
                    condition.wait_for(lock, 10s, [&] { return ready; });
                if (notified) {
                    woken.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        GO2CPP_REQUIRE_EVENTUALLY(condition.DebugSnapshot().waiters >= 4, 5s);
        {
            std::lock_guard<std::mutex> lock(mutex);
            ready = true;
        }
        condition.notify_all();
        for (auto& waiter : waiters) {
            GO2CPP_JOIN_WITH_WATCHDOG(waiter, 15s);
        }
        GO2CPP_CHECK(woken.load(std::memory_order_acquire) == 4);
    }

    // 9.2 notify_one：只唤醒一个等待者（其余继续睡到 stop）。
    {
        ParkingCondition condition;
        std::mutex mutex;
        int available = 0;
        bool stop = false;
        std::atomic<int> woken{0};
        std::vector<std::thread> waiters;
        constexpr int kWaiters = 4;
        for (int index = 0; index < kWaiters; ++index) {
            waiters.emplace_back([&] {
                std::unique_lock<std::mutex> lock(mutex);
                while (available == 0 && !stop) {
                    (void)condition.wait(lock,
                                         [&] { return available > 0 || stop; });
                }
                if (available > 0) {
                    --available;
                    woken.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        GO2CPP_REQUIRE_EVENTUALLY(
            condition.DebugSnapshot().waiters >= kWaiters, 5s);
        {
            std::lock_guard<std::mutex> lock(mutex);
            available = 1;
        }
        condition.notify_one();
        // 给足时间让「不应被唤醒」的等待者保持睡眠。
        std::this_thread::sleep_for(50ms);
        GO2CPP_CHECK(woken.load(std::memory_order_acquire) == 1);

        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
            available = kWaiters;
        }
        condition.notify_all();
        for (auto& waiter : waiters) {
            GO2CPP_JOIN_WITH_WATCHDOG(waiter, 15s);
        }
        GO2CPP_CHECK(woken.load(std::memory_order_acquire) == kWaiters);
    }

    Report("core::ParkingCondition", started, 8);
}

// ---------------------------------------------------------------------------
// 10. Channel<T> —— 多生产者/多消费者（有缓冲 + 无缓冲）
// ---------------------------------------------------------------------------
void TestChannelMpmc(std::size_t capacity, int producers, int consumers,
                     int per_producer) {
    const int total = producers * per_producer;
    std::vector<std::atomic<int>> seen(static_cast<std::size_t>(total));
    for (auto& slot : seen) {
        slot.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> send_failures{0};
    std::atomic<int> recv_failures{0};
    std::atomic<int> consumed{0};

    auto channel = MakeChannel<int>(capacity);
    std::vector<std::thread> worker_threads;
    for (int producer = 0; producer < producers; ++producer) {
        worker_threads.emplace_back([&, producer] {
            for (int index = 0; index < per_producer; ++index) {
                const int value = producer * per_producer + index;
                if (!channel->SendFor(value, 10s).Ok()) {
                    send_failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (int consumer = 0; consumer < consumers; ++consumer) {
        worker_threads.emplace_back([&] {
            for (;;) {
                const auto result = channel->RecvFor(10s);
                if (result.Ok()) {
                    const int value = result.ValueOrDefault();
                    if (value < 0 || value >= total) {
                        recv_failures.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        seen[static_cast<std::size_t>(value)].fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (result.status == ChannelStatus::kClosed) {
                    return;
                }
                recv_failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        });
    }

    // 生产者全部结束后关闭通道，唤醒所有等待中的消费者。
    for (int index = 0; index < producers; ++index) {
        GO2CPP_JOIN_WITH_WATCHDOG(
            worker_threads[static_cast<std::size_t>(index)], 300s);
    }
    channel->Close();
    for (std::size_t index = static_cast<std::size_t>(producers);
         index < worker_threads.size(); ++index) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker_threads[index], 300s);
    }

    GO2CPP_CHECK(send_failures.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(recv_failures.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(consumed.load(std::memory_order_acquire) == total);
    int missing = 0;
    int duplicated = 0;
    for (const auto& slot : seen) {
        const int count = slot.load(std::memory_order_relaxed);
        if (count == 0) {
            ++missing;
        } else if (count != 1) {
            ++duplicated;
        }
    }
    GO2CPP_CHECK(missing == 0);
    GO2CPP_CHECK(duplicated == 0);
    GO2CPP_CHECK(channel->IsClosed());
}

void TestChannels() {
    go2cpp_tests::announce(
        "stress: Channel 4x2500 buffered(7) + 4x1250 unbuffered");
    const auto started = Clock::now();
    TestChannelMpmc(7, 4, 4, 2500);
    TestChannelMpmc(0, 4, 4, 1250);
    Report("Channel<T>", started, 10000 + 5000);
}

}  // namespace

int main() {
    std::printf("=== 10 类并发结构压力测试（竞争 / 计数守恒 / 无死锁）===\n");
    const auto total_started = Clock::now();

    TestMutexContention();
    TestRWMutexReadersWriters();
    TestRecursiveMutex();
    TestTimedMutex();
    TestSpinMutex();
    TestFutexMutex();
    TestConditionVariablePairing();
    TestWaitGroup();
    TestParkingConditionPairing();
    TestChannels();

    const int failures = go2cpp_tests::g_failures.load();
    const double seconds =
        std::chrono::duration<double>(Clock::now() - total_started).count();
    std::printf("[primitives-stress] failures=%d elapsed=%.3fs\n", failures,
                seconds);
    return failures == 0 ? 0 : 1;
}
