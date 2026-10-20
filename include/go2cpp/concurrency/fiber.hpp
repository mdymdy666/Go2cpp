/**
 * @file fiber.hpp
 * @brief Fiber（用户态协程）的公开接口与执行上下文结构。
 *
 * ## 模块定位
 *
 * Fiber 是 G 的**执行载体**：它持有一段独立栈和一份保存的寄存器上下文。
 * `Scheduler` 决定"下一个跑谁"，`Fiber` 负责"怎么把 CPU 交给它/从它拿回来"。
 *
 * 本文件只定义**数据与接口**，真正的换栈在 `context_backend`（汇编）里。
 *
 * ## 本文件定义什么
 *
 * - `FiberState`：Fiber 自身的生命周期状态（可运行/运行中/挂起/终止）；
 * - `SuspendReason`：挂起原因（yield / park / io wait / 被取消…）。
 *   这个枚举决定了恢复时走哪条路径，调试时非常有价值；
 * - `FiberExecutionBinding`：Fiber 与它当前所在的 M/P 的绑定信息；
 * - `FiberContextFrame`：一份保存的寄存器上下文；换栈就是换它；
 * - `FiberResumeResult`：恢复一个 Fiber 后的返回信息（是否已终止等）；
 * - `Fiber`：Fiber 对象本身（创建、启动、恢复、销毁）。
 *
 * ## 栈的来源
 *
 * 本项目用**每 Fiber 独立栈 + 池化复用**（`FiberBin`）。这与 coost 的
 * "共享栈"是相反的取舍：独立栈换来更低的切换成本，代价是更大的内存占用；
 * 池化则把重复建栈的开销摊掉。
 *
 * ## 跨进程迁移
 *
 * `GO2CPP_MIGRATION_API=ON` 时额外提供 `ExportMigrationImage` /
 * `ImportMigrationImage` 与 `Channel<T>` 的等待状态导出/接纳接口；
 * 默认关闭，关闭时类型布局、热路径与行为均不变。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace go2cpp {

// ---------------------------------------------------------------------------
// 跨进程迁移 API（GO2CPP_MIGRATION_API=ON 时启用）
// ---------------------------------------------------------------------------
#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API

/**
 * @brief 通道等待者的可迁移视图（最小子集）。
 *
 * ## 适用范围
 *
 * 只表达「无缓冲、单生产者单消费者、无 select、无 deadline/context」的
 * 阻塞等待：通道队列里恰好有一个 `PendingRecv`/`PendingSend` 节点。
 * 复杂场景（有缓冲、select、多个等待者）在 `Channel<T>::ExportWaitState`
 * 里明确拒绝，而不是退化成一个语义不同的快照。
 *
 * ## 地址字段的含义
 *
 * `node_address`/`control_block` 是 **std::shared_ptr 的两个机器字**：
 * 前者指向等待节点对象，后者指向引用计数控制块。跨进程恢复时两者都
 * 必须被重建（节点由 `Channel<T>::AdoptWaitState` 新建），因此这里
 * 同时导出，导入侧据旧值生成重定位映射。
 *
 * `pending_slot` 是**等待方栈上 `std::shared_ptr` 变量的地址**：恢复后
 * 它会被重定位到新栈的同一相对位置，并在该位置就地构造新的
 * `shared_ptr`，让引用计数正确接管节点生命周期。
 */
struct ChannelWaitState {
    /// 等待方向：0=无，1=接收（PendingRecv），2=发送（PendingSend）。
    std::uint32_t kind{0};
    std::uint32_t reserved{0};
    /// 通道对象地址（旧侧导出 / 新侧导入，仅诊断）。
    std::uint64_t channel_address{0};
    /// 等待节点地址（std::shared_ptr 的第一个机器字）。
    std::uint64_t node_address{0};
    /// 引用计数控制块地址（std::shared_ptr 的第二个机器字）。
    std::uint64_t control_block{0};
    /// 等待方栈上 shared_ptr 变量的地址；0 表示未记录。
    std::uint64_t pending_slot{0};
    /// 值类型大小（诊断/校验，防止恢复侧 T 不一致）。
    std::uint64_t value_bytes{0};
    /// 导出时节点是否已经就绪（诊断）。
    std::uint32_t has_value{0};
};

