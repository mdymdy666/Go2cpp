# scheduler 模块审查报告

- 审查对象：`src/concurrency/scheduler.cpp`（4597 行）、`include/go2cpp/concurrency/scheduler/scheduler.hpp`（968 行）
- 基线：`a2fa0df`；本模块文件 md5 与审查快照一致（同期 Bento 工作区有并发改动，不影响本模块行号）
- 审查方式：只读逐函数审查 + 子审查交叉验证；覆盖 Task 状态机、worker_loop、enqueue/requeue、shutdown、park/wake、sysmon/BlockingRegion 抽查；未运行压测/TSAN
- 统计：**P0×2 / P1×4 / P2×6 / P3×1，共 13 条**

## 1. 问题清单

| ID | 级别 | 位置 | 问题 | 证据 | 建议修复 |
|---|---|---|---|---|---|
| SCHED-01 | P0 | `scheduler.cpp:3981,4121-4131,2621` | 慢路径持有 admission mutex 时调用 `recover_unclaimed()`→`enqueue()`，后者在 worker 分支再次锁同一个非递归 `m_impl->mutex`，同线程自死锁 | 3981 `std::lock_guard<std::mutex> admission_lock(m_impl->mutex);` 作用域覆盖 4121-4131；`recover_unclaimed` 3820-3822 在 `need_republish` 时 `(void)enqueue(task)`；`enqueue` 2456-2457 判定 `from_worker` 为 true 后 2621 再次 `std::unique_lock<std::mutex> admission_lock(m_impl->mutex);` | `recover_unclaimed` 只做判定：把待补发布任务收集到局部 vector，释放 admission 锁后再逐个入队；或新增 `enqueue_locked()` 变体供已持锁路径使用 |
| SCHED-02 | P0 | `scheduler.cpp:3877-3919,2571-2572,1781,2113-2120` | incoming 中路径持 `incoming_mutexes[stripe]` 时调用 `recover_unclaimed()`→`enqueue()`（锁 admission）；而 `shutdown_for` 持 admission 调 `begin_shutdown_locked()` 锁同一 incoming 条带，构成反向锁序，可死锁 | `take_incoming` 的 `incoming_lock`（3877-3878）覆盖 3912-3917 的 `recover_unclaimed(task)`；`enqueue` worker 分支 2621 锁 `m_impl->mutex`；`shutdown_for` 2113 持 `m_impl->mutex` → `begin_shutdown_locked` 1781 锁 `incoming_mutexes[stripe]` | 同 SCHED-01：补发布移出 incoming 锁；`begin_shutdown_locked` 的 incoming 扫描改为「先摘出队列、后处理」，统一锁序 |
| SCHED-03 | P1 | `scheduler.cpp:2895-2915,1816-1852,2182-2184` | `park_wait_for_cleanup` 缺少 draining/stopping 二次检查；shutdown 的 registry 扫描只执行一次，若扫描先于 park 提交 `kWaiting`，G 停在 Waiting 且无唤醒源，`shutdown()`（无超时）可永久阻塞 | `park_wait_for_cleanup` 2903 `park_for_scheduler()` 后直接 2908 `Fiber::SuspendForScheduler`，注释 2890-2891 明确「刻意不因取消/draining 拒绝」；对比 `park_wait` 2872-2878 有二次检查并 `wake_for_wait()` 回滚；`begin_shutdown_locked` 只在 2119 执行一次 | 补与 `park_wait` 相同的二次检查并在失败时 `wake_for_wait()` 回滚；或让 shutdown 在 drain 阶段循环重扫 registry 直至无状态变化（配合超时） |
| SCHED-04 | P1 | `scheduler.cpp:4199,4140-4186,4246-4252` | worker_loop 对 `Task::run()` 无异常保护：`run()` 未标 noexcept，内部失败分支的 `GO2CPP_LOG_ERROR`（1010-1011）与调试事件可能分配/抛；异常穿出线程函数触发 `std::terminate`，且 running_workers/running_machines/MState/PState 记账不会复位 | 809 `void Task::run()` 无 noexcept；4199 `task->run();` 外层无 try/catch；4140-4186 已把 running_workers/running_machines 置为 running；4246-4252 的还原在其后 | `try { task->run(); } catch (...) { ... }`，并把 4140-4252 的记账收进 RAII；或把 `Task::run` 声明 noexcept 并在内部隔离日志/事件异常 |
| SCHED-05 | P1 | `scheduler.cpp:2185-2197,848-857,936-949` | `shutdown_for` 有界超时返回前销毁 `deferred_destruction` 中任务的 callable，但这些 G 仍在 registry 且非终态；之后 worker 取到它时 `m_function` 为空，直接走终态分支，用户代码被静默跳过并报告为 Completed | 2190-2194 `for (auto& task : deferred_destruction) { auto abandoned_function = task->release_callable(); }`；Fiber 入口 848-854 `auto body = std::move(m_function); if (body) { body(); }`；`run()` 936-949 对空 body 置 kDead | 超时/放弃路径应把任务标为 `kCancelled`（而非让其自然 Completed），或禁止在未终态任务上释放 callable；至少记全局计数并可观测 |
| SCHED-06 | P1 | `scheduler.cpp:3820-3822,3105-3147` | 两处「补发布」路径失败后没有兜底：`recover_unclaimed` 忽略 `enqueue()` 返回值；`wake_io` 把状态改成 Runnable 后三条入队路径全失败即 return false，G 停在 Runnable 且无队列节点 | `if (need_republish) { (void)enqueue(task); }`（3820-3822）；`wake_io` 3145-3146 返回 false；对比 `wake_or_cancel` 3321-3343 有 `cancel_if_runnable_unqueued()` 与 emergency 队列 | 抽出 `publish_or_cancel` 公共 helper：enqueue 失败 → `cancel_if_runnable_unqueued()` → 仍失败走 emergency 队列（不分配、不抛） |
| SCHED-07 | P2 | `scheduler.cpp:2575-2590,2697,4501-4512` | 未安装 io idle poller 时，生产者不持 `m_impl->mutex` 增加 `runnable` 并 `notify_worker()`；worker 在 `condition.wait_for(lock, timeout, predicate)` 检查谓词后、进入睡眠前可能错过通知，依赖 10ms/250ms 超时兜底，造成延迟尖峰 | 生产者 2575 `runnable.fetch_add` 在 incoming 锁内、未持 admission；2697 `notify_worker()`；worker 4501 `condition.wait_for(lock, idle_timeout, ...)` 谓词读 runnable（4504） | 生产者持 `m_impl->mutex` 更新 runnable 后再 notify（或统一 eventfd）；至少在注释中声明延迟上界 |
| SCHED-08 | P2 | `scheduler.cpp:3011-3014,3112-3115,3303-3307` | 三个唤醒入口对 `WakeAction::kPending` 的返回值不一致：`wake_registered` 返回 false、`wake_io` 返回 true、`wake_or_cancel` 返回 false；调用方无法区分「已记录 permit」与「失败」 | `wake_registered` 3007-3010 `if (action == kPending) return false;`；`wake_io` 3112-3115 同条件 return true；`wake_or_cancel` 3303-3307 return false；sync.cpp:173-183 只能靠 `state()==kRunnable && !queued()` 二次猜测 | 统一契约（建议 kPending 一律返回 true 并在头文件注明），或改为三值枚举返回 |
| SCHED-09 | P2 | `scheduler.cpp:2680-2696,4169-4181,1441,1395-1398,1349` | `enqueue` 与每个 G 执行前都可能调用 `maybe_grow()`，而它内部会 `reap_dead_workers()`（`thread.join()`）与 `std::thread` 创建，把阻塞/分配重操作放进提交与执行热路径 | enqueue 2681-2695 四次原子读后可能 `maybe_grow(this)`；worker_loop 4170-4181 growth_period 最小 8；`maybe_grow` 1441 `reap_dead_workers()`（1395-1398 join）与 1497 `spawn_worker_locked`（1349 创建线程） | 热路径只置「维护待办」标志，由 sysmon 或独立节拍线程执行 reap/spawn |
| SCHED-10 | P2 | `scheduler.cpp:492-540` | `wake_for_scheduler` 是 `wake_for_wait` 的逐行复制，且两处内存序不一致（496 relaxed vs 527 release；498 relaxed vs 529 acquire），后续极易分叉 | 492-512 与 518-540 除函数名外几乎相同；`wake_for_io` 514-516 已采用转调写法 | `wake_for_scheduler` 直接 `return wake_for_wait();`，删除重复实现 |
| SCHED-11 | P2 | `scheduler.cpp:483-490,627-668,409-416` | 多个状态转换入口在 src 内无任何调用点（`promote_new`/`mark_waiting`/`mark_yielded`/`mark_runnable`/`request_wake`/`consume_wake`），状态机存在不可达分支/死代码，测试无法覆盖 | 全仓 grep 仅定义处命中；实际路径走 `park_for_scheduler`/`wake_for_wait`/`mark_yielded_fast` | 删除，或纳入真实路径并加单测；保留时在头文件标注 deprecated 与理由 |
| SCHED-12 | P2 | `scheduler.cpp:2023-2026,2185-2197,2290-2293` | `shutdown_for` 有界超时后调度器不可重启：`start()` 因 `started` 仍为 true 直接返回，但 `is_running()` 为 false；draining 只在 2200 或 2140 清除 | `if (started || stopping) return;`（2023-2026）；超时路径只释放 callable、不回滚 draining（2118 置 true）；`is_running` 要求 `!draining`（2290-2293） | `start()` 显式拒绝「draining 未完成」并返回 false，或提供 resume_after_shutdown 语义并文档化 |
| SCHED-13 | P3 | `scheduler.cpp:4581,4141-4159,650-659` | worker 退出把 `t_processor_id` 置 0，而 0 是合法 P id，非 worker 线程 `current_processor_id()` 返回 0 而非无效哨兵；另 `mark_yielded_fast` 无锁清 `m_wake_pending` 与 `wake_for_wait` 持锁设置存在窗口（G 已 Runnable 并会重新入队，影响限于误判） | 4581 `t_processor_id = 0;`；`current_processor_id()` 返回该 TLS；650-657 CAS 后 `store(false)`；`wake_for_wait` 524-539 持锁设置 pending | 退出置 `kInvalidPId` 哨兵；`mark_yielded_fast` 清 pending 前重读 state，或让 wake 对 Runnable 返回 kPending |

