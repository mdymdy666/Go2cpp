# Bento 传输（交接文档）

> **定位**：跑在 UDP 上的应用层可靠传输协议（Bento Transport Protocol v0.2）：可靠有序 STREAM、
> 不可靠 DATAGRAM、视频 VIDEO 三种通道 + SACK/快速重传 + FEC + BBR 拥塞控制 + 连接迁移 + 加密。
> **源码**：`include/go2cpp/bento/`（44 个头文件）、`src/bento/`（40+ 实现）
> **构建**：`cmake/Go2CppBento.cmake`，开关 `GO2CPP_WITH_BENTO`（默认 **OFF**）
> **测试**：`tests/bento/`；线上协议全文 [tests/bento/PROTOCOL.md](../../tests/bento/PROTOCOL.md)
> **模块设计**：[bento-foundation](../../docs/modules/bento-foundation.md)、[bento-transport](../../docs/modules/bento-transport.md)、
> [bento-session](../../docs/modules/bento-session.md)、[bento-security](../../docs/modules/bento-security.md)、[bento-scenarios](../../docs/modules/bento-scenarios.md)

## 1. 这个模块是什么

TCP 在弱网（随机丢包、抖动、NAT 重绑定、移动网络切换）下会被「队头阻塞 + 丢包即降速」
拖死，而裸 UDP 又没有任何可靠性。Bento 在 UDP 之上重建了一条**可会话延续的可靠传输**：

- **三种通道语义**：STREAM（可靠有序，seq + ACK + 重传 + 去重）、DATAGRAM（不可靠、可丢、
  单调 seq 去旧）、VIDEO（分片视频帧，关键帧标记 + 可选 XOR FEC）；
- **会话按 `conn_id` 识别**，不按源 IP:port；对端换 NAT 端口后会话可继续（连接迁移）；
- **丢包恢复**：SACK 多块扩展 + 快速重传 + 动态 PTO（RFC 9002 风格）+ 可选 FEC；
- **拥塞控制**：默认 BBRv1 子集（带宽 + min_rtt + pacing），保留 NewReno 作对照；
- **安全**：32B PSK 的 ChaCha20-Poly1305 认证加密，X25519 前向保密，HELLO 时间戳防重放。

它是 JinShan relay 场景（1 被控 : N 主控、操控/视频/文件混合流量）的传输底座；也用于
`app::Session`、`PeerHub`、`LinkServer` 等上层。**不是**通用 RPC 框架，也不兼容 WebRTC。

## 2. 架构 / 数据流

```
┌──────────────────────────── 应用层 ────────────────────────────┐
│ app::Session / LinkServer / OmniServer / relay / fetch / backup │
└───────────────────────────────┬────────────────────────────────┘
                                │ SendStream / SendDatagram / SendVideoFrame
┌───────────────────────────────▼────────────────────────────────┐
│ Link（单 peer）            PeerHub（1 被控 : N 主控，按 conn_id 路由）│
│  ├─ ReliableChannel  (STREAM, seq/ACK/SACK/重传/流控/分片重组)   │
│  ├─ UnreliableChannel(DATAGRAM, DropStale)                      │
│  ├─ VideoChannel     (VIDEO, 分片 + XOR FEC)                     │
│  ├─ Relation         (因果屏障 kBarrier / dep_seq)               │
│  └─ crypto/x25519/keystore（可选加密 + 前向保密）                 │
└───────────────────────────────┬────────────────────────────────┘
                                │ protocol.hpp：16B 帧头 + 可选 ACK/分片/FEC 子头
┌───────────────────────────────▼────────────────────────────────┐
│ UdpSocket（RAII，非阻塞）  /  UdpBatch（recvmmsg/sendmmsg + SO_REUSEPORT）│
└────────────────────────────────────────────────────────────────┘
```

### 2.1 帧格式（v0.2，16 字节头，大端）

| 偏移 | 字段 | 语义 |
|---:|---|---|
| 0 | ver u8 | 协议版本，当前 **2**；不等直接拒绝 |
| 1 | type u8 | `FrameType`（见下） |
| 2 | chan u8 | 通道 id：0 move / 1 click / 2 video（应用可自定义） |
| 3 | flags u8 | ACK / KEY / RETRANSMIT / FRAGMENTED / ENCRYPTED / FEC / RESUME / RESUME_ACK |
| 4 | conn_id u32 | 连接标识（HELLO 协商），路由与迁移都靠它 |
| 8 | seq u32 | 通道内序列号 |
| 12 | dep_seq u32 | 依赖的不可靠 seq，0 = 无依赖 |

