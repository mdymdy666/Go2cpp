# P1 三模块审查：SessionControl / PeerHub 加密 / LogHook（接口·功能·规范）

> **审查范围**：`session_control`（控制权仲裁）、`peer_hub` 的 PSK 加密与日志接入、`log_hook`（日志钩子）；
> 文档 `docs/modules/bento-scenarios.md`、`tests/bento/PROTOCOL.md`（是否需同步）。
> **基线**：HEAD `1e08723`（`feat(bento): 场景缺口补齐（控制权仲裁/多 peer 加密/日志钩子）`）。
> 工作树为**中间态**：`cmake/Go2CppBento.cmake` 有 +39 行未提交改动（并行任务新增 `transport` 目标），
> 部分 bento 文件仅 `100644→100755` 模式变化；本报告行号**以审查时工作树为准**。
> **方法**：静态阅读 + 实际运行 3 个新测试 + ctest 清单核对 + 独立小程序验证边界（只读，未改代码）。
> **结论口径**：P0 = 崩溃/安全/核心契约；P1 = 功能缺失/接口不可扩展/文档承诺不成立；P2 = 边界/可维护性/规范；P3 = 一致性小问题。

---

## 1. 结论摘要

| 维度 | 结论 |
|---|---|
| 可扩展性 | **状态机核心可复用，但「策略」不是可替换接口**：冲突策略是 `enum + if/else`（`session_control.cpp:331-352`），新增策略必须改 `RequestControl` 源码；**审计扩展要改公共结构体**（`ControlChange` 无 actor/时间戳/历史，`session_control.hpp:95-102`）。日志侧的事件枚举、单 sink 同样是固定形状。 |
| 功能点 | 控制权主路径（唯一控制器、抢占、租约、断开回收、变更回调）完整；**缺「申请-审批」状态、邀请式看控、审计历史、SessionControl 与 LogHook 的接线**；日志事件覆盖有 3 处盲区，另有一处重连后漏报。 |
| 使用规范 | 文档有 **6 处与实现/现状不一致**（9.1、9.5、18.5、7.2、19 节「不提交」、CMake 头注释），其中 9.1/9.5/18.5 是 P1 完成后未回改的过时描述。 |
| API 稳定性 | 与 Link/PeerHub 存在 **3 处风格分裂**：错误处理（SessionControl 全 bool 无原因）、JSON 导出（成员 vs 自由函数）、原因类型（枚举 vs `const char*`）。`Revoke()` 两个重载语义分叉，易误用。 |
| 验证 | `bento_test_session_control` 14816 checks PASS、`bento_test_log_hook` 124 checks PASS、`bento_test_peer_hub_secure` 重跑 506 checks PASS，**与文档 19.3 的三个数字完全一致**；Bento 测试数 26，与「26/26」的数量口径一致（未全量复跑，见 §6）。 |

**最需要先处理的一条**：`Grant(peer, kNone)` 之后该 peer **仍可通过 `RequestControl` 重新拿到控制权**，与「`kNone` = 无任何权限」的契约相反（已用独立程序复现，见 §3.1 D-01）。

---

## 2. 可扩展性评估

### 2.1 策略与扩展点表

