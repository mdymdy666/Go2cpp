/**
 * @file metrics.hpp
 * @brief `go2cpp::metrics` 模块：Counter / Gauge / Histogram 与 Prometheus 文本导出。
 *
 * @details 本模块只依赖 C++17 标准库，不引入 Prometheus 客户端依赖。设计目标：
 *
 *   - **计数绝对不丢**：`Counter` 用 `std::atomic<uint64_t>` 的 `fetch_add`，
 *     多线程 / 多 Fiber 并发 `Inc()` 后终值精确等于调用次数；
 *   - **标签缓存**：同一组标签值只创建一个子指标，`With()` 返回稳定引用，
 *     不会在每次请求时分配新对象；
 *   - **固定桶直方图**：桶边界创建后不可变，桶计数与总和均为原子读写，
 *     `Quantile()` 按 Prometheus `histogram_quantile` 的桶内线性插值估算；
 *   - **标准文本格式**：`Registry::Render()` 输出 `# HELP` / `# TYPE` /
 *     `name{label="value"} value`，可被 Prometheus 文本解析器直接读取。
 *
 *   典型用法：
 *   @code
 *   auto& registry = go2cpp::metrics::Registry::Default();
 *   auto& requests = registry.GetCounter(
 *       "http_requests_total", "HTTP requests", {"method", "path"});
 *   requests.With({"GET", "/api"}).Inc();
 *
 *   auto& latency = registry.GetHistogram(
 *       "http_request_duration_seconds", "Request latency",
 *       {"method"}, {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5});
 *   latency.With({"GET"}).Observe(0.023);
 *
 *   std::string text = registry.Render();
 *   @endcode
 *
 *   **标签族语义**：带标签的名字在注册表里代表一个“指标族”，根对象只用于
 *   `With()` 取子序列。对带标签的根对象直接 `Inc()` / `Set()` / `Observe()`
 *   会抛 `std::logic_error`，因为缺少标签值的样本无法按 Prometheus 文本格式
 *   导出；必须先用 `With()` 指定完整标签。无标签指标则可以直接操作根对象。
 *
 *   **NaN 语义**：`Histogram::Observe(NaN)` 被忽略（不改变 Count/Sum/桶计数），
 *   避免一个坏样本污染整条分布；`Gauge` 可以保存 NaN，`Set(NaN)` 后
 *   `Inc()`/`Dec()` 仍可安全返回。
 *
 *   **直方图分位数**：桶是固定上界（`le`），另有一个隐式 `+Inf` 桶。分位数
 *   采用 Prometheus 的桶内线性插值；当目标落在 `+Inf` 桶时返回最后一个有限
 *   上界（Prometheus 的经典语义）。桶越细，误差越小；误差上界是一个桶宽。
 *
 * ## 模块定位
 * 轻量指标层：Counter / Gauge / Histogram 与 Prometheus 文本导出。不引入 Prometheus
 * 客户端依赖，只依赖 C++17；被服务端、压测与示例用来暴露运行指标。
 *
 * ## 本文件定义什么
 * - `MetricType` / `MetricTypeName`：三种指标类型与文本名；
 * - `detail::MetricInfo`：指标族共享元数据；
 * - `Metric`（公共基类）与 `Counter` / `Gauge` / `Histogram`；
 * - `Registry`：按名字注册、`Render()` 输出 Prometheus 文本、`Default()` 进程级单例。
 *
 * ## 阅读建议
 * 先看 `Metric` 的标签族语义（根对象只用于 `With()`），再看 `Counter`/`Gauge` 的
 * 原子写法，最后看 `Histogram` 的「为什么分桶」与 `Quantile()` 的插值公式。
 *
 * ## 边界与陷阱
 * - 带标签的**根对象**不能直接 `Inc()`/`Set()`/`Observe()`，会抛 `std::logic_error`；
 *   必须先 `With({...})` 拿到子序列；
 * - `Observe(NaN)` 被忽略（不污染 Sum/分位数）；`Gauge` 允许 NaN；
 * - 文本导出只做最小转义（`\\`、`\"`、`\n`），标签值里的其它控制字符按原样输出；
 * - 注册按名字唯一：同名但类型/help/标签/bucket 不同会抛 `std::invalid_argument`。
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp {
namespace metrics {

/** @brief Prometheus 指标类型。 */
enum class MetricType : std::uint8_t {
    kCounter = 0,   ///< 单调递增计数
    kGauge = 1,     ///< 可增可减的瞬时值
    kHistogram = 2  ///< 固定桶分布
};