`FrameType`：`kHello(0x01)`、`kHelloAck(0x02)`、`kStream(0x03)`、`kDatagram(0x04)`、
`kVideo(0x05)`、`kAck(0x06)`、`kBarrier(0x07)`、`kMaxData(0x08)`、`kPmtudProbe(0x09)`、
`kPmtudAck(0x0A)`、`kFec(0x0B)`。

- 带 `kFlagAck` 的帧在帧头后跟 8B ACK 段（`ack_seq u32 + ack_bits u32`，位图覆盖 32 个 seq）；
- 纯 ACK 帧可带 **SACK 多块扩展**（8B 基础 + 1B 块数 + 4n 块，最多 16 块），双方通过
  HELLO caps 位 `kCapSackExt=0x0001` 协商，未协商自动降级为 32 位位图；
- STREAM 帧超过 1300B 自动分片（8B 子头：frame_id + frag_idx + frag_cnt）；
- 加密帧（`kFlagEncrypted`）载荷前有 AEAD 封装，帧头参与 AAD。

### 2.2 线程模型（交接重点）

- **收包与协议处理默认是单线程**：`Link::RunOnce(timeout_ms)` 做 poll → recvmmsg → 解帧 →
  更新通道/定时器 → 派发回调。`RunFor(ms)` 是循环封装。
- **发送是线程安全的**：`SendStream`/`SendDatagram`/`SendVideoFrame`/`ConcurrentSend` 可在
  任意线程调用；消息之间的先后顺序不保证，通道内由 seq 保证有序。
- **`rx_threads > 0`** 只是把 `poll+recvmmsg` 摊到多核（每队列无锁环），**协议处理与回调仍在
  `RunOnce` 调用线程**——回调线程语义不变。
- **回调在 `RunOnce` 内、锁外派发**：`on_connected/on_stream/on_datagram/on_video` 里可以继续
  调 `Send*`/`Close`；回调必须快速返回，回调里跑长任务会阻塞整条链路的收包。
- **PeerHub** 同样是单线程 `RunOnce`，回调里 `RemovePeer`/`Close` 是安全的（销毁延迟到安全点）。

### 2.3 握手与状态机

```
LinkState: kIdle → kConnecting → kConnected → kClosed / kTimedOut

客户端 Connect()：HELLO（magic "BENT" + session + conn_id [+ X25519 公钥]）
服务端 RunOnce：  校验 → HELLO_ACK（回显 session/conn_id，协商 caps/公钥）
双方：            派生会话密钥（PSK 或 PSK+X25519）→ kConnected
```

- HELLO 默认重传 16 轮（`hello_max_attempts`）、每轮 2 份冗余（`handshake_copies`），
  PTO 从 200ms 指数退避到 2s；40% 丢包下也能把失败概率压到可忽略。
- 加密默认开启（`require_encryption=true`）：没有 32B PSK 时 `Open/Connect` 直接失败并返回
  `SecurityError::kEncryptionRequired`；监听端拒收明文 HELLO，防降级。
- 0-RTT 恢复：`ExportSessionTicket()` / `Resume(ticket)` **只对纯 PSK 会话**开放；X25519 会话
  不导出票据（避免把前向保密静默降级）。

## 3. 对外 API（关键接口）

### 3.1 `Link`（单 peer）

