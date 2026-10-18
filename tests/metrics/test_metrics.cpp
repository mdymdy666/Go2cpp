/**
 * @file test_metrics.cpp
 * @brief `go2cpp::metrics` 的压测 / 随机测试 / 真实场景测试。
 *
 * 覆盖策略（PROJECT_RULES 第 3 节）：
 *   - **压测**：8 线程 × 1000 万次 `Counter::Inc()`，报告 Mops/s，并断言
 *     终值精确等于 80,000,000。另含 Gauge 并发增减、Histogram 并发观察、
 *     8 个 Fiber 并发计数、并发注册表访问。
 *   - **随机测试**：随机指标名/标签组合/特殊标签值，验证标签缓存与隔离；
 *     随机观察值（负数、1e300、+/-Inf、NaN）逐桶核对累计计数与 Sum。
 *   - **分位数正确性**：对 1..1000 的已知数据核对 P50/P95/P99，误差必须
 *     不超过一个桶宽；另测空直方图、+Inf 桶、非法 q。
 *   - **真实场景 5 个**：HTTP 请求计数、延迟直方图、活跃连接数、错误率、
 *     Prometheus 文本输出（内置严格解析器 + 可导出给标准解析器）。
 *
 * 运行：
 *   ctest --test-dir build -R go2cpp_metrics_test --output-on-failure
 *   ./build/go2cpp_metrics_test --dump-prometheus=/tmp/go2cpp_metrics.prom
 */
#include "go2cpp/metrics/metrics.hpp"

#include "go2cpp/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using go2cpp::metrics::Counter;
using go2cpp::metrics::Gauge;
using go2cpp::metrics::Histogram;
using go2cpp::metrics::Metric;
using go2cpp::metrics::MetricType;
using go2cpp::metrics::Registry;

int g_checks = 0;
int g_failures = 0;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

void CheckImpl(bool condition, const char* expression, const char* file,
               int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "[metrics] FAILED %s:%d: %s\n", file, line,
                     expression);
    }
}

void CheckNearImpl(double actual, double expected, double tolerance,
                   const char* expression, const char* file, int line) {
    ++g_checks;
    const bool ok = std::fabs(actual - expected) <= tolerance;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr,
                     "[metrics] FAILED %s:%d: %s (actual=%.17g expected=%.17g "
                     "tolerance=%.17g)\n",
                     file, line, expression, actual, expected, tolerance);
    }
}

template <typename Func>
void CheckThrowsImpl(const char* expression, Func&& func, const char* file,
                     int line) {
    ++g_checks;
    bool threw = false;
    try {
        std::forward<Func>(func)();
    } catch (const std::exception&) {
        threw = true;
    } catch (...) {
        threw = true;
    }
    if (!threw) {
        ++g_failures;
        std::fprintf(stderr, "[metrics] FAILED %s:%d: expected throw: %s\n",
                     file, line, expression);
    }
}

