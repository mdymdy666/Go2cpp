# P2P 公网穿透（STUN / 打洞 / TURN / ICE）

> 模块路径：`include/go2cpp/p2p/`、`src/p2p/`、`tests/p2p/`
> 目标：在两端各自位于 NAT/防火墙之后时，建立一条**尽量直连、失败可降级中继**
> 的 UDP 数据通道；并提供可独立部署的 STUN/TURN 服务器。
>
> 本文档覆盖原理、设计、部署、与 IM 信令的接口、测试数据与限制。

---

## 1. 能力总览

| 能力 | 实现 | 位置 |
|------|------|------|
| STUN Binding（RFC 5389 子集） | Binding Request/Response、XOR-MAPPED-ADDRESS、MAPPED-ADDRESS、ERROR-CODE、SOFTWARE、CHANGE-REQUEST | `stun_message.hpp` / `stun_client.hpp` |
| STUN echo 服务器 | 单线程 poll、最多 4 个 socket（支持双 IP/双端口 CHANGE-REQUEST） | `stun_server.hpp` |
| NAT 类型检测 | RFC 3489 四类：Full Cone / Restricted / Port Restricted / Symmetric | `stun_client.hpp`（`NatTypeDetector`） |
| UDP 打洞 | 双方同时向对端候选发 STUN Binding 检查，重试 + 超时 | `p2p_channel.cpp` |
| TCP 打洞（加分） | TCP simultaneous open（bind 后双方同时 connect） | `tcp_punch.hpp` |
| ICE 简化 | host/srflx/prflx/relay 候选、优先级排序、并行检查、第一个成功 | `candidate.hpp` / `p2p_channel.cpp` |
| TURN 简化 | 自定义中继协议：Allocation / Send / Data / Refresh / Release，服务器数据面转发 + 统计 | `turn.hpp` / `relay_protocol.hpp` |
| 弱网适配 | keepalive 保活 + RTT、序列号缺口统计、对端静默超时、控制包重传 | `p2p_channel.cpp` |
| 数据通道 | 12 字节帧头（magic/version/type/length/seq），单包 ≤1200B | `p2p_channel.cpp` |

零外部依赖：只使用 libc、pthread 与 Linux socket API。**不链接**
`go2cpp_runtime`，因此不会被 hook 的 syscall 拦截影响，也可以被独立工具直接链接。

---

## 2. 原理

### 2.1 NAT 的两种行为

NAT 对出站连接做两件事，打洞能否成功由它们共同决定：

1. **映射行为（mapping）**：内部 `(ip, port)` 映射成外部 `(ip', port')`。
   - Endpoint-Independent：无论目标是谁，都复用同一个外部端口（锥形 NAT）。
   - Address/Port-Dependent：每个目标分配不同的外部端口（对称 NAT）。
2. **过滤行为（filtering）**：外部包能否进入。
   - Endpoint-Independent：任何外部地址都能进来（Full Cone）。
   - Address-Dependent：只允许「内部曾发过包的 IP」（Restricted）。
   - Address-Port-Dependent：只允许「内部曾发过包的 IP:port」（Port Restricted）。

| 类型 | 映射 | 过滤 | 打洞 |
|------|------|------|------|
| Full Cone | 端点无关 | 不限制 | 容易 |
| Restricted Cone | 端点无关 | 限制 IP | 双方同时发包可成功 |
| Port Restricted Cone | 端点无关 | 限制 IP:port | 双方同时发包可成功 |
| Symmetric | 端点/端口相关 | 限制 IP:port | **基本不可行**，需中继 |

### 2.2 STUN

STUN 解决两个问题：告诉客户端「你在公网看起来是什么地址」，以及探测 NAT 行为。

**消息格式**（20 字节头 + TLV 属性，4 字节对齐）：

```
 0                   1                   2                   3
+-------------------+-------------------+-------------------+-------------------+
| type (2)          | length (2)        | magic cookie 0x2112A442 (4)       |
+-------------------+-------------------+-------------------+-------------------+
| transaction id (12 字节)                                                  |
+-------------------+-------------------+-------------------+-------------------+
| attributes: type(2) + length(2) + value(length) + padding                 |
```

`type` 的 method/class 按 RFC 5389 §6 交错编码（`EncodeStunType()`）：

```
Binding Request  = 0x0001
Binding Success  = 0x0101
Binding Error    = 0x0111
```

**XOR-MAPPED-ADDRESS**：IPv4 的地址与 magic cookie 异或、端口与 `0x2112`
异或；IPv6 的地址还要再与 transaction id 异或。这样中间设备不会把地址写进
缓存/日志，也避免某些 NAT 改写明文地址属性。

**NAT 类型检测**（RFC 3489 §10.1，本实现顺序严格一致）：

1. **Test I**：向主服务器发普通 Binding，得到映射 `M1`；
   若 `M1` 等于本机地址 → `OpenInternet`（没有 NAT）。
2. **Test II**：向主服务器发 `CHANGE-REQUEST(change IP + change port)`；
   服务器从**另一个 IP、另一个端口**回应。收到 → `FullCone`。
3. **Test III**：`CHANGE-REQUEST(change port)`；服务器从同 IP、另一端口回应。
   收到 → `RestrictedCone`（NAT 只限制来源 IP，不限制端口）。
4. **Test IV**：向备用服务器（另一个 IP）发普通 Binding，得到 `M2`；
   `M1 != M2` → `Symmetric`。
5. 以上都不成立 → `PortRestrictedCone`。

> **顺序很重要**：Test IV 会让 NAT 把备用服务器地址记入「已通信」过滤表，
> 如果先做 Test IV，再做 Test II/III，就会收到本应被 Port Restricted NAT
> 丢弃的响应，误判成 Full Cone。本实现把 Test IV 放在最后，并在测试中
> 用四种模拟 NAT 验证（见 §6）。

**双 IP 依赖**：Full Cone 与 Restricted 的区分需要两个**不同 IP** 的服务器
地址。本机可用 `127.0.0.1` + `127.0.0.2`（Linux 整个 `127/8` 都在 lo 上）
完整验证；公网部署需要至少两块网卡或两个公网 IP。只给单 IP 时检测器会
返回 `Unknown` 并在 `detail` 中说明信息不足，不会猜测。

### 2.3 UDP 打洞

```
        NAT A                 STUN/TURN                 NAT B
          |                       |                       |
   A ---- | --- Binding --------->|                       |
          |<-- MAPPED(A') --------|                       |
          |                       |<---- Binding -------- | ---- B
          |                       |------ MAPPED(B') ---> |
          |                       |                       |
          |   （通过 IM/信令交换 A' 与 B'）                |
          |                       |                       |
   A ---- | ==== Binding check ===|=====================> | ---- B
          |<=== Binding check =========================== | ---- A
          |   （双方同时发包，NAT 才会为对方 IP:port 放行）  |
```

关键点：

- **必须双方同时发包**。Port Restricted NAT 只放行「内部曾发过包的目标」；
  如果只有 A 先发，A 的包会被 B 的 NAT 丢弃，B 就永远不知道该向谁回。
- 打洞包用 **STUN Binding Request**，对端回 Success。收到对方请求或匹配
  事务 ID 的响应都视为连通（这样双方无论谁先到都能成功）。
- 检查包每 100ms 重发一轮，直到连接成功或超时。
- 连接成功后锁定对端地址，后续数据帧/心跳都发往该地址。

### 2.4 TCP 同时打开（可选）

双方各自 `bind` 一个本地端口，然后**同时** `connect` 对方端口。两个 SYN
交叉后双方进入 `ESTABLISHED`，无需任何人监听。

限制：

- 需要双方几乎同时发起；本实现每 5ms 重建 socket 重试，但没有精确时钟同步。
- 对称 NAT（对 TCP 也按目标分配端口）必然失败。
- 内核支持差异：Linux 支持，Windows/macOS 行为不同。
- 因此它是「加分能力」，`P2PChannel` 默认不启用，由使用方显式调用。

### 2.5 TURN 简化

完整 RFC 5766 需要 long-term credential、MESSAGE-INTEGRITY、ChannelData
绑定、权限表与配额，代码量是本模块的数倍。本实现保留 TURN 的三段式语义，
换成固定 16 字节头 + 紧凑 body 的自定义协议：

```
+-------------------+-------------------+-------------------+-------------------+
| magic "G2TR" (4)  | version (1)       | type (1)          | body len (2)      |
+-------------------+-------------------+-------------------+-------------------+
| request_id (4)    | reserved (4)                                            |
+-------------------+-------------------+-------------------+-------------------+
| body: [family(1) + port(2) + address(4/16) + payload]                      |
```

