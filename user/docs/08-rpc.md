# RPC（交接文档）

> **定位**：Go 风格进程间方法调用——方法名路由 + Reflect 序列化 + 一条 TCP 连接上的同步/异步/流式调用。
> 不引入 IDL、Protobuf、gRPC 工具链。
> **源码**：`include/go2cpp/rpc/`、`src/rpc/`　**构建**：`cmake/Go2CppRpc.cmake`（`-DGO2CPP_WITH_RPC=OFF` 可关）
> **测试**：`tests/rpc/`（`go2cpp_rpc_protocol_test` / `go2cpp_rpc_test` / `go2cpp_rpc_stream_test` /
> `go2cpp_rpc_websocket_test`，基准 `go2cpp_rpc_bench` 不注册 ctest）
> **设计全文**：[rpc.md](../../docs/modules/rpc.md)（协议表、handler 签名矩阵、objdump 证据）

## 1. 这个模块是什么

一句话：**`server.Register("User", &service)` 一行把 C++ 对象变成远程服务，客户端用
`client.Call<User>("User.Get", id)` 调它。**

它解决的是「两个 C++ 进程要互相调方法，但不想为几个接口引入整套 gRPC/Protobuf」的场景：

- **服务注册**：普通结构体 + 成员函数 + 一个 `GO2CPP_RPC_SERVICE` 宏，不需要继承基类；
- **序列化**：Reflect 字段表（`FieldList<T>` 特化，见 `include/go2cpp/reflect/`），无 .proto；
- **调用形态**：同步（抛异常/返回 Status 两版）、异步（`PendingCall` 可取消）、流式（`ClientStream`/`ServerStream`）；
- **传输**：一条 TCP 连接多路复用（`call_id` 分派），也可换 `WebSocketTransport` 或自定义 `Transport`。

刻意**不做**：IDL/代码生成、TLS、压缩、服务发现、负载均衡、跨语言、deadline 传播、metadata/拦截器。
需要这些时在传输层或业务层做，不要把 RPC 内核改成半套 gRPC。

## 2. 架构 / 数据流

### 2.1 帧与调用路径

```
  Client                                          Server
┌──────────────────────┐                  ┌──────────────────────────────┐
│ Call<User>(...)      │                  │ accept 线程                   │
│  PackArguments       │  G2RP 帧         │   └─ 每连接 reader 线程        │
│  BeginCall(call_id)  │ ───────────────► │        FrameCodec::Feed       │
│  WaitCall(timeout)   │  kRequest=1      │        ├─ 普通请求：读线程直接 │
│        ▲             │                  │        │   执行 handler（快路径）│
│        │ 读线程按      │  kResponse=2     │        ├─ 流式请求：入队 worker │
│        │ call_id 投递 │ ◄─────────────── │        ├─ Cancel：置取消标志   │
└──────────────────────┘                  │        └─ Ping：立即回 Pong    │
                                          │  连接 worker：跑流式 handler   │
                                          └──────────────────────────────┘
```

### 2.2 线程模型（交接重点）

| 侧 | 线程 | 说明 |
|---|---|---|
| 客户端 | 调用方线程 + 1 条读线程 | 写请求用一把写锁串行；等待按 `call_id` 并行，先自旋 100 µs 再进条件变量 |
| 服务端 | accept 线程 + 每连接 2 条（reader + worker） | 普通 handler 在 **reader 线程直接执行**；流式进 worker 队列 |
| 连接生命周期 | accept 线程 `Reap` | 连接结束回收并 join |

推论：**一个慢的普通 handler 会阻塞同连接后续请求**（与 Go `net/rpc` 每连接一个 goroutine 的语义一致）；
**同连接的流式 handler 串行**（worker 单线程），一条长流会挡住后面的流。要并发就开多条连接。

### 2.3 帧格式（`G2RP`）

帧层复用 `go2cpp::frame::FrameCodec`：magic `"G2RP"`、version 1、12 字节帧头、CRC-32/IEEE 帧尾、
`max_payload` 默认 4 MiB（两端必须一致）。

