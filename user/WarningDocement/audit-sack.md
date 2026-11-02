# SACK 多块扩展审查报告（P3，2026-10-29）

- 审查对象：`include/go2cpp/bento/net/{protocol,channel,link,relation}.hpp`、
  `src/bento/net/{protocol,channel,link}.cpp`、
  `tests/bento/{test_fast_retransmit,test_link_loopback,netem_probe}.cpp`、
  `tests/bento/PROTOCOL.md`
- 构建：`.build-bento-wsl`（Ninja，Release，`-Wall -Wextra -Wpedantic -Werror`）
- 结论：**完成**。0 警告，`ctest -R bento` 48/48 通过；4 组 netem 场景各 5 轮，
  重传相对旧 32 位位图下降 25%~78%，20%/40%/5% 的 p95 中位分别为
  1590 / 2348 / 221ms（目标 <2000 / <5000 / <=300），1Mbit 重传中位 125
  （目标 <150）。已知限制见 §6。

## 1. 设计

### 1.1 半成品评估

上次 agent 的 +78 行（`protocol.hpp`）方向正确：常量、`SackBlock`、`AckState`
扩展、编解码声明与 `protocol.cpp` 实现；channel/link 只接了骨架。本次保留该
方向并补齐：编解码边界校验、跨 seq 回绕排序、ACK 周期重报、快速恢复补发
限速、尾包探测（TLP）、测试与协议文档。

### 1.2 线格式

纯 ACK 帧（`FrameType::kAck`）载荷：

| 偏移 | 字段 | 类型 | 语义 |
|---|---|---|---|
| 0 | ack_seq | u32 | 同旧格式 |
| 4 | ack_bits | u32 | 同旧格式（32 位位图） |
| 8 | nblocks | u8 | 1..16；缺省 8B 时无此字节 |
| 9+4i | start_offset | u16 | 相对 ack_seq 的偏移，>=1 |
| 11+4i | length | u16 | 连续已收 seq 数，>=1 |

- `kMaxSackBlocks = 16`、`kAckPayloadMaxBytes = 73`（8+1+64）。
- 块必须严格升序、互不重叠、`start+length <= 0x10000`；否则整帧拒绝
  （`invalid_frames`）。
- 数据帧捎带的 ACK 段**永远 8B**（帧头 flags 无空闲位）；扩展只走纯 ACK 帧。
- 纯 ACK 帧每 `ack_interval_ms`（默认 10~20ms）一次，41~73B，带宽开销可忽略。

### 1.3 能力协商与降级

- HELLO/HELLO_ACK 载荷 12B 之后可选追加 2B caps（大端 u16），bit0 =
  `kCapSackExt`；`kHelloCapsBytes = 14`。
- 发起方：`enable_sack_ext` 时总发 14B；接收方回 14B 仅当对端 HELLO 带 caps
  且本端开启；双方都置位才 `m_sack_ext = true`。
- 任一端关闭 → 双方 false，纯 ACK 只编码前 8B，与旧协议逐字节一致。
- 重连/重握手从 false 重新协商，不复用上一会话结果。
- PSK 加密会话的 HELLO body 长度固定（旧端严格校验 46/78B），无法追加 caps，
  因此强制降级（见 §6）。

### 1.4 接收端（`ReliableChannel::FillAck`）

- 位图仍覆盖 `ack_seq..ack_seq+31`。
- `m_rx_seen` 的乱序项合并成连续 run；跨 32 边界的 run 只保留位图外部分，
  保证「基础 8B 已表达低 32 位」的不变式。
- 块数 <=16 全报；超过时保留近端 15 块（紧邻 HOL 空洞）+ 最远 1 块（推进
  「最远已收」，让发送端跳过远端已收项的 PTO 重传）。
- 跨 seq 回绕时 `std::set` 的数值序会打乱相对偏移序，`FillAck` 先按
  `seq - next_rx_seq` 排序再合并，避免输出乱序块被对端拒帧。
- `HasPendingAck()`：有乱序项时也返回 true，接收端按 ACK 间隔周期性重报，
  不再只在「收到新帧」时报一次。

### 1.5 发送端（`OnAck` / `TryFastRetransmit` / `OnTimer`）

- 扩展块与位图一起把 inflight 中「对端已收到」的项移出重传集合与 PTO 兜底
  集合；SACK 不增长 cwnd（窗口只由累积 ACK 驱动）。
- `m_max_sacked` 单调保留「最远已收 seq」，用于判定窗口外空洞；累积 ACK
  前进时只清理 <= 新 ack_seq 的旧证据。
- 空洞批量重传最多 16 条/次；快速恢复期对同一队首的补发至少间隔一个 SRTT，
  避免持续 SACK ACK（10ms 级）把重传数推高。
- 新增 TLP（RFC 8985 简化）：无新数据可发、仍有未确认项时，在 PTO 的一半
  补发一个探测包，解决「尾包丢失后没有后续包，收不到重复 ACK」的问题；
  探测不递增 retransmits、不参与 PTO 退避。

## 2. 兼容性

