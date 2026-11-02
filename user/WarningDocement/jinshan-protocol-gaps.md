# JinShanProject 新协议 → Bento 通用能力规划

> 日期：2026-10-29。来源：`E:\CodexWorkspace\JinShanProject\docs\05-detailed\protocol.md` v1.1
> （2026-10-28）、`03-architecture/architecture.md` v1.0、`05-detailed/error-codes.md`、`config.md`。
> 目的：把 JinShan 真实场景的协议需求，映射为 Bento 的通用能力（后续实现，不在本批次）。

## 1. JinShan 新协议要点（RDRL v1 + RDCH）

| 项 | 内容 | 通用化价值 |
|---|---|---|
| **RDRL 帧** | magic `RDRL`、version=1（不协商降级）、payload ≤64KiB、类型 1-8（REGISTER/CONNECT/RESPONSE/EVENT/DATA_OPEN/DATA_CONNECT/…）+ **CTRL(9)** | 握手/控制帧格式，通用「控制面协议」 |
| **RDCH 帧** | magic `RDCH`、payload ≤4MiB（实际 ≤48KiB）、应用层消息（能力握手/命令/文件） | 通用「应用面上层协议」 |
| **token 管道配对** | relay 生成 8 字节随机 token → `(key, token)` 把 server DATA 与 viewer DATA 1:1 配对；token 不落日志 | 通用「1:N 中转的管道句柄」 |
| **配对四段式** | CONTROL 注册 → 申请 token → 双端 DATA 配对 → RDCH HELLO 能力握手 | 通用「连接编排」 |
| **背压** | 每 DATA 方向 32KiB 拷贝 + 256KiB 有界队列 + 30s 写 deadline；慢 peer 只断自身管道 | 通用「转发背压」（Bento relay 已有） |
| **109 错误码** | `ERR-PROTO-xxx`/`ERR-AUTH-xxx` 等分层错误码 | 通用「错误码注册表」（Bento error 已有） |
| **key_id** | `SHA-256(key)` 前 4 字节 = 8 hex，唯一允许日志形态 | 通用「密钥脱敏」（Bento keystore 已有） |
| **不广播** | 每 DATA 只服务一个 Viewer，禁止 RDP 字节广播/复制 | 通用「1:N 独立通道」（Bento peer_hub 已有） |

## 2. Bento 现状映射

| JinShan 需求 | Bento 现状 | 覆盖度 |
|---|---|---|
| RDRL/RDCH 帧 | `net::Frame`（20B 头 BENT）+ `FrameFormat` 参数化（SACK 扩展时加入） | 可复用，缺 RDRL 布局与双帧切换 |
| token 管道配对 | `peer_hub`（conn_id）+ `relay`（双向转发） | **缺 token**（无 `(key, token)` 配对） |
| 配对四段式 | `pairing`（Register/Connect/Response/Activate）+ `session`（编排） | 三段覆盖，缺 token 段 |
| 能力握手（RDCH HELLO） | `link` HELLO/ACK + caps（2B 能力位） | 有雏形，缺 RDCH 应用层消息 |
| 背压 32KiB/256KiB | `relay` 64KiB 有界 + 慢客户端 | ✅（参数可调） |
| 109 错误码 | `bento::error` Registry（gRPC 风格 + 注册） | 可注册，缺映射表 |
| key_id | `keystore` Masked（前 4 后 4） | 可加 key_id（SHA-256 前 4B） |

## 3. 未来通用能力（实现计划）

| 能力 | 建议模块 | 接口草案 | 优先级 |
|---|---|---|---|
| **token 管道配对** | `bento::net::TokenPairing`（或并入 `peer_hub`） | `Pair(key, token, peer) → slot`；`Release(token)`；`List(key)` | P1 |
| **RDRL/RDCH 双帧** | `bento::net::FrameFormat`（已参数化）+ `RdrlFormat`/`RdchFormat` | `FrameFormat{magic, version, max_payload}` | P1 |
| **应用层消息（RDCH）** | `bento::net::RdchMessage`（类型/长度/能力位） | `Encode/Decode` + 能力握手 | P2 |
| **错误码映射** | `bento::error::Mapping`（已有） | `JinshanErrorCodes` 注册表（109 个） | P2 |
| **key_id** | `bento::net::keystore` | `Key::KeyId()`（SHA-256 前 4B hex） | P2 |

## 4. 约束（与 JinShan 对齐）

- RDRL version 不协商（不等即拒）；扩展只允许加类型/错误码/可选尾部；
- token 不落日志/UI/错误消息；只出现 key_id/session_id/peer_id；
- 1:N 独立 DATA（禁止广播）；
- 背压有界（32KiB 拷贝 + 256KiB 队列）。

## 5. 状态

- [ ] token 管道配对（P1）
- [ ] RDRL/RDCH 双帧（P1）
- [ ] RDCH 能力握手（P2）
- [ ] 109 错误码映射（P2）
- [ ] key_id（P2）

> 本文件只记录规划，不实现；实现时在各模块 `audit-*.md` 中跟踪。
