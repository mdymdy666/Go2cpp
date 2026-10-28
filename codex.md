# Go2Cpp 变更记录（主题版）

> **2026-10-18 压缩重写**：原文件是 10,068 行的时间线流水账（每个 agent 每轮追加一节）。
> 本版按**主题**重组，只保留「不知道就会再踩一次」的东西：
> **设计约束**（为什么这样、不能怎样）、**已知陷阱**（反直觉行为、踩过的坑）、
> **性能基线**（数字 + 测量条件）、**未完成项**（指向 `docs/NEXT-STEPS.md`）。
> 被删除的是：逐轮进度表、测试计数流水、「某 agent 做了什么」的过程记录、
> 已被后续改动覆盖的旧结论、与项目无关的调研。
>
> **本文件不是 API 文档**。模块语义看 `docs/modules.md` / `docs/design.md` /
> `docs/compatibility.md`；本文件只记经验与约束。历史版本见 git history。

---

## 0. 项目定位与当前基线

**Go2Cpp = 把 Go 的并发模型（GMP + 协程）带到 C++**，目标读者是写网络服务 / 存储 /
系统工具的 C++ 底层开发者。核心卖点只有一句话：**C++ 也能 `go func()`** ——
`read()` 看起来是同步阻塞调用，实际自动 park 协程、让出 worker 线程。

分层（详见 `ArchitectureDiagram/diagrams/G0-overview.md`）：

| 层 | 内容 |
|---|---|
| L5 应用协议 | `http` `websocket` `sse` `http2`（HPACK→帧→流状态机→连接层）、`quic`（picoquic 薄封装）、`http3`（可选） |
| L4 网络基础 | `socket` `address` `streams` `bytearray` `myendian`；`Transport` 抽象（Memory/Fiber/TLS 三实现） |
| L3 IO 后端 | `io`（IOManager + epoll）`io_backend` `fd_manager` `hook` |
| L2 并发原语 | `channel` `sync` `future` `parking_condition` `context` `error` |
| L1 GMP 调度 | `scheduler`（Task=G / Scheduler / P / M / worker_loop / sysmon）、`fiber` |
| L0 地基 | Fiber/swapcontext、FiberBin、ParkingCondition、Timer |
| 横切 | `log` `config` `go()` `defer` `FiberLocal` `thread_policy` `os` `metrics` |
| 调试 | `debug`（只读观察，不反向控制；整体删除后框架行为不变） |

**与 Go 的根本差异（所有设计约束的源头）**：

| 维度 | Go | Go2Cpp | 后果 |
|---|---|---|---|
| 调度 | 抢占式（异步抢占点） | **纯协作式**（只有显式 park/yield 点） | 长计算会饿死同 P 的 G；持锁 park 是 Go 不存在的死锁形态 |
| 栈 | 可增长（复制搬迁） | **固定栈**（默认 128 KiB，`fiber_stack_size`） | 深递归会失败；FiberBin 池化复用降低建栈成本 |
| 本地队列 | 有 `runnext` 优先槽 | 无 | 实测证明**不需要**（见 §1.9） |
| 锁 | `sync.Mutex` 可跨 goroutine 解锁 | 同样支持（无 owner） | 但因此无法退回 `std::mutex`（见 §2.6） |

**当前基线**：`ctest --test-dir build-quality` → **113/113 通过**（Release，约 17~66 s，
取决于机器负载）。**新增专项测试默认不注册进 ctest**，以保持与文档一致的 113 计数；
要注册必须同步更新所有出现计数的地方。

---

## 1. GMP 调度器

### 1.1 对象与四个位

| 对象 | C++ 类型 | 持有者与不变量 |
|---|---|---|
| G | `scheduler::Task` | 非终态期间由 Scheduler 注册表强持有；最多一个 `m_execution_claim` + 一个队列 claim |
| M | 私有 `Machine` + `std::thread` | 一个 worker 线程 + 首选 P；在 Scheduler 互斥量下动态加入/退休/join |
| P | 私有 `Processor` | 有界本地队列 + 活动 M 计数；只有声明 `BlockingRegion` 才允许替代 M 共享 P |

G 的队列/执行权由**三个原子位**描述（`scheduler.hpp:483-488`），必须一起理解：

| 位 | 含义 | 谁改 |
|---|---|---|
| `m_queued` | 在某个队列（本地/全局/救援）里 | 入队/出队，转换锁内 |
| `m_execution_claim` | 已认领执行权，即将/正在跑 | `try_mark_running` 取得；`run()` 退出时释放 |
| `m_run_claim` | **正在 `run()` 栈帧内**（防两个执行者同时进同一 Fiber） | `run()` 入口 CAS，出口释放 |

合法状态机：`new → runnable → running → runnable | waiting | dead | cancelled | failed`，
`waiting → runnable | cancelled`。**状态转移只归 Scheduler**；观察器只在状态锁外收事件，
插件回调不得改变当前 G 的执行权。

### 1.2 park 的两条等待记录 —— 必须分开 ★

**这是理解整个运行时的第一道门槛**：park（挂起）时，「我在等什么」的记录落在**两个完全
不同的地方**，由两个完全不同的源唤醒。

| 等什么 | 等待记录在哪 | 谁唤醒 | 代码位置 |
|---|---|---|---|
| **等锁**（`Mutex::Lock` 失败、`ConditionVariable::Wait`、`WaitGroup`、`channel`、`future`） | **`Mutex::Impl::m_waiters`**：`std::deque<std::shared_ptr<WaitNode>>` | `Unlock()` / `notify_*` | `sync.cpp:830` |
| **等 fd**（`read` 不可读、`write` 不可写、`accept` 无连接） | **`IOManager` 的 `FdSlot`**（epoll 注册 + 代际号） | epoll 事件 → IOManager 回调 | `io.cpp:427` |

两条路径都最终调用 `Task::park_for_scheduler()`，但**等待节点的所有权、唤醒源、取消路径
完全不同**。把两者合并（例如让 IO 也挂到 `m_waiters`，或让锁等待也进 epoll）会导致
「一方漏唤醒，另一方永远等」——这类 bug 不崩溃、不超时，只是不动。

### 1.3 park 时到底发生了什么（完整链路）

入口是 `sync.cpp:574` 的 `Await()`：

```cpp
if (!waiter->managed()) return finish(waiter->WaitNative());  // 普通 OS 线程：futex/cv
for (;;) {
    if (waiter->result() != kWaiting) return finish(...);     // notify-before-park 快路径
    backend()->Park(scheduler, task, std::nullopt, pred);     // 受管 G：park 会释放它的 M
    ...
}
```

1. `Task::state = kWaiting`（**先发布状态，再真正切栈**，否则唤醒会丢）；
2. **G 从 runqueue 移除**，执行权 `m_execution_claim` 释放；
3. 等待记录**留在** `m_waiters` / `FdSlot` 里（节点本身由等待者持有，`shared_ptr`）；
4. `Fiber::Suspend` 切回 worker，`worker_loop` 拿下一个 G 继续跑。

**关键：M 在此刻是 `MState::kIdle`，不是 `kWaiting`。** M 只是「暂时没 G 可跑」，
它仍在 `condition.wait` 上可被唤醒；一有 G 入队就醒。**把 M 想成会「死掉」是常见误解**，
由此会推出「park 会占死线程」的错误结论（实际 park 的收益正是**立刻空出 worker**）。

### 1.4 唤醒链路与「漏一次 notify」的代价 ★

```
Unlock()/notify():
  auto waiter = m_waiters.front(); m_waiters.pop_front();
  waiter->TryFinish(kNotified);          // 等待节点进入终态
  scheduler->wake_for_wait(task);        // Task::state = kRunnable + 入队（或 pending 位）
  // → 队列从空变非空时 notify_one()，唤醒一个 kIdle 的 M
```

**任何一处漏掉 `wake_for_wait` 或 `notify_one`，等价于让一个 G 永久 park**：
没有崩溃、没有超时、没有日志，程序「就是不往下走」。排查特征（已实测命中）：

- `gdb -p` attach 后看到 **main 停在 `Scheduler::shutdown_for → ParkingCondition::WaitOnce`**；
- **所有 worker 都空闲**，sysmon 与 TimerService 也空闲；
- **没有任何 G 在跑，却有一个已启动 G 永不进入终态** ⇒ 判定为**丢唤醒**，不是调度延迟。

这也是为什么 `test_sync` 结尾必须从「无限 `scheduler.shutdown()`」改成
`shutdown_for(5s)` + 超时 `watchdog_abort`：否则丢唤醒会表现为测试永久挂死而不是失败。

### 1.5 `m_execution_claim` / `m_run_claim` 必须在同一临界区翻转 ★

`scheduler.cpp` 的 `Task::run()` 收尾（有源码注释，别改）：

```cpp
m_execution_claim.store(false, std::memory_order_release);
// run_claim 必须与 execution_claim 在同一个临界区内释放。若晚一步（例如交给
// RAII 在函数返回时释放），新执行者会先拿到 execution_claim，再在 run() 入口
// 被 run_claim 拒绝并静默返回，把 G 永久留在 kRunning。
m_run_claim.store(false, std::memory_order_release);
```

配套：`run()` 入口 `m_run_claim.compare_exchange_strong(false→true)` 失败时，
**必须把 G 交回 `kRunnable` 并置 `m_deferred_enqueue`，同时清 `m_execution_claim`**，
否则这个 G 会停在 `kRunning` 且无人认领，`shutdown()` 永远等不到终态。

这两个位共同描述「谁在执行这个 G」，**必须原子地一起翻转**。这是一次真实事故的修复：
表现为 12 并发跑 sync 过滤时约 **1/36 概率**挂死。

### 1.6 动态 M 管理：基础 M 常驻、补充 M 阻塞结束即退休 ★

| 类别 | 标记 | 数量 | 退休条件 |
|---|---|---|---|
| **基础 M** | `replacement == false` | `min_workers`（0 时默认 = `processor_count`） | **永不因空闲退出**，常驻到 shutdown |
| **补充 M** | `replacement == true` | `blocking_workers > 0` 时创建，受 `max_workers`（默认 2P）约束 | **`blocking_workers` 归零后立即退休**（`leave_blocking()` 里 `notify_all`） |

- 旧行为「任意 M 空闲 250ms 就回收」只在显式 `allow_idle_shrink = true` 时启用，**默认 false**。
  理由：省一个 OS 线程收益极小，但「判定 `runnable == 0`」到「置 `kStopping`」之间若有新 G
  入队，就会拆掉一个本该干活的 M —— 与 §1.5 属同一类窗口。Go 的回收前提也是
  「阻塞调用已返回」，而不是「闲了一会儿」。
- **退休判定与 `active_workers`/P-local 计数递减必须在同一个临界区内完成**
  （「观测即最终状态」，不是「退出前再确认一次」）。
- `MachineSnapshot::replacement` 暴露该标记，便于验证基础/补充 M 分布。
- `max_workers` 默认 2P：并发阻塞数超过 P 时补充 M 会被截断，需要显式放大。

### 1.7 worker_loop 的窄窗口 —— 五次失败换来的规律

| 尝试 | 结果 |
|---|---|
| 把 `m_impl->mutex` 换成本项目 HybridMutex（带自旋） | 集成崩溃（`WaitOnce` 契约被改），回退 |
| 给 `m_impl->mutex` 加自旋预算（try_lock 128 次） | **-36%**：3 worker 1.60x→2.18x、4 worker 2.81x→3.48x |
| 拆出独立的 `global_queue` 锁 | 锁顺序反转（`enqueue` 全局→P，`worker_loop` P→全局）会死锁，放弃 |
| park 前做 `runnable` 无锁预判 | **-37%**，连 1 worker 都变慢 |
| 把 work stealing 移出全局锁 | **-34%**，且「30 µs 在 steal 上」的归因被证伪（1 worker 不走 steal 也变慢） |

**规律**：`worker_loop` 的「确认没活可干」与「真正开始等」之间有一个极窄窗口；
任何在窗口里做额外工作的改动（自旋/预判/重试）都会把「短暂的忙」放大成「持续的忙」，
反过来伤害真正持锁或持任务的 worker。`worker_loop` 的注释里早就写了这条
（「不能使用长自旋，否则一个 P 上的持锁 G 可能被同一 P 的等待 G 阻塞」）——
**读到注释不等于把它当约束**，这是五次失败最贵的一课。

**锁统计的结论（真正有用的那一份）**：瓶颈是**持有时长**，不是争用。

| 指标 | workers=1 | workers=4 |
|---|---|---|
| 每任务锁获取 | 0.06 次 | 1.02 次 |
| 每次等待 | 76.5 ns | 1,182.3 ns |
| **每次持有** | 7,453.2 ns | **30,542.0 ns** |
| 等待/持有 | 0.01 | **0.04** |

「等待/持有 = 0.04」意味着加自旋、加预判**都在优化错误的维度**。要动就得重构
admission 事务本身（让本地队列入队脱离全局锁保护，同时保持 `accepting/stopping` 语义），
这是一次独立的锁协议重构，不是调参。

### 1.8 sysmon 与 `BlockingRegion`

- `enable_sysmon=true` 时启动独立监控线程，按 `sysmon_interval` 扫描 `MState::kBlocking`；
  超过 `long_syscall_threshold` 就把 `processor_detached` 置真、从 P 的 attached 计数扣除、
  为该长调用申请**一个替代 M**（槽位 = `min_workers + detached_count`，仍受 `max_workers` 限制）。
- 这是**逻辑解绑**，不是 Go 的精确 `_Psyscall` 交接：**不会从另一个线程切断/迁移任意 C++ 栈**。
- **未 Hook、未包在 `BlockingRegion` 里的阻塞调用，sysmon 看不见**；raw syscall、第三方阻塞库、
  已达到 `max_workers` 的场景只能保持线程语义。
- `BlockingRegion` **不可移动**，记录进入它的 M，**不能跨 Fiber yield/park/迁移**；
  worker 在 G 任务边界发现仍活动的 region 会 fail-fast。
- `sysmon_pass_count()` 是**监控活性心跳**，在扫描尝试开始时递增，不代表本轮完成了 detach 扫描；
  部署自检应与 `sysmon_running()` 一起看。

### 1.9 与 Go 的剩余缺口（以及为什么先不做）

| Go 机制 | 现状 | 结论 |
|---|---|---|
| P 本地队列 + work stealing | ✅ | `Processor::queue` + `take_victim` |
| 全局队列（积压溢出） | ✅ | `global_queue` + `local_queue_limit` |
| M 阻塞脱离 P | ✅ | `BlockingRegion` + sysmon |
| **`runnext` 优先槽** | ❌ 零命中 | **实测否决**：积压 2000 任务时提交→执行 p50 = **8.9 µs**，而空队列是 **43.3 µs**。瓶颈是「没人醒着干活」（唤醒路径），不是「排在队尾」 |
| 抢占式调度 | ❌ | 需要信号 + 安全点 + 栈一致性，风险极高；先要有能复现「饿死」的压测 |
| 可增长栈 | ❌ | 需要搬栈修指针；FiberBin 池化已缓解重复建栈 |
| 自旋 M 数量上限 | ❌ | 只有 `idle_wait` 超时；收益需先测 |

> `runnext` 那次否决同时反证了「把调度器 condition 换成 futex 化的 ParkingCondition」
> 是对的方向 —— 唤醒路径正是延迟主体。

---

## 2. 锁与同步原语

### 2.1 分层锁：`sync::Mutex` vs `sync::SpinMutex` ★

Go2Cpp 是协作式调度，`read`/`write`/`connect`/`accept`/`sleep` 全被 Hook 成 park 点，
于是出现了 Go 里不存在的死锁形态：

```
G1: Lock(A) -> read(fd) -> park     // 持着 A 走了
G2: Lock(B) -> read(fd) -> park     // 持着 B 走了
G3: Lock(A) -> 等 G1 释放 A -> 永久死锁（fd 可能永不可读）
```

所以锁分两类，**这是代码审查的第一条规则**：

| 锁 | 临界区可以 park？ | 违反了怎么办 | 用在哪 |
|---|---|---|---|
| `sync::Mutex` | ✅ 可以（park G，M 复用） | **计数 + 一次性告警**，不 abort | 可能阻塞的临界区 |
| **`sync::SpinMutex`** | ❌ **禁止**，只允许纯计算 | **debug：打印 G id/深度/栈 → `abort()`**；release：降级为 Mutex 语义 + 计数 + 一次性警告 | 极短的数据结构更新 |

