/**
 * @file test_debug_sync.cpp
 * @brief 锁观测（DebugMutex / DebugConditionVariable）的测试。
 *
 * 覆盖（按项目规范：压测 + 随机 + >=5 个真实场景）：
 *   场景 1 未持有时快照：held=false、waiters=0；TryLock 成功且快照变为 held
 *   场景 2 持有时快照：held=true（且与 TryLock 结果互补）
 *   场景 3 有等待者：主线程持锁 + 工作线程竞争 -> waiters/native_waiters 增长
 *   场景 4 ConditionVariable 等待者快照
 *   场景 5 空指针代理的安全行为（返回默认快照，不崩）
 *   压测   20000 次 Snapshot
 *   随机   随机持锁/释放循环下快照字段始终自洽（held 与 TryLock 互补、fiber+native==waiters）
 */

#include "go2cpp/debug.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

namespace {

using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;

bool SelfConsistent(const go2cpp::sync::MutexDebugSnapshot& s) {
    return s.fiber_waiters + s.native_waiters == s.waiters;
}

}  // namespace

void run_debug_sync_tests() {
    go2cpp_tests::announce("debug mutex and condition variable snapshots");
    using namespace go2cpp::debug;

    // 场景 1：未持有 -> 持有
    {
        Mutex mutex;
        DebugMutex probe(mutex);
        const auto idle = probe.Snapshot();
        GO2CPP_CHECK(!idle.held);
        GO2CPP_CHECK(idle.waiters == 0);
        GO2CPP_CHECK(SelfConsistent(idle));

        GO2CPP_CHECK(mutex.TryLock());
        const auto locked = probe.Snapshot();
        GO2CPP_CHECK(locked.held);
        GO2CPP_CHECK(!mutex.TryLock());   // 已持有 -> 再试必失败
        GO2CPP_CHECK(SelfConsistent(locked));
        mutex.Unlock();
        GO2CPP_CHECK(!probe.Snapshot().held);
    }

    // 场景 2：held 与 TryLock 必须互补（同一观测点）
    {
        Mutex mutex;
        DebugMutex probe(mutex);
        for (int i = 0; i < 200; ++i) {
            GO2CPP_CHECK(!probe.Snapshot().held);
            GO2CPP_CHECK(mutex.TryLock());
            GO2CPP_CHECK(probe.Snapshot().held);
            mutex.Unlock();
        }
    }

    // 场景 3：有等待者（主线程持锁，工作线程竞争）
    {
        Mutex mutex;
        DebugMutex probe(mutex);
        GO2CPP_CHECK(mutex.TryLock());          // 主线程持有
        std::atomic<int> acquired{0};
        std::atomic<bool> started{false};
        std::thread worker([&] {
            started.store(true, std::memory_order_release);
            if (mutex.Lock()) {                 // 普通线程走原生等待
                acquired.fetch_add(1, std::memory_order_relaxed);
                mutex.Unlock();
            }
        });
        GO2CPP_REQUIRE_EVENTUALLY(started.load(std::memory_order_acquire),
                              std::chrono::seconds(2));
        // 等到快照里确实出现等待者（最多等 2 秒）
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool saw_waiter = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto snapshot = probe.Snapshot();
            if (snapshot.waiters > 0) {
                saw_waiter = true;
                GO2CPP_CHECK(SelfConsistent(snapshot));
                GO2CPP_CHECK(snapshot.has_waiters);
                GO2CPP_CHECK(snapshot.native_waiters >= 1);
                break;
            }
            std::this_thread::yield();
        }
        GO2CPP_CHECK(saw_waiter);
        mutex.Unlock();
        worker.join();
        GO2CPP_CHECK(acquired.load(std::memory_order_relaxed) == 1);
        GO2CPP_REQUIRE_EVENTUALLY(probe.Snapshot().waiters == 0,
                              std::chrono::seconds(2));
    }

    // 场景 4：ConditionVariable 等待者快照
    {
        Mutex mutex;
        ConditionVariable cond;
        DebugConditionVariable cond_probe(cond);
        GO2CPP_CHECK(cond_probe.Snapshot().waiters == 0);
        std::atomic<bool> notified{false};
        std::thread waiter([&] {
            if (mutex.Lock()) {
                cond.Wait(mutex);
                notified.store(true, std::memory_order_release);
                mutex.Unlock();
            }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool saw = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (cond_probe.Snapshot().waiters > 0) {
                saw = true;
                break;
            }
            std::this_thread::yield();
        }
        GO2CPP_CHECK(saw);
        cond.NotifyOne();
        waiter.join();
        GO2CPP_CHECK(notified.load(std::memory_order_acquire));
    }

    // 场景 5：空指针代理安全
    {
        DebugMutex null_probe(static_cast<Mutex*>(nullptr));
        const auto snapshot = null_probe.Snapshot();
        GO2CPP_CHECK(!snapshot.held && snapshot.waiters == 0);
        GO2CPP_CHECK(!null_probe.Held() && null_probe.Waiters() == 0);
        DebugConditionVariable null_cond(static_cast<ConditionVariable*>(nullptr));
        GO2CPP_CHECK(null_cond.Waiters() == 0);
    }

    // 压测：20000 次快照
    {
        Mutex mutex;
        DebugMutex probe(mutex);
        constexpr int kCount = 20000;
        const auto t0 = std::chrono::steady_clock::now();
        std::size_t sink = 0;
        for (int i = 0; i < kCount; ++i) {
            const auto snapshot = probe.Snapshot();
            sink += snapshot.waiters + (snapshot.held ? 1U : 0U);
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::clog << "  [压测] DebugMutex::Snapshot x" << kCount << ": "
                  << std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
                  << " us (sink=" << sink << ")" << std::endl;
        GO2CPP_CHECK(sink == 0);
    }

    // 随机：随机持锁/释放循环下快照自洽
    {
        Mutex mutex;
        DebugMutex probe(mutex);
        std::mt19937 rng(20261016);
        std::uniform_int_distribution<int> action_dist(0, 2);
        bool ok = true;
        for (int round = 0; round < 3000; ++round) {
            const int action = action_dist(rng);
            if (action == 0 && mutex.TryLock()) {
                const auto snapshot = probe.Snapshot();
                ok = ok && snapshot.held && SelfConsistent(snapshot);
                mutex.Unlock();
            } else if (action == 1) {
                const bool held = probe.Snapshot().held;
                const bool try_ok = mutex.TryLock();
                ok = ok && (held != try_ok);   // 互补关系
                if (try_ok) {
                    mutex.Unlock();
                }
            } else {
                ok = ok && SelfConsistent(probe.Snapshot());
            }
        }
        GO2CPP_CHECK(ok);
    }

    // WaitGroup 观测（第 10 轮补充）
    {
        using go2cpp::sync::WaitGroup;
        WaitGroup group;
        DebugWaitGroup probe(group);
        auto snapshot = probe.Snapshot();
        GO2CPP_CHECK(snapshot.count == 0);
        GO2CPP_CHECK(snapshot.waiters == 0);
        GO2CPP_CHECK(probe.Count() == 0);

        group.Add(3);
        GO2CPP_CHECK(probe.Snapshot().count == 3);
        GO2CPP_CHECK(probe.Snapshot().count == static_cast<std::int64_t>(group.Count()));

        // 一个线程进入 Wait：waiters 应变为 1
        std::atomic<bool> woken{false};
        std::thread waiter([&] {
            if (group.Wait()) {
                woken.store(true, std::memory_order_release);
            }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool saw_waiter = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (probe.Snapshot().waiters > 0) {
                saw_waiter = true;
                break;
            }
            std::this_thread::yield();
        }
        GO2CPP_CHECK(saw_waiter);

        group.Done();
        group.Done();
        GO2CPP_CHECK(probe.Snapshot().count == 1);
        group.Done();   // 归零 -> 唤醒
        waiter.join();
        GO2CPP_CHECK(woken.load(std::memory_order_acquire));
        GO2CPP_REQUIRE_EVENTUALLY(probe.Snapshot().waiters == 0, std::chrono::seconds(2));
        GO2CPP_CHECK(probe.Snapshot().count == 0);

        // 空指针代理安全
        DebugWaitGroup null_probe(static_cast<WaitGroup*>(nullptr));
        GO2CPP_CHECK(null_probe.Count() == 0 && null_probe.Waiters() == 0);
    }
}
