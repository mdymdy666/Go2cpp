# Go2Cpp 代码导读（审查路线图）

> 基线：`bd99603`（2026-10-20 14:19，47 文件修复提交）。本文所有行号按该提交的源码标注。
> 注意：审查期间工作树出现了 **19 处未提交改动**（`src/os/*`、`src/config/config.cpp`、`sync.cpp`、
> `http2/*`、`http3/*`、`quic/*`、`tests/*`，仍在变化），行号可能与 `git diff` 后的文件相差几行；
> 阅读并发/IO/HTTP2 实现时以 `git show bd99603:<path>` 为准。
>
> 本文回答四个问题：**代码分几层、一个请求怎么走、关键机制在哪、按什么顺序读**。
> 审查发现与逐条证据见 `/tmp/review_findings.md`（本文件只在第 6、7 节给索引）。

---

## 1. 项目速览

- 定位：C++17 运行时/库，实现 Go 运行时语义子集（不是 Go 转译器）。公共 API 在 `go2cpp` 命名空间。
- 规模：`src/` 约 **39.2k 行**，`include/` 约 **35.0k 行**；其中并发 12.9k、网络 14.4k（含 HTTP2/HTTP3/SSE/WS/TLS）。
- 构建：CMake，默认共享库 + Linux Hook；模块 target 20 余个（`cmake/Go2CppModules.cmake`）。
  测试 42 个 `add_test` / 当前文档记录 116 个 ctest 用例（`cmake/Go2CppTests.cmake`）。
- 运行环境假设：**Linux x86_64**（自有汇编上下文）、pthread、epoll/eventfd；其他平台回退 Boost.Context。

### 与 Go 的根本差异（决定所有设计约束）

| 维度 | Go | Go2Cpp |
|---|---|---|
| 调度 | 抢占式 | **纯协作式**，只在 park/yield/hook IO 点切换 |
| 栈 | 可增长（复制搬迁） | 8MiB 预留 + 初始 1 页 + **SIGSEGV 按需提交**（地址不变，不复制） |
| 锁 | `sync.Mutex` 可跨 goroutine 解锁 | 相同（无 owner），因此不能换成 `std::mutex` |
| channel | hchan | 预分配环形缓冲 + `sendx/recvx`，等待队列独立 |

---

## 2. 模块地图

### 2.1 目录 → 模块 → target

| 目录 | 职责 | CMake target | 真实定义位置 |
|---|---|---|---|
| `src/concurrency/` | Scheduler（G/M/P）、Fiber/栈、Context 取消树、Channel、Mutex/CV/WaitGroup、Future、Timer | `go2cpp_scheduler`、`go2cpp_fiber`、`go2cpp_context`、`go2cpp_channel`、`go2cpp_sync`、`go2cpp_future` | `include/go2cpp/concurrency/` |
| `src/network/` | IOManager/epoll、FD 管理、Socket、Address、ByteArray、HTTP/1.1、SSE、WebSocket、HTTP2、HTTP3/QUIC、TLS | `go2cpp_io`、`go2cpp_network`；Hook 单独 `go2cpp_hook`（仅 Linux 共享库） | `include/go2cpp/network/` |
| `src/tools/` | 错误链、defer/panic/recover、SelectLoop/EventBatch、默认调度器入口 | `go2cpp_error`、`go2cpp_control_flow` | `include/go2cpp/{error,control_flow,event,go}.hpp` |
| `src/config/` | INI、`ConfigVar<T>`、热加载事务、SO manifest/reloader | `go2cpp_config` | `include/go2cpp/config/` |
| `src/os/` | File/Dir/环境变量/Args（裸 syscall 绕过 Hook，仅接受普通文件） | `go2cpp_os` | `include/go2cpp/os/` |
| `src/logging/` | Logger/Formatter/Sink/滚动文件 | `go2cpp_log` | `include/go2cpp/logging/` |
| `src/metrics/` | Counter/Gauge/Histogram + Prometheus 文本 | `go2cpp_metrics` | `include/go2cpp/metrics/` |
| `src/math/`、`src/hash/`、`src/encoding/`、`src/compress/`、`src/path/`、`src/container/`、`src/bufio/` | Go 标准库对齐（math/rand、md5/sha、base64/hex/binary/csv、flate/gzip/zlib 薄封装、path/filepath、heap/list/ring、bufio） | 各自 target | 同名子目录 |
| 仅头文件 | `debug`（只读观察）、`json`、`container` 模板、`bytes/fmt` | 无独立 target | `include/go2cpp/` |

