/**
 * @file test_node_id.cpp
 * @brief `NodeId`：1e6 次生成无碰撞、并发唯一、Parse 往返与非法输入拒绝。
 *
 * 断言重点是「唯一性」与「严格解析」：
 * - 1e6 次单线程生成不能出现重复（低半区靠原子序号，高半区靠随机混合）；
 * - 8 线程并发生成的结果合并后仍然唯一（不是只测单线程）；
 * - `Parse` 必须拒绝空串、长度错误、非十六进制字符，不能静默接受。
 */

#include "go2cpp/distributed/node_id.hpp"

#include "distributed_test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using go2cpp::distributed::NodeId;

/// @brief 128 位键的哈希（FNV-1a 混合两个半区），用于 1e6 去重。
struct NodeIdKey {
    std::uint64_t high{0};
    std::uint64_t low{0};

    bool operator==(const NodeIdKey& other) const noexcept {
        return high == other.high && low == other.low;
    }
};

struct NodeIdKeyHash {
    std::size_t operator()(const NodeIdKey& key) const noexcept {
        std::uint64_t mixed = key.high ^ (key.low + 0x9E3779B97F4A7C15ULL +
                                          (key.high << 6) + (key.high >> 2));
        return static_cast<std::size_t>(mixed);
    }
};

NodeIdKey KeyOf(const NodeId& id) { return NodeIdKey{id.high(), id.low()}; }

void TestGenerateMillionUnique() {
    go2cpp_tests::announce("node_id.unique_1e6");
    std::unordered_set<NodeIdKey, NodeIdKeyHash> seen;
    seen.reserve(1000000 + 1);
    std::uint64_t duplicates = 0;
    for (int index = 0; index < 1000000; ++index) {
        const NodeId id = NodeId::Generate();
        if (!seen.insert(KeyOf(id)).second) {
            ++duplicates;
        }
    }
    GO2CPP_CHECK(duplicates == 0);
    GO2CPP_CHECK(seen.size() == 1000000);
    std::printf("  generated 1e6 node ids, unique=%zu, duplicates=%llu\n", seen.size(),
                static_cast<unsigned long long>(duplicates));
}

void TestConcurrentUnique() {
    go2cpp_tests::announce("node_id.concurrent");
    constexpr int kThreads = 8;
    constexpr int kPerThread = 20000;
    std::vector<std::vector<NodeIdKey>> per_thread(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([thread_index, &per_thread]() {
            auto& out = per_thread[static_cast<std::size_t>(thread_index)];
            out.reserve(kPerThread);
            for (int index = 0; index < kPerThread; ++index) {
                out.push_back(KeyOf(NodeId::Generate()));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    std::unordered_set<NodeIdKey, NodeIdKeyHash> seen;
    seen.reserve(static_cast<std::size_t>(kThreads) * kPerThread);
    for (const auto& chunk : per_thread) {
        seen.insert(chunk.begin(), chunk.end());
    }
    GO2CPP_CHECK(seen.size() == static_cast<std::size_t>(kThreads) * kPerThread);
    std::printf("  %d threads x %d ids, unique=%zu\n", kThreads, kPerThread, seen.size());
}

void TestParseRoundTrip() {
    go2cpp_tests::announce("node_id.parse_roundtrip");
    for (int index = 0; index < 1000; ++index) {
        const NodeId id = NodeId::Generate();
        const std::string text = id.ToString();
        GO2CPP_CHECK(text.size() == NodeId::kTextLength);
        const NodeId parsed = NodeId::Parse(text);
        GO2CPP_CHECK(parsed.IsValid());
        GO2CPP_CHECK(parsed == id);
        GO2CPP_CHECK(parsed.ToString() == text);
    }
    // 大写十六进制也必须接受。
    const NodeId id = NodeId::Generate();
    std::string upper = id.ToString();
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    GO2CPP_CHECK(NodeId::Parse(upper) == id);
    GO2CPP_CHECK(NodeId::FromParts(0x0123456789abcdefULL, 0xfedcba9876543210ULL).ToString() ==
                 "0123456789abcdeffedcba9876543210");
}

void TestInvalidInputRejected() {
    go2cpp_tests::announce("node_id.invalid");
    const std::string valid = NodeId::Generate().ToString();
    GO2CPP_CHECK(!NodeId::Parse("").IsValid());
    GO2CPP_CHECK(!NodeId::Parse(std::string(valid.begin(), valid.end() - 1)).IsValid());
    GO2CPP_CHECK(!NodeId::Parse(valid + "0").IsValid());
    GO2CPP_CHECK(!NodeId::Parse(std::string(64, 'a')).IsValid());
    GO2CPP_CHECK(!NodeId::Parse("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz").IsValid());
    GO2CPP_CHECK(!NodeId::Parse("0123456789abcdeffedcba987654321g").IsValid());
    GO2CPP_CHECK(!NodeId::Parse("0123456789abcdeffedcba98765432 0").IsValid());
    NodeId untouched = NodeId::Generate();
    const NodeId before = untouched;
    GO2CPP_CHECK(!NodeId::TryParse("bad", &untouched));
    GO2CPP_CHECK(untouched == before);
    GO2CPP_CHECK(!NodeId::TryParse(valid, nullptr));
    // 全 0 是合法解析结果（表示未设置），ToString 输出 32 个 0。
    const NodeId zero = NodeId::Parse(std::string(32, '0'));
    GO2CPP_CHECK(!zero.IsValid());
    GO2CPP_CHECK(zero.ToString() == std::string(32, '0'));
}

void TestOrdering() {
    go2cpp_tests::announce("node_id.ordering");
    const NodeId a = NodeId::FromParts(1, 2);
    const NodeId b = NodeId::FromParts(1, 3);
    const NodeId c = NodeId::FromParts(2, 0);
    GO2CPP_CHECK(a < b);
    GO2CPP_CHECK(b < c);
    GO2CPP_CHECK(a != b);
    GO2CPP_CHECK(!(a == b));
    GO2CPP_CHECK(NodeId{} == NodeId::FromParts(0, 0));
}

} // namespace

int main() {
    go2cpp_tests::announce("distributed.node_id");
    TestGenerateMillionUnique();
    TestConcurrentUnique();
    TestParseRoundTrip();
    TestInvalidInputRejected();
    TestOrdering();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_node_id_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("distributed_node_id_test: all cases passed\n");
    return 0;
}
