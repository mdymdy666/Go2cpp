/**
 * @file test_realtime_room.cpp
 * @brief 通用实时房间组件的无框架测试：同房间广播、房间隔离、慢连接踢出、
 *        Leave/CloseAll 幂等与并发广播。
 *
 * @details 不依赖第三方测试框架：每条连接一个 `Inbox` 收集 `SendFn` 收到的
 *          消息，断言用 `Check()` 累计。并发用例用 100 连接 × 100 条广播，
 *          校验入队总数、接收总数与每条连接的条数。
 *
 * @author go2cpp contributors
 * @date 2026-11-03
 * @copyright MIT License
 */

#include "go2cpp/app/realtime_room.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::app::realtime;

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

/// @brief 轮询等待条件成立（发送线程是异步的，断言前必须等到）。
bool WaitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

/// @brief 一条连接的收件箱：`SendFn` 追加，测试线程轮询。
class Inbox {
   public:
    void Push(std::string_view text) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_messages.emplace_back(text);
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_messages.size();
    }

    std::vector<std::string> Copy() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_messages;
    }

   private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_messages;
};

/// @brief 1. 两个连接同房间：两条广播各收两条，顺序一致。
void TestSameRoomBroadcast() {
    std::printf("\n[1] 两连接同房间广播\n");
    Inbox first;
    Inbox second;
    RoomHub hub;

    const RoomHub::ConnId id_first = hub.Join("lobby", [&first](std::string_view text) {
        first.Push(text);
        return true;
    });
    const RoomHub::ConnId id_second = hub.Join("lobby", [&second](std::string_view text) {
        second.Push(text);
        return true;
    });

    Check(id_first != 0 && id_second != 0 && id_first != id_second, "两条连接拿到不同 id");
    Check(hub.Online("lobby") == 2, "Online=2");
    Check(hub.TotalOnline() == 2, "TotalOnline=2");
    Check(hub.Broadcast("lobby", "hello-1") == 2, "第 1 条入队 2 个连接");
    Check(hub.Broadcast("lobby", "hello-2") == 2, "第 2 条入队 2 个连接");

    const bool both = WaitFor([&first, &second] { return first.size() == 2 && second.size() == 2; },
                              std::chrono::seconds(2));
    Check(both, "两连接各收 2 条");
    Check(first.Copy() == std::vector<std::string>{"hello-1", "hello-2"}, "连接 A 顺序与内容正确");
    Check(second.Copy() == std::vector<std::string>{"hello-1", "hello-2"}, "连接 B 顺序与内容正确");

    hub.Leave(id_first);
    Check(hub.Online("lobby") == 1, "Leave 一个后 Online=1");
    hub.Leave(id_second);
    Check(hub.Online("lobby") == 0, "Leave 全部后 Online=0");
}

/// @brief 2. 三个连接两个房间：广播只落到本房间。
void TestRoomIsolation() {
    std::printf("\n[2] 三连接两房间隔离\n");
    Inbox alpha_a;
    Inbox alpha_b;
    Inbox beta;
    RoomHub hub;

    const RoomHub::ConnId id_alpha_a = hub.Join("alpha", [&alpha_a](std::string_view text) {
        alpha_a.Push(text);
        return true;
    });
    const RoomHub::ConnId id_alpha_b = hub.Join("alpha", [&alpha_b](std::string_view text) {
        alpha_b.Push(text);
        return true;
    });
    const RoomHub::ConnId id_beta = hub.Join("beta", [&beta](std::string_view text) {
        beta.Push(text);
        return true;
    });

    Check(hub.Online("alpha") == 2, "alpha 在线 2");
    Check(hub.Online("beta") == 1, "beta 在线 1");
    Check(hub.TotalOnline() == 3, "TotalOnline=3");

    Check(hub.Broadcast("alpha", "a-1") == 2, "alpha 广播入队 2");
    Check(hub.Broadcast("beta", "b-1") == 1, "beta 广播入队 1");
    Check(hub.Broadcast("gamma", "lost") == 0, "不存在的房间入队 0");
    Check(hub.Online("gamma") == 0, "不存在的房间 Online=0");

    const bool arrived = WaitFor(
        [&alpha_a, &alpha_b, &beta] {
            return alpha_a.size() == 1 && alpha_b.size() == 1 && beta.size() == 1;
        },
        std::chrono::seconds(2));
    Check(arrived, "三个连接各收 1 条");
    Check(alpha_a.Copy() == std::vector<std::string>{"a-1"}, "alpha 连接 A 只收到 alpha 消息");
    Check(alpha_b.Copy() == std::vector<std::string>{"a-1"}, "alpha 连接 B 只收到 alpha 消息");
    Check(beta.Copy() == std::vector<std::string>{"b-1"}, "beta 连接只收到 beta 消息");

    hub.Leave(id_alpha_a);
    hub.Leave(id_alpha_b);
    hub.Leave(id_beta);
    Check(hub.TotalOnline() == 0, "全部 Leave 后 TotalOnline=0");
    Check(hub.Online("alpha") == 0 && hub.Online("beta") == 0, "两个房间都清空");
}