### 2.2 核心文件与行数（按审查优先级）

| 文件 | 行数 | 一句话 |
|---|---:|---|
| `src/concurrency/scheduler.cpp` | 4199 | GMP 全部状态机、队列、park/wake、sysmon、shutdown |
| `src/network/hook.cpp` | 2832 | `read/write/connect/accept/sleep/poll/...` 的 C ABI interposer |
| `src/network/io.cpp` | 2801 | IOManager：FdSlot 条带、WaitNode、epoll、deadline、批量唤醒 |
| `src/concurrency/context.cpp` | 2103 | 取消树、Done 回调、deadline/value Context、Rollback |
| `src/concurrency/sync.cpp` | 1967 | Mutex 两态、ConditionVariable、WaitGroup、WaitNode/Await |
| `src/concurrency/fiber.cpp` | 1785 | Fiber 状态机、上下文切换、growable 栈、FiberBin |
| `src/network/http2/connection.cpp` | 1163 | HTTP2 连接层：握手、SETTINGS、HPACK 接线、流控、帧分发 |
| `src/network/http3/client.cpp` | 850 | picoquic 事件循环、HQ/h3 流、请求/响应 |
| `src/network/websocket.cpp` | 834 | 握手、帧读写、分片、控制帧 |
| `src/network/sse.cpp` | 808 | SSE 增量 Parser（读游标）+ Stream 写侧 |
| `src/network/http2/hpack.cpp` | 786 | HPACK 动态表/霍夫曼/整数编码 |
| `src/network/http.cpp` | 717 | HTTP/1.1 客户端与服务端 keep-alive |
| `src/network/socket.cpp` | 684 | 非阻塞 socket + 三种等待策略 |
| `src/concurrency/parking_condition.cpp` | 678 | 同步原语的统一等待后端（futex/per-node/futex_waitv） |
| `src/concurrency/channel.cpp` | 534 | Channel/Select 的运行时部分 |
| `include/go2cpp/concurrency/channel.hpp` | 2744 | Channel/Select 模板主体（读这个比读 .cpp 更重要） |
| `include/go2cpp/concurrency/scheduler/scheduler.hpp` | 897 | Task/Scheduler 公开契约与状态位 |

---

## 3. 核心数据流

### 3.1 `go()`：创建一个 G

```
go2cpp::go(fn)                         include/go2cpp/go.hpp
  └─ Scheduler::spawn(fn, options)      scheduler.cpp:2317
       ├─ 创建 Task（G），绑定 owner_token / cancellation_gate
       └─ enqueue(task)                scheduler.cpp:2358
            ├─ prepare_enqueue：kNew→kRunnable，占 m_queued
            ├─ 首次注册进 16 条带 registry（强持有到终态）
            ├─ 非 worker：投递到 16 条带 incoming 队列（scheduler.cpp:2459）
            └─ maybe_grow() / notify_worker()（scheduler.cpp:1391）
worker_loop（scheduler.cpp:3472）
  ├─ 快路径：本 P 本地队列（仅当 runnable > P 数才启用，:3507）
  ├─ 中路径：incoming 条带（:3530）
  └─ 慢路径：admission mutex 下 emergency→本地→全局→窃取（:3630）
       └─ try_mark_running()：m_queued=false + m_execution_claim=true + kRunnable→kRunning
```

**G 的三个位**（`scheduler.hpp:483` 附近，`codex.md §1.1`）：
`m_queued`（在队列里）、`m_execution_claim`（拥有执行权）、`m_run_claim`（正在 `run()` 栈帧内）。
三者必须在 `m_transition_mutex` 下一起翻转；`run()` 入口的 `run_claim` CAS 失败分支
（`scheduler.cpp:813-827`）是历史挂死事故的修复点，改动时不要拆开。

