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

#include "go2cpp/debug/debug_resources.hpp"

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
 * 状态机只有三种形态：**无锁**、**N 个读者**、**1 个写者**。读者门是一个
 * 带符号原子计数（见 `m_reader_count`），写者队列与票据受 `m_state`
 * （`sync::Mutex`）保护；读者等待 `m_ready`，写者 park 时睡在各自登记到
 * 队列节点里的 `sync::ConditionVariable` 上。
 *
 * ## 唤醒策略（与旧实现的区别）
 *
 * 旧实现每次 `Unlock` 都无条件 `NotifyAll`，把读者和写者一起唤醒：被唤醒的
 * 读者看到 pending writer 又回去睡，既制造 futex 惊群，又让 0 等待者也付一次
 * 「构造临时通知批次」的堆分配。现在：
 *
 * - 0 个等待者：完全短路，不构造通知批次、不进条件变量内部互斥量；
 * - 读者快路径：门开着时只做一次原子 `fetch_add`，不碰 `m_state`；
 * - 写者交接：上一任写者把读者门连同交接令牌交给队首，正在自旋的队首
 *   直接登记；只有已经 park 的队首才被**精确 NotifyOne**（不再广播）；
 * - 短临界区：读者 / 写者在 park 前先自旋（写者按时间预算），把
 *   「关门-退读者-开门」的短窗口消化在用户态，避开 futex 往返；
 * - 写者优先：有写者排队时读者门保持关闭，读者继续等，直到写者队列清空。
 *
 * | 请求 | 放行条件 |
 * |---|---|
 * | `RLock` | 读者门开着（`m_reader_count >= 0`）且没有写者在排队 |
 * | `Lock` | 自己是等待队列票据最小者，且已关闭读者门、现有读者全部退出 |
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
private:
    /// @brief 写者等待节点：票据 + 该写者 park 时登记的条件变量。
    /// @details `cv == nullptr` 表示这个写者正在自旋（不在内核里睡），
    ///          交接时不要对它发通知；只有真正 park 的队首才需要 Notify。
    struct WriterWaiter {
        std::uint64_t ticket{0};
        ConditionVariable* cv{nullptr};
    };

public:
    /// @brief 创建未加锁的读写锁。
    RWMutex() {
#if GO2CPP_DEBUG_RESOURCES
        m_debug_resource =
            GO2CPP_DEBUG_RESOURCE_REGISTER(kRwMutex, this, nullptr);
#endif
    }

    /**
     * @brief 销毁读写锁。
     * @warning 调用方必须保证没有读者 / 写者持有，也没有等待者；否则未定义。
     */
    ~RWMutex() {
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_UNREGISTER(m_debug_resource);
#endif
    }

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
        // 快路径：单变量原子 RMW。写者关门用同一个变量做 `fetch_sub`，
        // 因此「读者进入」与「写者关门」有唯一的全局顺序，不存在两个变量
        // 互相看不见的窗口（旧双变量设计正是因此丢唤醒）。
        const std::int64_t previous =
            m_reader_count.fetch_add(1, std::memory_order_acq_rel);
        if (previous >= 0) {
            return;
        }
        // 门已关：回滚并等待写者。回滚可能让计数回到「门关且无读者」，
        // 此时必须补一次写者唤醒，否则写者会睡在一个已经归零的计数上。
        std::int64_t after =
            m_reader_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (after == -kReaderGate) {
            WakeWriterIfWaiting();
        }
        // 门刚关时先做有界自旋：本次优化的目标负载里写者临界区通常只有
        // 几条指令，等一次 futex 唤醒往返（~2.3us）远贵于自旋几百纳秒。
        // 自旋期间只在门开的一瞬间做一次 RMW 登记，失败立即回滚；门持续
        // 关闭（写者临界区长 / 写者排队）才落到 m_state + 条件变量慢路径。
        for (std::size_t spin = 0; spin < kReaderSpin; ++spin) {
            CpuRelax();
            if (m_reader_count.load(std::memory_order_acquire) < 0) {
                continue;
            }
            const std::int64_t acquired =
                m_reader_count.fetch_add(1, std::memory_order_acq_rel);
            if (acquired >= 0) {
                return;
            }
            after =
                m_reader_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (after == -kReaderGate) {
                WakeWriterIfWaiting();
            }
        }
        if (!m_state.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::RWMutex RLock: scheduler unavailable");
        }
        while (m_reader_count.load(std::memory_order_acquire) < 0 ||
               m_writing || !m_waiting_writers.empty()) {
            m_waiting_readers.fetch_add(1, std::memory_order_relaxed);
#if GO2CPP_DEBUG_RESOURCES
            GO2CPP_DEBUG_RESOURCE_EVENT_V5(
                kRwLockRead, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
                ::go2cpp::debug::ResourceResult::kBlocked,
                static_cast<std::uint32_t>(
                    m_waiting_readers.load(std::memory_order_relaxed)),
                true, 0, -1, 0, 0);
#endif
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
        // 持锁时写者无法关门（它也要取 m_state），因此这里的登记必定成功。
        m_reader_count.fetch_add(1, std::memory_order_acq_rel);
        m_state.Unlock();
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kRwLockRead, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            ::go2cpp::debug::ResourceResult::kOk, 0, false, 0, -1, 0, 0);
#endif
    }

    /**
     * @brief 释放读锁。
     * @details 最后一个读者离开且写者门已关时，精确唤醒等待中的写者队首；
     *          门开着时什么都不做（读者快路径不取内部锁）。
     * @throw std::logic_error 当前没有读者（重复解锁 / 从未加锁）。
     */
    void RUnlock() {
        const std::int64_t previous =
            m_reader_count.fetch_sub(1, std::memory_order_acq_rel);
        // 门关时计数整体偏移了 -kReaderGate，这里先还原成真实读者数。
        const std::int64_t readers_before =
            previous >= 0 ? previous : previous + kReaderGate;
        if (readers_before <= 0) {
            // 没有读者可减：重复解锁 / 从未加锁。恢复计数后报错，
            // 保持「抛异常前不改变状态」的既有语义。
            m_reader_count.fetch_add(1, std::memory_order_relaxed);
            throw std::logic_error(
                "go2cpp::sync::RWMutex RUnlock of unlocked RWMutex");
        }
        if (readers_before == 1) {
            // 最后一个读者：写者门若已关，唤醒等待中的写者。
            WakeWriterIfWaiting();
        }
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kRwUnlockRead, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            ::go2cpp::debug::ResourceResult::kOk, 0, false, 0, -1, 0, 0);
#endif
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
        m_waiting_writers.push_back(WriterWaiter{ticket, nullptr});
        // 本写者是否已经持有/关闭了读者门。门在同一时刻只属于一个写者：
        // 上一个写者释放时若还有写者排队，会把门直接交接下来。
        bool gate_closed = false;
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kRwLockWrite, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            ::go2cpp::debug::ResourceResult::kBlocked,
            static_cast<std::uint32_t>(m_waiting_writers.size()), true, 0, -1,
            0, 0);
#endif
        for (;;) {
            // ---- 持 m_state 的判定：队首 + 无写者 ----
            if (!m_writing && !m_waiting_writers.empty() &&
                m_waiting_writers.front().ticket == ticket) {
                if (!gate_closed) {
                    const std::int64_t current =
                        m_reader_count.load(std::memory_order_acquire);
                    if (current >= 0) {
                        // 门开着：队首写者关闭它，然后等剩余读者退出。
                        gate_closed = true;
                        const std::int64_t readers = m_reader_count.fetch_sub(
                            kReaderGate, std::memory_order_acq_rel);
                        if (readers == 0) {
                            AcquireWriteLocked(ticket);
                            return;
                        }
                    } else if (current == -kReaderGate) {
                        // 门已由上一个写者交接下来，且没有读者：直接继承。
                        gate_closed = true;
                        AcquireWriteLocked(ticket);
                        return;
                    } else {
                        // 门关着但仍有读者未退完（门是本写者刚关上的）。
                        gate_closed = true;
                    }
                } else if (m_reader_count.load(std::memory_order_acquire) ==
                           -kReaderGate) {
                    // 最后一批读者已经退出：获取写锁（门保持关闭）。
                    AcquireWriteLocked(ticket);
                    return;
                }
            }
            // ---- 无锁自旋：等交接令牌，或等自己关上的门里读者退完 ----
            // 关键优化：交接/退读者都是“短事件”，自旋命中即可完全避开
            // m_state 争用与 futex 唤醒；只有长时间等不到时才 park。
            m_state.Unlock();
            const auto spin_deadline =
                std::chrono::steady_clock::now() + kWriterSpinBudget;
            std::size_t spin_count = 0;
            for (;;) {
                if (m_write_handoff.load(std::memory_order_acquire) == ticket) {
                    break;
                }
                if (gate_closed &&
                    m_reader_count.load(std::memory_order_acquire) ==
                        -kReaderGate) {
                    break;
                }
                CpuRelax();
                if ((++spin_count & 255U) == 0U &&
                    std::chrono::steady_clock::now() >= spin_deadline) {
                    break;
                }
            }
            if (!m_state.Lock()) {
                // 调度器关闭等极端情况：先把自己关上的门开回去，再尽力
                // 抢回内部锁撤销票据；绝不带着半状态返回。
                if (gate_closed) {
                    m_reader_count.fetch_add(kReaderGate,
                                             std::memory_order_acq_rel);
                }
                if (RecoverInternalLock()) {
                    const bool was_head =
                        !m_waiting_writers.empty() &&
                        m_waiting_writers.front().ticket == ticket;
                    RemoveWaitingWriterLocked(ticket);
                    RepairWakeAfterWriteCancelLocked(was_head);
                    m_state.Unlock();
                }
                throw std::logic_error(
                    "go2cpp::sync::RWMutex Lock: scheduler unavailable");
            }
            // 关键：唤醒条件必须在**重新持有 m_state 之后**再判一次。
            // 自旋阶段看到的 ready 只是提示；交接令牌 / 读者归零可能恰好在
            // 自旋结束与这次 Lock 之间发生，若不再判一次就会把已经发出的
            // Notify 丢掉，然后永远睡下去。
            const std::uint64_t handoff =
                m_write_handoff.load(std::memory_order_acquire);
            // 防御：交接令牌只可能指向队首；若队列已变化则清掉陈旧令牌，
            // 避免“令牌命中但永远轮不到自己”的自旋死循环。
            if (handoff == ticket && !m_waiting_writers.empty() &&
                m_waiting_writers.front().ticket != ticket) {
                m_write_handoff.store(0, std::memory_order_release);
            }
            if ((handoff == ticket && !m_waiting_writers.empty() &&
                 m_waiting_writers.front().ticket == ticket) ||
                (gate_closed &&
                 m_reader_count.load(std::memory_order_acquire) ==
                     -kReaderGate)) {
                continue;
            }
            // ---- park：每个等待者睡在自己的条件变量上 ----
            // 交接时 Unlock 只 Notify 队首的那一个 CV，而不是广播。旧实现
            // 每次交接 NotifyAll，8 线程纯写会在「唤醒-抢锁-再睡」之间形成
            // 惊群（实测单次 op 从 ~1us 恶化到 ~75us）。
            {
                ConditionVariable wait_cv;
                const auto waiter = FindWaitingWriterLocked(ticket);
                if (waiter != m_waiting_writers.end()) {
                    waiter->cv = &wait_cv;
                }
                bool notified = false;
                try {
                    notified = wait_cv.Wait(m_state);
                } catch (const std::runtime_error&) {
                    // D3：内部锁已丢失；门必须在能持锁时开回去，否则读者
                    // 会永久看到「有写者」而饿死。
                    if (gate_closed) {
                        m_reader_count.fetch_add(kReaderGate,
                                                 std::memory_order_acq_rel);
                    }
                    AbortAfterLockLoss(true, ticket);
                }
                const auto after_wake = FindWaitingWriterLocked(ticket);
                if (after_wake != m_waiting_writers.end()) {
                    after_wake->cv = nullptr;
                }
                if (!notified) {
                    if (gate_closed) {
                        m_reader_count.fetch_add(kReaderGate,
                                                 std::memory_order_acq_rel);
                    }
                    const bool was_head = !m_waiting_writers.empty() &&
                                          m_waiting_writers.front().ticket ==
                                              ticket;
                    RemoveWaitingWriterLocked(ticket);
                    RepairWakeAfterWriteCancelLocked(was_head);
                    m_state.Unlock();
                    throw std::logic_error(
                        "go2cpp::sync::RWMutex Lock aborted (G cancelled or "
                        "scheduler shut down)");
                }
            }
        }
    }

    /**
     * @brief 释放写锁；队列非空时把写锁交给队首，否则放行读者。
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
#if GO2CPP_DEBUG_RESOURCES
        const bool woke = !m_waiting_writers.empty() ||
                          m_waiting_readers.load(std::memory_order_relaxed) !=
                              0U;
#endif
        m_writing = false;
        // 写者优先的交接：队列里还有写者时**不开门**，直接把关闭的读者门
        // 交给下一个写者，这样新读者无法在交接窗口插队。交接令牌让下一个
        // 写者即使正在自旋也能立刻看到（不必先被条件变量唤醒）；只有已经
        // park 的写者才需要精确 Notify。队列清空时才开门并放行读者。
        if (!m_waiting_writers.empty()) {
            m_write_handoff.store(m_waiting_writers.front().ticket,
                                  std::memory_order_release);
            WakeFrontWriterLocked();
        } else {
            m_write_handoff.store(0, std::memory_order_release);
            m_reader_count.fetch_add(kReaderGate, std::memory_order_acq_rel);
            WakeReadersLocked();
        }
        m_state.Unlock();
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kRwUnlockWrite, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            woke ? ::go2cpp::debug::ResourceResult::kWake
                 : ::go2cpp::debug::ResourceResult::kOk,
            0, false, 0, -1, 0, 0);
#endif
    }

    /**
     * @brief 非阻塞尝试获取读锁。
     * @return 成功返回 true（已持有读锁）；有写者持锁或有写者等待时返回 false。
     */
    bool TryRLock() noexcept {
        try {
            const std::int64_t previous =
                m_reader_count.fetch_add(1, std::memory_order_acq_rel);
            if (previous >= 0) {
                return true;
            }
            const std::int64_t after =
                m_reader_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (after == -kReaderGate) {
                WakeWriterIfWaiting();
            }
            return false;
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
            if (m_writing || m_reader_count.load(std::memory_order_acquire) !=
                                 0 ||
                !m_waiting_writers.empty()) {
                m_state.Unlock();
                return false;
            }
            // 门开着且无读者：关闭门。读者快路径不需要 m_state，可能在上
            // 面的 load 与这次 fetch_sub 之间抢先登记，因此必须检查返回值：
            // 只要有人抢到，就开回门并失败（TryLock 不等待）。
            const std::int64_t readers =
                m_reader_count.fetch_sub(kReaderGate,
                                         std::memory_order_acq_rel);
            if (readers != 0) {
                m_reader_count.fetch_add(kReaderGate,
                                         std::memory_order_acq_rel);
                m_state.Unlock();
                return false;
            }
            m_writing = true;
            m_write_handoff.store(0, std::memory_order_release);
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
        for (;;) {
            // 快路径复用 TryRLock：门开着时连 m_state 都不碰。
            if (TryRLock()) {
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            if (!m_state.Lock()) {
                return false;
            }
            if (m_reader_count.load(std::memory_order_acquire) >= 0 &&
                !m_writing && m_waiting_writers.empty()) {
                m_reader_count.fetch_add(1, std::memory_order_acq_rel);
                m_state.Unlock();
                return true;
            }
            m_waiting_readers.fetch_add(1, std::memory_order_relaxed);
            bool notified = false;
            try {
                notified = m_ready.WaitFor(m_state, deadline - now);
            } catch (const std::runtime_error&) {
                m_waiting_readers.fetch_sub(1, std::memory_order_relaxed);
                AbortAfterLockLoss(false, 0);
            }
            m_waiting_readers.fetch_sub(1, std::memory_order_relaxed);
            if (notified) {
                m_state.Unlock();
                continue;
            }
            // WaitFor 返回 false：通常是超时（内部锁已重新持有）。通知可能
            // 恰好与超时同时发生，先再判一次谓词，避免漏掉已经放行的机会。
            if (m_reader_count.load(std::memory_order_acquire) >= 0 &&
                !m_writing && m_waiting_writers.empty()) {
                m_reader_count.fetch_add(1, std::memory_order_acq_rel);
                m_state.Unlock();
                return true;
            }
            const bool expired = std::chrono::steady_clock::now() >= deadline;
            m_state.Unlock();
            if (expired) {
                return false;
            }
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
        m_waiting_writers.push_back(WriterWaiter{ticket, nullptr});
        bool gate_closed = false;
        // 尝试推进：队首写者关门并在无读者时立即获取。返回 true 表示已持有
        // 写锁（内部锁已释放）。
        const auto try_acquire = [&]() -> bool {
            if (m_writing || m_waiting_writers.front().ticket != ticket) {
                return false;
            }
            if (!gate_closed) {
                const std::int64_t current =
                    m_reader_count.load(std::memory_order_acquire);
                if (current >= 0) {
                    const std::int64_t readers = m_reader_count.fetch_sub(
                        kReaderGate, std::memory_order_acq_rel);
                    gate_closed = true;
                    if (readers != 0) {
                        return false;
                    }
                } else if (current == -kReaderGate) {
                    gate_closed = true;
                } else {
                    gate_closed = true;
                    return false;
                }
            } else if (m_reader_count.load(std::memory_order_acquire) !=
                       -kReaderGate) {
                return false;
            }
            AcquireWriteLocked(ticket);
            return true;
        };
        for (;;) {
            if (try_acquire()) {
                return true;
            }
            // 与 Lock() 相同的短自旋：定时路径不追求极致延迟，但同样
            // 不该为一次“读者马上退完”付 futex 往返。
            if (gate_closed) {
                for (std::size_t spin = 0; spin < kWriterDrainSpin; ++spin) {
                    if (m_reader_count.load(std::memory_order_acquire) ==
                        -kReaderGate) {
                        break;
                    }
                    CpuRelax();
                }
                if (try_acquire()) {
                    return true;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                if (gate_closed) {
                    m_reader_count.fetch_add(kReaderGate,
                                             std::memory_order_acq_rel);
                }
                const bool was_head =
                    !m_waiting_writers.empty() &&
                    m_waiting_writers.front().ticket == ticket;
                RemoveWaitingWriterLocked(ticket);
                RepairWakeAfterWriteCancelLocked(was_head);
                m_state.Unlock();
                return false;
            }
            {
                ConditionVariable wait_cv;
                const auto waiter = FindWaitingWriterLocked(ticket);
                if (waiter != m_waiting_writers.end()) {
                    waiter->cv = &wait_cv;
                }
                bool notified = false;
                try {
                    notified = wait_cv.WaitFor(m_state, deadline - now);
                } catch (const std::runtime_error&) {
                    if (gate_closed) {
                        m_reader_count.fetch_add(kReaderGate,
                                                 std::memory_order_acq_rel);
                    }
                    AbortAfterLockLoss(true, ticket);
                }
                const auto after_wake = FindWaitingWriterLocked(ticket);
                if (after_wake != m_waiting_writers.end()) {
                    after_wake->cv = nullptr;
                }
                if (notified) {
                    continue;
                }
            }
            // WaitFor 返回 false：通知可能恰好与超时同时到达，再推进一次。
            if (try_acquire()) {
                return true;
            }
            if (gate_closed) {
                m_reader_count.fetch_add(kReaderGate,
                                         std::memory_order_acq_rel);
            }
            const bool was_head = !m_waiting_writers.empty() &&
                                  m_waiting_writers.front().ticket == ticket;
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
        // 语义与旧实现一致：
        // - 持写锁期间恒为 0（门关代表「读者不可见」，而不是负数）；
        // - 写者只在排队、尚未持锁时，读者仍能看到自己的真实名额数。
        // 先看 m_writing 再读计数：读取瞬态（正在回滚的读者快路径）不会
        // 被写锁持有者观察到。
        if (m_writing.load(std::memory_order_acquire)) {
            return 0U;
        }
        const std::int64_t count =
            m_reader_count.load(std::memory_order_acquire);
        const std::int64_t readers =
            count >= 0 ? count : count + kReaderGate;
        return readers > 0 ? static_cast<std::size_t>(readers) : 0U;
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
    /// @brief 写者关闭读者门时从计数里减去的偏移量。
    /// @details 取一个远大于任何真实读者数的值，使「门关」状态在数值上
    ///          可以同时编码「还有多少读者没退完」。
    static constexpr std::int64_t kReaderGate = 1LL << 40;

    /// @brief 读者在门关后、park 前的自旋次数（约几百纳秒）。
    static constexpr std::size_t kReaderSpin = 2048;
    /// @brief 写者等交接令牌 / 等读者退完时的时间预算。
    /// @details 用时间而不是固定 pause 次数：短临界区（本优化的目标负载）
    ///          里等待者常常要跨越多次写者交接才轮到自己，按次数自旋会
    ///          过早 park；而一次 futex 唤醒往返（~2.3us）足以抵消自旋。
    static constexpr std::chrono::microseconds kWriterSpinBudget{100};
    /// @brief 写者关闭门后等读者退完的短自旋次数（定时路径复用）。
    static constexpr std::size_t kWriterDrainSpin = 1024;

    /// @brief 一次 CPU 让步提示；不进入内核（yield 仅作非 x86 兜底）。
    static void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
#if defined(__GNUC__) || defined(__clang__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        __asm__ __volatile__("yield" ::: "memory");
#else
        std::this_thread::yield();
#endif
    }

    /// @brief 在持 m_state 且已满足获取条件时完成写锁登记。
    /// @details 弹出自己的票据、清掉交接令牌、置 m_writing，然后释放内部锁。
    ///          调用方必须保证票据是队首（本类的所有调用点都已判定）。
    void AcquireWriteLocked(std::uint64_t ticket) {
        (void)ticket;
        m_waiting_writers.pop_front();
        m_write_handoff.store(0, std::memory_order_release);
        m_writing.store(true, std::memory_order_release);
        m_state.Unlock();
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kRwLockWrite, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            ::go2cpp::debug::ResourceResult::kOk, 0, false, 0, -1, 0, 0);
#endif
    }

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
            const bool was_head =
                !m_waiting_writers.empty() &&
                m_waiting_writers.front().ticket == writer_ticket;
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

    /// @brief 唤醒写者队首；调用方必须持有 m_state 且写者队列非空。
    /// @details 每个 park 的写者睡在**自己的**条件变量上（指针登记在队列
    ///          节点里），因此这里精确 Notify 队首，不再广播。只有队首可能
    ///          被交接令牌放行；其余写者继续睡到轮到自己，避免惊群。
    void WakeFrontWriterLocked() noexcept {
        if (m_waiting_writers.empty()) {
            return;
        }
        ConditionVariable* waiter_cv = m_waiting_writers.front().cv;
        if (waiter_cv != nullptr) {
            waiter_cv->NotifyOne();
        }
    }

    /// @brief 放行全部排队读者；无人等待时完全短路（不触碰条件变量）。
    void WakeReadersLocked() noexcept {
        if (m_waiting_readers.load(std::memory_order_relaxed) != 0U) {
            m_ready.NotifyAll();
        }
    }

    /// @brief 撤销写者票据后的交接 / 唤醒修补。
    /// @details 只有撤销队首票据才会改变其他等待者的可放行性：
    ///          - 还有写者排队：若门仍关着（上一任写者把门交接了下来），把
    ///            交接令牌转给新队首；门开着则清掉令牌，让新队首自己关门。
    ///          - 队列已空：清掉令牌；门若仍关着必须开回去，再放行读者，
    ///            否则读者会永久等待一个不存在的写者。
    void RepairWakeAfterWriteCancelLocked(bool removed_head) noexcept {
        if (!removed_head) {
            return;
        }
        // 有写者正在持锁：门和交接令牌都由它在 Unlock 时处理。等待者取消
        // 绝不能在这里开门（否则读者会在写者临界区内进入）或提前交接。
        if (m_writing.load(std::memory_order_acquire)) {
            m_write_handoff.store(0, std::memory_order_release);
            return;
        }
        if (!m_waiting_writers.empty()) {
            if (m_reader_count.load(std::memory_order_acquire) < 0) {
                m_write_handoff.store(m_waiting_writers.front().ticket,
                                      std::memory_order_release);
            } else {
                m_write_handoff.store(0, std::memory_order_release);
            }
            WakeFrontWriterLocked();
            return;
        }
        m_write_handoff.store(0, std::memory_order_release);
        if (m_reader_count.load(std::memory_order_acquire) < 0) {
            m_reader_count.fetch_add(kReaderGate, std::memory_order_acq_rel);
        }
        WakeReadersLocked();
    }

    /// @brief 按票据查找等待节点；调用方必须持有 m_state。
    std::deque<WriterWaiter>::iterator FindWaitingWriterLocked(
        std::uint64_t ticket) {
        return std::find_if(
            m_waiting_writers.begin(), m_waiting_writers.end(),
            [ticket](const WriterWaiter& waiter) {
                return waiter.ticket == ticket;
            });
    }

    /// @brief 从等待队列摘除票据；调用方必须持有 m_state，且票据仍在队列中。
    void RemoveWaitingWriterLocked(std::uint64_t ticket) {
        if (!m_waiting_writers.empty() &&
            m_waiting_writers.front().ticket == ticket) {
            m_waiting_writers.pop_front();
            return;
        }
        const auto it = FindWaitingWriterLocked(ticket);
        if (it != m_waiting_writers.end()) {
            m_waiting_writers.erase(it);
        }
    }

    /// @brief 读者计数刚归零且写者门已关时，唤醒等待中的写者。
    /// @details 没有任何写者时零开销返回（只多一次原子读）。
    void WakeWriterIfWaiting() noexcept {
        if (m_reader_count.load(std::memory_order_acquire) >= 0) {
            return;
        }
        try {
            if (!m_state.Lock()) {
                return;
            }
            if (!m_waiting_writers.empty()) {
                WakeFrontWriterLocked();
            }
            m_state.Unlock();
        } catch (...) {
            // 唤醒修补失败不能越过 RUnlock 的 noexcept 边界。
        }
    }

#if GO2CPP_DEBUG_RESOURCES
    /// @brief 资源级历史编号（构造时注册；OFF 构建不存在该成员）。
    ::go2cpp::debug::ResourceId m_debug_resource{0};
#endif
    /// 保护下面全部可变状态的内部互斥量（协程 / 线程混合）。
    mutable Mutex m_state;
    /// 读者广播：读者等待「无写者且无等待写者」。
    ConditionVariable m_ready;

    /// 读者门：`>= 0` 为读者数（门开着）；`== -kReaderGate` 为门关且无
    /// 读者（写者可获取）；`-kReaderGate < 值 < 0` 为门关但仍有读者未退完。
    /// 读者进入与写者关门都作用在这**同一个**原子量上，因此二者有唯一的
    /// 全局顺序（旧版用「计数 + 布尔门」两个变量，存在互相看不见的窗口）。
    // 独占缓存行：读者快路径只触碰这一个原子量，写者慢路径的 m_writing /
    // 等待队列在别的行上。共享一行会让每次读者 RMW 都作废写者正在读的行。
    alignas(64) mutable std::atomic<std::int64_t> m_reader_count{0};
    /// 写者交接令牌：`0` = 无交接；否则为“已被授权获取写锁”的队首票据。
    /// 上一任写者 `Unlock` 时若还有写者排队，就在**不开门**的前提下把令牌
    /// 交给队首；正在自旋的写者看到令牌即可直接登记，完全跳过 futex。
    /// 独立缓存行：写者自旋读它，读者只碰 m_reader_count，避免互相作废。
    alignas(64) std::atomic<std::uint64_t> m_write_handoff{0};
    /// 当前阻塞在 m_ready 上的读者数；供唤醒方判断是否值得通知。
    /// 用 atomic 是因为「内部锁丢失」的兜底路径（D3）必须在**未持锁**的
    /// 情况下完成自减，否则就是对受保护字段的数据竞争。
    std::atomic<std::size_t> m_waiting_readers{0};
    /// 当前是否有写者持锁。
    /// @note 用原子量：`ReaderCount()` 是 noexcept 诊断接口、且必须在不取
    ///       `m_state` 的情况下判断「是否有写者持锁」，否则会与写者路径构成
    ///       数据竞争（TSAN）。所有写入仍发生在 `m_state` 临界区内。
    mutable std::atomic<bool> m_writing{false};
    /// 下一个写者票据；只在 `m_state` 内递增，用于写者 FIFO。
    std::uint64_t m_next_writer_ticket{0};
    /// 正在等待的写者票据队列（push_back 即有序，front() 即队首）。
    /// 用 deque 而不是 set：票据单调递增，尾部插入天然有序，避免每次 Lock
    /// 的红黑树节点分配与 O(log n) 插入；取消时线性 erase（罕见路径）。
    std::deque<WriterWaiter> m_waiting_writers;
};

}  // namespace go2cpp::sync