```cpp
struct LinkConfig {
    std::string bind_address = "127.0.0.1";
    std::uint16_t bind_port = 0;              // 0 = 内核分配
    std::uint32_t rx_queues = 1;              // SO_REUSEPORT 队列数（1..16）
    std::uint32_t rx_threads = 0;             // 0 = RunOnce 调用线程收包
    std::string remote;                       // 非空 = 主动端；空 = 监听方
    std::uint16_t peer_port = 0;
    std::uint32_t handshake_timeout_ms = 5000;
    std::uint32_t peer_timeout_ms = 3000;
    std::uint32_t heartbeat_interval_ms = 500;
    std::uint32_t ack_interval_ms = 20;
    std::uint32_t hello_initial_pto_ms = 200;
    std::uint32_t hello_max_pto_ms = 2000;
    std::uint32_t hello_max_attempts = 16;
    std::uint32_t handshake_copies = 2;       // 每轮 HELLO 冗余份数
    bool enable_sack_ext = true;              // 加密会话会自动降级
    bool require_encryption = true;           // 生产别关
    bool allow_legacy_peer = false;
    std::vector<std::uint8_t> pre_shared_key; // 32B
    bool enable_x25519 = true;
    std::uint32_t handshake_replay_window_ms = 30000;
    ReliableChannel::Config reliable;         // STREAM 通道参数
    std::uint16_t video_fec_group_size = 0;   // 0=关，2..8 开启 XOR FEC
    bool enable_pmtud = false;
    std::uint32_t session_ticket_lifetime_ms = 60000;
    std::uint32_t migration_grace_ms = 3000;
};

class Link {
public:
    explicit Link(LinkConfig config);
    bool Open(std::string* error = nullptr);           // bind socket
    bool SetPreSharedKey(const std::uint8_t* key, std::size_t bytes); // 32B
    bool Connect();                                     // 主动端发 HELLO
    void Close();
    int  RunOnce(int timeout_ms);                       // 单线程事件循环
    int  RunFor(int duration_ms);

    bool SendStream(std::uint8_t channel, const void* data, std::size_t bytes,
                    std::uint32_t dep_seq = 0);         // 线程安全
    SendStatus TrySendStream(...);                      // 区分 WouldBlock
    bool SendDatagram(std::uint8_t channel, const void* data, std::size_t bytes,
                      std::uint32_t dep_seq = 0);
    bool SendVideoFrame(std::uint32_t frame_id, bool key, const void* data, std::size_t bytes);
    bool SendBarrier(std::uint32_t barrier_seq);

    LinkState state() const;  bool connected() const;
    std::uint32_t conn_id() const;
    std::uint32_t PathMtu() const;
    bool Migrate(const std::string& new_address, std::uint16_t new_port = 0, std::string* err = nullptr);
    std::vector<std::uint8_t> ExportSessionTicket();
    bool Resume(const std::vector<std::uint8_t>& ticket);

    LinkStats Stats() const;          // 聚合：链路 + RTT/拥塞/流控/带宽
    std::string StatsJson() const;
    std::uint64_t BandwidthEstimate() const;   // bit/s
    double LossRate() const;

    std::function<void()> on_connected;
    std::function<void(const char* reason)> on_disconnected;
    std::function<void(std::uint8_t ch, const std::uint8_t* data, std::size_t n)> on_stream;
    std::function<void(std::uint8_t ch, const std::uint8_t* data, std::size_t n)> on_datagram;
    std::function<void(std::uint8_t ch, std::uint32_t frame_id, bool key,
                       const std::uint8_t* data, std::size_t n)> on_video;
};
```

### 3.2 `ReliableChannel`（STREAM 语义）

```cpp
struct ReliableChannel::Config {
    RttEstimator::Config rtt;                 // min_pto 50ms / max_pto 2s / initial_rtt 200ms
    bool fast_retransmit = true;
    std::uint32_t fast_retransmit_threshold = 3;
    std::uint64_t reassembly_timeout_ms = 10000;
    CongestionController::Config congestion;  // 默认 BBR
    std::uint32_t initial_max_data = 1U << 20;// 流控窗口（RFC 9000 MAX_DATA）
    std::uint32_t max_data_refresh_ms = 200;
    FecConfig fec;                            // 默认关闭
};
bool Send(const std::uint8_t* data, std::size_t bytes, std::uint32_t dep_seq = 0);
SendStatus TrySend(...);
void OnFrame(const FrameHeader&, const std::uint8_t* payload, std::size_t, const AckState*);
void OnAck(const AckState&);
void OnTimer(std::uint64_t now_ms);
```

- 单条消息上限 `kMaxStreamMessageBytes = 256 × kStreamFragmentBytes`，超过 1300B 自动分片；
- 发送窗口 `kSendWindow = 256`，流控窗口默认 1 MiB；窗口满返回 `SendStatus::kWouldBlock`；
- 重组器按 `frame_id` 聚合，乱序容忍，缺片 10s 超时丢弃。

### 3.3 FEC / 拥塞控制 / PeerHub

