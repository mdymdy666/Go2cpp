# bento-session 模块组：配对、会话表、控制权仲裁、多 peer 与会话编排

> **状态**：现行（2026-10-28）。
> **基线**：`afc135a`（`wsl bash -lc "cd /UserData/CodexWorkSpace/Go2cpp && git rev-parse --short HEAD"`）。
> **源码**：头文件 1698 行 + 实现 4589 行，合计 6287 行（`wc -l` 实测）。
> **头文件**：`include/go2cpp/bento/net/{pairing,session_table,session_control,peer_hub}.hpp`、`include/go2cpp/bento/app/session.hpp`。
> **实现**：`src/bento/net/{pairing,session_table,session_control,peer_hub}.cpp`、`src/bento/app/session.cpp`。
> **引用约定**：`path:line` 指向上述基线；签名逐字取自头文件，保留默认值。
> **测试**：`tests/bento/test_{pairing,session_table,session_control,peer_hub,peer_hub_secure,session}.cpp`，运行 `ctest -R '^bento_'`。

## 1. 模块清单

| 模块 | 命名空间 | 头文件 | 实现 | 测试 | 职责 |
|---|---|---|---|---|---|
| pairing | `go2cpp::bento` | `pairing.hpp`（360 行） | `pairing.cpp`（707 行） | `test_pairing.cpp` | 32B 配对帧编解码、密钥指纹、单次迁移状态机、15s 超时重传 |
| session_table | `go2cpp::bento` | `session_table.hpp`（209 行） | `session_table.cpp`（264 行） | `test_session_table.cpp` | `key → 1 server + N client` 的 1:N 表、90s 租约、容量上限 |
| session_control | `go2cpp::bento` | `session_control.hpp`（342 行） | `session_control.cpp`（543 行） | `test_session_control.cpp` | View/Interact 权限、唯一控制器、FirstCome/Preempt、租约回收、审计 |
| peer_hub | `go2cpp::bento` | `peer_hub.hpp`（375 行） | `peer_hub.cpp`（1842 行） | `test_peer_hub.cpp`、`test_peer_hub_secure.cpp` | 单 UDP socket 承载 N peer、conn_id 路由、HELLO 重放防护、PSK |
| app::session | `go2cpp::bento::app` | `app/session.hpp`（412 行） | `app/session.cpp`（1233 行） | `test_session.cpp` | MOD-39：端点→传输→配对→通道→控制权→心跳→优雅关闭 |

## 2. 分层与调用关系

```
应用（JinShan Host/Viewer 或工具）
  │ 持有 SessionTable / PeerHub；构造并驱动 Session
  ▼
app::Session（MOD-39，单线程非阻塞）
  ├─ 拥有 PairingSession  → 只回答「这一对是否可信」
  ├─ 拥有 SessionControl  → 只回答「谁能操作」
  ├─ 拥有 ITransport      → TcpTransport / MemoryTransport（字节流）
  └─ 回调 on_state / on_data / on_error / on_control
SessionTable（应用持有）→ 只回答「这个 key 现在有哪些会话」
PeerHub（应用持有）    → 只回答「这帧属于哪个 peer」
```

- **pairing** 不碰 socket，只输入输出 `PairingFrame`；`app::Session` 把配对帧装进会话帧 `0x8001`（`session.cpp:553-567`）。
- **session_table** 不参与认证，也不被 `app::Session` 直接使用：Host 启动时 `AddServer`、Viewer 配对成功后 `AddClient`、心跳 `Touch`，由应用接线。
- **session_control** 是纯状态机，不发网络消息；`Session` 把 `on_change` 转发为 `on_control`，并在收到 `Control`/`ControlRequest` 帧时调用它（`session.cpp:612-764`）。
- **peer_hub** 复用 Link 的下层构件（`UdpSocket`/`ReliableChannel`/`UnreliableChannel`），不依赖 pairing/session_control；断线需一行 lambda 适配：`hub.on_peer_disconnected = [&](std::uint32_t id, const char*) { control.OnPeerDisconnected(id); };`（`session_control.hpp:24-27`）。
- **所有权**：`Session` 用 `unique_ptr` 独占配对、控制与传输；`SessionTable`、`PeerHub` 由应用持有，生命周期长于会话。