**`SpinMutex` 不是自旋锁**。协作式下自旋只会更慢（实测自旋预算 0/8/32 = 740/788/770 ns，
M=8/64 Fiber），它内部**完全复用 `sync::Mutex`**，只额外做契约检查：`Lock` 成功后
`CurrentG()->no_park_depth++`，`Unlock` 先递减再释放底层锁（记录加锁时的 G，
所以跨 G 解锁也能精确递减；普通线程无 G 可记，退化为普通 Mutex）。

**检测桩只有一个位置**：`scheduler.cpp` 的 `Task::park_for_scheduler()` **入口**（状态转换之前）。
所有 park 路径（`park_wait` / `park_io` / `park_with_reason` / `park_wait_for_cleanup`）
都汇聚到这里，一处检查覆盖全部协作点。两层标记**内层优先**：

| 条件 | 含义 | debug | release |
|---|---|---|---|
| `no_park_depth > 0` | `SpinMutex` 临界区 | abort + 栈 | 降级 + `spin_mutex_park_degrade_total()` + 一次性警告 |
| `held_mutex_count > 0 && no_park_depth == 0` | 普通 `Mutex` 持锁 park | 计数 + 一次性日志 | `mutex_held_park_total()` + 1 |

已知边界：普通线程不受检查；release 降级后同一进程只警告一次（长跑服务要监控计数器）；
计数器是进程级且不重置（测试断言用前后差值）；`*_f` 直调 libc **不触发**检查（它不 park，
只阻塞 M）；锁对象若比 G 活得久，owner `shared_ptr` 会延长 Task 生命周期。

**park 点清单**在 `docs/concurrency-park-points.md`（会 park / 不 park 但阻塞 M / 纯计算），
临界区审查口诀：**临界区里不要出现「等」**。

### 2.2 `FutexMutex` 只能当内部门锁 ★

`include/go2cpp/concurrency/core/futex_mutex.hpp`（取代已删除的 `HybridGate`）只用于
**极短唤醒门**：`WaitNode::{Arm,Disarm,Wake,Reset}`、IO 唤醒节点、`ParkingCondition` 节点门。
**绝不能拿它替代 `sync::Mutex`**，原因已实测：

| 场景（临界区内含 `Scheduler::yield`，等价于「替代 sync::Mutex」） | M=1 | M=4 | M=8 |
|---|---|---|---|
| `FutexMutex`（Fiber=2/8/64） | **死锁 / 死锁 / 死锁** | 894 / **死锁 / 死锁** | 1231 / 7516 / **死锁** |
| `sync::Mutex`（同口径） | 1029 / 654 / 600 | 1096 / 2273 / 21943 | 827 / 2608 / 85100 |

**根因**：`FUTEX_WAIT` 睡的是 **worker 线程（M）**。M 被睡住后，该 M 上的其他 G 全部停摆；
若持有锁的 G 也要靠这个 M 才能推进，就是死锁。**M=1 时三档全死**，因为唯一的 M 睡死。

对照：临界区为纯自增时（契约内用法），`FutexMutex` 与 `sync::Mutex` 同量级
（146/54/16 vs 146/58/14 等），说明**只要临界区极短、绝不让出，它就能工作**。

### 2.3 `FutexMutex` 的状态机与唤醒链（改错就会「睡死」）

状态字两个位：`bit0=locked`、`bit1=可能有线程睡在 futex 上`（0/1/2/3）。
**不变量：只要可能有线程睡在 futex 上，状态就永远不会是 0/1**，
这样新来的 `lock()` 无法用 `CAS(0→1)` 快路径绕过仍在睡眠的等待者。

旧实现 `exchange(0)` + 一次 `FUTEX_WAKE(1)` 的致命假设是「被唤醒者一定会接手锁并继续唤醒链」。
不成立：`FUTEX_WAKE` 只保证「唤醒了一个」，不保证它还能执行。构造性复现（`/proc/self/task/<tid>/wchan`
确认真睡进 futex 后，让 FIFO 队头放一个「醒来就退出」的 ghost waiter）：
WAKE 返回 1，`real` 永久睡死。**「只在 FUTEX_WAKE 返回 0 时重试」覆盖不了这个场景**（此处返回 1）。

现在的 `unlock()`：`fetch_and(~locked)` → 若有等待者位，进入唤醒链，每次 `FUTEX_WAKE(1)`；
返回 0 则 `CAS(2→0)` 收尾；返回 >0 但 locked 位仍为 0（被唤醒者已退出）就**继续唤醒下一个**；
有人接手立即停；最多 64 轮，超限保留等待者位交给下一个 `lock()` 推进（**有界，不空转**）。
回归证据：把实现换回旧版，新测试失败 13 项并以 25.8 s 超时结束；换回修复版 0.19 s 全绿。

已知边界：连续超过 64 个「已退出等待者」时 `unlock()` 会保留等待者位退出，若此后**永远没有新
流量**，剩余等待者不会自行醒来（有界优先的取舍，未加等待者侧超时）。

### 2.4 `sync::Mutex` 的 250 倍退化与修复 ★

**现象**：4 线程争用同一把 `sync::Mutex` 是 **15,954 ns/op**，8 线程 20,777 ns/op。

**根因（探针实测）**：4 线程 × 20,000 ops 下，每线程「进入等待队列 19,793 次、
`FUTEX_WAIT` 19,793 次」—— **除开头几个操作外每次加锁都真睡一次**。
链路是：`kWaiter` 常驻 → 竞争者只自旋 4 次就入队 → `WaitNative` 自旋 128（≈1.1 µs）
覆盖不了一次完整交接 → 每次操作一次睡眠。本机空闲 futex 往返 ~0.4 µs，loadavg 3~4 时
实测放大到 ~96 µs，这就是 25~30 µs/op 的来源。**`Unlock` 的 O(n) 队列遍历不是主因**
（队列长度只有 3~7）。

**修复**：入队前自旋 256 → **2048**（≈18 µs）；`WaitNative` 自旋 128 → **2048**
（改为「连续无进展才睡」）+ 262144 绝对上限（≈2.3 ms，严格有界）；新增 `m_futex_waiters`，
**只在确有人睡在 futex 上时** 才 `FUTEX_WAKE`（纯自旋交接完全在用户态完成）。
标定（8 线程）：256→~23 µs、1024→321 ns、**2048→225 ns**、4096→349 ns（多转无收益）。

| 线程 | BEFORE | AFTER | 加速 |
|---:|---:|---:|---:|
| 2 | 25.62 ns | 25.46 ns | 1.0x |
| 4 | **15,954.74 ns** | **60.33 ns** | **264x** |
| 8 | **20,777.13 ns** | **166.45 ns** | **125x** |

### 2.5 删掉「256 次 yield 重试」—— 7.4M 次 yield 换 196 次成功 ★

改前基线（`sync::Mutex`，临界区内 `Scheduler::yield`，5 次中位数）：

| sync::Mutex | Fiber=2 | Fiber=8 | Fiber=64 |
|---|---:|---:|---:|
| M=1 | 846.5 | 658.8 | 619.8 |
| M=4 | 1035.7 | 1925.3 | **21465.5** |
| M=8 | 1017.2 | 3783.9 | **79007.8** |

探针实测（M=8 / Fiber=64、32,000 次 lock）：协作 yield 循环进入 29,281 次，
累计 `yield_current()` **7,464,941** 次，其中只有 **196** 次真抢到锁；
其余 29,085 次烧完 256 次 yield 后仍然 park —— **每次 lock 平均 233 次上下文切换，全是无效功**。

第二层（删掉 yield 后才暴露）：yield 回队后 Scheduler 会 `notify_one()`，唤醒一个睡着的 M，
让它从**队尾偷走**刚回队的 G → 一次本地续跑变成 4~5 µs 的跨 M wake-from-sleep。
反证：只保留主循环 notify、关掉回队 notify，M=8/F=64 仍是 9,371 ns/op。

**新设计**：① 受管 G 竞争锁时**直接 `WaitNode + park`**（park 释放 M），不再 yield 重试；
② yield 回队用 `requeue_from_worker(task, false)`（**不 notify**），主循环只在
「本 G 真正终止且仍有其他可运行任务」时 notify；③ **不做 Fiber 忙自旋**（实测更慢：
持锁者要么在另一个 M 上跑——park/wake 交接实测只要 **390 ns**——要么自己也已挂起，自旋纯空烧）。

结果：64 Fiber **79,007.8 → 743.6 ns/op（106 倍）**；rounds=2000 时全部落在 560~694 ns/op。
**有意的取舍**：场景 A（纯自增）在 M=8/F=64 从 50.7 → 335.6 ns，因为那个 yield 循环正是场景 B
灾难的根因（绝对值仍 <0.5 µs）。

### 2.6 跨线程解锁与 FIFO 交接

- `sync::Mutex` **无 owner**，`Unlock` 不校验所有权，FIFO 交接。因此**不能退回 `std::mutex`**：
  Hook 场景存在跨线程解锁（Go 风格），`std::mutex` 的跨线程 unlock 是 UB。
- `core` 层**不能**用 `sync::Mutex`：`ParkingCondition` 是 `sync::Mutex` 的等待后端，
  用它会形成 `core → sync → core` 循环依赖。这正是 `FutexMutex` 存在的理由。
- futex 重构的教训：**只用 futex 替代「等待」，不替代外部锁**。外部 `unique_lock<std::mutex>`
  的契约保持不动，27 处调用点一行未改（`ParkingCondition` 的 wait/wait_for/wait_until/notify_*
  签名与 cv 完全一致）。
- **`WaitNative` 返回时外部锁必须保持解锁**（由 `WaitOnce` 关锁）。多写一次 `outer.lock()`
  会触发 EDEADLK；把 `WaitOnce` 契约从「返回时已加锁」改成「返回时保持解锁」会让集成测试崩溃
  （独立原语护栏全绿也挡不住）——**改锁契约 = 改所有调用方的隐含假设**。

### 2.7 channel 缓冲：对齐 Go `hchan`，不要「优化」成无锁

`channel` 的缓冲是**预分配数组 + `sendx`/`recvx`/`dataqsiz`**（就地构造，`cap==0` 不分配），
索引 `++` 后回绕，**锁内不做取模**（MPMC 下取模更慢）。刻意**不用 `std::deque`**：会偏离
Go `hchan` 语义且多一层间接。曾评估「原子标志 + `Try*` 无锁快路径」来优化空探测，**否决**：
偏离 hchan，且漏一处就是偶发丢值（这类 bug 不崩溃、只丢数据）。实测：SPSC 1024 268→226~239 ns，
MPMC 1024 738→652~694 ns，但 **MPMC 64 = 4,892→5,598~5,898 ns（退化 14~21%，未解决）**。

### 2.8 其它同步原语陷阱

- **`Timer` 句柄析构 = `Cancel`**。`auto t = ScheduleAfter(1ms, f)` 在循环迭代结束就取消，
  回调一个都不会跑；`(void)after(...)` 同理。写任何定时器测试都必须先把句柄存进容器。
- **`DoneSignal`：先 `notify_all` 再跑回调**，所以 `Done()` 返回**不保证** undo 回调已完成；
  移动后是空壳，`Wait`/`IsDone` 多处没有空检查。
- **等待队列不能存裸 `WaitNode*`**：队列 + 锁外 `Wake` ⇒ 等待方先销毁栈节点就 UAF（SIGSEGV）。
  现为 `deque<shared_ptr<WaitNode>>`；`WaitNode` 还必须有 generation，否则旧 Context 回调会
  污染缓存节点导致永久挂起。
- **`EventBatch` 在 `Lock()` 返回 false 时静默 return**，写方法会丢事件 ——
  不要靠「返回 void」判断已入队。
- **手动 Fiber 里 `Select` 返回 `kInvalid`**，而 `ParkingCondition` 返回 `kCancelled`；
  无 Scheduler 归属的等待必须返回 `kCancelled`（别以为两者语义一致）。
- **`noexcept` 函数体内构造 `lock_guard`**：加锁失败抛 `system_error` 直接 `terminate`
  （多处历史写法，已按 X-25 逐处收口；新代码用 try/catch 包住加锁）。
- **错误单例是函数局部 static**：进程退出期析构后再取会返回空 `ErrorPtr`，
  「超时」被误判为取消（`shutdown_for` 超时后仍有线程在跑时可见）。
- **`defer` 的恢复边界是 `thread_local s_defer_depth`**：同线程多路复用的另一个 Fiber
  可能消费不属于它的 panic。判据是 thread_local，所以**defer 回调内不得挂起或迁移 Fiber**。
- **`Context::Now()` 每次取全局锁并复制 `std::function`**，而运行时该 function 恒空；
  被 `Future::Wait`、每次建超时 Context 反复调用（已知热路径成本，未优化）。
  另注意**两个时间源**：`Context::Now` 可注入，`TimerService` 硬编码 `steady_clock` ——
  假时间仍会真等 30 s。
- **截止时间不饱和会回绕**：`now + max` 变成过去时间，「永久」Context 立即超时
  （用 `ContextDuration::max()` 且未饱和加法时）。

---

## 3. IO 与 hook

### 3.1 Hook 判定准则（唯一一条）

**本项目的唤醒机制是 `IOManager` 的 epoll —— 也就是「某个 fd 变得可读/可写」。**
因此：**只有当调用可以归约为「在某个 fd 上等待它就绪」时，它才应该 park。**
否则 park 之后**没有任何东西能唤醒它**，那会把「worker 被阻塞」变成「**Fiber 永久挂起**」——
后者更难查：没有崩溃、没有超时，就是不动。

| 类别 | 特征 | 处置 |
|---|---|---|
| A 可 park | 阻塞原因就是「某 fd 就绪」，epoll 能报 | hook，park Fiber |
| B 需桥接 | 阻塞事件不是 fd 就绪，但可用 eventfd/timerfd/signalfd 造一个 | **未实现**，只给方案 |
| C 不该 hook | 既不是 fd 就绪也无法桥接；或本身就是事件循环入口 | 保持 `BlockingRegion` + sysmon |

B 类（未做，方案已记录）：`fsync`/`fdatasync` → io_uring/eventfd；`wait4` → signalfd(SIGCHLD)；
`nanosleep` 类已改为 `cooperative_sleep_for`（eventfd + `WaitFor` 超时）而不是 timerfd。
C 类：`poll`/`select`/`epoll_wait`（事件循环入口，hook 会与 IOManager 内部的 `epoll_wait` 递归；
**Hook 对 libc 的 ABI 保持原样，不自动改写信号掩码、EINTR 或 revents**）、
`close`/`shutdown`/`fcntl`/`dup`/`dup2`、`open`（FIFO 无对端时还没有 fd）、`getaddrinfo`。

### 3.2 怎么识别 fd 类型 ★

| 判定 | 接口 | 说明 |
|---|---|---|
| socket | **`getsockopt(fd, SOL_SOCKET, SO_TYPE)`** | FIFO/pipe 返回 `ENOTSOCK` |
| FIFO/pipe | **`fstat` + `S_ISFIFO`**（`inspect_fifo`） | socket 接管失败后再试 |
| 普通文件 | `S_ISREG` | 内核同步完成，不会 `EAGAIN`，**可以**直调裸 syscall |

**不能只靠 `S_ISxxx` 做黑名单**：Linux 的 `anon_inodefs`（`eventfd`/`timerfd`/`signalfd`）
**不给 `st_mode` 类型位**（`S_IFMT == 0`，`ISREG/ISFIFO/ISSOCK/ISCHR` 全为 0），
任何黑名单都必然漏掉它们 —— 而它们恰好都会阻塞。所以只能**白名单**（见 §3.4）。

接管 socket/FIFO 后必须把 fd 切到 `O_NONBLOCK`（park 的前提），并通过 `fcntl` hook
对用户隐藏该位；`dup/dup2/dup3` 的 `clone_descriptor` 回退也要支持（dup 出来的副本共享
open file description，否则下一代会把内部 `O_NONBLOCK` 误判成用户意图而失去 park 能力）。
`OpenDescription` 用 `m_pollable` 判定能否等待，`m_socket` 退化为来源标记（只用于诊断）。

### 3.3 FIFO park 已合入：实测 39 vs 14~18 ★

| 调用 | 心跳增量 | 语义 |
|---|---|---|
| `read(pipe)`（park 后，400 ms 窗） | **39** | Fiber park，worker 立即空出 |
| `read(pipe)`（旧 `BlockingRegion` + sysmon，同窗） | **14~18** | worker 真阻塞，靠替代 M 接续 |
| `read_f(pipe)` | **0** | 直调 libc，真阻塞 |

