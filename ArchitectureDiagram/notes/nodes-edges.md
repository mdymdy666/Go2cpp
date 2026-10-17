# 架构图数据模型：节点与边清单

> 本文件把 G1~G9 九份报告末尾的「对架构图的贡献」节汇总成**可直接生成图的数据模型**。
> 生成物：`../00-architecture.dot`（Graphviz 源）→ `../00-architecture.svg`（最终成果）。
> 约定：节点 ID 用 `snake_case`；层号 L0（最底）→ L9（最上）；每条边都写清「谁调用谁、发生什么」。

---

## 1. 分层（自下而上）

| 层 | 名称 | 覆盖模块（报告） | 一句话 |
|---|---|---|---|
| **L0** | 平台 | 汇编后端、libc/pthread、Linux 内核 | 真正的执行力：上下文切换、系统调用、epoll |
| **L1** | 地基 | `context_backend`/`fiber`/`core::timer`/`parking_condition`（G1） | G 的载体 + 定时 + 等待分派 |
| **L2** | 调度 | `scheduler`（G2） | GMP：G/M/P、队列、窃取、sysmon |
| **L3** | 取消与错误 | `context`/`error`（G3） | 取消树、完成信号、不可变错误图 |
| **L4** | 并发原语 | `channel`/`future`/`sync`（G4） | Channel/Select/Future/混合锁 |
| **L5** | IO 后端 | `io`/`io_backend`/`fd_manager`/`hook`（G5） | epoll 等待、可替换后端、libc 拦截 |
| **L6** | 网络基础 | `socket`/`address`/`bytearray`/`framing`/`endian`/`socket_stream`（G6） | 字节与连接 |
| **L7** | 应用协议 | `http`/`websocket`/`sse`（G7） | 协议边界组件 |
| **L8** | 工具与新手层 | `control_flow`/`go`/`event`/`thread_policy`/`fiber_local`（G9） | 让转译代码写起来像 Go |
| **L9** | 横切设施 | `log`/`config`（G8） | 贯穿所有层 |

---

## 2. 节点清单

### L0 平台

| ID | 标签 | 说明 |
|---|---|---|
| `asm_ctx` | `context_x64.S：MakeContext/JumpContext` | 保存 rbp/rbx/r12-r15+rsp/rip，`jmp *%r8` 切换；entry 返回走 `_exit` |
| `boost_ctx` | `Boost.Context fallback` | 非 Linux-x86_64 平台的替代后端 |
| `kernel` | `Linux：epoll/eventfd/syscall` | epoll_wait、eventfd 唤醒、socket 系统调用 |
| `libc_pthread` | `libc + pthread` | `std::thread`、`std::mutex`、`condition_variable` |

### L1 地基

| ID | 标签 | 说明 |
|---|---|---|
| `context_backend` | `detail::context_backend` | `MakeContext/JumpContext/Transfer/Entry` 唯一平台依赖 |
| `fiber` | `Fiber（G 的执行栈）` | resume/Suspend/父链/取消标记；`DefaultStackSize=128 KiB` |
| `fiber_stack` | `FiberStack：mmap+guard` | 低地址 1 页 `PROT_NONE`；TLS≤8 + 全局≤128 缓存 |
| `fiber_record` | `FiberRecord：父链+墓碑` | `{parent, alive, state, depth, active_parent_id, execution}` |
| `fiber_bin` | `FiberBin：对象池` | 每 M 线程本地，默认 32 / 上限 4096 |
| `timer_service` | `TimerService（独立线程）` | multimap 队列 + generation；`TimerExecutor` 外置执行 |
| `parking_cond` | `ParkingCondition（三分支）` | 受管 G park / 原生线程 cv / 手动 Fiber 直接拒绝 |

### L2 调度

| ID | 标签 | 说明 |
|---|---|---|
| `scheduler` | `Scheduler（GMP）` | 入队/出队/窃取/park-wake/shutdown 的唯一入口 |
| `task_g` | `Task = G` | 状态机（New→Runnable→Running→…）+ 入队权/执行权 + pending 唤醒令牌 |
| `machine_m` | `Machine = M（std::thread）` | 绑定 P；可被 sysmon 逻辑 detach |
| `processor_p` | `Processor = P` | 本地 deque + attached 计数（P 从 0 编号） |
| `incoming_stripes` | `16 条 incoming 条带` | 外部生产者的分片入口 + 非空位图 |
| `global_queue` | `全局队列` | 本地队列满（256）后的溢出路径 |
| `sysmon` | `sysmon（受限监控）` | 只做阻塞记账 + 逻辑 P 脱离 + 扩容；**不切 C++ 栈** |
| `metrics` | `Metrics/Observer` | 累计计数（时序敏感）+ 生命周期事件回调 |