### 3.2 一次阻塞 `read()` 的完整路径（最重要的一条）

```
用户代码：read(fd, buf, n)
  │  Hook 符号（LD_PRELOAD / 共享库 interposer）
  ▼
hook.cpp: cooperative_io<ssize_t>(fd, real_read, IOEvent::kRead, ...)   hook.cpp:1082
  ├─ step1 解析 real 符号；无则 ENOSYS
  ├─ step2 descriptor_for(fd)；未接管则 try_adopt_socket / try_adopt_fifo
  │         （getsockopt(SO_TYPE) 认 socket；fstat S_ISFIFO 认 pipe）
  ├─ step3 wait_decision()：是否可 park（fd 已设 O_NONBLOCK、无 MSG_DONTWAIT、有 deadline）
  ├─ step4 持 DescriptorGuard 调 real_read
  │         ├─ 成功/非 EAGAIN → 直接返回（不 park）
  │         └─ EAGAIN → step6
  ├─ step6 IOManager::wait(fd, kRead, deadline)        io.cpp:2165
  │     ├─ FdSlot 条带锁（64 条带，fd & 63）           io.cpp:2044
  │     ├─ 创建/复用 WaitNode（TLS 缓存）              io.cpp:319
  │     ├─ 入 readers 队列，EPOLL_CTL_MOD/ADD（oneshot）
  │     └─ Scheduler::park_io(task)                    scheduler.cpp:2789
  │           └─ Task::park_for_scheduler()：kRunning→kWaiting（transition_mutex）
  │           └─ Fiber::SuspendForScheduler(Io)：切回 worker
  ▼
worker 空出，去跑别的 G
  ▼
epoll_wait 返回（worker 自己 poll 或独立 poller）
  └─ IOManager::State::process_events_batch()          io.cpp:1577
       ├─ 按 fd 取条带锁 → process_event_locked()      io.cpp:1450
       ├─ take_ready_locked()：claim() 一次性认领       io.cpp:1428
       └─ wake_nodes()：批量按 P 分桶 → wake_io_batch() io.cpp:1902 / scheduler.cpp:2976
            └─ wake_for_io：kWaiting→kRunnable，入 P 队列
  ▼
worker 再次调度该 G：Task::run() → Fiber::resume_from_scheduler()
  └─ read() 循环回到 step4 重试 real_read（不假设“唤醒=有数据”）
```

三条不变量（违反就是“永久 park”，无崩溃无超时）：

1. **先注册 readiness，再发布 deadline timer，最后 park**（顺序不能换）。
2. readiness / timeout / cancel / close 通过 `WaitNode::claim()` 的 CAS **只能有一个赢家**（`io.cpp:339`）。
3. `close()` 的终止性排空必须遍历**整个**队列，不能遇到忙节点就 `break`（`io.cpp:1372`，审计 3.6 修复点）。

### 3.3 一次 HTTP/1.1 请求（服务端）

```
Socket::accept（hook accept → wait_ready(kRead) → IOManager）
  └─ http::Server::serve_connection(conn, max_requests)     http.cpp:687
       ├─ 循环：read_request（按 \r\n\r\n 与 Content-Length/chunked 定界）
       ├─ handler(request) → response
       ├─ write_response（keep-alive / Connection: close）
       └─ 下一轮或关闭
HTTP/2：Transport（Memory/Fiber/TLS 三实现）
  └─ http2::Connection::Handshake() → ReadEvent() 循环 → StreamStateMachine
```

HTTP2 分层：`HPACK(+Huffman)` → `帧层 frame.cpp` → `流状态机 stream_state.cpp` →
`连接层 connection.cpp` → `Transport`（`transport.hpp` / `fiber_transport.hpp` / `tls_transport.cpp`）。
入口：`Handshake()` `connection.cpp:269`、`StartRequest()` `:396`、`ReadEvent()` `:835`、
`ApplyPeerSettings()` `:587`。

HTTP/3：`quic::PicoquicConnection`（薄封装）→ `http3::Client::Impl::Callback`
（`client.cpp:630`）→ `HandleEvent/OnStreamBytes/HandleControlStream` → `Send/Receive`
（`client.cpp:1079/1165`）。默认 `GO2CPP_WITH_PICOQUIC=OFF`。

