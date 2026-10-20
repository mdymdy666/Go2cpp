# Debug 可靠性说明：哪些结论可以信，哪些只能当参考

> 适用工作树：`Go2Cpp-work`
> 核对日期：2026-10-20
> 核对构建：`build-debug-full`（`GO2CPP_DEBUG_DUMP=ON`）、`build-doctor-full`
> （`GO2CPP_DEBUG_DUMP=ON` + `GO2CPP_DIAGNOSTIC=ON`）、`build-off`（两者 OFF）
> 一句话结论：**全部并发结构（10 个）+ park 的观测属于可靠范围；scheduler /
> IO / timer / 迁移 / 回放等能力保留、仍可用于优化与监督并发，但不保证完整，
> 只能当趋势与线索，不能当事实依据。**

---

## 1. 结论（置顶）

### 1.1 可靠范围（可以做事实判断）

| # | 结构 / 行为 | 位置 | 可靠的含义 |
|---|---|---|---|
| 1 | `sync::Mutex` | `include/go2cpp/sync.hpp:205` | 争用事件全量 + owner/waiters dump + 快照 |
| 2 | `sync::ConditionVariable` | `include/go2cpp/sync.hpp:263` | 等待走 park（全量）+ waiters dump |
| 3 | `sync::WaitGroup` | `include/go2cpp/sync.hpp:315` | 等待走 park（全量）+ C++ 快照（count/waiters） |
| 4 | `sync::RWMutex` | `include/go2cpp/concurrency/rwmutex.hpp:107` | 内部 Mutex/CV，等待走 park；无专属 dump |
| 5 | `sync::RecursiveMutex` | `include/go2cpp/concurrency/recursive_mutex.hpp:88` | 底层 Mutex，等待走 park；无专属 dump |
| 6 | `sync::TimedMutex` | `include/go2cpp/concurrency/timed_mutex.hpp:57` | 复用 `Mutex::LockFor`，等待/超时走 park；无专属 dump |
| 7 | `sync::SpinMutex` | `include/go2cpp/concurrency/spin_mutex.hpp:119` | 内部 Mutex，等待走 park；有 `Held()`/`OwnerNoParkDepth()` |
| 8 | `core::FutexMutex` | `include/go2cpp/concurrency/core/futex_mutex.hpp:85` | **不 park、无事件**；只用于运行时内部唤醒门。其边界已知，故列入清单，但不能用事件环观测它 |
| 9 | `Channel<T>` | `include/go2cpp/concurrency/channel.hpp:850` | send/recv/select/wake 事件全量（立即完成采样）+ 资源 dump |
| 10 | `core::ParkingCondition` | `include/go2cpp/concurrency/core/parking_condition.hpp` | wait/notify 事件 + 等待节点（含 gid）dump |
| 11 | **park**（特殊并发行为） | `src/concurrency/scheduler.cpp:2778` | 进入/返回/拒绝全量事件 + 原因码 + Fiber 链 |

> 「可靠」不等于「每个结构都有专属 dump」。上表里 1/9/10/11 有专属 dump 与
> 事件；2/3/4/5/6/7 的等待最终都落到同一个 park 协议，**park 事件是全量的**，
> 因此可以确定「谁在等、等了多久、何时被唤醒」；但它们没有各自的
> owner/waiters dump 组件，不能假装看得到。8 是唯一完全不产生 park 事件的
> 项，它的「可靠」只意味着**边界已知**（见 §2.2）。

### 1.2 保留但不保证可靠（只能当参考）

| 能力 | 保留状态 | 不保证的原因（摘要） |
|---|---|---|
| scheduler 事件 | 保留 | 采样（1/8、1/16、1/32）+ 每线程环形窗口（256+64）覆盖 |
| io / network dump | 保留 | 验收场景未注册（`--sections io` 直接 exit 3）；只有 fd 级计数 |
| timer 事件与 dump | 保留 | 来源默认 `kUnknown`；执行器路径无 `timer_id`；事件在普通区被覆盖 |
| 迁移（fiber migration） | 保留 | 只支持无缓冲单生产者单消费者接收等待；锁/IO/select 未覆盖 |
| 回放（replay） | 保留 | 只记录 channel/select/park/wake/context/timer；锁与 IO 不记录 |
| HTTP2/HTTP3 流状态 | 保留（压测工具） | 没有 DebugServer 组件、没有流状态事件 |
| CLI 的 section 选择、大响应传输 | 保留 | section 白名单不含 `timer`；单响应无分页，超上限即失败 |

