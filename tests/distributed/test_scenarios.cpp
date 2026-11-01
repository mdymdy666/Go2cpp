/**
 * @file test_scenarios.cpp
 * @brief 真实用法场景（单进程多线程模拟）：注册中心、任务锁、主备、分区、发布、重启。
 *
 * 这些场景刻意贴近生产故障形态（实例崩溃、任务超时、主进程退出、网络
 * 分区、滚动发布、协调者重启），而不是只测 API 的成功路径。
 */

#include "go2cpp/distributed/coordinator.hpp"
#include "go2cpp/distributed/election.hpp"
#include "go2cpp/distributed/lock.hpp"
#include "go2cpp/distributed/registry.hpp"
#include "go2cpp/distributed/transport.hpp"
#include "go2cpp/relay/hub.hpp"

#include "distributed_test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::distributed;
using namespace go2cpp::distributed::test;
using namespace std::chrono_literals;

bool AllValid(const std::vector<Registration> &registrations) {
    return std::all_of(registrations.begin(), registrations.end(),
                       [](const Registration &registration) { return registration.valid(); });
}

/// 场景 1：微服务注册中心——10 个实例，3 个进程崩溃（不再续约）。
void ScenarioMicroserviceRegistry() {
    go2cpp_tests::announce("scenario.microservice_registry");
    Coordinator coordinator(MakeOptions());
    ServiceRegistry registry(coordinator);
    std::vector<Registration> healthy;
    for (int index = 0; index < 7; ++index) {
        auto result = registry.Register("order-api", "pod-" + std::to_string(index),
                                        "10.20.0." + std::to_string(index + 1) + ":9000",
                                        150ms);
        GO2CPP_REQUIRE(result.ok());
        healthy.push_back(std::move(result.value));
    }
    for (int index = 7; index < 10; ++index) {
        // 这 3 个实例只上报一次就崩溃（没有心跳）。
        ServiceRegistration crashed;
        crashed.service = "order-api";
        crashed.instance = "pod-" + std::to_string(index);
        crashed.endpoint = "10.20.0." + std::to_string(index + 1) + ":9000";
        crashed.owner = crashed.instance;
        crashed.ttl = 90ms;
        GO2CPP_REQUIRE(coordinator.RegisterService(crashed).ok());
    }
    GO2CPP_CHECK(registry.Lookup("order-api").size() == 10);

    SleepMs(400);
    const auto alive = registry.Lookup("order-api");
    GO2CPP_REQUIRE(alive.size() == 7);
    std::set<std::string> names;
    for (const ServiceInstance &instance : alive) {
        GO2CPP_CHECK(names.insert(instance.instance).second);
        GO2CPP_CHECK(instance.instance.rfind("pod-", 0) == 0);
        GO2CPP_CHECK(instance.remaining > Duration::zero());
    }
    for (int index = 0; index < 7; ++index) {
        GO2CPP_CHECK(names.count("pod-" + std::to_string(index)) == 1);
    }
    GO2CPP_CHECK(AllValid(healthy));
}

/// 场景 2：分布式任务锁——A 执行超时，B 接管；A 必须发现自己已丢锁。
void ScenarioTaskLockTimeout() {
    go2cpp_tests::announce("scenario.task_lock_timeout");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    LockOptions manual;
    manual.auto_renew = false;

    auto holder = locks.Acquire("nightly-report", "worker-a", 120ms, manual);
    GO2CPP_REQUIRE(holder.ok());
    LockHandle old_holder = std::move(holder.value);
    const std::uint64_t old_token = old_holder.fencing_token();
    GO2CPP_CHECK(old_holder.valid());

    SleepMs(180);  // 模拟 A 在 GC/长任务里卡住，超过 TTL
    GO2CPP_CHECK(!old_holder.valid());
    GO2CPP_CHECK(old_holder.lost());

    auto takeover = locks.Acquire("nightly-report", "worker-b", 300ms, manual);
    GO2CPP_REQUIRE(takeover.ok());
    LockHandle new_holder = std::move(takeover.value);
    GO2CPP_CHECK(new_holder.fencing_token() > old_token);
    GO2CPP_CHECK(new_holder.owner() == "worker-b");
    // A 迟到的续约不能把 B 顶掉。
    GO2CPP_CHECK(!old_holder.Renew().ok());
    GO2CPP_CHECK(locks.Query("nightly-report")->owner == "worker-b");
    new_holder.Release();
}

