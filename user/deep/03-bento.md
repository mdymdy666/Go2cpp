# Bento 传输（UDP 可靠层 / FEC / BBR / 0-RTT / PeerHub）· 深入学习

> 读法：先读第一部分建立全局图，再按需下钻第二部分。每个结论都给了 `文件:行`，
> 行号以 2026-11-03 的工作树（WSL `/UserData/CodexWorkSpace/Go2cpp`）为准；代码会漂移，
> 读到时先 `sed -n` 看一眼。
> **数字纪律**：本项目历史上所有「弱网对比」都出自 **2026-10-28 基线**
> （`tests/bento/compare_results.baseline-2026-10-28.tsv`）。之后加了 SACK 扩展、快速重传、
> BBR、FEC，但**没有用同一口径重跑**。因此本文引用旧数据时一律带日期限定；
> 没有当场复跑的数字一律标 **未实测**。
> 配套考试：`docs/exam/03-bento.md`（20 题，本文每节标了对应题号）。

## 为什么不用 TCP

一句话：**传输层队头阻塞 + 丢包即降速 + 连接与四元组绑定**。一条 TCP 连接上一个丢包会卡住后面所有字节（哪怕它们属于不同消息/不同流）；CUBIC 把丢包当拥塞信号做乘性减，随机无线丢包会让窗口无谓缩水；NAT 重绑定/换网后 TCP 连接死掉。Bento 在 UDP 上自建可靠层，用 `conn_id` 识别会话（`protocol.hpp:19-21`，不按源 IP:port），并按应用需要提供三种语义（`channel.hpp:14-24` 的通道约定）：STREAM（可靠有序）、DATAGRAM（最新值优先）、VIDEO（分片 + 可选 FEC）。

边界：它是 JinShan relay 场景的传输底座，**不是通用 RPC 框架，也不兼容 WebRTC**；`Link` 只支持单 peer，1:N 用 `PeerHub`。

## 代码地图

| 层 | 文件 | 规模 | 职责 |
|---|---|---|---|
| 线格式 | `include/go2cpp/bento/net/protocol.hpp` / `src/bento/net/protocol.cpp` | 364 / 566 行 | 16B 帧头、SACK 扩展、`BuildFrame/ParseFrame` |
| 加密 | `include/go2cpp/bento/net/crypto.hpp` / `src/bento/net/crypto.cpp` | 358 / 1462 行 | PSK 派生、ChaCha20-Poly1305、64 位重放窗口、票据 |
| 链路 | `include/go2cpp/bento/net/link.hpp` / `src/bento/net/link.cpp` | 814 / 3377 行 | 握手、收发分离、TX 队列、心跳/ACK/PTO tick |
| 通道 | `include/go2cpp/bento/net/channel.hpp` / `src/bento/net/channel.cpp` | 860 / 2284 行 | Reliable / Unreliable / Video 三套语义 + FEC |
| 拥塞 | `include/go2cpp/bento/net/congestion.hpp` / `src/bento/net/congestion.cpp` | 515 / 937 行 | BBR / NewReno / pacing / 带宽估计 |
| FEC 编解码 | `src/bento/net/fec.cpp` | 466 行 | GF(256) RS / Cauchy 矩阵 / XOR |
| 多 peer | `include/go2cpp/bento/net/peer_hub.hpp` / `src/bento/net/peer_hub.cpp` | 436 / 2211 行 | 一个 UDP socket + 按 `conn_id` 路由 |
| 弱网工具 | `tests/bento/{test_netem.sh,netem_probe.cpp,bench_weaknet.cpp,loss_proxy.hpp,udp_proxy.hpp}` | — | tc netem / 用户态丢包代理 / 弱网矩阵 |

---

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 入口点清单

**Link：单 peer 链路**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `Link::Open(error)` | `include/go2cpp/bento/net/link.hpp:369` | 用户 | 建 socket、装 PSK、校验安全策略（无 PSK 且 `require_encryption` → `kEncryptionRequired`） | bool |
| `Link::SetPreSharedKey(key,bytes)` | `link.hpp:373`（锁内版本 `:605`） | 用户 / `Open` | 派生握手密钥与会话密钥方向 | bool（长度非 32 为 false） |
| `Link::Connect()` | `link.hpp:401`，实现 `link.cpp:2064` 的 `SendHello` | 客户端 | 进 `kConnecting`、发 HELLO（`handshake_copies` 份，`:2091-2094`），并排下一次退避（`:2096-2097`） | bool |
| `Link::RunOnce(timeout_ms)` | `link.hpp:409`，实现 `link.cpp:828` | **单线程事件循环**（调用方驱动） | 锁外 poll → 收包 → 锁内 Tick → 锁外派发回调 → `DrainTx` | int（处理的帧数） |
| `Link::SendStream(channel,data,bytes,dep_seq)` | `link.hpp:415`，实现 `link.cpp:2729`（薄封装）→ `TrySendStream:2708` | 任意线程 | 锁内 `GetOrCreateReliable`（`:2716`→`:1689`/`:1707`）→ `TrySend`（`:2720`）；**锁外** `DrainTx`（`:2726`） | bool |
| `Link::SendDatagram(channel,data,bytes,dep_seq)` | `link.hpp:430`，实现 `link.cpp:2740` | 任意线程 | `GetOrCreateUnreliable`（`:1711`/`:1748`）→ 直接封帧 | bool |
| `Link::ConcurrentSend(...)` | `link.hpp:426` | 任意线程 | 与 `SendStream` 同路径但显式声明可并发 | bool |
| `Link::SendVideoFrame(frame_id,key,data,bytes)` | `link.hpp:434` | 任意线程 | 视频分片（`VideoChannel::SendFrame` `channel.cpp:1473`） | bool |
| `Link::ExportSessionTicket()` | `link.hpp:474`，实现 `link.cpp:2483` | 客户端（连接成功后） | 只在**纯 PSK** 会话导出（`link.cpp:2488`） | `vector<uint8_t>`（不可导出返回空） |
| `Link::Resume(ticket)` | `link.hpp:478`，实现 `link.cpp:2521` | 客户端/监听方 | 解票据、校验过期、初始化重放窗口 | bool |
| `Link::Close()` | `link.hpp:404`，实现 `link.cpp:798` | 任意 | 清 TX 队列（`:820`）、置 `m_tx_pending=false`、唤醒 drainer（`:825`）；锁顺序 `m_mutex → m_tx_mutex`（`:817` 注释） | void |

**PeerHub：1 个 socket × N 个 peer**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `PeerHub::Open(error)` | `include/go2cpp/bento/net/peer_hub.hpp:213`，实现 `src/bento/net/peer_hub.cpp:323` | 用户 | 合并 FEC 配置（`:328`）、校验 PSK（`:331-341`）、建 socket | bool |
| `PeerHub::AddPeer(peer_id,addr,port,...)` | `peer_hub.hpp:221` | 用户 | 登记外发 peer（白名单语义，`:98`） | bool |
| `PeerHub::Connect(peer_id)` | `peer_hub.hpp:230` | 用户 | 对该 peer 发 HELLO | bool |
| `PeerHub::RunOnce(timeout_ms)` | `peer_hub.hpp:245`，实现 `peer_hub.cpp:2154` | 单线程事件循环 | 收包 → 按 conn_id 找 peer → 分派 → Tick | int |
| `PeerHub::SendStream/TrySendStream/SendDatagram/SendBarrier` | `peer_hub.hpp:250/253/256/259` | 用户 | 与 Link 同名语义，多一个 `peer_id` | bool / `SendStatus` |
| `PeerHub::RemovePeer(peer_id)` | `peer_hub.hpp:233` | 用户（含回调内） | 延迟销毁到安全点（`peer_hub.hpp:42-44`） | bool |
| `PeerHub::HandlePacket(...)` | `peer_hub.cpp:1572` | `RunOnce` | 解外层头 → conn_id 路由 → `kFec` 分发（`:1557`） | void |

**通道（三种语义）**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `ReliableChannel::Send/TrySend` | `include/go2cpp/bento/net/channel.hpp:237`（类），实现 `src/bento/net/channel.cpp:358/375` | Link/PeerHub | 分片、流控、窗口、入 `m_unsent`、`Pump` | bool / `SendStatus` |
| `ReliableChannel::OnFrame` | `channel.cpp:528` | Link 收包 | 去重、重排、交付、记 ACK 待发 | void |
| `ReliableChannel::OnAck` | `channel.cpp:662` | Link 收到 ACK | 累积确认 + SACK 证据 + `m_cc.OnAck` + 快速重传 | void |
| `ReliableChannel::OnTimer` | `channel.cpp:927` | `Link::Tick`（`link.cpp:2144`，调用在 `:2179`） | PTO 重传、TLP 探测、FEC 清理、重组超时 | void |
| `UnreliableChannel::Send` | `channel.cpp:1207` | Link/PeerHub | 单调 seq、`dep_seq` 依赖、DropStale（`channel.hpp:20`、`:629`） | bool |
| `VideoChannel::SendFrame` | `channel.cpp:1473` | Link | 按 `kFragmentBytes=1200` 分片（`channel.hpp:745`） | bool |
| `VideoChannel::SendFecParity` | `channel.cpp:1530` | `SendFrame` 内部 | XOR 组校验片（`channel.cpp:1548-1555`） | bool |

