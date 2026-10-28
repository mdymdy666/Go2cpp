# 模块文档：Channel / Select

> 适用基线：`HEAD 1b46de8`（2026-10-25），WSL 工作树 `/UserData/CodexWorkSpace/Go2cpp`。
> 源码：`include/go2cpp/concurrency/channel.hpp`（3283 行）、`src/concurrency/channel.cpp`（544 行）。
> 兼容转发头：`include/go2cpp/channel.hpp`（只 include 真身）。
> 引用约定：文中 `channel.hpp:NNN` 指 `include/go2cpp/concurrency/channel.hpp`，
> `channel.cpp:NNN` 指 `src/concurrency/channel.cpp`。
> 状态：**现行**。本文合并 `docs/concurrency-park-points.md`（channel 部分）、
> `docs/channel-hang.md`、`docs/concurrency-baseline.md`（channel 数据）与
> `docs/concurrency-5rounds.md`（场景），旧文档保留为历史快照。

## 1. 概述

`go2cpp::Channel<T>` 是 Go `chan T` 的对应物：容量 0 的无缓冲会合点、容量 > 0 的
有界 FIFO 缓冲、`Send/TrySend/SendFor`、`Recv/TryRecv/RecvFor`、`Close/Range` 式
排空，以及把 N 个 channel 操作折叠成一次等待的 `Select` 系列。

它建在同步原语之上、业务代码之下（`include/go2cpp/concurrency/channel.hpp:5-19`）：

- 业务层直接使用 `Channel<T>`（生产者-消费者、ping-pong、流控实验）；
- `SelectValue` / `SelectCase` / `SelectWaitState` 把 `select { case <-ch: }` 编译成
  「一次轮询 + 一个等待节点」；
- 底层 park/wake 走 `core::ParkingCondition`：受管 G 在此 park 并让出 M，普通线程
  退化为原生 futex 等待。因此**channel 阻塞不会占住 worker**。

设计上与 Go `hchan` 对齐：预分配环形缓冲 + `sendx/recvx/dataqsiz`，**刻意不用
`std::deque`**（`channel.hpp:31-39`）；等待队列是 `std::deque<std::shared_ptr<Node>>`，
发送/接收各一条，`m_mutex` 同时保护 buffer、队列与 `closed`（`channel.hpp:2601-2626`）。

### 1.1 最小示例

```cpp
#include "go2cpp/channel.hpp"

using namespace std::chrono_literals;

auto ch = go2cpp::MakeChannel<int>(0);          // cap==0：无缓冲 rendezvous
auto send = ch->SendFor(42, 100ms);             // 阻塞直到有接收者/超时
auto recv = ch->RecvFor(100ms);                 // recv.value == 42
if (send.Ok() && recv.Ok()) { /* ... */ }

ch->Close();                                    // 幂等；重复 Close => kAlreadyClosed
auto after = ch->Recv();                        // status == kClosed，value 为空
```

## 2. API 一览

### 2.1 状态与结果类型

| 名称 | 位置 | 语义 |
|---|---|---|
| `ChannelStatus` | `channel.hpp:101-111` | `kReady/kWouldBlock/kClosed/kAlreadyClosed/kCancelled/kDeadlineExceeded/kTimedOut/kNil/kInvalid` |
| `ChannelStatusIsReady` | `channel.hpp:114-116` | `kReady` 或 `kClosed` 都算「有可交付结果」 |
| `ChannelSendResult` | `channel.hpp:452-468` | `status + error`；`Ok()` 仅 `kReady`；`Closed()` 含 `kClosed/kAlreadyClosed`；`Cancelled()` 含三种取消/超时 |
| `ChannelRecvResult<T>` | `channel.hpp:726-749` | `optional<T> value + ok + ready + status + error`；`Closed()` 要求 `ready && kClosed && !ok`；`ValueOrDefault()` 返回零值 |
| `SelectProbe` | `channel.hpp:471-511` | 单 case 探测结果（`ready/ok/status/error` + 类型擦除值） |
| `SelectResult` | `channel.hpp:2822-2899` | `index/selected/ok/status/error/value/typed_value`；`kNoSelection = size_t(-1)`；`Value<T>()/TakeValue<T>()/Cast<T>()` |
| `SelectValue` | `channel.hpp:138-312` | move-only 值的类型擦除载体（`Get/Take/As/ToAny`） |

