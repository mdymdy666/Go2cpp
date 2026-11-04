/**
 * @file test_gateway.cpp
 * @brief 通用网关组件的端到端测试：注册式路由、中间件、Resolver、配置热更新、
 *        转发/超时/502/健康聚合与压测。
 *
 * @details 用 `go2cpp::net::Socket` 起两个最小 HTTP echo 上游（随机端口），
 *          用 `go2cpp::http::Client` 作为测试客户端；不依赖第三方测试框架。
 *
 * @author go2cpp contributors
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "go2cpp/app/gateway.hpp"

#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::app::gateway;
namespace http = go2cpp::http;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& name) {
    ++g_checks;
    if (condition) {
        std::printf("  PASS %s\n", name.c_str());
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", name.c_str());
    }
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string ExtractHeader(const std::string& lower_headers, std::string_view name) {
    const std::string needle = "\r\n" + std::string(name) + ":";
    const std::size_t position = lower_headers.find(needle);
    if (position == std::string::npos) {
        return {};
    }
    std::size_t begin = position + needle.size();
    while (begin < lower_headers.size() &&
           (lower_headers[begin] == ' ' || lower_headers[begin] == '\t')) {
        ++begin;
    }
    const std::size_t end = lower_headers.find("\r\n", begin);
    if (end == std::string::npos) {
        return {};
    }
    return lower_headers.substr(begin, end - begin);
}

std::uint16_t SocketPort(const go2cpp::net::Socket::Ptr& socket) {
    const auto address = socket->local_address();
    const auto ip = std::dynamic_pointer_cast<go2cpp::net::IPAddress>(address);
    return ip ? ip->port() : 0;
}

bool SendAll(const go2cpp::net::Socket::Ptr& socket, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto result = socket->send(data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (!result.ok() || result.transferred <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result.transferred);
    }
    return true;
}

/// @brief 建一条裸 TCP 连接（带 5s recv 超时），用于半开连接与流水线测试。
int OpenRaw(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    timeval timeout{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool SendRaw(int fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t written =
            ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

/// @brief 读到对端关闭或 5s 超时为止，返回全部响应字节。
std::string ReadUntilEof(int fd) {
    std::string output;
    std::array<char, 4096> chunk{};
    for (;;) {
        const ssize_t got = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (got <= 0) {
            break;
        }
        output.append(chunk.data(), static_cast<std::size_t>(got));
    }
    return output;
}

/// @brief 读到第一个 `\r\n\r\n`（响应头结束）为止；用于 100 Continue 预应答。
std::string ReadHeaderBlock(int fd) {
    std::string output;
    std::array<char, 1024> chunk{};
    while (output.find("\r\n\r\n") == std::string::npos) {
        const ssize_t got = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (got <= 0) {
            break;
        }
        output.append(chunk.data(), static_cast<std::size_t>(got));
    }
    return output;
}

/// @brief 在 timeout_ms 内读取已到达字节；超时或对端关闭即返回已读内容（不阻塞 5s）。
std::string ReadAvailable(int fd, int timeout_ms) {
    std::string output;
    std::array<char, 1024> chunk{};
    for (;;) {
        pollfd entry{};
        entry.fd = fd;
        entry.events = POLLIN;
        const int ready = ::poll(&entry, 1, timeout_ms);
        if (ready <= 0) {
            break;
        }
        const ssize_t got = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (got <= 0) {
            break;
        }
        output.append(chunk.data(), static_cast<std::size_t>(got));
    }
    return output;
}

/// @brief 最小 HTTP echo 上游：随机端口、/health、/slow 与回显请求行/头/body。
class EchoUpstream final {
public:
    explicit EchoUpstream(std::string name) : m_name(std::move(name)) {}
    ~EchoUpstream() { Stop(); }

    EchoUpstream(const EchoUpstream&) = delete;
    EchoUpstream& operator=(const EchoUpstream&) = delete;

    bool Start() {
        m_listener = go2cpp::net::ListenTCP("127.0.0.1", 0);
        if (!m_listener) {
            return false;
        }
        m_port = SocketPort(m_listener);
        m_thread = std::thread([this]() { Loop(); });
        return m_port != 0;
    }

    void Stop() {
        if (m_stopping.exchange(true)) {
            return;
        }
        if (m_listener) {
            m_listener->close();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        m_listener.reset();
    }

    std::uint16_t port() const noexcept { return m_port; }
    std::string address() const { return "127.0.0.1:" + std::to_string(m_port); }
    const std::string& name() const noexcept { return m_name; }

private:
    void Loop() {
        while (!m_stopping.load(std::memory_order_acquire)) {
            auto connection = m_listener->accept(std::chrono::milliseconds(50));
            if (!connection) {
                continue;
            }
            Serve(connection);
            connection->close();
        }
    }

    void Serve(const go2cpp::net::Socket::Ptr& connection) {
        std::string buffer;
        std::array<char, 4096> chunk{};
        std::size_t header_end = std::string::npos;
        while (buffer.size() < 64 * 1024) {
            const auto received =
                connection->recv(chunk.data(), chunk.size(), 0, std::chrono::seconds(5));
            if (!received.ok() || received.transferred <= 0) {
                return;
            }
            buffer.append(chunk.data(), static_cast<std::size_t>(received.transferred));
            header_end = buffer.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                break;
            }
        }
        if (header_end == std::string::npos) {
            return;
        }
        const std::size_t line_end = buffer.find("\r\n");
        if (line_end == std::string::npos) {
            return;
        }
        const std::string request_line = buffer.substr(0, line_end);
        const std::size_t first_space = request_line.find(' ');
        const std::size_t second_space =
            first_space == std::string::npos ? std::string::npos
                                             : request_line.find(' ', first_space + 1);
        if (first_space == std::string::npos || second_space == std::string::npos) {
            return;
        }
        const std::string method = request_line.substr(0, first_space);
        const std::string target =
            request_line.substr(first_space + 1, second_space - first_space - 1);
        const std::string lower_headers = Lower(buffer.substr(0, header_end + 2));
        std::size_t content_length = 0;
        const std::string length_text = ExtractHeader(lower_headers, "content-length");
        if (!length_text.empty()) {
            content_length = static_cast<std::size_t>(std::strtoul(length_text.c_str(), nullptr, 10));
        }
        const std::size_t body_begin = header_end + 4;
        while (buffer.size() < body_begin + content_length) {
            const auto received =
                connection->recv(chunk.data(), chunk.size(), 0, std::chrono::seconds(5));
            if (!received.ok() || received.transferred <= 0) {
                break;
            }
            buffer.append(chunk.data(), static_cast<std::size_t>(received.transferred));
        }
        std::string body;
        if (buffer.size() >= body_begin) {
            body = buffer.substr(body_begin, content_length);
        }
        const std::string path = target.substr(0, target.find('?'));
        std::string payload;
        if (path == "/health") {
            payload = "{\"ok\":true,\"service\":\"" + m_name + "\"}";
        } else if (path.size() >= 5 && path.compare(path.size() - 5, 5, "/slow") == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            payload = "slow";
        } else {
            payload = "server=" + m_name + ";method=" + method + ";target=" + target +
                      ";body=" + body + ";user=" + ExtractHeader(lower_headers, "x-user-id");
        }
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
            std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
        SendAll(connection, response);
    }

    std::string m_name;
    go2cpp::net::Socket::Ptr m_listener;
    std::thread m_thread;
    std::atomic<bool> m_stopping{false};
    std::uint16_t m_port{0};
};

struct GatewayHarness {
    Config config;
    std::unique_ptr<Gateway> gateway;
    std::thread thread;

    bool Start() {
        auto created = Gateway::Create(config);
        if (!created.ok()) {
            std::printf("  FAIL create gateway: %s\n", created.error().c_str());
            ++g_failures;
            return false;
        }
        gateway = std::move(created.value());
        thread = std::thread([this]() { gateway->Run(); });
        for (int i = 0; i < 400; ++i) {
            if (gateway->running()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::printf("  FAIL gateway did not start\n");
        ++g_failures;
        return false;
    }

    void Stop() {
        if (gateway) {
            gateway->Stop();
        }
        if (thread.joinable()) {
            thread.join();
        }
        gateway.reset();
    }

    ~GatewayHarness() { Stop(); }
};

http::Result Call(std::uint16_t port, const std::string& target,
                  http::Method method = http::Method::Get,
                  const std::string& body = {},
                  std::initializer_list<std::pair<std::string, std::string>> headers = {}) {
    http::Client client(std::chrono::milliseconds(3000));
    http::Request request;
    request.method = method;
    request.target = target;
    request.body = body;
    for (const auto& header : headers) {
        request.header.set(header.first, header.second);
    }
    return client.do_request(request, "127.0.0.1", port);
}

bool HasBody(const http::Result& result, std::string_view needle) {
    return result.ok() && result.response &&
           result.response->body.find(needle) != std::string::npos;
}

int StatusOf(const http::Result& result) {
    return result.ok() && result.response
               ? static_cast<int>(result.response->status)
               : -1;
}

void AddRouteChecked(Gateway& gateway, RouteSpec spec) {
    const auto result = gateway.AddRoute(std::move(spec));
    if (!result.ok()) {
        std::printf("  FAIL AddRoute: %s\n", result.error().c_str());
        ++g_failures;
    }
}

Response ErrorResponseForTest(int code, std::string_view message) {
    Response response;
    response.status = static_cast<http::Status>(code);
    response.header.set("Content-Type", "application/json; charset=utf-8");
    response.body = "{\"ok\":false,\"error\":\"" + std::string(message) +
                    "\",\"code\":" + std::to_string(code) + "}";
    return response;
}

class TenantMatcher final : public Matcher {
public:
    bool Match(const Request& request, PathParams* params) const override {
        if (request.header.get("X-Tenant") != "a") {
            return false;
        }
        (*params)["tenant"] = "a";
        return true;
    }
};

/// @brief 命中 /boom 的匹配器：用于验证 handler 抛异常时网关回 500 而不是崩溃。
class ThrowMatcher final : public Matcher {
public:
    bool Match(const Request& request, PathParams*) const override {
        return request.path() == "/boom";
    }
};

std::vector<double> g_latencies_us;

}  // namespace

int main() {
    std::printf("[gateway] 启动 2 个 echo 上游\n");
    EchoUpstream upstream_a("A");
    EchoUpstream upstream_b("B");
    if (!upstream_a.Start() || !upstream_b.Start()) {
        std::printf("FAIL: 无法启动 echo 上游\n");
        return 1;
    }
    std::printf("[gateway] upstream A=%s B=%s\n", upstream_a.address().c_str(),
                upstream_b.address().c_str());

    std::mutex order_mutex;
    std::vector<std::string> order;
    std::atomic<bool> record_order{true};

    GatewayHarness harness;
    harness.config.listen_host = "127.0.0.1";
    harness.config.port = 0;
    harness.config.worker_threads = 4;
    harness.config.auth = [](const Request& request, std::string* user_id,
                             std::string* error) {
        if (request.header.get("Authorization") == "Bearer good") {
            *user_id = "42";
            return true;
        }
        *error = "bad token";
        return false;
    };
    harness.config.rate_limit = [](const Request& request) {
        return request.path() != "/api/limited";
    };
    harness.config.decorate = [](const Request&, Response* response) {
        response->header.set("X-Gateway", "test");
    };
    if (!harness.Start()) {
        return 1;
    }
    Gateway& gateway = *harness.gateway;
    const std::uint16_t port = gateway.port();
    std::printf("[gateway] 监听 127.0.0.1:%u\n", static_cast<unsigned>(port));

    const auto push_order = [&](const char* value) {
        if (!record_order.load(std::memory_order_relaxed)) {
            return;
        }
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(value);
    };
    gateway.Use([&](Request&, Response&, const Next& next) {
        push_order("before-1");
        const Result<void> result = next();
        push_order("after-1");
        return result;
    });
    gateway.Use([&](Request&, Response&, const Next& next) {
        push_order("before-2");
        const Result<void> result = next();
        push_order("after-2");
        return result;
    });
    gateway.Use([&](Request& request, Response& response, const Next& next) {
        const Result<void> result = next();
        const auto found = request.path_values.find("id");
        if (found != request.path_values.end()) {
            response.header.set("X-Path-Id", found->second);
        }
        return result;
    });

    // ---- 1. 运行时注册三条路由（静态 / 参数 / 通配） ---------------------
    std::printf("\n[1] 运行时注册 3 条路由并命中\n");
    RouteSpec static_route;
    static_route.path_pattern = "/rt/static";
    static_route.upstream = upstream_a.address();
    static_route.timeout = std::chrono::milliseconds(500);
    AddRouteChecked(gateway, static_route);

    RouteSpec param_route;
    param_route.path_pattern = "/rt/item/:id";
    param_route.upstream = upstream_a.address();
    AddRouteChecked(gateway, param_route);

    RouteSpec wildcard_route;
    wildcard_route.path_pattern = "/rt/**";
    wildcard_route.upstream = upstream_a.address();
    AddRouteChecked(gateway, wildcard_route);

    Check(StatusOf(Call(port, "/rt/static")) == 200, "静态路由命中");
    const http::Result param_result = Call(port, "/rt/item/42");
    Check(StatusOf(param_result) == 200, "参数路由命中");
    Check(param_result.ok() && param_result.response &&
              param_result.response->header.get("X-Path-Id") == "42",
          "参数路由 path_values['id'] == 42");
    Check(HasBody(Call(port, "/rt/other/x"), "target=/rt/other/x"), "通配路由命中");

    // ---- 2. 原有路由场景 -------------------------------------------------
    std::printf("\n[2] 原有场景：命中 / 404 / 鉴权 / 透传 / 超时 / 502\n");
    RouteSpec echo_route;
    echo_route.method_mask = method_mask::kAll;
    echo_route.path_pattern = "/api/echo/**";
    echo_route.upstream = upstream_a.address();
    AddRouteChecked(gateway, echo_route);

    RouteSpec private_route;
    private_route.path_pattern = "/api/private";
    private_route.upstream = upstream_a.address();
    private_route.require_auth = true;
    AddRouteChecked(gateway, private_route);

    RouteSpec limited_route;
    limited_route.path_pattern = "/api/limited";
    limited_route.upstream = upstream_a.address();
    AddRouteChecked(gateway, limited_route);

    RouteSpec slow_route;
    slow_route.path_pattern = "/api/slow";
    slow_route.upstream = upstream_a.address();
    slow_route.timeout = std::chrono::milliseconds(200);
    AddRouteChecked(gateway, slow_route);

    RouteSpec bad_route;
    bad_route.path_pattern = "/api/bad";
    bad_route.upstream = "127.0.0.1:1";
    AddRouteChecked(gateway, bad_route);

    const http::Result ok_result = Call(port, "/api/echo?x=1");
    Check(StatusOf(ok_result) == 200, "路由命中 200");
    Check(ok_result.ok() && ok_result.response->header.get("X-Gateway") == "test",
          "decorate 追加响应头");

    Check(StatusOf(Call(port, "/nope")) == 404, "未匹配 404");
    Check(HasBody(Call(port, "/nope"), "\"code\":404"), "404 错误格式");

    const http::Result unauthorized = Call(port, "/api/private");
    Check(StatusOf(unauthorized) == 401, "鉴权缺失 401");
    Check(HasBody(unauthorized, "\"code\":401"), "401 错误格式");
    Check(HasBody(Call(port, "/api/private", http::Method::Get, {},
                       {{"Authorization", "Bearer good"}}),
                  "user=42"),
          "鉴权成功注入 X-User-Id");

    const http::Result limited = Call(port, "/api/limited");
    Check(StatusOf(limited) == 429, "限流 429");

    const http::Result passthrough = Call(
        port, "/api/echo/path?x=1&y=2", http::Method::Post, "hello-body");
    Check(StatusOf(passthrough) == 200, "POST 透传状态");
    Check(HasBody(passthrough, "method=POST"), "方法透传");
    Check(HasBody(passthrough, "target=/api/echo/path?x=1&y=2"), "路径 + query 透传");
    Check(HasBody(passthrough, "body=hello-body"), "body 透传");

    const http::Result timeout_result = Call(port, "/api/slow");
    Check(StatusOf(timeout_result) == 504, "上游超时 504");
    Check(HasBody(timeout_result, "\"code\":504"), "504 错误格式");
    Check(gateway.stats().timeouts >= 1, "stats.timeouts >= 1");

    const http::Result bad_gateway = Call(port, "/api/bad");
    Check(StatusOf(bad_gateway) == 502, "上游不可达 502");
    Check(HasBody(bad_gateway, "\"code\":502"), "502 错误格式");
    Check(gateway.stats().upstream_errors >= 1, "stats.upstream_errors >= 1");

    // ---- 3. 中间件顺序 ---------------------------------------------------
    std::printf("\n[3] 中间件洋葱顺序\n");
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.clear();
    }
    Call(port, "/rt/static");
    std::vector<std::string> snapshot;
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        snapshot = order;
    }
    const std::vector<std::string> expected = {"before-1", "before-2", "after-2", "after-1"};
    Check(snapshot == expected, "中间件 before-1→before-2→after-2→after-1");

    // ---- 4. 自定义 Matcher 与 Resolver ----------------------------------
    std::printf("\n[4] 自定义 Matcher 与可插拔 Resolver\n");
    gateway.AddRoute(std::make_shared<TenantMatcher>(),
                     std::make_shared<Handler>([&upstream_a](Request& request,
                                                             Response& response) {
                         http::Client client(std::chrono::milliseconds(500));
                         const auto result = client.do_request(request, "127.0.0.1",
                                                               upstream_a.port());
                         if (!result.ok() || !result.response) {
                             response = ErrorResponseForTest(502, "tenant upstream down");
                             return Result<void>::Ok();
                         }
                         response.status = result.response->status;
                         response.body = result.response->body;
                         const auto found = request.path_values.find("tenant");
                         if (found != request.path_values.end()) {
                             response.header.set("X-Tenant-Param", found->second);
                         }
                         return Result<void>::Ok();
                     }));
    RouteSpec tenant_static;
    tenant_static.path_pattern = "/tenant/probe";
    tenant_static.upstream = upstream_a.address();
    AddRouteChecked(gateway, tenant_static);
    const http::Result tenant_result =
        Call(port, "/tenant/probe", http::Method::Get, {}, {{"X-Tenant", "a"}});
    Check(HasBody(tenant_result, "server=A"), "自定义 Matcher 命中并转发到上游 A");
    Check(tenant_result.ok() && tenant_result.response &&
              tenant_result.response->header.get("X-Tenant-Param") == "a",
          "自定义 Matcher 参数 path_values['tenant'] == a");
    Check(HasBody(Call(port, "/tenant/probe"), "server=A"), "无租户头时静态路由命中");

    RouteSpec resolve_route;
    resolve_route.path_pattern = "/api/resolve";
    resolve_route.upstream = upstream_a.address();
    AddRouteChecked(gateway, resolve_route);
    Check(HasBody(Call(port, "/api/resolve"), "server=A"), "Resolver 默认使用 RouteSpec.upstream");
    gateway.SetResolver([&](const Request&) {
        Upstream upstream;
        upstream.host = "127.0.0.1";
        upstream.port = upstream_b.port();
        upstream.timeout = std::chrono::milliseconds(500);
        return Result<Upstream>::Ok(std::move(upstream));
    });
    Check(HasBody(Call(port, "/api/resolve"), "server=B"), "Resolver 切换后同一路由换上游 B");
    gateway.SetResolver(nullptr);

    // ---- 5. LoadConfig 热加载 3 条 ---------------------------------------
    std::printf("\n[5] LoadConfig 从 JSON 加载 3 条路由\n");
    const std::string config_json =
        "{\"routes\":["
        "{\"path\":\"/api/cfg/one\",\"upstream\":\"" + upstream_a.address() +
        "\",\"method\":\"GET\"},"
        "{\"path\":\"/api/cfg/two/:id\",\"upstream\":\"" + upstream_a.address() +
        "\",\"method\":[\"GET\",\"POST\"]},"
        "{\"path\":\"/api/cfg/**\",\"upstream\":\"" + upstream_a.address() +
        "\",\"method\":\"*\",\"timeout_ms\":500}"
        "]}";
    const auto loaded = gateway.LoadConfig(config_json);
    Check(loaded.ok(), "LoadConfig 解析 3 条成功");
    Check(HasBody(Call(port, "/api/cfg/one"), "server=A"), "LoadConfig 静态路由");
    Check(HasBody(Call(port, "/api/cfg/two/7"), "target=/api/cfg/two/7"), "LoadConfig 参数路由");
    Check(HasBody(Call(port, "/api/cfg/other/x"), "target=/api/cfg/other/x"),
          "LoadConfig 通配路由");
    const auto incremental = gateway.LoadConfig(
        "{\"routes\":[{\"path\":\"/api/cfg/three\",\"upstream\":\"" +
        upstream_a.address() + "\",\"method\":\"GET\"}]}");
    Check(incremental.ok(), "LoadConfig 增量加载");
    Check(HasBody(Call(port, "/api/cfg/three"), "server=A"), "增量新增路由生效");
    Check(HasBody(Call(port, "/api/cfg/one"), "server=A"), "旧路由在增量加载后保留");

    // ---- 6. 聚合健康 -----------------------------------------------------
    std::printf("\n[6] /health 与 /api/health/all\n");
    Check(HasBody(Call(port, "/health"), "\"service\":\"gateway\""), "自身 /health");
    RouteSpec health_a;
    health_a.path_pattern = "/api/health/a";
    health_a.upstream = upstream_a.address();
    AddRouteChecked(gateway, health_a);
    RouteSpec health_b;
    health_b.path_pattern = "/api/health/b";
    health_b.upstream = upstream_b.address();
    AddRouteChecked(gateway, health_b);
    const http::Result health_all = Call(port, "/api/health/all");
    Check(StatusOf(health_all) == 200, "聚合健康 200");
    Check(HasBody(health_all, "\"" + upstream_a.address() + "\":{\"ok\":true"),
          "聚合健康 A ok=true");
    Check(HasBody(health_all, "\"" + upstream_b.address() + "\":{\"ok\":true"),
          "聚合健康 B ok=true");

    // ---- 7. 压测 ---------------------------------------------------------
    std::printf("\n[7] 压测 1000 次转发\n");
    record_order.store(false, std::memory_order_relaxed);
    constexpr int kRequests = 1000;
    g_latencies_us.reserve(kRequests);
    int bench_failures = 0;
    const auto bench_begin = std::chrono::steady_clock::now();
    for (int i = 0; i < kRequests; ++i) {
        const auto started = std::chrono::steady_clock::now();
        const http::Result result = Call(port, "/rt/static");
        const auto finished = std::chrono::steady_clock::now();
        g_latencies_us.push_back(
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    finished - started)
                                    .count()));
        if (StatusOf(result) != 200) {
            ++bench_failures;
        }
    }
    const auto bench_end = std::chrono::steady_clock::now();
    const double elapsed_s =
        std::chrono::duration<double>(bench_end - bench_begin).count();
    std::vector<double> sorted = g_latencies_us;
    std::sort(sorted.begin(), sorted.end());
    const double p50 = sorted[static_cast<std::size_t>(sorted.size() * 50 / 100)];
    const double p99 = sorted[static_cast<std::size_t>(sorted.size() * 99 / 100)];
    std::printf("[bench] requests=%d qps=%.1f p50=%.0fus p99=%.0fus failures=%d\n",
                kRequests, static_cast<double>(kRequests) / elapsed_s, p50, p99,
                bench_failures);
    Check(bench_failures == 0, "压测 1000 次全部 200");
    Check(elapsed_s > 0.0, "压测耗时有效");

    // ---- 8. 聚合健康失败项 ----------------------------------------------
    std::printf("\n[8] 上游 B 停止后的聚合健康\n");
    upstream_b.Stop();
    const http::Result health_down = Call(port, "/api/health/all");
    Check(StatusOf(health_down) == 200, "聚合健康在部分失败时仍 200");
    Check(HasBody(health_down, "\"" + upstream_a.address() + "\":{\"ok\":true"),
          "A 仍 ok=true");
    Check(HasBody(health_down, "\"" + upstream_b.address() + "\":{\"ok\":false"),
          "B 标记 ok=false");
    Check(HasBody(health_down, "\"ok\":false,\"services\""), "整体 ok=false");

    // ---- 9. TOP1：handler 抛异常 → 500，进程不崩 -------------------------
    std::printf("\n[9] handler 抛异常 → 500（进程不崩）\n");
    auto throw_matcher = std::make_shared<ThrowMatcher>();
    auto throw_handler = std::make_shared<Handler>([](Request&, Response&) -> Result<void> {
        throw std::runtime_error("boom from test handler");
    });
    Check(gateway.AddRoute(throw_matcher, throw_handler).ok(), "注册抛异常路由");
    const http::Result boom = Call(port, "/boom");
    Check(StatusOf(boom) == 500, "throw handler 返回 500（修复前 SIGABRT）");
    Check(HasBody(boom, "\"code\":500"), "500 错误体格式");
    Check(gateway.stats().internal_errors >= 1, "internal_errors 计数 +1");
    Check(StatusOf(Call(port, "/health")) == 200, "异常请求后进程仍可服务");

    // ---- 10. TOP2：worker 被半开连接占满时，新请求受总时限约束 -----------
    std::printf("\n[10] worker=2 + 2 条半开连接 → 新请求在总时限内响应\n");
    GatewayHarness slow_harness;
    slow_harness.config.listen_host = "127.0.0.1";
    slow_harness.config.port = 0;
    slow_harness.config.worker_threads = 2;
    slow_harness.config.request_timeout = std::chrono::milliseconds(800);
    if (slow_harness.Start()) {
        const std::uint16_t slow_port = slow_harness.gateway->port();
        const int slow1 = OpenRaw(slow_port);
        const int slow2 = OpenRaw(slow_port);
        const std::string half = "GET /health HTTP/1.1\r\nHost: slow\r\n";
        Check(slow1 >= 0 && slow2 >= 0 && SendRaw(slow1, half) && SendRaw(slow2, half),
              "2 条半开连接已占住 worker");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto probe_begin = std::chrono::steady_clock::now();
        const http::Result probe = Call(slow_port, "/health");
        const auto probe_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - probe_begin)
                                  .count();
        std::printf("  [slowloris] probe=%lldms status=%d\n",
                    static_cast<long long>(probe_ms), StatusOf(probe));
        Check(StatusOf(probe) == 200, "半开连接占满 worker 后新请求仍成功");
        Check(probe_ms <= 5000, "新请求在总时限内响应（修复前约 30s）");
        Check(slow_harness.gateway->stats().request_timeouts >= 1,
              "request_timeouts 记录了慢连接淘汰");
        if (slow1 >= 0) {
            ::close(slow1);
        }
        if (slow2 >= 0) {
            ::close(slow2);
        }
        slow_harness.Stop();
    }

    // ---- 11. TOP3：同一次 send 3 个 pipeline 请求 → 3 个响应 -------------
    std::printf("\n[11] 同包流水线 3 请求 → 3 响应\n");
    const int pipeline_fd = OpenRaw(port);
    if (pipeline_fd >= 0) {
        const std::string triple =
            "GET /health HTTP/1.1\r\nHost: p\r\n\r\n"
            "GET /health HTTP/1.1\r\nHost: p\r\n\r\n"
            "GET /health HTTP/1.1\r\nHost: p\r\n\r\n";
        Check(SendRaw(pipeline_fd, triple), "一次 send 写入 3 个请求");
        const std::string responses = ReadUntilEof(pipeline_fd);
        ::close(pipeline_fd);
        std::size_t response_count = 0;
        for (std::size_t pos = 0;
             (pos = responses.find("HTTP/1.1 200", pos)) != std::string::npos;
             pos += 12) {
            ++response_count;
        }
        std::printf("  [pipeline] responses=%zu bytes=%zu\n", response_count,
                    responses.size());
        Check(response_count == 3, "3 个 pipeline 请求都得到响应（修复前只回 1 个）");
    }

    // ---- 12. G4：Expect: 100-continue → 先 100，再读 body，最后 200 ------
    std::printf("\n[12] G4 Expect: 100-continue → 先 100 再 200\n");
    {
        const int expect_fd = OpenRaw(port);
        if (expect_fd >= 0) {
            const std::string head_only =
                "POST /api/echo/g4 HTTP/1.1\r\nHost: e\r\n"
                "Content-Length: 5\r\nExpect: 100-continue\r\n\r\n";
            Check(SendRaw(expect_fd, head_only), "发送带 Expect 的请求头");
            const std::string interim = ReadHeaderBlock(expect_fd);
            std::printf("  [100-continue] interim=%zu bytes\n", interim.size());
            Check(interim.rfind("HTTP/1.1 100 Continue", 0) == 0,
                  "读 body 前先收到 100 Continue（修复前直接等 body 超时/关闭）");
            Check(SendRaw(expect_fd, "hello"), "收到 100 后再发 body");
            const std::string final_response = ReadUntilEof(expect_fd);
            ::close(expect_fd);
            Check(final_response.find("HTTP/1.1 200") != std::string::npos,
                  "body 到达后返回 200");
            Check(final_response.find("body=hello") != std::string::npos,
                  "Expect 请求的 body 被完整转发");
        }
    }

    // ---- 13. G7：畸形请求 → 400 再关 --------------------------------------
    std::printf("\n[13] G7 畸形请求 → 400 再关\n");
    {
        const int bad_fd = OpenRaw(port);
        if (bad_fd >= 0) {
            Check(SendRaw(bad_fd, "GET /health HTTP/1.1\r\nBrokenHeader\r\n\r\n"),
                  "发送缺冒号的畸形头");
            const std::string response = ReadUntilEof(bad_fd);
            ::close(bad_fd);
            std::printf("  [400] bytes=%zu\n", response.size());
            Check(response.find("HTTP/1.1 400") != std::string::npos,
                  "畸形请求得到 400（修复前静默断连）");
            Check(response.find("\"code\":400") != std::string::npos,
                  "400 错误体格式");
            Check(response.find("Connection: close") != std::string::npos,
                  "400 声明 Connection: close");
        }
    }

    // ---- 14. G6：队列满 → 503 + Connection: close -------------------------
    std::printf("\n[14] G6 背压：队列满 → 503\n");
    {
        GatewayHarness busy;
        busy.config.listen_host = "127.0.0.1";
        busy.config.port = 0;
        busy.config.worker_threads = 1;
        busy.config.max_queued_connections = 1;
        busy.config.request_timeout = std::chrono::milliseconds(5000);
        if (busy.Start()) {
            const auto entered = std::make_shared<std::atomic<bool>>(false);
            const auto release = std::make_shared<std::atomic<bool>>(false);
            struct BlockMatcher final : Matcher {
                bool Match(const Request& request, PathParams*) const override {
                    return request.path() == "/block";
                }
            };
            Check(busy.gateway
                      ->AddRoute(std::make_shared<BlockMatcher>(),
                                 std::make_shared<Handler>(
                                     [entered, release](Request&, Response& response) {
                                         entered->store(true);
                                         while (!release->load()) {
                                             std::this_thread::sleep_for(
                                                 std::chrono::milliseconds(5));
                                         }
                                         response.status = http::Status::OK;
                                         response.body = "released";
                                         return Result<void>::Ok();
                                     }))
                      .ok(),
                  "注册阻塞路由 /block");
            const std::uint16_t busy_port = busy.gateway->port();
            const int blocker_fd = OpenRaw(busy_port);
            const int queued_fd = OpenRaw(busy_port);
            Check(blocker_fd >= 0 && queued_fd >= 0, "占满唯一 worker + 占 1 个队列槽");
            if (blocker_fd >= 0) {
                Check(SendRaw(blocker_fd,
                              "GET /block HTTP/1.1\r\nHost: b\r\n\r\n"),
                      "发送阻塞请求占住 worker");
            }
            bool in_handler = false;
            for (int i = 0; i < 400 && !in_handler; ++i) {
                if (entered->load()) {
                    in_handler = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Check(in_handler, "worker 已进入阻塞 handler");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // 队列满的判定依赖 accept 线程时序：连续建连探测，直到某条连接被
            // 拒绝（503）；未被拒绝的连接会占住队列槽，因此最多几次必中。
            std::string rejected;
            for (int attempt = 0; attempt < 8 && rejected.empty(); ++attempt) {
                const int probe_fd = OpenRaw(busy_port);
                if (probe_fd < 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }
                const std::string chunk = ReadAvailable(probe_fd, 300);
                ::close(probe_fd);
                if (chunk.find("HTTP/1.1 503") != std::string::npos) {
                    rejected = chunk;
                }
            }
            std::printf("  [backpressure] bytes=%zu\n", rejected.size());
            Check(rejected.find("HTTP/1.1 503") != std::string::npos,
                  "队列满的新连接得到 503（修复前静默 close）");
            Check(rejected.find("Connection: close") != std::string::npos,
                  "503 声明 Connection: close");
            release->store(true);
            if (blocker_fd >= 0) {
                const std::string blocker_response = ReadUntilEof(blocker_fd);
                ::close(blocker_fd);
                Check(blocker_response.find("HTTP/1.1 200") != std::string::npos,
                      "释放后阻塞请求正常 200");
            }
            if (queued_fd >= 0) {
                ::close(queued_fd);
            }
        }
        busy.Stop();
    }

    // ---- 汇总 ------------------------------------------------------------
    const Stats stats = gateway.stats();
    std::printf(
        "\n[stats] requests=%llu forwarded=%llu unauthorized=%llu rate_limited=%llu "
        "not_found=%llu upstream_errors=%llu timeouts=%llu health_checks=%llu\n",
        static_cast<unsigned long long>(stats.requests),
        static_cast<unsigned long long>(stats.forwarded),
        static_cast<unsigned long long>(stats.unauthorized),
        static_cast<unsigned long long>(stats.rate_limited),
        static_cast<unsigned long long>(stats.not_found),
        static_cast<unsigned long long>(stats.upstream_errors),
        static_cast<unsigned long long>(stats.timeouts),
        static_cast<unsigned long long>(stats.health_checks));
    Check(stats.requests >= static_cast<std::uint64_t>(kRequests), "stats.requests 覆盖压测");
    Check(stats.unauthorized >= 1, "stats.unauthorized >= 1");
    Check(stats.rate_limited >= 1, "stats.rate_limited >= 1");
    Check(stats.not_found >= 1, "stats.not_found >= 1");
    Check(stats.health_checks >= 2, "stats.health_checks >= 2");
    Check(stats.internal_errors >= 1, "stats.internal_errors >= 1");

    harness.Stop();
    std::printf("\n[gateway] checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
