# 并发运行时（Fiber / Scheduler / Channel / 同步原语）· 逐层深入

> 这份文档按**五层**组织，一层比一层靠近源码。不要跳层读：
> 第 1 层回答「为什么要有它」，第 2 层回答「它长什么样、怎么动」，
> 第 3 层回答「每一行在哪、失败会怎样」，第 4 层回答「真实场景怎么踩坑」，
> 第 5 层回答「慢在哪、怎么调」。
>
> **行号纪律**：所有 `文件:行` 都以 2026-11-04 的工作树
> （WSL `/UserData/CodexWorkSpace/Go2cpp`）为准，写作时逐条 `sed -n` / `grep -n`
> 核对过；代码会漂移，读到时先看一眼再信。
> **数字纪律**：第 5 层的每个数字都标了出处（`docs/reports/asm-*` 或
> `docs/metrics-ledger.md`），没有出处的数字一律写「未实测」，不编。
> 浅层「模块是什么」在 `user/docs/01-concurrency.md`；本文只讲
> **为什么这么设计 / 改哪里 / 会踩什么坑**。

## 五层导航

| 层 | 主题 | 读完能回答 | 行数占比 |
|---|---|---|---|
| 1 | What / Why | 它解决什么问题、边界在哪、和 Go 差在哪 | ~20% |
| 2 | How | 数据结构字段级、关键函数逐步、对象图 | ~30% |
| 3 | 源码细节 | 逐函数 `file:line`、状态机、锁/所有权、失败路径 | ~30% |
| 4 | 实战与陷阱 | 三个真场景（含超时/取消/关闭）、默认行为、调试 | ~20% |
| 5 | 性能与调优 | 已知瓶颈、实测数字、调参建议 | ~10% |

---

# 第一层 · What / Why：这个模块到底在解决什么

## 1.1 一句话定位

把 Go 的「**G（任务）— M（线程）— P（处理器）**」调度模型、`chan`/`select`、
`sync` 原语、`context` 取消树，在 C++17 上重做一遍：**让阻塞等待不占线程**，
同时保留「可以随时换回原生 `std::thread`」的逃生门。

它不是「协程库」，也不是「线程池」。区别在于三条硬约束：

1. **一个 G 的挂起必须能跨线程恢复**——所以栈不能是线程栈；
2. **挂起/唤醒必须是无锁快路径**——所以状态和唤醒令牌打包进同一个字；
3. **所有等待必须有超时/取消入口**——否则 C++ 里没有 `select` 兜底，一挂就死。

## 1.2 问题域：从 Go 迁移到 C++ 时，到底缺什么

| Go 里免费得到的东西 | C++ 里默认没有 | 本模块的对应物 | 不做的后果 |
|---|---|---|---|
| `go f()` 廉价创建任务 | `std::thread` 每线程 ~8 MiB 栈 + 内核调度 | `Scheduler::spawn` + 128 KiB 可增长 Fiber 栈 | 1 万并发要 80 GiB 虚拟地址 |
| 阻塞调用自动让出 P | 阻塞 = 占死一个 OS 线程 | `park_for_scheduler` + `ParkingCondition` | 线程数 = 并发数，2k 就崩 |
| `chan T` 的直交语义 | 无 | `Channel<T>` + `PendingSend/Recv` | 手写条件变量，丢唤醒/丢值 |
| `select` 多路等待 | 无 | `Select` + `SelectWaitState` 单赢家 | 多通道等待只能轮询 |
| `context` 取消传播 | 无 | `Context` 取消树 + `DrainCancellation` | 超时后后台任务继续跑 |
| `sync.Mutex` 的 FIFO + 饥饿保护 | `std::mutex` 无公平性保证 | `sync::Mutex`（正常/饥饿双模式） | 尾延迟尖刺、写者饿死 |
| `time.After` 廉价定时器 | 每个定时器一个线程/轮询 | `TimerService` 单线程 + `multimap` | 定时器数量一多就炸 |

**关键取舍**：Go 的运行时改不了，C++ 的运行时可以「按需启用」。所以本模块
把很多东西做成**可选**（`collect_metrics`、`enable_sysmon`、`wake_policy`、
`fiber_stack_growable`），默认值偏向「像 Go」，但每一个都能关掉换确定性。

## 1.3 边界：明确不做什么

| 不做 | 原因 | 替代方案 |
|---|---|---|
| 异步抢占（信号强制切 G） | C++ 栈上对象没有 GC 安全点，强切会撕裂 RAII | 只在 park/yield 点协作让出；CPU 密集任务自己 `yield_current()` |
| 手动 Fiber 上的 `Select`/`Channel` park | 手动 Fiber 没有「回到调度器」的路径 | 返回 `kNoScheduler`（2026-11-05 修正，旧值 `kInvalid`），或改用受管 `spawn` |
| `Channel` 的 `range` 语法糖 | C++ 没有语言级 range-over-channel | 循环 `Recv()` 直到 `kClosed` |
| nil channel 的永久阻塞 | 与「所有等待可取消」冲突 | 返回 `kNil`，让调用方显式处理 |
| G 的栈收缩 | 收缩要搬栈 + 修指针，C++ 没有精确栈图 | 只增长不收缩，靠 FiberBin 复用 |
| 跨进程/分布式调度 | 不是本模块范围 | 见 `docs/reports/asm-distributed.md` |

**边界的实际含义**：如果一个任务在用户代码里写了 `while(true) { compute(); }`
且不调用任何 park/yield，它会**独占一个 M 直到结束**。sysmon 只能观察
「已声明的阻塞」（`blocking_since_ns`），不能抢占纯计算。

## 1.4 与 Go 的对照：语义差异表

这张表是本文最重要的一张——**迁移代码时踩的坑，90% 在这里**。

| 维度 | Go | 本模块 | 差异的后果 |
|---|---|---|---|
| 默认 P 数 | `GOMAXPROCS`（默认核数） | `processor_count=0 → 硬件并发` | 一致；但可用 `SchedulerConfig` 固定 |
| 任务创建 | `go f()`，栈 2 KiB 起按需增长 | `spawn(fn)`，默认 2 KiB 提交、8 MiB 预留 VA | 10 万 G ≈ 819 GB VA，会被 `max_map_count` 卡住 |
| 栈增长 | 复制栈 + 修指针 | `mprotect` 提交新页，**不搬栈** | 栈地址稳定（好事），但不能收缩（坏事） |
| 抢占 | 异步抢占（1.14+ 基于信号） | 只有协作点 | 纯计算任务会拖住 M |
| `chan` 关闭后读 | 返回零值 + `ok=false` | 返回 `kClosed` + 空 optional | 语义等价，但**必须检查状态码** |
| `chan` 关闭后写 | panic | 返回 `kClosed`，不抛 | 更安全，但错误可能被忽略 |
| nil channel | 永久阻塞 | 返回 `kNil` | 依赖「阻塞」做设计的代码会空转 |
| `select` 多路就绪 | 均匀随机选一个 | 线程本地随机起点轮转 + 有值优先 | 有值优先是**故意**的（修 D1 丢值） |
| `sync.Mutex` | 正常/饥饿双模式，1 ms 阈值 | 同左，阈值也是 1 ms | 基本一致 |
| `sync.RWMutex` | 写者优先 | 写者优先 | 一致 |
| `context` 取消 | 先标记、再通知，回调异步 | 先标记整棵子树、再逆序回调 | 回调**同步**执行，禁止阻塞 |
| 定时器 | 每 P 一个四叉堆 | 全进程一个后台线程 + `multimap` | 单线程串行，慢回调拖全局 |
| 错误处理 | `panic`/`error` | `ErrorPtr` + 状态码，不抛 | 需要显式检查每个返回值 |