### 2.2 Channel 成员

| API | 位置 | 说明 |
|---|---|---|
| `Channel<T>::Create(cap=0)` / `MakeChannel<T>(cap=0)` | `channel.hpp:907-909`、`2632-2635` | 返回 `shared_ptr<Channel<T>>`；cap 0 表示无缓冲 |
| `Capacity()` | `channel.hpp:929` | 固定容量，不含当前长度 |
| `Len()` | `channel.hpp:932-935` | 缓冲内元素数（不含正在交接的等待者） |
| `IsClosed()` | `channel.hpp:938-941` | 持锁读 `m_closed` |
| `DebugSnapshot()` | `channel.hpp:948-970` | 一次持锁采集 capacity/length/closed/send_waiters/recv_waiters/select_senders/select_receivers/change_waiters/generation |
| `Send(T, ctx={})` | `channel.hpp:973-999` | 无限阻塞；成功 `kReady`，关闭 `kClosed`，取消 `kCancelled/kDeadlineExceeded`，值操作失败 `kInvalid` |
| `SendFor(T, timeout, ctx={})` | `channel.hpp:1002-1013` | 相对超时；超时 `kTimedOut`；负超时按 0 处理 |
| `TrySend(T)` | `channel.hpp:1016-1039` | 非阻塞；无法立即完成返回 `kWouldBlock` |
| `Recv(ctx={})` | `channel.hpp:1042-1064` | 无限阻塞；关闭排空后 `kClosed` |
| `RecvFor(timeout, ctx={})` | `channel.hpp:1067-1077` | 超时 `kTimedOut`（不改变通道状态） |
| `TryRecv()` | `channel.hpp:1080-1104` | 非阻塞；空且未关闭返回 `kWouldBlock` |
| `Close()` | `channel.hpp:1108-1172` | `noexcept`、幂等；重复关闭 `kAlreadyClosed`；摘下全部等待者并锁外唤醒 |
| `Generation()` | `channel.hpp:1233-1235` | 单调递增；send/recv/close 改变可用性时 +1 |
| `WaitForChange(gen, timeout)` | `channel.hpp:1242-1250` | 等待代际变化；无观察者时通知方零成本 |
| `ForceWakeWaiters()` | `channel.hpp:1194-1229` | **Doctor 专用** spurious wake：不改结果、不入队、不递增 generation |
| `ArmSelectRecv/ArmSelectSend/DisarmSelect` | `channel.hpp:1255-1563` | select 内部注册/撤销（公共但属实现细节） |

### 2.3 方向视图与检查型自由函数

| API | 位置 | 说明 |
|---|---|---|
| `SendOnlyChannel<T>` / `AsSendOnly` | `channel.hpp:2653-2700`、`2753-2756` | 只暴露 Send/TrySend/SendFor/Close；不能接收 |
| `RecvOnlyChannel<T>` / `AsRecvOnly` | `channel.hpp:2708-2751`、`2758-2761` | 只暴露 Recv/TryRecv/RecvFor；不能发送/关闭 |
| `SendChecked(ch, value, ctx)` | `channel.hpp:2765-2772` | nil 通道返回 `kNil`（而不是像 Go 一样永久阻塞） |
| `RecvChecked(ch, ctx)` | `channel.hpp:2774-2781` | 同上 |

### 2.4 Select 构造器

