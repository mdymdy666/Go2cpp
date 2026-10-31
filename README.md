# Go2Cpp 运行时语义兼容层

本仓库提供一个可独立使用的 C++17 库，实现文档明确的 Go 运行时语义子集。
它是运行时/库，不是 Go 解析器，也不是 Go 到 C++ 的源码转译器。

仓库同时内置三块可选能力（默认 OFF，按需打开）：MCP 框架 `mcp/`（`-DGO2CPP_WITH_MCP=ON`）、
统一命令行 `tools/cli`（`-DGO2CPP_WITH_CLI=ON`）、主机监控与 eBPF 采集（`bpf/` + `go2cpp::os_monitor`，
`-DGO2CPP_ENABLE_EBPF=ON`，默认开；eBPF 不可用时自动降级为 `/proc` 采集）。

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

性能基线与优化记录见 [docs/performance-baseline-2026-10-19.md](docs/performance-baseline-2026-10-19.md)：
包含公平对比方法（排除 warmup、随机负载、同构 epoll 模型）、三轮优化的
根因/改动/前后数据，以及与 coost 的三框架对比。

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

可运行示例位于 `example/`，按模块子目录维护；完整清单、场景说明与运行方式见
[`example/README.md`](example/README.md)：

- `example/concurrency/`：`beginner_demo.cpp`、`runtime_demo.cpp`、
  `fiber_sync_demo.cpp`、`managed_pipeline_demo.cpp`、`dynamic_gmp_demo.cpp`、
  `mixed_runtime_demo.cpp`、`context_rollback_demo.cpp`、`timer_task_demo.cpp`；
- `example/network/`：`network_demo.cpp`、`io_wait_many_demo.cpp`、
  `io_hook_demo.cpp`（仅 Linux Hook 构建）、`sse_demo.cpp`、`websocket_demo.cpp`；
- `example/logging/log_config_demo.cpp`、`example/tools/control_flow_demo.cpp`。

其中 `mixed_runtime_demo.cpp` 展示普通线程与 Fiber 的同步、FiberLocal 值、
线程级 Hook 策略和显式 `BlockingRegion`。日志和配置的完整新手示例是
`example/logging/log_config_demo.cpp`，工程配置模板是根目录的 `go2cpp.ini`，
详细说明见 [`docs/logging.md`](docs/logging.md) 和
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

转译代码需要完整公共接口时，包含 `go2cpp/runtime.hpp`。模块 target（`go2cpp::error`、
`go2cpp::control_flow`、`go2cpp::context`、`go2cpp::channel`、`go2cpp::scheduler`、
`go2cpp::fiber`、`go2cpp::sync`、`go2cpp::future`、`go2cpp::io`、`go2cpp::network`、
`go2cpp::log`、`go2cpp::config`，以及 Hook 构建下的 `go2cpp::hook`）既可在源码树中
单独使用，也会与 `go2cpp::runtime` umbrella target 一起导出。


新手完整示例是 `example/concurrency/beginner_demo.cpp`，构建后运行：

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

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `src/` | 运行时实现，按模块分为 `concurrency/`、`network/`、`os/`、`logging/`、`config/`、`debug/`、`tools/`、`bufio/` 等 |
| `include/go2cpp/` | 公共头文件；`include/go2cpp/*.hpp` 是兼容转发头，真实定义在模块子目录 |
| `mcp/` | 内置 MCP 框架：`protocol/`、`server/`、`transport/`、`tools/`、`llm/`、`plugin/`、`vector/`、`retrieval/`、`app/` |
| `bpf/` | 6 个 eBPF C 程序（sched/futex/syscall/net/tcp/fileio），由 `cmake/embed_bpf.cmake` 编译并嵌入 |
| `tests/` | 测试源码，按 `tools/`、`concurrency/`、`network/`、`os/`、`debug/`、`bufio/` 等模块划分 |
| `tools/` | 独立工具：`cli/`、`migration/`、`replay/`、`persistence/` |
| `cmake/` | 8 个构建模块：选项、模块 target、测试、安装、迁移、监控、eBPF 嵌入、运行时配置 |
| `docs/` | 设计与使用文档，入口见 [`docs/INDEX.md`](docs/INDEX.md) |
| `example/` | 按模块分类的可运行示例，清单见 [`example/README.md`](example/README.md) |
| `benchmarks/` | 独立性能基准（需 `GO2CPP_BUILD_BENCHMARKS=ON`） |
| `third_party/` | 第三方与 Go 参考源码（大部分归档被 `.gitignore` 排除） |
| `build-*/`、`Testing/` | 本地构建与 CTest 产物，均被 `.gitignore` 忽略，不入库 |

推荐在源码树外构建（例如 `/UserData/build/go2cpp-mcp-integrated`），避免在仓库内产生
`build-*/`；仓库内构建同样被忽略。

