# concurrency 模块心智负担审查（只读）

- 仓库：`/UserData/CodexWorkSpace/Go2cpp`（WSL，非 `E:\CodexWorkspace\Go2Cpp`）
- 版本：`c2069ae`（2026-11-04）
- 范围：`include/go2cpp/concurrency/` + `src/concurrency/`，并覆盖它们实际依赖的 `include/go2cpp/sync.hpp`、`src/concurrency/sync.cpp`、`include/go2cpp/tools/go.hpp`
- 方法：只读代码走查 + 调用链对照；未构建、未运行、未改代码
- 立场：**不找「测试能不能过」，只找「真实业务第一次写会怎么踩」**

## 0. 结论先行

**统计：30 条危险点 —— 高 10 / 中 15 / 低 5。**

核心判断不是「实现有 bug」，而是三件事叠在一起，把心智负担放大：

1. **同一类失败有多种表达**：手动 Fiber 里，Channel 返回 `kInvalid`、`Mutex::Lock` 返回 `false`、`ConditionVariable::Wait` 返回 `false` 且保锁、`WaitGroup::Wait` 返回 `false`、`Select` 返回 `kInvalid`。五处五种，调用方无法写一个统一的错误分支。
2. **关键返回值默认可忽略**：`Channel::Send/Recv/Close`、`WaitGroup::Wait`、`Context::WithCancel` 都没有 `[[nodiscard]]`；关闭后发送只返回状态码，不 panic、不终止，于是「消息静默丢失」是默认结局。
3. **上下文默认值掩盖了执行环境**：`Channel(capacity=0)`、`Send(value)` 无超时、`go(fn)` 用进程级默认调度器 —— 在 fiber 里都对，换到普通线程/手动 Fiber/已 shutdown 的调度器上，行为从「正确」变成「永久阻塞 / 静默失败 / 悬空引用」，而 API 形状完全一样。

**非 fiber 上下文用 Channel/Mutex 的结论（重点回答）：**

| 上下文 | Channel cap=0 Send / 空 Recv | Mutex 竞争 |
|---|---|---|
| 纯 `std::thread` | 走 native futex，**真的阻塞 OS 线程**（`parking_condition.cpp:754-756` 返回 false → `sync.cpp:1003-1007` native 路径）。跨线程能工作；**单线程内先 Send 后 Recv 必自死锁**，因为没有别的 G 可跑 | 阻塞 OS 线程；无超时会永久卡住 |
| Scheduler worker 上的非受管代码 | 阻塞一台 M；未声明 `BlockingRegion` 时 sysmon **不会**补 M（`scheduler.hpp:279-280` 明确只观察已声明边界），该 P 被占住 → 吞吐塌陷，严重时全 runtime 停摆 | 同上，且持锁 park 检测默认只告警 |
| 手动 `Fiber`（`resume()`，未绑 Scheduler） | **不阻塞，直接失败**：`FiberWaitUnsupported()`（`parking_condition.cpp:754-756`）为真 → `Send` 返回 `kInvalid`（`channel.hpp:2902-2904`）、`Select` 返回 `kInvalid`（`channel.cpp:530-534`）；Mutex 返回 false（`sync.cpp:1615-1617`）、CondVar 返回 false（`sync.cpp:2406-2408`）、WaitGroup 返回 false（`sync.cpp:3067-3069`） | 返回 false / 抛 `logic_error`（`sync.hpp:390-392`），无统一错误码 |

所以真正的坑不是「普通线程里不能用」，而是：**API 不告诉你当前在哪种上下文，失败也不告诉你为什么失败**。用户看到 `false` 会写重试，看到 `kInvalid` 会当参数错误，而真实原因是「这个执行环境根本没有 park 能力」。

---

## 1. 危险点清单

格式：`[级别] file:line — 症状 / 为什么 / 改造建议`。所有行号取自 `c2069ae`。

### A. Channel

