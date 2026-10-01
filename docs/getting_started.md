# Go2Cpp 新手使用指南

这份文档给第一次接触 Go2Cpp 的 C++ 开发者使用。Go2Cpp 是一个 C++17
并发运行时库，提供受 Go 启发的 Fiber、G/M/P 调度、Context、Channel、
同步原语和 Linux IO 等能力。它不是 Go 语法解析器，也不会把 C++ 源码自动
翻译成 Go。

## 1. 环境和构建

推荐在 Linux 或 WSL2 中构建。需要：

- CMake 3.20 或更高版本；
- 支持 C++17 的 GCC 或 Clang；
- 共享库构建时，Linux Hook 需要 `dl`、`pthread` 和 epoll；
- x86-64 原生 Fiber 上下文使用项目内的汇编后端。其他架构应关闭 Hook，
  并先确认对应的上下文后端。

第一次构建建议使用独立的 `build` 目录，避免把构建产物混入源码：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

调试运行可以把 `Release` 改成 `Debug`。需要静态库时关闭 Hook，因为 Hook
需要所有模块共用同一份线程局部状态和 FD 注册表：

```sh
cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_SHARED_LIBS=OFF -DGO2CPP_BUILD_HOOK=OFF
cmake --build build-static --parallel
```

常用 CMake 选项如下：

| 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `GO2CPP_BUILD_TESTS` | `ON` | 构建单元测试、压力测试和 CTest 注册项 |
| `GO2CPP_BUILD_EXAMPLES` | `ON` | 构建 `example/` 下的可运行示例 |
| `GO2CPP_BUILD_HOOK` | Linux 下 `ON` | 构建 socket/poll/epoll 等阻塞调用 Hook |
| `GO2CPP_ENABLE_TSAN` | `OFF` | 在工具链支持时启用 ThreadSanitizer |

## 2. 最短可运行程序

包含 `go2cpp/runtime.hpp` 可以一次取得公共接口。程序必须保证 Scheduler
活着时任务仍然可以访问它，并在提交任务后等待或显式关闭：

```cpp
#include "go2cpp/runtime.hpp"

#include <iostream>

int main() {
    go2cpp::Scheduler scheduler(2);
    scheduler.start();

    auto task = go2cpp::go(scheduler, [] {
        std::cout << "Fiber 正在运行\n";
    });
    task->wait();

    scheduler.shutdown();
    return 0;
}
```

如果不需要自行管理 Scheduler，可以使用进程级默认调度器：

```cpp
auto task = go2cpp::go([] { /* Fiber 工作 */ });
task->wait();
go2cpp::shutdown_default_scheduler();
```

默认调度器是惰性创建的。调用 `shutdown_default_scheduler()` 后，下一次
`go()` 会创建新的默认 Scheduler；不要继续向已经关闭的 Scheduler 提交任务。

`go()` 返回 `std::shared_ptr<Task>`。Task 是一次提交的 G 任务句柄，可用
`wait()`、`wait_for()`、`cancel()`、`state()` 和 `failure()` 查看状态。任务
函数返回后，Fiber 才会进入完成状态；不要在任务尚未完成时销毁它仍在使用的
外部对象。

## 3. 调度器和 Fiber 生命周期

一个 Scheduler 包含受限数量的 P、运行它们的 M（OS 线程）以及可迁移的 G
（Task/Fiber）。P 负责本地队列，M 负责执行，G 在显式 park、yield、Channel、
Context 或 IO 等边界挂起。

使用时遵守以下顺序：

1. 构造 Scheduler；
2. `start()` 或直接调用 `go(scheduler, callback)`；
3. 提交任务；
4. 等待任务完成或取消；
5. 最后调用 `shutdown()`。

`shutdown()` 是终态操作。它会停止接收新任务、唤醒等待者并排空已接受任务。
不要让 Scheduler 在仍有任务运行或 IO 等待时离开作用域，也不要并发调用多个
关闭操作。Scheduler、IOManager、Context、Channel 和同步对象都必须长于所有
可能访问它们的任务和等待者。

可以先构造轻量 `go2cpp::fiber`，再绑定到 Scheduler：

```cpp
go2cpp::fiber job([] { /* Fiber 工作 */ });
auto task = job.bind(scheduler);
if (task) {
    task->wait();
}
```

Fiber 之间可以嵌套，但父 Fiber 必须在子 Fiber 完成或取消前保持存活。不要把
栈上的 Fiber、Mutex、ConditionVariable 或 Channel 交给生命周期更长的任务。

## 4. 同步：Fiber 和普通线程共用一套接口

`go2cpp::sync::Mutex`、`ConditionVariable` 和 `WaitGroup` 同时支持 managed
Fiber 与不参加调度器的普通线程。Fiber 竞争锁时会 park 当前 G，让 M 执行其他
任务；普通线程则使用原生等待路径。

```cpp
go2cpp::sync::Mutex mutex;
go2cpp::sync::WaitGroup group;
group.Add(1);

go2cpp::go(scheduler, [&] {
    std::lock_guard<go2cpp::sync::Mutex> lock(mutex);
    // 与普通线程共享的数据
    group.Done();
});

group.Wait();
```

注意事项：

- 成功 `Lock()` 必须恰好对应一次 `Unlock()`；不支持递归加锁；
- 不要把 `std::mutex`、`std::condition_variable` 或原生阻塞锁跨 Fiber park；
- 不要持有外部锁调用可能 park 的 Channel、Context、IO 或 Scheduler API，避免
  锁反转；
