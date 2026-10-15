# 01 · 运行流程说明（与架构图内「运行流程」块一一对应）

> 本文件是 `00-architecture.svg` 图内文字块的**完整展开版**：图里只能放要点，这里给出每一步的
> 代码级细节、涉及符号与不变量。所有结论均来自 `modules/G1~G9` 九份模块报告（本工作区实测/实读）。
> 阅读顺序：§1 一次 `go()` 的六个阶段 → §2 三条横向流程（取消 / IO / 错误）→ §3 四类边界。

---

## 1. 一次 `go()` 的六个阶段

### 阶段 ① 提交：`go(fn)` → G 入队

```text
go(fn)                       // include/go2cpp/tools/go.hpp：默认调度器（进程级 new，不析构）
  └─ Scheduler::spawn(fn)    // TaskOptions{stack_size, task_class, fiber_bin_capacity}
       ├─ Task 构造：m_state=kNew、m_queued=false、m_registered=false
       ├─ bind_owner(Scheduler 令牌) + prepare_enqueue(owner, cancellation_gate)
       └─ 入队路径（按优先级）：
            P 本地队列（processor.queue，上限 local_queue_limit=256）
            → 16 条 incoming 条带（外部生产者分片，TLS 缓存条带号）
            → 全局队列 global_queue
```

- **不变量**：每个 G 只能绑定一个 Scheduler；最多持一个队列 claim（`m_queued`）与一个执行 claim（`m_execution_claim`）。
- **坑**：`Scheduler::shutdown()` 是终态；`go()` 在 shutdown 后会**新建**一个默认 Scheduler（旧句柄仍可等待已提交任务）。
- 相关：`source: tools/go.hpp`、`scheduler.hpp` 的 `Task::prepare_enqueue`/`bind_owner`。

### 阶段 ② 取任务：worker(M) 的选择顺序

```text
worker_loop（M 线程）：
  1. emergency_pop_locked()      // 无分配救援链（仅分配失败/关闭交接）
  2. 本地 processor.queue 队首    // take_local（无锁快路径优先）
  3. global_queue 队首
  4. 轮转扫描其他 P，窃取队尾      // take_victim（queue.rbegin()），计入 steal_pops
软亲和：在队列前 task_affinity_budget(4) 项内优先同类 TaskClassId
```

- M 是**惰性创建**的：`start()` 只建 `min_workers`（默认 1）+ sysmon；之后按 backlog 经
  `maybe_grow()` 扩到 `min(max_workers, P + max(blocking, detached))`。
- `worker_count()` = 当前存活 M 数，**与 P 数没有固定等式**（声明阻塞区/被 sysmon detach 时可 > P）。

### 阶段 ③ 切入 Fiber：真正开始执行 G

```text
M: Fiber::resume_from_scheduler()
   ├─ 首次 resume 绑定父级（bind_caller）并固定 depth = parent+1
   ├─ initialize_context()（懒分配）：FiberStack = mmap(usable + 1 page) + mprotect(guard, PROT_NONE)
   ├─ detail::JumpContext(m_context, this)   → context_x64.S 保存 rbp/rbx/r12-r15+rsp/rip 并 jmp
   └─ G 状态 → Running；Fiber::BindCurrentExecution({scheduler,task,M,P})
```

- **串行化**：execution claim 保证同一时刻只有一个 M 进入该 Fiber。
- **异常边界**：Fiber trampoline 内 `catch(...)` → `m_failure` + `Failed`；`resume` 本身 `noexcept`。
- `yield()` 只让出给同一 Scheduler（`mark_yielded_fast()` + `defer_enqueue()`）。

### 阶段 ④ 等待：一切等待都汇聚到 `ParkingCondition`

```text
WaitOnce(lock, deadline, predicate)：
  managed = (current_scheduler() != nullptr && current_task() != nullptr)
  ├─ 非 managed 且 Fiber::Current() != nullptr → 直接 kCancelled（绝不阻塞 carrier 线程）
  ├─ managed → 构造 WaitNode（atomic<Result>{kWaiting} + HybridMutex + 可选 Timer）
  │             入 m_waiters（锁内二次检查 predicate，关闭 notify-before-publication 窗口）
  │             Scheduler::park_wait(task) → Task::park_for_scheduler() → Fiber::SuspendForScheduler()
  └─ 普通线程 → condition_variable_any 阻塞（native 路径）
```

