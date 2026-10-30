/**
 * @file test_token_pairing.cpp
 * @brief TokenPairing 的功能、边界、过期、容量、并发、随机与压测用例。
 * @details 用例覆盖的真实场景：
 *
 *          1. relay 为同一房间 key 的 8 个客户端签发 token（1:N）；
 *          2. 客户端用 (key, token) 认领管道，错误 key 组合被拒；
 *          3. token 冲突（重复 Pair）不破坏已有槽位；
 *          4. 容量打满时显式拒绝，释放后恢复；
 *          5. TTL 到期后 Find 失败、PurgeExpired / 容量压力回收；
 *          6. 多客户端并发签发、认领、消费；
 *          7. 随机操作序列与影子模型比对；
 *          8. 10 万级串行与多线程压测，给出吞吐指标。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/token_pairing.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::PeerId;
using go2cpp::bento::Slot;
using go2cpp::bento::TokenPairing;
using go2cpp::bento::TokenPairingConfig;

/// 直接睡眠；过期用例用 1ms TTL + 5ms 睡眠，避免依赖时钟注入接口。
void SleepMs(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

/// 生成合法的 16 位小写 hex token（测试用，不消耗熵源）。
std::string MakeToken(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

std::string MakeKey(std::size_t index) {
    return "room-" + std::to_string(index);
}

double RatePerSecond(std::size_t count, double elapsed_ms) {
    return elapsed_ms > 0.0 ? static_cast<double>(count) / (elapsed_ms / 1000.0) : 0.0;
}

/// 顺序执行一个用例；失败数在用例前后对比，便于定位是哪个用例挂了。
void RunCase(const char* name, void (*function)()) {
    const int failures_before = bento_test::g_failures;
    std::printf("[CASE] %s\n", name);
    function();
    if (bento_test::g_failures == failures_before) {
        std::printf("[OK]   %s\n", name);
    }
}

void TestTokenFormat() {
    std::unordered_set<std::string> seen;
    for (int index = 0; index < 256; ++index) {
        const std::string token = TokenPairing::GenerateToken();
        BENTO_CHECK_EQ(token.size(), go2cpp::bento::kTokenHexChars);
        BENTO_CHECK(TokenPairing::IsValidToken(token));
        seen.insert(token);
    }
    // 256 个 64 bit 随机值的碰撞概率约 1.8e-15，唯一性断言不会 flaky。
    BENTO_CHECK_EQ(seen.size(), 256u);

    BENTO_CHECK(TokenPairing::IsValidToken("0123456789abcdef"));
    BENTO_CHECK(!TokenPairing::IsValidToken(""));
    BENTO_CHECK(!TokenPairing::IsValidToken("0123456789abcde"));
    BENTO_CHECK(!TokenPairing::IsValidToken("0123456789abcdef0"));
    BENTO_CHECK(!TokenPairing::IsValidToken("0123456789ABCDEF"));
    BENTO_CHECK(!TokenPairing::IsValidToken("0123456789abcdeg"));
    BENTO_CHECK(!TokenPairing::IsValidToken("0123456789abcde "));
    BENTO_CHECK(!TokenPairing::IsValidToken("0123456789abcde\n"));

    BENTO_CHECK(TokenPairing::IsValidKey("room-1"));
    BENTO_CHECK(!TokenPairing::IsValidKey(""));
    BENTO_CHECK(TokenPairing::IsValidKey(std::string(go2cpp::bento::kMaxKeyBytes, 'k')));
    BENTO_CHECK(!TokenPairing::IsValidKey(std::string(go2cpp::bento::kMaxKeyBytes + 1, 'k')));
}

void TestPairAndFind() {
    TokenPairing table;
    const std::string key = "room-alpha";
    const std::string token = TokenPairing::GenerateToken();
    const auto created = table.Pair(key, token, 42);
    BENTO_CHECK(created.has_value());
    if (!created) {
        return;
    }
    BENTO_CHECK_EQ(created->key, key);
    BENTO_CHECK_EQ(created->token, token);
    BENTO_CHECK_EQ(created->peer_id, 42u);
    BENTO_CHECK(created->created_ms > 0);
    BENTO_CHECK(created->expires_ms > created->created_ms);
    BENTO_CHECK(!created->IsExpired(created->created_ms));

    const auto found = table.Find(key, token);
    BENTO_CHECK(found.has_value());
    if (found) {
        BENTO_CHECK_EQ(found->peer_id, 42u);
        BENTO_CHECK_EQ(found->token, token);
    }
    BENTO_CHECK(!table.Find("room-beta", token).has_value());
    BENTO_CHECK(!table.Find(key, TokenPairing::GenerateToken()).has_value());
    BENTO_CHECK_EQ(table.size(), 1u);
    BENTO_CHECK_EQ(table.capacity(), 4096u);
    BENTO_CHECK_EQ(table.ttl_ms(), 300000u);
}

void TestOneToMany() {
    TokenPairing table;
    constexpr std::size_t kPeers = 8;
    const std::string key = "room-1";
    std::vector<Slot> expected;
    expected.reserve(kPeers);
    for (std::size_t index = 0; index < kPeers; ++index) {
        const auto slot = table.Issue(key, 100 + index);
        BENTO_CHECK(slot.has_value());
        if (slot) {
            expected.push_back(*slot);
        }
    }
    BENTO_CHECK_EQ(table.size(), kPeers);

    const auto listed = table.List(key);
    BENTO_CHECK_EQ(listed.size(), kPeers);
    std::unordered_set<PeerId> peers;
    std::unordered_set<std::string> tokens;
    for (const Slot& slot : listed) {
        BENTO_CHECK_EQ(slot.key, key);
        BENTO_CHECK(TokenPairing::IsValidToken(slot.token));
        peers.insert(slot.peer_id);
        tokens.insert(slot.token);
    }
    BENTO_CHECK_EQ(peers.size(), kPeers);
    BENTO_CHECK_EQ(tokens.size(), kPeers);

    // List 的排序契约：created_ms 升序，同毫秒按 token 升序。
    for (std::size_t index = 1; index < listed.size(); ++index) {
        const Slot& previous = listed[index - 1];
        const Slot& current = listed[index];
        if (previous.created_ms == current.created_ms) {
            BENTO_CHECK(previous.token < current.token);
        } else {
            BENTO_CHECK(previous.created_ms < current.created_ms);
        }
    }

    for (const Slot& slot : expected) {
        const auto found = table.Find(key, slot.token);
        BENTO_CHECK(found.has_value());
        if (found) {
            BENTO_CHECK_EQ(found->peer_id, slot.peer_id);
        }
    }

    // 1:N 的隔离性：key 之间互不可见，token 不能跨 key 使用。
    BENTO_CHECK_EQ(table.List("room-2").size(), 0u);
    if (!expected.empty()) {
        BENTO_CHECK(!table.Find("room-2", expected.front().token).has_value());
    }
}

void TestDuplicateTokenRejected() {
    TokenPairing table;
    const std::string token = TokenPairing::GenerateToken();
    const auto first = table.Pair("room-a", token, 1);
    BENTO_CHECK(first.has_value());
    if (!first) {
        return;
    }

    // 同 token 跨 key、同 key 重放都必须拒绝。
    BENTO_CHECK(!table.Pair("room-b", token, 2).has_value());
    BENTO_CHECK(!table.Pair("room-a", token, 3).has_value());
    BENTO_CHECK_EQ(table.size(), 1u);

    const auto found = table.Find("room-a", token);
    BENTO_CHECK(found.has_value());
    if (found) {
        BENTO_CHECK_EQ(found->peer_id, 1u);
    }
    BENTO_CHECK(!table.Find("room-b", token).has_value());

    // 释放后同一 token 可以重新配对（新会话复用文本，熵不变）。
    BENTO_CHECK(table.Release(token));
    BENTO_CHECK(!table.Release(token));
    const auto reused = table.Pair("room-b", token, 4);
    BENTO_CHECK(reused.has_value());
    if (reused) {
        BENTO_CHECK_EQ(reused->peer_id, 4u);
        BENTO_CHECK_EQ(table.List("room-b").size(), 1u);
    }
}

void TestInvalidInputs() {
    TokenPairing table;
    const std::string valid_token = TokenPairing::GenerateToken();
    const std::string oversize_key(go2cpp::bento::kMaxKeyBytes + 1, 'k');

    BENTO_CHECK(!table.Pair("", valid_token, 1).has_value());
    BENTO_CHECK(!table.Pair(oversize_key, valid_token, 1).has_value());
    BENTO_CHECK(!table.Pair("room", "", 1).has_value());
    BENTO_CHECK(!table.Pair("room", "0123456789ABCDEF", 1).has_value());
    BENTO_CHECK(!table.Pair("room", "0123456789abcde", 1).has_value());
    BENTO_CHECK(!table.Pair("room", "0123456789abcdefg", 1).has_value());
    BENTO_CHECK(!table.Issue("", 1).has_value());
    BENTO_CHECK(!table.Issue(oversize_key, 1).has_value());

    BENTO_CHECK(!table.Find("", valid_token).has_value());
    BENTO_CHECK(!table.Find(oversize_key, valid_token).has_value());
    BENTO_CHECK(!table.Find("room", "bad").has_value());
    BENTO_CHECK(!table.Release("bad"));
    BENTO_CHECK(!table.Release(""));
    BENTO_CHECK(table.List("").empty());
    BENTO_CHECK(table.List(oversize_key).empty());
    BENTO_CHECK_EQ(table.size(), 0u);
}

void TestRelease() {
    TokenPairing table;
    const auto slot = table.Issue("room", 7);
    BENTO_CHECK(slot.has_value());
    if (!slot) {
        return;
    }
    BENTO_CHECK(table.Find("room", slot->token).has_value());
    BENTO_CHECK(table.Release(slot->token));
    BENTO_CHECK(!table.Find("room", slot->token).has_value());
    BENTO_CHECK(table.List("room").empty());
    BENTO_CHECK_EQ(table.size(), 0u);
    BENTO_CHECK(!table.Release(slot->token));
    BENTO_CHECK_EQ(table.purged(), 0u);
}

void TestCapacity() {
    TokenPairingConfig config;
    config.max_slots = 4;
    config.ttl_ms = 0;
    TokenPairing table(config);
    BENTO_CHECK_EQ(table.capacity(), 4u);

    std::vector<std::string> tokens;
    tokens.reserve(4);
    for (std::size_t index = 0; index < 4; ++index) {
        const auto slot = table.Issue("room", index);
        BENTO_CHECK(slot.has_value());
        if (slot) {
            tokens.push_back(slot->token);
        }
    }
    BENTO_CHECK_EQ(table.size(), 4u);
    BENTO_CHECK(!table.Issue("room", 99).has_value());
    BENTO_CHECK_EQ(table.size(), 4u);

    // 释放一个槽位后容量恢复；这不依赖过期清理。
    BENTO_CHECK(table.Release(tokens.front()));
    const auto accepted = table.Issue("room", 100);
    BENTO_CHECK(accepted.has_value());
    BENTO_CHECK_EQ(table.size(), 4u);
    BENTO_CHECK_EQ(table.purged(), 0u);

    TokenPairingConfig zero_config;
    zero_config.max_slots = 0;
    TokenPairing zero(zero_config);
    BENTO_CHECK(!zero.Issue("room", 1).has_value());
    BENTO_CHECK(!zero.Pair("room", TokenPairing::GenerateToken(), 1).has_value());
    BENTO_CHECK_EQ(zero.size(), 0u);
}

void TestExpiry() {
    TokenPairingConfig config;
    config.ttl_ms = 1;
    TokenPairing table(config);
    BENTO_CHECK_EQ(table.ttl_ms(), 1u);

    const auto slot = table.Issue("room", 1);
    BENTO_CHECK(slot.has_value());
    if (!slot) {
        return;
    }
    SleepMs(5);
    BENTO_CHECK(!table.Find("room", slot->token).has_value());
    BENTO_CHECK(table.List("room").empty());
    // 惰性语义：过期但未回收的槽仍计入 size，容量上限因此不会被突破。
    BENTO_CHECK_EQ(table.size(), 1u);
    BENTO_CHECK_EQ(table.PurgeExpired(), 1u);
    BENTO_CHECK_EQ(table.size(), 0u);
    BENTO_CHECK_EQ(table.purged(), 1u);
    BENTO_CHECK_EQ(table.PurgeExpired(), 0u);

    // ttl_ms = 0 表示永不过期。
    TokenPairingConfig persistent_config;
    persistent_config.ttl_ms = 0;
    TokenPairing persistent(persistent_config);
    const auto permanent = persistent.Issue("room", 2);
    BENTO_CHECK(permanent.has_value());
    if (permanent) {
        BENTO_CHECK_EQ(permanent->expires_ms, 0u);
        SleepMs(2);
        BENTO_CHECK(persistent.Find("room", permanent->token).has_value());
    }

    // Release 一条已过期的 token：返回 false，但槽位被回收并计数。
    TokenPairingConfig short_config;
    short_config.ttl_ms = 1;
    TokenPairing expiring(short_config);
    const auto doomed = expiring.Issue("room", 3);
    BENTO_CHECK(doomed.has_value());
    if (doomed) {
        SleepMs(5);
        BENTO_CHECK(!expiring.Release(doomed->token));
        BENTO_CHECK_EQ(expiring.size(), 0u);
        BENTO_CHECK_EQ(expiring.purged(), 1u);
    }
}

void TestCapacityPressurePurgesExpired() {
    TokenPairingConfig config;
    config.max_slots = 2;
    config.ttl_ms = 1;
    TokenPairing table(config);

    const auto first = table.Issue("room", 1);
    const auto second = table.Issue("room", 2);
    BENTO_CHECK(first.has_value());
    BENTO_CHECK(second.has_value());
    if (!first || !second) {
        return;
    }
    SleepMs(5);

    // 容量已满，但 Pair 会先回收过期槽，因此第三个 token 仍能签发。
    const auto third = table.Issue("room", 3);
    BENTO_CHECK(third.has_value());
    BENTO_CHECK_EQ(table.size(), 1u);
    BENTO_CHECK(table.purged() >= 2u);
    BENTO_CHECK(!table.Find("room", first->token).has_value());
    BENTO_CHECK(!table.Find("room", second->token).has_value());
    if (third) {
        BENTO_CHECK(table.Find("room", third->token).has_value());
    }
}

struct IssuedToken {
    std::string token;
    PeerId peer = 0;
};

void TestConcurrentPairFindRelease() {
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kPerThread = 200;
    TokenPairing table;
    std::vector<std::vector<IssuedToken>> issued(kThreads);
    std::atomic<int> failures{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&table, &issued, &failures, thread_index]() {
            issued[thread_index].reserve(kPerThread);
            const std::string key = MakeKey(thread_index);
            for (std::size_t index = 0; index < kPerThread; ++index) {
                const PeerId peer = static_cast<PeerId>(thread_index * 10000 + index);
                const auto slot = table.Issue(key, peer);
                if (!slot) {
                    ++failures;
                    continue;
                }
                issued[thread_index].push_back(IssuedToken{slot->token, peer});
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    BENTO_CHECK_EQ(failures.load(), 0);
    BENTO_CHECK_EQ(table.size(), kThreads * kPerThread);

    std::unordered_set<std::string> unique_tokens;
    for (const std::vector<IssuedToken>& per_thread : issued) {
        for (const IssuedToken& item : per_thread) {
            unique_tokens.insert(item.token);
        }
    }
    BENTO_CHECK_EQ(unique_tokens.size(), kThreads * kPerThread);

    // 并发认领：每个 token 只能被自己的 key 找到，peer 必须原样返回。
    std::atomic<int> find_failures{0};
    std::vector<std::thread> readers;
    readers.reserve(kThreads);
    for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
        readers.emplace_back([&table, &issued, &find_failures, thread_index]() {
            const std::string key = MakeKey(thread_index);
            for (const IssuedToken& item : issued[thread_index]) {
                const auto slot = table.Find(key, item.token);
                if (!slot || slot->peer_id != item.peer) {
                    ++find_failures;
                }
            }
        });
    }
    for (std::thread& reader : readers) {
        reader.join();
    }
    BENTO_CHECK_EQ(find_failures.load(), 0);

    // 并发消费：每个 token 恰好成功释放一次。
    std::atomic<int> release_failures{0};
    std::atomic<int> released{0};
    std::vector<std::thread> replayers;
    replayers.reserve(kThreads);
    for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
        replayers.emplace_back([&table, &issued, &release_failures, &released, thread_index]() {
            for (const IssuedToken& item : issued[thread_index]) {
                if (table.Release(item.token)) {
                    ++released;
                } else {
                    ++release_failures;
                }
            }
        });
    }
    for (std::thread& replayer : replayers) {
        replayer.join();
    }
    BENTO_CHECK_EQ(release_failures.load(), 0);
    BENTO_CHECK_EQ(released.load(), static_cast<int>(kThreads * kPerThread));
    BENTO_CHECK_EQ(table.size(), 0u);
    for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
        BENTO_CHECK(table.List(MakeKey(thread_index)).empty());
    }
}

void TestConcurrentSameKey() {
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kPerThread = 100;
    constexpr std::size_t kTotal = kThreads * kPerThread;
    const std::string key = "shared-room";
    TokenPairing table;
    std::vector<std::vector<std::string>> tokens(kThreads);
    std::atomic<int> failures{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&table, &tokens, &failures, &key, thread_index]() {
            tokens[thread_index].reserve(kPerThread);
            for (std::size_t index = 0; index < kPerThread; ++index) {
                const auto slot =
                    table.Issue(key, static_cast<PeerId>(thread_index * 1000 + index));
                if (!slot) {
                    ++failures;
                    continue;
                }
                tokens[thread_index].push_back(slot->token);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    BENTO_CHECK_EQ(failures.load(), 0);
    BENTO_CHECK_EQ(table.size(), kTotal);
    BENTO_CHECK_EQ(table.List(key).size(), kTotal);

    // 读线程持续取快照，同时 8 个线程消费；快照必须是完整的槽位值。
    std::atomic<bool> stop{false};
    std::atomic<int> bad_snapshots{0};
    std::thread reader([&table, &stop, &bad_snapshots, &key]() {
        while (!stop.load()) {
            const std::vector<Slot> snapshot = table.List(key);
            if (snapshot.size() > kTotal) {
                ++bad_snapshots;
            }
            for (const Slot& slot : snapshot) {
                if (slot.key != key || !TokenPairing::IsValidToken(slot.token)) {
                    ++bad_snapshots;
                }
            }
            std::this_thread::yield();
        }
    });

    std::atomic<int> released{0};
    std::atomic<int> release_failures{0};
    std::vector<std::thread> replayers;
    replayers.reserve(kThreads);
    for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
        replayers.emplace_back([&table, &tokens, &released, &release_failures, thread_index]() {
            for (const std::string& token : tokens[thread_index]) {
                if (table.Release(token)) {
                    ++released;
                } else {
                    ++release_failures;
                }
            }
        });
    }
    for (std::thread& replayer : replayers) {
        replayer.join();
    }
    stop.store(true);
    reader.join();

    BENTO_CHECK_EQ(bad_snapshots.load(), 0);
    BENTO_CHECK_EQ(release_failures.load(), 0);
    BENTO_CHECK_EQ(released.load(), static_cast<int>(kTotal));
    BENTO_CHECK_EQ(table.size(), 0u);
    BENTO_CHECK(table.List(key).empty());
}

void TestRandomizedAgainstShadow() {
    constexpr std::size_t kKeyCount = 8;
    constexpr std::size_t kTokenPool = 512;
    constexpr std::size_t kMaxSlots = 320;
    constexpr int kSteps = 4000;

    TokenPairingConfig config;
    config.max_slots = kMaxSlots;
    config.ttl_ms = 0;
    TokenPairing table(config);

    std::array<std::string, kKeyCount> keys;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        keys[index] = MakeKey(index);
    }
    std::vector<std::string> token_pool;
    token_pool.reserve(kTokenPool);
    for (std::size_t index = 0; index < kTokenPool; ++index) {
        token_pool.push_back(MakeToken(index + 1));
    }

    std::mt19937 random(20261030U);
    std::unordered_map<std::string, Slot> shadow_by_token;
    std::unordered_map<std::string, std::unordered_set<std::string>> shadow_by_key;

    for (int step = 0; step < kSteps; ++step) {
        const unsigned int operation = random() % 100U;
        const std::string& key = keys[random() % kKeyCount];
        const std::string& token = token_pool[random() % kTokenPool];

        if (operation < 45U) {
            const PeerId peer = static_cast<PeerId>(random());
            const bool token_taken = shadow_by_token.find(token) != shadow_by_token.end();
            const bool expect_ok = !token_taken && shadow_by_token.size() < kMaxSlots;
            const auto slot = table.Pair(key, token, peer);
            BENTO_CHECK_EQ(slot.has_value(), expect_ok);
            if (slot) {
                shadow_by_token.emplace(token, *slot);
                shadow_by_key[key].insert(token);
            }
        } else if (operation < 70U) {
            const auto slot = table.Find(key, token);
            const auto shadow = shadow_by_token.find(token);
            const bool expect_found = shadow != shadow_by_token.end() && shadow->second.key == key;
            BENTO_CHECK_EQ(slot.has_value(), expect_found);
            if (slot && expect_found) {
                BENTO_CHECK_EQ(slot->peer_id, shadow->second.peer_id);
            }
        } else if (operation < 95U) {
            const auto shadow = shadow_by_token.find(token);
            const bool expect_released = shadow != shadow_by_token.end();
            BENTO_CHECK_EQ(table.Release(token), expect_released);
            if (shadow != shadow_by_token.end()) {
                const std::string owner = shadow->second.key;
                shadow_by_token.erase(shadow);
                const auto key_position = shadow_by_key.find(owner);
                if (key_position != shadow_by_key.end()) {
                    key_position->second.erase(token);
                    if (key_position->second.empty()) {
                        shadow_by_key.erase(key_position);
                    }
                }
            }
        } else {
            const std::vector<Slot> listed = table.List(key);
            const auto shadow = shadow_by_key.find(key);
            const std::size_t expected_size =
                shadow == shadow_by_key.end() ? 0U : shadow->second.size();
            BENTO_CHECK_EQ(listed.size(), expected_size);
            for (const Slot& slot : listed) {
                BENTO_CHECK_EQ(slot.key, key);
                const auto item = shadow_by_token.find(slot.token);
                BENTO_CHECK(item != shadow_by_token.end());
                if (item != shadow_by_token.end()) {
                    BENTO_CHECK_EQ(slot.peer_id, item->second.peer_id);
                }
            }
        }
        if (step % 97 == 0) {
            BENTO_CHECK_EQ(table.size(), shadow_by_token.size());
        }
    }

    BENTO_CHECK_EQ(table.size(), shadow_by_token.size());
    for (const auto& entry : shadow_by_token) {
        const auto found = table.Find(entry.second.key, entry.first);
        BENTO_CHECK(found.has_value());
        if (found) {
            BENTO_CHECK_EQ(found->peer_id, entry.second.peer_id);
        }
    }
    // 影子中不存在的 token 在任意 key 下都不应命中。
    for (const std::string& token : token_pool) {
        if (shadow_by_token.find(token) != shadow_by_token.end()) {
            continue;
        }
        for (const std::string& key : keys) {
            BENTO_CHECK(!table.Find(key, token).has_value());
        }
    }
}

void TestThroughput() {
    constexpr std::size_t kOperations = 100000;
    constexpr std::size_t kKeys = 64;
    TokenPairingConfig config;
    config.max_slots = kOperations + 1;
    config.ttl_ms = 0;
    TokenPairing table(config);

    std::array<std::string, kKeys> keys;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        keys[index] = MakeKey(index);
    }
    std::vector<std::string> tokens;
    tokens.reserve(kOperations);
    for (std::size_t index = 0; index < kOperations; ++index) {
        tokens.push_back(MakeToken(index + 1));
    }

    const auto pair_start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < kOperations; ++index) {
        const auto slot =
            table.Pair(keys[index % kKeys], tokens[index], static_cast<PeerId>(index));
        BENTO_CHECK(slot.has_value());
    }
    const auto pair_end = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < kOperations; ++index) {
        const auto slot = table.Find(keys[index % kKeys], tokens[index]);
        BENTO_CHECK(slot.has_value());
    }
    const auto find_end = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < kOperations; ++index) {
        BENTO_CHECK(table.Release(tokens[index]));
    }
    const auto release_end = std::chrono::steady_clock::now();

    const double pair_ms = std::chrono::duration<double, std::milli>(pair_end - pair_start).count();
    const double find_ms = std::chrono::duration<double, std::milli>(find_end - pair_end).count();
    const double release_ms =
        std::chrono::duration<double, std::milli>(release_end - find_end).count();
    const double total_ms = pair_ms + find_ms + release_ms;
    const double operations_per_second =
        total_ms > 0.0 ? static_cast<double>(3 * kOperations) / (total_ms / 1000.0) : 0.0;
    std::printf(
        "[PERF] serial: pair %.1f ms, find %.1f ms, release %.1f ms, total %.1f ms "
        "(%.0f ops/s)\n",
        pair_ms, find_ms, release_ms, total_ms, operations_per_second);
    BENTO_CHECK(total_ms < 10000.0);
    BENTO_CHECK_EQ(table.size(), 0u);

    constexpr std::size_t kTokenCount = 20000;
    std::unordered_set<std::string> generated;
    generated.reserve(kTokenCount);
    const auto token_start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < kTokenCount; ++index) {
        const std::string token = TokenPairing::GenerateToken();
        BENTO_CHECK(TokenPairing::IsValidToken(token));
        generated.insert(token);
    }
    const auto token_end = std::chrono::steady_clock::now();
    const double token_ms =
        std::chrono::duration<double, std::milli>(token_end - token_start).count();
    std::printf("[PERF] GenerateToken: %zu in %.1f ms (%.0f/s, unique=%zu)\n", kTokenCount,
                token_ms, RatePerSecond(kTokenCount, token_ms), generated.size());
    // 允许至多 1 个理论碰撞（2 万次 64 bit 随机值碰撞概率约 1.1e-11）。
    BENTO_CHECK(generated.size() + 1 >= kTokenCount);

    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kPerThread = 5000;
    TokenPairingConfig concurrent_config;
    concurrent_config.max_slots = kThreads * kPerThread + 1;
    concurrent_config.ttl_ms = 0;
    TokenPairing concurrent(concurrent_config);
    std::atomic<int> failures{0};
    const auto concurrent_start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&concurrent, &failures, thread_index]() {
            const std::string key = MakeKey(thread_index);
            for (std::size_t index = 0; index < kPerThread; ++index) {
                const auto slot =
                    concurrent.Issue(key, static_cast<PeerId>(thread_index * 100000 + index));
                if (!slot) {
                    ++failures;
                    continue;
                }
                if (!concurrent.Find(key, slot->token).has_value()) {
                    ++failures;
                }
                if (!concurrent.Release(slot->token)) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const auto concurrent_end = std::chrono::steady_clock::now();
    const double concurrent_ms =
        std::chrono::duration<double, std::milli>(concurrent_end - concurrent_start).count();
    const double concurrent_ops =
        3.0 * static_cast<double>(kThreads * kPerThread) / (concurrent_ms / 1000.0);
    std::printf("[PERF] concurrent: %zu threads x %zu issue+find+release in %.1f ms (%.0f ops/s)\n",
                kThreads, kPerThread, concurrent_ms, concurrent_ops);
    BENTO_CHECK_EQ(failures.load(), 0);
    BENTO_CHECK_EQ(concurrent.size(), 0u);
    BENTO_CHECK(concurrent_ms < 20000.0);
}

}  // namespace

int main() {
    std::printf("== TokenPairing tests ==\n");
    RunCase("token_format", TestTokenFormat);
    RunCase("pair_and_find", TestPairAndFind);
    RunCase("one_to_many", TestOneToMany);
    RunCase("duplicate_token_rejected", TestDuplicateTokenRejected);
    RunCase("invalid_inputs", TestInvalidInputs);
    RunCase("release", TestRelease);
    RunCase("capacity", TestCapacity);
    RunCase("expiry", TestExpiry);
    RunCase("capacity_pressure_purges_expired", TestCapacityPressurePurgesExpired);
    RunCase("concurrent_pair_find_release", TestConcurrentPairFindRelease);
    RunCase("concurrent_same_key", TestConcurrentSameKey);
    RunCase("randomized_against_shadow", TestRandomizedAgainstShadow);
    RunCase("throughput", TestThroughput);
    return bento_test::Finish("token_pairing");
}
