# Context 局部回滚

`ContextRollback` 是 Go2Cpp 对“临时子 Context + 局部补偿动作”的受限封装。
它不是数据库事务，也不是 `longjmp`，不会恢复已经提交的父 Context 状态。

```cpp
go2cpp::ContextRollback scope(parent);
auto child = scope.context();
scope.record_undo([resource]() noexcept { resource->unregister(); });
auto mark = scope.savepoint();
scope.record_undo([resource]() noexcept { resource->reset(); });
scope.rollback_to(mark);
scope.commit();
```

> **命名约定**：`ContextRollback` 的成员同时提供 PascalCase 与
> 小写下划线两种拼写——`RecordUndo`/`record_undo`、
> `RecordUndoWithCause`/`record_undo_with_cause`、`Mark`/`savepoint`、
> `RollbackTo`/`rollback_to`、`Commit`/`commit`、`RollbackDone`/`rollback_done`。
> 小写形式是前者的内联转发，语义完全相同，本文示例统一使用小写形式。
> 另有 `committed()`、`had_failure()`、`pending_undo_count()`、`failures()`、
> `rollback_cause()`、`status_name()` 只提供小写形式，没有 PascalCase 版本
> （见 `include/go2cpp/concurrency/context.hpp` 的 `ContextRollback` 段）。

## 增强接口（2026-10-12）

以下接口在原有能力之上补齐了「**知道为什么回滚**」与「**完整诊断**」两件事，
全部为**新增**，不改变任何既有调用与行为。

### 带回滚原因的补偿动作

`record_undo_with_cause` 登记的补偿动作在执行时会收到本次回滚的原因，
便于按「超时 / 取消 / 业务失败」做不同处置：

```cpp
scope.record_undo_with_cause([&](const go2cpp::ErrorPtr& cause) {
    if (cause && go2cpp::Is(cause, go2cpp::DeadlineExceededError())) {
        metrics_.count_timeout();        // 超时：记指标、不告警
    } else {
        audit_.record_failure(cause);    // 其他：写审计
    }
    resource_.release();
});
```

`cause` 的取值按触发路径区分：

| 触发路径 | `cause` |
| --- | --- |
| 显式 `rollback(cause)` | 传入的该 `cause` |
| 父级取消 / deadline 到期（**自动**回滚） | `nullptr`（原因见 `context()->Err()`） |
| `rollback_to(mark)` 局部回滚 | `nullptr` |
| 析构自动回滚 | `nullptr` |

**两种动作共用同一条 LIFO 队列，可以混用**：先登记 `record_undo` 再登记
`record_undo_with_cause`，回滚时后者先执行。

### 观测接口

| 接口 | 作用 |
| --- | --- |
| `pending_undo_count()` | 尚未执行（或被领取执行）的补偿动作数量 |
| `failures()` | **全部**补偿异常，按发生顺序；`failure()` 仍只给第一个（历史语义不变） |
| `rollback_cause()` | 本次回滚原因；显式路径返回传入的 `cause`，自动路径回退为 `context()->Err()` |
| `status_name()` / `StatusName(s)` | 状态可读名：`active` / `rolling_back` / `committed` / `rolled_back` / `failed`，未知值返回 `unknown` |

**`failure()` 与 `failures()` 的分工**：前者的「只记第一个」是既有行为，
为兼容而保留；需要知道**所有**补偿是否失败时应使用 `failures()`。

### 登记失败的原因

`record_undo(action, &reason)` 会在失败时写入可读原因：

| 情形 | 原因文本 |
| --- | --- |
| 动作为空 | 补偿动作为空 |
| 事务已提交 | 事务已提交 |
| 事务回滚中或已回滚 | 事务已结束（回滚中或已回滚） |
| child Context 已被取消 | child Context 已被取消，事务转入回滚 |

> **注意**：父级取消通常会**先**把事务推入回滚（`status` 离开活动态），
> 因此取消之后调用 `record_undo` 多半命中「事务已结束」而不是「已被取消」；
> 后者只在取消与登记恰好竞态时出现。

### 审查与加固（同日）

本批接口完成后做了一轮**自我审查**（不凭记忆，先把改动后的代码读回来），
发现并修复 **8 处**问题：

**本次新增代码引入的 5 处**：