### 3.4 通用 park/wake 模型（所有同步原语共用）

```
等待侧                                     唤醒侧
WaitNode::Arm()                            Unlock/Notify/timer/epoll
  ↓ 发布节点                                   ↓ claim（CAS 一次性）
Scheduler::park_*()                        Task::wake_for_wait/io()
  ↓ kRunning→kWaiting                          ↓ kWaiting→kRunnable
Fiber::SuspendForScheduler()                Scheduler 入队 + notify_worker
  ↓ 切回 worker                                ↓
（M 空闲/去跑别的 G）                         worker 取到 G → resume 同一 continuation
```

关键点：**通知可能先于 park**。`park_for_scheduler()` 在 `m_transition_mutex` 内消费
`m_wake_pending`（`scheduler.cpp:598`）；若已置位则返回 `kRequeued`，不切栈。
`Await()`（`sync.cpp:721`）对伪唤醒是循环重试，不会把“被叫醒”误判成“取消”。

---

## 4. 关键机制

### 4.1 GMP 调度器（`scheduler.cpp` + `scheduler.hpp`）

| 概念 | C++ 对象 | 要点 |
|---|---|---|
| G | `scheduler::Task` | 首次接纳的 Scheduler 永久拥有；非终态由 16 条带 registry 强持有 |
| M | `Impl::Machine` | 一个 `std::thread` + 首选 P；`replacement` 标记补充 M |
| P | `Impl::Processor` | 有界本地 `deque` + `active_machines/running_machines` |

- **队列拓扑**：P 本地 FIFO（上限 `local_queue_limit`，溢出到全局）→ 全局 FIFO →
  受害者 P **队尾**窃取（`take_victim`，`scheduler.cpp:3695`）。另有 16 条带
  `incoming`（外部生产者）与 `emergency` 侵入式救援链。
- **锁层次**（`scheduler.cpp:1005`）：`m_lifecycle_mutex → m_mutex → 条带锁 → P.mutex
  → Task::m_transition_mutex`，任何路径不得反向。
- **动态 M**：基础 M 常驻；`BlockingRegion`/Hook 发布 `MState::kBlocking`，sysmon
  （`sysmon_pass` `:1482`）超过 `long_syscall_threshold` 后逻辑 detach 并申请补充 M；
  `blocking_workers==0` 时补充 M 立即退休（`worker_loop:4132`）。
- **shutdown**：关闭 admission → 取消未启动 G → 唤醒已启动等待 G → 等 registry 全终态
  → owner join（`shutdown_for` `:1991`）。`shutdown_for` 超时只返回 false，**不丢栈**。
- **观察者**：`SchedulerObserver::OnEvent` 在锁外调用，异常被隔离（`emit_event` `:1192`）。

### 4.2 Fiber 与 growable 栈（`fiber.cpp`、`context_x64.S`）

- **上下文布局**（`context_x64.S`）：
  - `go2cpp_context_make`（:19）：在栈顶下 56 字节布置 `r12..rbp` + `entry/r8` 槽；
    entry 由 `rbx` 携带，`rbp` 槽预置 `go2cpp_context_end`——`context_entry` 的
    `pushq %rbp` 把它变成 entry 的返回地址（防 entry 返回后踩空栈）。
  - `go2cpp_context_jump`（:52）：push `rbp,rbx,r15,r14,r13,r12` → 保存 `rsp` 到 rax
    （即 caller context）→ 切到目标栈 → pop 6 个 + `r8` → `jmp *r8`。
  - 返回时 `rax=对方保存的 context`、`rdx=data`，正好是 `Transfer{context,data}` 的 SysV 返回。
  - `go2cpp_context_entry` 把 `rax`（caller context）放进 `rdi`，于是 `Impl::entry(Transfer)`
    收到 `transfer.context=caller`、`transfer.data=this`（`fiber.cpp:1106-1108`）。
- **状态机**：`Ready → Running → Suspended → Running → Completed/Failed`；
  `resume_locked`（:1279）是唯一切换点，保存/恢复 errno，维护 `s_current_fiber` 与
  growable TLS；`suspend`（:1426）只允许 Running 且父链可恢复。
