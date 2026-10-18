# 状态机正确性与并发风险审查（2026-10-18）

> 范围：Fiber / Task(G) / HTTP/2 Stream / HTTP/2 Connection / Scheduler M-P /
> Hook 开关 / Timer / Channel / Future / SSE / HTTP-1.1 ResponseWriter。
> 约束：不改核心代码；只新增测试与 CMake 注册；全量 ctest 保持通过。
>
> 环境：WSL2 Ubuntu 24.04、AMD Ryzen 9 8945HX（12 逻辑核）、GCC 13.3.0、
> Release（build-quality）、RelWithDebInfo + TSan（build-tsan）。

> **2026-10-18 更新**：R1/R2/R13 已经批准修复，R3/R5~R12 已做文档级修正，复现测试已转正为回归测试。修复细节、验证结果与未修项见 **§8 修复记录**。

## 0. 先说结论

- 状态机的**纯逻辑部分**（HTTP/2 StreamState、Future 终态、Channel 关闭态、
  Timer、Fiber、GState）经穷举矩阵验证**没有发现非法转移被接受**；
  GState 8×17=136 格、StreamState 7×12=84 格全部符合预期。
- 发现的真缺陷集中在**状态推进与副作用/校验的先后顺序**上：
  `Connection::WriteData` 先推进流状态机、再做尺寸/流控校验，被拒绝的写入仍
  把 END_STREAM 记在流上（**高**）；连接层没有 GOAWAY/draining/closed 状态
  （**中**）。
- TSan 抓到 1 处真实数据竞争：`StreamStateMachine` 无任何并发保护，而头文件
  写的是"无锁、无 IO，**可在任意线程使用**"（**中**，文档歧义 + 误用风险）。
- 其余为低严重性的文档漂移、状态可达性不一致、配置项未生效等。
- 全量 ctest 最终结果：**103/103 通过、0 失败**（104 注册，其中
  `go2cpp_http2_state_desync_repro` 为 DISABLED 的缺陷复现）。中间某一轮
  曾与另一个并行任务新增的 `go2cpp_math_test` / `go2cpp_rand_test` 撞上，
  那两个测试随后已被其作者修好；与本审查无关。

## 1. 状态机清单

