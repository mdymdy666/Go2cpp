# G2 · GMP 调度模块 —— 一图看懂

> 覆盖：`Task`（= G）、`Scheduler`、P/M 资源、`worker_loop`、`sysmon`、`TaskRegistry`、`FiberBin`。
> 源码：`include/go2cpp/concurrency/scheduler/scheduler.hpp`、`src/concurrency/scheduler/scheduler.cpp`。

## ① 类架构：每个类用的底层能力 + 主要职责

```mermaid
classDiagram
    direction TB

    class Fiber {
        <<底层·用户态栈>>
        +resume()
        +suspend()
        +id() uint64
        +nesting_depth() size_t
    }
    class TimerService {
        <<底层·定时>>
        +schedule(deadline, cb)
    }
    class ParkingCondition {
        <<底层·挂起原语>>
        +park()
        +unpark()
    }
    class Context {
        <<取消与值>>
        +Done() DoneSignal
        +Err()
    }

    class Task {
        <<G：一个协程>>
        -unique_ptr~Fiber~ m_fiber
        -Function m_function
        -atomic~GState~ m_state
        -atomic~bool~ m_queued
        -TaskOptions m_options
        -shared_ptr~Promise~ m_completion
        +id() GId
        +state() GState
        +queued() bool
        +started() bool
        +wait() bool
        +failed() bool
        +cancellation_requested() bool
        -run()  ← Fiber 首次进入时执行
        -try_mark_queued() bool
    }

    class Scheduler {
        <<M/P 资源与调度核心>>
        -unique_ptr~Impl~ m_impl
        +start()
        +shutdown()
        +spawn(fn, opts) shared_ptr~Task~
        +set_observer(obs)
        +processors() vector~ProcessorSnapshot~
        +machines() vector~MachineSnapshot~
        +metrics() SchedulerMetrics
        -enqueue(task)          ← 本地优先，超限落全局
        -requeue_to_processor(t, p)
        -park_wait(node)        ← 安全点挂起
        -emit_event(evt)        ← 唯一观测出口
    }

    class Impl {
        <<结构核心>>
        -vector~Processor~ processors
        -deque~Machine~ machines
        -Queue global_queue
        -array~TaskRegistry,16~ task_registries
        -mutex registry_mutexes
        -TimerService timers
        -sysmon 线程
    }
    class Processor {
        <<P：可运行队列>>
        +deque~shared_ptr~Task~~ queue
        +mutex
        +atomic~PState~ state
    }
    class Machine {
        <<M：工作线程>>
        +thread
        +atomic~MState~ state
        +processor PId
        +atomic~GId~ blocking_task
        +atomic~long_syscall_count
    }
    class TaskRegistry {
        <<16 分片·强持有>>
        +unordered_map~GId,shared_ptr~Task~~
        ← 保证非终态 G 不被回收
    }
    class SchedulerObserver {
        <<只读插件边界>>
        +OnEvent(const SchedulerEvent&)
        ← 不能控制 G 执行权
    }
    class SchedulerEvent {
        +type SchedulerEventType
        +task_id / machine_id / processor_id
        +task_state GState
        +timestamp steady_clock
    }

    Task ..> Fiber : 用它的栈与 resume/suspend
    Task ..> Context : 取消传播
    Scheduler *-- Impl
    Impl *-- Processor : 若干 P
    Impl *-- Machine : 若干 M
    Impl *-- TaskRegistry : 强持有所有非终态 G
    Impl ..> TimerService : 定时唤醒
    Machine ..> Processor : 绑定/解绑
    Machine ..> Task : 执行 run()
    Task ..> ParkingCondition : 阻塞原语挂起
    Scheduler ..> SchedulerObserver : 只读事件
    SchedulerObserver ..> SchedulerEvent
```

**职责一句话总结**

| 类 | 主要职责 | 用到的底层能力 |
|---|---|---|
| `Fiber` | 提供**独立用户态栈**与 `resume/suspend`；只能协作式切换 | libc/pthread、`mmap` 栈 |
| `Task`(G) | 承载**一个协程**：入口函数、状态机、完成通知、取消请求 | `Fiber`、`Context`、`Promise`、原子量 |
| `Scheduler` | 对外 API：`start/shutdown/spawn`、观测出口 `emit_event` | 上述全部 |
| `Impl` | 真正的结构：P 数组、M 队列、**全局队列**、**16 分片注册表**、定时器、sysmon | `TimerService`、`std::mutex` |
| `Processor`(P) | 一个**可运行队列**（`deque`），本地 FIFO | 自身 `mutex` |
| `Machine`(M) | 一个**工作线程**，绑定/解绑 P，执行 G | `std::thread`、原子量 |
| `TaskRegistry` | **强持有**全部非终态 G，防止被误回收 | `unordered_map` + 分片锁 |
| `SchedulerObserver` | **只读**插件边界（不能控制 G） | — |

