# 可观测（交接文档）

> **定位**：零外部依赖的可观测层 —— 指标（数字）、日志（文本）、配置（开关）、事件环/诊断（时序事实）。
> **源码**：`include/go2cpp/{metrics,logging,config,debug}/`；实现 `src/{metrics,logging,config,debug}/`
> **构建接入**：库目标 `go2cpp_metrics`、`go2cpp_log`、`go2cpp_config`、`go2cpp_debug_event`、`go2cpp_debug`；选项 `GO2CPP_DEBUG_DUMP`、`GO2CPP_DIAGNOSTIC`、`GO2CPP_REPLAY`（默认全 OFF）、`GO2CPP_WITH_OPS_LOG_CONFIG`
> **测试目标**：`go2cpp_metrics_test`、`go2cpp_metrics_prometheus_test`、`go2cpp_metrics_endpoint_test`、`go2cpp_log_buffer_test`、`go2cpp_log_rotate_test`、`go2cpp_config_watch_test`、`go2cpp_config_registry_macro_test`、`go2cpp_so_manifest_test`、`go2cpp_resource_history_test`、`go2cpp_park_wake_pair`、`go2cpp_deadlock_detection`、`go2cpp_debug_record_correctness`
> **设计全文**：[metrics.md](../../docs/modules/metrics.md)、[metrics-prometheus.md](../../docs/modules/metrics-prometheus.md)、[logging.md](../../docs/modules/logging.md)、[config.md](../../docs/modules/config.md)、[debug.md](../../docs/modules/debug.md)、[ops-log-config.md](../../docs/modules/ops-log-config.md)

## 1. 这个模块是什么

四层设施，每层回答一个**不同的问题**，可单独用，也可串成「当前数值 → 为什么 → 时序关系」的链路：

| 层 | 回答的问题 | 数据形态 | 库目标 |
|---|---|---|---|
| `metrics` | 「现在 QPS / 延迟分布是多少」 | 原子计数 + 桶直方图 → Prometheus 文本 | `go2cpp_metrics` |
| `logging` | 「刚才那次失败为什么失败」 | 文本行，Filter → Formatter → Sink | `go2cpp_log` |
| `config` | 「开关怎么不重启就改」 | INI + 强类型变量 + 两阶段事务 | `go2cpp_config` |
| `debug` | 「这 5 秒哪个 G 在等哪把锁」 | 每线程环形事件、资源历史、dump/JSON | `go2cpp_debug_event`、`go2cpp_debug` |

统一取舍是**不引第三方库**：不引 Prometheus 客户端、不引 spdlog/fmt、不引 zlib（轮转的 gzip 用仓库自己的 `compress`）。理由是项目定位「单文件可部署」，多一个动态依赖就多一个现场装不上的理由。

能力边界一句话：**这是「观测」不是「追踪」**——没有 span、没有 trace_id 跨进程传播、没有采样决策中心；跨请求关联靠业务把 id 写进日志或标签。

## 2. 架构 / 数据流

### 2.1 写侧：业务线程上的三条热路径

```
 业务代码
   ├─ metrics:  counter.Inc() / gauge.Set(v) / hist.Observe(t)
   │             └─► 原子 fetch_add / CAS，无锁无分配 ──► Registry（mutex 只护注册）
   ├─ logging:  GO2CPP_LOG_INFO(log) << ... → LogLine 构造（抓时间/线程）→ 析构提交
   │             └─► LogItemWorker::Submit
   │                   ① Filter::Accept（不过滤就不格式化）  ② Formatter::Format
   │                   ③ Sink::Write（File/Stdout/Memory/Callback）
   └─ debug:    GO2CPP_DEBUG_EVENT(...) / GO2CPP_DEBUG_RESOURCE_EVENT(...)
                 └─► EventRing::Record（OFF 构建展开为 ((void)0)，参数不求值）
                       └─► per-thread 普通区 256 + 关键区 64 两段环
```

三条路径的共同点：**判级别 / 判采样发生在构造之前**。`ShouldLog` 只读一个原子量；`ResourceEventWanted()` 在构造 112 B 结构体之前判定；指标没有开关，但每次更新是单条原子指令。

### 2.2 读侧：四个出口

