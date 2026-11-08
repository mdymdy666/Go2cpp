# 乱序距离直方图（reorder_histogram）

> 模块路径：`include/go2cpp/net/reorder_histogram.hpp`、`src/net/reorder_histogram.cpp`、
> `tests/net/test_reorder_histogram.cpp`
> 构建接入：`cmake/Go2CppReorderHistogram.cmake`（`GO2CPP_WITH_REORDER_HISTOGRAM`，默认 ON）
> 定位：**P0-2 / G2**——用 p 分位把「公网乱序」与「真丢包」分开，**抑制误重传**。
> 纯计算：只依赖 C++17 标准库，**不依赖 fiber / scheduler / IO**，可裁剪。

---

## 1. 它解决什么问题

可靠传输层常见的丢包判据是「收到的序号跳变就认为中间丢了」：

```
收到 0, 1, 3 → 判定 2 丢失 → 立即重传 2
```

内网（丢包≈0、乱序≈0）几乎不出错；但公网乱序很常见（多路径 / 无线 /
QUIC 共存），迟到的包会被当成丢包，触发**重传风暴**：

```
真丢包 → 重传 → 重传包占满带宽 → 后续包更晚到 → 又被判丢 → 更多重传
```

`reorder_histogram` 维护「乱序距离 = 已见最大序号 − 到达序号」的滑动直方图，
用 p 分位回答一个问题：**「这个缺口，历史上正常的乱序要多久才到？」**
只有超过分位容忍度的缺口才判丢。

```
到达: 0 2 1 3 5 4 ...
乱序:    1   1     1        // 直方图样本 1,1,1
p50  → 1（0-based 距离）
tolerance = clamp(1 + 1, 1, 10) = 2   // 等 2 个序号仍没到才算丢
```

## 2. 契约

### 2.1 接口

```cpp
namespace go2cpp::net {

struct ReorderHistogramConfig;          // 见第 3 节
enum class PacketResult { kInOrder, kOutOfOrder, kDuplicate, kTooOld };
struct ReorderStats;

class ReorderHistogram {
 public:
  explicit ReorderHistogram(ReorderHistogramConfig config = {});

  PacketResult OnPacket(std::uint64_t seq, std::uint64_t arrival_time_ms,
                        bool is_retransmitted = false);

  bool IsLikelyLost(std::uint64_t seq) const;
  bool IsLikelyLost(std::uint64_t seq, std::uint64_t now_ms) const;

  std::uint64_t ReorderPercentile(double p) const;  // 0-based 乱序距离
  std::uint64_t ReorderPercentile() const;
  std::uint64_t tolerance() const;                  // 「再等几个序号」

  bool has_packets() const noexcept;
  bool has_seq(std::uint64_t seq) const noexcept;
  std::uint64_t max_seq_seen() const noexcept;
  std::uint64_t max_seq_seen_time_ms() const noexcept;
  std::size_t sample_count() const noexcept;
  std::size_t bucket_count(std::size_t index) const noexcept;
  const ReorderHistogramConfig& Config() const noexcept;
  const ReorderStats& Stats() const noexcept;
  void Reset();
};

}
```

### 2.2 语义

| 接口 | 契约 |
|---|---|
| `OnPacket(seq, t)` | 记录「该包已到达」。`kInOrder` 表示推进了 `max_seq_seen`；`kOutOfOrder` 表示迟到；`kDuplicate` 表示已收过；`kTooOld` 表示早于位图窗口、无法判定。**不分配、不阻塞、不调时钟**。 |
| `IsLikelyLost(seq)` | 纯 gap 判据：`gap = max_seq_seen − seq > tolerance()` 时为 true。已收到 / `seq > max_seq_seen` / 超出位图窗口 → 一律 false（保守）。 |
| `IsLikelyLost(seq, now_ms)` | 在纯 gap 判据之上追加停流兜底：流静止 `stale_timeout_ms` 后，容忍度内的缺口也判丢。`stale_timeout_ms=0`（默认）时等价于纯 gap 判据。 |
| `ReorderPercentile(p)` | 乱序距离的 p 分位（**0-based**，无样本返回 0）。采用最近秩法：`threshold = ceil(p × n)`，累计计数首次达到 threshold 的桶号。 |
| `tolerance()` | `clamp(ReorderPercentile(percentile) + 1, min_tolerance, max_tolerance)`；样本数 `< min_samples` 时返回 `min_tolerance`。`+1` 把 0-based 距离换成「再等几个序号」，对齐 WebRTC 的 1-based 返回值。 |

