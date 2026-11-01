#pragma once

/**
 * @file media_test_util.hpp
 * @brief 媒体测试共用的断言、跳过与收尾（CTest 原生 SKIP 语义）。
 * @details 与 `tests/bento/test_util.hpp` 保持同样的语义，但不依赖 Bento 的
 *          Link 头文件（媒体测试只链接 go2cpp_media）。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace media_test {

inline int g_failures = 0;
inline int g_checks = 0;
inline int g_skips = 0;

/// CTest 原生 SKIP：进程以该码退出表示环境不满足、用例未执行。
inline constexpr int kSkipReturnCode = 77;

/// @brief 标记一个环境不满足的用例。
inline void Skip(const char* reason) {
    std::printf("[SKIP] %s\n", reason);
    ++g_skips;
}

/// @brief 记录一次断言失败（不中断，便于一次看到全部问题）。
#define MEDIA_CHECK(condition)                                                          \
    do {                                                                                \
        ++media_test::g_checks;                                                         \
        if (!(condition)) {                                                             \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++media_test::g_failures;                                                   \
        }                                                                               \
    } while (0)

#define MEDIA_CHECK_EQ(actual, expected)                                                  \
    do {                                                                                  \
        ++media_test::g_checks;                                                           \
        if (!((actual) == (expected))) {                                                  \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual, \
                         #expected);                                                      \
            ++media_test::g_failures;                                                     \
        }                                                                                 \
    } while (0)

/// @brief 大于等于。
#define MEDIA_CHECK_GE(actual, bound)                                                     \
    do {                                                                                  \
        ++media_test::g_checks;                                                           \
        if (!((actual) >= (bound))) {                                                     \
            std::fprintf(stderr, "[FAIL] %s:%d: %s >= %s\n", __FILE__, __LINE__, #actual, \
                         #bound);                                                         \
            ++media_test::g_failures;                                                     \
        }                                                                                 \
    } while (0)

/// @brief 收尾：有失败返回 1；全部跳过且零检查返回 77；否则返回 0。
inline int Finish(const char* name) {
    if (g_failures > 0) {
        std::printf("[%s] FAILED (%d checks, %d failures)\n", name, g_checks, g_failures);
        return 1;
    }
    if (g_checks == 0) {
        std::printf("[%s] SKIPPED\n", name);
        return kSkipReturnCode;
    }
    std::printf("[%s] OK (%d checks)\n", name, g_checks);
    return 0;
}

}  // namespace media_test
