/**
 * @file test_rwmutex_opt.cpp
 * @brief `sync::RWMutex` 优化后的专项正确性测试：随机并发、超时/取消路径、
 *        写者交接 FIFO、协程-线程混合，以及 5 个真实场景。
 *
 * @date 2026-10-30
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * 本文件是独立可执行程序（不注册 ctest，也不进入 go2cpp_tests），用于验证
 * `include/go2cpp/concurrency/rwmutex.hpp` 的读者门 + 写者交接令牌改造：
 *
 * 1. 语义：读共享 / 写独占、写者优先、写者 FIFO、超时返回值与状态不变；
 * 2. 回归：`RLockFor` 在写者解锁后必须被唤醒（旧实现因未登记
 *    `m_waiting_readers` 会睡到超时）；
 * 3. 随机：多线程随机 RLock/TryRLock/Lock/TryLock/RLockFor/LockFor，
 *    校验读写互斥、计数器一致、无死锁；
 * 4. 场景：配置热加载、连接池、写者饥饿、超时取消风暴、Fiber+线程混合。
 *
 * 用法：`./test_rwmutex_opt`（失败返回 1，并打印 check 位置）。
 */
#include "go2cpp/concurrency/rwmutex.hpp"
#include "go2cpp/concurrency/scheduler/scheduler.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using go2cpp::sync::RWMutex;

constexpr auto kJoinTimeout = 20s;

double ElapsedUs(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now() - begin)
        .count();
}

go2cpp::SchedulerConfig MakeSchedulerConfig(std::size_t processors) {
    go2cpp::SchedulerConfig config;
    config.processor_count = processors;
    config.max_workers = processors;
    config.idle_wait = 1ms;
    return config;
}

void ShutdownOrReportLostWakeup(go2cpp::Scheduler& scheduler) {
    if (scheduler.shutdown_for(5s)) {
        return;
    }
    go2cpp_tests::watchdog_abort(
        "scheduler shutdown timed out: a started G lost its wakeup", __FILE__,
        __LINE__);
}

// ---------------------------------------------------------------------------
// 1. 基础语义
// ---------------------------------------------------------------------------

void TestSemantics() {
    go2cpp_tests::announce("RWMutex 读共享 / 写独占");
    RWMutex mutex;
    std::int64_t data = 0;
    std::atomic<int> readers_inside{0};
    std::atomic<int> writers_inside{0};
    std::atomic<int> violations{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 3000; ++i) {
                mutex.RLock();
                if (writers_inside.load(std::memory_order_seq_cst) != 0) {
                    violations.fetch_add(1);
                }
                readers_inside.fetch_add(1);
                const std::int64_t snapshot = data;
                (void)snapshot;
                readers_inside.fetch_sub(1);
                mutex.RUnlock();
            }
        });
    }
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                mutex.Lock();
                if (readers_inside.load(std::memory_order_seq_cst) != 0 ||
                    writers_inside.load(std::memory_order_seq_cst) != 0) {
                    violations.fetch_add(1);
                }
                writers_inside.fetch_add(1);
                ++data;
                writers_inside.fetch_sub(1);
                mutex.Unlock();
            }
        });
    }
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, kJoinTimeout);
    }
    GO2CPP_CHECK(violations.load() == 0);
    GO2CPP_CHECK(data == 2000);
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    GO2CPP_CHECK(mutex.ReaderCount() == 0);
}

// ---------------------------------------------------------------------------
// 2. 超时 / 唤醒回归
// ---------------------------------------------------------------------------

