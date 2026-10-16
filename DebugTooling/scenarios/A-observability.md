# scenarios · A 只读观测（8 个场景详细设计）

> 每个场景给出：**目的 / 前置条件 / 触发方式 / 期望信息 / 现有能力（证据）/ 缺口 / 风险**。
> A 类的共同约束：**不得干扰被观测者**（不改变 G 的调度顺序、不阻塞 worker）。

## 共同前置

- 观测动作**运行在独立的观测线程**上（绝不放在 `SchedulerObserver::OnEvent` 里做重活：
  回调是在 **worker 线程**上同步执行的，见 `scheduler.cpp:1104-1122` 与 `scheduler.hpp:140-183` 的注释）；
- 快照的**一致性**问题：现有接口都是逐个调用的（先 `runnable_count()` 再 `processors()`…），
  两次调用之间状态会变。是否需要"一次取一份一致快照"的组合接口，是**待定设计点**。

## A1 · 单个 Fiber 快照

- **目的**：给定一个 Fiber（或其 `id`），拿到它的完整可读状态。
- **触发**：`dump scheduler --fiber=1 --format=ini`
- **期望信息**：`id`、`state`（`FiberState`）、`suspend_reason`、`depth`、`main_fiber`、
  `alive`、`active`、`cancellation_requested`、`last_thread`、`execution.{scheduler_token,
  task_id,machine_id,processor_id,managed}`
- **现有能力**：✅ `Fiber::debug_info()` / `Fiber::id()` / `state()` / `suspend_reason()` /
  `nesting_depth()` / `execution_binding()`（`fiber.hpp:186-221`）
- **缺口**：拿到 `Fiber*` 的**入口**（如何按 id 找到 Fiber —— 见 §6.1 的注册表问题）
- **风险**：低（纯读）

## A2 · Fiber 调用链（含墓碑帧）

- **目的**：还原"谁创建了它、谁恢复的它"，含已结束父级的墓碑。
- **触发**：`dump scheduler --fiber=1 --stacks --stack-depth=8`
- **期望信息**：`FiberContextSnapshot` 的每一帧：`id`、`parent_id`、`active_parent_id`、
  `depth`、`alive`、`active`、`state`、`suspend_reason`、`last_thread`
- **现有能力**：✅ `Fiber::context_snapshot()`（`fiber.hpp:206-211`），
  注释明确「父对象结束后由共享记录保留 `alive=false` 的**墓碑帧**」
- **缺口**：与 A1 相同（入口）+ 调用栈里**没有函数名**（见 B1）
- **风险**：低

## A3 · G→M→P 绑定查询

- **目的**：回答"这个 Fiber 现在/最近在哪个 M、哪个 P 上"。
- **触发**：`dump scheduler --fibers --include=execution`
- **期望信息**：`FiberExecutionBinding`（4 个 id + `managed`）
- **现有能力**：✅ `Fiber::execution_binding()`（`fiber.hpp:215`）、
  `Scheduler::current_task()/current_machine_id()/current_processor_id()`（`scheduler.hpp:607-610`）
- **缺口**：无（若配合 A1 的入口）
- **风险**：低；注意 binding 是"**不拥有的标识**"，调度器可能已销毁 → 使用前需校验
  （`fiber.hpp:32-34`、`212-214` 注释）

## A4 · 调度器全局概览

- **目的**：一眼看到规模。
- **触发**：`dump scheduler --summary`
- **期望信息**：`processor_count`、`worker_count`、`runnable_count`、`global_runnable_count`、
  `is_running`、sysmon 状态
- **现有能力**：✅ `scheduler.hpp:448,582-598`
- **风险**：低

## A5 · 每个 P 的队列快照

- **目的**：定位"任务都堆在哪个 P 上"。
- **触发**：`dump scheduler --processors`
- **期望信息**：每 P 的 `id` / `state` / `queued`
- **现有能力**：✅ `processors()` → `ProcessorSnapshot`（`scheduler.hpp:210-214,592`）
- **缺口**：**没有队列内元素的明细**（只有长度）—— 使用者要求的
  「`G1LocalFiberQueue infos`（插件式显示 fiberid + 函数名 + 描述 + 状态）」需要
  **新增"列出某 P 队列内容"的接口** ❌
- **风险**：中（遍历队列需要与调度器同步，必须避免持锁过久）

## A6 · 每个 M 的状态与阻塞诊断

- **目的**：找出"卡住的 M"。
- **触发**：`dump scheduler --machines`
- **期望信息**：`id`、`state`、`processor`、`last_task_class`、`affinity_hits/misses`、
  `processor_detached`、`blocking_task`、`long_syscall_count`
- **现有能力**：✅ `machines()` → `MachineSnapshot`（`scheduler.hpp:217-233,593`）
- **风险**：低
- **价值**：`processor_detached` + `blocking_task` + `long_syscall_count` 三者组合，
  已足够定位"哪个 M 卡在原生调用上"——**这是现成的高价值诊断**

## A7 · 事件时间线

- **目的**：还原"什么时候发生了什么"。
- **触发**：`dump scheduler --events=1000`（历史）或 `--follow`（实时）
- **期望信息**：`SchedulerEventType`（10 类）+ `task_id`/`machine_id`/`processor_id`/
  `task_state`/`timestamp`
- **现有能力**：✅ 事件类型与结构（`scheduler.hpp:103-138`）+ 观察者（`173`）
- **缺口**：❌ **没有历史缓冲**（Observer 只有推送），需插件侧实现有界环形缓冲
- **风险**：中（缓冲写入必须在观测线程；回调里只做入队）

## A8 · 指标采样与差分

- **目的**：算出"这段时间的运行成本"。
- **触发**：两次 `dump scheduler --metrics` 求差
- **期望信息**：`task_runs`、`task_completions`、`fiber_resume_ns`、`local_queue_pops`、
  `steal_pops` 的差值
- **现有能力**：✅ `metrics()`（`scheduler.hpp:236-243,599`），注释明确「采用**累计值**，
  便于调用方按两次快照的差值计算」
- **缺口**：无（差分由使用方做）
- **风险**：低

## A 类共同缺口汇总

| 缺口 | 影响的场景 | 性质 |
|---|---|---|
| **如何枚举全部 Fiber**（注册表） | A1/A2/A3/A5 | **前置条件**，必须先在设计中解决 |
| 某 P 队列内的元素明细（fiberid/名字/状态） | A5 | 新增只读接口（需注意持锁时长） |
| 事件历史环形缓冲 | A7 | 插件侧实现，不进热路径 |
| Fiber 的函数名/描述/行号 | A1/A2 | 需新增元数据机制（见 B1） |
| 一致快照（多次调用间的状态漂移） | 全部 | 待定设计点 |
