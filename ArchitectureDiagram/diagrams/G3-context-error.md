# G3 · 取消与错误 —— 一图看懂

> 覆盖：`Context`（取消传播 / 值 / 截止时间）、`DoneSignal`（完成信号与回调）、
> `ContextRollback`（可撤销的上下文事务）、`Error` / `ErrorPtr` / `StringError` / `WrappedError` / `JoinError`。
> 源码：`include/go2cpp/concurrency/context.hpp`、`tools/error.hpp`。

## ① 类架构：每个类用的底层能力 + 主要职责

```mermaid
classDiagram
    direction TB

    class Error {
        <<错误基类>>
        +message() string
        +Visit(visitor) ErrorVisitResult
    }
    class StringError {
        <<最简错误>>
    }
    class WrappedError {
        <<包装下层错误>>
        +Unwrap() ErrorPtr
    }
    class JoinError {
        <<多错误合并>>
        +Errors() vector~ErrorPtr~
    }
    class ErrorPtr {
        <<shared_ptr~const Error~>>
    }

    class DoneSignal {
        <<完成信号>>
        -atomic~bool~ m_done
        -mutex + 回调表
        -deque~Callback~
        +IsDone() bool
        +Wait() void
        +WaitFor(timeout) bool
        +WaitUntil(deadline) bool
        +AddCallback(cb) CallbackId
        +RemoveCallback(id) void
        +Signal() void
        ← 回调在 Signal 后于**锁外**执行
    }

    class Context {
        <<取消与值>>
        -shared_ptr~impl~ m_impl
        -ContextPtr m_parent
        +IsDone() bool
        +Err() ErrorPtr
        +Cause() ErrorPtr
        +HasDeadline() bool
        +Cancel(cause) void
        +CancelDeadline() void
        +WithValue(key, value) ContextPtr
        +Value(key) any
        +Done() DoneSignal
        +context() ContextPtr
        ← 取消沿父子链向下游传播
    }

    class ContextRollback {
        <<可撤销事务>>
        -vector~UndoAction~ m_undo
        +RecordUndo(action, reason) bool
        +RollbackTo(mark) bool
        +Rollback(cause) bool
        +Commit() bool
        +active() bool
        +rollback_cause() ErrorPtr
        ← 用于"失败要把已做的步骤退回去"的场景
    }
    class Savepoint {
        <<回滚点标记>>
        +valid() bool
    }

    class ParkingCondition {
        <<G1 提供的等待原语>>
        +wait(lock, pred) bool
    }

    DoneSignal ..> ParkingCondition : Wait 用它的等待路径
    Context *-- DoneSignal : 每个 Context 一个完成信号
    Context --> Context : 父链（取消向下传播）
    Context ..> ErrorPtr : Err/Cause 返回错误
    StringError --|> Error
    WrappedError --|> Error
    JoinError --|> Error
    WrappedError ..> ErrorPtr : 包住下层
    JoinError ..> ErrorPtr : 合并多个
    ContextRollback ..> Savepoint
    ContextRollback ..> ErrorPtr : rollback_cause
```

| 类 | 主要职责 | 用的底层能力 |
|---|---|---|
| `DoneSignal` | 「这件事做完了/被取消了」的可等待信号；支持**回调注册**（`CallbackId` 可撤销） | 原子量、`mutex`、**`ParkingCondition`**（让受管 G 等着不占线程） |
| `Context` | 取消传播（父取消→子取消）、`WithValue` 存取值、截止时间 | `DoneSignal`、父链 `shared_ptr` |
| `ContextRollback` | 记录 undo 动作，失败时按 `Savepoint` 回滚已完成的步骤 | `Error`、`vector<UndoAction>` |
| `Error` 家族 | 错误类型；`StringError` 最简、`WrappedError` 可解包、`JoinError` 可合并 | `shared_ptr<const Error>` 不可变共享 |

## ② 重要函数：功能与实现要点