```
                  ┌──────────── Prometheus 抓取 ────────────┐
                  │ GET /metrics                             │
                  ▼                                          │
 Registry ─► prom::Exporter::Render() ─► PrometheusEndpoint ─► bento::HttpServer
     │              └ + 分位数族 <name>_quantile（gauge），基础文本复用 Registry::Render()
 LoggerManager ─► log/go2cpp.log ─► RotatingLogFile（大小/时间轮转 + gzip + 保留 N 份）
 EventRing ─► Snapshot(max) ─► DumpText / DumpJson ─► 文件或 DebugServer 响应
     │                        └ BuildViewJson("gmp"/"timeline"/"history"/…)
 DebugServer：Unix socket /tmp/go2cpp-<pid>.sock（0600）
   PING / LIST / STATUS / DUMP <component> / EVENTS / VIEW <name> / SUBSCRIBE events
   Doctor（GO2CPP_DIAGNOSTIC=ON）另开 G2CD/1 二进制帧，做受控唤醒与死锁判定
```

### 2.3 线程模型（交接重点）

| 线程 | 谁创建 | 干什么 | 结束方式 |
|---|---|---|---|
| 业务线程 | 调用方 | 写指标 / 日志 / 事件环 | 与业务一致 |
| 日志压缩线程 | `RotatingLogFile` 懒启动 | 压缩归档、删除过期归档，FIFO 串行 | `Close()` 等队列清空 |
| 配置监听线程 | `Config::StartWatcher` / `ConfigWatcher::Start` | inotify 或轮询，只置脏标记 | `StopWatcher()` / `Stop()` |
| 配置重载线程 | `ConfigWatcher<T>` | debounce → load → validate → 原子发布 → 回调 | 同上 |
| DebugServer 会话线程 | `DebugServer::Start` | 一连接一命令，只读 | `Stop()` |
| Doctor 接收 + 执行线程 | `DiagnosticAgent::Start` | 接收只做 I/O；执行严格串行 | `Stop()` |

**关键约定**：`ConfigWatcher` 的 `OnReload` / `OnError` 回调跑在**重载线程**；回调里禁止 `Stop()` / `ReloadNow()` / 析构 watcher，要驱动协程请用 `OnReloadEvent()` 投递。Doctor 执行线程单线程，所以一条 `status` 的多个字段是相邻读取，不会与另一条命令交叉。

### 2.4 一次配置热更新的完整往返

```
编辑方                                 监听方（进程内）
 写 go2cpp.ini.lock（非空）→ 写 .tmp      │
 rename(tmp → go2cpp.ini) ──────────────►│ inotify IN_CLOSE_WRITE / IN_MOVED_TO
 截断 .lock 为空                          ▼
                                  Config::LoadFromIni 八步
                                    1 取事务锁  2 快照全部变量 + 候选值表  3 ValidateString
                                    4 TransactionValidator  5 ApplyString（只写）  6 释放锁
                                    7 统一 NotifyPending  8 提交回调（异常不回滚）
```

`BindLoggingConfig` 把 `log.level`/`log.stdout`/`log.directory`/`log.file`/`log.format` 注册成 `ConfigVar`，任一项变化都读全五项后调用**一次** `LoggerManager::Configure`，避免「新级别 + 旧格式」的中间态。

## 3. 对外 API（关键接口）

### 3.1 指标（`include/go2cpp/metrics/metrics.hpp`）

```cpp
class Registry final {
public:
    static Registry& Default();
    Counter& GetCounter(const std::string& name, const std::string& help,
                        std::vector<std::string> label_names = {});
    Gauge& GetGauge(const std::string& name, const std::string& help,
                    std::vector<std::string> label_names = {});
    Histogram& GetHistogram(const std::string& name, const std::string& help,
                            std::vector<std::string> label_names = {},
                            std::vector<double> buckets = {});
    std::string Render() const;      // Prometheus 文本，按名字序稳定
};
// Counter: Inc()/Add(n)/Value()/With(labels)   Gauge: Set/Inc/Dec/Add/Value/With
// Histogram: Observe(v)/Count()/Sum()/Quantile(q)/CumulativeBucketCounts()/With
```

带标签的**根对象不能直接写**（`Inc`/`Set`/`Observe` 抛 `std::logic_error`），必须先 `With({...})`；同名重复注册时类型/help/标签名/桶任一不同即抛 `std::invalid_argument`——启动期失败，不是运行期容错。

