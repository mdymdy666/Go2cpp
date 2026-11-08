# 模块文档：自适应 FEC 冗余度（fec_controller）

> **状态：现行**（2026-11-08）。基线工作树 `/UserData/CodexWorkSpace/Go2cpp`，
> 构建 `.build-fec-ctrl`（Release，`GO2CPP_WITH_BENTO=ON` + tests ON）。
> 源码：`include/go2cpp/net/fec_controller.hpp`、`src/net/fec_controller.cpp`；
> 单测：`tests/net/test_fec_controller.cpp`（CTest 名 `go2cpp_fec_controller_test`）。
> 本文只回答四件事：**契约是什么、参数怎么调、与 WebRTC 差在哪、什么时候不该用**；
> 算法出处见 [`../research/webrtc-public-network.md`](../research/webrtc-public-network.md) §3。

---

## 0. 30 秒结论

| 问题 | 答案 |
|---|---|
| 它解决什么？ | `bento::FecConfig` 的冗余度是**固定**的：好网白传带宽、差网保护不足。本模块按丢包率闭环调冗余 |
| 输入是什么？ | 每个保护周期（建议 1s）一次**原始**丢包率 `[0,1]`（丢包数 / 期望到达数） |
| 输出是什么？ | `RecommendedRedundancy()`：冗余比例，0.1 = 每 100 字节媒体多发 10 字节校验；或 `RecommendedParityShards(k)` 直接换成 m |
| 上限是多少？ | **50%**（`kMaxTotalProtectionOverhead`），配置也突破不了 |
| 丢包 0% / 10% / 50% 时给多少？ | **2%** / **15.7%** / **50%** |
| 抖动 1%↔10% 反复呢？ | 200 个采样只变 **3** 次；后 100 个采样输出**极差 0**（瞬时映射同区间跳 99 次） |
| 依赖什么？ | **零依赖**：只用 C++ 标准库；不依赖 fiber/scheduler/时钟/IO |
| 线程安全吗？ | **否**。每通道/每方向一个实例，调用方串行 |
| 默认参数从哪来？ | 三段式（滤波丢包率 → 冗余比例 → 50% 上限）对齐 WebRTC `fec_controller_default.cc`；具体系数是本项目按 RPC 场景重定的（§4） |
| 能量化吗？ | 0% 丢包时：固定 20% 冗余每 100 万包白传 **228.9 MiB**，自适应只传 **22.9 MiB**，省 **206.0 MiB**（媒体的 18%，§5.2） |
| 什么时候不用？ | 突发型丢包、丢包 >50%、低 RTT + 重传更便宜、带宽极紧、统计窗口太小、多流共享链路（§6） |

---

## 1. 契约

### 1.1 数据流

```
 可靠层/统计器（调用方）
   │  每个保护周期统计：loss = 丢包数 / 期望到达数
   ▼
FecController::OnLossSample(loss)
   │  step1 夹紧 [0,1]
   │  step2 EWMA：f = α·loss + (1−α)·f
   │  step3 映射：target = piecewise_linear(f)
   │  step4 幅度闸门（up/down margin）
   │  step5 驻留闸门（dwell_samples）
   │  step6 降档确认闸门（down_confirm_samples）
   ▼
RecommendedRedundancy() ∈ [min, 50%]
   │
   ▼
FecConfig::parity_shards = RecommendedParityShards(data_shards)
   │
   ▼
bento::FecEncoder → 校验分片（每组头里带 parity_shards，在途组不受影响）
```

### 1.2 调用约束（违反会导致判定失真）

| 约束 | 原因 |
|---|---|
| 输入是**原始**周期丢包率，不要预先滤波 | 模块内部做 EWMA；二次滤波会让响应变钝、参数含义重叠 |
| 周期**固定**（建议 1s） | `dwell_samples` / `down_confirm_samples` 以「采样数」为单位；周期变化会让滞后时间跟着变 |
| 每个周期**有足够包数**（建议 ≥20） | 20 个包时最小可分辨丢包率是 5%，统计噪声会直接变成冗余抖动 |
| 同一实例**串行**调用 | 内部无锁、无原子 |
| 丢包率口径 = 丢包数 / 期望到达数（含重传后仍丢的） | 用「收到数 / 发送数」会把重传算成新包，低估丢包率 |
| 改 `parity_shards` 只影响**下一个** FEC 组 | `Channel::FlushFecGroup` 在组完成时才读配置并建编码器；组头带 `parity_shards`（`protocol.cpp` 校验 1..16），已发出的组按各自组头解码 |

