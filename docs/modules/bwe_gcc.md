# 模块文档：延迟梯度 BWE（GCC / trendline 简化版）

> **状态：现行**（2026-11-08）。基线工作树 `/UserData/CodexWorkSpace/Go2cpp`，
> 构建 `.build-bwe-gcc`（Release，`GO2CPP_WITH_BENTO=ON`）。
> 源码：`include/go2cpp/bento/net/bwe_gcc.hpp`、`src/bento/net/bwe_gcc.cpp`；
> 单测：`tests/net/test_bwe_gcc.cpp`（CTest 名 `bento_bwe_gcc`）。
> 本文只回答三件事：**契约是什么、参数怎么调、什么时候不该用**；
> 算法出处见 [`../research/webrtc-public-network.md`](../research/webrtc-public-network.md) §2。

---

## 0. 30 秒结论

| 问题 | 答案 |
|---|---|
| 它解决什么？ | **丢包之前**发现链路排队：RTT 上升但还没丢包时提前降速（BBR/NewReno 做不到） |
| 输入是什么？ | 逐包反馈 `(send_ms, arrival_ms, payload_bytes)`，按发送顺序串行喂 |
| 输出是什么？ | `TargetBitrate()`，单位**字节/秒**；直接 `Pacer::SetRate(bwe.TargetBitrate(), now_ms)` |
| 依赖什么？ | **零依赖**：只用 C++ 标准库；不依赖 fiber/scheduler/IO/platform |
| 线程安全吗？ | **否**。每连接/方向一个实例，调用方串行 |
| 默认参数从哪来？ | WebRTC `trendline_estimator.cc` / `aimd_rate_control.cc` 的默认值（见 §3） |
| 能量化吗？ | 延迟上升且**零丢包**：GCC 目标码率 **-53.3%**，同场景 BBR pacing **0.0%**（§5） |
| 什么时候不用？ | 没有逐包反馈、反馈批量聚合、发送时间不单调、需要丢包信号、内网固定带宽（§6） |

---

## 1. 契约

### 1.1 数据流

```
发送侧                    接收侧 / 反馈
  │  记录 (seq, send_ms, bytes)
  │──────────────────────────────────► 链路（排队时到达间隔被拉长）
  │                                      │
  │◄─────────────────────────────────────┘ 反馈 (send_ms, arrival_ms, bytes)
  │
  ▼
BweGcc::OnPacketFeedback(send_ms, arrival_ms, bytes)
  │  GccTrendlineEstimator：分组 → 累计延迟 → EWMA → 回归 → 三态
  │  GccAimdRateController：normal 增长 / overuse 降速 + hold
  ▼
TargetBitrate() ──► Pacer::SetRate(rate, now_ms)
```

### 1.2 调用约束（违反会导致判定失真）

| 约束 | 原因 |
|---|---|
| `send_ms` **单调不减** | 乱序反馈会让 `arrival_delta - send_delta` 变负，伪造「延迟下降」；模块丢弃并计入 `dropped_feedback` |
| `arrival_ms >= send_ms` | 同一时钟域；到达早于发送只可能来自坏时钟，直接丢弃 |
| 按**发送顺序**喂，不是按到达顺序 | trendline 的 x 轴是到达时间，但 delta 必须对应同一对包 |
| 反馈密度 ≥ 每 20 个包至少 1 个 | 窗口默认 20，反馈太稀疏会让 trend 长时间不更新（不是错误，但响应慢） |
| 同一实例串行调用 | 内部无锁；多线程调用会撕裂 `deque`/状态机 |

### 1.3 API 速查