/**
 * @brief 与模板无关的通道迁移接口，由 `Channel<T>` 实现。
 *
 * Fiber 的导出/导入代码不认识 `T`，只通过本接口读写等待状态。默认构建
 * 不定义该类型，`Channel<T>` 也不会多出虚表。
 */
class ChannelMigrationBase {
public:
    virtual ~ChannelMigrationBase() = default;

    /**
     * @brief 导出当前唯一的等待者。
     * @param out 输出等待状态。
     * @param error 失败原因（复杂场景会明确说明拒绝理由）。
     * @return 成功返回 true。
     */
    virtual bool ExportWaitState(ChannelWaitState* out, std::string* error) = 0;

    /**
     * @brief 把导入的等待者重新挂回通道队列。
     * @param state 导出侧的状态；`pending_slot` 必须是**新栈**上的槽地址。
     * @param adopted 输出新节点/新控制块地址。
     * @param error 失败原因。
     * @return 成功返回 true。
     */
    virtual bool AdoptWaitState(const ChannelWaitState& state,
                                ChannelWaitState* adopted,
                                std::string* error) = 0;

    /// @brief 通道对象（`Channel<T>` 本体）的起始地址。
    /// @details `Channel<T>` 可能以非 0 偏移继承本接口；栈上保存的是
    ///          `Channel<T>*`，句柄重定位必须用它而不是基类子对象地址。
    virtual std::uint64_t MigrationObjectAddress() const noexcept = 0;

    /// @brief 通道对象的字节数（作为句柄重定位的内部偏移上界）。
    virtual std::uint64_t MigrationObjectSize() const noexcept = 0;

    /// @brief 等待节点对象的字节数（作为句柄重定位的内部偏移上界）。
    virtual std::uint64_t MigrationNodeSize() const noexcept = 0;

    /// @brief 释放 Adopt 期间额外持有的节点引用。
    virtual void ReleaseMigrationHold() noexcept = 0;
};

/**
 * @brief 迁移导入选项。
 *
 * @details `channel` 是快照中等待者要挂回的**当前进程**通道。快照带等待
 * 状态时必填；无等待状态（纯 park）可以留空。
 */
struct FiberMigrationOptions {
    /// 导入侧：等待者要挂回的通道；导出侧忽略。
    ChannelMigrationBase* channel{nullptr};
    /// 导入侧：是否校验可执行文件 build-id（默认校验，同版本才可恢复）。
    bool verify_build_id{true};
    /// 导入侧：是否强制校验 HMAC 签名（默认强制；仅负例测试可关闭）。
    bool verify_signature{true};
    /**
     * @brief 导出侧：是否允许把「未登记堆指针」清零后继续导出。
     *
     * @details 默认 false：指针审计遇到未登记堆/匿名映射指针就拒绝。
     * 真实 Fiber 的等待路径会在栈上留下容器内部的死值（deque block、
     * 临时 shared_ptr 等），它们不可证明一定活跃，也不可迁移。受控 demo
     * 可以显式打开本开关：这些槽在快照里被登记为 `ClearedHeapSlot`，
     * 恢复时写 0（不用则无害，用了会立即崩溃而不是悬空执行）。生产环境
     * 应消除残留指针并保持默认拒绝。
     */
    bool allow_dead_heap_slots{false};
};

#endif  // GO2CPP_MIGRATION_API

/** @brief Fiber 生命周期状态。 */
enum class FiberState : std::uint8_t {
    Ready,
    Running,
    Suspended,
    Completed,
    Failed,
};

/** @brief Fiber 最近一次挂起的原因。 */
enum class SuspendReason : std::uint8_t {
    None,
    Yield,
    Park,
    Io,
    Timer,
    Synchronization,
};

// Fiber 当前执行位置所对应的调度器元数据。这里只保存不拥有对象的
// 标识，避免 Fiber 模块依赖 scheduler 模块，也避免调度器生命周期被
// 调试信息反向延长。scheduler_token 通常是 Scheduler 地址的整数形式。
/** @brief Fiber 当前 G/M/P 执行绑定的观测快照。 */
struct FiberExecutionBinding {
    std::uintptr_t scheduler_token{0};
    std::uint64_t task_id{0};
    std::uint64_t machine_id{0};
    std::uint32_t processor_id{0};
    bool managed{false};

