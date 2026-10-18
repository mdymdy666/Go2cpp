/**
 * @file sha256.hpp
 * @brief SHA-256 消息摘要（FIPS 180-4）的 C++ 实现，接口对齐 Go `crypto/sha256`。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法按 FIPS 180-4 独立实现）
 *
 * @details 实现依据 FIPS 180-4（<https://doi.org/10.6028/NIST.FIPS.180-4>）：
 *   - 状态 8 个 `uint32`，取前 8 个素数平方根小数部分
 *     （`0x6a09e667` ... `0x5be0cd19`）；
 *   - 64 轮，轮常量 K 取前 64 个素数立方根小数部分（标准表）；
 *   - 消息扩展使用 σ0/σ1，压缩函数使用 Σ0/Σ1 与 Ch/Maj；
 *   - 输入与长度字段均为 **big-endian**；填充规则与 SHA-1 相同。
 *
 * ## 与 Go `crypto/sha256` 的对应关系
 * | Go | 本实现 |
 * |---|---|
 * | `sha256.New() hash.Hash` | `Sha256 h;` |
 * | `sha256.Sum256(data) [32]byte` | `Sha256::Digest(data)` |
 * | `h.Sum(nil)` | `h.Sum()` |
 * | `h.Size()` / `h.BlockSize()` | `kSize`(32) / `kBlockSize`(64) |
 *
 * @note 本类只实现 SHA-256（不含 SHA-224）；`Size()==BlockSize()` 的组合
 *       与 Go 的 `crypto/sha256` 一致（64 字节块）。
 * @note `Sum(ByteSpan)` 不改变状态，实现方式与 `Md5`/`Sha1` 相同。
 * @note 未实现 Go 的 `New()` 工厂、`New224()` 与二进制序列化接口。
 */
#pragma once

#include "go2cpp/hash/hash.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace go2cpp::hash {

/**
 * @brief SHA-256 摘要器（FIPS 180-4）。
 *
 * @details 性能预期：Release（`-O3`）下 200~500 MB/s 属正常范围（标量实现，
 *          未用 SHA-NI 指令）；与 Go 的汇编实现相比会有差距，但满足「纯 C++、
 *          零依赖」的约束。压测数据见 `tests/hash/test_hash_stress.cpp`。
 */
class Sha256 final : public Hash {
 public:
    /// 摘要长度（字节），对应 Go `sha256.Size`。
    static constexpr std::size_t kSize = 32;
    /// 内部块大小（字节），对应 Go `sha256.BlockSize`。
    static constexpr std::size_t kBlockSize = 64;

    /// 构造即为空输入状态（等价于 Go `sha256.New()`）。
    Sha256() noexcept { Reset(); }

    void Write(ByteSpan p) override;

    /// 不改变状态；`out.size()` 必须不小于 `kSize`。
    void Sum(ByteSpan out) const override;

    void Reset() override;

    std::size_t Size() const override { return kSize; }
    std::size_t BlockSize() const override { return kBlockSize; }

    using Hash::Sum;
    using Hash::Write;

    /**
     * @brief 一次性计算 SHA-256（对应 Go `sha256.Sum256`）。
     * @param data 输入字节视图（只读）。
     * @return 32 字节摘要。
     */
    static std::array<std::uint8_t, kSize> Digest(ByteSpan data);

 private:
    void Transform(const std::uint8_t* block) noexcept;
    void Finish(std::uint8_t* out) noexcept;

    std::uint32_t m_state[8]{};
    std::uint64_t m_length{0};  ///< 已写入的总字节数。
    std::uint8_t m_buffer[kBlockSize]{};
    std::size_t m_buffer_len{0};
};

}  // namespace go2cpp::hash
