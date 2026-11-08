# WebRTC 公网策略源码调研（Go2Cpp P0 前置）

> **目的**：为 P0-1（Pacing）/P0-2（带宽估计）/P0-3（抗丢包）/P1-1（Jitter）/P1-2（迁移）提供**源码级**依据。
> **方法**：只读真源码，经 `cdn.jsdelivr.net/gh/webrtc-mirror/webrtc@main` 抓取（2026-11-08），未固定 SHA；引用以「文件:函数」定位。**只写文档，不改代码。**

## 1. Pacing（`modules/pacing/`）

**核心思想**：现代 pacer 已不是「每 5ms 发一次」的定时器，而是**欠账（debt）模型**：
发送时 `media_debt_ += 包大小`，时间流逝时 `media_debt_ -= rate × elapsed`；
下一发送时刻 = `media_debt_ / adjusted_media_rate_`。允许在一个 **burst 窗口**内连发多个包，
窗口默认 **40ms**（`PacerConfig::kDefaultTimeInterval`），且被 `kMaxBurstSize = 63KB` 夹住
（高速率下窗口自动缩到 63KB/rate，防止打爆 socket buffer）。
欠账上限 `kMaxDebtInTime = 500ms`；时钟跳变保护 `kMaxElapsedTime = 2s`；
暂停/拥塞时每 500ms 发 keepalive padding 防反馈断流。
队列排不空时 `MaybeUpdateMediaRateDueToLongQueue()` 会**临时超过 BWE 提速排空**
（`queue_time_limit = 2000ms`），即「延迟优先于带宽估计」。

**关键代码**
- `modules/pacing/pacing_controller.cc:NextSendTime()` — drain_time 与 burst 窗口取小
- `modules/pacing/pacing_controller.cc:ProcessPackets()` — 发送主循环 + circuit breaker（65536 次）
- `modules/pacing/pacing_controller.cc:UpdateBudgetWithSentData/UpdateBudgetWithElapsedTime`
- `modules/pacing/pacing_controller.cc:MaybeUpdateMediaRateDueToLongQueue()`
- `modules/pacing/interval_budget.cc` — **历史实现**（500ms 窗口、欠账补偿），`pacing_controller.cc` 已不 include
- ⚠️ **`paced_sender.cc` 在 main 分支已不存在**（404）；「5ms」实为 `kTargetPaddingDuration` 与 `kMinSleepTime=1ms`，非发包周期

**可借鉴（Go2Cpp）**
1. 用 **debt 而非令牌桶**做 pacing：令牌桶只回答「能不能发」，debt 还回答「下次什么时候发」，
   且天然支持「略早发送」和「积压提速排空」；`bento::Pacer` 的 `burst_ms=5` 应改成 40ms + 63KB 双夹。
2. 把 pacing 决策收敛到**单一 `NextSendTime()`**：便于单测与埋点（实际时刻 vs 目标时刻的抖动）。

## 2. 拥塞控制 GCC（`modules/congestion_controller/goog_cc/`）

**核心思想**：GCC = **延迟梯度**（是否在排队）+ **AIMD**（怎么调速率）+ 可选丢包模型。
反馈包按到达排序后，用 `InterArrivalDelta` 以 **5ms 发送时间分组**算 `recv_delta - send_delta`
（`kSendTimeGroupLength = 5ms`，流超时 2s 重置），得到「排队延迟增长」的一阶信号。
`TrendlineEstimator` 对累计延迟做 **EWMA（系数 0.9）+ 窗口线性回归**，
斜率 trend 的物理含义是 `(发送速率 − 瓶颈容量)/容量`；`modified_trend = min(样本数,60) × trend × 4.0`，
与**自适应阈值**（初值 12.5，`k_up=0.0087 / k_down=0.039`，夹在 [6,600]）比较，
连续 overuse 超过 10ms 且斜率未回落才判 `kBwOverusing`（防抖动误判）。

**关键代码**
- `goog_cc/delay_based_bwe.cc:IncomingPacketFeedbackVector()/IncomingPacketFeedback()`
- `goog_cc/trendline_estimator.cc:UpdateTrendline()/Detect()/UpdateThreshold()`
- **AIMD 不在 `goog_cc/` 下**，已迁到 `modules/remote_bitrate_estimator/aimd_rate_control.cc`
  （`delay_based_bwe.h` 里 `AimdRateControl rate_control_`）
- `remote_bitrate_estimator/aimd_rate_control.cc:ChangeBitrate()`：
  减 `β=0.85`（`throughput × 0.85 − 5kbps`），增 `α=1.08`/秒（乘性）；
  有链路容量估计时改用**加性增** `max(4000bps/s, avg_pkt / (2 × (rtt+100ms)))`；
  增长上限 `1.5 × throughput + 10kbps`；降速后进 `kRcHold`，等 `kBwNormal` 才恢复增长

**可借鉴（Go2Cpp）**
1. **延迟梯度是独立信号**：先落地 trendline（单文件、窗口 20 包、无状态依赖），
   在 `bento::CongestionController` 旁并行跑，输出 overuse/underuse/normal 三态。
