#pragma once

#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/thread_policy.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp {
class Fiber;
enum class SuspendReason : std::uint8_t;
}

namespace go2cpp::scheduler {

using GId = std::uint64_t;
using MId = std::uint64_t;
using PId = std::uint32_t;

/** @brief Fiber 唤醒请求的处理结果。 */
enum class WakeAction : std::uint8_t {
    kRejected,
    kEnqueue,
    kPending,
};

/** @brief Fiber park 请求的处理结果。 */
enum class ParkAction : std::uint8_t {
    kRejected,
    kParked,
    kRequeued,
};

/** @brief GMP 模型中 G（Fiber）状态。 */
enum class GState : std::uint8_t {
    kNew,
    kRunnable,
    kRunning,
    kWaiting,
    kDead,
    kCancelled,
    // 普通 C++ 异常在 Fiber 边界被捕获后的失败终态。
    kFailed,

    // 可读别名：便于转译代码直接使用接近 Go 的状态名称。
    New = kNew,
    Runnable = kRunnable,
    Running = kRunning,
    Waiting = kWaiting,
    Dead = kDead,
    Cancelled = kCancelled,
    Failed = kFailed,
};

/** @brief GMP 模型中 M（工作线程）状态。 */
enum class MState : std::uint8_t {
    kIdle,
    kRunning,
    kBlocking,
    kParked,
    kStopping,
    kDead,

    Idle = kIdle,
    Running = kRunning,
    Blocking = kBlocking,
    Parked = kParked,
    Stopping = kStopping,
    Dead = kDead,
};

/** @brief GMP 模型中 P（处理器资源）状态。 */
enum class PState : std::uint8_t {
    kIdle,
    kRunning,
    kStopping,
    kDead,

    Idle = kIdle,
    Running = kRunning,
    Stopping = kStopping,
    Dead = kDead,
};

/**
 * @brief 调度器生命周期观测事件类型。
 * @details 事件只描述已经发生的状态变化，不参与调度决策；实现方可以用
 *          它接入指标、追踪、故障诊断或外部调度策略。新增事件类型时应
 *          保持已有枚举值的语义不变。
 */
enum class SchedulerEventType : std::uint8_t {
    kStarted,
    kStopping,
    kWorkerStarted,
    kWorkerStopped,
    kTaskStarted,
    kTaskSuspended,
    kTaskCompleted,
    kTaskFailed,
};

/**
 * @brief 调度器事件的只读快照。
 * @details 未参与本事件的标识保持为 0；state 用于描述任务事件发生时
 *          的 G 状态。timestamp 使用 steady_clock，不能直接转换为墙上
 *          时间，但适合计算耗时和排序。
 */
struct SchedulerEvent {
    SchedulerEventType type{SchedulerEventType::kStarted};
    GId task_id{0};
    MId machine_id{0};
    PId processor_id{0};
    GState task_state{GState::kNew};
    std::chrono::steady_clock::time_point timestamp{};
};

/**
 * @brief 调度器事件扩展点。
 *
 * 依赖：Scheduler 仅持有观察者的 shared_ptr，不依赖具体日志、指标或
 *       tracing 实现。观察者不拥有 Scheduler，也不能通过事件回调控制
 *       当前 G 的执行权。
 * 对上层提供：一个稳定的插件边界，用于记录任务、M/P 生命周期，或将
 *       事件转发给外部监控系统。回调必须短小且无阻塞；异常会被隔离。
 */
class SchedulerObserver {
public:
    virtual ~SchedulerObserver() = default;

