/**
 * @file test_prometheus_endpoint.cpp
 * @brief `/metrics` 端点测试：直接 `Handle`、真实 `bento::HttpServer` 端到端、
 *        fallback 路由、并发抓取。
 *
 * 覆盖策略：
 *   - **直接调用**：GET 200 / 非 `/metrics` 404 / 非 GET/HEAD 405 + Allow /
 *     HEAD 语义 / 自定义路径 / 自定义 Content-Type；
 *   - **真实服务端**：`PrometheusEndpoint::Attach` 挂到 `bento::HttpServer`，
 *     用 `http::Get` 真实抓取，校验状态码与指标文本；
 *   - **fallback**：`Attach(server, fallback)` 保留业务路由；
 *   - **并发**：4 线程 × 10 次抓取，全部 200 且内容完整。
 *
 * 运行：
 *   ctest --test-dir .build-bento-wsl -R go2cpp_metrics_endpoint_test
 */

#include "go2cpp/metrics/endpoint.hpp"

#include "go2cpp/bento/net/http_server.hpp"
#include "go2cpp/metrics/metrics.hpp"
#include "go2cpp/network/http.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using go2cpp::http::Method;
using go2cpp::http::Request;
using go2cpp::http::Response;
using go2cpp::http::Status;
using go2cpp::metrics::PrometheusEndpoint;
using go2cpp::metrics::Registry;

int g_checks = 0;
int g_failures = 0;

#define CHECK(expression)                                                     \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(expression)) {                                                  \
            ++g_failures;                                                     \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,       \
                         __LINE__, #expression);                              \
        }                                                                     \
    } while (0)

Request MakeRequest(Method method, std::string target) {
    Request request;
    request.method = method;
    request.target = std::move(target);
    return request;
}

/// 用真实 HttpServer + http::Get 抓一次。
struct FetchResult {
    bool ok{false};
    int status{0};
    std::string body;
};

FetchResult Fetch(const std::string& url) {
    FetchResult result;
    const auto response = go2cpp::http::Get(url, std::chrono::seconds(5));
    if (!response.ok()) {
        return result;
    }
    result.ok = true;
    result.status = static_cast<int>(response.response->status);
    result.body = response.response->body;
    return result;
}

void TestHandleDirect() {
    std::printf("[test] direct Handle()\n");
    Registry registry;
    auto& counter = registry.GetCounter("endpoint_total", "Endpoint.", {"k"});
    counter.With({"v"}).Add(5);
    PrometheusEndpoint endpoint(registry);

    const Response ok = endpoint.Handle(MakeRequest(Method::Get, "/metrics"));
    CHECK(ok.status == Status::OK);
    CHECK(ok.header.get("Content-Type") ==
          "text/plain; version=0.0.4; charset=utf-8");
    CHECK(ok.header.get("Cache-Control") == "no-cache");
    CHECK(ok.header.get("X-Content-Type-Options") == "nosniff");
    CHECK(ok.body.find("endpoint_total{k=\"v\"} 5\n") != std::string::npos);
    CHECK(ok.body.find("# TYPE endpoint_total counter\n") != std::string::npos);

    const Response not_found =
        endpoint.Handle(MakeRequest(Method::Get, "/other"));
    CHECK(not_found.status == Status::NotFound);

    // 查询串不影响路径匹配（path_view 截到 '?'）。
    const Response with_query =
        endpoint.Handle(MakeRequest(Method::Get, "/metrics?foo=bar"));
    CHECK(with_query.status == Status::OK);

    const Response method_not_allowed =
        endpoint.Handle(MakeRequest(Method::Post, "/metrics"));
    CHECK(method_not_allowed.status == Status::MethodNotAllowed);
    CHECK(method_not_allowed.header.get("Allow") == "GET, HEAD");

    const Response head = endpoint.Handle(MakeRequest(Method::Head, "/metrics"));
    CHECK(head.status == Status::OK);
    CHECK(!head.body.empty());

    // 自定义路径 + Content-Type。
    go2cpp::metrics::EndpointOptions options;
    options.path = "/internal/metrics";
    options.content_type = "text/plain; charset=utf-8";
    PrometheusEndpoint custom(registry, options);
    CHECK(custom.Handle(MakeRequest(Method::Get, "/metrics")).status ==
          Status::NotFound);
    const Response custom_ok =
        custom.Handle(MakeRequest(Method::Get, "/internal/metrics"));
    CHECK(custom_ok.status == Status::OK);
    CHECK(custom_ok.header.get("Content-Type") == "text/plain; charset=utf-8");

    // 关闭分位数：与 registry.Render() 一致。
    go2cpp::metrics::EndpointOptions options_no_quantile;
    options_no_quantile.exporter.include_quantiles = false;
    PrometheusEndpoint plain(registry, options_no_quantile);
    CHECK(plain.Render() == registry.Render());
}