- 各原语只是换了 predicate 与队列：`Channel` 的 `PendingSend/PendingRecv`、
  `sync` 的 `WaitBackend::Park`、`Future` 的 `FutureWaitBackend`、
  `IOManager::wait_*` 的 `park_io`、`Context::DoneSignal` 的等待。
- **伪唤醒**不 busy-yield，而是重新 park（`WaitManaged` 循环）。
- `FiberWaitUnsupported()` 是「手动 Fiber」身份的探测器。

### 阶段 ⑤ 唤醒：原子单赢家 → 重新入队

```text
唤醒来源（四类，互斥）：
  · channel 对端 Send/Recv/Close
  · TimerService 到期（deadline）
  · epoll readiness（IOManager 的 poller 线程）
  · Context 取消（Cancel/Deadline）

路径：
  WaitNode::Finish(终态)   // compare_exchange_strong(kWaiting → result, acq_rel)：只有一个赢家
  WaitNode::Wake()         // HybridMutex 门内取 {scheduler, task}，锁外调用
  → Scheduler::wake_or_cancel(task) → Task::wake_for_wait()
        ├─ Waiting → Runnable（立即入队：P 亲和 → incoming 条带）
        └─ Running → 置 m_wake_pending（运行中的 G 在下次 park/yield 消费该令牌）
  → Scheduler::park_wait 的等待者被唤醒并复检（终态 / 取消 / scheduler 与 task 身份）
```

- **park 与 wake 共用 `Task::m_transition_mutex`**，因此不存在「park 读到 Running+pending=false」的丢失唤醒。
- `wake_for_io` 与 `wake_registered`/`wake_or_cancel` 对 `WakeAction::kPending` 的**返回值语义相反**（台账 X-10）：
  前者视为「已受理」返回 true，后两者视为「未立即入队」返回 false。

### 阶段 ⑥ 完成：终态 → 通知 → 回收

```text
主体返回 → trampoline 收尾：Fiber → Completed（或 Failed）
  → Task 终态（kDead / kFailed / kCancelled）→ notify_terminal()
      → m_completion_condition.notify_all()（唤醒 wait()/wait_for() 者）
  → Fiber::RecycleForScheduler(fiber, fiber_bin_capacity)
      条件：state ∈ {Completed, Failed} ∧ context/stack 已释放 ∧ 无 resume claim ∧ record.use_count()==1
      → 放回 **当前 M 的线程本地 FiberBin**（默认 32，上限 4096）
  → 栈归还 FiberStack 缓存（TLS ≤8 项；溢出进全局池 ≤128 项；再溢出 munmap）
```

- `metrics()` 的计数在 `task->run()` 返回后累加，**与 `wait()` 返回之间存在窗口**
  → 任何基于计数的断言都必须「轮询 + 超时」（项目规范，来自 commit `351f756` 的 flaky 复盘）。

---

## 2. 三条横向流程

### 2.1 取消流（`context`）

```text
Cancel(cause) / CancelDeadline() / deadline 到期
  → State::Cancel → DrainCancellation（**迭代**实现，显式 vector 栈，测试覆盖 20000 层深树）
       ① 整棵子树标记：写 error/cause、摘 timer、从 parent->children 摘除、搬走 before_done_callbacks
       ② 跑 before-done 钩子（逆序）—— 让 ContextRollback 冻结 undo 日志
       ③ 对 to_signal **逆序** 调 DoneSignal::Signal()（子先于父线性化）
  → DoneSignal::Signal()：置 done → 搬走两组回调 → 解锁 → notify_all()
       → 先跑 internal 回调（ContextRollback 执行补偿）→ 再跑用户回调
```

- `root` 的 `Cancel()` 是**空操作**（`if (root || error) continue;`）。
- **两个信号不可混用**：`context()->Done()` = 取消已线性化；`RollbackDone()` = 补偿跑完。
- 判定用身份比较：`Is(err, DeadlineExceededError())`。

### 2.2 IO 流（`hook` → `IOManager` → `epoll`）