    /**
     * @brief 接收一个已经发生的调度器事件。
     * @param event 事件只读快照；调用返回后不会继续使用其引用。
     * @note 观察者不应抛出异常；接口保留可抛出签名是为了让 Scheduler
     *       在 emit_event() 中统一捕获第三方插件异常，不能让 worker
     *       线程异常退出。
     */
    virtual void OnEvent(const SchedulerEvent& event) = 0;
};

using SchedulerObserverPtr = std::shared_ptr<SchedulerObserver>;

/**
 * @brief Scheduler 启动和运行参数。
 * @details 字段可通过 config 模块加载；线程数、P 数和栈容量会在启动期
 *          校验，动态配置不得拆开修改相互依赖的结构参数。
 */
struct SchedulerConfig {
    std::size_t processor_count = 0;
    std::size_t max_workers = 0;
    std::chrono::milliseconds idle_wait{10};
    std::size_t local_queue_limit = 256;
    // 为 0 时启动一个初始 worker。max_workers 为 0 时，在允许超额调度的
    // 情况下上限归一化为 2*P；普通可运行任务仍以 P 为上限，阻塞区可以
    // 使用额外 worker。
    std::size_t min_workers = 0;
    std::chrono::milliseconds idle_worker_timeout{250};
    std::size_t fiber_stack_size = 0;
    std::size_t task_affinity_budget = 4;
    // 调用方声明原生阻塞区时，允许 M 的数量超过 P。默认值保持 worker
    // 池弹性；若部署要求严格的一 M 对一 P 上限，可以关闭此开关。
    bool allow_worker_oversubscription = true;
    // Go sysmon 风格的阻塞监控。它只观察已声明的 BlockingRegion/Hook
    // 边界，不会从另一个线程强行切断任意 C++ 调用栈。
    bool enable_sysmon = true;
    std::chrono::milliseconds sysmon_interval{10};
    std::chrono::milliseconds long_syscall_threshold{50};
    // Linux 上可将 M 绑定到与 P 对应的 CPU。默认关闭，避免嵌入宿主已有
    // CPU 配额/容器亲和性策略；打开后超出 CPU 数量的 P 不执行绑定。
    bool pin_workers_to_cpu = false;
    // 每个 M 的 FiberBin 容量。只缓存已经完成/失败且上下文已释放的
    // 调度器内部 Fiber；设为 0 表示使用运行时默认值，过大只会增加
    // 每个 worker 的保留内存，不会改变 Fiber 语义。
    std::size_t fiber_bin_capacity = 32;
    // 性能敏感的部署可以关闭运行时累计计时；状态机和调度语义不受影响。
    // 默认开启，便于诊断和性能报告。
    bool collect_metrics = true;
    // 可选的生命周期观察者。观察者只在事件发生后被调用，不改变默认
    // GMP 调度策略；空指针表示关闭观测，适合性能敏感部署。
    SchedulerObserverPtr observer;
};

using TaskClassId = std::uint64_t;

struct TaskCancellationGate;

/** @brief 单个 G/Fiber 的调度选项。 */
struct TaskOptions {
    std::size_t stack_size = 0;
    TaskClassId task_class = 0;
    // 0 表示由 SchedulerConfig::fiber_bin_capacity 注入；直接构造并加入
    // 调度器的 Task 在未注入时使用 Fiber 运行时默认容量。
    std::size_t fiber_bin_capacity = 0;
};

/** @brief P 的只读运行快照。 */
struct ProcessorSnapshot {
    PId id{0};
    PState state{PState::kIdle};
    std::size_t queued{0};
};

/** @brief M 的只读运行快照。 */
struct MachineSnapshot {
    MId id{0};
    MState state{MState::kIdle};
    PId processor{0};
    // 软任务类别亲和性计数，仅用于观测：当只有其他类别任务可执行时，
    // worker 仍然可以窃取它们。
    TaskClassId last_task_class{0};
    std::size_t affinity_hits{0};
    std::size_t affinity_misses{0};
    // sysmon 将长时间阻塞的 M 标记为已脱离 P；M 仍在原生调用中运行，
    // 但该 P 的 attached 计数不再包含它，替代 M 可以接管调度资源。
    bool processor_detached{false};
    GId blocking_task{0};
    std::uint64_t long_syscall_count{0};
};

// 调度器热点计数器。所有时间均为 steady_clock 纳秒，采用累计值，便于
// 调用方按两次快照的差值计算单次 Fiber 运行、切换和本地队列命中成本。
/** @brief 调度器统计指标快照。 */
struct SchedulerMetrics {
    std::uint64_t task_runs{0};
    std::uint64_t task_completions{0};
    std::uint64_t fiber_resume_ns{0};
    std::uint64_t local_queue_pops{0};
    std::uint64_t steal_pops{0};
};

/**
 * 调度器中的 G（可恢复任务）对象。
 *
 * 依赖：Fiber 提供用户栈和挂起/恢复能力，Scheduler 负责队列、M/P 绑定
 * 和状态推进，Context/同步原语通过本类发布唤醒与取消请求。
 * 对上层提供：任务提交后的状态查询、取消、等待、失败传播和调度器内部
 * 的入队/执行权原子操作。职责边界是管理单个 G 的生命周期，不负责创建
 * worker 线程，也不直接操作 P 队列。
 */
/**
 * @brief Scheduler 管理的 G/Fiber 任务对象。
 * @details 保存入口函数、取消状态、生命周期和等待结果；Scheduler 负责
 *          将任务放入本地、P 或全局队列并保证同一时刻只在一个 M 上运行。
 */
class Task : public std::enable_shared_from_this<Task> {
public:
    using Function = std::function<void()>;

