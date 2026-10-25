/**
 * @file test_monitor_boundaries.cpp
 * @brief `os_monitor_*` / `HttpMonitorClient` 边界测试：失败必须可诊断、不崩溃。
 *
 * 覆盖：
 * 1. Manager 未启动（连接拒绝）→ `kConnect`，消息含 URL、Manager 与启动提示；
 * 2. HTTP 500 → `kHttpStatus` + 状态码 + 响应片段，工具层 `isError=true`；
 * 3. HTTP 404（带 body）→ 状态码、路径提示与响应片段；
 * 4. 响应超时（服务端挂起 400ms，timeout=100ms）→ `kTimeout`，消息给出
 *    timeout_ms 与可调大提示，且调用在 1.5s 内返回（不会无限阻塞）；
 * 5. 畸形响应：非 JSON、截断响应、非 HTTP 垃圾字节 → 明确错误，进程不崩溃；
 * 6. Manager 地址边界：空串、缺主机、端口 0/超范围/非数字、带路径前缀；
 * 7. 空 `MonitorClient`（装配遗漏）→ 四个工具仍可见，调用返回配置错误；
 * 8. 所有失败路径之后，工具表仍完整可用（服务不因单次查询失败而崩溃）。
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_util.hpp"

#include "go2cpp/os/monitor/manager.hpp"
#include "mcp/server/registry.hpp"
#include "mcp/tools/monitor_client.hpp"
#include "mcp/tools/monitor_tools.hpp"

namespace {

using go2cpp::mcp::Registry;
using go2cpp::mcp::ToolContext;
using go2cpp::mcp::tools::HttpMonitorClient;
using go2cpp::mcp::tools::MonitorErrorKind;
using go2cpp::mcp::tools::MonitorOptions;
using go2cpp::mcp::tools::MonitorQuery;

/// 调用工具并返回 (is_error, text)。
std::pair<bool, std::string> CallTool(const Registry& registry,
                                      const std::string& name,
                                      const go2cpp::mcp::json& arguments) {
    ToolContext context;
    context.arguments = arguments;
    const auto result = registry.InvokeTool(name, context);
    if (!result) {
        return {true, "__missing__"};
    }
    return {result->is_error,
            result->content.empty() ? "" : result->content[0].text};
}

/// 断言文本包含关键诊断词；失败时把完整文本打出来，便于定位。
void CheckContains(const std::string& text, const std::string& needle,
                   const std::string& label) {
    CHECK_MSG(text.find(needle) != std::string::npos,
              label + "：期望包含「" + needle + "」，实际文本: " + text);
}

std::string KindName(MonitorErrorKind kind) {
    switch (kind) {
        case MonitorErrorKind::kNone:
            return "kNone";
        case MonitorErrorKind::kInvalidEndpoint:
            return "kInvalidEndpoint";
        case MonitorErrorKind::kConnect:
            return "kConnect";
        case MonitorErrorKind::kTimeout:
            return "kTimeout";
        case MonitorErrorKind::kParse:
            return "kParse";
        case MonitorErrorKind::kHttpStatus:
            return "kHttpStatus";
        case MonitorErrorKind::kResponseFormat:
            return "kResponseFormat";
    }
    return "unknown";
}

/// 极简假 HTTP 服务端：按模式返回故障响应，用于验证客户端错误分类。
class FaultServer {
public:
    enum class Mode {
        kHttp500,
        kHttp404,
        kSlow,
        kBadJson,
        kTruncated,
        kGarbage,
        kOk,
    };

    explicit FaultServer(Mode mode) : mode_(mode) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return;
        }
        const int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                          &length) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        port_ = ntohs(address.sin_port);
        accept_thread_ = std::thread([this] { AcceptLoop(); });
    }

    ~FaultServer() { Stop(); }

    FaultServer(const FaultServer&) = delete;
    FaultServer& operator=(const FaultServer&) = delete;

    bool valid() const { return listen_fd_ >= 0 && port_ != 0; }

    std::uint16_t port() const { return port_; }

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    /// 最近一次请求行里的 target（用于验证 base_url 路径前缀拼接）。
    std::string LastTarget() const {
        std::lock_guard<std::mutex> lock(target_mutex_);
        return last_target_;
    }

    void Stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

private:
    void AcceptLoop() {
        while (!stopped_.load()) {
            pollfd entry{};
            entry.fd = listen_fd_;
            entry.events = POLLIN;
            const int ready = ::poll(&entry, 1, 50);
            if (ready <= 0) {
                continue;
            }
            const int connection = ::accept(listen_fd_, nullptr, nullptr);
            if (connection < 0) {
                continue;
            }
            workers_.emplace_back([this, connection] { Serve(connection); });
        }
    }

    void Serve(int connection) {
        std::string request;
        char buffer[2048];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t received =
                ::recv(connection, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                break;
            }
            request.append(buffer, static_cast<std::size_t>(received));
            if (request.size() > 64 * 1024) {
                break;
            }
        }
        const std::size_t first_space = request.find(' ');
        const std::size_t second_space =
            first_space == std::string::npos
                ? std::string::npos
                : request.find(' ', first_space + 1);
        if (first_space != std::string::npos && second_space != std::string::npos) {
            std::lock_guard<std::mutex> lock(target_mutex_);
            last_target_ =
                request.substr(first_space + 1, second_space - first_space - 1);
        }

        switch (mode_) {
            case Mode::kHttp500:
                Write(connection, BuildResponse(500, "Internal Server Error",
                                                "fault: 500"));
                break;
            case Mode::kHttp404:
                Write(connection, BuildResponse(404, "Not Found",
                                                "{\"ok\":false,\"error\":\"no route\"}"));
                break;
            case Mode::kSlow:
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                Write(connection, BuildResponse(200, "OK", "[]"));
                break;
            case Mode::kBadJson:
                Write(connection, BuildResponse(200, "OK", "this is not json"));
                break;
            case Mode::kTruncated:
                Write(connection,
                      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                      "Content-Length: 100\r\nConnection: close\r\n\r\n"
                      "{\"snapshots\":");
                break;
            case Mode::kGarbage:
                Write(connection, "GARBAGE\r\n\r\n");
                break;
            case Mode::kOk:
                Write(connection, BuildResponse(200, "OK", "[]"));
                break;
        }
        ::shutdown(connection, SHUT_WR);
        ::close(connection);
    }

    static std::string BuildResponse(int status, const char* reason,
                                     const std::string& body) {
        return "HTTP/1.1 " + std::to_string(status) + " " + reason +
               "\r\nContent-Type: text/plain\r\nContent-Length: " +
               std::to_string(body.size()) +
               "\r\nConnection: close\r\n\r\n" + body;
    }

    static void Write(int fd, const std::string& data) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const ssize_t written =
                ::send(fd, data.data() + offset, data.size() - offset,
                       MSG_NOSIGNAL);
            if (written <= 0) {
                return;
            }
            offset += static_cast<std::size_t>(written);
        }
    }

    Mode mode_;
    int listen_fd_{-1};
    std::uint16_t port_{0};
    std::atomic<bool> stopped_{false};
    std::thread accept_thread_;
    std::vector<std::thread> workers_;
    mutable std::mutex target_mutex_;
    std::string last_target_;
};

/// 1) Manager 未启动：四个客户端方法与四个工具都必须给出可诊断错误。
void TestManagerDown() {
    go2cpp::os::monitor::ManagerConfig config;
    config.listen_address = "127.0.0.1";
    config.port = 0;
    go2cpp::os::monitor::Manager probe(config);
    CHECK_MSG(probe.Start(), "探测用 Manager 必须能启动");
    const std::uint16_t dead_port = probe.port();
    probe.Stop();
    CHECK_MSG(dead_port != 0, "探测 Manager 应报告内核分配端口");
    const std::string url = "http://127.0.0.1:" + std::to_string(dead_port);

    HttpMonitorClient client(MonitorOptions{url, 1000});
    MonitorQuery query;
    query.limit = 10;
    query.timeout_ms = 1000;

    struct Case {
        const char* name;
        std::function<go2cpp::mcp::tools::Result<go2cpp::mcp::json>(
            HttpMonitorClient&, const MonitorQuery&)>
            call;
    };
    const std::vector<Case> cases = {
        {"latest", [](HttpMonitorClient& c, const MonitorQuery& q) {
             return c.Latest(q);
         }},
        {"stats", [](HttpMonitorClient& c, const MonitorQuery& q) {
             return c.Stats(q);
         }},
        {"kernel", [](HttpMonitorClient& c, const MonitorQuery& q) {
             return c.Kernel(q);
         }},
        {"health", [](HttpMonitorClient& c, const MonitorQuery& q) {
             return c.Health(q);
         }},
    };
    for (const auto& item : cases) {
        const auto result = item.call(client, query);
        const std::string label =
            std::string("Manager 未启动/") + item.name;
        CHECK_MSG(!result.ok, label + "：必须失败");
        CHECK_MSG(result.error.kind == MonitorErrorKind::kConnect,
                  label + "：错误种类应为 kConnect，实际 " +
                      KindName(result.error.kind));
        CheckContains(result.error.message, "无法访问 Manager", label);
        CheckContains(result.error.message, "Manager 已启动", label);
        CheckContains(result.error.message, url, label);
    }

    // 工具层：四个工具都要 isError，且文本原样包含客户端诊断。
    Registry registry;
    go2cpp::mcp::tools::RegisterMonitorTools(
        registry, MonitorOptions{url, 1000});
    for (const char* name : {"os_monitor_latest", "os_monitor_stats",
                             "os_monitor_kernel", "os_monitor_health"}) {
        const auto result = CallTool(registry, name, go2cpp::mcp::json::object());
        const std::string label = std::string("工具 ") + name;
        CHECK_MSG(result.first, label + "：Manager 未启动时必须 isError");
        CheckContains(result.second, "无法访问 Manager", label);
        CheckContains(result.second, "Manager 已启动", label);
    }
}

/// 2) HTTP 500 / 404：状态码、响应片段与工具层错误文本。
void TestHttpStatus() {
    {
        FaultServer server(FaultServer::Mode::kHttp500);
        CHECK_MSG(server.valid(), "假 500 服务端启动失败");
        HttpMonitorClient client(MonitorOptions{server.url(), 1000});
        MonitorQuery query;
        query.timeout_ms = 1000;
        const auto result = client.Health(query);
        CHECK_MSG(!result.ok, "HTTP 500 必须失败");
        CHECK_MSG(result.error.kind == MonitorErrorKind::kHttpStatus,
                  "HTTP 500 错误种类应为 kHttpStatus，实际 " +
                      KindName(result.error.kind));
        CHECK_MSG(result.error.http_status == 500,
                  "http_status 应为 500，实际 " +
                      std::to_string(result.error.http_status));
        CheckContains(result.error.message, "Manager 返回 HTTP 500", "HTTP 500");
        CheckContains(result.error.message, "响应片段: fault: 500", "HTTP 500");
        CheckContains(result.error.message, server.url(), "HTTP 500");

        // 工具层：不崩溃 + isError + 状态码透传。
        Registry registry;
        go2cpp::mcp::tools::RegisterMonitorTools(
            registry, MonitorOptions{server.url(), 1000});
        const auto tool = CallTool(registry, "os_monitor_latest",
                                   go2cpp::mcp::json::object());
        CHECK_MSG(tool.first, "HTTP 500 时工具必须 isError");
        CheckContains(tool.second, "Manager 返回 HTTP 500", "HTTP 500 工具");
        CheckContains(tool.second, "fault: 500", "HTTP 500 工具");
    }
    {
        FaultServer server(FaultServer::Mode::kHttp404);
        HttpMonitorClient client(MonitorOptions{server.url(), 1000});
        const auto result = client.Latest(MonitorQuery{});
        CHECK_MSG(!result.ok, "HTTP 404 必须失败");
        CHECK_MSG(result.error.http_status == 404,
                  "http_status 应为 404，实际 " +
                      std::to_string(result.error.http_status));
        CheckContains(result.error.message, "Manager 返回 HTTP 404", "HTTP 404");
        CheckContains(result.error.message, "路径不存在", "HTTP 404");
        CheckContains(result.error.message, "no route", "HTTP 404");
    }
}

/// 3) 超时：客户端必须按 timeout_ms 返回，且诊断里给出该值。
void TestTimeout() {
    FaultServer server(FaultServer::Mode::kSlow);
    CHECK_MSG(server.valid(), "假慢服务端启动失败");
    HttpMonitorClient client(MonitorOptions{server.url(), 100});
    MonitorQuery query;
    query.limit = 10;
    query.timeout_ms = 100;

    const auto started = std::chrono::steady_clock::now();
    const auto result = client.Latest(query);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    CHECK_MSG(!result.ok, "超时必须失败");
    CHECK_MSG(result.error.kind == MonitorErrorKind::kTimeout,
              "超时错误种类应为 kTimeout，实际 " + KindName(result.error.kind));
    CheckContains(result.error.message, "100ms 内未响应", "超时");
    CheckContains(result.error.message, "可增大 timeout_ms", "超时");
    CHECK_MSG(elapsed < 1500,
              "超时调用应在 1.5s 内返回，实际 " + std::to_string(elapsed) +
                  "ms；错误: " + result.error.message);

    // 工具层：同一故障下也必须快速失败且给出 timeout 提示。
    Registry registry;
    go2cpp::mcp::tools::RegisterMonitorTools(
        registry, MonitorOptions{server.url(), 100});
    const auto tool = CallTool(registry, "os_monitor_stats",
                               go2cpp::mcp::json::object());
    CHECK_MSG(tool.first, "超时工具调用必须 isError");
    CheckContains(tool.second, "未响应", "超时工具");
    CheckContains(tool.second, "timeout_ms", "超时工具");
}

/// 4) 畸形响应：非 JSON / 截断 / 垃圾字节。
void TestMalformedResponses() {
    {
        FaultServer server(FaultServer::Mode::kBadJson);
        HttpMonitorClient client(MonitorOptions{server.url(), 1000});
        const auto result = client.Latest(MonitorQuery{});
        CHECK_MSG(!result.ok, "非 JSON 响应必须失败");
        CHECK_MSG(result.error.kind == MonitorErrorKind::kParse,
                  "非 JSON 应为 kParse，实际 " + KindName(result.error.kind));
        CheckContains(result.error.message, "不是合法 JSON", "非 JSON");

        Registry registry;
        go2cpp::mcp::tools::RegisterMonitorTools(
            registry, MonitorOptions{server.url(), 1000});
        const auto tool = CallTool(registry, "os_monitor_latest",
                                   go2cpp::mcp::json::object());
        CHECK_MSG(tool.first, "非 JSON 时工具必须 isError");
        CheckContains(tool.second, "不是合法 JSON", "非 JSON 工具");
    }
    {
        FaultServer server(FaultServer::Mode::kTruncated);
        HttpMonitorClient client(MonitorOptions{server.url(), 1000});
        const auto result = client.Latest(MonitorQuery{});
        CHECK_MSG(!result.ok, "截断响应必须失败");
        // 截断在 HTTP 层表现为连接提前关闭（Receive/Closed），统一收敛为
        // 「无法访问 Manager」；不允许静默当成成功。
        CheckContains(result.error.message, "无法访问 Manager", "截断响应");
        CheckContains(result.error.message, server.url(), "截断响应");
    }
    {
        FaultServer server(FaultServer::Mode::kGarbage);
        HttpMonitorClient client(MonitorOptions{server.url(), 1000});
        const auto result = client.Latest(MonitorQuery{});
        CHECK_MSG(!result.ok, "非 HTTP 响应必须失败");
        CheckContains(result.error.message, "无法访问 Manager", "垃圾字节");
    }
}

/// 5) 地址边界与路径前缀。
void TestEndpointValidation() {
    // 空地址必须由客户端默认地址触发（query.base_url 为空表示「沿用默认」，
    // 因此要单独构造一个默认地址为空的客户端）。
    {
        HttpMonitorClient empty_client(MonitorOptions{"", 1000});
        const auto result = empty_client.Health(MonitorQuery{});
        CHECK_MSG(!result.ok, "空默认地址必须失败");
        CHECK_MSG(result.error.kind == MonitorErrorKind::kInvalidEndpoint,
                  "空默认地址应为 kInvalidEndpoint，实际 " +
                      KindName(result.error.kind));
        CheckContains(result.error.message, "Manager 地址为空", "空默认地址");
    }

    HttpMonitorClient client(MonitorOptions{"http://127.0.0.1:9101", 1000});
    MonitorQuery query;
    query.timeout_ms = 1000;
    struct Case {
        const char* url;
        const char* needle;
    };
    const std::vector<Case> invalid = {
        {"ftp://127.0.0.1:9101", "仅支持 http://"},
        {"http://", "缺少主机名"},
        {"http://127.0.0.1:0", "端口超出范围"},
        {"http://127.0.0.1:99999", "端口超出范围"},
        {"http://127.0.0.1:abc", "端口不是数字"},
        {"http://:9101", "地址格式错误"},
    };
    for (const auto& item : invalid) {
        query.base_url = item.url;
        const auto result = client.Health(query);
        const std::string label = std::string("非法地址「") + item.url + "」";
        CHECK_MSG(!result.ok, label + "：必须失败");
        CHECK_MSG(result.error.kind == MonitorErrorKind::kInvalidEndpoint,
                  label + "：错误种类应为 kInvalidEndpoint，实际 " +
                      KindName(result.error.kind));
        CheckContains(result.error.message, item.needle, label);
    }

    // 带路径前缀的 base_url 必须拼在被查询路径之前（不允许吞掉前缀）。
    FaultServer server(FaultServer::Mode::kOk);
    CHECK_MSG(server.valid(), "路径前缀用例的假服务端启动失败");
    query.base_url = server.url() + "/edge/";
    const auto result = client.Health(query);
    CHECK_MSG(result.ok, "带路径前缀的合法地址应成功");
    CHECK_MSG(server.LastTarget() == "/edge/api/health",
              "路径前缀拼接错误，实际 target=" + server.LastTarget());

    // 末尾多个斜杠也应规范化，不产生 `//api/health`。
    query.base_url = server.url() + "///";
    const auto second = client.Health(query);
    CHECK_MSG(second.ok, "多个尾部斜杠的地址应成功");
    CHECK_MSG(server.LastTarget() == "/api/health",
              "尾部斜杠规范化错误，实际 target=" + server.LastTarget());
}

/// 6) 空客户端：装配遗漏不是崩溃，而是四个可见工具 + 明确配置错误。
void TestMissingClient() {
    Registry registry;
    go2cpp::mcp::tools::RegisterMonitorTools(registry, nullptr);
    CHECK_MSG(registry.tool_count() == 4,
              "空客户端时仍应注册 4 个工具，实际 " +
                  std::to_string(registry.tool_count()));
    for (const char* name : {"os_monitor_latest", "os_monitor_stats",
                             "os_monitor_kernel", "os_monitor_health"}) {
        const auto result = CallTool(registry, name, go2cpp::mcp::json::object());
        const std::string label = std::string("空客户端/") + name;
        CHECK_MSG(result.first, label + "：必须 isError");
        CheckContains(result.second, "MonitorClient 未配置", label);
        CheckContains(result.second, "RegisterMonitorTools", label);
    }
}

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    TestManagerDown();
    TestHttpStatus();
    TestTimeout();
    TestMalformedResponses();
    TestEndpointValidation();
    TestMissingClient();

    // 所有失败路径跑完后，工具表仍然完整（没有因异常/崩溃被破坏）。
    Registry registry;
    go2cpp::mcp::tools::RegisterMonitorTools(
        registry, MonitorOptions{"http://127.0.0.1:1", 100});
    CHECK_MSG(registry.tool_count() == 4,
              "失败路径之后工具表应仍为 4 个工具");

    return go2cpp::mcp::test::Summary("monitor_boundaries");
}