```text
应用调用 read/write/connect/... （被 Hook 拦截的 31 个 libc 函数之一）
  → cooperative_io(fd, fn, event, ...)
       ① descriptor_for(fd)；未跟踪且属 managed 场景 → adopt_socket（真 fcntl 设 O_NONBLOCK）
       ② 在 DescriptorGuard 门内执行真实 syscall
       ③ EAGAIN/EWOULDBLOCK 且 can_wait() → wait_for_io(manager,…)
            · 先注册 readiness（IOManager 登记 WaitNode + 后端 add_or_modify）
            · 再发布 deadline（TimerService，绑定同一 WaitNode）
            · 最后 park Fiber（Scheduler::park_io）
         没有 IOManager → native_wait（有界原生 poll 切片，10 ms）
  → poller 线程 epoll_wait 就绪 → process_events_batch → wake_nodes
       → 原子 outcome claim（readiness / timeout / cancel / close 四类竞争，只有一个赢家）
  → 以相同绝对 deadline 重试 syscall
```

- poller 是**独立线程**：不在 M、不在 sysmon（`IOManager::start()` → `State::start()` → `m_poller`）。
- **fd 代际**：`DescriptorToken{fd, generation, valid}` + `DescriptorGuard`（**禁止跨 park 持有**）；
  Hook 必须在真实 `close` 前 `NotifyClose`/`Invalidate`，否则数字 fd 复用会误唤醒旧等待（ABA）。

### 2.3 错误流（`error`）

```text
失败路径统一返回 ErrorPtr（shared_ptr<const Error>）：
  NewError(msg)                       → StringError（叶子）
  Wrap(cause, msg)                    → WrappedError；**Message() 返回整条链**（"outer: inner: root"）
  Join({e1, e2, ...})                 → JoinError（多原因图；Message 用换行连接；过滤空原因）
  Is(err, target)                     → 地址去重的迭代遍历（环安全）；命中靠 next.get()==target.get() 或 next->Is(*target)
  As<T>(err)                          → dynamic_pointer_cast 的迭代版本，异常转 nullptr
  WalkErrors(err, visitor)            → 插件式环安全遍历（visitor 返回 kStop 或抛异常 → false）
```

- **必须用身份比较**：`Wrap` 的文本是链，拿文本判断层级一定会错。
- 深错误链的 `Message()` 与**析构**都用显式工作表改为迭代，避免递归爆栈。
- 8 个进程级错误单例（Canceled/DeadlineExceeded/Channel*/BrokenPromise）依赖「同一对象」才能被 `Is` 命中；
  函数局部 static 在退出阶段析构会让 `Is(err, {})` 退化为「err 是否为空」（台账 X-31）。

---

## 3. 四类边界（图中「明确边界」块的展开）

| 边界 | 具体含义 | 证据 |
|---|---|---|
| **栈** | 固定 128 KiB 不增长；越界 = SIGSEGV = 进程立即死亡（无 handler、无 sigaltstack）。手动等价物：到阈值 `resume` 一个新 Fiber 换栈 | `fiber.hpp`、`benchmarks/fiber_stack_probe.cpp` 45 组实测 |
| **调度** | 协作式让出（无异步抢占）；未 Hook 的阻塞调用调度器看不见，必须显式 `BlockingRegion`；`BlockingRegion` 禁嵌套、禁跨 park | `scheduler.hpp`、`docs/design.md` |
| **协议** | 无 TLS、无 HTTP/2/3；`http::split_url` 强制 `http://`；协议层不做路由/鉴权/连接池/服务发现 | `http.hpp`/`websocket.hpp`/`sse.hpp` 头注释 |
| **控制流** | `panic/recover` 是显式状态协议（不展开栈）；`defer` 是作用域级；`recover` 的边界是 `thread_local` 深度（跨 Fiber 需调用方自律） | `control_flow.hpp`、`docs/compatibility.md` |

---

## 4. 与图的对应关系

| 图内元素 | 本文件章节 |
|---|---|
| 右侧/底部「运行流程」块 ①②③④⑤⑥ | §1 阶段 ①②③④⑤⑥ |
| 「五条不可违反的协议」块 | §2.1 取消 / §2.3 错误 / §2.2 IO / §2.2 fd 代际 / §3 栈 |
| 「关键数字」块 | §1 阶段 ②③⑥（队列上限、条带数、FiberBin）、`modules/G1` §5.1 |
| 「明确边界」块 | §3 四类边界 |
| 「图例」块 | `notes/nodes-edges.md` §5 |
