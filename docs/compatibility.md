# 兼容性矩阵

本项目是可独立使用的 C++17 库，实现文档明确的 Go 运行时语义子集。
它不是 Go 解析器、编译器、ABI、垃圾回收器，也不声称与 Go 语言完全等价。

| Go 能力 | 已实现子集 | 明确差异或前置条件 | 验证 |
|---|---|---|---|
| G/M/P 调度 | `Task` 对应 G，worker `Machine` 对应 M，配置的 `Processor` 对应 P。每个 G 只有一个 scheduler owner、一个执行 claim、本地/全局 FIFO 队列、受限窃取、park/unpark pending-wake 交接、取消和 shutdown；sysmon 可对已声明长阻塞 M 做逻辑 detach 并申请替代 M。 | 调度只在显式 Fiber/yield/park 边界协作进行。没有编译器异步抢占、分段 Go 栈、Go 精确公平性或精确 `_Psyscall` 交接。sysmon 只观察 `BlockingRegion`/Hook 边界，不会发现或打断任意未 Hook 的 native 调用；替代 M 受 `max_workers`/严格模式限制。Scheduler/IOManager 必须长于 worker 和并发成员调用；析构或最终 shutdown 不是并发操作原语。 | `test_scheduler`、`test_dynamic_scheduler`、smoke 压力和 `dynamic_gmp_demo` |
| G 完成与生命周期 | callable 返回、普通 C++ 异常被 Fiber 记录或取消后都会到达 terminal；失败可通过 `Task::failed()/failure()/rethrow_failure()` 观察。shutdown 唤醒 parked G 并 join worker；Fiber trampoline 返回后立即释放已完成 Fiber 栈和 callable，即使用户仍持有 `Task`。提供 `shutdown_for()` 有界等待。 | 永不返回的非协作 callable 会使 shutdown/join 等待，类似未 join 的 `std::thread`；有界 shutdown 超时只返回 false，不释放仍运行的栈，后续必须再次收尾。Scheduler/IOManager owner 必须长于 worker。Fiber 自身不能析构自身；错误父级销毁会安全标记 Failed，但挂起栈上的 RAII 无法补做。 | Fiber 生命周期、shutdown、取消队列 capture 回收、析构重入和 watchdog 测试 |
| 动态 M | `min_workers`、有界 `max_workers`、按 backlog 增长、空闲回收、死亡 worker 回收、P 计数和软 task-class affinity。sysmon 超过阈值后把已声明长阻塞 M 从 P 计数中逻辑解绑，保持替代 M 到原调用返回。 | 普通 runnable 峰值保持 P 上限；替代目标按最小 worker 底线加 detached 数量计算，并受 `max_workers`/严格模式限制。不会从异线程抢占 C++ 栈，也不能自动发现任意未 Hook 的阻塞调用；这不是 Go 的精确 P 交接。affinity 只是有界偏好，不是缓存保证；不做 CPU 绑核。 | 动态增长/收缩/再增长、严格 P 上限、BlockingRegion overcommit、sysmon 长调用和 affinity 测试 |
| Fiber | Boost.Context `fcontext` 栈式 backend，带保护栈页、M 间迁移、errno 保存、取消后自然完成、ASan/TSan 切换钩子，以及可跨迁移并在 Fiber 完成时回收的 `FiberLocalCache/FiberLocal`。普通线程使用 TLS fallback；嵌套父链使用共享 `FiberRecord` 墓碑元数据。 | Boost.Context 是实现依赖且当前为固定大小栈；FiberLocal 值不是栈或对象池，key 是进程内单调 ID。普通 `thread_local` 状态不得跨迁移保存。resume 串行化；Ready Fiber 可安全跳过主体，Suspended Fiber 应由固定父级收尾。错误调用方析构会标记 Failed 并放弃上下文，不能展开挂起栈上的 RAII；Fiber owner/父级仍必须遵守生命周期契约。 | `test_fiber`、FiberLocal 隔离/生命周期、ASan/TSan/Valgrind Fiber 测试 |
| Socket/FD Hook | Linux shared interposer 默认启用，包装 `socket`/`socketpair`、`connect`、`accept*`、读写/recv/send、close、dup*、fcntl/ioctl、getsockopt/setsockopt 和 sleep；managed 阻塞操作接入 epoll。readiness 先于 deadline 发布，结果单次 claim，FD generation 防止 close/reuse。普通线程使用原生阻塞或 bounded poll fallback；managed Fiber 的 epoll 路径使用非阻塞 syscall 后 park；未跟踪 FD fallback 和 sleep fallback 在可能进入 libc 阻塞前发布 M::Blocking。 | 这是有界 syscall 表面：managed Fiber 的 `MSG_WAITALL` 返回 `ENOTSUP`，因为未模拟部分读取累计；普通线程转发 libc，但已被 runtime 接管的 socket 仍可能是 kernel nonblocking，不能保证完整 blocking `MSG_WAITALL`。managed `MSG_OOB` 返回 `ENOTSUP`；`recvmmsg`/`sendmmsg`、`sendfile`/`splice`、`poll`/`select`/`ppoll`、特殊信号重启、未知 variadic fcntl/ioctl、fork/exec 继承和非 Linux 平台均是边界。绕过 interposer 的 raw syscall 不在 generation 跟踪内。`bind_io_manager` 不得跨 Fiber 迁移或长于 manager。静态构建需关闭 Hook。IOManager 另提供 `WaitAny/WaitMany`，把多个 fd 一次注册到同一个 epoll poller，由任一就绪节点唤醒一个 Fiber；普通线程调用明确返回 `EPERM`，应直接使用原生 `poll/select`。 | `test_io`、`test_hook`、readiness/timeout 竞态、close/reuse、native fallback 和 `io_hook_demo` |
| context 根与取消 | `Background`、`TODO`、`WithCancel`、`WithCancelCause`、父子传播、幂等取消、`Done` 回调/等待、`Canceled`/`DeadlineExceeded`、cause、steady deadline、timeout 和不可变 typed/string value。取消遍历先标记整棵子树，再执行回调；managed G 会 park Fiber。 | 0.x 没有 `WithoutCancel`。根 context 忽略直接取消，`Done()` 是非空、永久 pending 的 `DoneSignal`，不同于 Go nil channel。空 parent 映射为 `Background()`（Go 会 panic）。值使用不可变 C++ `std::any`，typed key identity 不是 Go interface value。 | context 树、深层取消、fake clock、回调竞态和 managed Done |
| panic/recover/defer | 未实现。Fiber 只在边界捕获普通 C++ 异常并通过 `Fiber::failure()` 报告，用户代码应使用 C++ RAII、`try/catch` 和显式错误返回。 | 不提供 Go 的 panic、recover、defer 控制流，也不提供隐式异常转换。需要这些语义的转译代码不属于当前支持范围。 | 普通 C++ 异常 Fiber 边界测试 |
| error | 不可变 shared error、null-as-nil、包装、`Unwrap`、循环安全 `Is`/`As`、`Join` 以及与 context/channel 的转换。单元素 `Join` 保留 `JoinError` wrapper。 | 没有 Go interface/typed-nil ABI。`Wrap(nullptr, ...)` 返回 null。自定义 ownership/message 图必须无环；error 不会隐式变成 panic。 | 深链、identity/type 和 cycle 测试 |
| channel | 类型化容量 0/1/N、FIFO buffer、多生产者/消费者、阻塞 send/receive、close 唤醒、关闭后的零值接收、取消/deadline、方向视图、select/default/timeout 和无缓冲 rendezvous。managed G park 而不是阻塞 M。 | nil channel 返回显式 `kNil`，不模拟 Go 永久阻塞；空 select 返回 invalid。发送后关闭返回 status，`Close()` 不抛异常。为保证强异常安全，`Channel<T>` 要求 T 可不抛 move 构造且析构不抛；select 的 `std::any` 结果还要求值可复制。Channel 对象必须长于 raw member call/waiter；自定义 probe/回调必须满足 non-throwing rollback 契约。 | channel FIFO/close/select、P=1、取消和压力 |
| 协程同步 | `Fiber`、FIFO `sync::Mutex`、`ConditionVariable`、可复用 `WaitGroup`、timeout/cancellation，以及普通线程和 managed G 共用等待队列。 | native contention 在 waiter condition_variable 上阻塞；managed caller park Fiber；两者可相互 unlock/notify，锁不绑定线程。对象必须长于 waiter；不允许把 thread-owned `std::mutex` 跨 Fiber yield 或 Hook IO。 | `test_sync`、native/Fiber ownership/notification、P=1、取消/shutdown 和 `fiber_sync_demo` |
| 定时器与 Fiber 等待 | `TimerService` 提供可取消的单调时钟任务；定时器回调通过 channel、`DoneSignal` 或等待节点唤醒 Fiber，`Channel::WaitForChange` 使用 `ParkingCondition`，P=1 时不会占住唯一 M。 | 定时器回调本身运行在专用 native 线程，不会直接恢复 Fiber 栈；需要执行 Fiber 工作时必须显式调用 `go()`/`Scheduler::spawn()`。回调和拥有者的生命周期由调用方保证，取消是幂等的。 | `test_timer`、P=1 等待/唤醒和取消回归 |
| 新手 API | `go(scheduler, cb)`、进程级 `go(cb)`、小写 `fiber`、`Scheduler::add()`、`SelectLoop`/`EventBatch`。 | 便利层不提供编译器 `go` 关键字、隐式全局事件循环或任意阻塞回调的自动转换；默认调度器关闭后旧句柄不可再提交任务，事件状态访问有协程感知锁；结构修改与 `work()` 可并发但采用快照语义，批次对象仍须长于所有调用者。 | `test_beginner_api`、`beginner_demo` |
| 线程参与与 Hook 作用域 | `ScopedThreadParticipation` 记录每线程 GMP eligibility 和 Scheduler 关联；`ScopedThreadHookMode` 覆盖进程 Hook；runtime worker 自动标记。 | 外部 participation 只是元数据，不附加 M、不执行队列、不提供 run_one。Hook 关闭时 managed code 需显式 nonblocking API 或 `BlockingRegion`。 | `test_scheduler`、`test_hook`、`mixed_runtime_demo` |