> 判读：park 只多一次 ~1 µs 调度往返，而 `BlockingRegion` 要走「检测 → 申请 → 迁移」，
> 所以快 2 倍以上。基线口径：200 ms 空转 ≈ 20 次心跳（每 10 ms 一次）。

合入时产生的**契约变更**：`test_iomanager_untracked_fd_publishes_blocking` 原本用 pipe 验证
「未接管 fd 会发布 `M::Blocking`」，FIFO park 后 worker 不再阻塞，该断言必然失败。
采用**方案 A**：把该用例改成 `eventfd`（hook 明确不接管的 fd），契约本身不变。
**不选方案 B（park 前也构造 `BlockingRegion`）**：那会让 sysmon 把每次 FIFO 等待当成
「worker 被长系统调用占住」而白白创建替代 M，正好抵消 park 的收益并污染 sysmon 判断依据。

适用边界：socket / FIFO 上的 read/write → **park**；其它 fd（普通文件、eventfd、timerfd、
字符设备）→ 不接管，`BlockingRegion` + sysmon；`*_f` → 永远直调 libc。

### 3.4 `os::File` 只接受 `S_ISREG`（高危漏洞的修复）★

`os::File` 的读写走**裸 `syscall(SYS_read/SYS_write)`** 绕开 Hook，前提是「只有普通文件能进来」。
原实现**只拒绝 socket**，于是 FIFO/pipe、eventfd/timerfd/signalfd、字符设备全部放行 ——
它们在上面的 `Read` 会**真阻塞、卡死整个 worker 线程**（该 P 上所有 Fiber 停摆）。

修法（三个入口全覆盖，`Create` 走 `Open` 自动继承）：

```cpp
if (S_ISSOCK(info.st_mode)) { status.code = OsError::kIsSocket; return status; }
if (!S_ISREG(info.st_mode)) { status.code = OsError::kNotRegularFile; return status; }
```

- `kIsSocket` **保留**（「这是 socket」比「这不是普通文件」对使用者更有用）；
- **`RawOpen` 必须补 `O_NONBLOCK`**：`open("/tmp/some_fifo")` 在无写端时会**阻塞在 open 本身**，
  根本到不了后面的 `fstat` 校验；对普通文件是 no-op（POSIX 保证）。
- 9 种 fd 光谱实测：普通文件接受；FIFO/pipe/`/dev/urandom`/eventfd/timerfd/signalfd 拒绝
  `not-a-regular-file`；AF_UNIX/AF_INET socket 拒绝 `is-socket`。
- `readdir` 走 `getdents64`（strace 实证），所以 `os::Dir` 用 libc 的 `opendir/readdir` 是**正确**的：
  底层不发 `read`，Hook 碰不到它。

**方法论教训**：这个漏洞是在「11 个测试场景通过」的验收之后才发现的 ——
**边界正确性不能靠正向用例证明，得靠系统性枚举**（fd 类型光谱 × 4 个维度一张表就暴露了）。

### 3.5 `*_f` 原始调用 API（22 个）★

`read_f/readv_f/recv_f/recvfrom_f`、`write_f/writev_f/send_f/sendto_f`、`connect_f/accept_f`、
`sleep_f/usleep_f/nanosleep_f/clock_nanosleep_f`、`poll_f/select_f/epoll_wait_f`、
`close_f/shutdown_f/fcntl_f/dup_f/dup2_f`。

- `_f` = **raw / non-hooked**，头文件明确写了「**`_f` 不代表更快**」；在 Fiber 里调它会**真的
  阻塞整个 worker 线程**（实测 200 ms 窗内心跳增量 = **0**，而 hook 版会走 15~20 次）。
- 为什么比「运行时配置开关」好：编译期可见、只影响显式调用点、不存在「改了忘恢复」、
  普通线程同样可用。所以 `SetOpEnabled` 已降级为**仅诊断用**（全局开关＝全程序静默变慢的风险）。
- `*_f` 是全局 `extern "C"` 符号，**不在 `go2cpp::hook` 命名空间里**。
- 已知不一致：`hook.enable_shutdown` 配置项存在，但 `shutdown` **从来没被 hook**
  （`Originals` 里没有 `m_shutdown`），设开关无效（保留只为配置表完整性）。

### 3.6 IO 终止性排空的「队头阻塞」丢唤醒 ★

`State::drain_queue_locked` 原实现在队头 `begin_operation()` 失败时 `break`。
该队头正被 Context 取消回调的 `complete()` 在锁外 claim，随后 `notify_close()` **无条件删除
FdSlot**，队列中其余等待者失去 epoll 注册与定时器，**永久 park**。

修法：**遍历整个队列** —— 忙节点保留原位交给自身完成路径，其余节点逐个 claim + 移出 + 唤醒。
影响 `notify_close` / `cancel` / `cancel_all` / `fail_slot` / `shutdown`。
复现器：8 fd × 2 waiters × 200 轮，修复前 99/200 轮命中、138/1600 个等待者丢失 `kClosed`；
修复后 0/1600。

其它 IO 约束：IO 完成序固定（取队头 → 登记 operation → claim → 再出队）；
token 首检后、发布 waiter 前**必须再查 generation/FD**（close/dup2 与注册交错）；
内部零超时探测必须 `::syscall(SYS_poll)` **绕过 Hook**，否则会递归 park 当前 Fiber。

### 3.7 成本与测试写法

- `IOManager::WaitRead` 23.3~23.8 µs vs 裸 `epoll_wait` 7.3~7.4 µs（**3.3x**，
  ONESHOT rearm + 全局 State 锁 + park/wake，属设计成本，未改）。
- 测试写法（踩过多次）：
  - **必须加探针断言**：先证明「测量工具本身在工作」（如心跳 Fiber 真在跑），再断言被测行为。
    否则恒为真的断言与真正验证的断言，在报告里长得一模一样（`test_hook_raw` 场景 3 曾忘记
    `manager.start()`，心跳恒 0，断言「阻塞时心跳 <8」永远通过、什么也没证明）。
  - `SO_RCVTIMEO` **必须远大于观察窗**，否则阻塞 200 ms 就返回了，看起来「没阻塞」。
  - 并发断言用**相对速率**（park ≈2x 基线；BlockingRegion 只有 0.4~0.9x）而不是绝对阈值。
  - `Check(bool, const char*)` 需要两个参数；断言必须**可见**（未打印的失败会静默通过）。

---

## 4. 网络协议

### 4.1 HTTP/2 全栈（自研，已验证）

分层：`HPACK(+Huffman)` → `帧层` → `流状态机` → `连接层` → `Transport`。
**关键设计：把 IO 抽成 `Transport`**（`MemoryTransport` / `FiberTransport` / `TlsTransport`），
所以连接层 90% 的协议逻辑可以**完全脱离网络在内存里测**，生产只换 Transport 实现，
连接层一行不改 —— 这正是「把并发部分改改」的落地方式。

| 模块 | 状态 |
|---|---|
| HPACK（RFC 7541，含 Huffman） | ✅ 官方 §C.1~C.4.1 向量 + 随机压测 |
| 帧层（RFC 9113 §4/§6） | ✅ 10 场景 + 随机 3000 轮 |
| 流状态机 | ✅ 84 组合穷举矩阵 |
| 连接层（握手/请求/响应/流表/PING/SETTINGS/GOAWAY/RST） | ✅ 10 场景（含 CONTINUATION、流控） |
| 流控（RFC 9113 §6.9，连接级 + 流级，signed 64 位） | ✅ 窗口不足返回 `kFlowControl`（**不阻塞等待**） |
| TLS + ALPN（TLS1.2+ / SNI / `h2`） | ✅ `openssl s_server -alpn h2` 验证（TLSv1.3） |
| 互通 | ✅ curl 8.5.0(nghttp2 1.59) ← 我；Python 手写 h2c 服务端 ← 我（双向） |

**未做（不要假装支持）**：HTTP/2 over TLS 的**完整链路**（环境缺真 h2 TLS 服务器）、
服务端 TLS、**证书校验默认关闭**（`verify_peer=false` 是测试便利，**不是安全默认**）、
流控的阻塞等待（需 Fiber 条件变量）、优先级（RFC 已弃用）、PUSH 推送 API、会话复用/0-RTT。
TLS ALPN 测试**不进 ctest**（依赖外部 `openssl s_server` 与自签证书，会让 113 变成环境敏感）。

### 4.2 QUIC：不自己实现，用 picoquic 薄封装

自研 QUIC（varint/packet/frame，1702 行）**已删除**。现状：`picoquic_connection.hpp/.cpp`
（178+339 行）+ `example/network/picoquic_hq_client.cpp`，`GO2CPP_WITH_PICOQUIC` **默认 OFF**。
picoquic 库在 WSL `~/build-deps/`（**`/tmp` 会被重启清空，必须放 `~`**）。
`picoquic_create_client_cnx` 内部已调用 `picoquic_start_client_cnx`（再调一次返回 -1）；
测试环境必须 `picoquic_set_null_verifier`。
HTTP/3 建议**复用 picoquic 自带 `picohttp`**，不要自己写 QPACK。
教训：**「能编译」和「能用」是两个数量级**；`quic-go` 只作为阅读材料（Go 库，C++ 无法链接）。

### 4.3 HTTP / WebSocket / SSE 的坑

- **HTTP Parser 没有粘性错误态**：报错后重复喂入仍会报错，只因缓冲未丢弃 —— 不要据此以为
  `m_error` 是粘性的。
- **WebSocket**：`read_message` **没有超时**（对端不回帧即无限阻塞，表现为 EXIT=0 但 stdout 丢失）；
  `read_message` **不向调用者交付控制帧**（Ping/Pong 由库内部处理）；
  握手 `recv` 次数典型 150~250 次；测试可用 `socketpair` + `Socket::FromFd` 跑全流程，不需要真实端口。
- **SSE**：CR-only 换行是**确定性设计缺口**（缺 finish），不是 flaky；已按缺口修复。

### 4.4 调试模块（`debug`）：只读观察，绝不反向控制

结构性保证：**框架代码零 `#include` debug 头** —— 所以删掉整个 `debug/` 仍然可以构建，
这是「零影响」的**结构性**证明（而不是靠基准测出来）。

- 只能通过既有只读边界观察：`SchedulerObserver`、快照、`EventBuffer`。
  **事件回调只做一次非阻塞入队**；选中断点在安全点之外会让热路径变慢。
- `DebugTask` / `DebugScheduler` 用**代理（包装已建对象）**而非继承 —— 核心类多数是 `final`
  且**明确不去掉 `final`**（否则就是在改原框架）；可走「继承 / 组合 / 只读快照」三档。
- 代理需要访问内部状态时，`friend` + `private` 对**内联代理不生效**（实测），
  于是改为 `public` + `Debug` 前缀的接口，不动 `final` 与包含顺序（记为已知偏离）。
- 断点走**协作式 `Checkpoint`**（100 万次：无断点 53 ns、名称不匹配 72 ns），
  **不在调度器安全点加钩子**，也**永不强制 detach**。
- 全局选择器用二级快判（`s_has_selector`），**先发布对象再置标志、先清标志再清对象**，
  否则会读到空指针。
- `dump events` 用 `Peek` 只读不消耗；非法 target **报错而不是静默降级**。
- 观察者净开销 +3~6%（≈27 ns/事件，单 worker、2 万任务）；使用者已定案：
  **只保证不影响原框架性能，不再优化调试自身**（批量化实测只有 12.5%，方向已收敛）。

---

## 5. 工具库（Go 标准库对齐）

| 模块 | 实现来源 | 已知差异 / 边界 |
|---|---|---|
| `time` | 移植 Go 语义 | `Duration` 零值/`IsZero` 规则、`Sub` 用单调钟、`Equal` 只用墙钟；`Sleep` 直接 `::nanosleep`（靠既有 hook 提供合作式睡眠） |
| `streams` | 自研 `io.Reader/Writer` 族 | **不能叫 `io::`**（已被 IOManager 占用）；iostream 状态位无法表达 Go「n>0 且带错误」，必须自研 |
| `fmt` | 自研 Go `%v` 指令集 | **不做**「Go 格式串→fmt 格式串」转换层（否则双份解析器）；格式项键只能单字符，`T`/`F` 已内置 |
| `strings`/`strconv` | 薄封装 | `strconv` 底层 `from_chars/to_chars`（`std::stoi` 的 base/范围语义不对） |
| `json` | nlohmann 薄封装 | 真实风险是**解析后的递归操作**（dump/拷贝）；限深栅栏必须保留 |
| `math`/`rand` | `math` 委托 libm；`rand` = xoshiro256**(+splitmix64) | 与 Go 的 PRNG **序列不互通**（Go 1.23 用 ChaCha8）；未实现 `NormFloat64`/`ExpFloat64`/`Source` |
| `path` | 逐函数移植 Go 1.27.1 | Windows 分支**未真机验证**；未实现 `SplitList`/`VolumeName`/`ToSlash`/`WalkDir` |
| `compress` | **委托系统 zlib**（不自研 DEFLATE） | 析构**不自动 `Close`**（与 `bufio::Writer` 不自动 Flush 一致）；未实现预置字典/自定义 gzip 头/`Resetter` |
| `encoding` | 自研 base64/hex/binary/csv | base64 **拒绝** `\r\n`（比 Go 默认更严）、不校验尾部填充位、失败不返回部分结果；hex 失败不返回前缀；Binary 短输入返回错误而非 panic |
| `hash` | 自研 CRC32/Adler32/MD5/SHA-1/SHA-256 | 无 SHA-224/512/HMAC；MD5/SHA-1 头文件已标 `@warning`「不安全，仅校验/兼容」 |
| `container`/`bufio`/`sort` | — | `bufio::Writer` 不自动 Flush 是刻意一致的语义 |

**通用陷阱**：
- **`Promise` 不可复制**，不能按值捕获进 `go()` 的 `std::function`；`go()` 是异步的，
  在 `waiter->wait()` 之前读完成标志必然 false。
- **`worker_count()` 是存活 M 数**（惰性创建）；`PId`/`MId` 从 0 起、**0 合法**（不要用 `!=0` 判存在）。
- **`Task` 处于 Running 时 `request_cancel` 仍返回 true** 且不立即置终态；
  `cancel() == false` 只表示任务**已正常完成**。取消契约断言必须覆盖两条路径。
- **`ByteArray` 没有 `data()`**（`m_data` 私有），用 `to_hex_string()`。
- **`Fiber` 非多态**（`is_polymorphic == 0`，没有虚析构）：**不能**用 `Fiber*` /
  `unique_ptr<Fiber>` 销毁派生对象（如 `DebugFiber`），也不能把派生对象塞进 `Task::m_fiber`。
- **`machine id` 是全局递增、不从 0 开始**：不要断言 `machine.0`。
- **`noexcept` 与「异常被 try/catch 隔离」不可共存**（`-Wterminate` 会直接暴露）：
  需要在内部捕获并隔离异常的接口，就不要标 `noexcept`。
- **`chrono` 字面量在匿名 namespace 内不可见**（`using namespace` 只在 main 内生效）。
- **JSON 深嵌套的真实上限**（128 KiB Fiber 栈实测）：解析 ≥524,288 层（迭代状态栈）、
  `dump()` 1,162 层、拷贝 816 层（Release -O2；无优化时 609 / 185）。

---

## 6. 工程约束

### 6.1 构建

- **所有编译必须 `flock /tmp/go2cpp_build.lock` 包裹**（多 agent 并发会互踩 build 目录）。
- **`cmake --build <dir> --target <test>` 不会顺带重编它链接的库**：改了 `src/os/file.cpp`
  必须单独 `--target go2cpp_os`，否则手工跑探针链接的是旧 `.so`，会看到 `unknown` 之类假象。
  同理 `cmake --build <dir>` 的**默认 target 不包含所有库**，必须显式 `--target`。
- configure 失败时 `cmake --build` 的报错会误导 —— 改完 CMake 必须单独跑一次 configure
  并确认 `CMake Error` 计数 = 0。
- **没有 `-Werror`**：只有 `-Wall -Wextra -Wpedantic`，零警告靠人工保证。
- 构建在**干净窗口**下重跑才可信：并发构建期间会看到 `libgo2cpp_io.so.0: file too short`
  这类环境竞态（不是代码问题）。