**D1 [高] `include/go2cpp/concurrency/channel.hpp:1371-1376`（默认容量 0）+ `src/concurrency/parking_condition.cpp:754-756`**
- 症状：`Channel<int> ch;` / `Channel<int>::Create()` 默认无缓冲。在普通线程里 `ch.Send(1)` 会一直阻塞到有接收者；如果紧接着在同一个线程 `ch.Recv()`，**永久死锁**。在 worker 线程的非受管回调里调用，会占死一台 M。
- 为什么：Go 的 `make(chan T)` 也是 0 容量，但 Go 的 send 阻塞时 runtime 会切到别的 goroutine；C++ 普通线程没有「别的 G」。`ParkingCondition` 的 native 路径是忠实的 `futex` 等待，不会让出任何调度单位。
- 建议：保留 0 容量（Go 一致），但把「在 native 上下文调用会阻塞线程」变成显式选择：`Send` 在 `FiberWaitUnsupported()` 为真时返回统一错误码；新增 `SendBlocking()` 表示「我知道会阻塞线程」；编译期/运行期提供 `ExecutionContext::Current()` 诊断。
- **状态（2026-11-04，P1 H-10）：可观测性已修。** `ChannelWaitNative` 在原生线程首次真正 futex 入睡前打一条 WARN（每线程一次），提示「会卡住当前线程」及 Fiber/Try*/超时替代方案；新增 `Scheduler::InBlockingRegion()`，显式 `BlockingRegion`（普通线程也计入）内的阻塞不告警。`SendBlocking` 与统一错误码属 API 变更，未做。

**D2 [高] `channel.hpp:2780-2782` + `channel.hpp:468-484`（无 `[[nodiscard]]`）**
- 症状：`ch.Close(); ch.Send(v);` 不抛、不终止，只返回 `{kClosed, ChannelClosedError()}`；调用方写 `ch.Send(v);` 不接返回值，消息**静默丢失**。Go 在这里是 `panic: send on closed channel`。
- 为什么：把「用错生命周期」降级成「普通返回值」，而返回值又能被丢弃；`ChannelSendResult::operator bool` 只在 `kReady` 为真，`kClosed` 为 false，但没人检查。
- 建议：`Send/SendFor/TrySend/Recv/Close` 全部加 `[[nodiscard]]`；debug 构建对 closed-send 直接 `std::terminate`（模拟 Go panic），release 返回错误码；提供 `GO2CPP_CHANNEL_PANIC_ON_CLOSED_SEND` 开关。

**D3 [高] `channel.hpp:2902-2904`、`channel.hpp:3286-3288`、`src/concurrency/channel.cpp:530-534`**
- 症状：手动 Fiber 里 `Send`/`Recv`/`Select` 返回 `kInvalid`，与「参数错误/值复制失败」共用同一个码（`channel.hpp:2804/2808` 也返回 `kInvalid`）。用户无法区分「环境不支持」和「我传错了」。
- 为什么：`kInvalid` 是垃圾桶状态；`ChannelStatus` 枚举没有 `kUnsupported`。
- 建议：新增 `kUnsupportedContext`，所有阻塞原语统一用它；`Select` 的 `kInvalid` 同步拆分；错误信息里带 `Fiber::Current()!=nullptr && !Scheduler::current_task()` 的事实。

**D4 [高，待确认] `channel.hpp:1375`（构造函数 public）+ `channel.hpp:1385-1391`（析构里 `Close()`）**
- 症状：允许 `Channel<T> ch(0);` 栈上构造。若另一个线程/G 正阻塞在该 channel 的 `Send/Recv` 上，`~Channel()` 持锁 `Close()` 唤醒等待者后释放内存；等待者被唤醒后回到 `SendUntil` 的 `for(;;)` 重新 `lock.lock()`，访问的已经是释放后的 `m_lock/m_closed` → UAF。`Create()` 的 shared_ptr 用法没有这个问题。
- 为什么：`Channel` 继承 `enable_shared_from_this`（`channel.hpp:1353`），但 `Send/Recv` 从不调用 `shared_from_this()` 延长生命周期；等待节点也不持有 Channel 强引用。栈对象因此没有「等待者还活着」的保护。
- 建议：构造函数改 private，只保留 `Create()`；或所有等待节点持 `weak_ptr` 并在 park 前 `lock()` 失败即返回。当前至少要在类注释里写「必须 shared_ptr 持有」，并用 ASAN 复现确认（本条标待确认）。

**D5 [中] `channel.hpp:2724`（注释「失败时可能已被移动」）**
- 症状：`Send(std::move(v))` 返回 `kTimedOut` 后，调用方按直觉重试 `Send(std::move(v))`，实际发送的是被移出的空壳。
- 为什么：`SendUntil` 在入队前就 `node->value.emplace(std::move(value))`（`channel.hpp:2908-2910`），失败路径不回滚 `value`。
- 建议：失败时保持原值（先 `optional` 暂存，提交成功才 move）；做不到就在签名上改成 `Send(T&&)` + 文档强约束，并提供 `SendCopy`。