    bool operator==(const FiberExecutionBinding& other) const noexcept {
        return scheduler_token == other.scheduler_token &&
               task_id == other.task_id && machine_id == other.machine_id &&
               processor_id == other.processor_id && managed == other.managed;
    }
};

// 一个 Fiber 或线程 main_fiber 的可复制调试帧。main_fiber 没有 Fiber
// 对象，因此 id 为 0，并通过 main_fiber 字段区分。parent_id 是首次进入
// 时固定的逻辑父级；active_parent_id 是最近一次 resume 的实际调用者。
/** @brief Fiber 嵌套调用栈中的一层上下文信息。 */
struct FiberContextFrame {
    std::uint64_t id{0};
    std::uint64_t parent_id{0};
    std::uint64_t active_parent_id{0};
    std::size_t depth{0};
    bool main_fiber{false};
    bool alive{false};
    bool active{false};
    bool cancellation_requested{false};
    FiberState state{FiberState::Ready};
    SuspendReason suspend_reason{SuspendReason::None};
    std::thread::id last_thread{};
    FiberExecutionBinding execution{};
};

using FiberContextSnapshot = std::vector<FiberContextFrame>;

// ---------------------------------------------------------------------------
// 调试快照（只读）：栈范围 / committed / 父 Fiber
// ---------------------------------------------------------------------------

/**
 * @brief Fiber 栈的只读快照（诊断与重放用）。
 *
 * ## 边界
 * 全部地址都是进程虚拟地址；`has_stack=false` 表示该 Fiber 已终态、
 * 上下文已释放（`m_stack == nullptr`），此时地址字段全为 0。
 * `current_sp` 只在**当前线程正在执行该 Fiber** 时可读，其它情况为 0 ——
 * 不伪造一个「看起来有意义」的栈指针。
 */
struct FiberStackDebugSnapshot {
    std::uint64_t id{0};                      ///< Fiber 编号
    FiberState state{FiberState::Ready};      ///< 生命周期状态
    SuspendReason suspend_reason{SuspendReason::None}; ///< 最近挂起原因
    bool has_stack{false};                    ///< 是否仍有活跃栈（运行/挂起中）
    bool growable{false};                     ///< 是否启用按需增长
    std::size_t requested_bytes{0};           ///< 归一化后的初始提交请求
    std::size_t reserve_bytes{0};             ///< 预留虚拟地址空间
    std::size_t usable_bytes{0};              ///< 当前已提交区的可用量
    std::uintptr_t mapping_base{0};           ///< 映射底（预留区最低地址，含 guard）
    std::uintptr_t mapping_top{0};            ///< 映射顶（栈高地址）
    std::uintptr_t committed_begin{0};        ///< 当前已提交区底部
    std::uintptr_t current_sp{0};             ///< 仅当前线程正在执行时非 0
    std::uint64_t parent_id{0};               ///< 逻辑父 Fiber 编号
    std::uint64_t active_parent_id{0};        ///< 最近一次 resume 的实际调用者
    std::size_t nesting_depth{0};             ///< 嵌套深度（main_fiber 为 0）
};

// 一次 resume 的非异常结果。Fiber 不把 C++ 异常抛出到调度器；调用者
// 可以通过 failure 和 context_snapshot 显式取得失败原因及完整父链。
/** @brief resume 操作的可检查结果。 */
struct FiberResumeResult {
    bool accepted{false};
    FiberState state{FiberState::Ready};
    std::exception_ptr failure;
    FiberContextSnapshot context_snapshot;

    /** @brief 返回 Fiber 是否正常完成。 */
    bool completed() const noexcept { return state == FiberState::Completed; }
    /** @brief 返回 Fiber 是否以异常失败。 */
    bool failed() const noexcept { return state == FiberState::Failed; }
};

/**
 * 可在受控边界挂起和恢复的用户态 Fiber。
 *
 * 依赖：内部 Context 后端提供寄存器/栈切换，Scheduler 负责把 Fiber 作为
 * G 在 M/P 上排队；Fiber 不依赖调度器对象的生命周期，也不拥有 Scheduler。
 * 对上层提供顺序 resume、主动 Suspend、取消标记、嵌套调用链快照和失败
 * 结果。一个 Fiber 的 resume 调用必须串行；迁移到其他 OS 线程由调度器
 * 保证。Linux x86_64 使用项目内的上下文后端，其他平台可由构建选择后端。
 */