### 6.2 测试

- **测试间共享固定路径会互相踩**。热加载测试曾用固定 `/tmp/go2cpp-dynamic.ini` + `.lock`，
  两个 `ctest` 同时跑就互相写/删对方配置；同类固定路径还有 `test_os_file` 的 `/tmp/go2cpp_os_test`、
  `test_bufio` 场景 3、`test_config_registry_macro`、`test_debug_cli`、`test_hash_scenarios` 等，
  现已逐个改为**按 pid 隔离**。**不要靠放宽断言掩盖。**
- 未知 `GO2CPP_TEST_FILTER` 必须**返回非零（2）**，否则「一个用例没跑」会被伪装成通过；
  不要用模块白名单（会与既有 `selected` 调用形成第二份清单）。
- 新增测试/示例必须登记 `cmake/Go2CppTests.cmake`；测试还要在 `tests/test_main.cpp` 注册
  以支持 `GO2CPP_TEST_FILTER`；示例要进 `example/README.md` 速查表。
- 涉及另一执行流或异步计数的断言必须**轮询 + 超时**，单次采样必然 flaky。
- CTest 陷阱：`WILL_FAIL` 与 `PASS_REGULAR_EXPRESSION` **互相反转**，无法表达
  「非零退出且输出匹配」；要验这类语义就自己写 fork/exec 分派（`GO2CPP_SPIN_MUTEX_CHILD` 是范本）。

### 6.3 双仓库陷阱（历史上三次改动全丢）★

`/UserData/CodexWorkSpace/Go2Cpp`（WSL ext4）与 `E:\CodexWorkspace\Go2Cpp` / `/mnt/e/...`
（Windows 侧）是**两个独立的 git 仓库副本**，各自有 `.git`。走错一棵树 = 改动全丢。

- **动手前先 `pwd`**，确认在 `/UserData/...`；终端一律 `wsl bash -lc`。
- 同步工作树用 **`rsync -rlD`（不要 `-a`）**：`-a` 带权限位，会让 60+ 文件显示为 `M`
  而 `git diff --numstat` 是 `0 0`（只有 `old mode 100755 / new mode 100644`）；
  同步后要 `git config core.fileMode false`。
- **`.git` 必须一起同步**，否则 Windows 侧 `git push` 会显示 "Everything up-to-date"
  而提交根本没推上去（这是最容易误判为成功的情况）；推完必须
  `git log --oneline -1 origin/<branch>` 核对远端 HEAD，不要只看 rc=0。
- **WSL 内 `getent hosts github.com` 返回 `::1`**（本地回环），`git push` 必然 443 失败 ——
  git 的网络操作一律在 Windows 侧；WSL 只做本地 add/commit/log。
- **工具也会骗人**：`read`/`grep` 有时会解析到 E 盘旧副本（9p），`pwsh` 双引号会吞 `$`
  （`echo $HOME` 得到 `C:Usersm`），内嵌反引号会被 bash 当命令替换而输出空。
  **关键结论要交叉验证**（同一问题用两种方式读一次），否则会得出「`go2cpp_network` target
  不存在」「WebSocket 无测试覆盖」这类**基于旧副本的错误结论**（真实发生过）。

### 6.4 脚本与编辑

- **仓库行尾不统一**（`Go2CppTests.cmake` = CRLF，C++ 源码 = LF）：脚本必须按 **bytes**
  读写，文本模式回写会造成整文件 diff。
- **不要用 `sed` 按名字删 CMake 行**（会留下孤立的 `-O2 -Wall ...)` 或误删
  `set_tests_properties`）。要整体恢复用 `git show <commit>:<path> > <path>`。
- 「读入-改写-写回」时对内容 `rstrip` 会丢掉文件末尾换行，产生多余 diff —— 改完看 `git diff`。
- **多层 shell 里 `$?` 不可信**（pwsh→wsl→sh）：退出码要直接读进程返回码；
  `g++ ... | grep -c ; ./check` 会**掩盖编译失败并跑旧二进制**。
- `grep -v` 按行过滤时，路径含关键字的文件会被整份滤掉（`grep -rn` 输出带文件名前缀）。
- **`fork` 前要 `fflush`**：子进程会 flush 继承的 stdio 缓冲，导致父进程输出重复一次。
- 用 `git archive` 快照验证「示例/测试引用的 API 在 HEAD 存在」，否则全新 checkout 构建失败。
- 不要把简单事写成脚本（删文件用 `git rm`、改 CMake 用编辑、验证用 `cmake --build`）。

---

## 7. 性能基线（数字只在标注条件下成立）

> **读法**：本文跨多轮累积，**不得跨组横向比较，也不得单独取最好的一节对外宣称**。
> 同一负载（2,000 Fiber / 32 线程）曾分别记录为 99~107 ms、8.5~8.8 s、6.7~6.9 s。
> 需要「当前值」时，以组内最新一条为准。宿主统一为 AMD Ryzen 9 8945HX / WSL2 / g++ 13.3.0。

### 7.1 调度器

| 指标 | 数值 | 条件 |
|---|---|---|
| worker 扩展性（condition → ParkingCondition） | 3 worker 12,196.9 → **2,165.7** ns/任务（5.63x）；4 worker 24,376.7 → **3,817.5**（6.39x） | build-bench Release -O2，20,000 任务，3 轮 median，loadavg 1.44 |
| 倍率 | 3 worker 7.85x→**1.60x**；4 worker 15.70x→**2.81x** | 同上；残余来自全局 `m_impl->mutex` |
| 每任务锁持有时长 | workers=1 7,453 ns；workers=4 **30,542 ns**（等待/持有 = 0.04） | `-DGO2CPP_COUNT_GLOBAL_LOCK`，build-profile |
| 提交→执行延迟 | 空队列 p50 **43.3 µs**；积压 2000 任务 p50 **8.9 µs** | 单 P，5 次中位数 |
| IO 高并发 | 2048 fd × 20 波：Fiber **266 ms** vs 线程池+epoll **856 ms**（0.31x，快 3.2 倍） | 负载 0.41~1.17 |
| 混合随机 8000 任务 | Fiber 36.62 ms vs ThreadPool 57.19 ms（0.64x） | p50 15.5 vs 15.3 µs |
| 纯 yield 负载 | 50,000×4 yield：Fiber **225 ms** vs 64 ms（**慢 3.52x**） | 已知：调度器全局锁持有时长 |
| channel 32,000 消息 | 79 ms vs 105 ms | 同上 |
| mixed-mutex 2000 Fiber | 16 ms vs 39 ms（0.41x） | 同上 |
| Fiber 建栈成本 | 75~90 ms（短命进程不划算） | 4096 fd 场景前置开销 |

### 7.2 锁

| 指标 | 数值 | 条件 |
|---|---|---|
| **`sync::Mutex` 争用（修复后）** | 2 线程 25.46 ns、4 线程 **60.33 ns**、8 线程 **166.45 ns** | `sync_mutex_contention_benchmark`，rounds/thread=20000，`taskset -c 0-7`，A/B×5 中位数，loadavg 1.37~2.34 |
| 同上（修复前） | 4 线程 15,954.74、8 线程 20,777.13 ns（264x / 125x） | 同上 |
| **锁争用对照** | 4 线程 **16.83 ns**、8 线程 **21.68 ns**（修复前记录为 **19.3 / 31.3**） | `go2cpp_futex_mutex_contention_benchmark`，loadavg 0.96 |
| HybridGate vs FutexMutex | 2/4/8 线程 16.90/82.41/169.09 → **12.01/13.63/17.10** ns/op | loadavg 2.83，`taskset -c 0-7` |
| 长临界区 CPU | 2 ms 临界区 + 2 等待者：1.355 → **0.044 ms**；8 等待者：0.398 → 0.139 ms | 同上 |
| 锁开销矩阵 vs `std::mutex` | 短临界区 0.62~1.43x，长临界区趋于一致（1000 次自增 0.96~1.07x） | `lock_overhead_matrix_benchmark`，Release -O3，taskset 0-7，reps=5 |
| **`sync::Mutex` + Fiber（持锁 yield）** | M=8/Fiber=64：**79,007.8 → 743.6 ns/op（106x）**；rounds=2000 时 595.8 | `futex_mutex_fiber_benchmark` 场景 B，rounds/fiber=500，5 次中位数，`taskset -c 0-7` |
| 理想自旋预算 | 0 / 8 / 32 → 740 / 788 / 770 ns（M=8，64 Fiber） | 结论：**不做 Fiber 忙自旋** |
| park/wake 交接 | **390 ns** | 同上探针 |
| `WaitNode` 线程本地缓存 | 32,000 次等待中 node_alloc=68、reuse=31,931（稳态零堆分配） | 同上 |
| ParkingCondition futex 化 | C2 90.76 → 71.74 ns；C4 4,351.02 → **1,727.95 ns**（快 152%） | 普通线程路径，rounds=20000，loadavg 1.43 |
| 调度器条件变量换 futex 的 C4 对比 | 见 §7.1 worker 扩展性 | — |

### 7.3 channel / future / 事件

| 指标 | 数值 | 条件 |
|---|---|---|
| **channel 有缓冲往返** | 129.2 → 51.3 ns/op；**三轮中位数 58.2 ns** | Release，`taskset -c 0`，1 万次，5 样本中位数 |
| channel 无缓冲会合 | 3,870.7 ns/op（两次分配 + fiber park，未改） | 同上 |
| future 已就绪 Get | 38.6 → 20.6 ns/op（三轮中位数 29.2） | 同上 |
| future pending TryGet | 118.4 → 75.1 ns/op | 同上 |
| future 生命周期 | 163.9 → 135.5 ns/op | 同上 |
| 事件采集批量化 | 单线程 22.47 → 19.65 ns/事件（约 12.5%）；`flush_batch` 16 已饱和 | 200k 事件，5 轮 |
| 调度器观察者开销 | 未装观察者路径零成本（快路径不取时钟/不加锁/不拷 shared_ptr） | 三档对照 |

### 7.4 IO / hook

| 指标 | 数值 | 条件 |
|---|---|---|
| **FIFO park 心跳** | park **39** / 旧 BlockingRegion **14~18** / `read_f` **0**（400 ms 窗；基线 200 ms≈20） | Release 单 worker |
| `WaitRead` vs 裸 epoll | 23.3~23.8 µs vs 7.3~7.4 µs（**3.3x**） | 三轮中位数 |
| 关 fd 唤醒 | HUP 返回 ready；`shutdown` 16/16 唤醒；`cancel_all` 3/3；close 竞态 0 丢失 | 审计测试 |
| 高负载压力 RSS | 50,000 任务峰值 72,828 → 72,900 KB（Δ −0.1%） | `/usr/bin/time -v`，增长周期 8→64 |

### 7.5 工具库

| 指标 | 数值 | 条件 |
|---|---|---|
| hash | CRC32 **3,421 MB/s**、Adler32 **5,099 MB/s**、SHA-256 **442 MB/s**（MD5 985、SHA-1 291） | 1 GiB 流，4 MiB 缓冲，单线程 Release |
| encoding | base64 编 1,036 / 解 905 MiB/s；hex 编 694 / 解 358 MiB/s | 2 MiB 输入，15 轮最快 |
| compress | 1 MiB 低熵 ratio 0.1034（9.7x）；gzip -1 248~475 MiB/s、-9 32~43；解压 589~1,400 MiB/s | 5 次中位数；高熵压缩 43~70、解压 1.5~7.4 GiB/s |
| path | `Join` 18.18、`Clean` 26.49、`Match` 47.15 Mops/s | 各 100 万次 |
| math | sqrt 429.69、exp 255.30、log 275.72 Mops/s | 各 1000 万次 |
| rand | 全局 `Intn` 113.18 Mops/s（8.84 ns）、实例 219.09 Mops/s（4.56 ns） | 单线程 |

---

## 8. 未完成项与已知问题

**完整的未完成清单在 `docs/NEXT-STEPS.md`**（含 QUIC/HTTP3、FIFO 架构决定、`fsync`/`wait4` 桥接、
os 小缺口、架构图长图等）。这里只记**会影响下一步判断**的：

| 问题 | 状态 |
|---|---|
| Fiber 路径的**丢唤醒**（cancel + cleanup 重锁） | **根因未定位**。12 并发 × 5000 轮可复现（当时 1/36）；bisect 样本不足，倾向「老 bug 被新测试强度暴露」。独立复现器 `test_sync_cancel_stress.cpp` **尚未注册 ctest**。**不要靠放宽超时判定修复。** |
| 4 并发全量 ctest | 历史 11/12（一次 `go2cpp_tests` 120 s 超时）；2 并发 3 轮全绿。要求全绿需先修上面的丢唤醒 |
| `go2cpp_http2_state_desync_repro` | 曾是 DISABLED，已修复转正（6 场景 35 断言） |
| `go2cpp_fiber_stack_limit_full` | ctest 下偶发失败，单独跑 3/3 通过（依赖「被信号杀死」的检测方式差异），非代码 bug |
| HTTP/2 剩余 | TLS 完整链路、服务端 TLS、证书校验默认值、流控阻塞等待、优先级、推送、会话复用 |
| 补充 M 与配置 | `allow_idle_shrink` **未接入 ini/config**（只有 C++ API） |
| 调度器事务重构 | admission 事务需重写（把本地入队移出全局锁），属独立任务，不是调参 |
| 2→3 worker 数量级跳变 | 1/2/3/4/6/8 worker → 1,706/1,172/11,509/22,943/25,637/23,519 ns/任务（**2→3 跳 9.8x**），且 2k/20k/80k 任务量都不摊薄 ⇒ 是**持续开销**而非启动成本。已排除启动开销，**未做 profiling** |
| MPMC channel 小容量 | 64 深度退化 14~21%（4,892→5,598~5,898 ns），未定位 |
| 已记录但不修 | `hook.enable_shutdown` 无效项；`X-22` 逐字节 recv；`X-12` 压测内存（A/B 实测 −0.1%）；`X-06/X-07`（WSL 无 TSan） |

---

## 9. 方法论（压缩成 11 条，都是真实踩过的）

1. **边界正确性不能靠正向用例证明，得靠系统性枚举**。os 的 9 种 fd × 4 维光谱表暴露了高危漏洞，
   而「11 个测试场景通过」什么也没说明。
2. **先测再改，不要先推断再改**。「随并发度增长」不等于「在 steal 上」，那次强预测被实测证伪
   （预测降到 ~1,400 ns，实际 5,769 ns）。
3. **注释里写的是约束，不是背景**。`worker_loop` 的「不能长自旋」被读到四次才真正理解。
4. **改锁契约 = 改所有调用方的隐含假设**，独立原语护栏全绿也挡不住集成崩溃。
5. **「检查了」不等于「把检查结果接到决策上」**：每一步失败都要直接 `return`，
   不要打印后继续（`grep error` 打印后继续、断言恒成立、编译失败不中断，各犯过）。
6. **增量添加远比修改既有类安全**：给 `ConfigVarBase` 加可变性分层 → `config_center` 两个测试回归；
   改为纯新增接口（宏注册/注销/分组导出）→ 零回归。
7. **测量口径决定结论**：`Task::wait_for()` 的超时等待曾让 channel 场景某轮报 30,080 ms
   （其余轮 79~90 ms），造成「调度器卡顿」的误判。基准里不要用逐个 `wait_for` 计时。
8. **没有收益的优化要回退并留下测量**：TimerBin（对象池）实测 446.23 → 443.08 ns/op（0.7%，
   噪声内），根因是 `shared_ptr` 控制块仍要单独分配 —— 回退实现，保留测试。
9. **使用者的明确要求不能被「时间不够」改变**：做不了就报告做不了，不要自作主张交骨架。
10. **基准必须预热**：不预热会把冷启动算进先跑的那一档，曾据此得出「档 0 比档 1 慢 20 倍」的
    荒谬结论；档序敏感的实验必须交错 A/B 并多轮取中位数。
11. **归因前先测「新代码不存在」的对照档**：4 worker 的基线本身就是异常的，
    先把不含新代码的档 0 测出来，再谈是不是新代码的问题（否则会把既有劣化算到新改动头上）。

---

## 10. 文档索引

