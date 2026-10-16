# 01 · 项目已有的调试相关基础设施（第 1 轮，源码证据）

> 本文只记录**已读过源码并确认**的事实，每条都给出文件与行号。
> 未读到的部分明确标注"未确认"，不做推测。

## 1. 并发子系统的规模（实测）

命令：`wc -l include/go2cpp/concurrency/*.hpp include/go2cpp/concurrency/scheduler/*.hpp include/go2cpp/concurrency/core/*.hpp src/concurrency/*.cpp`

| 文件 | 行数 | 角色 |
|---|---:|---|
| `src/concurrency/scheduler.cpp` | **4048** | 调度器核心实现 |
| `src/concurrency/context.cpp` | **2412** | Fiber 上下文切换 |
| `src/concurrency/fiber.cpp` | **1502** | Fiber 运行时 |
| `src/concurrency/sync.cpp` | **1324** | 同步原语（含锁） |
| `src/concurrency/timer.cpp` | 778 | 定时器 |
| `src/concurrency/channel.cpp` | 402 | 通道 |
| `src/concurrency/parking_condition.cpp` | 337 | 挂起/唤醒 |
| `src/concurrency/thread_policy.cpp` | 202 | 线程策略 |
| `src/concurrency/fiber_local.cpp` | 183 | Fiber 局部存储 |
| `src/concurrency/future.cpp` | 175 | Future |
| `src/concurrency/context_backend.cpp` | 45 | 上下文后端 |
| `include/go2cpp/concurrency/channel.hpp` | 2108 | 通道（模板） |
| `include/go2cpp/concurrency/future.hpp` | 952 | Future（模板） |
| `include/go2cpp/concurrency/scheduler/scheduler.hpp` | 662 | **调度器公开接口** |
| `include/go2cpp/concurrency/context.hpp` | 505 | Context |
| `include/go2cpp/concurrency/core/timer.hpp` | 344 | 定时器 |
| `include/go2cpp/concurrency/fiber.hpp` | 228 | Fiber 接口 |
| `include/go2cpp/concurrency/thread_policy.hpp` | 153 | 线程策略 |
| `include/go2cpp/concurrency/fiber_local.hpp` | 140 | Fiber 局部存储 |
| `include/go2cpp/concurrency/core/hybrid_mutex.hpp` | 114 | 混合锁 |
| `include/go2cpp/concurrency/core/parking_condition.hpp` | 98 | 挂起条件 |
| `include/go2cpp/concurrency/core/status.hpp` | 20 | 状态 |

**结论**：并发子系统约 **1.67 万行**；调试模块必须**接入**既有结构，而不是另起一套。

## 2. GMP 三态枚举（`scheduler/scheduler.hpp:47-101`）

### G（Fiber / 协程）

```cpp
enum class GState : std::uint8_t {
    kNew, kRunnable, kRunning, kWaiting, kDead, kCancelled, kFailed,
    // 并提供了 Go 风格别名：New/Runnable/Running/Waiting/Dead/Cancelled/Failed
};
```

### M（工作线程）

```cpp
enum class MState : std::uint8_t {
    kIdle, kRunning, kBlocking, kParked, kStopping, kDead, /* + 别名 */
};
```

### P（处理器资源）

```cpp
enum class PState : std::uint8_t { kIdle, kRunning, kStopping, kDead, /* + 别名 */ };
```

**对调试的意义**：使用者要求的 `G1cur_State(阻塞/挂起/异常)` **已有对应语义**
（`kWaiting`/`kParked`/`kFailed` 等），dump 时可直接映射，无需自造状态词表。

## 3. 事件流（`scheduler/scheduler.hpp:103-138`）

```cpp
enum class SchedulerEventType : std::uint8_t {
    kStarted, kStopping, kWorkerStarted, kWorkerStopped,
    kTaskStarted, kTaskSuspended, kTaskCompleted, kTaskFailed, kTaskCancelled,
};

struct SchedulerEvent {
    SchedulerEventType type;
    GId task_id{0};  MId machine_id{0};  PId processor_id{0};
    GState task_state{GState::kNew};
    std::chrono::steady_clock::time_point timestamp{};
};
```

**已有**：G/M/P 三者的 ID 同时出现在一个事件里 —— 这正是"Fiber 与 M/P 关联关系"
的数据来源，也是**时间线可视化**的基础。
**注意**：`timestamp` 是 `steady_clock`，注释明确"不能直接转换为墙上时间，
但适合计算耗时和排序"。

## 4. 观察者插件边界（`scheduler/scheduler.hpp:140-183`）——**关键约束**

```cpp
class SchedulerObserver {
public:
    virtual ~SchedulerObserver() = default;
    virtual void OnEvent(const SchedulerEvent& event) = 0;
};
using SchedulerObserverPtr = std::shared_ptr<SchedulerObserver>;
```

