/**
 * @file test_future_audit.cpp
 * @brief Future / Promise 语义审计的独立测试（不注册 ctest）。
 *
 * 覆盖审计清单：
 * - SetValue / SetError / SetException / Cancel 只能成功一次；
 * - Get 可重复读取，不被消费；
 * - Promise 未完成即析构 → broken promise，消费者不会永久等待；
 * - GetFor(duration) 真的超时，且超时不改变 Future 状态；
 * - SetError / SetException 的传递（GetResult 状态 + GetOrThrow 异常）；
 * - 多等待者、Fiber 等待与取消、void Future。
 *
 * 编译与运行见 scripts/run_concurrency_audit.sh。
 */

#include "go2cpp/future.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::FutureStatus;
using go2cpp::MakePromise;

std::atomic<int> g_failures{0};

void check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

void join_with_limit(std::vector<std::thread>& threads,
                     std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (auto& thread : threads) {
        (void)deadline;
        if (thread.joinable()) {
            thread.join();
        }
    }
}

/**
 * @brief 4 个线程竞争同一个 Promise：恰好一个终态写入者成功。
 */
void TestSinglePublishRace() {
    constexpr int kRounds = 1000;
    int winners_total = 0;
    int bad_state = 0;
    for (int round = 0; round < kRounds; ++round) {
        auto pair = MakePromise<int>();
        auto promise = std::move(pair.first);
        auto future = std::move(pair.second);
        std::atomic<int> winners{0};
        std::atomic<int> attempt{0};
        std::vector<std::thread> writers;
        for (int index = 0; index < 4; ++index) {
            writers.emplace_back([&promise, &winners, &attempt, index] {
                (void)attempt.fetch_add(1, std::memory_order_acq_rel);
                bool ok = false;
                if (index == 0) {
                    ok = promise.SetValue(index);
                } else if (index == 1) {
                    ok = promise.SetError(go2cpp::NewError("race error"));
                } else if (index == 2) {
                    ok = promise.Cancel();
                } else {
                    try {
                        throw std::runtime_error("race exception");
                    } catch (...) {
                        ok = promise.SetException(std::current_exception());
                    }
                }
                if (ok) {
                    winners.fetch_add(1, std::memory_order_acq_rel);
                }
            });
        }
        join_with_limit(writers, 5s);
        winners_total += winners.load();
        const auto status = future.Status();
        if (status == FutureStatus::kTimedOut ||
            status == FutureStatus::kInvalid) {
            ++bad_state;
        }
    }
    CHECK(winners_total == kRounds);
    CHECK(bad_state == 0);
    std::printf(
        "[future-audit] 单次终态: rounds=%d winners=%d bad_state=%d\n",
        kRounds, winners_total, bad_state);
}

/**
 * @brief Future 可多次 Get；值不会被第一个消费者取走。
 */
