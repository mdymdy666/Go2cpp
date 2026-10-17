# G1 · 地基层 —— 一图看懂

> 覆盖：`Fiber`（用户态栈与协作式切换）、`FiberContextFrame`、`FiberExecutionBinding`、
> `ParkingCondition`（受管等待原语）、`Timer` / `TimerService` / `TimerDecision`（定时器）。
> 源码：`include/go2cpp/concurrency/fiber.hpp`、`core/parking_condition.hpp`、`core/timer.hpp`
> 与对应 `.cpp`。

**这一层是整个运行时的地基**：它不知道"调度器"的存在，只提供「切换执行流」与「未来某个时刻唤醒我」两件事。

## ① 类架构：每个类用的底层能力 + 主要职责

```mermaid
classDiagram
    direction TB

    class FiberState {
        <<枚举 4 值>>
        kReady
        kRunning
        kSuspended
        kCompleted
        kFailed
    }
    class SuspendReason {
        <<枚举>>
        Yield
        Wait
        Sleep
    }

    class FiberExecutionBinding {
        <<只读绑定信息>>
        +task_id uint64
        +machine_id uint64
        +managed bool
        ← 由 Scheduler 在 resume 时注入
    }
    class FiberContextFrame {
        <<上下文帧>>
        +id uint64
        +parent_id uint64
        +active_parent_id uint64
        +depth size_t
        +main_fiber bool
        +alive bool
        +active bool
        +cancellation_requested bool
    }
    class FiberResumeResult {
        <<resume 返回值>>
        +accepted bool
        +state FiberState
        +completed() bool
        +failed() bool
    }

    class Fiber {
        <<一个用户态执行流>>
        -Impl* m_impl  (unique_ptr)
        -Function m_function
        -上下文帧链表
        +DefaultStackSize() size_t
        +AcquireForScheduler(fn, stack, bin) unique_ptr~Fiber~
        +RecycleForScheduler(fiber, bin) void
        +resume() bool
        +resume_from_scheduler() FiberResumeResult
        +Suspend(reason) static bool
        +SuspendForScheduler(reason, binding) static bool
        +Current() static Fiber*
        +id() uint64
        +nesting_depth() size_t
        +execution_binding() FiberExecutionBinding
    }
    class FiberBin {
        <<栈复用池>>
        -capacity 32
        ← 避免每次 spawn 都 mmap/munmap
    }

    class ParkingCondition {
        <<受管等待原语>>
        +wait(lock, pred) bool
        +wait_for(lock, timeout, pred) bool
        +wait_until(lock, deadline, pred) bool
        +notify_one() void
        +notify_all() void
        -CancellationRequested() static bool
        -FiberWaitUnsupported() static bool
        ← 受管 G 走 park；普通线程走 condition_variable
    }

    class TimerState {
        <<枚举>>
        kIdle
        kScheduled
        kFired
        kCancelled
    }
    class TimerDecision {
        <<回调后的决策>>
        +Kind kStop
        +Kind kContinue
        +Kind kAfter
        +Kind kAt
    }
    class TimerExecutor {
        <<执行器接口>>
        +Submit(callback) bool
    }
    class Timer {
        <<单个定时器>>
        +Cancel() void
        +Active() bool
        +Reset(deadline) bool
        +Refresh() bool
    }
    class TimerService {
        <<定时器服务·最小堆 + 调度线程>>
        +schedule(deadline, cb) Timer
        +shutdown()
    }

    Fiber *-- FiberContextFrame : 帧链表
    Fiber ..> FiberExecutionBinding : 由调度器注入
    Fiber ..> FiberResumeResult : resume 返回
    Fiber ..> FiberState
    Fiber ..> SuspendReason
    Fiber ..> FiberBin : Acquire/Recycle 复用栈
    ParkingCondition ..> Fiber : 受管 G 时调用 Suspend
    TimerService *-- Timer
    Timer ..> TimerState
    Timer ..> TimerDecision
    TimerService ..> TimerExecutor : 到点后提交回调
```

**职责一句话总结**