/** @brief 返回文本格式中的类型名（counter / gauge / histogram）。 */
inline const char* MetricTypeName(MetricType type) noexcept {
    switch (type) {
        case MetricType::kCounter:
            return "counter";
        case MetricType::kGauge:
            return "gauge";
        case MetricType::kHistogram:
            return "histogram";
    }
    return "untyped";
}

namespace detail {

/**
 * @brief 指标族共享的元数据。
 *
 * 根指标持有该结构，所有标签子指标通过裸指针共享。子指标的生命周期严格
 * 短于根指标（由根指标的 Family 容器拥有），因此不需要 shared_ptr 的
 * 引用计数开销，也不会形成环。
 */
struct MetricInfo {
    std::string name;  ///< 指标名（不含 _bucket/_sum/_count）；注册时固定，Name() 读取
    std::string help;  ///< HELP 文本；注册时固定，Render 输出
    MetricType type{MetricType::kCounter};  ///< 指标类型；注册时固定，决定 Family 的具体类型
    std::vector<std::string> label_names;  ///< 标签名顺序；With() 按此顺序校验取值，Render 输出标签键
};

}  // namespace detail

class Registry;

/**
 * @brief 所有指标族的公共只读接口。
 *
 * `Metric` 不提供 `With()`：三种指标的子指标类型不同（`Counter&` /
 * `Gauge&` / `Histogram&`），C++ 无法用同一个返回类型表达。请使用
 * `Registry::GetCounter` / `GetGauge` / `GetHistogram` 取得具体类型。
 *
 * ## 定位
 * 被 `Registry` 统一持有（`map<string, unique_ptr<Metric>>`），向上暴露名字/help/
 * 类型/标签名；三种具体指标在它之上提供各自的修改接口。
 *
 * ## 不变量
 * - `m_info` 指向所属 Family 的元数据，生命周期与 Family 一致；
 * - `Metric` 不提供 `With()`：三种子指标类型不同，C++ 无法用统一返回类型表达。
 */
class Metric {
public:
    virtual ~Metric() = default;

    Metric(const Metric&) = delete;
    Metric& operator=(const Metric&) = delete;

    /** @brief 指标名（不含 `_bucket` / `_sum` / `_count` 后缀）。 */
    const std::string& Name() const noexcept { return m_info->name; }

    /** @brief 注册时提供的帮助文本。 */
    const std::string& Help() const noexcept { return m_info->help; }

    /** @brief 指标类型。 */
    MetricType Type() const noexcept { return m_info->type; }

    /** @brief 标签名列表，顺序与 `With()` 的取值顺序一致。 */
    const std::vector<std::string>& LabelNames() const noexcept {
        return m_info->label_names;
    }

    /**
     * @brief 当前存在的序列数量。
     *
     * 无标签指标恒为 1；带标签的根指标返回已缓存的标签组合数；子指标为 1。
     * 该接口用于诊断标签缓存是否正确，不参与热路径。
     */
    virtual std::size_t SeriesCount() const = 0;

protected:
    explicit Metric(const detail::MetricInfo* info) noexcept : m_info(info) {}

    /**
     * @brief 把本指标族按 Prometheus 文本格式追加到 out。
     *
     * 由 `Registry::Render()` 调用；派生类实现时负责 HELP/TYPE 与全部序列。
     */
    virtual void AppendPrometheus(std::string& out) const = 0;

    /// 指向 Family 的元数据；Name/Help/Type/LabelNames 都读它。
    const detail::MetricInfo* m_info;

private:
    friend class Registry;
};

/**
 * @brief 单调递增计数器，对应 Prometheus Counter。
 *
 * 所有修改走 `fetch_add(..., memory_order_relaxed)`：计数场景不需要与其他
 * 内存操作建立 happens-before 关系，relaxed 已足以保证“不丢更新”。
 *
 * ## 定位
 * 被业务代码 `Inc()`/`Add()` 累加，被 `Registry::Render()` 导出为 `name value`；
 * 带标签时通过 `With()` 取子计数器。
 *
 * ## 不变量
 * - `m_value` 只做 `fetch_add(relaxed)`：计数不需要 happens-before，只要不丢更新；
 * - 根对象的 `m_is_root == true`；子对象共享 `m_family` 且持有标签值。
 */