**D6 [中] `channel.hpp:1166-1169` `ValueOrDefault()`**
- 症状：`ch.Recv().ValueOrDefault()` 在 channel 已关闭、缓冲区为空时返回 `T{}`，和「收到一个零值」完全不可区分。Go 的 `v, ok := <-ch` 强制两个返回值。
- 为什么：为了模拟 Go 的 `<-ch` 单赋值，却把 `ok` 丢了。
- 建议：删除或标 `[[deprecated]]`；保留 `Recv()` 的 `ok/status` 双检查；提供 `RecvOr(T fallback)` 明确命名。

**D7 [中] `channel.hpp:1707-1718` + `channel.hpp:474-477` + `channel.hpp:130-132`**
- 症状：重复 `Close()` 返回 `kAlreadyClosed`，但 `ChannelSendResult::operator bool` 只认 `kReady`（`channel.hpp:483`）→ `if (ch.Close())` 为 false，调用方把幂等成功当失败；`ChannelStatusIsReady` 也不把 `kAlreadyClosed` 当 ready。
- 为什么：`Close` 的文档说「kAlreadyClosed 不要当错误」，但类型系统的默认行为与文档相反。
- 建议：`Close()` 改成返回 `enum class CloseResult { kClosed, kAlreadyClosed }`，或让 `operator bool` 对 `kAlreadyClosed` 返回 true；至少加 `[[nodiscard]]`。

**D8 [中] `channel.hpp:1466-1496` `TrySendBatch`**
- 症状：返回 0 同时表示「缓冲满」「已关闭」「有等待中的接收者」三种情况；调用方无法决定是重试、放弃还是回退单条。
- 为什么：批量 API 为了性能只返回数量，丢掉了原因。
- 建议：返回 `{count, status}` 小结构；或在 0 时提供 `LastBatchStatus()`（线程局部）——不推荐，仍是隐式状态。

**D9 [低] `channel.hpp:1702`**
- 症状：`Close()` 注释写「不可重入本通道的 Send/Recv（会自锁）」，但没有任何运行期检测；在关闭回调里顺手 `Send` 会直接死锁。
- 建议：debug 构建持锁时记录 owner 线程/G，重入即 assert；至少把这条写进 `Close` 的 Doxygen `@warning`。

**D10 [中] `channel.hpp:1548`、`channel.hpp:1618`**
- 症状：`Send(value, context = {})` / `Recv(context = {})` 默认无超时、无取消。普通线程上就是无限阻塞；fiber 里则会永久 park 一个 G（除非外部 cancel）。
- 为什么：默认参数让「无限等待」成为最省事的写法。
- 建议：删除默认参数，强制调用方显式传 `ContextPtr{}` 或 `SendFor`；或提供 `Send` 的 `[[deprecated]]` 无超时重载 + `SendOrCancel`。

### B. Context

**D11 [高] `include/go2cpp/concurrency/context.hpp:230-239` + `src/concurrency/context.cpp:2527-2533`**
- 症状：`WithCancel/WithTimeout` 返回 `pair<ContextPtr, CancelFunc>`，没有 `[[nodiscard]]`、没有 RAII 包装。`auto [ctx, _] = Context::WithTimeout(...)` 丢掉 cancel 后：child 的 `State` 被丢弃的 `CancelFunc` 捕获的 `shared_ptr` 保活（`context.cpp:2531-2532`），`WithDeadline` 注册的 timer 保留到到期（`context.cpp:2577-2611`）；取消无法提前传播，资源晚释放。Go 有 `go vet lostcancel`，这里没有。
- 为什么：与 Go 同形的 pair，但 C++ 没有 `defer` 文化，也没有静态检查；`CancelFunc` 是 `std::function`（`context.hpp:54`），拷贝后更难追踪。
- 建议：主 API 改成 move-only 的 `Canceler`（析构自动 `cancel()`，提供 `Detach()` 保留 Go 风格）；工厂函数加 `[[nodiscard]]`；给 `CancelFunc` 加 `[[nodiscard]]` 不够，必须 RAII。

**D12 [中，待确认] `src/concurrency/context.cpp:920-921`、`993`、`1193-1201`**
- 症状：父取消沿 `children` **在调用 cancel 的线程上同步级联**；深树或某个 child 的 `before_done` 回调耗时长时，`cancel()` 本身会长时间阻塞，甚至阻塞一个正在持有锁的 G。
- 为什么：同步传播换取「cancel 返回即全部子树已取消」的强语义；Go 也是同步传播，但 Go 的 cancel 回调通常很短。
- 建议：文档明确「cancel 回调必须是短、非阻塞、noexcept」；debug 构建对回调耗时做告警；提供 `CancelAsync()` 用于深树。

