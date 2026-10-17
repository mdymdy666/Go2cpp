# G4 · 并发原语 —— 一图看懂

> 覆盖：`Channel<T>`（含 `RingBuffer` / `SelectWaitState` / `SelectValue`）、`Future` / `Promise` /
> `FutureState<T>`、`sync::Mutex` / `ConditionVariable` / `WaitGroup`、以及调试扩展 `WaitSelector`。
> 源码：`include/go2cpp/concurrency/channel.hpp`、`future.hpp`、`sync.hpp`。

## ① 类架构：每个类用的底层能力 + 主要职责

```mermaid
classDiagram
    direction TB

    class ParkingCondition {
        <<G1 等待原语>>
    }
    class Mutex {
        <<互斥锁>>
        -atomic~uint8~ m_state  (kLocked|kWaiter)
        -deque~shared_ptr~WaitNode~~ m_waiters
        +Lock(ctx) bool
        +TryLock() bool
        +Unlock() void
        +DebugSnapshotInternal() MutexDebugSnapshot
    }
    class ConditionVariable {
        <<条件变量>>
        -deque~shared_ptr~WaitNode~~ m_waiters
        +Wait(mutex, ctx) bool
        +WaitFor(mutex, timeout, ctx) bool
        +NotifyOne() void
        +NotifyAll() void
    }
    class WaitGroup {
        <<计数等待>>
        -int64 m_count
        -deque~shared_ptr~WaitNode~~ m_waiters
        +Add(delta) void
        +Done() void
        +Wait(ctx) bool
        +Count() int64
    }
    class WaitSelector {
        <<D3 争用选择钩子>>
        +Select(candidates) optional~size_t~
        ← 只重排队列顺序，不改认领语义
    }

    class RingBuffer {
        <<无锁环形队列>>
        -vector~T~ m_slots
        -atomic m_head/m_tail/m_count
        +emplace_back(args) bool
        +pop_front(out) bool
        +pop_back_rollback() bool
        ← 取代 std::deque，消除节点分配
    }
    class SelectWaitState {
        <<select 的等待状态>>
        +TrySelect() bool
        +TrySelectPair() bool
        +Take() SelectResult
        +ArmSelectRecv() / ArmSelectSend()
        +DisarmSelect()
    }
    class Channel {
        <<Go 风格通道>>
        -shared_ptr~RingBuffer~ m_buffer
        -deque~Sender~ / deque~Receiver~
        -atomic~uint64~ m_generation
        +Send(value) bool
        +Recv(out) bool
        +TrySend / TryRecv
        +Close()
        +Len() / Cap()
    }
    class FutureState {
        <<共享状态 T / void 特化>>
        -variant~T, ErrorPtr, exception_ptr~ m_value
        +SetValue / SetError / SetException
        +Cancel(error) bool
        +Wait(timeout)
    }
    class Promise {
        <<写端>>
        +SetValue(v)
        +SetError(e)
        +GetFuture() Future
    }
    class Future {
        <<读端>>
        +Get() T
        +Wait(timeout) bool
        +Then(cb)
        +Ok() / Ready() / TimedOut() / Cancelled()
    }

    Mutex ..> ParkingCondition : 等待走它（受管 G 不占线程）
    ConditionVariable ..> ParkingCondition
    WaitGroup ..> ParkingCondition
    Mutex ..> WaitSelector : Unlock 时若注册则重排队列
    Channel *-- RingBuffer : 有缓冲时用它
    Channel ..> SelectWaitState : select 的挂起状态
    Channel ..> ParkingCondition : 阻塞收发
    Promise --> FutureState : 写
    Future --> FutureState : 读
    FutureState ..> ParkingCondition : Wait 走它
    ConditionVariable ..> Mutex : Wait 必须持有 mutex
```