- `BlockingRegion` 只能是短生命周期 RAII 对象，不能跨 yield、park、IO 或任务
  边界。它用于向 sysmon 声明一个无法 Hook 的原生阻塞调用；
- `ConditionVariable::Wait()` 返回前会尝试重新取得传入的 Mutex。超时、取消或
  调度器关闭都应检查返回值。

## 5. Channel、Select 和取消

`MakeChannel<T>(capacity)` 创建类型化 Channel。容量为 `0` 是无缓冲握手，容量
大于 `0` 时使用 FIFO 缓冲。关闭 Channel 使用 `Close()`；关闭后接收返回零值
和关闭状态，发送返回 `kClosed` 或对应错误状态。

```cpp
auto values = go2cpp::MakeChannel<int>(1);
auto sender = go2cpp::go(scheduler, [values] {
    (void)values->Send(42);
    (void)values->Close();
});

auto result = values->Recv();
if (result.status == go2cpp::ChannelStatus::kReady) {
    std::cout << *result.value << '\n';
}
sender->wait();
```

`SelectLoop`/`EventBatch` 用于等待多个 Channel 事件、超时或取消。内建
Channel Select case 要求元素类型可复制；move-only 值请使用普通 `Send/Recv`
或 `SelectValue`/`SelectCaster`，否则 Select 会返回 `kInvalid`，不会静默
永久等待。

Context 用于取消树和截止时间：

```cpp
#include <chrono>

using namespace std::chrono_literals;

auto channel = go2cpp::MakeChannel<int>(0);
auto [ctx, cancel] = go2cpp::WithTimeout({}, 2s);
auto result = channel->Recv(ctx);
if (ctx->IsDone()) {
    auto error = ctx->Err(); // Canceled 或 DeadlineExceeded
}
cancel(); // 幂等；不再需要时主动调用
```

父 Context 取消会传播到子 Context。Context 的值键应使用稳定、类型明确的键，
不要把可变全局对象作为值放入 Context；监听器中不要长时间阻塞。

## 6. Linux IO 和 Hook

`IOManager::WaitAnyFor()`、`WaitManyFor()` 只能从 managed Fiber 调用。它们会
把 FD 注册到 epoll，Fiber park，事件就绪或超时后再恢复。普通线程应使用系统
`poll/select/epoll`，或把调用放在显式 `BlockingRegion` 中。

启用 Hook 后，项目覆盖的 socket、poll/select、epoll 和 sleep 路径会自动接入
IOManager。Hook 不会自动发现任意第三方库的阻塞调用；未覆盖的系统调用必须
明确使用非阻塞接口或 `BlockingRegion`。

所有 IO 等待对象、Channel 和回调的生命周期必须覆盖等待过程。关闭 FD 前应
先取消对应等待或确保 IOManager 仍然存活；不要在回调中直接销毁仍被其他事件
引用的对象。

## 7. 配置和动态更新

根目录 `go2cpp.ini` 是完整模板。建议复制一份到程序工作目录，再通过
`LoadAndWatch()` 加载和热更新：

```cpp
go2cpp::config::Config& config = go2cpp::config::Config::Instance();
go2cpp::config::RuntimeConfig runtime;
std::string error;
if (!go2cpp::config::BindRuntimeConfig(config, &runtime, &error) ||
    !config.StartWatcher("go2cpp.ini", std::chrono::milliseconds(500), &error)) {
    std::cerr << "配置错误: " << error << '\n';
    return 1;
}
```

配置值会先统一解析和校验，再发布不可变快照。`processor_count`、P 数量、
队列拓扑和 M 上限等结构参数应在创建 Scheduler 前设置；Scheduler 已启动后
不要在线改变这些参数。日志级别、日志路径等运行时参数可以通过配置监听器
更新。所有数值参数都有范围限制，非法值应停止启动并检查返回的错误字符串。

## 8. 日志和故障定位

默认日志等级为 `warn`，默认写入工作目录下的 `log/go2cpp.log`，不会自动把
日志打到 stdout。需要控制台输出时，在 `[log]` 中设置 `stdout = true`，或在
代码中绑定 Logger 配置。

开发阶段建议按以下顺序定位问题：

1. 先运行 `ctest --test-dir build --output-on-failure`；
2. 再单独运行对应示例，确认生命周期和 API 使用方式；
3. 检查 Scheduler 是否提前销毁、Context/Channel 是否仍被引用；
4. 检查 Fiber 是否调用了未 Hook 的原生阻塞函数；
5. 最后使用 ASan/UBSan；并发问题再单独运行 TSan 或 Valgrind。

## 9. 当前明确限制

- Fiber 栈后端需要遵守项目支持的架构；当前不提供 Go 编译器级分段栈和异步抢占；
- sysmon 只观察 Hook 和 `BlockingRegion` 发布的阻塞边界，不能从其他线程强行
  打断任意 C++ 调用；
- `panic/recover` 是显式状态对象，不会模拟 C++ 栈跳转，也不应替代 RAII；
- 任务函数中的普通 C++ 异常会被 Fiber 边界捕获并记录到 Task 状态，资源释放
  仍依赖正常的 C++ 析构规则；
- 调度器、Context、Channel、IOManager 和同步对象的析构不是并发取消原语，必须
  在所有访问者结束后销毁。

更完整的语义边界、状态机和压力测试结果见：

- [`docs/compatibility.md`](compatibility.md)
- [`docs/correctness_audit.md`](correctness_audit.md)
- [`docs/testing.md`](testing.md)
- [`docs/configuration.md`](configuration.md)
- [`docs/logging.md`](logging.md)