- **growable 栈**（`fiber.cpp:480-914`）：
  - `mmap(reserve, PROT_NONE, MAP_NORESERVE)`，`top=base+reserve`，只把顶部
    `initial_usable` 改 RW；`committed` 向下增长，**栈地址不变**（所以局部变量指针不失效）。
  - SIGSEGV handler（:828）判定：故障在 `[base, committed)`、`rsp` 在 `[base, top)`、
    两者距离 ≤256KiB、剩余可提交 ≥64KiB；向下 64KiB `mprotect(RW)` 后**直接 return**。
  - 只在 Linux x86_64 + 原生上下文 + 非 ASan/TSan + `GO2CPP_GROWABLE_STACK != 0` 时启用；
    否则退化固定栈（usable + 1 页 guard）。
  - 池化键 `(initial_usable, reserve)`；复用前把增长区重新 `PROT_NONE` 并重置 committed（:701）。

### 4.3 同步原语（`sync.cpp`、`parking_condition.cpp`、`channel`）

- **`sync::Mutex` 两态**（`sync.cpp:1007-1101`，`LockImpl:1181`，`Unlock:1559`）：
  状态字节四位 `kLocked/kWaiter/kStarving/kWoken`。
  - 正常模式：`Unlock` 清 `kLocked` 再唤醒队首；新来者可 CAS 插队；`kWoken` 保证
    “同一时刻最多一个被唤醒者”，避免每次 unlock 一次 futex wake。
  - 饥饿模式：等待者入队时累计等待 >1ms 置 `kStarving`；`Unlock` **不释放锁**，
    直接把所有权交给 FIFO 队首（`TryFinishAndGrant`），新来者不得插队。
  - 等待者超时/取消时若持有 `kWoken`，必须 `DropWakeToken()`（:1039）补交接，否则丢唤醒。
- **ConditionVariable**（`sync.cpp:1689`）：独立通知队列；`Wait` 先入队再 `mutex.Unlock()`，
  `Await` 返回后 `LockForConditionWait()` 重锁；返回值为 `relocked && notified`。
- **WaitGroup**：计数 + CV 语义（`sync.cpp` 后段）。
- **ParkingCondition**（`parking_condition.cpp`）：所有等待的统一后端。
  - `WaitNode` 四态（`kWaiting→kNotified/kTimedOut/kCancelled`，CAS 一次性，:202）。
  - native 线程：per-node `futex_seq` + 共享 `m_broadcast.seq` 双字等待
    （`futex_waitv`，Linux 5.16+；否则逐节点 FUTEX_WAIT/WAKE）。
  - managed G：`WaitManaged()` → `Scheduler::park_wait`，由 `Wake()` 调 `wake_or_cancel`。
  - `Disarm` 门（`m_wake_mutex` + `m_active`）保证等待者栈回收后不会再有回调访问 Scheduler。
- **Channel**（`channel.hpp:820`，`channel.cpp`）：
  - 缓冲：预分配数组 + `sendx/recvx/dataqsiz`，`cap==0` 不分配；等待队列独立。
  - `Select`（`channel.cpp:156`）：随机起点探测 → 逐个 arm → `SelectWaitState::Take`
    一次性提交 → 挂起 → `disarm` 全回滚。取消分支由 `DoneCase.cancel_probe`
    在“探测后/回滚后”两个窗口补一次（审计 P1 #8 的修复，`:205-234/463-489`）。

### 4.4 IO / epoll（`io.cpp`、`io_backend.cpp`、`fd_manager.cpp`）

- **分层**：`IOManager`（门面）→ `IOManager::State`（全部状态）→ `IOBackend`（epoll 实现）
  → `FdSlot` 条带表。`State` 把 fd 按 `fd & 63` 分到 64 个 `SlotStripe`，每条纹带一把
  `std::mutex`；只有 epoll_ctl 的“注册表”用 `m_epoll_mutex`（高并发 try_lock）。
