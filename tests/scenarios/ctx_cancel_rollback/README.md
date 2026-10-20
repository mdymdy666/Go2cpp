# ctx_cancel_rollback：Context 取消 + 回滚解死锁

本场景演示**有主锁**的锁顺序死锁如何被安全解开：Doctor 只取消 Context，
被取消方用 `ContextRollback` 的 undo 释放自己持有的锁，另一个线程继续推进。

与 `tests/scenarios/deadlock_routes` 的区别：那边演示「owner 已死 → 人工
`mutex-unlock`」的故障处置；本场景的 owner **仍然存活**，恢复手段是业务自己
的取消/回滚路径，因此没有 stale-unlock。

## 场景图

```text
   线程 A(Fiber gid=1):  Lock(L1) ── 业务 ── Lock(L2, ctxA)   ← 阻塞
   线程 B(Fiber gid=2):  Lock(L2) ── 业务 ── Lock(L1, ctxB)   ← 阻塞
   主线程/Doctor:        deadlock-report → verdict=deadlock
                         ctx.cancel gid=1   （只取消，不 Unlock）
   ------------------------------ 取消之后 ------------------------------
   A: Lock(L2, ctx) 返回 false；ctx.Err() = "context canceled"
      ContextRollback 的 undo 在取消线程上同步执行 → L1.Unlock()
   B: Lock(L1, ctx) 返回 true → 业务完成 → 释放 L1/L2
```

关键点：**两把锁的持有都通过 `ContextRollback::RecordUndo` 绑定补偿动作**，
Doctor 不调用任何 `Unlock()`。`ctx.cancel` 只做 `Context::Cancel()`。

## 三种模式

| 模式 | 驱动方式 | 期望结果 |
|---|---|---|
| `--mode=cancel`（默认） | 主线程在进程内 `doctor.Execute(...)` | A 回滚（undo 执行、L1 释放），B 完成；锁收支平衡、无泄漏 |
| `--mode=external` | 外部 `doctor_client.py` 经真实 Unix socket | 同上；另验证 `ctx.cancel` 的拒绝路径（未注册 gid / 非法 gid / 未注册地址 / 缺参数） |
| `--mode=force` | `mutex-unlock name=L1 --force reason=...` | **对照组**：A 失去 L1（stale），undo 被丢弃、A 跳过 Unlock；锁仍平衡 |

## 编译与运行

```sh
cmake -S . -B build-doctor-full \
      -DGO2CPP_DIAGNOSTIC=ON -DGO2CPP_DEBUG_DUMP=ON
cmake --build build-doctor-full --target go2cpp_ctx_cancel_rollback -j

# 三个模式一起验收（cancel + force + external 拒绝路径）
bash tests/scenarios/ctx_cancel_rollback/run_ctx_cancel_rollback.sh \
     build-doctor-full
```

本目标**不注册 ctest**：`ctest` 用例总数保持 116，端到端验收由脚本外部驱动
（与 `deadlock_routes` / `full_trace` 的约定一致）。

## 关键输出（cancel 模式实测）

stdout 阶段行：

```text
READY pid=... mode=cancel doctor_socket=/tmp/ctx-cancel-demo.sock
DEADLOCK_READY gid_a=1 gid_b=2 L1.owner=1 L2.owner=2 L1.waiters=1 L2.waiters=1
DEADLOCK_REPORT verdict=deadlock
ACTION command=ctx.cancel gid=1 ok=1
RESULT mode=cancel pass=1 a_completed=1 b_completed=1 a_lock2_ok=0
       a_lock2_false=1 ctx_err=1 undo_executed=1 rollback_status=rolled_back
       pending_undo=0 rollback_failures=0 stale=0 a_still_owns_l1=0
       locks_acquired=3 locks_released=3 force_unlocks=0 balanced=1
       l1_held=0 l1_waiters=0 l1_native_waiters=0 l2_held=0 l2_waiters=0
       l2_native_waiters=0 shared_work=1 final_verdict=progressing
```

stderr 关键行（顺序即因果）：

```text
[undo] A: L1 released by ContextRollback undo (cancel thread=..., gid=1)
[B] COMPLETED: acquired L1 after the peer released it (commit=1 undo_executed=1)
[go2cpp][doctor][audit] ctx.cancel name=worker-A gid=1 address=0x...
    matched_by=gid actor=doctor pid=... ts_ns=... wall_ms=... was_done=false
    now_done=true cancels_total=1
[A] Lock(L2)=false ctx.Err=1(context canceled) rollback=1
    status=rolled_back pending_undo=0 failures=0
```

