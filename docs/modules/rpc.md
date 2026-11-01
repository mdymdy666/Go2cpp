# rpc：Go 风格 RPC（服务注册 / 调用 / 流式）

> 状态：现行（2026-11-01）。
> 源码：`include/go2cpp/rpc/*.hpp` + `src/rpc/*.cpp`；
> 测试：`tests/rpc/*.cpp`；构建：`cmake/Go2CppRpc.cmake`；
> 性能报告：`/tmp/rpc.md`（含 objdump 证据、下限与 gRPC 对比）。
> 目标消费者：需要「进程间方法调用」但不想引入 gRPC/Protobuf 工具链的服务。

## 1. 定位

一句话：**方法名路由 + Reflect 序列化 + 一条 TCP 连接上的同步/异步/流式调用**。

- 服务注册：`server.Register("User", &service)` 一行；方法表由
  `GO2CPP_RPC_SERVICE(UserService, Get, Create, ...)` 宏在服务命名空间内生成；
- 调用：`client.Call<User>("User.Get", id)`（失败抛 `RpcError`）、
  `client.TryCall("User.Get", &user, id)`（返回 `Status`）、
  `client.AsyncCall<User>(...)`（返回 `PendingCall`，可 `Cancel`）；
- 流式：`client.OpenStream("Chat.Watch", request)`，服务端 handler 拿
  `ServerStream*` 逐条 `Send`；
- 超时：默认 5 s，`Client::set_timeout` 可改；超时返回
  `DEADLINE_EXCEEDED` 并自动发 `Cancel` 帧；
- 取消：`PendingCall::Cancel()` 本地立即唤醒（`CANCELLED`），同时通知服务端；
- 多路复用：多个线程可同时用同一个 `Client`，读线程按 `call_id` 分派响应。

**不做**（刻意划界）：IDL/代码生成、TLS、压缩、服务发现、负载均衡、跨语言、
deadline 传播、metadata/拦截器。这些需要时应在传输层/业务层另做，而不是把
RPC 内核做成半套 gRPC。

与 Go `net/rpc` 的差异：Go 版只有「每连接一个 goroutine + gob + 同步 handler」；
本实现额外提供流式、超时、取消、单连接多路复用，但同样没有 IDL。

## 2. 目录与构建

```text
include/go2cpp/rpc/
  status.hpp               # StatusCode（对齐 gRPC 16 码）/ Status / RpcError
  registry.hpp             # ServiceRegistry / MethodBinding / GO2CPP_RPC_SERVICE
  protocol.hpp             # 帧类型 + 请求/响应/流帧编解码
  transport.hpp            # Transport 抽象 + TcpTransport / TcpListener
  server.hpp               # Server / ServerStream / CallContext
  client.hpp               # Client / PendingCall / ClientStream
  websocket_transport.hpp  # 可选：net::WebSocket 适配器
  rpc.hpp                  # 聚合头
src/rpc/
  status.cpp registry.cpp protocol.cpp transport.cpp server.cpp client.cpp
tests/rpc/
  rpc_test_support.hpp     # 迷你断言 + 测试服务（12 个方法覆盖全部签名形态）
  test_rpc_protocol.cpp    # 注册表/绑定/协议/畸形输入/10 万随机帧
  test_rpc.cpp             # 端到端：基本/错误/超时/取消/并发/1e6 延迟/断连
  test_rpc_stream.cpp      # 流式：顺序/接收超时/取消/背压/多流/断连
  test_rpc_websocket.cpp   # WebSocket 适配器端到端
  bench_rpc.cpp            # 协议层 1e7 / 序列化 / TCP echo 下限 / 1e6 / 前-后
cmake/Go2CppRpc.cmake
```

| 目标 | 类型 | 说明 |
|---|---|---|
| `go2cpp_rpc`（别名 `go2cpp::rpc`） | STATIC | 只链 `go2cpp_frame` + `go2cpp_reflect` + Threads |
| `go2cpp_rpc_protocol_test` | EXECUTABLE | 无网络，ctest 名同名 |
| `go2cpp_rpc_test` | EXECUTABLE | 端到端（1e6 + 1e5，默认满规模） |
| `go2cpp_rpc_stream_test` | EXECUTABLE | 流式 |
| `go2cpp_rpc_websocket_test` | EXECUTABLE | 需要 `go2cpp_network` 目标存在 |
| `go2cpp_rpc_bench` | EXECUTABLE | 性能基准（不注册 ctest，避免拖慢 CI） |

