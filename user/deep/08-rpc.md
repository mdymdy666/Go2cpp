# RPC（rpc）· 深入学习

> 本文按「总—分」写。所有 `文件:行` 都用 `sed -n` 复核；性能只写「本机实测（命令+环境+轮数）」，
> 没测过就写「未实测」。特别注意：`user/docs/08-rpc.md` 的流式背压段落与源码不一致，本文以源码为准。

---

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 入口点清单

| 入口（类/函数/宏） | 文件:行 | 谁调用它 | 触发什么 | 返回什么 |
|---|---|---|---|---|
| `Server::Server(ServerOptions)` | `include/go2cpp/rpc/server.hpp:147` | 应用 | 建注册表/impl；不监听 | 对象 |
| `Server::Register<Service>(name, svc)` | `server.hpp:159-170` | 应用（Listen 前） | 调 `ServiceRegistry::AddService`，ADL 找 `RpcMethods` | bool（重名/非法/空表 false） |
| `Server::Listen(address/Endpoint)` | `server.hpp:182/185` | 应用 | 注册表 `Freeze()`、绑监听、起 accept 线程 | bool |
| `Server::AddConnection(Transport)` | `server.hpp:192` | 应用（WS 升级后） | 接管已建连接，起 reader/worker | bool |
| `Server::Stop()` | `server.hpp:195` | 应用 | 关监听、`shutdown` 连接、join 全部线程 | void，幂等 |
| `GO2CPP_RPC_SERVICE(Service, M...)` | `include/go2cpp/rpc/registry.hpp:671-675` | 服务类命名空间 | 生成 `inline auto RpcMethods(Service*)`，返回 `Method<&Service::M>("#M")` 的 tuple | 编译期方法表 |
| `Client::Client(address/Transport, opts)` | `include/go2cpp/rpc/client.hpp:358/365` | 应用 | 建 TCP 或接管 Transport；起读线程 | 对象；失败 `connected()==false` |
| `Client::Call<Result>(method, args...)` | `client.hpp:393-416` | 应用 | 组请求 → 写 → 等响应 → 反序列化 | `Result`，失败抛 `RpcError` |
| `Client::TryCall<Result>(method, out, args...)` | `client.hpp:418-450` | 应用 | 同上，但不抛异常 | `Status` |
| `Client::CallStatus(method, args...)` | `client.hpp:452-472` | 应用 | 无返回值调用 | `Status` |
| `Client::AsyncCall<Result>(method, args...)` | `client.hpp:474-493` | 应用 | 发起后立即返回 `PendingCall` | `PendingCall<Result>` |
| `PendingCall::Wait(out, timeout)` | `client.hpp:293-318` | 应用 | 等完成/超时；结果只能取一次 | `Status` |
| `PendingCall::Cancel()` | `client.hpp:280-290` | 应用 | 本地唤醒 `kCancelled` + 发 `kCancel` | bool |
| `Client::OpenStream(method, args...)` | `client.hpp:495-504` | 应用 | 发流式请求，登记 `StreamState` | `ClientStream` |
| `ClientStream::Next<T>(out, status, timeout)` | `client.hpp:195-227` | 应用 | 从流缓冲取一条；必要时等 | bool |
| `ClientStream::Finish(timeout)` | `client.hpp:229` | 应用 | 等 `StreamEnd` / 连接关闭 | `Status` |
| `ClientStream::Cancel()` | `client.hpp:232` | 应用 | 本地结束 + 发 `kCancel` | bool |
| `Client::Ping()` | `client.hpp:506` | 应用 | 发 `kPing`，等 `kPong` | `Status` |
| `ServerStream::Send<T>(v)` | `server.hpp:78-92` | 服务端 handler | 序列化 + `SendStreamData` | bool |
| `ServerStream::SendSerialized` | `server.hpp:94` / `server.cpp:212-226` | 服务端 handler | 同上，字节版 | bool |
| `ServerStream::Finish(status)` | `server.hpp:101` / `server.cpp:228-242` | 服务端 handler | 发 `kStreamEnd` | `Status` |
| `ServerStream::cancelled()` | `server.hpp:104` | 服务端 handler | 读 `kCancel` 置的原子标志 | bool |
| `TcpTransport::Connect/Listen` | `include/go2cpp/rpc/transport.hpp:109+` | Client / Server | TCP 连接/监听；`TCP_NODELAY` | 对象/`unique_ptr` |
| `WebSocketTransport::WebSocketTransport(ws)` | `include/go2cpp/rpc/websocket_transport.hpp:65+` | 应用（HTTP 升级后） | 把一条 WS 连接适配成 `Transport` | 对象 |
| `ServiceRegistry::Find(full_name)` | `include/go2cpp/rpc/registry.hpp:494+` | 每次请求 | 无锁开放寻址查找 | `const BoundMethod*` |

