# 模块文档：同步原语（sync）

> 适用基线：`HEAD 1b46de8`（2026-10-25），WSL 工作树 `/UserData/CodexWorkSpace/Go2cpp`。
> 源码：`include/go2cpp/sync.hpp`（360 行）、`src/concurrency/sync.cpp`（2274 行）；
> 锁族：`include/go2cpp/concurrency/{rwmutex,recursive_mutex,timed_mutex,spin_mutex,locks}.hpp`；
> 等待底座：`include/go2cpp/concurrency/core/parking_condition.hpp`、
> `src/concurrency/parking_condition.cpp`、`core/futex_mutex.hpp`。
> 引用约定：`sync.hpp`/`sync.cpp` 指 `include/go2cpp/sync.hpp` 与
> `src/concurrency/sync.cpp`；`rwmutex.hpp`/`recursive_mutex.hpp`/`timed_mutex.hpp`/
> `spin_mutex.hpp`/`locks.hpp` 指 `include/go2cpp/concurrency/`；`parking_condition.*`
> 指 `include/go2cpp/concurrency/core/` 与 `src/concurrency/`；`futex_mutex.hpp` 指
> `include/go2cpp/concurrency/core/`。
> 状态：**现行**。本文合并 `docs/concurrency-park-points.md`、`docs/correctness_audit.md`
> （锁部分）、`docs/concurrency-baseline.md`（Mutex/RWMutex 数据），旧文档保留为历史快照。

## 1. 概述

`go2cpp::sync` 提供面向用户代码的同步原语，与内部唤醒门 `core::FutexMutex` 分层
（`src/concurrency/sync.cpp:5-18`）：

- **同一份代码在两种上下文都能等待**：受管 G 走 Scheduler park（让出 M），普通 OS
  线程走原生 futex/条件变量（真阻塞）；两者共享同一条 FIFO 队列并可互相唤醒
  （`include/go2cpp/concurrency/locks.hpp:20-24`）；
- **Mutex 是混合锁**：无竞争一次 CAS；受管 G 竞争时直接 park；普通线程先做很短的有界
  自旋（8 次 PAUSE，`sync.cpp:268-271`），再入 FIFO 队列；
- **两态状态机**：正常模式允许新来者插队，等待超过 1 ms 切饥饿模式，由 `Unlock` 把
  所有权直接交给 FIFO 队首（`sync.cpp:1005-1028`、`1203-1213`）；
- **其余原语**：`RWMutex`（写者优先）、`RecursiveMutex`（owner + 深度）、`TimedMutex`
  （超时返回 false）、`SpinMutex`（契约检查：临界区禁止 park）、`ConditionVariable`、
  `WaitGroup`、`ParkingCondition`（所有等待的最终出口）。

### 1.1 最小示例

```cpp
#include "go2cpp/sync.hpp"

go2cpp::sync::Mutex mu;
if (mu.Lock()) {                 // 可传 ContextPtr 取消；失败返回 false
  // 临界区：允许 park（IO/阻塞等待），但不要持锁做长 IO
  mu.Unlock();                   // 未持有时抛 std::logic_error
}

go2cpp::sync::ConditionVariable cv;
cv.WaitFor(mu, std::chrono::milliseconds(100));   // 返回时 mu 必被重新持有
```

## 2. API 一览

### 2.1 Mutex

| API | 位置 | 说明 |
|---|---|---|
| `Mutex()` / `~Mutex()` | `sync.cpp:1143-1179` | 析构要求无人持有、无等待者 |
| `Lock(ctx={})` | `sync.hpp:219`；`sync.cpp:1181-1183` | 阻塞直到持锁；`ctx` 取消/调度器关闭返回 false |
| `LockFor(timeout, parent={})` | `sync.hpp:222`；`sync.cpp:1577-1590` | `timeout<=0` 等价 `TryLock`；否则 `TimeoutContext` + `Lock` |
| `TryLock()` | `sync.hpp:226`；`sync.cpp:1592-1608` | 非阻塞；饥饿模式下也失败（禁止插队） |
| `Unlock()` | `sync.hpp:229`；`sync.cpp:1651-1775` | 正常模式释放+唤醒一个；饥饿模式直接交接；未持有时抛 `logic_error` |
| `lock()/try_lock()/unlock()` | `sync.hpp:233-235` | BasicLockable 兼容；`lock()` 失败抛 `logic_error`（`sync.cpp:1778-1783`） |
| `DebugSnapshotInternal()` | `sync.hpp:241`；`sync.cpp:1150-1173` | 只读快照：held/waiter/starving/woken/owner_gid 等 |

### 2.2 ConditionVariable

