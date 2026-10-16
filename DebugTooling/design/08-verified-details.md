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

---

## 核验 3 · `WaitNode` 的等待/唤醒机制（第 14 轮）

**源码**：`src/concurrency/sync.cpp:31-100`（`SchedulerWaitBackend`）、`132-235`（`WaitNode` 核心）、
`296-345`（`WaitNative`/`Reset`）、`360`（`t_wait_node_cache`）。

### 3.1 默认后端只有三件事

| 方法 | 实现要点 |
|---|---|
| `Park` | 循环：**谓词先查** → `scheduler->park_wait(task)` → **再查谓词**；退出条件三条：取消、**上下文漂移**（`current_scheduler()`/`current_task()` 已不是原来那个）、未挂起且调度器已停 |
| `Wake` | `wake_registered(task)`；若返回 false 且任务处于 `kRunnable` 且未入队，再 `enqueue(task)` 收口；**异常被 `catch (...)` 隔离** |
| `ParkForCleanup` | 循环 `park_wait_for_cleanup` 直到谓词满足（用于条件变量重锁，**不可被取消中断**） |

> `Wake` 的二次尝试对应注释里说的「`wake_registered` 可能在 Running/Waiting → Runnable 的 pending 窗口返回 false」；
> 若 execution claim 仍被持有，则留给所属 worker 的 post-Fiber 清理路径。

### 3.2 单赢家靠一次 CAS

```cpp
bool TryFinish(WaitResult result) noexcept {
    WaitResult expected = WaitResult::kWaiting;
    return m_result.compare_exchange_strong(expected, result,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire);
}
```

`m_result` 是 `std::atomic<WaitResult>`（三态：`kWaiting`/`kNotified`/`kCancelled`）。
**与 `SelectWaitState::TrySelect` 同构** —— 整个同步层的"谁赢"都收敛到"一次 CAS"。

### 3.3 **新发现：`WaitNode` 有 generation 机制**（此前文档未记录）

`TryCancelAndWake(generation)`（`sync.cpp:215-230`）的注释原文说明了原因：

> Context 的取消回调可能已经通过 weak_ptr 取得了本节点，随后 `RemoveCallback` 又与节点回收并发。
> 取消、active 检查和唤醒必须在**同一把门锁内**完成；否则旧回调可能在节点放回 thread-local cache 后，
> 把下一次等待的 `m_result` 错误地改成 Cancelled。**generation 用来区分同一个缓存节点的不同等待代次。**

**这对调试模块有直接影响**：

1. `WaitNode` 会被**缓存复用**（`t_wait_node_cache`，`sync.cpp:360`）；
2. 因此**调试快照绝不能把"缓存中的节点"当成"活跃等待者"** ——
   否则等待者数量会被**虚报**；
3. 判断活跃的正确依据是 `m_active`（仅在 `Arm()` 成功时为 true，`136-140`）与 `m_result == kWaiting`；
4. **订正**：`scenarios/D-locks.md` 的 D1 只写了"读 `m_waiters` 需要持队列锁"，
   **补充**：还需排除缓存节点，建议只在 `Arm` 成功后才把节点计入快照（即队列里的节点本身已是 active 的，
   但**已取消未清理**的节点需要单独呈现）。

### 3.4 两条等待路径（D2 的实现基础）

| 路径 | 机制 | 对应计数 |
|---|---|---|
| **Fiber** | `backend->Park(...)` → `Scheduler::park_wait(task)` | `m_waiters.size() - m_native_waiters` |
| **普通线程** | `WaitNative()`：`m_native_mutex` + `m_native_condition.wait(谓词)` | `m_native_waiters` |

另有一个细节：`WaitNative()` 把 `condition_variable` 的异常**转成 `kCancelled` 终态**，
理由是"不要让错误越过运行时等待边界"（该边界声明为 `noexcept`）。

---

## 核验 4 · 安全点统计（第 14 轮）—— **解除 B5 的阻塞项**

**命令**：`grep -rn "SuspendForScheduler" src/ include/` 与相关的挂起入口检索。

