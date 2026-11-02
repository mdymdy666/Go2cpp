#include "go2cpp/runtime.hpp"

#include <chrono>
#include <deque>
#include <iostream>

// sync 全面版：同一个组件的 6 个使用场景。
//   1  Mutex 互斥            多 Fiber 累加不丢更新
//   2  跨执行流解锁          A 加锁、B 解锁（Go 风格所有权，非线程绑定）
//   3  LockFor 超时          拿不到锁时按时返回 false，而不是一直等
//   4  Mutex + ConditionVariable   生产者/消费者，用谓词循环对抗虚假唤醒
//   5  WaitGroup 汇合        等待一波 Fiber 全部结束，Count 归零
//   6  Context 取消打断 Wait 取消后条件变量等待立即返回
//
// 每个场景独立成段；校验失败只记 ok=false，最后统一报告。

namespace {

using Ms = std::chrono::milliseconds;

bool scenario_mutex_mutual_exclusion(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景1] Mutex：多 Fiber 累加不丢更新\n";
    sync::Mutex mutex;
    long long counter = 0;
    constexpr int kFibers = 8;
    constexpr int kPerFiber = 2000;

    sync::WaitGroup group;
    for (int i = 0; i < kFibers; ++i) {
        group.Add(1);
        (void)go(scheduler, [&mutex, &counter, &group] {
            for (int j = 0; j < kPerFiber; ++j) {
                if (mutex.Lock()) {
                    ++counter;
                    mutex.Unlock();
                }
            }
            group.Done();
        });
    }
    group.Wait();
    const long long expected = static_cast<long long>(kFibers) * kPerFiber;
    std::cout << "  计数 = " << counter << "（预期 " << expected << "）\n";
    return counter == expected;
}

bool scenario_cross_execution_unlock(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景2] 跨执行流解锁：主执行流加锁，另一个 Fiber 解锁\n";
    sync::Mutex mutex;
    if (!mutex.Lock()) {
        return false;
    }
    // 关键：Unlock 不在加锁的那个执行流里调用。
    // 普通 std::mutex 在这里是 UB（要求同线程），sync::Mutex 允许。
    auto unlocker = go(scheduler, [&mutex] { mutex.Unlock(); });
    unlocker->wait();

    const bool reacquired = mutex.Lock();   // 能再拿到即证明已被正确释放
    if (reacquired) {
        mutex.Unlock();
    }
    const bool try_lock_ok = mutex.TryLock();
    if (try_lock_ok) {
        mutex.Unlock();
    }
    std::cout << "  重新加锁成功 = " << reacquired
              << " TryLock 成功 = " << try_lock_ok << "（预期 1 1）\n";
    return reacquired && try_lock_ok;
}

bool scenario_lock_for_timeout(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景3] LockFor：拿不到锁时按时返回 false\n";
    sync::Mutex mutex;
    if (!mutex.TryLock()) {
        return false;
    }
    // 锁已被主执行流持有，另一个 Fiber 用 LockFor 等 30ms 应当超时。
    bool got = true;
    auto waiter = go(scheduler, [&mutex, &got] {
        got = mutex.LockFor(Ms(30));        // 预期超时 → false
        if (got) {
            mutex.Unlock();
        }
    });
    waiter->wait();
    mutex.Unlock();                          // 主执行流释放
    std::cout << "  超时后仍拿到锁 = " << got << "（预期 0）\n";
    return !got;
}

bool scenario_condition_variable(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景4] Mutex + ConditionVariable：生产者/消费者\n";
    sync::Mutex mutex;
    sync::ConditionVariable cv;
    std::deque<int> queue;
    constexpr int kItems = 5;
    int consumed = 0;

    auto consumer = go(scheduler, [&] {
        for (;;) {
            mutex.Lock();
            // 谓词循环：条件变量可能被虚假唤醒，必须重查条件
            while (queue.empty() && consumed < kItems) {
                if (!cv.Wait(mutex)) {       // 释放锁并等待，返回时重新持有
                    mutex.Unlock();
                    return;
                }
            }
            if (queue.empty()) {             // 已消费完 kItems 项
                mutex.Unlock();
                return;
            }
            queue.pop_front();
            ++consumed;
            mutex.Unlock();
        }
    });

    for (int i = 0; i < kItems; ++i) {
        mutex.Lock();
        queue.push_back(i);
        mutex.Unlock();
        cv.NotifyOne();                      // 通知一个等待者
    }
    consumer->wait();
    std::cout << "  消费 " << consumed << " 项（预期 " << kItems << "）\n";
    return consumed == kItems;
}

bool scenario_wait_group() {
    using namespace go2cpp;
    std::clog << "[full][场景5] WaitGroup：汇合一波 Fiber\n";
    Scheduler scheduler(3);
    scheduler.start();
    sync::WaitGroup group;
    constexpr int kTasks = 6;
    std::atomic<int> finished{0};
    for (int i = 0; i < kTasks; ++i) {
        group.Add(1);
        (void)go(scheduler, [&finished, &group] {
            finished.fetch_add(1);
            group.Done();
        });
    }
    const bool waited = group.WaitFor(Ms(2000));
    const std::size_t remain = group.Count();
    scheduler.shutdown();
    std::cout << "  WaitFor 成功 = " << waited << " 完成 " << finished.load()
              << " 剩余计数 = " << remain << "（预期 1 " << kTasks << " 0）\n";
    return waited && finished.load() == kTasks && remain == 0;
}

bool scenario_cancel_unblocks_wait(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景6] Context 取消：条件变量等待立即返回\n";
    sync::Mutex mutex;
    sync::ConditionVariable cv;
    auto pair = WithCancel(Background());
    const auto ctx = pair.first;
    bool wait_returned = false;

    auto waiter = go(scheduler, [&] {
        mutex.Lock();
        (void)cv.Wait(mutex, ctx);          // 取消后不再永久阻塞
        wait_returned = true;
        mutex.Unlock();
    });

    std::this_thread::sleep_for(Ms(30));
    pair.second();                          // 取消 → 唤醒等待者
    waiter->wait();
    const bool cancelled = Is(ctx->Err(), CanceledError());
    std::cout << "  等待已返回 = " << wait_returned
              << " 上下文已取消 = " << cancelled << "（预期 1 1）\n";
    return wait_returned && cancelled;
}

}  // namespace

/**
 * @brief sync 全面版：6 个场景，覆盖互斥、跨执行流解锁、超时、条件变量与取消。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;

    Scheduler scheduler(4);
    scheduler.start();

    bool ok = true;
    ok = scenario_mutex_mutual_exclusion(scheduler) && ok;
    ok = scenario_cross_execution_unlock(scheduler) && ok;
    ok = scenario_lock_for_timeout(scheduler) && ok;
    ok = scenario_condition_variable(scheduler) && ok;
    ok = scenario_wait_group() && ok;
    ok = scenario_cancel_unblocks_wait(scheduler) && ok;

    scheduler.shutdown();
    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
