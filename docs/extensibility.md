# 模块解耦与扩展契约

本文是 Go2Cpp 的扩展总览。新增能力应先确定职责和生命周期，再选择下表中的
扩展边界；不要在一个模块中直接调用另一个模块的私有实现。

| 模块 | 核心职责 | 可替换边界 | 不应承担的职责 |
| --- | --- | --- | --- |
| `error` | 错误值、包装、解包、身份和类型判断 | `Error` 派生类型、`UnwrapAll` | 不负责抛出 C++ 异常，不负责调度 |
| `context` | 取消树、截止时间、Value、局部回滚 | 时钟、取消原因、监听器 | 不直接恢复 Fiber，不拥有业务资源 |
| `fiber` | 栈上下文、恢复、挂起、FiberLocal | Context 后端、栈分配器、诊断记录（当前为编译期后端） | 不决定队列策略，不管理 worker |
| `scheduler` | G/M/P、队列、窃取、M 生命周期 | 调度策略、阻塞观测、指标观察器 | 不实现 socket 协议和日志格式 |
| `sync` | Fiber/线程共用锁、条件变量、WaitGroup | 等待节点、park/wake 后端 | 不持有 Scheduler 所有权 |
| `channel` | FIFO、缓冲、关闭、select、取消 | 值转换 Caster、等待策略 | 不依赖具体 IO 后端 |
| `io`/`hook` | FD 生命周期、readiness、超时、取消 | Poller、平台 Hook、FD token | 不复制 Fiber 栈，不吞掉系统错误 |
| `timer` | 单调时钟、定时任务、取消 | TimerScheduler（当前默认单线程实现，替换接口属于后续边界） | 不直接执行业务 Fiber |
| `log` | Filter、Item、Formatter、Sink | Item 工厂、Sink、第三方适配器 | 不依赖 Scheduler 内部锁 |
| `config` | INI、类型校验、批次提交、热加载 | ValueCodec、校验器、提交监听器 | 不直接修改未注册的业务字段 |

## 扩展规则

### 批次事务

配置或其他需要整体更新的模块必须采用“预检、准备、提交、通知、回滚”的阶段协议。
预检阶段不得修改运行状态；提交阶段只发布不可变快照；通知阶段在内部事务锁外调用
插件；任何准备失败都必须逆序回滚。跨键约束不能依赖正在编辑的文件前缀，文件写入方
必须使用 `.lock` 加临时文件和原子 `rename`。

### Fiber、栈和调度

Fiber 的寄存器上下文、栈分配、FiberLocal 和 G/M/P 调度是四个不同职责。未来加入
分段栈、动态栈、不同 Fiber 类型或第三方上下文库时，应实现对应后端接口，不能让
Scheduler 直接访问栈布局。后端必须声明创建、恢复、挂起、销毁和失败状态；在没有
可验证的 C++ 栈迁移方案前，不得复制运行中的栈来声称实现 Go `morestack`。

### 多平台 IO

IOManager 只依赖 Poller 的 readiness、取消、关闭和超时协议。Linux epoll、BSD
kqueue、Windows IOCP 和 io_uring 只能作为后端实现；平台差异不得泄漏到 Channel、
Context 或 Scheduler 的公共状态机。每个等待请求使用 FD generation/token 和一次性
outcome claim，防止 close/reuse、超时和 readiness 并发时重复唤醒。

### 插件生命周期

插件注册返回稳定 ID 或句柄，注销后不能影响已经创建的快照对象。工厂和回调不在
注册表锁内执行；异常必须隔离并转成模块定义的错误或失败状态。插件不得捕获挂起
Fiber 的裸栈地址，也不得在回调中销毁仍被等待节点引用的 owner。

### 测试要求

每个新后端至少提供：正常路径、取消、超时、关闭、重复调用、资源分配失败和并发
随机测试。默认后端和替换后端必须共享同一组状态机不变量测试。跨模块改动完成后，
至少运行 Debug/Release、`-Werror`、高负载并发测试；涉及内存或等待节点时再分别运行
ASan、UBSan、TSan 和 Valgrind，不把单次通过当成无竞态证明。
