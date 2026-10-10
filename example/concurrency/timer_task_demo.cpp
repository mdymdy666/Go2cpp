#include "go2cpp/core/timer.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;
using go2cpp::core::Timer;
using go2cpp::core::TimerOptions;
using go2cpp::core::TimerRepeatMode;
using go2cpp::core::TimerService;

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

void print_result(const char* name, bool ok) {
    std::cout << "[timer][" << name << "] " << (ok ? "成功" : "失败")
              << '\n';
}

}  // 命名空间

/**
 * @brief 演示线程定时器、周期任务、弱引用和 Scheduler/Fiber 投递。
 * @return 所有场景通过返回 0，否则返回 1。
 */
int main() {
    std::cout << "[timer] 开始定时任务示例\n";
    std::atomic<int> exception_count{0};
    TimerService service([&](std::exception_ptr error) {
        if (error) {
            ++exception_count;
            std::cout << "[timer][异常] 回调异常已隔离，服务继续运行\n";
        }
    });

    // 场景一：普通线程执行一次性任务，并演示取消竞态。
    std::atomic<int> once_count{0};
    Timer once = service.ScheduleAfter(10ms, [&] {
        ++once_count;
        std::cout << "[timer][一次性] 在线程中执行\n";
    });
    const bool once_ok = wait_until(
        [&] { return once_count.load() == 1; }, 1s) &&
        once.StateValue() == go2cpp::core::TimerState::kCompleted;
    print_result("一次性", once_ok);

    std::atomic<bool> cancelled_called{false};
    Timer cancelled = service.ScheduleAfter(100ms, [&] {
        cancelled_called = true;
    });
    cancelled.Cancel();
    std::this_thread::sleep_for(130ms);
    const bool cancel_ok = !cancelled_called.load() && !cancelled.Active();
    print_result("取消", cancel_ok);

    // 场景一补充：业务事件提前完成，和超时任务竞争同一个句柄。
    std::atomic<bool> fallback_called{false};
    Timer fallback = service.ScheduleAfter(100ms, [&] {
        fallback_called.store(true, std::memory_order_release);
    });
    std::thread early_event([&] {
        std::this_thread::sleep_for(10ms);
        fallback.Cancel();
        std::cout << "[timer][提前完成] 业务事件赢得竞争，取消超时回调\n";
    });
    early_event.join();
    std::this_thread::sleep_for(130ms);
    const bool early_ok = !fallback_called.load() && !fallback.Active();
    print_result("提前完成", early_ok);

    // 场景二：固定速率周期任务执行三次；max_runs 到达后自动结束。
    TimerOptions repeat_options;
    repeat_options.repeat = TimerRepeatMode::kFixedRate;
    repeat_options.max_runs = 3;
    std::atomic<int> repeat_count{0};
    Timer repeated = service.ScheduleEvery(5ms, [&] {
        const int count = repeat_count.fetch_add(1) + 1;
        std::cout << "[timer][周期] 第 " << count << " 次\n";
    }, repeat_options);
    const bool repeat_ok = wait_until(
        [&] { return repeat_count.load() == 3; }, 1s) &&
        repeated.StateValue() == go2cpp::core::TimerState::kCompleted;
    print_result("固定速率", repeat_ok);

    // 场景三：动态回调根据资源状态改变下一次间隔，最后主动停止。
    std::atomic<int> reconnect_count{0};
    TimerOptions reconnect_options;
    reconnect_options.repeat = TimerRepeatMode::kFixedDelay;
    Timer reconnect = service.ScheduleRepeat(
        1ms,
        [&](const go2cpp::core::TimerContext&) {
            const int count = reconnect_count.fetch_add(1) + 1;
            std::cout << "[timer][重连] 尝试 " << count << '\n';
            if (count == 1) {
                return go2cpp::core::TimerDecision::ContinueAfter(20ms);
            }
            if (count == 2) {
                return go2cpp::core::TimerDecision::ContinueAfter(5ms);
            }
            return go2cpp::core::TimerDecision::Stop();
        },
        reconnect_options);
    const bool reconnect_ok = wait_until(
        [&] { return !reconnect.Active(); }, 1s) &&
        reconnect_count.load() == 3;
    print_result("动态重排", reconnect_ok);

    // 场景四：弱引用资源销毁后，周期回调不再访问悬空对象。
    auto resource = std::make_shared<int>(42);
    std::atomic<int> weak_count{0};
    TimerOptions weak_options;
    weak_options.repeat = TimerRepeatMode::kFixedDelay;
    Timer weak_timer = service.ScheduleWeakEvery(
        2ms, std::weak_ptr<void>(resource),
        [&](std::shared_ptr<void> value) {
            if (value) {
                ++weak_count;
                std::cout << "[timer][弱引用] 资源仍然有效\n";
            }
        },
        weak_options);
    const bool first_weak = wait_until(
        [&] { return weak_count.load() > 0; }, 1s);
    resource.reset();
    const bool weak_ok = first_weak &&
        wait_until([&] { return !weak_timer.Active(); }, 1s);
    print_result("弱引用生命周期", weak_ok);

    // 场景五：调度器接收执行后端，把到期回调变成 Fiber。
    go2cpp::Scheduler scheduler(1);
    scheduler.start();
    std::atomic<int> fiber_count{0};
    Timer fiber_timer = scheduler.schedule_after(10ms, [&] {
        ++fiber_count;
        std::cout << "[timer][Fiber] 在 Scheduler worker 中执行\n";
    });
    const bool fiber_ok = wait_until(
        [&] { return fiber_count.load() == 1; }, 1s) &&
        fiber_timer.StateValue() == go2cpp::core::TimerState::kCompleted;
    print_result("Scheduler/Fiber", fiber_ok);

    // 场景六：异常不会杀死定时器线程；空回调返回无效句柄。
    Timer throwing = service.ScheduleAfter(1ms, [] {
        throw std::runtime_error("示例异常");
    });
    const Timer invalid = service.ScheduleAfter(1ms, {});
    const bool error_ok = wait_until(
        [&] { return exception_count.load() == 1; }, 1s) &&
        throwing.StateValue() == go2cpp::core::TimerState::kFailed &&
        !invalid.Active();
    print_result("异常隔离", error_ok);

    scheduler.shutdown();
    const bool success = once_ok && cancel_ok && early_ok && repeat_ok &&
                         reconnect_ok && weak_ok && fiber_ok && error_ok;
    std::cout << "[timer] success=" << (success ? "true" : "false")
              << " once=" << once_count.load()
              << " repeat=" << repeat_count.load()
              << " fiber=" << fiber_count.load() << '\n';
    return success ? 0 : 1;
}
