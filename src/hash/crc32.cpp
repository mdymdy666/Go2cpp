/**
 * @file crc32.cpp
 * @brief `go2cpp::hash::Crc32` 的实现（slicing-by-8 查表）。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（语义对照 Go `hash/crc32`，实现独立完成）
 */

#include "go2cpp/hash/crc32.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace go2cpp::hash {

// 三个内建表都是 constexpr 生成，属于常量初始化（不做运行期计算）。
const Table IEEETable = MakeTable(IEEE);
const Table CastagnoliTable = MakeTable(Castagnoli);
const Table KoopmanTable = MakeTable(Koopman);

namespace {

/// 读取 4 字节 little-endian（CRC 的 32 位字按最低位先算，等价于 LE 读入）。
inline std::uint32_t LoadLE32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

std::uint32_t Crc32::Update(std::uint32_t crc, const Table& table,
                            ByteSpan data) noexcept {
    const std::uint32_t* const t = table.entries.data();
    const std::uint8_t* p = data.data();
    std::size_t remaining = data.size();

    // 与 Go 一致：进入时取反，离开时再取反。调用方传的 crc 是「已完成」的值。
    crc = ~crc;

    // slicing-by-8：一次消费 8 字节，8 张表分别处理 4 个 CRC 字节与 4 个数据字节。
    while (remaining >= 8) {
        crc ^= LoadLE32(p);
        crc = t[7 * 256 + (crc & 0xffu)] ^
              t[6 * 256 + ((crc >> 8) & 0xffu)] ^
              t[5 * 256 + ((crc >> 16) & 0xffu)] ^
              t[4 * 256 + ((crc >> 24) & 0xffu)] ^
              t[3 * 256 + p[4]] ^
              t[2 * 256 + p[5]] ^
              t[1 * 256 + p[6]] ^
              t[0 * 256 + p[7]];
        p += 8;
        remaining -= 8;
    }

    // 尾部不足 8 字节，退回基础 256 表逐字节处理。
    while (remaining != 0) {
        crc = t[(crc ^ *p) & 0xffu] ^ (crc >> 8);
        ++p;
        --remaining;
    }

    return ~crc;
}

std::uint32_t Crc32::ChecksumIEEE(ByteSpan data) noexcept {
    return Update(0, IEEETable, data);
}

std::uint32_t Crc32::Checksum(ByteSpan data, const Table& table) noexcept {
    return Update(0, table, data);
}

void Crc32::Sum(ByteSpan out) const {
    assert(out.size() >= kSize);
    // 与 Go `crc32.digest.Sum` 相同：大端写出。
    out[0] = static_cast<std::uint8_t>(m_crc >> 24);
    out[1] = static_cast<std::uint8_t>(m_crc >> 16);
    out[2] = static_cast<std::uint8_t>(m_crc >> 8);
    out[3] = static_cast<std::uint8_t>(m_crc);
}

}  // namespace go2cpp::hash