## ② 重要函数：功能与实现要点

```mermaid
flowchart TB
    subgraph SP["Scheduler::spawn(fn, opts)"]
        S1["构造 Task<br/>unique_ptr&lt;Fiber&gt; + m_function"] --> S2["登记到 TaskRegistry<br/>（16 分片，强持有）"]
        S2 --> S3["enqueue(task)"]
        S3 --> S4{"本线程绑定 P 且<br/>local_queue.size < local_queue_limit(256)?"}
        S4 -- 是 --> S5["push_back 到本地队列<br/>（饥饿时唤醒空闲 M）"]
        S4 -- 否 --> S6["落到 global_queue"]
        S5 --> S7["返回 shared_ptr&lt;Task&gt;"]
        S6 --> S7
    end

    subgraph RUN["Task::run() —— Fiber 首次进入 / 恢复后继续"]
        R1["首次进入：把 m_function **移出**<br/>（之后为空，避免二次执行）"] --> R2["调用用户函数"]
        R2 --> R3{"正常返回?"}
        R3 -- 是 --> R4["state = kDead<br/>完成 Promise，唤醒 wait()"]
        R3 -- 抛出 --> R5["state = kFailed<br/>保存 exception_ptr"]
        R3 -- 被取消 --> R6["state = kCancelled"]
    end

    subgraph WL["worker_loop（每个 M 一个）"]
        W1["绑定 P：state kIdle → kRunning"] --> W2["pop_front 本地队列（FIFO）"]
        W2 --> W3{"本地空?"}
        W3 -- 否 --> W6["resume 该 G 的 Fiber"]
        W3 -- 是 --> W4["先去 global_queue 取"]
        W4 --> W5{"仍空?"}
        W5 -- 是 --> W7["work stealing：<br/>从**别的 P 的队尾**偷"]
        W5 -- 否 --> W6
        W7 --> W8["彻底空 → 短暂 idle 等待<br/>（idle_wait=10ms）/ 退出到 kParked"]
        W6 --> W9["G 在 Fiber 内运行；<br/>遇到挂起点时 park_wait 返回"]
        W9 --> W2
    end

    subgraph PK["park_wait(node) —— 安全点挂起"]
        P1["登记 WaitNode 到所在等待队列"] --> P2{"能否立刻完成?"}
        P2 -- 能 --> P3["ParkAction::kRequeued<br/>直接重新入队，不进 park"]
        P2 -- 不能 --> P4["ParkAction::kParked<br/>真正切换回 worker"]
        P4 --> P5["被 Wake 时：<br/>WakeAction::kPending 先挂起标志<br/>（防丢唤醒）"]
        P1 --> P6{"取消已请求?"}
        P6 -- 是 --> P7["ParkAction::kRejected"]
    end

    SP --> RUN
    RUN --> WL
    WL --> PK
```

**要点（这些都是源码里真实存在的约束）**

- `spawn` 的落点：**本地优先**（上限 `local_queue_limit`，默认 256），溢出才进 **全局队列**；
- 本地取用是 `pop_front()`（**FIFO**），而 **work stealing 从队尾偷**（降低与本地 FIFO 的冲突）；
- `Task::run` **第一次**进入时会把 `m_function` **移出**，之后为空 —— 保证用户函数只执行一次；
- `park_wait` 有三个结果而不是两个：`kRejected`（已请求取消）/ `kParked`（真挂起）/ `kRequeued`（可立即继续，**不进 park** 省一次切换）；
- 唤醒侧用 **pending-wake** 语义：先挂标志再 park，保证不丢唤醒；
- 全流程只有**一个观测出口** `emit_event`：它把 observer 的 `shared_ptr` 在短锁内拷出，**锁外**调用回调，并捕获一切异常（绝不因插件让 worker 线程异常退出）。

## ③ 向上层提供了什么

```mermaid
graph LR
    subgraph API["对外能力（go2cpp::scheduler 顶层别名）"]
        A1["spawn(fn, opts) → shared_ptr&lt;Task&gt;"]
        A2["Task::wait() / wait_for(timeout)"]
        A3["Task::failed() / failure() / rethrow_failure()"]
        A4["Task::cancellation_requested()"]
        A5["start() / shutdown() / shutdown_for(timeout)"]
        A6["set_observer(SchedulerObserverPtr)"]
        A7["只读快照：processors() / machines() / metrics()<br/>runnable_count() / global_runnable_count()<br/>is_running() / sysmon_running() / sysmon_pass_count()"]
    end

    subgraph UP["上层用法"]
        U1["Go 风格 go()（tools/go）"]
        U2["channel / future / sync 的阻塞点<br/>都通过 park_wait 挂起"]
        U3["io / network / http：<br/>阻塞读写在 hook 下转为挂起"]
        U4["调试模块：DebugScheduler 代理<br/>（只用 A7 这组只读接口）"]
        U5["日志/指标：实现 SchedulerObserver"]
    end

    A1 --> U1
    A2 --> U1
    A5 --> U1
    A7 --> U4
    A6 --> U5
    A6 --> U5
    API --> UP
```

