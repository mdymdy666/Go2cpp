# 运行时设计与边界

## 范围与契约

Go2Cpp 是独立的 C++17 运行时/库，不是 Go 解析器、编译器或源码转译器。
当前支持 Linux，使用标准库、POSIX 线程、Linux epoll/eventfd 和可替换的
Fiber Context 后端。Linux x86_64 默认使用 Go2Cpp 自有汇编后端，其余平台回退
Boost.Context。公共名称位于 `go2cpp` 命名空间；0.x 不承诺
二进制 ABI 稳定。默认构建为共享库并启用 Linux Hook，静态构建必须关闭
`GO2CPP_BUILD_HOOK`，以保证进程内只有一份 FD/TLS 注册表。

调度、取消和等待不把 C++ 异常当作运行时控制流，也不使用 future、promise、
setjmp 或 longjmp。库提供显式的 RAII defer 和共享状态 panic/recover：它们
用于可验证的错误发布与作用域清理，不尝试跳转或复制 C++ 挂起栈。用户回调抛出的
普通 C++ 异常由 Fiber 边界捕获并记录到 Fiber::failure()，Task 通过失败状态报告；
普通 error 不会隐式转换为异常或 panic。

## 显式 defer、panic、recover

defer 不可复制，构造时按值保存回调参数，析构或 run_now() 只执行一次；同一 C++
作用域内的声明顺序决定逆序（LIFO）执行。回调内部通过 recover::take() 或
operator() 消费已绑定 panic 的状态，回调外调用会返回空或 false。panic::call()
只记录 PanicInfo（字符串、整数码或 std::any payload），不会执行 longjmp、抛出
内部专用异常，也不会替用户结束当前函数。defer 回调中的普通 C++ 异常被捕获到
当前线程的 LastDeferException()，不会穿过 noexcept 析构。因此这是一套显式状态
协议，不是 Go 编译器隐式插入的栈帧展开；需要跨 Fiber 传递时应显式共享 panic
对象并由目标 Fiber 的 defer 回调处理；库不保存 G/Fiber owner token，无法自动拒绝跨执行流 recover，用户必须自行绑定执行流。

## 所有权与生命周期

Task、Channel、Context、等待节点和 Fiber 状态均使用 RAII、`shared_ptr` 与
`weak_ptr`。Scheduler 注册表强持有尚未终态的 Task（包括 parked G），队列和
等待节点也只保存共享句柄。已启动 Task 要等 Fiber trampoline 自然返回后才从
注册表移除；未启动且已取消的 Task 可以在 admission/队列清理时回收。用户持有
的 Task 句柄可能让回调捕获的对象继续存活。

Context 子节点使用弱登记、父节点使用强锚点并在取消后清理，不形成循环引用。
Timer 回调只捕获弱等待节点或显式共享状态；每个等待节点在所有者返回前都有
一次性 gate/disarm 步骤。Scheduler/IOManager 必须长于所有 worker、外部成员调用
和未完成等待；析构、最终 `shutdown()`、`start()`、`enqueue()` 以及快照调用不能
并发。若从自身 G 析构 Scheduler 会快速失败，避免 C++ 对象生命周期 UAF。
Channel 析构也不是并发取消原语，必须先 `Close()` 并等待所有 member call 结束。

## G/M/P 调度

`Task` 是 G，内部 worker 线程是 M，`Processor` 是有界调度令牌 P。每个 G 只能
绑定一个 Scheduler，最多拥有一个队列 claim (`m_queued`) 和一个执行 claim
(`m_execution_claim`)。本地队列满时转入全局 FIFO；worker 依次尝试本地队列、
全局队列和受限的 victim 窃取。入队、出队、runnable 计数和 `running` claim 在
admission mutex 下提交，避免 wake 与 dequeue 竞态造成重复运行或丢任务。

合法 G 状态转换如下：

```text
new -> runnable -> running -> runnable | waiting | dead | cancelled | failed
waiting -> runnable | cancelled
failed/dead/cancelled -> terminal
```

`yield()`/`park()` 只能作用于当前 G。Fiber 后端保存 C++ continuation，恢复后从
原调用点继续，不会再次进入 callable。channel、IO、timer、Context 和同步通知
要么把 waiting G 原子转为 runnable，要么在 G 仍运行时记录一次 pending wake。
shutdown 先关闭 admission，取消未启动队列项，唤醒已启动等待 G，等待 Fiber 自然
返回，再由拥有者 join worker；`shutdown_for()` 超时只返回 false 并保留栈和停止状态，
后续调用必须继续收尾，绝不强行丢弃挂起的 C++ 栈。

