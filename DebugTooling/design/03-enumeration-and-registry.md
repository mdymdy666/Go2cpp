# design · 03 枚举全部 G/Fiber 的机制（第 5 轮，解决前置问题）

> 第 4 轮暴露的结构性问题「**如何枚举全部 Fiber**」在本轮解决：
> 数据源、遍历范式、持锁边界、以及要新增的接口形态，全部有源码依据。

## 1. 数据源已存在：`task_registries`

```cpp
// src/concurrency/scheduler.cpp:1002
static constexpr std::size_t kRegistryStripeCount = 16;

// 1792-1805（Scheduler::Impl 成员）
mutable std::array<std::mutex, kRegistryStripeCount> registry_mutexes;
std::array<std::mutex, kIncomingStripeCount> incoming_mutexes;
std::array<std::deque<std::shared_ptr<Task>>, kIncomingStripeCount> incoming_queues;
std::array<std::vector<std::shared_ptr<Task>>, kRegistryStripeCount> task_registries;
```

**关键注释**（`1529`，原文）：

> The registry **strongly owns every non-terminal G**, including a Fiber

即：**只要一个 G 尚未进入终态（`kDead`/`kCancelled`/`kFailed`），它就在 `task_registries` 的某个分片里**，
且被 `shared_ptr` 强引用。这正是 dump 需要的"全集"。

## 2. 遍历范式已存在（可直接复用其形状）

`scheduler.cpp:1694-1705`（取消扫描）就是一个标准遍历：

```cpp
for (std::size_t stripe = 0; stripe < kRegistryStripeCount; ++stripe) {
    std::lock_guard<std::mutex> registry_lock(registry_mutexes[stripe]);
    for (const auto& task : task_registries[stripe]) {
        if (!task) { continue; }
        const auto state = task->state();
        if (state == GState::kDead || state == GState::kCancelled ||
            state == GState::kFailed) {
            continue;
        }
        // ... 对非终态 G 做处理
    }
}
```

另一个例子是 `all_tasks_terminal_locked()`（`1743-1750`），用 `std::all_of` 遍历同样结构。

**只读快照的正确形状**（设计，非本轮实现）：

1. 逐个分片加锁（**绝不一次持有 16 把锁**，避免与热路径争用与死锁风险）；
2. 每分片下把非终态 G 的**只读字段复制出来**（`Task::id()`/`state()`/`queued()`/`started()`/
   `cancellation_requested()`/`last_processor_id()`/`task_class()`/`failed()`，
   见 `scheduler.hpp:268-280`），**不长期持有 `shared_ptr` 之外的任何锁**；
3. 每个 G 的 **Fiber 侧**信息（`id`/`state`/`suspend_reason`/`depth`/`execution`/`context_snapshot`）
   通过 `Task` 内部的 `m_fiber`（`scheduler.hpp:364`）读取 —— **该成员私有**，
   因此需要一个 `Task` 上的只读访问器（设计上新增，见 §4）；
4. 分片之间**不是原子一致**的：分片 0 读完再读分片 1 期间，状态会变。
   → 快照必须**自带"分片级一致、全局非原子"的声明**（写进 dump 输出的元信息里），
   否则使用者会误以为拿到了同一瞬间的全景。

## 3. 某 P 的本地队列：`Processor::queue`

```cpp
// 1007-1021
struct Processor {
    PId id;
    mutable std::mutex mutex;                     // 每 P 一把队列锁
    std::deque<std::shared_ptr<Task>> queue;       // ★ 本地可运行队列
    std::atomic<PState> state{PState::kIdle};
    std::atomic<std::size_t> active_machines{0};
    std::atomic<std::size_t> running_machines{0};
};
```

- `ProcessorSnapshot`（`scheduler.hpp:210-214`）只导出 `{id, state, queued}`，
  其中 `queued` 就是 `queue.size()`；
- 使用者要求的「`G1LocalFiberQueue infos`（插件式显示 fiberid + 函数名 + 描述 + 状态）」
  → **数据在 `Processor::queue` 里，只是没有导出的接口**；
- 新增接口需要**持 `Processor::mutex` 遍历 `queue`**，同样只复制只读字段。

## 4. 需要新增的接口（按最小改动列出）

| # | 接口形态（设计） | 用途 | 备注 |
|---:|---|---|---|
| 1 | `std::vector<TaskSnapshot> Scheduler::snapshot_tasks() const` | 枚举全部非终态 G | 内部按 §2 的分片遍历；返回**值语义快照**，不暴露 `shared_ptr` |
| 2 | `std::vector<TaskId> Scheduler::processor_queue(PId) const` | 某 P 的队列内容 | 持 `Processor::mutex` 复制 id 列表 |
| 3 | `FiberContextSnapshot Task::context_snapshot() const` | 取某 G 的 Fiber 调用链 | 转发到私有 `m_fiber` 的 `context_snapshot()`（`fiber.hpp:210`） |
| 4 | `FiberExecutionBinding Task::execution_binding() const` | 取某 G 的 M/P 绑定 | 转发到 `m_fiber->execution_binding()`（`fiber.hpp:215`） |
| 5 | `std::optional<TaskMetadata> Scheduler::metadata(GId) const` | 名字/描述/函数/行号 | 依赖元数据机制（B1，尚未定案） |

> 接口 1 与 2 是**只读**且**不改变任何调度行为**，属于低风险；
> 接口 3/4 需要 `Task` 暴露其私有 `m_fiber`，但只暴露**只读视图**，不转移所有权。

## 5. 事件类型数量的更正（诚实性修正）

前几轮我在文档里写过「10 类事件」——**这是错的**。`scheduler.hpp:103-120` 实际定义了 **9 类**：

```
kStarted, kStopping, kWorkerStarted, kWorkerStopped,
kTaskStarted, kTaskSuspended, kTaskCompleted, kTaskFailed, kTaskCancelled
```

而发射点有 6 处，其中 `scheduler.cpp:3843-3851` 用三元判断**一次覆盖 4 种任务事件**：

```cpp
const auto task_event_type =
    task_state_after_run == GState::kDead      ? SchedulerEventType::kTaskCompleted
  : task_state_after_run == GState::kFailed    ? SchedulerEventType::kTaskFailed
  : task_state_after_run == GState::kCancelled ? SchedulerEventType::kTaskCancelled
  :                                              SchedulerEventType::kTaskSuspended;
```

即：`kDead → Completed`、`kFailed → Failed`、`kCancelled → Cancelled`、
**其余状态（含 `kWaiting`/`kRunnable`）→ `kTaskSuspended`**。
这条映射对"用事件还原任务生命周期"的场景（A7）是必须知道的细节：
**`kTaskSuspended` 是一个兜底类别**，不代表"只在 park 时发出"。

## 6. 指标采集的边界（顺带确认）

`collect_metrics` 为真时（`scheduler.cpp:3852-3862`）才累计：

- `metric_task_runs`（每次 `task->run()`）
- `metric_fiber_resume_ns`（本次 resume 的墙钟纳秒差）
- `metric_task_completions`（`task->terminal()` 时）

→ 说明 `SchedulerMetrics` 的**准确含义**是"**已被观测到的**运行次数与耗时"，
关闭 `collect_metrics` 时这些值**恒为 0**（不是"没有任务运行"）。
dump 场景 A8 必须把这个语义写进输出，否则使用者会误读。