### 2.3 不变量

- `max_seq_seen` 单调不减，且 `has_seq(max_seq_seen)` 恒为 true；
- `sum(bucket_count(i)) == sample_count() <= config.window`；
- 所有查询**无副作用**、不分配；重复调用结果相同；
- 非线程安全：必须由同一执行流（通常是接收循环）调用。

## 3. 参数

| 参数 | 默认 | 含义 | 调参建议 |
|---|---|---|---|
| `num_buckets` | `10` | 桶数；乱序距离 ≥ 该值归入末桶（对齐 WebRTC `kNumReorderingBuckets`）。 | 一般不改。乱序距离可到几十时提到 16/32（容忍度上限随之提高）。 |
| `window` | `128` | 滑动窗口样本数（对齐 WebRTC `kMaxReorderedPackets` 用作 `Histogram` 的样本容量）。 | 链路画像变化快（地铁/WiFi 切换）→ 64；稳定长周期 → 256。 |
| `percentile` | `0.5` | 分位（对齐 WebRTC `WaitNumberOfPackets(0.5)`）。 | 0.5 是「一半乱序包会被重传」的取舍；要更保守（少重传）→ 0.3/0.2，代价是丢包恢复更慢。 |
| `min_tolerance` | `1` | 冷启动/样本不足时的容忍度。 | 高乱序多路径链路建议 2（代价：真丢包晚 1 个序号判定）。 |
| `max_tolerance` | `0`（用 `num_buckets`） | 容忍度上限。 | 与 `num_buckets` 解耦后可单独限深，例如 `max_tolerance=4`。 |
| `min_samples` | `1` | 样本少于此值不用分位。 | 噪音大的链路用 8~16，避免第一个样本把容忍度带偏。 |
| `stale_timeout_ms` | `0`（关闭） | 停流兜底；设为 RTT 量级（如 100~300ms）可覆盖「丢的是最后一个缺口」。 | 移动网络建议 = PTO/RTO 下限的一半。 |
| `received_window` | `1024` | 「已收」位图窗口（序号个数）。 | 内存敏感（每连接 1 KiB）→ 256；超长乱序 → 4096。 |
| `count_retransmitted` | `false` | 重传包是否计入乱序样本。 | 保持 false：把重传当乱序样本会让分位自我强化。 |

## 4. 判据与实测

### 4.1 三类场景（`tests/net/test_reorder_histogram.cpp` 场景 1–3）

| 场景 | 朴素 `seq > expected` | 直方图 p50 |
|---|---|---|
| 纯乱序（0,2,1,3,5,4…，7 次相邻交换） | 重传 **7** 次，全部误重传 | 重传 **0** 次 |
| 真丢包（缺 2、6） | 重传 2 次（2、6），漏判 0 | 重传 2 次（2、6），漏判 0 |
| 混合（真丢 2/6 + 乱序 4/9/12/15/16） | 重传 7 次，**误重传 5** | 重传 2 次，**误重传 0** |

### 4.2 学习收敛（场景 4）

首次遇到 `gap=2` 的乱序时直方图还没有样本，容忍度=1 → **误判 1 次**；
之后同距离（≤2）不再误判。要消掉这 1 次，把 `min_tolerance` 设为 2
（测试中已对照：`min_tolerance=2` 时误判为 0）。