## 1.5 一条设计主线：把「等待」从线程里拿出去

整个模块可以浓缩成一句话：**等待状态是 G 的属性，不是线程的属性。**

```
传统线程模型：            本模块：
  线程阻塞在 recv           G 调 recv → 无数据
  ├─ 内核挂起线程           ├─ 登记 PendingRecv 到 Channel 队列
  ├─ 线程栈保留             ├─ G 状态字 CAS 成 Waiting
  └─ 1 个线程 = 1 个等待    ├─ Fiber 换栈回 worker（栈保留，线程继续跑）
                            └─ 唤醒方把 G 放回队列，任意 M 可恢复
```

代价是三个必须自己解决的问题，这也正是模块最复杂的三个地方：

1. **栈要能离开线程**：Fiber 栈 mmap + 上下文切换（第 2.1 节）；
2. **状态转移要原子且不留窗口**：状态 + 唤醒令牌打包成一个 32 位字（第 2.4 节）；
3. **唤醒必须能找到「谁在等」**：Channel/WaitNode/注册表三套索引（第 2.7、3.4 节）。

## 1.6 术语表（后文只用这些词）

| 词 | 含义 | 源码里的名字 |
|---|---|---|
| G / Task | 可调度任务，拥有独立栈 | `Task`（`scheduler.cpp:1717` 的 `run()`） |
| M / Machine | OS 线程 | `Machine`（`scheduler.cpp:2042` 附近） |
| P / Processor | 逻辑处理器，持有本地队列 | `Processor`（`scheduler.cpp:2017` 附近） |
| park | G 主动挂起，让出 M | `Task::park_for_scheduler`（`scheduler.cpp:1429`） |
| wake | 把 Waiting 的 G 改成 Runnable 并入队 | `Task::wake_for_wait_impl`（`scheduler.cpp:1358`） |
| 令牌（token） | 状态字里的 bit8，表示「已有人在等你醒」 | `kWakePendingBit` |
| 条带（stripe） | 按对象地址/ID 分片，降低锁竞争 | `incoming[16]`、`registry[16]` |
| claim | 抢占执行权/队列节点所有权 | `claim_from_queue`（`scheduler.cpp:1494`） |

## 1.7 第一层小结 & 下一层看什么

**小结**：这是一个「按需复刻 Go 运行时」的库。它的价值不在 API 长得像 Go，
而在**把阻塞从线程里解耦**；它的复杂度也不在调度算法，而在
「原子状态 + 栈所有权 + 可取消等待」三件事的交界处。

**下一层看什么**：先看 Task/P/M 三个结构体的字段（谁保护谁、哪些是原子），
再看 `spawn → enqueue → worker_loop → claim → run → park → wake` 这条主链，
最后看 Channel 的直交/缓冲/等待三条分支——这三段读懂了，第三层才有意义。

---

# 第二层 · How：结构与方法

## 2.1 对象图（先建立空间感）

```
┌──────────────────────────── Scheduler ────────────────────────────┐
│  Impl                                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐ │
│  │ P[0..N-1]    │  │ M[0..K-1]    │  │ registry[16] 条带         │ │
│  │  queue: 256  │  │  thread      │  │  └ 强持有所有非终态 G     │ │
│  │  runnext: 1  │  │  inbox: 1    │  │ incoming[16] 条带         │ │
│  │  active_mach │  │  blocking_*  │  │ global_queue + emergency  │ │
│  └──────┬───────┘  └──────┬───────┘  └────────────┬─────────────┘ │
└─────────┼─────────────────┼───────────────────────┼───────────────┘
          │ 取/放 G          │ 执行                  │ 保活/遍历
          ▼                 ▼                       ▼
      ┌──────────────────────────────────────────────────┐
      │ Task (G)                                          │
      │  m_state_word   [state:8 | token:1 | ...]         │
      │  m_execution_claim / m_run_claim / m_queued       │
      │  fiber (首次执行才创建)                            │
      │  m_processor (kInvalidPId = 不在任何 P 上)         │
      └───────────────┬──────────────────────────────────┘
                      │ 挂起时
                      ▼
      ┌──────────────────────────────┐     ┌───────────────────────┐
      │ Channel<T> / Mutex / CV / WG │────▶│ ParkingCondition      │
      │  等待队列（侵入式节点）        │     │  queue_mutex + 链表   │
      └──────────────────────────────┘     │  per-node futex 字    │
                                           │  broadcast 代际字     │
                                           └───────────────────────┘
```

**三条所有权线**（第 3 层会逐条展开）：

- **G 的生命周期**：调用方 `shared_ptr<Task>` + `registry` 强持有非终态 G；
  终态后注册表放手，等调用方释放。
- **栈的生命周期**：G 持 `Fiber`；G 结束时 `Fiber::RecycleForScheduler`
  进 FiberBin（每 M 一个池）；池满才真析构。
- **等待节点的生命周期**：节点在**等待者栈帧**上，队列只存裸指针；
  取消/超时只是置标记，真正摘除由交接方做（防 UAF）。

## 2.2 Task（G）字段级

| 字段 | 类型/位宽 | 谁写 | 保护方式 | 作用 |
|---|---|---|---|---|
| `m_state_word` | `atomic<uint32_t>` | park/wake/claim/终态 | CAS/acq_rel | 低 8 位状态、bit8 唤醒令牌 |
| `m_execution_claim` | `atomic<bool>` | claim、`try_mark_running` | 原子 | 「有人正在执行这个 G」 |
| `m_run_claim` | `atomic<bool>` | `Task::run` 入口 | 原子 | 防止重入 `run()` |
| `m_queued` | `atomic<bool>` | 入队/出队 | 原子 | 「已有一个队列节点」，入队权 |
| `m_processor` | `atomic<PId>` | 入队/窃取 | 原子 | 当前归属 P；`kInvalidPId` 合法 |
| `m_fiber` | `shared_ptr<Fiber>` | `Task::run` | 仅执行者 | 首次运行时创建 |
| `m_no_park_depth` | `atomic<uint32_t>` | SpinMutex | 原子 | park 契约检查（禁止区深度） |
| `m_held_mutexes` | `atomic<uint32_t>` | Mutex | 原子 | 持锁 park 告警 |

**为什么状态和令牌要打包**（`scheduler.cpp:1429` 的 `park_for_scheduler`）：

```
线程 A（park）                      线程 B（wake）
 读 word = Running                  读 word = Running
 CAS(Running → Waiting)              CAS(Running → Running|token)
   ├─ 成功 → kParked                   ├─ 成功 → 看到 token 后
   │   此后 B 的 wake 看到 Waiting      │   若 A 已 Waiting，B 负责
   │   必须由 B 负责入队                │   把 Waiting→Runnable 并入队
   └─ 失败（B 先置 token）→ 重读        └─ 失败（A 已 Waiting）→ 走唤醒
       看到 token → 清 token 返回 kRequeued
```

