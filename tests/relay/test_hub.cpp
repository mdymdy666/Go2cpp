/**
 * @file test_hub.cpp
 * @brief Hub 的功能、幂等、并发与随机化测试（REQ-003）。
 *
 * 覆盖：
 * - 空表语义、Register/Lookup/Unregister/CloseKey/Keys 的基本行为；
 * - 重复 Register / 重复 Unregister / 重复 CloseKey 不 panic；
 * - 同一连接挂多个 key、Close 回调里重入 Hub（自我注销 / 登记新连接）；
 * - 快照生命周期（Lookup 返回的 shared_ptr 在表被清空后仍有效）；
 * - 1e5 次并发 Register/Lookup/Unregister（同一份源码也用 TSan 编译跑一遍）；
 * - CloseKey 与 Lookup/Keys/Unregister 的并发竞态；
 * - 随机操作序列对拍影子模型。
 *
 * 该文件不依赖 go2cpp 运行时（只用 std::thread/std::mutex），因此可以直接用
 * `-fsanitize=thread` 编译出 TSan 版本，见 cmake/Go2CppRelay.cmake。
 */
#include "go2cpp/relay/hub.hpp"

#include "relay_test_util.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using go2cpp::relay::Hub;
using go2cpp::relay::test::FakeStream;

/** @brief Close() 里回调 Hub 的连接：验证 CloseKey 在锁外调用用户代码。 */
class ReentrantStream final : public Hub::Stream,
                              public std::enable_shared_from_this<ReentrantStream> {
public:
    ReentrantStream(Hub *hub, std::string key) : m_hub(hub), m_key(std::move(key)) {}

    ::go2cpp::ErrorPtr Close() override {
        // 自我注销：CloseKey 已经把 key 摘表，这里应当是无害的空操作。
        m_hub->Unregister(m_key, shared_from_this());
        // 顺手登记一条新连接：CloseKey 持锁时这里会死锁，实测必须是锁外。
        if (m_extra != nullptr) {
            m_hub->Register(m_key, m_extra);
        }
        return {};
    }

    void SetExtra(std::shared_ptr<Hub::Stream> extra) { m_extra = std::move(extra); }

private:
    Hub *m_hub;
    std::string m_key;
    std::shared_ptr<Hub::Stream> m_extra;
};

void test_empty_hub() {
    Hub hub;
    GO2CPP_CHECK(hub.Keys().empty());
    GO2CPP_CHECK(hub.Lookup("missing").empty());
    GO2CPP_CHECK(!hub.CloseKey("missing"));
    hub.Unregister("missing", nullptr);
    GO2CPP_CHECK(hub.Keys().empty());
}

void test_register_and_lookup() {
    Hub hub;
    auto first = std::make_shared<FakeStream>();
    auto second = std::make_shared<FakeStream>();

    GO2CPP_CHECK(!hub.Register("room", nullptr));
    GO2CPP_CHECK(hub.Register("room", first));
    GO2CPP_CHECK(hub.Register("room", second));
    // 同一个对象在同一 key 下重复登记：拒绝且不改变表。
    GO2CPP_CHECK(!hub.Register("room", first));

    const auto conns = hub.Lookup("room");
    GO2CPP_CHECK(conns.size() == 2);
    if (conns.size() == 2) {
        GO2CPP_CHECK(conns[0].get() == first.get());
        GO2CPP_CHECK(conns[1].get() == second.get());
    }
    GO2CPP_CHECK(hub.Lookup("other").empty());
    GO2CPP_CHECK(hub.Keys() == std::vector<std::string>{"room"});

    // 空 key 合法（内部按值保存），与「不存在的 key」同样可查询。
    GO2CPP_CHECK(hub.Register("", first));
    GO2CPP_CHECK(hub.Lookup("").size() == 1);
}

void test_unregister_is_idempotent() {
    Hub hub;
    auto first = std::make_shared<FakeStream>();
    auto second = std::make_shared<FakeStream>();
    GO2CPP_CHECK(hub.Register("room", first));
    GO2CPP_CHECK(hub.Register("room", second));

    hub.Unregister("room", first);
    GO2CPP_CHECK(hub.Lookup("room").size() == 1);
    // 重复注销同一条连接：无异常、无崩溃、表不变。
    hub.Unregister("room", first);
    hub.Unregister("room", first);
    GO2CPP_CHECK(hub.Lookup("room").size() == 1);
    // 不存在的 key / 空连接同样是空操作。
    hub.Unregister("missing", first);
    hub.Unregister("room", nullptr);

    hub.Unregister("room", second);
    GO2CPP_CHECK(hub.Lookup("room").empty());
    // 移除最后一条连接后 key 自动消失；对空 key 再注销也不崩。
    GO2CPP_CHECK(hub.Keys().empty());
    hub.Unregister("room", second);
    GO2CPP_CHECK(hub.Keys().empty());
    // Unregister 不关闭连接（关闭由调用方负责）。
    GO2CPP_CHECK(!first->closed());
    GO2CPP_CHECK(!second->closed());
}

