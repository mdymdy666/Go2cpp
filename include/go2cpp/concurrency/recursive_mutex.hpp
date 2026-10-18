/**
 * @file recursive_mutex.hpp
 * @brief 可重入互斥锁 `RecursiveMutex`（按 G / OS 线程记录 owner）。
 *
 * @date 2026-10-18
 * @author DeepSeek Agent
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## ⚠️ 为什么 Go 不提供可重入锁（必读）
 *
 * Go 的 `sync.Mutex` **有意**不可重入（[Go 文档](https://pkg.go.dev/sync#Mutex)：
 * "A locked Mutex is not associated with a particular goroutine... it is not
 * a recursive lock"）。这不是遗漏，而是设计决定：
 *
 * 1. **可重入锁会掩盖设计问题。** 需要重入通常意味着「持锁时回调了用户代码」：
 *    库函数持锁 → 调用回调 → 回调重新进入同一把锁。正确修法是缩小临界区、
 *    把回调移到锁外，或改用消息传递；可重入只是让这个错误不立刻崩。
 * 2. **可重入锁破坏「锁 = 临界区边界」的推理。** 重入后内层临界区并不会
 *    因为外层持锁而自动满足「无人并发修改」以外的条件（例如不变式可能只在
 *    外层入口成立）。审查者看到 `Lock` 就假设进入了干净状态，这个假设会失效。
 * 3. **可重入锁与「Go 风格所有权」冲突。** `sync.Mutex` 允许 A 加锁、B 解锁
 *    （所有权随 channel 等交接），可重入锁必须绑定 owner，二者语义不能共存。
 *
 * 因此本类只应作为**移植既有代码**（例如从 C++/Java 翻译过来、确实存在递归
 * 加锁的调用链）的过渡设施。新代码优先重构调用链；如果发现自己在写「持锁
 * 回调」，先停下来改结构，而不是换这把锁。项目里 `RWMutex` 的文档同样给出
 * 了「不要持锁 IO」的警告，二者是同一类问题的两面。
 *
 * ## owner 怎么记录
 *
 * - **受管 G**：owner = `Scheduler::current_task_ptr()` 返回的 `Task*`。
 *   G 可以在不同 worker 线程之间迁移，所以**不能用线程 id** 判定 owner；
 *   用 G 身份才能保证「同一 G 跨线程重入 / 解锁」成立。
 * - **普通 OS 线程**：没有受管 G 时退化为 `std::thread::id`。
 *
 * 两套身份在同一个锁里互不相等：G 与普通线程、不同 G、不同线程都是不同 owner。
 *
 * ## 语义
 *
 * | 操作 | owner 是当前执行流 | owner 是别人 / 无人 |
 * |---|---|---|
 * | `Lock` | 递归深度 +1，立即返回 | 阻塞到底层锁可用，然后成为 owner |
 * | `TryLock` | 递归深度 +1，返回 true | 尝试一次，失败返回 false |
 * | `Unlock` | 深度 -1；归零才真正释放 | 抛 `std::logic_error` |
 *
 * 递归深度上限 1000；超过抛 `std::logic_error`，且**不改变深度**。上限存在的
 * 意义是尽早暴露无限递归（例如环状结构遍历），而不是节省内存。
 *
 * ## 使用边界
 *
 * - 非 owner 解锁**一定**抛异常。这一点与 `sync::Mutex` 的 Go 风格所有权不同。
 * - owner 记录的是 `Task*`；如果 G 没解锁就退出，指针会悬空，之后任何比较都是
 *   未定义行为。必须在 G 退出前解锁（推荐 RAII）。
 * - 底层仍是 `sync::Mutex`，因此**临界区内不要 park**（IO / 阻塞等待）；需要
 *   IO 时请把数据拷出临界区再做。
 * - 销毁一把仍被持有的锁是未定义行为。
 */
#pragma once

#include "go2cpp/concurrency/scheduler/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace go2cpp::sync {

/**
 * @brief 可重入互斥锁：同一 G（或同一 OS 线程）可重复加锁，需同样次数解锁。
 *
 * @details 内部用 `sync::Mutex` 提供真正的互斥与协程 / 线程双等待路径；
 *          另用一把很短的 `std::mutex` 保护 owner 与递归深度。`std::mutex` 的
 *          临界区只有几次内存写、**不含任何 park / 阻塞**，因此不会挡住同一
 *          worker 上的其他 Fiber。真正的阻塞发生在 `m_impl.Lock()`，调用它时
 *          绝不持有 `m_state`。
 */
class RecursiveMutex final {
public:
    /// @brief 允许的最大递归深度；超过即抛异常（防无限递归）。
    static constexpr std::uint32_t kMaxRecursionDepth = 1000;

    /// @brief 创建未加锁的可重入锁。
    RecursiveMutex() = default;

    /**
     * @brief 销毁可重入锁。
     * @warning 调用方必须保证锁已完全释放（深度归零）；否则未定义行为。
     */
    ~RecursiveMutex() = default;