| type | 方向 | 载荷 |
|---|---|---|
| `kRequest=1` | C→S | `call_id(varint) | method(string) | args_len(varint) | args` |
| `kResponse=2` | S→C | `call_id | status(u8) | message(string) | result_len | result` |
| `kStreamData=3` | S→C | `call_id | seq(varint) | len | payload` |
| `kStreamEnd=4` | S→C | `call_id | status(u8) | message` |
| `kCancel=5` | C→S | `call_id` |
| `kPing=6` / `kPong=7` | C↔S | `call_id` |

`flags` bit0 = 流式请求（`kFlagStreaming=0x0001`）。`args` 是「每个参数 varint 长度 + Serialize 字节」
的顺序拼接；服务端按注册签名逐个拆开并校验「子 Buffer 恰好读完」。状态码 1 字节，取值 0..15（对齐 gRPC），
越界直接判协议错误。**不传类型名/类型 ID**：方法名承担路由，Reflect 只负责字段编码。

一次 `User.Get`（int64 参数 + 3 字段结果）线上约 **60–80 字节**（帧头 16 字节含 CRC）。

## 3. 对外 API（关键接口）

### 3.1 服务端

```cpp
class Server {
public:
    explicit Server(ServerOptions options = {});
    template <typename Service>
    bool Register(std::string_view service_name, Service* service);   // 用 GO2CPP_RPC_SERVICE 生成的方法表
    bool Register(std::string_view service_name, void* service,
                  const MethodBinding* bindings, std::size_t count);  // 显式注册
    bool Listen(std::string_view address);        // ":8080" / "127.0.0.1:8080"，后台 accept 线程
    bool Listen(const Endpoint& endpoint);
    bool AddConnection(std::unique_ptr<Transport> transport);  // 接管 WebSocket 等已建连接
    void Stop();                                  // 关监听、shutdown 连接、join 全部线程（幂等）
    bool running() const;
    std::uint16_t port() const;                   // Listen 之后有效
    const ServiceRegistry& registry() const;
};
```

`ServerOptions`：`max_payload_bytes = 4 MiB`、`checksum = true`、`backlog = 128`、
`accept_poll_ms = 20`、`max_connections = 1024`。

### 3.2 客户端

```cpp
explicit Client(std::string_view address, ClientOptions options = {});
explicit Client(std::unique_ptr<Transport> transport, ClientOptions options = {});

template <typename Result = void, typename... Args>
Result Call(std::string_view method, Args&&... args);          // 失败抛 RpcError
template <typename Result, typename... Args>
Status TryCall(std::string_view method, Result* out, Args&&... args);   // 无异常
template <typename... Args>
Status CallStatus(std::string_view method, Args&&... args);    // 无返回值
template <typename Result, typename... Args>
PendingCall<Result> AsyncCall(std::string_view method, Args&&... args);
ClientStream OpenStream(std::string_view method, Args&&... args);
Status Ping();
void Close();
```

`ClientOptions`：`timeout = 5000 ms`、`max_payload_bytes = 4 MiB`、`checksum = true`、
`max_stream_frames = 1024`、`max_stream_bytes = 16 MiB`、`tcp_no_delay = true`。

### 3.3 流式与异步

```cpp
class ServerStream {
    bool SendSerialized(const void* data, std::size_t size);
    Status Finish(Status status = Status::Ok());
    bool cancelled() const;        // 感知客户端 Cancel
};

class ClientStream {
    template <typename T> bool Next(T* out);
    Status Finish(std::chrono::milliseconds timeout = detail::kUseDefaultTimeout);
    bool Cancel();
};

template <typename Result>
class PendingCall {
    Status Wait(Result* out);
    bool Cancel();                 // 本地立即唤醒（CANCELLED），并通知服务端
};
```

`PendingCall` 的结果只能 `Wait` 一次（结果 Buffer 被消费）；`ClientStream` 有 64 帧背压上限
（`max_stream_frames`），超过后服务端 `Send` 会阻塞或失败。

