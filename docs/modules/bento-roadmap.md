# Bento 路线图（2026-10-30 起，四次修订）

> 2026-10-29 晚四次修订（用户澄清）：
> 任务 1 = **底层能力提升**（检查 + Debug + MCP）；
> 任务 2 = **广度调研**（网络场景 × 协议 + OmniNetServer 架构设计）；
> 任务 3 = **多模块开发**（历史网络协议一网打尽，后续）。
>
> 2026-10-30 更新：任务 3 的 6 个服务端骨架（HTTP/SSE、Link、CoAP、MQTT、SOCKS5、WS）全部提交，状态见 §3。

## 任务 1：底层能力提升（检查 + Debug + MCP）

### 1.1 并发 + 网络基础模块检查

| 模块 | 检查 |
|---|---|
| scheduler / fiber / channel / sync / timer | 压测（10 万级）、随机、5 场景、解耦 |
| tcp_socket / udp_socket / io(epoll) | 连接/读写压测、超时/错误、fd 泄漏、边缘触发 |

### 1.2 Socket Debug 模式（设计 + 最小实现）

- 接口：`SocketDebug::Enable/Snapshot/Events/DumpTo(debug::View&)`；
- 开销：Debug 关闭近零；采样可配；
- 接入现有 debug 10 视图 / 事件环；
- 测试：Debug on/off 开销对比、事件完整性。

### 1.3 MCP 插件（开发）

- 工具：`socket_list / socket_stats / socket_events / socket_close`；
- 白名单 + 审计；
- 端到端测试（起 socket → 查询 → 关闭）。

## 任务 2：广度调研（网络场景 × 协议 + OmniNetServer 架构）

### 2.1 广度调研（`docs/research/omni-net-design.md`）

覆盖场景（≥15）：
- 请求-响应（HTTP/1.1、HTTP/2、gRPC）；
- 服务端推送（SSE、WebSocket、长轮询）；
- 实时流（音视频、游戏状态、遥测）；
- 可靠+不可靠混合（Bento STREAM/DATAGRAM、DDS QoS）；
- 消息队列（MQTT、AMQP、Kafka 协议）；
- RPC（gRPC、Thrift、Cap'n Proto）；
- 分布式共识（Raft、Paxos 传输层）；
- 文件/块传输（HTTP 分块、FTP、S3）；
- 发现/注册（mDNS、Consul、etcd）；
- 隧道/代理（SOCKS5、HTTP CONNECT、WireGuard）；
- 组播/广播（UDP multicast、DDS）；
- 远程桌面（RDP/RDRL、VNC）；
- 物联网（CoAP、LwM2M）；
- 工业（Modbus TCP、OPC UA）；
- 数据库协议（PostgreSQL wire、Redis RESP）。

每个场景：适用协议、QoS 需求、Bento 现有能力、缺口。

### 2.2 OmniNetServer 架构设计（"一个虚类太难开发"，先组合式）

```
OmniNetConfig {
  mode: Blocking | NonBlocking     // 默认阻塞/非阻塞
  transport: TCP | UDP | TLS | Memory
  protocols: [HTTP | SSE | WS | Link | Raw | Custom]
  concurrency: Gmp | Thread | Poll
  limits: max_conns, buffers, timeouts
}

OmniNetServer(OmniNetConfig) -> NetServer*
  → AsHttpServer()  -> HttpServer*
  → AsSseServer()   -> SseServer*
  → AsWsServer()    -> WsServer*
  → AsLinkServer()  -> LinkServer*   // 可靠 + 不可靠同时传输
  → AsRawServer()   -> RawServer*

// 统一：收到的、发出的全部通过 Channel<> 传输
Channel<TcpFrame> / Channel<UdpDatagram> / Channel<HttpRequest> ...
```

设计原则：
- **组合而非继承**：`NetServer` 持有 `OmniNetConfig` + 具体 server 实现（pimpl/工厂），不做单一巨虚类；
- **Channel<> 统一入口/出口**：应用从 `Channel` 收、往 `Channel` 发（与 base `concurrency::Channel<T>` 兼容）；
- **阻塞/非阻塞可配**：Config 决定 fd 模式与并发模型（阻塞=线程/非阻塞=Fiber/epoll）；
- **分阶段**：先 LinkServer（复用现有 channel/link），再 HTTP/SSE（复用 base），最后 WS/自定义。

## 任务 3：多模块开发（🟡 6 个骨架已收口，验收进行中）

### 3.1 已落地（2026-10-30，单测 6/6 通过）

| 模块 | 源码 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| HTTP/SSE Server | `include/go2cpp/bento/net/http_server.hpp`、`src/bento/net/http_server.cpp` | ✅ 已提交 `32ff39b` | 9 用例 / 87 checks；压测/场景/audit ⏳ |
| LinkServer（UDP 1:N） | `include/go2cpp/bento/net/link_server.hpp`、`src/bento/net/link_server.cpp` | ✅ 已提交 `280e38e` | 11 用例 / 1469 checks；压测/场景/audit ⏳ |
| CoAP（RFC 7252） | `include/go2cpp/bento/net/coap.hpp`、`src/bento/net/coap.cpp` | ✅ 已提交 `42b70b0` | 24 用例 / 38572 checks；压测/场景/audit ⏳ |
| MQTT 3.1.1 | `include/go2cpp/bento/net/mqtt.hpp`、`src/bento/net/mqtt.cpp` | ✅ 已提交 `5b80e86` | 27 用例 / 36232 checks（含 broker 路由哨兵修复）；压测/场景/audit ⏳ |
| SOCKS5（RFC 1928） | `include/go2cpp/bento/net/socks5.hpp`、`src/bento/net/socks5.cpp` | ✅ 已提交 `04372cb` | 15 用例 / 204 checks（协议层 + 真实回环）；压测/场景/audit ⏳ |
| WebSocket Server | `include/go2cpp/bento/net/ws_server.hpp`、`src/bento/net/ws_server.cpp` | ✅ 已提交 `bad224b` | 8 用例 / 83 checks；压测/场景/audit ⏳ |

- 一览、用法与限制见 [bento-servers.md](bento-servers.md)。
- 构建：`-DGO2CPP_WITH_BENTO=ON` 全量配置通过，6 个测试目标全部注册；
  2026-10-30 实测 `ctest -R "bento_socks5|bento_http_server|bento_ws_server|bento_link_server|bento_mqtt|bento_coap"` 6/6 通过（19.2 s）。

### 3.2 待办（都是验收，不是开发）

- 为 6 个模块补压测、随机、≥5 场景与 `audit-*.md`，每个协议经三遍审查后，再按
  `docs/research/net-scenarios.md` 的门槛改为「验收完成」；
- 补齐服务端缺口：TLS（HTTP/WS/SOCKS5）、连接级限流与指标导出、HTTP/2 与路由参数；
- 后续协议：gRPC 帧、Raft 传输、DDS QoS 等（顺序按 `net-scenarios.md` 的 P2 收益排）。

## 规则

- 任务 1 产出代码+文档；任务 2 产出调研文档；任务 3 产出协议骨架，验收后续；
- 0 警告 + ctest 全过；不引入第三方库。
