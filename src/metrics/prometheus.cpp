/**
 * @file prometheus.cpp
 * @brief `go2cpp::metrics::prom` 的实现：文本拼接、分位数、文本后处理。
 *
 * ## 实现顺序
 * step1. 数值/转义写入口（`AppendDouble` / `AppendUint` / `AppendEscaped*`）；
 * step2. `TextWriter`（纯 append，无中间 string）；
 * step3. `BucketQuantile`（与 `Histogram::Quantile()` 同一套插值）；
 * step4. `AppendQuantileFamilies`（扫描文本 -> 分组桶 -> 追加分位数族）；
 * step5. `Exporter`（`Registry::Render()` + 分位数追加）。
 *
 * ## 性能约束（与 docs/modules/metrics-prometheus.md §5 对应）
 * - 热路径（每个样本）只有：`append`（memcpy 级）+ `to_chars`（无分配）；
 * - 不构造临时 `std::string`、不用 `ostringstream`、不做 locale 转换；
 * - 分位数后处理只在存在 histogram 时发生，且对不含 `_bucket` 的行只做一次
 *   `find` 级别的丢弃。
 *
 * ## 边界
 * - 文本解析只认本模块 / `Registry::Render()` 的导出形态（`le` 在标签末尾、
 *   桶按升序连续出现）；不符合的行被跳过，不会产生半行输出；
 * - `AppendQuantileFamilies` 允许 `out` 与 `exposition` 同一对象：先构建
 *   增量再 `append`，增量构建期间不修改输入。
 */

#include "go2cpp/metrics/prometheus.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace go2cpp {
namespace metrics {
namespace prom {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kNegInf = -std::numeric_limits<double>::infinity();
constexpr double kPosInf = std::numeric_limits<double>::infinity();

/// `# HELP ` / `# TYPE ` 的字面长度；用 `append(ptr, len)` 避免 strlen。
constexpr std::size_t kHeaderKeywordSize = 7;
constexpr std::size_t kBucketSuffixSize = 7;   ///< "_bucket"
constexpr std::size_t kSumSuffixSize = 4;      ///< "_sum"
constexpr std::size_t kCountSuffixSize = 6;    ///< "_count"
constexpr std::size_t kQuantileSuffixSize = 9; ///< "_quantile"

/**
 * @brief 追加一组标签（含外层 `{}`）。
 *
 * 无标签时不输出任何字符 —— Prometheus 规范里 `name 1` 与 `name{} 1` 等价，
 * 但前者更短，且与本项目 `metrics.hpp` 的输出保持一致。
 */
void AppendLabelSet(std::string& out, const LabelView& labels) {
    if (labels.empty()) {
        return;
    }
    out.push_back('{');
    bool first = true;
    for (const Label& label : labels) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out.append(label.name.data(), label.name.size());
        out.push_back('=');
        out.push_back('"');
        AppendEscapedLabelValue(out, label.value);
        out.push_back('"');
    }
    out.push_back('}');
}

/// 追加 `name{labels}`（无标签时省略 `{}`）。
void AppendNameAndLabels(std::string& out, std::string_view name,
                         const LabelView& labels) {
    out.append(name.data(), name.size());
    AppendLabelSet(out, labels);
}

/// 追加 `_bucket` 序列的一行：`<name>_bucket{<labels>,le="<bound>"} value`。
void AppendBucketSample(std::string& out, std::string_view name,
                        const LabelView& labels, std::string_view le_text,
                        std::uint64_t value) {
    out.append(name.data(), name.size());
    out.append("_bucket", kBucketSuffixSize);
    out.push_back('{');
    bool first = true;
    for (const Label& label : labels) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out.append(label.name.data(), label.name.size());
        out.push_back('=');
        out.push_back('"');
        AppendEscapedLabelValue(out, label.value);
        out.push_back('"');
    }
    if (!first) {
        out.push_back(',');
    }
    out.append("le=\"", 4);
    out.append(le_text.data(), le_text.size());
    out.push_back('"');
    out.push_back('}');
    out.push_back(' ');
    AppendUint(out, value);
    out.push_back('\n');
}

/// 追加分位数族头：`# HELP <base>_quantile ...` + `# TYPE ... gauge`。
void AppendQuantileHeader(std::string& out, std::string_view base_name) {
    out.append("# HELP ", kHeaderKeywordSize);
    out.append(base_name.data(), base_name.size());
    out.append("_quantile", kQuantileSuffixSize);
    out.append(" Estimated quantiles derived from histogram buckets.\n");
    out.append("# TYPE ", kHeaderKeywordSize);
    out.append(base_name.data(), base_name.size());
    out.append("_quantile", kQuantileSuffixSize);
    out.append(" gauge\n");
}

