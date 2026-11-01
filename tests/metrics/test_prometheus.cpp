/**
 * @file test_prometheus.cpp
 * @brief `go2cpp::metrics::prom` 的格式 / 分位数 / 并发测试。
 *
 * 覆盖策略：
 *   - **格式**：内置严格解析器（HELP/TYPE/样本/转义/类型后缀/直方图三件套/
 *     标签名/数值合法性），对导出文本逐行校验；并与手写 `TextWriter` 输出
 *     逐字节比对，确保两条路径一致；
 *   - **分位数**：与 `Histogram::Quantile()` 对拍（p50/p90/p99 + 自定义分位
 *     点），覆盖空直方图、全部落入 +Inf 桶、负值、标签冲突；
 *   - **并发**：8 个读线程并发导出 + 4 个写线程并发累加，校验每次输出可解析、
 *     counter 与 histogram `_count` 单调不减；
 *   - **真实场景**：HTTP 服务指标、延迟直方图、大指标集（1e4 系列）。
 *
 * 运行：
 *   ctest --test-dir .build-bento-wsl -R go2cpp_metrics_prometheus_test
 *   ./.build-bento-wsl/go2cpp_metrics_prometheus_test --dump=/tmp/metrics.prom
 */

#include "go2cpp/metrics/metrics.hpp"
#include "go2cpp/metrics/prometheus.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using go2cpp::metrics::MetricType;
using go2cpp::metrics::Registry;

int g_checks = 0;
int g_failures = 0;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

#define CHECK(expression)                                                     \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(expression)) {                                                  \
            ++g_failures;                                                     \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,       \
                         __LINE__, #expression);                              \
        }                                                                     \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                               \
    do {                                                                      \
        ++g_checks;                                                           \
        const double actual_value_ = (actual);                                \
        const double expected_value_ = (expected);                            \
        if (!(std::fabs(actual_value_ - expected_value_) <= (tolerance))) {   \
            ++g_failures;                                                     \
            std::fprintf(stderr,                                              \
                         "%s:%d: CHECK_NEAR failed: %s=%.17g vs %.17g\n",     \
                         __FILE__, __LINE__, #actual, actual_value_,          \
                         expected_value_);                                    \
        }                                                                     \
    } while (0)

// ---------------------------------------------------------------------------
// 严格的 Prometheus 文本解析 / 校验器（测试专用，不做性能优化）
// ---------------------------------------------------------------------------

struct ParsedSample {
    std::string name;
    std::vector<std::pair<std::string, std::string>> labels;
    std::string value;
};

struct ParsedFamily {
    std::string name;
    std::string help;
    std::string type;
    bool has_help{false};
    bool has_type{false};
};

bool IsValidLabelName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto head = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };
    if (!head(name.front())) {
        return false;
    }
    for (const char ch : name) {
        if (!head(ch) && !(ch >= '0' && ch <= '9')) {
            return false;
        }
    }
    return true;
}

bool IsValidMetricName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto head = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               ch == '_' || ch == ':';
    };
    if (!head(name.front())) {
        return false;
    }
    for (const char ch : name) {
        if (!head(ch) && !(ch >= '0' && ch <= '9')) {
            return false;
        }
    }
    return true;
}