| API | 位置 | 说明 |
|---|---|---|
| `RecvCase(ChannelPtr<T>)` / `RecvCase(RecvOnlyChannel<T>)` | `channel.hpp:2901-2952`、`3018-3021` | 接收分支；不可复制 `T` 标记 `unsupported` |
| `SendCase(ChannelPtr<T>, T)` / `SendCase(SendOnlyChannel<T>, T)` | `channel.hpp:2953-3017`、`3023-3026` | 发送分支；值用 `shared_ptr` 保存，失败可保留 |
| `DefaultCase()` | `channel.hpp:3028-3042` | 默认分支，永远就绪 |
| `DoneCase(ctx)` | `channel.hpp:3157-3205` | `case <-ctx.Done()`；arm 时注册回调，disarm 时注销 |
| `AfterCase(deadline)` / `AfterCase(duration)` | `channel.hpp:3228-3269` | `case <-time.After(d)`；Timer 句柄登记在 `SelectCaseRegistry`，避免 arm 返回即析构取消 |
| `Select(cases, ctx={}, timeout=nullopt)` | `channel.hpp:3273-3281`；实现 `channel.cpp:156-542` | 多路复用；返回 `SelectResult` |

`Select` 的实现是**类型无关**的，所以放在 `.cpp`；`RecvCase/SendCase` 是模板，必须留在头里
（`channel.cpp:12-29`）。

## 3. 设计

### 3.1 环形缓冲：与 hchan 同构

`detail::RingBuffer<T>`（`channel.hpp:771-858`）用未初始化存储 + 就地构造，`cap==0`
不分配任何内存；`emplace_back/pop_front/pop_back` 只移动 `m_head/m_tail/m_count`，
回绕在缓冲内部完成，锁内不做取模（`channel.hpp:31-39`、`769-770`）。

```cpp
detail::RingBuffer<int> buffer(3);   // 只分配原始存储，不构造元素
buffer.emplace_back(1);
buffer.pop_front();                  // 析构队首
```

`Channel<T>` 的静态约束：`T` 必须 **nothrow move constructible + nothrow destructible**
（`channel.hpp:895-898`）。失败路径因此可以用「先构造、后提交」的强保证；例如
`RecvUntil` 直接 `std::move(front)` 不会留下半损坏元素（`channel.hpp:2309-2321`）。

### 3.2 cap0：发送者与接收者的直接交接

无缓冲通道没有缓冲槽位，`SendUntil` 在 `m_receivers` 为空、`m_buffer.size() < m_capacity`
为假时不会入缓冲，而是创建 `PendingSend` 挂到 `m_senders`（`channel.hpp:2166-2188`），
然后 park。`RecvUntil` 看到 `m_senders` 非空时直接取队首：

```cpp
T value = std::move(sender->value);
m_senders.pop_front();
sender->accepted = true;
sender->Signal();
sender->NotifyIfParked();
```

（`channel.hpp:2351-2362`）——**绕过缓冲**完成 rendezvous。反过来，`SendUntil` 也会
优先从 `m_receivers` 队首取接收者并直接 `emplace` 值（`channel.hpp:2081-2142`），
同样绕过缓冲。

因此 cap0 的语义是「发送与接收在 `m_mutex` 内线性化，值只 move 一次，双方各醒一次」；
没有中间队列，也没有「先存后取」的窗口。

### 3.3 有缓冲：入队与 `RefillBufferLocked`

有缓冲时：

- `SendUntil` 在无接收者且 `m_buffer.size() < m_capacity` 时 `emplace_back` 并返回
  `kReady`（`channel.hpp:2144-2155`）；
- `RecvUntil` 先从缓冲取队首，再调用 `RefillBufferLocked()` 把一个被阻塞的发送者
  补进刚空出的槽位（`channel.hpp:2314-2315`、`2533-2593`）。

`RefillBufferLocked` 只补一个槽位（`break`），因为一次 `Recv` 只释放一个槽位；它同时
处理 select 发送节点（`TrySelect` 失败则回滚 `pop_back`），并用 `notify` 在锁外唤醒
被接纳的发送者。这个「批量填缓冲」路径是 cap1024 达到 107 ns/msg 的关键
（`docs/module-audit-2026-10-20.md` §3）。

### 3.4 阻塞与唤醒：PendingSend / PendingRecv

两个等待节点结构对称（`channel.hpp:1588-1720`）：

