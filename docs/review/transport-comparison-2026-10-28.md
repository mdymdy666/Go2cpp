# TCP / UDP / Bento 三方对照实测报告（2026-10-28）

> 任务：用同一探针、同一 netem 场景对比 TCP、UDP、Bento 的送达率、延迟与重传，
> 为 Bento 的正确性、性能与可靠性提供可复现数据。
> 本报告与原始数据、脚本、探针一同落盘，未修改任何 Bento 实现。

## 1. 环境与方法

| 项目 | 值 |
|---|---|
| 工作树 | `/UserData/CodexWorkSpace/Go2cpp` |
| 构建目录 | `/tmp/build-bento-wsl`（Ninja，`GO2CPP_WITH_BENTO=ON`，Release） |
| 内核 / 编译器 | Linux 6.6.87.2-microsoft-standard-WSL2 / g++ 13.3.0 |
| tc | iproute2-6.1.0（netem + prio + u32） |
| 探针 | [tcp_probe.cpp](../../tests/bento/tcp_probe.cpp)、[udp_probe.cpp](../../tests/bento/udp_probe.cpp)、`bento_netem_probe`（已有，未改动） |
| 脚本 | [compare_transports.sh](../../tests/bento/compare_transports.sh) |
| 原始数据 | [compare_results.tsv](../../tests/bento/compare_results.tsv)（45 行 = 5 场景 × 3 传输 × 3 轮） |
| 运行日志 | `/tmp/transport-compare-logs/`（每轮 stdout/stderr） |

### 1.1 统一载荷与测量口径

- 每条 256B，共 200 条（51.2 KiB）；序号 + 魔数 + 固定 pattern + 发送时间戳。
- TCP：阻塞 socket、`TCP_NODELAY`；服务端按 256B 定长切分，逐条记录到达时间；
  重传取自 `getsockopt(TCP_INFO).tcpi_total_retrans`（客户端 + 服务端）。
- UDP：`sendto` / `recvfrom`，不重传；客户端发完后再等 1s 静默收尾。
- Bento：`bento_netem_probe` 的可靠通道 0（200 条）与不可靠通道 1（同载荷），
  PTO 下限 200ms，ack_interval 10ms，heartbeat 200ms；`--loss/--delay` 传 0，
  弱网完全由 tc netem 注入。
- p50/p95 是**发送到服务端收到**的单向延迟（同一进程内共享时钟）。
- 吞吐 = 送达字节 / 进程端到端 wall-clock（含握手、收尾），单位 Mbps。
  Bento 探针固定多 500ms 收尾，baseline 的 wall 又受进程调度影响，
  因此**吞吐只作趋势参考，p50/p95/送达率是主指标**。

### 1.2 netem 注入与隔离

直接用 `tc qdisc add dev lo root netem ...` 会波及 127.0.0.1 上的 DSH Web GUI
与其它回环服务。脚本采用 `prio` + `u32`，只把**探针端口**的双向流量放进 netem class：

```bash
tc qdisc add dev lo root handle 1: prio bands 4
tc qdisc add dev lo parent 1:3 handle 30: netem <场景参数>
tc filter add dev lo protocol ip parent 1:0 prio 3 u32 match ip dport <port> 0xffff flowid 1:3
tc filter add dev lo protocol ip parent 1:0 prio 3 u32 match ip sport <port> 0xffff flowid 1:3
```

每场景结束执行 `tc qdisc del dev lo root`，并用 `tc qdisc show dev lo` 校验回到
`noqueue`；5 个场景全部校验通过（见运行日志尾部）。

### 1.3 场景

| 场景 | netem 参数 | 链路特征 |
|---|---|---|
| baseline | — | 无额外损伤 |
| loss_5_delay_50 | `loss 5% delay 50ms` | 轻度丢包 + 低延迟 |
| loss_20_delay_200_jitter_50 | `loss 20% delay 200ms 50ms distribution normal` | 中度丢包 + 高延迟 + 抖动 |
| loss_40_delay_100 | `loss 40% delay 100ms` | 重度丢包 |
| rate_1mbit_loss_10_delay_100 | `rate 1mbit loss 10% delay 100ms` | 带宽受限 + 轻度丢包 |

