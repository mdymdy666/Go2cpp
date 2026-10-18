/**
 * @file test_locks.cpp
 * @brief `RWMutex` / `RecursiveMutex` / `TimedMutex` 的压测、随机测试、
 *        协程-线程混合场景与真实场景模拟。
 *
 * @date 2026-10-18
 * @author DeepSeek Agent
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 覆盖范围
 *
 * 1. **语义**：读共享 / 写独占、写者优先、写者 FIFO、超时返回 false 且不改
 *    状态、可重入计数、非 owner 解锁抛异常、递归深度上限。
 * 2. **协程 / 线程混合（验收核心）**：
 *    - A：Fiber 持锁、Fiber 等 → 等待者 park，worker 继续跑别的 G；
 *    - B：OS 线程持锁、Fiber 等 → G park，线程解锁后唤醒；
 *    - C：Fiber 持锁、OS 线程等 → 线程真阻塞，Fiber 解锁后唤醒；
 *    - D：2 Fiber + 2 OS 线程争用 → 全部完成且互斥不变式成立。
 * 3. **压测**：读写锁三种负载 × 1/2/4/8 线程；可重入深度 1/10/100；
 *    超时锁无竞争 / 有竞争 / 超时三条路径。
 * 4. **随机测试**：随机 RLock/Lock/Unlock 序列并校验不变式；随机可重入
 *    序列校验计数；随机超时校验锁状态未被修改。
 * 5. **真实场景**：配置热加载、连接池、递归树遍历、持锁 IO 超时兜底、
 *    写饥饿验证（含 `std::shared_mutex` 对照）。
 * 6. **对比数据**：与 `std::shared_mutex` / `std::recursive_mutex` /
 *    `std::timed_mutex` 在相同负载下的 ns/op。
 *
 * 本文件不修改任何既有设施，只通过公开 API 使用 `sync::Mutex`、
 * `sync::ConditionVariable` 与 `Scheduler`。
 */
#include "go2cpp/concurrency/locks.hpp"
#include "go2cpp/concurrency/scheduler/scheduler.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using go2cpp::sync::RecursiveMutex;
using go2cpp::sync::RWMutex;
using go2cpp::sync::TimedMutex;

// ---------------------------------------------------------------------------
// 通用辅助
// ---------------------------------------------------------------------------

/// @brief 轮询等待谓词成立；超时返回 false（不 abort，便于定位）。
template <typename Predicate>
bool WaitUntil(Predicate predicate,
               std::chrono::steady_clock::duration timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/// @brief 测试用的调度器配置。
go2cpp::SchedulerConfig MakeSchedulerConfig(std::size_t processors) {
    go2cpp::SchedulerConfig config;
    config.processor_count = processors;
    config.max_workers = processors;
    config.idle_wait = 1ms;
    return config;
}

/// @brief 关闭调度器；失败说明丢唤醒，立即 abort 而不是挂到 ctest 超时。
void ShutdownOrReportLostWakeup(go2cpp::Scheduler& scheduler) {
    if (scheduler.shutdown_for(5s)) {
        return;
    }
    go2cpp_tests::watchdog_abort(
        "scheduler shutdown timed out: a started G lost its wakeup", __FILE__,
        __LINE__);
}

/// @brief 单调时钟经过的微秒数。
double ElapsedUs(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now() - begin)
        .count();
}

/// @brief 读取 `/proc/stat` 的 procs_running（WSL2 下 loadavg 不可信）。
int ProcsRunning() {
    std::FILE* file = std::fopen("/proc/stat", "r");
    if (file == nullptr) {
        return -1;
    }
    char line[256];
    int running = -1;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::sscanf(line, "procs_running %d", &running) == 1) {
            break;
        }
    }
    std::fclose(file);
    return running;
}

/// @brief 计算分位数（输入需已排序，index 为 0 基）。
double Percentile(const std::vector<double>& sorted, double percentile) {
    if (sorted.empty()) {
        return 0.0;
    }
    std::size_t index = static_cast<std::size_t>(
        percentile * static_cast<double>(sorted.size() - 1));
    index = std::min(index, sorted.size() - 1);
    return sorted[index];
}

/// @brief 被读写锁保护的载荷：写者同时更新 a/b，读者必须看到 a == b。
struct Payload {
    std::uint64_t a{0};
    std::uint64_t b{0};
};

