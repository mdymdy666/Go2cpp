/**
 * @file test_relay_scenarios.cpp
 * @brief 把 Hub / Lease / Heartbeat 放进 JinShan relay 的真实用法里跑（≥5 个场景）。
 *
 * 场景（每个都模拟一个真实链路，而不是单 API 冒烟）：
 * 1. 房间生命周期：一个房间挂控制/数据连接，CloseKey 只清该房间；
 * 2. 半开判定：收到对端数据后 Renew，静默超时后租约关闭整个房间；
 * 3. 心跳续期：心跳成功即续租；对端失联后停止续租，租约只到期一次；
 * 4. 重连竞态：旧连接关闭后新连接登记同 key，旧租约停止，新连接不受影响；
 * 5. 心跳死亡：send 失败 → on_dead 关连接、停租约、清空 Hub；
 * 6. 1000 个房间 churn：每个连接恰好关闭一次，无 shared_ptr 泄漏；
 * 7. 并发房间 churn：8 个工作线程 + 2 个清扫线程并发 CloseKey，连接不被重复关闭。
 */
#include "go2cpp/relay/hub.hpp"
#include "go2cpp/relay/lease.hpp"

#include "relay_test_util.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::relay::Heartbeat;
using go2cpp::relay::Hub;
using go2cpp::relay::Lease;
using go2cpp::relay::test::FakeStream;
using go2cpp::relay::test::SleepMs;

/** 场景 1：房间生命周期 —— 只关闭目标房间，其它房间不受影响。 */
void ScenarioRoomLifecycle() {
    Hub hub;
    std::atomic<std::uint64_t> closes{0};
    auto control = std::make_shared<FakeStream>(&closes);
    auto data = std::make_shared<FakeStream>(&closes);
    auto other_room_viewer = std::make_shared<FakeStream>(&closes);

    GO2CPP_CHECK(hub.Register("room:alpha", control));
    GO2CPP_CHECK(hub.Register("room:alpha", data));
    GO2CPP_CHECK(hub.Register("room:beta", other_room_viewer));
    GO2CPP_CHECK(hub.Lookup("room:alpha").size() == 2);

    GO2CPP_CHECK(hub.CloseKey("room:alpha"));
    GO2CPP_CHECK(closes.load() == 2);
    GO2CPP_CHECK(control->invocations() == 1);
    GO2CPP_CHECK(data->invocations() == 1);
    GO2CPP_CHECK(hub.Lookup("room:alpha").empty());
    GO2CPP_CHECK(hub.Keys() == std::vector<std::string>{"room:beta"});
    GO2CPP_CHECK(!other_room_viewer->closed());
}

/** 场景 2：半开连接 —— 静默超过租约窗口后，租约关闭整个房间。 */
void ScenarioHalfOpenDetection() {
    Hub hub;
    auto control = std::make_shared<FakeStream>();
    GO2CPP_CHECK(hub.Register("viewer:1", control));

    Lease lease(200ms, [&hub] { hub.CloseKey("viewer:1"); });
    SleepMs(80);
    lease.Renew(); // 模拟收到对端数据，续约一次

    GO2CPP_REQUIRE_EVENTUALLY(control->closed(), 5s);
    GO2CPP_CHECK(hub.Lookup("viewer:1").empty());
    GO2CPP_CHECK(hub.Keys().empty());
    GO2CPP_CHECK(control->invocations() == 1);
}

/** 场景 3：心跳成功即续租；对端失联后停止续租，租约恰好到期一次。 */
void ScenarioHeartbeatKeepsLeaseAlive() {
    std::atomic<int> heartbeats{0};
    std::atomic<int> expiries{0};
    Lease lease(300ms, [&expiries] { expiries.fetch_add(1); });
    Heartbeat heartbeat(40ms);
    heartbeat.Start(
        nullptr,
        [&heartbeats, &lease] {
            const int count = heartbeats.fetch_add(1) + 1;
            lease.Renew(); // 心跳发送成功 → 续约
            return count < 5; // 第 5 次起模拟对端失联
        },
        nullptr);

    GO2CPP_REQUIRE_EVENTUALLY(heartbeats.load() >= 5, 5s);
    GO2CPP_REQUIRE_EVENTUALLY(expiries.load() == 1, 5s);
    SleepMs(300);
    GO2CPP_CHECK(expiries.load() == 1);
    heartbeat.Stop();
}

/** 场景 4：重连 —— 旧连接关闭后新连接登记同 key，旧租约停止。 */
void ScenarioReconnectRace() {
    Hub hub;
    std::atomic<std::uint64_t> closes{0};

    auto old_connection = std::make_shared<FakeStream>(&closes);
    GO2CPP_CHECK(hub.Register("room:gamma", old_connection));
    Lease old_lease(1s, nullptr);
    GO2CPP_CHECK(hub.CloseKey("room:gamma"));
    old_lease.Stop();

    auto new_connection = std::make_shared<FakeStream>(&closes);
    GO2CPP_CHECK(hub.Register("room:gamma", new_connection));
    const auto connections = hub.Lookup("room:gamma");
    GO2CPP_CHECK(connections.size() == 1);
    if (connections.size() == 1) {
        GO2CPP_CHECK(connections[0].get() == new_connection.get());
    }
    GO2CPP_CHECK(old_connection->closed());
    GO2CPP_CHECK(!new_connection->closed());
    GO2CPP_CHECK(closes.load() == 1);

    GO2CPP_CHECK(hub.CloseKey("room:gamma"));
    GO2CPP_CHECK(closes.load() == 2);
    GO2CPP_CHECK(hub.Keys().empty());
}

