# Bento 网络服务器：状态 · API · 正确性 · 性能 · 限制

> 范围：HTTP/SSE、WebSocket、LinkServer、MQTT 3.1.1、CoAP、SOCKS5 六个协议服务端，
> 以及 OmniNetServer M1（`AsLinkServer` / `AsHttpServer` 真实后端）。
>
> 更新：2026-10-31（基准 `7fc4952`、NODELAY 修复 `419786e`）· 源码
> `include/go2cpp/bento/net/` + `src/bento/net/`。
> 口径：**实现完成 ≠ 验收完成**。本文把「能跑」「跑对」「跑多快」「哪里不行」分开写；
> 逐行契约仍以头文件 `@file` 注释为准，本文只做一览、用法与限制。
>
> 实测环境：WSL2 / GCC 13 / Release（`-O3 -DNDEBUG`）、全回环、`taskset -c 2,3`、
> loadavg 1.39~2.23。延迟是「发送前 → 收到完整响应」的 RTT（含本进程调度与回环栈），
> 横向可比、绝对值不可外推到跨机。原始数据见
> [六协议基准报告](../reports/2026-10-31-http-perf.md)。

## 0. 结论速览

- **6 协议全部可用**：单测 6/6 通过；协议正确性实测 37 个真实 socket 场景 / 238 条断言 /
  0 失败（2026-10-31 复跑；另有 5 个 TCP_NODELAY 回归场景，见 §10.1）。
- **吞吐（单连接串行）**：HTTP 7,695 req/s、WS 5,366 msg/s、MQTT 8,778 msg/s、
  CoAP 9,449 req/s、Link 可靠 8,311 msg/s / 不可靠 46,254 msg/s、SOCKS5 45 conn/s。
- **延迟**：HTTP p99 263 µs、WS 371 µs、MQTT 238 µs、CoAP 208 µs、Link 可靠 140 µs；
  **SOCKS5 p99 51.7 ms 是唯一异常点**（待复测，见遗留清单 P0）。
- **OmniNetServer M1**：UDP → `AsLinkServer()`、TCP → `AsHttpServer()`、Memory → Raw
  回环；WS / Raw-over-TCP/UDP / 嗅探 / TLS 均未实现，非法组合在 `Create()` 阶段拒绝。
- **共同短板**：协议层尚未接 `bufio`（适配层已就绪，基准证明小包可提速 21~33×）；
  raw accept 路径仍缺 `TCP_NODELAY`；全部服务端无 TLS / 限流 / 指标导出。

### 0.1 一页表

| 协议 | 状态 | 单连接吞吐 | p50 | p99 | 基准场景 | 单测 |
|---|---|---|---|---|---|---|
| HTTP/SSE | ✅ 可用（无 TLS/HTTP2） | 7,695 req/s | 120 µs | 263 µs | 6 | 9 用例 / 87 checks |
| WebSocket | ✅ 可用（无 wss） | 5,366 msg/s | 163 µs | 370 µs | 6 | 8 用例 / 83 checks |
| LinkServer | ✅ 可用（UDP） | 8,311 / 46,254 msg/s | 78 / 13 µs | 140 / 102 µs | 5 | 11 用例 / 1,469 checks |
| MQTT 3.1.1 | ✅ 可用（无 QoS2） | 8,778 msg/s | 100 µs | 238 µs | 6 | 27 用例 / 36,232 checks |
| CoAP | ✅ 可用（无 DTLS） | 9,449 req/s | 98 µs | 208 µs | 5 | 24 用例 / 38,572 checks |
| SOCKS5 | ⚠️ 可用，p99 异常 | 45 conn/s（中继 911 MB/s） | 832 µs | **51,682 µs** | 5 | 15 用例 / 204 checks |
| OmniNet M1 | ✅ 可用（3 传输子集） | Link 8,021 / HTTP 7,926 | — | — | 4 | 13 用例 / 273 断言 |

> 单测数据来自 2026-10-30 的 `ctest`；基准场景数为 `7fc4952` 的 37 个（不含 NODELAY
> 回归）。SOCKS5 的 p99 是修复前批次，`419786e` 已给 accept 侧补 `TCP_NODELAY`，
> 新批次复测见遗留清单。基准的 238 条断言是**总计**，`--correctness` 输出不按协议
> 拆分，各协议的「场景数」见上表，断言明细在 `bench_protocols.cpp` 的
> `CheckCorrectness()` 里逐条可查。

