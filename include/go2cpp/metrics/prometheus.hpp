/**
 * @file prometheus.hpp
 * @brief Prometheus 文本格式导出：`# HELP` / `# TYPE` / 样本、标签转义、
 *        histogram 分位数（p50 / p90 / p99）。
 *
 * @details 本文件在 `metrics.hpp`（Counter / Gauge / Histogram）之上补一层
 * **展示格式**，不引入任何第三方 Prometheus 客户端依赖：
 *
 *   - **文本写入器**（`TextWriter`）：所有输出直接追加到调用方给的
 *     `std::string`，一次 `reserve` + 纯 `append` + `std::to_chars`，
 *     不构造中间 `std::string`、不用 `ostringstream`、不做 locale 转换；
 *   - **导出器**（`Exporter`）：把 `metrics::Registry` 渲染成可被 Prometheus
 *     抓取的文本；histogram 额外输出 `<name>_quantile{quantile="0.5"}` 三行
 *     （p50/p90/p99），分位数由桶内线性插值估算，语义与
 *     `Histogram::Quantile()` 完全一致；
 *   - **分位数后处理**（`AppendQuantileFamilies`）：对任意符合 Prometheus
 *     文本格式的子集（`# TYPE <name> histogram` + `<name>_bucket{...,le="…"}`）
 *     追加分位数族，不依赖 `Registry` 实例。
 *
 * ## 为什么分位数单独成一个指标族
 * Prometheus 的文本格式里，同一个指标名只能有一种 `TYPE`。Histogram 的
 * `_bucket` / `_sum` / `_count` 已经占用了 `<name>` 这个族；把
 * `<name>{quantile="0.5"}` 直接混进去会让抓取端当作两种类型冲突而拒收。
 * 因此本模块把分位数导出成独立族 `<name>_quantile`（`TYPE gauge`），
 * 标签为「原标签 + quantile」。这也是 OTel / 各类 exporter 的常见约定。
 *
 * ## 文本格式要点（Prometheus exposition format 0.0.4）
 * ```
 * # HELP http_requests_total Total HTTP requests.
 * # TYPE http_requests_total counter
 * http_requests_total{method="GET",path="/api"} 42
 * # HELP rpc_latency_seconds RPC latency.
 * # TYPE rpc_latency_seconds histogram
 * rpc_latency_seconds_bucket{le="0.005"} 3
 * ...
 * rpc_latency_seconds_bucket{le="+Inf"} 10
 * rpc_latency_seconds_sum 0.93
 * rpc_latency_seconds_count 10
 * rpc_latency_seconds_quantile{quantile="0.5"} 0.043
 * ```
 *
 * 转义规则（与规范一致）：
 *   - 标签值转义 `\\`、`\"`、`\n`；
 *   - HELP 转义 `\\`、`\n`（HELP 里的 `"` 不需要转义）；
 *   - 浮点：`NaN` / `+Inf` / `-Inf`，其余用 `std::to_chars` 的最短往返表示。
 *
 * ## 阅读建议
 * 先看 `TextWriter` 的几个 `Append*` 辅助（转义与数值格式化的边界），
 * 再看 `AppendQuantileFamilies` 的文本解析（只认自己导出的那一种形态），
 * 最后看 `Exporter::Render` 的两步：`Registry::Render()` + 分位数追加。
 *
 * ## 边界与陷阱
 * - `AppendQuantileFamilies(exposition, options, out)` 允许 `out` 与
 *   `exposition` 是同一个 `std::string`（内部先构建增量再 append），
 *   但**不允许** `exposition` 指向 `out` 之外一块会被同时修改的缓冲；
 * - 文本解析只识别「本模块 / `metrics::Registry` 导出的形态」：`le` 必须在
 *   标签列表末尾、每个序列的桶必须按升序连续出现；带 timestamp 或乱序桶的
 *   第三方文本不在支持范围（会跳过该序列，不产生错误输出）；
 * - 分位数是**估算值**（桶内线性插值），误差上界约一个桶宽；
 * - 标签值里出现 `\r` / 其他控制字符不会被转义 —— 与 `metrics.hpp` 的
 *   最小转义保持一致，抓取端可能拒收整行，调用方应保证标签值可打印。
 */

#pragma once

#include "go2cpp/metrics/metrics.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp {
namespace metrics {
namespace prom {

/** @brief 一个标签：名字与值都是借用视图，调用方保证生命周期覆盖写入调用。 */
struct Label {
    std::string_view name;   ///< 标签名（须是合法 `[a-zA-Z_][a-zA-Z0-9_]*`）
    std::string_view value;  ///< 标签值（写入时按规范转义）
};

/**
 * @brief 标签集合的只读视图（指针 + 长度），不拥有标签本身。
 *
 * 用视图而不是 `std::vector<Label>`，是为了让热路径（每次抓取上百万样本）
 * 不产生任何容器构造 / 析构开销；调用方可以复用同一组 `Label`。
 */
class LabelView {
public:
    /** @brief 空标签集。 */
    constexpr LabelView() noexcept = default;