| 方向 | 结果字段 | select 字段 | 唤醒载体 |
|---|---|---|---|
| `PendingSend` | `accepted/closed/cancelled/status/error` | `select_state/select_index` | `core::ParkingCondition cv` |
| `PendingRecv` | `ready/ok/cancelled/status/error` | 同上 | 同上 |

不变量（`channel.hpp:1582-1586`、`1662-1665`）：

- `PendingSend` 的 `accepted/closed/cancelled` **至多一个为真**，由 `Unlock`/`Close`/
  取消回调在 `m_mutex` 内竞争写入；
- `accepted == true` 后 `value` 已被移走，不得再读；
- `PendingRecv` 的 `ready && ok` 必有值，`!ok` 时值为空。

等待循环每次醒来都重检 `accepted/closed/cancelled`、`context->IsDone()`、
`ParkingCondition::CancellationRequested()` 与 deadline（`channel.hpp:2216-2296`、
`2452-2530`）。这就是「谓词式等待」，虚假唤醒不会导致错误返回。

### 3.5 自旋预算与 `signaled/parked` 协议

只有 **纯原生线程**（无 Scheduler Task、无 Fiber）且 `capacity <= 1` 时才在 park 前
自旋（`channel.hpp:2174`、`2414`；`channel.cpp:94-101`）：

- 受管 Fiber 自旋会占住 carrier M；
- cap>1 的吞吐依赖「等待者睡眠期间生产者批量填缓冲」，自旋会把批量交接退化成逐条
  rendezvous（实测 cap1024 从 ~40 ns/msg 退化到 ~100 ns/msg，`channel.hpp:2169-2172`）。

自旋预算初始 512 次 PAUSE，按实际 park 时长反馈：park ≤ 30 µs 翻倍、否则减半，
上下限 128/2048（`channel.hpp:562-575`）。每次 512 次 PAUSE 让出一次 CPU，避免
单核/超订场景把对端饿死（`channel.hpp:577-602`）。

`signaled` 与 `parked` 是等待节点上的两个原子位（`channel.hpp:1602-1614`）：

- 完成方在 `m_mutex` 内写结果后调用 `Signal()`（置 `signaled`）与 `NotifyIfParked()`；
- `NotifyIfParked()` 只对「真的已 park」的节点 `cv.notify_one()`，仍在自旋的节点会
  自己看到 `signaled`，从而省掉一次 `ParkingCondition` 内部锁。

### 3.6 Select：探测 → 注册 → 抢占 → 等待 → 拆除

`Select` 的主循环在 `src/concurrency/channel.cpp:156-542`，注释把步骤写在
`:130-155`。关键点：

1. **探测阶段**（`channel.cpp:241-264`）：每次重试从线程本地 xorshift32 取随机起点
   （`NextSelectOffset`，`channel.cpp:54-88`），逐个调用 `probe()`；这样多个同时就绪的
   case 不会被固定优先级饿死，也没有全局游标 cacheline 争用。旧实现的全局
   `atomic cursor` 已删除（`channel.cpp:54-61`）。
2. **default**（`channel.cpp:280-288`）：本轮无 case 就绪时走 default，不注册、不 park。
3. **注册阶段**（`channel.cpp:301-330`）：创建共享 `SelectWaitState`，把每个 case
   `arm` 到自己的等待队列（`Channel::ArmSelectRecv/ArmSelectSend`）。任一 arm 失败/抛异常
   必须 `Cancel()` + `disarm()` 完整回滚，否则会留下指向已返回 Select 的悬空节点。
4. **抢占结果**（`channel.cpp:332-362`）：arm 之后立刻 `Take()`；在 arm 期间可能有
   channel 已就绪并选中了本次 Select。
5. **挂起等待**（`channel.cpp:394-466`）：回调里只 `Notify`，状态判断留给唤醒之后；
   含 `probe` 的 case（非 channel，如 `AfterCase`）每 2 ms 轮询一次
   （`channel.cpp:450-455`），纯 channel case 则无限 `Wait`。