class Counter final : public Metric {
public:
    /** @brief 创建无标签计数器（独立使用或测试）。 */
    Counter();

    /** @brief 创建带标签的独立计数器；标签值通过 `With()` 指定。 */
    explicit Counter(std::vector<std::string> label_names);

    ~Counter() override;

    /** @brief 加 1。 */
    void Inc();

    /** @brief 加 n（n 为无符号，语义与 `fetch_add` 一致）。 */
    void Add(std::uint64_t n);

    /** @brief 读取当前值。 */
    std::uint64_t Value() const;

    /**
     * @brief 取得（或创建）一组标签对应的子计数器。
     *
     * 同一组标签值在当前进程内返回**同一个对象**；不同标签组合互不影响。
     * 标签数量或顺序不匹配时抛 `std::invalid_argument`。
     */
    Counter& With(std::initializer_list<std::string_view> labels);
    Counter& With(const std::vector<std::string>& labels);

    std::size_t SeriesCount() const override;

private:
    friend class Registry;

    struct Family;

    explicit Counter(std::unique_ptr<Family> family);
    Counter(Family* family, std::vector<std::string> label_values);

    static std::unique_ptr<Family> CreateFamily(detail::MetricInfo info);

    void AppendPrometheus(std::string& out) const override;
    void EnsureSeries(const char* operation) const;

    /// 根对象拥有的 Family（元数据 + 子序列表）；Counter 析构时释放。
    std::unique_ptr<Family> m_owned_family;
    /// 指向所属 Family；根对象指向自己的 m_owned_family，子序列指向父对象。
    Family* m_family{nullptr};
    /// 本序列的标签值（顺序与 label_names 一致）；根对象为空。
    std::vector<std::string> m_label_values;
    /// 是否为根对象；根对象带标签时禁止直接修改（EnsureSeries 检查）。
    bool m_is_root{false};
    /// 计数值；Inc/Add 只做 fetch_add(relaxed)，Value 读。
    std::atomic<std::uint64_t> m_value{0};
};

/**
 * @brief 可增可减的瞬时值，对应 Prometheus Gauge。
 *
 * 值以 IEEE-754 位模式保存在 `std::atomic<uint64_t>` 中，通过 CAS 循环做
 * 浮点加法。这样在 C++17 下也能实现无锁 `Inc`/`Dec`，且 `Set(NaN)` 之后
 * 的加法不会因为 NaN != NaN 而卡死。
 *
 * ## 定位
 * 被业务代码 `Set()`/`Inc()`/`Dec()`/`Add()`；用于瞬时值（连接数、队列长度）。
 *
 * ## 不变量
 * - 浮点值以 IEEE-754 位模式存 `atomic<uint64_t>`，`Add()` 用 CAS 循环；
 * - `Set(NaN)` 后 `Inc()`/`Dec()` 仍能安全返回（不会因 NaN != NaN 卡死循环）。
 */
class Gauge final : public Metric {
public:
    Gauge();
    explicit Gauge(std::vector<std::string> label_names);
    ~Gauge() override;

    /** @brief 直接设置当前值（允许 +/-Inf 与 NaN）。 */
    void Set(double value);

    /** @brief 加 1。 */
    void Inc();

    /** @brief 减 1。 */
    void Dec();

    /** @brief 加上任意增量（允许负数）。 */
    void Add(double delta);

    /** @brief 读取当前值。 */
    double Value() const;

    Gauge& With(std::initializer_list<std::string_view> labels);
    Gauge& With(const std::vector<std::string>& labels);

    std::size_t SeriesCount() const override;

private:
    friend class Registry;

    struct Family;

    explicit Gauge(std::unique_ptr<Family> family);
    Gauge(Family* family, std::vector<std::string> label_values);

    static std::unique_ptr<Family> CreateFamily(detail::MetricInfo info);

    void AppendPrometheus(std::string& out) const override;
    void EnsureSeries(const char* operation) const;

    /// 根对象拥有的 Family（元数据 + 子序列表）；Gauge 析构时释放。
    std::unique_ptr<Family> m_owned_family;
    /// 指向所属 Family；根对象指向自己的 m_owned_family，子序列指向父对象。
    Family* m_family{nullptr};
    /// 本序列的标签值（顺序与 label_names 一致）；根对象为空。
    std::vector<std::string> m_label_values;
    /// 是否为根对象；根对象带标签时禁止直接修改（EnsureSeries 检查）。
    bool m_is_root{false};
    /// 当前值的 IEEE-754 位模式；Set/Add 用 CAS 循环写，Value 读。
    std::atomic<std::uint64_t> m_bits{0};
};