编译/平台：
- `-DGO2CPP_WITH_RPC=ON`（默认 ON）；测试开关 `GO2CPP_BUILD_TESTS`。
- `TcpTransport` 仅 POSIX；`WebSocketTransport` 依赖 `net::WebSocket`。
- **当前源码没有 RPC-over-QUIC**：`CMakeLists.txt:92-94` 只剩悬空注释，`src/rpc/` 无
  `transport_picoquic.cpp`（历史与坑见 §2.8）。

### 1.2 总架构图

```
Server
  accept 线程（ServerImpl::AcceptLoop）
    └─ TcpListener::Accept → ServerConnection
         ├─ reader 线程：FrameCodec::Feed → HandleFrame
         │    ├─ kRequest(非流式) → Process()  ← 直接在本线程跑 handler
         │    ├─ kRequest(流式)   → m_queue.push → worker
         │    ├─ kCancel          → m_cancels[call_id].store(true)
         │    ├─ kPing            → 立即回 kPong
         │    └─ 其它             → 协议错误/忽略
         └─ worker 线程：m_queue.pop → Process()（只有流式）
               └─ StreamSenderImpl → SendStreamData → SendPayload
                                                   └─ m_write_mutex → Transport::WriteAll

Client
  调用线程：Call/TryCall/AsyncCall/OpenStream
    ├─ PackArguments（每个参数 varint 长度 + Serialize）
    ├─ BeginCall(call_id)：登记 m_pending / m_streams
    ├─ SendRequest/SendCancel/SendPing（m_write_mutex 串行化写）
    └─ Wait：100µs 自旋 → condvar；超时 RemovePending + SendCancel
  读线程（每条连接一条）：
    FrameCodec::Feed → Dispatch
      ├─ kResponse   → HandleResponse（找不到 pending 就丢）
      ├─ kStreamData → HandleStreamData（入 StreamState 队列 / 超限 Cancel）
      ├─ kStreamEnd  → HandleStreamEnd
      └─ kPong       → HandlePong（唤醒 Ping）

帧层：FrameCodec（复用 go2cpp::frame）
  magic "G2RP" | ver 1 | 12B 头 | payload | CRC-32/IEEE
  type: 1 Request, 2 Response, 3 StreamData, 4 StreamEnd, 5 Cancel, 6 Ping, 7 Pong
  flags bit0 = streaming
```

边界与锁：
- **每条连接 2 条线程**：reader + worker；流式 handler 串行。
- 普通 handler 在 reader 线程执行 → 慢 handler 阻塞同连接后续请求（含 Ping）。
- 注册表 `Freeze()` 后只读，查找无锁；注册路径有 mutex。
- 每条连接一把 `m_write_mutex` 串行化写；`PendingCall` 只能 `Wait` 一次。
- `ClientStream` 的缓冲在客户端；没有协议级窗口/credit。

### 1.3 复杂场景一：一次 streaming 调用（建立→数据→半关闭→收尾→超时恢复）

