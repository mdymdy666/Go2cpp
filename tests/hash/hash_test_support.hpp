/**
 * @file hash_test_support.hpp
 * @brief `hash` 模块测试共享的断言、十六进制工具、确定性数据生成器。
 *
 * @details 所有测试 TU 共用一个可执行文件（`go2cpp_hash_test`）。本头提供：
 *   - `HASH_CHECK` / `HASH_REQUIRE` / `HASH_CHECK_HEX` 断言宏；
 *   - 摘要 → 十六进制、十六进制 → 字节的转换；
 *   - 与 `tests/hash/scripts/gen_oracle.py` **逐字节一致**的 LCG 数据生成器。
 *
 *          数据生成器必须两边一致，否则「随机测试」会退化成「两边各测各的」：
 *          ```
 *          state = seed
 *          state = state * 6364136223846793005 + 1442695040888963407  (mod 2^64)
 *          byte  = state >> 56
 *          ```
 *          修改此处必须同步修改 Python 脚本并重新生成 oracle.tsv。
 */
#pragma once

#include "go2cpp/hash/adler32.hpp"
#include "go2cpp/hash/crc32.hpp"
#include "go2cpp/hash/hash.hpp"
#include "go2cpp/hash/md5.hpp"
#include "go2cpp/hash/sha1.hpp"
#include "go2cpp/hash/sha256.hpp"
#include "go2cpp/hash/sha512.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp_hash_test {

/** @brief 累计失败数；`main` 据此返回非零。 */
inline std::atomic<int> g_failures{0};

/** @brief 记录一次断言失败，不中断测试。 */
inline void Check(bool condition, const char* expression, const char* file,
                  int line) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s:%d: hash check failed: %s\n", file, line,
                 expression);
}

/** @brief 记录一次致命失败并立即中止（继续跑会掩盖真实问题）。 */
[[noreturn]] inline void Fail(const char* expression, const char* file,
                              int line) {
    std::fprintf(stderr, "%s:%d: hash REQUIRE failed: %s\n", file, line,
                 expression);
    std::fflush(stderr);
    std::abort();
}

/** @brief 打印当前测试段名，便于 ctest 输出定位。 */
inline void Announce(const char* name) {
    std::printf("[hash] %s\n", name);
    std::fflush(stdout);
}

using Clock = std::chrono::steady_clock;

/// 从 `start` 到现在的秒数（压测用）。
inline double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

/// 从 `start` 到现在的毫秒数。
inline double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
}

/// 吞吐：MiB/s（1 MiB = 1048576 字节）。
inline double MibPerSecond(std::uint64_t bytes, double seconds) {
    if (seconds <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
}

/// 吞吐：MB/s（1 MB = 1,000,000 字节，便于与厂商规格对照）。
inline double MbPerSecond(std::uint64_t bytes, double seconds) {
    if (seconds <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(bytes) / 1.0e6 / seconds;
}

/// 字节视图 → 小写十六进制串。
inline std::string Hex(go2cpp::hash::ByteSpan span) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(span.size() * 2);
    for (std::size_t i = 0; i < span.size(); ++i) {
        const std::uint8_t value = span[i];
        out.push_back(kDigits[value >> 4]);
        out.push_back(kDigits[value & 0x0fu]);
    }
    return out;
}

/// 32 位值 → 8 位十六进制串（CRC / Adler 用）。
inline std::string HexU32(std::uint32_t value) {
    char buffer[9];
    std::snprintf(buffer, sizeof(buffer), "%08x", value);
    return std::string(buffer);
}

/// 十六进制串 → 字节；非法输入返回空 vector（测试数据由本仓库生成，正常不会触发）。
inline std::vector<std::uint8_t> ParseHex(std::string_view text) {
    if (text.size() % 2 != 0) {
        return {};
    }
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = nibble(text[i]);
        const int low = nibble(text[i + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

/**
 * @brief 比较摘要与期望的十六进制串；失败时打印实际值与期望值。
 * @details 单纯 `HASH_CHECK(Hex(x) == "…")` 失败时只打印表达式，无法定位是
 *          哪一条向量错了——这里把两者都打出来。
 */
inline void CheckHex(const char* label, go2cpp::hash::ByteSpan actual,
                     std::string_view expected, const char* file, int line) {
    const std::string actual_hex = Hex(actual);
    if (actual_hex == expected) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "%s:%d: %s mismatch\n  actual  : %s\n  expected: %.*s\n",
                 file, line, label, actual_hex.c_str(),
                 static_cast<int>(expected.size()), expected.data());
}

/**
 * @brief 比较 32 位校验和与期望的 8 位十六进制串。
 */
inline void CheckU32(const char* label, std::uint32_t actual,
                     std::string_view expected, const char* file, int line) {
    const std::string actual_hex = HexU32(actual);
    if (actual_hex == expected) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s:%d: %s mismatch: actual=%s expected=%.*s\n", file,
                 line, label, actual_hex.c_str(),
                 static_cast<int>(expected.size()), expected.data());
}

/**
 * @brief 与 Python oracle 脚本一致的确定性伪随机数据生成器（Knuth MMIX LCG）。
 * @param length 字节数。
 * @param seed 64 位种子（oracle.tsv 第二列）。
 */
inline std::vector<std::uint8_t> Generate(std::size_t length,
                                          std::uint64_t seed) {
    std::vector<std::uint8_t> data(length);
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < length; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        data[i] = static_cast<std::uint8_t>(state >> 56);
    }
    return data;
}

/**
 * @brief 用任意摘要器一次性算出十六进制摘要。
 * @tparam H 满足 `Hash` 接口且可默认构造（Crc32 默认 IEEE）。
 */
template <typename H>
inline std::string DigestHex(go2cpp::hash::ByteSpan data) {
    H hash;
    hash.Write(data);
    std::vector<std::uint8_t> out(hash.Size());
    hash.Sum(go2cpp::hash::ByteSpan(out.data(), out.size()));
    return Hex(go2cpp::hash::AsBytes(out));
}

}  // namespace go2cpp_hash_test

#define HASH_CHECK(expression) \
    ::go2cpp_hash_test::Check(static_cast<bool>(expression), #expression, \
                              __FILE__, __LINE__)

#define HASH_REQUIRE(expression)                                           \
    do {                                                                    \
        if (!static_cast<bool>(expression)) {                               \
            ::go2cpp_hash_test::Fail(#expression, __FILE__, __LINE__);      \
        }                                                                   \
    } while (false)

#define HASH_CHECK_U32(label, actual, expected)                  \
    ::go2cpp_hash_test::CheckU32((label), (actual), (expected),  \
                                 __FILE__, __LINE__)

#define HASH_CHECK_HEX(label, actual, expected)                  \
    ::go2cpp_hash_test::CheckHex((label), (actual), (expected),  \
                                 __FILE__, __LINE__)