**D13 [低] `context.hpp:225-227`**
- 症状：`Background()` 与 `TODO()` 都返回「永不取消的根」，行为完全相同；误把 `TODO()` 当占位丢进生产代码没有任何提示。
- 建议：`TODO()` 在 debug 构建里对 `Err()`/`Deadline()` 打一次 warn；至少把两者语义差异写进注释（目前没有差异）。

**D14 [中] `context.cpp:2577-2598`**
- 症状：`WithDeadline` 注册 TimerService 失败（`id == 0`，TimerService 正在停止）时，直接把 child 标成 `DeadlineExceeded` 并返回。调用方拿到的是一个「已经超时」的 context，而不是「定时器不可用」的错误。
- 为什么：把基础设施故障翻译成了业务超时。
- 建议：返回 `ContextPtr` 之外再给一个创建结果，或至少 `Err()` 的 cause 区分 `DeadlineExceededError` 与 `TimerUnavailableError`。

### C. Mutex / ConditionVariable / WaitGroup

**D15 [高] `src/concurrency/sync.cpp:1615-1617` vs `include/go2cpp/sync.hpp:390-392`**
- 症状：手动 Fiber 里 `Mutex::Lock()` 返回 `false`（与「context 取消」同一个值），而 `mutex.lock()`（BasicLockable 拼写）却抛 `std::logic_error`。同一失败，两种表达。
- 为什么：`Lock()` 用 bool 表示「拿到没拿到」，`lock()` 为了满足 BasicLockable 契约只能抛。
- 建议：`Lock()` 返回 `LockResult { kAcquired, kCancelled, kUnsupported, kError }`；`lock()` 只在 `kUnsupported/kError` 抛；两者共享同一判定。

**D16 [高] `sync.cpp:2406-2408`（CondVar）、`sync.cpp:3067-3069`（WaitGroup）**
- 症状：手动 Fiber 里 `Wait()` 返回 false。CondVar 还保持 mutex 已持有（`abort_wait(true)`），WaitGroup 则直接返回 false —— 调用方无法知道「是没等到，还是环境不支持」，很容易写成 `while (!wg.Wait()) {}` 死循环。
- 为什么：bool 承载不了三种原因（通知/取消/不支持）。
- 建议：同上，统一 `WaitResult` 枚举；`WaitGroup::Wait` 文档必须写清 false 的所有可能。

**D17 [高] `sync.cpp:1003-1007` + `scheduler.hpp:279-280`**
- 症状：同一段 `mutex.lock()` 代码，在 fiber 里 park（让出 M），在普通线程里阻塞 OS 线程；若这个普通线程是 worker M 且没有 `BlockingRegion`，sysmon 看不到，不会补 M，P 被占死。
- 为什么：混合等待是特性，但「什么时候会占 M」对调用方不可见；`BlockingRegion` 需要手动声明（`scheduler.hpp:893-896`）。
- 建议：`BlockingRegion` 在 `Mutex/CondVar/Channel` 的 native 阻塞路径自动进入（RAII），而不是要求用户记得包；或至少 debug 构建在 native 阻塞超过阈值时打印「你正在阻塞一个 worker M」。

**D18 [中] `sync.hpp:280-284` + `sync.hpp:377-379`**
- 症状：Mutex 允许跨 G/线程解锁（Go 风格），而 C++ 用户按 `std::mutex` 直觉在同线程解锁；owner 检查默认只在 `held_park_detection` 开启时做，且只检测「持锁 park」，不检测「非 owner 解锁」。
- 为什么：语义向 Go 对齐，但错误检测向性能让步。
- 建议：debug 构建强制 owner 校验（记录 owner gid/tid），release 保持零开销；文档在 `Unlock` 上写「不要求同线程，但要求逻辑 owner」。

**D19 [中] `sync.cpp:2956`、`sync.cpp:2963`**
- 症状：`WaitGroup::Add` 负数/溢出抛 `std::logic_error`；在 fiber 里异常会从任务入口逃逸（Task 捕获后置 Failed），但调用方往往只在主线程 catch。
- 为什么：Go 的 `Add` 负数 panic 是故意快速失败；这里用异常，跨越了 fiber 边界。
- 建议：保留异常，但在 `Task` 的 failure 里带原始 `what()`；或提供 `TryAdd` 返回 bool。

