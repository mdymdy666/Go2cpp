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
 * 3. **可重入锁与「Go 风格所有权」冲突。** `sync::Mutex` 允许 A 加锁、B 解锁
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
 * - **普通 OS 线程**：没有受管 G 时退化为每线程唯一的 token。
 * - **手动 Fiber**：没有 Scheduler continuation / 受管 Task，因此也走 OS 线程
 *   路径。同一 OS 线程上交替运行的两个**手动** Fiber 会被视为同一 owner；
 *   这是「无受管 G 时退化为线程」的直接后果，需要严格 G 级所有权时请使用
 *   `Scheduler::spawn` 创建的受管 G。
 *
 * 两套身份编码进同一个 `std::uintptr_t` token，互不相交：
 *
 * - 受管 G：`reinterpret_cast<uintptr_t>(task) | 1`（奇数）；
 * - OS 线程：全局自增的偶数 id（从 2 开始）。
 *
 * 因此不需要哈希、不需要第二把锁：普通路径只有一次原子读；重入路径只由
 * owner 自己递增深度。真正的互斥与等待（Fiber park / 线程阻塞）全部交给
 * `sync::Mutex`。
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
 * - owner 记录的是 `Task*`；如果 G 没解锁就退出，指针会悬空 / 被新 Task 复用，
 *   之后任何比较都不可靠。必须在 G 退出前解锁（推荐 RAII）。
 * - 底层仍是 `sync::Mutex`，因此**临界区内不要 park**（IO / 阻塞等待）；需要
 *   IO 时请把数据拷出临界区再做。
 * - 销毁一把仍被持有的锁是未定义行为。
 */
#pragma once

#include "go2cpp/concurrency/scheduler/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>

namespace go2cpp::sync {

/**
 * @brief 可重入互斥锁：同一 G（或同一 OS 线程）可重复加锁，需同样次数解锁。
 *
 * @details 互斥与等待由 `sync::Mutex` 提供（Fiber park / 线程阻塞双路径）；
 *          owner 与递归深度用无锁 token 记录。只有 owner 会写深度，因此深度
 *          不需要 CAS；owner 的发布/撤销通过 release/acquire 配对，保证
 *          「看到 owner == 自己」时底层锁一定已被持有。
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
        const std::uintptr_t self = CurrentToken();
        if (m_owner.load(std::memory_order_acquire) == self) {
            AcquireRecursiveDepth();
            return;
        }
        // 真实阻塞发生在 m_impl；此时没有持有任何本对象的原子状态。
        if (!m_impl.Lock()) {
            throw std::logic_error(
                "go2cpp::sync::RecursiveMutex lock was cancelled or scheduler "
                "unavailable");
        }
        // 先写深度、再发布 owner：两者都是 release 语义，读者看到 owner ==
        // self 时深度必然已是 1。
        m_depth.store(1, std::memory_order_relaxed);
        m_owner.store(self, std::memory_order_release);
    }

    /**
     * @brief 解锁；深度归零时才释放底层锁。
     * @throw std::logic_error 当前执行流不是 owner（含锁未被持有）。
     */
    void Unlock() {
        const std::uintptr_t self = CurrentToken();
        if (m_owner.load(std::memory_order_acquire) != self) {
            throw std::logic_error(
                "go2cpp::sync::RecursiveMutex unlock by non-owner");
        }
        // 只有 owner 会写 m_depth，因此这里无需 CAS。
        if (m_depth.fetch_sub(1, std::memory_order_acq_rel) != 1U) {
            return;
        }
        // 先撤 owner、再释放底层锁：下一个 owner 在 m_impl.Lock() 返回后重新
        // 发布。若顺序反过来，中间窗口里别的执行流可能误判 owner。
        m_owner.store(0U, std::memory_order_release);
        m_impl.Unlock();
    }

    /**
     * @brief 非阻塞尝试加锁；owner 是当前执行流时同样增加递归深度。
     * @return 成功返回 true；被其他执行流持有返回 false。
     * @throw std::logic_error 递归深度超限（与 Lock 一致）。
     */
    bool TryLock() {
        const std::uintptr_t self = CurrentToken();
        if (m_owner.load(std::memory_order_acquire) == self) {
            AcquireRecursiveDepth();
            return true;
        }
        if (!m_impl.TryLock()) {
            return false;
        }
        m_depth.store(1, std::memory_order_relaxed);
        m_owner.store(self, std::memory_order_release);
        return true;
    }

    /**
     * @brief 当前执行流在该锁上的递归深度。
     * @return 非 owner 返回 0；owner 返回已加锁次数。
     */
    std::uint32_t RecursionCount() const noexcept {
        if (m_owner.load(std::memory_order_acquire) != CurrentToken()) {
            return 0;
        }
        return m_depth.load(std::memory_order_acquire);
    }

    /**
     * @brief 当前执行流是否持有该锁。
     * @return 持有（深度 > 0）返回 true。
     */
    bool IsLockedByCurrent() const noexcept { return RecursionCount() != 0; }

private:
    /// @brief 重入路径：只由 owner 执行，因此普通 load + store 即可。
    void AcquireRecursiveDepth() {
        if (m_depth.load(std::memory_order_relaxed) >= kMaxRecursionDepth) {
            throw std::logic_error(
                "go2cpp::sync::RecursiveMutex recursion depth limit (1000) "
                "exceeded");
        }
        m_depth.fetch_add(1, std::memory_order_relaxed);
    }

    /// @brief 当前执行流的 owner token：受管 G 用 Task*（奇数），否则偶数 id。
    static std::uintptr_t CurrentToken() noexcept {
        Task* const task = Scheduler::current_task_ptr();
        if (task != nullptr) {
            return reinterpret_cast<std::uintptr_t>(task) | 1U;
        }
        return CurrentThreadToken();
    }

    /// @brief 每线程唯一且非零的偶数 token（进程内自增）。
    ///
    /// 不用 `std::hash<std::thread::id>`：哈希碰撞在理论上可能，而 owner 判定
    /// 一旦碰撞就会把两个线程当成同一 owner，破坏互斥语义。这里直接分配唯一
    /// 值，代价是每线程一次原子自增。
    static std::uintptr_t CurrentThreadToken() noexcept {
        static thread_local std::uintptr_t token = 0;
        if (token == 0) {
            static std::atomic<std::uintptr_t> next_token{2};
            token = next_token.fetch_add(2, std::memory_order_relaxed);
        }
        return token;
    }

    /// 真正提供互斥与 Fiber/线程双等待路径的底层锁。
    Mutex m_impl;
    /// 当前 owner token；0 表示无人持有。
    std::atomic<std::uintptr_t> m_owner{0};
    /// 当前 owner 的递归深度；只由 owner 写。
    std::atomic<std::uint32_t> m_depth{0};
};

}  // namespace go2cpp::sync
