#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#if defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define GO2CPP_TEST_HAS_VALGRIND 1
#endif
#endif

#if defined(__SANITIZE_THREAD__)
#define GO2CPP_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define GO2CPP_TEST_TSAN 1
#endif
#endif

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

// Keep cooperative test loops schedulable under heavy instrumentation.
inline void yield_for_watchdog() noexcept {
    std::this_thread::yield();
#if defined(GO2CPP_TEST_HAS_VALGRIND)
    if (RUNNING_ON_VALGRIND) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
}

inline void pause_for_watchdog() noexcept {
#if defined(GO2CPP_TEST_HAS_VALGRIND)
    if (RUNNING_ON_VALGRIND) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
}


[[noreturn]] inline void watchdog_abort(const char* expression,
                                        const char* file,
                                        int line) {
    std::fprintf(stderr, "[watchdog] timed out: %s (%s:%d)\n",
                 expression ? expression : "operation", file, line);
    std::fflush(stderr);
    std::abort();
}

inline void require(bool condition, const char* expression,
                    const char* file, int line) {
    if (!condition) {
        watchdog_abort(expression, file, line);
    }
}

template <typename Predicate>
bool RequireEventually(Predicate&& predicate,
                       std::chrono::steady_clock::duration timeout,
                       const char* expression,
                       const char* file, int line) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            watchdog_abort(expression, file, line);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

template <typename Rep, typename Period>
void JoinWithWatchdog(std::thread& thread,
                      std::chrono::duration<Rep, Period> timeout,
                      const char* expression,
                      const char* file, int line) {
    if (!thread.joinable()) {
        return;
    }

    struct JoinState {
        std::mutex mutex;
        std::condition_variable condition;
        bool joined{false};
    };
    const auto state = std::make_shared<JoinState>();
#if defined(GO2CPP_TEST_TSAN)
    // TSan 会为每个 Fiber/等待路径建立额外的运行时线程，普通 3 秒预算
    // 可能在宿主机调度抖动时误报；仍保留有限 watchdog，只放大预算。
    const auto watchdog_timeout = timeout * 20;
#else
    const auto watchdog_timeout = timeout;
#endif
    std::thread watchdog([state, watchdog_timeout, expression, file, line] {
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->condition.wait_for(lock, watchdog_timeout,
                                      [&] { return state->joined; })) {
            watchdog_abort(expression, file, line);
        }
    });

    thread.join();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->joined = true;
    }
    state->condition.notify_one();
    watchdog.join();
}

}  // namespace go2cpp_tests

#define GO2CPP_CHECK(expression) \
    ::go2cpp_tests::check(static_cast<bool>(expression), #expression, __FILE__, \
                          __LINE__)


#define GO2CPP_REQUIRE(expression) \
    ::go2cpp_tests::require(static_cast<bool>(expression), #expression, __FILE__, \
                            __LINE__)

#define GO2CPP_REQUIRE_EVENTUALLY(expression, timeout) \
    ::go2cpp_tests::RequireEventually( \
        [&] { return static_cast<bool>(expression); }, (timeout), #expression, \
        __FILE__, __LINE__)

#define GO2CPP_JOIN_WITH_WATCHDOG(thread, timeout) \
    ::go2cpp_tests::JoinWithWatchdog((thread), (timeout), #thread, __FILE__, __LINE__)
