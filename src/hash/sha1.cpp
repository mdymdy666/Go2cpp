/**
 * @file sha1.cpp
 * @brief SHA-1（FIPS 180-4）的实现。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法按 FIPS 180-4 独立实现）
 *
 * @warning SHA-1 已不适合安全用途（有实际碰撞），详见 sha1.hpp 文件头。
 */

#include "go2cpp/hash/sha1.hpp"

#include "go2cpp/hash/hash.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace go2cpp::hash {

namespace {

inline std::uint32_t Rotl(std::uint32_t value, int shift) noexcept {
    return (value << shift) | (value >> (32 - shift));
}

/// 读 4 字节 big-endian（SHA-1 消息字与长度字段均为大端）。
inline std::uint32_t LoadBE32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

}  // namespace

void Sha1::Reset() {
    m_state[0] = 0x67452301u;
    m_state[1] = 0xefcdab89u;
    m_state[2] = 0x98badcfeu;
    m_state[3] = 0x10325476u;
    m_state[4] = 0xc3d2e1f0u;
    m_length = 0;
    m_buffer_len = 0;
}

void Sha1::Transform(const std::uint8_t* block) noexcept {
    std::uint32_t w[80];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = LoadBE32(block + i * 4);
    }
    for (std::size_t i = 16; i < 80; ++i) {
        w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = m_state[0];
    std::uint32_t b = m_state[1];
    std::uint32_t c = m_state[2];
    std::uint32_t d = m_state[3];
    std::uint32_t e = m_state[4];

    for (std::size_t i = 0; i < 80; ++i) {
        std::uint32_t f = 0;
        std::uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        const std::uint32_t temp = Rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rotl(b, 30);
        b = a;
        a = temp;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
}

void Sha1::Write(ByteSpan p) {
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

void Sha1::Finish(std::uint8_t* out) noexcept {
    const std::uint64_t bit_length = m_length * 8;

    std::uint8_t pad[64] = {};
    pad[0] = 0x80;
    const std::size_t pad_len =
        m_buffer_len < 56 ? 56 - m_buffer_len : 120 - m_buffer_len;
    Write(ByteSpan(pad, pad_len));

    // 64 位比特长度，big-endian。
    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (8 * (7 - i)));
    }
    Write(ByteSpan(length_bytes, 8));

    // 按大端写出 5 个状态字。
    for (std::size_t i = 0; i < 5; ++i) {
        const std::uint32_t value = m_state[i];
        out[i * 4 + 0] = static_cast<std::uint8_t>(value >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(value >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(value >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(value);
    }
}

void Sha1::Sum(ByteSpan out) const {
    assert(out.size() >= kSize);
    Sha1 copy = *this;
    std::uint8_t digest[kSize];
    copy.Finish(digest);
    std::memcpy(out.data(), digest, kSize);
}

std::array<std::uint8_t, Sha1::kSize> Sha1::Digest(ByteSpan data) {
    Sha1 hash;
    hash.Write(data);
    std::array<std::uint8_t, kSize> out{};
    hash.Sum(AsBytes(out));
    return out;
}

}  // namespace go2cpp::hash