**D20 [低] `sync.hpp:283-284`**
- 症状：类注释写「对象必须长于所有使用者和等待者」，但栈上 `Mutex` 传给另一个 fiber 是常见写法；一旦提前析构就是 UAF，没有运行期检测。
- 建议：debug 构建在析构时检查 `waiter_count == 0`（已有内部计数），非零即 assert/日志。

### D. Scheduler / go()

**D21 [高] `include/go2cpp/tools/go.hpp:137-139` + `go.hpp:174-192`**
- 症状：`Scheduler& s = default_scheduler();` 保存引用后调用 `shutdown_default_scheduler()`：shared_ptr 被移出，若没有其他持有者，Scheduler 立即析构，`s` 变成悬空引用；下一次 `s.spawn(...)` 是 UB。文档只说「不要继续使用旧引用」，没有编译期或运行期保护。
- 为什么：返回引用是为了「像全局对象一样用」，但默认调度器的生命周期是「可被 shutdown 替换」的。
- 建议：`default_scheduler()` 返回值改成 `std::shared_ptr<Scheduler>`（与 `default_scheduler_handle()` 合并），或返回带 `generation` 的 `SchedulerHandle`，`shutdown` 后 `handle.alive()` 为 false；删除引用重载。

**D22 [高] `go.hpp:201-206` + `src/concurrency/scheduler.cpp:3824-3846`**
- 症状：对已经 `shutdown()` 的实例调用 `go(s, fn)`：`is_running()` 为 false → `start()`（对终态是空操作）→ `spawn()` → `enqueue()` 失败 → `task->cancel()`，**返回一个非空但永远不会运行的 Task 句柄**。调用方通常只判断 `if (!task)`，于是任务静默消失。`go.hpp:199` 的注释「提交失败返回空指针」与实现不符。
- 为什么：`spawn` 的注释（`scheduler.cpp:3817`）说「让调用方通过 `task->state()` 看到终态」，但 `go()` 的签名和文档把「非空」暗示成「已提交」。
- 建议：`go()` 返回 `Result<Task>` 或 `std::optional<Task>`；或 `spawn` 失败返回 nullptr；至少把 `go.hpp:199` 的注释改成「返回非空也可能是已取消任务，必须查 `state()`」。

**D23 [中] `scheduler.hpp:865-880` + `scheduler.cpp:3361-3377`**
- 症状：显式 `Scheduler s(N)` 与默认调度器并存时，只打一条 stderr + 一条 WARN，然后**继续启动**。两个实例各自有 worker/sysmon；跨实例的 channel 能通（等待节点各绑各的 scheduler），但 `Task` 句柄混用、`yield/park` 用错实例无法检测，线程数翻倍。
- 为什么：为了旧代码渐进迁移，选择「告警不阻止」。
- 建议：默认构建直接 `std::terminate()`（或 `start()` 返回错误），要求显式定义 `GO2CPP_ALLOW_MULTI_SCHEDULER` 才放行；告警里打印两个实例的创建栈（debug）。

**D24 [中] `scheduler.hpp:968` + `go.hpp:174-192`**
- 症状：`shutdown()` 是无界等待；`shutdown_default_scheduler()` 是 `noexcept` 并吞掉异常。一个有 `while(true)` 且忽略取消的任务会让进程退出永久卡住，且没有任何超时/诊断。
- 为什么：与 Go 的「main 返回直接杀 goroutine」相反；这里选择「优雅排空」。
- 建议：`shutdown_default_scheduler()` 内部用 `shutdown_for(5s)`，超时后打印未完成任务列表并 `std::terminate`；提供 `shutdown_default_scheduler_for(timeout)`。

**D25 [中] `scheduler.hpp:256-269` vs `fiber.hpp:284-286` + `fiber.cpp:38-41`**
- 症状：调度器任务默认 `fiber_stack_size=2048`（页对齐后 4 KiB 初始），直接 `Fiber` 默认 128 KiB；`GO2CPP_FIBER_GROWABLE_STACK` 只在 Linux x86_64 定义。growable 不生效时，调度器任务退化为 **4 KiB 固定栈**，一个稍大的栈对象/递归就 SIGSEGV；这比用户以为的「128 KiB 固定」更危险。
- 为什么：两个默认值来自不同层（Fiber 运行时 vs SchedulerConfig），没有统一；growable 的退化条件分散在宏和平台判断里。
- 建议：统一默认常量；growable 不可用时把固定栈下限提到 64–128 KiB 并打一次 warn；在 `Fiber` 文档里写清「默认 128 KiB」只对直接构造成立。

