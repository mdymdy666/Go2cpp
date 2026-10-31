/**
 * @file test_scenarios.cpp
 * @brief 5 个真实场景模拟：热点、扫描污染、权重、CDN、混合。
 *
 * @details 每个场景都走「读穿透」路径（Get miss -> Put），与真实缓存层
 *          的接入方式一致。断言只锁定**策略语义**（谁该抗扫描、谁该守住
 *          字节预算），不锁定具体命中率小数——后者随机器/编译器波动，
 *          精确数字放在 `go2cpp_cache_bench` 的报告里。
 */
#include "cache_test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "go2cpp/cache/factory.hpp"
#include "go2cpp/cache/workload.hpp"

using go2cpp::cache::CacheOptions;
using go2cpp::cache::Policy;
using go2cpp::cache::Workload;
using go2cpp::cache::WorkloadKind;
using go2cpp::cache::WorkloadOptions;

namespace {

struct WeightedResult {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::uint64_t hit_weight = 0;

    double HitRate() const {
        const std::size_t total = hits + misses;
        return total == 0 ? 0.0
                          : static_cast<double>(hits) /
                                static_cast<double>(total);
    }
};

WeightedResult RunWeightedReadThrough(
    go2cpp::cache::Cache<std::uint64_t, std::uint64_t>& cache,
    const std::vector<go2cpp::cache::CacheRequest>& requests) {
    WeightedResult result;
    std::uint64_t value = 0;
    for (const auto& request : requests) {
        if (cache.Get(request.key, &value)) {
            ++result.hits;
            result.hit_weight += request.weight;
        } else {
            ++result.misses;
            cache.Put(request.key, request.key, request.weight);
        }
    }
    return result;
}

double RecoveryRate(const std::vector<std::uint64_t>& hot_keys,
                    go2cpp::cache::Cache<std::uint64_t, std::uint64_t>& cache) {
    std::size_t hits = 0;
    std::uint64_t value = 0;
    for (int round = 0; round < 10; ++round) {
        for (std::uint64_t key : hot_keys) {
            if (cache.Get(key, &value)) {
                ++hits;
            }
        }
    }
    return static_cast<double>(hits) /
           static_cast<double>(hot_keys.size() * 10U);
}

void WarmHotKeys(go2cpp::cache::Cache<std::uint64_t, std::uint64_t>& cache,
                 const std::vector<std::uint64_t>& hot_keys) {
    std::uint64_t value = 0;
    for (int round = 0; round < 20; ++round) {
        for (std::uint64_t key : hot_keys) {
            if (!cache.Get(key, &value)) {
                cache.Put(key, key);
            }
        }
    }
}

}  // namespace

