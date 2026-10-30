/**
 * @file scheduler/scheduler.hpp
 * @brief Scheduler 的完整公开接口（本文件是真身）。
 *
 * ## 模块定位
 *
 * 调度器是整个并发运行时的心脏：它把用户提交的 `Task` 包装成 **G**，交给
 * **P**（逻辑处理器）排队，由 **M**（OS 线程）执行。GMP 三层分离的意义在于：
 * G 可以在不同 M 之间迁移，P 的数量决定真正的并行度，M 可以因系统调用膨胀
 * 或收缩。
 *
 * ## 本文件定义什么
 *
 * 全部是**接口与状态枚举**，实现都在 `src/concurrency/scheduler.cpp`：
 *
 * - `GState` / `MState` / `PState`：三层的状态机枚举。这三个枚举是排查调度
 *   问题的第一手线索 —— 打印快照时应总是把它们带上；
 * - `WakeAction` / `ParkAction`：park/wake 时要求调度器执行的动作
 *   （例如 park 期间是否允许被抢占、唤醒后是否立刻重排）；
 * - `SchedulerEventType` / `SchedulerEvent` / `SchedulerObserver`：
 *   可观测性钩子，用于 debug 模块与压测时的事件追踪；
 * - `SchedulerConfig`：P 数量、栈大小、队列上限、sysmon 周期等所有可调参数；
 * - `Scheduler`：对外门面（spawn / yield / park / shutdown 等）。
 *
 * ## 阅读建议
 *
 * 先看三个状态枚举建立"一个 G 现在在哪、一个 M 在干什么"的心智模型，
 * 再看 `Scheduler` 的公开方法，最后才进 .cpp 看队列与窃取细节。
 */
#pragma once

#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/core/timer.hpp"
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

/**
 * @brief 按 gid 查询到的 G 终态分类（Scheduler::TaskTerminal() 的返回值）。
 * @details 只为「单点查一个 G 是否仍是活的 / 以什么终态结束」提供稳定答案：
 *          - kUnknown：该 gid 不在当前调度器注册表里。**不能**当作「已终止」
 *            ——既可能是从未存在，也可能是终态条目已被回收；
 *          - kCompleted / kFailed / kCancelled：分别对应正常的 kDead、
 *            异常失败、取消三种终态。
 *          与 GState 分开，是为了避免调用方把「查不到」误读成某个状态。
 */
enum class TaskTerminalState : std::uint8_t {
    kUnknown,
    kCompleted,
    kFailed,
    kCancelled,
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
    /** @brief 任务在运行前或等待期间被取消。 */
    kTaskCancelled,
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
 * @brief 唤醒/恢复时的投递策略（可运行期切换，用于 A/B 对比与调参）。
 *
 * ## 背景
 *
 * G 在哪个 M 上恢复决定了 TLS/栈/用户数据的缓存温度。Go 的 `goready`
 * 走「唤醒者自己 P 的 runnext → 本地运行队列 → 全局队列」，并在有空闲
 * spinning M 时直接 handoff；Bento 的 P 没有唯一 owner，因此把「同 M」
 * 表达为「最近 M 所在 P 的快速槽」。
 *
 * ## 策略
 *
 * - `kLastMachine`（① 纯亲和）：优先把 G 投回最近运行的 P 的快速槽，
 *   **不**做轮询分散；无法亲和时回退目标 P 队列；
 * - `kRunNext`（② Go 式，默认）：worker 本地唤醒/yield 直接进本 P 的
 *   runnext；外部唤醒投到最近 M 所在 P 的 runnext；仍失败才走本地队列
 *   与全局队列；
 * - `kRoundRobin`（③ 对照）：按 P 编号轮转投递到队列尾，用于证明亲和
 *   策略确实带来收益。
 *
 * 三种策略都不改变正确性，只改变「投到哪个队列/槽」。
 */
enum class WakePolicy : std::uint8_t {
    kLastMachine = 0,
    kRunNext = 1,
    kRoundRobin = 2,
};

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
    // 常驻「基础 M」数量。为 0 时默认等于 processor_count（Go 风格：
    // P 数量的常驻 M；仍受 max_workers 封顶）。补充 M 只用于 BlockingRegion
    // 期间接管被原生调用占住的 P，不参与常驻计数。
    std::size_t min_workers = 0;
    // 补充 M 的阻塞结束检查周期。基础 M 不再因为空闲而退出，因此这个值
    // 只决定补充 M 在等待唤醒时多久复查一次 blocking_workers 是否归零；
    // leave_blocking() 会主动 notify，正常情况下无需等满一个周期。
    std::chrono::milliseconds idle_worker_timeout{250};
    // Fiber 栈的初始提交量（字节）。默认 2048（Go 的 2KB 初始栈）；
    // Linux 页粒度是 4KB，所以归一化规则是
    //   initial_usable = max(page_align_up(requested), 1 * page)
    // 请求 2048 时实际初始可用 4096 字节（1 页）。其余部分属于
    // fiber_stack_reserve_size 预留区，初始为 PROT_NONE，按需增长。
    std::size_t fiber_stack_size = 2048;
    // 每个 Fiber 预留的虚拟地址空间（字节），默认 8MiB。初始只提交
    // fiber_stack_size 对应的一页，SIGSEGV handler 以 64KiB 为粒度向下
    // mprotect，上限是这块预留区。reserve 不大于初始提交量时退化为固定栈。
    std::size_t fiber_stack_reserve_size = 8U * 1024U * 1024U;
    // 是否允许 Fiber 栈按需增长。false、reserve 不足、非 Linux/x86_64、
    // 非原生上下文后端，或环境变量 GO2CPP_GROWABLE_STACK=0 时退化为固定栈；
    // 退化路径不安装 SIGSEGV handler，也不使用 sigaltstack。
    bool fiber_stack_growable = true;
    std::size_t task_affinity_budget = 4;
    // 调用方声明原生阻塞区时，允许 M 的数量超过 P。默认值保持 worker
    // 池弹性；若部署要求严格的一 M 对一 P 上限，可以关闭此开关。
    bool allow_worker_oversubscription = true;
    // 是否允许「基础 M」在空闲超过 idle_worker_timeout 后退出（历史行为）。
    // 默认 false：基础 M 常驻到 shutdown，只有补充 M 会在 blocking_workers
    // 归零后立即退休，从而消除「空闲判定与状态切换之间新 G 入队」的窗口。
    // 代价是空载时保留 min_workers（默认 P）个 OS 线程。
    bool allow_idle_shrink = false;
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
    std::size_t fiber_bin_capacity = 128;
    // 性能敏感的部署可以关闭运行时累计计时；状态机和调度语义不受影响。
    // 默认开启，便于诊断和性能报告。
    bool collect_metrics = true;
    // 可选的生命周期观察者。观察者只在事件发生后被调用，不改变默认
    // GMP 调度策略；空指针表示关闭观测，适合性能敏感部署。
    SchedulerObserverPtr observer;
    // 常规 worker 循环回收终态 G 的节拍：每完成这么多个终态 G，触发一次
    // 注册表分片轮转回收。单次只压缩一个条带（共 16 个），约 16 个节拍
    // 覆盖全表；回收只持有条带锁，不进入全局 admission 锁。
    // 0 表示关闭周期回收（仅 shutdown/cancel 收尾路径回收）。默认 256。
    std::size_t terminal_collect_interval = 256;
    // 唤醒投递策略（见 WakePolicy）。默认 Go 式 runnext；运行期可用
    // Scheduler::set_wake_policy() 切换做 A/B 对比。
    WakePolicy wake_policy = WakePolicy::kRunNext;
};