| API | 位置 | 说明 |
|---|---|---|
| `Wait(mutex, ctx={})` | `sync.hpp:277`；`sync.cpp:1887-1996` | 原子释放 mutex 并等待；返回时**重新持有** mutex |
| `WaitFor(mutex, timeout, parent={})` | `sync.hpp:280`；`sync.cpp:1998-2004` | 相对超时；返回规则同 `Wait` |
| `NotifyOne()` / `NotifyAll()` | `sync.hpp:284-287`；`sync.cpp:2006-2060` | 不要求持锁；无等待者时不累积 |
| `wait/wait_for/notify_one/notify_all` | `sync.hpp:289-297` | 小写别名 |

`Wait` 返回 `true` 仅当被通知；超时/取消返回 `false`，但锁仍被持有。唯一例外是
「重锁彻底失败」：抛 `std::runtime_error` 且锁**不被持有**（`sync.cpp:1835-1853`、
`1976-1986`）。

### 2.3 WaitGroup

| API | 位置 | 说明 |
|---|---|---|
| `Add(delta)` | `sync.hpp:328`；`sync.cpp:2110-2173` | 负值下溢/正值溢出抛 `logic_error`，计数保持不变 |
| `Done()` | `sync.hpp:331`；`sync.cpp:2175` | `Add(-1)`；归零时锁外唤醒整波等待者 |
| `Wait(ctx={})` / `WaitFor(timeout, parent={})` | `sync.hpp:334-338`；`sync.cpp:2177-2248` | 计数归零返回 true；取消/超时 false |
| `Count()` | `sync.hpp:344`；`sync.cpp:2250-2253` | 当前计数快照 |
| `DebugSnapshotInternal()` | `sync.hpp:342`；`sync.cpp:2096-2102` | `count + waiters` |

### 2.4 锁族（`locks.hpp` 汇总）

| 锁 | 关键 API | 位置 | 语义 |
|---|---|---|---|
| `RWMutex` | `RLock/RUnlock/Lock/Unlock/TryRLock/TryLock/RLockFor/LockFor` | `rwmutex.hpp:139-452` | 写者优先；写者票据 FIFO；按角色唤醒 |
| `RecursiveMutex` | `Lock/Unlock/TryLock/RecursionCount/IsLockedByCurrent` | `recursive_mutex.hpp:123-230` | 同一 G/线程可重入；深度上限 1000 |
| `TimedMutex` | `Lock/Unlock/TryLock/TryLockFor/TryLockUntil` | `timed_mutex.hpp:88-171` | 完全复用 `Mutex::LockFor`；超时返回 false |
| `SpinMutex` | `Lock/TryLock/LockFor/TryLockFor/Unlock/Guard` | `spin_mutex.hpp:140-274` | 临界区禁止 park；debug abort / release 降级 |

### 2.5 等待底座与扩展点

| API | 位置 | 说明 |
|---|---|---|
| `ParkingCondition::wait/wait_for/wait_until` | `parking_condition.hpp:113-152` | 带谓词；要求持外部锁；返回时仍持有 |
| `notify_one/notify_all` | `parking_condition.hpp:108-111` | Fiber 逐节点唤醒；native per-node futex / futex_waitv 广播 |
| `FiberWaitUnsupported()` | `parking_condition.hpp:160-161`；`.cpp:502-504` | 手动 Fiber 无 Scheduler 时为 true |
| `CancellationRequested()` | `parking_condition.hpp:154-155`；`.cpp:497-500` | 当前受管 G 是否已请求取消 |
| `DebugSnapshot()` | `parking_condition.hpp:163-167`；`.cpp:453-485` | 队列长度、节点状态、广播代际 |
| `WaitBackend` | `sync.hpp:95-127` | 可替换的 park/wake 协议（`Park/ParkForCleanup/Wake`） |
| `WaitSelector` | `sync.hpp:158-165` | 只改等待队列顺序，不改裁决；异常隔离 |
| `SetWaitBackendFactory` / `SetWaitSelectorFactory` | `sync.hpp:175-193`；`sync.cpp:920-994` | 只影响之后创建的等待节点/之后的争用 |
| `SetHeldLockParkDetection` / `HeldLockParkCount` | `sync.hpp:64-68`；`sync.cpp:2255-2265` | 持锁 park 检测开关与累计计数 |

### 2.6 内部唤醒门 FutexMutex（不属稳定公共 API）

`core::FutexMutex`（`core/futex_mutex.hpp:85-225`）只保护极短的唤醒门
（`WaitNode::{Arm,Disarm,Wake,Reset}`、IO 唤醒节点、`ParkingCondition` 节点门）：

- 状态字两位：`kLockedBit=1`、`kWaitersBit=2`（`futex_mutex.hpp:186-192`）；
- 无竞争一次 CAS，竞争时 `FUTEX_WAIT_PRIVATE` 睡眠，不自旋（`:98-122`）；
- `unlock()` 清锁位后推进唤醒链：`FUTEX_WAKE(1)` 返回 0 清等待者位；唤醒到已退出
  节点则继续唤醒下一个；最多 64 次（`:125-162`、`200`）；