`ctx.cancel` 响应（文本模式，节选）：

```text
schema=go2cpp.diagnostic.ctx-cancel/1
verdict=cancelled
context.name=worker-A
context.gid=1
context.address=0x...
context.matched_by=gid
context.was_done=false
context.now_done=true
audit.actor=doctor
audit.pid=...
audit.ts_ns=...
audit.wall_ms=...
audit.cancel_thread=doctor-executor
contexts_registered=1
cancels_total=1
```

## 取消与「已经拿到锁」的竞争（必须处理）

`Lock(ctx)` 返回 true **不豁免**取消检查：正常 Unlock 唤醒与 DoneSignal 的取消
回调是两个独立事件。极端时序下 A 会在取消线性化之后仍拿到 L2（本场景记为
`cancel-vs-unlock race`）；此时 `ctx.Err()` 非空，业务必须立刻释放并放弃这次
临界区，否则就会像 force 路径一样带着「已失去 L1」的错误假设继续跑。

本场景在 A 的 `Lock` 返回后**无条件检查 `ctx.Err()`**：

```cpp
const bool locked = g_l2.Lock(ctx);
const ErrorPtr error = ctx->Err();
if (!locked || error != nullptr) {
    if (locked) {          // 拿到锁但取消已发生：立刻归还
        g_l2.Unlock();
    }
    rollback.Rollback();   // L1 已由取消线程的 undo 释放
}
```

RESULT 行的 `cancel_race=1` 表示这次运行命中了该分支；两条路径都断言
`ctx_err=1`、`undo_executed=1`、`stale=0`、锁收支平衡。本场景修复前的一次
external 运行曾真实触发该竞争（表现为 `[A] STALE`），因此这不是假设分支。

## 与 `mutex-unlock --force` 的差异（force 模式实测）

| 观测 | `ctx.cancel` | `mutex-unlock --force` |
|---|---|---|
| A 的 `Lock(L2)` | 返回 **false**，`ctx.Err()` 非空 | 返回 true（它还在等 L2，最后也拿到了） |
| A 的 L1（回滚） | undo 执行，L1 被合法释放 | undo 被丢弃，A 发现自己已不持有 L1 |
| `undo_executed` | 1 | 0 |
| `stale` | 0 | 1（`[A] STALE: L1 no longer owned`） |
| A 之后的业务 | 取消路径不再执行第二段业务 | 仍继续执行（`g_a_business` +10） |
| 锁收支 | acquired=3 / released=3 | acquired=4 / released=3 + force_unlocks=1 |
| 风险 | 无（业务自己感知取消） | owner 不知道锁被夺走，靠业务侧的 stale 检测兜底 |

force 路径的审计行：

```text
[go2cpp][doctor][audit] mutex-unlock --force name=L1 reason=contrast-force-path
    owner_gid=1 owner_known=true waiters_before=1 waiters_after=0
    held_after=false forced_total=1
```

## 限制（如实声明）

1. **只对已注册 Context 生效**：`ctx.cancel` 按 gid / name / address 在
   `DiagnosticAgent::RegisterContext` 的登记表里匹配；未注册的一律
   `not-registered`，`--address` 也**不会**解引用任意指针。
2. **运行时不维护「G 当前 Context」的权威映射**：gid→Context 的关联由业务在
   `RegisterContext` 时显式给出（本场景 A 在 Fiber 内注册自己的 gid）。拿不到
   关联时用 `name=` 或 `--address <已注册指针>` 兜底。
3. **取消不等于杀死执行流**：`ctx.cancel` 只把 `Done` 置位并唤醒等待者；
   业务必须检查 `Lock/Wait` 的返回值、自己走回滚/退出路径。停留在纯 CPU
    循环里、不检查取消点的代码不会被这条命令打断。
4. **undo 在取消线程上同步执行**：`ContextRollback` 的 undo 由
   `DoneSignal` 内部回调执行，因此 `ctx.cancel` 的响应时间包含 undo 执行时间。
   undo 必须短小、非阻塞、幂等（本场景里只有 `Unlock()` 和计数）。
5. **回滚顺序由业务决定**：`ContextRollback` 只负责 LIFO 执行登记的动作，
   不会替业务判断「哪些锁还能解锁」。force 对照组里 A 必须先检查 L1 是否
   仍归自己，再决定是否 `Unlock`。
6. **Doctor 仍然只读运行时状态**：本场景里 Doctor 没有新增任何写锁状态的
   路径；`ctx.cancel` 写的是业务显式交给它的 Context。
