/**
 * @file metrics.cpp
 * @brief `go2cpp::metrics` 的实现：原子计数、标签缓存、直方图与文本导出。
 *
 * 设计约束：
 *   - Counter 的 `Inc` 必须是单条 `lock xadd`，不能用互斥锁或非原子自增；
 *   - Histogram 的每个桶也必须是原子计数，`Sum` 用位模式 CAS 维护；
 *   - 标签子指标存放在根对象的 `std::map` 中，引用稳定、按标签缓存；
 *   - 渲染时先把元数据与序列拼进一个 `std::string`，避免调用方逐行加锁。
 *
 * ## 模块定位
 * `metrics.hpp` 的实现：名字/标签校验、double 的原子位运算、三种指标的族管理与
 * Prometheus 文本渲染。无第三方依赖。
 *
 * ## 本文件定义什么
 * - 匿名命名空间的校验/转义/格式化工具（`IsValidMetricName`、`AppendEscapedLabelValue`、
 *   `FormatDouble`、`AppendSample`）；
 * - double 原子读写与 CAS 加法（`LoadDouble`/`StoreDouble`/`AddDouble`）；
 * - 三种指标的 Family、`With()`、`Observe()`、`Quantile()` 与 `AppendPrometheus()`；
 * - `Registry::RegisterImpl()` 与 `Render()`。
 *
 * ## 阅读建议
 * 先看 `FormatDouble`/`AppendEscapedLabelValue`（文本格式的边界），再看 `BucketIndex`
 * 与 `AddDouble`（分桶与无锁浮点加），最后看 `Histogram::Quantile()` 的插值。
 *
 * ## 边界与陷阱
 * - 标签值转义只覆盖 `\\`、`\"`、`\n`；其它字节原样输出（Prometheus 允许但需自担风险）；
 * - `FormatDouble` 对 NaN/Inf 输出 `NaN`/`+Inf`/`-Inf`（Prometheus 文本格式要求）；
 * - `Quantile()` 是估算：桶内线性插值，目标落 `+Inf` 桶时返回最后一个有限上界；
 * - `Render()` 持 `m_mutex` 输出全部指标：注册表大时会拉长注册临界区，但保证快照一致。
 */
#include "go2cpp/metrics/metrics.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace go2cpp {
namespace metrics {

namespace {

constexpr double kPosInf = std::numeric_limits<double>::infinity();
constexpr double kNegInf = -std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool IsValidMetricName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(name.front());
    if (!(std::isalpha(first) || first == '_' || first == ':')) {
        return false;
    }
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || c == '_' || c == ':')) {
            return false;
        }
    }
    return true;
}

bool IsValidLabelName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(name.front());
    if (!(std::isalpha(first) || first == '_')) {
        return false;
    }
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || c == '_')) {
            return false;
        }
    }
    return true;
}

void ValidateMetricName(const std::string& name) {
    if (!IsValidMetricName(name)) {
        throw std::invalid_argument(
            "go2cpp::metrics: invalid metric name: \"" + name + "\"");
    }
}

void ValidateHelp(const std::string& help) {
    for (const char ch : help) {
        if (ch == '\0' || ch == '\r') {
            throw std::invalid_argument(
                "go2cpp::metrics: help text contains NUL or CR");
        }
    }
}

void ValidateLabelValues(const std::vector<std::string>& values) {
    for (const auto& value : values) {
        for (const char ch : value) {
            const auto c = static_cast<unsigned char>(ch);
            if (ch == '\0' || ch == '\r' || (c < 0x20 && ch != '\n')) {
                throw std::invalid_argument(
                    "go2cpp::metrics: label value contains an unsupported "
                    "control character");
            }
        }
    }
}

void ValidateLabelNames(const std::vector<std::string>& names,
                        bool histogram) {
    std::set<std::string> seen;
    for (const auto& name : names) {
        if (!IsValidLabelName(name)) {
            throw std::invalid_argument(
                "go2cpp::metrics: invalid label name: \"" + name + "\"");
        }
        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') {
            throw std::invalid_argument(
                "go2cpp::metrics: label names beginning with \"__\" are "
                "reserved: \"" + name + "\"");
        }
        if (histogram && name == "le") {
            throw std::invalid_argument(
                "go2cpp::metrics: histogram label name \"le\" is reserved");
        }
        if (!seen.insert(name).second) {
            throw std::invalid_argument(
                "go2cpp::metrics: duplicate label name: \"" + name + "\"");
        }
    }
}

void ValidateBuckets(const std::vector<double>& buckets) {
    if (buckets.empty()) {
        throw std::invalid_argument(
            "go2cpp::metrics: histogram requires at least one bucket bound");
    }
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        if (!std::isfinite(buckets[i])) {
            throw std::invalid_argument(
                "go2cpp::metrics: histogram bucket bounds must be finite");
        }
        if (i != 0 && !(buckets[i] > buckets[i - 1])) {
            throw std::invalid_argument(
                "go2cpp::metrics: histogram bucket bounds must be strictly "
                "increasing");
        }
    }
}