- 跨线程解锁、不记录 owner（Go 风格，`:13-15`）。

## 3. 设计

### 3.1 Mutex 的两态状态机

状态字是一个原子字节，四个位必须一起理解（`sync.cpp:1005-1028`、`1051-1054`）：

| 位 | 含义 |
|---|---|
| `kLocked=1` | 锁已被持有 |
| `kWaiter=2` | 等待队列非空（诊断 + 「解锁必须进慢路径」提示） |
| `kStarving=4` | 饥饿模式：解锁必须把所有权直接交给 FIFO 队首 |
| `kWoken=8` | 正常模式下已唤醒一个等待者，它尚未获取/重新入队 |

**正常模式**：`Unlock` 先真正释放锁（清 `kLocked`），再唤醒一个等待者；被唤醒者必须
回到 `LockImpl` 顶部重新 CAS 竞争，新来者也可以插队（`sync.cpp:1203-1208`）。
`kWoken` 是「唤醒令牌」：同一时刻最多只有一个被唤醒者，避免每次 `Unlock` 都做一次
futex 唤醒的唤醒风暴（`sync.cpp:1716-1724`）。

**饥饿模式**：某个等待者从真正入队起累计等待超过 `kStarvationThresholdNs = 1 ms`
（`sync.cpp:241`），就在（再次）入队时置 `kStarving`。此后 `Unlock` **不释放
`kLocked`**，而是逐个 `TryFinishAndGrant()` 队首直到找到活节点，把所有权直接交给它
（`sync.cpp:1688-1715`；`WaitNode::TryFinishAndGrant` 在 `:359-367`）。新来者看到
`kStarving` 只能入队，不能插队（`sync.cpp:1317-1319`、`1354`）。退出条件：

- 队列清空（`sync.cpp:1712-1714`）；
- 被交接者自身等待未超过阈值，返回前清掉 `kStarving`（`sync.cpp:1504-1528`）——
  避免一次调度抖动让整把锁永久停在 futex 交接模式。

**令牌交接**：正常模式下 `Unlock` 摘除队首并置 `kWoken`，然后清 `kLocked`、锁外
`Wake`（`sync.cpp:1726-1752`、`1755-1769`）。如果令牌持有者因取消/`Context` done 退出，
`DropWakeToken()` 负责清 `kWoken` 并在锁空闲时补一次唤醒/交接，否则等待者会永久睡在
已经空闲的锁上（`sync.cpp:1068-1140`）。

### 3.2 快路径与等待路径

`LockImpl`（`sync.cpp:1236-1574`）的步骤：

1. `context` 已失效 → 返回 false（不拿锁）；
2. **快速路径**：状态无 `kLocked/kStarving` 时一次 CAS 拿锁（`sync.cpp:1257-1275`）；
3. 判定等待者类型：受管 G / 普通线程 / 手动 Fiber。手动 Fiber 无 M 可 park，直接
   返回 false（`sync.cpp:1277-1286`）；
4. 普通线程在正常模式下先做 `kNativeSpinAttempts`（默认 8，可用
   `GO2CPP_MUTEX_SPIN_ATTEMPTS` 覆盖）次有界自旋（`sync.cpp:1349-1385`）。历史值
   2048 在 SMT 下让多个线程 CAS 同一缓存行，反而更慢（`sync.cpp:243-267` 的标定表：
   8 次约 19–20 ns，2048 次 175–280 ns）；
5. 在 `m_mutex` 下发布 `kWaiter`（必要时 `kStarving`）并入队，`Arm()` 失败则回滚；
6. 等待结束：普通唤醒回到循环重新竞争；饥饿模式交接直接持锁返回；超时/取消从队列
   摘除且不持锁返回。

受管 G 竞争时**直接 park**，不自旋也不 yield（`sync.cpp:1228-1230`）——实测自旋只会
更慢。`WaitNode` 从线程本地缓存获取（`sync.cpp:611-632`），避免短临界区反复分配。

### 3.3 WaitNode 与 futex 等待

`WaitNode`（`sync.cpp:300-606`）的要点：

- 结果用 CAS 单赢家：`TryFinish(kNotified/kCancelled)`，先到先赢（`:338-346`）；
- 饥饿交接额外 `m_granted`：先写 `m_granted` 再 CAS `m_result`，保证等待者看到
  `kNotified` 时一定能看到「已授予」（`:348-367`）；
- **普通线程**走 `WaitNative()`（`:457-545`）：有界自适应自旋（`kWaitSpinBudget=2048`，
  约 18 µs；连续无进展才 `FUTEX_WAIT`）→ per-node `m_futex_seq` 代际字 → 重判终态；
  另有 `kWaitSpinTotalBudget=262144`（约 2.3 ms）防止无界忙等（`sync.cpp:66-79`）；