using TaskClassId = std::uint64_t;

struct TaskCancellationGate;

/** @brief 单个 G/Fiber 的调度选项。 */
struct TaskOptions {
    std::size_t stack_size = 0;
    TaskClassId task_class = 0;
    // 0 表示由 SchedulerConfig::fiber_bin_capacity 注入；直接构造并加入
    // 调度器的 Task 在未注入时使用 Fiber 运行时默认容量。
    std::size_t fiber_bin_capacity = 128;
    // 新增字段放在末尾，保持既有 TaskOptions{a, b} 聚合初始化语义不变。
    // 0 表示使用 SchedulerConfig::fiber_stack_reserve_size（再回落到
    // Fiber::DefaultStackReserveSize()）。
    std::size_t stack_reserve_size = 0;
    // 是否允许按需增长；SchedulerConfig::fiber_stack_growable=false 时
    // spawn() 会强制关闭。
    bool stack_growable = true;
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
    // 该 M 是否为「补充 M」：为阻塞区补位的动态 worker。补充 M 在
    // blocking_workers 归零后立即退休；基础 M 恒为 false 且常驻到 shutdown。
    bool replacement{false};
    // ---- M 亲和 / runnext 观测（每个 M 独立计数，便于定位负载倾斜）----
    std::size_t runnext_hits{0};      ///< 本 M 从 runnext 取到 G 的次数
    std::size_t inbox_hits{0};        ///< 本 M 从定向槽取到 G 的次数
    std::size_t same_machine_resumes{0};  ///< 在本 M 上续跑的 G 次数
    std::size_t cross_machine_resumes{0}; ///< 从别的 M 迁移来的 G 次数
    std::size_t locked_redirects{0};  ///< 本 M 重定向回绑定 M 的次数
};

// 调度器热点计数器。所有时间均为 steady_clock 纳秒，采用累计值，便于
// 调用方按两次快照的差值计算单次 Fiber 运行、切换和本地队列命中成本。
/** @brief 调度器统计指标快照。 */

/**
 * @brief 单个 G（Task）的只读快照（第 19 轮，任务1 补齐「枚举全部 G」）。
 * @note 全部字段都来自 `Task` 的**公开只读**方法，不访问任何私有状态。
 *       不包含「当前阻塞在哪个锁/哪个 channel」这类信息 —— 现有实现没有记录，
 *       因此**不虚构**（这也是设计文档里声明的观测边界之一）。
 */
struct TaskSnapshot {
    GId id{0};                             ///< G 的编号。
    GState state{GState::kNew};            ///< 状态机当前状态。
    bool queued{false};                    ///< 是否在某个可运行队列里。
    bool started{false};                   ///< 是否已经开始执行过。
    PId last_processor_id{0};              ///< 最近一次运行所在的 P。
    TaskClassId task_class{0};             ///< 任务类别（用于亲和性）。
    bool failed{false};                    ///< 是否以失败终态结束。
    bool cancellation_requested{false};    ///< 是否已请求取消。
};

