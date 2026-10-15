# G1 · 地基层：`detail/context_backend` / `fiber` / `core/timer` / `core/parking_condition`

> 依赖方向：`context_backend → Fiber → Timer → ParkingCondition → Scheduler`（单箭头，本组在最底端）。
> 规模（实测）：`context_backend` 44+45 行、`fiber` 228+1502 行（另 `fiber_local` 140+183、`thread_policy` 153+202）、
> `core/timer` 344+778 行、`core/parking_condition` 98+337 行。
> 引用纪律：全部给**符号名**；行号会随重排漂移，仅作参考。

---

## 0. 本组在整体运行时的位置（一句话）

**Fiber 是「G 的载体」**：它把一段 C++ 调用装进一块独立的 mmap 栈，靠自研 x86_64 汇编切换；
`Timer` 提供与调度器解耦的单调时钟定时；
`ParkingCondition` 站在 **Fiber × Scheduler 的交叉点**上，把「等待」按调用者身份分派成
「挂起 Fiber / 阻塞原生线程 / 直接拒绝」三条路。

这四者是后面一切并发语义（channel、context、sync、IO、http）能成立的地基。

---

## 1. `detail/context_backend` —— Fiber 的唯一平台依赖

### 1.1 职责与边界

只暴露一个极小的 ABI，把「寄存器/栈切换」这件平台相关的事与 Fiber 的生命周期管理分开：

```cpp
using Context  = void*;                                  // 执行栈上的保存区句柄
struct Transfer { Context context{nullptr}; const void* data{nullptr}; };
using Entry    = void (*)(Transfer) noexcept;            // 新上下文首次恢复时的入口
Context  MakeContext(void* stack_bottom, std::size_t stack_size, Entry entry) noexcept;
Transfer JumpContext(Context target, const void* data) noexcept;
```

它**不管**：栈的分配、guard page、缓存、父链、取消 —— 那些都在 Fiber 侧。

### 1.2 运行机制

- Linux x86_64：`context_backend.cpp` 转发到 `src/concurrency/context_x64.S` 的
  `go2cpp_context_make` / `go2cpp_context_jump`（`extern "C"`，布局借鉴 coost/TBOX，
  但没有复制 coost 的共享栈 memcpy 与 `_exit` 结束路径）。
- 汇编要点（子代理逐行读证的结论）：保存 `rbp/rbx/r12–r15` + `rsp`/`rip`，
  `go2cpp_context_jump` 用 `jmp *%r8` 跳到保存的返回点，并把 `Transfer{saved_context, data}`
  放 `rax/rdx` 回传；`make` 在栈顶对齐后留 56 字节保存区写入 entry 与
  `go2cpp_context_end`；**entry 若返回会走 `go2cpp_context_end` → `_exit@PLT`**（防御性终止）。
- 非 native 平台：`MakeContext` 返回空、`JumpContext` 返回 `{}`，由 `fiber.cpp` 走
  Boost.Context（`make_fcontext`/`jump_fcontext` + `protected_fixedsize_stack`）。
  选择逻辑在 `cmake/Go2CppModules.cmake`：`Linux && 64-bit && 非 aarch64/arm64/riscv/ppc/s390`
  时置 `GO2CPP_NATIVE_CONTEXT=ON` 并定义 `GO2CPP_USE_NATIVE_CONTEXT=1`。

### 1.3 不变量与约束

1. `Transfer::data` 指向**调用方拥有的只读数据**，切换实现**不得延长其生命周期**。
2. 每次 `JumpContext` 都会返回「保存的 caller 上下文」，因此 Fiber 可以在执行权被
   `Task` 的 execution claim 串行化之后**迁移到另一个 M（OS 线程）**。
3. 栈的 guard page/缓存/生命周期由 `FiberStack` 独占管理 —— **不得绕过 Fiber API 直接调用**这两个函数。

### 1.4 坑点

- 该头文件被**转发头假象**掩盖：`include/go2cpp/detail/context_backend.hpp` 就是真身
  （目录本身就是真身位置），不要去找"另一份"。
- 汇编入口返回 = 进程退出：这解释了为什么 Fiber trampoline 必须自己处理所有返回路径
  （`Impl::entry` 收尾时显式 `JumpContext(m_caller, self)` 而不是 `return`）。

### 1.5 与 Go 的对应与差异

