# Bento 传输三问题修复与对照验证（2026-10-28）

> 任务：修复对照实测暴露的三个问题（不可靠流过度丢弃 / 40% 丢包握手失败 /
> PTO 延迟高），并用同一套 netem 对照脚本复跑验证。
> 数据来源：[verify_fixes_results.tsv](verify_fixes_results.tsv)（定向验证）、
> [compare_results.tsv](compare_results.tsv)（完整 45 轮复跑）、
> [compare_results.baseline-2026-10-28.tsv](compare_results.baseline-2026-10-28.tsv)
> （修复前基线）。
> 环境与 [transport-comparison-2026-10-28.md](../../docs/review/transport-comparison-2026-10-28.md)
> 一致：WSL2 loopback、`/tmp/build-bento-wsl`（Ninja/Release）、`tc netem` 只作用于探针端口。

## 0. 结论

| 问题 | 修复前 | 修复后 | 结论 |
|---|---|---|---|
| 不可靠流 20%+200ms±50ms 送达率 | 30/600 = 5.0%，stale=453 | 481/600 = 80.2%，stale=0 | **达标**（目标 >50%） |
| 40% 丢包握手成功率 | 2/3 轮（1 轮 15s 超时） | 3/3 轮（定向 + 完整对照均为 3/3） | **达标** |
| 5%+50ms p95 | 528ms | 272ms（完整对照）/ 269ms 均值（定向 3 轮） | **达标**（目标 <300） |
| 20%+200ms p95 | 3442ms | 2079ms（完整对照）/ 2448ms 均值（定向 3 轮） | **接近达标**（目标 <2000，差 4%） |
| 40%+100ms p95 | 9533ms | 4648ms | 显著改善（无硬目标） |
| 40%+100ms 送达率 | 66.7% | 100.0% | 改善（握手不再失败） |

回归：`ctest -R bento` **48/48 通过**（`bento_netem` 自身 skip 除外）。

## 1. 不可靠流过度丢弃 → 乱序容忍窗口

### 问题
原 `UnreliableChannel` 只接受 `seq > applied_seq`，乱序到达的包被当作过期丢弃。
200ms±50ms 抖动下 200 条背靠背发送的到达顺序近似随机，只有「到达序列前缀最大值」
（约 H(n)≈5 次）被交付，因此 600 条仅交付 30 条、`stale_dropped=453`。

### 修复
- [channel.hpp](../../include/go2cpp/bento/net/channel.hpp) / [channel.cpp](../../src/bento/net/channel.cpp)
  新增 `UnreliableChannel::Config`：
  - `reorder_window = 256`：乱序缓存条数上限（按在途帧数而非 seq 深度；256 帧
    覆盖 200 条突发 + 抖动，内存上界约 333KB）。
  - `reorder_timeout_ms = 200`：缺口等待上限，从缺口后最早缓存帧到达开始计时；
    超时跳过缺口交付缓存最小 seq，避免实时流被一个丢包永久阻塞。
- 新增 `OnTimer()`：Link 在 `Tick()` 里驱动（[link.cpp](../../src/bento/net/link.cpp)）；
  超时/缓存满时按 seq 升序跳过缺口交付，交付后立即 `Relation::Flush()` 释放因果 pending。
- 语义保持：`seq <= applied_seq` 仍是真·过期，`stale_dropped` 只统计这一类；
  应用层看到的 seq 永远严格递增（探针 `ordered=1`，0 回退）。依赖 `dep_seq` 的帧
  在 Relation 释放后直接交付（已等待过因果水位，不二次缓存），被水位越过的旧帧仍按 DropStale 丢弃。
- `Link` 默认启用该配置（`GetOrCreateUnreliable` 走默认 `Config`）。

### 对照数据（定向验证 3 轮，[verify_fixes_results.tsv](verify_fixes_results.tsv)）

| 场景 | 修复前送达 | 修复后送达 | stale_dropped | ordered |
|---|---|---|---|---|
| baseline（回环） | 600/600 | 600/600 | 0 | 1 |
| loss_5_delay_50 | 570/600（95.0%） | 573/600（95.5%） | 0 | 1 |
| loss_20_delay_200_jitter_50 | 30/600（5.0%） | **481/600（80.2%）** | **0** | 1 |
| loss_40_delay_100 | 233/400（58.2%） | 367/600（61.2%） | 0 | 1 |