**D26 [低] `scheduler.hpp:922-927` + `go.hpp:202`**
- 症状：`start()` 对「已 started / stopping」是空操作；`go(Scheduler&)` 依赖 `is_running()` 自动 `start()`，但 shutdown 是终态，自动 start 不会复活实例，只会让后续 `spawn` 失败。
- 建议：`start()` 在 `stopped` 终态返回错误码/抛异常，而不是静默空操作；`go()` 先检查 `is_running() || !has_shutdown()`。

### E. Fiber / FiberLocal

**D27 [中] `include/go2cpp/concurrency/fiber_local.hpp:86-90` + `src/concurrency/fiber_local.cpp:94-116`**
- 症状：`FiberLocalCache` 析构不清理值（注释明确「值长于键对象」），key 单调递增、永不复用。于是把 `FiberLocal<T> x;` 写成函数局部变量（每次请求新建）时，每个实例都分配新 key，值一直留到 G 结束；长生命周期的 worker G 上内存单调增长，且没有回收接口。
- 为什么：key 复用会导致旧值串读，所以选择「永不复用」；但代价被转嫁给了误用者。
- 建议：把 `FiberLocal` 改成只能静态/长生命周期持有：构造函数私有 + `FiberLocal<T>::Global()`；或提供 `FiberLocalKey`（可复用）+ `FiberLocalSlot`（短生命周期）两层；debug 构建在进程退出时打印「仍然存活的 key 数」。

**D28 [中] `fiber_local.hpp:104-108`、`fiber_local.hpp:118-127`**
- 症状：同一个 `FiberLocal` 实例，在 Fiber 内读写的是「G 的映射表」，在 Fiber 外读写的是 `thread_local` 后备 map。同一个变量在两处是**两套值**；G 迁移到另一个 M 后 Fiber 内的值会跟着走（符合设计），但用户如果混用「Fiber 里写、普通线程里读」就会读到空。
- 为什么：为了让 Fiber 外也能用，退化成了线程局部；但 API 没有标注「你在哪一层」。
- 建议：`TryGet()` 在「Fiber 内不存在但线程后备存在」时 debug 告警；文档用表格写清两套存储；提供 `IsFiberBound()` 查询。

**D29 [中] `fiber.hpp:325-336`**
- 症状：`~Fiber()` 会请求取消并等待 Fiber 自然返回；忽略取消的主体（例如阻塞在 native `read()`）会让析构永久阻塞。超时路径（`fiber.hpp:330-335`）把 Impl 放进进程级隔离区，安全但泄漏。
- 为什么：析构承担了「停止 + 回收」两件事，且无法放弃。
- 建议：拆出 `RequestCancel()` 与 `JoinFor(timeout)`；析构只做「已终态才回收」，未终态直接 `std::terminate`（或隔离并计数），让误用尽早暴露。

**D30 [低] `fiber_local.cpp:79-92`**
- 症状：`Registry` 用 `new` 且不注册 atexit，故意泄漏一个空 map；配合 key 永不复用，进程生命周期内没有任何全局回收点。
- 建议：接受这个取舍，但在 `fiber_local.hpp` 顶部写明「这是有意的进程级泄漏」；提供 `DebugSnapshot()` 统计 Fiber 数/key 数，便于定位 D27。

---

## 2. Go 对照

