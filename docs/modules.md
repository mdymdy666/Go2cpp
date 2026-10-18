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
| `src/os`、`include/go2cpp/os` | 文件句柄（`os::File`：Open/Create/Read/Write/Seek/Stat/Sync/Truncate）、目录遍历（`ReadDir`/`MkdirAll`/`RemoveAll`）、符号链接、环境变量、`Args` | `go2cpp::os` |
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
- `include/go2cpp/os/`：`file.hpp`（文件句柄与错误码）、`os.hpp`（环境变量 / `Args` / `Exit`）、`dir.hpp`（目录遍历与符号链接）。
- `include/go2cpp/network/http2/`：HTTP/2 全栈 —— `hpack.hpp`（含 Huffman 码表 `huffman_tables.inc`）、`frame.hpp`（帧层）、`stream_state.hpp`（流状态机）、`connection.hpp`（连接层）、`transport.hpp` / `fiber_transport.hpp` / `tls_transport.hpp`（IO 抽象与三种实现）。
- `include/go2cpp/network/quic/`：picoquic 薄封装 —— `picoquic_connection.hpp` / `src/network/quic/picoquic_connection.cpp`（`GO2CPP_WITH_PICOQUIC`，默认 OFF）。自研 varint/packet/frame 已删除，**不要在任何文档或接口里承诺自研 QUIC**。

- `include/go2cpp/core/`：`parking_condition.hpp`、`status.hpp`、
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
| `go2cpp_os` | `go2cpp_config`（`src/os/config.cpp` 调 `Config::SaveToFile`） |
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

## 附：`os` 模块的接入状态（如实记录）

`os` 模块的代码（`include/go2cpp/os/` + `src/os/` + `tests/os/`）**已实现并有测试覆盖**
（`go2cpp_os_test`、`go2cpp_os_dir_test`），但**尚未在 `cmake/Go2CppModules.cmake`
里定义独立的库 target**：

- 目前 `src/os/*.cpp` 只被 `cmake/Go2CppTests.cmake` 里的测试 target 直接编译；
- 没有 `libgo2cpp_os.so` 产出，`go2cpp::runtime` umbrella 也不导出它；
- 因此**下游若只 include 头文件而不编译源文件，会链接失败**。

要把它变成正式模块，需要在 `Go2CppModules.cmake` 里：
1. `add_library(go2cpp_os src/os/dir.cpp src/os/file.cpp src/os/os.cpp src/os/config.cpp)`
2. 加进 `GO2CPP_MODULE_TARGETS` 列表
3. `add_library(go2cpp::os ALIAS go2cpp_os)`
4. `target_link_libraries(go2cpp_os PUBLIC go2cpp_config)`
5. 把 `go2cpp_os` 加进 `go2cpp::runtime` 的导出

**验证方式**（不要只编译）：写一个只 `#include <go2cpp/os/file.hpp>` 而不编译源文件的
小目标，链接 `-lgo2cpp_os` 能通过，才算真正接入。

`os` 已完成接入：`go2cpp_os` 已在 `GO2CPP_MODULE_TARGETS` 与 `go2cpp::runtime` 中。