### 3.2 Prometheus 导出与端点

```cpp
struct ExporterOptions { bool include_quantiles{true}; std::vector<double> quantiles{0.5, 0.9, 0.99}; };
std::string Render(const Registry& registry, const ExporterOptions& options = {});
struct EndpointOptions { std::string path{"/metrics"}; prom::ExporterOptions exporter{};
                         std::string content_type{"text/plain; version=0.0.4; charset=utf-8"}; };
class PrometheusEndpoint {
public:
    http::Response Handle(const http::Request& request) const;
    std::string Render() const;                        // 不走 HTTP，直接拿文本
    bool Matches(std::string_view path) const noexcept;
    template <class Server> void Attach(Server& server);                    // 独占
    template <class Server, class Fallback> void Attach(Server& server, Fallback fallback);
};
```

| 情况 | 状态码 | 说明 |
|---|---|---|
| `GET /metrics` | 200 | `text/plain; version=0.0.4; charset=utf-8` |
| `HEAD /metrics` | 200 | 返回完整 body，由 HTTP 层只发头 |
| 其它路径 / 非 GET、HEAD | 404 / 405 | 纯文本 `404 page not found`；405 带 `Allow: GET, HEAD` |

分位数输出成**独立族** `<name>_quantile{...,quantile="0.5"}`（类型 `gauge`）：同一指标名只能有一种 `TYPE`，塞进 histogram 族会被抓取端拒收整族。

### 3.3 日志与脱敏

```cpp
class LoggerManager final {      // include/go2cpp/logging/log.hpp
public:
    static LoggerManager& Instance();
    Logger::ptr Get(const std::string& name);
    void Configure(Level level, bool stdout_enabled, const std::string& directory,
                   const std::string& file, const std::string& pattern = {});
    void Flush();
};
using LogLevel = log::Level;     // go2cpp/log.hpp 的公开别名

// include/go2cpp/logging/redact.hpp
inline std::string KeyId(std::string_view key);          // SHA-256 前 8 位 hex
class Redactable { enum class Kind : std::uint8_t { kKey, kToken };
                   const std::string& redacted() const noexcept; };  // operator<< 只输出它
inline Redactable Redact(std::string_view key);
inline Redactable RedactToken(std::string_view token);
```

脱敏「构造时就算好」：`Redactable` 在构造里算 `redacted`，`operator<<` 只吐脱敏串，所以任何 `<<` 到日志的路径都不可能漏明文。密钥印成 `key_id=<8 hex>`，token 印成 `tok_***`。

### 3.4 轮转与配置热更新

```cpp
enum class RotationPeriod : std::uint8_t { kNone, kHourly, kDaily };
struct RotationOptions {         // include/go2cpp/logging/rotate.hpp
    std::size_t max_bytes{10u * 1024u * 1024u};  std::size_t max_files{5};
    RotationPeriod period{RotationPeriod::kNone}; bool compress{true};
    std::size_t write_buffer_bytes{64u * 1024u};
    std::size_t max_pending_bytes{64u * 1024u * 1024u};
    bool fsync_on_rotate{true}; bool create_dirs{true};
    std::function<std::chrono::system_clock::time_point()> clock;   // 可注入，测跨天
};
class RotatingLogFile final {
public:
    explicit RotatingLogFile(std::string path, RotationOptions options = {});
    std::size_t Write(std::string_view data);
    bool Flush(); bool Sync(); bool Reopen(); bool RotateNow(); void Close();
    std::uint64_t rotations() const; std::uint64_t dropped_bytes() const;
    std::size_t pending_bytes() const; std::string last_error() const;
};

class Config final {             // include/go2cpp/config/config.hpp
    static Config& Instance();
    template <typename T>
    std::shared_ptr<ConfigVar<T>> Lookup(const std::string& name, const T& default_value,
                                        const std::string& description);
    bool LoadFromIni(const IniFile& ini, std::string* error = nullptr);
    bool LoadFromFile(const std::string& path, std::string* error = nullptr);
    bool SaveToFile(const std::string& path, std::string* error = nullptr) const;
    bool StartWatcher(const std::string& path,
                      std::chrono::milliseconds interval = std::chrono::milliseconds(1000),
                      std::string* error = nullptr);
    void StopWatcher();
};
template <typename T> class ConfigWatcher final {   // include/go2cpp/config/watch.hpp
    ConfigWatcher(std::string path, Loader loader, Options options = {});
    bool Start(std::string* error = nullptr); void Stop();
    Snapshot Load() const noexcept;            // 读侧无锁（ConfigStore 8 槽位）
    std::uint64_t reloads() const noexcept; std::uint64_t failed_reloads() const noexcept;
    bool ReloadNow(std::string* error = nullptr);
};
```