class Fiber {
public:
    using Function = std::function<void()>;

    /** @brief 返回默认用户栈大小（字节）。 */
    static constexpr std::size_t DefaultStackSize() noexcept {
        return 128U * 1024U;
    }

    // 可变长栈的默认虚拟地址预留量。每个 Fiber 只在栈顶提交初始可用量，
    // 其余页保持 PROT_NONE，由 SIGSEGV handler 按需 mprotect 增长。
    /** @brief 返回默认预留的虚拟栈空间（字节）。 */
    static constexpr std::size_t DefaultStackReserveSize() noexcept {
        return 8U * 1024U * 1024U;
    }

    // 创建 Fiber。function 是主体回调；stack_size 是请求的初始提交量
    // （字节），页对齐后的值决定实际可用栈；stack_reserve_size 是预留的
    // 虚拟地址空间（0 表示 DefaultStackReserveSize()）。growable 为 false、
    // reserve 不大于初始提交量、或 GO2CPP_GROWABLE_STACK=0 时退化为固定栈。
    // 非法或过小值会由实现归一化；构造失败会抛出异常。
    /** @brief 创建一个 Fiber。@param function 入口函数。@param stack_size 初始栈大小。
     *  @param stack_reserve_size 预留虚拟地址大小，0 用默认值。
     *  @param stack_growable 是否允许按需增长。 */
    explicit Fiber(Function function,
                   std::size_t stack_size = DefaultStackSize(),
                   std::size_t stack_reserve_size = 0,
                   bool stack_growable = true);
    // Scheduler 专用的 FiberBin 接口。只有已经完成或失败、且不再有
    // 可恢复上下文的内部 G Fiber 才允许回收到创建它的 M 的线程本地池。
    // 普通用户 Fiber 不应调用这两个接口；它们不会跨线程转移仍挂起的
    // Fiber，也不会改变 Fiber 的公开所有权规则。
    // 从调度器所属 M 的 FiberBin 获取或新建 Fiber。function 为新的主体，
    // stack_size 为栈容量，bin_capacity 为回收池上限；返回拥有唯一所有权
    // 的 Fiber 指针。
    /** @brief 从当前 M 的缓存获取或创建 Fiber。 */
    static std::unique_ptr<Fiber> AcquireForScheduler(
        Function function, std::size_t stack_size,
        std::size_t bin_capacity = 32U,
        std::size_t stack_reserve_size = 0,
        bool stack_growable = true);
    // 把已终态且上下文已释放的 Fiber 放回当前 M 的缓存。fiber 为空或
    // 不满足回收条件时会安全丢弃；bin_capacity 是缓存上限。
    /** @brief 将完成或失败的 Fiber 放回当前 M 缓存。 */
    static void RecycleForScheduler(std::unique_ptr<Fiber> fiber,
                                    std::size_t bin_capacity = 32U) noexcept;
    // 析构会请求取消并等待 Fiber 自然返回。Ready Fiber 会跳过主体；
    // Suspended Fiber 只有在固定父级/调用方仍可恢复时才会继续执行。
    // owner 必须长于所有 resume()；不能在 Fiber 自身执行期间析构。
    // 若挂起 Fiber 在错误的父级之外析构，运行时会安全放弃该上下文并
    // 标记 Failed，而不是释放后继续恢复或直接终止进程；这条路径无法
    // 展开挂起栈上的局部 RAII，因此规范代码应始终由固定父级收尾。
    // 忽略取消的主体可能阻塞析构。
    /** @brief 销毁 Fiber 并释放其栈资源。 */
    ~Fiber();
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;

