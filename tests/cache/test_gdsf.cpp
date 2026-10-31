/**
 * @file test_gdsf.cpp
 * @brief GDSF：加权容量、cost/size×freq 淘汰、惰性堆与更新语义。
 */
#include "cache_test_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include "go2cpp/cache/gdsf.hpp"

using go2cpp::cache::CacheOptions;
using go2cpp::cache::GdsfCache;

void RunGdsfTests() {
    go2cpp_cache_test::Announce("GDSF entry capacity / frequency ordering");

    {
        CacheOptions options;
        options.capacity = 3;
        GdsfCache<int, int> cache(options);
        cache.Put(1, 10);
        cache.Put(2, 20);
        cache.Put(3, 30);
        int value = 0;
        CACHE_CHECK(cache.Get(1, &value));  // 频率 2，key_value 更高。
        cache.Put(4, 40);                   // 淘汰 key_value 最小的 2 或 3。
        CACHE_CHECK(cache.Size() == 3);
        CACHE_CHECK(cache.Get(1, &value) && value == 10);
        CACHE_CHECK(cache.Get(4, &value) && value == 40);
        CACHE_CHECK(cache.Stats().evictions == 1);
    }

    go2cpp_cache_test::Announce("GDSF weight budget evicts large cold object");
    {
        CacheOptions options;
        options.capacity = 0;  // 只按字节预算。
        options.max_weight = 100;
        GdsfCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10, 60);  // 60B
        cache.Put(2, 20, 30);  // 90B
        CACHE_CHECK(cache.Weight() == 90);
        cache.Put(3, 30, 20);  // 需要腾 10B：H 最小的 1（1/60 < 1/30）。
        CACHE_CHECK(cache.Weight() <= 100);
        CACHE_CHECK(cache.Size() == 2);
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Get(2, &value) && value == 20);
        CACHE_CHECK(cache.Get(3, &value) && value == 30);
    }

    go2cpp_cache_test::Announce("GDSF cost/size x freq protects hot small object");
    {
        CacheOptions options;
        options.capacity = 0;
        options.max_weight = 100;
        GdsfCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10, 50);  // 1/50 = 0.02
        for (int i = 0; i < 5; ++i) {
            CACHE_CHECK(cache.Get(1, &value));
        }
        // freq = 6 -> key_value = 6/50 = 0.12
        cache.Put(2, 20, 50);  // 1/50 = 0.02
        CACHE_CHECK(cache.Weight() == 100);
        cache.Put(3, 30, 10);  // 需要 10B：淘汰 2（0.02 < 0.12）。
        CACHE_CHECK(!cache.Get(2, &value));
        CACHE_CHECK(cache.Get(1, &value) && value == 10);
        CACHE_CHECK(cache.Get(3, &value) && value == 30);
    }

    go2cpp_cache_test::Announce("GDSF update / oversize rejection");
    {
        CacheOptions options;
        options.capacity = 2;
        options.max_weight = 100;
        GdsfCache<int, int> cache(options);
        cache.Put(1, 10, 10);
        cache.Put(1, 11, 20);
        CACHE_CHECK(cache.Size() == 1);
        CACHE_CHECK(cache.Weight() == 20);
        int value = 0;
        CACHE_CHECK(cache.Get(1, &value) && value == 11);
        CACHE_CHECK(cache.Stats().updates == 1);

        cache.Put(1, 12, 200);  // 超过 max_weight，拒绝更新且保留旧值。
        CACHE_CHECK(cache.Stats().rejections == 1);
        CACHE_CHECK(cache.Peek(1, &value) && value == 11);
        CACHE_CHECK(cache.Weight() == 20);
    }

    go2cpp_cache_test::Announce("GDSF erase / clear / lazy heap bound");
    {
        CacheOptions options;
        options.capacity = 16;
        GdsfCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10, 5);
        cache.Put(2, 20, 5);
        CACHE_CHECK(cache.Erase(1));
        CACHE_CHECK(!cache.Erase(1));
        CACHE_CHECK(cache.Weight() == 5);
        CACHE_CHECK(cache.Erase(2));

        cache.Put(3, 30, 5);
        for (int i = 0; i < 1000; ++i) {
            CACHE_CHECK(cache.Get(3, &value));
        }
        const auto stats = cache.Stats();
        CACHE_CHECK(stats.ghost_size <= cache.Size() * 4 + 64);
        CACHE_CHECK(stats.hits == 1000);
        CACHE_CHECK(cache.Describe().find("L=") != std::string::npos);

        cache.Clear();
        CACHE_CHECK(cache.Size() == 0);
        CACHE_CHECK(cache.Weight() == 0);
        CACHE_CHECK(cache.Describe().find("size=0") != std::string::npos);
    }

    go2cpp_cache_test::Announce("GDSF invalid options");
    {
        bool threw = false;
        try {
            CacheOptions options;
            options.capacity = 0;
            options.max_weight = 0;
            GdsfCache<int, int> cache(options);
            (void)cache;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CACHE_CHECK(threw);

        threw = false;
        try {
            CacheOptions options;
            options.capacity = 10;
            options.fetch_cost = 0.0;
            GdsfCache<int, int> cache(options);
            (void)cache;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CACHE_CHECK(threw);

        CacheOptions options;
        options.capacity = 4;
        GdsfCache<int, int> cache(options);
        CACHE_CHECK(std::string(cache.Name()) == "GDSF");
        CACHE_CHECK(cache.Capacity() == 4);
    }
}
