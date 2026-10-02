#include "go2cpp/future.hpp"

#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/**
 * @brief 构造 Future 测试使用的单处理器调度配置。
 * @return 适合测试的 SchedulerConfig 值。
 */
go2cpp::SchedulerConfig FutureSchedulerConfig() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    config.min_workers = 1;
    config.idle_wait = 1ms;
    return config;
}

/**
 * @brief 验证普通线程设置 Future 值以及超时读取。
 * @return 无；测试失败由统一断言统计。
 */
void TestNativeValueAndTimeout() {
    auto pair = go2cpp::MakePromise<int>();
    auto promise = std::move(pair.first);
    auto future = std::move(pair.second);

    const auto timed = future.GetResultFor(5ms);
    GO2CPP_CHECK(timed.status == go2cpp::FutureStatus::kTimedOut);
    GO2CPP_CHECK(!timed.Ok());

    std::thread setter([promise = std::move(promise)]() mutable {
        std::this_thread::sleep_for(10ms);
        GO2CPP_CHECK(promise.SetValue(42));
        GO2CPP_CHECK(!promise.SetError(go2cpp::NewError("late")));
    });
    const auto result = future.GetResultFor(2s);
    GO2CPP_JOIN_WITH_WATCHDOG(setter, 3s);
    GO2CPP_CHECK(result.status == go2cpp::FutureStatus::kReady);
    GO2CPP_CHECK(result.Value() != nullptr);
    GO2CPP_CHECK(*result.Value() == 42);
    GO2CPP_CHECK(result.CopyValue().value_or(0) == 42);
}

/**
 * @brief 验证 Future 等待对 Context 取消和截止时间的响应。
 * @return 无；测试失败由统一断言统计。
 */
void TestContextCancellationAndDeadline() {
    auto cancel_pair = go2cpp::MakePromise<int>();
    auto cancel_promise = std::move(cancel_pair.first);
    auto cancel_future = std::move(cancel_pair.second);
    auto cancel_context = go2cpp::WithCancel(go2cpp::Background());

    std::atomic<go2cpp::FutureStatus> cancel_status{
        go2cpp::FutureStatus::kInvalid};
    std::thread cancel_waiter([cancel_future, context = cancel_context.first,
                               &cancel_status] {
        cancel_status.store(cancel_future.GetResult(context).status,
                            std::memory_order_release);
    });
    std::this_thread::sleep_for(10ms);
    cancel_context.second();
    GO2CPP_JOIN_WITH_WATCHDOG(cancel_waiter, 3s);
    GO2CPP_CHECK(cancel_status.load(std::memory_order_acquire) ==
                 go2cpp::FutureStatus::kCancelled);
    GO2CPP_CHECK(cancel_promise.SetValue(7));

    auto deadline_pair = go2cpp::MakePromise<int>();
    auto deadline_future = std::move(deadline_pair.second);
    auto deadline_promise = std::move(deadline_pair.first);
    auto timeout_context = go2cpp::WithTimeout(go2cpp::Background(), 20ms);
    const auto deadline_result = deadline_future.GetResult(timeout_context.first);
    GO2CPP_CHECK(deadline_result.status == go2cpp::FutureStatus::kDeadlineExceeded ||
                 deadline_result.status == go2cpp::FutureStatus::kCancelled);
    GO2CPP_CHECK(deadline_promise.Cancel());
}

/**
 * @brief 验证 managed Fiber 等待 Future 时不会阻塞承载线程。
 * @return 无；测试失败由统一断言统计。
 */
void TestManagedFiberWait() {
    go2cpp::Scheduler scheduler(FutureSchedulerConfig());
    scheduler.start();

    auto pair = go2cpp::MakePromise<int>();
    auto promise = std::move(pair.first);
    auto future = std::move(pair.second);

    std::atomic<bool> entered{false};
    std::atomic<go2cpp::FutureStatus> status{
        go2cpp::FutureStatus::kInvalid};
    std::atomic<int> value{0};

    auto task = scheduler.spawn([future, &entered, &status, &value] {
        entered.store(true, std::memory_order_release);
        const auto result = future.GetResult();
        status.store(result.status, std::memory_order_release);
        if (result.Value()) {
            value.store(*result.Value(), std::memory_order_release);
        }
    });

    GO2CPP_REQUIRE_EVENTUALLY(
        entered.load(std::memory_order_acquire), 2s);
    GO2CPP_REQUIRE_EVENTUALLY(
        task->state() == go2cpp::GState::kWaiting, 2s);
    GO2CPP_CHECK(promise.SetValue(99));
    GO2CPP_CHECK(task->wait_for(2s));
    GO2CPP_CHECK(status.load(std::memory_order_acquire) ==
                 go2cpp::FutureStatus::kReady);
    GO2CPP_CHECK(value.load(std::memory_order_acquire) == 99);
    scheduler.shutdown();
}

