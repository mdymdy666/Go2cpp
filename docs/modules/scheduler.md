# 模块文档：scheduler（GMP 调度器）

> **状态：现行**。基线 `HEAD 1b46de8`（2026-10-26 核对）。
> 本文是 `scheduler` 模块的**唯一现行文档**：接口、设计、边界、测试都在这里。
> 旧文档（`docs/scheduler.md`、`docs/gmp-optimization-log.md`、`docs/gmp-perf-analysis.md`、
> `docs/gmp-scenario-analysis.md`、`docs/gmp-use-cases.md`、`docs/concurrency-park-points.md`、
> `docs/concurrency_audit_2026-10-18.md`）已并入本文并在各自顶部加了合并标注；
> 原文保留作追溯，**其中的行号、计数、性能结论以本文为准**。
> 引用约定：`file:line` 均指当前工作树；跨文档引用的数字标注了出处与前提。

---

## 1. 概述与职责

调度器是并发运行时的核心：它把用户提交的 `Task::Function` 包装成 **G**，交给 **P**
（逻辑处理器）排队，由 **M**（OS 线程）执行。三层的分工是：

| 层 | 对象 | 拥有什么 | 数量由谁决定 |
|---|---|---|---|
| G | `scheduler::Task`（内含一个 `Fiber`） | 入口函数、栈、状态机、取消标记 | 用户提交量 |
| P | 私有 `Scheduler::Impl::Processor` | 一个本地 runqueue + 活动/运行 M 计数 | `SchedulerConfig::processor_count`，默认硬件并发 |
| M | 私有 `Scheduler::Impl::Machine` + `std::thread` | 真正执行字节码/代码的线程，及其首选 P | `min_workers`（常驻）+ 阻塞区补位，受 `max_workers` 封顶 |

职责边界（`include/go2cpp/concurrency/scheduler/scheduler.hpp:572-586`）：

- **做**：任务生命周期、GMP 绑定、本地/全局队列、work stealing、park/wake 交接、
  取消与失败传播、sysmon 观测、有界 shutdown、指标与快照。
- **不做**：不替换用户提供的同步原语；不异步抢占任意 C++ 栈；不拥有调用方保存的
  `Task` 句柄；不实现 IO/定时器轮询（由 `IOManager`、`TimerService` 注入）。
- **关键前提**：所有阻塞原生调用必须通过 `BlockingRegion` 或 Hook 边界显式声明，
  调度器**不能**从别的线程强行切断任意 C++ 调用栈（`scheduler.hpp:580`）。

`include/go2cpp/concurrency/scheduler.hpp` 只是转发头（18 行），真身在
`include/go2cpp/concurrency/scheduler/scheduler.hpp`（968 行）。

---

## 2. 文件布局

| 文件 | 内容 |
|---|---|
| `include/go2cpp/concurrency/scheduler/scheduler.hpp` | 全部公开接口：状态枚举、`SchedulerConfig`、`Task`、`Scheduler`、快照与指标（968 行） |
| `src/concurrency/scheduler.cpp` | 实现：`Task` 状态机、`Scheduler::Impl`（P/M/队列/分条）、worker_loop、sysmon、park/wake、shutdown（4597 行） |
| `src/concurrency/fiber.cpp` / `context_x64.S` | G 的执行载体（见 [fiber.md](fiber.md)） |
| `include/go2cpp/concurrency/thread_policy.hpp` | `ScopedThreadParticipation` 等线程策略元数据，仅影响线程亲和性提示 |
| `include/go2cpp/concurrency/core/parking_condition.hpp` | worker 空转等待用的 futex 化条件变量 |

---

## 3. 核心 API

### 3.1 生命周期与配置

| API | 位置 | 语义 |
|---|---|---|
| `Scheduler(SchedulerConfig config = {})` | `scheduler.hpp:614` | 构造；`Impl` 在构造期归一化配置并创建 P |
| `Scheduler(std::size_t processor_count)` | `:615` | 便捷构造，等价于只设 P 数 |
| `void start()` | `:621`，实现 `scheduler.cpp:2021` | 拉起 `min_workers` 个基础 M 与 sysmon |
| `bool shutdown_for(duration)` | `:627`，实现 `scheduler.cpp:2075` | 有界收尾；超时/worker 自身调用返回 false，可稍后再试 |
| `void shutdown()` | `:622`，实现 `scheduler.cpp:2286` | 无界收尾 |
| `bool is_running()` | `:628` | 观测 |
| `BlockingRegion`（RAII，不可移动） | `:593-612`，实现 `scheduler.cpp:3383-3404` | 声明「本 M 将进入原生阻塞调用」 |

`SchedulerConfig` 关键字段与默认值（`scheduler.hpp:206-268`，归一化见
`scheduler.cpp:1247-1316`）：