### 4.1 实测：真实挂起点共 **5 处**（全部在 `src/concurrency/scheduler.cpp`）

| # | 行 | `SuspendReason` | 语义 |
|---:|---:|---|---|
| 1 | 2661 | `Yield` | 主动让出（`yield_current` 路径） |
| 2 | 2723 | `Park` | 挂起等待 |
| 3 | 2744 | `Park` | 挂起等待（另一条 park 路径） |
| 4 | 2782 | `Io` | IO 就绪等待 |
| 5 | 2809 | 传入的 `reason` | 通用 `park_with_reason` |

（另有定义处 `src/concurrency/fiber.cpp:1258`。）

### 4.2 对 B5（断点）的**可承诺范围**

- ✅ **可以承诺**：断点能停在以下**四类语义位置** ——
  **Yield（让出）/ Park（挂起）/ Io（IO 等待）/ 通用挂起点**；
- ❌ **不能承诺**：任意行、任意函数调用点、循环体内 —— 这是设计使然（项目明确不切断 C++ 调用栈）；
- **精度结论**：断点粒度 = **"一次挂起/让出"**，而不是"一行代码"。

### 4.3 因此的**文档订正**

1. `scenarios/verification-matrix.md` 中 B5 的"阻塞项"列原本写 **安全点未统计**
   → **现改为"已统计（5 处）"**，B5 **不再是阻塞项**；
2. `scenarios/B-mutation.md` 的 B5 原本只写"只能停在安全点"
   → **补充**具体是**哪四类**位置与"粒度 = 一次挂起"的量化描述；
3. 但 `FINAL-REPORT.md` §6 第 4 项「任意行断点做不到」**保持不变** ——
   这是**永久限制**，不是"待统计"的临时状态。

---

---

## 核验 5 · `ConditionVariable` 的通知模式（第 15 轮）

**源码**：`src/concurrency/sync.cpp:1021`（`Impl`）、`1046`（`Wait`）、`1123`（`WaitFor`）、
`1139`（`NotifyOne`）、`1166`（`NotifyAll`）。

### 5.1 `NotifyOne` 与 `Mutex::Unlock` **完全同构**

```cpp
void ConditionVariable::NotifyOne() noexcept {
    std::shared_ptr<WaitNode> selected;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        while (!m_impl->m_waiters.empty()) {
            auto candidate = std::move(m_impl->m_waiters.front());
            m_impl->m_waiters.pop_front();
            if (candidate && candidate->TryFinish(WaitResult::kNotified)) {
                selected = std::move(candidate);
                break;
            }
        }
    }
    if (selected) { selected->Wake(); }     // 锁外唤醒
}
```

**四点与 `Mutex::Unlock`（`955-1005`）一致**：队首取、`TryFinish` 认领、失效节点跳过、**锁外 `Wake`**。
→ 说明整个同步层的出队是一套统一模式（可复用同一份"选择钩子"设计，见 D3）。

### 5.2 `NotifyAll` 用 swap 端出队列

```cpp
std::deque<std::shared_ptr<WaitNode>> waiters;
{ std::lock_guard<std::mutex> lock(m_impl->m_mutex); waiters.swap(m_impl->m_waiters); }
for (const auto& waiter : waiters) { /* 锁外逐个唤醒 */ }
```
与 `Scheduler::Close()`（把队列整个端出来、锁外统一唤醒）是同一手法。

### 5.3 对 D5 的补充

- D5 需要的"等待者快照"可直接读 `m_waiters`（持 `m_impl->m_mutex`）；
- **通知统计**（`notify_one_calls`/`notify_all_calls`/实际唤醒数）需要新增计数，
  但位置很清楚：`NotifyOne` 的 `selected` 是否为空 + `NotifyAll` 的 `waiters.size()`；
- **唤醒失败**（`TryFinish` 返回 false，即节点已被取消/超时）也应计数 ——
  这能回答"通知是否打空"这个常见疑问。

---

## 核验 6 · sysmon 阻塞观测的完整生命周期（第 15 轮）

