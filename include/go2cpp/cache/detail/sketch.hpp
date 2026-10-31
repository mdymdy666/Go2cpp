/**
 * @file detail/sketch.hpp
 * @brief TinyLFU 的两件核心数据结构：4-bit Count-Min Sketch 与 Bloom 门卫。
 *
 * @details W-TinyLFU（Caffeine，2015）用「频率素描 + 准入过滤器」判断一个
 *          候选条目是否值得进入主缓存。直接对每个 key 维护精确计数在内
 *          存上不可行（key 数量远大于缓存条目），因此：
 *
 *          - **Count-Min Sketch**：`d = 4` 行、每行 `w` 个计数器，插入时
 *            四路哈希各 +1，查询取四路最小值。只高估、不低估，误差上界
 *            由 `e / w` 决定（Cormode & Muthukrishnan, 2005）。
 *          - **4-bit 饱和计数**：Caffeine 用 4 bit 存一个计数器，最大值 15。
 *            达到 15 后不再增加（饱和），并周期性把所有计数器右移一位
 *            （halve），让历史频率指数衰减——这就是「Window-TinyLFU」里的
 *            老化机制。
 *          - **Doorkeeper（Bloom 门卫）**：每个 key 第一次出现时只置位
 *            Bloom，不写 sketch；第二次出现才真正增加计数。一次性扫描
 *            因此几乎不污染频率素描。
 *
 *          这里的实现与 Caffeine 的 `FrequencySketch` 同构，区别只在
 *          哈希函数（SplitMix64 而非 `mix64`）与 Bloom 位宽选择。
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "go2cpp/cache/detail/hash.hpp"

namespace go2cpp {
namespace cache {
namespace detail {

/**
 * @brief 4 行 4-bit Count-Min Sketch。
 *
 * @note 非线程安全；由外层缓存（或 ThreadSafeCache）保证串行访问。
 */
class CountMinSketch4 {
public:
    static constexpr std::size_t kRows = 4;
    static constexpr std::uint8_t kMaxCount = 15;

    /**
     * @param capacity 预期的缓存条目数，用于推导宽度。
     * @param seed 哈希种子。
     */
    CountMinSketch4(std::size_t capacity, std::uint64_t seed)
        : width_(ChooseWidth(capacity)),
          stride_(width_ / 2U),
          table_(static_cast<std::size_t>(kRows) * stride_, 0U),
          seed_(seed) {}

    /** @brief 对 key 的哈希值计数 +1（4 路，饱和于 15）。 */
    void Increment(std::uint64_t hash) noexcept {
        for (std::size_t row = 0; row < kRows; ++row) {
            const std::size_t index = Index(hash, row);
            std::uint8_t& byte = table_[row * stride_ + index / 2U];
            const bool high = (index & 1U) != 0U;
            const std::uint8_t nibble =
                high ? static_cast<std::uint8_t>(byte >> 4U)
                     : static_cast<std::uint8_t>(byte & 0x0FU);
            if (nibble >= kMaxCount) {
                continue;  // 饱和，保持 15。
            }
            const std::uint8_t next = static_cast<std::uint8_t>(nibble + 1U);
            if (high) {
                byte = static_cast<std::uint8_t>(
                    (byte & 0x0FU) | static_cast<std::uint8_t>(next << 4U));
            } else {
                byte = static_cast<std::uint8_t>((byte & 0xF0U) | next);
            }
        }
    }

    /** @brief 取 4 路计数的最小值（只高估，不会低估）。 */
    std::uint32_t Estimate(std::uint64_t hash) const noexcept {
        std::uint8_t minimum = kMaxCount;
        for (std::size_t row = 0; row < kRows; ++row) {
            const std::size_t index = Index(hash, row);
            const std::uint8_t byte = table_[row * stride_ + index / 2U];
            const std::uint8_t nibble =
                (index & 1U) != 0U ? static_cast<std::uint8_t>(byte >> 4U)
                                   : static_cast<std::uint8_t>(byte & 0x0FU);
            if (nibble < minimum) {
                minimum = nibble;
            }
        }
        return minimum;
    }

    /**
     * @brief 所有计数器右移一位（频率减半），用于周期性老化。
     *
     * @details 半字节打包后 `(byte >> 1) & 0x77` 正好把高低两个 4-bit
     *          计数器同时右移 1，并把跨字节的进位清掉。
     */
    void Halve() noexcept {
        for (std::uint8_t& byte : table_) {
            byte = static_cast<std::uint8_t>((byte >> 1U) & 0x77U);
        }
    }

