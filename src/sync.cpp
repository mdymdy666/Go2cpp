#include "go2cpp/sync.hpp"

#include "go2cpp/core/hybrid_mutex.hpp"

#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

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
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
        if (result() != WaitResult::kWaiting) {
            return false;
        }
        m_active = true;
        return true;
    }

    void Disarm() noexcept {
        // A callback may already have left DoneSignal's registry. Waiting for
        // its Wake call here prevents it from outliving Scheduler shutdown.
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
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
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
        WakeLocked();
    }

    // Context 的取消回调可能已经通过 weak_ptr 取得了本节点，随后
    // RemoveCallback 又与节点回收并发。取消、active 检查和唤醒必须在
    // 同一把门锁内完成；否则旧回调可能在节点放回 thread-local cache
    // 后，把下一次等待的 m_result 错误地改成 Cancelled。generation 用来
    // 区分同一个缓存节点的不同等待代次。
    bool TryCancelAndWake(std::uint64_t generation) noexcept {
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
        if (generation != m_generation ||
            !TryFinish(WaitResult::kCancelled)) {
            return false;
        }
        // 取消可能与 Arm() 交错。未 Arm 时只发布终态，发布方会在
        // 入队锁内观察到 Cancelled 并放弃挂起；已 Arm 时才需要实际唤醒。
        if (m_active) {
            WakeLocked();
        }
        return true;
    }

    std::uint64_t generation() noexcept {
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
        return m_generation;
    }

