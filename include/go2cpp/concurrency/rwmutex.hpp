/**
 * @file rwmutex.hpp
 * @brief 写者优先的读写锁 `RWMutex`（协程 / OS 线程混合等待）。
 *
 * @date 2026-10-18
 * @author DeepSeek Agent
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 为什么需要读写锁
 *
 * `sync::Mutex` 是独占锁。配置热加载、连接池空闲表这类「读远多于写」的数据
 * 结构，用独占锁会把并发读也串行化。Go 标准库提供 `sync.RWMutex`；
 * Go2Cpp 之前只有独占的 `Mutex` / `SpinMutex`，本文件补上读写锁。
 *
 * ## 为什么写者优先
 *
 * 朴素的「读者优先」实现有个经典问题：只要读者持续到来，写者永远排不上队
 * （写饥饿）。本实现采用**写者优先**：一旦有写者在等，后续新读者必须排队，
 * 直到写者拿到并释放锁。代价是**读者可能被写者压制**，这是有意的取舍 ——
 * 写操作通常更短、更少，饿死写者往往意味着配置永远不生效 / 连接池永远不扩容。
 *
 * 写者之间严格 FIFO：每个写者领取一个单调递增的票据，只有票据最小者可以
 * 在「无读者、无写者」时取得锁。读者没有票据，只在「没有写者、也没有等待
 * 中的写者」时批量进入。
 *
 * ## 混合等待是怎么做到的
 *
 * 内部状态由 `sync::Mutex` 保护，等待用 `sync::ConditionVariable`。这两者
 * 本身就是**协程 / OS 线程双路径**：
 *
 * - 受管 G 在 `Wait` 里 park，让出 M（不会阻塞 worker 线程）；
 * - 普通 OS 线程在 `Wait` 里走原生条件变量 / futex（真阻塞）。
 *
 * 本文件不碰 futex、不自建等待队列，避免重复实现已经验证过的唤醒交接逻辑。
 *
 * ## ⚠️ 使用边界
 *
 * 1. **读锁持有者不得再申请写锁**：写锁要求「无任何读者」，而调用者自己就是
 *    读者，必然永久等待（自死锁）。`RWMutex` 无法识别这种升级请求，不提供
 *    `Upgrade`，也不做 owner 检测。
 * 2. **临界区内不要 park**：与 `sync::Mutex` 一样，读锁持有者若因 IO / 阻塞
 *    等待 park，写者会一直等这把读锁；写者优先又会让后续读者也排队，最终把
 *    整个读写锁拖死。需要 IO 的临界区请拆分：锁内只拷贝数据，锁外做 IO。
 * 3. **写锁不可重入**：同一 G 重复 `Lock()` 会自死锁。需要重入请用
 *    `RecursiveMutex`（并阅读它的警告）。
 * 4. 读锁可以重入（同一 G 拿两次读锁 = 两个读者名额，必须 `RUnlock` 两次）。
 *    这不检查 owner，与 Go 的 `RWMutex` 一致：读者没有所有权记录。
 * 5. 对象必须长于所有使用者和等待者；销毁一把仍被持有 / 有等待者的锁是未
 *    定义行为。
 */
#pragma once

#include "go2cpp/sync.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>

namespace go2cpp::sync {

/**
 * @brief 写者优先的读写锁。
 *
 * @details
 * 状态机只有三种形态：**无锁**、**N 个读者**、**1 个写者**。所有状态都受
 * `m_state`（`sync::Mutex`）保护；等待者挂在 `m_ready`
 * （`sync::ConditionVariable`）上。
 *
 * | 请求 | 放行条件 |
 * |---|---|
 * | `RLock` | `!m_writing && m_waiting_writers.empty()` |
 * | `Lock` | `!m_writing && m_readers == 0 && 自己是等待队列票据最小者` |
 * | `TryRLock` | 同 `RLock` 的放行条件，不等待 |
 * | `TryLock` | 同 `Lock` 的放行条件（含「没有等待中的写者」），不等待 |
 *
 * `TryLock` 在「锁空闲但已有写者在排队」时返回 false，这是为了不破坏写者
 * FIFO；`std::shared_mutex` 对 `try_lock` 的公平性不作保证，这里选择更严格
 * 的语义并写进文档。
 *
 * ## 等待路径
 *
 * `Wait` / `WaitFor` 内部把 `m_state` 原子地释放并挂起调用者：
 * Fiber 走 park，OS 线程走原生等待。返回后 `m_state` 重新被持有，循环重新
 * 检查谓词。谓词风格天然免疫虚假唤醒，也不会丢唤醒。
 */
class RWMutex final {
public:
    /// @brief 创建未加锁的读写锁。
    RWMutex() = default;

    /**
     * @brief 销毁读写锁。
     * @warning 调用方必须保证没有读者 / 写者持有，也没有等待者；否则未定义。
     */
    ~RWMutex() = default;

