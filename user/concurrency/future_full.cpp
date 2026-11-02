#include "go2cpp/runtime.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

// future 全面版：同一个组件的 6 个使用场景。
//   1  基本取值      MakePromise + 另一个 Fiber 里 SetValue，主执行流 GetResult
//   2  错误传递      SetError 后 status 变 kError，error 字段可取回
//   3  异常传递      SetException 后 status 变 kException，只有 GetOrThrow 才抛
//   4  等待超时      WaitFor 未完成时返回 kTimedOut，且不改变 Future 自身状态
//   5  Context 取消  取消后等待返回 kCancelled
//   6  Promise 被放弃  未 SetValue 就析构 → 消费者收到 BrokenPromiseError
// ★ 所有权要点：Promise 不可复制，而 go() 的 Task::Function 是 std::function
//   （要求目标可复制），所以要把 Promise 放进 shared_ptr 再捕获进 lambda。
//   直接用 auto promise = pair.first 会触发已删除的拷贝构造。

namespace {

using Ms = std::chrono::milliseconds;

// 把 Promise 搬进 shared_ptr：既满足 std::function 的可复制要求，
// 又明确表达"这个 Promise 由该 Fiber 独占"。
template <typename T>
std::shared_ptr<go2cpp::Promise<T>> Share(go2cpp::Promise<T>&& promise) {
    return std::make_shared<go2cpp::Promise<T>>(std::move(promise));
}

bool scenario_basic_value(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景1] 基本取值：另一个 Fiber 里 SetValue\n";
    auto pair = MakePromise<int>();
    const auto future = pair.second;
    auto promise = Share(std::move(pair.first));

    auto worker = go(scheduler, [promise] { (void)promise->SetValue(2026); });
    const auto result = future.GetResult();
    worker->wait();

    const bool field_ok = result.value && *result.value == 2026;
    std::cout << "  status=" << FutureStatusName(result.status)
              << " value=" << (result.value ? *result.value : -1)
              << "（预期 kReady 2026）\n";
    return result.Ok() && field_ok;
}

bool scenario_error(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景2] 错误传递：SetError（不抛异常）\n";
    auto pair = MakePromise<std::string>();
    const auto future = pair.second;
    auto promise = Share(std::move(pair.first));

    auto worker = go(scheduler, [promise] {
        (void)promise->SetError(NewError("上游返回 503"));
    });
    const auto result = future.GetResult();
    worker->wait();

    std::cout << "  status=" << FutureStatusName(result.status)
              << " 有错误=" << (result.error != nullptr) << "（预期 kError 1）\n";
    return result.status == FutureStatus::kError && result.error != nullptr;
}

bool scenario_exception(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景3] 异常传递：只有 GetOrThrow 才抛\n";
    auto pair = MakePromise<int>();
    const auto future = pair.second;
    auto promise = Share(std::move(pair.first));

    auto worker = go(scheduler, [promise] {
        try {
            throw std::runtime_error("业务异常");
        } catch (...) {
            (void)promise->SetException(std::current_exception());
        }
    });

    const auto result = future.GetResult();          // 结构化读取：不抛
    bool threw = false;
    try {
        (void)future.GetOrThrow();                   // 显式边界：这里必须抛
    } catch (const std::runtime_error&) {
        threw = true;
    }
    worker->wait();

    std::cout << "  status=" << FutureStatusName(result.status)
              << " GetOrThrow 抛出=" << threw << "（预期 kException 1）\n";
    return result.status == FutureStatus::kException && threw;
}

bool scenario_wait_timeout(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景4] 等待超时：只影响本次等待\n";
    auto pair = MakePromise<int>();
    const auto future = pair.second;
    auto promise = Share(std::move(pair.first));

    auto worker = go(scheduler, [promise] {
        std::this_thread::sleep_for(Ms(120));
        (void)promise->SetValue(7);
    });

    const auto early = future.WaitFor(Ms(20));       // 应超时
    const auto after = future.GetResult();           // 这次应等到
    worker->wait();

    std::cout << "  20ms 时=" << FutureStatusName(early)
              << " 之后=" << FutureStatusName(after.status)
              << "（预期 kTimedOut kReady）\n";
    return early == FutureStatus::kTimedOut && after.Ok();
}

bool scenario_context_cancel(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景5] Context 取消：等待返回 kCancelled\n";
    auto pair = MakePromise<int>();
    const auto future = pair.second;
    // 故意不完成：promise 留在作用域内保持 Future 挂起
    auto promise = Share(std::move(pair.first));
    (void)promise;

    auto cancel_pair = WithCancel(Background());
    const auto ctx = cancel_pair.first;
    auto canceller = go(scheduler, [cancel = cancel_pair.second] {
        std::this_thread::sleep_for(Ms(20));
        cancel();
    });

    const auto status = future.Wait(ctx);
    canceller->wait();

    std::cout << "  status=" << FutureStatusName(status) << "（预期 kCancelled）\n";
    return status == FutureStatus::kCancelled;
}

bool scenario_broken_promise() {
    using namespace go2cpp;
    std::clog << "[full][场景6] Promise 被放弃：拿到 BrokenPromiseError\n";
    auto pair = MakePromise<int>();
    Future<int> future = std::move(pair.second);
    {
        // 把 Promise 交给一个内层作用域，它析构时既没 SetValue 也没 SetError
        auto abandoned = Share(std::move(pair.first));
        (void)abandoned;
    }                                                // ← Promise 在这里析构

    const auto result = future.GetResult();
    const bool is_broken = result.error != nullptr &&
                           Is(result.error, BrokenPromiseError());
    std::cout << "  status=" << FutureStatusName(result.status)
              << " 是 BrokenPromiseError=" << is_broken << "（预期 kError 1）\n";
    return result.status == FutureStatus::kError && is_broken;
}

}  // namespace

/**
 * @brief future 全面版：6 个场景，覆盖取值、错误、异常、超时、取消与放弃。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;

    Scheduler scheduler(4);
    scheduler.start();

    bool ok = true;
    ok = scenario_basic_value(scheduler) && ok;
    ok = scenario_error(scheduler) && ok;
    ok = scenario_exception(scheduler) && ok;
    ok = scenario_wait_timeout(scheduler) && ok;
    ok = scenario_context_cancel(scheduler) && ok;
    ok = scenario_broken_promise() && ok;

    scheduler.shutdown();
    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
