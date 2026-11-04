# user/network —— socket / epoll IO / Hook / HTTP / WebSocket / SSE

> 网络栈的人手示例：从「一个 fd 怎么包装」到「HTTP 服务端、WebSocket 握手、
> SSE 推送」的完整链路，每一层都有可运行的最小示例。

## 这个模块是什么

本模块是 Go `net` 包在 C++ 里的对应物，解决三件事：

1. **fd 的 RAII 与地址抽象**：`net::Socket`（TCP/UDP/Unix）、`net::Address`、
   `net::ByteArray`（协议字节缓冲）、`net::FdManager`（fd 元数据）；
2. **阻塞式写法 + 非阻塞底层**：`io::IOManager`（Linux epoll）+ `net::hook`
   （拦截 `recv`/`send`/`connect`/`sleep`，EAGAIN 时让出 Fiber）；
3. **应用层协议**：TCP 分帧、HTTP/1.1（消息/解析器/客户端/服务端/路由/静态文件）、
   WebSocket（RFC 6455）、SSE、以及 HTTP/2 h2c 与 QUIC 的互通示例。

## 底层架构

```
   业务代码（阻塞式 recv/send）
            │
            ▼
   ┌──────────────────┐   EAGAIN    ┌─────────────────────────┐
   │ net::Socket      │ ──────────► │ net::hook（LD_PRELOAD）  │
   │ (RAII 拥有 fd)    │             │ 截获系统调用，转协作等待 │
   └────────┬─────────┘             └───────────┬─────────────┘
            │ 正常返回                          │ 注册 fd
            ▼                                   ▼
   ┌────────────────────────────────────────────────────────┐
   │ io::IOManager：epoll_wait 循环 + Fiber park/wake        │
   │  - WaitFor(fd, event, timeout, ctx)  单个 fd            │
   │  - WaitManyFor(requests, timeout)    多个 fd 一次就绪    │
   │  - Go(fn)                            在当前调度器上跑   │
   └────────────────────────────────────────────────────────┘
            │ 应用层
            ▼
   framing ─► http（Parser/Client/Server/Router/StaticFiles）
            ├─► websocket（握手 + 帧 + 分片）
            └─► sse（事件编码 + 增量解析 + 流式写入）
```

**线程模型**：`IOManager` 内部有 epoll 线程与 Scheduler；等待只能在
`IOManager` 管理的 Fiber 里做 —— 普通线程请用系统 `poll/select`，或把等待
封装成 Fiber 后用 Channel 把结果转交给普通线程。

**Hook 的意义**：让「阻塞式 C 代码」在 Fiber 中变成协作式等待。它不是万能的：
未被拦截的调用（文件 IO、第三方库内部阻塞）仍会卡住 M，需要用
`BlockingRegion` 显式声明。

**协议层不拥有连接**：`http::Parser`、`websocket::WebSocket`、`sse::Stream`
都是纯计算或只操作传入的 `Socket`，因此同一个协议实现既能跑在服务端，也能
跑在客户端/测试的 `socketpair` 上。

## 最小用法

HTTP 消息与解析器是**纯计算**（无网络），适合先理解协议层：

```cpp
#include "go2cpp/http.hpp"
#include <iostream>

int main() {
    using namespace go2cpp::http;

    Request req;
    req.method = Method::Post;
    req.target = "/api/items?id=7";
    req.header.set("Content-Type", "application/json");
    req.body = R"({"name":"box"})";

    Parser parser;              // 有状态：可反复 feed 半包
    Request parsed;
    const auto result = parser.feed(req.to_string(), parsed);
    // result.state == ParseState::Complete

    Response resp;
    resp.status = Status::Created;
    resp.body = "created";
    std::cout << resp.to_string();
    return result.state == ParseState::Complete ? 0 : 1;
}
```

带网络的最小闭环（Fiber 中的 epoll 等待）：

```cpp
go2cpp::SchedulerConfig config;
config.processor_count = 2;
go2cpp::IOManager manager(config);
manager.Start();
auto task = manager.Go([&] {
    const auto r = manager.WaitFor(fd, go2cpp::IOEvent::Read, 1500ms, ctx);
    if (r.ready()) { /* 此时 recv 不会阻塞 */ }
});
task->wait_for(3s);
```

完整版见 `io_wait_many_demo.cpp`（多 fd、超时、取消、Select 桥接）。

## 示例列表

全部注册进 ctest（`io_hook_*` 需要 `go2cpp_hook` 目标存在），目标名 = `go2cpp_<文件名>`：

```sh
cmake --build /tmp/build-user --target go2cpp_http_demo -j && /tmp/build-user/go2cpp_http_demo
```

