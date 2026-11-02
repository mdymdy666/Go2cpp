#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/runtime.hpp"

#include <chrono>
#include <iostream>
#include <mutex>

/**
 * @brief ParkingCondition 简单版：等待/通知、唤醒全部、带超时等待。
 * @details 它是"同时服务 Fiber 与普通线程"的条件等待适配器：
 *          受管 G 等待时挂起 Fiber（让出 M），普通线程等待时走原生 condition_variable。
 *          用法与 std::condition_variable 一致 —— 需要外部互斥锁 + 谓词。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;
    using Ms = std::chrono::milliseconds;

    Scheduler scheduler(2);
    scheduler.start();
    bool ok = true;

    // ① 等待 / 通知：一个 Fiber 等，另一个 Fiber 通知
    std::clog << "[parking][场景1] 等待与通知\n";
    core::ParkingCondition cv;
    std::mutex mutex;
    bool ready = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        auto notifier = go(scheduler, [&] {
            std::this_thread::sleep_for(Ms(20));
            {
                std::lock_guard<std::mutex> guard(mutex);
                ready = true;
            }
            cv.notify_one();
        });
        const bool signalled = cv.wait(lock, [&] { return ready; });
        lock.unlock();
        notifier->wait();
        std::cout << "  收到通知=" << signalled << " ready=" << ready << "（预期 1 1）\n";
        ok = ok && signalled && ready;
    }

    // ② 带超时等待：条件永不成立时按时返回 false
    std::clog << "[parking][场景2] 带超时等待\n";
    core::ParkingCondition timeout_cv;
    std::mutex timeout_mutex;
    {
        std::unique_lock<std::mutex> lock(timeout_mutex);
        const bool got = timeout_cv.wait_for(lock, Ms(30), [] { return false; });
        std::cout << "  超时后返回=" << got << "（预期 0）\n";
        ok = ok && !got;
    }

    // ③ 静态查询：普通线程里两者都应为 false
    std::clog << "[parking][场景3] 静态查询（普通线程）\n";
    const bool unsupported = core::ParkingCondition::FiberWaitUnsupported();
    const bool cancelled = core::ParkingCondition::CancellationRequested();
    std::cout << "  FiberWaitUnsupported=" << unsupported
              << " CancellationRequested=" << cancelled << "（预期 0 0）\n";
    ok = ok && !unsupported && !cancelled;

    scheduler.shutdown();
    std::clog << (ok ? "[parking] 全部通过\n" : "[parking] 有失败\n");
    return ok ? 0 : 1;
}
