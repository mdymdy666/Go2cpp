#include "go2cpp/channel.hpp"
#include "go2cpp/core/timer.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <thread>

void run_timer_tests() {
    go2cpp_tests::announce("定时器与 Fiber 等待");
    using namespace go2cpp;
    using namespace std::chrono_literals;

    // Timer callbacks run on the timer service thread and wake a Fiber through
    // a channel; the callback does not own a Fiber stack or pretend to be M.
    Scheduler scheduler(1);
    scheduler.start();
    auto timer_channel = MakeChannel<int>(0);
    std::atomic<bool> timer_waiting{false};
    std::atomic<int> timer_value{0};
    auto timer_task = scheduler.spawn([&] {
        timer_waiting.store(true, std::memory_order_release);
        const auto result = timer_channel->RecvFor(1s);
        if (result.Ok()) {
            timer_value.store(result.ValueOrDefault(), std::memory_order_release);
        }
    });

    GO2CPP_REQUIRE_EVENTUALLY(
        timer_waiting.load(std::memory_order_acquire), 1s);
    GO2CPP_REQUIRE_EVENTUALLY(
        timer_task->state() == GState::kWaiting, 1s);
    auto timer = core::TimerService::Default().Schedule(
        core::TimerService::Clock::now() + 20ms,
        [timer_channel] { (void)timer_channel->Send(17); });
    GO2CPP_REQUIRE(timer_task->wait_for(2s));
    GO2CPP_CHECK(timer_value.load(std::memory_order_acquire) == 17);
    GO2CPP_CHECK(!timer.Active());

    // WaitForChange must use ParkingCondition. A native condition wait would
    // occupy the only M and prevent the sender below from making progress.
    auto changed_channel = MakeChannel<int>(1);
    const auto generation = changed_channel->Generation();
    std::atomic<bool> change_waiting{false};
    std::atomic<bool> change_seen{false};
    auto change_task = scheduler.spawn([&] {
        change_waiting.store(true, std::memory_order_release);
        change_seen.store(changed_channel->WaitForChange(generation, 1s),
                          std::memory_order_release);
    });
    GO2CPP_REQUIRE_EVENTUALLY(
        change_waiting.load(std::memory_order_acquire), 1s);
    GO2CPP_REQUIRE_EVENTUALLY(
        change_task->state() == GState::kWaiting, 1s);
    auto sender = scheduler.spawn([&] {
        (void)changed_channel->Send(23);
    });
    GO2CPP_REQUIRE(change_task->wait_for(2s));
    GO2CPP_REQUIRE(sender->wait_for(2s));
    GO2CPP_CHECK(change_seen.load(std::memory_order_acquire));

    // 已取消的定时器不能在稍后再次执行回调。
    std::atomic<bool> cancelled_callback{false};
    auto cancelled = core::TimerService::Default().Schedule(
        core::TimerService::Clock::now() + 100ms,
        [&] { cancelled_callback.store(true, std::memory_order_release); });
    cancelled.Cancel();
    std::this_thread::sleep_for(150ms);
    GO2CPP_CHECK(!cancelled_callback.load(std::memory_order_acquire));

    scheduler.shutdown();
}
