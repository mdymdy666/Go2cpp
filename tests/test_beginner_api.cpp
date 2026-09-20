#include "go2cpp/event.hpp"
#include "go2cpp/go.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <stdexcept>

void run_beginner_api_tests() {
    go2cpp_tests::announce("新手 go/fiber/select 接口");
    using namespace go2cpp;
    using namespace std::chrono_literals;

    Scheduler scheduler(1);
    scheduler.start();

    std::atomic<int> go_value{0};
    auto go_task = go(scheduler, [&] { go_value.store(7, std::memory_order_release); });
    GO2CPP_REQUIRE(go_task->wait_for(2s));
    GO2CPP_CHECK(go_value.load(std::memory_order_acquire) == 7);

    std::atomic<int> fiber_value{0};
    fiber beginner_fiber([&] { fiber_value.store(9, std::memory_order_release); });
    auto fiber_task = scheduler.add(beginner_fiber);
    GO2CPP_REQUIRE(fiber_task);
    GO2CPP_REQUIRE(fiber_task->wait_for(2s));
    GO2CPP_CHECK(fiber_value.load(std::memory_order_acquire) == 9);

    auto channel = MakeChannel<int>(0);
    SelectLoop events;
    std::atomic<int> handled{0};
    events.bind(RecvCase(channel), [&](const SelectResult& result) {
        const auto value = result.Value<int>();
        if (value.has_value()) {
            handled.store(*value, std::memory_order_release);
        }
    });
    auto sender = go(scheduler, [channel] { (void)channel->Send(42); });
    const auto selected = events.work({}, 2s);
    GO2CPP_REQUIRE(sender->wait_for(2s));
    GO2CPP_CHECK(selected.selected);
    GO2CPP_CHECK(handled.load(std::memory_order_acquire) == 42);
    GO2CPP_CHECK(events.happened());
    GO2CPP_CHECK(events.handle().has_value());

    // EventBatch/SelectLoop 在 Fiber 中等待时不能占住唯一的 M；发送者
    // 仍应获得运行机会。处理回调抛出的异常要转成错误结果。
    auto fiber_channel = MakeChannel<int>(0);
    std::atomic<bool> fiber_selected{false};
    SelectLoop fiber_events;
    auto fiber_waiter = scheduler.spawn([&] {
        fiber_events.bind(RecvCase(fiber_channel),
                          [&](const SelectResult& result) {
                              fiber_selected.store(result.selected,
                                                   std::memory_order_release);
                          });
        const auto result = fiber_events.work({}, 2s);
        GO2CPP_CHECK(result.selected);
    });
    GO2CPP_REQUIRE_EVENTUALLY(
        fiber_waiter->state() == GState::kWaiting, 1s);
    auto fiber_sender = scheduler.spawn([fiber_channel] {
        (void)fiber_channel->Send(55);
    });
    GO2CPP_REQUIRE(fiber_waiter->wait_for(2s));
    GO2CPP_REQUIRE(fiber_sender->wait_for(2s));
    GO2CPP_CHECK(fiber_selected.load(std::memory_order_acquire));

    SelectLoop throwing_handler;
    throwing_handler.bind(DefaultCase(), [](const SelectResult&) {
        throw std::runtime_error("handler failure");
    });
    const auto handler_result = throwing_handler.work();
    GO2CPP_CHECK(handler_result.status == ChannelStatus::kInvalid);
    GO2CPP_CHECK(handler_result.error != nullptr);

    // 同一个事件批次允许 native 线程和 Fiber 依次工作；内部 sync::Mutex
    // 在 Fiber 竞争时会停靠 Fiber，而不是占住唯一的 M。
    auto mixed_channel = MakeChannel<int>(0);
    SelectLoop mixed_events;
    std::atomic<bool> native_selected{false};
    std::atomic<bool> mixed_fiber_selected{false};
    mixed_events.bind(RecvCase(mixed_channel));
    std::thread native_worker([&] {
        const auto result = mixed_events.work({}, 2s);
        native_selected.store(result.selected, std::memory_order_release);
    });
    std::this_thread::sleep_for(5ms);
    auto mixed_fiber = go(scheduler, [&] {
        const auto result = mixed_events.work({}, 2s);
        mixed_fiber_selected.store(result.selected, std::memory_order_release);
    });
    auto first_sender = go(scheduler, [mixed_channel] {
        (void)mixed_channel->Send(1);
    });
    auto second_sender = go(scheduler, [mixed_channel] {
        (void)mixed_channel->Send(2);
    });
    GO2CPP_REQUIRE(first_sender->wait_for(2s));
    GO2CPP_REQUIRE(second_sender->wait_for(2s));
    GO2CPP_REQUIRE(mixed_fiber->wait_for(2s));
    GO2CPP_JOIN_WITH_WATCHDOG(native_worker, 2s);
    GO2CPP_CHECK(native_selected.load(std::memory_order_acquire));
    GO2CPP_CHECK(mixed_fiber_selected.load(std::memory_order_acquire));

    SelectLoop default_event;
    default_event.bind(DefaultCase());
    const auto default_result = default_event.work();
    GO2CPP_CHECK(default_result.selected);
    GO2CPP_CHECK(default_event.runnable());
    default_event.stop();
    GO2CPP_CHECK(!default_event.runable());

    scheduler.shutdown();

    std::atomic<bool> default_done{false};
    auto default_task = go([&] { default_done.store(true, std::memory_order_release); });
    GO2CPP_REQUIRE(default_task->wait_for(2s));
    GO2CPP_CHECK(default_done.load(std::memory_order_acquire));
    shutdown_default_scheduler();

    // 关闭后再次调用 go() 应创建新的 Scheduler，而不是把任务遗留在终态实例中。
    std::atomic<bool> restarted_done{false};
    auto restarted_task = go([&] {
        restarted_done.store(true, std::memory_order_release);
    });
    GO2CPP_REQUIRE(restarted_task->wait_for(2s));
    GO2CPP_CHECK(restarted_done.load(std::memory_order_acquire));
    shutdown_default_scheduler();
}