| # | 状态机 | 定义位置 | 状态 | 谁在改 | 并发保护 |
|---|---|---|---|---|---|
| 1 | `GState`（Task/G） | `include/go2cpp/concurrency/scheduler/scheduler.hpp:76` | New / Runnable / Running / Waiting / Dead / Cancelled / Failed（7） | `Task::promote_new/try_mark_queued/try_mark_running/park_for_scheduler/wake_for_wait/mark_runnable/mark_yielded(_fast)/mark_waiting/request_cancel/cancel_if_runnable_unqueued/run`（`src/concurrency/scheduler.cpp:155-728`） | `std::atomic<GState>`（`scheduler.hpp:412`）+ `m_transition_mutex`；仅 `mark_yielded_fast` 走无锁 CAS（`scheduler.cpp:472`） |
| 2 | `MState`（worker/M） | 同上 `scheduler.hpp:97` | Idle / Running / Blocking / Parked / Stopping / Dead（6） | worker 主循环（`scheduler.cpp:3194/3293/3360/3364/3379/3417/3448`）、`enter_blocking`（2693）、`leave_blocking_for`（2749） | `std::atomic<MState>`（`scheduler.cpp:758`）；绝大多数写者是**本 M 自己**，blocking 迁移在 `Impl::mutex` 内 |
| 3 | `PState`（processor/P） | 同上 `scheduler.hpp:114` | Idle / Running / Stopping / Dead（4） | sysmon（`scheduler.cpp:1121`）、`leave_blocking_for`（2758）、worker（3236/3290/3367）、shutdown（1624/1740） | `std::atomic<PState>`（`scheduler.cpp:746`）；部分 worker 写入**不在** `Impl::mutex` 内 |
| 4 | `FiberState` | `include/go2cpp/concurrency/fiber.hpp:41` | Ready / Running / Suspended / Completed / Failed（5） | `resume_locked`（`fiber.cpp:813/919/939`）、`entry`（575/610）、`abandon_suspended`（968） | `std::atomic<FiberState>`（`fiber.cpp:487`）+ `m_resume_claim` CAS 串行化 resume |
| 5 | `TimerState` | `include/go2cpp/concurrency/core/timer.hpp:49` | kInvalid / kPending / kRunning / kCompleted / kCancelled / kFailed（6） | `Execute`（`timer.cpp:379`）、`Finish`（465-538）、`CancelState`（593）、`Cancel`（707-710） | `Timer::State::m_mutex`（`timer.cpp:149`）保护 `m_status`/`m_active`，读写都持锁 |
| 6 | `Channel` 关闭态 + `ChannelStatus` | `include/go2cpp/concurrency/channel.hpp:34`（结果码）、`m_closed`（:1685） | 打开 / 已关闭（2）；`ChannelStatus` 是结果码不是状态 | `Channel::Close`（:813，持 `m_mutex`） | `std::mutex m_mutex` |
| 7 | `FutureStateKind` | `include/go2cpp/concurrency/future.hpp:205` | kPending / kValue / kError / kException / kCancelled（5） | `FutureState::SetValue/SetError/SetException/Cancel`（:261-323） | `m_mutex`（:455）+ `m_kind`（:457）；先到先得 |
| 8 | HTTP/2 `StreamState` | `include/go2cpp/network/http2/stream_state.hpp:55` | idle / reserved(local) / reserved(remote) / open / half-closed(local) / half-closed(remote) / closed（7） | `stream_state.cpp:43-190` 的 8 个 Send/Receive 方法 | **无**（普通 `StreamState` + 两个 `bool` 成员，`:120-129`） |
| 9 | HTTP/2 `Connection` 连接态（隐式） | `include/go2cpp/network/http2/connection.hpp:130` | 未握手 / 已握手（`m_handshaked`）；**没有** draining/closed | `Handshake`（`connection.cpp:116-163`）、`FinishSettingsHandshake`（165-230） | 非线程安全（类注释声明单线程驱动），无锁；`ConnStatus` 只是结果码 |
| 10 | `http::StreamState`（HTTP/1.1 ResponseWriter） | `include/go2cpp/network/http.hpp:182` | Idle / HeadersCommitted / Closed / Failed（4） | `ResponseWriter::start/write/close`（`http.cpp:539-591`） | **无**（文档声明单线程/单 Fiber 持有） |
| 11 | `sse::StreamState` | `include/go2cpp/network/sse.hpp:133` | Idle / Open / Closed / Failed（4） | 由 `m_failed` + `ResponseWriter::state()` + `cancelled` 合成（`sse.cpp:548-568`） | `sync::Mutex m_mutex`（写路径与状态查询都持锁） |
| 12 | `ContextRollback::Status` | `include/go2cpp/concurrency/context.hpp:323` | kActive / kRollingBack / kCommitted / kRolledBack / kFailed（5） | `context.cpp:788-1070`（Record/Commit/Rollback/回调） | `State::mutex`（`context.cpp:736`），status 在锁内 |
| 13 | Hook 开关（位图） | `src/network/hook.cpp:1050` | 每个 op 2 态；另有进程级 `s_enabled`（:217）、`ScopedEnable` 计数（:218） | `SetOpEnabled`（:1127，`fetch_or/fetch_and`）、`SetAllOpsEnabled`（:1144，store） | 原子 RMW，无丢失更新 |
| 14 | `WaitStatus`（ParkingCondition）/ `io::WaitStatus` / `FrameStatus` / `HpackStatus` / `HuffmanStatus` / `ConnStatus` / `FutureStatus` / `ChannelStatus` | 见各头文件 | 均为**结果码**，不是持久状态机 | — | — |
| 15 | HTTP `ParseState` | `include/go2cpp/network/http.hpp:97` | NeedMore / Complete / Error（结果码） | `Parser::feed/finish`（`http.cpp:290-430`） | 无锁，单消费者；`Error` 不是持久终态 |
| 16 | `WebSocket` 关闭态 | `include/go2cpp/network/websocket.hpp:120/155` | 打开 / 已关闭 | `close`（`websocket.cpp:650-668`，持 `m_write_mutex` + `m_close_sent`） | 写锁 + `std::atomic<bool>` |

## 2. 穷举矩阵结果

新增两个矩阵测试（均为 ctest 常规测试，必须通过）：

```
tests/concurrency/test_state_machine_matrix.cpp   -> go2cpp_state_matrix
tests/network/test_http2_state_matrix.cpp         -> go2cpp_http2_state_matrix
```

### 2.1 GState（Task）8 局面 × 17 事件 = 136 格 —— 全部通过

局面把 `state + queued + execution_claim` 三者一起固定（只按 state 枚举会漏掉
"queued 为空时 `try_mark_running` 必须失败"这类不变量）：