## 2. 重点问题说明

### SCHED-01 / SCHED-02（P0）同一根因的两条死锁路径
`worker_loop` 的 B1 自愈逻辑（注释 3797-3823）在「队列节点已取出、claim 失败」时调用 `recover_unclaimed()`。该函数在 `state==kRunnable && !executing && queued==false` 时调用 `enqueue(task)` 补发布。两个调用点都持锁：

- 慢路径持 admission mutex（3981）→ `enqueue` worker 分支 2621 再次锁同一把非递归 `std::mutex` → 同线程死锁/UB。
- incoming 中路径持 stripe lock（3877）→ `enqueue` 锁 admission；而 shutdown 的 `begin_shutdown_locked` 在持 admission 时锁同一 stripe（1781）→ 反向锁序。

触发窗口是「另一 worker 已 claim 并运行到 park/yield，state 回到 Runnable 但尚未 requeue」的瞬间；窄但真实，且正是 B1 注释描述的高并发场景。建议把补发布做成延迟动作，`recover_unclaimed` 只返回判定结果。

### SCHED-03（P1）shutdown 无界挂死
`park_wait_for_cleanup` 的注释说「刻意不因取消/draining 拒绝 park，因为等待目标是释放用户 mutex」。但 shutdown 的 registry 扫描只执行一次（2119）；如果扫描发生在 park 提交 `kWaiting` 之前，这个 G 既不在扫描列表（或已扫描完）也没有唤醒源，`all_tasks_terminal_locked()` 永不满足，无超时的 `shutdown()` 永久阻塞。需要二次检查或循环重扫。

