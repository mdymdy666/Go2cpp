# 模块文档：Prometheus 文本导出与 `/metrics` 端点

> 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL）；构建目录 `.build-bento-wsl`（Release）。
> 新增源码：`include/go2cpp/metrics/prometheus.hpp`、`include/go2cpp/metrics/endpoint.hpp`、
> `src/metrics/prometheus.cpp`、`src/metrics/endpoint.cpp`；
> 测试：`tests/metrics/test_prometheus.cpp`、`tests/metrics/test_prometheus_endpoint.cpp`、
> `tests/metrics/bench_prometheus.cpp`；CMake：`cmake/Go2CppMetricsPrometheus.cmake`。
> 不改动现有 `metrics.hpp` / `metrics.cpp`；本文只描述新增层的行为。
> 状态：**现行**，未提交（多 agent 共享工作树）。

## 1. 概述

`go2cpp::metrics` 已经提供 Counter / Gauge / Histogram 与 `Registry::Render()`
（基础文本输出）。本模块在它之上补两件事：

1. **规范化的导出层**（`prometheus.hpp`）：把 `metrics::Registry` 渲染成
   Prometheus 文本格式 0.0.4，并额外输出 histogram 分位数（p50/p90/p99）；
2. **HTTP 端点**（`endpoint.hpp`）：把这份文本挂到现有 `bento::HttpServer` 的
   `/metrics`，供 Prometheus 抓取。

设计取舍：**不引入 Prometheus 客户端依赖**，只依赖 C++17；导出热路径只有
`std::string::append` 与 `std::to_chars`，不构造中间字符串、不用 `ostringstream`。

```
业务代码                  metrics::Registry          prom::Exporter
 Inc/Set/Observe  ──►   Counter/Gauge/Histogram  ──►  Registry::Render()
                                                        │ + 分位数后处理
                                                        ▼
                                              Prometheus 文本 ──► PrometheusEndpoint
                                                                     │ GET /metrics
                                                                     ▼
                                                                 bento::HttpServer
```

## 2. 文本格式

### 2.1 三种基础指标

```
# HELP http_requests_total Total HTTP requests.
# TYPE http_requests_total counter
http_requests_total{method="GET",path="/api"} 42

# TYPE http_in_flight gauge
http_in_flight 2.5

# TYPE http_request_duration_seconds histogram
http_request_duration_seconds_bucket{method="GET",le="0.005"} 1
http_request_duration_seconds_bucket{method="GET",le="0.01"} 2
http_request_duration_seconds_bucket{method="GET",le="+Inf"} 3
http_request_duration_seconds_sum{method="GET"} 0.483
http_request_duration_seconds_count{method="GET"} 3
```

规则（与 exposition format 0.0.4 一致）：

- 每个指标族先 `# HELP`（可选）后 `# TYPE`，`TYPE` 必须是
  `counter` / `gauge` / `histogram` / `summary` / `untyped`；
- 无标签时省略 `{}`（`name 1` 与 `name{} 1` 等价，前者更短）；
- 标签值转义 `\\`、`\"`、`\n`；HELP 转义 `\\`、`\n`（HELP 里的 `"` 不转义）；
- 浮点输出 `NaN` / `+Inf` / `-Inf`，其余用 `std::to_chars` 的最短往返表示；
- histogram 的 `_bucket` 是**累计**计数，末尾必须有 `le="+Inf"`，并同时有
  `_sum` 与 `_count`；counter 的值必须是非负整数。

### 2.2 分位数族（为什么不是 `<name>{quantile="0.5"}`）

Prometheus 文本格式中，同一指标名只能有一种 `TYPE`。Histogram 的
`_bucket` / `_sum` / `_count` 已经占用 `<name>` 这个族；把
`<name>{quantile="0.5"}` 混进去会让抓取端把它当成与 histogram 冲突的第二种
类型而拒收。因此本模块输出**独立族**：

```
# HELP http_request_duration_seconds_quantile Estimated quantiles derived from histogram buckets.
# TYPE http_request_duration_seconds_quantile gauge
http_request_duration_seconds_quantile{method="GET",quantile="0.5"} 0.08
http_request_duration_seconds_quantile{method="GET",quantile="0.9"} 0.4
http_request_duration_seconds_quantile{method="GET",quantile="0.99"} 0.4
```

- 标签 = 原 histogram 的标签 + `quantile`；类型 `gauge`；
- 分位数由**桶内线性插值**估算，语义与 `Histogram::Quantile()` 完全一致：
  目标落在 `+Inf` 桶时返回最后一个有限上界，空直方图返回 `NaN`，
  第一个桶的下界取 `min(0, 上界)`；