消息类型：`AllocateRequest/Response`、`SendIndication`、`DataIndication`、
`RefreshRequest/Response`、`ReleaseRequest/Response`、`ErrorResponse`。

服务器行为：

- 客户端 `Allocate` → 服务器创建一个 relay socket（绑定在同一 IP 的临时端口），
  返回 `relay 地址 + 生命周期`；
- 客户端 `SendIndication(target, payload)` → 服务器从该 relay socket 转发；
- relay socket 收到外部包 → 包装成 `DataIndication(source, payload)` 发给客户端；
- `Refresh` 续租，`Release` 释放；超过生命周期自动回收；
- 统计：allocation 数、双向包数/字节数、丢弃数。

> 权限：本实现**不校验** peer 权限，任何知道 relay 地址的人都能发包。
> 部署时必须把中继放在可信内网或用防火墙限制来源（见 §7）。

### 2.6 ICE 简化

完整 ICE（RFC 8445）包含 controlling/controlled 角色、nomination、prflx
候选发现、Trickle ICE、候选对状态机等。本实现保留对打洞最关键的部分：

1. **候选收集**：host（本机网卡）、srflx（STUN 映射）、relay（TURN 分配）；
2. **优先级**：`(type_pref << 24) | (local_pref << 8) | (256 - component)`，
   其中 host=126、prflx=110、srflx=100、relay=0，因此排序天然是
   **host > prflx > srflx > relay**；
3. **并行检查**：对所有候选对轮转发送 STUN Binding，**第一个成功**即锁定；
4. **降级**：直连阶段（70% 预算）失败后，用剩余的 30% 走 relay 候选。

与完整 ICE 的差异（见 §7）：没有角色/nomination、没有 MESSAGE-INTEGRITY、
没有 Trickle、prflx 只作为兜底不主动收集。

---

## 3. 设计

### 3.1 文件结构

| 文件 | 职责 |
|------|------|
| `endpoint.hpp/cpp` | IPv4/IPv6 端点值类型、解析、比较、哈希 |
| `transport.hpp/cpp` | `DatagramTransport` 抽象 + `UdpTransport`（poll/recvfrom）+ 网卡枚举 |
| `random.hpp/cpp` | getrandom → /dev/urandom → 兜底随机数 |
| `stun_message.hpp/cpp` | STUN 消息编解码、地址属性、ERROR-CODE |
| `stun_client.hpp/cpp` | StunClient + NatTypeDetector |
| `stun_server.hpp/cpp` | 可独立部署的 STUN echo 服务器（4 socket / CHANGE-REQUEST） |
| `candidate.hpp/cpp` | 候选类型、优先级、文本序列化 |
| `signaling.hpp/cpp` | `Signaling` 接口 + `LoopbackSignaling`（进程内测试） |
| `relay_protocol.hpp/cpp` | 简化 TURN 线格式（内部头，不对外导出） |
| `turn_server.hpp/cpp` | 中继服务器：allocation 表 + poll 数据面 + 统计 |
| `turn_client.hpp/cpp` | 中继客户端：Allocate/Refresh/Release/Send/Recv |
| `p2p_channel.hpp/cpp` | 对外唯一入口：候选收集/打洞/降级/数据面/统计 |
| `tcp_punch.hpp/cpp` | TCP 同时打开 |

### 3.2 对外接口

```cpp
class P2PChannel {
 public:
  explicit P2PChannel(P2PConfig config);

  std::vector<Candidate> GatherCandidates();     // host + srflx + relay
  void AddRemoteCandidate(const Candidate&);
  bool Connect(std::chrono::milliseconds timeout);
  bool Send(const void* data, size_t len);
  size_t Recv(void* buf, size_t cap, std::chrono::milliseconds timeout);
  bool IsRelayed();

  // 扩展（诊断/测试）
  bool IsConnected() const;
  P2PState state() const;
  Endpoint LocalEndpoint() const;
  Endpoint PeerEndpoint() const;
  std::vector<Candidate> LocalCandidates() const;
  std::vector<Candidate> RemoteCandidates() const;
  P2PStats Stats() const;
  std::string LastError() const;
  void Close();
};
```

