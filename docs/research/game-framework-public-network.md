# 游戏框架公网同步策略（源码级调研）

> 调研对象：Unity Netcode for GameObjects（NGO，默认分支 HEAD `5294b15`，2026-11-08 抓取）、
> Unity `EntityComponentSystemSamples` 的 `NetcodeSamples`（`master`）、Valve `source-sdk-2013`（`master`）。
> Go2Cpp 基线：`/UserData/CodexWorkSpace/Go2cpp` HEAD `a0126318`。**只读调研，未改代码**；行号对应上述抓取版本。
> Unity `com.unity.netcode`（Netcode for Entities）核心包不开源，相关能力只能从 samples 反推，见 §10。

## 0. 三句话结论

1. 游戏同步 = **固定 tick 的状态机**：服务器以 30/60 Hz 跑 tick、按 tick 发快照；客户端把「服务器时间」当唯一时间轴。
2. **插值卖延迟、预测卖响应**：插值窗口 ≈ tick 延迟（NGO 1–2 tick + 用户 offset）；预测靠「输入缓冲 + 服务器 ack + 未确认输入重放」。
3. delta 压缩的本质是 **baseline + 存在位**（NGO `m_PreviousValue`、Source `WriteUsercmd(to, from)`），不是通用二进制 diff。

## 1. 传输层抽象：`NetworkTransport`

**核心思想**：传输层只暴露「消息」语义，不暴露字节流——`Send(clientId, payload, NetworkDelivery)` + `PollEvent(out clientId, out payload, out receiveTime)`，消息边界和投递等级由传输层负责。
**源码**：`Runtime/Transports/NetworkTransport.cs:62`（Send）、`:71`（PollEvent）、`:101`（GetCurrentRtt）；`UnityTransport` 是 UTP 适配器（`Runtime/Transports/UTP/UnityTransport.cs:530`）。
**可借鉴**：Go2Cpp 的 `rpc::Transport` 只有 `Write/Read/Close/Fd`（`include/go2cpp/rpc/transport.hpp`），连「一条消息」都不存在。游戏层要的不是又一个 RPC，而是**在 mux 流上加「带 delivery 的消息帧」**：`FrameType` 增加 `delivery` 字段，发送侧按 delivery 分队列（NGO `NetworkMessageManager.cs:72` 的 `m_SendQueues` 就是 per-client 的 `List<SendQueueItem>`）。

## 2. 快照频率：tick 驱动 + 时间基座

**核心思想**：默认 **30 Hz**（`NetworkConfig.cs:45` `TickRate = 30`），所有同步都挂在 tick 上；时间同步每秒 1 次、走 `Unreliable`（`NetworkTimeSystem.cs:23`、`:170`），客户端本地维护 `ServerTime` 与 `LocalTime` 两个时钟。
**源码**：`NetworkTimeSystem.cs:250-253`——`ServerTime offset = serverTime - localTime - ServerBufferSec(50ms)`，`LocalTime offset` 再 `+ RTT/2 + LocalBufferSec`（客户端把本地时间「拨快」半个 RTT，让输入更早到服务器）；`:204-225` 用 1%/s 的 `AdjustmentRatio` 平滑追时间，偏差 > 0.2 s 才硬重置。
**可借鉴**：Go2Cpp 现在没有任何「世界时间」概念（grep `interpolat|extrapolat|predict|lag comp` 全空）。**先做时间基座**：`ServerTime / LocalTime / TimeTicksAgo(n)` 三个 API，比先做插值更基础——插值、lag comp、预测都依赖它。

## 3. 快照插值：`NetworkTransform` + `BufferedLinearInterpolator`

**核心思想**：收到快照不立刻用，放进按 `TimeSent` 排序的缓冲；渲染时用「服务器时间倒退 tickLatency」作为消费时间轴，在两个快照间插值。
**源码**：`NetworkTransform.cs:4338` `tickLatency = max(1, TickLatency + InterpolationBufferTickOffset)`（owner-authoritative 且非 host 时 `:4351` 再 +1，补偿 2×RTT）；`:4368` `tickLatencyAsTime = TimeTicksAgo(tickLatency)`；`:4388` `maxDeltaTime = tickLatency × tickPeriod`（落后太多时加速追赶）。
**外推**：**不做真正外推**。`BufferedLinearInterpolator.cs:503-515`：buffer 空且 target 已到达就停在最后值；只有「距最后一次状态超过 `maxDeltaTime + minDeltaTime`」才 `Reset` 到当前值，避免下次大跳。
**插值档位**：`NetworkTransform.cs:1111-1175` 三档——`LegacyLerp`（两段 lerp，兼容旧项目）、`Lerp`、`SmoothDampen`；第二段「lerp smoothing」是低通滤波，`MaximumInterpolationTime` 控制滤波强度（越大越平滑、也越滞后）。
**可借鉴**：窗口别拍脑袋——**缓冲区 = tickLatency × tickPeriod，至少 1 tick，默认 2 tick**（60 Hz ≈ 33 ms）；丢包时「停在最后状态」比外推安全（外推在 200 ms 丢包下会飞出地图）。Go2Cpp 的 `mux` 已有每流有界队列，插值器可以复用同款「有界环形缓冲 + 溢出丢弃最旧」。

