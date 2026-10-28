# Bento 场景矩阵：JinShan 真实场景 → Go2Cpp 能力映射

- **版本**：v1.0（2026-10-27）
- **目的**：把 JinShan 远程桌面工程（只读参考）里真实出现过的运行场景，逐条映射到
  Go2Cpp `bento` 传输层的能力上，标出「已有 / 部分 / 缺 / 不适用」，作为补齐
  通用框架能力的依据。
- **参考基线（只读）**：
  - `JinShanProject/docs/01-requirements/RTM.md` v1.0（40 条：CON 15 / FR 19 / NFR 6）
  - `JinShanProject/docs/02-analysis/anomalies.md` v0.1（AX-001–AX-100）
  - `JinShanProject/docs/02-analysis/risk-register.md` v0.1（RISK-001–RISK-078）
- **被测代码基线**：`include/go2cpp/bento/net/**`、`src/bento/net/**`、`tests/bento/**`。
- **状态定义**：

  | 状态 | 含义 |
  |---|---|
  | 已有 | 有可复用的实现，且有对应测试（`tests/bento/` 下能指到具体文件） |
  | 已补 | 本轮 P1 新实现并有对应测试（前一次评估为「缺」、本批补齐的条目） |
  | 部分 | 底层构件存在，但缺少场景级能力（协议字段、路由、统计或测试之一） |
  | 缺 | 当前完全没有实现，属于明确的能力空白 |
  | 不实现 | 评估后明确不做（部署形态/超出传输库职责），理由写在「缺口说明」，不是遗漏 |
  | 不适用 | 该需求绑定 Windows / RDP / 应用语义，不属于通用传输层职责，需由上层实现 |

- **编号引用约定**：`FR/CON/NFR` 取自 RTM；`AX-xxx` 取自 anomalies；`RISK-xxx`
  取自 risk-register。每行至少引用一个编号，便于反向追溯。

---

## 1. 弱网（高延迟 / 丢包 / 限速 / 抖动 / 断线）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 1.1 | 300ms RTT + 5% 丢包下会话保持 | CON-07、NFR-02、AX-022、RISK-033 | `channel.hpp:ReliableChannel`（RFC 9002 SRTT/RTTVAR、动态 PTO、快速重传）、`RttEstimator` | 已有 | 测试用用户态弱网代理而非真实 `tc netem`；带宽整形未覆盖 |
| 1.2 | 20% 双向丢包 + 200ms 单向延迟，200 条消息 100% 按序 | CON-07、AX-019、RISK-035 | `tests/bento/udp_proxy.hpp`（用户态丢包/延迟代理）、`netem_probe.cpp`、ctest `bento_weaknet_proxy` | 已有 | PTO 退避上限 2s，最坏 60s 收尾窗口；未测 30% 以上丢包 |
| 1.3 | 带宽受限与发送速率整形（慢对端不拖垮发送方） | NFR-02、AX-014、RISK-004 | `congestion.hpp:CongestionController`（cwnd + pacing 令牌桶）、`bandwidth.hpp:BandwidthEstimator` | 部分 | 有 pacing，但没有「限速注入」测试工具；udp_proxy 只做丢包/延迟，不做带宽整形 |
| 1.4 | 心跳误判与断线判定（≤2 次丢失继续，3 次判死） | AX-010、AX-019、RISK-033 | `link.hpp:LinkConfig{peer_timeout_ms, heartbeat_interval_ms}`、`test_rtt_pto` | 已有 | 重连退避/抖动策略在应用层，Bento 只给状态与回调 |
| 1.5 | 包乱序、重复包、跨 recv 粘包 | AX-012、AX-013 | `ReliableChannel` SACK 位图、去重、`kReceiveWindow`；`test_unreliable_dropstale` | 已有 | 没有专门的「乱序注入器」，乱序只靠网络自然发生 |

## 2. 可靠 / 不可靠混合（画面可丢、控制不可丢）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 2.1 | 控制指令走可靠有序流，画面帧走可丢通道 | FR-02、FR-03、CON-10 | `protocol.hpp:FrameType{kStream,kDatagram,kVideo}`；`channel.hpp:ReliableChannel/UnreliableChannel/VideoChannel` | 已有 | 屏幕采集与编码不在 Bento 职责内（JinShan 用 IRDPSRAPI） |
| 2.2 | 不可靠消息 DropStale + 因果依赖（dep_seq / BARRIER） | FR-03、AX-035 | `channel.hpp:UnreliableChannel`、`relation.hpp:Relation`、`test_unreliable_dropstale` | 已有 | 无「输入事件 → 画面帧」的端到端因果链测试，Bento 只提供原语 |
| 2.3 | 视频分片丢失后的 XOR FEC 恢复 | NFR-02、RISK-035 | `channel.hpp:VideoChannel`（`kMaxFecGroupSize=8`）、`test_fec` | 已有 | FEC 只接在 `Link` 上，`PeerHub` 的 P0 范围未接视频 |
| 2.4 | 多通道隔离（move/click/video 各自 seq 与窗口） | FR-06、CON-08 | `link.hpp:kMaxChannels=8`、每通道独立 seq/窗口/流控 | 已有 | 通道数上限 8，超出丢弃并计数 |
| 2.5 | 单条消息超过 1300B 自动分片与重组 | FR-14、AX-013 | `protocol.hpp:kStreamFragmentBytes`、`channel.hpp:StreamReassembler`、`test_stream_fragment` | 已有 | 单条消息上限 ≈330KB（256 分片），更大需上层再分块 |

