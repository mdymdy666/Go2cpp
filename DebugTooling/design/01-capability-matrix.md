# design · 01 能力矩阵（第 2 轮）

> 把使用者的每条需求映射为「现有能力（源码证据）/ 缺口 / 需要的新机制」。
> 本文件只写**已读到源码并确认**的内容；未读到的标 **未确认**。

## 0. 一句话结论

项目**已经具备**一套完整的**只读观测**基础设施（Fiber 编号、状态、挂起原因、G/M/P 绑定、
调用栈快照、调度器快照与指标、事件观察者），使用者要求的"看得见"部分**基本已就绪**；
真正缺的是 **①元数据（函数名/行号/描述）②运行时变更控制面（替换/迁移/插队/断点/锁归属）
③ dump 的序列化与 CLI ④远程与多机通道**。

## 1. 「看得见」的部分：已有能力（逐条给证据）

### 1.1 Fiber 的身份、状态与原因

| 能力 | 证据 | 说明 |
|---|---|---|
| Fiber 唯一编号 | `fiber.hpp:199-201` `std::uint64_t id() const noexcept` | 注释原文：「Fiber 的唯一调试编号。编号只在当前进程内有意义，**不复用**」 |
| Fiber 生命周期状态 | `fiber.hpp:14-20` `enum class FiberState { Ready, Running, Suspended, Completed, Failed }` | 5 态 |
| 最近一次挂起原因 | `fiber.hpp:23-30` `enum class SuspendReason { None, Yield, Park, Io, Timer, Synchronization }` | 直接回答"为什么挂起" |
| G 的调度状态 | `scheduler.hpp:47-66` `GState { kNew, kRunnable, kRunning, kWaiting, kDead, kCancelled, kFailed }` | 7 态，且**带 Go 风格别名** |
| 取消标记 | `fiber.hpp:160-166` `CancellationRequested()`；`FiberContextFrame::cancellation_requested` | 协作式取消 |

### 1.2 Fiber 与 G/M/P 的绑定

```cpp
// fiber.hpp:36-48
struct FiberExecutionBinding {
    std::uintptr_t scheduler_token{0};
    std::uint64_t  task_id{0};
    std::uint64_t  machine_id{0};
    std::uint32_t  processor_id{0};
    bool           managed{false};
};
```

注释原文：「Fiber 当前执行位置所对应的调度器元数据。这里**只保存不拥有对象的标识**，
避免 Fiber 模块依赖 scheduler 模块，也避免调度器生命周期被调试信息反向延长。」

- 读取：`Fiber::execution_binding()`（`fiber.hpp:215`）、`Fiber::bind_execution()`（`217`）；
- 全局发布：`Fiber::BindCurrentExecution(binding)`（`179-180`）；
- 调度器侧对应物：`Scheduler::current_task()` / `current_scheduler()` / `current_machine_id()` /
  `current_processor_id()`（`scheduler.hpp:607-610`，注释：「这些值**只用于观测，不参与所有权**」）。

**判定**：G→M→P 的映射**可直接用于可视化**；但它是**观测值**，不是控制接口。

### 1.3 调用栈（含墓碑帧）

```cpp
// fiber.hpp:54-67
struct FiberContextFrame {
    std::uint64_t id{0};
    std::uint64_t parent_id{0};          // 首次进入时固定的逻辑父级
    std::uint64_t active_parent_id{0};   // 最近一次 resume 的实际调用者
    std::size_t depth{0};
    bool main_fiber{false};
    bool alive{false};
    bool active{false};
    bool cancellation_requested{false};
    FiberState state{FiberState::Ready};
    SuspendReason suspend_reason{SuspendReason::None};
    std::thread::id last_thread{};
    FiberExecutionBinding execution{};
};
using FiberContextSnapshot = std::vector<FiberContextFrame>;
```

读取入口：`Fiber::debug_info()`（`fiber.hpp:204`）、`Fiber::context_snapshot()`（`210`）、
`Fiber::CurrentContext()` / `CurrentContextSnapshot()`（`171/173`）、`nesting_depth()`（`221`）。

注释原文（`206-208`）：「返回从本 Fiber 向上追溯的逻辑调用链。Fiber 被迁移或挂起后仍保留
首次进入时的父级关系；**父对象结束后由共享记录保留 `alive=false` 的墓碑帧**。」

**判定**：使用者要求的「Fiber 的调用栈（可选）」**已有**，且比预期更完整（区分
逻辑父级与活动父级、支持墓碑）。**缺的是函数名/行号**（见 §2.1）。

### 1.4 调度器全局与分片快照