公共 result/status API 会显式暴露不支持状态，而不是假装等价于 Go。
所有权和可替换边界见 `docs/design.md`、`docs/scheduler.md`、`docs/dependencies.md`。

直接 `syscall(2)`、`io_uring`、`close_range` 和 glibc no-cancel entry point
会绕过 interposer，可能阻塞 M，也不在 FD generation 跟踪和 managed Fiber
唤醒语义内。

## 用户代码硬约束

为了让 C++ 栈式 Fiber 的生命周期和 Go 风格的协作调度保持可验证，当前版本要求：

1. `Scheduler`、`IOManager`、`Mutex`、`ConditionVariable`、`WaitGroup`、`Channel` 和 Context 必须长于所有并发调用者及等待节点；不能在 worker 或 Fiber 自身执行期间销毁 owner。
2. 嵌套 Fiber 必须由创建它的固定父级继续 `resume()`，父 Fiber 返回前应收尾所有子 Fiber。逃逸的挂起子 Fiber 会进入 Failed 放弃路径，挂起栈上的普通 C++ 局部变量不会再执行析构。
3. 跨线程恢复 Fiber 时，最初执行 `resume()` 的线程和调用栈必须保持存活到该 Fiber 完成；不能把根 Fiber 从已经退出的线程迁移给其他线程。
4. `FiberLocal` 值的析构函数必须不抛异常；`BlockingRegion` 不得跨 `yield`、`park`、IO 或任务边界。违反这些后端不变量会进入 fail-fast，而不是继续运行可能损坏的栈或 GMP 记账。
5. `Channel<T>` 只接受不抛 move 构造和不抛析构的值类型；select 传输还应使用可复制值。任意 raw syscall、未 Hook 的阻塞调用和不在文档列出的 C ABI 都不会被 sysmon 自动抢占。
6. Hook 保留 POSIX 的 `SIGPIPE` 行为，不全局修改进程信号策略；需要忽略断开连接信号时，由调用方使用 `MSG_NOSIGNAL` 或设置自己的信号处理策略。