#define CHECK(expression) \
    CheckImpl(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define CHECK_NEAR(actual, expected, tolerance) \
    CheckNearImpl((actual), (expected), (tolerance), \
                  #actual " ~= " #expected, __FILE__, __LINE__)

#define CHECK_THROWS(...) \
    CheckThrowsImpl(#__VA_ARGS__, [&] { __VA_ARGS__; }, __FILE__, __LINE__)

void Announce(const char* name) {
    std::printf("[metrics] %s\n", name);
    std::fflush(stdout);
}

void AnnounceScenario(int index, const char* name) {
    std::printf("[metrics][scenario %d] %s\n", index, name);
    std::fflush(stdout);
}

double ExactQuantile(std::vector<double> values, double q) {
    if (values.empty()) {
        return kNaN;
    }
    std::sort(values.begin(), values.end());
    if (q <= 0.0) {
        return values.front();
    }
    if (q >= 1.0) {
        return values.back();
    }
    const double position = q * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper) {
        return values[lower];
    }
    const double fraction = position - static_cast<double>(lower);
    return values[lower] + fraction * (values[upper] - values[lower]);
}

std::vector<double> MakeBuckets(double first, double last, double step) {
    std::vector<double> buckets;
    for (double value = first; value <= last + step * 0.5; value += step) {
        buckets.push_back(value);
    }
    return buckets;
}

// ---------------------------------------------------------------------------
// Prometheus 文本解析器（测试用，严格但只覆盖本模块会输出的语法）
// ---------------------------------------------------------------------------

struct ParsedSample {
    std::string name;
    std::map<std::string, std::string> labels;
    double value{0.0};
};

struct ParsedMetrics {
    std::map<std::string, std::string> help;
    std::map<std::string, std::string> types;
    std::map<std::string, ParsedSample> samples;
    int sample_count{0};
};

void SkipSpaces(std::string_view text, std::size_t* cursor) {
    while (*cursor < text.size() &&
           (text[*cursor] == ' ' || text[*cursor] == '\t')) {
        ++*cursor;
    }
}

bool ParseLabelValue(std::string_view text, std::size_t* cursor,
                     std::string* value) {
    if (*cursor >= text.size() || text[*cursor] != '"') {
        return false;
    }
    ++*cursor;
    value->clear();
    while (*cursor < text.size()) {
        const char ch = text[*cursor];
        ++*cursor;
        if (ch == '"') {
            return true;
        }
        if (ch != '\\') {
            value->push_back(ch);
            continue;
        }
        if (*cursor >= text.size()) {
            return false;
        }
        const char escaped = text[*cursor];
        ++*cursor;
        switch (escaped) {
            case '\\':
                value->push_back('\\');
                break;
            case '"':
                value->push_back('"');
                break;
            case 'n':
                value->push_back('\n');
                break;
            default:
                return false;
        }
    }
    return false;
}

std::string SampleKey(const std::string& name,
                      const std::map<std::string, std::string>& labels) {
    std::string key = name;
    key.push_back('{');
    for (const auto& label : labels) {
        key.append(label.first);
        key.push_back('=');
        key.append(label.second);
        key.push_back(',');
    }
    key.push_back('}');
    return key;
}

bool ParsePrometheus(std::string_view text, ParsedMetrics* parsed,
                     std::string* error) {
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        const std::size_t line_end = text.find('\n', line_start);
        const std::string_view line =
            text.substr(line_start, line_end == std::string_view::npos
                                        ? std::string_view::npos
                                        : line_end - line_start);
        line_start = line_end == std::string_view::npos ? text.size() + 1
                                                        : line_end + 1;
        if (line.empty()) {
            continue;
        }
        if (line.rfind("# HELP ", 0) == 0) {
            const auto name_end = line.find(' ', 7);
            if (name_end == std::string_view::npos) {
                *error = "malformed HELP line";
                return false;
            }
            const std::string name(line.substr(7, name_end - 7));
            if (parsed->help.count(name) != 0) {
                *error = "duplicate HELP for " + name;
                return false;
            }
            parsed->help[name] = std::string(line.substr(name_end + 1));
            continue;
        }
        if (line.rfind("# TYPE ", 0) == 0) {
            const auto name_end = line.find(' ', 7);
            if (name_end == std::string_view::npos) {
                *error = "malformed TYPE line";
                return false;
            }
            const std::string name(line.substr(7, name_end - 7));
            const std::string type(line.substr(name_end + 1));
            if (parsed->types.count(name) != 0) {
                *error = "duplicate TYPE for " + name;
                return false;
            }
            parsed->types[name] = type;
            continue;
        }
        if (line.front() == '#') {
            continue;
        }

        std::size_t cursor = 0;
        const auto name_end = line.find_first_of(" {", cursor);
        if (name_end == std::string_view::npos || name_end == cursor) {
            *error = "malformed sample name";
            return false;
        }
        ParsedSample sample;
        sample.name = std::string(line.substr(0, name_end));
        cursor = name_end;
        SkipSpaces(line, &cursor);
        if (cursor < line.size() && line[cursor] == '{') {
            ++cursor;
            for (;;) {
                SkipSpaces(line, &cursor);
                if (cursor < line.size() && line[cursor] == '}') {
                    ++cursor;
                    break;
                }
                const auto label_name_end = line.find('=', cursor);
                if (label_name_end == std::string_view::npos) {
                    *error = "malformed label";
                    return false;
                }
                const std::string label_name(
                    line.substr(cursor, label_name_end - cursor));
                cursor = label_name_end + 1;
                std::string label_value;
                if (!ParseLabelValue(line, &cursor, &label_value)) {
                    *error = "malformed label value";
                    return false;
                }
                if (!sample.labels.emplace(label_name, label_value).second) {
                    *error = "duplicate label " + label_name;
                    return false;
                }
                SkipSpaces(line, &cursor);
                if (cursor < line.size() && line[cursor] == ',') {
                    ++cursor;
                    continue;
                }
                if (cursor < line.size() && line[cursor] == '}') {
                    ++cursor;
                    break;
                }
                *error = "expected ',' or '}'";
                return false;
            }
        }
        SkipSpaces(line, &cursor);
        const auto value_start = cursor;
        while (cursor < line.size() && line[cursor] != ' ' &&
               line[cursor] != '\t') {
            ++cursor;
        }
        if (cursor == value_start) {
            *error = "missing sample value";
            return false;
        }
        const std::string value_text(line.substr(value_start,
                                                 cursor - value_start));
        char* end = nullptr;
        sample.value = std::strtod(value_text.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            *error = "invalid sample value: " + value_text;
            return false;
        }
        SkipSpaces(line, &cursor);
        if (cursor != line.size()) {
            *error = "trailing characters after sample";
            return false;
        }
        const std::string key = SampleKey(sample.name, sample.labels);
        if (!parsed->samples.emplace(key, sample).second) {
            *error = "duplicate sample: " + key;
            return false;
        }
        ++parsed->sample_count;
    }
    return true;
}

const ParsedSample* FindSample(
    const ParsedMetrics& parsed, const std::string& name,
    std::initializer_list<std::pair<std::string, std::string>> labels) {
    std::map<std::string, std::string> label_map(labels.begin(), labels.end());
    const auto found = parsed.samples.find(SampleKey(name, label_map));
    return found == parsed.samples.end() ? nullptr : &found->second;
}

int CountSamplesWithPrefix(const ParsedMetrics& parsed,
                           const std::string& prefix) {
    int count = 0;
    for (const auto& entry : parsed.samples) {
        if (entry.second.name.rfind(prefix, 0) == 0) {
            ++count;
        }
    }
    return count;
}

bool ValidateHistogramFamily(const ParsedMetrics& parsed,
                             const std::string& base_name,
                             std::string* error) {
    struct BucketGroup {
        double upper{0.0};
        double cumulative{0.0};
    };
    std::map<std::string, std::vector<BucketGroup>> groups;
    std::map<std::string, std::map<std::string, std::string>> group_labels;

    for (const auto& entry : parsed.samples) {
        const ParsedSample& sample = entry.second;
        if (sample.name != base_name + "_bucket") {
            continue;
        }
        const auto le = sample.labels.find("le");
        if (le == sample.labels.end()) {
            *error = "histogram bucket without le label";
            return false;
        }
        char* end = nullptr;
        const double upper = std::strtod(le->second.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            *error = "invalid le value: " + le->second;
            return false;
        }
        std::map<std::string, std::string> labels = sample.labels;
        labels.erase("le");
        const std::string key = SampleKey("", labels);
        groups[key].push_back(BucketGroup{upper, sample.value});
        group_labels[key] = std::move(labels);
    }
    if (groups.empty()) {
        *error = "histogram has no bucket samples";
        return false;
    }
    for (auto& group : groups) {
        auto& buckets = group.second;
        std::sort(buckets.begin(), buckets.end(),
                  [](const BucketGroup& left, const BucketGroup& right) {
                      return left.upper < right.upper;
                  });
        double previous = 0.0;
        for (const auto& bucket : buckets) {
            if (bucket.cumulative < previous) {
                *error = "histogram cumulative buckets are not monotonic";
                return false;
            }
            previous = bucket.cumulative;
        }
        if (!std::isinf(buckets.back().upper) || buckets.back().upper < 0.0) {
            *error = "histogram missing +Inf bucket";
            return false;
        }
        std::map<std::string, std::string> label_map = group_labels[group.first];
        const auto count_it =
            parsed.samples.find(SampleKey(base_name + "_count", label_map));
        if (count_it == parsed.samples.end()) {
            *error = "histogram missing _count sample";
            return false;
        }
        if (count_it->second.value != buckets.back().cumulative) {
            *error = "histogram +Inf bucket != _count";
            return false;
        }
        const auto sum_it =
            parsed.samples.find(SampleKey(base_name + "_sum", label_map));
        if (sum_it == parsed.samples.end()) {
            *error = "histogram missing _sum sample";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 基础语义
// ---------------------------------------------------------------------------

void TestCounterBasics() {
    Announce("counter basics + label cache");
    Counter counter;
    CHECK(counter.Value() == 0);
    CHECK(counter.SeriesCount() == 1);
    counter.Inc();
    counter.Inc();
    counter.Add(40);
    CHECK(counter.Value() == 42);
    CHECK(counter.Name().empty());

    Counter labeled({"method", "path"});
    CHECK(labeled.Type() == MetricType::kCounter);
    CHECK(labeled.LabelNames().size() == 2);
    CHECK(labeled.SeriesCount() == 0);
    auto& get_user = labeled.With({"GET", "/users"});
    auto& get_user_again = labeled.With({"GET", "/users"});
    CHECK(&get_user == &get_user_again);
    CHECK(labeled.SeriesCount() == 1);
    get_user.Inc();
    CHECK(get_user.Value() == 1);
    CHECK(get_user_again.Value() == 1);

    auto& get_order = labeled.With({"GET", "/orders"});
    CHECK(&get_order != &get_user);
    get_order.Add(7);
    CHECK(get_order.Value() == 7);
    CHECK(labeled.SeriesCount() == 2);

    CHECK_THROWS(labeled.Inc());
    CHECK_THROWS(labeled.Value());
    CHECK_THROWS(labeled.With({"GET"}));
    CHECK_THROWS(labeled.With({"GET", "/users", "extra"}));
    CHECK_THROWS(get_user.With({"POST", "/users"}));
}

void TestGaugeBasics() {
    Announce("gauge basics + NaN safety");
    Gauge gauge;
    CHECK(gauge.Value() == 0.0);
    gauge.Set(3.5);
    gauge.Inc();
    gauge.Dec();
    gauge.Add(-1.25);
    CHECK_NEAR(gauge.Value(), 2.25, 1e-12);

    gauge.Set(kNaN);
    CHECK(std::isnan(gauge.Value()));
    gauge.Inc();
    CHECK(std::isnan(gauge.Value()));
    gauge.Set(kInf);
    gauge.Inc();
    CHECK(std::isinf(gauge.Value()) && gauge.Value() > 0.0);
    gauge.Set(-kInf);
    gauge.Dec();
    CHECK(std::isinf(gauge.Value()) && gauge.Value() < 0.0);
    gauge.Set(-0.0);
    CHECK(gauge.Value() == 0.0);

    Gauge labeled({"state"});
    auto& idle = labeled.With({"idle"});
    idle.Set(10.0);
    idle.Dec();
    CHECK_NEAR(idle.Value(), 9.0, 1e-12);
    CHECK(labeled.SeriesCount() == 1);
    CHECK_THROWS(labeled.Set(1.0));
    CHECK_THROWS(labeled.Value());
}

void TestHistogramBasics() {
    Announce("histogram basics + NaN/negative/infinite observations");
    Histogram empty;
    CHECK(empty.Count() == 0);
    CHECK(std::isnan(empty.Quantile(0.5)));
    CHECK(empty.Buckets().size() == 22);

    Histogram histogram({1.0, 2.0, 5.0, 10.0});
    histogram.Observe(0.5);
    histogram.Observe(2.0);
    histogram.Observe(5.0);
    histogram.Observe(kNaN);
    histogram.Observe(100.0);
    CHECK(histogram.Count() == 4);
    CHECK_NEAR(histogram.Sum(), 107.5, 1e-12);
    auto cumulative = histogram.CumulativeBucketCounts();
    CHECK(cumulative.size() == 5);
    CHECK(cumulative[0] == 1);  // 0.5
    CHECK(cumulative[1] == 2);  // 2.0
    CHECK(cumulative[2] == 3);  // 5.0
    CHECK(cumulative[3] == 3);
    CHECK(cumulative[4] == 4);  // 100.0 + 隐式 +Inf

    Histogram negative({0.0, 10.0});
    negative.Observe(-5.0);
    negative.Observe(-kInf);
    negative.Observe(5.0);
    negative.Observe(1e300);
    negative.Observe(kInf);
    negative.Observe(kNaN);
    CHECK(negative.Count() == 5);
    auto negative_buckets = negative.CumulativeBucketCounts();
    CHECK(negative_buckets[0] == 2);  // -5, -Inf
    CHECK(negative_buckets[1] == 3);  // 5
    CHECK(negative_buckets[2] == 5);  // 1e300, +Inf
    CHECK(negative_buckets.back() == negative.Count());

    Histogram huge({1.0, 2.0});
    huge.Observe(1e300);
    huge.Observe(kNaN);
    CHECK(huge.Count() == 1);
    CHECK_NEAR(huge.Sum(), 1e300, 1e288);
    CHECK(huge.CumulativeBucketCounts().back() == 1);

    Histogram hi_only({10.0, 100.0});
    hi_only.Observe(1000.0);
    CHECK_NEAR(hi_only.Quantile(0.5), 100.0, 1e-12);

    CHECK(std::isnan(Histogram({1.0}).Quantile(kNaN)));
    CHECK(std::isinf(Histogram({1.0}).Quantile(-0.1)));
    CHECK(Histogram({1.0}).Quantile(-0.1) < 0.0);
    CHECK(std::isinf(Histogram({1.0}).Quantile(1.1)));
    CHECK(Histogram({1.0}).Quantile(1.1) > 0.0);

    CHECK_THROWS(Histogram(std::vector<double>{}));
    CHECK_THROWS(Histogram({1.0, 1.0}));
    CHECK_THROWS(Histogram({2.0, 1.0}));
    CHECK_THROWS(Histogram({1.0, kInf}));
    CHECK_THROWS(Histogram({1.0, kNaN}));
    CHECK_THROWS(Histogram({1.0, 2.0}, {"le"}));
    CHECK_THROWS(Histogram({1.0, 2.0}, {"__reserved"}));
}

void TestQuantileAccuracy() {
    Announce("quantiles against known 1..1000 data");
    std::vector<double> data;
    for (int i = 1; i <= 1000; ++i) {
        data.push_back(static_cast<double>(i));
    }

    Histogram histogram(MakeBuckets(10.0, 1000.0, 10.0));
    for (double value : data) {
        histogram.Observe(value);
    }
    CHECK(histogram.Count() == 1000);
    CHECK_NEAR(histogram.Sum(), 500500.0, 1e-9);

    const double p50 = histogram.Quantile(0.50);
    const double p95 = histogram.Quantile(0.95);
    const double p99 = histogram.Quantile(0.99);
    const double exact50 = ExactQuantile(data, 0.50);
    const double exact95 = ExactQuantile(data, 0.95);
    const double exact99 = ExactQuantile(data, 0.99);
    std::printf(
        "[metrics][quantile] bucket_width=10 p50=%.3f exact=%.3f err=%.3f | "
        "p95=%.3f exact=%.3f err=%.3f | p99=%.3f exact=%.3f err=%.3f\n",
        p50, exact50, std::fabs(p50 - exact50), p95, exact95,
        std::fabs(p95 - exact95), p99, exact99, std::fabs(p99 - exact99));
    CHECK_NEAR(p50, exact50, 10.0);
    CHECK_NEAR(p95, exact95, 10.0);
    CHECK_NEAR(p99, exact99, 10.0);

    Histogram unit(MakeBuckets(1.0, 1000.0, 1.0));
    for (double value : data) {
        unit.Observe(value);
    }
    CHECK_NEAR(unit.Quantile(0.50), exact50, 1.0);
    CHECK_NEAR(unit.Quantile(0.95), exact95, 1.0);
    CHECK_NEAR(unit.Quantile(0.99), exact99, 1.0);

    // 默认桶向上扩到 10000：不传桶时 1..1000 也不应全部落进 +Inf。
    Histogram defaults;
    for (double value : data) {
        defaults.Observe(value);
    }
    CHECK(defaults.CumulativeBucketCounts().back() == 1000);
    CHECK_NEAR(defaults.Quantile(0.50), exact50, 500.0);
    CHECK_NEAR(defaults.Quantile(0.95), exact95, 500.0);
    CHECK_NEAR(defaults.Quantile(0.99), exact99, 500.0);

    const auto cumulative = histogram.CumulativeBucketCounts();
    CHECK(cumulative.size() == 101);
    for (std::size_t i = 1; i < cumulative.size(); ++i) {
        CHECK(cumulative[i] >= cumulative[i - 1]);
    }
    CHECK(cumulative.back() == 1000);
}

void TestRegistryBasics() {
    Announce("registry metadata, conflicts, escaping and Register()");
    Registry registry;
    auto& requests = registry.GetCounter(
        "http_requests_total", "HTTP requests", {"method", "path"});
    requests.With({"GET", "/api"}).Inc();
    requests.With({"GET", "/api"}).Inc();
    requests.With({"POST", "/api"}).Inc();
    CHECK(requests.With({"GET", "/api"}).Value() == 2);
    CHECK(requests.SeriesCount() == 2);
    CHECK(&registry.GetCounter("http_requests_total", "HTTP requests",
                               {"method", "path"}) == &requests);

    auto& active = registry.GetGauge("active_connections", "Active");
    active.Set(3.0);
    CHECK(active.Value() == 3.0);
    auto& latency = registry.GetHistogram(
        "request_latency_seconds", "Latency", {"method"},
        {0.005, 0.01, 0.05, 0.1, 1.0});
    latency.With({"GET"}).Observe(0.02);
    CHECK(latency.With({"GET"}).Count() == 1);
    CHECK_THROWS(latency.Count());
    CHECK_THROWS(latency.Observe(0.01));

    Metric& generic =
        registry.Register("registered_total", "Registered", MetricType::kCounter);
    CHECK(generic.Type() == MetricType::kCounter);
    CHECK(dynamic_cast<Counter*>(&generic) != nullptr);
    CHECK(generic.Name() == "registered_total");
    Metric& generic_gauge = registry.Register("registered_gauge", "Registered",
                                             MetricType::kGauge);
    CHECK(dynamic_cast<Gauge*>(&generic_gauge) != nullptr);
    Metric& generic_histogram = registry.Register(
        "registered_histogram", "Registered", MetricType::kHistogram);
    CHECK(dynamic_cast<Histogram*>(&generic_histogram) != nullptr);
    if (auto* histogram = dynamic_cast<Histogram*>(&generic_histogram)) {
        CHECK(histogram->Buckets().size() == 22);
    }
    CHECK(registry.Size() == 6);

    CHECK_THROWS(registry.GetCounter("http_requests_total",
                                     "different help", {"method", "path"}));
    CHECK_THROWS(registry.GetGauge("http_requests_total", "HTTP requests",
                                   {"method", "path"}));
    CHECK_THROWS(registry.GetCounter("http_requests_total", "HTTP requests",
                                     {"method"}));
    CHECK_THROWS(registry.GetHistogram("request_latency_seconds", "Latency",
                                       {"method"}, {0.01, 0.1}));
    CHECK_THROWS(registry.Register("bad_type", "bad",
                                   static_cast<MetricType>(99)));

    auto& escaped = registry.GetCounter(
        "escaped_total", "line1\\line2\nline3", {"path"});
    escaped.With({"a\"b\\c\nd"}).Inc();
    const std::string text = registry.Render();
    const auto help_pos = text.find("# HELP escaped_total ");
    CHECK(help_pos != std::string::npos);
    CHECK(text.find("line1\\\\line2\\nline3") != std::string::npos);
    CHECK(text.find("path=\"a\\\"b\\\\c\\nd\"") != std::string::npos);
    CHECK(text.find("# TYPE escaped_total counter\n") != std::string::npos);

    CHECK_THROWS(registry.GetCounter("bad-name", "help"));
    CHECK_THROWS(registry.GetCounter("bad_name", "help", {"bad-label"}));
    CHECK_THROWS(registry.GetCounter("bad_name", "help", {"x", "x"}));
    CHECK_THROWS(registry.GetCounter("bad_name", "help", {"__reserved"}));
    CHECK_THROWS(registry.GetCounter("bad_name", std::string("a\0b", 3)));

    Registry& default_registry = Registry::Default();
    CHECK(&Registry::Default() == &default_registry);
    auto& default_counter = default_registry.GetCounter(
        "metrics_default_registry_total", "default registry");
    default_counter.Inc();
    CHECK(default_counter.Value() == 1);
}

// ---------------------------------------------------------------------------
// 并发压测
// ---------------------------------------------------------------------------

void TestCounterStress() {
    Announce("stress: 8 threads x 10,000,000 Counter::Inc()");
    constexpr int kThreads = 8;
    constexpr std::uint64_t kPerThread = 10'000'000;
    Counter counter;
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&counter, &start] {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (std::uint64_t j = 0; j < kPerThread; ++j) {
                counter.Inc();
            }
        });
    }
    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin);
    const std::uint64_t expected =
        static_cast<std::uint64_t>(kThreads) * kPerThread;
    const double mops = static_cast<double>(expected) / elapsed.count() /
                        1'000'000.0;
    std::printf(
        "[metrics][stress] counter threads=%d per_thread=%llu total=%llu "
        "elapsed=%.3fs throughput=%.2f Mops/s\n",
        kThreads, static_cast<unsigned long long>(kPerThread),
        static_cast<unsigned long long>(counter.Value()), elapsed.count(),
        mops);
    CHECK(counter.Value() == expected);
}