| 需求（使用者原文） | 现有接口 | 证据 |
|---|---|---|
| 「现在多少 Fiber 了」 | `runnable_count()`、`global_runnable_count()`、`worker_count()`、`processor_count()` | `scheduler.hpp:582-591` |
| 「G1Processeer infos」 | `std::vector<ProcessorSnapshot> processors()` | `scheduler.hpp:592`；结构见 `210-214`（`id`/`state`/`queued`） |
| 「M 的信息」 | `std::vector<MachineSnapshot> machines()` | `scheduler.hpp:593`；结构见 `217-233` |
| 阻塞/长系统调用诊断 | `MachineSnapshot::processor_detached` / `blocking_task` / `long_syscall_count` | `scheduler.hpp:229-232` |
| 指标 | `SchedulerMetrics metrics()`（`task_runs`/`task_completions`/`fiber_resume_ns`/`local_queue_pops`/`steal_pops`） | `scheduler.hpp:236-243, 599` |
| sysmon 活性 | `sysmon_running()` / `sysmon_pass_count()`（注释：每周期递增，**只用于活性观测，不参与调度决策**） | `scheduler.hpp:595-598` |

### 1.5 事件流与观察者插件

- `SchedulerObserver::OnEvent(const SchedulerEvent&)`（`scheduler.hpp:173`），
  9 类事件（`103-120`）：`kStarted`/`kStopping`/`kWorkerStarted`/`kWorkerStopped`/
  `kTaskStarted`/`kTaskSuspended`/`kTaskCompleted`/`kTaskFailed`/`kTaskCancelled`；
- `SchedulerEvent` 同时携带 `task_id`/`machine_id`/`processor_id`/`task_state`/`timestamp`（`122-138`）；
- **可在运行期替换**：`set_observer()` / `observer()`（`scheduler.hpp:463-465`），
  注释：「替换只影响后续事件，已经发出的事件**不会回放**；回调在调度器内部**锁外**执行，
  观察者可以安全地把数据转发到独立队列」。

**判定**：这**就是**使用者要的「插件式」观测点，可直接承载 stdio/文件/topic 等多种后端。

## 2. 缺口（需要新增机制）

### 2.1 元数据：函数名 / 行号 / 描述 —— **完全没有**

- `Task` 只保存 `Function m_function`（`std::function<void()>`，`scheduler.hpp:362`，**私有**）；
- `FiberContextFrame` 有 `id`/`depth`/`state`… 但**没有任何名字、描述、文件、行号字段**；
- `SchedulerConfig` / `TaskOptions` 里也没有元数据字段（`161-197`、`201-208`）。

**影响**：使用者举的第一例子（"我需要知道任务信息（比如 `s_main_task` 名字、fiber 编号、
函数行号、描述）"）中，**只有 fiber 编号现成可用**，其余三项需要新增。

**可能的注入点（待第 3–5 轮读码后确认，不在本文件下结论）**：
转译期注入（Go2Cpp 是 Go→C++ 转译器，符号信息在转译时可得）／`TaskOptions` 扩展／
独立的调试符号表（`task_id → {name, file, line, description}`）。

### 2.2 运行时变更控制面 —— **Observer 明确不能承担**

`SchedulerObserver` 注释原文：「观察者不拥有 Scheduler，也**不能通过事件回调控制当前 G 的执行权**」。
`Scheduler::BlockingRegion` 注释（`409-412`）补充：「它**不从别的线程强行破坏调用栈**，
不能跨 Fiber yield/park，也不能跨迁移当前 G 的调用」。
`SchedulerConfig::enable_sysmon` 注释：「只观察已声明的 BlockingRegion/Hook 边界，
**不会从另一个线程强行切断任意 C++ 调用栈**」。

| 使用者需求 | 现状 | 缺什么 |
|---|---|---|
| 替换 Fiber 的函数 | `m_function` 私有，无 setter；`release_callable()` 私有（`356`） | 受控的替换接口 + **安全边界**（只能对未运行的 G 生效？） |
| 跨 M 迁移 | `FiberExecutionBinding` 是"不拥有的标识" | 迁移接口 + 安全边界（运行中的栈不能迁移） |
| 插队到指定 Fiber 之前 | 有 `enqueue`（`492`）与后端 `try_mark_queued*`（`301-308`），但**无按位置插入** | 队列的受控重排接口 |
| 强制暂停 / 断点命中 | `Fiber::Suspend()` 是**协作式**（Fiber 自己调用，`fiber.hpp:156`） | 外部触发的暂停机制（需解决"不能强行切断 C++ 调用栈"的约束） |
| 锁争用决定归属 | 见 §2.3 | 见 §2.3 |