## 1. 共同契约（先看这一节，再挑协议）

- **生命周期**：`Listen("host:port")` 只绑定、不起线程 → 注册 handler → `Start()` →
  `Stop()`；`Port()` 查内核分配端口（`0` 表示内核分配）。`Stop()` 幂等，返回后端口已释放。
- **错误传播**：HTTP/WS 用 `HttpServerResult` / `WsServerResult`；SOCKS5 用 `bool` +
  `last_error()`；Link/MQTT/CoAP 用枚举返回值。**不跨线程抛异常**：handler 异常被隔离
  （HTTP 500 + `Connection: close`，WS 关闭连接并计数）。
- **线程模型**：HTTP/WS 是「1 accept + 4 worker」，一条连接由一个 worker 串行处理；
  SOCKS5 是单线程 `poll`（可选后台线程）；LinkServer 是内置接收线程或外部 `RunOnce`；
  MQTT/CoAP 是非线程安全的纯状态机，由调用方串行驱动。
- **handler 内不要 `Stop()`**：HTTP/WS 会 detach 当前 worker 防 terminate，但 server
  对象可能在 handler 返回前被析构；需要确定性关闭时从其他线程调用 `Stop()`。
- **构建开关**：`-DGO2CPP_WITH_BENTO=ON`（默认 OFF）；`GO2CPP_BENTO_TESTS` 默认 ON，
  自动注册 6 个测试；`go2cpp_bento` = Link + MQTT + CoAP，`go2cpp_bento_http` /
  `go2cpp_bento_ws` 依赖 `go2cpp_network`，`go2cpp_bento_socks` 仅 POSIX。

## 2. HTTP / SSE（`HttpServer`）

**状态**：✅ 可用。任意方法/头/body、keep-alive、404、并发 accept、SSE 事件推流；
`AsSse` 路径精确匹配且优先。默认 4 worker、每连接 100 请求、header 64 KiB、body 8 MiB。

**API 摘要**

```cpp
go2cpp::bento::HttpServer server;
server.OnRequest([](const go2cpp::http::Request& req) {
    go2cpp::http::Response r;
    r.body = "hello " + req.path();
    return r;
});
server.AsSse("/events", [](const go2cpp::http::Request&, go2cpp::sse::Stream& s) {
    go2cpp::sse::Event e;
    e.event = "tick";
    e.data = "1";
    (void)s.send(e);   // 必须观察返回值或 IsStopping() 退出长循环
});
server.Listen("127.0.0.1:0");
server.Start();
```

**正确性**（6 场景）：真实 GET /hello、POST /echo（8 KB 含 `\r\n`）、同连接 5 连
keep-alive、SSE 三事件（`id/event/data`）、未知路径 404、4 客户端 × 50 请求并发。

**性能**

| 场景 | 吞吐 | p50 | p99 | p999 |
|---|---|---|---|---|
| GET /hello 单连接 keep-alive | 7,695 req/s | 120.06 µs | 263.16 µs | 439.16 µs |
| GET /hello 4 并发 × 1500 | 67,350 req/s | — | — | — |
| GET /big 16 KB 单连接 | 6,809 req/s（≈106 MB/s） | 132.61 µs | 285.69 µs | 309.38 µs |

4 并发聚合 67k 说明 2 核上 4 worker 足够；单连接受 RTT 支配，`16 KB` 档瓶颈是
「每条响应一次 write + 一次拷贝」。

**已知限制**：无 TLS、HTTP/2、路由参数/通配符、请求体流式读取、每连接读超时、连接级
限流；`NonBlocking` 是预留枚举，`Start()` 返回 `UnsupportedModel`（不静默降级）；
SSE handler 必须自己观察 `send()` 返回值或 `IsStopping()`，否则 `Stop()` 只能靠 socket
shutdown 让写失败。

## 3. WebSocket（`WsServer`）

**状态**：✅ 可用。Upgrade 校验与 101、非法请求 400、首帧 pending 注入、文本/二进制、
Ping/Pong、Close 握手、优雅 Stop、并发连接、运行期替换 handler、基础统计。
握手超时 10 s（钳制 100 ms–120 s）、header 16 KiB、worker 4。

**API 摘要**