void TestGaugeHistogramStress() {
    Announce("stress: Gauge inc/dec + Histogram concurrent observations");
    constexpr int kThreads = 8;
    constexpr int kIterations = 200'000;
    Gauge gauge;
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&gauge, &start] {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int j = 0; j < kIterations; ++j) {
                gauge.Inc();
            }
            for (int j = 0; j < kIterations; ++j) {
                gauge.Dec();
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK_NEAR(gauge.Value(), 0.0, 1e-12);

    Histogram histogram(MakeBuckets(1.0, 1000.0, 10.0));
    std::atomic<bool> histogram_start{false};
    threads.clear();
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&histogram, &histogram_start] {
            while (!histogram_start.load(std::memory_order_acquire)) {
            }
            for (int j = 0; j < kIterations / 2; ++j) {
                histogram.Observe(static_cast<double>((j % 1000) + 1));
            }
        });
    }
    histogram_start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    const std::uint64_t expected_count =
        static_cast<std::uint64_t>(kThreads) * (kIterations / 2);
    CHECK(histogram.Count() == expected_count);
    const double expected_sum =
        static_cast<double>(kThreads) *
        static_cast<double>(kIterations / 2) * 500.5;
    CHECK_NEAR(histogram.Sum(), expected_sum, expected_sum * 1e-12);
    const auto cumulative = histogram.CumulativeBucketCounts();
    CHECK(cumulative.back() == expected_count);
    for (std::size_t i = 1; i < cumulative.size(); ++i) {
        CHECK(cumulative[i] >= cumulative[i - 1]);
    }
}