如果拆成「状态原子 + 令牌原子」两个变量，就会出现
「令牌已发布、挂起已提交、但没人消费」的窗口——这是 Go runtime 里
`park` 与 `ready` 必须共用 `g.status` 的同一个原因。

## 2.3 P（Processor）与本地队列

`Processor`（`scheduler.cpp:2017` 附近）只有 6 个字段，但每个都有讲究：

| 字段 | 说明 | 不变量 |
|---|---|---|
| `id` | P 编号，0 合法 | 不要用 `!=0` 判断存在 |
| `mutex` | 冷路径锁（shutdown 排空/快照） | 热路径**不进入** |
| `queue` | 256 槽有界 MPMC 环（Vyukov） | 满时 push 返回 false，调用方回落全局队列 |
| `runnext` | 单槽 LIFO | 只有 owner 消费；窃取者**不偷** |
| `state` | idle/running/dead | 快照可锁外读 |
| `active_machines` / `running_machines` | 附着 M 数 / 正在跑 G 的 M 数 | 增减在 `Impl::mutex` 内 |

**为什么要 `runnext` 单槽**：刚被唤醒的 G 与唤醒者在同一条数据链上，
放 runnext 可以让它**立刻**在本 P 执行（缓存热、锁热）；放环尾要等 256 个。

**为什么窃取者不偷 runnext**：如果偷了，owner 下次取活会扑空，
而 runnext 的语义是「本 P 的下一口」；偷它会破坏 `wake_policy=kRunNext` 的局部性。
`publish_runnext`（`scheduler.cpp:4138`）被挤出者会回落到环尾。

```
LocalRunQueue（256 槽环）:
  head ──▶ [ G ][ G ][   ][ G ] ... [ G ] ◀── tail
            ↑ pop 端                      ↑ push 端
  满：push 返回 false（调用方转 global_queue）
  空：pop 返回 null（调用方去窃取/incoming）
```

## 2.4 M（Machine）字段级与阻塞记账

| 字段 | 说明 |
|---|---|
| `id` / `processor` | 全局递增 id（不从 0 开始）/ 首选 P |
| `replacement` | 补充 M 标记；启动前写一次，之后只读 |
| `thread` | `std::thread` 本体；shutdown 时 join |
| `state` | Idle/Running/Blocking/Parked/Stopping/Dead |
| `inbox` | 定向槽（`LockToCurrentM` 绑定的 G 只投这里） |
| `processor_detached` | 是否已从 P 解绑（sysmon 据此补 M） |
| `blocking_since_ns` | 进入阻塞区的时刻；sysmon 用它判长 syscall |
| `long_syscall_count` | 观测计数 |

**基础 M vs 补充 M**：

- 基础 M：启动时创建 `min_workers` 个，**永不因空闲退出**；
- 补充 M：`blocking_workers > 0` 时创建，阻塞归零后**立即退休**。

sysmon 每 10 ms 扫一次（`enable_sysmon` 默认 true），只处理**已声明**的阻塞：
普通线程跑长 syscall 不会被告知，只有走 `BlockingRegion` 的 M 才会被补位。

## 2.5 主链：spawn → enqueue → 取活 → claim → run

### spawn（`scheduler.cpp:3805` / 带 options 的 `:3824`）

```
step1. 构造 Task（shared_ptr），此时不建 Fiber、不建栈
step2. registry 登记（强持有，保证非终态 G 不会被析构）
step3. enqueue(task)
step4. 入队失败 → task->cancel()，返回终态句柄（不是永不运行的句柄）
```

**为什么首次执行才建栈**：spawn 是热路径，建栈要 mmap + mprotect；
而很多任务在排队期间就被取消/shutdown 了。这一取舍让 spawn 只付
一次堆分配 + 引用计数（第 5 层会看到它的代价）。

### enqueue（`scheduler.cpp:3901`）

```
外部生产者：
  按 GId/地址选 16 条带之一 → 该条带的 incoming 队列（叶子锁）
  持锁期间禁止再调 enqueue（锁层次：admission → 条带 → P → Task）
目标 P 的 M 正在跑：
  publish_runnext（scheduler.cpp:4138）→ runnext 单槽，被挤出者回环尾
否则：
  本地环 push；失败则 global_queue
```

### worker_loop（`scheduler.cpp:5651`）取活顺序

```
step0  inbox（定向槽）
step1  runnext
step2  本地环（256 槽）
step3  incoming[16] 条带（窃取外部生产者投递的任务）
step4  admission（全局队列 + 紧急链）
step5  都没有 → IO idle poller → 有界自旋 → 条件变量睡眠
```

**顺序不是随便排的**：先拿「本 P 私有、最热」的，再拿「共享、要加锁」的。
把 incoming 放到本地环之前会破坏局部性；放到 admission 之后会让外部
生产者饿死。这个顺序在 `scheduler.cpp:5651` 起的循环里可以逐段对应。

### claim（`scheduler.cpp:1494`）

```
for (;;) {
  word = m_state_word.load(acquire)
  if (状态 != Runnable) return false          // 已被别人抢/已取消
  if (CAS(word, Running)) {                    // 一次 CAS 即执行权仲裁
    m_queued = false                           // 释放入队权
    m_execution_claim = true                   // 标记执行者
    return true
  }
}
```

注释里写明「先抢 claim 再改状态要多付一次 RMW，实测 1P2M 的 CV 往返
每次切换多 ~8ns」，所以合并成单 CAS。**这是「测量驱动设计」的典型例子**：
教科书会先写 claim 再写 state，这里因为实测差 8 ns 而反过来。

### run（`scheduler.cpp:1717`）

```
step1. m_run_claim CAS，防重入
step2. 首次：创建 Fiber（128 KiB 默认栈、8 MiB 预留）
       非首次：从 FiberBin 取复用（use_count()!=1 就丢弃）
step3. Fiber::resume → 用户函数
step4. 返回后清理 claim、按状态记账：
       终态 → notify_terminal（scheduler.cpp:1115）+ RecycleForScheduler
       Waiting → 记 kTaskSuspended 事件
       Running（异常/未挂起）→ recover_orphan_running（scheduler.cpp:1638）
```

## 2.6 park / wake 的完整时序

```
G 在 M 上跑                    Channel 队列                 另一个 G / 线程
    │                              │                              │
    ├─ Send，无接收者               │                              │
    ├─ 建 PendingSend ────────────▶│ 入 m_senders                 │
    ├─ 注册 Context Done 回调       │                              │
    ├─ park_for_scheduler()        │                              │
    │    CAS Running→Waiting       │                              │
    ├─ Fiber 换栈回 worker ────────┼──────────────────────────────┤
    │                              │        Recv 到达             │
    │                              │◀─── 摘队首、写值、CAS signal │
    │                              │      锁外 NotifyChange       │
    │                              │───────────▶ wake_for_wait_impl
    │                              │             Waiting→Runnable
    │                              │             投 runnext/本地环
    │◀──── 某 M 取到、claim、resume ┘                              │
    ├─ 等待循环重检 accepted/closed/cancelled/deadline             │
    └─ 返回 kReady                                                │
```