## 4. 客户端预测与回滚：Unity 核心闭源，Source 有完整可读实现

**核心思想**：客户端本地先跑输入并对世界模拟；服务器回 ack + 权威状态，客户端丢弃已确认输入、从最后权威状态重放未确认输入。
**源码（Unity，只能到 samples 层）**：`NetcodeSamples/Assets/Samples/PredictionSwitching/PredictionSwitchingSystem.cs` 只演示 `GhostMode` 在 Interpolated ↔ Predicted 间切换；**输入缓冲大小与回滚逻辑在闭源包内，读不到**。
**源码（Source，完整）**：`source-sdk-2013/src/game/client/prediction.cpp:1434` `ComputeFirstCommandToExecute`——`skipahead = min(outgoing - acked - 1, m_nCommandsPredicted)`（`:1450-1452`），`RestoreEntityToPredictedFrame` 恢复预测态、`ShiftIntermediateDataForward`（`:1481`）丢掉已确认命令的中间态、再重放未确认命令；预测误差走 `ResetLatched` 全量或定向重置（`:1495-1550`）。输入命令 `CUserCmd` 带 `command_number`（序列）、`tick_count`（时间戳）、`random_seed`（可复现）（`src/game/shared/usercmd.h:137-140`）。
**可借鉴**：通用 RPC **不需要**预测回滚（见 §9）；若未来做游戏层，输入缓冲容量取 `ceil(2×RTT/tick) + 2`，并像 Source 一样用固定大小命令环 + 服务器侧「每包最多处理 N 条命令」双上限（具体常量本次未逐值验证）。最小实现是「保留最近 N 条输入 + 服务器 ack 序号 + 重放函数」，而不是完整 ECS 快照系统。

## 5. 延迟补偿：服务器回溯 + 时间戳对齐

**核心思想**：服务器保存每个玩家最近 1 s 的碰撞体/位置历史；收到「开火」时，把世界回溯到「客户端看到的那一刻」再做命中判定。
**源码**：`src/game/server/player_lagcompensation.cpp:34` `sv_maxunlag = 1.0`（clamp `[0,1]`）；`:258` 每帧删除 `curtime - sv_maxunlag` 之前的历史；`:393-399` `correct = 出向延迟 + 客户端插值延迟(lerpTicks)`，再 clamp；`:402` `targettick = cmd->tick_count - lerpTicks`；`:407-412` 若 `|correct - 本地估计| > 0.2 s`（200 ms）则放弃客户端 tick、改用延迟估计——**防作弊 + 防时间戳异常**。
**可借鉴**：回溯上限写死 1 s、且**永远以服务器自己的延迟估计做交叉校验**；历史记录存「碰撞体尺寸 + 位置 + 动画参数」，不是完整世界。Go2Cpp 若做此能力，先实现 `HistoryRing<PlayerState>`（每 tick 一条、容量 = 1 s × tickRate）即可。

## 6. Delta 压缩：脏标记 → 批处理 → baseline 差分

