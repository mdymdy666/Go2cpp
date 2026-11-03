# 网络（Socket / Hook / epoll / HTTP/1 / HTTP/2 / TLS / WS / SSE / QUIC）· 深入学习

> 读法：先读第一部分建立全局图，再按需下钻第二部分。每个结论都给了 `文件:行`，
> 行号以 2026-11-03 的工作树（WSL `/UserData/CodexWorkSpace/Go2cpp`）为准；代码会漂移，
> 读到时先 `sed -n` 看一眼。
> **数字纪律**：本文**不引用任何历史文档的 ns/吞吐当结论**。凡出现吞吐/延迟，
> 要么写「2026-11-03 本机实测：…（命令 + 核数 + 轮数）」，要么写「未实测」。
> 本文全部性能数字均为 **未实测**（原因见文末 §附-2），只保留可复现的入口与口径。
> 配套考试：`docs/exam/02-network.md`（20 题，本文每节标了对应题号）。

## 代码地图

| 层 | 文件 | 规模 | 职责 |
|---|---|---|---|
| Socket | `include/go2cpp/network/socket.hpp` / `src/network/socket.cpp` | 346 / 1188 行 | fd 生命周期、在途计数、accept/connect/send/recv/sendfile |
| IO 多路复用 | `include/go2cpp/network/io.hpp` / `src/network/io.cpp` | 360 / 2949 行 | epoll（ET+ONESHOT）、64 条带、deadline 表、批量唤醒 |
| Hook | `include/go2cpp/network/hook.hpp` / `src/network/hook.cpp` | 405 / 2979 行 | 22 个 `HookOp`、符号 `__wrap_*`、进程级开关位图 |
| HTTP/1 | `include/go2cpp/network/http.hpp` / `src/network/http.cpp` | 585 / 1498 行 | Parser/ResponseWriter/Router/Client/Server/StaticFiles |
| HTTP/2 | `src/network/http2/{connection,frame,hpack,huffman,stream_state,tls_transport}.cpp` | 1444 / 651 / 786 / — / 267 / 753 行 | 帧、HPACK、流状态机、TLS 传输 |
| TLS | `src/network/http2/tls_transport.cpp` | 753 行 | OpenSSL 封装、`MSG_NOSIGNAL` BIO、SIGPIPE 三级快路径 |
| WS / SSE | `src/network/websocket.cpp` / `src/network/sse.cpp` | 1018 / 808 行 | RFC 6455 帧与消息；SSE 事件流 |
| QUIC / HTTP3 | `src/network/quic/picoquic_connection.cpp` / `src/network/http3/client.cpp` | 698 / 1348 行 | picoquic 薄封装 + 客户端请求/响应 |
| bento HTTP 服务端 | `src/bento/net/http_server.cpp` | 755 行 | **pipeline 三策略（postpone/CORK/writev 批量）在这里，不在 `http::Server`** |
| bento 协议 | `src/bento/net/{mqtt,coap,socks5}.cpp` | 1380 / 976 / 1280 行 | MQTT 3.1.1 / CoAP / SOCKS5（`bento::net`，需 `GO2CPP_WITH_BENTO=ON`） |

---

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 入口点清单

**Socket 生命周期与数据面**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `Socket::accept(timeout=max)` | `include/go2cpp/network/socket.hpp:135`，实现 `src/network/socket.cpp:453` | 用户 / `bento` 监听循环 | `begin_operation` 固定 fd → `accept4(O_NONBLOCK)`（`:464`）→ 纠正 Hook 标志（`:473`）→ `FromFd`（`:475`）→ 默认 `TCP_NODELAY`（`:486`） | `Socket::Ptr`（失败空） |
| `Socket::connect(addr, timeout=max)` | `socket.hpp:136`，实现 `socket.cpp:503` | 客户端 | 一次 `::connect`（`:528`），`EINPROGRESS` → `wait_ready(kWrite)`（`:550`）→ `getsockopt(SO_ERROR)`（`:565`） | bool |
| `Socket::send/recv/sendv/recvv` | `socket.hpp:150-199`，实现 `socket.cpp:797/800/803/821` | 上层协议 | `begin_operation` → `sendmsg/recvmsg`（`send_impl` `:748`，`:758` 带 `MSG_NOSIGNAL`）→ 阻塞走 `wait_ready` | `SocketResult{transferred,error}` |
| `Socket::send_file(in_fd, off_t*, count, timeout)` | `socket.hpp:201`，实现 `socket.cpp:1026`（内部 `send_file_impl` `:1048`） | `http::send_file_body`、`bento::SendFileBody` | `sendfile(2)` 循环，处理 EINTR/EAGAIN/部分写 | `SocketResult` |
| `Socket::close()` | `socket.hpp:143` 附近，实现 `socket.cpp:573` | 任意 | 置 closing → 等 `m_inflight` 归零 → 真正 `::close` | bool |
| `Socket::shutdown(how)` | `socket.cpp:612` | 任意 | 直接 `::shutdown`（**Hook 层从未接管**，见 2.3） | bool |
| `Socket::SetTcpNoDelay / SetTcpNoDelayDefault` | `socket.hpp:96` / `:103`，实现 `socket.cpp:439` / `:447` | `accept` 内部 / 对比实验 | 进程级默认值（`:450` 读取） | bool / void |
| `Socket::begin_operation / end_operation` | `socket.cpp:224` / `:281` | 上面所有数据面入口 | 一次 `fetch_add/fetch_sub` 在途计数；0→1 装 keepalive（`:252`）；归零收尾（`:282-300`） | bool / void |

**IO 多路复用（epoll）**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `IOManager::wait_for(fd,event,timeout,ctx)` | `include/go2cpp/network/io.hpp:259`，实现 `src/network/io.cpp:2391` | `Socket::wait_ready` | 换算 deadline → `wait()` 登记 waiter → park | `WaitResult` |
| `IOManager::wait_many(requests,deadline,ctx)` | `io.hpp:272`，实现 `io.cpp:2400` 起 | 需要一次等多个 fd 的调用方 | 校验集合 → 一次 arm 全部 → park（**不是 wait-all，返回已就绪子集**） | `WaitManyResult` |
| `IOManager::poll_once(timeout_ms)` | `io.cpp:1878` | Scheduler 的空闲 worker（经 `set_io_idle_poller` 注入，`io.cpp:2178`） | `epoll_wait` → `process_event` → 唤醒节点 → 批量 `wake_io_batch`（`:1971`） | int（处理的事件数） |
| `State::process_event_locked` | `io.cpp:1509` 起 | `poll_once` / 线程 poller | 取队首 waiter、置 `pending_read/write`、ET 下强制 rearm（`:1533-1535`） | void |
| `IOManager::tickle()` | `io.cpp`（`m_eventfd` 写入） | 任意线程要打断 `epoll_wait` | 写 eventfd | void |

