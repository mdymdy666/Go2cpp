# 模块文档：调试与在线诊断（debug）

> 适用基线：`HEAD 27e892b`（2026-10-26），WSL 工作树 `/UserData/CodexWorkSpace/Go2cpp`。
> 源码：`include/go2cpp/debug/`（14 个头文件，5,565 行）、`src/debug/`（4 个实现，9,862 行）。
> 引用约定：`debug_event_ring.hpp` / `debug_resources.hpp` / `debug_dump.hpp` /
> `debug_server.hpp` / `diagnostic_agent.hpp` / `diagnostic_protocol.hpp` 均指
> `include/go2cpp/debug/` 下的同名文件；`debug_event_ring.cpp` / `debug_dump.cpp` /
> `debug_server.cpp` / `diagnostic_agent.cpp` 指 `src/debug/` 下的实现；
> `scheduler.hpp` 指 `include/go2cpp/concurrency/scheduler/scheduler.hpp`。
> 状态：**现行**。本文合并 `docs/debug-reliability.md`、`docs/debug-overhead.md`、
> `docs/diagnostic-channel.md`，旧文档顶部已标注合并去向、正文保留为历史快照。
> 旧文档的压测数字产生于 2026-10-20/21 的 v5 基线；v6/v7 已改写事件序号与采样
> 机制（见 §8），那些数字只能当作**上界参考**，不能当作当前实现的开销。

## 1. 概述

`go2cpp::debug` 不是单一功能，而是围绕「观测运行中的调度器/同步原语」叠出来的三套设施：

1. **进程内调试层**：`DebugScheduler`/`DebugEventCollector`/`debug_cli` 只读快照与
   text/ini/json 序列化（`debug_scheduler.hpp:97-118`、`debug_events.hpp:89`），
   另有协作式断点（`debug_breakpoints.hpp:73-97`）与 `SIGUSR1` 落盘
   （`debug_signal_dump.hpp:88-120`）；
2. **事件环 + 资源历史**：每线程固定容量环形缓冲记录 park/wake/runqueue/锁/通道/
   timer 事件（`debug_event_ring.hpp:5-93`），并配套资源注册表、线程栈样本与 Fiber
   父链样本（`debug_resources.hpp:3-48`）；
3. **在线服务**：`DebugServer` 把上述快照经 Unix socket 暴露成稳定文本协议
   `GO2CPP-DEBUG/1`（`debug_server.hpp:14-27`）；Doctor 在其上加受控唤醒、
   等待图与 Mutex 级解锁，协议为二进制帧 `G2CD/1`（`diagnostic_protocol.hpp:18-31`）。

分层的关键约定是「读」与「写」严格分开：DebugServer 只读；Doctor 只在**白名单**
内做受控写，且每次干预都留事件记录（`diagnostic_agent.hpp:54-71`）。

### 1.1 最小示例

```cpp
#include "go2cpp/debug/debug_server.hpp"

go2cpp::debug::DebugServer::Instance().Start();          // /tmp/go2cpp-<pid>.sock
go2cpp::debug::RegisterScheduler("main", scheduler);     // 组件名 → 只读 provider
go2cpp::debug::RegisterEventRing();                      // 事件环组件
```

```text
$ nc -U /tmp/go2cpp-<pid>.sock
PING
DUMP scheduler
VIEW gmp
VIEW history 3          # 3 是 ResourceId，不是 gid
SUBSCRIBE events 20 128
```

`VIEW`/`VIEWS` 的协议入口在 `debug_server.cpp:1028-1064`；六个基础视图在 HELP 里
列出（`debug_server.cpp:979-981`），实现另外接受四个资源视图（§4.3）。

## 2. 事件环：per-thread id + ts 排序

一条记录是固定字段结构（`debug_event_ring.hpp:438-460`）：

```text
seq ts_ns code gid arg2 pid thread_id arg3 arg4
    [resource_id target_gid stack_id chain_id result flags value waiters]
```

- `seq` 是**每线程单调**序号（1 起、普通自增、无原子），不是进程级序号；
- `thread_id` 是稳定每线程 id（1 起，线程首次记录时从全局原子分配一次，之后只读
  thread_local），用作排序次键（`debug_event_ring.hpp:24-26`、`debug_event_ring.cpp:183-200`）；
- `ts_ns` 是 `steady_clock` 纳秒，是排序主键；`tid` 只是线程 id 的 32 位哈希，
  供人读与旧 dump 对照（`debug_event_ring.hpp:27`）；
- 全局全序是 `(ts_ns, thread_id, seq)`（`debug_event_ring.hpp:90-93`、
  `debug_event_ring.cpp:1114-1135` 的排序）；跨线程不能直接比较 `seq`。

### 2.1 两段环形缓冲

