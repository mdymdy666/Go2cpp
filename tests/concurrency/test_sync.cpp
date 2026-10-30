#include "go2cpp/sync.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/channel.hpp"

#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

template <typename Predicate>
bool WaitUntil(Predicate predicate,
               std::chrono::steady_clock::duration timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

/**
 * @brief 关闭调度器，但绝不无限期等待失去唤醒的 G。
 *
 * @details `Scheduler::shutdown()` 会一直等到所有已启动 G 进入终态。一旦某个
 *   G 因丢唤醒永久停在 waiting，测试进程会卡到 ctest 的 120s 超时，日志里
 *   没有任何定位信息。这里把它限制为 5s：超时说明有 G 失去唤醒，立即 abort
 *   并带出测试位置。**这不是放宽断言** —— 失败仍然是失败，只是失败得更快、
 *   更能定位。
 */
void ShutdownOrReportLostWakeup(go2cpp::Scheduler& scheduler) {
    if (scheduler.shutdown_for(5s)) {
        return;
    }
    go2cpp_tests::watchdog_abort(
        "scheduler shutdown timed out: a started G lost its wakeup", __FILE__,
        __LINE__);
}

/**
 * @brief 构造同步原语测试使用的调度配置。
 * @param processors 处理器数量，测试会据此验证单 P 和多 P 路径。
 * @return 经过测试约束初始化的 SchedulerConfig 值。
 */
go2cpp::SchedulerConfig SchedulerConfig(std::size_t processors) {
    go2cpp::SchedulerConfig config;
    config.processor_count = processors;
    config.max_workers = processors;
    config.idle_wait = 1ms;
    return config;
}

/**
 * @brief 验证未托管线程边界和 WaitGroup 计数错误处理。
 * @return 无；测试失败由统一断言统计。
 */
void TestUnmanagedBoundaryAndCounterErrors() {
    go2cpp::sync::Mutex mutex;
    GO2CPP_CHECK(mutex.LockFor(0ms));
    mutex.Unlock();
    GO2CPP_CHECK(mutex.Lock());

    // An unmanaged contender now blocks on the same FIFO waiter path as a G.
    std::atomic<bool> native_entered{false};
    std::atomic<bool> native_acquired{false};
    std::thread native_waiter([&] {
        native_entered.store(true, std::memory_order_release);
        native_acquired.store(mutex.Lock(), std::memory_order_release);
        if (native_acquired.load(std::memory_order_acquire)) {
            mutex.Unlock();
        }
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return native_entered.load(std::memory_order_acquire); }));
    GO2CPP_CHECK(!native_acquired.load(std::memory_order_acquire));
    GO2CPP_CHECK(!mutex.TryLock());
    mutex.Unlock();
    GO2CPP_JOIN_WITH_WATCHDOG(native_waiter, 3s);
    GO2CPP_CHECK(native_acquired.load(std::memory_order_acquire));

    // BasicLockable spelling follows the same blocking native path.
    GO2CPP_CHECK(mutex.Lock());
    std::atomic<bool> adapter_entered{false};
    std::atomic<bool> adapter_acquired{false};
    std::thread adapter_waiter([&] {
        adapter_entered.store(true, std::memory_order_release);
        mutex.lock();
        adapter_acquired.store(true, std::memory_order_release);
        mutex.unlock();
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return adapter_entered.load(std::memory_order_acquire); }));
    GO2CPP_CHECK(!adapter_acquired.load(std::memory_order_acquire));
    mutex.Unlock();
    GO2CPP_JOIN_WITH_WATCHDOG(adapter_waiter, 3s);
    GO2CPP_CHECK(adapter_acquired.load(std::memory_order_acquire));
    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();

    go2cpp::sync::WaitGroup group;
    bool negative_threw = false;
    try {
        group.Done();
    } catch (const std::logic_error&) {
        negative_threw = true;
    }
    GO2CPP_CHECK(negative_threw);
    GO2CPP_CHECK(group.Count() == 0);
    group.Add(1);
    group.Done();
    GO2CPP_CHECK(group.Wait());
}


/**
 * @brief 验证普通线程与 managed Fiber 共享锁、条件变量和 WaitGroup。
 * @return 无；测试失败由统一断言统计。
 */