| 场景 | Go 的默认语义 / 错误提示 | 本实现 | 心智负担差异 |
|---|---|---|---|
| `make(chan T)` | 无缓冲，send 阻塞；runtime 切到其他 goroutine | `Channel<T>(0)`，send 阻塞；fiber 里 park，普通线程里阻塞 OS 线程 | Go 的「阻塞」隐含「调度器还在跑」；C++ 普通线程没有这个前提 |
| close 后 send | `panic: send on closed channel`（立即、不可忽略） | 返回 `{kClosed, error}`，可被丢弃（`channel.hpp:2780`） | 从「崩溃」降级为「可能静默丢数据」 |
| close 两次 | `panic: close of closed channel` | 返回 `kAlreadyClosed`，`operator bool` 为 false | 幂等成功看起来像失败 |
| `v, ok := <-ch` | 强制两个返回值 | `Recv()` 有 `ok`，但 `ValueOrDefault()` 把它丢了 | 单返回值 API 诱导丢 `ok` |
| `context.WithCancel` | 返回 `(ctx, cancel)`；`go vet lostcancel` 静态检查；不 cancel 会泄漏 timer/子树 | 同样返回 pair；无静态检查；`CancelFunc` 捕获 `shared_ptr<State>`，timer 保留到到期 | C++ 没有 vet，漏 cancel 更常见 |
| `sync.Mutex` 在 goroutine 里阻塞 | runtime park M，P 可被其他 M 接管 | fiber 里 park；普通线程里阻塞线程；worker M 上可能占死 P | 「阻塞」的代价取决于上下文，API 不区分 |
| `sync.WaitGroup.Add` 负数 | panic | 抛 `std::logic_error`（`sync.cpp:2956/2963`） | 异常可能跨 fiber 边界，调用方未必 catch |
| `go f()` 与 main 退出 | main 返回直接杀所有 goroutine | `shutdown()` 无界等待；忽略取消的任务会卡住退出 | Go 的退出是「立即」，这里是「排空」 |
| `chan` 是引用类型 | 不存在「栈上 chan 被 close 后仍被引用」 | `Channel` 构造函数 public，栈对象可被等待者引用 | C++ 值语义放大了生命周期错误 |

---

## 3. 「心智负担最小」改造建议

### 3.1 默认值（让默认行为 = 安全行为）

1. **`Channel` 默认容量保留 0，但默认操作不再是无限阻塞**：`Send(value)` 无 context 时，在 native 上下文直接返回 `kUnsupportedContext`；要阻塞必须写 `SendBlocking(value)` 或 `SendFor(value, timeout)`。
2. **`Context` 工厂去掉「可丢弃的 pair」默认形状**：`WithCancel` 返回 `Canceler`（move-only、析构自动 cancel），`Detach()` 才退化成 Go 风格。工厂函数加 `[[nodiscard]]`。
3. **`Scheduler` 唯一性默认强制**：第二个 `start()` 默认 `std::terminate`，`GO2CPP_ALLOW_MULTI_SCHEDULER` 才放行；告警从「stderr 一行」升级为「终止 + 创建栈」。
4. **栈默认值统一**：`SchedulerConfig.fiber_stack_size` 与 `Fiber::DefaultStackSize()` 用同一个常量；growable 不可用时固定栈下限 64 KiB，并在启动时 warn。
5. **`go()` 默认调度器生命周期**：`default_scheduler()` 只返回 handle；`go(Scheduler&)` 在 `is_running()==false` 且已 shutdown 时返回 `Result` 错误，而不是自动 `start()` 后静默取消任务。

### 3.2 API 形状（让错误无法被忽略）

1. 所有可能失败的入口加 `[[nodiscard]]`：`Channel::{Send,SendFor,TrySend,Recv,RecvFor,TryRecv,Close}`、`WaitGroup::Wait`、`Mutex::Lock`、`Context::{WithCancel,WithTimeout,WithDeadline}`。
2. `Channel` 构造函数私有化，只留 `Create()`；等待节点持 `weak_ptr<Channel>`，park 前 `lock()` 失败即返回 `kChannelGone`。
3. 用 `Result<T>` / `Expected<T>` 替换 `{status, error}` + `operator bool` 的组合；`Close()` 单独返回 `CloseResult`。
4. `Mutex/CondVar/WaitGroup` 的失败统一成 `WaitResult { kNotified, kCancelled, kUnsupportedContext, kError }`；`lock()` 只在 `kUnsupportedContext/kError` 抛。
5. `FiberLocal` 改成「静态持有」形状：构造函数私有 + `FiberLocal<T>::Global()`；短生命周期场景用显式的 `FiberLocalSlot<T>`。

### 3.3 错误语义（让失败原因可区分）

1. `ChannelStatus` 增加 `kUnsupportedContext`、`kChannelGone`；`kInvalid` 只保留给真正的参数/值错误。
2. 关闭后 `Send`：debug 构建 panic（`std::terminate` + 明确消息），release 返回 `kClosed` 且 `[[nodiscard]]`；提供编译开关。
3. `WaitGroup::Wait`/`CondVar::Wait` 的 false 必须能区分「取消」「不支持」「错误」；文档逐个列出。
4. `WithDeadline` 的 timer 注册失败返回 `TimerUnavailable`，不要伪装成 `DeadlineExceeded`。
5. `go()`/`spawn()` 失败返回空或 `Result`，不要返回「非空但已取消」的句柄；`go.hpp:199` 的注释同步修正。

### 3.4 生命周期（让悬空/泄漏尽早暴露）