| 字段 | 默认 | 说明 |
|---|---|---|
| `processor_count` | 0 → 硬件并发 | P 数 = 并行度；`default_processor_count()` 见 `scheduler.cpp:236` |
| `max_workers` | 0 → `2*P`（允许超额时），并受 `min(P, max)` 约束 | `scheduler.cpp:1259-1281` |
| `min_workers` | 0 → P | 常驻基础 M；**默认不允许空闲退出** |
| `idle_wait` | 10 ms | worker 空转等待节拍 |
| `local_queue_limit` | 256 | P 本地队列软上限；超过时入队改投全局队列（`scheduler.cpp:2637-2648`） |
| `terminal_collect_interval` | 256 | 每完成 N 个终态 G 回收一个注册表条带（共 16 条） |
| `idle_worker_timeout` | 250 ms | 补充 M 复查阻塞是否结束的周期 |
| `fiber_stack_size` | 2048 | 初始提交量；页对齐后最小 1 页（4096） |
| `fiber_stack_reserve_size` | 8 MiB | 每 Fiber 预留虚拟地址 |
| `fiber_stack_growable` | true | 按需增长总开关 |
| `fiber_bin_capacity` | 128（clamp 到 4096） | 每 M 的 Fiber 复用池上限 |
| `task_affinity_budget` | 4 | 软亲和扫描预算（只影响扫描顺序） |
| `allow_worker_oversubscription` | true | 阻塞区可临时超过 P；false 时严格 `M ≤ P` |
| `allow_idle_shrink` | **false** | true 才恢复「空闲超时缩容到 min_workers」 |
| `enable_sysmon` / `sysmon_interval` / `long_syscall_threshold` | true / 10 ms / 50 ms | 长阻塞监控 |
| `pin_workers_to_cpu` | false | 把 M 绑到与 P 对应的 CPU（Linux） |
| `collect_metrics` | true | 关闭后不再累计计时，语义不变 |
| `observer` | 空 | 生命周期事件观察者（见 §5.5） |

### 3.2 提交与执行

| API | 位置 | 语义 |
|---|---|---|
| `spawn(Function)` / `spawn(Function, TaskOptions)` | `scheduler.hpp:677-678` | 创建 G 并入队；返回 `shared_ptr<Task>` |
| `go(...)` / `Spawn` / `Go` / `add(...)` | `:697`、`:767-778` | 同义便捷入口；`add(task)` 返回入队结果或空指针 |
| `enqueue(task)` | `:698`，实现 `scheduler.cpp:2443` | 重新发布一个已构造 G |
| `schedule_at / schedule_after / schedule_every` | `:685-696` | 经 `TimerService` 在到期后提交 Fiber（定时器线程不直接恢复栈） |
| `yield(task)` / `yield_current()` | `:717`、`:729` | 当前 G 让出执行权，回本 P 队列 |
| `park(task)` / `park_current()` | `:718`、`:730` | 当前 G 进入等待态 |
| `park_wait(task)` | `:722` | 同步原语等待的轻量挂起（避免每次 handoff 拿 admission 锁） |
| `park_io(task)` | `:728` | IO 就绪等待；诊断帧保留 `SuspendReason::Io` |
| `park_wait_for_cleanup(task)` | `:725` | 取消清理阶段专用：允许在 shutdown/draining 中 park |
| `Task::wait() / wait_for(d)` | `:451-452` | 等待终态；Managed G 用 `ParkingCondition`，普通线程走原生条件变量 |
| `Task::run()` | 实现 `scheduler.cpp:809` | worker 的执行入口（内部串行化执行权） |

`TaskOptions`（`scheduler.hpp:275-288`）：`stack_size`、`task_class`、
`fiber_bin_capacity`、`stack_reserve_size`、`stack_growable`。

### 3.3 park / wake

| API | 位置 | 返回值语义 |
|---|---|---|
| `wake(task)` | `scheduler.hpp:734`，实现 `scheduler.cpp:2997` | `true` 已入队；`false` 仍可能已接受 pending 唤醒 |
| `wake_registered(task)` | `:738`，实现 `scheduler.cpp:3001` | 已注册 G 的快速唤醒，跳过所有权校验 |
| `wake_io(task)` | `:739`，实现 `scheduler.cpp:3105` | IO 轮询器专用；只发布状态/token，不碰 Fiber 栈 |
| `wake_io_batch(tasks)` | `:747`，实现 `scheduler.cpp:3166` | 批量唤醒：先逐任务 `Waiting→Runnable`，再按最近 P 分桶入队；**只接受本调度器已注册的 G** |
| `wake_or_cancel(task)` | `:750`，实现 `scheduler.cpp:3297` | 终态交接：跨越 shutdown/admission 竞态 |
| `cancel(task)` | `:751`，实现 `scheduler.cpp:3350` | 协作取消；不等待 |
| `Task::park_for_scheduler()` | 实现 `scheduler.cpp:577` | `kParked` / `kRequeued` / `kRejected`（见 §4.4） |

`WakeAction`（`kRejected/kEnqueue/kPending`，`scheduler.hpp:62-66`）与
`ParkAction`（`kRejected/kParked/kRequeued`，`:69-73`）是状态机的对外枚举。

### 3.4 观测与诊断