/// 场景 3：三节点主备——主节点主动下线，备节点在 TTL 内接管。
void ScenarioLeaderFailover() {
    go2cpp_tests::announce("scenario.leader_failover");
    Coordinator coordinator(MakeOptions());
    LeaderElection election(coordinator);
    auto node1 = election.Join("primary", "node-1", 200ms);
    auto node2 = election.Join("primary", "node-2", 200ms);
    auto node3 = election.Join("primary", "node-3", 200ms);
    GO2CPP_REQUIRE(node1.ok() && node2.ok() && node3.ok());
    GO2CPP_REQUIRE(election.Leader("primary").has_value());
    GO2CPP_CHECK(election.Leader("primary")->node == "node-1");
    const std::uint64_t first_epoch = election.Leader("primary")->epoch;

    node1.value.Leave();
    const auto second = election.Leader("primary");
    GO2CPP_REQUIRE(second.has_value());
    GO2CPP_CHECK(second->node == "node-2");
    GO2CPP_CHECK(second->epoch > first_epoch);
    GO2CPP_CHECK(WaitUntil([&] { return node2.value.is_leader(); }, 1s));

    node2.value.Leave();
    const auto third = election.Leader("primary");
    GO2CPP_REQUIRE(third.has_value());
    GO2CPP_CHECK(third->node == "node-3");
    GO2CPP_CHECK(third->epoch > second->epoch);
    node3.value.Leave();
    GO2CPP_CHECK(!election.Leader("primary").has_value());
}

/// 场景 4：网络分区——持锁进程与协调者失联，锁被回收；恢复后不能再写。
void ScenarioNetworkPartition() {
    go2cpp_tests::announce("scenario.network_partition");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    LockOptions manual;
    manual.auto_renew = false;
    auto partitioned = locks.Acquire("replica-0", "node-a", 100ms, manual);
    GO2CPP_REQUIRE(partitioned.ok());
    LockHandle old = std::move(partitioned.value);
    const std::uint64_t stale_token = old.fencing_token();

    SleepMs(160);
    auto healthy = locks.Acquire("replica-0", "node-b", 300ms);
    GO2CPP_REQUIRE(healthy.ok());
    LockHandle fresh = std::move(healthy.value);
    GO2CPP_CHECK(fresh.fencing_token() > stale_token);

    // 下游资源（例如共享日志）用 token 做最后一道闸门：旧 token 被拒绝。
    std::uint64_t committed = fresh.fencing_token();
    auto append = [&committed](std::uint64_t token, const std::string &) {
        if (token < committed) {
            return false;
        }
        committed = token;
        return true;
    };
    GO2CPP_CHECK(append(fresh.fencing_token(), "node-b"));
    GO2CPP_CHECK(!append(stale_token, "node-a"));
    fresh.Release();
}

/// 场景 5：滚动发布——老实例逐个下线、新实例逐个上线，查询始终可用且无重复。
void ScenarioRollingDeploy() {
    go2cpp_tests::announce("scenario.rolling_deploy");
    Coordinator coordinator(MakeOptions());
    ServiceRegistry registry(coordinator);
    std::vector<Registration> old_version;
    std::vector<Registration> new_version;
    for (int index = 0; index < 3; ++index) {
        auto result = registry.Register("gateway", "v1-" + std::to_string(index),
                                        "10.21.0." + std::to_string(index + 1), 300ms);
        GO2CPP_REQUIRE(result.ok());
        old_version.push_back(std::move(result.value));
    }
    for (int index = 0; index < 3; ++index) {
        auto fresh = registry.Register("gateway", "v2-" + std::to_string(index),
                                       "10.22.0." + std::to_string(index + 1), 300ms);
        GO2CPP_REQUIRE(fresh.ok());
        // 新实例的句柄必须持有到场景结束：临时 Result 析构会立即注销。
        new_version.push_back(std::move(fresh.value));
        old_version[static_cast<std::size_t>(index)].Stop();
        const auto instances = registry.Lookup("gateway");
        GO2CPP_CHECK(instances.size() >= 3);
        GO2CPP_CHECK(instances.size() <= 4);
        std::set<std::string> seen;
        for (const ServiceInstance &instance : instances) {
            GO2CPP_CHECK(seen.insert(instance.instance).second);
            GO2CPP_CHECK(instance.remaining > Duration::zero());
        }
        GO2CPP_CHECK(seen.count("v2-" + std::to_string(index)) == 1);
    }
    const auto final_instances = registry.Lookup("gateway");
    GO2CPP_REQUIRE(final_instances.size() == 3);
    for (const ServiceInstance &instance : final_instances) {
        GO2CPP_CHECK(instance.instance.rfind("v2-", 0) == 0);
    }
}

