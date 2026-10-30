# 网络场景 × 协议广度调研（Bento 视角）

> 日期：2026-10-30 · 工作树：`/mnt/e/CodexWorkspace/Go2Cpp`（WSL）
> 范围：**只做调研文档**。不写实现、不改代码、不 commit。
> 定位：回答「哪些网络场景值得 Bento 承载、Bento v0.1 在每个场景缺什么、先补什么」。
> 配套：`docs/modules/network.md`（标准协议栈现状）、`tests/bento/PROTOCOL.md`（v0.1 线格式）。

---

## 0. 结论摘要

- 覆盖 **22 个场景**：任务指定的 18 个 + 4 个补充场景（高延迟链路、多方协作、V2X、东西向代理）。
- 结论一句话：**Bento 的合理边界是「实时交互类」——游戏、音视频、远程桌面、混合 QoS 消息；其余 14 个场景应复用 `go2cpp::net` 已有栈 + 薄适配层，不应塞进 Bento。**
- 真正的横切缺口只有 6 类（见 §4），其中 **2 类是部署级阻断**、1 类是架构级限制、1 类是高复用单点，另 2 类（协议语义、可观测）不归 Bento 管：
  1. **无安全基座**（无加密 / 无认证 / 无完整性校验）——22 个场景里 **15 个**要求强制加密；
  2. **无速率与拥塞控制**——PTO 纯次数退避（`src/bento/net/channel.cpp` 的 `PtoFor` 只看重传次数，不采样 RTT），高 RTT 链路立即伪重传；单轮最多 256 帧 ≈333KB 突发；
  3. **单 Link = 单对端**（架构级）——监听方锁定第一个 HELLO 来源（`link.cpp` 的 `HandleHello`），服务端扇出靠 N 个 socket，共识/复制/多客户端场景全部受阻；
  4. **无组播**（高复用单点）——一次 socket 层改动同时打开发现、组播、工业、V2X 四个场景。
- 优先级分布：**P0 三项**（安全基座、低延迟可靠恢复、实时流抗丢包）、**P1 六项**（首位是多对端扇出）、**P2 九项**、**P3 四项**。
- **Bento 承载判定**：22 个场景中仅 4 个应演进 Bento（S03 实时流、S04 混合 QoS、S12 远程桌面、S17 游戏），4 个部分承载，14 个应复用 `go2cpp::net` 或不承载。

---

## 1. 调研方法与事实基线

### 1.1 事实来源

本文所有「Bento 现状」判断来自以下代码事实，不凭印象：

| 事实 | 出处 |
|---|---|
| 12B 帧头、7 种帧类型、可选 8B ACK 段 | `include/go2cpp/bento/net/protocol.hpp:37-71` |
| 单帧载荷上限 1300B、不依赖 IP 分片 | `protocol.hpp:47` |
| 可靠窗口 256 / 接收窗口 512、PTO 200ms→2s | `include/go2cpp/bento/net/channel.hpp:47-53` |
| 累积 ACK + **32 位** SACK 位图 | `protocol.hpp:83-87` |
| PTO 仅由重传次数决定，不基于 RTT 样本 | `src/bento/net/channel.cpp` `PtoFor` |
| 监听方锁定首个 HELLO 来源，其余计 `unknown_peer_frames` | `src/bento/net/link.cpp` `HandleHello` |
| 通道上限 8、单轮最多 256 帧 | `link.hpp:208-210` |
| VIDEO 分片 1200B、单帧 ≤256 片（≈307KB）、4 个并发重组、1s 超时 | `channel.hpp:236-244` |
| dep_seq 因果等待 + BARRIER 作废 | `include/go2cpp/bento/net/relation.hpp:34-71` |
| 无 TLS / 无 MAC / 无 token（`src/bento` 全目录检索 tls、加密、MAC 均零命中） | `src/bento/net/` 四个 `.cpp` |
| 弱网实测：20% 双向丢包 + 200ms 延迟下 STREAM 100% 按序 | `tests/bento/PROTOCOL.md`、`cmake/Go2CppBento.cmake:68-72` |

### 1.2 Bento v0.1 能力基线

| 能力 | 状态 | 说明 |
|---|---|---|
| 可靠有序消息 | ✅ | STREAM，PTO 重传 + 去重 + 严格按序交付 |
| 不可靠消息 | ✅ | DATAGRAM，每通道独立 seq，DropStale 保 0 乱序 |
| 视频帧分片 | ✅ | VIDEO，KEY 标志 + 分片重组 |
| 因果依赖 | ✅ | dep_seq 挂起 + BARRIER 释放，弱网下有上限 128 的 pending |
| 加密 / 认证 | ❌ | 完全没有，帧明文可伪造 |
| 拥塞控制 / pacing | ❌ | 无 cwnd、无带宽估计、无发送速率整形 |
| RTT 采样驱动 PTO | ❌ | 固定 200ms 起步 ×2 退避 |
| 快速重传（ACK 跳跃触发） | ❌ | 仅超时重传 |
| 多对端 / 服务端扇出 | ❌ | 一 Link 一 peer |
| 组播 / 广播 | ❌ | `UdpSocket` 只做单播 bind |
| 域名 / IPv6 | ❌ | 仅 IPv4 点分十进制（`link.hpp:45`） |
| 流控协商 | ❌ | 窗口是编译期常量 |
| FEC | ❌ | 无 |
| MTU 探测 | ❌ | 固定 1300B |
| 会话恢复 / 连接迁移 | ❌ | `session_id` 变化即视为对端重启并清空通道状态，无跨会话恢复 |

### 1.3 与 `go2cpp::net` 的分工（重要）

项目里已有两套网络资产，**不要混为一谈**：

- `go2cpp::net`（约 18.7k 行）：TCP/UDP/Unix Socket、Fiber Hook、epoll IOManager、HTTP/1、HTTP/2（帧/HPACK/流状态机）、TLS 客户端、WebSocket、SSE、picoquic 薄封装（默认 OFF）。它是**标准协议栈**，方向是"和生态互通"。
- `go2cpp::bento`（2389 行）：自研 UDP 传输，方向是"低延迟、可控 QoS、端到端自定协议"。

因此本文给每个场景标一个**承载判定**：`Bento 承载`（应演进 Bento）/ `network 承载`（复用标准栈）/ `不承载`（自研无意义）。

### 1.4 QoS 打分口径

四维各 0–3 分，**表示需求强度，不表示 Bento 当前能力**：

- **可靠**：0 = 可全丢（纯采样）；3 = 绝不丢、且必须严格有序。
- **延迟**：0 = 不敏感（分钟级）；3 = 硬实时（端到端 <50ms，抖动敏感）。
- **带宽**：0 = <1KB/s；3 = >10MB/s 或 GB 级对象。
- **安全**：0 = 明文可接受；3 = 强制加密 + 双向认证 + 防重放。