`P2PConfig` 关键字段：

| 字段 | 默认 | 说明 |
|------|------|------|
| `bind_endpoint` | `0.0.0.0:0` | 本地绑定；绑定具体 IP 时只收集该地址的 host 候选 |
| `stun_servers` | 空 | 空表示只做局域网直连 |
| `turn_server` | 无效 | 无效表示不启用中继降级 |
| `signaling` | `nullptr` | 为空时需手动 `AddRemoteCandidate` |
| `transport` | 空 | 测试注入（模拟 NAT）；空则内部创建 `UdpTransport` |
| `connect_timeout` | 5s | `Connect(0ms)` 时使用 |
| `check_interval` | 100ms | 打洞检查重发间隔 |
| `keepalive_interval` | 15s | 0 关闭；保活 NAT 映射 + 测 RTT |
| `peer_timeout` | 30s | 连接后静默超时 |
| `enable_relay` | true | 打洞失败是否降级 |
| `auto_exchange_candidates` | true | `Connect()` 内自动通过 signaling 交换 |
| `max_local_candidates` | 16 | 候选上限 |

### 3.3 状态机

```
        GatherCandidates()
  Idle -------------------> Idle（候选已就绪）
    |
    | Connect()
    v
Connecting --直连检查成功--> Connected
    |                          |
    | 直连失败 + relay 成功     | 收到中继检查成功
    +------------------------> Relayed
    |
    | 全部失败
    v
 Failed
```

`Close()` 可从任意状态进入 `Closed`，并释放中继 allocation。
`Send/Recv` 只在 `Connected`/`Relayed` 下工作。

### 3.4 数据帧

```
 0                   1                   2                   3
+-------------------+-------------------+-------------------+-------------------+
| magic "G2PD" (4)  | version (1)       | type (1)          | length (2)        |
+-------------------+-------------------+-------------------+-------------------+
| sequence (4)      | payload (length 字节，≤1200)                              |
```

- `type`：1=data、2=keepalive、3=keepalive-ack；
- `length` 是 payload 长度，必须与数据报剩余长度严格相等（防止截断/拼接）；
- 单包上限 `kP2PMaxPayload = 1200`，避免 1500 MTU 下的 IP 分片；
- 更大的消息由上层分片（例如复用 Bento 的可靠层）。

接收路径按顺序处理：STUN 检查包 → 数据帧 → keepalive/ack；源地址必须
等于已锁定的对端地址，否则计入 `dropped_packets` 并丢弃。

### 3.5 超时与重试

| 阶段 | 策略 |
|------|------|
| STUN Binding（收集） | 单次请求 `gather_timeout`（默认 800ms），失败跳过该服务器 |
| 信令交换 | 总预算的 50%，双方都先 Send 再 Recv，不会死锁 |
| 直连检查 | 有 relay 候选时用 70% 预算，否则用满 100%；每 `check_interval` 重发所有候选对 |
| 中继检查 | 剩余预算；Allocate 失败重试 `allocate_attempts` 次 |
| TURN 请求 | `request_timeout`（默认 500ms）× `allocate_attempts`（默认 5） |
| 已连接 | keepalive 每 15s；超过 `peer_timeout` 无任何有效包 → 标记 `Failed` |

### 3.6 线程模型

- `GatherCandidates()` / `Connect()` 是同步阻塞调用，不应并发调用；
- `Send()` 与 `Recv()` 可以在**不同线程**并发（一个发、一个收）；
- `Stats()` / `IsRelayed()` / `LastError()` 线程安全；
- 内部没有后台线程：keepalive 由 `Recv()` 驱动（`Recv` 每次进入时检查是否该
  发心跳）。只发不收的场景需要周期性调用 `Recv`（可用 0 超时轮询），
  否则 NAT 映射可能过期；
- `StunServer` / `RelayServer` 各自有一个后台 poll 线程，`Stop()` 会 join。

### 3.7 与 IM 的信令接口

`Signaling` 只有两个方法：

```cpp
class Signaling {
 public:
  virtual bool Send(const std::string& payload) = 0;
  virtual int Recv(std::string* payload, std::chrono::milliseconds timeout) = 0;
};
```

候选文本格式（可直接作为 IM 消息体）：