2. **降速后必须 hold**：否则 ACK 一恢复就立刻加性增，会在瓶颈处形成锯齿振荡。

## 3. 抗丢包（NACK + FEC）

**核心思想**：NACK **不是收到空洞就立刻要**，而是先判断「是不是乱序」：
`WaitNumberOfPackets(0.5)` 用**重排直方图的 50% 分位**决定「再等几个序号」，
再结合 RTT（`now - sent_at_time >= rtt_`）重发；单包最多 NACK 100 次，
NACK 表上限 1000（满了清表并请求关键帧），序号存活上限 10000。
FEC 冗余度不是固定值：`UpdateFecRates()` 拿**滤波后的丢包率**（max filter）、RTT、目标码率、帧率，
交 `VCMLossProtectionLogic` 算 key/delta 帧的 `fec_rate`，并把 **NACK+FEC 开销占比压在 50% 以内**
（超出则反压源码率）；默认 mask 用 `kFecMaskRandom`，bursty 码表存在但**默认不启用**。

**关键代码**
- `modules/video_coding/nack_requester.cc:GetNackBatch()/AddPacketsToNack()/UpdateRtt()`
  （⚠️ 不在 `modules/rtp_rtcp/source/`，旧路径已 404）
- `modules/video_coding/fec_controller_default.cc:UpdateFecRates()/SetProtectionMethod()`；常量 `kMaxNackRetries=100`、`kMaxNackPackets=1000`、`kMaxReorderedPackets=128`、默认 RTT=100ms

**可借鉴（Go2Cpp）**
1. **重排直方图**是低成本高收益：`bento` 的 fast-retransmit 直接用 dup-ack 计数，
   在公网乱序（尤其多路径/无线）下会误重传；加一个 128 桶直方图 + 50% 分位即可。
2. FEC 冗余度**按滤波丢包率闭环**，而不是编译期固定 `data/parity`；先做「每秒按 loss_rate 调 parity 数」的最小闭环。

## 4. Jitter Buffer（`modules/video_coding/timing/`）

**核心思想**：现代实现用 **Kalman 滤波**估「帧延迟变化」而非老版的 alpha/beta 指数平滑：
`frame_delay_variation_kalman_filter`（phi=0.97, psi=0.9999）按**帧大小**归因延迟变化
（大帧本来就要更久传完，不算抖动）。噪声方差用 EWMA（alpha 随样本数自适应，并按 30fps 缩放），
噪声阈值 `2.33σ − 30ms`（下限 1ms）；总估计 = Kalman 的 size-based 估计 + 噪声阈值；
**连续 NACK ≥ 3 次时再加 `rtt × rtt_multiplier`**（丢包说明路径已不稳）；
帧率 <5fps 不估，上限 10s，另加 `OPERATING_SYSTEM_JITTER = 10ms`。
缓冲深度 = `jitter_delay + RequiredDecodeTimeMs`（解码耗时的百分位滤波）。

**关键代码**
- `modules/video_coding/timing/jitter_estimator.cc:EstimateRandomJitter()/CalculateEstimate()/GetJitterEstimate()`
- `modules/video_coding/timing/timing.cc:MaxWaitingTime()/SetJitterDelay()`；常量 `kMaxJitterEstimate=10s`、`kMinJitterEstimate=1ms`、`kAlphaCountMax=400`

**可借鉴（Go2Cpp）**
1. `media::JitterBuffer` 已有 `target = clamp(base + J×factor, min, max)`，把 J 的来源从
   「到达间隔方差」换成 **Kalman + 帧/消息大小归因**，可显著降低大消息造成的虚高。
2. 把「NACK 次数」接进深度公式（>3 次加一个 RTT），这是对弱网最直接的补偿。

## 5. 连接迁移（`p2p/base/`）

**核心思想**：迁移=**候选对热切换**，不是拆连接重建。ICE restart 由应用层换凭据触发
（`SetIceParameters` 改 ufrag/pwd，下一次 `MaybeStartGathering` 重新收集）；
远端新凭据 `SetRemoteIceParameters` 时**旧凭据保留**，新 generation 排前，
并 `OnSortAndSwitchRequest(REMOTE_CANDIDATE_GENERATION_CHANGE)` 触发重排。
切换本身只翻转 selected 标记 + `NotifyNetworkRouteChanged`，**旧 Connection 不销毁**；
`PresumedWritable()` 让 TURN-TURN 连接在 permission 未建好时也上报 ready-to-send，
避免上层停发；网络失败时 `regather_on_failed_networks_interval` 定时重新收集候选。

**关键代码**
- `p2p/base/p2p_transport_channel.cc:SetIceParameters()/SetRemoteIceParameters()`
- `p2p/base/p2p_transport_channel.cc:SwitchSelectedConnectionInternal()`
- `p2p/base/basic_ice_controller.cc`（提名/剪枝）；⚠️ `p2p/base/ice_transport.cc` 不存在，接口在 `ice_transport_internal.cc`