/// 追加一行分位数样本：`<base>_quantile{<raw_labels>,quantile="q"} value`。
///
/// `raw_labels` 是原文本里的标签串（已转义、不含 `le`），可能为空。
void AppendQuantileSampleRaw(std::string& out, std::string_view base_name,
                             std::string_view raw_labels,
                             std::string_view q_text, double value) {
    out.append(base_name.data(), base_name.size());
    out.append("_quantile", kQuantileSuffixSize);
    out.push_back('{');
    if (!raw_labels.empty()) {
        out.append(raw_labels.data(), raw_labels.size());
        out.push_back(',');
    }
    out.append("quantile=\"", 10);
    out.append(q_text.data(), q_text.size());
    out.push_back('"');
    out.push_back('}');
    out.push_back(' ');
    AppendDouble(out, value);
    out.push_back('\n');
}

/// 把 double 格式化成短文本（复用 `to_chars`，栈缓冲，无分配）。
std::size_t FormatDoubleInto(double value, char* buffer, std::size_t capacity) {
    if (std::isnan(value)) {
        buffer[0] = 'N';
        buffer[1] = 'a';
        buffer[2] = 'N';
        return 3;
    }
    if (std::isinf(value)) {
        if (value > 0.0) {
            buffer[0] = '+';
            buffer[1] = 'I';
            buffer[2] = 'n';
            buffer[3] = 'f';
        } else {
            buffer[0] = '-';
            buffer[1] = 'I';
            buffer[2] = 'n';
            buffer[3] = 'f';
        }
        return 4;
    }
    const auto result = std::to_chars(buffer, buffer + capacity, value,
                                      std::chars_format::general);
    if (result.ec != std::errc{}) {
        buffer[0] = '0';
        return 1;
    }
    return static_cast<std::size_t>(result.ptr - buffer);
}

// ---------------------------------------------------------------------------
// 文本解析：只服务 `AppendQuantileFamilies`，不对外暴露。
// ---------------------------------------------------------------------------

/// 一行 histogram bucket 解析后的标签信息。
struct ParsedLabels {
    std::string_view rest;  ///< 不含 `le` 的标签串（视图，指回输入行）
    std::string_view le;    ///< `le` 的原始文本（如 `0.5` / `+Inf`）
    bool rest_owned{false};    ///< `rest` 指向 `owned`（慢路径重组结果）
    bool has_quantile{false};  ///< 原标签里已有 `quantile`（输出会冲突）
    bool ok{false};            ///< 解析是否成功
};

/// `ParseLabels` 的其余标签栈上缓冲（超过时回退到动态容器）。
constexpr std::size_t kInlineLabelCount = 8;

/**
 * @brief 解析 `{...}` 内部的标签列表。
 *
 * 扫描方式对引号内内容保持原样（值里的 `,` / `}` / `=` 不会误判），
 * 只识别转义 `\\` 与 `\"`。`le` 是**最后一段**时走零拷贝快路径；
 * 否则把其余标签重新拼接进 `owned`（慢路径，非本项目导出形态）。
 */
