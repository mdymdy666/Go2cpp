/**
 * @file spin_mutex.hpp
 * @brief 分层锁：`SpinMutex`（临界区禁止 park）。
 *
 * @date 2026-10-18
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 为什么需要两种锁
 *
 * Go2Cpp 是**协作式**调度：G 只有在显式 park 点才会让出 M。与 Go 不同，
 * 这里的 `read` / `write` / `connect` / `accept` / `sleep` 都被 hook 成了
 * park 点 —— 用户看到的「一行同步调用」实际上可能把当前 G 挂起。
 *
 * 于是出现一类 Go 里不存在的死锁：
 *
 * ```
 * G1: Lock(A) -> read(fd) -> park     // 持着 A 走了
 * G2: Lock(B) -> read(fd) -> park     // 持着 B 走了
 * G3: Lock(A) -> 等 G1 释放 A
 *     A 在 G1 手里，G1 在等可能永远不来的 fd -> 永久死锁
 * ```
 *
 * Linux 内核用两种锁区分这两类临界区：`spinlock_t`（睡着即 bug）与
 * `mutex`（可以睡）。本文件把同一思路搬进 Go2Cpp：
 *
 * - `sync::Mutex`：临界区内**可以** park（IO、阻塞等待），其他 G 继续跑；
 * - `sync::SpinMutex`：临界区内**禁止** park，只允许纯计算，必须极短。
 *
 * ## 实现：自旋 + 门（Go `runtime/lock_futex.go` 的移植）
 *
 * 本文件在 2026-10-30 之后从「纯包装 `sync::Mutex`」改为**混合自旋锁**。
 * 结构照搬 Go 运行时互斥锁（`GoSrc/go14/src/runtime/lock_futex.go`，
 * BSD 许可）的三段式，只是把 Go 的 `futexsleep` 换成项目里已有的
 * `sync::Mutex`——它就是 Go2Cpp 的「门」：FIFO、可 park、可取消、带
 * 1ms 饥饿保护。三段的对应关系：
 *
 * | 阶段 | Go `lock()` | 本文件 |
 * |---|---|---|
 * | 抢占快路径 | `xchg(key, mutex_locked)` | `m_state` 一次 CAS 0→1（acquire）|
 * | 主动自旋 | `active_spin=4` 轮 × `procyield(30)` | `kSpinAttempts=64` 次 `PAUSE` |
 * | 被动让出 | `passive_spin=1` × `osyield()` | `yield_current()` / `std::this_thread::yield()` |
 * | 睡眠门 | `futexsleep(key, mutex_sleeping)` | 慢路径门 `m_impl`（`sync::Mutex`）|
 *
 * 与 Go 的**差异**（刻意为之）：
 *
 * - Go 的门是内核 futex，睡眠/唤醒是两次系统调用；这里的门是用户态
 *   `sync::Mutex`，受管 G 走 park（不占 M），普通线程走原生条件变量；
 * - Go 的 `mutex_sleeping` 用一个状态位表示「有人在睡」；这里不需要，
 *   因为门本身的 FIFO 队列就是等待者名单，`Unlock` 只做一次 release
 *   store，不需要判断「要不要 futexwakeup」；
 * - 门上增加了 `kGateSpinAttempts=16` 的门内自旋：被唤醒后先在自己的
 *   CPU 上抢一次，避免「刚唤醒又立刻 park」的抖动。
 *
 * ## 三段式状态机
 *
 * 1. **快路径（无竞争）**：`m_state` 从 0 CAS 到 1，一条 `lock cmpxchg`。
 *    临界区退出时 `m_state.store(0, release)`，没有解锁唤醒判断；
 * 2. **自旋（有竞争但临界区短）**：连续 64 次 `PAUSE`，每次先 `load`
 *    再按需 CAS（先读后写，避免自旋期间反复申请独占所有权把缓存行
 *    钉在别的核上）；
 * 3. **门（临界区长或持锁者被抢占）**：`m_impl.Lock()` 排队/ park，
 *    被唤醒后在门内再自旋 16 次；仍失败则释放门、让出、重新排队。
 *
 * **不会死锁**：门只在「检查—重试」的一小段内被持有，任何持有者都
 * 必然在有限步内释放（成功即 `Unlock` 门，失败即重排），而真正的锁
 * 所有权只由 `m_state` 裁决，`Unlock` 不依赖门。
 *
 * ## 缓存行布局（为什么要对齐）
 *
 * ```
 * offset 0    : Mutex m_impl（门；内部 m_state 已 alignas(64)）
 * offset 128  : atomic<uint8_t> m_state（快路径唯一热字，独占一行）
 * offset 129  : shared_ptr<Task> m_owner（受管 G 的 owner 记录，同行）
 * ```
 *
 * - `m_state` 独占一条缓存行：自旋/CAS 只在一条线上做 cacheline 传输，
 *   不会把 `m_impl` 的队列指针或相邻 SpinMutex 实例拖进同一行（假共享）；
 * - `m_owner` 与 `m_state` 同行：普通线程路径上 `Unlock` 读 `m_owner`
 *   时该行必已因 `store(0)` 处于 Modified 状态，不额外付一次 miss；
 * - `m_impl` **必须是第一个成员**：`tests/debug/test_resource_history_stress.cpp`
 *   依赖「`&spin_mutex == &内部 Mutex`」来注销/重登记 debug 资源条目。
 *   这条约束不能为了布局好看而破坏。
 *
 * ## 两种模式（编译期决定）
 *
 * - **debug**（定义 `GO2CPP_DEBUG_LOCKS`，或构建类型为 Debug/未定义
 *   `NDEBUG`）：在 `no_park_depth > 0` 期间 park 会打印 G id、嵌套深度与
 *   调用栈，然后 `std::abort()`；
 * - **release**（`NDEBUG` 已定义且未显式开启 `GO2CPP_DEBUG_LOCKS`）：
 *   **不 abort**，降级为普通 `Mutex` 语义（正确但慢），打印一次警告并递增
 *   `scheduler::spin_mutex_park_degrade_total()`。
 *
 * 生产环境不能因为契约违反就崩溃，这是 release 不 abort 的唯一理由。
 *
 * ## 普通 `Mutex` 的持锁计数（与本文件的关系）
 *
 * 普通 `sync::Mutex` 的持锁计数由 `sync.cpp` 内置维护：
 * `Mutex::Lock` 成功后调用 `Task::note_mutex_locked()`，`Unlock` 时按
 * owner G 递减。检测开关是 `sync::SetHeldLockParkDetection()`（默认
 * Debug 开、Release 关，可用环境变量 `GO2CPP_DETECT_HELD_PARK` 覆盖）。
 *
 * 本文件只负责 `SpinMutex` 的 `no_park_depth`。park 入口对两者的处理是
 * **互斥**的：
 *
 * | 条件 | 语义 | debug | release |
 * |---|---|---|---|
 * | `no_park_depth > 0` | 禁止 park | abort | 降级 + 计数 |
 * | `held_mutex_count > 0 && no_park_depth == 0` | 允许但告警 | 计数 + 日志 | 计数 |
 *
 * ## 使用边界
 *
 * - `SpinMutex` 的临界区**必须**只做纯计算（内存操作、数据结构更新），
 *   不得调用 `read` / `write` / `connect` / `accept` / `sleep` / 竞争中的
 *   `Mutex::Lock` 等任何 park 点；
 * - `*_f` 版本（`read_f` / `write_f` / ...）不会 park，但会**真实阻塞整个
 *   M**，同一 M 上的其他 G 全部停摆，因此**同样不适合**放进临界区；
 * - 需要 IO 的临界区请用 `sync::Mutex`；
 * - `SpinMutex` 没有 owner 概念（与 `Mutex` 一致，Go 风格允许跨 G 解锁）。
 *   跨 G 解锁时，计数按**加锁时的 G** 递减，依赖所有权移交本身携带的
 *   happens-before；若在没有任何同步的情况下跨线程解锁，锁本身也不安全；
 * - **不保证 FIFO**：自旋阶段允许后来者直接抢到刚释放的锁（Go 的
 *   `active_spin` 同样如此）。这正是自旋锁用吞吐换公平性的取舍；门内
 *   仍有 `sync::Mutex` 的 FIFO + 1ms 饥饿保护兜底，不会无限饿死。
 */