## 3. 多客户端（1 被控 : N 主控）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 3.1 | 一个监听端口同时服务 3 个主控 | FR-06、B2-2、AX-085、RISK-028 | **`peer_hub.hpp:PeerHub::AddPeer/RunOnce/SendStream(peer_id,…)`**（P0-1 新增） | 已有 | 本轮新增；`Link` 仍是单 peer，未改动（避免 2000+ 行回归） |
| 3.2 | 多主控之间不串流、各自通道独立 | RISK-030、AX-085 | `PeerHub` 按 `conn_id` 路由 + 每 peer 独立 `Relation` 与通道表；`test_peer_hub` 场景 1 | 已有 | 测试断言 3×50 条 STREAM + 3×50 条 DATAGRAM 全部按 peer 隔离、零串台 |
| 3.3 | 主控数量上限与拒绝策略 | AX-023、RISK-032 | `PeerHubConfig::max_peers`、`PeerHubStats::peers_rejected` | 已有 | 无「每 IP 连接数」限流 |
| 3.4 | 看/控权限（同时可看、仅一人可控） | FR-03、FR-06、RISK-002、RISK-024 | **`session_control.hpp:SessionControl`**（P1 新增：`ControlMode{kNone,kView,kInteract}`、`Grant/RequestControl/Revoke`）；`test_session_control` | 已补 | 通用仲裁器保证「多个 View + 最多一个 Interact」；RDP attendee 语义映射仍在上层，`PeerHub` 只提供通道 |
| 3.5 | 多主控同时高帧率时的带宽公平 | RISK-031、AX-089 | 每 peer 独立 `CongestionController` | 部分 | 有 per-peer 拥塞控制，但没有跨 peer 的全局公平调度/配额 |
| 3.6 | 主控断开后清理其会话资源 | AX-086、AX-093 | `PeerHub` 内部机制 `DropPeer`（peer_timeout 触发，private 不可直接调用）+ `on_peer_disconnected` 回调 | 部分 | 回收动作（撤销控制权、释放邀请）需应用层实现 |

## 4. 回环中继（invitation 改写 + 本地转发）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 4.1 | 本地监听 → 远端连接 → 双向字节转发 | FR-04、AX-029 | **`tcp_proxy.hpp:TcpProxy::Open/Poll`**（P0-2 新增） | 已有 | 通用组件，不绑定 RDP；`test_tcp_proxy` 覆盖 100KB 回显 |
| 4.2 | 多个 Viewer 各自独立管道、互不串流 | RISK-003、RISK-030、AX-028 | `TcpProxy` 每连接独立 fd 对与缓冲区；`test_tcp_proxy` 场景 3（3 并发） | 已有 | — |
| 4.3 | 半关闭传播（请求-响应协议不截断响应） | FR-04、AX-020 | `TcpProxy` 的 `shutdown(SHUT_WR)` + `half_close_timeout_ms`；`test_tcp_proxy` 场景 2 | 已有 | — |
| 4.4 | 端口被占用 / 动态端口分配 | AX-028、RISK-016 | `TcpProxyConfig::listen_port=0`（内核分配）+ `SO_REUSEADDR` | 已有 | 冲突重试策略由调用方决定（Bento 只报错） |
| 4.5 | invitation XML 解析与地址改写 | FR-04、RISK-014、RISK-015 | 无 | 不适用 | IRDPSRAPI 生成的 XML 是 Windows/RDP 专属；通用代理只负责字节转发 |
| 4.6 | 慢读 Viewer 的背压与回收 | AX-014、RISK-004 | `TcpProxy` 每方向有界缓冲 + `idle_timeout_ms`；`test_tcp_proxy` 场景 5 | 部分 | TCP 代理有背压；UDP 侧的多 peer 中继没有等价背压 |

## 5. 侧信道（虚拟通道命令 / 文件下发）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 5.1 | 命令通道：长度前缀 + 可靠有序传输 | FR-08、AX-052、RISK-039 | `ReliableChannel`（分片 + 流控 MAX_DATA）、`test_flow_control` | 部分 | 传输原语已有；命令帧格式、执行器、输出回传属应用层 |
| 5.2 | 大命令/大输出（8KB 命令、10MB 输出） | AX-052、AX-053 | `kMaxStreamMessageBytes`（≈330KB）、`StreamReassembler` | 部分 | 超过 330KB 需上层分块；无 `truncated` 标记与输出上限语义 |
| 5.3 | Win7 无 RDP8 时侧信道降级到独立 TCP 通道 | RISK-008、RISK-038、AX-081 | 无 | 不适用 | 动态虚拟通道（DVC）是 RDP 专属；Bento 本身就是「独立通道」这一降级形态 |
| 5.4 | 命令注入防护 | RISK-040、NFR-04 | 无 | 不适用 | 属应用层安全策略（参数数组/白名单），传输层不解析业务 |

## 6. 大文件（100MB / 2GB、哈希校验）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 6.1 | 100MB 双向传输且 SHA256 一致 | FR-14、AX-045、RISK-048 | STREAM 分片 + 流控；`test_stream_fragment`、`test_flow_control` | 部分 | 无文件块协议/断点续传/哈希校验；单条消息 330KB 上限需上层分块 |
| 6.2 | 0 字节文件与 >2GB 上限 | AX-044、AX-045 | `ReliableChannel::Send(bytes=0)` 边界未覆盖 | 部分 | 0 字节语义、>2GB 拒绝策略需上层定义 |
| 6.3 | 传输中断后的重试与半文件清理 | AX-049、RISK-047 | 可靠层重传（PTO/快速重传） | 部分 | `.part` 文件、磁盘满、只读目录属本地文件系统语义 |
| 6.4 | 大文件吞吐压测 | RISK-048、PT | `test_tcp_proxy`（100KB）、`test_peer_hub` 吞吐场景（600×512B ≈ 30 Mbit/s） | 部分 | 现有压测规模为百 KB 级；100MB 级长跑未做 |

