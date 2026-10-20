# 语义级 record/replay 服务

> 状态：**v2**（2026-10-20）。v1 是最小实现（i64 + send/recv/close）；
> v2 增加 select、Context/超时/虚拟时钟、非 i64 值、自动注册入口、检查点分片。
> 代码：`tools/replay/**`（独立工程，不接入顶层 CMake，默认构建行为不变）。
> 定位：**语义级**记录/重放——记录调度与通信的**决定点**，而不是 rr 那样的
> 指令级执行。目标是让「偶发并发 bug」在离线、单线程、可重复的条件下重现。

---

## 1. 问题与目标

上一轮定位 2–3% 卡死靠的是调度事件环（`33/0`、`32/1`、`8/769` 这类
`已入队/已唤醒` 计数）。它能指出「有 G 没被唤醒」，但无法回答两个问题：

1. 这次卡死的**具体交错**是什么（谁先 park、谁没 wake）？
2. 修好之后，同样的交错还能不能重演一遍来确认？

语义级 replay 的答案：

- **记录**：所有改变交错的决定点（交接、select 决策、park、wake、close、
  context 取消/超时、fiber 生死）；
- **重放**：不重新跑真实调度器，而是**按记录的交错**在单线程上恢复每个
  fiber；channel/select/时钟/context 的结果全部来自记录；
- **校验**：重放产生的消息序列、select 决策序列、收发计数必须与记录一致；
  记录本身必须自洽（每个 park 都有 wake、每个 blocked 操作都有 park、
  send/recv/xfer 成对、select 的 chosen 在 case 表范围内、文件以 `kEnd` 收尾）。

v2 覆盖范围：单 `Scheduler`、N 个 fiber、无缓冲/有缓冲 channel 的
send/recv/close、**select（多 channel 竞争 / default / timeout / ctx）**、
**Context 取消与 deadline**、**i64 与 bytes 两类值**、**编译期自动入口**、
**检查点分片与只重放到出事点**。

---

## 2. 事件模型

### 2.1 哪些事件必须记录

判断标准：**如果这个决定点的结果不同，最终消息序列就会不同**。

| 决定点 | 事件 | 记录内容 | 为什么必须 |
|---|---|---|---|
| fiber 开始 | `kSpawn` | gid | 重放要按记录的启动顺序恢复 |
| fiber 结束 | `kExit` | gid、收发计数 | 判断「记录是否完整」与「fiber 是否跑完」 |
| channel 发送结果 | `kSend` | gid、ch、op_index、value/data、decision、blocked、xfer_id | 值、成功/关闭、是否等待过 |
| channel 接收结果 | `kRecv` | 同上 | 同上；value 是消息序列的直接来源 |
| 多路选择 | `kSelect` | chosen、选中 case 的 channel、decision、value/data、case 表、blocked | 选中哪个 case 是决定性分支；**不能靠重放重新竞争** |
| 关闭 | `kClose` | decision | 关闭时机决定等待者以 `kClosed` 还是 `kOk` 结束 |
| 挂起 | `kPark` | gid、ch、reason、op_index | **重放的驱动点**：引擎在此检查 fiber 已 park |
| 唤醒 | `kWake` | gid、ch、reason、op_index、xfer_id | **重放的驱动点**：引擎在此恢复 fiber |
| 定时/睡眠 | `kTimer` | kind、duration/deadline、vtime | 虚拟时钟推进；timeout 是 select 的一个 case |
| Context | `kCtx` | ctx_id、kind（cancel/deadline set/fired）、vtime | 记录取消/超时发生的时间与顺序 |
| 元信息 | `kMeta` | gid 数、channel 数、值类型、时钟模式、demo 名 | 文件自描述 |
| 诊断 | `kNote` | 文本 | 注入标记、watchdog、收尾原因、分片检查点 |

不记录、也**不能**驱动重放的东西：真实线程 id、P/M 编号、栈地址、指令。
它们只影响「谁先跑」，而「谁先跑」已经被 park/wake 的 `seq` 固化了。

### 2.2 select 的决策为什么不能重算

真实 select 在多个 channel 同时就绪时，选哪个 case 由运行时（Go 是伪随机
轮询）决定。重放如果重新执行一次 select，会拿到**另一次**随机决策，交错就
断了。因此 v2 把 select 的最终结果压成一条 `kSelect`：

```
kSelect { chosen, channel=选中 case 的 ch, decision, value/data,
          cases=[(channel, kind)...], blocked, xfer_id }
```

重放时 `SyncSelect::Wait()` 直接取这条记录并返回 `chosen`，不轮询 channel、
不重新竞争。若 select 的交接唤醒了 parked sender/receiver，channel 侧照常
写 `kWake`；若 select 自己被唤醒（交接/超时/ctx），唤醒方写一条 `kWake`，
select 醒来后写 `kSelect(blocked=1)`。

### 2.3 字段

| 字段 | 类型 | 含义 |
|---|---|---|
| `seq` | u64 | 全局线性化序号；**在产生决定点的锁内分配** |
| `ts_ns` | u64 | `steady_clock` 纳秒；仅同机比较 |
| `vtime_ns` | u64 | **虚拟时间**（相对录制起点的纳秒）；重放只信这个值 |
| `type` | u16 | 事件类型（`EventType`） |
| `flags` | u16 | bit0=注入/丢失，bit1=失败，bit2=收尾合成 |
| `gid` | u32 | 逻辑 fiber id（0 起）；跨线程迁移后仍不变 |
| `channel` | u32 | 逻辑 channel id；`0xFFFFFFFF` = 无 |
| `xfer_id` | u32 | 一次交接的唯一 id，send/recv 两侧相同；0 = 非直接配对 |
| `op_index` | u32 | 该 gid 的第几次 channel/select 操作（0 起）；重放查表主键 |
| `value` | i64 | 整型消息值；bytes 值存其 FNV-1a 64 摘要 |
| `data` | bytes | `ValueKind::kBytes` 的序列化值（string/bytes/结构体） |
| `decision` | u8 | `ok/closed/already-closed/would-block/cancelled/timed-out/stuck` |
| `blocked` | u8 | 0=立即完成；1=经 park 后完成；2=未完成 |
| `reason` | u8 | park/wake 原因：`send/recv/select/close/timeout/context/other` |
| `chosen` | u16 | select 选中的 case 下标 |
| `cases` | list | select 的 `(channel, kind)` 表；kind=`recv/send/default/timeout/context` |

### 2.4 为什么 `seq` 必须在锁内分配

重放的驱动顺序来自 `kPark/kWake` 的 `seq` 顺序。如果 `seq` 在锁外分配，
两个 fiber 在同一个临界区内完成的交接可能拿到与线性化点相反的顺序，
重放就会走到记录中根本不存在的状态。

实现上：`SyncChannel` 的所有事件写入都在 **组锁**（或独立的 `m_mutex`）内
完成，`Recorder::Write()` 在文件锁内部分配 `seq` 并立即 append，因此

```
文件顺序 == seq 顺序 == 真实线性化顺序
```

三者一致，流式写也不会有乱序。

### 2.5 select 的组锁

Go 的 select 要原子地在多个 channel 上「轮询 + 登记等待」。v2 选择更小、
可验证的边界：**参与同一个 select 的 channel 必须绑定同一个 `SelectGroup`**
（`SyncChannel` 构造参数），组内所有 channel 操作与 select 共用一把组锁。

- 语义等价：poll / 登记 / claim 全部在一把锁内完成；
- 代价：组内操作串行化（demo 规模足够；见 §7 限制）；
- 不在任何 group 里的 channel 仍使用自己的 `m_mutex`，**行为与 v1 完全一致**。

select 的等待者带 `SelectState`（原子 `claimed` + `ParkingCondition`），
谁先 `Claim()` 成功谁完成这次 select；其余 case 的登记由 select 醒来后撤销。
Context 的 `Cancel()` 只持 ctx 锁，用同一个 CAS 判定赢家，不需要回头拿组锁，
因此不存在反向锁顺序。

---

## 3. 记录格式（v2，向后兼容 v1）

### 3.1 文件布局

```
FileHeader (32 B)
Record*                 // 逐条 append，可流式写
Record(kEnd)            // 最后一条；带总条数与整文件滚动 CRC
```

### 3.2 FileHeader（32 字节，小端）

| 偏移 | 大小 | 字段 | 值 |
|---|---|---|---|
| 0 | 8 | magic | `"G2CRPLY\0"` |
| 8 | 2 | version | `2`（读端接受 1/2） |
| 10 | 2 | header_size | `32` |
| 12 | 4 | flags | bit0=逐条 CRC32 |
| 16 | 8 | created_unix_ns | 墙上时间（仅供人看） |
| 24 | 8 | producer_id | 录制器常量，便于区分来源 |

### 3.3 RecordHeader（40 字节，小端）

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 4 | size（整条字节数，>= 40） |
| 4 | 2 | type |
| 6 | 2 | flags |
| 8 | 8 | seq |
| 16 | 8 | ts_ns |
| 24 | 4 | gid |
| 28 | 4 | channel |
| 32 | 4 | aux（send/recv/select/park/wake 存 `xfer_id`） |
| 36 | 4 | crc32（覆盖 `[0,36)` + payload，IEEE 多项式 `0xEDB88320`） |

### 3.4 Payload

| 类型 | 编号 | payload |
|---|---|---|
| `kMeta` | 1 | `u32 n_gids, u32 n_channels, u32 value_kind, u32 rsv, string demo, [u32 ext_flags, u32 clock_mode]` |
| `kSpawn` | 2 | 空 |
| `kExit` | 3 | `i64 sent, i64 recv` |
| `kSend` / `kRecv` | 10 / 11 | `u8 decision, u8 blocked, u16 rsv, u32 op_index, i64 value, [u64 vtime_ns, string data]` |
| `kSelect` | 12 | `u8 decision, u8 blocked, u16 chosen, u32 op_index, i64 value, [u64 vtime_ns, string data, u32 n_cases, {u32 channel, u8 kind, u8 rsv, u16 rsv}*]` |
| `kClose` | 13 | `u8 decision, u8 blocked, u16 rsv, u32 op_index, [u64 vtime_ns]` |
| `kTimer` | 14 | `u8 kind, u8 rsv, u16 rsv, u32 op_index, u64 timer_ns, u64 vtime_ns, string name` |
| `kCtx` | 15 | `u32 ctx_id, u8 kind, u8 rsv, u16 rsv, u32 op_index, u64 vtime_ns` |
| `kPark` / `kWake` | 20 / 21 | `u8 reason, u8 op, u16 rsv, u32 op_index, [u64 vtime_ns]`（`xfer_id` 在 aux） |
| `kSched` | 30 | `u32 sched_type, u32 machine, u32 processor` |
| `kNote` | 40 | `string text` |
| `kEnd` | 99 | `u64 records, u64 dropped, u32 file_crc, u32 rsv` |

`string` = `u32 len + len 字节 UTF-8`。方括号是 v2 的**尾部追加**字段。

**v1 兼容策略**：v2 只在 v1 payload 尾部追加字段。v1 解码器读完自己认识
的字段后不检查剩余字节，因此 v2 文件在旧工具里仍可读（新事件类型按未知
类型跳过 payload）；v2 解码器遇到 v1 文件时按 `remaining()` 判断追加字段
不存在（`data` 为空、`vtime_ns=0`）。

`kMeta.ext_flags`：bit0=支持 bytes 值 codec，bit1=虚拟时钟。
`clock_mode`：0=真实时钟，1=虚拟时钟。
`value_kind`：0=i64，1=bytes，2=mixed。

### 3.5 校验

- **逐条 CRC32**：任何一条记录损坏都能在解码时定位到偏移；
- **文件级滚动 CRC**：`kEnd.file_crc` = 文件头之后到 `kEnd` 之前所有字节的
  滚动 CRC32（`Crc32(a||b) == Crc32(b, len_b, Crc32(a, len_a))`）。流式写入
  时边写边算，不需要回填；读取时一次校验即可发现截断/篡改。
- **`kEnd` 缺失**：视为记录被截断（进程被 kill），报告 `complete=false` 并按
  stuck 处理——卡死进程被强杀时正是这个形态。

### 3.6 分片的保真

`split` 产生的前缀分片原样复制事件（`seq/ts/vtime` 不变），每个分片重新
计算文件级 CRC，因此分片与源文件的检查点可以直接用 `seq` 对照。

---

## 4. Replay 语义

### 4.1 三种入口

| 模式 | 入口 | 是否需要原程序 | 能力 |
|---|---|---|---|
| 静态重演 | `ReplayFile(path)` | 否 | 文件校验、统计、park/wake 配对、stuck 判定、时间线、检查点、select 自洽 |
| 完整重放（显式） | `ReplayFileWith(path, factory)` | 是（factory） | 单线程按记录交错重跑，校验消息/select 序列 |
| 完整重放（自动） | `ReplayFileAuto(path)` / CLI `play` | 是（`RECORD_FIBER_ENTRY`） | 编译期注册入口，命令行直接重跑 |

### 4.2 完整重放算法

前提：channel/select/clock/context 使用 replay 模式——每个操作按
`(gid, op_index)` 查记录，**不执行真实的状态机**；记录说需要等待，就调用
`ReplaySource::ParkCurrent()` 把 CPU 交还引擎。

```
engine:
  for event in events:                      # 文件顺序 == seq 顺序
    if stop_at_seq and event.seq > stop_at_seq: break
    case kSpawn(gid):  create fiber(gid); resume(gid)   # 跑到第一个 park/exit
    case kPark(gid):   assert fiber(gid) 已停在 park 点
    case kWake(gid):   resume(gid)                       # 从 park 返回
    case kExit(gid):   assert fiber(gid) 已结束
    case kTimer:       advance(vtime + duration)
    case 其它:         跳过（fiber 侧查表时已使用）

fiber:
  while 未结束:
     op = source.NextOperation(gid)         # 第 op_index 个 send/recv/select/close
     if op.blocked: source.ParkCurrent(gid) # 交还 CPU，等 kWake 恢复
     return op.decision / op.value / op.data
```

关键不变量：

1. `kPark` 一定先于对应的 `kWake`（录制端在**解锁前**写 `kPark`，唤醒方
   拿锁才能交接，所以 wake 的 seq 必然更大）；因此引擎看到 `kWake` 时
   fiber 一定处于 parked 状态。
2. `kSpawn` 的到达顺序是**真实调度顺序**（可能 gid 1 先于 0），不是编号
   顺序；逻辑 gid 与 ucontext fiber id 之间必须显式建双向映射。
3. replay 模式下 `CurrentGid` 不能靠 thread_local：单线程上多个 fiber 交替
   执行会互相覆盖，必须由引擎按「当前 ucontext」返回（`CurrentFiberGid()`）。
4. select 的 `chosen` **只读记录**；重放不重新轮询，也不重新随机。

### 4.3 虚拟时钟与 Context

- 录制端 `ReplayClock::SleepFor(ns)` 真实睡眠，并写一条
  `kTimer(kSleep, timer_ns=ns, vtime=当前虚拟时间)`。
- 重放端 `SleepFor(ns)` 不睡：取 `(gid, sleep_index)` 对应的 `kTimer`，把
  虚拟时钟推进到 `vtime+ns` 后立即返回。因此录制时 100ms 的等待，重放
  只需要微秒级（实测 1.2ms vs 101ms）。
- `ReplayContext::SetDeadline(vtime)` 记录 `kCtx(kDeadlineSet)`；select 的
  live 等待用 `ParkingCondition::wait_until`（受管 G 由 `TimerService`
  真实唤醒），到期后写 `kTimer(kTimeoutSelected)`、`kCtx(kDeadlineExceeded)`、
  `kWake(reason=timeout)`，再写 `kSelect(decision=timed-out)`。
- `ReplayContext::Cancel()` 写 `kCtx(kCancelled)`，claim 等待它的 select 并
  写 `kWake(reason=context)`，再 notify。
- 重放端不读真实时钟；`report.virtual_ns` 是记录推进到的虚拟时间。

### 4.4 校验与 stuck 判定

完整重放结束（或中途失败）时检查：

- 每个启动的 fiber 是否 `kDone`；否则 **stuck**，报告 gid / channel / op；
- `kExit` 到达时 fiber 是否已结束；若仍在 park，说明**唤醒丢失** → stuck；
- 记录结束时 fiber 还要执行操作（`NextOperation` 越界）→ stuck；
- 每 gid 的 send/recv 序列与记录逐项比较（i64 与 bytes 都比较）；
- 每 gid 的 select `chosen` 序列与记录逐项比较；全局 select checksum 对照；
- 静态重演额外检查：`kPark` ↔ `kWake` 按 `(gid, op_index)` 一一配对、
  `blocked=1` 必有 park、`xfer_id` 必须是同 channel 上的一对 send/recv
  （select 选中 recv/send 也计入）、`kExit` 的计数等于该 gid 的操作数、
  `kSelect.chosen` 在 case 表范围内、文件以 `kEnd` 收尾。

### 4.5 故障注入：丢失唤醒

两种注入都走同一条通道 `Recorder::ShouldDropWake()` / 重放端
`ReplayOptions::drop_wake_at`：

- **录制端**（`GO2CPP_REPLAY_DROP_WAKE=N`）：第 N 次唤醒**不 notify、不写
  `kWake`**，等待者真的永远不被唤醒。进程卡死，看门狗在超时后补写
  `kNote("watchdog: ...")` + `kEnd` 并 `_Exit(0)`；
- **重放端**：正常记录 + 第 N 个 `kWake` 不 resume，重演卡死。

判定依据：记录中该 `kPark` 没有配对的 `kWake`（静态重演），或 fiber 在
`kExit` 到达时仍 parked（完整重放）。select 的等待者同样适用。

### 4.6 检查点分片与「只重放到出事点」

两种方式：

1. **不产生新文件**：`ReplayOptions::stop_at_seq` / CLI `--until=SEQ`。
   静态分析只统计前缀，返回 `kPartial`（不判 stuck）；完整重放只调度到该
   seq，保留 fiber 停在任意状态，也返回 `kPartial`。已执行部分的消息/select
   序列仍必须与记录前缀一致。
2. **产生可独立重放的前缀分片**：`replay split --every-events=N`（或
   `--every-ms=M`）。`partNNNN` = 文件头 + `kMeta` + 事件 `[0..checkpoint]`
   + `kNote("checkpoint: part ...")` + `kEnd`。中间分片重放得到 `kPartial`
   （到达检查点），最后一个分片是完整记录，重放得到 `kOk`。

分片适合二分定位：从最后一个分片往前试，第一个 `kPartial` 的分片就是
「出事点在它之后」；第一个 `stuck` 的分片说明问题在其覆盖范围内。代价是
存储放大 O(分片数 × 前缀大小)（见 §7）。

### 4.7 值类型扩展

`SyncChannel<T, Codec = ValueCodec<T>>`：

- 整型/枚举：`kind=kI64`，值写入 `Event::value`，`data` 为空（v1 兼容）；
- `std::string` / `std::vector<std::uint8_t>`：`kind=kBytes`，原始字节写入
  `Event::data`，`value` 存 FNV-1a 64 摘要；
- 其它类型：要求提供 `std::string Serialize() const` 与
  `static T Deserialize(const std::string&)`，主模板自动检测；
- 也可用 `SyncChannel<T, MyCodec>` 显式传入自定义 codec。

重放时 `Codec::FromValue(event->value, event->data)` 还原；引擎的序列校验
同时比较 `value` 与 `data`，避免摘要碰撞掩盖内容差异。

---

## 5. 实现与验证

### 5.1 结构

| 文件 | 职责 |
|---|---|
| `include/replay/format.hpp` / `src/format.cpp` | 磁盘格式 v1/v2、CRC32、FNV、编解码 |
| `include/replay/recorder.hpp` / `src/recorder.cpp` | 流式录制、seq、虚拟时间、注入开关 |
| `include/replay/sync_channel.hpp` | 可记录/可重放的 channel（值 codec + 组锁 + select 内部接口） |
| `include/replay/select.hpp` / `select_state.hpp` | select 决策、组锁、等待/claim |
| `include/replay/context.hpp` | ReplayContext：Cancel / deadline / select 集成 |
| `include/replay/clock.hpp` | 虚拟时钟与 sleep |
| `include/replay/value_codec.hpp` | 值序列化钩子 |
| `include/replay/auto_entry.hpp` / `src/auto_entry.cpp` | `RECORD_FIBER_ENTRY` 注册表与 `ReplayFileAuto` |
| `include/replay/split.hpp` / `src/split.cpp` | 前缀分片 |
| `include/replay/gid.hpp` | live 模式 `Task::id() → gid` 反查 |
| `include/replay/ucx.hpp` | 单线程 ucontext 协程（replay 专用） |
| `include/replay/replay_engine.hpp` / `src/replay_engine.cpp` | 静态重演 + 完整重放 + 报告/JSON |
| `demo/*` | fanin / select / context / values 四个场景 |
| `replay.cpp` | CLI：info / play / split |
| `tests/test_replay_tools.cpp` | 格式/分片/注册表单测 |

### 5.2 Demo 输出（一次实际运行）

fanin（96 次无缓冲 rendezvous，v2 格式）：

```
[record] file_bytes=24429 events=394
[record] consumer received=96 checksum=5187324405681548221
[replay] status=ok complete=true events=394
  events: sends=96 recvs=96 parks=96 wakes=96 blocked=96 selects=0
  messages=96 checksum=5187324405681548221 replayed_checksum=5187324405681548221
[replay] consistency=MATCH
```

select（2 个 channel、96 次 select、12 次 sleep）：

```
[select/record] received=96 checksum=12840380439870588009 choices=10910101582189575299
  events: sends=96 recvs=96 parks=96 wakes=96 blocked=96 selects=96 timers=12
  messages=96 checksum=1213212101906809961
  selects: choices=0,0,1,0,... checksum=10910101582189575299
  replay: executed=true
[select/replay] consistency=MATCH
```

Context/超时（录制真实等待 102ms；重放只推进虚拟时钟）：

```
[context/record] wall_ms=102.452
[context/record] phase1=timed-out phase2=cancelled phase3=ok value=42
[replay] status=ok
  events: sends=1 recvs=1 parks=3 wakes=3 blocked=3 selects=2 timers=3 ctx=3
  selects: choices=1,1 checksum=... replayed_checksum=...
  virtual_ns=101070439
[context/replay] wall_ms=1.319 virtual_ms=101.827
[context/replay] consistency=MATCH virtual_clock=OK
```

string/结构体值：

```
[values/record] received=16 checksum=18423838217698236003
[values/replay] received=16 checksum=18423838217698236003
  events: sends=16 recvs=16 parks=16 wakes=16
  messages=16 bytes=16
[values/replay] consistency=MATCH
```

CLI 自动重放与分片：

```
$ replay play /tmp/fanin.g2cr
{"status":"ok","auto_entry":true,"entry_name":"fanin-3x32",...}

$ replay split /tmp/fanin.g2cr --every-events=100 --out-dir=/tmp/parts
{"ok":true,"parts":[{"last_seq":100,...},{"last_seq":393,...}]}

$ replay play /tmp/parts/g2c-fanin.part0001.g2cr
{"status":"partial","stopped_early":true,...}

$ replay play /tmp/fanin.g2cr --until=200
{"status":"partial","stopped_at_seq":200,...}
```

压测脚本（`scripts/run_all.sh`）：

```
[stress] all 10 rounds matched
[select] all 5 rounds matched
[context] all 3 rounds matched with virtual clock
[values] all 3 rounds matched
[chunked] part1=partial part4=ok until=partial
[fault] 演示通过：两种注入都被重放判定为卡死
```

独立单测（`ctest --test-dir /tmp/go2cpp-replay-build`）：

```
1/1 Test #1: go2cpp_replay_tools ... Passed
```

### 5.3 关键修复记录（v2 实现期）

1. **select 等待路径的 out 值**：poll 成功时 `try_case` 写 out，但等待后被
   channel claim 的路径没有写，导致录制端 consumer 收到 0。修复：poll 不再
   写 out，统一在 `WaitLive` 结束时按 `chosen` 调用 `DeliverOut`。
2. **ctx Cancel 缺 kWake**：Context 取消唤醒 select 时没有写 `kWake`，记录里
   select 的 `kPark` 没有配对，重放判 stuck。修复：Cancel 在 claim 成功后写
   `kWake(reason=context)`。
3. **Cancel 的 state 生命周期**：原实现把 waiters 拷贝到锁外再遍历，select
   可能已 Disarm 并销毁 `SelectState`。修复：整个 claim/notify 在 ctx 锁内。
4. **`replayed_select_checksum` 初值**：未用 FNV offset basis 初始化，导致
   与记录侧 checksum 不可比。修复：构造函数显式初始化。
5. **分片最后一片被当成 partial**：每个分片都写 checkpoint note，最后一片
   的完整重放被自动 stop。修复：最后一片不写 checkpoint note。
6. **分片重放越界**：fiber 提前查表可能在引擎到达 stop 前耗尽操作；修复：
   stop 模式下的 `NextOperation/NextSleep` 越界标记 `stopped_early` 而不是
   stuck。

---

## 6. CLI / 库接口

CLI 由另一个 agent 统一实现；本服务只导出稳定入口：

```cpp
#include "replay/auto_entry.hpp"
#include "replay/replay_engine.hpp"

// 静态重演（无需原程序）：校验 + 统计 + stuck + 时间线 + 检查点
auto report = go2cpp::replay::ReplayFile(path, options);

// 完整重放（显式 factory）
auto report = go2cpp::replay::ReplayFileWith(path, factory, options);

// 完整重放（编译期注册入口）
void MyEntry(std::uint32_t gid, go2cpp::replay::ReplaySource& source);
RECORD_FIBER_ENTRY(MyEntry);
auto report = go2cpp::replay::ReplayFileAuto(path, options);

// 结构化输出
std::string json = go2cpp::replay::ReplayReportJson(report);
```

极简工具（CLI 可直接调用）：

```bash
replay <recording.g2cr>                     # stdout 单行 JSON；0=自洽 1=stuck 2=用法
replay play <recording.g2cr> [--until=SEQ]  # 自动完整重放；3=无注册入口
replay split <recording.g2cr> --every-events=N --out-dir=DIR
```

JSON schema `go2cpp.replay.report/1` 的主要字段：

| 字段 | 含义 |
|---|---|
| `status` | `ok / stuck / mismatch / format-error / io-error / partial` |
| `complete` / `checksum_ok` | 是否有 `kEnd`、文件级 CRC 是否通过 |
| `stuck` / `stuck_at{gid,channel,op_index,reason}` | 卡死定位 |
| `virtual_ns` | 重放结束时的虚拟时间 |
| `stopped_early` / `stopped_at_seq` | 是否按检查点截断、截断位置 |
| `auto_entry` / `entry_name` | 是否使用编译期入口、入口名 |
| `checkpoints[]` | `{seq,event_index,vtime_ns,ts_ns}` |
| `stats.file_events/sends/recvs/parks/wakes/blocked_ops/messages/selects/timers/ctx_events/bytes_messages` | 统计 |
| `stats.message_checksum` / `replayed_message_checksum` | 记录 vs 重放的消息序列指纹（FNV-1a 64） |
| `stats.select_checksum` / `replayed_select_checksum` | select 决策指纹 |
| `stats.global_recv_sequence` / `replayed_recv_sequence` | 完整消息序列 |
| `stats.select_choices` / `replayed_select_choices` | select 决策序列 |
| `per_gid[]` | 每 gid 的 started/exited/sends/recvs/parks/wakes/selects |
| `timeline[]` | 按 `seq` 排序的逐事件时间线（可选） |

时间线条目字段：`seq, ts_ns, vtime_ns, type, flags, gid, channel, op_index,
value, data_hex, decision, blocked, chosen`。

---

## 7. 限制（v2，如实声明）

1. **录制需要插桩**：`SyncChannel` 是新的 channel 实现，不是对任意
   `go2cpp::Channel<T>` 的自动插桩。要录制已有程序，需要把 channel 换成
   `SyncChannel`（或后续做编译开关 `GO2CPP_REPLAY=ON` 的桥接层）。
2. **select 的组锁串行化**：参与同一 select 的 channel 必须绑定同一个
   `SelectGroup`，组内操作串行执行。语义等价，但吞吐低于 Go 的 per-channel
   锁 + sudog 方案。多个无关 select 组之间互不影响。
3. **live sleep 阻塞 M**：`ReplayClock::SleepFor` 在录制端直接
   `std::this_thread::sleep_for`，在受管 fiber 内会占住承载线程。本工具定位
   是少量、低频的定时点，不是通用 timer 实现；可用 `ParkingCondition` 的
   超时等待替代（select timeout 已经是这样）。
4. **前缀分片存储放大**：每个分片都包含从开头到检查点的事件，K 个分片
   约 O(K×N) 字节。只重放到出事点首选 `play --until`（不产生新文件）。
5. **单注册入口**：`RECORD_FIBER_ENTRY` 在一个进程里只保留第一个入口（多
   场景用不同可执行文件，或显式 `ReplayFileWith`）。
6. **单 channel 模板实例**：`SyncChannel<T>` 的 channel id 由调用方分配，
   同一进程里多个 channel 需要各自 id；格式支持，demo 最多用 2 个。
7. **平台**：replay 引擎用 `ucontext`（Linux/glibc）；`tools/replay` 只在
   Linux 构建，Windows/WSL 交叉场景不支持。
8. **单 Scheduler 假设**：gid 在单个 Scheduler 内唯一；多 Scheduler 或多进程
   需要给 gid 加命名空间。
9. **时间戳不可跨机**：`ts_ns` 是 `steady_clock`，只能同机排序/做耗时；
   `vtime_ns` 也不做 NTP 校准，只保证同一次录制的相对顺序。
10. **gid 上限**：`Recorder` 的 `op_index/sleep_index` 表按 `gid_count` 分配；
    `gid >= gid_count` 时序号退化为 0，需按实际 fiber 数配置。
11. **不记录内存访问/锁/IO**：本服务只覆盖 channel + select + park/wake +
    context/时钟；数据竞争、`sync::Mutex`、网络 IO 不在范围内（那是指令级
    工具如 rr 的领域）。
12. **EventRing 导入未落地**：Debug 事件环（`debug_event_ring.hpp`）有
    `seq/ts/tid/code/gid/arg2`，但缺少 channel 配对与 xfer_id；本轮优先做
    语义事件，EventRing→G2CRPLY 的转换保留为后续工作（需要 Debug agent
    补 pid/事件码映射）。

---

## 8. 与「可迁移协程」的关系

Go2Cpp 的 G 会在 park 后被唤醒到**另一个 M 线程**上（work stealing /
跨线程 wake）。这一点直接决定了本实现的两个设计：

1. **逻辑 gid 必须随 G 走，不能随线程走。**
   第一版用 `thread_local` 保存 gid，录制结果里 gid 3（consumer）出现了 61
   次 send——因为它在另一个线程上被唤醒后，读到了那个线程上一个 producer
   的 gid。修复方式：fiber 入口用 `Task::id()` 注册 `task_id → gid`，之后每次
   操作按当前 `Task` 反查（带 thread_local 缓存）。**这是「可迁移协程」对
   replay 的硬约束**：任何以 OS 线程身份关联协程状态的方案，在迁移后都会
   张冠李戴。
2. **迁移不改变语义，因此可以在单线程上重演。**
   同一份记录在 ucontext 单线程引擎上完整重跑，消息序列与 2 个 P 上的真实
   运行逐项一致。反过来，这给了可迁移协程一个旁证：**迁移只改变「谁先跑」，
   不改变「跑到哪里」**；而「谁先跑」已经被 park/wake 的 seq 完整记录。
   如果某次迁移导致语义变化（丢唤醒、重复入队、错误的交接），重放会以
   `stuck` 或 `mismatch` 暴露出来。

对可迁移协程设计的启示：

- 协程身份（gid）必须独立于承载线程，且在所有同步原语的等待节点里携带；
- 唤醒必须能跨线程定向投递（`kWake` 记录的就是「投递给哪个 gid」）；
- 「迁移后重新排队」不能产生额外的语义事件（否则记录里会出现无法解释的
  park/wake 对）；
- 单线程重放是迁移正确性的低成本旁证：只要记录自洽且能重演，迁移本身
  就没有引入新的交错语义。

---

## 9. 后续工作

1. **EventRing → G2CRPLY**：等 Debug agent 补 pid/事件码后，直接把
   `debug_event_ring` 的 `seq/ts/tid/code/gid/arg2` 导入为 `kSched/kNote`
   事件，与语义事件合并分析。
2. **与 `Channel<T>` 的桥接**：编译开关 `GO2CPP_REPLAY=ON` 时让
   `go2cpp::Channel<T>` 内部调用 recorder（需要改核心库，超出本轮边界）。
3. **select 无组锁实现**：按 channel id 排序加锁 + sudog 风格 claim，去掉
   组内串行化；语义不变，只提升录制端吞吐。
4. **fiber 友好的 timer**：用 `ParkingCondition::wait_until` 替换 live
   `SleepFor` 的阻塞睡眠，避免占住 M。
5. **增量/快照分片**：当前前缀分片存储放大；可做「检查点快照 + 段文件」，
   分片只存差异。
6. **记录压缩**：大文件按 gid 建索引、zstd 分块压缩。
7. **与 rr 的对比**：指令级 replay 能覆盖数据竞争；语义级只覆盖调度/通信，
   两者互补，不是替代关系。