| 成员 | 语义 |
|---|---|
| `OnPacketFeedback(send, arrival, bytes=1200)` | 喂一个反馈；首包/5ms 同组/乱序不产生 delta，也不改速率 |
| `TargetBitrate()` / `target_bitrate_bytes_per_sec()` | 当前目标码率（字节/秒，已 clamp 到 `[min,max]`） |
| `state()` | `kNormal` / `kOverusing` / `kUnderusing` |
| `trend()` / `modified_trend()` / `threshold()` | 最近回归斜率（ms/ms）、放大后信号、自适应阈值 |
| `accumulated_delay_ms()` / `smoothed_delay_ms()` | 排队延迟的累计值 / EWMA 值 |
| `hold_until_ms()` / `IsHolding(now_ms)` | 降速后 hold 截止时刻 / 当前是否在 hold |
| `stats()` | 反馈数、有效样本数、丢弃数、降/增次数、hold 挡住次数 |
| `Reset()` | 清空历史，速率回到 `initial_bitrate_bytes_per_sec` |
| `GccTrendlineEstimator` / `GccAimdRateController` | 单独可测的两个子部件（公开类） |

### 1.4 与 Pacer 接线（示例）

```cpp
go2cpp::bento::BweGcc bwe;                       // 每连接一个
go2cpp::bento::Pacer pacer;
pacer.Configure(/*mss=*/1200, /*burst_ms=*/5, /*enabled=*/true);
pacer.SetRate(bwe.TargetBitrate(), now_ms);

// 收到反馈时：
bwe.OnPacketFeedback(send_ms, arrival_ms, payload_bytes);
pacer.SetRate(bwe.TargetBitrate(), now_ms);      // 新速率立即生效
```

> 模块**不**直接依赖 `bento::Pacer`，避免把传输层拖进纯计算库；接线只需两行。
> 若同时使用 `CongestionController`（BBR），建议把两者输出取
> `min(bbr_pacing, gcc_target)` 作为最终发送速率——BBR 负责带宽探测，GCC 负责
> 排队抑制；两者独立实例，互不改状态。

---

## 2. 算法

### 2.1 延迟梯度（trendline）

1. **分组**：与上一包的发送时间差 `< send_time_group_ms`（默认 5ms）时并入同组，
   只推进基准，不产生 delta。避免同一发送时刻的多个包被到达抖动放大。
2. **累计延迟**：`delta = (arrival_delta - send_delta)`，
   `accumulated_delay += delta`。物理含义是「排队时间的一阶积分」：
   链路稳定时恒定，队列增长时线性上升。
3. **EWMA**：`smoothed = 0.9 × smoothed + 0.1 × accumulated`（`smoothing_factor`）。
4. **滑动窗口线性回归**：保存最近 `trend_window`（默认 20）个
   `(arrival_ms, smoothed_delay)`，最小二乘求斜率 `trend`（ms/ms）。
5. **放大**：`modified_trend = min(delta_count, 60) × trend × threshold_gain`。
   `threshold_gain=4.0` 让 20 包窗口的量级与阈值 12.5 可比。
6. **自适应阈值**：`threshold += k × (|modified_trend| - threshold) × dt_s`，
   `k_up=0.0087/s`（正常态）、`k_down=0.039/s`（过载态），clamp `[6, 600]`；
   初始 12.5。阈值会随链路噪声水平缓慢漂移，避免固定阈值在抖动的公网上误判。
7. **三态判定**：`modified_trend > threshold` 记为一次 overuse 信号；
   连续 ≥ `overuse_count_threshold`（默认 2）**且**持续
   ≥ `overuse_time_threshold_ms`（默认 10ms）才切 `kOverusing`；
   负向对称切 `kUnderusing`；否则 `kNormal`。

### 2.2 AIMD（α=1.08/s、β=0.85、降后 hold）

| 状态 | 动作 |
|---|---|
| `kNormal` / `kUnderusing`，且不在 hold | 乘性：`rate *= pow(1.08, dt_s)`；或加性：`rate += 4000 × dt_s` |
| `kOverusing`，且距上次降速 ≥ `decrease_interval_ms` | `rate *= 0.85`，并 hold `hold_ms` |
| `kOverusing`，hold 内 | **不降也不增**（防止每个反馈样本都乘 0.85 造成指数雪崩） |
| 非过载，但仍在 hold | **不增长**，计入 `hold_blocks` |

