# 网络模块使用说明

> **部分合并：IOManager / epoll 的内部细节已合并至 [docs/modules/io.md](modules/io.md)（2026-10-26）**。
> 本文其余章节（Socket、FdManager、HTTP/WebSocket/SSE 用法）仍是现行说明；
> 凡涉及 IOManager 的线程模型与等待语义，以模块文档为准。


网络模块位于 `include/go2cpp/network/`，分成地址、字节缓冲、文件描述符元数据、Socket/Stream、分帧、HTTP 和 WebSocket 层。旧的 `go2cpp/address.hpp`、`socket.hpp`、`bytearray.hpp`、`http.hpp` 是兼容转发头，命名空间仍为 `go2cpp`，HTTP 详细类型位于 `go2cpp::http`，底层类型位于 `go2cpp::net`。

## 线程、Fiber 和等待

`Socket` 默认设置系统非阻塞标志。遇到 `EAGAIN` 时，如果当前线程属于 `IOManager`，Socket 调用 `IOManager::wait_for` 让出 Fiber；普通线程使用 `poll`。因此不能在 `HybridMutex`、`std::mutex` 或用户回调持锁时调用可能等待的读写。用户可以通过 `set_waiter` 注入其它多路复用后端。

每个读、写、accept、connect、选项查询和地址查询都会先固定当前 fd 并增加 in-flight 计数。`close/release` 先进入 closing 状态、通知 IOManager、等待 in-flight 清零，再删除 FdManager 元数据和关闭/转移 fd；重复 close 是幂等的。受管 Fiber 的等待使用 100ms 片段检查关闭状态，解决“close 发生在 readiness 注册前”时的丢唤醒；普通线程同样不使用无限期不可取消的 `poll`。关闭中的新操作返回 `ECANCELED` 或 `EBADF`。

`Socket::cancel_read()` 和 `cancel_write()` 使用独立的取消代际。调用线程即使没有
`IOManager` 也能取消另一个线程/Fiber 的读写等待；等待后端最多经过一个有限轮询片段就会
返回 `ECANCELED`。取消只终止当前等待，不转移 Socket 所有权；SSE Context 回调利用这条
路径打断慢消费者，回调本身不持有业务锁。

fd 元数据由 `FdManager` 维护，记录用户非阻塞、系统非阻塞和收发超时；它不拥有 fd。Socket 析构会先通知 IOManager，再删除元数据，避免 close 后数字 fd 复用唤醒旧等待。`Socket::send` 使用 `MSG_NOSIGNAL`，不会改变进程级 SIGPIPE 处理。

## TCP 分帧

TCP 是有序字节流，不保证一次 `recv` 对应一次消息。使用 `Framer::delimiter`、`Framer::fixed` 或 `Framer::big_endian32`，并设置合理的最大帧长度。`feed` 可以接收半帧和多帧，`take_frames` 返回所有完整帧；超限抛出 `length_error`，调用方应将其转换为连接级协议错误。

## ByteArray

`ByteArray` 单线程单所有者使用；读写游标不能被多个线程共同修改。定长整数默认小端，网络协议应调用 `set_little_endian(false)`。变长整数严格拒绝超过 32/64 位的编码，长度字符串超过 64 MiB 或读越界会抛出异常，使用前应在协议边界捕获异常。

## HTTP

`http::Parser` 增量解析 HTTP/1.x，支持分片头、Content-Length、严格的单一 `chunked`、chunk 扩展、trailer 和流水线剩余字节；默认头上限 64 KiB、body 上限 64 MiB、trailer 上限 16 KiB，可通过 `ParserLimits` 调整。重复但不一致的 Content-Length、Content-Length 与 chunked 同时出现、未知 Transfer-Encoding、非法头、非法 chunk 和超限输入会返回 `ParseState::Error`。1xx、204、304 以及通过 `set_response_to_head(true)` 标记的 HEAD 响应不会等待 body。模块**不实现 HTTP/3**、WebSocket upgrade 和 Expect:100；需要这些协议时应注入独立 ParserBackend。**HTTP/2 已由独立子模块 `go2cpp::net::http2` 提供**（不复用本 Parser，见下节 `## HTTP/2`）。