    RWMutex(const RWMutex&) = delete;
    RWMutex& operator=(const RWMutex&) = delete;
    RWMutex(RWMutex&&) = delete;
    RWMutex& operator=(RWMutex&&) = delete;

    /**
     * @brief 获取读锁（共享）。
     * @details 若当前有写者持锁，或**有任何写者在等待**，调用者排队等待。
     * @throw std::logic_error 调度器不可用 / G 被取消导致内部等待无法重锁。
     */
    void RLock() {
        if (!m_state.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::RWMutex RLock: scheduler unavailable");
        }
        while (m_writing || !m_waiting_writers.empty()) {
            if (!m_ready.Wait(m_state)) {
                // Wait 返回 false 时有两种可能：G 被取消（内部锁已重新持有）
                // 或调度器关闭且重锁失败（内部锁不再持有）。公共 API 无法区分，
                // 这里按「已持有」清理；若实际未持有，Unlock 自己会抛
                // logic_error，不会静默破坏状态。
                m_state.Unlock();
                throw std::logic_error(
                    "go2cpp::sync::RWMutex RLock aborted (G cancelled or "
                    "scheduler shut down)");
            }
        }
        ++m_readers;
        m_state.Unlock();
    }

    /**
     * @brief 释放读锁。
     * @details 最后一个读者离开且存在等待写者时，唤醒全部等待者（写者按票据
     *          FIFO 竞争，读者会看到 pending writer 继续排队）。
     * @throw std::logic_error 当前没有读者（重复解锁 / 从未加锁）。
     */
    void RUnlock() {
        if (!m_state.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::RWMutex RUnlock: scheduler unavailable");
        }
        if (m_readers == 0) {
            m_state.Unlock();
            throw std::logic_error(
                "go2cpp::sync::RWMutex RUnlock of unlocked RWMutex");
        }
        --m_readers;
        if (m_readers == 0 && !m_waiting_writers.empty()) {
            m_ready.NotifyAll();
        }
        m_state.Unlock();
    }

    /**
     * @brief 获取写锁（独占），写者之间 FIFO。
     * @details 等待条件：无写者、无读者、且自己是等待队列中票据最小的写者。
     * @throw std::logic_error 调度器不可用 / G 被取消导致内部等待无法重锁。
     */
    void Lock() {
        if (!m_state.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::RWMutex Lock: scheduler unavailable");
        }
        const std::uint64_t ticket = m_next_writer_ticket++;
        m_waiting_writers.insert(ticket);
        while (!WriterCanAcquireLocked(ticket)) {
            if (!m_ready.Wait(m_state)) {
                m_waiting_writers.erase(ticket);
                m_ready.NotifyAll();
                m_state.Unlock();
                throw std::logic_error(
                    "go2cpp::sync::RWMutex Lock aborted (G cancelled or "
                    "scheduler shut down)");
            }
        }
        m_waiting_writers.erase(ticket);
        m_writing = true;
        m_state.Unlock();
    }

    /**
     * @brief 释放写锁并唤醒所有等待者。
     * @throw std::logic_error 当前没有写者（重复解锁 / 从未加锁）。
     */
    void Unlock() {
        if (!m_state.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::RWMutex Unlock: scheduler unavailable");
        }
        if (!m_writing) {
            m_state.Unlock();
            throw std::logic_error(
                "go2cpp::sync::RWMutex Unlock of unlocked RWMutex");
        }
        m_writing = false;
        // 唤醒全部：FIFO 队首写者会立即取得锁；读者看到 pending writer 会继续
        // 排队；后续写者看到队首不是自己也会继续等。一次 NotifyAll 覆盖所有
        // 状态迁移，不需要按角色分别通知。
        m_ready.NotifyAll();
        m_state.Unlock();
    }