| 局面 \ 事件 | promote_new | prepare_enqueue(缺参) | try_mark_queued | try_mark_queued_worker | clear_queued | try_mark_running | mark_runnable | mark_yielded | mark_yielded_fast | mark_waiting | wake_for_wait | park | request_wake | cancel | cancel_if_unqueued | wait_for(0) | run |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| New | ✅→Runnable | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | Rejected | Rejected | ✅ | ✅→Cancelled | ❌ | ❌ | 不变 |
| Runnable(!queued) | ❌ | ❌ | ✅ | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | Rejected | Rejected | ✅ | ✅→Cancelled | ✅→Cancelled | ❌ | 不变 |
| Runnable(queued) | ❌ | ❌ | ❌ | ❌ | ✅ | ✅→Running | ❌ | ❌ | ❌ | ❌ | Rejected | Rejected | ✅ | ✅→Cancelled | ❌ | ❌ | 不变 |
| Running | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ | ✅→Runnable | ✅→Runnable | ✅→Waiting | Pending | ✅→Waiting | ✅ | ✅（保持 Running，仅置标记） | ❌ | ❌ | ✅→Dead |
| Waiting | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ | ✅→Runnable | ❌ | ❌ | ❌ | Pending→Runnable | Rejected | ✅ | ✅（保持 Waiting，仅置标记） | ❌ | ❌ | ✅→Dead |
| Dead | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | Rejected | Rejected | ✅ | ❌ | ❌ | ✅ | 不变 |
| Cancelled | 同上全拒绝 | | | | ✅ | | | | | | Rejected | Rejected | ✅ | ❌ | ❌ | ✅ | 不变 |
| Failed | 同上全拒绝 | | | | ✅ | | | | | | Rejected | Rejected | ✅ | ❌ | ❌ | ✅ | 不变 |

断言内容：返回值、迁移后状态、**终态行对任何事件都不得离开终态**。
结果：`cells=1844 failures=0`（1844 含 fixture 校验、终态不变量、300 轮
cancel-vs-complete 竞态与 M/P 采样断言）。

### 2.2 FiberState 5 状态 × 5 事件 = 25 格 —— 全部通过

- Ready×resume→Completed、Ready×RequestCancellation→仍 Ready；
- Suspended×resume→Completed、Suspended×RequestCancellation→仍 Suspended；
- Completed/Failed×resume→**明确拒绝**（false），状态不变；
- Running 只能在自身栈上观察：在 body 内 `resume()` 自恢复**被拒绝**、
  `Suspend()` 成功、`RequestCancellation()` 后 `CancellationRequested()` 可见；
- 线程外部 `Fiber::Suspend()` 一律 false。

### 2.3 TimerState 6 状态 × 5 事件 = 30 格 —— 全部通过

- Pending×Cancel→Cancelled；Pending×Reset→成功；Pending(一次性)×Refresh→**明确拒绝**；
- Running×Cancel→Cancelled（不阻断已在跑的回调）；Running×Reset/Refresh→拒绝；
- Completed/Cancelled/Failed×Cancel/Reset/Refresh→状态不变（终态不可复活）；
- kInvalid（默认 Timer）×任何事件→保持 kInvalid / 拒绝。

### 2.4 HTTP/2 StreamState 7 状态 × 12 事件 = 84 格 —— 全部通过

矩阵的期望值直接来自 RFC 9113 §5.1（idle 只允许 HEADERS/PUSH_PROMISE、
half-closed 方向单向、RST 可从任意非 idle 状态关闭、closed 上一切
`kStreamClosed`），逐格同时断言**返回值**与**迁移后状态**。7×12 全部一致。

附加：非法事件不污染后续合法事件（idle 上被拒的 RST 之后仍能发 HEADERS；
half-closed(local) 上被拒的 DATA 之后仍能发 RST）；随机 20000 步状态始终合法。

### 2.5 Connection 握手态 2 状态 × 6 事件 = 12 格 —— 全部通过

- 未握手：StartRequest→`kProtocolError`、未知流 WriteData/SendResponseHeaders/
  ResetStream→`kProtocolError`、ReadEvent(nullptr)→`kProtocolError`；
- 已握手：StartRequest→`kOk` 且流进入 half-closed(local)；重复 Handshake→`kOk`（幂等）。

### 2.6 HTTP/1.1 ResponseWriter 4 状态 × 5 事件 = 20 格 —— 全部通过

Idle 只能 start；HeadersCommitted 才能 write/flush/close；Closed/Failed 上
start/write/flush/close 一律明确返回 false，状态不变。

### 2.7 SSE 4 状态 × 6 事件 = 24 格 —— 全部通过

Idle 只能 start；Open 可 send/comment/tick/close；Closed 上 close 幂等成功、
其余拒绝；Failed（对端关闭 + 写超时触发）上一切拒绝。

## 3. 并发风险清单

### 3.1 TSan 结果

构建：

```
cmake -S . -B build-tsan -DGO2CPP_ENABLE_TSAN=ON -DGO2CPP_BUILD_EXAMPLES=OFF \
      -DGO2CPP_BUILD_BENCHMARKS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-tsan -j12 --target <test...>
setarch $(uname -m) -R ./build-tsan/<test>
```

> 必须 `setarch -R`：本机内核 `vm.mmap_rnd_bits=32`，TSan 直接运行会
> `FATAL: ThreadSanitizer: unexpected memory mapping`。这是环境问题，不是代码问题。