---

## 2. 场景详述

### S01 请求-响应（HTTP/1.1、HTTP/2、gRPC）

- **适用协议**：HTTP/1.1（含 chunked、keep-alive）、HTTP/2（多路复用 + HPACK）、gRPC（HTTP/2 + 5B 长度前缀 + trailer）。
- **QoS**：可靠 3 · 延迟 1 · 带宽 1 · 安全 3
- **Bento 现状**：**不匹配**。Bento 是消息通道，没有"请求/响应关联"概念；STREAM 只能保证有序字节，没有 header 压缩、没有流多路复用、没有 trailer。用它做 RPC 等于重造 HTTP/2 的一个子集。
- **缺口**：① 关联 ID 与超时语义需上层自建；② 无 HPACK 级头部压缩（大 header 场景浪费带宽）；③ 无单连接多并发流（8 通道上限且语义是消息而非流）；④ 无 TLS，公网不可用。
- **优先级**：**P2**。HTTP/1 与 HTTP/2 已在 `go2cpp::net` 实现，gRPC 只差「HTTP/2 + 长度前缀 + `grpc-status` trailer + 超时头」的适配层；注意 `docs/modules/network.md:57-61` 指出的构建缺口——HTTP/2 协议核心**尚未进库目标**，这是使用前必须先解决的工程问题。

### S02 服务端推送（SSE、WebSocket、长轮询）

- **适用协议**：SSE（`text/event-stream` + `Last-Event-ID`）、WebSocket（RFC 6455）、长轮询（HTTP 挂起 + 超时重发）。
- **QoS**：可靠 3 · 延迟 2 · 带宽 1 · 安全 3
- **Bento 现状**：**部分可承载**。STREAM 能给"可靠推送"，DATAGRAM 会丢事件（对推送不可接受）；但 Bento 没有"游标/断点续推"原语——重连后从哪补，协议层不表达，只能业务自己存。
- **缺口**：① 事件游标与断线补推（对应 SSE 的 `Last-Event-ID`）；② 服务端到客户端的背压（慢消费者目前只能丢或不发，无窗口通告）；③ 无 HTTP upgrade 能力（WebSocket 已由 `go2cpp::net` 提供，不必重做）；④ 无心跳与 half-open 检测之外的"会话存活"语义（Bento 有 heartbeat，但无 Last-Event 概念）。
- **优先级**：**P1**。若 Bento 要做"游戏内通知/配置下发/对局事件"，游标补推比加密更早暴露。SSE 与 WebSocket 走 `go2cpp::net` 即可，Bento 只补"可靠推送 + 游标"这一小块。

### S03 实时流（音视频、游戏状态、遥测）

- **适用协议**：RTP/RTCP（RFC 3550）、WebRTC（SRTP + DataChannel）、RTSP 承载、gRPC server-streaming、MQTT（遥测）。
- **QoS**：可靠 1（丢旧帧优于等新帧）· 延迟 3 · 带宽 3 · 安全 2
- **Bento 现状**：**最匹配的场景之一**。VIDEO 通道（1200B 分片、KEY 标志、4 路并发重组、1s 超时丢弃）与 DATAGRAM 的 DropStale 正好是实时流需要的"宁可丢、不要等"。
- **缺口**：① **无 FEC**——20% 丢包下靠 PTO 200ms 起步重传，端到端延迟直接翻倍，实时流不能接受；② **无带宽估计与码率自适应**——无 cwnd、无 pacing，突发会自伤丢包；③ **无帧间依赖表达**——P 帧丢失后解码器花屏，协议没有"请求 IDR/参考帧丢失"的反向信令（对应 RTCP PLI/FIR）；④ **无时间戳与抖动缓冲**——只有 `frame_id`，没有采集时刻，接收端无法算 jitter 或做平滑播放；⑤ 单帧 307KB 上限（`kMaxFragments=256`），未压缩高分辨率帧会被拒。
- **优先级**：**P0**。这是 Bento 存在的主要理由；FEC + 带宽估计 + 关键帧请求是该场景从"能跑"到"能用"的分水岭。

### S04 可靠 + 不可靠混合（STREAM/DATAGRAM、DDS QoS）

- **适用协议**：Bento 自身三通道模型、DDS（RTPS，22 个 QoS 策略）、SCTP（多流 + 部分可靠）、QUIC（多流 + 可重置）。
- **QoS**：可靠 2–3（按消息类）· 延迟 3 · 带宽 2 · 安全 2
- **Bento 现状**：**核心优势已具备**。三类通道 + dep_seq 因果等待 + BARRIER 作废，正好对应"输入必须可靠、状态可丢、事件要因果"的混合需求，且弱网实测通过。
- **缺口**：① **无 QoS 描述符**——DDS 的 `RELIABILITY / DURABILITY / DEADLINE / LIVELINESS / HISTORY / OWNERSHIP / TIME_BASED_FILTER / TRANSPORT_PRIORITY` 一个都没有，策略全是编译期常量；② **通道数上限 8**（`link.hpp:208`），且三种通道各一套，实际可用组合更少；③ **无 deadline 违约通知**——DDS 的 deadline 到期要回调，Bento 只有超时断链；④ **无 per-message 优先级**——所有 STREAM 消息在同一条有序 FIFO 里，队头阻塞无法被高优先级消息绕过。
- **优先级**：**P1**。不必做全 22 个策略，做 6 个（可靠/持久/期限/存活/历史/优先级）+ 通道优先级调度即可覆盖 80% 用例。队头阻塞是当前最容易被真实业务撞上的问题。

### S05 消息队列（MQTT、AMQP、Kafka 协议）

- **适用协议**：MQTT 3.1.1/5.0（QoS 0/1/2、Will、keepalive、主题树）、AMQP 0-9-1（channel 多路复用 + method/header/body 帧 + 事务）、Kafka 协议（correlation id + 批量 + 压缩 + 消费者组 + 幂等生产者）。
- **QoS**：可靠 3 · 延迟 1 · 带宽 2 · 安全 3
- **Bento 现状**：**不承载**。Bento 没有 broker、没有主题订阅、没有持久化、没有会话状态，做 MQ 是在错误层做错事。
- **缺口**：与 Bento 无关；真正缺口在适配层——MQTT 需要 TCP + 主题树 + 两级确认（PUBREC/PUBREL 的恰好一次）+ 遗嘱 + 会话过期；Kafka 需要长连接 + 请求流水线 + 批量压缩 + 组协调（JoinGroup/SyncGroup/Heartbeat）；AMQP 需要 channel 多路复用与流控（`channel.flow`）。
- **优先级**：**P3**（自研 broker 价值低）/ **P2**（MQTT 客户端适配器，因为 IoT 场景 S13 与它同源）。Kafka 客户端协议复杂度最高、收益最低，不建议本轮碰。