| API | 位置 | 说明 |
|---|---|---|
| `processor_count()` / `worker_count()` / `runnable_count()` | `scheduler.hpp:788-790` | 标量计数 |
| `global_runnable_count()` | `:791` | 全局队列长度（不含 incoming/P 本地） |
| `processors()` / `machines()` | `:798-799`，实现 `scheduler.cpp:3557`、`:3658` | 只读快照，不授予队列所有权 |
| `DebugSnapshotTasks()` | `:806`，实现 `scheduler.cpp:3573` | 枚举注册表中**非终态**的 G |
| `IsTaskAlive(gid)` | `:817`，实现 `scheduler.cpp:3608` | 单条带查询，开销与历史任务数无关 |
| `TaskTerminal(gid)` | `:828` | `kCompleted/kFailed/kCancelled`；`kUnknown` **不等于**已终止 |
| `metrics()` | `:860` | `SchedulerMetrics` 累计计数器（见下） |
| `sysmon_running()` / `sysmon_pass_count()` | `:856-859` | monitor 活性心跳 |
| `ForceWakeIdle()` | `:853`，实现 `scheduler.cpp:3533` | 诊断用受控唤醒（转发 `notify_all_workers()`） |
| `current_task()` / `current_task_ptr()` / `current_machine_id()` / `current_processor_id()` | `:868-887` | 线程局部观测 |
| `current_scheduler()` | `:873` | 当前 worker 所属调度器 |

`SchedulerMetrics`（`scheduler.hpp:355-366`）字段：`task_runs`、`task_completions`、
`fiber_resume_ns`、`local_queue_pops`、`steal_pops`。它们**只增不减、无时间窗**，
适合看趋势与比例，不适合直接算 QPS（`:341-354`）。

---

## 4. 设计

### 4.1 G：`Task` 状态机

`GState`（`scheduler.hpp:76-94`）：`kNew → kRunnable → kRunning →
kRunnable|kWaiting|kDead|kCancelled|kFailed`。别名 `New/Runnable/...` 供转译代码直接使用。

不变量（`scheduler.cpp` 与 `docs/scheduler.md` 原有契约，已核对实现）：

- 注册表（16 条带，`kRegistryStripeCount = 16`，`scheduler.cpp:1056`）强持有所有
  **非终态** G —— 包括已 park、不在任何队列里的 G，防止挂起栈被提前销毁
  （`scheduler.cpp:1686-1690`）；
- 一个可运行 G 最多拥有一个队列节点：`m_queued` 是入队权；`m_execution_claim`
  是执行权；出队与 `Runnable→Running` 在 `try_mark_running()` 的同一临界区完成
  （`scheduler.cpp:610-624`）；
- `runnable` 计数只统计已接纳的队列节点，pending wake 不计入；
- 用户回调/捕获对象**绝不在**调度器锁内析构：回收路径先把条目搬到
  `deferred_destruction`，锁外再 `release_callable()`（`scheduler.cpp:4300-4351`）。

### 4.2 队列拓扑与取任务路径

四条队列（`Scheduler::Impl`，`scheduler.cpp:1883-1955`）：

| 队列 | 结构 | 生产者 | 消费者 |
|---|---|---|---|
| P 本地 `queue` | `std::deque` + P 锁（`:1088-1090`） | 本 P 的 worker（yield/park 回流）、admission 入队 | 本 P 的 worker；窃取者从**队尾**取 |
| `incoming_queues[16]` | 16 条带 + 各自锁（`:1904-1910`） | 外部线程（`enqueue` 非 worker 路径） | 任意 worker |
| `global_queue` | `std::deque`，admission 锁保护（`:1925`） | 本地队列满、shutdown 收尾、worker 侧入队 | admission 路径 |
| 紧急救援链 `emergency_head/tail` | `Task` 内嵌 `m_emergency_next`（`:1926-1927`） | 分配失败、shutdown、取消收尾 | admission 路径优先取 |

worker_loop（`scheduler.cpp:3764`）每轮按固定顺序取 G：

1. **本地快路径**（`:3833-3857`）：只在 `runnable > P` 且自己 P 有货时执行，
   只拿 P 队列锁，不碰调度器总锁。取到后立刻 `try_mark_running()` claim。
2. **incoming 中路径**（`:3861-3962`）：按 `incoming_probe` 轮转起点扫描 16 条带；
   若 M 有 task class 亲和预算，先扫匹配类别的条带，再退回普通扫描；用
   `incoming_nonempty_mask` 跳过空条带。
3. **admission 慢路径**（`:3966-4134`）：拿 `Impl::mutex`，顺序为
   `emergency → 本地 FIFO → 全局 FIFO → 窃取`；本路径把「出队 + 关停检查 +
   `Runnable→Running` claim」作为一个事务完成。
4. **空闲**（`:4397-4547`）：若安装了 IO idle poller 且允许，worker 自己在
   本 M 上 `poll(timeout)`（Go `runtime.netpoll` 模型）；否则睡在
   `ParkingCondition` 上，按 `idle_wait`/`idle_worker_timeout` 超时醒来重检。

`local_queue_limit` 只在 **admission 入队**路径生效：本地队列达到上限时把新 G 放进
`global_queue`（`scheduler.cpp:2637-2648`），避免单个 P 无限膨胀。

### 4.3 work stealing

窃取发生在 admission 慢路径内（`scheduler.cpp:4084-4119`）：

