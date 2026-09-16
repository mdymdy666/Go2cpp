#pragma once

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>

namespace go2cpp_tests {

inline std::atomic<int> g_failures{0};
inline std::mutex g_output_mutex;

inline void check(bool condition, const char* expression, const char* file,
                  int line) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cerr << file << ':' << line << ": check failed: " << expression
              << '\n';
}

inline void announce(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cout << "[test] " << name << '\n';
}

}  // namespace go2cpp_tests

#define GO2CPP_CHECK(expression) \
    ::go2cpp_tests::check(static_cast<bool>(expression), #expression, __FILE__, \
                          __LINE__)
