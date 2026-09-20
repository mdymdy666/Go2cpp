# GMP 调度器契约

## 数据、所有权与状态

| 记录 | C++ 对象 | 所有权与不变量 |
|---|---|---|
| G | `scheduler::Task` | 非终态期间由 Scheduler 注册表强持有；最多一个 `m_execution_claim` 和一个 `m_queued` 队列 claim；首次接纳它的 Scheduler 永久拥有它 |
| M | 私有 `Machine` 与 `std::thread` | 一个 worker 线程和一个首选 P；在 Scheduler 互斥量下动态加入、退休、join/reap |
| P | 私有 `Processor` | 有界本地队列和活动 M 计数；只有声明 `BlockingRegion` 时才允许替代 M 与其共享 P |

G 的合法转换为：

```text
new -> runnable -> running -> runnable | waiting | dead | cancelled
waiting -> runnable | cancelled
dead/cancelled -> terminal
```

快照中的 M/P 转换为：

```text
M: idle -> running -> idle -> parked -> idle
                 \-> stopping -> dead
    parked -> stopping -> dead
P: idle <-> running; shutdown 时 idle -> dead
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
原始 Fiber API 属于高级接口，调用方仍必须合作返回。

## 动态 M 策略

`min_workers` 是保留底线。普通队列压力只把 M 增长到 P 有界上限；启用
`allow_worker_oversubscription` 时，每个活动 `BlockingRegion` 可申请一个替代 M，
但不超过 `max_workers`。关闭该选项即严格 P 上限。空闲 M 等待
`idle_worker_timeout` 后在 admission mutex 下预留退休名额，并各减少一次全局和
P-local 计数；死亡记录在锁外 join 后从快照删除。Task class 只影响有限扫描顺序，
窃取始终可用，不会因亲和性导致饥饿。

`BlockingRegion` 不可移动，并记录进入它的 M；不能跨 Fiber yield/park/迁移。它不会
探测任意 native syscall，也不会抢占 C++ continuation。Hook 的 socket/sleep fallback
会自动发布 blocking 记账；未 Hook 的调用必须显式包在 `BlockingRegion` 中。
`ScopedThreadParticipation` 只是每线程策略元数据，不会附加 M 或运行队列。

## shutdown 与 join

shutdown 是单向操作：关闭 admission，取消未启动队列 G，给已启动 G 设置合作取消，
唤醒 parked G，等待注册表全部终态。worker 自身发起 shutdown 时只发布 drain 请求，
最后一个 G 终态后 worker 标记 stopping 并退出，不会 join 自己；拥有者线程执行最终
join。Scheduler/IOManager 所有者必须长于 worker 和外部成员调用，析构不能与成员调用
并发。

managed G 调用 `Task::wait`/`Join` 时使用 `ParkingCondition`，所以 P=1 也能运行被
等待的子 G；普通线程使用 native condition fallback。自 join 或已取消的 managed
waiter 返回 false。销毁挂起 Fiber 是“请求取消 + 自然完成”，永不返回的 body 会让
join/shutdown 等待，这保证 C++ RAII 不被跳过。

## 可观测性与验证

`processors()`、`machines()` 只返回快照，不授予队列所有权。测试覆盖 P=1/多 P、
重复 enqueue/wake、跨 Scheduler 拒绝、park 取消、Fiber 取消、队列 capture 回收、
动态增长/收缩/再增长、严格 P 上限、BlockingRegion 替代 M、task class 计数、worker
shutdown、析构重入和带 watchdog 的压力循环。精确命令见 `docs/testing.md`。