```bash
cmake -S . -B .build-bento-wsl
cmake --build .build-bento-wsl --target \
  go2cpp_rpc_protocol_test go2cpp_rpc_test go2cpp_rpc_stream_test go2cpp_rpc_bench -j
ctest --test-dir .build-bento-wsl -R go2cpp_rpc --output-on-failure
./.build-bento-wsl/go2cpp_rpc_bench
```

开关：`-DGO2CPP_WITH_RPC=OFF` 关闭模块；`-DGO2CPP_RPC_TESTS=OFF` 只关测试。
测试规模可用 `RPC_ITERATIONS` / `RPC_CONCURRENT_TOTAL` / `RPC_BENCH_*` 调整。

## 3. 快速上手

服务定义（普通结构体 + 成员函数，无需继承/注册基类）：

```cpp
struct UserId  { std::int64_t id = 0; };
struct User    { std::int64_t id = 0; std::string name; std::int32_t level = 0; };
struct CreateRequest { std::string name; };

// 字段反射（Go2Cpp Reflect 的字段表）。
template <> struct go2cpp::reflect::FieldList<UserId> { /* GO2CPP_REFLECT_FIELD ... */ };
template <> struct go2cpp::reflect::FieldList<User>   { /* ... */ };
template <> struct go2cpp::reflect::FieldList<CreateRequest> { /* ... */ };

class UserService {
 public:
  User Get(const UserId& id);                                   // 值返回
  Status Create(const CreateRequest& request, User* out);       // out 参数
  Status Remove(const UserId& id);                              // 仅状态
  void Touch(const UserId& id);                                 // 仅动作
  Status Watch(const WatchRequest& request, rpc::ServerStream* stream);  // 流式
};

// 在 UserService 所在命名空间展开一次（ADL 找到方法表）。
GO2CPP_RPC_SERVICE(UserService, Get, Create, Remove, Touch, Watch)
```

服务端：

```cpp
rpc::Server server;
UserService service;
server.Register("User", &service);   // 完整方法名 "User.Get"/"User.Create"/...
server.Listen(":8080");              // 后台 accept 线程，立即返回
// ...
server.Stop();                       // 关监听、shutdown 连接、join 全部线程
```

客户端：

```cpp
rpc::Client client("127.0.0.1:8080");

User user = client.Call<User>("User.Get", UserId{42});          // 失败抛 RpcError
Status status = client.TryCall("User.Get", &user, UserId{42});  // 无异常
client.CallStatus("User.Remove", UserId{42});                   // 无返回值

auto pending = client.AsyncCall<User>("User.Get", UserId{7});   // 异步
pending.Cancel();                                               // 取消

auto stream = client.OpenStream("User.Watch", WatchRequest{1000, 0});
ChatMessage message;
rpc::Status stream_status;
while (stream.Next(&message, &stream_status)) { /* ... */ }
// stream_status.ok() == true 表示流正常结束
```

## 4. 协议

帧层复用 `go2cpp::frame::FrameCodec`：magic `"G2RP"`、version 1、12 字节帧头、
CRC-32/IEEE 帧尾、`max_payload` 默认 4 MiB（服务端/客户端必须一致）。

| 帧 type | 方向 | 载荷 |
|---|---|---|
| `kRequest=1` | C→S | `call_id(varint) | method(string) | args_len(varint) | args` |
| `kResponse=2` | S→C | `call_id | status(u8) | message(string) | result_len | result` |
| `kStreamData=3` | S→C | `call_id | seq(varint) | len | payload` |
| `kStreamEnd=4` | S→C | `call_id | status(u8) | message` |
| `kCancel=5` | C→S | `call_id` |
| `kPing=6` / `kPong=7` | C↔S | `call_id` |

- `flags` bit0 = 流式请求（`kFlagStreaming`）；
- `args` 是「每个参数 `varint 长度 + Serialize 字节`」的顺序拼接；服务端按注册
  签名逐个拆开并校验「子 Buffer 恰好读完」；
- 状态码 1 字节，取值 0..15（gRPC 对齐），越界直接判协议错误；
- 不做类型名/ID 的传输：方法名已经承担路由，Reflect 只负责字段编码；
- 解码严格：长度必须与剩余字节完全相等；畸形帧返回 false，调用方断开连接。