/**
 * @brief 调度器的累计计数器（`Scheduler::metrics()` 返回的快照）。
 *
 * ## 定位
 *
 * 供 `debug::SchedulerSnapshot` 与压测时观测调度行为；**不是精确的性能指标**
 * （只增不减、没有时间窗），适合看趋势与比例，不适合算 QPS。
 *
 * ## 字段
 *
 * - `task_runs` / `task_completions`：G 的执行次数与完成次数。两者差值反映
 *   「被挂起后重跑」的量（越多说明 park/wake 越频繁）；
 * - `fiber_resume_ns`：resume 的累计耗时（含 park/wake 往返）；
 * - `local_queue_pops` / `steal_pops`：本地队列取 vs 跨 P 窃取的比例。
 *   **窃取占比高说明负载不均衡** —— 但要记住 §1.9 的教训：把窃取移出全局锁
 *   反而 -34%，因为本地队列边界不能动。
 */
struct SchedulerMetrics {
    /// @brief G 被执行的累计次数（含挂起后重跑）。
    std::uint64_t task_runs{0};
    /// @brief G 进入终态的累计次数；与 task_runs 的差值反映 park/wake 频率。
    std::uint64_t task_completions{0};
    /// @brief resume 的累计耗时（纳秒，含 park/wake 往返）。
    std::uint64_t fiber_resume_ns{0};
    /// @brief 从本地 P 队列取到 G 的次数。
    std::uint64_t local_queue_pops{0};
    /// @brief 从别的 P 窃取到 G 的次数；占比高说明负载不均衡。
    std::uint64_t steal_pops{0};

    // ---- M 亲和 / runnext 观测（用于计算命中率）----
    /// @brief 投递到 P 快速槽（runnext）的累计次数。
    std::uint64_t runnext_pushes{0};
    /// @brief 从快速槽直接取到 G 的累计次数（命中）。
    std::uint64_t runnext_pops{0};
    /// @brief 快速槽被占满时被挤到本地队列的累计次数。
    std::uint64_t runnext_displaced{0};
    /// @brief 投递到 M 定向槽（inbox，LockToCurrentM/严格同 M）的次数。
    std::uint64_t inbox_pushes{0};
    /// @brief 从 M 定向槽取到 G 的次数。
    std::uint64_t inbox_pops{0};
    /// @brief 唤醒时命中「最近 M」投递提示的次数。
    std::uint64_t affinity_wakes{0};
    /// @brief 别的 M 误取到 LockToCurrentM 的 G、重定向回绑定 M 的次数。
    std::uint64_t pinned_redirects{0};
    /// @brief G 在同一个 M 上恢复的次数。
    std::uint64_t same_machine_resumes{0};
    /// @brief G 跨 M 迁移恢复的次数。
    std::uint64_t cross_machine_resumes{0};
    /// @brief 从别的 P 的快速槽窃取成功的次数。
    std::uint64_t steal_runnext{0};
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

namespace detail {

/**
 * @brief 调度器热路径的轻量互斥锁（TTAS 自旋 + yield 退避）。
 *
 * `std::mutex`（glibc pthread_mutex）即使无争用也要走完整状态机与 futex
 * 快路径：callgrind 基线里一次 lock+unlock 约 145 条指令，park/wake
 * 一次往返要付 6 次，占 20% 以上的指令数。Task 的状态转换、P 的本地
 * 队列临界区都只有几条到几十条指令，且绝大多数时候没有竞争者，因此
 * 用「一次 exchange 抢占 + 竞争时先 pause 自旋、再让出时间片」替代。
 *
 * 语义与 std::mutex 一致：lock()/unlock()/try_lock()，不可重入，
 * 不提供递归与超时；与 lock_guard/unique_lock 兼容。
 */
class HotSpinLock {
public:
    HotSpinLock() noexcept = default;
    ~HotSpinLock() = default;
    HotSpinLock(const HotSpinLock&) = delete;
    HotSpinLock& operator=(const HotSpinLock&) = delete;

    void lock() noexcept {
        if (!m_flag.exchange(true, std::memory_order_acquire)) {
            return;
        }
        for (;;) {
            // 竞争者存在：先只读自旋（缓存行在持有者手里，读比 RMW 便宜），
            // 超过预算后让出时间片，避免持有者被抢占时形成活锁。
            std::uint32_t spins = 0;
            while (m_flag.load(std::memory_order_relaxed)) {
                if (++spins < kSpinBudget) {
                    CpuRelax();
                } else {
                    std::this_thread::yield();
                }
            }
            if (!m_flag.exchange(true, std::memory_order_acquire)) {
                return;
            }
        }
    }

    void unlock() noexcept { m_flag.store(false, std::memory_order_release); }

    bool try_lock() noexcept {
        return !m_flag.exchange(true, std::memory_order_acquire);
    }

private:
    static constexpr std::uint32_t kSpinBudget = 128;

    static void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#endif
    }

    std::atomic<bool> m_flag{false};
};

}  // namespace detail

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
    // 最近一次运行所在的 M（0=尚未在受管 M 上运行）。唤醒方把它作为
    // M 亲和提示：优先投回该 M 所在 P 的快速槽（runnext）。
    MId last_machine_id() const noexcept {
        return m_affinity_machine.load(std::memory_order_acquire);
    }
    // LockToCurrentM 绑定的 M（0=未绑定）。绑定后该 G 只在绑定 M 上恢复。
    MId locked_machine_id() const noexcept {
        return m_locked_machine.load(std::memory_order_acquire);
    }
    // 把当前 G 绑定到它正在运行的 M（等价 Go 的 runtime.LockOSThread）。
    // 只允许在该 G 自己的执行流里调用；成功返回 true。
    bool lock_to_current_m() noexcept;
    // 解除 M 绑定；绑定 M 死亡时调度器也会自动调用。
    bool unlock_from_m() noexcept;
    TaskClassId task_class() const noexcept;
    /// @brief 当前 G 持有的 `sync::Mutex` 数量（仅持锁 park 检测开启时维护）。
    std::uint32_t held_mutex_count() const noexcept {
        return m_held_mutexes.load(std::memory_order_relaxed);
    }
    /// @brief 记录一次成功加锁（持锁 park 检测）。
    void note_mutex_locked() noexcept {
        m_held_mutexes.fetch_add(1, std::memory_order_relaxed);
    }
    /// @brief 记录一次解锁；计数已为 0 时保持不变，避免下溢。
    void note_mutex_unlocked() noexcept {
        std::uint32_t current = m_held_mutexes.load(std::memory_order_relaxed);
        while (current != 0U) {
            if (m_held_mutexes.compare_exchange_weak(
                    current, current - 1U, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return;
            }
        }
    }
    /// @brief 当前 G 处于 `sync::SpinMutex` 临界区的嵌套深度。
    /// @details 深度 > 0 表示临界区内**禁止** park；park 入口据此 abort
    ///          （debug）或降级为普通 Mutex 语义并计数（release）。
    std::uint32_t no_park_depth() const noexcept {
        return m_no_park_depth.load(std::memory_order_relaxed);
    }
    /// @brief 进入一段「禁止 park」的临界区（`SpinMutex::Lock` 成功后调用）。
    void enter_no_park_section() noexcept {
        m_no_park_depth.fetch_add(1, std::memory_order_relaxed);
    }
    /// @brief 离开一段「禁止 park」的临界区；计数已为 0 时保持不变。
    /// @note 跨 G 解锁时由 `SpinMutex` 记录的 owner 调用，因此不会把计数
    ///       减到别的 G 头上。
    void leave_no_park_section() noexcept {
        std::uint32_t current = m_no_park_depth.load(std::memory_order_relaxed);
        while (current != 0U) {
            if (m_no_park_depth.compare_exchange_weak(
                    current, current - 1U, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return;
            }
        }
    }
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
    // 已经完成状态转换后的本地入队快路径。通过 queued 原子位先占位，
    // 再校验状态；外部取消/唤醒仍使用同一位阻止重复入队，因此不需要
    // 为每次 yield 或 IO 批量唤醒获取 transition_mutex。
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
    // live_tasks 指向所属 Scheduler 的「未终态 G 数」原子计数：首次入队
    // 成功时 +1，进入终态时 -1。nullptr 表示调用方不是真实调度器
    // （状态机测试），此时不参与关停计数。
    bool prepare_enqueue(const std::shared_ptr<const void>& owner,
                         const std::shared_ptr<TaskCancellationGate>& gate,
                         std::atomic<std::size_t>* live_tasks = nullptr) noexcept;
    // 把任务绑定到唯一 Scheduler。owner 是不透明身份令牌，不会延长
    // Scheduler 对象的生命周期；返回值表示绑定是否成功。
    bool bind_owner(const std::shared_ptr<const void>& owner) noexcept;
    bool owned_by(const std::shared_ptr<const void>& owner) const noexcept;
    void bind_cancellation_gate(
        const std::shared_ptr<TaskCancellationGate>& gate) noexcept;
    bool promote_new() noexcept;
    WakeAction wake_for_scheduler() noexcept;
    // IO 轮询器的唤醒路径。轮询器只发布状态或 pending 唤醒令牌，不访问
    // Fiber 栈；状态转换与 park 共用转换锁，避免令牌被并发挂起覆盖。
    WakeAction wake_for_io() noexcept;
    // 跨 M 的同步等待唤醒使用与 park 相同的状态转换锁，串行完成
    // Waiting -> Runnable 或 Running -> pending token。它不访问 Fiber 栈，
    // 真正 resume 仍由 execution_claim 串行化，因此不会让两个 M 同时进入
    // 同一个 Fiber。
    WakeAction wake_for_wait() noexcept;
    // 与 wake_for_wait() 相同，但在 Waiting -> Runnable 且没有执行者的
    // 分支里顺带置位 m_queued（队列节点预留），把「唤醒状态转换」和
    // 「入队权预留」合并进同一个临界区。调用方必须随后调用
    // requeue_from_worker_reserved()；任何回退到普通入队/外部投递的路径
    // 都必须先 clear_queued()。返回 kPending/kRejected 时不做预留。
    WakeAction wake_for_wait_reserve() noexcept;
    ParkAction park_for_scheduler() noexcept;
    bool try_mark_running();
    // P0-3：队列节点出队后的无锁执行权认领。节点已由 pop 者独占，
    // 因此「抢 execution_claim → 清 queued → Runnable->Running」不需要
    // 转换锁；抢不到 claim 说明节点是并发残留，返回 false 交回调用方
    // 走 recover_unclaimed() 收尾。
    bool claim_from_queue() noexcept;
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
    // park/yield 未能真正交回执行流时（Fiber 拒绝挂起，或挂起提交后被
    // 唤醒但执行权仍在本线程），把状态回滚成 kRunning。否则 G 会以
    // "无人的等待者/Runnable" 留在 registry，既不会被唤醒，也会让后续
    // park 被状态机一律拒绝而同陷空转。
    bool resume_after_failed_park() noexcept;
    // 执行边界自愈：run() 返回后若 G 仍停在 kRunning 且已无执行者，
    // worker 用它把 G 重新标记为可运行并交回调度器，避免永久悬挂。
    // 返回 true 表示确实修复了一个孤儿 Running。
    bool recover_orphan_running() noexcept;
    bool Cancel() { return cancel(); }
    // 后端执行入口；run() 内部也会串行化直接调用。
    void run();

private:
    friend class Scheduler;
    // ---- 打包状态字（低 8 位 GState，bit 8 是 wake 令牌） -------------
    static constexpr std::uint32_t kStateMask = 0xFFU;
    static constexpr std::uint32_t kWakePendingBit = 0x100U;
    static GState StateOfWord(std::uint32_t word) noexcept {
        return static_cast<GState>(word & kStateMask);
    }
    GState StateValue(std::memory_order order = std::memory_order_acquire)
        const noexcept {
        return StateOfWord(m_state_word.load(order));
    }
    bool WakePendingValue(std::memory_order order = std::memory_order_acquire)
        const noexcept {
        return (m_state_word.load(order) & kWakePendingBit) != 0U;
    }
    /// @brief 保留令牌位地改写状态（CAS 循环）。
    void StoreStateValue(GState state, std::memory_order order) noexcept;
    /// @brief 只改令牌位（CAS 循环），保留状态。
    void StoreWakePending(bool pending, std::memory_order order) noexcept;
    /// @brief 交换令牌位，返回旧值（等价 std::atomic<bool>::exchange）。
    bool ExchangeWakePending(bool pending, std::memory_order order) noexcept;

    Function release_callable() noexcept;
    bool request_cancel(bool notify = true) noexcept;
    WakeAction wake_for_wait_impl(bool reserve_queue) noexcept;
    bool terminal() const noexcept;
    void notify_terminal() noexcept;
    // 存活 G 计数钩子。二者都要求调用方持有 m_transition_mutex：
    // 计数增减与状态转移共用同一线性化点，draining 才能用 O(1) 计数
    // 代替「每完成一个 G 扫描整张注册表」的 O(n) 判定。
    void bind_live_counter_locked(std::atomic<std::size_t>* counter) noexcept;
    void release_live_counter_locked() noexcept;

    GId m_id;
    Function m_function;
    TaskOptions m_options;
    std::unique_ptr<go2cpp::Fiber> m_fiber;
    // run() 热路径免锁读取的 Fiber 指针。写侧始终在 m_transition_mutex
    // 内与 m_fiber 同步更新（release），读侧用 acquire；执行权由
    // m_run_claim 串行化，同一时刻最多一个执行者读写它。
    std::atomic<go2cpp::Fiber*> m_fiber_ptr{nullptr};
    // 当前 G 持有的 sync::Mutex 数量。只在开启"持锁 park 检测"时维护。
    std::atomic<std::uint32_t> m_held_mutexes{0};
    // 当前 G 处于 sync::SpinMutex 临界区的嵌套深度。>0 时 park 入口
    // 按 debug/release 两种策略处理（abort / 降级计数）。
    std::atomic<std::uint32_t> m_no_park_depth{0};
    // 打包状态字：低 8 位是 GState，bit 8 是 wake 令牌。
    // 状态与令牌必须由同一次 CAS 线性化；拆成两个原子会让 park 与
    // wake 之间留下「令牌已发布、挂起已提交、却没人消费」的丢唤醒窗口。
    std::atomic<std::uint32_t> m_state_word;
    std::atomic<bool> m_queued{false};
    std::atomic<bool> m_deferred_enqueue{false};
    std::atomic<bool> m_registered{false};
    // 上述计数的弱引用。指针由 Scheduler::Impl 持有、生存期内地址稳定；
    // 只在 m_live_counted 为 true（已计入且尚未终态）期间访问，终态时
    // 在同一临界区内清空。m_live_counted 由 m_transition_mutex 保护。
    std::atomic<std::size_t>* m_live_tasks{nullptr};
    bool m_live_counted{false};
    std::atomic<bool> m_execution_claim{false};
    std::atomic<bool> m_run_claim{false};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_cancel_requested{false};
    MId m_last_machine_id{0};
    std::atomic<PId> m_last_processor_id{0};
    // M 亲和：最近运行的 M 与 LockToCurrentM 绑定。唤醒方（可能是外部
    // 线程）读这两个值做投递决策，因此必须是原子；写侧由执行该 G 的
    // worker 在 run() 内同步 Fiber 记录，同一 G 不会被并发执行。
    std::atomic<MId> m_affinity_machine{0};
    std::atomic<MId> m_locked_machine{0};
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
    // 状态转换锁只保护几条原子读写的组合（见 detail::HotSpinLock 注释），
    // 用轻量自旋锁替代 std::mutex：park/wake 往返的 6 次加锁从这里省下
    // 大部分指令数。
    mutable detail::HotSpinLock m_transition_mutex;
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
    /**
     * @brief 把当前线程当作 m0，执行一个可被调度的 main G（Go 的 main
     *        goroutine + runtime.main 的等价物）。
     *
     * ## 语义
     *
     * - 当前线程登记成一台不拥有 `std::thread` 的 M（绑定 P0），与普通
     *   worker 走**完全相同的**取任务/执行路径，因此 main G 可以 park /
     *   wake / yield / 使用 channel / 等待子 G；
     * - main G 通过 `LockToCurrentM` 语义绑定到这台 M：调度器只把它投进
     *   该 M 的定向槽，别的 M 误取到会重定向，所以它不会迁移；
     * - main G 进入终态后停止整个调度器（draining + join），`run_main`
     *   返回 true 表示 main 正常完成。
     *
     * ## 前提与边界
     *
     * 必须在 `start()` 之后、由**非 worker** 线程调用；同一 Scheduler 只能
     * 调用一次。main 返回等价 Go 的 `main` 返回：调度器停止，未完成的 G
     * 被取消（正在运行的 G 会在自己的安全点退出）。这与 Go 进程退出语义
     * 一致，但不会调用 `std::exit`，调用方仍可正常析构。
     *
     * @param function main 入口（在 Fiber 上执行）。
     * @return main 正常完成返回 true；调度器未启动/重复调用/入队失败返回
     *         false。
     */
    bool run_main(Task::Function function);
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

    /**
     * @brief 安装「M 自己轮询 IO 后端」的回调（Go runtime.netpoll 模型）。
     *
     * @details
     * 没有安装回调时，worker 无 G 可跑就睡在调度器条件变量上；IO 就绪事件
     * 由 IOManager 的独立 poller 线程发现，再经 wake_io() 跨线程唤醒
     * worker。在 WSL2/虚拟化环境下这条路径的一次往返实测 54–56µs，单事件
     * 场景因此被压到 5–9k/s。
     *
     * 安装后，worker 空闲时**直接在本 M 上调用 poll(timeout_ms)**：由同一个
     * 线程完成 epoll_wait、就绪事件处理与 G 的入队，park→poll→wake 在一个 M
     * 内闭环，不再产生跨线程唤醒。
     *
     * @param poll 后端轮询函数；timeout_ms 为最长等待毫秒数（>=0）。
     *             返回 >0 表示本轮处理了事件，0 表示正常超时，<0 表示后端
     *             不可用（worker 退回条件变量等待）。
     * @param wake 踢醒后端等待者的函数（写 eventfd）；可为空。
     *
     * @note 必须在 start() 之前安装（IOManager 在构造期完成）；重复安装返回
     *       false。回调在 worker 线程里执行，不得阻塞、不得抛出异常。
     */
    bool set_io_idle_poller(std::function<int(int)> poll,
                            std::function<void()> wake);
    /** @brief 已安装 IO 空闲轮询回调时返回 true。 */
    bool has_io_idle_poller() const noexcept;
    void SetObserver(SchedulerObserverPtr observer) noexcept {
        set_observer(std::move(observer));
    }
    SchedulerObserverPtr Observer() const noexcept { return observer(); }

    std::shared_ptr<Task> spawn(Task::Function function);
    std::shared_ptr<Task> spawn(Task::Function function, TaskOptions options);
    /**
     * @brief 在绝对时间提交一个 Fiber 任务。
     * @param deadline steady_clock 的绝对截止时间；过去时间会尽快提交。
     * @param function 到期后由 Scheduler 执行的 Fiber 入口。
     * @return 定时器句柄；Scheduler 停止或参数无效时返回无效句柄。
     */
    core::Timer schedule_at(std::chrono::steady_clock::time_point deadline,
                            Task::Function function);
    /** @brief 在相对延时后提交一个 Fiber 任务。 */
    core::Timer schedule_after(std::chrono::steady_clock::duration delay,
                               Task::Function function);
    /**
     * @brief 按固定延迟/固定频率周期提交 Fiber 任务。
     * @note 定时器线程只负责提交 Fiber，不直接恢复 Fiber 栈。
     */
    core::Timer schedule_every(std::chrono::steady_clock::duration period,
                               Task::Function function,
                               core::TimerOptions options = {});
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
    // 条件变量取消清理阶段使用；允许已启动 G 在 shutdown/draining 中
    // 继续 park，直到重新取得用户 mutex，避免返回时破坏锁契约。
    bool park_wait_for_cleanup(const std::shared_ptr<Task>& task);
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
    // IO poller 在一次 epoll_wait 中批量唤醒多个 G。实现先逐任务发布
    // Waiting->Runnable，再按最近 P 分桶、一次持有一个 P 队列锁，降低
    // 高频 IO 下的锁竞争；无法批量接纳的任务回退到 wake_io。
    // 前置条件：**只接受本 Scheduler 已登记（registered）的 G**。为降低批量
    // 路径的锁竞争，实现跳过 wake_io 的 owned_by 所有权校验，因此传入未注册
    // 或异属 task 会把不属于本调度器的 G 放进本调度器的 P 队列，破坏 owner
    // 不变量。当前唯一调用点是 IOManager::State::wake_nodes 的批量分支。
    bool wake_io_batch(const std::vector<std::shared_ptr<Task>>& tasks);
    // 等待节点选出终态后使用的通知交接。它跨越 shutdown/admission 竞态
    // 保留已经启动但仍挂起栈上的 G，避免提前标记为终态。
    bool wake_or_cancel(const std::shared_ptr<Task>& task);
    bool cancel(const std::shared_ptr<Task>& task);

    core::Timer ScheduleAt(std::chrono::steady_clock::time_point deadline,
                            Task::Function function) {
        return schedule_at(deadline, std::move(function));
    }
    core::Timer ScheduleAfter(std::chrono::steady_clock::duration delay,
                              Task::Function function) {
        return schedule_after(delay, std::move(function));
    }
    core::Timer ScheduleEvery(std::chrono::steady_clock::duration period,
                              Task::Function function,
                              core::TimerOptions options = {}) {
        return schedule_every(period, std::move(function), options);
    }

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

    /**
     * @brief 运行期切换唤醒投递策略（A/B 对比与线上调参）。
     * @param policy 新策略；只影响后续唤醒，已入队的 G 不会被搬动。
     * @note 与 config.wake_policy 同义；线程安全，可在调度运行期间调用。
     */
    void set_wake_policy(WakePolicy policy) noexcept;
    /** @brief 返回当前生效的唤醒投递策略。 */
    WakePolicy wake_policy() const noexcept;

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
    /**
     * @brief 枚举当前仍在注册表里的全部 G（只读快照）。
     * @details 遍历 16 个分片并逐个加锁读取；返回的是一组**分别读取**的结果，
     *          不是同一瞬间的全景（与其它快照一致，需如实标注 per-call 一致）。
     *          已进入终态并被回收的 G **不会**出现。
     */
    std::vector<TaskSnapshot> DebugSnapshotTasks() const;

    /**
     * @brief 按 gid 查该 G 是否仍在注册表中且**非终态**。
     * @details 只锁 id 按位与「分片数 - 1」对应的单个分片扫描，不枚举全量
     *          注册表；开销与进程历史任务数无关。DebugSnapshotTasks()
     *          返回的是全量存活快照，长跑进程下会有明显开销。
     * @return true 表示注册表里存在该 gid 且尚未进入终态；false 表示
     *         「已终态」或「不在注册表」，需要区分时用 TaskTerminal()。
     * @note 只读观测接口，不改变任何调度状态。
     */
    bool IsTaskAlive(GId id) const;

    /**
     * @brief 按 gid 查该 G 的终态分类（同样只查单个分片）。
     * @details 终态条目可能滞留注册表直到被回收，因此「已终止但未回收」的 G
     *          仍能查到具体终态；一旦被回收就变成 kUnknown。
     * @return kCompleted / kFailed / kCancelled；该 gid 不在注册表、
     *         或仍处于活动状态时返回 kUnknown（活动状态请配合
     *         IsTaskAlive() 判定）。
     * @note 只读观测接口；kUnknown **不等于**「已终止」。
     */
    TaskTerminalState TaskTerminal(GId id) const;

    // ---------------------------------------------------------------------
    // Doctor 的最小受控唤醒接口（不改变默认调度行为）
    // ---------------------------------------------------------------------

    /**
     * @brief 唤醒所有正睡在调度器空闲等待/IO 空闲轮询上的 worker。
     *
     * ## 为什么需要
     *
     * 外部诊断线程无法直接 notify 调度器内部的条件变量（它是 Impl 私有）。
     * 当唤醒令牌在业务侧丢失时，G 可能已经正确入队却没有 worker 被唤醒，
     * 于是整个进程「看起来卡死」。本接口只做**既有 `notify_all_workers()`
     * 的转发**：不修改任何状态字、不创建/销毁 G、不改变调度策略。
     *
     * ## 语义
     *
     * worker 被唤醒后必然重新检查 stopping/runnable/阻塞区判定，因此这是一次
     * **可安全重试**的唤醒：没有工作时它会重新睡回去（条件变量 + 超时）。
     *
     * @return 当前 worker 数（`worker_count()`；不代表此刻有几个在睡，只作为
     *         诊断输出）。
     * @note 只允许诊断/测试路径调用；热路径不要使用。
     */
    std::size_t ForceWakeIdle() noexcept;

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
    /// @brief 当前 worker 正在执行的 G（裸指针，仅限同线程立即使用）。
    /// @details 相比 `current_task()` 不复制 shared_ptr、不做原子引用计数，
    ///          适合加锁热路径上的诊断读取；可能返回 nullptr。
    static Task* current_task_ptr() noexcept;
    static Scheduler* current_scheduler() noexcept;
    /**
     * @brief 绑定/读取当前调度器上的 IOManager 不透明指针。
     * @details 由 io::IOManager 在构造/析构时维护；current() 用它避免在
     *          每次 recv/send hook 上取全局注册表锁。指针的存活期由
     *          IOManager 保证（析构时置空）。
     */
    void set_io_manager(void* manager) noexcept {
        m_io_manager.store(manager, std::memory_order_release);
    }
    void* io_manager() const noexcept {
        return m_io_manager.load(std::memory_order_acquire);
    }
    static MId current_machine_id() noexcept;
    static PId current_processor_id() noexcept;
    // 供拦截器和嵌入方在受管 G 中调用原生阻塞 API 时显式标记。一般代码
    // 优先使用 RAII 形式的 BlockingRegion。
    static bool enter_blocking() noexcept;
    static void leave_blocking() noexcept;
    static MId CurrentMachineId() noexcept { return current_machine_id(); }
    static PId CurrentProcessorId() noexcept { return current_processor_id(); }

private:
    // 前向声明提前到这里：publish_runnext/publish_to_machine 的参数用到
    // Impl::Processor；类内嵌套类型必须先声明再使用。
    class Impl;

    // 仅供拥有该 G 执行权的 worker 在 resume 返回后调用。
    // prefer_runnext：把 G 放进本 P 的快速槽（唤醒语义）还是队列尾
    // （yield 语义）。Go 的 goyield 用 runqput(next=false)，只有 goready
    // 用 runnext；两者混用会让持续 yield 的 G 一直占住快速槽，把被唤醒
    // 的 G 挤进队列而饿死（实测 1P/1M 的 Join 场景必现）。
    bool requeue_from_worker(const std::shared_ptr<Task>& task,
                             bool allow_notify = true,
                             bool prefer_runnext = true);
    // requeue_from_worker() 的「已预留 m_queued」版本：跳过入队权检查，
    // 直接发布本地队列节点。任何失败出口都会先 clear_queued() 并返回
    // false，由调用方走原有回退链（不会留下无节点的 reservation）。
    bool requeue_from_worker_reserved(const std::shared_ptr<Task>& task,
                                      bool allow_notify = true);
    // 外部 M 唤醒 G 时优先投递到 G 最近运行的 P，失败时回退到普通
    // admission 队列，避免混合锁交接每次争用全局 incoming stripe。
    bool requeue_to_processor(const std::shared_ptr<Task>& task,
                              PId processor_id);
    // 把已经占用 m_queued 的任务发布到 P 的 runnext 快速槽；被挤出的旧
    // 任务落到同一 P 的本地队列（满时再进全局队列）。返回 false 表示
    // 调用方需要回滚 m_queued 计数并走常规 enqueue()。
    // 参数用 PId 而不是 Impl::Processor&：Impl 是 .cpp 内的不完整类型，
    // 头文件无法引用它的嵌套类型（编译期硬约束）。
    bool publish_runnext(PId processor_id, const std::shared_ptr<Task>& task,
                         bool allow_notify);
    // 把任务发布到指定 M 的定向槽（inbox）。用于 LockToCurrentM 的
    // 严格绑定与「最近 M 空闲」的定向唤醒；M 不存在/已死亡返回 false。
    bool publish_to_machine(MId machine_id, const std::shared_ptr<Task>& task,
                            bool allow_notify);
    static void leave_blocking_for(Scheduler* scheduler,
                                   MId machine_id) noexcept;
    bool park_with_reason(const std::shared_ptr<Task>& task,
                          go2cpp::SuspendReason reason);

    // 不透明共享槽在 Scheduler 增删 machine 记录时保持动态 worker 的地址稳定。
    void worker_loop(std::shared_ptr<void> machine);

    class Impl;
    struct Lifetime;
    class TimerExecutorAdapter;
    std::shared_ptr<Lifetime> m_lifetime;
    std::unique_ptr<Impl> m_impl;
    std::atomic<void*> m_io_manager{nullptr};
};

// ---------------------------------------------------------------------------
// 持锁 park 契约计数器
// ---------------------------------------------------------------------------

/**
 * @brief 统计 release 模式下「`SpinMutex` 临界区内发生 park」的降级次数。
 *
 * @details release 构建不 abort：违反契约的 park 会降级为普通 `Mutex`
 *          语义（正确但慢），并在第一次发生时打印一次警告。debug 构建下
 *          同一事件会直接 abort，因此该计数恒为 0。
 * @return 进程内累计降级次数，单调递增。
 */
std::uint64_t spin_mutex_park_degrade_total() noexcept;

/**
 * @brief 统计普通 `Mutex` 临界区内发生 park 的次数（允许，但告警）。
 *
 * @details 计数来源是 `Task::held_mutex_count() > 0 && no_park_depth == 0`
 *          的所有 park 入口（包括 `read`/`write` 等 hook 触发的 IO park）。
 *          `sync.cpp` 的 `HeldLockParkCount()` 只覆盖 `sync` 原语自身的
 *          等待路径；本计数覆盖全部 park 入口，两者独立。
 * @return 进程内累计次数，单调递增。
 */
std::uint64_t mutex_held_park_total() noexcept;

}  // namespace go2cpp::scheduler

// 短别名让转译代码更紧凑；需要明确模块边界时仍可使用嵌套命名空间。
namespace go2cpp {
using Scheduler = scheduler::Scheduler;
using SchedulerConfig = scheduler::SchedulerConfig;
using Task = scheduler::Task;
using GState = scheduler::GState;
using TaskTerminalState = scheduler::TaskTerminalState;
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