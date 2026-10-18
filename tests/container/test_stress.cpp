/**
 * @file test_stress.cpp
 * @brief 压测：1,000,000 次规模，量化耗时 / 吞吐 / 节点内存，并与标准库基线对比。
 *
 * ## 指标与条件
 * - 环境：Release（-O2），单线程，数据规模 1,000,000，预热不计入；
 * - 每个实现跑 3 轮，报告中位数，消除单次抖动；
 * - Heap：1M push + 1M pop，对比 `std::priority_queue`（min-heap，同样预分配）；
 * - List：1M push_back + 全量遍历 + Clear，对比 `std::list`；
 * - Ring：1M 节点建环 + 1M 次 Next 遍历，对比手写裸指针环（同一指针追逐模式）。
 *
 * 压测同时校验校验和，防止优化器把整个循环删掉，也证明两边语义一致。
 */
#include "container_test_support.hpp"

#include "go2cpp/container/container.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <list>
#include <queue>
#include <random>
#include <utility>
#include <vector>

namespace container = go2cpp::container;

namespace {

constexpr std::size_t kCount = 1000000;
constexpr int kRounds = 3;

using Clock = go2cpp_container_test::Clock;

double Median(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

double MopsPerSecond(std::size_t operations, double milliseconds) {
    if (milliseconds <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(operations) / milliseconds / 1000.0;
}

// ===========================================================================
// Heap：1M push + 1M pop
// ===========================================================================
struct HeapResult {
    double push_ms = 0.0;
    double pop_ms = 0.0;
    long long checksum = 0;
};

HeapResult RunOurHeap(const std::vector<int>& values) {
    HeapResult result;
    container::Heap<int> heap;
    heap.Reserve(values.size());

    auto start = Clock::now();
    for (int value : values) {
        heap.Push(value);
    }
    result.push_ms = go2cpp_container_test::MsSince(start);

    start = Clock::now();
    while (!heap.Empty()) {
        result.checksum += heap.Pop();
    }
    result.pop_ms = go2cpp_container_test::MsSince(start);
    return result;
}

HeapResult RunStdHeap(const std::vector<int>& values) {
    HeapResult result;
    std::vector<int> storage;
    storage.reserve(values.size());
    std::priority_queue<int, std::vector<int>, std::greater<int>> heap(
        std::greater<int>(), std::move(storage));

    auto start = Clock::now();
    for (int value : values) {
        heap.push(value);
    }
    result.push_ms = go2cpp_container_test::MsSince(start);

    start = Clock::now();
    while (!heap.empty()) {
        result.checksum += heap.top();
        heap.pop();
    }
    result.pop_ms = go2cpp_container_test::MsSince(start);
    return result;
}

void StressHeap(const std::vector<int>& values) {
    go2cpp_container_test::Announce("stress: heap 1M push + 1M pop");

    std::vector<double> our_push;
    std::vector<double> our_pop;
    std::vector<double> std_push;
    std::vector<double> std_pop;
    long long our_checksum = 0;
    long long std_checksum = 0;

    for (int round = 0; round < kRounds; ++round) {
        const HeapResult ours = RunOurHeap(values);
        const HeapResult reference = RunStdHeap(values);
        our_push.push_back(ours.push_ms);
        our_pop.push_back(ours.pop_ms);
        std_push.push_back(reference.push_ms);
        std_pop.push_back(reference.pop_ms);
        our_checksum = ours.checksum;
        std_checksum = reference.checksum;
    }

    const double ours_push = Median(our_push);
    const double ours_pop = Median(our_pop);
    const double ref_push = Median(std_push);
    const double ref_pop = Median(std_pop);
    const double ours_total = ours_push + ours_pop;
    const double ref_total = ref_push + ref_pop;
    const std::size_t operations = kCount * 2;

    std::printf(
        "[stress] heap ours  : push=%8.2f ms pop=%8.2f ms total=%8.2f ms "
        "(%.2f Mops/s, %.1f ns/op)\n",
        ours_push, ours_pop, ours_total, MopsPerSecond(operations, ours_total),
        ours_total * 1e6 / static_cast<double>(operations));
    std::printf(
        "[stress] heap std   : push=%8.2f ms pop=%8.2f ms total=%8.2f ms "
        "(%.2f Mops/s, %.1f ns/op)\n",
        ref_push, ref_pop, ref_total, MopsPerSecond(operations, ref_total),
        ref_total * 1e6 / static_cast<double>(operations));
    std::printf("[stress] heap ratio : ours/std total = %.3f, checksum eq=%d\n",
                ref_total > 0.0 ? ours_total / ref_total : 0.0,
                our_checksum == std_checksum ? 1 : 0);
    std::printf("[stress] heap memory: N*sizeof(int)=%.2f MiB (payload only), "
                "sizeof(Heap<int>)=%zu\n",
                static_cast<double>(kCount * sizeof(int)) / (1024.0 * 1024.0),
                sizeof(container::Heap<int>));

    CONTAINER_CHECK(our_checksum == std_checksum);
}

// ===========================================================================
// List：1M push_back + traverse + Clear
// ===========================================================================
struct ListResult {
    double push_ms = 0.0;
    double traverse_ms = 0.0;
    double clear_ms = 0.0;
    long long checksum = 0;
};

ListResult RunOurList(const std::vector<int>& values) {
    ListResult result;
    container::List<int> list;

    auto start = Clock::now();
    for (int value : values) {
        list.PushBack(value);
    }
    result.push_ms = go2cpp_container_test::MsSince(start);

    start = Clock::now();
    for (auto* node = list.Front(); node != nullptr; node = node->Next()) {
        result.checksum += node->Value;
    }
    result.traverse_ms = go2cpp_container_test::MsSince(start);

    start = Clock::now();
    list.Clear();
    result.clear_ms = go2cpp_container_test::MsSince(start);
    return result;
}

ListResult RunStdList(const std::vector<int>& values) {
    ListResult result;
    std::list<int> list;

    auto start = Clock::now();
    for (int value : values) {
        list.push_back(value);
    }
    result.push_ms = go2cpp_container_test::MsSince(start);

    start = Clock::now();
    for (int value : list) {
        result.checksum += value;
    }
    result.traverse_ms = go2cpp_container_test::MsSince(start);

    start = Clock::now();
    list.clear();
    result.clear_ms = go2cpp_container_test::MsSince(start);
    return result;
}

void StressList(const std::vector<int>& values) {
    go2cpp_container_test::Announce("stress: list 1M push_back + traverse + clear");

    std::vector<double> our_push;
    std::vector<double> our_traverse;
    std::vector<double> our_clear;
    std::vector<double> std_push;
    std::vector<double> std_traverse;
    std::vector<double> std_clear;
    long long our_checksum = 0;
    long long std_checksum = 0;

    for (int round = 0; round < kRounds; ++round) {
        const ListResult ours = RunOurList(values);
        const ListResult reference = RunStdList(values);
        our_push.push_back(ours.push_ms);
        our_traverse.push_back(ours.traverse_ms);
        our_clear.push_back(ours.clear_ms);
        std_push.push_back(reference.push_ms);
        std_traverse.push_back(reference.traverse_ms);
        std_clear.push_back(reference.clear_ms);
        our_checksum = ours.checksum;
        std_checksum = reference.checksum;
    }

    const double ours_push = Median(our_push);
    const double ours_traverse = Median(our_traverse);
    const double ours_clear = Median(our_clear);
    const double ref_push = Median(std_push);
    const double ref_traverse = Median(std_traverse);
    const double ref_clear = Median(std_clear);
    const double ours_total = ours_push + ours_traverse + ours_clear;
    const double ref_total = ref_push + ref_traverse + ref_clear;

    std::printf(
        "[stress] list ours  : push=%8.2f ms traverse=%8.2f ms clear=%8.2f ms "
        "total=%8.2f ms (%.2f Mops/s)\n",
        ours_push, ours_traverse, ours_clear, ours_total,
        MopsPerSecond(kCount * 3, ours_total));
    std::printf(
        "[stress] list std   : push=%8.2f ms traverse=%8.2f ms clear=%8.2f ms "
        "total=%8.2f ms (%.2f Mops/s)\n",
        ref_push, ref_traverse, ref_clear, ref_total,
        MopsPerSecond(kCount * 3, ref_total));
    std::printf("[stress] list ratio : ours/std total = %.3f, checksum eq=%d, "
                "sizeof(Element<int>)=%zu\n",
                ref_total > 0.0 ? ours_total / ref_total : 0.0,
                our_checksum == std_checksum ? 1 : 0,
                sizeof(container::Element<int>));

    CONTAINER_CHECK(our_checksum == std_checksum);
}

// ===========================================================================
// Ring：1M 节点建环 + 1M 次 Next 遍历
// ===========================================================================
struct RawRingNode {
    RawRingNode* next = nullptr;
    int value = 0;
};

struct RingResult {
    double create_ms = 0.0;
    double traverse_ms = 0.0;
    long long checksum = 0;
};

RingResult RunOurRing() {
    RingResult result;

    auto start = Clock::now();
    container::Ring<int>* ring =
        container::Ring<int>::New(static_cast<std::size_t>(kCount));
    result.create_ms = go2cpp_container_test::MsSince(start);
    CONTAINER_REQUIRE(ring != nullptr);

    // 赋值不计时，避免把格式化/测试噪声混进遍历指标。
    container::Ring<int>* node = ring;
    for (std::size_t i = 0; i < kCount; ++i) {
        node->Value = static_cast<int>(i);
        node = node->Next();
    }

    start = Clock::now();
    for (std::size_t i = 0; i < kCount; ++i) {
        result.checksum += node->Value;
        node = node->Next();
    }
    result.traverse_ms = go2cpp_container_test::MsSince(start);
    CONTAINER_CHECK(node == ring);

    container::Ring<int>::DeleteRing(ring);
    return result;
}

RingResult RunRawRing() {
    RingResult result;
    std::vector<RawRingNode> nodes(kCount);

    auto start = Clock::now();
    for (std::size_t i = 0; i < kCount; ++i) {
        nodes[i].value = static_cast<int>(i);
        nodes[i].next = &nodes[(i + 1) % kCount];
    }
    result.create_ms = go2cpp_container_test::MsSince(start);

    start = Clock::now();
    const RawRingNode* node = &nodes[0];
    for (std::size_t i = 0; i < kCount; ++i) {
        result.checksum += node->value;
        node = node->next;
    }
    result.traverse_ms = go2cpp_container_test::MsSince(start);
    return result;
}

void StressRing() {
    go2cpp_container_test::Announce("stress: ring 1M New + 1M Next");

    std::vector<double> our_create;
    std::vector<double> our_traverse;
    std::vector<double> raw_create;
    std::vector<double> raw_traverse;
    long long our_checksum = 0;
    long long raw_checksum = 0;

    for (int round = 0; round < kRounds; ++round) {
        const RingResult ours = RunOurRing();
        const RingResult reference = RunRawRing();
        our_create.push_back(ours.create_ms);
        our_traverse.push_back(ours.traverse_ms);
        raw_create.push_back(reference.create_ms);
        raw_traverse.push_back(reference.traverse_ms);
        our_checksum = ours.checksum;
        raw_checksum = reference.checksum;
    }

    const double ours_create = Median(our_create);
    const double ours_traverse = Median(our_traverse);
    const double ref_create = Median(raw_create);
    const double ref_traverse = Median(raw_traverse);

    std::printf(
        "[stress] ring ours  : New=%8.2f ms Next(1M)=%8.2f ms "
        "(%.2f Mops/s)\n",
        ours_create, ours_traverse,
        MopsPerSecond(kCount, ours_traverse));
    std::printf(
        "[stress] ring raw   : build=%8.2f ms Next(1M)=%8.2f ms "
        "(%.2f Mops/s, reference only)\n",
        ref_create, ref_traverse,
        MopsPerSecond(kCount, ref_traverse));
    std::printf("[stress] ring ratio : traverse ours/raw = %.3f, checksum eq=%d, "
                "sizeof(Ring<int>)=%zu, nodes=%.2f MiB\n",
                ref_traverse > 0.0 ? ours_traverse / ref_traverse : 0.0,
                our_checksum == raw_checksum ? 1 : 0,
                sizeof(container::Ring<int>),
                static_cast<double>(kCount * sizeof(container::Ring<int>)) /
                    (1024.0 * 1024.0));

    CONTAINER_CHECK(our_checksum == raw_checksum);
}

}  // namespace

void RunStressTests() {
    std::mt19937_64 rng(20261018ULL);
    std::vector<int> values(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        values[i] = static_cast<int>(rng() % 100000000ULL);
    }

    StressHeap(values);
    StressList(values);
    StressRing();
}
