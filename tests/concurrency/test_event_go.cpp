#include "go2cpp/event.hpp"

#include "go2cpp/fiber.hpp"
#include "go2cpp/go.hpp"

#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;

/** @brief bind/add_event/size/runnable/clear 的基础簿记语义。 */
void check_batch_bookkeeping() {
    auto channel = go2cpp::MakeChannel<int>(0);
    go2cpp::EventBatch batch;
    GO2CPP_CHECK(batch.size() == 0);
    GO2CPP_CHECK(!batch.runnable());

    batch.bind(go2cpp::RecvCase(channel));
    GO2CPP_CHECK(batch.size() == 1);
    GO2CPP_CHECK(batch.runnable());

    batch.add_event(go2cpp::RecvCase(channel));
    GO2CPP_CHECK(batch.size() == 2);

    batch.clear();
    GO2CPP_CHECK(batch.size() == 0);
    GO2CPP_CHECK(!batch.runnable());
}

/** @brief 事件到达时 work() 选中 case 并调用绑定到该 case 的 handler。 */
void check_handler_invocation() {
    auto channel = go2cpp::MakeChannel<int>(1);
    GO2CPP_REQUIRE(channel->Send(42).Ok());

    go2cpp::EventBatch batch;
    std::atomic<int> handled{-1};
    batch.bind(go2cpp::RecvCase(channel),
               [&handled](const go2cpp::SelectResult& result) {
                   if (const auto value = result.Value<int>()) {
                       handled.store(*value, std::memory_order_release);
                   }
               });

    const auto result = batch.work({}, 2s);
    GO2CPP_CHECK(result.selected);
    GO2CPP_CHECK(batch.happened());
    GO2CPP_REQUIRE(handled.load(std::memory_order_acquire) == 42);
}

/** @brief 无发送者时按 loop timeout 返回，并默认停止整个批次。 */
void check_timeout_stops_batch() {
    auto channel = go2cpp::MakeChannel<int>(0);
    go2cpp::EventBatch batch;
    batch.bind(go2cpp::RecvCase(channel));
    batch.make_loop_time(20ms, true);

    const auto result = batch.work();
    GO2CPP_CHECK(result.status == go2cpp::ChannelStatus::kTimedOut);
    GO2CPP_CHECK(!batch.runnable());

    const auto after_stop = batch.work();
    GO2CPP_CHECK(after_stop.status == go2cpp::ChannelStatus::kCancelled);
}

/** @brief make_stop_config 的轮数上限生效，超限后转为停止结果。 */
void check_max_rounds() {
    auto channel = go2cpp::MakeChannel<int>(0);
    go2cpp::EventBatch batch;
    batch.bind(go2cpp::RecvCase(channel));
    batch.make_loop_time(10ms, false);
    batch.make_stop_config(2, 0);

    (void)batch.work();
    (void)batch.work();
    const auto third = batch.work();
    GO2CPP_CHECK(third.status == go2cpp::ChannelStatus::kCancelled);
    GO2CPP_CHECK(!batch.runnable());
}

/** @brief handler 抛异常时被隔离成结果错误，不穿透到调用者。 */
void check_handler_exception_isolation() {
    auto channel = go2cpp::MakeChannel<int>(1);
    GO2CPP_REQUIRE(channel->Send(1).Ok());

    go2cpp::EventBatch batch;
    batch.bind(go2cpp::RecvCase(channel),
               [](const go2cpp::SelectResult&) {
                   throw std::runtime_error("handler boom");
               });

    const auto result = batch.work({}, 2s);
    GO2CPP_CHECK(result.status == go2cpp::ChannelStatus::kInvalid);
    GO2CPP_CHECK(result.error != nullptr);
}

/** @brief stop() 让后续 work() 走协作式取消路径，reset() 后可重新启用。 */
void check_stop_and_reset() {
    auto channel = go2cpp::MakeChannel<int>(0);
    go2cpp::EventBatch batch;
    batch.bind(go2cpp::RecvCase(channel));

    batch.stop();
    GO2CPP_CHECK(!batch.runnable());
    const auto stopped = batch.work();
    GO2CPP_CHECK(stopped.status == go2cpp::ChannelStatus::kCancelled);

    batch.reset();
    GO2CPP_CHECK(batch.runnable());
}

