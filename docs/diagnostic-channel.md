> **已合并至 [docs/modules/debug.md](modules/debug.md)**：Doctor/DebugServer 分工、单线程命令平面、G2CD/1 帧协议、白名单与命令语义见该文 §5；本文保留为历史快照。

# Doctor：在线诊断通道与受控解锁

> 状态：**已实现并验证**（Unix socket / 单线程命令平面 / 白名单唤醒 / 等待图 /
> Mutex 受控解锁 / 诊断 Fiber / 死锁检测）。QUIC 承载已完成**协议、接口与
> 编译开关**，picoquic 服务端/客户端实现列入路线（见 §9）。
>
> 相关代码：
> - `include/go2cpp/debug/diagnostic_protocol.hpp`（帧协议，header-only）
> - `include/go2cpp/debug/diagnostic_agent.hpp` / `src/debug/diagnostic_agent.cpp`
> - `tests/debug/test_diagnostic.cpp`（单测）
> - `tests/debug/diagnostic_deadlock_demo.cpp` + `tests/debug/run_diagnostic_e2e.sh`
>   + `tests/debug/doctor_client.py`（端到端）
> - `tools/cli/doctor.cpp`（`go2cpp-cli doctor`）

---

## 1. 定位：Doctor 与 DebugServer 的分工

| 能力 | DebugServer（`GO2CPP_DEBUG_DUMP=ON`） | Doctor（`GO2CPP_DIAGNOSTIC=ON`） |
|---|---|---|
| 组件只读 dump | ✅ 7 组件 | ✅ 复用同一份 dump |
| 协议 | `GO2CPP-DEBUG/1` 文本 | `G2CD/1` 二进制帧 + 文本命令 |
| 多路复用 | 一连接一命令 | `stream_id` + 分块 + 每命令独立 stream |
| 受控唤醒 | ❌ 明确只读 | ✅ 白名单 `force-wake` |
| 等待图 | ❌ | ✅ `deadlock-report` |
| Mutex 解锁 | ❌ | ✅ `mutex-unlock`（仅 owner 已终止） |
| 诊断 Fiber | ❌ | ✅ `inject-fiber` |
| 默认开销 | OFF | OFF（不编译、无线程、无符号） |

两者可以同时启用；Doctor 的 `dump` 直接调用 `DebugServer` 的组件注册表，
因此 schema 不会分叉。

---

## 2. 线程模型：单线程命令平面

```text
transport（Unix socket / QUIC stream / 用户 topic 字节流）
     │  Recv 帧（只做 I/O，不读运行时状态）
[接收线程] ──enqueue──▶ [命令队列] ──dequeue──▶ [执行线程（唯一）]
                                                      │
                                            执行命令 + 采集快照 + 受控唤醒
                                                      │
     ◀──────────────── Send（按 stream_id / peer 回写，可分块）
```

### 为什么是「一个执行线程 + 一个接收线程」

- **执行线程只有一个**：`status` 里的协程数、runqueue、M/P 快照、模块信息
  来自多次独立读取；如果两条诊断命令并发执行，字段会互相穿插，读者无法
  判断它们是不是同一时刻。串行执行是「一致性」的最低成本实现。
- **接收线程只做 I/O**：长命令（例如全量 dump）执行期间，新命令仍能被
  `accept`/读取、按到达顺序排队；响应在命令完成后按 `stream_id` 独立回写。
- **两条线程都不经过 Scheduler**：即使所有 M 都 park、runqueue 为空，它们
  只阻塞在 transport / 条件变量上，因此仍能响应（这正是死锁现场可诊断的
  前提）。

### 并发语义（如实说明）

| 层 | 行为 |
|---|---|
| transport | 可以并发收到 N 条命令（N 条连接 / N 条 QUIC stream） |
| 队列 | FIFO，默认上限 1024；满时立即回 `queue-full`，不无限堆积 |
| 执行 | **严格串行**（`stats.concurrent_peak` 恒为 1） |
| 响应 | 每条命令按 `stream_id` 独立发送；大响应分块 |