### S06 RPC（gRPC、Thrift、Cap'n Proto）

- **适用协议**：gRPC（HTTP/2 + trailer）、Thrift（TBinary/TCompact + TMessage 头 + TFramedTransport）、Cap'n Proto RPC（零拷贝 + capability 引用传递 + E-order）。
- **QoS**：可靠 3 · 延迟 2 · 带宽 2 · 安全 3
- **Bento 现状**：**不匹配**。STREAM 能运字节，但 RPC 要的是"方法路由 + 请求 ID + 错误状态 + 元数据 + 流控"。Cap'n Proto 的 capability 传递（把远端对象当参数传）在消息模型里无对应物。
- **缺口**：① 方法路由与序列化格式不在传输层职责内，但传输层需给"请求 ID + 取消 + 截止时间"原语；② Thrift/Cap'n Proto 都需要"一个大消息跨多帧"的流式 API（Bento 单消息 ≤1300B，需上层切分）；③ 无截止时间传播（deadline 透传到对端并触发取消）；④ gRPC 的 `grpc-status` 在 trailer，Bento 无"头部/主体/trailer"三段式。
- **优先级**：**P2**。gRPC 复用 `go2cpp::net` 的 HTTP/2，只需补"HTTP/2 进库目标 + 长度前缀 + trailer"；Thrift/Cap'n Proto 属于"有需求再做"，Cap'n Proto 的 capability 模型与 Bento 消息模型根本不合。

### S07 分布式共识（Raft、Paxos 传输）

- **适用协议**：Raft（AppendEntries/RequestVote/InstallSnapshot）、Paxos（1a/1b/2a/2b）、通常跑在 TCP 或 gRPC 上。
- **QoS**：可靠 3（且严格有序 + 持久）· 延迟 2 · 带宽 0 · 安全 3
- **Bento 现状**：**部分可承载、但缺关键前提**。STREAM 给有序可靠，但：① 发送队列在内存（`m_inflight` / `m_unsent` 都是 `std::deque`），进程崩溃即丢 inflight；② 一 Link 一 peer，Raft 需要 leader 并行向 N 个 follower 复制；③ 无节点身份/证书，term 与传输层无关联。
- **缺口**：① **持久化发送队列**（日志落盘后才算已发，这是 Raft 正确性前提，不是优化）；② **多对端并行复制**（1→N 扇出 + 每 peer 独立窗口）；③ 节点认证与防冒充；④ 快照传输需要大对象分块（当前 ≤1300B/消息）。
- **优先级**：**P1**。多对端扇出是共识/复制/多客户端的共同硬前提（与 S11、S20 共用），优先级高于"共识协议本身"。**共识算法不应由传输层实现**，Bento 只提供"可持久、可扇出、有身份"的通道。

### S08 文件 / 块传输（HTTP 分块、FTP、S3）

- **适用协议**：HTTP/1.1 chunked、HTTP/2 DATA、FTP（控制 + 数据双连接）、S3 REST（分段上传 + 预签名）。
- **QoS**：可靠 3 · 延迟 0 · 带宽 3 · 安全 3
- **Bento 现状**：**不适合**。STREAM 单消息 ≤1300B、窗口 256（在途上限 ≈333KB），且无拥塞控制——吞吐会远低于 TCP。用它传文件是拿最弱的维度去碰最硬的场景。
- **缺口**：与 Bento 无关。真正的缺口在 `go2cpp::net` 侧：① HTTP/1 `Parser` body 是有限内存缓冲（默认 64MiB），大文件必须走 `ResponseWriter` 且自设上限；② 无 Range 请求/断点续传封装；③ 无多连接并行下载与限速；④ 无内容校验（CRC/哈希）与重试策略。
- **优先级**：**P2**。走 HTTP/1.1 chunked + `ResponseWriter` 就能覆盖多数需求；S3 只需签名与分段上传的适配。**明确不做**：不要为文件传输给 Bento 加"可靠大流"模式，那是 QUIC 的活且已有 picoquic 薄封装。

### S09 服务发现（mDNS、Consul、etcd）

- **适用协议**：mDNS/DNS-SD（5353 组播、PTR/SRV/TXT、TTL 与 goodbye）、Consul（HTTP API + 阻塞查询 + Serf gossip）、etcd（gRPC watch 双向流 + lease）。
- **QoS**：可靠 2 · 延迟 1 · 带宽 0 · 安全 2
- **Bento 现状**：**不承载**。`UdpSocket` 只做单播 bind，无 `IP_ADD_MEMBERSHIP`，收不到 5353 组播；也没有 DNS 报文编解码。
- **缺口**：① **组播加入/离开与 TTL 控制**（与 S11 同一缺口）；② mDNS 需要 DNS 记录编解码 + 缓存 + 主动 goodbye（TTL=0）；③ Consul/etcd 走 HTTP/gRPC，属 `go2cpp::net` 职责；④ 无服务实例的健康探测与本地缓存淘汰策略。
- **优先级**：**P1**（只做"组播 socket 能力"这一层）。理由：组播缺口同时卡住 S09、S11、S14、S21 四个场景，是全表**复用度最高的单点缺口**；而 mDNS 报文解析可以后置。

### S10 隧道 / 代理（SOCKS5、HTTP CONNECT、WireGuard）

- **适用协议**：SOCKS5（RFC 1928，CONNECT/BIND/UDP ASSOCIATE + 用户名密码认证 RFC 1929）、HTTP CONNECT（`200 Connection Established` 后裸字节）、WireGuard（Noise_IKpsk2 + ChaCha20-Poly1305 + AllowedIPs 路由）。
- **QoS**：可靠 3 · 延迟 3 · 带宽 3 · 安全 3
- **Bento 现状**：**不承载**。隧道要的是"双向字节流"，Bento 是"≤1300B 消息"；且完全无加密，做隧道等于明文代理。
- **缺口**：① 无字节流抽象（`SendStream` 是消息，不是流；隧道需在上层做分片/重组/窗口）；② 无加密与防重放（WireGuard 的 Cookie 抗 DoS 机制、Nonce 单调性都没有对应物）；③ 无 NAT 穿透（STUN/打洞）能力；④ 无 UDP ASSOCIATE 需要的"每目标地址一条逻辑流"多路复用。
- **优先级**：**P2**（SOCKS5/CONNECT 简单，走 `go2cpp::net` 的 Socket 即可，几十行适配）；**P3**（WireGuard——它是完整密码协议，含握手状态机与路由表，自研风险远大于收益，应链 upstream 的 `wireguard-go`/`boringtun` 或直接用内核实现）。

### S11 组播 / 广播（UDP multicast、DDS）

