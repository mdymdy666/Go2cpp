/**
 * @file test_sync_scenarios.cpp
 * @brief Mutex / ConditionVariable / WaitGroup 的真实用法场景测试
 *        （独立可执行，不注册 ctest）。
 *
 * @date 2026-10-30
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 为什么需要这个文件
 *
 * `test_sync.cpp` 覆盖的是 Scheduler/Fiber 语义下的正确性；`test_locks.cpp`
 * 覆盖的是 RWMutex/SpinMutex 等其它锁。本文件用**纯 std::thread**（不启动
 * Scheduler）压 sync 原语自身的热路径与边界：无竞争 CAS、等待队列、唤醒
 * 计数镜像、超时/取消后的摘除。这些路径上的计数一旦与队列不一致，症状是
 * 「偶发睡死」，只有反复的真实用法才能暴露。
 *
 * ## 场景（覆盖 Mutex / CV / WaitGroup 的全部对外功能）
 *
 * | # | 场景 | 验证的功能 |
 * |---|---|---|
 * | 1 | 有界阻塞队列（3 生产者 / 4 消费者） | Mutex lock/unlock、CV Wait/NotifyOne 两个方向、不丢不重 |
 * | 2 | 连接池（16 线程借还，容量 4） | CV 等待者排队、并发上限、release→NotifyOne |
 * | 3 | WaitGroup fan-out / fan-in + 复用 50 轮 | Add/Done/Wait、归零唤醒、归零后重新 Add、Count |
 * | 4 | Mutex 高争用公平性（12 线程 + 2ms 长临界区） | 不饿死、进度守恒 |
 * | 5 | CV 广播关闭（8 等待者 × 200 轮） | NotifyAll 唤醒全部、关闭标志 |
 * | 6 | 超时/取消（CV WaitFor、Mutex LockFor） | 超时返回 false、等待者被摘除、后续仍可用 |
 * | 7 | 随机交错（4 线程随机操作序列） | 混合路径下的不变量与不丢唤醒 |
 *
 * ## 运行
 *
 * ```
 * g++ -std=c++17 -O2 -DGO2CPP_SYNC_SCENARIOS_MAIN ... test_sync_scenarios.cpp
 * ./test_sync_scenarios
 * ```
 */
#include "go2cpp/sync.hpp"

#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using Clock = std::chrono::steady_clock;

/// 场景 1：有界阻塞队列——Mutex + 两个方向的条件变量。
void RunBoundedQueueScenario() {
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable not_empty;
    go2cpp::sync::ConditionVariable not_full;
    std::deque<int> items;
    constexpr std::size_t kCapacity = 8;
    constexpr int kProducers = 3;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 2000;

    std::atomic<bool> stop{false};
    std::atomic<long long> consumed{0};
    std::atomic<long long> produced{0};

    const auto producer = [&](int id) {
        for (int i = 0; i < kPerProducer; ++i) {
            mutex.Lock();
            while (items.size() >= kCapacity &&
                   !stop.load(std::memory_order_acquire)) {
                not_full.Wait(mutex);
            }
            if (stop.load(std::memory_order_acquire)) {
                mutex.Unlock();
                return;
            }
            items.push_back(id * 1000000 + i);
            mutex.Unlock();
            produced.fetch_add(1, std::memory_order_relaxed);
            not_empty.NotifyOne();
        }
    };

    const auto consumer = [&] {
        for (;;) {
            mutex.Lock();
            while (items.empty() && !stop.load(std::memory_order_acquire)) {
                not_empty.Wait(mutex);
            }
            if (items.empty()) {  // stop 已置位且队列已排空
                mutex.Unlock();
                return;
            }
            (void)items.front();
            items.pop_front();
            mutex.Unlock();
            consumed.fetch_add(1, std::memory_order_relaxed);
            not_full.NotifyOne();
        }
    };

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back(producer, t);
    }
    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int t = 0; t < kConsumers; ++t) {
        consumers.emplace_back(consumer);
    }

    for (auto& thread : producers) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 120s);
    }
    stop.store(true, std::memory_order_release);
    not_empty.NotifyAll();
    not_full.NotifyAll();
    for (auto& thread : consumers) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 120s);
    }

    const long long expected =
        static_cast<long long>(kProducers) * kPerProducer;
    GO2CPP_CHECK(produced.load() == expected);
    GO2CPP_CHECK(consumed.load() == expected);
    go2cpp_tests::announce(
        "scenario 1: bounded blocking queue 3P/4C 6000 items");
}