每线程一个普通区（默认 256）+ 一个关键区（默认 64），两段独立 head、独立槽位，
共享同一个 per-thread 序号计数器（`debug_event_ring.cpp:52-72`）。普通事件永远
不会写入关键区，因此高频 park/runqueue 事件写满 256 条后，wake/enqueue 拒绝/
claim 失败/park 拒绝/G 终态/诊断事件仍然保留（`debug_event_ring.hpp:39-53`）。
关键集合由 `SchedEventCodeIsCritical()` 判定（`debug_event_ring.hpp:390-397`），
容量可由 CMake 缓存变量覆盖（`debug_event_ring.hpp:108-116`）。

关键区本身仍是环：关键事件超过 64 条时更旧的关键事件会被覆盖，这是「不丢失全部」
而不是「绝对不丢」；dump 里的 `critical_event_count` / `published_total` 供消费方
判断是否发生截断（`debug_event_ring.cpp:1196-1231`）。

### 2.2 写入与读取协议

写线程「先清 seq、写槽位字段、最后写 seq、release 写 head」；读线程先 acquire 读
head 再复制槽位，用每线程唯一的 seq 二次校验，撕裂记录被丢弃而不是返回
（`debug_event_ring.cpp:5-11`、`202-260`）。`Snapshot()` 汇总所有线程环后排序，
按 `max` 截取**最新**的若干条（`debug_event_ring.cpp:1114-1135`）；`ReadSince()`
反过来保留**最旧**的 `max` 条，保证订阅端游标连续推进（`debug_event_ring.cpp:1153-1167`）。

游标是三字段结构（`debug_event_ring.hpp:462-478`）：`{ts_ns, thread_id, local_seq}`，
`{0,0,0}` 表示空环起点；`CurrentCursor()` 建立订阅起点（`debug_event_ring.cpp:1137-1151`）。

### 2.3 累计计数与采样

环形窗口会被覆盖，因此另有进程级累计计数 `TotalCount(code)`/`TotalCountAll()`：
16 个分片 × 256 个事件码，写本线程分片、读取时求和，避免同类事件在多核上争同一
cacheline（`debug_event_ring.cpp:124-146`、`887-890`、`1010-1019`）。

v7 把「事件条数」与「栈捕获」拆成三个独立旋钮（`debug_event_ring.hpp:399-435`）：

| 旋钮 | 默认 | 环境变量 | 作用范围 |
|---|---:|---|---|
| `ResourceEventSampleEvery()` | 4 | `GO2CPP_DEBUG_RESOURCE_SAMPLE` | 资源事件记录；blocked/wake/timeout/cancelled/closed 与 `kParkCaptured` 始终全量 |
| `SchedEventSampleEvery()` | 8 | `GO2CPP_DEBUG_SCHED_SAMPLE` | enqueue/requeue/runqueue push-pop-claim/GResumed |
| `GO2CPP_DEBUG_SAMPLE_SHIFT` | 0 | 同名 | 再乘 `2^n`；关键事件不受影响 |

采样相位是「第 1 次必记」，低频操作不会被稀释（`debug_event_ring.hpp:414-416`）。
`RecordSampled` 对 2 的幂周期用位与代替除法（`debug_event_ring.cpp:975-1008`）。

### 2.4 关闭时零开销

只有 `GO2CPP_DEBUG_DUMP=ON` 构建才记录。OFF 构建中 `GO2CPP_DEBUG_EVENT` /
`GO2CPP_DEBUG_TIMER_EVENT` / `GO2CPP_DEBUG_PARK_HINT` 等宏展开为 `((void)0)`，
参数不参与求值（`debug_event_ring.hpp:72-75`、`718-737`）；`EventRing` 的查询接口
仍然可用，但 `Enabled()==false`、累计计数恒 0。

## 3. 资源历史：10 类并发结构

资源维度回答「这把锁/这个 Channel 在什么时刻被谁加锁/等待/通知/收发」。三类数据
（`debug_resources.hpp:15-27`）：

1. **资源注册表**：`RegisterResource`/`FindResourceByAddress`，`ResourceId` 进程内
   单调、不复用，0 无效（`debug_resources.hpp:106-155`）；对象析构后 id 仍可查询
   （墓碑），用于回看历史（`debug_resources.hpp:45-47`）；
2. **线程栈样本**：`backtrace()` 取最多 16 帧，按帧地址序列哈希去重 + 引用计数，
   采集点不做符号解析（`debug_resources.hpp:161-225`）；
3. **Fiber 链样本**：从 main_fiber 到当前 Fiber 的父链，最多 9 节点
   （`debug_resources.hpp:231-290`）。

`resources` 视图的 `coverage` 段如实列出项目内 10 类并发结构的插桩状态
（`debug_dump.cpp:2677-2701`）：