1. 应用 `auto stream = client.OpenStream("User.Watch", req)`（`client.hpp:495-504`）→
   `OpenStreamRaw`（`client.hpp:518`）。
2. `PackArguments`（`client.hpp:112-120`）把 `req` 序列化；`BeginCall(..., streaming=true, ...)`
   （`client.hpp:510-512`）分配 `call_id`、创建 `StreamState`、登记 `m_streams`，
   然后 `SendRequest`（`client.cpp:232-241`）。
3. 服务端 reader 线程 `FrameCodec::Feed` 解出 `kRequest`，`HandleRequest`
   （`server.cpp:391-425`）看到 `streaming=true` → 创建 `PendingRequest`，把
   `shared_ptr<atomic<bool>> cancelled` 登记进 `m_cancels[call_id]`，再入 `m_queue`
   （`server.cpp:411-424`）。
4. worker 线程 `WorkerLoop`（`server.cpp:454-476`）`pop` 请求 → `Process`
   （`server.cpp:478-523`）；`bound->binding.streaming` 必须匹配，否则回 `kStreamEnd` +
   `kInvalidArgument`（`server.cpp:494-507`）。
5. `Process` 创建 `StreamSenderImpl` + `ServerStream`，调用 handler
   `invoke(service, ctx, args, result, &stream)`（`server.cpp:512-518`）。
6. handler 调 `stream.Send(v)` → `ServerStream::Send` → `SendSerialized` →
   `StreamSenderImpl::SendSerialized`（`server.cpp:212-226`）：加 `m_mutex`，检查
   `m_finished`，`m_sequence++`，`SendStreamData`。
7. `ServerConnection::SendStreamData`（`server.cpp:327-332`）编码 `kStreamData`（call_id +
   seq + len + payload），`SendPayload`（`server.cpp:296-318`）在 `m_write_mutex` 下
   `WriteAll`。
8. 客户端读线程 `Dispatch`（`client.cpp:451-474`）→ `HandleStreamData`
   （`client.cpp:520-556`）：`FindStream(call_id)`；若队列满/字节超限 →
   `ended=true`、`kResourceExhausted`、`RemoveStream`、`SendCancel`；否则入队并 `notify_all`。
9. 应用 `stream.Next<T>(&out, &status, timeout)`（`client.hpp:195-227`）→
   `NextPayload`（`client.hpp:243`）→ `WaitForStreamFrame`（`client.cpp:353-390`）：
   `cv.wait_for` 直到有帧/结束/超时；反序列化。
10. handler 结束：`stream.Finish(status)` → `StreamSenderImpl::Finish`
    （`server.cpp:228-242`）→ `SendStreamEnd`（`server.cpp:334-340`）；之后 `Process` 末尾
    `EraseCancel(call_id)`（`server.cpp:524`）。
11. 客户端 `HandleStreamEnd`（`client.cpp:557-577`）：置 `ended/end_status`，`notify_all`，
    `RemoveStream`；应用 `stream.Finish()`（`client.cpp:660-670`）返回最终状态。
12. 半关闭/取消路径：应用 `stream.Cancel()`（`client.cpp:674-690`）→ 本地置 `kCancelled`、
    唤醒、发 `kCancel`；服务端 `HandleCancel`（`server.cpp:428-445`）置原子标志；
    handler 通过 `stream.cancelled()` 观察，自行收尾。
13. 超时恢复路径：`Next` 超时返回 `kDeadlineExceeded`，但流仍可继续 `Next`（测试
    `test_rpc_stream.cpp:71` 的 `recv timeout then resume`）；连接不因单次读超时断开。
14. 连接断开：`Read` 返回 0 → reader 退出、`Close()`、`m_queue_cv.notify_all()`；
    worker 退出；客户端读线程 `Close()`；所有 pending/stream 被唤醒为 `kUnavailable`。

