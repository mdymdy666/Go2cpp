# scenarios · B 运行时变更（8 个场景详细设计）

> 每个场景给出：**目的 / 前置条件 / 安全边界 / 现有能力（证据）/ 需要新增什么 / 风险**。
> B 类的共同约束来自项目自身的注释（必须遵守，不可绕过）：
>
> 1. `SchedulerObserver`「**不能通过事件回调控制当前 G 的执行权**」（`scheduler.hpp:140-183`）；
> 2. `Scheduler::BlockingRegion` 与 `enable_sysmon`「**不会从另一个线程强行切断任意 C++ 调用栈**」
>    （`scheduler.hpp:399-400,409-412`）；
> 3. `Fiber::resume` 必须**串行**（`fiber.hpp:90-93`）；`Task::m_execution_claim`
>    保证同一时刻只有一个 M 进入同一个 Fiber（`scheduler.hpp:330-333`）。

## B1 · 查询任务元数据（名字 / 描述 / 函数 / 行号）

- **目的**：使用者第一诉求 —— 「知道这个 fiber 的 `s_main_task` 名字、fiber 编号、函数行号、描述」。
- **现状**：❌ 完全没有。`Task` 只有私有 `std::function<void()> m_function`（`scheduler.hpp:362`）；
  `FiberContextFrame` 无任何名字/行号字段（`fiber.hpp:54-67`）；
  `SchedulerConfig`/`TaskOptions` 也无元数据字段（`scheduler.hpp:161-208`）。
- **前置结论**：`context.cpp` 里**没有**任何执行位置 API（第 3 轮 grep 计数为 0），
  所以**行号不可能从运行时反射得到**。
- **两条可行路线（待定案，不能写死）**：
  | 路线 | 做法 | 优点 | 代价 |
  |---|---|---|---|
  | **A. 转译期注入** | Go→C++ 转译器把「函数名/文件/行号」作为 `TaskOptions` 或注册表条目写入 | 信息最全、零运行时代价 | 依赖转译器改造；对**手写 C++** 无效 |
  | **B. 运行期注册** | 新增 `Scheduler::register_metadata(GId, TaskMetadata)` 或 `spawn(fn, options)` 扩展 | 手写代码可用、实现简单 | 需要调用方配合；动态名称无法自动获得 |
  - **设计倾向（需使用者确认）**：A + B **共存**——转译期自动注入，运行期 API 允许覆盖/补充。
- **安全边界**：纯只读，无风险。
- **新增**：`TaskMetadata`（name/description/function/file/line）+ 存储 + 查询接口。

## B2 · 运行中替换 Fiber 的函数体

- **现状**：❌ `m_function` 私有且无 setter；`Task` 只有私有的 `release_callable()`（`scheduler.hpp:356,362`）。
- **关键未知（必须先读代码）**：`Task::run()` 如何使用 `m_function` ——
  是**每次运行都取一次**，还是**首次取出后清空**？这决定替换语义。
  从 `release_callable()` 的命名推测是"取出并移交"（一次性），**但这是推测，必须验证**（第 6 轮）。
- **安全边界**：
  - 只能对**未在执行**的 G 替换（即 `m_execution_claim == false`，`scheduler.hpp:370`）；
  - 对**已挂起但栈仍存活**的 G 替换"函数体"是**危险**的：挂起栈上的局部对象与
    RAII 状态属于原函数，替换后续执行会破坏栈不变量
    （`fiber.hpp:125-131` 已警告"挂起 Fiber 在错误父级析构会安全放弃上下文"）。
  - → **可行语义只有两种**：① 对**尚未首次运行**的 G 替换入口；
    ② **延迟替换**：登记新的函数体，**等当前主体正常返回后**由调度器再执行一次（不改栈）。
- **风险**：高（栈不变量）。必须先读 `Task::run()` / `release_callable()` 再定案。

## B3 · 把 Fiber 跨 M 迁移