**Hook**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `HookOp` 枚举（22 项） | `include/go2cpp/network/hook.hpp:175-198` | 配置/诊断 | `kRead…kDup2` + `kCount` | — |
| `OpEnabled(op)` | `hook.hpp:228` 附近，实现 `hook.cpp` | 每个 hook 入口 | 读进程级位图 | bool |
| `SetOpEnabled(op,enabled)` | `hook.hpp:235`，实现 `hook.cpp:1785` | 用户/测试 | 改位图；`close/shutdown/fcntl/dup/dup2` **硬拒绝**（`hook.cpp:1756-1768`、`:1793-1796`） | bool（拒绝即 false） |
| `read/write/accept/connect/sleep…`（`__wrap_*`/同名符号） | `hook.cpp:2327/2436/2297/2288/2017` | 被 LD_PRELOAD 或链接期替换的 libc 调用 | 关掉则直通原始符号；开着走 `cooperative_io` park | ssize_t/int |
| `shutdown_f(fd,how)` | `hook.hpp:393`，实现 `hook.cpp:2935` | 需要「命名统一」的调用方 | **等价于 `::shutdown`**：本层从未 hook shutdown（`:2936-2938`） | int |

**HTTP/1**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `Parser::feed(bytes, Request&)` | `include/go2cpp/network/http.hpp`，实现 `src/network/http.cpp:390` → `feed_impl:410` | Server / 测试 | 增量解析头 + body（Content-Length/chunked，`:583` 上限检查） | `ParseResult{state,consumed,error}` |
| `Parser::take_remaining()` | `http.cpp:409` | 任何希望拿 pipelining 剩余字节的人 | `swap` 出 `m_buffer` 并 `reset()` | `std::string` |
| `Parser::finish(Response&)` | `http.cpp:392` | Client 解析 EOF 定长响应 | 关闭定界收尾 | `ParseResult` |
| `Server::serve_connection(conn, max_requests)` | `http.hpp:561`，实现 `http.cpp:1454` | 用户的 accept 循环 | 单连接串行循环（`:1462`）：recv→feed→route→写头→sendfile→keep-alive | void |
| `Server::stop()` | `http.hpp:563` | 所有者线程 | 置 `m_stopped`，让主循环在下一轮退出 | void |
| `Router::add/match` | `http.hpp:438` 起 | `serve_connection` | 精确哈希 → `:name` 参数段 → 尾部 `*` 通配 | `Handler*` |
| `ResponseWriter::start/write/flush/close` | `http.cpp:762/783/813/814` | `sse::Stream`、chunked 发送方 | `start` 强制 chunked 并丢弃 `Response::file`（`:767`）；`write` 直接落 Socket | bool |
| `Client::get/head/post/…` | `http.hpp:341` 起，`do_request` 声明 `:344` | 用户 | 建连 + 发请求 + 解析响应（100-continue 用 `take_remaining` 保留流水线，`http.cpp:716`） | `Result` |
| `StaticFiles`（open + sendfile + Range/304/HEAD） | `http.cpp:1193` 起 | 路由 handler | 单区间 Range、ETag/Last-Modified、HEAD 只回头 | `Response` |

**HTTP/2 / TLS / QUIC**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `http2::Connection::Handshake` | `include/go2cpp/network/http2/connection.hpp:274` | 客户端/服务端 | 发本地 SETTINGS（`connection.cpp:383-393`）、读对端 SETTINGS | `ConnStatus` |
| `Connection::StartRequest` | `connection.hpp:295` | 客户端 | **先查对端 `MAX_CONCURRENT_STREAMS`**（`connection.cpp:502`）再分配流号 → HPACK 编码（`:529`） | `ConnStatus` |
| `Connection::WriteData` / `SendResponseHeaders` / `ResetStream` | `connection.hpp:308/312/317` | 双向 | 推进流状态机 + 写帧 | `ConnStatus` |
| `Connection::ReadEvent` | `connection.hpp:347` | 事件循环 | 帧分发 → HEADERS 拼 CONTINUATION（`connection.cpp:1110`）→ HPACK 解码（`:1129`）→ 两道上限 | `ConnStatus` |
| `StreamStateMachine::SendHeaders/CanSendData/ReceiveData` | `src/network/http2/stream_state.cpp:71/116/229` | `Connection` | 预检语义的状态转移（`RefreshFromEndFlags:49`） | `StreamEventResult` |
| `TlsTransport`（构造 `verify_peer=true`） | `include/go2cpp/network/http2/tls_transport.hpp:121` | 客户端 | 建 `SSL_CTX`、装 `MSG_NOSIGNAL` BIO（`tls_transport.cpp:58`） | 对象 |
| `ScopedSigpipeBlock(required)` | `tls_transport.cpp:185` | `SSL_*` 包装 | 三级快路径（`:190-200`） | RAII |
| `http3::Client::Connect/Send/Receive/Poll` | `src/network/http3/client.cpp:1030/1114/1200/1084` | 用户 | `Connect` 只建对象发 ClientHello，握手在首次 `Send/Receive/Poll` 推进 | `unique_ptr<Client>` / bool |
| `quic::PicoquicConnection::Connect/Poll/Close` | `src/network/quic/picoquic_connection.cpp:287/390/558` | `http3::Client` | 调 picoquic；本层不实现 QUIC | 对象 / bool |

**WS / SSE / bento::net 协议**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `WebSocket::Accept/AcceptWithPending` | `include/go2cpp/network/websocket.hpp:139/142`，实现 `websocket.cpp:406/411` | HTTP Upgrade handler | 校验握手头、回 101；**不拥有 fd** | `WebSocket::Ptr` |
| `WebSocket::Connect` | `websocket.hpp:146`，实现 `websocket.cpp:453` | 客户端 | 发 Upgrade 请求 | `Ptr` |
| `read_message/send_text/send_binary/ping/pong/close` | `websocket.hpp:167/169/171/173/175/177` | 用户 | 读单线程（`m_read_mutex`）、写多线程；`auto_pong` 默认 true（`websocket.hpp:90`） | `WebSocketResult` |
| `abort_transport()` | `websocket.cpp:554` | `~WebSocket` | 只中止读写，不 close fd | void |
| `sse::Stream::start/send/tick/close` | `include/go2cpp/network/sse.hpp:276/278/282/288`，实现 `sse.cpp:653/702` | 用户 | chunked 写事件；`tick()` 发心跳 | `Result` |
| `sse::Parser::feed` | `sse.cpp:320` | 客户端 | 有状态单消费者解析（`retry:` 解析在 `:71`，输出在 `:149`） | `Result` |
| `bento::net::MqttSession::HandleBytes` | `include/go2cpp/bento/net/mqtt.hpp:395`，实现 `src/bento/net/mqtt.cpp` | bento HTTP/WS 服务端 | MQTT 3.1.1 会话 | `MqttStatus` |
| `bento::net::CoapClient::Send` | `include/go2cpp/bento/net/coap.hpp:480` | 用户 | 发 CoAP 请求（`HandleDatagram` `:496`） | bool |
| `bento::net::Socks5Server::Open/Start` | `include/go2cpp/bento/net/socks5.hpp:307/314` | 用户 | SOCKS5 监听与代理 | bool |

**默认值与编译开关**