detail::MetricInfo MakeInfo(std::string name, std::string help,
                            MetricType type,
                            std::vector<std::string> label_names,
                            bool validate_name) {
    if (validate_name) {
        ValidateMetricName(name);
        ValidateHelp(help);
        ValidateLabelNames(label_names, type == MetricType::kHistogram);
    }
    return detail::MetricInfo{std::move(name), std::move(help), type,
                              std::move(label_names)};
}

detail::MetricInfo StandaloneInfo(MetricType type,
                                  std::vector<std::string> label_names) {
    ValidateLabelNames(label_names, type == MetricType::kHistogram);
    return detail::MetricInfo{"", "", type, std::move(label_names)};
}

// 标签缓存的键必须无歧义：只用分隔符无法区分 {"a,b"} 与 {"a","b"}。
// 这里对每个值做长度前缀（"3:GET2:/x"），值里出现任何字节都不会串键。
std::string EncodeLabelValues(const std::vector<std::string>& values) {
    std::string key;
    for (const auto& value : values) {
        key.append(std::to_string(value.size()));
        key.push_back(':');
        key.append(value);
    }
    return key;
}

/**
 * @brief 把 double 格式化为 Prometheus 文本（含 NaN/Inf 特例）。
 *
 * ## 步骤
 * step1. NaN -> `NaN`；+/-Inf -> `+Inf`/`-Inf`；
 * step2. 其余用 `std::to_chars(general)` 输出最短表示（无 locale 依赖）；
 * step3. `to_chars` 失败返回 "0"（缓冲区足够，正常不会发生）。
 */
std::string FormatDouble(double value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0.0 ? "+Inf" : "-Inf";
    }
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::general);
    if (result.ec != std::errc{}) {
        return "0";
    }
    return std::string(buffer, result.ptr);
}

std::string FormatUint(std::uint64_t value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) {
        return "0";
    }
    return std::string(buffer, result.ptr);
}

/**
 * @brief 按 Prometheus 文本格式转义并输出一个标签值。
 *
 * ## 说明
 * 只转义 `\\`、`\"`、换行三种：这是 Prometheus 文本解析器要求的**最小集合**；
 * 其余控制字符会原样进入输出，抓取端可能拒绝整行 —— 标签值应保持简短可打印。
 */
void AppendEscapedLabelValue(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                out.append("\\\\");
                break;
            case '"':
                out.append("\\\"");
                break;
            case '\n':
                out.append("\\n");
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    out.push_back('"');
}

