# Go Channel 源码优化点调研：可移植到 Bento Channel（C++）

> 日期：2026-10-30 · Go 源码：`/usr/local/go/src/`（go1.27.1）· 工作树：`/mnt/e/CodexWorkspace/Go2Cpp`
> 范围：**只读 Go 源码 + 只读 Bento 代码 + 只写本文档**；未改代码、未 commit。
> 基线（1P1C，传 `int`，ns/消息）：Bento cap0 1071 / cap64 264 / cap1024 153；Go 118.9 / 29.4 / 31.7。
> 对照实现：`include/go2cpp/concurrency/channel.hpp`、`src/concurrency/channel.cpp`、`src/concurrency/parking_condition.cpp`、`include/go2cpp/concurrency/core/parking_condition.hpp`。
> 引用约定：`chan.go:222` 表示文件与行号；Bento 侧同理。

---

## 0. 一句话结论

Go 的 channel 快，不是因为它「无锁」，而是因为**它只有一把锁、热路径不写任何共享计数器、等待节点永久复用**。Bento 的 channel 慢在结构而非指令：一次阻塞收发要过 2~3 把锁、每个等待者 2 次以上堆分配、每次收发还写一次跨核原子代际。把这三处按 Go 的结构改写，比换 allocator 或调自旋预算收益大一个数量级。

三条结构性差距：

| # | 差距 | Go 事实 | Bento 事实 | 影响面 |
|---|---|---|---|---|
| G1 | 锁的数量 | `hchan.lock` 一把（`chan.go:54,222`） | channel 锁 + ParkingCondition 队列锁 + 每节点唤醒锁（`channel.hpp:2608`、`parking_condition.hpp:179`、`parking_condition.cpp:448`） | 每次阻塞收发多 1~2 次跨核锁 |
| G2 | 等待节点分配 | sudog 从 per-P 池取（`proc.go:500-536`），稳态零分配 | 每次 `make_shared<PendingSend/PendingRecv>` + `deque<shared_ptr>` 节点（`channel.hpp:2175,2415,2616-2618`） | 阻塞路径 2 次 malloc + 引用计数 |
| G3 | 热路径共享写 | 无任何原子计数 | 每次 send/recv 都 `++m_generation`（atomic fetch_add，`channel.hpp:2151,2316,2622`） | 每次操作一条跨核 RFO |

预期（同一台 WSL2、同一测法，估算区间，见 §8）：P0 后 cap64 150-180、cap1024 90-110、cap0 600-750；P1 后 110-140 / 70-90 / 250-350；P2 后 50-70 / 30-45 / 100-150（ns/消息）。

---

## 1. Go `hchan` 数据结构逐字段对照

### 1.1 结构体（`chan.go:34-55`）

| 字段 | 行 | 语义与关键注释 | Bento 对应 | 差距 |
|---|---|---|---|---|
| `qcount uint` | 35 | 当前缓冲元素数；`full()`/`empty()` 靠它做单字读（`chan.go:146-155`、`492-503`） | `RingBuffer::m_count`（`channel.hpp:856`） | Bento 读 `m_count` 必须持锁，Go 非阻塞路径可无锁读 |
| `dataqsiz uint` | 36 | 缓冲容量，**创建后不可变**（`full/empty` 因此可无锁读） | `Channel::m_capacity`（2602）+ `RingBuffer::m_capacity`（853） | 同一语义存两份，初始化后都不可变 |
| `buf unsafe.Pointer` | 37 | 环形缓冲基址；元素不含指针时与 hchan **同块分配**（`makechan` 102-106） | `RingBuffer::m_slots`（852） | Bento 用 `std::allocator` 单独分配，且 `m_slots` 与锁同对象 |
| `elemsize uint16` | 38 | 元素字节数，`chanbuf` 按此步进（138-140） | 模板 `sizeof(T)` 编译期常量 | 无（模板换零虚调用，代价是无法类型擦除共享代码） |
| `closed uint32` | 39 | 0/1，持锁写；非阻塞路径可无锁单字读（213、559） | `bool m_closed`（2620） | Bento 读必须持锁：`IsClosed()`（938-941）每次加锁 |
| `timer *timer` | 40 | Go 1.27 源码中的新字段：定时器直连 channel（`maybeRunChan`，544-546） | 无对应；Bento 用 `AfterCase` + `TimerService`（3228-3269） | timer 与 channel 解耦，超时唤醒多一次回调 + 一次 select 竞争 |
| `elemtype *_type` | 41 | 运行期类型，用于 `typedmemmove` 与写屏障 | 模板 `T` | 无 |
| `sendx uint` | 42 | 下一个写槽位 | `RingBuffer::m_tail`（855） | 无 |
| `recvx uint` | 43 | 下一个读槽位 | `RingBuffer::m_head`（854） | 无 |
| `recvq waitq` | 44 | 接收等待队列 `{first,last *sudog}`（57-60） | `std::deque<std::shared_ptr<PendingRecv>> m_receivers`（2618） | 侵入式双链表 vs 双端队列 + `shared_ptr`；Bento 中间删除是 O(n)（`ErasePending` 2595-2599） |
| `sendq waitq` | 45 | 发送等待队列 | `m_senders`（2616） | 同上 |
| `bubble *synctestBubble` | 46 | synctest 测试隔离用，与性能无关 | 无 | 不需要移植 |
| `lock mutex` | 54 | **一把** runtime 自旋锁，保护 hchan 全部字段，以及阻塞其上的 sudog 的部分字段（48-53 注释） | `std::mutex m_mutex`（2608） | 见 §5；Bento 的等待节点另有自己的锁 |