## CMake 选项

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `ON` | 构建共享模块（`.so`）；静态构建需同时关闭 Hook |
| `GO2CPP_BUILD_HOOK` | `ON` | Linux 系统调用 Hook（透明 park Fiber），依赖共享库 |
| `GO2CPP_BUILD_TESTS` | `ON` | 构建运行时测试并注册 CTest |
| `GO2CPP_BUILD_EXAMPLES` | `ON` | 构建 `example/` 示例 |
| `GO2CPP_BUILD_BENCHMARKS` | `OFF` | 构建独立性能基准 |
| `GO2CPP_ENABLE_TSAN` | `OFF` | 支持时启用 ThreadSanitizer |
| `GO2CPP_BUILD_ENHANCED` | `OFF` | 一键打开 DEBUG_DUMP + DIAGNOSTIC + REPLAY + MIGRATION_API（未显式指定时也打开 CLI） |
| `GO2CPP_WITH_MCP` | `OFF` | 构建内置 MCP 框架与 `mcp_server` |
| `GO2CPP_WITH_CLI` | `OFF` | 构建 `go2cpp-cli` 统一命令行 |
| `GO2CPP_BUILD_OS_MONITOR_TESTS` | `ON` | 构建 `go2cpp::os::monitor` 测试并并入 CTest |
| `GO2CPP_ENABLE_EBPF` | `ON` | 编译并嵌入 eBPF 程序；缺少 clang/libbpf/libelf/libz 时自动降级 |
| `GO2CPP_DEBUG_DUMP` | `OFF` | 事件环 + DebugServer（Unix socket），默认零开销 |
| `GO2CPP_REPLAY` | `OFF` | 资源事件回放录制（G2CRPLY） |
| `GO2CPP_DIAGNOSTIC` | `OFF` | 在线诊断 Doctor（命令平面/等待图/受控唤醒） |
| `GO2CPP_DIAGNOSTIC_QUIC` | `OFF` | Doctor 的 QUIC 承载，需同时打开 DIAGNOSTIC 与 PICOQUIC |
| `GO2CPP_MIGRATION_API` | `OFF` | 跨进程 Fiber/Channel 迁移实验 API |
| `GO2CPP_WITH_PICOQUIC` | `OFF` | picoquic 承载的 QUIC/HTTP3 transport |
| `GO2CPP_DEBUG_EVENT_CAPACITY` | `256` | 每线程普通事件环容量（仅 DEBUG_DUMP=ON） |
| `GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY` | `64` | 每线程关键事件环容量（仅 DEBUG_DUMP=ON） |
| `MCP_BUILD_EXAMPLES` | `ON` | MCP 示例（仅在 `GO2CPP_WITH_MCP=ON` 时生效） |

MCP 集成构建示例：

```sh
cmake -S . -B /UserData/build/go2cpp-mcp-integrated -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_WITH_MCP=ON -DGO2CPP_BUILD_TESTS=OFF
cmake --build /UserData/build/go2cpp-mcp-integrated -j
```

该配置只注册监控与 bufio 的 14 个 CTest 用例（`GO2CPP_BUILD_OS_MONITOR_TESTS=ON`）；
完整运行时测试需 `-DGO2CPP_BUILD_TESTS=ON`。

## 文档导航

文档总入口是 [`docs/INDEX.md`](docs/INDEX.md)（全量文档的分类、状态与重复/过时/孤立清单）。常用入口：

- 入门：[getting_started.md](docs/getting_started.md)、[compatibility.md](docs/compatibility.md)、[design.md](docs/design.md)；
- 核心设计：[scheduler.md](docs/scheduler.md)、[modules.md](docs/modules.md)、[fiber-migration.md](docs/fiber-migration.md)；
- 使用：[configuration.md](docs/configuration.md)、[logging.md](docs/logging.md)、[fiber_guide.md](docs/fiber_guide.md)、[network.md](docs/network.md)、[cli.md](docs/cli.md)；
- 诊断：[diagnostic-channel.md](docs/diagnostic-channel.md)、[debug-reliability.md](docs/debug-reliability.md)；
- 测试与性能：[testing.md](docs/testing.md)、[performance-baseline-2026-10-19.md](docs/performance-baseline-2026-10-19.md)；
- MCP：[mcp/docs/architecture.md](mcp/docs/architecture.md)；
- 历史：[AGENT-HANDOFF.md](docs/AGENT-HANDOFF.md)、[NEXT-STEPS.md](docs/NEXT-STEPS.md)（仅追溯，数字不代表现状）。

## 测试

ctest 数量随构建配置变化，**以 `ctest -N` 为准**（逐条清单见 [testing.md](docs/testing.md)「测试地图」）：

| 构建配置 | profile | ctest 总数 |
| --- | --- | --- |
| Bento + MCP（`-DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MCP=ON`，`.build-bento-wsl`） | `bento+mcp` | **85**（60 bento + 3 bufio + 1 reflect_codec + 21 MCP 框架） |
| MCP 集成（`-DGO2CPP_WITH_MCP=ON -DGO2CPP_BUILD_OS_MONITOR_TESTS=ON`） | `mcp+osmon` | **35**（12 os/monitor + 2 bufio + 21 MCP 框架） |

Bento + MCP 构建实测 **Total Tests: 85**、**85/85 passed**；MCP 集成构建实测 **35/35 passed**（root 用例无权限时记 SKIP）。

```sh
ctest --test-dir .build-bento-wsl --output-on-failure
```

MCP 内置工具共 **33 个**（22 内置 + 3 检索 + 4 监控 + 4 Socket = 33 个工具）。