### 4.3 真实网络画像（每画像 20 万包，长尾乱序）

| 画像 | 朴素误重传 | 直方图误重传 | 直方图误重率 | 直方图漏判 |
|---|---|---|---|---|
| 内网（无丢包/无乱序） | 0 | 0 | 0.00% | 0 |
| 有线公网（1% 丢 / 1% 乱序） | 593 | 0 | 0.00% | 0 |
| 4G 移动（5% 丢 / 10% 乱序） | 5133 | 563 | 0.28% | 1 |
| WiFi 切换（10% 丢 / 20% 乱序） | 8714 | 1051 | 0.53% | 0 |
| 多路径（0% 丢 / 40% 乱序） | 17427 | 1422 | 0.71% | 0 |
| 恶劣链路（20% 丢 / 20% 乱序） | 7171 | 886 | 0.44% | 1 |

100 万包随机压力（3% 丢包 / 8% 乱序 / 长尾 1–12）：

```
朴素：  重传决策 51809，误重传 21792（占总包 2.18%）
直方图：重传决策 32301，误重传  2284（占总包 0.23%），漏判 0
误重传率：2.18% → 0.23%（降 89.5%）
```

P0-2 的验收口径是「**误重传包数 / 总包数 < 5%**」：上述画像全部满足
（最差 0.71%）。

### 4.4 最坏情况（如实报告）

把乱序距离改成**均匀 1–16**（不代表真实网络，是压力上界）时，
WiFi 画像（10% 丢 / 20% 乱序）的误重传率是 **6.66%**（32894 → 13311），
**超过 5% 目标**。原因是 50% 分位天然让一半的乱序距离落在容忍度之外；
均匀分布比真实长尾分布有更多「大距离乱序」。此时需要把 `percentile`
降到 0.3 或把 `max_tolerance` 提到 16 以上（代价是漏判增加）。

## 5. 与 WebRTC 的差异（源码级）

对照对象：`webrtc-mirror/webrtc@main`（2026-11-08 抓取，未固定 SHA）。

| # | WebRTC | 本模块 | 说明 |
|---|---|---|---|
| 1 | `nack_requester.cc:NackRequester::NackRequester` 构造 `reordering_histogram_(kNumReorderingBuckets, kMaxReorderedPackets)`，即 **10 桶 / 128 样本** | `num_buckets=10`、`window=128` | 默认值对齐。 |
| 2 | `histogram.cc:Histogram::Add` 把值夹到 `buckets_.size()-1`；`InverseCdf(p)` 累积概率后返回**1-based 桶号**（`++bucket` 在 `return` 之前） | 同样夹到末桶；`ReorderPercentile` 返回 **0-based 距离**，`tolerance()` 再做 `+1` 对齐 1-based 语义 | 语义等价，命名更直白；`tolerance()` 才是「等几个序号」。 |
| 3 | `nack_requester.cc:NackRequester::UpdateReorderingStatistics` 只在「收到迟到包」时 `Add(ReverseDiff(newest, seq))` | 同：只在 `seq < max_seq_seen` 且未收过时采样 | 对齐。 |
| 4 | **`OnReceivedPacket` 里 `bool is_retransmitted = true;` 硬编码**，导致 `if (!is_retransmitted) UpdateReorderingStatistics(...)` **永不执行**——主线当前的重排统计实际是死代码（源码里留有 TODO） | `OnPacket(..., is_retransmitted)` 默认 `false`（统计会更新），`count_retransmitted=false` 时**重传包**才被排除 | 修掉 WebRTC 的死代码缺陷；重传包仍登记「已收」，避免重复触发。 |
| 5 | 空直方图时 `WaitNumberOfPackets` 返回 **0** → `send_at = seq` → 缺口可立刻 NACK | 样本 `< min_samples` 时返回 `min_tolerance`（默认 **1**）→ 至少等 1 个序号 | 消除冷启动误重传；这是「纯乱序场景 0 次误重传」的关键。 |
| 6 | 两层触发：`GetNackBatch(kSeqNumOnly)` 的 `newest >= send_at_seq_num` **加** `kTimeOnly` 的 `now - sent_at >= rtt`（`kDefaultRtt=100ms`） | 单层 gap 判据 + 可选 `stale_timeout_ms` 时间兜底 | 本模块不持有 RTT（纯计算），时间由调用方传入；RTT 层仍由可靠层负责。 |
| 7 | `AddPacketsToNack` 在 `newest_seq_num_ = seq` **之前**调用，`send_at = seq + wait` | 调用方在 `OnPacket` **之后**调 `IsLikelyLost`，gap 用新 max 计算 | 两者都等价于「gap > wait 才 NACK」；本模块判据比 WebRTC 严格 1 个序号（`gap > tol` vs `gap >= tol`），更保守。 |
| 8 | 有 `nack_list`（重传次数上限 100、表上限 1000、序号存活 10000） | 无重传队列，只做「该不该重传」的判据 | 关注点分离：队列/限速/关键帧请求留给可靠层。 |
| 9 | 序号是 `uint16_t`，用 `AheadOf/ReverseDiff` 处理回绕 | 要求调用方传入**单调递增** `uint64_t` | 见第 7 节「不适用范围」；回绕归一化留在上层。 |