| Go | 本模块 | 差异 |
|---|---|---|
| `gogo`/`mcall` 汇编切换 | `go2cpp_context_jump` | 只保存 callee-saved + rsp/rip；没有 Go 的 g0 栈、没有 ABI 内部约定 |
| 栈增长 `morestack`/`copystack` | **无** | 栈固定 128 KiB，不增长（见 §2.4） |

---

## 2. `fiber` —— 真正的地基（G 的载体）

### 2.1 公开接口（`concurrency/fiber.hpp` 实读）

```cpp
enum class FiberState : uint8_t { Ready, Running, Suspended, Completed, Failed };
enum class SuspendReason : uint8_t { None, Yield, Park, Io, Timer, Synchronization };

class Fiber {
  using Function = std::function<void()>;
  static constexpr std::size_t DefaultStackSize() noexcept;      // 128 KiB
  explicit Fiber(Function, std::size_t stack_size = DefaultStackSize());
  ~Fiber();                                                       // 不可拷贝、不可移动
  bool resume() noexcept;                                         // 严格父级校验
  bool resume_from_scheduler() noexcept;                           // 调度器内部快路径
  FiberResumeResult resume_result() noexcept;                      // 带状态+失败+callsnapshot
  static bool Suspend(SuspendReason = Yield) noexcept;             // 只回到直接父级
  static bool SuspendForScheduler(SuspendReason = Park) noexcept;   // 沿父链一路抛到最外层
  static Fiber* Current() noexcept;
  static bool CancellationRequested() noexcept;                     // 沿父链继承
  static FiberContextFrame CurrentContext();
  static FiberContextSnapshot CurrentContextSnapshot();
  static void BindCurrentExecution(FiberExecutionBinding) noexcept; // M/P/G 绑定元数据
  void RequestCancellation() noexcept;                              // 协作式取消标记
  FiberState state() const noexcept;  SuspendReason suspend_reason() const noexcept;
  std::exception_ptr failure() const;  std::size_t stack_size() const noexcept;
  std::uint64_t id() const noexcept;   FiberContextFrame debug_info() const;
  FiberContextSnapshot context_snapshot() const;                     // 含墓碑帧
  FiberExecutionBinding execution_binding() const noexcept;
  std::size_t nesting_depth() const noexcept;                        // main_fiber = 0
  static std::unique_ptr<Fiber> AcquireForScheduler(Function, std::size_t, std::size_t bin_capacity = 32);
  static void RecycleForScheduler(std::unique_ptr<Fiber>, std::size_t bin_capacity = 32) noexcept;
};
```

辅助类型：`FiberExecutionBinding{scheduler_token,task_id,machine_id,processor_id,managed}`、
`FiberContextFrame{id,parent_id,active_parent_id,depth,main_fiber,alive,active,cancellation_requested,state,suspend_reason,last_thread,execution}`、
`FiberResumeResult{accepted,state,failure,context_snapshot; completed(); failed()}`。

### 2.2 执行栈（`fiber.cpp` 实测细节）

- 分配：`mmap(nullptr, usable + page, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS)`，
  随后 `mprotect(mapping, page, PROT_NONE)` 做 **guard page**，位于**低地址端**；
  栈向下增长（`bottom() = mapping + page`，`stack_pointer() = mapping + mapping_size`）。
- 归一化 `normalize_stack_size()`：`0 → 128 KiB`；下限 `2 * page`；向上页对齐；
  只做 `size + guard` 的溢出检查（构造期拒绝），**没有硬上限**（过大时 mmap 失败 → `bad_alloc`）。
- **懒分配**：`Fiber` 构造不 mmap；首次 `resume` 时 `initialize_context()` 才建栈。
- 栈缓存两级：`thread_local StackCache s_thread_stack_pool`（≤8 项）+ 全局
  `s_stack_pool`（≤128 项，按 size 精确匹配）；超限 `munmap`。
- 对象池：`thread_local FiberBin s_thread_fiber_bin`（默认容量 32、上限 4096），
  仅供 `AcquireForScheduler`/`RecycleForScheduler` 使用；复用条件
  `state ∈ {Completed, Failed} ∧ m_context/m_stack == nullptr ∧ 未持 claim ∧ record.use_count()==1`，
  否则**丢弃**（不是报错）。