| # | 结构 | 资源事件 | 说明 |
|---:|---|---|---|
| 1 | `sync::Mutex` | 是 | lock/unlock + fast-lock 采样 |
| 2 | `sync::RWMutex` | 是 | read/write 四种事件 |
| 3 | `sync::RecursiveMutex` | 是 | 记录递归深度 |
| 4 | `core::ParkingCondition` | 是 | wait/notify/notify_all，节点带 gid |
| 5 | `Channel<T>` | 是 | send/recv/select/wake + 值摘要 |
| 6 | `sync::ConditionVariable` | 是 | wait/notify（notify_all 另记） |
| 7 | `sync::WaitGroup` | 是 | wait/done/add |
| 8 | `sync::TimedMutex` | 是 | 复用 Mutex 事件 |
| 9 | `sync::SpinMutex` | **否** | 纯自旋、无等待队列；插桩会给无竞争路径加原子操作 |
| 10 | `core::FutexMutex` | **否** | 最底层门闩；逐次插桩会产生事件风暴并污染上层归因 |

因此「10 类结构有历史」的准确含义是 **8 类有事件、2 类边界已知但不可观测**，
引用时不能把 10 说成 10 类都可 dump。资源类别枚举另有 `kIoManager`
（`debug_resources.hpp:86-99`），它是 IO 等待资源，不在上述 10 类并发结构里。

### 3.1 资源事件与采样

`ResourceEventRecord` 复用 `SchedEventCode`，另带 `resource_id`/`actor_gid`/
`target_gid`/`value`/`result`/`waiters`/`flags`/`chain_id` 与 v5 的
`fd`/`bytes`/`aux`（`debug_resources.hpp:352-374`）。结果码 8 个
（`debug_resources.hpp:314-326`）；`flags` 标记值有效、目标为普通线程、栈/链来自
采样、是否 Try 操作（`debug_resources.hpp:328-335`）。

资源路径的采样与调度事件分开（`debug_resources.hpp:399-435`）：

- 记录周期默认 1/4，`ResourceEventWanted()` 在**构造 112 B 结构体之前**判定，
  避免宏命中率 1/8 时仍付构造代价（`debug_resources.hpp:376-393`）；
- 栈/链捕获周期默认 1/16，由 `CaptureStackSampleForResource` /
  `CaptureFiberChainForResource` 执行；`critical` 参数只为接口兼容，**不再全量**
  （`debug_resources.hpp:202-214`、`271-281`）；
- 容量上限：栈样本 4096、链样本 2048，满后丢弃并计数
  （`debug_resources.hpp:162-164`、`232-234`）。

### 3.2 回放录制

`GO2CPP_REPLAY=ON` 时资源事件在写环之前先转发给 `SetResourceReplaySink` 安装的
回调，录制器不需要环形缓冲（`debug_resources.hpp:63-80`、`399-420`）。REPLAY-only
构建中 `ResourceEventWanted()` 恒 true，保证录制事件流不被诊断采样削掉。

## 4. dump、组件与视图

### 4.1 数据契约

`debug_dump.hpp` 是输出层契约：所有组件返回纯数据快照，再序列化为 text
（`schema=`/`version=` 打头、一行一个 `key=value`、数组用 `<name>.<index>.<field>`）
或 JSON（`debug_dump.hpp:14-27`、`92-145`）。组件名是稳定常量
（`debug_dump.hpp:60-68`）：scheduler / fiber / channel / parking_condition /
mutex / io / events / runtime / timer。`DumpPayload::Unavailable(reason)` 表示组件
当前不可用，消费方不得把它当成空数据（`debug_dump.hpp:77-84`）。

### 4.2 一致性标记

快照由多次独立读取拼成，不是同一瞬间的全局原子视图。一致性探针读取事件环的
累计发布数（`debug_dump.hpp:147-176`）：`seq_start == seq_end` 表示采集期间没有
新事件发布；不相等则 dump 是多时刻拼接。该标记是**提示**不是事务：未插桩的状态
变化不会递增计数（`debug_dump.hpp:157-160`）。事件 dump 自身也带
`seq_at_start`/`seq_at_end`/`changed_during_dump`（`debug_event_ring.cpp:1214-1229`）。

### 4.3 六个基础视图 + 四个资源视图

`IsKnownViewName()` 实际接受 10 个名字（`debug_dump.cpp:2707-2712`）：