| 类 | 主要职责 | 用的底层能力 |
|---|---|---|
| `Mutex` | 互斥；**所有权可无缝转移**（A 加锁 B 解锁）；`TryLock` 不能插队 | 原子状态字节、`WaitNode` 队列、`ParkingCondition` |
| `ConditionVariable` | 条件等待；`NotifyOne` 与 `Mutex::Unlock` **同构** | `Mutex`、`WaitNode` |
| `WaitGroup` | 计数归零唤醒；计数下溢/溢出抛 `logic_error` | `ParkingCondition` |
| `WaitSelector` | **D3**：锁争用时决定谁先获得权限 —— **只重排队列**，不改认领语义 | 无（纯策略） |
| `RingBuffer` | 无缓冲节点分配的环形队列（取代 `std::deque`） | 原子 head/tail、索引回绕 |
| `Channel<T>` | Go 风格 send/recv/close/select；9 种状态 | `RingBuffer`、`SelectWaitState`、`ParkingCondition` |
| `FutureState<T>` | 共享状态：值 / 错误 / 异常三选一；`void` 特化 | `variant`、`ParkingCondition` |
| `Promise`/`Future` | 写端 / 读端分离 | `FutureState` |

## ② 重要函数：功能与实现要点

```mermaid
flowchart TB
    subgraph LOCK["Mutex::Lock / Unlock"]
        L1["Lock：CAS m_state 加上 kLocked"] --> L2{"成功?"}
        L2 -- 是 --> L3["直接返回（快路径，无锁无等待）"]
        L2 -- 否 --> L4["受管 G 先做有界协作重试（256/4 次）<br/>避免每次短临界区都 park"]
        L4 --> L5["仍失败：入队 m_waiters + k 置 kWaiter<br/>然后 park"]
        L5 --> L6["Unlock：CAS 清 kLocked"]
        L6 --> L7{"有等者?"}
        L7 -- 否 --> L8["直接返回"]
        L7 -- 是 --> L9["持队列锁：**若有 WaitSelector 先重排队列**<br/>然后逐个 TryFinish 认领"]
        L9 --> L10["锁外 Wake 获胜者<br/>（所有权无缝转移，不经过「解锁再抢锁」）"]
    end

    subgraph CH["Channel 的 send/recv"]
        H1{"有缓冲?"}
        H1 -- 有 --> H2["RingBuffer.emplace_back<br/>（索引回绕，无节点分配）"]
        H1 -- 无 --> H3["直接与对端交接<br/>（无缓冲通道是同步握手）"]
        H2 --> H4{"缓冲满?"}
        H4 -- 是 --> H5["发送方入队等待 + park"]
        H3 --> H6["对方不在则本端入队 + park"]
        H5 --> H7["被唤醒后重试，成功则返回 true"]
        H6 --> H7
        H7 --> H8{"通道已关闭?"}
        H8 -- 是 --> H9["Recv 返回 false（零值 + 关闭标志）；<br/>Send 返回 false"]
    end

    subgraph SEL["select 的实现"]
        S1["为每个 case 造一个 SelectWaitState"] --> S2["逐个尝试 TrySelect<br/>（能立刻完成就不挂起）"]
        S2 --> S3{"有已完成?"}
        S3 -- 是 --> S4["取走结果，Disarm 其余 case"]
        S3 -- 否 --> S5["把所有 case 都 Arm 上<br/>（ArmSelectRecv/ArmSelectSend）"]
        S5 --> S6["park 等待任意一个被唤醒<br/>用 m_generation 防陈旧唤醒"]
        S6 --> S7["胜者由 CAS 决出（TryFinish 单赢家）<br/>其余被打断的 case 回滚"]
    end

    subgraph FUT["Future / Promise"]
        F1["Promise.SetValue"] --> F2["FutureState 用 variant 存值<br/>（值/错误/异常三选一，只成功一次）"]
        F2 --> F3["置 done 并唤醒所有 Wait"]
        F3 --> F4["Future.Get：未完成则 Wait；<br/>有异常则 rethrow；有错误则抛 FutureError"]
    end

    LOCK --> CH
    CH --> SEL
    SEL --> FUT
```

**要点**

