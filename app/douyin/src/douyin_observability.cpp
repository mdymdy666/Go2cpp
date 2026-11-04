/**
 * @file douyin_observability.cpp
 * @brief 抖音网关可观测性接线：指标中间件、trace 中间件、`/metrics` 端点。
 *
 * @details 指标族全部注册在 `metrics::Registry::Default()`，复用既有的
 *          `Counter` / `Gauge` / `Histogram` 与 `PrometheusEndpoint`，
 *          本文件不实现任何存储或文本格式。
 *
 * ## 关键取舍
 * - **路径归一化**：`/api/video/<id>` 这类路径若原样做标签，序列数随业务
 *   数据线性增长（Prometheus 基数爆炸）。这里归一到服务级模板
 *   （`/api/video/*`），既覆盖「6 服务 QPS/延迟/错误率」的观测需求，又把
 *   序列数限制在几十个；
 * - **不统计 `/metrics` 自身**：抓取请求会自我放大 QPS，形成自指噪声；
 * - **状态码口径**：中间件在 `next()` 之后取 `response.status`，与真正写回
 *   客户端的值一致；中间件链返回错误时网关最终会写 500，按 500 计。
 *
 * @author douyin demo
 * @date 2026-11-04
 * @copyright MIT License
 */

#include "douyin_observability.hpp"

#include "go2cpp/distributed/trace.hpp"
#include "go2cpp/metrics/endpoint.hpp"
#include "go2cpp/metrics/metrics.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using go2cpp::app::gateway::Gateway;
using go2cpp::app::gateway::Handler;
using go2cpp::app::gateway::Matcher;
using go2cpp::app::gateway::Middleware;
using go2cpp::app::gateway::Next;
using go2cpp::app::gateway::PathParams;
using go2cpp::app::gateway::Request;
using go2cpp::app::gateway::Response;
using go2cpp::app::gateway::Result;
using go2cpp::metrics::Counter;
using go2cpp::metrics::EndpointOptions;
using go2cpp::metrics::Gauge;
using go2cpp::metrics::Histogram;
using go2cpp::metrics::PrometheusEndpoint;
using go2cpp::metrics::Registry;

/// @brief 已完成请求数（按归一化路径与状态码分组）。
constexpr const char* kRequestsTotal = "douyin_http_requests_total";
constexpr const char* kRequestsTotalHelp =
    "HTTP requests handled by the douyin gateway, by path template and status.";
/// @brief 网关内处理耗时（秒，含上游转发时间）。
constexpr const char* kDurationSeconds = "douyin_http_request_duration_seconds";
constexpr const char* kDurationHelp =
    "Gateway-side request duration in seconds, including upstream fan-out.";
/// @brief 正在处理中的请求数。
constexpr const char* kInFlight = "douyin_http_requests_in_flight";
constexpr const char* kInFlightHelp =
    "Requests currently being handled by the douyin gateway.";
/// @brief 5xx 响应数（上游 502/504 与透传的上游 5xx）。
constexpr const char* kUpstreamErrorsTotal = "douyin_upstream_errors_total";
constexpr const char* kUpstreamErrorsHelp =
    "Gateway responses with status >= 500, by upstream service.";
/// @brief 指标端点路径。
constexpr const char* kMetricsPath = "/metrics";
/// @brief 内建健康聚合路径：路径本身是静态的，不做模板归一化。
constexpr std::string_view kHealthAllPath = "/api/health/all";

/// @brief 延迟直方图桶（秒）：0.5ms～5s，覆盖本地静态资源与上游转发。
///
/// 网关典型耗时 <1ms（静态文件）到数百毫秒（上游转发）；桶从 0.5ms 起，
/// 保住了 p50 的分辨率，10s 以上的超时由隐式 `+Inf` 桶承接。
const std::vector<double>& DurationBuckets() {
    static const std::vector<double> buckets{
        0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05,
        0.1,    0.25,  0.5,    1.0,   2.5,  5.0};
    return buckets;
}

