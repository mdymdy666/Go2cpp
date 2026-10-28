# Bento 真实 tc netem 弱网实测与拥塞控制修复（2026-10-28）

> 工作树：`/UserData/CodexWorkSpace/Go2cpp`（基线 `45e3431`，修复为未提交工作区改动）
> 构建：`.build-bento-wsl`（Ninja + GCC 13.3.0，`-DGO2CPP_WITH_BENTO=ON -DCMAKE_BUILD_TYPE=Release`）
> 基线二进制：`git worktree add /UserData/CodexWorkSpace/g2c-weaknet-base HEAD` 后独立构建
> 探针：`bento_netem_probe`（200 条 × 256B，随机端口，真实 netem 场景不启用内置用户态代理）
> 原始日志：`.dsh_tmp/weaknet_logs/`（`before_final.log` 基线、`after2.log` 修复后、
> `ctest_bento.log` 回归、`quick20/quick40/tune20/tune40/tune40b.log` 调参过程）
> 本报告所有数字均可在这些日志中逐行核对。

## 1. 目的与方法

### 1.1 为什么补真实 netem

仓库此前只有用户态 UDP 代理（`tests/bento/udp_proxy.hpp`，ctest `bento_weaknet_proxy`）：
只模拟独立丢包与固定单向延迟，**不模拟抖动、队列与整形**；`tests/bento/test_netem.sh`
在无 `CAP_NET_ADMIN` 的机器上固定 SKIP。本次用 root + `tc netem` 在 `lo` 上注入
真实排队、丢包与抖动，补齐这一空白，并顺带得到 tc 侧的发送/丢弃计数。

### 1.2 注入与清理

- 注入：`sudo tc qdisc add dev lo root netem <args>`（**全局 lo**，按任务要求；
  仓库自带脚本出于安全考虑改成 prio+u32 只过滤探针端口）。
- 复位：每场景开始前 `sudo tc qdisc del dev lo root`；采集脚本 `trap EXIT/INT/TERM`
  兜底删除。全部场景结束后实测 `tc qdisc show dev lo` → `qdisc noqueue 0: root refcnt 2`。
- 风险：全局 lo netem 会波及本机所有 loopback 流量（含 IDE/GUI 的本机通信）。
  测试窗口内除探针外无其它 loopback 负载；生产环境应改用 `test_netem.sh` 的端口过滤方案。

### 1.3 测量口径

- `bento_netem_probe`：单进程双 Link（服务端 127.0.0.1 随机端口 + 客户端），
  可靠流 200 条 STREAM（256B）+ 不可靠流 200 条 DATAGRAM；每 5 条 `Pump(3ms)`。
  可靠流要求 200/200 按序、链路 `timeouts==0`。
- 每场景跑 **2 轮**纯 netem 探针（不传 `--loss/--delay-ms`，避免双重注入）。
- 延迟口径：每条消息从 `SendStream` 入队到服务端 `on_stream` 交付的墙钟差，
  p50/p95/p99 由探针内部统计。
- tc 统计：`tc -s qdisc show dev lo` 的 `Sent/dropped`。

## 2. 场景参数

| 场景 | netem 参数 | 定位 |
|---|---|---|
| S0 | 无（`noqueue`） | 基线（回归对照） |
| S1 | `loss 20% delay 200ms 50ms` | 任务指定（20% 丢包 + 200ms±50ms） |
| S3 | `loss 40% delay 100ms` | 重度随机丢包 |

## 3. 修复前数据（基线 `45e3431`，本机重测）