因此「长命令不阻塞其它 stream」准确含义是：**传输层不阻塞、请求不丢失、
响应按 id 匹配**；执行层的排队时间通过每帧响应的
`doctor_queue_wait_us` 如实暴露。若未来需要执行层并发，必须同时重新定义
快照一致性（例如 per-command 快照 + 版本号），不能只加线程。

---

## 3. 帧协议 G2CD/1

### 3.1 帧头（固定 24 字节，小端序）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | `magic` | `0x47 0x32 0x43 0x44`（ASCII `G2CD`） |
| 4 | 1 | `version` | 1 |
| 5 | 1 | `kind` | 1=request，2=response，3=error |
| 6 | 2 | `flags` | bit0=chunked，bit1=last_chunk，bit2=json_payload |
| 8 | 4 | `stream_id` | 应用层命令序号（客户端分配，响应原样带回） |
| 12 | 4 | `seq` | 同一 stream 内分块序号（请求恒 0） |
| 16 | 4 | `payload_len` | 0..16 MiB |
| 20 | 4 | `checksum` | FNV-1a/32（header 中本字段置 0 后 + payload） |
| 24 | N | `payload` | UTF-8 命令文本 / 结果文本 / JSON |

**校验和只用于发现截断与串线，不是密码学认证。** 长度上限 16 MiB，
超过直接拒绝，避免恶意长度造成内存放大。

### 3.2 请求 payload：文本命令

```text
status [json]
dump [sections=a,b] [format=json]
force-wake scope=scheduler-idle|parking-condition|channel|all|mutex
deadlock-report [json]
mutex-unlock name=<registered-mutex> [--force reason=<text>]
inject-fiber fn=<registered-function> [timeout_ms=2000] [json]
ping | stats [json] | help
```

`key=value` 用空格分隔；也接受裸值简写（`force-wake all`、
`mutex-unlock my-lock`、`inject-fiber traverse`）。

### 3.3 响应

- `kind=response`：成功；
- `kind=error`：payload 为 `code: message`（如
  `unknown-command: foo`、`bad-scope: ...`、`queue-full: ...`）；
- `flags.json_payload`：payload 是 JSON 对象；
- 大响应按 `chunk_bytes`（默认 64 KiB）分块：同一 `stream_id`、`seq` 从 0
  递增，`chunked=1`，最后一帧 `last_chunk=1`。

### 3.4 分块示例

```text
[stream=7 seq=0 kind=2 flags=chunked|json  ] {"schema":...,"schedulers":[...
[stream=7 seq=1 kind=2 flags=chunked|json  ] ...更多...
[stream=7 seq=2 kind=2 flags=chunked|last   ] ...结尾...}
```

客户端必须按 `stream_id` 归并；**不同 stream 的帧可能交错到达**（QUIC
或并发连接场景）。

### 3.5 粘包 / 拆包

传输层是字节流，帧边界与 `recv` 边界无关。必须使用增量解析器
（`DiagnosticFrameParser` / Python 版 `FrameReader`）。解析错误（magic/
version/kind/flags/checksum）立即关闭连接，不「猜」。

---

## 4. 命令语义

### 4.1 `status`

返回（text 与 JSON 两种）：

- `pid` / `uptime_ms` / `protocol`；
- `doctor.*`：运行状态、transport、队列深度、accepted/executed/failed/
  rejected/dropped、`concurrent_peak`、deadlock verdict、no-progress；
- `scheduler.<i>.*`：P/M 数量、runnable（local/global）、metrics；
- `goroutines.*`：total/running/runnable/waiting/dead；
- `runqueue.*`：`local_total` / `global` / `per_p`；
- `machine.<i>.*`、`processor.<i>.*`；
- `waits.<name>`：每个已注册锁/条件/Channel 的单行描述
  （ParkingCondition 含受管 G 的 `gids`）；
- `module.<i>.*`：主程序与 `libgo2cpp_*.so` 的 `path` / `base` / `build_id`
  （读 `/proc/self/maps` 与 ELF `PT_NOTE`；读不到时输出 `unavailable`，
  不伪造）。

示例（卡死现场）：