M 从 `min_workers` 开始，根据 runnable backlog 和忙碌 M 数量有界扩展，空闲超时
回收至下限，死亡记录会 join/reap。`allow_worker_oversubscription=false` 时最大
M 严格不超过 P；默认模式下普通 runnable 峰值仍以 P 为上限。sysmon 线程以
`sysmon_interval` 检查 `BlockingRegion`/Hook 发布的 `MState::Blocking`，超过
`long_syscall_threshold` 后将 M 从 P 的 attached 计数逻辑解绑，并按
`min_workers + detached_count` 申请替代 M；原 M 返回时由 RAII 重新绑定，替代 M 在此
之前不会被 idle 回收。该机制不抢占异线程 C++ 栈、不模拟精确 Go P 交接，未 Hook 的
阻塞调用仍需显式 `BlockingRegion`，并受 `max_workers` 限制。task class 只提供有界
软亲和性，不保证线程缓存或 CPU 绑核。

## Fiber 与 FiberLocal

`go2cpp::Fiber` 使用固定大小保护栈和 trampoline，Linux x86_64 由 Go2Cpp 自有
Context ABI 完成切换，其他平台使用 Boost.Context，状态
为 Ready/Running/Suspended/Completed/Failed。resume 串行化并保存/恢复 errno，Fiber
可以迁移到不同 M；普通 `thread_local` 不应当当作 G-local。`FiberLocalCache<T>`
（别名 `FiberLocal<T>`）按逻辑 Fiber 保存共享值，迁移不丢失，trampoline 完成后
释放；普通线程使用 TLS fallback。它是值局部设施，不是栈池或 Fiber 对象池。
析构会请求取消并继续 Ready/Suspended Fiber 直到自然完成，因此忽略取消的 Fiber
可能让析构等待。错误父级或错误调用方无法安全恢复时，运行时会清空上下文、清理
FiberLocal 并标记 Failed；不会把挂起栈伪造为正常完成。

嵌套父链的诊断、取消和 alive 标志存放在共享 `FiberRecord` 链中，父对象结束后
保留墓碑但不保留可恢复栈；Suspended 子 Fiber 只能由固定父级继续恢复。错误调用方会放弃上下文并标记
Failed，挂起栈上的 RAII 无法在该路径补做。父/子对象和发起 resume 的调用栈必须
按 owner 契约存活；Fiber 自身析构、自身栈上销毁仍是禁止用法。Fiber 不安装独立的
panic/recover/defer 上下文。

## 线程参与、Hook 与阻塞

`ScopedThreadParticipation` 只记录外部线程是否愿意参与 Scheduler，不能把普通
线程伪装成 M、运行队列或提供 `run_one`。`ScopedThreadHookMode` 只覆盖当前线程
的 socket Hook；runtime worker 会自动标记为 managed worker。Hook 关闭时，managed
代码应使用非阻塞 API 或显式 `BlockingRegion`。

`BlockingRegion` 是不可移动 RAII 对象，记录进入它的 M，结束时修复 M 状态，且
不能跨 Fiber yield/park/迁移；worker 在任务边界发现未销毁的 region 会 fail-fast。
它是 sysmon 的观测边界，只扩展线程池，不会把任意未 Hook 的阻塞调用变成可抢占操作。

## Context 与 Timer

取消传播采用迭代工作队列：每个状态只标记一次，先标记子树，再从叶到根触发
Done 回调。`DoneSignal::Wait/WaitFor/WaitUntil`、Channel 和同步等待在 managed G
中均走 `core::ParkingCondition`，Fiber 停靠后释放 M；native 线程才阻塞自己的
等待节点。Context deadline 使用单调时钟；TimerService 的回调在专用 native
定时器线程执行，只负责改变一次性状态或通知等待节点，绝不直接恢复 Fiber 栈。
要在回调后执行 Fiber 工作，必须显式 `go()`/`Scheduler::spawn()`。

Context key 使用进程内 identity token，值是不可变 `std::any`；字符串 key 仅为
动态转译代码提供便利。根 Context 的 DoneSignal 非空但永久不触发，这是与 Go
根 context nil channel 的明确差异。取消不会隐式变为 panic。

## x86_64 Linux Fiber Context 后端

Linux x86_64 默认使用 `detail/context_backend.hpp` 声明的 Go2Cpp 自有
上下文 ABI。其寄存器布局和首次 trampoline 参考 coost/TBOX 的
`tb_context_make/jump`，但没有复制 coost 的 `_exit` 终止路径，也没有复制
coost 的共享栈快照。每次 `JumpContext` 都返回保存的 caller context，因而
Fiber 可以在执行权由 Task 的 execution claim 串行化后迁移到另一个 M。

执行栈由 `FiberStack` 负责：底部保留 `mprotect(PROT_NONE)` guard page，映射
按大小进入当前 M 的 TLS 小缓存，溢出后进入有界全局缓存。这样去除了运行时对
Boost.Context 的链接依赖，同时保留越界保护和 ASan/TSan 的切换标记。非 Linux
x86_64 平台继续使用 Boost.Context fallback。