```cpp
// FEC：XOR（m=1，开销 1/k）或 GF(256) Cauchy Reed-Solomon（可恢复任意 m 片）
struct FecConfig { bool enabled=false; FecKind kind=FecKind::kXor;
                   std::uint16_t data_shards=8; std::uint16_t parity_shards=1;
                   std::size_t max_payload_bytes=1200; std::uint32_t group_timeout_ms=2000; };
// 边界：k+m <= 255；不同长度分片零填充到 max_len。

// 拥塞控制：kNone / kNewReno / kBbr（默认）
struct CongestionConfig { CongestionAlgorithm algorithm = CongestionAlgorithm::kBbr; ... };
// BBR 相位：kStartup → kDrain → kProbeBw ↔ kProbeRtt

// PeerHub：一个 UDP socket 承载 N 个 peer，按 conn_id 路由
struct PeerHubConfig {
    std::size_t max_peers = 16;
    bool accept_unknown_peers = true;
    bool require_encryption = true;
    std::vector<std::uint8_t> pre_shared_key;
    ReliableChannel::Config reliable;
    FecConfig fec;
};
class PeerHub {
public:
    bool Open(std::string* error = nullptr);
    bool Connect(const std::string& address, std::uint16_t port); // 主动建 peer
    void RemovePeer(peer_id);
    int RunOnce(int timeout_ms);
    bool SendStream(peer_id, std::uint8_t channel, const void* data, std::size_t bytes);
    std::vector<PeerInfo> peers() const;
    PeerHubStats Stats() const;
};
```

## 4. 最小用法（可编译）

两条 `Link` 在本机回环上握手、发一条 STREAM、收回调。**加密默认开启**，示例显式关闭以
保持 20 行；生产必须配 32B PSK（两端相同）。

```cpp
#include "go2cpp/bento/net/link.hpp"

#include <cstdio>

using go2cpp::bento::Link;
using go2cpp::bento::LinkConfig;

int main() {
    LinkConfig server_cfg;
    server_cfg.bind_address = "127.0.0.1";
    server_cfg.bind_port = 0;                 // 内核分配
    server_cfg.require_encryption = false;    // 仅演示！生产用 32B PSK
    Link server(server_cfg);
    if (!server.Open()) return 1;
    const auto port = server.local_port();

    LinkConfig client_cfg;
    client_cfg.remote = "127.0.0.1";
    client_cfg.peer_port = port;
    client_cfg.require_encryption = false;    // 仅演示
    Link client(client_cfg);
    if (!client.Open()) return 1;

    int got = 0;
    server.on_stream = [&](std::uint8_t, const std::uint8_t*, std::size_t n) {
        std::printf("server got %zu bytes\n", n);
        ++got;
    };
    if (!client.Connect()) return 1;

    for (int i = 0; i < 400 && !client.connected(); ++i) {
        client.RunOnce(2);
        server.RunOnce(2);
    }
    if (!client.connected()) return 1;

    client.SendStream(go2cpp::bento::kChannelMove, "hello", 5);
    for (int i = 0; i < 100 && got == 0; ++i) {
        client.RunOnce(2);
        server.RunOnce(2);
    }
    client.Close();
    server.Close();
    return got == 1 ? 0 : 1;
}
```

构建（仓库根目录，Bento 打开）：