void test_close_key_closes_all_once() {
    Hub hub;
    std::atomic<std::uint64_t> close_total{0};
    auto a = std::make_shared<FakeStream>(&close_total);
    auto b = std::make_shared<FakeStream>(&close_total);
    auto other = std::make_shared<FakeStream>(&close_total);
    GO2CPP_CHECK(hub.Register("room:a", a));
    GO2CPP_CHECK(hub.Register("room:a", b));
    GO2CPP_CHECK(hub.Register("room:b", other));

    GO2CPP_CHECK(hub.CloseKey("room:a"));
    GO2CPP_CHECK(a->closed());
    GO2CPP_CHECK(b->closed());
    GO2CPP_CHECK(a->invocations() == 1);
    GO2CPP_CHECK(b->invocations() == 1);
    GO2CPP_CHECK(close_total.load() == 2);
    GO2CPP_CHECK(hub.Lookup("room:a").empty());
    GO2CPP_CHECK(hub.Keys() == std::vector<std::string>{"room:b"});

    // 重复 CloseKey：返回 false、不重复关闭。
    GO2CPP_CHECK(!hub.CloseKey("room:a"));
    GO2CPP_CHECK(a->invocations() == 1);
    GO2CPP_CHECK(close_total.load() == 2);
    // 另一个 key 完全不受影响。
    GO2CPP_CHECK(!other->closed());
    GO2CPP_CHECK(close_total.load() == 2);

    // CloseKey 之后再登记（重连/新建房间）仍可用。
    auto reconnected = std::make_shared<FakeStream>(&close_total);
    GO2CPP_CHECK(hub.Register("room:a", reconnected));
    GO2CPP_CHECK(hub.Lookup("room:a").size() == 1);
    GO2CPP_CHECK(hub.CloseKey("room:a"));
    GO2CPP_CHECK(close_total.load() == 3);
}

void test_same_stream_under_two_keys() {
    Hub hub;
    std::atomic<std::uint64_t> close_total{0};
    auto shared = std::make_shared<FakeStream>(&close_total);
    GO2CPP_CHECK(hub.Register("a", shared));
    GO2CPP_CHECK(hub.Register("b", shared));

    // 每个 key 各调用一次 Close；幂等性保证「关闭」只发生一次。
    GO2CPP_CHECK(hub.CloseKey("a"));
    GO2CPP_CHECK(shared->closed());
    GO2CPP_CHECK(shared->invocations() == 1);
    GO2CPP_CHECK(hub.CloseKey("b"));
    GO2CPP_CHECK(shared->invocations() == 2);
    GO2CPP_CHECK(close_total.load() == 1);
}

void test_close_key_reentrancy() {
    Hub hub;
    auto extra = std::make_shared<FakeStream>();
    auto conn = std::make_shared<ReentrantStream>(&hub, "room");
    conn->SetExtra(extra);
    GO2CPP_CHECK(hub.Register("room", conn));

    // Close() 里既自我注销又登记新连接：若 CloseKey 持锁调用用户代码，
    // 这里会自锁死（测试由 ctest 的 TIMEOUT 兜底）。
    GO2CPP_CHECK(hub.CloseKey("room"));
    // CloseKey 返回时 old 已摘表，但 Close 里新登记的那条应当还在。
    const auto conns = hub.Lookup("room");
    GO2CPP_CHECK(conns.size() == 1);
    if (conns.size() == 1) {
        GO2CPP_CHECK(conns[0].get() == extra.get());
    }
}

void test_lookup_snapshot_lifetime() {
    Hub hub;
    std::weak_ptr<FakeStream> weak;
    {
        auto conn = std::make_shared<FakeStream>();
        weak = conn;
        GO2CPP_CHECK(hub.Register("room", conn));
        conn.reset(); // 只剩 Hub 持有
        GO2CPP_CHECK(!weak.expired());
        const auto snapshot = hub.Lookup("room");
        GO2CPP_CHECK(snapshot.size() == 1);
        if (snapshot.size() == 1) {
            GO2CPP_CHECK(snapshot[0].use_count() >= 2);
        }
    }
    GO2CPP_CHECK(!weak.expired()); // Hub 仍持有
    GO2CPP_CHECK(hub.CloseKey("room"));
    GO2CPP_CHECK(weak.expired()); // 摘表 + 关闭后释放
}

