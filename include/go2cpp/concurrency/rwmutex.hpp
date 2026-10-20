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
 * 4. 读锁可以重入（同一执行流拿两次读锁 = 两个读者名额，必须 `RUnlock` 两次），
 *    但**仅在没有任何写者等待时安全**。若已有写者排队，再调用 `RLock` /
 *    `RLockFor` 会等写者先完成，而写者又在等调用者释放手上的读锁 —— 自死锁。
 *    持读锁期间如需再取读锁，请用非阻塞的 `TryRLock`。读者没有 owner 记录，
 *    这一点与 Go 的 `RWMutex`（不可递归读）不同，本实现是放宽而非收紧。
 * 5. 对象必须长于所有使用者和等待者；销毁一把仍被持有 / 有等待者的锁是未
 *    定义行为。
 */
#pragma once

#include "go2cpp/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <thread>

namespace go2cpp::sync {

/**
 * @brief 写者优先的读写锁。
 *
 * @details
 * 状态机只有三种形态：**无锁**、**N 个读者**、**1 个写者**。所有状态都受
 * `m_state`（`sync::Mutex`）保护；读者与写者分别挂在 `m_ready` /
 * `m_writer_ready` 两个 `sync::ConditionVariable` 上，按角色唤醒。
 *
 * ## 唤醒策略（与旧实现的区别）
 *
 * 旧实现每次 `Unlock` 都无条件 `NotifyAll`，把读者和写者一起唤醒：被唤醒的
 * 读者看到 pending writer 又回去睡，既制造 futex 惊群，又让 0 等待者也付一次
 * 「构造临时通知批次」的堆分配。现在：
 *
 * - 0 个等待者：完全短路，不构造通知批次、不进条件变量内部互斥量；
 * - 有写者排队：只唤醒写者（写者优先），读者继续睡；
 * - 写者多于一个：广播唤醒（只有队首能前进，其余在 `m_state` 上排队流水
 *   推进，避免单播把每次交接串行化成一个 futex 唤醒延迟）；
 * - 没有写者：才放行全部排队读者。
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
            m_waiting_readers.fetch_add(1, std::memory_order_relaxed);
            bool notified = false;
            try {
                notified = m_ready.Wait(m_state);
            } catch (const std::runtime_error&) {
                // D3：Wait 抛 runtime_error 表示它已释放 m_state 却没能重锁
                // （调度器关闭）。此时**不持有**内部锁，不能 Unlock；先尽力
                // 取回 m_state 再清理，取不回则带着明确异常退出（绝不 UB）。
                m_waiting_readers.fetch_sub(1, std::memory_order_relaxed);
                AbortAfterLockLoss(false, 0);
            }
            m_waiting_readers.fetch_sub(1, std::memory_order_relaxed);
            if (!notified) {
                // Wait 返回 false ⇒ 契约保证内部锁仍被持有（取消路径）。
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
            // 只有队首写者能立刻前进：只交接写者，读者继续保持等待。
            WakeWriterLocked();
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
        m_waiting_writers.push_back(ticket);
        while (!WriterCanAcquireLocked(ticket)) {
            bool notified = false;
            try {
                notified = m_writer_ready.Wait(m_state);
            } catch (const std::runtime_error&) {
                // D3：同上，内部锁已丢失；票据必须在能持锁时撤销。
                AbortAfterLockLoss(true, ticket);
            }
            if (!notified) {
                const bool was_head = !m_waiting_writers.empty() &&
                                      m_waiting_writers.front() == ticket;
                RemoveWaitingWriterLocked(ticket);
                RepairWakeAfterWriteCancelLocked(was_head);
                m_state.Unlock();
                throw std::logic_error(
                    "go2cpp::sync::RWMutex Lock aborted (G cancelled or "
                    "scheduler shut down)");
            }
        }
        // 放行时该票据必然是队首（`WriterCanAcquireLocked` 已判定）。
        m_waiting_writers.pop_front();
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
        // 按角色唤醒：有写者排队时只唤醒写者（写者优先，读者继续睡）；
        // 没有写者时才放行读者；两者都没有时完全短路 —— 不构造 NotifyAll 的
        // 临时批次，也不进条件变量内部互斥量。
        if (!m_waiting_writers.empty()) {
            WakeWriterLocked();
        } else {
            WakeReadersLocked();
        }
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
            bool notified = false;
            try {
                notified = m_ready.WaitFor(m_state, deadline - now);
            } catch (const std::runtime_error&) {
                AbortAfterLockLoss(false, 0);
            }
            if (notified) {
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
        m_waiting_writers.push_back(ticket);
        for (;;) {
            if (WriterCanAcquireLocked(ticket)) {
                m_waiting_writers.pop_front();
                m_writing = true;
                m_state.Unlock();
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                const bool was_head = !m_waiting_writers.empty() &&
                                      m_waiting_writers.front() == ticket;
                RemoveWaitingWriterLocked(ticket);
                // 撤销的可能是队首票据：把唤醒交接给下一个写者。
                RepairWakeAfterWriteCancelLocked(was_head);
                m_state.Unlock();
                return false;
            }
            bool notified = false;
            try {
                notified = m_writer_ready.WaitFor(m_state, deadline - now);
            } catch (const std::runtime_error&) {
                AbortAfterLockLoss(true, ticket);
            }
            if (notified) {
                continue;
            }
            if (WriterCanAcquireLocked(ticket)) {
                m_waiting_writers.pop_front();
                m_writing = true;
                m_state.Unlock();
                return true;
            }
            const bool was_head = !m_waiting_writers.empty() &&
                                  m_waiting_writers.front() == ticket;
            RemoveWaitingWriterLocked(ticket);
            RepairWakeAfterWriteCancelLocked(was_head);
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
    /// @brief 内部锁丢失后，用不 park 的 TryLock 抢回 m_state 的预算。
    static constexpr std::size_t kLockRecoveryAttempts = 8192;

    /// @brief `ConditionVariable::Wait` 报告「没能重新持有内部锁」后的收尾。
    /// @details 正常路径下 Wait 返回 false 时内部锁仍被持有；唯一的例外是
    ///          调度器关闭导致重锁失败——`sync::ConditionVariable::Wait`
    ///          以 `std::runtime_error` 报告，并且**不持有**外部 mutex。
    ///          此时绝不能 `m_state.Unlock()`（会对未持有的锁抛异常），也
    ///          不能在无锁状态下修改 `m_waiting_writers`（数据竞争/UB）。
    ///          本函数先用 `TryLock` 在有限预算内抢回内部锁：m_state 的
    ///          临界区只做字段更新（不 park、不做 IO），正常几次即可取回。
    ///          取回后撤销写者票据并按需交接唤醒；取不回则保留票据、抛出
    ///          带明确诊断的异常——错误可观测，而不是静默 UB。
    [[noreturn]] void AbortAfterLockLoss(bool has_writer_ticket,
                                         std::uint64_t writer_ticket) {
        const bool recovered = RecoverInternalLock();
        if (recovered && has_writer_ticket) {
            const bool was_head = !m_waiting_writers.empty() &&
                                  m_waiting_writers.front() == writer_ticket;
            RemoveWaitingWriterLocked(writer_ticket);
            RepairWakeAfterWriteCancelLocked(was_head);
        }
        if (recovered) {
            m_state.Unlock();
        }
        throw std::logic_error(
            recovered
                ? "go2cpp::sync::RWMutex wait aborted (internal mutex was "
                  "lost while waiting; recovered)"
                : "go2cpp::sync::RWMutex wait aborted (internal mutex was "
                  "lost while waiting and could not be recovered)");
    }

    /// @brief 在有限预算内用 `TryLock` 抢回 m_state（不进入等待队列）。
    bool RecoverInternalLock() noexcept {
        for (std::size_t attempt = 0; attempt < kLockRecoveryAttempts;
             ++attempt) {
            if (m_state.TryLock()) {
                return true;
            }
            if ((attempt & 63U) == 63U) {
                std::this_thread::yield();
            }
        }
        return false;
    }

    /// @brief 唤醒写者；调用方必须持有 m_state 且写者队列非空。
    /// @details 只有一个等待写者时单播（唯一等待者就是队首）；多个写者时广播：
    ///          只有队首能前进，但其余写者会一起从 futex 醒来并在 m_state 上
    ///          排队流水推进。实测（4 线程纯写）单播会把每次交接串行化成一个
    ///          futex 唤醒延迟（~30 us/op），广播回到微秒级。与旧实现
    ///          「写者 + 读者一起 NotifyAll」的区别是读者不再被牵连。
    void WakeWriterLocked() noexcept {
        if (m_waiting_writers.size() > 1) {
            m_writer_ready.NotifyAll();
        } else {
            m_writer_ready.NotifyOne();
        }
    }

    /// @brief 放行全部排队读者；无人等待时完全短路（不触碰条件变量）。
    void WakeReadersLocked() noexcept {
        if (m_waiting_readers.load(std::memory_order_relaxed) != 0U) {
            m_ready.NotifyAll();
        }
    }

    /// @brief 撤销写者票据后的唤醒修补。
    /// @details 只有撤销队首票据才会改变其他等待者的可放行性；队列被清空时
    ///          必须放行等待中的读者，否则会留下永久睡眠的读者。
    void RepairWakeAfterWriteCancelLocked(bool removed_head) noexcept {
        if (!removed_head) {
            return;
        }
        if (!m_waiting_writers.empty()) {
            WakeWriterLocked();
        } else {
            WakeReadersLocked();
        }
    }

    /// @brief 从等待队列摘除票据；调用方必须持有 m_state，且票据仍在队列中。
    void RemoveWaitingWriterLocked(std::uint64_t ticket) {
        if (!m_waiting_writers.empty() && m_waiting_writers.front() == ticket) {
            m_waiting_writers.pop_front();
            return;
        }
        const auto it = std::find(m_waiting_writers.begin(),
                                  m_waiting_writers.end(), ticket);
        if (it != m_waiting_writers.end()) {
            m_waiting_writers.erase(it);
        }
    }

    /// @brief 写者放行判定；调用方必须持有 m_state。
    bool WriterCanAcquireLocked(std::uint64_t ticket) const {
        return !m_writing && m_readers == 0 && !m_waiting_writers.empty() &&
               m_waiting_writers.front() == ticket;
    }

    /// 保护下面全部可变状态的内部互斥量（协程 / 线程混合）。
    mutable Mutex m_state;
    /// 读者广播：读者等待「无写者且无等待写者」。
    ConditionVariable m_ready;
    /// 写者广播：每张票据等待「轮到自己」。按角色分成两个队列，避免唤醒到
    /// 注定继续等待的另一角色（惊群）。写者按票据 FIFO 排队，多写者时用广播
    /// 唤醒（见 WakeWriterLocked），所以不依赖条件变量的唤醒顺序。
    ConditionVariable m_writer_ready;

    /// 当前读者数量。
    std::size_t m_readers{0};
    /// 当前阻塞在 m_ready 上的读者数；供唤醒方判断是否值得通知。
    /// 用 atomic 是因为「内部锁丢失」的兜底路径（D3）必须在**未持锁**的
    /// 情况下完成自减，否则就是对受保护字段的数据竞争。
    std::atomic<std::size_t> m_waiting_readers{0};
    /// 当前是否有写者持锁。
    bool m_writing{false};
    /// 下一个写者票据；只在 `m_state` 内递增，用于写者 FIFO。
    std::uint64_t m_next_writer_ticket{0};
    /// 正在等待的写者票据队列（push_back 即有序，front() 即队首）。
    /// 用 deque 而不是 set：票据单调递增，尾部插入天然有序，避免每次 Lock
    /// 的红黑树节点分配与 O(log n) 插入；取消时线性 erase（罕见路径）。
    std::deque<std::uint64_t> m_waiting_writers;
};

}  // namespace go2cpp::sync
