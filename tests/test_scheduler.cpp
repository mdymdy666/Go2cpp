#include "go2cpp/fiber.hpp"
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
        // self-reference before the Fiber reads it.
        while (!published.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        yielded.fetch_add(1, std::memory_order_release);
        GO2CPP_CHECK(scheduler.yield(yielding));
        // A stackful G continues here; its callable is not invoked again.
        yielded.fetch_add(1, std::memory_order_release);
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

    // A cancelled, not-yet-started G can remain as a queue node while an M
    // is occupied by another callable. Once that node is dequeued, the
    // scheduler must release the callable captures rather than retaining
    // user resources in the task registry forever.
    Scheduler cancelled_queue(1);
    cancelled_queue.start();
    std::atomic<bool> blocker_entered{false};
    std::atomic<bool> release_blocker{false};
    auto blocker = cancelled_queue.spawn([&] {
        blocker_entered.store(true, std::memory_order_release);
        while (!release_blocker.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    const auto blocker_deadline = std::chrono::steady_clock::now() + 1s;
    while (!blocker_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < blocker_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(blocker_entered.load(std::memory_order_acquire));
    auto retained_capture = std::make_shared<int>(42);
    std::weak_ptr<int> retained_capture_weak = retained_capture;
    auto cancelled_queued =
        cancelled_queue.spawn([retained_capture] { (void)retained_capture; });
    GO2CPP_CHECK(cancelled_queued->cancel());
    retained_capture.reset();
    cancelled_queued.reset();
    GO2CPP_CHECK(!retained_capture_weak.expired());
    release_blocker.store(true, std::memory_order_release);
    const auto release_deadline = std::chrono::steady_clock::now() + 1s;
    while (!retained_capture_weak.expired() &&
           std::chrono::steady_clock::now() < release_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(retained_capture_weak.expired());
    GO2CPP_CHECK(blocker->wait_for(1s));
    cancelled_queue.shutdown();

    // Raw Fiber::Suspend(Park) is an advanced backend path. Cancellation may
    // arrive while the context switch is publishing Suspended; the task must
    // still be requeued and finish its continuation instead of remaining in
    // Waiting with no queue node.
    Scheduler raw_suspend_scheduler(1);
    raw_suspend_scheduler.start();
    for (int iteration = 0; iteration < 32; ++iteration) {
        std::atomic<bool> entered{false};
        std::atomic<bool> resumed_after_cancel{false};
        auto raw_task = raw_suspend_scheduler.spawn([&] {
            entered.store(true, std::memory_order_release);
            (void)go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Park);
            resumed_after_cancel.store(
                go2cpp::Fiber::CancellationRequested(),
                std::memory_order_release);
        });
        const auto entered_deadline = std::chrono::steady_clock::now() + 1s;
        while (!entered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < entered_deadline) {
            std::this_thread::yield();
        }
        GO2CPP_CHECK(entered.load(std::memory_order_acquire));
        GO2CPP_CHECK(raw_task->cancel());
        GO2CPP_CHECK(raw_task->wait_for(1s));
        GO2CPP_CHECK(resumed_after_cancel.load(std::memory_order_acquire));
    }
    raw_suspend_scheduler.shutdown();

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

    // A pre-cancelled public Task must be rejected before scheduler
    // registration. Otherwise an external handle can disappear while its
    // callable capture remains retained until a later shutdown collection.
    Scheduler pre_cancel_owner(sparse_config);
    pre_cancel_owner.start();
    auto pre_cancel_capture = std::make_shared<int>(7);
    std::weak_ptr<int> pre_cancel_capture_weak = pre_cancel_capture;
    auto pre_cancelled = std::make_shared<Task>([pre_cancel_capture] {
        (void)pre_cancel_capture;
    });
    GO2CPP_CHECK(pre_cancelled->Cancel());
    pre_cancel_capture.reset();
    GO2CPP_CHECK(!pre_cancel_owner.enqueue(pre_cancelled));
    pre_cancelled.reset();
    const auto pre_cancel_deadline = std::chrono::steady_clock::now() + 1s;
    while (!pre_cancel_capture_weak.expired() &&
           std::chrono::steady_clock::now() < pre_cancel_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(pre_cancel_capture_weak.expired());
    pre_cancel_owner.shutdown();

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

    // A managed Join parks the current Fiber rather than consuming the only
    // P with an OS-thread condition-variable wait. The child is deliberately
    // queued after its parent, so P=1 must resume the child to make progress.
    Scheduler join_scheduler(1);
    join_scheduler.start();
    std::atomic<bool> child_started{false};
    std::atomic<bool> parent_finished{false};
    std::atomic<bool> parent_joined{false};
    auto child = std::make_shared<Task>([&] {
        child_started.store(true, std::memory_order_release);
    });
    auto parent = std::make_shared<Task>([&] {
        parent_joined.store(child->wait_for(1s), std::memory_order_release);
        parent_finished.store(true, std::memory_order_release);
    });
    GO2CPP_CHECK(join_scheduler.enqueue(parent));
    GO2CPP_CHECK(join_scheduler.enqueue(child));
    const auto join_deadline = std::chrono::steady_clock::now() + 2s;
    while (!parent_finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < join_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(child_started.load(std::memory_order_acquire));
    GO2CPP_CHECK(parent_finished.load(std::memory_order_acquire));
    GO2CPP_CHECK(parent_joined.load(std::memory_order_acquire));
    GO2CPP_CHECK(parent->wait_for(1s));
    GO2CPP_CHECK(child->wait_for(1s));
    join_scheduler.shutdown();

    // Cancelling a G parked in Join must wake it through the owner gate and
    // let its Fiber return before the scheduler is destroyed.
    Scheduler cancel_join_scheduler(1);
    cancel_join_scheduler.start();
    std::atomic<bool> parked_in_join{false};
    std::atomic<bool> cancelled_join_returned{false};
    auto never = std::make_shared<Task>([] {
        while (Scheduler::current_task() &&
               !Scheduler::current_task()->cancellation_requested()) {
            (void)Scheduler::current_scheduler()->yield_current();
        }
    });
    auto cancellable_joiner = std::make_shared<Task>([&] {
        parked_in_join.store(true, std::memory_order_release);
        const bool joined = never->wait();
        cancelled_join_returned.store(!joined, std::memory_order_release);
    });
    GO2CPP_CHECK(cancel_join_scheduler.enqueue(cancellable_joiner));
    GO2CPP_CHECK(cancel_join_scheduler.enqueue(never));
    const auto parked_deadline = std::chrono::steady_clock::now() + 2s;
    while ((!parked_in_join.load(std::memory_order_acquire) ||
             cancellable_joiner->state() != GState::kWaiting) &&
           std::chrono::steady_clock::now() < parked_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(parked_in_join.load(std::memory_order_acquire));
    GO2CPP_CHECK(cancellable_joiner->state() == GState::kWaiting);
    GO2CPP_CHECK(cancellable_joiner->Cancel());
    GO2CPP_CHECK(cancellable_joiner->wait_for(2s));
    GO2CPP_CHECK(cancelled_join_returned.load(std::memory_order_acquire));
    cancel_join_scheduler.shutdown();
}