- **受管 G** 走 `WaitBackend::Park`，`Await()` 先读终态再决定是否 park，保留
  notify-before-park 语义（`sync.cpp:756-842`）；
- `Disarm()` 持 `m_wake_mutex` 等回调完成再清 `m_scheduler`，防止取消回调生命周期
  超过 Scheduler 关闭过程（UAF，`:326-332`、`:374-396`）。

`ContextSubscription`（`sync.cpp:684-718`）用 `weak_ptr + generation` 注册取消回调：
节点被回收复用后 `m_generation` +1，迟到的取消会被拒绝，不会误杀下一次等待
（`:379-395`、`:555-577`）。

### 3.4 ConditionVariable：契约与重锁

- `Wait` 必须持外部 `Mutex`，内部先入队再 `Unlock`，返回前重新加锁
  （`sync.cpp:1924-1967`）；
- 通知和取消都用 `TryFinish` 单赢家；`NotifyOne` 跳过已被超时/取消的僵尸节点
  （`sync.cpp:2006-2029`）；
- `NotifyAll` 先 `swap` 整个队列，再锁外逐个 `TryFinish` + `Wake`
  （`sync.cpp:2031-2060`）；
- **重锁失败**（调度器关闭，`LockForConditionWait` 返回 false）不能把「未持锁」返回给
  假定仍持锁的调用方：先用 `TryRelockWithoutParking` 在 8192 次预算内抢回，抢不回抛
  `std::runtime_error`，并累计 `g_cv_relock_failures`（`sync.cpp:1805-1853`、
  `1976-1986`、`2268-2272`）。这是审计 D3 的修复：旧实现会让 `RWMutex` 在未持锁
  状态下改容器（UB + 计数泄漏）。

### 3.5 WaitGroup

- `Impl` 只有 `m_count` 与 `m_waiters`，同一把 `m_mutex` 保护（`sync.cpp:2077-2087`）；
- `Add` 在锁内更新计数；归零时对整波 `TryFinish(kNotified)` 并 `swap` 队列，再锁外
  `Wake`（`sync.cpp:2110-2173`）；
- `Wait` 先无锁快判 `count==0`，再入队；`Await` 返回非 `kNotified` 时从队列摘除
  （`sync.cpp:2177-2241`）；
- `Add` 与 `Wait` 并发是调用方错误（Go 里会 panic/竞态）；计数归零后 `Add` 可开启
  新一轮（`docs/concurrency-5rounds.md` 的 workpool/pipeline 场景即如此使用）。

### 3.6 RWMutex：写者优先 + 票据 FIFO

状态机三形态：无锁 / N 读者 / 1 写者（`rwmutex.hpp:72-107`）：

| 请求 | 放行条件 | 实现 |
|---|---|---|
| `RLock` | `!m_writing && m_waiting_writers.empty()` | `rwmutex.hpp:139-180` |
| `Lock` | `!m_writing && m_readers == 0 && 自己是票据最小者` | `:216-258`、`:603-606` |
| `TryRLock` | 同 RLock，不等待 | `:299-314` |
| `TryLock` | 同 Lock（含「无等待写者」），不等待 | `:321-336` |

- 写者领取单调递增票据，`m_waiting_writers` 是 `std::deque<uint64_t>`，尾部插入天然
  有序，取消时线性 erase（`:629-634`）；
- 唤醒按角色拆分：有写者排队只唤醒写者（写者优先），没有写者才放行读者，0 等待者
  完全短路（`:80-90`、`:264-293`、`:554-573`）；
- 多写者时广播：只有队首能前进，其余写者一起从 futex 醒来并在 `m_state` 上流水推进，
  避免单播把每次交接串行化成一个 futex 唤醒延迟（`:554-559`）；
- 撤销队首票据时必须修补唤醒：队列清空则放行读者，否则唤醒下一个写者
  （`RepairWakeAfterWriteCancelLocked`，`:575-587`）；
- 内部锁丢失（`ConditionVariable::Wait` 抛 `runtime_error`）时，用
  `RecoverInternalLock`（8192 次 `TryLock`）抢回，再撤销票据/唤醒，抢不回则带明确
  异常退出（`:506-552`）。

### 3.7 RecursiveMutex：owner token + 深度

- owner token 编码两套身份（`recursive_mutex.hpp:29-47`、`:243-268`）：
  受管 G 为 `(Task::id() << 1) | 1`（奇数），普通线程为进程内自增偶数（从 2 开始）；
  不用 `Task*` 地址——地址会复用，旧 G 未解锁退出后新 G 可能继承 owner（审计 D4）；