**安全 / 弱网工具**

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回 |
|---|---|---|---|---|
| `AeadSeal / AeadOpen` | `include/go2cpp/bento/net/crypto.hpp:191/198`，实现 `crypto.cpp` | `SealFrame/OpenFrame` | ChaCha20-Poly1305 一次封装（12B nonce，16B tag，`crypto.hpp:67/70`） | void / bool |
| `SealFrame / OpenFrame` | `crypto.hpp:326/337`，实现 `crypto.cpp:1284/1313` | Link 发送/接收 | 外层明文头 + 内层整帧 AEAD；`OpenFrame` 走重放窗口 | bool |
| `SealHandshakeFrame` | `crypto.hpp:346`，实现 `crypto.cpp:1411` | 握手 | 握手帧单独定向密钥 | bool |
| `ReplayWindow::Accept/WouldAccept/InitializeBefore` | `crypto.hpp:262/261/253`，实现 `crypto.cpp:1074/1056/1042` | `OpenFrame` / 0-RTT | 64 位滑动窗口（`crypto.hpp:246`） | bool / void |
| `FecEncoder / FecDecoder`（`ValidShape`） | `src/bento/net/fec.cpp:55` | 通道 | `k∈[2,64]`、`m∈[1,16]`、XOR 只允许 `m=1`、`k+m<=255` | bool |
| `netem_probe` / `bench_weaknet` | `tests/bento/netem_probe.cpp`、`bench_weaknet.cpp` | 测试 | 弱网矩阵（丢包/延迟/FEC/BBR 对照） | 退出码/统计 |

**默认值（`include/go2cpp/bento/net/link.hpp` 的 `LinkConfig`）**

| 配置 | 默认 | 行号 | 说明 |
|---|---|---|---|
| `handshake_timeout_ms` | 5000 | `:91` | 握手基础超时；`Tick` 里再加一个 PTO 宽限（`link.cpp:2149-2152`） |
| `heartbeat_interval_ms` | 500 | `:93` | 心跳（纯 ACK 帧） |
| `ack_interval_ms` | 20 | `:94` | 独立 ACK 最小间隔（`link.cpp:2167-2174`） |
| `hello_initial_pto_ms` / `hello_max_pto_ms` | 200 / 2000 | `:95-96` | 握手退避区间 |
| `hello_max_attempts` | 16 | `:99` | 握手轮数上限 |
| `handshake_copies` | 2 | `:103` | 每轮冗余份数（1 = 关闭） |
| `enable_sack_ext` | true | `:110` | 双方 caps 都置位才启用；加密会话恒降级（见 2.8） |
| `require_encryption` | **true** | `:121` | 无 32B PSK 时 `Open/Connect` 失败 |
| `allow_legacy_peer` | false | `:127` | 允许无 X25519 公钥的旧对端回退纯 PSK |
| `enable_x25519` | true | `:136` | 前向保密；关掉 = 纯 PSK |
| `handshake_replay_window_ms` | 30000 | `:138` | 握手时间戳容忍窗口；0 = 不校验 |
| `video_fec_group_size` | 0（关） | `:146` | 2..8 才启用 |
| `enable_pmtud` / `pmtud_max_mtu` | false / 1500 | `:149`/`:151` | 默认关，用保守 1200 起步 |
| `session_ticket_lifetime_ms` | 60000 | `:159` | 0 = 禁用票据导出 |
| `migration_grace_ms` | 3000 | `:161` | 对端迁移后旧地址静默期 |

编译开关：`GO2CPP_WITH_BENTO`（默认 OFF，`cmake/Go2CppBento.cmake:17`）——没有它，整个 `bento` 不存在。

### 1.2 总架构图

```
┌────────────────────────────────────────────────────────────────────────────┐
│ 用户：link.RunOnce(20)（单线程驱动） + 任意线程 SendStream/SendDatagram      │
└───────────────┬───────────────────────────────────┬────────────────────────┘
                │ 收：poll/recvmmsg                  │ 发：Send*
┌───────────────▼───────────────┐   ┌───────────────▼────────────────────────┐
│ UdpSocket（SO_REUSEPORT 多队列）│   │ ReliableChannel（每 channel 一个）      │
│  RxQueue[0..15] + rx_threads   │   │  m_unsent / m_inflight / m_sacked       │
│  RecvBatch = recvmmsg(32)      │   │  cwnd + pacer + MSG 分片 + 流控 MAX_DATA │
└───────────────┬───────────────┘   └───────────────┬────────────────────────┘
                │ HandleFrame(1444)                  │ BuildFrame + SendNow(489)
┌───────────────▼───────────────────────────────────▼────────────────────────┐
│ Link（单 peer）                                                             │
│  m_mutex（**非递归**，link.hpp:811）：通道表 / 链路状态 / sendto            │
│  m_tx_mutex（link.hpp:793）：**只保护 TX 队列**，DrainTx 在锁外 seal+sendto  │
│  m_events（lock-free 入队，锁外派发回调）                                    │
└───────────────┬────────────────────────────────────────────────────────────┘
                │ SealFrame / OpenFrame（AEAD + ReplayWindow 64）
┌───────────────▼────────────────────────────────────────────────────────────┐
│ 内核：epoll/poll、recvmmsg、sendto、eventfd（rx 线程唤醒）                   │
└────────────────────────────────────────────────────────────────────────────┘

一次发送的跨层数据流：
  SendStream(2729) → ReliableChannel::Send(358) → TrySend(375) 分片/流控/窗口
    → m_unsent.push_back(437) → Pump(450) → SendNow(489) BuildFrame
    → m_sink → Link::SendBuiltFrame(1732) → EnqueueTxFrame(1736) 入 m_tx_queue(1786)
    → m_tx_pending=true(1790)
  RunOnce 末尾（或另一个 Send 线程）→ DrainTx(1800) 单 drainer 取整批(1826)
    → 锁外 SendPacketLocked(1840) → SealAndSend → SealFrame(1284, AEAD) → sendto
```

**四个边界要记牢**：

1. **单 peer vs 多 peer 边界**：`Link` 是**单 peer**（监听方只接受第一个握手成功的来源，`user/docs/03-bento.md:338`）；1:N 必须用 `PeerHub`（`peer_hub.cpp:1572` 按 `conn_id` 路由）。
2. **锁边界**：`Link` 的 `m_mutex` **非递归**、回调在锁外派发（`link.hpp:809-811`）；`PeerHub` 的 `m_mutex` **仍是 recursive**（`peer_hub.hpp:432`）——两条实现路径的安全模型不同，别把 PeerHub 的经验套到 Link。
3. **线程边界**：`rx_threads > 0` 只把 poll+recvmmsg 摊到多核；**解帧/ACK/重组/回调仍在 `RunOnce` 调用线程**（`user/docs/03-bento.md:75`）。
4. **定时器边界**：所有协议的定时器（ACK/心跳/PTO/TLP）都靠调用方传的 `RunOnce(timeout)` 驱动，`ClampPollTimeout`（`link.cpp:847` 附近）把 poll 超时钳到最近定时器——**应用传 50ms，协议精度就变成 50ms**（`link.cpp:838-845` 的注释）。

### 1.3 复杂场景 ≥10 步详解

#### 场景 A：一次可靠发送（入队 → 封包 → FEC → 发送 → ACK → 重传判定 → 窗口更新，16 步）