/**
 * @brief 验证无返回值 Future 与 Fiber 取消路径。
 * @return 无；测试失败由统一断言统计。
 */
void TestManagedCancellationAndVoid() {
    go2cpp::Scheduler scheduler(FutureSchedulerConfig());
    scheduler.start();

    auto pair = go2cpp::MakePromise<void>();
    auto promise = std::move(pair.first);
    auto future = std::move(pair.second);
    auto context = go2cpp::WithCancel(go2cpp::Background());

    std::atomic<go2cpp::FutureStatus> status{
        go2cpp::FutureStatus::kInvalid};
    auto task = scheduler.spawn([future, context = context.first, &status] {
        status.store(future.Wait(context), std::memory_order_release);
    });
    GO2CPP_REQUIRE_EVENTUALLY(
        task->state() == go2cpp::GState::kWaiting, 2s);
    context.second();
    GO2CPP_CHECK(task->wait_for(2s));
    GO2CPP_CHECK(status.load(std::memory_order_acquire) ==
                 go2cpp::FutureStatus::kCancelled);
    GO2CPP_CHECK(promise.SetValue());

    auto void_pair = go2cpp::MakePromise<void>();
    auto void_promise = std::move(void_pair.first);
    auto void_future = std::move(void_pair.second);
    GO2CPP_CHECK(void_promise.SetValue());
    GO2CPP_CHECK(void_future.GetResult().Ok());
    scheduler.shutdown();
}

/**
 * @brief 验证 Future 错误、异常、断裂 Promise 和单次发布约束。
 * @return 无；测试失败由统一断言统计。
 */
void TestErrorExceptionBrokenPromiseAndSinglePublish() {
    auto error_pair = go2cpp::MakePromise<int>();
    auto error_promise = std::move(error_pair.first);
    auto error_future = std::move(error_pair.second);
    const auto error = go2cpp::NewError("expected error");
    GO2CPP_CHECK(error_promise.SetError(error));
    const auto error_result = error_future.GetResult();
    GO2CPP_CHECK(error_result.status == go2cpp::FutureStatus::kError);
    GO2CPP_CHECK(go2cpp::Is(error_result.error, error));

    auto exception_pair = go2cpp::MakePromise<int>();
    auto exception_promise = std::move(exception_pair.first);
    auto exception_future = std::move(exception_pair.second);
    try {
        throw std::runtime_error("captured");
    } catch (...) {
        GO2CPP_CHECK(exception_promise.SetException(std::current_exception()));
    }
    const auto exception_result = exception_future.GetResult();
    GO2CPP_CHECK(exception_result.status == go2cpp::FutureStatus::kException);
    bool rethrown = false;
    try {
        (void)exception_future.GetOrThrow();
    } catch (const std::runtime_error& error_value) {
        rethrown = std::string(error_value.what()) == "captured";
    }
    GO2CPP_CHECK(rethrown);

    go2cpp::Future<int> abandoned;
    {
        go2cpp::Promise<int> promise;
        abandoned = promise.GetFuture();
    }
    const auto abandoned_result = abandoned.GetResult();
    GO2CPP_CHECK(abandoned_result.status == go2cpp::FutureStatus::kError);
    GO2CPP_CHECK(go2cpp::Is(abandoned_result.error,
                            go2cpp::BrokenPromiseError()));

    auto race_pair = go2cpp::MakePromise<int>();
    auto race_promise = std::move(race_pair.first);
    auto race_future = std::move(race_pair.second);
    std::atomic<int> winners{0};
    std::vector<std::thread> writers;
    for (int index = 0; index != 8; ++index) {
        writers.emplace_back([&race_promise, &winners, index] {
            if (race_promise.SetValue(index)) {
                winners.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& writer : writers) {
        GO2CPP_JOIN_WITH_WATCHDOG(writer, 3s);
    }
    GO2CPP_CHECK(winners.load(std::memory_order_relaxed) == 1);
    GO2CPP_CHECK(race_future.GetResult().Ok());
}

}  // namespace

/**
 * @brief 运行全部 Future 单元测试。
 * @return 无；测试失败由统一断言统计。
 */
void run_future_tests() {
    go2cpp_tests::announce("future");
    TestNativeValueAndTimeout();
    TestContextCancellationAndDeadline();
    TestManagedFiberWait();
    TestManagedCancellationAndVoid();
    TestErrorExceptionBrokenPromiseAndSinglePublish();
}