- 仅当 `processors.size() > 1` 时才尝试；用 `next_processor` 轮转选择第一个受害者，
  对所有非本 P 的 P 做**有界扫描**（最多 P−1 次）；
- 从受害者队列**队尾**取（LIFO），使受害者保有最老的 G（近似公平）；
- 成功时 `metric_steal_pops++`，失败按 1/16 采样打点；
- 亲和性只影响扫描顺序（`take_victim` 先在预算内找同类），找不到同类就取队尾，
  **窃取始终可用**，不会因亲和饿死（`scheduler.hpp:301-303`）。

历史结论（仍有效的方法论）：曾尝试「把窃取移出全局锁」，结果整体 **−34%**——本地
队列边界不能动（`scheduler.cpp:1071-1073`）。因此当前实现刻意保留
「本地快路径 → admission」两级结构。

### 4.4 park / wake 协议

**park 入口**（`Task::park_for_scheduler()`，`scheduler.cpp:577-607`）：

1. 契约检查（必须在状态转换前）：若当前 G 处于 `SpinMutex` 临界区
   （`no_park_depth > 0`）→ debug abort / release 降级并计数；
   若持有普通 `Mutex`（`held_mutexes > 0`）→ 计数 + 一次性告警；
2. 取 `m_transition_mutex`；
3. 非 `kRunning` → `kRejected`；
4. **消费 pending wake**：`m_wake_pending.exchange(false)` 为真 → `kRequeued`，
   留在当前执行流（避免「先通知后挂起」丢唤醒）；
5. 提交 `kWaiting` → `kParked`。

**wake 入口**（`scheduler.cpp:492-530`）：`Waiting` → `Runnable` 并入队（`kEnqueue`）；
若目标仍在 `Running`，只置 pending token（`kPending`），由下一次 park 消费。
所有 wake 分支共用同一把转换锁，**不访问目标 Fiber 栈**；真正的 resume 由
`m_execution_claim` 串行化，因此不存在两个 M 同时进入同一 Fiber。

**三个专用 wake**：`wake_for_scheduler`（通用）、`wake_for_io`（IO 轮询器）、
`wake_for_wait`（同步原语跨 M 等待）。后两者只发布状态/token，唤醒方不直接 resume。

**交接与自愈**：`mark_yielded_fast()`（`:650`）是无锁 CAS 快路径；
`resume_after_failed_park()`（`:718`）在 Fiber 拒绝挂起时把状态回滚为 `kRunning`；
`recover_orphan_running()`（`:740`）在 `run()` 返回后修复「无执行者的 Running」；
队列节点 claim 失败由 `recover_unclaimed`（`:3801-3823`）按状态条件清理或补发布。

**resume 后的入队策略**：yield 回来的 G 放回**本 P** 且 `allow_notify=false`
（`scheduler.cpp:4266`）。这是有意的：刚 yield 的 G 马上会被本 M 从队首取走，
此时 `notify_one` 会唤醒睡着的 M 从队尾偷走它，把几百纳秒的本地续跑放大成
**4~5µs** 的跨 M 唤醒（`scheduler.cpp:4261-4265`、`:4334-4341`；64 Fiber 实测整档
慢 10 倍）。只有「真正结束了一个 G 且仍有可运行任务」或 park 返回时才 notify。

### 4.5 动态 M、sysmon 与 BlockingRegion

**基础 M 与补充 M**（`scheduler.cpp:1104-1110`、`:1422-1508`）：

| 类型 | `replacement` | 创建时机 | 退休条件 |
|---|---|---|---|
| 基础 M | false | `start()` 创建 `min_workers` 个 | **不因空闲退出**，常驻到 shutdown |
| 补充 M | true | `blocking_workers > 0` / sysmon 解绑时 | `blocking_workers == 0` 且无 detached 时**立即退休** |

扩容上限 `worker_ceiling = min(max_workers, P + effective_blocking)`
（`scheduler.cpp:1462-1467`）：普通 runnable 洪峰只把 M 增长到 P 有界；
每个显式声明的阻塞 M 额外给一个补位槽。`allow_worker_oversubscription = false`
时 `max_workers` 被夹到 `P`，即严格一 M 一 P（`:1278-1281`）。

> 历史权衡：曾把默认 `min_workers` 改成 `2*P`，性能 **+11.7%**（s4 27,786 →
> 31,032），但破坏了 `tests/network/test_hook.cpp` 的 `read_f` 用例所要求的
> 「阻塞一个 M 后整个 P 停摆」语义，已回退（`scheduler.cpp:1285-1289`）。
> 性能收益不能以改变阻塞语义为代价。

**sysmon**（`enable_sysmon`，`scheduler.cpp:1513-1602`）：

1. 独立线程按 `sysmon_interval`（10 ms）扫描；心跳 `sysmon_pass_count` 在扫描前递增；
2. 扫描只对 `Impl::mutex` 做 `try_to_lock`，失败置 `sysmon_retry_pending` 并把下个
   周期缩到 1 ms（`:1526-1534`、`:1578-1586`）；