- **适用协议**：IPv4/IPv6 组播（IGMP、`IP_ADD_MEMBERSHIP`、`IP_MULTICAST_TTL/LOOP`）、广播（`255.255.255.255` / 子网广播）、DDS 的 RTPS 默认走组播发现。
- **QoS**：可靠 1（可降级为 NACK）· 延迟 2 · 带宽 3（1→N 放大）· 安全 1
- **Bento 现状**：**完全不支持**。单播单 peer 是架构级限制。
- **缺口**：① socket 层组播加入/退出、TTL、回环开关；② **可靠组播的反馈抑制**（NACK 风暴会让 1→N 退化成 N→1 雪崩，需要随机延迟 + 抑制）；③ 发送端不知道成员数，无法做速率适配；④ 广播地址与子网掩码（`IPv4Address` 已有 `broadcast_address()`，但 Bento 未使用）。
- **优先级**：**P1**。组播是"一次投入、四处复用"（S09 发现、S11 组播、S14 工业、S21 V2X）。但要注意：**组播与"可靠有序"天然冲突**，Bento 只能提供"组播上的不可靠 + 应用层 FEC"，不要试图给组播加 STREAM。

### S12 远程桌面（RDP/RDRL、VNC）

- **适用协议**：RDP（TPKT/X.224/MCS 多层 + 动态虚拟通道 + EGFX 图形管道 + RDRL 的 UDP 传输扩展）、VNC/RFB（Raw/CopyRect/Hextile/ZRLE/Tight 编码，通常 TCP 5900，VeNCrypt/TLS 可选）。
- **QoS**：可靠 2（输入必须可靠、画面可丢旧帧）· 延迟 3 · 带宽 3 · 安全 3
- **Bento 现状**：**架构上贴合**。STREAM（键鼠输入）+ VIDEO（画面 tile）+ DATAGRAM（光标位置）的组合正是 RDP 的思路；VIDEO 的"旧 frame_id 直接丢弃"对应 tile 过期丢弃。
- **缺口**：① **无 tile/区域语义**——VIDEO 是整帧，远程桌面是"脏矩形"，整帧重传会浪费带宽；② **无通道优先级调度**——输入被画面帧的队头阻塞会直接表现为"卡顿感"；③ **无带宽探测驱动的画质降级**（RDP 的动态分辨率/码率）；④ 无剪贴板/音频重定向/打印机等虚拟通道抽象；⑤ 无加密（RDP 有 CredSSP + RC4/AES 历史包袱，Bento 从零设计反而可以只做 AEAD）。
- **优先级**：**P1**。与 S03 共享 FEC/带宽估计缺口；额外只需补"通道优先级 + 区域/tile 语义"。这是 Bento 除游戏外**第二个真实目标场景**。

### S13 物联网（CoAP、LwM2M）

- **适用协议**：CoAP（RFC 7252：4B 头 + Token + Option 编码 + CON/NON、Observe RFC 7641、Block-wise RFC 7959）、DTLS 1.2（RFC 6347）/ OSCORE（RFC 8613）、LwM2M（注册/管理接口，OMA 对象模型）。
- **QoS**：可靠 1（CoAP 用 CON 可选确认）· 延迟 1 · 带宽 0 · 安全 3
- **Bento 现状**：**不适合**。12B 帧头 + 12B 最小帧在 NB-IoT/LoRa 的几十字节载荷上开销过大；也没有 CoAP 的 Token 匹配与 Observe 通知模型。
- **缺口**：① 报文开销（Bento 头 12B 固定，CoAP 头 4B + 变长 option）；② DTLS/OSCORE（与 Bento 的加密缺口同源，但 DTLS 是记录层协议，不能只加个 AEAD 了事）；③ Observe 的"通知序号 + 重注册"语义；④ Block1/Block2 的块传输协商；⑤ LwM2M 的对象模型与注册生命周期。
- **优先级**：**P2**。CoAP 的完整实现（含 DTLS）成本不低，但它是"能跑在 UDP 上的物联网标准"，比让 Bento 变形更合理。若做，建议作为 `go2cpp::net` 的独立协议模块，**不进 Bento**。

### S14 工业（Modbus TCP、OPC UA）

- **适用协议**：Modbus TCP（MBAP 7B 头 + 功能码 PDU，端口 502，无内建认证）、OPC UA（UA-TCP 分帧 + UA Secure Conversation + 信息模型 + 订阅/发布，PubSub 可走 UADP over UDP）。
- **QoS**：可靠 3（且周期性确定）· 延迟 2 · 带宽 0 · 安全 3（OPC UA 强制 X.509）
- **Bento 现状**：**不承载**。工业要的是确定性时序与长周期稳定连接，Bento 无队列调度保证、无时间同步、无连接保活之外的确定性。
- **缺口**：① Modbus TCP 极简（MBAP + 功能码 + 事务 ID），一天可做，但**无认证**是它自身的历史缺陷，改造需 Modbus Security/TLS；② OPC UA 的 UA Secure Conversation（非对称握手换对称密钥 + 分块签名加密 + 序列号/请求 ID）是完整安全层，自研成本高；③ 信息模型（NodeId/地址空间/服务集）与传输无关但工作量大；④ 无 PTP/TSN 级时间同步。
- **优先级**：**P2**（Modbus TCP——最简单、最容易验证的协议适配样板，适合作为"协议适配器框架"的第一个用例）/ **P3**（OPC UA——建议链 open62541，不自研）。

### S15 数据库协议（PostgreSQL wire、Redis RESP）

- **适用协议**：PostgreSQL 前端/后端协议（StartupMessage、SCRAM-SHA-256、Simple/Extended Query 的 Parse/Bind/Describe/Execute/Sync、COPY）、Redis RESP2/RESP3（`+ - : $ *` 与 `% ~ > |`、inline 命令、pipeline、pub/sub、Cluster MOVED/ASK）。
- **QoS**：可靠 3 · 延迟 2（每查询一次 RTT）· 带宽 2 · 安全 3
- **Bento 现状**：**不承载**。DB 协议要求 TCP 字节流 + 严格顺序 + 大结果集流式，Bento 的 UDP + 消息模型两者都不满足。
- **缺口**：与 Bento 无关；适配层缺口：① RESP 需要"读到完整帧"的增量解析（`Framer` 的三种模式都不匹配 `*N\r\n$len\r\n` 嵌套数组，需专用解析器）；② Postgres 需要 SCRAM 的 SASL 握手状态机与二进制格式编解码；③ pipeline/批量与背压；④ TLS 升级（Postgres 的 `SSLRequest`、Redis 的 `STREAM` 命令）。
- **优先级**：**P2**。**RESP 是最佳的第一个协议适配器样板**——文本、无握手、帧边界清晰、可本地起 redis-server 做互通验证。Postgres 复杂度高一个量级（SCRAM + 扩展查询 + 类型 OID），有需求再做。

