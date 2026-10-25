/**
 * @file test_monitor_client.cpp
 * @brief `MonitorClient` 解耦单测：MockMonitorClient 注入四个工具，全程离线。
 *
 * 覆盖：
 * 1. 只依赖接口注册：4 个工具、schema 参数、空客户端兜底；
 * 2. latest / stats / kernel / health 的 happy path 与 JSON 结构；
 * 3. 工具参数 → MonitorQuery 的透传与越界 clamp（manager_url/timeout/limit）；
 * 4. Mock 可编程数据与错误注入：错误文本原样返回、其它工具不受影响；
 * 5. 响应格式异常（非数组/非对象）的防御；
 * 6. HttpMonitorClient 的地址校验（非法 scheme/端口，不发请求）。
 *
 * 本测试不启动 Manager、不打开 socket，MockMonitorClient 不包含网络实现。
 */

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "test_util.hpp"

#include "mcp/tools/monitor_client.hpp"
#include "mcp/tools/monitor_tools.hpp"
#include "mcp/validation/validators.hpp"

namespace {

using go2cpp::mcp::json;
using go2cpp::mcp::Registry;
using go2cpp::mcp::ToolContext;
using go2cpp::mcp::tools::MockMonitorClient;
using go2cpp::mcp::tools::MonitorErrorKind;
using go2cpp::mcp::tools::MonitorOptions;
using go2cpp::mcp::tools::MonitorQuery;

/// 调用工具并返回 (is_error, text)。
std::pair<bool, std::string> CallTool(const Registry& registry,
                                      const std::string& name,
                                      const json& arguments) {
    ToolContext context;
    context.arguments = arguments;
    const auto result = registry.InvokeTool(name, context);
    if (!result) {
        return {true, "__missing__"};
    }
    return {result->is_error,
            result->content.empty() ? "" : result->content[0].text};
}

json ParseResult(const std::string& text) {
    try {
        return json::parse(text);
    } catch (const std::exception&) {
        return json(nullptr);
    }
}

bool HasTool(const Registry& registry, const std::string& name) {
    const json list = registry.ToolList();
    for (const auto& item : list["tools"]) {
        if (item.value("name", std::string{}) == name) {
            return true;
        }
    }
    return false;
}

bool HasKey(const json& object, const char* key) {
    return object.is_object() && object.find(key) != object.end();
}

void CheckRegistration(const Registry& registry) {
    for (const char* name : {"os_monitor_latest", "os_monitor_stats",
                             "os_monitor_kernel", "os_monitor_health"}) {
        CHECK(HasTool(registry, name));
    }
    CHECK_EQ(std::to_string(registry.tool_count()), "4");
}

void CheckSchema(const Registry& registry) {
    const auto latest = registry.ToolSchema("os_monitor_latest");
    CHECK(latest.has_value());
    if (!latest) {
        return;
    }
    const auto validate = [&latest](const json& arguments) {
        return go2cpp::mcp::validation::ValidateToolArguments(*latest, arguments)
            .ok;
    };
    CHECK(validate(json::object()));
    CHECK(validate({{"limit", 10},
                    {"timeout_ms", 3000},
                    {"manager_url", "http://127.0.0.1:9101"}}));
    CHECK(!validate({{"limit", 0}}));
    CHECK(!validate({{"limit", 257}}));
    CHECK(!validate({{"timeout_ms", 50}}));
    CHECK(!validate({{"timeout_ms", 60001}}));
    CHECK(!validate({{"manager_url", "ftp://127.0.0.1:9101"}}));
    CHECK(!validate({{"unexpected", 1}}));
    CHECK(!validate({{"limit", "10"}}));

    const auto health = registry.ToolSchema("os_monitor_health");
    CHECK(health.has_value());
    if (!health) {
        return;
    }
    CHECK(go2cpp::mcp::validation::ValidateToolArguments(*health, json::object())
              .ok);
    CHECK(!go2cpp::mcp::validation::ValidateToolArguments(*health,
                                                          {{"limit", 1}})
               .ok);
}

}  // namespace