| 视图 | 关键输入 | 输出 | 实现 |
|---|---|---|---|
| `gmp` | scheduler JSON + timer/io JSON | M/P/G 汇总 + park 配对 + 资源摘要 | `debug_dump.cpp:2755-2763` |
| `timeline` | gid（必填） | 单 G 的事件时间线 + 自己的 park 配对 | `debug_dump.cpp:2764-2772` |
| `sched` | 事件环 | enqueue/claim/steal/worker/sysmon 统计 | `debug_dump.cpp:2773-2775` |
| `park` | 事件环 | park↔wake↔return 配对、解释率 | `debug_dump.cpp:2776-2777` |
| `timer` | timer JSON + 事件 | arm/fire/cancel/reschedule/callback 链路 | `debug_dump.cpp:2778-2780` |
| `primitives` | 事件环 | mutex/rwlock/CV/PC/channel/select/waitgroup | `debug_dump.cpp:2781-2782` |
| `history` | ResourceId（复用 gid 参数） | 单资源事件时间线 + 栈/父链 + 统计 | `debug_dump.cpp:2783-2786`、`2490-2544` |
| `locks` | 资源注册表 + 事件 | 锁资源事件/持有/等待/唤醒聚合 | `debug_dump.cpp:2787-2788`、`2546-2595` |
| `channels` | 资源注册表 + 事件 | Channel 收发/等待/唤醒聚合 | `debug_dump.cpp:2789-2790`、`2597-2643` |
| `resources` | 资源注册表 | 全部资源 + 10 类覆盖矩阵 | `debug_dump.cpp:2791-2792`、`2645-2702` |

`history`/`locks`/`channels`/`resources` 不在 `VIEWS`/HELP 的 6 行清单里
（`debug_server.cpp:979-981`、`1028-1031`），但服务端会接受并渲染；这是「帮助文本
比实现保守」，不是能力不存在。所有视图共享同一层 window 元数据：
`event_count`/`first_ts_ns`/`last_ts_ns`/`gaps`/`truncated`/`total_published`/
`capacity_per_thread`/`critical_capacity_per_thread`/`threads`/`dropped_threads`
（`debug_dump.cpp:1129-1164`）与累计 `totals`（`debug_dump.cpp:1166-1216`）。

`gaps` 按 `thread_id` 分组检测 per-thread seq 跳号；`truncated` 同时考虑跳号与
「累计发布数 > 本次窗口条数」（`debug_dump.cpp:1139-1155`）。看到 `truncated=true`
时只能看趋势，不能据此断言「没有发生过某事件」。

## 5. DebugServer 与 Doctor

### 5.1 DebugServer（只读）

- socket：`/tmp/go2cpp-<pid>.sock`，权限 0600，一连接一命令（`debug_server.cpp:394-396`、
  `428-448`）；
- 命令：`PING` / `LIST` / `STATUS` / `DUMP <component>|ALL` / `TEXT` / `JSON` /
  `EVENTS [limit]` / `VIEW <name> [n]` / `VIEWS` / `SUBSCRIBE events [interval_ms]
  [max_events]` / `HELP`（`debug_server.hpp:16-27`）；
- 响应头是 `GO2CPP-DEBUG/1 OK <payload_bytes>\n<payload>`，`payload_bytes` 是精确
  字节数；错误码固定 `bad-command`/`unknown-component`/`disabled`/`internal`
  （`debug_server.hpp:63-72`）；
- `SUBSCRIBE` 从当前最新游标开始，服务端独立线程按 interval 轮询
  `ReadSince(EventCursor, max_events)`，慢客户端用 `MSG_DONTWAIT` 丢弃最旧整批并
  在 `dropped_total`/`dropped_backpressure` 如实上报（`debug_server.hpp:29-61`）；
- OFF 构建 `Start()` 返回 false、`last_error()=="debug-dump-disabled"`，但进程内
  注册表与 `Dump()` 仍可用（`debug_server.cpp:398-407`、`debug_server.hpp:74-78`）；
- 组件 provider 只保存指针/weak_ptr，不延长被观测对象生命周期
  （`debug_server.hpp:80-85`、`197-218`）。

### 5.2 Doctor（受控诊断）

Doctor 补 DebugServer 明确不做的三件事：受控唤醒、等待图、Mutex 级受控解锁
（`diagnostic_agent.hpp:9-17`）。线程模型是单接收线程 + 单执行线程 + FIFO 队列：

```text
transport ──Recv──▶ [接收线程] ──enqueue──▶ [命令队列] ──▶ [执行线程（唯一）]
                                                                    │
                                    发送（按 stream_id/peer，分块）◀──┘
```

执行线程严格串行，因此一条 `status` 内的多个字段是相邻读取、不会与另一条命令交叉；
接收线程只做 I/O，长命令执行期间新命令仍能入队；两条线程都不经过 Scheduler，
即使所有 M 都 park 也能响应（`diagnostic_agent.hpp:18-36`）。队列满立即回
`queue-full`，默认上限 1024（`diagnostic_agent.hpp:251-270`）。