/**
 * @brief 手动 Fiber（无 Scheduler 归属）中调用 Select 必须立即失败而非阻塞。
 * @details 源码 `src/concurrency/channel.cpp:189-193` 在进入等待**之前**检查
 *          `ParkingCondition::FiberWaitUnsupported()`，是 Fiber 且无 Scheduler 时
 *          返回 `kInvalid`。若该检查缺失或缺位，Select 会退化到 native 等待并
 *          整条占住 carrier OS 线程——这是最危险的回归之一。
 * @note 与 `ParkingCondition` 自身在同类情形返回 `kCancelled` 不同：
 *       Select 在更外层提前失败，返回的是 `kInvalid`。
 */
void check_manual_fiber_select_rejected() {
    auto channel = go2cpp::MakeChannel<int>(1);
    go2cpp::ChannelStatus observed = go2cpp::ChannelStatus::kReady;
    bool ran = false;
    go2cpp::Fiber fiber([&] {
        ran = true;
        const auto result = go2cpp::Select({go2cpp::RecvCase(channel)}, {}, 50ms);
        observed = result.status;
    });
    GO2CPP_REQUIRE(fiber.resume());
    GO2CPP_REQUIRE(ran);
    GO2CPP_CHECK(observed == go2cpp::ChannelStatus::kInvalid);
}

/** @brief go(scheduler, fn) 与 go(fn) 两个入口都能提交并等待任务。 */
void check_go_entry_points() {
    go2cpp::Scheduler scheduler(2);
    scheduler.start();

    std::atomic<int> explicit_runs{0};
    auto explicit_task = go2cpp::go(scheduler, [&explicit_runs] {
        explicit_runs.fetch_add(1, std::memory_order_release);
    });
    GO2CPP_REQUIRE(explicit_task != nullptr);
    GO2CPP_REQUIRE(explicit_task->wait_for(5s));
    GO2CPP_CHECK(explicit_runs.load(std::memory_order_acquire) == 1);

    std::atomic<int> default_runs{0};
    auto default_task = go2cpp::go([&default_runs] {
        default_runs.fetch_add(1, std::memory_order_release);
    });
    GO2CPP_REQUIRE(default_task != nullptr);
    GO2CPP_REQUIRE(default_task->wait_for(5s));
    GO2CPP_CHECK(default_runs.load(std::memory_order_acquire) == 1);

    scheduler.shutdown();
    go2cpp::shutdown_default_scheduler();
}

/** @brief 小写 fiber 描述器先构造、再由 bind() 提交到指定调度器。 */
void check_fiber_descriptor() {
    go2cpp::Scheduler scheduler(2);
    scheduler.start();

    std::atomic<int> runs{0};
    go2cpp::fiber descriptor([&runs] {
        runs.fetch_add(1, std::memory_order_release);
    });
    GO2CPP_CHECK(!descriptor.started());

    auto bound = descriptor.bind(scheduler);
    GO2CPP_REQUIRE(bound != nullptr);
    GO2CPP_REQUIRE(descriptor.wait_for(5s));
    GO2CPP_CHECK(runs.load(std::memory_order_acquire) == 1);
    GO2CPP_CHECK(descriptor.started());

    scheduler.shutdown();
}

}  // namespace

/**
 * @brief 验证 EventBatch/SelectLoop 与 go()/fiber 新手入口。
 * @details 这两块此前没有独立的 run_xxx_tests() 入口（台账 T-06）：
 *          SelectLoop 仅被 test_beginner_api 顺带覆盖，go2cpp::go 在 tests 中无命中。
 * @return 无；失败项由测试支持代码累计。
 */
void run_event_go_tests() {
    go2cpp_tests::announce("event batch and go entry points");
    check_batch_bookkeeping();
    check_handler_invocation();
    check_timeout_stops_batch();
    check_max_rounds();
    check_handler_exception_isolation();
    check_stop_and_reset();
    check_manual_fiber_select_rejected();
    check_go_entry_points();
    check_fiber_descriptor();
}