| # | 扩展点 | 当前实现 | 可替换性 | 加「新策略 / 看控权限 / 审计」要改什么 |
|---|---|---|---|---|
| E-01 | **控制权冲突策略** | `enum class ControlPolicy {kFirstCome,kPreempt}`（`session_control.hpp:63-66`）+ `RequestControl` 内 if/else（`session_control.cpp:331-352`） | 不可外部注册、无策略接口/工厂 | 加 `kApproval`/`kRoundRobin` 等：改枚举 + 改 `RequestControl` 函数体 + 可能加 `SessionControlConfig` 字段 + 改文档/测试（≥4 处，且策略逻辑与状态机耦合在同一函数） |
| E-02 | **控制权授予路径** | `Grant(peer,mode)` 的 `kInteract` 分支**无条件抢占**，不受 `policy` 约束（`session_control.cpp:200-220`）；`Join` 的 `default_mode=kInteract` 也不看 policy（`:126-128`，实测 preempt 策略下第二个 Join 仍是 View） | 三条入口各自隐含一套规则，无统一「授予决策」抽象 | 想统一「谁能获得控制权」需重构三处（Join/Grant/RequestControl），并新增「授予来源」概念；否则策略语义永远分裂 |
| E-03 | **租约/时钟** | 租约起算全部硬编码 `NowMs()`（`session_control.cpp:134,216,320,327,346,363,410`）；仅 `Tick(now_ms)` 可注入 | 半可测：`Tick` 可注入，但 `RequestControl` 写 deadline 的时钟不可注入 | 要做确定性单测/仿真需给所有入口注入 clock（改 4 个公开方法签名或引入 Clock 成员） |
| E-04 | **变更回调（审计/广播入口）** | 单个 `std::function<void(const ControlChange&)> on_change`（`session_control.hpp:204`） | 单订阅；运行期替换无同步（头文件已声明需外部保证） | 审计 + 广播 + 指标三个消费者需应用自行组合 lambda；加「多监听器」要改成员类型并补并发语义 |
| E-05 | **审计信息** | `ControlChange` 只有 `peer_id/from/to/reason/controller/generation`（`session_control.hpp:95-102`），无 **actor（谁授权/撤销）**、无 **timestamp**、无历史缓冲 | 审计只能靠应用在回调里另存 | 加 actor/timestamp = 改**公共结构体**（所有回调签名使用者重编译）；历史查询 = 新增环形缓冲/查询 API；`ControlChangeReason` 加新原因 = 改枚举 + `ControlChangeReasonName` switch |
| E-06 | **看控权限（邀请/审批）** | `ControlMode{kNone,kView,kInteract}`；`Join` 直接入会并给默认权限（`session_control.cpp:111-142`），无邀请令牌、无 pending 申请状态 | 现模型是「已在会内的人互相仲裁」，不是「邀请制」 | 邀请/审批需新增 `Invite/Accept/Reject` 与 `pending` 状态、扩展 `ControlChangeReason`、扩展 `ControlSnapshot`；`Join` 语义会变（可能要带 token） |
| E-07 | **日志事件集合** | 固定 `enum class LogEvent` 14 项 + `kLogEventCount`（`log_hook.hpp:56-76`）；名字 switch 与计数数组按枚举大小静态分配 | 新增事件必须改 3 处（枚举、`LogEventName` switch、数组上界） | 自定义业务事件无法在不改库的情况下加入；替代方案是复用 `value`/message 或另建业务日志（无扩展点） |
| E-08 | **日志 sink** | 单个 `Sink`（`log_hook.hpp:153`）+ `SetSink`（`:163-166`） | 单目的地；无过滤谓词、无采样、无异步 | 多目的地（文件+指标+控制台）需应用组合；加「只接收某 peer/某事件」需在 sink 内过滤，库不提供 |
| E-09 | **加密算法** | ChaCha20-Poly1305 + 32B PSK 固定（`kPskBytes`）；`SetPreSharedKey` 只接受 32B（`peer_hub.hpp:226`） | 无算法协商/注册 | 换算法或换密钥长度要改 `PeerHub`/`Link` 公共接口（与既有设计一致，非本轮引入，但 PeerHub 新 API 沿用了该限制） |
| E-10 | **多 peer 准入/限流** | `max_peers` + `accept_unknown_peers`（`peer_hub.hpp:84-88`）；无 per-IP 限额、无 peer 优先级 | 只有总数上限 | 每 IP 限流/优先级要在 `HandleHelloPacket` 的准入分支加钩子/配置（当前无钩子） |
| E-11 | **PeerHub 日志注入** | 裸指针 `LogHook*`（`peer_hub.hpp:105`）+ `SetLogHook`（`:247`）+ 私有 `EmitLog`（`peer_hub.cpp:766-771`） | 单 hook、同步调用；`EmitLog` 自身不加锁，依赖调用方持 `m_mutex` 的**隐式契约** | 多 sink/异步需在 `LogHook` 层解决；新增调用点忘记持锁即产生数据竞争（无断言/文档约束） |
| E-12 | **状态/统计序列化** | `ControlSnapshot::ToJson()` 成员函数（`session_control.hpp:120`，实现 `session_control.cpp:54-73`） | 可用；但与 Link 的 `LinkStatsToJson` 自由函数风格不一致（见 §5） | 统一风格需二选一改名/迁移；字段扩展（如 actor）要同步改手写 JSON |

