#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <iostream>

int main() {
    using namespace std::chrono_literals;
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
        while (!ready) {
            if (!condition.WaitFor(mutex, 1s)) {
                success.store(false);
                break;
            }
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
    scheduler.Shutdown();

    int continuation = 0;
    go2cpp::Fiber fiber([&] {
        int local = 40;
        go2cpp::Fiber::Suspend();
        continuation = local + 2;
    });
    const bool resumed = fiber.resume() && fiber.resume();
    std::cout << "P=1 mutex/condition/waitgroup counter=" << counter
              << " fiber-local=" << continuation << '\n';
    return joined && success.load() && counter == 2 && resumed &&
                   continuation == 42 ? 0 : 1;
}