| 想看什么 | 去哪 |
|---|---|
| 模块目录、CMake target、依赖边界 | `docs/modules.md` |
| GMP 调度器契约（状态机、park/wake、动态 M、shutdown、观察者） | `docs/scheduler.md` |
| **哪些函数会 park / 哪些阻塞 M / 临界区审查清单** | `docs/concurrency-park-points.md` |
| Hook 拦截判定准则与 FIFO 数据 | `docs/hook-blocking-policy.md` |
| 性能数据（分日期、分条件，含失败尝试） | `docs/performance.md` |
| 未完成项与建议顺序 | `docs/NEXT-STEPS.md` |
| 全局架构一图（分层 + 依赖 + `go()` 穿越路径） | `ArchitectureDiagram/diagrams/G0-overview.md` |
| 各模块细化图（G1~G11） | `ArchitectureDiagram/diagrams/` |
| 测试命令与测试矩阵 | `docs/testing.md` |
| 兼容性边界、配置项、可扩展点 | `docs/compatibility.md`、`docs/configuration.md`、`docs/extensibility.md` |
| HTTP/2、QUIC 细节 | `docs/network.md`、`docs/quic.md`、`docs/quic-lib-evaluation.md` |
| 16 组状态机审查与并发风险 | `docs/state_machine_audit_2026-10-18.md`、`docs/concurrency_audit_2026-10-18.md` |
| GMP vs thread/epoll 五场景压测（2026-10-18） | `docs/gmp-vs-thread-epoll-2026-10-18.md` |

---

## 11. 2026-10-18 GMP vs thread+epoll 五场景压测（deepseek）

**改动内容**：新增 `benchmarks/gmp_vs_thread_epoll_benchmark.cpp`（三模型服务端 +
epoll 客户端 + 进程内编排器，`run` 子命令直接跑 5 场景 × 3 模型 × 5 次并打印中位数表）、
`benchmarks/build_gmp_vs_thread_epoll.sh`（不改 `cmake/Go2CppTests.cmake`，直接链接
build-quality 产物）。**未修改任何既有源文件**。

**关键结论**（WSL2、12 核、每格 5 次中位数）：

- GMP 线程数优势成立：1 万连接 18 线程 vs 每连接一线程 10001 线程（少 556 倍）；
  10k 连接 RSS 64.5MB vs 93.5MB（低 31%）。
- 但本机五个场景 GMP 的 QPS 全部落后：相对 thread 0.43–0.50x，相对 epoll+池
  0.24–0.48x；P99 在 s2/s4 高 7–8 倍；s1 短连接还有 13.8% connect 失败。
- 机制证据：纯 Fiber park/wake 0.73–0.80µs，但 Socket 等待往返 185–225µs；
  WSL2 跨线程 condvar 往返 54–56µs；IOManager 单 poller 串行唤醒 → 单事件场景
  上限 ~5–9k/s。反证：项目自带 `go2cpp_io_epoll_fair_benchmark`（批量就绪）
  同机 IOManager+Fiber 153k events/s vs thread-pool epoll 45k events/s。
- s1 的失败根因：GMP accept 在非调度线程串行执行，每连接还要 `manager.go` 入队，
  ~5k accept/s 上限。

**测试结果**：`ctest --test-dir build-quality` 113/113 通过（串行 67.9s）。
`ctest -j4` 时 `go2cpp_tests` 偶发一次失败（hook FIFO 时序），单测重跑与串行全量均通过。

**后续状态**：未在裸机 Linux 复测，因此 WSL2 对 GMP 的惩罚幅度未量化；
未扫描 P 数、`idle_wait`、fiber 栈大小等参数；s1 高并发短连接不适合当前单 accept
线程实现，未做多 accept 线程改造。

**补测修正（同日，二次测量）**：s4 原实现先把 `pending_op` 清零再判断方向，实际一直是
100% READ；修正后是真 50/50（4KB 上/4KB 下交替），表中 s4 已替换为修正后 5 次中位数。
补充实验：`--pin` 下 64 连接 13.6k QPS、单连接仅 295 QPS（延迟对放置敏感）；
`--depth 32` 流水线只把 s2 从 7.7k 提到 10.7k QPS（p50 184ms），说明“批量就绪”不能单独
解释与内置基准的差距；s4 的 epoll+池 3050 QPS 属未定位的 IO 线程路径异常，不作为模型
代表值。详见 `docs/gmp-vs-thread-epoll-2026-10-18.md` 第 8 节。


---

## 12. 2026-10-18 IO 场景吞吐优化（deepseek，第二轮）

**目标**：GMP 在 s1/s3 的 QPS 超过 thread+epoll（硬指标 12000）。

### 改动内容（库层）

1. **M 自己轮询 IO 后端（Go runtime.netpoll 模型）**
   - Scheduler::set_io_idle_poller(poll, wake) 新增；worker_loop 在无 G 可跑时
     直接在本 M 上 epoll_wait，就绪事件由同一线程处理并把 G 投回本 P。
   - IOManager::State::poll_once() 新增，与 poll_loop 共用后端与锁。
   - 启用 worker 轮询后不再启动独立 poller 线程（State::m_worker_polling）：
     两线程抢同一个 epoll 队列会把已消除的跨线程唤醒重新引入。
   - notify_worker()/notify_all_workers() 在 worker 正睡在 epoll_wait 时写
     eventfd（m_waiters 计数门控），避免「检查完队列、还没进 epoll」丢唤醒。
   - 仅在常驻 M 上启用（allow_idle_shrink、补充 M 的退休语义保持不变）。

2. **描述符生命周期门按 fd 分条**（DescriptorRegistryState，64 条带）
   - Hook 的 recv/send/fcntl/setsockopt 过去共用一把全局递归锁。WSL2 上一次
     竞争约 55-75us；短连接 accept 路径的两次 setsockopt 实测 168us，把连接
     建立压到 5k/s。分条后仍保证同一 fd 的 Capture/Invalidate/在途 syscall
     互斥（fd 复用安全性不变），不同 fd 不再互相阻塞。
   - DescriptorGuard 改为显式传 fd；hook.cpp 18 处调用点同步更新。

3. **Socket 构造不再走 Hook 版 fcntl**：init_nonblock() 先用原始 syscall 读
   F_GETFL，仅在确实需要置位 O_NONBLOCK 时才走 Hook 版（accept4 已带
   SOCK_NONBLOCK）。accept 路径的 FromFd 从 148us 降到 3us。

4. **IOManager::wait 把 TimePoint::max() 归一化为 nullopt**（无超时等待不再
   分配定时器节点）。

5. benchmark 增加 GO2CPP_BENCH_IO_SLEEP_US（默认 5000，与原行为一致），便于
   区分「IO 延迟」与「模型开销」；未改任何断言或场景定义。

### 关键文件

src/concurrency/scheduler.cpp、src/network/io.cpp、src/network/hook.cpp、
src/network/socket.cpp、include/go2cpp/network/io.hpp、
include/go2cpp/concurrency/scheduler/scheduler.hpp、tests/network/test_io.cpp、
benchmarks/gmp_vs_thread_epoll_benchmark.cpp

### 测试结果

- ctest --test-dir build-quality：113/113 通过（85.45s）。
- run --repeats 3 --scenarios s1,s3 --models gmp,thread,epoll（3 次中位数）：
  - s1 GMP：10976 QPS（基线 4383），P50 4.91ms，错误 295（基线 1302），线程 20。
  - s1 thread：10870 QPS；epoll：3025。
  - s3 GMP：5291 QPS（基线 3463），P50 617ms，线程 26，RSS 128.5MB。
  - s3 thread：9585 QPS；epoll：3027。
- 隔离复测（单场景、无背景负载）s1 GMP 曾达 12068 / 12464 QPS、P50 4.36-4.49ms。
- s2（长连接 echo，非验收场景）：7589 -> 22318 QPS（thread 22398，已持平）。
- strace -e trace=futex 计数：每请求 futex 21.8 -> 8.85（同一测量方法，strace 下）。

### 未达标与后续

- s1 未稳定达到 12000：中位数 10976，隔离复测 12068-12464；受机器负载影响方差大
  （thread 档同轮也出现过 1102 QPS）。
- s1 仍有约 295 个客户端错误（基线 1302）：表现为服务端在响应前关闭连接（客户端
  看到 EOF/HUP），未定位到具体分支。
- s3 的 12000 目标在协议上不可达：客户端 conns=10000、interval=1000ms，每连接每秒
  1 个请求，QPS 上限恒为 10000；thread 档 9585 已接近上限，GMP 5291。已知 GMP 在
  1 万连接下每请求 RTT 617ms，瓶颈是「空闲连接 100ms 唤醒切片」与 1 万 fd 下的
  就绪事件投递，未定位到根因。
- s3 RSS 128.5MB（目标 100MB 以下）：1 万 Fiber 栈被真实触及后常驻；基线 60.9MB
  是「栈未被触及」的低估。
- 未在裸机 Linux 复测；WSL2 的 futex/epoll 时延仍是主要噪声来源。
---

## 13. 2026-10-18 GMP 性能分析：定位「不如 thread+epoll」的真正原因（deepseek）

> 接 §12。§12 记录「改了什么」，本篇记录「为什么、还差多少、下一处在哪里」。

**改动内容**：补完上一轮未完成的 `DescriptorGuard` 按 fd 分条（`include/go2cpp/network/io.hpp`
/ `src/network/io.cpp`，`kStripeCount = 64`）+ `Socket::init_nonblock()` 走 raw `SYS_fcntl`
读标志；`tests/network/test_io.cpp` 两处无参构造改为显式 fd。**未改任何其它源文件。**

**A/B（只改 kStripeCount，1 vs 64）**：s1 4,573 QPS / 13.67ms → **11,800 QPS / 4.6ms**（2.6x）。
旧文档的 4,383 / 13.86ms 与单锁档在误差内一致 ⇒ 历史「GMP 慢 2.4 倍」的
**主因是描述符注册表全局锁**，不是调度模型。

**其它结论**（详见 `docs/gmp-perf-analysis.md`）：

- 基准 `process_request` 默认注入 `U(0,5ms)` 的 `usleep`，**所有 p50 都被它抬到 ~2.5ms**；
  `--pin` 的「单连接 3.37ms」与 pin 无关（pin 21,911/2.92ms vs 不 pin 22,146/2.90ms）。
- 关注入后 s2 64c：GMP **30,720** vs thread 29,078（+5.6%）⇒ **不是设计问题**。
- s1 的「错误」是 `EPOLLHUP`（errno=0），**不是 connect 失败**；先读 EPOLLIN 再判 HUP 可把
  646 → 17，但吞吐崩到 1/4，已回滚（基准侧语义需两端同改）。
- s3（1 万空闲连接）：吞吐只差 12%（8,505 vs 9,671），**p50 160ms vs 0.53ms** —— 1 万事件
  突发要在 12 个 worker 上排队，是协作式 + 固定 worker 数的结构性代价。
- Fiber 栈 64/128 KiB 无显著差异，RSS 不随栈变化；1 MiB × 1 万连接不可用。
- **下一处同类全局锁**：`IOManager::State::m_mutex` 的临界区内做 `epoll_ctl`
  （gdb 采样：61/112 个线程栈顶停在 `pthread_mutex_lock` / `futex_wait`）。

**测试结果**：`ctest --test-dir build-quality` → **113/113**（串行两次：66.96s、77.04s）。

**顺带修正的过期文档**：`docs/modules.md`（`os`「待接入」→ 已接入、`quic/` 只剩 picoquic 薄封装、删掉已不存在的 `hybrid_mutex.hpp`）与 `docs/NEXT-STEPS.md`（99 个测试/98 通过 → 113/113）。

---

## 14. 2026-10-18 拆掉每 IO 的两把全局锁：描述符表 + IOManager 状态按 fd 分片（deepseek）

> 接 §13 的「下一处同类全局锁」。

**改动内容**：

- src/network/hook.cpp：s_descriptors_mutex + 单张 s_descriptors 拆成 64 条带
  （DescriptorStripe，分片键为 fd 数值 fd & 63）。同一个 fd 永远落在同一条带，
  接管/关闭/dup2/在途 syscall 的互斥关系不变；clone_descriptor 里的
  DescriptorGuard::Capture 移到条带锁之外，保持「注册表条带锁 -> 描述符表条带锁」
  的固定嵌套顺序，避免与 adopt_socket 反向嵌套死锁。
- src/network/io.cpp：IOManager::State::m_slots 拆成 64 个 SlotStripe（alignas(64)），
  每条带自带锁与代际计数器；定时器表独立为 m_timer_mutex（锁序严格「条带 -> 定时器」），
  expire_timers 改为两阶段摘取；m_accepting / m_stop_poller / m_permanently_stopped
  原子化；process_events_batch 由「整批一把锁」改为「逐事件按 fd 加条带锁」；
  epoll_ctl 加 try_lock 自适应（抢到则串行避开内核 epoll 自旋，抢不到则并发直连）；
  erase_timer_locked 增加 timer_armed 快速路径。
- 未改任何测试与基准代码。

**单独测量（每把锁改完立刻压测）**：

| 阶段 | s3 QPS | s3 P50 |
|---|---|---|
| 基线 | 6,114 | 483ms |
| 仅 P0-1（描述符表分片） | 6,643（+8.7%） | 400ms |
| 再加 P0-2（IOManager 分片） | 9,848（+48%） | 3.50ms |

**最终（3 次取中位数）**：

| 场景 | GMP | thread | 结论 |
|---|---|---|---|
| s1（64 短连接） | 6,695 / 7.71ms | 10,350 / 4.76ms | 回归 -38% |
| s2（长连接 echo） | 20,452 / 3.13ms | 22,071 / 2.96ms | 小幅回归 -7% |
| s3（1 万连接） | 9,791 / 3.38ms | 9,661 / 2.84ms | 达标，超过 thread |

**s1 回归的二分定位**（同一份代码每次只改一处）：

| 配置 | s1 | s3 |
|---|---|---|
| 分片 + 并发 epoll_ctl | 6,937 | 9,848 / 3.50ms |
| 分片 + epoll_ctl 全串行 | 8,808 | 7,523 / 299ms |
| 分片 + try_lock（最终） | 6,695 | 9,791 / 3.38ms |
| 分片整体退回单条带 | 11,244 | 7,163 / 365ms |

已排除：条带数（64 改 16 无变化）、alignas(64) cacheline 隔离、定时器全局锁
（timer_armed 快速路径）、atomic 化。**残余差异只来自「事件处理是否并行」**：
并行时每请求 CPU 由 404us 升到 900us，而 context switch 不增，说明是调度器全局锁
与内核 epoll 的争用，不是用户态互斥量争用。s3 必须并行、s1 必须串行，二者直接冲突；
当前取舍为优先 s3（用户核心诉求）。

**测试结果**：ctest --test-dir build-quality -> 113/113（33.33s）。

**后续状态**：
- 待做：process_events_batch 的「小批量串行 / 大批量并行」自适应（需维护活跃 fd 计数），
  以同时保住 s1。
- 未做：EPOLLONESHOT 改 ET（每连接单 waiter 下 accept 循环不读到 EAGAIN 会丢事件，
  风险高）；cooperative_sleep_for 复用 eventfd。
- 语义：未发现破坏。同 fd 的 Capture/Invalidate/在途 syscall 互斥、fd 复用代际门、
  ONESHOT 语义均由 113 个测试覆盖通过。

## 15. 2026-10-19 新增 SO 热重载配置层：manifest 静态校验 + 两阶段事务（deepseek）

**改动内容**：

- 新增 `docs/so-reload-design.md`：分层结构、17 个场景逐条对应键/校验、文字版
  事务时序、ABI 接口约定、明确的不做项与已知限制。
- 新增 `include/go2cpp/config/so_manifest.hpp` + `src/config/so_manifest.cpp`：
  `[so]` / `[so.group.*]` / `[so.<name>]` 解析；文件存在、SHA-256（复用
  `go2cpp_hash`）、声明 ABI、依赖环、符号前缀冲突、版本化路径静态校验；
  不引入 `<dlfcn.h>`。
- 新增 `include/go2cpp/config/so_reloader.hpp` + `src/config/so_reloader.cpp`：
  `SoLoader`/`SoUnloader`/`SoActivator` 回调注入；revision 幂等与栅栏；
  变更集/A1 依赖一致性；`ApplyString` → `NotifyPending` →
  `RollbackPending` 两阶段；失败只卸载本次新加载的 SO；`keep_versions`
  保留清单与 `so.<name>.runtime.*` 观测键。
- 新增 `tests/config/test_so_manifest.cpp`（假 loader，不编译真实 `.so`），覆盖
  A1/A2/A3/B2/B3/D1/D2/D3/E1/F2，另加 C2 的 Notify→Activator 顺序断言。
