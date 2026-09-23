# 可运行示例

`example/runtime_demo.cpp` 是端到端示例。它创建 G/M/P 调度器，通过类型化
channel 传递值，绑定 timeout context，演示 `WithValue` 和显式子节点取消，
使用仅发送/仅接收视图执行 `select`，并格式化和检查包装错误的身份。运行时
不提供 Go 的 panic/recover/defer 控制流，用户代码应使用普通 C++ 异常和 RAII。
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
