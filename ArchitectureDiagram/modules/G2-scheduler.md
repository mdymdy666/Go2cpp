# G2 · GMP 调度器：`scheduler`（Task / M / P / worker_loop / sysmon / FiberBin）

> 依赖：`go2cpp_fiber`（G 的执行栈）、`go2cpp_log`（长时长/错误日志）。
> 规模（实测）：`concurrency/scheduler/scheduler.hpp` 662 行 + `src/concurrency/scheduler.cpp` 4048 行。
> 这是全项目最大的模块，也是「Go 的 GMP」在本项目的落点。

---

## 0. 一句话定位

`Task` 是 **G**（带状态机与唤醒令牌的句柄），`Impl::Machine` 是 **M**（`std::thread`），
`Impl::Processor` 是 **P**（本地运行队列 + attached 计数）。
调度器负责：入队/出队/窃取、park/wake 的原子交接、取消收敛、有界 shutdown、sysmon 观测。

---

## 1. 类型与 ID（★ 含一处对交接文档的实测修正）

```cpp
using GId = std::uint64_t;   // G（Task）编号
using MId = std::uint64_t;   // M（worker 线程）编号
using PId = std::uint32_t;   // P（处理器）编号
```

**★ 实测修正**：`src/concurrency/scheduler.cpp` 顶部是 `std::atomic<GId> s_next_g_id{1}`、
`std::atomic<MId> s_next_m_id{1}` —— **G 与 M 的编号从 1 开始**；
`thread_local MId t_machine_id = 0` / `t_processor_id = 0` 里的 **0 是「本线程不属于任何 M/P」的哨兵**。
只有 **P 从 0 开始**（`processors.emplace_back(static_cast<PId>(i))`，i = 0..P-1）。
`docs/HANDOFF.md` §1.2 写的「PId / MId 从 0 开始编号，0 是合法 ID」对 MId 不成立。

状态枚举：

```cpp
enum class GState { kNew, kRunnable, kRunning, kWaiting, kDead, kCancelled, kFailed, /* 别名 New/Runnable/... */ };
enum class MState { kIdle, kRunning, kBlocking, kParked, kStopping, kDead };
enum class PState { kIdle, kRunning, kStopping, kDead };
enum class WakeAction { kRejected, kEnqueue, kPending };
enum class ParkAction { kRejected, kParked, kRequeued };
```

合法 G 状态转换（`docs/design.md`）：

```text
new -> runnable -> running -> runnable | waiting | dead | cancelled | failed
waiting -> runnable | cancelled
failed/dead/cancelled -> terminal
```

配置与观测：

```cpp
struct SchedulerConfig {
  std::size_t processor_count{0};        // 0 → hardware_concurrency()（≈ GOMAXPROCS）
  std::size_t max_workers{0};            // 0 且允许超额 → 归一化为 2*P；关闭超额 → min(max_workers, P)
  std::chrono::milliseconds idle_wait{10};
  std::size_t local_queue_limit{256};    // P 本地队列上限，满了转全局
  std::size_t min_workers{0};            // 启动时创建的 worker 数（0 → 1 个）
  std::chrono::milliseconds idle_worker_timeout{250};
  std::size_t fiber_stack_size{0};
  std::size_t task_affinity_budget{4};   // 软亲和：在队列前 N 项内找同类任务
  bool allow_worker_oversubscription{true};
  bool enable_sysmon{true};
  std::chrono::milliseconds sysmon_interval{10};
  std::chrono::milliseconds long_syscall_threshold{50};
  bool pin_workers_to_cpu{false};
  std::size_t fiber_bin_capacity{32};
  bool collect_metrics{true};            // 关闭后 metrics 全为 0
  SchedulerObserverPtr observer;         // 生命周期事件观察者
};
struct TaskOptions { std::size_t stack_size{0}; TaskClassId task_class{0}; std::size_t fiber_bin_capacity{0}; };
struct ProcessorSnapshot { PId id; PState state; std::size_t queued; };
struct MachineSnapshot  { MId id; MState state; PId processor; TaskClassId last_task_class;
                          std::size_t affinity_hits, affinity_misses;
                          bool processor_detached; GId blocking_task; std::uint64_t long_syscall_count; };
struct SchedulerMetrics { std::uint64_t task_runs, task_completions, fiber_resume_ns,
                          local_queue_pops, steal_pops; };   // 全部累计值
```