```cpp
go2cpp::bento::WsServer server;
server.OnMessage([](const go2cpp::bento::WsConnection::Ptr& c,
                    const go2cpp::net::WebSocketMessage& m) {
    if (m.opcode == go2cpp::net::WebSocketOpcode::Text) (void)c->SendText(m.payload);
});
server.Listen("127.0.0.1:0");
server.Start();
```

`WsConnection::Ptr` 可跨线程 `SendText/SendBinary/Ping/Pong/Close`；`Stats()` 给计数。

**正确性**（6 场景）：握手 + 文本 echo、256 B 二进制（含 `0x00/0xFF`）、Ping→Pong
（手写帧观察）、客户端 Close 1000、128 KB 二进制重组、无 Upgrade 的普通 GET 400。

**性能**

| 场景 | 吞吐 | p50 | p99 | p999 |
|---|---|---|---|---|
| 文本 32 B echo 单连接串行 | 5,366 msg/s | 162.62 µs | 370.46 µs | 527.33 µs |
| 二进制 1 KB echo 单连接 | 5,295 msg/s（≈5.2 MB/s） | 171.13 µs | 353.73 µs | 542.78 µs |
| 文本 echo 4 并发 × 400 | 9,044 msg/s | — | — | — |

比 HTTP 低约 30%：每消息多一层帧编解码与掩码校验；4 并发只到 9k，因为 echo 在
「读循环内同步」执行，同一连接内无法流水线。

**已知限制**：无 wss/TLS、permessage-deflate、子协议协商、路由与鉴权、服务端广播表、
连接级限流；分片消息由 base 重组后交付，无逐片回调。

## 4. LinkServer（UDP，可靠 + 不可靠同端口）

**状态**：✅ 可用。内部只持一个 `PeerHub`（1 socket 接 N peer），peer 标识是本地句柄；
线格式与 `Link`/`PeerHub` 互通。`STREAM` 有序可靠、自动分片（约 330 KB）；
`DATAGRAM` 不重传、≤1300 B；`channel` 0–255 两类各自编号。

**API 摘要**

```cpp
go2cpp::bento::LinkServer server;
server.OnPeerConnected([](go2cpp::bento::LinkPeerId peer) { /* ... */ });
server.OnStream([](go2cpp::bento::LinkPeerId peer, std::uint8_t channel,
                   const std::uint8_t* data, std::size_t bytes) { /* 可靠消息 */ });
server.Listen("0.0.0.0", 9000);
server.Start();                    // 起内置接收线程；或 start_receiver_thread=false 外部 RunOnce
server.Send(peer, 1, data, bytes, go2cpp::bento::LinkReliability::kReliable);
server.Broadcast(2, frame, frame_bytes, go2cpp::bento::LinkReliability::kUnreliable);
```

**正确性**（5 场景）：可靠 STREAM 双向 200 B（序号/校验）、不可靠 DATAGRAM 双向 100 B、
Broadcast 到 2 peers（投递数 == 2）、多 peer 单播隔离（A 收、B 不收）、客户端静默 →
peer 超时断开回调。

**性能**

| 场景 | 吞吐 | p50 | p99 | p999 |
|---|---|---|---|---|
| 可靠 STREAM 64 B 单向 | 8,311 msg/s | 77.57 µs | 139.76 µs | 139.76 µs |
| 不可靠 DATAGRAM 64 B 单向（节流） | 46,254 msg/s | 12.71 µs | 102.11 µs | 312.95 µs |

可靠通道含 ACK/重传/拥塞窗口；不可靠通道直写 UDP，必须**发送节流**（否则内核接收
缓冲溢出、到达率跌到个位数百分比），报告吞吐时需带到达率（本次 >95%）。

**已知限制**：`Send` 不内部排队，窗口满返回 `kWouldBlock`（256 条 / 1 MiB 流控），
调用方重试或降级；回调在 `RunOnce` 线程同步执行，回调内 `Stop()` 只置标志返回；
`Listen/Start/Stop` 必须串行；无多 socket 分片与迁移加固、无限流；PSK 可选
（Omni 的 UDP 路径默认要求加密，无 PSK 需显式允许明文，见 §8.3）。

## 5. MQTT 3.1.1（`MqttDecoder` / `MqttSession` / `MqttBroker`）

**状态**：✅ 可用（无网络层，调用方供字节流）。九种控制报文、VLQ（上限 268435455）、
粘包/半包流式解码；订阅路由支持 `+`/`#`（`$` 开头主题不被吞）；QoS 0/1（含 PUBACK）；
keepalive 1.5× 超时；遗嘱投递。

