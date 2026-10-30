/**
 * @file test_monitor_tools.cpp
 * @brief `os_monitor_*` 工具单测：注册、schema 校验、HTTP 失败、真实 Manager 查询。
 *
 * 覆盖：
 * 1. 四个工具出现在 `tools/list`，且 schema 暴露 manager_url / limit / timeout_ms；
 * 2. 校验层拒绝越界与未知参数（limit/timeout 边界、额外字段）；
 * 3. Manager 未启动 → `isError=true` 且带部署排查提示，进程不崩溃；
 * 4. 真实 Manager（端口由内核分配）+ 真实 `POST /api/push` → 四个工具返回预期 JSON；
 * 5. 错误路径：不支持的 scheme、404、Manager 停止后的连接失败。
 */

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "test_util.hpp"

#include "go2cpp/network/http.hpp"
#include "go2cpp/os/monitor/manager.hpp"
#include "go2cpp/os/monitor/monitor.hpp"
#include "mcp/tools/monitor_tools.hpp"
#include "mcp/validation/validators.hpp"

namespace {

using go2cpp::mcp::json;
using go2cpp::mcp::Registry;
using go2cpp::mcp::ToolContext;
using go2cpp::mcp::tools::MonitorOptions;

/// 调用工具并返回 (is_error, text)。
std::pair<bool, std::string> CallTool(const Registry& registry, const std::string& name,
                                      const json& arguments) {
    ToolContext context;
    context.arguments = arguments;
    const auto result = registry.InvokeTool(name, context);
    if (!result) {
        return {true, "__missing__"};
    }
    return {result->is_error, result->content.empty() ? "" : result->content[0].text};
}

/// 解析工具返回的 JSON 文本；失败返回 null。
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

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 构造一条可序列化的快照（不读 /proc，避免测试依赖宿主机实时状态）。
go2cpp::os::monitor::Snapshot MakeSnapshot(const std::string& host, double core_usage) {
    using namespace go2cpp::os::monitor;
    Snapshot snapshot;
    snapshot.host = host;
    snapshot.timestamp_ms = NowMs();
    CpuCore first;
    first.name = "cpu0";
    first.usage_percent = core_usage;
    snapshot.cpus.push_back(first);
    CpuCore second;
    second.name = "cpu1";
    second.usage_percent = core_usage;
    snapshot.cpus.push_back(second);
    snapshot.mem.total_kb = 1024 * 1024;
    snapshot.mem.available_kb = 768 * 1024;
    snapshot.mem.used_percent = 25.0;
    snapshot.load.load1 = 1.5;
    snapshot.load.load5 = 1.2;
    snapshot.load.load15 = 0.9;
    DiskStat disk;
    disk.name = "sda";
    disk.util_percent = 12.5;
    snapshot.disks.push_back(disk);
    NetDevStat net;
    net.name = "eth0";
    net.rcv_rate = 1024.0;
    net.snd_rate = 2048.0;
    snapshot.nets.push_back(net);
    snapshot.ebpf_active = false;
    snapshot.ebpf_reason = "test: no eBPF";
    return snapshot;
}

/// schema 与校验层：合法参数通过，越界/未知参数被拒。
void CheckArgumentValidation(const Registry& registry) {
    const auto latest = registry.ToolSchema("os_monitor_latest");
    CHECK(latest.has_value());
    if (!latest) {
        return;
    }
    CHECK(go2cpp::mcp::validation::ValidateToolArguments(*latest, json::object()).ok);
    CHECK(go2cpp::mcp::validation::ValidateToolArguments(
              *latest, {{"limit", 10}, {"timeout_ms", 3000},
                        {"manager_url", "http://127.0.0.1:9101"}})
              .ok);
    CHECK(!go2cpp::mcp::validation::ValidateToolArguments(*latest, {{"limit", 0}}).ok);
    CHECK(!go2cpp::mcp::validation::ValidateToolArguments(*latest, {{"limit", 257}}).ok);
    CHECK(!go2cpp::mcp::validation::ValidateToolArguments(*latest, {{"timeout_ms", 50}}).ok);
    CHECK(!go2cpp::mcp::validation::ValidateToolArguments(*latest, {{"timeout_ms", 60001}}).ok);
    CHECK(!go2cpp::mcp::validation::ValidateToolArguments(
               *latest, {{"manager_url", "ftp://127.0.0.1:9101"}})
               .ok);
    CHECK(!go2cpp::mcp::validation::ValidateToolArguments(*latest, {{"unexpected", 1}}).ok);
    CHECK(!go2cpp::mcp::validation::ValidateToolArguments(*latest, {{"limit", "10"}}).ok);

    // health 不接受 limit（schema 里没有该字段）。
    const auto health = registry.ToolSchema("os_monitor_health");
    CHECK(health.has_value());
    if (health) {
        CHECK(go2cpp::mcp::validation::ValidateToolArguments(*health, json::object()).ok);
        CHECK(!go2cpp::mcp::validation::ValidateToolArguments(*health, {{"limit", 1}}).ok);
    }
}

/// 「Manager 未启动」的错误必须可诊断：提到 Manager、地址与排查方向。
void CheckUnreachable(const Registry& registry, const std::string& base_url) {
    const auto result = CallTool(registry, "os_monitor_latest",
                                 {{"manager_url", base_url}, {"timeout_ms", 1000}});
    CHECK(result.first);
    CHECK(result.second.find("无法访问 Manager") != std::string::npos);
    CHECK(result.second.find("Manager 已启动") != std::string::npos);
}

}  // namespace

