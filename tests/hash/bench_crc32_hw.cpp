/**
 * @file bench_crc32_hw.cpp
 * @brief CRC32 硬件路径（PCLMULQDQ / crc32q）对拍 zlib + 吞吐基准（独立编译，不注册 ctest）。
 *
 * @details 本文件不参与 CMake 构建，按下面的命令单独编译（需要系统 zlib 作为 oracle）：
 *
 *          ```
 *          g++ -O2 -std=c++17 -Wall -Wextra -Wpedantic -Iinclude \
 *              tests/hash/bench_crc32_hw.cpp src/hash/crc32.cpp src/hash/hash.cpp \
 *              -lz -o /tmp/bench_crc32_hw
 *          /tmp/bench_crc32_hw
 *          ```
 *
 *          验收：
 *          - IEEE 路径与 zlib `crc32()` **逐字节一致**：长度 0..65535 全遍历、
 *            随机初值、1e7 随机数据随机分段、1e7 一次性；
 *          - Castagnoli（`crc32q`）与独立的 slicing-by-8 表实现一致；
 *          - 吞吐（GB/s）同时打印硬件路径与强制表路径，机器差异由读者判断。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include <zlib.h>

#include "go2cpp/hash/crc32.hpp"

namespace {

using go2cpp::hash::AsBytes;
using go2cpp::hash::Castagnoli;
using go2cpp::hash::CastagnoliTable;
using go2cpp::hash::Crc32;
using go2cpp::hash::IEEE;
using go2cpp::hash::IEEETable;
using go2cpp::hash::Koopman;
using go2cpp::hash::KoopmanTable;
using go2cpp::hash::MakeTable;
using go2cpp::hash::Table;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("[FAIL] %s\n", what);
        ++g_failures;
    }
}

/// 独立参考实现（slicing-by-8，与被测代码同表但不同调用路径）。
std::uint32_t TableReference(std::uint32_t crc, const Table& table, const std::uint8_t* p,
                             std::size_t n) {
    const std::uint32_t* t = table.entries.data();
    crc = ~crc;
    while (n >= 8) {
        crc ^= static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
        crc = t[7 * 256 + (crc & 0xff)] ^ t[6 * 256 + ((crc >> 8) & 0xff)] ^
              t[5 * 256 + ((crc >> 16) & 0xff)] ^ t[4 * 256 + ((crc >> 24) & 0xff)] ^
              t[3 * 256 + p[4]] ^ t[2 * 256 + p[5]] ^ t[1 * 256 + p[6]] ^ t[p[7]];
        p += 8;
        n -= 8;
    }
    while (n-- != 0) {
        crc = t[(crc ^ *p) & 0xff] ^ (crc >> 8);
        ++p;
    }
    return ~crc;
}

double SecondsSince(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
}

/// 强制表驱动：polynomial = 0 时 `Crc32::Update` 不会分派到任何硬件路径。
Table ForcedTable(std::uint32_t polynomial) {
    Table table = MakeTable(polynomial);
    table.polynomial = 0;
    return table;
}

void CheckAgainstZlib() {
    std::mt19937_64 rng(0x5eed1234);
    constexpr std::size_t kTotal = 10000000;
    std::vector<std::uint8_t> data(kTotal);
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(rng());
    }

    // 1) 长度 0..65535 全遍历 + 随机初值。
    std::size_t mismatches = 0;
    for (std::size_t len = 0; len <= 65535; ++len) {
        const std::uint32_t init = static_cast<std::uint32_t>(rng());
        const std::uint32_t got = Crc32::Update(init, IEEETable, AsBytes(data.data(), len));
        const std::uint32_t want = crc32(init, data.data(), static_cast<uInt>(len));
        if (got != want) {
            if (mismatches < 5) {
                std::printf("[FAIL] ieee len=%zu init=%08x got=%08x want=%08x\n", len, init, got,
                            want);
            }
            ++mismatches;
        }
    }
    Check(mismatches == 0, "IEEE 0..65535 与 zlib 一致");
    std::printf("[zlib] 长度 0..65535 × 随机初值: %s\n", mismatches == 0 ? "一致" : "不一致");

    // 2) 1e7 随机数据随机分段，与 zlib 一次性结果对拍（验证分段拼接）。
    std::uint32_t crc = 0;
    std::size_t offset = 0;
    std::size_t segments = 0;
    while (offset < kTotal) {
        const std::size_t take = std::min<std::size_t>(kTotal - offset, 1 + rng() % 8192);
        crc = Crc32::Update(crc, IEEETable, AsBytes(data.data() + offset, take));
        offset += take;
        ++segments;
    }
    const std::uint32_t want = crc32(0, data.data(), static_cast<uInt>(kTotal));
    Check(crc == want, "IEEE 1e7 分段与 zlib 一致");
    std::printf("[zlib] 1e7 分段 (%zu 段): got=%08x want=%08x %s\n", segments, crc, want,
                crc == want ? "一致" : "不一致");

    // 3) 1e7 一次性。
    const std::uint32_t once = Crc32::ChecksumIEEE(AsBytes(data.data(), kTotal));
    Check(once == want, "IEEE 1e7 一次性与 zlib 一致");
    std::printf("[zlib] 1e7 一次性: got=%08x want=%08x %s\n", once, want,
                once == want ? "一致" : "不一致");

    // 4) Castagnoli：crc32q vs 表实现（zlib 没有 CRC-32C，用独立表实现作 oracle）。
    std::size_t cast_mismatch = 0;
    for (std::size_t len = 0; len <= 4096; ++len) {
        const std::uint32_t init = static_cast<std::uint32_t>(rng());
        const std::uint32_t got = Crc32::Update(init, CastagnoliTable, AsBytes(data.data(), len));
        const std::uint32_t ref = TableReference(init, CastagnoliTable, data.data(), len);
        if (got != ref) {
            ++cast_mismatch;
        }
    }
    Check(cast_mismatch == 0, "Castagnoli 0..4096 与表实现一致");
    std::printf("[oracle] Castagnoli 0..4096: %s\n", cast_mismatch == 0 ? "一致" : "不一致");

    // 5) Koopman 没有硬件指令，必须仍与表实现一致（分派不误伤）。
    const std::uint32_t koop = Crc32::Checksum(AsBytes(data.data(), 4096), KoopmanTable);
    const std::uint32_t koop_ref = TableReference(0, KoopmanTable, data.data(), 4096);
    Check(koop == koop_ref, "Koopman 表路径一致");
    std::printf("[oracle] Koopman 4096B: %s\n", koop == koop_ref ? "一致" : "不一致");
}

void Bench() {
    std::mt19937_64 rng(0xabcdef);
    const std::size_t sizes[] = {64, 1024, 16384, 1048576};
    std::vector<std::uint8_t> buffer(1u << 20);
    for (auto& byte : buffer) {
        byte = static_cast<std::uint8_t>(rng());
    }
    const Table ieee_forced = ForcedTable(IEEE);
    const Table cast_forced = ForcedTable(Castagnoli);

    std::printf("\n%-22s %-10s %10s %12s\n", "路径", "块大小", "GB/s", "ns/次");
    for (const std::size_t size : sizes) {
        const std::size_t iterations = std::max<std::size_t>(16, 268435456 / size);
        struct Case {
            const char* name;
            const Table* table;
        };
        const Case cases[] = {
            {"IEEE 硬件折叠", &IEEETable},
            {"IEEE 表驱动", &ieee_forced},
            {"CRC32C crc32q", &CastagnoliTable},
            {"CRC32C 表驱动", &cast_forced},
        };
        for (const Case& item : cases) {
            // 预热
            std::uint32_t sink = 0;
            for (int round = 0; round < 2; ++round) {
                sink ^= Crc32::Update(0, *item.table, AsBytes(buffer.data(), size));
            }
            const auto begin = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < iterations; ++i) {
                sink ^= Crc32::Update(0, *item.table, AsBytes(buffer.data(), size));
            }
            const double seconds = SecondsSince(begin);
            const double gbps = static_cast<double>(iterations) * static_cast<double>(size) /
                                seconds / 1e9;
            std::printf("%-22s %-10zu %10.2f %12.1f  (sink=%08x)\n", item.name, size, gbps,
                        seconds * 1e9 / static_cast<double>(iterations), sink);
        }
    }
}

}  // namespace

int main() {
    std::printf("=== CRC32 hardware path vs zlib / table ===\n");
    CheckAgainstZlib();
    Bench();
    std::printf("\n=== failures=%d ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