## 6. 接线方式（本任务未接线，按铁律留待后续）

现有可靠层在 `src/bento/net/channel.cpp:786` 的 `ReliableChannel::TryFastRetransmit`：
判据是「SACK 空洞 + 重复 ACK 达阈值（默认 3）」。最小接入方式：

1. 在 `ReliableChannel` 增加成员 `go2cpp::net::ReorderHistogram m_reorder;`
2. 收到数据包时 `m_reorder.OnPacket(seq, now_ms)`；
3. 把 `sack_gap` 的判定改成「**且** `m_reorder.IsLikelyLost(first_unacked)`」，
   或对每个 `<= m_max_sacked` 的未确认项逐条调用 `IsLikelyLost`；
4. 快速恢复期间的重发也先过一层 `IsLikelyLost`，避免把「还在容忍度内」的缺口反复重发。

本任务**未改** `src/bento/` / `src/tunnel/`（铁律：需先上报），因此模块目前
无生产消费者；接线是下一步的独立改动。

## 7. 不适用范围

1. **序号回绕**：本模块假设 `seq` 单调递增的 `uint64_t`。16 位/32 位回绕必须
   由调用方先归一化为单调序号，否则 `gap = max − seq` 会瞬间变成巨大值。
2. **乱序距离 ≥ `received_window`（默认 1024）**：判据返回 false（信息不足），
   **不会**重传。极端长乱序（>1024 个包）需要调大窗口，否则只能等更高层超时。
3. **多线程**：非线程安全；不要多个接收线程共享一个实例。
4. **非单调时钟**：`IsLikelyLost(seq, now_ms)` 检测到 `now < max_seq_seen_time_ms`
   时跳过停流兜底（不猜）；调用方仍应传单调时钟（如 `steady_clock`）。
5. **丢包率/乱序分布突变**：直方图是滑动窗口，切换网络（WiFi→4G）后要
   `window` 个样本才收敛；期间可能多判或漏判几个包。切换后可 `Reset()`。
6. **按字节序号**：gap 的语义是「包数」，不是字节数。按字节窗口的协议
   （如 TCP `snd_una`）需要先换算成包序号，否则分位没有物理意义。
7. **不能替代 RTT/PTO**：它只抑制「乱序误判」，真丢包的最终兜底仍需
   RTO/PTO（本模块的 `stale_timeout_ms` 只是给「最后一个缺口」补一个时间维度）。