**判断**：`SessionControl` 把「仲裁状态机」与「策略」写在同一个 `RequestControl` 里，**策略只能通过改库源码扩展**；「审计」也不是预留点，而是需要动公共结构体。若近期确定要加「看控权限/审计」，建议先定接口（`IControlPolicy` / `ControlListener` / `ControlChange.actor+timestamp`），否则后续是破坏性改动。

### 2.2 硬编码与隐式契约

| 位置 | 硬编码/隐式规则 | 影响 |
|---|---|---|
| `peer_hub.cpp:1050` | `strcmp(reason, "peer timeout")` 决定发 `kPeerTimeout` 还是 `kPeerDisconnected` | 新增 reason 字符串（如 "hello timeout"）无法表达为超时事件；事件分类依赖字面量 |
| `peer_hub.cpp:1492-1497` | hello 超时统一 `DropPeer(peer,"hello timeout")` → 实际发 `kPeerDisconnected` | `LogEvent::kHandshakeFailed`（注释「超时上限/参数非法」）在握手超时时**不会发出**，只有 max peers 一处发（`:1329`） |
| `session_control.cpp:331-352` | 策略分支与状态迁移混写 | 新策略必须复制/改写整段迁移逻辑，易漏 `generation`/统计/事件顺序 |
| `session_control.cpp:126-128` | Join 时 `default_mode=kInteract` 且已有控制器 → 强制降 View | `kPreempt` 下 Join 也不抢占（实测 preemptions=0），策略作用域不完整 |
| `peer_hub.cpp:229-239` | `Close()` 直接清 peer 表，不触发 `on_peer_disconnected` / 日志 | 与 `RemovePeer`（`:617-628` 经 `DropPeer` 触发回调）语义不一致；调用方无法区分「批量关闭」与「逐个移除」 |
| `peer_hub.cpp:299-305` | `ResetChannels` 不清 `Peer::log_retransmits/log_loss_events`（`:163-164`） | 重连后新通道统计从 0 开始，旧基线仍保留 → 重传/丢包事件**漏报**，直到新计数超过旧基线（见 D-08） |
| `peer_hub.cpp:766-771` | `EmitLog` 不加锁，约定调用方持 `m_mutex` | 新增调用点若在锁外会与 `SetLogHook` 竞争（当前所有调用点均在锁内，属隐式契约） |

---

## 3. 功能缺口清单

### 3.1 控制权（对照场景矩阵 3.4 / 7.1–7.3）

| ID | 严重度 | 缺口 | 证据 | 影响 |
|---|---|---|---|---|
| D-01 | **P1（安全/契约）** | `Grant(peer, kNone)` 收回权限后，该 peer 仍可直接 `RequestControl` 成功 | 文档承诺 `kNone` = 「未加入/无权限」（`session_control.hpp:11-12,54`）、「收回权限但保留参与者记录」（`:156-160`）；但 `RequestControl` 只检查「是否在表内」和 `revoked`（`session_control.cpp:305-315`），不检查 `mode==kNone`；`Grant` 在 `:199` 把 `revoked` 清掉 | 被管理员降为 none 的 peer 可自行夺回控制权（同样的 `kNone` 语义在 `Grant` 里被当作「可恢复的降级」，在请求路径里被当作「可忽略」）。独立复现：`Grant(1,kNone)` 后 `RequestControl(1)` 返回 1、`controller=1`、`denials=0` |
| D-02 | **P1（功能）** | 无「申请-审批」状态：`RequestControl` 即时成功/失败，无法表达「申请中」 | `session_control.hpp:170-174`；`ControlChangeReason` 无 pending 语义 | JinShan 的「主控请求、被控端批准」流程只能由应用在会话外自己做，仲裁器不参与，无法防止并发申请/超时撤销 |
| D-03 | **P1（可扩展）** | 无审计：事件没有 actor（谁发起的 Grant/Revoke）、没有时间戳、没有历史 | `session_control.hpp:95-102` | 出问题只能看到「状态变了」，无法回答「谁在何时授权/撤销」；与「审计」需求直接冲突（见 E-05） |
| D-04 | P2 | 无「邀请制看控」：`Join` 即入会并拿到 `default_mode`，无鉴权/邀请令牌 | `session_control.cpp:111-142` | 任何知道 peer_id 的调用方都能 Join；是否允许看由应用自己先判断，仲裁器不承载 |
| D-05 | P2 | 变更广播只提供数据，不提供发送/序列化辅助：`ControlChange` 无 `ToJson`，只有 `ControlSnapshot::ToJson` | `session_control.hpp:94-121`；`session_control.cpp:54-73` | 广播「谁抢了控制权」需要应用自己拼 JSON；文档 7.3 只说到「遍历 PeerIds 发送」（`bento-scenarios.md:94`），没有可复制代码 |
| D-06 | P3 | `Revoke()` 与 `Revoke(peer_id)` 语义分叉：前者要求存在控制器，后者只标记禁入 | `session_control.cpp:238-263` vs `:265-294`；前者返回 false 当无控制器，后者对任意在表 peer 返回 true | 同名重载行为差异大，调用方易误用；建议改名（如 `RevokeCurrent`/`BanPeer`），但属 API 变更 |
| D-07 | P3 | `Renew` 在 `lease_ms==0`（永不过期）时对控制器也返回 false | `session_control.cpp:358-365`；文档只说「非控制器返回 false」（`session_control.hpp:176-177`） | 调用方把 false 当「失去控制权」，在「不过期」配置下误判 |

