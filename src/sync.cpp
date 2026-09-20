#include "go2cpp/sync.hpp"

#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace go2cpp::sync {
namespace {

enum class WaitResult : std::uint8_t {
    kWaiting,
    kNotified,
    kCancelled,
};

class WaitNode final {
public:
    WaitNode(Scheduler* scheduler, std::shared_ptr<Task> task)
        : m_scheduler(scheduler), m_task(std::move(task)) {}

    ~WaitNode() { Disarm(); }

    bool Arm() noexcept {
        std::lock_guard<std::mutex> lock(m_wake_mutex);
        if (result() != WaitResult::kWaiting) {
            return false;
        }
        m_active = true;
        return true;
    }

    void Disarm() noexcept {
        // A callback may already have left DoneSignal's registry. Waiting for
        // its Wake call here prevents it from outliving Scheduler shutdown.
        std::lock_guard<std::mutex> lock(m_wake_mutex);
        m_active = false;
        m_scheduler = nullptr;
    }

    WaitResult result() const noexcept {
        return m_result.load(std::memory_order_acquire);
    }

    bool TryFinish(WaitResult result) noexcept {
        WaitResult expected = WaitResult::kWaiting;
        return m_result.compare_exchange_strong(
            expected, result, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    void Wake() noexcept {
        std::lock_guard<std::mutex> lock(m_wake_mutex);
        try {
            if (m_active && m_scheduler != nullptr && m_task) {
                (void)m_scheduler->wake(m_task);
            }
        } catch (...) {
            // Scheduler's reliable wake handoff retains every started G.
        }
    }

    const std::shared_ptr<Task>& task() const noexcept { return m_task; }
    Scheduler* scheduler() const noexcept { return m_scheduler; }

private:
    Scheduler* m_scheduler{nullptr};
    std::shared_ptr<Task> m_task;
    std::atomic<WaitResult> m_result{WaitResult::kWaiting};
    std::mutex m_wake_mutex;
    bool m_active{false};
};

struct WaitTarget {
    Scheduler* scheduler{nullptr};
    std::shared_ptr<Task> task;

    explicit operator bool() const noexcept {
        return scheduler != nullptr && static_cast<bool>(task);
    }
};

WaitTarget CurrentTarget() noexcept {
    return {Scheduler::current_scheduler(), Scheduler::current_task()};
}

class ContextSubscription final {
public:
    ContextSubscription(const ContextPtr& context,
                        const std::shared_ptr<WaitNode>& waiter)
        : m_context(context) {
        if (!m_context || !waiter) {
            return;
        }
        const std::weak_ptr<WaitNode> weak_waiter(waiter);
        m_id = m_context->Done().AddCallback([weak_waiter] {
            const auto current = weak_waiter.lock();
            if (current && current->TryFinish(WaitResult::kCancelled)) {
                current->Wake();
            }
        });
    }

    ContextSubscription(const ContextSubscription&) = delete;
    ContextSubscription& operator=(const ContextSubscription&) = delete;

    ~ContextSubscription() { Reset(); }

    void Reset() noexcept {
        if (m_context && m_id != 0) {
            m_context->Done().RemoveCallback(m_id);
        }
        m_id = 0;
        m_context.reset();
    }

private:
    ContextPtr m_context;
    DoneSignal::CallbackId m_id{0};
};

class CancelGuard final {
public:
    explicit CancelGuard(CancelFunc cancel) : m_cancel(std::move(cancel)) {}
    ~CancelGuard() {
        if (m_cancel) {
            m_cancel();
        }
    }

    CancelGuard(const CancelGuard&) = delete;
    CancelGuard& operator=(const CancelGuard&) = delete;

private:
    CancelFunc m_cancel;
};

WaitResult Await(const std::shared_ptr<WaitNode>& waiter) noexcept {
    if (!waiter || waiter->scheduler() == nullptr || !waiter->task()) {
        return WaitResult::kCancelled;
    }

    Scheduler* const scheduler = waiter->scheduler();
    const auto& task = waiter->task();
    const auto finish = [&waiter](WaitResult result) {
        waiter->Disarm();
        return result;
    };
    for (;;) {
        if (task->cancellation_requested()) {
            if (waiter->result() == WaitResult::kWaiting) {
                (void)waiter->TryFinish(WaitResult::kCancelled);
            }
            return finish(waiter->result());
        }

        // Always attempt park after publication, even if a notifier may have
        // won already. A notify-before-park wake is stored as a pending token;
        // park consumes that token and returns without suspending.
        const bool suspended = scheduler->park(task);
        const WaitResult result = waiter->result();
        if (result != WaitResult::kWaiting) {
            return finish(result);
        }
        if (task->cancellation_requested() ||
            Scheduler::current_scheduler() != scheduler ||
            Scheduler::current_task().get() != task.get() ||
            (!suspended && !scheduler->is_running())) {
            (void)waiter->TryFinish(WaitResult::kCancelled);
            return finish(waiter->result());
        }
        // A false park can consume an unrelated pending permit. Both that
        // permit and an unrelated unpark are spurious, not cancellation.
    }
}

template <typename Queue>
void RemoveWaiter(Queue& waiters,
                  const std::shared_ptr<WaitNode>& waiter) noexcept {
    const auto position = std::find(waiters.begin(), waiters.end(), waiter);
    if (position != waiters.end()) {
        waiters.erase(position);
    }
}

ContextPtr TimeoutContext(ContextDuration timeout, const ContextPtr& parent,
                          CancelFunc* cancel) {
    auto pair = WithTimeout(parent ? parent : Background(), timeout);
    *cancel = std::move(pair.second);
    return std::move(pair.first);
}

}  // namespace

struct Mutex::Impl {
    std::mutex m_mutex;
    bool m_locked{false};
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

Mutex::Mutex() : m_impl(std::make_unique<Impl>()) {}
Mutex::~Mutex() = default;

bool Mutex::Lock(const ContextPtr& context) {
    if (context && context->IsDone()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (!m_impl->m_locked) {
            m_impl->m_locked = true;
            return true;
        }
    }

    const WaitTarget target = CurrentTarget();
    if (!target || target.task->cancellation_requested()) {
        return false;
    }
    const auto waiter =
        std::make_shared<WaitNode>(target.scheduler, target.task);
    ContextSubscription subscription(context, waiter);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if ((context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting) {
            return false;
        }
        if (!m_impl->m_locked) {
            m_impl->m_locked = true;
            return true;
        }
        m_impl->m_waiters.push_back(waiter);
        if (!waiter->Arm()) {
            m_impl->m_waiters.pop_back();
            return false;
        }
    }

    const WaitResult result = Await(waiter);
    subscription.Reset();
    if (result != WaitResult::kNotified) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
        return false;
    }
    return true;
}

bool Mutex::LockFor(ContextDuration timeout, const ContextPtr& parent) {
    if (parent && parent->IsDone()) {
        return false;
    }
    // A zero/negative timed lock still gets the standard immediate try-lock
    // opportunity. Routing it through an already-cancelled Context would
    // incorrectly fail even when the mutex is free.
    if (timeout <= ContextDuration::zero()) {
        return TryLock();
    }
    CancelFunc cancel;
    const ContextPtr context = TimeoutContext(timeout, parent, &cancel);
    CancelGuard cancel_guard(std::move(cancel));
    return Lock(context);
}

bool Mutex::TryLock() noexcept {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    if (m_impl->m_locked) {
        return false;
    }
    m_impl->m_locked = true;
    return true;
}

void Mutex::Unlock() {
    std::shared_ptr<WaitNode> selected;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (!m_impl->m_locked) {
            throw std::logic_error("go2cpp::sync::Mutex unlock of unlocked mutex");
        }
        while (!m_impl->m_waiters.empty()) {
            auto candidate = std::move(m_impl->m_waiters.front());
            m_impl->m_waiters.pop_front();
            if (candidate &&
                candidate->TryFinish(WaitResult::kNotified)) {
                selected = std::move(candidate);
                break;
            }
        }
        if (!selected) {
            m_impl->m_locked = false;
        }
        // With a selected waiter the lock remains logically held: ownership
        // is handed directly to the FIFO head, so a TryLock caller cannot barge.
    }
    if (selected) {
        selected->Wake();
    }
}

void Mutex::lock() {
    if (!Lock()) {
        throw std::logic_error(
            "go2cpp::sync::Mutex cannot block outside a runnable managed G");
    }
}

struct ConditionVariable::Impl {
    std::mutex m_mutex;
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

ConditionVariable::ConditionVariable() : m_impl(std::make_unique<Impl>()) {}
ConditionVariable::~ConditionVariable() = default;

bool ConditionVariable::Wait(Mutex& mutex, const ContextPtr& context) {
    if (context && context->IsDone()) {
        return false;
    }
    const WaitTarget target = CurrentTarget();
    if (!target || target.task->cancellation_requested()) {
        return false;
    }

    const auto waiter =
        std::make_shared<WaitNode>(target.scheduler, target.task);
    ContextSubscription subscription(context, waiter);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if ((context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting) {
            return false;
        }
        m_impl->m_waiters.push_back(waiter);
        if (!waiter->Arm()) {
            m_impl->m_waiters.pop_back();
            return false;
        }
    }

    try {
        // Publication precedes unlock. A concurrent Notify may run here, but
        // Scheduler pending-wake semantics make the subsequent park lossless.
        mutex.Unlock();
    } catch (...) {
        (void)waiter->TryFinish(WaitResult::kCancelled);
        waiter->Disarm();
        subscription.Reset();
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
        throw;
    }

    const WaitResult result = Await(waiter);
    subscription.Reset();
    {
        // Context-cancelled nodes are removed by their owner. Notify paths
        // detach their winners first, so this is also safe after notification.
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
    }

    // Cancellation of the Context does not waive the condition-variable
    // relock rule. Scheduler shutdown is different: Mutex::Lock refuses to
    // re-park a cancellation-requested G, but may still acquire immediately.
    const bool relocked = mutex.Lock();
    return relocked && result == WaitResult::kNotified;
}

bool ConditionVariable::WaitFor(Mutex& mutex, ContextDuration timeout,
                                const ContextPtr& parent) {
    CancelFunc cancel;
    const ContextPtr context = TimeoutContext(timeout, parent, &cancel);
    CancelGuard cancel_guard(std::move(cancel));
    return Wait(mutex, context);
}

void ConditionVariable::NotifyOne() noexcept {
    std::shared_ptr<WaitNode> selected;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        while (!m_impl->m_waiters.empty()) {
            auto candidate = std::move(m_impl->m_waiters.front());
            m_impl->m_waiters.pop_front();
            if (candidate &&
                candidate->TryFinish(WaitResult::kNotified)) {
                selected = std::move(candidate);
                break;
            }
        }
    }
    if (selected) {
        selected->Wake();
    }
}

void ConditionVariable::NotifyAll() noexcept {
    std::deque<std::shared_ptr<WaitNode>> waiters;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        waiters.swap(m_impl->m_waiters);
    }
    for (const auto& waiter : waiters) {
        if (waiter && waiter->TryFinish(WaitResult::kNotified)) {
            waiter->Wake();
        }
    }
}

