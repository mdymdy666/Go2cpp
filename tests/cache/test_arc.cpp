/**
 * @file test_arc.cpp
 * @brief ARC：T1/T2/B1/B2、ghost 自适应 p、扫描后恢复与不变量。
 */
#include "cache_test_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include "go2cpp/cache/arc.hpp"

using go2cpp::cache::ArcCache;
using go2cpp::cache::CacheOptions;

namespace {

double ParseP(const std::string& describe) {
    const std::string key = "p=";
    const std::size_t pos = describe.find(key);
    if (pos == std::string::npos) {
        return -1.0;
    }
    return std::stod(describe.substr(pos + key.size()));
}

}  // namespace

void RunArcTests() {
    go2cpp_cache_test::Announce("ARC t1/t2/ghost promotion");

    {
        CacheOptions options;
        options.capacity = 2;
        ArcCache<int, int> cache(options);
        cache.Put(1, 10);
        cache.Put(2, 20);
        int value = 0;
        CACHE_CHECK(cache.Get(1, &value));  // T1 -> T2。
        cache.Put(3, 30);                   // 2 -> B1，3 -> T1。
        CACHE_CHECK(cache.Size() == 2);

        CACHE_CHECK(!cache.Get(2, &value));  // B1 命中，p 增大。
        CACHE_CHECK(cache.Stats().ghost_hits == 1);
        CACHE_CHECK(ParseP(cache.Describe()) > 0.0);

        cache.Put(2, 21);  // pending 提升到 T2。
        CACHE_CHECK(cache.Get(2, &value) && value == 21);
        CACHE_CHECK(cache.Get(1, &value) || cache.Get(3, &value));
        CACHE_CHECK(cache.Size() == 2);
    }

    go2cpp_cache_test::Announce("ARC adaptive p (b1 up / b2 down)");
    {
        CacheOptions options;
        options.capacity = 4;
        ArcCache<int, int> cache(options);
        int value = 0;
        for (int key = 0; key < 4; ++key) {
            cache.Put(key, key * 10);
        }
        for (int key = 0; key < 4; ++key) {
            CACHE_CHECK(cache.Get(key, &value));  // 全部进入 T2。
        }
        // T1 为空、T2 4 条；插入两个新 key 会把 T2 尾部挤到 B2。
        cache.Put(4, 40);
        cache.Put(5, 50);
        const double p0 = ParseP(cache.Describe());
        CACHE_CHECK(p0 == 0.0);
        CACHE_CHECK(cache.Size() == 4);

        // B2 命中：p 应该下降（下界 0）。
        CACHE_CHECK(!cache.Get(0, &value));  // 0 大概率在 B2。
        const double p1 = ParseP(cache.Describe());
        CACHE_CHECK(p1 <= p0);
        CACHE_CHECK(cache.Stats().ghost_hits >= 1);
    }

    go2cpp_cache_test::Announce("ARC scan resistance");
    {
        CacheOptions options;
        options.capacity = 4;
        ArcCache<std::uint64_t, std::uint64_t> cache(options);
        std::uint64_t value = 0;
        for (std::uint64_t key = 0; key < 4; ++key) {
            cache.Put(key, key * 10);
        }
        for (std::uint64_t key = 0; key < 4; ++key) {
            CACHE_CHECK(cache.Get(key, &value));
        }
        CACHE_CHECK(cache.Size() == 4);

        for (std::uint64_t i = 0; i < 200; ++i) {
            const std::uint64_t key = 100000 + i;
            if (!cache.Get(key, &value)) {
                cache.Put(key, key);
            }
        }
        // 扫描期间 T1 轮转，T2 的 1/2/3 应保留（0 被第一个扫描 key 挤出）。
        std::size_t hot_hits = 0;
        for (std::uint64_t key = 1; key < 4; ++key) {
            if (cache.Get(key, &value)) {
                ++hot_hits;
            }
        }
        CACHE_CHECK(hot_hits >= 3);
        CACHE_CHECK(cache.Size() <= 4);
        const auto stats = cache.Stats();
        CACHE_CHECK(stats.ghost_size <= 4);
        CACHE_CHECK(cache.Describe().find("pending=") != std::string::npos);
    }

    go2cpp_cache_test::Announce("ARC erase / clear / weights");
    {
        CacheOptions options;
        options.capacity = 3;
        ArcCache<int, int> cache(options);
        cache.Put(1, 10, 100);
        cache.Put(2, 20, 50);
        CACHE_CHECK(cache.Weight() == 150);
        int value = 0;
        CACHE_CHECK(cache.Get(1, &value));  // T2。
        CACHE_CHECK(cache.Erase(1));
        CACHE_CHECK(cache.Weight() == 50);
        CACHE_CHECK(!cache.Erase(1));
        CACHE_CHECK(cache.Get(2, &value) && value == 20);

        cache.Clear();
        CACHE_CHECK(cache.Size() == 0);
        CACHE_CHECK(cache.Weight() == 0);
        CACHE_CHECK(cache.Stats().removals == 1);
        CACHE_CHECK(cache.Describe().find("ARC{") == 0);
    }

    go2cpp_cache_test::Announce("ARC capacity 1 / invalid capacity");
    {
        CacheOptions options;
        options.capacity = 1;
        ArcCache<int, int> cache(options);
        int value = 0;
        cache.Put(1, 10);
        CACHE_CHECK(cache.Get(1, &value));
        cache.Put(2, 20);  // 1 -> B1，2 -> T1。
        CACHE_CHECK(cache.Size() == 1);
        CACHE_CHECK(!cache.Get(1, &value));  // B1 命中（miss）。
        cache.Put(1, 11);
        CACHE_CHECK(cache.Get(1, &value) && value == 11);
        CACHE_CHECK(cache.Size() == 1);

        bool threw = false;
        try {
            CacheOptions bad;
            bad.capacity = 0;
            ArcCache<int, int> invalid(bad);
            (void)invalid;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CACHE_CHECK(threw);
    }
}
