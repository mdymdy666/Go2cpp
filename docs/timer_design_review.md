# 定时任务模块审计与解耦设计

本文记录对当前 `core::TimerService`、`Scheduler` 和 `IOManager` 的审计结果，作为定时任务扩展的设计约束。本文不把定时器线程伪装成 GMP 的 M，也不建议让 `Scheduler` 继承定时器管理器。

## 一、当前实现的边界

### `core::TimerService`

实现位于 `include/go2cpp/core/timer.hpp` 和 `src/timer.cpp`，当前行为如下：

1. 使用 `steady_clock` 和 `std::multimap<deadline, State>` 保存任务，并用
   `State*` 索引把取消和重排降为对数复杂度。
2. 每个服务实例拥有一个原生计时线程；它只领取到期项，用户回调在服务锁外运行。
3. 支持一次性、固定延迟、固定速率、动态 `TimerDecision`、`Reset`/`Refresh`、
   最大执行次数、忙等待窗口和弱引用周期任务。
4. `Cancel()` 幂等，只保证阻止尚未领取的任务；状态已经是 `Running` 时不能强行打断
   C++ 调用栈。服务停止时，正在返回的周期回调不会重新插回队列。
5. 回调异常会被捕获，当前任务进入 `kFailed`，并通过可替换的
   `TimerExceptionHandler` 观察；异常不会退出计时线程。
6. `ScheduleOn`/`ScheduleEveryOn` 通过 `TimerExecutor` 交给外部执行环境。
   Scheduler 使用的适配器只调用 `spawn`，不在计时线程恢复 Fiber 栈。
7. 回调如果长期阻塞，会阻塞同一个服务的后续领取；这时应把工作提交给执行器或另外的
   服务实例。析构会等待正在运行的回调结束，不能从该服务自己的回调中销毁服务，
   该边界会触发 `std::terminate`，以避免 worker 使用已销毁的 `Impl`。
8. `TimerService::Default()` 是进程级静态实例，服务没有热替换时钟的接口；需要独立
   生命周期、异常观察器或隔离阻塞回调时应显式构造 `TimerService`。

这些边界必须写入业务规范。尤其不能把 TimerService 线程称为 GMP 的 M，也不能在计时
线程直接恢复 Fiber 栈；应使用 Scheduler 的定时接口或自定义 TimerExecutor。

### `IOManager` 内部截止时间

`src/io.cpp` 的 `IOManager::State` 另外维护 `m_deadline_order` 和 `m_deadline_index`，用于 fd 等待的超时。轮询线程执行如下流程：

1. 在同一把状态锁下登记 fd 等待节点和 deadline。
2. 通过 `poll_timeout_ms()` 将最早 deadline 转换为 `epoll_wait` 的超时时间。
3. 先处理 epoll readiness，再调用 `expire_timers()`，因此同一轮中 readiness 优先于 timeout。
4. 新增更早的 deadline 时调用 `tickle()` 打断当前等待。

这套队列是 IO 等待状态机的一部分，节点还带有 fd generation、取消、关闭和一次性 outcome claim，不能直接替换成一个只会执行业务回调的 `TimerService`。可以复用“绝对截止时间索引”和“插入队首时唤醒轮询器”的思想，但 IO deadline 与业务定时任务应保留不同的完成协议。

### `Scheduler`

`Scheduler::schedule_at`、`schedule_after` 和 `schedule_every` 已通过 `TimerExecutorAdapter` 提供 Fiber 入口。计时线程只提交一个新的 G，Fiber 仍由 Scheduler 的 M/P 恢复，因此不会跨线程恢复同一 Fiber 栈。Scheduler 仍不继承 TimerManager，空闲 worker 也不直接读取业务 TimerQueue；业务 timer 由独立计时线程驱动，提交后通过调度器入队唤醒空闲 worker。IOManager 的 fd deadline 继续由自己的 epoll/poll 循环处理，两套完成协议不能混用。

周期任务的取消、异常、弱引用和 shutdown 由 TimerService 统一管理；Scheduler 销毁前会先使适配器失效，后续提交返回 false，避免访问已销毁的 Scheduler。尚未解决的边界是：用户回调长期阻塞时计时线程仍会被占用，TimerService 析构必须等待它结束，不能在该线程自身回调中销毁服务。
## 二、必须保持的状态与线程安全约束

定时器状态建议明确为：

```text
new -> armed -> dispatching -> fired
                 \-> cancelled
                 \-> failed
armed/dispatching -> cancelled (取消只能阻止尚未开始的用户回调)
```

每个句柄必须只有一个 `TimerId` 和一个 generation。取消、触发、重排、shutdown 都通过同一个原子状态或受保护状态完成一次 claim；不能用“先观察 active，再执行 callback”的双检查替代 claim。

定时器队列锁只保护 deadline 索引、句柄定位和状态转移。用户回调、执行器入队、弱引用 `lock()`、日志和异常处理必须在队列锁外进行。这样可以避免以下锁反转：