    // 转移执行权到本 Fiber。返回 false 表示 Fiber 正在运行、已经完成/失败
    // 或被并发恢复；用户异常不会越过 Fiber 边界，而会保存到 failure()，
    // 并将状态置为 Failed。
    /** @brief 从上次挂起点恢复执行。@return 接受恢复请求返回 true。 */
    bool resume() noexcept;
    // 调度器内部的快速恢复入口。G 已经由 Scheduler 串行化 resume，首次
    // 进入仍校验父级，后续恢复跳过重复的父链元数据锁；普通用户必须使用
    // resume()，以保留严格的调用者校验。
    /** @brief Scheduler 内部恢复入口，校验父子 Fiber 链。 */
    bool resume_from_scheduler() noexcept;
    // 与 resume 相同，但返回显式状态和失败时的调用链快照，适合父 Fiber
    // 按 try/catch 风格决定继续、转换错误或向上报告。
    FiberResumeResult resume_result() noexcept;

    // 挂起当前运行中的 Fiber。若当前线程不在 Fiber 中，或 reason 为 None，
    // 返回 false；下一次 resume() 后从调用点继续执行。
    /** @brief 当前 Fiber 主动挂起并返回调度器。 */
    static bool Suspend(SuspendReason reason = SuspendReason::Yield) noexcept;
    // Scheduler 后端专用入口。当前实现与 Suspend 使用同一安全的上下文
    // 切换路径；独立出来是为了让 scheduler 在切换前先提交 G 的 park/
    // yield 状态，并在后续版本中沿嵌套 Fiber 父链传播唤醒。
    /** @brief Scheduler 专用挂起入口。 */
    static bool SuspendForScheduler(
        SuspendReason reason = SuspendReason::Park) noexcept;
    /** @brief 返回当前线程正在执行的 Fiber。 */
    static Fiber* Current() noexcept;
    /** @brief 返回当前 Fiber 是否收到取消请求。 */
    static bool CancellationRequested() noexcept;

    // 返回当前线程的执行上下文。没有运行 Fiber 时返回该线程的
    // main_fiber 帧；main_fiber 只是上下文根，不是可 resume 的 Fiber。
    /** @brief 返回当前 Fiber 栈顶上下文帧。 */
    static FiberContextFrame CurrentContext();
    /** @brief 返回从 main_fiber 到当前 Fiber 的上下文链。 */
    static FiberContextSnapshot CurrentContextSnapshot();

    // 调度器在进入 worker 任务前可把 M/P/G 标识发布到当前线程的
    // main_fiber。Fiber resume 时会继承调用者的绑定，迁移到另一个线程
    // 后也会重新记录该线程的实际绑定。
    /** @brief 记录当前 Fiber 所在 Scheduler/M/P 绑定。 */
    static void BindCurrentExecution(
        FiberExecutionBinding binding) noexcept;
    // Scheduler/Task 的取消是协作式的。该函数设置 Fiber 本地标记，主体在
    // 下一次 resume 前后通过 CancellationRequested() 观察它。
    /** @brief 设置取消请求，实际停止在下一次安全点处理。 */
    void RequestCancellation() noexcept;

    // 查询当前状态；返回 FiberState 枚举值。
    /** @brief 返回 Fiber 当前生命周期状态。 */
    FiberState state() const noexcept;
    // 查询最近一次挂起原因；返回 SuspendReason 枚举值。
    /** @brief 返回最近一次挂起原因。 */
    SuspendReason suspend_reason() const noexcept;
    // 返回 Fiber 捕获的异常；没有异常时返回空 exception_ptr。
    /** @brief 返回 Fiber 失败时保存的异常。 */
    std::exception_ptr failure() const;
    // 返回实际保留的栈容量（字节）。
    /** @brief 返回实际栈容量（字节）。 */
    std::size_t stack_size() const noexcept;

