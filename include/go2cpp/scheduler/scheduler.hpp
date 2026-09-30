#pragma once

#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/thread_policy.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp {
class Fiber;
enum class SuspendReason : std::uint8_t;
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
    // 普通 C++ 异常在 Fiber 边界被捕获后的失败终态。
    kFailed,

    // Readable aliases for callers that mirror the Go terminology.
    New = kNew,
    Runnable = kRunnable,
    Running = kRunning,
    Waiting = kWaiting,
    Dead = kDead,
    Cancelled = kCancelled,
    Failed = kFailed,
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
    // Zero selects one initial worker. With max_workers=0 the normalized
    // ceiling is 2*P when overcommit is enabled, while ordinary runnable
    // demand remains P-bounded; blocking regions may consume the extra slots.
    std::size_t min_workers = 0;
    std::chrono::milliseconds idle_worker_timeout{250};
    std::size_t fiber_stack_size = 0;
    std::size_t task_affinity_budget = 4;
    // Permit more M workers than P when a caller declares a native blocking
    // region.  The default keeps the worker pool elastic; set false when a
    // deployment requires a strict one-M-per-P ceiling.
    bool allow_worker_oversubscription = true;
    // Go sysmon 风格的阻塞监控。它只观察已声明的 BlockingRegion/Hook
    // 边界，不会从另一个线程强行切断任意 C++ 调用栈。
    bool enable_sysmon = true;
    std::chrono::milliseconds sysmon_interval{10};
    std::chrono::milliseconds long_syscall_threshold{50};
    // Linux 上可将 M 绑定到与 P 对应的 CPU。默认关闭，避免嵌入宿主已有
    // CPU 配额/容器亲和性策略；打开后超出 CPU 数量的 P 不执行绑定。
    bool pin_workers_to_cpu = false;
    // 性能敏感的部署可以关闭运行时累计计时；状态机和调度语义不受影响。
    // 默认开启，便于诊断和性能报告。
    bool collect_metrics = true;
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
    // sysmon 将长时间阻塞的 M 标记为已脱离 P；M 仍在原生调用中运行，
    // 但该 P 的 attached 计数不再包含它，替代 M 可以接管调度资源。
    bool processor_detached{false};
    GId blocking_task{0};
    std::uint64_t long_syscall_count{0};
};

// 调度器热点计数器。所有时间均为 steady_clock 纳秒，采用累计值，便于
// 调用方按两次快照的差值计算单次 Fiber 运行、切换和本地队列命中成本。
struct SchedulerMetrics {
    std::uint64_t task_runs{0};
    std::uint64_t task_completions{0};
    std::uint64_t fiber_resume_ns{0};
    std::uint64_t local_queue_pops{0};
    std::uint64_t steal_pops{0};
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
    // 普通 C++ 异常导致的失败是可观察终态，不会静默当作正常完成。
    bool failed() const noexcept;
    std::exception_ptr failure() const;
    void rethrow_failure() const;
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
    bool Failed() const noexcept { return failed(); }
    std::exception_ptr Failure() const { return failure(); }
    void RethrowFailure() const { rethrow_failure(); }
    bool Join() const { return wait(); }
    // Internal queue/run claims.  They are public so the replaceable worker
    // backend can enforce the same invariant without exposing data members.
    bool try_mark_queued() noexcept;
    // 当前 worker 已经释放 execution claim 后的本地回队快路径。通过
    // queued 原子位先占位，再校验状态；外部取消/唤醒仍使用同一位阻止
    // 重复入队，因此不需要为每次 yield 获取 transition_mutex。
    bool try_mark_queued_from_worker() noexcept;
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
    // IO poller fast path. The poller only publishes a wake token; a running
    // worker consumes it after the Fiber returns from park, so this transition
    // can use an atomic state CAS without taking the scheduler mutex.
    WakeAction wake_for_io() noexcept;
    ParkAction park_for_scheduler() noexcept;
    bool try_mark_running();
    // Internal state transitions used by Scheduler. mark_runnable() only
    // wakes a task that is already waiting; a running task records a pending
    // wake token and must use the current-G-only yield/park path below.
    bool mark_runnable();
    bool mark_yielded();
    // 当前 G 明确执行 yield 时使用无锁 CAS 快路径。唤醒/取消仍通过
    // 完整状态机处理；下一次成功出队会清理旧的 wake permit。
    bool mark_yielded_fast() noexcept;
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
    MId m_last_machine_id{0};
    PId m_last_processor_id{0};
    bool m_binding_published{false};
    mutable std::mutex m_failure_mutex;
    std::exception_ptr m_failure;
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
    // Marks a native call that may block its current M.  The region publishes
    // M::Blocking; sysmon 超过阈值后会把该 M 与 P 的计数解绑并驱动替代
    // M。它不从别的线程强行破坏调用栈，不能跨 Fiber yield/park，
    // 也不能跨迁移当前 G 的调用。
    class BlockingRegion final {
    public:
        explicit BlockingRegion(Scheduler* scheduler = nullptr) noexcept;
        ~BlockingRegion() noexcept;

        BlockingRegion(const BlockingRegion&) = delete;
        BlockingRegion& operator=(const BlockingRegion&) = delete;
        // A region is bound to the M that entered it. Moving it could move
        // destruction to another Fiber/thread and leave the original M
        // marked Blocking, so the scope is intentionally non-movable.
        BlockingRegion(BlockingRegion&&) = delete;
        BlockingRegion& operator=(BlockingRegion&&) = delete;