void AppendEscapedHelp(std::string& out, std::string_view help) {
    for (const char ch : help) {
        switch (ch) {
            case '\\':
                out.append("\\\\");
                break;
            case '\n':
                out.append("\\n");
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
}

void AppendHeader(std::string& out, const detail::MetricInfo& info) {
    out.append("# HELP ");
    out.append(info.name);
    out.push_back(' ');
    AppendEscapedHelp(out, info.help);
    out.push_back('\n');
    out.append("# TYPE ");
    out.append(info.name);
    out.push_back(' ');
    out.append(MetricTypeName(info.type));
    out.push_back('\n');
}

void AppendSample(std::string& out, std::string_view name,
                  const std::vector<std::string>& label_names,
                  const std::vector<std::string>& label_values,
                  std::string_view value, std::string_view extra_name = {},
                  std::string_view extra_value = {}) {
    out.append(name.data(), name.size());
    if (!label_names.empty() || !extra_name.empty()) {
        out.push_back('{');
        bool first = true;
        for (std::size_t i = 0; i < label_names.size(); ++i) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            out.append(label_names[i]);
            out.push_back('=');
            AppendEscapedLabelValue(out, label_values[i]);
        }
        if (!extra_name.empty()) {
            if (!first) {
                out.push_back(',');
            }
            out.append(extra_name.data(), extra_name.size());
            out.push_back('=');
            AppendEscapedLabelValue(out, extra_value);
        }
        out.push_back('}');
    }
    out.push_back(' ');
    out.append(value.data(), value.size());
    out.push_back('\n');
}

// 用位模式保存 double，绕开 C++17 没有 atomic<double>::fetch_add 的限制，
// 同时避免 NaN != NaN 让 compare_exchange 永远失败。
static_assert(sizeof(double) == sizeof(std::uint64_t),
              "metrics requires 64-bit IEEE-754 double");

double BitsToDouble(std::uint64_t bits) noexcept {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint64_t DoubleToBits(double value) noexcept {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double LoadDouble(const std::atomic<std::uint64_t>& bits) noexcept {
    return BitsToDouble(bits.load(std::memory_order_relaxed));
}

void StoreDouble(std::atomic<std::uint64_t>& bits, double value) noexcept {
    bits.store(DoubleToBits(value), std::memory_order_relaxed);
}

/**
 * @brief 对以位模式存储的 double 做无锁加法（CAS 循环）。
 *
 * ## 步骤
 * step1. `delta == 0` 直接返回；
 * step2. 读当前位模式并转成 double，加上 delta 后转回位模式；
 * step3. `compare_exchange_weak` 成功则返回，失败用新值重试。
 *
 * ## 为什么不用 `atomic<double>`
 * C++17 没有 `fetch_add` 的浮点特化；位模式 + CAS 可移植，且 `Set(NaN)` 后仍能终止。
 */
void AddDouble(std::atomic<std::uint64_t>& bits, double delta) noexcept {
    if (delta == 0.0) {
        return;
    }
    std::uint64_t expected = bits.load(std::memory_order_relaxed);
    for (;;) {
        const double next = BitsToDouble(expected) + delta;
        const std::uint64_t desired = DoubleToBits(next);
        /// step3: 失败时 expected 已被更新，直接进入下一轮重算。
        if (bits.compare_exchange_weak(expected, desired,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
            return;
        }
    }
}

/**
 * @brief 二分查找 value 应落入的桶下标（le 语义）。
 *
 * ## 说明
 * `lower_bound` 返回第一个 `boundary >= value` 的位置，正好对应 `le` 桶；
 * 返回 `boundaries.size()` 表示落进隐式 `+Inf` 桶。
 */
std::size_t BucketIndex(const std::vector<double>& boundaries, double value) {
    return static_cast<std::size_t>(
        std::lower_bound(boundaries.begin(), boundaries.end(), value) -
        boundaries.begin());
}

// Prometheus 客户端的默认延迟桶（秒）。Registry 与独立 Histogram 共用同一份
// 定义，避免两处字面量漂移导致“默认桶不一致”的注册冲突。
std::vector<double> DefaultHistogramBuckets() {
    // 前 13 个是 Prometheus 客户端惯用的延迟桶（秒）；后面向上扩展到
    // 10000，覆盖毫秒/秒级数值以及通用的 1..1000 数据，避免默认直方图
    // 把大值全部塞进 +Inf 桶后失去分位数的分辨能力。
    return {0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25,
            0.5,   1.0,    2.5,   5.0,  10.0,  25.0, 50.0, 100.0,
            250.0, 500.0,  1000.0, 2500.0, 5000.0, 10000.0};
}

}  // namespace

// ---------------------------------------------------------------------------
// Counter
// ---------------------------------------------------------------------------

/**
 * ## 定位
 * `Counter` 根对象拥有的标签族：元数据 + 子序列缓存 + 保护 children 的互斥量。
 * 子序列通过裸指针共享它，生命周期由根对象的 `m_owned_family` 保证。
 *
 * ## 不变量
 * - `children` 的键是长度前缀编码后的标签值（无歧义）；
 * - children 一旦插入就不再删除，返回给调用方的引用稳定。
 */
struct Counter::Family {
    detail::MetricInfo info;  ///< 族共享元数据；CreateFamily 写入，Name/Help/Type 读取
    mutable std::mutex mutex;  ///< 保护 children；With 查找/插入，AppendPrometheus 遍历时持有
    std::map<std::string, std::unique_ptr<Counter>> children;  ///< 标签值编码到子计数器的缓存；With 插入/查找，SeriesCount/Render 读取
};

std::unique_ptr<Counter::Family> Counter::CreateFamily(
    detail::MetricInfo info) {
    auto family = std::unique_ptr<Family>(new Family());
    family->info = std::move(info);
    return family;
}

Counter::Counter()
    : Counter(CreateFamily(
          StandaloneInfo(MetricType::kCounter, {}))) {}

Counter::Counter(std::vector<std::string> label_names)
    : Counter(CreateFamily(
          StandaloneInfo(MetricType::kCounter, std::move(label_names)))) {}

Counter::Counter(std::unique_ptr<Family> family)
    : Metric(&family->info),
      m_owned_family(std::move(family)),
      m_family(m_owned_family.get()),
      m_is_root(true) {}

Counter::Counter(Family* family, std::vector<std::string> label_values)
    : Metric(&family->info),
      m_family(family),
      m_label_values(std::move(label_values)),
      m_is_root(false) {}

Counter::~Counter() = default;

void Counter::EnsureSeries(const char* operation) const {
    if (m_is_root && !m_family->info.label_names.empty()) {
        throw std::logic_error(
            std::string("go2cpp::metrics::Counter::") + operation +
            " on a labeled metric family requires With({...})");
    }
}

void Counter::Inc() {
    Add(1);
}

void Counter::Add(std::uint64_t n) {
    EnsureSeries("Add");
    // 计数场景不需要与其他内存操作同步；relaxed 的 RMW 仍然保证
    // 每次 fetch_add 都是原子的，终值精确等于所有增量之和。
    m_value.fetch_add(n, std::memory_order_relaxed);
}

std::uint64_t Counter::Value() const {
    EnsureSeries("Value");
    return m_value.load(std::memory_order_relaxed);
}

Counter& Counter::With(std::initializer_list<std::string_view> labels) {
    std::vector<std::string> values;
    values.reserve(labels.size());
    for (const auto label : labels) {
        values.emplace_back(label);
    }
    return With(values);
}

/**
 * @brief 取得/创建标签值对应的子计数器。
 *
 * ## 步骤
 * step1. 校验标签值不含控制字符；
 * step2. 子序列只接受自己的标签值，传错直接抛 `invalid_argument`；
 * step3. 根对象校验标签个数；无标签族直接返回根对象；
 * step4. 编码标签值，持族锁查找；
 * step5. 命中返回稳定引用；未命中创建子对象并插入（只增不删）。
 */
Counter& Counter::With(const std::vector<std::string>& labels) {
    ValidateLabelValues(labels);
    if (!m_is_root) {
        if (labels != m_label_values) {
            throw std::invalid_argument(
                "go2cpp::metrics::Counter::With: child metric only accepts "
                "its own label values");
        }
        return *this;
    }

    const auto& names = m_family->info.label_names;
    if (labels.size() != names.size()) {
        throw std::invalid_argument(
            "go2cpp::metrics::Counter::With: expected " +
            std::to_string(names.size()) + " label values, got " +
            std::to_string(labels.size()));
    }
    if (names.empty()) {
        return *this;
    }

    // step4: 键用长度前缀编码，{"a,b"} 与 {"a","b"} 不会串键
    const std::string key = EncodeLabelValues(labels);
    std::lock_guard<std::mutex> lock(m_family->mutex);
    const auto found = m_family->children.find(key);
    if (found != m_family->children.end()) {
        return *found->second;
    }
    auto child = std::unique_ptr<Counter>(new Counter(m_family, labels));
    Counter* raw = child.get();
    m_family->children.emplace(key, std::move(child));
    return *raw;
}

std::size_t Counter::SeriesCount() const {
    if (!m_is_root) {
        return 1;
    }
    if (m_family->info.label_names.empty()) {
        return 1;
    }
    std::lock_guard<std::mutex> lock(m_family->mutex);
    return m_family->children.size();
}

void Counter::AppendPrometheus(std::string& out) const {
    AppendHeader(out, m_family->info);
    if (!m_is_root) {
        AppendSample(out, m_family->info.name, m_family->info.label_names,
                     m_label_values, FormatUint(Value()));
        return;
    }
    if (m_family->info.label_names.empty()) {
        AppendSample(out, m_family->info.name, m_family->info.label_names,
                     m_label_values, FormatUint(Value()));
        return;
    }
    std::lock_guard<std::mutex> lock(m_family->mutex);
    for (const auto& entry : m_family->children) {
        const Counter& child = *entry.second;
        AppendSample(out, m_family->info.name, m_family->info.label_names,
                     child.m_label_values, FormatUint(child.m_value.load(
                                               std::memory_order_relaxed)));
    }
}

// ---------------------------------------------------------------------------
// Gauge
// ---------------------------------------------------------------------------

/**
 * ## 定位
 * `Gauge` 的标签族容器，结构与 `Counter::Family` 相同；分离定义是为了让
 * children 的元素类型是 `Gauge`，避免向下转型。
 *
 * ## 不变量
 * 同 `Counter::Family`：标签键无歧义、children 只增不删、引用稳定。
 */
struct Gauge::Family {
    detail::MetricInfo info;  ///< 族共享元数据；CreateFamily 写入，Name/Help/Type 读取
    mutable std::mutex mutex;  ///< 保护 children；With 查找/插入，AppendPrometheus 遍历时持有
    std::map<std::string, std::unique_ptr<Gauge>> children;  ///< 标签值编码到子 Gauge 的缓存；With 插入/查找，SeriesCount/Render 读取
};

std::unique_ptr<Gauge::Family> Gauge::CreateFamily(detail::MetricInfo info) {
    auto family = std::unique_ptr<Family>(new Family());
    family->info = std::move(info);
    return family;
}

Gauge::Gauge()
    : Gauge(CreateFamily(StandaloneInfo(MetricType::kGauge, {}))) {}

Gauge::Gauge(std::vector<std::string> label_names)
    : Gauge(CreateFamily(
          StandaloneInfo(MetricType::kGauge, std::move(label_names)))) {}

Gauge::Gauge(std::unique_ptr<Family> family)
    : Metric(&family->info),
      m_owned_family(std::move(family)),
      m_family(m_owned_family.get()),
      m_is_root(true) {}

Gauge::Gauge(Family* family, std::vector<std::string> label_values)
    : Metric(&family->info),
      m_family(family),
      m_label_values(std::move(label_values)),
      m_is_root(false) {}

Gauge::~Gauge() = default;

void Gauge::EnsureSeries(const char* operation) const {
    if (m_is_root && !m_family->info.label_names.empty()) {
        throw std::logic_error(
            std::string("go2cpp::metrics::Gauge::") + operation +
            " on a labeled metric family requires With({...})");
    }
}

void Gauge::Set(double value) {
    EnsureSeries("Set");
    StoreDouble(m_bits, value);
}

void Gauge::Inc() {
    Add(1.0);
}

void Gauge::Dec() {
    Add(-1.0);
}

void Gauge::Add(double delta) {
    EnsureSeries("Add");
    AddDouble(m_bits, delta);
}

double Gauge::Value() const {
    EnsureSeries("Value");
    return LoadDouble(m_bits);
}

Gauge& Gauge::With(std::initializer_list<std::string_view> labels) {
    std::vector<std::string> values;
    values.reserve(labels.size());
    for (const auto label : labels) {
        values.emplace_back(label);
    }
    return With(values);
}

Gauge& Gauge::With(const std::vector<std::string>& labels) {
    ValidateLabelValues(labels);
    if (!m_is_root) {
        if (labels != m_label_values) {
            throw std::invalid_argument(
                "go2cpp::metrics::Gauge::With: child metric only accepts its "
                "own label values");
        }
        return *this;
    }

    const auto& names = m_family->info.label_names;
    if (labels.size() != names.size()) {
        throw std::invalid_argument(
            "go2cpp::metrics::Gauge::With: expected " +
            std::to_string(names.size()) + " label values, got " +
            std::to_string(labels.size()));
    }
    if (names.empty()) {
        return *this;
    }

    const std::string key = EncodeLabelValues(labels);
    std::lock_guard<std::mutex> lock(m_family->mutex);
    const auto found = m_family->children.find(key);
    if (found != m_family->children.end()) {
        return *found->second;
    }
    auto child = std::unique_ptr<Gauge>(new Gauge(m_family, labels));
    Gauge* raw = child.get();
    m_family->children.emplace(key, std::move(child));
    return *raw;
}

std::size_t Gauge::SeriesCount() const {
    if (!m_is_root) {
        return 1;
    }
    if (m_family->info.label_names.empty()) {
        return 1;
    }
    std::lock_guard<std::mutex> lock(m_family->mutex);
    return m_family->children.size();
}

void Gauge::AppendPrometheus(std::string& out) const {
    AppendHeader(out, m_family->info);
    if (!m_is_root) {
        AppendSample(out, m_family->info.name, m_family->info.label_names,
                     m_label_values, FormatDouble(Value()));
        return;
    }
    if (m_family->info.label_names.empty()) {
        AppendSample(out, m_family->info.name, m_family->info.label_names,
                     m_label_values, FormatDouble(Value()));
        return;
    }
    std::lock_guard<std::mutex> lock(m_family->mutex);
    for (const auto& entry : m_family->children) {
        const Gauge& child = *entry.second;
        AppendSample(out, m_family->info.name, m_family->info.label_names,
                     child.m_label_values,
                     FormatDouble(LoadDouble(child.m_bits)));
    }
}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

/**
 * ## 定位
 * `Histogram` 的标签族容器：除元数据/子序列外还持有**不可变**桶边界，所有子
 * 序列共享同一组 `le` 上界。
 *
 * ## 不变量
 * - `boundaries` 严格递增且有限，构造后不再修改；
 * - 所有子序列的 `m_bucket_count` 都等于 `boundaries.size() + 1`。
 */
struct Histogram::Family {
    detail::MetricInfo info;  ///< 族共享元数据；CreateFamily 写入，Name/Help/Type 读取
    std::vector<double> boundaries;  ///< 有限桶上界（le）；CreateFamily 校验后固定，Observe/Quantile/Render 只读
    mutable std::mutex mutex;  ///< 保护 children；With 查找/插入，AppendPrometheus 遍历时持有
    std::map<std::string, std::unique_ptr<Histogram>> children;  ///< 标签值编码到子直方图的缓存；With 插入/查找，SeriesCount/Render 读取
};

std::unique_ptr<Histogram::Family> Histogram::CreateFamily(
    detail::MetricInfo info, std::vector<double> buckets) {
    ValidateBuckets(buckets);
    auto family = std::unique_ptr<Family>(new Family());
    family->info = std::move(info);
    family->boundaries = std::move(buckets);
    return family;
}

Histogram::Histogram()
    : Histogram(CreateFamily(
          StandaloneInfo(MetricType::kHistogram, {}),
          DefaultHistogramBuckets())) {}

Histogram::Histogram(std::vector<double> buckets)
    : Histogram(CreateFamily(
          StandaloneInfo(MetricType::kHistogram, {}), std::move(buckets))) {}

Histogram::Histogram(std::vector<double> buckets,
                     std::vector<std::string> label_names)
    : Histogram(CreateFamily(
          StandaloneInfo(MetricType::kHistogram, std::move(label_names)),
          std::move(buckets))) {}

Histogram::Histogram(std::unique_ptr<Family> family)
    : Metric(&family->info),
      m_owned_family(std::move(family)),
      m_family(m_owned_family.get()),
      m_is_root(true) {
    InitBuckets();
}

Histogram::Histogram(Family* family, std::vector<std::string> label_values)
    : Metric(&family->info),
      m_family(family),
      m_label_values(std::move(label_values)),
      m_is_root(false) {
    InitBuckets();
}

Histogram::~Histogram() = default;

void Histogram::InitBuckets() {
    m_bucket_count = m_family->boundaries.size() + 1;
    m_bucket_counts.reset(
        new std::atomic<std::uint64_t>[m_bucket_count]);
    for (std::size_t i = 0; i < m_bucket_count; ++i) {
        m_bucket_counts[i].store(0, std::memory_order_relaxed);
    }
}

void Histogram::EnsureSeries(const char* operation) const {
    if (m_is_root && !m_family->info.label_names.empty()) {
        throw std::logic_error(
            std::string("go2cpp::metrics::Histogram::") + operation +
            " on a labeled metric family requires With({...})");
    }
}

/**
 * @brief 观察一个样本（NaN 被忽略）。
 *
 * ## 步骤
 * step1. `EnsureSeries()`：带标签的根对象直接拒绝；
 * step2. `NaN` 直接返回（会污染 Sum 与分位数）；
 * step3. 二分找桶并递增桶计数；
 * step4. CAS 累加 Sum、递增 Count（三者都是 relaxed 原子操作）。
 */
void Histogram::Observe(double value) {
    EnsureSeries("Observe");
    if (std::isnan(value)) {
        // NaN 会污染 Sum 与分位数；Prometheus 客户端同样忽略 NaN 观测。
        return;
    }
    /// step3: 落桶是 `le` 语义（第一个 >= value 的边界）。
    const std::size_t index = BucketIndex(m_family->boundaries, value);
    /// step4: 桶计数/Sum/Count 均为 relaxed —— 监控允许近似，但不能丢更新。
    m_bucket_counts[index].fetch_add(1, std::memory_order_relaxed);
    AddDouble(m_sum_bits, value);
    m_count.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Histogram::Count() const {
    EnsureSeries("Count");
    return m_count.load(std::memory_order_relaxed);
}

double Histogram::Sum() const {
    EnsureSeries("Sum");
    return LoadDouble(m_sum_bits);
}

const std::vector<double>& Histogram::Buckets() const {
    return m_family->boundaries;
}

/**
 * @brief 返回累计桶计数快照，长度 `Buckets().size() + 1`。
 *
 * @details 逐桶读取并累加；并发 Observe 时各桶来自不同瞬间，因此是**近似快照**
 * （数组仍保证单调不减，末项等于本次读到的 Count 附近值）。
 */
std::vector<std::uint64_t> Histogram::CumulativeBucketCounts() const {
    EnsureSeries("CumulativeBucketCounts");
    // 先分配再逐桶累加；running 是当前 le 上界的累计值
    std::vector<std::uint64_t> result(m_bucket_count, 0);
    std::uint64_t running = 0;
    for (std::size_t i = 0; i < m_bucket_count; ++i) {
        running += m_bucket_counts[i].load(std::memory_order_relaxed);
        result[i] = running;
    }
    return result;
}

/**
 * @brief 估算 q 分位数（Prometheus 桶内线性插值）。
 *
 * ## 步骤
 * step1. 参数校验：NaN -> NaN；q<0 -> -Inf；q>1 -> +Inf；空直方图 -> NaN；
 * step2. 取累计桶计数，算目标 rank = q * total；
 * step3. 找第一个累计计数 >= rank 的桶；落 `+Inf` 桶时返回最后一个有限上界；
 * step4. 桶内线性插值：`lower + (rank - prior) / bucket_count * (upper - lower)`。
 *
 * ## 边界
 * 第一个桶的下界按 Prometheus 习惯取 `min(0, upper)`；空桶返回下界，避免除零。
 */
double Histogram::Quantile(double q) const {
    EnsureSeries("Quantile");
    if (std::isnan(q)) {
        return kNaN;
    }
    if (q < 0.0) {
        return kNegInf;
    }
    if (q > 1.0) {
        return kPosInf;
    }

    const std::uint64_t total = m_count.load(std::memory_order_relaxed);
    if (total == 0) {
        return kNaN;
    }

    /// step2: 取一次快照，保证后续计算自洽。
    const auto cumulative = CumulativeBucketCounts();
    const double rank = q * static_cast<double>(total);
    std::size_t index = 0;
    while (index < m_bucket_count &&
           static_cast<double>(cumulative[index]) < rank) {
        ++index;
    }
    if (index >= m_bucket_count) {
        index = m_bucket_count - 1;
    }

    // 目标落在隐式 +Inf 桶：Prometheus 的经典行为是返回最后一个有限上界。
    /// step3: 落在隐式 +Inf 桶。
    if (index == m_bucket_count - 1) {
        return m_family->boundaries.back();
    }

    const double upper = m_family->boundaries[index];
    // Prometheus 假设第一个桶的下界是 0；若桶上界本身为负，则退化为该上界，
    // 避免出现 lower > upper 的负宽度插值。
    /// step4: 第一个桶下界取 min(0, upper)，避免负宽度。
    const double lower =
        index == 0 ? std::min(0.0, upper) : m_family->boundaries[index - 1];
    const std::uint64_t prior = index == 0 ? 0 : cumulative[index - 1];
    const std::uint64_t bucket_count = cumulative[index] - prior;
    if (bucket_count == 0) {
        return lower;
    }
    const double fraction =
        (rank - static_cast<double>(prior)) /
        static_cast<double>(bucket_count);
    return lower + fraction * (upper - lower);
}

Histogram& Histogram::With(
    std::initializer_list<std::string_view> labels) {
    std::vector<std::string> values;
    values.reserve(labels.size());
    for (const auto label : labels) {
        values.emplace_back(label);
    }
    return With(values);
}

Histogram& Histogram::With(const std::vector<std::string>& labels) {
    ValidateLabelValues(labels);
    if (!m_is_root) {
        if (labels != m_label_values) {
            throw std::invalid_argument(
                "go2cpp::metrics::Histogram::With: child metric only accepts "
                "its own label values");
        }
        return *this;
    }

    const auto& names = m_family->info.label_names;
    if (labels.size() != names.size()) {
        throw std::invalid_argument(
            "go2cpp::metrics::Histogram::With: expected " +
            std::to_string(names.size()) + " label values, got " +
            std::to_string(labels.size()));
    }
    if (names.empty()) {
        return *this;
    }

    const std::string key = EncodeLabelValues(labels);
    std::lock_guard<std::mutex> lock(m_family->mutex);
    const auto found = m_family->children.find(key);
    if (found != m_family->children.end()) {
        return *found->second;
    }
    auto child = std::unique_ptr<Histogram>(new Histogram(m_family, labels));
    Histogram* raw = child.get();
    m_family->children.emplace(key, std::move(child));
    return *raw;
}

std::size_t Histogram::SeriesCount() const {
    if (!m_is_root) {
        return 1;
    }
    if (m_family->info.label_names.empty()) {
        return 1;
    }
    std::lock_guard<std::mutex> lock(m_family->mutex);
    return m_family->children.size();
}

/**
 * @brief 把一个 Histogram 族展开成 `_bucket`（累计）/`_sum`/`_count` 序列。
 *
 * ## 步骤
 * step1. 输出 HELP/TYPE 头；
 * step2. 定义 `append_series`：累计桶 + 有限上界各一行、`+Inf` 一行、sum/count 各一行；
 * step3. 子序列：只输出自己；
 * step4. 无标签根对象：输出自己；
 * step5. 带标签根对象：持族锁遍历 children，逐个输出。
 *
 * ## 为什么桶是累计的
 * Prometheus 的 `le` 桶语义就是「小于等于上界」，文本里直接输出累计计数，
 * 服务端不需要再做前缀和。
 */
void Histogram::AppendPrometheus(std::string& out) const {
    AppendHeader(out, m_family->info);

    // step2: 一个序列 = 有限桶 + 隐式 +Inf 桶 + sum + count
    const auto append_series = [&out, this](const Histogram& series) {
        const auto cumulative = series.CumulativeBucketCounts();
        const std::string bucket_name = m_family->info.name + "_bucket";
        for (std::size_t i = 0; i < cumulative.size() - 1; ++i) {
            AppendSample(out, bucket_name, m_family->info.label_names,
                         series.m_label_values, FormatUint(cumulative[i]),
                         "le", FormatDouble(m_family->boundaries[i]));
        }
        AppendSample(out, bucket_name, m_family->info.label_names,
                     series.m_label_values, FormatUint(cumulative.back()),
                     "le", "+Inf");
        AppendSample(out, m_family->info.name + "_sum",
                     m_family->info.label_names, series.m_label_values,
                     FormatDouble(series.Sum()));
        AppendSample(out, m_family->info.name + "_count",
                     m_family->info.label_names, series.m_label_values,
                     FormatUint(series.Count()));
    };

    // step3: 子序列只导出自己，不遍历兄弟
    if (!m_is_root) {
        append_series(*this);
        return;
    }
    // step4: 无标签根对象就是唯一序列
    if (m_family->info.label_names.empty()) {
        append_series(*this);
        return;
    }
    std::lock_guard<std::mutex> lock(m_family->mutex);
    // step5: 带标签根对象：children 按编码键有序，输出稳定
    for (const auto& entry : m_family->children) {
        append_series(*entry.second);
    }
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

Registry::Registry() = default;
Registry::~Registry() = default;

Registry& Registry::Default() {
    static Registry registry;
    return registry;
}

/**
 * @brief 注册或取得一个指标族，并校验同名参数一致性。
 *
 * ## 步骤
 * step1. 校验名字/help/标签名；Histogram 额外补默认桶并校验边界；
 * step2. 取注册表锁（查重与插入必须原子）；
 * step3. 已存在：逐项比较类型/help/标签名/桶，任一不同抛 `invalid_argument`；
 * step4. 不存在：按类型构造 Family 与具体指标对象；
 * step5. 入表并返回稳定引用（对象由 `unique_ptr` 持有，生命周期同注册表）。
 *
 * ## 为什么冲突要抛异常
 * 静默复用会让两个调用方写进同一条序列，指标含义被污染且难以发现；启动期
 * 抛异常比运行期给出错误数字更安全。
 */
Metric& Registry::RegisterImpl(const std::string& name,
                               const std::string& help, MetricType type,
                               std::vector<std::string> label_names,
                               std::vector<double> buckets) {
    // step1: 先校验元数据；Histogram 的空 buckets 在这里补默认值
    const detail::MetricInfo info =
        MakeInfo(name, help, type, std::move(label_names), true);
    // step1b: 桶必须严格递增且有限，否则 Observe 的二分没有意义
    if (type == MetricType::kHistogram) {
        if (buckets.empty()) {
            buckets = DefaultHistogramBuckets();
        }
        ValidateBuckets(buckets);
    }

    // step2: 注册表锁覆盖查重与插入，避免同名并发注册产生两个对象
    std::lock_guard<std::mutex> lock(m_mutex);
    // step3: 同名必须完全同构；任何差异都说明调用方对指标语义理解不一致
    const auto found = m_metrics.find(name);
    if (found != m_metrics.end()) {
        Metric& existing = *found->second;
        if (existing.Type() != type) {
            throw std::invalid_argument(
                "go2cpp::metrics: metric \"" + name +
                "\" already registered with a different type");
        }
        if (existing.Help() != help) {
            throw std::invalid_argument(
                "go2cpp::metrics: metric \"" + name +
                "\" already registered with different help text");
        }
        if (existing.LabelNames() != info.label_names) {
            throw std::invalid_argument(
                "go2cpp::metrics: metric \"" + name +
                "\" already registered with different label names");
        }
        if (type == MetricType::kHistogram) {
            const auto* histogram = static_cast<const Histogram*>(&existing);
            if (histogram->Buckets() != buckets) {
                throw std::invalid_argument(
                    "go2cpp::metrics: histogram \"" + name +
                    "\" already registered with different buckets");
            }
        }
        return existing;
    }

    // step4: 按类型构造；Family 由根指标独占，子序列通过裸指针共享
    std::unique_ptr<Metric> metric;
    switch (type) {
        case MetricType::kCounter:
            metric = std::unique_ptr<Metric>(
                new Counter(Counter::CreateFamily(info)));
            break;
        case MetricType::kGauge:
            metric = std::unique_ptr<Metric>(
                new Gauge(Gauge::CreateFamily(info)));
            break;
        case MetricType::kHistogram:
            metric = std::unique_ptr<Metric>(
                new Histogram(Histogram::CreateFamily(info, buckets)));
            break;
        default:
            throw std::invalid_argument(
                "go2cpp::metrics: invalid metric type");
    }
    Metric* raw = metric.get();
    // step5: 入表后指针稳定；返回引用在注册表生命周期内有效
    m_metrics.emplace(name, std::move(metric));
    return *raw;
}

Counter& Registry::GetCounter(const std::string& name,
                              const std::string& help,
                              std::vector<std::string> label_names) {
    return static_cast<Counter&>(RegisterImpl(
        name, help, MetricType::kCounter, std::move(label_names), {}));
}

Gauge& Registry::GetGauge(const std::string& name, const std::string& help,
                          std::vector<std::string> label_names) {
    return static_cast<Gauge&>(RegisterImpl(
        name, help, MetricType::kGauge, std::move(label_names), {}));
}

Histogram& Registry::GetHistogram(const std::string& name,
                                  const std::string& help,
                                  std::vector<std::string> label_names,
                                  std::vector<double> buckets) {
    return static_cast<Histogram&>(RegisterImpl(
        name, help, MetricType::kHistogram, std::move(label_names),
        std::move(buckets)));
}

Metric& Registry::Register(const std::string& name, const std::string& help,
                           MetricType type,
                           std::vector<std::string> label_names) {
    return RegisterImpl(name, help, type, std::move(label_names), {});
}

/**
 * @brief 渲染全部指标为 Prometheus 文本格式。
 *
 * ## 步骤
 * step1. 取 `m_mutex`（保证注册表快照一致）；
 * step2. 预分配输出，逐个调用 `AppendPrometheus()`（各自写 HELP/TYPE 与序列）；
 * step3. 返回整段文本。
 *
 * ## 边界
 * 持锁时间与指标数量成正比；抓取频率高、指标多时应避免频繁 `Render()`。
 */
std::string Registry::Render() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string out;
    out.reserve(m_metrics.size() * 256);
    /// step2: 指标按名字有序（std::map），输出稳定。
    for (const auto& entry : m_metrics) {
        entry.second->AppendPrometheus(out);
    }
    return out;
}

std::size_t Registry::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metrics.size();
}

}  // namespace metrics
}  // namespace go2cpp
