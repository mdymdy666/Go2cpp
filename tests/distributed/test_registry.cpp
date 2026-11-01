/**
 * @file test_registry.cpp
 * @brief 服务发现：注册/发现、心跳保活、过期剔除、幂等与参数校验。
 *
 * 覆盖：
 * - 10 个实例注册后按实例 id 排序返回；
 * - 心跳保活跨过多个 TTL 仍在；
 * - 不续约的实例在 TTL 后被剔除（惰性查询 + 后台清扫两条路径）；
 * - 主动注销立即生效；重复注册幂等；不同 owner 抢占；
 * - 句柄失效（本地 deadline）、回调、move 语义；
 * - 服务列表与单实例查询。
 */

#include "go2cpp/distributed/registry.hpp"

#include "distributed_test_support.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::distributed;
using namespace go2cpp::distributed::test;

void TestTenInstancesRegisterAndLookup() {
    go2cpp_tests::announce("registry.ten_instances");
    Coordinator coordinator(MakeOptions());
    ServiceRegistry registry(coordinator);

    std::vector<Registration> registrations;
    registrations.reserve(10);
    for (int index = 0; index < 10; ++index) {
        auto result = registry.Register("order-api", MakeName("inst", static_cast<std::uint64_t>(index)),
                                        "10.0.0." + std::to_string(index + 1) + ":9000", 300ms);
        GO2CPP_REQUIRE(result.ok());
        GO2CPP_CHECK(result.value.valid());
        registrations.push_back(std::move(result.value));
    }

    const std::vector<ServiceInstance> instances = registry.Lookup("order-api");
    GO2CPP_REQUIRE(instances.size() == 10);
    for (std::size_t index = 0; index < instances.size(); ++index) {
        GO2CPP_CHECK(instances[index].instance == MakeName("inst", index));
        GO2CPP_CHECK(instances[index].service == "order-api");
        GO2CPP_CHECK(instances[index].endpoint ==
                     "10.0.0." + std::to_string(index + 1) + ":9000");
        GO2CPP_CHECK(instances[index].remaining > std::chrono::milliseconds::zero());
        GO2CPP_CHECK(instances[index].lease != kInvalidLease);
    }
    GO2CPP_CHECK(registry.Services() == std::vector<std::string>{"order-api"});
    const auto single = registry.Get("order-api", "inst-7");
    GO2CPP_REQUIRE(single.has_value());
    GO2CPP_CHECK(single->endpoint == "10.0.0.8:9000");
    GO2CPP_CHECK(!registry.Get("order-api", "missing").has_value());
    GO2CPP_CHECK(registry.Lookup("missing-service").empty());
}

void TestHeartbeatKeepsInstancesAlive() {
    go2cpp_tests::announce("registry.heartbeat_keepalive");
    Coordinator coordinator(MakeOptions());
    ServiceRegistry registry(coordinator);
    std::vector<Registration> registrations;
    for (int index = 0; index < 10; ++index) {
        auto result = registry.Register("pay-api", MakeName("inst", static_cast<std::uint64_t>(index)),
                                        "10.1.0." + std::to_string(index + 1) + ":8080", 150ms);
        GO2CPP_REQUIRE(result.ok());
        registrations.push_back(std::move(result.value));
    }
    // 稳定跨过 4 个 TTL，验证后台心跳确实在续约（而不是一次性注册）。
    SleepMs(600);
    const auto instances = registry.Lookup("pay-api");
    GO2CPP_CHECK(instances.size() == 10);
    for (const Registration &registration : registrations) {
        GO2CPP_CHECK(registration.valid());
        GO2CPP_CHECK(!registration.lost());
    }
    GO2CPP_CHECK(coordinator.Stats().instances == 10);
}

