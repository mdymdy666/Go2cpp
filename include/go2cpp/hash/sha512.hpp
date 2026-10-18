/**
 * @file sha512.hpp
 * @brief SHA-512 消息摘要（FIPS 180-4）的 C++ 实现，接口对齐 Go `crypto/sha512`。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法按 FIPS 180-4 独立实现）
 *
 * @details **为什么是 header-only**：本模块的共享 CMake 由父 agent 统一维护，
 *   新增 `.cpp` 需要改 `cmake/Go2CppModules.cmake` 的源文件列表。把 SHA-512
 *   全部写成 `inline`/类内定义，既不新增链接期符号，也不改构建脚本，使用方
 *   只 include 本头即可（与 `sha256.hpp` 的 API 形状保持一致）。
 *
 *   实现依据 FIPS 180-4：
 *   - 状态 8 个 `uint64`，取前 8 个素数平方根小数部分（64 位）；
 *   - 80 轮，轮常量 K 取前 80 个素数立方根小数部分（标准表）；
 *   - 块大小 **128 字节**，长度字段 **16 字节大端**（高 64 位恒为 0，因为
 *     本实现按 `uint64` 字节数记录长度，位长度 = `len << 3`，与 Go 相同）；
 *   - 消息扩展 σ0/σ1 与压缩函数 Σ0/Σ1、Ch/Maj 均为 64 位运算。
 *
 * ## 与 Go `crypto/sha512` 的对应关系
 * | Go | 本实现 |
 * |---|---|
 * | `sha512.New() hash.Hash` | `Sha512 h;` |
 * | `sha512.Sum512(data) [64]byte` | `Sha512::Digest(data)` |
 * | `h.Sum(nil)` | `h.Sum()` |
 * | `h.Size()` / `h.BlockSize()` | `kSize`(64) / `kBlockSize`(128) |
 *
 * @note `Sum(ByteSpan)` 不改变状态（与 `Md5`/`Sha1`/`Sha256` 的实现方式一致）。
 * @note **未实现**：SHA-384 / SHA-512/224 / SHA-512/256（不同初始向量与截断，
 *       属于额外算法变体）、`New()` 工厂、`MarshalBinary` 序列化、HMAC。
 *       原因见 `hash.hpp` 的 C 类清单。
 *
 * @warning 与 SHA-256 一样，本实现是**标量纯 C++**，未使用 SHA-NI/AVX2；
 *          吞吐低于 Go 的汇编实现，但零外部依赖、可移植。
 */

#pragma once

#include "go2cpp/hash/hash.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace go2cpp::hash {

/**
 * @brief SHA-512 摘要器（FIPS 180-4）。
 *
 * @details 线程安全性与其它摘要一致：**同一实例**被多线程写需要外部同步；
 *          不同实例互不影响（所有状态都是成员）。
 */
class Sha512 final : public Hash {
 public:
    /// 摘要长度（字节），对应 Go `sha512.Size`。
    static constexpr std::size_t kSize = 64;
    /// 内部块大小（字节），对应 Go `sha512.BlockSize`。
    static constexpr std::size_t kBlockSize = 128;

    /// 构造即为空输入状态（等价于 Go `sha512.New()`）。
    Sha512() noexcept { Reset(); }

    void Write(ByteSpan p) override {
        const std::uint8_t* data = p.data();
        std::size_t remaining = p.size();
        m_length += remaining;

        if (m_buffer_len != 0) {
            const std::size_t need = kBlockSize - m_buffer_len;
            const std::size_t take = remaining < need ? remaining : need;
            if (take != 0) {
                std::memcpy(m_buffer + m_buffer_len, data, take);
            }
            m_buffer_len += take;
            // 空 Write 时 p.data() 可以是 nullptr；C++ 对 nullptr 做 `+ 0`
            // 也是未定义行为，因此只有真的拷贝了字节才推进指针。
            if (take != 0) {
                data += take;
                remaining -= take;
            }
            if (m_buffer_len == kBlockSize) {
                Transform(m_buffer);
                m_buffer_len = 0;
            }
        }

        while (remaining >= kBlockSize) {
            Transform(data);
            data += kBlockSize;
            remaining -= kBlockSize;
        }

        if (remaining != 0) {
            std::memcpy(m_buffer, data, remaining);
            m_buffer_len = remaining;
        }
    }

    /// 不改变状态；`out.size()` 必须不小于 `kSize`。
    void Sum(ByteSpan out) const override {
        assert(out.size() >= kSize);
        Sha512 copy = *this;
        std::uint8_t digest[kSize];
        copy.Finish(digest);
        std::memcpy(out.data(), digest, kSize);
    }