1. `default_scheduler()` 引用重载删除，统一返回 `shared_ptr` 或带代际的 `SchedulerHandle`；`shutdown` 后旧 handle 可检测。
2. `shutdown_default_scheduler()` 默认带 5s 超时；超时打印未完成任务并终止，不静默吞异常。
3. `Channel` 析构时若有等待者，debug 断言/日志；配合 `Create()` 强制 shared_ptr。
4. `Mutex/CondVar` 析构时检查 `waiter_count == 0`；`FiberLocal` 析构时 debug 打印本 key 残留值数量。
5. `Fiber` 拆出 `RequestCancel()`/`JoinFor()`；析构只回收已终态对象，未终态显式 terminate 或隔离并计数。
6. `Context` 的 cancel 回调强制文档化「短、非阻塞、noexcept」，debug 构建对超过 1ms 的回调告警。

---

## 4. 优先级排序

### P0（会导致静默丢数据、死锁或 UAF；本轮之后先改这些）

| # | 条目 | 验收标准 |
|---|---|---|
| 1 | D1 非 fiber 上下文的 Channel 阻塞 | 普通线程单线程 Send→Recv 在 1s 内返回明确错误，而不是挂死；worker M 上调用有告警 |
| 2 | D2 关闭后 Send 静默失败 | `Send` 加 `[[nodiscard]]`；debug 构建 closed-send 终止；新增单测覆盖「忽略返回值」编译告警 |
| 3 | D3/D15/D16 手动 Fiber 五种失败表达 | 五种原语统一返回 `kUnsupportedContext`；新增表驱动测试 |
| 4 | D21/D22 默认调度器悬空 + go() 返回已取消任务 | 保存旧引用并 shutdown 后调用必须编译失败或运行期报错；`go()` 在终态实例上返回空/错误 |
| 5 | D4 栈上 Channel 的 UAF | 构造函数私有；ASAN 复现用例转为回归测试 |
| 6 | D24 shutdown 无界等待 | `shutdown_default_scheduler()` 5s 超时并列出未完成任务 |

### P1（高频误用，第二波）

7. D27/D28 FiberLocal 临时实例泄漏与双份值。
8. D25 栈默认值不一致 + growable 退化 4 KiB。
9. D11 Context 漏 cancel（RAII + `[[nodiscard]]`）。
10. D23 多实例只 warn（默认终止）。
11. D5 Send 失败后值已移动。

### P2（可观测性/一致性，随重构做）

12. D6 `ValueOrDefault` 吞关闭信号。
13. D7 `kAlreadyClosed` 的 `operator bool` 反向。
14. D8 批量 API 丢失失败原因。
15. D12 cancel 回调耗时无告警。
16. D18/D19/D20 生命周期与 owner 检测。

---

## 5. 待确认项

1. **D4（栈上 Channel 的 UAF）**：需要 ASAN/TSAN 复现「一个线程阻塞在 Send，另一个线程析构栈上 Channel」。代码路径成立（`Close()` 唤醒后等待者重入 `SendUntil`），但没有实测，标「待确认」。
2. **D12（cancel 同步级联的阻塞上限）**：没有测量回调耗时，深树场景的 p99 未知。
3. **D25（growable 退化路径）**：`GO2CPP_FIBER_GROWABLE_STACK` 只在 `__linux__ && __x86_64__` 定义（`fiber.cpp:38-41`）；WSL 上默认生效，但发行构建若关掉 `GO2CPP_GROWABLE_STACK=0`，调度器任务就是 4 KiB 固定栈。需要确认发布构建的宏配置。
4. **D27（FiberLocal 临时实例）**：需要确认项目内是否已有「函数局部 `FiberLocal`」的用法；如果有，泄漏是现实而非理论。
5. **D22（`go()` 注释与实现不一致）**：`go.hpp:199` 写「提交失败返回空指针」，`spawn` 实际返回已取消的非空句柄；需确认是否有意为之（迁移期兼容），还是文档笔误。

---

## 6. 建议的下一步（按时间盒）

1. 先做 P0 的 1/2/4/6：这四条都是「改 API 形状 + 加断言」，不动核心调度算法，风险低、收益直接。
2. 给 D3/D15/D16 加一张表驱动测试（五种原语 × 三种上下文），把「失败表达」固定下来，再改实现。
3. D4/D27/D25 需要复现/测量，放到第二轮；D4 优先用 ASAN 验一次。
4. 报告中的所有行号在改动后会漂移，改完 P0 后重新生成一次基线。
