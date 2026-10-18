/**
 * @file adler32.cpp
 * @brief `go2cpp::hash::Adler32` 的实现（5552 字节分块 + 16 字节展开）。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（算法按 RFC 1950 独立实现）
 */

#include "go2cpp/hash/adler32.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace go2cpp::hash {

namespace {

/// 大于 65521 的最小素数，Adler-32 的模数（RFC 1950）。
constexpr std::uint32_t kMod = 65521;

/**
 * @brief 单块最大字节数：保证 `b` 在取模前不溢出 32 位。
 * @details 与 zlib 的 `NMAX` 相同（4054 时代之后的固定值），推导见 adler32.hpp。
 */
constexpr std::size_t kMaxChunk = 5552;

}  // namespace

void Adler32::Write(ByteSpan p) {
    const std::uint8_t* data = p.data();
    std::size_t remaining = p.size();

    std::uint32_t a = m_a;
    std::uint32_t b = m_b;

    while (remaining != 0) {
        const std::size_t chunk = remaining < kMaxChunk ? remaining : kMaxChunk;
        remaining -= chunk;
        std::size_t left = chunk;

        // 16 字节展开：让编译器把 16 次「a += byte; b += a」排成无分支序列。
        while (left >= 16) {
            a += data[0];
            b += a;
            a += data[1];
            b += a;
            a += data[2];
            b += a;
            a += data[3];
            b += a;
            a += data[4];
            b += a;
            a += data[5];
            b += a;
            a += data[6];
            b += a;
            a += data[7];
            b += a;
            a += data[8];
            b += a;
            a += data[9];
            b += a;
            a += data[10];
            b += a;
            a += data[11];
            b += a;
            a += data[12];
            b += a;
            a += data[13];
            b += a;
            a += data[14];
            b += a;
            a += data[15];
            b += a;
            data += 16;
            left -= 16;
        }
        while (left != 0) {
            a += *data++;
            b += a;
            --left;
        }

        a %= kMod;
        b %= kMod;
    }

    m_a = a;
    m_b = b;
}

void Adler32::Reset() {
    m_a = 1;
    m_b = 0;
}

std::uint32_t Adler32::Checksum(ByteSpan data) noexcept {
    Adler32 hash;
    hash.Write(data);
    return hash.Sum32();
}

void Adler32::Sum(ByteSpan out) const {
    assert(out.size() >= kSize);
    // 与 Go `adler32.digest.Sum` 相同：大端写出。
    const std::uint32_t value = Sum32();
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

}  // namespace go2cpp::hash