## 7. 控制权（唯一控制者 / 抢占 / 回收）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 7.1 | 多个主控竞争控制权，仅一人 INTERACTIVE | FR-03、FR-06、RISK-024、AX-036 | **`session_control.hpp:SessionControl`**：先到先得/可抢占、唯一控制器、`test_session_control` 场景 1/5 | 已补 | 16 线程并发请求只产生 1 个 Interact；策略可选；应用层只需把输入事件接到 `Renew` |
| 7.2 | 控制器断开后回收控制权 | RISK-025、AX-037 | `SessionControl::Leave/OnPeerDisconnected`（P1）+ `PeerHub::on_peer_disconnected`（签名 `(peer_id, const char*)`，需一行 lambda 适配）；`test_session_control` 场景 2 | 部分 | 回收 API 已具备；应用用一行 lambda 转调 `OnPeerDisconnected(peer_id)`（Bento 不内建 PeerHub ↔ 仲裁器绑定，示例见 19.6） |
| 7.3 | 控制权变更广播给所有主控 | AX-095、AX-097 | `SessionControl::on_change` + `Snapshot().ToJson()`（P1）+ `PeerHub::PeerIds()` | 部分 | 变更事件与可序列化快照已提供；实际发送仍需应用遍历 `PeerIds()`（Bento 不内置广播语义） |
| 7.4 | UIPI / 提权窗口输入拦截 | RISK-027、AX-071 | 无 | 不适用 | Windows 完整性级别机制，Linux 运行时不存在 |

## 8. 公网穿透（中转 / 无 P2P / 背压）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 8.1 | 跨网统一经中转，不做 P2P 直连 | CON-09、RISK-006 | 无（`TcpProxy` 只解决本地回环） | 不实现 | **不实现理由**：Bento 的定位是传输库，UDP 直连要求对端地址可达；公网中转是独立的服务部署形态（JinShan 侧由 Go 中转服务承担，见第 14 节），打洞/NAT 穿透涉及 STUN/TURN、鉴权与运维部署，超出本库职责。可复用的部分是 `TcpProxy`（本地回环转发）与 `PeerHub`（单端口多 peer） |
| 8.2 | 本地中继：主控把远端地址改写成 127.0.0.1 | FR-04、AX-029 | `tcp_proxy.hpp:TcpProxy` | 部分 | 只解决「本地回环 → 远端」；公网中转需另外部署 |
| 8.3 | 中转背压：慢客户端不拖垮服务 | RISK-004、AX-014 | `TcpProxy` 有界缓冲 + 背压 + 空闲超时 | 部分 | TCP 侧有；UDP 多 peer 侧无全局背压 |
| 8.4 | 中转不可达 / 连接超时 | RISK-009、AX-017 | `TcpProxyConfig::connect_timeout_ms`、`stats.dial_failed`；`PeerHub` 握手超时 | 部分 | 重试与提示在调用方 |
| 8.5 | 端口/DNS 解析失败要立即暴露 | AX-016、AX-017 | `TcpProxy::Open` / `PeerHub::AddPeer` 返回 `std::string* error`；`UdpSocket::Resolve` | 已有 | 无结构化错误码（见 12.4） |
| 8.6 | relay 重启后会话重建 | RISK-036、AX-021 | `PeerHub::Connect`（重新握手）、`Link::Resume`（0-RTT） | 部分 | 应用会话重建（邀请、录像状态）属上层 |

## 9. 加密（PSK / AEAD / 重放 / 0-RTT）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 9.1 | 预共享密钥认证加密 | NFR-04、RISK-007 | `link.hpp:Link::SetPreSharedKey`、`peer_hub.hpp:PeerHub::SetPreSharedKey/SetPeerPreSharedKey`（P1）、`crypto.hpp`（ChaCha20-Poly1305、`kPskBytes=32`）、`test_secure_link`、`test_peer_hub_secure` | 已有 | `Link` 与 `PeerHub` 均支持；监听端 per-peer PSK 只能继承 Hub 默认 PSK（见 19.5） |
| 9.2 | 篡改 / 重放 / 明文注入必须被拒 | NFR-04、AX-008、RISK-007 | `crypto.hpp:ReplayWindow`、`OpenStatus`；`test_secure_injection` | 已有 | — |
| 9.3 | 0-RTT 会话恢复（票据） | NFR-02、RISK-036 | `Link::ExportSessionTicket/Resume`、`test_zero_rtt` | 已有 | 仅 `Link`；票据不含密钥本身 |
| 9.4 | 多 peer 下的加密与密钥隔离 | FR-06、NFR-04 | **`PeerHub::SetPreSharedKey/SetPeerPreSharedKey`**（P1 复用 `crypto.hpp`）；每 peer 独立 nonce/方向密钥/wire seq/64 位重放窗口；`test_peer_hub_secure` 场景 1/4 | 已补 | 3 peer 加密并发收齐；per-peer 不同 PSK 连两个服务端；错误 PSK 5 次 AEAD 失败且无会话；明文 HELLO/数据帧均被拒 |
| 9.5 | 错误密钥 / 同 key 双注册的处理 | AX-004、AX-005、RISK-011 | `Link` 的 `auth_failures`；`PeerHub::secure_stats().auth_failures`（P1）+ `test_peer_hub_secure` 错误 PSK 场景 | 部分 | 已有 PSK 鉴权与认证失败计数；配对/多 key 策略在应用层，监听端 per-peer PSK 受限（见 19.5） |

## 10. 迁移（NAT rebinding / 换网 / 重连）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 10.1 | 本端换地址后会话延续（`Link::Migrate`） | AX-010、RISK-033 | `link.hpp:Link::Migrate`、`migration_grace_ms`、`test_conn_migration` | 已有 | 新旧地址族必须一致（IPv4/IPv6 不混用） |
| 10.2 | 对端地址变化识别与旧地址静默期 | AX-010、RISK-033 | `Link::UpdatePeerAddress` + `conn_id` 路由 + 退役地址窗口 | 已有 | 只有 AEAD 认证通过的帧才允许改地址（加密模式） |
| 10.3 | 多 peer 下某个 peer 换地址 | FR-06、RISK-033 | `PeerHub` 内部 `HandlePacket`（private）按 `conn_id` 更新 peer 地址 | 部分 | 逻辑已实现，但缺少专门测试（P1） |
| 10.4 | 重连风暴与端口复用 | AX-091、RISK-034 | `PeerHub::Connect`；`TcpProxy` 的 `SO_REUSEADDR` | 部分 | 退避/抖动/限流在调用方 |
| 10.5 | 单调时钟 vs 墙钟（NTP 校时不影响超时） | AX-092 | `protocol.hpp:NowMs()`（steady_clock） | 已有 | 握手时间戳用墙钟（跨进程可比），已单独区分 |