    explicit Task(Function function, TaskOptions options = {});
    ~Task();

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    GId id() const noexcept;
    GState state() const noexcept;
    bool queued() const noexcept;
    bool started() const noexcept;
    bool cancellation_requested() const noexcept;
    // 最近一次运行该 G 的 P。外部唤醒者只把它作为亲和性提示，真正的
    // 执行权仍由 Task 状态机串行化。
    PId last_processor_id() const noexcept {
        return m_last_processor_id.load(std::memory_order_relaxed);
    }
    TaskClassId task_class() const noexcept;
    // 普通 C++ 异常导致的失败是可观察终态，不会静默当作正常完成。
    bool failed() const noexcept;
    std::exception_ptr failure() const;
    void rethrow_failure() const;
    // 等待任务完成。返回值：true 表示任务已经进入终态；自等待或等待方
    // 已取消时返回 false。普通线程调用保持阻塞等待语义。
    bool wait() const;
    bool wait_for(std::chrono::steady_clock::duration timeout) const;
    GId Id() const noexcept { return id(); }
    GState State() const noexcept { return state(); }
    bool Queued() const noexcept { return queued(); }
    bool Started() const noexcept { return started(); }
    bool CancellationRequested() const noexcept {
        return cancellation_requested();
    }
    TaskClassId TaskClass() const noexcept { return task_class(); }
    bool Failed() const noexcept { return failed(); }
    std::exception_ptr Failure() const { return failure(); }
    void RethrowFailure() const { rethrow_failure(); }
    bool Join() const { return wait(); }
    // 调度器后端使用的入队权/执行权接口。它们保持公开是为了允许替换
    // worker 后端复用同一不变量，但不会暴露内部成员。
    bool try_mark_queued() noexcept;
    // 当前 worker 已经释放 execution claim 后的本地回队快路径。通过
    // queued 原子位先占位，再校验状态；外部取消/唤醒仍使用同一位阻止
    // 重复入队，因此不需要为每次 yield 获取 transition_mutex。
    bool try_mark_queued_from_worker() noexcept;
    void clear_queued() noexcept;
    void defer_enqueue() noexcept;
    bool consume_deferred_enqueue() noexcept;
    void request_wake() noexcept;
    bool consume_wake() noexcept;
    bool try_register() noexcept;
    // 仅供调度器唤醒快路径观察注册表强引用是否已经发布。
    bool registered() const noexcept;
    // 将 owner、取消门和首次入队的状态转换合并为一次锁操作。
    // 外部生产者的热路径必须保持这个原子边界，避免在多个状态锁之间
    // 反复切换；失败时不会发布队列节点。
    bool prepare_enqueue(const std::shared_ptr<const void>& owner,
                         const std::shared_ptr<TaskCancellationGate>& gate) noexcept;
    // 把任务绑定到唯一 Scheduler。owner 是不透明身份令牌，不会延长
    // Scheduler 对象的生命周期；返回值表示绑定是否成功。
    bool bind_owner(const std::shared_ptr<const void>& owner) noexcept;
    bool owned_by(const std::shared_ptr<const void>& owner) const noexcept;
    void bind_cancellation_gate(
        const std::shared_ptr<TaskCancellationGate>& gate) noexcept;
    bool promote_new() noexcept;
    WakeAction wake_for_scheduler() noexcept;
    // IO 轮询器的快速唤醒路径。轮询器只发布唤醒令牌，worker 在 Fiber
    // 从 park 返回后消费令牌，因此状态转换可以用原子 CAS 完成。
    WakeAction wake_for_io() noexcept;
    // 跨 M 的同步等待唤醒使用同一套 Waiting -> Runnable 原子线性化。
    // 它不访问 Fiber 栈，只发布状态和 pending token；真正 resume 仍由
    // execution_claim 串行化，因此不会让两个 M 同时进入同一个 Fiber。
    WakeAction wake_for_wait() noexcept;
    ParkAction park_for_scheduler() noexcept;
    bool try_mark_running();
    // Scheduler 使用的状态转换。mark_runnable() 只唤醒已经等待的任务；
    // 对运行中的任务记录 pending 唤醒令牌，并由当前 G 的 yield/park 路径
    // 完成后续转换。
    bool mark_runnable();
    bool mark_yielded();
    // 当前 G 明确执行 yield 时使用无锁 CAS 快路径。唤醒/取消仍通过
    // 完整状态机处理；下一次成功出队会清理旧的 wake permit。
    bool mark_yielded_fast() noexcept;
    bool mark_waiting();
    bool cancel();
    // 仅取消尚未入队的可运行任务。用于内部重新入队失败后的收敛，避免
    // 把已经被并发接受的入队误判为接纳失败。
    bool cancel_if_runnable_unqueued() noexcept;
    bool Cancel() { return cancel(); }
    // 后端执行入口；run() 内部也会串行化直接调用。
    void run();

private:
    friend class Scheduler;
    Function release_callable() noexcept;
    bool request_cancel(bool notify = true) noexcept;
    bool terminal() const noexcept;
    void notify_terminal() noexcept;

