/**
 * @file md5.cpp
 * @brief MD5（RFC 1321）的实现。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法按 RFC 1321 独立实现）
 *
 * @details 实现结构对照 RFC 1321 §3.4 的参考伪代码：
 *   四轮、每轮 16 步，F/G/H/I 四个非线性函数，消息字索引分别为
 *   `i`、`(5i+1) mod 16`、`(3i+5) mod 16`、`(7i) mod 16`。
 *   为了便于编译器展开，四轮写成四个独立循环而不是一个带分支的 64 次循环。
 */

#include "go2cpp/hash/md5.hpp"

#include "go2cpp/hash/hash.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace go2cpp::hash {

namespace {

/// 32 位循环左移；`shift` 必须在 [1, 31]。
inline std::uint32_t Rotl(std::uint32_t value, int shift) noexcept {
    return (value << shift) | (value >> (32 - shift));
}

/// RFC 1321 的 64 个加法常量 T[i] = floor(2^32 * abs(sin(i+1)))。
constexpr std::uint32_t kK[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu,
    0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
    0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu,
    0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u,
    0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
    0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u,
    0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
    0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du,
    0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

/// 每步的循环左移位数（四轮各 4 个值循环使用）。
constexpr int kS[64] = {
    7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22,
    5,  9,  14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 5,  9,  14, 20,
    4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
    6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21,
};

/// 读 4 字节 little-endian（MD5 的消息字与长度字段都是小端）。
inline std::uint32_t LoadLE32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

void Md5::Reset() {
    m_state[0] = 0x67452301u;
    m_state[1] = 0xefcdab89u;
    m_state[2] = 0x98badcfeu;
    m_state[3] = 0x10325476u;
    m_length = 0;
    m_buffer_len = 0;
}

void Md5::Transform(const std::uint8_t* block) noexcept {
    std::uint32_t x[16];
    for (std::size_t i = 0; i < 16; ++i) {
        x[i] = LoadLE32(block + i * 4);
    }

    std::uint32_t a = m_state[0];
    std::uint32_t b = m_state[1];
    std::uint32_t c = m_state[2];
    std::uint32_t d = m_state[3];

    // 第 1 轮：F(b,c,d) = (b & c) | (~b & d)，消息字按原顺序。
    for (std::uint32_t i = 0; i < 16; ++i) {
        const std::uint32_t f = (b & c) | (~b & d);
        const std::uint32_t tmp = d;
        d = c;
        c = b;
        b = b + Rotl(a + f + x[i] + kK[i], kS[i]);
        a = tmp;
    }
    // 第 2 轮：G(b,c,d) = (d & b) | (~d & c)，消息字索引 (5i+1) mod 16。
    for (std::uint32_t i = 16; i < 32; ++i) {
        const std::uint32_t f = (d & b) | (~d & c);
        const std::uint32_t g = (5u * i + 1u) & 15u;
        const std::uint32_t tmp = d;
        d = c;
        c = b;
        b = b + Rotl(a + f + x[g] + kK[i], kS[i]);
        a = tmp;
    }
    // 第 3 轮：H(b,c,d) = b ^ c ^ d，消息字索引 (3i+5) mod 16。
    for (std::uint32_t i = 32; i < 48; ++i) {
        const std::uint32_t f = b ^ c ^ d;
        const std::uint32_t g = (3u * i + 5u) & 15u;
        const std::uint32_t tmp = d;
        d = c;
        c = b;
        b = b + Rotl(a + f + x[g] + kK[i], kS[i]);
        a = tmp;
    }
    // 第 4 轮：I(b,c,d) = c ^ (b | ~d)，消息字索引 (7i) mod 16。
    for (std::uint32_t i = 48; i < 64; ++i) {
        const std::uint32_t f = c ^ (b | ~d);
        const std::uint32_t g = (7u * i) & 15u;
        const std::uint32_t tmp = d;
        d = c;
        c = b;
        b = b + Rotl(a + f + x[g] + kK[i], kS[i]);
        a = tmp;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
}

void Md5::Write(ByteSpan p) {
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

void Md5::Finish(std::uint8_t* out) noexcept {
    const std::uint64_t bit_length = m_length * 8;

    // 填充：0x80 + 零，直到 buffer_len ≡ 56 (mod 64)。pad 长度最大 64 字节。
    std::uint8_t pad[64] = {};
    pad[0] = 0x80;
    const std::size_t pad_len =
        m_buffer_len < 56 ? 56 - m_buffer_len : 120 - m_buffer_len;
    Write(ByteSpan(pad, pad_len));

    // 追加 64 位比特长度（little-endian）。
    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (8 * i));
    }
    Write(ByteSpan(length_bytes, 8));

    // 此时 m_buffer_len == 0，状态即为最终值；按小端写出 4 个字。
    for (std::size_t i = 0; i < 4; ++i) {
        const std::uint32_t value = m_state[i];
        out[i * 4 + 0] = static_cast<std::uint8_t>(value);
        out[i * 4 + 1] = static_cast<std::uint8_t>(value >> 8);
        out[i * 4 + 2] = static_cast<std::uint8_t>(value >> 16);
        out[i * 4 + 3] = static_cast<std::uint8_t>(value >> 24);
    }
}

void Md5::Sum(ByteSpan out) const {
    assert(out.size() >= kSize);
    Md5 copy = *this;
    std::uint8_t digest[kSize];
    copy.Finish(digest);
    std::memcpy(out.data(), digest, kSize);
}

std::array<std::uint8_t, Md5::kSize> Md5::Digest(ByteSpan data) {
    Md5 hash;
    hash.Write(data);
    std::array<std::uint8_t, kSize> out{};
    hash.Sum(AsBytes(out));
    return out;
}

}  // namespace go2cpp::hash