- **FdSlot**（`io.cpp:590`）：`readers/writers` 两个 FIFO；`interest` 是队列的精确投影；
  `pending_read/pending_write` 记录“事件已到但无人等”；`generation` 与 `DescriptorToken`
  对齐，拒绝 close/dup2 后的迟到事件。
- **WaitNode**（`io.cpp:319`）：`outcome` CAS 一次性 claim；`operation_state` 四态
  （0 空闲/1 活动/2 停止中/3 已停止）保护回收；`timer_armed` 让无 deadline 的等待
  不碰全局定时器表。
- **唤醒批处理**：`wake_nodes`（:1902）把节点按 `last_processor_id` 排序后交给
  `Scheduler::wake_io_batch`（`scheduler.cpp:2976`），减少每个事件一次 P 队列锁；
  `end_operation` 在唤醒后统一执行。
- **tickle**：`eventfd` 计数语义，`m_tickle_pending` 的 0→1 才写，drain 用 exchange(0)，
  避免“布尔合并 + drain 竞态”吞最后一次唤醒（`io.cpp:1783`）。
- **worker 自己 poll**：空闲 worker 直接 `poll_once`（:1856），事件在本线程处理并
  `wake_io` 回本 P，消除“poller 线程→条件变量→worker”的跨线程 handoff。

### 4.5 网络协议栈

| 协议 | 入口 | 机制要点 |
|---|---|---|
| HTTP/1.1 | `http.cpp:499` `Client::do_request`；`:687` `Server::serve_connection` | keep-alive 循环、Content-Length/chunked、请求行/头解析 |
| HTTP/2 | `connection.cpp:269/396/835` | 帧层 `frame.cpp`；HPACK 动态表 + Huffman（`hpack.cpp`）；流状态机 `stream_state.cpp`；SETTINGS/GOAWAY/流控 |
| TLS | `tls_transport.cpp:50` `CreateClient`、`:208` `Handshake`、`:280/319` Read/Write | OpenSSL 非阻塞 BIO + `WaitFor` 接 IOManager；CA/主机名校验 |
| HTTP/3 | `http3/client.cpp:995` Connect、`:1079` Send、`:1165` Receive | picoquic 回调驱动；QPACK 控制流；`PicoquicConnection` 负责 close/verify |
| SSE | `sse.cpp:320` `Parser::feed`、`:256` `consume_line`、`:214` `dispatch` | 增量解析 + 读游标（不再每行 `erase(0)`） |
| WebSocket | `websocket.cpp:259` Accept、`:306` Connect、`:480` read_frame、`:673` read_message、`:624` send_message | 握手校验、掩码、控制帧、入站分片重组、**出站默认分片** |

---

## 5. 阅读顺序建议

### 路线 A：先建立“协程如何切换”的直觉（1~2 天）

1. `include/go2cpp/concurrency/detail/context_backend.hpp`（65 行）——只看 `Transfer` 与两个函数。
2. `src/concurrency/context_x64.S`（83 行）——对照 `fiber.cpp:1099-1150` 的 `entry` 读。
3. `src/concurrency/fiber.cpp:512-914`（FiberStack + growable handler）。
4. `src/concurrency/fiber.cpp:1279-1460`（`resume_locked`/`suspend`）。
5. `docs/fiber-growable-stack-design.md` + `codex.md §18`（设计取舍与已知限制）。

### 路线 B：调度器（2~3 天）

1. `scheduler.hpp:361-566`（Task 公开契约、三个位、状态枚举）。
2. `scheduler.cpp:365-988`（try_mark_queued / wake_for_* / park_for_scheduler / run）。
3. `scheduler.cpp:3472-4185`（worker_loop 六步；重点 3507-3526 快路径、3630-3769 慢路径、
   3883-3928 自愈、3995-4050 IO 空闲 poll）。
4. `scheduler.cpp:2358-2710`（enqueue / requeue_from_worker）。
5. `scheduler.cpp:1391-1578`（maybe_grow / sysmon）+ `docs/scheduler.md`。
6. 带着问题读：`run_claim` 为什么必须与 `execution_claim` 同临界区？`wake_io_batch`
   为什么可以跳过 `owned_by`？`worker_loop` 的 notify 条件为什么排除 yield/park？

### 路线 C：IO 与 hook（2~3 天）