    GId m_id;
    Function m_function;
    TaskOptions m_options;
    std::unique_ptr<go2cpp::Fiber> m_fiber;
    std::atomic<GState> m_state;
    std::atomic<bool> m_queued{false};
    std::atomic<bool> m_deferred_enqueue{false};
    std::atomic<bool> m_wake_pending{false};
    std::atomic<bool> m_registered{false};
    std::atomic<bool> m_execution_claim{false};
    std::atomic<bool> m_run_claim{false};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_cancel_requested{false};
    MId m_last_machine_id{0};
    std::atomic<PId> m_last_processor_id{0};
    bool m_binding_published{false};
    mutable std::mutex m_failure_mutex;
    std::exception_ptr m_failure;
    // 无分配救援队列链接。只在所属 Scheduler 的接纳互斥锁下修改，且
    // 不会形成环。
    std::shared_ptr<Task> m_emergency_next;
    std::shared_ptr<const void> m_owner_anchor;
    std::weak_ptr<TaskCancellationGate> m_cancellation_gate;
    std::shared_ptr<Task> m_completion_notification_next;
    std::atomic<bool> m_completion_notification_queued{false};
    std::atomic<bool> m_completion_notified{false};
    mutable std::mutex m_transition_mutex;
    mutable std::mutex m_completion_mutex;
    mutable core::ParkingCondition m_completion_condition;
};

/**
 * GMP 风格的 G/M/P 调度器。
 *
 * 依赖：Task/Fiber 表示 G，内部 worker 线程表示 M，ProcessorSnapshot 表示
 * P；ParkingCondition 提供无忙等等待，ThreadPolicy 提供线程亲和性策略。
 * 对上层提供任务提交、yield/park/wake、取消、统计和有界 shutdown 服务。
 * Scheduler 拥有 worker 生命周期和队列，但不拥有调用方保存的 Task 句柄。
 * 所有阻塞原生调用必须通过 BlockingRegion 或 Hook 边界声明，调度器不能
 * 从其他线程强制切断任意 C++ 调用栈。
 */
/**
 * @brief Go2Cpp 的 GMP 调度器。
 * @details 维护 G、M、P 的绑定和队列，提供 Fiber/线程混合的 park/wake、
 *          work stealing、阻塞区段和 sysmon 观测接口。
 */
class Scheduler {
public:
    // 标记可能阻塞当前 M 的原生调用。区域会发布
    // M::Blocking; sysmon 超过阈值后会把该 M 与 P 的计数解绑并驱动替代
    // M。它不从别的线程强行破坏调用栈，不能跨 Fiber yield/park，
    // 也不能跨迁移当前 G 的调用。
    class BlockingRegion final {
    public:
        explicit BlockingRegion(Scheduler* scheduler = nullptr) noexcept;
        ~BlockingRegion() noexcept;