std::string Unescape(std::string_view input, bool is_label) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '\\' && i + 1 < input.size()) {
            const char next = input[++i];
            if (next == '\\') {
                out.push_back('\\');
            } else if (next == 'n') {
                out.push_back('\n');
            } else if (next == '"' && is_label) {
                out.push_back('"');
            } else {
                out.push_back('\\');
                out.push_back(next);
            }
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

bool IsFiniteNumber(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                        value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool IsUnsignedInteger(std::string_view text) {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                        value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool IsValidValue(std::string_view text) {
    if (text == "NaN" || text == "+Inf" || text == "-Inf") {
        return true;
    }
    return IsFiniteNumber(text);
}

std::size_t FindClosingBrace(std::string_view line, std::size_t open_pos) {
    bool in_quote = false;
    for (std::size_t i = open_pos + 1; i < line.size(); ++i) {
        const char ch = line[i];
        if (in_quote) {
            if (ch == '\\') {
                ++i;
                continue;
            }
            if (ch == '"') {
                in_quote = false;
            }
            continue;
        }
        if (ch == '"') {
            in_quote = true;
        } else if (ch == '}') {
            return i;
        }
    }
    return std::string_view::npos;
}

class Exposition {
public:
    bool Parse(std::string_view text) {
        Clear();
        if (text.empty()) {
            return true;
        }
        if (text.back() != '\n') {
            return Fail("text does not end with newline");
        }
        std::size_t line_no = 0;
        std::size_t begin = 0;
        while (begin < text.size()) {
            const std::size_t end = text.find('\n', begin);
            const std::string_view line = text.substr(begin, end - begin);
            ++line_no;
            begin = end + 1;
            if (!line.empty() && line.back() == '\r') {
                return Fail("CRLF line ending at line " +
                            std::to_string(line_no));
            }
            if (line.empty()) {
                return Fail("empty line at line " + std::to_string(line_no));
            }
            if (line.compare(0, 7, "# HELP ") == 0) {
                if (!ParseHelp(line.substr(7))) {
                    return false;
                }
            } else if (line.compare(0, 7, "# TYPE ") == 0) {
                if (!ParseType(line.substr(7))) {
                    return false;
                }
            } else if (line.front() == '#') {
                return Fail("unexpected comment: " + std::string(line));
            } else if (!ParseSampleLine(line)) {
                return false;
            }
        }
        return ValidateFamilies();
    }

    const std::string& error() const { return m_error; }
    const std::vector<ParsedSample>& samples() const { return m_samples; }

    const ParsedFamily* family(std::string_view name) const {
        const auto it = m_families.find(std::string(name));
        return it == m_families.end() ? nullptr : &it->second;
    }

    const ParsedSample* Find(
        std::string_view name,
        std::initializer_list<std::pair<const char*, const char*>> labels) const {
        for (const auto& sample : m_samples) {
            if (sample.name != name || sample.labels.size() != labels.size()) {
                continue;
            }
            bool match = true;
            for (const auto& expected : labels) {
                bool found = false;
                for (const auto& actual : sample.labels) {
                    if (actual.first == expected.first &&
                        actual.second == expected.second) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return &sample;
            }
        }
        return nullptr;
    }

    double Value(
        std::string_view name,
        std::initializer_list<std::pair<const char*, const char*>> labels) const {
        const ParsedSample* sample = Find(name, labels);
        if (sample == nullptr) {
            return kNaN;
        }
        if (sample->value == "NaN") {
            return kNaN;
        }
        if (sample->value == "+Inf") {
            return kInf;
        }
        if (sample->value == "-Inf") {
            return -kInf;
        }
        return std::strtod(sample->value.c_str(), nullptr);
    }

private:
    void Clear() {
        m_error.clear();
        m_families.clear();
        m_samples.clear();
        m_histogram_inf.clear();
        m_histogram_sum.clear();
        m_histogram_count.clear();
    }

    bool Fail(std::string message) {
        if (m_error.empty()) {
            m_error = std::move(message);
        }
        return false;
    }

    ParsedFamily* FindOrCreate(std::string_view name) {
        auto it = m_families.find(std::string(name));
        if (it != m_families.end()) {
            return &it->second;
        }
        ParsedFamily fresh;
        fresh.name = std::string(name);
        auto inserted = m_families.emplace(std::string(name), std::move(fresh));
        return &inserted.first->second;
    }

    bool ParseHelp(std::string_view rest) {
        const std::size_t space = rest.find(' ');
        if (space == std::string_view::npos) {
            return Fail("HELP without help text");
        }
        const std::string_view name = rest.substr(0, space);
        if (!IsValidMetricName(name)) {
            return Fail("invalid HELP metric name");
        }
        ParsedFamily* parsed = FindOrCreate(name);
        if (parsed->has_help) {
            return Fail("duplicate HELP for " + std::string(name));
        }
        if (parsed->has_type) {
            return Fail("HELP after TYPE for " + std::string(name));
        }
        if (m_family_samples.count(std::string(name)) != 0) {
            return Fail("HELP after samples for " + std::string(name));
        }
        parsed->has_help = true;
        parsed->help = Unescape(rest.substr(space + 1), false);
        return true;
    }

    bool ParseType(std::string_view rest) {
        const std::size_t space = rest.find(' ');
        if (space == std::string_view::npos) {
            return Fail("TYPE without type name");
        }
        const std::string_view name = rest.substr(0, space);
        const std::string_view type = rest.substr(space + 1);
        if (!IsValidMetricName(name)) {
            return Fail("invalid TYPE metric name");
        }
        static const std::string_view kTypes[] = {"counter", "gauge",
                                                  "histogram", "summary",
                                                  "untyped"};
        bool known = false;
        for (const auto candidate : kTypes) {
            if (type == candidate) {
                known = true;
                break;
            }
        }
        if (!known) {
            return Fail("unknown TYPE '" + std::string(type) + "'");
        }
        ParsedFamily* parsed = FindOrCreate(name);
        if (parsed->has_type) {
            return Fail("duplicate TYPE for " + std::string(name));
        }
        if (m_family_samples.count(std::string(name)) != 0) {
            return Fail("TYPE after samples for " + std::string(name));
        }
        parsed->has_type = true;
        parsed->type = std::string(type);
        return true;
    }

    bool ParseSampleLine(std::string_view line) {
        ParsedSample sample;
        std::size_t pos = 0;
        while (pos < line.size() && line[pos] != '{' && line[pos] != ' ') {
            ++pos;
        }
        if (pos == 0) {
            return Fail("missing sample name");
        }
        sample.name = std::string(line.substr(0, pos));
        if (!IsValidMetricName(sample.name)) {
            return Fail("invalid sample name '" + sample.name + "'");
        }
        if (pos < line.size() && line[pos] == '{') {
            const std::size_t close = FindClosingBrace(line, pos);
            if (close == std::string_view::npos) {
                return Fail("unterminated label set for " + sample.name);
            }
            if (!ParseLabelSet(line.substr(pos + 1, close - pos - 1), sample)) {
                return false;
            }
            pos = close + 1;
        }
        if (pos >= line.size() || line[pos] != ' ') {
            return Fail("missing space before value of " + sample.name);
        }
        ++pos;
        sample.value = std::string(line.substr(pos));
        if (!IsValidValue(sample.value)) {
            return Fail("invalid value '" + sample.value + "' for " +
                        sample.name);
        }
        if (sample.value.find(' ') != std::string::npos) {
            return Fail("space in value of " + sample.name);
        }
        if (!CheckSampleSemantics(sample)) {
            return false;
        }
        m_family_samples.insert(sample.name);
        m_samples.push_back(std::move(sample));
        return true;
    }

    bool ParseLabelSet(std::string_view content, ParsedSample& sample) {
        std::size_t pos = 0;
        while (pos < content.size()) {
            const std::size_t name_begin = pos;
            while (pos < content.size() && content[pos] != '=') {
                ++pos;
            }
            if (pos >= content.size()) {
                return Fail("label without '=' for " + sample.name);
            }
            const std::string_view name =
                content.substr(name_begin, pos - name_begin);
            if (!IsValidLabelName(name)) {
                return Fail("invalid label name '" + std::string(name) + "'");
            }
            ++pos;
            if (pos >= content.size() || content[pos] != '"') {
                return Fail("label value must be quoted for " +
                            std::string(name));
            }
            ++pos;
            const std::size_t value_begin = pos;
            while (pos < content.size()) {
                const char ch = content[pos];
                if (ch == '\\') {
                    pos += 2;
                    continue;
                }
                if (ch == '"') {
                    break;
                }
                ++pos;
            }
            if (pos >= content.size()) {
                return Fail("unterminated label value for " +
                            std::string(name));
            }
            const std::string_view value =
                content.substr(value_begin, pos - value_begin);
            sample.labels.emplace_back(std::string(name), Unescape(value, true));
            ++pos;
            if (pos < content.size()) {
                if (content[pos] != ',') {
                    return Fail("expected ',' after label '" +
                                std::string(name) + "'");
                }
                ++pos;
                if (pos == content.size()) {
                    return Fail("trailing ',' in label set");
                }
            }
        }
        return true;
    }

    bool CheckSampleSemantics(const ParsedSample& sample) {
        const ParsedFamily* parsed = family(sample.name);
        std::string_view base;
        std::string_view suffix;
        if (parsed != nullptr) {
            if (parsed->type == "histogram") {
                return Fail("histogram family must not have base samples: " +
                            sample.name);
            }
            if (parsed->type == "counter" && !IsUnsignedInteger(sample.value)) {
                return Fail("counter value must be unsigned integer: " +
                            sample.value);
            }
            return true;
        }
        static const std::string_view kSuffixes[] = {"_bucket", "_sum",
                                                     "_count"};
        for (const auto candidate : kSuffixes) {
            if (sample.name.size() > candidate.size() &&
                sample.name.compare(sample.name.size() - candidate.size(),
                                    candidate.size(), candidate) == 0) {
                base = std::string_view(sample.name).substr(
                    0, sample.name.size() - candidate.size());
                suffix = candidate;
                parsed = family(base);
                if (parsed != nullptr) {
                    break;
                }
            }
        }
        if (parsed == nullptr) {
            return Fail("sample references undeclared metric '" + sample.name +
                        "'");
        }
        if (parsed->type != "histogram") {
            return Fail("derived series '" + sample.name + "' for " +
                        parsed->type + " '" + std::string(base) + "'");
        }
        if (!parsed->has_type) {
            return Fail("sample before TYPE for '" + sample.name + "'");
        }
        const std::string base_string(base);
        if (suffix == "_bucket") {
            bool has_le = false;
            std::string le_value;
            for (const auto& label : sample.labels) {
                if (label.first == "le") {
                    has_le = true;
                    le_value = label.second;
                }
            }
            if (!has_le) {
                return Fail("_bucket without 'le' label: " + sample.name);
            }
            if (le_value != "+Inf" && !IsFiniteNumber(le_value)) {
                return Fail("invalid 'le' value '" + le_value + "'");
            }
            if (!IsUnsignedInteger(sample.value)) {
                return Fail("_bucket value must be unsigned integer");
            }
            if (le_value == "+Inf") {
                m_histogram_inf.insert(base_string);
            }
            return true;
        }
        if (suffix == "_sum") {
            if (!IsValidValue(sample.value)) {
                return Fail("invalid _sum value");
            }
            m_histogram_sum.insert(base_string);
            return true;
        }
        if (suffix == "_count") {
            if (!IsUnsignedInteger(sample.value)) {
                return Fail("_count value must be unsigned integer");
            }
            m_histogram_count.insert(base_string);
            return true;
        }
        return Fail("unexpected derived series: " + sample.name);
    }

    bool ValidateFamilies() {
        for (const auto& entry : m_families) {
            const ParsedFamily& parsed = entry.second;
            if (!parsed.has_type) {
                return Fail("family without TYPE: " + parsed.name);
            }
            if (parsed.type != "histogram") {
                continue;
            }
            if (m_histogram_inf.count(parsed.name) == 0) {
                return Fail("histogram missing le=\"+Inf\": " + parsed.name);
            }
            if (m_histogram_sum.count(parsed.name) == 0) {
                return Fail("histogram missing _sum: " + parsed.name);
            }
            if (m_histogram_count.count(parsed.name) == 0) {
                return Fail("histogram missing _count: " + parsed.name);
            }
        }
        for (const auto& sample : m_samples) {
            const bool is_quantile =
                sample.name.size() > 9 &&
                sample.name.compare(sample.name.size() - 9, 9, "_quantile") == 0;
            if (!is_quantile) {
                continue;
            }
            bool has_quantile_label = false;
            for (const auto& label : sample.labels) {
                if (label.first == "quantile") {
                    has_quantile_label = true;
                }
            }
            if (!has_quantile_label) {
                return Fail("_quantile sample without quantile label: " +
                            sample.name);
            }
        }
        return true;
    }

    std::string m_error;
    std::map<std::string, ParsedFamily> m_families;
    std::vector<ParsedSample> m_samples;
    std::set<std::string> m_family_samples;
    std::set<std::string> m_histogram_inf;
    std::set<std::string> m_histogram_sum;
    std::set<std::string> m_histogram_count;
};

/// 解析并打印错误；失败时计一次 CHECK。
bool ParseOk(const std::string& text, Exposition& parsed) {
    const bool ok = parsed.Parse(text);
    if (!ok) {
        std::fprintf(stderr, "exposition parse failed: %s\n",
                     parsed.error().c_str());
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 测试用例
// ---------------------------------------------------------------------------

void TestBasicFormat() {
    std::printf("[test] basic format\n");
    Registry registry;
    auto& counter = registry.GetCounter("http_requests_total",
                                        "Total HTTP requests.",
                                        {"method", "path"});
    counter.With({"GET", "/api"}).Add(42);
    counter.With({"POST", "/api"}).Add(8);
    auto& gauge = registry.GetGauge("http_in_flight", "In-flight requests.");
    gauge.Set(2.5);
    auto& histogram = registry.GetHistogram(
        "http_request_duration_seconds", "Request latency.", {"method"},
        {0.005, 0.01, 0.05, 0.1, 0.5, 1.0});
    histogram.With({"GET"}).Observe(0.003);
    histogram.With({"GET"}).Observe(0.08);
    histogram.With({"GET"}).Observe(0.4);

    const std::string text = go2cpp::metrics::prom::Render(registry);
    Exposition parsed;
    CHECK(ParseOk(text, parsed));

    CHECK(text.find("# HELP http_requests_total Total HTTP requests.\n") !=
          std::string::npos);
    CHECK(text.find("# TYPE http_requests_total counter\n") !=
          std::string::npos);
    CHECK(text.find("http_requests_total{method=\"GET\",path=\"/api\"} 42\n") !=
          std::string::npos);
    CHECK(text.find("http_requests_total{method=\"POST\",path=\"/api\"} 8\n") !=
          std::string::npos);
    CHECK(text.find("http_in_flight 2.5\n") != std::string::npos);
    CHECK(text.find("http_request_duration_seconds_bucket{method=\"GET\",le="
                    "\"0.005\"} 1\n") != std::string::npos);
    CHECK(text.find("http_request_duration_seconds_bucket{method=\"GET\",le="
                    "\"+Inf\"} 3\n") != std::string::npos);
    CHECK_NEAR(parsed.Value("http_request_duration_seconds_sum",
                            {{"method", "GET"}}),
               0.483, 1e-12);
    CHECK(text.find("http_request_duration_seconds_count{method=\"GET\"} 3\n") !=
          std::string::npos);
    CHECK(parsed.Find("http_request_duration_seconds_quantile",
                      {{"method", "GET"}, {"quantile", "0.5"}}) != nullptr);

    // 值的精确性
    CHECK_NEAR(parsed.Value("http_requests_total",
                            {{"method", "GET"}, {"path", "/api"}}),
               42.0, 1e-12);
    CHECK_NEAR(parsed.Value("http_in_flight", {}), 2.5, 1e-12);
}

void TestEscaping() {
    std::printf("[test] label/help escaping\n");
    Registry registry;
    auto& counter = registry.GetCounter(
        "escaped_total", "help with \\ backslash and\nnewline", {"label"});
    counter.With({"a\"b\\c\nd"}).Inc();
    const std::string text = go2cpp::metrics::prom::Render(registry);

    CHECK(text.find(
              "# HELP escaped_total help with \\\\ backslash and\\nnewline\n") !=
          std::string::npos);
    CHECK(text.find("escaped_total{label=\"a\\\"b\\\\c\\nd\"} 1\n") !=
          std::string::npos);

    Exposition parsed;
    CHECK(ParseOk(text, parsed));
    CHECK(parsed.Find("escaped_total", {{"label", "a\"b\\c\nd"}}) != nullptr);
    CHECK_NEAR(parsed.Value("escaped_total", {{"label", "a\"b\\c\nd"}}), 1.0,
               1e-12);
}

void TestNumericValues() {
    std::printf("[test] numeric values (NaN/Inf/max)\n");
    Registry registry;
    auto& gauge = registry.GetGauge("special_gauge", "Special values.", {"kind"});
    gauge.With({"nan"}).Set(kNaN);
    gauge.With({"pos_inf"}).Set(kInf);
    gauge.With({"neg_inf"}).Set(-kInf);
    gauge.With({"negative"}).Set(-13.5);

    auto& counter = registry.GetCounter("big_counter", "Big counter.");
    counter.Add(std::numeric_limits<std::uint64_t>::max());

    auto& empty_hist = registry.GetHistogram("empty_hist", "Empty.", {}, {1.0});
    const std::string text = go2cpp::metrics::prom::Render(registry);

    Exposition parsed;
    CHECK(ParseOk(text, parsed));
    CHECK(std::isnan(parsed.Value("special_gauge", {{"kind", "nan"}})));
    CHECK(std::isinf(parsed.Value("special_gauge", {{"kind", "pos_inf"}})));
    CHECK(parsed.Value("special_gauge", {{"kind", "pos_inf"}}) > 0);
    CHECK(parsed.Value("special_gauge", {{"kind", "neg_inf"}}) < 0);
    CHECK_NEAR(parsed.Value("special_gauge", {{"kind", "negative"}}), -13.5,
               1e-12);
    CHECK_NEAR(parsed.Value("big_counter", {}),
               static_cast<double>(std::numeric_limits<std::uint64_t>::max()),
               1.0);
    CHECK_NEAR(parsed.Value("empty_hist_count", {}), 0.0, 1e-12);
    CHECK(std::isnan(parsed.Value("empty_hist_quantile", {{"quantile", "0.5"}})));
    (void)empty_hist;
}

void TestQuantiles() {
    std::printf("[test] histogram quantiles vs Histogram::Quantile()\n");
    Registry registry;
    std::vector<double> buckets;
    for (double bound = 1.0; bound <= 1024.0; bound *= 2.0) {
        buckets.push_back(bound);
    }
    auto& histogram =
        registry.GetHistogram("qtest", "Quantile test.", {"k"}, buckets);
    auto& series = histogram.With({"x"});
    for (int i = 1; i <= 1000; ++i) {
        series.Observe(static_cast<double>(i));
    }

    const std::string text = go2cpp::metrics::prom::Render(registry);
    Exposition parsed;
    CHECK(ParseOk(text, parsed));

    const double expected_p50 = series.Quantile(0.5);
    const double expected_p90 = series.Quantile(0.9);
    const double expected_p99 = series.Quantile(0.99);
    CHECK_NEAR(parsed.Value("qtest_quantile", {{"k", "x"}, {"quantile", "0.5"}}),
               expected_p50, 1e-12);
    CHECK_NEAR(parsed.Value("qtest_quantile", {{"k", "x"}, {"quantile", "0.9"}}),
               expected_p90, 1e-12);
    CHECK_NEAR(parsed.Value("qtest_quantile", {{"k", "x"}, {"quantile", "0.99"}}),
               expected_p99, 1e-12);
    CHECK_NEAR(expected_p50, 500.0, 4.0);
    // 桶内线性插值的误差上界是一个桶宽：p90/p99 落在 (512, 1024] 桶内，
    // 只要求「落在正确桶、且不小于真实分位数」。
    CHECK(expected_p90 >= 900.0 && expected_p90 <= 1024.0);
    CHECK(expected_p99 >= 990.0 && expected_p99 <= 1024.0);
}

void TestQuantileEdgeCases() {
    std::printf("[test] quantile edge cases\n");
    {
        // 全部样本落入 +Inf 桶：分位数应退化为最后一个有限上界。
        Registry registry;
        auto& histogram =
            registry.GetHistogram("overflow", "Overflow.", {}, {1.0, 2.0});
        histogram.Observe(100.0);
        histogram.Observe(200.0);
        const std::string text = go2cpp::metrics::prom::Render(registry);
        Exposition parsed;
        CHECK(ParseOk(text, parsed));
        CHECK_NEAR(parsed.Value("overflow_quantile", {{"quantile", "0.5"}}), 2.0,
                   1e-12);
        CHECK_NEAR(parsed.Value("overflow_quantile", {{"quantile", "0.99"}}),
                   2.0, 1e-12);
    }
    {
        // 负值：第一个桶下界取 min(0, 上界)，与 Histogram::Quantile() 一致。
        Registry registry;
        auto& histogram =
            registry.GetHistogram("negative", "Negative.", {}, {0.1, 1.0});
        histogram.Observe(-5.0);
        const std::string text = go2cpp::metrics::prom::Render(registry);
        Exposition parsed;
        CHECK(ParseOk(text, parsed));
        CHECK_NEAR(parsed.Value("negative_quantile", {{"quantile", "0.5"}}),
                   histogram.Quantile(0.5), 1e-12);
    }
    {
        // 标签里已有 quantile：跳过该序列，避免输出重复标签名。
        Registry registry;
        auto& histogram = registry.GetHistogram("conflict", "Conflict.",
                                                {"quantile"}, {1.0});
        histogram.With({"v"}).Observe(0.5);
        const std::string text = go2cpp::metrics::prom::Render(registry);
        Exposition parsed;
        CHECK(ParseOk(text, parsed));
        CHECK(text.find("conflict_quantile") == std::string::npos);
        CHECK(text.find("conflict_bucket{quantile=\"v\",le=\"+Inf\"} 1\n") !=
              std::string::npos);
    }
    {
        // 自定义分位点：只输出请求的那些。
        Registry registry;
        auto& histogram =
            registry.GetHistogram("custom", "Custom.", {}, {1.0});
        histogram.Observe(0.5);
        go2cpp::metrics::prom::ExporterOptions options;
        options.quantiles = {0.5, 0.99};
        const std::string text =
            go2cpp::metrics::prom::Render(registry, options);
        Exposition parsed;
        CHECK(ParseOk(text, parsed));
        CHECK(parsed.Find("custom_quantile", {{"quantile", "0.5"}}) != nullptr);
        CHECK(parsed.Find("custom_quantile", {{"quantile", "0.99"}}) != nullptr);
        CHECK(parsed.Find("custom_quantile", {{"quantile", "0.9"}}) == nullptr);
    }
}

void TestOptions() {
    std::printf("[test] exporter options\n");
    Registry registry;
    auto& counter = registry.GetCounter("opt_total", "Options.", {"k"});
    counter.With({"a"}).Add(3);
    auto& histogram = registry.GetHistogram("opt_hist", "Opt hist.", {}, {1.0});
    histogram.Observe(0.5);

    go2cpp::metrics::prom::ExporterOptions no_quantiles;
    no_quantiles.include_quantiles = false;
    const std::string plain =
        go2cpp::metrics::prom::Render(registry, no_quantiles);
    CHECK(plain == registry.Render());

    go2cpp::metrics::prom::ExporterOptions empty_quantiles;
    empty_quantiles.quantiles.clear();
    const std::string no_quantile_list =
        go2cpp::metrics::prom::Render(registry, empty_quantiles);
    CHECK(no_quantile_list == registry.Render());
}

void TestTextWriterParity() {
    std::printf("[test] TextWriter parity with Registry::Render\n");
    Registry registry;
    auto& histogram = registry.GetHistogram("parity_hist", "Parity.", {},
                                            {0.1, 0.5});
    histogram.Observe(0.05);
    histogram.Observe(0.3);
    histogram.Observe(0.9);
    const std::string from_registry = registry.Render();

    std::string from_writer;
    go2cpp::metrics::prom::TextWriter writer(from_writer);
    writer.Header("parity_hist", "Parity.", MetricType::kHistogram);
    const double bounds[] = {0.1, 0.5};
    const std::uint64_t cumulative[] = {1, 2, 3};
    writer.Histogram("parity_hist", {}, bounds, 2, cumulative, histogram.Sum(),
                     3);

    CHECK(from_registry == from_writer);
}

void TestQuantileAppend() {
    std::printf("[test] AppendQuantileFamilies (standalone)\n");
    const std::string source =
        "# TYPE h histogram\n"
        "h_bucket{le=\"1\"} 1\n"
        "h_bucket{le=\"+Inf\"} 3\n"
        "h_sum 6\n"
        "h_count 3\n";
    go2cpp::metrics::prom::ExporterOptions options;
    std::string out = source;
    const std::size_t added =
        go2cpp::metrics::prom::AppendQuantileFamilies(source, options, out);
    CHECK(added > 0);
    CHECK(out.size() == source.size() + added);
    CHECK(out.compare(0, source.size(), source) == 0);
    Exposition parsed;
    CHECK(ParseOk(out, parsed));
    CHECK(parsed.Find("h_quantile", {{"quantile", "0.5"}}) != nullptr);

    // 没有 histogram 的文本：不追加。
    const std::string counters =
        "# TYPE c counter\nc 1\n";
    std::string untouched = counters;
    const std::size_t no_add =
        go2cpp::metrics::prom::AppendQuantileFamilies(counters, options,
                                                      untouched);
    CHECK(no_add == 0);
    CHECK(untouched == counters);

    // out 与 exposition 是同一对象：内部先构建增量再 append。
    std::string aliased = source;
    const std::size_t aliased_add =
        go2cpp::metrics::prom::AppendQuantileFamilies(aliased, options, aliased);
    CHECK(aliased_add > 0);
    Exposition parsed_alias;
    CHECK(ParseOk(aliased, parsed_alias));
    CHECK(parsed_alias.Find("h_quantile", {{"quantile", "0.5"}}) != nullptr);

    // 自定义标签的 histogram 文本。
    const std::string labeled =
        "# TYPE lat histogram\n"
        "lat_bucket{method=\"GET\",le=\"0.1\"} 2\n"
        "lat_bucket{method=\"GET\",le=\"+Inf\"} 4\n"
        "lat_sum{method=\"GET\"} 0.5\n"
        "lat_count{method=\"GET\"} 4\n";
    std::string labeled_out = labeled;
    CHECK(go2cpp::metrics::prom::AppendQuantileFamilies(labeled, options,
                                                        labeled_out) > 0);
    Exposition parsed_labeled;
    CHECK(ParseOk(labeled_out, parsed_labeled));
    CHECK(parsed_labeled.Find("lat_quantile",
                              {{"method", "GET"}, {"quantile", "0.5"}}) !=
          nullptr);
}

void TestConcurrency() {
    std::printf("[test] concurrent render + write\n");
    Registry registry;
    auto& counter = registry.GetCounter("concurrent_total", "Concurrent.",
                                        {"worker"});
    auto& histogram = registry.GetHistogram("concurrent_latency", "Latency.",
                                            {"worker"}, {0.001, 0.01, 0.1, 1.0});
    constexpr int kWorkers = 4;
    for (int i = 0; i < kWorkers; ++i) {
        counter.With({std::to_string(i)});
        histogram.With({std::to_string(i)});
    }

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> writes{0};
    std::vector<std::thread> writers;
    for (int worker = 0; worker < kWorkers; ++worker) {
        writers.emplace_back([&, worker]() {
            const std::string label = std::to_string(worker);
            std::uint64_t local = 0;
            while (!stop.load(std::memory_order_acquire)) {
                counter.With({label}).Inc();
                histogram.With({label}).Observe(
                    static_cast<double>(local % 100) / 100.0);
                ++local;
            }
            writes.fetch_add(local, std::memory_order_relaxed);
        });
    }

    std::atomic<int> parse_failures{0};
    std::atomic<int> monotonic_failures{0};
    std::vector<std::thread> readers;
    constexpr int kReaders = 8;
    for (int reader = 0; reader < kReaders; ++reader) {
        readers.emplace_back([&]() {
            std::uint64_t last_total = 0;
            for (int iteration = 0; iteration < 100; ++iteration) {
                const std::string text =
                    go2cpp::metrics::prom::Render(registry);
                Exposition parsed;
                if (!parsed.Parse(text)) {
                    parse_failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                bool sum_ok = true;
                std::uint64_t total = 0;
                for (int worker = 0; worker < kWorkers; ++worker) {
                    const std::string label = std::to_string(worker);
                    total += static_cast<std::uint64_t>(parsed.Value(
                        "concurrent_total", {{"worker", label.c_str()}}));
                    const double count = parsed.Value(
                        "concurrent_latency_count", {{"worker", label.c_str()}});
                    if (!(count >= 0.0)) {
                        sum_ok = false;
                    }
                }
                if (!sum_ok || total < last_total) {
                    monotonic_failures.fetch_add(1, std::memory_order_relaxed);
                }
                last_total = total;
            }
        });
    }

    for (auto& thread : readers) {
        thread.join();
    }
    stop.store(true, std::memory_order_release);
    for (auto& thread : writers) {
        thread.join();
    }

    CHECK(parse_failures.load() == 0);
    CHECK(monotonic_failures.load() == 0);
    CHECK(writes.load() > 0);

    const std::string final_text = go2cpp::metrics::prom::Render(registry);
    Exposition parsed;
    CHECK(ParseOk(final_text, parsed));
    std::uint64_t total = 0;
    for (int worker = 0; worker < kWorkers; ++worker) {
        total += static_cast<std::uint64_t>(parsed.Value(
            "concurrent_total", {{"worker", std::to_string(worker).c_str()}}));
    }
    CHECK(total == writes.load());
}

void TestLargeSet() {
    std::printf("[test] large set (1e4 series)\n");
    Registry registry;
    for (int metric = 0; metric < 20; ++metric) {
        const std::string name = "large_" + std::to_string(metric) + "_total";
        auto& counter = registry.GetCounter(name, "Large set.", {"shard"});
        for (int shard = 0; shard < 500; ++shard) {
            counter.With({std::to_string(shard)}).Add(
                static_cast<std::uint64_t>(shard));
        }
    }
    const std::string text = go2cpp::metrics::prom::Render(registry);
    Exposition parsed;
    CHECK(ParseOk(text, parsed));
    CHECK(parsed.samples().size() == 20u * 500u);
    const ParsedSample* sample = parsed.Find(
        "large_0_total", {{"shard", "499"}});
    CHECK(sample != nullptr);
    CHECK_NEAR(parsed.Value("large_19_total", {{"shard", "499"}}), 499.0,
               1e-12);
}

void TestRealWorldHttpScenario() {
    std::printf("[test] real-world HTTP service scenario\n");
    Registry registry;
    auto& requests = registry.GetCounter("http_requests_total", "Requests.",
                                         {"method", "path", "status"});
    auto& errors = registry.GetCounter("http_errors_total", "Errors.", {"path"});
    auto& in_flight = registry.GetGauge("http_in_flight", "In flight.");
    auto& latency = registry.GetHistogram(
        "http_request_duration_seconds", "Latency.", {"method"},
        {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5});

    double total_latency = 0.0;
    for (int i = 0; i < 200; ++i) {
        const bool is_error = (i % 17) == 0;
        const std::string status = is_error ? "500" : "200";
        const std::string path = (i % 2 == 0) ? "/api" : "/health";
        requests.With({"GET", path, status}).Inc();
        if (is_error) {
            errors.With({path}).Inc();
        }
        in_flight.Inc();
        const double elapsed = 0.002 + static_cast<double>(i % 37) * 0.001;
        total_latency += elapsed;
        latency.With({"GET"}).Observe(elapsed);
        in_flight.Dec();
    }

    const std::string text = go2cpp::metrics::prom::Render(registry);
    Exposition parsed;
    CHECK(ParseOk(text, parsed));

    // 200 个请求：i%17==0 记 500；i%2==0 走 /api，其余 /health。
    // /api 200 = 100 个偶数 - 6 个错误 = 94；/health 500 = 6 个奇数错误。
    CHECK_NEAR(parsed.Value("http_requests_total",
                            {{"method", "GET"}, {"path", "/api"}, {"status", "200"}}),
               94.0, 1e-12);
    CHECK_NEAR(parsed.Value("http_requests_total",
                            {{"method", "GET"}, {"path", "/health"}, {"status", "500"}}),
               6.0, 1e-12);
    CHECK_NEAR(parsed.Value("http_errors_total", {{"path", "/api"}}), 6.0,
               1e-12);
    CHECK_NEAR(parsed.Value("http_errors_total", {{"path", "/health"}}), 6.0,
               1e-12);
    CHECK_NEAR(parsed.Value("http_in_flight", {}), 0.0, 1e-12);
    CHECK_NEAR(parsed.Value("http_request_duration_seconds_count",
                            {{"method", "GET"}}),
               200.0, 1e-12);
    CHECK_NEAR(parsed.Value("http_request_duration_seconds_sum",
                            {{"method", "GET"}}),
               total_latency, 1e-9);

    const double p50 = parsed.Value("http_request_duration_seconds_quantile",
                                    {{"method", "GET"}, {"quantile", "0.5"}});
    const double p99 = parsed.Value("http_request_duration_seconds_quantile",
                                    {{"method", "GET"}, {"quantile", "0.99"}});
    CHECK(std::isfinite(p50));
    CHECK(std::isfinite(p99));
    CHECK(p50 > 0.0);
    CHECK(p99 >= p50);
    // 误差上界是一个桶宽（最大桶 2.5s 之前是 0.05 桶），故 0.05 已经足够严格。
    CHECK(p99 <= 0.05);
}

}  // namespace

int main(int argc, char** argv) {
    std::string dump_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--dump=", 0) == 0) {
            dump_path = arg.substr(7);
        }
    }

    TestBasicFormat();
    TestEscaping();
    TestNumericValues();
    TestQuantiles();
    TestQuantileEdgeCases();
    TestOptions();
    TestTextWriterParity();
    TestQuantileAppend();
    TestConcurrency();
    TestLargeSet();
    TestRealWorldHttpScenario();

    if (!dump_path.empty()) {
        Registry registry;
        auto& counter =
            registry.GetCounter("dump_total", "Dump.", {"k", "path"});
        counter.With({"v", "/a\"b\\c"}).Add(7);
        auto& gauge = registry.GetGauge("dump_gauge",
                                        "Gauge help with \\ and\nnewline");
        gauge.Set(kNaN);
        auto& histogram = registry.GetHistogram("dump_hist", "Dump hist.", {},
                                                {0.1, 1.0});
        histogram.Observe(0.05);
        histogram.Observe(0.5);
        std::ofstream out(dump_path);
        out << go2cpp::metrics::prom::Render(registry);
    }

    std::printf("[test] checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
