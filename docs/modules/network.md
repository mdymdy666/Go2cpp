# network 模块（总览）

> 本文是 network 模块的总入口；子模块有专文：
> [http1](http1.md) / [http2](http2.md) / [tls](tls.md) / [ws](ws.md) / [sse](sse.md)。

## 1. 定位

`go2cpp::network` 提供与 Go `net` 包对等的网络栈：地址/监听/连接、epoll IO 管理器、
syscall Hook（协作式阻塞转 park）、HTTP/1、HTTP/2、TLS、WebSocket、SSE、QUIC/HTTP3
薄封装。源码：`include/go2cpp/network/**`、`src/network/**`；构建目标 `go2cpp_network`
（`cmake/Go2CppModules.cmake`）。

## 2. 核心 API

| API | 位置 | 说明 |
|---|---|---|
| `net::Socket::CreateTCP()` / `CreateUDP()` | `include/go2cpp/network/socket.hpp` | 创建套接字 |
| `Socket::bind/Listen/Accept/Connect` | 同上 | 监听/接受/连接（阻塞语义由 Hook 转 park） |
| `IPv4Address::Create / IPv6Address` | `include/go2cpp/network/address.hpp` | 地址解析 |
| `io::IOManager` | `include/go2cpp/io/**` | epoll 事件循环（EPOLLET 重 arm、poller+worker 双驱动），见 [io.md](io.md) |
| `http::Server / http::Client` | `include/go2cpp/network/http.hpp` | HTTP/1，见 [http1.md](http1.md) |
| HTTP/2（HPACK/帧/流状态机） | `include/go2cpp/network/http2/**` | 见 [http2.md](http2.md) |
| `http2::TlsTransport` | `include/go2cpp/network/http2/tls_transport.hpp` | TLS（OpenSSL），见 [tls.md](tls.md) |
| WebSocket | `include/go2cpp/network/websocket*` | 见 [ws.md](ws.md) |
| `go2cpp::sse` | `include/go2cpp/network/sse.hpp` | SSE，独立模块，见 [sse.md](sse.md) |
| QUIC/HTTP3 | `include/go2cpp/network/quic*`、`http3*` | picoquic/picohttp 薄封装，`GO2CPP_WITH_PICOQUIC=ON` 才构建，默认 OFF、无 ctest |

## 3. 设计要点

### 3.1 Hook：22 个 HookOp

`src/network/hook.cpp` 截获 libc 网络/等待调用，把「会阻塞的语义」转成 Fiber park。
当前 **22 个 HookOp**（旧文档写 19）：

- **14 个可关闭**（`SetOpEnabled`）：read/write/accept/connect/recv/send/poll/select/...
- **5 个硬拒绝**（语义不允许协作化，直接失败）；
- **3 个语义空**（hook 后行为与原生一致，仅统计）。

**`shutdown` 从未被 hook**（旧文档误记为已覆盖）。

### 3.2 HTTP/2 核心的构建边界（重要）

`connection.cpp`/`frame.cpp`/`hpack.cpp`/`huffman.cpp`/`stream_state.cpp`
**不在 `go2cpp_network` 导出目标**里，只在 `cmake/Go2CppTests.cmake` 的测试目标中编译。
生产使用 HTTP/2 需自行把这些源文件加入目标（待修：见 [http2.md](http2.md) §6）。

### 3.3 TLS 默认校验

`TlsTransport` 的 `verify_peer` 默认 **true**（证书链 + 主机名）。头文件末段残留
「默认关闭」旧注释，以构造签名/实现为准（`tls_transport.hpp:94/113`）。

## 4. 边界与限制

- Hook 依赖 Linux（`src/network/hook.cpp` 仅在 `GO2CPP_BUILD_HOOK=ON` 且 Linux 下构建）；
- QUIC/HTTP3 无 ctest，仅编译验证；
- `shutdown` 不协作化：对端关闭检测依赖 read 返回 0；
- 部分 HookOp 硬拒绝（如 `futex` 的部分语义）需用调度器原语替代。

## 5. 测试

`cmake/Go2CppTests.cmake` 注册：`network` 相关（socket/address/dial/accept 等）、
HTTP/1（`go2cpp_tests` 内含）、HTTP/2（hpack/frame/stream_state/connection/...）、
TLS（`tls_alpn`）、WS/SSE（独立用例）。HTTP/2 纯协议测试可脱离 runtime 单独编译
运行（见 [http2.md](http2.md) 的实测数据）。

## 6. 相关文档

- 旧 [network.md](../network.md) 已部分合并至本文与子模块文档；
- [io.md](io.md)（epoll/IOManager）、[http1.md](http1.md)、[http2.md](http2.md)、
  [tls.md](tls.md)、[ws.md](ws.md)、[sse.md](sse.md)。
