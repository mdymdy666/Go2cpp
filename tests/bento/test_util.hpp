#pragma once

/**
 * @file test_util.hpp
 * @brief Bento 测试共用的断言、pump 辅助与延迟统计。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "go2cpp/bento/net/link.hpp"

namespace bento_test {

inline int g_failures = 0;
inline int g_checks = 0;

/// 记录一次断言失败；不中断测试，便于一次看到所有问题。
#define BENTO_CHECK(condition)                                                          \
    do {                                                                                \
        ++bento_test::g_checks;                                                         \
        if (!(condition)) {                                                             \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++bento_test::g_failures;                                                   \
        }                                                                               \
    } while (0)

#define BENTO_CHECK_EQ(actual, expected)                                                  \
    do {                                                                                  \
        ++bento_test::g_checks;                                                           \
        if (!((actual) == (expected))) {                                                  \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual, \
                         #expected);                                                      \
            ++bento_test::g_failures;                                                     \
        }                                                                                 \
    } while (0)

/// 双端交替 pump，直到谓词为真或超时。
template <typename Predicate>
bool PumpUntil(go2cpp::bento::Link& first, go2cpp::bento::Link& second, Predicate done,
               int timeout_ms) {
    const std::uint64_t deadline =
        go2cpp::bento::NowMs() + static_cast<std::uint64_t>(std::max(timeout_ms, 0));
    while (go2cpp::bento::NowMs() < deadline) {
        first.RunOnce(1);
        second.RunOnce(1);
        if (done()) {
            return true;
        }
    }
    return done();
}

/// 单端 pump。
template <typename Predicate>
bool PumpUntil(go2cpp::bento::Link& link, Predicate done, int timeout_ms) {
    const std::uint64_t deadline =
        go2cpp::bento::NowMs() + static_cast<std::uint64_t>(std::max(timeout_ms, 0));
    while (go2cpp::bento::NowMs() < deadline) {
        link.RunOnce(1);
        if (done()) {
            return true;
        }
    }
    return done();
}

/// 测试结束统一输出。
inline int Finish(const char* name) {
    if (g_failures == 0) {
        std::printf("[PASS] %s (%d checks)\n", name, g_checks);
        return 0;
    }
    std::fprintf(stderr, "[FAIL] %s: %d/%d checks failed\n", name, g_failures, g_checks);
    return 1;
}

/// 百分位（输入会被排序）。
inline std::uint64_t Percentile(std::vector<std::uint64_t> samples, double ratio) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const double index = ratio * static_cast<double>(samples.size() - 1);
    const std::size_t offset = static_cast<std::size_t>(index + 0.5);
    return samples[std::min(offset, samples.size() - 1)];
}

}  // namespace bento_test