ParsedLabels ParseLabels(std::string_view content, std::string& owned) {
    ParsedLabels parsed;
    std::size_t le_begin = 0;
    std::size_t le_end = 0;
    bool has_le = false;

    std::string_view inline_others[kInlineLabelCount];
    std::vector<std::string_view> overflow;
    std::size_t other_count = 0;

    std::size_t pos = 0;
    while (pos < content.size()) {
        const std::size_t name_begin = pos;
        while (pos < content.size() && content[pos] != '=') {
            ++pos;
        }
        if (pos >= content.size()) {
            return parsed;
        }
        const std::string_view name =
            content.substr(name_begin, pos - name_begin);
        ++pos;  // 跳过 '='
        if (pos >= content.size() || content[pos] != '"') {
            return parsed;
        }
        ++pos;  // 跳过开引号
        const std::size_t value_begin = pos;
        while (pos < content.size()) {
            const char ch = content[pos];
            if (ch == '\\') {
                pos += 2;  // 跳过被转义的字符（含 \" 与 \\）
                continue;
            }
            if (ch == '"') {
                break;
            }
            ++pos;
        }
        if (pos >= content.size()) {
            return parsed;
        }
        const std::string_view value =
            content.substr(value_begin, pos - value_begin);
        const std::size_t segment_end = pos + 1;  // 含闭引号

        if (name == "le") {
            has_le = true;
            le_begin = name_begin;
            le_end = segment_end;
            parsed.le = value;
        } else {
            if (name == "quantile") {
                parsed.has_quantile = true;
            }
            const std::string_view segment =
                content.substr(name_begin, segment_end - name_begin);
            if (other_count < kInlineLabelCount) {
                inline_others[other_count] = segment;
            } else {
                overflow.push_back(segment);
            }
            ++other_count;
        }

        ++pos;  // 跳过闭引号
        if (pos < content.size()) {
            if (content[pos] != ',') {
                return parsed;
            }
            ++pos;  // 跳过逗号
        }
    }

    if (!has_le) {
        return parsed;
    }

    // 快路径：`le` 是最后一段 -> 其余标签就是 content 的前缀（去掉末尾逗号）。
    if (le_end == content.size()) {
        std::string_view rest = content.substr(0, le_begin);
        if (!rest.empty() && rest.back() == ',') {
            rest.remove_suffix(1);
        }
        parsed.rest = rest;
        parsed.ok = true;
        return parsed;
    }

    // 慢路径：`le` 在中间（不是本项目的导出形态），重新拼接其余标签。
    owned.clear();
    for (std::size_t i = 0; i < other_count; ++i) {
        const std::string_view segment =
            i < kInlineLabelCount ? inline_others[i] : overflow[i - kInlineLabelCount];
        if (i != 0) {
            owned.push_back(',');
        }
        owned.append(segment.data(), segment.size());
    }
    parsed.rest = owned;
    parsed.rest_owned = true;
    parsed.ok = true;
    return parsed;
}

/**
 * @brief 在 `content` 中找与 `{` 配对的 `}`（跳过引号内的 `}`）。
 * @return `}` 的下标；找不到返回 `std::string_view::npos`。
 */
std::size_t FindClosingBrace(std::string_view content, std::size_t open_pos) {
    bool in_quote = false;
    for (std::size_t i = open_pos + 1; i < content.size(); ++i) {
        const char ch = content[i];
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

/// 解析 `le` 文本：数字走 `from_chars`，`+Inf` 走特判。
bool ParseBound(std::string_view text, double& value) {
    if (text == "+Inf" || text == "Inf" || text == "inf") {
        value = kPosInf;
        return true;
    }
    if (text == "-Inf" || text == "-inf") {
        value = kNegInf;
        return true;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                        value, std::chars_format::general);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

/// 解析行尾的累计计数。
bool ParseCount(std::string_view text, std::uint64_t& value) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{};
}

/// 一个直方图序列的桶：有限上界 + 累计计数（末项是 +Inf）。
struct BucketSeries {
    std::string labels;  ///< 原标签串（不含 `le`；已转义，直接回写）
    std::vector<double> bounds;             ///< 有限桶上界，升序
    std::vector<std::uint64_t> cumulative;  ///< 累计计数，长度 = bounds+1
    bool has_quantile_label{false};         ///< 原标签含 `quantile`，跳过输出
};

/// 一个 histogram 指标族。
struct HistogramFamily {
    std::string_view name;             ///< 指标名（不含 _bucket）
    std::vector<BucketSeries> series;  ///< 按出现顺序；同一标签组合只出现一次
    /// 标签串 -> series 下标。桶行按 series 连续出现，这里再缓存「上一行」，
    /// 让同一序列的 10 个桶只做一次哈希查找（否则退化成 O(series^2)）。
    std::unordered_map<std::string, std::size_t> index;
    std::size_t last_series{static_cast<std::size_t>(-1)};
};

constexpr std::size_t kInvalidSeries = static_cast<std::size_t>(-1);

/// 比较存下来的标签串与当前行的标签视图（避免 string_view 转换）。
bool SameLabels(const std::string& stored, std::string_view candidate) {
    return stored.size() == candidate.size() &&
           std::memcmp(stored.data(), candidate.data(), candidate.size()) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// 公共写入口
// ---------------------------------------------------------------------------

void AppendDouble(std::string& out, double value) {
    if (std::isnan(value)) {
        out.append("NaN", 3);
        return;
    }
    if (std::isinf(value)) {
        out.append(value > 0.0 ? "+Inf" : "-Inf", 4);
        return;
    }
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::general);
    if (result.ec != std::errc{}) {
        out.push_back('0');
        return;
    }
    out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

void AppendUint(std::string& out, std::uint64_t value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) {
        out.push_back('0');
        return;
    }
    out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

void AppendEscapedLabelValue(std::string& out, std::string_view value) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch != '\\' && ch != '"' && ch != '\n') {
            continue;
        }
        out.append(value.data() + start, i - start);
        switch (ch) {
            case '\\':
                out.append("\\\\", 2);
                break;
            case '"':
                out.append("\\\"", 2);
                break;
            default:
                out.append("\\n", 2);
                break;
        }
        start = i + 1;
    }
    out.append(value.data() + start, value.size() - start);
}