### S16 流媒体（RTSP、HLS）

- **适用协议**：RTSP（RFC 2326/7826：DESCRIBE/SETUP/PLAY/TEARDOWN + SDP）、RTP/RTCP（12B 头 + 时间戳 + SSRC + RR/SR/NACK/PLI）、HLS（RFC 8216 m3u8 + TS/fMP4 + LL-HLS 部分片段）。
- **QoS**：可靠 1（RTP 可丢）· 延迟 3（RTSP）/ 0（HLS）· 带宽 3 · 安全 1（SRTP）/ 3（HTTPS）
- **Bento 现状**：**部分接近**。VIDEO 通道的分片 + 丢弃策略接近 RTP 语义，但缺 RTP 的三个关键字段：**时间戳**（算 jitter、做音画同步）、**SSRC**（多源区分）、**payload type**（编码格式协商）。
- **缺口**：① RTP 头语义（时间戳/SSRC/PT/序号 16 位 vs Bento 的 32 位 frame_id）；② **RTCP 反馈通道**（NACK 重传请求、PLI/FIR 关键帧请求、RR 接收报告）——这与 S03 的"关键帧请求"是同一设计；③ RTSP 是文本控制通道，需要独立的命令/响应状态机；④ SRTP 加密（与安全缺口同源）；⑤ HLS 走 HTTP，属 `go2cpp::net`。
- **优先级**：**P1**（只做 RTCP 反馈子集：NACK + PLI）。它与 S03 共用设计，一次投入覆盖"实时流"与"流媒体"两个场景；RTSP 全文与 HLS 解析不必自研。

### S17 游戏（ENet、KCP、QUIC）

- **适用协议**：ENet（UDP + 多 channel + 可靠/不可靠 + 序号窗口 + ACK 位图 + RTT 自适应 RTO）、KCP（ARQ + 快速重传 + 不翻倍 RTO + 可配流控）、QUIC（RFC 9000/9001/9002：TLS1.3 集成、0-RTT、连接迁移、多流、丢包检测 + CUBIC/BBR）。
- **QoS**：可靠 2（混合）· 延迟 3 · 带宽 2 · 安全 1（传统游戏）/ 3（QUIC 路线）
- **Bento 现状**：**正面对标 ENet/KCP，主场**。已有三通道、累积 ACK + SACK、PTO 退避、drop-stale、因果原语，且弱网实测 100% 按序。
- **缺口**：① **无快速重传**——KCP 用"跳过 2 个 ACK 即重传"把恢复时间从 RTO 降到 ~1 RTT，Bento 只有超时（200ms 起步），丢包恢复慢一个量级；② **无 RTT 采样驱动 RTO**——高 RTT 链路（>200ms）会立即伪重传，制造无效流量；③ **SACK 位图仅 32 位 vs 窗口 256**——高丢包下 ACK 无法完整描述接收窗口，导致冗余重传；④ **无拥塞控制**（KCP 可关，但公网必须有）；⑤ **无加密**（QUIC 把加密做成强制，这是现代游戏的合规趋势）；⑥ 无连接迁移（切网断线）。
- **优先级**：**P0**。这是 Bento 的立身场景，缺 ① ② ③ 会让它在与 KCP 的对比中直接落败。**先做快速重传 + RTT 采样 RTO + 扩宽 SACK，再做拥塞控制与加密**——前者是单人可验证的局部改动，后者是架构级。

### S18 金融（FIX 协议）

- **适用协议**：FIX 4.2/4.4/5.0 SP2、FIXT（FIX over TLS）、FIXP、SBE 二进制编码；会话层含 Logon/Logout/Heartbeat/TestRequest/ResendRequest/SequenceReset/GapFill。
- **QoS**：可靠 3（绝不重复成交回报）· 延迟 3（微秒–毫秒）· 带宽 0 · 安全 3
- **Bento 现状**：**不承载，且语义冲突**。三个硬冲突：① Bento 的 PTO 重传会在"已送达但 ACK 丢失"时产生**重复消息**，而 FIX 的成交回报重复是重大事故（FIX 靠 MsgSeqNum + PossDupFlag 去重，但那要求会话层持久化序号）；② FIX 会话序号是**长生命周期单调递增**，与 Bento 的 32 位通道内 seq 和重连重置语义不匹配；③ 无持久化。
- **缺口**：① tag=value 编解码 + `8/9/10` 三字段校验（BodyLength 与 Checksum 必须严格）；② 会话层持久化（序号、已发已收消息存储）；③ ResendRequest/GapFill 的重传语义（与"传输层重传"是**两套独立机制**，必须分开）；④ 微秒级时间戳（`SendingTime` 精度）；⑤ TLS + 消息级认证。
- **优先级**：**P3**。金融机构不会采用自研 UDP 传输；即便做，也只能做 FIX 会话层适配（跑在 TCP/TLS 上）。**明确不做**：不要为 FIX 改 Bento 的重传语义。

### S19 高延迟 / 断续链路（卫星、远洋、深空）【补充】

- **适用协议**：DTN（Bundle Protocol RFC 5050/9171）、TCP 长肥管道（需 BBR 类拥塞控制）、QUIC（可配初始 RTT）。
- **QoS**：可靠 2（可容忍延迟送达）· 延迟 0（不敏感）· 带宽 1 · 安全 2
- **Bento 现状**：**会退化**。PTO 固定 200ms 起步、上限 2s，在 RTT 600ms 的 GEO 链路上每次丢包都误判为超时；窗口 256 × 1300B 在 600ms RTT 下理论吞吐仅 ≈550KB/s（且无 pacing 会突发）。断链后无"重连续传"语义，`session_id` 变化会清空所有通道状态。
- **缺口**：① RTT 采样与 RTO 自适应（与 S17 同一缺口）；② 大初始窗口 / 慢启动；③ **断连续传**（会话跨断链保持，而非 `m_reliable.clear()`）；④ 存储转发（Store-and-Forward，DTN 的核心，Bento 完全没有）。
- **优先级**：**P2**。补 ① 后此场景自然改善；③ ④ 属 DTN 范畴，与 Bento 的实时定位冲突，不建议做。

### S20 多方实时协作（WebRTC DataChannel / SCTP）【补充】