| 场景 | 轮次 | 送达 | 重传 | 快重传 | p50 | p95 | p99 | SRTT | 末 cwnd | ssthresh | pacing |
|---|---|---|---|---|---|---|---|---|---|---|---|
| S1 20%+200±50 | 1 | 200/200 有序 | 91 | 66 | **13.70 s** | **31.80 s** | 33.79 s | 410 ms | **3** | 2 | 8.78 kB/s |
| S1 20%+200±50 | 2 | 200/200 有序 | 104 | 81 | 9.85 s | 29.14 s | 34.58 s | 426 ms | **2** | 2 | 5.63 kB/s |
| S3 40%+100 | 1 | 200/200 有序 | 181 | 54 | **13.62 s** | **30.81 s** | 31.29 s | 201 ms | **2** | 2 | 11.94 kB/s |
| S3 40%+100 | 2 | 200/200 有序 | 161 | 63 | 26.91 s | 66.45 s | 67.25 s | 237 ms | **2** | 2 | 10.13 kB/s |
| S0 无 netem | 1–2 | 200/200 有序 | 0 | 0 | 1 ms | 2 ms | 2 ms | 3–4 ms | 66 | 64 | 3.96 MB/s |

- 四轮全部 `delivered=200/200`、`ordered=1`、`timeouts=0`，退出码 0。
- 与上一版报告（p95 35.0/30.8 s、73.2/54.7 s）同量级：说明该缺陷在本机可稳定复现。
- 末态 `cwnd=2~3`、`ssthresh=2`、pacing 5.6~11.9 kB/s：可靠流虽然 100% 送达，
  但每 RTT 只能发 2~3 个包，实时性完全不达标。

## 4. 根因

`src/bento/net/congestion.cpp`（基线版）是纯 NewReno：

1. `OnLoss`（快速重传）无条件 `ssthresh = cwnd/2, cwnd = ssthresh`；
2. `OnTimeout`（PTO）无条件 `ssthresh = max(cwnd/2, 2), cwnd = 2` 并回慢启动；
3. `min_cwnd_packets = 2`，pacing = `cwnd × MSS / SRTT`。

netem 的 `loss` 是**无队列随机丢弃**，与拥塞无关；但上述逻辑把每一次丢包都当拥塞：
20% 丢包下每个 RTT 都会丢包，窗口被反复减半并压到 2；40% 下更是如此。
更糟的是 SRTT 被丢包/Karn 算法推高（410–426 ms），pacing 分母随之变大，
速率进一步掉到 5.6 kB/s —— 形成「丢包 → 降窗 → 慢发 → 更容易超时 → 再降窗」的死亡螺旋。
`cwnd=2` 时每 RTT（约 400 ms）只能发 2 个包，200 条约需 40 s，与实测 p95 31 s 吻合。

## 5. 修复方案（本次改动）

改动范围严格限定在 `bento/net/congestion.*`、`bento/net/channel.*`、`tests/bento/**`
（未改 base、未 commit）。

### 5.1 随机丢包归因（核心）

`CongestionController` 新增 `LossContext{now_ms, srtt_ms, holes, inflight}` 与
`LooksLikeCongestion()`：丢包只在以下情形归因为**拥塞**才降窗 ——

1. **RTT 膨胀**：`SRTT > min_rtt × 2.0`（`rtt_inflate_permille=2000`）。
   排队累积是拥塞的强信号；没有 RTT 样本时（握手后首次丢包）不做这条判定。
2. **批量空洞 + 极高丢包率**：同一 SACK 窗口 `holes >= 2` 且丢包率
   `>= 60%`（`extreme_loss_permille=600`）。

其余情况（孤立空洞、无排队膨胀）归因为**随机丢包**：`cwnd`/`ssthresh` 都不动，
只退出慢启动（避免用丢包换来的指数增长继续放大发送速率），并计入
`random_loss_events`。真拥塞降窗仍保留「同一 SRTT 内最多降一次」的抑制。

`holes` 由 `ReliableChannel` 在收到 ACK 时统计：ACK 位图覆盖范围（32 个 seq）内
尚未被累积确认、也未被 SACK 的包数；窗口外的未确认包可能仍在网络中，不计入。

### 5.2 超时不再塌窗

- **拥塞型 PTO**：`ssthresh = cwnd/2`、`cwnd = min_cwnd`、回慢启动（保留旧语义，防真拥塞）。
- **随机丢包型 PTO**：**保留窗口**（`cwnd` 不变），只保持/切换慢启动与拥塞避免状态。
  旧实现把 `cwnd` 压到 2，是「超时一次就再也起不来」的直接原因。