### 1.3 API 速查

| 成员 | 语义 |
|---|---|
| `OnLossSample(loss)` | 喂一个周期采样，返回新的推荐冗余；NaN/负按 0、>1 按 1 |
| `RecommendedRedundancy()` | 当前冗余比例（已含滞后），∈ `[min_redundancy, 50%]` |
| `RecommendedParityShards(k, max_m=16)` | m = round(冗余 × k)，夹到 `[0, max_m]`；k=0 或 max_m=0 返回 0 |
| `RedundancyForLoss(loss)` | **稳态映射**（不改状态），诊断/对照用 |
| `FilteredLossRate()` | EWMA 后的丢包率（闭环中间量） |
| `samples()` / `changes()` / `up_changes()` / `down_changes()` | 采样数 / 变更次数 / 升 / 降（抖动指标） |
| `config()` | 修正后的配置（非法值读回实际值） |
| `Reset()` | 清状态回构造态（配置保留） |
| `kMaxTotalProtectionOverhead` | 常量 `0.50`，硬上限 |

### 1.4 接线示例

```cpp
#include "go2cpp/net/fec_controller.hpp"

go2cpp::net::FecController fec_ctl;      // 每连接/每方向一个，与 channel 同生命周期

// 每秒（或每个统计窗口）：
const double loss = static_cast<double>(lost) / static_cast<double>(expected);
fec_ctl.OnLossSample(loss);

const std::uint16_t k = /* 通道的 data_shards，例如 8 */;
const std::uint16_t m = fec_ctl.RecommendedParityShards(k, /*max_m=*/16);
// m 即下一次 FEC 组该配的 parity_shards。
```

> 模块**不**链接 `bento`、不改 `FecConfig`、不碰编码器；接线只需把 `m` 写进
> 通道配置的 `fec.parity_shards`。
>
> ⚠️ **当前 `bento::Channel` 的 `config()` 是只读的**（`channel.hpp:115`），
> 运行期改 `parity_shards` 需要 Channel 侧提供一个 setter —— 这属于后续接线
> 工作（本次只交付纯计算模块，不改现有 FEC 实现）。在 setter 落地前，本模块
> 仍可用于：连接建立前按历史丢包率算初始冗余、或给已支持动态配置的接入方使用。
>
> 若使用 XOR（`FecKind::kXor`，m 必须为 1），传 `max_m=1`：冗余 >0 时得到 1，
> 冗余不足半个包时得到 0（表示「该组不值得加校验」）。

---

## 2. 算法

### 2.1 EWMA 滤波（为什么不用瞬时丢包率）

```
f_i = α · loss_i + (1 − α) · f_{i−1},   f_0 = loss_0
```

- 首个采样直接作为初值：冷启动不从 0 爬，前几个周期就能给出正确保护。
- α 默认 0.25 ≈ 4 个采样的时间常数（1Hz 下 4s）。**这是抗抖动的第一道**。
- 单测实测：1%↔10% 交替 200 次，滤波后收敛到 6.14%（真实均值 5.5%，
  交替采样下 EWMA 停在两极点之间，属正常相位偏差）。

### 2.2 丢包率 → 冗余比例（分段线性）

```
              min_redundancy                       max_redundancy (≤0.5)
                    │                                      │
   r(loss) = min ───┤                                      │
                    │           线性插值                    │
                    └──────────●───────────●───────────────┘
                    loss_low           loss_high
                    (0.02)              (0.30)
```

- `loss ≤ loss_low` → 下限（死区，不为偶发丢包付冗余）；
- `loss ≥ loss_high` → 上限（饱和，丢包再高也不无限加）；
- 中间线性插值。

默认值对应的关键点：

| 滤波丢包率 | 0% | 2% | 5% | 8% | 10% | 20% | 30%+ |
|---|---|---|---|---|---|---|---|
| 冗余比例 | 2.0% | 2.0% | 7.1% | 12.3% | **15.7%** | 32.9% | **50.0%** |

为什么 10% 丢包给 15.7% 而不是「刚好够」的 11%：留 4~5 个百分点的突发余量——
平均 10% 的链路总有一秒是 15%，按 11% 配会在每次小突发整组失效。

为什么饱和点取 30%：50% 冗余的 MDS 组只能恢复 **1/3** 丢包（`r/(1+r)`），
再往上加冗余已补不住，必须靠重传/降码率（§6）。