/// 场景 2：连接池——容量耗尽时在 CV 上等，release 时唤醒一个。
class ConnectionPool {
public:
    explicit ConnectionPool(std::size_t capacity) : m_capacity(capacity) {}

    int Acquire() {
        m_mutex.Lock();
        while (m_free.empty() && m_created >= m_capacity) {
            m_available.Wait(m_mutex);
        }
        int id = 0;
        if (!m_free.empty()) {
            id = m_free.back();
            m_free.pop_back();
        } else {
            id = static_cast<int>(m_created);
            ++m_created;
        }
        m_mutex.Unlock();
        return id;
    }

    void Release(int id) {
        m_mutex.Lock();
        m_free.push_back(id);
        m_mutex.Unlock();
        m_available.NotifyOne();
    }

private:
    go2cpp::sync::Mutex m_mutex;
    go2cpp::sync::ConditionVariable m_available;
    std::vector<int> m_free;
    std::size_t m_capacity;
    std::size_t m_created{0};
};

void RunConnectionPoolScenario() {
    constexpr std::size_t kCapacity = 4;
    constexpr int kThreads = 16;
    constexpr int kPerThread = 500;
    ConnectionPool pool(kCapacity);
    std::atomic<int> in_use{0};
    std::atomic<int> peak{0};
    std::atomic<long long> borrowed{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                const int id = pool.Acquire();
                const int now = in_use.fetch_add(1) + 1;
                int observed = peak.load(std::memory_order_relaxed);
                while (now > observed &&
                       !peak.compare_exchange_weak(observed, now)) {
                }
                borrowed.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
                in_use.fetch_sub(1);
                pool.Release(id);
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 180s);
    }
    GO2CPP_CHECK(borrowed.load() ==
                 static_cast<long long>(kThreads) * kPerThread);
    GO2CPP_CHECK(in_use.load() == 0);
    GO2CPP_CHECK(peak.load() <= static_cast<int>(kCapacity));
    go2cpp_tests::announce(
        "scenario 2: connection pool cap=4 16 threads 8000 borrows");
}

/// 场景 3：WaitGroup fan-out / fan-in，并验证归零后可以重新 Add（复用）。
void RunWaitGroupFanOutScenario() {
    go2cpp::sync::WaitGroup group;
    constexpr int kWorkers = 8;
    constexpr int kRounds = 50;
    constexpr long long kWorkPerWorker = 1000;

    std::vector<long long> partial(static_cast<std::size_t>(kWorkers), 0);
    for (int round = 0; round < kRounds; ++round) {
        for (auto& slot : partial) {
            slot = 0;
        }
        group.Add(kWorkers);
        std::vector<std::thread> workers;
        workers.reserve(kWorkers);
        for (int t = 0; t < kWorkers; ++t) {
            workers.emplace_back([&, t] {
                auto& slot = partial[static_cast<std::size_t>(t)];
                for (long long i = 0; i < kWorkPerWorker; ++i) {
                    slot += i & 1;
                }
                group.Done();
            });
        }
        GO2CPP_CHECK(group.Wait());
        for (auto& worker : workers) {
            GO2CPP_JOIN_WITH_WATCHDOG(worker, 60s);
        }
        long long total = 0;
        for (const auto slot : partial) {
            total += slot;
        }
        GO2CPP_CHECK(group.Count() == 0);
        GO2CPP_CHECK(total == static_cast<long long>(kWorkers) *
                                 (kWorkPerWorker / 2));
    }
    go2cpp_tests::announce(
        "scenario 3: WaitGroup fan-out/fan-in 8 workers x 50 rounds");
}