## 3. API 参考

### 3.1 pairing（`go2cpp::bento`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `inline constexpr std::size_t kPairingFrameBytes = 32;` | 定长 32B 单包帧，无分片 | 同时有 `kPairingMagic = 0x42504152`、`kPairingTimeoutMs = 15000` |
| `enum class PairingFrameType : std::uint8_t` | `kRegister=0x01`、`kConnect=0x02`、`kResponse=0x03`、`kConfirm=0x04` | Register 是本地 API，通常不上线 |
| `enum class PairingError : std::uint8_t` | 12 个失败码：`kNone=0` 到 `kClosed=11`（含 `kKeyMismatch=1`、`kPeerTimeout=2`、`kAlreadyRegistered=3`、`kNotRegistered=4`、`kAlreadyPaired=5`、`kInvalidState=6`、`kInvalidArgument=7`、`kProtocolVersion=8`、`kMalformedFrame=9`、`kRejected=10`） | 数值即 RESPONSE 帧的 `status` |
| `struct PairingFrame` | `type`、`version=kPairingVersion`、`key_hash`、`nonce`、`pair_id`、`status` 六个字段 | 纯值类型，与传输层无关 |
| `void EncodePairingFrame(const PairingFrame& frame, std::uint8_t* out) noexcept;` | 写 32B 大端（magic/version/type/flags/key_hash/nonce/pair_id/status/reserved） | `out` 至少 32B |
| `bool DecodePairingFrame(const std::uint8_t* data, std::size_t size, PairingFrame* out) noexcept;` | magic/version/type/flags/保留位/status 任一非法返回 `false` | 允许尾部多余字节，为扩展留位 |
| `virtual bool HashKey(const std::string& key, std::uint64_t* out) const = 0;` | 计算密钥指纹；空 key 返回 `false` | `PairingAuthPolicy` 接缝；默认 `FixedKeyAuthPolicy`（FNV-1a 64，非密码学） |
| `PairingError Register(const std::string& key, std::uint64_t now_ms);` / `PairingError Connect(const std::string& key, std::uint64_t now_ms);` | 服务端登记 / 客户端入队 CONNECT，成功进入 Pending | 重复调用返回 `kAlreadyRegistered`，空 key 返回 `kInvalidArgument` |
| `PairingError Response(bool accept, std::uint64_t now_ms);` | `accept=true` 排队 RESPONSE 并进 Paired；`false` 回 Pending 并回 `kRejected` | 无挂起 CONNECT 或角色不符返回 `kInvalidState` |
| `PairingError Activate(std::uint64_t now_ms);` | 客户端发 CONFIRM，仍留 Paired 直到收到服务端 ACK | 服务端调用返回 `kInvalidState` |
| `PairingError HandleFrame(const PairingFrame& frame, std::uint64_t now_ms);` | 处理一帧；幂等忽略返回 `kNone` | 密钥不匹配时同时排队 `RESPONSE(kKeyMismatch)` 再返回该错误 |
| `bool Tick(std::uint64_t now_ms);` | 超时→Closed+`kPeerTimeout` 并清空队列；否则按 `retransmit_ms` 重发 | 返回状态是否改变；重传本身返回 `false` |
| `bool PopOutbound(PairingFrame* out);` / `bool Close();` / `void Reset();` | FIFO 取帧 / 主动关闭（幂等）/ 回 Idle（统计保留） | 发送队列上限 `kPairingMaxOutbound = 64`，满时丢最旧帧 |
| `std::function<void(PairingState from, PairingState to, PairingError error)> on_state_change;` | 状态变更回调，在修改方法返回前同步调用 | 回调内不得再调用修改方法 |

