/**
 * @file test_transport.cpp
 * @brief TCP 协调者：单进程内的 server/client 往返、断线重连、协议防御与并发。
 *
 * 多进程的真实崩溃/重选用 `test_multiprocess.cpp` 覆盖；本文件只验证传输
 * 层本身的正确性，避免把 fork/exec 的不稳定因素混进来。
 */

#include "go2cpp/distributed/transport.hpp"

#include "go2cpp/distributed/election.hpp"
#include "go2cpp/distributed/lock.hpp"
#include "go2cpp/distributed/registry.hpp"
#include "go2cpp/network/socket.hpp"

#include "distributed_test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::distributed;
using namespace go2cpp::distributed::test;
using namespace std::chrono_literals;

void TestServerClientFacades() {
    go2cpp_tests::announce("transport.facades");
    Coordinator coordinator(MakeOptions());
    CoordinatorServer server(coordinator);
    GO2CPP_REQUIRE(server.Start().ok());
    GO2CPP_CHECK(server.port() != 0);
    GO2CPP_CHECK(server.running());

    CoordinatorClient client("127.0.0.1", server.port());
    GO2CPP_CHECK(client.Connect().ok());
    GO2CPP_CHECK(client.connected());

    ServiceRegistry registry(client);
    std::vector<Registration> registrations;
    for (int index = 0; index < 5; ++index) {
        auto result = registry.Register("remote-api",
                                        "inst-" + std::to_string(index),
                                        "10.12.0." + std::to_string(index + 1) + ":9000",
                                        2s);
        GO2CPP_REQUIRE(result.ok());
        registrations.push_back(std::move(result.value));
    }
    GO2CPP_CHECK(registry.Lookup("remote-api").size() == 5);
    GO2CPP_CHECK(registry.Services() == std::vector<std::string>{"remote-api"});

    DistributedLock locks(client);
    auto first = locks.Acquire("remote-lock", "worker-a", 500ms);
    GO2CPP_REQUIRE(first.ok());
    GO2CPP_CHECK(first.value.fencing_token() == 1);
    const auto conflict = locks.Acquire("remote-lock", "worker-b", 500ms);
    GO2CPP_CHECK(conflict.status == Status::kConflict);
    first.value.Release();
    auto second = locks.Acquire("remote-lock", "worker-b", 500ms);
    GO2CPP_REQUIRE(second.ok());
    GO2CPP_CHECK(second.value.fencing_token() > 1);
    second.value.Release();

    LeaderElection election(client);
    auto node1 = election.Join("remote-election", "node-1", 500ms);
    auto node2 = election.Join("remote-election", "node-2", 500ms);
    GO2CPP_REQUIRE(node1.ok() && node2.ok());
    GO2CPP_CHECK(election.Leader("remote-election")->node == "node-1");
    node1.value.Leave();
    GO2CPP_CHECK(election.Leader("remote-election")->node == "node-2");
    node2.value.Leave();

    const CoordinatorStats stats = client.Stats();
    GO2CPP_CHECK(stats.instances == 5);
    GO2CPP_CHECK(stats.active_leases >= 5);
    (void)client.SweepExpired();
    GO2CPP_CHECK(server.requests_served() > 0);

    registrations.clear();
    GO2CPP_CHECK(registry.Lookup("remote-api").empty());
    client.Close();
    GO2CPP_CHECK(!client.connected());
    server.Stop();
    GO2CPP_CHECK(!server.running());
}

void TestReconnect() {
    go2cpp_tests::announce("transport.reconnect");
    Coordinator coordinator(MakeOptions());
    CoordinatorServer server(coordinator);
    GO2CPP_REQUIRE(server.Start().ok());
    CoordinatorClient client("127.0.0.1", server.port());
    ServiceRegistry registry(client);
    auto first = registry.Register("reconnect", "inst-1", "10.13.0.1:9000", 500ms);
    GO2CPP_REQUIRE(first.ok());
    client.Close();
    GO2CPP_CHECK(!client.connected());
    // 下一次调用自动重连；原租约仍有效，可以继续续约/查询。
    GO2CPP_CHECK(client.Connect().ok());
    GO2CPP_CHECK(registry.Lookup("reconnect").size() == 1);
    GO2CPP_CHECK(first.value.Renew().ok());
    first.value.Stop();
    server.Stop();
}

void TestServerStopClientFails() {
    go2cpp_tests::announce("transport.server_stop");
    Coordinator coordinator(MakeOptions());
    CoordinatorServerOptions options;
    options.port = 0;
    options.io_timeout = 500ms;
    CoordinatorServer server(coordinator, options);
    GO2CPP_REQUIRE(server.Start().ok());
    const std::uint16_t port = server.port();
    CoordinatorClient client("127.0.0.1", port);
    GO2CPP_CHECK(client.Connect().ok());
    GO2CPP_CHECK(client.Stats().active_leases == 0);

    server.Stop();
    // 服务器已停：连接失败必须表现为 kUnavailable，而不是伪造成功。
    const auto failed = client.RegisterService(ServiceRegistration{
        "after-stop", "inst-1", "10.14.0.1:9000", "inst-1", 500ms});
    GO2CPP_CHECK(!failed.ok());
    GO2CPP_CHECK(failed.status == Status::kUnavailable);
    GO2CPP_CHECK(!client.connected());

    // 同端口重启后，客户端下一次调用自动重连成功。
    CoordinatorServerOptions same_port;
    same_port.port = port;
    same_port.io_timeout = 500ms;
    CoordinatorServer restarted(coordinator, same_port);
    GO2CPP_REQUIRE(restarted.Start().ok());
    GO2CPP_CHECK(client.RegisterService(ServiceRegistration{
                     "after-restart", "inst-1", "10.14.0.1:9000", "inst-1", 500ms})
                     .ok());
    GO2CPP_CHECK(client.Stats().instances == 1);
    client.Close();
    restarted.Stop();
}

