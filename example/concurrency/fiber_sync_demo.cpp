#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <iostream>

/**
 * @brief 演示 Fiber 与调度器内的 Mutex、ConditionVariable、WaitGroup 协作。
 * @details 前半段使用一个 P 驱动多个 Fiber 完成生产者/消费者同步，后半段
 *          手动恢复 Fiber，说明 Fiber 也可以独立使用 Suspend/Resume。
 * @return 同步计数和 Fiber 恢复结果符合预期时返回 0。
 */
int main() {
    using namespace std::chrono_literals;
    std::clog << "[fiber-sync][场景1] Fiber 生产者/消费者共享 Mutex、ConditionVariable、WaitGroup\n";
    go2cpp::Scheduler scheduler(1);
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable condition;
    go2cpp::sync::WaitGroup group;
    bool ready = false;
    int counter = 0;
    std::atomic<bool> success{true};
    group.Add(2);
    scheduler.Start();
    auto consumer = scheduler.Go([&] {
        if (!mutex.Lock()) {
            success.store(false);
            group.Done();
            return;
        }
        const auto wait_deadline = std::chrono::steady_clock::now() + 2s;
        while (!ready &&
               std::chrono::steady_clock::now() < wait_deadline) {
            if (!condition.WaitFor(mutex, 1s)) {
                break;
            }
        }
        if (!ready) {
            success.store(false);
        }
        counter += ready ? 1 : 0;
        mutex.Unlock();
        group.Done();
    });
    auto producer = scheduler.Go([&] {
        if (mutex.Lock()) {
            ready = true;
            ++counter;
            mutex.Unlock();
            condition.NotifyAll();
        } else {
            success.store(false);
        }
        group.Done();
    });
    auto coordinator = scheduler.Go([&] {
        if (!group.WaitFor(2s)) {
            success.store(false);
        }
    });
    const bool joined = consumer->wait_for(3s) && producer->wait_for(3s) &&
                        coordinator->wait_for(3s);

    // 场景二：没有任何通知时，ConditionVariable 必须按超时返回，不能把 P
    // 或底层 M 永久占住。WaitFor 返回后 mutex 仍归当前 Fiber 持有。
    std::clog << "[fiber-sync][场景2] ConditionVariable 超时路径\n";
    go2cpp::sync::Mutex timeout_mutex;
    go2cpp::sync::ConditionVariable timeout_condition;
    std::atomic<bool> timeout_observed{false};
    auto timeout_task = scheduler.Go([&] {
        if (!timeout_mutex.Lock()) {
            success.store(false);
            return;
        }
        const bool notified = timeout_condition.WaitFor(timeout_mutex, 30ms);
        timeout_observed.store(!notified, std::memory_order_release);
        timeout_mutex.Unlock();
    });
    const bool timeout_joined = timeout_task->wait_for(2s);
    std::clog << "[fiber-sync][场景2] joined=" << timeout_joined
              << " timed_out=" << timeout_observed.load() << '\n';

    scheduler.Shutdown();

    std::clog << "[fiber-sync][场景3] 手动 Fiber Suspend/Resume 保留局部变量\n";
    int continuation = 0;
    go2cpp::Fiber fiber([&] {
        int local = 40;
        go2cpp::Fiber::Suspend();
        continuation = local + 2;
    });
    const bool resumed = fiber.resume() && fiber.resume();
    std::cout << "P=1 mutex/condition/waitgroup counter=" << counter
              << " fiber-local=" << continuation << '\n';
    return joined && timeout_joined && timeout_observed.load() &&
                   success.load() && counter == 2 && resumed &&
                   continuation == 42 ? 0 : 1;
}
