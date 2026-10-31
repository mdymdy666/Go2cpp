/**
 * @file test_lru_k.cpp
 * @brief LRU-K(2)：corral 准入、严格 reference-time 淘汰、扫描免疫。
 */
#include "cache_test_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include "go2cpp/cache/lru_k.hpp"

using go2cpp::cache::CacheOptions;
using go2cpp::cache::LruKCache;

namespace {

std::size_t MainSize(const LruKCache<std::uint64_t, std::uint64_t>& cache) {
    const auto stats = cache.Stats();
    return stats.size - stats.ghost_size;
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
}