| 问题 | 严重度 | 修复 |
| --- | --- | --- |
| 补偿失败记录 `RecordFailure` 标 `noexcept`，内部却加锁并做容器扩容 —— 而它正是在「补偿失败」处理路径上被同样 `noexcept` 的 `Execute` 调用 | **中** | 整体 try/catch；并**先写 `first_failure` 再 `push_back`**，使扩容失败时 `had_failure()`/`failure()` 仍自洽 |
| `rollback_cause()` 未区分事务是否真的回滚过：`commit()` 之后 child 再被取消会被**误报**为本事务的回滚原因 | **中** | 先看 `status()`，`kCommitted`/`kActive` 直接返回空 |
| `PendingCount()` / `ManualCause()` 标 `noexcept` 但加锁 | 低 | try/catch 兜底 |
| `StatusName()` 的 `switch` 缺 `default`，枚举越界是 UB | 低 | 补 `default:` |

**审查中一并发现的既有 3 处**（非本批引入，属台账 **X-25** 同类）：

`ContextRollback::status()`（**`active()`/`committed()`/`rolled_back()`/`status_name()` 全部转发到它**）、
`ContextRollback::had_failure()`、`Savepoint::valid()` 都是 `noexcept` 却内部加锁。
三处已按查询型接口的惯例兜底：**拿不到锁时返回保守答案**（`kRolledBack` / `false` / 令牌无效），
而不是 terminate。**台账 X-25 因此由 7 处扩充到 10 处。**

**测试同时由 8 组加强到 16 组**，新增内容包括：`commit()` 后取消不得污染
`rollback_cause()` 的回归、移动赋值（被覆盖方**先回滚自身**）、小写别名等价性、
部分补偿失败的聚合、状态族与 `status_name()` 一致性、**4 线程 × 32 次并发登记与观测**、
以及「显式 `rollback()` 不带 cause」时原因回退到 `CanceledError` 的验证。

## 示例如何验证结果

可运行的完整示例在
[`example/concurrency/context_rollback_demo.cpp`](../example/concurrency/context_rollback_demo.cpp)。它把
回滚结果拆成以下六条可观察路径，建议新手按日志顺序阅读：

| 场景 | 关键调用 | 成功判据 | 常见错误 |
| --- | --- | --- | --- |
| 成功提交 | `commit()` | `RollbackDone()` 完成，资源仍由业务持有 | 把 `commit()` 当成取消 child，导致下游 Context 被误用 |
| 分阶段失败 | `savepoint()`、`rollback_to()`、`rollback()` | 后半段先撤销，最终只留下已经提交的资源 | 重复使用旧 savepoint；一次局部回滚后 token 已失效 |
| 父取消/超时 | 父 `Cancel()` 或 deadline | child `Done()`、`RollbackDone()` 都完成，`Err()` 为取消原因 | 只等 `Done()` 就访问已注销资源，和 undo 并发产生 UAF |
| 截止时间 | `WithTimeout()`、`WithDeadline()` | 定时器触发后自动回滚，`Err()` 为 `DeadlineExceeded` | 把 deadline 当成业务成功，继续使用已撤销租约 |
| undo 异常 | undo 抛出异常 | 后续动作仍执行，`status()==kFailed` 且 `had_failure()` 为真 | 只检查 `rollback()` 返回值，忽略补偿失败 |
| 非法调用 | 无效 token、已提交后再次登记 | 调用立即返回 false，不重复执行补偿 | 把 false 当作“稍后会成功”，继续使用已结束事务 |

最小的成功/失败分支可以这样写。关键是先判断业务结果，再决定提交还是回滚，
并在需要使用已清理资源之前等待 `rollback_done()`：

```cpp
go2cpp::ContextRollback scope(parent);
resource.acquire();
if (!scope.record_undo([&resource]() noexcept { resource.release(); })) {
    resource.release();  // 登记失败时 scope 没有可执行的 undo，必须手动释放
    return;
}

if (request_ok) {
    if (!scope.commit()) {
        // 可能已经被 parent cancel 抢先领取，必须转入失败路径。
        GO2CPP_LOG_ERROR(logger) << "commit 与取消竞争，检查 scope.status()";
    }
} else {
    scope.rollback(go2cpp::NewError("业务校验失败"));
    scope.rollback_done().WaitFor(1s);
    if (scope.had_failure()) {
        // failure() 是 std::exception_ptr；业务可在这里接入自己的异常日志。
        GO2CPP_LOG_ERROR(logger) << "补偿动作失败，必须进入人工补偿或重试";
    }
}
```