### 3.2 多人 / 加密（对照 3.1–3.6、9.4–9.5）

| ID | 严重度 | 缺口 | 证据 | 影响 |
|---|---|---|---|---|
| D-08 | P2 | 日志增量基线不随通道重置清零 → 重连/`Connect` 后重传/丢包事件漏报 | 基线在 `Peer::log_retransmits/log_loss_events`（`peer_hub.cpp:163-164`）；`ResetChannels` 只清通道（`:299-305`）；Tick 以 `stats().retransmits > last` 判定（`:1529-1545`） | 重连后新通道统计从 0 开始，旧 last 可能已很大；在追上旧值前完全静默。属「静默失效」，排障时最危险 |
| D-09 | P2 | 监听端 per-peer PSK 只能继承 Hub 默认 PSK | `HandleSecureHello` 用 `m_psk` 解 HELLO（`peer_hub.cpp:1282`）；`m_secure_enabled==false` 时直接拒（`:1218-1224`） | 一个监听端口服务多个不同 PSK 的客户端做不到；文档 19.5 已承认，但场景 9.4 的「per-peer 不同 PSK」只覆盖主动端，表格易被读成双向支持 |
| D-10 | P2 | 多 peer 下地址迁移无专项测试 | 代码按 conn_id 更新地址（`peer_hub.cpp:1203-1207`，加密路径 `:1181-1184`）；`tests/bento` 无对应场景 | 场景 10.3 仍是「部分」；NAT rebinding 回归无保护 |
| D-11 | P3 | 每 IP 连接数限流 / 全局背压缺失 | `peer_hub.hpp:84-88` 只有 `max_peers` | 文档 3.3/3.5/8.3 已列「部分」；单机被同 IP 打满 peer 表时无区分 |
| D-12 | P3 | `Close()` 不通知 peer 断开（见 2.2） | `peer_hub.cpp:229-239` | 应用若依赖 `on_peer_disconnected` 清理会话，`Close` 路径会漏清理 |

### 3.3 日志（对照 12.5 / 19.1）