    // Fiber 的唯一调试编号。编号只在当前进程内有意义，不复用。
    /** @brief 返回 Fiber 唯一 ID。 */
    std::uint64_t id() const noexcept;
    // 返回本 Fiber 的调试帧；调用方不需要持有 Fiber 内部锁。
    /** @brief 返回当前 Fiber 调试帧。 */
    FiberContextFrame debug_info() const;
    FiberContextFrame DebugInfo() const { return debug_info(); }
    /**
     * @brief 返回栈范围、committed 与父 Fiber 的只读快照。
     * @note best-effort：与 Fiber 自身的 reset/析构并发时可能读到过渡值；
     *       本函数不加锁、不改状态，只用于诊断与重放记录。
     */
    FiberStackDebugSnapshot debug_stack() const;
    // 返回从本 Fiber 向上追溯的逻辑调用链。Fiber 被迁移或挂起后仍
    // 保留首次进入时的父级关系；父对象结束后由共享记录保留
    // alive=false 的墓碑帧。墓碑只用于观测、取消和诊断，不能恢复已销毁的父栈。
    /** @brief 返回父级到当前 Fiber 的调试帧链。 */
    FiberContextSnapshot context_snapshot() const;
    FiberContextSnapshot ContextSnapshot() const { return context_snapshot(); }
    // 当前/最近一次 resume 所使用的调度器绑定。该值只是不拥有的
    // 元数据，不能用于延长 Scheduler 或 Task 的生命周期。
    /** @brief 读取最近一次执行绑定。 */
    FiberExecutionBinding execution_binding() const noexcept;
    /** @brief 设置执行绑定元数据。 */
    void bind_execution(FiberExecutionBinding binding) noexcept;
    // 逻辑嵌套深度。线程 main_fiber 深度为 0，首次从它进入的 Fiber
    // 深度为 1。
    /** @brief 返回 Fiber 在嵌套链中的深度。 */
    std::size_t nesting_depth() const noexcept;

#if defined(GO2CPP_MIGRATION_API) && GO2CPP_MIGRATION_API
    /**
     * @brief 开启本 Fiber 的迁移等待路径。
     *
     * @details 只有显式开启后，`Channel<T>` 的阻塞收发才会走「等待节点
     * 状态外置 + 直接 park」的迁移友好路径。默认关闭，普通 Fiber 的行为
     * 与不编译迁移 API 时完全一致。必须在 Fiber 首次进入通道等待前调用。
     * @return 当前 Fiber 有效返回 true。
     */
    bool EnableMigration() noexcept;
    /** @brief 返回本 Fiber 是否开启了迁移等待路径。 */
    bool migration_enabled() const noexcept;
    /** @brief 返回当前构建是否包含迁移 API。 */
    static bool MigrationApiEnabled() noexcept { return true; }

    /**
     * @brief 导出挂起 Fiber 的跨进程镜像。
     *
     * @details 只允许在 Fiber 处于 park（等待通道）时调用。导出内容：
     * 栈快照 + 上下文保存区 + 等待状态；同时做指针审计，任何未登记的
     * 堆指针都会被拒绝（避免恢复侧执行悬空指针）。快照使用
     * `tools/migration` 的 v3 格式并强制 HMAC-SHA256 签名。
     * @param path 输出文件路径。
     * @param options 迁移选项（导出侧目前只用于扩展/诊断）。
     * @param error 失败原因。
     * @return 成功返回 true；不改变 Fiber 状态。
     */
    bool ExportMigrationImage(const std::string& path,
                              const FiberMigrationOptions& options = {},
                              std::string* error = nullptr) const;

    /**
     * @brief 在目标进程重建 Fiber。
     *
     * @details 按快照重建栈、重定位指针、把等待者挂回 `options.channel`，
     * 返回一个 Suspended 状态、可 `resume()` 的 Fiber。导入过程不会
     * 自动恢复执行；调用方应先完成必要的交接（例如发送消息），再
     * `resume()`。
     * @param path 快照文件路径。
     * @param options 迁移选项；快照带等待状态时 `channel` 必填。
     * @param error 失败原因。
     * @return 成功返回新 Fiber；失败返回空指针并填写 error。
     */
    static std::unique_ptr<Fiber> ImportMigrationImage(
        const std::string& path,
        const FiberMigrationOptions& options = {},
        std::string* error = nullptr);

    /**
     * @brief 内部接口：通道迁移等待路径登记等待节点。
     * @note 仅供 `Channel<T>` 的迁移等待子集调用，不是稳定用户 API。
     */
    bool BeginMigrationWait(ChannelMigrationBase* channel, void* node,
                            const void* pending_slot,
                            std::uint32_t kind) noexcept;
    /** @brief 内部接口：注销迁移等待登记。 */
    void EndMigrationWait() noexcept;
#else
    /** @brief 返回当前构建是否包含迁移 API。 */
    static bool MigrationApiEnabled() noexcept { return false; }
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp
