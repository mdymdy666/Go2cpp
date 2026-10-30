# Channel 状态机

- 源文件：`include/go2cpp/concurrency/channel.hpp`（`Channel<T>` 在 756 行起，成员在 1751 行起）
- 关键函数：`Send/SendUntil`、`Recv/RecvUntil`、`TrySend/TryRecv`、`Close`、`DispatchSenders`（把缓冲空间分给等待发送者）、`Select`
- 底层：`core::ParkingCondition m_change_cv`（受管 G park、普通线程条件变量）

## 1. 主状态（由「缓冲长度 + 两个等待队列」共同决定）

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Empty | `Send(v)`（无等待接收者、cap==0） | WaitingSender | 登记 `PendingSend` 到 `m_senders`，锁外 park |
| Empty | `Send(v)`（cap>0） | Buffered(1) | 直接入 `m_buffer` |
| Empty | `Recv()` | WaitingReceiver | 登记 `PendingRecv` 到 `m_receivers`，锁外 park |
| WaitingSender | `Recv()` | Empty | 直接把等待者的值交给接收者，**双方一起唤醒**（不经缓冲） |
| WaitingReceiver | `Send(v)` | Empty | 直接把值塞给等待者，双方唤醒 |
| Buffered(n) | `Send(v)`（n < cap） | Buffered(n+1) | 入缓冲；若有等待接收者则改为直接交接 |
| Buffered(cap) | `Send(v)` | WaitingSender | 缓冲满，发送者排到 `m_senders` |
| Buffered(n) | `Recv()` | Buffered(n-1) | 出队；随后 `DispatchSenders` 从 `m_senders` 补入（保持 FIFO） |
| 任意 | `Close()` | Closed | 唤醒全部 `m_senders`/`m_receivers`（等待者得到 `kClosed`），`m_generation++` |
| Closed | `Send(v)` | Closed | 返回 `kClosed`，不抛异常 |
| Closed | `Recv()`（缓冲非空） | Closed | 仍返回缓冲里的值（Close 后读尽剩余值的语义依赖这点） |
| Closed | `Recv()`（缓冲空） | Closed | 返回 `kClosed` |

## 2. 不变量

- `m_buffer.size() <= m_capacity`；`cap==0` 时 `m_buffer` 不分配（`m_allocated == false`）。
- `m_senders` 非空 ⇒ `m_buffer` 满 **或** `cap==0`；`m_receivers` 非空 ⇒ `m_buffer` 空。
- 同一个 channel 的 `m_senders` / `m_receivers` 都是 FIFO（Go 语义），`DispatchSenders` 按序补入缓冲。
- `m_closed == true` 之后 `m_senders`/`m_receivers` 必然为空（Close 已排空）。
- 所有对 `m_buffer`/两个队列/`m_closed` 的访问都在 `m_mutex` 内；park 一定在锁外。

## 3. Select 的接入方式

`Select` 不改变上述状态，它只做三件事：

1. **轮询**：对每个 case 试 `TrySend/TryRecv`（无锁外等待），命中就返回；
2. **登记**：把未命中的 case 各自挂一个 `SelectProbe`，并在 channel 上记录「有 select 在等」；
3. **park**：在 `SelectWaitState` 上等待，任一 case 就绪或 context 取消即返回。

**代际**：`m_generation` 每次 `Close` 加一；select 的等待节点用它判断自己是否跨代失效，
避免关闭后仍把值交给一个已经放弃的 select。

## 4. 边界行为

| 场景 | 实际行为 |
|---|---|
| 无缓冲 `Send` 时无接收者 | park（不占 worker，`ParkingCondition` 让出 M） |
| `TrySend` 到满缓冲 | 返回 `kWouldBlock`，不 park |
| `Recv` 到已关闭且空缓冲 | 返回 `kClosed`（`T` 为默认构造值） |
| `Close` 两次 | 第二次返回 `kAlreadyClosed`；析构里也会调一次 `Close()`，幂等 |
| 手动 Fiber（未归属 Scheduler）里 `Select` | 返回 `kInvalid`（`ParkingCondition` 那条路径返回 `kCancelled`，两者不等价） |