/// @brief 3. 慢连接塞满队列被踢，快连接完整收到全部消息。
void TestSlowConsumerEvicted() {
    std::printf("\n[3] 慢连接队列满被踢\n");
    RoomConfig config;
    config.send_queue_cap = 4;
    Inbox fast;
    std::atomic<int> slow_sent{0};
    RoomHub hub(config);

    hub.Join("lag", [&slow_sent](std::string_view) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        slow_sent.fetch_add(1, std::memory_order_relaxed);
        return true;
    });
    hub.Join("lag", [&fast](std::string_view text) {
        fast.Push(text);
        return true;
    });
    Check(hub.Online("lag") == 2, "两个连接在线");

    // 先让慢连接取走一条并进入 200ms 的发送；此后它无法再消费新入队的消息。
    Check(hub.Broadcast("lag", "warm") == 2, "预热消息入队 2");
    Check(WaitFor([&fast] { return fast.size() == 1; }, std::chrono::seconds(2)), "快连接收到预热消息");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 每条之间留 2ms 给快连接消费：容量 4 是 hub 级配置，若主线程瞬间灌 20 条，
    // 快连接也会因为来不及消费而被误判为慢连接。
    for (int i = 0; i < 20; ++i) {
        hub.Broadcast("lag", "burst-" + std::to_string(i));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    Check(WaitFor([&hub] { return hub.Online("lag") == 1; }, std::chrono::seconds(3)),
          "慢连接被踢出（Online=1）");
    Check(slow_sent.load() <= 2, "慢连接在途消息不超过 1 条");

    const bool fast_ok = WaitFor([&fast] { return fast.size() == 21; }, std::chrono::seconds(2));
    Check(fast_ok, "快连接完整收到 21 条，不受慢连接影响");
    Check(hub.Broadcast("lag", "after") == 1, "踢出后广播只入队快连接");
    Check(WaitFor([&fast] { return fast.size() == 22; }, std::chrono::seconds(2)),
          "快连接继续收到新消息");

    hub.CloseAll();
    Check(hub.Online("lag") == 0, "CloseAll 后 Online=0");
}

