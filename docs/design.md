# 运行时设计与边界

## 范围与契约

Go2Cpp 是独立的 C++17 运行时/库，不是 Go 解析器、编译器或源码转译器。
当前支持 Linux，使用标准库、POSIX 线程、Linux epoll/eventfd 和可替换的
Boost.Context 栈式 Fiber 后端。公共名称位于 `go2cpp` 命名空间；0.x 不承诺
二进制 ABI 稳定。默认构建为共享库并启用 Linux Hook，静态构建必须关闭
`GO2CPP_BUILD_HOOK`，以保证进程内只有一份 FD/TLS 注册表。

调度、取消、等待和 panic/defer 的运行时控制流不使用 C++ 异常、future、promise、
`setjmp` 或 `longjmp`。用户回调意外抛出的 C++ 异常会在 Fiber/Task 最外层边界
被记录为未恢复 panic，正常 C++ 析构仍会执行；转译器必须显式创建
`panic_defer::Frame`。普通 `error` 永远不会隐式变成 panic。

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
new -> runnable -> running -> runnable | waiting | dead | cancelled
waiting -> runnable | cancelled
dead/cancelled -> terminal
```

`yield()`/`park()` 只能作用于当前 G。Fiber 后端保存 C++ continuation，恢复后从
原调用点继续，不会再次进入 callable。channel、IO、timer、Context 和同步通知
要么把 waiting G 原子转为 runnable，要么在 G 仍运行时记录一次 pending wake。
shutdown 先关闭 admission，取消未启动队列项，唤醒已启动等待 G，等待 Fiber 自然
返回，再由拥有者 join worker；绝不强行丢弃挂起的 C++ 栈。

M 从 `min_workers` 开始，根据 runnable backlog 和忙碌 M 数量有界扩展，空闲超时
回收至下限，死亡记录会 join/reap。`allow_worker_oversubscription=false` 时最大
M 严格不超过 P；默认模式下普通 runnable 峰值仍以 P 为上限，只有显式
`BlockingRegion` 为一个已知的短 native 阻塞调用申请替代 M，并受
`max_workers` 限制。阻塞 M 保留其 P 记账令牌，替代 M 可能共享 P，这不是异步
抢占或精确 Go P 交接。task class 只提供有界软亲和性，不保证线程缓存或 CPU 绑核。

## Fiber 与 FiberLocal

`go2cpp::Fiber` 使用 Boost.Context `fcontext_t`、固定大小保护栈和 trampoline，状态
为 Ready/Running/Suspended/Completed/Failed。resume 串行化并保存/恢复 errno，Fiber
可以迁移到不同 M；普通 `thread_local` 不应当当作 G-local。`FiberLocalCache<T>`
（别名 `FiberLocal<T>`）按逻辑 Fiber 保存共享值，迁移不丢失，trampoline 完成后
释放；普通线程使用 TLS fallback。它是值局部设施，不是栈池或 Fiber 对象池。
析构会请求取消并继续 Ready/Suspended Fiber 直到自然完成，因此忽略取消的 Fiber
可能让析构等待，但不会使用危险的强制栈释放。

每次 resume 周围用 `panic_defer::Binding` 安装 Fiber 自己的 ExecutionContext，
避免 M 复用下 panic/recover 状态串 G。

## 线程参与、Hook 与阻塞

`ScopedThreadParticipation` 只记录外部线程是否愿意参与 Scheduler，不能把普通
线程伪装成 M、运行队列或提供 `run_one`。`ScopedThreadHookMode` 只覆盖当前线程
的 socket Hook；runtime worker 会自动标记为 managed worker。Hook 关闭时，managed
代码应使用非阻塞 API 或显式 `BlockingRegion`。

`BlockingRegion` 是不可移动 RAII 对象，记录进入它的 M，结束时修复 M 状态，且
不能跨 Fiber yield/park/迁移。它只扩展线程池，不会把任意未 Hook 的阻塞调用变成
可抢占操作。`panic_defer::panic()` 只记录运行时状态；转译代码必须回到活动
Frame 边界，不能把它当作普通 C++ 控制流跳转。

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

## Channel、Select 与同步

`Channel<T>` 使用互斥保护的 FIFO 缓冲、独立发送/接收等待队列和一次性
`SelectWaitState`。容量 0/1/N、多生产者/消费者、close、取消、deadline、方向
视图、select/default/timeout 都有状态结果。close 唤醒所有等待者；缓冲排空后接收
返回零值与 `ok=false`；发送已关闭 channel 返回 closed status，只有
`SendOrPanic()` 才记录 Go 风格 panic。空 select 返回 invalid，避免测试永久挂起。

`sync::Mutex`、`ConditionVariable`、`WaitGroup` 使用 FIFO 等待节点和 disarm gate。
managed G 在等待前释放外部锁并 park，native 线程阻塞自己的 condition variable，
两者共用同一移交队列，所以一个线程可以解锁由另一个 M 执行的 Fiber。禁止把
线程所有的 `std::mutex` 跨 Fiber yield 或 Hook IO；锁和等待对象必须长于所有等待者。

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
批次对象必须长于所有并发调用，`stop()` 只发出协作式停止请求。