### 3.2 session_table（`go2cpp::bento`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `SessionStatus AddServer(const std::string& key, std::uint64_t server_id, std::uint64_t now_ms = NowMs());` | 注册 Host；成功 `kOk` | 重复 key 返回 `kAlreadyExists`，key 数满返回 `kKeyLimit` |
| `SessionStatus AddClient(const std::string& key, std::uint64_t client_id, std::uint64_t now_ms = NowMs());` | 为已存在 key 加 Viewer | `kNotFound`/`kAlreadyExists`/`kFull`/`kInvalidArgument` |
| `std::optional<SessionEntry> Find(const std::string& key, std::uint64_t now_ms = NowMs()) const;` | 返回条目副本，不存在或已过期 `nullopt` | 纯只读：不续租、不清理 |
| `bool Touch(const std::string& key, std::uint64_t now_ms = NowMs());` | 心跳续租，刷新 `lease_deadline_ms` | 条目不存在/已过期返回 `false` |
| `bool Remove(const std::string& key, std::uint64_t now_ms = NowMs());` / `SessionStatus RemoveClient(const std::string& key, std::uint64_t client_id, std::uint64_t now_ms = NowMs());` / `SessionStatus RemoveServer(const std::string& key, std::uint64_t server_id, std::uint64_t now_ms = NowMs());` | 删整条 / 删 client / 校验 server_id 后删整条 | `Remove` 删除成功返回 `true` |
| `std::vector<SessionEntry> List(std::uint64_t now_ms = NowMs()) const;` / `std::size_t Tick(std::uint64_t now_ms = NowMs());` | 列出未过期条目 / 物理清理过期条目并返回删除数 | `Find`/`List` 只过滤，`Tick` 与写操作才删除 |
| `inline constexpr std::size_t kSessionMaxClientsPerKey = 4;` 等 | 默认 4 client/key、`kSessionLeaseMs = 90000`、`kSessionMaxKeys = 1024` | 配置 0 会钳到 1；`lease_ms == 0` 表示永不过期 |

### 3.3 session_control（`go2cpp::bento`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `enum class ControlMode : std::uint8_t` | `kNone=0 < kView=1 < kInteract=2` | `kNone` 是「无权限」：收回后不能请求 |
| `virtual ControlDecision Decide(const ControlRequestContext& context) const = 0;` | `kDeny` 拒绝 / `kGrant` 批准（原控制器降 kView） | `IControlPolicy` 在仲裁器锁下调用，不得回调本类、不得阻塞 |
| `std::shared_ptr<const IControlPolicy> MakeControlPolicy(ControlPolicy policy);` | `kFirstCome` 或 `kPreempt` 内置策略 | 也可经构造函数注入自定义策略 |
| `bool Join(std::uint32_t peer_id);` / `bool Leave(std::uint32_t peer_id);` / `bool OnPeerDisconnected(std::uint32_t peer_id);` | 加入（默认 kView）/ 主动离开 / 断开回收 | 离开或断开若持有控制权则立即回收 |
| `bool RequestControl(std::uint32_t peer_id);` / `bool Renew(std::uint32_t peer_id);` | 申请唯一控制权 / 续租 | 未 Join、`kNone`、被 Revoke 一律 `false` |
| `bool Grant(std::uint32_t peer_id, ControlMode mode, std::uint32_t actor);` / `bool Revoke(std::uint32_t peer_id, std::uint32_t actor);` | 显式授权（`kInteract` 直接抢占，不受策略限制）/ 撤销并禁止再请求 | Revoke 按 peer 身份持久，只有 Grant 解除 |
| `bool Tick(std::uint64_t now_ms);` | 用调用方时钟检查租约；过期则回收并返回 `true` | 默认租约 `lease_ms = 30000`；`0` 永不过期 |
| `ControlSnapshot Snapshot() const;` / `std::string ToJson() const;`（`ControlSnapshot` 成员） | 控制器、generation、租约剩余、参与者、最近审计（旧→新） | `ToJson()` 不含 audit，可直接广播 |
| `std::function<void(const ControlChange&)> on_change;` | 每次变化同步回调，携带 `seq/actor/timestamp_ms/session_id` | 在释放内部锁之后调用，可重入查询 |

