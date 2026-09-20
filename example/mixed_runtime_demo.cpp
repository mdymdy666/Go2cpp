#include "go2cpp/fiber_local.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"
#include "go2cpp/thread_policy.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace {

template <typename Rep, typename Period>
void join_with_watchdog(std::thread& thread,
                        std::chrono::duration<Rep, Period> timeout,
                        const char* label) {
    if (!thread.joinable()) {
        return;
    }
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        bool joined{false};
    };
    const auto state = std::make_shared<State>();
    std::thread watchdog([state, timeout, label] {
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->condition.wait_for(lock, timeout,
                                      [&] { return state->joined; })) {
            std::fprintf(stderr, "example watchdog timed out: %s\n", label);
            std::fflush(stderr);
            std::abort();
        }
    });
    thread.join();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->joined = true;
    }
    state->condition.notify_one();
    watchdog.join();
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 3;
    config.idle_worker_timeout = 40ms;
    config.allow_worker_oversubscription = true;

    go2cpp::Scheduler scheduler(config);
    go2cpp::sync::Mutex mutex;
    go2cpp::sync::ConditionVariable condition;
    go2cpp::sync::Mutex condition_mutex;
    go2cpp::sync::WaitGroup group;
    go2cpp::FiberLocal<int> fiber_value;

    std::atomic<bool> ok{true};
    std::atomic<bool> stop{false};
    std::atomic<bool> native_locked{false};
    std::atomic<bool> fiber_waiting{false};
    std::atomic<bool> fiber_has_lock{false};
    std::atomic<bool> native_reacquired{false};
    std::atomic<bool> condition_waiting{false};
    std::atomic<bool> condition_ready{false};
    std::atomic<bool> blocking_entered{false};
    std::atomic<bool> release_blocking{false};
    std::atomic<bool> external_policy_seen{false};
    std::atomic<std::size_t> peak_workers{0};

    const auto wait_flag = [](const std::atomic<bool>& flag,
                              std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!flag.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return flag.load(std::memory_order_acquire);
    };

    scheduler.Start();
    group.Add(3);

    // This native thread owns the mutex first, then waits for the Fiber to
    // acquire and release it. The same FIFO queue is used in both directions.
    std::thread native_owner([&] {
        go2cpp::ScopedThreadParticipation participation(
            scheduler, go2cpp::ThreadParticipationMode::kGmpEligible);
        go2cpp::ScopedThreadHookMode hook_mode(
            go2cpp::ThreadHookMode::kDisabled);
        const auto policy = go2cpp::CurrentThreadPolicy();
        external_policy_seen.store(
            policy.participates_in_gmp() && !policy.runtime_worker &&
                policy.hook == go2cpp::ThreadHookMode::kDisabled,
            std::memory_order_release);

        if (!mutex.LockFor(2s)) {
            ok.store(false, std::memory_order_release);
            return;
        }
        native_locked.store(true, std::memory_order_release);
        if (!wait_flag(fiber_waiting, 2s) || stop.load(std::memory_order_acquire)) {
            stop.store(true, std::memory_order_release);
            mutex.Unlock();
            return;
        }
        mutex.Unlock();

        if (!wait_flag(fiber_has_lock, 2s) || stop.load(std::memory_order_acquire)) {
            stop.store(true, std::memory_order_release);
            return;
        }
        if (!mutex.LockFor(2s)) {
            ok.store(false, std::memory_order_release);
            return;
        }
        native_reacquired.store(true, std::memory_order_release);
        mutex.Unlock();
    });

    if (!wait_flag(native_locked, 2s)) {
        stop.store(true, std::memory_order_release);
        join_with_watchdog(native_owner, 3s, "native owner");
        scheduler.Shutdown();
        return 1;
    }

    // The first Fiber waits on a mutex held by the native thread, then keeps
    // it long enough for the native thread to wait on the Fiber-owned lock.
    auto mutex_fiber = scheduler.Go([&] {
        fiber_waiting.store(true, std::memory_order_release);
        int& value = fiber_value.GetOrCreate(10);
        if (!mutex.LockFor(2s)) {
            ok.store(false, std::memory_order_release);
            stop.store(true, std::memory_order_release);
            group.Done();
            return;
        }
        fiber_has_lock.store(true, std::memory_order_release);
        value += 2;
        if (!scheduler.yield_current()) {
            ok.store(false, std::memory_order_release);
        }
        if (fiber_value.GetOrCreate() != 12) {
            ok.store(false, std::memory_order_release);
        }
        mutex.Unlock();
        group.Done();
    });

    // A native condition-variable notifier wakes a managed Fiber waiter.
    std::thread native_notifier([&] {
        if (!wait_flag(condition_waiting, 2s) || stop.load(std::memory_order_acquire)) {
            stop.store(true, std::memory_order_release);
            return;
        }
        if (!condition_mutex.LockFor(2s)) {
            ok.store(false, std::memory_order_release);
            return;
        }
        condition_ready.store(true, std::memory_order_release);
        condition_mutex.Unlock();
        condition.NotifyAll();
    });

    auto condition_fiber = scheduler.Go([&] {
        if (!condition_mutex.LockFor(2s)) {
            ok.store(false, std::memory_order_release);
            stop.store(true, std::memory_order_release);
            group.Done();
            return;
        }
        condition_waiting.store(true, std::memory_order_release);
        const auto condition_deadline =
            std::chrono::steady_clock::now() + 3s;
        while (!condition_ready.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < condition_deadline) {
            if (!condition.WaitFor(condition_mutex, 2s)) {
                break;
            }
        }
        if (!condition_ready.load(std::memory_order_acquire)) {
            ok.store(false, std::memory_order_release);
            stop.store(true, std::memory_order_release);
            condition_mutex.Unlock();
            group.Done();
            return;
        }
        condition_mutex.Unlock();
        group.Done();
    });

    // A declared blocking region allows a replacement M to run queued Gs
    // while this M is inside a native blocking call.
    auto blocking_fiber = scheduler.Go([&] {
        go2cpp::BlockingRegion blocking;
        if (!blocking.active()) {
            ok.store(false, std::memory_order_release);
            stop.store(true, std::memory_order_release);
        }
        blocking_entered.store(true, std::memory_order_release);
        int& value = fiber_value.GetOrCreate(20);
        while (!release_blocking.load(std::memory_order_acquire) &&
               !stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        value += 1;
        group.Done();
    });

    if (!wait_flag(blocking_entered, 2s)) {
        stop.store(true, std::memory_order_release);
        release_blocking.store(true, std::memory_order_release);
        join_with_watchdog(native_owner, 3s, "native owner");
        join_with_watchdog(native_notifier, 3s, "native notifier");
        scheduler.Shutdown();
        return 1;
    }

    const auto sample_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < sample_deadline &&
           (!mutex_fiber->wait_for(1ms) || !condition_fiber->wait_for(1ms))) {
        peak_workers.store(
            std::max(peak_workers.load(std::memory_order_relaxed),
                     scheduler.WorkerCount()),
            std::memory_order_relaxed);
    }
    release_blocking.store(true, std::memory_order_release);

    const bool all_done = group.WaitFor(3s);
    stop.store(true, std::memory_order_release);
    join_with_watchdog(native_owner, 3s, "native owner");
    join_with_watchdog(native_notifier, 3s, "native notifier");
    const bool joined = mutex_fiber->wait_for(2s) &&
                        condition_fiber->wait_for(2s) &&
                        blocking_fiber->wait_for(2s);
    peak_workers.store(
        std::max(peak_workers.load(std::memory_order_relaxed),
                 scheduler.WorkerCount()),
        std::memory_order_relaxed);
    scheduler.Shutdown();

    std::cout << "mixed sync: native_reacquired="
              << native_reacquired.load() << " external_policy="
              << external_policy_seen.load() << " peak_workers="
              << peak_workers.load() << " all_done=" << all_done << '\n';
    return ok.load() && all_done && joined &&
           native_reacquired.load() && external_policy_seen.load()
               ? 0
               : 1;
}
