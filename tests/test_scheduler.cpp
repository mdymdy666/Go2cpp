#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/thread_policy.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

/**
 * @brief 验证调度器任务生命周期、取消、阻塞区和关闭流程。
 * @return 无；测试失败由统一断言统计。
 */
void run_scheduler_tests() {
    go2cpp_tests::announce("GMP scheduler state and shutdown");
    using namespace go2cpp;
    using namespace std::chrono_literals;

    // 观察者是可选插件；测试只记录事件计数，不在 worker 回调中调用
    // Scheduler，验证扩展点不会改变 G/M/P 状态机或引入回调重入。
    struct Observer final : SchedulerObserver {
        std::atomic<int> started{0};
        std::atomic<int> stopping{0};
        std::atomic<int> workers_started{0};
        std::atomic<int> workers_stopped{0};
        std::atomic<int> tasks_started{0};
        std::atomic<int> tasks_terminal{0};

        void OnEvent(const SchedulerEvent& event) noexcept override {
            switch (event.type) {
                case SchedulerEventType::kStarted:
                    started.fetch_add(1, std::memory_order_relaxed);
                    break;
                case SchedulerEventType::kStopping:
                    stopping.fetch_add(1, std::memory_order_relaxed);
                    break;
                case SchedulerEventType::kWorkerStarted:
                    workers_started.fetch_add(1, std::memory_order_relaxed);
                    break;
                case SchedulerEventType::kWorkerStopped:
                    workers_stopped.fetch_add(1, std::memory_order_relaxed);
                    break;
                case SchedulerEventType::kTaskStarted:
                    tasks_started.fetch_add(1, std::memory_order_relaxed);
                    break;
                case SchedulerEventType::kTaskCompleted:
                case SchedulerEventType::kTaskFailed:
                case SchedulerEventType::kTaskSuspended:
                    tasks_terminal.fetch_add(1, std::memory_order_relaxed);
                    break;
            }
        }
    };
    auto observer = std::make_shared<Observer>();

    SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.local_queue_limit = 4;
    config.observer = observer;
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
            go2cpp_tests::yield_for_watchdog();
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
    const auto terminal_deadline = std::chrono::steady_clock::now() + 3s;
    bool all_terminal = false;
    while (std::chrono::steady_clock::now() < terminal_deadline) {
        all_terminal = std::all_of(
            tasks.begin(), tasks.end(), [](const auto& task) {
                return task->state() == GState::kDead;
            });
        if (all_terminal) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    GO2CPP_CHECK(all_terminal);
    GO2CPP_CHECK(scheduler.runnable_count() == 0);

    std::atomic<int> yielded{0};
    std::atomic<bool> published{false};
    std::shared_ptr<Task> yielding;
    yielding = std::make_shared<Task>([&] {
        // enqueue() may run the task before it returns. Publish the
        // self-reference before the Fiber reads it.
        GO2CPP_REQUIRE_EVENTUALLY(
            published.load(std::memory_order_acquire), 3s);
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
        go2cpp_tests::yield_for_watchdog();
    }
    GO2CPP_CHECK(yielded.load(std::memory_order_acquire) == 2);

    std::atomic<bool> running_entered{false};
    std::atomic<bool> running_release{false};
    auto externally_touched = scheduler.spawn([&] {
        running_entered.store(true, std::memory_order_release);
        const auto release_deadline = std::chrono::steady_clock::now() + 5s;
        while (!running_release.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < release_deadline) {
            go2cpp_tests::yield_for_watchdog();
        }
        if (!running_release.load(std::memory_order_acquire)) {
            go2cpp_tests::watchdog_abort("running task release", __FILE__,
                                         __LINE__);
        }
    });
    const auto running_deadline = std::chrono::steady_clock::now() + 1s;
    while ((!running_entered.load(std::memory_order_acquire) ||
             externally_touched->state() != GState::kRunning) &&
           std::chrono::steady_clock::now() < running_deadline) {
        go2cpp_tests::yield_for_watchdog();
    }
    GO2CPP_CHECK(running_entered.load(std::memory_order_acquire));
    GO2CPP_CHECK(externally_touched->state() == GState::kRunning);
    GO2CPP_CHECK(!scheduler.yield(externally_touched));
    GO2CPP_CHECK(!scheduler.park(externally_touched));
    running_release.store(true, std::memory_order_release);
    while (externally_touched->state() != GState::kDead &&
           std::chrono::steady_clock::now() < running_deadline) {
        go2cpp_tests::yield_for_watchdog();
    }
    GO2CPP_CHECK(externally_touched->state() == GState::kDead);

    scheduler.shutdown();
    GO2CPP_CHECK(!scheduler.is_running());
    GO2CPP_CHECK(scheduler.worker_count() == 0);
    GO2CPP_CHECK(observer->started.load(std::memory_order_relaxed) == 1);
    GO2CPP_CHECK(observer->stopping.load(std::memory_order_relaxed) == 1);
    GO2CPP_CHECK(observer->workers_started.load(std::memory_order_relaxed) >= 1);
    GO2CPP_CHECK(observer->workers_stopped.load(std::memory_order_relaxed) >= 1);
    GO2CPP_CHECK(observer->tasks_started.load(std::memory_order_relaxed) >=
                 task_count);
    GO2CPP_CHECK(observer->tasks_terminal.load(std::memory_order_relaxed) >=
                 task_count);
    for (const auto& processor : scheduler.processors()) {
        GO2CPP_CHECK(processor.state == PState::kDead);
    }
    auto rejected = scheduler.spawn([] {});
    GO2CPP_CHECK(rejected->state() == GState::kCancelled);

    // A cancelled, not-yet-started G can remain as a queue node while an M
    // is occupied by another callable. Once that node is dequeued, the
    // scheduler must release the callable captures rather than retaining
    // user resources in the task registry forever.
    SchedulerConfig cancelled_config;
    cancelled_config.processor_count = 1;
    cancelled_config.min_workers = 1;
    cancelled_config.max_workers = 1;
    cancelled_config.allow_worker_oversubscription = false;
    Scheduler cancelled_queue(cancelled_config);
    cancelled_queue.start();
    std::atomic<bool> blocker_entered{false};
    std::atomic<bool> release_blocker{false};
    auto blocker = cancelled_queue.spawn([&] {
        blocker_entered.store(true, std::memory_order_release);
        const auto release_deadline = std::chrono::steady_clock::now() + 5s;
        while (!release_blocker.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < release_deadline) {
            go2cpp_tests::yield_for_watchdog();
        }
        if (!release_blocker.load(std::memory_order_acquire)) {
            go2cpp_tests::watchdog_abort("blocker release", __FILE__,
                                         __LINE__);
        }
    });
    const auto blocker_deadline = std::chrono::steady_clock::now() + 1s;
    while (!blocker_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < blocker_deadline) {
        go2cpp_tests::yield_for_watchdog();
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
        go2cpp_tests::yield_for_watchdog();
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
            go2cpp_tests::yield_for_watchdog();
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
        go2cpp_tests::yield_for_watchdog();
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
        go2cpp_tests::yield_for_watchdog();
    }
    GO2CPP_CHECK(pre_cancel_capture_weak.expired());
    pre_cancel_owner.shutdown();

    Scheduler concurrent_shutdown(config);
    std::atomic<int> shutdown_arrived{0};
    std::atomic<int> shutdown_returned{0};
    for (int i = 0; i < 2; ++i) {
        concurrent_shutdown.spawn([&] {
            shutdown_arrived.fetch_add(1, std::memory_order_acq_rel);
            const auto arrival_deadline =
                std::chrono::steady_clock::now() + 5s;
            while (shutdown_arrived.load(std::memory_order_acquire) != 2 &&
                   std::chrono::steady_clock::now() < arrival_deadline) {
                go2cpp_tests::yield_for_watchdog();
            }
            if (shutdown_arrived.load(std::memory_order_acquire) != 2) {
                go2cpp_tests::watchdog_abort("concurrent shutdown arrival",
                                             __FILE__, __LINE__);
            }
            concurrent_shutdown.shutdown();
            shutdown_returned.fetch_add(1, std::memory_order_release);
        });
    }
    concurrent_shutdown.start();
    const auto shutdown_deadline = std::chrono::steady_clock::now() + 2s;
    while (shutdown_returned.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < shutdown_deadline) {
        go2cpp_tests::yield_for_watchdog();
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
        go2cpp_tests::yield_for_watchdog();
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
        const auto cancellation_deadline =
            std::chrono::steady_clock::now() + 10s;
        while (Scheduler::current_task() &&
               !Scheduler::current_task()->cancellation_requested() &&
               std::chrono::steady_clock::now() < cancellation_deadline) {
            (void)Scheduler::current_scheduler()->yield_current();
            // Memcheck needs a small native pause here; other sanitizer
            // runs retain the original cooperative yield-only behavior.
            go2cpp_tests::pause_for_watchdog();
        }
        if (Scheduler::current_task() &&
            !Scheduler::current_task()->cancellation_requested()) {
            go2cpp_tests::watchdog_abort("join target cancellation", __FILE__,
                                         __LINE__);
        }
    });
    auto cancellable_joiner = std::make_shared<Task>([&] {
        parked_in_join.store(true, std::memory_order_release);
        const bool joined = never->wait_for(5s);
        cancelled_join_returned.store(!joined, std::memory_order_release);
    });
    GO2CPP_CHECK(cancel_join_scheduler.enqueue(cancellable_joiner));
    GO2CPP_CHECK(cancel_join_scheduler.enqueue(never));
    GO2CPP_REQUIRE_EVENTUALLY(
        parked_in_join.load(std::memory_order_acquire) &&
            cancellable_joiner->state() == GState::kWaiting,
        5s);
    GO2CPP_REQUIRE(cancellable_joiner->Cancel());
    GO2CPP_REQUIRE(cancellable_joiner->wait_for(5s));
    GO2CPP_CHECK(cancelled_join_returned.load(std::memory_order_acquire));
    cancel_join_scheduler.shutdown();

    // Participation and hook policy are explicit TLS scopes.  Merely marking
    // an external thread eligible does not change Scheduler::current_scheduler
    // or execute a queue item; only an internal worker is a runtime M.
    GO2CPP_CHECK(!go2cpp::ThreadParticipatesInGMP());
    {
        go2cpp::ScopedThreadParticipation participation(
            scheduler, go2cpp::ThreadParticipationMode::kGmpEligible);
        const auto snapshot = go2cpp::CurrentThreadPolicy();
        GO2CPP_CHECK(snapshot.participates_in_gmp());
        GO2CPP_CHECK(!snapshot.runtime_worker);
        GO2CPP_CHECK(go2cpp::Scheduler::current_scheduler() == nullptr);
        {
            go2cpp::ScopedThreadParticipation unmanaged(
                nullptr, go2cpp::ThreadParticipationMode::kUnmanaged);
            GO2CPP_CHECK(!go2cpp::ThreadParticipatesInGMP());
        }
        GO2CPP_CHECK(go2cpp::ThreadParticipatesInGMP());
    }
    GO2CPP_CHECK(!go2cpp::ThreadParticipatesInGMP());

    const auto original_hook_mode = go2cpp::CurrentThreadHookMode();
    {
        go2cpp::ScopedThreadHookMode disabled(
            go2cpp::ThreadHookMode::kDisabled);
        GO2CPP_CHECK(go2cpp::CurrentThreadHookMode() ==
                     go2cpp::ThreadHookMode::kDisabled);
    }
    GO2CPP_CHECK(go2cpp::CurrentThreadHookMode() == original_hook_mode);

    Scheduler policy_scheduler(1);
    policy_scheduler.start();
    std::atomic<bool> worker_policy_seen{false};
    std::atomic<bool> nested_worker_policy_seen{false};
    auto policy_task = policy_scheduler.spawn([&] {
        const auto snapshot = go2cpp::CurrentThreadPolicy();
        worker_policy_seen.store(
            snapshot.runtime_worker &&
                snapshot.scheduler == &policy_scheduler &&
                snapshot.participation ==
                    go2cpp::ThreadParticipationMode::kGmpEligible,
            std::memory_order_release);
        {
            // An external-style nested scope must not lie about the M/P
            // binding of an actual runtime worker.
            go2cpp::ScopedThreadParticipation nested(
                nullptr, go2cpp::ThreadParticipationMode::kUnmanaged);
            const auto nested_snapshot = go2cpp::CurrentThreadPolicy();
            nested_worker_policy_seen.store(
                nested_snapshot.runtime_worker &&
                    nested_snapshot.scheduler == &policy_scheduler &&
                    nested_snapshot.participation ==
                        go2cpp::ThreadParticipationMode::kGmpEligible,
                std::memory_order_release);
        }
    });
    GO2CPP_CHECK(policy_task->wait_for(1s));
    GO2CPP_CHECK(worker_policy_seen.load(std::memory_order_acquire));
    GO2CPP_CHECK(nested_worker_policy_seen.load(std::memory_order_acquire));

    std::atomic<bool> blocking_api_reusable{false};
    auto blocking_api_task = policy_scheduler.spawn([&] {
        const bool first = go2cpp::Scheduler::enter_blocking();
        go2cpp::Scheduler::leave_blocking();
        const bool second = go2cpp::Scheduler::enter_blocking();
        go2cpp::Scheduler::leave_blocking();
        blocking_api_reusable.store(first && second,
                                    std::memory_order_release);
    });
    GO2CPP_CHECK(blocking_api_task->wait_for(1s));
    GO2CPP_CHECK(blocking_api_reusable.load(std::memory_order_acquire));
    policy_scheduler.shutdown();

    // 普通 C++ 异常只在 Fiber 边界记录为失败；Task 的终态和
    // exception_ptr 都必须可观察。
    Scheduler failure_scheduler(1);
    failure_scheduler.start();
    auto failed_task = failure_scheduler.spawn([] {
        throw std::runtime_error("scheduler task failure");
    });
    GO2CPP_CHECK(failed_task->wait_for(1s));
    GO2CPP_CHECK(failed_task->state() == GState::kFailed);
    GO2CPP_CHECK(failed_task->failed());
    GO2CPP_CHECK(static_cast<bool>(failed_task->failure()));
    bool rethrown = false;
    try {
        failed_task->rethrow_failure();
    } catch (const std::runtime_error& error) {
        rethrown = std::string_view(error.what()) ==
                   "scheduler task failure";
    } catch (...) {
        rethrown = false;
    }
    GO2CPP_CHECK(rethrown);
    failure_scheduler.shutdown();

    // 有界 shutdown 不会强行释放仍在原生调用中的 Fiber 栈；超时后可
    // 等待调用自然返回，再次 shutdown_for 完成 worker 回收。
    Scheduler bounded_scheduler(1);
    bounded_scheduler.start();
    std::atomic<bool> blocking_entered{false};
    auto blocking_task = bounded_scheduler.spawn([&] {
        blocking_entered.store(true, std::memory_order_release);
        std::this_thread::sleep_for(80ms);
    });
    GO2CPP_REQUIRE_EVENTUALLY(
        blocking_entered.load(std::memory_order_acquire), 1s);
    GO2CPP_CHECK(!bounded_scheduler.shutdown_for(1ms));
    GO2CPP_CHECK(blocking_task->wait_for(1s));
    GO2CPP_CHECK(bounded_scheduler.shutdown_for(1s));

}