### 3.4 peer_hub（`go2cpp::bento`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `bool Open(std::string* error = nullptr);` / `void Close();` | 创建并绑定 UDP socket / 关闭 socket 与所有 peer（幂等） | `bind_port = 0` 由内核分配，测试必需 |
| `bool AddPeer(std::uint32_t peer_id, const std::string& address, std::uint16_t port, std::string* error = nullptr);` | 注册主动 peer 并发首个 HELLO | peer_id 非 0 且未占用；`Connect(peer_id)` 用于超时后重连 |
| `int RunOnce(int timeout_ms);` / `int RunFor(int duration_ms);` | 处理一轮收发与定时器 / 循环运行指定时长 | 头文件没有 `OnDatagram` 方法：数据经回调 `on_datagram(peer_id, channel, data, bytes)` 交付；发送用 `SendDatagram(...)` |
| `bool SendStream(std::uint32_t peer_id, std::uint8_t channel, const void* data, std::size_t bytes, std::uint32_t dep_seq = 0);` / `SendStatus TrySendStream(std::uint32_t peer_id, std::uint8_t channel, const void* data, std::size_t bytes, std::uint32_t dep_seq = 0);` | 可靠消息（阻塞 / 区分 WouldBlock 与永久错误） | 线程安全；Send* 可与 RunOnce 并发 |
| `bool SendDatagram(std::uint32_t peer_id, std::uint8_t channel, const void* data, std::size_t bytes, std::uint32_t dep_seq = 0);` / `bool SendBarrier(std::uint32_t peer_id, std::uint32_t barrier_seq);` | 不可靠消息 / 因果屏障 | 不可靠层按 seq 单调，接收端 DropStale |
| `std::vector<std::uint32_t> PeerIds() const;` / `std::vector<PeerInfo> Peers() const;` | 当前 peer id 与快照（按 id 升序） | 被动 peer 超时后从表中消失，主动 peer 保留供重连 |
| `bool SetPreSharedKey(const std::uint8_t* key, std::size_t bytes);` / `bool SetPeerPreSharedKey(std::uint32_t peer_id, const std::uint8_t* key, std::size_t bytes);` | 启用 PSK 认证加密（必须 32B）/ 单 peer 覆盖 | 加密后拒绝明文 HELLO 与明文数据帧；每 peer 独立密钥与重放窗口 |
| `PeerState peer_state(std::uint32_t peer_id) const;` / `PeerHubStats stats() const;` / `PeerSecureStats secure_stats() const;` | 状态查询与计数器快照 | 超时默认 `peer_timeout_ms = 5000`；握手 `handshake_timeout_ms = 5000` |

