# 模块文档：指标（metrics）

> 适用基线：`HEAD 27e892b`（2026-10-26），WSL 工作树 `/UserData/CodexWorkSpace/Go2cpp`。
> 源码：`include/go2cpp/metrics/metrics.hpp`（526 行）、`src/metrics/metrics.cpp`（1,201 行）；
> CMake 目标 `go2cpp_metrics`。
> 引用约定：`metrics.hpp` 指 `include/go2cpp/metrics/metrics.hpp`，`metrics.cpp` 指
> `src/metrics/metrics.cpp`。
> 状态：**现行**。本模块此前没有独立文档，本文为首版；`docs/metrics*.md` 不存在，
> 无旧文档需要合并。

## 1. 概述

`go2cpp::metrics` 是只依赖 C++17 标准库的轻量指标层，**不引入 Prometheus 客户端
依赖**。它提供三种指标（Counter / Gauge / Histogram）、标签族缓存、进程级注册表和
Prometheus 文本导出（`metrics.hpp:3-14`、`44-63`）。

```
业务代码                          Registry（Default()/独立实例）
  Inc()/Add()/Set()/Observe()  ──►  name → unique_ptr<Metric>
  With({labels}) → 子序列            │
                                     └─ Render() ──► Prometheus 文本
```

四条设计目标（`metrics.hpp:5-14`）：

1. **计数绝对不丢**：`Counter` 用 `std::atomic<uint64_t>` 的 `fetch_add`；
2. **标签缓存**：同一组标签值只创建一个子指标，`With()` 返回稳定引用；
3. **固定桶直方图**：桶边界创建后不可变，桶计数与总和均为原子读写；
4. **标准文本格式**：`# HELP` / `# TYPE` / `name{label="value"} value`。

### 1.1 最小示例

```cpp
#include "go2cpp/metrics.hpp"

auto& registry = go2cpp::metrics::Registry::Default();

auto& requests = registry.GetCounter(
    "http_requests_total", "HTTP requests", {"method", "path"});
requests.With({"GET", "/api"}).Inc();

auto& latency = registry.GetHistogram(
    "http_request_duration_seconds", "Request latency",
    {"method"}, {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5});
latency.With({"GET"}).Observe(0.023);

std::string text = registry.Render();   // 直接喂给 /metrics 端点
```

## 2. 数据模型

### 2.1 类型

`MetricType` 三个值（`metrics.hpp:82-99`）：`kCounter` / `kGauge` / `kHistogram`；
`MetricTypeName` 返回 `counter`/`gauge`/`histogram`/`untyped`，直接用于 `# TYPE` 行。

### 2.2 指标族与子序列

`detail::MetricInfo` 是族共享元数据：`name`、`help`、`type`、`label_names`
（`metrics.hpp:120-125`）。根指标持有 Family，所有子指标通过裸指针共享它；子指标
生命周期严格短于根指标（由 Family 容器拥有），因此不需要 `shared_ptr` 引用计数，
也不会成环（`metrics.hpp:103-119`）。

`Metric` 是公共只读基类，只暴露 `Name/Help/Type/LabelNames/SeriesCount`；**不提供
`With()`**，因为三种子指标类型不同，C++ 无法用统一返回类型表达，必须走
`GetCounter`/`GetGauge`/`GetHistogram`（`metrics.hpp:132-146`）。

### 2.3 标签族语义（最重要的一条）

带标签的名字在注册表里代表一个**指标族**，根对象只用于 `With()` 取子序列：

- 对带标签的根对象直接 `Inc()` / `Set()` / `Observe()` 会抛 `std::logic_error`，
  因为缺少标签值的样本无法按 Prometheus 文本格式导出（`metrics.hpp:31-34`、
  `metrics.cpp:441-447`、`801-807`）；
- `With({...})` 的标签数量/顺序必须与注册时一致，否则抛 `std::invalid_argument`
  （`metrics.cpp:495-501`）；
- 子对象再次 `With()` 只接受**自己的**标签值，传别的值直接拒绝
  （`metrics.cpp:486-493`）；
- 无标签指标直接操作根对象即可，`With()` 返回根自身（`metrics.cpp:502-504`）。

标签值用**长度前缀**编码成缓存键（如 `"3:GET2:/x"`），因此 `{"a,b"}` 与
`{"a","b"}` 不会串键（`metrics.cpp:179-190`、`506-516`）。