**API 摘要**

```cpp
go2cpp::bento::net::MqttBroker broker;
broker.OpenSession("sensor-1", nullptr);
std::vector<go2cpp::bento::net::MqttOutbound> out;
broker.HandleBytes("sensor-1", data, size, &out);   // 内部完成 CONNECT/SUBSCRIBE/PUBLISH
for (const auto& m : out) SendTo(m.client_id, m.bytes);
```

**正确性**（6 场景）：CONNECT→CONNACK(0)、SUBSCRIBE→SUBACK、QoS0 自订阅回环、QoS1
PUBACK（packet id 一致）+ 回环、通配符 `sensor/+/temp` 只命中单层、CONNECT 拆两段发送
（半包）→ 正常 CONNACK。

**性能**

| 场景 | 吞吐 | p50 | p99 | p999 |
|---|---|---|---|---|
| QoS0 PUBLISH 64 B 自订阅回环（TCP） | 8,778 msg/s | 100.15 µs | 238.33 µs | 463.86 µs |
| broker 内存路由 64 B（无 socket） | 4,877,505 ops/s | — | — | — |

内存路由与端到端相差 550 倍：瓶颈完全在 TCP 往返，不在会话表/通配符匹配。

**已知限制**：QoS 2 返回 `kUnsupported`；无持久会话（session present 恒 false）、
retained 存储、TLS/WS、用户名密码/ACL、共享订阅、最大报文协商；UTF-8 只拒内嵌 NUL
与越界；全部非线程安全（一个连接由同一线程串行驱动）。

## 6. CoAP（RFC 7252，`CoapClient` / `CoapServer`）

**状态**：✅ 可用（无网络层，`CoapSendFn` 注入）。客户端单请求在途、自动 Message ID/
Token、重传字节与首发一致、`ack_timeout_ms` 2000 起严格翻倍、最多 4 次（无抖动）；
服务端 CON 回 piggyback ACK、NON 回新 MID NON；按 (Message ID, Token) 去重；无 handler
时 GET→2.05、PUT→2.04、POST→2.01。

**API 摘要**

```cpp
go2cpp::bento::net::CoapServer server([](const std::vector<std::uint8_t>& w) { return SendUdp(w); });
server.SetHandler([](const go2cpp::bento::net::CoapMessage& req) {
    return go2cpp::bento::net::CoapResponse::Content({'o', 'k'}, 0);
});
server.Handle(datagram);                     // 生成并发送响应

go2cpp::bento::net::CoapClient client(send_fn);
client.SendRequest(go2cpp::bento::net::CoapCode::kGet, "/sensors/temp");
client.Poll();                               // 周期调用，驱动指数退避重传
```

**正确性**（5 场景）：CON GET 2.05 + piggyback ACK、CON PUT 2.04、服务端丢首包后
50 ms 重传成功（handler 只调 1 次）、NON GET → NON 响应、重复 (MID, Token) 去重。

**性能**

| 场景 | 吞吐 | p50 | p99 | p999 |
|---|---|---|---|---|
| CON GET 串行（真实 UDP） | 9,449 req/s | 97.63 µs | 207.51 µs | 237.25 µs |
| NON GET 串行（真实 UDP） | 10,374 req/s | 92.01 µs | 197.75 µs | 212.55 µs |

NON 略快于 CON（无需 ACK 等待）。历史上另一次低负载运行曾测到 50.6k req/s，
差异来自共享机器调度，跨次比较须以同一批次为准。

**已知限制**：无 DTLS/OSCORE、Observe、Blockwise、Proxy；客户端同时只允许一个
pending；去重缓存默认 8 条且没有 247 s EXCHANGE_LIFETIME 窗口；separate response 只
支持「空 ACK 后等 CON」，不主动查询；非线程安全；`random_seed` 默认固定（见 §11.3）。

## 7. SOCKS5（RFC 1928，`go2cpp::bento::socks5::Socks5Server`）

**状态**：⚠️ 可用，但建连 p99 异常。分两层：协议层 `ParseSocks5Greeting` /
`ParseSocks5Request` / `EncodeSocks5Reply` 纯字节编解码；服务端层 accept → 握手 →
拨号 → 双向中继。

**API 摘要**