- `Lock`：owner 是自己则深度 +1 立即返回；否则 `m_impl.Lock()` 后写深度=1、发布 owner
  （`:123-155`）；
- `Unlock`：非 owner 抛 `logic_error`；只有 owner 写深度，因此无需 CAS；深度归零时
  先撤 owner 再释放底层锁（`:161-186`）；
- 深度上限 `kMaxRecursionDepth = 1000`，超过抛 `logic_error` 且不改深度
  （`:93`、`:234-241`）；
- `RecursionCount()` 先用 `m_owner==0` 快速否定，避免每次跨库调用
  `Scheduler::current_task_ptr()`（`:211-224`）。

### 3.8 TimedMutex

完全复用 `Mutex::LockFor`（`timed_mutex.hpp:16-20`）。`TryLockFor/TryLockUntil` 先做一次
`TryLock`，避免无竞争时也创建 `TimeoutContext` 并注册定时器（约 10 µs，`:145-171`）。
非递归；`Lock()` 在调度器关闭/G 被取消时抛 `logic_error`（`:84-104`）。

### 3.9 SpinMutex：契约检查而非真自旋

协作式调度下自旋 = 烧 CPU 且不让出，比 park 更糟；`SpinMutex` 内部**完全复用**
`sync::Mutex`，只在持锁期间给当前 G 打 `no_park_depth` 标记（`spin_mutex.hpp:29-34`）。
两种模式（`:36-45`）：

- **debug**（定义 `GO2CPP_DEBUG_LOCKS` 或未定义 `NDEBUG`）：`no_park_depth > 0` 期间
  park → 打印 G id/深度/调用栈后 `abort()`；
- **release**（`NDEBUG` 且未显式开启）：不 abort，降级为普通 `Mutex` 语义，打印一次
  警告并递增 `scheduler::spin_mutex_park_degrade_total()`。

普通 OS 线程没有受管 G，`no_park_depth` 无处记录，`SpinMutex` 退化为普通 `Mutex`
（`:114-117`、`:278-290`）。嵌套加锁按层递增/递减；`Guard` 提供 RAII（`:244-274`）。

### 3.10 ParkingCondition：唯一的等待原语

`ParkingCondition` 是运行时里唯一的「等待原语」：Scheduler idle、`sync::Mutex`、
Channel、Future 最终都落到它上面（`parking_condition.hpp:5-19`）。

两条路径：

- **Fiber**：Scheduler park；`WaitNode` 绑定 Task，唤醒方通过 `wake_or_cancel` 把 G
  放回调度队列，不经过内核 futex；
- **线程**：每个 `WaitNode` 自己的 32 位 futex 代际字。`notify_one` 只推进**选中
  节点**的字并只唤醒它（D2：共享代际字会唤醒错节点，被选中者留在内核睡眠队列里丢
  唤醒）；`notify_all` 用 `futex_waitv` 让等待者同时监听「节点字 + 共享广播字」，
  一次 `FUTEX_WAKE(INT_MAX)` 唤醒全部 native waiter，N 次系统调用降为 1 次；内核不
  支持时逐节点回退（`parking_condition.hpp:14-22`、`182-193`；
  `parking_condition.cpp:90-160`、`:575-619`）。

park 前自旋 `kSpinBudget = 2048`（`parking_condition.cpp:168-172`）：覆盖「唤醒已在
路上」的窗口；实测无自旋时 2 个等待者场景比 `std::condition_variable` 慢 3.3 倍
（`parking_condition.hpp:24-30`）。`WaitOnce` 的步骤与锁契约见 `.cpp:621-776`：
谓词在入队前重检、超时 Timer 用 `weak_ptr`、返回时外部锁一定被持有。

### 3.11 持锁 park 检测

Go2Cpp 把 `read/write/connect/accept/sleep` 全 hook 成 park 点，用户很容易「持着锁走
了」。检测**只告警 + 计数**，不 panic、不强制解锁（`sync.hpp:51-61`）：

- 普通 `Mutex`：`Lock` 成功后 `Task::note_mutex_locked()`，`Unlock` 按 owner G 递减；
  默认 Debug 开、Release 关，可用 `GO2CPP_DETECT_HELD_PARK=1/0` 覆盖；
- `SpinMutex`：`Task::no_park_depth`，debug abort / release 降级；
- park 入口在 `Task::park_for_scheduler()` 汇聚两类检查（`docs/concurrency-park-points.md`
  §6）；`HeldLockParkCount()` 读的是 `scheduler::mutex_held_park_total()`
  （`sync.cpp:2261-2265`）。

### 3.12 扩展点