```
g2p2-candidates/1
cand host 2130706431 192.168.1.5:5000 192.168.1.5:5000 3f2a1c9b
cand srflx 1694498815 203.0.113.7:62000 192.168.1.5:5000 91d0e4aa
cand relay 16777215 198.51.100.9:49152 198.51.100.9:3478 0b7c22d1
```

字段：类型、优先级、候选地址、base 地址、foundation。

接入 IM 的示例（伪代码）：

```cpp
class ImSignaling : public go2cpp::p2p::Signaling {
 public:
  bool Send(const std::string& payload) override {
    return im_client_->SendText(peer_user_id_, payload);   // 走 IM 私聊
  }
  int Recv(std::string* payload, std::chrono::milliseconds timeout) override {
    return im_client_->PollMessage(peer_user_id_, payload, timeout);
  }
};
```

时序：

```
A                      IM 服务器                      B
| -- Connect() -----------> |                          |
| -- Send(候选 A) --------> | -- 推送 -->              |
|                          |            -- Send(候选 B) |
| <-- 推送 ---------------- | <-------                  |
| ========== 同时向对方 srflx 打洞 ==================== |
```

生产注意事项：

- IM 消息可能乱序/重复/丢失：信令层要保证**至少一次**投递，收到候选后去重
  （`AddRemoteCandidate` 内部按 `(type, endpoint)` 去重）；
- 双方 `Connect()` 必须在几秒内先后发生；间隔太久时对端可能已经超时；
- 不要在信令里传鉴权凭据（本模块的 STUN/TURN 不接受凭据）。

---

## 4. 部署

### 4.1 STUN 服务器

```cpp
#include "go2cpp/p2p/stun_server.hpp"

go2cpp::p2p::StunServerConfig config;
config.primary = *go2cpp::p2p::Endpoint::Parse("0.0.0.0:3478");
config.advertised_address = *go2cpp::p2p::Endpoint::Parse("203.0.113.10:0");
config.alternate = *go2cpp::p2p::Endpoint::Parse("203.0.113.11:3479");
go2cpp::p2p::StunServer server(config);
std::string error;
if (!server.Start(&error)) {
    // 端口被占用/权限不足
}
// server.primary_endpoint() / server.alternate_endpoint()
```

- 只做地址发现：单 IP 单端口即可（`alternate` 留空）；
- 要做完整四类 NAT 检测：需要两个 IP（`primary` 与 `alternate` 的 IP 不同），
  防火墙放行 UDP 3478（主端口）与备用端口；
- 服务器不鉴权、不校验 MESSAGE-INTEGRITY，只回应 Binding，不承载用户数据。

### 4.2 中继服务器

```cpp
go2cpp::p2p::RelayServerConfig config;
config.bind_endpoint = *go2cpp::p2p::Endpoint::Parse("0.0.0.0:3478");
config.advertised_address = *go2cpp::p2p::Endpoint::Parse("203.0.113.10:0");
config.allocation_lifetime = std::chrono::seconds(120);
config.max_allocations = 4096;
go2cpp::p2p::RelayServer relay(config);
relay.Start();
// relay.stats()：allocation 数、双向包/字节、丢弃数
```

容量估算：

- 单线程 poll 循环，实测 loopback 稳态约 4400 pps / 2.2 MB/s（512B 包，
  发送端限速后 100% 到达）；公网还受网卡与带宽限制；
- 每个 allocation 占一个 UDP socket 与约 2MB 内核接收缓冲（受
  `net.core.rmem_max` 限制，默认 208KB，建议 `sysctl -w net.core.rmem_max=4194304`）；
- 生产建议：多个中继进程 + 四层负载均衡，或用 `SO_REUSEPORT` 横向扩展。

### 4.3 客户端

```cpp
go2cpp::p2p::P2PConfig config;
config.bind_endpoint = go2cpp::p2p::Endpoint::Wildcard(0);
config.stun_servers = {*go2cpp::p2p::Endpoint::Parse("stun.example.com:3478")};
config.turn_server = *go2cpp::p2p::Endpoint::Parse("turn.example.com:3478");
config.signaling = &im_signaling;
config.connect_timeout = std::chrono::seconds(5);

go2cpp::p2p::P2PChannel channel(config);
channel.GatherCandidates();
if (!channel.Connect(std::chrono::seconds(5))) {
    // 对称 NAT + 没有 TURN，或防火墙封了 UDP
}
```