    void Reset() override {
        m_state[0] = 0x6a09e667f3bcc908ULL;
        m_state[1] = 0xbb67ae8584caa73bULL;
        m_state[2] = 0x3c6ef372fe94f82bULL;
        m_state[3] = 0xa54ff53a5f1d36f1ULL;
        m_state[4] = 0x510e527fade682d1ULL;
        m_state[5] = 0x9b05688c2b3e6c1fULL;
        m_state[6] = 0x1f83d9abfb41bd6bULL;
        m_state[7] = 0x5be0cd19137e2179ULL;
        m_length = 0;
        m_buffer_len = 0;
    }

    std::size_t Size() const override { return kSize; }
    std::size_t BlockSize() const override { return kBlockSize; }

    using Hash::Sum;
    using Hash::Write;

    /**
     * @brief 一次性计算 SHA-512（对应 Go `sha512.Sum512`）。
     * @param data 输入字节视图（只读）。
     * @return 64 字节摘要。
     */
    static std::array<std::uint8_t, kSize> Digest(ByteSpan data) {
        Sha512 hash;
        hash.Write(data);
        std::array<std::uint8_t, kSize> out{};
        hash.Sum(AsBytes(out));
        return out;
    }

 private:
    static std::uint64_t Rotr(std::uint64_t value, int shift) noexcept {
        return (value >> shift) | (value << (64 - shift));
    }

    static std::uint64_t LoadBE64(const std::uint8_t* p) noexcept {
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value = (value << 8) | static_cast<std::uint64_t>(p[i]);
        }
        return value;
    }

    void Transform(const std::uint8_t* block) noexcept {
        // FIPS 180-4 §4.2.3：前 80 个素数立方根小数部分的前 64 位。
        static constexpr std::uint64_t kK[80] = {
            0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
            0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
            0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
            0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
            0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
            0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
            0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
            0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
            0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
            0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
            0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
            0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
            0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
            0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
            0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
            0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
            0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
            0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
            0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
            0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
            0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
            0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
            0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
            0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
            0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
            0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
            0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
            0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
            0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
            0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
            0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
            0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
            0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
            0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
            0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
            0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
            0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
            0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
            0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
            0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
        };

        std::uint64_t w[80];
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = LoadBE64(block + i * 8);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            const std::uint64_t s0 =
                Rotr(w[i - 15], 1) ^ Rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const std::uint64_t s1 =
                Rotr(w[i - 2], 19) ^ Rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint64_t a = m_state[0];
        std::uint64_t b = m_state[1];
        std::uint64_t c = m_state[2];
        std::uint64_t d = m_state[3];
        std::uint64_t e = m_state[4];
        std::uint64_t f = m_state[5];
        std::uint64_t g = m_state[6];
        std::uint64_t h = m_state[7];

        for (std::size_t i = 0; i < 80; ++i) {
            const std::uint64_t s1 = Rotr(e, 14) ^ Rotr(e, 18) ^ Rotr(e, 41);
            const std::uint64_t ch = (e & f) ^ (~e & g);
            const std::uint64_t temp1 = h + s1 + ch + kK[i] + w[i];
            const std::uint64_t s0 = Rotr(a, 28) ^ Rotr(a, 34) ^ Rotr(a, 39);
            const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint64_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    void Finish(std::uint8_t* out) noexcept {
        const std::uint64_t bit_length = m_length << 3;

        std::uint8_t pad[128] = {};
        pad[0] = 0x80;
        const std::size_t pad_len =
            m_buffer_len < 112 ? 112 - m_buffer_len : 240 - m_buffer_len;
        Write(ByteSpan(pad, pad_len));

        // 128 位大端长度：高 64 位恒为 0（本实现用 uint64 记录字节数）。
        std::uint8_t length_bytes[16] = {};
        for (std::size_t i = 0; i < 8; ++i) {
            length_bytes[8 + i] =
                static_cast<std::uint8_t>(bit_length >> (8 * (7 - i)));
        }
        Write(ByteSpan(length_bytes, 16));

        for (std::size_t i = 0; i < 8; ++i) {
            const std::uint64_t value = m_state[i];
            for (std::size_t j = 0; j < 8; ++j) {
                out[i * 8 + j] =
                    static_cast<std::uint8_t>(value >> (8 * (7 - j)));
            }
        }
    }

    std::uint64_t m_state[8]{};
    std::uint64_t m_length{0};  ///< 已写入的总字节数（位长度 = m_length << 3）。
    std::uint8_t m_buffer[kBlockSize]{};
    std::size_t m_buffer_len{0};
};

}  // namespace go2cpp::hash