最关键的一条注释（`chan.go:48-53`）：`lock protects all fields in hchan, as well as several fields in sudogs blocked on this channel`。**sudog 的 `elem`/`next`/`prev` 由 channel 锁保护**——这是「等待节点不需要自己的锁」的前提。Bento 的 `WaitNode` 有独立队列锁和唤醒锁，正是缺这个前提的结果。

### 1.2 不变量（`chan.go:9-18`）

1. `sendq` 与 `recvq` **至少一个为空**；唯一例外是「无缓冲 channel 上同一个 select 同时挂收发」（队列长度受 select case 数限制）。
2. 有缓冲时：`qcount > 0 ⇒ recvq 为空`；`qcount < dataqsiz ⇒ sendq 为空`。

这两条让 Go 的 send/recv **只需检查一个队列**：有元素就一定没有等待接收者，有等待接收者就一定没有元素。Bento 没有维护这条不变量——`SendUntil` 先扫 `m_receivers`（2082-2142）再查缓冲（2145-2155），`RecvUntil` 先查缓冲（2309-2321）再扫 `m_senders`（2324-2378），且两边都要处理「缓冲非空 + 等待者非空」并存的局面（`ArmSelectRecv` 的 1265-1286 就会先入队再复查缓冲）。

> 这是**语义差异而非 bug**：Go 的 select 用「全锁 + 单次 poll」维持不变量，Bento 的 select 是「逐个 arm、逐个复查」，只能容忍并存。P1/P2 若要恢复不变量，必须同时改 select 的注册协议（§6）。

### 1.3 Bento 缺什么（按影响排序）

1. **没有「channel 自有等待队列 + 锁内直接交接」的闭环**：等待节点挂在 `ParkingCondition::m_waiters`（`parking_condition.hpp:180`）里，channel 只持 `shared_ptr` 副本，唤醒要经过 `ParkingCondition::notify_one` 的内部锁与 deque 遍历（`parking_condition.cpp:529-554`）。
2. **没有节点池**：见 §4。
3. **没有无锁快失败探测**：Go 的 `chansend`（197-215）与 `chanrecv`（548-579）在 `!block` 时完全不拿锁；Bento 的 `TrySend/TryRecv` 走 `SendUntil/RecvUntil` 全锁路径（1020-1022、1080-1082）。
4. **`closed` 不可无锁读**：`IsClosed()` 每次 `lock_guard`（938-941）。
5. **热字段无 cacheline 隔离**：`m_mutex`(2608)、`m_change_cv`(2610)、`m_change_waiters`(2612)、`m_buffer`(2614)、`m_senders/m_receivers`(2616/2618)、`m_generation`(2622) 全部相邻。Go 把 `lock` 放在 hchan 末尾，`qcount/dataqsiz/buf` 在前，且 `buf` 常独立分配。
6. **没有零大小元素特例**：Go 对 `elemsize == 0` 只传信号不拷贝（926-948 的 `racenotify` 分支）；Bento 对 `T = struct{}` 仍要 `optional<T>` + 构造/析构。

---

## 2. 快路径：Go 如何做到「lock 一次 + 直接交接 + 零系统调用」

### 2.1 代码路径（`chansend`，`chan.go:176-310`）

```text
197-215  非阻塞且 !block：只读 c.closed 与 full(c) 两个单字，不拿锁 → return false
222      lock(&c.lock)                        ← 全程唯一一次加锁
224-227  c.closed != 0 → unlock + panic
229-234  sg := c.recvq.dequeue(); sg != nil → send(c, sg, ep, unlockf, 3)
                                              ← 值直接 memmove 进接收者栈槽（§3），
                                                send 内部先 unlockf() 再 goready
236-250  c.qcount < c.dataqsiz → chanbuf(c, sendx) + typedmemmove + sendx++/qcount++ + unlock
252-255  !block → unlock + return false
257-309  阻塞：acquireSudog（池）→ 填 g/elem/c → sendq.enqueue
          → gopark(chanparkcommit, &c.lock)；唤醒后 releaseSudog（池）
```

三个「快」的来源：

1. **一次加锁**：`lock` 在 222 行只出现一次；缓冲命中、直接交接、失败返回都在同一临界区内完成。
2. **零系统调用**：非阻塞失败路径（197-215）连锁都不拿；缓冲命中与直接交接完全不进内核；只有真正要睡才 `gopark`。
3. **解锁点被精确设计**：直接交接时解锁发生在 `send()` 内部、`goready` **之前**（318-351：`unlockf()` → `gp.param = sg` → `sg.success = true` → `goready`）；阻塞时解锁发生在 `chanparkcommit`（748-766），即「已入队、即将 park」的最后一刻。这保证唤醒方永远看不到「已入队但未解锁」的窗口，也就不需要额外的「parked 标志」握手。