| ID | 严重度 | 缺口 | 证据 | 影响 |
|---|---|---|---|---|
| D-13 | **P1（功能）** | `LogEvent::kHandshakeFailed` 实际只在「max peers 拒绝」一处发出；握手超时/参数非法不上报该事件 | 全库唯一发出点 `peer_hub.cpp:1329`；hello 超时走 `DropPeer` → `kPeerDisconnected`（`:1492-1497`, `:1050-1053`） | 文档 12.5（`bento-scenarios.md:147`）和 19.1 声称「握手失败已接入」，实际握手超时只能看到 `peer_disconnected`；排障时分不清「对端未响应」与「正常断开」 |
| D-14 | P2 | 帧解析失败、发送封装失败无日志事件 | `HandlePacket` 明文 `ParseFrame` 失败仅计数（`peer_hub.cpp:1198-1201`）；加密内层 `ParseFrame` 失败仅计数（`:1176-1179`）；`SealAndSend` 失败仅计数（`:434-447`） | `kFrameRejected` 只在帧头解码失败（`:1122-1125`）和「明文 peer 收到加密帧」（`:1191-1196`）发出；`kSendError` 只在 `sendto`/握手封装失败发出。文档 12.5「帧格式/长度非法」「收发错误」覆盖不完整 |
| D-15 | P2 | `SessionControl` 未接 `LogHook`：`kControlChange` 是预留死枚举，无调用、无辅助函数 | `log_hook.hpp:71` 注释「由 SessionControl 上层接入」；全库无 `kControlChange` 使用（grep 仅定义/命名） | 应用要自己写 10 行映射（reason → message → `hook.Info`），且没有示例；文档 12.5 未把该事件算进「已接入」 |
| D-16 | P2 | 无异步/采样/墙钟：sink 在 `RunOnce` 线程同步调用；`timestamp_ms` 是 `NowMs()` 单调时钟 | `log_hook.hpp:19-23,136,206`；PeerHub 在锁内发事件（`peer_hub.cpp:1525-1546`） | sink 慢会直接拖慢 `RunOnce`；单调时钟无法与外部日志/审计系统对齐（对比握手用 `WallClockMs`，`peer_hub.cpp:1392`）。文档 19.5 只说「事件是批而不是逐帧」，未警告 sink 阻塞风险 |
| D-17 | P3 | 未注入 sink 时 `per_level/per_event` 不累计，只有 `filtered` | `log_hook.hpp:192-196`；测试断言 `Count(kRetransmit)==0`（`test_log_hook.cpp:120`） | 与头文件第 12 行「未注入时所有 Emit 只做计数」的表述不符（见 §4 DOC-07） |
| D-18 | P3 | 事件测试未覆盖 `kHandshakeFailed`、`kFrameRejected`、`kPlaintextRejected` 的断言 | `test_log_hook.cpp` 场景 3–6 只断言 Connected/HandshakeRetry/Retransmit/PacketLost/PeerTimeout/AuthFailure | 上述覆盖盲区（D-13/D-14）没有测试兜底 |

---

## 4. 文档 / 规范不一致清单

> 行号以工作树为准（`bento-scenarios.md` 内容与 HEAD 一致，仅 mode 变化）。

