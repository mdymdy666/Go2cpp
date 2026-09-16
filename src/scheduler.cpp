#include "go2cpp/scheduler/scheduler.hpp"

#include "go2cpp/panic_defer.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
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

std::size_t default_processor_count() noexcept {
    const auto count = std::thread::hardware_concurrency();
    return count == 0 ? 1U : static_cast<std::size_t>(count);
}

}  // namespace

Task::Task(Function function)
    : m_id(s_next_g_id.fetch_add(1, std::memory_order_relaxed)),
      m_function(std::move(function)),
      m_state(GState::kNew) {}

Task::~Task() = default;

GId Task::id() const noexcept {
    return m_id;
}

GState Task::state() const noexcept {
    return m_state.load(std::memory_order_acquire);
}

bool Task::queued() const noexcept {
    return m_queued.load(std::memory_order_acquire);
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
        m_state.store(GState::kRunnable, std::memory_order_release);
        m_deferred_enqueue.store(true, std::memory_order_release);
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
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    const auto state = m_state.load(std::memory_order_relaxed);
    if (state == GState::kDead || state == GState::kCancelled) {
        return false;
    }
    m_state.store(GState::kCancelled, std::memory_order_release);
    m_queued.store(false, std::memory_order_release);
    m_wake_pending.store(false, std::memory_order_release);
    m_deferred_enqueue.store(false, std::memory_order_release);
    return true;
}

bool Task::cancel_if_runnable_unqueued() noexcept {
    std::lock_guard<std::mutex> lock(m_transition_mutex);
    if (m_state.load(std::memory_order_relaxed) != GState::kRunnable ||
        m_queued.load(std::memory_order_relaxed) ||
        m_execution_claim.load(std::memory_order_relaxed)) {
        return false;
    }
    m_state.store(GState::kCancelled, std::memory_order_release);
    m_wake_pending.store(false, std::memory_order_relaxed);
    m_deferred_enqueue.store(false, std::memory_order_relaxed);
    return true;
}