private:
    void WakeLocked() noexcept {
        if (!m_active) {
            return;
        }
        try {
            if (m_scheduler != nullptr && m_task) {
                // A managed waiter resumes through its owning scheduler.
                // 已注册 G 走 wake_registered()；该入口仍保留 started G，
                // 即使 shutdown 或队列分配与这个回调并发。
                (void)m_scheduler->wake_registered(m_task);
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

public:

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
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
        m_active = false;
        m_scheduler = scheduler;
        m_task = std::move(task);
        m_result.store(WaitResult::kWaiting, std::memory_order_release);
        ++m_generation;
        if (m_generation == 0U) {
            m_generation = 1U;
        }
    }

private:
    Scheduler* m_scheduler{nullptr};
    std::shared_ptr<Task> m_task;
    std::atomic<WaitResult> m_result{WaitResult::kWaiting};
    core::HybridMutex m_wake_mutex;
    std::mutex m_native_mutex;
    std::condition_variable m_native_condition;
    bool m_active{false};
    std::uint64_t m_generation{1U};
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

inline void RelaxCpu() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
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
        const std::uint64_t generation = waiter->generation();
        m_id = m_context->Done().AddCallback([weak_waiter, generation] {
            const auto current = weak_waiter.lock();
            if (current) {
                (void)current->TryCancelAndWake(generation);
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

WaitResult Await(WaitNode* waiter) noexcept {
    if (!waiter) {
        return WaitResult::kCancelled;
    }

    const auto finish = [waiter](WaitResult result) {
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
        // Unlock() 可以在 waiter 刚发布、但尚未真正调用 park() 的窗口内
        // 完成交接。此时 Wake 已把结果设为 Notified；若仍然进入
        // park_with_reason()，会无谓获取 Scheduler admission 锁，短临界区
        // 的每次交接都会被放大成一次调度慢路径。先读取终态还能保留
        // notify-before-park 语义：结果已经线性化时无需再次挂起。
        const WaitResult published = waiter->result();
        if (published != WaitResult::kWaiting) {
            return finish(published);
        }
        if (task->cancellation_requested()) {
            if (waiter->result() == WaitResult::kWaiting) {
                (void)waiter->TryFinish(WaitResult::kCancelled);
            }
            return finish(waiter->result());
        }

        // Always attempt park after publication, even if a notifier may have
        // won already. A notify-before-park wake is stored as a pending token;
        // park consumes that token and returns without suspending.
        const bool suspended = scheduler->park_wait(task);
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

template <typename Queue, typename Waiter>
void RemoveWaiter(Queue& waiters, const Waiter& waiter) noexcept {
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
    // 同一个原子字节同时发布“已持有”和“已有等待者/等待者正在入队”
    // 两个状态。Unlock 的 compare_exchange 因此不会读取到旧的等待者
    // 标记后错误地释放锁，避免丢失唤醒。
    static constexpr std::uint8_t kLocked = 1U;
    static constexpr std::uint8_t kWaiter = 2U;
    std::atomic<std::uint8_t> m_state{0U};
    // 只统计已经进入等待队列的普通线程。managed Fiber 在没有 native
    // waiter 时可以采用有界协作重试，避免每次短临界区都 park；一旦有
    // native waiter，所有后续 G 回到 FIFO 队列，保证线程不会饥饿。
    std::atomic<std::size_t> m_native_waiters{0U};
    // 等待节点由 Lock 调用栈或带 Context 的拥有者保存。指针队列避免
    // 无 Context 的短等待反复分配 shared_ptr 控制块，生命周期由等待者
    // 在挂起期间保证。
    std::deque<WaitNode*> m_waiters;
};

Mutex::Mutex() : m_impl(std::make_unique<Impl>()) {}
Mutex::~Mutex() = default;

bool Mutex::Lock(const ContextPtr& context) {
    if (context && context->IsDone()) {
        return false;
    }

    std::uint8_t expected_state = 0U;
    if (m_impl->m_state.compare_exchange_strong(
            expected_state, Impl::kLocked, std::memory_order_acquire,
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
    // 没有 native waiter 时，managed G 走有界协作重试，避免每次短竞争
    // 都进入 WaitNode + park。managed waiter 之间允许有限度地越过队列，
    // 以换取吞吐；一旦有普通线程 waiter，最多再重试 4 次后回到 FIFO
    // handoff，保证线程不会被 Fiber 长时间压住。预算耗尽后仍会进入
    // WaitNode + park，不会无界占用 M。
    if (target && target.scheduler->processor_count() > 1 &&
        m_impl->m_native_waiters.load(std::memory_order_acquire) ==
            0U) {
        // 竞争刚出现时，临界区通常仍在另一个 P 上运行。协作让出可以
        // 避免把短临界区升级为 Fiber 挂起/恢复和跨 M 唤醒；单 P 不走
        // 这条路径，因此不会把唯一 M 忙等住。预算耗尽后再进入 waiter。
        constexpr int kCooperativeAttempts = 64;
        for (int attempt = 0; attempt < kCooperativeAttempts; ++attempt) {
            // native waiter 出现后，重试预算被限制为 4 次，避免 Fiber
            // 无限越过普通线程；这次检查放在每轮 CAS 前。
            if (m_impl->m_native_waiters.load(std::memory_order_acquire) !=
                    0U &&
                attempt >= 4) {
                break;
            }
            expected_state = 0U;
            if (m_impl->m_state.compare_exchange_weak(
                    expected_state, Impl::kLocked,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return true;
            }
            if (target.task->cancellation_requested()) {
                break;
            }
            // 让出当前 G，使持锁者有机会在另一个 M 上完成短临界区。
            // 次数有界，耗尽后进入 FIFO 等待队列，确保 native waiter
            // 不会被无限期阻塞。
            if (!target.scheduler->yield_current()) {
                break;
            }
        }
    }
    // std::mutex 在 Linux 上也会先进行短暂自旋；混合锁的 native
    // 调用者如果立即进入条件变量慢路径，会把很短的临界区放大成一次
    // futex 唤醒。这里保留较小的有界自旋，并且一旦队列发布立即退出，
    // 因而不会越过 FIFO waiter。
    if (!target &&
        (m_impl->m_state.load(std::memory_order_acquire) & Impl::kWaiter) ==
            0U) {
        constexpr int kNativeSpinAttempts = 256;
        for (int attempt = 0; attempt < kNativeSpinAttempts; ++attempt) {
            if ((m_impl->m_state.load(std::memory_order_acquire) &
                 Impl::kWaiter) != 0U) {
                break;
            }
            expected_state = 0U;
            if (m_impl->m_state.compare_exchange_weak(
                    expected_state, Impl::kLocked,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return true;
            }
            RelaxCpu();
        }
    }
    WaitNode stack_waiter(target.scheduler, target.task);
    std::shared_ptr<WaitNode> owned_waiter;
    WaitNode* waiter = &stack_waiter;
    if (context) {
        owned_waiter = AcquireWaitNode(target.scheduler, target.task);
        waiter = owned_waiter.get();
    }
    ContextSubscription subscription(context, owned_waiter);
    const auto release_waiter = [&] {
        subscription.Reset();
        if (owned_waiter) {
            ReleaseWaitNode(std::move(owned_waiter));
        }
    };
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if ((context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting) {
            release_waiter();
            return false;
        }

        // 等待者位只在持有队列锁时发布。这样不会出现“等待者已经设置
        // 标志但尚未入队，Unlock 却覆盖标志”的窗口：Unlock 的无竞争
        // CAS 可以在本段之前把锁释放掉，当前调用随后重新观察到空闲锁
        // 并直接取得；如果 CAS 发生在发布之后，则必然看到 kWaiter 并
        // 进入同一把队列锁完成 FIFO 交接。
        for (;;) {
            expected_state = m_impl->m_state.load(std::memory_order_acquire);
            if ((expected_state & Impl::kLocked) == 0U &&
                m_impl->m_waiters.empty()) {
                if (m_impl->m_state.compare_exchange_weak(
                        expected_state, Impl::kLocked,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    release_waiter();
                    return true;
                }
                continue;
            }

            if ((expected_state & Impl::kWaiter) == 0U) {
                const std::uint8_t published = static_cast<std::uint8_t>(
                    expected_state | Impl::kWaiter);
                if (!m_impl->m_state.compare_exchange_weak(
                        expected_state, published,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    continue;
                }
            }
            break;
        }

        m_impl->m_waiters.push_back(waiter);
        if (!target) {
            m_impl->m_native_waiters.fetch_add(1U, std::memory_order_release);
        }
        if (!waiter->Arm()) {
            m_impl->m_waiters.pop_back();
            if (!target) {
                m_impl->m_native_waiters.fetch_sub(1U,
                                                    std::memory_order_release);
            }
            if (m_impl->m_waiters.empty()) {
                m_impl->m_state.fetch_and(
                    static_cast<std::uint8_t>(~Impl::kWaiter),
                    std::memory_order_release);
            }
            release_waiter();
            return false;
        }
    }

    const WaitResult result = Await(waiter);
    subscription.Reset();
    if (result != WaitResult::kNotified) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        const auto position =
            std::find(m_impl->m_waiters.begin(), m_impl->m_waiters.end(),
                      waiter);
        if (position != m_impl->m_waiters.end()) {
            m_impl->m_waiters.erase(position);
            if (!waiter->managed()) {
                m_impl->m_native_waiters.fetch_sub(
                    1U, std::memory_order_release);
            }
        }
        if (m_impl->m_waiters.empty()) {
            m_impl->m_state.fetch_and(
                static_cast<std::uint8_t>(~Impl::kWaiter),
                std::memory_order_release);
        }
        if (owned_waiter) {
            ReleaseWaitNode(std::move(owned_waiter));
        }
        return false;
    }
    if (owned_waiter) {
        ReleaseWaitNode(std::move(owned_waiter));
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
    std::uint8_t expected = 0U;
    const bool acquired = m_impl->m_state.compare_exchange_strong(
        expected, Impl::kLocked, std::memory_order_acquire,
        std::memory_order_relaxed);
    return acquired;
}

void Mutex::Unlock() {
    // 无等待者时只需一次 CAS，不进入队列互斥量。若等待者位同时存在，
    // CAS 必定失败并转入下面的 FIFO 交接路径。
    std::uint8_t expected_state = Impl::kLocked;
    if (m_impl->m_state.compare_exchange_strong(
            expected_state, 0U, std::memory_order_release,
            std::memory_order_acquire)) {
        return;
    }

    WaitNode* selected = nullptr;
    {
        // Unlock 与 waiter 发布必须共享同一把队列锁。原子快路径无法
        // 同时覆盖 waiter 入队和另一个线程重新取得锁的交错，统一在此
        // 线性化可以避免丢唤醒和“误解锁后来 owner”。
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if ((m_impl->m_state.load(std::memory_order_acquire) &
             Impl::kLocked) == 0U) {
            throw std::logic_error("go2cpp::sync::Mutex unlock of unlocked mutex");
        }
        while (!m_impl->m_waiters.empty()) {
            WaitNode* candidate = m_impl->m_waiters.front();
            m_impl->m_waiters.pop_front();
            if (candidate && !candidate->managed()) {
                m_impl->m_native_waiters.fetch_sub(
                    1U, std::memory_order_release);
            }
            if (candidate && candidate->TryFinish(WaitResult::kNotified)) {
                selected = candidate;
                break;
            }
        }
        if (!selected) {
            m_impl->m_state.store(0U, std::memory_order_release);
        } else {
            m_impl->m_state.store(
                static_cast<std::uint8_t>(Impl::kLocked |
                                           (m_impl->m_waiters.empty()
                                                ? 0U
                                                : Impl::kWaiter)),
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

    const WaitResult result = Await(waiter.get());
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

    const WaitResult result = Await(waiter.get());
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
