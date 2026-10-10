# WebSocket P1 使用说明

P1 提供独立的 RFC 6455 WebSocket 协议层，位于 `go2cpp::net::WebSocket`。它依赖已经建立的 `net::Socket`，可以在普通线程或 IOManager 管理的 Fiber 中使用；协议对象不拥有调度器，也不隐式创建线程。

服务端先用 HTTP Parser 读取 Upgrade 请求，再调用：

```cpp
go2cpp::WebSocketResult result;
auto ws = go2cpp::WebSocket::Accept(socket, request, {}, &result);
if (!ws) {
    // result.error 和 result.message 记录握手失败原因
}
```

如果 HTTP Parser 在同一次 `recv` 中已经读到了 Upgrade 之后的首个 WebSocket 帧，必须把 `parser.take_remaining()` 的结果传给 `AcceptWithPending`，否则不能丢弃这段字节：

```cpp
auto ws = go2cpp::WebSocket::AcceptWithPending(socket, request,
                                                parser.take_remaining(), {}, &result);
```

客户端调用 `WebSocket::Connect(socket, host, target, config, &result)`。握手严格要求 GET、HTTP/1.1、Upgrade/Connection、版本 13 和 16 字节 Base64 key，并验证 `Sec-WebSocket-Accept`。默认不启用扩展、TLS 或 HTTP/2 CONNECT。

建立连接后，`send_text`、`send_binary`、`ping`、`pong` 和 `close` 都返回 `WebSocketResult`；`read_message` 会自动重组分片消息，自动响应 Ping，并将 Close 帧作为 `WebSocketMessage` 返回一次。读方向只能由一个线程或 Fiber 使用，写方向由项目的 Fiber/线程混合 Mutex 串行化；业务代码不能再用外部锁包住可能 park 的 Socket 调用。写入发生部分失败后连接会进入不可复用状态，底层读写等待会收到取消通知。

```cpp
go2cpp::WebSocketMessage message;
for (;;) {
    const auto status = ws->read_message(message);
    if (!status.ok()) break;
    if (message.opcode == go2cpp::WebSocketOpcode::Close) break;
    if (message.text()) {
        ws->send_text("echo: " + message.payload);
    } else {
        ws->send_binary(message.payload);
    }
}
```

解析器会拒绝错误掩码方向、RSV 位、未知 opcode、非最小 126/127 长度编码、非法 64 位长度、控制帧分片或超过 125 字节、非法 Close code/reason、非法 UTF-8 和超出 `max_frame_payload/max_message_payload` 的消息；不可恢复的协议、UTF-8、大小错误分别发送 1002、1007、1009 Close。握手要求 Host，拒绝控制字符、HTTP body 和 Transfer-Encoding。默认帧上限 16 MiB、消息上限 64 MiB；真实服务应按业务调低并设置 `io_timeout`，该超时也覆盖握手写入。

当前明确限制：不支持 permessage-deflate、TLS、HTTP/2 CONNECT、扩展协商、发送端主动分片和跨连接会话恢复。SSE、路由鉴权、限流、审计和服务治理不属于 WebSocket 核心对象，应由后续 NetService 层组合。