### 2.3 父链、挂起传播与生命周期

- 每个 Fiber 持有 `std::shared_ptr<FiberRecord>`：`{parent(shared_ptr), alive, state, reason, depth,
  active_parent_id, last_thread, execution, mutex}`。
- **首次 resume 固定父级**：`bind_caller(s_current_fiber)` 把调用者记成直接父 Fiber；
  之后换别的 Fiber/线程来 resume 一律**直接返回 false**。
- `A resume B` 是**同线程同步切栈**（父级在 `resume` 处阻塞），不是并行。
- 挂起有三种出口：`Suspend()`（`propagate=false`，只回直接父级）、
  `SuspendForScheduler()`（`propagate=true`，沿父链循环上抛到最外层 `resume_locked` 循环）、
  以及主体自然返回（→ `Completed`）。
- 取消**沿父链继承**：`Fiber::CancellationRequested()` 从当前 record 往 `parent` 上查。
- 析构语义（关键）：
  1. 先抢 `m_resume_claim`（自旋，见坑点）；
  2. 置 `record->alive=false`、`cancellation_requested=true`；
  3. `Ready` → 直接跳过主体并标 `Completed`；
  4. `Suspended` → `resume_locked(true)` 继续跑到自然结束；
  5. 若调用者不是「可恢复的固定父级」→ `abandon_suspended()`：标 `Failed`、清理 FiberLocal、
     **不展开挂起栈上的 RAII**；
  6. 在自身栈上析构 → `std::terminate()`（明确的 API 前置条件违反）。

### 2.4 栈溢出行为（本项目的硬边界）

- guard page 存在，但**全项目没有 SIGSEGV handler、没有 sigaltstack**（对 `src/`、`include/` 全量 grep 无命中）。
- 因此越界 = 内核默认动作 = **进程立即终止（退出码 139），不可捕获、不可恢复**。
- 实测公式（`benchmarks/fiber_stack_probe.cpp` 45 组数据，`stack_utilization` 全为 1.0000）：
  `可承受调用深度 ≈ 栈容量 / (每帧局部变量字节数 + 128)`；128 字节是与帧无关的固定开销。
  本工作区复现值：128 KiB 栈 → 帧 0 时 **1021** 层、帧 256 B 时 **339** 层；8 MiB pthread → **65496** 层。

### 2.5 `fiber_local` 与 `thread_policy`（同组小模块）

- `FiberLocalCache<T>`（别名 `FiberLocal<T>`）：值存在全局 `Registry`
  （`mutex` + `unordered_map<Fiber*, unordered_map<KeyId, shared_ptr<void>>>`），**按 Fiber 对象指针隔离**；
  在 Fiber 外调用退化为 `thread_local` 后备值。键 `KeyId` 由 `AllocateKey()` 原子单调分配（0 为哨兵）。
  值在 trampoline 结束与 `abandon_suspended` 时清理，析构不在锁内执行。
- `thread_policy`：`ScopedThreadParticipation`（外部线程是否愿意参与 Scheduler）与
  `ScopedThreadHookMode`（当前线程 socket Hook 覆盖），**只是 TLS 元数据 scope**，
  不会把普通线程变成 M。

### 2.6 坑点（非显然行为）

1. **owner 必须长于所有 `resume`**；不能在 Fiber 自身栈上析构（`terminate`）。
2. `resume()` 返回 `false` 的统一含义是「本次没有执行」：Running / 已终态 /
   `m_resume_claim` 竞争失败 / 父级校验失败 / 后端上下文创建失败（最后一种会在 `failure()` 留异常）。
3. **异常永不跨 resume 边界**：`entry` 内 `catch(...)` → `m_failure`，状态 `Failed`；`resume` 全 `noexcept`。
4. 每层嵌套 = 独立一块栈：嵌套 N 层 ≈ N × (128 KiB + 1 页 guard) 虚拟内存。
5. `abandon_suspended()` 会**静默丢弃挂起栈上的 RAII**（局部对象析构、锁释放都不执行）——
   所以必须由固定父级收尾，这是编码规范而不是可选项。
6. **只有 `errno` 按 Fiber 保存/恢复**（`m_saved_errno`）；其他 `thread_local` 不随 Fiber 隔离，
   因此 `thread_local` 不能当 G-local 用（要 G-local 就用 `fiber_local`）。
