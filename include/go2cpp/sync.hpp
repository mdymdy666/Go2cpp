#pragma once

#include "go2cpp/context.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace go2cpp::sync {
namespace go2cpp::debug {
class DebugMutex;
class DebugConditionVariable;
}  // namespace go2cpp::debug


/**
 * @brief Mutex 的调试只读快照（第 8 轮，P3 锁观测）。
 *
 * @note **持有者身份不可知**：`Mutex::Impl::m_state` 只记录「已持有」与
 *       「有等待者」两个位，不记录 owner（Go 风格所有权允许 A 加锁、B 解锁）。
 *       因此本快照**不包含** owner 字段；要支持它必须新增 owner 追踪并声明行为差异。
 */
struct MutexDebugSnapshot {
    bool held{false};                  ///< 是否已被持有。
    bool has_waiters{false};           ///< 队列标记位是否置位。
    std::size_t waiters{0};            ///< 等待队列长度（含 Fiber 与普通线程）。
    std::size_t native_waiters{0};     ///< 其中普通线程的数量。
    std::size_t fiber_waiters{0};      ///< 其中受管 Fiber 的数量（= waiters - native_waiters）。
};

/**
 * @brief ConditionVariable 的调试只读快照。
 * @note 通知次数需要额外计数，当前实现**未提供**，故不在此结构中虚构字段。
 */
struct ConditionVariableDebugSnapshot {
    std::size_t waiters{0};  ///< 等待队列长度（含 Fiber 与普通线程）。
};


/**
 * @brief Fiber 等待后端的最小可替换协议。
 * @details sync 公共头不依赖 Scheduler 的具体类，使用不透明指针和共享
 *          Task 句柄传递运行时上下文。默认实现适配 Go2Cpp Scheduler；
 *          其他 Fiber 后端可以注册工厂而不改动 Mutex/ConditionVariable。
 */
class WaitBackend {
public:
    /** @brief 释放等待后端。 */
    virtual ~WaitBackend() = default;

    /**
     * @brief park 当前受管任务，直到谓词、取消或截止时间满足。
     * @param scheduler 不透明调度器指针；由工厂与调用方约定其类型。
     * @param task 不透明任务共享句柄；后端负责保持其生命周期。
     * @param deadline 可选绝对截止时间。
     * @param predicate 状态谓词，后端必须允许虚假唤醒后重复检查。
     * @return 谓词满足返回 true；取消、停止或超时返回 false。
     */
    virtual bool Park(
        void* scheduler, const std::shared_ptr<void>& task,
        std::optional<ContextTimePoint> deadline,
        const std::function<bool()>& predicate) = 0;

    /**
     * @brief 为条件变量重锁执行不可被 G 取消中断的 park。
     * @details 默认转发 Park；Scheduler 后端覆盖此方法，以允许 draining 中
     *          已启动 G 暂停等待锁 owner 释放锁，保证重锁后再退出。
     */
    virtual bool ParkForCleanup(
        void* scheduler, const std::shared_ptr<void>& task,
        const std::function<bool()>& predicate) {
        return Park(scheduler, task, std::nullopt, predicate);
    }

    /** @brief 唤醒指定受管任务；失败不得抛出异常。 */
    virtual void Wake(void* scheduler,
                      const std::shared_ptr<void>& task) noexcept = 0;
};

using WaitBackendFactory = std::function<std::shared_ptr<WaitBackend>()>;

/**
 * @brief 等待队列中的一个候选者（**不含内部节点类型**，因此可安全暴露）。
 * @note `wait_ns`（已等待时长）当前**没有**提供：`WaitNode` 未记录等待起点，
 *       要支持它需要改动节点结构，本轮不做，也不虚构字段。
 */
struct WaiterCandidate {
    std::size_t index{0};    ///< 在等待队列中的位置（0 = FIFO 队首）。
    bool is_fiber{false};    ///< 是否受管 Fiber（false 表示普通线程）。
};

/**
 * @brief 锁争用时的等待者选择策略（D3）。
 *
 * **语义边界（必须遵守）**：
 *   选择器**只能改变等待队列的顺序**，不能改变"谁最终获得锁"的裁决方式 ——
 *   排序之后仍然由 `Mutex::Unlock` 按 FIFO 逐个 `TryFinish` 认领，
 *   因此「所有权**无缝转移**给下一个持有者、`TryLock` 调用者**不能插队**」
 *   这两条既有语义**完全不受影响**。
 *
 * **调用约束**：在**持队列锁**时同步调用；实现**不得阻塞、不得抛异常**
 *   （抛出的异常会被运行时隔离并退化为默认 FIFO）。
 *
 * **饥饿**：本接口不做饥饿保护。若策略可能长期跳过某个等待者，
 *   实现方需自行加入"跳过次数上限后强制 FIFO"的机制 —— 这是**策略的责任**。
 */