```text
TimerQueue -> Context/Channel/Scheduler
Scheduler  -> TimerQueue
```

回调不应在 TimerQueue 锁内调用 Scheduler、Channel 或 Context。若 callback 需要取消自身，先释放队列锁，再执行回调；句柄保留强状态引用直到 dispatch 完成。

## 三、推荐的解耦边界

### 1. `TimerQueue`：只负责时间和状态

新增内部接口（名称可按现有命名调整）：

```cpp
class ITimerClock {
public:
    virtual ~ITimerClock() = default;
    virtual TimePoint now() noexcept = 0;
};

class TimerQueue {
public:
    TimerHandle schedule_once(TimePoint deadline, TimerCallback callback);
    TimerHandle schedule_periodic(PeriodicOptions options,
                                  TimerCallback callback);
    bool cancel(TimerId id, TimerGeneration generation) noexcept;
    bool reset(TimerId id, TimerGeneration generation,
               TimePoint deadline) noexcept;
    std::optional<TimePoint> next_deadline() const noexcept;
    std::size_t take_expired(TimePoint now, std::vector<DispatchItem>& out);
};
```

`TimerQueue` 不创建线程、不知道 Fiber、不知道 epoll，也不直接执行 callback。实现初期可使用“最小堆 + `TimerId -> heap entry` 索引”把取消从当前的 O(n) 降到 O(log n)。若后续需要百万级短周期任务，再以接口外接 timing wheel；不能把 timing wheel 细节泄漏到 Scheduler API。

### 2. `ITimerExecutor`：只负责把任务交给执行环境

```cpp
class ITimerExecutor {
public:
    virtual ~ITimerExecutor() = default;
    virtual bool dispatch(TimerCallback callback,
                          TimerDispatchInfo info) = 0;
};
```

至少提供两种实现：

- `NativeTimerExecutor`：把回调投递到一个或多个普通线程。默认单线程可保持 deadline 顺序；多线程模式必须在文档中说明回调可能并发、完成顺序不保证。
- `SchedulerTimerExecutor`：只调用 `Scheduler::spawn()`/`wake_registered()` 发布 G，绝不在 TimerService 线程恢复 Fiber 栈。Fiber 中的阻塞操作继续由 channel、IOManager 或 Context 自己 park。

可选的 `InlineExecutor` 只允许测试使用，不能作为默认值，否则会在 IOManager poller 或 Scheduler idle 锁路径中执行用户回调。

### 3. `TimerManager`：组合 Queue、Clock 和 Executor

`TimerManager` 负责生命周期、线程泵和 shutdown，但不让 `Scheduler` 继承它：

```text
TimerManager
  ├── TimerQueue
  ├── ITimerClock
  ├── ITimerExecutor
  └── ITimerWaiter（条件变量、IO backend 或 Scheduler tickle）
```

`TimerManager` 对普通线程提供 `schedule_*`，对 Scheduler 提供一个轻量的
`SchedulerTimerBridge`。Scheduler 只持有 bridge 或可选 timer source，在 worker idle 前查询
`next_deadline()`，不拥有 TimerQueue 的实现细节；IOManager 也可以把自己的
`next_deadline()` 作为另一类 `DeadlineSource` 接入 poller。

## 四、周期、提前触发和弱引用语义

### 周期模式

周期任务必须显式选择以下一种模式：

- `fixed_delay`：本次 callback 返回后再加 interval。适合重连、轮询外部资源，不会因 callback 变慢而同时排队多个实例。
- `fixed_rate`：以理论时间线推进 deadline。若错过多个周期，默认 `coalesce` 为一次触发并把下一次 deadline 推到未来；`catch_up(n)` 只允许有界补偿，不能无限循环追赶。
- `reschedule`：callback 返回 `Stop`、`Again` 或一个新的绝对 deadline。适合资源可能消失或下次间隔由结果决定的任务。

周期 callback 的返回值应包含下一步动作，而不是要求 callback 直接修改内部 Timer 对象。callback 抛异常时，默认转为 `failed` 并停止该周期；如果业务需要重试，应显式返回下一次 deadline。

### 提前触发

“提前完成”不是把同一个 callback 执行两次。等待某资源的周期任务应采用两个句柄：

1. 外部事件完成时，先 claim 业务状态并取消/重排原定时器；
2. 事件回调与 timer 回调竞争同一个 generation，只有赢家执行后续链；
3. 失败或资源消失时由 callback 返回 `Stop` 或新的重试时间。

如果 deadline 与 readiness 在同一轮到达，IOManager 保持现有协议：readiness 先 claim，只有没有 readiness claim 的节点才返回 timeout。业务 TimerQueue 也必须使用一次性 claim，不能依赖执行顺序猜测赢家。

### 弱引用和强引用

- `schedule_condition(weak_ptr<void>, callback)` 在 dispatch 前执行 `weak.lock()`；对象已消失则标记 `cancelled`，不执行 callback。
- 必须保证资源存活时使用 `shared_ptr` 捕获；TimerService 不能替用户延长弱资源寿命。
- `TimerHandle` 的析构只取消尚未 dispatch 的回调；需要等待 callback 结束应另提供 `wait_finished()`，不能让 `Cancel()` 隐式阻塞。