这些能力**没有删除、也不是没有用**：做性能优化、观察并发趋势、定位
「大概是哪一类问题」时仍然有效。它们的问题是**不能支撑精确断言**——
采样会漏，窗口会截断，未注册就是看不到，未验证就是不知道。

### 1.3 一句话判断规则

> 能在「可靠范围」里用 dump/事件直接看到的，写结论；
> 只能从 scheduler/io/timer/迁移/回放看到的，写**假设**，并注明需要交叉验证。

---

## 2. 可靠范围

### 2.1 为什么这些可靠（机制层，不是口头保证）

1. **park 主线事件不采样。** `kParkEnter` / `kParkReturn` / `kParkRejected`
   走 `GO2CPP_DEBUG_EVENT`（`src/concurrency/scheduler.cpp:2778`、`:2835`），
   不是 `GO2CPP_DEBUG_EVENT_SAMPLED`；每条 park 都带 `arg2=SuspendReason`、
   `arg3=原语原因码`（`include/go2cpp/debug/debug_event_ring.hpp:123-145`）。
2. **所有混合等待都收敛到 park。** `Mutex::LockImpl`、`ConditionVariable::Wait`、
   `WaitGroup::Wait` 的受管 G 路径最终调用
   `backend()->Park(...)`（`src/concurrency/sync.cpp:723-790`）；Channel 与
   ParkingCondition 同样 park。因此即使某个结构没有专属事件，它的**阻塞点**
   仍在事件环里，且不依赖采样。
3. **累计计数不受窗口影响。** `TotalCount(code)` 是进程启动以来的原子累计
   （`include/go2cpp/debug/debug_event_ring.hpp:363-365`），事件环只保留最近
   若干条；覆盖率验收脚本刻意用 `totals` 而不是窗口计数
   （`tests/scenarios/full_trace/verify_coverage.py`）。
4. **dump 只读、只报已注册对象。** 未注册组件返回 `unknown-component` /
   `exit 3`（`src/debug/debug_server.cpp:1058-1065`；实测 `io`、`fiber` 均
   exit 3），不存在「编造一份看起来正常的 dump」。
5. **快路径采样只用于补充信号。** `kMutexFastLock`（1/32）、`kPCWait`（1/16）、
   `kChannelSendOk`/`kChannelRecvOk`（1/32）缺失不会破坏「阻塞→唤醒」主线，
   因为主线事件全量；它们只影响「无竞争次数」这类统计的精度。
6. **dump 有明确的一致性标记。** 每个组件段带 `seq_at_start` / `seq_at_end` /
   `changed_during_dump`（`src/debug/debug_dump.cpp:108-136`），采集期间是否
   有事件发生是可见的，不需要猜。

### 2.2 逐结构能力表（有什么、没有什么）

| 结构 | 阻塞/唤醒路径 | 事件 | dump / 快照 | 统计 / 历史 |
|---|---|---|---|---|
| `sync::Mutex` | FIFO 交接；受管 G park | `kMutexLock` 全量（仅争用）、`kMutexUnlock` 全量、`kMutexFastLock` 1/32（`sync.cpp:1260`、`:1680`、`:1221`） | `DumpMutex`：`held`/`waiters`/`native_waiters`/`fiber_waiters`/`state_bits`/`locked`/`waiter_flag`/`starving`/`woken`/`owner_gid`/`owner_known`（`debug_dump.cpp:491-526`） | `MutexDebugSnapshot`；`TotalCount` |
| `sync::ConditionVariable` | `WaitNode` → park | 无 CV 专属事件；park 事件带外层原因码 | `DumpConditionVariable`：`waiters`（`debug_dump.cpp:528-543`） | `ConditionVariableDebugSnapshot` |
| `sync::WaitGroup` | `WaitNode` → park | 无专属事件；park 事件 | **无 DebugServer 组件**；仅 C++ `DebugSnapshotInternal()`：`count`/`waiters`（`sync.hpp:82`、`:342`） | 无 |
| `sync::RWMutex` | 内部 `Mutex` + `ConditionVariable` → park | 只有内部 Mutex 争用时的 `kMutexLock`；**没有读/写角色事件** | **无专属 dump**；内部对象不对外暴露 | 无 |
| `sync::RecursiveMutex` | 底层 `Mutex` → park | 只有底层争用事件 | **无 dump**；`owner`/`depth` 只是 C++ 原子字段（`recursive_mutex.hpp:114-146`） | 无 |
| `sync::TimedMutex` | `Mutex::LockFor` → park / Context 超时 | 只有底层争用事件 | **无 dump** | 无（超时只返回 false） |
| `sync::SpinMutex` | 内部 `Mutex` → park；持锁 park 属违约 | 底层争用事件；违约走 abort/降级计数 | 仅 C++ 接口 `Held()`、`OwnerNoParkDepth()`（`spin_mutex.hpp:229-241`） | `spin_mutex_park_degrade_total` |
| `core::FutexMutex` | 原生 futex 睡眠，**不 park** | **无** | **无** | **无** |
| `Channel<T>` | send/recv/select → park | `kChannelSend`/`kChannelRecv`/`kChannelWake` 全量；`SendOk`/`RecvOk` 1/32；`SelectEnter`/`Select` 全量（`channel.hpp:2066`、`:2293`、`:1532`、`:930`、`:973`） | `DumpChannel`：`capacity`/`length`/`closed`/`send_waiters`/`recv_waiters`/`select_senders`/`select_receivers`/`change_waiters`/`generation`（`debug_dump.cpp:409-438`） | `TotalCount`；`generation` |
| `core::ParkingCondition` | 受管等待 park | `kPCWait` 1/16；`kPCNotify` / `kPCNotifyAll` 全量（`parking_condition.cpp:395`、`:536`、`:579`） | `DumpParkingCondition`：`waiters`/`native_waiters`/`fiber_waiters`/`waiting_nodes`/`notified_nodes`/`broadcast_seq`/每个节点的 `gid`（`debug_dump.cpp:440-490`） | `TotalCount`；`broadcast_seq` |
| **park** | `Scheduler::Park` | `kParkEnter`/`kParkReturn`/`kParkRejected` 全量 + 原因码（`scheduler.cpp:2778-2835`） | `DumpFiber`：`state`/`suspend_reason`/`parent_id`/`active_parent_id`/`nesting_depth`/栈范围（`debug_dump.cpp:352-376`）；`timeline` 视图 | `TotalCount`；`seq_at_start/end` |

**读表要点**

- `RWMutex` / `RecursiveMutex` / `TimedMutex` / `SpinMutex` 的可靠结论只能写到
  「等待/唤醒」层：它们**没有** owner/waiters dump，也没有读写角色事件。
  若要回答「谁持有读锁」，当前代码答不了，文档不能假装能答。
- `WaitGroup` 的 `DebugSnapshotInternal()` 是 C++ 接口，DebugServer 没有对应
  组件；CLI 用户看不到它。
- `FutexMutex` 是内部唤醒门（`sync.cpp:308`、`parking_condition.cpp:243`），
  不是给业务用的锁。它不 park，因此**不会出现在等待图/事件环里**；把它列入
  可靠清单是因为「我们知道它不可见」，而不是「它可观测」。
- **资源级历史（已验收，纳入可靠范围）。** `include/go2cpp/debug/debug_resources.hpp`
  提供资源注册表（10 类并发结构）、线程栈样本（`backtrace` 去重 + 延迟符号化）、
  Fiber 链样本（父链 + 任务函数名），并通过 `kParkCaptured = 136` 与资源事件写入
  事件环；视图 `--view history [--resource]` / `--view resources` 输出按 timestamp
  排序、按 address 归并的历史。2026-10-20 验收：`tests/debug/run_resource_history_demo.sh`
  **ALL PASS**（mutex events=73、holds=24/waits=23/wakes=23、with_stack=49、
  with_chain=51；channel sends=31/recvs=33/blocked=10/11；栈符号可解析），
  ctest 116/116。它补齐了 RWMutex/RecursiveMutex/TimedMutex/SpinMutex/WaitGroup
  的「谁持有」观测缺口。

### 2.3 park 与 Fiber 链

**可靠的部分（有实测）**

- 一次 park 的完整语义：`kParkEnter(arg2=SuspendReason, arg3=原语原因码)` →
  `kParkReturn`（正常/取消）；被拒有 `kParkRejected` 与拒绝码。
- 原因码覆盖 channel/select/mutex/parking_condition/io/timer/future/scheduler
  等（`debug_event_ring.hpp:123-145`），外层原因优先，不会被子层覆盖。
- `timeline` 视图能把一个 gid 的生命周期拼出来；本次 10s 场景的 demo G：
  `events=17`、`park=1`、`wake=1`、`resume=1`、`lifecycle=created/started/completed`
  全部命中（2026-10-20 本次复测输出）。
- 窗口内 park↔wake 配对解释率 **97.1%**（checked=68、paired=64、stuck=2），
  验收脚本的硬性阈值是 ≥50%（`verify_coverage.py`）。

**前提（必须写清楚）**

1. **Fiber 链要注册对象。** `DumpFiber` 需要 `RegisterFiber(name, Fiber&)`；
   验收场景没有注册，实测 `--sections fiber` 返回 exit 3。没有注册就只能用
   `timeline` 视图（依赖事件窗口）。
2. **没有按 gid 查 Fiber 的入口。** CLI/协议都没有「gid → Fiber dump」的
   命令（P2-2，`tests/enhanced/defects.md`）；`DumpScheduler.tasks` 也不带
   `parent_id`/栈字段（P2-1）。因此「Fiber 链」当前可靠的用法是：
   `timeline --gid N` 看事件层父子/生命周期，或进程自己注册 Fiber 对象后
   dump 该对象；不能对任意 gid 直接拉栈。
3. **窗口外的历史不可见。** timeline 依赖事件环；窗口截断时只能看到最近
   若干条（见 §3.1）。demo G 在 QUIESCENT 窗口后运行，所以完整；长跑进程
   的早期 G 不行。

### 2.4 dump 性能数据（可靠，可复现）

| 指标 | 数值 | 条件 / 来源 |
|---|---|---|
| 单事件成本 | **19.65–22.47 ns/事件**（批量 200k、5 轮）；含时钟的观测口径约 **25 ns** | `codex.md` §7.3；`EventRing::Record` 为每线程一次 `seq` + `steady_clock` + 定长拷贝 |
| 端到端 dump（`--view gmp`） | **p50 6.76 ms / p95 8.29 ms**，输出 8.5 KB | 10s 混合场景、4 worker、20 轮、含 CLI 进程启动与 socket 传输（2026-10-20 复测） |
| 长跑 dump（`--sections scheduler`） | **6–10 ms / 3.7 KB / 5 个存活 task / 0 个终态**（另有 4 个 P、4 个 M） | 48s 持续 spawn 场景、第 45s 抓取；两次测量（2026-10-20 复测） |
| 修复前对照 | 48s 达 **23 万条 kDead / 61 MB**，接近 CLI 传输上限 | `src/concurrency/scheduler.cpp:3529-3533` 注释；`tests/enhanced/defects.md` D2 |
| 用户报告口径 | 单事件 ~25 ns、dump p50 5 ms / p95 7 ms、事件环 256+64/线程 | 2026-10-20 实测报告（与本次复测同量级） |
| 修复后长跑（用户报告） | **2 个存活 task / 2.2 KB** | 另一负载；与本次 5 tasks/3.7 KB 同属「不随历史任务数增长」 |
| 事件环容量 | 普通区 **256** + 关键区 **64**，每线程 | `include/go2cpp/debug/debug_event_ring.hpp:97-102` |
| 关键区保护 | wake / enqueue 拒绝 / claim 失败 / park 拒绝 / G 终态 / 诊断事件不被普通事件覆盖 | `SchedEventCodeIsCritical()`，同文件 `:265` |

**为什么 dump 性能可以写「可靠」**

- dump 只读已注册对象、只做一次快照拷贝，不反向控制运行时（`codex.md` §4.4）。
- 终态 G 已从 scheduler 快照过滤（`scheduler.cpp:3524-3556`），dump 体积不再
  随历史完成任务数线性增长——这是 48s 复测只有 3.7 KB 的直接原因。
- 一致性标记（`changed_during_dump`）让「采集期间有并发活动」可见，不会把
  动过的快照当静止快照。

### 2.5 可靠范围的边界（不是缺陷，是前提）

- **不是全局原子快照。** 每个组件 `consistent=per_call`，跨组件字段可能不同刻；
  同一组件内 `seq_at_start != seq_at_end` 时，字段是「采集过程中」的值。
- **只覆盖已注册对象。** DebugServer 不会自动发现进程里的锁/Channel；必须
  调用 `RegisterMutex` / `RegisterChannel` / `RegisterParkingCondition` 等。
  未注册 = 看不到，且会明确报错。
- **计数与明细的来源不同。** `TotalCount` 是进程累计（可信总量）；事件环是
  最近窗口（可信明细，但只限窗口内）。两者不能互相替代。