/** 场景 5：心跳死亡 —— on_dead 里清理房间并停掉租约。 */
void ScenarioHeartbeatDeathClosesRoom() {
    Hub hub;
    auto control = std::make_shared<FakeStream>();
    GO2CPP_CHECK(hub.Register("room:dead", control));

    std::atomic<int> deaths{0};
    Lease lease(5s, [&hub] { hub.CloseKey("room:dead"); });
    Heartbeat heartbeat(20ms);
    heartbeat.Start(
        nullptr, [] { return false; }, [&] {
            deaths.fetch_add(1);
            hub.CloseKey("room:dead");
            lease.Stop();
        });

    GO2CPP_REQUIRE_EVENTUALLY(deaths.load() == 1 && control->closed(), 5s);
    GO2CPP_CHECK(hub.Lookup("room:dead").empty());
    GO2CPP_CHECK(hub.Keys().empty());
    GO2CPP_CHECK(control->invocations() == 1);
    heartbeat.Stop();
}

/** 场景 6：1000 个房间快速建/查/销毁，连接恰好关闭一次且无泄漏。 */
void ScenarioRoomChurnNoLeak() {
    constexpr int kRooms = 1000;
    Hub hub;
    std::atomic<std::uint64_t> closes{0};
    std::vector<std::weak_ptr<FakeStream>> watches;
    watches.reserve(kRooms * 2);

    for (int index = 0; index < kRooms; ++index) {
        const std::string key = "room:" + std::to_string(index);
        auto control = std::make_shared<FakeStream>(&closes);
        auto data = std::make_shared<FakeStream>(&closes);
        watches.push_back(control);
        watches.push_back(data);
        GO2CPP_CHECK(hub.Register(key, control));
        GO2CPP_CHECK(hub.Register(key, data));
        GO2CPP_CHECK(hub.Lookup(key).size() == 2);
        GO2CPP_CHECK(hub.CloseKey(key));
    }

    GO2CPP_CHECK(hub.Keys().empty());
    GO2CPP_CHECK(closes.load() == static_cast<std::uint64_t>(kRooms) * 2);
    for (const auto &watch : watches) {
        GO2CPP_CHECK(watch.expired()); // Hub 与 CloseKey 都不残留引用
    }
}

/** 场景 7：并发建/查/清扫房间，CloseKey 与登记竞争时连接最多被关一次。 */
void ScenarioConcurrentRoomChurn() {
    constexpr int kWorkers = 8;
    constexpr int kIterationsPerWorker = 400;
    constexpr int kSweepers = 2;

    Hub hub;
    std::vector<std::thread> threads;
    std::atomic<bool> stop_sweepers{false};

    for (int worker = 0; worker < kWorkers; ++worker) {
        threads.emplace_back([&hub, worker] {
            std::vector<std::shared_ptr<FakeStream>> connections;
            connections.reserve(kIterationsPerWorker * 2);
            for (int iteration = 0; iteration < kIterationsPerWorker; ++iteration) {
                const std::string key =
                    "room:" + std::to_string(worker) + ":" + std::to_string(iteration);
                auto control = std::make_shared<FakeStream>();
                auto data = std::make_shared<FakeStream>();
                connections.push_back(control);
                connections.push_back(data);
                GO2CPP_CHECK(hub.Register(key, control));
                GO2CPP_CHECK(hub.Register(key, data));
                const auto snapshot = hub.Lookup(key);
                for (const auto &entry : snapshot) {
                    GO2CPP_CHECK(entry != nullptr);
                }
                if (iteration % 2 == 0) {
                    // 清扫线程可能已经先一步关掉了这个 key：CloseKey 返回 false
                    // 也是正确结果，这里只断言「本线程处理完以后 key 不再可见」。
                    (void)hub.CloseKey(key);
                    GO2CPP_CHECK(hub.Lookup(key).empty());
                } else {
                    hub.Unregister(key, control);
                    hub.Unregister(key, data);
                    GO2CPP_CHECK(hub.Lookup(key).empty());
                }
            }
            // 每条连接只挂在一个 key 下：Hub 至多调用一次 Close。
            for (const auto &connection : connections) {
                GO2CPP_CHECK(connection->invocations() <= 1);
            }
        });
    }
    for (int sweeper = 0; sweeper < kSweepers; ++sweeper) {
        threads.emplace_back([&hub, &stop_sweepers] {
            std::mt19937_64 random(20261102);
            while (!stop_sweepers.load(std::memory_order_relaxed)) {
                const auto keys = hub.Keys();
                if (!keys.empty()) {
                    const auto &key = keys[random() % keys.size()];
                    (void)hub.CloseKey(key);
                } else {
                    SleepMs(1);
                }
            }
        });
    }
    for (int worker = 0; worker < kWorkers; ++worker) {
        threads[static_cast<std::size_t>(worker)].join();
    }
    stop_sweepers.store(true, std::memory_order_relaxed);
    for (int sweeper = 0; sweeper < kSweepers; ++sweeper) {
        threads[static_cast<std::size_t>(kWorkers + sweeper)].join();
    }
    GO2CPP_CHECK(hub.Keys().empty());
}

} // namespace

int main() {
    go2cpp_tests::announce("relay.scenarios");

    ScenarioRoomLifecycle();
    ScenarioHalfOpenDetection();
    ScenarioHeartbeatKeepsLeaseAlive();
    ScenarioReconnectRace();
    ScenarioHeartbeatDeathClosesRoom();
    ScenarioRoomChurnNoLeak();
    ScenarioConcurrentRoomChurn();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "relay_scenarios_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("relay_scenarios_test: all scenarios passed\n");
    return 0;
}
