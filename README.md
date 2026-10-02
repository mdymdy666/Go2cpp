# Go2Cpp 运行时语义兼容层

本仓库提供一个可独立使用的 C++17 库，实现文档明确的 Go 运行时语义子集。
它是运行时/库，不是 Go 解析器，也不是 Go 到 C++ 的源码转译器。

公共 API 位于 `go2cpp` 命名空间，包含：

- M/P/G 风格调度器：受限 P、本地/全局队列、工作窃取、M 动态增减、显式
  `BlockingRegion` 替代 M、park 和 shutdown；
- 带截止时间、值和取消原因的 context 取消树；
- 可迁移的栈式 Fiber、`FiberLocal` 值、调度器感知的互斥锁/条件变量/等待组，
  以及支持阻塞、关闭和 select 的类型化 channel；
- Linux epoll `IOManager` 和默认启用、带 FD generation 关闭保护的 socket Hook；
- 支持 `Is`、`As`、`Unwrap`、`Join` 的不可变错误链；
- 线程安全、可替换 Filter/Formatter/Worker/Sink 的日志模块，以及可校验的 INI
  配置模块；默认 warning/error 写入 `log/go2cpp.log`，stdout 需显式开启。
- Fiber 边界捕获普通 C++ 异常并通过 Fiber::failure()/Task::failure() 报告；资源清理由 RAII 和 try/catch 负责。
- 提供显式 defer、panic、recover 状态对象：defer 用 RAII 保证作用域退出的 LIFO 回调，
  panic::call() 发布共享错误状态，recover::take() 只能在 defer 回调中消费；它不会伪造 C++ 栈跳转。
- 独立的 SelectValue/SelectCaster 提供可替换的类型擦除中介；它可以承载不可
  复制值并转换成业务类型。内建 Channel 的 RecvCase/SendCase 和 EventBatch
  为保证等待节点可回滚，要求 T 可复制；move-only 值请使用普通 Send/Recv 或
  直接使用 SelectValue。

构建并运行默认测试套件：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

默认 Linux 构建启用共享模块和透明 Hook。静态构建请使用
`-DBUILD_SHARED_LIBS=OFF -DGO2CPP_BUILD_HOOK=OFF`。

兼容性矩阵、设计边界和可复现验证矩阵分别见
[`docs/compatibility.md`](docs/compatibility.md)、[`docs/design.md`](docs/design.md)
以及模块扩展契约 [`docs/extensibility.md`](docs/extensibility.md)
和 [`docs/testing.md`](docs/testing.md)。源码/依赖清单及 Go 参考版本见
[`docs/dependencies.md`](docs/dependencies.md) 和
[`third_party/go-reference/README.md`](third_party/go-reference/README.md)。

第一次使用请先阅读
[`docs/getting_started.md`](docs/getting_started.md)。该文档包含环境要求、最短
可运行程序、Fiber/线程混合规则、Channel/Context/IO 生命周期、配置校验和常见
错误排查步骤。

可运行示例位于 `example/`：`runtime_demo.cpp`、`fiber_sync_demo.cpp`、
`mixed_runtime_demo.cpp`、`managed_pipeline_demo.cpp`、`dynamic_gmp_demo.cpp`
以及 Linux Hook 构建下的 `io_hook_demo.cpp`。其中
`mixed_runtime_demo.cpp` 展示普通线程与 Fiber 的同步、FiberLocal 值、
线程级 Hook 策略和显式 `BlockingRegion`。

日志和配置的完整新手示例是 `example/log_config_demo.cpp`，工程配置模板是根目录
的 `go2cpp.ini`，详细说明见 [`docs/logging.md`](docs/logging.md) 和
[`docs/configuration.md`](docs/configuration.md)。

配置中心支持强类型 `ConfigVar<T>`、变更监听器和文件热加载；日志模块支持多
Formatter/Sink、Sylar 风格格式、Logger 父级传播和滚动文件输出。


## 新手入口

如果只需要类似 Go 的提交方式，可以先启动一个 Scheduler，然后直接调用：

```cpp
#include "go2cpp/runtime.hpp"

go2cpp::Scheduler scheduler(2);
scheduler.start();
auto task = go2cpp::go(scheduler, [] { /* Fiber 工作 */ });
task->wait();
scheduler.shutdown();
```

不想管理 Scheduler 时，可使用进程级默认调度器，并在程序结束前显式关闭：

```cpp
auto task = go2cpp::go([] { /* Fiber 工作 */ });
task->wait();
go2cpp::shutdown_default_scheduler();
```

`go2cpp::fiber` 是保存一个任务的轻量描述器，`Scheduler::add()` 或
`fiber::bind()` 会把它加入指定调度器。`go2cpp::SelectLoop`/`EventBatch`
只包装已有的 channel `SelectCase`；可能阻塞的业务逻辑仍应放进 `go()`，
再通过 channel 传递结果。事件状态访问支持普通线程与 Fiber 并发，结构修改
采用锁保护和快照语义；批次对象仍须长于所有调用者，`stop()` 通过内部 Context 唤醒等待，并只发出协作式停止请求。

转译代码需要完整公共接口时，包含 `go2cpp/runtime.hpp`。模块 target
（`go2cpp::error`、`go2cpp::control_flow`、`go2cpp::context`、`go2cpp::channel`、
`go2cpp::scheduler`、`go2cpp::fiber`、`go2cpp::sync`、`go2cpp::io`、
`go2cpp::log`、`go2cpp::config`、`go2cpp::hook`）既可在源码树中单独使用，
也会与 `go2cpp::runtime` umbrella target 一起导出。


新手完整示例是 `example/beginner_demo.cpp`，构建后运行：

```sh
cmake --build build --target go2cpp_beginner_demo
./build/go2cpp_beginner_demo
```
## 显式控制流与类型转换

    go2cpp::panic failure;
    go2cpp::recover recovery(failure);
    {
        go2cpp::defer cleanup([&] {
            if (auto info = recovery.take()) {
                // 处理显式 panic 状态
            }
        });
        failure.call("错误码", 7);
        return;  // C++ 不会自动跳转，业务代码自行结束当前路径
    }

defer 的带参数构造会在注册时复制/移动参数；回调异常不会穿过析构函数，
可通过 LastDeferException() 查询。SelectResult::TypedValue<T>() 和
MakeCaster<From, To>() 用于把 channel/select 的类型擦除值转换成业务类型；
move-only 结果使用 SelectResult::TakeValue<T>()；但 Channel Select/EventBatch
不会对 move-only T 建立异步等待，调用会立即返回 kInvalid，避免静默挂起。
panic/recover 是显式共享状态，库不验证 G/Fiber 身份，调用方应把它们限制在同一
执行流中。

## 多 FD 等待

IOManager::WaitAnyFor() 和 WaitManyFor() 只在该 IOManager 的 managed Fiber
中挂起并接入 epoll；普通线程返回 EPERM，应直接使用原生 poll/select。
