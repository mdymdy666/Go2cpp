# 并发运行时（Fiber / Scheduler / Channel / 同步原语）· 深入学习

> 读法：先读第一部分建立全局图，再按需下钻第二部分。每个结论都给了 `文件:行`，
> 行号以 2026-11-03 的工作树为准；代码会漂移，读到时先 `sed -n` 看一眼。
> **数字纪律**：本文不引用任何历史文档的 ns/吞吐当结论。凡给性能，都是
> 「2026-11-03 本机实测（12 vCPU WSL2，未绑核，3 轮）」，换环境必须重测。
> 配套考试：`docs/exam/01-concurrency.md`。

## 代码地图

| 层 | 文件 | 规模 | 职责 |
|---|---|---|---|
| 门面 | `include/go2cpp/concurrency/scheduler.hpp` | 18 行 | 转发到 `scheduler/scheduler.hpp` |
| 调度器 | `include/go2cpp/concurrency/scheduler/scheduler.hpp` | 1310 行 | G/M/P 状态、配置、Task、公开 API |
| 调度器实现 | `src/concurrency/scheduler.cpp` | 6528 行 | 队列、park/wake、worker_loop、sysmon、shutdown |
| Fiber | `include/go2cpp/concurrency/fiber.hpp` / `src/concurrency/fiber.cpp` | 552 / 4074 行 | 栈、上下文切换、FiberLocal、复用池 |
| Channel | `include/go2cpp/concurrency/channel.hpp` / `src/concurrency/channel.cpp` | 4305 / 829 行 | hchan 缓冲、等待队列、Select |
| 同步 | `src/concurrency/sync.cpp` + `rwmutex.hpp` 等 | 3068 行 | Mutex/RWMutex/Cond/WaitGroup/WaitNode |
| 等待原语 | `include/go2cpp/concurrency/core/parking_condition.hpp` | 214 行 | Fiber/线程双路径等待、per-node futex |
| Context/Future | `src/concurrency/context.cpp` / `future.hpp` | 2733 / 1237 行 | 取消传播、回滚、Promise/Future |
| Timer | `src/concurrency/timer.cpp` / `core/timer.hpp` | 1343 / 505 行 | multimap + 单后台线程 |

---

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 入口点清单

**调度器生命周期**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `Scheduler(SchedulerConfig)` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:892` | 用户 / 默认调度器 | 归一化配置、创建 P 数组（`src/concurrency/scheduler.cpp:2103-2116`） | 对象 |
| `Scheduler(processor_count)` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:893` | 用户 | 等价于只设 P 数 | 对象 |
| `start()` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:899`，实现 `src/concurrency/scheduler.cpp:3282` | 用户 / `spawn` 前 | 拉起 `min_workers` 个基础 M + sysmon | void |
| `run_main(fn)` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:925`，实现 `src/concurrency/scheduler.cpp:3317` | 想要 Go `main` 语义的用户 | 当前线程当 m0，main G 绑定到本 M | bool |
| `shutdown_for(d)` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:931`，实现 `src/concurrency/scheduler.cpp:3402` | 所有者线程 | 关 admission → drain → 等终态 → 停 sysmon → join | bool（超时/worker 内调用为 false） |
| `shutdown()` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:926`，实现 `src/concurrency/scheduler.cpp:3616` | 所有者线程 | `shutdown_for(duration::max())` | void |

**任务提交与等待**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `spawn(fn)` / `spawn(fn, opts)` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:981-982`，实现 `src/concurrency/scheduler.cpp:3750` | 用户 / `go()` | 构造 Task（首次执行才建栈）、入队 | `shared_ptr<Task>` |
| `go(fn)` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:1001` | 用户 | `spawn` 别名 | 同上 |
| `park_wait(task)` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:1026`，实现 `src/concurrency/scheduler.cpp:4426` | 同步原语（Mutex/CV/WaitGroup） | 把当前 G 置 Waiting 并切走 | bool（被唤醒/取消） |
| `yield_current()` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:1033`，实现 `src/concurrency/scheduler.cpp:4384` | 用户 / 调度器 | 当前 G 回本 P 环尾 | bool |
| `park_current()` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:1034`，实现 `src/concurrency/scheduler.cpp:4569` | 用户 | 当前 G park | bool |
| `wake(task)` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:1038`，实现 `src/concurrency/scheduler.cpp:4573` | 任意线程 | Waiting→Runnable 并入队 | bool |
| `wake_io(task)` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:1043` | IO 轮询器 | 只发布状态/token，不碰 Fiber 栈 | bool |
| `cancel(task)` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:1055` | 任意线程 | 协作取消 + 唤醒 | bool |
| `current_task()` / `current_scheduler()` | `include/go2cpp/concurrency/scheduler/scheduler.hpp:1181` / `:1186` | 任意代码 | 读 TLS 的当前 G / 调度器 | 指针 |

**Fiber**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `Fiber(fn, stack_size=128 KiB, reserve=0, growable=true)` | `include/go2cpp/concurrency/fiber.hpp:303-306` | 手动 Fiber 用户 | 建栈（growable 时 mmap + 顶部提交） | 对象 |
| `resume()` | `include/go2cpp/concurrency/fiber.hpp:347`，实现 `src/concurrency/fiber.cpp:2601` | 任意线程（同一时刻一个） | 换栈进入/恢复 | bool（claim 失败为 false） |
| `Suspend(reason)` | `include/go2cpp/concurrency/fiber.hpp:360`，实现 `src/concurrency/fiber.cpp:2629` | Fiber 内部 | 切回 resume 者 | bool |
| `Current()` | `include/go2cpp/concurrency/fiber.hpp:368` | 任意代码 | 当前线程正在跑的 Fiber | 指针 |
| `LockToCurrentM()` | `include/go2cpp/concurrency/fiber.hpp:418`，实现 `src/concurrency/scheduler.cpp:1005` | 需要线程亲和的 G | 把 G 绑定到当前 M | M id（0=失败） |
| `DefaultStackSize()` / `DefaultStackReserveSize()` | `include/go2cpp/concurrency/fiber.hpp:284` / `:291` | 构造函数默认值 | 128 KiB / 8 MiB | size_t |

**Channel / Select**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `MakeChannel<T>(cap=0)` | `include/go2cpp/concurrency/channel.hpp:3569` | 用户 | 建 Channel；cap=0 不分配缓冲 | `ChannelPtr<T>` |
| `Send(v)` / `SendFor` | `include/go2cpp/concurrency/channel.hpp:1548`（落点 `SendUntil` `:2740`） | 用户 | 直交等待接收者 → 入缓冲 → 否则登记 PendingSend 并 park | `ChannelSendResult` |
| `Recv()` / `RecvFor` | `include/go2cpp/concurrency/channel.hpp:1618`（落点 `RecvUntil` `:3114`） | 用户 | 直交 / 取缓冲 / 否则登记 PendingRecv 并 park | `ChannelRecvResult<T>` |
| `TrySend` / `TryRecv` | `include/go2cpp/concurrency/channel.hpp:1591` / `:1656` | 用户 | 非阻塞版本；失败返回 `kWouldBlock` | 同上 |
| `Close()` | `include/go2cpp/concurrency/channel.hpp:1685` | 用户 | 置 closed、唤醒全部等待者、缓冲值仍可读完 | `ChannelSendResult` |
| `Select(cases, ctx, timeout)` | `include/go2cpp/concurrency/channel.hpp:4295` | 用户 | 探测 → arm（全序锁）→ park → 单赢家提交 | `SelectResult` |
| `RecvCase` / `SendCase` / `DefaultCase` / `DoneCase` / `AfterCase` | `include/go2cpp/concurrency/channel.hpp:3850` / `:3932` / `:4038` / `:4171` / `:4246` | 用户 | 构造 SelectCase（`probe` 闭包） | `SelectCase` |

