#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/runtime.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// ParkingCondition 全面版：同一个组件的 6 个使用场景。
//   1  ★ 受管 G 等待不阻塞 M    P=1 下「一个 Fiber 等、另一个 Fiber 通知」能跑通
//   2  谓词式等待              条件未成立时反复等待，天然对抗虚假唤醒
//   3  notify_all              一次唤醒全部等待者
//   4  wait_for 超时           条件永不成立时按时返回 false
//   5  普通线程等待路径        同一个对象也能被普通线程使用（走原生 CV）
//   6  静态查询                FiberWaitUnsupported / CancellationRequested
//
// ★ 场景 1 是这个组件存在的理由：如果受管 G 的等待会阻塞承载它的 M，
//   那么在 P=1 时通知者将永远排不上，本场景会直接超时失败。

namespace {

using Ms = std::chrono::milliseconds;

bool scenario_managed_fiber_does_not_block_m() {
    using namespace go2cpp;
    std::clog << "[full][场景1] 受管 G 等待不阻塞 M（P=1 反证）\n";
    // 只给一个 P：若等待阻塞了 M，通知者就永远没机会运行。
    Scheduler scheduler(1);
    scheduler.start();

    core::ParkingCondition cv;
    std::mutex mutex;
    bool ready = false;
    std::atomic<bool> waiter_done{false};

    auto waiter = go(scheduler, [&] {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return ready; });     // 受管 G 在此挂起并让出 M
        waiter_done.store(true);
    });
    auto notifier = go(scheduler, [&] {
        std::this_thread::sleep_for(Ms(20));
        {
            std::lock_guard<std::mutex> guard(mutex);
            ready = true;
        }
        cv.notify_one();
    });

    // ★ 必须先等 waiter 结束再读标志：go() 是异步的，
    //   若在这里直接读，必然还是 false（本示例首版就犯了这个错）。
    //   真正的反证在于：若受管 G 的等待会阻塞唯一的 M，
    //   那么 waiter->wait() 本身就会挂住 —— 由外部 timeout 兜底。
    waiter->wait();
    const bool finished = waiter_done.load();
    notifier->wait();
    scheduler.shutdown();
    std::cout << "  等待者已完成=" << finished
              << "（预期 1；若为 0 说明等待阻塞了唯一的 M）\n";
    return finished;
}

bool scenario_predicate_loop() {
    using namespace go2cpp;
    std::clog << "[full][场景2] 谓词式等待：条件不成立就继续等\n";
    Scheduler scheduler(2);
    scheduler.start();
    core::ParkingCondition cv;
    std::mutex mutex;
    int counter = 0;

    auto producer = go(scheduler, [&] {
        for (int i = 1; i <= 3; ++i) {
            std::this_thread::sleep_for(Ms(5));
            {
                std::lock_guard<std::mutex> guard(mutex);
                counter = i;
            }
            cv.notify_one();
        }
    });

    std::unique_lock<std::mutex> lock(mutex);
    // 谓词要求 counter 达到 3；中间的 1、2 会被谓词挡回去继续等。
    const bool reached = cv.wait(lock, [&] { return counter >= 3; });
    lock.unlock();
    producer->wait();
    scheduler.shutdown();
    std::cout << "  谓词最终成立=" << reached << " counter=" << counter << "（预期 1 3）\n";
    return reached && counter == 3;
}

bool scenario_notify_all() {
    using namespace go2cpp;
    std::clog << "[full][场景3] notify_all 唤醒全部等待者\n";
    Scheduler scheduler(5);
    scheduler.start();
    core::ParkingCondition cv;
    std::mutex mutex;
    bool open = false;
    std::atomic<int> woken{0};
    constexpr int kWatchers = 4;

    std::vector<std::shared_ptr<Task>> watchers;
    for (int i = 0; i < kWatchers; ++i) {
        watchers.push_back(go(scheduler, [&] {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return open; });
            woken.fetch_add(1);
        }));
    }
    auto opener = go(scheduler, [&] {
        std::this_thread::sleep_for(Ms(30));
        {
            std::lock_guard<std::mutex> guard(mutex);
            open = true;
        }
        cv.notify_all();
    });

    for (auto& w : watchers) {
        w->wait();
    }
    opener->wait();
    scheduler.shutdown();
    std::cout << "  被唤醒=" << woken.load() << "（预期 " << kWatchers << "）\n";
    return woken.load() == kWatchers;
}

bool scenario_wait_for_timeout() {
    using namespace go2cpp;
    std::clog << "[full][场景4] wait_for 超时：条件不成立时按时返回\n";
    core::ParkingCondition cv;
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);

    const auto begin = std::chrono::steady_clock::now();
    const bool got = cv.wait_for(lock, Ms(30), [] { return false; });
    const auto spent = std::chrono::duration_cast<Ms>(
        std::chrono::steady_clock::now() - begin).count();
    std::cout << "  返回=" << got << " 耗时约 " << spent << "ms（预期 0，且 >= 25ms）\n";
    return !got && spent >= 25;
}

bool scenario_native_thread_wait() {
    using namespace go2cpp;
    std::clog << "[full][场景5] 普通线程等待路径（同一个对象）\n";
    core::ParkingCondition cv;
    std::mutex mutex;
    bool ready = false;

    // 普通线程等待（走原生 condition_variable），另一个线程通知
    std::thread notifier([&] {
        std::this_thread::sleep_for(Ms(20));
        {
            std::lock_guard<std::mutex> guard(mutex);
            ready = true;
        }
        cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(mutex);
    const bool signalled = cv.wait(lock, [&] { return ready; });
    lock.unlock();
    notifier.join();
    std::cout << "  普通线程收到通知=" << signalled << "（预期 1）\n";
    return signalled;
}

bool scenario_static_queries() {
    using namespace go2cpp;
    std::clog << "[full][场景6] 静态查询：普通线程下两者均为 false\n";
    const bool unsupported = core::ParkingCondition::FiberWaitUnsupported();
    const bool cancelled = core::ParkingCondition::CancellationRequested();
    // 在受管 Fiber 里，FiberWaitUnsupported 仍应为 false（有 Scheduler 后端）
    Scheduler scheduler(1);
    scheduler.start();
    std::atomic<bool> in_fiber_unsupported{true};
    auto task = go(scheduler, [&in_fiber_unsupported] {
        in_fiber_unsupported.store(core::ParkingCondition::FiberWaitUnsupported());
    });
    task->wait();
    scheduler.shutdown();

    std::cout << "  普通线程 FiberWaitUnsupported=" << unsupported
              << " 受管 Fiber 内=" << in_fiber_unsupported.load()
              << " CancellationRequested=" << cancelled << "（预期 0 0 0）\n";
    return !unsupported && !in_fiber_unsupported.load() && !cancelled;
}

}  // namespace

/**
 * @brief ParkingCondition 全面版：6 个场景，覆盖 Fiber/线程双路径与超时、广播。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_managed_fiber_does_not_block_m() && ok;
    ok = scenario_predicate_loop() && ok;
    ok = scenario_notify_all() && ok;
    ok = scenario_wait_for_timeout() && ok;
    ok = scenario_native_thread_wait() && ok;
    ok = scenario_static_queries() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}