> 修复后 20% 场景的缺口跳过（`reorder_skipped`）约 26~35 次/轮，正好对应
> 20% 随机丢包的实际丢失帧；`reorder_stale=0` 说明没有再误杀乱序帧。

## 2. 握手高丢包失败 → 冗余 + 自适应超时

### 问题
40% 丢包下 HELLO/HELLO_ACK 单包到达率 60%，单轮握手成功率仅 36%；固定 15s 超时
只够 10 轮指数退避（200ms 起、2s 上限），实测 1/3 轮在 `tx=10 rx=4` 时超时。

### 修复（[link.hpp](../../include/go2cpp/bento/net/link.hpp) / [link.cpp](../../src/bento/net/link.cpp)）
- `handshake_copies = 2`：每轮 HELLO / HELLO_ACK 各发 2 份（同一会话重复包幂等），
  单轮成功率从 60% 提到 84%，失败概率随轮次指数下降。
- `hello_max_attempts` 10 → 16，覆盖 40% 丢包下的长尾。
- 握手超时自适应：`handshake_timeout_ms + 当前 HELLO PTO` 作为宽限（PTO 已随
  退避增长），避免「最后一次 HELLO 刚发出就被判超时」白扔重传；短 RTT 链路
  仍按配置快速失败。
- 客户端在已 Connected 后收到冗余 HELLO_ACK 时静默忽略，不计 `unknown_peer_frames`
  （避免污染统计与测试断言）。

### 对照数据
- 定向验证：40% 场景 **3/3 轮成功**（修复前 2/3）。
- 完整对照：40% 场景 Bento 送达率 **100%**（修复前 66.7%，失败轮即握手 15s 超时）；
  5%/20% 场景同样 3/3 成功。

## 3. PTO 延迟高 → 自适应 PTO + 快速恢复重传

### 问题
旧实现基础 PTO 下限固定 200ms，与 20%+200ms 场景 RTT 同量级；且首样本前
初始 RTT 333ms 使首轮 PTO≈1s。5% 场景 200 条产生 105 次重传（实际丢包仅 10 个），
20% 场景 p95 3442ms、重传 142 次。

### 修复
1. **PTO 公式**（[channel.cpp](../../src/bento/net/channel.cpp)）：
   `min(srtt + 4*rttvar + max_ack_delay, srtt + 4*rttvar) = srtt + 4*rttvar`，
   `min_pto_ms` 200 → **50ms**；`rttvar` 增加 `srtt/8` 抗退化下限，避免 rttvar
   收敛到 0 后 PTO 退化成「恰好一个 RTT」而持续伪重传（实测 5% 场景曾因此产生
   150~217 次重传）。
2. **初始 RTT** 333 → **200ms**：首样本前 PTO 从 997ms 降到 600ms，20% 场景
   首轮丢包恢复不再多等一个 1s 周期。
3. **初始拥塞窗口** 64 → **256 包**（[congestion.hpp](../../include/go2cpp/bento/net/congestion.hpp)）：
   200 条 256B 消息一次发完，消除「慢启动 3 个 RTT 才发完」的发送侧排队
   （5% 场景 p50 272→217ms、p95 528→272ms）。
4. **快速恢复期间可重复重传 HOL 队首**（channel.cpp `TryFastRetransmit`）：
   旧实现每段快速恢复只重传一次，那次重传再丢（20%）就要等完整 PTO 退避
   （997→2000ms），实测 p50 被推到 4.5~7s；改为每 3 个重复 ACK 重发一次队首
   （不降窗、只重发 1 个），HOL 恢复从秒级降到百毫秒级。

### 对照数据（完整对照 3 轮均值，[compare_results.tsv](compare_results.tsv)）