线上开销：一次 `User.Get`（int64 参数 + 3 字段结果）约 **60-80 字节**，
其中帧头 16 字节（12 头 + 4 CRC）。作为对比，gRPC 单次 unary 在 HTTP/2 +
HPACK + protobuf 下的公开量级是 **150-250 字节**（含 HEADERS/DATA 帧头与 HPACK
动态表命中后的路径；首次请求更高）。

## 5. 服务注册与方法绑定

支持的 handler 签名（`MethodBinder` 在编译期展开，运行期无分支）：

| 形态 | 示例 | result |
|---|---|---|
| 值返回 | `User Get(const UserId&)` | 返回值的序列化 |
| 输出参数 | `Status Create(const Req&, User* out)` | `out` 的序列化 |
| 仅状态 | `Status Remove(const UserId&)` | 空 |
| 仅动作 | `void Touch(const UserId&)` | 空 |
| 服务端流 | `Status Watch(const Req&, ServerStream*)` | 只发 `StreamData`/`StreamEnd` |

- 输入参数按 `std::decay_t` 存值后反序列化，因此 `const T&` / `T` 形参都可用；
- `const` 成员函数同样支持（`std::invoke`）；
- 参数个数上限 255（`MethodBinding::argc` 是 u8）；
- 方法名规则：`[A-Za-z0-9_.-]{1,64}`，完整名 `"服务名.方法名"` ≤ 128 字节；
- `ServiceRegistry::Find` **无锁**（注册完成后只读）；`Listen` 会 `Freeze()`，
  之后注册返回 false——这是刻意的：每次 RPC 都要查表，不能有全局锁。
- 哈希表：开放寻址 + FNV-1a 64，查找无分配；服务名/方法名重复注册被拒绝。

显式注册（不用宏）也支持：

```cpp
server.Register("User", &service, std::array<rpc::MethodBinding, 2>{
    rpc::Method<&UserService::Get>("Get"),
    rpc::Method<&UserService::Create>("Create"),
}.data(), 2);
```

## 6. 客户端

- `Call<Result>(method, args...)`：便捷版，失败抛 `RpcError`；
- `TryCall(method, out, args...)`：无异常，结果写 `out`；
- `CallStatus(method, args...)`：无返回值；
- `AsyncCall<Result>(method, args...)`：返回 `PendingCall<Result>`，
  `Cancel()` 本地立即唤醒并通知服务端，`Wait()` 取结果；
- `OpenStream(method, args...)`：返回 `ClientStream`，
  `Next(&msg, &status)` 逐条取，`Finish()` 等结束，`Cancel()` 取消；
- `Ping()`：一次心跳往返，用于健康检查。

### 超时与取消

- 默认超时 `ClientOptions::timeout = 5000 ms`，`set_timeout` 修改后续调用；
- 等待路径先自旋 100 µs（覆盖本机 loopback 常见 RTT，避开 futex 唤醒），
  超过窗口再回到条件变量；
- 超时后：从等待表摘除、发 `Cancel` 帧、返回 `DEADLINE_EXCEEDED`；
  迟到的响应按 `call_id` 找不到等待者，直接丢弃，不会串到下一次调用；
- `Cancel` 语义：本地状态立即置为 `CANCELLED`，服务端尽快停止（流式 handler
  通过 `ServerStream::cancelled()` 感知）。

### 多路复用

一条连接一个读线程 + 一把写锁 + `call_id -> CallState` 表。多个线程可以同时
`Call`，写请求串行、等待并行；响应由读线程按 `call_id` 投递。`call_id` 从 1
递增（u64），客户端生命周期内不会回绕到冲突。

## 7. 服务端

```text
                ┌──────────────┐
   TCP accept ─▶│ 连接 reader   │  recv → FrameCodec::Feed → 解析
                │              │   ├─ 普通请求：直接执行 handler（快路径）
                │              │   ├─ 流式请求：入队 → worker
                │              │   ├─ Cancel：置位取消标志
                │              │   └─ Ping：立即回 Pong
                └──────┬───────┘
                       │ 流式请求队列
                ┌──────▼───────┐
                │ 连接 worker   │  执行流式 handler，逐条 Send StreamData
                └──────────────┘
```

- **普通请求在读线程直接执行**：少一次线程唤醒（性能关键路径）；
- **流式请求入队给 worker**：handler 可能长时间推送，读线程必须继续读 Cancel；
- 每连接 2 个线程；连接结束时 accept 线程回收并 join（`Reap`）；
- `Stop()` 幂等：关监听 → join accept → shutdown 所有连接 → join 连接线程；
- `ServerOptions`：`max_payload_bytes`、`checksum`、`backlog`、`accept_poll_ms`、
  `max_connections`。

