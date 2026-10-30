#pragma once

#include "go2cpp/context.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

// ---- 本次构建是否记录资源级事件（推导与 go2cpp/debug/debug_resources.hpp 一致）----
// sync.hpp 不包含那个较重的 debug 头，但 Mutex 的内联快路径必须知道「资源
// 事件是否参与编译」：参与时必须把每次加锁/解锁留在 .so 的完整实现里逐次
// 记录，不能走内联快路径。
#ifndef GO2CPP_DEBUG_RESOURCES
#if (defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP) || \
    (defined(GO2CPP_REPLAY) && GO2CPP_REPLAY)
#define GO2CPP_DEBUG_RESOURCES 1
#else
#define GO2CPP_DEBUG_RESOURCES 0
#endif
#endif

namespace go2cpp::sync {
namespace go2cpp::debug {
class DebugMutex;
class DebugConditionVariable;
}  // namespace go2cpp::debug

namespace detail {

/// @brief 持锁 park 检测开关（-1=未解析，0=关，1=开）。
/// @details 定义在 sync.cpp；写入口是 SetHeldLockParkDetection()。
///          在公共头暴露是为了让 Mutex 的内联快路径能以一次 relaxed load
///          读到它——否则每次加锁都要多做一次跨库函数调用。
extern std::atomic<int> g_held_park_detect;

/// @brief 检测开关的热路径读法：一次 relaxed load + 可预测分支。
/// @note 变量在 sync.cpp 里由静态初始化按「环境变量 / NDEBUG」定好初值，
///       因此热路径没有「尚未解析」这一层分支。
inline bool held_park_detection_on() noexcept {
    return g_held_park_detect.load(std::memory_order_relaxed) != 0;
}

}  // namespace detail


/**
 * @brief Mutex 的调试只读快照（第 8 轮，P3 锁观测）。
 *
 * @note **持有者身份不可知**：`Mutex::m_state` 只记录「已持有」与
 *       「有等待者」两个位，不记录 owner（Go 风格所有权允许 A 加锁、B 解锁）。
 *       因此本快照**不包含** owner 字段。持锁 park 检测在开启时会额外记录
 *       最近一次加锁的受管 G（诊断用的近似值），但不作为快照的权威 owner。
 */
struct MutexDebugSnapshot {
    bool held{false};                  ///< 是否已被持有。
    bool has_waiters{false};           ///< 队列标记位是否置位。
    std::size_t waiters{0};            ///< 等待队列长度（含 Fiber 与普通线程）。
    std::size_t native_waiters{0};     ///< 其中普通线程的数量。
    std::size_t fiber_waiters{0};      ///< 其中受管 Fiber 的数量（= waiters - native_waiters）。