`http::ResponseWriter` 提供有限状态的 HTTP/1.1 chunked 输出：`Idle -> start() -> write()/flush() -> close()`（其中 `flush()` 只做状态校验、不写出内容，因为 `write()` 已按 chunk 直接写入 Socket、全程无缓冲）。它适合 SSE、大文件和业务流式响应，所有 Socket 短写都会由 `SocketStream::write_all` 处理；重复 start、close 后 write、未 start 的 write 都返回失败。一个 writer 只能由一个线程或 Fiber 持有，业务需要并发发送时必须在上层串行化。

## WebSocket

WebSocket 位于独立的 `go2cpp::net::WebSocket`，握手和帧状态机详见 [websocket.md](websocket.md)。HTTP Parser 只负责读取 Upgrade 请求；服务端必须把 `take_remaining()` 的剩余字节传给 `AcceptWithPending`。当前支持 RFC 6455 基础握手、掩码、分片、Ping/Pong、Close、UTF-8 和大小限制，不隐式启用 TLS、扩展或 HTTP/2。

## SSE

SSE 位于独立的 `go2cpp::sse`，完整用法详见 [sse.md](sse.md)。`sse::Stream` 复用
`ResponseWriter` 输出 chunked 的 `text/event-stream`，可以在线程或 IOManager Fiber 中使用；
`sse::Parser` 按字节流增量解析多行 data、BOM、注释、retry 和 Last-Event-ID。Stream 不建立
无界队列，慢消费者由 Socket 写超时或错误反馈给业务；`StreamOptions::max_event_bytes` 必须
结合业务消息大小设置。心跳由 `tick()` 显式调用，适合交给 TimerService 调度；传入 Context
后取消会唤醒等待写入并使后续操作返回 Cancelled/DeadlineExceeded。SSE 本身不提供跨连接事件
存储和自动重连，重连时由业务把 `Parser::last_event_id()` 设置到 HTTP 的 Last-Event-ID。

客户端提供 `http::Client::get/post/head` 以及 `http::Get/Post/Head` 便捷函数；每个 Client 请求独占连接，超时覆盖 connect、send、recv，并按单个请求的绝对 deadline 计算剩余时间，慢速逐字节响应不会为每次 recv 重新获得完整超时。DNS 目前通过 `getaddrinfo` 同步完成：在 Fiber 中会占用当前 M/worker，deadline 不能中断内核解析；高并发或需要取消的场景应预解析地址，或注入独立的有界异步 Resolver/解析线程池，避免在持有业务锁时解析。服务端 `http::Server::serve_connection` 对单连接串行处理，路由回调在锁外执行；**它不对连接的 recv/write 施加超时**（与上面的 Client 不同，使用无限等待），连接级超时需由调用方在 handler 内或上层负责；生产程序应让 accept、IOManager 和业务 worker 分层，并在 handler 中避免持有线程锁等待网络。

## HTTP/2

HTTP/2 是**独立子模块** `go2cpp::net::http2`（`include/go2cpp/network/http2/` +
`src/network/http2/`，共 3637 行），**不复用** `http::Parser` —— 二进制帧与 HPACK
压缩和 HTTP/1.x 的文本解析没有可复用的部分。

### 分层（从下往上）

| 层 | 文件 | 职责 |
|---|---|---|
Huffman | `huffman.hpp/.cpp` + `huffman_tables.inc` | RFC 7541 附录 B 的 257 项码表，头部的值压缩 |
HPACK | `hpack.hpp/.cpp` | 静态表 61 项、动态表（FIFO + 大小上限）、整数前缀编码、三种字面量表示 |
帧层 | `frame.hpp/.cpp` | 10 种帧的解析与构造；帧长、stream id、固定长度的校验都在这一层 |
流状态机 | `stream_state.hpp/.cpp` | RFC 9113 §5.1 的 7 个状态与非法转换拒绝 |
连接层 | `connection.hpp/.cpp` | 握手、流表、CONTINUATION 分片、流控窗口 |
IO 抽象 | `transport.hpp` | `Read` / `Write` / `Closed` 三个方法，**协议逻辑只认它** |
真实 IO | `fiber_transport.hpp` / `tls_transport.hpp` | 前者走 `Socket`（Fiber 挂起），后者走 OpenSSL（TLS 1.2+ / SNI / ALPN=h2） |

### 为什么有 `Transport` 抽象