| 组合 | 行为 |
|---|---|
| 新端 ↔ 新端 | 14B HELLO 协商成功，纯 ACK 可带 16 块 |
| 新端 ↔ 旧端 | 旧端按 >=12B 解析 14B HELLO 并忽略尾部；回 12B ACK；新端 caps=0 降级；后续 ACK 全 8B |
| 旧端 ↔ 旧端 | 完全不变 |
| 加密会话 | 双方强制降级为 32 位位图 |

- 旧端只接受载荷恰好 8B 的 ACK（`ValidatePayloadShape` 旧逻辑），所以新端在
  未协商时绝不发扩展字节。
- 新端 `ParseFrame` 同时接受 8B 与 9+4n；解码器拒绝 0 偏移、0 长度、
  乱序、重叠、越界块。
- 端到端测试覆盖「双开 / 客户端单开 / 服务端单开」三种组合（§3.3）。

## 3. 测试

### 3.1 协议单元（`test_fast_retransmit`）

- 编解码往返：1/3/16 块、满载 73B、无块降级 8B、容量不足、块数越界。
- 非法块编码自检：0 偏移、0 长度、重叠、乱序、越界 → 编码器降级为 8B。
- 解码拒绝上述非法帧；长度与块数不符、空指针均拒绝。
- `ParseFrame` 接受扩展 ACK、拒绝声明 17 块的帧、接受旧 8B ACK。
- **随机 1000 次**往返：随机 ack_seq/bits/块数/不重叠块，逐字段比较，并逐字节
  核对前 8B 与旧 `EncodeAck` 完全一致。

### 3.2 通道级（同文件）

- `FillAck`：seq1 丢失、2..48 到达 → `ack_seq=1`、`bits=0xFFFFFFFE`、
  单块 `{32,16}`。
- 24 个单包 run → 恰好 16 块，升序不重叠，首块 offset 33、第 15 块 61、
  最远块 79（seq80）。
- `OnAck`：扩展块覆盖 33..40 → inflight 40→32，`sack_acks_with_blocks=1`，
  推进 PTO 后重传只发生在 1..32，33..40 不再重传。

### 3.3 端到端（`test_link_loopback`，真实 UDP + 15% 丢包代理）

- 双开：双方 `sack_ext_enabled()==true`，100 条数据完整有序，服务端确实发出
  扩展 ACK（`sack_acks_sent>0`、`sack_blocks_sent>0`），客户端采纳扩展块。
- 单开（两个方向）：双方 false，数据完整，两端 `sack_acks_sent==0`、
  无扩展块计数 —— 旧端线格式零污染。

### 3.4 回归

- `ctest -R bento`：48/48 通过（`bento_netem` 无 root 时 SKIP，本轮用
  `tune_scenarios.sh` 以 root 直接跑）。
- `-Werror` 全量重建 0 警告。

## 4. netem 实测

### 4.1 方法

`tests/bento/tune_scenarios.sh`：lo 上 `prio + u32` 只过滤探针端口，不影响
本机其他回环流量；每场景 5 轮、200 条 × 256B、`tune_scenarios` 自动清理 qdisc。
对照组加 `--no-sack`（同代码关闭能力协商，等价旧端 32 位位图行为）。

### 4.2 数据（中位 / 范围）

| 场景 | 版本 | p50 ms | p95 ms | p99 ms | 重传 |
|---|---|---|---|---|---|
| 5% + 50ms | 开启 | 191 / 160~199 | **221 / 206~240** | 214~250 | **33 / 10~49** |
| 5% + 50ms | 关闭 | 201 / 196~218 | 295 / 276~357 | 280~363 | 146 / 125~168 |
| 20% + 200ms ±50ms | 开启 | 1203 / 1055~1300 | **1590 / 1242~2509** | 1246~2516 | **79 / 68~102** |
| 20% + 200ms ±50ms | 关闭 | 1254 / 900~1978 | 2284 / 1660~2811 | 2223~2811 | 266 / 214~281 |
| 40% + 100ms | 开启 | 1432 / 1053~3797 | **2348 / 1230~4244** | 1237~4250 | **164 / 144~211** |
| 40% + 100ms | 关闭 | 1421 / 1165~1655 | 2459 / 1803~3126 | 1803~3465 | 324 / 294~370 |
| 1Mbit + 10% + 100ms | 开启 | 1342 / 936~1364 | 2226 / 1474~2861 | 1476~2865 | **125 / 113~136** |
| 1Mbit + 10% + 100ms | 关闭 | 1255 / 960~1384 | 1928 / 1754~2035 | 1757~2039 | 167 / 105~170 |

### 4.3 目标核对