| 项 | 默认 | 出处 | 说明 |
|---|---|---|---|
| 新连接 `TCP_NODELAY` | 开 | `socket.cpp:447-451`、`:483-487` | 只对 `AF_INET/AF_INET6` 的 `SOCK_STREAM` 设置；Unix socket 会 `EOPNOTSUPP` |
| `serve_connection` 的 `max_requests` | 由调用方传（bento 侧默认 100） | `http.cpp:1454`、`:1462` | 防单连接饿死其它连接 |
| HTTP 解析上限 | `ParserLimits`（`max_body_bytes` 等） | `http.cpp:583` | 超限返回 `ParseState::Error` |
| `verify_peer` | **true** | `tls_transport.hpp:22`、`:102`、`:121` | 审计后从 false 改过来的 fail-closed；关掉同时失去链校验与主机名匹配 |
| HTTP/2 `max_header_block_size` / `max_header_list_size` | 64 KiB / 64 KiB | `connection.hpp:90` / `:97` | 两道闸：压缩块 vs 解压后列表 |
| HTTP/2 `max_concurrent_streams` | 100 | `connection.hpp:105` | 本端通告值；对端值另存 `m_peer_max_concurrent_streams`（`:445`） |
| WS `max_frame_payload` / `max_message_payload` | 16 MiB / 64 MiB | `websocket.hpp:87-88` | 发送超限按 16 MiB 拆 continuation（`:92-94`） |
| `auto_pong` / `validate_utf8` | true / true | `websocket.hpp:90-91` | — |
| `GO2CPP_NETWORK_HOOK_ENABLED` | 由 CMake 决定 | `cmake/Go2CppModules.cmake:507`（=1）/ `:509`（=0） | 关闭时 `hook.cpp` 不接管，`socket.cpp:472` 那段纠正也编译掉 |
| `GO2CPP_WITH_BENTO` | **OFF** | `cmake/Go2CppBento.cmake:17` | 打开才有 MQTT/CoAP/SOCKS5/bento HTTP server |

### 1.2 总架构图

```
┌────────────────────────────────────────────────────────────────────────────┐
│ 用户代码：accept 循环 → serve_connection；Client::get；WebSocket；SSE       │
└──────────────────────────┬─────────────────────────────────────────────────┘
                           │ 公开 API（§1.1）
┌──────────────────────────▼─────────────────────────────────────────────────┐
│ 协议层（本模块）                                                            │
│  http::Parser / Router / ResponseWriter / StaticFiles     sse::Stream       │
│  http2::Connection / HPACK / StreamStateMachine           websocket         │
│  TlsTransport(SSL_CTX + NoSignalBIO)                      http3::Client     │
└──────┬───────────────────────────────┬─────────────────────────────────────┘
       │ 字节流                         │ 事件
┌──────▼───────────────┐   ┌───────────▼─────────────────────────────────────┐
│ Socket（fd 归属）     │   │ IOManager::State                                 │
│  m_inflight 原子计数  │   │  64 条带（fd → FdSlot）                           │
│  keepalive 自引用     │   │  readers/writers FIFO + pending_read/write        │
│  wait_ready 分片等待  │   │  deadline multimap + registrations(generation)    │
└──────┬───────────────┘   │  m_backend = epoll(EPOLLET|EPOLLONESHOT)          │
       │                   │  m_eventfd = tickle                              │
       │                   └───────────┬─────────────────────────────────────┘
       │                               │
┌──────▼───────────────────────────────▼─────────────────────────────────────┐
│ Hook（可选，编译期开关）                                                    │
│  HookOp 位图（进程级） → cooperative_io → IOManager::wait → Scheduler park  │
│  关闭/未编译：直接走原始 libc，阻塞整个 M                                   │
└──────┬─────────────────────────────────────────────────────────────────────┘
       │ 线程/Fiber 边界
┌──────▼─────────────────────────────────────────────────────────────────────┐
│ 运行时/内核                                                                 │
│  Scheduler：worker 空闲时 poll_once（set_io_idle_poller）→ wake_io_batch    │
│  epoll_wait / accept4 / connect / sendmsg(MSG_NOSIGNAL) / sendfile / recvmmsg│
└────────────────────────────────────────────────────────────────────────────┘

一次「读就绪 → Fiber 被唤醒」的数据流：
  Fiber 调 recv → EAGAIN → Socket::wait_ready（socket.cpp:662）
    → 受管 Fiber 分支：IOManager::wait_for（io.cpp:2391）按 ≤100ms 分片
      → 登记 waiter 到条带 FIFO → epoll_ctl ADD/MOD（ONESHOT 每次重 arm）
      → Scheduler park（Fiber 切走，M 继续跑别的 G）
  空闲 worker：poll_once（io.cpp:1878）→ epoll_wait 返回
    → process_event（:1547）→ process_event_locked（:1509）取队首 waiter
    → 收集成 vector → wake_io_batch（:1971，>1 个时）或逐节点 wake
    → G 回到 P 队列 → 某 M resume → recv 重试
```

**四个边界要记牢**：

1. **fd 边界**：`Socket::fd()` 是瞬时快照（`socket.cpp:344`），拿到后**不能跨操作持有**；要用就得 `begin_operation` 固定住（`:224`）。
2. **Fiber 边界**：受管 Fiber 的等待是 park，不占 M（`hook.hpp:170-174` 的注释把这条讲清楚了）；手动 Fiber 只能 100ms 分片轮询（`socket.cpp:695-706`）。
3. **epoll 边界**：`EPOLLONESHOT` 意味着**每次被唤醒后必须重新 arm**（`io.cpp:273` 字段表、`:1834` 注释）；ET 意味着**必须读到 `EAGAIN`**，否则该 fd 静默。
4. **Hook 边界**：Hook 开关是进程级全局（`hook.hpp:220` 警告），关掉只影响「新发起的调用」，不影响已 park 的 Fiber 的唤醒路径（`hook.hpp:172`）。

### 1.3 复杂场景 ≥10 步详解

#### 场景 A：一次 HTTP/1.1 GET 从 accept 到响应写完（17 步，含 pipeline）

