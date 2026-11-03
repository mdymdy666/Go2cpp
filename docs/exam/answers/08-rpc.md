# 08 rpc 答卷（5 题）

> 环境：WSL2 / 12 vCPU。RPC 默认 ON；`.build-bento-wsl` 有 rpc 测试二进制。

## Q01

**答**：宏在服务类所在命名空间展开成 `RpcMethods(Service*)` 重载；`AddService` 里 `RpcMethods(service)` 是**无限定调用**，靠 ADL 只查实参类型的关联命名空间——全局命名空间不在其中。写错位置时找不到精确重载 → 匹配兜底 `RpcMethods(...)` → `NoMethods` → `static_assert` 报「no rpc method table」，不是链接错误。实际容量是 **12**（`ENTRY_12` + `PICK` 的 12 个占位），注释里的「最多 8 个方法」已过时；第 13 个方法在预处理阶段报参数个数错误。其它约束：方法名必须是合法标识符（`#MethodName` 做线上名）、不能直接注册重载（`&Service::M` 有歧义）、服务类型必须非 const、自定义线上名要手写 `Method<&Service::M>("Other")` + 显式 Register；注册表在 `Listen` 时 `Freeze()`，之后再 `Register` 返回 false。

**代码证据**：
```text
include/go2cpp/rpc/registry.hpp:25-27  * 宏展开成一个 `RpcMethods(UserService*)` 重载，`AddService` 用**无限定调用**找到它（ADL）。
include/go2cpp/rpc/registry.hpp:476  inline detail::NoMethods RpcMethods(...) { return {}; }
include/go2cpp/rpc/registry.hpp:510-512  bool AddService(...) { static_assert(!std::is_const_v<Service>, ...); auto table = RpcMethods(service);
include/go2cpp/rpc/registry.hpp:592  * 用法：在服务类型所在命名空间（或全局）展开一次，最多 8 个方法：
include/go2cpp/rpc/registry.hpp:651  #define GO2CPP_RPC_DETAIL_ENTRY_12(S, a, b, c, d, e, f, g, h, i, j, k, l)
```

**自证**：`grep -n "ENTRY_12\|PICK" include/go2cpp/rpc/registry.hpp` → 651/659/662（实际 12）；`sed -n '592p'` 显示注释「最多 8 个方法」与实现矛盾。

**我没答出的**：没有写「宏放全局 + 13 个方法」的编译探针贴报错。

## Q02

**答**：超时是**客户端单侧**决定：`Wait` 超时后 `RemovePending` + `SendCancel`，返回 `kDeadlineExceeded`；服务端普通 handler 继续跑完，结果写回时客户端已无等待者，`HandleResponse` 找不到 pending 直接丢弃。`Cancel` 只对流式 handler 有意义（`ServerStream::cancelled()`），普通 handler 没有入口、也不检查取消标志。同连接排队：普通 handler 在 **reader 线程**直接执行（`Process(request)`），后续 `kRequest` 的 `Read`/`Process` 都排在它后面，`Ping` 也在同一 reader 线程，所以慢 handler 会同时抬高心跳/超时。`Stop()` 死锁条件：在 handler（reader 线程）里调 `Server::Stop()` → 对当前连接 `Join()` 自己 → 死锁/`std::system_error`。

**代码证据**：
```text
src/rpc/server.cpp:342  void ServerConnection::ReaderLoop() { while (!m_closed...) {
src/rpc/server.cpp:401  request->cancelled = std::make_shared<std::atomic<bool>>(false);
src/rpc/server.cpp:478  void ServerConnection::Process(const std::shared_ptr<PendingRequest>& request) {
src/rpc/client.cpp:275  Status Connection::Wait(const std::shared_ptr<CallState>& state, std::chrono::milliseconds timeout)
src/rpc/client.cpp:480  bool Connection::HandleResponse(const frame::Frame& frame_value) { ... // 找不到 pending 就丢弃
```

**自证**：代码路径核对；未跑 `go2cpp_rpc_test` 的 5 ms 超时 + 恢复用例。

**我没答出的**：没有实测「慢 handler 阻塞 Ping ~295ms / 另起连接立刻返回」。

## Q03

**答**：没有协议级流控：帧格式里没有 window/credit/ACK；服务端 `SendStreamData` 只做编码 + `SendPayload`（`m_write_mutex` 下 `WriteAll`），不阻塞等客户端读，唯一阻塞点是 TCP 发送缓冲满。真正的背压位置在**客户端接收队列**（默认 `max_stream_frames=1024`、`max_stream_bytes=16 MiB`）：`frames.size() >= max_frames` 或 `buffered_bytes + payload > max_bytes` → `ended=true`、`kResourceExhausted`、`RemoveStream` + `SendCancel`。文档的「64 帧背压」来自测试里显式设置 `options.max_stream_frames=64`，不是默认值；「服务端 Send 会阻塞」不成立。影响：慢消费者不会让服务端降速，只会让客户端丢流；要真背压必须在应用层做 ack/credit。

