#include "go2cpp/future.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

/**
 * @brief 演示 Promise/Future 的成功、超时、错误、异常与放弃路径。
 * @details 运行时会一直返回结构化结果；只有显式调用 GetOrThrow 才会把
 *          error/exception 转换成 C++ 异常。
 * @return 所有场景按预期完成时返回 0。
 */
int main() {
    using namespace go2cpp;
    using namespace std::chrono_literals;

    // 场景一（成功路径）：生产者线程发布值，消费者拿到结构化结果。
    auto ready_pair = MakePromise<int>();
    std::thread producer([&ready_pair] {
        std::this_thread::sleep_for(10ms);
        (void)ready_pair.first.SetValue(7);
    });
    const auto ready = ready_pair.second.GetResultFor(2s);
    producer.join();
    const bool ready_ok = ready.Ok() && ready.Value() != nullptr &&
                          *ready.Value() == 7;
    std::cout << "[future][场景一] status=" << FutureStatusName(ready.status)
              << " value=" << (ready.Value() ? *ready.Value() : -1)
              << "（预期 kReady 且 value=7）\n";

    // 场景二（等待/超时路径）：未完成的 Future 按时限返回，不无限阻塞。
    auto pending_pair = MakePromise<int>();
    const auto timed_out = pending_pair.second.GetResultFor(20ms);
    const bool timeout_ok = timed_out.TimedOut();
    std::cout << "[future][场景二] status=" << FutureStatusName(timed_out.status)
              << " timed_out=" << timed_out.TimedOut() << "（预期 kTimedOut）\n";

    // 场景三（错误路径）：SetError 让消费者读错误，而不是抛异常。
    (void)pending_pair.first.SetError(NewError("producer failed"));
    const auto errored = pending_pair.second.GetResultFor(2s);
    const bool error_ok = errored.status == FutureStatus::kError &&
                          errored.error != nullptr;
    std::cout << "[future][场景三] status=" << FutureStatusName(errored.status)
              << " message="
              << (errored.error ? ErrorMessage(errored.error) : std::string{})
              << "（预期 kError）\n";

    // 场景四（异常路径）：只有 GetOrThrow 才把异常交回调用者。
    auto throwing_pair = MakePromise<int>();
    (void)throwing_pair.first.SetException(
        std::make_exception_ptr(std::runtime_error("boom")));
    bool caught = false;
    try {
        (void)throwing_pair.second.GetOrThrow();
    } catch (const std::exception& exception) {
        caught = true;
        std::cout << "[future][场景四] 捕获异常: " << exception.what() << '\n';
    }

    // 场景五（生命周期边界）：Promise 先析构时消费者收到 broken promise。
    auto abandoned_pair = MakePromise<int>();
    auto abandoned_future = abandoned_pair.second;
    {
        auto promise = std::move(abandoned_pair.first);
        (void)promise;
    }
    const auto broken = abandoned_future.GetResultFor(2s);
    const bool broken_ok = broken.status == FutureStatus::kError;
    std::cout << "[future][场景五] status=" << FutureStatusName(broken.status)
              << " message="
              << (broken.error ? ErrorMessage(broken.error) : std::string{})
              << "（预期 kError / broken promise）\n";

    const bool all = ready_ok && timeout_ok && error_ok && caught && broken_ok;
    std::cout << "[future][汇总] ready=" << ready_ok << " timeout=" << timeout_ok
              << " error=" << error_ok << " exception=" << caught
              << " broken=" << broken_ok << " all=" << std::boolalpha << all
              << '\n';
    return all ? 0 : 1;
}