| 命令 | 语义 | 实现 |
|---|---|---|
| `status` | pid/uptime/队列/协程计数/M-P-runqueue/锁等待/模块与 build-id | `diagnostic_agent.cpp:1767-1817` |
| `dump [sections=a,b]` | 复用 DebugServer 组件注册表，不另造 schema | `diagnostic_agent.cpp:2108-2157` |
| `force-wake scope=...` | 只唤醒白名单对象 | `diagnostic_agent.cpp:2163-2306` |
| `deadlock-report` | 等待图 + verdict + M 状态 | `diagnostic_agent.cpp:2313-2535` |
| `mutex-unlock name=<n>` | 仅 owner 已证明不会再访问临界区时释放一次 | `diagnostic_agent.cpp:2553-2834` |
| `ctx.cancel` | 只取消已注册 Context | `diagnostic_agent.cpp:2835-3087` |
| `inject-fiber fn=<n>` | 在诊断 Fiber（受管 G）内执行注册函数 | `diagnostic_agent.cpp:3088-3156` |
| `stats` | Doctor 自身队列/执行/拒绝/死锁统计 | `diagnostic_agent.cpp:3157-3231` |

**白名单**（`diagnostic_agent.hpp:54-71`）：`scheduler-idle`（只唤醒 worker 重查
runqueue）、`parking-condition`（标准 notify_all，等待方重检谓词）、`channel`
（只发 spurious wake，不伪造数据、不关闭）、`all`（三者组合）。`scope=mutex`
一律 `refused`：Mutex 等待者醒来不会重试同一把锁（FIFO 交接），盲目唤醒会破坏锁
状态，正确做法是 `mutex-unlock`。`mutex-unlock` 只接受**注册名**，不接受裸地址；
owner 存活/不可知/无等待者时拒绝（`diagnostic_agent.hpp:65-71`、`86-91`）。

**死锁判定**（`diagnostic_agent.hpp:307-314`、`diagnostic_agent.cpp:3319-3345`、
`3411-3431`）：每 `detect_interval`（默认 500 ms）算一次进展指纹
（scheduler 计数 + 最多 512 个 G 的状态 + M 状态/blocking_task + 各唤醒目标的
describe + Mutex owner/waiters，`diagnostic_agent.cpp:3232-3308`）；指纹变化即
`kProgressing`；无进展时若没有任何 waiting G/等待者则 `kIdle`，若有 M 在
`kRunning` 或 `kBlocking` 则 `kProgressing`（原生阻塞可能正在推进 IO，
`3375-3382`），否则按 `deadlock_threshold`（默认 5 s）落到 `kStalled`/`kDeadlock`。

`auto_force_wake` 默认关闭：检测到死锁只报告，不自动干预
（`diagnostic_agent.hpp:259-263`）；`DiagnosticAgentStats` 里
`deadlock_events`/`auto_force_wakes`/`forced_unlocks` 可审计
（`diagnostic_agent.hpp:288-305`）。

### 5.3 帧协议 G2CD/1

24 字节小端头：magic `G2CD`、version 1、kind（1 request/2 response/3 error）、
flags（chunked/last_chunk/json）、stream_id、seq、payload_len（上限 16 MiB）、
FNV-1a/32 校验和（`diagnostic_protocol.hpp:18-31`、`75-83`）。校验和只用于发现
截断/串线，不是密码学认证；`Decode` 严格校验 magic/version/kind/flags/长度/
checksum，任何不一致直接报错不猜（`diagnostic_protocol.hpp:50-55`、`224-292`）。
`DiagnosticFrameParser` 处理粘包/拆包，缓冲上限 = 单帧上限 + 帧头
（`diagnostic_protocol.hpp:299-375`）。传输层可插拔：Unix socket（默认，每连接
一条命令）、内存 transport（测试）、用户 topic 字节流
（`diagnostic_agent.hpp:43-52`、`124-175`）。

编译开关：`GO2CPP_DIAGNOSTIC`（默认 OFF）才编译 `diagnostic_agent.cpp` 并加入
`go2cpp_debug`；OFF 构建没有 Doctor 代码、线程、socket，ctest 用例数也不变
（`diagnostic_agent.hpp:72-76`、`cmake/Go2CppOptions.cmake:79-86`）。

## 6. BlockingRegion / sysmon 的集成点

debug 模块**不实现** BlockingRegion，它由调度器提供（`scheduler.hpp:593-603`），
进入时把 M 标到 `kBlocking` 并记录起始时间，离开时恢复（`scheduler.cpp:3383-3451`）。
debug 侧看到的是它的**观测量**，不是另一套状态：