    // ---- 状态字（第 26 轮补全：owner / kLocked / kWaiter / kStarving / kWoken）----
    // `state_bits` 是 `Mutex::m_state` 的原始值；四个 bit 与
    // Go sync.Mutex 对应：kLocked=1, kWaiter=2, kStarving=4, kWoken=8。
    // 单独给出布尔字段是为了让 CLI 不必硬编码位值；新增字段放在末尾，
    // 保持既有聚合初始化兼容。
    std::uint8_t state_bits{0};        ///< 原始状态字。
    bool locked{false};                ///< = held；kLocked 位。
    bool waiter_flag{false};           ///< kWaiter 位（队列非空的提示）。
    bool starving{false};              ///< kStarving 位（饥饿模式）。
    bool woken{false};                 ///< kWoken 位（唤醒令牌已发出）。
    ///
    /// **近似 owner**：`m_owner` 只在「持锁 park 检测」开启时由加锁路径写入，
    /// 且 Go 风格所有权允许 A 加锁、B 解锁；因此它只是最近一次记录的受管 G，
    /// 不是权威所有者。`owner_known=false` 表示没有记录（不是「无主」）。
    std::uint64_t owner_gid{0};
    bool owner_known{false};
};

// ---- 持锁 park 检测 ----
//
// 用户场景：G1 `Lock(A)` 成功后 `read(fd)` 被 hook、park 在 IO 上；
// G2 `Lock(A)` 于是永久阻塞。Go 也不会自动解锁，但 Go2Cpp 把
// read/write/connect 全 hook 了，用户很难意识到"我持着锁"。
//
// 检测**只告警 + 计数**：不 panic、不强制解锁，保持与 Go 一致的语义。
// 默认 Debug 构建开启、Release 构建关闭；环境变量
// `GO2CPP_DETECT_HELD_PARK=1/0` 或本组接口可运行时覆盖。
// 开启后每次加锁/解锁各多一次 relaxed 原子操作，默认关闭时
// 热路径只多一次原子读。

/** @brief 开启/关闭持锁 park 检测。 */
void SetHeldLockParkDetection(bool enabled) noexcept;
/** @brief 当前是否开启持锁 park 检测。 */
bool HeldLockParkDetection() noexcept;
/** @brief 进程内累计"持锁 park"次数（可接到 metrics/Prometheus）。 */
std::uint64_t HeldLockParkCount() noexcept;

/**
 * @brief ConditionVariable 的调试只读快照。
 * @note 通知次数需要额外计数，当前实现**未提供**，故不在此结构中虚构字段。
 */
struct ConditionVariableDebugSnapshot {
    std::size_t waiters{0};  ///< 等待队列长度（含 Fiber 与普通线程）。
};

/**
 * @brief WaitGroup 的调试只读快照。
 * @note `count` 与 `Count()` 同一来源；`waiters` 需持内部锁读取。
 */
struct WaitGroupDebugSnapshot {
    std::int64_t count{0};     ///< 当前计数（>0 表示仍有未完成的 Done）。
    std::size_t waiters{0};    ///< 正在 Wait 的等待者数量。
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
 * **调用约束**：在**持队列锁**时同步调用，**不得阻塞**。
 *   实现应当不抛异常；万一抛出，`Mutex::Unlock` 会在调用点捕获并退化为默认 FIFO。
 *   （注意：接口**刻意不加 `noexcept`** —— 若加了，`throw` 会直接 `std::terminate`，
 *    那样"异常被隔离"就只是空话。）
 *
 * **饥饿**：本接口不做饥饿保护。若策略可能长期跳过某个等待者，
 *   实现方需自行加入"跳过次数上限后强制 FIFO"的机制 —— 这是**策略的责任**。
 */
class WaitSelector {
public:
    virtual ~WaitSelector() = default;