void AppendEscapedHelp(std::string& out, std::string_view help) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < help.size(); ++i) {
        const char ch = help[i];
        if (ch != '\\' && ch != '\n') {
            continue;
        }
        out.append(help.data() + start, i - start);
        out.append(ch == '\\' ? "\\\\" : "\\n", 2);
        start = i + 1;
    }
    out.append(help.data() + start, help.size() - start);
}

// ---------------------------------------------------------------------------
// TextWriter
// ---------------------------------------------------------------------------

void TextWriter::Header(std::string_view name, std::string_view help,
                        MetricType type, bool include_help) {
    std::string& out = *m_out;
    if (include_help) {
        out.append("# HELP ", kHeaderKeywordSize);
        out.append(name.data(), name.size());
        out.push_back(' ');
        AppendEscapedHelp(out, help);
        out.push_back('\n');
    }
    out.append("# TYPE ", kHeaderKeywordSize);
    out.append(name.data(), name.size());
    out.push_back(' ');
    out.append(MetricTypeName(type));
    out.push_back('\n');
}

void TextWriter::Sample(std::string_view name, const LabelView& labels,
                        std::uint64_t value) {
    std::string& out = *m_out;
    AppendNameAndLabels(out, name, labels);
    out.push_back(' ');
    AppendUint(out, value);
    out.push_back('\n');
}

void TextWriter::Sample(std::string_view name, const LabelView& labels,
                        double value) {
    std::string& out = *m_out;
    AppendNameAndLabels(out, name, labels);
    out.push_back(' ');
    AppendDouble(out, value);
    out.push_back('\n');
}

void TextWriter::Histogram(std::string_view name, const LabelView& labels,
                           const double* bounds, std::size_t bound_count,
                           const std::uint64_t* cumulative, double sum,
                           std::uint64_t count) {
    std::string& out = *m_out;
    char le_buffer[64];
    for (std::size_t i = 0; i < bound_count; ++i) {
        const std::size_t le_size =
            FormatDoubleInto(bounds[i], le_buffer, sizeof(le_buffer));
        AppendBucketSample(out, name, labels,
                           std::string_view(le_buffer, le_size), cumulative[i]);
    }
    AppendBucketSample(out, name, labels, std::string_view("+Inf", 4),
                       cumulative[bound_count]);

    out.append(name.data(), name.size());
    out.append("_sum", kSumSuffixSize);
    AppendLabelSet(out, labels);
    out.push_back(' ');
    AppendDouble(out, sum);
    out.push_back('\n');

    out.append(name.data(), name.size());
    out.append("_count", kCountSuffixSize);
    AppendLabelSet(out, labels);
    out.push_back(' ');
    AppendUint(out, count);
    out.push_back('\n');
}

void TextWriter::QuantileHeader(std::string_view base_name) {
    AppendQuantileHeader(*m_out, base_name);
}

void TextWriter::QuantileSample(std::string_view base_name,
                                const LabelView& labels, double quantile,
                                double value) {
    std::string& out = *m_out;
    char q_buffer[32];
    const std::size_t q_size =
        FormatDoubleInto(quantile, q_buffer, sizeof(q_buffer));

    out.append(base_name.data(), base_name.size());
    out.append("_quantile", kQuantileSuffixSize);
    out.push_back('{');
    bool first = true;
    for (const Label& label : labels) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out.append(label.name.data(), label.name.size());
        out.push_back('=');
        out.push_back('"');
        AppendEscapedLabelValue(out, label.value);
        out.push_back('"');
    }
    if (!first) {
        out.push_back(',');
    }
    out.append("quantile=\"", 10);
    out.append(q_buffer, q_size);
    out.push_back('"');
    out.push_back('}');
    out.push_back(' ');
    AppendDouble(out, value);
    out.push_back('\n');
}

