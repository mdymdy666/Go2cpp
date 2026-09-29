#include "go2cpp/sync.hpp"

#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

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
        if (!m_active) {
            return;
        }
        try {
            if (m_scheduler != nullptr && m_task) {
                // A managed waiter resumes through its owning scheduler.
                // Scheduler::wake_or_cancel also retains started Gs when
                // shutdown or queue allocation races this callback.
                (void)m_scheduler->wake(m_task);
            } else {
                // Native callers wait on this node's private condition
                // variable. The result is the predicate, so a notify that
                // arrives before wait() is not lost.
                m_native_condition.notify_one();
            }
        } catch (...) {
            // Scheduler's reliable wake handoff retains every started G.
        }
    }

    bool managed() const noexcept {
        return m_scheduler != nullptr && static_cast<bool>(m_task);
    }

    WaitResult WaitNative() noexcept {
        try {
            std::unique_lock<std::mutex> lock(m_native_mutex);
            m_native_condition.wait(lock, [this] {
                return result() != WaitResult::kWaiting;
            });
        } catch (...) {
            // condition_variable may report an implementation/system error.
            // Never let that escape the runtime wait boundary (which is
            // noexcept because scheduler and cancellation callbacks are
            // advisory); convert it to the same terminal path as cancellation.
            (void)TryFinish(WaitResult::kCancelled);
        }
        return result();
    }

    const std::shared_ptr<Task>& task() const noexcept { return m_task; }
    Scheduler* scheduler() const noexcept { return m_scheduler; }

    void Reset(Scheduler* scheduler, std::shared_ptr<Task> task) noexcept {
        std::lock_guard<std::mutex> lock(m_wake_mutex);
        m_active = false;
        m_scheduler = scheduler;
        m_task = std::move(task);
        m_result.store(WaitResult::kWaiting, std::memory_order_release);
    }

private:
    Scheduler* m_scheduler{nullptr};
    std::shared_ptr<Task> m_task;
    std::atomic<WaitResult> m_result{WaitResult::kWaiting};
    std::mutex m_wake_mutex;
    std::mutex m_native_mutex;
    std::condition_variable m_native_condition;
    bool m_active{false};
};

// WaitNode 只在等待期间被队列或取消回调引用。完成一次等待后将对象放回
// 当前 M 的小缓存，避免混合 Mutex 在短临界区竞争时反复分配控制块和
// condition_variable。缓存节点已清空 scheduler/task，不延长用户对象生命期。
thread_local std::vector<std::shared_ptr<WaitNode>> t_wait_node_cache;

std::shared_ptr<WaitNode> AcquireWaitNode(Scheduler* scheduler,
                                          std::shared_ptr<Task> task) {
    if (!t_wait_node_cache.empty()) {
        auto waiter = std::move(t_wait_node_cache.back());
        t_wait_node_cache.pop_back();
        waiter->Reset(scheduler, std::move(task));
        return waiter;
    }
    return std::make_shared<WaitNode>(scheduler, std::move(task));
}

void ReleaseWaitNode(std::shared_ptr<WaitNode> waiter) noexcept {
    if (!waiter) {
        return;
    }
    waiter->Reset(nullptr, {});
    if (t_wait_node_cache.size() < 32U) {
        t_wait_node_cache.push_back(std::move(waiter));
    }
}

struct WaitTarget {
    Scheduler* scheduler{nullptr};
    std::shared_ptr<Task> task;
    bool unsupported_manual_fiber{false};

    explicit operator bool() const noexcept {
        return scheduler != nullptr && static_cast<bool>(task);
    }
};

