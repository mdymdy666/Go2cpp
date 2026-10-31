/**
 * @file sync.go.hpp
 * @brief Go `sync` 包的 C++ 对应物（Mutex / RWMutex / WaitGroup / Once / Cond）。
 *
 * ## 定位
 * `go2cpp::sync` 已提供 Fiber/线程混合的 `Mutex`、`ConditionVariable`、
 * `WaitGroup`，`go2cpp::sync::RWMutex` 提供读写锁。本文件按 Go 的书写习惯做
 * **零成本包装**，并补齐 Go 有、现有实现没有的 `Once` 与 `Cond` 形态：
 *
 * ```go
 * var mu sync.Mutex
 * mu.Lock(); defer mu.Unlock()
 *
 * var wg sync.WaitGroup
 * wg.Add(1); go func() { defer wg.Done(); ... }(); wg.Wait()
 *
 * var once sync.Once
 * once.Do(func() { ... })
 * ```
 *
 * 对应 C++：
 *
 * ```cpp
 * go::Mutex mu; mu.Lock(); ...; mu.Unlock();
 * go::WaitGroup wg; wg.Add(1); ...; wg.Done(); wg.Wait();
 * go::Once once; once.Do([] { ... });
 * ```
 *
 * ## 与 Go 的差异
 * - Go 的 `Lock()` 不返回错误；若底层 Scheduler 已关闭导致无法等待，本封装
 *   抛 `std::logic_error`（与底层一致），并额外提供 `LockCtx` 返回 bool。
 * - `sync.Once.Do` 在 Go 中即使 f panic 也会标记完成；C++ 的 `std::call_once`
 *   在 f 抛异常时**不**标记完成。需要「panic 也算完成」的场景请用 `DoNoThrow`
 *   或在 f 内部捕获。
 * - `Cond` 绑定一个 `Mutex`（与 Go 一致），但等待方仍需要自己检查条件。
 */

#pragma once

#include "go2cpp/concurrency/rwmutex.hpp"
#include "go2cpp/go/error.go.hpp"
#include "go2cpp/go/time.go.hpp"
#include "go2cpp/sync.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace go2cpp::go {

/**
 * @brief 对应 `sync.Mutex`。
 * @details 底层是 Go2Cpp 的 Fiber/线程混合互斥锁；零值可用。
 */
class Mutex final {
public:
    Mutex() = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    /** @brief 对应 `mu.Lock()`；无法等待时抛 `std::logic_error`。 */
    void Lock() {
        if (!m_mutex.Lock()) {
            throw std::logic_error("go::Mutex::Lock: scheduler unavailable");
        }
    }

    /** @brief 带 Context 的加锁；取消/超时返回 false。 */
    bool LockCtx(const ContextPtr& context) {
        return m_mutex.Lock(context);
    }

    /** @brief 对应 `mu.TryLock()`（Go 1.18）。 */
    bool TryLock() noexcept { return m_mutex.TryLock(); }

    /** @brief 对应 `mu.Unlock()`。 */
    void Unlock() { m_mutex.Unlock(); }

    /** @brief 暴露底层 Mutex，供 ConditionVariable 等既有 API 使用。 */
    ::go2cpp::sync::Mutex& Raw() noexcept { return m_mutex; }
    const ::go2cpp::sync::Mutex& Raw() const noexcept { return m_mutex; }

private:
    ::go2cpp::sync::Mutex m_mutex;
};

/**
 * @brief 对应 `sync.RWMutex`。
 * @details 写者优先（有写者等待时新读者排队），与底层实现一致。
 */
class RWMutex final {
public:
    RWMutex() = default;
    RWMutex(const RWMutex&) = delete;
    RWMutex& operator=(const RWMutex&) = delete;

    void RLock() { m_mutex.RLock(); }
    void RUnlock() { m_mutex.RUnlock(); }
    void Lock() { m_mutex.Lock(); }
    void Unlock() { m_mutex.Unlock(); }
    bool TryRLock() noexcept { return m_mutex.TryRLock(); }
    bool TryLock() noexcept { return m_mutex.TryLock(); }

private:
    ::go2cpp::sync::RWMutex m_mutex;
};