void TestExpiredInstancesRemoved() {
    go2cpp_tests::announce("registry.expire");
    Coordinator coordinator(MakeOptions());
    for (int index = 0; index < 3; ++index) {
        ServiceRegistration registration;
        registration.service = "inventory";
        registration.instance = MakeName("crashed", static_cast<std::uint64_t>(index));
        registration.endpoint = "10.2.0." + std::to_string(index + 1);
        registration.owner = registration.instance;
        registration.ttl = 80ms;
        GO2CPP_CHECK(coordinator.RegisterService(registration).ok());
    }
    // 已过期但清扫尚未跑到时，Lookup 也不能返回过期实例（惰性过滤）。
    SleepMs(100);
    GO2CPP_CHECK(coordinator.LookupService("inventory").empty());
    // 再等清扫回收索引。
    GO2CPP_CHECK(WaitUntil([&] { return coordinator.Stats().active_leases == 0; }, 2s));
    GO2CPP_CHECK(coordinator.ListServices().empty());
    GO2CPP_CHECK(coordinator.ListElections().empty());
}

void TestStopDeregistersImmediately() {
    go2cpp_tests::announce("registry.stop");
    Coordinator coordinator(MakeOptions());
    ServiceRegistry registry(coordinator);
    auto result = registry.Register("ship-api", "inst-1", "10.3.0.1:9000", 500ms);
    GO2CPP_REQUIRE(result.ok());
    Registration registration = std::move(result.value);
    GO2CPP_CHECK(registry.Lookup("ship-api").size() == 1);
    registration.Stop();
    GO2CPP_CHECK(!registration.valid());
    GO2CPP_CHECK(registry.Lookup("ship-api").empty());
    // 幂等：重复 Stop 不抛、不改变状态。
    registration.Stop();
    GO2CPP_CHECK(registry.Lookup("ship-api").empty());
}

void TestDuplicateRegistrationSemantics() {
    go2cpp_tests::announce("registry.duplicate");
    Coordinator coordinator(MakeOptions());
    ServiceRegistration first;
    first.service = "auth";
    first.instance = "node-1";
    first.endpoint = "10.4.0.1:80";
    first.owner = "node-1";
    first.ttl = 300ms;
    const auto registered = coordinator.RegisterService(first);
    GO2CPP_REQUIRE(registered.ok());

    // 同一 owner 重复注册：租约编号不变，endpoint 更新。
    ServiceRegistration again = first;
    again.endpoint = "10.4.0.2:80";
    const auto updated = coordinator.RegisterService(again);
    GO2CPP_REQUIRE(updated.ok());
    GO2CPP_CHECK(updated.value.id == registered.value.id);
    GO2CPP_CHECK(updated.value.endpoint == "10.4.0.2:80");

    // 不同 owner 抢占同一 (service, instance)：新租约编号更大。
    ServiceRegistration takeover = first;
    takeover.owner = "node-2";
    takeover.endpoint = "10.4.0.3:80";
    const auto replaced = coordinator.RegisterService(takeover);
    GO2CPP_REQUIRE(replaced.ok());
    GO2CPP_CHECK(replaced.value.id > registered.value.id);
    GO2CPP_CHECK(replaced.value.owner == "node-2");
    const auto instances = coordinator.LookupService("auth");
    GO2CPP_REQUIRE(instances.size() == 1);
    GO2CPP_CHECK(instances[0].lease == replaced.value.id);
    GO2CPP_CHECK(instances[0].endpoint == "10.4.0.3:80");
}

void TestInvalidArgumentsRejected() {
    go2cpp_tests::announce("registry.invalid_args");
    Coordinator coordinator(MakeOptions());
    ServiceRegistry registry(coordinator);
    GO2CPP_CHECK(!registry.Register("", "inst", "addr", 300ms).ok());
    GO2CPP_CHECK(!registry.Register("svc", "", "addr", 300ms).ok());
    // 低于 min_ttl(10ms) 与高于 max_ttl(10s) 都拒绝。
    GO2CPP_CHECK(!registry.Register("svc", "inst", "addr", 5ms).ok());
    GO2CPP_CHECK(!registry.Register("svc", "inst", "addr", 11s).ok());
    // 边界上合法。
    GO2CPP_CHECK(registry.Register("svc", "inst", "addr", 10ms).ok());
}