class WaitSelector {
public:
    virtual ~WaitSelector() = default;

    /// @return 要优先的候选下标；返回 `std::nullopt` 表示**不干预**（保持 FIFO）。
    virtual std::optional<std::size_t> Select(
        const std::vector<WaiterCandidate>& candidates) noexcept = 0;
};

using WaitSelectorFactory = std::function<std::shared_ptr<WaitSelector>()>;

/**
 * @brief 注册锁争用选择器工厂。
 * @param factory 自定义选择器；为空表示恢复默认（纯 FIFO）。
 * @note 与 `SetWaitBackendFactory` 同风格：**只影响之后发生的争用**，
 *       且默认（未注册）时热路径只多一次原子判空。
 */
bool SetWaitSelectorFactory(WaitSelectorFactory factory);

/** @brief 清除自定义选择器，恢复纯 FIFO。 */
void ResetWaitSelectorFactory() noexcept;


/**
 * @brief 注册新建同步等待节点所使用的后端工厂。
 * @param factory 自定义后端；为空表示恢复默认 Scheduler 后端。
 * @return 工厂发布成功返回 true。
 * @note 只影响注册后创建的等待节点，正在等待的 Fiber 不会迁移后端。
 */
bool SetWaitBackendFactory(WaitBackendFactory factory);

/** @brief 清除自定义同步等待后端。 */
void ResetWaitBackendFactory() noexcept;

/** @brief 为一个等待节点创建后端，失败时回退默认 Scheduler 适配器。 */
std::shared_ptr<WaitBackend> CreateWaitBackend() noexcept;

/**
 * 支持调度器的 FIFO 混合互斥锁。
 *
 * 依赖：Context 提供取消/截止时间，Scheduler 提供 G 的 park/wake；普通
 * 线程竞争时使用原生 condition_variable。两类调用者共享一个 FIFO 交接
 * 队列，因此 Fiber 加锁后可以由其他 Fiber 或 OS 线程解锁。
 * 对上层提供 Lock/TryLock/LockFor 及 BasicLockable 小写接口。对象必须
 * 长于所有使用者和等待者；不支持递归加锁，成功 Lock 必须恰好对应一次
 * Unlock。Unlock 不要求由同一 G/线程调用，采用 Go 风格所有权。
 */
class Mutex final {
public:
    /** @brief 创建未加锁的混合 Mutex。 */
    Mutex();
    /** @brief 销毁 Mutex；调用方必须先释放锁。 */
    ~Mutex();

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    // 阻塞直到获得锁或 context 取消；返回 true 表示已持有锁。
    /** @brief 阻塞获取锁，支持 Context 取消。 */
    bool Lock(const ContextPtr& context = {});
    // 最多等待 timeout；parent/context 取消或超时返回 false。
    /** @brief 在相对超时内获取锁。 */
    bool LockFor(ContextDuration timeout,
                 const ContextPtr& parent = {});
    // 不阻塞地尝试加锁；成功返回 true。
    /** @brief 非阻塞尝试获取锁。 */
    bool TryLock() noexcept;
    // 释放锁并按 FIFO 唤醒下一个等待者；未持有锁时报告逻辑错误。
    /** @brief 释放锁并唤醒一个 FIFO 等待者。 */
    void Unlock();

    // BasicLockable 兼容拼写。lock() 在普通线程上竞争时阻塞；受管 G
    // 无法继续等待（例如 Scheduler 正在关闭）时抛出 std::logic_error。
    void lock();
    bool try_lock() noexcept { return TryLock(); }
    void unlock() { Unlock(); }

    // 调试只读快照。**放在 public 并冠以 Debug 前缀**：原设计是 friend + private，
    // 但在本项目的包含顺序下 friend 未对该内联代理生效（编译报 private 访问错误）。
    // 为保持零行为影响，选择公开 + 显式命名，而不是去掉原类的 final 或改动包含结构。
    // 调用方应仅由 go2cpp::debug 模块使用。
    MutexDebugSnapshot DebugSnapshotInternal() const;

private:

