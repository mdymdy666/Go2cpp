/**
 * @file endpoint.hpp
 * @brief 把指标注册表挂到 HTTP `/metrics`：Prometheus 抓取端点。
 *
 * @details 端点只做三件事：
 *   - **路由**：`GET /metrics`（以及 `HEAD`，语义由 HTTP 层按 HEAD 处理）；
 *   - **渲染**：调用 `prom::Exporter` 生成文本（见 `prometheus.hpp`）；
 *   - **响应头**：`Content-Type: text/plain; version=0.0.4; charset=utf-8`
 *     （Prometheus 文本格式 0.0.4）、`Cache-Control: no-cache`、
 *     `X-Content-Type-Options: nosniff`。
 *
 * 非 `/metrics` 返回 404，非 GET/HEAD 返回 405 + `Allow: GET, HEAD`。
 *
 * ## 与 HttpServer 的接入方式
 * `bento::HttpServer` 只有一个全局 `RequestHandler`，因此提供两种挂法：
 *
 * @code
 * // 1) 独占服务端
 * go2cpp::metrics::PrometheusEndpoint endpoint(registry);
 * go2cpp::bento::HttpServer server;
 * endpoint.Attach(server);          // 内部 server.OnRequest(...)
 * server.Listen("127.0.0.1:9100");
 * server.Start();
 *
 * // 2) 与既有业务 handler 共存（fallback 处理其它路径）
 * endpoint.Attach(server, [](const go2cpp::http::Request& req) {
 *     go2cpp::http::Response response;
 *     response.body = "hello";
 *     return response;
 * });
 * @endcode
 *
 * `Attach` 是模板：`endpoint.hpp` 只依赖 `network/http.hpp`，不硬依赖
 * `bento` 库；把端点挂到任何提供
 * `void OnRequest(std::function<Response(const Request&)>)` 的服务端都可以，
 * 只有真正实例化 `Attach` 的翻译单元才需要该服务端的完整类型。
 *
 * ## 线程安全
 * `Handle()` / `Render()` 只读注册表与 `EndpointOptions`（构造后不可变），
 * 可以被多个 worker 线程并发调用；每次请求生成独立的文本缓冲。
 *
 * ## 边界与陷阱
 * - `Attach(server)` 会**替换**服务端已有的 `OnRequest` handler；需要保留
 *   原 handler 时用带 fallback 的重载；
 * - `HEAD` 返回完整 body，由 `HttpServer` 按 `method == Head` 只发响应头
 *   （`Content-Length` 仍是 GET 的长度）；自己接 HTTP 层时要复用该语义；
 * - 抓取端并发很高时，每次请求都会重新渲染整份文本（`Registry::Render()`
 *   持锁）；指标规模大时应由调用方做缓存，本端点不内置缓存。
 */

#pragma once

#include "go2cpp/metrics/prometheus.hpp"
#include "go2cpp/network/http.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace go2cpp {
namespace metrics {

/** @brief `/metrics` 端点的配置；构造后不可变。 */
struct EndpointOptions {
    /// 暴露路径（精确匹配，不含查询串）。
    std::string path{"/metrics"};
    /// 文本导出选项（HELP / 分位数）。
    prom::ExporterOptions exporter{};
    /// 响应 `Content-Type`；默认是 Prometheus 文本格式 0.0.4。
    std::string content_type{"text/plain; version=0.0.4; charset=utf-8"};
};

/**
 * @brief `/metrics` 端点：把 `Registry` 暴露成 Prometheus 抓取目标。
 *
 * 生命周期：注册表必须比端点活得久（端点只保存它的地址）。
 */
class PrometheusEndpoint {
public:
    /** @brief 使用默认配置（路径 `/metrics`，输出 p50/p90/p99）。 */
    explicit PrometheusEndpoint(const Registry& registry);

    /** @brief 自定义配置。 */
    PrometheusEndpoint(const Registry& registry, EndpointOptions options);

    PrometheusEndpoint(const PrometheusEndpoint&) = default;
    PrometheusEndpoint& operator=(const PrometheusEndpoint&) = default;

    /**
     * @brief 处理一个 HTTP 请求并返回响应。
     *
     * - 路径不匹配 → 404；
     * - 方法不是 GET/HEAD → 405 + `Allow: GET, HEAD`；
     * - 其余 → 200 + 指标文本。
     */
    http::Response Handle(const http::Request& request) const;

    /** @brief 直接渲染指标文本（绕过 HTTP，便于测试 / 写文件）。 */
    std::string Render() const;

    /** @brief 路径是否命中本端点（精确比较，不含查询串）。 */
    bool Matches(std::string_view path) const noexcept;

    /** @brief 当前配置（只读）。 */
    const EndpointOptions& options() const noexcept { return m_options; }

    /** @brief 绑定的注册表（只读）。 */
    const Registry& registry() const noexcept { return *m_registry; }

    /**
     * @brief 挂到一个 HTTP 服务端，接管它的全部请求。
     * @tparam Server 提供 `OnRequest(std::function<Response(const Request&)>)`。
     */
    template <class Server>
    void Attach(Server& server) {
        server.OnRequest([this](const http::Request& request) {
            return Handle(request);
        });
    }

    /**
     * @brief 挂到服务端，但把非 `/metrics` 的请求交给 `fallback`。
     * @param fallback 可调用对象，签名 `http::Response(const http::Request&)`。
     */
    template <class Server, class Fallback>
    void Attach(Server& server, Fallback fallback) {
        server.OnRequest([this, fallback = std::move(fallback)](
                             const http::Request& request) mutable {
            if (Matches(request.path_view())) {
                return Handle(request);
            }
            return fallback(request);
        });
    }

private:
    const Registry* m_registry;  ///< 数据源；端点不拥有注册表
    EndpointOptions m_options;   ///< 构造后不可变，Handle 并发只读
    prom::Exporter m_exporter;   ///< 基于同一注册表；无状态，可并发 Render
};

}  // namespace metrics
}  // namespace go2cpp