### 2.3 锁：现有的是**自旋策略**，不是**等待者选择**

`core/hybrid_mutex.hpp`（114 行）全文已读：

```cpp
struct HybridGateOptions { std::size_t fast_attempts{64}; };
class HybridGate final {
    void lock() noexcept;    // 先自旋 fast_attempts 次，再进 std::mutex 慢路径继续自旋
    void unlock() noexcept { m_fast.clear(std::memory_order_release); }
    static void SetDefaultOptions(HybridGateOptions) noexcept;   // 可运行期修改默认策略
    static HybridGateOptions DefaultOptions() noexcept;
};
```

- **可插拔的只有"自旋次数"**（`fast_attempts`，默认为 64，可用 `SetDefaultOptions` 运行期改）；
- 它**没有等待者队列**，因此**没有"选择哪个等待者获得锁"的钩子**；
- 注释限定了使用范围：「只用于**非常短**的 Fiber 唤醒/撤销临界区，**不得**保护可能阻塞、
  执行用户回调或执行系统调用的代码」（`hybrid_mutex.hpp:3-5`）。

**第 3 轮已确认**（`include/go2cpp/sync.hpp` 219 行 + `src/concurrency/sync.cpp` 关键段）：

- 用户可见锁是 `sync::Mutex`（`sync.hpp:78`），语义为「**FIFO 混合互斥锁**」：
  「两类调用者共享一个 FIFO 交接队列，因此 Fiber 加锁后可以由其他 Fiber 或 OS 线程解锁」，
  且「Unlock **不要求**由同一 G/线程调用，采用 Go 风格所有权」（`sync.hpp:70-77`）；
- 等待队列确实存在：`std::deque<std::shared_ptr<WaitNode>> m_waiters`（`sync.cpp:686`），
  用 `shared_ptr` 的原因写在注释里：「Unlock 会先从队列取出等待者，再在释放队列锁后调用 Wake；
  如果队列只保存裸指针，等待方可能在这段窗口内返回并销毁栈上的 WaitNode」；
- **有一个官方可替换点**：`WaitBackend`（`sync.hpp:13-44`）与
  `SetWaitBackendFactory()` / `ResetWaitBackendFactory()`（`sync.hpp:46-66`、`sync.cpp:612/627`）。
  注释原文：「其他 Fiber 后端**可以注册工厂而不改动 Mutex/ConditionVariable**」，
  但「只影响**注册后创建的等待节点**，正在等待的 Fiber **不会迁移后端**」；
- **但"选谁获得锁"不可替换**：`WaitBackend` 只定义 `Park`/`ParkForCleanup`/`Wake`
  （即"怎么等/怎么唤醒"），FIFO 出队发生在 `Unlock` 路径；
- 现有争用策略是**硬编码且克制**的（`sync.cpp:711-760` 注释）：
  managed G 在「`processor_count() > 1`、无 native waiter、无 context」时做
  **有界协作重试（`kCooperativeAttempts = 256`）**，一旦出现普通线程等待者就把重试
  **限制为 4 次**并回到 FIFO handoff；**单 P 不走这条路径**（避免忙等住唯一的 M）。
  原文：「managed waiter 之间允许有限度地越过队列，以换取吞吐」。

**结论（对需求 D3「锁争用决策」）**：需要**新增**"选择等待者"的接口，且必须与上述既有策略共存，
不能简单替换成另一套公平性模型。

### 2.4 dump 的序列化与 CLI —— **完全不存在**

已确认存在的是**数据**（§1.4 的快照与指标），缺失的是：
- 没有 ini / json 序列化器；
- 没有任何 CLI（项目里未见 `dump` 子命令相关代码）；
- 没有"重定向到文件"的统一出口（项目规范要求"调试输出与日志要有统一入口，不散落裸 printf"）。

### 2.5 与 gdb 的协作、多机同步 —— **完全没有**

- 未见任何 gdb 脚本、线程/协程映射导出、符号表导出；
- 未见任何网络化调试通道（Stratum 式 topic 传输需要新设计）。

### 2.6 事件发射的锁边界（第 4 轮已确认）

`emit_event`（`src/concurrency/scheduler.cpp:1104-1122`）的实现：

```cpp
void emit_event(SchedulerEvent event) noexcept {
    if (event.timestamp == steady_clock::time_point{}) {
        event.timestamp = steady_clock::now();
    }
    SchedulerObserverPtr current;
    {
        std::lock_guard<std::mutex> lock(observer_mutex);
        current = observer;          // 先复制观察者
    }                                // 立刻释放互斥锁
    if (!current) { return; }        // 无观察者时零开销返回
    try { current->OnEvent(event); } catch (...) { }   // 锁外调用 + 异常隔离
}
```