**源码**：`scheduler.cpp:1051-1054`（字段）、`3200-3218`（声明阻塞）、
`1432-1458`（sysmon 超阈 detach）、`3255-3270`（退出阻塞）、`3386-3388`（导出快照）。

### 6.1 三个阶段

| 阶段 | 位置 | 做了什么 |
|---|---|---|
| **① 声明阻塞** | `3200-3218` | 仅当 M 处于 `kRunning` 才进入 `MState::kBlocking`；记 `blocking_since_ns = steady_now_ns()`；**`blocking_task = current_task()->id()`**；复位 `processor_detached`；`blocking_workers++` |
| **② 超阈 detach** | `1432-1458` | sysmon 周期扫描；对 `kBlocking` 且 `now - blocking_since_ns >= threshold` 且尚未 detached 的 M：置 `processor_detached = true`、**`long_syscall_count++`**、**`processor.active_machines--`**（可能因此启动替代 M） |
| **③ 退出清理** | `3255-3270` | `processor_detached.exchange(false)`、`blocking_since_ns = 0`、`blocking_task = 0`、状态回 `kRunning`、`blocking_workers--` |

`threshold` 来自 `SchedulerConfig::long_syscall_threshold`（默认 **50 ms**，`scheduler.hpp:178-181`）。

### 6.2 **两个易误读点（订正 A6 的解读）**

1. **`long_syscall_count` 是「累计超阈次数」，不是「已阻塞多久」** ——
   它在 detach 时 `fetch_add(1)`（`1447`），退出阻塞时**不会清零**。
   → 调试输出必须标注为"次数"；想知道"当前阻塞了多久"，要用
   `blocking_since_ns`（但**该字段不在 `MachineSnapshot` 里**，需新增或另算）。
2. **`blocking_task` 是「当前被阻塞的那个 G 的 id」**（`3212`：`current_task()->id()`），
   而**不是**"机器 id"或"任务类别"。→ 这让 A6 能直接回答
   **「是哪个 Fiber 把 M 卡住了」**，比此前文档的表述更有用。
3. **detach 会减少 `processor.active_machines`** → 可能触发**替代 M 的启动**。
   → 因此调试时看到"worker 数量上升"可能正是长阻塞导致的，**不应误判为异常**。

### 6.3 对 `scenarios/A-observability.md` 的**订正**

A6 原文只写「`processor_detached` + `blocking_task` + `long_syscall_count` 三者组合，
已足够定位"哪个 M 卡在原生调用上"」。**补充**：
- `blocking_task` 直接给出**阻塞中的 G id**；
- `long_syscall_count` 是**次数**（易误读）；
- **"当前阻塞时长"需要新增字段**（`blocking_since_ns` 未导出）→ 归入 R 组接口的补充项。

---

---

## 核验 7 · `Context` 的回调机制（第 16 轮）

**源码**：`include/go2cpp/concurrency/context.hpp:41-89,256-258,450-451`；
`src/concurrency/context.cpp:23-27,105,126,176,266,409-410`。

### 7.1 回调有**三类**，不是一类

| 类别 | 容器 | 注册接口 |
|---|---|---|
| **内部回调** | `internal_callbacks`（`context.cpp:26`） | `AddInternalCallback`（`266`） |
| **before-done 回调** | `before_done_callbacks`（`context.cpp:410`） | `Context::AddBeforeDoneCallback`（`context.hpp:256`） |
| **普通回调** | `callbacks`（`context.cpp:27`） | `DoneSignal::AddCallback`（`context.hpp:75`） |

另有 `next_id{1}`（`context.cpp:23`）与 `next_before_done_id{1}`（`409`）两套 id 计数器。

**对调试模块的含义**：调试插件若要观测"上下文何时结束"，必须**明确注册到哪一类** ——
三类回调的调用时机与顺序不同（**具体顺序本会话未读，标注为未确认，不推测**）。

### 7.2 **回调生命周期由调用方负责**（关键约束）

`context.hpp:73` 的注释原文：

> owner，应先调用 `RemoveCallback`；信号**无法推断回调的对象所有权**。