### L3 取消与错误

| ID | 标签 | 说明 |
|---|---|---|
| `context` | `Context（取消树）` | root 单例 + WithCancel/WithDeadline/WithTimeout/WithValue |
| `done_signal` | `DoneSignal` | 一次性关闭 + 两组回调（internal 先于 user）；可复制共享 |
| `context_key` | `ContextKey<T>` | 身份 = 堆地址；锚点防 ABA；Name 不参与匹配 |
| `context_rollback` | `ContextRollback + Savepoint` | LIFO 补偿；`RollbackDone` 与 `Done` 分离 |
| `error` | `Error（不可变错误图）` | String/Wrapped(链式 Message)/Join(多原因)；`Is` 按身份 |

### L4 并发原语

| ID | 标签 | 说明 |
|---|---|---|
| `channel` | `Channel<T>` | cap 0 rendezvous / >0 FIFO；缓冲 + 发送/接收等待队列 |
| `select` | `Select（轮转 + 单赢家）` | `SelectCase` + `SelectWaitState::TrySelectPair`；只接受可复制 T |
| `future` | `Future/Promise` | 一次性结果；**无 then 链**；默认等待后端 = ParkingCondition |
| `sync_prims` | `sync::Mutex/Cond/WaitGroup` | FIFO 混合队列；Go 风格所有权（可跨 Fiber/线程 Unlock） |
| `hybrid_gate` | `core::HybridGate` | 仅内部唤醒门；★ 用户已决策移除（X-35） |

### L5 IO 后端

| ID | 标签 | 说明 |
|---|---|---|
| `iomanager` | `IOManager`（内含 Scheduler） | 状态机：注册→deadline→park；单赢家 outcome |
| `io_poller` | `poller 线程` | **独立线程**（不在 M、不在 sysmon）；`expire_timers` |
| `io_backend` | `IOBackend` → `EpollBackend` | epoll_create1 + eventfd（token=0 保留）；ONESHOT |
| `desc_registry` | `DescriptorRegistry/Guard` | fd 代际 token 防 close/reuse ABA |
| `hook` | `Hook（31 个 libc 函数）` | `cooperative_io`：真调用 → EAGAIN → park |
| `fd_manager` | `FdManager` | fd 元数据（读侧无锁）；close/dup2 显式维护 |

### L6 网络基础

| ID | 标签 | 说明 |
|---|---|---|
| `socket` | `Socket（RAII fd）` | in-flight 保护 + 延迟关闭 + 读/写取消代际 |
| `socket_stream` | `SocketStream` | partial I/O 处理（read/write/write_all） |
| `address` | `Address 族` | IPv4/IPv6/Unix/Unknown；保留 getaddrinfo 顺序 |
| `byte_array` | `ByteArray` | 定长/变长/长度前缀 + iovec 出口；**默认小端** |
| `framer` | `Framer` | 分隔符/定长/大端 32 位长度；失败即清空 |
| `endian` | `endian 工具` | constexpr byteswap/host_to_be/be_to_host |

### L7 应用协议

| ID | 标签 | 说明 |
|---|---|---|
| `http` | `HTTP/1.x` | `Parser`（CL+chunked+trailer+EOF 定界）+ Client/Server/ResponseWriter |
| `websocket` | `WebSocket（RFC 6455）` | 一读多写双锁；分片重组；Ping/Pong 内部消化 |
| `sse` | `SSE` | 事件编码 + 增量解析（LF/CRLF/CR）+ 流写入 + 心跳注释 |

### L8 工具与新手层

| ID | 标签 | 说明 |
|---|---|---|
| `go_new` | `go() / after() / every()` | 默认调度器入口（进程级 `new` 且不析构） |
| `fiber_handle` | `fiber` 类 | 轻量任务句柄（不可拷贝 → 防重复提交） |
| `control_flow` | `defer / panic / recover` | 显式状态协议（无栈展开） |
| `event_batch` | `EventBatch` | SelectCase 集合 + 单消费者 `work()` + 协作式 stop |
| `thread_policy` | `thread_policy scope` | 外部线程参与 + 线程级 Hook 模式 |
| `fiber_local` | `FiberLocal<T>` | G 局部存储（全局 Registry 按 Fiber 指针隔离） |

### L9 横切设施

