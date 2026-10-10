# 运行时逻辑链路审计

本文记录 Go2Cpp 当前实现可以被代码和测试共同支持的正确性范围。这里的“证明”是对状态转换、锁保护范围、线性化点和生命周期的可审计论证；它不能把 C++ 用户违反生命周期或协作约定的程序变成安全程序，也不能替代原生 Linux 上的竞态检测。

## 结论和适用前提

在以下使用约定下，本轮没有发现新的可复现任务丢失、重复执行、重复唤醒、等待永久悬挂或已知的确定性内存泄漏：

1. `Scheduler`/`IOManager` 的生命周期覆盖所有已提交的 `Task`、挂起的 `Fiber` 和 Hook 调用。
2. 可能阻塞的未封装原生调用显式使用 `BlockingRegion`；Hook 只覆盖项目实现的 Linux socket、poll/select/epoll 和 sleep 路径。
3. `BlockingRegion`、原生锁和持有外部锁的 C++ 对象不跨 Fiber `yield`/`park`。
4. 业务代码通过 `go2cpp::go`、`Scheduler::spawn` 或 `IOManager::go` 创建受调度的 G；手动 `Fiber` 只在固定父级仍可恢复时嵌套。
5. Channel 对象不会与正在执行的成员函数并发析构，元素类型满足公开的 nothrow move/destructor 约束。

这些是 API 契约，不是隐藏的运行时假设。违反契约时，代码会返回失败、取消或进入 `Failed` 终态；任意 C++ 栈的强制异步回收仍然不安全。

## G/M/P 调度链路

### G（`Task`）状态和唯一执行权

`src/scheduler.cpp` 中 `Task::m_transition_mutex` 串行化状态、`m_queued`、`m_execution_claim` 和取消请求。允许的主路径是：

```text
New --prepare_enqueue--> Runnable --try_mark_running--> Running
Running --park_for_scheduler--> Waiting
Waiting --wake_for_wait/wake_for_scheduler--> Runnable
Running --Fiber 返回--> Dead / Failed / Cancelled
```

`Runnable -> Running` 同时清除队列预约位并取得 `execution_claim`；因此 worker 从任何 P、全局队列或 emergency 队列取出节点后，只有取得 claim 的 M 可以调用 `Task::run()`。`run_claim` 是第二道防线，防止后端误调用造成并发 `resume`。

等待唤醒只发布状态和 permit：如果 G 仍持有 execution claim，唤醒方设置 `m_deferred_enqueue`/`m_wake_pending`，不会直接进入 Fiber 栈。G 返回 worker 后才重新入队。这一步保证一个 G 不会同时被两个 M 恢复。

### 队列和 runnable 计数不变量

- 每个已发布节点先取得 `m_queued=true`，并在节点发布前 `runnable++`。
- worker 弹出节点后先 `runnable--`，再执行 `try_mark_running()`；失败节点只清除预约位，不执行回调。
- 本地 P 队列、incoming 条带、全局队列和 emergency 链都遵守同一规则。
- 普通 `push_back` 发生异常时，已启动 G 使用 Task 内嵌的 emergency 链；未启动 G 走取消终态。这样 OOM 不会静默丢失一个已经启动的 Fiber。
- registry 强持有所有非终态 Task，终态回收在调度器锁外销毁用户闭包，因此挂起栈不会在没有强引用时被释放，用户析构函数也不会重入调度器锁。

### M/P 和 sysmon

`active_machines`、`running_machines`、`processor_detached` 的计数修改在 `Impl::mutex` 或对应 P 队列锁的规定范围内完成。sysmon 只观察显式 `BlockingRegion`：达到阈值时把 M 从 P 的 attached 计数中摘除，再由 `maybe_grow()` 申请有界替代 M；原 M 返回时只回接一次。sysmon 不从其他线程跳转或终止正在运行的 C++ 栈。