**这是硬约束**：`DoneSignal` 不会替你管理回调的存活期。项目内部已经给出了正确做法 ——
`channel.hpp` 的 `CallbackGuard`（RAII：析构时 `RemoveCallback`）。
→ **调试插件必须采用同样的 RAII 纪律**，否则会在 context 上留下悬挂回调（且是悬空指针级的问题）。

### 7.3 对 B6（取消收敛）的补充

- 自定义 `WaitBackend`（`scenarios/B-mutation.md` 的 B8）与调试订阅都要在当前 G 的
  context 上注册回调 → **必须用 RAII**；
- 观测"取消是否收敛"可依赖已有的 `kTaskCancelled` 事件（`scheduler.hpp:117`），
  而不必自己挂钩回调 —— **推荐做法**，避免额外生命周期负担。

---

## 核验 8 · `Hook` 与 `BlockingRegion` 的协作（第 16 轮）

**源码**：`src/network/hook.cpp:206,832-846,940-963,1025`。

### 8.1 Hook 在拦截点临时构造 `BlockingRegion`

```cpp
std::optional<go2cpp::BlockingRegion> blocking_region;   // hook.cpp:206 / 846 / 953
```

出现在 `cooperative_io`（`832`）、`cooperative_connect`（`940`）、`cooperative_sleep_for`（`1025`）等
拦截函数中 —— 即**被拦截的系统调用执行期间**用一个 `BlockingRegion` 声明"可能阻塞当前 M"。

### 8.2 **只对 managed G 生效**（易误判点）

`hook.cpp:963` 的注释原文：

> managed G 中补齐 `BlockingRegion`，普通线程保持原生语义。

**含义**：同一个被 Hook 的调用，**在 Fiber 里**会声明阻塞区（进而可能被 sysmon 观测、可能触发
M 与 P 解绑），**在普通线程里**则保持原生阻塞语义（不进调度器账本）。
→ **调试时同一段代码在不同上下文中的表现不同**，看到 `processor_detached` 与否取决于执行者是不是 managed G。
这一点必须写进调试文档，否则会被误判为"行为不稳定"。

### 8.3 与 A6 的关系

A6 的三个字段（`processor_detached` / `blocking_task` / `long_syscall_count`）的**数据来源就是这里**：
Hook（或显式 `BlockingRegion`）声明 → 记录 `blocking_since_ns` 与 `blocking_task` →
sysmon 超阈置 detached 并计数（详见核验 6）。
→ 因此 **A6 能看到的阻塞，仅限"已声明"的阻塞**；未被 Hook 覆盖、也未显式声明的原生阻塞
（例如第三方库里直接调用的阻塞 syscall）**不会进入这套观测**。这是观测的**覆盖边界**，
应当明确写在文档里，避免使用者以为"没显示阻塞就没有阻塞"。

---

## 核验阶段小结（第 13–16 轮）

| 轮次 | 核验内容 | 产出 |
|---:|---|---|
| 13 | `worker_loop` 出队路径（四级 + 本地 FIFO + 偷取从队尾）；6 个事件发射点的锁边界（**全部不持锁**） | 订正 B4/C2 与 `design/04` §3 |
| 14 | `WaitNode`/`WaitBackend` 机制（CAS 单赢家、两条等待路径、**`generation` 防缓存节点误唤醒**）；**安全点统计（5 处）** | **解除 B5 阻塞项** |
| 15 | `ConditionVariable` 与 `Mutex::Unlock` **同构**；sysmon 阻塞观测三阶段 | 订正 A6 三处易误读 |
| 16 | `Context` 回调**三类** + **生命周期由调用方负责**；`Hook` 的 `BlockingRegion` **仅对 managed G 生效** | 补充 B6 与 A6 的**覆盖边界** |

**净成果**：订正 **9 处**结论、解除 **1 个**阻塞项（安全点统计）、新增 **4 条**易误读提示
（`long_syscall_count` 语义、detach 与 worker 数、缓存 `WaitNode`、Hook 的 managed G 分支）、
以及 **1 条观测覆盖边界**（未声明的原生阻塞不可见）。

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