        BlockingRegion(const BlockingRegion&) = delete;
        BlockingRegion& operator=(const BlockingRegion&) = delete;
        // 区域绑定到进入它的 M。移动可能让析构发生在其他 Fiber/线程，
        // 使原 M 永久保持 Blocking，因此该对象刻意禁止移动。
        BlockingRegion(BlockingRegion&&) = delete;
        BlockingRegion& operator=(BlockingRegion&&) = delete;

        bool active() const noexcept { return m_active; }
        bool Active() const noexcept { return active(); }

    private:
        Scheduler* m_scheduler{nullptr};
        bool m_active{false};
        MId m_machine_id{0};
    };

    explicit Scheduler(SchedulerConfig config = {});
    explicit Scheduler(std::size_t processor_count);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start();
    void shutdown();
    // 请求停止并在给定时间内等待所有 Fiber/worker 完成。返回 true 表示
    // 已完成并已回收 worker；返回 false 表示超时或从 worker 自身调用。
    // 超时不会释放仍在运行的 Fiber 栈，调用者可稍后再次调用本接口或
    // 使用无界 shutdown() 完成最终回收。
    bool shutdown_for(std::chrono::steady_clock::duration timeout);
    bool is_running() const noexcept;

    void Start() { start(); }
    void Shutdown() { shutdown(); }
    bool ShutdownFor(std::chrono::steady_clock::duration timeout) {
        return shutdown_for(timeout);
    }
    bool IsRunning() const noexcept { return is_running(); }

    /**
     * @brief 安装或替换运行期调度器观察者。
     * @param observer 新观察者；传入空指针表示关闭事件通知。
     * @note 替换只影响后续事件，已经发出的事件不会回放；回调在调度器
     *       内部锁外执行，观察者可以安全地把数据转发到独立队列。
     */
    void set_observer(SchedulerObserverPtr observer) noexcept;
    /** @brief 返回当前观察者快照；空指针表示未安装观察者。 */
    SchedulerObserverPtr observer() const noexcept;
    void SetObserver(SchedulerObserverPtr observer) noexcept {
        set_observer(std::move(observer));
    }
    SchedulerObserverPtr Observer() const noexcept { return observer(); }