- **`owner_known` 可能为 false。** Mutex 的 owner 记录依赖持锁 park 检测
  （Debug 默认开、Release 默认关），此时只能看到 `owner_known=false`，不能
  推断「无人持有」。
- **Fiber 链依赖窗口或显式注册**（见 §2.3）。

---

## 3. 保留但不保证可靠（逐项说明「为什么」）

以下能力**不删除、不否定其价值**。它们在优化、监督并发、定位方向时仍然
有用；问题是不能从它们推出精确结论。

### 3.1 scheduler 事件：采样 + 窗口截断，只能看趋势

**采样点（代码依据）**

| 事件 | 采样率 | 位置 |
|---|---|---|
| `kMutexFastLock` | 1/32 | `src/concurrency/sync.cpp:1221` |
| `kPCWait` | 1/16 | `src/concurrency/parking_condition.cpp:395` |
| `kChannelSendOk` / `kChannelRecvOk` | 1/32 | `channel.hpp:930`、`:973` |
| `kStealFail` | 1/16 | `scheduler.cpp:4061` |
| `kWorkerPark` / `kWorkerUnpark` | 1/8 | `scheduler.cpp:4365`、`:4469` |
| `kSysmonPass` | 1/32 | `scheduler.cpp:1520` |

**窗口截断（实测）**

10s 混合场景（4 worker）一次抓取：

```text
事件窗口: count=1910 first_seq=4 last_seq=134026 gaps=248 truncated=True
          total_published=134026
关键区:   critical_event_count=320（= 5 个线程 ring × 64，关键区均已写满；
          是否发生过覆盖要看 seq/gaps，不能只看这个数）
```

也就是说：**窗口里只有 1910 条，进程实际发布了 134026 条**；`gaps=248`
说明序号不连续。任何「窗口内计数 = 总量」的推断都是错的。窗口内的
park↔wake 配对仍然可信（97.1% 解释率），但那只对窗口内 68 条成立。

**其他固有限制**

- **最多 128 个线程 ring**（`debug_event_ring.hpp:329`，`kMaxThreads=128`）。
  150 个线程时 `dropped_threads=22`（`docs/bug-report-2026-10-20.md:223`）；
  超出后新建线程的事件不会被记录。
- **线程退出会摘除 ring**：实测两个线程各记 100 条 `kThreadStop` 后 join，
  dump 里 `after_join=0`；保持线程存活则 `while_alive=200`
  （`docs/bug-report-2026-10-20.md:188`）。因此线程生命周期事件事后不可见。
- **关键区仍是环**：超过 64 条关键事件后旧的会被覆盖；只能保证「不全丢」，
  不能保证「不丢」。

**结论**：scheduler 事件适合回答「有没有大量 park/steal/worker 睡眠」这类
趋势问题；不适合回答「park 恰好发生了几次」「某个 wake 是否丢了」。

### 3.2 io / network 的 dump：未在验收场景注册，覆盖不全

- `RegisterIOManager` 存在（`debug_server.cpp:1203`），但全链路验收场景
  （`tests/scenarios/full_trace/full_trace_scenario.cpp`）和死锁场景都没有
  注册它；实测 `go2cpp-cli dump --sections io` → `exit 3`，
  报「目标进程未注册组件 'io'」。
- `DumpIOManager` 只输出**聚合计数**：`fd_slots`、`slots_with_waiters`、
  `epoll_registered`、`reader_waiters`、`writer_waiters`、`pending_*`
  （`debug_dump.cpp:545-600`），没有 per-fd、per-connection、HTTP2 流状态。
- IO 事件只有 `kIoWait` / `kIoWake` / `kIoWakeBatch`，同样受窗口截断。
- **未验证**：没有验收场景证明「任意 IO 错误都能在 dump 里定位」。

**结论**：io/network dump 可用于看「有没有 fd 堆积」的趋势；不能用于断言
「某个连接卡在哪」。

### 3.3 timer：来源分类 `kUnknown`、执行器路径无 `timer_id`

- **来源默认 `kUnknown`。** `TimerSource` 默认值是 `kUnknown`
  （`src/concurrency/timer.cpp:130`、`:201`）；只有 `ScheduleOn`/`EveryOn`
  自动标成 `kScheduler`（`timer.cpp:1192`、`:1222`）。context、
  parking_condition、channel、timed_mutex 这些使用方**没有**设置
  `TimerSourceScope`（`codex.md` §23 明确记录「本次边界不允许改这些文件」），
  所以它们的定时器在 dump 里统一记成 `kUnknown`。