hold 期间 `last_update` 随每个反馈推进，因此 hold 结束后从当前时刻开始增长，
**不会补涨** hold 期累积的「欠账」。

### 2.3 与单测场景的对应

| 场景 | 构造 | 期望 |
|---|---|---|
| 延迟上升 | 发送 10ms/包，到达 13ms/包（delta=3ms），无丢包 | `kOverusing`，降速 |
| 稳定 | 发送=到达=10ms/包 | `kNormal`，缓慢增长 |
| 降后 hold | 稳定 → 排队 → 第一次降速后停 | hold 内速率不变，hold 后恢复增长 |
| 加性增 | `increase_mode=kAdditive` | 每秒 +4000 B/s |
| BBR 对照 | 同样 200 包，RTT 50→150ms，无丢包 | GCC -53.3%；BBR pacing/cwnd 轨迹逐步相同 |

---

## 3. 参数

| 参数 | 默认 | WebRTC 对照 | 调大/调小的后果 |
|---|---|---|---|
| `send_time_group_ms` | 5.0 | `kSendTimeGroupLength` | 调大 → 反馈更稀疏、抗抖动强但响应慢 |
| `trend_window` | 20 | `TrendlineEstimator` 默认窗口 | 调大 → 斜率更稳、过载发现更晚 |
| `smoothing_factor` | 0.9 | `kSmoothingFactor` | 调大 → 更平滑、更滞后 |
| `threshold_gain` | 4.0 | `threshold_gain_` | 调大 → 对同一斜率更敏感 |
| `initial_threshold` | 12.5 | `kDefaultThreshold` | 调小 → 更早降速、更易误判 |
| `min_threshold` / `max_threshold` | 6 / 600 | `kMinThreshold` / `kMaxThreshold` | 自适应阈值的夹逼区间 |
| `threshold_up_per_sec` | 0.0087 | `k_up` | 正常态阈值上升速度 |
| `threshold_down_per_sec` | 0.039 | `k_down` | 过载态阈值上升速度（避免持续误判） |
| `overuse_count_threshold` | 2 | 无（WebRTC 只有时间条件） | 调大 → 更保守、更晚降速 |
| `overuse_time_threshold_ms` | 10 | `overuse_time_threshold_ms` | 调大 → 过滤短尖峰 |
| `initial_bitrate_bytes_per_sec` | 204800（200 KiB/s） | 由应用/REMB 给 | 应接近链路可用带宽的保守下界；过低会让启动期长期低于容量 |
| `min_bitrate_bytes_per_sec` | 12500（100 kbps） | WebRTC `kMinBitrateBps=10000` bps（=1250 B/s） | 保证最差链路仍有最低速率 |
| `max_bitrate_bytes_per_sec` | 52428800（50 MiB/s） | 应用配置 | 防止无反馈时无限增长 |
| `alpha` | 1.08 | `kIncreaseFactor` | 每秒增长比例；调大 → 更快占满带宽、更易排队 |
| `beta` | 0.85 | `kDecreaseFactor` | 每次降速比例；调小 → 降得更狠 |
| `hold_ms` | 500 | WebRTC `kRcHold`（事件驱动，无固定值） | 调大 → 更稳、恢复更慢 |
| `decrease_interval_ms` | 500（≥ hold） | 无直接对应 | 持续排队时两次降速的最小间隔 |
| `increase_mode` | `kMultiplicative` | 默认乘性；有 link capacity 时加性 | 见 §4 第 2 条 |
| `additive_increase_bytes_per_sec` | 4000 | `kAdditiveIncreaseBps` 下限 | 仅加性模式生效 |

---

## 4. 与 WebRTC 的差异（**不要当成 GoogCC 等价物**）