```bash
cmake -S . -B build-bento -DGO2CPP_WITH_BENTO=ON
cmake --build build-bento -j
g++ -std=c++17 -Iinclude demo_link.cpp -Lbuild-bento -lgo2cpp_bento -lpthread -o demo_link
./demo_link
```

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/bento/net/protocol.hpp` / `src/bento/net/protocol.cpp` | 364 / — | 16B 帧头、FrameType、ACK/SACK/分片/FEC 子头、HELLO 载荷编解码 |
| `include/go2cpp/bento/net/link.hpp` / `src/bento/net/link.cpp` | 814 / 2000+ | 单 peer 链路：握手、重传、ACK、迁移、PMTUD、0-RTT、回调派发 |
| `include/go2cpp/bento/net/channel.hpp` / `src/bento/net/channel.cpp` | 860 / — | ReliableChannel / UnreliableChannel / VideoChannel / 重组器 / RTT 估计 |
| `include/go2cpp/bento/net/fec.hpp` / `src/bento/net/fec.cpp` | 191 / — | XOR 与 GF(256) Reed-Solomon 编解码 |
| `include/go2cpp/bento/net/congestion.hpp` / `src/bento/net/congestion.cpp` | 515 / — | BBRv1 子集 + NewReno 对照 + RTT/带宽估计 + pacer |
| `include/go2cpp/bento/net/peer_hub.hpp` / `src/bento/net/peer_hub.cpp` | 436 / — | 1:N 多 peer，按 conn_id 路由，per-peer 密钥与重放窗口 |
| `include/go2cpp/bento/net/udp_batch.hpp` / `src/bento/net/udp_batch.cpp` | 172 / — | recvmmsg/sendmmsg + SO_REUSEPORT 多队列 |
| `include/go2cpp/bento/net/crypto.hpp` / `x25519.hpp` / `keystore.hpp` | — | ChaCha20-Poly1305、X25519、PSK 加载与轮换 |
| `include/go2cpp/bento/net/{frame,heartbeat,backoff,relay,transport}.hpp` | — | 20B 通用帧、心跳、退避、双向中继、ITransport |
| `include/go2cpp/bento/net/{link_server,omni_server,http_server,ws_server,mqtt,coap,socks5}.hpp` | — | 基于 Link 的服务器/协议适配层 |
| `include/go2cpp/bento/app/{session,fetch,file_transfer,backup,zip,capability}.hpp` | — | 会话编排与文件类应用 |

## 6. 性能数据

> **口径提醒**：Bento 没有 `docs/reports/asm-*.md` 的汇编级审计；下列数字来自既有实测报告，
> 其中 netem 对比是 **2026-10-28 基线**，之后代码补了 SACK/快速重传/BBR/FEC，**尚未重跑该对比**。

弱网三方实测（`docs/reports/vs-network.md` §5，5 场景 × 3 传输 × 3 轮，每场景 200 条 × 256B）：

| 场景 | 传输 | 送达率 | p50 (ms) | p95 (ms) | 重传均值 |
|---|---|---:|---:|---:|---:|
| baseline | TCP / UDP / **Bento** | 100% | 0.02 / 0.01 / **1.00** | 0.06 / 0.03 / **2.00** | 0 / 0 / 0 |
| loss 5% delay 50 | TCP / UDP / **Bento** | 100 / 94.3 / **100%** | 193.84 / 50.15 / **272.33** | 193.96 / 50.18 / **527.67** | 0.7 / 0 / **105.3** |
| loss 20% delay 200 jitter 50 | TCP / UDP / **Bento** | 100 / 81.3 / **100%** | 1143.71 / 204.70 / **1842.67** | 1580.04 / 278.61 / **3441.67** | 4.0 / 0 / **142.0** |
| loss 40% delay 100 | TCP / UDP / **Bento** | 100 / 59.8 / **66.7%** | 2729.49 / 100.09 / **5944.50** | 2796.25 / 100.13 / **9533.00** | 4.7 / 0 / **397.0** |
| rate 1mbit loss 10% delay 100 | TCP / UDP / **Bento** | 100 / 87.8 / **100%** | 786.75 / 311.30 / **1372.00** | 786.81 / 497.03 / **1837.00** | 1.7 / 0 / **70.3** |

如实记录的三条差距（原文）：① baseline 下 Bento p50 1.00ms vs TCP 0.02ms，差在「500ms 固定收尾 +
ack_interval/PTO 参数」，不是数据面；② 5% 丢包重传 105 次 vs TCP 0.7 次，PTO 比内核激进；
③ 40% 丢包送达率 66.7% 的根因是**一轮握手 15s 超时**，其余两轮 200/200。
这三条已在后续提交中用「`handshake_copies=2`、16 轮握手、SACK 扩展、快速重传、BBR」针对性修改，
但**没有重跑同口径对比**，引用时必须带「2026-10-28 基线」限定。

其他已有数字：

| 项 | 数据 | 来源 |
|---|---|---|
| PeerHub 吞吐 | 3 peer × 50 STREAM + 50 DATAGRAM 隔离，307 KB/80ms ≈ **30.7 Mbit/s** | `docs/modules/bento-scenarios.md` §测试 |
| 加密吞吐 | ChaCha20-Poly1305 ≥ **50 MB/s** | `tests/bento/test_crypto.cpp` |
| 批量收包 | recvmmsg/sendmmsg 单核回环约 **2~3×**（相对于一包一 syscall） | `udp_batch.hpp` 注释 |
| 协议服务器单连接 | HTTP 7,695 req/s、WS 5,366 msg/s、MQTT 8,778 msg/s、CoAP 38,268 req/s | `docs/modules/bento-servers.md` |

## 7. 边界与已知限制

1. **`Link` 是单 peer**：监听方只接受第一个握手成功的来源；要 1:N 必须用 `PeerHub`（独立组件，
   不是把 Link 改成多 peer）。两条 UDP 代码路径并存是有意的，避免回归 2000+ 行实现。
2. **加密默认开启**：`require_encryption=true` 时无 32B PSK 的 `Open/Connect` 直接失败；
   老代码若忽略返回值仍然「能跑」，但所有发送都会被安全策略拒绝。
3. **SACK 扩展在加密会话降级**：PSK 加密握手的 HELLO body 长度固定、不带 caps，因此加密会话
   只用 32 位位图（已知限制，见 `include/.../link.hpp` 注释与 `audit-sack.md`）。
4. **FEC 默认关闭**：开启是「带宽换延迟」；`k+m <= 255`（GF(256)）；组内分片零填充到 max_len。
5. **BBR 是 v1 最小子集**：没有 ack aggregation、没有包级精确 round 判定、没有完整 inflight_hi
   探测；媒体层（`media/`）目前**没有接入** `congestion.hpp`，端到端码率闭环未做。
6. **单线程 `RunOnce` 是吞吐上限**：`rx_threads>0` 只摊收包系统调用，协议处理与回调仍是单线程；
   要再扩需要按 `conn_id`/通道分片到多线程（未做）。
7. **UDP 语义限制**：不保证送达/顺序（DATAGRAM 通道）、接收缓冲不足时内核丢弃超出部分且无法补读；
   connected socket 的 `ECONNREFUSED` 应视为对端不可达。
8. **PMTUD 默认关闭**：需要 DF + 探针；保守 MTU 1200，最大 1500，跨 IPv4/IPv6 不混用。
9. **0-RTT 仅纯 PSK**：X25519 会话不导出票据，避免前向保密被静默降级。
10. **协议版本 2 与旧 12B 线格式不兼容**：`PROTOCOL.md` 前 10 节描述的是 P0/P1 旧格式，以 §11 为准。

## 8. 测试与验证方法

```bash
# 配置（Bento 默认 OFF）
cmake -S . -B build-bento -G Ninja -DGO2CPP_WITH_BENTO=ON
cmake --build build-bento -j