**核心思想**：写值只置脏位；帧末统一收集脏 NetworkBehaviour；发送时对每个字段「与 baseline 比较，相同只写 1 bit 存在位」。
**源码**：`NetworkVariableBase.cs:205` `m_IsDirty`、`:292` `ResetDirty`、`:237` `CanSend`（`MaxSecondsBetweenUpdates` 到期强制发，否则 `MinSecondsBetweenUpdates` 到期且超脏阈值才发）；`NetworkVariable.cs:128` `m_PreviousValue` 就是 baseline；`NetworkVariableDeltaMessage.cs:146-149` 逐字段 `shouldWrite`，`:115` 每字段先写 1 个 bool。
**可借鉴**：Go2Cpp 的 `frame::FrameCodec` 只有帧头 + 载荷（`include/go2cpp/frame/frame_codec.hpp`），没有任何「字段级增量」。最小实现 = **字段 dirty bitset + 上次 ack 的快照值**；`p2p` 的 `DatagramTransport` 与 RPC 都可复用同一套 `DeltaWriter<T>`。
**另一份教科书**：Source 的 `WriteUsercmd(to, from)`（`src/game/shared/usercmd.cpp:26` 起）——命令号/tick 是 `+1` 时只写 1 bit，字段相同也只写 1 bit；baseline 就是「上一条命令」，完全无需额外状态。
**两个必要补丁**：① 不可靠 delta 必须周期性发 full sync 兜底——`NetworkTransform.cs:2167-2177` 每个对象在 1 秒内错峰发一次完整帧同步（`m_NextTickSync += tickRate`），静止对象不发；② 服务端要把客户端产生的 dirty 转发给其他客户端（`NetworkVariableDeltaMessage.cs:213-231` 的 `m_ForwardUpdates`），否则 owner-authoritative 变量的更新到不了旁观者。

## 7. 可靠/不可靠混合：`NetworkDelivery` → UTP pipeline

**核心思想**：5 档语义（`Unreliable / UnreliableSequenced / Reliable / ReliableSequenced / ReliableFragmentedSequenced`，`Runtime/Transports/NetworkDelivery.cs:6-32`），发送时映射到不同 pipeline。
**源码**：`UnityTransport.cs:530-548`——`Unreliable → FragmentationPipelineStage`，`UnreliableSequenced → Fragmentation + UnreliableSequencedPipelineStage`，三种 Reliable 全部 → `ReliableSequencedPipelineStage`（`:1816-1846` 建管道）。
**哪些走哪档**：时间同步 `Unreliable`（`NetworkTimeSystem.cs:170`）；`NetworkTransform` 默认可靠（`NetworkTransform.cs:4712`），开 `UseUnreliableDeltas` 后走 `UnreliableSequenced`（`NetworkVariableDeltaMessage` 里也带 `NetworkDelivery` 元数据）；RPC/生成/销毁走可靠。
**可借鉴**：**注意 NGO 自己也踩坑**——三种 Reliable 共用一个有序管道，小包会被大包队头阻塞；这正是 Go2Cpp 的 TCP mux 在公网的问题。Go2Cpp 应做的是「**控制面走流、状态面走 datagram + 自建序号**」，而不是把状态塞进 mux 流。
**批量细节**：`NetworkMessageManager.cs:717-741` 把「同一 client、同一 delivery、装得下」的消息合并进一个 batch（`BatchHeader` 只加一次包头），不同 delivery 立即新开 queue item——这是「很多小状态更新」能扛住 30 Hz 的关键，Go2Cpp 若做 tick 批量发送可直接照抄。

## 8. 对照表

| 机制 | 游戏框架做法 | Go2Cpp 现状 | 差距 | 最小实现 |
|---|---|---|---|---|
| 传输抽象 | 消息级 `Send(payload, delivery)` + `PollEvent` | `rpc::Transport` 只有字节流（write/read/close/fd）；`p2p::DatagramTransport` 有 UDP 语义但无可靠性 | 无「消息 + 投递等级」抽象 | `MessageFrame{seq, delivery, payload}` + 每 delivery 一个发送队列 |
| 快照频率 | tick 驱动，默认 30 Hz，时间同步 1 Hz | 无 tick、无服务器时间 | 无时间基座 | `TickTimer` + `ServerTime/LocalTime/TimeTicksAgo` |
| 快照插值 | 缓冲 + `renderTime = serverTime - tickLatency` 两帧 lerp，不外推 | 无（grep 无 interpolat/extrapolat） | 无缓冲、无渲染时钟 | `SnapshotRing<T>` + `Interpolator<T>`（容量 `2×tickLatency+2`） |
| 客户端预测 | 输入缓冲 + ack + 未确认输入重放（Source 可读；Unity 核心闭源） | 无（RPC 无状态机、无输入序号） | 无输入序列/回滚点 | `InputBuffer{seq, cmd}` + `Replay(fromSeq)`（游戏层才做） |
| 延迟补偿 | 1 s 历史 + `targettick = cmd.tick - lerpTicks`，200 ms 交叉校验 | 无 | 无逐 tick 历史 | `HistoryRing<PlayerState>`（1 s × tickRate） |
| Delta 压缩 | 脏标记 → 帧末批处理 → baseline + 存在位 | 帧头 + 载荷，无字段级增量（`frame_codec.hpp`） | 无 dirty/baseline | `DeltaWriter<T>` + 字段 bitset + 上次 ack 值 |
| 可靠/不可靠 | 5 档 delivery → 不同 UTP pipeline | 只有 TCP（mux）与裸 UDP（p2p），中间层空 | 无混合可靠性 | `FrameType.delivery` + 关键帧重传队列（NACK） |