6. **拆除**（`channel.cpp:468-502`）：无论哪条路径返回都 `remove_callback` + `Cancel` +
   `disarm`；`take_selected` 再把迟到的交接结果拿回来，避免「取消与交接同时发生」时
   丢值。

**单赢家**由 `detail::SelectWaitState` 保证（`channel.hpp:606-720`）：

```cpp
bool TrySelect(std::size_t index, SelectProbe probe);   // 只有一个 case 能成功
static bool TrySelectPair(first, first_index, first_probe,
                          second, second_index, second_probe);  // 两个 select 配对
bool Cancel();                                          // 已选中则拒绝取消
bool Take(std::size_t* index, SelectProbe* probe);      // 只允许取一次
```

`TrySelectPair` 用 `std::scoped_lock` 同时锁两个 wait_state，保证「select 发送者 ↔
select 接收者」的配对要么双方都提交、要么都不提交（`channel.hpp:623-648`）。这就是
两个 select 互相匹配时不会出现「一方拿到值、另一方还在等」的原因。

**取消竞态**（审计 D1）：`context` 已取消但本轮没有 channel case 就绪时，`DoneCase`
的 `cancel_probe` 是唯一就绪分支；`take_cancelled_case` 在返回 `kNoSelection` 之前
补一次无副作用探测（`channel.cpp:207-236`、`469-501`），否则会出现
`selected=false` 的 `kCancelled`（历史复现率 0.03%/次 → 8%~19%，见
`docs/module-audit-2026-10-20.md` §2 P1-8）。

### 3.7 关闭：先摘队列，再锁外唤醒

`Close()`（`channel.hpp:1108-1172`）：

```cpp
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_closed) return {kAlreadyClosed, ChannelAlreadyClosedError()};
  m_closed = true;
  ++m_generation;
  senders.swap(m_senders);
  receivers.swap(m_receivers);
  // 给每个节点写终态（select 节点走 TrySelect/Cancel）
}
for (auto& sender : senders) { sender->Signal(); sender->NotifyIfParked(); }
for (auto& receiver : receivers) { receiver->Signal(); receiver->NotifyIfParked(); }
NotifyChange();
```

因此 close 后**不会再接受发送**，已缓冲的值仍可被接收读完，接收者随后得到 `kClosed`
（`channel.hpp:2381-2384`）。`Close` 是 `noexcept`：即使 `SelectProbe` 构造抛异常，
也只会 `select_state->Cancel()` 后继续唤醒（`channel.hpp:1126-1133`）。

### 3.8 generation 与外部 select

`Channel::Generation()` 在每次 send/recv/close 改变可用性时递增；`WaitForChange`
观察代际并 park 在 `m_change_cv` 上（`channel.hpp:1237-1250`）。观察者计数
`m_change_waiters` 让热路径在没有观察者时跳过 `notify_all`（`channel.hpp:2008-2015`）。
这为外部的 `SelectLoop` / 自定义多路复用器提供了「不注册节点也能等变化」的能力。

### 3.9 调试与 Doctor

- `DebugSnapshot()`（`channel.hpp:948-970`）一次持锁读取 9 个字段，给 CLI/Doctor 的
  等待图与挂起诊断用；
- `ForceWakeWaiters()`（`channel.hpp:1194-1229`）只对普通等待节点发 spurious wake，
  不写结果、不递增 generation；用于确认「等待者确实只是等待，而不是唤醒令牌丢失」；
- 资源事件（`kChannelSend/kChannelRecv/kChannelWake`）只在 `GO2CPP_DEBUG_DUMP=ON`
  时记录；`channel.cpp:159-160`、`channel.hpp:980-998`。

## 4. 边界与陷阱

### 4.1 关闭后的语义（以实现为准）

| 操作 | 结果 | 位置 |
|---|---|---|
| `Send`（关闭后新调用） | `kClosed` + `ChannelClosedError()` | `channel.hpp:2076-2079` |
| `Send`（关闭前已排队） | 醒来 `kClosed` | `channel.hpp:1137-1139`、`2221-2224` |
| `Close()`（重复） | `kAlreadyClosed` | `channel.hpp:1112-1114` |
| `Recv`（有缓冲） | 先读完缓冲，再 `kClosed` | `channel.hpp:2380-2384` |
| `TrySend`（关闭后） | `kClosed`（不是 `kWouldBlock`） | `channel.hpp:2076-2079` |