```cpp
go2cpp::bento::socks5::Socks5ServerConfig config;
config.listen_port = 0;                     // 0 = 内核分配，可并行多实例
go2cpp::bento::socks5::Socks5Server server(config);
server.Open();                              // 只 bind + listen
server.Start();                             // 后台线程循环 Poll()
// 客户端连 server.local_port()：握手 → CONNECT → 双向中继
server.Stop();                              // 停线程并 join（幂等）
server.Close();                             // 关监听与全部连接（幂等）
```

只协商无认证 0x00（未提供则回 `05 FF` 并关闭）；ATYP 支持 IPv4/域名/IPv6；成功填
`getsockname` 的 `BND.ADDR/PORT`，失败回 `0.0.0.0:0` + RFC 码；半关闭传播。
默认连接上限 128、握手 10 s、connect 5 s、空闲 60 s、错误宽限 2 s、缓冲 64 KiB。

**正确性**（5 场景）：无认证握手、CONNECT IPv4 中继、CONNECT 域名 + DNS + 中继、
未监听端口非 0 回复码、只提供用户名密码 → `05 FF`。

**性能**

| 场景 | 吞吐 | p50 | p99 | p999 |
|---|---|---|---|---|
| 握手 + CONNECT IPv4 + 关闭 | 45 conn/s | 831.94 µs | **51,682 µs** | 51,896 µs |
| 中继双向 4 MB（64 KB 块） | 911 MB/s | — | — | — |

p50 832 µs 说明多数连接很快，约 2% 出现 ~50 ms 停顿。`419786e` 已在 accept 后设置
`TCP_NODELAY`（与 HTTP/WS 同源修复：握手是 2~10 B 小包往返），但**修复后未复测**，
51.7 ms 这个数仍是修复前批次，不能当作当前结论。

**已知限制**：无用户名密码/GSSAPI、BIND/UDP ASSOCIATE（回 0x07）、UDP 中继、TLS、
ACL（0x02 无规则源）；DNS 在驱动线程同步 `getaddrinfo`；除 `Start/Stop` 外同线程调用；
p99 异常见遗留清单 P0。

## 8. OmniNetServer M1（组合式统一入口）

**定位**：不做「一个巨类 + 虚函数」，而是配置驱动组合已验收的后端：UDP 组合
`LinkServer`、TCP 组合 `HttpServer`、Memory 保留 M0 的 Raw 回环。`Create()` 一次性
完成校验与后端构造，不绑定端口、不建线程；`Start()` 才真实监听。

### 8.1 用法

```cpp
using namespace go2cpp::bento::omni;

// UDP → LinkServer 视图
OmniNetConfig udp;
udp.transport = TransportKind::kUdp;
udp.protocols = {ProtocolKind::kLink};
udp.concurrency = ConcurrencyKind::kThread;
udp.bind = {"127.0.0.1", 0};
// udp.link_psk = std::vector<std::uint8_t>(32, 0x42);  // 空 = 明文，仅本机测试
auto server = NetServer::Create(udp);
std::string error;
server->Start(&error);
auto link = server->AsLinkServer();          // valid() == true
link.Send(peer, 1, data, bytes, LinkReliability::kReliable);

// TCP → HttpServer 视图
OmniNetConfig tcp;
tcp.transport = TransportKind::kTcp;
tcp.protocols = {ProtocolKind::kHttp, ProtocolKind::kSse};
tcp.bind = {"127.0.0.1", 0};
auto http_server = NetServer::Create(tcp);
auto http = http_server->AsHttpServer();     // 先拿视图、注册 handler，再 Start
http.OnRequest([](const go2cpp::http::Request&) { return go2cpp::http::Response{}; });
http_server->Start();
http_server->Stop();                          // 终态；端口已释放
```

| transport | 允许的 protocol | 允许的 concurrency | 视图 |
|---|---|---|---|
| `kMemory` | `kRaw` | 任意（M0 兼容，`PollOnce()` 驱动） | `AsRawServer()` |
| `kUdp` | `kLink` | `kGmp` / `kThread` | `AsLinkServer()` |
| `kTcp` | `kHttp` / `kSse` | `kGmp` / `kThread` | `AsHttpServer()` |
| `kTls` / `kWs` | — | — | `Create()` 返回 `kUnsupported` |
| TCP/UDP + `kRaw` | — | — | `kUnsupported`（M3 嗅探） |