1. **accept**：用户调 `Socket::accept()`（`socket.cpp:453`）→ `begin_operation`（`:224`）`fetch_add` 在途计数，0→1 时装 keepalive（`:252`）；`m_closing` 已置则立即 `end_operation` 返回（`:226-230`）。
2. **建连**：`::accept4(..., SOCK_CLOEXEC|SOCK_NONBLOCK)`（`:464`）；`EAGAIN` 则 `wait_ready(kRead, left)`（`:498`），超时置 `ETIMEDOUT`（`:496`）。
3. **纠正 Hook 元数据**：`mark_system_nonblocking(accepted)`（`:473`）——否则 Hook 把运行时的 `O_NONBLOCK` 误判成用户意图，拒绝 park，退化成 100ms 分片轮询（注释 `:467-471` 记录了 10.3 次 `epoll_ctl`/请求的旧数据）。
4. **新连接默认值**：`FromFd`（`:475`）→ `m_connected=true`（`:477`）→ 仅 IPv4/v6 的 stream 才 `SetTcpNoDelay`（`:483-487`）。
5. **交给协议层**：用户把 `Socket::Ptr` 传给 `Server::serve_connection(conn, max_requests)`（`http.cpp:1454`）；一个连接一个 Fiber 串行跑（`http.hpp:539`）。
6. **进主循环**：`for (i < max_requests && !m_stopped)`（`http.cpp:1462`）；`max_requests` 是公平性上限，不是 bug（`:1419` 的参数注释）。
7. **读**：无 pending 时 `connection->recv(buffer, 8192)`（`:1467`）→ `Socket::recv`（`socket.cpp:800`）→ `recv_impl`（`:773`）→ `begin_operation` 固定 fd；`EAGAIN` → `wait_ready`（`:662`）。
8. **park 链**：受管 Fiber 走 `IOManager::wait_for`（`io.cpp:2391`），单次最多 100ms（`socket.cpp:697-700`）——分片的目的不是超时语义，而是**让 `close()` 能打断无限等待**；`IOManager::wait` 把 waiter 挂到 64 条带之一并 `epoll_ctl`，随后 Scheduler park（`park_for_scheduler`）。
9. **唤醒**：空闲 worker 在 `poll_once`（`io.cpp:1878`）里 `epoll_wait`；`effective` 超时取「调用方超时」与 `poll_timeout_ms()`（`:1685`，最近 deadline）的较小值（`:1885-1890`）；事件走 `process_event_locked`（`:1509`）摘队首 waiter。
10. **ET/ONESHOT 收尾**：本轮边沿已被消费，若同 fd 还有等待者必须 `force_rearm`（`:1533-1535`）让内核再产生一次边沿；随后 `update_interest_locked`（`:1537`）重新 arm。
11. **批量唤醒**：`wake_io_batch`（`:1971`）一次投递多个 G，避免「一个事件一次跨线程唤醒」。
12. **解析**：`parser.feed(bytes)`（`http.cpp:390` → `feed_impl:410`）；body 上限 `max_body_bytes`（`:583`），chunked 与 Content-Length 在 `:505-526` 分流。
13. **pipeline 剩余**：`consumed` 返回的是**本次喂入字节数**（`:404`），不是「已消费偏移」；所以不能用它 `erase` 拿第二条，必须 `take_remaining()`（`:409`，`swap` + `reset`）→ `pending = ...`（`:1471`）。
14. **路由**：`m_router.match`（`:1475`）→ 精确哈希 → 参数段 → 尾部通配；HEAD 未注册时回退 GET（`:1478-1481`）；全不中走 `m_fallback`（`:1482`）。
15. **handler**：`(*selected)(request)`（`:1484`）；异常 → 500 + 强制 close（`:1485`）；无 handler → 404（`:1486`）。
16. **写头**：`response.to_string(is_head)`（`:1488`）→ `SocketStream::write_all`（`:1490`）→ `Socket::send_impl`（`socket.cpp:748`）带 `MSG_NOSIGNAL`（`:758`），从源头不产生 SIGPIPE。
17. **写体 + 收尾**：非 HEAD 且有文件 → `send_file_body`（`http.cpp:829`）`open`（`:833`）+ `socket->send_file`（`socket.cpp:1026`）循环（`:836-846`）；`request.close || response.close` 才退出（`:1494`），否则回到第 6 步直到 `max_requests`。

失败/阻塞分支：
- 第 1 步 `m_closing` → `EBADF`（`socket.cpp:228-230`）；
- 第 7 步 `transferred <= 0` → `serve_connection` 直接 `return`（没有半关闭响应，连接级超时由上层负责）；
- 第 12 步解析错误 → `return`（`:1468`），不回 400（**已知限制**：错误响应由调用方自己写）；
- 第 16 步写失败 → `return`（`:1490`），文件体写失败同样 `return`（`:1492`）。

> **pipeline 的真实加速不在这个函数里**。`http::Server` 是「一条请求一次 write」。把 N 条响应攒成一次 `writev`、`TCP_CORK` 合包、postpone_output 的是 **`src/bento/net/http_server.cpp`**（`kWriteBatchBytes` `:509`、CORK 阈值 `:73`、CORK 开关 `:99-105`/`:123-127`、`FlushBatch` `:156`）。仓库里 pipeline 单连接 72.9k 的**唯一出处**是 `docs/reports/asm-http-e2e.md:18` 与 `:127`；411k 这个数全库查不到，**不要引用**。

#### 场景 B：TLS 握手 + 一次小包写（13 步，SIGPIPE 快路径）

1. **建 transport**：`TlsTransport(socket, server_name, verify_peer=true, ca_file, ca_path, alpn)`（`tls_transport.hpp:121`）——默认校验（`:22`、`:102`），审计后从 `false` 改过来。
2. **建 SSL_CTX**：设最低 TLS1.2、按 `verify_peer` 设 `SSL_VERIFY_PEER`；`verify_peer=true` 时加载 `ca_file/ca_path` 或默认信任库（`:114-118`）。
3. **装自定义 BIO**：`NoSignalBioWrite`（`tls_transport.cpp:58`）用 `send(MSG_NOSIGNAL)` 替代 OpenSSL 自带 socket BIO 的 `write()`（`:51-57` 注释）。读仍走 `recv`，保持 `retry` 标志语义。
4. **握手写 ClientHello**：`SSL_connect` → BIO 写 → 若对端已关，`send` 返回 `EPIPE` → `SSL_ERROR_SYSCALL`，**不产生信号**。
5. **握手失败分类**：`SSL_ERROR_WANT_READ/WRITE` → 交给上层的 IO 等待；其它错误 → `ERR_get_error` 链式取出（`:38-45` 的 `SslErrorText`）。
6. **应用写**：`TlsTransport::Write` 包装 `SSL_write`。
7. **旧实现的税**：每次写完都要 `pthread_sigmask(SIG_SETMASK)` 恢复（`:186-200` 注释）；旧口径是每次写 4 次信号系统调用。
8. **快路径①**：`required=false`（已装 `MSG_NOSIGNAL` BIO）→ **直接 return，0 次系统调用**（`:190-192`）。
9. **快路径②**：进程已 `SIG_IGN` SIGPIPE → `SigpipeGloballyIgnored()` 成立 → 也不进内核（`:192-193`）。
10. **兜底**：只有前两条都不成立才 `sigemptyset/sigaddset/pthread_sigmask(SIG_BLOCK)`（`:195-197`）。
11. **失败才查 pending**：`MarkFailure()`（`:224`）由调用方在 `SSL_*` 返回 ≤0 且非 `WANT_*` 时调用；析构里**只有 `m_failed` 为真**才 `sigpending` + `sigtimedwait` 消费（`:208-216`）。
12. **为什么必须消费**：写失败时内核把 SIGPIPE 记为「本线程 pending」，恢复掩码的瞬间按默认动作投递 → 进程被杀（`:202-207` 注释，实测对端先 close 时客户端析构仍会 SIGPIPE）。
13. **恢复**：`pthread_sigmask(SIG_SETMASK, &m_previous)`（`:218`）——这是唯一一次「成功路径也要付」的调用。

失败/阻塞分支：
- 第 3 步 BIO 创建失败 → `TlsTransport` 构造返回空，调用方拿不到对象；
- 第 5 步证书校验失败 → 握手错误，**不会**自动降级（`verify_peer` 只由显式配置改）；
- 第 11 步 `sigpending` 失败 → 直接恢复掩码（可能仍被 SIGPIPE 杀，这是兜底路径的已知缺口）；
- TLS 只有**客户端**：没有 `SSL_accept` 路径、没有会话复用/0-RTT/ticket（`tls_transport.hpp:22-26` 的能力边界）。