    std::shared_ptr<Task> spawn(Task::Function function);
    std::shared_ptr<Task> spawn(Task::Function function, TaskOptions options);
    std::shared_ptr<Task> go(Task::Function function) { return spawn(std::move(function)); }
    bool enqueue(const std::shared_ptr<Task>& task);
    // 新手入口：把已经构造好的任务加入当前调度器。任务不会被复制，
    // 调度器取得一个共享句柄；调用者可以继续使用原来的句柄等待或取消。
    std::shared_ptr<Task> add(const std::shared_ptr<Task>& task) {
        return enqueue(task) ? task : std::shared_ptr<Task>{};
    }
    // 直接加入一个回调，等价于 spawn，但名字更接近 Go 的使用方式。
    std::shared_ptr<Task> add(Task::Function function) {
        return spawn(std::move(function));
    }
    // 允许 go2cpp::fiber 等轻量包装器通过 task() 接口接入，而不让
    // scheduler.hpp 依赖上层包装器的定义。
    template <typename TaskLike,
              typename = std::enable_if_t<std::is_same_v<
                  decltype(std::declval<const TaskLike&>().task()),
                  std::shared_ptr<Task>>>>
    std::shared_ptr<Task> add(const TaskLike& task_like) {
        return add(task_like.task());
    }
    bool yield(const std::shared_ptr<Task>& task);
    bool park(const std::shared_ptr<Task>& task);
    // 同步原语等待的轻量挂起路径。Task 已由 WaitNode 注册，使用与 IO
    // 相同的原子 Waiting -> Runnable 交接，避免每次 Mutex handoff 获取
    // scheduler admission 锁；shutdown 通过二次检查和 wake_for_wait 收敛。
    bool park_wait(const std::shared_ptr<Task>& task);
    // IOManager 使用此入口记录 Fiber 正在等待 readiness；它与普通
    // park 共用同一 Task 状态和 wake 竞态，但诊断帧会保留 Io 原因。
    bool park_io(const std::shared_ptr<Task>& task);
    bool yield_current();
    bool park_current();
    // 唤醒等待中的 G。返回 true 表示已经立即入队；返回 false 仍可能表示
    // 已接受一个 pending 唤醒（任务正在运行），该令牌会在下一次 park 时
    // 被消费。
    bool wake(const std::shared_ptr<Task>& task);
    // 已经注册到调度器的挂起 G 使用此入口，跳过重复的所有权/注册表
    // 校验，只把一个新的队列节点发布到 incoming 条带。等待原语使用它
    // 以降低普通线程唤醒 Fiber 时的 admission 开销。
    bool wake_registered(const std::shared_ptr<Task>& task);
    bool wake_io(const std::shared_ptr<Task>& task);
    // 等待节点选出终态后使用的通知交接。它跨越 shutdown/admission 竞态
    // 保留已经启动但仍挂起栈上的 G，避免提前标记为终态。
    bool wake_or_cancel(const std::shared_ptr<Task>& task);
    bool cancel(const std::shared_ptr<Task>& task);

    std::shared_ptr<Task> Spawn(Task::Function function) {
        return spawn(std::move(function));
    }
    std::shared_ptr<Task> Go(Task::Function function) {
        return spawn(std::move(function));
    }
    bool Enqueue(const std::shared_ptr<Task>& task) { return enqueue(task); }
    std::shared_ptr<Task> Add(const std::shared_ptr<Task>& task) {
        return add(task);
    }
    std::shared_ptr<Task> Add(Task::Function function) {
        return add(std::move(function));
    }
    bool Yield(const std::shared_ptr<Task>& task) { return yield(task); }
    bool Park(const std::shared_ptr<Task>& task) { return park(task); }
    bool Wake(const std::shared_ptr<Task>& task) { return wake(task); }
    bool WakeOrCancel(const std::shared_ptr<Task>& task) {
        return wake_or_cancel(task);
    }
    bool Cancel(const std::shared_ptr<Task>& task) { return cancel(task); }