- 新增 `docs/state-machines/SoReloader.md`（注释规范要求的状态转化表）。
- 改 `cmake/Go2CppModules.cmake`（`go2cpp_config` 增加两个源文件并链接
  `go2cpp_hash`）与 `cmake/Go2CppTests.cmake`（新增 `go2cpp_so_manifest_test`）。
- **未改** `include/go2cpp/config/config.hpp` 与 `src/config/config.cpp`。

**测试结果**：

- `cmake --build build-work -j8`：全量构建通过；新代码 `-Wall -Wextra -Wpedantic`
  无告警。
- `ctest --test-dir build-work -R so_manifest --output-on-failure`：1/1 通过
  （0.01s）。
- `ctest --test-dir build-work -R 'so_manifest|config_registry'`：2/2 通过。
- `ctest --test-dir build-work -R '^go2cpp_tests$'`：首次运行出现 2 个 hook
  时序失败（既有用例），立即重跑 1/1 通过；失败与本轮 config 改动无直接关联，
  但如实记录为波动项。

**后续状态**：

- 未实现：E3 的 `IN_Q_OVERFLOW` 全量重扫、F1 的灰度/进程隔离、F2 的成功/失败
  计数与耗时、F3 的跨进程测试；真实 `dlopen` loader 与 vtable 切换不在本层。
- 已知限制：已加载组/SO 从 manifest 消失会被拒绝（不在本层退役）；一次
  `Reload` 跨组全或全无；路径若含 `#`/`;` 会被 `IniFile` 截断。

## 16. 2026-10-19 SO 热重载收尾：F2 三指标 + E3 ForceRescan + 使用文档（deepseek）

**改动内容**：

- `include/go2cpp/config/so_reloader.hpp`：新增 `ForceRescan(const SoManifest&)`
  与 `ForceRescan()`；新增 `ReloadSuccessTotal()`/`ReloadFailureTotal()`/
  `LastReloadDurationMs()` 与三个 `so.runtime.*` 键常量；新增私有
  `ReloadAttempt` 计时/结果作用域、`ReloadImpl`、`FinishReload`、`PublishMetrics`、
  `EnsureMetricVars`。**未改** `Reload`、`CurrentManifest` 等既有公开签名。
- `src/config/so_reloader.cpp`：`Reload` 委托给 `ReloadImpl(manifest, false)`；
  `ForceRescan` 走同一实现并置 `force_rescan=true`，跳过 revision 幂等、仍拒绝
  回退/受 `highest_seen` 栅栏约束，把 manifest 全部 SO 加入变更集重新加载；
  所有返回路径经 `fail`/`MarkSuccess`/`MarkSkipped` 更新原子计数；用
  `std::chrono::steady_clock` 计时并在返回前把三个指标镜像到 `so.runtime.*`。
  三个指标都是 `ConfigVar<std::string>`（十进制文本，与既有 `so.runtime.*`
  一致），在构造时注册；数值访问器仍返回 `uint64_t`/`int64_t`。
- `tests/config/test_so_manifest.cpp`：新增 F2 指标场景（两次成功 + 一次 loader
  失败，断言 `success_total=2`、`failure_total=1`、`duration_ms>=0`，并验证幂等
  命中不改变计数）；新增 E3 场景（同 revision 普通 `Reload` 幂等，`ForceRescan`
  让两个 SO 重新加载，loader 调用 2→4→6）；E1 增加抢占计数断言。
- 新增 `docs/so-reload-usage.md`（528 行）：最小 manifest、`dlopen` 三回调、
  5 步发布流程、`so.runtime.*` 排障、限制与不适用场景。
- 更新 `docs/so-reload-design.md`、`docs/state-machines/SoReloader.md`、
  `docs/so-reload-progress.md`。
- **未改** `include/go2cpp/config/config.hpp` 与 `src/config/config.cpp`。

**测试结果**：

- `cmake --build build-work -j8`：全量构建通过，新代码无新增告警。
- `ctest --test-dir build-work -R so_manifest --output-on-failure`：1/1 通过。
- 测试二进制直接运行：`[F2 指标] success=2 failure=1 duration_ms=0`；
  `[E3] revision 未变仍全量重扫：loader 调用 2→4→6，版本 1→2`；
  `failures=0`。
- `ctest --test-dir build-work`：114/114 通过（61.75s）。

**后续状态**：

- 未做：真实 inotify 事件源、真实 `dlopen` loader/vtable 切换、F1 灰度/进程隔离、
  F3 跨进程测试、随机 fuzz/压测。
- 语义：`ForceRescan` 成功时 `applied=true` 即使 revision 未变；三个指标键为
  进程级固定名，多实例会互相覆盖，生产应只创建一个 `SoReloader`。
- 本轮未提交 git，留给主 agent 审查。

## 17. 2026-10-19 Channel::Select 增加 DoneCase / AfterCase（deepseek）

**改动内容**：

- `include/go2cpp/concurrency/channel.hpp`（+222 行，只新增）：
  - 新增公共 API `DoneCase(const ContextPtr&)`（等价 Go `case <-ctx.Done()`）与
    `AfterCase(time_point)` / `AfterCase(ContextDuration)`（等价
    `case <-time.After(d)`），插在 `DefaultCase()` 之后；**未改** `Select` /
    `RecvCase` / `SendCase` / `DefaultCase` 的既有签名。
  - 新增 `detail::SelectCaseRegistry`：以 `SelectWaitState*` 为键保存 Done 回调 id
    与 `std::shared_ptr<core::Timer>`。不用 case 内单槽位，是因为 `Select` 在没有
    分支获胜时会重建等待状态并**重新 arm**（同一个 `wait_state` 指针被复用），
    且同一个 `SelectCase` 对象可能被多线程共享；单槽会互相覆盖，导致 disarm 删错
    id、回调永久留在 `Done` 上（每次 select 泄漏一个回调）。
  - `DoneCase`：probe 看 `ctx->IsDone()`（`status=kCancelled`，
    `error = ctx->Err() ?: CanceledError()`）；arm 用 `ctx->Done().AddCallback(...)`，
    回调里 `wait_state->TrySelect(index, probe)`；disarm 取回 id 后
    `RemoveCallback`。
  - `AfterCase`：probe 比较 `steady_clock::now() >= deadline`（命中 `ok=true`、
    `status=kReady`）；arm 用 `core::TimerService::Default().Schedule(deadline, cb)`，
    `Timer` 必须由 `std::shared_ptr` 持有（局部 `Timer` 在 arm 返回时析构，析构等价
    于 `Cancel`，回调将永不触发）；disarm `timer->Cancel()`。
  - 新增 include：`go2cpp/core/timer.hpp`、`<unordered_map>`。
- `tests/concurrency/test_channel.cpp`（在原文件内追加，未新建文件，+540 行）：
  - T1~T11 场景测试 + B1~B4 大数据量测试 + R1 随机事件源回归；
  - 泄漏断言用两种可核验手段：`core::TimerService::Default().PendingCount()`
    （定时器是否真的撤销）与"取消后保留的 `wait_state` 不得被回调改写"
    （T9(b)：漏 `RemoveCallback` 时回调会触发，直接把状态标成已选中）；
  - 所有大数据量用例打印 `[perf]` 实际耗时。

**测试结果**（build-work，Release，实际运行输出）：

- `cmake --build build-work -j8`：通过（增量 12.2s）。
- `GO2CPP_TEST_FILTER=channel ./build-work/go2cpp_tests`：全部通过，`[perf]` 输出见下表。
- `ctest --test-dir build-work -R '^go2cpp_tests$' --output-on-failure`：1/1 通过
  （27.3s；改动前的同项运行 30.9s）。
- 收尾时把 T6 的"等待 arm 完成"从固定 `sleep_for(5ms)` 改成轮询
  `PendingCount()`（`GO2CPP_REQUIRE_EVENTUALLY`），避免慢机器/插桩环境下的假失败；
  下表是这次改动后的最终一次运行。
- `ctest --test-dir build-work`：**首次** 1/114 失败——`go2cpp_tests` 在
  `tests/network/test_io.cpp:736/738`（`many_result.ready_indices.size()==2`、
  `ready_indices[1]==1`）失败；单独重跑 `-R '^go2cpp_tests$'` 通过，再跑全量
  **114/114 通过**（60.1s）。
  该断言在 `GO2CPP_TEST_FILTER=io` 单独跑 10 次里也失败 2 次（完全不经过 channel
  模块），判定为既有偶发（IO 多路等待"就绪批"的时序），与本轮改动无关。
- 变异验证（确认测试真的能抓到泄漏，验证后已还原）：
  - 去掉 `DoneCase` disarm 的 `RemoveCallback` → T9(b) `fired == 0` 失败；
  - 去掉 `AfterCase` disarm 的 `Timer::Cancel`（且不释放句柄）→ T6
    (`PendingCount() <= baseline`) 与 T10 (`disarmed_visible`) 失败。
  说明两条泄漏断言都不是"恒真断言"。

大数据量/计时实测（同一次运行，单位为 ms）：

| 用例 | 规模 | 实测 |
|---|---|---|
| T1 DoneCase(已取消 ctx) | 1 次 | 0.003 |
| T4 AfterCase(30ms) | 1 次 | 30.18（≈ deadline） |
| T5 AfterCase(40ms) vs Recv | 1 次 | 40.18 |
| T7(c) 三合一 timer 先到 | 1 次 | 25.17（deadline 25ms） |
| T8 Done+Recv+Default 全走 default | 1000 次 | 0.28 |
| T9(a) Select(已取消 DoneCase) | 10000 次 | 1.22 |
| T9(b) DoneCase arm/disarm | 10000 次 | 1.14 |
| T10 AfterCase arm+立即 disarm | 10000 次 | 5.47 |
| B1 Send+Select(Recv+Default) | 100 万次 | 265.63 |
| B2 1000 个 RecvCase（第 1000 个有数据） | 1000 cases | 0.19 |
| B3 AfterCase 创建+立即 disarm | 10 万次 | 58.73 |
| B4 8 线程 × Select(Recv+Done) + 阻塞取消 | 8×10000 + 8×100 | 50.78 |
| R1 随机事件源 | 3000 轮 | 4.11 |

**后续状态**：

- 语义：`DoneCase` 与"已就绪 channel"同时可选中时不做 context 优先（由 `Select`
  的轮转探测决定赢家，与 Go 的随机选择一致）；T3 覆盖的是"先挂起、数据先到、
  之后才 cancel"的路径。
- 已知限制：`DoneCase(nullptr)` 永不就绪（对齐 Go 的 nil channel），只有它一个
  分支的 `Select` 会永久等待，调用方需自己保证非空。
- 未做：Fiber（受管 G）里 `DoneCase`/`AfterCase` 的专项用例；本轮覆盖的是普通
  线程 + `Select` 的既有等待路径（`Select` 本身对受管 G 的支持未改动）。
- 仓库状态：本轮**没有执行任何 git 提交**；但另一个进程在 15:20 用
  `feat(config): SO 热重载收尾...`（58b31d4）把工作区里的 `channel.hpp` 与
  `test_channel.cpp` 一并提交了，主 agent 审查/提交时注意这一点。

## 18. 2026-10-19 Fiber 可变长栈：8MiB 预留 + 2KiB 初始提交 + SIGSEGV 按需增长（deepseek）

**修改时间**：2026-10-19 21:30（Asia/Shanghai）

**改动内容**：

- 配置层：
  - `SchedulerConfig::fiber_stack_size` 默认 `0` → `2048`；
  - 新增 `fiber_stack_reserve_size = 8MiB`、`fiber_stack_growable = true`；
  - `TaskOptions` 末尾新增 `stack_reserve_size` / `stack_growable`（保持既有
    `TaskOptions{a, b}` 聚合初始化语义）；
  - `Scheduler::spawn` 用配置补全 reserve/growable，`Task::run` 传给
    `Fiber::AcquireForScheduler`。
- `Fiber` 栈布局（`src/concurrency/fiber.cpp`）：
  - growable：`mmap(reserve, PROT_NONE, MAP_NORESERVE)`，`top = base + reserve`，
    `committed = top - initial_usable`，只把顶部 `initial_usable` 改 RW；
  - 归一化 `initial_usable = max(page_align_up(requested), 1 页)`；
    默认请求 2048 → 实际 4096（1 页）；
  - reserve == initial（或只多出不足 64KiB）、`GO2CPP_GROWABLE_STACK=0`、
    非 Linux/x86_64、非原生上下文、ASAN/TSAN 时退化为固定栈；固定栈保留旧布局
    （usable + 1 页 PROT_NONE guard），且不安装 handler；
  - 池化键 `(normalized usable, reserve)`；释放增长过的栈前用 `mprotect(PROT_NONE)`
    把 `[top-initial, committed)` 重置，并把 committed 重置回 `top-initial`，
    防止复用后溢出静默写坏已提交页。
- SIGSEGV 增长：
  - 每线程 `sigaltstack`（256KiB，mmap），进程级 `sigaction`：
    `SA_SIGINFO | SA_ONSTACK`，不设 `SA_NODEFER`；
  - `thread_local FiberStack* t_current_growable_stack` 在
    `resume_locked` / `entry` / `finish_switch_to_fiber` / `suspend` 中维护；
    迁移到新 M 时在 resume 前补装 altstack；
  - handler 判定：重入保护、`REG_ERR & 0x10` 取指故障、`si_addr ∈ [base, committed)`、
    `rsp ∈ [base, top]`、`|rsp - si_addr| ≤ 256KiB`、`committed - base ≥ 64KiB`，
    然后 `new_low = align_down(min(si_addr, rsp)) - 64KiB`（clamp 到 base），
    `mprotect(new_low, committed-new_low, RW)`，成功后更新原子 committed 并
    **直接 return**；
  - 有意偏离设计稿的一点：设计稿要求 `rsp ∈ [committed, top]`，实测函数序言
    `sub rsp` 会先越过 committed（probe: fault=committed-8, rsp=committed-0x50），
    因此放宽为 `rsp ∈ [base, top]` 且用 256KiB 窗口约束；提交范围也改为从
    `min(si_addr, rsp)` 向下，以覆盖大帧；
  - fatal 路径：恢复 SIG_DFL、解除屏蔽、`raise(sig)`、`_exit(128+sig)`；
    只使用 async-signal-safe 调用；增长次数用原子计数，不打印日志。
- 配置兼容：`src/config/config.cpp` 的 `kMinFiberStack` 16384 → 2048，
  `go2cpp.ini` 的 `fiber_stack_size` 0 → 2048。
- 测试：
  - 新增 `tests/concurrency/test_fiber_growable_stack.cpp`（273 行）并注册到
    `go2cpp_tests` 与独立 ctest `go2cpp_fiber_growable_stack`；
  - `test_fiber.cpp` 的初始栈下限断言从 2 页改为 1 页（新归一化语义）；
  - `example/concurrency/fiber_stack_limit_full.cpp` 在 main 开头显式设置
    `GO2CPP_GROWABLE_STACK=0`，保留其固定栈临界深度/爆栈实验的可复现性。

**关键文件**：

- `src/concurrency/fiber.cpp`（+614/−82）
- `include/go2cpp/concurrency/fiber.hpp`、`include/go2cpp/concurrency/scheduler/scheduler.hpp`
- `src/concurrency/scheduler.cpp`、`src/config/config.cpp`、`go2cpp.ini`
- `tests/concurrency/test_fiber_growable_stack.cpp`、`tests/concurrency/test_fiber.cpp`、
  `tests/test_main.cpp`、`cmake/Go2CppTests.cmake`
- `example/concurrency/fiber_stack_limit_full.cpp`

**测试结果**（build-work，Release，实际运行）：

- `ctest --test-dir build-work --output-on-failure`：**116/116 通过，63.81s**
  （115 个既有测试 + 新增 `go2cpp_fiber_growable_stack`；既有测试全绿）。
- 新增专项（`GO2CPP_TEST_FILTER=fiber_growable`）：
  - 256KiB-class：frames=1024，checksum=68616（expected 68616）
  - 1MiB-class：frames=4096，checksum=323616（expected 323616）
  - 深递归返回后外层局部：outer=0x123456789ABCDEF0，地址不变
  - 迁移后增长：checksum=68616
  - 4 workers × 32 tasks 并发增长：completed=32
  - `GO2CPP_GROWABLE_STACK=0` / reserve==initial：4KiB 固定栈 Fiber 正常完成，
    恢复 env 后增长路径不复用固定栈缓存