- 默认 `p50 / p90 / p99`，可用 `ExporterOptions::quantiles` 覆盖；
- 原标签里已经有 `quantile` 的序列会被跳过（避免重复标签名让抓取端拒收
  整个样本），对应族也不会输出只有 HELP/TYPE 的空壳。

### 2.3 实现路径

`prom::Exporter::Render()` 分两步：

1. `registry.Render()` 得到基础文本（现有实现，逐族 HELP/TYPE + 样本）；
2. `AppendQuantileFamilies()` **扫描文本**找 `# TYPE <name> histogram` 与
   `<name>_bucket{...}` 行，按标签分组重算分位数，追加 `<name>_quantile` 族。

为什么不直接读 `Histogram` 对象：`Registry` 没有对外的遍历接口（指标容器
私有），`Metric::AppendPrometheus()` 是 protected；在不修改现有
`metrics.hpp` / `metrics.cpp` 的边界下，文本是唯一能拿到全部序列的公开数据源。
解析器只认本模块 / `Registry::Render()` 的导出形态（`le` 在标签末尾、桶按
升序连续），不符合的行会被跳过，不会产生半行输出。

## 3. API

### 3.1 `prom::TextWriter`（纯拼接写入器）

```cpp
std::string out;
out.reserve(1 << 16);
go2cpp::metrics::prom::TextWriter writer(out);
writer.Header("http_requests_total", "Total HTTP requests.",
              go2cpp::metrics::MetricType::kCounter);
const go2cpp::metrics::prom::Label tags[] = {{"method", "GET"}};
writer.Sample("http_requests_total",
              go2cpp::metrics::prom::LabelView(tags, 1),
              static_cast<std::uint64_t>(42));
// out 里已经是完整文本
```

- `LabelView` 是 `Label* + size` 的只读视图，不拥有标签；热路径不分配容器；
- `LabelView::From({...})` 用于临时标签集合（只在当前表达式内有效）；
- `Histogram(...)` 一次写完整序列（桶 + `_sum` + `_count`）；
- `QuantileHeader` / `QuantileSample` 写分位数族。

### 3.2 `prom::Exporter`

```cpp
go2cpp::metrics::prom::ExporterOptions options;
options.include_quantiles = true;          // 关掉时输出 == registry.Render()
options.quantiles = {0.5, 0.9, 0.99};

std::string text = go2cpp::metrics::prom::Render(registry, options);
// 或
go2cpp::metrics::prom::Exporter exporter(registry);
std::string text2 = exporter.Render(options);
```

线程安全：只读注册表；每次 `Render()` 使用独立缓冲，可并发调用
（`Registry::Render()` 内部持锁，返回某个时刻的一致快照）。

### 3.3 `prom::AppendQuantileFamilies`（对任意文本追加分位数）

```cpp
std::size_t added = go2cpp::metrics::prom::AppendQuantileFamilies(text, options, out);
// 允许 out 与 text 是同一个 std::string：内部先构建增量再 append
```

返回追加的字节数；没有 histogram 时返回 0。

### 3.4 端点

```cpp
go2cpp::metrics::PrometheusEndpoint endpoint(registry);

// 1) 独占服务端
go2cpp::bento::HttpServer server;
endpoint.Attach(server);
server.Listen("127.0.0.1:9100");
server.Start();

// 2) 与业务路由共存
endpoint.Attach(server, [](const go2cpp::http::Request& req) {
    go2cpp::http::Response response;
    response.body = "hello";
    return response;
});

// 3) 不走 HTTP，直接拿文本（测试 / 写文件）
std::string text = endpoint.Render();
```

`Attach` 是模板：`endpoint.hpp` 只依赖 `network/http.hpp`，不硬依赖
`bento` 库；任何提供 `OnRequest(std::function<Response(const Request&)>)` 的
服务端都能挂。

响应语义：

| 情况 | 状态码 | 说明 |
|---|---|---|
| `GET /metrics` | 200 | `Content-Type: text/plain; version=0.0.4; charset=utf-8` |
| `HEAD /metrics` | 200 | 返回完整 body，由 HTTP 层按 HEAD 只发头（`Content-Length` 仍是 GET 的长度） |
| 其它路径 | 404 | 纯文本 `404 page not found` |
| 非 GET/HEAD | 405 | `Allow: GET, HEAD` |

`EndpointOptions` 可改路径、`ExporterOptions` 与 `Content-Type`。

## 4. 接入与运行

### 4.1 构建

```bash
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=ON
cmake --build .build-bento-wsl --target \
    go2cpp_metrics go2cpp_metrics_prometheus_test \
    go2cpp_metrics_endpoint_test go2cpp_metrics_prometheus_bench -j8
```

### 4.2 测试

```bash
ctest --test-dir .build-bento-wsl -R go2cpp_metrics --output-on-failure
# 55  go2cpp_metrics_test            Passed（现有，未改动）
# 222 go2cpp_metrics_prometheus_test Passed  84 断言
# 223 go2cpp_metrics_endpoint_test   Passed  38 断言
```

