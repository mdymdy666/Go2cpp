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