## 3. 三种指标

### 3.1 Counter

`Inc()` / `Add(n)` 走 `fetch_add(relaxed)`；`Value()` 走 relaxed load
（`metrics.hpp:193-261`、`metrics.cpp:449-462`）。计数场景不需要与其它内存操作建立
happens-before，relaxed 已足以保证「不丢更新」。

### 3.2 Gauge

浮点值以 **IEEE-754 位模式**存进 `std::atomic<uint64_t>`，`Set` 用 store、`Add` 用
CAS 循环（`AddDouble`）、`Inc`/`Dec` 是 `Add(±1)`（`metrics.hpp:263-327`、
`metrics.cpp:350-380`、`613-634`）。这样在 C++17 下也能无锁更新，且 `Set(NaN)` 之后
的加法不会因为 `NaN != NaN` 而卡死 CAS 循环（`metrics.hpp:266-268`、`273-275`）。
`Set` 允许 `±Inf` 与 NaN（`metrics.hpp:283-284`）。

### 3.3 Histogram

桶是**升序有限上界**，语义为 `le`；所有序列隐含一个 `+Inf` 桶，因此
`m_bucket_count == Buckets().size() + 1`（`metrics.hpp:329-351`、`metrics.cpp:792-799`）。

`Observe(value)` 的四步（`metrics.cpp:809-830`）：校验不是带标签根对象 → NaN 直接
返回 → 二分找桶（第一个 `>= value` 的边界）→ 桶计数 `fetch_add`、`AddDouble` 累加
Sum、Count `fetch_add`。负值、`+Inf`、`-Inf` 按边界落桶，Sum 如实累计
（`metrics.hpp:332-334`）。

默认桶（`metrics.cpp:380-392`，与 Prometheus 客户端惯例一致并向上扩展）：

```text
0.001 0.0025 0.005 0.01 0.025 0.05 0.1 0.25 0.5 1 2.5 5 10 25 50
100 250 500 1000 2500 5000 10000
```

`Quantile(q)` 的语义（`metrics.cpp:864-926`）：NaN 入参→NaN；`q<0`→`-Inf`；
`q>1`→`+Inf`；空直方图→NaN；否则按累计桶计数算 `rank = q * total`，找第一个
`cumulative >= rank` 的桶，在桶内做线性插值
`lower + (rank - prior) / bucket_count * (upper - lower)`；目标落在 `+Inf` 桶时返回
最后一个有限上界（Prometheus 经典语义）。第一个桶的下界按 `min(0, upper)` 处理，
空桶返回下界避免除零（`metrics.cpp:873-874`、`895-925`）。

`CumulativeBucketCounts()` 逐桶读取并累加，长度 `Buckets().size()+1`，末项是
`+Inf` 桶；并发 `Observe` 时各桶来自不同瞬间，是**近似快照**，但数组仍单调不减
（`metrics.hpp:405-411`、`metrics.cpp:846-862`）。

## 4. 注册表

`Registry` 是进程级指标注册表，`Default()` 是函数内静态单例；测试可构造独立实例
做隔离（`metrics.hpp:452-480`、`metrics.cpp:1051-1057`）。

| API | 行为 | 引用 |
|---|---|---|
| `GetCounter(name, help, label_names)` | 注册或取得 Counter | `metrics.cpp:1145-1150` |
| `GetGauge(...)` | 注册或取得 Gauge | `metrics.cpp:1152-1156` |
| `GetHistogram(..., buckets)` | 空 buckets 用默认桶 | `metrics.cpp:1158-1165` |
| `Register(name, help, type, labels)` | 按类型注册，返回基类引用；Histogram 用默认桶 | `metrics.cpp:1167-1171` |
| `Render()` | 输出全部指标的 Prometheus 文本 | `metrics.cpp:1184-1193` |
| `Size()` | 已注册指标族数量 | `metrics.cpp:1195-1198` |

注册按名字唯一，且**同名必须完全同构**：类型、help、标签名、直方图桶任一不同都抛
`std::invalid_argument`（`metrics.cpp:1088-1118`）。设计理由：静默复用会让两个调用方
写进同一条序列，指标含义被污染且难以发现；启动期抛异常比运行期给出错误数字更安全
（`metrics.cpp:1069-1071`、`metrics.hpp:455-457`）。

注册表本身线程安全：注册/查重/插入都在 `m_mutex` 内，返回的引用在注册表生命周期内
稳定（`metrics.hpp:457-469`、`metrics.cpp:1088-1142`）。

