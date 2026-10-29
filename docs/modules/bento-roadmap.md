# Bento 路线图（2026-10-30 起）

> 依据：2026-10-29 的 22 模块审查 + JinShan 新协议（RDRL/RDCH）+ 用户要求
> （常用模块 + 网络传输层 QoS 抽象）。

## 阶段 A：常用模块（明天优先）

| # | 模块 | 作用 | 优先级 |
|---|---|---|---|
| 1 | **TokenPairing** | `(key, token)` 1:N 管道配对（relay 式），Pair/Release/List | P1 |
| 2 | **RDRL/RDCH 双帧** | `FrameFormat` 参数化出 RDRL（64KiB/version=1）与 RDCH（4MiB） | P1 |
| 3 | **RDCH 消息层** | 应用层消息（能力握手/命令/文件控制），复用 caps | P2 |
| 4 | **错误码映射** | JinShan 109 个 ERR-* → `bento::error` Registry | P2 |
| 5 | **key_id** | SHA-256 前 4B = 8 hex（唯一可日志形态） | P2 |
| 6 | **连接池** | 复用 TCP 连接（健康检查/上限/超时） | P2 |
| 7 | **通用限流** | 令牌桶/漏桶（从 congestion 抽出） | P2 |
| 8 | **优雅关闭** | Shutdown{Add/Close/Wait}（信号/超时/顺序） | P2 |
| 9 | **健康检查** | Health{Alive/Ready} + 探活回调 | P2 |
| 10 | **配置绑定增强** | bind.hpp 嵌套/默认值/校验 | P3 |

## 阶段 B：网络传输层 QoS 抽象（用户方向）

### 目标

把传输层各维度（可靠性/优先级/延迟/带宽/安全/背压）收成**声明式 QosProfile**，
上层只声明需求，底层自动选通道与参数。

### 设计

```
QosProfile {
  reliability:  Reliable | Unreliable | BestEffort
  priority:     Control | Interactive | Streaming | Bulk   (0-3)
  latency:      RealTime | Low | Normal | Bulk
  bandwidth:    min/max/expected
  security:     None | Psk | PskForwardSecret
  loss_policy:  Retransmit | Fec | DropStale | DropOldest
  queue:        bytes/msgs + overflow (block/drop)
  deadline:     per-message / per-connection
}

QosManager::Open(peer, profile) -> Channel
QosManager::Update(channel, profile)   // 动态降级
QosManager::Stats(channel) -> QosStats // 命中 vs 目标
```

### 映射

| Profile 维度 | 底层组件 |
|---|---|
| Reliable | StreamChannel（SACK/PTO/窗口） |
| Unreliable | DatagramChannel（DropStale/乱序窗口） |
| Fec | VideoChannel（XOR FEC） |
| PskFS | crypto + X25519 |
| RealTime | 小队列 + 快速重传 + pacing 上限 |
| priority=Control | 高优先级队列（先发/不排队） |
| queue | relay/channel 有界缓冲 |

### 标准 profile（4 个）

| Profile | 组合 |
|---|---|
| **Control** | Reliable + Control 优先级 + PskFS + Retransmit + 小队列 |
| **Interactive** | Reliable + Interactive + Low 延迟 + Retransmit |
| **Streaming** | Unreliable + Streaming + Fec + DropStale + 中等队列 |
| **Bulk** | Reliable + Bulk + Retransmit + 大队列 |

### 验证

每个 profile × 4 场景（基线/弱网/并发/长稳）netem 实测，报告 `audit-qos.md`。

## 阶段 C：JinShan 合并准备

- token 配对 + RDRL/RDCH 就绪后，JinShan 的 relay 可对接 Bento 通用实现；
- 检查 `example/WarningDocement/jinshan-protocol-gaps.md` 的 P1 项。

## 规则

- 每个模块：实现 → 压测/随机/5 场景 → audit 文档 → 三遍审查（源码/测试/扩展性）；
- 0 警告 + ctest 全过；
- 不引入第三方库；基础模块不动（留给用户）。