void TestConcurrentLabels() {
    Announce("stress: concurrent With() cache and unique labels");
    Counter shared({"worker"});
    std::atomic<Counter*> observed{nullptr};
    std::atomic<bool> address_mismatch{false};
    std::atomic<bool> start{false};
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100'000;
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&shared, &observed, &address_mismatch, &start] {
            while (!start.load(std::memory_order_acquire)) {
            }
            Counter& child = shared.With({"shared"});
            Counter* expected = nullptr;
            if (!observed.compare_exchange_strong(expected, &child)) {
                if (expected != &child) {
                    address_mismatch.store(true, std::memory_order_relaxed);
                }
            }
            for (int j = 0; j < kPerThread; ++j) {
                child.Inc();
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(!address_mismatch.load(std::memory_order_relaxed));
    CHECK(shared.SeriesCount() == 1);
    CHECK(shared.With({"shared"}).Value() ==
          static_cast<std::uint64_t>(kThreads) * kPerThread);

    Counter unique({"worker"});
    threads.clear();
    start.store(false, std::memory_order_release);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&unique, &start, i] {
            while (!start.load(std::memory_order_acquire)) {
            }
            Counter& child = unique.With({"worker-" + std::to_string(i)});
            for (int j = 0; j < 50'000; ++j) {
                child.Inc();
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(unique.SeriesCount() == static_cast<std::size_t>(kThreads));
    std::uint64_t total = 0;
    for (int i = 0; i < kThreads; ++i) {
        const std::uint64_t value =
            unique.With({"worker-" + std::to_string(i)}).Value();
        CHECK(value == 50'000);
        total += value;
    }
    CHECK(total == static_cast<std::uint64_t>(kThreads) * 50'000);

    Registry registry;
    std::atomic<Counter*> registry_first{nullptr};
    std::atomic<bool> registry_mismatch{false};
    std::atomic<bool> registry_start{false};
    threads.clear();
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&registry, &registry_first, &registry_mismatch,
                              &registry_start] {
            while (!registry_start.load(std::memory_order_acquire)) {
            }
            Counter& counter = registry.GetCounter(
                "concurrent_registry_total", "concurrent");
            Counter* expected = nullptr;
            if (!registry_first.compare_exchange_strong(expected, &counter)) {
                if (expected != &counter) {
                    registry_mismatch.store(true, std::memory_order_relaxed);
                }
            }
            for (int j = 0; j < 100'000; ++j) {
                counter.Inc();
            }
        });
    }
    registry_start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(!registry_mismatch.load(std::memory_order_relaxed));
    CHECK(registry.Size() == 1);
    CHECK(registry.GetCounter("concurrent_registry_total", "concurrent")
              .Value() == static_cast<std::uint64_t>(kThreads) * 100'000);
}

void TestFiberStress() {
    Announce("stress: 8 fibers x 500,000 Counter::Inc()");
    constexpr int kFibers = 8;
    constexpr int kPerFiber = 500'000;
    Counter counter;
    go2cpp::Scheduler scheduler(kFibers);
    scheduler.start();
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int i = 0; i < kFibers; ++i) {
        tasks.push_back(go2cpp::go(scheduler, [&counter] {
            for (int j = 0; j < kPerFiber; ++j) {
                counter.Inc();
            }
        }));
    }
    for (auto& task : tasks) {
        task->wait();
    }
    scheduler.shutdown();
    CHECK(counter.Value() ==
          static_cast<std::uint64_t>(kFibers) * kPerFiber);

    Counter labeled({"fiber"});
    go2cpp::Scheduler label_scheduler(kFibers);
    label_scheduler.start();
    std::vector<std::shared_ptr<go2cpp::Task>> label_tasks;
    for (int i = 0; i < kFibers; ++i) {
        label_tasks.push_back(go2cpp::go(label_scheduler, [&labeled] {
            Counter& child = labeled.With({"shared"});
            for (int j = 0; j < 50'000; ++j) {
                child.Inc();
            }
        }));
    }
    for (auto& task : label_tasks) {
        task->wait();
    }
    label_scheduler.shutdown();
    CHECK(labeled.SeriesCount() == 1);
    CHECK(labeled.With({"shared"}).Value() ==
          static_cast<std::uint64_t>(kFibers) * 50'000);
}

// ---------------------------------------------------------------------------
// 随机测试
// ---------------------------------------------------------------------------

void TestRandomLabels() {
    Announce("random: metric names + label combinations + cache isolation");
    std::mt19937_64 rng(0x6d657472696373ULL);
    Registry registry;
    std::size_t expected_series = 0;

    for (int family = 0; family < 32; ++family) {
        const int label_count = 1 + static_cast<int>(rng() % 3);
        std::vector<std::string> label_names;
        for (int i = 0; i < label_count; ++i) {
            label_names.push_back("label_" + std::to_string(i));
        }
        const std::string name =
            "random_metric_" + std::to_string(family) + "_total";
        Counter& root = registry.GetCounter(
            name, "random metric " + std::to_string(family), label_names);
        std::set<std::vector<std::string>> unique_values;
        while (unique_values.size() < 16) {
            std::vector<std::string> values;
            for (int i = 0; i < label_count; ++i) {
                const int value = static_cast<int>(rng() % 8);
                std::string text = "v" + std::to_string(value);
                if ((rng() % 7) == 0) {
                    text += "\"\\\n";
                }
                values.push_back(std::move(text));
            }
            unique_values.insert(std::move(values));
        }
        for (const auto& values : unique_values) {
            Counter& first = root.With(values);
            Counter& second = root.With(values);
            CHECK(&first == &second);
            first.Inc();
            CHECK(first.Value() == 1);
        }
        CHECK(root.SeriesCount() == unique_values.size());
        expected_series += unique_values.size();
    }

    const std::string text = registry.Render();
    ParsedMetrics parsed;
    std::string error;
    CHECK(ParsePrometheus(text, &parsed, &error));
    CHECK(error.empty());
    CHECK(parsed.sample_count == static_cast<int>(expected_series));
    CHECK(parsed.types.size() == 32);
    for (const auto& entry : parsed.types) {
        CHECK(entry.second == "counter");
    }
    for (const auto& entry : parsed.samples) {
        CHECK(entry.second.value == 1.0);
    }
}

void TestRandomHistogram() {
    Announce("random: negative / huge / +/-Inf / NaN observations");
    const std::vector<double> buckets = {-100.0, -10.0, 0.0,
                                         10.0,   100.0, 1000.0};
    Histogram histogram(buckets);
    std::vector<std::uint64_t> expected_buckets(buckets.size() + 1, 0);
    std::mt19937_64 rng(0x686973746f677261ULL);
    std::uniform_real_distribution<double> finite(-5000.0, 5000.0);
    long double expected_sum = 0.0L;
    bool has_pos_inf = false;
    bool has_neg_inf = false;
    std::uint64_t expected_count = 0;

    for (int i = 0; i < 200'000; ++i) {
        double value = finite(rng);
        const auto roll = rng() % 1000;
        if (roll == 0) {
            value = kNaN;
        } else if (roll == 1) {
            value = kInf;
        } else if (roll == 2) {
            value = -kInf;
        } else if (roll == 3) {
            value = 1e300;
        } else if (roll == 4) {
            value = -1e300;
        }
        histogram.Observe(value);
        if (std::isnan(value)) {
            continue;
        }
        ++expected_count;
        const auto index = static_cast<std::size_t>(
            std::lower_bound(buckets.begin(), buckets.end(), value) -
            buckets.begin());
        ++expected_buckets[index];
        if (std::isinf(value)) {
            if (value > 0.0) {
                has_pos_inf = true;
            } else {
                has_neg_inf = true;
            }
        } else {
            expected_sum += static_cast<long double>(value);
        }
    }

    std::uint64_t running = 0;
    const auto cumulative = histogram.CumulativeBucketCounts();
    CHECK(cumulative.size() == expected_buckets.size());
    for (std::size_t i = 0; i < expected_buckets.size(); ++i) {
        running += expected_buckets[i];
        CHECK(cumulative[i] == running);
    }
    CHECK(histogram.Count() == expected_count);
    CHECK(cumulative.back() == expected_count);

    if (has_pos_inf && has_neg_inf) {
        CHECK(std::isnan(histogram.Sum()));
    } else if (has_pos_inf) {
        CHECK(std::isinf(histogram.Sum()) && histogram.Sum() > 0.0);
    } else if (has_neg_inf) {
        CHECK(std::isinf(histogram.Sum()) && histogram.Sum() < 0.0);
    } else {
        const double expected = static_cast<double>(expected_sum);
        const double tolerance = std::max(1.0, std::fabs(expected)) * 1e-12;
        CHECK_NEAR(histogram.Sum(), expected, tolerance);
    }

    double previous = -kInf;
    for (int i = 0; i <= 100; ++i) {
        const double q = static_cast<double>(i) / 100.0;
        const double value = histogram.Quantile(q);
        CHECK(!std::isnan(value));
        if (i != 0) {
            CHECK(value >= previous);
        }
        previous = value;
    }
}

// ---------------------------------------------------------------------------
// 真实场景
// ---------------------------------------------------------------------------

void ScenarioHttpRequests() {
    AnnounceScenario(1, "HTTP request counts by method + path");
    Registry registry;
    auto& requests = registry.GetCounter(
        "http_requests_total", "HTTP requests", {"method", "path"});
    const std::vector<std::string> methods = {"GET", "POST", "PUT",
                                              "DELETE"};
    const std::vector<std::string> paths = {"/api/users", "/api/orders",
                                            "/health"};
    constexpr int kRequests = 120000;
    for (int i = 0; i < kRequests; ++i) {
        const auto& method = methods[static_cast<std::size_t>(i) %
                                    methods.size()];
        const auto& path =
            paths[static_cast<std::size_t>(i / 4) % paths.size()];
        requests.With({method, path}).Inc();
    }
    std::uint64_t total = 0;
    for (const auto& method : methods) {
        for (const auto& path : paths) {
            const auto value = requests.With({method, path}).Value();
            CHECK(value == kRequests / 12);
            total += value;
        }
    }
    CHECK(total == kRequests);
    CHECK(requests.SeriesCount() == 12);
    const std::string text = registry.Render();
    ParsedMetrics parsed;
    std::string error;
    CHECK(ParsePrometheus(text, &parsed, &error));
    const auto* sample = FindSample(
        parsed, "http_requests_total", {{"method", "GET"}, {"path", "/api/users"}});
    CHECK(sample != nullptr);
    if (sample != nullptr) {
        CHECK(sample->value == kRequests / 12);
    }
    std::printf("[metrics][scenario 1] requests=%d series=%zu samples=%d\n",
                kRequests, requests.SeriesCount(), parsed.sample_count);
}

void ScenarioLatency() {
    AnnounceScenario(2, "latency histogram: 10k requests, P50/P95/P99");
    Registry registry;
    auto& latency = registry.GetHistogram(
        "http_request_duration_seconds", "HTTP request latency", {"method"},
        MakeBuckets(10.0, 1000.0, 10.0));
    auto& get_latency = latency.With({"GET"});
    std::vector<double> values;
    values.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
        const double value = static_cast<double>((i % 1000) + 1);
        get_latency.Observe(value);
        values.push_back(value);
    }
    const double p50 = get_latency.Quantile(0.50);
    const double p95 = get_latency.Quantile(0.95);
    const double p99 = get_latency.Quantile(0.99);
    const double exact50 = ExactQuantile(values, 0.50);
    const double exact95 = ExactQuantile(values, 0.95);
    const double exact99 = ExactQuantile(values, 0.99);
    CHECK_NEAR(p50, exact50, 10.0);
    CHECK_NEAR(p95, exact95, 10.0);
    CHECK_NEAR(p99, exact99, 10.0);
    CHECK(get_latency.Count() == 10000);
    CHECK_NEAR(get_latency.Sum(), 5005000.0, 1e-6);
    const auto cumulative = get_latency.CumulativeBucketCounts();
    CHECK(cumulative.size() == 101);
    CHECK(cumulative.back() == 10000);
    for (std::size_t i = 1; i < cumulative.size(); ++i) {
        CHECK(cumulative[i] >= cumulative[i - 1]);
    }

    const std::string text = registry.Render();
    ParsedMetrics parsed;
    std::string error;
    CHECK(ParsePrometheus(text, &parsed, &error));
    CHECK(ValidateHistogramFamily(parsed, "http_request_duration_seconds",
                                  &error));
    CHECK(error.empty());
    std::printf(
        "[metrics][scenario 2] count=%llu p50=%.3f (exact %.3f) p95=%.3f "
        "(exact %.3f) p99=%.3f (exact %.3f) bucket_samples=%d\n",
        static_cast<unsigned long long>(get_latency.Count()), p50, exact50,
        p95, exact95, p99, exact99,
        CountSamplesWithPrefix(parsed, "http_request_duration_seconds_bucket"));
}

void ScenarioConnections() {
    AnnounceScenario(3, "active connections gauge inc/dec");
    Gauge active;
    constexpr int kConnections = 16;
    std::atomic<int> ready{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < kConnections; ++i) {
        threads.emplace_back([&active, &ready, &release] {
            active.Inc();
            ready.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
            }
            active.Dec();
        });
    }
    while (ready.load(std::memory_order_acquire) != kConnections) {
    }
    CHECK_NEAR(active.Value(), static_cast<double>(kConnections), 1e-12);
    release.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK_NEAR(active.Value(), 0.0, 1e-12);
    CHECK(active.SeriesCount() == 1);

    Registry registry;
    auto& labeled = registry.GetGauge("active_connections",
                                      "Active connections", {"state"});
    labeled.With({"idle"}).Set(5);
    labeled.With({"busy"}).Inc();
    labeled.With({"busy"}).Inc();
    CHECK_NEAR(labeled.With({"idle"}).Value(), 5.0, 1e-12);
    CHECK_NEAR(labeled.With({"busy"}).Value(), 2.0, 1e-12);
    std::printf("[metrics][scenario 3] peak=%d final=%.0f series=%zu\n",
                kConnections, active.Value(), labeled.SeriesCount());
}