worker 缩容先在调度器锁内预约 `active_workers--`，然后离开循环，避免多个空闲 M 同时把数量减到配置下限以下。`shutdown` 的顺序是：停止接收 → draining 扫描队列和 registry → 请求取消并唤醒等待 G → 等待全部 Task 终态 → 停止 sysmon → 等待 worker 退出 → join。worker 自身发起 shutdown 时不执行 join，由外部 owner 完成最终回收。

## Fiber 和嵌套恢复

`src/fiber.cpp` 的每个 Fiber 有独立 `FiberRecord`。记录保存逻辑父级、深度、最近线程和 G/M/P 绑定；真正的 fcontext 仍要求父 `Fiber` 对象存活。普通 `Fiber::Suspend()` 只返回直接父级；`SuspendForScheduler()` 通过 `m_scheduler_propagate` 沿 `child -> parent -> Task Fiber -> main_fiber` 逐级传播，因此子 Fiber 不会无条件跳回 main_fiber。

每次 `resume` 取得 `m_resume_claim`。首次进入固定直接父级，迁移到另一个 M 只更新执行元数据，不改变父链。Fiber 完成或失败后，`Task::run()` 先清空 fcontext 和栈，再把对象放入当前 M 的 FiberBin；仍可能被子 Fiber引用的父记录不会被复用。错误父级析构会将 Fiber 标记为 `Failed` 并放弃挂起栈，不能展开该栈上的 C++ 局部对象，所以规范代码必须由固定父级收尾。

native x86_64 context 使用保护页栈。若系统有 Valgrind 头文件，FiberStack 在映射获得和释放时使用 `VALGRIND_STACK_REGISTER/DEREGISTER`，使 Memcheck 能识别自定义汇编换栈；这只影响检测标记，不改变运行时栈布局。

## 混合同步原语

### Mutex

`src/sync.cpp` 用一个原子字节发布 `kLocked` 和 `kWaiter`，等待队列由 `m_impl->m_mutex`保护。等待者位只在持有队列锁时发布并入队，`Unlock` 也在同一把队列锁下完成：

1. 检查锁仍由当前 owner 持有。
2. 从 FIFO 头部移除已取消节点，直到找到 `TryFinish(Notified)` 的唯一获胜者。
3. 有获胜者时保持 `kLocked`，把锁直接交给该 waiter；没有获胜者时清除锁和等待者位。
4. 释放队列锁后再执行唤醒，避免锁反转。

这使“发布等待者”和“释放/交接锁”只有一个线性化顺序。managed Fiber 在没有 native waiter 时最多协作重试 64 次；有 native waiter 后最多再重试 4 次，随后进入 FIFO，避免普通线程饥饿。`WaitNode` 的取消使用 wake gate 和 generation，防止 Context 回调在节点回收到 TLS cache 后修改下一次等待。

### ConditionVariable、WaitGroup、Context

ConditionVariable 先把 WaitNode 发布到自己的队列，再释放外部 Mutex；通知和 Context 取消都用 `TryFinish` 单赢家，`Await` 结束后必重新取得外部 Mutex。WaitGroup 的 count 检查、入队和零值转移在同一把锁内；零值转移会摘取整波 waiter，再在锁外唤醒。

Context 的取消线性化点是 `State::error` 在该节点锁内第一次写入。取消遍历先标记整棵子树，再执行 rollback 内部回调、`Done` 信号和普通用户回调；父子登记使用弱引用，child 析构会反向移除父节点登记。Deadline timer 在 Context 锁外移除，并在写入 timer id 时再次检查 `error`，覆盖“timer 先到期、注册尚未完成”的竞态。

## Channel 和 Select

Channel 的 buffer、send/recv 队列、closed 标志和 generation 由一个 `m_mutex`保护。发送线性化顺序是“旧 receiver 交接 → buffer 入队 → sender waiter 入队”；接收顺序是“buffer 出队 → sender 交接 → closed 零值 → receiver waiter 入队”。`Close()` 在锁内设置 closed、摘下全部等待队列并给每个节点写入终态，锁外统一唤醒；因此 close 后不会再接受发送，buffer 会先被接收再报告 closed。