### SCHED-05（P1）超时放弃导致用户代码丢失
`shutdown_for` 超时路径对 `deferred_destruction` 中的任务调用 `release_callable()`，但任务仍在 registry、状态未终态。之后 worker 或后续 shutdown 取到它，Fiber 入口拿到空 callable 直接返回，`Task::run` 把状态置 `kDead`，从外部看是「正常完成」。这会把「未执行」伪装成「已完成」，比单纯超时更危险。

## 3. 亮点

- Task 三个位（`m_queued`/`m_execution_claim`/`m_run_claim`）在同一个 `m_transition_mutex` 内翻转；`run()` 入口对 `m_run_claim` CAS 失败的处理（813-828）明确引用了历史挂死事故，修复思路正确。
- `park_for_scheduler` 把「契约检查 → 状态转换 → 消费 wake 令牌」写成明确五步，状态与令牌在同一临界区完成，避免「先通知后挂起」丢唤醒。
- `shutdown_for` 区分 worker/外部调用者（worker 不 join 其他 M）；join 时先持锁摘出 thread 再解锁 join，避免 worker epilogue 需要同一把锁的死锁。
- `emit_event` 快照观察者、锁外回调并隔离异常；锁层次在文件头明确写出（1032-1040）。
- `BlockingRegion` 在跨 G 边界残留时 `std::terminate()` fail-fast（4243-4245），避免把不一致的 blocking 计数传给后续 G。

## 4. 维度结论

| 维度 | 结论 |
|---|---|
| 正确性 | park/wake 令牌与状态转换基本自洽；B1 自愈路径存在 P0 死锁、P1 补发布失败吞没；cleanup park 的二次检查缺失可导致 shutdown 挂死 |
| 并发 | 锁层次文档清晰，但 `recover_unclaimed` 打破「admission→incoming→P」单向顺序；CV notify 在未装 io poller 时依赖超时兜底；三个唤醒入口 kPending 语义不一致 |
| 资源与错误 | shutdown 的协作取消/超时语义完整；但超时放弃会释放 callable 并伪装完成；`maybe_grow` 的 join/spawn 在热路径 |
| 风格与可维护性 | 注释密度极高、历史事故有记录，是最大亮点；但存在逐行复制的 `wake_for_scheduler` 与 6 个无调用点的状态转换入口 |

## 5. 未覆盖与限制

- 未运行压测/TSAN；P0 死锁按代码路径与锁序推断，触发窗口窄但路径确定，建议用「高频 spawn + cancel + shutdown」压测复现。
- sysmon 的 detach/替代 M 计数、`maybe_grow` 线程创建失败路径、`schedule_*` 定时器线程与 shutdown 的竞态未逐行展开。
- 子审查曾抽查锁顺序并认为「未见反向嵌套」，但未覆盖 `recover_unclaimed` 的锁内 enqueue；本报告的 SCHED-01/02 是对该路径的补充结论。