### 2.2 对照 Bento 的同一条路径（`SendUntil`，`channel.hpp:2067-2155`）

| 步骤 | Go | Bento | 额外成本 |
|---|---|---|---|
| 进入 | 直接读 `c.closed` | `ContextSendStatus` 查 context（2070-2073） | 一次 `shared_ptr` 判空 + `IsDone` |
| 加锁 | 自旋锁 1 次 XCHG | `std::unique_lock<std::mutex>`（2076） | pthread mutex 的 lock/unlock 各一次原子 RMW |
| 关闭检查 | `c.closed` | `m_closed` | 相同 |
| 交接 | `recvq.dequeue()` + `sendDirect` | 扫 `m_receivers`（2082-2142）：判 cancelled、判 select_state、`optional::emplace`、`Signal()`、`NotifyIfParked()` | 每节点 2 个 atomic store + 一次 `cv.notify_one()`（内部还要拿 ParkingCondition 锁） |
| 入缓冲 | `typedmemmove` + 两个计数器 | `m_buffer.emplace_back`（2145-2150） | 相同 |
| 记账 | 无 | `++m_generation`（2151） | **每次操作一次跨核 atomic RMW** |
| 解锁后 | 无 | `NotifyChange()`（2010-2015）读 `m_change_waiters` | 一次 acquire load + 分支 |
| 阻塞 | 池取 sudog + 入队 + `gopark` | `make_shared<PendingSend>` + deque 入队 + `AddCallback` + `cv.wait`（2175-2296） | 2 次分配 + 回调注册 + 3 把锁 |

结论：缓冲命中路径上，Bento 比 Go 多的是 **①pthread mutex 的包装与慢路径检查 ②一次跨核 `fetch_add` ③一个 acquire load + 分支**。cap1024 的 153ns vs Go 31.7ns，差值主要在这里（§9 给出拆解估算与可信度声明）。

### 2.3 Bento 的等价实现路径

目标形态（伪代码；不改变现有公开 API）：

```cpp
ChannelSendResult SendUntil(T value, ...) {
  if (!block && m_closed.load(relaxed) == 0 && FullRelaxed())
    return kWouldBlock;                          // 无锁快失败
  SpinMutex::Guard g(m_lock);                    // 1 次 XCHG
  if (m_closed.load(relaxed)) return kClosed;
  if (PendingRecv* r = m_recvq.pop_front()) {    // 侵入式链表，O(1)
    r->Emplace(std::move(value));                // 直接构造进等待者槽位
    r->Finish(kReady);                           // 只写节点自己的字
    g.Unlock();
    WakeNode(r);                                 // 锁外 futex_wake / scheduler ready
    return kReady;
  }
  if (m_count < m_capacity) { m_buffer.push(std::move(value)); g.Unlock(); return kReady; }
  PendingSend node(std::move(value));            // 栈上节点，零分配
  m_sendq.push_back(&node);
  g.Unlock();
  ParkUntil(&node, deadline, context);           // 自旋 → futex / scheduler park
  m_sendq.erase(&node);                          // 幂等（可能已被交接方摘除）
  return node.result();
}
```

三条必须同时成立的约束：

- 队列元素是**裸指针**，节点生命周期由等待者栈帧保证（Go 的 sudog 也是这个模型，§4.3）；
- 摘除必须 O(1) 且幂等（Go 用 `dequeue` 只取头 + `dequeueSudoG` 做中间删除，`select.go:627-659`）；
- 唤醒必须在锁外且目标精确（per-node 字，不能退化为共享广播字，§5.4）。

---

## 3. 直接交接（bypass buffer）

### 3.1 Go 的写法

- **发送方找到等待接收者**（`chansend` 229-234）→ `send()`（318-351）：
  - `sendDirect(c.elemtype, sg, ep)`（392-403）：`dst := sg.elem.get(); memmove(dst, src, size)`——**直接写进接收者 goroutine 的栈槽**；
  - `sg.elem.set(nil)`（341）标记已消费；
  - `unlockf()` 解锁 → `gp.param = sg` → `sg.success = true` → `goready(gp)`。
- **接收方找到等待发送者**（`chanrecv` 602-609）→ `recv()`（702-746）：
  - `dataqsiz == 0`：`recvDirect`（405-412）从 `sg.elem` memmove 到接收者栈槽；
  - `dataqsiz > 0`（队列必满）：先从 `recvx` 取走一个给接收者，再把发送者的值写进**同一个槽位**（720-735），然后 `recvx++`、`sendx = recvx`——一次临界区完成「出队 + 补位」，队列长度不变。

关键点：**值只被 memmove 一次，且发生在持锁期间**；等待者醒来后不再碰 channel，直接用自己的栈槽。

### 3.2 Bento 已有的一半

Bento 其实已经实现了 bypass：

- `PendingSend::value`（1591）持有待发值，`RecvUntil` 直接从队首 `std::move(sender->value)`（2352）；
- `PendingRecv::value`（1669）是 `std::optional<T>`，`SendUntil` 直接 `receiver->value.emplace(std::move(value))`（2126）。