## 8. 传输

`Transport` 只要求四个动作：`WriteAll` / `Read` / `Close` / `fd`。

- `TcpTransport`：POSIX 阻塞 socket + `poll`；连接建立用非阻塞 connect +
  poll 超时；客户端与服务端连接都开 `TCP_NODELAY`（小包往返不关 Nagle 会被
  delayed ACK 压成 ~40 ms）；`Close` 只 `shutdown` 不 `close`，真正 close 在
  析构里，避免读线程仍阻塞时 fd 被复用；
- `WebSocketTransport`：把一条 WebSocket 二进制消息当作一段字节流，内部缓存
  当前消息剩余字节；服务端用 `WebSocket::Accept` 完成握手后
  `server.AddConnection(...)`，客户端 `WebSocket::Connect` 后构造
  `Client(std::unique_ptr<Transport>)`；
- 自定义传输：实现 `Transport` 即可（共享内存、Unix socket、TLS 流等）。

## 9. 性能

环境：WSL2（Linux 6.x，12 vCPU），`.build-bento-wsl` Release（`-O3` 库，
测试 `-O2`），`taskset -c 2-9`。原始输出见 `/tmp/rpc-bench.txt` 与
`/tmp/rpc.md`。

| 指标 | 数值 | 口径 |
|---|---|---|
| 单次调用 p50 / p99 | 97.3 / 299.2 µs | 1e6 次，复用连接，`Call<User>`（含 string 结果） |
| 单次调用均值 / 吞吐 | 115.9 µs / 8618 次/s | 单连接、单线程同步调用 |
| 并发 1e5 次 | 166847 次/s（599.4 ms，0 失败） | 8 客户端 × 4 线程，每客户端一条连接 |
| 协议层完整路径 | 109.2 ns/op | 1e7 次：请求编码 + CRC32 帧 + 半包重组 + 请求解码 |
| 序列化 + 反序列化 | 70.5 ns/op | 1e7 次，`UserId`（9 B） |
| 网络下限（TCP echo 1B） | 106.2 µs p50 | 1e4 次，4 次 syscall + 2 次线程唤醒 |
| RPC / 网络下限 | 0.92x（另一次运行 1.22x） | WSL2 抖动；RPC ≈ 1.0-1.2x 下限 |
| 前：每次新建连接 | 546.3 µs 均值（p50 414.8） | 2000 次 |
| 后：连接复用 | 97.6 µs 均值（p50 83.5） | 2000 次，**5.6x** |

一次完整端到端运行（1e6 次）的分布：

```text
n=1000000 ok=1000000 fail=0 wall=115651.5ms throughput=8647/s
p50=100.2us p90=190.8us p99=285.3us p999=424.4us max=7674.9us mean=115.5us
```

并发（8 客户端 × 4 线程，共享连接）：

```text
clients=8 threads=32 total=100000 ok=100000 fail=0 wall=599.4ms throughput=166847/s
```

协议层（1e7 次完整线上路径，含 CRC-32 与半包重组）**109.2 ns/op**；
序列化（Serialize + copy + Deserialize）**70.5 ns/op**。两者相对 100 µs 的 RTT
只占 0.1%，说明瓶颈在 syscall 与线程唤醒，不在协议。

### 物理下限

一次同步调用至少包含：

1. 客户端 1 次 `send`（请求帧）+ 1 次 `recv`（响应帧）；
2. 服务端 1 次 `recv` + 1 次 `send`；
3. 两次跨线程唤醒（服务端读线程、客户端读线程）。

`TCP echo 1B` 基准测的是同一台机器上「2 次 send + 2 次 recv + 2 次线程唤醒」的
下限，因此 **RPC 延迟 / echo 延迟** 就是「协议 + 序列化 + 分派」相对网络下限的
倍数。协议层 `ns/op` 是纯 CPU 成本（无 syscall），用它说明序列化与帧编解码
不是瓶颈。

### objdump 证据

