/**
 * @file test_error_fiber.cpp
 * @brief `ErrorLocal` / `error_fiber.hpp` 的 Fiber 迁移验收测试。
 *
 * @details 验证三件事：
 *  1. 安装 fiber 后端后，`ErrorLocal` 的值绑定 G：G 从 M1 挂起、在 M2 恢复后
 *     读到的错误状态完全一致（指针、码、文本都不变）；
 *  2. 析构工作表随 G 走：在 M1 构造的十万层错误链，在 M2 上析构不爆栈、不留
 *     残留节点；
 *  3. 不退化：普通线程仍然使用线程局部语义，两个线程互不可见；Fiber 结束后
 *     值被回收，新 Fiber 看不到旧值。
 *
 * 运行：见 tests/error/CMakeLists.txt 或 `tests/error/run.sh`。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/error.hpp"
#include "go2cpp/error/error_fiber.hpp"

#include "go2cpp/concurrency/fiber.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

// 测试诊断用：错误码直接流入 CHECK_EQ 的失败信息。
std::ostream& operator<<(std::ostream& out, go2cpp::error::ErrorCode code) {
    return out << static_cast<std::int32_t>(code);
}

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

using namespace go2cpp::error;
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
long CurrentThreadId() noexcept { return static_cast<long>(::syscall(SYS_gettid)); }

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
        const bool suspended = gate.condition.wait_for(lock, 5s, [&] { return gate.suspended; });
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
// 1. 后端安装与线程退化
// ---------------------------------------------------------------------------
void TestBackendSelection() {
    CHECK(IsFiberErrorLocalEnabled());
    CHECK(CurrentErrorLocalBackend() == ErrorLocalBackendKind::kFiber);
    CHECK(InstallFiberErrorLocalStorage());

    // 普通线程：值只在本线程可见。
    ErrorLocal<int> slot;
    slot.GetOrCreate(7) = 11;
    CHECK_EQ(*slot.TryGet(), 11);

    std::atomic<int> other_value{-1};
    std::atomic<bool> other_absent{false};
    std::thread other([&] {
        other_absent.store(slot.TryGet() == nullptr, std::memory_order_release);
        other_value.store(slot.GetOrCreate(3), std::memory_order_release);
        slot.Reset();
    });
    other.join();
    CHECK(other_absent.load(std::memory_order_acquire));
    CHECK_EQ(other_value.load(std::memory_order_acquire), 3);
    CHECK_EQ(*slot.TryGet(), 11);
    slot.Reset();
    CHECK(slot.TryGet() == nullptr);
}

// ---------------------------------------------------------------------------
// 2. 错误状态随 Fiber 迁移
// ---------------------------------------------------------------------------
void TestErrorStateFollowsMigration() {
    ErrorLocal<ErrorPtr> slot;
    const ErrorPtr produced = Wrap(MakeError<TimeoutError>("peer timeout", "10.0.0.1:80", 2500),
                                   "dial");
    ErrorPtr seen_before;
    ErrorPtr seen_after;
    long tid_before = 0;
    long tid_after = 0;

    go2cpp::Fiber fiber([&] {
        tid_before = CurrentThreadId();
        SetCurrentError(produced);
        slot.GetOrCreate() = produced;
        seen_before = CurrentError();
        CHECK(seen_before != nullptr);
        CHECK_EQ(seen_before->Code(), ErrorCode::kWrappedError);
        CHECK_EQ(As<TimeoutError>(seen_before)->TimeoutMs(), std::uint64_t{2500});

        CHECK(go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Yield));

        // 恢复点：可能已经在另一个 M 上。
        tid_after = CurrentThreadId();
        seen_after = CurrentError();
        CHECK(seen_after != nullptr);
        CHECK(seen_after == seen_before); // 同一对象，不是“重建出一条相似错误”
        CHECK_EQ(seen_after->Code(), ErrorCode::kWrappedError);
        CHECK_EQ(seen_after->Message(), std::string("dial: peer timeout [path=10.0.0.1:80]"));
        CHECK(slot.TryGet() != nullptr);
        CHECK(*slot.TryGet() == produced);
    });

    const auto [first, second] = RunAcrossThreads(fiber);
    CHECK(fiber.state() == go2cpp::FiberState::Completed);
    CHECK(first != second); // 确实跨 M 恢复
    CHECK(tid_before != tid_after);
    CHECK(seen_before == produced);
    CHECK(seen_after == produced);
    CHECK(CurrentError() == nullptr); // Fiber 的状态没有泄漏到主线程
    CHECK(slot.TryGet() == nullptr);
}

// ---------------------------------------------------------------------------
// 3. Fiber 之间的隔离与结束回收
// ---------------------------------------------------------------------------
void TestFiberIsolationAndCleanup() {
    ErrorLocal<ErrorPtr> slot;
    const ErrorPtr first_error = MakeError<TypeMismatchError>("first", "int", "string");
    const ErrorPtr second_error = MakeError<CancelledError>("second", "ctx");
    ErrorPtr seen_by_first_after_second;

    go2cpp::Fiber first([&] {
        slot.GetOrCreate() = first_error;
        CHECK(go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Yield));
        seen_by_first_after_second = *slot.TryGet();
    });

    CHECK(first.resume());
    {
        go2cpp::Fiber second([&] {
            // 新 Fiber 看不到第一条 Fiber 的值。
            CHECK(slot.TryGet() == nullptr);
            slot.GetOrCreate() = second_error;
            CHECK_EQ((*slot.TryGet())->Code(), ErrorCode::kCancelled);
        });
        CHECK(second.resume());
        CHECK(second.state() == go2cpp::FiberState::Completed);
    }
    CHECK(first.resume());
    CHECK(first.state() == go2cpp::FiberState::Completed);
    CHECK(seen_by_first_after_second == first_error);
    CHECK(slot.TryGet() == nullptr);
    CHECK(CurrentError() == nullptr);
}

// ---------------------------------------------------------------------------
// 4. 深链析构工作表随 G 迁移
// ---------------------------------------------------------------------------
void TestDestroyTableFollowsMigration() {
    constexpr int kDepth = 50000;
    long tid_before = 0;
    long tid_after = 0;
    std::size_t pending_after = 1;
    bool draining_after = true;

    go2cpp::Fiber fiber([&] {
        tid_before = CurrentThreadId();
        ErrorPtr chain = NewError("leaf", ErrorCode::kCorruption);
        for (int index = 0; index < kDepth; ++index) {
            chain = Wrap(std::move(chain), "layer");
        }
        CHECK(chain != nullptr);
        CHECK(go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Yield));
        tid_after = CurrentThreadId();
        // 在迁移后的 M 上析构整条链：外层循环必须使用当前上下文的工作表。
        chain.reset();
        const auto* table = detail::DestroyTable().TryGet();
        pending_after = table != nullptr ? table->pending.size() : 0;
        draining_after = table != nullptr ? table->draining : false;
    });

    const auto [first, second] = RunAcrossThreads(fiber);
    CHECK(first != second);
    CHECK(tid_before != tid_after);
    CHECK(fiber.state() == go2cpp::FiberState::Completed);
    CHECK_EQ(pending_after, std::size_t{0});
    CHECK(!draining_after);
}

// ---------------------------------------------------------------------------
// 5. 旧体系释放表在 Fiber 内可用
// ---------------------------------------------------------------------------
void TestLegacyReleaseTableInFiber() {
    std::atomic<bool> destroyed{false};
    go2cpp::Fiber fiber([&] {
        ::go2cpp::ErrorPtr chain = ::go2cpp::NewError("leaf");
        for (int index = 0; index < 20000; ++index) {
            chain = ::go2cpp::Wrap(std::move(chain), "layer");
        }
        CHECK(chain != nullptr);
        CHECK(go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Yield));
        chain.reset();
        destroyed.store(true, std::memory_order_release);
    });

    const auto [first, second] = RunAcrossThreads(fiber);
    CHECK(first != second);
    CHECK(fiber.state() == go2cpp::FiberState::Completed);
    CHECK(destroyed.load(std::memory_order_acquire));
    CHECK(CurrentError() == nullptr);
}

} // namespace

int main() {
    std::cout << "=== go2cpp::error fiber-local tests ===\n";
    RunTest("backend_selection", TestBackendSelection);
    RunTest("error_state_follows_migration", TestErrorStateFollowsMigration);
    RunTest("fiber_isolation_and_cleanup", TestFiberIsolationAndCleanup);
    RunTest("destroy_table_follows_migration", TestDestroyTableFollowsMigration);
    RunTest("legacy_release_table_in_fiber", TestLegacyReleaseTableInFiber);

    const int failures = g_failures.load();
    std::cout << "=== " << (failures == 0 ? "PASSED" : "FAILED") << ": 5 tests, "
              << g_checks.load() << " checks, " << failures << " failures ===\n";
    return failures == 0 ? 0 : 1;
}