# 全部 Bento 测试（ctest 名前缀 bento_）
ctest --test-dir build-bento -R '^bento_' --output-on-failure

# 单跑链路/通道
ctest --test-dir build-bento -R 'bento_(link_loopback|stream_fragment|reliable_channel)' -V
```

- `tests/bento/` 覆盖：protocol 编解码、link 回环/迁移/0-RTT、channel（SACK/重传/流控/分片）、
  fec、congestion、peer_hub（1 被控 3 主控/伪造 conn_id/吞吐/回调删除）、crypto/x25519/keystore、
  weaknet（用户态丢包/延迟代理）、netem（需要 `CAP_NET_ADMIN`，无权限时 `SKIP_RETURN_CODE=77`）。
- 弱网测试优先用 `tests/bento/udp_proxy.hpp`（用户态代理，不需要 root），netem 只在有权限时跑。
- 观测：`Link::StatsJson()` 一次拿到 frames/bytes/retransmits/cwnd/rtt/bandwidth/loss_rate/PMTUD/
  FEC/流控计数；PeerHub 用 `PeerHubStats`。

## 9. 交接注意

- **先读 `tests/bento/PROTOCOL.md` §11（P2 变更）再看代码**：帧头 16B/ver2、conn_id、PMTUD、
  FEC、0-RTT 都是 P2 加的；前 10 节是历史，不要按它改代码。
- **改线格式要同步四处**：`protocol.hpp` 编解码、`link.cpp` 发送/接收、`peer_hub.cpp` 路由、
  `PROTOCOL.md`；否则单测和旧端会同时坏。
- **回调线程语义**：所有 `on_*` 在 `RunOnce` 内、状态锁外执行；可以在回调里 `Send*`/`Close`，
  但不能阻塞。需要跨线程投递请用 `ConcurrentSend` 或自己的队列。
- **`Stats()` vs `stats()`**：`stats()` 是原始链路计数器（不加锁），并发场景用 `Stats()` 聚合快照；
  `LinkStats` 里 `cwnd/inflight/bandwidth` 是**多通道求和**，不是单通道值。
- **加密测试的坑**：默认 `require_encryption=true`，测试里要么两端都设 32B PSK，要么显式
  `require_encryption=false`（并接受明文）；PeerHub 的 per-peer PSK 仍需给 Hub 一个默认 PSK 占位。
- **不要直接把 `Link` 改成多 peer**：用 `PeerHub`；Link 的单 peer 假设散落在 2000 行实现里，
  改动会波及全部既有测试。
- **未完成事项**：媒体层接 BBR、加密会话 SACK 扩展、30%+ 丢包的公开压测、100MB 级长跑、
  `bento_netem` 在 CI 的权限方案。