### 4.4 弱网参数建议

| 场景 | keepalive | peer_timeout | check_interval | 说明 |
|------|-----------|--------------|----------------|------|
| 移动网络 | 5s | 15s | 100ms | 频繁换网，保活要密 |
| 家用宽带 | 15s | 30s | 100ms | 默认 |
| 服务器间 | 30s | 60s | 50ms | 稳定链路 |
| 高丢包 | 3s | 10s | 150ms | 重发更频繁，但会增加带宽 |

本模块**没有**重传/拥塞控制/FEC：数据面是纯 UDP。需要可靠传输时，把
`P2PChannel` 当作一条 UDP 链路，交给上层的可靠层（例如 Bento 的
reliable channel）处理重传、拥塞控制与分片。

---

## 5. 测试与验收

### 5.1 构建与运行

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake -S . -B .build-bento-wsl            # GO2CPP_WITH_P2P / GO2CPP_P2P_TESTS 默认 ON
cmake --build .build-bento-wsl -j
cd .build-bento-wsl
ctest -R p2p --output-on-failure
# 或直接运行
./p2p_test_stun
./p2p_test_p2p
./p2p_test_tcp_punch
./p2p_test_p2p_interprocess
./p2p_bench                                 # 基准，不注册 ctest
```

### 5.2 场景覆盖（≥10）

| # | 场景 | 断言/结果 |
|---|------|-----------|
| 1 | STUN 消息编解码往返（IPv4/IPv6/CHANGE-REQUEST/字符串） | 属性长度、事务 ID、地址一致 |
| 2 | 非法/截断消息拒绝（每个前缀、坏 magic、长度篡改、尾部垃圾） | 全部拒绝 |
| 3 | ERROR-CODE 编解码 | 420 + 原因短语 |
| 4 | 本地 STUN echo 映射 | 映射 == 客户端实际地址；XOR/MAPPED 双属性 |
| 5 | CHANGE-REQUEST(port) / (ip+port) | 响应源地址与端口符合预期 |
| 6 | 无 NAT 检测 | OpenInternet |
| 7 | Full Cone 模拟 NAT | FullCone |
| 8 | Restricted 模拟 NAT | RestrictedCone |
| 9 | Port Restricted 模拟 NAT | PortRestrictedCone |
| 10 | Symmetric 模拟 NAT | Symmetric |
| 11 | 候选优先级与文本序列化 | host > srflx > relay；往返一致；非法输入整体失败 |
| 12 | 局域网 host 直连 | 双向数据；PeerEndpoint 正确；IsRelayed=false |
| 13 | Full Cone NAT 打洞 | 成功，双向数据 |
| 14 | Port Restricted NAT 打洞 | 双方同时发包，成功 |
| 15 | Symmetric NAT 打洞失败 → 中继 | IsRelayed=true，双向数据，服务器转发统计 |
| 16 | 纯中继（无 STUN） | 成功，relayed_packets 计数 |
| 17 | 连接超时（黑洞候选） | false，300ms ± 余量，LastError 非空 |
| 18 | keepalive / RTT / 统计 | 双向心跳、rtt>0、序列号缺口 0 |
| 19 | 多候选（黑洞 + 真实） | 并行检查后选真实候选 |
| 20 | 500 条随机长度数据 | 全部完整 |
| 21 | 2000 包吞吐/延迟 | 到达率 100%、P50/P95/P99 |
| 22 | TCP 同时打开 | 成功（本机 Linux；失败记 SKIP） |
| 23 | 跨进程 host 直连（fork+exec 两进程） | 双向请求-响应 |
| 24 | 跨进程 STUN srflx 打洞（两进程） | 双向请求-响应 |

### 5.3 实测数据

环境：WSL2 Ubuntu，Release，loopback，单核调度（非公网，仅用于回归对比）。

| 指标 | 结果 |
|------|------|
| STUN Binding（1000 次） | 1000/1000 成功，≈10000 req/s，P50=0.09ms，P95=0.16ms，P99=0.20ms |
| 直连 1KB × 20000 包 | 100% 到达，≈190–210MB/s，P50≈0.02ms，P95≈0.10ms，P99≈0.13ms |
| 直连 RTT（1000 轮） | P50=0.010ms，P95=0.011ms，P99=0.018ms |
| 中继 512B × 5000 包（10k pps 限速） | 100% 到达，≈2.24MB/s，P50=0.12ms，P95=0.21ms，P99=0.26ms |
| 中继 RTT（200 轮） | P50=0.21ms，P95=0.38ms，P99=0.46ms |
| 测试断言总数 | 约 2030（STUN）+ 94（P2P）+ 1（TCP）+ 4（跨进程），0 失败 |

> 中继吞吐为什么明显低于直连：数据要经过「客户端 → 服务器 relay socket →
> 服务器 listen socket → 客户端」两次用户态转发，且单线程 poll。瞬时洪泛
> 会打满 `net.core.rmem_max`（默认 208KB）导致内核丢包；基准按 10k pps
> 限速测的是可持续吞吐。

---

## 6. 限制与安全边界

1. **对称 NAT 不可打洞**。每个目标分配不同外部端口，双方从 STUN 得到的
   映射端口对打洞包无效；只能走 TURN。本模块会自动降级，但延迟更高、
   占用服务器带宽。
2. **UDP 完全封锁**时打洞与 TURN 都不可用；TCP 同时打开只在部分 NAT/防火墙
   组合下有效，且平台支持不一致。企业网可能需要 HTTP 代理/WebSocket 隧道
   （不在本模块范围）。
3. **STUN 无鉴权**：任何能访问服务器的人都能查询映射地址。生产环境应限制
   来源网段；不要把它当作可信身份来源。
4. **TURN 无鉴权、无权限表**：知道 relay 地址的人都能发包，也能消耗服务器
   带宽。只能部署在可信内网，或用防火墙 + 配额限制。需要公网多租户时，
   应换成完整 TURN（RFC 5766）或自建鉴权层。
5. **ICE 简化**：没有 controlling/controlled 角色与 nomination，理论上第三方
   若能在正确时机向 socket 发合法 Binding 检查，可能插入连接；实际利用需要
   猜测事务 ID 或抢先发包。对安全敏感的场景应在数据面加端到端加密与身份
   校验（本模块不提供加密）。
6. **无可靠传输**：不重传数据、不做拥塞控制、无 FEC。弱网下建议把
   `P2PChannel` 作为链路层，交给上层可靠协议。
7. **单路径**：连接成功后只使用一个 peer 地址；对端换网/端口变化会导致
   连接失效（需要重新 `Connect`）。
8. **IPv6**：STUN/端点/候选支持 IPv6；`TcpPuncher` 只支持 IPv4；
   `MockNat`（测试）只支持 IPv4。IPv6 下通常没有 NAT，host 候选直连即可。
9. **STUN 服务器双 IP**：完整四类检测需要两个公网 IP；单 IP 时检测结果为
   `Unknown`（不会瞎猜）。
10. **没有实现 MESSAGE-INTEGRITY/FINGERPRINT**：STUN 响应不做完整性校验，
    可被中间人篡改映射地址（导致打洞失败或流量被引导）。需要防篡改时应在
    信令层对候选做签名。

---

## 7. 与现有模块的边界

- 不依赖 `go2cpp::network` / `go2cpp_runtime`，因此不会被 hook 拦截；
- 不与 Bento 的 UDP 链路层共享 socket/协议：Bento 负责可靠传输，本模块负责
  「如何打通网络」；
- 不修改任何现有模块；只新增 `include/go2cpp/p2p/**`、`src/p2p/**`、
  `tests/p2p/**`、`cmake/Go2CppP2P.cmake` 与本文档。

---

## 8. 变更记录

### 2026-10-31：初版（P2P 公网穿透）

- 新增 STUN 消息编解码、客户端、NAT 类型检测、echo 服务器（双 IP/4 socket）。
- 新增 ICE 候选、优先级、文本信令格式与 `Signaling` 接口。
- 新增简化 TURN：自定义线格式、中继服务器（allocation + 数据面 + 统计）、
  客户端（Allocate/Refresh/Release/Send/Recv）。
- 新增 `P2PChannel`：候选收集、UDP 打洞、直连失败降级中继、数据帧、
  keepalive/RTT/序列号缺口统计、超时。
- 新增 TCP 同时打开（`TcpPuncher`）。
- 新增 24 个测试场景（单进程/跨进程/四种模拟 NAT/压测/随机/fuzz）。
- 已知限制见 §6；未 commit。