## 11. 差环境（Win7 / PE / 低权限 / 缺组件）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 11.1 | 无 CAP_NET_ADMIN 时的弱网测试 | CON-06、NFR-01 | `tests/bento/udp_proxy.hpp`（用户态丢包/延迟） | 已有 | 不能模拟带宽整形与真实链路抖动 |
| 11.2 | 无 root、纯用户态运行 | CON-06、AX-071 | 全部组件只用普通 UDP/TCP socket，无特权调用 | 已有 | `tc netem` 测试在无权限时 SKIP（`SKIP_RETURN_CODE 77`） |
| 11.3 | Windows 7 / PE / IRDPSRAPI | CON-03、CON-10、AX-025、AX-070 | 无 | 不适用 | Go2Cpp 是 Linux 运行时；Win32/COM/RDP 能力不在范围内 |
| 11.4 | 关键组件缺失的降级与提示 | AX-065、AX-073、NFR-03 | 无 | 不适用 | 能力检测与降级矩阵属应用层 |
| 11.5 | 32 位内存受限（2GB 地址空间） | AX-075、RISK-074 | 有界缓冲：`PeerHubConfig::max_peers`、`TcpProxyConfig::buffer_bytes/max_connections` | 部分 | 没有 32 位构建与内存基线测试 |
| 11.6 | 端口占用/权限不足时的明确报错 | AX-063、RISK-075 | `Open()` 返回 `std::string* error`（bind/listen 失败原样透出） | 已有 | — |

## 12. 可观测（统计 / 日志 / 错误码）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 12.1 | 链路层与可靠层统计 | NFR-03、C7 | `link.hpp:LinkStats`、`Link::Stats()`、`LinkStatsToJson`、`test_stats_json` | 已有 | — |
| 12.2 | 多 peer 统计与 per-peer 视角 | FR-06、AX-085 | `peer_hub.hpp:PeerHubStats/PeerInfo`、`reliable_stats(peer_id, channel)` | 已有 | — |
| 12.3 | 代理层统计（accept/dial/idle/reset） | AX-014、AX-028 | `tcp_proxy.hpp:TcpProxyStats` | 已有 | — |
| 12.4 | 结构化错误码（而非只有字符串） | NFR-03、AX-096 | 目前只有 `std::string* error` 与回调 `reason` 字符串 | 部分 | 无错误码枚举，调用方无法稳定分支 |
| 12.5 | 统一日志接入 | RISK-076、AX-024 | **`log_hook.hpp:LogHook`**（P1）+ `PeerHub::SetLogHook`；`test_log_hook` | 已补 | 回调/级别/按级别与事件计数；连接、握手重传、可靠层重传、丢包、认证失败、超时、明文拒绝已接入；握手失败仅覆盖 max peers 拒绝，帧解析/发送封装失败无事件（部分事件，缺口见 19.5）；未注入时只累计 `filtered` 计数 |
| 12.6 | 帧级非法/未知来源可观测 | AX-008、AX-012 | `LinkStats{invalid_frames, unknown_peer_frames}`、`PeerHubStats` 同名字段 | 已有 | — |

## 13. 录屏（会话录制 / AVI / 索引）

| # | 场景 | JinShan 编号 | Go2Cpp 现有能力（文件:接口） | 状态 | 缺口说明 |
|---|---|---|---|---|---|
| 13.1 | 会话录屏与回放 | FR-17、RISK-054、AX-098 | 无 | 不适用 | AVI 封装、`idx1` 索引、损坏标记属应用层文件格式 |
| 13.2 | 录制不阻塞 UI（独立线程 + 有界队列） | RISK-055 | 无 | 不适用 | 线程模型属应用层；Bento 的接收是单线程 `RunOnce` 模型，可被复用但不是录制器 |
| 13.3 | 录屏帧的传输通道 | FR-17、FR-02 | `channel.hpp:VideoChannel`（分片 + FEC）、`test_video_fragment`、`test_fec` | 部分 | 可承载帧数据；落盘与回放不在 Bento |
| 13.4 | 磁盘满导致录制中断 | RISK-056、AX-098 | 无 | 不适用 | 本地文件系统语义 |

---

## 14. 不适用项与理由（汇总）

| 项 | 涉及编号 | 不适用理由 |
|---|---|---|
| Win7/PE/32 位 Windows 兼容 | CON-03、AX-070、AX-082 | Go2Cpp 是 Linux 运行时（epoll/poll + POSIX socket），不交付 Windows 桌面形态；Win32 API 与 PE 环境不存在 |
| IRDPSRAPI / RDP 媒体层 | CON-10、AX-025、AX-031 | `SharingSession/Viewer`、RDP 编码协商、H.264 回退都是 Windows COM 组件；Bento 只做传输，不自研采屏/编码 |
| invitation XML 解析与改写 | FR-04、RISK-014、RISK-015 | 系统生成的 RDP 邀请串格式随 Windows 版本变化，属 RDP 会话建立；通用回环代理只搬运字节 |
| 虚拟通道（DVC）协议 | FR-08、RISK-008、AX-081 | DVC 是 RDP 的动态通道，Windows 专属；Bento 提供的是「独立可靠/不可靠通道」这一等价能力 |
| 控制权仲裁与 UIPI | FR-03、RISK-002、RISK-024、RISK-027 | 控制权是 RDP attendee 语义，UIPI 是 Windows 完整性级别；Bento 只提供 per-peer 消息通道 |
| 命令注入 / 路径穿越防护 | RISK-040、RISK-046、AX-047 | 属应用层输入校验；传输层不解析业务载荷 |
| 备份打包上传 / AVI 录制 | FR-18、FR-17、RISK-050、RISK-054 | 文件格式与 HTTP 上传属应用层；Bento 不引入 zip/AVI 依赖 |
| Go 中转服务部署 | CON-02、CON-14、RISK-075、RISK-077 | JinShan 的中转是 Go 独立服务；Go2Cpp 侧能提供的是传输组件与本地代理，不是部署形态 |
| 提权重连 / 服务模式（Session 0） | FR-07、RISK-005、RISK-058 | Windows 进程与 UAC 语义，Linux 无对应物 |