    /// @return 要优先的候选下标；返回 `std::nullopt` 表示**不干预**（保持 FIFO）。
    virtual std::optional<std::size_t> Select(
        const std::vector<WaiterCandidate>& candidates) = 0;
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
 *
 * ## 快路径（第 27 轮：去间接）
 *
 * 状态字 `m_state` **内联在 Mutex 对象起始**（不再住在 `Impl` 里），因此
 * 无竞争 `Lock`/`Unlock` 只需一次 relaxed load + **一次 CAS**：既不解引用
 * `m_impl`（唯一的堆间接），也不查调度器、不碰等待队列、不调用任何虚函数。
 * 所有需要 `Impl` 的逻辑（自旋、FIFO 队列、饥饿交接、超时/取消清理、
 * 跨线程解锁唤醒）都留在慢路径，语义与之前完全一致。
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
    /**
     * @brief 阻塞获取锁（无 Context 重载）。
     *
     * @details 单独拆出无参重载，让 `mutex.Lock()` 不再构造一个空的
     *          `shared_ptr` 临时对象；无竞争路径只有一次 relaxed load +
     *          一次 CAS。快路径失败后交给 `LockImpl`（定义在 sync.cpp）。
     */
    bool Lock() {
#if GO2CPP_DEBUG_RESOURCES
        return LockImpl({}, false);
#else
        // 先 relaxed load 再 CAS：无竞争时只多一次 L1 load；一旦有等待者
        // （状态里带 kWaiter/kWoken），这避免了「先 CAS(0) 必然失败、再
        // CAS(expected) 成功」的第二次缓存行争用（4t/16t 实测快约 3 ns/op）。
        std::uint8_t expected = m_state.load(std::memory_order_relaxed);
        if ((expected & (kLockedBit | kStarvingBit)) == 0U &&
            m_state.compare_exchange_weak(
                expected,
                static_cast<std::uint8_t>(expected | kLockedBit),
                std::memory_order_acquire, std::memory_order_relaxed)) {
            if (detail::held_park_detection_on()) {
                NoteLockHeldSlow();
            }
            return true;
        }
        return LockImpl({}, false);
#endif
    }
    /** @brief 阻塞获取锁，支持 Context 取消。 */
    bool Lock(const ContextPtr& context) {
#if GO2CPP_DEBUG_RESOURCES
        return LockImpl(context, false);
#else
        if (context && context->IsDone()) {
            return false;
        }
        // 先 relaxed load 再 CAS：无竞争时只多一次 L1 load；一旦有等待者
        // （状态里带 kWaiter/kWoken），这避免了「先 CAS(0) 必然失败、再
        // CAS(expected) 成功」的第二次缓存行争用（4t/16t 实测快约 3 ns/op）。
        std::uint8_t expected = m_state.load(std::memory_order_relaxed);
        if ((expected & (kLockedBit | kStarvingBit)) == 0U &&
            m_state.compare_exchange_weak(
                expected,
                static_cast<std::uint8_t>(expected | kLockedBit),
                std::memory_order_acquire, std::memory_order_relaxed)) {
            if (detail::held_park_detection_on()) {
                NoteLockHeldSlow();
            }
            return true;
        }
        return LockImpl(context, false);
#endif
    }
    // 最多等待 timeout；parent/context 取消或超时返回 false。
    /** @brief 在相对超时内获取锁。 */
    bool LockFor(ContextDuration timeout,
                 const ContextPtr& parent = {});
    // 不阻塞地尝试加锁；成功返回 true。
    /** @brief 非阻塞尝试获取锁。 */
    bool TryLock() noexcept;
    // 释放锁并按 FIFO 唤醒下一个等待者；未持有锁时报告逻辑错误。
    /** @brief 释放锁并唤醒一个 FIFO 等待者。 */
    void Unlock() {
#if GO2CPP_DEBUG_RESOURCES
        UnlockSlow();
#else
        // 检测必须在 CAS 之前读 owner：释放之后 owner 可能已被新持有者改写。
        if (detail::held_park_detection_on()) {
            NoteUnlockHeldSlow();
        }
        std::uint8_t expected = kLockedBit;
        if (m_state.compare_exchange_strong(expected, 0U,
                                            std::memory_order_release,
                                            std::memory_order_acquire)) {
            return;
        }
        UnlockSlow();
#endif
    }

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

    // 状态位（Impl 里的同名常量直接引用这里，保证快路径与慢路径同源）。
    static constexpr std::uint8_t kLockedBit = 1U;
    static constexpr std::uint8_t kWaiterBit = 2U;
    static constexpr std::uint8_t kStarvingBit = 4U;
    static constexpr std::uint8_t kWokenBit = 8U;

    /** @brief 条件变量返回前使用；忽略 G 取消标记并完成不可中断重锁。 */
    bool LockForConditionWait();
    /** @brief 完整加锁实现：Lock 快路径失败后的慢路径落点。 */
    bool LockImpl(const ContextPtr& context, bool ignore_task_cancellation);
    /** @brief Unlock 快路径 CAS 失败后的队列交接（原 Unlock 实现）。 */
    void UnlockSlow();
    /** @brief 持锁 park 检测的冷路径；只在检测开关打开时调用。 */
    void NoteLockHeldSlow();
    void NoteUnlockHeldSlow();

    // 快路径状态字（kLocked=1, kWaiter=2, kStarving=4, kWoken=8）。
    // 内联在 Mutex 对象上：无竞争 Lock/Unlock 只碰这一个字节，不再经过
    // m_impl 指针解引用（那是一次额外的依赖 load）。独占一条缓存行，
    // 避免与 Impl 里的等待队列/等待者计数/owner 假共享。
    // **放在对象起始**：CAS 的地址就是 `this`，热路径少一条 `lea`。
    alignas(64) std::atomic<std::uint8_t> m_state{0U};

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
    // 调试只读快照，说明见 Mutex 中的同名方法。
    WaitGroupDebugSnapshot DebugSnapshotInternal() const;

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