// ---------------------------------------------------------------------------
// 分位数
// ---------------------------------------------------------------------------

double BucketQuantile(const double* bounds, std::size_t bound_count,
                      const std::uint64_t* cumulative, std::uint64_t count,
                      double q) {
    if (std::isnan(q)) {
        return kNaN;
    }
    if (q < 0.0) {
        return kNegInf;
    }
    if (q > 1.0) {
        return kPosInf;
    }
    if (count == 0 || bound_count == 0 || bounds == nullptr ||
        cumulative == nullptr) {
        return kNaN;
    }

    const double rank = q * static_cast<double>(count);
    std::size_t index = 0;
    // cumulative 的有效下标是 [0, bound_count]；最后一个桶是隐式 +Inf。
    while (index <= bound_count &&
           static_cast<double>(cumulative[index]) < rank) {
        ++index;
    }
    if (index > bound_count) {
        index = bound_count;
    }

    // 目标落在隐式 +Inf 桶：Prometheus 的经典行为是返回最后一个有限上界。
    if (index == bound_count) {
        return bounds[bound_count - 1];
    }

    const double upper = bounds[index];
    // Prometheus 假设第一个桶的下界是 0；若桶上界本身为负，则退化为该上界，
    // 避免出现 lower > upper 的负宽度插值。
    const double lower = index == 0 ? std::min(0.0, upper) : bounds[index - 1];
    const std::uint64_t prior = index == 0 ? 0 : cumulative[index - 1];
    const std::uint64_t bucket_count = cumulative[index] - prior;
    if (bucket_count == 0) {
        return lower;
    }
    const double fraction =
        (rank - static_cast<double>(prior)) / static_cast<double>(bucket_count);
    return lower + fraction * (upper - lower);
}

// ---------------------------------------------------------------------------
// 文本后处理
// ---------------------------------------------------------------------------