---

## 15. 缺口汇总与优先级

| 优先级 | 缺口 | 影响场景 | 建议动作 |
|---|---|---|---|
| P0（已补） | 单 peer 限制：一个 socket 无法服务多个主控 | 3.1–3.3 | 新增 `PeerHub`（多 peer 路由 + per-peer 通道），`test_peer_hub` 覆盖 3 peer 隔离 |
| P0（已补） | 无通用 TCP 回环代理 | 4.1–4.4、8.2 | 新增 `TcpProxy`（并发、半关闭、超时），`test_tcp_proxy` 覆盖 100KB/半关闭/并发 |
| P1（本轮已补） | 控制权仲裁缺失（唯一控制器/回收/广播） | 3.4、7.1–7.3 | 新增 `SessionControl`；`test_session_control` 覆盖并发/断开/租约/抢占/随机压力 |
| P1（本轮已补） | `PeerHub` PSK 加密缺失（历史缺口） | 9.4、9.5 | `SetPreSharedKey/SetPeerPreSharedKey`，每 peer 独立 nonce/密钥/重放窗口 |
| P1（本轮已补） | 无统一日志钩子 | 12.5 | 新增 `LogHook`，PeerHub 接入连接/重传/丢包/认证事件 |
| P1 | `PeerHub` 未接 0-RTT 会话恢复 | 9.3 | 票据机制在 `Link` 上；peer 粒度的票据缓存需重新设计 |
| P1 | `PeerHub` 未接视频通道与 FEC | 2.3、13.3 | 复用 `VideoChannel`，per-peer 重组状态 |
| P1 | 多 peer 下地址迁移缺测试 | 10.3 | 增加 NAT rebinding 测试（按 conn_id 更新地址） |
| P1 | 结构化错误码缺失 | 12.4 | 引入 `enum class BentoError`，与字符串并存 |
| P2 | 带宽整形/乱序注入测试工具 | 1.3、1.5 | 扩展 `udp_proxy`：限速、乱序、抖动 |
| P2 | 100MB 级大文件压测 | 6.1、6.4 | 应用层分块协议 + 端到端哈希 |

---

## 16. 覆盖统计与自检

| 指标 | 数值 |
|---|---|
| 场景类别 | 13 类（弱网/混合可靠/多客户端/回环中继/侧信道/大文件/控制权/公网穿透/加密/迁移/差环境/可观测/录屏） |
| 场景条目 | 66 条 |
| 已有 | 30 条 |
| 已补 | 4 条（3.4 看控权限、7.1 控制权仲裁、9.4 多 peer 加密、12.5 日志钩子） |
| 部分 | 22 条 |
| 缺 | 0 条 |
| 不实现 | 1 条（8.1 公网中转；理由见该行与第 14 节） |
| 不适用 | 9 条（另见第 14 节 9 项理由） |

自检清单：

- [x] 13 类场景齐全，每类至少 2 条具体场景。
- [x] 每行都引用了 JinShan 编号（FR/CON/NFR、AX-xxx 或 RISK-xxx）。
- [x] 每行的 Go2Cpp 能力都指向真实文件与接口（`include/go2cpp/bento/net/**`、`tests/bento/**`）。
- [x] 「不适用」项给出理由，而不是简单标成缺失。
- [x] 缺口按 P0/P1/P2 分级；P0 两项与 P1 三项均已在代码中补齐并有测试，8.1 明确标「不实现」并给理由。
- [x] 统计数字与表格行数一致：场景行 66 = 已有 30 + 已补 4 + 部分 22 + 缺 0 + 不实现 1 + 不适用 9，
      可用以下命令复核：

  ```bash
  grep -cE '^\| [0-9]+\.[0-9]+ \|' docs/modules/bento-scenarios.md
  grep -E '^\| [0-9]+\.[0-9]+ \|' docs/modules/bento-scenarios.md \
    | grep -oE '\| (已有|已补|部分|缺|不实现|不适用) \|' | sort | uniq -c
  ```

---

## 17. 变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-10-27 | v1.0 | 首次建立：从 RTM/anomalies/risk-register 抽取 13 类 66 条场景，映射到 Bento 现有能力；记录 P0 两项缺口（多 peer、TCP 回环代理）的补法 |
| 2026-10-27 | v1.1 | P1 落地：控制权仲裁（`SessionControl`）、多 peer 加密（`PeerHub` PSK）、日志钩子（`LogHook`）补齐；3.4/7.1/9.4/12.5 标「已补」，8.1 标「不实现」并给理由；新增 3 个测试目标，ctest 26/26（netem 无权限 SKIP） |
| 2026-10-28 | v1.2 | 文档回改（审查 `review-p1-modules-api.md` DOC-01～13）：9.1/9.5/18.5 更新为已接 PSK，7.2 按实际回调签名改写，19 节改为已提交并补 19.6 最小示例，12.5 标注事件覆盖缺口，3.6/10.3 标注 private 机制；新增 §20 并发政策例外 ADR。能力与 16 节统计口径不变 |

---

## 18. 落地记录（2026-10-27）

> 本节记录本矩阵对应的代码落地与验证结果。按任务边界，改动只落在
> `include/go2cpp/bento/net/**`、`src/bento/net/**`、`tests/bento/**`、
> `cmake/Go2CppBento.cmake`、`docs/modules/bento-scenarios.md`。

### 18.1 新增