- **现状**：❌ `FiberExecutionBinding` 注释明确是"**只保存不拥有对象的标识**"（`fiber.hpp:32-34`），
  是**观测元数据**，不是控制接口。
- **安全边界（决定性的）**：**运行中的 Fiber 不可能被迁移** —— 它的栈正在某个 M 上执行。
  唯一安全窗口是 G 处于 `kWaiting`/`kRunnable`（未占用 M）。
- **已有的基础设施**：`Scheduler::requeue_to_processor(task, processor_id)`（**私有**，`scheduler.hpp:623`）
  与注释「外部 M 唤醒 G 时**优先投递到 G 最近运行的 P**」（`621-624`）。
  → 迁移的语义就是"**改变下次唤醒时投递到哪个 P**"，已经有内部实现，只需受控暴露。
- **新增**：`bool Scheduler::migrate(GId, PId target_processor)`，前置校验：
  G 存在、非终态、当前**未在执行**（`execution_claim == false`）；失败返回 false 并**不做任何改动**。
- **风险**：中（队列归属变更，需与 work stealing 和 P 队列锁正确协同）。

## B4 · 把 Fiber 插队到指定 Fiber 之前

- **现状**：❌ `enqueue` 只做追加（`scheduler.hpp:492`）；`Task` 的后端入队接口
  （`try_mark_queued*`，`301-308`）也没有位置概念。
- **数据层可行性**：✅ 队列是 `std::deque<std::shared_ptr<Task>>`（`Processor::queue`，`scheduler.cpp:1020`）
  与 `std::vector<...> task_registries` —— **都支持中间插入**。
- **第 19 轮订正（关键约束）**：本地入队有**两条**路径 ——
  `scheduler.cpp:2471`（`Scheduler::enqueue`，受 `local_queue_limit` 约束，超限改投全局队列）
  与 `2553`（`requeue_to_processor`，唤醒路径）。
  **插队实现必须同时覆盖两条路径**，否则另一条路径仍会把任务追加到队尾，导致行为不一致。
- **安全边界**：
  - 必须持 `Processor::mutex`（`scheduler.cpp:1018`）修改队列；
  - 目标 Fiber 必须**确实在同一个 P 的队列里**，否则语义不成立（应报错而非静默失败）；
  - **插队会破坏 FIFO 公平性**（现有设计的隐含约定）→ 必须**默认关闭**、
    由显式开关（例如 `GO2CPP_DEBUG_ALLOW_REORDER`）启用，并在 dump 输出中标注"已启用调试重排"。
- **新增**：`bool Scheduler::enqueue_before(GId moving, GId target)`。
- **风险**：中高（改变可运行顺序会影响延迟分布，测试需覆盖）。

## B5 · 外部暂停 / 插件式断点

- **现状**：❌ `Fiber::Suspend()` 是**协作式**（Fiber 自己调用，`fiber.hpp:156`）；
  `SuspendForScheduler` 也是被调度器在切换前调用的（`161-162`）。
- **硬约束**：项目注释反复强调「**不会从另一个线程强行切断任意 C++ 调用栈**」
  （`scheduler.hpp:399-400`、`enable_sysmon` 注释、`BlockingRegion` 注释）。
  Go 1.14+ 的"信号异步抢占"在这里**不可用**（会破坏 C++ 栈的 RAII 不变量）。
- **唯一可行方案：协作式断点（安全检查点）**
  - 在已有的**协作挂起点**上检查断点标志：`Fiber::Suspend`/`SuspendForScheduler`、
    `Task::park_for_scheduler`、`mark_yielded*`、`Scheduler::yield_current`/`park_current`
    （`scheduler.hpp:335-345,511-524`）；
  - 断点标志可复用与取消标记相同的机制（Fiber 本地标记 + `CancellationRequested()` 同款，
    `fiber.hpp:165-166,181-184`）；
  - **代价**：只能停在安全点，**不能停在任意行**。这一点必须在文档与 CLI 帮助里**明确写出**，
    否则使用者会按 gdb 的直觉期待"随时中断"。