- `ReliableChannel` 的 `m_pto_recovery` 仍保证一次 PTO 事件只上报一次。

### 5.3 提高下限与 pacing 保护

- `min_cwnd_packets`：2 → **8**（即使被误降窗，每 RTT 也有 8 个包的基本盘）。
- `initial_cwnd_packets`：32 → **64**；`initial_ssthresh_packets`：64 → **128**。
  弱网 RTT 400 ms 下，首窗 32 包要 7 轮 RTT 才发完 200 条；64/128 直接砍掉 2–3 轮。
- pacing 速率硬下限 `min_pacing_bytes_per_sec = 12500`（**100 kbps**）。
- pacing 分母优先取 **min_rtt**（`use_min_rtt_for_pacing`），切断
  「丢包 → SRTT 膨胀 → pacing 衰减」的正反馈；无 RTT 样本时退回 SRTT。

### 5.4 丢包率估计

每个 `acked + lost` 包事件为一个样本，凑满 `loss_sample_packets=32` 个事件后
刷新千分比 `loss_rate_permille`（包事件口径，不用字节），供归因规则与统计使用。

### 5.5 批量重传提速（channel.hpp）

- `kPtoRetransmitBatch`：4 → 8（每 tick 最多补 8 帧 ≈ 2.4 KB）。
- `kFastRetransmitBatch`：8 → 16（窗口 128、40% 丢包时一轮丢约 50 个包，
  批量 8 需要 6 轮快速恢复才能补齐）。

### 5.6 可配置项（`CongestionController::Config`）

`random_loss_aware`（默认开）、`min_cwnd_packets=8`、`rtt_inflate_permille=2000`、
`extreme_loss_permille=600`、`min_pacing_bytes_per_sec=12500`、
`use_min_rtt_for_pacing=true`、`loss_sample_packets=32`；
`random_loss_aware=false` 可完整复现旧 NewReno 行为（回归对照用）。

## 6. 修复后数据（同机、同探针、同场景）

| 场景 | 轮次 | 送达 | 重传 | 快重传 | p50 | p95 | p99 | SRTT/minRTT | 末 cwnd | ssthresh | pacing | 随机/拥塞降窗 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| S1 20%+200±50 | 1 | 200/200 有序 | 221 | 51 | **5.23 s** | **6.41 s** | 6.42 s | 333/333 ms | **128** | 128 | **461 kB/s** | 8 / 0 |
| S1 20%+200±50 | 2 | 200/200 有序 | 201 | 131 | **1.70 s** | **8.81 s** | 8.82 s | 333/333 ms | **92** | 128 | 332 kB/s | 22 / 0 |
| S3 40%+100 | 1 | 200/200 有序 | 488 | 54 | **3.94 s** | **10.13 s** | 10.13 s | 207/207 ms | **128** | 128 | **742 kB/s** | 14 / 0 |
| S3 40%+100 | 2 | 200/200 有序 | 547 | 43 | 10.74 s | **15.70 s** | 15.71 s | 206/206 ms | **128** | 128 | 746 kB/s | 11 / 0 |
| S0 无 netem | 1–2 | 200/200 有序 | 0 | 0 | 1 ms | 2 ms | 2 ms | 3–4 ms | 129 | 128 | 7.74 MB/s | 0 / 0 |

### 6.1 修复前后对比

| 指标 | 修复前 | 修复后 | 变化 |
|---|---|---|---|
| S1 p50 | 13.70 / 9.85 s | 5.23 / 1.70 s | **−62% / −83%** |
| S1 p95 | 31.80 / 29.14 s | 6.41 / 8.81 s | **−80% / −70%** |
| S3 p50 | 13.62 / 26.91 s | 3.94 / 10.74 s | **−71% / −60%** |
| S3 p95 | 30.81 / 66.45 s | 10.13 / 15.70 s | **−67% / −76%** |
| 末 cwnd | 2~3 | 92~128 | **×30~60** |
| pacing | 5.6~11.9 kB/s | 332~746 kB/s | **×30~130** |
| wall（每轮） | 33.5~67.1 s | 7.5~18.0 s | **−57%~−78%** |