void Task::run() {
    if (!m_execution_claim.load(std::memory_order_acquire)) {
        return;
    }

    bool completed = true;
    try {
        if (m_function) {
            // Each G gets a fresh panic/defer boundary.  The helper keeps
            // panic state local to this worker invocation, so a reused M
            // cannot leak recoverable state into its next G.
            completed = panic_defer::run(m_function);
        }
    } catch (...) {
        // C++ exceptions are not a runtime control-flow mechanism here.  A
        // callback that throws terminates its G at this boundary and cannot
        // take down an M worker or another G.
        completed = false;
    }

    // A body can request a cooperative yield before asking for an unhandled
    // panic. In that case panic_defer::run() returns false while the state is
    // already runnable; force a terminal state so the worker cannot requeue a
    // task whose goroutine has actually terminated. State and wake-token
    // updates share the transition mutex with park/wake/cancel.
    {
        std::lock_guard<std::mutex> lock(m_transition_mutex);
        if (!completed) {
            m_deferred_enqueue.store(false, std::memory_order_relaxed);
            const auto state = m_state.load(std::memory_order_relaxed);
            if (state != GState::kCancelled && state != GState::kDead) {
                m_state.store(GState::kDead, std::memory_order_release);
            }
        } else {
            // A cooperative yield deliberately leaves the task runnable. Only
            // a task still owned by this execution claim may be finalized;
            // the worker loop requeues a yielded task after releasing it.
            if (m_state.load(std::memory_order_relaxed) == GState::kRunning) {
                m_state.store(GState::kDead, std::memory_order_release);
            }
        }
        const auto state = m_state.load(std::memory_order_relaxed);
        if (state == GState::kDead || state == GState::kCancelled) {
            m_wake_pending.store(false, std::memory_order_relaxed);
            m_deferred_enqueue.store(false, std::memory_order_relaxed);
        }
    }
    m_execution_claim.store(false, std::memory_order_release);
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
    };

    explicit Impl(SchedulerConfig requested)
        : config(normalize(requested)),
          owner_token(std::make_shared<const std::uint8_t>(0)) {
        for (std::size_t i = 0; i < config.processor_count; ++i) {
            processors.emplace_back(static_cast<PId>(i));
        }
    }

    ~Impl() = default;

    static SchedulerConfig normalize(SchedulerConfig requested) {
        if (requested.processor_count == 0) {
            requested.processor_count = default_processor_count();
        }
        requested.processor_count = std::max<std::size_t>(1,
                                                           requested.processor_count);
        if (requested.max_workers == 0) {
            requested.max_workers = requested.processor_count;
        }
        requested.max_workers = std::max<std::size_t>(1, requested.max_workers);
        // This implementation gives each worker a stable P affinity. Keep
        // one M per P so a P is never concurrently owned by multiple M's.
        requested.max_workers = std::min(requested.max_workers,
                                         requested.processor_count);
        if (requested.idle_wait <= std::chrono::milliseconds::zero()) {
            requested.idle_wait = std::chrono::milliseconds(1);
        }
        return requested;
    }

    void prune_registry_locked() {
        // Terminal Gs no longer participate in shutdown cancellation. Remove
        // their weak entries even when callers intentionally retain handles;
        // otherwise a long-lived scheduler's bookkeeping would grow without
        // bound despite the task objects themselves being reference-counted.
        task_registry.erase(
            std::remove_if(task_registry.begin(), task_registry.end(),
                           [](const auto& entry) {
                               const auto task = entry.lock();
                               return !task ||
                                      task->state() == GState::kDead ||
                                      task->state() == GState::kCancelled;
                           }),
            task_registry.end());
    }

    void cancel_queued_locked(
        std::vector<std::shared_ptr<Task>>& deferred_destruction) {
        // Caller owns mutex.  Each processor queue is locked independently;
        // enqueue uses the same mutex -> processor lock order.
        std::size_t queued_count = global_queue.size();
        for (const auto& processor : processors) {
            std::lock_guard<std::mutex> queue_lock(processor.mutex);
            queued_count += processor.queue.size();
        }
        deferred_destruction.reserve(deferred_destruction.size() +
                                     queued_count);
        for (auto& processor : processors) {
            std::lock_guard<std::mutex> queue_lock(processor.mutex);
            while (!processor.queue.empty()) {
                auto task = std::move(processor.queue.front());
                processor.queue.pop_front();
                task->clear_queued();
                task->cancel();
                runnable.fetch_sub(1, std::memory_order_relaxed);
                deferred_destruction.emplace_back(std::move(task));
            }
            bool has_worker = false;
            for (const auto& machine : machines) {
                if (machine.processor == processor.id &&
                    machine.thread.joinable()) {
                    has_worker = true;
                    break;
                }
            }
            // An unbound P has no worker that can perform the normal exit
            // transition, so finish it here. Bound P instances remain
            // stopping until their M leaves worker_loop().
            if (processor.state.load(std::memory_order_acquire) !=
                PState::kDead) {
                processor.state.store(has_worker ? PState::kStopping
                                                  : PState::kDead,
                                      std::memory_order_release);
            }
        }
        while (!global_queue.empty()) {
            auto task = std::move(global_queue.front());
            global_queue.pop_front();
            task->clear_queued();
            task->cancel();
            runnable.fetch_sub(1, std::memory_order_relaxed);
            deferred_destruction.emplace_back(std::move(task));
        }

        // Waiting Gs are not present in a run queue. Cancel them while the
        // scheduler mutex is held so a concurrent park cannot publish a new
        // waiting state after this scan.
        for (auto it = task_registry.begin(); it != task_registry.end();) {
            if (auto task = it->lock()) {
                const auto state = task->state();
                if (state == GState::kWaiting || state == GState::kRunnable ||
                    state == GState::kNew) {
                    task->cancel();
                }
                ++it;
            } else {
                it = task_registry.erase(it);
            }
        }
        prune_registry_locked();
    }

    SchedulerConfig config;
    std::shared_ptr<const void> owner_token;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::shared_ptr<Task>> global_queue;
    std::vector<std::weak_ptr<Task>> task_registry;
    std::deque<Processor> processors;
    std::deque<Machine> machines;
    std::atomic<bool> started{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> accepting{true};
    std::atomic<std::size_t> runnable{0};
    std::atomic<std::size_t> active_workers{0};
    std::atomic<std::size_t> next_processor{0};
    std::mutex join_mutex;
};

Scheduler::Scheduler(SchedulerConfig config)
    : m_impl(std::make_unique<Impl>(config)) {}

Scheduler::Scheduler(std::size_t processor_count)
    : Scheduler(SchedulerConfig{processor_count, processor_count,
                                std::chrono::milliseconds{10}, 256}) {}

Scheduler::~Scheduler() {
    shutdown();
}

void Scheduler::start() {
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    if (m_impl->started.load(std::memory_order_acquire) ||
        m_impl->stopping.load(std::memory_order_acquire)) {
        return;
    }

    m_impl->started.store(true, std::memory_order_release);
    m_impl->accepting.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < m_impl->config.max_workers; ++i) {
        const auto processor = static_cast<PId>(i % m_impl->processors.size());
        m_impl->machines.emplace_back(s_next_m_id.fetch_add(
                                          1, std::memory_order_relaxed),
                                      processor);
    }
    // Publish the complete machine table before any worker can inspect it;
    // otherwise a first worker could race a later deque insertion.
    for (std::size_t i = 0; i < m_impl->config.max_workers; ++i) {
        const auto machine_index = i;
        auto& machine = m_impl->machines[i];
        m_impl->active_workers.fetch_add(1, std::memory_order_relaxed);
        try {
            machine.thread = std::thread([this, machine_index] {
                worker_loop(machine_index);
            });
        } catch (...) {
            m_impl->active_workers.fetch_sub(1, std::memory_order_relaxed);
            throw;
        }
    }
    lock.unlock();
    m_impl->condition.notify_all();
}