**同步原语 / Context / Future / Timer**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `Mutex::Lock` / `TryLock` / `Unlock` | `src/concurrency/sync.cpp:1572` / `:1928` / `:1994`（UnlockSlow） | 用户 / 上层原语 | 混合锁：CAS → 自旋 → FIFO 队列 park | bool / void |
| `ConditionVariable::Wait` / `NotifyOne` | `src/concurrency/sync.cpp:2327` / `:2619` | 用户 / RWMutex | 入队、解锁、park、重锁（D3 失败抛异常） | bool |
| `WaitGroup::Add` / `Done` / `Wait` | `src/concurrency/sync.cpp:2844` / `:2897` / `:2931` | 用户 | 计数 + 归零唤醒 | void / bool |
| `RWMutex::RLock` / `Lock` | `include/go2cpp/concurrency/rwmutex.hpp:147` / `:206` | 用户 | 写者优先的门 + 读者计数 | bool / void |
| `Context::Background` / `WithCancel` / `WithTimeout` | `include/go2cpp/concurrency/context.hpp:225` / `:230` / `:238` | 用户 | 建上下文树 | `ContextPtr` |
| `Context::Cancel` / `IsDone` / `Err` | `include/go2cpp/concurrency/context.hpp:303` / `:263` / `:266` | 用户 / 内部 | 取消整棵子树 | void / bool / `ErrorPtr` |
| `MakePromise<T>()` | `include/go2cpp/concurrency/future.hpp:1232` | 用户 | 建 Promise/Future 对 | pair |
| `Future::GetResult` / `GetResultFor` | `include/go2cpp/concurrency/future.hpp:1077` / `:1083` | 用户 | 等待完成（可带 Context/超时） | `FutureResult<T>` |
| `TimerService::Default()` | `include/go2cpp/concurrency/core/timer.hpp:379` | 任意代码 | 进程级单例（单后台线程） | `TimerService&` |
| `ScheduleAfter` / `ScheduleEvery` | `include/go2cpp/concurrency/core/timer.hpp:396` / `:406` | 用户 | 插入 multimap，到期回调 | `Timer` 句柄 |
| `SpinMutex::Lock` / `TryLock` / `Unlock` | `include/go2cpp/concurrency/spin_mutex.hpp:223` / `:237` / `:261` | 极短临界区 | CAS → 64 次 PAUSE → 门 | void / bool |

**默认值与编译开关**（`include/go2cpp/concurrency/scheduler/scheduler.hpp:240-308`）

| 配置 | 默认 | 说明 |
|---|---|---|
| `processor_count` | 0 → 硬件并发 | P 数 = 并行度 |
| `min_workers` / `max_workers` | 0 → P / 0 → 2P | 基础 M 常驻；补充 M 阻塞补位 |
| `local_queue_limit` | 256 | 只约束 admission 入队，不约束 incoming 条带 |
| `fiber_stack_size` / `reserve` | 2048 / 8 MiB | 页对齐后初始 4096；reserve 是 PROT_NONE VA |
| `fiber_stack_growable` | true | false/非 Linux/非原生后端 → 固定栈，不装 SIGSEGV handler |
| `task_affinity_budget` | 4 | 软亲和扫描预算 |
| `wake_policy` | `kRunNext` | `kLastMachine` / `kRunNext` / `kRoundRobin` |
| `collect_metrics` | **false** | 打开后每任务多两次取钟 + 跨核 RMW |
| `allow_idle_shrink` | false | true 才恢复空闲缩容 |
| `enable_sysmon` / 周期 / 阈值 | true / 10 ms / 50 ms | 只观察已声明的阻塞 |

编译开关：`GO2CPP_GROWABLE_STACK=0` 禁用栈增长；`GO2CPP_DEBUG_LOCKS` 强制 SpinMutex 的 debug 契约；`GO2CPP_COUNT_GLOBAL_LOCK` 统计 admission 锁；`GO2CPP_DEBUG_RESOURCES` 注册调试资源；`GO2CPP_FIBER_GROWABLE_STACK` 控制 handler 编译。

### 1.2 总架构图

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ 用户代码：go(fn) / ch->Send(v) / mu.Lock() / ctx->Cancel() / future.Get()    │
└───────────────────────────────┬──────────────────────────────────────────────┘
                                │ 公开 API（§1.1）
┌───────────────────────────────▼──────────────────────────────────────────────┐
│ 门面层                                                                        │
│  Scheduler ── Task(G) ── Fiber        Channel<T> ── SelectCase                │
│  sync::Mutex/RWMutex/Cond/WG          Context/Future        Timer             │
└───────┬───────────────────────┬───────────────────────┬──────────────────────┘
        │ 调度                    │ 数据面                 │ 等待/超时
┌───────▼───────────────────────▼───────────────────────▼──────────────────────┐
│ 核心层（本模块）                                                               │
│  ┌──────────── Scheduler::Impl ────────────┐  ┌──── 等待原语 ────┐            │
│  │ P[0..N-1]：本地 256 槽环 + runnext 单槽  │  │ ParkingCondition │            │
│  │ incoming[16] 条带（外部生产者）          │  │  └ WaitNode      │            │
│  │ global_queue + emergency 链              │  │     ├ per-node  │            │
│  │ registry[16] 条带（强持有非终态 G）      │  │     │   futex    │            │
│  │ M[]：基础 M（常驻）/ 补充 M（阻塞补位）  │  │     └ 队列锁     │            │
│  │ sysmon 线程（10ms 扫描，只 detach）      │  └──────────────────┘            │
│  └─────────────────────────────────────────┘                                 │
│  Fiber：mmap 8MiB VA + 顶部 4KiB 提交 + SIGSEGV 增长 + TLS FiberBin(128)      │
│  Channel：RingBuffer(head/tail/count) + 自旋锁 + 侵入式 sendq/recvq            │
└───────┬───────────────────────┬───────────────────────┬──────────────────────┘
        │ 线程边界               │ 系统调用               │ 信号
┌───────▼───────────────────────▼───────────────────────▼──────────────────────┐
│ 运行时/内核                                                                    │
│  std::thread（M）  futex（WaitNode/FutexMutex）  epoll（idle poller）          │
│  mmap/mprotect（Fiber 栈）  SIGSEGV handler（栈增长）  clock_gettime           │
└──────────────────────────────────────────────────────────────────────────────┘

跨层数据流（一次 park→wake）：
  G 在 M 上执行
    → 调用 Channel/Send 等：登记等待节点            （include/go2cpp/concurrency/channel.hpp:2740 起）
    → Task::park_for_scheduler：状态字 CAS 成 Waiting（src/concurrency/scheduler.cpp:1424）
    → Fiber::SuspendForScheduler：换栈回 worker      （src/concurrency/fiber.cpp:2629）
  worker 继续取下一个 G（src/concurrency/scheduler.cpp:5548 循环）
    → 唤醒方（另一 G/线程）：signal CAS + 锁外 notify（include/go2cpp/concurrency/channel.hpp:2757 起）
    → Task::wake_for_wait_impl：Waiting→Runnable     （src/concurrency/scheduler.cpp:1353）
    → 入目标 P 的 runnext 或本地环                   （src/concurrency/scheduler.cpp:4186 起）
    → 某个 M 取到、claim、Task::run → Fiber::resume  （src/concurrency/scheduler.cpp:6045）
    → 回到 park 点继续执行
