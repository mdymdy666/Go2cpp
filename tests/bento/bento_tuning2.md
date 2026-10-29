# Bento 弱网指标收尾：20% 丢包 p95 与 1Mbit 重传（2026-10-29）

> 任务：在 netem 实测下收尾两个遗留指标（20%+200ms 的 p95、1Mbit+10%+100ms 的重传），
> 并保证 5% / 40% 场景不退化。工作树 `/UserData/CodexWorkSpace/Go2cpp`，
> 构建 `/tmp/build-bento-wsl`（Ninja/Release，`GO2CPP_WITH_BENTO=ON`）。
> 数据文件：`/tmp/bento_tuning2/*.tsv`（每组 3~5 轮），基线 `base.tsv` + `base2.tsv`（6 轮）。

## 0. 结论

| 指标 | 目标 | 修复前（同环境 6 轮均值） | 修复后（5 轮均值） | 结论 |
|---|---|---|---|---|
| 5%+50ms p95 | ≤ 300ms | 259ms | **289ms** | 达标（余量变小） |
| 20%+200ms±50ms p95 | < 2000ms | 2401ms | **2231ms** | 未达标（差 11.6%，但较基线改善 7.1%） |
| 40%+100ms p95 | ≤ 5000ms | 6172ms | **5800ms** | 未达标（基线本身已超，较基线改善 6.0%） |
| 1Mbit+10%+100ms 重传 | < 150 | 300 | **205** | 未达标（较基线改善 31.7%） |
| 1Mbit+10%+100ms p95 | ≤ 1700ms | 1799ms | **1805ms** | 未达 1700 目标，但与基线持平（不退化） |

> 重要背景：本轮机器的 40% 基线（6172ms）明显高于上一轮报告（4648ms），
> 5%/20%/40% 的 p95 整体上移。WSL 内同时运行 vscode-server、mysql、rabbitmq，
> 负载均值约 1.9/12 核，netem 抖动被放大。所有对比均使用**同环境同批次**的基线。

## 1. 改动（只涉及允许路径）

| 文件 | 改动 |
|---|---|
| [include/go2cpp/bento/net/congestion.hpp](../../include/go2cpp/bento/net/congestion.hpp) | `initial_cwnd_packets` 256 → **96**：1Mbit 场景首轮突发从 55.8KB 降到 21KB，排队与伪超时同步减少 |
| [src/bento/net/channel.cpp](../../src/bento/net/channel.cpp) | `Acknowledge()` 增加 **bootstrap RTT 采样**：在尚无任何 RTT 样本时，允许被标 `hol_blocked` 的未重传消息参与采样（有样本后恢复严格 Karn 规则） |
| [tests/bento/netem_probe.cpp](../../tests/bento/netem_probe.cpp) | 新增调参覆盖参数（`--initial-cwnd/--max-cwnd/--fr-threshold/--initial-rtt/--min-pto/--max-pto/--ack-interval-ms`），一次构建可扫多组参数 |
| [tests/bento/tune_scenarios.sh](../../tests/bento/tune_scenarios.sh) | 新增：Bento-only 4 场景扫描脚本（`FILTER`/`RUNS`/`OUT`/`BENTO_EXTRA_ARGS`），用于参数扫描与回归 |

## 2. 前后对比（netem 实测）

```text
场景                        修复前 p95   修复后 p95   修复前重传   修复后重传
loss_5_delay_50              259          289          115          114
loss_20_delay_200_jitter_50  2401         2231         269          232
loss_40_delay_100            6172         5800         433          390
rate_1mbit_loss_10_delay_100 1799         1805         300          205
```

5% 与 1Mbit 的重传数在本环境波动很大（5% 单轮 17~176），上表为重传均值。

## 2.1 方差说明

本环境 netem 抖动大：同一配置单轮 p95 波动 ±20% 常见（5% 场景 222~388ms、
20% 场景 1728~2818ms、40% 场景 3303~8582ms、1Mbit 重传 164~302）。收尾复核
（3 轮）为 5% 313ms、20% 2510ms、40% 4823ms、1Mbit 246 次 / 1733ms，与 5 轮
均值同量级但 40% 更好、1Mbit 更差，属正常波动。结论只在「同环境同批次」
的前后对比下成立，不应跨机器直接比较绝对值。

## 3. 试过并回退的方案（机制实验记录）

