#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

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
    kParked,
    kStopping,
    kDead,

    Idle = kIdle,
    Running = kRunning,
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
};

class Task {
public:
    using Function = std::function<void()>;

    explicit Task(Function function);
    ~Task();

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    GId id() const noexcept;
    GState state() const noexcept;
    bool queued() const noexcept;
    GId Id() const noexcept { return id(); }
    GState State() const noexcept { return state(); }
    bool Queued() const noexcept { return queued(); }
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
    void run();

private:
    friend class Scheduler;
    GId m_id;
    Function m_function;
    std::atomic<GState> m_state;
    std::atomic<bool> m_queued{false};
    std::atomic<bool> m_deferred_enqueue{false};
    std::atomic<bool> m_wake_pending{false};
    std::atomic<bool> m_registered{false};
    std::atomic<bool> m_execution_claim{false};
    std::shared_ptr<const void> m_owner_anchor;
    mutable std::mutex m_transition_mutex;
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
    std::shared_ptr<Task> go(Task::Function function) { return spawn(std::move(function)); }
    bool enqueue(const std::shared_ptr<Task>& task);
    bool yield(const std::shared_ptr<Task>& task);
    bool park(const std::shared_ptr<Task>& task);
    // Returns true when a waiting G was enqueued immediately. A false result
    // may still represent an accepted pending wake for a currently running G;
    // that token is consumed by its next park.
    bool wake(const std::shared_ptr<Task>& task);

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

private:
    void worker_loop(std::size_t machine_index);

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
using Goroutine = scheduler::Task;
using Machine = scheduler::MachineSnapshot;
using Processor = scheduler::ProcessorSnapshot;
}  // namespace go2cpp