| 文件 | 作用 |
|---|---|
| `include/go2cpp/bento/net/peer_hub.hpp` + `src/bento/net/peer_hub.cpp` | 多 peer 链路：单 socket + `conn_id` 路由 + per-peer 通道 |
| `include/go2cpp/bento/net/tcp_proxy.hpp` + `src/bento/net/tcp_proxy.cpp` | 通用 TCP 回环代理：并发连接、半关闭、超时、背压 |
| `tests/bento/test_peer_hub.cpp` | 1 被控 3 主控、伪造 conn_id、超时隔离、参数校验、吞吐 |
| `tests/bento/test_tcp_proxy.cpp` | 100KB 回显、半关闭、并发隔离、拨号失败、空闲超时、连接上限、幂等关闭 |

### 18.2 修改

| 文件 | 改动 | 理由 |
|---|---|---|
| `src/bento/net/link.cpp` | `UdpSocket::Open` 增加 `SO_RCVBUF/SO_SNDBUF`（1MB） | 多 peer 突发时内核接收队列是最先丢包点，实测 150 个 80B 包丢 8 个 |
| `cmake/Go2CppBento.cmake` | 两个新源文件；两个测试目标 + ctest 注册 + TIMEOUT | 让新组件进入既有构建/测试入口 |

### 18.3 验证结果

| 项 | 结果 |
|---|---|
| 编译 | Bento 39 个目标全部成功，`-Werror` 下 0 警告 |
| `bento_test_peer_hub` | PASS，389 断言；3 peer × 50 STREAM + 50 DATAGRAM 全隔离；0 串台；吞吐 307KB/80ms ≈ 30.7 Mbit/s |
| `bento_test_tcp_proxy` | PASS，44 断言；100KB 回显字节一致；半关闭响应完整；3 并发不串流；超时/上限生效 |
| ctest（Bento） | 23/23 通过（含 21 个测试程序 + weaknet + netem） |
| 回归 | `bento_test_link_loopback` 866 断言通过，`bento_secure_link` 等 20 个测试全部通过 |
| 稳定性 | 两个新测试连续运行 5 次全部 PASS，无 flaky |

### 18.4 实现中发现的两个坑（已修复）

1. **控制帧漏写 `conn_id`**：`PeerHub::SendControl` 最初直接 `BuildFrame`，
   ACK/心跳/MAX_DATA 的 `conn_id=0`，接收端按 `conn_id` 路由查不到 → ACK 回不去
   → 发送端 `Pump` 被初始 cwnd 卡住。实测表现：每 peer 发 50 条只交付 32 条
   （32 正好是初始 cwnd）。修复：控制帧统一写 `peer.conn_id`。
2. **UDP 突发打满内核 rcvbuf**：3 peer × 50 条 80B 包一次性突发，默认
   rmem ~212KB 叠加 skb 开销会丢包（实测 8/150）。修复：`UdpSocket::Open`
   把收发缓冲抬到 1MB；测试侧改为「边发边 pump」。

### 18.5 已知限制

- PSK 已于 19.2 补齐（`PeerHub::SetPreSharedKey/SetPeerPreSharedKey`）；`PeerHub` 仍未接 0-RTT/VIDEO/PMTUD（这些只在 `Link` 上），列为 P1。
- 多 peer 下地址迁移已有实现（按 `conn_id` 更新地址）但缺专项测试。
- `TcpProxy` 是单线程 poll 模型，连接上限默认 64，定位是工具/本地中继。
- 仓库无 `.clang-format`，新增文件沿用既有 4 空格/100 列风格，未跑格式化。

---

## 19. P1 落地记录（2026-10-27）

> 本节记录 P1 三项缺口的落地：控制权仲裁、多 peer 加密、日志钩子。改动只落在
> `include/go2cpp/bento/net/**`、`src/bento/net/**`、`tests/bento/**`、
> `cmake/Go2CppBento.cmake`、`docs/modules/bento-scenarios.md`；不改 base。
> 已提交到 HEAD `1e08723`（`feat(bento): 场景缺口补齐（控制权仲裁/多 peer 加密/日志钩子）`）。

### 19.1 新增

| 文件 | 作用 |
|---|---|
| `include/go2cpp/bento/net/session_control.hpp` + `src/bento/net/session_control.cpp` | 通用控制权仲裁：View/Interact、唯一控制器、先到先得/抢占、租约与断开回收、变更回调与 JSON 快照 |
| `include/go2cpp/bento/net/log_hook.hpp` | 轻量日志钩子：级别过滤、结构化事件、按级别/事件计数、线程安全 Emit（sink 在锁外调用） |
| `tests/bento/test_session_control.cpp` | 16 线程并发请求、断开回收、Revoke 禁入、租约过期、抢占、回调/快照、4000 步随机不变式 |
| `tests/bento/test_peer_hub_secure.cpp` | 3 peer 加密、错误 PSK、明文拒绝、per-peer 不同 PSK、PTO 重传恢复、参数校验 |
| `tests/bento/test_log_hook.cpp` | 级别/计数/Reset、并发 4000 条、PeerHub 连接/握手重传/重传丢包/超时/认证失败事件 |

### 19.2 修改

| 文件 | 改动 | 理由 |
|---|---|---|
| `include/go2cpp/bento/net/peer_hub.hpp` | 新增 `PeerHubConfig{pre_shared_key, handshake_replay_window_ms, log_hook}`、`PeerSecureStats`、PSK/日志 API 与 per-peer 加密状态 | 复用 `crypto.hpp` 与 Link 的握手消息格式（HelloBody/HelloAckBody/SealHandshakeFrame/SealFrame） |
| `src/bento/net/peer_hub.cpp` | 加密 HELLO/HELLO_ACK、数据帧 Seal/Open、明文与重放拒绝、per-peer 密钥、日志事件 | 每 peer 独立 nonce/方向密钥/wire seq/64 位重放窗口；只有 AEAD 通过才更新地址 |
| `cmake/Go2CppBento.cmake` | 新增 `session_control.cpp` 与 3 个测试目标、ctest 注册与 TIMEOUT | 让新组件进入既有构建与测试入口 |

