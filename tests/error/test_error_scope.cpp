/**
 * @file test_error_scope.cpp
 * @brief `ErrorScope` 的验收测试：多重收集、Fiber 隔离、退出自动抽干、线程退化。
 *
 * @details 覆盖四个场景：
 *  1. 同一上下文 Add 3 条（其中一条带重复指针、一条空指针）→ `TakeAll()` 返回
 *     3 条并清空，顺序/上下文/错误文本/时间戳正确；
 *  2. 10 个 Fiber 并发，各自 Add 5 条、互不串味（各自 `TakeAll()` 5 条）；
 *  3. Fiber 不手动 `TakeAll()` 就结束 → trampoline 自动 drain，日志 sink 收到
 *     3 条 WARN；新 Fiber 看不到上一条 Fiber 的收集器；
 *  4. 普通线程（无 Fiber）退化为线程局部，4 个线程各自收集/取走互不影响。
 *
 * 另外验证运行期关闭自动抽干后不再打印、恢复后重新打印。
 *
 * 运行：见 tests/error/CMakeLists.txt（`error_scope_test`）。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#include "go2cpp/error/error_scope.hpp"

#include "go2cpp/concurrency/fiber.hpp"
#include "go2cpp/error/error_fiber.hpp"
#include "go2cpp/logging/log.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

std::atomic<int> g_failures{0};
std::atomic<int> g_checks{0};
std::mutex g_output_mutex;

void ReportFailure(const char* expression, const char* file, int line, const std::string& detail) {
    g_failures.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cerr << file << ':' << line << ": CHECK failed: " << expression;
    if (!detail.empty()) {
        std::cerr << "  [" << detail << ']';
    }
    std::cerr << '\n';
}

} // namespace

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        g_checks.fetch_add(1);                                                                     \
        if (!(condition)) {                                                                        \
            ReportFailure(#condition, __FILE__, __LINE__, std::string());                          \
        }                                                                                          \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        g_checks.fetch_add(1);                                                                     \
        const auto check_actual = (actual);                                                        \
        const auto check_expected = (expected);                                                    \
        if (!(check_actual == check_expected)) {                                                   \
            std::ostringstream check_detail;                                                       \
            check_detail << "actual=" << check_actual << " expected=" << check_expected;           \
            ReportFailure(#actual " == " #expected, __FILE__, __LINE__, check_detail.str());       \
        }                                                                                          \
    } while (false)

namespace {

using go2cpp::Fiber;
using go2cpp::FiberState;
using go2cpp::error::ErrorPtr;
using go2cpp::error::ErrorScope;
using go2cpp::error::MakeError;
using go2cpp::error::StringError;
using go2cpp::log::Level;
using go2cpp::log::LogItemWorker;
using go2cpp::log::LoggerManager;
using go2cpp::log::MemorySink;
using go2cpp::log::MinimumLevelFilter;
using go2cpp::log::PatternFormatter;

/// @brief 造一条可辨识的文本错误。
ErrorPtr MakeText(std::string message) {
    return MakeError<StringError>(std::move(message));
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string joined;
    for (const std::string& line : lines) {
        joined += line;
        joined += '\n';
    }
    return joined;
}

// ---------------------------------------------------------------------------
// 1. 收集、不去重、取走并清空
// ---------------------------------------------------------------------------
void TestAddAndTakeAll() {
    auto& scope = ErrorScope::Current();
    scope.Clear();
    CHECK(scope.empty());

    scope.Add(MakeText("first"), "stage-1");
    scope.Add(MakeText("second"), "stage-2");
    scope.Add(MakeText("third"));
    CHECK(!scope.empty());
    CHECK_EQ(scope.size(), std::size_t{3});

    const std::vector<ErrorScope::Entry> entries = scope.TakeAll();
    CHECK_EQ(entries.size(), std::size_t{3});
    CHECK(scope.empty());
    CHECK_EQ(scope.size(), std::size_t{0});

    CHECK(entries[0].error != nullptr);
    CHECK_EQ(entries[0].error->Message(), std::string("first"));
    CHECK_EQ(entries[0].context, std::string("stage-1"));
    CHECK_EQ(entries[1].context, std::string("stage-2"));
    CHECK(entries[2].context.empty());
    CHECK(entries[0].at.time_since_epoch().count() > 0);

    // 不去重：同一个 ErrorPtr 收集两次就是两条；空指针被忽略。
    const ErrorPtr shared = MakeText("same");
    scope.Add(shared);
    scope.Add(shared);
    scope.Add(nullptr);
    CHECK_EQ(scope.size(), std::size_t{2});
    const std::vector<ErrorScope::Entry> duplicated = scope.TakeAll();
    CHECK_EQ(duplicated.size(), std::size_t{2});
    CHECK(duplicated[0].error == duplicated[1].error);
    CHECK(scope.empty());
}

// ---------------------------------------------------------------------------
// 2. 10 个 Fiber 并发，各自的收集器互不可见
// ---------------------------------------------------------------------------
void TestFiberIsolation() {
    constexpr int kFiberCount = 10;
    constexpr int kErrorsPerFiber = 5;
    std::atomic<int> passed{0};
    std::vector<std::thread> threads;
    threads.reserve(kFiberCount);

    for (int fiber_index = 0; fiber_index < kFiberCount; ++fiber_index) {
        threads.emplace_back([fiber_index, &passed] {
            bool ok = true;
            Fiber fiber([fiber_index, &ok] {
                ErrorScope* const existing = ErrorScope::TryCurrent();
                ok = ok && (existing == nullptr || existing->empty());
                auto& scope = ErrorScope::Current();
                for (int error_index = 0; error_index < kErrorsPerFiber; ++error_index) {
                    scope.Add(MakeText("fiber-" + std::to_string(fiber_index) + "-error-" +
                                       std::to_string(error_index)),
                              "ctx-" + std::to_string(fiber_index) + "-" +
                                  std::to_string(error_index));
                }
                ok = ok && scope.size() == static_cast<std::size_t>(kErrorsPerFiber);
                const std::vector<ErrorScope::Entry> entries = scope.TakeAll();
                ok = ok && entries.size() == static_cast<std::size_t>(kErrorsPerFiber);
                const std::string prefix = "ctx-" + std::to_string(fiber_index) + "-";
                for (const ErrorScope::Entry& entry : entries) {
                    ok = ok && entry.context.rfind(prefix, 0) == 0;
                }
                ok = ok && scope.empty();
            });
            ok = ok && fiber.resume();
            ok = ok && fiber.state() == FiberState::Completed;
            if (ok) {
                passed.fetch_add(1);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK_EQ(passed.load(), kFiberCount);

    // Fiber 结束后收集器随 G 回收，主线程看不到它们。
    const ErrorScope* const main_scope = ErrorScope::TryCurrent();
    CHECK(main_scope == nullptr || main_scope->empty());
}

// ---------------------------------------------------------------------------
// 3. 普通线程退化：线程局部，互不影响
// ---------------------------------------------------------------------------
void TestPlainThreadFallback() {
    constexpr int kThreadCount = 4;
    std::atomic<int> passed{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
        threads.emplace_back([thread_index, &passed] {
            bool ok = true;
            auto& scope = ErrorScope::Current();
            ok = ok && scope.empty();
            for (int error_index = 0; error_index < 2; ++error_index) {
                scope.Add(MakeText("thread-" + std::to_string(thread_index)),
                          "t" + std::to_string(thread_index));
            }
            ok = ok && scope.size() == std::size_t{2};
            const std::vector<ErrorScope::Entry> entries = scope.TakeAll();
            ok = ok && entries.size() == std::size_t{2};
            for (const ErrorScope::Entry& entry : entries) {
                ok = ok && entry.context == "t" + std::to_string(thread_index);
            }
            ok = ok && scope.empty();
            if (ok) {
                passed.fetch_add(1);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK_EQ(passed.load(), kThreadCount);
}

// ---------------------------------------------------------------------------
// 4. Fiber 退出自动抽干：日志 sink 收到每条错误 + 上下文
// ---------------------------------------------------------------------------
void TestAutoDrainLogs(const std::shared_ptr<MemorySink>& sink) {
    auto& main_scope = ErrorScope::Current();
    main_scope.Clear();
    const std::size_t before = sink->Snapshot().size();

    Fiber fiber([] {
        auto& scope = ErrorScope::Current();
        scope.Add(MakeText("drain-message-1"), "drain-context-1");
        scope.Add(MakeText("drain-message-2"), "drain-context-2");
        scope.Add(MakeText("drain-message-3"));
        CHECK_EQ(scope.size(), std::size_t{3});
    });
    CHECK(fiber.resume());
    CHECK(fiber.state() == FiberState::Completed);

    const std::vector<std::string> lines = sink->Snapshot();
    CHECK_EQ(lines.size(), before + 3);
    const std::string joined = JoinLines(lines);
    CHECK(joined.find("drain-context-1") != std::string::npos);
    CHECK(joined.find("drain-context-2") != std::string::npos);
    CHECK(joined.find("drain-message-1") != std::string::npos);
    CHECK(joined.find("drain-message-3") != std::string::npos);
    CHECK(joined.find("ErrorScope") != std::string::npos);
    CHECK(joined.find("WARN") != std::string::npos);

    // 下一条 Fiber 是全新上下文：自动抽干已清空并回收旧收集器。
    Fiber next([] { CHECK(ErrorScope::TryCurrent() == nullptr); });
    CHECK(next.resume());
    CHECK(next.state() == FiberState::Completed);
}

// ---------------------------------------------------------------------------
// 5. 运行期开关：关闭后不打印，恢复后继续打印
// ---------------------------------------------------------------------------
void TestAutoDrainSwitch(const std::shared_ptr<MemorySink>& sink) {
    go2cpp::error::SetErrorScopeAutoDrainEnabled(false);
    CHECK(!go2cpp::error::ErrorScopeAutoDrainEnabled());
    const std::size_t before_disabled = sink->Snapshot().size();
    Fiber disabled([] {
        ErrorScope::Current().Add(MakeText("suppressed-message"), "suppressed-context");
    });
    CHECK(disabled.resume());
    CHECK(disabled.state() == FiberState::Completed);
    CHECK_EQ(sink->Snapshot().size(), before_disabled);

    go2cpp::error::SetErrorScopeAutoDrainEnabled(true);
    CHECK(go2cpp::error::ErrorScopeAutoDrainEnabled());
    const std::size_t before_enabled = sink->Snapshot().size();
    Fiber enabled([] {
        ErrorScope::Current().Add(MakeText("enabled-message"), "enabled-context");
    });
    CHECK(enabled.resume());
    const std::vector<std::string> lines = sink->Snapshot();
    CHECK_EQ(lines.size(), before_enabled + 1);
    CHECK(JoinLines(lines).find("enabled-context") != std::string::npos);
}

void ConfigureTestLogging(const std::shared_ptr<MemorySink>& sink,
                          const std::filesystem::path& directory) {
    LoggerManager::Instance().Configure(Level::Trace, false, directory.string(), "scope.log");
    const auto logger = LoggerManager::Instance().Get("go2cpp.error");
    logger->SetLevel(Level::Trace);
    logger->SetPropagate(false);
    logger->ClearWorkers();
    logger->AddWorker(std::make_shared<LogItemWorker>(
        std::make_shared<MinimumLevelFilter>(Level::Trace),
        std::make_shared<PatternFormatter>("%p %m%n"), sink));
}

void RunTest(const char* name, const std::function<void()>& function) {
    const int failures_before = g_failures.load();
    std::cout << "[ RUN      ] " << name << std::endl;
    function();
    if (g_failures.load() == failures_before) {
        std::cout << "[       OK ] " << name << std::endl;
    } else {
        std::cout << "[  FAILED  ] " << name << std::endl;
    }
}

} // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("go2cpp-error-scope-test-" + std::to_string(static_cast<long>(::getpid())));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    auto sink = std::make_shared<MemorySink>();
    ConfigureTestLogging(sink, directory);

    RunTest("ErrorScope.AddAndTakeAll", TestAddAndTakeAll);
    RunTest("ErrorScope.FiberIsolation", TestFiberIsolation);
    RunTest("ErrorScope.PlainThreadFallback", TestPlainThreadFallback);
    RunTest("ErrorScope.AutoDrainLogs", [&sink] { TestAutoDrainLogs(sink); });
    RunTest("ErrorScope.AutoDrainSwitch", [&sink] { TestAutoDrainSwitch(sink); });

    std::filesystem::remove_all(directory);

    std::cout << (g_failures.load() == 0 ? "[  PASSED  ] " : "[  FAILED  ] ") << g_checks.load()
              << " checks" << std::endl;
    return g_failures.load() == 0 ? 0 : 1;
}
