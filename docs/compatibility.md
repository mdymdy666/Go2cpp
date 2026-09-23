# 兼容性矩阵

本项目是可独立使用的 C++17 库，实现文档明确的 Go 运行时语义子集。
它不是 Go 解析器、编译器、ABI、垃圾回收器，也不声称与 Go 语言完全等价。

| Go 能力 | 已实现子集 | 明确差异或前置条件 | 验证 |
|---|---|---|---|
| G/M/P 调度 | `Task` 对应 G，worker `Machine` 对应 M，配置的 `Processor` 对应 P。每个 G 只有一个 scheduler owner、一个执行 claim、本地/全局 FIFO 队列、受限窃取、park/unpark pending-wake 交接、取消和 shutdown；sysmon 可对已声明长阻塞 M 做逻辑 detach 并申请替代 M。 | 调度只在显式 Fiber/yield/park 边界协作进行。没有编译器异步抢占、分段 Go 栈、Go 精确公平性或精确 `_Psyscall` 交接。sysmon 只观察 `BlockingRegion`/Hook 边界，不会发现或打断任意未 Hook 的 native 调用；替代 M 受 `max_workers`/严格模式限制。Scheduler/IOManager 必须长于 worker 和并发成员调用；析构或最终 shutdown 不是并发操作原语。 | `test_scheduler`、`test_dynamic_scheduler`、smoke 压力和 `dynamic_gmp_demo` |
| G 完成与生命周期 | callable 返回、panic 被处理/未处理或取消后都会到达 terminal。shutdown 唤醒 parked G 并 join worker；Fiber trampoline 返回后立即释放已完成 Fiber 栈和 callable，即使用户仍持有 `Task`。直接调用 `Task::run()` 有独立执行 claim。 | 永不返回的非协作 callable 会使 shutdown/join 等待，类似未 join 的 `std::thread`。未启动的取消任务可能在旧队列节点回收前保留 callable。Scheduler/IOManager owner 必须长于 worker；从自身 G 析构会 fail-fast，避免 UAF。运行时绝不强行丢弃挂起的 C++ 栈。 | Fiber 生命周期、shutdown、取消队列 capture 回收、析构重入和 watchdog 测试 |
| 动态 M | `min_workers`、有界 `max_workers`、按 backlog 增长、空闲回收、死亡 worker 回收、P 计数和软 task-class affinity。sysmon 超过阈值后把已声明长阻塞 M 从 P 计数中逻辑解绑，保持替代 M 到原调用返回。 | 普通 runnable 峰值保持 P 上限；替代目标按最小 worker 底线加 detached 数量计算，并受 `max_workers`/严格模式限制。不会从异线程抢占 C++ 栈，也不能自动发现任意未 Hook 的阻塞调用；这不是 Go 的精确 P 交接。affinity 只是有界偏好，不是缓存保证；不做 CPU 绑核。 | 动态增长/收缩/再增长、严格 P 上限、BlockingRegion overcommit、sysmon 长调用和 affinity 测试 |
| Fiber | Boost.Context `fcontext` 栈式 backend，带保护栈页、M 间迁移、errno 保存、取消后自然完成、ASan/TSan 切换钩子，以及可跨迁移并在 Fiber 完成时回收的 `FiberLocalCache/FiberLocal`。普通线程使用 TLS fallback；嵌套父链使用共享 `FiberRecord` 墓碑元数据。 | Boost.Context 是实现依赖；FiberLocal 值不是栈或对象池，key 是进程内单调 ID；临时 cache wrapper 会保留条目到 Fiber 完成。普通 `thread_local` 状态不得跨迁移保存。resume 串行化；Ready Fiber 可安全跳过主体，Suspended Fiber 必须由固定父级恢复，错误析构调用方会 fail-fast，不能假装等价于 Go 栈回收。 | `test_fiber`、FiberLocal 隔离/生命周期、ASan/TSan/Valgrind Fiber 测试 |
| Socket/FD Hook | Linux shared interposer 默认启用，包装 `socket`/`socketpair`、`connect`、`accept*`、读写/recv/send、close、dup*、fcntl/ioctl、getsockopt/setsockopt 和 sleep；managed 阻塞操作接入 epoll。readiness 先于 deadline 发布，结果单次 claim，FD generation 防止 close/reuse。普通线程使用原生阻塞或 bounded poll fallback；managed Fiber 的 epoll 路径使用非阻塞 syscall 后 park；未跟踪 FD fallback 和 sleep fallback 在可能进入 libc 阻塞前发布 M::Blocking。 | 这是有界 syscall 表面：managed Fiber 的 `MSG_WAITALL` 返回 `ENOTSUP`，因为未模拟部分读取累计；普通线程转发 libc，但已被 runtime 接管的 socket 仍可能是 kernel nonblocking，不能保证完整 blocking `MSG_WAITALL`。managed `MSG_OOB` 返回 `ENOTSUP`；`recvmmsg`/`sendmmsg`、`sendfile`/`splice`、`poll`/`select`/`ppoll`、特殊信号重启、未知 variadic fcntl/ioctl、fork/exec 继承和非 Linux 平台均是边界。绕过 interposer 的 raw syscall 不在 generation 跟踪内。`bind_io_manager` 不得跨 Fiber 迁移或长于 manager。静态构建需关闭 Hook。 | `test_io`、`test_hook`、readiness/timeout 竞态、close/reuse、native fallback 和 `io_hook_demo` |
| context 根与取消 | `Background`、`TODO`、`WithCancel`、`WithCancelCause`、父子传播、幂等取消、`Done` 回调/等待、`Canceled`/`DeadlineExceeded`、cause、steady deadline、timeout 和不可变 typed/string value。取消遍历先标记整棵子树，再执行回调；managed G 会 park Fiber。 | 0.x 没有 `WithoutCancel`。根 context 忽略直接取消，`Done()` 是非空、永久 pending 的 `DoneSignal`，不同于 Go nil channel。空 parent 映射为 `Background()`（Go 会 panic）。值使用不可变 C++ `std::any`，typed key identity 不是 Go interface value。 | context 树、深层取消、fake clock、回调竞态和 managed Done |
| defer | 显式 `Frame` 在注册时求值参数，正常返回和 panic 展开均按 LIFO 执行，处理期间新增 defer 也会执行。 | 转译器必须显式创建 frame；C++ 编译器不会自动注入。 | 嵌套/LIFO、参数捕获、正常/panic |
| panic/recover | 每 Fiber 独立 `ExecutionContext`，嵌套 frame 展开，recover 只能在活动 panic 期间直接执行的 defer 中生效，支持 re-panic、`panic(nil)` 标记、未处理观察器和跨 G 隔离。 | runtime 控制流不使用 C++ throw、promise、`longjmp`/`setjmp`；用户 C++ throw 在最外层 body 转为未处理 panic。`panic()` 只记录状态，不改写普通 C++ 控制流；转译代码必须返回到 frame 边界。没有 handler 时 scheduler 终止 G，但不模拟 Go 进程级 abort。 | panic/defer 和 Fiber 迁移测试 |
| error | 不可变 shared error、null-as-nil、包装、`Unwrap`、循环安全 `Is`/`As`、`Join` 以及与 context/channel 的转换。单元素 `Join` 保留 `JoinError` wrapper。 | 没有 Go interface/typed-nil ABI。`Wrap(nullptr, ...)` 返回 null。自定义 ownership/message 图必须无环；error 不会隐式变成 panic。 | 深链、identity/type 和 cycle 测试 |
| channel | 类型化容量 0/1/N、FIFO buffer、多生产者/消费者、阻塞 send/receive、close 唤醒、关闭后的零值接收、取消/deadline、方向视图、select/default/timeout 和无缓冲 rendezvous。managed G park 而不是阻塞 M。 | nil channel 返回显式 `kNil`，不模拟 Go 永久阻塞；空 select 返回 invalid。发送后关闭返回 status，`SendOrPanic` 才触发 panic。Channel 对象必须长于 raw member call/waiter；select transfer 和自定义 probe 必须满足 non-throwing rollback 契约。 | channel FIFO/close/select、P=1、取消和压力 |
| 协程同步 | `Fiber`、FIFO `sync::Mutex`、`ConditionVariable`、可复用 `WaitGroup`、timeout/cancellation，以及普通线程和 managed G 共用等待队列。 | native contention 在 waiter condition_variable 上阻塞；managed caller park Fiber；两者可相互 unlock/notify，锁不绑定线程。对象必须长于 waiter；不允许把 thread-owned `std::mutex` 跨 Fiber yield 或 Hook IO。 | `test_sync`、native/Fiber ownership/notification、P=1、取消/shutdown 和 `fiber_sync_demo` |
| 定时器与 Fiber 等待 | `TimerService` 提供可取消的单调时钟任务；定时器回调通过 channel、`DoneSignal` 或等待节点唤醒 Fiber，`Channel::WaitForChange` 使用 `ParkingCondition`，P=1 时不会占住唯一 M。 | 定时器回调本身运行在专用 native 线程，不会直接恢复 Fiber 栈；需要执行 Fiber 工作时必须显式调用 `go()`/`Scheduler::spawn()`。回调和拥有者的生命周期由调用方保证，取消是幂等的。 | `test_timer`、P=1 等待/唤醒和取消回归 |
| 新手 API | `go(scheduler, cb)`、进程级 `go(cb)`、小写 `fiber`、`Scheduler::add()`、`SelectLoop`/`EventBatch`。 | 便利层不提供编译器 `go` 关键字、隐式全局事件循环或任意阻塞回调的自动转换；默认调度器关闭后旧句柄不可再提交任务，事件状态访问有协程感知锁；结构修改与 `work()` 可并发但采用快照语义，批次对象仍须长于所有调用者。 | `test_beginner_api`、`beginner_demo` |
| 线程参与与 Hook 作用域 | `ScopedThreadParticipation` 记录每线程 GMP eligibility 和 Scheduler 关联；`ScopedThreadHookMode` 覆盖进程 Hook；runtime worker 自动标记。 | 外部 participation 只是元数据，不附加 M、不执行队列、不提供 run_one。Hook 关闭时 managed code 需显式 nonblocking API 或 `BlockingRegion`。 | `test_scheduler`、`test_hook`、`mixed_runtime_demo` |