7. `AllocateKey()` 回绕即 `std::terminate()`（设计上的隔离保护）。
8. FiberBin 复用要求 `record.use_count()==1`：有子 Fiber 持有父 record 时不会被复用。

### 2.7 与 Go 的对应与差异

| Go | 本模块 | 差异 |
|---|---|---|
| goroutine（G） | `Fiber` + `Task` | G 由 Scheduler 持有 `Task`，`Fiber` 只是执行栈 |
| 自动栈增长（2 KiB→1 GiB） | **无** | 固定 128 KiB；越界即进程死亡。**手动等价物**：到阈值就 `resume` 一个新 Fiber（换栈） |
| M:N 抢占调度 | 协作式 | 只在 `suspend`/`park`/`yield` 点让出 |
| `gopark`/`sudog` | `Suspend` + `ParkingCondition` | 见 §4 |

---

## 3. `core/timer` —— 与调度器解耦的单调时钟定时

### 3.1 类型与配置

```cpp
enum class TimerState      { kInvalid, kPending, kRunning, kCompleted, kCancelled, kFailed };
enum class TimerWaitMode   { kConditionVariable, kBusyWait };
enum class TimerRepeatMode { kOnce, kFixedDelay, kFixedRate };
struct TimerOptions { TimerRepeatMode repeat{kOnce}; duration period{}; TimerWaitMode wait_mode{kConditionVariable};
                      duration spin_window{50us}; std::size_t max_runs{0}; bool skip_missed{true}; };
struct TimerDurationLimits { duration warn_after{30d}; duration error_after{365d}; };  // 只告警，不拒绝
struct TimerContext { std::size_t run_count{0};   // 从 1 开始
                      time_point scheduled_deadline; time_point started_at; };
class TimerDecision { static Stop(); Continue(); ContinueAfter(delay); ContinueAt(deadline); };
```

### 3.2 结构与机制

- `Timer` = 轻量句柄（`shared_ptr<State>`，`State::m_owner` 以 `weak_ptr` 回指 `Impl`）；
  **析构等价于 Cancel**；`Reset(deadline)` / `Refresh()` / `Active()` / `StateValue()` / `RunCount()`。
- `TimerService` = 单 worker 线程 + `std::multimap<deadline, State>` 队列 +
  `unordered_map<State*, iterator>` 索引 + `atomic<uint64_t> m_generation`（重排/取消时递增，
  用 `cv.wait_until` 的谓词与 busy-wait 的退出条件）。
- 注册族：`Schedule` / `ScheduleAfter` / `ScheduleEvery` / `ScheduleRepeat` / `ScheduleOn` /
  `ScheduleEveryOn` / `ScheduleWeakAfter` / `ScheduleWeakEvery`（weak 版在 owner 消失时
  自动完成或 `Stop`）。
- `TimerExecutor` 抽象（`Submit(std::function<void()>)`）把「到期后在哪执行」外置：
  **契约是 Submit 必须快速返回，且不得在 Submit 内恢复别的线程的 Fiber 栈**。
  Scheduler 通过自己的 `TimerExecutorAdapter` 只把回调**提交为 Fiber**。
- 执行路径：`Run()` 取最早项 → `Execute(状态锁内取回调/置 kRunning/run_count++；锁外调用)`
  → `Finish()` 决定下一次（`kFixedRate` 用 `scheduled_deadline + period` 并对齐 `skip_missed`；
  `kFixedDelay` 用 `now + period`）→ 异常走 `NotifyException`（handler 在锁外）并置 `kFailed`。
- 长时长任务只记 WARN/ERROR（`log_long_duration`），**不拒绝**注册。
- `Default()` 是进程级单例（`static TimerService`），进程结束时回收。
- 在**自身定时器线程内**析构服务 → `std::terminate()`（fail-fast）。

### 3.3 坑点

1. **临时 `Timer` 句柄立即析构 = 取消任务**：想让它活着就必须持有句柄（历史血泪教训）。
2. `TimerService::Default()` 的回调跑在**定时器线程**上，不是 Fiber；
   要跑 Fiber 工作必须显式 `go()`/`spawn()`。
3. `Finish()` 在服务已 `stopping` 时会把任务收敛为 `kCancelled`，**禁止重新入队**（避免留下永不执行的任务）。
4. `Timer` 不可拷贝、只能移动；移动后源句柄失效。
5. `busy_wait` 只在 `spin_window`（默认 50 µs）内自旋，不会长期占满 CPU。