WaitTarget CurrentTarget() noexcept {
    Scheduler* const scheduler = Scheduler::current_scheduler();
    auto task = Scheduler::current_task();
    const bool manual_fiber = Fiber::Current() != nullptr &&
                              (scheduler == nullptr || !task);
    return {scheduler, std::move(task), manual_fiber};
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
    if (!waiter) {
        return WaitResult::kCancelled;
    }

    const auto finish = [&waiter](WaitResult result) {
        waiter->Disarm();
        return result;
    };
    if (!waiter->managed()) {
        // Native callers consume the same result state but sleep on a
        // condition_variable; this keeps a blocked OS thread out of the GMP
        // worker pool while preserving notify-before-wait semantics.
        return finish(waiter->WaitNative());
    }

    // A managed G never waits on a native condition_variable: parking releases
    // its M and the scheduler wake path requeues the same logical G.
    Scheduler* const scheduler = waiter->scheduler();
    const auto& task = waiter->task();
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

// ConditionVariable::Wait is specified with the same precondition as
// std::condition_variable::wait: the caller owns the mutex. Even an already
// cancelled context or a zero timeout must perform the atomic-looking
// unlock/relock boundary so another waiter can make progress and the caller
// regains its lock before observing the false result. Mutex::Lock may decline
// to re-park a G during scheduler shutdown; in that case the documented result
// is false with the mutex left unlocked.
bool AbortConditionWait(Mutex& mutex, bool preserve_lock = false) {
    // A manually resumed Fiber has no scheduler continuation. Unlocking and
    // then blocking on relock could still park the carrier thread if another
    // owner wins the race, so this explicitly constrained path keeps the
    // caller's lock and reports false.
    if (preserve_lock) {
        return false;
    }
    mutex.Unlock();
    (void)mutex.Lock();
    return false;
}

}  // namespace

struct Mutex::Impl {
    std::mutex m_mutex;
    // 无竞争路径只需一次 CAS，不再为每个 Fiber 的 Lock/Unlock 获取
    // 慢路径互斥量；有等待者时仍由 m_mutex 串行化 FIFO handoff。
    std::atomic<bool> m_fast_locked{false};
    std::atomic<bool> m_has_waiters{false};
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

Mutex::Mutex() : m_impl(std::make_unique<Impl>()) {}
Mutex::~Mutex() = default;

bool Mutex::Lock(const ContextPtr& context) {
    if (context && context->IsDone()) {
        return false;
    }

    bool expected = false;
    if (!m_impl->m_has_waiters.load(std::memory_order_acquire) &&
        m_impl->m_fast_locked.compare_exchange_strong(
            expected, true, std::memory_order_acquire,
            std::memory_order_relaxed)) {
        return true;
    }

    const WaitTarget target = CurrentTarget();
    if (target.unsupported_manual_fiber) {
        // A manually resumed Fiber has no scheduler continuation to park.
        // Returning false is safer than blocking its carrier thread.
        return false;
    }
    if (target && target.task->cancellation_requested()) {
        return false;
    }

    // 混合并发下，短临界区通常只需要等待当前 Fiber 完成一小段工作。
    // 先让受调度的 G 协作式让出执行权，避免为每一次短暂竞争分配
    // WaitNode、注册取消回调并进入 Scheduler::park 的慢路径。只在没有
    // 已发布等待者时使用该路径；一旦形成等待队列，仍由 FIFO handoff
    // 保证公平和跨线程唤醒语义。单 P 调度器不启用重试，因此保留严格
    // 的发布顺序；多 P 场景只在尚未发布等待节点时走该短路径。
    if (target && target.scheduler->processor_count() > 1 &&
        !m_impl->m_has_waiters.load(std::memory_order_acquire)) {
        constexpr int kCooperativeAttempts = 16;
        for (int attempt = 0; attempt < kCooperativeAttempts; ++attempt) {
            if (m_impl->m_has_waiters.load(std::memory_order_acquire)) {
                break;
            }
            expected = false;
            if (m_impl->m_fast_locked.compare_exchange_weak(
                    expected, true, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return true;
            }
            if (target.task->cancellation_requested() ||
                !target.scheduler->yield_current()) {
                break;
            }
        }
    }
    const auto waiter = AcquireWaitNode(target.scheduler, target.task);
    ContextSubscription subscription(context, waiter);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if ((context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting) {
            subscription.Reset();
            ReleaseWaitNode(waiter);
            return false;
        }
        expected = false;
        if (m_impl->m_fast_locked.compare_exchange_strong(
                expected, true, std::memory_order_acquire,
                std::memory_order_relaxed)) {
            subscription.Reset();
            ReleaseWaitNode(waiter);
            return true;
        }
        m_impl->m_waiters.push_back(waiter);
        m_impl->m_has_waiters.store(true, std::memory_order_release);
        if (!waiter->Arm()) {
            m_impl->m_waiters.pop_back();
            m_impl->m_has_waiters.store(!m_impl->m_waiters.empty(),
                                        std::memory_order_release);
            subscription.Reset();
            ReleaseWaitNode(waiter);
            return false;
        }
    }

    const WaitResult result = Await(waiter);
    subscription.Reset();
    if (result != WaitResult::kNotified) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
        m_impl->m_has_waiters.store(!m_impl->m_waiters.empty(),
                                    std::memory_order_release);
        ReleaseWaitNode(waiter);
        return false;
    }
    ReleaseWaitNode(waiter);
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
    if (m_impl->m_has_waiters.load(std::memory_order_acquire)) {
        return false;
    }
    bool expected = false;
    return m_impl->m_fast_locked.compare_exchange_strong(
        expected, true, std::memory_order_acquire,
        std::memory_order_relaxed);
}

void Mutex::Unlock() {
    if (!m_impl->m_has_waiters.load(std::memory_order_acquire)) {
        bool expected = true;
        if (m_impl->m_fast_locked.compare_exchange_strong(
                expected, false, std::memory_order_release,
                std::memory_order_relaxed)) {
            return;
        }
    }
    std::shared_ptr<WaitNode> selected;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (!m_impl->m_fast_locked.load(std::memory_order_acquire)) {
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
            m_impl->m_has_waiters.store(false, std::memory_order_release);
            m_impl->m_fast_locked.store(false, std::memory_order_release);
        } else {
            m_impl->m_has_waiters.store(!m_impl->m_waiters.empty(),
                                        std::memory_order_release);
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
            "go2cpp::sync::Mutex lock was cancelled or scheduler unavailable");
    }
}