**代码证据**：
```text
include/go2cpp/rpc/client.hpp:133-135  // 流缓冲上限（字节）。 max_stream_bytes = 16u * 1024u * 1024u;
src/rpc/client.cpp:532-556  HandleStreamData：frames >= max_frames 或 buffered+payload > max_bytes → kResourceExhausted + SendCancel
src/rpc/server.cpp:300  bool ServerConnection::SendPayload(FrameType type, std::uint16_t flags, ...)  // 题面写 296，实际 300（本卷埋点）
src/rpc/server.cpp:327  bool ServerConnection::SendStreamData(...) { ... return SendPayload(FrameType::kStreamData, 0, payload);
tests/rpc/test_rpc_stream.cpp:128  // 背压：客户端缓冲超限后流以 RESOURCE_EXHAUSTED 结束
```
> **题面错误**：Q03 的 `src/rpc/server.cpp:296-318` 实际是 `Finished()` 与 `SendPayload` 注释区；`SendPayload` 定义在 **300**、`SendStreamData` 在 **327**（本卷埋点，偏移 4 行）。

**自证**：`grep -n "SendPayload" src/rpc/server.cpp` → 121/300/324/331/339；`sed -n '296,301p'` 显示 Finished 后才是 SendPayload。

**我没答出的**：没有把 `max_stream_frames=8` + 慢消费者探针跑出 `kResourceExhausted`。

## Q04

**答**：WS 写侧约定：`WriteAll(data,size)` 把这段字节作为**一条**二进制消息；上层每次写一整帧，所以正常路径「一条消息 = 一帧」。但读侧不保证：`Read(buf,size)` 在 `size` 小于消息剩余长度时只返回一部分；调用方缓冲小、或未来一次写入多帧时，消息边界与帧边界不再一一对应。TCP 更是纯字节流（可能半帧/多帧），两者都必须靠 `FrameCodec` 的半包/粘包状态机。额外边界：文本消息也按字节流接受；Close 时 `read_message` 返回 Closed → `Read` 返回 0（EOF）；`max_message_payload` 默认 64 MiB > RPC 的 4 MiB。线程约束：`Read` 只允许一个线程；`WriteAll` 由上层 `m_write_mutex` 串行化。

**代码证据**：
```text
include/go2cpp/rpc/websocket_transport.hpp:8-9  * RPC 的 Transport 接口是**字节流**，而 WebSocket 是**消息流**。适配规则很直接：
include/go2cpp/rpc/websocket_transport.hpp:121  std::ptrdiff_t Read(void* data, std::size_t size, Status* status) override {
src/rpc/transport.cpp:274  TcpTransport::Read 直接 recv（一次拿多少算多少）
src/rpc/client.cpp:440  FrameCodec::Feed 处理半包/粘包
```

**自证**：代码核对；未跑 `go2cpp_rpc_websocket_test`。

**我没答出的**：没有用 1 字节 Read 缓冲验证 FrameCodec 分多次才吐一帧。

## Q05

**答**：当前工作树的事实：RPC 只有 `TcpTransport` 与 `WebSocketTransport` 两种实现；`src/rpc/` 只有 `client/protocol/registry/server/status/transport` 六个 `.cpp`，没有 `transport_picoquic.cpp`；`tests/rpc/` 只有 `bench_rpc/test_rpc/test_rpc_protocol/test_rpc_stream/test_rpc_websocket`，没有 `test_rpc_picoquic.cpp`；`CMakeLists.txt:92-94` 只有 `include(cmake/Go2CppRpc.cmake)` 与一行 `# RPC over picoquic…：见` 的**悬空注释**，没有 include 对应 cmake。但 `.build-bento-wsl` 里仍有 `go2cpp_rpc_picoquic_test`、`go2cpp_rpc_quic_test`、`libgo2cpp_rpc_picoquic.a`、`libgo2cpp_rpc_quic.a`——构建目录是**旧配置残留**，`ctest -N` 读构建目录不是源码。历史坑：`docs/testing.md` 记录该测试 `mux_reused=0`、256 KiB 往返、尾部 **double free**，不是「已修好」。

**代码证据**：
```text
CMakeLists.txt:91-94  include(cmake/Go2CppRpc.cmake) ... # RPC over picoquic（QUIC 传输，GO2CPP_WITH_PICOQUIC 默认 OFF）：见
ls src/rpc → client.cpp protocol.cpp registry.cpp server.cpp status.cpp transport.cpp
ls .build-bento-wsl | grep rpc → go2cpp_rpc_picoquic_test / go2cpp_rpc_quic_test / libgo2cpp_rpc_picoquic.a
grep -rn "transport_picoquic" --exclude-dir=".build*" . → 只命中 docs/reports/reuse-audit.md 与 exam 题面，无源码
```

**自证**：上述 4 条命令均为本轮实跑；`grep` 无源码命中，`ls` 有旧二进制。

**我没答出的**：没有跑旧 `go2cpp_rpc_picoquic_test --help` 证明二进制仍能启动（担心触发旧 double free）。
