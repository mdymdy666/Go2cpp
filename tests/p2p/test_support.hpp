#pragma once

/**
 * @file test_support.hpp
 * @brief P2P 测试共用断言与统计工具（无外部测试框架依赖）。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace p2p_test {

inline int g_checks = 0;
inline int g_failures = 0;
inline int g_skips = 0;

inline void Check(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s:%d: %s\n", file, line, expression);
    }
}

inline void CheckMsg(bool condition, const char* what, const char* file, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s:%d: %s\n", file, line, what);
    }
}

inline void Skip(const std::string& reason) {
    ++g_skips;
    std::printf("[SKIP] %s\n", reason.c_str());
}

inline int Summary(const char* name) {
    std::printf("\n[%s] checks=%d failures=%d skips=%d\n", name, g_checks, g_failures, g_skips);
    return g_failures == 0 ? 0 : 1;
}

inline double Percentile(std::vector<double> samples, double percentile) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t index = static_cast<std::size_t>(
        std::min<double>(static_cast<double>(samples.size() - 1),
                         percentile * static_cast<double>(samples.size())));
    return samples[index];
}

}  // namespace p2p_test

#define P2P_CHECK(condition) ::p2p_test::Check((condition), #condition, __FILE__, __LINE__)
#define P2P_CHECK_MSG(condition, message) \
    ::p2p_test::CheckMsg((condition), (message), __FILE__, __LINE__)
