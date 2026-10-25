// worker → manager 端到端测试：Aggregator 采集 → Pusher HTTP 推送 →
// Manager 内存存储 → HTTP 查询。
//
// 覆盖：
//   1. Manager 绑定 0 端口（内核分配）并真正接受 HTTP 连接；
//   2. Pusher 把 Snapshot JSON POST 过去，Manager 收到并回 {"ok":true}；
//   3. GET /api/query/latest、/api/query/stats、/api/query/kernel、
//      /api/health 的返回内容；
//   4. 非法 JSON 被拒绝（400，rejected 计数增加）；
//   5. Aggregator + Pusher 后台周期推送，Manager 端计数持续增长。

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/network/http.hpp"
#include "go2cpp/os/monitor/aggregator.hpp"
#include "go2cpp/os/monitor/manager.hpp"
#include "go2cpp/os/monitor/pusher.hpp"
#include "test_support.hpp"

namespace {

using go2cpp::Json;
using go2cpp::http::Client;
using go2cpp::http::Status;
using go2cpp::os::monitor::Aggregator;
using go2cpp::os::monitor::AggregatorConfig;
using go2cpp::os::monitor::Manager;
using go2cpp::os::monitor::ManagerConfig;
using go2cpp::os::monitor::Pusher;
using go2cpp::os::monitor::PusherConfig;
using go2cpp::os::monitor::Snapshot;
using go2cpp::os::monitor::SyscallLatency;

std::string BaseUrl(std::uint16_t port) {
    return "http://127.0.0.1:" + std::to_string(port);
}

void TestManagerPushAndQuery() {
    go2cpp_tests::announce("manager_push_and_query");
    ManagerConfig manager_config;
    manager_config.listen_address = "127.0.0.1";
    manager_config.port = 0;  // 由内核分配，避免与固定端口冲突
    manager_config.capacity = 8;
    Manager manager(manager_config);
    GO2CPP_CHECK(manager.Start());
    GO2CPP_CHECK(manager.running());
    const std::uint16_t port = manager.port();
    GO2CPP_CHECK(port != 0);
    std::printf("[e2e] manager listening on port %u\n",
                static_cast<unsigned int>(port));

    AggregatorConfig aggregator_config;
    aggregator_config.enable_ebpf = false;
    Aggregator aggregator(aggregator_config);

    PusherConfig pusher_config;
    pusher_config.manager_url = BaseUrl(port) + "/api/push";
    pusher_config.timeout = std::chrono::seconds(5);
    Pusher pusher(pusher_config, aggregator);

    // 推送两条不同主机的快照。
    Snapshot first = aggregator.CollectNow();
    first.host = "e2e-host-a";
    first.timestamp_ms = 1000;
    // 非 root 环境下三个 tracepoint 采集器会降级，这里注入内核算法的结果，
    // 用于验证 /api/query/kernel 的字段透传与聚合（与采集器本身解耦）。
    first.syscalls.active = true;
    first.syscalls.total = 1234;
    first.syscalls.hist[1] = 1000;
    first.syscalls.top.push_back(
        SyscallLatency{1, "write", 900, 2.0, 1.0, 4.0, 9.0});
    first.sched.active = true;
    first.sched.switches = 42;
    first.sched.wait_count = 10;
    first.sched.wait_p99_us = 20.0;
    first.futex.active = true;
    first.futex.wait_count = 7;
    first.futex.wake_count = 5;
    first.futex.wait_wake_ratio = 1.4;
    GO2CPP_CHECK(pusher.PushSnapshot(first));

    Snapshot second = aggregator.CollectNow();
    second.host = "e2e-host-b";
    second.timestamp_ms = 2000;
    GO2CPP_CHECK(pusher.PushSnapshot(second));

    GO2CPP_REQUIRE_EVENTUALLY(manager.received() >= 2,
                              std::chrono::seconds(10));
    GO2CPP_CHECK(manager.rejected() == 0);
    GO2CPP_CHECK(manager.stored() == 2);

    // 查询最近数据。
    Client client(std::chrono::seconds(5));
    auto latest = client.get(BaseUrl(port) + "/api/query/latest?n=10");
    GO2CPP_CHECK(latest.ok());
    if (latest.ok()) {
        GO2CPP_CHECK(latest.response->status == Status::OK);
        const Json body = Json::parse(latest.response->body);
        GO2CPP_CHECK(body.is_array());
        GO2CPP_CHECK(body.size() == 2);
        GO2CPP_CHECK(body[0]["host"].get<std::string>() == "e2e-host-a");
        GO2CPP_CHECK(body[1]["host"].get<std::string>() == "e2e-host-b");
        GO2CPP_CHECK(body[1]["mem"]["total_kb"].get<std::uint64_t>() > 0);
        GO2CPP_CHECK(!body[1]["cpus"].empty());
    }

    // 查询主机摘要：两台主机各一条，按评分降序。
    auto stats = client.get(BaseUrl(port) + "/api/query/stats");
    GO2CPP_CHECK(stats.ok());
    if (stats.ok()) {
        const Json body = Json::parse(stats.response->body);
        GO2CPP_CHECK(body.is_array());
        GO2CPP_CHECK(body.size() == 2);
        for (const auto& item : body) {
            const double score = item["score"].get<double>();
            GO2CPP_CHECK(score >= 0.0);
            GO2CPP_CHECK(score <= 100.0);
            GO2CPP_CHECK(item["cpu_percent"].get<double>() >= 0.0);
        }
        const double first_score = body[0]["score"].get<double>();
        const double second_score = body[1]["score"].get<double>();
        GO2CPP_CHECK(first_score >= second_score);
    }

    // 内核 tracepoint 摘要：每主机一条，带 syscall top / sched / futex 字段。
    auto kernel = client.get(BaseUrl(port) + "/api/query/kernel");
    GO2CPP_CHECK(kernel.ok());
    if (kernel.ok()) {
        GO2CPP_CHECK(kernel.response->status == Status::OK);
        const Json body = Json::parse(kernel.response->body);
        GO2CPP_CHECK(body.is_array());
        GO2CPP_CHECK(body.size() == 2);
        bool found = false;
        for (const auto& item : body) {
            if (item["host"].get<std::string>() != "e2e-host-a") {
                continue;
            }
            found = true;
            GO2CPP_CHECK(item["syscall"]["active"].get<bool>());
            GO2CPP_CHECK(item["syscall"]["total"].get<std::uint64_t>() == 1234);
            GO2CPP_CHECK(item["syscall"]["top"].size() == 1);
            GO2CPP_CHECK(item["syscall"]["top"][0]["name"].get<std::string>() ==
                         "write");
            GO2CPP_CHECK(item["sched"]["switches"].get<std::uint64_t>() == 42);
            GO2CPP_CHECK(item["sched"]["wait_count"].get<std::uint64_t>() == 10);
            GO2CPP_CHECK(item["futex"]["wait_count"].get<std::uint64_t>() == 7);
            GO2CPP_CHECK(item["futex"]["wake_count"].get<std::uint64_t>() == 5);
            GO2CPP_CHECK(item["futex"]["wait_wake_ratio"].get<double>() > 1.3);
        }
        GO2CPP_CHECK(found);
    }

    // 健康探针。
    auto health = client.get(BaseUrl(port) + "/api/health");
    GO2CPP_CHECK(health.ok());
    if (health.ok()) {
        const Json body = Json::parse(health.response->body);
        GO2CPP_CHECK(body["status"].get<std::string>() == "ok");
        GO2CPP_CHECK(body["received"].get<std::uint64_t>() >= 2);
    }

    // 非法 JSON：400，并计入 rejected。
    {
        const std::string bad = "{this is not json";
        auto rejected =
            client.post(BaseUrl(port) + "/api/push", bad, "application/json");
        GO2CPP_CHECK(rejected.response != nullptr);
        if (rejected.response != nullptr) {
            GO2CPP_CHECK(rejected.response->status == Status::BadRequest);
        }
        GO2CPP_CHECK(manager.rejected() >= 1);
    }

    manager.Stop();
    GO2CPP_CHECK(!manager.running());
    manager.Stop();  // 幂等
}

void TestPeriodicPush() {
    go2cpp_tests::announce("aggregator_pusher_periodic");
    ManagerConfig manager_config;
    manager_config.port = 0;
    Manager manager(manager_config);
    GO2CPP_CHECK(manager.Start());
    const std::uint16_t port = manager.port();

    AggregatorConfig aggregator_config;
    aggregator_config.interval = std::chrono::milliseconds(80);
    aggregator_config.enable_ebpf = false;
    aggregator_config.workers = 4;
    Aggregator aggregator(aggregator_config);

    PusherConfig pusher_config;
    pusher_config.manager_url = BaseUrl(port) + "/api/push";
    pusher_config.interval = std::chrono::milliseconds(80);
    Pusher pusher(pusher_config, aggregator);

    GO2CPP_CHECK(aggregator.Start());
    GO2CPP_CHECK(pusher.Start());
    GO2CPP_REQUIRE_EVENTUALLY(manager.received() >= 3,
                              std::chrono::seconds(15));
    // Manager 先收到请求、Pusher 再拿到响应并 ++pushed；并发负载下两者有
    // 一个请求的时间差，因此同样用 eventually 而不是立即断言。
    GO2CPP_REQUIRE_EVENTUALLY(pusher.Stats().pushed >= 3,
                              std::chrono::seconds(10));
    const auto stats = pusher.Stats();
    GO2CPP_CHECK(stats.failed == 0);

    pusher.Stop();
    aggregator.Stop();
    manager.Stop();
    GO2CPP_CHECK(!pusher.running());
    GO2CPP_CHECK(!aggregator.running());
}

void TestCapacityEviction() {
    go2cpp_tests::announce("manager_capacity_eviction");
    ManagerConfig config;
    config.port = 0;
    config.capacity = 3;
    Manager manager(config);
    GO2CPP_CHECK(manager.Start());

    AggregatorConfig aggregator_config;
    aggregator_config.enable_ebpf = false;
    Aggregator aggregator(aggregator_config);
    for (int i = 0; i < 6; ++i) {
        Snapshot snapshot = aggregator.CollectNow();
        snapshot.host = "capacity-host";
        snapshot.timestamp_ms = 1000 + i;
        GO2CPP_CHECK(manager.Ingest(snapshot));
    }
    GO2CPP_CHECK(manager.stored() == 3);
    const auto latest = manager.Latest(10);
    GO2CPP_CHECK(latest.size() == 3);
    // 保留的是最后三条。
    GO2CPP_CHECK(latest.front().timestamp_ms == 1003);
    GO2CPP_CHECK(latest.back().timestamp_ms == 1005);
    manager.Stop();
}

}  // namespace

int main() {
    TestManagerPushAndQuery();
    TestPeriodicPush();
    TestCapacityEviction();

    const int failures = go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::fprintf(stderr, "[test] failures=%d\n", failures);
        return 1;
    }
    std::printf("[test] all monitor e2e checks passed\n");
    return 0;
}