void ScenarioErrorRate() {
    AnnounceScenario(4, "error rate = errors / total");
    Registry registry;
    auto& requests =
        registry.GetCounter("app_requests_total", "App requests", {"status"});
    std::uint64_t errors = 0;
    constexpr std::uint64_t kRequests = 200000;
    for (std::uint64_t i = 0; i < kRequests; ++i) {
        if ((i % 100) < 3) {
            requests.With({"error"}).Inc();
            ++errors;
        } else {
            requests.With({"ok"}).Inc();
        }
    }
    const double error_rate =
        static_cast<double>(requests.With({"error"}).Value()) /
        static_cast<double>(kRequests);
    CHECK_NEAR(error_rate, 0.03, 1e-15);
    CHECK(requests.With({"error"}).Value() == 6000);
    CHECK(requests.With({"ok"}).Value() == kRequests - 6000);
    CHECK(requests.With({"error"}).Value() + requests.With({"ok"}).Value() ==
          kRequests);
    std::printf(
        "[metrics][scenario 4] total=%llu errors=%llu rate=%.4f (expected "
        "0.0300)\n",
        static_cast<unsigned long long>(kRequests),
        static_cast<unsigned long long>(errors), error_rate);
}

void ScenarioPrometheusOutput() {
    AnnounceScenario(5, "Prometheus text output + parser validation");
    Registry registry;
    auto& requests = registry.GetCounter(
        "http_requests_total", "HTTP requests", {"method", "path"});
    requests.With({"GET", "/api/users"}).Add(1200);
    requests.With({"POST", "/api/orders"}).Add(300);
    auto& latency = registry.GetHistogram(
        "http_request_duration_seconds", "HTTP request latency", {"method"},
        {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0});
    for (int i = 0; i < 100; ++i) {
        latency.With({"GET"}).Observe(0.003 + (i % 10) * 0.004);
    }
    auto& active = registry.GetGauge("http_active_connections",
                                     "Active HTTP connections");
    active.Set(42);
    auto& errors =
        registry.GetCounter("app_errors_total", "Application errors", {"kind"});
    errors.With({"timeout"}).Add(3);
    errors.With({"panic"}).Inc();

    const std::string text = registry.Render();
    ParsedMetrics parsed;
    std::string error;
    CHECK(ParsePrometheus(text, &parsed, &error));
    CHECK(error.empty());
    CHECK(parsed.types.size() == 4);
    CHECK(parsed.sample_count >= 18);
    CHECK(ValidateHistogramFamily(parsed, "http_request_duration_seconds",
                                  &error));
    CHECK(error.empty());
    const auto* active_sample =
        FindSample(parsed, "http_active_connections", {});
    CHECK(active_sample != nullptr);
    if (active_sample != nullptr) {
        CHECK(active_sample->value == 42.0);
    }
    const auto* errors_sample =
        FindSample(parsed, "app_errors_total", {{"kind", "timeout"}});
    CHECK(errors_sample != nullptr);
    if (errors_sample != nullptr) {
        CHECK(errors_sample->value == 3.0);
    }
    std::printf("[metrics][scenario 5] samples=%d types=%zu bytes=%zu\n",
                parsed.sample_count, parsed.types.size(), text.size());
    std::printf("[metrics][prometheus-sample-begin]\n%s"
                "[metrics][prometheus-sample-end]\n",
                text.c_str());
}

