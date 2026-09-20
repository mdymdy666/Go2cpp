#pragma once

#include "go2cpp/context.hpp"

#include <cstdint>
#include <memory>

namespace go2cpp::sync {

/**
 * A scheduler-aware FIFO mutex.
 *
 * A managed G suspends through Scheduler::park instead of blocking its M.
 * An unmanaged caller waits on a native condition_variable when the mutex is
 * contended. Both caller classes share one FIFO handoff queue, so a mutex
 * acquired by a Fiber may be released by a different Fiber or OS thread.
 *
 * As with std::mutex, the object must outlive every user and waiter.
 * Ownership is Go-style: Unlock may be called by a different G or thread.
 * Recursive locking is not supported and a successful Lock must be paired
 * with exactly one Unlock.
 */
class Mutex final {
public:
    Mutex();
    ~Mutex();

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    bool Lock(const ContextPtr& context = {});
    bool LockFor(ContextDuration timeout,
                 const ContextPtr& parent = {});
    bool TryLock() noexcept;
    void Unlock();

    // BasicLockable-compatible spellings. lock() blocks a native caller on
    // contention and throws std::logic_error only when a managed G cannot
    // continue waiting (for example during scheduler shutdown).
    void lock();
    bool try_lock() noexcept { return TryLock(); }
    void unlock() { Unlock(); }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/**
 * A scheduler-aware condition variable for sync::Mutex.
 *
 * Managed G waiters park their Fiber while ordinary callers block on a native
 * condition variable. Both kinds can notify one another through the same FIFO
 * queue. Wait always reacquires the mutex before returning from a normal
 * notification or Context cancellation. An already-done Context and a zero or
 * negative WaitFor timeout still perform the unlock/relock boundary. During
 * scheduler shutdown the G is not allowed to park again; if immediate
 * reacquisition is impossible, Wait returns false with the mutex unlocked.
 */
class ConditionVariable final {
public:
    ConditionVariable();
    ~ConditionVariable();

    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;
    ConditionVariable(ConditionVariable&&) = delete;
    ConditionVariable& operator=(ConditionVariable&&) = delete;

    bool Wait(Mutex& mutex, const ContextPtr& context = {});
    bool WaitFor(Mutex& mutex, ContextDuration timeout,
                 const ContextPtr& parent = {});
    void NotifyOne() noexcept;
    void NotifyAll() noexcept;

    bool wait(Mutex& mutex, const ContextPtr& context = {}) {
        return Wait(mutex, context);
    }
    bool wait_for(Mutex& mutex, ContextDuration timeout,
                  const ContextPtr& parent = {}) {
        return WaitFor(mutex, timeout, parent);
    }
    void notify_one() noexcept { NotifyOne(); }
    void notify_all() noexcept { NotifyAll(); }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/**
 * A reusable Go-style wait group for both managed Gs and ordinary threads.
 *
 * Managed waiters park their Fiber; ordinary waiters block on a native
 * condition variable. Each transition to zero releases the waiters from that
 * wave. A later Add
 * starts a new wave even if waiters released from the previous wave have not
 * run yet. Counter underflow and signed overflow throw std::logic_error while
 * leaving the counter unchanged.
 */
class WaitGroup final {
public:
    WaitGroup();
    ~WaitGroup();

    WaitGroup(const WaitGroup&) = delete;
    WaitGroup& operator=(const WaitGroup&) = delete;
    WaitGroup(WaitGroup&&) = delete;
    WaitGroup& operator=(WaitGroup&&) = delete;

    void Add(std::int64_t delta);
    void Done();
    bool Wait(const ContextPtr& context = {});
    bool WaitFor(ContextDuration timeout,
                 const ContextPtr& parent = {});
    std::int64_t Count() const noexcept;

    void add(std::int64_t delta) { Add(delta); }
    void done() { Done(); }
    bool wait(const ContextPtr& context = {}) { return Wait(context); }
    bool wait_for(ContextDuration timeout,
                  const ContextPtr& parent = {}) {
        return WaitFor(timeout, parent);
    }
    std::int64_t count() const noexcept { return Count(); }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::sync