公共 result/status API 会显式暴露不支持状态，而不是假装等价于 Go。
所有权和可替换边界见 `docs/design.md`、`docs/scheduler.md`、`docs/dependencies.md`。

直接 `syscall(2)`、`io_uring`、`close_range` 和 glibc no-cancel entry point
会绕过 interposer，可能阻塞 M，也不在 FD generation 跟踪和 managed Fiber
唤醒语义内。

## 2026-09-23 监控与嵌套 Fiber 补充

- sysmon 活性：monitor 使用独立等待条件和主锁 try_to_lock 扫描；sysmon_pass_count 是周期心跳尝试计数，sysmon_running 是线程存活标志。只保留一个 monitor，M 的高负载扩容由有界 worker 策略完成，不能突破 max_workers，也不能发现 raw/未声明阻塞。
- 嵌套 IO：单 FD 等待可在三层 Fiber 父链中连续经历 timeout、ready、cancel 和 close，唤醒后恢复原 continuation；父级对象失效时只保留诊断墓碑，不能恢复已释放的栈。没有公共多 FD wait_any/Fiber select，多个 FD 需拆成多个 G/Fiber 后使用 Channel Select 汇合。
- 系统调用错误：Hook 保留 libc 返回值与 errno。monitor 不终止 Fiber、不迁移 C++ 栈、不把 write/send 错误转换成 panic；是否包装为 error 由调用者显式决定。
- 栈：固定保护栈而非 Go 自动增长栈；不能把 C++ 挂起栈安全复制为 Go 栈。超过配置栈容量属于可诊断失败，后续若要替换 segmented-stack backend 必须单独验证 ABI、sanitizer 和 RAII。