| 二进制 | 结果 | TSan 报告数 |
|---|---|---|
| go2cpp_stream_state_test | exit 0 | 0 |
| go2cpp_connection_test | exit 0 | 0 |
| go2cpp_state_machine | exit 0 | 0 |
| go2cpp_timer_stress | exit 0 | 0 |
| go2cpp_high_load_stress | exit 0 | 0 |
| go2cpp_state_matrix（新） | exit 0 | 0 |
| go2cpp_http2_state_matrix（新） | exit 66 | **1（预期，见下）** |

新矩阵中的并发探针（2 线程 × 50000 事件驱动同一个 `StreamStateMachine`）在
TSan 下报出：

```
WARNING: ThreadSanitizer: data race
  Write of size 1 ... by thread T1:
    #0 StreamStateMachine::RefreshFromEndFlags() stream_state.cpp:39
    #1 StreamStateMachine::SendHeaders(bool)   stream_state.cpp:51
  Previous read of size 1 ... by thread T2:
    #0 StreamStateMachine::SendData(bool)      stream_state.cpp:87
SUMMARY: ThreadSanitizer: data race .../stream_state.cpp:39
```

对应代码：`m_state` / `m_local_end` / `m_remote_end` 都是普通成员
（`stream_state.hpp:120-129`），没有任何原子或锁。功能层面只能断言"状态值仍在
枚举范围内"，无法保证语义正确。

### 3.2 人工审查结论（逐状态机）

| 状态机 | 状态字段是否原子 | 转移是否在锁内 | check-then-act | 结论 |
|---|---|---|---|---|
| GState | 是（除 `mark_yielded_fast` 无锁 CAS） | 是（`m_transition_mutex`）；`mark_yielded_fast` 例外 | `try_mark_*` 都在锁内成组校验 `state+queued+claim` | 未发现缺陷；矩阵 136 格 + TSan 全过 |
| MState | 是 | 大部分是；worker 自己写不需要锁 | 否 | 未发现具体竞争；见风险 R10 |
| PState | 是 | worker 的 `kRunning/kIdle` 写在锁外 | sysmon 的 `active_machines` 判断在锁内 | 未复现 lost update；见风险 R10 |
| FiberState | 是 | `m_resume_claim` CAS 串行化 resume；状态切换本身无锁 | `resume_locked` 读状态后写 Running，由 claim 独占 | 未发现缺陷 |
| TimerState | 普通字段 | 是（`State::m_mutex`），查询也持锁 | 否 | 未发现缺陷 |
| Channel 关闭态 | 普通 bool | 是（`m_mutex`） | 否 | 未发现缺陷 |
| Future | 普通字段 | 是（`m_mutex`），`m_kind` 判定与写入同锁 | 否 | 未发现缺陷 |
| HTTP/2 StreamState | **否** | **否** | 是 | **R3：无并发保护** |
| Connection | 普通字段 | 否（声明单线程） | `WriteData` 先转移后校验 | **R1/R2** |
| http::ResponseWriter | 普通字段 | 否（声明单线程） | 否（先 IO 后置状态） | 未发现状态机缺陷 |
| SSE | `m_failed` 普通 bool | 是（`sync::Mutex`） | 否 | **R6：start 失败不置 Failed** |
| ContextRollback | 普通字段 | 是（`State::mutex`） | 否 | 未发现缺陷（未做穷举矩阵） |
| Hook 开关 | 是（原子 RMW） | 不需要 | 否 | 未发现缺陷 |

### 3.3 内存序抽查

- `Task::state()` 用 `acquire`，状态迁移用 `release`，与执行权/排队标志配对合理；
- `Future`/`Timer`/`Channel`/`Context` 用互斥量，无内存序问题；
- Hook 的 `relaxed` 只用于配置位图（不承载数据发布），可接受；
- `ProcessorSnapshot`/`MachineSnapshot` 读取用 `acquire`/`relaxed`，都是观测用途；
- 未发现"该同步却用 relaxed"的实例。

## 4. 风险表（按严重性排序）