**三个必须理解的细节**：

1. **park 前先登记等待节点，再改状态**——顺序反了会出现「已挂起但没人知道」；
2. **通知必须在锁外**——锁内 notify 会引入「Channel 锁 → 调度器锁」的反向嵌套；
3. **醒来必须重检**——`signal` 可能被取消/超时抢先，单赢家语义靠 CAS 保证。

## 2.7 Channel 的结构与三条分支

`Channel<T>` 的核心字段（`include/go2cpp/concurrency/channel.hpp`）：

| 字段 | 作用 |
|---|---|
| `m_capacity` | 0 = rendezvous，不分配缓冲 |
| `m_lock`（ChannelSpinLock） | 保护 buffer/senders/receivers/closed，**禁止持锁 park** |
| `m_buffer`（RingBuffer） | head/tail/count；cap=0 不分配 |
| `m_senders` / `m_receivers` | 侵入式等待队列，节点由等待者栈帧持有 |
| `m_closed` | 关闭标记 |
| `m_generation` / `m_change_waiters` | 状态代际；为 0 时快路径不写，避免 cacheline 迁移 |

**三条分支**（`SendUntil` `include/go2cpp/concurrency/channel.hpp:2881` / `RecvUntil` `include/go2cpp/concurrency/channel.hpp:3262`）：

```
Send(v):
  ├─ 锁内看到 m_receivers 非空 → 直交（值直接 emplace 进节点）
  ├─ 否则缓冲有空位 → 入缓冲，返回 kReady
  └─ 否则 → 登记 PendingSend + 注册取消回调 → park
             醒来重检 accepted/closed/cancelled/deadline

Recv():
  ├─ 锁内看到 m_senders 非空 → 直交（取值）
  ├─ 否则缓冲有值 → 取出
  └─ 否则 → 登记 PendingRecv → park

Close():
  置 m_closed → 先唤醒接收者（缓冲值仍可读完）→ 再唤醒发送者（kClosed）
```

**直交为什么必须在锁内完成「写值 + CAS signal」**：如果先唤醒再写值，
接收者可能被别的线程恢复后读到空值。所以顺序是
「值先落位 → CAS signal → 锁外通知」。

## 2.8 Select：探测 + arm + 单赢家

```
step1. 随机起点探测每个 case 的 probe()
       kReady → 立即返回（传输已发生，不可回滚）
       closed/nil/invalid → 暂存（无副作用）
step2. 无 ready → 检查 default / 取消 / 超时
step3. arm：按 SelectLockKey（对象地址）全序，一次性持有整组 Channel 锁
       锁内重新验证并登记等待节点
step4. park
step5. 任一 channel 就绪 → SelectWaitState::TrySelect 只有一个赢家
       输家节点由清理逻辑摘除（不重复提交结果）
```

**为什么要全序加锁**：N 个 channel 两两组合，任意顺序加锁都可能 ABBA。
用地址排序后，所有 Select 的加锁顺序一致。

**为什么 closed 不优先**：旧实现「第一个 ready 就返回」导致 closed 遮蔽
同批另一个通道的缓冲值（D1 丢值）。现在 `kReady` 立即返回、closed 延后，
代价是「有值 vs 可发送」不排序——两者都立即传输，谁先探测到谁赢。

## 2.9 同步原语的结构速览

| 原语 | 核心字段 | 快路径 | 慢路径 |
|---|---|---|---|
| `Mutex` | 状态字节（Locked/Waiter/Starving/Woken）+ FIFO `m_waiters` | 一次 CAS | 自旋 8 次 → 入队 → park |
| `RWMutex` | 读者计数 + 写者门 + `m_pending` 合并原子字 | 读者一次 `fetch_add` | 写者进 `m_writer_mutex` 排队 |
| `ConditionVariable` | `m_single` 快路径槽 + `m_waiters` FIFO | 1P1C 无锁槽 | 入队 + park + 重锁 |
| `WaitGroup` | 计数原子 + 等待者队列 | `fetch_add/sub` | 归零整波唤醒 |
| `SpinMutex` | 门（内部 Mutex）+ 热字 + owner | CAS 0→1 | 64 次 PAUSE → 门 |
| `ParkingCondition` | `queue_mutex` + 侵入链表 + per-node futex | 自旋预算 | futex_wait / park |

**Mutex 的状态字为什么是「一个字节」**：`kLocked/kWaiter/kStarving/kWoken`
四个位必须在同一个原子字里，否则「释放时判断有没有等待者」会出现
「读到没有 → 直接释放 → 等待者入队 → 永久睡眠」的丢唤醒。
`src/concurrency/sync.cpp:1577` 的 `LockImpl` 与 `:1999` 的 `UnlockSlow`
就是围绕这一个字节做文章。

## 2.10 Context 与 Timer 的结构

**Context**（`src/concurrency/context.cpp`）：

| 字段 | 作用 |
|---|---|
| `error` / `cause` | 取消原因；非空即已取消（幂等） |
| `children` | 子节点集合；取消时收集并清空 |
| `before_done_callbacks` | 内部/用户回调；先搬出、锁外执行 |
| `timer_id` | deadline 定时器；取消时锁外移除 |
| `done`（DoneSignal） | 等待者信号；锁内只翻转 done 并搬出回调 |

**Timer**（`src/concurrency/timer.cpp`）：

| 字段 | 作用 |
|---|---|
| `m_entries`（`multimap<time_point, State>`） | 按 deadline 有序，`begin()` 即下一个 |
| `m_index`（`unordered_map<State*, iterator>`） | 取消 O(1) 定位 |
| `m_generation` | 插入/删除递增，后台线程据此重算等待 |

**单线程 + multimap 的取舍**：插入 O(log n)、删除 O(1)、到期 O(k)；
代价是所有回调串行，一个慢回调拖慢该服务全部任务。

## 2.11 第二层小结 & 下一层看什么

**小结**：结构上有三个「反直觉」但必需的设计——状态与令牌同字、
runnext 单槽、等待节点放栈上。它们分别解决原子性、局部性、UAF 三个问题。

**下一层看什么**：带着三个问题读第 3 层——
「这个函数在哪个文件哪一行？」「它失败时对象处于什么状态？」
「它持有什么锁，锁序是什么？」

---

# 第三层 · 源码细节：逐函数、状态机、锁与失败路径

## 3.1 逐函数索引（写作时逐条核对）