- **执行器路径丢 `timer_id`。** 普通 `kTimerCallback` 记录
  `(gid=source_gid, arg2=timer_id)`（`timer.cpp:580`）；但 `ScheduleRepeat`
  的周期路径写死 `kTimerCallback, 0, 0`（`timer.cpp:1215`），没有 `timer_id`，
  无法把这次回调与哪只 timer 对上。
- **事件在普通区，会被覆盖。** 10k 级 arm 事件会挤掉同区的普通事件；
  `DUMP timer` 只展示最近 8 条 timer 事件（`debug_dump.cpp` 中
  `recent_timer_events` 的 `> 8U` 截断）。
- **实测**：10s 场景进程累计 `fire=4067`、`callback=1335`；窗口内
  `fire_events=108`、`fire_with_callback=38`（约 35%）。这个差值**不能**
  直接解释为「丢了 65% 回调」——其中包含「匹配不到 timer_id」与窗口截断。
- CLI 的 `--sections` 白名单不含 `timer`（实测 `exit 2`：未知 section），
  只能用 `--view timer` 或组件名直连。

**结论**：timer 事件能回答「timer 机制有没有在跑」；不能回答「第 N 号
timer 是否按时回调」「callback 与 fire 是否一一对应」。

### 3.4 迁移（fiber migration）：只支持一条窄路径

- 文档明确：只支持**无缓冲、单生产者单消费者、无 select 的接收等待**
  （`docs/fiber-migration.md:775`、`:855`）。
- 发送等待、select、锁、IO、跨机器/跨架构、调度器自动接入均**未验证**。
- 迁移等待路径不进入 Scheduler park 协议，唤醒由宿主显式 `resume()`
  （`codex.md` §22.3）——这意味着迁移中的 G 不会产生常规 park 事件。

**结论**：迁移的 dump/restore 可以在 demo 路径作为工具使用；不能拿它诊断
真实业务里的锁/IO 挂起。

### 3.5 回放（replay）：只覆盖通信与时钟，不覆盖锁/IO

- 事件类型只有 `kSend`/`kRecv`/`kSelect`/`kClose`/`kTimer`/`kCtx`/`kPark`/
  `kWake`/`kSched`/`kNote`（`tools/replay/include/replay/format.hpp:78-100`）。
- 文档限制第 11 条：**不记录内存访问/锁/IO**，只覆盖 channel + select +
  park/wake + context/时钟（`docs/replay-service.md:531`）。
- 录制需要把 channel 换成 `SyncChannel`，不是对任意 `Channel<T>` 自动插桩；
  EventRing → G2CRPLY 的导入尚未落地（同文档 `:534`）。
- 回放引擎基于 `ucontext`，只在 Linux 构建；时间戳不可跨机。

**结论**：回放能复现「channel/select 调度序列」；不能复现锁竞争或 IO 时序，
因此不能用它证明「锁相关 bug 已修复」。

### 3.6 HTTP2 / HTTP3 流状态：没有在线诊断视图

- `src/debug/` 下没有 http2/http3 组件，`Dump*` 函数列表里也没有流状态
  （`include/go2cpp/debug/debug_dump.hpp:192-221`）。
- 现有材料是压测报告（`DebugTooling/http2-stress-report.md`），不是运行时
  dump；事件码里也没有流/帧状态。
- 死锁场景的验收只覆盖 fiber/mutex/scheduler/events，不覆盖 HTTP2/3。

**结论**：HTTP2/3 的流状态只能靠日志与压测；不要声称 dump 能看到流卡死。

### 3.7 工具与传输层：不影响「能力有无」，但会影响可用性

- `go2cpp-cli dump --sections` 的白名单是
  `scheduler/fiber/channel/parking_condition/mutex/condition_variable/io/events/runtime`
  ——不含 `timer`（实测 exit 2）。
- DebugServer 单响应一次性返回，不分页；CLI 侧上限 **64 MiB**
  （`tools/cli/transport.cpp:51-55`）。历史上 16 MiB 上限曾让 46.5 MiB 的
  `status`/`dump` 直接失败（`docs/bug-report-2026-10-20.md:106-110`）；
  调到 64 MiB 后该案例可用，但超大 `events` dump 仍可能触顶。
- Doctor 的 `dump` 复用同一注册表，schema 不会分叉；但它同样只覆盖已注册
  组件。

