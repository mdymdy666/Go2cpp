/**
 * @file test_control_flow_fiber.cpp
 * @brief `control_flow` 的 defer 状态在 Fiber 迁移下的验收测试。
 *
 * @details 验证四类行为：
 *  1. defer 边界（嵌套深度）随 G 迁移：在 M1 的 defer 回调里挂起，M2 恢复后
 *     `recover::take()` 仍被认为在 defer 边界内；
 *  2. defer 异常记录随 G 迁移：M1 记录，M2 读到同一个 `exception_ptr`；
 *  3. 隔离与回收：不同 Fiber/线程互不可见，G 结束后状态被回收；
 *  4. errno 随 G 迁移：Socket 依赖 Fiber 运行时的 errno 保存/恢复，验证该
 *     不变量（`src/network/socket.cpp` 本身没有自建 thread_local errno）。
 *
 * 运行：见 tests/error/CMakeLists.txt 或 `tests/error/run.sh`。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/control_flow.hpp"
#include "go2cpp/error/error_fiber.hpp"

#include "go2cpp/concurrency/fiber.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

std::atomic<int> g_failures{0};
std::atomic<int> g_checks{0};
std::mutex g_output_mutex;

void ReportFailure(const char* expression, const char* file, int line,
                   const std::string& detail) {
    g_failures.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cerr << file << ':' << line << ": CHECK failed: " << expression;
    if (!detail.empty()) {
        std::cerr << "  [" << detail << ']';
    }
    std::cerr << '\n';
}

} // namespace

#define CHECK(condition)                                                           \
    do {                                                                           \
        g_checks.fetch_add(1);                                                     \
        if (!(condition)) {                                                        \
            ReportFailure(#condition, __FILE__, __LINE__, std::string());          \
        }                                                                          \
    } while (false)

#define CHECK_EQ(actual, expected)                                                 \
    do {                                                                           \
        g_checks.fetch_add(1);                                                     \
        const auto check_actual = (actual);                                        \
        const auto check_expected = (expected);                                    \
        if (!(check_actual == check_expected)) {                                   \
            std::ostringstream check_detail;                                       \
            check_detail << "actual=" << check_actual << " expected="             \
                         << check_expected;                                        \
            ReportFailure(#actual " == " #expected, __FILE__, __LINE__,            \
                          check_detail.str());                                     \
        }                                                                          \
    } while (false)

namespace {

using namespace std::chrono_literals;

void RunTest(const char* name, void (*function)()) {
    const auto start = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_output_mutex);
        std::cout << "[ RUN      ] " << name << '\n';
    }
    function();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    {
        std::lock_guard<std::mutex> lock(g_output_mutex);
        std::cout << "[       OK ] " << name << " (" << elapsed << " ms)\n";
    }
}

/// @brief 当前 OS 线程 id（gettid）；不要用 std::this_thread::get_id()。
long CurrentThreadId() noexcept {
    return static_cast<long>(::syscall(SYS_gettid));
}

/// @brief 强制重新取当前 M 的 errno 槽。
/// @details 编译器可能把 `__errno_location()` 的结果缓存到寄存器并跨
///          `Fiber::Suspend` 复用；升级为 noinline 调用后每次都会重新取，
///          才能观测 Fiber 运行时的 errno 保存/恢复语义。业务代码不应依赖
///          errno 跨挂起点（见 `src/network/socket.cpp` 的逐返回点重写）。
__attribute__((noinline)) int ReadErrno() noexcept { return errno; }
__attribute__((noinline)) void WriteErrno(int value) noexcept { errno = value; }

/// @brief 跨线程恢复 Fiber 的控制块：主线程校验挂起，再交给第二个线程恢复。
struct MigrationGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool suspended = false;
    bool allow_exit = false;
};

/**
 * @brief 让 `fiber` 在 M1 跑一段、挂起、换 M2 恢复，最后等 M1 退出。
 * @return 两个 M 的线程 id（相同说明没有真正迁移，测试会失败）。
 */
std::pair<long, long> RunAcrossThreads(go2cpp::Fiber& fiber) {
    MigrationGate gate;
    long first_thread = 0;
    long second_thread = 0;

    std::thread first([&] {
        first_thread = CurrentThreadId();
        CHECK(fiber.resume());
        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            gate.suspended = true;
        }
        gate.condition.notify_all();
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.condition.wait(lock, [&] { return gate.allow_exit; });
    });

    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        const bool suspended =
            gate.condition.wait_for(lock, 5s, [&] { return gate.suspended; });
        CHECK(suspended);
    }

    std::thread second([&] {
        second_thread = CurrentThreadId();
        CHECK(fiber.resume());
    });
    second.join();

    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.allow_exit = true;
    }
    gate.condition.notify_all();
    first.join();
    return {first_thread, second_thread};
}

