/**
 * @file douyin_observability.hpp
 * @brief 抖音网关的可观测性接线：指标中间件、trace 中间件、`GET /metrics` 端点。
 *
 * ## 模块定位
 *
 * `include/go2cpp/distributed/metrics.hpp` 与 `trace.hpp` 只提供**原语**
 * （Counter/Gauge/Histogram、TraceContext），没有业务接入点；本文件是抖音
 * 网关的接入层：把「请求数 / 延迟分布 / 上游错误」三类信号写进
 * `metrics::Registry::Default()`，并用既有的 `PrometheusEndpoint` 暴露为
 * Prometheus 文本（不自己拼文本格式）。
 *
 * 只负责**抖音网关**这一个进程；六个上游服务各自是独立二进制，需要时再
 * 复用同一组指标名与中间件。
 *
 * ## 与网关组件的边界
 *
 * 中间件签名由 `app::gateway::Middleware` 决定（洋葱模型），注册顺序决定
 * 观测范围：
 * - `Metrics()` 必须注册在 `Trace()` **之后**（trace 先给请求打上 id，
 *   指标里的链路才有值；两者都对 `/metrics` 自身跳过）；
 * - `/metrics` 路由用自定义 `Matcher` 注册，避免被 `/api/**` 通配路由吞掉；
 * - 抓取请求不计入 `douyin_http_requests_total`，否则 Prometheus 每抓一次
 *   就给自己的 QPS 加一，形成自指噪声。
 *
 * ## 指标口径（Prometheus 命名约定）
 *
 * | 指标 | 类型 | 标签 | 含义 |
 * |---|---|---|---|
 * | `douyin_http_requests_total` | Counter | `path,status` | 已完成请求数 |
 * | `douyin_http_request_duration_seconds` | Histogram | `path` | 网关内处理耗时 |
 * | `douyin_http_requests_in_flight` | Gauge | — | 正在处理的请求数 |
 * | `douyin_upstream_errors_total` | Counter | `service` | 5xx 响应（含上游 502/504） |
 *
 * `path` 是**归一化**后的模板（`/api/video/*`、`/videos/*`），不是原始
 * 请求路径：原始路径含视频 id/用户 id，直接做标签会让序列数无限增长
 * （Prometheus 基数爆炸）。`service` 取 `/api/<service>/...` 的第二段，
 * 非 `/api/` 路径记为 `local`。
 *
 * ## 线程安全
 *
 * 中间件在网关 worker 线程调用；`Registry` 注册与 `With()` 内部加锁，
 * `Counter::Inc()` / `Histogram::Observe()` 是原子操作。trace 上下文存在
 * `thread_local`，只在当前 worker 线程内有效（不跨线程传播）。
 *
 * @author douyin demo
 * @date 2026-11-04
 * @copyright MIT License
 */

#pragma once

#include "go2cpp/app/gateway.hpp"

#include <string>

namespace douyin::observability {

/// 响应头：整条链路的 trace id（32 位小写十六进制）。
inline constexpr const char* kTraceIdHeader = "X-Trace-Id";
/// 响应头：本次请求在网关侧的 span id（16 位小写十六进制）。
inline constexpr const char* kSpanIdHeader = "X-Span-Id";

/**
 * @brief 注册全部指标族（幂等）。
 *
 * @details 在 `main()` 里、启动 worker 之前调用一次：让 `/metrics` 在第一个
 * 请求到达前就有 `# HELP` / `# TYPE` 行，抓取方不会看到「指标晚到」。
 * 重复调用安全（`Registry::Get*` 同名同类型直接返回已有族）。
 *
 * @return 无。
 */
void Init();

/**
 * @brief 指标中间件：请求计数、延迟直方图、在途请求数、上游错误。
 *
 * @return 可直接传给 `Gateway::Use()` 的中间件；必须注册在 trace 之后。
 *
 * @details 计数在 `next()` **返回后**发生，因此拿到的是最终状态码（含
 *          Handler 覆盖的 4xx/5xx 与上游透传的 5xx）。`/metrics` 自身跳过。
 */
go2cpp::app::gateway::Middleware Metrics();

/**
 * @brief trace 中间件：继承上游 trace 或新建根 trace，响应头回写 id。
 *
 * @details 从请求头 `g2c-traceparent`（`distributed/trace.hpp` 的线格式）
 *          解析上游上下文：解析成功则派生 `ChildSpan()`，否则
 *          `CreateRoot()`。响应头写 `X-Trace-Id` / `X-Span-Id`，便于按
 *          一次请求在日志、抓包与指标之间对齐。
 *
 * @return 可直接传给 `Gateway::Use()` 的中间件。
 */
go2cpp::app::gateway::Middleware Trace();

/**
 * @brief 注册 `GET /metrics` 本地路由（自定义 Matcher，不产生上游请求）。
 *
 * @param gateway [in] 目标网关——借用，调用方持有其生命周期；不可为 nullptr。
 * @param error   [out] 失败原因——成功时保持原值；失败时写入可直接打印的文本。
 *
 * @return
 * - true：路由已注册，`/metrics` 可被 Prometheus 抓取；
 * - false：注册失败（pattern 冲突等）——调用方**必须**打印 error 并退出，
 *   不要继续启动一个没有指标端点的网关。
 */
bool AttachMetricsRoute(go2cpp::app::gateway::Gateway* gateway, std::string* error);

}  // namespace douyin::observability