## 5. 导出格式

`Render()` 持注册表锁、按 `std::map` 名字序遍历，因此输出顺序稳定、可直接 diff
（`metrics.cpp:1184-1193`）。每个族输出：

```text
# HELP http_requests_total HTTP requests
# TYPE http_requests_total counter
http_requests_total{method="GET",path="/api"} 1
```

Histogram 展开成三组序列（`metrics.cpp:1007-1049`）：

```text
http_request_duration_seconds_bucket{method="GET",le="0.005"} 12
...
http_request_duration_seconds_bucket{method="GET",le="+Inf"} 100
http_request_duration_seconds_sum{method="GET"} 2.34
http_request_duration_seconds_count{method="GET"} 100
```

文本转义只做最小集（`\`、`"`、`\n`）；标签值里的其它控制字符在**注册/取值时**就被
拒绝（`metrics.hpp:62`、`metrics.cpp:105-116`）。`FormatDouble` 对 NaN/Inf 输出
`NaN`/`+Inf`/`-Inf`，符合 Prometheus 文本格式（`metrics.cpp:28`、`199-230`）。

### 5.1 名字与标签校验

| 规则 | 违反时 | 引用 |
|---|---|---|
| 指标名：首字符 `[A-Za-z_:]`，其余 `[A-Za-z0-9_:]` | `invalid_argument` | `metrics.cpp:55-70`、`89-94` |
| help 不含 NUL/CR | `invalid_argument` | `metrics.cpp:96-103` |
| 标签名：首字符 `[A-Za-z_]`，其余 `[A-Za-z0-9_]` | `invalid_argument` | `metrics.cpp:72-87` |
| 标签名不得以 `__` 开头（保留） | `invalid_argument` | `metrics.cpp:126-130` |
| Histogram 标签名不得为 `le`（保留） | `invalid_argument` | `metrics.cpp:131-134` |
| 标签名不得重复 | `invalid_argument` | `metrics.cpp:135-138` |
| 标签值不含 NUL/CR 及除 `\n` 外的控制字符 | `invalid_argument` | `metrics.cpp:105-116` |
| 桶边界非空、有限、严格递增 | `invalid_argument` | `metrics.cpp:142-158` |

## 6. API 一览

| API | 位置 | 说明 |
|---|---|---|
| `MetricType` / `MetricTypeName` | `metrics.hpp:82-99` | counter/gauge/histogram |
| `MetricInfo`（detail） | `metrics.hpp:120-125` | 族共享元数据，注册后不可变 |
| `Metric::Name/Help/Type/LabelNames/SeriesCount` | `metrics.hpp:154-174` | 只读公共接口 |
| `Counter()` / `Counter(labels)` / `Inc` / `Add` / `Value` / `With` / `SeriesCount` | `metrics.hpp:207-261`；`metrics.cpp:419-528` | 单调计数 |
| `Gauge()` / `Gauge(labels)` / `Set` / `Inc` / `Dec` / `Add` / `Value` / `With` | `metrics.hpp:277-327`；`metrics.cpp:584-708` | 瞬时值；允许 NaN/Inf |
| `Histogram()` / `Histogram(buckets)` / `Histogram(buckets, labels)` | `metrics.hpp:358-377`；`metrics.cpp:759-789` | 固定桶 |
| `Observe` / `Count` / `Sum` / `Quantile` / `Buckets` / `CumulativeBucketCounts` / `With` | `metrics.hpp:379-416`；`metrics.cpp:818-1005` | 分布 |
| `Registry::Default/GetCounter/GetGauge/GetHistogram/Register/Render/Size` | `metrics.hpp:471-523`；`metrics.cpp:1145-1198` | 注册与导出 |

## 7. 设计说明

1. **为什么计数用 relaxed 原子**：`fetch_add` 的原子性已经保证不丢更新；监控数字不
   需要与业务内存建立 happens-before，用 `seq_cst` 只会增加多核争用成本
   （`metrics.hpp:196-198`）。
2. **为什么 Gauge 存位模式**：C++17 没有 `atomic<double>`；用 `atomic<uint64_t>` 存
   IEEE-754 位模式 + CAS 循环，既无锁又能正确处理 NaN（`metrics.hpp:263-268`）。