注释原文：「复制观察者后再释放互斥锁，**绝不在调度器内部状态锁上执行外部代码**；
观察者异常被隔离，不能影响 worker 生命周期。」

**设计含义**：
1. 插件回调**在 worker 线程上同步执行** → 插件必须只做「快速入队」，
   由独立线程消费（这也正是 `set_observer` 注释建议的做法）；
2. 无观察者时开销极低（一次加锁 + 一次 `shared_ptr` 拷贝 + 判空）；
3. 事件发射点仅有 6 处（`scheduler.cpp:1939/1992/3503/3833/3848/4036`），
   而事件类型有 10 种 → 部分类型可能在同一处按 switch 分发，**第 5 轮需确认**。

### 2.7 锁的 FIFO 交接细节（第 4 轮已确认）

`Mutex::Unlock`（`src/concurrency/sync.cpp:955-1005`）：

- 无等待者时**一次 CAS** 即释放（`m_state: kLocked → 0`），不进队列锁；
- 有等待者时进入队列锁，**严格从队首取**，`TryFinish(kNotified)` 认领成功者即被选定，
  已取消/超时的节点被跳过（继续取下一个）；
- **选定的 waiter 使锁在逻辑上保持持有**，所有权直接交给队首；
  注释原文：「因而 **`TryLock` 调用者不能插队**」；
- 唤醒在**队列锁之外**执行（`selected->Wake()`）。

**对需求 D3（锁争用决策）的设计约束**：
策略钩子的形态应该是「**从等待队列中选谁**」，而不是「先释放锁再让所有人重新竞争」——
后者会破坏"所有权无缝转移、`TryLock` 不能插队"这一既有语义。

## 3. 需求 → 机制 的归属划分（本文件最重要的结论）

| 使用者需求 | 归属机制 | 现有基础 |
|---|---|---|
| 看 Fiber 编号/状态/挂起原因/所在 M/P/调用栈 | **只读观测** | ✅ 已具备，只需序列化 |
| 看调度器全局、P/M 快照、指标、事件流 | **只读观测** | ✅ 已具备（`SchedulerObserver` 可插拔） |
| `dump scheduler` CLI（ini/json，≥15 选项） | **只读观测 + 序列化层** | 数据齐、层缺 |
| 替换函数 / 跨 M 迁移 / 插队 | **运行时变更控制面** | ❌ 必须新建，且需安全边界 |
| 插件式断点（暂停/单步/命中） | **运行时变更控制面** | ❌ 必须新建（受"不能强行切断调用栈"约束） |
| 锁争用决定 Fiber 归属 | **运行时变更控制面**（与调度器协同） | ❌ 现有仅自旋参数 |
| 多机同步 / 远程调试 | **传输层插件** | ❌ 必须新建 |
| 与 gdb 混合调试 | **符号与线程映射导出** | ❌ 必须新建 |

**设计含义**：不能把"可视化"和"控制"混在一个接口里。观测侧可以做得很快、可插拔、
对性能几乎零影响；变更侧必须**默认关闭、显式启用、带前置条件校验**，并且要正视
现有注释反复强调的那条约束——**不强行切断任意 C++ 调用栈**。

## 4. 未确认事项（不推测，留待后续轮次）

1. `src/concurrency/sync.cpp`（1324 行）：用户可见锁是否有等待者队列与可替换策略；
2. ~~`context.cpp` 是否维护执行位置（行号）~~ —— **第 3 轮已确认：没有**。
   证据：`include/go2cpp/concurrency/context.hpp` 中检索 `line|symbol|source|debug|pc` 只命中
   无关的 `deadline`/`WaitUntil` 等；`grep -c "source_location|__builtin_return_address|dladdr|backtrace"
   src/concurrency/context.cpp` 的结果为 **0**。即 Fiber 运行时**不保存任何执行位置信息**，
   使用者要的「函数行号」必须由**新增机制**提供（转译期注入或独立符号表），
   不能指望从上下文里查出来；
3. `src/concurrency/scheduler.cpp`（4048 行）：`emit_event()` 的实际调用点与锁外保证；
4. 转译器（Go→C++）是否能提供函数名/文件/行号（决定元数据是转译期注入还是运行期注册）；
5. `Task` 的 `m_fiber`（`scheduler.hpp:364`）与 `Fiber::Impl` 是否留有可挂钩的调试位。