### 3.5 app::session（`go2cpp::bento::app`）

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `SessionError Start();` | 解析端点→建传输→Connect→启动配对；成功返回 `kNone` 并进入 Connecting | 参数错误只设 `last_error` 并保持 Idle；建传输后失败进 Closed |
| `bool Poll(int timeout_ms = 0);` | 读传输→解帧→自动接受配对→定时器→发送队列；`true` 表示未 Closed | 顺序见 `session.cpp:439-458`；`timeout_ms` 只影响一次 `Read` |
| `void Close();` | 优雅关闭：发 `kClose`、Flush、`ShutdownWrite`，进入 Closing | 幂等；之后需 `Poll` 驱动到 Closed。**没有 `Stop()`** |
| `SessionError AcceptPairing();` | 服务端手动接受挂起 CONNECT（`auto_accept_pairing=false` 时） | 无挂起请求/角色不符返回 `kInvalidState` |
| `SessionError Send(const std::uint8_t* data, std::size_t size);` / `SessionError Send(std::string_view data);` / `SessionError Send(std::uint32_t channel_id, const std::uint8_t* data, std::size_t size);` | 在默认/指定通道发 Data 帧 | 非 Active 返回 `kInvalidState`，channel 不符 `kInvalidArgument`，队列满 `kQueueFull` |
| `SessionError RequestControl();` / `SessionError RequestControl(go2cpp::bento::ControlMode mode);` | 按配置/指定模式申请控制权 | 仲裁端直接决策并广播；非仲裁端发 `ControlRequest` |
| `bool WaitFor(SessionState target, int timeout_ms);` | 轮询直到目标状态或真实时间超时 | 只驱动本端 `Poll`，双端测试需外部交替驱动 |
| `SessionConfig` | 身份：`endpoint`、`server`、`pairing_key`、`local_peer_id=1`、`channel_id=1`；超时：`connect_timeout_ms=5000`、`io_timeout_ms=50`、`close_timeout_ms=2000`、`heartbeat_interval_ms=30000`、`heartbeat_timeout_ms=90000`、`pairing_timeout_ms=15000`、`pairing_retransmit_ms=1000`；控制：`requested_control_mode=kInteract`、`control`；注入：`transport_factory`、`now`、`max_outbox_frames=256`；测试：`pairing_nonce`、`pair_id`、`auto_accept_pairing=true` | `server=true` 必须注入已接受的 transport（`ITransport` 无 Listen/Accept） |
| `SessionState state() const noexcept;` / `SessionError last_error() const noexcept;` / `const go2cpp::bento::SessionControl& control() const noexcept;` | 状态、最近错误、只读仲裁器 | 修改控制权须走 `Session` API 以保持广播一致 |
| `SessionStateHandler on_state;` / `SessionDataHandler on_data;` / `SessionErrorHandler on_error;` / `SessionControlHandler on_control;` | 状态迁移 / 应用数据（指针仅回调期有效）/ 关闭前一次错误 / 控制权变更 | 非线程安全：回调内不得再调 `Poll`/`Send`/`Close` |
| `SessionStats` | `polls`、`frames_sent/received`、`bytes_*`、`data_frames_*`、`heartbeats_*`、`state_changes`、`control_requests`、`protocol_errors`、`dropped_frames` | `dropped_frames` 含内部帧与数据帧 |

## 4. 会话层协议（与 PROTOCOL §12 对齐）

`app::Session` 的线格式是**20B 头 + CRC-32**，与 Link 的 16B UDP 头不是一套协议（`frame.hpp:59-78`）。逐条核对 `tests/bento/PROTOCOL.md` 第 684-776 行：