## 2. 汇总结果（3 轮聚合）

| 场景 | 传输 | 送达/发送 | 送达率 | p50 均值 | p95 均值（最大） | 重传均值 | 端到端均值 | 有效吞吐均值 |
|---|---|---|---|---|---|---|---|---|
| baseline | TCP | 600/600 | 100.0% | 0.02 | 0.06（0.08） | 0.0 | 5 | 84.196 |
| baseline | UDP | 600/600 | 100.0% | 0.01 | 0.03（0.04） | 0.0 | 5 | 88.747 |
| baseline | Bento | 600/600 | 100.0% | 1.00 | 2.00（2.00） | 0.0 | 654 | 0.626 |
| loss_5_delay_50 | TCP | 600/600 | 100.0% | 193.84 | 193.96（281.60） | 0.7 | 640 | 0.995 |
| loss_5_delay_50 | UDP | 566/600 | 94.3% | 50.15 | 50.18（50.24） | 0.0 | 1057 | 0.365 |
| loss_5_delay_50 | Bento | 600/600 | 100.0% | 272.33 | 527.67（848.00） | 105.3 | 1318 | 0.318 |
| loss_20_delay_200_jitter_50 | TCP | 600/600 | 100.0% | 1143.71 | 1580.04（2366.06） | 4.0 | 1991 | 0.252 |
| loss_20_delay_200_jitter_50 | UDP | 488/600 | 81.3% | 204.70 | 278.61（281.87） | 0.0 | 1330 | 0.251 |
| loss_20_delay_200_jitter_50 | Bento | 600/600 | 100.0% | 1842.67 | 3441.67（4587.00） | 142.0 | 4499 | 0.094 |
| loss_40_delay_100 | TCP | 600/600 | 100.0% | 2729.49 | 2796.25（6920.10） | 4.7 | 4018 | 0.205 |
| loss_40_delay_100 | UDP | 359/600 | 59.8% | 100.09 | 100.13（100.15） | 0.0 | 1106 | 0.222 |
| loss_40_delay_100 | Bento | 400/600 | 66.7% | 5944.50 | 9533.00（9796.00） | 397.0 | 11978 | 0.026 |
| rate_1mbit_loss_10_delay_100 | TCP | 600/600 | 100.0% | 786.75 | 786.81（899.65） | 1.7 | 1328 | 0.357 |
| rate_1mbit_loss_10_delay_100 | UDP | 527/600 | 87.8% | 311.30 | 497.03（502.62） | 0.0 | 1528 | 0.235 |
| rate_1mbit_loss_10_delay_100 | Bento | 600/600 | 100.0% | 1372.00 | 1837.00（1922.00） | 70.3 | 2755 | 0.149 |

> 说明：40% 场景 Bento 的 66.7% 来自 1 轮（run 3）**握手 15s 超时**，
> 不是数据面丢包；其余 2 轮均为 200/200。Bento 的 p50/p95 只统计成功轮。

## 3. Bento 不可靠通道（通道 1，DropStale）

| 场景 | 不可靠流送达 | 送达率 | 无回退 | stale_dropped | 有效轮 |
|---|---|---|---|---|---|
| baseline | 600/600 | 100.0% | 1 | 0 | 3/3 轮有数据 |
| loss_5_delay_50 | 570/600 | 95.0% | 1 | 4 | 3/3 轮有数据 |
| loss_20_delay_200_jitter_50 | 30/600 | 5.0% | 1 | 453 | 3/3 轮有数据 |
| loss_40_delay_100 | 233/400 | 58.2% | 1 | 0 | 2/3 轮有数据 |
| rate_1mbit_loss_10_delay_100 | 544/600 | 90.7% | 1 | 0 | 3/3 轮有数据 |

