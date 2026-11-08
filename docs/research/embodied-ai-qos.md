# 具身智能「快慢脑」分层控制 → 公网 QoS 分级调研

> 目的：为 Go2Cpp 公网主线 P0-4「QoS 分级」（`docs/plans/public-network-p0.md`）提供可核对依据。
> 范围：只采信论文原文与官方文档；**Go2Cpp 现状**一列基于本仓库源码，基线 commit `a012631`。
> 标记：✅ = 直接引自原文；⚠️ = 抓取失败或仅二手，见 §6。

## 1. 结论先行

1. 快慢脑不是「两种 AI」，而是**三个时间尺度**：伺服环 100–1000 Hz、策略/快脑 5–50 Hz、规划/慢脑 0.2–2 Hz。人类 System 1（直觉）在毫秒级（简单反应时 ~200 ms、选择反应时 ~400–600 ms），System 2（受控推理）在秒级——量级差异约 **10–100 倍**。
2. VLA 的延迟**不是靠更快的网络解决的**，而是靠 action chunking 把「推理周期」与「控制周期」解耦：π0 在 50 Hz 机器人上**每 0.5 s 才推理一次**。
3. 所以 QoS 分级的核心不是「多开几条通道」，而是两条规则：**关键小包不许排在大包后面**；**过期数据宁丢不补**。

## 2. 频率分层（核心表）

| 层 | 代表系统与实测/默认值 | 频率 | 延迟容忍 | 丢包容忍 |
|---|---|---|---|---|
| **L0 伺服/控制环** | `ros2_control` controller_manager `update_rate` 默认 **100 Hz** ✅；周期偏差 >5 Hz 即 warn、>10 Hz 即 error ✅ | 100–1000 Hz | 1 个周期（1–10 ms） | ≈0：丢一拍=抖动，丢多拍=失稳 |
| **L1 人类快脑（System 1）** | Kahneman 双系统：自动、联想、不费力；认知科学用反应时度量，无「Hz」指标 ⚠️ | 2–5 Hz 等效 | 100–300 ms | 中：可纠错，但体验断裂 |
| **L1 快脑/策略（VLA）** | RT-2：55B **1–3 Hz**、5B **~5 Hz** ✅；OpenVLA 7B **~6 Hz**（单张 4090、无量化、无投机解码）✅；π0 最高 **50 Hz** ✅ | 5–50 Hz | 20–100 ms | 低：可重发/插值，但**过期动作必须丢** |
| **L2 慢脑/规划/语义** | π0 的高层 VLM 指令；Nav2 `planner_frequency` 默认 1 Hz 量级 ⚠️ | 0.2–2 Hz | 0.5–2 s | 高：可重试、可重排 |
| **L3 数据面** | 日志、图像回传、模型/权重 | 事件驱动 | 秒–分钟 | 很高：只要完整 |

> 自动驾驶栈常用「感知 10–20 Hz / 规划 10 Hz / 控制 50–100 Hz」，与上表同构；**本条未取得逐字一手文档，仅作量级旁证** ⚠️。

### 2.1 为什么 L1 能低到 50 Hz 却不用 50 Hz 推理

- **action chunking**：一次前向产 H 步动作，执行完再推理。
  - π0：H=50；20 Hz 机器人「每 0.8 s 推理一次、执行 16 个动作」，50 Hz 机器人「每 0.5 s 推理一次、执行 25 个动作」✅。
  - OpenVLA **不支持** chunking（单步动作），论文明确把它列为上不去高频的原因 ✅；其 5 Hz / 15 Hz 控制器改用 chunk：T=8/X=3、T=16/X=8 ✅。
  - π0 试过 temporal ensembling（拼接历史 chunk），**实测反而降低策略性能**，故改为纯 open-loop 执行 ✅。
- **延迟预算**（π0 板载分解）：图像编码 14 ms + 观测前向 32 ms + 10×流匹配动作 27 ms + **网络 13 ms（仅离板时）** ✅。
  → 公网可容忍的量级是**十几到几十毫秒**；超过 100 ms 就必须靠 chunk 覆盖。

## 3. ROS 2 QoS：工业标准的实际维度（比「五维」多三个）

| 策略 | 取值 | 解决什么 |
|---|---|---|
| History + Depth | keep last(N) / keep all | 队列满时丢旧还是全存（N 只在 keep last 下生效）|
| Reliability | best effort / reliable | 丢不丢、重不重传 |
| Durability | volatile / transient local | 迟到订阅者能否拿到历史样本 |
| **Deadline** | duration | 发布方多久没发算违约，**有事件回调** |
| Lifespan | duration | 消息过期后**静默丢弃**，永不被收到 |
| **Liveliness** + Lease | automatic / manual by topic | 发布者是否还活着，租约到期即判死 |
| （兼容性） | requested vs offered | 不兼容则**根本不建立连接**，且无消息 |

关键设计：**sensor data profile = best effort + 小队列**（「及时」优先于「全到」）✅；兼容性是「订阅方请求最低质量、发布方提供最高质量」✅。

## 4. 对 Go2Cpp 的启示

### 4.1 QoS 分级怎么做（关键流 vs 大包流）