void TestRLockForWakesWhenWriterUnlocks() {
    go2cpp_tests::announce("RLockFor 等待写者时不得丢唤醒");
    RWMutex mutex;
    std::atomic<bool> writer_locked{false};
    std::thread writer([&] {
        mutex.Lock();
        writer_locked.store(true, std::memory_order_release);
        std::this_thread::sleep_for(40ms);
        mutex.Unlock();
    });
    while (!writer_locked.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // 旧实现里 RLockFor 不递增 m_waiting_readers，写者 Unlock 时完全短路，
    // 调用者会一直睡到 2s 超时；正确实现应在写者解锁后立即返回 true。
    const auto begin = std::chrono::steady_clock::now();
    const bool acquired = mutex.RLockFor(2s);
    const double waited_us = ElapsedUs(begin);
    if (acquired) {
        mutex.RUnlock();
    }
    writer.join();
    GO2CPP_CHECK(acquired);
    GO2CPP_CHECK(waited_us < 1'000'000.0);  // 远小于 2s 超时
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    GO2CPP_CHECK(mutex.ReaderCount() == 0);
}

void TestTimeoutDoesNotCorruptState() {
    go2cpp_tests::announce("LockFor/RLockFor 超时不改变状态");
    RWMutex mutex;
    mutex.RLock();
    GO2CPP_CHECK(!mutex.TryLock());
    GO2CPP_CHECK(!mutex.LockFor(5ms));
    GO2CPP_CHECK(mutex.ReaderCount() >= 1);
    mutex.RUnlock();

    GO2CPP_CHECK(mutex.TryLock());
    GO2CPP_CHECK(!mutex.TryRLock());
    const auto begin = std::chrono::steady_clock::now();
    GO2CPP_CHECK(!mutex.RLockFor(5ms));
    GO2CPP_CHECK(ElapsedUs(begin) < 100'000.0);
    mutex.Unlock();

    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    GO2CPP_CHECK(mutex.ReaderCount() == 0);
}

void TestWriterFifoHandoff() {
    go2cpp_tests::announce("写者 FIFO 交接（OS 线程）");
    RWMutex mutex;
    mutex.Lock();  // 压住写锁，让后续写者按顺序排队
    std::mutex order_mutex;
    std::vector<int> order;
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) {
        writers.emplace_back([&, i] {
            mutex.Lock();
            {
                std::lock_guard<std::mutex> lock(order_mutex);
                order.push_back(i);
            }
            mutex.Unlock();
        });
        std::this_thread::sleep_for(5ms);
    }
    GO2CPP_REQUIRE_EVENTUALLY(mutex.HasPendingWriter(), 2s);
    mutex.Unlock();
    for (auto& writer : writers) {
        GO2CPP_JOIN_WITH_WATCHDOG(writer, kJoinTimeout);
    }
    GO2CPP_CHECK(order.size() == 4);
    GO2CPP_CHECK(order == std::vector<int>({0, 1, 2, 3}));
    GO2CPP_CHECK(!mutex.IsWriteLocked());
}

void TestLockForWaitsBehindWriter() {
    go2cpp_tests::announce("LockFor 在写者后面排队并成功交接");
    RWMutex mutex;
    mutex.Lock();
    std::atomic<bool> started{false};
    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        started.store(true, std::memory_order_release);
        acquired.store(mutex.LockFor(2s), std::memory_order_release);
        if (acquired.load()) {
            mutex.Unlock();
        }
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    GO2CPP_REQUIRE_EVENTUALLY(mutex.HasPendingWriter(), 2s);
    std::this_thread::sleep_for(20ms);
    mutex.Unlock();
    GO2CPP_JOIN_WITH_WATCHDOG(waiter, kJoinTimeout);
    GO2CPP_CHECK(acquired.load());
    GO2CPP_CHECK(!mutex.IsWriteLocked());
}

// ---------------------------------------------------------------------------
// 3. 随机并发
// ---------------------------------------------------------------------------

void TestRandomStress() {
    go2cpp_tests::announce("RWMutex 随机并发（随机接口 / 随机超时）");
    RWMutex mutex;
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> reads{0};
    std::atomic<std::int64_t> writes{0};
    std::atomic<std::int64_t> timeouts{0};
    std::atomic<std::int64_t> violations{0};
    std::atomic<std::int64_t> reader_inside{0};
    std::atomic<std::int64_t> writer_inside{0};
    std::int64_t a = 0;
    std::int64_t b = 0;

    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64 rng(0x9E3779B97F4A7C15ULL * (t + 1));
            const auto deadline = std::chrono::steady_clock::now() + 1200ms;
            while (std::chrono::steady_clock::now() < deadline &&
                   !stop.load(std::memory_order_relaxed)) {
                const std::uint64_t roll = rng() % 100;
                if (roll < 60) {
                    mutex.RLock();
                    if (writer_inside.load(std::memory_order_seq_cst) != 0 ||
                        a != b) {
                        violations.fetch_add(1);
                    }
                    reader_inside.fetch_add(1);
                    (void)a;
                    (void)b;
                    reader_inside.fetch_sub(1);
                    mutex.RUnlock();
                    reads.fetch_add(1);
                } else if (roll < 70) {
                    if (mutex.TryRLock()) {
                        if (writer_inside.load(std::memory_order_seq_cst) !=
                            0) {
                            violations.fetch_add(1);
                        }
                        mutex.RUnlock();
                        reads.fetch_add(1);
                    }
                } else if (roll < 85) {
                    mutex.Lock();
                    if (reader_inside.load(std::memory_order_seq_cst) != 0 ||
                        writer_inside.load(std::memory_order_seq_cst) != 0) {
                        violations.fetch_add(1);
                    }
                    writer_inside.fetch_add(1);
                    ++a;
                    ++b;
                    writer_inside.fetch_sub(1);
                    mutex.Unlock();
                    writes.fetch_add(1);
                } else if (roll < 92) {
                    if (mutex.TryLock()) {
                        ++a;
                        ++b;
                        mutex.Unlock();
                        writes.fetch_add(1);
                    }
                } else if (roll < 96) {
                    if (mutex.LockFor(std::chrono::milliseconds(
                            static_cast<int>(rng() % 3) + 1))) {
                        ++a;
                        ++b;
                        mutex.Unlock();
                        writes.fetch_add(1);
                    } else {
                        timeouts.fetch_add(1);
                    }
                } else {
                    if (mutex.RLockFor(std::chrono::milliseconds(
                            static_cast<int>(rng() % 3) + 1))) {
                        mutex.RUnlock();
                        reads.fetch_add(1);
                    } else {
                        timeouts.fetch_add(1);
                    }
                }
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, kJoinTimeout);
    }
    stop.store(true);
    GO2CPP_CHECK(violations.load() == 0);
    GO2CPP_CHECK(a == b);
    GO2CPP_CHECK(reads.load() + writes.load() > 0);
    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    GO2CPP_CHECK(mutex.ReaderCount() == 0);
    std::printf(
        "[rwmutex-opt][random] reads=%lld writes=%lld timeouts=%lld "
        "violations=%lld\n",
        static_cast<long long>(reads.load()),
        static_cast<long long>(writes.load()),
        static_cast<long long>(timeouts.load()),
        static_cast<long long>(violations.load()));
}