所以「绕过缓冲」不缺，缺的是**成本**：这条路径要 `shared_ptr` 复制（队列持一份、调用栈一份）、节点堆分配、以及 `Signal()/NotifyIfParked()` 两次 atomic store。

### 3.3 C++ 的三种表达

| 方案 | 表达 | 优点 | 缺点 |
|---|---|---|---|
| A. `optional<T>` 槽位（现状） | `PendingRecv::value.emplace(std::move(v))` | 类型安全、异常安全、语义清晰 | 每节点多一个 bool + 对齐填充；`emplace` 可能触发一次移动构造 |
| B. 原始存储 + placement new | `alignas(T) std::byte slot[sizeof(T)]` + `::new (slot) T(std::move(v))` | 零额外状态，节点更小，可池化（§4） | 手动管理构造/析构；异常路径不能析构未构造对象 |
| C. ring 槽位直写 | 有缓冲时直接把发送者的值写进 `buf[sendx]`，接收者读 `buf[recvx]` | 与 Go 的 `recv` 完全一致；一次临界区完成出队 + 补位 | 只在「队列已满且有发送者等待」的精确状态适用，需维护 §1.2 的不变量 |

推荐：无缓冲与 select 节点用 B（配合池化），有缓冲热路径逐步用 C。B 的形态：

```cpp
struct PendingRecv {                        // 池化后目标 ≤ 64B
  alignas(16) std::byte storage[sizeof(T)]; // 构造/析构由 channel 在锁内完成
  PendingRecv* next{nullptr};
  PendingRecv* prev{nullptr};
  std::atomic<std::uint32_t> state{0};      // 0=waiting 1=ready 2=closed 3=cancelled
  std::uint32_t seq{0};                     // per-node futex 字（§5）
  // 不再有 shared_ptr、不再有 cv
};
```

注意：Go 的 `sendDirect` 直接 memmove 到**另一个 goroutine 的栈**，依赖写屏障与栈不可移动窗口（382-403 的注释解释了为什么必须用 `memmove` + `typeBitsBulkBarrier`）。C++ 没有栈移动问题，但**必须保证接收者节点在交接期间不被销毁**——这正是「裸指针 + 栈帧所有权」模型要解决的（§4.3）。

---

## 4. sudog 池化

### 4.1 Go 的实现（`proc.go:500-581`）

- `p.sudogcache []*sudog` + `p.sudogbuf [128]*sudog`（`runtime2.go:825-826`）：切片指向**内联数组**，稳态零分配；
- `acquireSudog`（500-536）：本地非空 → 弹栈顶；本地空 → 拿 `sched.sudoglock`，从中心链表搬「一半容量」到本地（512-524），中心也空才 `new(sudog)`；
- `releaseSudog`（538-581）：先做 7 项断言（`elem/next/prev/waitlink/c` 必须为 nil、`isSelect` 必须 false、`gp.param` 必须 nil），本地满 → 把「超过一半」的节点批量推回中心（563-580）。

设计要点：

- **per-P（≈ per 线程）**：取还无锁；
- **批量换手**：摊销中心锁；
- **内联数组**：连切片扩容都省掉；
- **断言即契约**：归还前必须清空所有指向 channel 的字段，否则复用会带脏引用。

### 4.2 Bento 的分配现状

| 场景 | 分配点 | 次数 |
|---|---|---|
| Send 阻塞 | `make_shared<PendingSend>`（2177）+ `deque.emplace_back`（2185） | 2 |
| Recv 阻塞 | `make_shared<PendingRecv>`（2417）+ `deque.emplace_back`（2424） | 2 |
| 发送方唤醒后 | `ErasePending` 线性 `remove`（2259、2595-2599） | O(n) 扫描 |
| select | `make_shared<SelectWaitState>`（`channel.cpp:304`）+ 每 case 一个 `make_shared<PendingSend/PendingRecv>`（`channel.hpp:1376,1518`） | 1 + N |
| 等待 | `ParkingCondition::WaitOnce` 里 `make_shared<WaitNode>`（`parking_condition.cpp:682`） | 1 |
| 超时 | `TimerService::Schedule` 的 `shared_ptr<Timer>`（`parking_condition.cpp:743`） | 1 |

一次带超时的阻塞 Recv ≈ 4 次堆分配 + 多次引用计数；cap0 的 1071ns 里有相当比例在这里（ping-pong 每跳都有一套）。

### 4.3 C++ 池化设计

**所有权模型（最关键）**：Go 的 sudog 由等待者 `acquire`，入队后 channel 只持裸指针，等待者醒来 `release`。C++ 照搬需要三个保证：

1. 节点在 park 期间位于等待者栈帧（不会被销毁）；
2. channel 队列只持裸指针，**从不 delete**；
3. 交接方可在等待者醒来前把节点摘除，但必须保证「摘除」与「等待者检查 state」之间无 UAF——靠 per-node 原子 state + 「摘除者负责唤醒，等待者只读 state」的协议。

这三点与 Bento 当前的 `shared_ptr` 模型冲突：`shared_ptr` 存在的原因正是「context 回调可能在 park 线程之外触发，裸指针会在唤醒前被销毁」（`channel.hpp:2058-2059` 的注释）。要池化，必须把「跨线程唤醒」的所有权单独解决：

