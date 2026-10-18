/**
 * @file container_test_support.hpp
 * @brief `container` 模块测试共享的断言、计时与 Go 风格接口堆实现。
 *
 * @details 所有测试 TU 共用一个可执行文件（`go2cpp_container_test`），本头提供：
 *   - `CONTAINER_CHECK` / `CONTAINER_REQUIRE` 断言宏（失败会累计或立即中止）；
 *   - 高精度计时与吞吐打印工具；
 *   - `IntVectorHeap`：与 Go `container/heap` 例子同形态的用户容器，供接口式
 *     堆的单元测试、随机测试与压测复用，避免三处各写一份。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace go2cpp_container_test {

/** @brief 累计失败数；main 据此返回非零。 */
inline std::atomic<int> g_failures{0};

/** @brief 记录一次断言失败，不中断测试。 */
inline void Check(bool condition, const char* expression, const char* file,
                  int line) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s:%d: container check failed: %s\n", file, line,
                 expression);
}

/** @brief 打印当前测试段名，便于 ctest 输出定位。 */
inline void Announce(const char* name) {
    std::printf("[container] %s\n", name);
    std::fflush(stdout);
}

/** @brief 记录一次致命失败并立即中止（继续跑下去会掩盖真实问题）。 */
[[noreturn]] inline void Fail(const char* expression, const char* file, int line) {
    std::fprintf(stderr, "%s:%d: container REQUIRE failed: %s\n", file, line,
                 expression);
    std::fflush(stderr);
    std::abort();
}

using Clock = std::chrono::steady_clock;

/** @brief 从 start 到现在的毫秒数。 */
inline double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/**
 * @brief 与 Go `myHeap` 同形态的接口式最小堆。
 *
 * @details `Push` 只追加、`Pop` 只删除末尾——这正是 Go `heap.Interface` 对用户
 *          容器的要求；sift 由 `container::Push/Pop` 自由函数完成。测试用它验证
 *          接口式形态，同时也作为压测对象之一。
 */
class IntVectorHeap {
 public:
    std::size_t Len() const { return m_data.size(); }

    bool Less(std::size_t i, std::size_t j) const { return m_data[i] < m_data[j]; }

    void Swap(std::size_t i, std::size_t j) {
        std::swap(m_data[i], m_data[j]);
    }

    void Push(int value) { m_data.push_back(value); }

    int Pop() {
        const int value = m_data.back();
        m_data.pop_back();
        return value;
    }

    std::vector<int>& Data() { return m_data; }
    const std::vector<int>& Data() const { return m_data; }

 private:
    std::vector<int> m_data;
};

}  // namespace go2cpp_container_test

#define CONTAINER_CHECK(expression) \
    ::go2cpp_container_test::Check(static_cast<bool>(expression), #expression, \
                                   __FILE__, __LINE__)

#define CONTAINER_REQUIRE(expression)                                        \
    do {                                                                     \
        if (!static_cast<bool>(expression)) {                                \
            ::go2cpp_container_test::Fail(#expression, __FILE__, __LINE__);  \
        }                                                                    \
    } while (false)