#pragma once

#include "go2cpp/concurrency/scheduler/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace go2cpp::sync {

namespace detail {

/**
 * @brief 自旋等待的 CPU 提示。
 *
 * @details x86 用 `PAUSE`：它告诉流水线「这是自旋」，减少退出自旋时的
 *          分支预测失败开销，也降低同核超线程（SMT）上的执行端口争用。
 *          与 `sync.cpp` 的 `RelaxCpu` 同义，但本文件必须自包含——不能
 *          依赖 `sync.cpp` 的内部符号（那会引入一个跨 DSO 调用）。
 * @note ARM 用 `YIELD`；其他架构退回 `sched_yield`（宁可让出也不空转）。
 */
inline void SpinRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

}  // namespace detail

/**
 * @brief 不可睡眠锁：临界区内禁止发生 park（协作点）。
 *
 * @details 混合自旋锁：`m_state` 的一次 CAS 是唯一的互斥裁决点，内部
 *          `sync::Mutex`（`m_impl`）只在「自旋 64 次仍拿不到」时作为门
 *          使用，负责排队、park 与取消。`Lock()` / `TryLock()` 成功后给
 *          当前 G 增加 `no_park_depth`；`Unlock()` 先递减再释放。
 *          加锁时的 G 会被记录下来，因此跨 G 解锁也能精确递减。
 *
 * ## 与 `sync::Mutex` 的区别
 *
 * | 维度 | `Mutex` | `SpinMutex` |
 * |---|---|---|
 * | 临界区内 park | 允许 | **禁止**（debug abort / release 降级）|
 * | 典型用途 | 保护可能 IO 的临界区 | 保护极短纯计算临界区 |
 * | 争用处理 | 8 次自旋 + FIFO 队列 park | 64 次 PAUSE + 让出 + 门（FIFO park）|
 * | 无竞争开销 | 一次 CAS | 一次 CAS（同一量级）|
 *
 * ## 嵌套
 *
 * 允许同一 G 嵌套加锁（只要不形成真正的自死锁）。每层 `Lock` 使
 * `no_park_depth` 加一，每层 `Unlock` 减一；深度归零后才恢复可 park。
 *
 * ## 异常安全
 *
 * - `Lock()` 在门不可用（被 Context 取消或调度器已关闭）时抛
 *   `std::logic_error`，且不改变任何计数；
 * - `Unlock()` 在 `m_state` 未被持有时抛 `std::logic_error`（与
 *   `Mutex::Unlock` 的「unlock of unlocked mutex」一致）；
 * - 推荐使用 `Guard`（RAII），它保证正常路径与异常路径都会解锁。
 *
 * ## 与普通线程的关系
 *
 * 在普通 OS 线程（没有受管 G）上，`no_park_depth` 无处记录，`SpinMutex`
 * 退化为普通 `Mutex`。这是有意的：契约检查只对协作式 G 有意义。
 * 慢路径的「让出」也会在两种执行体之间自动选择：受管 G 用
 * `yield_current()`（不阻塞 M），普通线程用 `std::this_thread::yield()`
 * （让出时间片）。
 */