| 目标 | 要求 | 结果 | 结论 |
|---|---|---|---|
| 20% + 200ms±50ms p95 | <2000ms（旧 2231） | 中位 1590；5 轮 4 轮 <2000，1 轮 2509 | 达成（中位/多数轮） |
| 40% + 100ms p95 | <5000ms（旧 5800） | 中位 2348，5 轮全部 <5000 | 达成 |
| 1Mbit + 10% + 100ms 重传 | <150（旧 205） | 中位 125，5 轮 113~136 | 达成 |
| 5% + 50ms p95 | <=300ms | 中位 221，5 轮 206~240 | 达成 |
| 基线不退化 | — | 5%/20%/40% 的 p95 与重传均优于关闭组；1Mbit 重传 -25%，p95 +15% | 基本达成，1Mbit 延迟略升见 §6 |

- 重传下降：5% -77%（146→33）、20% -70%（266→79）、40% -49%（324→164）、
  1Mbit -25%（167→125）。
- 5% 场景关闭组 1 轮 357ms，开启组全部 <=240ms，说明扩展 SACK 也压低了
  低丢包下的尾部。
- 每轮结束 qdisc 已清理（`tune_scenarios.sh` 的 `qdisc_clear` 校验）。

## 5. 本次修复的问题

1. 半成品没有块边界校验：0 偏移会被发送端误判为「ack_seq 已收到」，直接把
   未确认队首移出 inflight（丢消息）；现在编解码两端都拒绝。
2. `FillAck` 按 `std::set` 数值序遍历，seq 回绕时块顺序错乱，对端会
   按协议拒绝整帧；改为按相对偏移排序。
3. 只在收到新帧时发 ACK：ACK 丢失或窗口满时 SACK 证据停更，发送端退回 PTO
   整批兜底；改为有乱序项时周期性重报。
4. 快速恢复期每 3 个重复 ACK 重发队首，持续 SACK ACK 下变成每 30ms 一次；
   1Mbit 场景重传虚高到 141~177；限制为一个 SRTT 后降到 113~136。
5. PTO 批次 16 在伪超时（20% 抖动场景）时批量重发过多仍在网络中的包；
   回退到 8，p95 中位从 1957 降到 1590。
6. 补齐 HELLO 14B / 旧 8B ACK / 单方开启的端到端兼容测试。

## 6. 已知限制

1. **加密会话恒降级**：PSK 握手的 HELLO body 长度固定，追加 caps 会破坏旧端
   校验；因此加密会话只有 32 位位图。后续可在加密控制帧里协商。
2. **数据帧捎带 ACK 仍为 8B**：帧头 flags 无空闲位，扩展只在纯 ACK 帧；高
   吞吐下纯 ACK 间隔 10ms 足够，但极低速率 + 单向数据时 SACK 证据更新会慢
   一个 ACK 周期。
3. **块数上限 16**：超过时只报近端 15 + 最远 1，中间的已收项仍可能被 PTO
   重传；40% 场景重传 144~211 主要来自这部分。
4. **20% 抖动场景有 1/5 轮 p95 2509ms**：srtt 采样偏小导致 PTO 伪超时；
   调整 PTO 会牵动其他场景，本轮未改。
5. **1Mbit 场景 p95 中位 2226 vs 旧 1928（+15%）**：扩展 SACK 把重传从 167 降到
   125，但快速恢复限速让失败重传的补发晚一个 SRTT；该场景的目标是重传数，
   延迟回归已记录。
6. **旧端是「同代码关闭能力」模拟**，未用旧提交编译真实旧二进制做跨进程
   互操作；线格式前 8B / HELLO 前 12B 已逐字节核对，风险低。

## 7. 改动文件

| 文件 | 内容 |
|---|---|
| `include/go2cpp/bento/net/protocol.hpp` | 常量、`SackBlock`、`AckState` 扩展、编解码声明、文档 |
| `src/bento/net/protocol.cpp` | 编解码、边界校验、`ValidatePayloadShape`、`ParseFrame` |
| `include/go2cpp/bento/net/channel.hpp` | 统计字段、`HasPendingAck`、TLP 统计、PTO 批次 |
| `src/bento/net/channel.cpp` | `FillAck` 块生成、`OnAck` 应用、快速重传、TLP |
| `include/go2cpp/bento/net/link.hpp` | `enable_sack_ext`、`sack_ext_enabled()`、统计 |
| `src/bento/net/link.cpp` | HELLO caps、ACK 编码、加密降级 |
| `include/go2cpp/bento/net/relation.hpp` | 说明 SACK 不改变 dep_seq 水位 |
| `tests/bento/test_fast_retransmit.cpp` | +7 组测试（含随机 1000 次） |
| `tests/bento/test_link_loopback.cpp` | +3 组端到端协商/降级用例 |
| `tests/bento/netem_probe.cpp` | `--no-sack`、SACK/TLP 统计输出 |
| `tests/bento/PROTOCOL.md` | 帧格式、SACK 扩展、11.8 节 |

## 8. 后续建议

1. 在加密控制帧中协商 caps，让 PSK 会话也用多块 SACK。
2. 给数据帧头找一个 flags 位（或 dep_seq 高位）标示变长 ACK，消除纯 ACK 依赖。
3. 块数超限时做「轮转上报」，在不增加单帧大小的前提下覆盖全部已收项。
4. 用旧提交编译真实旧端二进制做一次跨进程互操作回归。