| ID | 严重性 | 问题 | 位置 | 复现 | 建议 |
|---|---|---|---|---|---|
| R1 | **高** | `WriteData` 先调用 `SendData()` 推进状态机，再校验帧尺寸/流控/写 Transport。被拒绝的写入仍把 END_STREAM 记在流上，状态与线上状态脱节；流控错误返回 `kFlowControl` 且文档要求"等 WINDOW_UPDATE 后重试"，但重试会被状态机拒绝 | `src/network/http2/connection.cpp:269-291`（`SendData` :277，尺寸 :281，流控 :286） | `go2cpp_http2_state_desync_repro` repro1/repro1b：20KB + END_STREAM → `kUnsupported`，随后合法 2 字节 END_STREAM 写入被拒；窗口耗尽后 `kFlowControl`，补窗口重试仍失败 | 先做纯校验（尺寸、窗口预检），全部通过后再 `ConsumeSendWindow()` + `SendData()` + `WriteFrame()`；写失败时回滚状态或显式把流置为 `closed` 并向上返回不可重试错误 |
| R2 | **中** | 连接没有 GOAWAY/draining/closed 状态：`SendGoAway` 只写帧不置标志（与头文件注释"标记连接不再接受新流"矛盾）；收到 GOAWAY 只产事件不落状态，之后仍可 `StartRequest` | `src/network/http2/connection.cpp:434-444`、`:620-629`；注释 `include/go2cpp/network/http2/connection.hpp:172` | repro2/repro3：发/收 GOAWAY 后 `StartRequest` 仍返回 `kOk` | 增加 `m_state{open,draining,closed}` + `m_last_stream_id`；GOAWAY 后拒绝新流、超过 last_stream_id 的流标记可重试；补文档与矩阵（把 R2 转正为常规用例） |
| R3 | **中** | `StreamStateMachine` 完全无并发保护，但类注释写"无锁、无 IO，可在任意线程使用"，容易被理解为线程安全；`Connection` 虽声明单线程，但该类是 public 头文件、可被独立复用 | `include/go2cpp/network/http2/stream_state.hpp:81`；成员 `:120-129`；TSan 报告 `stream_state.cpp:39` vs `:87` | TSan：2 线程同时 SendHeaders/SendData | 二选一：① 文档改成"**非线程安全**，同一实例同一时刻只允许一个线程/Fiber 使用"；② 若确需并发，把三个字段收敛为单个 `std::atomic<uint32_t>` 位打包并用 CAS 迁移 |
| R4 | **中** | `max_concurrent_streams` 只写进 SETTINGS，从未校验；`m_streams` 无上限，对端可无限开流 | `include/go2cpp/network/http2/connection.hpp:79`；唯一使用点 `connection.cpp:153` | 代码检索即可证明（全仓仅 2 处出现） | 建流前检查活跃流数；超限返回 `kRefusedStream`/拒绝；补压力测试 |
| R13 | **中** | 接收侧流控窗口不校验：对端 DATA 超过可用窗口时照单全收，既不回 `FLOW_CONTROL_ERROR`，还继续补发 `WINDOW_UPDATE`；窗口可无限为负，补窗增量最终可能超过 2^31-1（本身又是协议错误） | `src/network/http2/connection.cpp:541-575`（扣窗口在 :545，补窗在 :553-575） | repro4：握手后向 open 流连发 5 个 16384 字节 DATA（超出 65535 窗口），5 次 `ReadEvent` 全部返回 `kOk` | 扣窗前检查 `m_local_window` / 流窗口，越界返回 `ConnStatus::kProtocolError`（并停止补窗）；补窗增量按 2^31-1 饱和 |
| R5 | 低 | 写失败后连接没有 latched closed；`StartRequest` 在写帧前就消耗流 id 并插入流表 | `connection.cpp:242-266` | MemoryTransport 正常，未构造失败写传输；人工审查 | 引入连接状态（同 R2）并在写失败时置 closed；流表插入延后到写成功或失败时清理 |
| R6 | 低 | SSE `start()` 在 socket 无效时返回 `Error::Io` 但不置 `m_failed`，`state()` 仍是 `Idle`，与"失败后流进入 Failed"的注释不符，且该 Idle 永远无法再次 start | `src/network/sse.cpp:454-461` | 矩阵开发时实测：closed socket + start → 非 ok，但 `state()==Idle` | 把该分支也置 `m_failed=true`；矩阵里 Failed 行改用真实写失败构造（当前测试已如此） |
| R7 | 低 | `SendGoAway` 的 `last_stream_id` 取 `m_streams` 最大 id（含本端发起的流）；对 client 角色，RFC 的 last-stream-id 指对端发起的流 | `connection.cpp:434-443` | 人工审查 | 按角色只统计对端发起的 stream id |
| R8 | 低 | `ResponseWriter::flush()` 不 flush，只返回状态；命名/语义误导 | `src/network/http.cpp:579` | 阅读代码 | 改名 `writable()` 或真正 flush（`ResponseWriter` 无缓冲，建议后者删掉该 API 或加注释） |
| R9 | 低 | 注释漂移：Timer 注释写 `kExpired`，枚举实际是 `kCompleted` | `src/concurrency/timer.cpp:138`、`:427` | 阅读代码 | 改注释，避免后来者按不存在的状态写代码 |
| R10 | 低 | M/P 状态没有集中的 `Transition()`；`PState` 的 `kRunning/kIdle` 有锁外 worker 写入（原子），理论上可与 sysmon/`leave_blocking` 的锁内写入形成 lost update | `scheduler.cpp:3236/3290/3367` vs `1121/2758` | 未复现（采样断言通过） | 把 M/P 迁移收敛到一个函数并规定锁归属；至少补注释说明"worker 自写 + 锁内记账"的不变量 |
| R11 | 低 | HTTP `Parser` 的 `ParseState::Error` 不是持久终态：继续 `feed()` 会重新解析同一缓冲；调用方必须自行丢弃 Parser | `src/network/http.cpp:310-430` | 阅读代码 | 文档写明"Error 后 Parser 不可复用"；或内部 latch 错误 |
| R12 | 信息 | `go2cpp_fiber_stack_limit_full` 在改动前首轮 ctest 失败、复跑 6 次全过（含 5 次单跑），判定为偶发；非本次改动引入 | — | 见第 5 节 | 后续单独排查其负载敏感性 |

