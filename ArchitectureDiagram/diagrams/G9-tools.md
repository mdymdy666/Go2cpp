# G9 · 工具与新手层 —— 一图看懂

> 覆盖：`tools/go`（`go()` / `fiber` / `DefaultSchedulerState`）、`tools/control_flow`（`defer`）、
> `tools/event`（`EventBatch` / `SelectLoop`）、`core/hybrid_mutex`（`HybridGate` / `HybridMutex`）、
> `fiber_local`（`FiberLocalCache` / `FiberLocal<T>`）、`thread_policy`
> （`ThreadParticipationMode` / `ThreadHookMode` / `ThreadPolicySnapshot` /
> `ScopedThreadParticipation` / `ScopedThreadHookMode`）。
> 源码：`include/go2cpp/tools/go.hpp`、`tools/control_flow.hpp`、`tools/event.hpp`、
> `concurrency/core/hybrid_mutex.hpp`、`concurrency/fiber_local.hpp`、`concurrency/thread_policy.hpp`。

**这一层是「让 Go 代码几乎原样能编译」的那一层** —— 提供 `go`、`defer`、`select` 的近似物，
以及"这个线程要不要参与运行时"的显式控制。

## ① 类架构：每个类用的底层能力 + 主要职责

> 本模块共 **18 个类型**，一次性画会被 Mermaid 横向铺开（实测 784×230，文字不可读）。
> 按职责拆成 **3 张**（每张 5~7 类，一屏可读）。

### ①-A 启动协程：`go()` 与 `defer`

```mermaid
classDiagram
    direction TB

    class Scheduler {
        <<G2>>
        +spawn(fn, opts) shared_ptr~Task~
    }
    class DefaultSchedulerState {
        <<进程默认调度器>>
        +instance() Scheduler&
        ← go() 不指定调度器时用它
    }
    class go_fn {
        <<自由函数 go(fn, ...)>>
        +go(fn) -> shared_ptr~Task~
        +go(scheduler, fn)
    }
    class fiber {
        <<go 的返回值包装>>
        +wait() bool
        +cancel() void
        +join()
    }
    class defer_guard {
        <<RAII：作用域退出时执行>>
        +defer() 注册
        ← 只能作用域级，不是 Go 的函数级
    }

    go_fn ..> Scheduler : 调 spawn
    go_fn ..> DefaultSchedulerState : 缺省调度器
    go_fn ..> fiber : 返回包装
```

### ①-B `select` 近似物与混合互斥

```mermaid
classDiagram
    direction TB

    class Channel {
        <<G4>>
    }
    class TimerService {
        <<G1>>
    }
    class Context {
        <<G3>>
    }
    class ParkingCondition {
        <<G1>>
    }
    class EventBatch {
        <<select 的近似物>>
        +AddChannel(ch, op, handler)
        +AddTimer(deadline, handler)
        +AddContext(ctx, handler)
        +Wait()
        ← 每次 Wait 只取一个就绪事件
    }
    class SelectLoop {
        <<EventBatch 的别名>>
    }
    class HybridGateOptions {
        <<策略>>
    }
    class HybridGate {
        <<混合互斥：G 用 park，普通线程用 condvar>>
        +lock / unlock
        +try_lock
    }
    class HybridMutex {
        <<HybridGate 的别名>>
    }

    SelectLoop ..> EventBatch : 别名
    EventBatch ..> Channel : 监听可读/可写
    EventBatch ..> TimerService : 定时分支
    EventBatch ..> Context : 取消分支
    HybridMutex ..> HybridGate : 别名
    HybridGate ..> ParkingCondition : 受管 G 路径
    HybridGateOptions ..> HybridGate : 配置
```

### ①-C 协程局部存储与线程参与策略

```mermaid
classDiagram
    direction TB

    class KeyId {
        <<协程局部变量键>>
    }
    class Fiber {
        <<G1>>
    }
    class FiberLocalCache {
        <<按 G 隔离的存储>>
        +Get(key) T&
        +Set(key, value)
        ← 每个 G 一份，普通线程另用线程局部
    }
    class FiberLocal {
        <<FiberLocal~T~ 别名>>
    }
    class ThreadParticipationMode {
        <<线程是否参与运行时>>
    }
    class ThreadHookMode {
        <<线程是否参与阻塞 hook>>
    }
    class ThreadPolicySnapshot {
        <<策略快照>>
    }
    class ScopedThreadParticipation {
        <<RAII 切换参与模式>>
    }
    class ScopedThreadHookMode {
        <<RAII 切换 hook 模式>>
    }

    FiberLocal ..> FiberLocalCache : 别名
    FiberLocalCache ..> KeyId
    FiberLocalCache ..> Fiber : 按 G id 索引
    ScopedThreadParticipation ..> ThreadParticipationMode
    ScopedThreadHookMode ..> ThreadHookMode
    ThreadPolicySnapshot ..> ThreadParticipationMode
```


**类职责汇总（18 个类型）**

| 类/函数 | 主要职责 | 用的底层能力 |
|---|---|---|
| `go(fn)` / `go(scheduler, fn)` | Go 风格启动协程；返回可 `wait`/`cancel` 的 `fiber` | `Scheduler::spawn`、`DefaultSchedulerState` |
| `DefaultSchedulerState` | 未显式传 scheduler 时的进程级默认实例 | `Scheduler` |
| `fiber` | `go()` 的返回值包装，提供 `wait`/`cancel`/`join` | `Task` |
| `defer` | **作用域退出时执行**的 RAII 守卫 | 析构函数 |
| `EventBatch`（`SelectLoop`） | `select` 的近似：注册多个 channel/timer/context 分支，`Wait` 取**一个**就绪 | `Channel`、`TimerService`、`Context` |
| `HybridGate`（`HybridMutex`） | **两种等待路径**：受管 G 走 park（不占线程），普通线程走原生 condvar | `ParkingCondition` + `std::mutex` |
| `FiberLocalCache`（`FiberLocal<T>`） | 按 **G** 隔离的存储（普通线程则按线程隔离） | G id → 值表 |
| `KeyId` | 协程局部变量的键类型 | 无 |
| `ThreadParticipationMode` / `ThreadHookMode` | 声明「这个线程参不参与运行时 / hook」 | 线程局部标志 |
| `ThreadPolicySnapshot` | 策略快照（用于观测） | 无 |
| `ScopedThreadParticipation` / `ScopedThreadHookMode` | RAII 切换并自动恢复 | 线程局部标志 |

## ② 重要函数：功能与实现要点

```mermaid
flowchart TB
    subgraph GO["go(fn) 的路径"]
        G1["取默认调度器（或用户传入）"] --> G2["Scheduler::spawn(fn)"]
        G2 --> G3["返回 Task 的包装 fiber"]
        G3 --> G4{"用户调用 wait()?"}
        G4 -- 是 --> G5["Task::wait -> 阻塞到终态"]
        G4 -- 否 --> G6["detach：只保留注册表强引用<br/>G 仍会被执行完"]
    end

    subgraph DEF["defer 的实现"]
        D1["defer(lambda) 构造一个守卫对象"] --> D2["守卫持有 lambda"]
        D2 --> D3["作用域退出 -> 守卫析构 -> 执行 lambda"]
        D3 --> D4{"lambda 抛异常?"}
        D4 -- 是 --> D5["**捕获并吞掉**（析构函数不能抛）<br/>这一点与 Go 的 defer 不同"]
        D4 -- 否 --> D6["正常继续"]
    end

    subgraph SEL["EventBatch::Wait 的实现"]
        S1["把每个分支注册成 SelectWaitState<br/>（Arm 上）"] --> S2["尝试一次性检查是否已有就绪"]
        S2 --> S3{"有就绪?"}
        S3 -- 是 --> S4["CAS 认领一个（单赢家）"]
        S3 -- 否 --> S5["park 等待任意一个被唤醒<br/>（生成计数 m_generation 防陈旧唤醒）"]
        S5 --> S4
        S4 --> S6["Disarm 其余分支并回滚它们的注册"]
        S6 --> S7["调用对应分支的 handler，返回"]
        S7 --> S8["下一轮 Wait 重新 Arm（select 在 for 里用）"]
    end

    subgraph HYB["HybridGate::lock 的双路径"]
        H1{"当前是受管 G?"}
        H1 -- 是 --> H2["走协程路径：CAS 快路径 -> 有界重试 -> park"]
        H1 -- 否 --> H3["走线程路径：std::mutex + condition_variable"]
        H2 --> H4["两条路径**共享同一个门状态**<br/>因此 G 与普通线程可以互斥"]
        H3 --> H4
    end

    GO --> DEF
    DEF --> SEL
    SEL --> HYB
```