class alignas(64) SpinMutex final {
public:
    /// @brief 创建未加锁的 `SpinMutex`。
    SpinMutex() = default;

    /**
     * @brief 销毁 `SpinMutex`。
     * @warning 调用方必须已释放锁；与 `Mutex` 一样，销毁一把仍被持有的锁
     *          是未定义行为。
     */
    ~SpinMutex() = default;

    SpinMutex(const SpinMutex&) = delete;
    SpinMutex& operator=(const SpinMutex&) = delete;
    SpinMutex(SpinMutex&&) = delete;
    SpinMutex& operator=(SpinMutex&&) = delete;

    /**
     * @brief 阻塞获取锁，并进入「禁止 park」区域。
     * @throw std::logic_error 门被 Context 取消或调度器已不可用。
     */
    void Lock() {
        // 快路径：先 relaxed load 再 CAS。无竞争时只有一条 `lock cmpxchg`，
        // 且读阶段不会申请独占所有权（纯 load 不污染其他核的共享副本）。
        if (m_state.load(std::memory_order_relaxed) == 0U && TryAcquire()) {
            MarkOwnerChecked();
            return;
        }
        LockSlow();
    }

    /**
     * @brief 非阻塞尝试获取锁。
     * @return 成功返回 true 并进入「禁止 park」区域；失败返回 false。
     */
    bool TryLock() noexcept {
        if (m_state.load(std::memory_order_relaxed) != 0U) {
            return false;
        }
        if (!TryAcquire()) {
            return false;
        }
        try {
            MarkOwner();
        } catch (...) {
            m_state.store(0U, std::memory_order_release);
            return false;
        }
        return true;
    }

