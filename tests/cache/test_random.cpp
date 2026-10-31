/**
 * @file test_random.cpp
 * @brief 随机对拍：值一致性、容量不变量、统计计数与 Clear/Erase 语义。
 *
 * @details 对拍模型只做「弱一致」校验：缓存可以随时淘汰任意条目，所以
 *          不能要求「Put 过的 key 一定命中」；但可以要求「命中时返回的
 *          值必须等于最后一次 Put 的值」——这能抓住索引/链表错位、
 *          悬垂迭代器、值拷贝错位等绝大多数实现错误。
 */
#include "cache_test_support.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>

#include "go2cpp/cache/factory.hpp"

using go2cpp::cache::CacheOptions;
using go2cpp::cache::Policy;

namespace {

template <class CacheT>
void FuzzUint64(CacheT& cache, std::size_t capacity, std::uint64_t seed,
                int steps) {
    std::mt19937_64 rng(seed);
    std::unordered_map<std::uint64_t, std::uint64_t> shadow;
    std::uint64_t get_count = 0;

    for (int step = 0; step < steps; ++step) {
        const std::uint64_t key = rng() % (capacity * 8U + 16U);
        const int op = static_cast<int>(rng() % 100U);
        if (op < 40) {
            std::uint64_t value = 0;
            ++get_count;
            if (cache.Get(key, &value)) {
                auto it = shadow.find(key);
                CACHE_CHECK(it != shadow.end());
                if (it != shadow.end()) {
                    CACHE_CHECK(it->second == value);
                }
            } else {
                const std::uint64_t fill = key * 3U + 1U;
                cache.Put(key, fill);
                shadow[key] = fill;
            }
        } else if (op < 72) {
            const std::uint64_t value = rng();
            cache.Put(key, value, 1U + (rng() % 4U));
            shadow[key] = value;
        } else if (op < 88) {
            const bool erased = cache.Erase(key);
            if (erased) {
                std::uint64_t value = 0;
                ++get_count;
                CACHE_CHECK(!cache.Get(key, &value));
            }
            shadow.erase(key);
        } else if (op < 96) {
            std::uint64_t value = 0;
            if (cache.Peek(key, &value)) {
                auto it = shadow.find(key);
                CACHE_CHECK(it != shadow.end());
                if (it != shadow.end()) {
                    CACHE_CHECK(it->second == value);
                }
            }
        } else {
            cache.Clear();
            shadow.clear();
        }

        const auto stats = cache.Stats();
        CACHE_CHECK(cache.Size() <= capacity + stats.ghost_size);
        CACHE_CHECK(stats.size <= capacity + stats.ghost_size);
        CACHE_CHECK(stats.hits + stats.misses == get_count);
        CACHE_CHECK(stats.capacity > 0);
    }
}

}  // namespace

void RunRandomTests() {
    go2cpp_cache_test::Announce("random fuzz: 7 policies x 20k ops");

    CacheOptions options;
    options.capacity = 32;
    for (Policy policy : {Policy::kLru, Policy::kLruK, Policy::kTwoQueue,
                          Policy::kTinyLfu, Policy::kGdsf, Policy::kArc,
                          Policy::kClock}) {
        auto cache = go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
            policy, options);
        FuzzUint64(*cache, options.capacity, 0xC0FFEEU + 7U *
                                                     static_cast<std::uint64_t>(
                                                         policy),
                   20000);
        std::printf("[cache]   fuzz %s done: %s\n", cache->Name(),
                    cache->Describe().c_str());
    }

    go2cpp_cache_test::Announce("random fuzz: small capacity 1..7");
    for (std::size_t capacity = 1; capacity <= 7; ++capacity) {
        CacheOptions small;
        small.capacity = capacity;
        for (Policy policy : {Policy::kLru, Policy::kTwoQueue,
                              Policy::kTinyLfu, Policy::kArc,
                              Policy::kClock}) {
            auto cache =
                go2cpp::cache::MakeCache<std::uint64_t, std::uint64_t>(
                    policy, small);
            FuzzUint64(*cache, capacity, 1000U + capacity, 3000);
        }
    }

    go2cpp_cache_test::Announce("random fuzz: string keys/values");
    {
        CacheOptions strings;
        strings.capacity = 24;
        go2cpp::cache::LruCache<std::string, std::string> lru(strings);
        std::mt19937_64 rng(42);
        std::unordered_map<std::string, std::string> shadow;
        for (int step = 0; step < 5000; ++step) {
            const std::string key =
                "key-" + std::to_string(rng() % 80U);
            if (step % 3 == 0) {
                std::string value;
                if (lru.Get(key, &value)) {
                    auto it = shadow.find(key);
                    CACHE_CHECK(it != shadow.end());
                    if (it != shadow.end()) {
                        CACHE_CHECK(it->second == value);
                    }
                } else {
                    shadow[key] = key;
                    lru.Put(key, key);
                }
            } else if (step % 3 == 1) {
                const std::string value = key + "-v";
                lru.Put(key, value);
                shadow[key] = value;
            } else {
                lru.Erase(key);
                shadow.erase(key);
            }
            CACHE_CHECK(lru.Size() <= strings.capacity);
        }
    }

    go2cpp_cache_test::Announce("random fuzz: thread-safe wrapper");
    {
        CacheOptions thread_options;
        thread_options.capacity = 64;
        auto cache = go2cpp::cache::MakeThreadSafeCache<std::uint64_t,
                                                        std::uint64_t>(
            Policy::kArc, thread_options);
        FuzzUint64(*cache, thread_options.capacity, 99, 5000);
    }
}