- **适用协议**：WebRTC（SCTP over DTLS over ICE）、SCTP（多流 + 部分可靠 + 消息边界）、CRDT 同步协议（Yjs/Automerge 的二进制更新）。
- **QoS**：可靠 2 · 延迟 2 · 带宽 2 · 安全 3
- **Bento 现状**：**架构冲突**。单 Link 单 peer，多方协作需要 mesh（N×(N-1)/2 连接）或 SFU 中转；Bento 两者都没有——没有连接表、没有转发、没有成员管理。
- **缺口**：① 多对端连接表与扇出（与 S07 同一缺口）；② 中转/转发模式（一个 Link 收、多个 Link 发）；③ 部分可靠的多流（SCTP 的 `PR-SCTP` 语义，Bento 的通道模型接近但无"部分可靠"中间态）；④ DTLS/SRTP 级加密与 ICE 打洞。
- **优先级**：**P2**。补多对端后可做"小型 mesh"，但 ICE/DTLS 是 WebRTC 互通的硬门槛，成本高于收益。若目标只是"自家客户端之间协作"，Bento 多对端 + 自有加密就够，但那不是 WebRTC。

### S21 车联网 V2X（DSRC / C-V2X）【补充】

- **适用协议**：IEEE 802.11p / DSRC、C-V2X（LTE-V2X PC5）、ETSI ITS-G5 的 GeoNetworking + BTP、SAE J2735 消息集（BSM/CAM/DENM）。
- **QoS**：可靠 1（周期性广播，丢一帧可接受）· 延迟 3（<100ms，碰撞预警）· 带宽 1 · 安全 3（强制 PKI 证书 + 假名化）
- **Bento 现状**：**不承载**。既无组播/广播，也无安全凭证体系；单播单 peer 与"1→N 周期性广播"根本对立。
- **缺口**：① 组播/广播（与 S11 同一缺口）；② 地理路由（按位置定向广播，GeoNetworking 的核心）；③ **安全凭证管理**（SCMS/CCMS 的假名证书轮换与撤销列表）——这是车联网最重的部分；④ 极短帧（BSM 约 100–300B，Bento 头开销可接受但需免握手）。
- **优先级**：**P3**。安全凭证体系（SCMS）远超本项目范围；没有它任何 V2X 部署都不合规。仅作场景完整性登记。

### S22 集群东西向 / Sidecar 代理【补充】

- **适用协议**：HTTP/1.1 keep-alive、HTTP/2 多路复用、gRPC、mTLS（SPIFFE/SVID）、PROXY protocol。
- **QoS**：可靠 3 · 延迟 2 · 带宽 3 · 安全 3
- **Bento 现状**：**不承载**。这是"高并发连接复用 + mTLS + 可观测"的场景，Bento 单 peer、无 TLS、无连接池。
- **缺口**：① 连接池与连接复用（`http::Client` 当前"每请求独占连接"，`docs/modules/network.md:154`）；② mTLS 与服务身份（当前只有客户端 TLS，无服务端 TLS，`docs/modules/network.md:176`）；③ 代理协议头（PROXY protocol v1/v2）；④ 限流/熔断/重试预算；⑤ 可观测（连接级指标、分布式追踪透传）。
- **优先级**：**P2**。这是 `go2cpp::net` 的补齐方向而非 Bento；其中"服务端 TLS + 连接池"是生产可用性的硬门槛，值得单独立项。

---

## 3. 汇总矩阵

| # | 场景 | 主协议 | QoS（可/延/带/安） | 承载判定 | 优先级 | 主缺口 |
|---|---|---|---|---|---|---|
| S01 | 请求-响应 | HTTP/1.1、HTTP/2、gRPC | 3/1/1/3 | network 承载 | P2 | gRPC trailer 与 HTTP/2 进库目标 |
| S02 | 服务端推送 | SSE、WebSocket、长轮询 | 3/2/1/3 | 部分 Bento | P1 | 事件游标补推 + 背压 |
| S03 | 实时流 | RTP/RTCP、WebRTC | 1/3/3/2 | **Bento 承载** | **P0** | FEC、带宽估计、关键帧请求、时间戳 |
| S04 | 可靠+不可靠混合 | Bento、DDS/RTPS、SCTP | 2–3/3/2/2 | **Bento 承载** | P1 | QoS 描述符、通道优先级、deadline |
| S05 | 消息队列 | MQTT、AMQP、Kafka | 3/1/2/3 | 不承载 | P3/P2 | 主题树、两级确认、组协调 |
| S06 | RPC | gRPC、Thrift、Cap'n Proto | 3/2/2/3 | network 承载 | P2 | deadline 传播、大消息流式 |
| S07 | 分布式共识 | Raft、Paxos | 3/2/0/3 | 部分 Bento | P1 | 持久化发送队列、多对端扇出、节点身份 |
| S08 | 文件/块传输 | HTTP chunked、FTP、S3 | 3/0/3/3 | network 承载 | P2 | Range/续传/多连接/校验 |
| S09 | 服务发现 | mDNS、Consul、etcd | 2/1/0/2 | network 承载 | P1（仅组播） | 组播 socket、DNS 报文 |
| S10 | 隧道/代理 | SOCKS5、CONNECT、WireGuard | 3/3/3/3 | network 承载 | P2/P3 | 字节流抽象、加密、打洞 |
| S11 | 组播/广播 | UDP multicast、DDS | 1/2/3/1 | **Bento 缺失** | P1 | 组播成员管理、NACK 抑制 |
| S12 | 远程桌面 | RDP/RDRL、VNC | 2/3/3/3 | **Bento 承载** | P1 | tile 语义、通道优先级、画质自适应 |
| S13 | 物联网 | CoAP、LwM2M | 1/1/0/3 | 不承载 | P2 | 报文开销、DTLS/OSCORE、Observe |
| S14 | 工业 | Modbus TCP、OPC UA | 3/2/0/3 | 不承载 | P2/P3 | 安全通道、信息模型、时间同步 |
| S15 | 数据库协议 | PostgreSQL wire、RESP | 3/2/2/3 | 不承载 | P2 | 嵌套帧解析、SASL、pipeline |
| S16 | 流媒体 | RTSP、HLS、RTP | 1/3/3/1 | 部分 Bento | P1 | RTP 头语义、RTCP 反馈 |
| S17 | 游戏 | ENet、KCP、QUIC | 2/3/2/1 | **Bento 承载** | **P0** | 快速重传、RTT-RTO、SACK 扩宽、加密 |
| S18 | 金融 | FIX 4.4/5.0、SBE | 3/3/0/3 | 不承载 | P3 | 会话层持久化、序号恢复 |
| S19 | 高延迟链路 | DTN、TCP-BBR、QUIC | 2/0/1/2 | 部分 Bento | P2 | RTO 自适应、断连续传 |
| S20 | 多方协作 | WebRTC/SCTP、CRDT | 2/2/2/3 | **Bento 缺失** | P2 | 多对端、转发模式、DTLS/ICE |
| S21 | 车联网 V2X | DSRC、C-V2X、GeoNetworking | 1/3/1/3 | 不承载 | P3 | 地理路由、SCMS 凭证 |
| S22 | 东西向/Sidecar | HTTP/2、gRPC、mTLS | 3/2/3/3 | network 承载 | P2 | 服务端 TLS、连接池、可观测 |