/// @brief 指标句柄：注册后地址稳定，热路径只读引用。
struct Handles {
    Counter& requests;        ///< 请求计数（path,status）
    Counter& upstream_errors; ///< 5xx 计数（service）
    Gauge& in_flight;         ///< 在途请求（无标签）
    Histogram& duration;      ///< 耗时直方图（path）
};

/**
 * @brief 取得（首次调用时注册）全部指标族。
 *
 * @return 进程级句柄引用，生命周期与注册表相同。
 *
 * ## 定位
 * 唯一的注册点：所有指标名、标签名、桶边界都在这里固定，避免不同调用点
 * 注册出同名异构的指标族（`Registry` 会抛 `invalid_argument`）。
 *
 * ## 并发/失败语义
 * - 调用约束：任意线程；首次注册由 C++11 magic static 保证只执行一次。
 * - 失败语义：注册冲突（同名不同类型）抛出异常，属于编程错误，启动期暴露。
 */
const Handles& GetHandles() {
    static const Handles handles = [] {
        Registry& registry = Registry::Default();
        return Handles{
            registry.GetCounter(kRequestsTotal, kRequestsTotalHelp,
                                {"path", "status"}),
            registry.GetCounter(kUpstreamErrorsTotal, kUpstreamErrorsHelp,
                                {"service"}),
            registry.GetGauge(kInFlight, kInFlightHelp),
            registry.GetHistogram(kDurationSeconds, kDurationHelp, {"path"},
                                  DurationBuckets()),
        };
    }();
    return handles;
}

/// @brief `/metrics` 端点：复用 `PrometheusEndpoint`，分位数取 p50/p95/p99。
const PrometheusEndpoint& Endpoint() {
    static const PrometheusEndpoint endpoint = [] {
        EndpointOptions options;
        options.exporter.quantiles = {0.5, 0.95, 0.99};
        return PrometheusEndpoint(Registry::Default(), std::move(options));
    }();
    return endpoint;
}

/// @brief 路径是否为 `/api/<service>/...` 形态（服务级路径）。
bool IsServicePath(std::string_view path) noexcept {
    return path.rfind("/api/", 0) == 0;
}

/**
 * @brief 把请求路径归一到受限基数：`/api/<service>/<rest>` → `/api/<service>/*`。
 *
 * @param path [in] 原始请求路径（不含查询串）——借用，生命周期覆盖本次调用。
 * @return 归一化路径；非服务路径原样返回（`/`、`/health`、`/videos/*` 等）。
 *
 * ## 定位
 * 指标标签的唯一来源。归一只影响**标签值**，不影响转发/路由。
 *
 * ## 步骤
 * step1. 空路径或根路径直接返回；
 * step2. 静态短路径（只有一段）原样保留；
 * step3. `/api/health/all` 是内建聚合端点，路径静态，原样保留；
 * step4. `/api/<service>/...` 归一为 `/api/<service>/*`；
 * step5. `/videos/<name>` 归一为 `/videos/*`（视频名是用户数据）；
 * step6. 其余路径原样返回（基数由路由表决定，不由业务数据决定）。
 *
 * ## 并发/失败语义
 * - 无状态、不抛异常；每次调用只做一次分配。
 */