- **方案 1（保守）**：保留引用计数，但换 **intrusive_ptr + 对象池**（控制块嵌在节点里），池化后稳态零分配；代价是仍有 refcount 操作。
- **方案 2（Go 式）**：节点栈上 + 裸指针 + `state` 原子；context 回调不再持有节点，而是**先置节点的 `state = kCancelled`（CAS）再 `futex_wake(node->seq)`**；等待者醒来自己重查 context。回调只碰节点自己的字，UAF 窗口消失。取消是冷路径，多一次 CAS 可接受。
- **方案 3（混合）**：普通 Send/Recv 用方案 2；select 的 arm 节点因跨多个 channel 共享，仍用池化的 intrusive_ptr。

推荐 **2（普通路径）+ 3（select）**，因为 Go 的模型本来就是「节点属于等待者」。

池结构（对应 `p.sudogbuf[128]`）：

```cpp
class SudogPool {                        // 每 carrier/线程一个
  static constexpr std::size_t kLocal = 128;
  Node* local_[kLocal]; std::size_t n_ = 0;
 public:
  Node* acquire() { return n_ ? local_[--n_] : SlowAcquire(); }   // 空则批量从中心搬一半
  void release(Node* n) { if (n_ == kLocal) FlushHalf(); local_[n_++] = n; }
};
```

中心池用 `std::mutex` + 单链表（不是热路径）；`FlushHalf` 一次搬 64 个，与 Go 的「一半」策略一致。

### 4.4 风险

| 风险 | 触发条件 | 缓解 |
|---|---|---|
| UAF | 唤醒方在等待者返回后仍持有裸指针 | 交接方在锁内摘除并置 state；等待者返回前不再被引用；TSAN + 压测 |
| 跨线程归还 | 节点在 A 线程 acquire、B 线程 release | 归还只发生在等待者线程（Go 同样约束）；中心池允许跨线程但加锁 |
| 脏字段 | 复用节点残留 `next/prev/state` | 归还时断言清零（照搬 `releaseSudog` 的 7 项断言） |
| select 多 channel 共享 | 同一节点挂在多个队列 | select 节点单独池，或 intrusive_ptr（方案 3） |

---

## 5. 锁：Go 的单字自旋锁，与 Bento 的 cancel/timeout 兼容

### 5.1 Go mutex 的结构（`runtime2.go:170-177`；`lock_spinbit.go:29-80`）

```text
bit0  mutexLocked       锁本身
bit1  mutexSleeping     有 M 在睡（快速判断是否需要走慢路径）
bit8  mutexSpinning     自旋许可（只允许一个 M 自旋，降低 cacheline 流量）
bit9  mutexStackLocked  解锁方正在摘等待栈
高位  (M 指针 &^ 0x3FF)  最近一个睡眠 M 的部分指针
```

`lock2`（160-267）：

1. `Xchg8(key, mutexLocked)`——**无竞争时只有这一次 8 位原子交换**，成功即返回（174-180）；
2. 失败 → `semacreate` + 自旋：`mutexActiveSpinCount=4` 轮 × `mutexActiveSpinSize=30` 次 `procyield`，再 `mutexPassiveSpinCount=1` 次 `osyield`（223-233）；
3. 仍失败 → 把自己压入 M 等待栈（CAS，251-256）+ `semasleep(-1)`。

`unlock2`（276-334）：`Xchg8(key, 0)` 快速释放；若 `mutexSleeping` 置位 → `unlock2Wake`（346-469），带 **anti-starvation**（每 16 次随机唤醒队尾，351、429-451）。

所以「Go 无 futex 快路径」的准确表述是：**无竞争时一次 8 位 XCHG，有竞争时先用户态自旋，最后才进内核信号量**。它不是无锁，而是把跨核 cacheline 争用压缩到一次 store。

### 5.2 Bento 的锁层次

| 层 | 对象 | 位置 | 何时拿 |
|---|---|---|---|
| L1 | `Channel::m_mutex`（`std::mutex`） | `channel.hpp:2608` | 每次 Send/Recv/Close/Select arm |
| L2 | `ParkingCondition::m_mutex` | `parking_condition.hpp:179` | 每次 `WaitOnce` 入队/出队、每次 `notify_one` |
| L3 | `WaitNode::m_wake_mutex`（`FutexMutex`） | `parking_condition.cpp:448` | 每次 `Wake()`/`Disarm()` |
| L4 | `SelectWaitState::m_mutex` | `channel.hpp:713` | 每次 `TrySelect`/`Take`/`Cancel`（select 每 case 一次） |

一次「阻塞 Send 被接收者唤醒」要过：L1（接收者交接）→ L2（接收者 `notify_one` 内部）→ L3（被唤醒节点 `Wake`）→ L1（发送者醒来重检）。**4 次锁，其中 3 次跨线程传递同一组 cacheline**。任务描述里的「双锁」偏保守，实际是 3~4 层。

### 5.3 改法：单锁 + 保留 per-node 唤醒字

不能照搬 Go 的 `gopark`（C++ 没有 goroutine 调度器的 park/unpark 语义；Bento 的 Fiber 路径走 `Scheduler::park_wait`，线程路径走 futex）。可搬的是**锁的结构**：