int main() {
    using namespace go2cpp::mcp;
    using go2cpp::os::monitor::Manager;
    using go2cpp::os::monitor::ManagerConfig;
    using go2cpp::os::monitor::SnapshotToJson;

    // ------------------------------------------------------------------
    // 1. 注册：四个工具都能在 tools/list 中看到，且 schema 结构正确。
    // ------------------------------------------------------------------
    Registry registry;
    tools::RegisterMonitorTools(
        registry,
        tools::MakeHttpMonitorClient(MonitorOptions{"http://127.0.0.1:1", 500}));
    for (const char* name : {"os_monitor_latest", "os_monitor_stats", "os_monitor_kernel",
                             "os_monitor_health"}) {
        CHECK(HasTool(registry, name));
    }
    CHECK_EQ(std::to_string(registry.tool_count()), "4");
    CheckArgumentValidation(registry);

    // ------------------------------------------------------------------
    // 2. Manager 未启动：用一个刚释放的监听端口，连接必然得到 RST。
    // ------------------------------------------------------------------
    ManagerConfig config;
    config.listen_address = "127.0.0.1";
    config.port = 0;
    std::uint16_t dead_port = 0;
    {
        Manager probe(config);
        CHECK(probe.Start());
        dead_port = probe.port();
        CHECK(dead_port != 0);
        probe.Stop();
        CHECK(!probe.running());
    }
    CHECK(dead_port != 0);
    CheckUnreachable(registry, "http://127.0.0.1:" + std::to_string(dead_port));

    // 非法 scheme / 非法端口：处理器自身也要防御（不只依赖 schema）。
    {
        const auto scheme = CallTool(registry, "os_monitor_health",
                                     {{"manager_url", "ftp://127.0.0.1:9101"}});
        CHECK(scheme.first);
        CHECK(scheme.second.find("仅支持 http://") != std::string::npos);

        const auto port = CallTool(registry, "os_monitor_health",
                                   {{"manager_url", "http://127.0.0.1:99999"}});
        CHECK(port.first);
        CHECK(port.second.find("端口超出范围") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // 3. 真实 Manager：绑定内核分配端口，用真实 POST /api/push 灌两条快照。
    // ------------------------------------------------------------------
    Manager manager(config);
    CHECK(manager.Start());
    const std::uint16_t port = manager.port();
    CHECK(port != 0);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(port);

    const auto healthy = MakeSnapshot("mcp-monitor-test", 20.0);
    const auto busy = MakeSnapshot("mcp-monitor-test-2", 60.0);
    auto pushed = go2cpp::http::Post(base_url + "/api/push", SnapshotToJson(healthy),
                                     "application/json");
    CHECK(pushed.ok());
    pushed = go2cpp::http::Post(base_url + "/api/push", SnapshotToJson(busy),
                                "application/json");
    CHECK(pushed.ok());
    CHECK_EQ(std::to_string(manager.received()), "2");

    Registry live;
    tools::RegisterMonitorTools(
        live, tools::MakeHttpMonitorClient(MonitorOptions{base_url, 2000}));

    // 3.1 latest：默认按快照时间正序返回，字段透传。
    {
        const auto result = CallTool(live, "os_monitor_latest", {{"limit", 10}});
        CHECK(!result.first);
        const json payload = ParseResult(result.second);
        CHECK(payload.is_object());
        CHECK_EQ(std::to_string(payload.value("count", 0)), "2");
        CHECK(payload["snapshots"].is_array());
        if (payload["snapshots"].is_array() && payload["snapshots"].size() == 2) {
            CHECK_EQ(payload["snapshots"][0].value("host", std::string{}), "mcp-monitor-test");
            CHECK_EQ(std::to_string(payload["snapshots"][0]["cpus"].size()), "2");
            CHECK(payload["snapshots"][0]["mem"].value("used_percent", 0.0) > 24.9);
            CHECK_EQ(std::to_string(
                         payload["snapshots"][1].value("timestamp_ms", std::int64_t{0})),
                     std::to_string(busy.timestamp_ms));
        }
    }

    // 3.2 limit 生效：请求 1 条时本地截断并标记 truncated。
    {
        const auto result = CallTool(live, "os_monitor_latest", {{"limit", 1}});
        CHECK(!result.first);
        const json payload = ParseResult(result.second);
        CHECK_EQ(std::to_string(payload.value("count", 0)), "1");
        CHECK_EQ(std::to_string(payload["snapshots"].size()), "1");
    }

    // 3.3 stats：每主机一条 + 健康评分，按分数降序（20% CPU 的主机在前）。
    {
        const auto result = CallTool(live, "os_monitor_stats", json::object());
        CHECK(!result.first);
        const json payload = ParseResult(result.second);
        CHECK_EQ(std::to_string(payload.value("count", 0)), "2");
        CHECK(payload["hosts"].is_array());
        if (payload["hosts"].is_array() && payload["hosts"].size() == 2) {
            const json& first = payload["hosts"][0];
            CHECK_EQ(first.value("host", std::string{}), "mcp-monitor-test");
            const double score = first.value("score", -1.0);
            CHECK(score >= 0.0 && score <= 100.0);
            CHECK(first.value("cpu_percent", 0.0) > 19.9);
            CHECK(first.value("mem_used_percent", 0.0) > 24.9);
            CHECK(first.value("ebpf_active", true) == false);
        }
    }

    // 3.4 kernel：每个主机都带 syscall/sched/futex/tcp/fileio 五个对象。
    {
        const auto result = CallTool(live, "os_monitor_kernel", json::object());
        CHECK(!result.first);
        const json payload = ParseResult(result.second);
        CHECK_EQ(std::to_string(payload.value("count", 0)), "2");
        const json& host = payload["hosts"][0];
        for (const char* key : {"syscall", "sched", "futex", "tcp", "fileio"}) {
            CHECK(HasKey(host, key));
            CHECK(host[key].is_object());
        }
        CHECK(HasKey(host["syscall"], "active"));
        CHECK(HasKey(host["tcp"], "rtt_p99_us"));
        CHECK(HasKey(host["fileio"], "read_iops"));
    }

    // 3.5 health：Manager 自身计数（received 含两条推送，rejected 为 0）。
    {
        const auto result = CallTool(live, "os_monitor_health", json::object());
        CHECK(!result.first);
        const json payload = ParseResult(result.second);
        CHECK(HasKey(payload, "health"));
        CHECK_EQ(payload["health"].value("status", std::string{}), "ok");
        CHECK_EQ(std::to_string(payload["health"].value("received", 0)), "2");
        CHECK_EQ(std::to_string(payload["health"].value("rejected", 0)), "0");
        CHECK_EQ(std::to_string(payload["health"].value("stored", 0)), "2");
    }

    // 3.6 路径前缀错误：Manager 对未知路径返回无 body 的 404（无可解析头），
    // 工具必须给出「无法访问 Manager + 响应无法解析」的明确诊断，而不是崩溃。
    {
        const auto result =
            CallTool(live, "os_monitor_latest",
                     {{"manager_url", base_url + "/does-not-exist"}, {"timeout_ms", 1000}});
        CHECK(result.first);
        // 当前 HTTP 客户端能解析无 body 的 404，因此给出的是更明确的
        // 「HTTP 404 + 路径不存在」诊断（旧断言基于解析失败时的兜底文本）。
        CHECK(result.second.find("HTTP 404") != std::string::npos);
        CHECK(result.second.find("路径不存在") != std::string::npos);
    }

    // 3.7 参数覆盖地址：manager_url 优先于注册时的 base_url。
    {
        const auto result = CallTool(live, "os_monitor_health",
                                     {{"manager_url", "http://127.0.0.1:" +
                                                          std::to_string(dead_port)}});
        CHECK(result.first);
        CHECK(result.second.find("无法访问 Manager") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // 4. Manager 停止：同一地址的连接错误必须可诊断，工具表仍然可用。
    // ------------------------------------------------------------------
    manager.Stop();
    CHECK(!manager.running());
    CheckUnreachable(live, base_url);
    CHECK(HasTool(live, "os_monitor_kernel"));

    return go2cpp::mcp::test::Summary("monitor_tools");
}