- **目标达成**：S1 p95 两轮 6.41/8.81 s（目标 < 10 s）；
  S3 p95 两轮 10.13/15.70 s（目标 < 20 s）。
- 可靠性与无 netem 性能均无回退：四轮仍 200/200 按序、`timeouts=0`；
  S0 p50/p95 仍为 1/2 ms。
- `congestion_loss_events = 0`：全部丢包都被判为随机丢包（符合 netem 的
  「无队列随机丢弃」性质），没有出现「误判随机丢包后窗口被真拥塞打爆」的情况
  （本测试无瓶颈队列，故不覆盖真拥塞路径；该路径由单测 `TestConsecutiveLossHalvesWindow`
  与 `TestTimeoutHalvesWindowOnCongestion` 覆盖）。

## 7. 单测与回归

### 7.1 单测（`bento_test_congestion`，298 个断言）

覆盖：慢启动/拥塞避免、丢包减半（旧语义对照）、超时回慢启动（旧语义对照）、
pacing 令牌桶不超速、**pacing 下限 + min_rtt 分母**、**随机丢包不降窗**、
**连续丢包（多空洞 + 高丢包率）降窗**、**丢包率估计**、
**拥塞型超时降窗**、**随机型超时保留窗口**、
**ReliableChannel 集成：拥塞丢包减半 / 随机丢包保窗**、
pacing 门控、通道超时、Link 统计导出。

新增用例（7 个）：`TestPacingFloorAndMinRttDenominator`、`TestRandomLossKeepsWindow`、
`TestConsecutiveLossHalvesWindow`、`TestLossRateEstimate`、
`TestTimeoutHalvesWindowOnCongestion`、`TestTimeoutKeepsWindowOnRandomLoss`、
`TestReliableChannelRandomLossKeepsWindow`。

结果：`[PASS] test_congestion (298 checks)`，退出码 0。

### 7.2 回归（`ctest -R bento`）

```
100% tests passed, 0 tests failed out of 48
Total Test time (real) = 25.19 sec
bento_netem (Skipped：需要 CAP_NET_ADMIN / 检测到自定义 root qdisc)
```

其中 `bento_weaknet_proxy`（20% 丢包 + 200 ms 用户态代理）耗时 **5.13 s**，
上一版报告记录为 38.86 s —— 同一条测试从 38.9 s 降到 5.1 s，
与 netem 实测的改善方向一致（拥塞控制不再是瓶颈）。

## 8. 结论

1. **可靠性与实时性同时成立**：S0/S1/S3 全部 200/200 按序送达、`timeouts=0`；
   20% 丢包 + 200 ms±50 ms 下 p95 从 31.8/29.1 s 降到 6.4/8.8 s，
   40% 丢包下从 30.8/66.5 s 降到 10.1/15.7 s。
2. **根因是「随机丢包被当拥塞」**：修复用丢包归因（RTT 膨胀 + 批量空洞）
   替代「见丢包就减半」，配合 min_cwnd=8、超时不塌窗、pacing 下限与 min_rtt 分母，
   把 cwnd 从 2~3 抬到 92~128、pacing 从 5.6~11.9 kB/s 抬到 332~746 kB/s。
3. **无 netem 与全量回归无退化**：S0 p95 仍 2 ms；48 个 bento 测试全过。
4. **仍存在的方差**：同样 40% 丢包，两轮 p95 为 10.1 s 与 15.7 s（wall 10.5/18.0 s）。
   原因是随机种子决定的丢包位置：关键包连续丢失时仍要等 PTO 退避。
   若要进一步压缩方差，需要更快的空洞恢复（如更大 SACK 位图 / ACK 频率自适应），
   超出本次边界。