| 协议要素 | 实现位置 | 文档结论 |
|---|---|---|
| 20B 头：magic(4)+version(2)+type(2)+flags(2)+reserved(2)+length(4)+checksum(4)，CRC-32/IEEE 覆盖头前 16B + 载荷 | `frame.hpp:76-78`、`src/bento/net/frame.cpp:129-160`；会话侧 `session.cpp:46,937` | 一致 |
| magic `0x53455353`（"SESS"）、version `1` | `session.hpp:126-129`、`session.cpp:46` | 一致 |
| 与 Link 隔离：Link 用 "BENT"/1，混接在 magic/version 阶段失败 | `frame.hpp:144-150`、`session.cpp:418`（`FrameDecoder(kFrameMaxPayload, kSessionFormat)`） | 一致 |
| 8 种帧类型 `0x8001`..`0x8008` | `session.hpp:137-146`、`session.cpp:33-43` | 一致 |
| Pairing 载荷 32B（`EncodePairingFrame`） | `session.cpp:553-567` | 一致 |
| Heartbeat 空载荷，非空即协议错误 | `session.cpp:517-522` | 一致 |
| Data：2B channel_id + 应用数据，仅 Active 接受 | `session.cpp:588-610` | 一致 |
| Control：1B action + 4B peer_id + 1B mode，peer 必须属于本会话 | `session.cpp:612-650` | 一致 |
| Close：1B `SessionError` | `session.cpp:782-788` | 一致 |
| ChannelOpen：2B channel_id + 4B peer_id；ChannelAck：2B channel_id | `session.cpp:652-715` | 一致 |
| ControlRequest：4B peer_id + 1B 模式，仅客户端→仲裁端 | `session.cpp:717-764`（非仲裁端收到即 `kProtocolError`） | 一致 |
| 状态机 Idle→Connecting→Paired→Active→Closing→Closed | `session.hpp:149-162`、`session.cpp:117-135` | 一致 |
| 超时：配对 `pairing_timeout_ms`、心跳 `heartbeat_interval_ms`/`heartbeat_timeout_ms`、关闭 `close_timeout_ms` | `session.cpp:889-916`、`session.cpp:1083` | 一致 |
| 发送队列 `max_outbox_frames=256`，满丢帧并计 `dropped_frames` | `session.hpp:272`、`session.cpp:928-933,1030-1032` | 一致（丢帧范围表述略窄，见下） |
| 单通道约束：Data/ChannelOpen/ChannelAck 的 channel_id 必须等于配置 | `session.cpp:598-601,669-672,703-706` | 一致 |

**核对发现的差异（2026-10-28 终审已修正 PROTOCOL.md §12，以下保留为核对记录）**：

> 终审按 `afc135a` 代码把本节 4 处连同 Link 帧类型范围（0x01..0x0A）、flags 解码行为、CRC 引用共 7 处一并改正到 `tests/bento/PROTOCOL.md` §12。


1. §12.4 把 `Flush` 写成可观察的操作（“`Flush` 每帧另有 `io_timeout_ms` 预算”），但 `Session` 头文件没有公开 `Flush()`；`Impl::Flush()` 由 `Start`/`Poll`/`Close` 内部调用（`session.cpp:970`），对外只有 `Poll`。
2. §12.3 说 `ChannelOpen.peer_id` “非 0 且不同于配置的本端 id 无意义”，实现是**硬错误**：`peer_id == 0 || peer_id == local_peer_id` 直接 `kProtocolError`（`session.cpp:674-676`）。
3. §12.5 的 `bento_test_session` 是可执行文件名；ctest 用例名是 `bento_session`（`cmake/Go2CppBento.cmake:451`），`ctest -R '^bento_'` 才能匹配。
4. §12.4 说“队列满时内部帧会被丢弃”，实现对所有 `SendFrame`（含 Data 帧）统一 `++dropped_frames`（`session.cpp:928-933`），`Send()` 再返回 `kQueueFull`——语义不冲突，但范围比文档宽。

## 5. 设计要点

- **控制权唯一性**：仲裁器只维护一个 `m_controller`（`session_control.cpp:341-425`）；`kInteract` 参与者至多一个，广播的 `Control.peer_id` 必须属于本会话（`session.cpp:628-635`）。
- **抢占语义**：冲突路径只把决策权交给 `IControlPolicy::Decide`，降级原控制器、审计与统计仍由仲裁器统一执行；`Grant(peer, kInteract)` 是管理操作，直接抢占且不受策略限制（`session_control.cpp:240-260`）。
- **租约过期**：`Tick(now)` 在 `now >= lease_deadline_ms` 时把控制器降为 `kView` 并记 `kLeaseExpired`；`lease_ms == 0` 表示永不过期，此时 `Renew` 返回 `false`（`session_control.cpp:514-539`）。
- **撤销按身份持久**：`m_revoked_peers` 跨 `Leave`/断开/重连保留，只有显式 `Grant` 才 `erase`；被撤销者 `Join` 后仍为 `kView` 且请求被拒（`session_control.cpp:159,239,363`）。
- **HELLO 重放防护**：时间戳窗口 `handshake_replay_window_ms = 30000` + 已接受 client nonce 的 FIFO 缓存；重放只计入 `replay_drops`，不打断现有会话（`peer_hub.cpp:1520-1535`）。
- **conn_id 迁移**：加密模式下只有 AEAD 校验通过才更新 peer 地址；明文模式因 conn_id 已确认身份，源地址变化直接视为 NAT 重绑定（`peer_hub.cpp:1378-1405`）。
- **队列满丢帧（含内部帧）**：`SendFrame` 在 `outbox.size() >= max_outbox_frames` 时 `++dropped_frames` 并返回 `false`，避免 ChannelAck/心跳静默卡死握手（注释 D-07，`session.cpp:928-933`）。
- **回调不重入**：`SessionControl` 先在锁内收集 `ControlChange`，释放锁后 `Dispatch`；`PeerHub` 用 `m_callback_depth` + 延迟销毁队列，允许在回调里 `RemovePeer`/`Close`（`peer_hub.hpp:41-49`）。

