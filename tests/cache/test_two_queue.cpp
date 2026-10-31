/**
 * @file test_two_queue.cpp
 * @brief 2Q：A1in FIFO、A1out ghost 提升、Am LRU 与统计。
 */
#include "cache_test_support.hpp"

#include <stdexcept>
#include <string>

#include "go2cpp/cache/two_queue.hpp"

using go2cpp::cache::CacheOptions;
using go2cpp::cache::TwoQueueCache;

void RunTwoQueueTests() {
    go2cpp_cache_test::Announce("2Q a1in -> ghost -> am promotion");

    {
        CacheOptions options;
        options.capacity = 2;
        TwoQueueCache<int, int> cache(options);
        cache.Put(1, 10);
        cache.Put(2, 20);

        int value = 0;
        cache.Put(3, 30);  // A1in 满，尾部 1 进入 A1out。
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Stats().ghost_hits == 1);

        cache.Put(1, 11);  // ghost 命中后的 Put 直接进 Am。
        CACHE_CHECK(cache.Get(1, &value) && value == 11);
        CACHE_CHECK(cache.Get(2, &value) || cache.Get(3, &value));
        CACHE_CHECK(cache.Size() == 2);
    }

    go2cpp_cache_test::Announce("2Q a1in is fifo (hit does not reorder)");
    {
        CacheOptions options;
        options.capacity = 4;
        TwoQueueCache<int, int> cache(options);
        cache.Put(1, 10);
        cache.Put(2, 20);
        cache.Put(3, 30);
        cache.Put(4, 40);
        int value = 0;
        CACHE_CHECK(cache.Get(1, &value));
        cache.Put(5, 50);  // 1 仍在 A1in 尾部，被淘汰到 ghost。
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Peek(5, &value));
    }

    go2cpp_cache_test::Announce("2Q am lru eviction");
    {
        CacheOptions options;
        options.capacity = 2;
        TwoQueueCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10);
        cache.Put(2, 20);
        cache.Put(3, 30);  // 1 -> ghost。
        CACHE_CHECK(!cache.Get(1, &value));
        cache.Put(1, 11);  // 1 -> Am；2 -> ghost；A1in 只剩 3。
        CACHE_CHECK(cache.Get(1, &value) && value == 11);
        CACHE_CHECK(!cache.Get(2, &value));  // 2 在 ghost。
        cache.Put(2, 21);                    // 2 -> Am，Am = [2, 1]。
        CACHE_CHECK(cache.Get(1, &value));   // Am = [1, 2]。
        CACHE_CHECK(cache.Get(2, &value));   // Am = [2, 1]。
        cache.Put(4, 40);  // A1in 已空 -> 淘汰 Am 尾（1）。
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Get(2, &value) && value == 21);
        CACHE_CHECK(cache.Get(4, &value) && value == 40);
    }

    go2cpp_cache_test::Announce("2Q ghost capacity");
    {
        CacheOptions options;
        options.capacity = 2;
        TwoQueueCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10);
        cache.Put(2, 20);
        cache.Put(3, 30);  // 1 -> ghost。
        cache.Put(4, 40);  // 2 -> ghost，ghost 容量 1，1 被挤出。
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Stats().ghost_hits == 0);
        CACHE_CHECK(!cache.Get(2, &value));
        CACHE_CHECK(cache.Stats().ghost_hits == 1);
    }

    go2cpp_cache_test::Announce("2Q erase / clear / stats");
    {
        CacheOptions options;
        options.capacity = 3;
        TwoQueueCache<std::string, std::string> cache(options);
        cache.Put("a", "1");
        cache.Put("b", "2");
        std::string value;
        CACHE_CHECK(cache.Erase("a"));
        CACHE_CHECK(!cache.Erase("a"));
        CACHE_CHECK(cache.Get("b", &value) && value == "2");
        cache.Clear();
        CACHE_CHECK(cache.Size() == 0);
        CACHE_CHECK(cache.Weight() == 0);
        CACHE_CHECK(!cache.Get("b", &value));
        CACHE_CHECK(cache.Stats().removals == 1);
    }

    go2cpp_cache_test::Announce("2Q capacity 1 / weight");
    {
        CacheOptions options;
        options.capacity = 1;
        TwoQueueCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10, 7);
        CACHE_CHECK(cache.Get(1, &value) && value == 10);
        CACHE_CHECK(cache.Weight() == 7);
        cache.Put(2, 20, 3);
        CACHE_CHECK(cache.Size() == 1);
        CACHE_CHECK(cache.Weight() == 3);
        CACHE_CHECK(!cache.Get(1, &value));
        CACHE_CHECK(cache.Get(2, &value));
    }

    go2cpp_cache_test::Announce("2Q invalid capacity / describe");
    {
        bool threw = false;
        try {
            CacheOptions options;
            options.capacity = 0;
            TwoQueueCache<int, int> cache(options);
            (void)cache;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CACHE_CHECK(threw);

        CacheOptions options;
        options.capacity = 8;
        TwoQueueCache<int, int> cache(options);
        CACHE_CHECK(cache.Describe().find("2Q{") == 0);
        CACHE_CHECK(std::string(cache.Name()) == "2Q");
    }
}
