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
- 显式 frame/defer/panic/recover 协议，运行时控制流不使用 C++ 异常或 `longjmp`。

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
和 [`docs/testing.md`](docs/testing.md)。源码/依赖清单及 Go 参考版本见
[`docs/dependencies.md`](docs/dependencies.md) 和
[`third_party/go-reference/README.md`](third_party/go-reference/README.md)。

可运行示例位于 `example/`：`runtime_demo.cpp`、`fiber_sync_demo.cpp`、
`mixed_runtime_demo.cpp`、`managed_pipeline_demo.cpp`、`dynamic_gmp_demo.cpp`
以及 Linux Hook 构建下的 `io_hook_demo.cpp`。其中
`mixed_runtime_demo.cpp` 展示普通线程与 Fiber 的同步、FiberLocal 值、
线程级 Hook 策略和显式 `BlockingRegion`。


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
（`go2cpp::error`、`go2cpp::context`、`go2cpp::channel`、
`go2cpp::scheduler`、`go2cpp::panic_defer`、`go2cpp::fiber`、
`go2cpp::sync`、`go2cpp::io`、`go2cpp::hook`）既可在源码树中单独使用，
也会与 `go2cpp::runtime` umbrella target 一起导出。


新手完整示例是 `example/beginner_demo.cpp`，构建后运行：

```sh
cmake --build build --target go2cpp_beginner_demo
./build/go2cpp_beginner_demo
```
