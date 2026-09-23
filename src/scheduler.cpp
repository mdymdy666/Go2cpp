#include "go2cpp/scheduler/scheduler.hpp"

#include "go2cpp/fiber.hpp"
#include "go2cpp/thread_policy.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace go2cpp::scheduler {
namespace {

std::atomic<GId> s_next_g_id{1};
std::atomic<MId> s_next_m_id{1};

thread_local Scheduler* t_scheduler = nullptr;
thread_local std::shared_ptr<Task> t_task;
thread_local MId t_machine_id = 0;
thread_local PId t_processor_id = 0;
// BlockingRegion is deliberately non-nestable: one M has one state
// transition, and rejecting nested scopes avoids double accounting. The
// machine id is retained so a misuse that lets a Fiber migrate before its
// region is destroyed can still clear the original M's state.
thread_local MId t_blocking_machine_id = 0;

std::size_t default_processor_count() noexcept {
    const auto count = std::thread::hardware_concurrency();
    return count == 0 ? 1U : static_cast<std::size_t>(count);
}

std::int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t milliseconds_to_ns(
    std::chrono::milliseconds duration) noexcept {
    constexpr auto max_value = std::numeric_limits<std::int64_t>::max();
    const auto milliseconds = duration.count();
    if (milliseconds <= 0) {
        return 0;
    }
    constexpr auto scale = static_cast<std::int64_t>(1000000);
    if (milliseconds > max_value / scale) {
        return max_value;
    }
    return milliseconds * scale;
}

}  // namespace

// Tasks retain only a weak handle. This short gate excludes Scheduler
// destruction from an in-flight external Task::Cancel without retaining the
// Scheduler itself or holding an admission lock while entering the gate.
struct TaskCancellationGate {
    explicit TaskCancellationGate(Scheduler* scheduler)
        : m_scheduler(scheduler) {}

    std::mutex m_mutex;
    Scheduler* m_scheduler;
};

Task::Task(Function function, TaskOptions options)
    : m_id(s_next_g_id.fetch_add(1, std::memory_order_relaxed)),
      m_function(std::move(function)),
      m_options(options),
      m_state(GState::kNew) {}

Task::~Task() = default;

Task::Function Task::release_callable() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    return std::move(m_function);
}

GId Task::id() const noexcept {
    return m_id;
}

GState Task::state() const noexcept {
    return m_state.load(std::memory_order_acquire);
}

bool Task::queued() const noexcept {
    return m_queued.load(std::memory_order_acquire);
}

bool Task::started() const noexcept {
    return m_started.load(std::memory_order_acquire);
}

bool Task::cancellation_requested() const noexcept {
    return m_cancel_requested.load(std::memory_order_acquire);
}

TaskClassId Task::task_class() const noexcept {
    return m_options.task_class;
}

bool Task::failed() const noexcept {
    return state() == GState::kFailed;
}

std::exception_ptr Task::failure() const {
    std::lock_guard<std::mutex> lock(m_failure_mutex);
    return m_failure;
}

void Task::rethrow_failure() const {
    const auto error = failure();
    if (error) {
        std::rethrow_exception(error);
    }
}

bool Task::terminal() const noexcept {
    const auto current = state();
    return current == GState::kDead || current == GState::kCancelled ||
           current == GState::kFailed;
}

bool Task::wait() const {
    if (Scheduler::current_task().get() == this) {
        return false;
    }
    std::unique_lock<std::mutex> lock(m_completion_mutex);
    return m_completion_condition.wait(lock, [this] { return terminal(); });
}

bool Task::wait_for(std::chrono::steady_clock::duration timeout) const {
    if (Scheduler::current_task().get() == this) {
        return false;
    }
    std::unique_lock<std::mutex> lock(m_completion_mutex);
    return m_completion_condition.wait_for(
        lock, timeout, [this] { return terminal(); });
}

void Task::notify_terminal() noexcept {
    if (m_completion_notified.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // Synchronize with a waiter's predicate -> cv.wait transition. Merely
    // notifying an atomic terminal flag can lose the notification between
    // those two steps while the waiter still owns completion_mutex.
    {
        std::lock_guard<std::mutex> lock(m_completion_mutex);
    }
    m_completion_condition.notify_all();
}

bool Task::try_mark_queued() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kRunnable ||
        m_execution_claim.load(std::memory_order_relaxed) ||
        m_queued.load(std::memory_order_relaxed)) {
        return false;
    }
    m_queued.store(true, std::memory_order_release);
    return true;
}

void Task::clear_queued() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    m_queued.store(false, std::memory_order_release);
}

void Task::defer_enqueue() noexcept {
    m_deferred_enqueue.store(true, std::memory_order_release);
}

bool Task::consume_deferred_enqueue() noexcept {
    return m_deferred_enqueue.exchange(false, std::memory_order_acq_rel);
}

void Task::request_wake() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    m_wake_pending.store(true, std::memory_order_release);
}

bool Task::consume_wake() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    return m_wake_pending.exchange(false, std::memory_order_acq_rel);
}

bool Task::try_register() noexcept {
    bool expected = false;
    return m_registered.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire);
}

bool Task::bind_owner(const std::shared_ptr<const void>& owner) noexcept {
    if (!owner) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (!m_owner_anchor) {
        m_owner_anchor = owner;
        return true;
    }
    return m_owner_anchor.get() == owner.get();
}

bool Task::owned_by(const std::shared_ptr<const void>& owner) const noexcept {
    if (!owner) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    return m_owner_anchor && m_owner_anchor.get() == owner.get();
}

void Task::bind_cancellation_gate(
    const std::shared_ptr<TaskCancellationGate>& gate) noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    m_cancellation_gate = gate;
}

bool Task::promote_new() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kNew) {
        return false;
    }
    m_state.store(GState::kRunnable, std::memory_order_release);
    return true;
}

WakeAction Task::wake_for_scheduler() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    const auto state = m_state.load(std::memory_order_relaxed);
    if (state == GState::kWaiting) {
        m_wake_pending.store(false, std::memory_order_relaxed);
        m_state.store(GState::kRunnable, std::memory_order_release);
        if (m_execution_claim.load(std::memory_order_relaxed)) {
            // park() changed the logical state before the cooperative callable
            // returned. Let the owning worker publish the next run only after
            // it releases the execution claim.
            m_deferred_enqueue.store(true, std::memory_order_release);
            return WakeAction::kPending;
        }
        return WakeAction::kEnqueue;
    }
    if (state == GState::kRunning) {
        m_wake_pending.store(true, std::memory_order_release);
        return WakeAction::kPending;
    }
    return WakeAction::kRejected;
}

ParkAction Task::park_for_scheduler() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kRunning) {
        return ParkAction::kRejected;
    }
    if (m_wake_pending.exchange(false, std::memory_order_acq_rel)) {
        // The wake raced before the Fiber committed its suspension. Consume
        // the permit and keep running from the current call frame.
        return ParkAction::kRequeued;
    }
    m_state.store(GState::kWaiting, std::memory_order_release);
    return ParkAction::kParked;
}

bool Task::try_mark_running() {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (!m_queued.load(std::memory_order_relaxed) ||
        m_execution_claim.load(std::memory_order_relaxed) ||
        m_state.load(std::memory_order_relaxed) != GState::kRunnable) {
        return false;
    }
    // Dequeue reservation and the runnable -> running transition are one
    // indivisible Task operation. An external enqueue cannot republish the G
    // between these updates.
    m_queued.store(false, std::memory_order_relaxed);
    m_execution_claim.store(true, std::memory_order_relaxed);
    m_state.store(GState::kRunning, std::memory_order_release);
    return true;
}

bool Task::mark_runnable() {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kWaiting) {
        return false;
    }
    m_state.store(GState::kRunnable, std::memory_order_release);
    m_wake_pending.store(false, std::memory_order_relaxed);
    return true;
}

bool Task::mark_yielded() {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kRunning) {
        return false;
    }
    // A wake received while the G was running has been superseded by the
    // explicit yield: the G is already being requeued and must not consume
    // that old token on a later park.
    m_state.store(GState::kRunnable, std::memory_order_release);
    m_wake_pending.store(false, std::memory_order_release);
    return true;
}

bool Task::mark_waiting() {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kRunning) {
        return false;
    }
    m_state.store(GState::kWaiting, std::memory_order_release);
    return true;
}

bool Task::cancel() {
    std::shared_ptr<TaskCancellationGate> gate;
    {
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        gate = m_cancellation_gate.lock();
    }
    const auto self = weak_from_this().lock();
    if (gate && self) {
        std::lock_guard<std::mutex> lock(gate->m_mutex);
        if (gate->m_scheduler) {
            return gate->m_scheduler->cancel(self);
        }
    }
    return request_cancel();
}

bool Task::request_cancel(bool notify) noexcept {
    bool became_terminal = false;
    {
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        const auto state = m_state.load(std::memory_order_relaxed);
        if (state == GState::kDead || state == GState::kCancelled ||
            state == GState::kFailed) {
            return false;
        }
        m_cancel_requested.store(true, std::memory_order_release);
        if (m_fiber) {
            m_fiber->RequestCancellation();
        }
        // A started Fiber must resume to its trampoline so stack locals and
        // stack cleanup finishes normally. Only an unstarted G is terminal immediately.
        if (!m_started.load(std::memory_order_relaxed) &&
            !m_execution_claim.load(std::memory_order_relaxed)) {
            m_state.store(GState::kCancelled, std::memory_order_release);
            m_queued.store(false, std::memory_order_release);
            became_terminal = true;
        }
        m_wake_pending.store(false, std::memory_order_release);
        if (became_terminal) {
            m_deferred_enqueue.store(false, std::memory_order_release);
        }
    }
    if (became_terminal && notify) {
        notify_terminal();
    }
    return true;
}