> 注意：头文件 `channel.hpp:50-51` 的注释写「对已关闭的 channel `Send` 返回
> `kAlreadyClosed`」，但实现（`SendUntil` 的 `m_closed` 检查）返回的是 `kClosed`；
> `kAlreadyClosed` 只用于重复 `Close()`。以代码为准，本文按实现描述。

### 4.2 阻塞与超时

- `Send/Recv` 无超时：只能靠对端、`Close` 或 `Context` 取消退出；
- `SendFor/RecvFor` 的相对超时在**进入等待前**先检查一次（`DeadlineReached`），
  已经过期就立刻返回 `kTimedOut`；`TrySend/TryRecv` 用 `deadline=now` + `nonblocking`
  返回 `kWouldBlock`（`channel.hpp:2157-2161`、`2391-2396`）；
- `Context` 的 `DeadlineExceeded` 映射为 `kDeadlineExceeded`，普通取消映射为
  `kCancelled`（`channel.hpp:1978-2002`）；
- 超时**不改变通道状态**：不会把值从缓冲/队列里拿走。

### 4.3 类型约束

- `Channel<T>`：`T` nothrow move + nothrow destruct（`channel.hpp:895-898`）；
- 内建 `SelectCase`（`RecvCase/SendCase`）：还要求 `T` 可复制；不可复制类型
  `unsupported=true`，`Select` 直接返回 `kInvalid`（`channel.hpp:2797-2799`、
  `2950`、`3015`；`channel.cpp:166-174`）。move-only 值应使用 `SelectValue` 自定义路径；
- `SelectResult::Value<T>()` 对 move-only 类型返回 `nullopt`，必须用
  `TakeValue<T>()`（`channel.hpp:2848-2877`）。

### 4.4 手动 Fiber / nil 通道

- 手动 Fiber（未归属 Scheduler）不能 park：`SendUntil/RecvUntil` 返回 `kInvalid`
  （`channel.hpp:2163-2165`、`2407-2410`），`Select` 也返回 `kInvalid`
  （`channel.cpp:295-299`）；
- 空 `ChannelPtr` 通过 `SendChecked/RecvChecked` 返回 `kNil`，**不会**像 Go 的 nil
  channel 一样永久阻塞（`channel.hpp:2763-2781`）；`RecvCase(nil)` 的 probe 也返回
  `kNil`（`channel.hpp:2904-2911`）。

### 4.5 Select 的性能边界

- 纯 channel case：注册后无限等待，无轮询开销；
- 含 `probe`-only case（如 `AfterCase`/`DoneCase` 的 probe 部分）：每 2 ms 醒一次
  重新探测（`channel.cpp:450-455`），大量短超时会放大 syscall/定时器开销
  （`docs/concurrency-5rounds.md` §5.6 观察）；
- `Select` 的 case 表按值传入，`SelectCase` 内部是 `std::function`，可被多线程共享；
  但 `arm/disarm` 用 `SelectCaseRegistry` 以 `wait_state` 为键登记回调/Timer，避免
  重试时互相覆盖（`channel.hpp:3044-3111`）。

### 4.6 生命周期

`Channel` 拥有等待节点与缓冲值，不拥有生产者/消费者任务；对象必须长于仍在使用它的
等待者（`channel.hpp:751-760`）。析构会 `Close()`（`channel.hpp:917-922`），但如果在
析构时仍有线程在 `Send/Recv`，那是未定义行为——必须先用 `Close` + 等待/协调收尾。

### 4.7 迁移子集（`GO2CPP_MIGRATION_API`）