- 增长证据：
  - 1000 个挂起 Fiber：VmSize 6428 kB → 8199212 kB（+8192784 kB ≈ 8GiB，
    即每个 8MiB 预留）；VmRSS 仅 +4864 kB（每 Fiber 初始 1 页 4KiB）；
  - 单个深递归 Fiber：VmRSS +1152 kB，stack_size=4096；
  - `strace -e mprotect` 单 Fiber：初始 `mprotect(...,4096,RW)`，每次 SIGSEGV
    `mprotect(...,69632,RW)`（64KiB+故障页），完成后
    `mprotect(...,1183744,PROT_NONE)` 重置。
- 性能（fair_bench，同机）：
  - A（200×2 depth1，12 workers，10s）：改动前 qps=132804/cpu=71.3µs/RSS=16484kB；
    改动后 qps=146622/cpu=70.4µs/RSS=16752kB；同代码 fixed 回退
    qps=143913/cpu=71.1µs/RSS=16736kB。
  - s3（8000 rate1，GMP P=4，12s）：改动前 qps=11239/cpu=182.5µs/RSS=221768kB
    （3 errors）；改动后两次 qps=10807/10869，cpu=178.7/174.9µs，
    RSS=221332/224180kB，completed=138126/140054（均高于改动前 136020）；
    同代码 fixed 回退 qps=11532/cpu=175.5µs/RSS=218856kB。s3 的 qps 受客户端
    rate/采样 span 影响大，以 completed 与 cpu µs/req 看无回退。

**后续状态 / 已知限制**：

- 直接 `Fiber(fn)` 的 `Fiber::DefaultStackSize()` 仍为 128KiB（为兼容既有示例与
  测试）；调度器默认走 `SchedulerConfig` 的 2048，对应 1 页初始提交。
- handler 是进程级覆盖 SIGSEGV；宿主若已有自定义 SIGSEGV handler 会被替换。
  fatal 路径按设计恢复 SIG_DFL。
- 大帧 >256KiB（或 `alloca`/VLA 远超窗口）按设计 fatal；初始只有 1 页，
  函数序言的大帧依赖 64KiB 增长块与 256KiB 窗口。
- `codex.md` 仅追加本记录；本轮**没有 git commit / push**。
- 未改 `docs/design.md`、`docs/boundary_audit.md` 中“没有自动扩容”的旧描述，
  后续如需要可单独更新文档。

## 19. 2026-10-19 性能优化轮次：fiber cache 扩容 + 批量发送（deepseek）

### 19.1 改动

1. **fiber/栈池扩容**（`src/concurrency/fiber.cpp`、`scheduler.hpp:240`）：
   thread-local 栈池 8 → 32、全局栈池 128 → 512、`fiber_bin_capacity` 32 → 128。
2. **GMP benchmark 批量发送**（`benchmarks/gmp_vs_epoll_fair_benchmark.cpp`）：
   `serve_gmp_connection` 一次 recv 解析出的所有帧合并为一次 `send_all`，
   `out.reserve(16KB)`；此前每帧一次 `sendmsg`。

### 19.2 数据（同机、同窗口）

- **s1 短连接**（旧 benchmark，2 clients）：22,606 → **33,058 QPS（+46%）**、
  cpu 248.7 → 216.9 µs/req、p99 40.8 → **5.6ms**、VmSize 84GB → **8.2GB**。
- **B pipeline depth8**：168,548 → **189,159 QPS（+12.2%）**、cpu 62.6 → 58.2、
  sys 8.5 → **4.7**、`sendmsg` 1.2 → **0.33 次/请求**。
- **A depth1**：142,491 → 141,968（噪声内）、cpu 70.3 → 70.7。
- **s3 8000 rate1**：7,983 → 7,983、cpu 121.9 → 123.6、RSS 162 → 190MB（out 缓冲触碰页）。

### 19.3 结论与回退

- 池容量继续加倍（64/1024/256）无收益（s1 32.1k），**已回退到 32/512/128**；
- 批量发送是本轮最大单点收益（pipeline +12~14%、sys −47%），但只改了 benchmark
  应用层；库侧 `Socket` 的写合并需要发送缓冲与 flush 策略，风险更高，未做；
- `ctest` **116/116** 通过；
- 未 commit / push；主目录未改动。

## 20. 2026-10-20 四线集成联调（deepseek）

### 20.1 改动

1. **修复 `GO2CPP_DEBUG_DUMP=ON` 全量构建失败**：`EventRing::Record` 的实现原本
   只在 `go2cpp_debug` 中，而 scheduler/io 在 ON 构建直接调用它；反向链接会与
   `go2cpp_debug -> scheduler` 构成目标依赖环。新增零依赖库
   `go2cpp_debug_event` 承载 `src/debug/debug_event_ring.cpp`，`go2cpp_debug`
   与（仅 ON 构建的）scheduler/io 链接它。未改 `src/**`。
2. **CLI 迁移协议对齐 v2**：`G2CMIG01` → `G2CMIG02`，新增 `version=2` 校验
   （v1 与 version≠2 均退出 4）；帮助、文档、fixture、断言同步更新。
3. **新增可复现集成测试**：
   `tools/cli/tests/integration_e2e.sh`（44 断言）、
   `tools/cli/tests/debug_longrun_demo.cpp`、
   `tools/cli/tests/debug_socket_probe.py`；
   详细报告 `docs/integration-test.md`。

### 20.2 关键文件

- `cmake/Go2CppModules.cmake`
- `tools/cli/formats.hpp`、`tools/cli/formats.cpp`、`tools/cli/commands.cpp`、
  `tools/cli/help.cpp`
- `tools/cli/tests/make_binaries.py`、`tools/cli/tests/run_all.sh`
- `tools/cli/tests/integration_e2e.sh`（新增）、
  `tools/cli/tests/debug_longrun_demo.cpp`（新增）、
  `tools/cli/tests/debug_socket_probe.py`（新增）
- `docs/cli.md`、`docs/integration-test.md`（新增）

### 20.3 测试结果（实际运行）

- `bash tools/cli/tests/integration_e2e.sh`：**44 PASS / 0 FAIL**
  （Debug 真实 socket、迁移 dump/verify/restore、CLI migrate、重放 record/replay、
  CLI replay、丢唤醒复现）。
- `ctest --test-dir build-integration --output-on-failure -j6`：
  **116/116 通过，26.84s**（ON 构建）。
- `GO2CPP_DEBUG_DUMP=OFF` 独立构建 `go2cpp_tests`：成功。
- `BIN=build-integration/tools/cli/go2cpp-cli bash tools/cli/tests/run_all.sh`：
  **PASS=202 FAIL=0**。
- `bash tools/migration/tests/run_tests.sh build-migration`：**23/23 通过**。
- `bash tools/replay/scripts/stress_record_replay.sh build-replay 10`：
  **10/10 轮 MATCH**；`fault_injection.sh build-replay`：两种注入均 stuck。

### 20.4 后续状态 / 已知限制

- 本轮所有改动**未 commit**。
- `migrate dump --pid/--gid` 仍是 demo 能力（后端只写自身进程 A 的快照），
  CLI 已明确提示，未伪造按 pid/gid 抓取。
- CLI `replay play` 调用离线 `tools/replay/replay`，报告
  `replay_executed=false`；完整 Fiber 重放由 `go2cpp_replay_replay` 覆盖。
- OFF 构建只验证了编译，未重跑完整 ctest；ON 构建 116/116 已完整通过。

## 21. 2026-10-20 在线诊断 Doctor + 受控解锁（deepseek）

### 21.1 改动

1. **新增 Doctor（单线程命令平面）**：
   - `include/go2cpp/debug/diagnostic_protocol.hpp`：G2CD/1 帧协议
     （24 字节头：magic/version/kind/flags/stream_id/seq/len/FNV-1a 校验），
     header-only，CLI 与 topic 项目不需要链接运行时；
   - `include/go2cpp/debug/diagnostic_agent.hpp` +
     `src/debug/diagnostic_agent.cpp`：接收线程（只做 I/O）+ FIFO 队列 +
     唯一执行线程；所有命令串行执行，`stats.concurrent_peak` 恒为 1；
   - 可插拔 `DiagnosticTransport`：Unix socket / 内存实现；接口按
     `Recv(envelope, timeout)` / `Send(envelope)` 定义，`peer` 与
     `stream_id` 解耦，供 QUIC / 用户 topic 字节流接入。
2. **命令集**：`status`（协程/M-P/runqueue/锁等待/模块基址+build-id）、
   `dump`（复用 DebugServer 组件）、`force-wake`（四个白名单 scope）、
   `deadlock-report`（资源级等待图）、`mutex-unlock`（仅 owner 已终止）、
   `inject-fiber`（诊断 Fiber 内执行）、`stats`/`ping`/`help`。
3. **安全白名单**：force-wake 只作用于 scheduler-idle / ParkingCondition /
   Channel；`scope=mutex` 明确 refused（FIFO 所有权交接不允许 spurious wake）。
4. **诊断 Fiber**：`InstallDiagnosticFiber` 注入常驻受管 G，park 在私有
   ParkingCondition；命令期间 `notify` + `Scheduler::ForceWakeIdle()` 唤醒，
   在 GMP 上下文执行 `traverse`（gid/processor/machine/tasks）。
5. **死锁检测**：执行线程每 500ms 采样进展指纹（runnable/metrics/前 512 G/
   M 状态/等待者），超阈值记录 `kDiagnosticSuspected` 事件；默认只报告，
   `auto_force_wake` 默认关闭。
6. **最小受控唤醒接口（不改默认行为）**：
   - `Scheduler::ForceWakeIdle()`：仅转发既有 `notify_all_workers()`；
   - `ParkingCondition` 等待者快照增加 `gid`（只读暴露）；
   - `Channel<T>::ForceWakeWaiters()`：只发 spurious wake，不写结果位、
     不关闭、不递增 generation；
   - 事件码追加 `kDiagnosticSuspected=60` / `kDiagnosticForceWake=61` /
     `kDiagnosticMutexUnlock=62`。
7. **CLI**：`go2cpp-cli doctor`（Unix socket；`--concurrency` 并发探针；
   `--transport quic` 明确返回「尚未实现」而不是静默退回）。
8. **编译开关**：`GO2CPP_DIAGNOSTIC`（默认 OFF）
   + `GO2CPP_DIAGNOSTIC_QUIC`（默认 OFF，依赖 picoquic）。
9. **文档**：`docs/diagnostic-channel.md`（协议/白名单/等待图/QUIC stream
   映射/用户 topic 接入/限制）；`docs/cli.md` 增 4.8 节。

### 21.2 关键文件

新增：
`include/go2cpp/debug/diagnostic_protocol.hpp`、
`include/go2cpp/debug/diagnostic_agent.hpp`、
`src/debug/diagnostic_agent.cpp`、
`docs/diagnostic-channel.md`、
`tests/debug/test_diagnostic.cpp`、
`tests/debug/diagnostic_deadlock_demo.cpp`、
`tests/debug/doctor_client.py`、
`tests/debug/run_diagnostic_e2e.sh`、
`tools/cli/doctor.hpp`、`tools/cli/doctor.cpp`。

修改：`include/go2cpp/concurrency/scheduler/scheduler.hpp`、
`src/concurrency/scheduler.cpp`、`include/go2cpp/concurrency/core/parking_condition.hpp`、
`src/concurrency/parking_condition.cpp`、`include/go2cpp/concurrency/channel.hpp`、
`include/go2cpp/debug/debug_event_ring.hpp`、`cmake/Go2CppOptions.cmake`、
`cmake/Go2CppModules.cmake`、`cmake/Go2CppTests.cmake`、`tests/test_main.cpp`、
`tools/cli/{main,help,CMakeLists}`、`docs/cli.md`。

### 21.3 测试结果（实际运行）

- `GO2CPP_TEST_FILTER=diagnostic ./build-diag/go2cpp_tests`：
  **11 个场景 + 200 次往返压测 + 1000 次随机帧解析，all checks passed**；
- `bash tests/debug/run_diagnostic_e2e.sh build-diag`：**14/14 PASS**
  （卡死期间 status/dump、build-id、deadlock 判定、等待图、mutex scope
  refused、force-wake 恢复 `RECOVERED ready=1`、并发 10、concurrent_peak=1、
  死锁事件计数）；
- `ctest --test-dir build-integration -j4`（Doctor OFF 回归）：
  **116/116 通过，26.21s**；
- `build-diag`（Doctor ON，examples OFF）：**41/41 通过，26.75s**
  （#1 `go2cpp_tests` 内含诊断模块，未新增 ctest 用例）；
- `build-doctor-full`（Doctor ON，完整配置：examples/CLI 全开）：
  **116/116 通过，26.67s**；
- OFF 构建（`GO2CPP_DEBUG_DUMP=OFF`、`GO2CPP_DIAGNOSTIC=OFF`）：
  `nm -C libgo2cpp_debug.so | grep -c DiagnosticAgent` = **0**，
  且 `go2cpp_debug` 只编译 `debug_dump.cpp` / `debug_server.cpp`。

### 21.4 后续状态 / 已知限制

- 所有改动**未 commit**；
- QUIC：协议/接口/开关已落地，picoquic 服务端/客户端未实现（路线见
  `docs/diagnostic-channel.md` §9）；
- 执行线程可能卡在运行时内部锁（原生死锁）上，此时 Doctor 也会挂起；
- 等待图为资源级；Mutex 的等待者没有 gid，owner 是近似值；
- `mutex-unlock` 依赖持锁 park 检测（Debug 默认开，Release 默认关）；
- 诊断 Fiber 与业务共享 P 的 runqueue（命令期间短暂占用）；
- 帧协议无身份认证，跨机必须由 topic/QUIC 层提供 mTLS。

## 22. 可迁移协程：生产版 Fiber / Channel adopt API（2026-10-20）

### 22.1 改动

- 新增编译开关 `GO2CPP_MIGRATION_API`（默认 OFF，`cmake/Go2CppOptions.cmake`）；
  OFF 时迁移实现不参与编译、`Channel<T>` 无额外基类/虚表，默认构建 116 项与
  热路径不变；
- `include/go2cpp/concurrency/fiber.hpp`：`ChannelWaitState`、
  `ChannelMigrationBase`、`FiberMigrationOptions`、`Fiber::EnableMigration` /
  `migration_enabled`、`ExportMigrationImage` / `ImportMigrationImage`、
  内部 `BeginMigrationWait` / `EndMigrationWait`；
- `include/go2cpp/concurrency/channel.hpp`：迁移等待路径（不入
  `ParkingCondition`，登记节点后直接 `Fiber::Suspend`）、`ExportWaitState` /
  `AdoptWaitState`、`MigrationObjectAddress/Size/NodeSize`、
  `ReleaseMigrationHold`、noinline 等待节点辅助函数；
- `src/concurrency/fiber.cpp`：导出/导入实现（G2CMIG03 快照、栈槽扫描、
  栈/句柄/模块重定位、build-id、HMAC 签名、指针审计、`ClearedHeapSlot` 兜底）；
- `cmake/Go2CppModules.cmake` + 新增 `cmake/Go2CppMigration.cmake`：
  私有链接 `go2cpp_migration_format`（复用 `tools/migration` 格式层）、
  ON 构建全模块关闭 stack protector、新增 demo 与反例可执行文件；
- `tools/migration/adopt_demo.cpp`：跨进程 dump/restore/selftest；
- `docs/fiber-migration.md`：§11 标题与落地说明更新，新增 §11.6–§11.8；
- 报告：`/tmp/adopt_api.md`。

### 22.2 验证（实际运行）

- `./go2cpp_fiber_adopt_demo dump /tmp/fiber-adopt.mig` →
  `restore`：输出「fiber 继续执行，收到消息 = 42」「state=Completed」；
- `selftest` 8 场景全过：正常闭环、100 次随机值重复恢复
  （28.3ms / 283us 每次）、篡改快照拒绝、无密钥导出拒绝、多等待者拒绝、
  未登记堆指针拒绝、已关闭通道拒绝、其他 build-id 拒绝；
- `ctest --test-dir build-work -j2`：**116/116 通过**（30.23s）；
- `tools/migration/tests/run_tests.sh build-migration`：**53/53 通过**；
- OFF 构建符号检查：`libgo2cpp_fiber.so` / `libgo2cpp_channel.so` 中
  `ExportMigrationImage` / `ChannelMigrationBase` 符号计数均为 0。

