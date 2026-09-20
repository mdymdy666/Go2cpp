#pragma once

#include "go2cpp/core/parking_condition.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace go2cpp {
class Fiber;
}

namespace go2cpp::scheduler {

using GId = std::uint64_t;
using MId = std::uint64_t;
using PId = std::uint32_t;

enum class WakeAction : std::uint8_t {
    kRejected,
    kEnqueue,
    kPending,
};

enum class ParkAction : std::uint8_t {
    kRejected,
    kParked,
    kRequeued,
};

enum class GState : std::uint8_t {
    kNew,
    kRunnable,
    kRunning,
    kWaiting,
    kDead,
    kCancelled,

    // Readable aliases for callers that mirror the Go terminology.
    New = kNew,
    Runnable = kRunnable,
    Running = kRunning,
    Waiting = kWaiting,
    Dead = kDead,
    Cancelled = kCancelled,
};

enum class MState : std::uint8_t {
    kIdle,
    kRunning,
    kBlocking,
    kParked,
    kStopping,
    kDead,

    Idle = kIdle,
    Running = kRunning,
    Blocking = kBlocking,
    Parked = kParked,
    Stopping = kStopping,
    Dead = kDead,
};

enum class PState : std::uint8_t {
    kIdle,
    kRunning,
    kStopping,
    kDead,

    Idle = kIdle,
    Running = kRunning,
    Stopping = kStopping,
    Dead = kDead,
};

struct SchedulerConfig {
    std::size_t processor_count = 0;
    std::size_t max_workers = 0;
    std::chrono::milliseconds idle_wait{10};
    std::size_t local_queue_limit = 256;
    // Zero selects one initial worker. This backend clamps the effective
    // max_workers to P; a blocking-region M>P handoff is not yet provided.
    std::size_t min_workers = 0;
    std::chrono::milliseconds idle_worker_timeout{250};
    std::size_t fiber_stack_size = 0;
    std::size_t task_affinity_budget = 4;
};

using TaskClassId = std::uint64_t;

struct TaskCancellationGate;

struct TaskOptions {
    std::size_t stack_size = 0;
    TaskClassId task_class = 0;
};

struct ProcessorSnapshot {
    PId id{0};
    PState state{PState::kIdle};
    std::size_t queued{0};
};

struct MachineSnapshot {
    MId id{0};
    MState state{MState::kIdle};
    PId processor{0};
    // Soft task-class locality counters. They are observational only: a
    // worker may always steal a different class when that is the only work.
    TaskClassId last_task_class{0};
    std::size_t affinity_hits{0};
    std::size_t affinity_misses{0};
};

class Task : public std::enable_shared_from_this<Task> {
public:
    using Function = std::function<void()>;

    explicit Task(Function function, TaskOptions options = {});
    ~Task();

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    GId id() const noexcept;
    GState state() const noexcept;
    bool queued() const noexcept;
    bool started() const noexcept;
    bool cancellation_requested() const noexcept;
    TaskClassId task_class() const noexcept;
    // Returns false for a self-join or when the joining managed G is
    // cancelled. Unmanaged callers retain the normal thread join behavior.
    bool wait() const;
    bool wait_for(std::chrono::steady_clock::duration timeout) const;
    GId Id() const noexcept { return id(); }
    GState State() const noexcept { return state(); }
    bool Queued() const noexcept { return queued(); }
    bool Started() const noexcept { return started(); }
    bool CancellationRequested() const noexcept {
        return cancellation_requested();
    }
    TaskClassId TaskClass() const noexcept { return task_class(); }
    bool Join() const { return wait(); }
    // Internal queue/run claims.  They are public so the replaceable worker
    // backend can enforce the same invariant without exposing data members.
    bool try_mark_queued() noexcept;
    void clear_queued() noexcept;
    void defer_enqueue() noexcept;
    bool consume_deferred_enqueue() noexcept;
    void request_wake() noexcept;
    bool consume_wake() noexcept;
    bool try_register() noexcept;
    // Bind a task to exactly one scheduler. The anchor is an opaque identity
    // token and does not retain the Scheduler object itself.
    bool bind_owner(const std::shared_ptr<const void>& owner) noexcept;
    bool owned_by(const std::shared_ptr<const void>& owner) const noexcept;
    void bind_cancellation_gate(
        const std::shared_ptr<TaskCancellationGate>& gate) noexcept;
    bool promote_new() noexcept;
    WakeAction wake_for_scheduler() noexcept;
    ParkAction park_for_scheduler() noexcept;
    bool try_mark_running();
    // Internal state transitions used by Scheduler. mark_runnable() only
    // wakes a task that is already waiting; a running task records a pending
    // wake token and must use the current-G-only yield/park path below.
    bool mark_runnable();
    bool mark_yielded();
    bool mark_waiting();
    bool cancel();
    // Cancel only an unqueued runnable task. This is used after an internal
    // requeue attempt so an already-accepted concurrent enqueue is not
    // mistaken for an admission failure.
    bool cancel_if_runnable_unqueued() noexcept;
    bool Cancel() { return cancel(); }
    // Backend entry point. Direct calls are serialized by run() too.
    void run();

private:
    friend class Scheduler;
    Function release_callable() noexcept;
    bool request_cancel(bool notify = true) noexcept;
    bool terminal() const noexcept;
    void notify_terminal() noexcept;

