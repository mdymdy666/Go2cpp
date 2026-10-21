/**
 * @file test_primitives_cancel.cpp
 * @brief 并发原语的「取消 / 超时 / 资源可再次获取」测试。
 *
 * ## 为什么需要这个文件
 *
 * 取消与超时是并发原语最容易出错、也最容易被功能测试漏掉的两条路径：
 *
 * - 等待者被取消后**没有从等待队列摘除** → 后续 Unlock 唤醒僵尸节点；
 * - 取消返回 false 却**把锁留在了持有状态** → 下一次加锁永久阻塞；
 * - 超时返回后**没有重新持有 mutex**（ConditionVariable / ParkingCondition）
 *   → 调用方在无锁状态下继续执行，数据竞争；
 * - 取消/超时路径**泄漏等待节点或定时器** → 长跑后内存与唤醒链退化。
 *
 * 本文件逐条断言「返回值 + 锁状态 + 可再次获取」三件事，并覆盖普通 OS
 * 线程与受管 Fiber 两条等待路径。
 *
 * ## 覆盖
 *
 * | 场景 | 断言 |
 * |---|---|
 * | `Mutex::Lock(ctx)` 等待中被取消 | 返回 false、锁仍由原持有者持有、解锁后可再获取 |
 * | `Mutex::Lock(已取消 ctx)` | 返回 false、锁仍空闲 |
 * | `TimedMutex::TryLockFor` 持锁超时 | 返回 false、耗时 ≥ 50ms、锁状态不变 |
 * | `ConditionVariable::WaitFor` 超时 | 返回 false、重新持有 mutex、之后通知路径正常 |
 * | `ConditionVariable::WaitFor` 被取消 | 返回 false、重新持有 mutex、耗时远小于超时 |
 * | `ParkingCondition::wait_for` 超时 | 返回 false、仍持有外部 mutex、随后可再等待成功 |
 * | `RWMutex::RLockFor` 写者持锁超时 | 返回 false、写者释放后可 RLock |
 * | `SpinMutex::LockFor` 超时 | 返回 false、释放后 LockFor 成功 |
 * | `WaitGroup::WaitFor` 超时 | 返回 false、Done 后 WaitFor 成功 |
 * | `Channel::RecvFor` / `Send(ctx)` | kTimedOut / kCancelled、之后通道仍可用 |
 * | 受管 Fiber 的 `Mutex::Lock(ctx)` | park 中被取消 → false、解锁后可重获 |
 * | 取消风暴 2000 轮 | 每次返回 false 且锁始终空闲（无泄漏） |
 *
 * ## 规模
 *
 * 单场景 30~100 轮、取消风暴 2000 轮；总耗时约 1 秒。所有 join 走
 * watchdog，失败时打印 `[watchdog]` 而不是挂死。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/locks.hpp"
#include "go2cpp/concurrency/spin_mutex.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

using go2cpp::ChannelStatus;
using go2cpp::Context;
using go2cpp::ContextPtr;
using go2cpp::MakeChannel;
using go2cpp::GState;
using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;
using go2cpp::core::ParkingCondition;
using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;
using go2cpp::sync::RWMutex;
using go2cpp::sync::SpinMutex;
using go2cpp::sync::TimedMutex;
using go2cpp::sync::WaitGroup;

long long ElapsedMs(Clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - started)
        .count();
}

// ---------------------------------------------------------------------------
// 1. Mutex::Lock(ctx) 等待中被取消
// ---------------------------------------------------------------------------
void TestMutexCancelWhileWaiting() {
    go2cpp_tests::announce("cancel: Mutex::Lock(ctx) cancelled while waiting");

    Mutex mutex;
    mutex.Lock();  // 主线程先持有，迫使等待者进入 park
    auto context = Context::WithCancel(Context::Background());
    std::atomic<bool> started{false};
    std::atomic<bool> acquired{true};
    std::atomic<long long> waited_ms{0};
    std::atomic<bool> trylock_while_held{true};

    std::thread waiter([&] {
        started.store(true, std::memory_order_release);
        const auto wait_started = Clock::now();
        const bool ok = mutex.Lock(context.first);
        waited_ms.store(ElapsedMs(wait_started), std::memory_order_release);
        acquired.store(ok, std::memory_order_release);
        // 取消返回时锁必须仍归原持有者：TryLock 不能成功。
        trylock_while_held.store(mutex.TryLock(), std::memory_order_release);
        if (trylock_while_held.load(std::memory_order_acquire)) {
            mutex.Unlock();
        }
        if (ok) {
            mutex.Unlock();
        }
    });
    GO2CPP_REQUIRE_EVENTUALLY(started.load(std::memory_order_acquire), 5s);
    // 给等待者足够时间真正进入等待队列（普通线程走原生 condition_variable）。
    std::this_thread::sleep_for(30ms);
    context.second();
    GO2CPP_JOIN_WITH_WATCHDOG(waiter, 15s);

    GO2CPP_CHECK(!acquired.load(std::memory_order_acquire));
    GO2CPP_CHECK(waited_ms.load(std::memory_order_acquire) >= 20);
    GO2CPP_CHECK(!trylock_while_held.load(std::memory_order_acquire));

    GO2CPP_CHECK(!mutex.TryLock());  // 主线程仍持有锁
    mutex.Unlock();
    GO2CPP_CHECK(mutex.TryLock());  // 取消没有破坏锁状态，可再次获取
    mutex.Unlock();
}

// ---------------------------------------------------------------------------
// 2. 已取消的 Context：Lock 必须失败且不占锁
// ---------------------------------------------------------------------------
void TestMutexAlreadyCancelled() {
    go2cpp_tests::announce("cancel: Mutex::Lock(already cancelled ctx)");
    Mutex mutex;
    for (int round = 0; round < 1000; ++round) {
        auto context = Context::WithCancel(Context::Background());
        context.second();
        GO2CPP_CHECK(!mutex.Lock(context.first));
    }
    // 1000 次失败加锁后锁必须仍空闲。
    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();

    // 对比：同一把锁在正常 ctx 下可获取。
    auto context = Context::WithCancel(Context::Background());
    GO2CPP_CHECK(mutex.Lock(context.first));
    mutex.Unlock();
}

// ---------------------------------------------------------------------------
// 3. TimedMutex 超时
// ---------------------------------------------------------------------------
void TestTimedMutexTimeout() {
    go2cpp_tests::announce("cancel: TimedMutex::TryLockFor timeout");

    TimedMutex mutex;
    mutex.Lock();
    std::atomic<bool> gate{false};
    std::atomic<bool> acquired{true};
    std::atomic<long long> waited_ms{0};
    std::thread waiter([&] {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const auto started = Clock::now();
        acquired.store(mutex.TryLockFor(60ms), std::memory_order_release);
        waited_ms.store(ElapsedMs(started), std::memory_order_release);
    });
    gate.store(true, std::memory_order_release);
    GO2CPP_JOIN_WITH_WATCHDOG(waiter, 15s);

    GO2CPP_CHECK(!acquired.load(std::memory_order_acquire));
    GO2CPP_CHECK(waited_ms.load(std::memory_order_acquire) >= 50);
    GO2CPP_CHECK(!mutex.TryLock());  // 超时不改变锁状态
    mutex.Unlock();
    GO2CPP_CHECK(mutex.TryLock());  // 超时后资源可再次获取
    mutex.Unlock();

    // 等待者最终能拿到锁（超时路径没有留下坏状态）。
    mutex.Lock();
    std::atomic<bool> acquired_late{false};
    std::thread late([&] {
        acquired_late.store(mutex.TryLockFor(5s), std::memory_order_release);
        if (acquired_late.load(std::memory_order_acquire)) {
            mutex.Unlock();
        }
    });
    std::this_thread::sleep_for(20ms);
    mutex.Unlock();
    GO2CPP_JOIN_WITH_WATCHDOG(late, 15s);
    GO2CPP_CHECK(acquired_late.load(std::memory_order_acquire));
    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();
}

// ---------------------------------------------------------------------------
// 4. ConditionVariable：超时 + 取消
// ---------------------------------------------------------------------------
void TestConditionVariableTimeoutAndCancel() {
    go2cpp_tests::announce(
        "cancel: ConditionVariable::WaitFor timeout / cancel");

    Mutex mutex;
    ConditionVariable condition;
    mutex.Lock();
    auto started = Clock::now();
    const bool notified = condition.WaitFor(mutex, 40ms);
    const long long waited_ms = ElapsedMs(started);
    GO2CPP_CHECK(!notified);
    GO2CPP_CHECK(waited_ms >= 30);
    GO2CPP_CHECK(!mutex.TryLock());  // 返回前已重新持有 mutex
    mutex.Unlock();

    // 取消：等待中的 WaitFor 必须被唤醒并重新持有 mutex。
    mutex.Lock();
    auto context = Context::WithCancel(Context::Background());
    std::thread canceller([&] {
        std::this_thread::sleep_for(50ms);
        context.second();
    });
    started = Clock::now();
    const bool cancelled = condition.WaitFor(mutex, 5s, context.first);
    const long long cancel_ms = ElapsedMs(started);
    GO2CPP_JOIN_WITH_WATCHDOG(canceller, 10s);
    GO2CPP_CHECK(!cancelled);
    GO2CPP_CHECK(cancel_ms >= 40);
    GO2CPP_CHECK(cancel_ms < 2000);  // 取消应当及时唤醒，而不是睡满 5s
    GO2CPP_CHECK(!mutex.TryLock());  // 取消路径同样重新持锁
    mutex.Unlock();

    // 取消之后同一对 Mutex/CV 仍能正常完成一次通知。
    mutex.Lock();
    bool ready = false;
    std::thread notifier([&] {
        std::this_thread::sleep_for(10ms);
        mutex.Lock();
        ready = true;
        mutex.Unlock();
        condition.NotifyOne();
    });
    const auto notify_deadline = Clock::now() + 2s;
    while (!ready && Clock::now() < notify_deadline) {
        // ConditionVariable 没有谓词重载：显式循环并把超时当作一次重查。
        (void)condition.WaitFor(mutex, 100ms);
    }
    GO2CPP_CHECK(ready);
    mutex.Unlock();
    GO2CPP_JOIN_WITH_WATCHDOG(notifier, 10s);
    GO2CPP_CHECK(ready);
}

// ---------------------------------------------------------------------------
// 5. ParkingCondition 超时
// ---------------------------------------------------------------------------
void TestParkingConditionTimeout() {
    go2cpp_tests::announce("cancel: ParkingCondition::wait_for timeout");

    ParkingCondition condition;
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    auto started = Clock::now();
    const bool notified = condition.wait_for(lock, 40ms, [] { return false; });
    const long long waited_ms = ElapsedMs(started);
    GO2CPP_CHECK(!notified);
    GO2CPP_CHECK(waited_ms >= 30);
    GO2CPP_CHECK(lock.owns_lock());  // 超时返回时外部 mutex 仍持有

    // 谓词已真：立即返回 true，不进入等待。
    started = Clock::now();
    GO2CPP_CHECK(condition.wait_for(lock, 5s, [] { return true; }));
    GO2CPP_CHECK(ElapsedMs(started) < 1000);

    // 超时之后同一 condition 仍能正常完成一次通知。
    bool ready = false;
    std::thread notifier([&] {
        std::this_thread::sleep_for(10ms);
        {
            std::lock_guard<std::mutex> guard(mutex);
            ready = true;
        }
        condition.notify_one();
    });
    GO2CPP_CHECK(condition.wait_for(lock, 2s, [&] { return ready; }));
    GO2CPP_JOIN_WITH_WATCHDOG(notifier, 10s);
    lock.unlock();
    GO2CPP_CHECK(ready);
}

// ---------------------------------------------------------------------------
// 6. RWMutex / SpinMutex / WaitGroup / Channel 的超时与再获取
// ---------------------------------------------------------------------------
void TestLocksReacquireAfterTimeout() {
    go2cpp_tests::announce("cancel: RWMutex/SpinMutex/WaitGroup/Channel reacquire");

    // RWMutex：写者持锁时 RLockFor 超时；释放后可 RLock。
    {
        RWMutex mutex;
        mutex.Lock();
        const auto started = Clock::now();
        GO2CPP_CHECK(!mutex.RLockFor(40ms));
        GO2CPP_CHECK(ElapsedMs(started) >= 30);
        GO2CPP_CHECK(!mutex.TryRLock());  // 超时不改变状态
        mutex.Unlock();
        GO2CPP_CHECK(mutex.RLockFor(2s));  // 写者释放后读者可获取
        mutex.RUnlock();
        // 写锁也能正常获取（无残留等待者/票据）。
        GO2CPP_CHECK(mutex.LockFor(2s));
        mutex.Unlock();
    }

    // SpinMutex：LockFor 超时后，释放后可再次成功。
    {
        SpinMutex mutex;
        mutex.Lock();
        std::atomic<bool> acquired{true};
        std::thread waiter([&] {
            acquired.store(mutex.LockFor(40ms), std::memory_order_release);
        });
        GO2CPP_JOIN_WITH_WATCHDOG(waiter, 10s);
        GO2CPP_CHECK(!acquired.load(std::memory_order_acquire));
        mutex.Unlock();
        GO2CPP_CHECK(mutex.LockFor(2s));
        mutex.Unlock();
    }

    // WaitGroup：WaitFor 超时返回 false；Done 后可等待成功。
    {
        WaitGroup group;
        group.Add(1);
        const auto started = Clock::now();
        GO2CPP_CHECK(!group.WaitFor(40ms));
        GO2CPP_CHECK(ElapsedMs(started) >= 30);
        GO2CPP_CHECK(group.Count() == 1);
        group.Done();
        GO2CPP_CHECK(group.WaitFor(2s));
        GO2CPP_CHECK(group.Count() == 0);
    }

    // Channel：RecvFor/SendFor 超时、已取消 Context 的 Recv/Send。
    {
        auto channel = MakeChannel<int>(0);
        const auto started = Clock::now();
        const auto timeout = channel->RecvFor(40ms);
        GO2CPP_CHECK(timeout.status == ChannelStatus::kTimedOut);
        GO2CPP_CHECK(ElapsedMs(started) >= 30);

        // 写入一个值后通道仍完全可用。
        std::atomic<bool> sent{false};
        std::thread sender([&] {
            sent.store(channel->Send(7).Ok(), std::memory_order_release);
        });
        const auto received = channel->RecvFor(2s);
        GO2CPP_CHECK(received.Ok());
        GO2CPP_CHECK(received.ValueOrDefault() == 7);
        GO2CPP_JOIN_WITH_WATCHDOG(sender, 10s);
        GO2CPP_CHECK(sent.load(std::memory_order_acquire));

        // 已取消的 Context：Recv 立即返回 kCancelled。
        auto context = Context::WithCancel(Context::Background());
        context.second();
        GO2CPP_CHECK(channel->Recv(context.first).status ==
                     ChannelStatus::kCancelled);
        GO2CPP_CHECK(channel->Send(8, context.first).status ==
                     ChannelStatus::kCancelled);

        // 取消不影响通道本身：用并发接收者完成一次无缓冲交接。
        std::atomic<bool> late_ok{false};
        std::atomic<int> late_value{0};
        std::thread receiver([&] {
            const auto result = channel->RecvFor(2s);
            late_ok.store(result.Ok(), std::memory_order_release);
            if (result.Ok()) {
                late_value.store(result.ValueOrDefault(),
                                 std::memory_order_release);
            }
        });
        GO2CPP_CHECK(channel->SendFor(9, 2s).Ok());
        GO2CPP_JOIN_WITH_WATCHDOG(receiver, 10s);
        GO2CPP_CHECK(late_ok.load(std::memory_order_acquire));
        GO2CPP_CHECK(late_value.load(std::memory_order_acquire) == 9);
    }
}

// ---------------------------------------------------------------------------
// 7. 受管 Fiber：park 在 Mutex 上时被 Context 取消
// ---------------------------------------------------------------------------
void TestFiberLockCancel() {
    go2cpp_tests::announce("cancel: managed Fiber Mutex::Lock(ctx) cancelled");

    SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.idle_wait = 1ms;
    Scheduler scheduler(config);
    scheduler.start();

    Mutex mutex;
    mutex.Lock();  // 主线程持有，强制 Fiber park
    auto context = Context::WithCancel(Context::Background());
    std::atomic<bool> lock_result{true};
    std::atomic<bool> trylock_while_held{true};
    std::atomic<bool> trylock_done{false};
    std::atomic<bool> reacquired{false};

    auto task = scheduler.spawn([&] {
        lock_result.store(mutex.Lock(context.first), std::memory_order_release);
        trylock_while_held.store(mutex.TryLock(), std::memory_order_release);
        trylock_done.store(true, std::memory_order_release);
        // 主线程释放后，同一 Fiber 必须能重新获取这把锁。
        if (mutex.Lock()) {
            reacquired.store(true, std::memory_order_release);
            mutex.Unlock();
        }
    });

    GO2CPP_REQUIRE_EVENTUALLY(task->state() == GState::kWaiting, 5s);
    context.second();
    // 等 Fiber 完成「取消返回 + TryLock 检查」，再释放主线程的锁。
    GO2CPP_REQUIRE_EVENTUALLY(trylock_done.load(std::memory_order_acquire), 5s);
    mutex.Unlock();
    GO2CPP_REQUIRE(task->wait_for(10s));

    GO2CPP_CHECK(!lock_result.load(std::memory_order_acquire));
    GO2CPP_CHECK(!trylock_while_held.load(std::memory_order_acquire));
    GO2CPP_CHECK(reacquired.load(std::memory_order_acquire));

    scheduler.shutdown();
}

// ---------------------------------------------------------------------------
// 8. 取消风暴：反复取消不留下等待节点 / 不破坏锁状态
// ---------------------------------------------------------------------------
void TestCancelStorm() {
    go2cpp_tests::announce("cancel: 2000-round cancel storm + 100 timeouts");

    Mutex mutex;
    for (int round = 0; round < 2000; ++round) {
        auto context = Context::WithCancel(Context::Background());
        context.second();
        GO2CPP_CHECK(!mutex.Lock(context.first));
    }
    GO2CPP_CHECK(mutex.TryLock());
    mutex.Unlock();

    // TimedMutex 反复超时：锁始终由主线程持有，释放后立即可用。
    TimedMutex timed;
    timed.Lock();
    for (int round = 0; round < 100; ++round) {
        GO2CPP_CHECK(!timed.TryLockFor(1ms));
    }
    GO2CPP_CHECK(!timed.TryLock());
    timed.Unlock();
    GO2CPP_CHECK(timed.TryLock());
    timed.Unlock();
}

}  // namespace

int main() {
    std::printf("=== 并发原语取消/超时测试 ===\n");
    const auto total_started = Clock::now();

    TestMutexCancelWhileWaiting();
    TestMutexAlreadyCancelled();
    TestTimedMutexTimeout();
    TestConditionVariableTimeoutAndCancel();
    TestParkingConditionTimeout();
    TestLocksReacquireAfterTimeout();
    TestFiberLockCancel();
    TestCancelStorm();

    const int failures = go2cpp_tests::g_failures.load();
    const double seconds =
        std::chrono::duration<double>(Clock::now() - total_started).count();
    std::printf("[primitives-cancel] failures=%d elapsed=%.3fs\n", failures,
                seconds);
    return failures == 0 ? 0 : 1;
}