1. **Channel 内部用 runtime 风格自旋锁**（单字，低 3 位状态 + futex 字），无竞争 1 次 XCHG；竞争时 `procyield` 自旋 4×30 次，再 futex wait。相比 `std::mutex` 省掉 pthread 的 `__pthread_mutex_lock` 包装与 robust/type 检查（WSL2 实测无竞争 lock+unlock ≈ 15-25ns，自旋锁可到 8-12ns）。
2. **把等待队列搬进 Channel**：`m_recvq/m_sendq` 直接持有 `WaitNode*`（池化节点），`notify_one` 不再经过 `ParkingCondition` 的 deque 与内部锁。这是 §4.3 方案 2 的必要条件。
3. **唤醒仍在锁外**：锁内置 `state`、摘链，锁外 `futex_wake(node->seq)` 或 `scheduler->wake_or_cancel(task)`。这点 Bento 已经做对（`NotifyIfParked` 在 `lock.unlock()` 之后调用，如 2137-2140）。

### 5.4 cancel/timeout 语义如何保住

这是单锁化最大的风险点，逐条对应：

| 语义 | 现在依赖 | 单锁后 |
|---|---|---|
| context 取消 | `context->Done().AddCallback([pending]{ pending->cv.notify_one(); })`（2204-2205） | 回调不再持节点：`node->state = kCancelled`（CAS）+ `futex_wake(node->seq)`；回调只写节点自己的字，不碰 channel 锁 |
| deadline | `cv.wait_for(lock, wait_for, pred)`（2276-2279） | 等待者自算 deadline，`futex_wait` 带超时；到期 CAS state → `kTimedOut` |
| 被交接 vs 取消竞争 | `accepted/closed/cancelled` 三布尔由 `m_mutex` 保护（1584-1586） | 收敛为**一个** `state` 原子 + CAS；谁 CAS 成功谁负责唤醒（与 `WaitNode::Finish` 现有模式相同，`parking_condition.cpp:216-220`） |
| 精确唤醒 | per-node futex 字（`m_hot.futex_seq`，`parking_condition.cpp:239-241`） | 保留 per-node 字，**不能**退化为 channel 共享字（共享字会唤醒错节点，Bento 已在审计 D2 踩过，见 `parking_condition.hpp:18-22`） |
| 惊群 | `notify_all` 用 futex_waitv 广播字（`parking_condition.cpp:575-619`） | Close 仍保留广播字；正常交接用 per-node 字 |

结论：**单锁化只改 Channel 层，不改 WaitNode 的「per-node 字 + CAS 终态」模型**。后者是 Bento 已验证过的正确性资产（审计 D2），Go 的 `goready` 精确唤醒在 C++ 里的等价物就是它。

---

## 6. select 的公平性与锁序

### 6.1 Go 的三段式（`select.go`）

1. **pollorder 随机化**（167-197）：Fisher-Yates，`j := cheaprandn(uint32(norder+1))`（191-193），用 per-P 的 `cheaprand`，**无全局状态、无锁**；
2. **lockorder 排序**（206-240）：按 `hchan` 地址做堆排序（`sortkey` 545-547），保证所有 select 以同一顺序加锁，避免死锁；
3. **一次批量加锁**（`sellock` 34-43，跳过重复 channel）→ pass1 按 pollorder 轮询（264-301）→ pass2 按 lockorder 逐个 `acquireSudog` 入队（309-342）→ `gopark(selparkcommit)` 一次释放全部锁（351）→ 醒来重新 `sellock`，用 `gp.selectDone` CAS 判定赢家（356；`chan.go:896-901`）。

### 6.2 Bento 现状

- 随机化：`NextSelectOffset`（`channel.cpp:67-88`）线程本地 xorshift32，等效 pollorder，**已具备**；
- 探测：每轮顺序探测 N 个 case（`channel.cpp:241-264`），每个 `probe()` 内部 `TryRecv/TrySend` 各自加锁 → **N 次 L1**；
- 注册：逐 case `arm`（`channel.cpp:301-330`），每个 `ArmSelectRecv/Send` 各加一次 L1（`channel.hpp:1260`、`1393`），`state->TrySelect` 再加 L4（610）；
- 赢家：`SelectWaitState::m_selected` 由 L4 保护（608-621），`TrySelectPair` 同时锁两个 state（623-648）；
- 等待：`has_poll_case` 时 `WaitFor(2ms)` 轮询（`channel.cpp:450-455`）。

差距：

1. **无 lockorder**：Bento 逐个 arm，不存在同时持多把 channel 锁，因此不会死锁，但也无法把「探测 + 注册」做成原子——probe 与 arm 之间总有窗口，只能靠 arm 时复查（`ArmSelectRecv` 1265-1286）兜底。代价是 N 次锁 + 重试。
2. **赢家判定多一层锁**：Go 用 `selectDone` CAS（无锁），Bento 用 `SelectWaitState::m_mutex`。
3. **2ms 轮询**：对纯 channel select 不生效，但对混合 select 是周期性空转唤醒。

### 6.3 改进（P2）