8. **不限制重传速率**：它减少「不该重传的重传」，但如果上层对判丢无条件
   重传，拥塞时的重传风暴仍需重传限速/拥塞控制兜底。

## 8. 可裁剪性与依赖

- 目标：`go2cpp_reorder_histogram`（STATIC，`POSITION_INDEPENDENT_CODE ON`）；
- 依赖：**只有** C++17 标准库（`<cstdint>`、`<cstddef>`、`<vector>`、`<algorithm>`、`<cmath>`）；
- 关闭：`-DGO2CPP_WITH_REORDER_HISTOGRAM=OFF` 即完全移出构建，不影响
  `go2cpp_runtime` / `go2cpp_bento` / `go2cpp_tunnel`；
- 内存：每实例约 `num_buckets×4 + window×4 + received_window×1` 字节（默认约 1.6 KiB）。

## 9. 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp

# 全量构建（与 CI quick 同选项）
cmake -S . -B .build-reorder -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON \
      -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MCP=ON -DMCP_BUILD_TESTS=ON \
      -DGO2CPP_WITH_APP=ON -DGO2CPP_WITH_P2P=ON -DGO2CPP_WITH_SQLITE=ON
cmake --build .build-reorder --parallel "$(nproc)"

# 本模块单测（117 checks，约 0.7s）
ctest --test-dir .build-reorder -R reorder_histogram --output-on-failure
# 或直接跑：.build-reorder/go2cpp_reorder_histogram_test
```

测试内容：场景 1–4（乱序/丢包/混合/收敛）、场景 5（停流兜底）、场景 6
（配置与边界，含位图滚动/大跳跃/重复包/重传包/分位语义）、场景 7（6 个真实
画像 + 最坏均匀上界）、场景 8（100 万包随机对照）。

## 10. 参考

- `docs/research/webrtc-public-network.md`（§3 抗丢包、§7 差距表 G2）
- `docs/plans/public-network-p0.md`（G2 / P0-2）
- WebRTC 源码（2026-11-08，`webrtc-mirror` main）：
  - `modules/video_coding/nack_requester.cc`：`NackRequester::UpdateReorderingStatistics`、
    `WaitNumberOfPackets`、`AddPacketsToNack`、`GetNackBatch`；
  - `modules/video_coding/histogram.cc`：`Histogram::Add` / `InverseCdf`。

## 11. 验证记录（2026-11-08）

在 **干净 worktree `332e4aa` + 本任务文件**（提交 `7bc4208` 的内容）上验证，
以隔离并行任务的未提交改动：

| 项 | 命令 / 条件 | 结果 |
|---|---|---|
| 全量构建 | CI quick 同选项（Release、Bento/MCP/app/p2p/sqlite 全开，12 并发） | **0 error**；唯一 warning 是既有 `src/tunnel/net.cpp:529:9 unused variable`（本任务未改该文件） |
| 模块单测 | `ctest -R reorder_histogram` | **Passed**，117 checks / 0.94s |
| 跨平台语法 | `g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Iinclude -U__linux__ -fsyntax-only`（实现 + 测试） | **通过**；三个文件零 `__linux__` / `_WIN32` / `__APPLE__` 分支 |
| 既有测试 | `ctest -R bento`（63 项，低负载 load≈6） | **63/63 通过**，`bento_netem` 环境性 skip |
| 竞争条件说明 | 验证期间并行任务构建把 load 推到 8–12 时，曾出现 `bento_omni_server_m1` / `bento_http_server` 超时与 `bento_session_table` 并发计数偶发 | 同一构建低负载重跑全过；**基线构建（无本任务改动）重复 3 次也全过**；本任务对 `src/bento/`、`tests/bento/`、`cmake/Go2CppBento.cmake` 的 diff 为空 |

已知边界：均匀 1–16 乱序模型下 WiFi 画像误重传率 6.66%（见 §4.4），
真实长尾模型下 6 个画像均 ≤ 0.71%。