| 观测点 | 字段 | 引用 |
|---|---|---|
| M 快照 | `MState::kBlocking`、`processor_detached`、`blocking_task`、`long_syscall_count`、`replacement` | `scheduler.hpp:298-315` |
| scheduler dump | `sysmon_running`/`sysmon_passes`、每 M 状态 | `debug_dump.cpp:232-233`、`debug_scheduler.hpp:103-104` |
| 死锁判定 | `kBlocking` 的 M 视为「可能推进 IO」，不判全局死锁 | `diagnostic_agent.cpp:3375-3382` |
| 进展指纹 | `machine.blocking_task` 参与哈希 | `diagnostic_agent.cpp:3277-3281` |
| 等待图 | `parked_machines`/`running_machines`/`blocking_machines` 三条计数 | `diagnostic_agent.cpp:2352-2390` |

因此「BlockingRegion 集成」的正确说法是：sysmon 在 M 层维护阻塞事实，debug 只读
这些事实并在 dump/等待图中呈现；它无法替 sysmon 决定「是否补 M」，也不改变
BlockingRegion 的嵌套规则（`scheduler.cpp:214`、`4239`）。

## 7. 可靠性边界（合并旧文档结论）

`docs/debug-reliability.md` 的核心结论在当前实现里仍然成立，但需按 v6/v7 修正：

**可以做事实判断的**：

- 8 类已插桩并发原语的争用/等待/唤醒事件（资源事件按 1/4 记录，但
  blocked/wake/timeout/cancelled/closed 全量）；
- park 的进入/返回/拒绝与唤醒系列（关键区，默认不被普通事件覆盖）；
- 资源注册表、栈样本、Fiber 链样本的 id 与内容（采样未命中时 `stack_id=0`，
  不能伪造成「无栈」，`debug_resources.hpp:39-44`）。

**只能当趋势/线索的**：

- scheduler 事件：普通区 256 条 + 关键区 64 条 + 1/8 记录采样，必然截断；
- io/network：DebugServer 没有默认注册 IO 组件，只有 fd 级计数，不能凭「没看到」
  断言没有 IO 阻塞；
- timer：来源默认 `kUnknown`，执行器路径没有 `timer_id`（事件字段语义见
  `debug_event_ring.hpp:232-235`、`debug_dump.cpp:2778-2780`）；
- 迁移/回放：只覆盖声明过的窄路径（接收等待/通信与时钟），锁与 IO 不记录。

**判断规则**：能用资源事件/park 事件直接看到的写结论；只能从 scheduler/io/timer/
迁移/回放看到的写假设，并注明需要交叉验证。窗口是否截断先看
`window.truncated`/`gaps`，一致性先看 `changed_during_dump`。

**Doctor 的已知限制**（`diagnostic_agent.hpp:78-92`）：执行线程调用的快照接口会取
运行时内部互斥量，若业务在持该锁时原生死锁，`status` 也可能挂起；等待图只能到
资源级，Mutex 边标 `waiter_ids=unavailable`，不虚构 G→Mutex 边；`mutex-unlock`
只接受注册名；QUIC transport 依赖 `GO2CPP_WITH_PICOQUIC=ON`。

## 8. 开销（历史数据与 v6/v7 修正）

`docs/debug-overhead.md` 在 2026-10-21（v5 基线）测到的全开开销：

| 场景 | OFF 吞吐 | 全开吞吐 | 全开慢倍数 |
|---|---:|---:|---:|
| channel（Fiber ping-pong） | 2,011,419 | 100,511 | 20.0× |
| mutex（4 线程争用） | 39,521,349 | 2,096,489 | 18.9× |
| spawn（Fiber 创建/恢复） | 156,910 | 38,355 | 4.09× |
| timer（arm/fire/回调） | 244,070 | 67,857 | 3.60× |
| resources（RW/CV/WG） | 64,876,069 | 1,609,907 | 40.3× |
| select（两路） | 834,144 | 89,337 | 9.34× |

（单位 ops/s，N=200,000，3 轮中位数。）旧文档把主因归为「栈/Fiber 链捕获 +
全局 `seq` 的 fetch_add 缓存行弹跳」。**当前实现已改掉后一项**：v6 起 `seq` 是
每线程自增、无全局原子（`debug_event_ring.hpp:18-24`、`debug_event_ring.cpp:876-877`）；
v7 又把资源记录、资源栈/链、调度高频事件拆成三个采样周期
（`debug_event_ring.hpp:399-435`）。因此：

- 上表只能作为**最坏上界**，不能引用为当前开销；
- 当前复测必须重新构建 `GO2CPP_DEBUG_DUMP=ON` 与 OFF 两套二进制，按
  `tests/bench` 或旧文档 §7 的命令重跑；
- 结论性建议不变：默认 OFF；排障按类别开（`GO2CPP_DEBUG_CATEGORIES`），
  `stacks`/`chains` 只在需要调用栈/父链时开；`GO2CPP_DEBUG_SAMPLE_SHIFT` 对
  关键事件无效。

## 9. 设计说明

