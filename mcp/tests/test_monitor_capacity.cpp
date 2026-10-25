/**
 * @file test_monitor_capacity.cpp
 * @brief Manager 容量淘汰与 push 拒绝的边界/压测。
 *
 * 覆盖：
 * 1. 容量 8：推 20 条后只保留最新 8 条，`received` 仍计 20，`stored=8`；
 * 2. 淘汰后 `Latest` / 工具 `os_monitor_latest` 只能看到未淘汰的尾部数据；
 * 3. 默认容量 256：推 300 条 → 旧 44 条被淘汰（首条为第 45 条）；
 * 4. 容量 1：只保留最后一条；容量 0 表示不限制（文档化语义）；
 * 5. 非法 JSON：400 + `rejected` 计数 + 不进入存储，工具 health 透传；
 * 6. 4 线程 × 100 次真实 HTTP push：并发下 `stored <= capacity`、计数不丢。
 *
 * 这些断言保证「容量淘汰」不是静默丢数据：接收计数、存储条数与淘汰顺序
 * 三者必须同时自洽。
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_util.hpp"

#include "go2cpp/network/http.hpp"
#include "go2cpp/os/monitor/manager.hpp"
#include "go2cpp/os/monitor/monitor.hpp"
#include "mcp/server/registry.hpp"
#include "mcp/tools/monitor_client.hpp"
#include "mcp/tools/monitor_tools.hpp"

namespace {

using go2cpp::mcp::json;
using go2cpp::mcp::Registry;
using go2cpp::mcp::ToolContext;
using go2cpp::mcp::tools::MonitorOptions;

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 最小可序列化快照；host 与 timestamp 由调用方控制，便于断言淘汰顺序。
go2cpp::os::monitor::Snapshot MakeSnapshot(const std::string& host,
                                           std::int64_t timestamp_ms) {
    using namespace go2cpp::os::monitor;
    Snapshot snapshot;
    snapshot.host = host;
    snapshot.timestamp_ms = timestamp_ms;
    snapshot.host_info.hostname = host;
    snapshot.host_info.kernel = "test";
    snapshot.host_info.arch = "x86_64";
    CpuCore core;
    core.name = "cpu0";
    core.usage_percent = 20.0;
    snapshot.cpus.push_back(core);
    snapshot.mem.total_kb = 1024 * 1024;
    snapshot.mem.available_kb = 768 * 1024;
    snapshot.mem.used_percent = 25.0;
    snapshot.load.load1 = 1.0;
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

/// 通过真实 HTTP 推送一条快照，返回 HTTP 状态码；网络失败返回 0。
int Push(go2cpp::os::monitor::Manager& manager,
         const go2cpp::os::monitor::Snapshot& snapshot) {
    const std::string url = "http://127.0.0.1:" +
                            std::to_string(manager.port()) + "/api/push";
    const auto result = go2cpp::http::Post(
        url, go2cpp::os::monitor::SnapshotToJson(snapshot), "application/json");
    if (!result.ok() || !result.response) {
        return 0;
    }
    return static_cast<int>(result.response->status);
}

void CheckLatestOrder(const go2cpp::os::monitor::Manager& manager,
                      std::size_t expected_count, const std::string& first_host,
                      const std::string& last_host, const std::string& label) {
    const auto recent = manager.Latest(1000);
    CHECK_MSG(recent.size() == expected_count,
              label + "：Latest 条数应为 " + std::to_string(expected_count) +
                  "，实际 " + std::to_string(recent.size()));
    if (recent.size() != expected_count || recent.empty()) {
        return;
    }
    CHECK_MSG(recent.front().host == first_host,
              label + "：首条应为 " + first_host + "，实际 " +
                  recent.front().host);
    CHECK_MSG(recent.back().host == last_host,
              label + "：末条应为 " + last_host + "，实际 " +
                  recent.back().host);
}

/// 1) 容量 8 + 20 条推送：淘汰最旧 12 条，工具层只能看到 8 条。
void TestCapacityEviction() {
    go2cpp::os::monitor::ManagerConfig config;
    config.listen_address = "127.0.0.1";
    config.port = 0;
    config.capacity = 8;
    go2cpp::os::monitor::Manager manager(config);
    CHECK_MSG(manager.Start(), "容量用例 Manager 启动失败");
    const std::string base_url =
        "http://127.0.0.1:" + std::to_string(manager.port());

    for (int index = 0; index < 20; ++index) {
        const std::string host = "cap-" + std::to_string(index);
        const int status = Push(manager, MakeSnapshot(host, 1000 + index));
        CHECK_MSG(status == 200, host + " 推送状态码应为 200，实际 " +
                                     std::to_string(status));
    }
    CHECK_MSG(manager.received() == 20,
              "received 应为 20，实际 " + std::to_string(manager.received()));
    CHECK_MSG(manager.rejected() == 0, "rejected 应为 0");
    CHECK_MSG(manager.stored() == 8,
              "容量 8 时 stored 应为 8，实际 " + std::to_string(manager.stored()));
    CheckLatestOrder(manager, 8, "cap-12", "cap-19", "容量 8");

    Registry registry;
    go2cpp::mcp::tools::RegisterMonitorTools(
        registry, MonitorOptions{base_url, 1000});

    // 工具要求 256 条，但 Manager 只有 8 条：requested 保留用户意图，
    // count 如实反映容量上限，不能把淘汰数据重新变出来。
    {
        const auto result =
            CallTool(registry, "os_monitor_latest", {{"limit", 256}});
        CHECK_MSG(!result.first, "容量用例 latest 工具不应报错");
        const json payload = ParseResult(result.second);
        CHECK_MSG(payload.is_object(), "容量用例 latest 应返回 JSON 对象");
        CHECK_MSG(payload.value("count", 0) == 8,
                  "latest count 应为 8，实际 " +
                      std::to_string(payload.value("count", -1)));
        CHECK_MSG(payload.value("requested", 0) == 256,
                  "latest requested 应保留 256");
        if (payload["snapshots"].is_array() &&
            payload["snapshots"].size() == 8) {
            CHECK_MSG(payload["snapshots"][0].value("host", std::string{}) ==
                          "cap-12",
                      "淘汰后首条应为 cap-12");
            CHECK_MSG(payload["snapshots"][7].value("host", std::string{}) ==
                          "cap-19",
                      "淘汰后末条应为 cap-19");
        }
    }

    // health 的三个计数必须自洽：received=20, stored=8, rejected=0。
    {
        const auto result = CallTool(registry, "os_monitor_health", json::object());
        CHECK_MSG(!result.first, "容量用例 health 工具不应报错");
        const json payload = ParseResult(result.second);
        const json& health = payload["health"];
        CHECK_MSG(health.value("received", -1) == 20,
                  "health.received 应为 20，实际 " +
                      std::to_string(health.value("received", -1)));
        CHECK_MSG(health.value("stored", -1) == 8,
                  "health.stored 应为 8，实际 " +
                      std::to_string(health.value("stored", -1)));
        CHECK_MSG(health.value("rejected", -1) == 0,
                  "health.rejected 应为 0");
    }

    manager.Stop();
    CHECK_MSG(!manager.running(), "容量用例 Manager 应已停止");
}

/// 2) 默认容量 256：300 条中前 44 条被淘汰。
void TestDefaultCapacity() {
    go2cpp::os::monitor::ManagerConfig config;
    config.listen_address = "127.0.0.1";
    config.port = 0;
    CHECK_MSG(config.capacity == 256, "默认容量应为 256");
    go2cpp::os::monitor::Manager manager(config);
    CHECK_MSG(manager.Start(), "默认容量 Manager 启动失败");
    bool all_ingested = true;
    for (int index = 0; index < 300; ++index) {
        if (!manager.Ingest(MakeSnapshot("bulk-" + std::to_string(index),
                                         1000 + index))) {
            all_ingested = false;
        }
    }
    CHECK_MSG(all_ingested, "300 条 Ingest 应全部被接受");
    CHECK_MSG(manager.received() == 300,
              "默认容量 received 应为 300，实际 " +
                  std::to_string(manager.received()));
    CHECK_MSG(manager.stored() == 256,
              "默认容量 stored 应为 256，实际 " +
                  std::to_string(manager.stored()));
    CheckLatestOrder(manager, 256, "bulk-44", "bulk-299", "默认容量");
    manager.Stop();
}

/// 3) 极端容量：1 条与 0（不限制）。
void TestTinyAndUnboundedCapacity() {
    {
        go2cpp::os::monitor::ManagerConfig config;
        config.listen_address = "127.0.0.1";
        config.port = 0;
        config.capacity = 1;
        go2cpp::os::monitor::Manager manager(config);
        CHECK_MSG(manager.Start(), "容量 1 Manager 启动失败");
        for (int index = 0; index < 3; ++index) {
            CHECK(manager.Ingest(
                MakeSnapshot("one-" + std::to_string(index), 1000 + index)));
        }
        CHECK_MSG(manager.stored() == 1,
                  "容量 1 时 stored 应为 1，实际 " +
                      std::to_string(manager.stored()));
        CheckLatestOrder(manager, 1, "one-2", "one-2", "容量 1");
        manager.Stop();
    }
    {
        // MemorySnapshotStore 的契约：capacity == 0 表示不淘汰（不是「全丢」）。
        go2cpp::os::monitor::ManagerConfig config;
        config.listen_address = "127.0.0.1";
        config.port = 0;
        config.capacity = 0;
        go2cpp::os::monitor::Manager manager(config);
        CHECK_MSG(manager.Start(), "容量 0 Manager 启动失败");
        for (int index = 0; index < 5; ++index) {
            CHECK(manager.Ingest(
                MakeSnapshot("free-" + std::to_string(index), 1000 + index)));
        }
        CHECK_MSG(manager.stored() == 5,
                  "容量 0 表示不限制，stored 应为 5，实际 " +
                      std::to_string(manager.stored()));
        manager.Stop();
    }
}

/// 4) 非法 push：400 + rejected 计数，不污染存储。
void TestRejectedPush() {
    go2cpp::os::monitor::ManagerConfig config;
    config.listen_address = "127.0.0.1";
    config.port = 0;
    config.capacity = 4;
    go2cpp::os::monitor::Manager manager(config);
    CHECK_MSG(manager.Start(), "拒绝用例 Manager 启动失败");
    const std::string base_url =
        "http://127.0.0.1:" + std::to_string(manager.port());

    const auto bad = go2cpp::http::Post(base_url + "/api/push", "not-json",
                                        "application/json");
    CHECK_MSG(bad.ok() && bad.response != nullptr, "非法 push 应收到 HTTP 响应");
    if (bad.ok() && bad.response) {
        CHECK_MSG(static_cast<int>(bad.response->status) == 400,
                  "非法 push 状态码应为 400，实际 " +
                      std::to_string(static_cast<int>(bad.response->status)));
        CHECK_MSG(bad.response->body.find("\"ok\":false") != std::string::npos,
                  "非法 push 响应体应说明失败: " + bad.response->body);
    }
    CHECK_MSG(manager.rejected() == 1,
              "rejected 应为 1，实际 " + std::to_string(manager.rejected()));
    CHECK_MSG(manager.stored() == 0,
              "非法 push 不应进入存储，stored=" +
                  std::to_string(manager.stored()));
    CHECK_MSG(manager.received() == 0, "非法 push 不应计入 received");

    // 拒绝之后正常数据仍可写入，计数互不影响。
    CHECK(Push(manager, MakeSnapshot("after-reject", NowMs())) == 200);
    CHECK_MSG(manager.stored() == 1, "合法 push 应写入 1 条");
    CHECK_MSG(manager.rejected() == 1, "合法 push 不应改变 rejected");

    Registry registry;
    go2cpp::mcp::tools::RegisterMonitorTools(
        registry, MonitorOptions{base_url, 1000});
    const auto health = CallTool(registry, "os_monitor_health", json::object());
    CHECK_MSG(!health.first, "拒绝用例 health 工具不应报错");
    const json payload = ParseResult(health.second);
    CHECK_MSG(payload["health"].value("rejected", -1) == 1,
              "health.rejected 应为 1");
    CHECK_MSG(payload["health"].value("received", -1) == 1,
              "health.received 应为 1（不含被拒绝的请求）");
    manager.Stop();
}

/// 5) 并发 push 压测：容量上限始终成立，接收计数不丢。
void TestConcurrentPush() {
    go2cpp::os::monitor::ManagerConfig config;
    config.listen_address = "127.0.0.1";
    config.port = 0;
    config.capacity = 16;
    go2cpp::os::monitor::Manager manager(config);
    CHECK_MSG(manager.Start(), "并发 push Manager 启动失败");
    const std::string base_url =
        "http://127.0.0.1:" + std::to_string(manager.port());

    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;
    std::atomic<int> failures{0};
    std::atomic<int> http_errors{0};
    std::vector<std::thread> workers;
    const auto started = std::chrono::steady_clock::now();
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&, thread] {
            for (int index = 0; index < kPerThread; ++index) {
                const std::string host =
                    "conc-" + std::to_string(thread) + "-" +
                    std::to_string(index);
                const auto result = go2cpp::http::Post(
                    base_url + "/api/push",
                    go2cpp::os::monitor::SnapshotToJson(
                        MakeSnapshot(host, 1000 + thread * 1000 + index)),
                    "application/json");
                if (!result.ok() || !result.response) {
                    http_errors.fetch_add(1);
                } else if (static_cast<int>(result.response->status) != 200) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    CHECK_MSG(http_errors.load() == 0,
              "并发 push 网络错误数应为 0，实际 " +
                  std::to_string(http_errors.load()));
    CHECK_MSG(failures.load() == 0,
              "并发 push 非 200 数应为 0，实际 " +
                  std::to_string(failures.load()));
    CHECK_MSG(manager.received() ==
                  static_cast<std::uint64_t>(kThreads * kPerThread),
              "并发 push received 应为 400，实际 " +
                  std::to_string(manager.received()));
    CHECK_MSG(manager.stored() == 16,
              "并发 push 后 stored 应等于容量 16，实际 " +
                  std::to_string(manager.stored()));
    CHECK_MSG(manager.Latest(1000).size() == 16,
              "并发 push 后 Latest 应返回 16 条");
    std::printf(
        "[capacity] 并发 push %d 条（%d 线程，容量 16）耗时 %lldms "
        "≈ %.0f req/s\n",
        kThreads * kPerThread, kThreads, static_cast<long long>(elapsed),
        elapsed > 0 ? (kThreads * kPerThread * 1000.0 / elapsed) : 0.0);
    manager.Stop();
}

}  // namespace

int main() {
    TestCapacityEviction();
    TestDefaultCapacity();
    TestTinyAndUnboundedCapacity();
    TestRejectedPush();
    TestConcurrentPush();
    return go2cpp::mcp::test::Summary("monitor_capacity");
}