失败路径：方法未注册（`kNotFound`）、streaming 标志不匹配（`kInvalidArgument`）、
客户端缓冲超限（`kResourceExhausted` + Cancel）、发送失败（`kUnavailable`）、
服务端 `Stop()`（连接 shutdown，worker/reader 退出）。

### 1.4 复杂场景二：TCP 与 WebSocket 的路径分叉

1. **建立**：TCP 由 `TcpTransport::Connect` 建 socket、设 `TCP_NODELAY`
   （`src/rpc/transport.cpp:51-54`）；WS 由 `net::WebSocket::Connect/Accept` 完成 HTTP 升级，
   再把 `WebSocket` 交给 `WebSocketTransport`（`websocket_transport.hpp:65-83`）。
2. **服务端接入**：TCP 由 `TcpListener::Accept` → `AcceptLoop` → `ServerConnection`；
   WS 由应用在升级后 `server.AddConnection(std::make_unique<WebSocketTransport>(ws))`
   （`server.hpp:192`）。
3. **写路径**：`Client::SendRequest` / `ServerConnection::SendPayload` 都先编码完整帧，
   加 `m_write_mutex`，调 `Transport::WriteAll`。
   - TCP：`TcpTransport::WriteAll` 循环 `send` 直到写完或失败（`transport.cpp:227-272`）。
   - WS：`WebSocketTransport::WriteAll` 把整段字节作为**一条二进制消息** `send_binary`
     （`websocket_transport.hpp:85-117`）。
4. **读路径**：
   - TCP：`TcpTransport::Read` 一次 `recv`，返回内核给多少算多少；EAGAIN 时 `poll` 1s
     （`transport.cpp:274-300`）。
   - WS：`WebSocketTransport::Read` 先消费 `m_pending` 当前消息的剩余字节；读空才
     `read_message()` 取下一条（`websocket_transport.hpp:121-160`）。
5. **帧边界**：两者都不保证一次 `Read` 等于一帧；`FrameCodec::Feed` 负责半包/粘包
   （`client.cpp:440-449`、`server.cpp:348-356`）。WS 的「一条消息 = 一帧」只在上层每次
   `WriteAll` 一整帧时成立，是**约定**不是传输保证。
6. **关闭**：TCP `Close()` 只 `shutdown(SHUT_RDWR)` 唤醒阻塞读，真正 `close` 在析构
   （`transport.hpp:30-31`、`transport.cpp:308-316`）；WS `Close()` 发 Close 帧并标记，
   唤醒读（`websocket_transport.hpp:167-176`）。
7. **错误映射**：TCP `recv==0` → EOF；`ECONNRESET/EPIPE` → `kUnavailable`；WS
   `read_message` 的 `Closed` → `Read` 返回 0，其它错误 → `kUnavailable`。
8. **半包示例**：客户端一次写入 2 帧（TCP 合法；WS 会变成一条消息含 2 帧），
   `Feed` 一次吐出 2 个 `Frame`；客户端缓冲设为 1 字节时，同一帧要多次 `Read` 才拼齐。
9. **Ping/Pong**：RPC 层 `kPing/kPong` 是应用帧，和 WS 协议层 ping/pong 不是一回事；
   WS 的心跳由 `net::WebSocket` 自己处理，RPC 的 `Ping()` 仍走 `kPing`。
10. **失败路径**：WS 握手失败不进入 RPC；文本消息被当字节流接受（不推荐）；单条 WS 消息
    超过 `max_message_payload`（默认 64 MiB）→ 读错误；RPC 帧上限 4 MiB 由两端
    `max_payload_bytes` 决定，不一致会在协议层判 `kDataLoss`。

### 1.5 关键不变量清单

