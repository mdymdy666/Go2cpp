/**
 * @file test_lru.cpp
 * @brief LRU 基线：淘汰顺序、命中提升、Peek 无副作用、统计与边界。
 */
#include "cache_test_support.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "go2cpp/cache/lru.hpp"

using go2cpp::cache::CacheOptions;
using go2cpp::cache::LruCache;

void RunLruTests() {
    go2cpp_cache_test::Announce("LRU basic / eviction order");

    {
        CacheOptions options;
        options.capacity = 3;
        LruCache<int, int> cache(options);
        cache.Put(1, 10);
        cache.Put(2, 20);
        cache.Put(3, 30);

        int value = 0;
        CACHE_CHECK(cache.Get(1, &value));
        CACHE_CHECK(value == 10);

        cache.Put(4, 40);  // 2 是 LRU（1 刚被访问过）。
        CACHE_CHECK(cache.Size() == 3);
        CACHE_CHECK(!cache.Get(2, &value));
        CACHE_CHECK(cache.Get(1, &value) && value == 10);
        CACHE_CHECK(cache.Get(3, &value) && value == 30);
        CACHE_CHECK(cache.Get(4, &value) && value == 40);

        const auto stats = cache.Stats();
        CACHE_CHECK(stats.inserts == 4);
        CACHE_CHECK(stats.evictions == 1);
        CACHE_CHECK(stats.hits == 4);
        CACHE_CHECK(stats.misses == 1);
        CACHE_CHECK(stats.size == 3);
        CACHE_CHECK(stats.capacity == 3);
        CACHE_CHECK(stats.HitRate() > 0.79 && stats.HitRate() < 0.81);
    }

    go2cpp_cache_test::Announce("LRU peek does not reorder");
    {
        CacheOptions options;
        options.capacity = 2;
        LruCache<int, int> cache(options);
        cache.Put(1, 10);
        cache.Put(2, 20);
        int value = 0;
        CACHE_CHECK(cache.Peek(1, &value) && value == 10);
        cache.Put(3, 30);  // Peek 不改变顺序，被淘汰的是 1。
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Get(2, &value));
        CACHE_CHECK(cache.Get(3, &value));
        // Peek 不计入命中/未命中。
        const auto stats = cache.Stats();
        CACHE_CHECK(stats.hits == 2);
        CACHE_CHECK(stats.misses == 1);
    }

    go2cpp_cache_test::Announce("LRU update / weight");
    {
        CacheOptions options;
        options.capacity = 2;
        LruCache<int, int> cache(options);
        cache.Put(1, 10, 100);
        cache.Put(2, 20, 200);
        CACHE_CHECK(cache.Weight() == 300);
        cache.Put(1, 11, 50);
        CACHE_CHECK(cache.Size() == 2);
        CACHE_CHECK(cache.Weight() == 250);
        int value = 0;
        CACHE_CHECK(cache.Get(1, &value) && value == 11);
        const auto stats = cache.Stats();
        CACHE_CHECK(stats.inserts == 2);
        CACHE_CHECK(stats.updates == 1);
    }

    go2cpp_cache_test::Announce("LRU erase / clear");
    {
        CacheOptions options;
        options.capacity = 3;
        LruCache<std::string, std::string> cache(options);
        cache.Put("a", "1");
        cache.Put("b", "2");
        std::string value;
        CACHE_CHECK(cache.Erase("a"));
        CACHE_CHECK(!cache.Erase("a"));
        CACHE_CHECK(!cache.Get("a", &value));
        CACHE_CHECK(cache.Get("b", &value) && value == "2");
        CACHE_CHECK(cache.Weight() == 1);

        cache.Clear();
        CACHE_CHECK(cache.Size() == 0);
        CACHE_CHECK(cache.Weight() == 0);
        CACHE_CHECK(!cache.Get("b", &value));
        // Clear 不清空累计统计。
        CACHE_CHECK(cache.Stats().removals == 1);
        CACHE_CHECK(cache.Stats().hits == 1);
    }

    go2cpp_cache_test::Announce("LRU capacity 1 / null out");
    {
        CacheOptions options;
        options.capacity = 1;
        LruCache<int, int> cache(options);
        cache.Put(1, 10);
        cache.Put(2, 20);
        int value = 0;
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Get(2, &value) && value == 20);
        // out 允许为空：只查询是否存在。
        CACHE_CHECK(cache.Get(2, nullptr));
        CACHE_CHECK(!cache.Get(1, nullptr));
    }

    go2cpp_cache_test::Announce("LRU invalid capacity");
    {
        bool threw = false;
        try {
            CacheOptions options;
            options.capacity = 0;
            LruCache<int, int> cache(options);
            (void)cache;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CACHE_CHECK(threw);
    }

    go2cpp_cache_test::Announce("LRU large values");
    {
        CacheOptions options;
        options.capacity = 4;
        LruCache<int, std::string> cache(options);
        for (int i = 0; i < 8; ++i) {
            cache.Put(i, std::string(1024, static_cast<char>('a' + i)));
        }
        CACHE_CHECK(cache.Size() == 4);
        std::string value;
        CACHE_CHECK(cache.Get(7, &value));
        CACHE_CHECK(value.size() == 1024);
        CACHE_CHECK(value[0] == static_cast<char>('a' + 7));
    }
}
