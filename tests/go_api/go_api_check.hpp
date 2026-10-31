/**
 * @file go_api_check.hpp
 * @brief `tests/go_api` 的轻量断言工具（不引入 GoogleTest/Catch2）。
 *
 * 与项目既有 `tests/test_support.hpp` 保持同样的「失败计数 + 非零退出」约定，
 * 但只保留本目录需要的最小集合，避免把整个测试支撑层拉进 Go 接口测试。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace go_api_test {

inline std::atomic<int> g_failures{0};
inline std::mutex g_mutex;

/** @brief 记录一次失败（不终止进程）。 */
inline void Fail(bool condition, const char* expression, const char* file, int line,
                 const std::string& detail = {}) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "%s:%d: check failed: %s%s%s\n", file, line, expression,
                 detail.empty() ? "" : " | ", detail.c_str());
    std::fflush(stderr);
}

/** @brief 条件不满足时终止进程（用于后续依赖该条件成立的场景）。 */
inline void Require(bool condition, const char* expression, const char* file,
                    int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "%s:%d: requirement failed: %s\n", file, line, expression);
    std::fflush(stderr);
    std::exit(1);
}

/** @brief 打印场景名。 */
inline void Announce(const char* name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::printf("[scenario] %s\n", name);
    std::fflush(stdout);
}

/** @brief 返回主函数退出码。 */
inline int ExitCode() {
    if (g_failures.load(std::memory_order_relaxed) == 0) {
        std::printf("[go_api] all scenarios passed\n");
        return 0;
    }
    std::fprintf(stderr, "[go_api] %d check(s) failed\n",
                 g_failures.load(std::memory_order_relaxed));
    return 1;
}

}  // namespace go_api_test

#define GO_CHECK(condition) \
    ::go_api_test::Fail((condition), #condition, __FILE__, __LINE__)

#define GO_CHECK_MSG(condition, detail) \
    ::go_api_test::Fail((condition), #condition, __FILE__, __LINE__, (detail))

#define GO_REQUIRE(condition) \
    ::go_api_test::Require((condition), #condition, __FILE__, __LINE__)

#define GO_ANNOUNCE(name) ::go_api_test::Announce(name)