void Scheduler::shutdown() {
    if (!m_impl) {
        return;
    }

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
    bool was_started = false;

    {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        was_started = m_impl->started.load(std::memory_order_acquire);
        if (!was_started) {
            m_impl->accepting.store(false, std::memory_order_release);
            if (!m_impl->stopping.load(std::memory_order_acquire)) {
                // Drain first and publish the terminal stop state only after
                // all potentially-allocating bookkeeping has succeeded.
                // This leaves a retryable state if an allocation fails.
                m_impl->cancel_queued_locked(deferred_destruction);
                m_impl->stopping.store(true, std::memory_order_release);
            }
        } else if (!m_impl->stopping.load(std::memory_order_acquire)) {
            m_impl->accepting.store(false, std::memory_order_release);
            // No queued G may survive shutdown.  Running Gs are allowed to
            // finish on their current M. Publish stopping only after the
            // queue-drain transaction succeeds, so an allocation failure can
            // be retried without losing queued work.
            m_impl->cancel_queued_locked(deferred_destruction);
            m_impl->stopping.store(true, std::memory_order_release);
        }
    }
    // The queue/P/scheduler locks are no longer held here. Keep the vector
    // until shutdown has finished so its final clear also occurs outside the
    // join mutex and cannot deadlock a re-entrant user destructor.
    if (!was_started) {
        if (join_lock.owns_lock()) {
            join_lock.unlock();
        }
        deferred_destruction.clear();
        return;
    }
    m_impl->condition.notify_all();

    if (called_from_worker) {
        deferred_destruction.clear();
        return;
    }

    const auto self = std::this_thread::get_id();
    for (auto& machine : m_impl->machines) {
        if (!machine.thread.joinable()) {
            continue;
        }
        if (machine.thread.get_id() == self) {
            // A worker may request shutdown from inside its own G.  It cannot
            // join itself; the owning thread (usually the destructor caller)
            // will join it on the next shutdown() call.
            continue;
        }
        machine.thread.join();
    }

    for (auto& processor : m_impl->processors) {
        if (processor.active_machines.load(std::memory_order_acquire) == 0) {
            processor.state.store(PState::kDead, std::memory_order_release);
        }
    }

    bool any_joinable = false;
    for (const auto& machine : m_impl->machines) {
        any_joinable = any_joinable || machine.thread.joinable();
    }
    if (!any_joinable) {
        m_impl->started.store(false, std::memory_order_release);
    }
    if (join_lock.owns_lock()) {
        join_lock.unlock();
    }
    deferred_destruction.clear();
}

bool Scheduler::is_running() const noexcept {
    return m_impl && m_impl->started.load(std::memory_order_acquire) &&
           !m_impl->stopping.load(std::memory_order_acquire);
}

std::shared_ptr<Task> Scheduler::spawn(Task::Function function) {
    auto task = std::make_shared<Task>(std::move(function));
    if (!enqueue(task)) {
        task->cancel();
    }
    return task;
}

bool Scheduler::enqueue(const std::shared_ptr<Task>& task) {
    if (!task || !m_impl) {
        return false;
    }

    // Admission and queue insertion share the scheduler mutex.  This closes
    // the shutdown race where a producer could observe accepting=true, then
    // insert a G after shutdown had already drained the queues.
    std::unique_lock<std::mutex> admission_lock(m_impl->mutex);
    if (!m_impl->accepting.load(std::memory_order_acquire) ||
        m_impl->stopping.load(std::memory_order_acquire)) {
        // A failed attempt by an unrelated/stopped scheduler must not poison
        // an otherwise unowned task. spawn() performs the cancellation it
        // needs after enqueue() reports failure.
        if (task->owned_by(m_impl->owner_token)) {
            task->cancel();
        }
        return false;
    }

    // A G is permanently associated with the scheduler that first admits it;
    // keeping this binding under the admission lock also preserves the global
    // scheduler -> task-transition lock order used by workers and shutdown.
    if (!task->bind_owner(m_impl->owner_token)) {
        return false;
    }

    m_impl->prune_registry_locked();
    if (task->try_register()) {
        try {
            m_impl->task_registry.emplace_back(task);
        } catch (...) {
            task->m_registered.store(false, std::memory_order_release);
            task->cancel();
            return false;
        }
    }

    GState state = task->state();
    if (state == GState::kNew) {
        state = task->promote_new() ? GState::kRunnable : task->state();
    }
    if (state != GState::kRunnable || !task->try_mark_queued()) {
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
            m_impl->runnable.fetch_sub(1, std::memory_order_relaxed);
            task->clear_queued();
            task->cancel();
            return false;
        }
    }
    admission_lock.unlock();
    m_impl->condition.notify_one();
    return true;
}