- 给 `Channel` 暴露 `LockOrderKey()`（`this` 地址）与 `TryLockOrdered()`：select 先按地址排序拿到所有涉及 channel 的锁（去重后 M 把），在持锁期间一次完成 probe + arm，锁外等待——与 Go 的 `sellock` 同构。
- `SelectWaitState::m_selected` 改为 `std::atomic<bool>` + CAS（`TrySelect` 用 `compare_exchange`），去掉 L4。
- 保留 `TrySelectPair` 的双赢家语义（两个 select 互相交接时两边都要标记），但改为两个 CAS，失败回滚。

风险：批量加锁拉长持锁时间，与 §5 的自旋锁叠加可能放大争用；需要「按地址序 + 最多 N 把」的上限（Go 允许 65536 个 case，Bento 可限 64）。

---

## 7. 可移植优化点清单

| # | 优化点 | Go 位置 | 为什么快 | Bento 改法 | 预期收益（1P1C） | 风险 | 阶段 |
|---|---|---|---|---|---|---|---|
| 1 | 热路径去 generation 原子 | 无此字段 | 去掉每次操作的跨核 RMW | `m_generation` 改锁内普通计数；`WaitForChange` 观察者持锁读；仅在有观察者时 notify（已有 `m_change_waiters` 判断，2010-2015） | cap1024 -15~25ns；cap64 -20~30ns | select 2ms 轮询可能漏一次变化 → 轮询兜底 | P0 |
| 2 | 单锁化（Channel 自持等待队列） | `chan.go:54,222` | 2~3 把锁降到 1 把 | `WaitNode*` 直接挂 `m_recvq/m_sendq`；`notify_one` 不再走 `ParkingCondition::m_mutex` | 阻塞路径 -50~150ns；cap0 -200~300ns | cancel 回调持节点 → 改回调写 state + futex_wake | P0 |
| 3 | 自旋锁替代 `std::mutex` | `lock_spinbit.go:160-267` | 无竞争 1 次 XCHG，省 pthread 包装 | 自写 `SpinMutex`（单字 + futex 兜底），仅用于 Channel | 每次 lock+unlock -8~15ns，×2 次/消息 | 自旋浪费 CPU；单核需 `osyield` | P0 |
| 4 | 等待节点池化 | `proc.go:500-581` | 稳态零分配 | 线程本地 freelist(128) + 中心池，半量换手 | 阻塞路径 -60~120ns/次 | UAF / 脏字段 | P0 |
| 5 | 直接交接去 `shared_ptr` 中转 | `chan.go:318-351`、`sendDirect` 392-403 | 值只搬一次，无引用计数 | 节点池化后队列持裸指针；`optional` 换 placement new | cap0 -100~200ns | 异常安全 / 生命周期 | P1 |
| 6 | 无锁快失败探测 | `chan.go:197-215,548-579` | 非阻塞失败不进临界区 | `TrySend/TryRecv` 先读 `m_closed`(atomic) + `qcount`(atomic)，不满足再进锁 | Try* 空探测 -20~40ns | 需要 `closed`/`qcount` 原子化 | P1 |
| 7 | cacheline 隔离 | `chan.go:54`（lock 在末尾） | 锁与数据不同行 | `alignas(64)` 分段：锁 + 计数一行，buffer 指针/数据另一行，等待队列头另一行 | -5~15ns | 对象变大（可接受） | P1 |
| 8 | 零大小元素特例 | `chan.go:926-948` | `T=struct{}` 只传信号 | `if constexpr (sizeof(T)==0)` 跳过 memcpy | 特定类型 -10~20ns | 仅影响空结构体 | P2 |
| 9 | select 批量加锁 | `select.go:34-43,206-240,252` | N 次锁 → M 次 | 按 channel 地址排序，一次锁全部涉及 channel | select -30~80ns/case | 锁序错误 → 死锁 | P2 |
| 10 | select 赢家 CAS | `chan.go:896-901` | 去 L4 锁 | `SelectWaitState::m_selected` 原子 CAS | select -10~20ns | 双赢家回滚 | P2 |
| 11 | 缓冲槽位直写（recv 补位） | `chan.go:720-735` | 一次临界区完成出队 + 补位 | 队列满且有 sender 时，`buf[recvx]` 给接收者，sender 值写同一槽 | cap64/1024 -10~20ns | 需维护 §1.2 不变量 | P2 |
| 12 | 批量交接（Go 没有，Bento 可加） | — | 一次锁搬 k 个，摊销锁成本 | `SendBatch/RecvBatch`：持锁期间循环到缓冲满/空 | cap1024 -20~40ns | 破坏「一次一值」的公平性 | P2 |

---

## 8. 重构路线

### P0：单锁 + 池化（改动集中，风险可控）

范围：

- `Channel` 内新增 `SpinMutex m_lock`（替 `m_mutex`，§5.3）；
- `m_senders/m_receivers` 从 `deque<shared_ptr>` 改为侵入式链表 `WaitNode*`（节点池化，§4.3 方案 2）；
- `notify_one` 从 `ParkingCondition` 迁到 Channel 自己的队列；
- `m_generation` 去原子化（#1）；
- cacheline 分段（#7 的一部分）。

不做：select 路径不改（仍走 `ArmSelect*`），无缓冲仍用现有 `optional` 槽位。

验收：