| ID | 标签 | 说明 |
|---|---|---|
| `log` | `Log（四层组合，同步）` | Filter × Formatter(Item 注册表) × Sink × Worker |
| `config` | `Config（两阶段事务）` | INI + `ConfigVar<T>`（无锁读）+ 锁文件热重载 |

---

## 3. 边清单（关键路径，含标签）

| # | 从 | 到 | 标签 / 语义 |
|---:|---|---|---|
| 1 | `go_new` | `scheduler` | `spawn()`（未运行则先 `start()`） |
| 2 | `fiber_handle` | `scheduler` | `bind()` → `add()` |
| 3 | `scheduler` | `task_g` | 持有 `shared_ptr<Task>`；入队/出队/状态推进 |
| 4 | `scheduler` | `machine_m` | 惰性创建 worker、join/reap |
| 5 | `scheduler` | `processor_p` | P 本地队列；`local_queue_limit=256` |
| 6 | `scheduler` | `incoming_stripes` / `global_queue` | 外部生产者入队 / 溢出 |
| 7 | `machine_m` | `fiber` | `resume_from_scheduler()`（execution claim 串行化） |
| 8 | `task_g` | `fiber` | 每 G 一个 `unique_ptr<Fiber>`；终态后 `RecycleForScheduler` |
| 9 | `fiber` | `context_backend` | `MakeContext` / `JumpContext` |
| 10 | `context_backend` | `asm_ctx` / `boost_ctx` | 真实寄存器切换 |
| 11 | `fiber` | `fiber_stack` | mmap + guard page + 两级缓存 |
| 12 | `fiber` | `fiber_record` | 父链绑定、深度、墓碑 |
| 13 | `task_g` | `fiber_bin` | 完成/失败后回收（`use_count()==1`） |
| 14 | `parking_cond` | `scheduler` | `park_wait()` / `wake_or_cancel()` |
| 15 | `parking_cond` | `fiber` | `SuspendForScheduler` 沿父链上抛 |
| 16 | `parking_cond` | `timer_service` | deadline → `Finish(kTimedOut)` |
| 17 | `channel` | `parking_cond` | 内嵌 `PendingSend/PendingRecv` |
| 18 | `select` | `channel` | `ArmSelectRecv/Send` + `TrySelectPair` 单赢家 |
| 19 | `sync_prims` | `parking_cond` | `WaitBackend::Park/ParkForCleanup/Wake` |
| 20 | `future` | `parking_cond` | 默认 `FutureWaitBackend` 包装 |
| 21 | `context` | `done_signal` | `Signal()` 恰好一次 |
| 22 | `context_rollback` | `done_signal` | internal 回调先跑（冻结 undo） |
| 23 | `context` | `timer_service` | deadline 到期 → `state->Cancel()` |
| 24 | `context` / `channel` / `sync_prims` | `error` | 状态码 + `ErrorPtr` |
| 25 | `iomanager` | `scheduler` | `park_io` / `wake_io` / `wake_io_batch` |
| 26 | `io_poller` | `io_backend` | `wait(output, capacity, timeout_ms)` |
| 27 | `io_backend` | `kernel` | `epoll_wait` + eventfd |
| 28 | `hook` | `iomanager` | `EAGAIN` → `wait_for_io` → park |
| 29 | `hook` | `fd_manager` | 用户/系统非阻塞与超时元数据 |
| 30 | `hook` | `desc_registry` | 真实 `close` 前 `NotifyClose`/`Invalidate` |
| 31 | `socket` | `iomanager` | `wait_ready`（≤100 ms 切片 + poll 回退） |
| 32 | `socket` | `parking_cond` | `close/release` 等待在途操作归零 |
| 33 | `http` | `socket` | 读写（`Client` 每次新建连接） |
| 34 | `websocket` | `http` / `socket` | 复用 Upgrade 校验；读/写各一把 `sync::Mutex` |
| 35 | `sse` | `http` / `context` | 复用 `ResponseWriter`；取消 → `cancel_write` |
| 36 | `sysmon` | `machine_m` / `processor_p` | 逻辑 detach + `maybe_grow`（不切栈） |
| 37 | `metrics` | `scheduler` | 累计计数 + 事件回调（时序敏感） |
| 38 | `fiber_local` | `fiber_record` | trampoline 结束 / `abandon_suspended` 时清理 |
| 39 | `thread_policy` | `hook` | 线程级 Hook 模式覆盖 |
| 40 | `control_flow` | `fiber` | `LastDeferException` / 共享 `PanicState` |
| 41 | `event_batch` | `select` / `channel` | `work()` 单消费者；`stop()` 协作式 |
| 42 | `log` | ← 全模块 | `GO2CPP_LOG_*`（默认级别 Warn，同步落盘） |
| 43 | `config` | `scheduler` / `log` | `BindRuntimeConfig` / `BindLoggingConfig`（结构参数不可热改） |
| 44 | `scheduler` | `timer_service` | `schedule_at/after/every` → `TimerExecutorAdapter` 提交为 Fiber |
| 45 | `libc_pthread` | ← `machine_m` / `parking_cond` / `sync_prims` | `std::thread` / `std::mutex` / 原生 cv |