---

## 2. `Task`（G）：状态机 + 一次性唤醒令牌

### 2.1 数据成员的语义（决定并发正确性）

| 成员 | 语义 |
|---|---|
| `m_state` (`atomic<GState>`) | G 生命周期；所有转换由 `m_transition_mutex` 或 claim 串行化 |
| `m_queued` | **入队权**（最多一个队列节点） |
| `m_execution_claim` | **执行权**（同一时刻只允许一个 M 进入该 Fiber） |
| `m_run_claim` | `run()` 内部的一次性执行 claim |
| `m_wake_pending` | 「运行中的 G 收到唤醒」的 pending 令牌，下一次 park/yield 消费 |
| `m_deferred_enqueue` | yield 快路径的延迟入队标记（由 worker 在 resume 返回后提交） |
| `m_registered` | 是否已发布到调度器注册表（决定走 `wake_registered` 还是完整 admission） |
| `m_cancel_requested` | 协作式取消标记（`Fiber::CancellationRequested()` 也查父链） |
| `m_transition_mutex` | `park_for_scheduler` 与 `wake_for_wait` 共用，防「park 读到 Running+pending=false」的丢失唤醒 |
| `m_completion_mutex` + `core::ParkingCondition m_completion_condition` | `wait()` / `notify_terminal()` 的完成通知 |
| `m_emergency_next` | **无分配救援链**（分配失败/关闭交接时使用，不形成环） |
| `m_owner_anchor` | 不透明 Scheduler 身份令牌（`bind_owner`/`owned_by`，**不延长 Scheduler 生命周期**） |
| `m_cancellation_gate` | `weak_ptr<TaskCancellationGate>`：避免裸 `Scheduler*` 在取消路径上悬空 |

`Task` 实现 `enable_shared_from_this`，不可拷贝。

### 2.2 公开 API（分两类）

**面向使用者**：`id()`、`state()`、`queued()`、`started()`、`cancellation_requested()`、
`last_processor_id()`、`task_class()`、`failed()`、`failure()`、`rethrow_failure()`、
`wait()`、`wait_for()`、`cancel()`，以及一批 CamelCase 别名（`Id/State/Join/Failure/...`）。

**面向调度器后端**（公开是为了可替换后端，不是给用户调）：
`try_mark_queued()`、`try_mark_queued_from_worker()`、`clear_queued()`、`defer_enqueue()`、
`consume_deferred_enqueue()`、`request_wake()`、`consume_wake()`、`try_register()`、`registered()`、
`prepare_enqueue(owner, gate)`、`bind_owner()`、`owned_by()`、`bind_cancellation_gate()`、`promote_new()`、
`wake_for_scheduler()`、`wake_for_io()`、`wake_for_wait()`、`park_for_scheduler()`、`try_mark_running()`、
`mark_runnable()`、`mark_yielded()`、`mark_yielded_fast()`、`mark_waiting()`、`cancel()`、
`cancel_if_runnable_unqueued()`、`run()`。

### 2.3 三个唤醒入口的返回值语义**不一致**（已知项 X-10，本轮复核确认）

| 入口 | `WakeAction::kPending` 的含义 | 返回 |
|---|---|---|
| `wake_for_scheduler()` | 未立即入队 | `false` |
| `wake_registered()` / `wake_or_cancel()` | 未立即入队（令牌待消费） | `false` |
| `wake_for_io()` | **已受理**（运行中的 G 会自行消费） | `true` |

头文件里只有 `wake()` 写了返回值说明，其余入口没有。调用方**不能**把返回值统一理解为「是否已入队」。

---

## 3. `Scheduler`：M/P 与队列

### 3.1 公开 API（分组）

