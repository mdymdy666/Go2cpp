/**
 * @file md5.hpp
 * @brief MD5 消息摘要（RFC 1321）的 C++ 实现，接口对齐 Go `crypto/md5`。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法按 RFC 1321 独立实现）
 *
 * @warning **MD5 已不适合安全用途。** 2004 年王小云团队给出实用碰撞攻击，
 *          此后有大量公开的碰撞样本（含伪造证书、碰撞文件）。它只能用于
 *          校验和、去重、兼容旧协议等**非安全**场景。新设计请用 `Sha256`
 *          或更强的算法；需要抗碰撞时**不要**用本类。
 *
 * @details 实现依据 RFC 1321（<https://www.rfc-editor.org/rfc/rfc1321>）：
 *   - 状态 4 个 `uint32`（A/B/C/D），初值 `0x67452301`、`0xefcdab89`、
 *     `0x98badcfe`、`0x10325476`；
 *   - 64 轮，每轮 `a = b + rotl(a + F(b,c,d) + X[k] + T[i], s)`；
 *   - 输入按 **little-endian** 读入 16 个 32 位字，长度字段也是 little-endian
 *     （这一点与 SHA 系列相反，是最容易写错的地方）；
 *   - 填充：`0x80` + 若干 `0x00` 直到长度 ≡ 56 (mod 64)，再补 64 位比特长度。
 *
 * ## 与 Go `crypto/md5` 的对应关系
 * | Go | 本实现 |
 * |---|---|
 * | `md5.New() hash.Hash` | `Md5 h;` |
 * | `md5.Sum(data) [16]byte` | `Md5::Digest(data)` |
 * | `h.Write(p)` | `h.Write(span)` |
 * | `h.Sum(nil)` | `h.Sum()`（返回 `std::vector<uint8_t>`） |
 * | `h.Size()` / `h.BlockSize()` | `kSize`(16) / `kBlockSize`(64) |
 *
 * @note `Sum(ByteSpan)` **不改变状态**（对齐 Go）；内部通过拷贝状态完成收尾，
 *       因此可以安全地在任意时刻多次取摘要，也可以 `Sum` 之后继续 `Write`。
 *       代价是一次 88 字节左右的状态拷贝，对摘要场景可以忽略。
 *
 * @note 未实现 Go 的 `New()` 工厂返回接口指针、以及 `encoding.BinaryMarshaler`
 *       （`MarshalBinary`/`UnmarshalBinary`）——它们不属于摘要算法的核心语义。
 */
#pragma once

#include "go2cpp/hash/hash.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace go2cpp::hash {

/**
 * @brief MD5 摘要器（RFC 1321）。
 * @warning 不安全，见文件头。仅用于校验/兼容。
 */
class Md5 final : public Hash {
 public:
    /// 摘要长度（字节），对应 Go `md5.Size`。
    static constexpr std::size_t kSize = 16;
    /// 内部块大小（字节），对应 Go `md5.BlockSize`。
    static constexpr std::size_t kBlockSize = 64;

    /// 构造即为空输入状态（等价于 Go `md5.New()`）。
    Md5() noexcept { Reset(); }

    void Write(ByteSpan p) override;

    /// 不改变状态；`out.size()` 必须不小于 `kSize`。
    void Sum(ByteSpan out) const override;

    void Reset() override;

    std::size_t Size() const override { return kSize; }
    std::size_t BlockSize() const override { return kBlockSize; }

    // 让基类的 `Sum()` / `Write(std::string_view)` 便利重载参与重载决议。
    using Hash::Sum;
    using Hash::Write;

    /**
     * @brief 一次性计算 MD5（对应 Go `md5.Sum`）。
     * @param data 输入字节视图（只读）。
     * @return 16 字节摘要。
     */
    static std::array<std::uint8_t, kSize> Digest(ByteSpan data);

 private:
    /// 处理一个 64 字节块（调用方保证 `block` 至少 64 字节）。
    void Transform(const std::uint8_t* block) noexcept;
    /// 收尾并写出摘要；会改变 `*this`，仅供 `Sum` 在副本上调用。
    void Finish(std::uint8_t* out) noexcept;

    std::uint32_t m_state[4]{};
    std::uint64_t m_length{0};  ///< 已写入的总字节数（收尾时换算成比特数）。
    std::uint8_t m_buffer[kBlockSize]{};
    std::size_t m_buffer_len{0};
};

}  // namespace go2cpp::hash