关键现象：20% 丢包 + 200ms±50ms 抖动下，不可靠流 600 条只交付 30 条（5.0%），
`stale_dropped=453`。这是 DropStale 的语义：迟到的旧序号帧被丢弃，保证消费端
永不回退；但在**高抖动**链路下大量帧因乱序被判过期，实时通道可用性骤降。
5% 丢包/50ms 场景为 95.0%，40% 丢包/100ms 为 58.2%，1 Mbit+10% 为 90.7%。

## 4. 逐轮完整数据

| 场景 | 传输 | 轮次 | 送达/发送 | p50 (ms) | p95 (ms) | 重传 | 端到端 (ms) | 吞吐 (Mbps) |
|---|---|---|---|---|---|---|---|---|
| baseline | TCP | 1 | 200/200 | 0.017 | 0.025 | 0 | 4 | 102.400 |
| baseline | TCP | 2 | 200/200 | 0.015 | 0.074 | 0 | 6 | 68.267 |
| baseline | TCP | 3 | 200/200 | 0.018 | 0.078 | 0 | 5 | 81.920 |
| baseline | UDP | 1 | 200/200 | 0.008 | 0.027 | 0 | 4 | 102.400 |
| baseline | UDP | 2 | 200/200 | 0.008 | 0.035 | 0 | 5 | 81.920 |
| baseline | UDP | 3 | 200/200 | 0.006 | 0.018 | 0 | 5 | 81.920 |
| baseline | Bento | 1 | 200/200 | 1 | 2 | 0 | 654 | 0.626 |
| baseline | Bento | 2 | 200/200 | 1 | 2 | 0 | 655 | 0.625 |
| baseline | Bento | 3 | 200/200 | 1 | 2 | 0 | 653 | 0.627 |
| loss_5_delay_50 | TCP | 1 | 200/200 | 150.150 | 150.151 | 1 | 1277 | 0.321 |
| loss_5_delay_50 | TCP | 2 | 200/200 | 150.065 | 150.141 | 0 | 255 | 1.606 |
| loss_5_delay_50 | TCP | 3 | 200/200 | 281.300 | 281.599 | 1 | 387 | 1.058 |
| loss_5_delay_50 | UDP | 1 | 187/200 | 50.135 | 50.148 | 0 | 1057 | 0.362 |
| loss_5_delay_50 | UDP | 2 | 189/200 | 50.214 | 50.239 | 0 | 1057 | 0.366 |
| loss_5_delay_50 | UDP | 3 | 190/200 | 50.100 | 50.161 | 0 | 1058 | 0.368 |
| loss_5_delay_50 | Bento | 1 | 200/200 | 308 | 456 | 145 | 1280 | 0.320 |
| loss_5_delay_50 | Bento | 2 | 200/200 | 303 | 848 | 122 | 1578 | 0.260 |
| loss_5_delay_50 | Bento | 3 | 200/200 | 206 | 279 | 49 | 1096 | 0.374 |
| loss_20_delay_200_jitter_50 | TCP | 1 | 200/200 | 2365.987 | 2366.057 | 6 | 2767 | 0.148 |
| loss_20_delay_200_jitter_50 | TCP | 2 | 200/200 | 511.709 | 1820.462 | 4 | 2246 | 0.182 |
| loss_20_delay_200_jitter_50 | TCP | 3 | 200/200 | 553.447 | 553.601 | 2 | 960 | 0.427 |
| loss_20_delay_200_jitter_50 | UDP | 1 | 158/200 | 206.946 | 279.538 | 0 | 1349 | 0.240 |
| loss_20_delay_200_jitter_50 | UDP | 2 | 170/200 | 201.289 | 281.867 | 0 | 1321 | 0.264 |
| loss_20_delay_200_jitter_50 | UDP | 3 | 160/200 | 205.855 | 274.423 | 0 | 1320 | 0.248 |
| loss_20_delay_200_jitter_50 | Bento | 1 | 200/200 | 1593 | 2968 | 146 | 3974 | 0.103 |
| loss_20_delay_200_jitter_50 | Bento | 2 | 200/200 | 1897 | 2770 | 126 | 3855 | 0.106 |
| loss_20_delay_200_jitter_50 | Bento | 3 | 200/200 | 2038 | 4587 | 154 | 5669 | 0.072 |
| loss_40_delay_100 | TCP | 1 | 200/200 | 300.151 | 300.232 | 5 | 1508 | 0.272 |
| loss_40_delay_100 | TCP | 2 | 200/200 | 1168.402 | 1168.413 | 4 | 1373 | 0.298 |
| loss_40_delay_100 | TCP | 3 | 200/200 | 6719.928 | 6920.095 | 5 | 9172 | 0.045 |
| loss_40_delay_100 | UDP | 1 | 127/200 | 100.121 | 100.150 | 0 | 1106 | 0.235 |
| loss_40_delay_100 | UDP | 2 | 109/200 | 100.077 | 100.124 | 0 | 1106 | 0.202 |
| loss_40_delay_100 | UDP | 3 | 123/200 | 100.060 | 100.110 | 0 | 1107 | 0.228 |
| loss_40_delay_100 | Bento | 1 | 200/200 | 6855 | 9270 | 429 | 10096 | 0.041 |
| loss_40_delay_100 | Bento | 2 | 200/200 | 5034 | 9796 | 365 | 10832 | 0.038 |
| loss_40_delay_100 | Bento | 3 | 0/200 | - | - | - | 15005 | 0.000 |
| rate_1mbit_loss_10_delay_100 | TCP | 1 | 200/200 | 899.612 | 899.654 | 3 | 2110 | 0.194 |
| rate_1mbit_loss_10_delay_100 | TCP | 2 | 200/200 | 764.015 | 764.106 | 1 | 970 | 0.422 |
| rate_1mbit_loss_10_delay_100 | TCP | 3 | 200/200 | 696.613 | 696.685 | 1 | 903 | 0.454 |
| rate_1mbit_loss_10_delay_100 | UDP | 1 | 173/200 | 307.359 | 490.644 | 0 | 1524 | 0.232 |
| rate_1mbit_loss_10_delay_100 | UDP | 2 | 178/200 | 314.484 | 502.624 | 0 | 1533 | 0.238 |
| rate_1mbit_loss_10_delay_100 | UDP | 3 | 176/200 | 312.068 | 497.813 | 0 | 1527 | 0.236 |
| rate_1mbit_loss_10_delay_100 | Bento | 1 | 200/200 | 1280 | 1878 | 88 | 2739 | 0.150 |
| rate_1mbit_loss_10_delay_100 | Bento | 2 | 200/200 | 1509 | 1922 | 60 | 2779 | 0.147 |
| rate_1mbit_loss_10_delay_100 | Bento | 3 | 200/200 | 1327 | 1711 | 63 | 2748 | 0.149 |