仅当宏开启时存在 `ExportWaitState/AdoptWaitState`（`channel.hpp:1722-1944`）：
只支持**无缓冲、单生产者单消费者、无 select、无 deadline/context** 的接收等待；
其余情况明确拒绝，不做近似导出（`channel.hpp:1745-1775`）。迁移等待路径直接
`Fiber::Suspend`，不经过 `ParkingCondition`（`channel.hpp:1902-1942`）。

## 5. 测试

### 5.1 ctest 用例

| 用例 | 源 | 覆盖 | 超时 |
|---|---|---|---|
| `go2cpp_tests`（`GO2CPP_TEST_FILTER=channel`） | `tests/concurrency/test_channel.cpp` | 有/无缓冲、关闭、取消、nil、select、DoneCase/AfterCase（T1–T11、B1–B4、R1） | 全局 |
| `go2cpp_channel_concurrency` | `tests/concurrency/test_channel_concurrency.cpp` | Fiber/线程混合、并发收发 | 300 s |
| `go2cpp_fiber_select_test` | `tests/concurrency/test_fiber_select.cpp` | Fiber 上的 select 语义 | 120 s |
| `go2cpp_concurrency_combinations` | `tests/concurrency/test_concurrency_combinations.cpp` | 原语组合 | 120 s |
| `go2cpp_concurrency_fuzz` | `tests/concurrency/test_concurrency_fuzz.cpp` | 随机操作序列 | 180 s |
| `go2cpp_park_wake_pair` | `tests/concurrency/test_park_wake_pair.cpp` | park/wake 配对、10 轮 × 4 Fiber | 300 s |
| `go2cpp_deadlock_detection` | `tests/concurrency/test_deadlock_detection.cpp` | 死锁检测 | 120 s |

注册位置：`cmake/Go2CppTests.cmake`（`go2cpp_tests` 源列表 `:29`，
`go2cpp_channel_concurrency` `:775-784`，`go2cpp_fiber_select_test` `:677-684`）。
另有非 ctest 的 11 亿条压测装置 `tests/concurrency/test_channel_thread_stress.cpp`
（见 §5.3）。

### 5.2 性能数据（同机相对值）

| 指标 | 优化前 | 优化后 | 来源 |
|---|---:|---:|---|
| Channel cap0 rendezvous | 30.3 µs | **1.19 µs**（25.5×） | `docs/performance.md:14` |
| Channel ping-pong | 59 µs | **2.65 µs** | `docs/performance.md:15` |
| cap1024 批量 | — | **107 ns/msg** | `docs/module-audit-2026-10-20.md` §3 |
| MPMC 100 万消息 | — | 0 丢/重/幽灵 | 同上 |
| close 唤醒 32 Recv/32 Send/32 Select | — | 0~2 ms | 同上 |

调试监控开销（`docs/concurrency-baseline.md` §2，OFF 构建，cap=64）：
`mpmc` 7.76 M ops/s、p50 30 ns、p99 39.7 µs；`select2` 1.01 M ops/s、
`select4` 0.70 M ops/s。debug 全开（FULL）时 `mpmc` 降到 0.67 M ops/s（11.5× 慢），
`select2` 降到 64k ops/s（15.8× 慢）——主要成本是栈/Fiber 链捕获，不是事件编码
（`docs/concurrency-baseline.md` §3.1）。

### 5.3 线程版挂起调查（已结案）

`docs/channel-hang.md` 记录了「线程版 Channel 高频混合场景挂起一次」的调查：

- 装置：`tests/concurrency/test_channel_thread_stress.cpp`，7 场景（cap64/1/0、
  8+8、rtt0、mixed），每轮 10 万消息，watchdog 30 s + `DebugSnapshot` + gdb；
- 结果：4 进程 × 100 轮 × 5 场景 2.0 亿条、8 进程 × 150 轮 × 7 场景 8.58 亿条、
  DEBUG_DUMP=ON 0.56 亿条，合计 **≈11.14 亿条**（≈22 亿次原语）0 挂起；
- 后续用还原装置重跑：第一次「100% 复现」的根因是**测试装置 Gate 计数 bug**
  （`gate.WaitAll(kWorkers+2)` 把只在 stop 后 Arrive 的 helper 计入），与 Channel 无关；
  修正后 24,600 轮 / 8,320 万条 + 旧库 11,500 轮 / 1.446 亿条 = 累计 **≈13.4 亿条 0 挂起**；