    std::size_t processor_count() const noexcept;
    std::size_t worker_count() const noexcept;
    std::size_t runnable_count() const noexcept;
    std::size_t global_runnable_count() const noexcept;
    std::size_t ProcessorCount() const noexcept { return processor_count(); }
    std::size_t WorkerCount() const noexcept { return worker_count(); }
    std::size_t RunnableCount() const noexcept { return runnable_count(); }
    std::size_t GlobalRunnableCount() const noexcept {
        return global_runnable_count();
    }
    std::vector<ProcessorSnapshot> processors() const;
    std::vector<MachineSnapshot> machines() const;
    // 返回 sysmon 线程是否正在运行，便于部署自检和测试。
    bool sysmon_running() const noexcept;
    // 每次监控周期都会递增，即使调度器互斥量正被高负载路径占用。
    // 该计数只用于活性观测，不参与调度决策。
    std::uint64_t sysmon_pass_count() const noexcept;
    SchedulerMetrics metrics() const noexcept;
    bool SysmonRunning() const noexcept { return sysmon_running(); }
    std::uint64_t SysmonPassCount() const noexcept {
        return sysmon_pass_count();
    }
    SchedulerMetrics Metrics() const noexcept { return metrics(); }

    // 返回当前线程正在执行的任务（若有）。这些值只用于观测，不参与所有权。
    static std::shared_ptr<Task> current_task() noexcept;
    static Scheduler* current_scheduler() noexcept;
    static MId current_machine_id() noexcept;
    static PId current_processor_id() noexcept;
    // 供拦截器和嵌入方在受管 G 中调用原生阻塞 API 时显式标记。一般代码
    // 优先使用 RAII 形式的 BlockingRegion。
    static bool enter_blocking() noexcept;
    static void leave_blocking() noexcept;
    static MId CurrentMachineId() noexcept { return current_machine_id(); }
    static PId CurrentProcessorId() noexcept { return current_processor_id(); }

private:
    // 仅供拥有该 G 执行权的 worker 在 resume 返回后调用。
    bool requeue_from_worker(const std::shared_ptr<Task>& task);
    // 外部 M 唤醒 G 时优先投递到 G 最近运行的 P，失败时回退到普通
    // admission 队列，避免混合锁交接每次争用全局 incoming stripe。
    bool requeue_to_processor(const std::shared_ptr<Task>& task,
                              PId processor_id);
    static void leave_blocking_for(Scheduler* scheduler,
                                   MId machine_id) noexcept;
    bool park_with_reason(const std::shared_ptr<Task>& task,
                          go2cpp::SuspendReason reason);

    // 不透明共享槽在 Scheduler 增删 machine 记录时保持动态 worker 的地址稳定。
    void worker_loop(std::shared_ptr<void> machine);

    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::scheduler

// 短别名让转译代码更紧凑；需要明确模块边界时仍可使用嵌套命名空间。
namespace go2cpp {
using Scheduler = scheduler::Scheduler;
using SchedulerConfig = scheduler::SchedulerConfig;
using Task = scheduler::Task;
using GState = scheduler::GState;
using MState = scheduler::MState;
using PState = scheduler::PState;
using GId = scheduler::GId;
using MId = scheduler::MId;
using PId = scheduler::PId;
using TaskClassId = scheduler::TaskClassId;
using SchedulerEventType = scheduler::SchedulerEventType;
using SchedulerEvent = scheduler::SchedulerEvent;
using SchedulerObserver = scheduler::SchedulerObserver;
using SchedulerObserverPtr = scheduler::SchedulerObserverPtr;
using TaskOptions = scheduler::TaskOptions;
using BlockingRegion = scheduler::Scheduler::BlockingRegion;
using Goroutine = scheduler::Task;
using Machine = scheduler::MachineSnapshot;
using Processor = scheduler::ProcessorSnapshot;
}  // namespace go2cpp