void TestMixedThreadAndFiberSynchronization() {
    go2cpp::Scheduler scheduler(SchedulerConfig(1));
    scheduler.start();

    // A native owner can hand the logical mutex to a Fiber. The mutex does not
    // encode OS-thread ownership, so migration never turns this into a
    // pthread-owner violation.
    go2cpp::sync::Mutex mutex;
    GO2CPP_CHECK(mutex.Lock());
    std::atomic<bool> fiber_waiting{false};
    std::atomic<bool> fiber_acquired{false};
    auto fiber_waiter = scheduler.spawn([&] {
        fiber_waiting.store(true, std::memory_order_release);
        const bool acquired = mutex.Lock();
        fiber_acquired.store(acquired, std::memory_order_release);
        if (acquired) {
            mutex.Unlock();
        }
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return fiber_waiting.load(std::memory_order_acquire); }));
    GO2CPP_CHECK(WaitUntil(
        [&] { return fiber_waiter->state() == go2cpp::GState::kWaiting; }));
    mutex.Unlock();
    GO2CPP_CHECK(fiber_waiter->wait_for(2s));
    GO2CPP_CHECK(fiber_acquired.load(std::memory_order_acquire));

    // The reverse direction blocks a native thread behind a Fiber owner.
    std::atomic<bool> fiber_holds{false};
    std::atomic<bool> release_fiber{false};
    auto fiber_owner = scheduler.spawn([&] {
        GO2CPP_CHECK(mutex.Lock());
        fiber_holds.store(true, std::memory_order_release);
        const auto release_deadline = std::chrono::steady_clock::now() + 5s;
        while (!release_fiber.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < release_deadline) {
            (void)scheduler.yield_current();
            go2cpp_tests::pause_for_watchdog();
        }
        if (!release_fiber.load(std::memory_order_acquire)) {
            go2cpp_tests::watchdog_abort("fiber owner release", __FILE__,
                                         __LINE__);
        }
        mutex.Unlock();
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return fiber_holds.load(std::memory_order_acquire); }));
    std::atomic<bool> native_entered{false};
    std::atomic<bool> native_acquired{false};
    std::thread native_waiter([&] {
        native_entered.store(true, std::memory_order_release);
        const bool acquired = mutex.Lock();
        native_acquired.store(acquired, std::memory_order_release);
        if (acquired) {
            mutex.Unlock();
        }
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return native_entered.load(std::memory_order_acquire); }));
    GO2CPP_CHECK(!native_acquired.load(std::memory_order_acquire));
    release_fiber.store(true, std::memory_order_release);
    GO2CPP_CHECK(fiber_owner->wait_for(2s));
    GO2CPP_JOIN_WITH_WATCHDOG(native_waiter, 3s);
    GO2CPP_CHECK(native_acquired.load(std::memory_order_acquire));

    // Native waits honor the same Context timeout/cancellation callbacks.
    GO2CPP_CHECK(mutex.Lock());
    std::atomic<bool> timeout_entered{false};
    std::atomic<bool> timeout_result{true};
    std::thread timeout_waiter([&] {
        timeout_entered.store(true, std::memory_order_release);
        timeout_result.store(mutex.LockFor(20ms), std::memory_order_release);
        if (timeout_result.load(std::memory_order_acquire)) {
            mutex.Unlock();
        }
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return timeout_entered.load(std::memory_order_acquire); }));
    GO2CPP_JOIN_WITH_WATCHDOG(timeout_waiter, 3s);
    GO2CPP_CHECK(!timeout_result.load(std::memory_order_acquire));
    mutex.Unlock();

    GO2CPP_CHECK(mutex.Lock());
    auto native_cancel = go2cpp::WithCancel(go2cpp::Background());
    std::atomic<bool> cancel_entered{false};
    std::atomic<bool> cancel_result{true};
    std::thread cancel_waiter([&] {
        cancel_entered.store(true, std::memory_order_release);
        cancel_result.store(mutex.Lock(native_cancel.first),
                            std::memory_order_release);
        if (cancel_result.load(std::memory_order_acquire)) {
            mutex.Unlock();
        }
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return cancel_entered.load(std::memory_order_acquire); }));
    native_cancel.second();
    GO2CPP_JOIN_WITH_WATCHDOG(cancel_waiter, 3s);
    GO2CPP_CHECK(!cancel_result.load(std::memory_order_acquire));
    mutex.Unlock();

    // A native CV waiter can be notified by a managed G.
    go2cpp::sync::ConditionVariable condition;
    std::atomic<bool> native_ready{false};
    std::atomic<bool> native_cv_started{false};
    std::atomic<bool> native_cv_result{false};
    std::thread native_cv_waiter([&] {
        GO2CPP_CHECK(mutex.Lock());
        native_cv_started.store(true, std::memory_order_release);
        const auto ready_deadline = std::chrono::steady_clock::now() + 3s;
        while (!native_ready.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < ready_deadline) {
            const auto remaining = ready_deadline -
                                   std::chrono::steady_clock::now();
            const std::chrono::steady_clock::duration max_wait = 2s;
            const auto wait_duration =
                remaining < max_wait ? remaining : max_wait;
            if (!condition.WaitFor(mutex, wait_duration)) {
                break;
            }
        }
        if (!native_ready.load(std::memory_order_acquire)) {
            mutex.Unlock();
            go2cpp_tests::watchdog_abort("native condition notification",
                                         __FILE__, __LINE__);
        }
        native_cv_result.store(true, std::memory_order_release);
        mutex.Unlock();
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return native_cv_started.load(std::memory_order_acquire); }));
    auto managed_notifier = scheduler.spawn([&] {
        GO2CPP_CHECK(mutex.Lock());
        native_ready.store(true, std::memory_order_release);
        condition.NotifyOne();
        mutex.Unlock();
    });
    GO2CPP_CHECK(managed_notifier->wait_for(2s));
    GO2CPP_JOIN_WITH_WATCHDOG(native_cv_waiter, 3s);
    GO2CPP_CHECK(native_cv_result.load(std::memory_order_acquire));

    // A managed CV waiter can be notified by an ordinary thread.
    native_ready.store(false, std::memory_order_release);
    std::atomic<bool> managed_cv_started{false};
    std::atomic<bool> managed_cv_result{false};
    auto managed_cv_waiter = scheduler.spawn([&] {
        GO2CPP_CHECK(mutex.Lock());
        managed_cv_started.store(true, std::memory_order_release);
        const auto ready_deadline = std::chrono::steady_clock::now() + 3s;
        while (!native_ready.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < ready_deadline) {
            const auto remaining = ready_deadline -
                                   std::chrono::steady_clock::now();
            const std::chrono::steady_clock::duration max_wait = 2s;
            const auto wait_duration =
                remaining < max_wait ? remaining : max_wait;
            if (!condition.WaitFor(mutex, wait_duration)) {
                break;
            }
        }
        if (!native_ready.load(std::memory_order_acquire)) {
            mutex.Unlock();
            go2cpp_tests::watchdog_abort("managed condition notification",
                                         __FILE__, __LINE__);
        }
        managed_cv_result.store(true, std::memory_order_release);
        mutex.Unlock();
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return managed_cv_started.load(std::memory_order_acquire); }));
    std::thread native_notifier([&] {
        GO2CPP_CHECK(mutex.Lock());
        native_ready.store(true, std::memory_order_release);
        condition.NotifyOne();
        mutex.Unlock();
    });
    GO2CPP_JOIN_WITH_WATCHDOG(native_notifier, 3s);
    GO2CPP_CHECK(managed_cv_waiter->wait_for(2s));
    GO2CPP_CHECK(managed_cv_result.load(std::memory_order_acquire));

    // WaitGroup uses the same dual-mode waiter: native Wait/Fiber Done and
    // Fiber Wait/native Done are both valid.
    go2cpp::sync::WaitGroup group;
    group.Add(1);
    std::atomic<bool> native_group_started{false};
    std::atomic<bool> native_group_result{false};
    std::thread native_group_waiter([&] {
        native_group_started.store(true, std::memory_order_release);
        native_group_result.store(group.WaitFor(2s), std::memory_order_release);
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return native_group_started.load(std::memory_order_acquire); }));
    auto managed_done = scheduler.spawn([&] { group.Done(); });
    GO2CPP_CHECK(managed_done->wait_for(2s));
    GO2CPP_JOIN_WITH_WATCHDOG(native_group_waiter, 3s);
    GO2CPP_CHECK(native_group_result.load(std::memory_order_acquire));

    group.Add(1);
    std::atomic<bool> managed_group_result{false};
    auto managed_group_waiter = scheduler.spawn([&] {
        managed_group_result.store(group.WaitFor(2s),
                                   std::memory_order_release);
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return managed_group_waiter->state() == go2cpp::GState::kWaiting; }));
    std::thread native_done([&] { group.Done(); });
    GO2CPP_JOIN_WITH_WATCHDOG(native_done, 3s);
    GO2CPP_CHECK(managed_group_waiter->wait_for(2s));
    GO2CPP_CHECK(managed_group_result.load(std::memory_order_acquire));

    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 验证已取消 Context 下条件变量和 WaitGroup 立即返回。
 * @return 无；测试失败由统一断言统计。
 */