---

## 4. 使用建议

### 4.1 agent 的判断规则（按顺序执行）

1. **先确认窗口状态**：`dump` 的 `events` 段看 `truncated`、`gaps`、
   `total_published`、`critical_event_count`。窗口截断时，明细只用来看
   「窗口内发生了什么」，总量只用 `totals`。
2. **只对已注册组件下结论**：`dump --sections X` 返回 exit 3 就说明没注册，
   此时「看不到」是事实，不要用其它组件的字段替代。
3. **park/channel/parking_condition/mutex 的结论可以直接写**：事件有全量
   主线，dump 有明确字段。
4. **RWMutex/RecursiveMutex/TimedMutex/SpinMutex 只写等待层结论**：
   「谁在 park、何时醒」可以写；「谁持有读锁/写锁、owner 是谁」不能写。
5. **WaitGroup/FutexMutex 不下 dump 结论**：前者没有 DebugServer 组件，
   后者不 park、不产生事件。
6. **scheduler/io/timer/迁移/回放只写假设**：必须带「采样/窗口/未注册/
   未验证」限定词，并给出交叉验证手段。

### 4.2 不可靠范围的正确用法

- **趋势可以，精确计数不行**：scheduler 采样计数只用于比较「这一段比上一段
  更忙/更闲」，不要拿它算丢失率。
- **线索可以，定论不行**：timer 的 fire/callback 差值先怀疑「窗口 + 来源
  分类 + 无 timer_id」，不要直接判定「丢回调」。
- **交叉验证**：用 `--view gmp`（活跃 G 快照）+ `--view park`（窗口内配对）
  + `--view primitives`（原语计数）三份互相印证；单份视图不下结论。
- **复跑**：采样与窗口相关的异常，至少复跑一次（同一场景、同一时长），
  两次不一致的结论按「未验证」处理。

### 4.3 排障顺序（从可靠到不可靠）

1. `dump --sections scheduler,mutex,channel,parking_condition,events`
   （看谁在等、谁持有、通道长度、事件窗口）。
2. `dump --view timeline --gid <gid>`（看单个 G 的 park/wake/resume 生命周期）。
3. `dump --view park` + `dump --view primitives`（窗口内配对与原语计数）。
4. 只有以上都不够时，才用 `--view sched` / `--view timer` / `--view gmp`
   的趋势信息，并标注「仅供参考」。

---

## 5. 验证方法（每项给命令与期望）

### 5.1 验证「可靠范围」确实被覆盖

```bash
# 1) 全链路视图验收：park↔wake / enqueue↔claim / timer 链路
bash tests/scenarios/full_trace/run_full_trace.sh 10 8
# 期望：PASS；park 解释率 ≥50%；6 个视图 schema 正确

# 2) 死锁闭环：真实 HTTP + 锁顺序反转 + dump + force-wake/解锁
bash tests/scenarios/deadlock_routes/run_deadlock_scenario.sh build-doctor-full
# 期望：dump 能看到 gid/state/waiting_on/owner/waiters；Doctor 判定 deadlock

# 3) 单元测试（Debug 事件/ dump / timer 调试 / Doctor）
GO2CPP_TEST_FILTER=debug_dump   ./build-debug-full/go2cpp_tests
GO2CPP_TEST_FILTER=debug_events ./build-debug-full/go2cpp_tests
GO2CPP_TEST_FILTER=timer_debug  ./build-debug-full/go2cpp_tests
GO2CPP_TEST_FILTER=timer_replay ./build-work/go2cpp_tests
GO2CPP_TEST_FILTER=diagnostic   ./build-doctor-full/go2cpp_tests
# 期望：均输出 "[test] all checks passed"
```

### 5.2 验证「不可靠范围」的边界

```bash
# io / fiber 未注册：应为 exit 3，而不是返回空 dump
go2cpp-cli dump --pid <pid> --sections io      # exit 3
go2cpp-cli dump --pid <pid> --sections fiber   # exit 3

# timer 不能用 --sections：应为 exit 2（未知 section）
go2cpp-cli dump --pid <pid> --sections timer   # exit 2
# 改用视图：sources 里应出现 kUnknown 条目（来源未标注；占比取决于使用方）
go2cpp-cli dump --pid <pid> --view timer --out /tmp/timer.json

# 窗口截断：events 段应出现 truncated=true / gaps>0
go2cpp-cli dump --pid <pid> --sections events --out /tmp/events.json

# 迁移/回放的覆盖边界（看测试矩阵，而不是猜）
bash tools/migration/tests/run_tests.sh build-migration
# 期望：53/53；报告中明确只支持接收等待
```