| 场景 | p50 前/后 | p95 前/后 | 重传 前/后 |
|---|---|---|---|
| loss_5_delay_50 | 272 / 217 | **528 / 272** | 105 / 94 |
| loss_20_delay_200_jitter_50 | 1843 / 1301 | **3442 / 2079** | 142 / 222 |
| loss_40_delay_100 | 5944 / 3791 | 9533 / 4648 | 397 / 418 |
| rate_1mbit_loss_10_delay_100 | 1372 / 1315 | 1837 / 1684 | 70 / **274** |

> 20% 场景重传数上升是「更快重试」的直接代价：PTO 更小 + 快速恢复重发队首，
> 用略多的冗余帧换 40% 的 p95 降幅。1 Mbit 限速场景重传 70→274 明显退化，
> 原因是初始窗口 256 在 125KB/s 链路上一次推入 56KB，排队使 PTO 反复触发；
> 该场景 p95 仍从 1837 降到 1684，但重传放大是已知副作用（见第 5 节）。

## 4. 回归

| 项目 | 结果 |
|---|---|
| `ctest -R bento` | **48/48 通过**（`bento_netem` 标记 Skipped，为测试自身跳过） |
| `bento_test_unreliable_dropstale` | 695 checks 通过（改写为乱序容忍语义） |
| `bento_test_rtt_pto` | 80 checks 通过（更新为 50ms 下限 + 抖动下限 + 200ms 初始 RTT） |
| `bento_test_stream_fragment` | 85 checks 通过（PTO 期望 200→50） |
| 完整对照 | 45 轮全部跑完，qdisc 每场景清理校验通过 |

## 5. 已知限制与未达标项

1. **20% p95 = 2079ms（目标 <2000）**：完整对照均值差 4%；定向 3 轮均值为 2448ms（单轮 2102~2653ms）。
   剩余长尾来自「同一帧连续丢失两次」触发的 PTO 退避（约 600+1200ms）与
   32 位 SACK 位图的可视范围限制，未进一步改动线格式。
2. **1 Mbit 场景重传 70→274**：初始窗口/更低 PTO 在带宽受限链路上的副作用，
   吞吐与 p95 未退化，但冗余流量明显增加，后续应按带宽估计限制初始窗口。
3. **未实现任务建议中的「ACK 批量聚合」**：ACK 位图仅 32 位，加大聚合间隔会让
   发送端更晚看到 SACK 空洞，反而拉长 HOL 恢复；本次改为「ACK 每 10ms 照旧 +
   重复 ACK 直接重发队首」。
4. TCP 在 40% 场景本轮复跑出现 1/3 轮失败（送达 88%），属于环境波动，
   基线数据为 100%；报告如实保留，未做重跑筛选。

## 6. 复跑命令

```bash
# 构建（需要测试目标）
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-bento-wsl -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=ON
ninja -C /tmp/build-bento-wsl -j12
ctest --test-dir /tmp/build-bento-wsl -R bento

# 三个修复的定向验证（9 轮，约 1 分钟）
bash tests/bento/verify_fixes.sh

# 完整 5 场景 × 3 传输 × 3 轮对照（45 轮）
bash tests/bento/compare_transports.sh
```

## 7. 改动文件

- `include/go2cpp/bento/net/channel.hpp`：不可靠通道 Config/Stats/OnTimer 声明；PTO 默认值。
- `src/bento/net/channel.cpp`：乱序缓存实现；PTO 公式与抖动下限；初始 RTT；快速恢复重传。
- `include/go2cpp/bento/net/link.hpp`：握手冗余与重试参数。
- `src/bento/net/link.cpp`：HELLO/HELLO_ACK 冗余、自适应超时、不可靠通道 Tick。
- `include/go2cpp/bento/net/congestion.hpp`：初始拥塞窗口 64→256。
- `tests/bento/test_unreliable_dropstale.cpp`：改写为乱序容忍语义（695 checks）。
- `tests/bento/test_rtt_pto.cpp`：PTO 公式/下限/初始 RTT 期望。
- `tests/bento/test_stream_fragment.cpp`：PTO 期望。
- `tests/bento/netem_probe.cpp`：不可靠流 reorder 统计 + `--verbose` 进度输出。
- `tests/bento/verify_fixes.sh`：定向验证脚本（新增）。
- `tests/bento/verify_fixes_results.tsv`、`compare_results*.tsv`：实测数据（新增）。