        bool active() const noexcept { return m_active; }
        bool Active() const noexcept { return active(); }

    private:
        Scheduler* m_scheduler{nullptr};
        bool m_active{false};
        MId m_machine_id{0};
    };

    explicit Scheduler(SchedulerConfig config = {});
    explicit Scheduler(std::size_t processor_count);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start();
    void shutdown();
    // 请求停止并在给定时间内等待所有 Fiber/worker 完成。返回 true 表示
    // 已完成并已回收 worker；返回 false 表示超时或从 worker 自身调用。
    // 超时不会释放仍在运行的 Fiber 栈，调用者可稍后再次调用本接口或
    // 使用无界 shutdown() 完成最终回收。
    bool shutdown_for(std::chrono::steady_clock::duration timeout);
    bool is_running() const noexcept;

    void Start() { start(); }
    void Shutdown() { shutdown(); }
    bool ShutdownFor(std::chrono::steady_clock::duration timeout) {
        return shutdown_for(timeout);
    }
    bool IsRunning() const noexcept { return is_running(); }

    std::shared_ptr<Task> spawn(Task::Function function);
    std::shared_ptr<Task> spawn(Task::Function function, TaskOptions options);
    std::shared_ptr<Task> go(Task::Function function) { return spawn(std::move(function)); }
    bool enqueue(const std::shared_ptr<Task>& task);
    // 新手入口：把已经构造好的任务加入当前调度器。任务不会被复制，
    // 调度器取得一个共享句柄；调用者可以继续使用原来的句柄等待或取消。
    std::shared_ptr<Task> add(const std::shared_ptr<Task>& task) {
        return enqueue(task) ? task : std::shared_ptr<Task>{};
    }
    // 直接加入一个回调，等价于 spawn，但名字更接近 Go 的使用方式。
    std::shared_ptr<Task> add(Task::Function function) {
        return spawn(std::move(function));
    }
    // 允许 go2cpp::fiber 等轻量包装器通过 task() 接口接入，而不让
    // scheduler.hpp 依赖上层包装器的定义。
    template <typename TaskLike,
              typename = std::enable_if_t<std::is_same_v<
                  decltype(std::declval<const TaskLike&>().task()),
                  std::shared_ptr<Task>>>>
    std::shared_ptr<Task> add(const TaskLike& task_like) {
        return add(task_like.task());
    }
    bool yield(const std::shared_ptr<Task>& task);
    bool park(const std::shared_ptr<Task>& task);
    // IOManager 使用此入口记录 Fiber 正在等待 readiness；它与普通
    // park 共用同一 Task 状态和 wake 竞态，但诊断帧会保留 Io 原因。
    bool park_io(const std::shared_ptr<Task>& task);
    bool yield_current();
    bool park_current();
    // Returns true when a waiting G was enqueued immediately. A false result
    // may still represent an accepted pending wake for a currently running G;
    // that token is consumed by its next park.
    bool wake(const std::shared_ptr<Task>& task);
    bool wake_io(const std::shared_ptr<Task>& task);
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
    std::shared_ptr<Task> Add(const std::shared_ptr<Task>& task) {
        return add(task);
    }
    std::shared_ptr<Task> Add(Task::Function function) {
        return add(std::move(function));
    }
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
    // 返回 sysmon 线程是否正在运行，便于部署自检和测试。
    bool sysmon_running() const noexcept;
    // 每次监控周期都会递增，即使调度器互斥量正被高负载路径占用。
    // 该计数只用于活性观测，不参与调度决策。
    std::uint64_t sysmon_pass_count() const noexcept;
    SchedulerMetrics metrics() const noexcept;
    bool SysmonRunning() const noexcept { return sysmon_running(); }
    std::uint64_t SysmonPassCount() const noexcept {
        return sysmon_pass_count();
    }
    SchedulerMetrics Metrics() const noexcept { return metrics(); }

    // Returns the task currently executing on this thread, if any.  These
    // values are observational and are never used for ownership.
    static std::shared_ptr<Task> current_task() noexcept;
    static Scheduler* current_scheduler() noexcept;
    static MId current_machine_id() noexcept;
    static PId current_processor_id() noexcept;
    // Explicit hooks for interposers and embedders that call a native blocking
    // API from a managed G. BlockingRegion is preferred for RAII use.
    static bool enter_blocking() noexcept;
    static void leave_blocking() noexcept;
    static MId CurrentMachineId() noexcept { return current_machine_id(); }
    static PId CurrentProcessorId() noexcept { return current_processor_id(); }

private:
    // 仅供拥有该 G 执行权的 worker 在 resume 返回后调用。
    bool requeue_from_worker(const std::shared_ptr<Task>& task);
    static void leave_blocking_for(Scheduler* scheduler,
                                   MId machine_id) noexcept;
    bool park_with_reason(const std::shared_ptr<Task>& task,
                          go2cpp::SuspendReason reason);

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
using BlockingRegion = scheduler::Scheduler::BlockingRegion;
using Goroutine = scheduler::Task;
using Machine = scheduler::MachineSnapshot;
using Processor = scheduler::ProcessorSnapshot;
}  // namespace go2cpp