// ---------------------------------------------------------------------------
// 4. 五个真实场景
// ---------------------------------------------------------------------------

/// 场景 1：配置热加载 —— 写者整体替换配置，读者必须看到一致的快照。
void TestScenarioConfigHotReload() {
    go2cpp_tests::announce("场景 1：配置热加载");
    RWMutex mutex;
    struct Config {
        std::int64_t values[8]{};
        std::int64_t version{0};
    };
    Config config;
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> torn{0};
    std::atomic<std::int64_t> reads{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                mutex.RLock();
                Config snapshot = config;
                mutex.RUnlock();
                for (int i = 1; i < 8; ++i) {
                    if (snapshot.values[i] != snapshot.values[0]) {
                        torn.fetch_add(1);
                    }
                }
                if (snapshot.values[0] != snapshot.version) {
                    torn.fetch_add(1);
                }
                reads.fetch_add(1);
            }
        });
    }
    for (int version = 1; version <= 200; ++version) {
        mutex.Lock();
        config.version = version;
        for (auto& value : config.values) {
            value = version;
        }
        mutex.Unlock();
        std::this_thread::sleep_for(1ms);
    }
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        GO2CPP_JOIN_WITH_WATCHDOG(reader, kJoinTimeout);
    }
    GO2CPP_CHECK(torn.load() == 0);
    GO2CPP_CHECK(reads.load() > 0);
}