- `Mutex::Lock` 的**快路径**只做一次 CAS —— 这是"无竞争几乎零成本"的原因；
- **受管 G 有有界协作重试**（源码注释里的 256/4 次）：短临界区不必每次 park，这是与"简单 park 实现"的关键差异；
- `Unlock` 的**所有权无缝转移**：不是"解锁→别人抢"，而是**直接认领下一个持有者**，因此 `TryLock` **不能插队**；D3 的 `WaitSelector` 只改**队列顺序**，所以这条语义不受影响；
- `RingBuffer` 用**索引回绕**（`emplace_back` 时 index+1 后判断越界再归零，不做取模），`pop_back` 只在回滚时用；
- `Channel` 用 `m_generation` 原子量防止"陈旧节点"的取消回调误伤新等待者；
- `select` 的正确性靠 **CAS 单赢家**（`WaitNode::TryFinish`）+ `generation` 守卫。

## ③ 向上层提供了什么

```mermaid
graph LR
    subgraph G4API["并发原语"]
        B1["Channel：Send/Recv/TrySend/TryRecv/Close/Len/Cap/select"]
        B2["Future/Promise：SetValue/SetError/Get/Wait/Then"]
        B3["Mutex / ConditionVariable / WaitGroup：Lock/Unlock/Wait/Notify/Add/Done"]
        B4["D3：SetWaitSelectorFactory（锁争用策略）"]
    end
    subgraph USE["谁在用"]
        V1["tools/go + 用户代码：<br/>goroutine 间通信"]
        V2["io / network / http：<br/>请求-响应配对用 Future"]
        V3["几乎所有多协程场景：<br/>Mutex 保护共享状态"]
        V4["调试模块：DebugMutex / DebugConditionVariable /<br/>DebugWaitGroup 只读快照 + WaitSelector"]
    end
    B1 --> V1
    B2 --> V2
    B3 --> V3
    B4 --> V3
    B1 --> V4
    B3 --> V4
```

## ④ 状态流转

### Channel 的 9 种状态（`ChannelStatus`）

```mermaid
stateDiagram-v2
    [*] --> kOpen : 构造（buffer 容量 0 = 无缓冲）
    kOpen --> kSendWaiting : 有发送者但无人接收
    kOpen --> kRecvWaiting : 有接收者但无人发送
    kSendWaiting --> kOpen : 接收者取走
    kRecvWaiting --> kOpen : 发送者送达
    kOpen --> kClosedNoData : Close() 且缓冲已空
    kSendWaiting --> kClosedNoData : Close()（发送者被拒绝）
    kRecvWaiting --> kOpen : Close() 前已交接
    kClosedNoData --> [*]

    note right of kClosedNoData
        关闭后：Recv 返回 false（零值）；
        Send 返回 false；再次 Close 安全幂等
    end note
```

### Mutex 的等待者交接（`WaitNode` 的状态）

```mermaid
stateDiagram-v2
    [*] --> kArmed : 入队并 Arm
    kArmed --> kNotified : Unlock 选中并 TryFinish(kNotified) 获胜
    kArmed --> kCancelled : 取消/超时（TryFinish(kCancelled) 获胜）
    kArmed --> kRequeued : 发现可立刻获取（不 park）
    kNotified --> [*] : 成为新持有者
    kCancelled --> [*] : 返回 false
    kRequeued --> [*] : 直接继续执行
```

## 与 Go 的差异

| 项 | Go | Go2Cpp |
|---|---|---|
| channel 缓冲 | `hchan` 环形缓冲 + `sudog` 队列 | `RingBuffer`（索引回绕）+ 自定义 `WaitNode` 队列 |
| `select` | 运行时随机选可用 case | 逐个 TrySelect，**先到先得**；无随机化 |
| 锁 | `sync.Mutex`（可自旋 + 饥饿模式） | 无自旋；受管 G 用**有界协作重试**（256/4） |
| 锁争用策略 | 运行时内建 | **可插拔**（D3 `WaitSelector`），默认 FIFO |
| `Future` | `sync.WaitGroup` / channel 组合 | 独立的 `Promise`/`Future`（含 `void` 特化、`Then`） |
| `WaitGroup` | 计数 + `sudog` | 计数 + `WaitNode`，下溢/溢出抛 `logic_error` |