struct ConditionVariable::Impl {
    std::mutex m_mutex;
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

ConditionVariable::ConditionVariable() : m_impl(std::make_unique<Impl>()) {}
ConditionVariable::~ConditionVariable() = default;

bool ConditionVariable::Wait(Mutex& mutex, const ContextPtr& context) {
    // 必须先识别手动 Fiber。即使 Context 已取消，也不能走普通线程的
    // unlock/relock 路径，否则竞争中的重锁会阻塞 carrier 线程。
    const WaitTarget target = CurrentTarget();
    if (target.unsupported_manual_fiber) {
        return AbortConditionWait(mutex, true);
    }
    if (context && context->IsDone()) {
        return AbortConditionWait(mutex);
    }
    if (target && target.task->cancellation_requested()) {
        return AbortConditionWait(mutex);
    }

    const auto waiter =
        std::make_shared<WaitNode>(target.scheduler, target.task);
    ContextSubscription subscription(context, waiter);
    bool abort_before_publish = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        abort_before_publish =
            (context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting;
        if (!abort_before_publish) {
            m_impl->m_waiters.push_back(waiter);
            if (!waiter->Arm()) {
                m_impl->m_waiters.pop_back();
                abort_before_publish = true;
            }
        }
    }
    if (abort_before_publish) {
        // Do this after releasing the CV registry lock. Unlock can wake a
        // waiter that calls NotifyOne, which must be able to inspect the
        // registry without a lock inversion.
        return AbortConditionWait(mutex);
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
    if (target.unsupported_manual_fiber) {
        return false;
    }
    if (target && target.task->cancellation_requested()) {
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