## 2026-09-23 监控与嵌套 Fiber 补充

- sysmon 活性：monitor 使用独立等待条件和主锁 try_to_lock 扫描；sysmon_pass_count 是周期心跳尝试计数，sysmon_running 是线程存活标志。只保留一个 monitor，M 的高负载扩容由有界 worker 策略完成，不能突破 max_workers，也不能发现 raw/未声明阻塞。
- 嵌套 IO：单 FD 等待可在三层 Fiber 父链中连续经历 timeout、ready、cancel 和 close，唤醒后恢复原 continuation；父级对象失效时只保留诊断墓碑，不能恢复已释放的栈。`WaitAny/WaitMany` 可在一个 Fiber 中原子注册多个 FD；任一完成后会取消清理其余节点，结果按请求索引返回。
- 系统调用错误：Hook 保留 libc 返回值与 errno。monitor 不终止 Fiber、不迁移 C++ 栈、不把 write/send 错误转换成 C++ 异常；是否包装为 error 由调用者显式决定。Hook 不全局屏蔽 SIGPIPE，调用者需按普通 POSIX 约定使用 `MSG_NOSIGNAL` 或进程信号策略。
- 栈：固定保护栈而非 Go 自动增长栈；不能把 C++ 挂起栈安全复制为 Go 栈。超过配置栈容量属于可诊断失败，后续若要替换 segmented-stack backend 必须单独验证 ABI、sanitizer 和 RAII。