std::string NormalizePath(std::string_view path) {
    // step1: 根路径最常见，直接短路避免分配。
    if (path.empty()) {
        return "/";
    }
    if (path == "/") {
        return "/";
    }
    // step2: 第一段之后没有 `/`：静态短路径（/health、/metrics、/favicon.ico）。
    const std::size_t first_end = path.find('/', 1);
    if (first_end == std::string_view::npos) {
        return std::string(path);
    }
    const std::string_view head = path.substr(0, first_end);
    // step3: 内建健康聚合是静态路径，保留完整路径便于按端点观测。
    if (path == kHealthAllPath) {
        return std::string(path);
    }
    // step4: 服务路径归一：只保留服务名，其余段折叠为 `*`。
    if (head == "/api") {
        const std::size_t service_end = path.find('/', first_end + 1);
        if (service_end == std::string_view::npos) {
            return std::string(path);
        }
        const std::string_view service =
            path.substr(first_end + 1, service_end - first_end - 1);
        if (service.empty() || service == "health") {
            return std::string(path);
        }
        std::string normalized;
        normalized.reserve(first_end + service.size() + 3);
        normalized.append(head);
        normalized.push_back('/');
        normalized.append(service);
        normalized.append("/*");
        return normalized;
    }
    // step5: 静态视频按名字访问，名字是用户数据，折叠为通配。
    if (head == "/videos") {
        return "/videos/*";
    }
    // step6: 其余路径（未来新增的静态前缀）原样保留。
    return std::string(path);
}

/**
 * @brief 从请求路径提取上游服务名：`/api/<service>/...` → `<service>`。
 *
 * @param path [in] 原始请求路径——借用。
 * @return 服务名；非 `/api/` 路径返回 `local`（本地 handler，无上游）。
 */
std::string ServiceOf(std::string_view path) {
    if (!IsServicePath(path)) {
        return "local";
    }
    const std::size_t start = 5;  // 跳过 "/api/"
    const std::size_t end = path.find('/', start);
    if (end == std::string_view::npos) {
        return std::string(path.substr(start));
    }
    return std::string(path.substr(start, end - start));
}

/// @brief `/metrics` 路由匹配：精确路径匹配，不产生上游请求。
class MetricsMatcher final : public Matcher {
public:
    /// @brief 仅当请求路径恰为 `/metrics` 时命中。
    bool Match(const Request& request, PathParams* /*params*/) const override {
        return request.path_view() == kMetricsPath;
    }
};

/// @brief `/metrics` handler：把注册表渲染成 Prometheus 文本。
///
/// @param request [in] 入站请求——借用；方法/路径校验由端点自己完成。
/// @param response [out] 出站响应——整体覆盖为端点响应（200/404/405）。
/// @return 总是 Ok；错误语义由状态码表达。
Result<void> ServeMetrics(Request& request, Response& response) {
    response = Endpoint().Handle(request);
    return Result<void>::Ok();
}

}  // namespace

namespace douyin::observability {

void Init() {
    // 首次调用即完成注册；magic static 保证并发安全。
    (void)GetHandles();
    (void)Endpoint();
}

/**
 * @brief 构造指标中间件。
 *
 * @return 中间件：计时、计数、记录在途与 5xx。
 *
 * ## 步骤
 * step1. `/metrics` 自抓取直接放行，不产生指标（避免自指噪声）；
 * step2. 记录起始时间并递增在途 Gauge；
 * step3. 调用 `next()` 执行剩余链（路由/鉴权/转发）；
 * step4. 递减在途 Gauge，计算耗时（秒）；
 * step5. 取最终状态码：链路返回 Err 时网关会改写为 500，按 500 统计；
 * step6. 归一化路径后写请求计数与耗时直方图；5xx 额外记上游错误。
 *
 * ## 并发/失败语义
 * - 调用约束：网关 worker 线程；同一请求只在当前线程内执行。
 * - 原子性：Counter/Gauge/Histogram 内部为无锁原子操作；`With()` 首次
 *   创建子序列时加族锁，之后命中只读。
 * - 失败语义：不吞异常；指标写入失败（标签非法）会向上抛，由网关兜底 500。
 */
Middleware Metrics() {
    return [](Request& request, Response& response,
              const Next& next) -> Result<void> {
        // step1: 指标端点自身不计入业务 QPS。
        const std::string_view path = request.path_view();
        if (path == kMetricsPath) {
            return next();
        }
        const Handles& handles = GetHandles();
        // step2: 计时与在途计数必须在 next() 之前。
        const auto started = std::chrono::steady_clock::now();
        handles.in_flight.Inc();
        // step3: 洋葱模型——next() 返回时响应已确定。
        const Result<void> result = next();
        // step4: 先减在途，再算耗时（直方图只关心处理时长）。
        handles.in_flight.Dec();
        const double elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          started)
                .count();
        // step5: 中间件链返回 Err 时 Dispatch 会用 500 覆盖响应。
        const int status = result.ok() ? static_cast<int>(response.status) : 500;
        // step6: 标签值来自受限基数的归一化路径与状态码。
        const std::string normalized = NormalizePath(path);
        const std::string status_text = std::to_string(status);
        handles.requests.With({normalized, status_text}).Inc();
        handles.duration.With({normalized}).Observe(elapsed_seconds);
        if (status >= 500) {
            handles.upstream_errors.With({ServiceOf(path)}).Inc();
        }
        return result;
    };
}

