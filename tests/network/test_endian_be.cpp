/**
 * @file test_endian_be.cpp
 * @brief 大端读写公共实现（go2cpp::net::ReadBE* / WriteBE*）的逐字节对拍。
 *
 * @details 对拍基准是「改动前各模块里的逐字节实现」的逐行副本：
 *          - 16/32/64 三档位宽的读与写；
 *          - 固定边界值 + 1e6 字节随机数据的所有窗口 + 未对齐偏移；
 *          - 写入结果逐字节 memcmp，读取结果数值相等。
 *
 *          独立可执行（不注册 ctest：cmake/ 不在本次改动边界）：
 *          g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -I include \
 *              tests/network/test_endian_be.cpp -o /tmp/test_endian_be
 */

#include "go2cpp/network/endian.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("[FAIL] %s\n", what);
    }
}

// ---------------------------------------------------------------------------
// 改动前的逐字节实现（参考基准，逐行副本；不要改成 memcpy/bswap 版本）
// ---------------------------------------------------------------------------

std::uint16_t LegacyReadBE16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                      static_cast<std::uint16_t>(in[1]));
}

std::uint32_t LegacyReadBE32(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

std::uint64_t LegacyReadBE64(const std::uint8_t* in) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(in[i]);
    }
    return value;
}

void LegacyWriteBE16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

void LegacyWriteBE32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

void LegacyWriteBE64(std::uint8_t* out, std::uint64_t value) noexcept {
    for (int i = 7; i >= 0; --i) {
        out[7 - i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

// ---------------------------------------------------------------------------
// 固定值：写入逐字节一致 + 读回相等
// ---------------------------------------------------------------------------

void CheckFixed16(std::uint16_t value) {
    std::uint8_t got[2] = {0xAA, 0xAA};
    std::uint8_t want[2] = {0xAA, 0xAA};
    go2cpp::net::WriteBE16(got, value);
    LegacyWriteBE16(want, value);
    Check(std::memcmp(got, want, sizeof(got)) == 0, "WriteBE16 与旧实现逐字节一致");
    Check(go2cpp::net::ReadBE16(got) == value, "ReadBE16 往返");
    Check(go2cpp::ReadBE16(got) == value, "go2cpp::ReadBE16 using 别名可用");
}

void CheckFixed32(std::uint32_t value) {
    std::uint8_t got[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    std::uint8_t want[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    go2cpp::net::WriteBE32(got, value);
    LegacyWriteBE32(want, value);
    Check(std::memcmp(got, want, sizeof(got)) == 0, "WriteBE32 与旧实现逐字节一致");
    Check(go2cpp::net::ReadBE32(got) == value, "ReadBE32 往返");
}

void CheckFixed64(std::uint64_t value) {
    std::uint8_t got[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    std::uint8_t want[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    go2cpp::net::WriteBE64(got, value);
    LegacyWriteBE64(want, value);
    Check(std::memcmp(got, want, sizeof(got)) == 0, "WriteBE64 与旧实现逐字节一致");
    Check(go2cpp::net::ReadBE64(got) == value, "ReadBE64 往返");
}

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // 1) 固定边界值
    // ------------------------------------------------------------------
    const std::uint16_t u16_values[] = {0x0000, 0x0001, 0x00ff, 0xff00, 0x0102,
                                        0x7fff, 0x8000, 0xffff};
    for (const std::uint16_t value : u16_values) {
        CheckFixed16(value);
    }
    const std::uint32_t u32_values[] = {0x00000000u, 0x00000001u, 0x000000ffu, 0x0000ff00u,
                                        0x00ff0000u, 0xff000000u, 0x01020304u, 0x7fffffffu,
                                        0x80000000u, 0xdeadbeefu, 0xffffffffu};
    for (const std::uint32_t value : u32_values) {
        CheckFixed32(value);
    }
    const std::uint64_t u64_values[] = {0x0000000000000000ULL,
                                        0x0000000000000001ULL,
                                        0x00000000000000ffULL,
                                        0x0123456789abcdefULL,
                                        0x7fffffffffffffffULL,
                                        0x8000000000000000ULL,
                                        0xfedcba9876543210ULL,
                                        0xffffffffffffffffULL};
    for (const std::uint64_t value : u64_values) {
        CheckFixed64(value);
    }
    std::printf("固定边界值检查完成\n");

    // ------------------------------------------------------------------
    // 2) 1e6 字节随机数据：所有窗口（天然覆盖未对齐地址）
    // ------------------------------------------------------------------
    constexpr std::size_t kBytes = 1000000;
    std::vector<std::uint8_t> data(kBytes);
    std::mt19937_64 rng(0x9E3779B97F4A7C15ULL);
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(rng());
    }

    for (std::size_t offset = 0; offset + 2 <= kBytes; ++offset) {
        Check(go2cpp::net::ReadBE16(data.data() + offset) == LegacyReadBE16(data.data() + offset),
              "ReadBE16 1e6 窗口逐字节一致");
    }
    for (std::size_t offset = 0; offset + 4 <= kBytes; ++offset) {
        Check(go2cpp::net::ReadBE32(data.data() + offset) == LegacyReadBE32(data.data() + offset),
              "ReadBE32 1e6 窗口逐字节一致");
    }
    for (std::size_t offset = 0; offset + 8 <= kBytes; ++offset) {
        Check(go2cpp::net::ReadBE64(data.data() + offset) == LegacyReadBE64(data.data() + offset),
              "ReadBE64 1e6 窗口逐字节一致");
    }
    std::printf("1e6 字节随机窗口读对拍完成\n");

    // ------------------------------------------------------------------
    // 3) 写入对拍：随机值 10 万组
    // ------------------------------------------------------------------
    for (int index = 0; index < 100000; ++index) {
        const auto value16 = static_cast<std::uint16_t>(rng());
        CheckFixed16(value16);
        const auto value32 = static_cast<std::uint32_t>(rng());
        CheckFixed32(value32);
        const std::uint64_t value64 = rng();
        CheckFixed64(value64);
    }
    std::printf("10 万组随机写入对拍完成\n");

    std::printf("endian 对拍：checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