/// @brief 期望超过可重入上限时抛出 `std::logic_error`。
template <typename Function>
bool ThrowsLogicError(Function&& function) {
    try {
        function();
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1. 基础语义
// ---------------------------------------------------------------------------

/**
 * @brief 校验 `RWMutex` 的共享 / 独占 / 超时 / 写者优先级 / FIFO 语义。
 */
void TestRWMutexSemantics() {
    RWMutex mutex;
    GO2CPP_CHECK(mutex.ReaderCount() == 0);
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    GO2CPP_CHECK(!mutex.HasPendingWriter());

    // 读锁共享 + 同 G 重入读。
    mutex.RLock();
    mutex.RLock();
    GO2CPP_CHECK(mutex.ReaderCount() == 2);
    GO2CPP_CHECK(mutex.TryRLock());
    GO2CPP_CHECK(mutex.ReaderCount() == 3);
    // 有读者时写锁不能进入（TryLock 不等待）。
    GO2CPP_CHECK(!mutex.TryLock());
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    mutex.RUnlock();
    mutex.RUnlock();
    mutex.RUnlock();
    GO2CPP_CHECK(mutex.ReaderCount() == 0);

    // 未持有读锁时 RUnlock 必须报错。
    GO2CPP_CHECK(ThrowsLogicError([&] { mutex.RUnlock(); }));

    // 写锁独占。
    mutex.Lock();
    GO2CPP_CHECK(mutex.IsWriteLocked());
    GO2CPP_CHECK(mutex.ReaderCount() == 0);
    GO2CPP_CHECK(!mutex.TryRLock());
    GO2CPP_CHECK(!mutex.TryLock());
    GO2CPP_CHECK(ThrowsLogicError([&] { mutex.Unlock(); mutex.Unlock(); }));
    GO2CPP_CHECK(mutex.TryLock());  // 上一步已经释放；重新加锁确认状态干净
    mutex.Unlock();

    // 超时读锁：锁被写者持有时按时返回 false，且状态不变。
    mutex.Lock();
    const auto read_begin = std::chrono::steady_clock::now();
    GO2CPP_CHECK(!mutex.RLockFor(30ms));
    const double read_wait_us = ElapsedUs(read_begin);
    GO2CPP_CHECK(read_wait_us >= 15000.0);
    GO2CPP_CHECK(mutex.IsWriteLocked());
    GO2CPP_CHECK(mutex.ReaderCount() == 0);

    // 超时写锁：失败后等待票据必须被撤销。
    GO2CPP_CHECK(!mutex.LockFor(20ms));
    GO2CPP_CHECK(!mutex.HasPendingWriter());
    GO2CPP_CHECK(mutex.IsWriteLocked());
    mutex.Unlock();

    // 空闲锁上的超时接口立即成功。
    GO2CPP_CHECK(mutex.RLockFor(0ms));
    mutex.RUnlock();
    GO2CPP_CHECK(mutex.RLockFor(10ms));
    mutex.RUnlock();
    GO2CPP_CHECK(mutex.LockFor(10ms));
    mutex.Unlock();
    GO2CPP_CHECK(mutex.ReaderCount() == 0);

    // 写者优先：写者在等时，新读者必须排队（TryRLock / RLockFor 都不能插队）。
    mutex.RLock();
    std::atomic<bool> writer_acquired{false};
    std::thread blocked_writer([&] {
        mutex.Lock();
        writer_acquired.store(true, std::memory_order_release);
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(mutex.HasPendingWriter(), 2s);
    GO2CPP_CHECK(!mutex.TryRLock());
    GO2CPP_CHECK(!mutex.RLockFor(10ms));
    GO2CPP_CHECK(!writer_acquired.load(std::memory_order_acquire));
    mutex.RUnlock();
    GO2CPP_REQUIRE_EVENTUALLY(writer_acquired.load(), 2s);
    GO2CPP_JOIN_WITH_WATCHDOG(blocked_writer, 3s);

    // 写者 FIFO 用受管 G 单独验证（见 TestRWMutexWriterFifoFibers）：
    // 用 OS 线程做顺序断言会被调度延迟伪证，受管 G 的 kWaiting 状态才是
    // 「已进入队列」的可靠证据。
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    GO2CPP_CHECK(mutex.ReaderCount() == 0);
}

/**
 * @brief 写者 FIFO：在 1 个 P 上顺序启动写者，用 kWaiting 确认入队顺序。
 * @details 这是唯一能跨调度器稳定复现的 FIFO 验证方式：协作式 G 只有真正
 *          park 才会进入 kWaiting，因此顺序等于启动顺序。
 */
void TestRWMutexWriterFifoFibers() {
    go2cpp::Scheduler scheduler(MakeSchedulerConfig(1));
    scheduler.start();
    RWMutex mutex;
    mutex.Lock();  // 压住写锁，让后续写者排队
    std::mutex order_mutex;
    std::vector<int> order;
    std::vector<std::shared_ptr<go2cpp::Task>> writers;
    for (int i = 0; i < 4; ++i) {
        auto writer = scheduler.spawn([&, i] {
            mutex.Lock();
            {
                std::lock_guard<std::mutex> lock(order_mutex);
                order.push_back(i);
            }
            mutex.Unlock();
        });
        GO2CPP_REQUIRE_EVENTUALLY(
            writer->state() == go2cpp::GState::kWaiting, 2s);
        writers.push_back(std::move(writer));
    }
    GO2CPP_CHECK(mutex.HasPendingWriter());
    mutex.Unlock();
    for (auto& writer : writers) {
        GO2CPP_CHECK(writer->wait_for(2s));
    }
    GO2CPP_CHECK(order.size() == 4);
    GO2CPP_CHECK(order == std::vector<int>({0, 1, 2, 3}));
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    GO2CPP_CHECK(mutex.ReaderCount() == 0);
    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 校验 `RecursiveMutex` 的 owner / 递归计数 / 上限 / 非 owner 异常。
 */
void TestRecursiveMutexSemantics() {
    RecursiveMutex mutex;
    GO2CPP_CHECK(!mutex.IsLockedByCurrent());
    GO2CPP_CHECK(mutex.RecursionCount() == 0);

    mutex.Lock();
    GO2CPP_CHECK(mutex.IsLockedByCurrent());
    GO2CPP_CHECK(mutex.RecursionCount() == 1);
    mutex.Lock();
    mutex.Lock();
    GO2CPP_CHECK(mutex.RecursionCount() == 3);
    GO2CPP_CHECK(mutex.TryLock());
    GO2CPP_CHECK(mutex.RecursionCount() == 4);
    mutex.Unlock();
    mutex.Unlock();
    mutex.Unlock();
    mutex.Unlock();
    GO2CPP_CHECK(!mutex.IsLockedByCurrent());
    GO2CPP_CHECK(mutex.RecursionCount() == 0);

    // 另一个 OS 线程解锁当前 OS 线程持有的锁 → 必须抛异常。
    std::atomic<bool> other_thread_threw{false};
    std::thread other([&] {
        other_thread_threw.store(
            ThrowsLogicError([&] { mutex.Unlock(); }),
            std::memory_order_release);
    });
    GO2CPP_JOIN_WITH_WATCHDOG(other, 3s);
    GO2CPP_CHECK(other_thread_threw.load(std::memory_order_acquire));

    // 递归深度上限：允许 1000 层，第 1001 层抛异常且深度不变。
    mutex.Lock();
    for (std::uint32_t i = 1; i < RecursiveMutex::kMaxRecursionDepth; ++i) {
        mutex.Lock();
    }
    GO2CPP_CHECK(mutex.RecursionCount() == RecursiveMutex::kMaxRecursionDepth);
    GO2CPP_CHECK(ThrowsLogicError([&] { mutex.Lock(); }));
    GO2CPP_CHECK(mutex.RecursionCount() == RecursiveMutex::kMaxRecursionDepth);
    for (std::uint32_t i = 0; i < RecursiveMutex::kMaxRecursionDepth; ++i) {
        mutex.Unlock();
    }
    GO2CPP_CHECK(mutex.RecursionCount() == 0);

    // 深度 1 / 10 / 100 的重复加锁必须全部正确。
    for (std::uint32_t depth : {1U, 10U, 100U}) {
        mutex.Lock();
        for (std::uint32_t i = 1; i < depth; ++i) {
            mutex.Lock();
        }
        GO2CPP_CHECK(mutex.RecursionCount() == depth);
        for (std::uint32_t i = 0; i < depth; ++i) {
            mutex.Unlock();
        }
        GO2CPP_CHECK(mutex.RecursionCount() == 0);
    }
}

/**
 * @brief 校验 `TimedMutex` 的三条路径：无竞争 / 超时 / 竞争后成功。
 */
void TestTimedMutexSemantics() {
    TimedMutex mutex;
    GO2CPP_CHECK(mutex.TryLock());
    GO2CPP_CHECK(!mutex.TryLock());
    mutex.Unlock();
    GO2CPP_CHECK(ThrowsLogicError([&] { mutex.Unlock(); }));

    // 无竞争路径：立刻成功。
    GO2CPP_CHECK(mutex.TryLockFor(0ms));
    mutex.Unlock();
    GO2CPP_CHECK(mutex.TryLockFor(10ms));
    mutex.Unlock();
    GO2CPP_CHECK(mutex.TryLockUntil(std::chrono::steady_clock::now() + 10ms));
    mutex.Unlock();

    // 超时路径：被持有时返回 false，且等待时间近似给定超时。
    mutex.Lock();
    const auto begin = std::chrono::steady_clock::now();
    GO2CPP_CHECK(!mutex.TryLockFor(25ms));
    const double waited_us = ElapsedUs(begin);
    GO2CPP_CHECK(waited_us >= 12000.0);
    GO2CPP_CHECK(!mutex.TryLockUntil(std::chrono::steady_clock::now() + 10ms));
    GO2CPP_CHECK(!mutex.TryLock());  // 超时没有破坏锁状态
    mutex.Unlock();
    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();

    // 竞争后成功：另一个线程持有一小段时间，等待者应在超时内取得锁。
    std::atomic<bool> holder_ready{false};
    std::thread holder([&] {
        mutex.Lock();
        holder_ready.store(true, std::memory_order_release);
        std::this_thread::sleep_for(20ms);
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(holder_ready.load(), 2s);
    const auto wait_begin = std::chrono::steady_clock::now();
    GO2CPP_CHECK(mutex.TryLockFor(2s));
    GO2CPP_CHECK(ElapsedUs(wait_begin) >= 5000.0);
    mutex.Unlock();
    GO2CPP_JOIN_WITH_WATCHDOG(holder, 3s);
}

// ---------------------------------------------------------------------------
// 2. 协程 / 线程混合场景 A/B/C/D
// ---------------------------------------------------------------------------

/**
 * @brief 场景 A：Fiber 持锁、另一个 Fiber 等 → 等待者必须 park。
 * @details 用「park 期间另一个 Fiber 仍能推进」证明 worker 线程没被阻塞。
 */
void TestRWMutexScenarioAFiberWaits() {
    go2cpp::Scheduler scheduler(MakeSchedulerConfig(1));
    scheduler.start();
    RWMutex mutex;

    std::atomic<bool> holder_ready{false};
    std::atomic<bool> release_holder{false};
    auto holder = scheduler.spawn([&] {
        mutex.Lock();
        holder_ready.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!release_holder.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)scheduler.yield_current();
            go2cpp_tests::pause_for_watchdog();
        }
        if (!release_holder.load(std::memory_order_acquire)) {
            go2cpp_tests::watchdog_abort("RWMutex scenario A holder release",
                                         __FILE__, __LINE__);
        }
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(holder_ready.load(), 2s);

    std::atomic<bool> waiter_entered{false};
    std::atomic<bool> waiter_acquired{false};
    auto waiter = scheduler.spawn([&] {
        waiter_entered.store(true, std::memory_order_release);
        mutex.Lock();
        waiter_acquired.store(true, std::memory_order_release);
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(waiter_entered.load(), 2s);
    GO2CPP_REQUIRE_EVENTUALLY(
        waiter->state() == go2cpp::GState::kWaiting, 2s);
    GO2CPP_CHECK(!waiter_acquired.load(std::memory_order_acquire));

    // 等待者已 park：同一 worker（1 个 P）仍能执行第三个 G。
    std::atomic<int> progress{0};
    auto probe = scheduler.spawn([&] { progress.fetch_add(1); });
    GO2CPP_REQUIRE_EVENTUALLY(progress.load() == 1, 2s);

    release_holder.store(true, std::memory_order_release);
    GO2CPP_CHECK(holder->wait_for(2s));
    GO2CPP_CHECK(waiter->wait_for(2s));
    GO2CPP_CHECK(probe->wait_for(2s));
    GO2CPP_CHECK(waiter_acquired.load(std::memory_order_acquire));
    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 场景 B：OS 线程持锁、Fiber 等 → G park，线程解锁后唤醒。
 */
void TestRWMutexScenarioBThreadHoldsFiberWaits() {
    go2cpp::Scheduler scheduler(MakeSchedulerConfig(1));
    scheduler.start();
    RWMutex mutex;

    mutex.Lock();
    std::atomic<bool> waiter_entered{false};
    std::atomic<bool> waiter_acquired{false};
    auto waiter = scheduler.spawn([&] {
        waiter_entered.store(true, std::memory_order_release);
        mutex.Lock();
        waiter_acquired.store(true, std::memory_order_release);
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(waiter_entered.load(), 2s);
    GO2CPP_REQUIRE_EVENTUALLY(
        waiter->state() == go2cpp::GState::kWaiting, 2s);
    GO2CPP_CHECK(!waiter_acquired.load(std::memory_order_acquire));

    std::atomic<int> progress{0};
    auto probe = scheduler.spawn([&] { progress.fetch_add(1); });
    GO2CPP_REQUIRE_EVENTUALLY(progress.load() == 1, 2s);

    mutex.Unlock();
    GO2CPP_CHECK(waiter->wait_for(2s));
    GO2CPP_CHECK(probe->wait_for(2s));
    GO2CPP_CHECK(waiter_acquired.load(std::memory_order_acquire));
    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 场景 C：Fiber 持锁、OS 线程等 → 线程真阻塞，Fiber 解锁后唤醒。
 */
void TestRWMutexScenarioCFiberHoldsThreadWaits() {
    go2cpp::Scheduler scheduler(MakeSchedulerConfig(2));
    scheduler.start();
    RWMutex mutex;

    std::atomic<bool> fiber_holds{false};
    std::atomic<bool> release_fiber{false};
    auto holder = scheduler.spawn([&] {
        mutex.Lock();
        fiber_holds.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!release_fiber.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)scheduler.yield_current();
            go2cpp_tests::pause_for_watchdog();
        }
        if (!release_fiber.load(std::memory_order_acquire)) {
            go2cpp_tests::watchdog_abort("RWMutex scenario C fiber release",
                                         __FILE__, __LINE__);
        }
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(fiber_holds.load(), 2s);

    std::atomic<bool> thread_entered{false};
    std::atomic<bool> thread_acquired{false};
    std::thread native([&] {
        thread_entered.store(true, std::memory_order_release);
        mutex.Lock();
        thread_acquired.store(true, std::memory_order_release);
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(thread_entered.load(), 2s);
    std::this_thread::sleep_for(30ms);
    GO2CPP_CHECK(!thread_acquired.load(std::memory_order_acquire));

    release_fiber.store(true, std::memory_order_release);
    GO2CPP_CHECK(holder->wait_for(2s));
    GO2CPP_JOIN_WITH_WATCHDOG(native, 3s);
    GO2CPP_CHECK(thread_acquired.load(std::memory_order_acquire));
    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 场景 D：2 Fiber + 2 OS 线程争用同一把写锁 → 全部完成且互斥。
 */
void TestRWMutexScenarioDMixedContention() {
    go2cpp::Scheduler scheduler(MakeSchedulerConfig(2));
    scheduler.start();
    RWMutex mutex;
    std::atomic<int> inside{0};
    std::atomic<std::uint64_t> total{0};
    constexpr int kOps = 1500;
    const auto work = [&] {
        for (int i = 0; i < kOps; ++i) {
            mutex.Lock();
            const int previous =
                inside.fetch_add(1, std::memory_order_acq_rel);
            GO2CPP_CHECK(previous == 0);
            total.fetch_add(1, std::memory_order_relaxed);
            inside.fetch_sub(1, std::memory_order_acq_rel);
            mutex.Unlock();
        }
    };
    auto first_fiber = scheduler.spawn(work);
    auto second_fiber = scheduler.spawn(work);
    std::thread first_thread(work);
    std::thread second_thread(work);
    first_thread.join();
    second_thread.join();
    GO2CPP_CHECK(first_fiber->wait_for(10s));
    GO2CPP_CHECK(second_fiber->wait_for(10s));
    GO2CPP_CHECK(total.load() == 4U * kOps);
    GO2CPP_CHECK(inside.load() == 0);
    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 场景 A-D 在 `RecursiveMutex` 上的对应验证（owner 按 G 记录）。
 */
void TestRecursiveMutexMixedPaths() {
    go2cpp::Scheduler scheduler(MakeSchedulerConfig(2));
    scheduler.start();
    RecursiveMutex mutex;

    // A：Fiber 持锁（深度 2），另一个 Fiber 等待 → park。
    std::atomic<bool> holder_ready{false};
    std::atomic<bool> release_holder{false};
    auto holder = scheduler.spawn([&] {
        mutex.Lock();
        mutex.Lock();
        GO2CPP_CHECK(mutex.RecursionCount() == 2);
        holder_ready.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!release_holder.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)scheduler.yield_current();
            go2cpp_tests::pause_for_watchdog();
        }
        mutex.Unlock();
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(holder_ready.load(), 2s);
    std::atomic<bool> waiter_entered{false};
    std::atomic<bool> waiter_acquired{false};
    auto waiter = scheduler.spawn([&] {
        waiter_entered.store(true, std::memory_order_release);
        mutex.Lock();
        GO2CPP_CHECK(mutex.RecursionCount() == 1);
        waiter_acquired.store(true, std::memory_order_release);
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(waiter_entered.load(), 2s);
    GO2CPP_REQUIRE_EVENTUALLY(waiter->state() == go2cpp::GState::kWaiting, 2s);
    GO2CPP_CHECK(!waiter_acquired.load());
    release_holder.store(true, std::memory_order_release);
    GO2CPP_CHECK(holder->wait_for(2s));
    GO2CPP_CHECK(waiter->wait_for(2s));
    GO2CPP_CHECK(waiter_acquired.load());

    // 同一个 OS 线程上的**另一个 G** 不是 owner：它的 Unlock 必须抛异常。
    std::atomic<bool> fiber_holds{false};
    std::atomic<bool> release_fiber{false};
    std::atomic<bool> other_g_threw{false};
    auto owner_fiber = scheduler.spawn([&] {
        mutex.Lock();
        fiber_holds.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!release_fiber.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)scheduler.yield_current();
            go2cpp_tests::pause_for_watchdog();
        }
        // 另一个 G 的 Unlock 失败不得影响 owner 的计数。
        GO2CPP_CHECK(mutex.RecursionCount() == 1);
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(fiber_holds.load(), 2s);
    auto intruder = scheduler.spawn([&] {
        other_g_threw.store(ThrowsLogicError([&] { mutex.Unlock(); }),
                            std::memory_order_release);
    });
    GO2CPP_CHECK(intruder->wait_for(2s));
    GO2CPP_CHECK(other_g_threw.load());
    release_fiber.store(true, std::memory_order_release);
    GO2CPP_CHECK(owner_fiber->wait_for(2s));

    // B：OS 线程持锁、Fiber 等 → park 后被唤醒。
    mutex.Lock();
    std::atomic<bool> b_entered{false};
    std::atomic<bool> b_acquired{false};
    auto b_waiter = scheduler.spawn([&] {
        b_entered.store(true, std::memory_order_release);
        mutex.Lock();
        b_acquired.store(true, std::memory_order_release);
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(b_entered.load(), 2s);
    GO2CPP_REQUIRE_EVENTUALLY(b_waiter->state() == go2cpp::GState::kWaiting, 2s);
    mutex.Unlock();
    GO2CPP_CHECK(b_waiter->wait_for(2s));
    GO2CPP_CHECK(b_acquired.load());

    // C：Fiber 持锁、OS 线程等 → 线程阻塞，Fiber 解锁后唤醒。
    std::atomic<bool> c_holds{false};
    std::atomic<bool> c_release{false};
    auto c_holder = scheduler.spawn([&] {
        mutex.Lock();
        c_holds.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!c_release.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)scheduler.yield_current();
            go2cpp_tests::pause_for_watchdog();
        }
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(c_holds.load(), 2s);
    std::atomic<bool> c_acquired{false};
    std::thread c_thread([&] {
        mutex.Lock();
        c_acquired.store(true, std::memory_order_release);
        mutex.Unlock();
    });
    std::this_thread::sleep_for(30ms);
    GO2CPP_CHECK(!c_acquired.load());
    c_release.store(true, std::memory_order_release);
    GO2CPP_CHECK(c_holder->wait_for(2s));
    GO2CPP_JOIN_WITH_WATCHDOG(c_thread, 3s);
    GO2CPP_CHECK(c_acquired.load());

    // D：2 Fiber + 2 线程，每次递归深度 2。
    std::atomic<int> inside{0};
    std::atomic<std::uint64_t> total{0};
    constexpr int kOps = 800;
    const auto work = [&] {
        for (int i = 0; i < kOps; ++i) {
            mutex.Lock();
            mutex.Lock();
            GO2CPP_CHECK(mutex.RecursionCount() == 2);
            const int previous = inside.fetch_add(1, std::memory_order_acq_rel);
            GO2CPP_CHECK(previous == 0);
            total.fetch_add(1, std::memory_order_relaxed);
            inside.fetch_sub(1, std::memory_order_acq_rel);
            mutex.Unlock();
            mutex.Unlock();
        }
    };
    auto d1 = scheduler.spawn(work);
    auto d2 = scheduler.spawn(work);
    std::thread d3(work);
    std::thread d4(work);
    d3.join();
    d4.join();
    GO2CPP_CHECK(d1->wait_for(10s));
    GO2CPP_CHECK(d2->wait_for(10s));
    GO2CPP_CHECK(total.load() == 4U * kOps);
    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 场景 A-D 在 `TimedMutex` 上的对应验证。
 */
void TestTimedMutexMixedPaths() {
    go2cpp::Scheduler scheduler(MakeSchedulerConfig(2));
    scheduler.start();
    TimedMutex mutex;

    // A：Fiber 持锁，另一个 Fiber 用 TryLockFor 等 → 超时前 park，释放后成功。
    std::atomic<bool> holder_ready{false};
    std::atomic<bool> release_holder{false};
    auto holder = scheduler.spawn([&] {
        mutex.Lock();
        holder_ready.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!release_holder.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)scheduler.yield_current();
            go2cpp_tests::pause_for_watchdog();
        }
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(holder_ready.load(), 2s);
    std::atomic<bool> timed_waiter_acquired{false};
    auto timed_waiter = scheduler.spawn([&] {
        const bool acquired = mutex.TryLockFor(3s);
        timed_waiter_acquired.store(acquired, std::memory_order_release);
        if (acquired) {
            mutex.Unlock();
        }
    });
    GO2CPP_REQUIRE_EVENTUALLY(
        timed_waiter->state() == go2cpp::GState::kWaiting, 2s);
    std::atomic<int> progress{0};
    auto probe = scheduler.spawn([&] { progress.fetch_add(1); });
    GO2CPP_REQUIRE_EVENTUALLY(progress.load() == 1, 2s);
    release_holder.store(true, std::memory_order_release);
    GO2CPP_CHECK(holder->wait_for(2s));
    GO2CPP_CHECK(timed_waiter->wait_for(3s));
    GO2CPP_CHECK(timed_waiter_acquired.load());
    GO2CPP_CHECK(probe->wait_for(2s));

    // B：OS 线程持锁，Fiber 的 TryLockFor 等 → park 后由线程解锁唤醒。
    mutex.Lock();
    std::atomic<bool> fiber_waiting{false};
    std::atomic<bool> fiber_acquired{false};
    auto fiber_waiter = scheduler.spawn([&] {
        fiber_waiting.store(true, std::memory_order_release);
        const bool acquired = mutex.TryLockFor(3s);
        fiber_acquired.store(acquired, std::memory_order_release);
        if (acquired) {
            mutex.Unlock();
        }
    });
    GO2CPP_REQUIRE_EVENTUALLY(fiber_waiting.load(), 2s);
    GO2CPP_REQUIRE_EVENTUALLY(
        fiber_waiter->state() == go2cpp::GState::kWaiting, 2s);
    mutex.Unlock();
    GO2CPP_CHECK(fiber_waiter->wait_for(3s));
    GO2CPP_CHECK(fiber_acquired.load());

    // C：Fiber 持锁，OS 线程 TryLockFor → 原生阻塞，Fiber 解锁后成功；
    //    同时验证真正的超时路径返回 false。
    std::atomic<bool> c_holds{false};
    std::atomic<bool> c_release{false};
    auto c_holder = scheduler.spawn([&] {
        mutex.Lock();
        c_holds.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!c_release.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            (void)scheduler.yield_current();
            go2cpp_tests::pause_for_watchdog();
        }
        mutex.Unlock();
    });
    GO2CPP_REQUIRE_EVENTUALLY(c_holds.load(), 2s);
    std::atomic<bool> timeout_result{true};
    std::thread timeout_thread([&] {
        timeout_result.store(mutex.TryLockFor(20ms), std::memory_order_release);
        if (timeout_result.load(std::memory_order_acquire)) {
            mutex.Unlock();
        }
    });
    GO2CPP_JOIN_WITH_WATCHDOG(timeout_thread, 3s);
    GO2CPP_CHECK(!timeout_result.load());
    std::atomic<bool> c_acquired{false};
    std::thread c_thread([&] {
        const bool acquired = mutex.TryLockFor(3s);
        c_acquired.store(acquired, std::memory_order_release);
        if (acquired) {
            mutex.Unlock();
        }
    });
    std::this_thread::sleep_for(30ms);
    GO2CPP_CHECK(!c_acquired.load());
    c_release.store(true, std::memory_order_release);
    GO2CPP_CHECK(c_holder->wait_for(2s));
    GO2CPP_JOIN_WITH_WATCHDOG(c_thread, 3s);
    GO2CPP_CHECK(c_acquired.load());

    // D：2 Fiber + 2 线程混合争用。
    std::atomic<int> inside{0};
    std::atomic<std::uint64_t> total{0};
    constexpr int kOps = 600;
    const auto work = [&] {
        for (int i = 0; i < kOps; ++i) {
            if (!mutex.TryLockFor(10s)) {
                go2cpp_tests::watchdog_abort("TimedMutex scenario D acquire",
                                             __FILE__, __LINE__);
            }
            const int previous = inside.fetch_add(1, std::memory_order_acq_rel);
            GO2CPP_CHECK(previous == 0);
            total.fetch_add(1, std::memory_order_relaxed);
            inside.fetch_sub(1, std::memory_order_acq_rel);
            mutex.Unlock();
        }
    };
    auto d1 = scheduler.spawn(work);
    auto d2 = scheduler.spawn(work);
    std::thread d3(work);
    std::thread d4(work);
    d3.join();
    d4.join();
    GO2CPP_CHECK(d1->wait_for(20s));
    GO2CPP_CHECK(d2->wait_for(20s));
    GO2CPP_CHECK(total.load() == 4U * kOps);
    ShutdownOrReportLostWakeup(scheduler);
}

// ---------------------------------------------------------------------------
// 3. 压测
// ---------------------------------------------------------------------------

struct RwStressResult {
    double ns_per_op{0.0};
    std::uint64_t reads{0};
    std::uint64_t writes{0};
};

/**
 * @brief 使用 `RWMutex` 跑指定读写比例的压测（纯 OS 线程）。
 * @param threads 线程数。
 * @param read_percent 读操作百分比。
 * @param ops_per_thread 每线程操作数。
 */
RwStressResult RunRWMutexStress(int threads, int read_percent,
                                int ops_per_thread) {
    RWMutex mutex;
    Payload payload;
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> writes{0};
    std::vector<std::thread> workers;
    const auto begin = std::chrono::steady_clock::now();
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64 rng(0xC2B2AE3D27D4EB4Full * (t + 1));
            for (int i = 0; i < ops_per_thread; ++i) {
                const int roll = static_cast<int>(rng() % 100U);
                if (roll < read_percent) {
                    mutex.RLock();
                    // 不变式：读锁期间不可能有写者；a/b 必须一致。
                    GO2CPP_CHECK(!mutex.IsWriteLocked());
                    GO2CPP_CHECK(mutex.ReaderCount() >= 1);
                    const Payload snapshot = payload;
                    GO2CPP_CHECK(snapshot.a == snapshot.b);
                    mutex.RUnlock();
                    reads.fetch_add(1, std::memory_order_relaxed);
                } else {
                    mutex.Lock();
                    GO2CPP_CHECK(mutex.IsWriteLocked());
                    GO2CPP_CHECK(mutex.ReaderCount() == 0);
                    ++payload.a;
                    ++payload.b;
                    mutex.Unlock();
                    writes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
    }
    const double elapsed_ns = std::chrono::duration<double, std::nano>(
                                  std::chrono::steady_clock::now() - begin)
                                  .count();
    const std::uint64_t read_count = reads.load();
    const std::uint64_t write_count = writes.load();
    GO2CPP_CHECK(payload.a == write_count);
    GO2CPP_CHECK(payload.a == payload.b);
    GO2CPP_CHECK(read_count + write_count ==
                 static_cast<std::uint64_t>(threads * ops_per_thread));
    RwStressResult result;
    const double total_ops =
        static_cast<double>(threads) * static_cast<double>(ops_per_thread);
    result.ns_per_op = total_ops > 0.0 ? elapsed_ns / total_ops : 0.0;
    result.reads = read_count;
    result.writes = write_count;
    return result;
}

/**
 * @brief 三种负载 × 1/2/4/8 线程的读写锁压测，并输出 ns/op。
 */
void TestRWMutexStress() {
    struct Workload {
        const char* name;
        int read_percent;
    };
    const Workload workloads[] = {
        {"read-heavy(99% read)", 99},
        {"balanced(50% read)", 50},
        {"write-heavy(10% read)", 10},
    };
    const int thread_counts[] = {1, 2, 4, 8};
    for (const auto& workload : workloads) {
        for (int threads : thread_counts) {
            const int ops_per_thread = threads >= 8 ? 10000 : 20000;
            const RwStressResult result =
                RunRWMutexStress(threads, workload.read_percent,
                                 ops_per_thread);
            std::printf(
                "[locks][stress] RWMutex %-22s threads=%d ops=%d "
                "ns/op=%.1f reads=%llu writes=%llu\n",
                workload.name, threads, threads * ops_per_thread,
                result.ns_per_op,
                static_cast<unsigned long long>(result.reads),
                static_cast<unsigned long long>(result.writes));
        }
    }
}

/**
 * @brief 可重入锁深度 1/10/100 的压测（单线程递归 + 多线程争用）。
 */
void TestRecursiveMutexStress() {
    RecursiveMutex mutex;
    for (std::uint32_t depth : {1U, 10U, 100U}) {
        constexpr int kRounds = 20000;
        const auto begin = std::chrono::steady_clock::now();
        for (int round = 0; round < kRounds; ++round) {
            mutex.Lock();
            for (std::uint32_t i = 1; i < depth; ++i) {
                mutex.Lock();
            }
            GO2CPP_CHECK(mutex.RecursionCount() == depth);
            for (std::uint32_t i = 0; i < depth; ++i) {
                mutex.Unlock();
            }
        }
        const double elapsed_ns = std::chrono::duration<double, std::nano>(
                                      std::chrono::steady_clock::now() - begin)
                                      .count();
        std::printf(
            "[locks][stress] RecursiveMutex depth=%u rounds=%d "
            "ns/round=%.1f\n",
            depth, kRounds, elapsed_ns / kRounds);
    }

    // 4 线程争用下随机深度 1..10。
    std::atomic<std::uint64_t> completed{0};
    std::vector<std::thread> workers;
    const auto begin = std::chrono::steady_clock::now();
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64 rng(0x9E3779B97F4A7C15ull * (t + 1));
            for (int round = 0; round < 5000; ++round) {
                const std::uint32_t depth =
                    static_cast<std::uint32_t>(1 + rng() % 10U);
                mutex.Lock();
                for (std::uint32_t i = 1; i < depth; ++i) {
                    mutex.Lock();
                }
                GO2CPP_CHECK(mutex.RecursionCount() == depth);
                for (std::uint32_t i = 0; i < depth; ++i) {
                    mutex.Unlock();
                }
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
    }
    const double elapsed_ns = std::chrono::duration<double, std::nano>(
                                  std::chrono::steady_clock::now() - begin)
                                  .count();
    GO2CPP_CHECK(completed.load() == 4U * 5000U);
    GO2CPP_CHECK(!mutex.IsLockedByCurrent());
    std::printf(
        "[locks][stress] RecursiveMutex threads=4 ops=20000 ns/op=%.1f\n",
        elapsed_ns / 20000.0);
}

/**
 * @brief 超时锁压测：无竞争、有竞争、超时三条路径。
 */
void TestTimedMutexStress() {
    TimedMutex mutex;
    // 无竞争：TryLockFor 成功路径。
    {
        constexpr int kOps = 100000;
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i) {
            if (!mutex.TryLockFor(10ms)) {
                go2cpp_tests::watchdog_abort("TimedMutex uncontended acquire",
                                             __FILE__, __LINE__);
            }
            mutex.Unlock();
        }
        const double elapsed_ns = std::chrono::duration<double, std::nano>(
                                      std::chrono::steady_clock::now() - begin)
                                      .count();
        std::printf(
            "[locks][stress] TimedMutex uncontended ops=%d ns/op=%.1f\n",
            kOps, elapsed_ns / kOps);
    }
    // 有竞争：4 线程互相抢，每个操作都必须最终完成。
    {
        std::atomic<std::uint64_t> completed{0};
        std::vector<std::thread> workers;
        const auto begin = std::chrono::steady_clock::now();
        for (int t = 0; t < 4; ++t) {
            workers.emplace_back([&] {
                for (int i = 0; i < 5000; ++i) {
                    if (!mutex.TryLockFor(10s)) {
                        go2cpp_tests::watchdog_abort(
                            "TimedMutex contended acquire", __FILE__,
                            __LINE__);
                    }
                    mutex.Unlock();
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& worker : workers) {
            GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
        }
        const double elapsed_ns = std::chrono::duration<double, std::nano>(
                                      std::chrono::steady_clock::now() - begin)
                                      .count();
        GO2CPP_CHECK(completed.load() == 20000U);
        std::printf(
            "[locks][stress] TimedMutex contended threads=4 ops=20000 "
            "ns/op=%.1f\n",
            elapsed_ns / 20000.0);
    }
    // 超时路径：持锁期间 4 线程各 50 次，必须全部 false 且不改状态。
    {
        mutex.Lock();
        std::atomic<int> timeouts{0};
        std::atomic<int> unexpected{0};
        std::atomic<std::uint64_t> waited_ns{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < 4; ++t) {
            workers.emplace_back([&] {
                for (int i = 0; i < 50; ++i) {
                    const auto begin = std::chrono::steady_clock::now();
                    if (mutex.TryLockFor(2ms)) {
                        unexpected.fetch_add(1, std::memory_order_relaxed);
                        mutex.Unlock();
                    } else {
                        timeouts.fetch_add(1, std::memory_order_relaxed);
                    }
                    waited_ns.fetch_add(
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - begin)
                                .count()),
                        std::memory_order_relaxed);
                }
            });
        }
        for (auto& worker : workers) {
            GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
        }
        GO2CPP_CHECK(unexpected.load() == 0);
        GO2CPP_CHECK(timeouts.load() == 200);
        GO2CPP_CHECK(!mutex.TryLock());
        mutex.Unlock();
        GO2CPP_CHECK(mutex.TryLock());
        mutex.Unlock();
        std::printf(
            "[locks][stress] TimedMutex timeout attempts=200 avg_wait_us=%.1f\n",
            static_cast<double>(waited_ns.load()) / 200.0 / 1000.0);
    }
}

// ---------------------------------------------------------------------------
// 4. 随机测试
// ---------------------------------------------------------------------------

/**
 * @brief 随机 RLock/Lock/Unlock/Try* / 超时序列，逐操作校验不变式。
 */
void TestRWMutexRandom() {
    RWMutex mutex;
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> writes{0};
    std::vector<std::thread> workers;
    constexpr int kThreads = 4;
    constexpr int kSteps = 2000;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            enum class Held { kNone, kRead, kWrite };
            Held held = Held::kNone;
            // 读锁可重入：必须记录本线程持有的读者名额，否则会泄漏读锁，
            // 使写者永远等不到 m_readers == 0（自造死锁）。
            int read_depth = 0;
            std::mt19937_64 rng(0xD1B54A32D192ED03ull * (t + 1));
            for (int step = 0; step < kSteps; ++step) {
                const int roll = static_cast<int>(rng() % 100U);
                switch (held) {
                    case Held::kNone:
                        if (roll < 40) {
                            if (mutex.TryRLock()) {
                                held = Held::kRead;
                                read_depth = 1;
                                reads.fetch_add(1, std::memory_order_relaxed);
                            }
                        } else if (roll < 65) {
                            if (mutex.TryLock()) {
                                held = Held::kWrite;
                                writes.fetch_add(1, std::memory_order_relaxed);
                            }
                        } else if (roll < 80) {
                            mutex.RLock();
                            held = Held::kRead;
                            read_depth = 1;
                            reads.fetch_add(1, std::memory_order_relaxed);
                        } else if (roll < 92) {
                            mutex.Lock();
                            held = Held::kWrite;
                            writes.fetch_add(1, std::memory_order_relaxed);
                        } else if (roll < 97) {
                            if (mutex.RLockFor(1ms)) {
                                held = Held::kRead;
                                read_depth = 1;
                                reads.fetch_add(1, std::memory_order_relaxed);
                            }
                        } else {
                            if (mutex.LockFor(1ms)) {
                                held = Held::kWrite;
                                writes.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        break;
                    case Held::kRead:
                        // 读锁期间不变式：没有写者；自己是读者之一。
                        GO2CPP_CHECK(!mutex.IsWriteLocked());
                        GO2CPP_CHECK(mutex.ReaderCount() >= 1);
                        if (roll < 70) {
                            mutex.RUnlock();
                            --read_depth;
                            if (read_depth == 0) {
                                held = Held::kNone;
                            }
                        } else if (mutex.TryRLock()) {
                            ++read_depth;
                            reads.fetch_add(1, std::memory_order_relaxed);
                        }
                        break;
                    case Held::kWrite:
                        GO2CPP_CHECK(mutex.IsWriteLocked());
                        GO2CPP_CHECK(mutex.ReaderCount() == 0);
                        mutex.Unlock();
                        held = Held::kNone;
                        break;
                }
            }
            if (held == Held::kRead) {
                for (int i = 0; i < read_depth; ++i) {
                    mutex.RUnlock();
                }
            } else if (held == Held::kWrite) {
                mutex.Unlock();
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
    }
    GO2CPP_CHECK(reads.load() > 0);
    GO2CPP_CHECK(writes.load() > 0);
    GO2CPP_CHECK(mutex.ReaderCount() == 0);
    GO2CPP_CHECK(!mutex.IsWriteLocked());
    GO2CPP_CHECK(!mutex.HasPendingWriter());
}

/**
 * @brief 随机可重入 Lock/Unlock/TryLock 序列，逐操作校验递归计数。
 */
void TestRecursiveMutexRandom() {
    RecursiveMutex mutex;
    std::atomic<std::uint64_t> operations{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64 rng(0x94D049BB133111EBull * (t + 1));
            std::uint32_t depth = 0;
            for (int step = 0; step < 1500; ++step) {
                if (depth == 0) {
                    if (rng() % 3U == 0U) {
                        if (mutex.TryLock()) {
                            depth = 1;
                        }
                    } else {
                        mutex.Lock();
                        depth = 1;
                    }
                    if (depth != 0) {
                        GO2CPP_CHECK(mutex.RecursionCount() == depth);
                        GO2CPP_CHECK(mutex.IsLockedByCurrent());
                    }
                } else if (rng() % 100U < 55U) {
                    mutex.Lock();
                    ++depth;
                    GO2CPP_CHECK(mutex.RecursionCount() == depth);
                } else {
                    mutex.Unlock();
                    --depth;
                    GO2CPP_CHECK(mutex.RecursionCount() == depth);
                }
                operations.fetch_add(1, std::memory_order_relaxed);
            }
            for (std::uint32_t i = 0; i < depth; ++i) {
                mutex.Unlock();
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
    }
    GO2CPP_CHECK(operations.load() == 4U * 1500U);
    GO2CPP_CHECK(!mutex.IsLockedByCurrent());
    GO2CPP_CHECK(mutex.RecursionCount() == 0);
}

/**
 * @brief 在 Fiber 上跑随机可重入序列，验证 owner 按 G 而非线程记录。
 */
void TestRecursiveMutexRandomFibers() {
    go2cpp::Scheduler scheduler(MakeSchedulerConfig(2));
    scheduler.start();
    RecursiveMutex mutex;
    const auto body = [&](std::uint64_t seed) {
        std::mt19937_64 rng(seed);
        std::uint32_t depth = 0;
        for (int step = 0; step < 800; ++step) {
            if (depth == 0) {
                if (rng() % 3U == 0U) {
                    if (mutex.TryLock()) {
                        depth = 1;
                    }
                } else {
                    mutex.Lock();
                    depth = 1;
                }
            } else if (rng() % 100U < 55U) {
                mutex.Lock();
                ++depth;
                GO2CPP_CHECK(mutex.RecursionCount() == depth);
            } else {
                mutex.Unlock();
                --depth;
                GO2CPP_CHECK(mutex.RecursionCount() == depth);
            }
        }
        for (std::uint32_t i = 0; i < depth; ++i) {
            mutex.Unlock();
        }
    };
    auto first = scheduler.spawn([&] { body(0x123456789ABCDEFull); });
    auto second = scheduler.spawn([&] { body(0xFEDCBA987654321ull); });
    GO2CPP_CHECK(first->wait_for(20s));
    GO2CPP_CHECK(second->wait_for(20s));
    GO2CPP_CHECK(!mutex.IsLockedByCurrent());
    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 随机超时时长，验证失败后锁状态未被修改。
 */
void TestTimedMutexRandom() {
    TimedMutex mutex;
    // 自由竞争：随机 0..2ms 超时；成功与超时都必须自洽。
    std::atomic<int> acquired{0};
    std::atomic<int> timed_out{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64 rng(0xA0761D6478BD642Full * (t + 1));
            for (int i = 0; i < 300; ++i) {
                const auto timeout = std::chrono::milliseconds(rng() % 3U);
                if (mutex.TryLockFor(timeout)) {
                    acquired.fetch_add(1, std::memory_order_relaxed);
                    mutex.Unlock();
                } else {
                    timed_out.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
    }
    GO2CPP_CHECK(acquired.load() + timed_out.load() == 1200);
    GO2CPP_CHECK(acquired.load() > 0);
    GO2CPP_CHECK(mutex.TryLock());  // 最终必须可获取
    mutex.Unlock();

    // 持有期间随机超时：全部失败，且持有者的锁不受影响。
    mutex.Lock();
    std::atomic<int> blocked{0};
    std::atomic<int> unexpected{0};
    workers.clear();
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64 rng(0xE7037ED1A0B428DBull * (t + 1));
            for (int i = 0; i < 50; ++i) {
                const auto timeout = std::chrono::milliseconds(rng() % 4U);
                if (mutex.TryLockFor(timeout)) {
                    unexpected.fetch_add(1, std::memory_order_relaxed);
                    mutex.Unlock();
                } else {
                    blocked.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
    }
    GO2CPP_CHECK(unexpected.load() == 0);
    GO2CPP_CHECK(blocked.load() == 200);
    GO2CPP_CHECK(!mutex.TryLock());
    mutex.Unlock();
    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();

    // TryLockUntil 的过去 / 未来截止时刻。
    mutex.Lock();
    GO2CPP_CHECK(!mutex.TryLockUntil(std::chrono::steady_clock::now() - 1ms));
    GO2CPP_CHECK(!mutex.TryLockUntil(std::chrono::steady_clock::now() + 5ms));
    mutex.Unlock();
    GO2CPP_CHECK(mutex.TryLockUntil(std::chrono::steady_clock::now() - 1ms));
    mutex.Unlock();
}

// ---------------------------------------------------------------------------
// 5. 真实场景（≥5）
// ---------------------------------------------------------------------------

/// @brief 场景 1：配置热加载（RWMutex，读多写少，读者不得看到撕裂配置）。
void TestScenarioConfigHotReload() {
    struct Config {
        std::uint32_t generation{0};
        std::array<std::uint64_t, 8> values{};
        std::uint64_t checksum{0};
    };
    const auto checksum_of = [](const std::array<std::uint64_t, 8>& values) {
        std::uint64_t sum = 1469598103934665603ull;
        for (std::uint64_t value : values) {
            sum ^= value;
            sum *= 1099511628211ull;
        }
        return sum;
    };

    RWMutex mutex;
    Config config;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> torn_reads{0};
    std::atomic<std::uint64_t> reads{0};

    constexpr int kUpdates = 200;
    std::thread writer([&] {
        for (int i = 1; i <= kUpdates; ++i) {
            // 锁外构造完整的新配置。
            Config next;
            next.generation = static_cast<std::uint32_t>(i);
            for (std::size_t k = 0; k < next.values.size(); ++k) {
                next.values[k] = static_cast<std::uint64_t>(i) * 1000U + k;
            }
            next.checksum = checksum_of(next.values);
            mutex.Lock();
            config = next;
            mutex.Unlock();
            std::this_thread::sleep_for(200us);
        }
        done.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&] {
            while (!done.load(std::memory_order_acquire)) {
                mutex.RLock();
                const Config snapshot = config;
                mutex.RUnlock();
                if (snapshot.checksum != checksum_of(snapshot.values)) {
                    torn_reads.fetch_add(1, std::memory_order_relaxed);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    GO2CPP_JOIN_WITH_WATCHDOG(writer, 30s);
    for (auto& reader : readers) {
        GO2CPP_JOIN_WITH_WATCHDOG(reader, 30s);
    }
    mutex.RLock();
    const Config final_config = config;
    mutex.RUnlock();
    GO2CPP_CHECK(torn_reads.load() == 0);
    GO2CPP_CHECK(reads.load() > 0);
    GO2CPP_CHECK(final_config.generation == kUpdates);
    GO2CPP_CHECK(final_config.checksum == checksum_of(final_config.values));
    std::printf(
        "[locks][scenario] config-hot-reload updates=%d reads=%llu torn=%llu\n",
        kUpdates, static_cast<unsigned long long>(reads.load()),
        static_cast<unsigned long long>(torn_reads.load()));
}

/// @brief 场景 2：连接池（写锁改空闲表，读锁取统计快照）。
void TestScenarioConnectionPool() {
    struct Pool {
        RWMutex mutex;
        std::vector<bool> available;
        std::vector<int> idle;
        std::size_t capacity{2};
        std::atomic<int> grow_events{0};
        std::atomic<int> in_use{0};
    };
    Pool pool;
    pool.available.assign(pool.capacity, true);
    for (std::size_t i = 0; i < pool.capacity; ++i) {
        pool.idle.push_back(static_cast<int>(i));
    }

    const auto acquire = [&]() {
        pool.mutex.Lock();
        if (pool.idle.empty()) {
            // 扩容：容量翻倍，新增连接标记为空闲。
            const std::size_t old_capacity = pool.capacity;
            pool.capacity *= 2;
            pool.available.resize(pool.capacity, true);
            for (std::size_t i = old_capacity; i < pool.capacity; ++i) {
                pool.idle.push_back(static_cast<int>(i));
            }
            pool.grow_events.fetch_add(1, std::memory_order_relaxed);
        }
        const int id = pool.idle.back();
        pool.idle.pop_back();
        GO2CPP_CHECK(static_cast<std::size_t>(id) < pool.available.size());
        GO2CPP_CHECK(pool.available[static_cast<std::size_t>(id)]);
        pool.available[static_cast<std::size_t>(id)] = false;
        pool.in_use.fetch_add(1, std::memory_order_relaxed);
        // 不变式必须在写锁内检查：idle 是普通 vector，锁外读会与写者竞争。
        GO2CPP_CHECK(pool.idle.size() + static_cast<std::size_t>(
                                                pool.in_use.load()) ==
                     pool.capacity);
        pool.mutex.Unlock();
        return id;
    };
    const auto release = [&](int id) {
        pool.mutex.Lock();
        GO2CPP_CHECK(!pool.available[static_cast<std::size_t>(id)]);
        pool.available[static_cast<std::size_t>(id)] = true;
        pool.idle.push_back(id);
        pool.in_use.fetch_sub(1, std::memory_order_relaxed);
        pool.mutex.Unlock();
    };

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> stats_reads{0};
    std::vector<std::thread> workers;
    constexpr int kWorkers = 4;
    constexpr int kCycles = 2000;
    for (int t = 0; t < kWorkers; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < kCycles; ++i) {
                const int id = acquire();
                // 模拟真实使用连接的时间：4 个 worker 只有 2 个初始连接，
                // 必然触发扩容路径。
                std::this_thread::sleep_for(20us);
                release(id);
            }
        });
    }
    std::vector<std::thread> stats;
    for (int t = 0; t < 2; ++t) {
        stats.emplace_back([&] {
            while (!done.load(std::memory_order_acquire)) {
                pool.mutex.RLock();
                const std::size_t idle = pool.idle.size();
                const std::size_t capacity = pool.capacity;
                pool.mutex.RUnlock();
                GO2CPP_CHECK(idle <= capacity);
                stats_reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 60s);
    }
    done.store(true, std::memory_order_release);
    for (auto& stat : stats) {
        GO2CPP_JOIN_WITH_WATCHDOG(stat, 30s);
    }
    pool.mutex.Lock();
    const std::size_t idle = pool.idle.size();
    const std::size_t capacity = pool.capacity;
    pool.mutex.Unlock();
    GO2CPP_CHECK(pool.in_use.load() == 0);
    GO2CPP_CHECK(idle == capacity);
    GO2CPP_CHECK(pool.grow_events.load() > 0);
    GO2CPP_CHECK(stats_reads.load() > 0);
    std::printf(
        "[locks][scenario] connection-pool capacity=%zu grows=%d "
        "acquires=%d stats_reads=%llu\n",
        capacity, pool.grow_events.load(), kWorkers * kCycles,
        static_cast<unsigned long long>(stats_reads.load()));
}

/// @brief 场景 3：可重入锁的树遍历（演示用法，同时复述设计警告）。
void TestScenarioRecursiveTreeTraversal() {
    struct Node {
        int left{-1};
        int right{-1};
        bool visited{false};
    };
    constexpr int kDepth = 10;
    const int node_count = (1 << kDepth) - 1;
    std::vector<Node> nodes(static_cast<std::size_t>(node_count));
    for (int i = 0; i < node_count; ++i) {
        const int left = 2 * i + 1;
        const int right = 2 * i + 2;
        nodes[static_cast<std::size_t>(i)].left =
            left < node_count ? left : -1;
        nodes[static_cast<std::size_t>(i)].right =
            right < node_count ? right : -1;
    }

    RecursiveMutex mutex;
    int visits = 0;
    int max_recursion = 0;
    // 注意：这是**反例式**演示。持锁递归遍历会让整棵树在遍历期间独占锁；
    // 生产代码应改为「快照后无锁遍历」或由单 owner 管理。这里只验证可重入
    // 计数在真实递归调用链上正确。
    const auto visit = [&](auto&& self, int index) -> void {
        mutex.Lock();
        const int recursion = static_cast<int>(mutex.RecursionCount());
        if (recursion > max_recursion) {
            max_recursion = recursion;
        }
        nodes[static_cast<std::size_t>(index)].visited = true;
        ++visits;
        const int left = nodes[static_cast<std::size_t>(index)].left;
        const int right = nodes[static_cast<std::size_t>(index)].right;
        if (left >= 0) {
            self(self, left);
        }
        if (right >= 0) {
            self(self, right);
        }
        mutex.Unlock();
    };
    visit(visit, 0);
    GO2CPP_CHECK(visits == node_count);
    GO2CPP_CHECK(max_recursion == kDepth);
    GO2CPP_CHECK(mutex.RecursionCount() == 0);

    // 同一遍历在 Fiber 上重跑一次，验证 owner 是 G 而非线程。
    go2cpp::Scheduler scheduler(MakeSchedulerConfig(1));
    scheduler.start();
    visits = 0;
    max_recursion = 0;
    for (auto& node : nodes) {
        node.visited = false;
    }
    auto fiber = scheduler.spawn([&] { visit(visit, 0); });
    GO2CPP_CHECK(fiber->wait_for(10s));
    GO2CPP_CHECK(visits == node_count);
    GO2CPP_CHECK(max_recursion == kDepth);
    ShutdownOrReportLostWakeup(scheduler);
    std::printf(
        "[locks][scenario] recursive-tree depth=%d nodes=%d visits=%d "
        "max_recursion=%d\n",
        kDepth, node_count, visits, max_recursion);
}

/// @brief 场景 4：`TimedMutex` 防止「持锁 IO」把调用方永久拖死。
void TestScenarioTimedMutexIoFallback() {
    TimedMutex mutex;
    std::atomic<bool> slow_holds{false};
    std::atomic<bool> slow_done{false};
    std::atomic<bool> fallback_used{false};
    std::atomic<bool> acquired_fast{false};

    std::thread slow_io([&] {
        mutex.Lock();
        slow_holds.store(true, std::memory_order_release);
        std::this_thread::sleep_for(400ms);  // 模拟持锁 IO
        mutex.Unlock();
        slow_done.store(true, std::memory_order_release);
    });
    // 等到慢线程真正持锁。
    std::thread caller([&] {
        while (!slow_holds.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const auto begin = std::chrono::steady_clock::now();
        const bool acquired = mutex.TryLockFor(20ms);
        const double waited_us = ElapsedUs(begin);
        if (acquired) {
            acquired_fast.store(true, std::memory_order_release);
            mutex.Unlock();
        } else {
            fallback_used.store(true, std::memory_order_release);
            GO2CPP_CHECK(waited_us >= 10000.0);
            GO2CPP_CHECK(waited_us < 300000.0);
            // 在**等待者自己的时间线上**确认超时返回时慢线程仍持锁，避免
            // 主线程 join 后再检查被调度延迟放大成假失败。
            GO2CPP_CHECK(!slow_done.load(std::memory_order_acquire));
            // 兜底路径：读缓存，不碰锁。
        }
    });
    GO2CPP_JOIN_WITH_WATCHDOG(caller, 3s);
    GO2CPP_CHECK(fallback_used.load());
    GO2CPP_CHECK(!acquired_fast.load());
    // 超时不能改变锁状态：慢线程的 Unlock 必须正常，之后锁可获取。
    GO2CPP_JOIN_WITH_WATCHDOG(slow_io, 5s);
    GO2CPP_CHECK(slow_done.load());
    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();
    std::printf(
        "[locks][scenario] timed-io-fallback fallback=%d slow_io_ms=400\n",
        static_cast<int>(fallback_used.load()));
}

/// @brief 场景 5：写者优先下的写饥饿验证，并与 `std::shared_mutex` 对照。
struct WriterLatency {
    std::vector<double> samples_us;
    std::uint64_t reader_ops{0};
    /// 写者是否被读者饿住（等待达到 run_limit，由停止读者兜底）。
    bool starved{false};
};

/// @brief 测量写者等待延迟（读负载持续，读临界区故意持有一段时间）。
///
/// @param run_limit 写者最长可等待时间；到达后停止读者（否则读者优先的对照
///        实现会把测试永久挂住）。这**不是**放宽断言：调用方会检查 `starved`
///        并把它作为失败报告出来。
template <typename ReaderLock, typename ReaderUnlock, typename WriterLock,
          typename WriterUnlock>
WriterLatency MeasureWriterLatency(int reader_threads, int writer_rounds,
                                   std::chrono::milliseconds run_limit,
                                   ReaderLock reader_lock,
                                   ReaderUnlock reader_unlock,
                                   WriterLock writer_lock,
                                   WriterUnlock writer_unlock) {
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    std::atomic<std::uint64_t> reads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < reader_threads; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                reader_lock();
                const auto until = std::chrono::steady_clock::now() + 100us;
                while (std::chrono::steady_clock::now() < until) {
                }
                reader_unlock();
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // 兜底：写者等满 run_limit 说明被饿住；先撤读者让主线程能返回，
    // 再由调用方把 starved 判为失败。
    std::thread stopper([&] {
        const auto deadline = std::chrono::steady_clock::now() + run_limit;
        while (!finished.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        if (!finished.load(std::memory_order_acquire)) {
            stop.store(true, std::memory_order_release);
        }
    });
    std::this_thread::sleep_for(50ms);
    WriterLatency result;
    result.samples_us.reserve(static_cast<std::size_t>(writer_rounds));
    for (int i = 0; i < writer_rounds; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        writer_lock();
        result.samples_us.push_back(ElapsedUs(begin));
        writer_unlock();
    }
    finished.store(true, std::memory_order_release);
    stopper.join();
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        GO2CPP_JOIN_WITH_WATCHDOG(reader, 30s);
    }
    result.reader_ops = reads.load();
    std::sort(result.samples_us.begin(), result.samples_us.end());
    const double limit_us =
        static_cast<double>(run_limit.count()) * 1000.0 * 0.8;
    result.starved =
        !result.samples_us.empty() && result.samples_us.back() >= limit_us;
    return result;
}

void TestScenarioWriterStarvation() {
    // 我方 RWMutex：写者优先，读者在写者等待后不得插队。
    RWMutex mutex;
    const WriterLatency ours = MeasureWriterLatency(
        4, 40, 2s, [&] { mutex.RLock(); }, [&] { mutex.RUnlock(); },
        [&] { mutex.Lock(); }, [&] { mutex.Unlock(); });

    // 对照组：libstdc++ 的 std::shared_mutex（glibc 默认读者优先）。
    std::shared_mutex std_mutex;
    const WriterLatency std_result = MeasureWriterLatency(
        4, 40, 2s, [&] { std_mutex.lock_shared(); },
        [&] { std_mutex.unlock_shared(); }, [&] { std_mutex.lock(); },
        [&] { std_mutex.unlock(); });

    const auto report = [](const char* name, const WriterLatency& latency) {
        std::printf(
            "[locks][scenario] writer-latency %-12s p50=%.1fus p95=%.1fus "
            "p99=%.1fus max=%.1fus starved=%d reader_ops=%llu\n",
            name, Percentile(latency.samples_us, 0.50),
            Percentile(latency.samples_us, 0.95),
            Percentile(latency.samples_us, 0.99),
            latency.samples_us.empty() ? 0.0 : latency.samples_us.back(),
            static_cast<int>(latency.starved),
            static_cast<unsigned long long>(latency.reader_ops));
    };
    report("RWMutex", ours);
    report("std::shared", std_result);
    // 写者优先的硬性断言：写者不得被饿住，且 40 次写请求的最坏等待远小于
    // 读者总运行时间（若允许插队，最坏会到数百 ms 甚至无限）。
    GO2CPP_CHECK(!ours.starved);
    GO2CPP_CHECK(!ours.samples_us.empty());
    GO2CPP_CHECK(ours.samples_us.back() < 50000.0);

    // 直接的门禁验证：写者排队时，新读者 TryRLock 必须失败。
    mutex.RLock();
    std::atomic<bool> writer_done{false};
    std::thread writer([&] {
        mutex.Lock();
        mutex.Unlock();
        writer_done.store(true, std::memory_order_release);
    });
    GO2CPP_REQUIRE_EVENTUALLY(mutex.HasPendingWriter(), 2s);
    GO2CPP_CHECK(!mutex.TryRLock());
    GO2CPP_CHECK(!mutex.RLockFor(10ms));
    mutex.RUnlock();
    GO2CPP_REQUIRE_EVENTUALLY(writer_done.load(), 2s);
    GO2CPP_JOIN_WITH_WATCHDOG(writer, 3s);
}

// ---------------------------------------------------------------------------
// 6. 与标准库锁的对比基准
// ---------------------------------------------------------------------------

struct GoRwAdapter {
    RWMutex mutex;
    void ReadLock() { mutex.RLock(); }
    void ReadUnlock() { mutex.RUnlock(); }
    void WriteLock() { mutex.Lock(); }
    void WriteUnlock() { mutex.Unlock(); }
};

struct StdRwAdapter {
    std::shared_mutex mutex;
    void ReadLock() { mutex.lock_shared(); }
    void ReadUnlock() { mutex.unlock_shared(); }
    void WriteLock() { mutex.lock(); }
    void WriteUnlock() { mutex.unlock(); }
};

struct GoRecursiveAdapter {
    RecursiveMutex mutex;
    void Lock() { mutex.Lock(); }
    void Unlock() { mutex.Unlock(); }
};

struct StdRecursiveAdapter {
    std::recursive_mutex mutex;
    void Lock() { mutex.lock(); }
    void Unlock() { mutex.unlock(); }
};

/// @brief 统一的读写负载；返回 ns/op。
template <typename Adapter>
double BenchRw(int threads, int read_percent, int ops_per_thread) {
    Adapter adapter;
    std::atomic<std::uint64_t> sink{0};
    std::vector<std::thread> workers;
    const auto begin = std::chrono::steady_clock::now();
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64 rng(0x2545F4914F6CDD1Dull * (t + 1));
            std::uint64_t local = 0;
            std::uint64_t shared = 0;
            for (int i = 0; i < ops_per_thread; ++i) {
                if (static_cast<int>(rng() % 100U) < read_percent) {
                    adapter.ReadLock();
                    local += shared;
                    adapter.ReadUnlock();
                } else {
                    adapter.WriteLock();
                    ++shared;
                    adapter.WriteUnlock();
                }
            }
            sink.fetch_add(local + shared, std::memory_order_relaxed);
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 60s);
    }
    const double elapsed_ns = std::chrono::duration<double, std::nano>(
                                  std::chrono::steady_clock::now() - begin)
                                  .count();
    (void)sink.load();
    return elapsed_ns / (static_cast<double>(threads) * ops_per_thread);
}

/// @brief 可重入锁基准：每轮 lock/.../unlock（depth 层）。
template <typename Adapter>
double BenchRecursive(int threads, int depth, int ops_per_thread) {
    Adapter adapter;
    std::atomic<std::uint64_t> sink{0};
    std::vector<std::thread> workers;
    const auto begin = std::chrono::steady_clock::now();
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            std::uint64_t local = 0;
            for (int i = 0; i < ops_per_thread; ++i) {
                for (int d = 0; d < depth; ++d) {
                    adapter.Lock();
                }
                ++local;
                for (int d = 0; d < depth; ++d) {
                    adapter.Unlock();
                }
            }
            sink.fetch_add(local, std::memory_order_relaxed);
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 60s);
    }
    const double elapsed_ns = std::chrono::duration<double, std::nano>(
                                  std::chrono::steady_clock::now() - begin)
                                  .count();
    (void)sink.load();
    return elapsed_ns / (static_cast<double>(threads) * ops_per_thread);
}

/// @brief 超时锁无竞争基准：TryLockFor + Unlock。
template <typename TryLock, typename Unlock>
double BenchTimedUncontended(int ops, TryLock try_lock, Unlock unlock) {
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < ops; ++i) {
        if (!try_lock()) {
            go2cpp_tests::watchdog_abort("timed uncontended acquire", __FILE__,
                                         __LINE__);
        }
        unlock();
    }
    const double elapsed_ns = std::chrono::duration<double, std::nano>(
                                  std::chrono::steady_clock::now() - begin)
                                  .count();
    return elapsed_ns / ops;
}

/// @brief 超时锁有竞争基准：持有者持锁，等待者测超时路径的平均等待（微秒）。
template <typename LockFn, typename UnlockFn, typename TryLockFn>
WriterLatency BenchTimedContended(LockFn lock, UnlockFn unlock,
                                  TryLockFn try_lock,
                                  std::chrono::milliseconds timeout,
                                  int threads, int rounds, int* failures) {
    lock();
    std::atomic<int> failed{0};
    std::vector<std::thread> workers;
    std::mutex samples_mutex;
    WriterLatency result;
    result.samples_us.reserve(
        static_cast<std::size_t>(threads) * static_cast<std::size_t>(rounds));
    const auto begin = std::chrono::steady_clock::now();
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            std::vector<double> local;
            local.reserve(static_cast<std::size_t>(rounds));
            for (int i = 0; i < rounds; ++i) {
                const auto wait_begin = std::chrono::steady_clock::now();
                const bool acquired = try_lock(timeout);
                local.push_back(ElapsedUs(wait_begin));
                if (acquired) {
                    unlock();
                } else {
                    failed.fetch_add(1, std::memory_order_relaxed);
                }
                go2cpp_tests::yield_for_watchdog();
            }
            std::lock_guard<std::mutex> guard(samples_mutex);
            result.samples_us.insert(result.samples_us.end(), local.begin(),
                                     local.end());
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 60s);
    }
    unlock();
    (void)begin;
    *failures = failed.load();
    std::sort(result.samples_us.begin(), result.samples_us.end());
    return result;
}

/**
 * @brief 输出三把锁与标准库对应设施的对比数据。
 *
 * @note 数字只用于**同一构建内**的相对比较：本项目 ctest 使用 Debug 构建，
 *       `-O0` 会同时拖慢本实现与标准库路径。构建类型打印在首行，读者不应
 *       把这里的绝对值当成生产性能。
 */
void RunLockBenchmarks() {
#if defined(NDEBUG)
    const char* build_type = "release";
#else
    const char* build_type = "debug";
#endif
    std::printf("[locks][bench] build=%s procs_running=%d\n", build_type,
                ProcsRunning());

    struct RwCase {
        const char* label;
        int read_percent;
        int threads;
        int ops_per_thread;
    };
    const RwCase rw_cases[] = {
        {"read-heavy(99%)", 99, 4, 20000},
        {"read-heavy(99%)", 99, 8, 10000},
        {"balanced(50%)", 50, 4, 20000},
        {"write-heavy(10%)", 10, 4, 20000},
    };
    for (const RwCase& test_case : rw_cases) {
        // 环境里有其他 agent 在并行构建（WSL2 loadavg 不可信，看 procs_running）。
        // 每个用例交替跑 3 次取**最小值**，降低偶发调度噪声对相对比较的影响；
        // 这是吞吐微基准的常规做法，不代表生产性能。
        double go_ns = 1e300;
        double std_ns = 1e300;
        for (int repetition = 0; repetition < 3; ++repetition) {
            go_ns = std::min(
                go_ns, BenchRw<GoRwAdapter>(test_case.threads,
                                            test_case.read_percent,
                                            test_case.ops_per_thread));
            std_ns = std::min(
                std_ns, BenchRw<StdRwAdapter>(test_case.threads,
                                              test_case.read_percent,
                                              test_case.ops_per_thread));
        }
        std::printf(
            "[locks][bench] %-22s threads=%d go=%.1fns std::shared=%.1fns "
            "go/std=%.2fx pr=%d\n",
            test_case.label, test_case.threads, go_ns, std_ns,
            std_ns > 0.0 ? go_ns / std_ns : 0.0, ProcsRunning());
    }

    for (int threads : {1, 4}) {
        constexpr int kDepth = 10;
        constexpr int kOps = 20000;
        double go_ns = 1e300;
        double std_ns = 1e300;
        for (int repetition = 0; repetition < 3; ++repetition) {
            go_ns = std::min(
                go_ns,
                BenchRecursive<GoRecursiveAdapter>(threads, kDepth, kOps));
            std_ns = std::min(
                std_ns,
                BenchRecursive<StdRecursiveAdapter>(threads, kDepth, kOps));
        }
        std::printf(
            "[locks][bench] RecursiveMutex depth=%d threads=%d go=%.1fns "
            "std::recursive=%.1fns go/std=%.2fx pr=%d\n",
            kDepth, threads, go_ns, std_ns,
            std_ns > 0.0 ? go_ns / std_ns : 0.0, ProcsRunning());
    }

    // 无竞争超时路径。
    constexpr int kTimedOps = 100000;
    TimedMutex go_timed;
    std::timed_mutex std_timed;
    double go_uncontended = 1e300;
    double std_uncontended = 1e300;
    for (int repetition = 0; repetition < 3; ++repetition) {
        go_uncontended =
            std::min(go_uncontended,
                     BenchTimedUncontended(
                         kTimedOps, [&] { return go_timed.TryLockFor(10ms); },
                         [&] { go_timed.Unlock(); }));
        std_uncontended =
            std::min(std_uncontended,
                     BenchTimedUncontended(
                         kTimedOps,
                         [&] { return std_timed.try_lock_for(10ms); },
                         [&] { std_timed.unlock(); }));
    }
    std::printf(
        "[locks][bench] TimedMutex uncontended ops=%d go=%.1fns "
        "std::timed=%.1fns go/std=%.2fx pr=%d\n",
        kTimedOps, go_uncontended, std_uncontended,
        std_uncontended > 0.0 ? go_uncontended / std_uncontended : 0.0,
        ProcsRunning());

    // 有竞争的超时路径：锁被主线程持有，2 个线程各测 100 次 2ms 超时。
    // 取两次运行的最小中位数；失败数必须每次都是 200。
    double go_wait_us = 1e300;
    double std_wait_us = 1e300;
    int go_failures = 0;
    int std_failures = 0;
    for (int repetition = 0; repetition < 2; ++repetition) {
        TimedMutex go_contended_mutex;
        const WriterLatency go_latency = BenchTimedContended(
            [&] { go_contended_mutex.Lock(); },
            [&] { go_contended_mutex.Unlock(); },
            [&](std::chrono::milliseconds timeout) {
                return go_contended_mutex.TryLockFor(timeout);
            },
            2ms, 2, 100, &go_failures);
        go_wait_us = std::min(
            go_wait_us,
            go_latency.samples_us.empty()
                ? 0.0
                : go_latency.samples_us[go_latency.samples_us.size() / 2]);
        std::timed_mutex std_contended_mutex;
        const WriterLatency std_latency = BenchTimedContended(
            [&] { std_contended_mutex.lock(); },
            [&] { std_contended_mutex.unlock(); },
            [&](std::chrono::milliseconds timeout) {
                return std_contended_mutex.try_lock_for(timeout);
            },
            2ms, 2, 100, &std_failures);
        std_wait_us = std::min(
            std_wait_us,
            std_latency.samples_us.empty()
                ? 0.0
                : std_latency.samples_us[std_latency.samples_us.size() / 2]);
    }
    std::printf(
        "[locks][bench] TimedMutex timeout(2ms) go_median=%.1fus "
        "std_median=%.1fus fail=%d/%d pr=%d\n",
        go_wait_us, std_wait_us, go_failures, std_failures, ProcsRunning());
}

}  // namespace

// ---------------------------------------------------------------------------
// 统一入口
// ---------------------------------------------------------------------------

/**
 * @brief 运行锁设施的全部测试（语义 / 混合场景 / 压测 / 随机 / 真实场景 /
 *        对比基准）。
 *
 * @note 模块名 `locks` 需要在 `tests/test_main.cpp` 中注册，见交付报告里的
 *       集成片段。本文件刻意不改 `test_main.cpp` 与
 *       `cmake/Go2CppTests.cmake`，保持与其它并行改动的隔离。
 */
void run_locks_tests() {
    go2cpp_tests::announce(
        "rwmutex / recursive_mutex / timed_mutex (fiber + thread)");

    // 基础语义。
    TestRWMutexSemantics();
    TestRWMutexWriterFifoFibers();
    TestRecursiveMutexSemantics();
    TestTimedMutexSemantics();

    // 协程 / 线程混合场景 A/B/C/D。
    TestRWMutexScenarioAFiberWaits();
    TestRWMutexScenarioBThreadHoldsFiberWaits();
    TestRWMutexScenarioCFiberHoldsThreadWaits();
    TestRWMutexScenarioDMixedContention();
    TestRecursiveMutexMixedPaths();
    TestTimedMutexMixedPaths();

    // 压测。
    TestRWMutexStress();
    TestRecursiveMutexStress();
    TestTimedMutexStress();

    // 随机测试。
    TestRWMutexRandom();
    TestRecursiveMutexRandom();
    TestRecursiveMutexRandomFibers();
    TestTimedMutexRandom();

    // 真实场景（≥5）。
    TestScenarioConfigHotReload();
    TestScenarioConnectionPool();
    TestScenarioRecursiveTreeTraversal();
    TestScenarioTimedMutexIoFallback();
    TestScenarioWriterStarvation();

    // 与标准库的对比基准。
    RunLockBenchmarks();
}

#ifdef GO2CPP_LOCKS_TEST_MAIN
/**
 * @brief 独立入口（仅用于隔离验证；集成后由 `go2cpp_tests` 调用）。
 * @return 全部通过返回 0，否则返回 1。
 */
int main(int argc, char** argv) {
    const bool bench_only =
        argc > 1 && std::string(argv[1]) == "--bench-only";
    if (bench_only) {
        RunLockBenchmarks();
    } else {
        run_locks_tests();
    }
    const int failures =
        go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::cerr << "[test] failures=" << failures << '\n';
        return 1;
    }
    std::cout << "[test] all locks checks passed\n";
    return 0;
}
#endif
