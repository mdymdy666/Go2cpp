# Bento 网络服务器一览（HTTP/SSE · WS · Link · MQTT · CoAP · SOCKS5）

> 更新：2026-10-30 · 源码 `include/go2cpp/bento/net/` + `src/bento/net/`（WSL 工作树）
> 口径：**实现完成 ≠ 验收完成**。6 个骨架均已提交，单测 6/6 通过（§1 实测）；
> 压测 / 随机 / ≥5 场景 / audit 报告仍缺（门槛见 [bento-roadmap](bento-roadmap.md) §3.2）。
> 逐行契约以头文件 `@file` 注释为准，本文只做一览、用法与限制。

## 1. 一览（2026-10-30 实测：`ctest` 6/6 通过，19.2 s）

| 模块 | 头文件 / 实现（行） | CMake 目标 | 传输 | 并发模型 | 测试（用例 / 实测 checks） | 提交 |
|---|---|---|---|---|---|---|
| HttpServer（含 SSE） | `http_server.hpp` 283 / `.cpp` 485 | `go2cpp_bento_http` | TCP | 1 accept + 4 worker | 9 用例 / 87 checks | `32ff39b` |
| WsServer | `ws_server.hpp` 396 / `.cpp` 609 | `go2cpp_bento_ws` | TCP | 1 accept + 4 worker | 8 用例 / 83 checks | `bad224b` |
| LinkServer | `link_server.hpp` 362 / `.cpp` 594 | `go2cpp_bento` | UDP（PeerHub） | 内置接收线程，或外部 `RunOnce` | 11 用例 / 1469 checks | `280e38e` |
| MQTT 3.1.1 | `mqtt.hpp` 582 / `.cpp` 1380 | `go2cpp_bento` | 无（调用方供字节流） | 非线程安全，调用方串行 | 27 用例 / 36232 checks | `5b80e86` |
| CoAP | `coap.hpp` 611 / `.cpp` 943 | `go2cpp_bento` | 无（`CoapSendFn` 注入） | 非线程安全，调用方串行 | 24 用例 / 38572 checks | `42b70b0` |
| SOCKS5 | `socks5.hpp` 399 / `.cpp` 1256 | `go2cpp_bento_socks` | TCP（POSIX socket） | 单线程 `poll`，可选后台线程 | 15 用例 / 204 checks | `04372cb` |

构建：`-DGO2CPP_WITH_BENTO=ON`（默认 OFF），`GO2CPP_BENTO_TESTS` 默认 ON 自动注册 6 个测试；
`go2cpp_bento` = link_server + mqtt + coap，`http`/`ws` 依赖 `go2cpp_network`（standalone 跳过），`socks` 仅 POSIX。

## 2. HTTP / WS 的共同契约

- 生命周期：`Listen("host:port")` 只绑定不起线程 → 注册 handler → `Start()` → `Stop()`；`Port()` 查内核分配端口。
- 结果用 `HttpServerResult` / `WsServerResult` 返回，不跨线程抛异常；handler 异常被隔离（HTTP 500 + `Connection: close`，WS 终止连接并计数）。
- 一条连接由一个 worker 串行处理；handler 内**不要** `Stop()` 或析构 server；`Stop()` 幂等，返回后端口已释放。