| 函数 | 位置 | 职责 | 失败返回 |
|---|---|---|---|
| `Task::park_for_scheduler` | `scheduler.cpp:1429` | 契约检查 + 状态字 CAS | `kRejected` / `kRequeued` / `kParked` |
| `Task::claim_from_queue` | `scheduler.cpp:1494` | 单 CAS 抢执行权 | false（已被抢/非 Runnable） |
| `Task::wake_for_wait_impl` | `scheduler.cpp:1358` | Waiting→Runnable，必要时延迟入队 | `WakeAction` |
| `Task::resume_after_failed_park` | `scheduler.cpp:1616` | Fiber 拒绝挂起时回滚 Running | bool |
| `Task::notify_terminal` | `scheduler.cpp:1115` | 终态通知（等待者/注册表） | — |
| `Task::mark_runnable` | `scheduler.cpp:1520` | 把 G 放回可运行并发布 | bool |
| `Task::recover_orphan_running` | `scheduler.cpp:1638` | 自愈：run 返回后仍 Running | bool |
| `Task::run` | `scheduler.cpp:1717` | 建/复用 Fiber 并 resume | — |
| `Scheduler::spawn` | `scheduler.cpp:3805` / `:3824` | 建 Task + 入队 | 终态句柄 |
| `Scheduler::enqueue` | `scheduler.cpp:3901` | 选条带/投 runnext/本地环/全局 | bool |
| `Scheduler::publish_runnext` | `scheduler.cpp:4138` | 单槽投递 + 挤出者回环 | bool |
| `Scheduler::worker_loop` | `scheduler.cpp:5651` | 取活/执行/记账/休眠 | — |
| `Scheduler::start` | `scheduler.cpp:3323` | 拉起基础 M + sysmon | void |
| `Scheduler::run_main` | `scheduler.cpp:3382` | 当前线程当 m0 | bool |
| `Scheduler::shutdown_for` | `scheduler.cpp:3467` | 关 admission → drain → 等终态 → join | bool（超时） |
| `Scheduler::shutdown` | `scheduler.cpp:3686` | `shutdown_for(max)` | void |
| `begin_shutdown_locked` | `scheduler.cpp:2792` | 排空所有队列、取消/唤醒 | — |
| `Channel::SendUntil` | `include/go2cpp/concurrency/channel.hpp:2881` | 直交/缓冲/等待三路 | `ChannelSendResult` |
| `Channel::RecvUntil` | `include/go2cpp/concurrency/channel.hpp:3262` | 直交/取缓冲/等待三路 | `ChannelRecvResult` |
| `Channel::Close` | `include/go2cpp/concurrency/channel.hpp:1821` | 置 closed + 分波唤醒 | `ChannelCloseResult` |
| `Mutex::LockImpl` | `sync.cpp:1577` | 快路径/自旋/入队/park | bool（可取消） |
| `Mutex::UnlockSlow` | `sync.cpp:1999` | 交接/唤醒/饥饿降级 | void |
| `ConditionVariable::Wait` | `sync.cpp:2360` | 入队→解锁→park→重锁 | bool |
| `ConditionVariable::NotifyOne` | `sync.cpp:2679` | 摘节点/清槽 + 锁外唤醒 | void |
| `WaitGroup::Add` / `Done` / `Wait` | `sync.cpp:2935` / `:2993` / `:3052` | 计数与归零 | void / bool |
| `Context::DrainCancellation` | `context.cpp`（`DrainCancellation`） | 标记整棵子树 + 逆序回调 | — |
| `IOManager::wait` | `io.cpp:2545` | 登记 waiter + park | `WaitResult` |

## 3.2 状态机全集

**G（Task）**：

```
New ──spawn──▶ Runnable ──claim──▶ Running
                  ▲                  │
                  │                  ├─ park ──▶ Waiting ──wake──▶ Runnable
                  │                  ├─ yield ──▶ Runnable（回环尾）
                  │                  ├─ 返回 ──▶ Completed/Failed
                  └──cancel──────────┴─ cancel ──▶ Cancelled
Waiting 下被 cancel：置 token，唤醒后由等待循环收尾（不直接改终态）
```

**M**：`Idle → Running → {Blocking → Idle | Parked → Idle | Stopping → Dead}`

**P**：`Idle → Running → Stopping → Dead`

**Fiber**：`Ready → Running → Suspended → Running → Completed/Failed`；
只有 Completed/Failed 才允许进 FiberBin。

**Channel 等待节点**：`Waiting → kReady | kClosed | kCancelled | kTimedOut`，
由 `signal` 的 CAS 选出唯一赢家；输家只做清理，不改结果。

**Mutex**：`正常模式 ↔ 饥饿模式`（等待 >1 ms 触发；队列空或被交接者
未超阈值时退出）。状态位是 `kLocked/kWaiter/kStarving/kWoken`。

**Timer**：`Pending → Running → Completed`；`Cancelled/Failed` 终态。
取消只阻止尚未领取的回调。

## 3.3 锁层次与所有权

**全局锁序**（任意时刻只能按这个方向嵌套，反向必死锁）：

```
admission（Impl::mutex）
   └─▶ incoming 条带锁（叶子锁，持锁期间禁止再调 enqueue）
        └─▶ P 锁（冷路径）
             └─▶ Task 状态字（原子，不是锁）
```

**另外三条独立链**（与上面不交叉）：

- Channel：`ChannelSpinLock` → 等待者 futex 字（**不能**在持 Channel 锁时 park）；
- ParkingCondition：`queue_mutex` ↔ 节点门（注释明确「从不嵌套」）；
- IO：`IOManager::State::m_mutex`（deadline/registrations）→ 条带锁。

**所有权三条铁律**：

1. `fd`/Fiber 栈/等待节点都是「**谁创建谁持有，谁出队谁负责**」；
2. 队列只存裸指针/弱引用，**不允许**在队列里放 `shared_ptr` 之外的强生命周期延长；
3. 回调（Context/Channel 取消）**永远**在锁外执行，且禁止 park。

## 3.4 失败路径分类（F1–F12）

| # | 失败 | 触发点 | 代码行为 | 调用方该做什么 |
|---|---|---|---|---|
| F1 | 入队失败 | `enqueue` 返回 false | `spawn` 内 `cancel()`，返回终态句柄 | 检查 `task->done()`，别当它还会跑 |
| F2 | claim 失败 | `claim_from_queue` false | `recover_unclaimed`（`scheduler.cpp:5697` 的 lambda）判定是否补发布 | 无（内部自愈），但可观测计数 |
| F3 | Fiber 拒绝挂起 | `park_for_scheduler` 返回 kRejected | `resume_after_failed_park` 回滚 Running | 重试或直接返回错误 |
| F4 | 取消先于 park | 状态字已 Cancelled | `kRejected`，等待循环立即返回 | 检查 `kCancelled` |
| F5 | 唤醒令牌先到 | bit8 已置 | 清令牌返回 `kRequeued`，**不挂起** | 继续执行（不要假设一定睡过） |
| F6 | 超时 | deadline 到期，`TryFinish(kTimedOut)` | 摘节点、返回 `kTimedOut` | 区分「没等到」与「关闭」 |
| F7 | 关闭与发送并发 | Send 锁内看到 `m_closed` | 返回 `kClosed`，不写入 | 停止发送，排空接收 |
| F8 | 重复关闭 | 第二次 `Close` | `kAlreadyClosed` | 幂等处理 |
| F9 | 重锁失败（CV） | 调度器已关闭 | 先 8192 次无 park 抢回，抢不回抛异常 | 捕获，别在未持锁时改容器 |
| F10 | shutdown 超时 | `shutdown_for` 有界超时 | 返回 false，保持 draining | 可再次 `shutdown()` 继续 |
| F11 | 锁内 park | `no_park_depth != 0` | debug abort / release 降级 + 计数 | 拆临界区，别在 SpinMutex 里等 |
| F12 | 注册表泄漏 | 非终态 G 未释放 | shutdown 永不返回 | 检查是否有 G 卡在 Running |