非法组合（如 UDP + HTTP、TCP + `kPoll`）在 `Create()` 就失败并写中文诊断，
**不留下运行期「配置成功但不工作」的半支持状态**。

### 8.2 正确性与性能

- 正确性：基准 4 个场景（Omni 单测另有 13 用例 / 273 断言）——UDP `AsLinkServer`
  真实收发 + HTTP 视图 invalid；TCP `AsHttpServer` 真实 GET `omni:/hello`；TCP SSE
  推流 3 事件；`Stop()` 后同端口可被新实例重新绑定。
- 性能：`AsLinkServer` 可靠 64 B 单向 8,021 msg/s（p50 20.18 ms 是**流水线端到端**，
  含 2000 条发送队列等待，不是空载单程；同链路空载参考 LinkServer 77.57 µs）；
  `AsHttpServer` GET 7,926 req/s（p50 111.79 µs / p99 284.52 µs），与直接
  `HttpServer` 的 7,695 req/s 同量级，组合层开销可忽略。

### 8.3 M1 限制（重要）

- **只做 UDP/TCP/Memory**：`kWs`、`kTls`、TCP/UDP 上的 `kRaw`、协议嗅探、连接级
  限流与指标导出均未实现（M2/M3/M4）。
- **TCP 的 `accepted` 恒为 0**：`HttpServer` 未暴露累计连接数，`NetStats.accepted`
  只有 UDP（LinkServer peer 计数）与 Memory 有值；`active` 是「正在处理的连接数」，
  keep-alive 空闲连接不计。
- **`Stop()` 是终态**：同一实例不能再次 `Start()`；换端口/重绑用新实例。
- **UDP 明文需显式允许**：`PeerHub` 默认要求加密，`link_psk` 为空表示明文（仅限
  本机回环/测试，不要用于生产）。
- **视图可晚于 `NetServer` 析构**：视图持有共享状态，析构后只能读终值
  （`Port() == 0`、`Send()` 返回 `kNotConnected`、`peer_count() == 0`）。
- **不支持视图级阻塞收发**：`IoMode` 只参与合法性校验，M1 的 `Recv/Send` 语义由
  后端决定。

## 9. 协议选择指南

| 场景 | 推荐 | 理由 / 注意 |
|---|---|---|
| 浏览器/前端 REST、SSE 推送 | **HTTP/SSE** | 生态最好、代理友好；SSE 是单向，别拿它做双向交互 |
| 浏览器全双工、低延迟协作 | **WS** | 帧开销小；当前无 wss，跨公网需在前面加 TLS 终止 |
| 游戏状态 / 音视频 / 遥测混合 | **LinkServer** | 可靠与不可靠同端口、同一 peer 会话；不可靠通道必须节流 |
| 受限 UDP 物联网、CoAP 设备 | **CoAP** | 报文小、无连接；无 DTLS/Observe，公网需自行加安全层 |
| 设备消息总线、主题订阅 | **MQTT 3.1.1** | 通配符与 QoS1 成熟；无持久会话/retained，别当 Kafka 用 |
| 出站代理 / 内网穿透隧道 | **SOCKS5** | 只做 CONNECT；无认证/ACL，只适合可信内网 |
| 需要统一入口 / 多协议共存 | **OmniNetServer M1** | 当前只覆盖 TCP-HTTP/SSE 与 UDP-Link；WS/嗅探/TLS 等 M2+ |

快速判断：

1. 对端是浏览器 → HTTP（请求-响应）或 WS（双向）；SSE 只在「服务器单向推」时选。
2. 对端是游戏/媒体客户端，且需要「可靠控制流 + 不可靠实时流」→ LinkServer。
3. 对端是 MCU/受限网络，UDP 且报文极小 → CoAP；需要订阅模型则 MQTT。
4. 需要把流量代理出去 → SOCKS5；不要用它做认证网关。
5. 已有多种客户端、想统一生命周期与统计 → OmniNetServer，但先确认所需组合在
   §8.1 的矩阵内。

## 10. 共同限制与优化状态

### 10.1 TCP_NODELAY：已修（`419786e`）

`Socket::accept` 现在默认给新连接设 `TCP_NODELAY`（进程级开关
`Socket::SetTcpNoDelayDefault()`，测试可 A/B 关闭）。修复前 WS 小包 echo 会被
Nagle + 延迟 ACK 压成每轮 ~40 ms（实测 44 ms → 0.15 ms，292×）。HTTP/WS/SOCKS5
的 accept 路径已覆盖；**raw accept 路径仍有缺口**（`tcp_proxy` / `debug_server`），
见遗留清单 P1。