```text
schema=go2cpp.diagnostic.status/1
protocol=G2CD/1
pid=2047622
doctor.running=true
doctor.transport=unix
doctor.deadlock_verdict=deadlock
scheduler.0.processors=2
scheduler.0.workers=2
scheduler.0.runnable=0
goroutines.total=3
goroutines.waiting=2
runqueue.local_total=0
runqueue.global=0
waits.demo-cv=waiters=1 native_waiters=0 fiber_waiters=1 gids=1
waits.demo-channel=send_waiters=0 recv_waiters=1 capacity=0 length=0 closed=false
module.0.role=exe
module.0.base=0x5e9fa1896000
module.0.build_id=2e7b67b378e3f20164de8e474fc55552de512a75
```

`build-id` 用于外部符号化：把 `base` 与 `build-id` 交给调试器/符号服务器，
即可把 `status`/事件环里的地址映射回源码。

### 4.2 `dump`

复用 `DebugServer::Instance().Dump()`：

- `sections=all`（默认）返回聚合文本，组件段用 `[component <name>]` 标记；
- `sections=scheduler,fiber,...` 只取指定组件；
- 未注册组件输出 `# component <name> unavailable: <reason>`，不编造；
- 响应末尾固定追加 `[doctor duration_us=... queue_wait_us=... ok=...]`，
  JSON 响应则在对象里加 `doctor_duration_us` / `doctor_queue_wait_us`。

正常路径下 `dump` 很快；如果 GMP 快照真的卡顿，`duration_us` 会直接反映，
调用方据此区分「Doctor 慢」与「运行时内部锁被卡住」。

### 4.3 `force-wake`（白名单）

| scope | 作用对象 | 走的既有 API | 为什么安全 |
|---|---|---|---|
| `scheduler-idle` | 空闲 M | `Scheduler::ForceWakeIdle()` → 既有 `notify_all_workers()` | worker 醒来重检 runqueue/stopping，没有工作就继续睡 |
| `parking-condition` | 已注册的 `ParkingCondition` | `notify_all()` | 标准条件变量语义；等待方重检谓词后继续等或返回 |
| `channel` | 已注册的 `Channel<T>` | `Channel::ForceWakeWaiters()`（只发 spurious wake） | 不写结果位、不关闭、不递增 generation；阻塞方重检条件 |
| `all` | 以上三者 | 先 parking/channel（G 入队）再 scheduler-idle（唤醒 M） | 顺序保证入队后有人取 |
| `mutex` | —— | **refused** | Mutex 是 FIFO 所有权交接，被唤醒者不会重检「锁是否可用」；盲目唤醒会破坏 `kLocked/kWoken` |

拒绝示例：

```text
schema=go2cpp.diagnostic.force-wake/1
scope=mutex
verdict=refused
woken_total=0
refused_reason=mutex-waiters-are-not-retryable
detail=sync::Mutex 的交接是 FIFO 所有权转移：...请使用 mutex-unlock...
```

成功示例：

```text
schema=go2cpp.diagnostic.force-wake/1
scope=all
verdict=ok
woken_total=4
detail=woken.parking-condition=1 targets=1 woken.channel=1 targets=1 woken.scheduler-idle=2 targets=1
```

`woken.scheduler-idle` 是「被唤醒的 worker 数」（即 `worker_count()`），
不是「等待者数」；其余两个 scope 是等待节点数。

### 4.4 `deadlock-report`（等待图）

输出 `verdict`、`no_progress_ms`、`threshold_ms`、`edges[]`、`events[]`。

边的类型：

- `scheduler:<name>`：parked/running M 数、runnable；
- `parking-condition` 目标：`describe` 含 `waiters=`、`gids=`（谁在等）；
- `channel` 目标：`send_waiters` / `recv_waiters`；
- `mutex` 目标：`held`、`waiters`（native/fiber）、`owner_gid`、
  `owner_known`、`owner_alive`、`retryable=false`。

**精度边界（不虚构）**：现有内省没有「G 当前阻塞在哪个 Mutex」的权威
记录，`Mutex` 的 owner 只是近似值。因此 Mutex 边标注
`waiter_ids=unavailable`；只有 ParkingCondition 的受管等待者能给出 gid。
这是资源级等待图，不是逐 G 的精确 wait-for graph。

### 4.5 `mutex-unlock`（受控解锁）

判定顺序（任一不满足即 `refused`）：

