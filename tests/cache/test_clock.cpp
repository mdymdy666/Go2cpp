/**
 * @file test_clock.cpp
 * @brief CLOCK：二次机会、引用位、空洞复用与统计。
 */
#include "cache_test_support.hpp"

#include <stdexcept>
#include <string>

#include "go2cpp/cache/clock.hpp"

using go2cpp::cache::CacheOptions;
using go2cpp::cache::ClockCache;

void RunClockTests() {
    go2cpp_cache_test::Announce("CLOCK basic second chance");

    {
        CacheOptions options;
        options.capacity = 2;
        ClockCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10);
        cache.Put(2, 20);
        CACHE_CHECK(cache.Get(1, &value));  // 1 获得引用位。
        cache.Put(3, 30);                   // 2 无引用位，被淘汰。
        CACHE_CHECK(cache.Get(1, &value) && value == 10);
        CACHE_CHECK(!cache.Get(2, &value));
        CACHE_CHECK(cache.Get(3, &value) && value == 30);
        CACHE_CHECK(cache.Size() == 2);
        CACHE_CHECK(cache.Stats().evictions == 1);
    }

    go2cpp_cache_test::Announce("CLOCK sweep clears reference bits");
    {
        CacheOptions options;
        options.capacity = 2;
        ClockCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10);
        cache.Put(2, 20);
        CACHE_CHECK(cache.Get(1, &value));
        CACHE_CHECK(cache.Get(2, &value));
        cache.Put(3, 30);  // 两个引用位都要清一次，然后淘汰其中一个。
        const bool one = cache.Get(1, &value);
        const bool two = cache.Get(2, &value);
        CACHE_CHECK(one != two);  // 恰好保留一个。
        CACHE_CHECK(cache.Get(3, &value));
        CACHE_CHECK(cache.Size() == 2);
    }

    go2cpp_cache_test::Announce("CLOCK erase reuses slot");
    {
        CacheOptions options;
        options.capacity = 2;
        ClockCache<int, int> cache(options);
        cache.Put(1, 10);
        cache.Put(2, 20);
        CACHE_CHECK(cache.Erase(1));
        CACHE_CHECK(!cache.Erase(1));
        int value = 0;
        CACHE_CHECK(!cache.Get(1, &value));
        cache.Put(3, 30);
        CACHE_CHECK(cache.Get(2, &value) && value == 20);
        CACHE_CHECK(cache.Get(3, &value) && value == 30);
        CACHE_CHECK(cache.Weight() == 2);
    }

    go2cpp_cache_test::Announce("CLOCK update / clear / weight");
    {
        CacheOptions options;
        options.capacity = 3;
        ClockCache<int, int> cache(options);
        cache.Put(1, 10, 100);
        cache.Put(2, 20, 50);
        CACHE_CHECK(cache.Weight() == 150);
        cache.Put(1, 11, 25);
        int value = 0;
        CACHE_CHECK(cache.Get(1, &value) && value == 11);
        CACHE_CHECK(cache.Weight() == 75);
        CACHE_CHECK(cache.Stats().updates == 1);

        cache.Clear();
        CACHE_CHECK(cache.Size() == 0);
        CACHE_CHECK(cache.Weight() == 0);
        CACHE_CHECK(!cache.Get(2, &value));
        CACHE_CHECK(cache.Describe().find("CLOCK{") == 0);
        CACHE_CHECK(std::string(cache.Name()) == "CLOCK");
    }

    go2cpp_cache_test::Announce("CLOCK capacity 1 / invalid capacity");
    {
        CacheOptions options;
        options.capacity = 1;
        ClockCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10);
        CACHE_CHECK(cache.Get(1, &value));
        cache.Put(2, 20);
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Get(2, &value));
        CACHE_CHECK(cache.Size() == 1);

        bool threw = false;
        try {
            CacheOptions bad;
            bad.capacity = 0;
            ClockCache<int, int> invalid(bad);
            (void)invalid;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CACHE_CHECK(threw);
    }
}