连接层里的协议逻辑**不知道字节从哪来**，因此三种实现可以互换：

- `MemoryTransport` —— 双向内存缓冲，**单线程就能跑完整闭环**，不需要 socket、不需要 Fiber；
- `FiberTransport` —— 真实 socket，`recv`/`send` 经 hook 让出 Fiber；
- `TlsTransport` —— TLS 之上的同一套逻辑。

这是本模块能"先测协议、再接网络"的关键。**并发只体现在 Transport 的实现里。**

### 用法（同步阻塞写法）

```cpp
go2cpp::net::http2::FiberTransport transport(socket, 10s);
go2cpp::net::http2::Connection connection(&transport, go2cpp::net::http2::Role::kClient);
std::string error;
connection.Handshake(&error);                       // 阻塞到 SETTINGS 交换完成

uint32_t stream_id = 0;
connection.StartRequest({{":method", "GET"},
                         {":scheme", "https"},
                         {":path", "/"},
                         {":authority", "example.com"}},
                        /*end_stream=*/true, &stream_id, &error);

go2cpp::net::http2::IncomingEvent event;
while (connection.ReadEvent(&event, &error) == ConnStatus::kOk) {
    if (event.kind == IncomingEvent::Kind::kHeaders) { /* :status ... */ }
    if (event.kind == IncomingEvent::Kind::kData)    { /* event.data */ }
}
```

**TLS + ALPN**：把 `FiberTransport` 换成 `TlsTransport::CreateClient(socket, "example.com")`，
`Handshake()` 之后用 `NegotiatedProtocol()` 读 ALPN 结果（应为 `"h2"`）。
**证书校验默认关闭**（`verify_peer=false`），这是为自签名证书测试的便利，**不是安全默认**，
生产必须打开并配置 CA。

### 已实现并验证

| 能力 | 验证方式 |
|---|---|
HPACK | **RFC 7541 §C.1/C.2/C.3 官方示例逐字节比对** |
Huffman | **`www.example.com` 编码逐字节匹配 §C.4.1** |
帧层 | 10 场景 + 随机 3000 轮 |
流状态机 | **84 组合穷举矩阵** + 随机 5000 轮 |
连接层 | 10 场景（含 CONTINUATION 50000 字节 header、流控窗口耗尽/恢复）|
`FiberTransport` | socketpair 真实 IO 闭环 |
`TlsTransport` | `openssl s_server -alpn h2` 实测 `TLSv1.3` / `ALPN='h2'` |
**双向互通** | curl 8.5.0（内置 nghttp2 1.59.0）当客户端 → 我的服务端 ✅；我的客户端 → `scripts/minimal_h2c_server.py`（手拼帧的独立实现）✅ |

示例与脚本：
- `example/network/http2_h2c_server.cpp` —— 明文 h2c 服务端；
- `example/network/http2_h2c_client.cpp` —— h2c 客户端；
- `scripts/minimal_h2c_server.py` —— 用 socket + struct 手拼帧的最小服务端，用于反向验证；
- `scripts/verify_tls_alpn.sh` —— TLS + ALPN 验证（需 `openssl s_server`）。

### 未实现（如实标注）

| 未做 | 说明 |
|---|---|
**QUIC / HTTP3** | **零实现代码**。只有选型与阅读指南 `docs/quic-go-reading-guide.md`；quic-go 是 Go 库，C++ 无法直接链接，本项目**没有引入任何 Go 依赖** |
**HTTP/2 over TLS 完整链路** | 只验证到 TLS 层 + ALPN 协商；缺真正的 h2 TLS 服务器（nginx/nghttpd）|
**服务端 TLS** | 只做了客户端 |
**流控的阻塞等待** | 窗口不足返回 `ConnStatus::kFlowControl`，不挂起等 `WINDOW_UPDATE`，由调用方重试 |
**DATA 分片** | 单帧超过 `SETTINGS_MAX_FRAME_SIZE` 返回 `kUnsupported`（HEADERS 有 CONTINUATION，DATA 没有对应机制）|
**优先级** | PRIORITY 帧能解析但不参与调度（RFC 9113 已弃用）|
**服务器推送 API** | 能解析 PUSH_PROMISE，无推送接口 |
**会话复用 / 0-RTT** | 未做 |

### 线程与 Fiber 约束