// ---------------------------------------------------------------------------
// 1. defer 边界随 G 迁移
// ---------------------------------------------------------------------------
void TestDeferBoundaryFollowsMigration() {
    go2cpp::panic pa;
    go2cpp::recover rec(pa);
    bool taken_before = false;
    bool in_defer_after = false;
    long tid_before = 0;
    long tid_after = 0;

    go2cpp::Fiber fiber([&] {
        tid_before = CurrentThreadId();
        go2cpp::defer boundary([&] {
            const auto value = rec.take();
            taken_before = value.has_value();
            CHECK(value.has_value());
            CHECK(go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Yield));
            tid_after = CurrentThreadId();
            in_defer_after = go2cpp::detail::InDeferCallback();
            CHECK(go2cpp::detail::InDeferCallback());
        });
        pa.call("defer boundary", 9);
    });

    const auto [first, second] = RunAcrossThreads(fiber);
    CHECK(fiber.state() == go2cpp::FiberState::Completed);
    CHECK(first != second);
    CHECK(tid_before != tid_after);
    CHECK(taken_before);
    CHECK(in_defer_after);
}

// ---------------------------------------------------------------------------
// 2. defer 异常记录随 G 迁移
// ---------------------------------------------------------------------------
void TestDeferExceptionFollowsMigration() {
    std::exception_ptr before;
    std::exception_ptr after;
    bool same = false;
    bool cleared_after = false;
    long tid_before = 0;
    long tid_after = 0;

    go2cpp::Fiber fiber([&] {
        tid_before = CurrentThreadId();
        {
            go2cpp::defer thrower([] { throw std::runtime_error("defer boom"); });
        }
        before = go2cpp::LastDeferException();
        CHECK(before != nullptr);
        CHECK(go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Yield));
        tid_after = CurrentThreadId();
        after = go2cpp::LastDeferException();
        same = (after == before);
        go2cpp::ClearDeferException();
        cleared_after = (go2cpp::LastDeferException() == nullptr);
    });

    const auto [first, second] = RunAcrossThreads(fiber);
    CHECK(fiber.state() == go2cpp::FiberState::Completed);
    CHECK(first != second);
    CHECK(tid_before != tid_after);
    CHECK(before != nullptr);
    CHECK(after != nullptr);
    CHECK(same);
    CHECK(cleared_after);
    CHECK(go2cpp::LastDeferException() == nullptr);
}

// ---------------------------------------------------------------------------
// 3. Fiber 之间的隔离
// ---------------------------------------------------------------------------
void TestDeferStateIsolatedBetweenFibers() {
    go2cpp::ClearDeferException();
    bool a_suspended = false;
    bool a_in_defer_after_resume = false;
    bool b_in_defer = true;
    std::exception_ptr b_exception;

    go2cpp::Fiber a([&] {
        go2cpp::defer boundary([&] {
            a_suspended = true;
            CHECK(go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Yield));
            a_in_defer_after_resume = go2cpp::detail::InDeferCallback();
        });
    });
    CHECK(a.resume());
    CHECK(a_suspended);

    // A 挂起在 defer 回调里；主线程与新建的 B 都不应看到 A 的边界/异常。
    CHECK(!go2cpp::detail::InDeferCallback());
    go2cpp::Fiber b([&] {
        b_in_defer = go2cpp::detail::InDeferCallback();
        b_exception = go2cpp::LastDeferException();
    });
    CHECK(b.resume());
    CHECK(b.state() == go2cpp::FiberState::Completed);
    CHECK(!b_in_defer);
    CHECK(b_exception == nullptr);

    CHECK(a.resume());
    CHECK(a.state() == go2cpp::FiberState::Completed);
    CHECK(a_in_defer_after_resume);
}

// ---------------------------------------------------------------------------
// 4. G 结束后状态回收
// ---------------------------------------------------------------------------
void TestDeferStateReclaimedAfterFiber() {
    go2cpp::ClearDeferException();
    go2cpp::Fiber fiber([&] {
        {
            go2cpp::defer thrower([] { throw std::runtime_error("reclaim"); });
        }
        CHECK(go2cpp::LastDeferException() != nullptr);
        CHECK(!go2cpp::detail::InDeferCallback());
    });
    CHECK(fiber.resume());
    CHECK(fiber.state() == go2cpp::FiberState::Completed);
    CHECK(go2cpp::LastDeferException() == nullptr);
    CHECK(!go2cpp::detail::InDeferCallback());

    bool next_saw_exception = true;
    bool next_in_defer = true;
    go2cpp::Fiber next([&] {
        next_saw_exception = (go2cpp::LastDeferException() != nullptr);
        next_in_defer = go2cpp::detail::InDeferCallback();
    });
    CHECK(next.resume());
    CHECK(!next_saw_exception);
    CHECK(!next_in_defer);
}