```cpp
// 生命周期
explicit Scheduler(SchedulerConfig = {});  explicit Scheduler(std::size_t processor_count);
void start();  void shutdown();  bool shutdown_for(duration);  bool is_running() const noexcept;

// 观测者
void set_observer(SchedulerObserverPtr) noexcept;  SchedulerObserverPtr observer() const noexcept;

// 提交
std::shared_ptr<Task> spawn(Task::Function);  spawn(Function, TaskOptions);
core::Timer schedule_at(deadline, Function);  schedule_after(delay, Function);
core::Timer schedule_every(period, Function, TimerOptions = {});
std::shared_ptr<Task> go(Function);  bool enqueue(const std::shared_ptr<Task>&);
std::shared_ptr<Task> add(const std::shared_ptr<Task>&);  add(Function);  add(TaskLike);

// 让出/挂起（必须在拥有该 G 的 worker 上调用）
bool yield(const std::shared_ptr<Task>&);  bool park(const std::shared_ptr<Task>&);
bool park_wait(const std::shared_ptr<Task>&);            // 同步原语等待的轻量路径
bool park_wait_for_cleanup(const std::shared_ptr<Task>&); // 条件变量重锁阶段（忽略取消/draining）
bool park_io(const std::shared_ptr<Task>&);              // IOManager readiness 等待
bool yield_current();  bool park_current();

// 唤醒
bool wake(const std::shared_ptr<Task>&);  bool wake_registered(const std::shared_ptr<Task>&);
bool wake_io(const std::shared_ptr<Task>&);  bool wake_io_batch(const std::vector<std::shared_ptr<Task>>&);
bool wake_or_cancel(const std::shared_ptr<Task>&);  bool cancel(const std::shared_ptr<Task>&);

// 快照与度量
std::size_t processor_count/worker_count/runnable_count/global_runnable_count() const noexcept;
std::vector<ProcessorSnapshot> processors() const;  std::vector<MachineSnapshot> machines() const;
bool sysmon_running() const noexcept;  std::uint64_t sysmon_pass_count() const noexcept;
SchedulerMetrics metrics() const noexcept;

// 当前线程观测 / 阻塞区声明
static std::shared_ptr<Task> current_task() noexcept;   static Scheduler* current_scheduler() noexcept;
static MId current_machine_id() noexcept;               static PId current_processor_id() noexcept;
static bool enter_blocking() noexcept;                  static void leave_blocking() noexcept;
class BlockingRegion;   // RAII，禁拷贝禁移动
```

命名空间末尾提供短别名：`go2cpp::Scheduler/Task/Goroutine/Machine/Processor/GState/...`。

### 3.2 内部数据结构与取任务顺序

- `Impl::Processor`：`{id, mutex, deque<shared_ptr<Task>> queue, state, active_machines, running_machines}`。
- `Impl::Machine`：`{id, processor（绑定后不迁移）, std::thread, state, last_task_class,
  affinity_budget/hits/misses, processor_detached, blocking_since_ns, blocking_task, long_syscall_count}`。
- 全局队列：`Impl::global_queue`（受 `Impl::mutex`）。
- 外部生产者入口：**16 条 `incoming_queues` 条带**（`kIncomingStripeCount`）+ 非空位图；
  线程用函数局部 `static thread_local` 缓存条带号（已知项 X-11：跨 Scheduler 实例共享，只影响分布均匀性）。
- `worker_loop` 取任务优先级：
  1. `emergency_pop_locked()`（救援链，仅分配失败/关闭交接）；
  2. 本地 `processor.queue` **队首**（`take_local`；无总锁快路径）；
  3. `global_queue` 队首；
  4. 轮转扫描其他 P **窃取队尾**（`take_victim`，`queue.rbegin()`；`next_processor.fetch_add`，计入 `steal_pops`）。
- 软亲和：`TaskClassId last_task_class` + `task_affinity_budget` —— 在队列前 N 项内找同类任务；
  只有其他类别时才窃取（`affinity_hits/misses` 仅供观测）。

### 3.3 park / wake 的交接协议（与 `ParkingCondition` 的接口）

```text
ParkingCondition::WaitNode::WaitManaged()
        └─ Scheduler::park_wait(task)
              └─ Task::park_for_scheduler()      // transition_mutex：Waiting 或 pending 令牌
                    └─ Fiber::SuspendForScheduler(SuspendReason::Park)   // 真正切栈
等待者被唤醒：
WaitNode::Wake() → Scheduler::wake_or_cancel(task) → Task::wake_for_wait()
        ├─ Waiting → Runnable（立即入队，走 P 亲和 → incoming 条带）
        └─ Running → 置 m_wake_pending（运行中的 G 在下次 park/yield 消费）
```