### 2.3 滞后：三道闸门（**升快降慢**）

| 闸门 | 默认 | 作用 |
|---|---|---|
| **幅度** | `up_margin=0.02` / `down_margin=0.05` | 目标比当前高 2 个百分点才升；低 5 个百分点才降。降的门槛更高 = 保护不轻易拆 |
| **驻留** | `dwell_samples=2` | 两次变更之间至少隔 2 个采样，防「升一点降一点」爬行 |
| **降档确认** | `down_confirm_samples=3` | 降档信号要连续 3 个采样成立，单次异常采样不能把保护打掉 |

**贴底特例**：`target == min_redundancy`（丢包已回到死区）时**忽略
`down_margin` 直接允许降回下限**。否则绝对门限会留下「降到下限 + 5 个百分点
就停住」的死区，好网永远清不掉残余冗余——单测 `TestClosedLoopRecovery`
覆盖（10% → 0% 后第 8 个采样回到 2.0%）。

### 2.4 输出换算

```
m = round(redundancy × data_shards)，夹到 [0, max_parity]
```

- `k=8, 冗余 15.7%` → `m = round(1.256) = 1`；
- `k=16, 冗余 15.7%` → `m = round(2.51) = 3`；
- `k=8, 冗余 2%` → `m = round(0.16) = 0`（好网连 1 个校验包都不发）。

---

## 3. 参数

| 参数 | 默认 | 含义 | 调大 / 调小的后果 |
|---|---|---|---|
| `ewma_alpha` | 0.25 | 滤波系数 α ∈ (0,1] | 调大 → 响应快、抖动大；调小 → 平滑、恢复慢 |
| `min_redundancy` | 0.02 | 冗余下限 | 调大 → 好网也带保护（卫星/高 RTT 建议 0.10）；调 0 → 好网零冗余 |
| `max_redundancy` | 0.50 | 冗余上限 | **被夹到 50%，改不上去**；调小 → 更省带宽但差网补不住 |
| `loss_low` | 0.02 | 死区上沿 | 调大 → 更容忍小丢包；调 0 → 任何丢包都加冗余 |
| `loss_high` | 0.30 | 饱和点 | 调小 → 更早满冗余（更保守）；调大 → 高丢包时仍给不足 |
| `up_margin` | 0.02 | 升档门限 | 调大 → 升得少、响应钝；调小 → 更贴目标、更易抖 |
| `down_margin` | 0.05 | 降档门限 | 调大 → 保护更顽固；调小 → 更易降、更省带宽 |
| `down_confirm_samples` | 3 | 降档连续确认次数 | 调大 → 更抗异常采样、恢复更慢 |
| `dwell_samples` | 2 | 两次变更最小间隔（采样数） | 调大 → 更稳、跟踪更慢 |
| `min_samples_before_adapt` | 1 | 预热采样数 | 调大 → 冷启动更保守（前 N 秒用下限） |

**调参速查**

| 场景 | 建议 |
|---|---|
| 内网 / 专线（丢包≈0） | `min_redundancy = 0`，其余默认（或直接不启用 FEC） |
| 公网 RPC（默认） | 全部默认 |
| 卫星 / 跨洋（RTT 200ms+，重传昂贵） | `min_redundancy = 0.10`，`ewma_alpha = 0.15`（更慢更稳） |
| 实时音视频（秒级预算） | `ewma_alpha = 0.5`，`dwell_samples = 1`（响应优先） |
| 统计窗口 < 1s | 先修统计窗口：包数不足时丢包率噪声会直接变成冗余抖动 |

---

## 4. 与 WebRTC 的差异（**不要当成 `VCMLossProtectionLogic` 等价物**）