/// 场景 4：高争用下的公平性——持锁线程不应让其它线程饿死。
void RunMutexFairnessScenario() {
    go2cpp::sync::Mutex mutex;
    constexpr int kThreads = 12;
    constexpr int kOpsPerThread = 20000;
    std::atomic<int> finished{0};
    std::atomic<long long> counter{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < kOpsPerThread; ++i) {
                mutex.Lock();
                counter.fetch_add(1, std::memory_order_relaxed);
                if (i == 0) {
                    // 首轮制造一次 2ms 长临界区：其余线程必须排队而不是空转。
                    std::this_thread::sleep_for(2ms);
                }
                mutex.Unlock();
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }
    // 主线程持续抢锁：正常模式允许插队，验证等待者不会被永久饿死。
    for (int i = 0; i < 2000; ++i) {
        mutex.Lock();
        mutex.Unlock();
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 180s);
    }
    GO2CPP_CHECK(finished.load() == kThreads);
    GO2CPP_CHECK(counter.load() ==
                 static_cast<long long>(kThreads) * kOpsPerThread);
    go2cpp_tests::announce(
        "scenario 4: mutex fairness 12 threads x 20000 ops");
}

/// 场景 5：条件变量广播——NotifyAll 必须唤醒所有已在等待的线程。
void RunBroadcastShutdownScenario() {
    constexpr int kWatchers = 8;
    constexpr int kRounds = 200;
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable closed_cv;
    bool closed = false;

    for (int round = 0; round < kRounds; ++round) {
        mutex.Lock();
        closed = false;
        mutex.Unlock();
        std::atomic<int> entered{0};
        std::atomic<int> woke{0};

        std::vector<std::thread> watchers;
        watchers.reserve(kWatchers);
        for (int t = 0; t < kWatchers; ++t) {
            watchers.emplace_back([&] {
                mutex.Lock();
                // 在锁内登记：主线程随后能取到锁，说明 8 个 watcher 都已
                // 释放过锁（即都已进入 Wait 或已经退出），不会漏唤醒。
                entered.fetch_add(1, std::memory_order_release);
                while (!closed) {
                    closed_cv.Wait(mutex);
                }
                mutex.Unlock();
                woke.fetch_add(1, std::memory_order_release);
            });
        }
        GO2CPP_REQUIRE_EVENTUALLY(entered.load() == kWatchers, 20s);
        mutex.Lock();
        closed = true;
        mutex.Unlock();
        closed_cv.NotifyAll();
        for (auto& watcher : watchers) {
            GO2CPP_JOIN_WITH_WATCHDOG(watcher, 60s);
        }
        GO2CPP_CHECK(woke.load() == kWatchers);
    }
    go2cpp_tests::announce(
        "scenario 5: CV NotifyAll shutdown 8 watchers x 200 rounds");
}