### 3.4 与 Go 的对应与差异

| Go | 本模块 | 差异 |
|---|---|---|
| runtime timer heap（P 上） | 独立 worker 线程 + multimap | 定时器线程**不直接恢复 Fiber 栈**（Go 的 timer 回调直接跑在 G 上） |
| `time.AfterFunc` | `ScheduleAfter` | 语义接近；另有 `weak` 变体（Go 无） |
| `Ticker` | `ScheduleEvery`（`kFixedRate`/`kFixedDelay`） | 显式区分"补跑"与"跳过错过周期" |

---

## 4. `core/parking_condition` —— Fiber × Scheduler 的交叉点

### 4.1 职责：一个 wait，三种身份

`ParkingCondition::WaitOnce()` 在阻塞前先判定**当前执行者是谁**，再分派：

| 身份 | 判定 | 行为 |
|---|---|---|
| 受管 G | `Scheduler::current_scheduler() != nullptr && current_task() != nullptr` | `park_wait(task)` 挂起 Fiber，**让出 M** |
| 普通线程 | 上面不成立且 `Fiber::Current() == nullptr` | 阻塞 `std::condition_variable_any` |
| 手动 Fiber（无 Scheduler） | `Fiber::Current() != nullptr` 但无 Task | **直接返回 `kCancelled`**，绝不阻塞 carrier 线程 |

`FiberWaitUnsupported()` 就是第三种身份的探测器；头文件明确要求调用方先绑定 Scheduler 或改用非阻塞接口。

### 4.2 等待节点协议（单赢家 + 一次性 gate）

`WaitNode` 字段：`m_scheduler`（裸指针）、`shared_ptr<Task> m_task`、
`atomic<Result> m_result{kWaiting}`、`HybridMutex m_wake_mutex`、`condition_variable_any m_native_cv`、`bool m_active`。

- `Finish(result)`：`compare_exchange_strong(kWaiting → result, acq_rel)` —— **只有第一个调用者赢**。
- `Wake()`：在 `m_wake_mutex` 下检查 `m_active`，取出 `{scheduler, task}` 后**在锁外**
  调 `wake_or_cancel(task)`（异常被吞），或对 native 路径 `notify_one()`。
  注释写明：Disarm 必须在任何 in-flight 回调离开这道门之后才允许，避免裸 Scheduler 指针活过等待栈。
- `Disarm()`：置 `m_active=false`、`m_scheduler=nullptr`。
- `WaitManaged()` 的循环（关键）：
  1. 先读 `result()`，**已发布终态就直接返回**（省掉一次无意义的 park）；
  2. 检查 `task->cancellation_requested()`；
  3. `scheduler->park_wait(task)`；
  4. 醒来后复检：终态 / 取消 / `!suspended && !scheduler->is_running()` /
     `current_scheduler()!=scheduler` / `current_task()!=task` → 收敛为 `kCancelled`；
  5. 否则视为**伪唤醒**，重新 park（注释明确：**不允许用 busy-yield 模拟等待**）。

### 4.3 注册与清理（`WaitOnce` 全流程）

1. 取 `scheduler`/`task`，判定 managed；
2. 非 managed 的手动 Fiber → `kCancelled`；managed 且已取消 → `kCancelled`；已过 deadline → `kTimedOut`；
3. 构造 `WaitNode`，在 `m_mutex` 下**二次检查 predicate**（关闭 notify-before-publication 窗口）后入 `m_waiters`；
4. 若 managed 且有 deadline：用 `TimerService::Default().Schedule(deadline, [weak_node]{ Finish(kTimedOut) 成功后 Wake(); })`
   —— 用 `weak_ptr` 保证节点销毁后回调不悬空；
5. managed → `WaitManaged()`；native → `WaitNative()`（`outer.unlock()` 后 cv 等待，超时自行 `Finish(kTimedOut)`）；
6. `cleanup()`：`Finish(kCancelled)` → `Disarm()` → `timer.Cancel()` → 从 `m_waiters` 摘除；
7. 重新锁回外部 mutex（异常路径也保证重新加锁后再抛出）。