void TestHttpServer() {
    std::printf("[test] bento::HttpServer end-to-end\n");
    Registry registry;
    auto& counter = registry.GetCounter("live_total", "Live.", {"method"});
    counter.With({"GET"}).Add(2);
    auto& histogram = registry.GetHistogram("live_latency", "Live latency.", {},
                                            {0.1, 1.0});
    histogram.Observe(0.05);
    histogram.Observe(0.5);

    PrometheusEndpoint endpoint(registry);
    go2cpp::bento::HttpServer server;
    endpoint.Attach(server);
    const auto listen = server.Listen("127.0.0.1:0");
    CHECK(listen.ok());
    if (!listen.ok()) {
        return;
    }
    const auto started = server.Start();
    CHECK(started.ok());
    if (!started.ok()) {
        server.Stop();
        return;
    }

    const std::string base =
        "http://127.0.0.1:" + std::to_string(server.Port());
    const FetchResult metrics = Fetch(base + "/metrics");
    CHECK(metrics.ok);
    CHECK(metrics.status == 200);
    CHECK(metrics.body.find("live_total{method=\"GET\"} 2\n") !=
          std::string::npos);
    CHECK(metrics.body.find("live_latency_bucket{le=\"+Inf\"} 2\n") !=
          std::string::npos);
    CHECK(metrics.body.find("live_latency_quantile{quantile=\"0.5\"}") !=
          std::string::npos);

    const FetchResult missing = Fetch(base + "/nope");
    CHECK(missing.ok);
    CHECK(missing.status == 404);

    server.Stop();
    CHECK(!server.IsRunning());
}

void TestFallback() {
    std::printf("[test] Attach(server, fallback)\n");
    Registry registry;
    auto& counter = registry.GetCounter("fallback_total", "Fallback.");
    counter.Inc();
    PrometheusEndpoint endpoint(registry);

    go2cpp::bento::HttpServer server;
    endpoint.Attach(server, [](const Request& request) {
        Response response;
        response.body = "fallback:" + std::string(request.path_view());
        return response;
    });
    const auto listen = server.Listen("127.0.0.1:0");
    CHECK(listen.ok());
    if (!listen.ok()) {
        return;
    }
    const auto started = server.Start();
    CHECK(started.ok());
    if (!started.ok()) {
        server.Stop();
        return;
    }
    const std::string base =
        "http://127.0.0.1:" + std::to_string(server.Port());

    const FetchResult metrics = Fetch(base + "/metrics");
    CHECK(metrics.ok);
    CHECK(metrics.status == 200);
    CHECK(metrics.body.find("fallback_total 1\n") != std::string::npos);

    const FetchResult other = Fetch(base + "/hello");
    CHECK(other.ok);
    CHECK(other.status == 200);
    CHECK(other.body == "fallback:/hello");

    server.Stop();
}

void TestConcurrentFetch() {
    std::printf("[test] concurrent scrape\n");
    Registry registry;
    auto& counter = registry.GetCounter("scrape_total", "Scrape.", {"worker"});
    for (int i = 0; i < 8; ++i) {
        counter.With({std::to_string(i)}).Add(static_cast<std::uint64_t>(i + 1));
    }
    PrometheusEndpoint endpoint(registry);
    go2cpp::bento::HttpServer server;
    endpoint.Attach(server);
    const auto listen = server.Listen("127.0.0.1:0");
    CHECK(listen.ok());
    if (!listen.ok()) {
        return;
    }
    const auto started = server.Start();
    CHECK(started.ok());
    if (!started.ok()) {
        server.Stop();
        return;
    }
    const std::string url = "http://127.0.0.1:" +
                            std::to_string(server.Port()) + "/metrics";

    std::atomic<int> ok_count{0};
    std::atomic<int> bad_count{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&]() {
            for (int iteration = 0; iteration < 10; ++iteration) {
                const FetchResult result = Fetch(url);
                if (result.ok && result.status == 200 &&
                    result.body.find("scrape_total{worker=\"7\"} 8\n") !=
                        std::string::npos) {
                    ok_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    bad_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(bad_count.load() == 0);
    CHECK(ok_count.load() == 40);
    server.Stop();
}

}  // namespace

int main() {
    TestHandleDirect();
    TestHttpServer();
    TestFallback();
    TestConcurrentFetch();
    std::printf("[test] checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