## 5. 结论

### 5.1 正确性（Bento）

- 可靠通道：Bento 15 轮中 14 轮 **200/200 且 ordered=1**，无重复（duplicates=0）、
  无损坏（corrupt=0）；跨场景覆盖 5% / 20% / 40% 丢包、200ms 抖动与 1 Mbit 限速。
- 失败轮：`loss_40_delay_100 / bento / run3`，日志为
  `handshake timeout (client=0 server=1 tx=10 rx=4)`——40% 丢包下握手在 15s 内
  未完成，数据面未开始。即**数据面可靠，握手阶段在极端丢包下存在失败风险**。
- 不可靠通道：所有有效轮 `ordered=1`（无回退），DropStale 计数随抖动上升；
  语义正确，但高抖动下交付率低（20% 场景仅 5.0%）。

### 5.2 Bento vs TCP

| 场景 | Bento p95 / TCP p95 | Bento 重传 / TCP 重传 | 送达率（Bento / TCP） |
|---|---|---|---|
| 5% + 50ms | 528 / 194 ms（2.7×） | 105 / 0.7 | 100% / 100% |
| 20% + 200ms±50ms | 3442 / 1580 ms（2.2×） | 142 / 4.0 | 100% / 100% |
| 40% + 100ms | 9533 / 2796 ms（3.4×） | 397 / 4.7 | 66.7% / 100%（含握手失败） |
| 1 Mbit + 10% + 100ms | 1837 / 787 ms（2.3×） | 70 / 1.7 | 100% / 100% |