3. 只观察状态为 `MState::kBlocking` 的 M：`blocking_since` 超过
   `long_syscall_threshold`（50 ms）→ 置 `processor_detached`，从 P 的 attached 计数
   扣一次，并申请替代 M（`:1535-1570`）；
4. 原 M **仍在自己 C++ 调用栈中运行**，返回时 `leave_blocking_for()` 原子清标记、
   把 M 重新计入 P，并 `notify_all_workers()` 让补充 M 当场退休（`:3465-3507`）。
5. 替代槽按 `min_workers + detached` 计算，不会因整机 P 多而无条件开 P 个线程
   （`:1474-1485`）。

**BlockingRegion 契约**（`scheduler.hpp:589-612`、`scheduler.cpp:3383-3449`）：

- 只有当前 M 处于 `kRunning` 且当前有 G 时才能进入（`:3425-3437`）；
- 进入时发布 `MState::kBlocking` + `blocking_since_ns` + `blocking_task` +
  `blocking_workers++`，并在锁外调用 `maybe_grow()` 让排队的 G 立刻有替代 M 可跑
  （`:3442-3447`）；
- 销毁时按**进入时记录的 machine id** 恢复，而不是重新查当前 M —— 期间可能发生
  Fiber 迁移（`:3451-3462`）；
- 对象**禁止移动**（`scheduler.hpp:600-603`）：移动会让析构发生在别的 Fiber/线程，
  使原 M 永久停在 `kBlocking`；
- worker 在 G 的任务边界发现仍有活动 region 会 `std::terminate()`
  （`scheduler.cpp:4239-4245`）：宁可 fail-fast，也不能把失真的
  `blocking_workers`/attached 计数静默传给下一个 G。

### 4.6 shutdown、draining 与回收

- `shutdown()` 单向：关闭 admission → 取消未启动 G → 给已启动 G 设置合作取消 →
  唤醒 parked G → 等注册表全部终态（`scheduler.cpp:1729-1855`、
  `shutdown_for` `:2075`）。
- 未启动 G 没有挂起栈，可原地取消；**已启动 G 必须保持可运行**，让 Fiber 从
  trampoline 正常返回并执行栈上 RAII（`:1732-1734`）。
- worker 自己发起 shutdown 时只发布 drain 请求，不 join 自己；拥有者线程做最终 join
  （`reap_dead_workers`，`:1362-1404`）。
- 注册表回收：常规路径每完成 `terminal_collect_interval`（256）个终态 G 压缩**一个**
  条带（约 16 个节拍覆盖全表，`:4312-4325`）；draining 时全表回收并检查是否
  `all_tasks_terminal`（`:4305-4311`、`:1857-1881`）。
- 持锁退出的终态 G（`held_mutex_count > 0`）**不回收**：它是 Doctor 判定
  owner-gone 的唯一证据，数量等于泄漏的 mutex 数，不随历史任务数增长
  （`reclaimable_terminal`，`:1613-1623`）。

---

## 5. 边界与降级

### 5.1 不做异步抢占

`scheduler.hpp:830-853` 与 `scheduler.cpp:1510-1512` 都明确：sysmon 只做**可逆的
资源记账**（detach + 替代 M），不会从另一个线程终止、迁移或恢复任意 C++ Fiber 栈，
也不会向 `write` 之类系统调用注入异常。未 Hook、未包在 `BlockingRegion` 中的阻塞
调用**不可被 sysmon 发现**；raw syscall、第三方阻塞库、达到 `max_workers` 时只能保持
现有线程语义。

### 5.2 持锁 park 的检测与降级（调度器侧）

> 完整的「哪些调用会 park / 哪些会阻塞 M」清单由 `docs/modules/sync.md` §4.3 维护
> （该文认领了 `docs/concurrency-park-points.md`）；本节只保留调度器侧的检测点、
> 降级策略与 BlockingRegion 契约。

调度是协作式的，park 点显式存在。`SpinMutex` 临界区内只允许纯计算；任何可能 park
或阻塞 M 的调用必须移到临界区外，或改用 `sync::Mutex`。

| 条件 | 含义 | debug | release |
|---|---|---|---|
| `no_park_depth > 0` | `SpinMutex` 临界区 | 打印 G id/深度/调用栈 → `abort()` | 降级为 `Mutex` 语义 + `spin_mutex_park_degrade_total()` +1 + 一次性警告 |
| `held_mutexes > 0` 且 `no_park_depth == 0` | 普通 `Mutex` 临界区 | 计数 + 一次性日志 | `mutex_held_park_total()` +1 |

检测点在 `Task::park_for_scheduler()` 最前面（`scheduler.cpp:577-591`），所有 park
路径（`park_wait`/`park_io`/`park_with_reason`/`park_wait_for_cleanup`）都汇聚于此。
计数器接口见 `scheduler.hpp:923-942`。

