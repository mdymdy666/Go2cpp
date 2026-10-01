# 可运行示例

第一次接触项目建议按以下顺序运行：

1. `beginner_demo.cpp`：最短的 `go()`、Scheduler 和 Channel 示例；
2. `runtime_demo.cpp`：Context、错误链和 Select 的组合用法；
3. `fiber_sync_demo.cpp`：Fiber 与普通线程共用 Mutex、ConditionVariable 和
   WaitGroup；
4. `io_wait_many_demo.cpp`：多个 FD 的 epoll 等待、超时和取消；
5. `context_rollback_demo.cpp`：Context 局部回滚和补偿动作。

详细的环境准备、生命周期约束和故障排查请先阅读
[`docs/getting_started.md`](../docs/getting_started.md)。示例都要求先构建
`GO2CPP_BUILD_EXAMPLES=ON` 的构建目录；示例返回前会等待任务并关闭自己创建的
Scheduler，便于复制到新项目。

`example/runtime_demo.cpp` 是端到端示例。它创建 G/M/P 调度器，通过类型化
channel 传递值，绑定 timeout context，演示 `WithValue` 和显式子节点取消，
使用仅发送/仅接收视图执行 select，并格式化和检查包装错误的身份。显式控制流
示例见 control_flow_demo.cpp；它展示 RAII defer、状态式 panic/recover 和
SelectCaster，不伪造 Go 的隐式栈跳转。
构建命令：

```sh
cmake -S . -B build -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build --target go2cpp_runtime_demo
./build/go2cpp_runtime_demo
```

普通线程与 managed Fiber 的边界场景请运行 `example/mixed_runtime_demo.cpp`：

```sh
cmake --build build --target go2cpp_mixed_runtime_demo
./build/go2cpp_mixed_runtime_demo
```

该示例让普通线程和 Fiber 共同使用 `sync::Mutex`、`ConditionVariable`、
`WaitGroup` 的等待队列，检查 `FiberLocal` 状态跨协作式 yield 保持，按普通
线程设置 Hook 参与策略，并声明短生命周期 `BlockingRegion` 让替代 M 服务
队列任务。外部 `ScopedThreadParticipation` 只是策略/资格作用域；它不会把
普通线程附加成调度器 worker。


## 新手入口

`example/beginner_demo.cpp` 展示最短的 `go(scheduler, callback)`、
`go2cpp::fiber`、`Scheduler::add()` 和 `SelectLoop` 用法；不想显式
管理调度器时可以直接 `go2cpp::go(callback)`，程序结束前调用
`shutdown_default_scheduler()`。

## 多 FD 等待

example/io_wait_many_demo.cpp 同时注册两个 socket FD，演示 WaitManyFor() 收集
多个就绪索引，再用 WaitAnyFor() 验证超时。它只在 managed Fiber 中调用
IOManager 多路等待；普通线程仍应使用系统 poll/select。

## Context 局部回滚

`example/context_rollback_demo.cpp` 展示 `ContextRollback` 的新手入口：先登记
补偿动作，再用 `savepoint()` 局部回滚；如果没有调用 `commit()`，离开作用域时会
自动完成剩余回滚。构建并运行：

```sh
cmake --build build --target go2cpp_context_rollback_demo
./build/go2cpp_context_rollback_demo
```

## 控制流与 Caster

    cmake --build build --target go2cpp_control_flow_demo
    ./build/go2cpp_control_flow_demo

SelectValue/SelectCaster 的完整测试在 test_control_flow 中；普通 std::any
自定义事件可用 SelectProbe::SetAny() 接入同一转换路径。独立 SelectValue 支持
move-only；Channel Select/EventBatch 的内建 case 要求可复制值，违反时返回
kInvalid 而不是永久等待。