- 排除握手失败轮后，数据面送达率两者都是 100%，Bento 的正确性与 TCP 同级；但 **p95 是 TCP 的
  2.2–3.4 倍**，重传数高 35–150 倍（1–2 个数量级）。
- 主因是协议参数与工作模式，而非丢包本身：Bento PTO 下限 200ms，与 20% 场景
  的 RTT（≈400ms）同量级，固定下限会触发大量 PTO 重传；探针又背靠背发送 200 条
  独立可靠帧，ACK 聚合（10ms）与 200ms 心跳进一步放大在途帧数。TCP 则有
  SACK、快速重传与自适应 RTO，重传次数低一个数量级。
- 在高丢包（40%）下 Bento 还出现 1/3 握手失败，可靠性不如 TCP 稳定。

### 5.3 Bento vs UDP

- UDP 的 p50 接近链路单向延迟（50 / 205 / 100 / 311 ms；1 Mbit 场景含排队），因为它不重传、
  不排队补偿；代价是送达率随丢包率下降：94.3% / 81.3% / 59.8% / 87.8%。
- Bento 用**延迟换可靠**：p50 为 272 / 1843 / 5945 / 1372 ms，约为 UDP 的
  4.4–59 倍，但（除握手失败轮）送达率 100%。
- 对实时/可丢场景，UDP 仍是最低延迟选择；Bento 的不可靠通道本应填补中间地带，
  但 20%/200ms 抖动下 DropStale 只交付 5%，此时**实时性反而不如 UDP**。
  Bento 更适合“可靠优先、可容忍百毫秒到秒级延迟”的文件/命令/控制面流量。

### 5.4 总体判断

1. **正确性成立**：Bento 可靠流在全部成功轮中 100% 按序送达，无重复/损坏；
   不可靠流无回退。40% 丢包下 1 轮握手超时是明确的可靠性缺口。
2. **性能不占优**：相同弱网下 Bento p95 是 TCP 的 2.2–3.4 倍，重传高 1–2 个数量级；
   端到端吞吐在丢包场景低于 TCP（0.026–0.318 vs 0.205–0.995 Mbps）。
3. **可靠性优于 UDP、弱于 TCP 的握手稳定性**：UDP 按链路丢包率丢弃约 6%–40%，
   Bento 数据面 100%；但 40% 丢包下 Bento 握手 2/3 成功，TCP 3/3。
4. **可优化点**（不在本次改动范围）：PTO 下限应随 SRTT 自适应；ACK 聚合间隔
   与心跳可在大 RTT 下调整；握手重传预算/超时在极端丢包下应更激进。

## 6. 局限

- 单机 loopback，无真实跨网络路径、MTU 分片与交叉流量；netem 只作用于探针端口。
- 每场景仅 3 轮，样本量小；失败轮如实保留，未做重跑筛选。
- Bento 探针为“背靠背发送 + 固定 500ms 收尾”的测试模式，吞吐口径与 TCP/UDP
  不完全一致；p50/p95 与送达率可比性更强。
- baseline 的 TCP/UDP 端到端时间在毫秒级，受进程调度影响，吞吐数值仅作趋势。

## 7. 复现

```bash
# 1) 构建
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-bento-wsl -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=OFF
ninja -C /tmp/build-bento-wsl bento_netem_probe

# 2) 编译 TCP/UDP 探针
g++ -O2 -std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread \
    tests/bento/tcp_probe.cpp -o /tmp/probes/tcp_probe
g++ -O2 -std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread \
    tests/bento/udp_probe.cpp -o /tmp/probes/udp_probe

# 3) 跑矩阵（需要 root 密码，按任务约定 echo root | sudo -S）
bash tests/bento/compare_transports.sh
```