`WaitBackend`（`sync.hpp:95-127`）把「如何 park 当前任务」抽象出来：`Park`、
`ParkForCleanup`（draining 中的 G 暂停等待锁 owner，保证重锁后再退出）、`Wake`。
默认实现 `SchedulerWaitBackend`（`sync.cpp:136-207`）在 `.cpp` 内出现 Scheduler 类型，
公共头因此与调度器解耦；`CreateWaitBackend()` 共享一个默认实例，避免每个等待节点
重新分配控制块（`sync.cpp:971-994`）。

`WaitSelector`（`sync.hpp:141-165`）只改变等待队列顺序，**不能改变裁决方式**：排序后
仍由 `Unlock` 按 FIFO 逐个 `TryFinish`；`TryLock` 调用者不能插队。调用约束是「持队列
锁时同步调用、不得阻塞」；异常会被 `Unlock` 捕获并退化为默认 FIFO（`sync.cpp:887-916`）。
未注册选择器时热路径只多一次原子读（`s_has_selector`，`sync.cpp:869-875`）。

## 4. 边界与陷阱

### 4.1 不可重入（Go 语义）

`sync::Mutex` **不可重入**：同一执行流重复 `Lock()` 会自死锁。`ConditionVariable::Wait`
要求调用方已持锁，内部会 `Unlock` 再重锁，因此「持锁状态下再次 Lock 同一 Mutex（含
经由 Wait 重入）」同样是错误（`docs/concurrency-5rounds.md` §5.1 的真实自死锁案例）。
需要重入请用 `RecursiveMutex`，并先读它的移植警告（`recursive_mutex.hpp:9-27`）。

### 4.2 owner-gone / 持锁退出

- `sync::Mutex` **不记录 owner**（Go 风格允许 A 加锁、B 解锁），因此无法检测「持锁 G
  已退出」；锁会保持 `kLocked`，其他等待者只能等 `Context` 取消或调度器关闭
  （`sync.hpp:18-24`、`196-204`、`sync.cpp:1037-1040`）；
- `RecursiveMutex` 记录 owner token；若 G 未解锁就退出，token 残留，后续 `Unlock`
  会抛 `logic_error`，锁无法再被获取。必须在 G 退出前解锁（推荐 RAII，
  `recursive_mutex.hpp:60-67`）；
- `SpinMutex` 的 `no_park_depth` 也绑在 G 上：跨 G 解锁时按加锁时的 G 递减，依赖所有权
  移交本身的 happens-before；无同步的跨线程解锁本身就不安全
  （`spin_mutex.hpp:70-72`）。

### 4.3 临界区内不要 park

临界区内 IO / `time.Sleep` / 阻塞 channel 操作 / 竞争中的 `Mutex::Lock` 都会 park，
把「协作式等待」变成「持锁消失」，是 Go2Cpp 特有的死锁来源
（`docs/concurrency-park-points.md` §1、`spin_mutex.hpp:14-27`）。`*_f` 版本不 park，
但会真实阻塞整个 M，同样不适合放进临界区（`docs/concurrency-park-points.md` §3.1）。
需要 IO 的临界区请拆分：锁内只拷贝数据，锁外做 IO（`locks.hpp:36-46`）。

### 4.4 RWMutex 的使用边界

- 读锁持有者**不得再申请写锁**（升级必然自死锁；不提供 `Upgrade`）
  （`rwmutex.hpp:38-40`）；
- 写锁不可重入；同一 G 重复 `Lock()` 自死锁（`:44-45`）；
- 读锁可重入，但**仅在没有任何写者等待时安全**：已有写者排队时再 `RLock` 会等写者，
  而写者又在等调用者释放读锁 → 自死锁。持读锁期间如需再取读锁，用 `TryRLock`
  （`:46-50`）；
- `TryLock` 在「锁空闲但已有写者排队」时返回 false，这是为了不破坏写者 FIFO
  （`:99-101`）。

### 4.5 关闭/取消/调度器停机

- `Mutex::Lock` 失败返回 false 时**不持锁**；所有失败路径都在拿到锁之前返回
  （`sync.cpp:1226-1234`）；
- `ConditionVariable::Wait` 的 `false` 仍保证持锁；只有重锁失败抛
  `runtime_error` 时才不持锁（`sync.cpp:1976-1986`）；
- 手动 Fiber 上 `Mutex::Lock` 返回 false、`CV::Wait` 返回 false 且保留进入前的锁状态、
  `WaitGroup::Wait` 返回 false（`sync.cpp:1277-1286`、`1912-1914`、`2188-2194`）；
- 调度器关闭时 `LockForConditionWait` 可能无法再次 park；`TryRelockWithoutParking`
  在有限预算内抢锁，失败即抛异常而不是静默 UB（`sync.cpp:1817-1833`）。

### 4.6 销毁与生命周期

锁对象必须长于所有使用者和等待者；销毁一把仍被持有/有等待者的锁是未定义行为
（`sync.hpp:201-203`、`rwmutex.hpp:51-52`、`recursive_mutex.hpp:66-67`）。
`WaitGroup` 的 `Add/Done` 与对象析构并发同样是错误。