bool Task::cancel_if_runnable_unqueued() noexcept {
    {
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        if (m_state.load(std::memory_order_relaxed) != GState::kRunnable ||
            m_queued.load(std::memory_order_relaxed) ||
            m_execution_claim.load(std::memory_order_relaxed)) {
            return false;
        }
        if (m_started.load(std::memory_order_relaxed)) {
            // This helper is only an admission-failure rescue for an
            // unstarted G. A started Fiber must remain runnable and be
            // requeued/cancelled through Scheduler::cancel instead.
            return false;
        }
        m_cancel_requested.store(true, std::memory_order_release);
        m_state.store(GState::kCancelled, std::memory_order_release);
        m_wake_pending.store(false, std::memory_order_relaxed);
        m_deferred_enqueue.store(false, std::memory_order_relaxed);
    }
    notify_terminal();
    return true;
}

void Task::run() {
    if (!m_execution_claim.load(std::memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (!m_run_claim.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
    struct RunClaimGuard final {
        std::atomic<bool>& claim;
        ~RunClaimGuard() { claim.store(false, std::memory_order_release); }
    } run_claim_guard{m_run_claim};

    Fiber* fiber = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        fiber = m_fiber.get();
    }
    if (fiber == nullptr) {
        try {
            const auto stack_size = m_options.stack_size == 0
                                        ? Fiber::DefaultStackSize()
                                        : m_options.stack_size;
            auto created_fiber = std::make_unique<Fiber>(
                [this] {
                    // Move the callable onto the G stack. Its captures are
                    // released without any scheduler lock held when the body
                    // returns or the Fiber is deliberately unwound.
                    auto body = std::move(m_function);
                    if (body) {
                        // Fiber::entry catches ordinary C++ exceptions and
                        // exposes them through Fiber::failure(). User code must
                        // handle ordinary C++ exceptions explicitly.
                        body();
                    }
                },
                stack_size);
            {
                std::lock_guard<std::mutex> lock(m_transition_mutex);
                if (!m_fiber) {
                    m_fiber = std::move(created_fiber);
                    m_started.store(true, std::memory_order_release);
                }
                fiber = m_fiber.get();
            }
        } catch (...) {
            const auto error = std::current_exception();
            {
                std::lock_guard<std::mutex> lock(m_failure_mutex);
                m_failure = error;
            }
            {
                std::lock_guard<std::mutex> lock(m_transition_mutex);
                m_state.store(GState::kFailed, std::memory_order_release);
                m_execution_claim.store(false, std::memory_order_release);
            }
            auto abandoned_function = release_callable();
            notify_terminal();
            return;
        }
    }

    // Cancellation may have raced with Fiber construction before the new
    // pointer was published. The task flag is authoritative; mirror it into
    // the Fiber before entering its trampoline.
    if (m_cancel_requested.load(std::memory_order_acquire)) {
        fiber->RequestCancellation();
    }
    // 发布 G/M/P 绑定。Fiber 首次进入时会继承该绑定，嵌套 Fiber 之后
    // 只能沿同一逻辑 G 的父链恢复，即使 G 迁移到另一个 M 也不会丢失
    // 调度器归属。
    if (Scheduler* const scheduler = Scheduler::current_scheduler()) {
        fiber->bind_execution(FiberExecutionBinding{
            reinterpret_cast<std::uintptr_t>(scheduler), id(), t_machine_id,
            t_processor_id, true});
    }
    // Only this run claim can move/destroy m_fiber, and it does so after
    // resume() returns while holding m_transition_mutex. A concurrent cancel
    // can therefore safely request cancellation on this stable pointer.
    const bool resumed = fiber->resume();
    const FiberState fiber_state = fiber->state();
    const auto fiber_failure = fiber_state == FiberState::Failed
                                   ? fiber->failure()
                                   : std::exception_ptr{};
    bool became_terminal = false;
    Function completed_function;
    std::unique_ptr<Fiber> completed_fiber;
    {
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        if (!resumed || fiber_state == FiberState::Completed ||
            fiber_state == FiberState::Failed) {
            m_deferred_enqueue.store(false, std::memory_order_relaxed);
            m_wake_pending.store(false, std::memory_order_relaxed);
            if (fiber_failure) {
                std::lock_guard<std::mutex> failure_lock(m_failure_mutex);
                m_failure = fiber_failure;
            }
            const auto terminal_state = fiber_state == FiberState::Failed
                                            ? GState::kFailed
                                            : (m_cancel_requested.load(
                                                   std::memory_order_relaxed)
                                                   ? GState::kCancelled
                                                   : GState::kDead);
            m_state.store(terminal_state, std::memory_order_release);
            // A cancelled-before-first-resume Fiber can still retain the
            // callable. Move it out while updating state, then destroy it
            // after the transition lock is released.
            completed_function = std::move(m_function);
            became_terminal = true;
            // The trampoline has returned, so no C++ stack is still active on
            // this Fiber. Release its protected stack even when a caller
            // retains the public Task handle after completion; retaining a
            // Task must not retain an already-finished G stack.
            completed_fiber = std::move(m_fiber);
        } else if (fiber_state == FiberState::Suspended &&
                   m_state.load(std::memory_order_relaxed) ==
                       GState::kRunning) {
            // Direct Fiber::Suspend calls are supported for advanced users.
            // Scheduler helpers set the state before switching, while this
            // fallback maps a raw suspension to the matching queue state.
            // A cancellation/wake can race with the context switch before
            // the Fiber has published Suspended. In that window the notifier
            // records a pending permit while the state is still Running;
            // never overwrite that permit with Waiting or shutdown can strand
            // the G forever.
            const bool pending_wake =
                m_wake_pending.load(std::memory_order_relaxed);
            const bool cancellation_requested =
                m_cancel_requested.load(std::memory_order_relaxed);
            if (fiber->suspend_reason() == SuspendReason::Yield ||
                pending_wake || cancellation_requested) {
                m_state.store(GState::kRunnable, std::memory_order_release);
                m_deferred_enqueue.store(true, std::memory_order_release);
                m_wake_pending.store(false, std::memory_order_release);
            } else {
                m_state.store(GState::kWaiting, std::memory_order_release);
            }
        }
        m_execution_claim.store(false, std::memory_order_release);
    }
    if (became_terminal) {
        notify_terminal();
        completed_fiber.reset();
    }
}

class Scheduler::Impl {
public:
    struct Processor {
        explicit Processor(PId processor_id) : id(processor_id) {}

        PId id;
        mutable std::mutex mutex;
        std::deque<std::shared_ptr<Task>> queue;
        std::atomic<PState> state{PState::kIdle};
        std::atomic<std::size_t> active_machines{0};
        std::atomic<std::size_t> running_machines{0};
    };

    struct Machine {
        explicit Machine(MId machine_id, PId processor_id)
            : id(machine_id), processor(processor_id) {}

        MId id;
        PId processor;
        std::thread thread;
        std::atomic<MState> state{MState::kIdle};
        // These fields are owned by the worker represented by this object.
        // Snapshot readers access them only while holding Impl::mutex.
        TaskClassId last_task_class{0};
        std::size_t affinity_budget{0};
        std::size_t affinity_hits{0};
        std::size_t affinity_misses{0};
        // 这些原子字段由 worker、BlockingRegion 和 sysmon 共同观察；
        // P 计数的增减仍在 Impl::mutex 下完成。
        std::atomic<bool> processor_detached{false};
        std::atomic<std::int64_t> blocking_since_ns{0};
        std::atomic<GId> blocking_task{0};
        std::atomic<std::uint64_t> long_syscall_count{0};
    };

    explicit Impl(SchedulerConfig requested, Scheduler* scheduler)
        : config(normalize(requested)),
          owner_token(std::make_shared<const std::uint8_t>(0)),
          cancellation_gate(std::make_shared<TaskCancellationGate>(scheduler)) {
        for (std::size_t i = 0; i < config.processor_count; ++i) {
            processors.emplace_back(static_cast<PId>(i));
        }
    }

    ~Impl() { stop_sysmon(true); }

    static SchedulerConfig normalize(SchedulerConfig requested) {
        if (requested.processor_count == 0) {
            requested.processor_count = default_processor_count();
        }
        requested.processor_count = std::max<std::size_t>(1,
                                                           requested.processor_count);
        if (requested.max_workers == 0) {
            // Keep ordinary runnable demand P-bounded, but reserve a bounded
            // replacement ceiling for explicitly declared native blocking M's.
            // This lets the default scheduler make progress without requiring
            // every caller to guess a max-worker value; callers can still set
            // an exact max or disable overcommit below.
            requested.max_workers =
                requested.allow_worker_oversubscription &&
                        requested.processor_count <=
                            std::numeric_limits<std::size_t>::max() / 2
                    ? requested.processor_count * 2
                    : std::numeric_limits<std::size_t>::max();
        }
        requested.max_workers = std::max<std::size_t>(1, requested.max_workers);
        // A strict one-M-per-P ceiling is still available for deployments
        // that set allow_worker_oversubscription=false.  The default permits
        // declared native blocking regions to grow replacement M workers;
        // this is an intentional overcommit, not a claim of asynchronous
        // preemption for arbitrary native calls.
        if (!requested.allow_worker_oversubscription) {
            requested.max_workers =
                std::min(requested.max_workers, requested.processor_count);
        }
        if (requested.min_workers == 0) {
            requested.min_workers = 1;
        }
        requested.min_workers = std::max<std::size_t>(1, requested.min_workers);
        requested.min_workers = std::min(requested.min_workers,
                                         requested.max_workers);
        if (requested.idle_wait <= std::chrono::milliseconds::zero()) {
            requested.idle_wait = std::chrono::milliseconds(1);
        }
        if (requested.idle_worker_timeout <= std::chrono::milliseconds::zero()) {
            requested.idle_worker_timeout = std::chrono::milliseconds(1);
        }
        if (requested.sysmon_interval <= std::chrono::milliseconds::zero()) {
            requested.sysmon_interval = std::chrono::milliseconds(1);
        }
        if (requested.long_syscall_threshold <=
            std::chrono::milliseconds::zero()) {
            requested.long_syscall_threshold = requested.sysmon_interval;
        }
        return requested;
    }

    // Create one worker while mutex is held. The thread starts only after its
    // shared Machine object has been published, so snapshots and shutdown can
    // safely retain the same object even when later workers are appended.
    bool spawn_worker_locked(Scheduler* scheduler) {
        if (!scheduler || stopping.load(std::memory_order_acquire) ||
            machines.size() >= std::numeric_limits<std::size_t>::max() ||
            active_workers.load(std::memory_order_relaxed) >=
                config.max_workers) {
            return false;
        }

        PId processor_id = 0;
        std::size_t least = std::numeric_limits<std::size_t>::max();
        for (const auto& processor : processors) {
            const auto active =
                processor.active_machines.load(std::memory_order_relaxed);
            if (active < least) {
                least = active;
                processor_id = processor.id;
            }
        }

        auto machine = std::make_shared<Machine>(
            s_next_m_id.fetch_add(1, std::memory_order_relaxed), processor_id);
        machines.emplace_back(machine);
        active_workers.fetch_add(1, std::memory_order_relaxed);
        processors[processor_id].active_machines.fetch_add(
            1, std::memory_order_relaxed);
        try {
            machine->thread = std::thread([scheduler, machine] {
                scheduler->worker_loop(std::static_pointer_cast<void>(machine));
            });
        } catch (...) {
            processors[processor_id].active_machines.fetch_sub(
                1, std::memory_order_relaxed);
            active_workers.fetch_sub(1, std::memory_order_relaxed);
            machines.pop_back();
            throw;
        }
        return true;
    }

    void reap_dead_workers() {
        std::vector<std::thread> retired;
        const auto self = std::this_thread::get_id();
        // shutdown() owns join_mutex while draining Gs. A worker must not
        // block here during requeue, or shutdown would be waiting for a G
        // whose M is waiting for shutdown's join lock.
        std::unique_lock<std::mutex> join_lock(join_mutex, std::try_to_lock);
        if (!join_lock.owns_lock()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            // Reserve before moving any joinable std::thread. If allocation
            // fails, maybe_grow() can leave the dead records for a later
            // retry; after this point thread moves are noexcept and cannot
            // make vector destruction terminate the process.
            retired.reserve(machines.size());
            for (auto it = machines.begin(); it != machines.end();) {
                const auto& machine = *it;
                if (!machine) {
                    it = machines.erase(it);
                } else if (machine->state.load(std::memory_order_acquire) ==
                               MState::kDead &&
                           machine->thread.get_id() != self) {
                    if (machine->thread.joinable()) {
                        retired.emplace_back(std::move(machine->thread));
                    }
                    it = machines.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& thread : retired) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        // Keep shutdown/reaper serialization through the actual join. The
        // retired worker records are already erased, but releasing the gate
        // earlier would let an owner observe a half-finished maintenance pass.
        join_lock.unlock();
    }

    // 统计当前仍处于 sysmon 逻辑解绑状态的 M。调用方必须持有 mutex；
    // 这样 sysmon 标记后、原 G 返回前的短竞态不会使用过期计数扩容。
    std::size_t detached_machine_count_locked() const noexcept {
        std::size_t count = 0;
        for (const auto& machine : machines) {
            if (machine &&
                machine->processor_detached.load(std::memory_order_acquire)) {
                ++count;
            }
        }
        return count;
    }

    // 普通入队只按 runnable demand 扩容；每个仍 detached 的长阻塞 M
    // 预留一个替代槽，即便当前队列暂时为空，也不会让 P 长时间失去
    // 可运行的 M。
    void maybe_grow(Scheduler* scheduler) {
        if (!scheduler) {
            return;
        }
        if (!started.load(std::memory_order_acquire) ||
            draining.load(std::memory_order_acquire) ||
            stopping.load(std::memory_order_acquire)) {
            return;
        }
        try {
            reap_dead_workers();
        } catch (...) {
            // Failure to allocate a temporary join list cannot invalidate
            // accepted Gs. The dead records remain safe for shutdown/retry.
        }
        std::unique_lock<std::mutex> lock(mutex);
        if (!started.load(std::memory_order_acquire) ||
            draining.load(std::memory_order_acquire) ||
            stopping.load(std::memory_order_acquire)) {
            return;
        }
        const auto queued = runnable.load(std::memory_order_acquire);
        const auto busy = running_workers.load(std::memory_order_acquire);
        const auto active = active_workers.load(std::memory_order_relaxed);
        const auto blocking = blocking_workers.load(std::memory_order_acquire);
        const auto detached = detached_machine_count_locked();
        const auto effective_blocking = std::max(blocking, detached);
        // Normal runnable bursts stay P-bounded.  Each explicitly declared
        // native blocking M contributes one replacement slot, up to the
        // configured max; this prevents a large queue from creating dozens
        // of threads merely because max_workers is generous.
        const auto extra_ceiling =
            effective_blocking > std::numeric_limits<std::size_t>::max() -
                              config.processor_count
                ? std::numeric_limits<std::size_t>::max()
                : config.processor_count + effective_blocking;
        const auto worker_ceiling = std::min(config.max_workers, extra_ceiling);
        const auto demand =
            queued > std::numeric_limits<std::size_t>::max() - busy
                ? std::numeric_limits<std::size_t>::max()
                : queued + busy;
        auto desired = std::min(worker_ceiling,
                                std::max(config.min_workers, demand));
        if (detached != 0) {
            // 替代槽只需要补足当前的最小 worker 底线；普通 runnable
            // 需求仍由上面的 P 有界 ceiling 控制。不能因为机器有很多
            // 个 P，就为一个长 syscall 无条件启动 P+1 个 M。
            const auto handoff_target = std::min(
                worker_ceiling,
                detached > std::numeric_limits<std::size_t>::max() -
                                  config.min_workers
                    ? std::numeric_limits<std::size_t>::max()
                    : config.min_workers + detached);
            desired = std::max(desired, handoff_target);
        }
        if (desired <= active) {
            return;
        }
        const auto to_add = desired - active;
        for (std::size_t i = 0; i < to_add; ++i) {
            try {
                if (!spawn_worker_locked(scheduler)) {
                    break;
                }
            } catch (...) {
                // Thread-resource exhaustion degrades parallelism, not the
                // accepted Task lifetime. Existing M's retain all queue work.
                break;
            }
        }
        lock.unlock();
        condition.notify_all();
    }

    // sysmon 只做可逆的资源记账：它不会从别的线程跳转或终止正在
    // syscall 中的 C++ 栈。达到阈值后，M 继续执行原生调用，但从 P 的
    // attached 计数中移除，并按阻塞数申请替代 M。
    void sysmon_pass(Scheduler* scheduler) noexcept {
        if (!scheduler || !config.enable_sysmon) {
            return;
        }
        // 心跳先于调度器锁更新。高负载时即使本轮无法取得锁，
        // monitor 线程也不会永久卡在业务队列上，调用者仍能观察到它在线。
        const auto now = steady_now_ns();
        sysmon_pass_count.fetch_add(1, std::memory_order_relaxed);
        const auto threshold = milliseconds_to_ns(config.long_syscall_threshold);
        std::size_t detached_count = 0;
        {
            std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                // 不要把一次锁竞争当成“本轮扫描完成”。worker 在高负载
                // 下可能连续持有主锁；保留重试标记让 monitor 缩短下一
                // 个周期，而不是静默丢失长 syscall 的解绑机会。
                sysmon_retry_pending.store(true, std::memory_order_release);
                return;
            }
            sysmon_retry_pending.store(false, std::memory_order_release);
            for (const auto& machine : machines) {
                if (!machine ||
                    machine->state.load(std::memory_order_acquire) !=
                        MState::kBlocking) {
                    continue;
                }
                const auto since = machine->blocking_since_ns.load(
                    std::memory_order_acquire);
                if (since <= 0 || now < since || now - since < threshold ||
                    machine->processor_detached.load(std::memory_order_acquire)) {
                    continue;
                }
                machine->processor_detached.store(true,
                                                   std::memory_order_release);
                machine->long_syscall_count.fetch_add(
                    1, std::memory_order_relaxed);
                const auto processor_id = machine->processor % processors.size();
                auto& processor = processors[processor_id];
                if (processor.active_machines.load(
                        std::memory_order_relaxed) != 0) {
                    processor.active_machines.fetch_sub(
                        1, std::memory_order_acq_rel);
                }
                if (processor.active_machines.load(
                        std::memory_order_relaxed) == 0 &&
                    !stopping.load(std::memory_order_acquire)) {
                    processor.state.store(PState::kIdle,
                                          std::memory_order_release);
                }
                ++detached_count;
            }
        }
        if (detached_count != 0) {
            maybe_grow(scheduler);
            condition.notify_all();
        }
    }

    void sysmon_loop(Scheduler* scheduler) noexcept {
        while (!sysmon_stop.load(std::memory_order_acquire)) {
            // sysmon 的节拍等待不能争用调度器主锁。worker 在高负载下可能
            // 持有主锁进行队列/生命周期处理；监控线程只应等待自己的停止
            // 条件，然后在扫描阶段用 try_to_lock 做一次有界尝试。
            const bool retry =
                sysmon_retry_pending.exchange(false, std::memory_order_acq_rel);
            // 锁竞争后的重试保持有界退避，避免 monitor 在主锁长期繁忙
            // 时忙等，同时确保一次失败扫描不会被完整周期掩盖。
            const auto wait_interval = retry
                                           ? std::min(
                                                 config.sysmon_interval,
                                                 std::chrono::milliseconds(1))
                                           : config.sysmon_interval;
            std::unique_lock<std::mutex> lock(sysmon_wait_mutex);
            sysmon_wait_condition.wait_for(lock, wait_interval, [this] {
                return sysmon_stop.load(std::memory_order_acquire) ||
                       stopping.load(std::memory_order_acquire) ||
                       sysmon_retry_pending.load(std::memory_order_acquire);
            });
            const bool stop = sysmon_stop.load(std::memory_order_acquire) ||
                              stopping.load(std::memory_order_acquire);
            lock.unlock();
            if (stop) {
                break;
            }
            sysmon_pass(scheduler);
        }
        sysmon_active.store(false, std::memory_order_release);
    }

    void stop_sysmon(bool join) noexcept {
        sysmon_stop.store(true, std::memory_order_release);
        sysmon_wait_condition.notify_all();
        if (join && sysmon_thread.joinable() &&
            sysmon_thread.get_id() != std::this_thread::get_id()) {
            sysmon_thread.join();
        }
    }

    void collect_terminal_locked(
        std::vector<std::shared_ptr<Task>>& deferred_destruction) noexcept {
        // The registry strongly owns every non-terminal G, including a Fiber
        // parked outside a run queue. This prevents destruction of a suspended
        // stack. Move terminal entries to caller-owned storage so arbitrary
        // user captures are never destroyed while the scheduler lock is held.
        try {
            deferred_destruction.reserve(deferred_destruction.size() +
                                         task_registry.size());
        } catch (...) {
            // Retaining a terminal entry is safe. A later worker/shutdown pass
            // can reclaim it when allocation succeeds.
            return;
        }
        for (auto it = task_registry.begin(); it != task_registry.end();) {
            if (!*it || (*it)->terminal()) {
                if (*it) {
                    deferred_destruction.emplace_back(std::move(*it));
                }
                it = task_registry.erase(it);
            } else {
                ++it;
            }
        }
    }

    void emergency_enqueue_locked(const std::shared_ptr<Task>& task) noexcept {
        if (!task) {
            return;
        }
        task->m_emergency_next.reset();
        if (emergency_tail) {
            emergency_tail->m_emergency_next = task;
        } else {
            emergency_head = task;
        }
        emergency_tail = task;
    }

    std::shared_ptr<Task> emergency_pop_locked() noexcept {
        if (!emergency_head) {
            return {};
        }
        auto task = std::move(emergency_head);
        emergency_head = std::move(task->m_emergency_next);
        if (!emergency_head) {
            emergency_tail.reset();
        }
        return task;
    }

    void begin_shutdown_locked(
        std::vector<std::shared_ptr<Task>>& deferred_destruction,
        std::vector<std::shared_ptr<Task>>& completion_notifications) {
        // Unstarted Gs have no suspended stack and can be cancelled in place.
        // Started Gs must remain runnable so their Fiber can return through its
        // trampoline and execute stack-local destructors.
        for (auto& processor : processors) {
            std::lock_guard<std::mutex> queue_lock(processor.mutex);
            std::deque<std::shared_ptr<Task>> resumable;
            while (!processor.queue.empty()) {
                auto task = std::move(processor.queue.front());
                processor.queue.pop_front();
                if (task->started()) {
                    task->request_cancel(false);
                    resumable.emplace_back(std::move(task));
                } else {
                    task->clear_queued();
                    task->request_cancel(false);
                    completion_notifications.emplace_back(task);
                    runnable.fetch_sub(1, std::memory_order_relaxed);
                    deferred_destruction.emplace_back(std::move(task));
                }
            }
            processor.queue.swap(resumable);
        }

        std::deque<std::shared_ptr<Task>> resumable_global;
        while (!global_queue.empty()) {
            auto task = std::move(global_queue.front());
            global_queue.pop_front();
            if (task->started()) {
                task->request_cancel(false);
                resumable_global.emplace_back(std::move(task));
            } else {
                task->clear_queued();
                task->request_cancel(false);
                completion_notifications.emplace_back(task);
                runnable.fetch_sub(1, std::memory_order_relaxed);
                deferred_destruction.emplace_back(std::move(task));
            }
        }
        global_queue.swap(resumable_global);

        // The rescue queue contains only previously started Gs when a normal
        // queue allocation failed. Keep them runnable and request cooperative
        // cancellation; no new node allocation is required here.
        for (auto task = emergency_head; task; task = task->m_emergency_next) {
            task->request_cancel(false);
        }

        // Waiting Gs are not in a queue. Request cancellation and perform the
        // same pending-wake handoff used by I/O readiness so a park racing
        // this scan cannot be lost.
        for (const auto& task : task_registry) {
            if (!task) {
                continue;
            }
            const auto state = task->state();
            if (state == GState::kDead || state == GState::kCancelled ||
                state == GState::kFailed) {
                continue;
            }
            task->request_cancel(false);
            if (task->terminal()) {
                completion_notifications.emplace_back(task);
            }
            if (!task->started()) {
                continue;
            }
            const auto action = task->wake_for_scheduler();
            if (action == WakeAction::kEnqueue && task->try_mark_queued()) {
                runnable.fetch_add(1, std::memory_order_relaxed);
                try {
                    global_queue.emplace_back(task);
                } catch (...) {
                    emergency_enqueue_locked(task);
                }
            } else if (task->state() == GState::kRunnable &&
                       !task->queued() && task->try_mark_queued()) {
                // A notifier may have changed Waiting -> Runnable before it
                // acquired this admission mutex. Shutdown must claim that G
                // itself rather than leave a suspended Runnable unqueued.
                runnable.fetch_add(1, std::memory_order_relaxed);
                emergency_enqueue_locked(task);
            }
        }
        collect_terminal_locked(deferred_destruction);
    }

    bool all_tasks_terminal_locked() const {
        return std::all_of(task_registry.begin(), task_registry.end(),
                           [](const auto& task) {
                               return !task || task->terminal();
                           });
    }

    bool finish_draining_locked() noexcept {
        if (!draining.load(std::memory_order_acquire) ||
            !all_tasks_terminal_locked()) {
            return false;
        }
        draining.store(false, std::memory_order_release);
        stopping.store(true, std::memory_order_release);
        return true;
    }

    SchedulerConfig config;
    std::thread sysmon_thread;
    std::atomic<bool> sysmon_stop{false};
    std::atomic<bool> sysmon_active{false};
    std::atomic<bool> sysmon_retry_pending{false};
    std::atomic<std::uint64_t> sysmon_pass_count{0};
    std::mutex sysmon_wait_mutex;
    std::condition_variable sysmon_wait_condition;
    std::shared_ptr<const void> owner_token;
    std::shared_ptr<TaskCancellationGate> cancellation_gate;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::shared_ptr<Task>> global_queue;
    std::shared_ptr<Task> emergency_head;
    std::shared_ptr<Task> emergency_tail;
    std::vector<std::shared_ptr<Task>> task_registry;
    std::deque<Processor> processors;
    // Machine records outlive their worker thread. Keeping shared slots makes
    // machine indices/references stable while dynamic growth appends workers,
    // and lets snapshots safely observe a worker that is being reaped.
    // A deque keeps published machine slots stable while dynamic M growth
    // appends records; snapshots and BlockingRegion lookup never observe a
    // vector relocation of an existing shared_ptr slot.
    std::deque<std::shared_ptr<Machine>> machines;
    std::atomic<bool> started{false};
    std::atomic<bool> draining{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> accepting{true};
    std::atomic<std::size_t> runnable{0};
    std::atomic<std::size_t> active_workers{0};
    std::atomic<std::size_t> running_workers{0};
    std::atomic<std::size_t> blocking_workers{0};
    std::atomic<std::size_t> next_processor{0};
    std::mutex join_mutex;
};

Scheduler::Scheduler(SchedulerConfig config)
    : m_impl(std::make_unique<Impl>(config, this)) {}

Scheduler::Scheduler(std::size_t processor_count)
    : Scheduler([processor_count] {
          SchedulerConfig config;
          config.processor_count = processor_count;
          return config;
      }()) {}

Scheduler::~Scheduler() {
    // The worker loop stores this object as its scheduler identity. Destroying
    // the owner from one of its own Gs would otherwise free m_impl while that
    // loop is still using it. There is no safe implicit "delete later" point
    // for a stack-allocated Scheduler, so fail fast instead of permitting a
    // use-after-free. The documented ownership contract is that the Scheduler
    // (and IOManager) outlives all worker threads.
    if (current_scheduler() == this) {
        std::terminate();
    }
    shutdown();
    if (m_impl && m_impl->cancellation_gate) {
        std::lock_guard<std::mutex> lock(m_impl->cancellation_gate->m_mutex);
        m_impl->cancellation_gate->m_scheduler = nullptr;
    }
}

void Scheduler::start() {
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    if (m_impl->started.load(std::memory_order_acquire) ||
        m_impl->stopping.load(std::memory_order_acquire)) {
        return;
    }

    m_impl->started.store(true, std::memory_order_release);
    m_impl->accepting.store(true, std::memory_order_release);
    m_impl->sysmon_stop.store(false, std::memory_order_release);
    try {
        for (std::size_t i = 0; i < m_impl->config.min_workers; ++i) {
            (void)m_impl->spawn_worker_locked(this);
        }
        if (m_impl->config.enable_sysmon) {
            m_impl->sysmon_active.store(true, std::memory_order_release);
            m_impl->sysmon_thread = std::thread([impl = m_impl.get(), this] {
                impl->sysmon_loop(this);
            });
        }
    } catch (...) {
        m_impl->sysmon_active.store(false, std::memory_order_release);
        m_impl->accepting.store(false, std::memory_order_release);
        lock.unlock();
        // Drain and join the workers that were successfully created before
        // propagating the thread-creation failure. Leaving a partially
        // started scheduler in draining state would strand accepted Gs.
        shutdown();
        throw;
    }
    lock.unlock();
    m_impl->condition.notify_all();
}

bool Scheduler::shutdown_for(
    std::chrono::steady_clock::duration timeout) {
    if (!m_impl) {
        return true;
    }

    const auto clock_now = std::chrono::steady_clock::now();
    const bool bounded =
        timeout != std::chrono::steady_clock::duration::max();
    const auto nonnegative_timeout =
        timeout < std::chrono::steady_clock::duration::zero()
            ? std::chrono::steady_clock::duration::zero()
            : timeout;
    const auto max_time = std::chrono::steady_clock::time_point::max();
    const auto deadline =
        bounded && nonnegative_timeout < max_time - clock_now
            ? clock_now + nonnegative_timeout
            : max_time;

    // A worker may initiate shutdown, but it must never join another M: two
    // workers doing so concurrently would each wait for the other to return
    // from shutdown. Worker callers only publish the stop request; a later
    // external caller (normally the owner/destructor) performs all joins.
    const bool called_from_worker = current_scheduler() == this;
    std::unique_lock<std::mutex> join_lock(m_impl->join_mutex,
                                           std::defer_lock);
    if (!called_from_worker) {
        join_lock.lock();
    }

    // Keep cancelled queue-owned tasks alive until all scheduler locks have
    // been released. A user capture's destructor is arbitrary C++ code and
    // must not run while it can re-enter this Scheduler.
    std::vector<std::shared_ptr<Task>> deferred_destruction;
    std::vector<std::shared_ptr<Task>> completion_notifications;
    bool was_started = false;

    {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        was_started = m_impl->started.load(std::memory_order_acquire);
        if (!m_impl->draining.load(std::memory_order_acquire) &&
            !m_impl->stopping.load(std::memory_order_acquire)) {
            m_impl->accepting.store(false, std::memory_order_release);
            m_impl->draining.store(true, std::memory_order_release);
            m_impl->begin_shutdown_locked(deferred_destruction,
                                           completion_notifications);
        }
    }

    // sysmon 在 draining 阶段仍保持运行，直到所有已启动 G 完成自然
    // 取消/唤醒。这样长系统调用返回前，监控不会提前丢失替代 M。
    for (const auto& task : completion_notifications) {
        if (task) {
            task->notify_terminal();
        }
    }

    if (!was_started) {
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->stopping.store(true, std::memory_order_release);
            m_impl->draining.store(false, std::memory_order_release);
            m_impl->collect_terminal_locked(deferred_destruction);
            for (auto& processor : m_impl->processors) {
                processor.state.store(PState::kDead,
                                      std::memory_order_release);
            }
        }
        m_impl->stop_sysmon(true);
        if (join_lock.owns_lock()) {
            join_lock.unlock();
        }
        for (auto& task : deferred_destruction) {
            if (task) {
                auto abandoned_function = task->release_callable();
            }
        }
        deferred_destruction.clear();
        completion_notifications.clear();
        return true;
    }
    m_impl->condition.notify_all();

    if (called_from_worker) {
        for (auto& task : deferred_destruction) {
            if (task) {
                auto abandoned_function = task->release_callable();
            }
        }
        deferred_destruction.clear();
        return false;
    }

    // Keep workers alive while every started Fiber observes cancellation and
    // returns through its trampoline. This can wait indefinitely for a G that
    // never reaches a cooperative boundary, just as joining an uncooperative
    // std::thread can; discarding its suspended stack would skip RAII.
    {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        const auto terminal = [this] {
            return m_impl->all_tasks_terminal_locked();
        };
        const bool completed =
            bounded ? m_impl->condition.wait_until(lock, deadline, terminal)
                    : (m_impl->condition.wait(lock, terminal), true);
        m_impl->collect_terminal_locked(deferred_destruction);
        if (!completed) {
            // 保持 draining 状态和已发布的取消请求；Fiber 栈仍由 worker
            // 拥有，不能因为超时而强制释放。后续 shutdown_for()/shutdown()
            // 会从这里继续收尾。
            lock.unlock();
            for (auto& task : deferred_destruction) {
                if (task) {
                    auto abandoned_function = task->release_callable();
                }
            }
            deferred_destruction.clear();
            completion_notifications.clear();
            return false;
        }
        m_impl->stopping.store(true, std::memory_order_release);
        m_impl->draining.store(false, std::memory_order_release);
    }
    m_impl->stop_sysmon(true);
    m_impl->condition.notify_all();

    // 先等待 worker 主动退出；这样有界调用不会在 timeout 后把仍运行的
    // std::thread 留给析构路径。若本轮超时，stopping 保持为 true，后续
    // 调用可以继续等待并完成 join。
    if (bounded) {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        const bool workers_stopped = m_impl->condition.wait_until(
            lock, deadline, [this] {
                return m_impl->active_workers.load(std::memory_order_acquire) ==
                       0;
            });
        if (!workers_stopped) {
            lock.unlock();
            for (auto& task : deferred_destruction) {
                if (task) {
                    auto abandoned_function = task->release_callable();
                }
            }
            deferred_destruction.clear();
            completion_notifications.clear();
            return false;
        }
    }

    // The external caller owns join_mutex, which prevents the worker-side
    // reaper from moving or erasing thread objects while they are joined.
    // Extract one std::thread at a time while holding Impl::mutex, then join
    // it after releasing that mutex because worker epilogues take the mutex
    // before exit. Moving a std::thread is noexcept, so shutdown does not
    // need a temporary vector allocation that could fail during OOM cleanup.
    for (;;) {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            for (const auto& machine : m_impl->machines) {
                if (machine && machine->thread.joinable()) {
                    worker = std::move(machine->thread);
                    break;
                }
            }
        }
        if (!worker.joinable()) {
            break;
        }
        // Worker-initiated shutdown returned above, so this path is always an
        // external owner. Join every remaining thread; comparing reusable
        // std::thread::id values here could otherwise leave a joinable record.
        worker.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        for (auto& processor : m_impl->processors) {
            if (processor.active_machines.load(std::memory_order_acquire) ==
                0) {
                processor.state.store(PState::kDead,
                                      std::memory_order_release);
            }
        }

        bool any_joinable = false;
        for (const auto& machine : m_impl->machines) {
            any_joinable = any_joinable ||
                           (machine && machine->thread.joinable());
        }
        if (!any_joinable) {
            m_impl->started.store(false, std::memory_order_release);
        }
    }
    if (join_lock.owns_lock()) {
        join_lock.unlock();
    }
    for (auto& task : deferred_destruction) {
        if (task) {
            auto abandoned_function = task->release_callable();
        }
    }
    deferred_destruction.clear();
    completion_notifications.clear();
    return true;
}

void Scheduler::shutdown() {
    (void)shutdown_for(std::chrono::steady_clock::duration::max());
}

bool Scheduler::is_running() const noexcept {
    return m_impl && m_impl->started.load(std::memory_order_acquire) &&
           !m_impl->draining.load(std::memory_order_acquire) &&
           !m_impl->stopping.load(std::memory_order_acquire);
}

std::shared_ptr<Task> Scheduler::spawn(Task::Function function) {
    return spawn(std::move(function), TaskOptions{});
}

std::shared_ptr<Task> Scheduler::spawn(Task::Function function,
                                      TaskOptions options) {
    if (options.stack_size == 0) {
        options.stack_size = m_impl->config.fiber_stack_size;
    }
    auto task = std::make_shared<Task>(std::move(function), options);
    if (!enqueue(task)) {
        task->cancel();
    }
    return task;
}

bool Scheduler::enqueue(const std::shared_ptr<Task>& task) {
    if (!task || !m_impl) {
        return false;
    }

    // Do not bind/register a task that has already reached a terminal state.
    // Besides making the public enqueue contract deterministic, this avoids
    // retaining a pre-cancelled callable in the scheduler registry until the
    // next shutdown/worker collection pass.
    const auto initial_state = task->state();
    if (initial_state == GState::kDead || initial_state == GState::kCancelled ||
        initial_state == GState::kFailed) {
        return false;
    }

    // Admission and queue insertion share the scheduler mutex.  This closes
    // the shutdown race where a producer could observe accepting=true, then
    // insert a G after shutdown had already drained the queues.
    std::unique_lock<std::mutex> admission_lock(m_impl->mutex);
    const bool drain_resume =
        m_impl->draining.load(std::memory_order_acquire) &&
        task->started() && task->cancellation_requested();
    if ((!m_impl->accepting.load(std::memory_order_acquire) && !drain_resume) ||
        m_impl->stopping.load(std::memory_order_acquire)) {
        // A failed attempt by an unrelated/stopped scheduler must not poison
        // an otherwise unowned task. spawn() performs the cancellation it
        // needs after enqueue() reports failure.
        bool notify = false;
        if (task->owned_by(m_impl->owner_token)) {
            task->request_cancel(false);
            notify = task->terminal();
        }
        admission_lock.unlock();
        if (notify) {
            task->notify_terminal();
        }
        return false;
    }

    const auto admission_state = task->state();
    if (!drain_resume && admission_state != GState::kNew &&
        admission_state != GState::kRunnable) {
        return false;
    }

    // A G is permanently associated with the scheduler that first admits it;
    // keeping this binding under the admission lock also preserves the global
    // scheduler -> task-transition lock order used by workers and shutdown.
    if (!task->bind_owner(m_impl->owner_token)) {
        return false;
    }
    // Task::cancel reads this weak pointer under the transition mutex. Keep
    // first admission synchronized with concurrent external cancellation;
    // a plain weak_ptr assignment here would be a data race.
    task->bind_cancellation_gate(m_impl->cancellation_gate);

    bool registered_here = task->try_register();
    if (registered_here) {
        try {
            m_impl->task_registry.emplace_back(task);
        } catch (...) {
            task->m_registered.store(false, std::memory_order_release);
            registered_here = false;
            task->request_cancel(false);
            admission_lock.unlock();
            if (task->terminal()) {
                task->notify_terminal();
            }
            return false;
        }
    }

    GState state = task->state();
    if (state == GState::kNew) {
        state = task->promote_new() ? GState::kRunnable : task->state();
    }
    if (state != GState::kRunnable || !task->try_mark_queued()) {
        // A concurrent external cancellation can win after the initial check,
        // after registration but before queue admission. Remove that terminal
        // entry now; arbitrary callable destruction happens after the
        // admission lock is released.
        std::vector<std::shared_ptr<Task>> deferred_destruction;
        if (registered_here) {
            const auto found = std::find(m_impl->task_registry.begin(),
                                         m_impl->task_registry.end(), task);
            if (found != m_impl->task_registry.end()) {
                if (task->terminal()) {
                    deferred_destruction.emplace_back(std::move(*found));
                }
                m_impl->task_registry.erase(found);
            }
            task->m_registered.store(false, std::memory_order_release);
        }
        admission_lock.unlock();
        for (auto& retained_task : deferred_destruction) {
            if (retained_task) {
                auto abandoned_function = retained_task->release_callable();
            }
        }
        return false;
    }

    const auto preferred = (current_scheduler() == this)
                               ? static_cast<std::size_t>(t_processor_id)
                               : m_impl->next_processor.fetch_add(
                                     1, std::memory_order_relaxed);
    auto& processor = m_impl->processors[preferred % m_impl->processors.size()];

    bool use_global = false;
    {
        std::lock_guard<std::mutex> queue_lock(processor.mutex);
        use_global = processor.queue.size() >= m_impl->config.local_queue_limit;
        // Publish the runnable count before making the node visible to a
        // worker.  Otherwise a fast worker can pop immediately after
        // push_back and decrement a counter that has not been incremented
        // yet (size_t underflow and a permanently false wake predicate).
        m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
        try {
            if (!use_global) {
                processor.queue.push_back(task);
            } else {
                m_impl->global_queue.push_back(task);
            }
        } catch (...) {
            if (task->started()) {
                // An already-started Fiber cannot be destroyed or left
                // Runnable without a queue node after allocation failure.
                // The intrusive rescue queue reuses its Task-owned link.
                m_impl->emergency_enqueue_locked(task);
            } else {
                m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                task->clear_queued();
                task->request_cancel(false);
                admission_lock.unlock();
                task->notify_terminal();
                return false;
            }
        }
    }
    admission_lock.unlock();
    // Grow outside the admission critical section. The helper reaps workers
    // that already timed out, then admits only the amount of M needed for the
    // current runnable backlog.
    m_impl->maybe_grow(this);
    m_impl->condition.notify_one();
    return true;
}

bool Scheduler::yield(const std::shared_ptr<Task>& task) {
    if (!task || current_scheduler() != this ||
        current_task().get() != task.get() || task->cancellation_requested() ||
        !task->mark_yielded()) {
        return false;
    }
    task->defer_enqueue();
    if (!Fiber::SuspendForScheduler(SuspendReason::Yield)) {
        return false;
    }
    return !task->cancellation_requested();
}

bool Scheduler::yield_current() {
    return yield(current_task());
}

bool Scheduler::park(const std::shared_ptr<Task>& task) {
    return park_with_reason(task, SuspendReason::Park);
}

bool Scheduler::park_io(const std::shared_ptr<Task>& task) {
    return park_with_reason(task, SuspendReason::Io);
}

bool Scheduler::park_with_reason(const std::shared_ptr<Task>& task,
                                  SuspendReason reason) {
    if (!task || current_scheduler() != this ||
        current_task().get() != task.get()) {
        return false;
    }
    // Shutdown scans waiting Gs under this same mutex. Do not enter waiting
    // after the scan has begun.
    ParkAction action = ParkAction::kRejected;
    {
        std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
        if (m_impl->draining.load(std::memory_order_acquire) ||
            m_impl->stopping.load(std::memory_order_acquire) ||
            task->cancellation_requested()) {
            return false;
        }
        action = task->park_for_scheduler();
    }
    if (action != ParkAction::kParked) {
        return false;
    }
    if (!Fiber::SuspendForScheduler(reason)) {
        return false;
    }
    return !task->cancellation_requested();
}

bool Scheduler::park_current() {
    return park(current_task());
}

bool Scheduler::wake(const std::shared_ptr<Task>& task) {
    return wake_or_cancel(task);
}

bool Scheduler::wake_or_cancel(const std::shared_ptr<Task>& task) {
    if (!task || !task->owned_by(m_impl->owner_token)) {
        return false;
    }
    const auto action = task->wake_for_scheduler();
    if (action == WakeAction::kPending) {
        // The running G will consume the permit if it parks. It is not
        // immediately runnable because its C++ callable still owns the M.
        return false;
    }
    if (action == WakeAction::kRejected) {
        return false;
    }
    if (!enqueue(task)) {
        // An accepted concurrent/shutdown handoff may already have queued the
        // G. Cancel only unstarted work; started Gs must be retained for drain.
        if (!task->started()) {
            (void)task->cancel_if_runnable_unqueued();
        } else if (task->state() == GState::kRunnable && !task->queued()) {
            bool notify = false;
            {
                std::lock_guard<std::mutex> lock(m_impl->mutex);
                if (task->try_mark_queued()) {
                    task->request_cancel(false);
                    m_impl->runnable.fetch_add(1, std::memory_order_relaxed);
                    m_impl->emergency_enqueue_locked(task);
                    notify = task->terminal();
                }
            }
            if (notify) {
                task->notify_terminal();
            }
        }
        m_impl->condition.notify_all();
        return false;
    }
    return true;
}

bool Scheduler::cancel(const std::shared_ptr<Task>& task) {
    if (!task || !m_impl || !task->owned_by(m_impl->owner_token)) {
        return false;
    }
    const bool changed = task->request_cancel();
    if (!changed || !task->started()) {
        if (changed) {
            // Unstarted cancellation changes the shutdown predicate without
            // necessarily enqueueing or waking a worker.
            m_impl->condition.notify_all();
        }
        return changed;
    }

    const auto action = task->wake_for_scheduler();
    if (action == WakeAction::kEnqueue) {
        if (!enqueue(task)) {
            (void)task->cancel_if_runnable_unqueued();
        }
    }
    // A running G consumes the pending token at its next park. A waiting G is
    // now queued, while an already-runnable G needs no additional node.
    return true;
}

Scheduler::BlockingRegion::BlockingRegion(Scheduler* scheduler) noexcept
    : m_scheduler(scheduler != nullptr ? scheduler
                                       : Scheduler::current_scheduler()) {
    if (m_scheduler == nullptr ||
        m_scheduler != Scheduler::current_scheduler()) {
        return;
    }
    m_active = Scheduler::enter_blocking();
    if (m_active) {
        m_machine_id = Scheduler::current_machine_id();
    }
}

Scheduler::BlockingRegion::~BlockingRegion() noexcept {
    if (m_active && m_scheduler != nullptr) {
        Scheduler::leave_blocking_for(m_scheduler, m_machine_id);
        if (t_blocking_machine_id == m_machine_id) {
            t_blocking_machine_id = 0;
        }
        m_active = false;
    }
}

bool Scheduler::enter_blocking() noexcept {
    Scheduler* const scheduler = current_scheduler();
    if (scheduler == nullptr || !scheduler->m_impl || !current_task()) {
        return false;
    }
    bool entered = false;
    {
        std::lock_guard<std::mutex> lock(scheduler->m_impl->mutex);
        for (const auto& machine : scheduler->m_impl->machines) {
            if (!machine || machine->id != t_machine_id) {
                continue;
            }
            const auto state = machine->state.load(std::memory_order_relaxed);
            if (state == MState::kRunning) {
                machine->state.store(MState::kBlocking,
                                     std::memory_order_release);
                machine->blocking_since_ns.store(steady_now_ns(),
                                                std::memory_order_release);
                const auto task = current_task();
                machine->blocking_task.store(
                    task ? task->id() : 0, std::memory_order_release);
                machine->processor_detached.store(false,
                                                   std::memory_order_release);
                scheduler->m_impl->blocking_workers.fetch_add(
                    1, std::memory_order_relaxed);
                entered = true;
            }
            break;
        }
    }
    if (entered) {
        t_blocking_machine_id = t_machine_id;
        // Growth is deliberately outside the admission lock.  A queued G can
        // now obtain a replacement M even when this M enters a native call.
        scheduler->m_impl->maybe_grow(scheduler);
    }
    return entered;
}

void Scheduler::leave_blocking() noexcept {
    Scheduler* const scheduler = current_scheduler();
    if (scheduler == nullptr || !scheduler->m_impl ||
        t_blocking_machine_id == 0) {
        return;
    }
    leave_blocking_for(scheduler, t_blocking_machine_id);
    t_blocking_machine_id = 0;
}

void Scheduler::leave_blocking_for(Scheduler* scheduler,
                                   MId machine_id) noexcept {
    if (scheduler == nullptr || !scheduler->m_impl || machine_id == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(scheduler->m_impl->mutex);
    for (const auto& machine : scheduler->m_impl->machines) {
        if (!machine || machine->id != machine_id) {
            continue;
        }
        if (machine->state.load(std::memory_order_relaxed) ==
            MState::kBlocking) {
            const bool detached = machine->processor_detached.exchange(
                false, std::memory_order_acq_rel);
            machine->blocking_since_ns.store(0, std::memory_order_release);
            machine->blocking_task.store(0, std::memory_order_release);
            machine->state.store(MState::kRunning,
                                 std::memory_order_release);
            scheduler->m_impl->blocking_workers.fetch_sub(
                1, std::memory_order_relaxed);
            if (detached) {
                auto& processor = scheduler->m_impl->processors[
                    machine->processor % scheduler->m_impl->processors.size()];
                processor.active_machines.fetch_add(1,
                                                    std::memory_order_acq_rel);
                processor.state.store(PState::kRunning,
                                      std::memory_order_release);
            }
        }
        break;
    }
}

std::size_t Scheduler::processor_count() const noexcept {
    return m_impl ? m_impl->processors.size() : 0;
}

std::size_t Scheduler::worker_count() const noexcept {
    return m_impl ? m_impl->active_workers.load(std::memory_order_acquire) : 0;
}

std::size_t Scheduler::runnable_count() const noexcept {
    return m_impl ? m_impl->runnable.load(std::memory_order_acquire) : 0;
}

std::size_t Scheduler::global_runnable_count() const noexcept {
    if (!m_impl) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->global_queue.size();
}

std::vector<ProcessorSnapshot> Scheduler::processors() const {
    std::vector<ProcessorSnapshot> result;
    if (!m_impl) {
        return result;
    }
    result.reserve(m_impl->processors.size());
    for (const auto& processor : m_impl->processors) {
        std::lock_guard<std::mutex> lock(processor.mutex);
        result.push_back(ProcessorSnapshot{
            processor.id, processor.state.load(std::memory_order_acquire),
            processor.queue.size()});
    }
    return result;
}

std::vector<MachineSnapshot> Scheduler::machines() const {
    std::vector<MachineSnapshot> result;
    if (!m_impl) {
        return result;
    }
    // start() appends to this deque while holding the scheduler mutex.  Take
    // the same lock for snapshots so embedders can inspect machines while a
    // scheduler is being started without racing container mutation.
    std::lock_guard<std::mutex> scheduler_lock(m_impl->mutex);
    result.reserve(m_impl->machines.size());
    for (const auto& machine : m_impl->machines) {
        if (!machine) {
            continue;
        }
        result.push_back(MachineSnapshot{
            machine->id, machine->state.load(std::memory_order_acquire),
            machine->processor, machine->last_task_class,
            machine->affinity_hits, machine->affinity_misses,
            machine->processor_detached.load(std::memory_order_acquire),
            machine->blocking_task.load(std::memory_order_acquire),
            machine->long_syscall_count.load(std::memory_order_acquire)});
    }
    return result;
}

bool Scheduler::sysmon_running() const noexcept {
    return m_impl && m_impl->sysmon_active.load(std::memory_order_acquire) &&
           !m_impl->sysmon_stop.load(std::memory_order_acquire);
}

std::uint64_t Scheduler::sysmon_pass_count() const noexcept {
    return m_impl ? m_impl->sysmon_pass_count.load(std::memory_order_acquire)
                  : 0;
}

std::shared_ptr<Task> Scheduler::current_task() noexcept {
    return t_task;
}

Scheduler* Scheduler::current_scheduler() noexcept {
    return t_scheduler;
}

MId Scheduler::current_machine_id() noexcept {
    return t_machine_id;
}

PId Scheduler::current_processor_id() noexcept {
    return t_processor_id;
}

void Scheduler::worker_loop(std::shared_ptr<void> opaque_machine) {
    auto machine = std::static_pointer_cast<Impl::Machine>(opaque_machine);
    if (!machine) {
        return;
    }
    t_scheduler = this;
    thread_policy::detail::EnterRuntimeWorker(this);
    t_machine_id = machine->id;
    t_processor_id = machine->processor;
    auto& own_processor =
        m_impl->processors[machine->processor % m_impl->processors.size()];
    bool retired = false;

    for (;;) {
        std::shared_ptr<Task> task;
        bool claimed = false;
        {
            // Queue removal, the shutdown check, and the runnable->running
            // claim form one admission transaction.  In particular, an
            // external enqueue cannot observe a popped Runnable G with its
            // queued bit already cleared and publish a duplicate node before
            // this worker owns the execution claim.
            std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
            if (!m_impl->stopping.load(std::memory_order_acquire)) {
                const bool prefer_class =
                    machine->last_task_class != 0 &&
                    machine->affinity_budget != 0 &&
                    m_impl->config.task_affinity_budget != 0;
                const auto preferred_class = machine->last_task_class;
                const auto scan_limit = m_impl->config.task_affinity_budget;
                const auto take_local = [&](auto& queue) {
                    if (prefer_class) {
                        auto it = queue.begin();
                        std::size_t scanned = 0;
                        for (; it != queue.end() && scanned < scan_limit;
                             ++it, ++scanned) {
                            if (*it && (*it)->task_class() == preferred_class) {
                                auto selected = std::move(*it);
                                queue.erase(it);
                                return selected;
                            }
                        }
                    }
                    if (queue.empty()) {
                        return std::shared_ptr<Task>{};
                    }
                    auto selected = std::move(queue.front());
                    queue.pop_front();
                    return selected;
                };
                const auto take_global = [&](auto& queue) {
                    if (prefer_class) {
                        auto it = queue.begin();
                        std::size_t scanned = 0;
                        for (; it != queue.end() && scanned < scan_limit;
                             ++it, ++scanned) {
                            if (*it && (*it)->task_class() == preferred_class) {
                                auto selected = std::move(*it);
                                queue.erase(it);
                                return selected;
                            }
                        }
                    }
                    if (queue.empty()) {
                        return std::shared_ptr<Task>{};
                    }
                    auto selected = std::move(queue.front());
                    queue.pop_front();
                    return selected;
                };
                const auto take_victim = [&](auto& queue) {
                    if (prefer_class) {
                        auto it = queue.rbegin();
                        std::size_t scanned = 0;
                        for (; it != queue.rend() && scanned < scan_limit;
                             ++it, ++scanned) {
                            if (*it && (*it)->task_class() == preferred_class) {
                                auto base = std::next(it).base();
                                auto selected = std::move(*base);
                                queue.erase(base);
                                return selected;
                            }
                        }
                    }
                    if (queue.empty()) {
                        return std::shared_ptr<Task>{};
                    }
                    auto selected = std::move(queue.back());
                    queue.pop_back();
                    return selected;
                };

                // Rescue resumes are serviced first: they exist only after
                // queue allocation failure or a shutdown wake handoff.
                task = m_impl->emergency_pop_locked();

                // Prefer the local FIFO queue.
                if (!task) {
                    std::lock_guard<std::mutex> queue_lock(own_processor.mutex);
                    if (!own_processor.queue.empty()) {
                        task = take_local(own_processor.queue);
                    }
                }

                // Then inspect the global FIFO queue.
                if (!task && !m_impl->global_queue.empty()) {
                    task = take_global(m_impl->global_queue);
                }

                // Finally steal from the back of another P. A bounded scan
                // avoids monopolizing the scheduler when all processors are
                // busy.
                if (!task && m_impl->processors.size() > 1) {
                    const auto start = m_impl->next_processor.fetch_add(
                        1, std::memory_order_relaxed);
                    for (std::size_t offset = 0;
                         offset < m_impl->processors.size(); ++offset) {
                        auto& victim = m_impl->processors[
                            (start + offset) % m_impl->processors.size()];
                        if (&victim == &own_processor) {
                            continue;
                        }
                        std::lock_guard<std::mutex> queue_lock(victim.mutex);
                        if (!victim.queue.empty()) {
                            task = take_victim(victim.queue);
                            break;
                        }
                    }
                }

                if (task) {
                    m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
                    claimed = task->try_mark_running();
                    if (!claimed) {
                        // The node is no longer present in a queue. Clear its
                        // reservation even when cancellation or a stale
                        // duplicate made the execution claim fail.
                        task->clear_queued();
                    }
                    if (claimed) {
                        m_impl->running_workers.fetch_add(
                            1, std::memory_order_relaxed);
                        machine->state.store(MState::kRunning,
                                             std::memory_order_release);
                        const auto task_class = task->task_class();
                        if (task_class != 0 && task_class ==
                                                    machine->last_task_class &&
                            machine->affinity_budget != 0) {
                            ++machine->affinity_hits;
                            --machine->affinity_budget;
                        } else {
                            if (machine->last_task_class != 0 &&
                                task_class != machine->last_task_class) {
                                ++machine->affinity_misses;
                            }
                            machine->last_task_class = task_class;
                            machine->affinity_budget =
                                task_class == 0
                                    ? 0
                                    : m_impl->config.task_affinity_budget;
                        }
                    }
                }
            }
        }

        if (task && claimed) {
            // A pre-start backlog needs growth too. Account for this claim
            // before checking demand so a blocked first G cannot prevent its
            // queued peer from getting a replacement M (within the P cap).
            m_impl->maybe_grow(this);
            own_processor.running_machines.fetch_add(1,
                                                     std::memory_order_relaxed);
            own_processor.state.store(PState::kRunning,
                                      std::memory_order_release);
            t_task = task;
            Fiber::BindCurrentExecution(FiberExecutionBinding{
                reinterpret_cast<std::uintptr_t>(this), 0, t_machine_id,
                t_processor_id, true});
            task->run();
            t_task.reset();
            Fiber::BindCurrentExecution(FiberExecutionBinding{
                reinterpret_cast<std::uintptr_t>(this), 0, t_machine_id,
                t_processor_id, true});
            // BlockingRegion 不能跨 Fiber yield/park 或 G 的任务边界。若
            // 仍有活动标记，继续调度会让 blocking_workers 和 P 的 attached
            // 计数永久失真；宁可在边界处 fail-fast，也不能静默清掉 TLS 后
            // 把不一致状态传给后续 G。正常 Hook/RAII 路径在这里必为 0。
            if (t_blocking_machine_id != 0) {
                std::terminate();
            }
            if (own_processor.running_machines.fetch_sub(
                    1, std::memory_order_acq_rel) == 1) {
                own_processor.state.store(PState::kIdle,
                                          std::memory_order_release);
            }
            machine->state.store(MState::kIdle, std::memory_order_release);
            m_impl->running_workers.fetch_sub(1, std::memory_order_acq_rel);

            // A yield issued by the running G is committed only after its
            // execution claim is released, preventing a second M from
            // entering the same callable concurrently.
            if (task->state() == GState::kRunnable &&
                !task->queued()) {
                (void)task->consume_deferred_enqueue();
                if (!enqueue(task)) {
                    (void)task->cancel_if_runnable_unqueued();
                }
            } else {
                (void)task->consume_deferred_enqueue();
            }
            std::vector<std::shared_ptr<Task>> deferred_destruction;
            {
                std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
                m_impl->collect_terminal_locked(deferred_destruction);
                (void)m_impl->finish_draining_locked();
            }
            m_impl->condition.notify_all();
            for (auto& retained_task : deferred_destruction) {
                if (retained_task) {
                    auto abandoned_function =
                        retained_task->release_callable();
                }
            }
            deferred_destruction.clear();
            continue;
        }

        if (task && !claimed) {
            // A cancelled/unstarted queue node can lose its execution claim
            // after dequeue. It still has a registry entry; prune it here so
            // a quiet scheduler does not retain terminal callables forever.
            std::vector<std::shared_ptr<Task>> deferred_destruction;
            {
                std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
                m_impl->collect_terminal_locked(deferred_destruction);
            }
            for (auto& retained_task : deferred_destruction) {
                if (retained_task) {
                    auto abandoned_function =
                        retained_task->release_callable();
                }
            }
            deferred_destruction.clear();
            // A concurrent external cancellation can make the registry
            // terminal without touching the scheduler condition variable.
            // Wake an owner waiting for the final terminal transition after
            // this stale queue node has been consumed.
            m_impl->condition.notify_all();
        }

        if (m_impl->stopping.load(std::memory_order_acquire)) {
            machine->state.store(MState::kStopping, std::memory_order_release);
            break;
        }

        machine->state.store(MState::kParked, std::memory_order_release);
        if (own_processor.running_machines.load(std::memory_order_acquire) ==
            0) {
            own_processor.state.store(PState::kIdle,
                                      std::memory_order_release);
        }
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        if (m_impl->finish_draining_locked()) {
            lock.unlock();
            m_impl->condition.notify_all();
            machine->state.store(MState::kStopping,
                                 std::memory_order_release);
            break;
        }
        // 长 syscall 的原 M 仍计入 active_workers，但已经从 P 的
        // attached 计数中移除。替代 M 在原 M 返回前必须保留，不能被
        // 普通 idle timeout 提前回收。
        const auto required_workers = [this] {
            const auto detached = m_impl->detached_machine_count_locked();
            if (detached == 0) {
                return m_impl->config.min_workers;
            }
            const auto handoff_floor =
                detached > std::numeric_limits<std::size_t>::max() -
                              m_impl->config.min_workers
                    ? std::numeric_limits<std::size_t>::max()
                    : m_impl->config.min_workers + detached;
            return std::max(
                m_impl->config.min_workers,
                std::min(m_impl->config.max_workers, handoff_floor));
        };
        const auto worker_floor = required_workers();
        const bool can_shrink =
            m_impl->active_workers.load(std::memory_order_relaxed) >
            worker_floor;
        const auto idle_timeout = can_shrink
                                    ? m_impl->config.idle_worker_timeout
                                    : m_impl->config.idle_wait;
        const bool woke = m_impl->condition.wait_for(lock, idle_timeout, [this] {
            return m_impl->stopping.load(std::memory_order_acquire) ||
                   m_impl->runnable.load(std::memory_order_acquire) != 0;
        });
        const auto current_floor = required_workers();
        if (!woke && can_shrink &&
            m_impl->runnable.load(std::memory_order_acquire) == 0 &&
            !m_impl->stopping.load(std::memory_order_acquire) &&
            m_impl->active_workers.load(std::memory_order_relaxed) >
                current_floor) {
            machine->state.store(MState::kStopping, std::memory_order_release);
            // Reserve the retirement while still holding mutex. Otherwise
            // several idle M's can all observe active > min and shrink below
            // the configured floor before their epilogues run.
            m_impl->active_workers.fetch_sub(1, std::memory_order_relaxed);
            own_processor.active_machines.fetch_sub(1,
                                                    std::memory_order_relaxed);
            retired = true;
            break;
        }
        machine->state.store(MState::kIdle, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!retired) {
            // sysmon 可能已经从 P 的 attached 计数中移除了这个 M。
            // 退出时只撤销仍然 attached 的计数，避免 size_t 下溢。
            if (!machine->processor_detached.load(std::memory_order_acquire)) {
                own_processor.active_machines.fetch_sub(
                    1, std::memory_order_relaxed);
            }
            m_impl->active_workers.fetch_sub(1, std::memory_order_relaxed);
        }
        if (own_processor.active_machines.load(std::memory_order_relaxed) == 0) {
            own_processor.state.store(
                m_impl->stopping.load(std::memory_order_acquire)
                    ? PState::kDead
                    : PState::kIdle,
                std::memory_order_release);
        }
        machine->state.store(MState::kDead, std::memory_order_release);
    }
    m_impl->condition.notify_all();
    t_task.reset();
    Fiber::BindCurrentExecution(FiberExecutionBinding{});
    thread_policy::detail::LeaveRuntimeWorker();
    t_scheduler = nullptr;
    t_machine_id = 0;
    t_processor_id = 0;
    t_blocking_machine_id = 0;
}

}  // namespace go2cpp::scheduler
