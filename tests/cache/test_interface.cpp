/**
 * @file test_interface.cpp
 * @brief 统一接口、工厂、线程安全包装、分片与并发一致性。
 */
#include "cache_test_support.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/cache/factory.hpp"

using go2cpp::cache::Cache;
using go2cpp::cache::CacheOptions;
using go2cpp::cache::Policy;

namespace {

const Policy kAllPolicies[] = {
    Policy::kLru,   Policy::kLruK, Policy::kTwoQueue, Policy::kTinyLfu,
    Policy::kGdsf,  Policy::kArc,  Policy::kClock,
};

}  // namespace

void RunInterfaceTests() {
    go2cpp_cache_test::Announce("interface policy names / factory");

    CACHE_CHECK(std::string(go2cpp::cache::PolicyName(Policy::kLru)) == "LRU");
    CACHE_CHECK(std::string(go2cpp::cache::PolicyName(Policy::kLruK)) ==
                "LRU-K(2)");
    CACHE_CHECK(std::string(go2cpp::cache::PolicyName(Policy::kTwoQueue)) ==
                "2Q");
    CACHE_CHECK(std::string(go2cpp::cache::PolicyName(Policy::kTinyLfu)) ==
                "W-TinyLFU");
    CACHE_CHECK(std::string(go2cpp::cache::PolicyName(Policy::kGdsf)) ==
                "GDSF");
    CACHE_CHECK(std::string(go2cpp::cache::PolicyName(Policy::kArc)) == "ARC");
    CACHE_CHECK(std::string(go2cpp::cache::PolicyName(Policy::kClock)) ==
                "CLOCK");

    {
        CacheOptions options;
        options.capacity = 16;
        for (Policy policy : kAllPolicies) {
            auto cache = go2cpp::cache::MakeCache<int, std::string>(policy,
                                                                    options);
            CACHE_REQUIRE(cache != nullptr);
            CACHE_CHECK(std::string(cache->Name()) ==
                        go2cpp::cache::PolicyName(policy));
            cache->Put(1, "one");
            std::string value;
            CACHE_CHECK(cache->Get(1, &value) && value == "one");
            CACHE_CHECK(cache->Erase(1));
            CACHE_CHECK(!cache->Get(1, &value));
            CACHE_CHECK(cache->Capacity() == 16);
        }
    }

    go2cpp_cache_test::Announce("interface polymorphic use / weight / peek");
    {
        CacheOptions options;
        options.capacity = 4;
        std::unique_ptr<Cache<int, std::string>> cache =
            go2cpp::cache::MakeCache<int, std::string>(Policy::kLru, options);
        cache->Put(1, "a", 64);
        cache->Put(2, "b", 32);
        CACHE_CHECK(cache->Weight() == 96);
        std::string value;
        CACHE_CHECK(cache->Peek(1, &value) && value == "a");
        CACHE_CHECK(cache->Stats().hits == 0);  // Peek 不计命中。
        CACHE_CHECK(cache->Get(1, &value));
        CACHE_CHECK(cache->Stats().hits == 1);
        CACHE_CHECK(!cache->Peek(3, &value));
    }

    go2cpp_cache_test::Announce("ThreadSafeCache concurrent get/put");
    {
        CacheOptions options;
        options.capacity = 256;
        auto cache = go2cpp::cache::MakeThreadSafeCache<std::uint64_t,
                                                        std::uint64_t>(
            Policy::kLru, options);
        constexpr int kThreads = 4;
        constexpr int kOpsPerThread = 20000;
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int thread = 0; thread < kThreads; ++thread) {
            workers.emplace_back([&cache, thread]() {
                std::uint64_t value = 0;
                for (int i = 0; i < kOpsPerThread; ++i) {
                    const std::uint64_t key =
                        static_cast<std::uint64_t>(thread) * 1000U +
                        static_cast<std::uint64_t>(i % 500);
                    if (!cache->Get(key, &value)) {
                        cache->Put(key, key);
                    }
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        const auto stats = cache->Stats();
        CACHE_CHECK(stats.hits + stats.misses ==
                    static_cast<std::uint64_t>(kThreads * kOpsPerThread));
        CACHE_CHECK(cache->Size() <= options.capacity);
        CACHE_CHECK(stats.size <= options.capacity);
    }

    go2cpp_cache_test::Announce("ShardedCache concurrent");
    {
        CacheOptions options;
        options.capacity = 512;
        go2cpp::cache::ShardedCache<std::uint64_t, std::uint64_t> cache(
            Policy::kTinyLfu, options, 8);
        CACHE_CHECK(cache.Capacity() == 512);
        std::vector<std::thread> workers;
        for (int thread = 0; thread < 4; ++thread) {
            workers.emplace_back([&cache, thread]() {
                std::uint64_t value = 0;
                for (int i = 0; i < 5000; ++i) {
                    const std::uint64_t key =
                        static_cast<std::uint64_t>(thread) * 100000U +
                        static_cast<std::uint64_t>(i % 2000);
                    if (!cache.Get(key, &value)) {
                        cache.Put(key, key);
                    }
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        const auto stats = cache.Stats();
        CACHE_CHECK(stats.hits + stats.misses == 20000);
        CACHE_CHECK(cache.Size() <= cache.Capacity());
        CACHE_CHECK(cache.Describe().find("shards=8") != std::string::npos);
    }

    go2cpp_cache_test::Announce("factory invalid capacity");
    {
        CacheOptions options;
        options.capacity = 0;
        for (Policy policy : kAllPolicies) {
            bool threw = false;
            try {
                auto cache = go2cpp::cache::MakeCache<int, int>(policy, options);
                (void)cache;
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            CACHE_CHECK(threw);
        }
    }

    go2cpp_cache_test::Announce("ThreadSafeCache null inner");
    {
        bool threw = false;
        try {
            go2cpp::cache::ThreadSafeCache<int, int> cache(
                std::unique_ptr<Cache<int, int>>{});
            (void)cache;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CACHE_CHECK(threw);
    }
}
