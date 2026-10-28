#pragma once

/**
 * @file test_error_support.hpp
 * @brief bento error/reflect 测试共用的断言与收尾输出。
 *
 * @details 与 tests/bento/test_util.hpp 分开，避免错误/反射测试被传输层
 *          （link.hpp / go2cpp_bento）的依赖拖进构建。断言不中断执行，
 *          一次运行可以看到全部失败点。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstdio>

namespace bento_error_test {

inline int g_failures = 0;
inline int g_checks = 0;

/// @brief 记录一次失败（由宏调用，保留文件/行号）。
inline void RecordFailure(const char* file, int line, const char* expression) {
    ++g_failures;
    std::fprintf(stderr, "[FAIL] %s:%d: %s\n", file, line, expression);
}

/// @brief 测试结束统一输出；返回进程退出码。
inline int Finish(const char* name) {
    if (g_failures == 0) {
        std::printf("[PASS] %s (%d checks)\n", name, g_checks);
        return 0;
    }
    std::fprintf(stderr, "[FAIL] %s: %d/%d checks failed\n", name, g_failures, g_checks);
    return 1;
}

} // namespace bento_error_test

#define BENTO_ERROR_CHECK(condition)                                                               \
    do {                                                                                           \
        ++::bento_error_test::g_checks;                                                            \
        if (!(condition)) {                                                                        \
            ::bento_error_test::RecordFailure(__FILE__, __LINE__, #condition);                     \
        }                                                                                          \
    } while (0)

#define BENTO_ERROR_CHECK_EQ(actual, expected)                                                     \
    do {                                                                                           \
        ++::bento_error_test::g_checks;                                                            \
        if (!((actual) == (expected))) {                                                           \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual,           \
                         #expected);                                                                   \
            ++::bento_error_test::g_failures;                                                      \
        }                                                                                          \
    } while (0)
