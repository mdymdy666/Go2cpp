# Context 状态机

- 源文件：`src/concurrency/context.cpp`（类声明在 `include/go2cpp/concurrency/context.hpp`）
- 关键函数：`Context::Background/TODO/WithCancel/WithCancelCause/WithDeadline/WithTimeout`、
  `Context::State::Cancel`（内部）、`TimerService` 到期回调、`DoneSignal::Signal`
- 相关类：`Context`（门面）、`Context::State`（共享状态）、`DoneSignal`（完成信号）

## 1. Context 的生命周期状态

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| — | `Background()` | Active(root) | 永不自动取消的根节点 |
| — | `TODO()` | Active(root) | 占位根节点，语义同 Background |
| Active | `WithCancel(parent)` | Active(child) | 挂到父节点 `children`（弱引用） |
| Active | `WithCancelCause(parent)` | Active(child) | 同上，额外记录 `cause` |
| Active | `WithDeadline(parent, t)` | Active(child) | 额外注册 `TimerService`（`timer_id`） |
| Active | `WithTimeout(parent, d)` | Active(child) | `WithDeadline(now + d)` 的包装 |
| Active | `Cancel()`（用户调用） | Canceled | `done.Signal()` → 级联全部 children |
| Active | `TimerService` 到期 | Canceled(DeadlineExceeded) | 定时器线程回调里调 Cancel |
| Active | 父节点 Cancel | Canceled | 沿 `children` 级联，在**取消线程**上同步完成 |
| Canceled | `Cancel()` | Canceled | 幂等空操作（`State::error` 已非空） |

**不变量**

- `error != nullptr` ⇔ 已取消；`Err()` 返回它，`Cause()` 返回 `cause`（可能为空）。
- 取消单向：Canceled 不会回到 Active。
- `root == true` 的节点不会被父级联取消（没有父）。
- `deadline` 一旦设置不可改；到期前 Cancel 会 `TimerService::Remove(timer_id)`。

## 2. DoneSignal 状态机

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Pending | `Signal()` | Done | 搬出两组回调 → `cv.notify_all()` → 先 internal 后 user |
| Pending | `AddCallback(cb)` | Pending | 登记到 `callbacks`，`Signal` 时执行 |
| Pending | `AddCallbackImpl(cb, internal=true)` | Pending | 登记到 `internal_callbacks`（先执行） |
| Done | `AddCallback(cb)` | Done | **立即执行一次**回调（不登记） |
| Done | `Signal()` | Done | 幂等空操作 |
| Done | `Wait()/WaitFor()/WaitUntil()` | Done | 立即返回 true |

**不变量**

- `internal_callbacks` 与 `callbacks` 都在 `Signal()` **释放 mutex 之后**执行 —— 回调里可以安全地重入信号对象。
- 回调抛出的异常被吞掉，不阻断其余回调，也不让 `Signal()`（`noexcept`）终止进程。

## 3. 与调度器的交互点

- `DoneSignal::State::cv` 是 `core::ParkingCondition`：受管 G 调用 `Wait*` 时**park 并让出 M**，
  普通线程走原生条件变量。因此「等待取消」不会占住 worker。
- 取消级联发生在**调用 `Cancel()` 的那个线程/那个 Fiber** 上；回调里只应做 `Notify()` 一类
  非阻塞动作，否则会把取消线程拖住。

## 4. 非法 / 边界行为

| 场景 | 实际行为 |
|---|---|
| 对已取消的 Context 再 `Cancel()` | 空操作（`error` 已非空） |
| 子节点 Cancel 后父节点仍 Active | 正常：子节点取消不上传到父（单向） |
| `WithDeadline` 传入过去时间 | 创建后**立即** Cancel（由 `WithDeadline` 内部检查） |
| `WithTimeout(max)` 未饱和加法 | 已修复：`ContextDuration::max()` 用饱和加法，避免回绕成立即超时 |