## 6. 边界与已知限制

- **容量**：会话表每 key 4 client、全表 1024 key；仲裁器 64 参与者、64 条审计；`PeerHub` 16 peer、每 peer 8 通道、单次 `RunOnce` 最多 256 帧；会话层单帧载荷上限 1 MiB。
- **超时默认值**：配对 15000ms/重传 1000ms；控制租约 30000ms；会话心跳 30000ms/存活 90000ms、关闭 2000ms、连接 5000ms、单次 I/O 50ms；`PeerHub` 握手 5000ms、peer 5000ms、心跳 500ms、独立 ACK 20ms、HELLO PTO 200→2000ms/10 次。
- **命名空间历史债**：`pairing`/`session_table`/`session_control`/`peer_hub` 位于 `go2cpp::bento` 根，`app::Session` 位于 `go2cpp::bento::app`；本文按现状记录，不做“修正”。
- **单通道**：`app::Session` 只支持一个通道，`Data`/`ChannelOpen`/`ChannelAck` 的 channel_id 必须等于 `SessionConfig::channel_id`，没有多路复用。
- **无重传层**：会话层跑在 `TcpTransport`/`MemoryTransport` 上，可靠性依赖字节流；写队列只处理部分写的剩余字节，不做丢包重传。
- **服务端需外部接线**：`ITransport` 没有 Listen/Accept，`server=true` 必须由 `transport_factory` 注入已接受的连接（`session.cpp:360-364`）。
- **未实现项**：`SessionTable` 不参与 `Session` 编排；`Session` 无 `Stop()`、无公开 `Flush()`；`PeerHub` 明文模式的 conn_id 迁移可被伪源地址抑制（DoS，PROTOCOL §11.7）。

## 7. 测试数据

| 测试文件 | 用例/断言（`grep` 实测） | 覆盖点 |
|---|---|---|
| `test_pairing.cpp` | 13 用例 / 41 `CHECK(` | 编解码往返、错误密钥、超时、重复注册、RESPONSE 丢失重试、CONFIRM 重试、陈旧响应、并发 CONNECT 拒绝、随机化 |
| `test_session_table.cpp` | 9 用例 / 37 `CHECK(` | Add/Find、client 上限、重复与删除、租约过期、并发 AddServer/AddClient/混合、统计 |
| `test_session_control.cpp` | 13 用例 / 168 `CHECK(` | 并发请求、断开回收、Revoke 阻断、租约边界、抢占、自定义策略、审计、Revoke 跨重连、随机化 |
| `test_peer_hub.cpp` | 8 用例 / 94 `CHECK(` | 1 服务端 3 peer、未知 conn_id 拒绝、超时隔离、AddPeer 校验、吞吐、回调删除、重连、超时回收 |
| `test_peer_hub_secure.cpp` | 10 用例 / 157 `CHECK(` | 三 peer 加密、错误 PSK、明文拒绝、per-peer PSK、重传恢复、重放/篡改、分片流、重连、HELLO 重放 |
| `test_session.cpp` | 20 用例 / 165 `CHECK(` | 端点/配置校验、配对、通道握手、心跳、控制仲裁、关闭窗口、写失败、协议错误、会话线格式、队列满丢内部帧、部分写续传 |

