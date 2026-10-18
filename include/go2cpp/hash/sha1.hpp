/**
 * @file sha1.hpp
 * @brief SHA-1 消息摘要（FIPS 180-4）的 C++ 实现，接口对齐 Go `crypto/sha1`。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法按 FIPS 180-4 独立实现）
 *
 * @warning **SHA-1 已不适合安全用途。** 2017 年 SHAttered 攻击给出了两个
 *          不同 PDF 的实际碰撞，此后碰撞成本持续下降（2020 年已低于 10 万美元）。
 *          NIST 自 2011 年起就禁止在数字签名中使用 SHA-1。它只能用于校验和、
 *          内容寻址、兼容旧协议（Git 对象名等）等**非安全**场景。新设计请用
 *          `Sha256` 或更强的算法；需要抗碰撞时**不要**用本类。
 *
 * @details 实现依据 FIPS 180-4（<https://doi.org/10.6028/NIST.FIPS.180-4>）：
 *   - 状态 5 个 `uint32`，初值 `0x67452301`、`0xefcdab89`、`0x98badcfe`、
 *     `0x10325476`、`0xc3d2e1f0`；
 *   - 80 轮，消息扩展 `W[i] = rotl(W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16], 1)`；
 *   - 输入与长度字段均为 **big-endian**（与 MD5 相反）；
 *   - 填充规则与 MD5 相同：`0x80` + 零，直到 ≡ 56 (mod 64)，再补 64 位比特长度。
 *
 * ## 与 Go `crypto/sha1` 的对应关系
 * | Go | 本实现 |
 * |---|---|
 * | `sha1.New() hash.Hash` | `Sha1 h;` |
 * | `sha1.Sum(data) [20]byte` | `Sha1::Digest(data)` |
 * | `h.Sum(nil)` | `h.Sum()` |
 * | `h.Size()` / `h.BlockSize()` | `kSize`(20) / `kBlockSize`(64) |
 *
 * @note `Sum(ByteSpan)` 不改变状态，实现方式与 `Md5` 相同（拷贝状态后收尾）。
 * @note 未实现 Go 的 `New()` 工厂与二进制序列化接口（`MarshalBinary` 等）。
 */
#pragma once

#include "go2cpp/hash/hash.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace go2cpp::hash {

/**
 * @brief SHA-1 摘要器（FIPS 180-4）。
 * @warning 不安全，见文件头。仅用于校验/兼容。
 */
class Sha1 final : public Hash {
 public:
    /// 摘要长度（字节），对应 Go `sha1.Size`。
    static constexpr std::size_t kSize = 20;
    /// 内部块大小（字节），对应 Go `sha1.BlockSize`。
    static constexpr std::size_t kBlockSize = 64;

    /// 构造即为空输入状态（等价于 Go `sha1.New()`）。
    Sha1() noexcept { Reset(); }

    void Write(ByteSpan p) override;

    /// 不改变状态；`out.size()` 必须不小于 `kSize`。
    void Sum(ByteSpan out) const override;

    void Reset() override;

    std::size_t Size() const override { return kSize; }
    std::size_t BlockSize() const override { return kBlockSize; }

    using Hash::Sum;
    using Hash::Write;

    /**
     * @brief 一次性计算 SHA-1（对应 Go `sha1.Sum`）。
     * @param data 输入字节视图（只读）。
     * @return 20 字节摘要。
     */
    static std::array<std::uint8_t, kSize> Digest(ByteSpan data);

 private:
    void Transform(const std::uint8_t* block) noexcept;
    void Finish(std::uint8_t* out) noexcept;

    std::uint32_t m_state[5]{};
    std::uint64_t m_length{0};  ///< 已写入的总字节数。
    std::uint8_t m_buffer[kBlockSize]{};
    std::size_t m_buffer_len{0};
};

}  // namespace go2cpp::hash
