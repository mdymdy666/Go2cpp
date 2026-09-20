#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

#define SMOKE_CHECK(expression)                                              \
    do {                                                                      \
        if (!(expression)) {                                                  \
            std::fprintf(stderr, "scheduler smoke check failed: %s (%s:%d)\n", \
                         #expression, __FILE__, __LINE__);                    \
            std::abort();                                                     \
        }                                                                     \
    } while (false)

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

void test_single_processor_fifo_and_completion() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    config.local_queue_limit = 4;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    std::atomic<int> completed{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int i = 0; i < 100; ++i) {
        tasks.push_back(scheduler.spawn([&completed] {
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    SMOKE_CHECK(wait_until([&] { return completed.load() == 100; }));
    for (const auto& task : tasks) {
        SMOKE_CHECK(task->wait_for(3s));
        SMOKE_CHECK(task->state() == go2cpp::GState::kDead);
    }
    scheduler.shutdown();
    SMOKE_CHECK(!scheduler.is_running());
}

void test_steal_and_current_identity() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 4;
    config.max_workers = 4;
    config.local_queue_limit = 0;  // exercise the global FIFO path
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    std::atomic<int> completed{0};
    std::mutex mutex;
    std::set<std::thread::id> workers;
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int i = 0; i < 200; ++i) {
        tasks.push_back(scheduler.spawn([&] {
            SMOKE_CHECK(go2cpp::Scheduler::current_scheduler() != nullptr);
            SMOKE_CHECK(go2cpp::Scheduler::current_task() != nullptr);
            std::lock_guard<std::mutex> lock(mutex);
            workers.insert(std::this_thread::get_id());
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    SMOKE_CHECK(wait_until([&] { return completed.load() == 200; }));
    SMOKE_CHECK(scheduler.global_runnable_count() == 0);
    SMOKE_CHECK(!workers.empty());
    scheduler.shutdown();
}

void test_yield_and_park_wake() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    go2cpp::Scheduler scheduler(config);
    std::atomic<int> yield_runs{0};
    std::shared_ptr<go2cpp::Task> yielding;
    yielding = scheduler.spawn([&] {
        yield_runs.fetch_add(1, std::memory_order_release);
        SMOKE_CHECK(scheduler.yield(yielding));
        yield_runs.fetch_add(1, std::memory_order_release);
    });

    std::atomic<int> park_runs{0};
    std::shared_ptr<go2cpp::Task> parked;
    parked = scheduler.spawn([&] {
        park_runs.fetch_add(1, std::memory_order_release);
        SMOKE_CHECK(scheduler.park(parked));
        park_runs.fetch_add(1, std::memory_order_release);
    });
    scheduler.start();
    SMOKE_CHECK(wait_until([&] { return yield_runs.load() == 2; }));
    SMOKE_CHECK(wait_until([&] { return park_runs.load() == 1; }));
    SMOKE_CHECK(parked->state() == go2cpp::GState::kWaiting);
    (void)scheduler.wake(parked);
    SMOKE_CHECK(wait_until([&] { return park_runs.load() == 2; }));
    scheduler.shutdown();
}

void test_wake_before_park_handoff() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    go2cpp::Scheduler scheduler(config);
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> attempted{false};
    std::atomic<bool> park_return{true};
    std::shared_ptr<go2cpp::Task> task;
    task = std::make_shared<go2cpp::Task>([&] {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (!attempted.exchange(true, std::memory_order_acq_rel)) {
            park_return.store(scheduler.park(task), std::memory_order_release);
        }
    });
    SMOKE_CHECK(scheduler.enqueue(task));
    scheduler.start();
    SMOKE_CHECK(wait_until([&] {
        return entered.load(std::memory_order_acquire) &&
               task->state() == go2cpp::GState::kRunning;
    }));

    // The task is still running, so wake() cannot enqueue it directly. The
    // pending token must nevertheless prevent the subsequent park from being
    // lost and the task must complete without an external second wake.
    SMOKE_CHECK(!scheduler.wake(task));
    release.store(true, std::memory_order_release);
    SMOKE_CHECK(wait_until([&] {
        return task->state() == go2cpp::GState::kDead;
    }));
    SMOKE_CHECK(!park_return.load(std::memory_order_acquire));
    scheduler.shutdown();
}

void test_wake_token_cleared_by_yield() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    go2cpp::Scheduler scheduler(config);
    std::atomic<int> runs{0};
    std::atomic<bool> entered{false};
    std::atomic<bool> release_yield{false};
    std::atomic<bool> park_entered{false};
    std::atomic<bool> park_return{false};
    std::shared_ptr<go2cpp::Task> task;
    task = std::make_shared<go2cpp::Task>([&] {
        runs.fetch_add(1, std::memory_order_acq_rel);
        entered.store(true, std::memory_order_release);
        while (!release_yield.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        SMOKE_CHECK(scheduler.yield(task));
        runs.fetch_add(1, std::memory_order_acq_rel);
        park_entered.store(true, std::memory_order_release);
        park_return.store(scheduler.park(task), std::memory_order_release);
        runs.fetch_add(1, std::memory_order_acq_rel);
    });
    SMOKE_CHECK(scheduler.enqueue(task));
    scheduler.start();
    SMOKE_CHECK(wait_until([&] {
        return entered.load(std::memory_order_acquire) &&
               task->state() == go2cpp::GState::kRunning;
    }));
    SMOKE_CHECK(!scheduler.wake(task));
    release_yield.store(true, std::memory_order_release);
    SMOKE_CHECK(wait_until([&] {
        return park_entered.load(std::memory_order_acquire) &&
               task->state() == go2cpp::GState::kWaiting;
    }));
    SMOKE_CHECK(!park_return.load(std::memory_order_acquire));
    (void)scheduler.wake(task);
    SMOKE_CHECK(wait_until([&] {
        return runs.load(std::memory_order_acquire) >= 3 &&
               task->state() == go2cpp::GState::kDead;
    }));
    SMOKE_CHECK(park_return.load(std::memory_order_acquire));
    scheduler.shutdown();
}

void test_enqueue_rejected_while_yielding_callable_is_active() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    go2cpp::Scheduler scheduler(config);
    std::atomic<int> runs{0};
    std::atomic<int> active{0};
    std::atomic<int> duplicate{0};
    std::shared_ptr<go2cpp::Task> task;
    task = std::make_shared<go2cpp::Task>([&] {
        if (active.fetch_add(1, std::memory_order_acq_rel) != 0) {
            duplicate.fetch_add(1, std::memory_order_relaxed);
        }
        runs.fetch_add(1, std::memory_order_release);
        SMOKE_CHECK(scheduler.yield(task));
        runs.fetch_add(1, std::memory_order_release);
        SMOKE_CHECK(scheduler.yield(task));
        runs.fetch_add(1, std::memory_order_release);
        active.fetch_sub(1, std::memory_order_release);
    });
    SMOKE_CHECK(scheduler.enqueue(task));
    scheduler.start();
    const auto duplicate_deadline = std::chrono::steady_clock::now() + 3s;
    while (task->state() != go2cpp::GState::kDead &&
           std::chrono::steady_clock::now() < duplicate_deadline) {
        (void)scheduler.enqueue(task);
        std::this_thread::yield();
    }
    SMOKE_CHECK(wait_until([&] {
        return runs.load(std::memory_order_acquire) == 3 &&
               task->state() == go2cpp::GState::kDead;
    }));
    SMOKE_CHECK(duplicate.load(std::memory_order_acquire) == 0);
    scheduler.shutdown();
}

void test_wake_after_park_defers_until_callable_returns() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    go2cpp::Scheduler scheduler(config);
    std::atomic<int> runs{0};
    std::atomic<bool> parked{false};
    std::shared_ptr<go2cpp::Task> task;
    task = std::make_shared<go2cpp::Task>([&] {
        runs.fetch_add(1, std::memory_order_release);
        parked.store(true, std::memory_order_release);
        SMOKE_CHECK(scheduler.park(task));
        runs.fetch_add(1, std::memory_order_release);
    });
    SMOKE_CHECK(scheduler.enqueue(task));
    scheduler.start();
    SMOKE_CHECK(wait_until([&] {
        return parked.load(std::memory_order_acquire) &&
               task->state() == go2cpp::GState::kWaiting;
    }));

    (void)scheduler.wake(task);
    SMOKE_CHECK(wait_until([&] {
        return runs.load(std::memory_order_acquire) == 2 &&
               task->state() == go2cpp::GState::kDead;
    }));
    scheduler.shutdown();
}

void test_shutdown_cancels_waiting_task() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    go2cpp::Scheduler scheduler(config);
    std::atomic<bool> parked{false};
    std::atomic<bool> park_result{true};
    std::shared_ptr<go2cpp::Task> task;
    task = scheduler.spawn([&] {
        parked.store(true, std::memory_order_release);
        park_result.store(scheduler.park(task), std::memory_order_release);
    });
    scheduler.start();
    SMOKE_CHECK(wait_until([&] {
        return parked.load(std::memory_order_acquire) &&
               task->state() == go2cpp::GState::kWaiting;
    }));
    scheduler.shutdown();
    SMOKE_CHECK(task->state() == go2cpp::GState::kCancelled);
    SMOKE_CHECK(!park_result.load(std::memory_order_acquire));
}

void test_exception_isolation_and_shutdown_cancel() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    auto bad = scheduler.spawn([] { throw std::runtime_error("task failure"); });
    std::atomic<bool> reached{false};
    auto good = scheduler.spawn([&] { reached.store(true); });
    SMOKE_CHECK(wait_until([&] { return reached.load(); }));
    SMOKE_CHECK(bad->state() == go2cpp::GState::kDead);
    SMOKE_CHECK(good->state() == go2cpp::GState::kDead);

    std::atomic<bool> release{false};
    auto blocker = scheduler.spawn([&] {
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    SMOKE_CHECK(wait_until([&] { return blocker->state() == go2cpp::GState::kRunning; }));
    std::vector<std::shared_ptr<go2cpp::Task>> pending;
    for (int i = 0; i < 10; ++i) {
        pending.push_back(scheduler.spawn([] {}));
    }
    std::thread stopper([&] {
        std::this_thread::sleep_for(5ms);
        release.store(true, std::memory_order_release);
        scheduler.shutdown();
    });
    stopper.join();
    SMOKE_CHECK(blocker->state() == go2cpp::GState::kDead ||
                blocker->state() == go2cpp::GState::kCancelled);
    for (const auto& task : pending) {
        SMOKE_CHECK(task->state() == go2cpp::GState::kCancelled ||
                    task->state() == go2cpp::GState::kDead);
    }
}

void test_pre_start_shutdown_cancels_admitted_tasks() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    go2cpp::Scheduler scheduler(config);
    auto task = scheduler.spawn([] {});
    SMOKE_CHECK(task->state() == go2cpp::GState::kRunnable);
    scheduler.shutdown();
    SMOKE_CHECK(task->state() == go2cpp::GState::kCancelled);
    SMOKE_CHECK(scheduler.runnable_count() == 0);
}

void test_shutdown_destroys_queue_tasks_outside_runtime_locks() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    go2cpp::Scheduler scheduler(config);
    std::atomic<bool> destructor_reentered{false};

    struct ReenterOnDestroy {
        go2cpp::Scheduler* scheduler;
        std::atomic<bool>* observed;

        ReenterOnDestroy(go2cpp::Scheduler* owner,
                         std::atomic<bool>* flag)
            : scheduler(owner), observed(flag) {}
        ReenterOnDestroy(const ReenterOnDestroy&) = delete;
        ReenterOnDestroy& operator=(const ReenterOnDestroy&) = delete;

        ~ReenterOnDestroy() {
            observed->store(true, std::memory_order_release);
            auto transient = std::make_shared<go2cpp::Task>([] {});
            (void)scheduler->enqueue(transient);
        }
    };

    auto probe = std::make_shared<ReenterOnDestroy>(&scheduler,
                                                    &destructor_reentered);
    auto queued = std::make_shared<go2cpp::Task>([probe] {});
    SMOKE_CHECK(scheduler.enqueue(queued));
    queued.reset();
    probe.reset();
    scheduler.shutdown();
    SMOKE_CHECK(destructor_reentered.load(std::memory_order_acquire));
}

}  // namespace

int run_go2cpp_scheduler_smoke_tests() {
    test_single_processor_fifo_and_completion();
    test_steal_and_current_identity();
    test_yield_and_park_wake();
    test_wake_before_park_handoff();
    test_wake_token_cleared_by_yield();
    test_enqueue_rejected_while_yielding_callable_is_active();
    test_wake_after_park_defers_until_callable_returns();
    test_shutdown_cancels_waiting_task();
    test_exception_isolation_and_shutdown_cancel();
    test_pre_start_shutdown_cancels_admitted_tasks();
    test_shutdown_destroys_queue_tasks_outside_runtime_locks();
    return 0;
}

#ifdef GO2CPP_SCHEDULER_SMOKE_MAIN
int main() { return run_go2cpp_scheduler_smoke_tests(); }
#endif