### 19.3 验证结果

| 项 | 结果 |
|---|---|
| 编译 | 全量构建成功；`-Werror` 下 0 警告 |
| `bento_test_session_control` | PASS，14,816 断言；16 线程并发请求仅 1 个 Interact；4000 步随机操作不变式成立 |
| `bento_test_peer_hub_secure` | PASS，506 断言；3 peer 加密 90 STREAM + 60 DATAGRAM 全隔离；错误 PSK 5 次 AEAD 失败且无会话；明文拒绝；per-peer 双 PSK 各 10/10；PTO 重传 32 次后 200 条按序恢复 |
| `bento_test_log_hook` | PASS，124 断言；8 线程 × 500 条并发计数一致；连接/握手重传/重传/丢包/超时/认证失败事件均命中 |
| ctest（Bento） | 26/26 通过（原 23 + 新增 3；bento_netem 因无 CAP_NET_ADMIN 按设计 SKIP） |
| 稳定性 | 三个新测试连续运行 3 次全部 PASS |

### 19.4 实现中发现的坑（已修复）

1. **per-peer 加密 ACK 被 Hub 默认 PSK 判断挡住**：`HandleHelloAckPacket` 最初用
   `m_secure_enabled` 决定是否处理加密 ACK；`SetPeerPreSharedKey` 场景下 Hub 本身没有
   默认 PSK，ACK 被当非法帧丢弃 → 服务端已完成握手、客户端永远停在 kConnecting。
   修复：加密 ACK 改按 `peer->secure_enabled` 分派。
2. **PTO 丢包不计入 `loss_events`**：`CongestionController::OnTimeout` 只增加
   `timeout_events`，日志钩子最初只比较 `loss_events`，PTO 场景没有丢包事件。
   修复：丢包事件按 `loss_events + timeout_loss_events` 的增量计。

### 19.5 已知限制

- `SessionControl` 与 `PeerHub` 不内建绑定：断开回收与广播发送需应用分别接
  `on_peer_disconnected` 与遍历 `PeerIds()`（避免状态机依赖 socket）。
- 监听端的 per-peer PSK 只能继承 Hub 默认 PSK（未知来源在收到 HELLO 前无法确定 PSK）；
  `SetPeerPreSharedKey` 的差异化密钥用于主动端。
- `PeerHub` 仍未接 0-RTT、VIDEO/FEC、PMTUD；这些仍在 `Link` 上（见 18.5）。
- 日志事件按每 tick 的统计增量聚合，不逐包回调；极端高重传速率下事件是「批」而不是「逐帧」。
- 3.4 的「看控权限」只提供通用 View/Interact 原语，RDP attendee 语义映射仍由应用完成。
- 日志事件仍有覆盖缺口：握手超时只发 `kPeerDisconnected`（不发 `kHandshakeFailed`），
  帧解析失败与发送封装失败只计数、不发 `kFrameRejected`/`kSendError`（审查 D-13/D-14）。
- 并发模型对 `docs/review/00-concurrency-policy.md` 的 R1/R3 属结构性例外，
  决策与约束见 §20 ADR-BENTO-001。

### 19.6 最小接线示例（SessionControl + PeerHub 加密）

下面是一个可编译的最小接线：PSK 加密的多 peer 收发、断开回调转给仲裁器、控制权
变更广播。省略业务协议与错误处理；`PeerHub::on_peer_disconnected` 的签名是
`(peer_id, const char* reason)`，必须用一行 lambda 适配
`SessionControl::OnPeerDisconnected(peer_id)`（这也是 7.2 的正确接法）。

```cpp
#include <cstdint>
#include <cstdio>
#include <string>

#include "go2cpp/bento/net/log_hook.hpp"
#include "go2cpp/bento/net/peer_hub.hpp"
#include "go2cpp/bento/net/session_control.hpp"

using namespace go2cpp::bento;

int main() {
    // 1) 日志钩子（可选）：Bento 事件统一落到一处。
    LogHook log(
        [](const LogRecord& record) {
            std::printf("[bento] %s peer=%u value=%llu %s\n", LogEventName(record.event),
                        record.peer_id, static_cast<unsigned long long>(record.value),
                        record.message ? record.message : "");
        },
        LogLevel::kInfo);

    // 2) 控制权仲裁：先到先得、30s 租约、默认只读。
    SessionControlConfig control_config;
    control_config.policy = ControlPolicy::kFirstCome;
    control_config.lease_ms = 30000;
    SessionControl control(control_config);

    // 3) PeerHub：监听端，启用 PSK 后只接受加密 HELLO。
    PeerHubConfig hub_config;
    hub_config.bind_address = "0.0.0.0";
    hub_config.bind_port = 0;  // 0 = 内核分配；生产环境应固定端口
    hub_config.max_peers = 8;
    hub_config.log_hook = &log;  // 非拥有指针：log 的生命周期必须覆盖 hub
    PeerHub hub(hub_config);
    if (!hub.Open()) {
        return 1;
    }

    std::uint8_t psk[32] = {};  // 实际从安全配置读取，禁止硬编码
    if (!hub.SetPreSharedKey(psk, sizeof(psk))) {
        return 1;
    }

    // 4) 断开 -> 回收控制权。两个签名不同，必须用一行 lambda 适配。
    hub.on_peer_disconnected = [&control](std::uint32_t peer_id, const char* /*reason*/) {
        control.OnPeerDisconnected(peer_id);
    };

    // 5) 变更 -> 广播 JSON 快照。PeerHub 不内建广播语义，由应用遍历 PeerIds。
    //    on_change 在 SessionControl 锁外执行；可能由 Hub 回调触发，
    //    PeerHub::SendStream 线程安全且其锁在同线程可重入，不会自锁。
    control.on_change = [&control, &hub](const ControlChange&) {
        const std::string snapshot = control.Snapshot().ToJson();
        for (std::uint32_t peer_id : hub.PeerIds()) {
            hub.SendStream(peer_id, 0, snapshot.data(), snapshot.size());
        }
    };

    // 6) 入会与授权（真实应用在收到控制协议消息后调用）。
    control.Join(1);
    control.Join(2);
    control.Grant(2, ControlMode::kInteract);  // 管理操作：直接指定控制器
    control.RequestControl(1);                 // 已有控制器，被拒
    control.Renew(2);                          // 收到输入事件时续租

    // 7) 单线程驱动：RunOnce 收包并触发回调；Tick 回收过期租约。
    for (int i = 0; i < 100; ++i) {
        hub.RunOnce(10);
        control.Tick();
    }
    hub.Close();
    return 0;
}
```