    /**
     * @brief 离开「禁止 park」区域并释放锁。
     * @note 先递减计数、再 store 释放：释放之后本 G 不会再被自己的计数
     *       误伤；等待者观察到 0 时，计数也已经归零。
     * @note 释放只有一次 release store，**不需要**判断/唤醒门——门里的
     *       等待者会自己轮询重试（门只负责限流与 park，不承担所有权转移）。
     * @throw std::logic_error 锁未被持有时。
     */
    void Unlock() {
        if (!m_owner) {
            // 普通线程加锁的快路径：没有受管 G 计数需要回滚。
            if (m_state.load(std::memory_order_relaxed) == 0U) {
                throw std::logic_error(
                    "go2cpp::sync::SpinMutex unlock of unlocked mutex");
            }
            m_state.store(0U, std::memory_order_release);
            return;
        }
        std::shared_ptr<Task> owner = std::move(m_owner);
        m_owner.reset();
        owner->leave_no_park_section();
        m_state.store(0U, std::memory_order_release);
    }

    /**
     * @brief 在相对超时内获取锁，并进入「禁止 park」区域。
     *
     * @param timeout 最长等待时间；`<= 0` 等价于一次 `TryLock()`。
     * @return 成功返回 true；超时、Context 取消或调度器不可用返回 false。
     *         **失败时不修改锁状态**，也不改变 `no_park_depth`。
     * @note 这是防死锁的主动手段：等待锁的上限由调用方决定，而不是无限期。
     */
    bool LockFor(std::chrono::milliseconds timeout) {
        if (timeout.count() <= 0) {
            return TryLock();
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (m_state.load(std::memory_order_relaxed) == 0U && TryAcquire()) {
            MarkOwnerChecked();
            return true;
        }
        for (int attempt = 0; attempt < kSpinAttempts; ++attempt) {
            if (m_state.load(std::memory_order_relaxed) == 0U && TryAcquire()) {
                MarkOwnerChecked();
                return true;
            }
            detail::SpinRelax();
        }
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now);
            if (remaining.count() <= 0) {
                return false;
            }
            if (!m_impl.LockFor(remaining)) {
                return false;
            }
            if (TryAcquire()) {
                m_impl.Unlock();
                MarkOwnerChecked();
                return true;
            }
            m_impl.Unlock();
            Backoff();
        }
    }

    /// @brief `LockFor` 的等价拼写；语义与返回值完全相同。
    bool TryLockFor(std::chrono::milliseconds timeout) {
        return LockFor(timeout);
    }

    /**
     * @brief 查询锁当前是否被持有。
     * @note 只是诊断快照：读取后状态可能立刻变化，不能用于同步决策。
     */
    bool IsLocked() const noexcept {
        return m_state.load(std::memory_order_acquire) != 0U;
    }

    /**
     * @brief 当前持锁 G 的 `no_park_depth`（无持锁者或普通线程时为 0）。
     * @note 仅供诊断与测试；锁状态在读取后可能变化。
     */
    std::uint32_t OwnerNoParkDepth() const noexcept {
        return m_owner ? m_owner->no_park_depth() : 0U;
    }

    /// @brief `SpinMutex` 的 RAII 守卫。
    class Guard final {
    public:
        /// @brief 立即加锁；失败时抛异常，不构造守卫。
        explicit Guard(SpinMutex& mutex) : m_mutex(&mutex) { m_mutex->Lock(); }

        /// @brief 如果锁尚未释放则释放。
        ~Guard() {
            if (m_mutex != nullptr) {
                m_mutex->Unlock();
            }
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& other) noexcept : m_mutex(other.m_mutex) {
            other.m_mutex = nullptr;
        }
        Guard& operator=(Guard&&) = delete;

        /// @brief 提前释放；重复调用无副作用。
        void Unlock() {
            if (m_mutex != nullptr) {
                SpinMutex* mutex = m_mutex;
                m_mutex = nullptr;
                mutex->Unlock();
            }
        }

    private:
        SpinMutex* m_mutex;
    };

