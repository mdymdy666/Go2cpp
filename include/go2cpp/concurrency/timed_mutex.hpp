/**
 * @file timed_mutex.hpp
 * @brief 显式超时的互斥锁 `TimedMutex`，语义对齐 `std::timed_mutex`。
 *
 * @date 2026-10-18
 * @author DeepSeek Agent
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 为什么需要它
 *
 * `sync::Mutex` 已经有 `LockFor`，但它的返回值约定是「Context 取消或超时」，
 * 而 Go 风格的锁调用点常常需要一把**与标准库同名、语义直白**的锁：
 * 「等不到就返回 false，不抛、不改状态」，便于在持锁 IO / 跨服务调用前做
 * 兜底，避免一个永久不释放的锁把整个 worker 拖死。
 *
 * `TimedMutex` **完全复用** `sync::Mutex::LockFor`：
 *
 * - 受管 G：超时由 `Context` / TimerService 触发，等待期间 park，让出 M；
 * - 普通 OS 线程：等待期间走原生路径真阻塞，超时后返回 false；
 * - 唤醒交接、FIFO、取消语义全部沿用 `sync::Mutex`，本文件没有一行等待逻辑。
 *
 * ## 语义（与 `std::timed_mutex` 对齐）
 *
 * | 操作 | 行为 |
 * |---|---|
 * | `Lock()` | 无限等待，成功返回 |
 * | `TryLock()` | 不等待，成功返回 true |
 * | `TryLockFor(d)` | 最多等 `d`；成功 true；**超时 false 且锁状态不变** |
 * | `TryLockUntil(tp)` | 最多等到绝对时刻 `tp`；成功 true；超时 false |
 * | `Unlock()` | 释放；未持有时抛 `std::logic_error` |
 *
 * ## 与标准库的差异
 *
 * - `std::timed_mutex` 不允许在错误线程解锁（未定义行为）；这里沿用
 *   `sync::Mutex` 的 Go 风格所有权：任意执行流都可以 `Unlock`，未持有时抛
 *   `std::logic_error` 而不是 UB。
 * - `Lock()` 在调度器关闭 / G 被取消时抛 `std::logic_error`（`std::timed_mutex`
 *   的 `lock()` 不返回失败）。这是协程运行时独有的失败面，不能吞掉。
 * - 没有 `try_lock_for` 等小写拼写；需要 `std::lock_guard` 时请用
 *   `std::unique_lock<TimedMutex>` 并显式调用，或直接用 `Lock()` / `Unlock()`。
 */
#pragma once

#include "go2cpp/sync.hpp"

#include "go2cpp/debug/debug_resources.hpp"

#include <chrono>
#include <stdexcept>

namespace go2cpp::sync {

/**
 * @brief 带超时的互斥锁；底层是 `sync::Mutex`。
 *
 * @details 非递归：同一执行流重复 `Lock()` 会自死锁。需要重入请用
 *          `RecursiveMutex`；需要共享读请用 `RWMutex`。
 */
class TimedMutex final {
public:
    /// @brief 创建未加锁的互斥锁。
    TimedMutex() {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        m_debug_resource =
            GO2CPP_DEBUG_RESOURCE_REGISTER(kTimedMutex, this, nullptr);
#endif
    }

    /**
     * @brief 销毁互斥锁。
     * @warning 调用方必须保证锁已释放且没有等待者。
     */
    ~TimedMutex() {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        GO2CPP_DEBUG_RESOURCE_UNREGISTER(m_debug_resource);
#endif
    }

    TimedMutex(const TimedMutex&) = delete;
    TimedMutex& operator=(const TimedMutex&) = delete;
    TimedMutex(TimedMutex&&) = delete;
    TimedMutex& operator=(TimedMutex&&) = delete;

    /**
     * @brief 无限等待直到获得锁。
     * @throw std::logic_error 调度器不可用或 G 被取消，导致无法继续等待。
     */
    void Lock() {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        GO2CPP_DEBUG_RESOURCE_EVENT(
            kMutexLock, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            ::go2cpp::debug::ResourceResult::kBlocked, 0, true);
#endif
        if (!m_impl.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::TimedMutex lock was cancelled or scheduler "
                "unavailable");
        }
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        GO2CPP_DEBUG_RESOURCE_EVENT(
            kMutexLock, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            ::go2cpp::debug::ResourceResult::kOk, 0, false);
#endif
    }

    /**
     * @brief 释放锁。
     * @throw std::logic_error 当前未持有锁。
     */
    void Unlock() {
        m_impl.Unlock();
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        GO2CPP_DEBUG_RESOURCE_EVENT(
            kMutexUnlock, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            ::go2cpp::debug::ResourceResult::kOk, 0, false);
#endif
    }

    /**
     * @brief 非阻塞尝试加锁。
     * @return 成功返回 true（已持有）；已被持有返回 false。
     */
    bool TryLock() noexcept {
        const bool acquired = m_impl.TryLock();
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        GO2CPP_DEBUG_RESOURCE_EVENT_EX(
            kMutexLock, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            acquired ? ::go2cpp::debug::ResourceResult::kOk
                     : ::go2cpp::debug::ResourceResult::kWouldBlock,
            0, false,
            static_cast<std::uint32_t>(::go2cpp::debug::kResourceFlagTry));
#endif
        return acquired;
    }

    /**
     * @brief 在相对超时内尝试加锁。
     * @param timeout 最长等待时间；`<= 0` 等价于一次 `TryLock`。
     * @return 成功返回 true（已持有）；超时返回 false，且**锁状态未被修改**。
     * @note 受管 G 等待时 park，不阻塞 worker 线程；普通线程真阻塞。
     * @note 先做一次无竞争快速尝试：`sync::Mutex::LockFor` 每次都会创建
     *       TimeoutContext 并向 TimerService 注册定时器（约 10µs），而无竞争
     *       时这一步纯属浪费。std::timed_mutex 也是「先试再注册」的语义。
     */
    bool TryLockFor(std::chrono::milliseconds timeout) {
        if (m_impl.TryLock()) {
            return true;
        }
        if (timeout <= std::chrono::milliseconds::zero()) {
            return false;
        }
        return m_impl.LockFor(timeout);
    }

    /**
     * @brief 在绝对时刻前尝试加锁。
     * @param deadline 截止时刻（`std::chrono::steady_clock`）；已过期时等价于
     *        `TryLock`。
     * @return 成功返回 true（已持有）；超时返回 false，且**锁状态未被修改**。
     */
    bool TryLockUntil(std::chrono::steady_clock::time_point deadline) {
        // 与 TryLockFor 相同的快速路径：无竞争时不注册定时器。
        if (m_impl.TryLock()) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (deadline <= now) {
            return false;
        }
        return m_impl.LockFor(deadline - now);
    }

private:
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    /// @brief 资源级历史编号（构造时注册；OFF 构建不存在该成员）。
    ::go2cpp::debug::ResourceId m_debug_resource{0};
#endif
    /// 底层混合互斥锁；超时、park、FIFO 全部由它提供。
    Mutex m_impl;
};

}  // namespace go2cpp::sync