| 函数（Symbol） | 指令数 | call | 栈帧 B | 分配/释放 | 原子/锁 |
|---|---|---|---|---|---|
| `EncodeRequest` | 165 | 9 | 24 | 0 | 0 |
| `DecodeRequest` | 517 | 13 | 88 | 0 | 2 |
| `ServiceRegistry::FindUnlocked` | 59 | 2 | 24 | 0 | 0 |
| `ServiceRegistry::Hash` | 15 | 0 | 0 | 0 | 0 |
| `ServerConnection::Process` | 793 | 50 | 328 | 0 | 9 |
| `Connection::Wait`（客户端） | 255 | 20 | 104 | 0 | 0 |
| `FrameCodec::Encode` | 328 | 11 | 8 | 0 | 1 |
| `FrameCodec::FeedImpl` | 284 | 13 | 88 | 0 | 1 |
| `MethodBinder<&UserService::Get>::Invoke` | 469 | 19 | 392 | 0 | 1 |

- 查找路径无锁、无分配：`Hash` 15 条指令，`FindUnlocked` 59 条、24 B 栈帧；
- `EncodeRequest` 165 条、24 B 栈帧：写方法名/参数与 varint 循环；
- `DecodeRequest` 517 条、2 个原子指令（Buffer/shared_ptr 状态）；
- `MethodBinder::Invoke` 392 B 栈帧包含参数子 Buffer、反序列化与结果序列化；
- 直接 `malloc/free` 调用为 0：热路径的 Buffer/vector 复用容量，分配发生在
  `std::string`/`std::vector` 的 `_M_realloc_insert` 内部（间接、非每调用一次）。

## 10. 测试

| 测试 | 覆盖 |
|---|---|
| `go2cpp_rpc_protocol_test` | 注册表（注册/重名/非法名/冻结/枚举）、5 种 handler 签名、协议往返、截断/尾随/坏状态码拒绝、10 万随机帧、1e6 协议压测 |
| `go2cpp_rpc_test` | 基本类型/大载荷/错误码、超时与恢复、异步取消、8 客户端 × 4 线程 1e5 次并发、1e6 次延迟 p50/p99、断连 |
| `go2cpp_rpc_stream_test` | 1000 条推流顺序、接收超时后续读、取消传播到服务端、64 帧背压上限、同连接 4 条流、断连唤醒 |
| `go2cpp_rpc_websocket_test` | 真实 WebSocket 握手（socketpair）+ 调用 + 流 + 心跳 + 关闭 |
| `go2cpp_rpc_bench` | 序列化 1e7、协议层 1e7、TCP echo 下限、RPC 1e6、前/后对比 1e4 |

复现：

```bash
cmake --build .build-bento-wsl --target \
  go2cpp_rpc_protocol_test go2cpp_rpc_test go2cpp_rpc_stream_test \
  go2cpp_rpc_bench go2cpp_rpc_websocket_test -j12
./.build-bento-wsl/go2cpp_rpc_protocol_test
RPC_ITERATIONS=1000000 RPC_CONCURRENT_TOTAL=100000 ./.build-bento-wsl/go2cpp_rpc_test
./.build-bento-wsl/go2cpp_rpc_stream_test
./.build-bento-wsl/go2cpp_rpc_bench
```

## 11. 边界与已知限制

- **无 IDL**：C++ 签名就是 schema；跨语言/跨版本兼容需要自己约束（例如只用
  追加字段的 Reflect 结构）；
- **同连接的流式 handler 串行**：worker 单线程按队列执行，一条长流会阻塞同连接
  后面的流；需要真正并发流时用多条连接（或后续把 worker 换线程池）；
- **普通 handler 阻塞读线程**：一个慢 handler 会推迟同连接后续请求（与 Go
  `net/rpc` 的每连接 goroutine 语义一致）；
- **无 TLS**：需要安全传输时在 `Transport` 层套 TLS，或改用 WebSocket over TLS；
- **无压缩**：Reflect 的 RLE 未接入；大载荷建议业务层先压缩；
- **无重连/服务发现/负载均衡**：`Client` 一条连接，断开返回 `UNAVAILABLE`；
- **单进程内测试**：跨机延迟取决于真实网络 RTT，本报告的数值是回环下限；
- **`PendingCall` 只能 `Wait` 一次**（结果 Buffer 被消费）。

## 12. 参考

- Go `net/rpc`：`Register`/`Call`/`gob` 语义；
- gRPC 状态码：`grpc/status.h` 的 16 个 Canonical Error Codes；
- 本仓库 `docs/modules/frame-codec.md`（帧层）与
  `include/go2cpp/reflect/`（序列化层头文件）。
