#include "go2cpp/channel.hpp"
#include "go2cpp/core/timer.hpp"
#include "go2cpp/log.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

/**
 * @brief 验证定时器唤醒 Fiber、版本变化等待和取消。
 * @return 无；测试失败由统一断言统计。
 */
void run_timer_tests() {
    go2cpp_tests::announce("定时器与 Fiber 等待");
    using namespace go2cpp;
    using namespace std::chrono_literals;

    // 定时器回调运行在独立服务线程中，只通过 Channel 唤醒 Fiber；回调
    // 不拥有 Fiber 栈，也不会把定时器线程伪装成 GMP 模型中的 M。
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

    // WaitForChange 必须使用停车条件。原生条件变量会占住唯一的 M，
    // 使下面的发送任务无法获得执行机会。
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

    // 独立原生定时服务：一次性、重排、周期、动态决策和忙等待。
    std::atomic<int> exception_count{0};
    core::TimerService service([&](std::exception_ptr error) {
        if (error) {
            exception_count.fetch_add(1, std::memory_order_release);
        }
    });

    // 长期任务只提升日志级别，不因达到月份/年份阈值而拒绝；超过
    // steady_clock 可表示范围时，deadline 饱和到最大值而不是整数溢出。
    core::TimerDurationLimits duration_limits;
    duration_limits.warn_after = 1h;
    duration_limits.error_after = 2h;
    GO2CPP_CHECK(service.SetDurationLimits(duration_limits));
    {
        auto timer_logger = log::LoggerManager::Instance().Get("Timer");
        const auto old_timer_log_level = timer_logger->level();
        auto timer_log_sink = std::make_shared<log::MemorySink>();
        auto timer_log_worker = std::make_shared<log::LogItemWorker>(
            std::make_shared<log::MinimumLevelFilter>(log::Level::Trace),
            std::make_shared<log::PatternFormatter>("{level}:{message}"),
            timer_log_sink);
        struct TimerLoggerRestore final {
            log::Logger::ptr logger;
            log::LogItemWorker::ptr worker;
            log::Level old_level;
            ~TimerLoggerRestore() {
                logger->RemoveWorker(worker);
                logger->SetLevel(old_level);
            }
        } timer_logger_restore{timer_logger, timer_log_worker,
                               old_timer_log_level};
        timer_logger->SetLevel(log::Level::Trace);
        timer_logger->AddWorker(timer_log_worker);
        auto warn_timer = service.ScheduleAfter(90min, [] {});
        auto error_timer = service.ScheduleAfter(24h * 365, [] {});
        const auto duration_log_lines = timer_log_sink->Snapshot();
        bool saw_warn = false;
        bool saw_error = false;
        for (const auto& line : duration_log_lines) {
            saw_warn = saw_warn || line.find("WARN:") != std::string::npos;
            saw_error = saw_error || line.find("ERROR:") != std::string::npos;
        }
        GO2CPP_CHECK(saw_warn);
        GO2CPP_CHECK(saw_error);
        warn_timer.Cancel();
        error_timer.Cancel();
    }
    auto one_year = service.ScheduleAfter(24h * 365, [] {});
    GO2CPP_CHECK(one_year.Active());
    auto saturated = service.ScheduleAfter(
        std::chrono::steady_clock::duration::max(), [] {});
    GO2CPP_CHECK(saturated.Active());
    GO2CPP_CHECK(service.NextDeadline().has_value());
    saturated.Cancel();
    one_year.Cancel();

    // 参数边界：零周期不注册任务，返回无效句柄而不是启动忙循环。
    auto invalid_period = service.ScheduleEvery(
        0ms, [] {});
    GO2CPP_CHECK(!invalid_period.Active());
    GO2CPP_CHECK(invalid_period.StateValue() == core::TimerState::kInvalid);

    // 同一绝对截止时间的任务按入队顺序执行，验证队列的先进先出约束。
    const auto fifo_deadline = core::TimerService::Clock::now() + 15ms;
    std::mutex fifo_mutex;
    std::atomic<int> fifo_count{0};
    std::vector<int> fifo_order;
    std::vector<core::Timer> fifo_handles;
    for (int index = 0; index < 8; ++index) {
        fifo_handles.emplace_back(service.Schedule(
            fifo_deadline, [&, index] {
                std::lock_guard<std::mutex> lock(fifo_mutex);
                fifo_order.push_back(index);
                fifo_count.fetch_add(1, std::memory_order_release);
            }));
    }
    GO2CPP_REQUIRE_EVENTUALLY(
        fifo_count.load(std::memory_order_acquire) == 8, 1s);
    {
        std::lock_guard<std::mutex> lock(fifo_mutex);
        for (int index = 0; index < 8; ++index) {
            GO2CPP_CHECK(fifo_order[static_cast<std::size_t>(index)] == index);
        }
    }
    std::atomic<int> one_shot_count{0};
    auto one_shot = service.ScheduleAfter(5ms, [&] {
        one_shot_count.fetch_add(1, std::memory_order_release);
    });
    GO2CPP_REQUIRE_EVENTUALLY(
        one_shot_count.load(std::memory_order_acquire) == 1, 1s);
    GO2CPP_CHECK(one_shot.StateValue() == core::TimerState::kCompleted);
    GO2CPP_CHECK(one_shot.RunCount() == 1);

    std::atomic<int> reset_count{0};
    auto reset_timer = service.ScheduleAfter(500ms, [&] {
        reset_count.fetch_add(1, std::memory_order_release);
    });
    GO2CPP_CHECK(reset_timer.Reset(core::TimerService::Clock::now() + 5ms));
    GO2CPP_REQUIRE_EVENTUALLY(
        reset_count.load(std::memory_order_acquire) == 1, 1s);
    GO2CPP_CHECK(reset_timer.StateValue() == core::TimerState::kCompleted);

    core::TimerOptions repeat_options;
    repeat_options.repeat = core::TimerRepeatMode::kFixedDelay;
    repeat_options.max_runs = 3;
    std::atomic<int> repeat_count{0};
    auto repeat_timer = service.ScheduleEvery(
        2ms,
        [&] { repeat_count.fetch_add(1, std::memory_order_release); },
        repeat_options);
    GO2CPP_REQUIRE_EVENTUALLY(
        repeat_count.load(std::memory_order_acquire) == 3, 2s);
    GO2CPP_CHECK(repeat_timer.RunCount() == 3);
    GO2CPP_CHECK(repeat_timer.StateValue() == core::TimerState::kCompleted);

    std::atomic<int> dynamic_count{0};
    core::TimerOptions dynamic_options;
    dynamic_options.repeat = core::TimerRepeatMode::kFixedDelay;
    auto dynamic_timer = service.ScheduleRepeat(
        1ms,
        [&](const core::TimerContext& context) {
            const int count = dynamic_count.fetch_add(
                                  1, std::memory_order_acq_rel) +
                              1;
            if (count == 1) {
                return core::TimerDecision::ContinueAfter(4ms);
            }
            if (count == 2) {
                return core::TimerDecision::ContinueAt(
                    context.started_at + 1ms);
            }
            return core::TimerDecision::Stop();
        },
        dynamic_options);
    GO2CPP_REQUIRE_EVENTUALLY(!dynamic_timer.Active(), 2s);
    GO2CPP_CHECK(dynamic_count.load(std::memory_order_acquire) == 3);
    GO2CPP_CHECK(dynamic_timer.StateValue() == core::TimerState::kCompleted);

    core::TimerOptions busy_options;
    busy_options.repeat = core::TimerRepeatMode::kFixedDelay;
    busy_options.wait_mode = core::TimerWaitMode::kBusyWait;
    busy_options.spin_window = 100us;
    busy_options.max_runs = 1;
    std::atomic<bool> busy_fired{false};
    auto busy_timer = service.ScheduleEvery(
        2ms, [&] { busy_fired.store(true, std::memory_order_release); },
        busy_options);
    GO2CPP_REQUIRE_EVENTUALLY(
        busy_fired.load(std::memory_order_acquire), 1s);
    GO2CPP_CHECK(busy_timer.StateValue() == core::TimerState::kCompleted);

    // 弱引用资源释放后，周期任务在下一次领取时停止；回调不会延长资源寿命。
    auto owner = std::make_shared<int>(7);
    std::atomic<int> weak_count{0};
    core::TimerOptions weak_options;
    weak_options.repeat = core::TimerRepeatMode::kFixedDelay;
    auto weak_timer = service.ScheduleWeakEvery(
        1ms, std::weak_ptr<void>(owner),
        [&](std::shared_ptr<void> value) {
            GO2CPP_CHECK(static_cast<bool>(value));
            weak_count.fetch_add(1, std::memory_order_release);
        },
        weak_options);
    GO2CPP_REQUIRE_EVENTUALLY(
        weak_count.load(std::memory_order_acquire) > 0, 1s);
    owner.reset();
    GO2CPP_REQUIRE_EVENTUALLY(!weak_timer.Active(), 1s);

    // 可插拔执行器拒绝任务时，定时器失败并通知异常观察器。
    struct RejectingExecutor final : core::TimerExecutor {
        bool Submit(std::function<void()>) override { return false; }
    };
    auto rejected = service.ScheduleOn(
        core::TimerService::Clock::now() + 1ms, [] {},
        std::make_shared<RejectingExecutor>());
    GO2CPP_REQUIRE_EVENTUALLY(
        rejected.StateValue() == core::TimerState::kFailed, 1s);

    auto throwing = service.ScheduleAfter(1ms, [] {
        throw std::runtime_error("timer test exception");
    });
    std::atomic<bool> after_exception{false};
    auto survivor = service.ScheduleAfter(8ms, [&] {
        after_exception.store(true, std::memory_order_release);
    });
    GO2CPP_REQUIRE_EVENTUALLY(
        exception_count.load(std::memory_order_acquire) == 2, 1s);
    GO2CPP_REQUIRE_EVENTUALLY(
        after_exception.load(std::memory_order_acquire), 1s);
    GO2CPP_CHECK(throwing.StateValue() == core::TimerState::kFailed);
    GO2CPP_CHECK(survivor.StateValue() == core::TimerState::kCompleted);

    // 多线程并发注册，确认每个到期任务只执行一次且服务锁不泄漏。
    constexpr int kProducerCount = 4;
    constexpr int kTasksPerProducer = 32;
    constexpr int kExpected = kProducerCount * kTasksPerProducer;
    std::atomic<int> concurrent_count{0};
    std::mutex timer_vector_mutex;
    std::vector<core::Timer> timer_handles;
    timer_handles.reserve(kExpected);
    std::vector<std::thread> timer_producers;
    for (int producer = 0; producer < kProducerCount; ++producer) {
        timer_producers.emplace_back([&, producer] {
            for (int index = 0; index < kTasksPerProducer; ++index) {
                auto handle = service.ScheduleAfter(
                    std::chrono::milliseconds(2 + ((producer + index) % 4)),
                    [&] {
                        concurrent_count.fetch_add(1,
                                                   std::memory_order_release);
                    });
                std::lock_guard<std::mutex> lock(timer_vector_mutex);
                timer_handles.emplace_back(std::move(handle));
            }
        });
    }
    for (auto& producer : timer_producers) {
        GO2CPP_JOIN_WITH_WATCHDOG(producer, 3s);
    }
    GO2CPP_REQUIRE_EVENTUALLY(
        concurrent_count.load(std::memory_order_acquire) == kExpected, 3s);
    GO2CPP_CHECK(service.PendingCount() == 0);
    GO2CPP_CHECK(!service.NextDeadline().has_value());

    // 调度器接口只把到期函数提交成 Fiber，定时服务线程不恢复 Fiber 栈。
    Scheduler timed_scheduler(1);
    timed_scheduler.start();
    std::atomic<int> fiber_timer_count{0};
    auto fiber_timer = timed_scheduler.schedule_after(5ms, [&] {
        fiber_timer_count.fetch_add(1, std::memory_order_release);
    });
    GO2CPP_REQUIRE_EVENTUALLY(
        fiber_timer_count.load(std::memory_order_acquire) == 1, 1s);
    GO2CPP_CHECK(fiber_timer.StateValue() == core::TimerState::kCompleted);

    core::TimerOptions fiber_repeat_options;
    fiber_repeat_options.repeat = core::TimerRepeatMode::kFixedDelay;
    fiber_repeat_options.max_runs = 3;
    std::atomic<int> fiber_repeat_count{0};
    auto fiber_repeat = timed_scheduler.schedule_every(
        2ms,
        [&] { fiber_repeat_count.fetch_add(1, std::memory_order_release); },
        fiber_repeat_options);
    GO2CPP_REQUIRE_EVENTUALLY(
        fiber_repeat_count.load(std::memory_order_acquire) == 3, 2s);
    GO2CPP_CHECK(fiber_repeat.StateValue() == core::TimerState::kCompleted);
    timed_scheduler.shutdown();

    // 服务停止与周期回调并发时，收尾阶段不能把任务重新插回已关闭队列。
    std::atomic<bool> long_callback_started{false};
    core::Timer running_repeat;
    {
        core::TimerService stopping_service;
        core::TimerOptions stopping_options;
        stopping_options.repeat = core::TimerRepeatMode::kFixedDelay;
        running_repeat = stopping_service.ScheduleEvery(
            1ms,
            [&] {
                long_callback_started.store(true, std::memory_order_release);
                std::this_thread::sleep_for(20ms);
            },
            stopping_options);
        GO2CPP_REQUIRE_EVENTUALLY(
            long_callback_started.load(std::memory_order_acquire), 1s);
    }
    GO2CPP_CHECK(!running_repeat.Active());
    GO2CPP_CHECK(running_repeat.StateValue() == core::TimerState::kCancelled);

    // 服务析构会取消未领取任务；回调不应在析构之后突然访问外部对象。
    std::atomic<bool> abandoned_callback{false};
    {
        core::TimerService short_lived_service;
        auto abandoned = short_lived_service.ScheduleAfter(
            1h, [&] { abandoned_callback.store(true, std::memory_order_release); });
        GO2CPP_CHECK(abandoned.Active());
    }
    std::this_thread::sleep_for(20ms);
    GO2CPP_CHECK(!abandoned_callback.load(std::memory_order_acquire));

    scheduler.shutdown();
}