要点：**park 与 wake 共用 `m_transition_mutex`**，所以不存在「park 读到 Running+pending=false → wake 置 pending → park 写 Waiting」导致的丢失唤醒；
跨 M 唤醒不接触 Fiber 栈，真正的 `resume` 仍由 execution claim 串行化。

### 3.4 sysmon（Go sysmon 的受限版）

- 单线程 `sysmon_loop`，周期 `sysmon_interval`（默认 10 ms），**主锁用 `try_to_lock`**：
  拿不到就置 `sysmon_retry_pending`，下一轮退避到 1 ms（保证心跳不被队列锁饿死）。
- 只做**可逆记账**：扫 `MState::kBlocking` 且 `now - blocking_since_ns >= long_syscall_threshold` 的 M →
  `processor_detached = true`、`long_syscall_count++`、从 P 的 `active_machines` 减 1、
  `maybe_grow()` 申请替代 M；M 返回时由 `leave_blocking_for()` 归还。
- **绝不**从别的线程切断 C++ 调用栈、终止 Fiber 或注入异常；
  未声明 `BlockingRegion` 的阻塞调用它观察不到。

### 3.5 生命周期与 shutdown

- M 惰性创建：`start()` 只创建 `min_workers`（默认 1）+ sysmon；
  `maybe_grow()` 按 `demand = runnable + running_workers`、`ceiling = min(max_workers, P + max(blocking, detached))` 扩。
- 扩容触发点：`enqueue()` 的维护票据（`&63`/`&1023` 采样）、`enter_blocking()`、`sysmon_pass`。
- `shutdown_for(timeout)`：`accepting=false` → `draining=true` → `begin_shutdown_locked()`
  （**已启动的 G 保留可运行**，让 trampoline 自然返回并跑栈上析构；未启动的就地取消并进 `deferred_destruction`）
  → 等 `all_tasks_terminal_locked()` → `stopping=true` → `stop_sysmon(true)` → 逐个 `join`。
  **超时不会释放仍挂起的 Fiber 栈**，返回 `false` 并保持 draining，可再次调用续做。
  从 worker 自身调用 → 不 join 且返回 `false`。`shutdown()` = `shutdown_for(duration::max())`。
- 析构里做 `shutdown()`；若 `current_scheduler() == this` → `std::terminate()`（防 UAF）。
- `reap_dead_workers()` 用 `join_mutex.try_to_lock`，拿不到就直接返回（避免与 shutdown 死锁）。

### 3.6 FiberBin 复用

任务终态后 `Task::run()` 调 `Fiber::RecycleForScheduler(std::move(fiber), m_options.fiber_bin_capacity)`；
新 G 用 `Fiber::AcquireForScheduler(...)` 取。仅回收 **Completed/Failed、非当前 Fiber、
且 `record.use_count()==1`** 的对象；复用前重置 FiberRecord/ID/父链/失败状态/取消标记/sanitizer 状态。
池是**每个 M 的线程本地**，不跨 M 共享（`docs/design.md` 2026-10-01 节）。

### 3.7 度量（时序敏感，踩过坑）

- `SchedulerMetrics` 是**累计原子计数**；`collect_metrics=false` 时连计数都不加（全 0）。
- `task_runs`/`task_completions`/`fiber_resume_ns` 在 `task->run()` 返回后才累加，
  与 `task->wait()` 返回/`notify_terminal()` 之间**存在窗口**。
- 历史 flaky：`go2cpp_scheduler_full` 曾经 8 次跑 2 次失败（`task_completions=11` vs 期望 12），
  结论是**示例断言过严**，改为「轮询 + 超时」（commit `351f756`）。这条已固化为项目规范。
- `local_queue_pops` 只在无锁快路径自增：走全局 admission 的本地出队不计入。

---

## 4. 坑点清单（非显然行为）

1. **`yield`/`park`/`park_io`/`park_wait` 只能在拥有该 G 的 worker 上调用**
   （内部校验 `current_scheduler()==this && current_task().get()==task.get()`），否则返回 false。
2. `park_wait_for_cleanup()` **故意忽略取消标记与 draining**，只允许「已在等待队列中线性化注册」的调用方使用。
3. `wake_io_batch()` **跳过 `owned_by` 校验**，只接受本 Scheduler 已 `registered` 的 G；
   传异属 task 会把不属于本调度器的 G 放进本调度器的 P 队列（破坏 owner 不变量）。