/**
 * @brief 构造 trace 中间件：继承上游链路或新建根链路，并在响应头回写 id。
 *
 * @return 中间件：响应头包含 `X-Trace-Id` 与 `X-Span-Id`。
 *
 * ## 步骤
 * step1. 读请求头 `g2c-traceparent`，解析成功则派生 `ChildSpan()`；
 * step2. 无上游上下文（或解析失败）时 `CreateRoot()` 新建链路；
 * step3. 写入线程局部的 current context，供本线程后续日志读取；
 * step4. 执行 `next()`；
 * step5. 回写 `X-Trace-Id` / `X-Span-Id` 响应头（序列化前）；
 * step6. 清理线程局部上下文，避免同一 worker 处理下一个请求时串链。
 *
 * ## 并发/失败语义
 * - 调用约束：网关 worker 线程；thread_local 上下文只在当前请求生命周期内有效。
 * - 失败语义：解析失败降级为新建根链路，不抛异常、不阻断请求。
 * - 不变量：响应一定带 `X-Trace-Id`（32 位十六进制），除非 next() 抛异常。
 */
Middleware Trace() {
    return [](Request& request, Response& response,
              const Next& next) -> Result<void> {
        namespace trace = go2cpp::distributed;
        // step1: 优先继承上游链路，保持一次端到端请求同一个 trace id。
        trace::TraceContext context;
        bool inherited = false;
        const std::string parent_text =
            request.header.get(trace::kTraceHeader);
        if (!parent_text.empty()) {
            trace::TraceContext parent;
            if (trace::TraceContext::TryParseHeader(parent_text, &parent)) {
                context = parent.ChildSpan();
                inherited = context.IsValid();
            }
        }
        // step2: 没有有效上游上下文就是新链路的根 span。
        if (!inherited) {
            context = trace::TraceContext::CreateRoot();
        }
        // step3: 线程局部上下文给本线程的日志/后续中间件使用。
        trace::SetCurrentTraceContext(context);
        // step4: 执行剩余链。
        const Result<void> result = next();
        // step5: 响应头在序列化之前写入，客户端可直接关联日志。
        response.header.set(kTraceIdHeader, context.trace.ToString());
        response.header.set(kSpanIdHeader, context.span.ToString());
        // step6: 清理，防止线程复用导致链路串号。
        trace::ClearCurrentTraceContext();
        return result;
    };
}

bool AttachMetricsRoute(Gateway* gateway, std::string* error) {
    if (gateway == nullptr) {
        if (error != nullptr) {
            *error = "gateway is null";
        }
        return false;
    }
    const auto result = gateway->AddRoute(std::make_shared<MetricsMatcher>(),
                                          std::make_shared<Handler>(&ServeMetrics));
    if (!result.ok()) {
        if (error != nullptr) {
            *error = result.error();
        }
        return false;
    }
    return true;
}

}  // namespace douyin::observability