3. **为什么直方图不存原始样本**（`metrics.hpp:340-345`）：
   - 并发成本恒定：一次 `Observe` 只做「二分 + 桶 fetch_add + Sum CAS + Count fetch_add」；
   - 内存有界：占用 O(buckets)，与请求量无关；
   - 与文本格式同构：`le` 桶就是导出格式本身，`Render()` 不需要再聚合；
   - 代价是分位数只是估算，误差上界约一个桶宽。
4. **为什么标签子指标只增不删**：`With()` 必须返回稳定引用，日志/监控热路径不能因
   插入而失效；代价是标签基数无上限（见 §8）。
5. **为什么同名冲突抛异常**：见 §4 的「静默复用污染序列」理由；这是启动期失败，
   不是运行期容错。

## 8. 边界与陷阱

- **带标签根对象不能直接写**：`Inc`/`Set`/`Observe`/`Value` 都会先
  `EnsureSeries()`，根对象且带标签时抛 `logic_error`（`metrics.cpp:441-447`、
  `605-611`、`801-807`）；必须先 `With({...})`。
- **`Observe(NaN)` 被忽略**：不改变 Count/Sum/桶计数，避免一个坏样本污染整条分布
  （`metrics.hpp:36-38`、`metrics.cpp:818-823`）；`Gauge` 相反，允许保存 NaN。
- **分位数是估算**：误差上界约一个桶宽；目标落在 `+Inf` 桶时返回最后一个有限上界，
  不是真实尾部值（`metrics.hpp:40-42`、`metrics.cpp:905-910`）。
- **`CumulativeBucketCounts()` 是近似快照**：并发 `Observe` 时各桶取自不同瞬间，
  但数组单调不减、末项接近当前 Count（`metrics.hpp:408-409`、`metrics.cpp:849-861`）。
- **标签基数无上限**：子指标一旦创建就不删除；用用户输入（URL、用户 ID）做标签值会
  让 `SeriesCount()` 无界增长。需要限制时由调用方做白名单/归一化，模块不替业务裁剪。
- **`Render()` 持注册表锁**：持锁时间与指标数成正比；抓取频率高、指标多时避免在
  热路径频繁调用（`metrics.cpp:1181-1186`）。
- **桶边界不可变**：同名 Histogram 用不同桶注册会抛异常，不能在线改桶
  （`metrics.hpp:468-469`、`metrics.cpp:1109-1116`）。
- **文本导出不是完整转义器**：只处理 `\`、`"`、`\n`，控制字符在入口就被拒绝，因此
  不会写出破坏行结构的样本（`metrics.hpp:62`）。

## 9. 与运行时其他模块的关系

- `metrics` 与调度器内部的 `SchedulerMetrics`（`scheduler.hpp:355-366`）是**两套**：
  前者是通用 Prometheus 指标，后者是调度器自用的累计计数器，通过 dump 暴露
  （见 [debug.md](debug.md) §4）。模块之间没有编译依赖。
- 指标导出端点由业务自己接（例如 HTTP handler 调 `Registry::Default().Render()`），
  模块不内置 HTTP server。
- `-DGO2CPP_WITH_MCP=ON` 等可选组件不改变本模块行为；`go2cpp_metrics` 是独立 target
  （`cmake/Go2CppTests.cmake:745-754` 链接它）。

## 10. 测试（对应 ctest）

| ctest | 覆盖 | 源码 |
|---|---|---|
| `go2cpp_metrics_test` | Counter 并发不丢、Gauge NaN/Inf、Histogram 分桶与分位、标签族缓存、同名冲突、Render 文本 | `tests/metrics/test_metrics.cpp`；注册在 `cmake/Go2CppTests.cmake:745-754` |

```bash
ctest --test-dir /tmp/build-ci-sim -R go2cpp_metrics_test --output-on-failure
# 2026-10-26 实跑：Passed 1.96 s（/tmp/build-ci-sim，11 项相关测试全通过）
```

压测口径建议：并发 `Inc()` 后的终值必须等于调用次数（`fetch_add` 语义）；Gauge 在
多线程 `Add` 后允许近似但不得丢更新；Histogram 的 `Count()` 必须等于各桶计数之和。

## 11. 变更记录

- 2026-10-26：首版。按 `HEAD 27e892b` 核对 API/行号，补标签族语义、默认桶清单、
  分位插值公式、校验规则表、标签基数与 `Render()` 持锁边界；实跑 `go2cpp_metrics_test`
  通过。