4. `BlockingRegion` 不可移动、禁嵌套（单槽 `t_blocking_machine_id`）、**不能跨 Fiber yield/park**；
   违反时 worker 在任务边界 `std::terminate()`（fail-fast）。
5. `Task::wait()` 在 Fiber 内走 `ParkingCondition`（挂起 Fiber），普通线程走原生 cv；
   **自等待立即返回 false**。
6. `Scheduler` 必须活得比所有 worker、外部成员调用和未完成等待都长；析构、`start()`、`enqueue()`、
   快照调用不可并发。
7. 已知项 X-12：`worker_loop` 用同一个 `maintenance_budget` 驱动扩容与终态回收，
   积压时 `growth_period=8` 会不断清零它，使 `collect_terminal_locked`（阈值 64）只能靠 draining 触发 ——
   实测内存影响为噪声级（A/B：72,828 KB vs 72,900 KB），故保持登记、不改。
8. `worker_count()` 是**当前存活 M 数**，与 P 数无固定等式（阻塞/detach 时会 >P）。

---

## 5. 与 Go runtime 的对应与差异

| Go | 本项目 | 差异 |
|---|---|---|
| goroutine | `Task` + `Fiber` | 无抢占（只在挂起点让出）；无 GC；`Task::wait()` 是 Go 没有的 API |
| GOMAXPROCS | `SchedulerConfig::processor_count` | **没有运行期 setter**，必须在启动前定好 |
| P 本地 runq + work stealing | `Processor::queue` + `run_victim` 队尾窃取 | 有窃取，但公平性是轮转而非 Go 的随机 |
| `entersyscall/exitsyscall` P 交接 | `BlockingRegion` 声明 + sysmon 记账 | **必须显式声明**；Go 由 runtime 自动感知 |
| sysmon | `sysmon_loop` | 只做逻辑 P 脱离与扩容，不做 GC STW、不抢占 |
| `sync.Cond` | `core::ParkingCondition` | 同时支持 Fiber 与原生线程，并支持 deadline |
| `errgroup`/`WaitGroup` | `Task::wait()` / `Task::wait_for()` | 单个 G 的完成等待 |

---

## 6. 对架构图的贡献

**节点**

- `Scheduler`（大节点，含 M / P / 全局队列 / incoming 条带 / sysmon / FiberBin）
- `Task(G)`（状态机：New→Runnable→Running→{Runnable|Waiting|Dead|Cancelled|Failed}）
- `Machine(M)`（`std::thread`，绑定 P，可被 sysmon detach）
- `Processor(P)`（本地 deque + attached 计数）
- `Sysmon`（独立线程，try_to_lock，仅记账）
- `Metrics/Observer`（累计计数 + 事件回调）

**连线**

- `Scheduler → Fiber`：「resume_from_scheduler / RecycleForScheduler（FiberBin）」
- `Task → Fiber`：「每个 G 持有一个 unique_ptr<Fiber>」
- `ParkingCondition ⇄ Task`：「park_for_scheduler / wake_for_wait（共用 transition_mutex）」
- `Scheduler → TimerService`：「schedule_at/after/every 通过 TimerExecutorAdapter 提交为 Fiber」
- `IOManager → Scheduler`：「park_io / wake_io / wake_io_batch（批量唤醒）」
- `sysmon → Machine/P`：「逻辑脱离 + maybe_grow（不切栈）」

**图内文字**

> GMP：`Task` 是 G（带入队权/执行权两个 claim 与一个 pending 唤醒令牌），
> `Machine` 是 M（OS 线程，绑定 P，必要时被 sysmon 逻辑脱离），`Processor` 是 P（本地队列）。
> worker 取任务顺序：救援链 → 本地队首 → 全局队列 → 轮转窃取其他 P 的队尾。
> park/wake 通过 `Task::m_transition_mutex` 线性化：等待者要么被原子转成 Runnable 入队，
> 要么拿到一个 pending 令牌；**调度器从不从另一个线程切断 C++ 调用栈**。
> 编号事实：G/M 从 1 开始（0 是「非 worker 线程」哨兵），P 从 0 开始。