| 类 | 主要职责 | 用到的底层能力 |
|---|---|---|
| `Fiber` | 一块**固定大小的用户态栈** + `resume`/`Suspend` 协作式切换；记录上下文帧链路 | `mmap`/`munmap`（栈）、`ucontext` 或等价的手写切换 |
| `FiberBin` | **栈复用池**（默认 32 个），避免每次 `spawn` 都系统调用 | `Fiber` 回收 |
| `FiberContextFrame` | 记录「谁在谁里面」（父子、深度、是否 main、是否被取消） | 无（纯数据） |
| `FiberExecutionBinding` | 记录「这个 Fiber 当前属于哪个 G / 哪个 M / 是否受管」 | 无（纯数据） |
| `ParkingCondition` | 让**受管 G** 用 park 等待、让**普通线程**用 `condition_variable` 等待，**同一套 API** | `Fiber::Suspend` 或 `std::condition_variable` |
| `Timer` / `TimerService` | 「未来某时刻唤醒我」；`TimerDecision` 让回调决定停止/继续/改期 | `std::thread` + 最小堆 + 条件变量 |
| `TimerExecutor` | 把「到点的回调」**交给谁执行**（通常提交给调度器） | 抽象接口 |

## ② 重要函数：功能与实现要点

```mermaid
flowchart TB
    subgraph ACQ["Fiber::AcquireForScheduler(fn, stack_size, bin_capacity=32)"]
        A1["先从 FiberBin 取一个空闲 Fiber"] --> A2{"取到?"}
        A2 -- 是 --> A3["重置 m_function = fn<br/>重置上下文帧状态"]
        A2 -- 否 --> A4["新建 Fiber：mmap 一块 stack_size 的栈<br/>（DefaultStackSize 为默认值）"]
        A4 --> A3
        A3 --> A5["返回 unique_ptr&lt;Fiber&gt;"]
    end

    subgraph RES["Fiber::resume_from_scheduler(binding)"]
        B1["记录 FiberExecutionBinding<br/>（task_id / machine_id / managed）"] --> B2{"当前状态允许 resume?"}
        B2 -- 否 --> B3["返回 FiberResumeResult{accepted=false}"]
        B2 -- 是 --> B4["压入新的上下文帧<br/>（id/parent/depth 递增）"]
        B4 --> B5["切换到该 Fiber 的栈执行"]
        B5 --> B6{"Fiber 内部发生了什么?"}
        B6 -- "正常返回" --> B7["state=Completed<br/>弹出上下文帧，切回调用方"]
        B6 -- "抛出异常" --> B8["state=Failed<br/>异常被 Fiber 边界捕获，不越过栈"]
        B6 -- "调用 Suspend" --> B9["state=Suspended<br/>保存上下文，切回调用方"]
        B7 --> B10["返回 FiberResumeResult"]
        B8 --> B10
        B9 --> B10
    end

    subgraph SUS["Fiber::Suspend(reason) —— 协作式让出"]
        C1["只能在 Fiber 内部调用"] --> C2["记录 SuspendReason（Yield/Wait/Sleep）"]
        C2 --> C3["把当前上下文帧标记为非 active"]
        C3 --> C4["切回 resume 的调用方<br/>（即 worker 的 worker_loop）"]
        C4 --> C5["**绝不 detach 任意 C++ 调用栈**<br/>只在 Fiber 自己愿意的地方让出"]
    end

    subgraph PC["ParkingCondition::wait(lock, pred)"]
        D1["解锁用户 mutex"] --> D2{"当前是否受管 G?"}
        D2 -- 是 --> D3["走 Fiber::Suspend<br/>（受管等待）"]
        D2 -- 否 --> D4["走 std::condition_variable<br/>（普通线程等待）"]
        D3 --> D5["被 notify 且 predicate 满足后<br/>重新加锁用户 mutex 并返回 true"]
        D4 --> D5
        D1 --> D6{"取消已请求?"}
        D6 -- 是 --> D7["返回 false（调用方需自行处理）"]
    end

    ACQ --> RES
    RES --> SUS
    SUS --> PC
```

**要点（源码约束）**

- `Fiber` 的栈**固定大小**，**不可增长**（与 Go 的可增长栈是本质差异，深递归会失败）；
- `AcquireForScheduler` / `RecycleForScheduler` 成对出现，配合 **`FiberBin`（默认 32）** 复用栈，避免频繁 `mmap`；
- `resume_from_scheduler` 是**调度器视角**的入口（带 `binding`），`resume()` 是无绑定的通用入口；
- 返回 `FiberResumeResult` 而非 `bool`，因为调用方需要知道**是完成、失败还是挂起**；
- `Suspend` 是**静态**方法且**只能在 Fiber 内调用**，意味着「让出」必须由 G 自己发起 —— 这就是"纯协作式"的技术根源；
- `ParkingCondition` 把**受管 G** 与**普通线程**两种等待统一到一个 API：前者 park、后者 `condition_variable`；
- `TimerDecision` 让定时器回调返回 `kStop`/`kContinue`/`kAfter`/`kAt` —— 决定"是否继续、下次何时"，因此**周期定时器不需要反复重建**。

