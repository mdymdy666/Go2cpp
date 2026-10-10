# 模块边界审计与使用契约

本文记录 Go2Cpp 各模块在并发、生命周期、错误和扩展点上的边界。通过的单元测试只能证明已覆盖路径；未列为“已修复”的项目不能按 Go 运行时的完全语义使用。

## 1. GMP 调度器

### 已检查

- G 任务只有一个运行 claim，进入本地队列、P 队列或全局队列时由状态和队列计数共同保护。
- M 只通过 Scheduler worker 运行 G；P 的 attached/detached 计数由调度器主锁和 P 队列锁维护。
- sysmon 只处理 `BlockingRegion` 与 Hook 发布的长阻塞，不能从另一个线程强行打断任意 C++ 调用栈。
- 任务结束状态现在区分 completed、failed、cancelled 和 suspended；取消不再被观察者错误报告为 suspended。

### 使用契约与限制

- 调度是协作式的。没有 Go 编译器的异步抢占、`_Psyscall` 精确交接、分段栈或 Go 的公平性保证。
- 未封装的阻塞系统调用必须显式包在 `BlockingRegion` 中；不能跨 `yield`、park、IO 或任务边界。
- Scheduler 必须长于所有 worker、Task、IOManager 和观察者回调；`shutdown()` 只接受协作式停止，不会终止仍在执行的 C++ 栈。
- M 动态扩缩受 `min_workers/max_workers` 和系统线程资源限制；monitor 不会在高负载时无限复制自身。

## 2. Fiber、上下文和栈

### 已修复

- native context 的页大小失败、页对齐加法和映射尺寸加法均进行回退与溢出检查，极端尺寸会抛出长度错误，不会回绕成错误映射。
- Fiber 的父级、当前执行绑定和恢复 continuation 会在调度切换时恢复；普通调度路径不会把嵌套 Fiber 直接跳回 main Fiber。

### 使用契约与限制

- 当前使用受保护固定栈：Linux x86_64 由 Go2Cpp 自有上下文后端提供，其他平台回退 Boost.Context（平台判定见 `cmake/Go2CppModules.cmake:3-10`，后端说明见 `docs/design.md` 的「x86_64 Linux Fiber Context 后端」）。没有 Go `morestack/newstack/copystack` 自动扩容。必须根据最深调用链设置 `fiber_stack_size`；栈越界会触发保护页故障。
- 销毁仍可恢复的挂起 Fiber 不能安全执行其 C++ 栈上的析构函数或 defer；调用方必须先让 Fiber 完成、取消并回到稳定边界，再释放拥有者。
- `panic/recover` 是显式状态记录设施，不会进行语言级栈展开，也不能跳过 C++ 析构函数；不能把它当作 Go panic 的完全替代。
- defer/recover 的精确边界是当前执行状态和 API 规定的受控调用范围，不支持任意跨 Fiber、跨 M 的隐式恢复。

## 3. Context、Timer 和回滚

- Context 取消幂等，子树先标记再通知，Done 回调在信号锁外执行；回调必须短小、非阻塞并且可重入安全。
- TimerService 默认单线程串行执行到期回调。慢回调会延迟同一服务上的其他 deadline；需要隔离时把工作投递给 Scheduler。回调不得销毁正在执行回调的所属 TimerService，生命周期操作应投递到其他执行流。
- `ContextRollback` 只负责显式注册的补偿动作，不是事务数据库，也不回滚任意外部副作用。
- Context value 使用类型化 key 和不可变共享值；不要把裸指针指向短生命周期对象。

## 4. 混合 Mutex、ConditionVariable、WaitGroup 和 Future

- `sync::Mutex` 不绑定线程，Fiber 和普通线程可混合等待、解锁和通知；不支持递归加锁，成功加锁必须恰好对应一次解锁。
- 不得把 `std::mutex`、文件锁或持有外部锁的对象跨 Fiber park/yield；否则会阻塞 carrier M，sysmon 也无法安全迁移其 C++ 栈。
- 自定义 WaitBackend/FutureWaitBackend 工厂在锁外执行；后端异常会回到调用方，Future 的 Context 回调由作用域守卫清理。
- 条件变量通知不累积；WaitGroup 计数必须先 Add 后启动任务，计数归零后不能再次使用同一轮。