1. 必须按**注册名**查找（不接受裸地址，避免解引用野指针把目标打崩）；
2. `locked` 必须为真，否则 `not-locked`；
3. `waiters > 0`，否则 `no-waiters`（解了也没人受益）；
4. `owner_known && owner_gid != 0`，否则 `owner-unknown`（无法证明临界区
   被遗弃；**owner-unknown 不等于 owner-gone，绝不据此解锁**）；
5. owner 不在任何已注册 Scheduler 的活动 G 表里（或已处于终态），否则
   `owner-alive`；
6. 调用**既有** `Mutex::Unlock()`；异常映射为 `unlock-failed`。

成功示例：

```text
schema=go2cpp.diagnostic.mutex-unlock/1
name=abandoned
verdict=ok
reason=owner-gone
owner_gid=7
waiters_before=1
waiters_after=0
held_after=false
```

已知限制：owner 记录依赖「持锁 park 检测」（`SetHeldLockParkDetection(true)`，
Debug 构建默认开启，Release 默认关闭）。未开启时 `owner_known=false`，
`mutex-unlock` 一律拒绝 —— 这是有意的保守行为。

#### 4.5.1 `--force`：受控强制解锁（危险，默认拒绝）

```text
mutex-unlock name=<registered-mutex> --force reason=<text>
```

锁顺序反转（A→B 与 B→A）造成的死锁里，两个 owner 都是**存活**的 G，
上面的安全检查必然返回 `owner-alive`。为了让「演示环境里解开死锁」这件事
可操作，Doctor 提供一条显式、带审计的强制路径：

1. 必须同时出现 `--force`（或 `force=1`）与 `reason=<非空文本>`；
   缺 reason 直接 `bad-argument: mutex-unlock --force requires reason=<text>`；
2. 跳过 owner-gone 证明，直接在**调用线程**上执行既有 `Mutex::Unlock()`；
3. 自增 `stats().forced_unlocks`（**在 Unlock 之前**自增：被唤醒的等待者
   可能在 Unlock 返回前就跑完临界区，晚自增会让协作方观测不到这次强制解锁）；
4. 响应体给出 `verdict=forced`、`audit=true`、`force=true`、`reason=`、
   `owner_gid/owner_known/waiters_before/waiters_after/held_after/forced_total`；
5. 同时在 stderr 打一行 `[go2cpp][doctor][audit] mutex-unlock --force ...`。

> **风险（必须写进使用文档，不能只在代码注释里）**
>
> - 被夺锁的 owner **不知道**自己已经失去锁，之后仍可能访问临界区，
>   造成两个执行流同时进入临界区；
> - 若 owner 之后仍然 `Unlock()`，轻则抛
>   `go2cpp::sync::Mutex unlock of unlocked mutex`，重则把锁从新的合法
>   持有者手里抢走；
> - 因此 `--force` **只适用于「进程已经卡死、数据一致性已不可保」的
>   故障处置/demo**，不能作为常规运维手段；生产环境应优先修锁顺序、
>   使用 try-lock/退避，或直接重启进程；
> - 协作方（业务代码）若要在 force 之后继续运行，必须自己识别「锁已被
>   夺走」并跳过对旧锁的释放。`tests/scenarios/deadlock_routes/` 给出了
>   一个例子：handler 释放前比较 `forced_unlocks` 计数 + Mutex owner，
>   只释放仍属于自己的锁（日志里记为 `stale-unlock-skip`）。

强制解锁示例：

```text
schema=go2cpp.diagnostic.mutex-unlock/1
name=res-a
verdict=forced
audit=true
force=true
reason=deadlock-scenario-demo
owner_gid=2100
owner_known=true
waiters_before=5
waiters_after=4
held_after=false
forced_total=1
```


### 4.6 `inject-fiber`（诊断 Fiber）

`InstallDiagnosticFiber(name, scheduler)` 注入一个常驻受管 G：

- 它只 park 在 Doctor 私有的 `ParkingCondition` 上，不占 runqueue；
- 命令到达时：`notify` 唤醒 G（既有 wake 路径）→ G 入队 →
  `Scheduler::ForceWakeIdle()` 唤醒 worker 取队列；