private:
    /// 锁状态位：0 = 空闲，1 = 已持有（`m_state` 的全部取值空间）。
    static constexpr std::uint8_t kLocked = 1U;
    /// 拿不到锁时的主动自旋次数（Go `active_spin` 的等价物，PAUSE 64 次）。
    static constexpr int kSpinAttempts = 64;
    /// 进门（park）被唤醒后的再自旋次数，避免「刚醒又睡」。
    static constexpr int kGateSpinAttempts = 16;

    /// @brief 一次 CAS 抢锁；成功即成为唯一持有者。
    bool TryAcquire() noexcept {
        std::uint8_t expected = 0U;
        return m_state.compare_exchange_weak(
            expected, kLocked, std::memory_order_acquire,
            std::memory_order_relaxed);
    }

    /// @brief 抢到锁之后记录 owner；失败则回滚锁状态并重新抛出。
    void MarkOwnerChecked() {
        try {
            MarkOwner();
        } catch (...) {
            m_state.store(0U, std::memory_order_release);
            throw;
        }
    }

    /**
     * @brief 让出执行机会。
     * @details 受管 G 走 `yield_current()`（调度器把 G 放回运行队列，
     *          **不阻塞 M**，同一 M 上的其他 G 继续跑）；普通线程走
     *          `std::this_thread::yield()`。这里不 `sleep`：自旋预算只有
     *          64 次，一次系统调用的代价远大于收益。
     */
    void Backoff() {
        Scheduler* scheduler = Scheduler::current_scheduler();
        if (scheduler != nullptr) {
            (void)scheduler->yield_current();
        } else {
            std::this_thread::yield();
        }
    }

    /**
     * @brief 竞争慢路径：自旋 → 让出 → 门内自旋 → 重试。
     * @throw std::logic_error 门被取消或调度器已不可用。
     */
    void LockSlow() {
        // 第一段：主动自旋（短临界区通常在这里就能等到）。
        for (int attempt = 0; attempt < kSpinAttempts; ++attempt) {
            // 先读后 CAS：自旋期间只做共享 load，不反复申请独占所有权，
            // 否则 4/16 线程时所有核都在同一条缓存行上互相打回，实测反而
            // 比直接排队慢 60% 以上（Go lock_futex.c 的 while+cas 同理）。
            if (m_state.load(std::memory_order_relaxed) == 0U && TryAcquire()) {
                MarkOwnerChecked();
                return;
            }
            detail::SpinRelax();
        }
        // 第二段：让出一次。若持锁者只是被抢占，这一步常能直接拿到。
        Backoff();
        // 第三段：门。同一时刻至多一个等待者在门内检查，避免所有等待者
        // 一起 hammer 同一条缓存行；门内再自旋一小段，覆盖「刚唤醒又
        // 被别人抢走」的窗口。
        for (;;) {
            if (!m_impl.Lock()) {
                throw std::logic_error(
                    "go2cpp::sync::SpinMutex lock was cancelled or scheduler "
                    "unavailable");
            }
            for (int attempt = 0; attempt < kGateSpinAttempts; ++attempt) {
                if (m_state.load(std::memory_order_relaxed) == 0U &&
                    TryAcquire()) {
                    m_impl.Unlock();
                    MarkOwnerChecked();
                    return;
                }
                detail::SpinRelax();
            }
            m_impl.Unlock();
            Backoff();
        }
    }

    /// 记录当前 G 的禁止 park 计数；普通线程或已释放时清空 `m_owner`。
    void MarkOwner() {
        Task* self = Scheduler::current_task_ptr();
        if (self == nullptr) {
            // 普通线程：不碰 m_owner（Unlock 已保证其为空），热路径只多
            // 一次 TLS 读，避免 shared_ptr 的原子操作。
            return;
        }
        // 先取 shared_ptr 再加计数：若 shared_from_this 抛异常，调用方回滚
        // 底层锁并重新抛出，计数不会留下半成品。
        std::shared_ptr<Task> owner = self->shared_from_this();
        self->enter_no_park_section();
        m_owner = std::move(owner);
    }

    /// 慢路径门：FIFO、可 park、可取消。**必须是第一个成员**——debug
    /// 资源登记依赖它与 `SpinMutex` 同址（见 `test_resource_history_stress`）。
    Mutex m_impl;
    /// 快路径唯一热字（0=空闲，1=已持有），独占一条缓存行。
    alignas(64) std::atomic<std::uint8_t> m_state{0U};
    /// 加锁时的受管 G；普通线程加锁时为空。
    /// @note 只在持锁期间访问，由 `m_state` 的所有权串行化；跨 G 解锁
    ///       依赖所有权移交本身提供的 happens-before（与 Go 风格一致）。
    ///       与 `m_state` 同一条缓存行：普通线程 `Unlock` 读它时不额外 miss。
    std::shared_ptr<Task> m_owner;
};

}  // namespace go2cpp::sync