int main() {
    using go2cpp::mcp::test::Summary;

    // ------------------------------------------------------------------
    // 1. Mock 注入：工具注册不依赖 Manager/网络。
    // ------------------------------------------------------------------
    auto client = std::make_shared<MockMonitorClient>();
    Registry registry;
    go2cpp::mcp::tools::RegisterMonitorTools(registry, client);
    CheckRegistration(registry);
    CheckSchema(registry);
    CHECK_EQ(client->BaseUrl(), "mock://monitor");

    // ------------------------------------------------------------------
    // 2. latest：默认 limit=10，Mock 按 Manager 的 ?n= 语义返回 2 条。
    // ------------------------------------------------------------------
    {
        const auto result =
            CallTool(registry, "os_monitor_latest", json::object());
        CHECK(!result.first);
        const json payload = ParseResult(result.second);
        CHECK(payload.is_object());
        CHECK_EQ(payload.value("manager", std::string{}), "mock://monitor");
        CHECK_EQ(payload.value("endpoint", std::string{}),
                 "/api/query/latest");
        CHECK_EQ(std::to_string(payload.value("requested", 0)), "10");
        CHECK_EQ(std::to_string(payload.value("count", 0)), "2");
        CHECK(payload["snapshots"].is_array());
        if (payload["snapshots"].is_array() &&
            payload["snapshots"].size() == 2) {
            CHECK_EQ(payload["snapshots"][0].value("host", std::string{}),
                     "mock-host-a");
            CHECK_EQ(std::to_string(payload["snapshots"][0]["cpus"].size()),
                     "2");
            CHECK(payload["snapshots"][0]["mem"].value("used_percent", 0.0) >
                  24.9);
        }
        CHECK(client->CallCount(MockMonitorClient::Endpoint::kLatest) == 1);
        const auto& call = client->calls().back();
        CHECK(call.query.limit == 10);
        CHECK(call.query.timeout_ms == 3000);
        CHECK_EQ(call.query.base_url, "mock://monitor");
    }

    // limit 生效（Mock 模拟 Manager 的服务端截断）。
    {
        const auto result =
            CallTool(registry, "os_monitor_latest", {{"limit", 1}});
        CHECK(!result.first);
        const json payload = ParseResult(result.second);
        CHECK_EQ(std::to_string(payload.value("requested", 0)), "1");
        CHECK_EQ(std::to_string(payload.value("count", 0)), "1");
        CHECK(client->calls().back().query.limit == 1);
    }

    // 参数透传：limit/timeout/manager_url 全部进入 MonitorQuery。
    {
        const auto result =
            CallTool(registry, "os_monitor_latest",
                     {{"limit", 7},
                      {"timeout_ms", 1200},
                      {"manager_url", "http://127.0.0.1:9911"}});
        CHECK(!result.first);
        const json payload = ParseResult(result.second);
        CHECK_EQ(payload.value("manager", std::string{}),
                 "http://127.0.0.1:9911");
        CHECK_EQ(std::to_string(payload.value("requested", 0)), "7");
        const auto& call = client->calls().back();
        CHECK(call.query.limit == 7);
        CHECK(call.query.timeout_ms == 1200);
        CHECK_EQ(call.query.base_url, "http://127.0.0.1:9911");
    }

    // ------------------------------------------------------------------
    // 3. stats / kernel：本地按 limit 截断并标记 total/truncated。
    // ------------------------------------------------------------------
    {
        client->ClearCalls();
        const auto result =
            CallTool(registry, "os_monitor_stats", json::object());
        CHECK(!result.first);
        const json payload = ParseResult(result.second);
        CHECK_EQ(std::to_string(payload.value("count", 0)), "2");
        CHECK_EQ(std::to_string(payload.value("total", 0)), "2");
        CHECK(payload.value("truncated", true) == false);
        CHECK(payload["hosts"].is_array());
        if (payload["hosts"].is_array() && payload["hosts"].size() == 2) {
            CHECK_EQ(payload["hosts"][0].value("host", std::string{}),
                     "mock-host-a");
            const double score = payload["hosts"][0].value("score", -1.0);
            CHECK(score >= 0.0 && score <= 100.0);
            CHECK(payload["hosts"][0].value("cpu_percent", 0.0) > 24.9);
            CHECK(payload["hosts"][0].value("ebpf_active", true) == false);
        }
        CHECK(client->calls().back().query.limit == 10);

        const auto limited =
            CallTool(registry, "os_monitor_stats", {{"limit", 1}});
        CHECK(!limited.first);
        const json one = ParseResult(limited.second);
        CHECK_EQ(std::to_string(one.value("count", 0)), "1");
        CHECK_EQ(std::to_string(one.value("total", 0)), "2");
        CHECK(one.value("truncated", false) == true);
    }

    {
        const auto result =
            CallTool(registry, "os_monitor_kernel", json::object());
        CHECK(!result.first);
        const json payload = ParseResult(result.second);
        CHECK_EQ(std::to_string(payload.value("count", 0)), "2");
        CHECK_EQ(payload.value("endpoint", std::string{}),
                 "/api/query/kernel");
        const json& host = payload["hosts"][0];
        for (const char* key : {"syscall", "sched", "futex", "tcp", "fileio"}) {
            CHECK(HasKey(host, key));
            CHECK(host[key].is_object());
        }
        CHECK(HasKey(host["syscall"], "active"));
        CHECK(HasKey(host["tcp"], "rtt_p99_us"));
        CHECK(HasKey(host["fileio"], "read_iops"));
    }

    // ------------------------------------------------------------------
    // 4. health：Mock 固定计数，endpoint 正确。
    // ------------------------------------------------------------------
    {
        const auto result =
            CallTool(registry, "os_monitor_health", json::object());
        CHECK(!result.first);
        const json payload = ParseResult(result.second);
        CHECK(HasKey(payload, "health"));
        CHECK_EQ(payload["health"].value("status", std::string{}), "ok");
        CHECK_EQ(std::to_string(payload["health"].value("received", 0)), "2");
        CHECK_EQ(payload["endpoint"].get<std::string>(), "/api/health");
        CHECK(client->CallCount(MockMonitorClient::Endpoint::kHealth) == 1);
    }

    // ------------------------------------------------------------------
    // 5. 错误注入：错误文本原样返回，其它工具不受影响。
    // ------------------------------------------------------------------
    {
        client->SetFailure(MockMonitorClient::Endpoint::kLatest,
                           "模拟连接失败：Manager 未启动", MonitorErrorKind::kConnect);
        const auto failed =
            CallTool(registry, "os_monitor_latest", json::object());
        CHECK(failed.first);
        CHECK_EQ(failed.second, "模拟连接失败：Manager 未启动");

        const auto stats =
            CallTool(registry, "os_monitor_stats", json::object());
        CHECK(!stats.first);

        client->ClearFailure(MockMonitorClient::Endpoint::kLatest);
        const auto recovered =
            CallTool(registry, "os_monitor_latest", json::object());
        CHECK(!recovered.first);
    }

    // ------------------------------------------------------------------
    // 6. 响应格式异常：客户端数据结构不对时给出明确诊断。
    // ------------------------------------------------------------------
    {
        client->SetLatest(json{{"unexpected", true}});
        auto result = CallTool(registry, "os_monitor_latest", json::object());
        CHECK(result.first);
        CHECK(result.second.find("响应格式异常") != std::string::npos);

        client->SetStats(json::object());
        result = CallTool(registry, "os_monitor_stats", json::object());
        CHECK(result.first);
        CHECK(result.second.find("响应格式异常") != std::string::npos);

        client->SetKernel(json("not-an-array"));
        result = CallTool(registry, "os_monitor_kernel", json::object());
        CHECK(result.first);
        CHECK(result.second.find("响应格式异常") != std::string::npos);

        client->SetHealth(json::array());
        result = CallTool(registry, "os_monitor_health", json::object());
        CHECK(result.first);
        CHECK(result.second.find("响应格式异常") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // 7. 直接 InvokeTool（绕过 schema）时的边界 clamp。
    // ------------------------------------------------------------------
    {
        auto clamped_client = std::make_shared<MockMonitorClient>();
        Registry clamped;
        go2cpp::mcp::tools::RegisterMonitorTools(clamped, clamped_client);

        ToolContext context;
        context.arguments = {{"limit", 0}, {"timeout_ms", 1}};
        auto invoked = clamped.InvokeTool("os_monitor_latest", context);
        CHECK(invoked.has_value());
        CHECK(invoked && !invoked->is_error);
        CHECK(clamped_client->calls().back().query.limit == 1);
        CHECK(clamped_client->calls().back().query.timeout_ms == 100);

        context.arguments = {{"limit", 100000}, {"timeout_ms", 999999}};
        invoked = clamped.InvokeTool("os_monitor_latest", context);
        CHECK(invoked.has_value());
        CHECK(clamped_client->calls().back().query.limit == 256);
        CHECK(clamped_client->calls().back().query.timeout_ms == 60000);
    }

    // ------------------------------------------------------------------
    // 8. 未注入客户端：四个工具仍可见，调用返回装配提示。
    // ------------------------------------------------------------------
    {
        Registry missing;
        go2cpp::mcp::tools::RegisterMonitorTools(missing, nullptr);
        CHECK_EQ(std::to_string(missing.tool_count()), "4");
        const auto result =
            CallTool(missing, "os_monitor_latest", json::object());
        CHECK(result.first);
        CHECK(result.second.find("MonitorClient 未配置") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // 9. HttpMonitorClient 地址校验：在发请求前失败，因此完全离线。
    // ------------------------------------------------------------------
    {
        go2cpp::mcp::tools::HttpMonitorClient http(
            MonitorOptions{"http://127.0.0.1:1", 100});
        const auto scheme =
            http.Latest(MonitorQuery{10, 100, "ftp://127.0.0.1:9101"});
        CHECK(!scheme.ok);
        CHECK(scheme.error.kind == MonitorErrorKind::kInvalidEndpoint);
        CHECK(scheme.error.message.find("仅支持 http://") != std::string::npos);

        const auto port =
            http.Latest(MonitorQuery{10, 100, "http://127.0.0.1:99999"});
        CHECK(!port.ok);
        CHECK(port.error.kind == MonitorErrorKind::kInvalidEndpoint);
        CHECK(port.error.message.find("端口超出范围") != std::string::npos);
    }

    return Summary("monitor_client");
}
