# 模块目录与依赖边界

项目保留 `go2cpp` 命名空间和原有根路径头文件，源码按职责拆分。本文件是模块索引；
各模块的语义、兼容性边界与扩展契约分别见 `docs/design.md`、`docs/compatibility.md`
和 `docs/extensibility.md`。

## 1. 源码目录与 CMake target

| 目录 | 职责 | target |
| --- | --- | --- |
| `src/tools`、`include/go2cpp/tools` | 错误链、显式控制流（defer/panic/recover）、`SelectLoop`/`EventBatch`、默认调度器入口 | `go2cpp::error`、`go2cpp::control_flow` |
| `src/concurrency`、`include/go2cpp/concurrency` | Scheduler（G/M/P）、Fiber 与栈、FiberLocal、Context、Channel、同步原语、Future、Timer | `go2cpp::scheduler`、`go2cpp::fiber`、`go2cpp::context`、`go2cpp::channel`、`go2cpp::sync`、`go2cpp::future` |
| `src/network`、`include/go2cpp/network` | IOManager 与 epoll 后端、FD 管理、Socket、Address、ByteArray、Framer、HTTP、SSE、WebSocket、Hook | `go2cpp::io`、`go2cpp::network`、`go2cpp::hook`（仅 Linux 共享库构建） |
| `src/logging`、`include/go2cpp/logging` | 日志等级、Formatter/Item、Sink、Logger 父级传播 | `go2cpp::log` |
| `src/config`、`include/go2cpp/config` | `ConfigVar<T>`、INI 快照、变更监听与热加载事务 | `go2cpp::config` |
| 仅头文件 | 聚合 umbrella，导出全部模块 target 与 `include/` 路径 | `go2cpp::runtime`（INTERFACE） |

共计 13 个模块 target 加 1 个 `go2cpp::runtime`；完整别名清单见
`cmake/Go2CppModules.cmake` 的 `add_library(... ALIAS ...)` 段。转译代码只需包含
`go2cpp/runtime.hpp`。

## 2. 头文件布局

- `include/go2cpp/*.hpp`（部分模块另有同名 `.h`）：根路径**转发头**，仅为兼容旧
  include 而保留，不重复定义类。
- `include/go2cpp/{concurrency,tools,network,logging,config,scheduler}/`：模块真实定义。
- `include/go2cpp/core/`：`hybrid_mutex.hpp`、`parking_condition.hpp`、`status.hpp`、
  `timer.hpp`。供同步与 IO 复用的核心设施，**不属于稳定公共 API**。
- `include/go2cpp/detail/`：`context_backend.hpp`，Fiber 上下文后端声明
  （Linux x86_64 为 Go2Cpp 自有 ABI，其他平台回退 Boost.Context），属实现细节。

## 3. 依赖边界

`cmake/Go2CppModules.cmake` 中的 `target_link_libraries` 依赖如下：

| target | PUBLIC 依赖 |
| --- | --- |
| `go2cpp_scheduler` | `go2cpp_fiber`、`go2cpp_log` |
| `go2cpp_context` | `go2cpp_error`、`go2cpp_scheduler` |
| `go2cpp_channel` | `go2cpp_context`、`go2cpp_error` |
| `go2cpp_sync` | `go2cpp_context`、`go2cpp_scheduler` |
| `go2cpp_future` | `go2cpp_context`、`go2cpp_scheduler` |
| `go2cpp_io` | `go2cpp_context`、`go2cpp_scheduler` |
| `go2cpp_network` | `go2cpp_io`、`go2cpp_context`、`go2cpp_scheduler`、`go2cpp_sync` |
| `go2cpp_config` | `go2cpp_scheduler`、`go2cpp_log` |
| `go2cpp_hook` | `go2cpp_io`、`go2cpp_fiber`、`${CMAKE_DL_LIBS}` |
| `go2cpp_error`、`go2cpp_control_flow`、`go2cpp_log`、`go2cpp_fiber` | 仅 `Threads::Threads`；`go2cpp_fiber` 在非 native 上下文平台额外链接 `Boost::context` |

`go2cpp_network` 不再被 HTTP 业务代码反向依赖。`Socket` 通过 `IOManager::current()`
选择 Fiber 等待，否则使用 poll。地址、字节缓冲和 Framer 不依赖调度器，可在线程
环境单独使用。

## 4. 测试与示例布局

测试和示例分别按相同模块放入 `tests/{tools,concurrency,logging,network}` 与
`example/{tools,concurrency,logging,network}`。CMake 保持显式文件列表，便于审计和
交叉编译，不使用 glob 隐藏新增源文件。新增测试与示例必须同时登记到
`cmake/Go2CppTests.cmake`；测试还须在 `tests/test_main.cpp` 注册以支持
`GO2CPP_TEST_FILTER`，示例须登记进 `example/README.md` 的速查表。