## 3.5 三个关键时序的逐行读法

### 时序 1：park 的原子提交（`scheduler.cpp:1429`）

```
1. 读 no_park_depth / m_held_mutexes（只在当前 G 是 this 时）
2. load state_word(acquire)
3. 状态 != Running → kRejected（已取消/终态）
4. token 位已置 → CAS 清 token → kRequeued（不睡）
5. CAS(Running → Waiting) → kParked
6. Fiber::Suspend 换栈；worker 继续下一轮
```

**读法**：这段代码没有锁、没有分配、`noexcept`。它的正确性完全依赖
「状态 + token 在同一个字」。第 4 步是「唤醒先到」的快速路径，
第 5 步失败说明有人并发改了状态，循环重读。

### 时序 2：wake 的延迟入队（`scheduler.cpp:1358`）

```
1. CAS(Waiting → Runnable)
2. 若执行权仍被占（m_execution_claim）→ 只置 deferred_enqueue
3. 否则立即入队（runnext/本地环）
```

**为什么延迟**：唤醒者可能是在被唤醒 G 自己的栈上（同线程回调），
此时立刻入队会被同一 M 取到并重入。延迟到 `run()` 退出后再发布，
保证「一个 G 不会在自己还没退出时被再次执行」。

### 时序 3：shutdown 的 12 步（`scheduler.cpp:3467` / `:2792`）

```
1. 关 admission（set_accepting(false)、set_draining(true)）
2. begin_shutdown_locked 排空 P：runnext + 环
3. 已启动 G → request_cancel + 放回可运行；未启动 → 原地取消
4. 排空 global / 每台 M 的 inbox / 16 条 incoming
5. 扫 registry[16] 唤醒 parked G
6. 等 live_tasks_total()==0（O(1) 谓词）
7. 超时 → 返回 false，保持 draining
8. 停 sysmon（set_stopping + stop_sysmon）
9. 等 active_workers==0（有界路径）
10. 持锁摘 thread，放锁后 join
11. P 置 Dead、清 started
12. 锁外释放 deferred_destruction 里的用户捕获
```

**第 10 步的顺序不能反**：worker 尾声要先拿 `Impl::mutex`，
如果持锁 join 会自死锁。

## 3.6 Fiber 栈增长与信号路径

前面把 G 当成「有栈的任务」，但栈从哪来、用完怎么办，是本节的内容。
这一节没有新概念，只有四个必须同时成立的约束。

### 3.6.1 栈的三段式布局

```
高地址
┌──────────────────────────┐  ← 预留区顶端（8 MiB VA，PROT_NONE）
│  guard / 未提交预留区      │     访问 → SIGSEGV → 增长判定
├──────────────────────────┤  ← 已提交窗口顶端（初始 4 KiB，页对齐）
│  已提交可用栈（向下增长）   │     正常 push/pop 在这里
├──────────────────────────┤  ← 当前 rsp 附近
│  已提交保留区（低水位）     │     增长时 mprotect 把窗口往下推
└──────────────────────────┘  ← 预留区底端
低地址
```

对应实现：`FiberStack` 的 `m_initial_usable / m_reserve / m_committed`
（`src/concurrency/fiber.cpp` 的 `FiberStack` 段），以及 `Fiber::Impl`
的 `m_stack / m_stack_size / m_stack_growable`。三个数字的含义分别是
「一开始能用的字节数」「VA 预留总量」「已提交到哪」。

**为什么预留不等于提交**：8 MiB × 10 万 G = 819 GB VA，但实际只提交
每栈顶部几 KiB。VA 不占物理页，但占 `vm.max_map_count` 的映射数——
这是「3 万 G 触顶」的根因（每个 G 至少两个映射：栈 + guard）。

### 3.6.2 增长判定：handler 只认三种信号

访问未提交页 → SIGSEGV。handler 必须判断「这是栈增长，还是真 bug」，
只有同时满足才增长：

```
条件 1：fault 地址落在该 Fiber 的预留区内（不是任意地址）
条件 2：当前 rsp 也在预留区内（不是野指针写到了栈区）
条件 3：距离已提交窗口不超过 256 KiB（防止一次跳太远把 guard 跨过去）
```

三条都成立 → `mprotect` 提交下一页/一段 → 返回继续执行；
任一不成立 → 恢复默认动作，保留 core（**不要吞掉真崩溃**）。

**固定栈退化**：`fiber_stack_growable=false`、非 Linux、非原生后端三种情况
都不安装 handler、不分配 `sigaltstack`。此时爆栈 = 进程被杀，
没有「增长失败」这种中间态。这是可移植性换来的代价。

### 3.6.3 切换只换 rsp

`resume()` 与 `Suspend()` 的核心是保存 6 个 callee-saved 寄存器 +
返回地址，然后换 rsp 跳转（汇编在 `src/concurrency/context_x64.S`）。
**不拷贝栈**，所以 G 迁移到另一个 M 时栈地址不变——这既是优点
（指针稳定、无需修栈）也是限制（不能收缩、不能搬栈）。

`resume()` 的并发保护是 `m_switch` 里的 claim 位：第二个并发 resume
直接返回 false，不破坏栈。跨线程 resume 会更新
`m_last_resume_thread_hash` 并触发诊断，但**不禁止**——调度器本来就
要迁移 G。

### 3.6.4 复用：FiberBin

终态 Fiber 进每 M 的 `FiberBin`（默认 32/调度器 128/上限 4096 槽）。
复用时要求 `use_count()==1`，否则丢弃——因为还有别人持有这个 Fiber
（观察者/调试器），复用会撕裂其视图。池满则真析构（归还 VA）。

**代价**：池化让「首次执行」与「复用执行」走不同分支，
`Task::run` 里因此有一次 `if (首次)`；这也是 spawn 路径指令数偏高的
原因之一（见第 5 层）。

## 3.7 锁序自检表（改代码前先过一遍）

| 你要拿的锁 | 允许先持有 | 禁止先持有 | 违反后果 |
|---|---|---|---|
| `Impl::mutex`（admission） | 无 | 任何条带锁、P 锁 | ABBA 死锁 |
| incoming 条带锁 | admission | P 锁、Task 状态 | 死锁 + 丢唤醒 |
| P 锁 | admission | 条带锁 | 反向嵌套 |
| `ChannelSpinLock` | 无 | 任何调度器锁 | 持锁 park → 自死锁 |
| `ParkingCondition::m_queue_mutex` | 无 | 节点门 | 注释明确「从不嵌套」 |
| `IOManager::State::m_mutex` | 无 | 条带锁 | `expire_timers` 两阶段就是为了破这个环 |

**读法**：这张表是「静态检查清单」，不是运行期机制。代码 review 时
先看新加的锁在表里的哪一行，再看它上方允许的锁是否真的持有。

## 3.8 第三层小结 & 下一层看什么


**小结**：这一层的关键不是记住行号，而是记住**失败路径的形状**——
几乎所有错误都被折叠成「状态码 + 等待循环重检」，而不是异常。
唯一的例外是 CV 重锁失败（F9），因为它破坏了「返回即持锁」的契约。

**下一层看什么**：拿三个真实场景去验证上面的机制——
一次 Channel 超时、一次取消与关闭的竞态、一次 shutdown 卡住。
重点看「默认值在什么情况下会坑你」。