| # | WebRTC | 本模块 | 影响 / 补偿 |
|---|---|---|---|
| 1 | 5ms 分组 + `InterArrivalDelta` 的组间/组内完整状态机 | 简化为「与上一包差 < 5ms 则更新基准」 | 包间隔 ≥5ms 时等价；密集小包场景 delta 偏稀疏，响应略慢 |
| 2 | AIMD 有 link capacity 估计时切**加性增** `max(4000, avg_pkt/(2×(rtt+100ms)))` | 默认**乘性增** α=1.08/s，加性模式固定 4000 B/s | 本模块无 `avg_pkt/rtt` 的容量估计；需要时应按 RTT 动态设置 `additive_increase_bytes_per_sec` |
| 3 | 降速用 `throughput × 0.85 − 5 kbps`，增长上限 `1.5 × throughput + 10 kbps` | 降速 `rate × 0.85`；**无吞吐上限** | 无吞吐量反馈时更激进；若应用有确认速率，应在外部对 `TargetBitrate()` 取 min |
| 4 | 降后 `kRcHold` 是**状态机**，直到 `kBwNormal` 才恢复增长 | 固定 `hold_ms`（默认 500ms）时间窗 | 简化、可预测；但拥塞持续时 500ms 后会再降一次（由 `decrease_interval_ms` 控制） |
| 5 | overuse 判定用「持续 10ms + trend 未回落」 | 「连续 N 次 + 持续 10ms」双条件 | N=2 时与 WebRTC 接近；N 可配置为更保守 |
| 6 | 阈值更新按秒、`k_up/k_down` 与假设状态联动 | 相同公式与系数，单步 dt 上限 1s | 长时间无反馈时不会跳变到 600 |
| 7 | 有 REMB/transport-cc 反馈解析、丢包模型、ALR/ProbeRTT | **全部没有** | 需要应用层提供逐包反馈；丢包信号请继续用 BBR/NewReno |
| 8 | 用 `arrival_time` 排序并可处理乱序 | 要求按**发送顺序**喂；乱序丢弃 | 调用方需在发送侧记录并保序回传（RPC 批量 ACK 需拆成逐包反馈） |
| 9 | 有 pacer 的 debt 模型与「积压提速排空」 | 不实现 pacer，只输出速率 | 由 `bento::Pacer` 消费；其 debt 改造见 P0-1 的另一项任务 |
| 10 | 有帧率/帧大小归因 | 无（RPC 无帧概念） | 大消息会推高到达间隔方差，必要时按字节分桶（本模块未做） |

---

## 5. 验证

### 5.1 单测（`tests/net/test_bwe_gcc.cpp`）

```bash
cmake -S . -B .build-bwe-gcc -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=ON
cmake --build .build-bwe-gcc --target bento_test_bwe_gcc -j12
ctest --test-dir .build-bwe-gcc -R bento_bwe_gcc --output-on-failure
```

实测输出（2026-11-08，`43 checks, 0 failures`）：

```
[INFO] delay-rise: initial=204800 stable=207817 after=151961 B/s trend=0.2296 modified=55.10 threshold=13.30 state=overusing decreases=2
[INFO] stable: initial=204800 final=235049 B/s ratio=1.1477 increases=179 decreases=0
[INFO] hold: rate_after=178778 hold_until=1119 after_hold=182953 B/s hold_blocks=26
[INFO] additive: initial=204800 final=211960 (+7160 B/s)
[INFO] compare: gcc stable=235049 rising=109792 B/s (-53.3%), bbr stable=90000 rising=90000 B/s (+0.0%), bbr loss=0, bbr cwnd=10/10
[test_bwe_gcc] 43 checks, 0 failures
```

### 5.2 「延迟上升但无丢包」的对照（核心价值证明）

同一 200 包节奏、**零丢包**，只把 RTT 样本从 50ms 抬到 150ms：

| 控制器 | 稳定 RTT | RTT 上升到 150ms | 变化 | 结论 |
|---|---|---|---|---|
| **GCC（本模块）** | 235049 B/s | 109792 B/s | **-53.3%** | 延迟上升 → 提前降速 |
| **BBR（`CongestionController`）** | 90000 B/s | 90000 B/s | **0.0%** | 延迟上升 → 完全无响应 |

