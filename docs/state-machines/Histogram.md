# Histogram 状态机（指标族 / 序列生命周期与 observe 路径）

- 源文件：`src/metrics/metrics.cpp`（类定义在 `include/go2cpp/metrics/metrics.hpp`）
- 关键函数：`Histogram::InitBuckets()`、`Histogram::With()`、`Histogram::Observe()`、
  `Histogram::CumulativeBucketCounts()`、`Histogram::Quantile()`、`Registry::Render()`

## 状态

| 状态 | 含义 |
|---|---|
| Uninitialized | 构造中，桶计数数组尚未分配 |
| Ready | `m_bucket_count == boundaries.size() + 1`，Count/Sum/桶计数全为 0 |
| SeriesReady | 带标签根对象已通过 `With()` 创建/取得一个子序列（仍为 0 样本） |
| Accumulating | 至少一个有限样本已计入，Count > 0 且 Sum 有限 |
| NonFiniteSum | 观察过 `+Inf`/`-Inf`，Count 增长但 Sum 变为非有限值 |
| Exported | `AppendPrometheus()`/`Registry::Render()` 正在/已经输出快照（只读） |

## 转化表

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Uninitialized | `Histogram()` / `InitBuckets()` | Ready | 分配 `bucket_count` 个原子计数并清零 |
| Ready | `With(labels)` | SeriesReady | 根族创建子序列；同标签组合复用已有对象 |
| Ready | `With({})`（无标签） | Ready | 直接返回根对象，不创建子序列 |
| SeriesReady | `Observe(finite)` | Accumulating | 二分落桶 + 桶计数、Sum、Count 各 +1 |
| Accumulating | `Observe(finite)` | Accumulating | 只追加；桶计数/Count 单调不减 |
| Accumulating | `Observe(NaN)` | Accumulating | 直接返回，Count/Sum/桶计数都不变 |
| Accumulating | `Observe(+Inf/-Inf)` | NonFiniteSum | 落到 `+Inf` 桶，Sum 变为 Inf/NaN |
| NonFiniteSum | `Observe(finite/Inf)` | NonFiniteSum | Sum 不可能恢复为有限值 |
| Accumulating / NonFiniteSum | `CumulativeBucketCounts()` | 不变 | 逐桶读并累加，返回近似快照 |
| Accumulating / NonFiniteSum | `Quantile(q)` | 不变 | 只读估算；空直方图返回 NaN |
| Ready / SeriesReady / Accumulating | `AppendPrometheus()` / `Registry::Render()` | Exported | 只读导出，不改变计数 |
| Exported | `Observe(finite)` | Accumulating | 导出后仍可继续观察（监控是持续过程） |

## 不变量

- `m_bucket_count == Buckets().size() + 1`，末尾是隐式 `+Inf` 桶。
- `Count() == 所有桶计数之和`（NaN 不计入）；桶计数与 Count 单调不减。
- `CumulativeBucketCounts()` 返回的数组单调不减，末项等于本次读到的 Count 附近值。
- `Sum()` 一旦因 `±Inf` 变为非有限值，不会再变回有限值。
- 桶边界构造后不可变，所有子序列共享同一组 `le` 上界。

## 非法转化

- Ready -> Accumulating（通过 `Observe(NaN)`）：NaN 被忽略，状态与计数都不变。
- 带标签根对象直接 `Observe()`：抛 `std::logic_error`，状态不变；必须先 `With({...})`。
- Accumulating -> Ready：没有递减/重置接口；计数只增不减。
- 空直方图 `Quantile()`：返回 NaN，而不是把状态改为 Accumulating。