**会 park（临界区禁止）**：`read/recv/recvfrom/recvmsg`、`write/send/sendto/sendmsg`、
`connect`、`accept/accept4`、FIFO/pipe 读写（非 `O_NONBLOCK` 时）；
`sleep/usleep/nanosleep/clock_nanosleep`；`sync::Mutex::Lock*`、
`ConditionVariable::Wait*`、`WaitGroup::Wait*`、`channel` 收发、`future/promise::get`。
**不 park 但阻塞整个 M（同样禁止）**：`*_f` 原始版本、`poll/select/epoll_wait`
（事件循环入口，只标 `BlockingRegion`）、未 hook 的 `fsync/getaddrinfo/wait4`。

经验法则：临界区里不要出现「等」这个词；`*_f` 不是规避检查的手段，它只是把
协程死锁换成线程阻塞。

### 5.3 降级矩阵

| 触发条件 | 降级结果 | 语义影响 |
|---|---|---|
| `allow_worker_oversubscription=false` | `max_workers = min(max, P)` | 阻塞区无法补位，P 可能被占住 |
| `allow_idle_shrink=true` | 基础 M 空闲缩到 `min_workers` | 恢复历史行为，空载线程数下降 |
| 线程创建失败/资源耗尽 | `maybe_grow` 停止扩容 | 并行度下降，已接纳 G 不丢（`:1500-1503`） |
| 队列节点分配失败 | 已启动 G 进紧急救援链；未启动 G 取消并通知 | 不分配、不抛、不悬挂（`:2650-2676`） |
| `enable_sysmon=false` | 无 detach/替代 M | 长阻塞期间该 P 停摆 |
| 未声明阻塞（无 region/hook） | 不可见 | 与普通线程语义相同 |
| `set_io_idle_poller` 未安装 | worker 睡条件变量，IO 走 poller 线程跨线程唤醒 | WSL2 实测一次往返 54–56µs（`scheduler.hpp:653`），单事件 5–9k/s |

### 5.4 观测边界

- `DebugSnapshotTasks()` 是**逐条带分别读取**的快照，不是同一瞬间全景；已回收的终态
  G 不会出现。全量 dump 在长跑进程下会随历史任务数线性膨胀（实测 48 s 达 23 万条 /
  61 MB），所以它只返回非终态 G（`scheduler.cpp:3578-3582`）。
- `TaskSnapshot` 不包含「阻塞在哪个锁/channel」——实现没有记录，**不虚构**
  （`scheduler.hpp:321-326`）。
- `kUnknown` 只表示「不在注册表或仍活动」，不能当作已终止（`scheduler.hpp:96-110`）。

### 5.5 观察者插件边界

`SchedulerObserver`（`scheduler.hpp:185-199`）在 `SchedulerConfig::observer` 注入，或
`set_observer()` 热替换。`emit_event()`（`scheduler.cpp:1223-1245`）先复制观察者指针、
释放锁，再回调；异常在边界隔离。没有观察者时走 `has_observer` 原子快路径，不取时钟、
不加锁、不拷 `shared_ptr`（`:1897-1901`）。事件是只读快照，**不能**用它改变当前 G 的
执行权；需要新队列/栈/IO 后端时应实现独立后端，再把结果以事件接入。

---

## 6. 测试

### 6.1 当前构建的 `ctest -N`（事实记录）

在任务指定的构建 `/UserData/build/go2cpp-mcp-integrated` 上执行 `ctest -N`：

```text
Total Tests: 14
  #1-#12  go2cpp_os_monitor_* / go2cpp_os_kernel_monitor* / go2cpp_os_tcp_io_monitor*
  #13     go2cpp_bufio_test
  #14     go2cpp_bufio_stress
```

该构建是 eBPF/bufio 集成构建，**未注册 scheduler/fiber 用例**。并发用例注册在
`cmake/Go2CppTests.cmake`，默认构建（如 `build-work`）的 `ctest -N` 才能看到它们。
本文不把「未在该构建出现」写成「没有测试」。

### 6.2 与 scheduler 直接相关的 ctest 注册

| 用例 | 注册位置 | 说明 |
|---|---|---|
| `go2cpp_tests` | `cmake/Go2CppTests.cmake:216`（TIMEOUT 120） | 汇总二进制；`GO2CPP_TEST_FILTER` 选模块，未匹配返回 2 |
| `go2cpp_unknown_filter_rejected` | `:289` | 断言未知 filter 返回 2，不能把空跑伪装成通过 |
| `go2cpp_known_filter_runs` | `:295` | `GO2CPP_TEST_FILTER=error` 的配对验证 |
| `go2cpp_scheduler_smoke` | `:318`（TIMEOUT 30） | `tests/concurrency/scheduler_smoke.cpp`，`local_queue_limit=4`（:41） |
| `go2cpp_scheduler_queue_latency` | `:350`（TIMEOUT 300） | `tests/concurrency/test_scheduler_queue_latency.cpp` |
| `go2cpp_park_wake_pair` | `:152`（TIMEOUT 300） | park/wake 配对压测 |
| `go2cpp_primitives_stress` | `:132` | 10 类并发结构压测 |
| `go2cpp_primitives_cancel` | `:142` | 取消/超时 |
| `go2cpp_concurrency_combinations` | `:243` | 组合场景 |
| `go2cpp_concurrency_fuzz` | `:253` | 随机并发 |
| `go2cpp_deadlock_detection` | `:263` | 死锁检测 |
| `go2cpp_high_load_stress` | `:327` | 高负载 |
| `go2cpp_state_machine` | `:342` | 状态机矩阵 |