#### 场景 C：客户端 connect + 超时 + 取消（12 步）

1. **入口**：`Socket::connect(addr, timeout)`（`socket.cpp:503`）；先读 `m_write_cancel_generation`（`:505`）作为取消谓词的锚点。
2. **固定 fd**：`begin_operation(fd)`（`:506`）；失败（已关闭）直接返回 false。
3. **取消先检**：代号已变 → `errno=ECANCELED` + `end_operation` 返回（`:511-517`）。
4. **记录远端**：`m_remote = UnknownAddress(...)`（`:523-525`），供 `reconnect()`（`:567`）使用。
5. **一次 connect**：`::connect`（`:528`）；返回 0 → `m_connected=true`（`:530`）、`end_operation`、返回 true（`:534`）。
6. **错误分流**：`errno` 不是 `EINPROGRESS/EALREADY` → 真错误，`end_operation` 返回 false（`:536-540`）。
7. **等可写**：`remaining(timeout, start)`（`:544`）算剩余预算（**不能每片重算**，见 `:655-657` 的注释），`wait_ready(kWrite, ...)`（`:550`）。
8. **等待实现**：`wait_ready`（`:662`）先做三项前置检查（fd 无效/已取消/正在关闭，`:674-676`），关闭优先于等待。
9. **专用 waiter**：若 `m_waiter` 非空（例如 `bento` 注入的等待器）→ `waiter->wait`（`:684`），返回后**再查一次取消**（`:685`）。
10. **受管 Fiber 分支**：`IOManager::current()` 非空 → 100ms 分片 `wait_for`（`:695-706`）；每次 park 返回后重新查取消/关闭/超时，并**重新写 errno**（Fiber 可能已换线程，`:699-703` 注释）。
11. **查结果**：`getsockopt(SO_ERROR)`（`:561-565`）；内核错误码通过 `errno` 返回，`end_operation` 收尾（`:558`）。
12. **成功**：置 `m_connected`（`:563`）、`end_operation`（`:565`）、返回 true。

失败/阻塞分支：
- 第 3/9/10 步取消 → `ECANCELED`，且 `end_operation` 保证在途计数归零；
- 第 7 步超时 → `wait_ready` 返回 `ETIMEDOUT`，`errno` 显式设置（`:496` 的 accept 分支同理）；
- 第 11 步 `SO_ERROR` 非 0 → 返回 false（`:566-573`），**不重试**；
- `timeout=Duration::max()` 表示无限，分片逻辑单独判断（`:653-654` 注释）。

### 1.4 关键不变量（破了就一定出 bug）

| # | 不变量 | 依据 | 破了会怎样 |
|---|---|---|---|
| I1 | fd 的所有系统调用都必须在 `begin_operation`/`end_operation` 之间 | `socket.cpp:224-246`、`:281-300` | 关闭路径永远等不到 inflight 归零 → `close()` 卡死；或用已关闭/已复用 fd 做 IO |
| I2 | `end_operation` 归零时必须**先 notify 再 finalize** | `socket.cpp:282-300` 的顺序注释 | 关闭者看不到「inflight==0」的谓词成立 → 互等 |
| I3 | `EPOLLONESHOT` 下每次唤醒都要重新 arm；ET 下必须读到 `EAGAIN` | `io.cpp:273`、`:1533-1537`、`:1834` | 连接永久静默（Q01 的正确答案） |
| I4 | 同 fd 还有等待者时，ET 边沿必须强制 MOD 重新产生 | `io.cpp:1531-1535` | 第一个 waiter 吃掉边沿，后续 waiter 永远等不到 |
| I5 | `Parser::feed` 的 `consumed` = 本次喂入字节数，pipelining 剩余必须 `take_remaining()` | `http.cpp:404`、`:409`、`:1471` | 第二条请求被 `erase` 丢掉 → pipeline 只处理第一条 |
| I6 | HPACK 必须先解完再回 `RST_STREAM` | `http2/connection.cpp:1196-1200` | 动态表两边不同步 → 后续所有 header 解析偏移 |
| I7 | 超限判定不能在单帧路径上跳过（单片 HEADERS 不走 `AssembleHeaderBlock`） | `connection.cpp:1117-1124` | 一个 10MB 单帧绕过第一道闸 → 内存打爆 |
| I8 | 写路径必须全程无 SIGPIPE 源（`MSG_NOSIGNAL` BIO 或屏蔽 + 消费 pending） | `tls_transport.cpp:190-218`、`socket.cpp:758` | 对端一关就杀进程；正是「只恢复掩码不够」的那条 |
| I9 | `HookOp::kClose/kShutdown/kFcntl/kDup/kDup2` 不可关闭 | `hook.cpp:1756-1768`、`:1793-1796` | 关掉后 fd 元数据与内核脱节，fd 复用时 Hook 把普通文件当 socket |
| I10 | Hook 开关是进程级位图，必须 RAII 恢复 | `hook.hpp:220`、`:332` | 忘恢复 → 后续所有 Fiber 静默退化（不 park、阻塞 worker） |
| I11 | chunked 路径上没有 sendfile：`ResponseWriter::start` 必须丢弃 `Response::file` | `http.cpp:767` | 长度语义冲突，客户端读到坏包 |

---

## 第二部分 · 分：逐块细讲

### 2.1 Socket：在途计数与「关不掉」的 fd

**职责**：把裸 fd 的生死交给一次原子计数，让「一个 Fiber 正在 recv、另一个 Fiber 把 fd close 掉」不产生 UAF。

**关键数据结构**

| 字段 | 位置 | 说明 |
|---|---|---|
| `m_inflight` | `socket.cpp` Impl 内（`begin_operation` 读写） | 在途操作计数，热路径只有一次 `fetch_add/sub` |
| `m_closing_flag` | `socket.cpp:225` 读 | 置位后新操作直接 `EBADF` |
| `m_inflight_keepalive` | `socket.cpp:252` 装、`:295` 移出 | 0→1 时装一次 `shared_from_this()`，避免每包两跳原子引用计数 |
| `m_fd_fast` | `socket.cpp:236` 读 | fd 的无锁快照（`fd()` `:344` 也是它） |
| `m_waiter` | `socket.cpp:427`（set_waiter） | 可注入的等待器（bento/自定义） |

**`begin_operation` 逐步拆**（`socket.cpp:224`）

1. `m_inflight.fetch_add(1, acq_rel)`（`:225`）——所有路径都要先计数。
2. 若已 closing → `end_operation()` 走统一归零路径，`errno=EBADF`，返回 false（`:226-230`）。
3. 若 `prev==0`（从空闲转有活）→ `InstallKeepalive()`（`:252`）；失败同样回滚（`:231-235`）。
4. 取 fd 快照（`:236`）；`<0` 则回滚（`:237-241`）。
5. 成功：把 fd 写给调用方（`:243`）。

**失败路径与限制**
- 计数异常（重复 `end_operation`）时恢复 0，避免无符号下溢把关闭路径永久卡住（`socket.cpp:283-286`）；
- `fd()` 是瞬时值，**不能跨操作持有**（`:344-347`）——这是 Q03 最容易答错的地方；
- `wait_ready` 的分片是 100ms（`:697-700`），不是协议超时；真正的超时由 `remaining()` 计算。