std::string BuildDump() {
    Registry registry;
    auto& requests = registry.GetCounter(
        "demo_http_requests_total", "HTTP requests", {"method", "path"});
    requests.With({"GET", "/api/users"}).Add(1200);
    requests.With({"POST", "/api/orders"}).Add(300);
    auto& latency = registry.GetHistogram(
        "demo_http_request_duration_seconds", "HTTP request latency",
        {"method"}, {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0});
    for (int i = 0; i < 100; ++i) {
        latency.With({"GET"}).Observe(0.003 + (i % 10) * 0.004);
    }
    registry.GetGauge("demo_active_connections", "Active connections").Set(42);
    auto& errors = registry.GetCounter("demo_errors_total",
                                       "Application errors", {"kind"});
    errors.With({"timeout"}).Add(3);
    errors.With({"panic"}).Inc();
    return registry.Render();
}

void WriteDumpIfRequested(int argc, char** argv) {
    constexpr std::string_view kPrefix = "--dump-prometheus=";
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument.rfind(kPrefix, 0) != 0) {
            continue;
        }
        const std::string path(argument.substr(kPrefix.size()));
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::fprintf(stderr, "[metrics] cannot open dump path: %s\n",
                         path.c_str());
            ++g_failures;
            return;
        }
        const std::string text = BuildDump();
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output) {
            std::fprintf(stderr, "[metrics] failed to write dump: %s\n",
                         path.c_str());
            ++g_failures;
        } else {
            std::printf("[metrics] wrote Prometheus dump: %s (%zu bytes)\n",
                        path.c_str(), text.size());
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    TestCounterBasics();
    TestGaugeBasics();
    TestHistogramBasics();
    TestQuantileAccuracy();
    TestRegistryBasics();
    TestCounterStress();
    TestGaugeHistogramStress();
    TestConcurrentLabels();
    TestFiberStress();
    TestRandomLabels();
    TestRandomHistogram();
    ScenarioHttpRequests();
    ScenarioLatency();
    ScenarioConnections();
    ScenarioErrorRate();
    ScenarioPrometheusOutput();
    WriteDumpIfRequested(argc, argv);

    std::printf("[metrics] checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