---

## 4. 图内文字块（将直接画进 SVG）

### 文字块 A —— 一次 `go()` 的完整生命周期

```text
① go(fn) → Scheduler::spawn → Task(G) 入队（P 本地队列 / 16 条 incoming 条带 / 全局队列）
② worker(M) 取任务：救援链 → 本地队首 → 全局队列 → 轮转窃取其他 P 的队尾
③ Fiber::resume_from_scheduler 切入独立栈（128 KiB + guard page），G 状态 Running
④ 等待发生时统一进 ParkingCondition：受管 G park（让出 M）/ 普通线程原生 cv / 手动 Fiber 直接失败
⑤ 唤醒来源（对端 channel、timer 到期、epoll readiness、context 取消）→ WaitNode 原子单赢家
   → Scheduler::wake_* → G 重新入队
⑥ 主体返回 → Fiber Completed → Task 终态 → notify_terminal 唤醒 wait() 者 → FiberBin 回收
```

### 文字块 B —— 五条不可违反的协议

```text
· 取消：先整棵子树标记（摘 timer、摘 parent 登记、搬走 before-done 回调），
        再逆序（子→父）线性化 DoneSignal —— 父的 Done 回调里同步等子不会死锁；
        补偿是否跑完看 RollbackDone()，不是 Done()。
· 错误：ErrorPtr 是不可变共享值；Wrap 的 Message 是整条链（"outer: inner: root"），
        所以种类/层级判定一律用身份比较 Is(err, XxxError())。
· IO：先注册 readiness → 再发布 deadline → 最后 park；readiness/timeout/cancel/close
      通过一个原子 outcome claim 决出唯一赢家，然后以相同绝对 deadline 重试 syscall。
· fd：数字 fd 会复用（ABA）→ 用 DescriptorToken 代际 + Hook 在真实 close 前 NotifyClose。
· 栈：固定 128 KiB 且不增长；越界 = SIGSEGV = 进程立即死亡（无 handler、无 sigaltstack）。
```

### 文字块 C —— 关键数字（本工作区实测）

```text
Fiber 默认栈 128 KiB；可承受递归 ≈ 栈容量 / (帧字节 + 128)
  实测：128 KiB 帧 0 → 1021 层；帧 256 B → 339 层；8 MiB pthread → 65496 层
G/M 编号从 1 开始（0 = 「本线程不属于任何 M/P」哨兵）；P 从 0 开始
P 数 = processor_count（0 → hardware_concurrency）；M 数 ≈ P，声明阻塞区时可 > P
incoming 条带 16 条；本地队列上限 256；FiberBin 32（上限 4096）；栈缓存 TLS 8 + 全局 128
Hook 拦截 31 个 libc 函数；epoll token 0 保留给 eventfd 唤醒
```

### 文字块 D —— 明确边界（不做的事）

```text
无 GC、无异步抢占、无 morestack/copystack 自动栈增长、无 cgo
无 HTTP/3；HTTP/2 与 TLS 已由 go2cpp::net::http2 提供；协议层不做路由/鉴权/连接池/服务发现
panic/recover 不是栈展开（显式状态协议）；defer 是作用域级而非函数级
未 Hook 的阻塞调用调度器看不见 —— 必须显式 BlockingRegion 声明
poll/select/epoll_wait 不被 park；IOManager 的 wait_any/wait_many 仅限受管 Fiber
```

---

## 5. 图例

| 视觉 | 含义 |
|---|---|
| 实线箭头 | 调用 / 数据流方向 |
| 虚线箭头 | 横切依赖（日志、错误、注册表） |
| 加粗边框节点 | 用户最常直接使用的入口（`go()`/`Channel`/`Context`/`Socket`） |
| 灰色节点 | 平台/内核（不由本项目实现） |
| 点线边框 | 内部设施（非稳定公共 API） |