    /** @brief 指向 `count` 个连续 `Label` 的视图。 */
    constexpr LabelView(const Label* labels, std::size_t count) noexcept
        : m_data(labels), m_size(count) {}

    /**
     * @brief 从初始化列表构造视图。
     *
     * 不提供 `LabelView(std::initializer_list)` 隐式构造：initializer_list 的
     * 底层数组生命周期只到完整表达式结束，把它存进构造函数会触发
     * `-Winit-list-lifetime`（GCC）。`From()` 只在该表达式内使用视图，明确
     * 表达「临时标签集合」的语义。
     */
    static LabelView From(std::initializer_list<Label> labels) noexcept {
        return LabelView(labels.begin(), labels.size());
    }

    const Label* begin() const noexcept { return m_data; }
    const Label* end() const noexcept { return m_data + m_size; }
    std::size_t size() const noexcept { return m_size; }
    bool empty() const noexcept { return m_size == 0; }
    const Label& operator[](std::size_t index) const noexcept {
        return m_data[index];
    }

private:
    const Label* m_data{nullptr};  ///< 标签数组首地址；空视图为 nullptr
    std::size_t m_size{0};         ///< 标签数量；与 m_data 同时有效
};

/**
 * @brief 导出选项。
 *
 * 默认值面向「直接挂到 /metrics」：输出 HELP、输出 histogram 分位数
 * p50/p90/p99。需要和其他 exporter 对齐时可以只改 `quantiles`。
 */
struct ExporterOptions {
    /// 是否从 histogram 桶追加 `<name>_quantile` 分位数族。
    bool include_quantiles{true};
    /// 分位数列表，取值 (0, 1)；默认 p50 / p90 / p99。
    std::vector<double> quantiles{0.5, 0.9, 0.99};
};

/**
 * @brief 把 `double` 按 Prometheus 文本格式写入 `out`。
 *
 * `NaN` → `NaN`，`+Inf` → `+Inf`，`-Inf` → `-Inf`；其余用
 * `std::to_chars(..., chars_format::general)` 的最短往返表示，无 locale 依赖。
 */
void AppendDouble(std::string& out, double value);

/** @brief 把无符号整数写入 `out`（`std::to_chars`，十进制）。 */
void AppendUint(std::string& out, std::uint64_t value);

/** @brief 按规范转义标签值（`\\`、`\"`、`\n`）并写入 `out`，不含外层引号。 */
void AppendEscapedLabelValue(std::string& out, std::string_view value);

/** @brief 按规范转义 HELP 文本（`\\`、`\n`）并写入 `out`。 */
void AppendEscapedHelp(std::string& out, std::string_view help);

/**
 * @brief 由累计桶计数估算 q 分位数（与 `Histogram::Quantile()` 同语义）。
 *
 * @param bounds      有限桶上界，严格升序，长度 `bound_count`。
 * @param bound_count 有限桶数量（不含隐式 `+Inf`）。
 * @param cumulative  累计桶计数，长度 `bound_count + 1`（末项是 `+Inf` 桶）。
 * @param count       样本总数（应等于 `cumulative[bound_count]`）。
 * @param q           分位数，取值 [0, 1]。
 *
 * 返回值语义：
 *   - `q` 为 NaN → NaN；`q < 0` → `-Inf`；`q > 1` → `+Inf`；
 *   - `count == 0` → NaN（空直方图）；
 *   - 目标落在 `+Inf` 桶 → 最后一个有限上界（Prometheus 经典行为）；
 *   - 落在第一个桶 → 下界取 `min(0, bounds[0])`，避免负宽度；
 *   - 桶内线性插值；空桶退化为该桶下界。
 */
double BucketQuantile(const double* bounds, std::size_t bound_count,
                      const std::uint64_t* cumulative, std::uint64_t count,
                      double q);

/**
 * @brief 直接拼进 `std::string` 的 Prometheus 文本写入器。
 *
 * 所有方法都只做 `append`，不分配中间字符串；一次导出应复用同一个
 * `TextWriter`。构造时传入的输出缓冲由调用方持有，`TextWriter` 不拥有它。
 *
 * @code
 * std::string out;
 * out.reserve(1 << 16);
 * prom::TextWriter writer(out);
 * writer.Header("http_requests_total", "Total HTTP requests.", MetricType::kCounter);
 * const prom::Label tags[] = {{"method", "GET"}};
 * writer.Sample("http_requests_total", prom::LabelView(tags, 1),
 *               static_cast<std::uint64_t>(42));
 * @endcode
 */
class TextWriter {
public:
    /** @brief 绑定输出缓冲；`TextWriter` 不接管所有权。 */
    explicit TextWriter(std::string& out) noexcept : m_out(&out) {}

    TextWriter(const TextWriter&) = delete;
    TextWriter& operator=(const TextWriter&) = delete;