编译（仓库根目录；把 `build-review` 换成你自己的 Bento 构建目录）：

```bash
g++ -std=c++17 -I include /tmp/bento_wiring.cpp \
  build-review/libgo2cpp_bento.a -lpthread -o /tmp/bento_wiring
```

要点：
- `LogHook::Sink` 在锁外调用，但仍是同步调用；慢 sink 会直接拖慢 `RunOnce`（见 19.5）。
- `PeerHub` 的四个回调在 Hub 锁内执行，回调里禁止 park/阻塞/重入 `RunOnce`；
  只做转发和 `Send*`（见 §20 ADR）。
- `SessionControl::on_change` 在锁外执行，可在里面查询 `Snapshot()`；广播由应用
  遍历 `PeerIds()` 完成，库不内建发送语义。

---

## 20. ADR-BENTO-001：Bento 并发政策例外（R1/R3）

> 状态：**已接受**（2026-10-28）· 决策者：go2cpp maintainers
> 关联：`docs/review/00-concurrency-policy.md` R1/R3/R5；审查 `review-p1-modules-api.md` DOC-09、D-12、D-16。

### 20.1 背景

`00-concurrency-policy.md` 要求（R1）新增代码默认用 `sync::Mutex`、禁止裸
`std::mutex`；（R3）回调一律锁外执行，持锁回调必须标注「不得 park/阻塞」并加断言。
Bento 三个 P1 模块（`SessionControl`、`LogHook`、`PeerHub`）落地时与两条规则冲突：

- Bento 是**可选、可独立编译**的传输组件，`cmake/Go2CppBento.cmake` 只链 `Threads`
  与 `hash`，**不依赖 base/scheduler/sync/fiber**；引入 `sync::Mutex` 会把传输层
  拖进整个运行时依赖。
- `SessionControl` / `LogHook` 使用裸 `std::mutex`；`PeerHub` 使用
  `std::recursive_mutex`（支持回调内重入 `Send*`）。
- `PeerHub` 的 `on_peer_connected` / `on_peer_disconnected` / `on_stream` /
  `on_datagram` 在 `RunOnce` 持 `m_mutex` 期间同步调用；`SessionControl::on_change`
  与 `LogHook` sink 已在锁外执行。

### 20.2 决策

1. **R1 例外**：Bento 内部允许裸 `std::mutex` / `std::recursive_mutex`，不使用
   `sync::Mutex`。理由：Bento 不链接 base，且所有临界区只做内存操作与短时 socket
   调用，不 park、不跨 Fiber。若未来 Bento 被 Fiber 代码调用，需重新评估（20.5）。
2. **R3 例外**：`PeerHub` 的四个回调在 Hub 锁内执行，属明确例外；回调契约：
   - **禁止** park、阻塞（sleep、同步 IO、等待其他线程/锁）；
   - **禁止**重入 `RunOnce` / `RunFor`；
   - 允许调用 `Send*`（内部 `recursive_mutex` 同线程可重入）与短小纯内存操作；
   - 需要重活时，回调只入队，由 `RunOnce` 返回后的应用线程处理。
   `SessionControl::on_change` 与 `LogHook` sink 保持**锁外**执行，符合 R3。
3. **R5 落地**：本节即 ADR；失效后果：回调里 park 会阻塞 `RunOnce` 线程，该线程
   同时持有 peer 表锁 → 所有 peer 的收发/超时被拖住；若在回调里等待另一个持有
   Hub 锁的线程则直接死锁。

### 20.3 理由

- **独立性优先**：Bento 的卖点是「可单独拷进其他工程」（`GO2CPP_WITH_BENTO=OFF`
  不影响主库）；依赖 base 会破坏这一点。
- **回调一致性**：`PeerHub` 在锁内回调可直接读取 peer 状态并安全发送，避免复制
  快照/延长 peer 生命周期的复杂度；当前回调仅用于应用层转发（一行 lambda），
  收益大于风险。
- **可验证**：`test_log_hook` 的并发用例、`test_peer_hub` 的回调路径覆盖同线程
  重入；`SessionControl` 的 16 线程压测覆盖锁外回调。

### 20.4 后果

- Bento 调用方必须遵守上述回调契约；违反属**调用方缺陷**，库不做运行时断言
  （无 Fiber 运行时，无法检测 park）。
- `LogHook` sink 虽锁外执行，仍是**同步**调用；慢 sink 会线性拖慢 `RunOnce`，
  文档 19.5 已提示。
- 若未来引入 async sink / 多 listener / Fiber 支持，本 ADR 失效，需重开决策。

### 20.5 复审条件

- Bento 开始依赖 base（sync/scheduler/fiber）；
- 需要异步日志或回调锁外化；
- 在 Fiber 环境中调用 Bento，且临界区可能 park。

### 20.6 影响文件

`include/go2cpp/bento/net/session_control.hpp`（`std::mutex`）、
`include/go2cpp/bento/net/log_hook.hpp`（`std::mutex`）、
`include/go2cpp/bento/net/peer_hub.hpp`（`std::recursive_mutex`、回调约定）、
`cmake/Go2CppBento.cmake`（依赖只有 Threads/hash）。
