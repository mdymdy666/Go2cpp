#include "go2cpp/scheduler.hpp"
#include "go2cpp/panic_defer.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

void run_scheduler_tests() {
    go2cpp_tests::announce("GMP scheduler state and shutdown");
    using namespace go2cpp;
    using namespace std::chrono_literals;

    SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.local_queue_limit = 4;
    Scheduler scheduler(config);
    scheduler.start();
    GO2CPP_CHECK(scheduler.is_running());
    GO2CPP_CHECK(scheduler.processor_count() == 2);

    constexpr int task_count = 120;
    std::atomic<int> completed{0};
    std::atomic<int> duplicate{0};
    std::mutex seen_mutex;
    std::vector<const void*> seen;
    std::vector<std::shared_ptr<Task>> tasks;
    tasks.reserve(task_count);
    for (int i = 0; i < task_count; ++i) {
        tasks.push_back(scheduler.spawn([&] {
            const auto current = Scheduler::current_task();
            GO2CPP_CHECK(current != nullptr);
            {
                std::lock_guard<std::mutex> lock(seen_mutex);
                if (std::find(seen.begin(), seen.end(), current.get()) != seen.end()) {
                    duplicate.fetch_add(1, std::memory_order_relaxed);
                }
                seen.push_back(current.get());
            }
            std::this_thread::yield();
            completed.fetch_add(1, std::memory_order_release);
        }));
    }
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (completed.load(std::memory_order_acquire) != task_count &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    GO2CPP_CHECK(completed.load(std::memory_order_acquire) == task_count);
    GO2CPP_CHECK(duplicate.load(std::memory_order_relaxed) == 0);
    for (const auto& task : tasks) {
        GO2CPP_CHECK(task->state() == GState::kDead);
    }
    GO2CPP_CHECK(scheduler.runnable_count() == 0);

    std::atomic<int> yielded{0};
    std::atomic<bool> published{false};
    std::shared_ptr<Task> yielding;
    yielding = std::make_shared<Task>([&] {
        // enqueue() may run the task before it returns. Publish the
        // self-reference before the first callback reads it.
        while (!published.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const int run = yielded.fetch_add(1, std::memory_order_relaxed) + 1;
        if (run == 1) {
            GO2CPP_CHECK(scheduler.yield(yielding));
        }
    });
    // The release pairs with the callback's acquire before it reads the
    // shared self-reference.
    GO2CPP_CHECK(scheduler.enqueue(yielding));
    published.store(true, std::memory_order_release);
    const auto yield_deadline = std::chrono::steady_clock::now() + 1s;
    while (yielded.load(std::memory_order_acquire) < 2 &&
           std::chrono::steady_clock::now() < yield_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(yielded.load(std::memory_order_acquire) == 2);

    std::atomic<int> panic_runs{0};
    std::atomic<bool> panic_published{false};
    std::shared_ptr<Task> yield_then_panic;
    yield_then_panic = std::make_shared<Task>([&] {
        while (!panic_published.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (panic_runs.fetch_add(1, std::memory_order_relaxed) == 0) {
            GO2CPP_CHECK(scheduler.yield(yield_then_panic));
            go2cpp::panic_defer::panic(
                go2cpp::panic_defer::PanicValue::text("unhandled"));
        }
    });
    GO2CPP_CHECK(scheduler.enqueue(yield_then_panic));
    panic_published.store(true, std::memory_order_release);
    const auto panic_deadline = std::chrono::steady_clock::now() + 1s;
    while (yield_then_panic->state() != GState::kDead &&
           std::chrono::steady_clock::now() < panic_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(yield_then_panic->state() == GState::kDead);
    GO2CPP_CHECK(panic_runs.load(std::memory_order_acquire) == 1);

    std::atomic<bool> running_entered{false};
    std::atomic<bool> running_release{false};
    auto externally_touched = scheduler.spawn([&] {
        running_entered.store(true, std::memory_order_release);
        while (!running_release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    const auto running_deadline = std::chrono::steady_clock::now() + 1s;
    while ((!running_entered.load(std::memory_order_acquire) ||
             externally_touched->state() != GState::kRunning) &&
           std::chrono::steady_clock::now() < running_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(running_entered.load(std::memory_order_acquire));
    GO2CPP_CHECK(externally_touched->state() == GState::kRunning);
    GO2CPP_CHECK(!scheduler.yield(externally_touched));
    GO2CPP_CHECK(!scheduler.park(externally_touched));
    running_release.store(true, std::memory_order_release);
    while (externally_touched->state() != GState::kDead &&
           std::chrono::steady_clock::now() < running_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(externally_touched->state() == GState::kDead);

    scheduler.shutdown();
    GO2CPP_CHECK(!scheduler.is_running());
    GO2CPP_CHECK(scheduler.worker_count() == 0);
    for (const auto& processor : scheduler.processors()) {
        GO2CPP_CHECK(processor.state == PState::kDead);
    }
    auto rejected = scheduler.spawn([] {});
    GO2CPP_CHECK(rejected->state() == GState::kCancelled);

    SchedulerConfig sparse_config;
    sparse_config.processor_count = 4;
    sparse_config.max_workers = 1;
    Scheduler sparse(sparse_config);
    sparse.start();
    sparse.shutdown();
    for (const auto& processor : sparse.processors()) {
        GO2CPP_CHECK(processor.state == PState::kDead);
    }

    Scheduler never_started(sparse_config);
    never_started.shutdown();
    for (const auto& processor : never_started.processors()) {
        GO2CPP_CHECK(processor.state == PState::kDead);
    }

    // A task admitted by one scheduler cannot be migrated to another
    // scheduler's P/M table, and the rejected operation must not cancel it.
    Scheduler owner_a(sparse_config);
    Scheduler owner_b(sparse_config);
    auto foreign_task = owner_a.spawn([] {});
    GO2CPP_CHECK(foreign_task->state() == GState::kRunnable);
    GO2CPP_CHECK(!owner_b.enqueue(foreign_task));
    GO2CPP_CHECK(foreign_task->state() == GState::kRunnable);
    GO2CPP_CHECK(!owner_b.wake(foreign_task));
    owner_a.shutdown();
    owner_b.shutdown();

    Scheduler rejected_owner(sparse_config);
    rejected_owner.shutdown();
    auto unbound_task = std::make_shared<Task>([] {});
    GO2CPP_CHECK(!rejected_owner.enqueue(unbound_task));
    GO2CPP_CHECK(unbound_task->state() == GState::kNew);
    Scheduler accepting_owner(sparse_config);
    GO2CPP_CHECK(accepting_owner.enqueue(unbound_task));
    accepting_owner.start();
    const auto accepted_deadline = std::chrono::steady_clock::now() + 1s;
    while (unbound_task->state() != GState::kDead &&
           std::chrono::steady_clock::now() < accepted_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(unbound_task->state() == GState::kDead);
    accepting_owner.shutdown();

    Scheduler concurrent_shutdown(config);
    std::atomic<int> shutdown_arrived{0};
    std::atomic<int> shutdown_returned{0};
    for (int i = 0; i < 2; ++i) {
        concurrent_shutdown.spawn([&] {
            shutdown_arrived.fetch_add(1, std::memory_order_acq_rel);
            while (shutdown_arrived.load(std::memory_order_acquire) != 2) {
                std::this_thread::yield();
            }
            concurrent_shutdown.shutdown();
            shutdown_returned.fetch_add(1, std::memory_order_release);
        });
    }
    concurrent_shutdown.start();
    const auto shutdown_deadline = std::chrono::steady_clock::now() + 2s;
    while (shutdown_returned.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < shutdown_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(shutdown_returned.load(std::memory_order_acquire) == 2);
    concurrent_shutdown.shutdown();
    GO2CPP_CHECK(concurrent_shutdown.worker_count() == 0);
}