## 3. HTTP / SSE（`HttpServer`）

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
    (void)s.send(e);   // handler 必须观察返回值或 IsStopping() 退出
});
server.Listen("127.0.0.1:0");
server.Start();
```

任意方法/头/body、keep-alive、404、并发 accept；`AsSse` 路径**精确匹配**且优先。默认 4 worker、每连接 100 请求、header 64 KiB、body 8 MiB。
**限制**：无 TLS、HTTP/2、路由参数与通配符、流式请求体、每连接读超时、限流；`NonBlocking` 是预留枚举，`Start()` 返回 `UnsupportedModel`（不静默降级）。

## 4. WebSocket（`WsServer`）

```cpp
go2cpp::bento::WsServer server;
server.OnMessage([](const go2cpp::bento::WsConnection::Ptr& c,
                    const go2cpp::net::WebSocketMessage& m) {
    if (m.opcode == go2cpp::net::WebSocketOpcode::Text) (void)c->SendText(m.payload);
});
server.Listen("127.0.0.1:0");
server.Start();
```

- 由 `http::Parser` 读 Upgrade、`WebSocket::ValidateUpgradeRequest()` 校验；非法请求回 400；多读的首帧经 `AcceptWithPending()` 注入。
- Ping/Pong 与 Close 由 base 自动处理；`WsConnection::Ptr` 可跨线程 `SendText/SendBinary/Ping/Pong/Close`；`Stats()` 给计数。
- 握手超时 10 s（钳制 100 ms–120 s）、header 16 KiB、worker 4；**限制**：无 wss/TLS、deflate、子协议、路由鉴权、广播表、限流；分片由 base 重组，无逐片回调。

## 5. LinkServer（UDP，可靠 + 不可靠同端口）

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

- 内部只持一个 `PeerHub`（1 socket 接 N peer），peer 标识是本地句柄；线格式与 `Link`/`PeerHub` 互通。`STREAM` 有序可靠、自动分片（约 330 KB）；`DATAGRAM` 不重传、≤1300 B；`channel` 0–255 两类各自编号。
- `Send` 不内部排队：窗口满返回 `kWouldBlock`（256 条 / 1 MiB 流控），调用方重试或降级；另有 `kNotConnected`/`kInvalid`/`kFailed`。
- 回调在 `RunOnce` 线程内同步执行；回调内可 `Send`/`Broadcast`，但 `Stop()` 在同线程时只置标志返回，确定性关闭需从其他线程调用。
- 配置透传 `PeerHubConfig`（`max_peers` 16、握手/peer 超时 5 s、可选 32 B PSK）；**限制**：无多 socket 分片与迁移加固，`Listen/Start/Stop` 必须串行，无限流。

## 6. MQTT 3.1.1（`MqttDecoder` / `MqttSession` / `MqttBroker`）

```cpp
go2cpp::bento::net::MqttBroker broker;
broker.OpenSession("sensor-1", nullptr);
std::vector<go2cpp::bento::net::MqttOutbound> out;
broker.HandleBytes("sensor-1", data, size, &out);   // 内部完成 CONNECT/SUBSCRIBE/PUBLISH
for (const auto& m : out) SendTo(m.client_id, m.bytes);
```

- 九种控制报文、VLQ（上限 268435455）、粘包/半包流式解码（非法帧清空缓冲，调用方应关连接）。
- 订阅路由支持 `+`/`#`（`$` 开头主题不被吞）；QoS 0/1（含 PUBACK）；keepalive 1.5× 超时；遗嘱投递。
- `MqttSession` 是**服务端**会话状态机；`MqttBroker` 要求先 `OpenSession` 登记，CONNECT 的 client_id 必须一致，否则 `kProtocolViolation`。
- **限制**：QoS 2 返回 `kUnsupported`；无持久会话（session present 恒 false）、retained、TLS/WS、用户名密码/ACL、共享订阅；UTF-8 只拒内嵌 NUL 与越界；全部非线程安全。

## 7. CoAP（RFC 7252，`CoapClient` / `CoapServer`）

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

- 客户端：单请求在途、自动 Message ID/Token、重传字节与首发一致、`ack_timeout_ms` 2000 起严格翻倍、最多 4 次（无抖动）。
- 服务端：CON 回 piggyback ACK、NON 回新 MID NON；去重缓存按 (Message ID, Token) 命中后重发响应且不重复调 handler；无 handler 时 GET→2.05、PUT→2.04、POST→2.01。
- **限制**：无 DTLS/OSCORE、Observe、Blockwise、Proxy；客户端同时只允许一个 pending；去重缓存默认 8 条且**没有** 247 s EXCHANGE_LIFETIME 窗口；separate response 只支持「空 ACK 后等 CON」，不主动查询；非线程安全。

## 8. SOCKS5（RFC 1928，`go2cpp::bento::socks5::Socks5Server`）

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

- 分两层：协议层 `ParseSocks5Greeting`/`ParseSocks5Request`/`EncodeSocks5Reply` 纯字节编解码；服务端层 accept → 握手 → 拨号 → 中继。
- 只协商无认证 0x00（未提供则回 `05 FF` 并关闭）；ATYP 支持 IPv4/域名/IPv6；成功填 `getsockname` 的 `BND.ADDR/PORT`，失败回 `0.0.0.0:0` + RFC 码；半关闭传播。
- 默认：连接上限 128、握手 10 s、connect 5 s、空闲 60 s、错误响应宽限 2 s、缓冲 64 KiB；`Stats()` 给 accept/拒绝/握手失败/拨号/字节/超时/重置计数。
- **限制**：无用户名密码/GSSAPI、BIND/UDP ASSOCIATE（回 0x07）、UDP 中继、TLS、ACL（0x02 无规则源）；DNS 在驱动线程同步 `getaddrinfo`；除 `Start/Stop` 外同线程调用。

## 9. 选型与当前缺口

- 选型：HTTP API/SSE → `HttpServer`；浏览器双向 → `WsServer`；混合可靠性实时流 → `LinkServer`；受限 UDP 物联网 → CoAP；消息总线 → MQTT；出站隧道 → SOCKS5。
- 缺口：HTTP/WS/SOCKS5 无服务端 TLS（Link 可选 PSK），均无限流与指标导出；6 个模块只有单测，压测/随机/≥5 场景/`audit-*.md` 仍缺。
- 构建：`cmake` 全量配置已恢复（MQTT 编译错误在 `5b80e86` 修复）；`test_mqtt.cpp`/`test_coap.cpp` 仍有 `if (EXISTS ...)` 防御包裹。相关：[bento-roadmap](bento-roadmap.md)、[OmniNetServer 设计](../research/omni-net-design.md)、[审计索引](../../example/WarningDocement/README.md)。