**典型误用**：拿到 `fd()` 后在另一个线程里 `::read`；或在 `Socket` 析构后继续用 `Socket::Ptr` 的裸 fd 做 `epoll_ctl`。

### 2.2 epoll：ET + ONESHOT + 条带

**职责**：把 fd 就绪事件翻译成「唤醒哪个 waiter」，并且保证同一 fd 的多个等待者都能拿到边沿。

**关键数据结构**（`io.cpp:620-639` 的 `FdSlot`）

| 字段 | 行号 | 说明 |
|---|---|---|
| `readers` / `writers` | `:622` / `:624` | FIFO 等待队列；`register_wait` 入队、`process_event_locked` 出队 |
| `registered` / `armed` | `:627` / `:630` | 内核里是否有注册 / ONESHOT 后是否仍在监听 |
| `interest` | `:632` | 当前注册的掩码；0 表示「停用但保留注册」 |
| `registration_id` | `:634` | 编码 fd + 序号，用来丢弃迟到的旧事件 |
| `force_rearm` | `:636` | **ET 下同 fd 还有等待者时必须 MOD** |
| `pending_read` / `pending_write` | `:638` / `:640` | 事件到了但没 waiter，留给下一次 wait 直接消费 |

**`poll_once` 逐步拆**（`io.cpp:1878`）

1. 后端缺失或 `m_stop_poller` → 返回 -1（`:1880-1882`）。
2. `effective = max(0, timeout)`，再与 `poll_timeout_ms()`（`:1685`）取小；deadline 已到期则 `effective=0`（`:1885-1890`）。
3. `BackendEvent events[256]` **不做值初始化**——`epoll_wait` 只填 count 个（`:1892-1896` 注释：空转时 4KB 清零占主导）。
4. `m_waiters++`、`t_inside_poll=true`（`:1897-1898`），供 `tickle()` 判断是否需要 eventfd 打断。
5. `m_backend->wait(...)`；EINTR **只重试 wait，不重试整个函数**（`:1843` 注释）。
6. 逐个 `process_event(registration_id, events)`（`:1547`）：`registration_id` 与 slot 当前值不符 → 丢弃（迟到的旧注册）。
7. 锁外 `wake_nodes`，避免「条带锁 + 调度器锁」嵌套。

**状态机**（一个 slot 的注册状态）

```
未注册 ──register_wait──▶ registered=true, armed=true（epoll_ctl ADD）
   ▲                              │ 事件返回（ONESHOT 自动 disarm）
   │                              ▼
   │                     armed=false, interest 可能仍非 0
   └──interest==0 且无 waiter──── 停用（保留 fd 在 epoll 里）
```

**失败路径与限制**
- `update_interest_locked` 失败 → `fail_slot_locked` 把该 fd 所有 waiter 置错误（`:1537-1540`）；
- 无 `futex_waitv` 环境不影响这里，但影响 `ParkingCondition` 的 `notify_all`；
- **已知未验证**：64 条带的哈希碰撞在极端 fd 分布下的表现，本文没有压测。

**典型误用**：把 `EPOLLET` 当「有数据就通知」（Q01）；或以为 `interest==0 && registered==true` 是 bug——注释明确说这是**合法状态**（`io.cpp:290`）。

### 2.3 Hook：22 个操作，其中 5 个不许关

**职责**：把 libc 的阻塞调用换成「登记 → park → 唤醒」的协作式 IO；不改变语义，只改变等待方式。

**关键数据结构**

| 项 | 行号 | 说明 |
|---|---|---|
| `HookOp` 枚举 | `hook.hpp:175-198` | `kRead,kReadv,kRecv,kRecvfrom,kWrite,kWritev,kSend,kSendto,kConnect,kAccept,kSleep,kNanosleep,kUsleep,kClockNanosleep,kPoll,kSelect,kEpollWait,kClose,kShutdown,kFcntl,kDup,kDup2,kCount` |
| `g_hook_op_mask` | `hook.cpp`（`SetOpEnabled` 里 `fetch_or/fetch_and`，`:1797-1801`） | 进程级原子位图 |
| `IsNonDisableable` | `hook.cpp:1758-1770` | close/shutdown/fcntl/dup/dup2 硬拒绝 |
| `s_originals` | `hook.cpp`（`initialize_originals`） | 原始符号表；`kShutdown` **没有对应项**（`:2936`） |

**关键函数**

- `SetOpEnabled`（`hook.cpp:1785`）：`bit >= kHookOpCount` → false；`!enabled && IsNonDisableable` → **不改位图**返回 false（`:1793-1796`）；否则 `fetch_or/and`（`:1797-1801`）。
- `read`（`hook.cpp:2327`）：`OpEnabled(kRead)` 为假 → 调 `s_originals.m_read` 或裸 `syscall(SYS_read,...)`（`:2329-2331`）；`s_resolving` 期间同样直通（`:2332-2334`）；否则 `cooperative_io<ssize_t>`（`:2336`）。
- `shutdown_f`（`hook.cpp:2935`）：注释写明「`shutdown` 在本层**从未被 hook**（`Originals` 里没有对应项，只有 `hook.enable_shutdown` 这个配置项是配置表完整性遗留）」（`:2936-2938`）——**这就是 Q05 的答案**：枚举里有 `kShutdown`（`hook.hpp:195`）、配置表里有名字（`hook.cpp:1715`、`:1944`），但**实现层没有接管**，所以对端关闭只能靠 `read` 返回 0。

**状态机**：每个 op 只有「开/关」两态；关闭是**惰性**的——已 park 的 Fiber 仍会被正常唤醒（`hook.hpp:172`），只有新调用走原生阻塞。

**失败路径与限制**
- 关闭 `close/shutdown/fcntl/dup/dup2` 一律被拒（`hook.cpp:1793-1796`）；
- `SetAllOpsEnabled` 会保留不可关闭位（`hook.cpp:1773-1781` 的 `NonDisableableMask`）；
- `*_f` 版本不 park，会真阻塞整个 M（`hook.hpp` 对 `read_f` 的说明）。

**典型误用**：生产路径上 `SetOpEnabled(op,false)` 后忘了恢复（Q05 的错误答案之一）；或以为 Hook 是每连接独立的（它是进程级）。

### 2.4 HTTP/1：解析器与 pipelining

**职责**：增量解析请求/响应、把同一 `recv` 里的多条请求逐条处理。

**关键数据结构**

| 字段 | 位置 | 说明 |
|---|---|---|
| `m_buffer` | `http.cpp` Parser 内 | 累积字节；`feed_impl` 从头解析，完成后 `erase(0, consumed)`（`:589`） |
| `m_limits.max_body_bytes` | `http.cpp:583` | body 上限，超限 `ParseState::Error` |
| `m_expected_body` / `m_chunk_remaining` | `http.cpp:389` 的 `reset()` | 增量状态 |
| `m_response_close_delimited` | `http.cpp:389`、`:392-406` | 响应无长度时按 EOF 定界，`finish()` 才收尾 |

**`feed_impl` 逐步拆**（`http.cpp:410`）