/**
 * @brief 对应 `sync.WaitGroup`。
 * @details 支持 Fiber/线程混合等待；计数不能为负。
 */
class WaitGroup final {
public:
    WaitGroup() = default;
    WaitGroup(const WaitGroup&) = delete;
    WaitGroup& operator=(const WaitGroup&) = delete;

    /** @brief 对应 `wg.Add(delta)`。 */
    void Add(std::int64_t delta) { m_group.Add(delta); }
    /** @brief 对应 `wg.Done()`。 */
    void Done() { m_group.Done(); }
    /** @brief 对应 `wg.Wait()`；无法等待时抛 `std::logic_error`。 */
    void Wait() {
        if (!m_group.Wait()) {
            throw std::logic_error("go::WaitGroup::Wait: scheduler unavailable");
        }
    }
    /** @brief 带 Context 的等待；取消/超时返回 false。 */
    bool WaitCtx(const ContextPtr& context) { return m_group.Wait(context); }
    /** @brief 带相对超时的等待。 */
    bool WaitFor(Duration timeout) {
        return m_group.WaitFor(std::chrono::nanoseconds(timeout.Nanoseconds()));
    }
    /** @brief 当前计数快照。 */
    std::int64_t Count() const noexcept { return m_group.Count(); }

private:
    ::go2cpp::sync::WaitGroup m_group;
};

/**
 * @brief 对应 `sync.Once`。
 * @note 与 Go 的 panic 语义差异见文件头。
 */
class Once final {
public:
    Once() = default;
    Once(const Once&) = delete;
    Once& operator=(const Once&) = delete;

    /** @brief 对应 `once.Do(f)`；f 抛异常时不标记完成（std::call_once 语义）。 */
    template <typename Function>
    void Do(Function&& function) {
        std::call_once(m_flag, [this, &function] {
            function();
            m_done.store(true, std::memory_order_release);
        });
    }

    /**
     * @brief `Do` 的「异常也算完成」版本，贴近 Go 的 once 语义。
     * @details 捕获并丢弃异常；若需要记录失败，请在 f 内自行处理。
     */
    template <typename Function>
    void DoNoThrow(Function&& function) noexcept {
        std::call_once(m_flag, [this, &function]() noexcept {
            try {
                function();
            } catch (...) {
                // 与 Go 一致：f panic 后 once 仍视为已完成。
            }
            m_done.store(true, std::memory_order_release);
        });
    }

    /** @brief 是否已经执行过（近似值，仅诊断用）。 */
    bool Done() const noexcept { return m_done.load(std::memory_order_acquire); }

private:
    std::once_flag m_flag;
    std::atomic<bool> m_done{false};
};

/**
 * @brief 对应 `sync.Cond`。
 * @details 与 Go 一样绑定一个 `Mutex`；`Wait` 前必须已持有该锁。
 *          底层 ConditionVariable 支持 Fiber/线程混合等待。
 */
class Cond final {
public:
    /** @brief 创建绑定到 mutex 的条件变量。 */
    explicit Cond(Mutex& mutex) : m_mutex(mutex) {}
    Cond(const Cond&) = delete;
    Cond& operator=(const Cond&) = delete;

    /** @brief 对应 `cond.Wait()`：释放锁并等待，返回前重新加锁。 */
    void Wait() {
        if (!m_condition.Wait(m_mutex.Raw())) {
            throw std::logic_error("go::Cond::Wait: scheduler unavailable");
        }
    }

    /** @brief 带相对超时的等待；返回是否被正常唤醒。 */
    bool WaitFor(Duration timeout) {
        return m_condition.WaitFor(
            m_mutex.Raw(), std::chrono::nanoseconds(timeout.Nanoseconds()));
    }

    /** @brief 对应 `cond.Signal()`。 */
    void Signal() noexcept { m_condition.NotifyOne(); }
    /** @brief 对应 `cond.Broadcast()`。 */
    void Broadcast() noexcept { m_condition.NotifyAll(); }

private:
    Mutex& m_mutex;
    ::go2cpp::sync::ConditionVariable m_condition;
};

}  // namespace go2cpp::go