1. `io.cpp:271-520`（State 组成、WaitNode 状态机）。
2. `io.cpp:590-760`（FdSlot 字段与 generation）。
3. `io.cpp:1080-1420`（register_wait / complete / drain_queue_locked）。
4. `io.cpp:1450-1660`（process_event_locked / expire_timers）。
5. `io.cpp:1849-2000`（poll_once / wake_nodes / finish_fd）。
6. `hook.cpp:1082-1162`（cooperative_io 六步）+ `:2198-2330`（read/write 薄壳）。
7. `socket.cpp:417-520`（wait_ready 三种策略）+ `docs/hook-blocking-policy.md`。

### 路线 D：同步与 channel（2~3 天）

1. `sync.cpp:297-830`（WaitNode / Await / 选择器重排）。
2. `sync.cpp:1007-1101`（Mutex 状态字与 DropWakeToken）。
3. `sync.cpp:1181-1662`（LockImpl / Unlock 两态）。
4. `parking_condition.cpp:202-415`（native/managed 两条等待路径）+ `:478-676`。
5. `channel.hpp:820-1500`（Channel 主体）+ `channel.cpp:156-532`（Select）。
6. `codex.md §2.1-§2.8`（锁契约与历史事故）。

### 路线 E：网络协议（按需）

1. `http2/frame.cpp:236-560`（帧编解码）→ `hpack.cpp`（Decoder/Encoder）。
2. `stream_state.cpp:49-267`（状态机，配 `docs/state_machine_audit_2026-10-18.md`）。
3. `connection.cpp:269-835`（握手/SETTINGS/写帧）→ `:835-1163`（ReadEvent）。
4. `tls_transport.cpp:312-460`。
5. `http3/client.cpp:508-1000`（Impl/Callback/OnStreamBytes）→ `picoquic_connection.cpp`。
6. `sse.cpp:320-430`、`websocket.cpp:480-760`。

### 路线 F：外围模块（各半天）

`context.cpp:650/1902/1921`（取消树）→ `timer.cpp:230/816`（TimerService）→
`config/config.cpp`（ConfigVar 事务）→ `so_manifest.cpp:173/365/580` + `so_reloader.cpp:61`
→ `os/file.cpp:199/501`、`os/dir.cpp:236` → `metrics/metrics.cpp`、`logging/log.cpp:694`。

---

## 6. 已知坑（读代码前必须知道）

来自 `codex.md`（真实事故）与本次审查：

1. **park 有两套等待记录**：锁等 `Mutex::Impl::m_waiters`，IO 等 `FdSlot::readers/writers`。
   合并会“一方漏唤醒、另一方永远等”（`codex.md §1.2`）。
2. **丢唤醒没有崩溃/超时**：表现为所有 worker 空闲、队列空、某个 G 永不终态。
   排查先看 `Scheduler::shutdown_for → ParkingCondition::WaitOnce`（`codex.md §1.4`）。
3. **`m_execution_claim` 与 `m_run_claim` 必须同临界区翻转**（`scheduler.cpp:970-976`）。
4. **锁分两类**：`sync::Mutex` 临界区可以 park；`SpinMutex` 临界区**禁止** park
   （debug abort / release 降级 + 计数）。检测点在 `park_for_scheduler` 入口（`scheduler.cpp:576`）。
5. **`FutexMutex` 只能当内部门锁**，不能替代 `sync::Mutex`（会睡死 worker）。
6. **`Timer` 句柄析构 = Cancel**：`auto t = ScheduleAfter(...)` 循环一次就取消，回调不会跑。
7. **`DoneSignal` 先 notify 再跑回调**：`Done()` 返回不代表 undo 已完成。
8. **等待队列不能存裸 `WaitNode*`**：必须 `shared_ptr` + generation。
9. **`Context::Now()` 可注入，TimerService 硬编码 steady_clock**：假时间仍会真等。
10. **`Duration::max()` 不饱和会回绕**：`now + max` 变成过去，永久 Context 立即超时。
11. **growable 栈 handler 是进程级覆盖**：宿主自己的 SIGSEGV handler 会被替换；
    大帧 >256KiB / alloca 超窗口会 fatal；ASan/TSan 下退化固定栈。