| ID | 严重度 | 位置 | 声称 | 实际 | 建议 |
|---|---|---|---|---|---|
| DOC-01 | **P1** | `docs/modules/bento-scenarios.md:112`（9.1） | 「仅 `Link` 支持；`PeerHub` 未接 PSK」 | `PeerHub::SetPreSharedKey/SetPeerPreSharedKey` 已实现（`peer_hub.hpp:221-243`，`test_peer_hub_secure` PASS） | 改为「Link 与 PeerHub 均支持；PeerHub 监听端 per-peer PSK 受限（见 19.5）」 |
| DOC-02 | P1 | `bento-scenarios.md:116`（9.5） | 「`PeerHub` 无鉴权」 | 已有 PSK 认证与 `PeerSecureStats.auth_failures` | 改为「有 PSK 鉴权；配对/多 key 策略在应用层」 |
| DOC-03 | P1 | `bento-scenarios.md:280`（18.5） | 「`PeerHub` 未接 PSK/0-RTT/VIDEO/PMTUD」 | PSK 已在 19.2 补齐；19.5 已修正为「0-RTT/VIDEO/PMTUD」 | 18.5 补「PSK 已于 19.2 补齐」或删除该项，避免 P0 记录与当前能力冲突 |
| DOC-04 | **P1** | `bento-scenarios.md:93`（7.2） | 「`PeerHub::on_peer_disconnected` **直接接入**」 | 签名不匹配：`on_peer_disconnected(uint32_t, const char*)`（`peer_hub.hpp:265`）vs `SessionControl::OnPeerDisconnected(uint32_t)`（`session_control.hpp:151`）；必须写 lambda 适配 | 改为「需一行 lambda 适配（示例：`[&](uint32_t id, const char*){ sc.OnPeerDisconnected(id); }`）」；同一行后半句「仍需应用一行回调」与前半句自相矛盾，统一措辞 |
| DOC-05 | P2 | `bento-scenarios.md:289-291`（19 节引言） | 「不改 base、**不提交**」 | HEAD `1e08723` 已包含全部 P1 文件（`git show --stat 1e08723`） | 改为「已于 1e08723 提交」 |
| DOC-06 | P2 | `cmake/Go2CppBento.cmake:2-14` | 「12B 帧头」「协议 v0.1」「只构建 Bento 与它的 **3+1 个测试目标**」 | 实际 16B/v2（`tests/bento/PROTOCOL.md:3-5`）；当前 26 个测试目标（工作树再增 transport 后 27） | 更新头注释为 16B/v2，删除固定目标数 |
| DOC-07 | P2 | `include/go2cpp/bento/net/log_hook.hpp:12` | 「未注入时所有 Emit **只做计数**，零字符串格式化、零分配」 | 无 sink 时只 `++filtered` 后返回（`:192-196`），`per_level/per_event` **不累计**；测试明确断言 `filtered=1, emitted=0`（`test_log_hook.cpp:84-86`）且 `Count(kRetransmit)==0`（`:120`） | 改为「未注入时只累计 filtered 计数」；若要「事件计数与 sink 解耦」，需改实现（P2 设计问题） |
| DOC-08 | **P1** | `bento-scenarios.md:147`（12.5）+ `:301`（19.1） | 「连接、握手重传、可靠层重传、丢包、认证失败、超时、明文拒绝均已接入」 | 有测试覆盖的 6 类成立；但「握手失败」只在 max peers 一处（D-13），「帧格式非法」只覆盖帧头（D-14），「收发错误」只覆盖 sendto/握手封装 | 把 12.5 从「已补」改为「已补（部分事件，缺口见 P2-D13/D14）」，或在 19.5 增加已知限制条目 |
| DOC-09 | P2 | `docs/review/00-concurrency-policy.md:13-15,21-23` | 「新增代码禁止裸 `std::mutex`」「回调一律锁外执行」；例外必须写 ADR | 三个新模块全部使用裸 `std::mutex`（`session_control.hpp:225`、`log_hook.hpp:269`）与 `std::recursive_mutex`（`peer_hub.hpp:335`）；`PeerHub` 的 `on_stream/on_peer_connected/on_peer_disconnected` 在持锁下调用（`peer_hub.cpp:1037-1039,1054-1056`、`:334,365`）；仓库无 ADR | Bento 不依赖 base（`cmake/Go2CppBento.cmake:52` 只链 Threads），属结构性例外；应在 **19.5 或 ADR** 明确「Bento 豁免 R1，`PeerHub` 回调持锁且禁止 park/阻塞」，否则政策与实现长期冲突 |
| DOC-10 | P2 | `codex.md`（项目 AI 协作记录） | 全局 AGENTS 要求项目型任务写入协作记录 | `codex.md` 全文无 `Bento`/`peer_hub`/`session_control`/`LogHook` 记录；P1 只在 commit message 与 `bento-scenarios.md` 19 节留痕 | 追加一节 Bento P1（时间/改动/关键文件/测试结果/后续状态），否则下一个 agent 无法从协作记录发现该工作 |
| DOC-11 | P3 | `tests/bento/PROTOCOL.md:8-10` | 工作树 `/mnt/e/CodexWorkspace/Go2Cpp`、构建目录 `/tmp/build-bento` | 当前工作树 `/UserData/CodexWorkSpace/Go2cpp`，本环境共享 `build-review` | 历史文档可保留，但补充「路径为当时环境」；PROTOCOL.md 未记录 P1 三模块（不改线格式，可接受），建议加一句「P1 不改线格式」 |
| DOC-12 | P3 | `bento-scenarios.md:57`（3.6）、`:124`（10.3） | 把 `PeerHub::DropPeer`、`PeerHub::HandlePacket` 当「现有能力（文件:接口）」列出 | 两者都是 **private**（`peer_hub.hpp:301,316`） | 改为「内部机制：`DropPeer`（peer_timeout 触发）」，避免读者以为可调用 |
| DOC-13 | P3 | `bento-scenarios.md:287-340`（19 节） | 落地记录 | **没有任何可运行示例**（只有表格、断言数、限制） | 「示例是否可跑」无从验证；建议补 §19.6 最小接线示例（SessionControl + PeerHub 断开回调 + 广播），并标注编译命令 |
| DOC-14 | 正面 | `bento-scenarios.md:197-222`（16 节） | 66 = 已有 30 + 已补 4 + 部分 22 + 缺 0 + 不实现 1 + 不适用 9 | `grep` 复核完全一致（66 行，分项 30/4/22/1/9） | 保持；该自检可信 |