## 5. 测试与 ctest 结果

### 5.1 新增测试

| 可执行文件 | 断言数 | 内容 | 状态 |
|---|---|---|---|
| `go2cpp_state_matrix` | 1844 | GState 136 格 + Fiber 25 + Timer 30 + Channel 14 + Future 20 + 300 轮 cancel-vs-complete + M/P 采样 | 通过 |
| `go2cpp_http2_state_matrix` | 20567 | StreamState 84 格 + 随机 20000 步 + 并发探针 + Connection 12 + ResponseWriter 20 + SSE 24 | 通过（TSan 下 1 条预期竞争报告） |
| `go2cpp_http2_state_desync_repro` | 7 个失败断言 | R1/R2/R13 的五个缺陷复现 | **DISABLED**（缺陷未修，避免把已知缺陷算成回归） |

运行命令：

```
flock /tmp/go2cpp_build.lock cmake -S . -B build-quality
flock /tmp/go2cpp_build.lock cmake --build build-quality -j12
cd build-quality && ctest -j8
./go2cpp_state_matrix
./go2cpp_http2_state_matrix
./go2cpp_http2_state_desync_repro        # 预期 exit 1（缺陷仍在）
```

复现测试输出（当前）：

```
[repro] FAILED: repro1: 被拒绝的写入把流推进到了 half-closed(local)/closed
[repro] FAILED: repro1: 被拒绝的写入之后，合法的 END_STREAM 写入也被拒绝
[repro] FAILED: repro1b: 流控拒绝的写入把流推进到了 half-closed(local)
[repro] FAILED: repro1b: 补窗口后重试仍失败（状态机已提前 half-closed）
[repro] FAILED: repro2: 已发 GOAWAY 的连接仍然接受新流
[repro] FAILED: repro3: 收到 GOAWAY 后仍接受新流
[repro] FAILED: repro4: 超过接收窗口的对端 DATA 被当成合法事件接收
[repro] failures=7（预期非零：缺陷仍在）
```

### 5.2 全量 ctest

| 轮次 | 时间 | 注册数 | 结果 |
|---|---|---|---|
| 改动前基线 | 12:01 | 99 | **98/99**，失败：`go2cpp_fiber_stack_limit_full` |
| 改动后第 1 轮 | 12:08 | 102（101 运行 + 1 DISABLED） | **101/101 通过，0 失败** |
| 改动后第 2 轮 | 12:09 | 104（103 运行 + 1 DISABLED） | 101/103；失败：`go2cpp_math_test`、`go2cpp_rand_test`（**另一个并行任务在 12:08 新增的模块测试**，与本审查无关；我的 3 个目标均通过） |
| 改动后第 3 轮（最终） | 12:13 | 104（103 运行 + 1 DISABLED） | **103/103 通过，0 失败**（并行的 math/rand 测试已被其作者修好） |

`go2cpp_fiber_stack_limit_full` 单独复跑 5/5 通过，判定为偶发抖动。

## 6. 仍然不够的地方

1. **MState/PState 无注入式矩阵**：没有集中的状态转移函数，无法像 GState 那样
   逐格注入。目前只有"真实调度器采样 + 终态不复活"的弱断言，不能证明没有
   lost update。要真正验证，需要先把 M/P 迁移收敛成可单测的状态机（见 R10）。
2. **GState 矩阵用的是 Task 的后端接口**（`promote_new`/`park_for_scheduler` 等），
   等价于"单元级"验证；真实调度器里 G/M/P 的交错只做了 300 轮 cancel-vs-complete
   与 2 个采样线程，未做长时间随机调度压力下的线性一致性检查。
3. **HTTP/2 连接未做真实 socket/TLS 传输矩阵**：只用 `MemoryTransport`，没有覆盖
   半帧写入、部分写、对端 RST/断连中途的 Transport 行为。R1 的"写失败后状态
   回滚"路径因此只做了人工审查。
4. **TSan 覆盖有限**：只跑了 5 个既有二进制 + 2 个新矩阵；没有跑
   `go2cpp_http2_stress`、`go2cpp_tests` 全量、hook 系列与 examples。Fiber 的
   TSan 注解存在，但自定义栈切换 + TSan 的组合仍需更大规模验证。