void TestManualRenewWithoutHeartbeat() {
    go2cpp_tests::announce("registry.manual_renew");
    CoordinatorOptions options = MakeOptions();
    Coordinator coordinator(options);
    ServiceRegistryOptions registry_options;
    registry_options.auto_renew = false;
    registry_options.default_ttl = 120ms;
    ServiceRegistry registry(coordinator, registry_options);

    auto result = registry.Register("manual", "inst-1", "10.5.0.1:9000");
    GO2CPP_REQUIRE(result.ok());
    Registration registration = std::move(result.value);
    GO2CPP_CHECK(registration.valid());
    for (int attempt = 0; attempt < 6; ++attempt) {
        SleepMs(60);
        GO2CPP_REQUIRE(registration.Renew().ok());
        GO2CPP_CHECK(registration.valid());
    }
    GO2CPP_CHECK(registry.Lookup("manual").size() == 1);

    // 停止续约：本地 deadline 到期后句柄必须自己报警，而不是一直说 valid。
    GO2CPP_CHECK(WaitUntil([&] { return !registration.valid(); }, 1s));
    GO2CPP_CHECK(registration.lost());
    // 到期后协调者可能已经清扫（kNotFound）或仍保留记录（kExpired），
    // 两种都是失败；这里只要求不是 kOk。
    GO2CPP_CHECK(!registration.Renew().ok());
}

void TestLostCallback() {
    go2cpp_tests::announce("registry.lost_callback");
    Coordinator coordinator(MakeOptions());
    ServiceRegistry registry(coordinator);
    auto result = registry.Register("callback", "inst-1", "10.6.0.1:9000", 100ms);
    GO2CPP_REQUIRE(result.ok());
    Registration registration = std::move(result.value);
    Signal signal;
    registration.OnLost([&signal] { signal.Set(); });
    // 用协调者强制释放（等价于租约被清扫/被抢占）：下一次心跳续约会失败。
    GO2CPP_CHECK(coordinator.ReleaseLease(registration.lease(), "inst-1").ok());
    GO2CPP_CHECK(signal.WaitFor(2s));
    GO2CPP_CHECK(registration.lost());
    GO2CPP_CHECK(!registration.valid());
}

void TestMoveSemantics() {
    go2cpp_tests::announce("registry.move");
    Coordinator coordinator(MakeOptions());
    ServiceRegistry registry(coordinator);
    auto first = registry.Register("move", "inst-1", "10.7.0.1:9000", 300ms);
    auto second = registry.Register("move", "inst-2", "10.7.0.2:9000", 300ms);
    GO2CPP_REQUIRE(first.ok() && second.ok());
    Registration a = std::move(first.value);
    Registration b = std::move(second.value);

    // move 构造：源句柄失效，目标接管。
    Registration moved = std::move(a);
    GO2CPP_CHECK(!a.valid());
    GO2CPP_CHECK(moved.valid());
    GO2CPP_CHECK(registry.Lookup("move").size() == 2);

    // move 赋值：被覆盖的句柄必须先注销，不能留下幽灵实例。
    b = std::move(moved);
    GO2CPP_CHECK(!moved.valid());
    GO2CPP_CHECK(b.valid());
    const auto instances = registry.Lookup("move");
    GO2CPP_REQUIRE(instances.size() == 1);
    GO2CPP_CHECK(instances[0].instance == "inst-1");
}

} // namespace

int main() {
    go2cpp_tests::announce("distributed.registry");
    TestTenInstancesRegisterAndLookup();
    TestHeartbeatKeepsInstancesAlive();
    TestExpiredInstancesRemoved();
    TestStopDeregistersImmediately();
    TestDuplicateRegistrationSemantics();
    TestInvalidArgumentsRejected();
    TestManualRenewWithoutHeartbeat();
    TestLostCallback();
    TestMoveSemantics();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_registry_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("distributed_registry_test: all cases passed\n");
    return 0;
}