struct WaitGroup::Impl {
    mutable std::mutex m_mutex;
    std::int64_t m_count{0};
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

WaitGroup::WaitGroup() : m_impl(std::make_unique<Impl>()) {}
WaitGroup::~WaitGroup() = default;

void WaitGroup::Add(std::int64_t delta) {
    std::deque<std::shared_ptr<WaitNode>> released;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (delta < 0 &&
            (delta == std::numeric_limits<std::int64_t>::min() ||
             m_impl->m_count < -delta)) {
            throw std::logic_error("go2cpp::sync::WaitGroup negative counter");
        }
        if (delta > 0 &&
            m_impl->m_count >
                std::numeric_limits<std::int64_t>::max() - delta) {
            throw std::logic_error("go2cpp::sync::WaitGroup counter overflow");
        }
        m_impl->m_count += delta;
        if (m_impl->m_count == 0) {
            for (const auto& waiter : m_impl->m_waiters) {
                if (waiter) {
                    (void)waiter->TryFinish(WaitResult::kNotified);
                }
            }
            released.swap(m_impl->m_waiters);
        }
    }

    // Mark the old wave while its zero transition is locked. A later Add
    // cannot change those results; arbitrary scheduler work stays outside.
    for (const auto& waiter : released) {
        if (waiter && waiter->result() == WaitResult::kNotified) {
            waiter->Wake();
        }
    }
}

void WaitGroup::Done() { Add(-1); }

bool WaitGroup::Wait(const ContextPtr& context) {
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_count == 0) {
            return true;
        }
    }
    if (context && context->IsDone()) {
        return false;
    }

    const WaitTarget target = CurrentTarget();
    if (!target || target.task->cancellation_requested()) {
        return false;
    }
    const auto waiter =
        std::make_shared<WaitNode>(target.scheduler, target.task);
    ContextSubscription subscription(context, waiter);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_count == 0) {
            return true;
        }
        if ((context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting) {
            return false;
        }
        m_impl->m_waiters.push_back(waiter);
        if (!waiter->Arm()) {
            m_impl->m_waiters.pop_back();
            return false;
        }
    }

    const WaitResult result = Await(waiter);
    subscription.Reset();
    if (result != WaitResult::kNotified) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
        return false;
    }
    return true;
}

bool WaitGroup::WaitFor(ContextDuration timeout, const ContextPtr& parent) {
    CancelFunc cancel;
    const ContextPtr context = TimeoutContext(timeout, parent, &cancel);
    CancelGuard cancel_guard(std::move(cancel));
    return Wait(context);
}

std::int64_t WaitGroup::Count() const noexcept {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_count;
}

}  // namespace go2cpp::sync