/// 场景 6：超时与取消——CV WaitFor 超时、Mutex LockFor 超时，之后原语仍可用。
void RunTimeoutScenario() {
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable cv;
    bool flag = false;

    // 6a：CV 超时返回 false，且等待者被正确摘除（连续 200 次不残留）。
    mutex.Lock();
    for (int i = 0; i < 200; ++i) {
        const auto start = Clock::now();
        const bool notified = cv.WaitFor(mutex, 2ms);
        const auto elapsed = Clock::now() - start;
        GO2CPP_CHECK(!notified);
        GO2CPP_CHECK(elapsed >= 1ms);
    }
    mutex.Unlock();

    // 超时风暴之后，正常的 wait/notify 配对仍然工作（说明没有残留节点）。
    std::atomic<int> notified_count{0};
    std::thread waiter([&] {
        mutex.Lock();
        while (!flag) {
            cv.Wait(mutex);
        }
        mutex.Unlock();
        notified_count.fetch_add(1, std::memory_order_release);
    });
    std::this_thread::sleep_for(20ms);
    mutex.Lock();
    flag = true;
    mutex.Unlock();
    cv.NotifyOne();
    GO2CPP_JOIN_WITH_WATCHDOG(waiter, 30s);
    GO2CPP_CHECK(notified_count.load() == 1);

    // 6b：Mutex LockFor 超时——锁被别的线程持有 200ms，2ms 超时必须失败。
    std::atomic<bool> held{false};
    std::thread holder([&] {
        mutex.Lock();
        held.store(true, std::memory_order_release);
        std::this_thread::sleep_for(200ms);
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(held.load(), 10s);
    const auto lock_start = Clock::now();
    GO2CPP_CHECK(!mutex.LockFor(2ms));
    const auto lock_elapsed = Clock::now() - lock_start;
    GO2CPP_CHECK(lock_elapsed >= 1ms);
    GO2CPP_CHECK(lock_elapsed < 150ms);
    GO2CPP_JOIN_WITH_WATCHDOG(holder, 30s);
    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();
    go2cpp_tests::announce("scenario 6: CV/Mutex timeout + cancel paths");
}

/// 场景 7：随机交错——每线程随机执行 Mutex/CV/WaitGroup 操作，验证不变量。
void RunRandomizedInterleavingScenario() {
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 2000;
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable cv;
    go2cpp::sync::WaitGroup group;
    long long protected_counter = 0;
    std::atomic<long long> group_ops{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> workers;
    workers.reserve(kThreads + 1U);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937 rng(static_cast<std::uint32_t>(t) * 7919U + 17U);
            std::uniform_int_distribution<int> op(0, 4);
            for (int i = 0; i < kOpsPerThread; ++i) {
                switch (op(rng)) {
                    case 0:
                    case 1: {
                        mutex.Lock();
                        ++protected_counter;
                        mutex.Unlock();
                        break;
                    }
                    case 2: {
                        mutex.Lock();
                        (void)cv.WaitFor(mutex, 1ms);
                        mutex.Unlock();
                        break;
                    }
                    case 3: {
                        cv.NotifyOne();
                        break;
                    }
                    default: {
                        group.Add(1);
                        group_ops.fetch_add(1, std::memory_order_relaxed);
                        group.Done();
                        break;
                    }
                }
            }
        });
    }
    workers.emplace_back([&] {
        while (!stop.load(std::memory_order_acquire)) {
            cv.NotifyAll();
            std::this_thread::yield();
        }
    });
    for (int t = 0; t < kThreads; ++t) {
        GO2CPP_JOIN_WITH_WATCHDOG(workers[static_cast<std::size_t>(t)], 120s);
    }
    stop.store(true, std::memory_order_release);
    GO2CPP_JOIN_WITH_WATCHDOG(workers.back(), 30s);
    GO2CPP_CHECK(group.Wait());
    GO2CPP_CHECK(group.Count() == 0);
    GO2CPP_CHECK(protected_counter > 0);
    (void)group_ops;
    go2cpp_tests::announce(
        "scenario 7: randomized interleaving 4 threads x 2000 ops");
}

}  // namespace

#ifdef GO2CPP_SYNC_SCENARIOS_MAIN
#include <iostream>

int main() {
    RunBoundedQueueScenario();
    RunConnectionPoolScenario();
    RunWaitGroupFanOutScenario();
    RunMutexFairnessScenario();
    RunBroadcastShutdownScenario();
    RunTimeoutScenario();
    RunRandomizedInterleavingScenario();
    const int failures =
        go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::cerr << "[test] failures=" << failures << '\n';
        return 1;
    }
    std::cout << "[test] all sync scenario checks passed\n";
    return 0;
}
#endif
