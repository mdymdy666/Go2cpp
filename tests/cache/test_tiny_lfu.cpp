/**
 * @file test_tiny_lfu.cpp
 * @brief W-TinyLFU：窗口/SLRU 布局、频率准入、扫描抗性、容量边界。
 */
#include "cache_test_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include "go2cpp/cache/tiny_lfu.hpp"

using go2cpp::cache::CacheOptions;
using go2cpp::cache::TinyLfuCache;

void RunTinyLfuTests() {
    go2cpp_cache_test::Announce("TinyLFU window -> probation -> protected");

    {
        CacheOptions options;
        options.capacity = 100;
        TinyLfuCache<int, int> cache(options);
        cache.Put(1, 10);  // 窗口。
        int value = 0;
        CACHE_CHECK(cache.Get(1, &value) && value == 10);  // 提升到 protected。
        CACHE_CHECK(cache.Get(1, &value));
        CACHE_CHECK(cache.Describe().find("protected=1/") != std::string::npos);
        CACHE_CHECK(cache.Stats().hits == 2);
        CACHE_CHECK(cache.Stats().misses == 0);
    }

    go2cpp_cache_test::Announce("TinyLFU frequency admission rejects scan");
    {
        CacheOptions options;
        options.capacity = 100;
        TinyLfuCache<std::uint64_t, std::uint64_t> cache(options);
        std::uint64_t value = 0;

        // 预热 100 个热点，每个访问 20 次，把频率拉到饱和。
        for (int round = 0; round < 20; ++round) {
            for (std::uint64_t key = 0; key < 100; ++key) {
                if (!cache.Get(key, &value)) {
                    cache.Put(key, key);
                }
            }
        }
        const auto before = cache.Stats();
        CACHE_CHECK(before.hits > 0);
        CACHE_CHECK(before.rejections <= 200);

        // 扫描 2000 个只访问一次的冷 key，每个都读穿透 Put 回缓存。
        for (std::uint64_t i = 0; i < 2000; ++i) {
            const std::uint64_t key = 100000 + i;
            if (!cache.Get(key, &value)) {
                cache.Put(key, key);
            }
        }
        CACHE_CHECK(cache.Stats().rejections > 0);
        CACHE_CHECK(cache.Size() <= 100);

        // 扫描后热点仍然在缓存里：命中率应接近满命中。
        std::size_t hot_hits = 0;
        for (std::uint64_t key = 0; key < 100; ++key) {
            if (cache.Get(key, &value)) {
                ++hot_hits;
            }
        }
        CACHE_CHECK(hot_hits >= 90);
    }

    go2cpp_cache_test::Announce("TinyLFU admission compares frequency");
    {
        CacheOptions options;
        options.capacity = 2;
        TinyLfuCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10);                    // 窗口。
        cache.Put(2, 20);                    // 1 -> probation。
        CACHE_CHECK(cache.Get(1, &value));   // 1 的频率 +1，提升 protected
                                             // 但 protected 容量 0，回到 probation。
        cache.Put(3, 30);  // 窗口里的 2 被挤出，与 1 比较频率：1 胜出。
        CACHE_CHECK(cache.Stats().rejections >= 1);
        CACHE_CHECK(cache.Get(1, &value) && value == 10);
        CACHE_CHECK(!cache.Get(2, &value));
        CACHE_CHECK(cache.Get(3, &value) && value == 30);
    }

    go2cpp_cache_test::Announce("TinyLFU capacity 1 / 2");
    {
        CacheOptions options;
        options.capacity = 1;
        TinyLfuCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10);
        CACHE_CHECK(cache.Get(1, &value));  // 容量 1：窗口命中不丢数据。
        cache.Put(2, 20);
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Get(2, &value));
        CACHE_CHECK(cache.Size() == 1);

        CacheOptions two;
        two.capacity = 2;
        TinyLfuCache<int, int> small(two);
        small.Put(1, 10);
        small.Put(2, 20);
        CACHE_CHECK(small.Size() == 2);
        CACHE_CHECK(small.Get(1, &value));
    }

    go2cpp_cache_test::Announce("TinyLFU erase / clear / weight");
    {
        CacheOptions options;
        options.capacity = 100;
        TinyLfuCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10, 100);
        CACHE_CHECK(cache.Get(1, &value));  // 进 protected。
        CACHE_CHECK(cache.Weight() == 100);
        CACHE_CHECK(cache.Erase(1));

        cache.Put(2, 20);  // 窗口。
        cache.Put(3, 30);  // 2 -> probation。
        CACHE_CHECK(cache.Erase(2));  // 从 probation 删除。
        CACHE_CHECK(cache.Erase(3));  // 从窗口删除。
        CACHE_CHECK(!cache.Erase(3));

        cache.Put(4, 40, 7);
        CACHE_CHECK(cache.Weight() == 7);
        cache.Clear();
        CACHE_CHECK(cache.Size() == 0);
        CACHE_CHECK(cache.Weight() == 0);
        CACHE_CHECK(cache.Stats().removals == 3);
        CACHE_CHECK(cache.Describe().find("W-TinyLFU") == 0);
    }

    go2cpp_cache_test::Announce("TinyLFU invalid capacity / peek");
    {
        bool threw = false;
        try {
            CacheOptions options;
            options.capacity = 0;
            TinyLfuCache<int, int> cache(options);
            (void)cache;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CACHE_CHECK(threw);

        CacheOptions options;
        options.capacity = 16;
        TinyLfuCache<int, int> cache(options);
        cache.Put(1, 10);
        int value = 0;
        CACHE_CHECK(cache.Peek(1, &value) && value == 10);
        CACHE_CHECK(!cache.Peek(2, &value));
        CACHE_CHECK(cache.Stats().hits == 0);
    }
}
