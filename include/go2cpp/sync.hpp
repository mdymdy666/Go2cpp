#pragma once

#include "go2cpp/context.hpp"

#include <cstdint>
#include <memory>

namespace go2cpp::sync {

/**
 * 支持调度器的 FIFO 混合互斥锁。
 *
 * 依赖：Context 提供取消/截止时间，Scheduler 提供 G 的 park/wake；普通
 * 线程竞争时使用原生 condition_variable。两类调用者共享一个 FIFO 交接
 * 队列，因此 Fiber 加锁后可以由其他 Fiber 或 OS 线程解锁。
 * 对上层提供 Lock/TryLock/LockFor 及 BasicLockable 小写接口。对象必须
 * 长于所有使用者和等待者；不支持递归加锁，成功 Lock 必须恰好对应一次
 * Unlock。Unlock 不要求由同一 G/线程调用，采用 Go 风格所有权。
 */
class Mutex final {
public:
    Mutex();
    ~Mutex();

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    // 阻塞直到获得锁或 context 取消；返回 true 表示已持有锁。
    bool Lock(const ContextPtr& context = {});
    // 最多等待 timeout；parent/context 取消或超时返回 false。
    bool LockFor(ContextDuration timeout,
                 const ContextPtr& parent = {});
    // 不阻塞地尝试加锁；成功返回 true。
    bool TryLock() noexcept;
    // 释放锁并按 FIFO 唤醒下一个等待者；未持有锁时报告逻辑错误。
    void Unlock();

    // BasicLockable 兼容拼写。lock() 在普通线程上竞争时阻塞；受管 G
    // 无法继续等待（例如 Scheduler 正在关闭）时抛出 std::logic_error。
    void lock();
    bool try_lock() noexcept { return TryLock(); }
    void unlock() { Unlock(); }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/**
 * 与 sync::Mutex 配套的调度器条件变量。
 *
 * 依赖：Mutex 的解锁/重锁语义、Context 的取消和 Scheduler 的 Fiber park。
 * 对上层提供 Wait/WaitFor、NotifyOne/NotifyAll；受管 G 挂起 Fiber，普通
 * 线程阻塞原生条件变量，双方共享 FIFO 等待队列。正常通知或取消返回前
 * 都会重新获得 mutex。没有 Scheduler 的手动 resume Fiber 不受支持；关闭
 * 阶段如果无法立即重锁，Wait 返回 false 且 mutex 已解锁。
 */
class ConditionVariable final {
public:
    ConditionVariable();
    ~ConditionVariable();

    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;
    ConditionVariable(ConditionVariable&&) = delete;
    ConditionVariable& operator=(ConditionVariable&&) = delete;

    // 释放 mutex 并等待通知/取消，然后重新获得 mutex；返回是否正常
    // 完成重锁，mutex 参数必须由调用方在进入前持有。
    bool Wait(Mutex& mutex, const ContextPtr& context = {});
    // 最多等待 timeout，参数 parent 为可选取消上下文；返回规则同 Wait。
    bool WaitFor(Mutex& mutex, ContextDuration timeout,
                 const ContextPtr& parent = {});
    // 唤醒一个等待者；没有等待者时通知不会累积。
    void NotifyOne() noexcept;
    // 唤醒当前所有等待者。
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
 * 同时支持受管 G 与普通线程的可复用 WaitGroup。
 *
 * 依赖：Context、Scheduler park/wake 和原生 condition_variable。对上层提供
 * Add/Done/Wait/WaitFor/Count；计数归零时唤醒当前波次，之后 Add 可开启新
 * 波次。计数下溢或有符号溢出抛出 std::logic_error，且计数保持不变。
 */
class WaitGroup final {
public:
    WaitGroup();
    ~WaitGroup();

    WaitGroup(const WaitGroup&) = delete;
    WaitGroup& operator=(const WaitGroup&) = delete;
    WaitGroup(WaitGroup&&) = delete;
    WaitGroup& operator=(WaitGroup&&) = delete;

    // 将计数增加 delta；负值不能使计数下溢。
    void Add(std::int64_t delta);
    // 将计数减少一；计数为零时唤醒等待者。
    void Done();
    // 等待计数归零或 context 取消；返回 true 表示归零。
    bool Wait(const ContextPtr& context = {});
    // 最多等待 timeout；parent/context 取消或超时返回 false。
    bool WaitFor(ContextDuration timeout,
                 const ContextPtr& parent = {});
    // 返回当前计数快照；结果仅表示调用时刻，不阻止随后 Add/Done。
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