| # | WebRTC `fec_controller_default.cc` | 本模块 | 影响 / 补偿 |
|---|---|---|---|
| 1 | 用 **max filter**（窗口内最大丢包率）抗抖 | 用 **EWMA**（一阶低通） | max filter 偏保守（突发按最坏值给冗余，浪费带宽）；EWMA 更省但会漏掉单次尖峰。要更保守可调大 α 或调小 `loss_high` |
| 2 | `fec_rate` 由**滤波丢包率 + RTT + 目标码率 + 帧率 + 帧类型（key/delta）**共同决定 | 只用滤波丢包率 | RPC/数据通道没有帧与码率概念；RTT 的影响体现在「重传成本」上，由调用方决定是否提高 `min_redundancy` |
| 3 | 开销预算 = **NACK + FEC 合计 ≤50%** | 只有 FEC 侧 ≤50% | Go2Cpp 的重传由可靠层负责、不占 FEC 预算；因此本模块的 50% 是「FEC 单独」的口径，比 WebRTC 宽 |
| 4 | 有 `kFecMaskRandom` / `kFecMaskBursty` 两种掩码与私有码表 | 不选掩码 | 掩码由现有 `bento::FecEncoder`（XOR / Cauchy RS）决定，本模块只给「要多少冗余」 |
| 5 | 每**帧**更新一次（视频帧驱动） | 每**周期**更新（建议 1s） | 数据通道无帧概念；周期越短越灵敏、越费 CPU |
| 6 | 开销超 50% 时**反压源码率** | 不反压，只夹冗余 | Go2Cpp 不能丢 RPC 消息；需要限速时由 BBR/GCC 与队列负责 |
| 7 | 无显式滞回（靠 max filter 窗口 + 帧级平滑） | **显式三闸门**（幅度/驻留/确认） | 本模块的滞后是独立可配的，行为可预测、可单测 |
| 8 | `fec_rate` 是「FEC 包 / 媒体包」 | 冗余比例是「校验字节 / 媒体字节」 | 分片等长时两者等价；不等长时字节口径更准 |

> 一句话：**采样方式（EWMA vs max filter）与决策结构（三闸门 vs 帧级平滑）
> 是本项目的工程选择，不是对 WebRTC 的复刻**；只有「滤波丢包率 → 冗余 →
> 50% 上限」这条主线对齐。

---

## 5. 验证

### 5.1 单测（`tests/net/test_fec_controller.cpp`）

```bash
cmake -S . -B .build-fec-ctrl -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=ON
cmake --build .build-fec-ctrl --target go2cpp_fec_controller_test -j12
ctest --test-dir .build-fec-ctrl -R go2cpp_fec_controller_test --output-on-failure
```

实测输出（2026-11-08，`100 checks, 0 failures`）：

```
=== 1. 0% 丢包：冗余趋近下限（不浪费）===
  最终冗余=0.0200  变更次数=0  极差=0.0000
  k=8 → parity=0（0.02×8=0.16，四舍五入为 0，不浪费一个校验包）

=== 2. 10% 丢包：冗余升到 15.7%（10%~20% 区间）===
  冗余=0.1571（15.7%）  稳态映射=0.1571  变更=1↑/0↓  分片 m(k=8)=1

=== 3. 50% / 100% 丢包：冗余封顶 50% ===
  50% 丢包 → 冗余=0.5000（上限常量 0.50）
  100% 丢包 → 冗余=0.5000（不无限增）
  配置 max_redundancy=0.90 → 实际 0.50（被夹到硬上限）

=== 4. 1%↔10% 抖动（200 个采样交替）===
  瞬时映射：极差=0.1371 大跳=199 次 最大单步=0.1371 均值=0.0886
  控制器  ：极差=0.0661 大跳=3 次 最大单步=0.0229 均值=0.0846 变更=3
  稳态段（后 100 采样）：控制器极差=0.000000 大跳=0 次 ｜ 瞬时极差=0.1371 大跳=99 次

=== 6. 闭环双向：10% → 0% 恢复后冗余降回下限 ===
  0% 恢复段：冗余=0.0200，在第 8 个采样回到下限（共 100 个），down_changes=2

=== 9. 100 万随机采样（不变量 + 耗时）===
  100 万采样：不变量=OK 最大冗余=0.5000 耗时=8.541 ms（单次 8.5 ns）
```

### 5.2 「固定 20% vs 自适应」的带宽浪费（**核心数据**）

场景：100 万个媒体包、每包 1200 B（媒体 1.2 GB），**0% 丢包**。

| 方案 | 冗余比例 | 多传字节 | 多传包数 | 相对媒体 |
|---|---|---|---|---|
| 固定 20% | 20.0% | **240,000,000 B（228.9 MiB）** | 200,000 | +20.00% |
| 自适应（本模块） | 2.0%（k=8 时 parity=0） | **24,000,000 B（22.9 MiB）** | 0 | +2.00%（按比例口径） |
| **差额** | **18 个百分点** | **216,000,000 B（206.0 MiB）** | **180,000 个校验包** | **+18.00%** |

> 冗余开销相对降低 **90%**（20% → 2%）；若把 `min_redundancy` 设为 0，
> 好网冗余为 0，差额持续为 100% 的冗余带宽。