    RecursiveMutex(const RecursiveMutex&) = delete;
    RecursiveMutex& operator=(const RecursiveMutex&) = delete;
    RecursiveMutex(RecursiveMutex&&) = delete;
    RecursiveMutex& operator=(RecursiveMutex&&) = delete;

    /**
     * @brief 加锁；同一执行流重复调用只增加递归深度，不阻塞。
     * @throw std::logic_error 递归深度超过 `kMaxRecursionDepth`，或底层锁被
     *        取消 / 调度器不可用。
     */
    void Lock() {
        {
            std::lock_guard<std::mutex> lock(m_state);
            if (OwnedByCurrentLocked()) {
                if (m_depth >= kMaxRecursionDepth) {
                    throw std::logic_error(
                        "go2cpp::sync::RecursiveMutex recursion depth limit "
                        "(1000) exceeded");
                }
                ++m_depth;
                return;
            }
        }
        // 关键：调用可能 park 的底层锁时不得持有 m_state，否则同一 worker 上
        // 的其他 Fiber 会卡在 std::mutex 上，把协作式调度变成线程阻塞。
        if (!m_impl.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::RecursiveMutex lock was cancelled or scheduler "
                "unavailable");
        }
        std::lock_guard<std::mutex> lock(m_state);
        m_owner = CurrentOwner();
        m_depth = 1;
    }

    /**
     * @brief 解锁；深度归零时才释放底层锁。
     * @throw std::logic_error 当前执行流不是 owner（含锁未被持有）。
     */
    void Unlock() {
        {
            std::lock_guard<std::mutex> lock(m_state);
            if (!OwnedByCurrentLocked()) {
                throw std::logic_error(
                    "go2cpp::sync::RecursiveMutex unlock by non-owner");
            }
            --m_depth;
            if (m_depth != 0) {
                return;
            }
            m_owner = Owner{};
        }
        // 先清 owner、再释放底层锁：下一个 owner 在 m_impl.Lock() 返回后写
        // owner。若顺序反过来，中间窗口里别的执行流可能误判 owner。
        m_impl.Unlock();
    }

    /**
     * @brief 非阻塞尝试加锁；owner 是当前执行流时同样增加递归深度。
     * @return 成功返回 true；被其他执行流持有返回 false。
     * @throw std::logic_error 递归深度超限（与 Lock 一致）。
     */
    bool TryLock() {
        {
            std::lock_guard<std::mutex> lock(m_state);
            if (OwnedByCurrentLocked()) {
                if (m_depth >= kMaxRecursionDepth) {
                    throw std::logic_error(
                        "go2cpp::sync::RecursiveMutex recursion depth limit "
                        "(1000) exceeded");
                }
                ++m_depth;
                return true;
            }
        }
        if (!m_impl.TryLock()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_state);
        m_owner = CurrentOwner();
        m_depth = 1;
        return true;
    }

    /**
     * @brief 当前执行流在该锁上的递归深度。
     * @return 非 owner 返回 0；owner 返回已加锁次数。
     * @note noexcept：内部状态短暂上锁，失败时返回 0。
     */
    std::uint32_t RecursionCount() const noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_state);
            return OwnedByCurrentLocked() ? m_depth : 0;
        } catch (...) {
            return 0;
        }
    }

    /**
     * @brief 当前执行流是否持有该锁。
     * @return 持有（深度 > 0）返回 true。
     */
    bool IsLockedByCurrent() const noexcept { return RecursionCount() != 0; }

private:
    /// 所有者身份：受管 G 用 `Task*`，普通线程用 `std::thread::id`。
    struct Owner {
        Task* task{nullptr};
        std::thread::id thread{};
    };

    /// @brief 取当前执行流的 owner 标识（不 park、不做原子引用计数）。
    static Owner CurrentOwner() noexcept {
        Owner owner;
        owner.task = Scheduler::current_task_ptr();
        if (owner.task == nullptr) {
            owner.thread = std::this_thread::get_id();
        }
        return owner;
    }

    /// @brief owner 相等判定；`task` 非空时忽略线程 id（G 可迁移）。
    static bool SameOwner(const Owner& lhs, const Owner& rhs) noexcept {
        if (lhs.task != nullptr || rhs.task != nullptr) {
            return lhs.task == rhs.task;
        }
        return lhs.thread == rhs.thread;
    }

    /// @brief 调用方必须持有 `m_state`。
    bool OwnedByCurrentLocked() const noexcept {
        return m_depth != 0 && SameOwner(m_owner, CurrentOwner());
    }

    /// 保护 owner / 深度的短临界区互斥量（纯内存操作，无阻塞）。
    mutable std::mutex m_state;
    /// 真正的互斥与等待路径；协程 park / 线程阻塞由它内部决定。
    Mutex m_impl;

    /// 当前 owner；深度为 0 时为空。
    Owner m_owner{};
    /// 当前 owner 的递归深度。
    std::uint32_t m_depth{0};
};

}  // namespace go2cpp::sync