## ④ 状态流转：G / M / P

### G（Task）的状态机 —— 用户点名的"Fiber 如何在 Scheduler 流转"

```mermaid
stateDiagram-v2
    [*] --> kNew : spawn() 构造 Task + Fiber

    kNew --> kRunnable : enqueue（本地队列或全局队列）
    kRunnable --> kRunning : worker_loop resume 该 Fiber
    kRunning --> kRunnable : park_wait 返回 kRequeued<br/>（可立即继续，不进 park）
    kRunning --> kWaiting : park_wait 返回 kParked<br/>（真正切换回 worker）
    kWaiting --> kRunnable : Wake（pending-wake）<br/>或被取消后重新入队
    kRunning --> kDead : 用户函数正常返回
    kRunning --> kFailed : 用户函数抛出异常<br/>（在 Fiber 边界捕获）
    kNew --> kCancelled : 未运行即被取消
    kRunnable --> kCancelled : 排队期间被取消
    kWaiting --> kCancelled : 等待期间被取消
    kDead --> [*]
    kFailed --> [*]
    kCancelled --> [*]

    note right of kWaiting
        只有 5 个安全点可以挂起：
        park_wait 内部（协程主动让出）
        绝不强制 detach 任意 C++ 调用栈
    end note
```

### M（工作线程）与 P（处理器）的状态

```mermaid
stateDiagram-v2
    state "M（工作线程）" as M {
        [*] --> kIdle : 线程创建，未绑定 P
        kIdle --> kRunning : 绑定 P，进入 worker_loop
        kRunning --> kBlocking : 进入长系统调用
（超过 long_syscall_threshold=50ms）
        kBlocking --> kRunning : 系统调用返回，重新绑定 P
        kRunning --> kParked : 队列空且 idle 超时
（idle_worker_timeout=250ms）
        kParked --> kRunning : 有新 G 入队被唤醒
        kRunning --> kStopping : shutdown()
        kParked --> kStopping : shutdown()
        kBlocking --> kStopping : shutdown()
        kStopping --> kDead : 线程退出
        kDead --> [*]
    }

    state "P（处理器资源）" as P {
        [*] --> pIdle : 启动时创建 processor_count 个
        pIdle --> pRunning : 被某个 M 绑定
        pRunning --> pIdle : M 解绑（进入 kBlocking/kParked）
        pRunning --> pStopping : shutdown()
        pIdle --> pStopping : shutdown()
        pStopping --> pDead : 队列排空且 M 退出
        pDead --> [*]
    }
```

## 附：一次 `spawn` 到结束的时序（ASCII，补充时序细节）

```
调用方线程                worker(M)                    Fiber(G)                注册表/Observer
    |                        |                            |                          |
    |-- spawn(fn) ---------->|                            |                          |
    |                        |-- 构造 Task+Fiber ---------|                          |
    |                        |-- 登记 TaskRegistry --------------------------------->|
    |                        |-- enqueue：本地队列 or 全局                           |
    |<- shared_ptr<Task> ----|                            |                          |
    |                        |                            |                          |
    |                        |-- pop_front() 取到 G ----->|                          |
    |                        |-- resume(Fiber) ---------->|                          |
    |                        |                            |-- 首次进入: 移出 m_function
    |                        |                            |-- 调用户函数              |
    |                        |                            |   (遇阻塞 -> park_wait)   |
    |                        |<-- park_wait 返回 kParked -|                          |
    |                        |   (切回 worker_loop)       |                          |
    |                        |                            |                          |
    |                        |   [被 Wake]                |                          |
    |                        |-- 重新入队 -> resume ----->|                          |
    |                        |                            |-- 继续执行直到返回        |
    |                        |<-- state=kDead ------------|                          |
    |                        |-- emit_event(kTaskCompleted) ------------------------->|
    |<- wait() 返回 ---------|                            |                          |
```

## 与 Go 的差异（必须知道，避免误用）

| 项 | Go | Go2Cpp |
|---|---|---|
| 抢占 | 有协作式抢占点（`asyncpreempt`） | **纯协作**：只能在 5 个安全点让出，长循环不主动让出会饿死同 P 的其他 G |
| G 栈 | 可增长（copy 栈） | `Fiber` 栈**固定大小**（`fiber_stack_size`），深递归会失败 |
| work stealing | 从**队尾偷一半** | 从**队尾偷一个** |
| 观测 | `runtime/trace` 丰富 | 只有 `SchedulerObserver` 的 9 类事件，**只读**、不能控制 G |
| 时钟 | `nanotime` | `steady_clock`，**不可跨机比较** |