/// 场景 2：连接池 —— 写者扩容/缩容，读者只读快照，大小必须一致。
void TestScenarioConnectionPool() {
    go2cpp_tests::announce("场景 2：连接池容量管理");
    RWMutex mutex;
    std::vector<int> slots;
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> inconsistent{0};
    std::atomic<std::int64_t> acquires{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                mutex.RLock();
                const std::size_t size = slots.size();
                std::int64_t sum = 0;
                for (std::size_t i = 0; i < size; ++i) {
                    sum += slots[i];
                }
                mutex.RUnlock();
                if (size > 64 || sum < 0) {
                    inconsistent.fetch_add(1);
                }
                acquires.fetch_add(1);
            }
        });
    }
    for (int round = 0; round < 300; ++round) {
        mutex.Lock();
        const std::size_t target = static_cast<std::size_t>((round * 7) % 64);
        slots.resize(target);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            slots[i] = static_cast<int>(i);
        }
        mutex.Unlock();
        std::this_thread::sleep_for(500us);
    }
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        GO2CPP_JOIN_WITH_WATCHDOG(reader, kJoinTimeout);
    }
    GO2CPP_CHECK(inconsistent.load() == 0);
    GO2CPP_CHECK(acquires.load() > 0);
}

/// 场景 3：写者饥饿 —— 读者持续短临界区，写者仍须有限时间内拿到锁。
void TestScenarioWriterStarvation() {
    go2cpp_tests::announce("场景 3：写者饥饿（读者持续负载）");
    RWMutex mutex;
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> reader_ops{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 6; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                mutex.RLock();
                std::this_thread::sleep_for(50us);
                mutex.RUnlock();
                reader_ops.fetch_add(1);
            }
        });
    }
    double max_wait_us = 0.0;
    int failures = 0;
    for (int i = 0; i < 100; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        if (!mutex.LockFor(1s)) {
            ++failures;
            continue;
        }
        const double waited_us = ElapsedUs(begin);
        max_wait_us = std::max(max_wait_us, waited_us);
        mutex.Unlock();
    }
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        GO2CPP_JOIN_WITH_WATCHDOG(reader, kJoinTimeout);
    }
    std::printf(
        "[rwmutex-opt][scenario] writer-starvation max_wait_us=%.1f "
        "failures=%d reader_ops=%lld\n",
        max_wait_us, failures,
        static_cast<long long>(reader_ops.load()));
    GO2CPP_CHECK(failures == 0);
    GO2CPP_CHECK(max_wait_us < 500'000.0);
}

/// 场景 4：超时 / 取消风暴 —— 大量带超时的读写请求不得破坏锁状态。
void TestScenarioTimeoutStorm() {
    go2cpp_tests::announce("场景 4：超时/取消风暴");
    RWMutex mutex;
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> ok{0};
    std::atomic<std::int64_t> timed_out{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64 rng(0xD1B54A32D192ED03ULL * (t + 3));
            while (!stop.load(std::memory_order_relaxed)) {
                const auto timeout =
                    std::chrono::milliseconds(static_cast<int>(rng() % 2) + 1);
                if ((rng() % 2) == 0) {
                    if (mutex.RLockFor(timeout)) {
                        mutex.RUnlock();
                        ok.fetch_add(1);
                    } else {
                        timed_out.fetch_add(1);
                    }
                } else {
                    if (mutex.LockFor(timeout)) {
                        mutex.Unlock();
                        ok.fetch_add(1);
                    } else {
                        timed_out.fetch_add(1);
                    }
                }
            }
        });
    }
    // 背景读写者制造真实的竞争与排队。
    std::thread background([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            mutex.Lock();
            std::this_thread::sleep_for(20us);
            mutex.Unlock();
            mutex.RLock();
            mutex.RUnlock();
        }
    });
    std::this_thread::sleep_for(800ms);
    stop.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, kJoinTimeout);
    }
    GO2CPP_JOIN_WITH_WATCHDOG(background, kJoinTimeout);
    GO2CPP_CHECK(ok.load() + timed_out.load() > 0);
    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    GO2CPP_CHECK(mutex.ReaderCount() == 0);
    std::printf("[rwmutex-opt][scenario] timeout-storm ok=%lld timeout=%lld\n",
                static_cast<long long>(ok.load()),
                static_cast<long long>(timed_out.load()));
}