`test_prometheus.cpp` 覆盖：基础格式、标签/HELP 转义、NaN/±Inf/uint64 极值、
分位数与 `Histogram::Quantile()` 对拍、空/溢出/负值/标签冲突、自定义分位点、
`TextWriter` 与 `Registry::Render()` 逐字节一致、`AppendQuantileFamilies`
独立/别名调用、并发导出（8 读 × 100 次 + 4 写）、1e4 系列、HTTP 服务场景。

`test_prometheus_endpoint.cpp` 覆盖：直接 `Handle`（200/404/405/HEAD/查询串/
自定义路径/Content-Type）、真实 `bento::HttpServer` + `http::Get` 端到端、
`Attach(server, fallback)`、4 线程 × 10 次并发抓取。

### 4.3 与官方解析器交叉验证

```bash
./.build-bento-wsl/go2cpp_metrics_prometheus_test --dump=/tmp/metrics_prom_dump.prom
python3 -c 'from prometheus_client.parser import text_string_to_metric_families as p; \
            print(len(list(p(open("/tmp/metrics_prom_dump.prom").read()))))'
# 3   （histogram / counter / gauge 三个族全部被官方解析器接受）
```

## 5. 性能

基准 `tests/metrics/bench_prometheus.cpp`，`.build-bento-wsl`（Release）、
`taskset -c 2`、1e6 系列、3 次取最小，两轮复测（load ≈ 3.9）：

```
第 1 轮 / 第 2 轮
writer      28.04 / 30.78 ns/metric   1349 / 1229 MB/s
baseline   131.25 / 95.94 ns/metric    289 /  395 MB/s
registry   100.53 / 110.46 ns/metric   377 /  343 MB/s
quantile     1.235 / 1.130 µs/系列（追加 113 / 124 MB/s）
memcpy      7808 / 8679 MB/s（37.83 MB）
```

- **writer** 是纯字符串拼接下限：1e6 条 `name{id="…"} value`，一次
  `reserve` + 每样本 3~4 次 `append` + 一次 `to_chars`，**28~31 ns/指标**；
  相对同字节数 `memcpy` 是 **5.8~7.1×**。
- **baseline / registry** 是 `Registry::Render()`（`std::map` 遍历 +
  现有格式化 + 从 256 KB 起反复扩容到 37 MB），约 **96~131 ns/指标**、
  **289~395 MB/s**；其中导出层本身（writer 口径）只占 ~28 ns，
  其余是现有数据源成本 —— 本模块没有修改 `metrics.cpp`，这部分不在本文件
  的优化边界内。
- **quantile** 对 4.71 MB 文本、1e4 个 histogram 系列（10 桶）追加
  1.40 MB 分位数文本：**11.3~12.4 ms**，约 **1.13~1.24 µs/系列**（每系列
  11 个桶 + 3 个分位点）。首版对每个桶行线性扫描全部序列是 O(n²)
  （934 ms），改为「上一行序列快路径 + 标签哈希索引」后 **75×**。
- 分配：`TextWriter` 热路径 0 次堆分配；`Exporter` 只分配最终文本与
  分位数追加缓冲；分位数解析为每个序列分配一个标签串 + 桶数组。

## 6. 边界与已知限制

- **分位数是桶内插值估算**，误差上界约一个桶宽；要更准需要更细的桶；
- 文本解析只支持本模块 / `Registry::Render()` 的形态：`le` 必须在标签末尾、
  桶必须升序连续；第三方 exporter 的文本（带 timestamp、乱序桶）会被跳过；
- `HEAD` 返回完整 body，依赖 HTTP 层按 HEAD 裁剪；自己接 HTTP 时要复用该语义；
- 端点不内置缓存：每次抓取都重新渲染（`Registry::Render()` 持锁）。指标规模
  大、抓取频率高时应由调用方加缓存；
- 标签值只转义 `\\`、`\"`、`\n`，其它控制字符原样输出（抓取端可能拒收整行）；
- `Attach(server)` 会替换服务端已有的 `OnRequest`；需要保留原 handler 时用
  `Attach(server, fallback)`。

## 7. 与 `metrics.md` 的关系

- `docs/modules/metrics.md` 描述 Counter / Gauge / Histogram 的数据模型与
  `Registry::Render()`；本文只描述它之上的导出格式与端点；
- 现有 `Registry::Render()` 的输出仍是本模块的基础输入，行为未改动；
- 新增的 `prom::TextWriter` 与 `metrics.cpp` 内部的 `AppendSample` 是两套
  实现（后者在匿名命名空间、不可复用），输出逐字节一致由
  `TestTextWriterParity` 保证。