### 3.5 事件环与在线诊断

```cpp
class EventRing final {          // include/go2cpp/debug/debug_event_ring.hpp
public:
    static void Record(...);                       // OFF 构建由宏展开为 ((void)0)
    static bool Enabled() noexcept;
    static std::uint64_t TotalCount(SchedEventCode code) noexcept;
    static std::uint64_t TotalCountAll() noexcept;
    static std::vector<EventRecord> Snapshot(std::size_t max = 0);          // 取最新
    static EventCursor CurrentCursor();
    static std::vector<EventRecord> ReadSince(const EventCursor& cursor, std::size_t max);
    static std::string DumpText(std::size_t max = 0);
    static std::string DumpJson(std::size_t max = 0);
};
struct DebugConfig {             // include/go2cpp/debug/debug_config.hpp
    bool lifecycle{true}; bool sched{true}; bool park{true}; bool primitives{true};
    bool timer{true}; bool io{true}; bool stacks{true}; bool chains{true};
    std::uint32_t sample_shift{0};
    static DebugConfig Get() noexcept; static void Set(const DebugConfig&) noexcept;
    static bool Enabled(DebugCategory) noexcept;
    static bool ParseCategories(const char* text, DebugConfig* out) noexcept;
    static bool LoadFromEnv() noexcept;
};
class DebugServer final {        // include/go2cpp/debug/debug_server.hpp
public:
    static DebugServer& Instance();
    bool Start(const std::string& path = {});      // 默认 /tmp/go2cpp-<pid>.sock
    void Stop();
    bool Register(const std::string& name, const std::string& description, DebugProvider provider);
    DumpPayload Dump(const std::string& component) const;
    std::string HandleCommand(const std::string& line);
};
bool RegisterScheduler(const std::string& name, Scheduler& scheduler);
bool RegisterEventRing(const std::string& name = kComponentEvents);
```

`Snapshot(max)` 取**最新** `max` 条并按 `(ts_ns, thread_id, seq)` 排序；`ReadSince` 保留**最旧**的 `max` 条，保证订阅端游标连续推进；`CurrentCursor()` 建立订阅起点。

## 4. 最小用法（可编译示例）

```cpp
// obs_demo.cpp —— 指标 + 分位数导出 + 一条脱敏日志
#include <iostream>
#include "go2cpp/metrics/metrics.hpp"
#include "go2cpp/metrics/prometheus.hpp"
#include "go2cpp/log.hpp"
#include "go2cpp/logging/redact.hpp"

int main() {
    auto& registry = go2cpp::metrics::Registry::Default();
    auto& requests = registry.GetCounter("demo_requests_total", "Demo requests", {"method"});
    requests.With({"GET"}).Inc();
    auto& latency = registry.GetHistogram("demo_latency_seconds", "Demo latency", {},
                                          {0.001, 0.005, 0.01, 0.05, 0.1});
    latency.Observe(0.003);
    latency.Observe(0.020);

    go2cpp::LoggerManager::Instance().Configure(go2cpp::LogLevel::Info, /*stdout=*/true,
                                                "log", "go2cpp.log");
    auto logger = go2cpp::LoggerManager::Instance().Get("demo");
    GO2CPP_LOG_INFO(logger) << "psk " << go2cpp::log::Redact("super-secret-key")
                            << " requests=" << requests.With({"GET"}).Value();

    go2cpp::metrics::ExporterOptions options;
    options.quantiles = {0.5, 0.99};
    std::cout << go2cpp::metrics::prom::Render(registry, options);
    go2cpp::LoggerManager::Instance().Flush();   // 不 Flush 时低级别日志可能还在缓冲里
    return 0;
}
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target go2cpp_metrics go2cpp_log -j
g++ -std=c++17 -O2 -Iinclude obs_demo.cpp -Lbuild -lgo2cpp_metrics -lgo2cpp_log -lpthread -o obs_demo
./obs_demo | head -20
```