反面对照（**差网保护不足**）：30% 丢包时

| 方案 | 冗余 | 组内理论可恢复丢包率 `r/(1+r)` | 结论 |
|---|---|---|---|
| 固定 20% | 20% | 16.7% | **补不住 30% 丢包**，其余靠 RTO |
| 自适应 | 50% | 33.3% | 覆盖 30% 丢包并留 3 个点余量 |

### 5.3 6 个网络画像（每个 120 个 1Hz 采样，带噪声）

| 画像 | 滤波丢包 | 末态冗余 | 均值冗余 | 变更次数 |
|---|---|---|---|---|
| 内网 0% | 0.00% | 2.0% | 2.0% | 0 |
| 稳定 3%（±1%） | 3.09% | 4.0% | 4.0% | 1 |
| 稳定 8%（±3%） | 7.60% | 13.4% | 14.0% | 3 |
| 稳定 20%（±5%） | 19.46% | 30.2% | 34.9% | 6 |
| 突发 0%→25% | 25.73% | 43.9% | 21.2% | 7（全升） |
| 抖动 1%↔12% | 7.29% | 10.0% | 9.8% | 3 |

### 5.4 构建与可移植性

```bash
# Linux 全量构建：零 error / 零新增 warning（Bento ON + tests ON）
cmake --build .build-fec-ctrl -j12

# 纯计算模块的跨平台语法检查（不依赖 __linux__）
g++ -std=c++17 -U__linux__ -Iinclude -Wall -Wextra -Wpedantic -Werror \
    -fsyntax-only src/net/fec_controller.cpp
g++ -std=c++17 -U__linux__ -Iinclude -Wall -Wextra -Wpedantic -Werror \
    -fsyntax-only tests/net/test_fec_controller.cpp
```

---

## 6. 不适用范围

| 场景 | 原因 | 应该用什么 |
|---|---|---|
| **突发型丢包**（一次丢 10 个连续包） | Cauchy RS 只能恢复组内 m 个**任意**丢失；m=2 时连丢 3 个整组失效。而本模块只按**平均**丢包率算冗余，不知道突发长度 | 提高 k/m 并做**交织**（interleaving），或改用 NACK 重传 |
| **丢包率 > 50%** | 50% 冗余的 MDS 上限是恢复 33.3% 丢包；再加冗余会挤占全部有效带宽 | 降码率 / 换路径 / 重传；FEC 只能兜一部分 |
| **低 RTT + 有可靠重传**（如内网 1ms） | 重传成本 = 1 个 RTT 的延迟；FEC 成本 = **永久**多传字节。低 RTT 下重传更便宜 | 关闭 FEC 或 `min_redundancy = 0`，用 NACK/SACK |
| **带宽极紧**（2G / 拥塞卫星） | 50% 冗余会把有效载荷砍半，可能压垮链路 | 先降码率；FEC 只在小冗余（≤10%）下用 |
| **统计窗口太小**（每周期 <20 包） | 20 包时最小可分辨丢包率 5%，噪声直接变成冗余抖动 | 拉长统计窗口（≥1s 且 ≥20 包），或对丢包率做二次平滑 |
| **多流共享一条物理链路** | 每流一个实例会各自加冗余，叠加后可能远超 50% | 按**链路**聚合统计，一个控制器统管所有流 |
| **无反馈的单向广播** | 没有丢包率输入，闭环无输入 | 静态配置 `FecConfig`（本模块不参与） |
| **需要按帧类型分级保护**（关键帧多保护） | 本模块只有单一冗余输出 | 上层按优先级/帧类型给不同实例或额外叠加 |

---

## 7. 构建开关与文件清单

| 文件 | 作用 |
|---|---|
| `include/go2cpp/net/fec_controller.hpp` | 配置结构 + `FecController` 类 |
| `src/net/fec_controller.cpp` | EWMA + 分段线性映射 + 三道闸门 |
| `tests/net/test_fec_controller.cpp` | 10 组测试：3 个丢包档位 / 抖动 / 闸门 / 闭环 / 边界 / 画像 / 100 万压力 / 带宽对照 |
| `cmake/Go2CppFecController.cmake` | `go2cpp_fec_controller` 静态库 + `go2cpp_fec_controller_test` |
| `cmake/Go2CppTests.cmake`（末尾一行 include） | 注册 |

```bash
-DGO2CPP_WITH_FEC_CONTROLLER=OFF     # 完全移出构建；不影响 runtime/bento/tunnel
```