    /** @brief 清空所有计数器（仅测试/诊断使用）。 */
    void Clear() noexcept { std::fill(table_.begin(), table_.end(), 0U); }

    std::size_t Width() const noexcept { return width_; }

    /** @brief 素描占用的字节数（含 4 行）。 */
    std::size_t MemoryBytes() const noexcept { return table_.size(); }

private:
    static std::size_t ChooseWidth(std::size_t capacity) noexcept {
        // 每个缓存条目约 8 个计数器（Caffeine 的经验值），再夹到
        // [64, 1<<20] 之间；宽度必须是 2 的幂，便于用掩码取模。
        const std::size_t scaled =
            capacity > (1U << 17U) ? (1U << 20U) : capacity * 8U;
        const std::size_t bounded = scaled < 64U ? 64U : scaled;
        return NextPowerOfTwo(bounded > (1U << 20U) ? (1U << 20U) : bounded);
    }

    std::size_t Index(std::uint64_t hash, std::size_t row) const noexcept {
        const std::uint64_t row_seed =
            seed_ + static_cast<std::uint64_t>(row) * 0x9E3779B97F4A7C15ULL;
        return static_cast<std::size_t>(SplitMix64(hash ^ row_seed)) &
               (width_ - 1U);
    }

    std::size_t width_;
    std::size_t stride_;
    std::vector<std::uint8_t> table_;
    std::uint64_t seed_;
};

/**
 * @brief 2 位 Bloom 门卫：区分「第一次出现」与「已经出现过」。
 *
 * @details 位图大小按容量推导，饱和后只是失去过滤能力，不会误判为
 *          「没出现过」以外的错误（Bloom 无假阴性）。
 */
class BloomDoorkeeper {
public:
    explicit BloomDoorkeeper(std::size_t capacity)
        : bit_count_(ChooseBits(capacity)),
          bits_((bit_count_ + 63U) / 64U, 0U) {}

    /**
     * @brief 查询并置位。
     * @return true 表示两个 bit 之前都已置位（key 很可能见过）。
     */
    bool TestAndSet(std::uint64_t hash) noexcept {
        const std::size_t mask = bit_count_ - 1U;
        const std::size_t first = static_cast<std::size_t>(hash) & mask;
        const std::size_t second =
            static_cast<std::size_t>(hash >> 32U) & mask;
        const bool seen = Get(first) && Get(second);
        Set(first);
        Set(second);
        return seen;
    }

    void Clear() noexcept {
        for (std::uint64_t& word : bits_) {
            word = 0U;
        }
    }

    std::size_t MemoryBytes() const noexcept { return bits_.size() * 8U; }

    /** @brief 已置位比例，用于诊断（0.0 ~ 1.0）。 */
    double FillRatio() const noexcept {
        std::size_t used = 0;
        for (const std::uint64_t word : bits_) {
            used += PopCount(word);
        }
        return bit_count_ == 0 ? 0.0
                              : static_cast<double>(used) /
                                    static_cast<double>(bit_count_);
    }

private:
    static std::size_t ChooseBits(std::size_t capacity) noexcept {
        const std::size_t scaled =
            capacity > (1U << 21U) ? (1U << 24U) : capacity * 8U;
        const std::size_t bounded = scaled < 64U ? 64U : scaled;
        return NextPowerOfTwo(bounded > (1U << 24U) ? (1U << 24U) : bounded);
    }

    static std::size_t PopCount(std::uint64_t value) noexcept {
        // Hacker's Delight 的 SWAR popcount（避免依赖 C++20 <bit>）。
        value = value - ((value >> 1U) & 0x5555555555555555ULL);
        value = (value & 0x3333333333333333ULL) +
                ((value >> 2U) & 0x3333333333333333ULL);
        value = (value + (value >> 4U)) & 0x0F0F0F0F0F0F0F0FULL;
        return static_cast<std::size_t>((value * 0x0101010101010101ULL) >> 56U);
    }

    bool Get(std::size_t bit) const noexcept {
        return (bits_[bit / 64U] >> (bit % 64U)) & 1U;
    }

    void Set(std::size_t bit) noexcept {
        bits_[bit / 64U] |= (1ULL << (bit % 64U));
    }

    std::size_t bit_count_;
    std::vector<std::uint64_t> bits_;
};

}  // namespace detail
}  // namespace cache
}  // namespace go2cpp
