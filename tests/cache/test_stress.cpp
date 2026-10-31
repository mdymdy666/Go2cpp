/**
 * @file test_stress.cpp
 * @brief 压测：大容量稳定性、权重预算、并发吞吐、长跑统计守恒。
 */
#include "cache_test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "go2cpp/cache/factory.hpp"

using go2cpp::cache::CacheOptions;
using go2cpp::cache::Policy;

void RunStressTests() {
    go2cpp_cache_test::Announce("stress lru 1M ops");

    {
        CacheOptions options;
        options.capacity = 10000;
        auto cache = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
            Policy::kLru, options);
        std::mt19937_64 rng(7);
        std::uint64_t value = 0;
        const auto start = go2cpp_cache_test::Clock::now();
        constexpr int kOps = 1000000;
        for (int i = 0; i < kOps; ++i) {
            const std::uint64_t key = rng() % 50000U;
            if (!cache->Get(key, &value)) {
                cache->Put(key, key);
            }
        }
        const double ms = go2cpp_cache_test::MsSince(start);
        const auto stats = cache->Stats();
        const double mops = static_cast<double>(kOps) / (ms * 1000.0);
        std::printf("[cache]   LRU %.1f ms, %.2f Mops/s, hit=%.4f, size=%zu\n",
                    ms, mops, stats.HitRate(), cache->Size());
        CACHE_CHECK(stats.hits + stats.misses == kOps);
        CACHE_CHECK(cache->Size() <= options.capacity);
        CACHE_CHECK(cache->Size() == options.capacity);
        CACHE_CHECK(mops > 0.1);
    }

    go2cpp_cache_test::Announce("stress lru-k corral bound");
    {
        CacheOptions options;
        options.capacity = 4096;
        auto cache = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
            Policy::kLruK, options);
        for (std::uint64_t key = 0; key < 200000; ++key) {
            cache->Put(key, key);
        }
        const auto stats = cache->Stats();
        const std::size_t corral_limit = options.capacity;
        std::printf("[cache]   LRU-K size=%zu ghost=%zu\n", cache->Size(),
                    stats.ghost_size);
        CACHE_CHECK(stats.ghost_size <= corral_limit);
        CACHE_CHECK(cache->Size() <= options.capacity + corral_limit);
        CACHE_CHECK(stats.inserts == 200000);
    }

    go2cpp_cache_test::Announce("stress gdsf weight budget");
    {
        CacheOptions options;
        options.capacity = 0;
        options.max_weight = 100000;
        auto cache = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
            Policy::kGdsf, options);
        std::mt19937_64 rng(11);
        std::uint64_t value = 0;
        for (int i = 0; i < 200000; ++i) {
            const std::uint64_t key = rng() % 20000U;
            const std::uint32_t weight = 1U + static_cast<std::uint32_t>(
                                                  rng() % 1000U);
            if (!cache->Get(key, &value)) {
                cache->Put(key, key, weight);
            }
            CACHE_CHECK(cache->Weight() <= options.max_weight);
        }
        std::printf("[cache]   GDSF size=%zu weight=%zu\n", cache->Size(),
                    cache->Weight());
        CACHE_CHECK(cache->Size() > 0);
        CACHE_CHECK(cache->Stats().rejections == 0);
    }

    go2cpp_cache_test::Announce("stress arc ghost bound");
    {
        CacheOptions options;
        options.capacity = 512;
        auto cache = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
            Policy::kArc, options);
        std::mt19937_64 rng(13);
        std::uint64_t value = 0;
        for (int i = 0; i < 300000; ++i) {
            const std::uint64_t key = rng() % 5000U;
            if (!cache->Get(key, &value)) {
                cache->Put(key, key);
            }
            if ((i % 997) == 0) {
                const auto stats = cache->Stats();
                CACHE_CHECK(stats.ghost_size <= options.capacity);
                CACHE_CHECK(cache->Size() <= options.capacity);
            }
        }
        std::printf("[cache]   ARC size=%zu ghost=%zu\n", cache->Size(),
                    cache->Stats().ghost_size);
    }

    go2cpp_cache_test::Announce("stress thread-safe 8 threads x 50k");
    {
        CacheOptions options;
        options.capacity = 2048;
        auto cache = go2cpp::cache::MakeThreadSafeCache<std::uint64_t,
                                                        std::uint64_t>(
            Policy::kTinyLfu, options);
        constexpr int kThreads = 8;
        constexpr int kOpsPerThread = 50000;
        const auto start = go2cpp_cache_test::Clock::now();
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int thread = 0; thread < kThreads; ++thread) {
            workers.emplace_back([&cache, thread]() {
                std::uint64_t value = 0;
                std::mt19937_64 rng(static_cast<std::uint64_t>(thread) + 99U);
                for (int i = 0; i < kOpsPerThread; ++i) {
                    const std::uint64_t key = rng() % 20000U;
                    if (!cache->Get(key, &value)) {
                        cache->Put(key, key);
                    }
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        const double ms = go2cpp_cache_test::MsSince(start);
        const auto stats = cache->Stats();
        std::printf(
            "[cache]   TinyLFU mt %.1f ms, %.2f Mops/s, hit=%.4f, size=%zu\n",
            ms, static_cast<double>(kThreads * kOpsPerThread) / (ms * 1000.0),
            stats.HitRate(), cache->Size());
        CACHE_CHECK(stats.hits + stats.misses ==
                    static_cast<std::uint64_t>(kThreads * kOpsPerThread));
        CACHE_CHECK(cache->Size() <= options.capacity);
    }

    go2cpp_cache_test::Announce("stress clear cycles");
    {
        CacheOptions options;
        options.capacity = 1024;
        auto cache = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
            Policy::kClock, options);
        for (int cycle = 0; cycle < 50; ++cycle) {
            for (std::uint64_t key = 0; key < 2000; ++key) {
                cache->Put(cycle * 10000U + key, key);
            }
            CACHE_CHECK(cache->Size() == options.capacity);
            cache->Clear();
            CACHE_CHECK(cache->Size() == 0);
            CACHE_CHECK(cache->Weight() == 0);
        }
    }
}
