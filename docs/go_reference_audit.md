# Go 参考源码与 C++ 实现偏差审计

## 结论先行

Go2Cpp 不是 Go runtime 的逐行移植，也不是经过形式化证明的等价实现。
Go `go1.23.0` 源码和测试是语义参考目标；C++ 实现使用独立的数据结构、线程、Fiber、RAII 和 C++ 内存模型。
因此“Go 源码正确”不能推出“Go2Cpp 对应组件必然正确”。当前项目只能声称实现了文档明确、经过项目测试的兼容子集。

完整 Go 源码已在 WSL 展开到：

```text
third_party/go1.23.0-full/
```

该目录来自 `third_party/go1.23.0.src.tar.gz`，归档 SHA-256 为：

```text
42b7a8e80d805daa03022ed3fde4321d4c3bf2c990a144165d01eeecd6f699c6
```

完整目录只作为本地只读参考，不进入 Git，也不加入 CMake target。

## 明确没有按 Go runtime 原实现的部分

### 1. G/M/P 调度器

参考文件：

- `third_party/go1.23.0-full/src/runtime/proc.go`
- `third_party/go1.23.0-full/src/runtime/runtime2.go`

当前 C++ 实现：

- `src/scheduler.cpp`
- `include/go2cpp/scheduler/scheduler.hpp`
- `include/go2cpp/fiber.hpp`

不是原实现的地方：

- Go 的 `G`、`M`、`P` 内部结构、状态位、栈所有权和 GC 扫描状态没有移植；C++ 使用 `Task`、worker 和 `Processor`。
- 没有 Go 编译器生成的栈图、`g0`、`mcall`、`gogo`、`gopark` ABI、分段/可增长 Go 栈。
- 没有 Go 的异步抢占、`sysmon`、精确 `GOMAXPROCS`、锁定 goroutine、系统调用 P 交接、GC stop-the-world 协议、cgo 调度协议。
- Fiber 只能在显式 `yield`、`park`、Hook IO 和库等待点切换；未被 Hook 的 native 阻塞调用仍可能占用 worker。
- `BlockingRegion` 是 C++ 的有界替代线程机制，不是 Go 的 `_Psyscall` 和精确 M/P 交接。
- 本地/全局队列、窃取和公平策略是项目自定义实现，不保证与 Go 的 `runqget`、`globrunqget`、`findRunnable` 逐行为一致。
- `FiberLocal` 是 C++ Fiber 的值存储，不是 Go 的 G-local 或 GC 感知对象。

### 2. Context

参考文件：

- `third_party/go1.23.0-full/src/context/context.go`
- `third_party/go1.23.0-full/src/context/context_test.go`

当前 C++ 实现：

- `src/context.cpp`
- `include/go2cpp/context.hpp`

不是原实现的地方：

- Go 使用 `Context` 接口，C++ 使用具体的 `Context` 类和 `shared_ptr`。
- Go 的 `Background().Done()` 和 `TODO().Done()` 返回 `nil` channel；C++ 根 Context 返回一个永久 pending 的 `DoneSignal`。
- 没有完整提供 Go 1.23 的 `WithoutCancel`、`AfterFunc`、`WithDeadlineCause`、`WithTimeoutCause` API。
- Go 的 key 可以是任意可比较的 interface value；C++ 主要使用进程内 identity token、字符串 key 和 `std::any`。
- 空 parent 的处理与 Go 不同，C++ 将空 parent 映射为 `Background()`。
- C++ 使用自己的 TimerService 和 callback 表；它不是 Go context 的 `cancelCtx`、`timerCtx`、`afterFuncCtx` 和 `propagateCancel` 内部实现。
- C++ callback 会在自己的取消传播流程中执行；不能据此声称与 Go Done channel 的关闭时序、调度和 goroutine 行为完全相同。

### 3. Channel / select

参考文件：

- `third_party/go1.23.0-full/src/runtime/chan.go`
- `third_party/go1.23.0-full/src/runtime/select.go`
- `third_party/go1.23.0-full/src/runtime/chan_test.go`

当前 C++ 实现：

- `include/go2cpp/channel.hpp`
- `src/channel.cpp`

不是原实现的地方：

- Go channel 是编译器和 runtime 共同使用的 `hchan`，C++ 是模板 `Channel<T>` 和共享状态对象。
- Go nil channel 的阻塞 send/receive 会永久阻塞；C++ 返回 `ChannelStatus::kNil`，不会复现永久阻塞语义。
- Go 向 closed channel 发送必然 panic；C++ 普通 `Send` 返回 closed status，只有 `SendOrPanic` 才记录运行时 panic。
- Go 重复 close 会 panic；C++ `Close` 返回 `kAlreadyClosed`。
- Go 的 select 由编译器/runtime 生成并使用 sudog、selectgo、随机化顺序；C++ 使用 `SelectCase`、probe/arm/disarm 和自定义等待状态。
- C++ 的 select 公平性是有限的轮转游标，不是 Go runtime 的随机选择算法和调度交互。
- C++ 的 timeout、Context、取消和自定义 probe 是额外协议，不存在于 Go `chan.go` 的同一实现边界。
- 通道对象生命周期由 C++ 调用方负责，不能依赖 Go GC 自动保活。