### 3.4 状态码

```cpp
enum class StatusCode : std::uint8_t {
    kOk = 0, kCancelled = 1, kUnknown = 2, kInvalidArgument = 3, kDeadlineExceeded = 4,
    kNotFound = 5, kAlreadyExists = 6, kPermissionDenied = 7, kResourceExhausted = 8,
    kFailedPrecondition = 9, kAborted = 10, kOutOfRange = 11, kUnimplemented = 12,
    kInternal = 13, kUnavailable = 14, kDataLoss = 15,
};
struct Status { StatusCode code = StatusCode::kOk; std::string message; bool ok() const; };
```

取值与 gRPC 16 码**逐一对齐**（方便与公开 gRPC 基准对照）。**新增码只能追加在 15 之后**，
不能重排——错误码会写进日志与监控。

### 3.5 传输与注册表

```cpp
class Transport {                       // 四个动作就够了
    virtual bool WriteAll(const void* data, std::size_t size) = 0;
    virtual int  Read(void* buffer, std::size_t capacity) = 0;
    virtual void Close() = 0;
    virtual int  fd() const = 0;
};

const BoundMethod* ServiceRegistry::Find(std::string_view full_name) const noexcept;  // 无锁
void ServiceRegistry::Freeze() noexcept;   // Listen 时调用，之后注册返回 false
```

`TcpTransport` 开 `TCP_NODELAY`（否则小包往返被 delayed ACK 压成 ~40 ms）；`Close()` 只 `shutdown`
不 `close`，真正 close 在析构里，避免 reader 仍阻塞时 fd 被复用。

## 4. 最小用法（可编译示例）

```cpp
#include "go2cpp/rpc/rpc.hpp"
#include <iostream>

struct UserId { std::int64_t id = 0; };
struct User   { std::int64_t id = 0; std::string name; std::int32_t level = 0; };

template <> struct go2cpp::reflect::FieldList<UserId> {
    static constexpr go2cpp::reflect::TypeId kId = 101;
    static constexpr const char* kName = "UserId";
    static const go2cpp::reflect::FieldDescriptor* Fields(std::size_t& n) {
        static const go2cpp::reflect::FieldDescriptor f[] = {GO2CPP_REFLECT_FIELD(UserId, id)};
        n = sizeof(f) / sizeof(f[0]);  return f;
    }
};
template <> struct go2cpp::reflect::FieldList<User> {
    static constexpr go2cpp::reflect::TypeId kId = 102;
    static constexpr const char* kName = "User";
    static const go2cpp::reflect::FieldDescriptor* Fields(std::size_t& n) {
        static const go2cpp::reflect::FieldDescriptor f[] = {
            GO2CPP_REFLECT_FIELD(User, id), GO2CPP_REFLECT_FIELD(User, name),
            GO2CPP_REFLECT_FIELD(User, level)};
        n = sizeof(f) / sizeof(f[0]);  return f;
    }
};

class UserService {
public:
    User Get(const UserId& id) { return User{id.id, "alice", 3}; }
};
GO2CPP_RPC_SERVICE(UserService, Get)   // 在 UserService 所在命名空间展开一次

int main() {
    go2cpp::rpc::Server server;
    UserService service;
    if (!server.Register("User", &service)) return 1;
    if (!server.Listen("127.0.0.1:18080")) return 1;

    go2cpp::rpc::Client client("127.0.0.1:18080");
    User user = client.Call<User>("User.Get", UserId{42});   // 失败抛 RpcError
    std::cout << user.name << " level=" << user.level << "\n";
    server.Stop();
    return 0;
}
```