1. **为什么 v6 把 seq 改成每线程自增**：v2–v5 的进程级 `fetch_add` 让每个
   park/wake 事件都争同一条 cacheline，在 park 密集场景是主要固定成本；稳定
   `thread_id` 只需线程首次记录时分配一次，之后全序仍可由
   `(ts_ns, thread_id, seq)` 恢复（`debug_event_ring.hpp:18-26`、
   `debug_event_ring.cpp:151-200`）。
2. **为什么环形缓冲要拆两段**：普通事件（park 进入、runqueue push/pop）频率高，
   会把 256 条窗口迅速写满；关键事件（wake、enqueue 拒绝、claim 失败、G 终态、
   Doctor 干预）必须活到被消费方看到。两段互不覆盖是最小成本的隔离；关键区仍
   有限，所以文档明确「不丢全部」而不是「绝对不丢」（`debug_event_ring.hpp:39-53`）。
3. **为什么采样要分层（v7）**：v5 只有一个全局 shift，对资源事件几乎无效——事件量
   大头来自「记录」而不是「条数」。v7 把记录、栈/链捕获、调度高频事件拆成三个
   独立周期，同时让 blocked/wake/timeout/cancelled/closed 始终全量，保证
   park↔wake 的配对语义不被采样破坏（`debug_event_ring.hpp:399-416`、
   `debug_resources.hpp:202-214`）。
4. **为什么 Doctor 只用一个执行线程**：一条 `status` 的字段来自多次独立读取；
   并发执行两条诊断命令会让字段互相穿插，读者无法判断它们是否同一时刻。串行执行
   是一致性的最低成本实现；接收线程只做 I/O，保证长命令执行期间请求仍能入队
   （`diagnostic_agent.hpp:18-36`）。
5. **为什么 force-wake 是白名单而不是「任意唤醒」**：Mutex 的等待者醒来不会重试
   同一把锁（FIFO 交接），盲目唤醒会破坏锁状态；Channel/ParkingCondition 的等待方
   本来就会重检谓词，spurious wake 是安全语义。未注册对象一律不碰，避免诊断工具
   成为「把目标进程打崩」的捷径（`diagnostic_agent.hpp:54-71`、`86-91`）。
6. **为什么 dump 用稳定的 text/JSON 契约**：CLI、DebugServer、重放服务只依赖
   `schema=`/`key=value` 与 JSON 字段，不依赖组件内部类型布局；新增字段不会让
   外部工具重新编译（`debug_dump.hpp:9-27`）。

## 10. API 一览（按层）

| 层 | API | 位置 | 说明 |
|---|---|---|---|
| 配置 | `DebugConfig::Enabled/Get/Set/Reset/ParseCategories/LoadFromEnv` | `debug_config.hpp:98-141` | 6 类事件 + stacks/chains + sample_shift |
| 事件环 | `EventRing::Record/RecordSampled/RecordTimer` | `debug_event_ring.hpp:551-579` | OFF 构建空操作 |
| 事件环 | `Snapshot/CurrentCursor/ReadSince/DumpText/DumpJson/WriteToFile` | `debug_event_ring.hpp:620-651` | 只读，不调用调度器 |
| 事件环 | `BindProcessor/UnbindProcessor/BindTask/BoundTask` | `debug_event_ring.hpp:581-600` | 供 channel/mutex 填 pid/gid |
| 事件环 | `TotalCount/TotalCountAll/PublishedEvents/ThreadCount/DroppedThreads` | `debug_event_ring.hpp:602-614` | 覆盖率与截断判断 |
| 资源 | `RegisterResource/UnregisterResource/LookupResource/FindResourceByAddress` | `debug_resources.hpp:124-155` | id 单调不复用；OFF 返回 0 |
| 资源 | `CaptureStackSample/LookupStackSample/ResolveStackSample` | `debug_resources.hpp:188-225` | 16 帧；解析在视图输出时做 |
| 资源 | `CaptureFiberChain/LookupFiberChain` | `debug_resources.hpp:263-290` | 9 节点父链 |
| 资源 | `RecordResourceEvent/ResourceEventWanted/SetResourceReplaySink` | `debug_resources.hpp:369-420` | 宏优先，避免构造被稀释的 112 B |
| dump | `DumpScheduler/DumpFiber/DumpChannel/DumpMutex/DumpTimer/DumpEvents/DumpRuntimeInfo` | `debug_dump.hpp:191-221` | 统一 `DumpPayload` |
| dump | `BeginConsistency/EndConsistency/AppendConsistency` | `debug_dump.hpp:169-176` | 一致性提示，不是事务 |
| dump | `BuildViewJson/IsKnownViewName/WriteDumpFile` | `debug_dump.hpp:227-255` | 视图 / 原子写文件 |
| 服务 | `DebugServer::Start/Stop/Register/Unregister/Dump/HandleCommand` | `debug_server.hpp:116-152` | 只读；SUBSCRIBE 走 socket |
| Doctor | `DiagnosticAgent::Start/Stop/Execute/stats/deadlock_verdict` | `diagnostic_agent.hpp:349-484` | 单线程命令平面 |
| Doctor | `RegisterScheduler/RegisterParkingCondition/RegisterChannel/RegisterMutex/RegisterContext` | `diagnostic_agent.hpp:369-436` | 未注册对象不会被触碰 |
| Doctor | `InstallDiagnosticFiber` / `RunOnDiagnosticFiber` | `diagnostic_agent.hpp:453-468` | 在受管 G 内执行注册函数 |
| 宏 | `GO2CPP_DEBUG_EVENT/SAMPLED/TIMER_EVENT` | `debug_event_ring.hpp:661-737` | OFF 时零求值 |
| 宏 | `GO2CPP_DEBUG_RESOURCE_EVENT*` / `GO2CPP_DEBUG_RESOURCE_REGISTER` | `debug_resources.hpp:466-604` | OFF 时零求值 |
| 断点 | `DebugBreakpoints::Add/Remove/Clear/Checkpoint/WaitForResume` | `debug_breakpoints.hpp:125-288` | 协作式，只能停在调用点 |
| 信号 | `DebugSignalDumper::Install/Uninstall/PollOnce/StartWatcher` | `debug_signal_dump.hpp:88-120` | 处理器只做一次原子置位 |