/**
 * @brief 固定桶直方图，对应 Prometheus Histogram。
 *
 * 桶边界是升序的有限上界，语义为 `le`（小于等于）。所有序列都隐含一个
 * `+Inf` 桶，因此桶数 = `Buckets().size() + 1`。`Observe(NaN)` 被忽略；
 * 负值、+Inf、-Inf 按边界落桶，`Sum()` 如实累计。
 *
 * ## 定位
 * 被业务代码 `Observe()` 记录延迟/大小分布；`Registry::Render()` 展开为
 * `_bucket`（累计）/`_sum`/`_count` 三组序列。
 *
 * ## 为什么不存原始样本、而是固定分桶
 * - **并发成本恒定**：一次 `Observe` 只做「二分找桶 + 桶计数 fetch_add + Sum CAS +
 *   Count fetch_add」，不需要锁、不需要维护有序样本；
 * - **内存有界**：占用 O(buckets)，与请求量无关；存原始样本会随流量无界增长；
 * - **与 Prometheus 文本格式同构**：`le` 桶就是导出格式本身，`Render()` 不需要再聚合；
 * - 代价是分位数只是**估算**（桶内线性插值，误差上界约一个桶宽）—— 对监控足够。
 *
 * ## 不变量
 * - `m_bucket_count == Buckets().size() + 1`（末尾是隐式 `+Inf` 桶）；
 * - `Count()` 等于所有桶计数之和（NaN 不计入）；
 * - 桶边界构造后不可变，`CumulativeBucketCounts()` 单调不减。
 */
/**
 * ## 状态机
 * 指标族/序列的生命周期（根对象 → `With()` 建子序列 → `Observe()` 累计 →
 * `AppendPrometheus()` 导出）见 `docs/state-machines/Histogram.md`。
 * 关键函数：`With`、`Observe`、`CumulativeBucketCounts`、`Quantile`。
 */
class Histogram final : public Metric {
public:
    /**
     * @brief 使用默认桶创建无标签直方图。
     *
     * 默认桶从 Prometheus 惯用的 0.001s 起，向上扩展到 10000，既能覆盖
     * 秒级延迟，也能对 1..1000 这类通用数值保留桶内分辨率。
     */
    Histogram();

    /**
     * @brief 使用自定义桶创建无标签直方图。
     * @param buckets 严格递增的有限上界；不可为空。
     */
    explicit Histogram(std::vector<double> buckets);

    /** @brief 使用自定义桶与标签名创建独立直方图。 */
    Histogram(std::vector<double> buckets, std::vector<std::string> label_names);

    ~Histogram() override;

    /**
     * @brief 观察一个值。
     *
     * NaN 被忽略；其余值（含负数与 +/-Inf）计入 Count、Sum 和对应桶。
     */
    void Observe(double value);

    /** @brief 已观察的样本数（NaN 不计入）。 */
    std::uint64_t Count() const;

    /** @brief 所有已观察值的总和；出现 +Inf/-Inf 后可能为非有限值。 */
    double Sum() const;

    /**
     * @brief 估算 q 分位数（0 <= q <= 1）。
     *
     * 采用 Prometheus 经典桶内线性插值：
     * `lower + (rank - prior) / bucket_count * (upper - lower)`。
     * 目标落在 +Inf 桶时返回最后一个有限上界；空直方图返回 NaN；
     * q < 0 返回 -Inf，q > 1 返回 +Inf。
     */
    double Quantile(double q) const;

    /** @brief 有限桶上界（不含隐式 +Inf）。 */
    const std::vector<double>& Buckets() const;

    /**
     * @brief 各桶的累计计数快照，末尾一项是 +Inf 桶。
     *
     * 返回长度 = `Buckets().size() + 1`；数组单调不减，最后一项等于 Count()
     * （并发写入时是近似快照）。
     */
    std::vector<std::uint64_t> CumulativeBucketCounts() const;

    Histogram& With(std::initializer_list<std::string_view> labels);
    Histogram& With(const std::vector<std::string>& labels);

    std::size_t SeriesCount() const override;

private:
    friend class Registry;

    struct Family;