12. **HTTP/2 HPACK 解码有解压炸弹闸门**（`connection.cpp:886-899`），不要只按帧长限流。
13. **Hook 只对“可归约为 fd 就绪”的调用 park**；`poll/select/epoll_wait/open/
    getaddrinfo` 明确不 hook（`codex.md §3.1`）。
14. **`os::File` 只接受 `S_ISREG`**：Read/Write 走裸 syscall，放行 FIFO/eventfd 会卡死 worker。
15. **`worker_loop` 的窄窗口不能加自旋/预判**：历史五次尝试全部更慢（`codex.md §1.7`）。

---

## 7. 文档与代码不符（截至 bd99603）

| 文档 | 说法 | 实际 |
|---|---|---|
| `docs/design.md:221` | “当前没有 Go morestack/copystack 式自动扩容” | 已实现 8MiB 预留 + SIGSEGV 按需提交（`fiber.cpp:480-914`），只是不复制栈 |
| `codex.md:213` | 可增长栈 ❌ | 同上，已实现 |
| `codex.md:761` | Fiber 路径丢唤醒“根因未定位” | HEAD 已修（`scheduler.cpp:376-393` 双队列节点线性化） |
| `docs/module-audit-2026-10-20.md` | TLS 不在发布物；OS NUL/ReadFile/RemoveAll 未修 | TLS 已进 `go2cpp_network`（`Go2CppModules.cmake:43-54`）；OS 项在 HEAD 仍未修，但工作树正在修 |
| `docs/module-audit-2026-10-20.md` | HTTP3 5s 超时失效 / 空指针 / verify_peer 无效 | HEAD 已修（`client.cpp:677-690/782-789`、`picoquic_connection.cpp:243/282-293`） |
| `docs/module-audit-2026-10-20.md` | SSE Parser O(n²)、WS 无发送分片 | HEAD 已修（`sse.cpp:351` 读游标；`websocket.cpp:754` 出站分片） |
| `docs/quic.md:37-58/86-102` | 自研 varint/包头/帧层、94/94 测试 | 已删除，仅剩 picoquic 薄封装（`docs/quic.md` 顶部已订正，正文未改） |
| `docs/modules.md:19` | “13 个模块 target” | 现为 20+（metrics/bufio/path/container/compress/encoding/hash/math/rand/os 均已接入） |
| `docs/modules.md:67-87` | “os 尚未定义 target”与“已完成接入”并存 | 后者正确，前者是旧文 |
| `docs/parameter-tuning.md:19/27` | fiber_stack 默认 0、fiber_bin 32 | 源码默认 2048 / 128（`config.cpp`、`scheduler.hpp`） |
| `src/network/http2/hpack.cpp:425` | “当前不支持 Huffman” | 下一行就调用 `HuffmanDecode` |
| `docs/network.md:113` | TlsTransport 已实测 | HEAD 已接线，现在成立（审计时无 target） |

---

## 8. 审查检查表（建议按此顺序问问题）

1. **状态机**：每个 `kWaiting/kRunnable/kRunning` 转换是否都在 `m_transition_mutex` 内？
   有没有“读状态→做决定→写状态”之间释放锁的窗口？
2. **一次性 claim**：`WaitNode::claim`、`WaitNode::Finish`、`SelectWaitState::Take`、
   `Mutex::Unlock` 的 `kWoken`，是否保证同一等待只被一个源完成？
3. **锁外动作**：`Wake()`、`notify_*`、观察者回调、用户回调是否都在锁外？
4. **回收门**：`Disarm`/`stop_operations`/`stop_callbacks` 是否覆盖所有在途回调？
5. **代际**：close/dup2 后旧事件是否被 `generation`/`registration_id` 拒绝？
6. **队列完整性**：终止性排空是否遍历整条队列（不是 break）？
7. **异常路径**：`noexcept` 函数里加锁失败、分配失败、`mprotect` 失败是否有安全出口？
8. **文档契约**：返回时是否仍持锁（CV）、是否保证 FIFO（Mutex 饥饿模式）、
   是否允许跨 G 解锁。