运行：`ctest -R '^bento_'`（需先按 `cmake/Go2CppBento.cmake` 构建）。**本次未执行测试**，上表数字来自 `grep -c 'CHECK('` 与 `grep -c '^void Test'` 的实际输出，不代表“已验证通过”。

## 8. 示例

两个示例均在基线代码上以 `g++ -std=c++17 -Wall -Wextra -Werror` 编译并运行通过，只依赖对应 `.cpp` 与 `-Iinclude`。

示例 1：配对帧编解码 + 双端配对状态机（编译：`g++ -std=c++17 -Iinclude demo.cpp src/bento/net/pairing.cpp`）。

```cpp
#include <cstdint>
#include "go2cpp/bento/net/pairing.hpp"

using namespace go2cpp::bento;

int main() {
    PairingFrame frame;
    frame.type = PairingFrameType::kConnect;
    frame.key_hash = FixedKeyAuthPolicy::Hash("invite-2026");
    frame.nonce = 0x1234567890abcdefULL;
    std::uint8_t wire[kPairingFrameBytes] = {};
    EncodePairingFrame(frame, wire);
    PairingFrame decoded;
    if (!DecodePairingFrame(wire, sizeof(wire), &decoded)) return 1;

    PairingSession server, client;
    server.Register("invite-2026", 0);
    client.Connect("invite-2026", 0);
    PairingFrame out;
    while (client.PopOutbound(&out)) server.HandleFrame(out, 1);
    server.Response(true, 2);
    while (server.PopOutbound(&out)) client.HandleFrame(out, 3);
    client.Activate(4);
    while (client.PopOutbound(&out)) server.HandleFrame(out, 5);
    while (server.PopOutbound(&out)) client.HandleFrame(out, 6);
    return (server.state() == PairingState::kActive && client.state() == PairingState::kActive) ? 0 : 2;
}
```

示例 2：SessionControl 的抢占、租约回收与撤销持久化（编译：`g++ -std=c++17 -Iinclude demo.cpp src/bento/net/session_control.cpp src/bento/net/protocol.cpp`）。

```cpp
#include <cstdio>
#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/bento/net/session_control.hpp"

using namespace go2cpp::bento;

int main() {
    SessionControlConfig config;
    config.policy = ControlPolicy::kPreempt;
    config.lease_ms = 1000;
    SessionControl control(config);
    control.Join(1);
    control.Join(2);
    control.RequestControl(1);                    // 1 获得控制权
    control.RequestControl(2);                    // Preempt：2 抢占，1 降 kView
    control.Tick(NowMs() + config.lease_ms + 1);  // 租约过期，控制权归零

    control.Revoke(2);                            // 撤销按身份持久
    control.Leave(2);
    control.Join(2);
    if (control.RequestControl(2)) return 1;      // 仍被拒绝

    std::printf("%s\n", control.Snapshot().ToJson().c_str());
    return control.has_controller() ? 2 : 0;
}
```

## 9. 相关文档

- [bento-security.md](bento-security.md)：密钥指纹、PSK AEAD、重放窗口的安全边界。
- [bento-transport.md](bento-transport.md)：`ITransport`/`TcpTransport`/`MemoryTransport` 与帧编解码。
- [bento-app.md](bento-app.md)：`Session` 之上的应用编排与生命周期。
- [bento-scenarios.md](bento-scenarios.md)：1 被控 : N 主控的端到端场景。
- [PROTOCOL.md](../../tests/bento/PROTOCOL.md)：§12 会话层协议原文（本文 §4 的核对基准）。