---

## 5. API 稳定性（与 Link / PeerHub 风格对照）

| 维度 | SessionControl | PeerHub / Link | 结论 |
|---|---|---|---|
| 错误处理 | 全部 `bool`，无 error/reason 输出；`Join` 的 false 有 3 种含义（0/重复/超限） | `Open(std::string* error)`、`AddPeer(..., std::string* error)`（`peer_hub.hpp:175,183`） | **不一致**；文档 12.4 已列「结构化错误码缺失」，但 P1 新 API 没有预留字段，未来加错误码要改全部返回值（破坏性） |
| JSON 导出 | `ControlSnapshot::ToJson()` 成员函数（`session_control.hpp:120`） | `LinkStatsToJson(const LinkStats&)` 自由函数（`link.hpp:258`） | **不一致**；同类能力两种形态 |
| 原因/错误类型 | `ControlChangeReason` 强类型枚举（`session_control.hpp:69-78`） | `on_peer_disconnected(peer_id, const char* reason)`（`peer_hub.hpp:265`） | **不一致**，且同一批 P1 模块内；字符串 reason 让应用无法稳定分支（文档 12.4 已承认） |
| 命名 | 动作 Pascal（`Join/Grant/Revoke/RequestControl/Tick`）+ 查询 snake（`mode/controller/participants`）+ 返回结构体 Pascal（`Snapshot`） | 同款（`Open/Close/AddPeer` + `peer_state/Peers/PeerIds`） | 一致；`*Name` 函数（`ControlModeName`/`PeerStateName`/`LogLevelName`）风格统一，**好** |
| 统计 | `SessionControlStats` + `stats()` | `PeerHubStats`/`PeerSecureStats` + `stats()`/`secure_stats()` | 一致；`*_ms`、`*Stats` 命名统一 |
| 重载语义 | `Revoke()` = 撤销当前控制器；`Revoke(peer)` = 禁入标记；`Grant(peer)` = Grant(kView) 并解除禁入；`Grant(peer,kInteract)` = 无条件抢占 | — | **风险点**：同名重载语义差异大（D-06）；`Grant(kInteract)` 绕开 policy 是文档化的管理操作，但与「策略」概念并列时容易误解 |
| 生命周期 | `on_change` 是 public `std::function`（与 PeerHub 回调一致） | 同款 | 一致；但 SessionControl 回调在**调用者线程/锁外**，PeerHub 回调在 **RunOnce/锁内**，两者线程语义不同，文档只分别描述、未并排对比 |
| 死代码/误导注释 | `was_revoked` 被赋值为 `(void)was_revoked;` 后弃用（`session_control.cpp:277,289`），注释却称「幂等：已经处于撤销态时不再重复回调」 | — | P3：注释描述的机制不是由该变量实现的（实际靠「非控制器路径不 AppendChange」）；删除死变量或让注释与实现对齐 |
| 悬垂风险 | — | `PeerHubConfig::log_hook` 非拥有指针（`peer_hub.hpp:104-105`） | 文档已写「生命周期由调用方保证」；`LogHook` 头文件也写 message 只在回调期间有效（`log_hook.hpp:25-26`），**好** |

**稳定性总评**：命名与统计风格稳定；**错误/原因/JSON 三个横切面不统一**。如果下一轮要统一，建议顺序：① 给 SessionControl 的失败路径补原因（可先加 `std::string* error` 兼容既有 bool）；② 统一 `reason` 类型（枚举 + `const char*` 双出口）；③ JSON 统一为自由函数（`ControlSnapshotToJson`）。

---

## 6. 验证记录（可复现）