`go2cpp_tests` 内与调度器相关的模块过滤器（`tests/test_main.cpp:87-90`，`beginner` 见 `:98`）：
`scheduler`、`scheduler_stress`、`terminal_reclaim`、`dynamic`、`beginner`。

对应源文件：`tests/concurrency/test_scheduler.cpp`（announce `"GMP scheduler state and
shutdown"`）、`test_scheduler_stress.cpp`、`test_dynamic_scheduler.cpp`、
`test_terminal_reclaim.cpp`、`test_park_wake_pair.cpp`、`test_concurrency_fuzz.cpp`。

覆盖范围（`docs/scheduler.md:108-113` 原文，已核对代码仍成立）：P=1/多 P、重复
enqueue/wake、跨 Scheduler 拒绝、park 取消、Fiber 取消、队列 capture 回收、动态
增长/收缩/再增长、严格 P 上限、BlockingRegion 替代 M、task class 计数、worker
shutdown、析构重入、带 watchdog 的压力循环。

### 6.3 可引用的性能/场景数据（含前提，勿当现行基线）

| 数据 | 前提 | 出处 |
|---|---|---|
| P=48/M=12 27,809 qps ≈ P=12/M=12 27,786 qps | 证明 P 超配不增加并行度 | `scheduler.cpp:1251-1254` 引 `docs/parameter-tuning.md` 轮次 6 |
| 默认 `min_workers=2P` 时 +11.7%（27,786→31,032）但破坏阻塞语义 | 已回退 | `scheduler.cpp:1285-1289` |
| 一次跨线程 IO 唤醒往返 54–56µs，单事件 5–9k/s | WSL2/虚拟化 | `scheduler.hpp:653` |
| 跨 M 唤醒 4~5µs vs 本地续跑几百 ns；64 Fiber 整档慢 10 倍 | 64 Fiber yield 场景 | `scheduler.cpp:4264-4265`、`:4340-4341` |
| 把窃取移出全局锁 −34% | 早期优化实验 | `scheduler.cpp:1072-1073` |
| 批量就绪扇出 331,812 events/s vs epoll 池 51,000（6.5×） | FDS=64、25 波、workers=8、WSL2 12 核 | `docs/gmp-use-cases.md:33-40`（2026-10-19 修正版） |
| 长连接 s3：吞吐平手，P99 14.98ms vs 5.44ms、CPU 601% vs 86% | 8,000 连接 | `docs/gmp-use-cases.md:71-81` |
| 多核小包 s2：GMP 83,424 vs epoll 池 93,311 qps（−11.9%） | 256 连接 × depth 32 | `docs/gmp-use-cases.md:87-96` |
| s1 短连接：GMP 19,121 vs epoll 池 64w 16,687（+14.6%），但 CPU 682% vs 145%、0.48% 连接重置 | 4 客户端 × 16 连接 | `docs/gmp-use-cases.md:60-65` |
| s1 对 **per-connection thread**：20,284 vs 19,524（+3.9%） | 注意基线是「每连接一线程」，不是线程池 | `docs/gmp-perf-analysis.md:402` |

> ⚠️ `docs/gmp-perf-analysis.md`、`docs/gmp-scenario-analysis.md` 与
> `docs/gmp-optimization-log.md` 的旧对照数据基于**不公平的 epoll 池模型**
> （256 线程抢同一把锁、每请求两次跨线程交接），作者已自标作废（见各文件顶部
> 2026-10-19 警告与 `docs/INDEX.md` 第十节）。本文只保留其中的方法论结论
> （如「注入延迟会污染 p50」「必须用 utime/stime 拆分用户态与内核成本」）。

---

### 6.4 运行期开关（诊断/实验）

| 开关 | 作用 | 位置 |
|---|---|---|
| `Scheduler(processor_count)` / benchmark `--workers` | 显式指定 P 数；曾因 benchmark 把 `processor_count` 写死为 0 而误判 GMP 能力 | `scheduler.hpp:615` |
| `pin_workers_to_cpu` | 把 M 绑到与 P 对应的 CPU（历史实测 QPS 29,075→32,789、sys −35%） | `scheduler.hpp:252` |
| `GO2CPP_NO_GROW=1` | 诊断旁路：跳过 `maybe_grow`/`reap_dead_workers`，用于判断扩容维护是否是劣化来源 | `scheduler.cpp:1426-1434` |
| `GO2CPP_SLEEP_BLOCKING=1` | 让 `usleep` 类调用真阻塞 M（走 `invoke_native_blocking`），用于对比 park 与真阻塞 | `src/network/hook.cpp:1301-1306` |
| `GO2CPP_TEST_FILTER` | 选择 `go2cpp_tests` 模块；未匹配返回 2 | `tests/test_main.cpp:73-79` |
| `GO2CPP_GROWABLE_STACK=0` | Fiber 栈退化为固定栈（见 fiber.md §5.4） | `src/concurrency/fiber.cpp:328-335` |

