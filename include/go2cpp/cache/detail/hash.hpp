/**
 * @file detail/hash.hpp
 * @brief 缓存模块内部使用的小型哈希工具。
 *
 * @details `SplitMix64` 是 Steele/Lea/Flood 的 mix 函数，用作 TinyLFU
 *          Count-Min Sketch 的多路哈希：输入是用户 key 的 `std::hash`
 *          结果，输出位模式足够均匀，且每个 seed 得到独立的一路。
 *          它只依赖整数运算，编译期可内联，不引入任何外部依赖。
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace go2cpp {
namespace cache {
namespace detail {

/** @brief SplitMix64 的 64 位混合函数（双射，雪崩性好）。 */
inline std::uint64_t SplitMix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}

/** @brief 把任意 64 位值折成一个可用的哈希（再混合一次 seed）。 */
inline std::uint64_t MixHash(std::uint64_t hash, std::uint64_t seed) noexcept {
    return SplitMix64(hash ^ seed);
}

/** @brief 向上取整到 2 的幂；输入 0 返回 1。 */
inline std::size_t NextPowerOfTwo(std::size_t value) noexcept {
    if (value <= 1) {
        return 1;
    }
    std::size_t result = 1;
    while (result < value) {
        result <<= 1U;
    }
    return result;
}

}  // namespace detail
}  // namespace cache
}  // namespace go2cpp