`notify_one()` 会**跳过已被别人抢先 `Finish` 的节点**（循环取队首直到赢得一个）；
`notify_all()` 先整体 swap 出队列再逐个 `Finish`+`Wake`（锁外唤醒）。

### 4.4 坑点

1. `wait()/wait_for()/wait_until()` **要求调用方持有传入的 `unique_lock<std::mutex>`**；
   它不拥有业务状态、也不负责通知条件的产生。
2. managed G 被**取消**时 `wait()`（返回 `bool` 的那个模板）返回的是 `predicate()` 的值 ——
   即「可能没等到条件成立就返回了」；需要严格语义时用 `wait_for/wait_until` 的返回值或自己的谓词二次确认。
3. `m_result` 是 `atomic` 但**不能替代队列锁**：入队/摘除仍在 `m_mutex` 下。
4. deadline 是靠**外部 TimerService** 实现的，因此极端情况下定时器线程的抖动会影响超时精度（毫秒级）。
5. 手动 Fiber 上等待直接失败而不是阻塞 —— 这是「宁可返回失败，也不阻塞整条 carrier 线程」的设计选择。

### 4.5 与 Go 的对应与差异

| Go | 本模块 | 差异 |
|---|---|---|
| `gopark`/`goready` | `park_wait` / `wake_or_cancel` | 节点是显式堆对象 + 原子单赢家，不是 sudog |
| `sync.Cond` | `ParkingCondition` | 同时服务 Fiber 与原生线程；**支持 deadline**（Go 的 Cond 不支持） |
| `select` 的超时 | `TimerService` + weak 节点 | 超时与唤醒共用同一原子结果位 |

---

## 5. 本组小结

### 5.1 关键数字（本工作区实测/实读）

| 项 | 值 |
|---|---|
| Fiber 默认栈 | 128 KiB（`Fiber::DefaultStackSize()`） |
| guard page | 1 页，低地址端，`PROT_NONE` |
| 128 KiB 栈可承受递归深度 | 帧 0 → 1021 层；帧 256 B → 339 层（Release 基准：1022 / 340） |
| 8 MiB pthread 对照 | 65496 层（Release：65497） |
| 栈缓存上限 | TLS 8 项 + 全局 128 项 |
| FiberBin 上限 | 默认 32，硬上限 4096 |
| `TimerOptions::spin_window` | 50 µs（仅 busy-wait 模式） |
| 长时长告警阈值 | WARN 30 天 / ERROR 365 天 |

### 5.2 对架构图的贡献

**节点（建议标签）**

- `ContextBackend`（子标签：`context_x64.S` / Boost.Context fallback）
- `Fiber`（子标签：`FiberStack(mmap+guard)` / `FiberRecord(父链)` / `FiberBin(对象池)`）
- `FiberLocal`（G-local 存储）、`ThreadPolicy`（线程参与/Hook 模式 scope）
- `TimerService`（独立 worker 线程）、`Timer`（句柄）
- `ParkingCondition`（三分支：managed park / native cv / 手动 Fiber 拒绝）

**连线（方向与标签）**

- `Fiber → ContextBackend`：「MakeContext/JumpContext（寄存器+栈切换）」
- `TimerService → Timer`：「句柄弱引用 Impl，析构=Cancel」
- `ParkingCondition → Scheduler`：「park_wait / wake_or_cancel（挂起 Fiber，让出 M）」
- `ParkingCondition → TimerService`：「deadline 由 TimerService 触发 Finish(kTimedOut)」
- `ParkingCondition → libc`：「原生线程路径：condition_variable_any」
- `Fiber → ParkingCondition`：「SuspendForScheduler 沿父链上抛」

**图内文字（建议直接放进图的说明块）**

> 地基层：Fiber 把一段 C++ 调用装进固定 128 KiB 的 mmap 栈（低地址端 1 页 guard）。
> 爆栈 = SIGSEGV = 进程立即死亡（无 handler、无 sigaltstack），所以深度必须由调用者自己保证。
> 等待语义全部汇聚到 ParkingCondition：受管 G 挂起、普通线程阻塞、手动 Fiber 直接拒绝。
> Timer 是独立线程，负责 deadline 与周期任务，从不直接恢复 Fiber 栈。

**待补充（由 G4/G5 提供）**：Channel/sync/IO 如何各自使用 ParkingCondition（本组只定义协议，不定义业务等待队列）。