/// 场景 6：协调者重启——客户端句柄必须失效（不能继续自认为主）。
void ScenarioCoordinatorRestart() {
    go2cpp_tests::announce("scenario.coordinator_restart");
    Coordinator coordinator(MakeOptions());
    CoordinatorServer server(coordinator);
    GO2CPP_REQUIRE(server.Start().ok());
    const std::uint16_t port = server.port();

    CoordinatorClient client("127.0.0.1", port);
    DistributedLock locks(client);
    auto acquired = locks.Acquire("restart-lock", "worker-a", 200ms);
    GO2CPP_REQUIRE(acquired.ok());
    LockHandle handle = std::move(acquired.value);
    GO2CPP_CHECK(handle.valid());
    Signal lost;
    handle.OnLost([&lost] { lost.Set(); });

    server.Stop();
    // 连接断开后心跳续约失败：最多一个 TTL 后句柄必须报 lost。
    GO2CPP_CHECK(lost.WaitFor(3s));
    GO2CPP_CHECK(handle.lost());
    GO2CPP_CHECK(!handle.valid());
    client.Close();
}

/// 场景 7：服务发现 + 连接池（relay::Hub）组合：实例下线时关闭其连接。
void ScenarioRegistryWithConnectionPool() {
    go2cpp_tests::announce("scenario.registry_with_hub");
    Coordinator coordinator(MakeOptions());
    ServiceRegistry registry(coordinator);
    go2cpp::relay::Hub hub;

    class CountingCloser final : public ::go2cpp::streams::Closer {
    public:
        explicit CountingCloser(std::atomic<int> *closed) : m_closed(closed) {}
        ::go2cpp::ErrorPtr Close() override {
            if (!m_closed_flag.exchange(true)) {
                m_closed->fetch_add(1);
            }
            return {};
        }

    private:
        std::atomic<int> *m_closed;
        std::atomic<bool> m_closed_flag{false};
    };

    std::atomic<int> closed{0};
    auto registration = registry.Register("storage", "node-1", "10.23.0.1:9000", 300ms);
    GO2CPP_REQUIRE(registration.ok());
    auto connection = std::make_shared<CountingCloser>(&closed);
    GO2CPP_CHECK(hub.Register("storage/node-1", connection));
    GO2CPP_CHECK(registry.Lookup("storage").size() == 1);
    // 实例下线：服务发现摘除 + 连接池关闭（两个动作由调用方编排）。
    registration.value.Stop();
    GO2CPP_CHECK(hub.CloseKey("storage/node-1"));
    GO2CPP_CHECK(closed.load() == 1);
    GO2CPP_CHECK(registry.Lookup("storage").empty());
}

} // namespace

int main() {
    go2cpp_tests::announce("distributed.scenarios");
    ScenarioMicroserviceRegistry();
    ScenarioTaskLockTimeout();
    ScenarioLeaderFailover();
    ScenarioNetworkPartition();
    ScenarioRollingDeploy();
    ScenarioCoordinatorRestart();
    ScenarioRegistryWithConnectionPool();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_scenarios_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("distributed_scenarios_test: all scenarios passed\n");
    return 0;
}