| # | 不变量 | 证据 | 破法后果 |
|---|---|---|---|
| 1 | 普通 handler 在 reader 线程执行 | `server.cpp:401-410` | 慢 handler 阻塞同连接后续请求/Ping |
| 2 | 流式 handler 在单 worker 串行执行 | `server.cpp:454-476` | 一条长流挡住同连接后续流 |
| 3 | 注册表 `Listen` 时冻结，之后只读 | `registry.hpp`（`Freeze`）、`server.cpp:531+` | 动态注册返回 false；改锁会毁掉无锁查找 |
| 4 | `BoundMethod*` 指向注册表内部，生命周期 = Server | `registry.hpp:481-485` | Server 析构后持有 → UAF |
| 5 | 服务对象是裸指针，Server 存活期间不能析构 | `registry.hpp:503-507` | 悬空调用 |
| 6 | 每连接一把写锁，`WriteAll` 必须全写或失败 | `server.cpp:314-315`、`transport.hpp:70-75` | 帧交错/半帧 |
| 7 | `call_id` 单调且 pending 表按 id 分派 | `client.cpp:480-505` | 迟到响应串话 |
| 8 | 超时只清客户端等待项，服务端继续执行 | `client.cpp:288-297`、`server.cpp:478-523` | 误以为取消生效；同连接排队 |
| 9 | 流式缓冲是客户端单侧上限，无协议窗口 | `client.hpp:131-132`、`client.cpp:532-556` | 慢消费者丢流，服务端不降速 |
| 10 | 当前 RPC 只有 TCP/WS 两种 Transport | `src/rpc/`、`CMakeLists.txt:92-94` | 按旧文档找 picoquic 会扑空 |

---

## 第二部分 · 分：逐块细讲

### 2.1 状态码与错误

- `StatusCode`（`include/go2cpp/rpc/status.hpp:36-53`）：16 码对齐 gRPC，值 0..15；
  线上只占 1 字节，越界判协议错误。
- `Status`（68-100）：code + message；成功时 message 为空。
- `RpcError`（124+）：继承 `std::runtime_error`，`Call<Result>` 失败时抛。
- 状态码只能追加在 15 之后；重排会破坏日志/监控/跨版本兼容。

**失败路径**：非法状态码 → `DecodeResponse` 返回 false → 连接断开；错误 message 不脱敏？
本模块只做通用 RPC，业务错误文本由 handler 负责。

### 2.2 协议与帧

- `FrameType`（`protocol.hpp:61-69`）：Request/Response/StreamData/StreamEnd/Cancel/Ping/Pong。
- `RequestMessage`（82-88）：call_id + method + streaming + args。
- `ResponseMessage`（90-95）：call_id + Status + payload。
- `StreamDataMessage`（97-102）：call_id + sequence + payload。
- `StreamEndMessage`（104-108）：call_id + Status。
- 编解码：`EncodeRequest/Response/StreamData/StreamEnd/Cancel/Ping`（115-141），
  `Decode*`（150-162）。
- 帧层复用 `go2cpp::frame::FrameCodec`：magic `G2RP`、version 1、12B 头、CRC-32 帧尾、
  `max_payload_bytes` 两端必须一致。
- `args` 是「每个参数 varint 长度 + Serialize 字节」的顺序拼接；服务端按注册签名拆开并校验
  「子 Buffer 恰好读完」。
- **不传类型名/类型 ID**：方法名路由，Reflect 只负责字段编码；跨语言/跨版本要自己约束。

### 2.3 注册表与方法表宏

- `ServiceRegistry`（`registry.hpp:494+`）：开放寻址哈希（2 的幂、线性探测、FNV-1a 64）、
  `std::deque<BoundMethod>` 保证指针稳定、查找无锁、注册加锁。
- `BoundMethod`（481-485）：`full_name`、`service`、`binding`。
- `MethodBinding`：`name`（短名，指向字面量）、`argc`、`streaming`、`invoke` 函数指针。
- `Method<M>(name)`（`registry.hpp:460-471`）：`static_assert(argc<=255)`，填 `binding.invoke`。
- `GO2CPP_RPC_SERVICE`（671-675）：
  ```cpp
  #define GO2CPP_RPC_DETAIL_ENTRY(ServiceType, MethodName) \
      ::go2cpp::rpc::Method<&ServiceType::MethodName>(#MethodName)
  #define GO2CPP_RPC_SERVICE(ServiceType, ...)                         \
      inline auto RpcMethods(ServiceType*) {                            \
          return std::make_tuple(GO2CPP_RPC_DETAIL_ENTRIES(ServiceType, __VA_ARGS__)); \
      }
  ```
  用 `PICK` 在 `ENTRY_1..ENTRY_12` 间选择；**最多 12 个方法**（注释里的 8 已过时）。