- 内置函数 `traverse`：在**受管 G 的 GMP 上下文**里读取
  `current_task()/current_processor_id()/current_machine_id()`，并遍历
  processors/machines/tasks。

示例（节选）：

```json
{"schema":"go2cpp.diagnostic.inject-fiber/1","function":"traverse",
 "result":{"context":{"gid":5,"managed":true,"processor_id":1,"machine_id":2},
           "processors":[...],"machines":[...],"tasks":[...]}}
```

「独立注入路径」的含义：诊断 Fiber 不是从普通业务队列里抢任务，而是
`ParkingCondition` 定向唤醒 + `wake` 入队 + `ForceWakeIdle` 叫醒 worker；
这条路径只使用既有 API，不修改调度状态字。它仍与业务 G 共享 P 的
runqueue（只在命令执行期间短暂运行），这一点在文档中如实声明。

### 4.7 `stats`

Doctor 自身：accepted/executed/failed/rejected/dropped、队列深度与峰值、
`concurrent_peak`（应为 1）、deadlock 事件数、自动唤醒次数、fiber job 数。

---

## 5. 全局无进展检测

执行线程每 `detect_interval`（默认 500ms）采样一次「进展指纹」：

- runnable/runnable_global、worker 数；
- `metrics.task_runs` / `task_completions`；
- 前 512 个 G 的 id/state/queued；
- 每个 M 的 state/processor/blocking_task；
- 已注册目标的 `describe`（等待者数）与 Mutex 快照。

判定：

- 指纹变化 → `progressing`；
- 无变化且没有等待者 → `idle`（空闲不是死锁）；
- 无变化、存在等待者，但任一 M 为 `kRunning`/`kBlocking` → `progressing`
  （可能正在做 IO；不误报）；
- 其余 → `stalled`；超过 `deadlock_threshold`（默认 5s）→ `deadlock`。

触发时：

1. 事件环追加 `kDiagnosticSuspected`（gid=0，arg2=无进展毫秒数；节流 1s）；
2. `deadlock-report` 的 verdict 变为 `deadlock`；
3. 若 `auto_force_wake=true`，自动执行一次 `force-wake all`，并记录
   `kDiagnosticForceWake`。**默认关闭**：自动解只应作为显式配置。

---

## 6. 接入运输层

### 6.1 Unix socket（默认）

- 路径：`/tmp/go2cpp-<pid>-doctor.sock`，可由
  `DiagnosticAgentConfig::socket_path` 或 `GO2CPP_DOCTOR_SOCKET` 覆盖；
- 权限默认 `0600`；
- 每条命令一个短连接：`accept` → 读一个请求帧 → 入队 → 执行 →
  回写响应帧（可分块）→ 关闭；
- `Recv` 内部用 `poll` 同时监听 listener 与所有活跃连接；单连接缓冲
  上限 16 MiB + 24 字节。

### 6.2 用户的「网络 topic 传输字节流」

把 topic 当作**可靠、有序**的字节流即可：

```text
producer：G2CD 帧（请求）→ topic.send(bytes)
consumer：topic.recv() 追加到 buffer → DiagnosticFrameParser → 处理
          响应帧 → topic.send(bytes)（同一 correlation id / 会话）
```

要点：

1. **粘包**：不要假设一条 topic 消息等于一帧；用增量解析器；
2. **关联**：帧里的 `stream_id` 由客户端分配；跨机器场景建议把
   `stream_id` 与会话 id 一起放进 topic header（或每条命令一个 topic key），
   服务端原样回带；
3. **可靠性**：Doctor 不实现重传/去重；topic 层必须保证「至少一次」时，
   命令应幂等（`status`/`dump` 幂等；`force-wake`/`mutex-unlock` 重复执行
   也安全，但 `mutex-unlock` 第二次会返回 `not-locked`）；
4. **鉴权**：socket 的 `0600` 是本地保护；跨机器必须在 topic/QUIC 层做
   mTLS 或等价认证 —— Doctor 帧协议本身**没有**身份认证；
5. **背压**：响应分块大小可配（`chunk_bytes`）；topic 发送失败应丢弃该
   stream 并记录 `dropped`，不要让 Doctor 执行线程阻塞在网络上。

C++ 自定义 transport 骨架：