```

**三个边界要记牢**：

1. **Fiber 栈边界**：G 挂起时栈还在；`resume` 只换 rsp，不拷贝栈（`src/concurrency/fiber.cpp:885-893`）。
2. **M 线程边界**：G 可以在任意 M 上恢复；TLS（`t_scheduler/t_machine_id/t_processor_id/t_task`）在 worker_loop 入口设置（`src/concurrency/scheduler.cpp:5466-5471`）。
3. **内核边界**：只有普通线程/`*_f` 会真阻塞；受管 G 的等待走 park，不占 M（`include/go2cpp/concurrency/core/parking_condition.hpp:14-22`）。

### 1.3 复杂场景 ≥10 步详解

#### 场景 A：一次任务提交 → 执行 → park → wake → 完成（14 步）

1. **用户调用** `go(fn)` → `Scheduler::spawn`（`include/go2cpp/concurrency/scheduler/scheduler.hpp:981`，实现 `src/concurrency/scheduler.cpp:3750`）。此时只构造 Task，**不建栈**（`src/concurrency/scheduler.cpp:1741-1745` 才建）。
2. **入队**：`spawn` 末尾 `enqueue(task)`（`src/concurrency/scheduler.cpp:3768`）；失败则 `task->cancel()`，返回一个终态句柄，而不是永不运行的句柄。
3. **外部生产者路径**：`enqueue`（`src/concurrency/scheduler.cpp:3800`）按 16 条带选一条 incoming 队列；条带锁是叶子锁，持锁期间**禁止**再调 `enqueue`（`src/concurrency/scheduler.cpp:1970-1977`）。
4. **worker 取任务**：`worker_loop`（`src/concurrency/scheduler.cpp:5461`）每轮按 step0 inbox → runnext（`:5558-5577`）→ step2 本地环（`:5602-5637`）→ step3 incoming（`:5641-5763`）→ step4 admission（`:5767-5838`）找活。
5. **认领**：`claim_from_queue`（`src/concurrency/scheduler.cpp:1489`）用一次 CAS 把 Runnable→Running，同时清 `m_queued`、置 `m_execution_claim`。
6. **执行**：`t_task->run()`（`src/concurrency/scheduler.cpp:6045`）→ `Task::run`（`:1712`）→ 首次创建/复用 Fiber（`:1745-1759`）→ `Fiber::resume`。
7. **用户函数运行**：G 在 M 上跑；若调用 `ch->Send` 且无接收者，进入 `Channel::SendUntil`（`include/go2cpp/concurrency/channel.hpp:2740`），登记 `PendingSend`（`:2312`）并 park。
8. **park 提交**：`Task::park_for_scheduler`（`src/concurrency/scheduler.cpp:1424`）先做契约检查（SpinMutex 临界区禁止、持普通 Mutex 告警，`:1429-1437`），再把状态字 CAS 成 Waiting；`Fiber::SuspendForScheduler`（`src/concurrency/fiber.cpp:2629`）切回 worker。
9. **worker 记账**：`run()` 返回后处理状态（`src/concurrency/scheduler.cpp:6046-6076`），把 G 的 `kWaiting` 记成 `kTaskSuspended` 事件；worker 继续下一轮。
10. **接收者出现**：另一个 G 调 `Recv`，在 Channel 锁内摘 `sendq` 队首，写入值并 CAS `signal`（`include/go2cpp/concurrency/channel.hpp:2757-2769`），锁外 `NotifyChange`（`:2727-2738`）。
11. **唤醒**：`wake_for_wait_impl`（`src/concurrency/scheduler.cpp:1353`）把 Waiting→Runnable；执行权仍被占时只置 `deferred_enqueue`（`:1366-1369`），等 `run()` 退出后再发布。
12. **重新入队**：目标 P 有运行中的 M 就投 runnext（`src/concurrency/scheduler.cpp:4186-4198`），否则投本地环（任意 M 可窃取）；`publish_runnext` 被挤出者落回环尾（`:4005-4035`）。
13. **再次执行**：另一个（或同一个）M 取到、claim、`Task::run` → `Fiber::resume` 回到第 8 步的 park 点；`SendUntil` 的等待循环重检 `accepted`（`include/go2cpp/concurrency/channel.hpp:3030` 起）。
14. **完成**：用户函数返回 → Fiber trampoline 回收 → `Task::run` 进入终态（`src/concurrency/scheduler.cpp:1921-1944`）→ `notify_terminal` + `Fiber::RecycleForScheduler`；注册表按节拍回收（`:6187-6200`）。

失败/阻塞分支：
- 第 3 步入队失败 → 第 2 步已 `cancel()`，句柄终态 `kCancelled`；
- 第 5 步 claim 失败 → `recover_unclaimed`（`src/concurrency/scheduler.cpp:5507-5522`）判定是否需要补发布；
- 第 8 步 Fiber 拒绝挂起 → `resume_after_failed_park`（`:1618` 附近）回滚为 Running，调用方重试或返回；
- 第 14 步 `run()` 返回后仍是 Running → `recover_orphan_running`（`src/concurrency/scheduler.cpp:6153`）自愈重发布。

#### 场景 B：无缓冲 Channel 的 Send/Recv + 超时 + 取消（15 步）

1. **接收者先到**：G2 调 `Recv`（`include/go2cpp/concurrency/channel.hpp:1618`）→ `RecvUntil`（`:3114`）；无值、无发送者 → 创建 `PendingRecv`（`:2340`）挂到 `m_receivers`。
2. **接收者 park**：`ParkingCondition::wait` 或 Scheduler `park_wait`（`src/concurrency/scheduler.cpp:4426`）→ `park_for_scheduler`（`:1424`）→ 状态 Waiting。
3. **发送者到达**：G1 调 `Send`（`include/go2cpp/concurrency/channel.hpp:1548`）→ `SendUntil`（`:2740`）；锁内看到 `m_receivers` 非空。
4. **直交**：摘队首、跳过已取消节点（`:2759-2769`），把值 `emplace` 进节点，CAS `signal` 0→kReady；值只在交接成功后移动（`:2729-2730`）。
5. **锁外通知**：释放 Channel 锁后再 `NotifyChange`/唤醒，避免锁序反转（`:2716`、`:2727-2728`）。
6. **接收者被唤醒**：等待循环重检 `signal`，读到 kReady，取走值并返回 `kReady`。
7. **超时路径**：若 G2 用 `RecvFor(1ms)`，`RecvUntil` 带 deadline（`include/go2cpp/concurrency/channel.hpp:3114`）；到期后 `TryFinish(kTimedOut)`，等待循环返回 `kTimedOut` 并摘除节点。
8. **取消路径**：Context 取消回调只持 `weak_ptr<CancelHub>` + token，在 hub/Channel 锁内按 token 定位节点、CAS signal 后唤醒（`include/go2cpp/concurrency/channel.hpp:778-782`）；回调从不直接解引用队列外节点。
9. **Close 并发**：G3 `Close`（`include/go2cpp/concurrency/channel.hpp:1685`）先置 `m_closed`，再逐个唤醒等待者；对接收者**先交付缓冲值**，缓冲空才给 `kClosed`（`:1725-1785`）。
10. **发送者遇到关闭**：`SendUntil` 在 step2 看到 `m_closed` 直接返回 `kClosed`（`:2753-2755`）；重复 `Close` 返回 `kAlreadyClosed`（`:1689-1691`）。
11. **僵尸节点清理**：取消/超时节点不从队列中间摘；交接方 `pop_front` 时跳过并回收（`:2759-2769`）。
12. **Select 变体**：如果 G2 用 `Select`，先按随机起点探测（`src/concurrency/channel.cpp:446-501`）：`kReady` 立即返回，closed/nil 暂存；没有 ready 才 arm。
13. **arm**：按 `SelectLockKey`（对象地址）全序一次性持有整组 Channel 锁（`include/go2cpp/concurrency/channel.hpp:1894-1896`、`:3746`），锁内重新验证并登记等待节点。
14. **单赢家**：`SelectWaitState` 的 `TrySelect/TrySelectPair`（`include/go2cpp/concurrency/channel.hpp:651` 起）保证取消/超时/交接只有一个能提交结果。
15. **关闭后的排空**：接收者循环 `Recv` 直到 `kClosed`；缓冲值先读完（`:53-54`、`:1725-1785`），不会因为 closed 丢掉已入队的值。

失败/阻塞分支：
- 第 4 步值构造抛异常 → 保留接收/发送节点，后续可重试（`include/go2cpp/concurrency/channel.hpp:2729-2730`）；
- 第 6 步等待者被错误唤醒 → `signal` 已是终态，循环重检后返回，不会重复提交；
- 第 9 步关闭与发送并发 → 发送方在锁内看到 closed，返回 `kClosed`，不会写入已关闭通道。

#### 场景 C：`shutdown()`（12 步）

1. **入口**：`shutdown()`（`src/concurrency/scheduler.cpp:3616`）→ `shutdown_for(max)`（`:3402`）；worker 自己调用时只发布请求，不 join 自己（`:3421-3430`）。
2. **关 admission**：持 `Impl::mutex`，`set_accepting(false)`、`set_draining(true)`（`:3440-3449`）。
3. **排空 P**：`begin_shutdown_locked`（`:2753`）对每个 P：取 runnext（`:2764`）+ 逐个 `queue.pop()`（`:2767`）。
4. **区分已启动/未启动**：`started()` 的 G 只 `request_cancel` 并放回可运行（`:2768-2770`）；未启动的 G 原地取消、清队列、进延迟析构（`:2771-2776`）。
5. **排空 global/inbox/incoming**：global（`:2789-2803`）、每台 M 的 inbox（`:2807-2825`）、16 条 incoming 条带（`:2830-2858`）按同样规则处理。
6. **唤醒 parked G**：扫注册表 16 条带（`:2870-2897`），对非终态 G `request_cancel`；已启动的用 `wake_for_scheduler` 放回可运行。
7. **等终态**：`shutdown_for` 用 `live_tasks_total()==0` 的 O(1) 谓词等待（`:3503-3513`）；有界超时返回 false，保持 draining 以便下次继续（`:3515-3527`）。
8. **停 sysmon**：终态归零后 `set_stopping(true)`、`stop_sysmon(true)`（`:3529-3532`）；draining 期间 sysmon 仍在跑，保证长 syscall 返回前替代 M 不丢。
9. **等 worker 退出**：有界路径先等 `active_workers==0`（`:3538-3555`）；无界路径直接进入 join。
10. **join**：持 `Impl::mutex` 逐个摘出 `std::thread`，释放锁后 `join`（`:3564-3582`）；worker 尾声会先拿 `Impl::mutex`，所以必须先放锁。
11. **收尾**：把 P 置 Dead、确认没有 joinable 线程、清 `started`（`:3584-3601`）。
12. **锁外析构**：`deferred_destruction` 里的用户捕获在锁外释放（`:3606-3613`），避免在调度器锁内跑任意 C++ 析构。

失败/阻塞分支：
- 有界 timeout：返回 false，draining 保持、取消请求保持，后续 `shutdown()` 可继续；
- 未启动调度器：走 `!was_started` 分支（`:3463-3486`），直接停 sysmon、置 Dead，不 join；
- 长 syscall 的 M：draining 期间 sysmon 仍可能 detach 并补 M，所以不能提前停 sysmon。

### 1.4 关键不变量（破了就一定出 bug）

| # | 不变量 | 依据 | 破了会怎样 |
|---|---|---|---|
| I1 | 一个 G 同时最多一个执行者：`execution_claim` 与 `run_claim` 必须在同一临界区一起翻转 | `src/concurrency/scheduler.cpp:1913-1919`、`:1712-1731` | 两个 M 同时进同一个栈 → 栈被踩烂；或 G 永久 `kRunning`，shutdown 永不返回 |
| I2 | 一个可运行 G 最多一个队列节点：`m_queued` 是入队权 | `src/concurrency/scheduler.cpp:1489-1513`、`:5507-5522` | 重复入队 → 同一 G 被两个 M 取到；丢节点 → 「Runnable 但无人调度」的丢唤醒 |
| I3 | G 状态与唤醒令牌在同一个 32 位字里（低 8 位状态、bit8 令牌） | `src/concurrency/scheduler.cpp:1424-1468`、`:1353-1387` | 拆成两个原子会留下「令牌已发布、挂起已提交、却没人消费」的窗口 |
| I4 | 注册表强持有所有非终态 G（含已 park、不在任何队列里的） | `src/concurrency/scheduler.cpp:1956`、`:2708-2719` | 挂起栈被提前析构 → resume 时踩已释放内存 |
| I5 | `incoming` 里的 G 一定没有 P（`processor == kInvalidPId`） | `src/concurrency/scheduler.cpp:1984` | 跨 P 误认领 → 同一 G 被两个 P 的 worker 抢 |
| I6 | 锁层次严格为 admission → 条带 → P → Task；incoming 条带锁是叶子锁 | `src/concurrency/scheduler.cpp:1960-1977` | 反向嵌套 → ABBA 死锁（SCHED-01/02 的真实来源） |
| I7 | Channel 等待节点由等待者栈帧持有，队列只持裸指针；取消回调不解引用队列外节点 | `include/go2cpp/concurrency/channel.hpp:768-782` | UAF；或取消路径把节点删了、交接方还在用 |
| I8 | WaitNode 内部锁必须无 owner、可跨线程解锁、且临界区禁 park | `src/concurrency/sync.cpp:309-320`、`docs/audit/waitnode-lock-design.md` | 用 `std::mutex` 跨线程解锁是 UB；持锁 park 会自死锁 |
| I9 | shutdown 期间已启动 G 必须保持可运行，不能强制释放其栈 | `src/concurrency/scheduler.cpp:2756-2759`、`:3503-3513` | 跳过栈上 RAII、泄漏锁；或 Fiber 永远不返回 |
| I10 | Timer 回调里不能销毁 `TimerService` | `src/concurrency/timer.cpp:276-278` | 后台线程 join 自身 → `std::terminate()` |

---

## 第二部分 · 分：逐块细讲

### 2.1 Fiber

**职责**：G 的执行载体。提供独立栈、可挂起/恢复的上下文、Fiber 局部存储、以及按 M 复用的 FiberBin。

**关键数据结构**

| 结构 | 字段 | 行号 | 说明 |
|---|---|---|---|
| `Fiber::Impl` | `m_switch` | `src/concurrency/fiber.cpp:2442` | 打包状态 + claim 位 + 传播位，独占缓存行 |
| | `m_context` / `m_caller` | `src/concurrency/fiber.cpp:2449-2451` | 目标栈上下文 / 切回点 |
| | `m_stack` / `m_stack_size` / `m_stack_growable` | `src/concurrency/fiber.cpp:2478-2481` | 独立栈；规范化后的初始提交量；是否真增长 |
| | `m_record` | `src/concurrency/fiber.cpp:2469` | 与观察者共享的元数据（父链/时间戳） |
| `FiberStack` | `m_initial_usable` / `m_reserve` / `m_committed` | `src/concurrency/fiber.cpp:914-935` | 顶部已提交窗口；8 MiB 预留；committed 向下移动 |
| `FiberBin` | `kSlots=128`，默认 32 / 调度器 128 / 上限 4096 | `src/concurrency/fiber.cpp:282`、`include/go2cpp/concurrency/fiber.hpp:317` | 每 M 复用终态 Fiber；复用时 `use_count()!=1` 就丢弃（`src/concurrency/fiber.cpp:2559`） |

**`resume()` 逐步拆**

1. 读 `m_switch`，检查是否已有执行者（claim 位）；有则返回 false（`src/concurrency/fiber.cpp:1952-1965`）。
2. 首次 resume：`initialize_context` 绑定栈入口与父链；之后只更新 caller（`src/concurrency/fiber.cpp:2088-2103`）。
3. `switch_to_fiber`（`src/concurrency/fiber.cpp:2214-2260`）：保存 6 个 callee-saved + 返回地址、换 rsp、跳转（汇编在 `src/concurrency/context_x64.S:54-73`）。
4. Fiber 端 `entry`（`src/concurrency/fiber.cpp:1907-1944`）：执行用户函数；异常写入 `m_failure`；结束发布 Completed/Failed。
5. `Suspend`（`src/concurrency/fiber.cpp:2629`）切回 caller；`resume` 返回后释放 claim 位（`src/concurrency/fiber.cpp:2159-2167`）。

**状态机**（`include/go2cpp/concurrency/fiber.hpp:164-170`）：`Ready → Running → Suspended → Running → Completed/Failed`。`Suspended` 只在挂起期间可见；`Completed/Failed` 才允许进 FiberBin。

**失败路径与限制**

- 并发 `resume` 的第二个调用直接返回 false，不会破坏栈（`src/concurrency/fiber.cpp:1961-1965`）；
- 跨线程恢复会更新 `m_last_resume_thread_hash` 并触发诊断（`src/concurrency/fiber.cpp:2236`），但**不禁止**——调度器本来就要迁移 G；
- 固定栈路径不安装 SIGSEGV handler、不分配 sigaltstack（`src/concurrency/fiber.cpp:1053-1056`），爆栈 = 进程被杀；
- handler 只处理「fault 在未提交区 + rsp 在预留区 + 距离 ≤256 KiB」（`src/concurrency/fiber.cpp:1290-1308`），其余恢复默认动作保留 core。

**典型误用**：把 `FiberLocal` 当 `thread_local`（G 迁移后读到别的 G 的值，`src/concurrency/fiber_local.cpp:7-17`）；在手动 Fiber 里用 `Select`（返回 `kInvalid`，因为无法 park）。

### 2.2 Scheduler

**职责**：把 Task 包成 G，交给 P 排队、M 执行；负责 park/wake、work stealing、sysmon、shutdown、观测。

**关键数据结构**

| 结构 | 关键字段 | 行号 | 说明 |
|---|---|---|---|
| `Task`（G） | `m_state_word` / `m_execution_claim` / `m_run_claim` / `m_queued` | `src/concurrency/scheduler.cpp:1472-1513`、`:1913-1919` | 状态与令牌同字；两个 claim 描述「谁在执行」 |
| `Processor`（P） | `queue`（256 槽环）、`runnext`、`active_machines`、`running_machines` | `src/concurrency/scheduler.cpp:2019-2040` | 本地队列 + 快速槽 + 活动 M 计数 |
| `Machine`（M） | `processor`、`replacement`、`inbox`、`blocking_since_ns` | `src/concurrency/scheduler.cpp:2042-2101` | 基础 M / 补充 M；定向槽；阻塞记账 |
| `LocalRunQueue` | `m_cells[256]`、`m_head`、`m_tail` | `src/concurrency/scheduler.cpp:410-574` | Vyukov 有界 MPMC 环；满时返回 false |
| `RunNextSlot` | `m_task`、`m_mutex` | `src/concurrency/scheduler.cpp:599-646` | 单槽 LIFO；被挤出者回环尾 |

**`worker_loop` 逐步拆**

1. 入口设置 TLS：`t_scheduler/t_machine_id/t_processor_id`（`src/concurrency/scheduler.cpp:5466-5471`）；可选绑核（`:5475-5485`）。
2. 取活：inbox → runnext（`:5558-5577`）→ 本地环（`:5602-5637`）→ incoming 16 条带（`:5641-5763`）→ admission（`:5767-5838`）。
3. claim：`claim_from_queue` 单 CAS（`:1489-1513`）；失败走 `recover_unclaimed`（`:5507-5522`）。
4. 执行：`t_task->run()`（`:6045`）；`Task::run` 内建栈/复用 Fiber 并 resume（`:1712-1810`）。
5. 终态/挂起：按 `run()` 返回后的状态记账（`:6046-6076`）；yield 回队（`:6120-6146`）；orphan 自愈（`:6153`）。
6. 维护：每 64 拍 `maybe_grow`（`:6007-6021`）；按节拍回收终态 G（`:6187-6200`）。
7. 无活：IO idle poller（`:6277-6330`）→ 有界自旋（`:6343-6350`）→ 条件变量（`:6360-6476`）。

**状态机**

```
G: New → Runnable → Running → {Dead, Cancelled, Failed}
                    Running → Waiting → Runnable