链接：`target_link_libraries(app PRIVATE go2cpp::rpc)`。要点：**服务对象必须比 `Server` 活得久**
（`Register` 只存裸指针）；`GO2CPP_RPC_SERVICE` 必须在服务类所在命名空间展开（ADL 找方法表）。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/rpc/status.hpp` / `src/rpc/status.cpp` | 136 / 69 | `StatusCode` 16 码、`Status`、`RpcError` |
| `include/go2cpp/rpc/registry.hpp` / `registry.cpp` | 676 / 174 | `ServiceRegistry`（开放寻址 + FNV-1a）、`Method<&M>`、`GO2CPP_RPC_SERVICE` |
| `include/go2cpp/rpc/protocol.hpp` / `protocol.cpp` | 164 / 380 | 帧类型、请求/响应/流帧编解码、varint |
| `include/go2cpp/rpc/transport.hpp` / `transport.cpp` | 195 / 445 | `Transport` 抽象、`TcpTransport`、`TcpListener` |
| `include/go2cpp/rpc/server.hpp` / `server.cpp` | 216 / 707 | `Server`、每连接 reader/worker、`ServerStream` |
| `include/go2cpp/rpc/client.hpp` / `client.cpp` | 525 / 794 | `Client`、`PendingCall`、`ClientStream`、读线程分派 |
| `include/go2cpp/rpc/websocket_transport.hpp`（全 inline，无 .cpp） | 196 | `WebSocketTransport`：一条 WS 二进制消息当字节流 |
| `include/go2cpp/rpc/rpc.hpp` | 22 | 聚合头 |
| `cmake/Go2CppRpc.cmake` | — | `go2cpp_rpc` + 4 个测试 + `go2cpp_rpc_bench` |

## 6. 性能数据

**`docs/reports/` 下没有 `asm-rpc.md`**——本模块的 objdump 证据与压测数字记录在
[rpc.md](../../docs/modules/rpc.md) §9（原始报告在 `/tmp/rpc.md`，未归档进仓库），下表原样引用：

| 指标 | 数值 | 口径 |
|---|---|---|
| 单次调用 p50 / p99 | **97.3 / 299.2 µs** | 1e6 次，复用连接，`Call<User>`（含 string 结果） |
| 单次调用均值 / 吞吐 | 115.9 µs / 8618 次/s | 单连接、单线程同步调用 |
| 并发 1e5 次 | **166847 次/s**（599.4 ms，0 失败） | 8 客户端 × 4 线程，每客户端一条连接 |
| 协议层完整路径 | **109.2 ns/op** | 1e7 次：请求编码 + CRC32 帧 + 半包重组 + 请求解码 |
| 序列化 + 反序列化 | 70.5 ns/op | 1e7 次，`UserId`（9 B） |
| 网络下限（TCP echo 1B） | 106.2 µs p50 | 1e4 次，4 次 syscall + 2 次线程唤醒 |
| RPC / 网络下限 | **0.92×**（另一次运行 1.22×） | WSL2 抖动；RPC ≈ 1.0–1.2× 下限 |
| 连接复用前 → 后 | 546.3 µs → **97.6 µs**（均值，**5.6×**） | 2000 次，前=每次新建连接 |

objdump（`-O3`，同一份报告）：

| 函数 | 指令数 | call | 栈帧 B | 分配 | 原子 |
|---|---:|---:|---:|---:|---:|
| `EncodeRequest` | 165 | 9 | 24 | 0 | 0 |
| `DecodeRequest` | 517 | 13 | 88 | 0 | 2 |
| `ServiceRegistry::FindUnlocked` | 59 | 2 | 24 | 0 | 0 |
| `ServiceRegistry::Hash` | 15 | 0 | 0 | 0 | 0 |
| `ServerConnection::Process` | 793 | 50 | 328 | 0 | 9 |
| `MethodBinder<&UserService::Get>::Invoke` | 469 | 19 | 392 | 0 | 1 |

解读：协议层 109.2 ns 相对 ~100 µs 的调用延迟只占 **0.1%**，瓶颈在 syscall 与线程唤醒，
不在编解码；注册表查找无锁无分配（`Hash` 15 条、`FindUnlocked` 59 条）是刻意设计——每次 RPC 都要查表。

## 7. 边界与已知限制

1. **无 IDL**：C++ 签名就是 schema，跨语言/跨版本兼容要自己约束（只用可追加字段的 Reflect 结构）。
2. **同连接流式 handler 串行**：worker 单线程按队列执行，一条长流挡住同连接后续流；需要并发流就多开连接。
3. **普通 handler 阻塞 reader 线程**：慢 handler 会推迟同连接后续请求（与 Go `net/rpc` 语义一致）。
4. **无 TLS / 无压缩**：安全传输请在 `Transport` 层套 TLS 或换 WebSocket over TLS；大载荷业务层先压缩。
5. **无重连 / 服务发现 / 负载均衡**：`Client` 一条连接，断开返回 `UNAVAILABLE`，重连要用新对象。
6. **`PendingCall` 只能 `Wait` 一次**；超时后迟到的响应按 `call_id` 找不到等待者，直接丢弃。
7. **`max_payload_bytes` 两端必须一致**（默认 4 MiB），不一致会在协议层判 `kDataLoss`。
8. **进程内测试口径**：所有性能数字都是回环下限，跨机要加真实 RTT。

## 8. 测试与验证方法

```bash
cmake -S . -B .build-rpc -G Ninja -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_RPC=ON
cmake --build .build-rpc --target go2cpp_rpc_protocol_test go2cpp_rpc_test \
      go2cpp_rpc_stream_test go2cpp_rpc_websocket_test go2cpp_rpc_bench -j"$(nproc)"
