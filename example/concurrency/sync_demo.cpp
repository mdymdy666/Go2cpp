#include "go2cpp/runtime.hpp"

#include <iostream>

/**
 * @brief sync 简单版：Fiber/线程混合 Mutex、条件变量、WaitGroup 三件套。
 * @details ① Mutex —— 保护多 Fiber 共享的计数器，计数不丢；
 *          ② WaitGroup —— 等待一组 Fiber 全部结束；
 *          ③ Unlock 不要求同线程 —— 可以在另一个执行流里解锁。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;

    Scheduler scheduler(4);
    scheduler.start();
    bool ok = true;

    // ① Mutex 保护共享状态：8 个 Fiber 各加 1000 次，结果必须是 8000
    std::clog << "[sync][场景1] Mutex 保护共享计数器\n";
    sync::Mutex mutex;
    long long counter = 0;
    constexpr int kFibers = 8;
    constexpr int kPerFiber = 1000;
    {
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
    }
    std::cout << "  计数 = " << counter << "（预期 "
              << kFibers * kPerFiber << "）\n";
    ok = ok && (counter == static_cast<long long>(kFibers) * kPerFiber);

    // ② Unlock 不要求同线程：主执行流加锁，交给另一个 Fiber 解锁
    std::clog << "[sync][场景2] 跨执行流解锁（Go 风格所有权）\n";
    sync::Mutex handoff;
    ok = ok && handoff.Lock();
    auto unlocker = go(scheduler, [&handoff] { handoff.Unlock(); });
    unlocker->wait();
    const bool reacquired = handoff.Lock();   // 能再次拿到，说明确实被解锁了
    if (reacquired) {
        handoff.Unlock();
    }
    std::cout << "  跨执行流解锁后重新加锁成功 = " << reacquired << "（预期 1）\n";
    ok = ok && reacquired;

    // ③ WaitGroup：三个 Fiber 各做一点事，Wait 返回时应当全部结束
    std::clog << "[sync][场景3] WaitGroup 汇合\n";
    sync::WaitGroup group;
    std::atomic<int> finished{0};
    for (int i = 0; i < 3; ++i) {
        group.Add(1);
        (void)go(scheduler, [&finished, &group] {
            finished.fetch_add(1);
            group.Done();
        });
    }
    group.Wait();
    std::cout << "  完成数 = " << finished.load() << "，Count = " << group.Count()
              << "（预期 3 0）\n";
    ok = ok && finished.load() == 3 && group.Count() == 0;

    scheduler.shutdown();
    std::clog << (ok ? "[sync] 全部通过\n" : "[sync] 有失败\n");
    return ok ? 0 : 1;
}