M: Idle → Running → {Blocking, Parked, Stopping} → Dead
P: Idle → Running → Stopping → Dead
```

**失败路径与限制**

- **无异步抢占**：CPU 密集 G 只能靠 yield/park 让出；sysmon 只处理已声明的阻塞（`src/concurrency/scheduler.cpp:2498-2556`）；
- **本地队列边界不能动**：把窃取移出全局锁实测更慢（`src/concurrency/scheduler.cpp:2009-2010`）；
- **8 MiB VA/G**：10 万 G 约 819 GB VA，默认 `max_map_count` 下约 3 万 G 触顶；
- **draining 判零必须精确**：用 `live_tasks` 而非分片近似（`:6170-6186`）。

**典型误用**：在 `SpinMutex` 临界区里 park（debug abort / release 降级，`src/concurrency/scheduler.cpp:1429-1437`）；在持普通 Mutex 时做长阻塞（只告警不禁止，但会把 M 占住）。

### 2.3 Channel

**职责**：Go 风格带缓冲/无缓冲通道，外加 Select 的多路等待；阻塞时 park G，不占 M。

**关键数据结构**（`include/go2cpp/concurrency/channel.hpp:3534-3563`）

| 字段 | 行号 | 说明 |
|---|---|---|
| `m_capacity` | `:3534` | 0 = rendezvous，不分配缓冲 |
| `m_lock`（ChannelSpinLock） | `:3541` | 保护 buffer/senders/receivers/closed；**绝不允许持锁 park** |
| `m_buffer`（RingBuffer） | `:3547` | head/tail/count 环形数组；`cap==0` 不分配（`:1290-1300`） |
| `m_senders` / `m_receivers` | `:3549-3552` | 侵入式等待队列，节点由等待者栈帧持有 |
| `m_closed` | `:3555` | 关闭标记；关闭后 Send 返回 kClosed |
| `m_generation` / `m_change_waiters` | `:3557-3561` | 状态代际；为 0 时快路径不写，避免跨核 cacheline 迁移 |

**`SendUntil` 逐步拆**（`:2740` 起）

1. 上下文已取消/超时 → 直接返回，不进锁（`:2743-2747`）。
2. 持 Channel 自旋锁；`m_closed` → `kClosed`（`:2750-2755`）。
3. 优先直交 `m_receivers` 队首：跳过已取消节点；普通节点 `emplace` + `notify_one`；select 节点走 `TrySelect`（`:2757-2785`）。
4. 无接收者但有缓冲空位 → 入缓冲（`:2831` 附近）。
5. 都失败 → 检查 deadline/Fiber 支持；创建 `PendingSend`（`:2312`）入 `m_senders`，把 `context->Done()` 回调注册成「唤醒这个 pending」。
6. 等待循环：每次醒来重检 `accepted/closed/cancelled/deadline`，任一成立就清理并返回（`:3030` 起）。

**状态机**：`kReady`（成功）/`kWouldBlock`（Try* 非阻塞）/`kClosed`/`kAlreadyClosed`/`kCancelled`/`kTimedOut`/`kNil`/`kInvalid`（`include/go2cpp/concurrency/channel.hpp:117-127`）。

**失败路径与限制**

- **D1 丢值**：旧 Select「第一个 ready 就返回」会让 closed 遮蔽同批另一个通道的缓冲值；现在 `kReady` 立即返回、closed 延后（`src/concurrency/channel.cpp:446-501`）；
- **D2 错唤醒**：共享 futex 字会唤醒错节点；现在每节点自己的 32 位字（`include/go2cpp/concurrency/channel.hpp:778-782`）；
- **ABI 约束**：不能给 `SelectCase` 加字段；公共结构体尺寸变化会让旧库+新头在增量构建切换点 SEGFAULT（`src/concurrency/channel.cpp:457-461` 注释）；
- **Close 与 Send 并发**：Send 在锁内看到 closed 返回 `kClosed`，不会写入已关闭通道。

**典型误用**：把 nil channel 当 Go 的永久阻塞（本项目返回 `kNil`）；丢弃 `Close()` 的返回值；在 Channel 锁内做用户回调。

### 2.4 Mutex

**职责**：Go 风格不可重入互斥锁；受管 G 竞争时 park，普通线程走自旋 + futex；带 1 ms 饥饿保护。

**关键数据结构**

| 字段 | 行号 | 说明 |
|---|---|---|
| 状态位 `kLocked/kWaiter/kStarving/kWoken` | `src/concurrency/sync.cpp:1303-1306` | 同一个原子字节；内联快路径与慢路径同源 |
| `m_state` | 内联在 `Mutex` 对象（独立 cacheline） | Impl 只保存指向它的指针（`src/concurrency/sync.cpp:1299-1301`） |
| `m_waiters` | `src/concurrency/sync.cpp:1321-1321` | FIFO，存 `shared_ptr<WaitNode>`；先出队、锁外 Wake |

**`LockImpl` 逐步拆**（`src/concurrency/sync.cpp:1572`）

1. 上下文已失效 → 返回 false（`:1574-1577`）。
2. 快路径：状态无 `kLocked/kStarving` 时一次 CAS 拿锁（`:1554`）。
3. 判定等待者类型：受管 G / 普通线程 / 手动 Fiber；手动 Fiber 无法 park，直接返回 false。
4. 普通线程先短自旋（8 次），再进等待队列；受管 G 直接 park（不自旋）。
5. 在 `m_mutex` 下发布 `kWaiter`（必要时 `kStarving`）并 push（`:1726-1742`）。
6. 等待结束：普通唤醒回顶部重新 CAS；饥饿交接直接持锁返回；超时/取消从队列摘除且不持锁返回。

**状态机**：正常模式（释放后唤醒、允许插队）↔ 饥饿模式（等待 >1 ms 触发；不释放 `kLocked`，直接交给 FIFO 队首；队列空或被交接者未超阈值时退出）。

**失败路径与限制**

- **状态字必须原子 RMW**：`load+store` 曾抹掉并发 `TryLock` 的 `kLocked`，造成双持 + 解锁异常（`src/concurrency/sync.cpp:1276-1279`、`:1347`）；
- 正常模式不保证「唤醒即持有」，只保证 1 ms 饥饿上界；
- 队列存 `shared_ptr`，否则「出队→Wake」窗口内等待方销毁栈节点会 UAF（`src/concurrency/sync.cpp:1283`）。

**典型误用**：假设锁有 owner（Go 风格允许 A 加锁 B 解锁，`src/concurrency/sync.cpp:1539-1549` 的两态描述不包含 owner）；持锁 park（只告警，但会把 M 占住）。

### 2.5 RWMutex

**职责**：写者优先的读写锁；读者快路径一次 `fetch_add`，写者慢路径用内部 `sync::Mutex` 做 FIFO。

**关键数据结构**（`include/go2cpp/concurrency/rwmutex.hpp`）

| 字段 | 行号 | 说明 |
|---|---|---|
| `m_state` | `:789` | 读者计数 + 写者门 + 慢路径标记的合并原子字，独占缓存行 |
| `m_pending` | `:794` | 正在慢路径等待的写者数；>0 时读者不再走快路径 |
| `m_writer_mutex` / `m_park_mutex` | `:796-800` | 写者 FIFO / 慢路径 park 的互斥量 |
| `m_reader_cv` / `m_writer_cv` | `:802-804` | 读者等门开；写者等上一个写者开门 / 读者归零 |
| `m_waiting_readers` / `m_gate_waiters` / `m_drain_waiters` | `:806-808` | 唤醒短路计数 |

**关键函数**

- `RLock`（`:147`）：一次 `fetch_add`，结果落在 `[0, kGate)` 且 `m_pending==0` 就进入；否则走慢路径。
- `Lock`（`:211`）：先宣告「有写者在等」（读者快路径看到就不再进门），再单 CAS 关门；慢路径进 `m_writer_mutex` 排队、等读者归零。
- `Unlock`（`:235`）：一次性还原写者位 + 慢路径位 + 门偏移；写者持锁期间读者不可能登记。

**状态机**：门开（N 个读者）↔ 门关（最多 1 个写者 + R 个未退出读者）。写者优先：一旦 `m_pending>0`，新读者必须排队。

**失败路径与限制**

- 读者可能被写者压制，这是有意取舍（`:15-20`）；
- 旧实现写者 park 与 `NotifyOne` 之间丢唤醒，8 线程 50/50 混合负载卡死（`:66-69`）；
- **不提供升级**：读锁持有者申请写锁必然自己等自己（`:71`）；写锁不可重入；读锁重入在有写者排队时同样自死锁（`:76`）；
- `TryLock` 在「锁空闲但有写者排队」时返回 false，以保护写者 FIFO（`:113`）。

**典型误用**：写锁临界区里做 IO（写者队列整体停摆）；用 `RLock` 递归保护同一容器（有写者排队时自死锁）。

### 2.6 ConditionVariable

**职责**：条件等待；本身无状态，等待队列与快路径槽都在 `Impl`；受管 G park、普通线程走 futex。

**关键数据结构**（`src/concurrency/sync.cpp:2245-2280`）

| 字段 | 说明 |
|---|---|
| `m_single` / `m_fast_waiter` | 1P1C 快路径的「唯一等待者」原子槽 |
| `m_waiters` | 通用路径的 FIFO 队列，存 `shared_ptr<WaitNode>` |
| `m_change_mutex` / 条件 | 冷路径等待互斥量（若实现含） |

**`Wait` 逐步拆**（`src/concurrency/sync.cpp:2327`）

1. 识别手动 Fiber：即使 Context 已取消，也不能走普通线程 unlock/relock 路径（`:2328-2345`）。
2. 取/复用 `WaitNode`（线程本地缓存，`AcquireWaitNode`，`:2381`）。
3. **先入队（或占快路径槽）再解锁**：入队与清槽必须在同一临界区（`:2451`），否则通知落在入队前就丢。
4. 解锁后 park；被唤醒后重新加锁。
5. 重锁失败（调度器关闭）→ 先 8192 次无 park 抢回，抢不回抛 `runtime_error`（`:2193`、`:2359`）。

**`NotifyOne`**（`:2619`）：锁内摘节点/清槽，锁外唤醒；裸 `store` 会覆盖新等待者刚发布的槽，导致唤醒永久丢失（`:2628`）。

**失败路径与限制**

- **D3**：`Wait` 返回时调用方假定仍持锁；重锁失败不能返回 false，否则上层会在未持锁状态下改容器；
- `notify-before-park` 窗口只能靠「入队/清槽同临界区 + 谓词复检」闭合；
- 节点池化：每次 `make_shared` 一个含原子/互斥/条件变量的控制块约 40–60 ns（源码注释），比一次交接还贵。

**状态机**：CV 对象本身无状态；每个等待节点走 `Waiting → Notified | TimedOut | Cancelled`，由节点结果的 CAS 选唯一赢家。

**典型误用**：不持外部锁调用 `wait`（契约要求调用方持有传入锁）；把通知方当成「转移锁所有权」（Cond 不做 handoff，锁由等待者重锁）。

### 2.7 WaitGroup

**职责**：计数 + 归零唤醒；`Add/Done` 是热路径，用单次 RMW 而非 CAS 循环。

**关键数据结构**（`src/concurrency/sync.cpp:2741-2770`）：计数原子 + `m_waiters` 队列（`shared_ptr<WaitNode>`）。

**关键函数**

- `Add(n)`（`:2844`）：单次 `fetch_add` 更新计数并判零；负数/溢出抛 `logic_error`（`:2860-2870`）。
- `Done()`（`:2897`）：`fetch_sub`；归零时整波 `TryFinish` + swap，锁外唤醒。
- `Wait(ctx)`（`:2931`）：先自旋预算（`:2953`），超出后睡 futex；取消/超时返回 false。

**复用语义**：计数归零后可开始新一轮（`:2727-2740`），前提是上一轮等待者已全部返回；`Add` 与 `Wait` 并发是调用方错误（`docs/modules/sync.md:210`）。

**状态机**：计数 `0 → N → 0`；归零瞬间把当前等待者整波置 `Notified`；新一轮从 0 开始，前提是上一轮等待者已全部返回。

**典型误用**：在 `Wait` 之后 `Add`（Wait 可能已返回）；把 WaitGroup 当一次性屏障反复复用而不等上一轮收尾。

### 2.8 Select

**职责**：把 N 个 channel 的等待折叠成一次「探测 + 一个等待节点」，保证单赢家与有值优先。

**关键数据结构**

| 结构 | 行号 | 说明 |
|---|---|---|
| `SelectCase` / `SelectProbe` | `channel.hpp`（Select 段，`3850` 起构造） | case 的 probe 闭包与结果 |
| `SelectWaitState` | `:651` | 等待路径的单赢家状态 |
| `SelectLockKey` | `:1894-1896` | 用对象地址做全序键；arm 按它排序加锁 |

**执行流程**

1. **探测**（`src/concurrency/channel.cpp:446-501`）：线程本地随机起点轮转；`probe()` 是真实传输。
2. `kReady` 立即返回（传输已发生，不可回滚）；closed/nil/invalid 无副作用，暂存到扫描结束（`:479-490`）。
3. 没有 ready → 检查取消/default/超时。
4. **arm**：按 `SelectLockKey` 全序一次性持有整组 Channel 锁（`include/go2cpp/concurrency/channel.hpp:3746`），锁内重新验证并登记节点。
5. **单赢家**：任一 channel 就绪只会有一个 case 提交结果（`SelectWaitState`）。

**失败路径与限制**

- 不给 `SelectCase` 加 `peek` 字段：公共结构体尺寸变化 → 旧 .so + 新头 SEGFAULT（`docs/modules/select-semantics.md:94-104` 记录的历史事故）；
- 外部再用 `std::mutex` 保护多通道，锁序不确定可死锁（`docs/modules/concurrency-usage.md:387`）；
- 「有值 vs 可发送」不排序：两者都立即传输，谁先探测到谁赢。

**典型误用**：以为 closed 一定优先（本项目是有值优先）；在 select 的 case 里做阻塞操作。

### 2.9 Future

**职责**：一次性的异步结果；Promise 写、Future 读；等待可带 Context/超时。

**关键数据结构**

| 结构 | 行号 | 说明 |
|---|---|---|
| `FutureStatus` | `include/go2cpp/concurrency/future.hpp:168` | Ready/Error/Cancelled 等结果分类 |
| `FutureStateKind` | `:213` | 状态机种类 |
| `FutureState<T,false>` / `<T,true>` | `:340` / `:598` | 可拷贝 / move-only 两套实现 |
| `FutureWaiterCount` | `:271` | 等待者计数（唤醒短路） |

**关键函数**

- `MakePromise<T>()`（`:1232`）：建 pair。
- `Promise::SetValue`（`:965`）：写结果 + 唤醒；重复设置返回 false。
- `Future::GetResult` / `GetResultFor`（`:1077` / `:1083`）：等待完成；带 Context/超时。
- **先注册取消回调再取状态锁**（`:425`）：`AddCallback` 在已取消时会同步执行回调，先取锁会自锁。
- **每个等待者独立取消标记**（`:258`）：共享会让一个超时误唤醒其他等待者。
- **Promise 析构发布 broken-promise**（`:26`）：消费者不会永久等待。

**状态机**：`Pending → Ready | Error | Broken`（终态不可逆；重复 set 返回 false）。

**典型误用**：把 `GetResult` 的返回值当异常（只有 `GetOrThrow` 才抛）；多个线程共享一个取消标记。

### 2.10 Context

**职责**：取消树、deadline、值传递；取消传播两阶段（先标记整棵子树、再执行回调），外加局部回滚。

**关键数据结构**（`src/concurrency/context.cpp:928-985`、`:220`）

| 字段 | 说明 |
|---|---|
| `error` / `cause` | 取消原因；非空即已取消（幂等） |
| `children` | 子节点集合；取消时收集并清空 |
| `before_done_callbacks` | 内部/用户回调；先搬出、锁外执行 |
| `timer_id` | deadline 定时器；取消时锁外移除 |
| `done`（DoneSignal） | 等待者信号；锁内只翻转 done 并搬出回调 |

**`DrainCancellation` 逐步拆**（`src/concurrency/context.cpp:1133`）

1. 显式栈 `work` 迭代（不递归，深树不爆栈）。
2. 每轮在 `state->mutex` 内标记 error/cause、摘 timer、搬回调、收集 children 并清空；已取消子树跳过（`:1143-1202`）。
3. 锁外移除 timer、从父节点解绑、把回调收集到全局列表。
4. 全部标记完成后，**逆序**执行 `before_done`，再**逆序** `Signal`（`:1219-1232`）。
5. 先标记后唤醒的原因：父回调里可能同步等子；先通知父会父等子、子等父（`:1119-1121`）。

**`ContextRollback`**：先标记所有 Context 取消，再冻结 undo（`:1214-1218`）；`Commit` 发现 child 已取消就转回滚（`:1560-1630`），避免取消窗口里把新动作当可提交。

**失败路径与限制**：回调在通知线程同步执行，禁止 park/阻塞（`include/go2cpp/concurrency/context.hpp:25`）；回调异常被吞掉；补偿按逆序执行。

**典型误用**：在 `Done` 回调里同步等待子 Context；在回调里捕获裸指针（UAF）。

### 2.11 Timer

**职责**：业务定时器；单后台线程 + `multimap` 按 deadline 有序 + 反向索引；`Timer` 是句柄，析构即取消。

**关键数据结构**

| 字段 | 行号 | 说明 |
|---|---|---|
| `Timer::State::m_status` | `src/concurrency/timer.cpp:210` | `kPending/kRunning/kCompleted/kCancelled/kFailed` |
| `m_next_deadline` / `m_callback` / `m_executor` | `src/concurrency/timer.cpp:206-213` | 到期时间、回调、可插拔执行器 |
| `Impl::m_entries` | `src/concurrency/timer.cpp:973` | `multimap<time_point, State>`，`begin()` 是下一个 |
| `Impl::m_index` | `timer.cpp`（紧邻） | `unordered_map<State*, iterator>`，取消 O(1) 定位 |
| `m_generation` | `src/concurrency/timer.cpp:234-235` | 插入/删除递增，后台线程据此重算等待 |

**线程模型**：一个后台线程，`wait_until` 最近 deadline；到期后把 State 摘出，**锁外**执行回调（`src/concurrency/timer.cpp:274-275`）。回调里可以再操作定时器而不死锁；但在回调里销毁 `TimerService` 会 join 自身 → `terminate()`（`:276-278`）。

**状态机**：`Pending → Running → Completed`；`Cancelled/Failed` 为终态。取消只阻止尚未领取的回调（`include/go2cpp/concurrency/core/timer.hpp:46-48`）。

**失败路径与限制**

- 单线程串行：一个慢回调拖慢该服务所有后续任务（`:280-284` 建议 IO 超时走 IOManager 的 deadline 队列）；
- `~Timer()` = `Cancel()`（`src/concurrency/timer.cpp:1006`）；临时句柄 `(void)ScheduleAfter(...)` 会当场取消；
- 已 Running 的回调不会被强杀。

**典型误用**：丢弃 `Timer` 返回值；在回调里销毁服务；把每请求 IO 超时塞进 TimerService。

### 2.12 SpinMutex

**职责**：极短纯计算临界区；禁止 park；debug abort / release 降级。

**关键数据结构**（`include/go2cpp/concurrency/spin_mutex.hpp:68-82`）

| 字段 | 位置 | 说明 |
|---|---|---|
| `m_impl` | 对象 offset 0 | 门（内部 `sync::Mutex`）；必须第一个成员（测试依赖地址相等） |
| `m_state` | offset 128 | 快路径唯一热字，独占 cacheline |
| `m_owner` | 同行 | 受管 G 的 owner 记录 |

**三段式**（`include/go2cpp/concurrency/spin_mutex.hpp:29-66`）：CAS 0→1 → 64 次 PAUSE → 门 `m_impl.Lock()`，唤醒后门内再自旋 16 次。

**状态机**：`0`（空闲）/`1`（持有）；`no_park_depth` 在 `Lock` 成功后 +1、`Unlock` 时按加锁 G 递减。

**失败路径**：debug 下 park 直接 abort；release 不 abort、降级为普通 Mutex 语义并计数（`:86-93`）；普通 OS 线程没有 `no_park_depth`，退化为普通 Mutex（`:196`）。

**典型误用**：在临界区里做 IO 或竞争 `Mutex::Lock`；用 `*_f` 版本规避 park（它不 park，但真阻塞整个 M，`include/go2cpp/concurrency/spin_mutex.hpp:115`）。

### 2.13 ParkingCondition

**职责**：运行时唯一的等待原语；同一份代码在受管 G 上 park、在普通线程上走 per-node futex。

**关键数据结构**（`include/go2cpp/concurrency/core/parking_condition.hpp:195-211`）

| 字段 | 行号 | 说明 |
|---|---|---|
| `m_queue_mutex` | `:195` | 只保护链表链接关系与计数；与节点门从不嵌套 |
| `m_head/m_tail/m_waiter_count` | `:196-198` | 侵入式双向链表；入队/摘链 O(1) |
| `BroadcastState{seq,waiters}` | `:206-211` | `notify_all` 的共享广播字；`futex_waitv` 同时监听「节点字 + 广播字」 |

**两条路径**（`:14-22`）

- **Fiber**：Scheduler park；唤醒方走 `wake_or_cancel` 把 G 放回调度队列，不经过 futex。
- **线程**：每节点自己的 32 位 futex 代际字；`notify_one` 只推进选中节点；`notify_all` 一次广播字 + 一次 `FUTEX_WAKE(INT_MAX)`，不支持 `futex_waitv` 时退回逐节点唤醒。

**为什么 park 前要自旋**（`:24-30`）：直接 `FUTEX_WAIT` 会在「刚检查完条件、还没进内核」的窗口丢唤醒；`kSpinBudget` 自旋把窗口跨过去。

**失败路径**：共享代际字会唤醒错节点（审计 D2）；`notify_all` 的广播路径在无 `futex_waitv` 环境退化为 N 次唤醒。

**状态机**：节点 `Waiting → Notified | TimedOut | Cancelled`；`notify_all` 推进广播代际，native 等待者由一次 `FUTEX_WAKE(INT_MAX)` 唤醒。

**典型误用**：不持外部锁调用 `wait`；把 `notify_one` 当成「一定唤醒我指定的那个线程」（FIFO 队首，不指定）。

---

## 附：读完之后的自检清单

1. 能否不看文档画出「G → P → M → 内核」的完整路径，并指出每一步的锁？
2. 能否解释 `runnext` 为什么是单槽、为什么窃取者不偷它？
3. 能否说出一个 G 从 `kWaiting` 回到 `kRunning` 的完整链路（谁改状态、谁入队、谁 resume）？
4. 能否指出 `Channel` 的 D1/D2 两个历史缺陷分别修在哪一行？
5. 能否解释 `Context` 为什么必须「先标记完整子树、再执行回调」？
6. 能否在 10 分钟内用 `bench_scheduler_task_opt` / `startup_cost_park_wake` 实测一次并报出条件？

如果有一题答不上，回到对应小节重读；这份文档不替代源码，只负责告诉你「该看哪一段、为什么」。