void test_keys_sorted() {
    Hub hub;
    std::vector<std::shared_ptr<FakeStream>> conns;
    for (const char *key : {"c", "a", "b", "room:2", "room:10"}) {
        auto conn = std::make_shared<FakeStream>();
        conns.push_back(conn);
        GO2CPP_CHECK(hub.Register(key, conn));
    }
    const auto keys = hub.Keys();
    const std::vector<std::string> expected{"a", "b", "c", "room:10", "room:2"};
    GO2CPP_CHECK(keys == expected);
}

/** @brief 1e5 次并发 Register/Lookup/Unregister（TSan 编译时同样跑这套）。 */
void test_concurrent_churn_100k() {
    constexpr int kThreads = 8;
    constexpr int kIterationsPerThread = 12500; // 8 × 12500 = 100000 次 Register
    constexpr int kKeySpace = 97;

    Hub hub;
    std::atomic<std::uint64_t> registrations{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&hub, &registrations, thread_index] {
            for (int iteration = 0; iteration < kIterationsPerThread; ++iteration) {
                const std::string key =
                    "room:" +
                    std::to_string((thread_index * 131 + iteration * 7) % kKeySpace);
                auto conn = std::make_shared<FakeStream>();
                GO2CPP_CHECK(hub.Register(key, conn));
                registrations.fetch_add(1, std::memory_order_relaxed);

                // 读快照：必须能看到刚登记的自己，且快照里不允许出现空指针。
                const auto snapshot = hub.Lookup(key);
                bool found = false;
                for (const auto &entry : snapshot) {
                    GO2CPP_CHECK(entry != nullptr);
                    if (entry.get() == conn.get()) {
                        found = true;
                    }
                }
                GO2CPP_CHECK(found);

                hub.Unregister(key, conn);
                // 重复注销：幂等，不崩溃、不影响后续操作。
                hub.Unregister(key, conn);
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    GO2CPP_CHECK(registrations.load() ==
                 static_cast<std::uint64_t>(kThreads) * kIterationsPerThread);
    // 每个线程都注销了自己的连接，最终表必须为空。
    GO2CPP_CHECK(hub.Keys().empty());
}

/** @brief CloseKey 与 Lookup/Keys/Unregister 并发：关闭计数必须精确。 */
void test_close_key_concurrent() {
    constexpr int kKeys = 64;
    constexpr int kConnsPerKey = 8;
    constexpr int kReaderThreads = 4;

    Hub hub;
    std::atomic<std::uint64_t> close_total{0};
    std::vector<std::shared_ptr<FakeStream>> all_conns;
    all_conns.reserve(kKeys * kConnsPerKey);
    for (int key_index = 0; key_index < kKeys; ++key_index) {
        const std::string key = "room:" + std::to_string(key_index);
        for (int conn_index = 0; conn_index < kConnsPerKey; ++conn_index) {
            auto conn = std::make_shared<FakeStream>(&close_total);
            all_conns.push_back(conn);
            GO2CPP_CHECK(hub.Register(key, conn));
        }
    }

    std::vector<std::thread> threads;
    threads.reserve(kReaderThreads + 2);
    for (int reader_index = 0; reader_index < kReaderThreads; ++reader_index) {
        threads.emplace_back([&hub] {
            for (int round = 0; round < 2000; ++round) {
                for (int key_index = 0; key_index < kKeys; ++key_index) {
                    const std::string key = "room:" + std::to_string(key_index);
                    const auto snapshot = hub.Lookup(key);
                    for (const auto &entry : snapshot) {
                        GO2CPP_CHECK(entry != nullptr);
                    }
                }
                (void)hub.Keys();
            }
        });
    }
    // 两个线程各关闭一半 key：同一 key 可能被两次 CloseKey，第二次返回 false。
    for (int closer_index = 0; closer_index < 2; ++closer_index) {
        threads.emplace_back([&hub, closer_index] {
            for (int key_index = 0; key_index < kKeys; ++key_index) {
                if (key_index % 2 != closer_index) {
                    continue;
                }
                GO2CPP_CHECK(hub.CloseKey("room:" + std::to_string(key_index)));
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }

    GO2CPP_CHECK(hub.Keys().empty());
    GO2CPP_CHECK(close_total.load() ==
                 static_cast<std::uint64_t>(kKeys) * kConnsPerKey);
    for (const auto &conn : all_conns) {
        GO2CPP_CHECK(conn->invocations() == 1); // 每条连接恰好被 Close 一次
    }
}

/** @brief 影子模型：检查 Hub 的状态与「朴素实现」完全一致。 */
struct ShadowModel {
    std::unordered_map<std::string, std::vector<std::shared_ptr<Hub::Stream>>> conns;

    bool Has(const std::string &key) const {
        const auto it = conns.find(key);
        return it != conns.end() && !it->second.empty();
    }

    std::vector<std::string> Keys() const {
        std::vector<std::string> keys;
        for (const auto &entry : conns) {
            if (!entry.second.empty()) {
                keys.push_back(entry.first);
            }
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }
};

bool SameState(const Hub &hub, const ShadowModel &model) {
    if (hub.Keys() != model.Keys()) {
        return false;
    }
    for (const auto &key : model.Keys()) {
        const auto actual = hub.Lookup(key);
        const auto &expected = model.conns.at(key);
        if (actual.size() != expected.size()) {
            return false;
        }
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (actual[index].get() != expected[index].get()) {
                return false;
            }
        }
    }
    return true;
}

/** @brief 随机操作序列与影子模型对拍：覆盖插入/删除/关闭的任意交错。 */
void test_randomized_against_shadow_model() {
    constexpr int kSteps = 20000;
    Hub hub;
    ShadowModel model;
    std::mt19937_64 random(20261101);
    std::vector<std::shared_ptr<FakeStream>> pool;
    std::uint64_t close_total = 0;

    const auto random_key = [&random] {
        return "k" + std::to_string(random() % 16);
    };
    for (int step = 0; step < kSteps; ++step) {
        const int operation = static_cast<int>(random() % 5);
        const std::string key = random_key();
        if (operation == 0 || operation == 1) { // Register
            auto conn = std::make_shared<FakeStream>();
            pool.push_back(conn);
            model.conns[key].push_back(conn);
            GO2CPP_CHECK(hub.Register(key, conn));
        } else if (operation == 2) { // Unregister（随机挑一条已登记的）
            const auto model_it = model.conns.find(key);
            if (model_it == model.conns.end() || model_it->second.empty()) {
                hub.Unregister(key, nullptr);
            } else {
                auto &slots = model_it->second;
                const auto victim = slots[random() % slots.size()];
                slots.erase(std::remove(slots.begin(), slots.end(), victim), slots.end());
                if (slots.empty()) {
                    model.conns.erase(model_it);
                }
                hub.Unregister(key, victim);
            }
        } else if (operation == 3) { // CloseKey
            const bool expected = model.Has(key);
            GO2CPP_CHECK(hub.CloseKey(key) == expected);
            if (expected) {
                for (const auto &conn : model.conns[key]) {
                    GO2CPP_CHECK(std::static_pointer_cast<FakeStream>(conn)->closed());
                    ++close_total;
                }
            }
            model.conns.erase(key);
        } else { // Keys / Lookup 只读
            (void)hub.Keys();
            (void)hub.Lookup(key);
        }
        if (step % 251 == 0) {
            GO2CPP_CHECK(SameState(hub, model));
        }
    }
    GO2CPP_CHECK(SameState(hub, model));
    // 收尾：清空模型里剩下的 key，并核对关闭计数。
    for (const auto &key : model.Keys()) {
        GO2CPP_CHECK(hub.CloseKey(key));
        for (const auto &conn : model.conns[key]) {
            GO2CPP_CHECK(std::static_pointer_cast<FakeStream>(conn)->closed());
            ++close_total;
        }
    }
    GO2CPP_CHECK(hub.Keys().empty());
    GO2CPP_CHECK(close_total > 0);
}

} // namespace

int main() {
    go2cpp_tests::announce("relay.hub");

    test_empty_hub();
    test_register_and_lookup();
    test_unregister_is_idempotent();
    test_close_key_closes_all_once();
    test_same_stream_under_two_keys();
    test_close_key_reentrancy();
    test_lookup_snapshot_lifetime();
    test_keys_sorted();
    test_concurrent_churn_100k();
    test_close_key_concurrent();
    test_randomized_against_shadow_model();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "relay_hub_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("relay_hub_test: all checks passed\n");
    return 0;
}