## 9. 游戏同步 vs 通用 RPC：能借什么、不能借什么

**能借（与「游戏」无关，是通用实时系统能力）**
- **消息级传输抽象 + delivery 语义**：Go2Cpp 现在「一次请求 = 请求帧 + 响应帧」，没有投递等级；加一层 delivery 就能让日志/控制面走可靠、状态面走不可靠。
- **baseline 差分**：字段级「与上一版比较 + 存在位」，对 RPC 的大结构体参数同样有效（比通用 protobuf 全量更省）。
- **时间基座与插值器**：远程渲染、流式状态展示、音视频同步都需要「按服务器时间倒退播放」。
- **固定 tick 的批处理**：把「每事件一次 syscall」变成「每 tick 一次批量写」，对 RPC 吞吐同样成立。

**不能借（依赖游戏特有的「完整世界状态 + 确定性模拟」）**
- **客户端预测 + 回滚**：RPC 没有可回滚的世界状态，也没有「输入」与「模拟」的分离；硬套只会得到「重试」而不是「预测」。
- **服务器 lag compensation**：需要逐 tick 的碰撞体历史与「客户端开火时刻」语义，通用 RPC 没有「命中判定」。
- **30–60 Hz 全量状态广播**：RPC 是事件驱动的，把状态当事件全量广播会把带宽吃光；游戏能这么干是因为「状态小、变化连续、可 delta」。
- **Reliable 单管道**：NGO 把三种 Reliable 塞进一个有序管道，队头阻塞明显；Go2Cpp 的 TCP mux 已经受这个限制，公网场景**不能**把状态同步也塞进去。

**半借**：`NetworkVariable` 的「脏标记 + 帧末批处理」适合 RPC 的批量小消息合并，但「每秒最多 N 次」的节流策略要按业务改，不要照搬 30 Hz。

## 10. 读不到 / 未验证的部分（如实声明）

- **Unity Netcode for Entities（`com.unity.netcode`）核心预测**：`GhostPredictionSystemGroup`、输入缓冲、回滚实现均在闭源包内；`NetcodeSamples` 只有 `PredictionSwitching` 示例（§4），**无法给出 Unity 的输入缓冲大小与回滚源码级结论**，§4 的回滚细节来自 Source 引擎（同源思想，但不是 Unity 实现）。
- **`NetworkRigidbodyBase`**：`NetworkRigidbody.cs:15` 继承它，但该类文件未出现在本次抓取的 HEAD 文件树中（只找到 `Editor/NetworkRigidbodyBaseEditor.cs`），故刚体插值只按类注释与 `NetworkTransform` 路径描述。
- **NGO `TickLatency` 的具体取值**：`NetworkTransform` 只消费它（`:4338`），定义在未抓取的 `NetworkTime` 相关文件；本文按「至少 1 tick、随 RTT 增长」使用，未逐行验证。
- **Go2Cpp 现状**：基于 `a0126318` 的 grep（`interpolat|extrapolat|predict|lag comp|snapshot` 在 `src/`、`include/` 无命中，仅 `logging` 有同名 Snapshot），以及 `tunnel/mux.hpp`、`frame/frame_codec.hpp`、`rpc/transport.hpp`、`p2p/transport.hpp` 的头注释与实现。

## 11. 落地顺序建议（对应 P1-3「快照插值」）

1. **时间基座**（`TickTimer` + `ServerTime/LocalTime/TimeTicksAgo`）——所有后续能力的前置；
2. **快照环形缓冲 + 插值器**（先做「停住不 extrapolate」，与 NGO 一致）；
3. **delivery 分档**（`FrameType` 加 delivery；状态帧允许丢，控制帧必达）——这一步同时解掉 mux 的队头阻塞；
4. **delta 压缩**（baseline + 存在位，先做标量/向量，不做通用反射）；
5. 预测/回滚与 lag comp 只在「有游戏层世界状态」时再做，不要在通用 RPC 层引入。
