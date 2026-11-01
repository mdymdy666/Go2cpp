/**
 * @file test_lru_k.cpp
 * @brief LRU-K(2)：corral 准入、严格 reference-time 淘汰、扫描免疫。
 */
#include "cache_test_support.hpp"

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>

#include "go2cpp/cache/lru_k.hpp"
#include "lru_k_reference.hpp"

using go2cpp::cache::CacheOptions;
using go2cpp::cache::CacheRequest;
using go2cpp::cache::CacheStats;
using go2cpp::cache::LruKCache;
using go2cpp::cache::Workload;
using go2cpp::cache::WorkloadKind;
using go2cpp::cache::WorkloadOptions;

namespace {

std::size_t MainSize(const LruKCache<std::uint64_t, std::uint64_t>& cache) {
    const auto stats = cache.Stats();
    return stats.size - stats.ghost_size;
}

/**
 * @brief 逐字段比较时间轮实现与旧 std::map 参考实现的可观察状态。
 *
 * @details 只在 500k 随机对拍里使用：任何一次 Size/Weight/统计计数/
 *          Describe 不一致都立即 abort，避免「两边都错所以相等」的误报。
 */
void RequireSameState(const LruKCache<std::uint64_t, std::uint64_t>& fast,
                      const go2cpp::cache::test::LruKReferenceCache<
                          std::uint64_t, std::uint64_t>& reference,
                      const char* phase, std::size_t op) {
    const auto lhs = fast.Stats();
    const auto rhs = reference.Stats();
    const bool same =
        fast.Size() == reference.Size() &&
        fast.Weight() == reference.Weight() && lhs.hits == rhs.hits &&
        lhs.misses == rhs.misses && lhs.inserts == rhs.inserts &&
        lhs.updates == rhs.updates && lhs.evictions == rhs.evictions &&
        lhs.ghost_hits == rhs.ghost_hits && lhs.rejections == rhs.rejections &&
        lhs.removals == rhs.removals && lhs.size == rhs.size &&
        lhs.capacity == rhs.capacity && lhs.weight == rhs.weight &&
        lhs.max_weight == rhs.max_weight && lhs.ghost_size == rhs.ghost_size;
    if (!same) {
        std::fprintf(
            stderr,
            "[differential] state mismatch phase=%s op=%zu\n"
            "  wheel: size=%zu weight=%zu hits=%llu misses=%llu "
            "inserts=%llu updates=%llu evictions=%llu removals=%llu "
            "ghost=%zu\n"
            "  map  : size=%zu weight=%zu hits=%llu misses=%llu "
            "inserts=%llu updates=%llu evictions=%llu removals=%llu "
            "ghost=%zu\n",
            phase, op, fast.Size(), fast.Weight(),
            static_cast<unsigned long long>(lhs.hits),
            static_cast<unsigned long long>(lhs.misses),
            static_cast<unsigned long long>(lhs.inserts),
            static_cast<unsigned long long>(lhs.updates),
            static_cast<unsigned long long>(lhs.evictions),
            static_cast<unsigned long long>(lhs.removals), lhs.ghost_size,
            reference.Size(), reference.Weight(),
            static_cast<unsigned long long>(rhs.hits),
            static_cast<unsigned long long>(rhs.misses),
            static_cast<unsigned long long>(rhs.inserts),
            static_cast<unsigned long long>(rhs.updates),
            static_cast<unsigned long long>(rhs.evictions),
            static_cast<unsigned long long>(rhs.removals), rhs.ghost_size);
        CACHE_REQUIRE(false);
    }
    if ((op & 0xFFFU) == 0U) {
        CACHE_CHECK(fast.Describe() == reference.Describe());
    }
}

/**
 * @brief 50 万次随机操作对拍：时间轮 vs 旧 std::map 实现。
 *
 * @details 覆盖 6 种容量（含 1/3 这种极小边界）、Get/Put/Erase/Peek/
 *          Clear、热/冷 key、权重 1~9、value=nullptr 分支。每一步都
 *          比较全部统计计数，每 4096 步比较 Describe；阶段结束时再比一次。
 */
void RunLruKDifferential(std::size_t total_operations) {
    using go2cpp::cache::test::LruKReferenceCache;
    const std::size_t capacities[] = {1U, 3U, 17U, 128U, 1024U, 2048U};
    constexpr std::size_t kPhases =
        sizeof(capacities) / sizeof(capacities[0]);
    std::mt19937_64 rng(0x4C52554B32ULL);  // "LRUK2"
    std::size_t done = 0;
    for (std::size_t phase = 0; phase < kPhases; ++phase) {
        const std::size_t capacity = capacities[phase];
        const std::size_t ops = (phase + 1U == kPhases)
                                    ? total_operations - done
                                    : total_operations / kPhases;
        done += ops;
        CacheOptions options;
        options.capacity = capacity;
        LruKCache<std::uint64_t, std::uint64_t> fast(options);
        LruKReferenceCache<std::uint64_t, std::uint64_t> reference(options);
        const std::uint64_t key_span =
            static_cast<std::uint64_t>(capacity) * 4U + 16U;
        for (std::size_t op = 0; op < ops; ++op) {
            if (op != 0U && (op % 50000U) == 0U) {
                fast.Clear();
                reference.Clear();
                RequireSameState(fast, reference, "clear", op);
                continue;
            }
            const std::uint64_t key = (rng() % 10U < 7U)
                                          ? (rng() % capacity)
                                          : (rng() % key_span);
            const std::uint64_t value = rng();
            const std::size_t weight =
                1U + static_cast<std::size_t>(rng() % 9U);
            const std::uint64_t action = rng() % 20U;
            if (action < 12U) {
                const bool want_value = (rng() & 7U) != 0U;
                std::uint64_t fast_value = ~0ULL;
                std::uint64_t reference_value = ~0ULL;
                const bool fast_hit =
                    fast.Get(key, want_value ? &fast_value : nullptr);
                const bool reference_hit = reference.Get(
                    key, want_value ? &reference_value : nullptr);
                CACHE_CHECK(fast_hit == reference_hit);
                if (fast_hit && want_value) {
                    CACHE_CHECK(fast_value == reference_value);
                }
            } else if (action < 16U) {
                fast.Put(key, value, weight);
                reference.Put(key, value, weight);
            } else if (action < 18U) {
                CACHE_CHECK(fast.Erase(key) == reference.Erase(key));
            } else {
                std::uint64_t fast_value = 0;
                std::uint64_t reference_value = 0;
                const bool fast_hit = fast.Peek(key, &fast_value);
                const bool reference_hit =
                    reference.Peek(key, &reference_value);
                CACHE_CHECK(fast_hit == reference_hit);
                if (fast_hit) {
                    CACHE_CHECK(fast_value == reference_value);
                }
            }
            RequireSameState(fast, reference, "mixed", op);
        }
        // 收尾内容对拍：逐 key Peek，确保两边留下的条目集合与 value 完全一致
        // （只比统计计数无法排除「淘汰了不同条目但计数相同」）。
        for (std::uint64_t key = 0; key < key_span; ++key) {
            std::uint64_t fast_value = 0;
            std::uint64_t reference_value = 0;
            const bool fast_hit = fast.Peek(key, &fast_value);
            const bool reference_hit = reference.Peek(key, &reference_value);
            CACHE_CHECK(fast_hit == reference_hit);
            if (fast_hit) {
                CACHE_CHECK(fast_value == reference_value);
            }
        }
        fast.Clear();
        reference.Clear();
        RequireSameState(fast, reference, "phase-end", ops);
        CACHE_CHECK(fast.Size() == 0);
        CACHE_CHECK(reference.Size() == 0);
    }
}

/**
 * @brief 复刻 cache_bench --full 的 5 个场景（capacity=2000, ops=300000）。
 *
 * @details 这里不复制基准的打印逻辑，只复制**请求序列**：scan 是
 *          50k 预热 + 50k 扫描 + 50k 恢复，其余场景是 300k 的
 *          Workload 生成。这样可以在一处验证「基准确认的命中率」确实
 *          来自同一批请求。
 */
std::vector<CacheRequest> BuildBenchRequests(WorkloadKind kind) {
    constexpr std::size_t kCapacity = 2000;
    constexpr std::size_t kOps = 300000;
    constexpr std::size_t kScanLength = 50000;
    constexpr std::size_t kRecoveryOps = 50000;
    constexpr std::uint64_t kSeed = 0x5DEECE66DULL;
    std::vector<CacheRequest> requests;
    if (kind == WorkloadKind::kScan) {
        WorkloadOptions warm_options;
        warm_options.hot_keys = kCapacity;
        warm_options.cold_keys = kCapacity * 100U;
        warm_options.operations = kRecoveryOps;
        warm_options.seed = kSeed;
        requests = Workload(WorkloadKind::kHotspot, warm_options).Generate();
        for (std::size_t i = 0; i < kScanLength; ++i) {
            requests.push_back(
                CacheRequest{100000 + static_cast<std::uint64_t>(i), 1});
        }
        WorkloadOptions recover_options = warm_options;
        recover_options.seed = kSeed + 1U;
        const auto tail =
            Workload(WorkloadKind::kHotspot, recover_options).Generate();
        requests.insert(requests.end(), tail.begin(), tail.end());
        return requests;
    }
    WorkloadOptions options;
    options.hot_keys = kCapacity;
    options.cold_keys = kCapacity * 100U;
    options.operations = kOps;
    options.seed = kSeed;
    options.videos = kCapacity / 20U + 10U;
    options.segments_per_video = 100;
    return Workload(kind, options).Generate();
}

/** @brief 用同一请求序列跑一遍读穿透，返回精确命中数与最终统计。 */
template <class CacheT>
std::uint64_t RunScenario(const std::vector<CacheRequest>& requests,
                          CacheStats* stats, std::size_t* size,
                          std::size_t* weight) {
    CacheOptions options;
    options.capacity = 2000;
    CacheT cache(options);
    std::uint64_t value = 0;
    std::uint64_t hits = 0;
    for (const CacheRequest& request : requests) {
        if (cache.Get(request.key, &value)) {
            ++hits;
        } else {
            cache.Put(request.key, request.key, request.weight);
        }
    }
    *stats = cache.Stats();
    *size = cache.Size();
    *weight = cache.Weight();
    return hits;
}

/** @brief cache_bench 的 5 个场景：旧/新精确命中数必须逐场景相等。 */
void RunLruKScenarioParity() {
    using go2cpp::cache::test::LruKReferenceCache;
    const WorkloadKind kinds[] = {WorkloadKind::kHotspot, WorkloadKind::kScan,
                                  WorkloadKind::kWeighted, WorkloadKind::kCdn,
                                  WorkloadKind::kMixed};
    for (WorkloadKind kind : kinds) {
        const std::vector<CacheRequest> requests = BuildBenchRequests(kind);
        CacheStats map_stats;
        CacheStats wheel_stats;
        std::size_t map_size = 0;
        std::size_t map_weight = 0;
        std::size_t wheel_size = 0;
        std::size_t wheel_weight = 0;
        const std::uint64_t map_hits =
            RunScenario<LruKReferenceCache<std::uint64_t, std::uint64_t>>(
                requests, &map_stats, &map_size, &map_weight);
        const std::uint64_t wheel_hits =
            RunScenario<LruKCache<std::uint64_t, std::uint64_t>>(
                requests, &wheel_stats, &wheel_size, &wheel_weight);
        CACHE_CHECK(map_hits == wheel_hits);
        CACHE_CHECK(map_size == wheel_size);
        CACHE_CHECK(map_weight == wheel_weight);
        CACHE_CHECK(map_stats.hits == wheel_stats.hits);
        CACHE_CHECK(map_stats.misses == wheel_stats.misses);
        CACHE_CHECK(map_stats.inserts == wheel_stats.inserts);
        CACHE_CHECK(map_stats.updates == wheel_stats.updates);
        CACHE_CHECK(map_stats.evictions == wheel_stats.evictions);
        CACHE_CHECK(map_stats.removals == wheel_stats.removals);
        CACHE_CHECK(map_stats.ghost_size == wheel_stats.ghost_size);
        std::printf("[cache]   %-10s requests=%zu hits=%llu identical=%d\n",
                    go2cpp::cache::WorkloadName(kind), requests.size(),
                    static_cast<unsigned long long>(wheel_hits),
                    map_hits == wheel_hits ? 1 : 0);
    }
}

}  // namespace

