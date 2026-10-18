/**
 * @file sha256.cpp
 * @brief SHA-256（FIPS 180-4）的实现。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法按 FIPS 180-4 独立实现）
 */

#include "go2cpp/hash/sha256.hpp"

#include "go2cpp/hash/hash.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace go2cpp::hash {

namespace {

inline std::uint32_t Rotr(std::uint32_t value, int shift) noexcept {
    return (value >> shift) | (value << (32 - shift));
}

inline std::uint32_t LoadBE32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

/// FIPS 180-4 §4.2.2 的 64 个轮常量：前 64 个素数立方根小数部分的前 32 位。
constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

}  // namespace

void Sha256::Reset() {
    m_state[0] = 0x6a09e667u;
    m_state[1] = 0xbb67ae85u;
    m_state[2] = 0x3c6ef372u;
    m_state[3] = 0xa54ff53au;
    m_state[4] = 0x510e527fu;
    m_state[5] = 0x9b05688cu;
    m_state[6] = 0x1f83d9abu;
    m_state[7] = 0x5be0cd19u;
    m_length = 0;
    m_buffer_len = 0;
}

void Sha256::Transform(const std::uint8_t* block) noexcept {
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = LoadBE32(block + i * 4);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = m_state[0];
    std::uint32_t b = m_state[1];
    std::uint32_t c = m_state[2];
    std::uint32_t d = m_state[3];
    std::uint32_t e = m_state[4];
    std::uint32_t f = m_state[5];
    std::uint32_t g = m_state[6];
    std::uint32_t h = m_state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

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

void Sha256::Write(ByteSpan p) {
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
        data += take;
        remaining -= take;
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

void Sha256::Finish(std::uint8_t* out) noexcept {
    const std::uint64_t bit_length = m_length * 8;

    std::uint8_t pad[64] = {};
    pad[0] = 0x80;
    const std::size_t pad_len =
        m_buffer_len < 56 ? 56 - m_buffer_len : 120 - m_buffer_len;
    Write(ByteSpan(pad, pad_len));

    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (8 * (7 - i)));
    }
    Write(ByteSpan(length_bytes, 8));

    for (std::size_t i = 0; i < 8; ++i) {
        const std::uint32_t value = m_state[i];
        out[i * 4 + 0] = static_cast<std::uint8_t>(value >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(value >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(value >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(value);
    }
}

void Sha256::Sum(ByteSpan out) const {
    assert(out.size() >= kSize);
    Sha256 copy = *this;
    std::uint8_t digest[kSize];
    copy.Finish(digest);
    std::memcpy(out.data(), digest, kSize);
}

std::array<std::uint8_t, Sha256::kSize> Sha256::Digest(ByteSpan data) {
    Sha256 hash;
    hash.Write(data);
    std::array<std::uint8_t, kSize> out{};
    hash.Sum(AsBytes(out));
    return out;
}

}  // namespace go2cpp::hash