### 4.7 性能边界

- 无竞争 `Mutex` 只有一次 CAS（约 18–19 ns，见 §5.2）；
- 普通线程竞争时先 8 次自旋再入队；受管 G 直接 park——「Fiber 路径吞吐高、线程路径
  尾延迟受 futex 影响」是预期差异（`docs/concurrency-baseline.md` §4）；
- `RWMutex` 写者优先会让读者排队，`rwmix` 的 p99（14.9 µs）是原语层尾延迟的主要
  来源（`docs/concurrency-baseline.md` §4）；
- `ConditionVariable` 的 `NotifyAll` 在 0 等待者时也付出一次队列 swap；RWMutex 已在
  上层做了 0 等待者短路（`rwmutex.hpp:568-573`）。

## 5. 测试

### 5.1 ctest 用例

| 用例 | 源 | 覆盖 | 超时 |
|---|---|---|---|
| `go2cpp_tests`（`GO2CPP_TEST_FILTER=sync`） | `tests/concurrency/test_sync.cpp` | 混合同步、取消、notify-before-park、CV 重锁、WaitGroup 波次、持锁 park 检测 | 全局 |
| `go2cpp_tests`（`GO2CPP_TEST_FILTER=locks`） | `tests/concurrency/test_locks.cpp` | RWMutex/RecursiveMutex/TimedMutex 语义 + Fiber/线程混合 A–D + 压测 | 全局 |
| `go2cpp_tests`（`GO2CPP_TEST_FILTER=spin_mutex`） | `tests/concurrency/test_spin_mutex.cpp` | debug abort / release 降级契约 | 全局 |
| `go2cpp_tests`（`GO2CPP_TEST_FILTER=futex_mutex`） | `tests/concurrency/test_futex_mutex.cpp` | 内部唤醒门 | 全局 |
| `go2cpp_tests`（`GO2CPP_TEST_FILTER=parking_condition`） | `tests/concurrency/test_parking_condition.cpp` | PC wait/notify | 全局 |
| `go2cpp_sync_cancel_stress` | `tests/concurrency/test_sync_cancel_stress.cpp` | Fiber cancel + cleanup 丢唤醒复现器 | 300 s |
| `go2cpp_parking_condition_stress` | `tests/concurrency/test_parking_condition_stress.cpp` | PC 压力 | 300 s |
| `go2cpp_primitives_stress` | `tests/concurrency/test_primitives_stress.cpp` | Mutex 8×100k、Recursive 4×50k×5、Timed、Spin、Futex、WaitGroup 20 波×16×100 | 300 s |
| `go2cpp_primitives_cancel` | `tests/concurrency/test_primitives_cancel.cpp` | 原语取消 | 300 s |
| `go2cpp_park_wake_pair` | `tests/concurrency/test_park_wake_pair.cpp` | park/wake 配对 | 300 s |
| `go2cpp_concurrency_combinations` / `go2cpp_concurrency_fuzz` | `tests/concurrency/test_concurrency_combinations.cpp`、`test_concurrency_fuzz.cpp` | 原语组合/随机 | 120/180 s |

注册位置：`cmake/Go2CppTests.cmake`（`go2cpp_tests` 源列表 `:29-38`、`:53-54`；
`go2cpp_primitives_stress` `:125-133`；`go2cpp_park_wake_pair` `:145-153`；
`go2cpp_sync_cancel_stress` `:806-815`；`go2cpp_parking_condition_stress` `:831-840`）。
过滤器名称来自 `tests/test_main.cpp:86-94`（`channel`/`sync`/`locks`/…）。

### 5.2 性能数据（同机相对值）

| 指标 | 优化前 | 优化后 | 来源 |
|---|---:|---:|---|
| Mutex latch（8 线程） | 24–55 µs | **18.3–19 ns** | `docs/performance.md:10` |
| ParkingCondition pc-rtt | 32.1 µs | **3.4 µs** | `docs/performance.md:12` |
| RWMutex alloc/op | 3.00 | **0.02** | `docs/performance.md:16` |
| RWMutex RLock | 64–70 µs | **43–45 ns** | `docs/performance.md:17` |
| 锁（fiber 路径） | — | 12.47 Mops/s（80.2 ns/op） | `docs/module-audit-2026-10-20.md` §3 |
| PC D2 修复后 pc-rtt | — | 23.8 µs（修复前 18.9 µs，单轮，WSL2 噪声内） | 同上 §6.2 |

> `performance.md` 的 pc-rtt 3.4 µs 是汇总口径；`module-audit` §6.2 的同轮记录是
> 23.8 µs（修复 per-node futex 后立即测，WSL2 噪声大）。两处都保留，引用时注明口径。