这里的 Context 指 Fiber 的寄存器/栈上下文；`src/context.cpp` 中的取消树、
Done、deadline、value Context 仍是独立的 C++ 运行时设施，coost 没有对应实现。
coost 的 Buffer 只是固定共享栈槽的挂起快照，不是自动动态栈，因此本项目仍不
声称实现 Go 式 `morestack`/`copystack`。

### Context 局部回滚

`ContextRollback` 是一个临时子 Context 的补偿边界：它在自己的状态锁下管理
LIFO undo 和 savepoint，显式 `rollback()` 会取消 child 并解除 parent 的弱登记，
`commit()` 只丢弃 undo 而保留 child 的正常取消传播。undo 在 Context 锁外执行，
异常被隔离并记录为 `kFailed`；`RollbackDone()` 与 Context `Done()` 分别表示
补偿完成和取消线性化。取消线程可能是 TimerService 或任意调用 `Cancel()` 的线程，
因此动作不得 park、yield、阻塞、访问 Fiber 专属状态或捕获悬空栈引用。
取消会先冻结 rollback action 日志并发布 child 的 `Done`，再由内部回调执行 undo，
最后才运行普通 `Done` 观察回调；需要等待注销动作完成的调用方仍必须等待
`RollbackDone()`。undo 不得等待自身的 `RollbackDone()` 或依赖尚未发布的信号。
事务状态操作可并发，但事务对象的移动和析构必须由 owner 串行化；内部回调先于
普通回调，普通 `DoneSignal` 回调之间只保证至多一次，不保证相互顺序。
只有显式登记的 undo 会执行；child 的后代取消不会反向触发 scope。该机制只补偿
尚未发布的内部注册关系，不能恢复父 Context、已发布 value、网络/文件写入或其他
外部副作用；当前不使用 `setjmp/longjmp`。

## Channel、Select 与同步

`Channel<T>` 使用互斥保护的 FIFO 缓冲、独立发送/接收等待队列和一次性
`SelectWaitState`。容量 0/1/N、多生产者/消费者、close、取消、deadline、方向
视图、select/default/timeout 都有状态结果。为保证接收和 select 的强异常安全，
T 必须是不抛 move 构造且析构不抛的类型；内建 RecvCase/SendCase 还要求 T
可复制。不可复制值可以通过普通 Send/Recv 或独立 SelectValue 传递，但放入
Channel Select/EventBatch 会立即返回 kInvalid，不会登记一个无法回滚的等待者。
close 唤醒所有等待者；缓冲排空后接收返回零值与 ok=false；
`sync::Mutex`、`ConditionVariable`、`WaitGroup` 使用 FIFO 等待节点和 disarm gate。
managed G 在等待前释放外部锁并 park，native 线程阻塞自己的 condition variable，
两者共用同一移交队列，所以一个线程可以解锁由另一个 M 执行的 Fiber。没有 Scheduler
的手动 Fiber 在竞争等待、已取消 Context 或超时路径返回 false 且不阻塞 carrier；
禁止把线程所有的 `std::mutex` 跨 Fiber yield 或 Hook IO；锁和等待对象必须长于所有等待者。

## Socket Hook 与等待顺序

Linux Hook 是共享 C ABI interposer，包装 socket、connect、accept、读写、close、
dup、fcntl/ioctl、超时和 sleep。IOManager 下的 Fiber 使用 epoll；普通 Scheduler
上的 managed Fiber 使用受限的 native poll fallback；普通线程转发 libc。每次阻塞
调用遵循固定顺序：先注册 readiness，再发布 deadline timer，再 park Fiber；readiness、
timeout、cancel、close 通过一个原子 outcome claim 唤醒，随后以相同绝对 deadline
重试 syscall。FD generation/token 防止 close/reuse ABA，`MSG_WAITALL`、`MSG_OOB`、
`poll/select`、`*mmsg`、io_uring、raw syscall 等明确返回/保留边界，不声称透明等价。

## 可替换边界与参考代码

SchedulerConfig、TaskOptions、BlockingRegion、FiberLocalCache、线程策略 scope、
Context 测试时钟、SelectCase、Descriptor token/guard 和 C Hook 控制是支持的替换
边界。0.x 没有插件 ABI；替换实现必须保持上述状态转换、单次 wake claim、所有权
和 shutdown 契约。

曾阅读 `/UserData/CodexWorkSpace/sylar2/sylar/` 中的 fiber/scheduler/IOManager/hook
作为 clean-room 设计参考，只吸收栈切换、队列、epoll 和 readiness-before-timer
等概念；该目录不是依赖，代码没有复制或参与构建。Go 上游参考、许可证和版本记录
见 `docs/dependencies.md` 与 `third_party/go-reference/README.md`。

