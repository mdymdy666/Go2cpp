/**
 * @file endpoint.cpp
 * @brief `PrometheusEndpoint` 的实现：路由判定与响应头拼装。
 *
 * ## 实现顺序
 * step1. 路径匹配（`Matches`，不含查询串）；
 * step2. 方法判定（只接受 GET/HEAD，其余 405 + Allow）；
 * step3. 渲染指标文本并填 200 响应头。
 *
 * ## 边界
 * - 404 / 405 的 body 是纯文本单行，便于 curl / 浏览器直接看；
 * - `HEAD` 返回完整 body：由 HTTP 层按 HEAD 语义只发头（Content-Length 仍
 *   是 GET 的长度），端点不在这里裁剪 body，避免重复渲染或长度对不上。
 */

#include "go2cpp/metrics/endpoint.hpp"

#include <utility>

namespace go2cpp {
namespace metrics {

PrometheusEndpoint::PrometheusEndpoint(const Registry& registry)
    : m_registry(&registry), m_options(), m_exporter(registry) {}

PrometheusEndpoint::PrometheusEndpoint(const Registry& registry,
                                       EndpointOptions options)
    : m_registry(&registry),
      m_options(std::move(options)),
      m_exporter(registry) {}

bool PrometheusEndpoint::Matches(std::string_view path) const noexcept {
    return path == m_options.path;
}

std::string PrometheusEndpoint::Render() const {
    return m_exporter.Render(m_options.exporter);
}

http::Response PrometheusEndpoint::Handle(const http::Request& request) const {
    http::Response response;
    if (!Matches(request.path_view())) {
        response.status = http::Status::NotFound;
        response.header.set("Content-Type", "text/plain; charset=utf-8");
        response.body = "404 page not found\n";
        return response;
    }
    if (request.method != http::Method::Get &&
        request.method != http::Method::Head) {
        response.status = http::Status::MethodNotAllowed;
        response.header.set("Allow", "GET, HEAD");
        response.header.set("Content-Type", "text/plain; charset=utf-8");
        response.body = "405 method not allowed\n";
        return response;
    }

    response.status = http::Status::OK;
    response.header.set("Content-Type", m_options.content_type);
    response.header.set("Cache-Control", "no-cache");
    response.header.set("X-Content-Type-Options", "nosniff");
    response.body = m_exporter.Render(m_options.exporter);
    return response;
}

}  // namespace metrics
}  // namespace go2cpp