bool Scheduler::yield(const std::shared_ptr<Task>& task) {
    // A running G has no resumable C++ stack. Only that G can yield itself;
    // allowing an external caller to mark it runnable would publish a second
    // queue entry while its execution claim is still held.
    if (!task || current_scheduler() != this ||
        current_task().get() != task.get() || !task->mark_yielded()) {
        return false;
    }
    task->defer_enqueue();
    return true;
}

bool Scheduler::park(const std::shared_ptr<Task>& task) {
    if (!task || current_scheduler() != this ||
        current_task().get() != task.get()) {
        return false;
    }
    // Shutdown scans waiting Gs under this same mutex. Do not enter waiting
    // after the scan has begun.
    std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
    if (m_impl->stopping.load(std::memory_order_acquire)) {
        return false;
    }
    return task->park_for_scheduler() == ParkAction::kParked;
}

bool Scheduler::wake(const std::shared_ptr<Task>& task) {
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
        (void)task->cancel_if_runnable_unqueued();
        return false;
    }
    return true;
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
        result.push_back(MachineSnapshot{
            machine.id, machine.state.load(std::memory_order_acquire),
            machine.processor});
    }
    return result;
}

std::shared_ptr<Task> Scheduler::current_task() noexcept {
    return t_task;
}

Scheduler* Scheduler::current_scheduler() noexcept {
    return t_scheduler;
}

void Scheduler::worker_loop(std::size_t machine_index) {
    auto& machine = m_impl->machines[machine_index];
    t_scheduler = this;
    t_machine_id = machine.id;
    t_processor_id = machine.processor;
    auto& own_processor = m_impl->processors[machine.processor % m_impl->processors.size()];
    own_processor.active_machines.fetch_add(1, std::memory_order_relaxed);

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
                // Prefer the local FIFO queue.
                {
                    std::lock_guard<std::mutex> queue_lock(own_processor.mutex);
                    if (!own_processor.queue.empty()) {
                        task = std::move(own_processor.queue.front());
                        own_processor.queue.pop_front();
                    }
                }

                // Then inspect the global FIFO queue.
                if (!task && !m_impl->global_queue.empty()) {
                    task = std::move(m_impl->global_queue.front());
                    m_impl->global_queue.pop_front();
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
                            task = std::move(victim.queue.back());
                            victim.queue.pop_back();
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
                }
            }
        }

        if (task && claimed) {
            machine.state.store(MState::kRunning, std::memory_order_release);
            own_processor.running_machines.fetch_add(1,
                                                     std::memory_order_relaxed);
            own_processor.state.store(PState::kRunning,
                                      std::memory_order_release);
            t_task = task;
            task->run();
            t_task.reset();
            if (own_processor.running_machines.fetch_sub(
                    1, std::memory_order_acq_rel) == 1) {
                own_processor.state.store(PState::kIdle,
                                          std::memory_order_release);
            }
            machine.state.store(MState::kIdle, std::memory_order_release);

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
            {
                std::lock_guard<std::mutex> admission_lock(m_impl->mutex);
                m_impl->prune_registry_locked();
            }
            continue;
        }

        if (m_impl->stopping.load(std::memory_order_acquire)) {
            machine.state.store(MState::kStopping, std::memory_order_release);
            break;
        }

        machine.state.store(MState::kParked, std::memory_order_release);
        if (own_processor.running_machines.load(std::memory_order_acquire) ==
            0) {
            own_processor.state.store(PState::kIdle,
                                      std::memory_order_release);
        }
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        m_impl->condition.wait_for(lock, m_impl->config.idle_wait, [this] {
            return m_impl->stopping.load(std::memory_order_acquire) ||
                   m_impl->runnable.load(std::memory_order_acquire) != 0;
        });
        machine.state.store(MState::kIdle, std::memory_order_release);
    }

    if (own_processor.active_machines.fetch_sub(1, std::memory_order_acq_rel) ==
        1) {
        own_processor.state.store(PState::kDead,
                                  std::memory_order_release);
    }
    machine.state.store(MState::kDead, std::memory_order_release);
    m_impl->active_workers.fetch_sub(1, std::memory_order_relaxed);
    t_task.reset();
    t_scheduler = nullptr;
    t_machine_id = 0;
    t_processor_id = 0;
}

}  // namespace go2cpp::scheduler