## ③ 向上层提供了什么

```mermaid
graph LR
    subgraph G1API["地基层对外能力"]
        F1["Fiber：独立栈 + resume/Suspend"]
        F2["FiberBin：栈复用"]
        F3["FiberContextFrame：父子上下文链"]
        F4["ParkingCondition：统一的等待原语"]
        F5["Timer/TimerService：定时唤醒"]
        F6["TimerDecision：回调决定是否续期"]
    end

    subgraph USED["谁在用"]
        S1["Scheduler：用 Fiber 执行每个 G<br/>用 FiberBin 降低 spawn 成本"]
        S2["sync::Mutex / ConditionVariable / WaitGroup：<br/>全部通过 ParkingCondition 挂起"]
        S3["channel / future：等待对方时同样走 ParkingCondition"]
        S4["timer 相关 API：用 TimerService 实现 After/AfterFunc/Ticker"]
        S5["context：用 FiberContextFrame 传播取消"]
    end

    F1 --> S1
    F2 --> S1
    F3 --> S5
    F4 --> S2
    F4 --> S3
    F5 --> S4
    F6 --> S4
```

## ④ 状态流转

### Fiber 的状态机（对应 G 在调度器中的可见状态）

```mermaid
stateDiagram-v2
    [*] --> kReady : AcquireForScheduler / 构造

    kReady --> kRunning : resume / resume_from_scheduler
（压入上下文帧）
    kRunning --> kSuspended : Suspend(reason)
（Yield / Wait / Sleep）
    kSuspended --> kRunning : 再次 resume / resume_from_scheduler
    kRunning --> kCompleted : 用户函数正常返回
（弹出上下文帧）
    kRunning --> kFailed : 用户函数抛出
（在 Fiber 边界捕获）
    kCompleted --> [*] : RecycleForScheduler（进 FiberBin）
    kFailed --> [*] : RecycleForScheduler

    note right of kSuspended
        与 G 的 kWaiting 对应：
        Fiber 挂起 == 调度器侧看到 G 需要等待
        恢复时必须由别人 resume，Fiber 不会自旋等待
    end note
```

### Timer 的状态机

```mermaid
stateDiagram-v2
    [*] --> kScheduled : schedule(deadline, cb)
    kScheduled --> kFired : 到达 deadline
（TimerService 取出最小堆顶部）
    kScheduled --> kCancelled : Cancel()
    kFired --> kScheduled : 回调返回 kContinue / kAfter / kAt
（续期，无需重建 Timer 对象）
    kFired --> [*] : 回调返回 kStop
    kCancelled --> [*]
```

## 附：一次「挂起 → 被唤醒」的栈变化（ASCII）

```
worker 线程栈                         Fiber 栈
     |                                   |
     |-- resume_from_scheduler --------->|
     |                                   |-- 用户代码
     |                                   |   (遇到锁 / channel)
     |                                   |-- ParkingCondition::wait
     |                                   |-- Fiber::Suspend(Wait)
     |<-- 切回（保存 Fiber 的寄存器上下文）|
     |                                   |
     |   (worker 去执行别的 G)            |  (Fiber 栈保持原状，不销毁)
     |                                   |
     |-- 被唤醒，再次 resume ----------->|
     |                                   |-- 从 Suspend 之后继续
     |                                   |-- 返回用户代码
     |<-- 栈上数据全部保留 ---------------|
```

> **关键点**：`Fiber::Suspend` 只是**保存寄存器并切回调用方**，Fiber 的栈与局部变量**原样保留** ——
> 这正是"协程比线程便宜"的原因，也是为什么**必须在 Fiber 内主动让出**（不能从外部 detach 栈）。

## 与 Go 的差异

| 项 | Go | Go2Cpp |
|---|---|---|
| 栈 | 可增长（copy 栈，2KB 起） | **固定大小**，`fiber_stack_size` 决定，不可增长 |
| 切换 | 运行时抢占点 + `gopark` | **纯协作**，只有 `Suspend` 能切，且必须在 Fiber 内调用 |
| 上下文 | `goexit` 栈 + defer 链 | `FiberContextFrame` 链表（父子/深度/取消标志） |
| 定时器 | 四叉堆 + `netpoll` 合流 | 最小堆 + 独立调度线程，到点通过 `TimerExecutor` 提交 |
| 等待原语 | `sudog` 队列 | `ParkingCondition`（受管走 park、普通线程走 `condition_variable`） |