### 10.2 bufio 尚未接入协议层（P1）

`include/go2cpp/network/bufio_socket.hpp` 已提供 `SocketReader` / `SocketBatchWriter`
（TCP `writev`、UDP `sendmmsg`、文件 `sendfile`），基准也证明：小包（<4 KB）用
`bufio::Writer(4 KB, kHybrid)` 可把系统调用从 1 条/消息降到 1/32~1/64，吞吐提升
**21~33×**；`bufio+batch` 与纯 bufio 同量级。但 6 个协议服务端仍逐条 `send`，
没有接这层适配器——这是当前最大的「已证明、未落地」优化。

### 10.3 大包走 bufio 反而慢 19%（P2，路由策略）

4096 B 报文：单写 151,121 ops/s，bufio 122,721（**0.81×**），bufio+batch 146,702
（0.97×）。原因是报文 ≥ 缓冲容量时退化成「直写 + 一次内存拷贝」，系统调用数仍是
1/msg。正确做法是按长度阈值**旁路 bufio**（或走 `sendfile`），而不是全局开 bufio。

### 10.4 SOCKS5 p99 51.7 ms（P0）

修复前批次：45 conn/s、p50 832 µs、p99 51,682 µs，约 2% 连接出现 ~50 ms 停顿；
中继 911 MB/s 不受影响。`419786e` 已补 accept 侧 `TCP_NODELAY`（同源假设），但
**没有修复后的复测数据**，因此仍按 P0 跟踪：先复测，再决定是否继续定位 accept
轮询/线程调度。

### 10.5 CoAP 固定随机种子（P1）

`CoapClientConfig::random_seed` 默认 `0x636f61705f636c69`，两个新客户端会生成相同
`(Message ID, Token)`，被服务端去重缓存判为重复并**返回上一个请求的响应**。基准里
表现为「PUT 返回上一个 GET 的 2.05」。多实例共存时必须显式换种子（基准用
`NowNs() ^ 常量`）；产品默认值应改为熵/时间初始化。

### 10.6 UDP 批量只有 1.4×（P3，环境上限）

回环上 `sendmmsg` 批量相对单条 `sendto` 的增益远小于 TCP 的 `writev`（历史批次约
1.4×），原因是 UDP 每个段仍是独立数据报、接收端内核缓冲与单核调度先饱和；这不是
适配层缺陷。结论：**UDP 批量收益要按「减少 syscall」而不是「线性吞吐」预期**，
跨机/多队列环境需重测。

### 10.7 其它共同缺口

- 6 个协议均无 TLS（Link 可选 PSK）、无限流、无指标导出（只有各自 `Stats()`）。
- 只有单测 + 回环基准，**没有跨机 RTT/丢包/带宽数据**；结论方向可信，绝对值需重跑。
- 延迟只测 RTT，没有拆服务端 handler 耗时；SSE 只验证推流正确性，未做长连接吞吐。
- MQTT/CoAP 是非线程安全状态机，接入多线程网络层时需按连接分片或由调用方加锁。

## 11. 复现与相关文档

```sh
# 构建（工作树根目录）
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DCMAKE_BUILD_TYPE=Release
ninja -C .build-bento-wsl bento_bench_protocols

# 正确性（实测 37 场景 / 238 断言；ctest 也跑这一部分）
taskset -c 2,3 ./.build-bento-wsl/bento_bench_protocols --correctness

# 性能（分协议或全量；建议绑核并确认 loadavg < 4）
taskset -c 2,3 ./.build-bento-wsl/bento_bench_protocols --perf
taskset -c 2,3 ./.build-bento-wsl/bento_bench_protocols --perf --only=http,ws,mqtt,coap
taskset -c 2,3 ./.build-bento-wsl/bento_bench_protocols --perf --only=socks5,link,omni,write
```

退出码：0 = 全部场景通过；1 = 有断言失败（失败项打印在 stderr）。

相关：[六协议基准报告](../reports/2026-10-31-http-perf.md)、
[协议遗留清单](../reports/2026-10-31-protocol-backlog.md)、
[Bento 路线图](bento-roadmap.md)、[OmniNetServer 设计](../research/omni-net-design.md)。