### 22.3 后续状态 / 已知限制

- 所有改动**未 commit**；
- 只支持无缓冲、单生产者单消费者、无 select 的接收等待（`<-ch`）；
- 迁移等待路径不进入 Scheduler park 协议，唤醒由宿主显式 `resume()`；
- 迁移构建关闭 stack protector（canary 不可跨进程）；
- 真实等待路径会留下容器内部死值，默认拒绝导出；受控 demo 用
  `allow_dead_heap_slots` 清零，不是安全边界；
- 未验证：跨机器/跨架构、调度器自动接入、异常展开中的栈、持锁挂起点、
  select/发送等待。

## 23. Timer 调试内省与回放（2026-10-20）

**能力**
- `TimerService::DebugSnapshot()`：活动数 / 堆顶 deadline / 按来源分类 / fired-cancelled-rescheduled 统计 / 后台线程状态；
  `DumpTimer` 输出 text+JSON 并带 `seq_at_start/seq_at_end`；`RegisterTimer` 接入 DebugServer。
- 事件环新增 `kTimerArm/Fire/Cancel/Reschedule`（90~93）：`gid`=timer_id、`arg2`=deadline_ns、
  `arg3`=kind、`arg4`=source_gid。文本事件行保持 7 列不变，JSON 与 `DUMP timer` 给出语义字段视图。
- `tools/replay` 格式 v3：`kTimer` 尾部追加 `timer_id/timer_source/deadline_ns/fired_at_ns/callback_result`；
  `ReplayTimerService` 支持 live 录制与 replay 虚拟时间驱动；引擎按 `kTimerFire` 派发回调并校验 fire checksum。

**约束 / 陷阱**
- 来源分类依赖线程本地 `TimerSourceScope`；本次边界不允许改 context/parking_condition/channel/timed_mutex，
  运行时这些使用方未标注 → `kUnknown`（`ScheduleOn/EveryOn` 已自动归 `kScheduler`）。
- replay 的 timer 回调在引擎线程执行，不能 park；live 周期是固定延迟；
  “正在执行的回调不可取消”（Cancel 返回 false）是有意取舍，避免记录出现 cancel 在 fire 前的假顺序。
- timer 事件走事件环普通区：10k 级 arm 不会挤掉关键区 wake 线索，但会被后续事件覆盖，
  `timer_events.count` 只表示“最近可见条数”。

**验证**
- `ctest --test-dir build-work` → 116/116；`ctest --test-dir build-debug-full` → 116/116。
- 回放 MATCH：same-deadline / periodic(≥10 次) / cancel / random-1000 / stress10k；
  独立 `tools/replay` 工程构建与单测通过。
- 10k 压测：core TimerService schedule p99≈12.8µs、cancel p99≈3.0µs；
  replay live 驱动 schedule p99≈30.9µs、20k trace 回放≈74ms。
- 报告：`/tmp/timer_debug_replay.md`；demo：`/tmp/timer_dump_demo.txt`、`/tmp/timer_replay_demo.txt`。

## 24. 2026-10-20 真实场景死锁闭环：100+ HTTP client → 锁顺序死锁 → Doctor 解锁（deepseek）

**新增能力**
- `tests/scenarios/deadlock_routes/`：真实 HTTP 服务（`http::Server` + 每连接一个 Fiber）+
  122 个并发 client + 两把 `sync::Mutex` 的 A→B / B→A 锁顺序反转。两个触发 client
  在 `ParkingCondition` 上会合后各持一把锁再抢第二把 → 确定性死锁；追加 8 个单锁
  请求排队 → 卡住 10 个请求。
- 观测：`go2cpp-cli dump --sections scheduler,fiber,mutex,events` 抓到
  gid/state/waiting_on/owner/waiters 与 park/mutex 事件；场景注册了带注解的
  `fiber`/`mutex` 组件（schema `go2cpp.scenario.*`），不伪造库内 schema。
- Doctor `mutex-unlock --force reason=<text>`：默认拒绝、缺 reason 拒绝；执行
  `Mutex::Unlock()`，`stats().forced_unlocks` 在 Unlock **之前**自增，响应带
  audit 字段并在 stderr 打审计行。文档：`docs/diagnostic-channel.md` §4.5.1。

**实测数据（build-doctor-full, Release -O3, 12 vCPU WSL2）**
- 122 client / 1500ms：4968 请求，3008 QPS，成功率 95.45%，
  p50 0.37ms / p95 0.64ms / p99 8.63ms；失败为短连接 connect(179)/receive(48)，
  与死锁无关，`FINAL_ERRORS` 如实输出。
- 死锁：gid 4794(res-a) ↔ 4795(res-b)，A/B 各 5 waiters，卡住 10；
  `deadlock-report` verdict=deadlock @663ms，两条 mutex 边 owner_alive=true；
  `force-wake all` → woken_total=4（只唤醒 idle worker），waiters/verdict 不变；
  `mutex-unlock name=res-a` → refused owner-alive；
  `--force reason=...` → verdict=forced，waiters 5→4、held_after=false；
  恢复后 stuck 10→0，10 个请求全部 200，`stale-unlock-skip` 1 次。
- 触发率 `--repro 10` → 10/10；恢复 10/10。

**约束 / 陷阱**
- force 解锁后 owner 不知道锁已丢：业务代码必须自己识别（本场景比较
  `forced_unlocks` 计数 + Mutex owner，只释放仍属于自己的锁，记
  `stale-unlock-skip`），否则会出现 `unlock of unlocked mutex` 或抢锁；
  `--force` 仅限故障处置/demo，不能作为常规运维手段。
- 加载期不能让 client 走交叉锁路由：否则压测中随时可能出现计划外自然死锁，
  触发率退化成随机事件。交叉锁路由只由 2 个触发 client 在固定时序下并发访问。
- `deadlock-report` 需要全局无进展超过阈值（本场景 600ms）才从 `stalled` 变
  `deadlock`；刚卡住就查会得到 `verdict=stalled`。
- Release 构建默认关闭持锁 park 检测，场景必须显式
  `SetHeldLockParkDetection(true)`，否则 owner_gid 不可知，`mutex-unlock` 一律 refused。

**验证**
- `bash tests/scenarios/deadlock_routes/run_deadlock_scenario.sh build-doctor-full`
  → 17 PASS / 0 FAIL；`--repro 10` → 触发率 10/10、恢复 10/10；
- `ctest --test-dir build-doctor-full` → **115/116**：唯一失败是
  `test_diagnostic` 的 owner-gone 用例，把本任务改动还原到 HEAD 后同样失败，
  属既有回归（`DebugSnapshotTasks()` 过滤终态 G → `mutex-unlock` 看不到已终止
  owner），不是本场景引入；详见 `/tmp/deadlock_scenario.md` §11.3；
- `cmake --build build-work` 全量通过（默认开关零影响）。
- 报告：`/tmp/deadlock_scenario.md`；dump：`/tmp/scenario-deadlock-dump.txt`。

## 25. 2026-10-20 Context 取消 + 回滚解死锁：Doctor `ctx.cancel` + 场景（deepseek）

- 需求：A 等锁被取消 → A 回滚释放已持锁 → B 继续。一次做完，不 commit。
- Doctor：新增 `ctx.cancel <gid> | gid=<n> | name=<n> | --address <ptr>`。
  只对 `RegisterContext()` 登记的 Context 调用 `Cancel()`，**不 Unlock**；
  `--address` 只在注册表内按指针身份匹配（不解引用任意地址）。响应
  `schema=go2cpp.diagnostic.ctx-cancel/1`，`verdict=cancelled` + `context.*`
  + `audit.actor/pid/ts_ns/wall_ms/cancel_thread`；stderr 有审计行；
  `stats().context_cancels` 计数。拒绝：`not-registered`（未注册 gid/address）、
  `bad-argument`（非法 gid、缺参数）。
- 新场景 `tests/scenarios/ctx_cancel_rollback/`：A/B 交叉持锁死锁，两把锁的
  持有都绑定 `ContextRollback::RecordUndo`；`deadlock-report` 判 deadlock 后
  `ctx.cancel gid=A` → undo 在取消线程释放 L1，A 的 `Lock(L2)` 返回 false、
  `ctx.Err()` 非空；B 拿到 L1 完成。三模式：cancel / external（真实 socket +
  doctor_client.py）/ force（对照 stale-unlock）。
- 实测（build-doctor-full, Release）：
  - cancel：pass=1，undo_executed=1，rolled_back，locks 3=3，L1/L2
    held=0 waiters=0，shared_work=1；单独连续 10/10 轮通过；
  - external：65 项断言全 PASS（含 4 条拒绝路径），连续 5 轮 PASS=65 FAIL=0；
  - force：stale=1、undo_executed=0、committed、4/3 + force1 = balanced
    （对照组证明强制解锁的 stale 风险）。
- 关键竞态：取消的 undo 先释放 L1 → B 完成释放 L2 → A 被正常唤醒抢到 L2 →
  取消回调尚未执行。A 在 `Lock` 返回后必须**无条件检查 `ctx.Err()`**，
  拿到锁但已取消就立刻归还（`cancel_race=1`），否则会带着「已失去 L1」的
  假设继续跑。
- 既有缺陷（未改）：`Mutex::DebugSnapshotInternal().native_waiters` 在取消与
  Unlock 竞争时偶发下溢（SIZE_MAX）；权威队列长度是 `waiters`。另有
  build-doctor-full 陈旧构建导致 go2cpp_tests 假 SEGFAULT（重建 go2cpp_tests
  后 ctest 116/116）。
- 验证：`bash tests/scenarios/ctx_cancel_rollback/run_ctx_cancel_rollback.sh
  build-doctor-full` → PASS=65 FAIL=0；`ctest` → 116/116；`git` 未 commit。
- 报告：`/tmp/ctx_cancel_rollback.md`。

## 26. 2026-10-27 Bento P1：控制权仲裁 / 多 peer 加密 / 日志钩子（deepseek）

**新增能力**
- `SessionControl`（`session_control.hpp/.cpp`）：View/Interact 唯一控制器、先到先得/抢占、
  租约 + 断开回收、变更回调与 `Snapshot().ToJson()`；16 线程并发请求只产生 1 个 Interact。
- `PeerHub` PSK 加密（`peer_hub.hpp/.cpp`）：`SetPreSharedKey`/`SetPeerPreSharedKey`，
  每 peer 独立 nonce/方向密钥/wire seq/64 位重放窗口；加密模式下拒绝明文 HELLO/数据帧。
- `LogHook`（`log_hook.hpp`）：级别过滤、14 类结构化事件、按级别/事件计数、锁外 sink。
- 测试：`test_session_control`（14,816 checks）、`test_peer_hub_secure`（506）、
  `test_log_hook`（124）；ctest Bento 26/26（netem 无 CAP_NET_ADMIN 按设计 SKIP）。

**约束 / 陷阱**
- `Grant(peer, kNone)` 只清 `revoked` 标记，之后该 peer 仍可 `RequestControl` 重新拿到
  控制权（审查 D-01，P1 安全契约漏洞，本轮未修）。
- `PeerHub` 的 `on_peer_connected`/`on_peer_disconnected`/`on_stream`/`on_datagram`
  在 `RunOnce` 持锁期间同步调用；回调里禁止 park/阻塞/重入 `RunOnce`，只可转发或
  `Send*`（递归锁）。`SessionControl::on_change`/`LogHook` sink 在锁外。
- 监听端 per-peer PSK 只能继承 Hub 默认 PSK；`SetPeerPreSharedKey` 的差异化密钥只用于主动端。
- 日志事件有覆盖缺口：握手超时走 `kPeerDisconnected`（不发 `kHandshakeFailed`），
  帧解析/发送封装失败只计数；`LogHook` 未注入时只累计 `filtered`。
- Bento 不依赖 base（只链 Threads/hash），并发政策 R1/R3 属结构性例外，
  ADR 见 `docs/modules/bento-scenarios.md` §20。

**验证 / 状态**
- 已提交 `1e08723`；文档已于 2026-10-28 按审查 `docs/review/review-p1-modules-api.md` 回改
  （9.1/9.5/18.5 的 PSK 状态、7.2 回调签名、19 节提交状态、19.6 示例、并发 ADR、CMake 头注释）。
- 未全量复跑 ctest；审查只复跑 3 个新增测试。首次 `bento_test_peer_hub_secure` 失败归因于
  共享构建目录的二进制被并发重建（重跑 PASS），CI 应使用独立构建目录。


## 27. 2026-10-28 Bento P2 app 层 + UDP/X25519（deepseek）

- 需求：补齐 JinShan 场景缺口 FR-09（下载保存）/FR-15（远程文件访问）/FR-18（备份上传）/
  CON-06（差环境探测），落地 `udp_transport`/`x25519`；同时修复批2审查
  （`docs/review/review-batch2-api.md`）A-01/A-02 与 API 一致性关键项。
- 新增模块（`include|src/bento/app/**`）：
  - `app::Fetch`（MOD-27）：HTTP(S) 下载、路径沙箱、流式 SHA-256、大小上限、
    重试/退避、重定向、原子落盘；http 自研 / https 调 curl（缺失返回
    `kBackendUnavailable`）。
  - `app::ZipWriter`/`ListZip`/`ExtractZip`（MOD-31）：store-only ZIP32、CRC-32、
    条目/大小上限、Zip Slip 防护，非 0 method 明确拒绝。
  - `app::Backup`/`Upload`（MOD-31）：`.part` → rename 原子包；curl PUT/POST、
    token 走 stdin、指数退避、失败保留本地包。
  - `app::FileBrowser`（MOD-30）：根目录沙箱 List/Get/Put/Stat、拒绝符号链接、
    临时文件 + rename、覆盖策略。
  - `app::Probe`（MOD-37）：15 项只读探测（/proc、cgroup、epoll、rlimit、
    ifaddrs、statfs、uname）。
  - `app::Session`（MOD-39）：endpoint → transport → pairing → channel → control
    → heartbeat → graceful close；transport 工厂可注入。
  - `net::UdpTransport`：无连接 UDP 的 `ITransport` 实现，错误映射与 TCP 对齐。
  - `net::X25519*`：RFC 7748 ECDH，TweetNaCl 移植（来源/许可证见 `AI_COLLAB.md`）。
- 规范修复（批2审查）：
  - A-01：`bento-scenarios.md` §14 删除 zip/AVI 过时表述，§18/§19 边界声明补
    app 层，新增 §21 落地登记；§16 自检补 `app/**` 引用范围。
  - A-02：`docs/INDEX.md` 登记 `modules/bento-scenarios.md`；本节补齐
    MOD-27/30/31/37/39 记录。
  - C-01/C-03：fetch/zip/backup/file_browser 从 `go2cpp::bento` 迁入
    `go2cpp::bento::app`；file_browser 的 `List/Get/Put` 随命名空间收敛。
  - C-02：backup/zip 状态字符串 kebab → snake_case（kebab 只留给 JSON）。
  - C-04：fetch/backup 的重复 `SleepBeforeRetry` 抽到
    `app::detail::SleepBeforeRetry(base, failed_attempt, cap)`
    （下载 2s / 上传 30s 封顶）。
  - C-05：删除死枚举 `UploadStatus::kRetryExhausted`；重试耗尽返回最后一次
    失败分类。
  - 另补 C-08/C-09：`kFileBrowserMaxEntriesCeiling` 常量替代
    `file_browser.cpp` 的 `10 * 1000 * 1000` 魔法数；C-12：修正
    `udp_transport.hpp` 零长读注释（不再是「唯一例外」）。
- 验证（`/tmp/build-appfix`，Release，Ninja）：
  - `ninja` 378 目标全成功，`-Werror` 0 警告；
  - 两次全量 ctest 均 **59/59 通过**：`-j 4` 56.63s（netem 36.59s 实跑）、
    `-j 2` 64.20s（netem 按设计 SKIP=77，其余 58 项 Passed）；
  - app 断言：fetch 237、zip 291、backup 111、upload 97、file_browser 1890；
    net：capability 125、session 259、udp 158、x25519 1160、command 93、
    file_transfer 162。
- 未 commit；已知缺口：app 层无 `LogHook` 注入、`Upload` 无远端校验
  （HEAD/ETag）、`Probe()` 无库内降级消费者、store-only 带宽代价、HTTPS 依赖
  系统 curl、FileBrowser TOCTOU 与 Backup `.part` 并发（D-05/D-06 未修）。