挂到 HTTP 上（`bento::HttpServer` 存在时）：`PrometheusEndpoint endpoint(registry); endpoint.Attach(server); server.Listen("127.0.0.1:9100");`——`Attach` 会替换服务端已有的 `OnRequest`，要保留原 handler 用 `Attach(server, fallback)`。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `metrics/metrics.hpp` / `src/metrics/metrics.cpp` | 526 / 1,201 | 三种指标、标签族缓存、注册表、基础文本输出 |
| `metrics/prometheus.hpp` / `src/metrics/prometheus.cpp` | 309 / 846 | 规范化导出、`TextWriter`、分位数族追加 |
| `metrics/endpoint.hpp` / `src/metrics/endpoint.cpp` | 143 / 66 | `/metrics` 端点、状态码、Content-Type |
| `logging/log.hpp` / `src/logging/log.cpp` | 1,147 / 1,243 | Filter/Formatter/Sink 三段、Worker 快照、LoggerManager |
| `logging/redact.hpp` | 261（header-only） | `KeyId` / `Redactable` / `RedactedField` |
| `logging/rotate.hpp` | 1,025（header-only） | 缓冲写、大小/时间轮转、gzip 归档、保留清理、后台线程 |
| `config/config.hpp` / `src/config/config.cpp` | 772 / 1,324 | INI、`ConfigVar` 两阶段事务、注册表、watcher |
| `config/watch.hpp` | 1,751（header-only） | `FileWatcher`、`ConfigStore`（8 槽无锁读）、`ConfigWatcher<T>` |
| `config/so_manifest.hpp` / `src/config/so_manifest.cpp` | 214 / 689 | SO manifest 解析与静态校验（不碰 `dlopen`） |
| `config/so_reloader.hpp` / `src/config/so_reloader.cpp` | 311 / 521 | SO 两阶段重载编排 |
| `debug/debug_event_ring.hpp` / `src/debug/debug_event_ring.cpp` | 739 / 2,248 | 两段环、per-thread seq、采样、快照/订阅 |
| `debug/debug_dump.hpp` / `src/debug/debug_dump.cpp` | 255 / 2,807 | 组件快照契约、10 个视图、文本/JSON 序列化 |
| `debug/debug_server.hpp` / `src/debug/debug_server.cpp` | 222 / 1,315 | Unix socket、`GO2CPP-DEBUG/1`、SUBSCRIBE |
| `debug/diagnostic_*.hpp` / `src/debug/diagnostic_agent.cpp` | 约 1,900 / 3,493 | `G2CD/1` 帧、受控唤醒、死锁判定 |

头文件路径省略 `include/go2cpp/` 前缀。`cmake/Go2CppMetricsPrometheus.cmake` 把 `prometheus.cpp` / `endpoint.cpp` 直接挂到已有 `go2cpp_metrics`（对 `go2cpp_network` 是 PUBLIC 依赖），不新增库目标。

## 6. 性能数据

**有 asm 级实测**（[docs/reports/asm-debug-replay.md](../../docs/reports/asm-debug-replay.md)，2026-11-02，Release `-O3 -DNDEBUG`，12 核 WSL2）：

| 函数 | 指令 | call | 原子 | 栈帧 | 说明 |
|---|---:|---:|---:|---:|---|
| `EventRing::Record`（emit） | 171 | 12 | 1 | 144 B | 6 次 `__tls_get_addr` + 1 次 `__tls_init` |
| `EventRing::RecordSampled` | 66 | 2 | 0 | 0 | 采样判定 + 转调 |
| `EventRing::Snapshot` | 284 | 12 | 0 | 280 B | 遍历线程环 + 全量排序 |
| `EventRing::DumpText` | 3873 | 206 | 0 | 536 B | 序列化是最贵的一段 |
| `EventRing::DumpJson` | 2327 | 313 | 0 | 568 B | 每事件 60 次 `operator delete` |
| **OFF 对照**（`Record`/`RecordTimer`/`RecordSampled`） | **4** | **0** | **0** | **0** | `endbr64; ret` + 对齐 nop |