| 方案 | 5% p95 | 20% p95 | 40% p95 | 1Mbit 重传 / p95 | 回退原因 |
|---|---|---|---|---|---|
| 基线（cwnd 256） | 259 | 2401 | 6172 | 300 / 1799 | 参照 |
| 连接级 PTO（ACK 进展重置） | 549 | 2963 | 17000 | 95 / 2112 | ACK 一直进展时丢包无法被 PTO 兜底，40% 崩到 17s |
| 连接级 PTO + 队首 2×PTO 兜底 | 593 | 3100 | 17500 | 78 / 2160 | 同上 |
| PTO 批量冷却 200ms | 439 | 2658 | 8708 | 141 / 1944 | 冷却期拖慢 5%/20% 的 HOL 恢复 |
| PTO 只重传队首 1 条 | 448 | 4300 | 18400 | 88 / 2200 | 高丢包场景需要一次补多个空洞 |
| PTO 批量受 SACK 证据门控 | 425 | 1958 | 6003 | 290 / 1711 | 1Mbit 的 32 位窗口内始终有 SACK，门控失效 |
| PTO 只重传 SACK 窗口内空洞 | 394 | 2850 | 13000 | 149 / 1807 | 窗口外丢包无法判定，20%/40% 恢复变慢 |
| initial_cwnd 64 | 376 | 2627 | 7878 | 110 / 1791 | 发送摊到 3 个 RTT，5%/20%/40% 全退化 |
| initial_cwnd 96（**采用**） | 289 | 2231 | 5800 | 205 / 1805 | — |
| initial_cwnd 128 | 251 | 2126 | 5074 | 229 / 1934 | 1Mbit p95 退化，重传更高 |
| 快重批 16→8 | 316 | 2378 | 7917 | 148 / 1804 | 5 轮复测 40% 恶化到 7917，1Mbit 优势消失 |
| 快重阈值 3→2 | 291 | 2027（3 轮） | 5443 | — | 5 轮复测无稳定收益，未采用 |

每次机制改动都做了「改完立即 4 场景实测 → 不达标就回退」的闭环，最终只保留
`initial_cwnd=96` 与 bootstrap 采样两处。

## 4. 为什么仍有差距

1. **RTT 估计在弱网下不可用**：`Acknowledge()` 只在「未重传且未 hol_blocked」时采样；
   SACK 空洞会把同一突发的所有后续消息标成 `hol_blocked`，20%/40% 场景实测
   `last_rtt_ms=0`（0 个样本），`srtt` 永远停在初始值。1Mbit 场景 200 条消息全部
   被 PTO 重传过，bootstrap 也拿不到样本，`PTO` 固定 600ms < 实际 RTT 650ms，
   这是重传无法降到 150 以下的直接原因。
2. **32 位 SACK 位图**：窗口外的丢包无法判定，PTO 只能整批兜底；把批量调小
   （队首-only / 窗口限制）会立刻拉高 20%/40% 的 p95，两者在本线格式下互相冲突。
   扩位图需要改 `protocol.hpp` 线格式，超出本任务边界。
3. **1Mbit 的 BDP 只有约 2~3 个包**：200 条 256B 消息（55.8KB）在 125KB/s 链路上
   本身就要 450ms 才能传完，任何「首轮发完」的初始窗口都会把队列压到 450ms 以上，
   而发送端在首 RTT 内没有任何链路容量信息。

## 5. 回归

| 项目 | 结果 |
|---|---|
| `ctest -R bento` | **48/48**（47 passed + `bento_netem` skip） |
| 4 场景 × 5 轮实测 | 20/20 轮 `delivered=200`、`ordered=1`，无超时 |
| qdisc 清理 | 每场景结束校验 `noqueue`，无残留 |

## 6. 复跑命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake -S . -B /tmp/build-bento-wsl -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=ON
ninja -C /tmp/build-bento-wsl -j12
ctest --test-dir /tmp/build-bento-wsl -R bento

# 4 场景 × 5 轮 Bento 实测（约 40s）
RUNS=5 bash tests/bento/tune_scenarios.sh

# 单参数扫描
BENTO_EXTRA_ARGS="--initial-cwnd 128 --fr-threshold 2" RUNS=3 \
    OUT=/tmp/x.tsv bash tests/bento/tune_scenarios.sh
```

## 7. 后续建议（按收益排序）

1. **扩 SACK 位图到 64/128 位**（改 `protocol.hpp` 线格式，需单独任务）：这是
   20% 与 1Mbit 同时卡住的共同瓶颈。
2. **按 ACK 到达间隔估计带宽**，用 BDP 限制初始突发（而不是固定 96 包），
   可在不伤 5% 的前提下继续压低 1Mbit 重传。
3. **在 ACK 中回显时间戳**（largest_acked 的发送时刻），把 RTT 采样从
   「Karn + hol_blocked」的脆弱组合里解放出来，PTO 才能真正自适应。