调试监控开销基线（`docs/concurrency-baseline.md` §2，OFF 构建）：

| 场景 | OFF 吞吐 | p50 | p99 | FULL 慢倍数 |
|---|---:|---:|---:|---:|
| mutex4 | 22.48 M ops/s | 32 ns | 1.69 µs | 192.1× |
| mutex8 | 18.58 M ops/s | 32 ns | 825 ns | 152.5× |
| rwmix | 3.68 M ops/s | 43 ns | 14.9 µs | 65.6× |
| pc_rtt | 2.75 M ops/s | 681 ns | 841 ns | 16.1× |
| wg | 28.54 M ops/s | 85 ns | 1.74 µs | 9.8× |

v7 优化后（`docs/concurrency-baseline.md` §8）：FULL `mutex4` 从 192.1× 降到 **4.8×**，
`mutex8` p99 从 1.11 ms 降到 **3.85 µs**；事件/操作 `mutex4` 2.35 → 0.40。主要手段是
资源事件的栈/链独立采样与记录采样，而不是改锁本身（§8.2）。

### 5.3 测试装置与压测结论

- `test_primitives_stress.cpp`：Mutex 8 线程 × 100k、RecursiveMutex 4×50k×深度 5、
  TimedMutex 4×50k（含超时路径）、SpinMutex 4×100k、FutexMutex 4×100k、
  WaitGroup 20 波 × 16 worker × 100 Done（`tests/concurrency/test_primitives_stress.cpp:107-555`）；
- `test_locks.cpp`：RWMutex 语义/写者 FIFO、Recursive/Timed 语义、Fiber/线程混合
  A–D、三类压测（`tests/concurrency/test_locks.cpp:2070-2110`）；
- 5 轮随机：mixedlocks 场景 5 轮各约 63.9 万次操作、0 失败，断言
  `counter_mu == mu_locks`、`counter_rec == recursive_locks`、`version == rw_writes`、
  `torn == 0`、无写锁残留（`docs/concurrency-5rounds.md` §3.1、§4）；
- 审计数据：锁原语 2.24e8 操作 / 823 超时，正确性 0 失败；两个 P1 性能缺陷
  （Mutex FIFO 锁死、RWMutex 写路径）已修（`docs/module-audit-2026-10-20.md` §1-§2）。

### 5.4 复现命令

```bash
# 统一测试二进制
GO2CPP_TEST_FILTER=sync  ./build-work/go2cpp_tests
GO2CPP_TEST_FILTER=locks ./build-work/go2cpp_tests
GO2CPP_TEST_FILTER=spin_mutex ./build-work/go2cpp_tests

# 专项 ctest
ctest --test-dir build-work -R \
  'go2cpp_sync_cancel_stress|go2cpp_parking_condition_stress|go2cpp_primitives_stress|go2cpp_park_wake_pair' \
  --output-on-failure

# 性能基线（5 配置 × 3 轮）
bash tests/bench/run_concurrency_baseline.sh
```

> 说明：任务指定的 `/UserData/build/go2cpp-mcp-integrated` 为
> `GO2CPP_BUILD_TESTS=OFF`（仅 14 个 os/bufio 用例），并发 ctest 需在启用测试的构建
> （如 `build-work`）中运行。用例名与超时来自当前树的 `cmake/Go2CppTests.cmake`。

## 6. 旧文档合并与相关文档

| 旧文档 | 合并内容 | 现状 |
|---|---|---|
| `docs/concurrency-park-points.md` | park 点清单、临界区禁用、`SpinMutex` 契约、持锁 park 检测 | 顶部标注「已合并至 docs/modules/sync.md」 |
| `docs/correctness_audit.md` | Mutex 线性化点、shared_ptr 队列修复、Channel/Select 论证 | 顶部标注「已合并至 docs/modules/sync.md」 |
| `docs/concurrency-baseline.md` | Mutex/RWMutex/WaitGroup/PC 调试开销与 v7 数据 | 顶部标注「已合并至 docs/modules/channel.md 与 sync.md」 |
| `docs/concurrency-5rounds.md` | mixedlocks/workpool/pipeline/storm 场景与断言 | 顶部标注「已合并至 docs/modules/channel.md 与 sync.md」 |

相关文档：

- `docs/scheduler.md` —— park/wake 的 Scheduler 侧契约；
- `docs/state-machines/` —— 对象生命周期图（Channel 等）；
- `docs/modules/channel.md` —— 同目录的 Channel/Select 文档；
- `docs/correctness_audit.md`（历史）—— 完整线性化论证；
- `docs/performance.md` —— 性能汇总（本文数据来源）。

## 7. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-10-26 | 首版模块文档：合并 4 份旧文档，核对 `HEAD 1b46de8` 的 API/行号/数字；记录 pc-rtt 两处口径差异 |
