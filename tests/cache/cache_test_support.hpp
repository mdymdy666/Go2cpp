/**
 * @file cache_test_support.hpp
 * @brief `go2cpp_cache_test` 的共享断言、计时、读穿透驱动器。
 *
 * @details 测试约定与项目其它独立测试一致：每个测试段先打印
 *          `[cache] 名称`；断言失败累计到 `g_failures`，`main` 返回非零。
 *          `CacheRequire` 用于「继续跑会掩盖真实问题」的致命断言。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "go2cpp/cache/cache.hpp"
#include "go2cpp/cache/workload.hpp"

namespace go2cpp_cache_test {

/** @brief 累计失败数。 */
inline std::atomic<int> g_failures{0};

/** @brief 记录一次断言失败，不中断。 */
inline void Check(bool condition, const char* expression, const char* file,
                  int line) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s:%d: cache check failed: %s\n", file, line,
                 expression);
}

/** @brief 记录一次致命失败并立即中止。 */
[[noreturn]] inline void Fail(const char* expression, const char* file,
                              int line) {
    std::fprintf(stderr, "%s:%d: cache REQUIRE failed: %s\n", file, line,
                 expression);
    std::fflush(stderr);
    std::abort();
}

/** @brief 打印测试段名。 */
inline void Announce(const char* name) {
    std::printf("[cache] %s\n", name);
    std::fflush(stdout);
}

using Clock = std::chrono::steady_clock;

/** @brief 从 start 到现在的毫秒数。 */
inline double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
}

/** @brief 读穿透结果：命中/未命中次数。 */
struct ReadThroughResult {
    std::size_t hits = 0;
    std::size_t misses = 0;

    double HitRate() const {
        const std::size_t total = hits + misses;
        return total == 0 ? 0.0
                          : static_cast<double>(hits) /
                                static_cast<double>(total);
    }
};

/**
 * @brief 模拟真实缓存层的读穿透：Get 未命中时立刻 Put 回缓存。
 *
 * @details 这是缓存最常用的接入方式（cache-aside / read-through），也是
 *          「扫描污染」能发生的真实条件：应用并不知道当前请求是热点还是
 *          扫描，miss 后一律填充。LRU-K / 2Q / TinyLFU 的抗扫描能力就是
 *          在这种条件下才有意义。
 */
template <class K, class V, class CacheT>
ReadThroughResult RunReadThrough(CacheT& cache,
                                 const std::vector<go2cpp::cache::CacheRequest>&
                                     requests) {
    ReadThroughResult result;
    for (const go2cpp::cache::CacheRequest& request : requests) {
        V value{};
        if (cache.Get(static_cast<K>(request.key), &value)) {
            ++result.hits;
        } else {
            ++result.misses;
            cache.Put(static_cast<K>(request.key),
                      static_cast<V>(request.key), request.weight);
        }
    }
    return result;
}

/** @brief 校验 size/weight 不变量与统计计数的一致性。 */
template <class K, class V, class CacheT>
void CheckInvariants(CacheT& cache, std::size_t expected_capacity,
                     const char* label, const char* file, int line) {
    const go2cpp::cache::CacheStats stats = cache.Stats();
    Check(cache.Size() <= expected_capacity, label, file, line);
    Check(stats.size == cache.Size(), label, file, line);
    Check(stats.hits + stats.misses == stats.Total(), label, file, line);
    Check(stats.capacity == expected_capacity, label, file, line);
}

}  // namespace go2cpp_cache_test

/** @brief 非致命断言：失败累计，继续执行。 */
#define CACHE_CHECK(expr) \
    ::go2cpp_cache_test::Check((expr), #expr, __FILE__, __LINE__)

/** @brief 致命断言：失败立即 abort。 */
#define CACHE_REQUIRE(expr)                                      \
    do {                                                         \
        if (!(expr)) {                                           \
            ::go2cpp_cache_test::Fail(#expr, __FILE__, __LINE__); \
        }                                                        \
    } while (false)