其 Doxygen 注释明确写了两条**限制**（原文要点）：

1. 「观察者**不拥有 Scheduler，也不能通过事件回调控制当前 G 的执行权**」；
2. 「回调**必须短小且无阻塞**；异常会被隔离」（`emit_event()` 统一捕获第三方插件异常，
   不能让 worker 线程异常退出）。

**这是本次设计最重要的分界线**：

| 使用者的需求 | 属于 |
|---|---|
| 查看 Fiber 状态 / 事件时间线 / 指标 | **只读观测** → 现有 `SchedulerObserver` 可承载 |
| 替换函数、跨 M 迁移、插队、强制断点暂停、决定锁的归属 | **运行时变更** → **Observer 明确不能做**，必须新增受控机制 |

## 5. 快照与指标（`scheduler/scheduler.hpp:210-256`）

```cpp
struct ProcessorSnapshot { PId id; PState state; std::size_t queued; };
struct MachineSnapshot {
    MId id; MState state; PId processor;
    TaskClassId last_task_class;
    std::size_t affinity_hits, affinity_misses;
    bool processor_detached;      // sysmon 标记的"长时间阻塞、已脱离 P"
    GId blocking_task;
    std::uint64_t long_syscall_count;
};
struct SchedulerMetrics {
    std::uint64_t task_runs, task_completions, fiber_resume_ns,
                  local_queue_pops, steal_pops;
};
```

**对调试的意义**：
- `queued` → 直接对应使用者要的 `G1LocalFiberQueue infos` 的**规模**；
- `processor_detached` / `blocking_task` / `long_syscall_count` → **阻塞诊断**的现成字段；
- `fiber_resume_ns` / `steal_pops` / `local_queue_pops` → 性能归因；
- **缺口**：这些快照**没有** Fiber 的"名字 / 描述 / 行号"，也没有调用栈。

## 6. 配置面（`scheduler/scheduler.hpp:161-197`）

`SchedulerConfig` 已含（全字段）：`processor_count`、`max_workers`、`idle_wait`、
`local_queue_limit`、`min_workers`、`idle_worker_timeout`、`fiber_stack_size`、
`task_affinity_budget`、`allow_worker_oversubscription`、`enable_sysmon`、`sysmon_interval`、
`long_syscall_threshold`、`pin_workers_to_cpu`、`fiber_bin_capacity`、`collect_metrics`、
`observer`。

注释里两条对调试有用的说明：
- `enable_sysmon`：「Go sysmon 风格的阻塞监控。它**只观察已声明的 BlockingRegion/Hook 边界，
  不会从另一个线程强行切断任意 C++ 调用栈**」——这条同样约束了"强制暂停 Fiber"的设计；
- `pin_workers_to_cpu`：「Linux 上可将 M 绑定到与 P 对应的 CPU」，默认关闭。

## 7. 目前确认的**能力缺口**（对应使用者需求）

| 使用者需求 | 现状 | 缺口 |
|---|---|---|
| Fiber 任务信息（名字、编号、行号、描述） | 有 `GId`、`GState`；**无名字/描述/行号** | 需要元数据注入机制（转译期或运行期） |
| 运行中替换函数 | 无 | 需要受控的控制面 |
| 跨 M 迁移 | 无 | 需要控制面 + 安全边界（运行中的栈不可迁移） |
| 插队到指定 Fiber 之前 | 无 | 需要队列的受控重排接口 |
| `dump scheduler` CLI（ini/json，≥15 选项） | 有快照/指标**数据**，**无序列化器、无 CLI** | 需要 dump 模块与格式层 |
| 插件式断点 | 有 Observer（只读） | 需要"暂停 + 单步 + 断点命中"机制 |
| 锁争用决策 | `core/hybrid_mutex.hpp`（114 行）已存在策略 | 需确认策略是否可插拔（**未读，待第 2 轮**） |
| 与 gdb 混合调试 | 无 | 需要线程/协程映射与符号信息的导出 |
| 多机同步调试（Stratum topic） | 无 | 需要传输层插件与 topic 协议 |

## 8. 未确认事项（不做推测，留待后续轮次）

- `Task` 类的完整公开接口（`scheduler.hpp:258-406` **未读**）；
- `Scheduler` 类的完整公开接口（`scheduler.hpp:407-640` **未读**）；
- `fiber.hpp` 全文（**未读**）——Fiber 是否携带名字/描述/调用栈信息，尚未确认；
- `core/hybrid_mutex.hpp` 与 `sync.cpp` 的锁策略点（**未读**）；
- `context.cpp` 是否维护可查询的执行位置（行号）（**未读**）。