- `RpcMethods(...)` 兜底（476）：返回 `NoMethods`，让 `AddService` 的 `static_assert` 发出
  清晰错误。
- ADL：`AddService` 里 `RpcMethods(service)` 是无限定调用，只查 `Service` 的关联命名空间；
  宏必须写在该命名空间。写全局 → 找不到 → 静态断言。

**失败路径**：重名/非法名/空表返回 false；`Listen` 后注册返回 false；方法名超 64 字节拒绝。

### 2.4 服务端

- `ServerOptions`（`server.hpp:119-140`）：`max_payload_bytes=4MiB`、`checksum=true`、
  `backlog=128`、`accept_poll_ms=20`、`max_connections=1024`。
- `Server`（144-216）：`Register/Listen/AddConnection/Stop/running/port/registry`。
- `ServerImpl`：accept 线程 + 连接表；`Stop()`（`server.cpp:609-625`）关监听 → join accept →
  swap 连接表 → 对每个 `Close()` → `Join()`。
- `ServerConnection`：
  - `ReaderLoop`（342-370）：`Transport::Read` → `FrameCodec::Feed` → `HandleFrame`。
  - `HandleFrame`（372-389）：Request/Cancel/Ping/Pong 分派；反向帧返回 false（断连）。
  - `HandleRequest`（391-426）：登记 cancel、普通直接 `Process`、流式入队。
  - `WorkerLoop`（454-476）：单 worker 串行跑流式。
  - `Process`（478-523）：查表、校验 streaming、调用 handler、回 Response/StreamEnd。
  - `SendPayload`（296-318）：编码 + 写锁 + `WriteAll`。
- `ServerStream`（60-116）：`Send/SendSerialized/Finish/cancelled/valid`；
  `StreamSenderImpl`（`server.cpp:212-242`）负责序号与 finished 状态。

**状态机**：
```
Listening ──Accept──► Connected(reader+worker)
Connected ──Read==0/协议错/Stop──► Closed
Closed ──Join──► 回收
```

**失败路径**：方法未注册、streaming 不匹配、写失败、连接数超限、`Stop` 在 handler 内调用
（自 join 死锁）。

### 2.5 客户端

- `ClientOptions`（`client.hpp:122-138`）：`connect_timeout=1000`、`timeout=5000`、
  `max_payload_bytes=4MiB`、`checksum`、`max_stream_frames=1024`、`max_stream_bytes=16MiB`、
  `tcp_no_delay`。
- `Connection`：读线程 + `m_pending`（call_id→CallState）+ `m_streams`（call_id→StreamState）
  + `m_write_mutex`。
- `Wait`（`client.cpp:275-300`）：100µs 自旋 → condvar；超时 `RemovePending` + `SendCancel` +
  `kDeadlineExceeded`。
- `CancelCall`（304-329）：从 pending 移除、置 `kCancelled`、唤醒、`SendCancel`。
- `HandleResponse`（480-505）：找不到 pending 丢弃；流式请求收到普通 Response 时转
  `StreamEnd`。
- `HandleStreamData`（520-556）：入队；超限 `kResourceExhausted` + Cancel。
- `HandleStreamEnd`（557-577）：置结束、唤醒、移除。
- `PendingCall::Wait` 只能取一次结果（Buffer 被 move 消费）。

**失败路径**：连接失败 `connected()==false`，调用返回 `kInvalidArgument`/`kUnavailable`；
读线程退出唤醒所有等待；`Client` 不做重连/服务发现。