| 验证项 | 命令 | 结果 |
|---|---|---|
| `bento_test_session_control` | `./build-review/bento_test_session_control` | PASS，**14816 checks**（文档 19.3 同为 14816）；16 线程 winner=1；4000 步随机不变式成立 |
| `bento_test_log_hook` | `./build-review/bento_test_log_hook` | PASS，**124 checks**（文档 19.3 同为 124）；handshake_retry=7、retransmit=5、loss=1、timeout=1、auth_failure=7 |
| `bento_test_peer_hub_secure` | `./build-review/bento_test_peer_hub_secure` | **首次 FAIL**（`test_peer_hub_secure.cpp:399,405`）+ core；**重跑 PASS 506 checks**（文档 19.3 同为 506）；`stream=90 dgram=60`、`auth_failures=5`、`per_peer_psk rx=10/10`、`retransmits=32 rx=200` 均与文档一致 |
| 首次失败归因 | `stat build-review/bento_test_peer_hub_secure` | 二进制 mtime `11:51:13` 在运行期间被并发重建（`build-review` 为其他审查任务共享目录），判断为构建/运行重叠导致的二进制替换，**不是可稳定复现的测试缺陷**；但说明共享构建目录下运行测试有风险，CI 应独立目录 |
| ctest 数量 | `ctest -N` | Bento 测试 15–40 共 **26 个**，与文档「ctest 26/26」数量口径一致；**未全量复跑**（避免与并行任务争用共享目录），只复跑 3 个新增测试 |
| 边界复现程序 | `g++ -std=c++17 ... /tmp/sc_gap_check.cpp` | ① `Grant(1,kNone)` 后 `RequestControl(1)=1`、`controller=1`、`denials=0`（D-01 确认）；② `policy=kPreempt + default_mode=kInteract` 下 `Join(2)` 仍为 View、`preemptions=0`（E-02 确认）；③ 对非控制器重复 `Revoke` 不累加 `revocations`（D-06 旁证） |
| 未做 | — | TSAN/ASAN、全量 ctest、`-Werror` 全量重建（`build-review` 已被其他任务构建）、真实 netem 弱网（无 CAP_NET_ADMIN） |

**文档断言数可信度**：19.3 的三项断言数（14816/124/506）与实测**完全一致**，说明落地记录不是编造的；但 19.3 的「ctest 26/26」未全量复验，且「全量 0 警告」未获得构建日志。

---

## 7. 建议优先级

| 顺序 | 事项 | 类型 | 关联 |
|---|---|---|---|
| 1 | 修 `Grant(kNone)` 后可 `RequestControl` 的契约漏洞（拒绝 `mode==kNone` 的请求，或让 `Grant(kNone)` 同时置 `revoked`） | 功能/安全 | D-01 |
| 2 | 补审计字段（`ControlChange.actor/timestamp`）与历史查询；在此之前明确「审计由应用回调自存」的文档承诺 | 可扩展 | E-05/D-03 |
| 3 | 把冲突策略抽成可替换接口（或至少在 `RequestControl` 外暴露 `Decide()`），并为 Join/Grant 统一授予规则 | 可扩展 | E-01/E-02 |
| 4 | 修正文档 9.1/9.5/18.5/7.2/19 节「不提交」与 CMake 头注释 | 文档 | DOC-01…06 |
| 5 | 补齐日志事件覆盖（握手超时→`kHandshakeFailed`、解析/封装失败→`kFrameRejected/kSendError`、通道重置清基线） | 功能 | D-08/D-13/D-14 |
| 6 | 写 Bento 并发例外 ADR（裸 mutex + PeerHub 锁内回调），或改回调到锁外 | 规范 | DOC-09 |
| 7 | 在 `codex.md` 追加 P1 落地记录；补 §19.6 最小接线示例 | 规范/可用性 | DOC-10/DOC-13 |
| 8 | `SessionControl` 接入 `LogHook`（提供 `LogControlChange` 辅助），补 SessionControl↔PeerHub 联调测试 | 功能/测试 | D-15/D-04 |

---

## 8. 附：本次审查覆盖的文件

- `include/go2cpp/bento/net/session_control.hpp`（228 行）、`src/bento/net/session_control.cpp`（468 行）
- `include/go2cpp/bento/net/log_hook.hpp`（275 行）
- `include/go2cpp/bento/net/peer_hub.hpp`（338 行）、`src/bento/net/peer_hub.cpp`（1603 行；重点 380–500 / 740–780 / 1025–1060 / 1119–1212 / 1214–1460 / 1490–1560）
- `tests/bento/test_session_control.cpp`（417）、`test_log_hook.cpp`（369）、`test_peer_hub_secure.cpp`（627）、`test_peer_hub.cpp`（552，抽读）
- `docs/modules/bento-scenarios.md`（340）、`tests/bento/PROTOCOL.md`（抽读）、`cmake/Go2CppBento.cmake`（194）、`docs/review/00-concurrency-policy.md`（128）、`codex.md`（检索）