## 五、等待策略与空闲模式

定时器等待器应支持配置而不是固定忙等：

```text
WaitPolicy::condition_variable  // 默认，空闲时完全休眠
WaitPolicy::spin_then_sleep     // 先短暂自旋，再进入条件变量
WaitPolicy::busy_wait           // 仅实时/基准测试，明确消耗一个 CPU
```

`spin_count`、`max_callbacks_per_tick`、`idle_wait_cap` 需要有上限校验。忙等待不能成为 Scheduler 默认配置，也不能因为有一个远期 timer 让所有 worker 持续自旋。

Scheduler/IOManager 空闲循环建议统一为：

1. 先处理本地队列、全局队列和到期 timer；
2. 计算 `min(next_timer_deadline, configured_idle_cap)`；
3. 没有任务且没有 deadline 时在 `ParkingCondition`/epoll 中无限休眠；
4. 插入更早 timer、发布新 G、取消或 shutdown 时 tickle；
5. poller 返回后先处理 readiness，再处理同一时刻到期 timer，保持 IO 的现有边界。

定时器回调执行批次必须有限制。单批次处理过多 callback 会饿死 IO readiness 和普通 G；剩余到期项留到下一轮，并主动 tickle。

## 六、shutdown 和异常策略

以下是后续可选增强，当前实现已经提供 request-stop 的等价内部路径，但尚未提供带超时的公开 join_for 接口；不要把建议接口当成现有 API 使用。

建议将停止拆成三步：

1. `request_stop()`：拒绝新注册，唤醒 waiter；
2. `drain(policy)`：选择取消未 dispatch、等待已 dispatch，或将 callback 投递到 executor；
3. `join_for(timeout)`：有界等待线程退出，超时返回 false，保留状态供后续继续收尾。

后续若要允许用户在 callback 中销毁自身服务，必须把队列状态和线程所有权拆开：worker 只持有状态的 `shared_ptr`，线程句柄由外层 owner 管理；owner 从 worker 回调中销毁时只 request stop 并 detach/转交 join，状态在 worker 退出后自动释放。当前实现尚未具备这套安全转交，因此明确以 `std::terminate()` 拒绝该用法，并用文档和测试保持边界可见。

callback 的异常必须通过 `TimerErrorObserver` 或计数器报告。默认行为是隔离异常、停止当前一次/周期任务并继续服务线程；不能静默吞掉后让用户误以为任务成功。

## 七、必须补充的测试和示例

测试必须同时覆盖普通线程和 managed Fiber：

1. 单次 timer：提前取消、截止时间触发、重复取消、同一 deadline FIFO。
2. 周期 timer：fixed-delay、fixed-rate、coalesce/catch-up 上限、动态 reschedule。
3. 资源条件：weak_ptr 对象提前销毁、shared_ptr 保活、callback 中释放最后一个 owner。
4. 竞态：多个线程同时 schedule/cancel/reset，cancel 与 dispatch 同时发生，shutdown 与注册同时发生。
5. 运行环境：P=1 的 Scheduler、多个 P、IOManager epoll readiness 与 timeout 同时到达、Timer callback 阻塞、callback 异常。
6. 等待策略：condition variable、spin_then_sleep、busy_wait 的 CPU 使用和延迟对比；busy_wait 只能显式开启。
7. 性能：大量短期 timer 的 schedule/cancel、周期重连场景、混合 native thread/Fiber 任务；报告 p50/p99 延迟、每秒触发数、CPU 和内存。
8. 故障注入：后端 wake 失败、线程创建失败、超大 deadline、时钟回拨（steady clock 不应受墙钟影响）、executor 拒绝任务。

示例至少包含：

- `TimerService` 在线程中执行一次性和周期任务；
- `Scheduler` 通过简单 API 将 timer 回调投递成 Fiber；
- `IOManager` 中 socket readiness 提前结束一项定时重试，并展示 timer/IO 的单次 claim；
- weak/shared 生命周期、取消、异常和 shutdown 超时的错误路径；
- 日志打印 `timer_id`、generation、执行后端、计划时间、实际时间和最终状态。

## 八、与 sylar2 的对应关系

`/UserData/CodexWorkSpace/sylar2/sylar/timer.h/.cc` 提供了 recurring、`reset`、`refresh`、
`addConditionTimer(weak_ptr)`、`getNextTimer`、`listExpiredCb` 和“插入队首时 tickle”等
重要思路；`IOManager::idle()` 将最早 timer 转换为 `epoll_wait` timeout，并在锁外
调度 callback。Go2Cpp 应吸收这些功能点，但保留更严格的 outcome claim、generation、
Fiber 不跨线程恢复和可插拔 executor 边界，不能直接复制 sylar 的裸 `TimerManager*`
生命周期或在计时器锁中运行用户代码。