// ---------------------------------------------------------------------------
// 5. 线程隔离（默认线程后端语义不退化）
// ---------------------------------------------------------------------------
void TestDeferExceptionThreadIsolation() {
    go2cpp::ClearDeferException();
    {
        go2cpp::defer thrower([] { throw std::runtime_error("main thread"); });
    }
    CHECK(go2cpp::LastDeferException() != nullptr);

    std::exception_ptr seen_in_worker;
    bool worker_saw_own = false;
    std::thread worker([&] {
        seen_in_worker = go2cpp::LastDeferException();
        {
            go2cpp::defer thrower([] { throw std::runtime_error("worker"); });
        }
        worker_saw_own = (go2cpp::LastDeferException() != nullptr);
    });
    worker.join();
    CHECK(seen_in_worker == nullptr);
    CHECK(worker_saw_own);
    CHECK(go2cpp::LastDeferException() != nullptr);
    go2cpp::ClearDeferException();
    CHECK(go2cpp::LastDeferException() == nullptr);
}

// ---------------------------------------------------------------------------
// 6. errno 随 G 迁移（Socket 的 errno 语义依赖）
// ---------------------------------------------------------------------------
void TestErrnoFollowsMigration() {
    long tid_before = 0;
    long tid_after = 0;
    int before = 0;
    int after = 0;

    go2cpp::Fiber fiber([&] {
        tid_before = CurrentThreadId();
        WriteErrno(EAGAIN);
        before = ReadErrno();
        CHECK(go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Yield));
        tid_after = CurrentThreadId();
        after = ReadErrno();
    });

    const auto [first, second] = RunAcrossThreads(fiber);
    CHECK(fiber.state() == go2cpp::FiberState::Completed);
    CHECK(first != second);
    CHECK(tid_before != tid_after);
    CHECK_EQ(before, EAGAIN);
    CHECK_EQ(after, EAGAIN);
}

// ---------------------------------------------------------------------------
// 7. 嵌套 defer 深度随 G 迁移
// ---------------------------------------------------------------------------
void TestNestedDeferFollowsMigration() {
    bool outer_in_defer = false;
    bool inner_in_defer_after = false;
    long tid_before = 0;
    long tid_after = 0;

    go2cpp::Fiber fiber([&] {
        tid_before = CurrentThreadId();
        go2cpp::defer outer([&] {
            outer_in_defer = go2cpp::detail::InDeferCallback();
            go2cpp::defer inner([&] {
                CHECK(go2cpp::detail::InDeferCallback());
                CHECK(go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Yield));
                tid_after = CurrentThreadId();
                inner_in_defer_after = go2cpp::detail::InDeferCallback();
            });
        });
    });

    const auto [first, second] = RunAcrossThreads(fiber);
    CHECK(fiber.state() == go2cpp::FiberState::Completed);
    CHECK(first != second);
    CHECK(tid_before != tid_after);
    CHECK(outer_in_defer);
    CHECK(inner_in_defer_after);
}

} // namespace

int main() {
    std::cout << "=== go2cpp::control_flow defer state fiber tests ===\n";
    RunTest("defer_boundary_follows_migration", TestDeferBoundaryFollowsMigration);
    RunTest("defer_exception_follows_migration", TestDeferExceptionFollowsMigration);
    RunTest("defer_state_isolated_between_fibers", TestDeferStateIsolatedBetweenFibers);
    RunTest("defer_state_reclaimed_after_fiber", TestDeferStateReclaimedAfterFiber);
    RunTest("defer_exception_thread_isolation", TestDeferExceptionThreadIsolation);
    RunTest("errno_follows_migration", TestErrnoFollowsMigration);
    RunTest("nested_defer_follows_migration", TestNestedDeferFollowsMigration);

    const int failures = g_failures.load();
    std::cout << "=== " << (failures == 0 ? "PASSED" : "FAILED") << ": 7 tests, "
              << g_checks.load() << " checks, " << failures << " failures ===\n";
    return failures == 0 ? 0 : 1;
}