/// 场景 5：Fiber 与 OS 线程混合 —— Fiber 写者排在 OS 线程读者之后，
///         交接后由 Fiber 持有写锁，读者继续等待。
void TestScenarioFiberThreadHandoff() {
    go2cpp_tests::announce("场景 5：Fiber + OS 线程混合交接");
    go2cpp::Scheduler scheduler(MakeSchedulerConfig(2));
    scheduler.start();
    RWMutex mutex;
    std::atomic<std::int64_t> counter{0};
    std::atomic<bool> fiber_done{false};

    // 主线程先持写锁：Fiber 写者排在它后面，读者线程也被写者优先级挡住。
    mutex.Lock();
    auto fiber_writer = scheduler.spawn([&] {
        mutex.Lock();
        counter.fetch_add(100);
        mutex.Unlock();
        fiber_done.store(true, std::memory_order_release);
    });
    GO2CPP_REQUIRE_EVENTUALLY(
        fiber_writer->state() == go2cpp::GState::kWaiting, 2s);

    std::atomic<bool> reader_started{false};
    std::atomic<bool> reader_done{false};
    std::thread reader([&] {
        reader_started.store(true, std::memory_order_release);
        mutex.RLock();
        counter.fetch_add(1);
        mutex.RUnlock();
        reader_done.store(true, std::memory_order_release);
    });
    while (!reader_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(30ms);
    // 写者优先：读者此时必须仍在等，计数器还没有 +1。
    GO2CPP_CHECK(counter.load() == 0);
    // 释放写锁：门先交接到队首 Fiber 写者，之后才放行线程读者。
    mutex.Unlock();
    for (int i = 0; i < 200 && !fiber_done.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(1ms);
    }
    GO2CPP_JOIN_WITH_WATCHDOG(reader, kJoinTimeout);
    GO2CPP_CHECK(fiber_done.load());
    GO2CPP_CHECK(reader_done.load());
    GO2CPP_CHECK(counter.load() == 101);
    GO2CPP_CHECK(fiber_writer->wait_for(2s));
    ShutdownOrReportLostWakeup(scheduler);
}

/// 场景 6：90% 读 / 10% 写混合吞吐下的数据一致性（8 线程）。
void TestScenarioReadMostlyInvariant() {
    go2cpp_tests::announce("场景 6：90/10 混合负载不变式");
    RWMutex mutex;
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> reads{0};
    std::atomic<std::int64_t> writes{0};
    std::atomic<std::int64_t> torn{0};
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64 rng(0xA24BAED4963EE407ULL * (t + 11));
            const auto deadline = std::chrono::steady_clock::now() + 800ms;
            while (std::chrono::steady_clock::now() < deadline) {
                if ((rng() % 10) == 0) {
                    mutex.Lock();
                    ++x;
                    ++y;
                    mutex.Unlock();
                    writes.fetch_add(1);
                } else {
                    mutex.RLock();
                    if (x != y) {
                        torn.fetch_add(1);
                    }
                    mutex.RUnlock();
                    reads.fetch_add(1);
                }
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, kJoinTimeout);
    }
    GO2CPP_CHECK(torn.load() == 0);
    GO2CPP_CHECK(x == y);
    GO2CPP_CHECK(reads.load() > 0);
    std::printf("[rwmutex-opt][scenario] read-mostly reads=%lld writes=%lld\n",
                static_cast<long long>(reads.load()),
                static_cast<long long>(writes.load()));
}

}  // namespace

int main() {
    TestSemantics();
    TestRLockForWakesWhenWriterUnlocks();
    TestTimeoutDoesNotCorruptState();
    TestWriterFifoHandoff();
    TestLockForWaitsBehindWriter();
    TestRandomStress();

    TestScenarioConfigHotReload();
    TestScenarioConnectionPool();
    TestScenarioWriterStarvation();
    TestScenarioTimeoutStorm();
    TestScenarioFiberThreadHandoff();
    TestScenarioReadMostlyInvariant();

    const int failures =
        go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::cerr << "[rwmutex-opt] failures=" << failures << '\n';
        return 1;
    }
    std::cout << "[rwmutex-opt] all checks passed\n";
    return 0;
}