## 5. Channel、Select 和 Timer 等待

- Channel close 只允许一次；关闭会唤醒发送、接收和 select 等待者。关闭后接收返回零值和成功标志，关闭后发送返回明确错误/终止路径。
- select 的 probe/arm 失败现在会取消已注册节点并立即返回 `kInvalid`，不能在未注册事件时进入无限等待；调用方仍应为每个 work 周期提供 Context 或 deadline。
- Channel 元素需要满足实现要求的移动/复制异常约束；有副作用的复制构造函数不要放入无缓冲同步路径。
- Timer 取消、Channel close 和 Context cancel 都是幂等竞争；用户回调不得保存临时 `string_view` 或等待节点地址。

## 6. IOBackend、Hook 和 socket

- 默认 epoll 使用 level-triggered readiness，避免同一 fd 有多个等待者且第一个只消费部分数据时丢失后续唤醒。
- 一个 IOBackend 实例只能由一个 IOManager 独占；不能把同一个 `shared_ptr<IOBackend>` 注入多个 manager。不同 manager 必须分别调用工厂创建后端。
- managed Fiber 的 socket 读写会先注册 readiness，再设置 deadline；`WaitAny/WaitMany` 需要受管 Fiber。普通线程应使用原生 `poll/select`。
- Hook 对 `poll/select/epoll_wait` 等等待函数仍有明确支持边界；绕过 interposer 的 raw syscall 不进入 fd generation 跟踪。
- `MSG_WAITALL`、特殊 variadic ioctl/fcntl、fork/exec 继承和非 Linux 后端不是完整 Go 兼容面。`SIGPIPE` 保留 Linux 原生语义，调用方需要 `MSG_NOSIGNAL`、`SO_NOSIGPIPE` 或进程级信号策略。
- close 与并发 dup/dup2 必须遵守 POSIX 生命周期约束；不要在线程仍使用旧 fd 时立即复用其整数值。

## 7. Error、日志和配置

- Error 查询会隔离用户 `Unwrap/Is/As` 的普通异常；但用户自定义的 `noexcept` 虚函数若违反契约仍会 terminate。`AsMutable` 只能用于确实由非 const 对象创建的错误。
- Logger parent 必须是无环树；`SetParent` 会拒绝自环和间接环。自定义 Filter、Formatter、Sink、CallbackSink 必须自行保证线程安全，且不能保存 `string_view` 到回调返回之后。
- 日志扩展点异常不能作为业务控制流；插件应捕获自己的异常并报告错误。磁盘满、rename 失败等输出错误必须由应用监控 sink 状态。
- IniFile 解析失败不会覆盖上一份完整快照。`.lock` 是协作式编辑标志而非内核互斥锁：写入方应先写非空锁、使用临时文件完成内容并原子 rename，最后清空锁。
- Config 的单变量原子发布不等于跨变量读取快照。需要一致批次时只在 `CommitListener` 收到完整提交后读取；不要在批次 Apply 期间要求多个 `GetValue()` 组成无锁快照。
- `ConfigVar` listener 可能重入、并发且不保证跨变量回调顺序；监听器不得捕获已销毁的裸对象。`BindRuntimeConfig` 的目标对象必须覆盖整个监听生命周期，并由调用方提供额外同步。

## 8. 本轮验证

本轮新增/修正了：Error 自定义解包异常隔离、默认 IOBackend 不可注销、Fiber native 栈尺寸溢出检查、epoll 多等待者回归、Scheduler cancelled 事件、Future 回调作用域清理、IniFile 失败快照保持和 Logger parent 环检测。

验证命令（WSL）：

```text
cmake --build build-final --parallel 1
ctest --test-dir build-final --output-on-failure
GO2CPP_TEST_FILTER=error ./build-final/go2cpp_tests
GO2CPP_TEST_FILTER=io ./build-final/go2cpp_tests
```

ASan、TSan、UBSan 和 Valgrind 的历史结果仍以 `docs/testing.md` 中对应构建目录日志为准；任何未覆盖的 sanitizer 配置不得解释为不存在竞态或泄漏。