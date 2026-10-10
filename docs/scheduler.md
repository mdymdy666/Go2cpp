# GMP 调度器契约

## 数据、所有权与状态

| 记录 | C++ 对象 | 所有权与不变量 |
|---|---|---|
| G | `scheduler::Task` | 非终态期间由 Scheduler 注册表强持有；最多一个 `m_execution_claim` 和一个 `m_queued` 队列 claim；首次接纳它的 Scheduler 永久拥有它 |
| M | 私有 `Machine` 与 `std::thread` | 一个 worker 线程和一个首选 P；在 Scheduler 互斥量下动态加入、退休、join/reap |
| P | 私有 `Processor` | 有界本地队列和活动 M 计数；只有声明 `BlockingRegion` 时才允许替代 M 与其共享 P |

G 的合法转换为：

```text
new -> runnable -> running -> runnable | waiting | dead | cancelled | failed
waiting -> runnable | cancelled
failed/dead/cancelled -> terminal
```

快照中的 M/P 转换为：

```text
M: idle -> running -> idle -> parked -> idle
                 \-> blocking [processor_detached=true] -> running
                 \-> stopping -> dead
    parked -> stopping -> dead
P: idle <-> running; detached M 时 attached 计数可暂时为零；shutdown 时 idle -> dead
```

入队、出队、runnable 计数、shutdown admission 和 `runnable -> running` claim 在
admission mutex 下串行；Task transition mutex 保护状态字段。任何用户回调/捕获对象
都不会在 Scheduler、P、队列或 completion 锁内析构。

队列不变量如下：

- runnable G 恰好拥有一个队列/救援节点，或正处在 admission handoff，不能同时出现在两个队列；
- running G 拥有 `m_execution_claim=true` 且不在任何队列；出队会在同一转换中清除 `m_queued` 并取得执行 claim；
- `runnable_count` 统计已接纳的队列节点，pending wake 不计入 runnable；终态 G 没有队列和执行 claim。

## park、wake 与 Fiber continuation

只有当前 G 可以调用 `yield` 或 `park`。通知者看到 waiting 时把它改为 runnable 并
入队；若 G 仍在 running，则只设置一个 pending wake 位，由下一次 park 消费。shutdown
竞态中的 wake 要么进入队列/救援列表，要么转为取消。等待节点另有 wake gate，
`disarm()` 会等待正在执行的回调，之后才允许 Scheduler 指针失效。

每次 worker admission 都恢复同一 Fiber continuation；yield 后从切换点继续，绝不会
重新调用 callable。`Fiber::Suspend(Yield)` 映射为 runnable，其它原因映射为 waiting；
若切换前发生 wake/cancel，则恢复为 runnable，避免 shutdown 把 G 留在状态发布空窗。
原始 Fiber API 属于高级接口，调用方仍必须合作返回。嵌套 Fiber 的父链由共享
元数据记录保存，父对象结束后快照可以得到 `alive=false` 墓碑帧；但真正的
fcontext 恢复仍要求固定父 Fiber 对象存活。Ready 子 Fiber 尚未进入用户栈，析构时
可以安全跳过主体；Suspended 子 Fiber 若在错误父级之外析构会标记 Failed 并放弃上下文，避免
跳入错误父栈或释放仍可恢复的栈；该路径不能执行挂起栈上的 RAII，因此正常用法
仍是由固定父级完成子 Fiber。

## 动态 M 策略

`min_workers` 是保留底线。普通队列压力只把 M 增长到 P 有界上限；启用
`allow_worker_oversubscription` 时，每个活动 `BlockingRegion` 可申请一个替代 M，
但不超过 `max_workers`。关闭该选项即严格 P 上限。空闲 M 等待
`idle_worker_timeout` 后在 admission mutex 下预留退休名额，并各减少一次全局和
P-local 计数；死亡记录在锁外 join 后从快照删除。Task class 只影响有限扫描顺序，
窃取始终可用，不会因亲和性导致饥饿。

### sysmon 长系统调用交接

当 `enable_sysmon=true` 时，Scheduler 启动一个独立监控线程，以
`sysmon_interval` 检查已发布的 `MState::Blocking`。进入 `BlockingRegion` 或 Hook
的 native fallback 时记录 `blocking_since` 和当前 G；持续时间达到
`long_syscall_threshold` 后，sysmon 将 `MachineSnapshot::processor_detached` 置为真，
从所属 P 的 attached M 计数中扣除一次，并为该长调用申请一个替代 M。替代槽按
`min_workers + detached_count` 计算，仍受 `max_workers` 和严格模式限制，不会因为
整机 P 数量很大而无条件创建 P 个线程。原 M 仍在自己的 C++ 调用栈中运行，返回时
`BlockingRegion` 原子地清除标记并把 M 重新计入 P；在此之前替代 M 不会被 idle timeout
回收。

这是安全的“逻辑解绑”，不是 Go runtime 的精确 `_Psyscall` 交接，也不会从另一个
线程强行切断/迁移任意 C++ 栈。未 Hook、未包在 `BlockingRegion` 中的阻塞调用不可被
sysmon 发现；raw syscall、第三方阻塞库和达到 `max_workers` 的场景只能保持现有
线程语义。