### 2.6 TCP 传输

- `Endpoint::Parse` 只支持 IPv4/域名，不支持 `[::1]:port`（`transport.hpp:55-65`）。
- `TcpTransport::Connect`：`socket` + `connect` + `TCP_NODELAY`（`transport.cpp:51-54`）。
- `WriteAll`（227-272）：循环 `send`，处理 EINTR/EAGAIN，`Close` 后快速失败。
- `Read`（274-300）：`recv`；EAGAIN → `poll` 1s；返回 0 表示 EOF。
- `Close`（308-316）：只 `shutdown`，真正 `close` 在析构；避免 reader 还阻塞时 fd 被复用。
- `TcpListener`：`bind/listen/accept`，`accept` 用 `poll` 等待。

**失败路径**：连接拒绝/超时/对端关闭；`fd` 耗尽；`max_payload_bytes` 不一致。

### 2.7 WebSocket 传输

- `WebSocketTransport`（`websocket_transport.hpp:65-196`）：把 `net::WebSocket` 适配成
  `Transport`。
- `WriteAll`（85-117）：`send_binary(string_view)`，一条消息 = 一次写；失败返回
  `kUnavailable`。
- `Read`（121-160）：先消费 `m_pending`；消息读空才 `read_message`；空消息跳过；
  `Closed` → 返回 0；其它错误 → -1。
- `Close`（167-176）：发 Close 帧并标记。
- `fd()`（178-184）：返回底层 socket fd，仅供诊断；不要拿它做 poll 来决定是否有数据
  （WS 可能已有解码缓冲）。
- 线程：`Read` 只允许一个线程；`WriteAll` 由上层写锁串行化。

**失败路径**：握手失败、消息超限、对端关闭、文本消息（被当字节流接受）。

### 2.8 RPC-over-QUIC 的历史与现状

当前工作树：
- `CMakeLists.txt:92-94`：只有 `# RPC over picoquic（QUIC 传输，GO2CPP_WITH_PICOQUIC 默认 OFF）：见`
  的悬空注释，没有 `include(cmake/Go2CppPicoquic.cmake)`。
- `src/rpc/` 只有 `client/protocol/registry/server/status/transport`；
  `tests/rpc/` 没有 `test_rpc_picoquic.cpp`。
- `.build-bento-wsl/CTestTestfile.cmake:499` 仍登记 `go2cpp_rpc_picoquic_test`，二进制也还在——
  这是**旧构建目录的残留**，不代表源码现状。

历史记录（只作坑的线索，不当现状）：
- `docs/testing.md:869-872`：该测试独立复跑失败特征 `mux_reused=0`、256 KiB 往返、
  尾部 `double free`。
- `docs/reports/reuse-audit.md:257`：T12.2 曾是
  `include/go2cpp/rpc/transport_picoquic.hpp` + `.cpp`（1397 行），客户端复用
  `net::quic::PicoquicConnection`，服务端自写 picoquic 泵。
- 结论：如果重做，应先抽 `PicoquicServer`（多连接、单线程驱动），再写 RPC 适配层；
  不要复活旧文件，也不要相信 `ctest -N`。

---

## 附：本机自测入口（不引用历史数字）

```bash
cmake -S . -B .build-rpc -G Ninja -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_RPC=ON
cmake --build .build-rpc -j"$(nproc)" \
  --target go2cpp_rpc_protocol_test go2cpp_rpc_test go2cpp_rpc_stream_test \
           go2cpp_rpc_websocket_test go2cpp_rpc_bench
ctest --test-dir .build-rpc -R go2cpp_rpc --output-on-failure
./.build-rpc/go2cpp_rpc_bench   # 基准，不进 ctest；数字要带环境/轮数
```

规模可调：`RPC_ITERATIONS`、`RPC_CONCURRENT_TOTAL`、`RPC_BENCH_*`；调小后的数字不能当性能结论。