    explicit Histogram(std::unique_ptr<Family> family);
    Histogram(Family* family, std::vector<std::string> label_values);

    static std::unique_ptr<Family> CreateFamily(detail::MetricInfo info,
                                                std::vector<double> buckets);

    void InitBuckets();
    void AppendPrometheus(std::string& out) const override;
    void EnsureSeries(const char* operation) const;

    /// 根对象拥有的 Family（元数据 + 子序列表）；Histogram 析构时释放。
    std::unique_ptr<Family> m_owned_family;
    /// 指向所属 Family；根对象指向自己的 m_owned_family，子序列指向父对象。
    Family* m_family{nullptr};
    /// 本序列的标签值（顺序与 label_names 一致）；根对象为空。
    std::vector<std::string> m_label_values;
    /// 是否为根对象；根对象带标签时禁止直接修改（EnsureSeries 检查）。
    bool m_is_root{false};
    /// 样本数（NaN 不计）；Observe 递增，Count/Quantile 读。
    std::atomic<std::uint64_t> m_count{0};
    /// 样本和的位模式；Observe 用 AddDouble(CAS) 累加，Sum 读。
    std::atomic<std::uint64_t> m_sum_bits{0};
    /// 各桶计数（含 +Inf 桶）；InitBuckets 分配，Observe 递增。
    std::unique_ptr<std::atomic<std::uint64_t>[]> m_bucket_counts;
    /// 桶数量 = 有限边界数 + 1（隐式 +Inf）；InitBuckets 设置。
    std::size_t m_bucket_count{0};
};

/**
 * @brief 指标注册表；`Default()` 是进程级单例。
 *
 * 注册按名字唯一：同名但类型、help、标签名或直方图桶不同会抛
 * `std::invalid_argument`，避免两个调用方静默写进同一条序列。
 * 注册表本身线程安全，返回的指标引用在注册表生命周期内保持不变。
 *
 * 测试或需要隔离的场景可以构造独立 `Registry`；进程级监控使用
 * `Registry::Default()`。
 *
 * ## 定位
 * 进程级指标注册表：`Default()` 给业务用，测试可构造独立实例做隔离；
 * `Render()` 是 HTTP 指标端点的唯一数据来源。
 *
 * ## 不变量
 * - 注册与名字查重都在 `m_mutex` 内；返回的指标引用在注册表生命周期内稳定；
 * - 同名重复注册时，类型/help/标签名/bucket 任一不同即抛 `invalid_argument`，
 *   避免两个调用方静默写进同一条序列。
 */
class Registry final {
public:
    Registry();
    ~Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    /** @brief 进程级默认注册表。 */
    static Registry& Default();

    /** @brief 注册或取得无标签/带标签 Counter。 */
    Counter& GetCounter(const std::string& name, const std::string& help,
                        std::vector<std::string> label_names = {});

    /** @brief 注册或取得无标签/带标签 Gauge。 */
    Gauge& GetGauge(const std::string& name, const std::string& help,
                    std::vector<std::string> label_names = {});

    /**
     * @brief 注册或取得 Histogram。
     * @param buckets 空 vector 表示使用默认桶；否则必须是严格递增有限值。
     */
    Histogram& GetHistogram(const std::string& name, const std::string& help,
                            std::vector<std::string> label_names = {},
                            std::vector<double> buckets = {});

    /**
     * @brief 按类型注册指标，返回公共基类引用。
     *
     * 需要具体类型时优先调用 `GetCounter` / `GetGauge` / `GetHistogram`。
     * 本接口的 Histogram 使用默认桶。
     */
    Metric& Register(const std::string& name, const std::string& help,
                     MetricType type,
                     std::vector<std::string> label_names = {});

    /** @brief 渲染全部指标为 Prometheus 文本格式。 */
    std::string Render() const;

    /** @brief 已注册的指标族数量（按名字计）。 */
    std::size_t Size() const;

private:
    Metric& RegisterImpl(const std::string& name, const std::string& help,
                         MetricType type, std::vector<std::string> label_names,
                         std::vector<double> buckets);

    /// 保护 m_metrics；Register/Render/Size 都在它内部操作。
    mutable std::mutex m_mutex;
    /// 按名字保存指标族；注册后指针稳定，返回的引用随注册表存活。
    std::map<std::string, std::unique_ptr<Metric>> m_metrics;
};

}  // namespace metrics
}  // namespace go2cpp