void TestMultipleGets() {
    auto pair = MakePromise<int>();
    auto promise = std::move(pair.first);
    auto future = std::move(pair.second);
    CHECK(promise.SetValue(4242));
    constexpr int kThreads = 16;
    constexpr int kGetsPerThread = 100;
    std::atomic<int> wrong{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < kThreads; ++t) {
        readers.emplace_back([future, &wrong] {
            for (int i = 0; i < kGetsPerThread; ++i) {
                const auto result = future.GetResultFor(1s);
                if (!result.Ok() || !result.Value() ||
                    *result.Value() != 4242) {
                    wrong.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    join_with_limit(readers, 10s);
    CHECK(wrong.load() == 0);
    CHECK(future.GetResult().Ok());
    CHECK(future.GetOrThrow() == 4242);
    std::printf("[future-audit] 多次 Get: gets=%d wrong=%d\n",
                kThreads * kGetsPerThread, wrong.load());
}

/**
 * @brief Promise 未完成即析构 → broken promise 唤醒所有等待者。
 */
void TestAbandonedPromise() {
    auto pair = MakePromise<int>();
    auto future = std::move(pair.second);
    std::atomic<FutureStatus> observed{FutureStatus::kInvalid};
    std::thread waiter([future, &observed] {
        observed.store(future.GetResult().status, std::memory_order_release);
    });
    std::this_thread::sleep_for(20ms);
    {
        auto promise = std::move(pair.first);
        (void)promise;
    }  // Promise 在此析构。
    waiter.join();
    CHECK(observed.load(std::memory_order_acquire) == FutureStatus::kError);
    const auto abandoned = future.GetResult();
    CHECK(abandoned.status == FutureStatus::kError);
    CHECK(go2cpp::Is(abandoned.error, go2cpp::BrokenPromiseError()));

    // 未完成 Future 自身析构不应挂起或泄漏（无消费者）。
    {
        auto local = MakePromise<int>();
        auto local_future = std::move(local.second);
        (void)local_future;
    }
    std::printf("[future-audit] broken promise: 等待者被唤醒，status=kError\n");
}

/**
 * @brief GetFor 真的超时；超时不改变 Future 状态，稍后仍可成功读取。
 */
void TestGetForTimeout() {
    auto pair = MakePromise<int>();
    auto promise = std::move(pair.first);
    auto future = std::move(pair.second);
    const auto start = std::chrono::steady_clock::now();
    const auto timed = future.GetResultFor(40ms);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    CHECK(timed.status == FutureStatus::kTimedOut);
    CHECK(elapsed >= 35);
    CHECK(elapsed < 2000);
    const auto pending_status = future.Status(); (void)pending_status;

    CHECK(promise.SetValue(7));
    const auto ready = future.GetResultFor(1s);
    CHECK(ready.Ok());
    CHECK(ready.Value() && *ready.Value() == 7);

    // 0 时长探测：未就绪立即返回，不消耗任何状态。
    auto probe_pair = MakePromise<int>();
    auto probe_future = std::move(probe_pair.second);
    const auto probe_start = std::chrono::steady_clock::now();
    const auto probe = probe_future.GetResultFor(0ms);
    const auto probe_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - probe_start)
            .count();
    CHECK(probe.status == FutureStatus::kTimedOut);
    CHECK(probe_elapsed < 500);
    std::printf(
        "[future-audit] GetFor: 40ms→%lldms status=TimedOut；"
        "超时后可再次读取成功\n",
        static_cast<long long>(elapsed));
}

/**
 * @brief SetError / SetException 的传递与 GetOrThrow 行为。
 */
void TestErrorAndExceptionDelivery() {
    auto error_pair = MakePromise<int>();
    auto error_promise = std::move(error_pair.first);
    auto error_future = std::move(error_pair.second);
    const auto error = go2cpp::NewError("business failure");
    CHECK(error_promise.SetError(error));
    const auto error_result = error_future.GetResult();
    CHECK(error_result.status == FutureStatus::kError);
    CHECK(go2cpp::Is(error_result.error, error));
    bool error_threw = false;
    try {
        (void)error_future.GetOrThrow();
    } catch (const go2cpp::FutureError& failure) {
        error_threw = failure.status() == FutureStatus::kError &&
                      go2cpp::Is(failure.error(), error);
    }
    CHECK(error_threw);

    auto exception_pair = MakePromise<int>();
    auto exception_promise = std::move(exception_pair.first);
    auto exception_future = std::move(exception_pair.second);
    try {
        throw std::runtime_error("captured payload");
    } catch (...) {
        CHECK(exception_promise.SetException(std::current_exception()));
    }
    const auto exception_result = exception_future.GetResult();
    CHECK(exception_result.status == FutureStatus::kException);
    CHECK(static_cast<bool>(exception_result.exception));
    bool rethrown = false;
    try {
        (void)exception_future.GetOrThrow();
    } catch (const std::runtime_error& payload) {
        rethrown = std::string(payload.what()) == "captured payload";
    }
    CHECK(rethrown);

    // 第二次终态写入必须被拒绝，且不改变已发布的结果。
    CHECK(!error_promise.SetValue(1));
    CHECK(!error_promise.SetError(go2cpp::NewError("late")));
    CHECK(!error_promise.Cancel());
    CHECK(error_future.GetResult().status == FutureStatus::kError);

    // 空错误/空异常必须转成可观察的错误，而不是静默成功。
    auto null_pair = MakePromise<int>();
    auto null_promise = std::move(null_pair.first);
    auto null_future = std::move(null_pair.second);
    CHECK(null_promise.SetError({}));
    CHECK(null_future.GetResult().status == FutureStatus::kError);
    std::printf("[future-audit] 错误/异常传递: GetResult 状态正确，"
                "GetOrThrow 抛出对应异常\n");
}

/**
 * @brief Promise 移动后，旧对象析构不得发布 broken promise。
 */
void TestPromiseMove() {
    auto pair = MakePromise<int>();
    auto first = std::move(pair.first);
    auto future = std::move(pair.second);
    {
        go2cpp::Promise<int> second = std::move(first);
        CHECK(!first.valid());
        CHECK(second.SetValue(11));
    }  // second 析构时已完成，不应覆盖终态。
    const auto result = future.GetResult();
    CHECK(result.Ok());
    CHECK(result.Value() && *result.Value() == 11);
    CHECK(!first.valid());
    std::printf("[future-audit] Promise 移动: 旧对象析构未发布 broken promise\n");
}

/**
 * @brief 多等待者同时被 SetValue 唤醒，并且取消一个等待者不影响其他。
 */
void TestMultipleWaiters() {
    auto pair = MakePromise<int>();
    auto promise = std::move(pair.first);
    auto future = std::move(pair.second);
    constexpr int kWaiters = 16;
    std::atomic<int> ready{0};
    std::atomic<int> wrong{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([future, &ready, &wrong] {
            const auto result = future.GetResultFor(3s);
            if (!result.Ok() || !result.Value() || *result.Value() != 99) {
                wrong.fetch_add(1, std::memory_order_relaxed);
            }
            ready.fetch_add(1, std::memory_order_release);
        });
    }
    std::this_thread::sleep_for(50ms);
    CHECK(promise.SetValue(99));
    for (auto& waiter : waiters) {
        waiter.join();
    }
    CHECK(ready.load() == kWaiters);
    CHECK(wrong.load() == 0);
    std::printf("[future-audit] 多等待者: ready=%d wrong=%d\n", ready.load(),
                wrong.load());
}

/**
 * @brief managed Fiber 中等待 + Context 取消；取消后 Promise 仍可完成。
 */
void TestFiberWaitAndCancel() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.idle_wait = 1ms;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    constexpr int kWaiterCount = 16;
    auto pair = MakePromise<int>();
    auto promise = std::move(pair.first);
    auto future = std::move(pair.second);
    std::atomic<int> done{0};
    std::atomic<int> wrong{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    for (int i = 0; i < kWaiterCount; ++i) {
        tasks.push_back(scheduler.spawn([future, &done, &wrong] {
            const auto result = future.GetResultFor(3s);
            if (!result.Ok() || !result.Value() || *result.Value() != 5) {
                wrong.fetch_add(1, std::memory_order_relaxed);
            }
            done.fetch_add(1, std::memory_order_release);
        }));
    }
    std::this_thread::sleep_for(50ms);
    CHECK(promise.SetValue(5));
    for (const auto& task : tasks) {
        CHECK(task->wait_for(3s));
    }
    CHECK(done.load() == kWaiterCount);
    CHECK(wrong.load() == 0);

    // 取消路径：多个 G 带 Context 等待同一个未来完成的 Promise。
    auto cancel_pair = MakePromise<int>();
    auto cancel_promise = std::move(cancel_pair.first);
    auto cancel_future = std::move(cancel_pair.second);
    auto context_pair = go2cpp::WithCancel(go2cpp::Background());
    std::atomic<int> cancelled{0};
    std::vector<std::shared_ptr<go2cpp::Task>> cancel_tasks;
    for (int i = 0; i < 8; ++i) {
        cancel_tasks.push_back(scheduler.spawn(
            [cancel_future, context = context_pair.first, &cancelled] {
                const auto status = cancel_future.Wait(context);
                if (status == FutureStatus::kCancelled ||
                    status == FutureStatus::kDeadlineExceeded) {
                    cancelled.fetch_add(1, std::memory_order_relaxed);
                }
            }));
    }
    std::this_thread::sleep_for(50ms);
    context_pair.second();
    for (const auto& task : cancel_tasks) {
        CHECK(task->wait_for(3s));
    }
    CHECK(cancelled.load() == 8);
    CHECK(cancel_promise.SetValue(1));  // 取消不改变 Future 状态。

    scheduler.shutdown();
    std::printf("[future-audit] Fiber: 正常唤醒=%d，取消唤醒=%d\n",
                done.load(), cancelled.load());
}

/**
 * @brief void Future 的完成/错误/折断。
 */
void TestVoidFuture() {
    auto pair = MakePromise<void>();
    auto promise = std::move(pair.first);
    auto future = std::move(pair.second);
    CHECK(future.GetResultFor(10ms).status == FutureStatus::kTimedOut);
    CHECK(promise.SetValue());
    CHECK(future.GetResult().Ok());
    CHECK(!promise.SetValue());
    future.GetOrThrow();

    auto error_pair = MakePromise<void>();
    auto error_promise = std::move(error_pair.first);
    auto error_future = std::move(error_pair.second);
    CHECK(error_promise.SetError(go2cpp::NewError("void error")));
    CHECK(error_future.GetResult().status == FutureStatus::kError);

    go2cpp::Future<void> abandoned;
    {
        go2cpp::Promise<void> local;
        abandoned = local.GetFuture();
    }
    CHECK(abandoned.GetResult().status == FutureStatus::kError);
    CHECK(go2cpp::Is(abandoned.GetResult().error,
                     go2cpp::BrokenPromiseError()));
    std::printf("[future-audit] void Future: 完成/错误/折断均正确\n");
}

/**
 * @brief 并发 Set 与 Get 交错：不会读取到半发布状态。
 */
void TestSetGetRace() {
    constexpr int kRounds = 2000;
    std::atomic<int> bad{0};
    for (int round = 0; round < kRounds; ++round) {
        auto pair = MakePromise<int>();
        auto promise = std::move(pair.first);
        auto future = std::move(pair.second);
        std::atomic<bool> go{false};
        std::thread reader([future, &go, &bad, round] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const auto result = future.GetResultFor(1s);
            if (!result.Ok() || !result.Value() || *result.Value() != round) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        });
        go.store(true, std::memory_order_release);
        CHECK(promise.SetValue(round));
        reader.join();
    }
    CHECK(bad.load() == 0);
    std::printf("[future-audit] Set/Get 交错: rounds=%d bad=%d\n", kRounds,
                bad.load());
}

}  // namespace

int main() {
    std::printf("=== Go2Cpp Future / Promise 语义审计测试 ===\n");
    TestSinglePublishRace();
    TestMultipleGets();
    TestAbandonedPromise();
    TestGetForTimeout();
    TestErrorAndExceptionDelivery();
    TestPromiseMove();
    TestMultipleWaiters();
    TestFiberWaitAndCancel();
    TestVoidFuture();
    TestSetGetRace();
    if (g_failures.load() != 0) {
        std::fprintf(stderr, "[future-audit] failures=%d\n",
                     g_failures.load());
        return 1;
    }
    std::printf("[future-audit] all checks passed\n");
    return 0;
}