```cpp
class TopicTransport final : public go2cpp::debug::DiagnosticTransport {
public:
    bool Start(std::string* error) override { return topic_.Open(error); }
    void Stop() override { topic_.Close(); }
    bool Recv(go2cpp::debug::DiagnosticEnvelope& envelope,
              int timeout_ms, std::string* error) override {
        std::string bytes;
        if (!topic_.Poll(&bytes, timeout_ms, error)) return false;
        if (!parser_.Feed(bytes)) { *error = parser_.error(); return false; }
        go2cpp::debug::DiagnosticFrame frame;
        if (!parser_.Next(frame)) return false;  // 半帧：继续等
        envelope.frame = std::move(frame);
        envelope.peer = session_id_;             // 回信目标
        return true;
    }
    bool Send(const go2cpp::debug::DiagnosticEnvelope& envelope,
              std::string* error) override {
        return topic_.Send(
            go2cpp::debug::EncodeDiagnosticFrame(envelope.frame), error);
    }
    const char* name() const noexcept override { return "topic"; }
    bool running() const noexcept override { return topic_.running(); }
private:
    Topic topic_;
    go2cpp::debug::DiagnosticFrameParser parser_;  // 每会话一个
    std::uint64_t session_id_{0};
};
```

### 6.3 QUIC stream 承载（协议已定，实现见 §9）

映射规则：

| QUIC 概念 | Doctor 概念 |
|---|---|
| 一条 QUIC 连接 | 一个 Doctor 会话（可复用） |
| 一条客户端发起的 bidi stream | **一条命令**（请求 + 响应） |
| stream 内字节 | 1 个请求帧 + 1..N 个响应帧（分块） |
| 帧内 `stream_id` | 应用层命令 id（**不等于** QUIC stream id） |
| ALPN | `go2cpp-doctor/1` |

为什么每命令一条 stream：

- 不同命令的请求/响应互不队头阻塞（QUIC 按 stream 流控）；
- 长命令（全量 dump）的多个 DATA 帧在自己的 stream 上分块，不占用短命令
  的 stream；
- 连接级握手/证书/mTLS 只做一次。

服务端实现要点（picoquic）：

1. 单线程事件循环放在 Doctor **接收线程**里（不新增线程）；
2. `picoquic_set_callback` 的 `stream_data` 回调：按 `stream_id` 找到
   `DiagnosticFrameParser`，喂字节，取出请求帧后入队；
3. 执行线程完成命令后，把响应帧交给接收线程的发送队列
   （`picoquic_add_to_stream`），分块顺序发送，最后 `FIN`；
4. 连接关闭时清理 per-stream parser；
5. `picoquic` 的定时器/重传由事件循环驱动，不能阻塞在 Doctor 执行上。

客户端（CLI）：

```text
go2cpp-cli doctor --transport quic --host <ip> --port <p> --cert <ca.pem> \
    [--insecure] status
```

`--transport quic` 在未启用 `GO2CPP_DIAGNOSTIC_QUIC` 的构建里必须明确报
「未构建」，不能静默退回 Unix socket。

---

## 7. 编译开关与零开销

| 开关 | 默认 | 作用 |
|---|---|---|
| `GO2CPP_DIAGNOSTIC` | OFF | 编译 `diagnostic_agent.cpp`，启用 Doctor 测试与 demo |
| `GO2CPP_DIAGNOSTIC_QUIC` | OFF | 预留 QUIC transport（依赖 `GO2CPP_WITH_PICOQUIC=ON`） |
| `GO2CPP_DEBUG_DUMP` | OFF | 既有：事件环 + DebugServer socket |

默认 OFF 构建：

- `go2cpp_debug` **不包含** `DiagnosticAgent` 符号（`nm` 可验证）；
- 不创建任何线程 / socket / 定时器；
- `ctest` 用例数不变（Doctor 测试条件编译，不 `add_test`）。

开启方式：

```bash
cmake -S . -B build-diag -DGO2CPP_DEBUG_DUMP=ON -DGO2CPP_DIAGNOSTIC=ON
cmake --build build-diag -j4
```

---

## 8. 测试与复现