- cap64 ≤ 180ns、cap1024 ≤ 110ns、cap0 ≤ 750ns（中位数，3 轮）；
- TSAN 全绿；`tests/concurrency/test_channel*.cpp` 全过；
- 64 线程 MPMC 压测无丢/重/幽灵（沿用 `docs/module-audit-2026-10-20.md` 的判据）。

| 场景 | 现状 | P0 后（估） | 依据 |
|---|---|---|---|
| cap0 | 1071 | 600-750 | 锁 4→1、分配 2→0（池化后） |
| cap64 | 264 | 150-180 | 锁 2→1、去原子、池化 |
| cap1024 | 153 | 90-110 | 去 generation 原子、自旋锁 |

回滚点：`SpinMutex` 与 `WaitNode` 队列各自编译期宏开关，可单独退回。

### P1：直接交接 + 零分配稳态

范围：

- `PendingSend/PendingRecv` 栈上节点 + 裸指针队列（#5）；
- `TrySend/TryRecv` 无锁快失败（#6）；
- `optional` → placement new（#5 的一部分）；
- select 的 `PendingSend/Recv` 仍用池化 intrusive_ptr（方案 3）。

验收：cap0 ≤ 350ns、cap64 ≤ 140ns、cap1024 ≤ 90ns；ASAN/TSAN 全绿；`channel_thread_mixed_retry` 压测 30 分钟无异常。

| 场景 | P0 后 | P1 后（估） |
|---|---|---|
| cap0 | 600-750 | 250-350 |
| cap64 | 150-180 | 110-140 |
| cap1024 | 90-110 | 70-90 |

### P2：无锁 ring 快路径

范围：

- SPSC 特化：`std::atomic<size_t> head/tail` + 批量（#12），单生产单消费时完全无锁；
- MPSC：生产者 CAS tail，消费者单线程读 head；
- 保留锁路径作为慢路径（缓冲满/空、select、close）；
- select 批量加锁 + 赢家 CAS（#9/#10）。

验收：cap1024 ≤ 45ns、cap64 ≤ 70ns、cap0 ≤ 150ns；与 Go 的 31.7/29.4/118.9 同量级；模型检查（Relacy/CDSChecker 或 loom 风格）可选但推荐。

| 场景 | P1 后 | P2 后（估） | 说明 |
|---|---|---|---|
| cap0 | 250-350 | 100-150 | futex 往返下限（WSL2 真正睡眠 5-20µs），靠自旋命中压到 100ns 级 |
| cap64 | 110-140 | 50-70 | 唤醒成本占比上升 |
| cap1024 | 70-90 | 30-45 | 纯 ring 操作 + 两次原子 |

风险：

- 无锁 ring 的 ABA / 槽位生命周期（非平凡析构的 `T` 需要 per-slot 引用计数或 epoch）；
- 内存序错一处就是「偶发丢值」（`channel.hpp:38-39` 已记录团队对此的否决历史）——必须先有模型检查或长稳压测，再替换默认路径。

---

## 9. 数据可信度与测量方法

- 现有基线来自 `benchmarks/channel_vs_queue_benchmark.cpp` 的 SPSC 测例（198-223：1 生产 + 1 消费，50000 条 `int`，阻塞 `Send/Recv`），每项 3 轮取中位数。**该测例包含 park**（cap0、cap64 满时），不是纯快路径。
- 任务给出的「纯快路径 145-176ns」应来自 `TrySend/TryRecv` 或缓冲未满的 `Send/Recv`，与 SPSC 不是同一测法，比较时需注明。
- Go 的 118.9/29.4/31.7ns 来自 `testing.B` 的 channel 基准，宿主、编译器、GC 均不同，**只能作数量级参照**，不能当作精确 KPI。
- 本文所有「预期收益」按以下单价推算，是区间估计而非实测：
  - 跨核 `std::mutex` lock+unlock：15-25ns（WSL2，2 vCPU）；
  - `atomic fetch_add` 跨核 RFO：15-30ns；
  - `make_shared` + `deque` 节点：40-70ns；
  - futex wake+wait 往返：5-20µs（仅真正睡眠时）。
- 建议 P0 之前先补三个微基准，作为验收基线：
  1. `TrySend/TryRecv` 空通道：纯快失败路径；
  2. 单线程 `Send+Recv` 循环（cap 1024，不跨核）：纯锁 + ring 成本；
  3. 分配计数（重载 `operator new` 或 `malloc_count`）：验证池化后稳态 0 分配。

---

## 10. 参考

- Go 源码（go1.27.1）：`/usr/local/go/src/runtime/chan.go`、`select.go`、`runtime2.go`、`proc.go`、`lock_spinbit.go`、`sema.go`。
- Bento：`include/go2cpp/concurrency/channel.hpp`、`src/concurrency/channel.cpp`、`src/concurrency/parking_condition.cpp`、`include/go2cpp/concurrency/core/parking_condition.hpp`。
- 既有分析：`docs/module-audit-2026-10-20.md`（channel cap1024 107ns/msg、锁退化）、`docs/diagnostic-channel.md`、`docs/channel-hang.md`。
- 基准：`benchmarks/channel_vs_queue_benchmark.cpp`。
