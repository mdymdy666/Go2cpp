#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace go2cpp {

enum class FiberState : std::uint8_t {
    Ready,
    Running,
    Suspended,
    Completed,
    Failed,
};

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

// 一次 resume 的非异常结果。Fiber 不把 C++ 异常抛出到调度器；调用者
// 可以通过 failure 和 context_snapshot 显式取得失败原因及完整父链。
struct FiberResumeResult {
    bool accepted{false};
    FiberState state{FiberState::Ready};
    std::exception_ptr failure;
    FiberContextSnapshot context_snapshot;

    bool completed() const noexcept { return state == FiberState::Completed; }
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

    static constexpr std::size_t DefaultStackSize() noexcept {
        return 128U * 1024U;
    }

    // 创建 Fiber。function 是主体回调；stack_size 是请求的栈容量（字节），
    // 非法或过小值会由实现归一化为可用容量；构造失败会抛出异常。
    explicit Fiber(Function function,
                   std::size_t stack_size = DefaultStackSize());
    // Scheduler 专用的 FiberBin 接口。只有已经完成或失败、且不再有
    // 可恢复上下文的内部 G Fiber 才允许回收到创建它的 M 的线程本地池。
    // 普通用户 Fiber 不应调用这两个接口；它们不会跨线程转移仍挂起的
    // Fiber，也不会改变 Fiber 的公开所有权规则。
    // 从调度器所属 M 的 FiberBin 获取或新建 Fiber。function 为新的主体，
    // stack_size 为栈容量，bin_capacity 为回收池上限；返回拥有唯一所有权
    // 的 Fiber 指针。
    static std::unique_ptr<Fiber> AcquireForScheduler(
        Function function, std::size_t stack_size,
        std::size_t bin_capacity = 32U);
    // 把已终态且上下文已释放的 Fiber 放回当前 M 的缓存。fiber 为空或
    // 不满足回收条件时会安全丢弃；bin_capacity 是缓存上限。
    static void RecycleForScheduler(std::unique_ptr<Fiber> fiber,
                                    std::size_t bin_capacity = 32U) noexcept;
    // 析构会请求取消并等待 Fiber 自然返回。Ready Fiber 会跳过主体；
    // Suspended Fiber 只有在固定父级/调用方仍可恢复时才会继续执行。
    // owner 必须长于所有 resume()；不能在 Fiber 自身执行期间析构。
    // 若挂起 Fiber 在错误的父级之外析构，运行时会安全放弃该上下文并
    // 标记 Failed，而不是释放后继续恢复或直接终止进程；这条路径无法
    // 展开挂起栈上的局部 RAII，因此规范代码应始终由固定父级收尾。
    // 忽略取消的主体可能阻塞析构。
    ~Fiber();
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;

    // 转移执行权到本 Fiber。返回 false 表示 Fiber 正在运行、已经完成/失败
    // 或被并发恢复；用户异常不会越过 Fiber 边界，而会保存到 failure()，
    // 并将状态置为 Failed。
    bool resume() noexcept;
    // 调度器内部的快速恢复入口。G 已经由 Scheduler 串行化 resume，首次
    // 进入仍校验父级，后续恢复跳过重复的父链元数据锁；普通用户必须使用
    // resume()，以保留严格的调用者校验。
    bool resume_from_scheduler() noexcept;
    // 与 resume 相同，但返回显式状态和失败时的调用链快照，适合父 Fiber
    // 按 try/catch 风格决定继续、转换错误或向上报告。
    FiberResumeResult resume_result() noexcept;

    // 挂起当前运行中的 Fiber。若当前线程不在 Fiber 中，或 reason 为 None，
    // 返回 false；下一次 resume() 后从调用点继续执行。
    static bool Suspend(SuspendReason reason = SuspendReason::Yield) noexcept;
    // Scheduler 后端专用入口。当前实现与 Suspend 使用同一安全的上下文
    // 切换路径；独立出来是为了让 scheduler 在切换前先提交 G 的 park/
    // yield 状态，并在后续版本中沿嵌套 Fiber 父链传播唤醒。
    static bool SuspendForScheduler(
        SuspendReason reason = SuspendReason::Park) noexcept;
    static Fiber* Current() noexcept;
    static bool CancellationRequested() noexcept;

    // 返回当前线程的执行上下文。没有运行 Fiber 时返回该线程的
    // main_fiber 帧；main_fiber 只是上下文根，不是可 resume 的 Fiber。
    static FiberContextFrame CurrentContext();
    static FiberContextSnapshot CurrentContextSnapshot();

    // 调度器在进入 worker 任务前可把 M/P/G 标识发布到当前线程的
    // main_fiber。Fiber resume 时会继承调用者的绑定，迁移到另一个线程
    // 后也会重新记录该线程的实际绑定。
    static void BindCurrentExecution(
        FiberExecutionBinding binding) noexcept;
    // Scheduler/Task 的取消是协作式的。该函数设置 Fiber 本地标记，主体在
    // 下一次 resume 前后通过 CancellationRequested() 观察它。
    void RequestCancellation() noexcept;

    // 查询当前状态；返回 FiberState 枚举值。
    FiberState state() const noexcept;
    // 查询最近一次挂起原因；返回 SuspendReason 枚举值。
    SuspendReason suspend_reason() const noexcept;
    // 返回 Fiber 捕获的异常；没有异常时返回空 exception_ptr。
    std::exception_ptr failure() const;
    // 返回实际保留的栈容量（字节）。
    std::size_t stack_size() const noexcept;

    // Fiber 的唯一调试编号。编号只在当前进程内有意义，不复用。
    std::uint64_t id() const noexcept;
    // 返回本 Fiber 的调试帧；调用方不需要持有 Fiber 内部锁。
    FiberContextFrame debug_info() const;
    FiberContextFrame DebugInfo() const { return debug_info(); }
    // 返回从本 Fiber 向上追溯的逻辑调用链。Fiber 被迁移或挂起后仍
    // 保留首次进入时的父级关系；父对象结束后由共享记录保留
    // alive=false 的墓碑帧。墓碑只用于观测、取消和诊断，不能恢复已销毁的父栈。
    FiberContextSnapshot context_snapshot() const;
    FiberContextSnapshot ContextSnapshot() const { return context_snapshot(); }
    // 当前/最近一次 resume 所使用的调度器绑定。该值只是不拥有的
    // 元数据，不能用于延长 Scheduler 或 Task 的生命周期。
    FiberExecutionBinding execution_binding() const noexcept;
    void bind_execution(FiberExecutionBinding binding) noexcept;
    // 逻辑嵌套深度。线程 main_fiber 深度为 0，首次从它进入的 Fiber
    // 深度为 1。
    std::size_t nesting_depth() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp
