/**
 * @file rwmutex.hpp
 * @brief 写者优先的读写锁 `RWMutex`（协程 / OS 线程混合等待）。
 *
 * @date 2026-10-31
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
 * （写者饥饿）。本实现采用**写者优先**：一旦有写者在等，后续新读者必须排队，
 * 直到写者拿到并释放锁。代价是**读者可能被写者压制**，这是有意的取舍 ——
 * 写操作通常更短、更少，饿死写者往往意味着配置永远不生效 / 连接池永远不扩容。
 *
 * ## 状态编码：一个 64 位原子字
 *
 * 门与读者计数合并在一个 `std::atomic<std::int64_t>` 里，读写都只碰这一个
 * 缓存行（这是内层锁唯一的热点行）：
 *
 * | 区间 | 含义 |
 * |---|---|
 * | `0 .. kGate-1` | 门开，值 = 当前读者数 |
 * | `kWriteLocked + R` | 门关（有写者），R = 尚未退出的读者数 |
 * | 再加 `kSlowBit` | 持锁者是慢路径（持有 `m_writer_mutex`） |
 *
 * 因此：
 * - **读者快路径**：一次 `fetch_add(1)`；`prev < kGate` 即门开可直接进入，
 *   否则回滚一次 `fetch_sub(1)`（与旧实现同为单 RMW，读 16t 不退化）；
 * - **写者快路径（无读者、无写者、无人排队）**：一次
 *   `CAS(0 → kWriteLocked)`；`Unlock` 一次 `fetch_sub` 即可开门。这是 1t
 *   纯写的目标路径，不碰任何互斥量 / 队列 / 条件变量。
 *
 * ## 写者 FIFO：慢路径用 `sync::Mutex` 串行化
 *
 * 写者只在**快路径失败**（有读者、有写者、或已有写者排队）时走慢路径：
 * 先 `m_pending++`（告诉读者门要关了，别再进），再 `m_writer_mutex.Lock()`
 * 排队。`sync::Mutex` 本身是 FIFO 的、且对协程走 park（不阻塞 M），所以
 * 写者顺序由它保证，**不再需要票号 / deque / 每个写者自带条件变量**。
 *
 * 代价与取舍（实测见 `docs/reports/2026-10-31-rw-write-hard.md`）：
 *
 * - 1t 无竞争：单 CAS + 单 fetch_sub，约 5 ns，优于 Go 的 6.7 ns；
 * - 4t 纯写：每 op 一次 `sync::Mutex` 交接（跨核 CAS / futex）+ 2 次状态字
 *   RMW。**跨核交接的硬件下限约 30 ns（ping-pong 实测 30.7 ns）**，所以
 *   4t 不可能做到 Go 的 21 ns —— Go 的 goroutine 在同一个 P 上直接 handoff，
 *   不跨核；C++ 的 OS 线程必须跨核。本实现目标是把额外开销压到接近这个
 *   下限，而不是伪造数据。
 *
 * ## 等待路径（慢路径才进入）
 *
 * 慢路径的 park 统一用 `m_park_mutex` + 两个条件变量：
 *
 * - `m_reader_cv`：读者等「门开且无写者排队」；
 * - `m_writer_cv`：写者等「上一个写者开门」或「读者退完」。
 *
 * 条件检查与 `Wait` 都在 `m_park_mutex` 下完成；**通知方也先取同一把
 * 互斥量再 `NotifyAll`**（只在其等待者计数非 0 时才取，无等待者时只多一次
 * 原子 load），因此不存在「检查完条件、还没入队就被通知」的丢唤醒窗口
 * —— 这正是旧实现 8 线程 50/50 混合负载下卡死的原因（写者 park 与
 * `NotifyOne` 之间的窗口，见 `tests/concurrency/test_locks.cpp` 的 8t 用例）。
 *
 * ## ⚠️ 使用边界
 *
 * 1. **读锁持有者不得再申请写锁**：写锁要求「无任何读者」，而调用者自己就是
 *    读者，必然永久等待（自死锁）。本类不提供 `Upgrade`，也不做 owner 检测。
 * 2. **写锁临界区内不要 park**：写者持有 `m_writer_mutex`，其他写者会被排住；
 *    若持锁写者因 IO park，写者队列整体停摆。需要 IO 的临界区请拆分。
 * 3. **写锁不可重入**：同一 G 重复 `Lock()` 会自死锁（`m_writer_mutex` 不可递归）。
 * 4. **读锁可以重入**：同一执行流拿两次读锁 = 两个读者名额，必须 `RUnlock`
 *    两次；但若已有写者排队，重入会等写者先完成 —— 而写者又在等调用者释放
 *    手上的读锁，构成自死锁。持读锁期间如需再取读锁，请用非阻塞的
 *    `TryRLock`（它在有写者排队时返回 false，不会阻塞）。
 * 5. 对象必须长于所有使用者和等待者；销毁一把仍被持有 / 有等待者的锁是
 *    未定义行为。
 */
#pragma once

#include "go2cpp/sync.hpp"

#include "go2cpp/debug/debug_resources.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace go2cpp::sync {

/**
 * @brief 写者优先的读写锁。
 *
 * @details
 * 状态机：**门开（N 个读者）** 或 **门关（最多 1 个写者 + R 个尚未退出的读者）**。
 * 读者与写者都只通过 `m_state` 这一个原子字裁决，`m_writer_mutex` 只负责
 * 慢路径写者的 FIFO 排队，`m_park_mutex` + 条件变量只负责真正的 park。
 *
 * | 请求 | 放行条件 |
 * |---|---|
 * | `RLock` | 状态字门开（`< kGate`）且没有写者在排队 |
 * | `Lock` | 快路径：状态字为 0 时单 CAS 关门；慢路径：拿到 `m_writer_mutex` 后关门并等读者归零 |
 * | `TryRLock` | 同 `RLock` 的放行条件，不等待 |
 * | `TryLock` | 同 `Lock` 的放行条件（含「没有等待中的写者」），不等待 |
 *
 * `TryLock` 在「锁空闲但已有写者排队」时返回 false，这是为了不破坏写者
 * FIFO；`std::shared_mutex` 对 `try_lock` 的公平性不作保证，这里选择更严格
 * 的语义并写进文档。
 */
class RWMutex final {
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
        // 快路径：一次 fetch_add。门开（结果落在 [0, kGate)）且没有写者
        // 排队时直接进入。读者快路径与写者关门作用在同一个原子字上，
        // 因此「读者进入」与「写者关门」有唯一的全局顺序，不存在两个变量
        // 互相看不见的窗口。
        const std::int64_t previous =
            m_state.fetch_add(1, std::memory_order_acq_rel);
        if (previous >= 0 && previous < kGate &&
            m_pending.load(std::memory_order_acquire) == 0) {
#if GO2CPP_DEBUG_RESOURCES
            GO2CPP_DEBUG_RESOURCE_EVENT_V5(
                kRwLockRead, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0,
                0, ::go2cpp::debug::ResourceResult::kOk, 0, false, 0, -1, 0,
                0);
#endif
            return;
        }
        // 门关：回滚登记，再走 park 路径。回滚可能把「门关 + 剩余读者」
        // 变成「门关 + 读者归零」，必须补一次写者唤醒，否则写者会睡在
        // 一个已经归零的计数上。
        const std::int64_t after =
            m_state.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if ((after & kWriteBit) != 0 && (after & kLowMask) == 0) {
            WakeDrainWatchers();
        }
        RLockSlow();
    }

    /**
     * @brief 释放读锁。
     * @details 最后一个读者离开且写者门已关时，精确唤醒等待读者归零的写者；
     *          门开着时什么都不做（读者快路径不取任何锁）。
     * @throw std::logic_error 当前没有读者（重复解锁 / 从未加锁）。
     */
    void RUnlock() {
        const std::int64_t previous =
            m_state.fetch_sub(1, std::memory_order_acq_rel);
        const bool closed = (previous & kWriteBit) != 0;
        const std::int64_t readers_before =
            closed ? (previous & kLowMask) : previous;
        if (readers_before <= 0) {
            // 没有读者可减：重复解锁 / 从未加锁。恢复计数后报错，
            // 保持「抛异常前不改变状态」的既有语义。
            m_state.fetch_add(1, std::memory_order_relaxed);
            throw std::logic_error(
                "go2cpp::sync::RWMutex RUnlock of unlocked RWMutex");
        }
        if (closed && readers_before == 1) {
            // 最后一个读者：写者可能正等着读者归零。
            WakeDrainWatchers();
        }
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kRwUnlockRead, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            ::go2cpp::debug::ResourceResult::kOk, 0, false, 0, -1, 0, 0);
#endif
    }

    /**
     * @brief 获取写锁（独占），写者之间 FIFO。
     * @details 快路径：状态字为 0（无读者、无写者、无人排队）时一次 CAS
     *          直接关门；否则走慢路径排队。
     * @throw std::logic_error 调度器不可用 / G 被取消导致内部等待无法重锁。
     */
    void Lock() {
        // 快路径：先看有没有写者在排队，再单 CAS 关门。无竞争（1t）时
        // 只有一次原子 load + 一次 CAS，不碰任何互斥量。
        if (m_pending.load(std::memory_order_acquire) == 0) {
            std::int64_t expected = 0;
            if (m_state.compare_exchange_strong(
                    expected, kWriteLocked, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
#if GO2CPP_DEBUG_RESOURCES
                GO2CPP_DEBUG_RESOURCE_EVENT_V5(
                    kRwLockWrite, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(),
                    0, 0, ::go2cpp::debug::ResourceResult::kOk, 0, false, 0,
                    -1, 0, 0);
#endif
                return;
            }
        }
        LockSlow(false, std::chrono::milliseconds::zero(), {});
    }

    /**
     * @brief 释放写锁。
     * @throw std::logic_error 当前没有写者（重复解锁 / 从未加锁）。
     */
    void Unlock() {
        const std::int64_t snapshot = m_state.load(std::memory_order_acquire);
        if ((snapshot & kWriteBit) == 0) {
            throw std::logic_error(
                "go2cpp::sync::RWMutex Unlock of unlocked RWMutex");
        }
        const bool slow = (snapshot & kSlowBit) != 0;
        // 开门：一次性把「写者位 + 慢路径位 + 门偏移」全部还原。
        // 写者持锁期间读者不可能登记（门关），因此低位读者数必为 0。
        m_state.fetch_sub(kFastRelease + (slow ? kSlowBit : 0),
                          std::memory_order_release);
        if (slow) {
            m_writer_mutex.Unlock();
        }
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kRwUnlockWrite, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            (m_waiting_readers.load(std::memory_order_relaxed) != 0U ||
             m_gate_waiters.load(std::memory_order_relaxed) != 0U)
                ? ::go2cpp::debug::ResourceResult::kWake
                : ::go2cpp::debug::ResourceResult::kOk,
            0, false, 0, -1, 0, 0);
#endif
        WakeReadersIfWaiting();
        WakeGateIfWaiting();
    }

    /**
     * @brief 非阻塞尝试获取读锁。
     * @return 成功返回 true（已持有读锁）；有写者持锁或有写者等待时返回 false。
     */
    bool TryRLock() noexcept {
        try {
            const std::int64_t previous =
                m_state.fetch_add(1, std::memory_order_acq_rel);
            if (previous >= 0 && previous < kGate &&
                m_pending.load(std::memory_order_acquire) == 0) {
                return true;
            }
            const std::int64_t after =
                m_state.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if ((after & kWriteBit) != 0 && (after & kLowMask) == 0) {
                WakeDrainWatchers();
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
            if (m_pending.load(std::memory_order_acquire) != 0) {
                return false;
            }
            std::int64_t expected = 0;
            return m_state.compare_exchange_strong(
                expected, kWriteLocked, std::memory_order_acquire,
                std::memory_order_relaxed);
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
        if (TryRLock()) {
            return true;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (!m_park_mutex.Lock()) {
            return false;
        }
        for (;;) {
            const std::int64_t current = m_state.load(std::memory_order_acquire);
            if (current >= 0 && current < kGate &&
                m_pending.load(std::memory_order_acquire) == 0) {
                const std::int64_t registered =
                    m_state.fetch_add(1, std::memory_order_acq_rel);
                if (registered >= 0 && registered < kGate) {
                    m_park_mutex.Unlock();
                    return true;
                }
                const std::int64_t after =
                    m_state.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if ((after & kWriteBit) != 0 && (after & kLowMask) == 0) {
                    WakeDrainWatchers();
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                m_park_mutex.Unlock();
                return false;
            }
            m_waiting_readers.fetch_add(1, std::memory_order_relaxed);
            bool notified = false;
            try {
                notified = WaitReaderLocked(deadline - now);
            } catch (...) {
                m_waiting_readers.fetch_sub(1, std::memory_order_relaxed);
                throw;
            }
            m_waiting_readers.fetch_sub(1, std::memory_order_relaxed);
            if (!notified) {
                // WaitFor 返回 false：通常是超时（内部锁已重新持有）。
                // 通知可能恰好与超时同时发生，回到循环再判一次谓词。
                const std::int64_t after =
                    m_state.load(std::memory_order_acquire);
                if (after >= 0 && after < kGate &&
                    m_pending.load(std::memory_order_acquire) == 0) {
                    const std::int64_t registered =
                        m_state.fetch_add(1, std::memory_order_acq_rel);
                    if (registered >= 0 && registered < kGate) {
                        m_park_mutex.Unlock();
                        return true;
                    }
                    const std::int64_t rolled =
                        m_state.fetch_sub(1, std::memory_order_acq_rel) - 1;
                    if ((rolled & kWriteBit) != 0 &&
                        (rolled & kLowMask) == 0) {
                        WakeDrainWatchers();
                    }
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    m_park_mutex.Unlock();
                    return false;
                }
            }
        }
    }

    /**
     * @brief 在超时内获取写锁。
     * @param timeout 最长等待时间；`<= 0` 等价于一次 `TryLock`。
     * @return 成功返回 true 且已持有写锁；超时返回 false，且**不改变读写状态**。
     * @throw std::logic_error G 被取消 / 调度器关闭导致等待中止（非超时）。
     */
    bool LockFor(std::chrono::milliseconds timeout) {
        if (timeout <= std::chrono::milliseconds::zero()) {
            return TryLock();
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (TryLock()) {
            return true;
        }
        return LockSlow(true, timeout, deadline);
    }

    /**
     * @brief 当前读者数量快照（含调用者自己）。
     * @return 读者数量。诊断用途，只表示调用瞬间的值。
     */
    std::size_t ReaderCount() const noexcept {
        const std::int64_t current = m_state.load(std::memory_order_acquire);
        if ((current & kWriteHeldBit) != 0) {
            // 写者已进入临界区：旧实现语义下读者数恒为 0。
            return 0U;
        }
        const std::int64_t readers = (current & kWriteBit) != 0
                                         ? (current & kLowMask)
                                         : current;
        return readers > 0 ? static_cast<std::size_t>(readers) : 0U;
    }

    /**
     * @brief 当前是否有写者持有写锁。
     * @return 持有返回 true。诊断用途，只表示调用瞬间的值。
     * @note 单次原子 load：不再像旧实现那样为诊断去取内部互斥量。
     */
    bool IsWriteLocked() const noexcept {
        return (m_state.load(std::memory_order_acquire) & kWriteHeldBit) != 0;
    }

    /**
     * @brief 当前是否有写者在等待（不含已持锁的写者）。
     * @return 有等待写者返回 true。诊断用途，也用于验证写者优先。
     */
    bool HasPendingWriter() const noexcept {
        return m_pending.load(std::memory_order_acquire) != 0;
    }

private:
    /// @brief 读者计数容量 / 门偏移。远大于任何真实读者数。
    static constexpr std::int64_t kGate = 1LL << 40;
    /// @brief 写者持有位（门关）。
    static constexpr std::int64_t kWriteBit = 1LL << 62;
    /// @brief 慢路径标记位：持锁写者同时持有 `m_writer_mutex`。
    static constexpr std::int64_t kSlowBit = 1LL << 61;
    /// @brief 写者已持有位：读者全部退出、写者正式进入临界区后置位。
    /// @details 与 kWriteBit（门已关，可能还在等读者退出）分开，是为
    ///          了让 IsWriteLocked() 保持旧语义：读者临界区里即使写者
    ///          已经关门在等它退出，也必须返回 false。
    static constexpr std::int64_t kWriteHeldBit = 1LL << 60;
    /// @brief 低位读者计数掩码。
    static constexpr std::int64_t kLowMask = kGate - 1;
    /// @brief 快路径一次性关门 + 立即持有（原值必为 0，无读者）。
    static constexpr std::int64_t kWriteLocked = kWriteBit + kWriteHeldBit;
    /// @brief 慢路径关门时一次性加上的增量（含慢路径位，不含「已持有」位）。
    static constexpr std::int64_t kSlowClose = kWriteBit + kSlowBit;
    /// @brief 慢路径关门时读者已归零：关门与「已持有」合并成一次 CAS。
    static constexpr std::int64_t kSlowCloseHeld =
        kWriteBit + kSlowBit + kWriteHeldBit;
    /// @brief Unlock 需要减掉的全部写者位（快路径）。
    static constexpr std::int64_t kFastRelease = kWriteBit + kWriteHeldBit;

    /// @brief 写者等门 / 等读者归零时的短自旋次数。
    static constexpr std::size_t kWriterSpin = 4096;
    /// @brief 读者等写者放行时的短自旋次数。
    static constexpr std::size_t kReaderSpin = 256;

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

    /**
     * @brief 写者慢路径：FIFO 排队、关门、等读者归零。
     * @param timed 是否是带超时的调用（`LockFor`）。
     * @param timeout 仅诊断用，保留与 `LockFor` 的对称语义。
     * @param deadline 截止时间；`timed == false` 时忽略。
     * @return 获得写锁返回 true；超时返回 false（状态已还原）。
     * @throw std::logic_error 调度器不可用 / 等待中止。
     */
    bool LockSlow(bool timed, std::chrono::milliseconds timeout,
                  std::chrono::steady_clock::time_point deadline) {
        (void)timeout;
        // 先宣告「有写者在等」：读者快路径看到它就不再进门，保证写者优先。
        m_pending.fetch_add(1, std::memory_order_acq_rel);
        bool holding_writer_mutex = false;
        if (timed) {
            if (deadline <= std::chrono::steady_clock::now()) {
                m_pending.fetch_sub(1, std::memory_order_acq_rel);
                return false;
            }
            if (!m_writer_mutex.LockFor(deadline - std::chrono::steady_clock::now())) {
                m_pending.fetch_sub(1, std::memory_order_acq_rel);
                return false;
            }
            holding_writer_mutex = true;
        } else {
            if (!m_writer_mutex.Lock()) {
                m_pending.fetch_sub(1, std::memory_order_acq_rel);
                throw std::logic_error(
                    "go2cpp::sync::RWMutex Lock: scheduler unavailable");
            }
            holding_writer_mutex = true;
        }
        // 关门。关门后读者不能再登记，剩余读者会逐个退出。
        // already_held：本次关门的 CAS 是否已经把「已持有」位一并置上。
        bool already_held = false;
        for (;;) {
            const std::int64_t current = m_state.load(std::memory_order_acquire);
            if (current == 0) {
                // 无读者：关门与「已持有」合并成一次 CAS —— 纯写争用的
                // 主路径，每 op 少一次 fetch_or。
                std::int64_t expected = 0;
                if (m_state.compare_exchange_strong(
                        expected, kSlowCloseHeld, std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    already_held = true;
                    break;
                }
                continue;
            }
            if (current > 0 && current < kGate) {
                // 门开但有读者：一次 CAS 关门（把读者数带上一起关门）。
                std::int64_t expected = current;
                if (m_state.compare_exchange_strong(
                        expected, current + kSlowClose,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }
            // 门已关：上一个写者还在临界区（或正在等读者）。等它开门。
            if (timed && std::chrono::steady_clock::now() >= deadline) {
                m_pending.fetch_sub(1, std::memory_order_acq_rel);
                if (holding_writer_mutex) {
                    m_writer_mutex.Unlock();
                }
                return false;
            }
            WaitGateOpen(timed, deadline);
        }
        // 等读者归零（正常情况下读者已经在退出，很快）。
        while ((m_state.load(std::memory_order_acquire) & kLowMask) != 0) {
            if (timed && std::chrono::steady_clock::now() >= deadline) {
                // 超时：把门开回去，保持「不改变读写状态」的契约。
                m_state.fetch_sub(already_held ? kSlowCloseHeld
                                             : kSlowClose,
                                  std::memory_order_release);
                m_pending.fetch_sub(1, std::memory_order_acq_rel);
                if (holding_writer_mutex) {
                    m_writer_mutex.Unlock();
                }
                WakeReadersIfWaiting();
                return false;
            }
            WaitReadersDrain(timed, deadline);
        }
        // 读者已全部退出：正式持有写锁（IsWriteLocked 从这里为真）。
        // 无读者的主路径已在关门的 CAS 里置过该位，这里只补
        // 「关门时还有读者」的慢路径。
        if (!already_held) {
            m_state.fetch_or(kWriteHeldBit, std::memory_order_release);
        }
        m_pending.fetch_sub(1, std::memory_order_acq_rel);
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_DEBUG_RESOURCE_EVENT_V5(
            kRwLockWrite, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(), 0, 0,
            ::go2cpp::debug::ResourceResult::kOk, 0, false, 0, -1, 0, 0);
#endif
        return true;
    }

    /**
     * @brief 读者慢路径：park 到「门开且无写者排队」。
     * @throw std::logic_error 调度器不可用 / 等待中止。
     */
    void RLockSlow() {
        if (!m_park_mutex.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::RWMutex RLock: scheduler unavailable");
        }
        for (;;) {
            const std::int64_t current = m_state.load(std::memory_order_acquire);
            if (current >= 0 && current < kGate &&
                m_pending.load(std::memory_order_acquire) == 0) {
                // 登记后必须复查返回值：写者的快路径 CAS 不取 m_park_mutex，
                // 可能在「检查门开」与「fetch_add」之间抢先关门。此时
                // fetch_add 返回的是门关状态（>= kGate），必须回滚继续等。
                const std::int64_t registered =
                    m_state.fetch_add(1, std::memory_order_acq_rel);
                if (registered >= 0 && registered < kGate) {
                    m_park_mutex.Unlock();
#if GO2CPP_DEBUG_RESOURCES
                GO2CPP_DEBUG_RESOURCE_EVENT_V5(
                    kRwLockRead, m_debug_resource, GO2CPP_DEBUG_BOUND_TASK(),
                    0, 0, ::go2cpp::debug::ResourceResult::kOk, 0, false, 0,
                    -1, 0, 0);
#endif
                    return;
                }
                const std::int64_t after =
                    m_state.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if ((after & kWriteBit) != 0 && (after & kLowMask) == 0) {
                    WakeDrainWatchers();
                }
            }
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
                notified = m_reader_cv.Wait(m_park_mutex);
            } catch (const std::runtime_error&) {
                m_waiting_readers.fetch_sub(1, std::memory_order_relaxed);
                throw std::logic_error(
                    "go2cpp::sync::RWMutex RLock aborted (internal wait "
                    "lost)");
            }
            m_waiting_readers.fetch_sub(1, std::memory_order_relaxed);
            if (!notified) {
                m_park_mutex.Unlock();
                throw std::logic_error(
                    "go2cpp::sync::RWMutex RLock aborted (G cancelled or "
                    "scheduler shut down)");
            }
        }
    }

    /**
     * @brief 写者等「上一个写者开门」。
     * @details 先短自旋（交接窗口通常只有几条指令），再 park。
     */
    void WaitGateOpen(bool timed,
                      std::chrono::steady_clock::time_point deadline) {
        for (std::size_t spin = 0; spin < kWriterSpin; ++spin) {
            if (m_state.load(std::memory_order_acquire) == 0) {
                return;
            }
            CpuRelax();
        }
        if (!m_park_mutex.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::RWMutex Lock: scheduler unavailable");
        }
        m_gate_waiters.fetch_add(1, std::memory_order_relaxed);
        while (m_state.load(std::memory_order_acquire) != 0) {
            bool notified = false;
            try {
                if (timed) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        notified = false;
                        break;
                    }
                    notified = m_writer_cv.WaitFor(m_park_mutex, deadline - now);
                } else {
                    notified = m_writer_cv.Wait(m_park_mutex);
                }
            } catch (const std::runtime_error&) {
                m_gate_waiters.fetch_sub(1, std::memory_order_relaxed);
                throw std::logic_error(
                    "go2cpp::sync::RWMutex Lock aborted (internal wait lost)");
            }
            if (!notified && !timed) {
                m_gate_waiters.fetch_sub(1, std::memory_order_relaxed);
                m_park_mutex.Unlock();
                throw std::logic_error(
                    "go2cpp::sync::RWMutex Lock aborted (G cancelled or "
                    "scheduler shut down)");
            }
        }
        m_gate_waiters.fetch_sub(1, std::memory_order_relaxed);
        m_park_mutex.Unlock();
    }

    /**
     * @brief 写者等「读者全部退出」。
     * @details 与 `WaitGateOpen` 共用 `m_writer_cv`（各自重查自己的谓词）。
     */
    void WaitReadersDrain(bool timed,
                          std::chrono::steady_clock::time_point deadline) {
        for (std::size_t spin = 0; spin < kWriterSpin; ++spin) {
            if ((m_state.load(std::memory_order_acquire) & kLowMask) == 0) {
                return;
            }
            CpuRelax();
        }
        if (!m_park_mutex.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::RWMutex Lock: scheduler unavailable");
        }
        m_drain_waiters.fetch_add(1, std::memory_order_relaxed);
        while ((m_state.load(std::memory_order_acquire) & kLowMask) != 0) {
            bool notified = false;
            try {
                if (timed) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        break;
                    }
                    notified = m_writer_cv.WaitFor(m_park_mutex, deadline - now);
                } else {
                    notified = m_writer_cv.Wait(m_park_mutex);
                }
            } catch (const std::runtime_error&) {
                m_drain_waiters.fetch_sub(1, std::memory_order_relaxed);
                throw std::logic_error(
                    "go2cpp::sync::RWMutex Lock aborted (internal wait lost)");
            }
            if (!notified && !timed) {
                m_drain_waiters.fetch_sub(1, std::memory_order_relaxed);
                m_park_mutex.Unlock();
                throw std::logic_error(
                    "go2cpp::sync::RWMutex Lock aborted (G cancelled or "
                    "scheduler shut down)");
            }
        }
        m_drain_waiters.fetch_sub(1, std::memory_order_relaxed);
        m_park_mutex.Unlock();
    }

    /// @brief 带截止时间的读者等待（供 `RLockFor` 使用）。
    bool WaitReaderLocked(std::chrono::steady_clock::duration budget) {
        try {
            return m_reader_cv.WaitFor(m_park_mutex, budget);
        } catch (const std::runtime_error&) {
            throw std::logic_error(
                "go2cpp::sync::RWMutex RLockFor aborted (internal wait lost)");
        }
    }

    /// @brief 唤醒等「读者归零」的写者；无人等待时只多一次原子 load。
    void WakeDrainWatchers() noexcept {
        if (m_drain_waiters.load(std::memory_order_relaxed) == 0U) {
            return;
        }
        WakeWriterWaiters();
    }

    /// @brief 唤醒等「上一个写者开门」的写者；无人等待时短路。
    void WakeGateIfWaiting() noexcept {
        if (m_gate_waiters.load(std::memory_order_relaxed) == 0U) {
            return;
        }
        WakeWriterWaiters();
    }

    /// @brief 唤醒 park 中的读者；无人等待时短路。
    void WakeReadersIfWaiting() noexcept {
        if (m_waiting_readers.load(std::memory_order_relaxed) == 0U) {
            return;
        }
        try {
            if (!m_park_mutex.Lock()) {
                return;
            }
            m_reader_cv.NotifyAll();
            m_park_mutex.Unlock();
        } catch (...) {
            // 唤醒失败不能越过 noexcept 边界。
        }
    }

    /// @brief 唤醒 `m_writer_cv` 上的全部写者（各自重查谓词）。
    void WakeWriterWaiters() noexcept {
        try {
            if (!m_park_mutex.Lock()) {
                return;
            }
            m_writer_cv.NotifyAll();
            m_park_mutex.Unlock();
        } catch (...) {
        }
    }

#if GO2CPP_DEBUG_RESOURCES
    /// @brief 资源级历史编号（构造时注册；OFF 构建不存在该成员）。
    ::go2cpp::debug::ResourceId m_debug_resource{0};
#endif
    /// @brief 读者计数 + 写者门 + 慢路径标记的合并状态字。
    /// @details 独占缓存行：读写快路径都只碰这一行。
    alignas(64) mutable std::atomic<std::int64_t> m_state{0};
    /// @brief 正在慢路径等待的写者数（>0 时读者不再走快路径）。
    /// @details 独立缓存行：读者快路径每次 RLock 读它，不能和写者的
    ///          状态字更新假共享。
    alignas(64) std::atomic<std::size_t> m_pending{0};
    /// @brief 写者 FIFO 排队（慢路径专用；协程走 park、线程走 futex）。
    /// @details `sync::Mutex` 自带 FIFO 与饥饿保护，慢路径直接复用它，
    ///          不再手写票号 / 队列 / 每个写者一个条件变量。
    Mutex m_writer_mutex;
    /// @brief 慢路径 park 共用的内部互斥量（包住条件检查与 `Wait`）。
    Mutex m_park_mutex;
    /// @brief 读者等待：门开且无写者排队。
    ConditionVariable m_reader_cv;
    /// @brief 写者等待：上一个写者开门 / 读者全部退出。
    ConditionVariable m_writer_cv;
    /// @brief 阻塞在 `m_reader_cv` 上的读者数（唤醒短路用）。
    std::atomic<std::size_t> m_waiting_readers{0};
    /// @brief 阻塞在 `m_writer_cv` 上等「门开」的写者数。
    std::atomic<std::size_t> m_gate_waiters{0};
    /// @brief 阻塞在 `m_writer_cv` 上等「读者归零」的写者数。
    std::atomic<std::size_t> m_drain_waiters{0};
};

}  // namespace go2cpp::sync