1. **入口**：`Link::SendStream`（`link.cpp:2729`）→ `TrySendStream`（`:2708`）：持 `m_mutex` 检查 `kConnected`（`:2713-2715`）→ `GetOrCreateReliable(channel)`（`:2716`→`:1689`，不存在就建 `:1707`）→ `target->TrySend`（`:2720`）；返回后**锁外** `DrainTx`（`:2726`）——所以 `SendStream` 的调用线程自己也可能成为 drainer。
2. **参数校验**：`ReliableChannel::Send`（`channel.cpp:358`）→ `TrySend`（`:375`）；`bytes > kMaxStreamMessageBytes` 或空指针 → `kError`（`:376-380`）。
3. **分片判定**：`bytes > kMaxFramePayload`（1300，`protocol.hpp:105`）→ 按 1292B 分片，8B 子头使每帧载荷仍 ≤1300（`channel.cpp:382-387`）。
4. **流控闸**：`m_tx_bytes_total + wire_payload > m_peer_max_data` → `kWouldBlock`（`:396-402`）；**超窗是暂时状态，不能静默丢**，调用方在收到 `MAX_DATA` 后重试同一条消息。
5. **窗口闸**：`unsent + inflight + frag_count > kSendWindow` → `kWouldBlock`（`:405-409`）——一条大消息的所有分片必须一次进窗，否则破坏消息原子性。
6. **构造 `Outgoing`**：`seq = m_next_tx_seq++`（0 保留，`:411-415`）；`frame_id = m_next_frame_id++`（`:411`）；分片消息写 8B 子头（`:425-435`）。
7. **入未发队列**：`m_unsent.push_back`（`:437`）；`m_tx_bytes_total += wire_payload`（`:439`）；`Pump(Now())`（`:441`）。
8. **Pump**（`:450`）：受 `cwnd`、pacing 令牌、窗口三重约束把 `m_unsent` 搬进 `m_inflight` 并 `SendNow`。
9. **封帧**：`SendNow`（`:489`）→ `FillAck(&ack)`（`:1081`，拿当前接收状态做捎带 ACK）→ flags 叠加 `kFlagAck`/`kFlagRetransmit`/`kFlagFragmented`（`:497-500`）→ `BuildFrame`（`protocol.cpp:297`，16B 头 + 8B ACK + 载荷）→ `m_sink(...)`（`channel.cpp:514`）。
10. **进 TX 队列**：`m_sink` 是 Link 注入的 lambda → `Link::SendBuiltFrame`（`link.cpp:1732`）→ `EnqueueTxFrame`（`:1736`）：查 socket/peer（`:1737-1746`），统一打 `conn_id` 与 `kFlagResume`（复用 `m_tx_stamp_scratch`，`:1751-1770`）。
11. **背压**：`m_tx_queue.size() >= kMaxTxQueue` → 丢弃并 `++m_stats.send_errors`（`:1781-1784`）——**UDP 语义，不无限堆**；否则 `push_back`（`:1786`）并 `m_tx_pending=true`（`:1790`）。
12. **单 drainer 发送**：`DrainTx`（`:1872`）保证只有一个线程在做 seal+sendto（`:1820-1826`），一次取空队列（`:1826-1832`），**锁外** `SendPacketLocked`（`:1840`→`:1794-1799`），完成后更新 `m_tx_drained`、唤醒等待者（`:1850-1858`）。
13. **AEAD**：加密会话走 `SealAndSend` → `SealFrame`（`crypto.cpp:1284`）：外层头明文（`conn_id`/`seq` 用于路由与重放判定，且作为 AAD，`:1299-1308`），内层整帧密文，nonce 由 `direction+seq` 派生（`:1306-1307`）。
14. **对端收**：`ReceiveOnce`（`link.cpp:1129`）→ `RecvBatch`（`link.cpp:555`，`recvmmsg` `:574`）→ `HandleFrame`（`:1444`）：解外层头（`:1449`）→ 按 `conn_id` 路由（`:1494`）→ `OpenFrame`（`crypto.cpp:1313`，重放窗口 `m_data_rx_window` `link.hpp:783`）→ `DispatchFrame`（`:1570`）。
15. **回 ACK**：接收端 `FillAck`（`channel.cpp:1081`）在下一个数据帧捎带，或 `Link::Tick` 到 `ack_interval_ms` 时 `SendAckFrame`（`link.cpp:2167-2174`，实现 `:2100`）。
16. **ACK 处理**：发送端 `OnAck`（`channel.cpp:662`）→ 累积确认出队（`:692-698`）→ SACK 位图/扩展块（`:700-745`）→ `Acknowledge` 采样 RTT（`:901`）→ `m_cc.OnAck`（`:763`）→ `Pump` 补发（`:764`）→ `TryFastRetransmit`（`:765`→`:784`）。

失败/阻塞分支：
- 第 4/5 步 `kWouldBlock`：**不丢消息**，调用方退避重试；流控上限由对端 `MAX_DATA` 推进（`channel.cpp:1026` 的 `OnMaxData`）；
- 第 11 步队列满：丢帧 + 计数（可观测，但消息已丢，可靠层的 PTO 会补）；
- 第 13 步 seal 失败：`SendPacketLocked` 返回 false 只影响该批的统计（`link.cpp:1841-1845`），不阻断后续帧；
- 第 14 步打开失败：分 `kMalformed/kNotEncrypted/kReplay/kAuthFailed`（`crypto.cpp:1316-1340`），计数器不同（明文注入有独立指标，`link.cpp:1490`）。

#### 场景 B：弱网从丢包到重传成功（14 步，含退避与 TLP）

1. **丢包发生**：某个 STREAM 帧在链路上丢失；接收端只收到后面的帧（乱序到达）。
2. **接收端记空洞**：`ReliableChannel::OnFrame`（`channel.cpp:528`）记录已收 seq，更新待发 ACK 状态（`FillAck` 时把位图/扩展块编码进 `AckState`）。
3. **ACK 回传**：`SendNow` 捎带 8B ACK（数据帧**永远只带 32 位位图**，`protocol.hpp:189-197` 的 `AckState` 注释说明扩展块只走纯 ACK 帧），或 `SendAckFrame` 发纯 ACK（可带 `kMaxSackBlocks=16` 块，`protocol.hpp:80`）。
4. **发送端看到重复 ACK**：`OnAck`（`channel.cpp:662`）里 `progressed=false`（`ack_seq` 未前进，`:667`）。
5. **快速重传门限**：`TryFastRetransmit`（`:784`）累计 `m_dup_acks`，达到 `fast_retransmit_threshold=3`（`channel.hpp:329`）就重发队首。
6. **快速恢复期继续推进**：`m_in_fast_recovery` 期间每满 threshold 个重复 ACK 重发一次队首（`:790-800`）——注释记录了旧实现「整段恢复只重传一次」的后果：那次重传再丢（20% 丢包）就要等完整 PTO 退避，p50 被推到 4.5–7s。
7. **重传仍丢**：没有任何 ACK 前进 → 时间推进到 `Link::Tick`（`link.cpp:2144`）→ 对每个通道 `OnTimer`（`:2179`）。
8. **PTO 判定**：`OnTimer`（`channel.cpp:927`）对 `m_inflight` 逐条算 `m_rtt.BackoffPtoMs(it->retransmits)`（`:940`），到期才重传，且**每个 tick 最多 `kPtoRetransmitBatch` 条**（`:932-935`）。
9. **跳过已被 SACK 的**：`m_sacked.count(seq)` 命中 → 直接跳过（`:936-939`）——扩展 SACK 让这里的候选集显著变小。
10. **退避**：`++it->retransmits`（`:945`）→ 下次 PTO 翻倍（上限由 `CongestionConfig::max_rto_ms=2000` 约束，`congestion.hpp:82`）。
11. **降窗一次**：本次有重传且 `!m_pto_recovery`（`:977`）→ `m_cc.OnTimeout(loss_context)`（`:983-991`）并置 `m_pto_recovery=true`（`:978`）——一次丢包事件只降一次窗，ACK 前进后清除（`:669-674`）。
12. **TLP 兜底**：若没有新数据可发、但仍有未确认（`m_unsent.empty() && !m_inflight.empty()`），在 PTO 的一半时刻补发一个探测包（`:953-975`）；探测**不递增** `retransmits`、不参与退避，否则一次尾包丢失会把后续恢复间隔整体翻倍。
13. **恢复**：ACK 前进 → `m_highest_ack` 更新、`m_dup_acks=0`、`m_sack_evidence=0`、`m_in_fast_recovery=false`（`:669-674`）→ `m_cc.OnAck` → `Pump` 发新数据。
14. **FEC 旁路**：若开了 FEC，组内校验片可能在第 7 步之前就恢复了丢帧（`TryRecoverFec` `channel.cpp:2139`、`OnFecFrame` `:2075`），重传到不了第 8 步。

失败/阻塞分支：
- 第 8 步 `SendNow` 失败 → **仍推进时间戳**，避免同一 tick 反复重试（`:948-951`）；
- 第 11 步 `OnTimeout` 在 `kStartup` 相位**不降窗**（`congestion.cpp:629-660` 的 `BbrOnLoss`：startup 期丢包不是拥塞信号）；
- 第 12 步探测也只发一帧，丢了就等下一个 PTO；
- **已知限制**：BBR 是 BBRv1 最小子集，没有 ack aggregation、没有包级精确 round 判定（`user/docs/03-bento.md:345`）。

#### 场景 C：0-RTT 建连与重放窗口（12 步）

1. **前提**：`require_encryption=true`（`link.hpp:121`）+ 32B PSK（`:131`）+ `enable_x25519=true`（`:136`）。握手成功后会话密钥派生自 PSK 与 X25519 共享密钥（`crypto.cpp` 的 `DeriveSessionKey` `crypto.hpp:215`）。
2. **导出票据**：`ExportSessionTicket`（`link.cpp:2483`）；条件里包含 `IsZeroX25519Key(m_x25519_public)`（`:2488`）——**X25519 会话直接拒绝导出**（不静默降级前向保密），这是 Q04/Q15 的核心。
3. **票据内容**：`conn_id`、`session_id`、`issued_ms`/`expires_ms`、双方 nonce、双方 `next_wire_seq`（`:2490-2510`）。
4. **票据加密**：`EncodeSessionTicket(ticket, m_psk, &bytes)`（`:2513`）——票据本身就是 AEAD 密文，拿到明文票据 = 拿到 PSK 派生密钥。
5. **有效期**：`expires_ms = now + session_ticket_lifetime_ms`（`:2496`，默认 60000，`link.hpp:159`）。
6. **恢复**：`Resume(ticket)`（`:2521`）→ 解票据（`:2530-2535`）→ 过期即拒（`:2536-2539`）→ socket/peer 状态检查（`:2540-2542`）。
7. **重放窗口初始化**：用票据里的 `next_wire_seq` 调 `ReplayWindow::InitializeBefore`（`crypto.hpp:253`，实现 `crypto.cpp:1042`）——旧会话已确认过的序号必须视为**已消费**，否则重放的旧包会被当新包接受。
8. **首帧标记**：发送端在恢复后的首批帧置 `kFlagResume`（`protocol.hpp:163`）；接收端见到该标志先清空旧应用的通道状态再处理本帧（`protocol.hpp:161-163` 注释）。
9. **监听端采用**：`HandleFrame`（`link.cpp:1444`）在 `m_state != kConnected` 或 `conn_id` 不匹配时进入 0-RTT 分支（`:1464-1467`），`TryAdoptCachedSession` 按 conn_id 查进程内会话缓存并试解首包；**解不开绝不改现有状态**（`:1467-1481`）。
10. **采用成功**：`NoteResumeEpoch(view.header.flags)`（`:1474-1476`）→ `DispatchFrame(view)`（`:1478`），数据直接交付上层。
11. **确认**：监听端回 `kFlagResumeAck`（`protocol.hpp:166`），客户端见到后停止在后续帧置 `kFlagResume`（`protocol.hpp:164-166` 注释：避免「服务端旧心跳先到」导致客户端过早清标志）。
12. **重放防线**（三层）：① 握手时间戳窗口 `handshake_replay_window_ms=30000`（`link.hpp:138`）+ `m_handshake_rx_window`（`link.hpp:784`）；② 数据帧 64 位滑动窗口 `ReplayWindow`（`crypto.hpp:244-246`）；③ 票据 60s 过期。**窗口关了会怎样**：重放的非幂等操作会被重复执行（下单/扣款），协议层无法区分「重放」与「真的又发了一次」（Q04）。

失败/阻塞分支：
- 第 2 步非纯 PSK 会话 → 返回空票据，调用方只能走完整握手（1-RTT）；
- 第 6 步票据过期/被篡改 → `tickets_rejected++`（`:2526`、`:2534`）返回 false；
- 第 9 步缓存未命中 → 走正常握手路径，不会因为「看到 conn_id」就接受；
- **已知未验证**：0-RTT 的重放攻击面只有代码防线，仓库没有攻防实测（`docs/exam/03-bento.md:306-307`）。

### 1.4 关键不变量（破了就一定出 bug）

| # | 不变量 | 依据 | 破了会怎样 |
|---|---|---|---|
| I1 | `m_tx_mutex` 只保护队列，seal/sendto 必须在锁外 | `link.hpp:793`、`link.cpp:1826-1845` | 回到「发送线程被整段 poll 挡住」的旧结构；p99 回到 timeout 量级 |
| I2 | 同一时刻只允许一个 drainer | `link.cpp:1820-1826` | 多线程抢锁做 seal+sendto → 退化成串行，且 `m_tx_drained` 语义乱 |
| I3 | 回调只在锁外派发，回调里可 `Send*`/`Close` | `link.hpp:803-804`、`link.cpp:877` | 持锁回调 → 用户 `Send*` 重入 → 死锁（旧 `recursive_mutex` 就是为此存在） |
| I4 | 可靠窗口/流控不足必须返回 `kWouldBlock`，不许静默丢 | `channel.cpp:396-409` | 消息丢失且重传层不知道；上层以为发出去了 |
| I5 | `seq==0` 保留为无效，`frame_id==0` 保留 | `channel.cpp:411-415` | 0 号 seq 被当有效包 → 去重/ACK 编号错乱 |
| I6 | SACK 证据只减重传集，**不增 cwnd** | `channel.cpp:700-720` 注释 | 一次 SACK 风暴在丢包减窗前虚增 cwnd → 越丢越发 |
| I7 | 一次 PTO 事件只降窗一次（`m_pto_recovery`） | `channel.cpp:977-991` | 每个 tick 都降窗 → 窗口塌到 min_cwnd，恢复要几十个 RTT |
| I8 | 0-RTT 票据只对纯 PSK 会话导出 | `link.cpp:2488` | X25519 会话的前向保密被静默降级成长期 PSK |
| I9 | 重放窗口必须按票据里的 `next_wire_seq` 初始化 | `crypto.hpp:250-253`、`crypto.cpp:1042` | 旧会话的已确认包被当新包重放 |
| I10 | 默认 `require_encryption=true`，无 PSK 直接失败 | `link.hpp:121`、`peer_hub.cpp:331-341` | 明文可被注入/降级；最危险的是**忽略返回值仍然能跑**但所有发送被拒 |
| I11 | 帧头 16B 与 `kFlagEncrypted` 的明文外层可被路由读取 | `crypto.cpp:1294-1308` | 把 conn_id 也加密 → 接收端无法在解密前按 conn_id 路由（0-RTT 与迁移都会坏） |

---

## 第二部分 · 分：逐块细讲

### 2.1 protocol：16 字节头与 SACK 扩展

**职责**：定义线格式与编解码；所有字段大端。

**关键结构**

| 结构 | 行号 | 字段 |
|---|---|---|
| `FrameHeader` | `protocol.hpp:169` | `version(u8)/type(u8)/channel(u8)/flags(u8)/conn_id(u32)/seq(u32)/dep_seq(u32)` = 16B（`kFrameHeaderBytes` `:70`） |
| `SackBlock` | `:180` | `start_offset(u16)` 相对 ack_seq（>=1）、`length(u16)` |
| `AckState` | `:189` | `ack_seq(u32)`、`ack_bits(u32)`、`sack_block_count`、`sack_blocks[16]` |
| `FrameView` | `:199` | 解析视图，`payload` 借用调用方缓冲（零拷贝） |
| `FrameType` | `:123-143` | HELLO/HELLO_ACK/STREAM/DATAGRAM/VIDEO/ACK/BARRIER/MAX_DATA/PMTUD_PROBE/PMTUD_ACK/FEC |

**为什么是 16B 不是 12B**（Q05）：v0.2 加了 4B `conn_id`（`protocol.hpp:19-21` 注释）——接收端按它路由而不是源 IP:port，NAT 重绑定后会话可续。`dep_seq` 是「依赖的不可靠 seq」，0 表示无依赖，用于因果屏障（`kBarrier`，`:130`）。

**关键函数**

- `EncodeHeader`（`protocol.cpp:147`）/`DecodeHeader`（`:157`）：16B 定长、逐字段写大端。
- `EncodeAckPayload`（`:213`）/`DecodeAckPayload`（`:252`）：前 8B 恒定位图；扩展块只在「纯 ACK 帧」的尾部（`protocol.hpp:85-90` 的字节数常量）。
- `BuildFrame`（`:297`）：`EncodeHeader`（`:340`）+ 可选 ACK + 载荷。
- `ParseFrame`（`:354`）：`DecodeHeader`（`:359`）+ 按类型解 ACK（`:407`）。

**失败路径与限制**：`ver != kProtocolVersion(2)` 的帧在解析阶段被拒（`protocol.hpp:66-67`）；载荷 > `kMaxFramePayload=1300` 走分片而不是 IP 分片（`:103-105`），PMTUD 探针例外（`kMaxProbeFrameBytes=1500` `:112`）。

**PMTUD（Q17）**：默认关闭（`link.hpp:149`），探测帧类型 `kPmtudProbe/kPmtudAck`（`protocol.hpp:135-139`），探针上限 `kMaxProbeFrameBytes=1500`（`:112`），递增探针 1200→1500（`link.hpp:147-148` 注释）。关闭时用保守 1200（IPv6 最小 MTU 1280 减去隧道开销的安全值），最大 1500。代价是无法利用更大的 MTU，好处是不会因 MTU 黑洞丢包；跨 IPv4/IPv6 不混用 MTU。仓里有 `tests/bento/test_pmtud.cpp`，**本文未跑**。

**典型误用**：把 tunnel 的 12B `TUNL` 帧当成 Bento 头（Q05 错答）；以为 `conn_id` 就是 UDP 源端口。

### 2.2 crypto：PSK + ChaCha20-Poly1305 + 重放窗口

**职责**：认证加密、握手、重放防护、0-RTT 票据。

**关键常量**（`crypto.hpp`）

| 常量 | 行号 | 值 |
|---|---|---|
| `kChaCha20KeyBytes` | `:64` | 32 |
| `kPskBytes` | `:73` | 32 |
| `kChaCha20NonceBytes` | `:67` | 12 |
| `kAeadTagBytes` | `:70` | 16 |
| `kSecureOverheadBytes` | `:79` | 16B 头 + 16B tag = 32 |
| `ReplayWindow::kWindow` | `:246` | 64 |

**`SealFrame` 逐步拆**（`crypto.cpp:1284`）

1. 参数校验：`seq==0`、`bytes==0`、`bytes>kMaxProbeFrameBytes` 全部拒（`:1286-1289`）。
2. 解内层头，取 `type/channel/flags/conn_id`（`:1290-1293`）。
3. 组外层头：`flags |= kFlagEncrypted`（`:1298`）、`conn_id` 明文并**参与 AAD**（`:1299-1300`）、`seq` 用外层 wire seq（`:1301`）、`dep_seq=0`（`:1302`）。
4. 分配 `16 + bytes + 16`（`:1304`），写外层头（`:1305`）。
5. `MakeSecureNonce(direction, seq, nonce)`（`:1306-1307`）——方向 + seq 决定 nonce，**同 seq 同方向不可复用**。
6. `AeadSeal(key, nonce, aad=外层头, plaintext=内层帧, ...)`（`:1308-1309`）。

**`OpenFrame`**（`:1313`）：失败先清空调用方缓冲（`:1319-1322`，防上一次明文残留）；`kFlagEncrypted` 未置 → `kNotEncrypted`（`:1333-1338`）；长度不足 → `kMalformed`；重放窗口检查 → `kReplay`。

**`ReplayWindow`**（`crypto.hpp:244-262`）：`kWindow=64`，`highest` 是收到过的最大 wire seq，bitmap bit i 表示 `highest-i` 已收到；`InitializeBefore(next_seq)`（`:253`）只用于 0-RTT（`crypto.cpp:1042`）。

**自研 AEAD 的真实状态（重要，别抄旧结论）**：`asm-tls.md` 里「1MB 比 OpenSSL 慢 13.41×、全文件 0 条 SIMD 指令」的口径**已经过时**——`crypto.cpp:367` 起有 AVX2（8 路，`:573`）与 AVX-512（16 路，`:650`）后端，`ChaCha20Xor`（`:806`）会按 CPU 能力分派（`:820-823`）；标量路径也有 4 路并行（`:137-141`，注释记录 1MiB 吞吐 709→830 MB/s）。**现状是否追上 OpenSSL：未实测**，本文不给结论。

**典型误用**：以为 PSK 有前向保密（Q15 错答）；以为「加个 AVX 就能补上 13.41×」（先看 `:367` 已经加了，再看是否还差）。

### 2.3 Link：收发分离与单 drainer

**职责**：单 peer 的握手、收包、发送队列、定时器；回调安全。

**关键字段**（`link.hpp:790-811`）

| 字段 | 行号 | 说明 |
|---|---|---|
| `m_tx_mutex` | `:793` | **只保护队列本身**，不保护链路状态（`:791-792` 注释） |
| `m_tx_queue` | `:795` | `deque<TxPacket>` |
| `m_tx_enqueued` / `m_tx_drained` | `:796` / `:797` | 入队/已发序号，供等待者判断 |
| `m_tx_draining` | `:798` | 「已有 drainer」标记 |
| `m_tx_pending` | `:800` | `atomic<bool>`，免锁空转的快速判断 |
| `m_events` / `m_events_pending` | `:803` / `:804` | 锁内入队、锁外派发（`:802` 注释） |
| `m_mutex` | `:811` | 保护通道表、socket 发送、统计、流控；**非递归**（`:809-810` 注释） |

**`RunOnce` 逐步拆**（`link.cpp:828`）

1. socket 未开 → `DispatchEvents()` 后返回 0（`:832-835`）。
2. **锁内只做两件事**：`PollPathMtuErrors()`、`ClampPollTimeout(timeout, now)`（`:838-850`）——把 poll 超时钳到最近定时器（`:847`），否则 `RunOnce(50)` 会把 ACK/PTO/心跳精度拖到 50ms。
3. **锁外** `ReceiveOnce(wait_ms, &rx_drained)`（`:857`）：poll + `recvmmsg` + 解帧（`:1129`）。
4. 错误只 `++m_stats.poll_errors`（`:861-864`）。
5. 锁内 `FlushImmediateAcks`（`:868`）+ `Tick`（`:874`，实现 `:2144`）。
6. 锁外 `DispatchEvents()`（`:877`）——回调从这里跳出去。
7. 锁外 `DrainTx()`（`:880`）。

**`DrainTx` 逐步拆**（`:1872`）

1. 取 `m_tx_mutex`；队列空 → 若有 drainer 在跑就等它（`:1809-1815`），否则 `m_tx_pending=false` 返回 true（`:1816-1818`）。
2. 已有 drainer → 等这一批结束再看队列（`:1820-1826`）——**N 个发送线程不会各自抢锁做 seal+sendto**。
3. 置 `m_tx_draining=true`、记 `upto=m_tx_enqueued`（`:1824-1826`）、整批搬到局部 `batch`（`:1826-1832`）、`m_tx_pending=false`（`:1832`）。
4. **锁外**持 `m_mutex` 逐帧 `SendPacketLocked`（`:1840-1847`）。
5. 回收 `m_tx_draining=false`、`m_tx_drained=upto`、记 `m_tx_last_ok`、看是否还有（`:1850-1860`）、`notify_all`（`:1861`）。

**为什么不是「换个锁」**（Q06）：旧结构是 `RunOnce` 第一行 `lock_guard<recursive_mutex>` 罩住 `poll + 全部 HandleFrame + Tick`，发送入口抢同一把锁 → 发送线程最多被阻塞一个 `timeout_ms`。p99 1405ms → 0.045ms 是**提交 `5a2c0fa` 的口径**，本文**未复跑**；但结构原因可以直接从 `:828-882` 读出来。

**失败路径与限制**
- 队列满丢帧（`:1781-1784`）；
- `OpenRxQueues`（`:885`）在 `SO_REUSEPORT` 不可用时回退到已打开的数量（`:915-928`），`rx_threads` 同步下调（`:920-923`）；
- `rx_threads>0` 只并行收包 syscall，协议处理仍在 `RunOnce`（Q19）。

**谁在 drain / 锁顺序**：两条路径都会触发 `DrainTx`——`RunOnce` 末尾（`:879-880`）与 `TrySendStream` 锁外（`:2726`）。`m_tx_draining` 保证同一时刻只有一个 drainer；锁顺序固定为 `m_mutex → m_tx_mutex`（`:817` 注释），`Close` 的清理段也遵守同一顺序（`:818-825`）。

- **多队列的天花板（Q07/Q19）**：`SO_REUSEPORT`（`link.cpp:316`）让每个队列一个 fd、内核按四元组哈希分流，配合 `recvmmsg` 一次收多包（`:574`）；`OpenRxQueues`（`:885`）建队列，不支持时回退到已打开数量（`:915-928`）。但**单 peer 时协议处理仍在 `RunOnce` 单线程**：解帧/ACK/重组/回调都串行，收包 syscall 根本不是瓶颈 → 收益为零。收益场景是 PeerHub 多 peer。

**典型误用**：在回调里 sleep（阻塞整条链路的收包与定时器，Q12）；以为 `rx_threads` 能线性扩展（Q19）。

### 2.4 ReliableChannel：窗口、分片、流控

**职责**：可靠有序消息；分片重组、流控、拥塞控制、重传。

**关键字段/配置**

| 项 | 行号 | 说明 |
|---|---|---|
| `Config::fast_retransmit_threshold` | `channel.hpp:329` | 3 个重复 ACK 触发快速重传 |
| `Config::initial_max_data` | `:336` | 1 MiB 接收窗口 |
| `Config::max_data_refresh_ms` | `:340` | 200ms 重发 MAX_DATA |
| `Config::fec` / `fec_delivered_history` / `fec_max_groups` | `:346` / `:349` / `:351` | FEC 挂在可靠通道上（Q08） |
| `m_fec_send_group` / `m_fec_rx_groups` | `:609` / `:612` | 发送组 / 接收组（key = base_seq） |
| `VideoChannel::kFragmentBytes` / `kMaxReassembly` / `kMaxFecGroupSize` | `:745` / `:749` / `:753` | 1200B 分片、4 个并发重组、FEC 组 ≤8 |

**状态机**（窗口/消息）

```
Send:  m_unsent ──Pump(cwnd/pacing)──▶ m_inflight ──ACK/SACK──▶ 已确认（出队，释放窗口）
                    ▲                      │
                    └──── PTO/快速重传 ─────┘（重传不重新入队，留在 inflight）
收:    out-of-order ──▶ m_sacked 位图 ──▶ 空洞补齐 ──▶ 交付上层
```

**关键失败路径**
- 流控/窗口不足 → `kWouldBlock`（`:396-409`）；
- 重组超时 → `StreamReassembler::OnTimer`（`channel.cpp:993` 调用）；
- **半修状态（Q18，题面与源码已漂移）**：STREAM 分片重组在首片到达时仍一次性 `assembly.data.assign(frag_count * kStreamFragmentBytes, 0)`（`channel.cpp:202`）+ `assembly.received.assign(frag_count, 0)`（`:203`），即使只到 1 片。**但 VIDEO 通道已经改掉**：`channel.hpp:817-822` 的注释写明「`data` 按需增长：只在分片真正写到的偏移处扩容，不再于首片到达时一次性分配并清零」，`received` 也改成内联数组。
  所以答题时要说清「哪条路径还浪费」——`docs/exam/03-bento.md` Q18 把 `channel.cpp:203` 归给视频通道，实际那行属于 `StreamReassembler::AddFragment`（`channel.cpp:176`）。

**典型误用**：以为 `Send` 返回 true 就代表对端收到（只代表入队）；把 `kWouldBlock` 当错误丢弃。

### 2.5 UnreliableChannel：DropStale 与 dep_seq

**职责**：DATAGRAM 语义——不重传、只保留最新值、可选的有限乱序缓存。

**关键配置**（`channel.hpp:628-639`）

| 字段 | 行号 | 默认 | 说明 |
|---|---|---|---|
| `reorder_window` | `:634` | 256 帧 | 0 = 关闭缓存，退回纯 DropStale；按「在途帧数」而不是 seq 深度设限（`:630-633` 说明：200ms±50ms 抖动会让几乎全部帧同时乱序） |
| `reorder_timeout_ms` | `:638` | 200ms | 缺口等待上限；超时跳过缺口、交付最小 seq |

**`Accept` 逐步拆**（`channel.cpp:1269`）

1. `!SeqGreater(seq, m_applied_seq)` → `stale_dropped++` 直接丢（`:1271-1274`）——这就是 DropStale。
2. `from_pending`（被 `Relation` 因果唤醒）：不再进乱序缓存，直接交付并作废更旧项（`:1275-1287`），避免二次延迟。
3. 恰好是下一个期望 seq → 立即交付 + `AdvanceBuffered`（`:1289-1294`）。
4. `reorder_window==0` → 跳过缺口直接交付（`:1296-1300`）。
5. 其余入 `m_reorder`，等缺口补齐或 `OnTimer`（`:1403`）超时。

**`dep_seq` 保的是什么序**（Q14）：`FrameHeader.dep_seq`（`protocol.hpp:169-178`，0 = 无依赖）表示「依赖的不可靠 seq」。带 `dep_seq` 的帧在依赖未满足时进 `Relation` 的 pending 队列；依赖满足后若它自己的 seq 已被更新的帧越过，按 DropStale 丢弃（`channel.hpp:19-22`）。所以它保证的是**因果语义**，不是可靠有序。

**典型误用**：以为 DATAGRAM 会重传（Q14 错答）；把 `dep_seq` 当 TCP 序号。

### 2.6 FEC：带宽换延迟

**职责**：组内冗余，让丢包在**同一 RTT** 内恢复；不替代重传。

**关键点**

- `ValidShape`（`fec.cpp:55`）：`k∈[2,64]`、`m∈[1,16]`、XOR 只允许 `m=1`、`k+m<=255`（GF(256)）。
- Cauchy 系数（`fec.cpp:48-53`）：`x_j=k+j`、`y_i=i`，两集合不相交保证 `(k+j)^i != 0`。
- 视频通道 XOR（`channel.cpp:1548-1555`）：按 `max_len` 零填充后逐字节异或；组内长度表编进载荷（`:1557-1560`）。
- 可靠通道：`AppendFecSource`（`:1911`）→ `FlushFecGroup`（`:1960`）→ `SendFecOutbox`（`:2032`）；接收 `OnFecFrame`（`:2075`）→ `TryRecoverFec`（`:2139`）→ `PruneFecGroups`（`:2259`）。
- 开关：`LinkConfig::video_fec_group_size=0`（`link.hpp:146`，默认关）；`ReliableChannel::Config::fec`（`channel.hpp:346`）；PeerHub 在 `Open` 时把 `m_config.fec` 灌进 reliable（`peer_hub.cpp:328`）。

**划算区间（Q03）**：低丢包（~1% 以下）冗余几乎全浪费；中等丢包 + 高 RTT 收益最大；20%+ 仍要重传兜底。**仓库只有 PeerHub 5% 丢包重传 -90.3% 一个点（提交 `475cc15`）**，没有 FEC on/off 的吞吐-延迟曲线 → **拐点未实测**。

**典型误用**：以为 FEC 能替代重传；以为开了 `video_fec_group_size` 所有通道都 FEC（只影响 video 发送端，`link.hpp:144-146`）。

### 2.7 BBR：用模型而不是丢包判拥塞

**职责**：带宽 + min_rtt + pacing 决定 inflight。

**相位**（`congestion.hpp:52-57`）：`kStartup`（指数探测）→ `kDrain`（排空队列）→ `kProbeBw`（1.25/0.75/1.0 摆动）→ `kProbeRtt`（10s 一次压 cwnd 刷 min_rtt，`:116-117`）。

**关键参数**（`congestion.hpp:62-113`）：`mss_bytes=1200`、`initial_cwnd_packets=64`、`min_cwnd_packets=4`、`max_cwnd_packets=256`、`min_rto_ms=200`、`max_rto_ms=2000`、`startup_cwnd_gain=2.885`、`startup_exit_loss_rate=0.20`、`probe_bw_cycle_rounds=8`。

**关键函数**
- `CongestionController::OnAck`（`congestion.cpp:339`）→ `BbrOnAck`（`:456`，startup 分支 `:479`）。
- `OnLoss`（`:359`）→ `BbrOnLoss`（`:629`）：`congestion_signal = !timeout && state != kStartup && ...`（`:640`）——**startup 期丢包不算拥塞**。
- `OnTimeout`（`:372`）→ `BbrOnLoss(..., timeout=true)`。
- 两个重载：`OnAck(packets,rtt,now)`（`:880`）是给通道用的窄接口，内部转 `AckSample`。

**为什么高丢包链路上 BBR 不崩（Q02）**：CUBIC 把丢包当拥塞信号乘性减；BBR 用 `BDP = bw × min_rtt` 定 inflight、用 pacing 定速率，随机丢包不再直接触发减窗。**但**本实现是 BBRv1 最小子集：无 ack aggregation、无包级 round 判定、无完整 `inflight_hi` 探测；竞争公平性与深队列 RTT 膨胀是已知问题，**仓库未实测**。

**典型误用**：以为 BBR 对丢包免疫（区间丢包仍要靠重传）；把 `algorithm=kNone` 当成「关掉拥塞控制的优化」（只是回归对照，`congestion.hpp:44-48`）。

### 2.8 0-RTT：省一轮 RTT 的代价

已在场景 C 拆过步骤，这里只补三点：

- **票据用 PSK 加密**（`link.cpp:2513`），不支持 X25519 会话（`:2488`）；
- **监听端可能还没有会话状态**：`HandleFrame` 的 0-RTT 分支（`:1462-1481`）是「按 conn_id 查缓存并试解」，解不开就不改状态——这是「服务端重启后仍能接住 0-RTT」的实现方式；
- **应用层责任**：0-RTT 只适合幂等请求；非幂等操作必须应用层去重（协议层区分不了重放与重复）。

**未实测**：重放攻击面没有攻防测试（`docs/exam/03-bento.md:306-307`）。

### 2.9 SACK：为什么加密会话恒降级

**机制**：HELLO/HELLO_ACK 载荷 12B 之后可选追加 2B caps（`protocol.hpp:93-96`）；`kCapSackExt=0x0001`（`:101`）；双方都置位才启用，任一端关就退回 32 位位图，线格式与旧协议逐字节一致。

**降级原因**（Q10）：PSK 加密会话的 HELLO body 长度固定（旧端严格校验 46/78B，`crypto.hpp:95-103`），追加 caps 会破坏旧端校验 → **加密会话强制降级**（`docs/audit/audit-sack.md:48`、`:176`）。

**数据帧永远只带 8B ACK**（`protocol.hpp:189-197` 注释：帧头 flags 没有空闲位），扩展只走纯 ACK 帧。

**降级损失**：只有 32 位位图，乱序窗口 >32 的已收项无法告诉对端，PTO 会重复重传；块数上限 16（`kMaxSackBlocks` `:80`）。

**典型误用**：以为「开了 `enable_sack_ext` 就有多块 SACK」（加密会话拿不到 caps）。

### 2.10 PeerHub：一个 socket × N 个 peer

**职责**：1:N 场景；按 `conn_id` 路由，per-peer 密钥/重放窗口/通道表。

**关键点**

- 为什么不改 Link：Link 的单 peer 假设散落在 2000+ 行实现里；两条 UDP 路径并存是**有意**的（避免全量回归，Q20）。
- `PeerHub::Open`（`peer_hub.cpp:323`）：先合并 FEC 配置（`:328`），再校验 PSK（`:331-341`）——**默认安全策略先于 socket 创建**。
- `HandlePacket`（`:1572`）：解外层头 → 按 conn_id 找 peer → `kFec` 帧按 channel 路由到 per-peer 可靠通道（`:1557-1565`）。
- `m_mutex` 仍是 `recursive_mutex`（`peer_hub.hpp:432`），与 Link 的「非递归 + 锁外派发」不同——**迁移经验时注意**。
- 容量参考：3 peer × 50 STREAM + 50 DATAGRAM 隔离，307KB/80ms ≈ 30.7 Mbit/s 是**旧口径**（`user/docs/03-bento.md:331`），未复跑。

**典型误用**：以为 PeerHub 是 `vector<Link>`（Q20 错答）；以为 FEC 在 Hub 层统一编码（实际挂在 per-peer 通道上，Q08）。

### 2.11 弱网工具：怎么复现那套基线

| 工具 | 路径 | 用途 |
|---|---|---|
| tc netem 脚本 | `tests/bento/test_netem.sh` | 只对探针端口加丢包/延迟（prio + u32，不波及 lo 上其它流量）；无权限/无 netem 输出 SKIP + exit 77 |
| 弱网探针 | `tests/bento/netem_probe.cpp` | 单场景探测 |
| 弱网矩阵 | `tests/bento/bench_weaknet.cpp` | 批量回环吞吐 + 5 万帧弱网矩阵（丢包/延迟/FEC/BBR 对照） |
| 用户态丢包代理 | `tests/bento/loss_proxy.hpp`、`udp_proxy.hpp` | 无 root 也能造丢包/延迟 |
| 旧基线原始数据 | `tests/bento/compare_results.baseline-2026-10-28.tsv` | 5 场景 × tcp/udp/bento × 3 轮；表头含 `sent/delivered/ordered/p50_ms/p95_ms/p99_ms/retrans/wall_ms/throughput_mbps` |

**怎么读旧基线（Q09）**：40% 丢包送达率 66.7% 的根因是**一轮握手 15s 超时**（其余两轮 200/200），不是数据面丢包；之后用 `handshake_copies=2`、16 轮握手、SACK 扩展、快速重传、BBR 针对修改过，但**没有用同一口径重跑**。所以正确表述是「2026-10-28 基线在 40% 丢包下 66.7%，根因已定位并改过，新数据未实测」。

### 2.12 握手：16 轮 × 每轮 2 份

**为什么是 16 轮**（Q11）：`hello_max_attempts=16`（`link.hpp:99`）、`handshake_copies=2`（`:103`）。每轮 2 份独立丢失概率 `p²`，32 份全丢约 `p^32`；40% 丢包下已可忽略。代价是最坏耗时随 PTO 退避累加（旧基线里「一轮 15s 超时」就是它）。

**`SendHello` 逐步拆**（`link.cpp:2064`）

1. 未配置 peer 直接返回（`:2065-2067`）。
2. `m_hello_attempts>0` 记 `hello_retransmits`（`:2068-2070`），再 `++m_hello_attempts`（`:2071`）。
3. **加密模式复用同一份缓存密文（同一 wire seq）**（`:2073-2076`）——监听方按「当前 nonce 的重复 HELLO」幂等重发 HELLO_ACK，**不会把重传误判为一次重放**。
4. 明文模式：每次重新握手都从「未协商」开始（`m_sack_ext=false`，`:2081`），按 `handshake_copies` 发 N 份（`:2091-2094`）。
5. 排下一次：`m_next_hello_ms = now + m_hello_pto_ms`；`m_hello_pto_ms = min(×2, hello_max_pto)`（`:2096-2097`），初值 `hello_initial_pto_ms`（`:756`）。

**超时判定**（`Tick` `:2144-2162`）：额外宽限 = 当前 PTO（`:2149-2152`）——固定超时会在「最后一次 HELLO 刚发出」时判失败，等于白扔那轮重传。

**失败路径**：`hello_attempts >= hello_max_attempts` 或超过 `handshake_timeout + grace` → `Fail("hello timeout")`（`:2153-2158`）。

**典型误用**：以为重传越多越好（占用的是最坏握手时长）；以为握手丢包能靠 SACK 恢复（握手阶段没有后续包触发重复 ACK，只能靠冗余 + 退避）。

---

---

## 第三部分 · 深：三个「架构改动 > 汇编优化」的完整案例

> 第二部分按模块讲「是什么」；这一部分按**改动**讲「为什么动结构、动了多少、怎么验证」。
> 三个案例都满足同一个模式：瓶颈不在指令，而在锁/所有权/唤醒模型；
> 修复后收益是数量级，而不是百分之几。

### 3.0 五层映射（这一部分补的是层 3–5）

| 层 | 内容 | 位置 |
|---|---|---|
| 层 1 · What | 为什么 UDP 上要自己造可靠层、边界在哪 | 开篇「为什么不用 TCP」 |
| 层 2 · How | 协议/加密/Link/Channel/FEC/BBR/0-RTT 的结构 | 第一部分 + 第二部分 |
| 层 3 · 源码 | `RunOnce`/`DrainTx`/`SendHello` 的逐行拆解 | 第二部分 2.3/2.12 + 本部分案例一 |
| 层 4 · 实战 | 三个真实改动：锁、内存、唤醒 | 本部分 3.1–3.3 |
| 层 5 · 性能 | p99 1405→0.045 ms、RSS 0.96×、p99 −45% | 本部分 3.1–3.3 的数据段 |

三个案例的顺序不是随意的：**先锁（数量级）、再内存（数量级但另一笔账）、最后唤醒模型（接近数量级）**。
它们共同回答一个问题：为什么这个项目的优化顺序是「架构 > 汇编」。
### 3.1 案例一：Link 收发分离（p99 1405 ms → 0.045 ms）

**症状**：弱网下发送延迟 p99 达到 **1405 ms**，而网络 RTT 只有几十毫秒；吞吐也上不去（1e4 包基准 7.4× 提升空间）。

**旧结构（问题根因）**：`RunOnce` 第一行就 `lock_guard<recursive_mutex>`，把

```text
poll(timeout_ms)  →  recvmmsg  →  逐帧 HandleFrame  →  Tick  →  回调派发
```

整段罩在同一把锁里。发送入口（`TrySendStream`/`SendPacket`）要拿**同一把** `m_mutex` 才能 `sendto` 或改流控状态，于是：

1. 接收线程持锁一个 `timeout_ms`（默认 50 ms，弱网下 poll 经常等满）；
2. 发送线程在锁上排队，排队长度 = 并发发送线程数；
3. p99 ≈ 锁持有时间 × 排队轮数，最坏可以累到秒级——**1405 ms 不是网络抖动，是锁排队**。

用排队论看更清楚：把 `m_mutex` 看成单服务器、服务时间 = 一次 `RunOnce`（poll 等到事件 + 处理一帧），到达率 = 发送请求率。利用率接近 1 时，等待时间发散；p99 对服务时间的长尾极其敏感。**换更快的锁、把 `recursive_mutex` 换成 `mutex`、缩小临界区几个字段，都不改变「单服务器」这个结构**。

**新结构（三步）**：

1. **接收与发送解耦**：`RunOnce` 的 poll 与 `HandleFrame` 仍在 `m_mutex` 内（协议状态必须串行），但**发送不再抢这把锁**——`Send*` 只把帧塞进 `m_tx_queue`（由 `m_tx_mutex` 保护，只护队列本身）。
2. **单 drainer**：`DrainTx`（`src/bento/net/link.cpp:1872`）用 `m_tx_draining` 保证同一时刻只有一个线程做「取批 → seal → sendto」；其他线程把帧入队后立即返回，不参与发送。
3. **事件锁外派发**：接收路径「锁内入队事件、锁外 `DispatchEvents`」（`link.cpp:877`），回调里再慢也不占协议锁。

**为什么 p99 能到 0.045 ms**：发送路径只剩「入队（一次短锁）+ 由 drainer 批量 `sendto`」；它不再等待接收循环，也不再受 `timeout_ms` 影响。0.045 ms 是「一次入队 + 一次系统调用」的量级，不是网络 RTT 的量级——这正是「锁延迟」与「网络延迟」的分界。

**吞吐 7.4× 的来源**：旧结构下接收线程持锁期间发送完全停摆，链路有效带宽 = 接收循环空闲时间 × 发送速率；新结构下接收与发送重叠，UDP 的 send/recv 可以并行。

**锁顺序（改动的隐藏契约）**：固定 `m_mutex → m_tx_mutex`（`link.cpp:817` 注释），`Close` 清理段也遵守。反了就是 ABBA 死锁；`m_tx_mutex` **只保护队列**，不保护链路状态（`:791-792` 注释），这是能拆开的前提。

**怎么验证（本文未复跑，给出可执行步骤）**：

```bash
# 1) 取提交 5a2c0fa 前后两个二进制（或 git checkout 该提交父/子各编一次）
# 2) 同一弱网配置、同一机器、同一包序列：
bash tests/bento/test_netem.sh <netem_probe>
./bench_weaknet --weak-frames=10000 --loss=5 --delay=50
# 3) 同时抓锁竞争（旧结构应有大量 futex 等待）：
perf lock record -p <pid> -- sleep 10 && perf lock report
# 4) 报告必须带：p50/p99/p999、吞吐、丢包率、CPU 型号、是否 taskset
```

**边界**：1405 ms → 0.045 ms 与 7.4× 出自提交 `5a2c0fa` 的口径，本文**没有复跑**；结构原因可以从 `src/bento/net/link.cpp:828-882`（`RunOnce`）与 `:1872-1861`（`DrainTx`）直接读出来。不要把它当成「所有弱网场景都 3 万倍」——它是「锁排队消失」的收益，不是协议本身的收益。

**这个案例的一般化**：判断该不该动架构，看「临界区里有没有阻塞调用（poll/read/write/回调）」。有 → 先拆锁/拆所有权；没有 → 再谈指令优化。

### 3.2 案例二：WsServer 每连接内存（预取 16 KiB → 1 KiB、526 B 栈 → 成员）

**症状**：20k 长连接下 RSS 是 Go 的 2.2–2.3 倍（`ws-200k-bench.md`：690.4 MB vs 298.2 MB），p99 也是 2–3 倍。

**内存账本（每连接）**：

| 项 | 旧默认 | 新默认 | 20k 连接差额 |
|---|---|---|---|
| WebSocket 读预取 | 16 KiB（`network/websocket.hpp:99-103`） | **1 KiB**（`bento/net/ws_server.hpp:139` `websocket_prefetch_bytes{1024}`） | 省 300 MB |
| 握手读块 | 4 KiB | 4 KiB（`ws_server.cpp:23`） | 不变 |
| 发送小帧缓冲 | 526 B 在**栈**上（`write_frame_unlocked`，`std::array<uint8_t, 14+512> buffer{}`） | 移到成员 | 每帧省 10–20 条指令 + 526 B 清零 |
| 连接对象/请求/回调 | 固定 | 固定 | 不变 |

**为什么 16 KiB 是主项**：预取缓冲是**每连接常驻**的；20k × 16 KiB = 320 MB，20k × 1 KiB = 20 MB。仅这一项就能解释 RSS 从 690 MB 降到 220 MB 量级的大部分。三层证据链：

1. 代码：`network/websocket.hpp:99-103` 的 16 KiB 默认与 `bento/net/ws_server.hpp:139` 的 1 KiB 默认；
2. 压测：`ws-200k-bench.md` 的 RSS 斜率（每连接边际内存）；
3. 报告：`three-piece-status-2026-11-04.md` 的「读预取 16K→1K、握手 4K→1K、526B 栈→成员 → RSS 227.7 MB = Go 的 0.96×」。

**一个必须指出的文档漂移**：`ws_server.cpp:52-56` 的注释写「默认按 idle 心跳取 4 KiB」，但 `ws_server.hpp:139` 的实际默认是 **1024**；`NormalizeConfig` 只做 `max(512, 配置值)`。也就是说：配置为 0 时走 base 的 16 KiB 默认（注释说的就是这个分支），配置非 0 时下限 512。**读代码要以 hpp 的成员初值为准，注释与代码在此处不一致**。

**526 B 栈帧为什么值得单独说**（`asm-network-review.md` P16）：`write_frame_unlocked` 的栈帧是 `0x1428 = 5160 B`，主要来自那个 526 B 的 inline buffer。超过 4 KiB 保护页 → 每次调用多一条栈探测写；`buffer{}` 还会先零初始化 526 B。每帧省 10–20 条指令看起来小，但小帧路径是每消息一次，20k 连接 × 心跳频率下是可观的热路径。修法：移到成员/线程局部，或拆「小帧/大帧」两条路径。

