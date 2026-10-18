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
 * ## 状态机（两个位）
 *
 * 状态字只有两个位，其余值不合法：
 *
 * | 值 | `kLockedBit` | `kWaitersBit` | 含义 |
 * |----|--------------|---------------|------|
 * | 0  | 0 | 0 | 空闲，没有已知等待者 |
 * | 1  | 1 | 0 | 已加锁，没有已知等待者 |
 * | 2  | 0 | 1 | **空闲，但可能仍有线程睡在 futex 上** |
 * | 3  | 1 | 1 | 已加锁，且有线程已登记等待 |
 *
 * 关键不变量：**只要可能有线程睡在 futex 上，`kWaitersBit` 就必须保持置位，
 * 状态就永远不会是 0/1**。这样新来的 `lock()` 不可能用 `CAS(0 -> 1)`
 * 快路径绕过仍在睡眠的等待者 —— 否则它释放时看到 `previous==1` 不会
 * `FUTEX_WAKE`，睡眠者就永久丢失唤醒。
 *
 * ## 为什么 `unlock()` 必须走唤醒链（X-36）
 *
 * 旧实现是 `exchange(0)` 后 `FUTEX_WAKE(1)` 一次即完。它依赖“被唤醒的等待者
 * 一定会接手锁并继续唤醒链”。这个依赖不成立：`FUTEX_WAKE` 只保证“唤醒了
 * 一个”，不保证被唤醒者还能执行。若该等待者已经退出（线程终止、被取消、
 * 或在 `FUTEX_WAIT` 返回后立即死亡），它不会接手锁，队列里剩下的等待者
 * 就永远等不到下一次 `FUTEX_WAKE` —— 锁空闲、`state==0`、却有人睡死。
 *
 * 现在的 `unlock()`：
 *
 * 1. 用一次 `fetch_and(~kLockedBit)` 清锁位（**快路径仍是单次原子操作**）；
 * 2. 若 `kWaitersBit` 已置位，进入唤醒链：每次 `FUTEX_WAKE(1)`；
 * 3. `FUTEX_WAKE` 返回 `0` 表示队列真的空了 → `CAS(2 -> 0)` 清等待者位；
 * 4. 返回 `>0` 但状态里 `kLockedBit` 仍为 0，说明被唤醒者没有接手
 *    （已退出 / 已死），**继续唤醒下一个**；
 * 5. 一旦有人接手（`kLockedBit` 置位），立即停止 —— 它释放时会继续推进；
 * 6. 最多 `kMaxWakeAttempts` 次，超限则保留等待者位，让下一个 `lock()`
 *    走慢路径继续推进（有界，不空转）。
 *
 * 第 4 步是对“唤醒对象已退出”的直接处理；第 3 步的 `FUTEX_WAKE == 0`
 * 则是任务 ① 要求的返回值判定。两者缺一不可：只判返回值时，
 * “唤醒了已退出的等待者”（返回 1）仍会丢唤醒。
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
    /** @brief 销毁门；调用方必须保证析构时无人持有、无人等待。 */
    ~FutexMutex() = default;

    FutexMutex(const FutexMutex&) = delete;
    FutexMutex& operator=(const FutexMutex&) = delete;
    FutexMutex(FutexMutex&&) = delete;
    FutexMutex& operator=(FutexMutex&&) = delete;

    /** @brief 加锁；竞争时在当前线程睡眠，不自旋。 */
    void lock() noexcept {
#if defined(__linux__)
        std::uint32_t expected = kUnlocked;
        if (m_state.compare_exchange_strong(expected, kLockedBit,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
            return;
        }
        // 慢路径：把自己登记为“有等待者”，同时原子地检查是否刚好空闲。
        // exchange 返回的旧值若锁位为 0（0 或 2），本次直接接管；
        // 否则睡到状态变成“已加锁且有等待者”（值为 3）。
        for (;;) {
            const std::uint32_t previous = m_state.exchange(
                kLockedBit | kWaitersBit, std::memory_order_acquire);
            if ((previous & kLockedBit) == 0U) {
                return;
            }
            WaitFutex(kLockedBit | kWaitersBit);
        }
#else
        std::unique_lock<std::mutex> lock(m_fallback_mutex);
        m_fallback_cv.wait(lock, [this] { return !m_fallback_locked; });
        m_fallback_locked = true;
#endif
    }

    /** @brief 释放门并唤醒等待者；可以由任意线程调用。 */
    void unlock() noexcept {
#if defined(__linux__)
        // 一次原子操作清锁位、保留等待者位。无竞争时这就是全部开销，
        // 与旧实现的 exchange(0) 同样是单次 RMW。
        const std::uint32_t previous =
            m_state.fetch_and(~kLockedBit, std::memory_order_release);
        if ((previous & kWaitersBit) == 0U) {
            // 没有已知等待者：不必进内核。previous 为 0 是重复解锁，
            // 保持静默（跨线程所有权语义不记录 owner，无法可靠报告）。
            return;
        }
        // 有等待者：必须把唤醒链推进到“有人接手”或“队列确实为空”。
        for (int attempt = 0; attempt < kMaxWakeAttempts; ++attempt) {
            if (WakeFutex() == 0) {
                // 队列已空：安全地清掉等待者位。用 CAS(2 -> 0) 而不是
                // 无条件 store：期间可能已有新线程 exchange(3) 接管锁
                // （状态变成 3），清成 0 会抹掉它的持锁标记。
                std::uint32_t expected = kWaitersBit;
                m_state.compare_exchange_strong(expected, kUnlocked,
                                                std::memory_order_release,
                                                std::memory_order_relaxed);
                return;
            }
            if ((m_state.load(std::memory_order_acquire) & kLockedBit) != 0U) {
                // 有人已经接手；它的 unlock() 会继续推进唤醒链。
                return;
            }
            // 唤醒成功但无人接手：被唤醒的等待者已经退出（不再执行），
            // 继续唤醒队列里的下一个。
        }
        // 有界退出：保留等待者位。下一个 lock() 走慢路径，
        // 拿到锁后释放时会继续推进这条链，不会永久睡死。
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
        return m_state.compare_exchange_strong(expected, kLockedBit,
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
    /// 锁位：置位表示“当前有持有者”。
    static constexpr std::uint32_t kLockedBit = 1U;
    /// 等待者位：置位表示“可能有线程睡在 futex 上”。
    static constexpr std::uint32_t kWaitersBit = 2U;

    /**
     * @brief 唤醒链的最大尝试次数。
     * @details 每轮对应一次 `FUTEX_WAKE` 系统调用。正常竞争只需 2 轮
     *          （唤醒一个 + 确认队列空）；上限只为“连续多个唤醒对象都已
     *          退出”的退化场景留余量，同时保证 `unlock()` 有界。
     */
    static constexpr int kMaxWakeAttempts = 64;

    /// 睡到状态不再是 `expected`。值已变时立即返回（EAGAIN），
    /// 由调用方重新 exchange，不能把 EAGAIN 当成错误。
    void WaitFutex(std::uint32_t expected) noexcept {
        (void)::syscall(SYS_futex,
                        reinterpret_cast<std::uint32_t*>(&m_state),
                        FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
    }

    /// 唤醒最多一个等待者。返回实际唤醒数：0 表示队列为空。
    int WakeFutex() noexcept {
        return static_cast<int>(
            ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&m_state),
                      FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0));
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
