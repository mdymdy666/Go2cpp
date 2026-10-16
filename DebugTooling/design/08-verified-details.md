# design · 08 源码核验补充（逐轮消除「未确认」）

> 本文件记录**后续轮次读源码核验**的成果，用于**订正**前面文档里标注为「未确认」的内容。
> 每条都给出源码位置与结论；**订正会在条目中显式写明改了哪个文档的哪句话**。
> 目的：让交付文档里的"未知"逐步变成"已知"，而不是靠推测填空。

---

## 核验 1 · `worker_loop` 的出队路径与 work stealing（第 13 轮）

**源码**：`src/concurrency/scheduler.cpp:3494`（`worker_loop` 入口）、`3519-3541`（本地快路径）、
`3742`（偷取注释）、`3759`（偷取计数）。

### 出队顺序（四级，按优先级）

| 级 | 来源 | 关键代码 | 备注 |
|---:|---|---|---|
| ① | **本 P 本地队列** | 持 `own_processor.mutex`，`queue.front()` + `pop_front()` | **队首出 = FIFO**；注释：「本地队列由同一 P 的 worker 独占消费，先走无调度器总锁路径」 |
| ② | **incoming 条带** | `incoming_probe.fetch_add(1)` 轮转探测 + 任务类别亲和性 | 外部生产者投递的入口 |
| ③ | **全局队列** | `global_queue.pop_front()`（`1636`） | 需 admission 锁 |
| ④ | **work stealing** | 注释原文：`Finally steal from the back of another P. A bounded scan`（`3742`） | **从其他 P 的队列尾部偷**；`metric_steal_pops++`（`3759`） |

### 本轮确认的三个细节（此前文档未写）

1. **本地 FIFO、偷取 LIFO**：本地从**队首**取，偷取从**队尾**拿 —— 这是标准 work-stealing 设计。
2. **本地快路径有门槛**：仅当
   `!stopping && running_workers != 0 && runnable > processors.size()`
   时才走"无全局锁"的本地路径；**低负载时直接进入通用路径**。
3. **出队后立即认领**：`runnable.fetch_sub(1)` → `task->try_mark_running()`，
   **认领失败则 `clear_queued()`**（`3539-3541`）—— 处理"出队后状态已被并发改变"的竞态。

### 对 C2（插队）分析的**订正**

原 `scenarios/B-mutation.md` 的 B4 与 `design/06-interface-signatures.md` 的 C2 只写了
"唯一本地入队点是 `processor.queue.push_back`"与"必须持 `Processor::mutex`"。**补充两条**：

- **插队目标必须考虑偷取方向**：偷取发生在**队尾**，因此插到**队首**的 G
  在本 P 消费前**不会**被其他 P 偷走；而插到靠近队尾的位置则可能立刻被偷走
  → 若要保证"插队后按预期顺序执行"，应插到**队首方向**；
- **跨 P 插队仍然不可行**（与既有结论一致）：目标 G 必须与移动 G 在**同一个 P 的队列**中。

---

## 核验 2 · 6 个 `emit_event` 发射点的锁边界（第 13 轮）

**源码**：`grep -n -B4 -A3 "emit_event" src/concurrency/scheduler.cpp`。

| # | 行 | 事件 | 所在位置 | **是否持锁** |
|---:|---:|---|---|---|
| 1 | 1939 | `kStarted` | `start()` 尾部 | **否** —— 紧跟 `lock.unlock()`（1937）与 `condition.notify_all()`（1938）之后 |
| 2 | 1992 | `kStopping` | `shutdown()` 中 | **否** —— 在 `if (was_started)` 分支，锁已释放 |
| 3 | 3503 | `kWorkerStarted` | `worker_loop` 入口 | **否** |
| 4 | 3833 | `kTaskStarted` | `task->run()` 之前 | **否** |
| 5 | 3848 | 任务事件（4 种由三元判断分发） | `task->run()` 返回之后 | **否** |
| 6 | 4036 | `kWorkerStopped` | worker 退出路径 | **否** |

### 结论与含义

1. **全部 6 个发射点都不在调度器状态锁内** —— 与 `emit_event` 的注释
   「绝不在调度器内部状态锁上执行外部代码」**完全一致**（可交叉验证，非推测）；
2. **但 #4/#5 位于 worker 的执行循环上**（`task->run()` 前后）→
   **回调仍然占用 worker 线程的时间** → 这是"插件只能做快速入队"的直接依据（此前结论获得实现级支撑）；
3. **#3 有个有用细节**：`kWorkerStarted` 是在 `t_machine_id = machine->id;`
   与 `t_processor_id = machine->processor;`（`3501-3502`）**之后**才发出的，
   因此插件在这个回调里调用 `Scheduler::current_machine_id()` /
   `current_processor_id()` 能读到**正确的** thread-local 值（`scheduler.hpp:609-610`）。
   → 这对"把事件按 M/P 归类"的插件很有用，此前文档未提及。

### 对 `design/04-encoding-roadmap.md` §3 的**订正**

原文说「`SchedulerObserver` 的回调在 worker 线程上同步执行，所以 sink 的 `Write` 必须非阻塞」。
**补充实现级依据**：6 个发射点均**不持锁**（因此不会引入锁反转风险），
但 **#4/#5 在任务执行热路径上**，所以"非阻塞"的要求来自**时序影响**而非**锁竞争**。
这一区分很重要：它意味着「只要入队足够快，就不需要担心与调度器锁的交互」。

---

## 核验 3 · 待办（后续轮次）

| # | 待核验 | 影响 |
|---:|---|---|
| 1 | `Mutex::Unlock` 之后 `WaitNode::Wake()` 的完整路径（`WaitBackend` 默认实现如何 park/wake） | D1/D2/D3 的实现细节 |
| 2 | `CondVar` 的 `NotifYOne`/`NotifyAll`（`sync.cpp:1139`）的等待队列操作 | D5 |
| 3 | `Hook` 与 `sysmon` 的交互（阻塞区如何被观测） | A6 的字段来源 |
| 4 | `Context` 的取消传播与 `DoneSignal` 回调注册/移除的锁边界 | 影响"取消观测"的场景（B6）与 `CallbackGuard` 语义 |
| 5 | `timer` 的内部结构与 `schedule_every` 的线程模型 | 与 `--watch` 周期快照的实现方式相关 |
| 6 | 安全点的**数量与分布**（`Suspend`/`park_for_scheduler`/`mark_yielded*` 的调用点统计） | **B5 断点精度的前提** |

> 注：上述第 6 项是 `scenarios/verification-matrix.md` 中列出的**阻塞项之一**，
> 只有统计后才能对断点精度作出承诺。

---

## 本文件的约定

- 每条核验都必须给出**可复现的命令或源码位置**；
- 若核验结果与前面文档冲突，**以本文件为准，并在条目中写明订正了哪里**；
- 核验不了的（例如需要跑起来才能测的），移入"待办"而不是写成结论。