- **三档即可，不要八档**：`P0 control`（<64 B、20–50 Hz、可靠但丢旧）、`P1 state/telemetry`（1–10 Hz、best effort）、`P2 bulk`（图像/日志/模型，可被抢占）。
- 调度用**严格优先级 + 每轮配额**（如 4:2:1）防饿死；帧头加 1 字节 class 字段，接收端可观测。
- 现有 `tunnel/mux` 已有「每流独立接收队列」，但**溢出即关流**（`mux.hpp:24-27`）——对控制流是错的，应改为「丢旧保新」。
- 验收沿用 P0-4：**P2 打满时 P0 的 P99 不劣化**。

### 4.2 要不要「动作分块」式批量发送

**要，但分两层，别混为一谈。**

- 传输层：系统调用级合并**已有**（`tunnel/mux.cpp:311,580` 的 writev 多帧合并；`rd_net/socket.cpp` 的 sendmmsg/writev 批量）。它省 syscall，**不减少往返**。
- 应用层：一次 RPC 带 K 个动作 + `(t0, Δt)` 时间戳，接收端按时间戳调度执行。K 由 RTT 与推理周期决定（对标 π0：25 动作/0.5 s）。
- **必须配套两件事**：过期动作丢弃（帧携带发送时刻）、周期性重规划（在 chunk 用完前发起下一次推理，而不是用完才发）。
- **反向教训**：chunk 越大越抗抖动，但反应性线性下降；π0 的 ensembling 实验说明「拼接」不是免费收益 ✅。

### 4.3 ROS 2 的 QoS 维度哪些值得抄

- **值得抄**：Reliability（分级而非全局）、Deadline（违约要可观测）、Liveliness（已有心跳，补语义）、History/Depth（丢旧保新）、Lifespan（过期丢弃）。
- **简化后再抄**：Durability 只对「状态快照」做 transient local，不做全量补发；QoS 兼容性协商改成「发送方标等级、接收方订阅等级」，避免 DDS 那套组合爆炸。
- **不必抄**：ownership、time-based filter、完整 DDS 策略集——收益低、复杂度高。

## 5. QoS 维度对照表

| QoS 维度 | ROS 2 做法 | Go2Cpp 现状（`a012631`） | 最小实现 |
|---|---|---|---|
| History/Depth | keep last(N) 覆盖写 | mux 每流有界队列，**满了关流**（`tunnel/mux.hpp:24-27`） | 满时丢最旧；控制流 depth=1 |
| Reliability | best effort / reliable 可逐话题选 | TCP 天然可靠；bento Link 自建 ACK/重传；UDP 无 NACK | 每流 1 个标志位；关键帧走 NACK |
| Durability | volatile / transient local | 无（断线不补发） | 仅对状态快照做可选补发 |
| Deadline | 周期违约触发事件回调 | 只有 RPC 调用超时 + Cancel（`rpc/client.hpp:13-14`） | 每流 deadline，超时回调/丢帧 |
| Lifespan | 过期静默丢弃 | 无（帧不带时间戳） | 帧头加 `sent_at`，超时丢弃 |
| Liveliness/Lease | automatic / manual + 租约 | **已有**：`bento/net/heartbeat.cpp`、relay lease、p2p keepalive、tunnel `idle_timeout`（`tunnel/mux.cpp:946`） | 补「租约丢失→降级/告警」 |
| Priority | DDS `TRANSPORT_PRIORITY` | **无**；HTTP/2 PRIORITY 解析但不参与调度（`http2/connection.hpp:37`）；mux 单发送互斥量（`mux.hpp:19-20`） | 2–3 级队列 + 配额，见 §4.1 |

## 6. 来源与核实状态

✅ 已读原文：ROS 2 QoS（docs.ros.org humble，8 策略与兼容性表）、ros2_control controller_manager（control.ros.org jazzy，`update_rate` 默认 100）、RT-2（arXiv 2307.15818，1–3 Hz / 5 Hz）、OpenVLA（arXiv 2406.09246，6 Hz / 1.2 Hz / 3 Hz / chunk T=16,X=8）、π0（arXiv 2410.24164，50 Hz / H=50 / 0.8 s / 0.5 s / 13 ms 网络）、Open X-Embodiment（arXiv 2310.08864，22 种机器人 / 527 技能 / 160,266 任务）。

⚠️ 未核实：① Kahneman 原书无 Hz 指标，反应时数字取自认知心理学通用量级；② LeRobot 异步推理（PolicyServer/RobotClient + action queue）在本环境多次抓取 `huggingface.co/docs/lerobot/async` 失败，仅作方向性引用；③ 自动驾驶三档频率无逐字一手来源；④ Nav2 默认频率为记忆量级。

- Kahneman, D. *Thinking, Fast and Slow*. 2011；Evans & Stanovich, *Dual-Process Theories of Higher Cognition*, Perspect. Psychol. Sci. 8(3), 2013.
- ROS 2 QoS: https://docs.ros.org/en/humble/Concepts/Intermediate/About-Quality-of-Service-Settings.html
- ros2_control: https://control.ros.org/jazzy/doc/ros2_control/controller_manager/doc/userdoc.html
- RT-2: https://arxiv.org/abs/2307.15818 ｜ OpenVLA: https://arxiv.org/abs/2406.09246 ｜ π0: https://arxiv.org/abs/2410.24164 ｜ OXE: https://arxiv.org/abs/2310.08864
- LeRobot async: https://huggingface.co/docs/lerobot/en/async （⚠️ 未抓取成功）