5. **Hook 开关只做了阅读级审计**（原子 RMW 正确），没有用矩阵覆盖
   `SetAllOpsEnabled(false)` 与 `SetOpEnabled` 并发交错；现有 `test_hook_ops`
   覆盖的是顺序语义。
6. **ContextRollback::Status 未做穷举矩阵**（不在本次点名列表内），只确认了
   `status` 在 `State::mutex` 内；`kRollingBack` 的瞬态无法从外部注入。
7. **HTTP Parser / WebSocket 没有专门的矩阵**：只做了代码级审查，`ParseState`
   的"Error 非终态"与 WebSocket 关闭态没有新测试。
8. **并行任务干扰**：审查期间另一个任务在同一工作树新增了 math/rand 模块与
   测试（cmake 12:08 的改动晚于我的 12:05），导致最后一轮 ctest 出现 2 个
   非本审查的失败项。若要求"全绿"作为交付门槛，需要在无并行写入的窗口复跑。
9. 按约束未执行任何 git 命令，因此无法用 diff 证明"只改了测试/CMake/文档"。
   实际改动清单见第 7 节。

## 7. 改动清单（不含核心代码）

| 文件 | 类型 |
|---|---|
| `tests/concurrency/test_state_machine_matrix.cpp` | 新增（GState/Fiber/Timer/Channel/Future 矩阵） |
| `tests/network/test_http2_state_matrix.cpp` | 新增（Stream/Connection/ResponseWriter/SSE 矩阵） |
| `tests/network/test_http2_state_desync_repro.cpp` | 新增（R1/R2 复现，DISABLED） |
| `cmake/Go2CppTests.cmake` | 注册 3 个目标；复现目标 `DISABLED TRUE` |
| `docs/state_machine_audit_2026-10-18.md` | 本报告 |
| `codex.md` | 追加本轮记录 |

**未修改任何 `src/` 或 `include/` 下的核心代码。**

---

## 8. 修复记录（2026-10-18，经总管理批准后执行）

> 约束遵守：修复前先有可复现测试（R1/R2a/R2b/R13 在阶段一已稳定红）；
> 修复后把复现测试**转正**为回归测试；未执行任何 git 命令；所有编译在
> `flock /tmp/go2cpp_build.lock` 下完成。

### 8.1 P1：R1 + R13

**R1（WriteData 校验顺序）** — `src/network/http2/connection.cpp:286-334`

- 新流程：`CanSendData()`（只读）→ 帧尺寸 → `HasSendWindow()`（只读）→
  `SendData()` → `ConsumeSendWindow()` → `WriteFrame()`；
- `WriteFrame` 失败：`machine->ForceClose()`，把该流本地置 `closed`，不再留
  "状态已变、线上没变"；
- `ConsumeSendWindow` 提交失败（单驱动者下不可达）：同样 `ForceClose()` 并
  返回 `kFlowControl`；
- `StreamStateMachine` 新增两个小方法（`stream_state.hpp/.cpp`）：
  `bool CanSendData() const noexcept`（只读预检）和 `void ForceClose() noexcept`
  （本地强制关闭，不发 RST）。
- 回归用例：**R1a**（超尺寸 END_STREAM 不再推进状态、随后合法 END_STREAM
  成功）、**R1b**（`kFlowControl` 不推进状态、不扣额度、补窗后重试成功）。

**R13（接收侧流控）** — `src/network/http2/connection.cpp:600-680`

- 在 `ReceiveData` 与扣窗口**之前**检查连接级 + 流级可用窗口；越界返回
  `kProtocolError`（错误消息含 `FLOW_CONTROL_ERROR`），窗口不再被推到负值；
- WINDOW_UPDATE 增量按 `2^31-1` 饱和（避免补窗本身变成协议错误）；
- **复现方式修正（重要）**：连续发 5 个满帧**不能**证明越界 —— 接收方在
  窗口降到一半以下时会回 WINDOW_UPDATE 并恢复自己的 credit，后续帧是
  **合法**的。改用 `initial_window_size = 1` + 一个 16 字节 DATA 帧，第一帧
  就明确超过可用窗口，与补窗策略无关。旧的 5 帧复现在修复后也会通过，
  因此已整体替换。
- 回归用例：**R13**。

### 8.2 P2：R2 连接状态（新增 3 个状态）

- 新增 `enum class ConnState : std::uint8_t { kOpen, kDraining, kClosed };`
  （`connection.hpp`）；成员 `m_state{kOpen}`、`m_peer_last_stream_id{0}`；
  公开只读 `state()` 与 `peer_last_stream_id()`。
- 迁移点共 3 条：
  1. `SendGoAway` 写帧成功后 → `kDraining`；
  2. 收到 GOAWAY（`ReadEvent`）→ `kDraining` 并记录 `last_stream_id`；
  3. 任意 `WriteFrame` 失败（含 client preface 写失败）→ `kClosed`。