    /**
     * @brief 非阻塞尝试获取读锁。
     * @return 成功返回 true（已持有读锁）；有写者持锁或有写者等待时返回 false。
     */
    bool TryRLock() noexcept {
        try {
            if (!m_state.Lock()) {
                return false;
            }
            if (m_writing || !m_waiting_writers.empty()) {
                m_state.Unlock();
                return false;
            }
            ++m_readers;
            m_state.Unlock();
            return true;
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief 非阻塞尝试获取写锁。
     * @return 成功返回 true（已持有写锁）；有读者 / 写者，或已有写者排队时
     *         返回 false。
     */
    bool TryLock() noexcept {
        try {
            if (!m_state.Lock()) {
                return false;
            }
            if (m_writing || m_readers != 0 || !m_waiting_writers.empty()) {
                m_state.Unlock();
                return false;
            }
            m_writing = true;
            m_state.Unlock();
            return true;
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief 在超时内获取读锁。
     * @param timeout 最长等待时间；`<= 0` 等价于一次 `TryRLock`。
     * @return 成功返回 true 且已持有读锁；超时返回 false，且**不改变任何状态**。
     * @throw std::logic_error G 被取消 / 调度器关闭导致等待中止（非超时）。
     */
    bool RLockFor(std::chrono::milliseconds timeout) {
        if (timeout <= std::chrono::milliseconds::zero()) {
            return TryRLock();
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (!m_state.Lock()) {
            return false;
        }
        for (;;) {
            if (!m_writing && m_waiting_writers.empty()) {
                ++m_readers;
                m_state.Unlock();
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                m_state.Unlock();
                return false;
            }
            if (m_ready.WaitFor(m_state, deadline - now)) {
                continue;
            }
            // WaitFor 返回 false：通常是超时（内部锁已重新持有）。但通知可能
            // 恰好与超时同时发生，先再判一次谓词，避免漏掉已经放行的机会。
            if (!m_writing && m_waiting_writers.empty()) {
                ++m_readers;
                m_state.Unlock();
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                m_state.Unlock();
                return false;
            }
            m_state.Unlock();
            throw std::logic_error(
                "go2cpp::sync::RWMutex RLockFor aborted (G cancelled or "
                "scheduler shut down)");
        }
    }

    /**
     * @brief 在超时内获取写锁。
     * @param timeout 最长等待时间；`<= 0` 等价于一次 `TryLock`。
     * @return 成功返回 true 且已持有写锁；超时返回 false，等待票据被撤销，
     *         且**不改变读写状态**。
     * @throw std::logic_error G 被取消 / 调度器关闭导致等待中止（非超时）。
     */
    bool LockFor(std::chrono::milliseconds timeout) {
        if (timeout <= std::chrono::milliseconds::zero()) {
            return TryLock();
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (!m_state.Lock()) {
            return false;
        }
        const std::uint64_t ticket = m_next_writer_ticket++;
        m_waiting_writers.insert(ticket);
        for (;;) {
            if (WriterCanAcquireLocked(ticket)) {
                m_waiting_writers.erase(ticket);
                m_writing = true;
                m_state.Unlock();
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                m_waiting_writers.erase(ticket);
                // 撤销的可能是队首票据：必须唤醒其余等待者重新选队首。
                m_ready.NotifyAll();
                m_state.Unlock();
                return false;
            }
            if (m_ready.WaitFor(m_state, deadline - now)) {
                continue;
            }
            if (WriterCanAcquireLocked(ticket)) {
                m_waiting_writers.erase(ticket);
                m_writing = true;
                m_state.Unlock();
                return true;
            }
            m_waiting_writers.erase(ticket);
            m_ready.NotifyAll();
            if (std::chrono::steady_clock::now() >= deadline) {
                m_state.Unlock();
                return false;
            }
            m_state.Unlock();
            throw std::logic_error(
                "go2cpp::sync::RWMutex LockFor aborted (G cancelled or "
                "scheduler shut down)");
        }
    }

    /**
     * @brief 当前读者数量快照（含调用者自己）。
     * @return 读者数量。诊断用途，只表示调用瞬间的值。
     * @note noexcept：内部锁不可用时返回 0（调度器关闭阶段诊断值无意义）。
     */
    std::size_t ReaderCount() const noexcept {
        try {
            if (!m_state.Lock()) {
                return 0;
            }
            const std::size_t readers = m_readers;
            m_state.Unlock();
            return readers;
        } catch (...) {
            return 0;
        }
    }

    /**
     * @brief 当前是否有写者持有写锁。
     * @return 持有返回 true。诊断用途，只表示调用瞬间的值。
     */
    bool IsWriteLocked() const noexcept {
        try {
            if (!m_state.Lock()) {
                return false;
            }
            const bool writing = m_writing;
            m_state.Unlock();
            return writing;
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief 当前是否有写者在等待队列中（不含已持锁的写者）。
     * @return 有等待写者返回 true。诊断用途，也用于验证写者优先。
     */
    bool HasPendingWriter() const noexcept {
        try {
            if (!m_state.Lock()) {
                return false;
            }
            const bool pending = !m_waiting_writers.empty();
            m_state.Unlock();
            return pending;
        } catch (...) {
            return false;
        }
    }

private:
    /// @brief 写者放行判定；调用方必须持有 `m_state`。
    bool WriterCanAcquireLocked(std::uint64_t ticket) const {
        return !m_writing && m_readers == 0 && !m_waiting_writers.empty() &&
               *m_waiting_writers.begin() == ticket;
    }

    /// 保护下面全部可变状态的内部互斥量（协程 / 线程混合）。
    mutable Mutex m_state;
    /// 状态变化广播：读者等待「无写者且无等待写者」，写者等待「轮到自己」。
    ConditionVariable m_ready;

    /// 当前读者数量。
    std::size_t m_readers{0};
    /// 当前是否有写者持锁。
    bool m_writing{false};
    /// 下一个写者票据；只在 `m_state` 内递增，用于写者 FIFO。
    std::uint64_t m_next_writer_ticket{0};
    /// 正在等待的写者票据集合（有序，`begin()` 即队首）。
    std::set<std::uint64_t> m_waiting_writers;
};

}  // namespace go2cpp::sync
