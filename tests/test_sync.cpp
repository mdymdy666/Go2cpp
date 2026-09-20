#include "go2cpp/sync.hpp"

#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
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

go2cpp::SchedulerConfig SchedulerConfig(std::size_t processors) {
    go2cpp::SchedulerConfig config;
    config.processor_count = processors;
    config.max_workers = processors;
    config.idle_wait = 1ms;
    return config;
}

void TestUnmanagedBoundaryAndCounterErrors() {
    go2cpp::sync::Mutex mutex;
    GO2CPP_CHECK(mutex.LockFor(0ms));
    mutex.Unlock();
    GO2CPP_CHECK(mutex.Lock());
    GO2CPP_CHECK(!mutex.Lock());
    GO2CPP_CHECK(!mutex.TryLock());

    bool lock_threw = false;
    try {
        mutex.lock();
    } catch (const std::logic_error&) {
        lock_threw = true;
    }
    GO2CPP_CHECK(lock_threw);
    mutex.Unlock();
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
        wait_finished.store(group.Wait(), std::memory_order_release);
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
        second_wave.store(group.Wait(), std::memory_order_release);
    });
    GO2CPP_CHECK(WaitUntil([&] {
        return scheduler.runnable_count() == 0;
    }));
    group.Done();
    GO2CPP_CHECK(WaitUntil(
        [&] { return second_wave.load(std::memory_order_acquire); }));

    scheduler.shutdown();
}

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

    scheduler.shutdown();
}

struct NotifyRaceState {
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable condition;
    std::atomic<bool> waiter_entered{false};
    std::atomic<int> completed{0};
    std::atomic<bool> wait_result{false};
};

void TestNotifyBeforeParkRace() {
    go2cpp::Scheduler scheduler(SchedulerConfig(2));
    scheduler.start();

    // The notifier first waits for the same mutex. Wait publishes its CV node
    // and then Unlock hands the mutex to the notifier on the other M, making
    // NotifyOne race directly with the waiters subsequent park operation.
    for (int iteration = 0; iteration < 64; ++iteration) {
        auto state = std::make_shared<NotifyRaceState>();
        scheduler.spawn([state] {
            GO2CPP_CHECK(state->mutex.Lock());
            state->waiter_entered.store(true, std::memory_order_release);
            state->wait_result.store(
                state->condition.WaitFor(state->mutex, 500ms),
                std::memory_order_release);
            state->mutex.Unlock();
            state->completed.fetch_add(1, std::memory_order_release);
        });
        scheduler.spawn([state] {
            while (!state->waiter_entered.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            GO2CPP_CHECK(state->mutex.Lock());
            state->condition.NotifyOne();
            state->mutex.Unlock();
            state->completed.fetch_add(1, std::memory_order_release);
        });
        GO2CPP_CHECK(WaitUntil([&] {
            return state->completed.load(std::memory_order_acquire) == 2;
        }));
        GO2CPP_CHECK(state->wait_result.load(std::memory_order_acquire));
    }

    scheduler.shutdown();
}

void TestSpuriousWakeDoesNotCancelWait() {
    go2cpp::Scheduler scheduler(SchedulerConfig(1));
    go2cpp::sync::WaitGroup group;
    group.Add(1);
    std::atomic<bool> result{false};
    scheduler.spawn([&] {
        // An earlier, unrelated wake permit must not cancel this new wait.
        (void)scheduler.wake(go2cpp::Scheduler::current_task());
        result.store(group.Wait(), std::memory_order_release);
    });
    scheduler.spawn([&] { group.Done(); });
    scheduler.start();
    GO2CPP_CHECK(WaitUntil(
        [&] { return result.load(std::memory_order_acquire); }));
    scheduler.shutdown();
}

void TestWaitGroupWaveReleasedBeforeReuse() {
    go2cpp::Scheduler scheduler(SchedulerConfig(1));
    go2cpp::sync::WaitGroup group;
    group.Add(1);
    scheduler.start();
    std::atomic<bool> first_result{false};
    auto first = scheduler.spawn([&] {
        first_result.store(group.Wait(), std::memory_order_release);
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
            std::this_thread::yield();
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
        second_result.store(group.Wait(), std::memory_order_release);
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return second->state() == go2cpp::GState::kWaiting; }));
    GO2CPP_CHECK(!second_result.load(std::memory_order_acquire));
    group.Done();
    GO2CPP_CHECK(second->wait_for(2s));
    GO2CPP_CHECK(second_result.load(std::memory_order_acquire));
    scheduler.shutdown();
}

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
            while (!race.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            cancellation.second();
        });
        std::thread unlocker([&] {
            while (!race.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            mutex.Unlock();
        });
        race.store(true, std::memory_order_release);
        canceller.join();
        unlocker.join();
        GO2CPP_CHECK(waiter->wait_for(2s));
        GO2CPP_CHECK(returned.load(std::memory_order_acquire));
        const bool acquired = mutex.TryLock();
        GO2CPP_CHECK(acquired);
        if (acquired) {
            mutex.Unlock();
        }
    }
    scheduler.shutdown();
}

struct StackRelease final {
    explicit StackRelease(std::atomic<int>& released) : m_released(released) {}
    ~StackRelease() { m_released.fetch_add(1, std::memory_order_release); }
    std::atomic<int>& m_released;
};

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
            GO2CPP_CHECK(!group.Wait(cancellation.first));
            returned.store(true, std::memory_order_release);
        });
        GO2CPP_CHECK(WaitUntil(
            [&] { return waiter->state() == go2cpp::GState::kWaiting; }));
        std::atomic<bool> race{false};
        std::thread canceller([&] {
            while (!race.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            cancellation.second();
        });
        race.store(true, std::memory_order_release);
        scheduler->shutdown();
        scheduler.reset();
        canceller.join();
        GO2CPP_CHECK(returned.load(std::memory_order_acquire));
        GO2CPP_CHECK(released.load(std::memory_order_acquire) == 1);
        GO2CPP_CHECK(waiter->wait_for(100ms));
        // The queue must no longer retain the finished waiter's scheduler.
        group.Done();
    }
}

}  // namespace

void run_sync_tests() {
    go2cpp_tests::announce("scheduler-aware synchronization");
    TestUnmanagedBoundaryAndCounterErrors();
    TestFifoAndSingleProcessorProgress();
    TestContextAndTimeout();
    TestNotifyBeforeParkRace();
    TestSpuriousWakeDoesNotCancelWait();
    TestWaitGroupWaveReleasedBeforeReuse();
    TestUnlockRacesContextCancellation();
    TestContextCallbackRacesSchedulerShutdown();
}

#ifdef GO2CPP_SYNC_TEST_MAIN
#include <iostream>

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
