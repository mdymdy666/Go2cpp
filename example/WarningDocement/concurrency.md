# concurrency 使用限制

> 依据：`example/concurrency/` 示例 + 实际头/实现；每条标题末尾给出 file:line。

## 1. `go()` 回调必须可复制 —— `example/concurrency/future_full.cpp:17-19`
`Task::Function` 是 `std::function`，move-only 对象（如 Promise）须先搬进 `shared_ptr`。
```cpp
auto p = std::make_shared<Promise<int>>(std::move(pair.first));
go(scheduler, [p] { p->SetValue(1); });
```
**后果**：直接捕获 `pair.first` 编译失败（拷贝构造已删除）。
## 2. Fiber 栈有硬上限 —— `include/go2cpp/concurrency/fiber.hpp:284-286`、`include/go2cpp/concurrency/scheduler/scheduler.hpp:227-235`
直接构造默认 128 KiB 初始提交，Scheduler 默认 2048 B（页对齐 4 KiB）；预留 8 MiB 可增长，但仅限 Linux/x86_64 + 原生上下文 + `GO2CPP_GROWABLE_STACK!=0`。
```cpp
Fiber fiber(fn, Fiber::DefaultStackSize());   // 关增长或非 Linux 即固定栈
```
**后果**：退化路径不装 SIGSEGV handler，爆栈 = 进程被内核杀掉（`src/concurrency/fiber.cpp:32`）。
## 3. `SpinMutex` 临界区禁止 park —— `include/go2cpp/concurrency/spin_mutex.hpp:27,98`
只允许纯计算、必须极短；`Lock()` 成功后进入 no-park 区域（`include/go2cpp/concurrency/scheduler/scheduler.hpp:423-432`）。
```cpp
m.Lock();    // 此后禁止 channel / IO / 条件变量等协作点
m.Unlock();
```
**后果**：Debug 构建 abort；Release 构建计数降级，等于自死锁风险。
## 4. `sync::Mutex::Lock()` 可能失败 —— `example/concurrency/sync_demo.cpp:45-51`、`example/concurrency/mixed_runtime_demo.cpp:272-274`
返回 bool，取消/超时/Scheduler 不可用时为 false，此时**不能** Unlock；Unlock 本身不要求同线程。
```cpp
if (!mutex.LockFor(2s)) { return; }   // 失败路径必须直接返回
mutex.Unlock();
```
**后果**：未持锁就 Unlock → 破坏等待队列或数据竞争。
## 5. 取消回调禁止 park/阻塞 —— `include/go2cpp/concurrency/context.hpp:25-26`、`src/concurrency/context.cpp:228-233`
回调在通知线程上同步执行（锁外、每组恰好一次），只能做非阻塞唤醒。
```cpp
signal.AddCallback([] { waiter.TryCancelAndWake(gen); });   // 不做 IO、不等锁
```
**后果**：取消方被挂起，整棵子树取消传播卡住；捕获裸指针还会 UAF。
## 6. Timer 句柄析构 = 取消 —— `example/concurrency/timer_full.cpp:20-22`、`include/go2cpp/concurrency/core/timer.hpp:311-315`
`after()` 返回的句柄必须存住；临时句柄析构会取消尚未领取的任务，已 Running 的回调无法被打断。
```cpp
auto timer = after(Ms(20), fn);   // 不要写成 (void)after(Ms(20), fn)
```
**后果**：定时器永不触发；TimerService 只有一个 worker，慢回调串行拖慢全部任务（`include/go2cpp/concurrency/core/timer.hpp:363-367`）。
## 7. 线程策略是 TLS —— `example/concurrency/thread_policy_full.cpp:16,51-53`
守卫必须在同一 OS 线程内构造/析构；`ScopedThreadParticipation` 必须传入 Scheduler 身份。
```cpp
ScopedThreadParticipation scope(scheduler, ThreadParticipationMode::kGmpEligible);
```
**后果**：跨线程/跨 Fiber 使用导致恢复错乱；漏传 scheduler 时 `participates_in_gmp()` 为 false。
## 8. `BlockingRegion` 不可移动 —— `include/go2cpp/concurrency/scheduler/scheduler.hpp:600-603`
区域绑定进入它的 M，移动会让析构发生在别的 Fiber/线程（`example/concurrency/mixed_runtime_demo.cpp:222-235`）。
```cpp
go2cpp::BlockingRegion blocking;   // 就地构造，不 move、不跨作用域
```
**后果**：原 M 永久保持 Blocking，队列中的 G 无人执行。
## 9. `FiberLocal` 属于 G 不属于 M —— `example/concurrency/fiber_local_full.cpp:19-21`
值随 G 迁移、在 Fiber trampoline 结束时销毁；Fiber 外使用走线程局部后备，线程退出即销毁。
```cpp
FiberLocal<int> v;
int& x = v.GetOrCreate(10);   // 同一 G 内始终是同一对象
```
**后果**：以为可跨 Fiber 共享 → 各拿各的；在线程里当全局缓存 → 线程退出后失效。
## 10. 关闭前必须收敛任务 —— `include/go2cpp/concurrency/scheduler/scheduler.hpp:623-627`
`shutdown_for(timeout)` 超时不会释放仍在运行的 Fiber 栈，可稍后再调；worker 内调用返回 false。
```cpp
task->wait();
scheduler.shutdown();   // 默认调度器还要 shutdown_default_scheduler()
```
**后果**：资源未回收；正确顺序见 `example/concurrency/beginner_demo.cpp:58-64`。
## 11. `go()` 是异步的 —— `example/concurrency/parking_condition_full.cpp:52-53`、`example/concurrency/scheduler_full.cpp:135-137`
返回即走，不保证已开始/已完成；`metrics()` 与 `wait()` 之间仍有窗口。
```cpp
auto t = go(scheduler, fn);
t->wait();   // 读业务标志前必须先 wait
```
**后果**：直接读标志必为旧值；把 metrics 当强一致会偶发少一次。