1. 已完成直接返回 `Complete`（`:411`）。
2. 溢出检查后把新字节追加进 `m_buffer`。
3. 解析起始行与头部；重复 `Content-Length` 且不一致 → 错误（`:505-507`）。
4. 无 body（HEAD 响应、204/304、chunked 头）→ 直接交付（`:526`）。
5. 有 `Content-Length` → 等到 `body_start + content_length`（`:583-585`）。
6. 完成后：body `assign` 给 request/response（`:587`），`erase(0, consumed)`（`:589`），置 `m_finished`。

**`take_remaining`**（`:409`）：`result.swap(m_buffer); reset(); return result;`——三步都不能省：`reset()` 清增量状态，否则下一条请求会带着上一条的 `m_expected_body`。

**状态机**：`NeedMore → Complete | Error`；`finish()` 额外处理 EOF 定界（`:392-406`）。

**失败路径与限制**
- 解析错误时 `serve_connection` 直接 `return`（`:1468`），**不回 400**；
- `http.hpp:247` 仍写着「`consumed` 之前的字节已交付」，这是**过期注释**：实际 `consumed = m_buffer.size()`（`http.cpp:404`），返回的是本次喂入字节数；
- 请求体大于 `max_body_bytes` → 错误而不是 413（413 在 `StaticFiles` 的文件路径上）。

**典型误用**：用 `consumed` 去 `erase` 再解析第二条（Q04 的标准错答）；或 `feed` 一个空串期望拿到下一条。

### 2.5 静态文件：Range / 304 / sendfile

**职责**：零拷贝把文件体发出去，并处理条件请求与单区间 Range。

**关键点**

- `send_file_body`（`http.cpp:829`）：`open(O_RDONLY|O_CLOEXEC)`（`:833`）→ 循环 `socket->send_file(fd,&offset,remaining)`（`:836`）→ 部分写继续、`transferred<=0` 中断（`:838-845`）→ `close`（`:847`）。
- `Socket::send_file`（`socket.cpp:1026`）内部 `send_file_impl`（`:1048`）处理 EINTR/EAGAIN。
- **`sendfile(2)` 没有 `MSG_NOSIGNAL`**（`socket.cpp:41` 注释）：对端关闭后继续发会收到 SIGPIPE。本库的兜底在 TLS 层（`ScopedSigpipeBlock`）与进程级 `SIG_IGN`，**不是** sendfile 自带保护。Q07 的正确答案就在这里。
- `StaticFiles`（`http.cpp:1193` 起）：200/206/304/400/403/413/416、单区间 Range、ETag/Last-Modified/If-Range、HEAD 只回头。
- bento 侧另有一套策略：`kCorkMinFileBytes=16KiB`（`http_server.cpp:73`）、`kInlineFileBytes=16KiB`（`:78`，小文件读进用户态一次 send 而不是 sendfile）、CORK 开关（`:99-105`、`:123-127`）——**只在 `bento::HttpServer` 生效**。

**典型误用**：以为 sendfile 在内核里发就不会有信号（Q07 错答）；HEAD 请求走 sendfile。

### 2.6 ResponseWriter 与 SSE

**职责**：chunked 写出与事件流；`ResponseWriter` **没有用户态缓冲**。

**关键点**

- `flush()`（`http.cpp:813`）只返回 `m_socket && m_state==HeadersCommitted`——**不写任何字节、不调任何系统调用**；叫 flush 是历史命名（`:809-812` 注释，Q11）。
- `start()`（`:762`）强制 chunked 并**丢弃 `Response::file`**（`:767`）：chunked 路径没有 sendfile。
- `close()`（`:814`）写 `0\r\n\r\n` 终止块。
- SSE：`Event::encode` 输出 `id/event/data/retry`（`sse.cpp:130`，`retry:` 在 `:149-150`）；`Parser` 是有状态单消费者，并发 `feed` 必须调用方加锁（`sse.hpp` 说明）；`Stream::send`（`sse.cpp:653`）与心跳 `tick`（`:702`）。

**典型误用**：`flush()` 之后以为客户端能立刻收到（Q11）；并发调 `Parser::feed`。

### 2.7 HTTP/2：状态机与两道闸

**职责**：多路复用、HPACK、流状态机；不做 HTTP/3。

**流状态机**（`stream_state.cpp`）

```
            SendHeaders/ReceiveHeaders        本地 END_STREAM
open ───────────────────────────────▶ half-closed(local) ──┐
  │                                        ▲                 │ 对端 END_STREAM
  │ 对端 END_STREAM                        │                 ▼
  ▼                                        │              closed
half-closed(remote) ───────────────────────┘
```

| 事件 | 行号 | 语义 |
|---|---|---|
| `RefreshFromEndFlags` | `:49` | 由 `local_end`/`remote_end` 两个 bool 重算七状态（**不是直接赋值**） |
| `SendHeaders(end)` | `:71` | `kClosed`→`kStreamClosed`（`:73`）；非法转移 `kInvalidTransition` 且不改字段 |
| `SendData` | `:142` | 先 `CanSendData()`（`:147`），否则 `kInvalidTransition` |
| `ReceiveHeaders` | `:191` | 对端方向转移 |
| `ReceiveData` | `:229` | 同上；`kClosed` 后一律 `kStreamClosed`（`:231`） |

**两道 header 上限**

| 闸 | 行号 | 防什么 |
|---|---|---|
| `max_header_block_size`（64 KiB，`connection.hpp:90`） | `connection.cpp:985`、`:1021`（CONTINUATION 边拼边累计）、`:1120`（**单片 HEADERS 独立检查**） | CONTINUATION 泛滥 / 单片 10MB HEADERS |
| `max_header_list_size`（64 KiB，`connection.hpp:97`） | `connection.cpp:1141` | HPACK 解压炸弹（每项按 `name+value+32` 计） |

**并发流超限的顺序**（Q13）：`connection.cpp:1196-1200`——注释写明「此时 HPACK 已解码完毕，动态表保持同步，可以直接拒绝该流」，随后回 `RST_STREAM(REFUSED_STREAM)`；客户端侧在**分配流号之前**检查对端上限（`:502`）。`OpenStreamCount()`（`:695`）只统计 open + 两个 half-closed。

**失败路径与限制**
- 超限是**连接错误**，不是流错误（`:1141-1145`）；
- **Q18 的前提已过时（2026-11-03 核对源码）**：五个核心 `.cpp`（`huffman/hpack/frame/stream_state/connection`）现在已在 `go2cpp_network` 的源列表里（`cmake/Go2CppModules.cmake:206`、`:220-224`），注释自己写着「此前只在测试目标里编译」——也就是说这个坑已经填了。今天新建目标只要链 `go2cpp_network`；仍**没有** `go2cpp_http2` 这个独立目标，所以 `-lgo2cpp_http2` 永远无效；
- 服务端 + h2 over TLS 全链路**未验证**（缺 nginx/nghttpd 对端）。

**典型误用**：在 `half-closed(remote)` 上以为不能发（Q12）；超限时先丢帧再补解 HPACK（Q13）。

### 2.8 TLS

**职责**：客户端 TLS 封装；**默认校验**，**写路径零信号系统调用**。

**关键字段**（`tls_transport.hpp`）

