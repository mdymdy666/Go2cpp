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
 * ## `SpinMutex` 不是真自旋
 *
 * 协作式调度下自旋 = 烧 CPU 且不让出，**比 park 更糟**。`SpinMutex` 内部
 * 完全复用 `sync::Mutex`（FIFO 混合锁），只是在持锁期间给当前 G 打上
 * `no_park_depth` 标记。它的价值是**契约检查**：让「持锁 park」这类 bug
 * 在开发期立刻暴露，而不是变成线上偶发死锁。
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
 *   happens-before；若在没有任何同步的情况下跨线程解锁，锁本身也不安全。
 */
#pragma once

#include "go2cpp/concurrency/scheduler/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace go2cpp::sync {

/**
 * @brief 不可睡眠锁：临界区内禁止发生 park（协作点）。
 *
 * @details 内部复用 `sync::Mutex`，**不是** CAS 自旋锁。`Lock()` / `TryLock()`
 *          成功后给当前 G 增加 `no_park_depth`；`Unlock()` 先递减再释放
 *          底层锁。加锁时的 G 会被记录下来，因此跨 G 解锁也能精确递减。
 *
 * ## 与 `sync::Mutex` 的区别
 *
 * | 维度 | `Mutex` | `SpinMutex` |
 * |---|---|---|
 * | 临界区内 park | 允许 | **禁止**（debug abort / release 降级）|
 * | 典型用途 | 保护可能 IO 的临界区 | 保护极短纯计算临界区 |
 * | 底层实现 | FIFO 混合锁 | 同一个 FIFO 混合锁 + 计数 |
 *
 * ## 嵌套
 *
 * 允许同一 G 嵌套加锁（只要不形成真正的自死锁）。每层 `Lock` 使
 * `no_park_depth` 加一，每层 `Unlock` 减一；深度归零后才恢复可 park。
 *
 * ## 异常安全
 *
 * - `Lock()` 在底层锁被取消/调度器不可用时抛 `std::logic_error`，且不改变
 *   任何计数；
 * - `Unlock()` 若底层报告「未持锁」，会先回滚计数再重新抛出；
 * - 推荐使用 `Guard`（RAII），它保证正常路径与异常路径都会解锁。
 *
 * ## 与普通线程的关系
 *
 * 在普通 OS 线程（没有受管 G）上，`no_park_depth` 无处记录，`SpinMutex`
 * 退化为普通 `Mutex`。这是有意的：契约检查只对协作式 G 有意义。
 */
class SpinMutex final {
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
     * @throw std::logic_error 底层锁被 Context 取消或调度器已不可用。
     */
    void Lock() {
        if (!m_impl.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::SpinMutex lock was cancelled or scheduler "
                "unavailable");
        }
        try {
            MarkOwner();
        } catch (...) {
            // 计数没能建立时不能把锁留在持有状态。
            m_impl.Unlock();
            throw;
        }
    }

    /**
     * @brief 非阻塞尝试获取锁。
     * @return 成功返回 true 并进入「禁止 park」区域；失败返回 false。
     */
    bool TryLock() noexcept {
        if (!m_impl.TryLock()) {
            return false;
        }
        try {
            MarkOwner();
        } catch (...) {
            m_impl.Unlock();
            return false;
        }
        return true;
    }

    /**
     * @brief 离开「禁止 park」区域并释放锁。
     * @note 先递减计数、再释放底层锁：被唤醒的下一个 G 在其他 M 上运行时
     *       看到的是**它自己**的计数，与这里无关；这个顺序保证本 G 在
     *       释放锁之后不会再被自己的计数误伤。
     * @throw std::logic_error 底层报告未持锁时（计数已回滚）。
     */
    void Unlock() {
        if (!m_owner) {
            // 普通线程加锁的快路径：没有受管 G 计数需要回滚。
            m_impl.Unlock();
            return;
        }
        std::shared_ptr<Task> owner = std::move(m_owner);
        m_owner.reset();
        owner->leave_no_park_section();
        try {
            m_impl.Unlock();
        } catch (...) {
            owner->enter_no_park_section();
            m_owner = std::move(owner);
            throw;
        }
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
        if (!m_impl.LockFor(timeout)) {
            return false;
        }
        try {
            MarkOwner();
        } catch (...) {
            m_impl.Unlock();
            throw;
        }
        return true;
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
        try {
            return m_impl.DebugSnapshotInternal().held;
        } catch (...) {
            return false;
        }
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

    /// 底层 FIFO 混合锁；`SpinMutex` 的互斥语义完全由它提供。
    Mutex m_impl;
    /// 加锁时的受管 G；普通线程加锁时为空。
    /// @note 只在持锁期间访问，由 `m_impl` 的所有权串行化；跨 G 解锁
    ///       依赖所有权移交本身提供的 happens-before（与 Go 风格一致）。
    std::shared_ptr<Task> m_owner;
};

}  // namespace go2cpp::sync