| 示例 | 演示什么 |
| --- | --- |
| `network_demo` | 分帧（粘包/半包/超长帧）+ HTTP 增量解析的五场景闭环 |
| `io_wait_many_demo` | WaitMany 多 fd、socket→Channel 桥接、DefaultCase、Context 取消 |
| `io_hook_demo` / `io_hook_full` | Hook 开关与作用域、Fiber 里原生 `recv/send`、`SO_RCVTIMEO→ETIMEDOUT` |
| `io_backend_demo` / `io_backend_basic_demo` / `io_backend_full` | IO 后端注册/创建/协议调用序列、默认后端名 |
| `http_demo` / `http_full` | 请求响应往返、头大小写不敏感、多值头、chunked、8 个解析场景 |
| `websocket_demo` / `websocket_full` | socketpair 上的握手 + 文本 echo；握手校验/分片/Ping-Pong/关闭 |
| `sse_basic_demo` / `sse_demo` / `sse_full` | 事件编码与增量解析、心跳、Context 取消、7 个解析边界 |
| `address_bytearray_demo` / `address_full` | IPv4 解析、子网计算、Unix 地址工厂、6 个地址场景 |
| `bytearray_full` | 大端/小端定长整数、长度前缀字符串、6 个编解码场景 |
| `fd_manager_demo` / `fd_manager_full` | fd 元数据自动创建/非阻塞标志/超时/erase 语义 |
| `http2_h2c_server` / `http2_h2c_client` | 明文 HTTP/2（h2c）互通，验证握手与帧（**不注册 ctest**） |
| `picoquic_hq_client` | 通过薄封装连本地 picoquic server，hq-interop 端到端（**不注册 ctest**） |

后三个是「互通验证程序」，需要外部工具（curl / picoquic）配合，因此只构建、
不注册 ctest；运行方式见各自文件头注释与 `docs/quic.md`。

## 源码位置

| 能力 | 头文件 | 实现 |
| --- | --- | --- |
| Socket（RAII） | `include/go2cpp/network/socket.hpp` | `src/network/socket.cpp` |
| 地址抽象 | `include/go2cpp/network/address.hpp` | `src/network/address.cpp` |
| 协议字节缓冲 | `include/go2cpp/network/byte_array.hpp` | `src/network/byte_array.cpp` |
| fd 元数据 | `include/go2cpp/network/fd_manager.hpp` | `src/network/fd_manager.cpp` |
| epoll / IOManager | `include/go2cpp/network/io.hpp` | `src/network/io.cpp` |
| IO 后端注册 | `include/go2cpp/network/io_backend.hpp` | `src/network/io_backend.cpp` |
| Hook | `include/go2cpp/network/hook.hpp` | `src/network/hook.cpp` |
| TCP 分帧 | `include/go2cpp/network/framing.hpp` | （header-only 主体） |
| HTTP/1.1 | `include/go2cpp/network/http.hpp` | `src/network/http.cpp` |
| WebSocket | `include/go2cpp/network/websocket.hpp` | `src/network/websocket.cpp` |
| SSE | `include/go2cpp/network/sse.hpp` | `src/network/sse.cpp` |
| HTTP/2 | `include/go2cpp/network/http2/` | `src/network/http2/` |
| HTTP/3 / QUIC | `include/go2cpp/network/http3/`、`quic/` | `src/network/http3/`、`quic/` |

协议细节与状态机见 `docs/network.md`、`docs/sse.md`、
`docs/modules/ws.md`、`docs/modules/http1.md`、`docs/modules/http2.md`、`docs/quic.md`。

## 注意事项

- **`WaitFor`/`WaitManyFor` 只能在当前 IOManager 管理的 Fiber 里调用**；
  普通线程用系统 `poll/select`。
- **`WaitManyFor` 不是「等全部完成」的屏障**：它返回一次就绪的索引集合；
  想让 epoll 观察到全部 fd，先等所有等待节点注册完再写数据（示例里有这个竞态）。
- **`Parser` 有状态**：一次 `feed()` 可以只喂半包，必须持续喂到 `Complete`；
  收到错误后要 `reset()` 或重建，不要复用。
- **分帧一定要设 `max_frame`**：长度前缀模式先校验上限再等数据，否则一个
  恶意长度就能让进程申请几个 GB。
- **Hook 只在 `LD_PRELOAD`（或链接 `go2cpp_hook`）时生效**：Hook 依赖 PLT 拦截，
  静态内联/直接调用会绕过它 —— 这也是 `go2cpp_hook` 关闭 LTO 的原因。
  关闭 Hook 的进程里，`Socket` 会退化为普通阻塞 socket。
- **服务端示例监听固定端口**（8080/8081/9100/9200 等），重复运行前先确认端口空闲。