**要点（含与 Go 的真实差异）**

- `defer` 是 **RAII**：它绑定**作用域**，不是 Go 的"函数退出"；并且**析构函数不能抛异常**，所以 defer 里的异常会被吞掉 —— 这是**必须知道的行为差异**；
- `go(fn)` 不 `wait` 时 G 仍会被执行完（注册表强持有），但**没有句柄可以取消**；
- `EventBatch::Wait` 每次只取**一个**就绪分支，所以典型用法是 `while (true) { batch.Wait(); }`；
- `select` 的"随机选一个就绪分支"**没有实现**（本项目是**先到先得**），这一点在 G4 里也标注过；
- `HybridGate` 的价值是**同一把锁既能给 G 用、也能给普通线程用**，且两条路径共享同一个门状态。

## ③ 向上层提供了什么

```mermaid
graph LR
    subgraph G9API["新手层能力"]
        A1["go(fn) / fiber：一行起协程"]
        A2["defer：作用域退出执行"]
        A3["EventBatch：select 的近似"]
        A4["HybridMutex：G 与线程共用一把锁"]
        A5["FiberLocal<T>：按 G 隔离的存储"]
        A6["thread_policy：显式声明线程参与模式"]
    end
    subgraph USE["谁在用"]
        U1["迁移 Go 代码：几乎可以直接写 go/defer"]
        U2["需要在 select 里等多种事件：EventBatch"]
        U3["库代码同时被 G 与普通线程调用：HybridMutex"]
        U4["要「每协程一份」状态：FiberLocal"]
        U5["嵌入到已有线程模型的宿主：thread_policy"]
        U6["调试模块：不直接用它们；<br/>但示例与测试用 go() 起任务"]
    end
    A1 --> U1
    A2 --> U1
    A3 --> U2
    A4 --> U3
    A5 --> U4
    A6 --> U5
    A1 --> U6
```

## ④ 状态流转

### EventBatch 的一次 Wait

```mermaid
stateDiagram-v2
    [*] --> kBuilding : 构造并 Add 分支
    kBuilding --> kArmed : Wait 开始，逐个 Arm
    kArmed --> kClaiming : 已有分支就绪（或 park 后被唤醒）
    kClaiming --> kDisarm : CAS 认领成功（单赢家）
    kDisarm --> kDispatched : 调用 handler
    kDispatched --> [*] : 本次 Wait 结束（下一次重新 kBuilding/kArmed）
    kArmed --> kCancelled : Context 取消
    kCancelled --> kDisarm
```

### HybridMutex 的双路径

```mermaid
stateDiagram-v2
    [*] --> kFree : 初始
    kFree --> kHeldByG : 受管 G 获得
    kFree --> kHeldByThread : 普通线程获得
    kHeldByG --> kFree : G 释放
    kHeldByThread --> kFree : 线程释放
    kHeldByG --> kFree : G 挂起前释放（等待时不能持锁）
    note right of kFree
        同一门状态让两种持有者互斥；
        等待者按自身类型走 park 或 condvar
    end note
```

## 与 Go 的差异

| 项 | Go | Go2Cpp |
|---|---|---|
| `defer` | **函数级**，可多次、逆序执行，panic 时也执行 | **RAII 作用域级**；析构不抛，异常被吞 |
| `select` | 多个就绪分支**随机选一个** | `EventBatch`：**先到先得**，无随机化 |
| `goroutine` 句柄 | 无（不能 wait/cancel 单个 goroutine） | `fiber` 可 `wait`/`cancel` |
| 协程局部存储 | 无（要靠显式传参） | `FiberLocal<T>` |
| 跨线程锁 | 无 `HybridMutex` 概念 | `HybridMutex`：G 与普通线程共用一把锁 |
| 线程参与 | 运行时不区分 | `thread_policy` 显式声明 |