- emit：单线程 **25.95 ms / 25.95 ns 每条（38.5 M/s）**；4 线程 **7.20 ms / 7.20 ns 每条（138.8 M/s）**。
- `Snapshot` **3.70 ms/次（181 ns/条）**；`DumpText` **0.69 µs/条**；`DumpJson` **4.35 µs/条**。
- 全开 vs OFF（N=200,000、3 轮中位数）：channel **3.14×**、mutex **6.65×**、spawn **1.63×**、timer **1.44×**、resources **8.76×**、select **3.28×**；按类别降采样（`GO2CPP_DEBUG_CATEGORIES=sched,park` + `SAMPLE_SHIFT=5`）后 channel 1.8×、mutex 1.2×、spawn 1.8×、timer 1.2×、select 2.7×。

**指标导出层**（[metrics-prometheus.md](../../docs/modules/metrics-prometheus.md) §5，`taskset -c 2`、1e6 系列、两轮复测、load ≈ 3.9）：

```
writer      28.04 / 30.78 ns/metric   1349 / 1229 MB/s    ← 纯拼接下限
baseline   131.25 / 95.94 ns/metric    289 /  395 MB/s    ← Registry::Render()
quantile     1.235 / 1.130 µs/系列（追加 113 / 124 MB/s）
memcpy      7808 / 8679 MB/s（37.83 MB）
```

**轮转与热更新**（[ops-log-config.md](../../docs/modules/ops-log-config.md)，单次运行，测试机有其它任务）：1e6 条 / 约 39.9 MB / 12 次轮转，写入 **0.160 s（约 238 MB/s）**；端到端（含关闭时压缩 12 个归档）**0.317 s，约 120 MB/s**，丢行/乱序/压缩失败 0/0/0；`ConfigStore::Load()` **3.58 ns/次**，`Store()`（1 写者 + 4 读者）**1.0 µs/次、0 撕裂**；1e4 次变更（inotify，每次等版本前进）**2.066 s，206.6 µs/次，0 失败**。

**`log` / `config` 主体没有 asm 级实测**：`docs/reports/` 下没有对应报告，本文不编指令数或 ns 数字。日志只有成本结构（级别检查一次原子读；通过后每条一次 `Formatter::Format` + 一次 `Sink::Write`），要数字必须按 §8 自己量。

## 7. 边界与已知限制

1. **不是 tracing**：无 span、无 trace_id 传播、无跨进程上下文。
2. **标签基数无上限**：`With()` 创建的子指标不删除，用 URL / 用户 ID 做标签值会让 `SeriesCount()` 无界增长；模块不替业务裁剪。
3. **`Registry::Render()` 持注册表锁**：持锁时间与指标数成正比；端点不缓存，每次抓取重新渲染。
4. **分位数是桶内插值估算**：误差约一个桶宽；目标落 `+Inf` 桶时返回最后一个有限上界。
5. **日志不自动 Flush**：`BufferedSink` 只在容量/时间条件或显式 `Flush()` 时刷盘，`kByTime` 是**惰性检查**（没新日志就不刷）；崩溃前必须 `LoggerManager::Flush()`；不要在持锁期间打日志（业务锁 → stdio 锁 → 磁盘 IO 会串成一条链）。
6. **`Configure` 会重开文件**：热更新频率高会放大 `open` 成本；`stdout` 跨实例可能交错。
7. **轮转不格式化、不带级别**：只是字节流写手，接日志流水线要用 `RotatingLogSink`；`SIGKILL` 时未刷缓冲会丢；多进程写同一路径不受支持；单条大于 `max_bytes` 时仍整条写入。
8. **`ConfigStore` 只有 8 槽**：最多 7 个可长期持有，写者在空槽耗尽时等待（背压，不覆盖数据）。
9. **inotify 只听 `IN_CLOSE_WRITE` / `IN_MOVED_TO`**：mmap 写不产生 `CLOSE_WRITE`；轮询回退在同尺寸原地覆盖 + 同一文件系统时间戳粒度（常见 1–4 ms）内可能漏检；不递归目录。
10. **事件环必然截断**：普通区 256 + 关键区 64 + 采样，`window.truncated=true` 只能看趋势；关键区本身仍是环，「不丢全部」不是「绝对不丢」。
11. **10 类并发结构里 8 类可观测**：`sync::SpinMutex` 与 `core::FutexMutex` 明确不插桩；引用时不能说「10 类都能 dump」。dump 由多次独立读取拼成，一致性看 `changed_during_dump`。
12. **Doctor 默认 OFF**：`scope=mutex` 的 force-wake 一律 `refused`（Mutex 等待者醒来不会重试同一把锁），`mutex-unlock` 只接受注册名。
13. **`GO2CPP_DEBUG_DUMP=OFF` 是真零开销**（`Record` 只剩 `endbr64; ret`），但 OFF 构建里没有事件可 dump——排障先确认现场二进制是 ON 构建。