**为什么内存达标了 p99 还差 2.37 倍**：内存优化减少的是「每连接占用」，不改变「谁来 poll、谁来唤醒」。p99 的瓶颈在 IO 唤醒模型（见 3.3）；两件事必须分开做、分开验证。**这也是为什么不能拿 RSS 达标去宣布延迟达标**。

**怎么验证**：

```bash
# 1) 每连接边际内存：5k/10k/20k 三档，读 /proc/<pid>/status 的 VmRSS
# 2) 分解到分配点：heaptrack / massif，或 LD_PRELOAD 统计 malloc 尺寸分布
# 3) 确认预取真的生效：连接建立后打印 config.websocket.prefetch_bytes
# 4) 对比必须同客户端、同档位、同心跳间隔
```

### 3.3 案例三：单 poller（p99 3.078 ms → 1.691 ms，−45%）

**症状**：20k idle WebSocket 心跳 p99 为 Go 的 2.37 倍（1.810 ms vs 2.994–4.294 ms），RSS 已达标。

**旧结构**：每个 worker 自己在空闲时 `epoll_wait`（`io.cpp` 的 `poll_once`），所有 worker 共享同一个 epoll fd。后果：

1. **唤醒风暴**：一个就绪事件可能唤醒多个 worker（thundering herd），只有一个拿到任务，其余白跑一次调度；
2. **就绪队列争用**：N 个 worker 同时 `epoll_wait` 同一 fd，内核就绪链表上的取事件操作互相竞争；
3. **事件解析在 worker 内**：取到事件后立刻解析/派发，解析慢会拖长下一次 `epoll_wait` 的间隔。

**新结构（`GO2CPP_IO_SINGLE_POLLER=1`）**：

| 组件 | 旧 | 新 |
|---|---|---|
| poll 者 | 每个 worker | **一个 poller 线程**（`poll_loop`，`io.cpp:1785`） |
| 事件传递 | 内核 → worker 直接处理 | poller 批量入环形队列（8192 容量，`io.cpp:2348`），worker 批量取 ≤256（`drain_event_batch`，`:1999`） |
| 积压处理 | 无 | poller 发现队列有积压时下一轮 `epoll_wait` 超时取 0，持续搬空 |
| 唤醒 | 每个事件唤醒 | 批量入队后一次唤醒；worker 处理完仍有积压再唤醒下一个 |

**实测（`iomanager-et-investigation.md` §三，20k 连接、同机同参数）**：

| 组 | p50 | **p99（中位）** | p999 | RSS |
|---|---:|---:|---:|---:|
| B：新二进制、开关关 | 1.056 ms | 3.078 ms | 4.343 ms | 228.8–229.6 MB |
| C：新二进制、开关开 | 0.614 ms | **1.691 ms** | 3.480 ms | 228.1–229.0 MB |
| 变化 | −42% | **−45.1%（1.82×）** | −19.9% | <0.4% |

- 20k 连接全部握手成功，`ping_timeout=0`、`disconnects=0`；
- A（旧二进制关）与 B（新二进制关）在噪声内一致 → **默认路径未退化**；
- 开关默认关闭（`io.cpp:682` 读 env），因为它是行为改变，不是纯优化。

**为什么有效**：p99 的主体是「事件从就绪到被处理」的排队时间。旧结构下 N 个 worker 争抢，排队时间随并发度上升；新结构把「取事件」收敛到一个线程，worker 只做「批量取 + 处理」，队列深度可控、唤醒次数从 O(事件数 × worker 数) 降到 O(批次数)。**这不是把 epoll 换得更快，是把「谁负责等待」这件事改对了**。

**边界与代价**：

1. poller 是单点：它的 `epoll_wait` 循环不能做重活，否则所有连接一起卡；代码里它只入队、不解析。
2. 环形队列满时「就地处理溢出」（`enqueue_events`，`:1941`）——这是背压，不是丢事件。
3. 该模式仍是 env 开关，没有默认开启；生产启用前要跑目标负载的 p99/p999 与 CPU。
4. 客户端单线程在 20k 档已吃掉约 0.57 核，心跳 RTT 含客户端排队；横向差值可信，绝对值有下限。

**怎么复现**：

```bash
# 关（默认）
BIN=/home/ubunturoot/wsbench-bin bash app/ws_bench/run_bench.sh go2cpp 20000 20 18083 4
# 开
GO2CPP_IO_SINGLE_POLLER=1 BIN=/home/ubunturoot/wsbench-bin \
  bash app/ws_bench/run_bench.sh go2cpp 20000 20 18082 4
```

报告必须带：连接数、时长、pin 核、客户端单线程限制、p50/p99/p999、RSS。

### 3.4 三个案例的共同结论

| 案例 | 瓶颈类型 | 改动 | 收益 | 验证方式 |
|---|---|---|---|---|
| Link 收发分离 | 锁罩住阻塞调用 | 拆锁 + 单 drainer | p99 3.1 万倍、吞吐 7.4× | 提交口径 + 结构可读 |
| WsServer 内存 | 每连接常驻缓冲 | 16 KiB→1 KiB、栈→成员 | RSS 690→227.7 MB（0.96× Go） | 压测斜率 |
| 单 poller | 唤醒模型 | N worker poll → 1 poller + 队列 | p99 −45.1% | A/B 同机对照 |

三条都符合「架构优化 > 汇编优化」：**先问「临界区里有没有阻塞」「每连接有没有常驻大缓冲」「谁在等待」**，再问「这条指令能不能省」。反过来做，就是在错误的架构上抠百分点。

一个反例：把 `m_tx_mutex` 换成无锁队列并不会让 p99 更好——瓶颈是「接收循环持锁时长」，不是「入队那一次 CAS」。这就是「先架构后汇编」的具体含义。

### 3.5 本部分小结

1. **p99 的敌人通常不是网络，是排队**：锁、唤醒、单线程解析都会制造排队。
2. **内存与延迟是两笔账**：预取从 16 KiB 降到 1 KiB 解决 RSS，不解决 p99；单 poller 解决 p99，不改变 RSS。
3. **默认关闭的开关是负责任的**：单 poller 改变行为，所以保留 env 开关与 A/B 对照，而不是直接改默认。
4. **未复跑的数字必须标注**：1405 ms 与 7.4× 来自提交，本文只做结构解释。
5. **三个案例各有自己的失败条件**：锁顺序反了是 ABBA 死锁；预取配置成 0 会退回 base 的 16 KiB；事件队列满会走「就地处理溢出」的背压路径。改动前先把失败条件写进测试，再谈收益。
## 附：读完之后的自检清单

1. 能否不看文档画出「SendStream → m_unsent → m_inflight → TX 队列 → DrainTx → SealFrame → sendto → 对端 OnAck」的完整链路，并指出每一步的锁？
2. 能否解释为什么 `m_tx_mutex` 只保护队列、`m_mutex` 非递归，以及 `PeerHub` 为什么还敢用 recursive？
3. 能否说出一次 PTO 事件为什么只降一次窗，`m_pto_recovery` 在什么时候清除？
4. 能否解释 TLP 为什么不递增 `retransmits`、不参与退避？
5. 能否说清 0-RTT 票据为什么只对纯 PSK 导出，以及重放窗口如何按票据的 `next_wire_seq` 初始化？
6. 能否解释 SACK 扩展在加密会话里为什么恒降级，降级后损失什么？
7. 能否指出 FEC 挂在哪一层、默认开不开、拐点为什么「未实测」？
8. 能否在 10 分钟内用 `tests/bento/test_netem.sh` + `bench_weaknet` 复现一次弱网矩阵并报出条件？

## 附-2：本文明确「未实测」的地方

- **Link 收发分离的 p99 1405ms → 0.045ms、吞吐 7.4×**：出自提交 `5a2c0fa`，本文**未复跑**；结构原因可以从 `src/bento/net/link.cpp:828-882` 直接读。
- **PeerHub 5% 丢包重传 -90.3%（517 checks）**：出自提交 `475cc15`，未复跑。
- **3 peer ≈ 30.7 Mbit/s**：旧口径（`user/docs/03-bento.md:331`），未复跑。
- **2026-10-28 弱网基线全表**：原始数据在 `tests/bento/compare_results.baseline-2026-10-28.tsv`，之后协议栈改过（SACK/快速重传/BBR/FEC），**没有重跑**。
- **FEC 划算区间拐点**：只有 5% 丢包一个点，没有 on/off 曲线。
- **0-RTT 重放攻击面**：只有代码防线，没有攻防实测。
- **BBR 竞争公平性 / 深队列 RTT 膨胀**：仓库无实测，只能定性。
- **自研 ChaCha20-Poly1305 vs OpenSSL**：`asm-tls.md` 的 13.41× 是**加 SIMD 之前**的口径；`crypto.cpp:367` 起已有 AVX2/AVX-512 后端，**现状未实测**。
- **PMTUD**：默认关闭（`link.hpp:149`），仓里有 `tests/bento/test_pmtud.cpp`，本文未跑。

要复跑时最省事的入口：`GO2CPP_WITH_BENTO=ON` 后跑 `ctest -R bento`，弱网单场景用 `bash tests/bento/test_netem.sh <netem_probe>`，矩阵用 `bench_weaknet --weak-frames=10000`。