```bash
# 1) 单元测试（协议/队列/白名单/丢唤醒恢复/Channel/Mutex/诊断 Fiber/socket）
GO2CPP_TEST_FILTER=diagnostic ./build-diag/go2cpp_tests

# 2) 端到端：真实进程 + 真实 Unix socket + 丢唤醒恢复
bash tests/debug/run_diagnostic_e2e.sh build-diag

# 3) 真实场景：100+ HTTP client → 锁顺序死锁 → dump → force-wake → --force 解锁
bash tests/scenarios/deadlock_routes/run_deadlock_scenario.sh build-doctor-full
#    重复触发率：追加 --repro 10

# 4) 手动体验
./build-diag/go2cpp_diagnostic_demo /tmp/demo.sock
python3 tests/debug/doctor_client.py /tmp/demo.sock status
python3 tests/debug/doctor_client.py /tmp/demo.sock deadlock-report
python3 tests/debug/doctor_client.py /tmp/demo.sock "force-wake scope=all"
# 进程 stdout 出现 RECOVERED 即恢复
```

`run_diagnostic_e2e.sh` 覆盖 14 项断言：READY、卡死期间 status、build-id、
dump 组件、duration 元数据、deadlock 判定、等待图边、mutex scope refused、
force-wake 恢复、恢复后 status、并发 10 命令、concurrent_peak=1、死锁事件
计数。

---

## 9. QUIC 落地范围与后续路线

**已完成**：

- 帧协议（`stream_id`/`seq`/分块/校验）与 header-only 编解码；
- `DiagnosticTransport` 接口（`Recv(envelope, timeout)` / `Send(envelope)`），
  `peer` 与 `stream_id` 解耦，天然支持一条连接多 stream；
- Unix socket 实现、内存 transport、自定义 topic 接入说明；
- `GO2CPP_DIAGNOSTIC_QUIC` 编译开关与依赖校验（需要 picoquic）；
- CLI `--concurrency` 验证「多请求并发、单线程执行、按 id 匹配」。

**未完成（明确不在本版本声称可用）**：

1. picoquic 服务端事件循环与 `stream_data` 回调；
2. CLI 的 `--transport quic` 客户端（TLS/mTLS 配置、ALPN、连接复用）；
3. QUIC 连接上的 per-stream 背压与发送队列上限；
4. 跨机时钟对齐（`status` 里已有 `steady_ns`，但文档明确「仅同机可比」）。

路线：先在 `GO2CPP_DIAGNOSTIC_QUIC=ON` 下实现最小 echo（单连接、双 stream、
按 stream 回显），再把 `DiagnosticFrameParser` 接到 `stream_data`，最后接
CLI。协议与接口不需要再改。

---

## 10. 已知限制（如实声明）

1. **运行时内部锁被原生死锁时，Doctor 也可能挂起**：执行线程调用的
   `Scheduler::machines()` 等快照接口会取运行时内部互斥量。如果业务在持有
   该锁时不是 park 而是原生死锁（例如自写 futex 死循环），`status` 会等待
   该锁。Doctor 不解决「运行时内部锁本身卡死」。
2. **等待图是资源级**：Mutex 的等待者没有 gid；owner 是近似值。
3. **`mutex-unlock` 保守**：仅 owner 已不在活动 G 表时执行；owner 记录依赖
   持锁 park 检测（Debug 默认开、Release 默认关）。
4. **诊断 Fiber 与业务共享 P 的 runqueue**：命令执行期间会短暂占用一个 P；
   它不会长期驻留队列（park 在私有条件变量上）。
5. **鉴权**：Unix socket 用文件权限；帧协议没有身份认证；跨机必须由
   topic/QUIC 层提供。
6. **QUIC 未实现**（见 §9）。
7. **分块响应不保证跨命令的全局顺序**：只保证同一 `stream_id` 内 `seq`
   递增；调用方按 id 归并。

---

## 11. 变更记录

- 2026-10-20：新增 Doctor（单线程命令平面、G2CD/1、白名单 force-wake、
  deadlock-report、mutex-unlock、inject-fiber、死锁检测）；对
  `Scheduler::ForceWakeIdle()`、`ParkingCondition` 等待者 gid、
  `Channel<T>::ForceWakeWaiters()` 做最小受控唤醒接口；测试与 e2e 通过。
  未 commit。