void TestImmediateConditionAndWaitGroupAbort() {
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable condition;

    // An already-cancelled native context still follows the CV unlock/relock
    // contract. The caller owns the mutex after the false result.
    GO2CPP_CHECK(mutex.Lock());
    auto cancelled = go2cpp::WithCancel(go2cpp::Background());
    cancelled.second();
    GO2CPP_CHECK(!condition.Wait(mutex, cancelled.first));
    const bool native_probe_after_cancel = mutex.TryLock();
    // 断言：Wait(false) 返回时调用方必须仍持有 mutex（契约规定的
    // unlock/relock）。旧注释写成「未持锁的回归也算通过」，与断言相反；
    // 这里只承认「仍持锁」一种结果。
    GO2CPP_CHECK(!native_probe_after_cancel);
    mutex.Unlock();

    // Zero and negative durations create an already-expired context
    // synchronously; they must not strand a native caller or leave an
    // unexpected lock state.
    GO2CPP_CHECK(mutex.Lock());
    GO2CPP_CHECK(!condition.WaitFor(mutex, 0ms));
    const bool native_probe_after_timeout = mutex.TryLock();
    GO2CPP_CHECK(!native_probe_after_timeout);
    mutex.Unlock();

    go2cpp::sync::WaitGroup group;
    group.Add(1);
    GO2CPP_CHECK(!group.WaitFor(0ms));
    auto group_cancel = go2cpp::WithCancel(go2cpp::Background());
    group_cancel.second();
    GO2CPP_CHECK(!group.Wait(group_cancel.first));
    group.Done();
    GO2CPP_CHECK(group.Wait());

    // Exercise the same immediate paths from a managed G. The task remains
    // runnable because the timeout/cancellation is observed before publication.
    go2cpp::Scheduler scheduler(SchedulerConfig(1));
    scheduler.start();
    std::atomic<bool> managed_cv_done{false};
    std::atomic<bool> managed_cv_relocked{false};
    auto managed_cv = scheduler.spawn([&] {
        GO2CPP_CHECK(mutex.Lock());
        const bool result = condition.WaitFor(mutex, 0ms);
        const bool probe = mutex.TryLock();
        if (probe) {
            mutex.Unlock();
        } else {
            managed_cv_relocked.store(true, std::memory_order_release);
            mutex.Unlock();
        }
        GO2CPP_CHECK(!result);
        managed_cv_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(managed_cv->wait_for(2s));
    GO2CPP_CHECK(managed_cv_done.load(std::memory_order_acquire));
    GO2CPP_CHECK(managed_cv_relocked.load(std::memory_order_acquire));

    group.Add(1);
    std::atomic<bool> managed_group_done{false};
    std::atomic<bool> managed_group_result{true};
    auto managed_group = scheduler.spawn([&] {
        managed_group_result.store(group.WaitFor(0ms),
                                    std::memory_order_release);
        managed_group_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(managed_group->wait_for(2s));
    GO2CPP_CHECK(managed_group_done.load(std::memory_order_acquire));
    GO2CPP_CHECK(!managed_group_result.load(std::memory_order_acquire));
    group.Done();
    GO2CPP_CHECK(group.Wait());
    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 验证手动 Fiber 争用同步原语时不会永久阻塞承载线程。
 * @return 无；测试失败由统一断言统计。
 */
void TestManualFiberDoesNotBlockCarrier() {
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable condition;
    go2cpp::sync::WaitGroup group;
    group.Add(1);
    auto channel = go2cpp::MakeChannel<int>(0);
    std::atomic<bool> finished{false};
    std::atomic<bool> mutex_result{true};
    std::atomic<bool> condition_result{true};
    std::atomic<bool> group_result{true};
    std::atomic<go2cpp::ChannelStatus> channel_result{
        go2cpp::ChannelStatus::kReady};

    GO2CPP_CHECK(mutex.Lock());
    go2cpp::Fiber fiber([&] {
        // There is no Scheduler::current_task() for this manually resumed
        // Fiber. Contended waits must return promptly instead of blocking the
        // thread that owns the Fiber stack.
        mutex_result.store(mutex.Lock(), std::memory_order_release);
        condition_result.store(condition.Wait(mutex), std::memory_order_release);
        group_result.store(group.WaitFor(50ms), std::memory_order_release);
        channel_result.store(channel->RecvFor(50ms).status,
                             std::memory_order_release);
        finished.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(fiber.resume());
    GO2CPP_CHECK(finished.load(std::memory_order_acquire));
    GO2CPP_CHECK(!mutex_result.load(std::memory_order_acquire));
    GO2CPP_CHECK(!condition_result.load(std::memory_order_acquire));
    GO2CPP_CHECK(!group_result.load(std::memory_order_acquire));
    GO2CPP_CHECK(channel_result.load(std::memory_order_acquire) ==
                 go2cpp::ChannelStatus::kInvalid);

    // 即使 Context 已取消，手动 Fiber 也必须保留调用方持有的锁，
    // 不能走普通线程的 unlock/relock 路径而阻塞 carrier。
    auto cancelled_context = go2cpp::WithCancel(go2cpp::Background());
    cancelled_context.second();
    std::atomic<bool> cancelled_condition_result{true};
    go2cpp::Fiber cancelled_fiber([&] {
        cancelled_condition_result.store(
            condition.Wait(mutex, cancelled_context.first),
            std::memory_order_release);
    });
    GO2CPP_CHECK(cancelled_fiber.resume());
    GO2CPP_CHECK(!cancelled_condition_result.load(std::memory_order_acquire));
    GO2CPP_CHECK(!mutex.TryLock());

    // ConditionVariable's constrained manual-Fiber path preserves the lock.
    mutex.Unlock();
    group.Done();
}

/**
 * @brief 验证同步等待队列 FIFO 顺序和单 P 下的任务推进。
 * @return 无；测试失败由统一断言统计。
 */
void TestFifoAndSingleProcessorProgress() {
    go2cpp::Scheduler scheduler(SchedulerConfig(1));
    go2cpp::sync::Mutex mutex;
    constexpr int waiter_count = 12;
    std::atomic<int> entered{0};
    std::atomic<int> completed{0};
    std::mutex order_mutex;
    std::vector<int> order;

    GO2CPP_CHECK(mutex.Lock());
    for (int index = 0; index < waiter_count; ++index) {
        scheduler.spawn([&, index] {
            entered.fetch_add(1, std::memory_order_release);
            if (mutex.Lock()) {
                {
                    std::lock_guard<std::mutex> lock(order_mutex);
                    order.push_back(index);
                }
                mutex.Unlock();
            }
            completed.fetch_add(1, std::memory_order_release);
        });
    }
    scheduler.start();
    GO2CPP_CHECK(WaitUntil([&] {
        return entered.load(std::memory_order_acquire) == waiter_count;
    }));
    mutex.Unlock();
    GO2CPP_CHECK(WaitUntil([&] {
        return completed.load(std::memory_order_acquire) == waiter_count;
    }));
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        GO2CPP_CHECK(order.size() == waiter_count);
        for (int index = 0; index < waiter_count; ++index) {
            GO2CPP_CHECK(order[static_cast<std::size_t>(index)] == index);
        }
    }

    // The first G must suspend without occupying the only M so the second G
    // can drive the counter to zero and wake it.
    go2cpp::sync::WaitGroup group;
    group.Add(1);
    std::atomic<bool> wait_started{false};
    std::atomic<bool> wait_finished{false};
    scheduler.spawn([&] {
        wait_started.store(true, std::memory_order_release);
        wait_finished.store(group.WaitFor(3s), std::memory_order_release);
    });
    scheduler.spawn([&] { group.Done(); });
    GO2CPP_CHECK(WaitUntil([&] {
        return wait_started.load(std::memory_order_acquire) &&
               wait_finished.load(std::memory_order_acquire);
    }));

    // Reuse after zero is a distinct wave.
    group.Add(1);
    std::atomic<bool> second_wave{false};
    scheduler.spawn([&] {
        second_wave.store(group.WaitFor(3s), std::memory_order_release);
    });
    GO2CPP_CHECK(WaitUntil([&] {
        return scheduler.runnable_count() == 0;
    }));
    group.Done();
    GO2CPP_CHECK(WaitUntil(
        [&] { return second_wave.load(std::memory_order_acquire); }));

    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 验证锁、条件变量和 WaitGroup 的 Context/超时行为。
 * @return 无；测试失败由统一断言统计。
 */
void TestContextAndTimeout() {
    go2cpp::Scheduler scheduler(SchedulerConfig(1));
    scheduler.start();

    go2cpp::sync::Mutex mutex;
    GO2CPP_CHECK(mutex.Lock());
    auto cancel_pair = go2cpp::WithCancel(go2cpp::Background());
    std::atomic<bool> lock_started{false};
    std::atomic<bool> lock_done{false};
    std::atomic<bool> lock_result{true};
    scheduler.spawn([&] {
        lock_started.store(true, std::memory_order_release);
        lock_result.store(mutex.Lock(cancel_pair.first),
                          std::memory_order_release);
        lock_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return lock_started.load(std::memory_order_acquire); }));
    cancel_pair.second();
    GO2CPP_CHECK(WaitUntil(
        [&] { return lock_done.load(std::memory_order_acquire); }));
    GO2CPP_CHECK(!lock_result.load(std::memory_order_acquire));
    mutex.Unlock();

    go2cpp::sync::WaitGroup group;
    group.Add(1);
    std::atomic<bool> timed_done{false};
    std::atomic<bool> timed_result{true};
    scheduler.spawn([&] {
        timed_result.store(group.WaitFor(20ms), std::memory_order_release);
        timed_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return timed_done.load(std::memory_order_acquire); }));
    GO2CPP_CHECK(!timed_result.load(std::memory_order_acquire));
    group.Done();

    go2cpp::sync::Mutex condition_mutex;
    go2cpp::sync::ConditionVariable condition;
    std::atomic<bool> condition_done{false};
    std::atomic<bool> condition_result{true};
    scheduler.spawn([&] {
        GO2CPP_CHECK(condition_mutex.Lock());
        condition_result.store(condition.WaitFor(condition_mutex, 20ms),
                               std::memory_order_release);
        condition_mutex.Unlock();
        condition_done.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return condition_done.load(std::memory_order_acquire); }));
    GO2CPP_CHECK(!condition_result.load(std::memory_order_acquire));

    ShutdownOrReportLostWakeup(scheduler);
}

struct NotifyRaceState {
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable condition;
    std::atomic<bool> waiter_entered{false};
    std::atomic<int> completed{0};
    std::atomic<bool> wait_result{false};
};

/**
 * @brief 验证通知先于等待 Fiber park 的竞态不会丢失唤醒。
 * @return 无；测试失败由统一断言统计。
 */
void TestNotifyBeforeParkRace() {
    go2cpp::Scheduler scheduler(SchedulerConfig(2));
    scheduler.start();
    std::mt19937 random(0xBADC0DEU);

    // 通知方先竞争同一把锁。Wait 发布节点并释放锁后，Unlock 会把锁交给
    // 另一 M 上的通知方，使 NotifyOne 与等待 Fiber 的 park 直接交错。
    // 固定 seed 在持锁阶段插入不同次数的主动让出，扩大时序覆盖并保证复现。
    for (int iteration = 0; iteration < 256; ++iteration) {
        auto state = std::make_shared<NotifyRaceState>();
        const unsigned waiter_yields = random() % 4U;
        const unsigned notifier_yields = random() % 4U;
        auto waiter_task = scheduler.spawn([state, &scheduler, waiter_yields] {
            GO2CPP_CHECK(state->mutex.Lock());
            state->waiter_entered.store(true, std::memory_order_release);
            for (unsigned index = 0; index < waiter_yields; ++index) {
                (void)scheduler.yield_current();
            }
            state->wait_result.store(
                state->condition.WaitFor(state->mutex, 500ms),
                std::memory_order_release);
            state->mutex.Unlock();
            state->completed.fetch_add(1, std::memory_order_release);
        });
        auto notifier_task = scheduler.spawn(
            [state, &scheduler, notifier_yields] {
            GO2CPP_REQUIRE_EVENTUALLY(
                state->waiter_entered.load(std::memory_order_acquire), 3s);
            GO2CPP_CHECK(state->mutex.Lock());
            for (unsigned index = 0; index < notifier_yields; ++index) {
                (void)scheduler.yield_current();
            }
            state->condition.NotifyOne();
            state->mutex.Unlock();
            state->completed.fetch_add(1, std::memory_order_release);
        });
        const bool completed = WaitUntil([&] {
            return state->completed.load(std::memory_order_acquire) == 2;
        });
        GO2CPP_CHECK(waiter_task != nullptr);
        GO2CPP_CHECK(notifier_task != nullptr);
        GO2CPP_CHECK(completed);
        GO2CPP_CHECK(state->wait_result.load(std::memory_order_acquire));
    }

    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 验证 G 取消前后进入 ConditionVariable::Wait 时都能重新持有 mutex。
 * @details 固定 seed 扰动通知/取消与锁释放顺序；每次等待和任务回收都有 watchdog。
 * @return 无；失败由统一断言统计。
 */
void TestConditionCancellationAlwaysRelocks() {
    go2cpp::Scheduler scheduler(SchedulerConfig(2));
    scheduler.start();

    // G 在调用 Wait 之前已被取消。提前返回也必须经过专用重锁路径，
    // 普通 Mutex::Lock 会看到取消标记并拒绝获取锁。
    for (int iteration = 0; iteration < 32; ++iteration) {
        go2cpp::sync::Mutex mutex;
        go2cpp::sync::ConditionVariable condition;
        std::atomic<bool> entered{false};
        std::atomic<bool> allow_wait{false};
        std::atomic<bool> result{true};
        std::atomic<bool> owns_mutex{false};
        auto task = scheduler.spawn([&] {
            GO2CPP_CHECK(mutex.Lock());
            entered.store(true, std::memory_order_release);
            while (!allow_wait.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            result.store(condition.Wait(mutex), std::memory_order_release);
            const bool probe_acquired = mutex.TryLock();
            owns_mutex.store(!probe_acquired, std::memory_order_release);
            mutex.Unlock();
            if (probe_acquired) {
                mutex.Unlock();
            }
        });
        GO2CPP_REQUIRE_EVENTUALLY(entered.load(std::memory_order_acquire), 2s);
        GO2CPP_CHECK(task->cancel());
        allow_wait.store(true, std::memory_order_release);
        GO2CPP_CHECK(task->wait_for(2s));
        GO2CPP_CHECK(!result.load(std::memory_order_acquire));
        GO2CPP_CHECK(owns_mutex.load(std::memory_order_acquire));
    }

    // 已注册的 Wait 被 Scheduler 取消后，另一个 G 暂时持有 mutex。
    // 被取消的 G 必须在 cleanup park 中等待 owner 释放锁，再以持锁状态返回。
    std::mt19937 random(0xC0A7E11U);
    constexpr int kRaceIterations = 256;
    for (int iteration = 0; iteration < kRaceIterations; ++iteration) {
        go2cpp::sync::Mutex mutex;
        go2cpp::sync::ConditionVariable condition;
        std::atomic<bool> waiter_ready{false};
        std::atomic<bool> holder_locked{false};
        std::atomic<bool> release_holder{false};
        std::atomic<bool> wait_result{true};
        std::atomic<bool> owns_mutex{false};

        auto waiter = scheduler.spawn([&] {
            GO2CPP_CHECK(mutex.Lock());
            waiter_ready.store(true, std::memory_order_release);
            wait_result.store(condition.Wait(mutex), std::memory_order_release);
            const bool probe_acquired = mutex.TryLock();
            owns_mutex.store(!probe_acquired, std::memory_order_release);
            if (probe_acquired) {
                mutex.Unlock();
            }
            mutex.Unlock();
        });
        auto holder = scheduler.spawn([&] {
            GO2CPP_REQUIRE_EVENTUALLY(
                waiter_ready.load(std::memory_order_acquire), 2s);
            GO2CPP_CHECK(mutex.Lock());
            holder_locked.store(true, std::memory_order_release);
            while (!release_holder.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            mutex.Unlock();
        });

        GO2CPP_REQUIRE_EVENTUALLY(
            holder_locked.load(std::memory_order_acquire), 2s);
        GO2CPP_REQUIRE_EVENTUALLY(
            waiter->state() == go2cpp::GState::kWaiting, 2s);
        GO2CPP_CHECK(waiter->cancel());
        const auto jitter = std::chrono::microseconds(random() % 200U);
        std::this_thread::sleep_for(jitter);
        release_holder.store(true, std::memory_order_release);

        GO2CPP_CHECK(waiter->wait_for(2s));
        GO2CPP_CHECK(holder->wait_for(2s));
        GO2CPP_CHECK(!wait_result.load(std::memory_order_acquire));
        GO2CPP_CHECK(owns_mutex.load(std::memory_order_acquire));
    }

    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 验证无关唤醒令牌不会误结束新的等待。
 * @return 无；测试失败由统一断言统计。
 */
void TestSpuriousWakeDoesNotCancelWait() {
    go2cpp::Scheduler scheduler(SchedulerConfig(1));
    go2cpp::sync::WaitGroup group;
    group.Add(1);
    std::atomic<bool> result{false};
    scheduler.spawn([&] {
        // An earlier, unrelated wake permit must not cancel this new wait.
        (void)scheduler.wake(go2cpp::Scheduler::current_task());
        result.store(group.WaitFor(3s), std::memory_order_release);
    });
    scheduler.spawn([&] { group.Done(); });
    scheduler.start();
    GO2CPP_CHECK(WaitUntil(
        [&] { return result.load(std::memory_order_acquire); }));
    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 验证 WaitGroup 的一轮释放完成后可以安全复用。
 * @return 无；测试失败由统一断言统计。
 */
void TestWaitGroupWaveReleasedBeforeReuse() {
    go2cpp::Scheduler scheduler(SchedulerConfig(1));
    go2cpp::sync::WaitGroup group;
    group.Add(1);
    scheduler.start();
    std::atomic<bool> first_result{false};
    auto first = scheduler.spawn([&] {
        first_result.store(group.WaitFor(3s), std::memory_order_release);
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return first->state() == go2cpp::GState::kWaiting; }));

    // Occupy the sole M briefly so the released old wave cannot run before
    // Add starts the next one. The watchdog still releases the M on failure.
    std::atomic<bool> blocker_entered{false};
    std::atomic<bool> release_blocker{false};
    auto blocker = scheduler.spawn([&] {
        blocker_entered.store(true, std::memory_order_release);
        const auto watchdog = std::chrono::steady_clock::now() + 2s;
        while (!release_blocker.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < watchdog) {
            go2cpp_tests::yield_for_watchdog();
        }
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return blocker_entered.load(std::memory_order_acquire); }));
    group.Done();
    group.Add(1);
    release_blocker.store(true, std::memory_order_release);
    GO2CPP_CHECK(blocker->wait_for(2s));
    GO2CPP_CHECK(first->wait_for(2s));
    GO2CPP_CHECK(first_result.load(std::memory_order_acquire));
    GO2CPP_CHECK(group.Count() == 1);

    std::atomic<bool> second_result{false};
    auto second = scheduler.spawn([&] {
        second_result.store(group.WaitFor(3s), std::memory_order_release);
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return second->state() == go2cpp::GState::kWaiting; }));
    GO2CPP_CHECK(!second_result.load(std::memory_order_acquire));
    group.Done();
    GO2CPP_CHECK(second->wait_for(2s));
    GO2CPP_CHECK(second_result.load(std::memory_order_acquire));
    ShutdownOrReportLostWakeup(scheduler);
}

/**
 * @brief 验证解锁与 Context 取消同时发生时等待者只完成一次。
 * @return 无；测试失败由统一断言统计。
 */
void TestUnlockRacesContextCancellation() {
    go2cpp::Scheduler scheduler(SchedulerConfig(2));
    scheduler.start();
    for (int iteration = 0; iteration < 128; ++iteration) {
        go2cpp::sync::Mutex mutex;
        GO2CPP_CHECK(mutex.Lock());
        auto cancellation = go2cpp::WithCancel(go2cpp::Background());
        std::atomic<bool> returned{false};
        auto waiter = scheduler.spawn([&] {
            if (mutex.Lock(cancellation.first)) {
                mutex.Unlock();
            }
            returned.store(true, std::memory_order_release);
        });
        GO2CPP_CHECK(WaitUntil(
            [&] { return waiter->state() == go2cpp::GState::kWaiting; }));
        std::atomic<bool> race{false};
        std::thread canceller([&] {
            const auto race_deadline =
                std::chrono::steady_clock::now() + 3s;
            while (!race.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < race_deadline) {
                go2cpp_tests::yield_for_watchdog();
            }
            if (!race.load(std::memory_order_acquire)) {
                go2cpp_tests::watchdog_abort("sync race start", __FILE__,
                                             __LINE__);
            }
            cancellation.second();
        });
        std::thread unlocker([&] {
            const auto race_deadline =
                std::chrono::steady_clock::now() + 3s;
            while (!race.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < race_deadline) {
                go2cpp_tests::yield_for_watchdog();
            }
            if (!race.load(std::memory_order_acquire)) {
                go2cpp_tests::watchdog_abort("sync race start", __FILE__,
                                             __LINE__);
            }
            mutex.Unlock();
        });
        race.store(true, std::memory_order_release);
        GO2CPP_JOIN_WITH_WATCHDOG(canceller, 3s);
        GO2CPP_JOIN_WITH_WATCHDOG(unlocker, 3s);
        GO2CPP_CHECK(waiter->wait_for(2s));
        GO2CPP_CHECK(returned.load(std::memory_order_acquire));
        const bool acquired = mutex.TryLock();
        GO2CPP_CHECK(acquired);
        if (acquired) {
            mutex.Unlock();
        }
    }
    ShutdownOrReportLostWakeup(scheduler);
}

struct StackRelease final {
    explicit StackRelease(std::atomic<int>& released) : m_released(released) {}
    ~StackRelease() { m_released.fetch_add(1, std::memory_order_release); }
    std::atomic<int>& m_released;
};

/**
 * @brief 验证 Context 回调与 Scheduler 销毁并发时资源完整释放。
 * @return 无；测试失败由统一断言统计。
 */
void TestContextCallbackRacesSchedulerShutdown() {
    for (int iteration = 0; iteration < 64; ++iteration) {
        auto scheduler =
            std::make_unique<go2cpp::Scheduler>(SchedulerConfig(2));
        go2cpp::sync::WaitGroup group;
        group.Add(1);
        auto cancellation = go2cpp::WithCancel(go2cpp::Background());
        std::atomic<int> released{0};
        std::atomic<bool> returned{false};
        scheduler->start();
        auto waiter = scheduler->spawn([&] {
            StackRelease release(released);
            GO2CPP_CHECK(!group.WaitFor(3s, cancellation.first));
            returned.store(true, std::memory_order_release);
        });
        GO2CPP_CHECK(WaitUntil(
            [&] { return waiter->state() == go2cpp::GState::kWaiting; }));
        std::atomic<bool> race{false};
        std::thread canceller([&] {
            const auto race_deadline =
                std::chrono::steady_clock::now() + 3s;
            while (!race.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < race_deadline) {
                go2cpp_tests::yield_for_watchdog();
            }
            if (!race.load(std::memory_order_acquire)) {
                go2cpp_tests::watchdog_abort("sync race start", __FILE__,
                                             __LINE__);
            }
            cancellation.second();
        });
        race.store(true, std::memory_order_release);
        ShutdownOrReportLostWakeup(*scheduler);
        scheduler.reset();
        GO2CPP_JOIN_WITH_WATCHDOG(canceller, 3s);
        GO2CPP_CHECK(returned.load(std::memory_order_acquire));
        GO2CPP_CHECK(released.load(std::memory_order_acquire) == 1);
        GO2CPP_CHECK(waiter->wait_for(100ms));
        // The queue must no longer retain the finished waiter's scheduler.
        group.Done();
    }
}

}  // namespace

/**
 * @brief 持锁 park 检测：G1 持着 outer 去等 inner，检测计数必须增长。
 *
 * @details 这是"持着锁等另一把锁"的最小可复现场景，对应真实代码里
 *          "持锁读 fd（hook 后 park）"。检测只计数/告警，不干预语义：
 *          释放 inner 后 G1 必须正常拿到 inner、释放两把锁并退出。
 */
void TestHeldLockParkDetection() {
    const bool saved = go2cpp::sync::HeldLockParkDetection();
    go2cpp::sync::SetHeldLockParkDetection(true);
    const std::uint64_t baseline = go2cpp::sync::HeldLockParkCount();

    go2cpp::Scheduler scheduler(SchedulerConfig(2));
    scheduler.start();
    go2cpp::sync::Mutex outer;
    go2cpp::sync::Mutex inner;
    std::atomic<bool> inner_held{false};
    std::atomic<bool> release_inner{false};
    std::atomic<bool> done{false};

    auto holder = scheduler.spawn([&] {
        inner.Lock();
        inner_held.store(true, std::memory_order_release);
        while (!release_inner.load(std::memory_order_acquire)) {
            scheduler.yield_current();
        }
        inner.Unlock();
    });
    // 先确定 holder 已持有 inner，再让 waiter 去抢，避免抢跑。
    GO2CPP_REQUIRE_EVENTUALLY(inner_held.load(std::memory_order_acquire), 2s);
    auto waiter = scheduler.spawn([&] {
        outer.Lock();
        inner.Lock();  // 持着 outer 等 inner：持锁 park
        inner.Unlock();
        outer.Unlock();
        done.store(true, std::memory_order_release);
    });

    // 等待 G1 真正以"持锁"状态 park。
    GO2CPP_REQUIRE_EVENTUALLY(
        go2cpp::sync::HeldLockParkCount() > baseline, 2s);
    release_inner.store(true, std::memory_order_release);
    GO2CPP_REQUIRE_EVENTUALLY(done.load(std::memory_order_acquire), 2s);
    GO2CPP_CHECK(go2cpp::sync::HeldLockParkDetection());
    holder->wait();
    waiter->wait();
    ShutdownOrReportLostWakeup(scheduler);
    go2cpp::sync::SetHeldLockParkDetection(saved);
}

/**
 * @brief P1 回归：LockFor 超时清理不得抹掉并发 TryLock 写入的 kLocked。
 *
 * @details 修复前 `LockImpl` 的超时/取消清理用非原子 `load` + `store`
 *   改写状态字：读到「锁空闲」后，另一线程的 `TryLock` 可能已用 CAS 置位
 *   `kLocked`，随后的 `store` 会用旧快照覆盖它，导致两个执行流同时持锁
 *   （实测 3 秒 9–34 次 `unlock of unlocked mutex`、629–1683 次双持）。
 *   修复后状态写入统一为 CAS 循环；本用例在 1 秒内同时压两条路径，断言
 *   双方 `Unlock` 都不抛异常、helper 持锁窗口内主线程不得成功加锁。
 */
void TestMutexTimeoutCleanupRace() {
    go2cpp::sync::Mutex mutex;
    std::atomic<bool> stop{false};
    std::atomic<bool> holding{false};
    std::atomic<long> helper_unlock_errors{0};
    std::thread helper([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (!mutex.TryLock()) {
                std::this_thread::yield();
                continue;
            }
            holding.store(true, std::memory_order_seq_cst);
            std::this_thread::sleep_for(50us);
            holding.store(false, std::memory_order_seq_cst);
            try {
                mutex.Unlock();
            } catch (const std::logic_error&) {
                helper_unlock_errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    std::mt19937 rng(20261030U);
    long overlaps = 0;
    long main_unlock_errors = 0;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!mutex.LockFor(std::chrono::microseconds(1 + rng() % 100U))) {
            continue;
        }
        // 主线程已持锁时 helper 不可能再拿到锁；仍看到持锁标记即双持。
        if (holding.load(std::memory_order_seq_cst)) {
            ++overlaps;
        }
        try {
            mutex.Unlock();
        } catch (const std::logic_error&) {
            ++main_unlock_errors;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    helper.join();
    GO2CPP_CHECK(helper_unlock_errors.load(std::memory_order_relaxed) == 0);
    GO2CPP_CHECK(overlaps == 0);
    GO2CPP_CHECK(main_unlock_errors == 0);
    // 所有线程退出后锁必须可再次获取（状态位无残留）。
    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();
}

/**
 * @brief 运行混合同步原语和关闭竞态测试。
 * @return 无；测试失败由统一断言统计。
 */
void run_sync_tests() {
    go2cpp_tests::announce("scheduler-aware synchronization");
    TestUnmanagedBoundaryAndCounterErrors();
    TestMixedThreadAndFiberSynchronization();
    TestImmediateConditionAndWaitGroupAbort();
    TestManualFiberDoesNotBlockCarrier();
    TestFifoAndSingleProcessorProgress();
    TestContextAndTimeout();
    TestNotifyBeforeParkRace();
    TestConditionCancellationAlwaysRelocks();
    TestSpuriousWakeDoesNotCancelWait();
    TestWaitGroupWaveReleasedBeforeReuse();
    TestUnlockRacesContextCancellation();
    TestContextCallbackRacesSchedulerShutdown();
    TestHeldLockParkDetection();
    TestMutexTimeoutCleanupRace();
}

#ifdef GO2CPP_SYNC_TEST_MAIN
#include <iostream>

/**
 * @brief 同步测试的独立入口。
 * @return 全部测试通过返回 0，否则返回非零值。
 */
int main() {
    run_sync_tests();
    const int failures =
        go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::cerr << "[test] failures=" << failures << '\n';
        return 1;
    }
    std::cout << "[test] all sync checks passed\n";
    return 0;
}
#endif