- **与 gdb 的分工（重要）**：gdb 能停**任意行**但看不懂 Fiber（只看到 M 线程）；
  本机制能停在**安全点**但看得懂 GMP。两者是**互补**而非替代 —— 这是 E4 场景的基础。
- **风险**：中（安全点数量决定断点精度，需要先统计安全点分布）。

## B6 · 取消指定 Fiber 并观测收敛

- **现状**：✅ 基本具备：`Scheduler::cancel(task)`（`scheduler.hpp:545`）、
  `Task::cancel()`（`346`）、协作式 `CancellationRequested`（`fiber.hpp:165-166`）、
  以及事件 `kTaskCancelled`（`scheduler.hpp:117`）。
- **设计要点**：取消是**协作式**的 —— 「主体在下一次 resume 前后通过 `CancellationRequested()` 观察它」
  （`fiber.hpp:181-184`）。**忽略取消的主体不会被强制终止**。
  → 调试工具的"取消"必须提供**超时与观测**：发出取消 → 观察 `kTaskCancelled` 事件或超时 → 报告"未收敛"。
- **风险**：低（复用既有机制）。

## B7 · 运行期修改调度参数

- **现状**：⚠️ `SchedulerConfig` 只在构造时传入（`scheduler.hpp:161-197`），
  未见运行期 setter；唯一例外是观察者（`set_observer`，`463`）。
- **可行性分级（基于字段语义，需第 6 轮读实现确认）**：
  | 类别 | 字段 | 可热改性 |
  |---|---|---|
  | 观测类 | `observer`（✅ 已有）、`collect_metrics` | 高（只影响采集） |
  | 节律类 | `idle_wait`、`idle_worker_timeout`、`sysmon_interval`、`long_syscall_threshold` | 中（影响调度节律，需原子读） |
  | 结构类 | `processor_count`、`max_workers`、`min_workers`、`fiber_stack_size`、`fiber_bin_capacity` | **低/不可**（注释明确「动态配置不得拆开修改相互依赖的结构参数」，`163-165`） |
- **设计含义**：只提供**观测/节律类**的 setter，**结构类明确不支持**（并给出错误信息），
  而不是做一个"什么都能改"的接口 —— 后者必然破坏不变量。

## B8 · 运行期替换同步等待后端

- **现状**：✅ **已有官方接口**：`WaitBackend`（`sync.hpp:13-44`）+
  `SetWaitBackendFactory()` / `ResetWaitBackendFactory()`（`sync.hpp:46-66`、`sync.cpp:612/627`）。
- **关键约束（注释原文）**：「只影响**注册后创建的等待节点**，正在等待的 Fiber **不会迁移后端**」。
- **能做什么**：替换"**怎么 park / 怎么 wake**"，例如插入日志、统计每次等待耗时、
  转发到外部监视器。
- **不能做什么**：❌ **不能决定"谁获得锁"** —— FIFO 出队在 `Mutex::Unlock` 路径
  （`sync.cpp:955-1005`），`WaitBackend` 接口里没有"选择"的语义。
- **风险**：中（后端实现必须遵守 `Park` 的谓词重查与虚假唤醒约定）。

## B 类共同结论

1. **唯一已经开放的运行期替换点是 B8**（`WaitBackend` 工厂）——其余 7 个都需要新增接口；
2. **所有变更类接口都必须默认关闭、显式启用**，因为其中多个（B4 插队、B5 断点）
   会改变既有语义（FIFO 公平性、无强制中断假设）；
3. **三个必须先读代码才能定案的问题**（第 6 轮）：
   - `Task::run()` / `release_callable()` 的调用语义（决定 B2 的替换时机）；
   - `requeue_to_processor` 的完整实现与调用点（决定 B3 的接口形状）；
   - P 队列的取任务路径（决定 B4 插入点的正确位置）。