| 字段 | 行号 | 默认 |
|---|---|---|
| `verify_peer` | `:102`、`:121` | **true** |
| `server_name` | `:101` | 空 = 不设 SNI、不做主机名匹配 |
| `ca_file` / `ca_path` | `:107-109` | 空 = 用默认信任库（尊重 `SSL_CERT_FILE/DIR`，`:22-24`） |

**能力边界**：只有客户端（`tls_transport.hpp:22-26`）；无 `SSL_accept`、无会话复用/0-RTT/ticket。

**典型误用**：把 `verify_peer=false` 带进生产（同时失去链校验与主机名匹配，Q17）；只 `signal(SIGPIPE,SIG_IGN)` 就以为安全（Q16 的错答——真正的坑是 pending 信号在恢复掩码瞬间投递）。

### 2.9 WebSocket

**职责**：RFC 6455 帧/消息；**不拥有 fd**。

**关键点**

- `Accept`（`websocket.cpp:406`）/`AcceptWithPending`（`:411`）/`Connect`（`:453`）拿的都是 `Socket::Ptr`，WebSocket 不持有 fd 的所有权。
- 析构只 `abort_transport()`（`:554`）：中止读写、唤醒阻塞方，**不 close fd**（Q19）。
- 读方向单线程（`m_read_mutex`）、写方向多线程（`m_write_mutex`）（`websocket.hpp:115`）。
- 配置默认：`max_frame_payload=16MiB`、`max_message_payload=64MiB`、`auto_pong=true`、`validate_utf8=true`（`websocket.hpp:87-91`）；发送超 `max_frame_payload` 自动拆 continuation（`:92-94`）。
- 控制帧：FIN 必须置位、载荷 ≤125（`websocket.cpp:672-674`）。

**典型误用**：以为析构会 close fd（fd 泄漏）；读方向多线程调用 `read_message`。

### 2.10 HTTP/3：picoquic 薄封装

**职责**：在 picoquic 上做 HTTP/3 客户端；不自研 QUIC。

**关键点**

- `http3::Client::Connect`（`client.cpp:1030`）**只建对象并生成 ClientHello，不做握手**（`client.hpp:135-136`）；握手在首次 `Send/Receive/Poll` 推进。
- `PicoquicConnection::Connect`（`picoquic_connection.cpp:287`）、`Poll`（`:390`）、`Close`（`:558`）是唯一的 picoquic 接触面。
- 能力边界：默认 OFF、无 ctest、只有客户端、无 0-RTT/push/迁移、QPACK 只用静态表（`docs/quic.md:127`）。
- **诚实标注**：QUIC vs TCP 的弱网实测只存在于提交信息与 `/tmp`，没归档进 `docs/reports`（Q15 要求说出这一点）。

**典型误用**：以为 `Connect` 成功 = 对端可达；以为 HTTP/3 一定比 HTTP/2 快（无丢包低 RTT 下用户态每包处理与更大头部更贵）。

### 2.11 MQTT / CoAP / SOCKS5：它们在 bento 里

**职责**：三个应用层协议，**不在 `go2cpp::network` 命名空间**。

**关键点**

- 实现：`src/bento/net/mqtt.cpp`（1380 行）、`coap.cpp`（976 行）、`socks5.cpp`（1280 行）；头文件在 `include/go2cpp/bento/net/`。
- 需要 `GO2CPP_WITH_BENTO=ON`（`cmake/Go2CppBento.cmake:17`，默认 OFF）。
- `network/` 只管 Socket/IO/Hook/HTTP1/HTTP2/TLS/WS/SSE/QUIC 薄封装（Q20）。
- `bento::HttpServer` 也在这里（`src/bento/net/http_server.cpp`），所以 pipeline 三策略的归属别记错。

**典型误用**：在 `src/network/` 里翻 MQTT；或忘了开 `GO2CPP_WITH_BENTO` 就链接。

### 2.12 endian：大端是线上格式

**职责**：Bento 与部分网络协议都是大端；这里给 constexpr 的字节序工具。

**关键点**

- `byteswap16/32/64`（`endian.hpp:40/45/53`）纯位运算、可 constexpr。
- `host_to_be16/32/64`（`:65/79/86`）用条件编译分支，运行期无分支。
- `ReadBE16/32/64`（`:114/129/144`）与 `WriteBE16/32/64`（`:155/161/169`）是**按字节读写**，不依赖对齐与主机序。
- 对应测试：`tests/network/test_endian_be.cpp`。

**典型误用**：把 `struct` 直接 `memcpy` 上线（对齐/填充/主机序三重坑）；用 `ntohl` 处理 64 位。

---

## 附：读完之后的自检清单

1. 能否不看文档画出「Fiber 调 recv → park → epoll 就绪 → wake_io_batch → resume」的完整链路，并指出每一步的锁？
2. 能否解释 `EPOLLONESHOT` + `EPOLLET` 为什么要求「每次重 arm」且「必须读到 EAGAIN」？
3. 能否说清 `Socket::close()` 如何等到在途操作归零，以及 `fd()` 为什么不能跨操作持有？
4. 能否指出 `consumed` 与 `take_remaining()` 的区别，以及 pipeline 在 `http::Server` 里到底慢在哪？
5. 能否解释 TLS 写路径的三级快路径，以及「只恢复掩码」为什么会杀进程？
6. 能否说出 HTTP/2 的两道 header 上限分别防什么，以及为什么单片 HEADERS 要单独检查？
7. 能否指出 MQTT/CoAP/SOCKS5 与 pipeline 优化分别属于哪个模块、哪个 CMake 开关？
8. 能否在 10 分钟内用 `tests/network/test_http_e2e.cpp` 与 `tests/network/bench_net_batch.cpp` 跑一次并报出条件？

## 附-2：本文明确「未实测」的地方

- **所有吞吐/延迟数字**：包括 pipeline 72.9k、8 连接 239.5k、跨线程唤醒 54–56 µs——它们都只出现在历史报告（`docs/reports/asm-http-e2e.md`、`user/docs/02-network.md:60`）里，本文写作时**没有复跑**，因此只作为「报告称」引用，不作为结论。
- **跨引擎对比口径（Q09）**：`0.45–0.65× nginx` 与同口径 `1.02–2.97×` 的差异是**测量方法**（旧口径 nginx 未绑核 + 负载污染），不是优化幅度；两个数字都出自历史提交/报告，本文未复跑。
- **`sendfile` 的 strace 证据**（`docs/reports/asm-http-e2e.md:100`）：未复跑。
- **TLS 信号系统调用计数**（`docs/reports/asm-tls.md:151-162` 的 15 次 vs 40 万次）：未复跑；本文只描述代码里的三级快路径。
- **64 条带哈希碰撞在极端 fd 分布下的表现**：未压测。
- **h2 服务端 + h2 over TLS 全链路**：仓库自述未验证；本文同样未验证。
- **QUIC vs TCP 弱网对照**：证据只在提交信息与 `/tmp`，未归档；本文不引用具体倍数。

要复跑时最省事的三个入口：`tests/network/test_http_e2e.cpp`（协议正确性）、`tests/network/bench_net_batch.cpp`（批量 socket）、`tests/bento/bench_weaknet.cpp`（弱网矩阵，需要 `GO2CPP_WITH_BENTO=ON`）。