```mermaid
flowchart TB
    subgraph CANCEL["Context::Cancel(cause)"]
        C1["持 impl 锁，置 done=true<br/>并记录 cause"] --> C2["把自己的 DoneSignal 置位<br/>（唤醒所有 Wait 者 + 触发回调）"]
        C2 --> C3["递归向下游子 Context 传播<br/>（子链在 impl 里维护）"]
        C3 --> C4["回调在**锁外**逐个执行<br/>（避免回调里再取锁导致死锁）"]
    end

    subgraph SYNC["DoneSignal::Signal()"]
        D1["置 m_done = true（release）"] --> D2["把回调表整体移出到局部变量"]
        D2 --> D3["释放锁"]
        D3 --> D4["在锁外执行每个回调<br/>异常被逐个捕获"]
        D4 --> D5["唤醒所有在 Wait/WaitFor 上的等待者"]
    end

    subgraph WAIT["DoneSignal::Wait()"]
        W1{"已 IsDone?"} -- 是 --> W2["立即返回"]
        W1 -- 否 --> W3["加锁注册一个 waiter<br/>或用 ParkingCondition 等待"]
        W3 --> W4["被 Signal 唤醒后检查 done<br/>（防虚假唤醒）"]
        W4 --> W2
    end

    subgraph ROLL["ContextRollback::Rollback(cause)"]
        R1["从最近一个 Savepoint 开始"] --> R2["逆序执行 UndoAction"]
        R2 --> R3["记录 rollback_cause<br/>（第一个失败原因）"]
        R3 --> R4{"全部 undo 成功?"}
        R4 -- 是 --> R5["active=false，返回 true"]
        R4 -- 否 --> R6["返回 false，但已执行的 undo 不重复"]
    end

    CANCEL --> SYNC
    SYNC --> WAIT
    ROLL --> R2
```

**要点**

- 回调必须**在锁外**执行：否则回调里再调 `AddCallback`/`Cancel` 就会自死锁；
- `Signal` 的回调表是**整体移出**再执行，保证"执行期间新注册的回调"不会被这次 Signal 触发（语义清晰）；
- `CallbackId` 可 `RemoveCallback`，用于"对象销毁前撤销悬挂回调"（`future.hpp` 里的 `FutureCallbackGuard` 就是干这个）；
- `Wait` 用 `ParkingCondition` 而不是裸 `condition_variable`，所以**受管 G 等待时不占工作线程**；
- `ContextRollback` 的 undo 是**逆序**执行，且记录第一个失败原因而不中断（尽力回滚）。

## ③ 向上层提供了什么

```mermaid
graph LR
    subgraph G3API["取消与错误能力"]
        A1["Context：取消传播 + 值 + 截止时间"]
        A2["DoneSignal：可等待 + 可撤销回调"]
        A3["ContextRollback：撤销/回滚/提交"]
        A4["Error 家族：字符串/包装/合并 + Visitor"]
    end
    subgraph USE["谁在用"]
        U1["scheduler：Task 的取消请求<br/>（cancellation_requested）"]
        U2["sync / channel / future：<br/>等待时支持取消，取消后返回 false"]
        U3["io / network：<br/>超时与连接断开都走 Context"]
        U4["init / 配置加载：<br/>部分失败要回滚已创建的资源"]
        U5["调试模块：DebugScheduler 的事件里带 GState<br/>；锁观测不涉及 Context"]
    end
    A1 --> U1
    A1 --> U2
    A1 --> U3
    A2 --> U2
    A3 --> U4
    A4 --> U1
    A4 --> U3
```

## ④ 状态流转

### Context 的生命周期

```mermaid
stateDiagram-v2
    [*] --> kActive : WithCancel / WithDeadline / Background
    kActive --> kActive : WithValue（派生新 Context，父仍 active）
    kActive --> kCancelled : Cancel(cause)（显式）
    kActive --> kDeadlineExceeded : 到达 deadline（CancelDeadline）
    kActive --> kCancelled : 父 Context 取消（向下传播）
    kCancelled --> [*] : 所有下游都收到取消
    kDeadlineExceeded --> [*]

    note right of kCancelled
        Err() 返回取消原因；
        再次 Cancel 是幂等的（保留第一个原因）
    end note
```

### ContextRollback 的状态

```mermaid
stateDiagram-v2
    [*] --> kIdle : 构造（active=false）
    kIdle --> kActive : 记录第一条 UndoAction
    kActive --> kActive : 继续 RecordUndo
    kActive --> kCommitted : Commit()（丢弃 undo 动作）
    kActive --> kRolledBack : Rollback()（逆序执行 undo）
    kActive --> kRolledBack : RollbackTo(mark)（回滚到某个 Savepoint）
    kCommitted --> [*]
    kRolledBack --> [*]
```

## 与 Go 的差异

| 项 | Go | Go2Cpp |
|---|---|---|
| `context.Context` | 接口 + `context.WithCancel` 等 | `Context` 具体类 + `ContextPtr` 共享 |
| 取消检查 | `select { case <-ctx.Done(): }` | `DoneSignal::Wait/IsDone`，或等待 API 的返回值 |
| 错误链 | `errors.Is/As` + `%w` | `WrappedError::Unwrap` + `Visit(visitor)` |
| 事务回滚 | 无标准库 | `ContextRollback`（多 undo 动作 + Savepoint） |