    /** @brief 预留输出容量（只在需要时调用，避免重复扩容）。 */
    void Reserve(std::size_t bytes) { m_out->reserve(bytes); }

    /**
     * @brief 写 `# HELP` / `# TYPE` 两行。
     * @param include_help 为 false 时只写 `# TYPE`（HELP 可省略）。
     */
    void Header(std::string_view name, std::string_view help, MetricType type,
                bool include_help = true);

    /** @brief 写一条整数样本：`name{labels} value`。 */
    void Sample(std::string_view name, const LabelView& labels,
                std::uint64_t value);

    /** @brief 写一条浮点样本：`name{labels} value`。 */
    void Sample(std::string_view name, const LabelView& labels, double value);

    /**
     * @brief 写一个 histogram 序列的完整展开（`_bucket` / `_sum` / `_count`）。
     *
     * @param name        指标名（不含 `_bucket` 等后缀）。
     * @param labels      原标签（不含 `le`）。
     * @param bounds      有限桶上界，升序，长度 `bound_count`。
     * @param bound_count 有限桶数量。
     * @param cumulative  累计桶计数，长度 `bound_count + 1`（末项 `+Inf`）。
     * @param sum         样本和。
     * @param count       样本数。
     */
    void Histogram(std::string_view name, const LabelView& labels,
                   const double* bounds, std::size_t bound_count,
                   const std::uint64_t* cumulative, double sum,
                   std::uint64_t count);

    /** @brief 写分位数族的 HELP/TYPE（`<base>_quantile`，类型 gauge）。 */
    void QuantileHeader(std::string_view base_name);

    /** @brief 写一条分位数样本：`<base>_quantile{labels,quantile="0.5"} v`。 */
    void QuantileSample(std::string_view base_name, const LabelView& labels,
                        double quantile, double value);

    /** @brief 底层输出缓冲（只读）。 */
    const std::string& buffer() const noexcept { return *m_out; }

    /** @brief 底层输出缓冲（可写，供调用方追加自定义行）。 */
    std::string& buffer() noexcept { return *m_out; }

    /** @brief 已写入字节数。 */
    std::size_t size() const noexcept { return m_out->size(); }

private:
    std::string* m_out;  ///< 输出缓冲；由调用方持有，生命周期必须覆盖 writer
};

/**
 * @brief 从 Prometheus 文本中提取 histogram 桶并追加 `<name>_quantile` 族。
 *
 * 输入要求（与 `metrics::Registry::Render()` 的输出一致）：
 *   - `# TYPE <name> histogram` 行声明类型；
 *   - `<name>_bucket{<labels>,le="<bound>"} <cumulative>` 行，按 `le` 升序，
 *     末尾必须带 `le="+Inf"`；
 *   - `<name>_sum` / `<name>_count` 行可选（本函数只用桶）。
 *
 * 追加内容（每个 histogram 一个族）：
 * ```
 * # HELP <name>_quantile Estimated quantiles derived from histogram buckets.
 * # TYPE <name>_quantile gauge
 * <name>_quantile{<labels>,quantile="0.5"} 0.043
 * ```
 *
 * @param exposition 输入文本（可以是 `out` 本身）。
 * @param options    导出选项（`quantiles` 决定输出哪些分位点）。
 * @param out        追加目标；与 `exposition` 同一对象也安全。
 * @return 追加的字节数；没有 histogram 时返回 0。
 */
std::size_t AppendQuantileFamilies(std::string_view exposition,
                                   const ExporterOptions& options,
                                   std::string& out);

/**
 * @brief 把 `metrics::Registry` 导出为 Prometheus 文本。
 *
 * 实现分两步：
 *   1. `registry.Render()` 得到标准文本（Counter / Gauge / Histogram 的
 *      `_bucket` / `_sum` / `_count`）；
 *   2. 若 `options.include_quantiles`，用 `AppendQuantileFamilies()` 在末尾
 *      追加 `<name>_quantile` 分位数族。
 *
 * 线程安全：只读注册表；每次 `Render()` 使用独立缓冲，可被多个线程并发调用
 * （`Registry::Render()` 内部持锁，返回的文本是某个时刻的一致快照）。
 */
class Exporter {
public:
    /** @brief 绑定注册表；注册表必须比 exporter 活得久。 */
    explicit Exporter(const Registry& registry) noexcept : m_registry(&registry) {}

    /** @brief 导出为文本；同一 exporter 可并发调用。 */
    std::string Render(const ExporterOptions& options = {}) const;

    /** @brief 绑定的注册表。 */
    const Registry& registry() const noexcept { return *m_registry; }

private:
    const Registry* m_registry;  ///< 数据源；exporter 不拥有它
};

/** @brief `Exporter(registry).Render(options)` 的便捷写法。 */
std::string Render(const Registry& registry, const ExporterOptions& options = {});

}  // namespace prom
}  // namespace metrics
}  // namespace go2cpp