函数级入口：`Scheduler::set_io_idle_poller()`（worker 自己 `poll`，见 §4.2）与
`Scheduler::ForceWakeIdle()`（诊断唤醒）。二者都不改变默认调度策略。

---

## 7. 已证伪 / 回滚清单（不要重试）

以下都是历史上真实做过、有数据、然后被推翻或回退的改动。写在这里是为了让后来者
**不要再花一轮**；数字来自 `docs/gmp-optimization-log.md` 的轮次记录（历史日志，
口径已标注），不是现行基线。

| 改动 | 结果 | 结论 |
|---|---|---|
| 把 work stealing 移出全局锁 | 整体 **−34%** | 本地队列边界不能动（`scheduler.cpp:1071-1073`） |
| 默认 `min_workers = 2*P` | +11.7%（27,786→31,032），但破坏「阻塞一个 M 后整个 P 停摆」语义 | 已回退（`scheduler.cpp:1285-1289`） |
| accept 批量化 | ±0 | 服务端 accept 不是瓶颈 |
| Fiber 池容量 32→256 | +2% | 收益不够覆盖内存，保持 128 配置默认 |
| 单 iovec 的 `::recv/::send` 替身 | s2 **−5.6%** | 不采用 |
| pending wake 快速路径 | 净收益 0 | 已删除 |
| `interest==0` 时不 `EPOLL_CTL_DEL` | 轮次 30 关注入 +5.9%，轮次 31 开注入 s4 **−7.8%** | 已回滚；教训是任何改动必须用开注入口径复测 |
| epoll 忙轮询 | 0.65 次/请求中 84% 是真实事件，收益不抵 CPU | 不采用 |
| `thread_local` eventfd 复用 | 轮次 1 回滚，轮次 33 重新采用；sys 仅 −1% | 大头是跨 M futex，不是 eventfd 创建 |
| 用 `strace -c` 的每请求 syscall 数归因 | 放大 5–10 倍，`usecs/call` 放大 10–100 倍 | 只能统计「次数」，不能当 CPU 成本 |

另有三条**引用陷阱**（数据没错、但前提被误读）：

1. `--model thread` 是**每连接一个 `std::thread`**（s3 时 8,001 线程），不是线程池；
   线程池基线必须写 `--model epoll --workers N`。旧文档的「3.6×」「线程数 1/385」
   「+3.9%」都因混淆基线作废。
2. 开注入延迟（每请求 `usleep(U(0,5ms))`）时 p50 主要由注入决定，不能用来比较调度器；
   关/开注入的数字不可直接对比（`docs/gmp-perf-analysis.md:29-41`）。
3. 跨轮次绝对 QPS 不可比：同场景 s2 在 WSL2 上从 30,720 漂到 21,389（约 25%）。
   新结论只写**同窗口相对差**。

---

## 8. 旧文档合并对照

| 旧文档 | 并入本文的内容 | 不再保留 |
|---|---|---|
| `docs/scheduler.md` | G/M/P 契约、状态转换、队列不变量、动态 M、sysmon、shutdown、观察者边界 | 2026-09-23 的时效性叙事 |
| `docs/gmp-optimization-log.md`（1,663 行） | 失败优化的教训（窃取移出全局锁 −34%、`2P` 常驻 M 破坏语义）、观察者热路径成本 | 逐轮 QPS 对比（作废） |
| `docs/gmp-perf-analysis.md` | 方法论：注入延迟、utime/stime 拆分、客户端瓶颈 | 旧模型对照数字（作废） |
| `docs/gmp-scenario-analysis.md` | 「批量就绪」是 GMP 唯一显著优势的机制解释 | 未完成的逐轮追加 |
| `docs/gmp-use-cases.md`（2026-10-19 修正版） | 选型结论、6.5× 扇出、s1/s3/s2 代价 | 旧版「3.6×/1/385」结论（已作废） |
| `docs/concurrency-park-points.md` | 调度器侧的持锁 park 检测/降级矩阵（§5.2） | park 点全量清单已归入 `docs/modules/sync.md`/`channel.md`，不在此重复 |
| `docs/concurrency_audit_2026-10-18.md` | channel/future/io 的 park 与唤醒审计结论（已合入 §4.4/§5.2 的交接语义） | 113/113 时期的计数与具体缺陷单 |

---

## 9. 已知局限与待办

1. **无异步抢占**：CPU 密集的 G 只能靠 yield/park 让出；`sysmon` 只能处理已声明的
   阻塞。
2. **`local_queue_limit` 只约束 admission 入队**：外部生产者的 incoming 条带无此上限，
   靠分条锁与 worker 轮转摊平。
3. **`blocking_workers` 与 detached 计数依赖显式声明**：未声明的阻塞在观测上不可见。
4. **`DebugSnapshotTasks()` 是逐条带快照**，长跑进程下开销与存活 G 成正比。
5. **观察者回调在 worker 线程内执行**：必须短小无阻塞，否则直接拖慢调度。
6. 本文未在本机构建上重跑并发用例；`ctest -N` 的 14 条与并发注册清单已如实分开记录。
   需要实测时用默认构建 + `GO2CPP_TEST_FILTER=scheduler`（见 §6.2）。