---

# 第四层 · 实战与陷阱

## 4.1 场景 1：Channel 超时（`RecvFor`）

**背景**：一个 worker 从任务通道收活，等待 1 ms 超时后去检查退出标志。

```cpp
auto r = ch->RecvFor(std::chrono::milliseconds(1), ctx);
switch (r.status) {
  case ChannelStatus::kReady:      handle(r.value); break;
  case ChannelStatus::kTimedOut:   check_shutdown(); break;
  case ChannelStatus::kClosed:     return;   // 通道关了
  case ChannelStatus::kCancelled:  return;   // ctx 取消了
  default: /* kWouldBlock / kNil */ break;
}
```

**会发生什么**（按源码顺序）：

1. `RecvUntil`（`include/go2cpp/concurrency/channel.hpp:3262`）先做一次锁内直交/缓冲检查；
2. 没有值 → 建 `PendingRecv` 入 `m_receivers`，注册 Context 回调；
3. 计算 deadline，用 `TimerService::Default().Schedule`（`include/go2cpp/concurrency/channel.hpp:2645`）登记超时；
4. park；超时后 `TryFinish(kTimedOut)` 摘节点、返回 `kTimedOut`；
5. 如果此刻发送者正好到达，`signal` 的 CAS 只有一个赢家——
   要么拿到值（kReady），要么超时（kTimedOut），**不会两者都发生**。

**坑 1：1 ms 不是精确值**。Timer 是单线程 + `multimap`，后台线程
`wait_until` 到期后才摘；实际误差取决于该线程被调度的时间。要用
精确超时，走 `IOManager` 的 deadline 队列（IO 路径），不要用业务 Timer。

**坑 2：超时后节点不会立刻从队列中间消失**。取消/超时节点靠交接方
`pop_front` 时跳过并回收（僵尸节点）。所以「超时后立刻复用同一个
Channel 对象」是安全的，但「超时后立刻销毁 Channel」要看是否还有
交接方在跑——这也是为什么 `Channel` 用 `shared_ptr` 管理。

**坑 3：`kTimedOut` 与 `kCancelled` 要分开处理**。前者是「这次没等到」，
后者是「整体取消了」。混在一起会导致退出流程吞掉真实错误。

## 4.2 场景 2：取消与关闭的竞态（UAF 风险点）

**背景**：HTTP handler 用 `ctx` 取消一个正在 `Send` 的 goroutine，
同时另一个线程 `Close()` 通道。

三个角色同时动：

```
G1: Send(v)  → 已登记 PendingSend，park
G2: Close()  → 置 m_closed，准备唤醒 senders
G3: ctx.Cancel() → 回调按 token 找到 PendingSend，CAS signal=kCancelled
```

**源码怎么保证不 UAF**：

1. 取消回调只持 `weak_ptr<CancelHub>` + token，在 hub/Channel 锁内
   按 token 定位节点并 CAS；**从不直接解引用队列外节点**；
2. 节点在 G1 的栈帧上，G1 不返回，节点就不会失效；
3. Close 与 Cancel 都只做「CAS signal + 唤醒」，真正的摘除由 G1
   醒来后的等待循环做；
4. 若 G1 先被唤醒并已返回，Cancel 的 token 查找会失败（节点已标记），
   回调变成 no-op。

**坑**：在取消回调里做重活。回调在通知线程同步执行，
阻塞会拖住整个取消链（Context 的 `DrainCancellation` 是迭代的，
但单个回调慢仍会拖慢整批）。禁止在回调里 park。

**验证方法**：开 `-fsanitize=address` 跑 `tests/concurrency/test_channel.cpp`，
再用 `GO2CPP_DEBUG_LOCKS=1` 抓「回调里持锁」的误用。

## 4.3 场景 3：shutdown 卡住不返回

**症状**：`shutdown_for(1s)` 返回 false，再调一次还是 false，进程退不出。

**排查顺序**（按概率）：

| 步 | 检查 | 命令/入口 | 结论 |
|---|---|---|---|
| 1 | 是否还有非终态 G | `live_tasks_total()`（`scheduler.cpp` 内） | >0 说明有 G 卡住 |
| 2 | 哪个 G 卡在 Running | 注册表遍历 + `Task::state()` | Running 但无 M 执行 → orphan |
| 3 | 是否在等一个永远不会来的条件 | gdb 抓所有 worker 栈 | 看是否在 `park_for_scheduler` |
| 4 | 是否有人在 SpinMutex 里 park | `GO2CPP_DEBUG_LOCKS=1` | debug 下会 abort 并打印 |
| 5 | 是否 `max_workers` 用尽 | `machine_stats()` | 全是 Blocking → 补 M 失败 |

**最常见的两个真因**：

- **F12 注册表泄漏**：某个 G 在 `Running` 状态但对应 M 已经死了
  （例如用户代码 `std::terminate` 前没有走 run 的收尾）。自愈路径
  `recover_orphan_running`（`scheduler.cpp:1638`）只在 `run()` 正常返回时触发。
- **在 `no_park_depth != 0` 的临界区里 park**：release 下会降级而不是
  abort，表现为「任务偶尔卡死」；开 debug 立刻暴露。

**注意**：draining 期间 sysmon 仍在跑，这是故意的——长 syscall 的 M
需要被 detach 并补位，提前停 sysmon 会让 shutdown 永远等不到
`active_workers==0`。

## 4.4 默认行为表（不知道这些就会误判）

| 项 | 默认 | 出处 | 什么时候会坑你 |
|---|---|---|---|
| `collect_metrics` | **false** | `include/go2cpp/concurrency/scheduler/scheduler.hpp:240-308` | 想看指标发现全 0；打开后每任务多两次取钟 |
| `enable_sysmon` | true，10 ms | 同上 | 以为长 syscall 会自动补 M，其实只补「已声明」的 |
| `allow_idle_shrink` | false | 同上 | 空闲 M 不缩容，容器里 RSS 偏高 |
| `fiber_stack_growable` | true | 同上 | 非 Linux/非原生后端自动退化固定栈 |
| `wake_policy` | `kRunNext` | 同上 | 跨 P 唤醒多时局部性差，可换 `kLastMachine` |
| `local_queue_limit` | 256 | 同上 | 只约束 admission，不约束 incoming 条带 |
| `TimerService` | 单线程 | `timer.cpp` | 慢回调拖全局；每请求超时别塞这里 |
| `Context` 回调 | 同步、锁外 | `include/go2cpp/concurrency/context.hpp:25` | 回调里阻塞会拖慢取消链 |

## 4.5 可选策略（按场景选，不要默认全开）

| 场景 | 建议 | 理由 |
|---|---|---|
| 延迟敏感、任务短 | `wake_policy=kRunNext` + 小 `local_queue_limit` | 保持缓存局部性 |
| 跨 P 唤醒多（生产者/消费者分离） | `wake_policy=kLastMachine` | 减少 G 迁移 |
| 容器内、内存敏感 | `fiber_stack_size` 调小 + `allow_idle_shrink=true` | VA 与 RSS 都降 |
| 需要排障 | `collect_metrics=true` + `GO2CPP_DEBUG_LOCKS=1` | 指标 + 契约检查 |
| 纯计算任务 | 显式 `yield_current()`，别指望抢占 | 没有异步抢占 |
| 大量短定时器 | 用 `IOManager` deadline，不用业务 Timer | 单线程 Timer 是瓶颈 |

## 4.6 调试方法

**gdb**：卡死时 `thread apply all bt`，重点看三类栈——
`park_for_scheduler`（正常挂起）、`FutexMutex::lock`（等锁）、
`SpinMutex::Lock`（自旋中，通常是 bug）。

**日志/计数**：`machine_stats()` 给每台 M 的
`runnext_hits/inbox_hits/same_machine_resumes/cross_machine_resumes/
long_syscall_count`；`cross_machine_resumes` 突然升高说明亲和性失效。

**指标**：打开 `collect_metrics` 后看任务延迟分布；注意它本身有成本
（每任务两次取钟 + 跨核 RMW），压测时要关。

**Sanitizer**：ASan 抓 UAF（Channel 节点/Context 回调），
TSan 抓锁序（admission/条带/P 的反向嵌套）。

**契约检查**：`GO2CPP_DEBUG_LOCKS=1` 让 SpinMutex 的「禁止 park」
从 release 降级变成 abort；`GO2CPP_DEBUG_RESOURCES` 注册调试资源，
用于查泄漏。

## 4.7 第四层小结 & 下一层看什么

**小结**：这一层的三个场景分别对应「超时」「取消」「关闭」——
它们是并发原语最容易出错的三个边界。共同规律是：
**所有等待都必须有出口，所有出口都必须能区分原因**。

**下一层看什么**：既然默认值有成本（metrics、sysmon、栈预留），
第 5 层就用实测数字回答「这些成本具体是多少、值不值得关」。

---

# 第五层 · 性能与调优

> 数字口径：全部来自仓库内报告，标注了文件名与行号；**不是本文新测**。
> 换机器/换编译器必须重测，不要跨环境引用。

## 5.1 已知基线（引用）

| 指标 | 数值 | 出处 |
|---|---|---|
| 任务创建（spawn 全链） | 656–764 ns | `docs/reports/2026-11-01-asm-concurrency-review.md:6`（引 `docs/metrics-ledger.md` M16） |
| Channel 单次收发 | 10.73 ns | 同上 `:6` |
| Mutex 加解锁 | 3.75 ns | 同上 `:6` |
| Fiber 上下文切换 | 23.5 ns（物理下限 21 ns） | 同上 `:6` |
| `Scheduler::spawn` 指令数 | 194 条，栈帧 0x58 | 同上 `:14` |
| `Scheduler::enqueue` 指令数 | **789 条，栈帧 712 B** | 同上 `:15` |
| 优化后目标（A 档：编译选项+局部改写） | 560–600 ns | 同上 `:211` |
| 优化后目标（B 档：结构小改） | 470–520 ns | 同上 `:212` |

**判读**：Fiber 切换已经贴着物理下限（23.5 vs 21 ns），所以「优化协程切换」
没有意义；真正的差距在**每个任务要付一次堆分配、5 次引用计数、
2 套同步原语、148 个函数的栈保护**（同报告 `:202` 的结论）。

## 5.2 瓶颈在哪（按收益排序）

| 瓶颈 | 证据 | 收益估计 | 风险 |
|---|---|---|---|
| `enqueue` 789 指令 / 712 B 栈帧 | 反汇编统计（`:15`） | 15–25 ns/任务 | 低（拆分函数/减内联） |
| spawn 路径的堆分配 + 40 条清 0 | `:105` 的 `operator new` + 清 0 store | 10–15 ns/任务 | 中（对象池） |
| 引用计数 5 次 | `:202` | 结构改动 | 高（改所有权模型） |
| TLS 访问 `__tls_get_addr` | `:64`（park/wake 往返 6–10 次） | 10–20 ns/park | 低（initial-exec） |
| 栈保护 canary | `:38`（GCC 默认 `-fstack-protector-strong`） | 2–5 ns/任务 | 低（`-fno-stack-protector` 局部） |
| 可运行计数重复计算 4 次 | `:70` | 10–20 ns/wake | 低 |

**架构 > 汇编**：上表前两项是「结构」（分配/所有权），后两项是「指令」。
按仓库的性能标准，先把架构问题解决，再抠指令。

## 5.3 调参建议（按代价从低到高）

1. **关掉不需要的观测**：`collect_metrics=false`（默认就是 false，
   确认没被环境变量打开）；压测时尤其要关。
2. **栈参数**：`fiber_stack_size` 默认 128 KiB 提交、8 MiB 预留（2026-11-05 修正，旧值 2 KiB）；
   短任务多时把预留调小（例如 1 MiB）可显著降 VA 压力，
   但要接受「栈增长到上限就崩」。
3. **队列边界**：`local_queue_limit=256` 是实测最优点
   （`scheduler.cpp:2009` 附近注释：把窃取移出全局锁实测 -34%），
   不要凭直觉调大。
4. **wake_policy**：默认 `kRunNext`；跨 P 通信多时试 `kLastMachine`。
5. **worker 数**：`min_workers=0 → P`、`max_workers=0 → 2P`；
   IO 密集可上调 `max_workers`，CPU 密集保持默认。
6. **Timer**：业务定时器与 IO deadline 分开，别把每请求超时塞进
   `TimerService`（单线程）。

## 5.4 怎么验证调参有效

- 用仓库 bench（`bench_scheduler_task_opt`、`startup_cost_park_wake`），
  **绑核 + 多轮 + 报中位数**，不要单轮；
- 每次只改一个参数，记录「前/后 + 条件」；
- 指标看 p99 而不是均值——调度器的退化通常先体现在尾部；
- 反汇编确认改动生效（`objdump -d -C --no-show-raw-insn` 数指令/栈帧）。

## 5.5 第五层小结

**小结**：这个模块的性能已经「接近下限」的部分是 Fiber 切换与
Channel 快路径；差距在任务创建路径的分配与引用计数。调参能拿到
个位数到十几个百分点的收益，结构改动（对象池/所有权）才能拿到
数量级——但风险也高一个量级。

---

# 附：读完之后的自检清单

1. 能否画出「G → P → M → 内核」的完整路径，并指出每一步的锁？
2. 能否解释 `runnext` 为什么是单槽、为什么窃取者不偷它？
3. 能否说出一个 G 从 `kWaiting` 回到 `kRunning` 的完整链路
   （谁改状态、谁入队、谁 resume）？
4. 能否指出 Channel 的 D1/D2 两个历史缺陷分别修在哪一段？
5. 能否解释 Context 为什么必须「先标记完整子树、再执行回调」？
6. 能否解释为什么「状态 + 令牌」必须在同一个 32 位字里？
7. 能否在 10 分钟内用 bench 实测一次 spawn 并报出条件？

# 附-2：本文明确「未复跑」的地方

- 第 5 层的全部数字都来自报告引用，**本文没有重新压测**；
- `cross_machine_resumes` 等计数的实际分布未采样；
- 64 条带/16 条带在极端分布下的碰撞未压测；
- `fiber_stack_growable=false` 的性能差异未测；
- 所有结论以当前工作树为准，代码漂移后请先 `grep -n` 再引用。
