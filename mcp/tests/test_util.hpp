/**
 * @file test_util.hpp
 * @brief 轻量断言工具：不引入 gtest，测试可执行文件直接返回非零表示失败。
 */

#pragma once

#include <cstdio>
#include <string>

namespace go2cpp::mcp::test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void Check(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    }
}

inline void CheckEqual(const std::string& actual, const std::string& expected,
                       const char* file, int line) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s:%d: expected \"%s\", got \"%s\"\n", file, line,
                     expected.c_str(), actual.c_str());
    }
}

/// 带上下文说明的断言：失败时同时打印表达式与调用方给出的诊断信息。
inline void CheckWithMessage(bool condition, const char* expression,
                             const char* message, const char* file, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s:%d: %s — %s\n", file, line, expression,
                     message != nullptr ? message : "");
    }
}

/// 打印统计并返回进程退出码。
inline int Summary(const char* suite) {
    std::printf("[%s] %d 项断言，失败 %d\n", suite, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace go2cpp::mcp::test

#define CHECK(cond) ::go2cpp::mcp::test::Check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) \
    ::go2cpp::mcp::test::CheckEqual((actual), (expected), __FILE__, __LINE__)
#define CHECK_MSG(cond, message)                                            \
    ::go2cpp::mcp::test::CheckWithMessage(                                  \
        (cond), #cond, std::string(message).c_str(), __FILE__, __LINE__)