## 新手 API

`go2cpp::go(scheduler, callback)` 会启动（若尚未启动）指定 Scheduler 并返回 Task；
`go2cpp::go(callback)` 使用进程级默认 Scheduler。调用
`shutdown_default_scheduler()` 后旧句柄不可再提交任务，下一次 `go()` 会创建新实例。
小写 `go2cpp::fiber` 只保存一个 Task，可用 `Scheduler::add()` 或 `fiber::bind()`
提交。`EventBatch`/`SelectLoop` 只包装 channel SelectCase，内部使用
`sync::Mutex` 串行 `work()`，支持 native/Fiber 调用；事件列表修改采用锁保护和
`work()` 快照语义。任意阻塞业务回调应先用 `go()` 启动并通过 channel 报告结果。
批次对象必须长于所有并发调用，`stop()` 通过内部 Context 唤醒等待，并只发出协作式停止请求。

## 2026-09-23 边界复核补充

### monitor 与父链

sysmon 的等待锁与 Scheduler 主锁分离，监控线程在队列锁竞争时仍能推进心跳；扫描本身采用 try_to_lock，因此可能跳过一轮，但不会无限等待。系统只创建一个 monitor 线程，避免重复扫描和 detached 计数竞争；高负载下由 worker 池按 backlog/BlockingRegion 有界增加 M，受 max_workers 和线程资源限制。monitor 只做逻辑 P 脱离，绝不异步打断 C++ 栈、终止 Fiber 或跨线程注入异常。

嵌套 Fiber 的 FiberRecord 保存 main_fiber、直接父级、当前执行绑定和挂起原因。SuspendForScheduler/park_io 使 IO 挂起沿父链传播，唤醒时恢复同一 continuation；三层链连续超时和 readiness 的测试确认不会跳回 main_fiber 或跳过父级。Suspended Fiber 仍应由固定父级恢复；错误生命周期会进入 Failed 并释放不可恢复上下文，这是 C++ RAII 无法跨栈复制的边界。

### 栈策略与多 FD 等待

Fiber 使用带保护页的固定大小 Boost.Context 栈（默认 128 KiB，可通过配置指定）。当前没有 Go morestack/newstack/copystack 式自动扩容：C++ 编译器不会提供 Go 栈图和可安全重写的挂起指针，直接复制正在运行的 C++ 栈会破坏 RAII、引用和 fcontext。需要更深调用栈时必须显式提高 fiber_stack_size，栈保护页会把越界变成可诊断故障，而不是声称已经实现 Go 栈增长。

Fiber 栈仍是固定保护栈，尚未实现 Go 风格动态扩容。IOManager 提供
`WaitAny/WaitMany`：每个请求对应一个带 generation 的 WaitNode，全部登记在
同一个 epoll poller 中；任一节点完成时通过 Scheduler 的 pending-wake 交接唤醒
当前 Fiber，返回 ready 请求索引，并对其余节点执行幂等取消。截止时间、Context
取消和 NotifyClose 会完成整组节点，避免残留 waiter。WaitMany 返回同一轮已
完成的全部索引，而不是“等待所有请求”的屏障；需要屏障时使用 WaitGroup。
同一集合不得重复提交相同 fd/方向，重复项返回 EINVAL；注册过程中遇到无效 fd
会取消已注册节点并返回整体错误。该接口要求 managed Fiber；普通线程得到 EPERM，
继续使用原生 poll/select。

SelectCaster 的函数对象可能被多个 Fiber 并发调用；若内部有可变状态，调用方必须自行加锁或为每个执行流创建独立实例。

## 2026-10-01 FiberBin 与 M 私有队列评估

调度器现在提供 `SchedulerConfig::fiber_bin_capacity` 和
`TaskOptions::fiber_bin_capacity`。任务完成或失败、上下文已经失效、没有活动
resume claim、且父记录没有其他持有者时，Fiber 对象只回收到当前 M 的线程本地
FiberBin；默认容量为 32，配置上限为 4096。复用前会生成新的 FiberRecord 和 ID，
清理父链、失败状态、取消标志和 sanitizer 状态。普通 Fiber 不进入该池，池中
对象也不会跨 M 共享；Fiber 栈仍由原有的受保护栈缓存负责回收。

曾验证过 M 私有任务队列方案：它需要同时改动 worker 消费优先级、yield 回队、
P/global 批量回灌、sysmon 脱离和 shutdown 排空。首次接入在 50,000 个任务、每个
任务 4 次 yield 的压力测试中出现完成超时，说明 runnable 计数、配额公平和等待
唤醒尚未形成完整线性化协议。因此当前版本不启用 MLocalTaskQueue，也不在配置中
暴露未实现的参数；现有 P 本地队列、incoming 分片和全局窃取路径保持为生产路径。
