# 定时任务使用指南

> **已合并至 [docs/modules/timer.md](modules/timer.md)（2026-10-26）**。
> 本文保留作历史快照：其中的路径、行号、计数与性能数字都可能已过期，
> 现行接口、设计与测试以模块文档为准。


定时任务模块由 core::TimerService、core::Timer、core::TimerExecutor 和 Scheduler 的适配接口组成。计时使用 std::chrono::steady_clock，不受系统墙上时间回拨影响。TimerService 只负责登记截止时间、状态和取消，不继承调度器，也不直接恢复 Fiber 栈。

## 一次性任务

普通线程可以直接构造服务。服务创建一个计时线程，回调在线程执行：

    core::TimerService service;
    auto timer = service.ScheduleAfter(100ms, [] {
        std::cout << "到期" << std::endl;
    });
    // timer.Cancel() 只阻止尚未进入 Running 的回调。

Timer 是不可复制的移动句柄，析构等价于取消。必须在回调完成前保留句柄；临时句柄会立即取消任务。Active、StateValue 和 RunCount 可用于观测，Reset 只能重排仍处于 Pending 的任务。截止时间已到或回调已经开始时，重排会返回 false。

## 周期与动态间隔

ScheduleEvery 支持两种周期策略：

- kFixedDelay：本次回调返回后再等待 period，适合重连、轮询等慢任务；
- kFixedRate：沿计划时间线推进，skip_missed=true 时合并错过的周期，避免回调变慢后忙循环追赶。

示例：

    core::TimerOptions options;
    options.repeat = go2cpp::core::TimerRepeatMode::kFixedRate;
    options.max_runs = 5;
    auto timer = service.ScheduleEvery(1s, poll_resource, options);

需要按照返回结果改变下一次等待时间时使用 ScheduleRepeat。回调可以返回 TimerDecision::ContinueAfter、ContinueAt 或 Stop。回调抛出的异常会让当前任务进入 kFailed，周期不会自动重试；通过构造函数或 SetExceptionHandler 注册错误观察器，需要重试时由 TimerDecision 显式返回下一次时间。

## Fiber 和 Scheduler

Scheduler::schedule_after、schedule_at、schedule_every 会把到期函数提交为一个新的 Fiber。计时线程不恢复其他线程的 Fiber 栈：

    go2cpp::Scheduler scheduler(2);
    scheduler.start();
    auto timer = scheduler.schedule_after(20ms, [] {
        // 这里运行在 Scheduler 的 G/M/P 链路中，可以使用 Channel、Context 等 Fiber 原语。
    });
    timer.Cancel();
    scheduler.shutdown();

新手可以调用 go2cpp::after(delay, callback) 或 go2cpp::every(period, callback)，它们使用默认 Scheduler。需要明确生命周期的库代码应保留 Scheduler 对象并在退出前调用 shutdown。Scheduler 销毁前会使定时器执行适配器失效，后续提交会失败，不会访问悬空对象。
TimerExecutor::Submit 返回 true 只表示执行器接收了回调，不表示 Fiber 已经完成；周期任务的 fixed-delay 间隔从提交返回后计算。需要等待业务完成时，应在 Fiber 内部用 Channel、Future 或 Context 显式同步。

## 资源生命周期与提前完成

资源可能提前消失时使用 ScheduleWeakAfter 或 ScheduleWeakEvery：

    auto resource = std::make_shared<Resource>();
    auto timer = service.ScheduleWeakEvery(
        1s, std::weak_ptr<void>(resource),
        [](std::shared_ptr<void> locked) { use(*locked); });
    resource.reset();  // 下一次领取时任务返回 Stop，不会访问悬空对象。

网络 readiness、Context 取消和定时到期属于同一业务操作时，必须共享一个“只允许一个赢家”的状态。readiness 先成功就调用 Cancel 或 Reset；timer 先成功就标记超时并让 IO 回调检查状态。不能依赖线程调度顺序来判断谁先完成，也不能让同一业务回调执行两次。

## 等待策略和并发规范

默认 kConditionVariable 在没有任务时休眠，不占用 CPU。kBusyWait 只在截止时间前的 spin_window 内自旋，适合极低延迟、且能接受占用一个 CPU 的场景。不要把忙等待作为所有周期任务的默认配置。

定时器锁只保护队列和状态。回调、日志、Context 取消、Channel 操作和执行器提交都在锁外完成。回调不得持有 sync::Mutex、HybridMutex 或其他混合锁后再调用可能 park 的 Fiber 原语；需要等待时先释放锁。多个线程可以同时注册、取消和重排，但业务对象本身仍需由调用方提供线程安全保护。

## 阻塞、关闭和测试

同一个 TimerService 的回调如果长期阻塞，会延迟该服务后续任务。应把重工作提交给 TimerExecutor，或者为不同业务创建独立服务。服务析构会等待正在运行的回调完成，不支持强行终止 C++ 调用栈；不能在该服务的计时回调中销毁服务，否则按契约触发 std::terminate。所有示例和测试都有有限超时，避免把这个边界误用为可恢复的异步取消。

完整示例见 example/timer_task_demo.cpp，覆盖一次性、取消、周期、动态重排、弱引用、异常和 Scheduler/Fiber。测试入口为 GO2CPP_TEST_FILTER=timer；性能基准和线程/coost 对照见 docs/timer_benchmark.md。