### 4. defer / panic / recover

参考文件：

- `third_party/go1.23.0-full/src/runtime/panic.go`
- `third_party/go1.23.0-full/src/runtime/defer_test.go`
- `third_party/go1.23.0-full/src/runtime/panic_test.go`

当前 C++ 实现：

- `include/go2cpp/panic_defer.hpp`
- `src/panic_defer.cpp`

不是原实现的地方：

- Go 编译器自动插入 defer；C++ 必须显式创建 `panic_defer::Frame`。
- Go panic 会由 runtime 进行栈展开；C++ `panic()` 主要记录状态，必须返回到活动 Frame 边界才能完成展开。
- 没有 Go 的精确 goroutine 栈追踪、`Goexit`、runtime panic 进程终止和内部 ABI。
- 用户 C++ exception 在最外层被转换为未恢复 panic，这是兼容层策略，不是 Go 行为。
- `panic(nil)` 使用 C++ 的显式 nil 标记，不等同于 Go runtime 的完整 panic nil 对象和版本细节。

### 5. error

参考来源：Go 标准库 `errors` 包及 context 错误语义。

当前 C++ 实现：

- `include/go2cpp/error.hpp`
- `src/error.cpp`

不是原实现的地方：

- C++ 使用不可变 `shared_ptr<Error>`，没有 Go interface value、动态类型和 typed-nil ABI。
- `Wrap(nullptr, ...)`、空错误和单元素 `Join` 的行为是项目定义，不是 Go 内部对象布局。
- 错误链的所有权、循环保护和线程安全由 C++ 实现负责，不是 Go `error` 接口自动提供。
- error 不会隐式触发 panic，这是项目明确边界。

### 6. Fiber、Timer、同步和 Socket Hook

这些能力没有对应的 Go runtime 逐行移植：

- Fiber 使用 Boost.Context 和固定保护栈，不是 Go 的可增长用户栈和 GC 栈扫描。
- Timer 使用 C++ `TimerService` 专用线程，不是 Go runtime timer heap/netpoller 的完整实现。
- Mutex、ConditionVariable、WaitGroup 是 Fiber-aware C++ 同步原语，不是 Go `sync` 包的内部实现。
- Socket Hook 是 Linux interposer + epoll，不能覆盖 raw syscall、io_uring、所有 libc 变体和全部阻塞入口。
- `BlockingRegion`、Hook scope、FiberLocal 和 EventBatch 都是 Go2Cpp 自定义设施。

## 可以说“参考了 Go”的部分

- 调度模型的 G/M/P 概念、runnable/waiting/dead 等状态意图。
- Context 的父子取消、deadline、timeout、value、cause 语义目标。
- Channel 的容量、FIFO、阻塞收发、关闭和 select 目标。
- defer 的注册时参数求值、LIFO 和 panic 路径执行目标。
- panic/recover 的同 Fiber 隔离和直接 defer 恢复边界目标。
- error 的包装、解包、身份判断和 Join 目标。

这些是“语义目标”，不是 Go runtime 内部代码的证明性复刻。

## 审计建议

如果要求达到更高可信度，下一步应做 Go/C++ 双实现的随机对拍、逐测试映射、调度状态机模型检查，以及对 Context/Channel/select/panic 的差异用例补齐；在这些工作完成前，不应宣称 100% Go 兼容。
## 自举前 C 源码的实际位置

如果目标是查看“最初不是用 Go 写的 Go”，不要从 `go1.23.0-full` 开始。应查看：

```text
third_party/go1.4.3-c-bootstrap/
```

该目录从 Go 1.4.3 官方归档抽取 C、头文件、汇编及少量 yacc/lex 输入，重点包括：

```text
src/runtime/proc.c
src/runtime/panic.c
src/runtime/malloc.c
src/runtime/chan.h
src/runtime/asm_amd64.s
src/cmd/5c/
src/cmd/6c/
src/cmd/gc/
```

注意：Go 1.4.3 的 runtime 已经是 C、Go 和汇编混合实现；不存在一个“整个 runtime 都只有 C”的 Go 1.4 源码目录。这里抽取的是自举前真正由 C/汇编承担的部分。Go 1.4.3 仍有 `proc.go`、`chan.go` 等 Go 文件，而 Go 1.5 才完成主要 compiler/runtime 自举转换。

