# 模块目录与依赖边界

项目保留 `go2cpp` 命名空间和原有根路径头文件，源码按职责拆分：

- `include/go2cpp/logging`、`src/logging`：日志格式化、过滤和输出。
- `include/go2cpp/config`、`src/config`：配置值、INI 快照和动态监听。
- `include/go2cpp/concurrency`、`src/concurrency`：Scheduler、Fiber、Context、Channel、同步原语、Future 和 Timer。
- `include/go2cpp/tools`、`src/tools`：错误、控制流和通用入口。
- `include/go2cpp/network`、`src/network`：既有 IOManager/Hook 加上 Address、ByteArray、FdManager、Socket、Framer 和 HTTP。

网络 target 为 `go2cpp_network`，依赖 `go2cpp_io`、`go2cpp_context` 和 `go2cpp_scheduler`；它不反向依赖 HTTP 业务代码。`Socket` 通过 `IOManager::current()` 选择 Fiber 等待，否则使用 poll。地址、字节缓冲和 Framer 不依赖调度器，可在线程环境单独使用。根路径转发头仅负责兼容旧 include，不重复定义类。

测试和示例分别按相同模块放入 `tests/{tools,concurrency,logging,network}` 与 `example/{tools,concurrency,logging,network}`。CMake 保持显式文件列表，便于审计和交叉编译，不使用 glob 隐藏新增源文件。