**分布**：Bento 承载 4 个（S03/S04/S12/S17）、部分承载 4 个（S02/S07/S16/S19）、network 承载 6 个（S01/S06/S08/S09/S10/S22）、不承载 8 个（S05/S11/S13/S14/S15/S18/S20/S21）。
**信号很明确**：22 个场景里只有 4 个真正属于 Bento，其中 2 个（S03/S17）是 P0。**广度调研的产出不是"扩 Bento 的功能列表"，而是"确认 Bento 的边界"**。

---

## 4. 缺口聚类（横切分析）

把 22 个场景的缺口按"改一处、多场景受益"聚类，得到 6 类。**只有前 4 类是 Bento 该管的**。

### G1 安全基座缺失（影响 15/22 个场景）

安全需求为 3 的场景有 15 个：S01、S02、S05、S06、S07、S08、S10、S12、S13、S14、S15、S18、S20、S21、S22。Bento 当前**零加密、零认证、零完整性**：帧头无 MAC，载荷明文，任何人可伪造 STREAM 帧注入业务。

这不是"某场景的缺口"，是**整个传输层的部署前提**。任何在公网或半可信内网跑的 Bento 应用今天都是不安全的。

需要的最小集合（不是全部）：
1. 握手阶段的密钥协商与对端认证（可用 Noise IK 类模式，或 TLS 1.3 的 record 层复用）；
2. 数据帧 AEAD（ChaCha20-Poly1305 或 AES-GCM），含**单调 nonce**（32 位 seq 天然可做 nonce 的一半，但需绑定方向与 session）；
3. 重放窗口（AEAD 失败即丢，配合 `m_rx_seen` 已有结构）；
4. 密钥轮换与 session_id 绑定（当前 `session_id` 只用于重启检测，可复用为密钥代际）。

**关键判断**：不要自己发明密码协议。Noise Framework 的 `IK`/`NK` 模式或直接复用 OpenSSL 的 TLS 1.3（项目已依赖 OpenSSL，`docs/modules/network.md:54-56`）都比手写握手安全。

### G2 速率与拥塞控制缺失（影响 6 个场景）

S03、S08、S17、S19、S22、S12。当前 Bento 没有 cwnd、没有 pacing、没有带宽估计，PTO 是纯次数退避（`PtoFor` 只看 `retransmits`）。两个直接后果：

1. **高 RTT 链路伪重传**：RTT 600ms 时，200ms 的 PTO 会在 ACK 到达前就重传，每次丢包被放大成 3 次发送。
2. **突发自伤**：一次 `RunOnce` 可发最多 256 帧（`kMaxFramesPerRun`），1300B × 256 ≈ 333KB 瞬时突发，在瓶颈链路必然丢包，而丢包又触发重传，形成正反馈。

最小可做顺序：**RTT 采样（已有 `last_rtt_ms` 字段但未参与 PTO）→ RTO = SRTT + 4×RTTVAR 且带 Karn 算法 → 快速重传（3 次重复 ACK 或 SACK 空洞）→ 简单 cwnd（NewReno 起始 10 包）→ pacing**。这一步做完，S17 才真正能和 KCP 比。

### G3 多对端扇出缺失（影响 4 个场景）

S07、S12、S20、S22。监听方在 `HandleHello` 里锁定第一个来源，之后其他来源一律计入 `unknown_peer_frames`。这意味着**服务端只能服务一个客户端**，多客户端要 N 个 `Link` 实例（N 个 socket、N 个端口或 `SO_REUSEPORT`）。

需要的是"连接表 + 每对端独立通道状态 + 事件回调带 peer 标识"，而不是给 `Link` 加锁（`link.hpp:5-9` 明确写了单线程不变量，破坏它会引入一整类并发 bug）。

### G4 组播/发现缺失（影响 4 个场景）

S09、S11、S14、S21。`UdpSocket::Open` 只有 `bind`，无 `setsockopt(IP_ADD_MEMBERSHIP)`、无 `IP_MULTICAST_TTL/LOOP`、无子网广播地址使用。这是**复用度最高的单点缺口**（一次 socket 层改动打开 4 个场景）。

注意：组播上不能跑 STREAM（可靠有序在 1→N 上会退化成 N 个独立重传流）。正确做法是"组播传不可靠 + 应用层 FEC + NACK 抑制"，这条路径与 S03 的 FEC 是同一条技术线。

### G5 协议语义缺口（不是传输层该管的，影响 8 个场景）

S01、S05、S06、S13、S14、S15、S18、S21。这些场景的缺口（HPACK、主题树、SCRAM、FIX 会话、SCMS 凭证）**全部属于应用/会话层**。把它们塞进 Bento 会让传输层变成"协议大杂烩"，违背 Bento 的极简定位。

正确处理：在 `go2cpp::net` 之上做**协议适配器框架**（统一 `Transport` 抽象 + 每协议一个模块），S15 的 RESP 是最佳第一个样板。

### G6 可观测与运维缺口（影响全部场景）

当前 `LinkStats` / `ReliableChannel::Stats` 有计数器，但缺：
- **延迟分布**（p50/p99 RTT、重传延迟、乱序深度）——只有 `last_rtt_ms` 一个标量；
- **每场景 QoS 达标度量**（如"99% 的消息在 100ms 内送达"），这是判断补丁是否有效的唯一依据；
- 与项目已有 debug 视图 / MCP 的接入（Bento 目前完全独立，不在 `DebugTooling` 里）。

没有 G6，G1–G4 的任何改动都无法**证明**有效——只能靠"跑通了"。

---

## 5. 优先级与执行顺序

### P0（3 项）——不做则 Bento 不可部署 / 不可比较

| ID | 动作 | 影响场景 | 验证方式 | 风险 |
|---|---|---|---|---|
| E1 | 安全基座：握手认证 + AEAD + 防重放 | 15 | 篡改/重放/降级三类攻击用例必须全部被拒 | 自研握手易出错，优先 Noise/TLS1.3 复用 |
| E2 | 低延迟可靠恢复：RTT 采样 RTO + 快速重传 + SACK 扩宽 | S17、S03、S19、S07 | 与 KCP 同条件对比（20% 丢包、200ms RTT）的 p99 送达延迟 | SACK 位图从 32 位扩到 256 位需改帧格式（版本升级） |
| E3 | 实时流抗丢包：FEC + 带宽估计 + 关键帧请求 | S03、S12、S16 | 20% 丢包下端到端延迟与花屏恢复时间 | FEC 增加带宽开销，需与码率自适应联动 |