- 拒绝路径：`StartRequest` 在非 `kOpen` 返回 `kProtocolError`；`Handshake`
  在非 `kOpen` 返回 `kClosed`；`WriteData` / `SendResponseHeaders` /
  `ResetStream` / `ReadEvent` 在 `kClosed` 快速返回 `kClosed`。
- 验证：**R2a**（发 GOAWAY → `kDraining` + StartRequest 被拒 + 状态不变）、
  **R2b**（收 GOAWAY → `kDraining` + `peer_last_stream_id` + 拒绝新流）、
  **R2c**（写失败 → `kClosed` → StartRequest/ReadEvent 拒绝）。
- 状态数：**3**（`kOpen` / `kDraining` / `kClosed`），原来只有隐式的
  "未握手/已握手" 2 个布尔局面。

### 8.3 P3：R3 只改文档

`stream_state.hpp` 类注释改为：

```
 * @details 无锁、无 IO；**非线程安全**：同一个实例同一时刻只允许一个
 *          线程/Fiber 驱动（上层 Connection 也按单驱动者使用）。可以在
 *          任意线程上使用，但不能被多个线程并发调用；需要并发访问时由
 *          调用方加锁。
```

成员区另加注释：三个字段是普通值，不要改成"看起来原子"的写法。按要求
**没有**做 atomic 位打包。

### 8.4 P4：R5~R12（只做文档/注释级）

| 项 | 修正 |
|---|---|
| R5 | `WriteFrame` 写失败现在置 `kClosed`（属 R2 的行为修正）；`StartRequest` 先消耗流 id/插流表的顺序**未改**，仅在注释中记录 |
| R6 | `sse.cpp` 注释说明"无效 socket 不置 `m_failed`，状态仍 Idle"，调用方应丢弃该 Stream |
| R7 | `SendGoAway` 注释记录 `last_stream_id` 对 client 角色的语义偏差 |
| R8 | `ResponseWriter::flush` 注释说明无用户态缓冲、只查询可写 |
| R9 | `timer.cpp` 两处 `kExpired` → `kCompleted` |
| R10 | `scheduler.cpp` worker 的 `PState::kIdle` 写入处补锁归属说明 |
| R11 | `http.hpp` Parser 注释写明 `Error` 不是持久终态，收到后应丢弃/reset |
| R12 | 未改（`go2cpp_fiber_stack_limit_full` 偶发，非本次引入） |

### 8.5 复现测试转正

- `cmake/Go2CppTests.cmake`：去掉 `DISABLED TRUE`，注释改为"回归测试"。
- `tests/network/test_http2_state_desync_repro.cpp`：改为回归测试，6 个场景
  （R1a / R1b / R2a / R2b / R13 / R2c）。
- 修复前（阶段一）同一文件稳定失败：**7 个失败断言**；
  修复后：**checks=35，failures=0，全部通过**。

### 8.6 修复后验证

| 项目 | 结果 |
|---|---|
| 构建（含 `go2cpp_http2_state_matrix`、`go2cpp_http2_state_desync_repro`） | 0 error（仅既有 `-Wreorder` 警告：`Connection` 初始化列表顺序，非本次引入） |
| `go2cpp_http2_state_desync_repro` | **6/6 场景通过，failures=0** |
| `go2cpp_http2_state_matrix` | **20567 断言，0 失败** |
| 全量 ctest | **104/104 通过，0 失败**（复现测试已作为常规测试运行并通过） |
| TSan（`setarch -R`） | state_matrix / regression / connection / stream_state / http2_stress 全部 exit 0、**0 报告**；http2_state_matrix 仅剩 1 条**故意的**双线程探针报告（R3 按要求只改文档、未改代码） |

### 8.7 仍然没修（明确列出）

| 项 | 状态 | 原因 |
|---|---|---|
| **R4（中）** `max_concurrent_streams` 未校验、流表无上限 | 未修 | 不在本次批准的 P1~P4 范围 |
| **R6（低）** SSE 无效 socket 后状态仍 Idle | 仅注释 | 批准为文档级 |
| **R7（低）** SendGoAway 的 `last_stream_id` 统计本端流 | 仅注释 | 批准为文档级 |
| **R5（低）** StartRequest 先消耗流 id/插流表再写帧 | 部分（写失败置 kClosed） | 顺序未改，仅注释 |
| **R10（低）** M/P 无集中转移函数 | 仅注释 | 批准为文档级 |
| **R11（低）** Parser 的 Error 非持久终态 | 仅注释 | 批准为文档级 |
| **R12** `go2cpp_fiber_stack_limit_full` 偶发 | 未排查 | 非本次引入 |
| **R3 代码级** StreamStateMachine 仍非线程安全 | 未改代码 | 按要求不做 atomic 位打包 |
| 真实 socket/TLS 的半帧/部分写矩阵 | 未做 | 本轮仍只用 MemoryTransport（含 http2_stress） |