- 逐点审计（谓词重检、cap≤1 spin/parked/signaled、per-node futex、锁序、Close 唤醒）
  未发现丢唤醒/锁序环/无界自旋；TSAN 1 轮 × 2000 消息无数据竞争。

判定：**Channel 线程路径无产品缺陷证据**；最初挂起最可能是旧二进制/旧进程混淆，
其次是已修复的 `WaitNode` 竞态（`docs/channel-hang.md` §「后续重跑结论」「最终判定」）。

### 5.4 5 轮随机压测

`docs/concurrency-5rounds.md`：5/5 轮完成，OFF 轮 7,838,342 + ON 轮 7,802,299 =
15,640,641 次原语操作；0 死锁、0 崩溃；132 项计数守恒断言全过；6/6 场景覆盖
（pc/rwcache/workpool/pipeline/mixedlocks/storm）；峰值 RSS 6.0~6.2 MB 稳定。
其中 `storm` 场景覆盖 `Send/TrySend/Recv/TryRecv/RecvFor/Select/Wait/Notify`
全部路径（`docs/concurrency-5rounds.md` §4）。

### 5.5 复现命令

```bash
# 统一测试二进制（channel 过滤）
ctest --test-dir build-work -R '^go2cpp_tests$' --output-on-failure
GO2CPP_TEST_FILTER=channel ./build-work/go2cpp_tests

# 专项
ctest --test-dir build-work -R 'go2cpp_channel_concurrency|go2cpp_fiber_select_test' \
      --output-on-failure

# 线程版 11 亿条装置（手工）
g++ -std=c++17 -O2 -I include tests/concurrency/test_channel_thread_stress.cpp \
  -L build-work -lgo2cpp_channel -lgo2cpp_sync -lgo2cpp_parking_condition \
  -Wl,-rpath,$PWD/build-work -o /tmp/channel_stress
/tmp/channel_stress --rounds=100 --scenario=mixed
/tmp/channel_stress --inject-hang      # 自检抓现场链路
```

> 说明：任务指定的构建目录 `/UserData/build/go2cpp-mcp-integrated` 的
> `GO2CPP_BUILD_TESTS=OFF`（只有 14 个 os/bufio 用例），并发 ctest 需在启用测试的
> 构建（如 `build-work`）中运行；本文的用例名来自 `cmake/Go2CppTests.cmake` 的当前树。

## 6. 旧文档合并与相关文档

| 旧文档 | 合并内容 | 现状 |
|---|---|---|
| `docs/concurrency-park-points.md` | channel 的 park 点、临界区禁用规则 | 顶部标注「已合并至 docs/modules/sync.md」（主体是锁/ park 清单） |
| `docs/channel-hang.md` | 线程版挂起调查与 13.4 亿条结论 | 顶部标注「已合并至 docs/modules/channel.md」 |
| `docs/concurrency-baseline.md` | channel 调试开销基线（mpmc/select2/select4） | 顶部标注「已合并至 docs/modules/channel.md 与 sync.md」 |
| `docs/concurrency-5rounds.md` | 5 轮随机场景与 storm/pipeline 覆盖 | 顶部标注「已合并至 docs/modules/channel.md 与 sync.md」 |

相关文档：

- `docs/state-machines/Channel.md` —— Channel 的状态机视图；
- `docs/correctness_audit.md` §「Channel 和 Select」—— 线性化点与单赢家论证；
- `docs/diagnostic-channel.md` —— Doctor 对 channel 等待者的在线诊断；
- `docs/modules/sync.md` —— 同目录的同步原语文档（Mutex/CV/WaitGroup/锁族）。

## 7. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-10-26 | 首版模块文档：合并 4 份旧文档，核对 `HEAD 1b46de8` 的 API/行号/数字；记录「关闭后 Send 返回 kClosed」与头文件注释不一致 |