**E1 与 E2/E3 的次序**：E1 先做。理由：E2/E3 会改帧格式，而加密正好是一次版本升级的机会，两件事合并只改一次线格式；分开做要改两次，且中间态是"更强的传输 + 仍然明文"。

### P1（6 项）——打开第二类场景

| ID | 动作 | 影响场景 |
|---|---|---|
| E4 | 多对端扇出（连接表 + 每 peer 独立状态） | S07、S12、S20、S22 |
| E5 | 通道优先级 + QoS 描述符子集（可靠/持久/期限/存活/历史/优先级） | S04、S12、S02 |
| E6 | 组播 socket 能力（成员管理/TTL/回环） | S09、S11、S14、S21 |
| E7 | 持久化发送队列 + 断连续传 | S07、S19 |
| E8 | 事件游标补推 + 服务端到客户端背压 | S02、S04 |
| E9 | RTCP 反馈子集（NACK + PLI） | S16、S03 |

E4 是 P1 里的第一优先：它同时是共识（S07）、远程桌面（S12）、多方协作（S20）的前置。

### P2（9 项）——复用 `go2cpp::net`，Bento 不演进

E10 gRPC/HTTP2 工程化（HTTP/2 进库目标 + trailer + 长度前缀，S01/S06）、E11 文件传输能力（Range/chunked/校验，S08）、E12 协议适配器框架（RESP 打样，S15）、E13 CoAP + DTLS 模块（S13）、E14 Modbus TCP（S14）、E15 SOCKS5/CONNECT（S10）、E16 大消息流式分帧 API（S06/S08）、E17 服务端 TLS + 连接池 + mTLS（S22）、E18 隧道字节流抽象（S10）。

### P3（4 项）——明确不建议自研

自研 MQ/AMQP/Kafka broker（S05）、WireGuard（S10）、OPC UA（S14）、FIX 会话层与 V2X SCMS / DTN 存储转发（S18/S19/S21）。

---

## 6. 明确不做的事

写下这些是为了**防止后续 agent 把"广度调研"误读成"什么都做"**：

1. **不把 Bento 变成通用协议栈**。Bento 的价值是"低延迟 + 可控 QoS + 极简"，加 HTTP 语义、加 broker、加信息模型都是在稀释这个价值。
2. **不给组播加可靠有序**。1→N 的可靠组播需要 NACK 抑制，与 STREAM 的逐包 ACK 模型冲突，只能共存不能合并。
3. **不为 FIX/金融改重传语义**。重传必然产生重复，FIX 的 MsgSeqNum/PossDup 是会话层去重，两者必须分层，不能靠"传输层绝不重复"来保证。
4. **不手写密码学**。握手与 AEAD 优先复用 OpenSSL / Noise 参考实现；Curve25519、ChaCha20-Poly1305 自己实现是高风险低收益。
5. **不引入 Go 依赖**。quic-go 只能用来看设计（`docs/quic-go-reading-guide.md`），不能链接。
6. **不在 `Link` 内部加锁**。单线程不变量是 Bento 全部简单性的来源，多对端应做"连接表 + 每连接一个 Link"，而不是"一个 Link 多个 peer 加锁"。

---

## 7. 可复用的开源参考（只读研究，不引入依赖）

| 领域 | 参考 | 值得抄的局部 |
|---|---|---|
| 游戏可靠 UDP | KCP（`skywind3000/kcp`） | 快速重传判据、RTO 不翻倍、`ikcp_flush` 的拥塞窗口管理 |
| 游戏可靠 UDP | ENet（`lsalzman/enet`） | 多 channel 独立序号、ACK 位图与 RTT 平滑 |
| 现代 UDP 传输 | QUIC（`quiche`、`msquic`） | ACK 帧带 Delay/ECN、丢包检测、连接迁移、0-RTT |
| SCTP / 部分可靠 | `usrsctp` | PR-SCTP 的部分可靠语义（S20 的中间态参考） |
| 安全握手 | Noise Protocol Framework | IK/NK 模式的握手消息编排与 nonce 规则 |
| FEC | `libfec`、RaptorQ 论文 | 视频帧的 XOR/RS 冗余块选择 |
| 组播可靠 | NACK-Oriented Reliable Multicast（RFC 5405 实践） | 反馈抑制与随机退避 |
| 远程桌面 | FreeRDP | tile/脏矩形、动态虚拟通道、RDRL 的 UDP 传输扩展 |
| 流媒体 | `libsrtp`、pion/rtcp | RTCP NACK/PLI 的报文格式与发送节奏 |
| 工业 | `open62541` | UA Secure Conversation 的分块与签名编排 |
| 数据库 | `hiredis`、`libpq` | RESP 嵌套解析、SCRAM 状态机 |
| 金融 | QuickFIX | 会话层序号恢复与 ResendRequest/GapFill |

**引用纪律**：只借鉴协议语义与状态机，不复制 GPL/AGPL 代码；KCP 是 MIT、ENet 是 MIT、open62541 是 MPL-2.0、FreeRDP 是 Apache-2.0，商用前需逐个确认许可证。

---

## 8. 待确认 / 已知不确定

1. **Bento 是否要进主线库**。当前 `GO2CPP_WITH_BENTO` 默认 OFF（`cmake/Go2CppBento.cmake:15`），且 `tests/bento/PROTOCOL.md:26-42` 记录了 base 模块源码缺失只能 standalone 构建。P0 的 E2/E3 都要改帧格式，若定位未定，改格式的沉没成本很高。
2. **加密的合规边界**。若目标市场要求国密（SM2/SM3/SM4），AEAD 选型与 Noise 复用都要重估。
3. **S03 的"游戏状态"与 S17 的"游戏"是否同一套通道**。若同一套，E2/E3 的验收指标应合并设计（一个"实时通道 QoS 等级"概念），避免两套并行参数。
4. **多对端是否需要跨机器**。若只是单机多客户端，共享内存/Unix socket 可能比 UDP 扇出更合适，E4 的技术选型会变。
5. **QoS 打分的权重**。本文四维等权，若业务实际以"延迟 > 安全 > 带宽 > 可靠"排序，P0 内部次序会变（E2 应排在 E1 前）。

---

## 9. 一句话总结

**22 个场景验证了一个反直觉结论：Bento 的广度不在"支持更多协议"，而在"把实时性做到位"。**
4 个真正属于 Bento 的场景里，S03（实时流）与 S17（游戏）共享同一组缺口——FEC、带宽估计、低延迟可靠恢复、加密；S12（远程桌面）在此之上再加多对端与通道优先级；S04（混合 QoS）只差 QoS 描述符与通道优先级。**补 E1–E5 五项即可覆盖这 4 个场景**；其余 18 个场景的正确做法是复用 `go2cpp::net` 的 HTTP/1、HTTP/2、WebSocket、SSE、TLS，做薄适配层。