## 11. 测试（对应 ctest）

构建：`cmake -S . -B /tmp/build-ci-sim -G Ninja -DGO2CPP_DEBUG_DUMP=ON
-DGO2CPP_DIAGNOSTIC=ON`，然后 `cmake --build /tmp/build-ci-sim -j12`。

| ctest | 覆盖 | 源码 |
|---|---|---|
| `go2cpp_tests` | 15 个 debug/诊断子模块（filter：`debug_fiber`/`debug_scheduler`/`debug_cli`/`debug_sync`/`debug_lock_selector`/`debug_events`/`debug_breakpoints`/`debug_signal_dump`/`debug_dump`/`debug_config`/`resource_replay`/`resources_v5`/`timer_debug`/`timer_replay`/`diagnostic`） | `tests/test_main.cpp:114-129` |
| `go2cpp_ctx_cancel_test` | Doctor Context 取消 | `cmake/Go2CppTests.cmake:112` |
| `go2cpp_park_wake_pair` | park↔wake 配对与解释率 | `cmake/Go2CppTests.cmake:152` |
| `go2cpp_resource_history_stress` | 资源历史高并发压测 | `cmake/Go2CppTests.cmake:162` |
| `go2cpp_resource_history_test` | history/locks/channels/resources 视图 | `cmake/Go2CppTests.cmake:844` |
| `go2cpp_deadlock_detection` | 死锁判定闭环 | `cmake/Go2CppTests.cmake:263` |
| `go2cpp_debug_record_correctness` | 事件记录正确性（撕裂/并发） | `cmake/Go2CppTests.cmake:274` |

单模块快速回归：

```bash
GO2CPP_TEST_FILTER=debug_dump /tmp/build-ci-sim/go2cpp_tests
GO2CPP_TEST_FILTER=diagnostic /tmp/build-ci-sim/go2cpp_tests
ctest --test-dir /tmp/build-ci-sim -R 'park_wake_pair|resource_history|deadlock' --output-on-failure
```

`tests/debug/run_diagnostic_e2e.sh` 是真实进程 + 真实 Unix socket 的端到端脚本
（14 项断言：READY、卡死期间 status、build-id、dump 组件、duration 元数据、
deadlock 判定、等待图边、mutex scope refused、force-wake 恢复、恢复后 status、
并发 10 命令、concurrent_peak=1、死锁事件计数），不在 ctest 默认集合里。

## 12. 合并说明与变更记录

- 合并 `docs/debug-reliability.md`：可靠性边界、10 类结构能力表、窗口截断与
  采样偏差、dump 性能数据；正文保留为历史快照。
- 合并 `docs/debug-overhead.md`：ON/OFF 六场景压测、类别归因与「按需开类别」建议；
  数据标注为 v5 基线，v6/v7 后需复测。
- 合并 `docs/diagnostic-channel.md`：Doctor 与 DebugServer 分工、单线程命令平面、
  G2CD/1 帧协议、命令语义、白名单、编译开关与 e2e 测试；正文保留为历史快照。
- 2026-10-26：按 `HEAD 27e892b` 核对行号与实现，补 §2.3 采样三分、§6
  BlockingRegion 集成点、§4.3 四个资源视图（旧文档只写 6 个基础视图）。