std::size_t AppendQuantileFamilies(std::string_view exposition,
                                   const ExporterOptions& options,
                                   std::string& out) {
    if (options.quantiles.empty() || exposition.empty()) {
        return 0;
    }

    // step1: 收集所有 histogram 指标名（# TYPE <name> histogram）。
    std::vector<HistogramFamily> families;
    {
        std::size_t line_begin = 0;
        while (line_begin < exposition.size()) {
            const std::size_t line_end = exposition.find('\n', line_begin);
            const std::size_t end = line_end == std::string_view::npos
                                        ? exposition.size()
                                        : line_end;
            const std::string_view line =
                exposition.substr(line_begin, end - line_begin);
            line_begin = end + 1;

            if (line.size() <= kHeaderKeywordSize ||
                line.compare(0, kHeaderKeywordSize, "# TYPE ") != 0) {
                continue;
            }
            const std::string_view rest = line.substr(kHeaderKeywordSize);
            const std::size_t space = rest.find(' ');
            if (space == std::string_view::npos ||
                rest.substr(space + 1) != "histogram") {
                continue;
            }
            HistogramFamily family;
            family.name = rest.substr(0, space);
            families.push_back(std::move(family));
        }
    }
    if (families.empty()) {
        return 0;
    }

    // step2: 扫描 `<name>_bucket{...} value` 行，按序列累积桶。
    {
        std::size_t line_begin = 0;
        while (line_begin < exposition.size()) {
            const std::size_t line_end = exposition.find('\n', line_begin);
            const std::size_t end = line_end == std::string_view::npos
                                        ? exposition.size()
                                        : line_end;
            const std::string_view line =
                exposition.substr(line_begin, end - line_begin);
            line_begin = end + 1;

            const std::size_t brace = line.find('{');
            if (brace == std::string_view::npos || brace < kBucketSuffixSize) {
                continue;
            }
            if (line.compare(brace - kBucketSuffixSize, kBucketSuffixSize,
                             "_bucket") != 0) {
                continue;
            }
            const std::string_view base_name =
                line.substr(0, brace - kBucketSuffixSize);

            HistogramFamily* family = nullptr;
            for (auto& candidate : families) {
                if (candidate.name == base_name) {
                    family = &candidate;
                    break;
                }
            }
            if (family == nullptr) {
                continue;
            }

            const std::size_t close = FindClosingBrace(line, brace);
            if (close == std::string_view::npos) {
                continue;
            }
            std::uint64_t value = 0;
            if (!ParseCount(line.substr(close + 1), value)) {
                continue;
            }

            std::string owned;
            const ParsedLabels parsed = ParseLabels(
                line.substr(brace + 1, close - brace - 1), owned);
            if (!parsed.ok) {
                continue;
            }
            double bound = 0.0;
            if (!ParseBound(parsed.le, bound)) {
                continue;
            }
            const bool is_inf = std::isinf(bound);
            if (!is_inf && !std::isfinite(bound)) {
                continue;
            }

            // 找到/新建该标签组合的序列。快路径：与上一行同一序列（桶连续）。
            BucketSeries* series = nullptr;
            if (family->last_series != kInvalidSeries &&
                SameLabels(family->series[family->last_series].labels,
                           parsed.rest)) {
                series = &family->series[family->last_series];
            } else {
                const std::string key(parsed.rest.data(), parsed.rest.size());
                const auto it = family->index.find(key);
                if (it != family->index.end()) {
                    family->last_series = it->second;
                    series = &family->series[it->second];
                } else {
                    BucketSeries fresh;
                    fresh.labels = key;
                    fresh.has_quantile_label = parsed.has_quantile;
                    const std::size_t index = family->series.size();
                    family->series.push_back(std::move(fresh));
                    family->index.emplace(key, index);
                    family->last_series = index;
                    series = &family->series.back();
                }
            }
            if (is_inf) {
                // 只有「有限桶都已出现」时才接受 +Inf 桶，保证 cumulative 连续。
                if (series->cumulative.size() != series->bounds.size()) {
                    continue;
                }
                series->cumulative.push_back(value);
            } else {
                if (series->cumulative.size() != series->bounds.size()) {
                    // 已经出现过 +Inf 桶，再来的有限桶说明输入乱序：丢弃该行。
                    continue;
                }
                series->bounds.push_back(bound);
                series->cumulative.push_back(value);
            }
        }
    }

    // step3: 计算分位数并追加到独立缓冲（避免边读边写原文本）。
    std::size_t series_total = 0;
    for (const auto& family : families) {
        series_total += family.series.size();
    }
    if (series_total == 0) {
        return 0;
    }

    std::string addition;
    addition.reserve(series_total * options.quantiles.size() * 64 + 256);

    char q_buffer[32];
    for (const auto& family : families) {
        if (family.series.empty()) {
            continue;
        }
        // 若所有序列都因为标签冲突/输入截断被跳过，就不要输出只有 HELP/TYPE
        // 没有样本的族（Prometheus 会把它当成空族，且没有意义）。
        bool has_output = false;
        for (const auto& series : family.series) {
            if (!series.has_quantile_label &&
                series.cumulative.size() == series.bounds.size() + 1) {
                has_output = true;
                break;
            }
        }
        if (!has_output) {
            continue;
        }
        AppendQuantileHeader(addition, family.name);
        for (const auto& series : family.series) {
            if (series.has_quantile_label) {
                continue;  // 原标签已占用 quantile，重复标签会让抓取端拒收
            }
            // 输入被截断（没有 +Inf 桶）时跳过，避免越界读 cumulative。
            if (series.cumulative.size() != series.bounds.size() + 1) {
                continue;
            }
            const std::uint64_t count = series.cumulative.back();
            for (const double q : options.quantiles) {
                const double value =
                    BucketQuantile(series.bounds.data(), series.bounds.size(),
                                   series.cumulative.data(), count, q);
                const std::size_t q_size =
                    FormatDoubleInto(q, q_buffer, sizeof(q_buffer));
                AppendQuantileSampleRaw(addition, family.name, series.labels,
                                        std::string_view(q_buffer, q_size),
                                        value);
            }
        }
    }

    if (addition.empty()) {
        return 0;
    }
    const std::size_t before = out.size();
    if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
    }
    out.append(addition);
    return out.size() - before;
}

// ---------------------------------------------------------------------------
// Exporter
// ---------------------------------------------------------------------------

std::string Exporter::Render(const ExporterOptions& options) const {
    std::string text = m_registry->Render();
    if (!options.include_quantiles || options.quantiles.empty()) {
        return text;
    }
    AppendQuantileFamilies(text, options, text);
    return text;
}

std::string Render(const Registry& registry, const ExporterOptions& options) {
    return Exporter(registry).Render(options);
}

}  // namespace prom
}  // namespace metrics
}  // namespace go2cpp