Select 使用 `SelectWaitState::TrySelect`/`Cancel` 的单赢家。每个 armed case 的节点由 shared_ptr 保持，返回、取消或超时都执行 disarm；迟到的 channel 操作只能看到已选/已取消状态，不能再次消费值。内建 `SelectCase` 对不可复制的 Channel 元素显式报告 `kInvalid`，move-only 业务应使用 `SelectValue`/`Caster`。

## IO、Hook 和 fd 代际

`IOManager::State` 用 fd generation、registration id 和 WaitNode outcome CAS 丢弃迟到的 epoll 事件。注册 readiness 后才注册 deadline；timeout、close、cancel、readiness 竞争时，只有第一个 `claim()` 能改变 outcome，WakeList 在回调完成后才回收节点。

本轮修复了两个确定的竞态：

- 没有 reader/writer waiter 时撤销旧的 epoll registration，避免无消费者事件和 fd 代际复用残留。
- 初次 descriptor token 校验后，在取得 State 锁、发布等待节点前再次检查 token。close/dup2 在这两个检查之间发生时，不会把失效 fd 放回没有后续事件的队列。

有等待者时保留已经发布的 ET interest 位，原因是内核可能已经把旧 registration 的事件放入 epoll 队列；直接收缩并删除旧 registration 会丢迟到事件。只有整个 fd 没有等待者时才撤销 registration，并把 pending 位留给下一次注册消费。

## 验证证据

本轮使用 canonical WSL checkout `/mnt/e/CodexWorkspace/NewGo2Cpp`：

- `cmake --build build-context-test -j2`：通过，`-Wall -Wextra -Wpedantic`。
- `timeout 90s ctest --test-dir build-context-test --output-on-failure -j2`：13/13 通过。
- `./build-context-test/go2cpp_high_load_stress`：大负载检查通过；同一版本连续 3 次均在 90 秒 watchdog 内完成。
- `build-engineering-werror`：3/3 通过（工程化重点目标）。
- ASan/UBSan：现有 `build-native-asan` 全量单元测试通过；未发现 sanitizer 报告。
- Valgrind Memcheck：`build-native-context/valgrind-audit3.log`，`ERROR SUMMARY: 0`；definite、indirect、possible 均为 0，416 bytes/4 blocks 为进程级 still reachable 缓存。新增的自定义嵌套 Fiber IO 测试也包含在该运行中。
- TSan：WSL 默认运行受 `unexpected memory mapping` 启动限制，不能据此声称全套 TSan 通过；应在原生 Linux runner 上完成最终竞态门禁。

## 仍然不能由本框架保证的内容

当前结论不覆盖 Go 编译器/ABI/GC 等价、异步抢占、Go 风格动态 segmented stack、任意未 Hook 的系统调用、跨 Fiber 持有原生锁、任意 C++ 栈的强制终止和 native Linux TSan 之外的宿主差异。IO 高负载仍可能受宿主调度和 epoll 事件形态影响；这不是把本轮状态机安全误写成性能保证的理由。

## 复测后修复的混合 Mutex 生命周期竞态

后续 Release 高负载复测发现一个此前未覆盖到的确定性风险：`Mutex::Impl::m_waiters`
原来保存裸 `WaitNode*`，而 `Unlock()` 在弹出节点、释放队列锁后才调用 `Wake()`。
等待方可能在这个窗口中结束等待并销毁栈节点，或把拥有节点重置后放回线程本地缓存，
使解锁方访问悬空或已复用的对象。该链路解释了随机 SIGSEGV、永久等待和混合 Mutex
性能长尾。

当前实现让队列持有 `std::shared_ptr<WaitNode>`，所有争用等待者通过
`AcquireWaitNode()` 获取节点，`Unlock()` 将 shared ownership 保持到锁外
`Wake()` 返回后。修复后 Release CTest 14/14、Werror CTest 3/3、ASan/UBSan
单测和 Valgrind 全量单测均通过；Valgrind `ERROR SUMMARY: 0`，definite、
indirect、possible leak 均为 0。该修复增加了争用等待的引用计数开销，混合 Mutex
仍有明显性能长尾，不能把安全修复误写成性能优化。