`BlockingRegion` 不可移动，并记录进入它的 M；不能跨 Fiber yield/park/迁移。worker
在 G 任务边界发现仍活动的 region 会 fail-fast，避免把旧 M 的 blocking 记账静默
遗留给后续 G。Hook 的 socket/sleep 和未跟踪 FD fallback 会自动发布 blocking 记账；
未 Hook 的调用必须显式包在 `BlockingRegion` 中。`ScopedThreadParticipation` 只是每线程
策略元数据，不会附加 M 或运行队列。

## shutdown 与 join

shutdown 是单向操作：关闭 admission，取消未启动队列 G，给已启动 G 设置合作取消，
唤醒 parked G，等待注册表全部终态。worker 自身发起 shutdown 时只发布 drain 请求，
最后一个 G 终态后 worker 标记 stopping 并退出，不会 join 自己；拥有者线程执行最终
join。Scheduler/IOManager 所有者必须长于 worker 和外部成员调用，析构不能与成员调用
并发。

managed G 调用 `Task::wait`/`Join` 时使用 `ParkingCondition`，所以 P=1 也能运行被
等待的子 G；普通线程使用 native condition fallback。自 join 或已取消的 managed
waiter 返回 false。销毁挂起 Fiber 通常是“请求取消 + 自然完成”，永不返回的 body 会让
join/shutdown 等待，这保证 C++ RAII 不被跳过；错误父级路径会返回 Failed，
 超时后不得释放仍运行的栈。

## 可观测性与验证

`processors()`、`machines()` 只返回快照，不授予队列所有权。测试覆盖 P=1/多 P、
重复 enqueue/wake、跨 Scheduler 拒绝、park 取消、Fiber 取消、队列 capture 回收、
动态增长/收缩/再增长、严格 P 上限、BlockingRegion 替代 M、task class 计数、worker
shutdown、析构重入和带 watchdog 的压力循环。精确命令见 `docs/testing.md`。

### 观察者插件边界

`SchedulerObserver` 是调度器与日志、指标、分布式追踪之间的职责边界。
在 `SchedulerConfig::observer` 中注入实现，或在启动后调用
`Scheduler::set_observer()` 热替换。观察者接收 `SchedulerEvent` 快照，事件包括
调度器启动/停止、M 启停和 G 开始运行、挂起、完成、失败；快照中的时间使用
`steady_clock`，适合计算区间但不能当作墙上时间。

事件只读且不参与队列状态转换。Scheduler 先复制观察者指针、释放观察者锁，
再执行 `OnEvent()`，因此插件不得依赖事件回调改变当前 G 的执行权，也不会在
Scheduler/P/队列锁内执行用户代码。回调必须短小、无阻塞；第三方实现即使抛出
异常，运行时也会在 `emit_event()` 边界隔离该异常并继续 worker 生命周期。

该接口用于“观测”和策略适配，不伪装成可异步抢占的调度器替换点。若需要新的
队列、Fiber 栈或 IO Poller 后端，应先实现各自的状态协议和独立后端，再把结果以
事件或快照接入 Scheduler；不要在 `OnEvent()` 中直接操作内部队列。默认空观察者
路径没有额外事件分配，性能敏感部署可以保持 `observer == nullptr`。

## 2026-09-23 监控活性与嵌套 IO 复核

sysmon 的节拍等待使用独立的 sysmon_wait_condition，不再先获取 Scheduler 队列主锁；每一轮只在扫描阶段对主锁执行一次 try_to_lock。sysmon_pass_count() 在扫描尝试开始时递增，因此它是 monitor 活性心跳，不代表本轮一定完成了 detach 扫描。sysmon_running() 与该计数应一起用于部署自检。实现保持一个 monitor 线程，不会在高负载时复制多个 monitor；真正的扩容由 maybe_grow() 根据 runnable backlog、声明的阻塞 M 数量和 max_workers 有界执行。达到上限、线程资源耗尽或未声明的阻塞调用都不会被伪装成已扩容。

Hook IO 的 readiness、deadline、cancel 和 close 唤醒均通过一次性等待节点和 park_io() 进入 Fiber；嵌套 Fiber 的 IO 挂起会沿固定父链传播，唤醒后按原 fcontext 继续，测试覆盖三层父链、连续超时/ready 两次等待和父级返回顺序。IOManager 现提供 `WaitAny/WaitMany`：多个 (fd, event) 会登记到同一 epoll poller，任一节点完成后唤醒当前 Fiber，并幂等清理其余节点；WaitMany 返回同一轮已就绪索引集合。普通线程调用返回 EPERM，应使用原生 poll/select。WaitAny/WaitMany 同一集合不得重复提交相同 fd/方向，返回 EINVAL；无效 fd 会取消已登记节点并返回整体错误。Hook 对 libc poll/select 的 ABI 仍保持原样，不自动改写信号掩码、EINTR 或 revents。

monitor 不会从异线程终止、迁移或恢复任意 C++ Fiber 栈，也不会向 write 等系统调用注入 C++ 异常。Hook 系统调用错误保持 libc 的返回值和 errno；调用者应显式转换为 error 或使用普通 C++ 异常。关闭对端后的错误由调用者处理。
