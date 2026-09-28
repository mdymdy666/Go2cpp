# Context 局部回滚

`ContextRollback` 是 Go2Cpp 对“临时子 Context + 局部补偿动作”的受限封装。
它不是数据库事务，也不是 `longjmp`，不会恢复已经提交的父 Context 状态。

```cpp
go2cpp::ContextRollback scope(parent);
auto child = scope.context();
scope.record_undo([resource] noexcept { resource->unregister(); });
auto mark = scope.savepoint();
scope.record_undo([resource] noexcept { resource->reset(); });
scope.rollback_to(mark);
scope.commit();
```

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