`Connection` **不是线程安全的**，与 Go 的 `http2` 一致：一个连接只能由一个 Fiber/线程驱动。
`TlsTransport` 同理。若要并发处理多条连接，每条连接一个 Fiber。

## Hook 的按函数动态开关

`hook` 模块的每个被拦截的 libc 函数都有**运行时开关**：`hook.enable_<op>`。

### 能力

- **19 个 `HookOp`**：read / readv / recv / recvfrom / write / writev / send / sendto /
  connect / accept / sleep / poll / select / epoll_wait / close / shutdown / fcntl /
  dup / dup2
- 存储是**单个 `std::atomic<uint32_t>` 位图**，检查开销约 1~2 ns
  （relaxed load + 移位 + 与），**无锁、无系统调用**
- 默认**全开**，与引入开关前的行为完全一致
- 支持**运行时热切换**（配置项挂 `AddListener`）

### 已接线（关闭后直调原始 libc，不拦截、不 park Fiber）

`read` / `write` / `recv` / `send` / `connect` / `readv` / `writev` / `sendto` /
`accept` / `recvfrom` / `sleep` —— 共 **11 个**。

### 不可关闭（硬拒绝）

`close` / `shutdown` / `fcntl` / `dup` / `dup2`。

理由：关闭它们会让 fd 元数据与内核状态**脱节**，fd 复用时 Hook 会拿**过期
descriptor** 做判断（例如把普通文件当 socket）。`SetOpEnabled(op, false)` 对这 5 个
返回 `false`，且 `SetAllOpsEnabled(false)` 也会保留它们的位，避免一句话绕过硬拒绝。

### `poll` / `select` / `epoll_wait` 的开关在语义上是空的

它们本来就是 `invoke_native_blocking`（**从不做协程让出**）。关掉只有两种写法：

1. 去掉 `BlockingRegion` → sysmon 收不到「这个 M 在阻塞」的信号，`poll(-1)` 会让 M
   **卡死且无人接管**，比不接线更危险；
2. 保留 → 与现在行为**完全相同**，等于没做。

所以这三个**没有接线**。配置项虽然注册了，但没有实际效果。

### 四个必须知道的危险点

1. **不对称**：`read`/`write`、`recv`/`send`、`recvfrom`/`sendto`、`readv`/`writev`
   是**四对**，必须成对设置。只关一侧会让同一个 fd 的收发路径行为不一致。
2. **在 Fiber 内关 `read`/`recv` 会阻塞整个 worker 线程** —— 该 P 上所有 Fiber 停摆。
   这正是 Hook 存在的理由。
3. **`sleep` 被禁**最能暴露代价：一个 Fiber `sleep_for(1s)`，整个 worker 停 1 秒。
4. **运行中从开→关不会让已 park 的 Fiber 卡死** —— 唤醒路径**不查**这个开关，
   已 park 的 Fiber 仍挂在 fd 等待队列里，可读时照常被 `IOManager` 唤醒。
   开关只影响**新发起**的调用。副作用：可能同时存在「Fiber park 着」与
   「新调用在 worker 上阻塞」。

### 警告文案写在配置里

每个开关的 `description` 都写了**具体的破坏后果**（不是笼统的「危险」），
分两级措辞：「**!!! 危险：不建议关闭 !!!**」与「**!!! 不应关闭 !!!**」。
生成到 `app.ini` 后是普通文本注释（INI 没有颜色），用大写与感叹号做视觉强调。

**一句话原则**：这个开关的存在是为了**诊断**，不是为了**优化**。
关掉它不会让程序更快，只会让协程退化成阻塞线程。

## 错误与限制

Socket 原始调用返回 `SocketResult` 并携带 errno，`SocketStream` 的负返回值为负 errno；短读和短写是正常情况。解析器 body 仍是有限内存缓冲，不能把它用于不受限的大文件；大文件、SSE、multipart 应使用 `ResponseWriter` 并自行设置业务级上限和取消策略。DNS 目前通过 `getaddrinfo` 同步完成，若需要 Fiber 可取消 DNS，应注入异步 Resolver。`sendmsg` 默认加 `MSG_NOSIGNAL`，避免单次写入触发 SIGPIPE 终止进程，但其他裸 fd 写入仍需由调用方遵守平台信号策略。