> BBR 的 90000 B/s 是「带宽估计 120 kB/s × drain gain 0.75」的结果，与 RTT
> 无关；测试逐步比较了两组的 pacing/cwnd 轨迹，**每一步都相同**（`bbr_trajectory_identical`）。
> 这正是 GCC 的价值：BBR 要等**丢包**才降，GCC 在排队阶段就降。

### 5.3 构建、基线对比与可移植性

```bash
# Linux 全量构建（Release，Bento ON + tests ON）
cmake --build .build-bwe-gcc -j12          # 968/968 成功，零 error
# 纯计算模块的跨平台语法检查（不依赖 __linux__）
g++ -std=c++17 -U__linux__ -Iinclude -Wall -Wextra -Wpedantic -Werror \
    -fsyntax-only src/bento/net/bwe_gcc.cpp
```

- **全量构建**：968/968 成功，**零 error**；3 个 warning 全部来自既有文件
  （`src/tunnel/net.cpp:529`、`tests/os/monitor/test_monitor_e2e.cpp:14`、
  `tests/os/monitor/test_tcp_io_monitor.cpp:27`），在基线 `332e4aa` 上同样存在；
  本模块目标带 `-Wall -Wextra -Wpedantic -Werror`，**零告警**。
- **`ctest -R bento` 基线对比**（基线 worktree `/tmp/go2cpp-base-332e4aa`，62 项）：
  - 新增 `bento_bwe_gcc` **通过**（43 checks）；
  - `bento_http_server`、`bento_session_table` 在基线**同样失败**（既有问题）；
  - `bento_udp_transport` 首轮失败，重跑 3/3 通过（flaky）；
  - `bento_ws_server` 首轮被中断，重跑 2/3 通过、1 次挂起；基线单次通过——
    既有测试偶发挂起，与本模块无关；
  - `bento_bench_weaknet` 为性能基准，两侧均被人工中断，不计入回归。

---

## 6. 不适用范围

| 场景 | 原因 | 应该用什么 |
|---|---|---|
| 没有逐包反馈（只有整批 ACK） | 无法计算 `arrival_delta - send_delta` | 先把 ACK 拆成逐包反馈；或继续用 BBR（带宽估计不需要逐包时序） |
| 反馈里没有 `send_time` | 同上；`arrival` 单独无法判断排队 | 发送侧记录 `send_ms` 随包/随反馈带回 |
| 发送时间不单调（多线程乱序发包、时钟回拨） | delta 会为负；模块会丢弃，长期无样本则不降速 | 发送侧串行化时间戳（或每线程独立实例） |
| 链路容量固定且远大于需求（内网） | 延迟梯度只会增加误判风险 | 关闭本模块，用 BBR/固定窗口 |
| 需要「丢包即降速」的强反应 | GCC 对丢包不敏感（这是设计取舍） | 与 NewReno/BBR 并行，取 min；或加丢包模型 |
| 需要精确的链路容量（不是「不排队」） | GCC 只回答「是否在排队」，不估计容量 | BBR 的带宽探测 |
| 超低延迟控制流（<10ms 周期） | 5ms 分组 + 20 包窗口需要至少几十个反馈才收敛 | 固定小窗口 + 优先级队列（P0-3 QoS） |

---

## 7. 构建开关与文件清单

| 项 | 值 |
|---|---|
| CMake target | `go2cpp_bwe_gcc`（静态库，别名 `go2cpp::bwe_gcc`） |
| 构建条件 | `GO2CPP_WITH_BENTO=ON`（与 Bento 同目录组织；模块本身不链接 `go2cpp_bento`） |
| 测试 target | `bento_test_bwe_gcc`（链接 `go2cpp_bwe_gcc` + `go2cpp_bento`，后者仅用于 BBR 对照） |
| CTest 名 | `bento_bwe_gcc` |
| 新增文件 | `include/go2cpp/bento/net/bwe_gcc.hpp`、`src/bento/net/bwe_gcc.cpp`、`tests/net/test_bwe_gcc.cpp`、`docs/modules/bwe_gcc.md` |
| 修改文件 | `cmake/Go2CppBento.cmake`（仅新增 target/测试注册，不改现有目标） |
