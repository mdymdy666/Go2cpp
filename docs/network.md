# 网络模块使用说明

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

`http::Parser` 增量解析 HTTP/1.x，支持分片头、Content-Length、严格的单一 `chunked`、chunk 扩展、trailer 和流水线剩余字节；默认头上限 64 KiB、body 上限 64 MiB、trailer 上限 16 KiB，可通过 `ParserLimits` 调整。重复但不一致的 Content-Length、Content-Length 与 chunked 同时出现、未知 Transfer-Encoding、非法头、非法 chunk 和超限输入会返回 `ParseState::Error`。1xx、204、304 以及通过 `set_response_to_head(true)` 标记的 HEAD 响应不会等待 body。模块不实现 HTTP/2、HTTP/3、WebSocket upgrade 和 Expect:100；需要这些协议时应注入独立 ParserBackend。

`http::ResponseWriter` 提供有限状态的 HTTP/1.1 chunked 输出：`Idle -> start() -> write()/flush() -> close()`。它适合 SSE、大文件和业务流式响应，所有 Socket 短写都会由 `SocketStream::write_all` 处理；重复 start、close 后 write、未 start 的 write 都返回失败。一个 writer 只能由一个线程或 Fiber 持有，业务需要并发发送时必须在上层串行化。

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

客户端提供 `http::Client::get/post/head` 以及 `http::Get/Post/Head` 便捷函数；每个 Client 请求独占连接，超时覆盖 connect、send、recv，并按单个请求的绝对 deadline 计算剩余时间，慢速逐字节响应不会为每次 recv 重新获得完整超时。DNS 目前通过 `getaddrinfo` 同步完成：在 Fiber 中会占用当前 M/worker，deadline 不能中断内核解析；高并发或需要取消的场景应预解析地址，或注入独立的有界异步 Resolver/解析线程池，避免在持有业务锁时解析。服务端 `http::Server::serve_connection` 对单连接串行处理，路由回调在锁外执行；生产程序应让 accept、IOManager 和业务 worker 分层，并在 handler 中避免持有线程锁等待网络。

## 错误与限制

Socket 原始调用返回 `SocketResult` 并携带 errno，`SocketStream` 的负返回值为负 errno；短读和短写是正常情况。解析器 body 仍是有限内存缓冲，不能把它用于不受限的大文件；大文件、SSE、multipart 应使用 `ResponseWriter` 并自行设置业务级上限和取消策略。DNS 目前通过 `getaddrinfo` 同步完成，若需要 Fiber 可取消 DNS，应注入异步 Resolver。`sendmsg` 默认加 `MSG_NOSIGNAL`，避免单次写入触发 SIGPIPE 终止进程，但其他裸 fd 写入仍需由调用方遵守平台信号策略。
