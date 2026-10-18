/**
 * @file futex_mutex.hpp
 * @brief 最小跨线程互斥门：原子 CAS 快路径 + Linux futex 阻塞慢路径。
 *
 * ## 模块定位
 *
 * 取代已删除的 `HybridGate`（X-35）。它只保护运行时内部的**极短唤醒门**
 * （`WaitNode::{Arm,Disarm,Wake,Reset}`、IO 唤醒节点、`ParkingCondition`
 * 的节点门），临界区只有几条赋值，绝不允许阻塞、执行用户回调或系统调用。
 *
 * ## 语义
 *
 * - **跨线程解锁**：不记录 owner，任何线程都可以 `unlock()`（Go 风格）。
 *   这是 `std::mutex` 做不到的，也是历史 HybridGate 存在的唯一理由。
 * - **无自旋**：竞争时直接 `FUTEX_WAIT` 睡眠，由 `unlock()` 的
 *   `FUTEX_WAKE` 唤醒。热路径无竞争时只有一次 CAS，不进入内核。
 * - **有界**：没有 `for(;;)` 自旋，不会因为持有者被换出而烧 CPU（X-35）。
 *
 * ## 状态机
 *
 * `0` = 空闲，`1` = 已加锁（无等待者），`2` = 已加锁且有等待者。
 * 每次从“空闲/有等待者”状态抢到锁时都把状态写成 `2`，因此下一次
 * `unlock()` 一定会 `FUTEX_WAKE`，不会丢掉唤醒链。等待者从 futex
 * 醒来后重新 `exchange(2)`：返回 0 表示抢到；否则继续睡。
 *
 * ## 非 Linux 回退
 *
 * 没有 futex 的平台使用 `std::mutex + std::condition_variable` 实现同样的
 * “bool 状态 + 跨线程解锁”语义（`std::mutex` 只保护状态，**不是**门的
 * 所有权）。生产目标平台是 Linux；回退分支只保证可编译与语义正确。
 */
#pragma once

#include <atomic>
#include <cstdint>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <condition_variable>
#include <mutex>
#endif

namespace go2cpp::core {

/**
 * @brief 不可递归、可跨线程解锁的最小互斥门。
 * @details 满足 BasicLockable（`lock` / `unlock` / `try_lock`），可直接与
 *          `std::lock_guard` / `std::unique_lock` 组合。析构时要求无人持有。
 */
class FutexMutex final {
public:
    /** @brief 创建未加锁的门。 */
    FutexMutex() noexcept = default;
    /** @brief 销毁门；调用方必须保证析构时无人持有。 */
    ~FutexMutex() = default;

    FutexMutex(const FutexMutex&) = delete;
    FutexMutex& operator=(const FutexMutex&) = delete;
    FutexMutex(FutexMutex&&) = delete;
    FutexMutex& operator=(FutexMutex&&) = delete;

    /** @brief 加锁；竞争时在当前线程睡眠，不自旋。 */
    void lock() noexcept {
#if defined(__linux__)
        std::uint32_t expected = kUnlocked;
        if (m_state.compare_exchange_strong(expected, kLocked,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
            return;
        }
        // 慢路径：把自己登记为"有等待者"，同时原子地检查是否刚好空闲。
        // exchange 返回 0 说明上一任持有者已经释放，本次直接接管。
        while (m_state.exchange(kContended, std::memory_order_acquire) !=
               kUnlocked) {
            WaitFutex();
        }
#else
        std::unique_lock<std::mutex> lock(m_fallback_mutex);
        m_fallback_cv.wait(lock, [this] { return !m_fallback_locked; });
        m_fallback_locked = true;
#endif
    }

    /** @brief 释放门并唤醒一个等待者；可以由任意线程调用。 */
    void unlock() noexcept {
#if defined(__linux__)
        const std::uint32_t previous =
            m_state.exchange(kUnlocked, std::memory_order_release);
        // previous == 2 表示有人可能睡在 futex 上；previous == 1 无等待者，
        // 不必进入内核。previous == 0 是重复解锁，保持静默（跨线程所有权
        // 语义不记录 owner，无法像 sync::Mutex 那样可靠地报告逻辑错误）。
        if (previous == kContended) {
            WakeFutex();
        }
#else
        std::lock_guard<std::mutex> lock(m_fallback_mutex);
        m_fallback_locked = false;
        m_fallback_cv.notify_one();
#endif
    }

    /** @brief BasicLockable 兼容拼写。 */
    void Lock() noexcept { lock(); }
    /** @brief BasicLockable 兼容拼写。 */
    void Unlock() noexcept { unlock(); }

    /** @brief 非阻塞尝试加锁；仅当门完全空闲时成功。 */
    bool try_lock() noexcept {
#if defined(__linux__)
        std::uint32_t expected = kUnlocked;
        return m_state.compare_exchange_strong(expected, kLocked,
                                               std::memory_order_acquire,
                                               std::memory_order_relaxed);
#else
        std::lock_guard<std::mutex> lock(m_fallback_mutex);
        if (m_fallback_locked) {
            return false;
        }
        m_fallback_locked = true;
        return true;
#endif
    }

private:
#if defined(__linux__)
    static constexpr std::uint32_t kUnlocked = 0U;
    static constexpr std::uint32_t kLocked = 1U;
    static constexpr std::uint32_t kContended = 2U;

    /// 睡到状态不再是 kContended。值已变时立即返回（EAGAIN），
    /// 由调用方重新 exchange，不能把 EAGAIN 当成错误。
    void WaitFutex() noexcept {
        (void)::syscall(SYS_futex,
                        reinterpret_cast<std::uint32_t*>(&m_state),
                        FUTEX_WAIT_PRIVATE, kContended, nullptr, nullptr, 0);
    }

    /// 唤醒最多一个等待者。唤醒数可能为 0，不能据此推断等待者数量。
    void WakeFutex() noexcept {
        (void)::syscall(SYS_futex,
                        reinterpret_cast<std::uint32_t*>(&m_state),
                        FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    }

    // 独占一条缓存行：这是唤醒门唯一的共享状态，避免与节点其他字段
    // 产生伪共享。值语义见文件头状态机说明。
    alignas(64) std::atomic<std::uint32_t> m_state{kUnlocked};
#else
    std::mutex m_fallback_mutex;
    std::condition_variable m_fallback_cv;
    bool m_fallback_locked{false};
#endif
};

}  // namespace go2cpp::core