示例中的日志同时输出“动作完成”“取消原因”“补偿结束”和“最终资源数”。如果
看到 `rollback()` 返回 true 但最终状态为 `kFailed`，这是一个可预期的错误场景：
回滚请求已经被领取，但某个 undo 没有成功；业务必须记录并采取人工补偿或重试，
不能把它当成完整事务回滚。

- 构造函数创建临时子 Context。父取消或 deadline 取消时，尚未提交的 undo
  按同一回滚路径执行。
- `rollback()` 取消临时子 Context，并在 Context 锁外按 LIFO 执行全部 undo；
  重复调用幂等，不能恢复父 Context。
- `rollback_to(savepoint)` 只撤销 savepoint 之后的动作，事务仍保持活动；一次成功
  的局部回滚会使该事务中已有的 savepoint 全部失效，调用方应重新创建令牌。
- `commit()` 丢弃 undo，但不取消 child；调用方可以继续使用 `context()`。如果下游
  不再需要它，应显式取消 child，否则其 parent/deadline 注册会持续到正常生命周期结束。
- 未显式提交的析构会执行回滚。
- 只有显式调用 `record_undo()` 登记的动作会回滚；在 child 下创建的后代 Context
  deadline 不会反向取消这个 scope。需要让某个 deadline 驱动回滚时，应把带 deadline
  的 Context 作为 scope 的 parent，或显式取消 `scope.context()`。
- `RollbackDone()` 与 `Context::Done()` 分离：前者表示 commit 或完整 rollback
  的补偿动作已经结束，后者只表示 Context 取消已经线性化。取消时先冻结
  action 日志并发布 `Done`，再由内部回调执行 undo，最后才运行普通 `Done()`
  观察回调；因此普通观察回调可以等待 `RollbackDone()`。
- `Done()` 的等待者可能早于 undo 完成恢复，必须等待 `RollbackDone()` 才能使用
  已注销的资源。undo 回调本身不得等待 child `Done()`（除非它已明确知道信号
  已发布）、自身的 `RollbackDone()`，也不得依赖普通 Done 观察回调先行执行。
- `DoneSignal` 的内部回调先于普通用户回调，普通回调之间仍只保证至多调用一次，
  不保证相互顺序。

## 安全边界

Undo 回调必须幂等、短小、非阻塞，最好为 `noexcept`。回调可能在显式调用
`rollback()` 的线程、父取消线程或 Context 定时器线程执行，不能直接恢复 Fiber、
操作另一个线程专属的锁，也不能依赖已经离开的栈变量；`errno`、TLS、FiberLocal
和 M/P 绑定也不能假定属于业务 owner。调用 `Cancel()` 前必须释放外部锁，避免
undo 再取同一把锁形成同步自死锁；undo 也不能等待自身的 `RollbackDone()` 或
依赖另一个尚未发布的 `Done()`。需要由 Fiber owner 执行的清理，应只设置标志
或投递任务，再由 owner 在安全点处理。

回滚只补偿尚未对外提交的逻辑状态，例如 fd/timer/wait-node 注册、select 等待
节点和临时调度元数据。已经发生的 `send`/`write`、文件写入、网络对端状态、
信号、第三方库副作用，以及 Context 的 `Done`、`Err`、`Cause`、父 deadline 和
已发布 `Value` 都不能恢复。

undo 回调抛出的异常会被捕获，后续动作继续执行；最终状态为 `kFailed`，可通过
`had_failure()`/`failure()` 查询。异常不会穿过 Context 取消线程。
这里的异常隔离指用户 undo/观察回调；进程内存耗尽时 Context 的容器扩容可能
抛出或终止，当前没有把 OOM 伪装成可回滚的业务错误。

如果 `rollback_to()` 的局部动作抛出异常，剩余动作仍会执行，scope 回到活动状态但
被标记为失败，之后 `commit()` 会拒绝；调用方应继续显式 `rollback()`，并检查
`had_failure()`。如果并发的完整回滚或 Context 取消升级了该请求，调用方还应检查
最终 `status()`，不能只依赖 `rollback_to()` 的返回值。

当前实现不使用 `setjmp/longjmp`，不跳过 C++ 析构函数，也不跨 M/P、线程或 Fiber
迁移控制流。`commit()` 与取消/回滚竞争时，以 ContextRollback 状态锁中先完成的
状态转换者为准；已被 `DoneSignal` 取出的并发回调不能被强行撤销。

同一个事务的 `record_undo`、savepoint、`rollback` 和 `commit` 可以并发调用；事务
对象本身的移动构造、移动赋值和析构不能与这些调用并发进行。scope 必须长于所有
回调和等待者。