/// @brief 4. Leave / CloseAll 幂等，离开后 SendFn 不再被调用。
void TestLeaveAndCloseAll() {
    std::printf("\n[4] Leave / CloseAll 幂等与停止发送\n");
    Inbox a;
    Inbox b;
    Inbox c;
    Inbox d;
    RoomHub hub;

    const RoomHub::ConnId id_a = hub.Join("stage", [&a](std::string_view text) {
        a.Push(text);
        return true;
    });
    const RoomHub::ConnId id_b = hub.Join("stage", [&b](std::string_view text) {
        b.Push(text);
        return true;
    });
    const RoomHub::ConnId id_c = hub.Join("stage", [&c](std::string_view text) {
        c.Push(text);
        return true;
    });
    hub.Join("stage", [&d](std::string_view text) {
        d.Push(text);
        return true;
    });

    Check(hub.Online("stage") == 4, "四个连接在线");
    Check(hub.Broadcast("stage", "m1") == 4, "首条广播入队 4");
    Check(WaitFor([&a, &b, &c, &d] {
              return a.size() == 1 && b.size() == 1 && c.size() == 1 && d.size() == 1;
          }, std::chrono::seconds(2)),
          "四个连接各收 1 条");

    hub.Leave(id_a);
    hub.Leave(id_b);
    Check(hub.Online("stage") == 2, "Leave 两个后 Online=2");
    Check(hub.Broadcast("stage", "m2") == 2, "其后广播入队 2");
    Check(WaitFor([&c, &d] { return c.size() == 2 && d.size() == 2; }, std::chrono::seconds(2)),
          "剩余两个连接各收 2 条");
    Check(a.size() == 1 && b.size() == 1, "已 Leave 的连接不再收到消息");

    hub.Leave(id_a);  // 幂等：重复 Leave 不崩溃、不影响计数
    hub.Leave(0);
    Check(hub.Online("stage") == 2, "重复 Leave 不改变在线数");

    hub.CloseAll();
    Check(hub.Online("stage") == 0, "CloseAll 后 Online=0");
    Check(hub.TotalOnline() == 0, "CloseAll 后 TotalOnline=0");
    const std::size_t c_before = c.size();
    const std::size_t d_before = d.size();
    Check(hub.Broadcast("stage", "m3") == 0, "CloseAll 后广播入队 0");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Check(c.size() == c_before && d.size() == d_before, "CloseAll 后 SendFn 不再被调用");

    hub.CloseAll();  // 幂等
    hub.Leave(id_c);  // 已失效 id
    Check(hub.TotalOnline() == 0, "重复 CloseAll 后仍为 0");
}

/// @brief 5. 100 连接 × 100 条广播：并发发布无崩溃，接收总数正确。
void TestConcurrentBroadcast() {
    std::printf("\n[5] 100 连接 × 100 条并发广播\n");
    constexpr int kConnections = 100;
    constexpr int kRounds = 100;
    constexpr std::size_t kExpected = static_cast<std::size_t>(kConnections) * kRounds;

    std::vector<std::unique_ptr<Inbox>> inboxes;
    inboxes.reserve(kConnections);
    std::atomic<std::size_t> received{0};
    RoomHub hub;  // 在 inboxes 之后声明：hub 先析构，SendFn 捕获的 Inbox 仍有效。

    for (int i = 0; i < kConnections; ++i) {
        auto inbox = std::make_unique<Inbox>();
        Inbox* raw = inbox.get();
        const RoomHub::ConnId id = hub.Join("bulk", [raw, &received](std::string_view text) {
            raw->Push(text);
            received.fetch_add(1, std::memory_order_relaxed);
            return true;
        });
        if (id == 0) {
            Check(false, "连接加入失败");
            return;
        }
        inboxes.push_back(std::move(inbox));
    }
    Check(hub.Online("bulk") == kConnections, "100 连接全部在线");

    std::atomic<std::size_t> queued{0};
    const auto publish = [&hub, &queued](int rounds) {
        for (int i = 0; i < rounds; ++i) {
            queued.fetch_add(hub.Broadcast("bulk", "payload"), std::memory_order_relaxed);
        }
    };
    std::thread publisher_a(publish, kRounds / 2);
    std::thread publisher_b(publish, kRounds / 2);
    publisher_a.join();
    publisher_b.join();

    Check(queued.load() == kExpected, "100 条广播共入队 10000 次");
    Check(WaitFor([&received] { return received.load() == kExpected; }, std::chrono::seconds(10)),
          "接收总数 = 10000");
    Check(hub.Online("bulk") == kConnections, "广播后 100 连接仍在线");

    bool all_full = true;
    for (const auto& inbox : inboxes) {
        if (inbox->size() != static_cast<std::size_t>(kRounds)) {
            all_full = false;
            break;
        }
    }
    Check(all_full, "每条连接恰好收到 100 条");

    hub.CloseAll();
    Check(hub.TotalOnline() == 0, "CloseAll 后并发连接归零");
    const std::size_t after = received.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Check(received.load() == after, "CloseAll 后接收数冻结");
}

}  // namespace

int main() {
    std::printf("[realtime_room] 通用实时房间组件测试\n");
    TestSameRoomBroadcast();
    TestRoomIsolation();
    TestSlowConsumerEvicted();
    TestLeaveAndCloseAll();
    TestConcurrentBroadcast();
    std::printf("\n[realtime_room] checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
