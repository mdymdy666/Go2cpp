/**
 * @file encoding_test_support.hpp
 * @brief `encoding` 模块测试共享的断言、计时、随机数据与独立 hex 解析工具。
 *
 * @details 所有测试 TU 共用一个可执行文件（`go2cpp_encoding_test`）。本头提供：
 *   - `ENCODING_CHECK` / `ENCODING_REQUIRE` 断言宏；
 *   - `Announce` 测试段名打印与吞吐报告；
 *   - `RandomBytes`：可复现的随机字节串生成；
 *   - `FromHex`：**独立于被测模块**的十六进制解析，用于读取 Python 对拍数据，
 *     避免「用 hex 模块验证 base64 模块」的同源错误；
 *   - `TestdataPath`：ctest 跨目录运行时定位 `tests/encoding/testdata`。
 *
 * @note 测试代码不进入库，断言失败只影响当前可执行文件。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>

#ifndef GO2CPP_ENCODING_TESTDATA_DIR
#define GO2CPP_ENCODING_TESTDATA_DIR "tests/encoding/testdata"
#endif

namespace go2cpp_encoding_test {

/// 累计失败数；`main` 据此返回非零。
inline std::atomic<int> g_failures{0};

/// 记录一次断言失败，不中断测试。
inline void Check(bool condition, const char* expression, const char* file,
                  int line) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s:%d: encoding check failed: %s\n", file, line,
                 expression);
}

/// 打印当前测试段名，便于 ctest 输出定位。
inline void Announce(const char* name) {
    std::printf("[encoding] %s\n", name);
    std::fflush(stdout);
}

/// 记录一次致命失败并立即中止（继续跑会建立在损坏状态上）。
[[noreturn]] inline void Fail(const char* expression, const char* file,
                              int line) {
    std::fprintf(stderr, "%s:%d: encoding REQUIRE failed: %s\n", file, line,
                 expression);
    std::fflush(stderr);
    std::abort();
}

using Clock = std::chrono::steady_clock;

/// 从 `start` 到现在的秒数。
inline double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

/// 打印吞吐（按 1 MiB = 1048576 字节换算）。
inline void ReportThroughput(const char* label, std::size_t bytes,
                             double seconds) {
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    const double rate = seconds > 0.0 ? mib / seconds : 0.0;
    std::printf("[encoding] %-38s %8.2f MiB in %8.3f ms  %9.2f MiB/s\n", label,
                mib, seconds * 1000.0, rate);
    std::fflush(stdout);
}

/// 生成 `n` 个随机字节（固定种子由调用方传入，保证可复现）。
inline std::string RandomBytes(std::mt19937_64& rng, std::size_t n) {
    std::string out;
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<char>(rng() & 0xff);
    }
    return out;
}

/// 独立十六进制解析器（被测模块的 hex 实现不参与，避免同源验证）。
inline bool FromHex(std::string_view hex, std::string& out) {
    if (hex.size() % 2 != 0) {
        return false;
    }
    const auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = value(hex[i]);
        const int low = value(hex[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

/// 测试数据文件路径（`GO2CPP_ENCODING_TESTDATA_DIR` 由 CMake 注入）。
inline std::string TestdataPath(const char* name) {
    std::string path = GO2CPP_ENCODING_TESTDATA_DIR;
    path += '/';
    path += name;
    return path;
}

/// 按字节比较两个字符串（失败时打印长度与首个差异位置）。
inline void CheckBytesEqual(const std::string& actual, const std::string& expected,
                            const char* what, const char* file, int line) {
    if (actual == expected) {
        return;
    }
    std::size_t mismatch = 0;
    while (mismatch < actual.size() && mismatch < expected.size() &&
           actual[mismatch] == expected[mismatch]) {
        ++mismatch;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "%s:%d: encoding check failed: %s (size %zu != %zu, first diff "
                 "at %zu)\n",
                 file, line, what, actual.size(), expected.size(), mismatch);
}

}  // namespace go2cpp_encoding_test

#define ENCODING_CHECK(expression) \
    ::go2cpp_encoding_test::Check(static_cast<bool>(expression), #expression, \
                                  __FILE__, __LINE__)

#define ENCODING_CHECK_BYTES(actual, expected)                            \
    ::go2cpp_encoding_test::CheckBytesEqual(actual, expected, #actual,    \
                                            __FILE__, __LINE__)

#define ENCODING_REQUIRE(expression)                                        \
    do {                                                                    \
        if (!static_cast<bool>(expression)) {                               \
            ::go2cpp_encoding_test::Fail(#expression, __FILE__, __LINE__);  \
        }                                                                   \
    } while (false)