### 5.3 验证「OFF 构建零开销、无诊断符号」

```bash
# OFF 构建：不应有 DiagnosticAgent 符号（实测 0）
nm -C build-off/libgo2cpp_debug.so | grep -c DiagnosticAgent

# Doctor 构建：应有符号（实测 168）
nm -C build-doctor-full/libgo2cpp_debug.so | grep -c DiagnosticAgent

# 事件环实现只在 DEBUG_DUMP=ON 的库里（实测 38 个 EventRing 符号）
nm -C build-debug-full/libgo2cpp_debug_event.so | grep -c EventRing
```

### 5.4 验证 dump 性能与规模

```bash
# 长跑 48s 后抓 scheduler dump：应是「毫秒级 + 小体积 + 只含存活 G」
# 复刻：启动 full_trace 场景 48s，READY 后 sleep 45，再执行：
go2cpp-cli dump --pid <pid> --sections scheduler --out /tmp/sched48.json
# 本次复测：6–10 ms / 3.7 KB / 5 tasks / 0 dead（另有 4 P + 4 M）

# 端到端 dump 分位：20 轮取 p50/p95
# （本次复测 --view gmp：p50 6.76 ms / p95 8.29 ms）
```

### 5.5 验证事件成本

```bash
# 批量采集基准（仓库内无独立 ns 基准；数字来自 codex.md §7.3 的批量测量）
# 单线程 22.47 → 19.65 ns/事件（200k 事件、5 轮、flush_batch=16 饱和）
# 复核：自建 200k 次 EventRing::Record 循环，或用 debug_events 测试确认接线
GO2CPP_TEST_FILTER=debug_events ./build-debug-full/go2cpp_tests
```

---

## 6. 附：本次复测的原始数据与口径

### 6.1 10s 混合场景（`run_full_trace.sh 10 8`）

```text
事件窗口: count=1910 first_seq=4 last_seq=134026 gaps=248 truncated=True
          total_published=134026
G 生命周期: created=1340 started=1340 resumed=6737 completed=1335
park/wake: park=6744 wake=6726 delta=18 park_return=6737 park_rejected=5
窗口配对: checked=68 paired=64 stuck=2 解释率=97.1%
原语: mutex_lock=810 mutex_unlock=812 mutex_fast=765
      channel_send_block=1623 channel_recv_block=602 channel_select=1995
      pc_wait=369 pc_notify=7213
timer: fire=4067 callback=1335 g_enqueue=1335
       窗口 fire_events=108 fire_with_callback=38
timeline: gid=1582 events=17 park=1 wake=1 resume=1 lifecycle=True/True/True
PASS
```

（用户报告的另一轮：park 6489 / wake 6470、timer fire 3957 / callback 1308，
与上表同量级，说明数据可复现。）

### 6.2 dump 性能（20 轮，端到端含 CLI 进程与 socket）

```text
--view gmp:      p50=6760us p95=8286us size=8503B
--sections scheduler,events（12s 场景）: p50=22041us p95=25475us size=980351B
```

> 第二个数字大，是因为 `events` 段在 12s 场景里仍有 1890 条事件（JSON 展开
> 后约 1 MB）；48s 长跑且只抓 scheduler 时是 6 ms / 3.7 KB。可见 dump 成本
> 主要由「抓哪些组件、事件窗口多大」决定，而不是运行时长。

### 6.3 48s 长跑

```text
第 45s: dump_ms=6~10 size≈3768 tasks=5 dead=0（另有 4 P + 4 M）
TICK 44 produced=54554 consumed=63817 selects=8594 mutex_ops=109234 pc_waits=4296
```

对照修复前（`scheduler.cpp:3529-3533` 注释与 `defects.md` D2）：48s 时
23 万条终态 G、61 MB——现已过滤终态，dump 规模与历史任务数解耦。

---

## 7. 变更记录

- 2026-10-20：新建。依据 `docs/diagnostic-channel.md`、
  `docs/bug-report-2026-10-20.md`、`docs/replay-service.md`、
  `docs/fiber-migration.md`、`tests/enhanced/defects.md` 与当日实测复跑；
  明确「10 个并发结构 + park 可靠，scheduler/io/timer/迁移/回放保留但
  不保证」，并给出逐项验证命令。未 commit。