## 8. 测试与验证方法

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_DEBUG_DUMP=ON -DGO2CPP_DIAGNOSTIC=ON -DGO2CPP_BUILD_TESTS=ON
cmake --build build -j12

ctest --test-dir build -R 'go2cpp_metrics' --output-on-failure
ctest --test-dir build -R 'go2cpp_(log_buffer|log_rotate|config_watch|config_registry_macro)' \
      --output-on-failure
ctest --test-dir build -R 'so_manifest|park_wake_pair|resource_history|deadlock' --output-on-failure
GO2CPP_TEST_FILTER=debug_dump ./build/go2cpp_tests
GO2CPP_TEST_FILTER=diagnostic ./build/go2cpp_tests

# 与官方解析器交叉验证导出格式
./build/go2cpp_metrics_prometheus_test --dump=/tmp/metrics_dump.prom
python3 -c 'from prometheus_client.parser import text_string_to_metric_families as p; \
            print(len(list(p(open("/tmp/metrics_dump.prom").read()))))'   # 3
```

自建最小闭环（不依赖 Prometheus）：

1. 起一个跑 `PrometheusEndpoint` 的进程，监听 `127.0.0.1:9100`；
2. `curl -sS http://127.0.0.1:9100/metrics | grep demo_` 应看到 `_bucket` / `_sum` / `_count` / `_quantile` 四组序列；
3. `curl -sS -o /dev/null -w '%{http_code}\n' http://127.0.0.1:9100/nope` → 404；
4. `curl -sS -X POST -o /dev/null -w '%{http_code}\n' http://127.0.0.1:9100/metrics` → 405。

真实进程 + 真实 socket 的排障闭环（不在 ctest 默认集合）：`bash tests/debug/run_diagnostic_e2e.sh`（14 项断言）。验证脱敏真的生效：把 `Redact("...")` 的输出打日志后 `grep -c 'super-secret-key' log/go2cpp.log` 必须为 0，只应看到 `key_id=` 前缀的 8 位 hex。

## 9. 交接注意

- **先决定要哪一层**：只要「当前数值」用 `metrics`；要「为什么」用 `logging`；要「时序关系」必须开 `GO2CPP_DEBUG_DUMP`（OFF 构建里事件环是空壳）。
- **`metrics` 与调度器的 `SchedulerMetrics` 是两套**：前者是通用 Prometheus 指标，后者是调度器内部累计计数器（通过 dump 暴露），无编译依赖，别混用。
- **改事件环前先读 `docs/reports/asm-debug-replay.md`**：v6 把全局 `seq` 的 `fetch_add` 改成每线程自增（当年 channel 场景 20× 的主要来源），v7 又把记录/栈链捕获/调度高频事件拆成三个采样旋钮；回退任一条都会把开销推回 v5 的 3.6–40.3×。
- **加指标别加在热循环里**：`With()` 是注册表锁内的查找，每请求一次可以，内层循环要缓存子对象引用。
- **`RotatingLogFile::Close()` 会阻塞**等压缩队列清空；退出路径要留时间，不要在信号处理器里调用。
- **默认值不要改**：`/metrics` 路径、`/tmp/go2cpp-<pid>.sock`、`log/go2cpp.log` 已被 [user/logging/README.md](../logging/README.md) 与测试脚本依赖。
- **两份素材不存在**：任务提到的 `docs/modules/debug-overhead.md` 与 `debug-reliability.md` 在当前工作树没有，内容已合并进 [debug.md](../../docs/modules/debug.md) §8/§7；引用开销用本文 §6 的 asm 报告口径，v5 旧数字只能当上界。
- **未完成**：`signal::NotifyContext`、`DumpJson` 的「临时 string + 逐段 append」、replay 录制每事件一把互斥锁；改动任一模块行为请同步 `docs/modules/` 对应文档与本文 §3 的签名摘录。