ctest --test-dir .build-rpc -R go2cpp_rpc --output-on-failure
./.build-rpc/go2cpp_rpc_bench          # 基准，不注册 ctest
```

| 测试 | 覆盖重点 |
|---|---|
| `go2cpp_rpc_protocol_test` | 注册表（注册/重名/非法名/冻结/枚举）、5 种 handler 签名、协议往返、截断/尾随/坏状态码拒绝、10 万随机帧、1e6 协议压测 |
| `go2cpp_rpc_test` | 基本类型/大载荷/错误码、超时与恢复、异步取消、8 客户端 × 4 线程 1e5 次并发、1e6 次延迟、断连 |
| `go2cpp_rpc_stream_test` | 1000 条推流顺序、接收超时、取消传播、64 帧背压、同连接 4 条流、断连唤醒 |
| `go2cpp_rpc_websocket_test` | 真实 WS 握手（socketpair）+ 调用 + 流 + 心跳 + 关闭 |
| `go2cpp_rpc_bench` | 序列化 1e7、协议层 1e7、TCP echo 下限、RPC 1e6、前/后对比 1e4 |

规模可调：`RPC_ITERATIONS`、`RPC_CONCURRENT_TOTAL`、`RPC_BENCH_*`。CI 上想缩短时间就调小，
但**别把调小后的数字当性能结论**。

## 9. 交接注意

- **注册表在 `Listen` 时冻结**：之后再 `Register` 返回 false。要动态加服务只能在 `Listen` 前注册，
  或重建 `Server`。这是「查找路径无锁」的前提，别改成加锁版本。
- **`ServiceRegistry::Find` 返回的 `BoundMethod*` 指向注册表内部**，注册表生命周期 = `Server`；
  不要在 `Server` 析构后还持有它。
- **服务对象是裸指针**：`Register(name, &service)` 不接管所有权，`Server` 存活期间服务对象不能析构。
- **改协议要同步三处**：`protocol.hpp` 的帧表、`docs/modules/rpc.md` §4、以及 `max_payload`/`checksum`
  两端的默认值。`status` 只占 1 字节且范围 0..15，加码必须走「追加在 15 之后」并同步解码校验。
- **超时路径先自旋 100 µs**：这是为 loopback 常见 RTT 做的优化，改等待逻辑时别把自旋删掉，
  否则本机 p50 会明显变差（futex 唤醒代价）。
- **`Stop()` 是幂等的**，但不要在 handler 或回调里调 `Stop()`（reader 线程自 join 死锁）。
- **未完成事项**：IDL/代码生成、TLS、压缩、服务发现、deadline 传播、metadata/拦截器、
  流式 worker 线程池化。逐条见 [rpc.md](../../docs/modules/rpc.md) §11。