void RunScenarioTests() {
    go2cpp_cache_test::Announce("scenario hotspot 80/20");
    {
        WorkloadOptions options;
        options.hot_keys = 100;
        options.cold_keys = 10000;
        options.operations = 20000;
        Workload workload(WorkloadKind::kHotspot, options);
        const auto requests = workload.Generate();

        CacheOptions cache_options;
        cache_options.capacity = 100;
        for (Policy policy : {Policy::kLru, Policy::kTinyLfu, Policy::kArc,
                              Policy::kTwoQueue, Policy::kClock}) {
            auto cache =
                go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
                    policy, cache_options);
            const auto result =
                go2cpp_cache_test::RunReadThrough<std::uint64_t, std::uint64_t>(
                    *cache, requests);
            std::printf("[cache]   hotspot %-10s hit=%.4f\n", cache->Name(),
                        result.HitRate());
            CACHE_CHECK(result.HitRate() > 0.45);
            CACHE_CHECK(cache->Stats().hits >= result.hits);
        }
    }

    go2cpp_cache_test::Announce("scenario scan pollution / recovery");
    {
        const std::vector<std::uint64_t> hot_keys = [] {
            std::vector<std::uint64_t> keys;
            for (std::uint64_t key = 0; key < 100; ++key) {
                keys.push_back(key);
            }
            return keys;
        }();

        CacheOptions cache_options;
        cache_options.capacity = 100;

        auto measure = [&hot_keys, &cache_options](Policy policy) {
            auto cache =
                go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
                    policy, cache_options);
            WarmHotKeys(*cache, hot_keys);
            const double baseline = RecoveryRate(hot_keys, *cache);

            std::uint64_t value = 0;
            for (std::uint64_t i = 0; i < 2000; ++i) {
                const std::uint64_t key = 100000 + i;
                if (!cache->Get(key, &value)) {
                    cache->Put(key, key);
                }
            }
            const double recovery = RecoveryRate(hot_keys, *cache);
            std::printf(
                "[cache]   scan %-10s baseline=%.3f recovery=%.3f\n",
                cache->Name(), baseline, recovery);
            return std::make_pair(baseline, recovery);
        };

        const auto lru = measure(Policy::kLru);
        const auto lru_k = measure(Policy::kLruK);
        const auto tiny = measure(Policy::kTinyLfu);
        const auto arc = measure(Policy::kArc);
        const auto clock = measure(Policy::kClock);

        CACHE_CHECK(lru.first > 0.99);
        CACHE_CHECK(lru.second < 0.10);      // 扫描把 LRU 冲刷干净。
        CACHE_CHECK(lru_k.second > 0.95);    // LRU-K 主表不受扫描影响。
        CACHE_CHECK(tiny.second > 0.90);     // TinyLFU 的频率准入拒绝扫描。
        CACHE_CHECK(arc.second > 0.90);      // ARC 的 T2 保留热点。
        CACHE_CHECK(clock.second < 0.10);    // CLOCK 与 LRU 一样会被冲刷。
        CACHE_CHECK(lru_k.second > lru.second + 0.5);
    }

    go2cpp_cache_test::Announce("scenario weighted 1:1000");
    {
        WorkloadOptions options;
        options.hot_keys = 200;
        options.cold_keys = 20000;
        options.operations = 20000;
        Workload workload(WorkloadKind::kWeighted, options);
        const auto requests = workload.Generate();

        CacheOptions lru_options;
        lru_options.capacity = 200;
        auto lru = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
            Policy::kLru, lru_options);
        const auto lru_result = RunWeightedReadThrough(*lru, requests);

        CacheOptions gdsf_options;
        gdsf_options.capacity = 0;  // 只按字节预算。
        gdsf_options.max_weight = 200U * 200U;  // 约等于 200 个平均对象。
        auto gdsf = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
            Policy::kGdsf, gdsf_options);
        const auto gdsf_result = RunWeightedReadThrough(*gdsf, requests);

        std::printf("[cache]   weighted LRU hit=%.4f avg_hit_w=%.1f\n",
                    lru_result.HitRate(),
                    lru_result.hits == 0
                        ? 0.0
                        : static_cast<double>(lru_result.hit_weight) /
                              static_cast<double>(lru_result.hits));
        std::printf("[cache]   weighted GDSF hit=%.4f avg_hit_w=%.1f "
                    "weight=%zu/%zu\n",
                    gdsf_result.HitRate(),
                    gdsf_result.hits == 0
                        ? 0.0
                        : static_cast<double>(gdsf_result.hit_weight) /
                              static_cast<double>(gdsf_result.hits),
                    gdsf->Weight(), gdsf_options.max_weight);
        CACHE_CHECK(lru_result.HitRate() > 0.30);
        CACHE_CHECK(gdsf_result.HitRate() > 0.30);
        CACHE_CHECK(gdsf->Weight() <= gdsf_options.max_weight);
        // GDSF 的字节预算优先保护小对象：命中请求的平均大小应更小。
        CACHE_CHECK(gdsf_result.hit_weight * lru_result.hits <
                    lru_result.hit_weight * gdsf_result.hits);
    }

    go2cpp_cache_test::Announce("scenario cdn zipf + segment sizes");
    {
        WorkloadOptions options;
        options.videos = 200;
        options.segments_per_video = 100;
        options.operations = 30000;
        Workload workload(WorkloadKind::kCdn, options);
        const auto requests = workload.Generate();

        CacheOptions options_lru;
        options_lru.capacity = 1000;
        auto lru = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
            Policy::kLru, options_lru);
        const auto lru_result =
            go2cpp_cache_test::RunReadThrough<std::uint64_t, std::uint64_t>(
                *lru, requests);

        CacheOptions options_gdsf;
        options_gdsf.capacity = 0;
        options_gdsf.max_weight = 1000U * 1160U;  // 约 1000 个平均分片。
        auto gdsf = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
            Policy::kGdsf, options_gdsf);
        const auto gdsf_result =
            go2cpp_cache_test::RunReadThrough<std::uint64_t, std::uint64_t>(
                *gdsf, requests);

        std::printf("[cache]   cdn LRU hit=%.4f GDSF hit=%.4f\n",
                    lru_result.HitRate(), gdsf_result.HitRate());
        CACHE_CHECK(lru_result.HitRate() > 0.20);
        CACHE_CHECK(gdsf_result.HitRate() > 0.20);
        CACHE_CHECK(gdsf->Weight() <= options_gdsf.max_weight);
    }

    go2cpp_cache_test::Announce("scenario mixed hotspot + scan + burst");
    {
        WorkloadOptions options;
        options.hot_keys = 256;
        options.cold_keys = 20000;
        options.operations = 30000;
        Workload workload(WorkloadKind::kMixed, options);
        const auto requests = workload.Generate();

        CacheOptions cache_options;
        cache_options.capacity = 256;
        for (Policy policy : {Policy::kLru, Policy::kLruK, Policy::kTinyLfu,
                              Policy::kArc}) {
            auto cache =
                go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
                    policy, cache_options);
            const auto result =
                go2cpp_cache_test::RunReadThrough<std::uint64_t, std::uint64_t>(
                    *cache, requests);
            std::printf("[cache]   mixed %-10s hit=%.4f evict=%llu\n",
                        cache->Name(), result.HitRate(),
                        static_cast<unsigned long long>(
                            cache->Stats().evictions));
            CACHE_CHECK(result.HitRate() > 0.20);
            const auto stats = cache->Stats();
            CACHE_CHECK(stats.size - stats.ghost_size <=
                        cache_options.capacity);
            CACHE_CHECK(cache->Size() <=
                        cache_options.capacity + stats.ghost_size);
        }
    }
}