void TestLargeLookup() {
    go2cpp_tests::announce("transport.large_lookup");
    Coordinator coordinator(MakeOptions());
    CoordinatorServer server(coordinator);
    GO2CPP_REQUIRE(server.Start().ok());
    CoordinatorClient client("127.0.0.1", server.port());
    constexpr int kInstances = 500;
    for (int index = 0; index < kInstances; ++index) {
        ServiceRegistration registration;
        registration.service = "large";
        registration.instance = "inst-" + std::to_string(index);
        registration.endpoint = "10.15.0." + std::to_string(index % 250) + ":9000";
        registration.owner = registration.instance;
        registration.ttl = 5s;
        GO2CPP_REQUIRE(coordinator.RegisterService(registration).ok());
    }
    const auto instances = client.LookupService("large");
    GO2CPP_REQUIRE(instances.size() == kInstances);
    GO2CPP_CHECK(instances.front().instance == "inst-0");
    // 按实例 id 字典序排序：`inst-99` 排在 `inst-499` 之后，不能假设最后
    // 一个是注册顺序里的最后一个。
    GO2CPP_CHECK(std::any_of(instances.begin(), instances.end(),
                             [](const ServiceInstance &instance) {
                                 return instance.instance == "inst-499";
                             }));
    client.Close();
    server.Stop();
}

void TestConcurrentClients() {
    go2cpp_tests::announce("transport.concurrent_clients");
    Coordinator coordinator(MakeOptions());
    CoordinatorServerOptions options;
    options.io_timeout = 2000ms;
    CoordinatorServer server(coordinator, options);
    GO2CPP_REQUIRE(server.Start().ok());

    constexpr int kClients = 4;
    constexpr int kIterations = 150;
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::atomic<int> errors{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < kClients; ++worker) {
        threads.emplace_back([&, worker] {
            CoordinatorClient client("127.0.0.1", server.port());
            DistributedLock locks(client);
            LockOptions manual;
            manual.auto_renew = false;
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                auto acquired = locks.Acquire("multi-proc-lock",
                                              "worker-" + std::to_string(worker), 3000ms,
                                              manual);
                if (!acquired.ok()) {
                    if (acquired.status != Status::kConflict) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    --iteration;
                    std::this_thread::yield();
                    continue;
                }
                const int now_inside = inside.fetch_add(1, std::memory_order_acq_rel) + 1;
                int observed = max_inside.load(std::memory_order_relaxed);
                while (now_inside > observed &&
                       !max_inside.compare_exchange_weak(observed, now_inside)) {
                }
                inside.fetch_sub(1, std::memory_order_acq_rel);
                acquired.value.Release();
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (std::thread &thread : threads) {
        thread.join();
    }
    GO2CPP_CHECK(errors.load() == 0);
    GO2CPP_CHECK(max_inside.load() == 1);
    GO2CPP_CHECK(!coordinator.QueryLock("multi-proc-lock").has_value());
    server.Stop();
}

void TestMalformedFrameDoesNotKillServer() {
    go2cpp_tests::announce("transport.malformed_frame");
    Coordinator coordinator(MakeOptions());
    CoordinatorServerOptions options;
    options.max_frame_bytes = 1024;
    options.io_timeout = 500ms;
    CoordinatorServer server(coordinator, options);
    GO2CPP_REQUIRE(server.Start().ok());

    // 1) 未知操作码：服务端返回 protocol_error 后继续服务，不关闭连接。
    auto raw = go2cpp::net::DialTCP("127.0.0.1", server.port(), 1s);
    GO2CPP_REQUIRE(raw != nullptr);
    const std::uint8_t unknown_op[] = {0x00, 0x00, 0x00, 0x01, 0xFF};
    GO2CPP_CHECK(raw->send(unknown_op, sizeof(unknown_op), 0, 1s).ok());
    std::uint8_t reply[8]{};
    const auto reply_result = raw->recv(reply, sizeof(reply), 0, 1s);
    GO2CPP_REQUIRE(reply_result.ok());
    GO2CPP_CHECK(reply_result.transferred >= 5);
    GO2CPP_CHECK(reply[4] == static_cast<std::uint8_t>(Status::kProtocolError));
    raw->close();

    // 2) 超大长度帧：服务端拒绝并断开，但不影响已有客户端。
    CoordinatorClient client("127.0.0.1", server.port());
    GO2CPP_CHECK(client.Connect().ok());
    auto oversized = go2cpp::net::DialTCP("127.0.0.1", server.port(), 1s);
    GO2CPP_REQUIRE(oversized != nullptr);
    const std::uint8_t huge_header[] = {0x00, 0x10, 0x00, 0x00};  // 1 MiB > 1 KiB 上限
    GO2CPP_CHECK(oversized->send(huge_header, sizeof(huge_header), 0, 1s).ok());
    // 服务端关闭连接：读端最终得到 EOF；这里只等待连接被关（不阻塞断言）。
    SleepMs(50);
    oversized->close();
    GO2CPP_CHECK(client.Stats().active_leases == 0);
    client.Close();
    server.Stop();
}

} // namespace

int main() {
    go2cpp_tests::announce("distributed.transport");
    TestServerClientFacades();
    TestReconnect();
    TestServerStopClientFails();
    TestLargeLookup();
    TestConcurrentClients();
    TestMalformedFrameDoesNotKillServer();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_transport_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("distributed_transport_test: all cases passed\n");
    return 0;
}