**可借鉴（Go2Cpp）**
1. **新旧并存**：Go2Cpp 的 `p2p` 有 STUN/TURN/打洞，但切换即断；最小改动是
   「切换时保留旧 path 3–5s 双写/双收」，验证新 path 可用后再摘旧 path。
2. **凭据版本化**：为每次重连生成 generation 号，让「更新的候选」优先，而不依赖到达顺序。

## 6. 不能照搬的 WebRTC 假设

| # | WebRTC 假设 | Go2Cpp 现实 | 后果 |
|---|---|---|---|
| 1 | 恒定帧率、帧=一组包（AIMD 用 1/30s，jitter 按帧） | RPC 消息 1B–1MB，无帧率 | 按帧的时间窗要改成按字节/按消息 |
| 2 | 有逐包反馈（transport-cc）+ 周期 RR | RPC 多为批量 ACK | trendline/AIMD 无输入，须先提高反馈密度 |
| 3 | 可降源码率/丢帧保实时 | RPC 不能丢消息 | 「压源码率」不能当第一手段，只能重传/排队 |
| 4 | 单向大流量、长生命周期 | 双向小包 + 请求响应，RTT 主导 | 不能只看吞吐，要按 RPC 优先级分级 |
| 5 | RTP 时间戳可对齐 | 无统一媒体时钟 | 只用 `recv_delta−send_delta` 差值，防时钟漂移 |
| 6 | pacer 单线程串行 | Go2Cpp 多线程/多 fiber 发送 | debt 必须单线程队列或原子化 |

## 7. Go2Cpp 差距表

| 机制 | WebRTC 做法 | Go2Cpp 现状（代码事实） | 差距 | 最小可行实现 |
|---|---|---|---|---|
| 平滑发送 | debt + 40ms burst + 63KB 上限 + 积压提速 | `bento::Pacer` 令牌桶（burst_ms=5），仅 bento 通道 | RPC 主路径无 pacing；无排空优先 | 抽出 `go2cpp_pacing` 可选模块，Pacer 加 debt 语义与双上限 |
| 延迟梯度 | trendline 回归 + 自适应阈值 12.5 | 无；bento 是 BBR（delivery-rate）+ NewReno | 缺「排队增长」信号 | 单文件 trendline（20 包窗口）+ 三态输出 + 单测 |
| AIMD 系数 | α=1.08/s、β=0.85、降后 hold | NewReno β=0.5；BBR 不因丢包降窗 | 无延迟驱动降速 | `CongestionAlgorithm::kGcc` 枚举 + 复用现有 cwnd/pacing |
| NACK 时机 | 重排直方图 50 分位 + RTT 重发 + 100 次上限 | SACK 位图 + dup-ack fast retransmit | 无乱序误判保护 | `rd_net` 加重排直方图，替换 dup-ack 阈值 |
| FEC 冗余度 | 滤波丢包率→fec_rate，开销≤50% | 静态 XOR/Cauchy-RS shape | 无自适应 | 每秒按 loss_rate 调 parity 数（1→3） |
| 抖动估计 | Kalman + 噪声阈值 + NACK×RTT | `media::JitterBuffer` 有 `J×factor` 自适应 | 估计器简单，无大小归因 | 用 Kalman 替换方差估计 |
| 连接迁移 | 新旧凭据并存 + 候选对热切换 | `src/p2p` 有 STUN/TURN/打洞，无 restart/热备 | 切换=重连 | 旧 path 保留 3–5s 双写，新 path 验证后摘除 |
| 排空优先 | queue_time_limit 2000ms 时超 BWE 提速 | 无 | 大包阻塞关键流 | 优先级队列 + 老化（快慢脑分级） |

## 8. 建议落地顺序（与 P0 计划对齐）

1. **P0-1 Pacing**：`bento::Pacer` 的 5ms/令牌桶改成 debt + 40ms/63KB（收益最大、依赖最少）。
2. **P0-2 GCC**：只做 trendline 三态 + AIMD(1.08/0.85/hold)，**不**做完整 `GoogCCNetworkController`（探针/ALR 先砍）；**P0-3 NACK** 先在 `rd_net` 加重排直方图。

## 9. 未读到 / 未能验证的部分（如实说明）

- `modules/pacing/paced_sender.cc`：**404，main 分支已无此文件**（由 `pacing_controller.*` 取代）。
- `rtp_sender.cc`、`rtcp_receiver.cc` 未完整取到：**RTT 测量公式（LSR/DLSR）未验证**，只依据 `NackRequester::UpdateRtt()` 的注入接口。
- `fec_private_tables_bursty.cc`、`media_optimization.cc` 未取到：**丢包率→fec_rate 映射表未验证**，只确认调用链与 50% 开销上限。
- `p2p/base/port.cc`：未取到，迁移细节依据 `p2p_transport_channel.cc` 与 `basic_ice_controller.cc`。
- 以上缺口不影响结论方向，但实现前需补读对应文件。