void RunLruKTests() {
    go2cpp_cache_test::Announce("LRU-K corral admission");

    {
        CacheOptions options;
        options.capacity = 4;
        LruKCache<int, int> cache(options);
        cache.Put(1, 10);
        CACHE_CHECK(cache.Size() == 1);
        CACHE_CHECK(cache.Stats().ghost_size == 1);  // 首次写入只进 corral。
        int value = 0;
        CACHE_CHECK(cache.Get(1, &value) && value == 10);
        CACHE_CHECK(cache.Stats().ghost_size == 0);  // 二次访问晋升主表。
        CACHE_CHECK(cache.Get(1, &value) && value == 10);
        CACHE_CHECK(cache.Stats().hits == 2);
    }

    go2cpp_cache_test::Announce("LRU-K scan resistance");
    {
        CacheOptions options;
        options.capacity = 4;
        LruKCache<std::uint64_t, std::uint64_t> cache(options);
        std::uint64_t value = 0;
        for (std::uint64_t key = 0; key < 4; ++key) {
            cache.Put(key, key * 10);
            CACHE_CHECK(cache.Get(key, &value));
            CACHE_CHECK(cache.Get(key, &value));  // 提升到主表。
        }
        CACHE_CHECK(MainSize(cache) == 4);

        // 读穿透扫描：200 个只访问一次的冷 key，全部 Put 回缓存。
        for (std::uint64_t i = 0; i < 200; ++i) {
            const std::uint64_t key = 100000 + i;
            if (!cache.Get(key, &value)) {
                cache.Put(key, key);
            }
        }
        // 扫描没有进入主表，4 个热点仍然全部命中。
        for (std::uint64_t key = 0; key < 4; ++key) {
            CACHE_CHECK(cache.Get(key, &value));
            CACHE_CHECK(value == key * 10);
        }
        CACHE_CHECK(MainSize(cache) == 4);
        CACHE_CHECK(cache.Stats().size <= 8);  // 主表 4 + corral 4。
    }

    go2cpp_cache_test::Announce("LRU-K reference-time eviction");
    {
        CacheOptions options;
        options.capacity = 2;
        LruKCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10);
        CACHE_CHECK(cache.Get(1, &value));   // 晋升，ref = 第一次访问时间。
        CACHE_CHECK(cache.Get(1, &value));   // 更新 ref。
        cache.Put(2, 20);
        CACHE_CHECK(cache.Get(2, &value));   // 晋升 2。
        CACHE_CHECK(cache.Get(1, &value));   // 让 1 的 ref 比 2 新。
        CACHE_CHECK(cache.Get(1, &value));
        cache.Put(3, 30);
        CACHE_CHECK(cache.Get(3, &value));   // 晋升 3，淘汰 ref 最旧的 2。
        CACHE_CHECK(!cache.Get(2, &value));
        CACHE_CHECK(cache.Get(1, &value) && value == 10);
        CACHE_CHECK(cache.Get(3, &value) && value == 30);
    }

    go2cpp_cache_test::Announce("LRU-K corral fifo capacity");
    {
        CacheOptions options;
        options.capacity = 4;  // corral = 4。
        LruKCache<int, int> cache(options);
        for (int i = 0; i < 6; ++i) {
            cache.Put(i, i * 10);
        }
        CACHE_CHECK(cache.Size() == 4);
        int value = 0;
        CACHE_CHECK(!cache.Get(0, &value));
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Get(2, &value) && value == 20);
        CACHE_CHECK(cache.Get(5, &value) && value == 50);
    }

    go2cpp_cache_test::Announce("LRU-K erase / clear / stats");
    {
        CacheOptions options;
        options.capacity = 3;
        LruKCache<int, int> cache(options);
        cache.Put(1, 10);
        CACHE_CHECK(cache.Erase(1));
        CACHE_CHECK(!cache.Erase(1));
        cache.Put(2, 20);
        int value = 0;
        CACHE_CHECK(cache.Get(2, &value));
        CACHE_CHECK(cache.Erase(2));
        CACHE_CHECK(!cache.Get(2, &value));

        cache.Clear();
        CACHE_CHECK(cache.Size() == 0);
        CACHE_CHECK(cache.Weight() == 0);
        CACHE_CHECK(cache.Stats().removals == 2);
        CACHE_CHECK(cache.Stats().hits == 1);
        CACHE_CHECK(cache.Stats().misses == 1);
        CACHE_CHECK(cache.Describe().find("LRU-K(2)") == 0);
    }

    go2cpp_cache_test::Announce("LRU-K capacity 1");
    {
        CacheOptions options;
        options.capacity = 1;
        LruKCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10);
        CACHE_CHECK(cache.Get(1, &value));
        cache.Put(2, 20);
        CACHE_CHECK(cache.Get(2, &value));  // 晋升 2 淘汰主表里的 1。
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Size() == 1);
    }

    go2cpp_cache_test::Announce("LRU-K weight accounting");
    {
        CacheOptions options;
        options.capacity = 2;
        LruKCache<int, int> cache(options);
        cache.Put(1, 10, 100);
        CACHE_CHECK(cache.Weight() == 100);
        int value = 0;
        CACHE_CHECK(cache.Get(1, &value));
        CACHE_CHECK(cache.Weight() == 100);  // 晋升不改总权重。
        cache.Put(2, 20, 50);
        CACHE_CHECK(cache.Weight() == 150);
        CACHE_CHECK(cache.Erase(1));
        CACHE_CHECK(cache.Weight() == 50);
    }

    go2cpp_cache_test::Announce("LRU-K invalid capacity");
    {
        bool threw = false;
        try {
            CacheOptions options;
            options.capacity = 0;
            LruKCache<int, int> cache(options);
            (void)cache;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CACHE_CHECK(threw);
    }

    go2cpp_cache_test::Announce("LRU-K 500k random differential (wheel vs map)");
    RunLruKDifferential(500000U);

    go2cpp_cache_test::Announce("LRU-K cache_bench scenario parity (exact hits)");
    RunLruKScenarioParity();
}