    friend class ConditionVariable;
    /** @brief 条件变量返回前使用；忽略 G 取消标记并完成不可中断重锁。 */
    bool LockForConditionWait();
    bool LockImpl(const ContextPtr& context, bool ignore_task_cancellation);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/**
 * 与 sync::Mutex 配套的调度器条件变量。
 *
 * 依赖：Mutex 的解锁/重锁语义、Context 的取消和 Scheduler 的 Fiber park。
 * 对上层提供 Wait/WaitFor、NotifyOne/NotifyAll；受管 G 挂起 Fiber，普通
 * 线程阻塞原生条件变量，双方共享 FIFO 等待队列。正常通知或取消返回前
 * 都会重新获得 mutex。没有 Scheduler 的手动 resume Fiber 不受支持；关闭
 * 阶段如果无法立即重锁，Wait 返回 false 且 mutex 已解锁。
 */
class ConditionVariable final {
public:
    /** @brief 创建 Fiber/线程混合条件变量。 */
    ConditionVariable();
    ~ConditionVariable();

    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;
    ConditionVariable(ConditionVariable&&) = delete;
    ConditionVariable& operator=(ConditionVariable&&) = delete;

    // 释放 mutex 并等待通知/取消，然后重新获得 mutex；返回是否正常
    // 完成重锁，mutex 参数必须由调用方在进入前持有。
    /** @brief 原子释放 mutex 并等待通知或取消。 */
    bool Wait(Mutex& mutex, const ContextPtr& context = {});
    // 最多等待 timeout，参数 parent 为可选取消上下文；返回规则同 Wait。
    /** @brief 带超时等待通知。 */
    bool WaitFor(Mutex& mutex, ContextDuration timeout,
                 const ContextPtr& parent = {});
    // 唤醒一个等待者；没有等待者时通知不会累积。
    /** @brief 唤醒一个条件变量等待者。 */
    void NotifyOne() noexcept;
    // 唤醒当前所有等待者。
    /** @brief 唤醒全部条件变量等待者。 */
    void NotifyAll() noexcept;

    bool wait(Mutex& mutex, const ContextPtr& context = {}) {
        return Wait(mutex, context);
    }
    bool wait_for(Mutex& mutex, ContextDuration timeout,
                  const ContextPtr& parent = {}) {
        return WaitFor(mutex, timeout, parent);
    }
    void notify_one() noexcept { NotifyOne(); }
    void notify_all() noexcept { NotifyAll(); }

    // 调试只读快照，说明见 Mutex 中的同名方法。
    ConditionVariableDebugSnapshot DebugSnapshotInternal() const;

private:

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/**
 * 同时支持受管 G 与普通线程的可复用 WaitGroup。
 *
 * 依赖：Context、Scheduler park/wake 和原生 condition_variable。对上层提供
 * Add/Done/Wait/WaitFor/Count；计数归零时唤醒当前波次，之后 Add 可开启新
 * 波次。计数下溢或有符号溢出抛出 std::logic_error，且计数保持不变。
 */
class WaitGroup final {
public:
    /** @brief 创建计数为零的 WaitGroup。 */
    WaitGroup();
    ~WaitGroup();

    WaitGroup(const WaitGroup&) = delete;
    WaitGroup& operator=(const WaitGroup&) = delete;
    WaitGroup(WaitGroup&&) = delete;
    WaitGroup& operator=(WaitGroup&&) = delete;

    // 将计数增加 delta；负值不能使计数下溢。
    /** @brief 增减等待计数；计数不能变为负数。 */
    void Add(std::int64_t delta);
    // 将计数减少一；计数为零时唤醒等待者。
    /** @brief 将计数减一并在归零时唤醒等待者。 */
    void Done();
    // 等待计数归零或 context 取消；返回 true 表示归零。
    /** @brief 等待计数归零或 Context 取消。 */
    bool Wait(const ContextPtr& context = {});
    // 最多等待 timeout；parent/context 取消或超时返回 false。
    /** @brief 在相对超时内等待计数归零。 */
    bool WaitFor(ContextDuration timeout,
                 const ContextPtr& parent = {});
    // 返回当前计数快照；结果仅表示调用时刻，不阻止随后 Add/Done。
    /** @brief 返回当前等待计数。 */
    std::int64_t Count() const noexcept;

    void add(std::int64_t delta) { Add(delta); }
    void done() { Done(); }
    bool wait(const ContextPtr& context = {}) { return Wait(context); }
    bool wait_for(ContextDuration timeout,
                  const ContextPtr& parent = {}) {
        return WaitFor(timeout, parent);
    }
    std::int64_t count() const noexcept { return Count(); }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::sync