5. **未实现完整 BBR**：时间与边界限制下采用「BBR 思路的最小集」
   （min_rtt 分母 + 丢包无关性），没有做带宽探测/瓶颈建模。若后续需要
   在有队列瓶颈的真实链路上限速，应补带宽采样（`BandwidthEstimator::delivery_rate_bps`
   已有，但未接入 pacing）。

## 9. 限制

- **归因是启发式**：真拥塞若表现为「丢包但 RTT 不涨」（浅队列、AQM、极短流），
  会被误判为随机丢包而不降窗；兜底是 `max_cwnd=256`、PTO 与连接级超时。
  生产环境建议把 `rtt_inflate_permille` 调到更敏感的值（如 1500）再评估。
- **注入范围**：全局 `lo` netem 影响本机所有 loopback 流量；非真实广域网
  （无 MTU/ECN/非对称链路/真实瓶颈队列）。本次未测 `rate 1mbit` 瓶颈场景
  （上一版报告的 S4 已表明 `rate` 对 200×256B 几乎不构成瓶颈）。
- **环境**：WSL2 `6.6.87.2`，单机单进程双 Link；测试期间 WSL 清理过一次 `/tmp`
  （因此构建目录改用工作区 `.build-bento-wsl`，基线用 git worktree 独立构建）。
- **样本量**：每场景 2 轮 × 200 条，p99 仅作趋势参考；未测 50%+ 丢包、
  显式 `reorder`/`duplicate`、长时间稳定性与多客户端。
- **不可靠流**：S1/S3 的 `stale_dropped`（140/128、0）与探针「每 5 条突发 3 ms」
  的发送模式强相关，属协议单调性设计，不在本次范围。
- **格式化**：仓库未配置 `.clang-format`，未做全仓格式化；新增代码风格与周边一致。
- **未提交**：按要求未 commit；工作区还包含其它未提交改动。

## 10. 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp

# 构建（.build-bento-wsl；/tmp 不可靠时不要用 /tmp）
cmake -S . -B .build-bento-wsl -G Ninja -DGO2CPP_WITH_BENTO=ON -DCMAKE_BUILD_TYPE=Release
ninja -C .build-bento-wsl bento_netem_probe bento_test_congestion

# 单测
./.build-bento-wsl/bento_test_congestion          # [PASS] test_congestion (298 checks)

# 回归
ctest --test-dir .build-bento-wsl -R bento --output-on-failure

# 单场景（S1）：注入 -> 两轮探针 -> 清理
sudo tc qdisc del dev lo root 2>/dev/null
sudo tc qdisc add dev lo root netem loss 20% delay 200ms 50ms
./.build-bento-wsl/bento_netem_probe --messages 200 --payload 256 --timeout-ms 120000
./.build-bento-wsl/bento_netem_probe --messages 200 --payload 256 --timeout-ms 120000
tc -s qdisc show dev lo
sudo tc qdisc del dev lo root

# 40% 场景
sudo tc qdisc add dev lo root netem loss 40% delay 100ms
./.build-bento-wsl/bento_netem_probe --messages 200 --payload 256 --timeout-ms 120000
sudo tc qdisc del dev lo root

# 基线（对比用）：HEAD 独立 worktree
git worktree add /UserData/CodexWorkSpace/g2c-weaknet-base HEAD
cmake -S /UserData/CodexWorkSpace/g2c-weaknet-base -B /UserData/CodexWorkSpace/g2c-weaknet-base/.build \
      -G Ninja -DGO2CPP_WITH_BENTO=ON -DCMAKE_BUILD_TYPE=Release
ninja -C /UserData/CodexWorkSpace/g2c-weaknet-base/.build bento_netem_probe
```

## 11. 环境与清理核验

- WSL2 `6.6.87.2-microsoft-standard-WSL2`（Ubuntu 24.04），CMake 3.28.3、Ninja 1.11.1、GCC 13.3.0。
- 全部场景结束后：`tc qdisc show dev lo` → `qdisc noqueue 0: root refcnt 2`。
- 基线 worktree 与临时脚本在任务结束前删除（见交付说明）。