    GId m_id;
    Function m_function;
    TaskOptions m_options;
    std::unique_ptr<go2cpp::Fiber> m_fiber;
    std::atomic<GState> m_state;
    std::atomic<bool> m_queued{false};
    std::atomic<bool> m_deferred_enqueue{false};
    std::atomic<bool> m_wake_pending{false};
    std::atomic<bool> m_registered{false};
    std::atomic<bool> m_execution_claim{false};
    std::atomic<bool> m_run_claim{false};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_cancel_requested{false};
    // No-allocation rescue queue link. It is changed only under the owning
    // scheduler's admission mutex and never forms a cycle.
    std::shared_ptr<Task> m_emergency_next;
    std::shared_ptr<const void> m_owner_anchor;
    std::weak_ptr<TaskCancellationGate> m_cancellation_gate;
    std::shared_ptr<Task> m_completion_notification_next;
    std::atomic<bool> m_completion_notification_queued{false};
    std::atomic<bool> m_completion_notified{false};
    mutable std::mutex m_transition_mutex;
    mutable std::mutex m_completion_mutex;
    mutable core::ParkingCondition m_completion_condition;
};

class Scheduler {
public:
    explicit Scheduler(SchedulerConfig config = {});
    explicit Scheduler(std::size_t processor_count);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start();
    void shutdown();
    bool is_running() const noexcept;

    void Start() { start(); }
    void Shutdown() { shutdown(); }
    bool IsRunning() const noexcept { return is_running(); }

    std::shared_ptr<Task> spawn(Task::Function function);
    std::shared_ptr<Task> spawn(Task::Function function, TaskOptions options);
    std::shared_ptr<Task> go(Task::Function function) { return spawn(std::move(function)); }
    bool enqueue(const std::shared_ptr<Task>& task);
    bool yield(const std::shared_ptr<Task>& task);
    bool park(const std::shared_ptr<Task>& task);
    bool yield_current();
    bool park_current();
    // Returns true when a waiting G was enqueued immediately. A false result
    // may still represent an accepted pending wake for a currently running G;
    // that token is consumed by its next park.
    bool wake(const std::shared_ptr<Task>& task);
    // Notifier handoff used after a wait node chose a terminal outcome. It
    // preserves started Gs across shutdown/admission races instead of making
    // them terminal while their stack is still suspended.
    bool wake_or_cancel(const std::shared_ptr<Task>& task);
    bool cancel(const std::shared_ptr<Task>& task);

    std::shared_ptr<Task> Spawn(Task::Function function) {
        return spawn(std::move(function));
    }
    std::shared_ptr<Task> Go(Task::Function function) {
        return spawn(std::move(function));
    }
    bool Enqueue(const std::shared_ptr<Task>& task) { return enqueue(task); }
    bool Yield(const std::shared_ptr<Task>& task) { return yield(task); }
    bool Park(const std::shared_ptr<Task>& task) { return park(task); }
    bool Wake(const std::shared_ptr<Task>& task) { return wake(task); }
    bool WakeOrCancel(const std::shared_ptr<Task>& task) {
        return wake_or_cancel(task);
    }
    bool Cancel(const std::shared_ptr<Task>& task) { return cancel(task); }

    std::size_t processor_count() const noexcept;
    std::size_t worker_count() const noexcept;
    std::size_t runnable_count() const noexcept;
    std::size_t global_runnable_count() const noexcept;
    std::size_t ProcessorCount() const noexcept { return processor_count(); }
    std::size_t WorkerCount() const noexcept { return worker_count(); }
    std::size_t RunnableCount() const noexcept { return runnable_count(); }
    std::size_t GlobalRunnableCount() const noexcept {
        return global_runnable_count();
    }
    std::vector<ProcessorSnapshot> processors() const;
    std::vector<MachineSnapshot> machines() const;

    // Returns the task currently executing on this thread, if any.  These
    // values are observational and are never used for ownership.
    static std::shared_ptr<Task> current_task() noexcept;
    static Scheduler* current_scheduler() noexcept;
    static MId current_machine_id() noexcept;
    static PId current_processor_id() noexcept;
    static MId CurrentMachineId() noexcept { return current_machine_id(); }
    static PId CurrentProcessorId() noexcept { return current_processor_id(); }

private:
    // The opaque shared slot keeps a dynamic worker stable while machine
    // records are appended or retired by the scheduler.
    void worker_loop(std::shared_ptr<void> machine);

    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::scheduler

// The short aliases keep generated code concise while the nested namespace
// remains available for users that want an explicit module boundary.
namespace go2cpp {
using Scheduler = scheduler::Scheduler;
using SchedulerConfig = scheduler::SchedulerConfig;
using Task = scheduler::Task;
using GState = scheduler::GState;
using MState = scheduler::MState;
using PState = scheduler::PState;
using GId = scheduler::GId;
using MId = scheduler::MId;
using PId = scheduler::PId;
using TaskClassId = scheduler::TaskClassId;
using TaskOptions = scheduler::TaskOptions;
using Goroutine = scheduler::Task;
using Machine = scheduler::MachineSnapshot;
using Processor = scheduler::ProcessorSnapshot;
}  // namespace go2cpp
