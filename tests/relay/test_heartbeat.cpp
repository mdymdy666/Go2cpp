/**
 * @file test_heartbeat.cpp
 * @brief Heartbeat 的周期发送、死亡判定、Context 取消与停止语义（REQ-005）。
 *
 * 覆盖：
 * - 首次发送在 interval 之后（ticker 语义），且发送间隔不出现突发；
 * - send 返回 false → on_dead 恰好一次，之后不再发送；
 * - send 抛异常按死亡处理（不能让后台线程把进程带走）；
 * - Stop 之后不再发送；Stop 在首次发送之前也生效；
 * - Context 取消 → 静默停止，不触发 on_dead；启动前已取消 → 不启动；
 * - on_dead 里调用 Stop()（自停止）不死锁；多线程并发 Stop 幂等；
 * - 停止后可重新 Start。
 */
#include "go2cpp/relay/lease.hpp"

#include "go2cpp/concurrency/context.hpp"

#include "relay_test_util.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::relay::Heartbeat;
using go2cpp::relay::test::SleepMs;
using go2cpp::relay::test::SteadyNowMs;

void test_interval_and_stop() {
    std::atomic<int> sends{0};
    std::atomic<int> deaths{0};
    Heartbeat heartbeat(20ms);
    const std::int64_t start = SteadyNowMs();
    heartbeat.Start(
        nullptr, [&sends] { return sends.fetch_add(1) + 1 > 0; },
        [&deaths] { deaths.fetch_add(1); });

    GO2CPP_REQUIRE_EVENTUALLY(sends.load() >= 5, 5s);
    heartbeat.Stop();
    const int sends_at_stop = sends.load();
    const std::int64_t elapsed = SteadyNowMs() - start;
    GO2CPP_CHECK(elapsed >= 80); // 5 个周期至少需要约 100ms，留抖动余量
    GO2CPP_CHECK(deaths.load() == 0);

    // Stop 之后不能再发；计数保持不变。
    SleepMs(150);
    GO2CPP_CHECK(sends.load() == sends_at_stop);
    GO2CPP_CHECK(deaths.load() == 0);
}

void test_first_send_is_after_interval() {
    std::atomic<int> sends{0};
    Heartbeat heartbeat(200ms);
    heartbeat.Start(nullptr, [&sends] { sends.fetch_add(1); return true; }, nullptr);

    SleepMs(50);
    GO2CPP_CHECK(sends.load() == 0); // ticker 语义：先等一个 interval
    GO2CPP_REQUIRE_EVENTUALLY(sends.load() >= 1, 5s);
    heartbeat.Stop();
}

void test_dead_on_send_failure() {
    std::atomic<int> sends{0};
    std::atomic<int> deaths{0};
    Heartbeat heartbeat(10ms);
    heartbeat.Start(
        nullptr,
        [&sends] {
            const int previous = sends.fetch_add(1);
            return previous + 1 < 3; // 第 3 次返回 false
        },
        [&deaths] { deaths.fetch_add(1); });

    GO2CPP_REQUIRE_EVENTUALLY(deaths.load() == 1, 5s);
    SleepMs(200);
    GO2CPP_CHECK(deaths.load() == 1); // 只触发一次
    GO2CPP_CHECK(sends.load() == 3);  // 死亡后不再发送
    heartbeat.Stop();
}

void test_send_exception_treated_as_dead() {
    std::atomic<int> deaths{0};
    Heartbeat heartbeat(10ms);
    heartbeat.Start(
        nullptr, []() -> bool { throw std::runtime_error("模拟发送失败"); },
        [&deaths] { deaths.fetch_add(1); });

    GO2CPP_REQUIRE_EVENTUALLY(deaths.load() == 1, 5s);
    SleepMs(100);
    GO2CPP_CHECK(deaths.load() == 1);
    heartbeat.Stop();
}

void test_stop_before_first_send() {
    std::atomic<int> sends{0};
    std::atomic<int> deaths{0};
    Heartbeat heartbeat(100ms);
    heartbeat.Start(nullptr, [&sends] { sends.fetch_add(1); return true; },
                    [&deaths] { deaths.fetch_add(1); });
    SleepMs(10);
    heartbeat.Stop();
    SleepMs(300);
    GO2CPP_CHECK(sends.load() == 0);
    GO2CPP_CHECK(deaths.load() == 0);
}

void test_context_cancel_stops_without_dead() {
    auto context_and_cancel =
        go2cpp::Context::WithCancel(go2cpp::Context::Background());
    auto &context = context_and_cancel.first;
    const auto &cancel = context_and_cancel.second;

    std::atomic<int> sends{0};
    std::atomic<int> deaths{0};
    Heartbeat heartbeat(10ms);
    heartbeat.Start(
        context.get(), [&sends] { sends.fetch_add(1); return true; },
        [&deaths] { deaths.fetch_add(1); });

    GO2CPP_REQUIRE_EVENTUALLY(sends.load() >= 3, 5s);
    cancel();
    // Context 取消是主动关停：静默停止，不算对端死亡。
    SleepMs(200);
    const int sends_after_cancel = sends.load();
    SleepMs(150);
    GO2CPP_CHECK(sends.load() == sends_after_cancel);
    GO2CPP_CHECK(deaths.load() == 0);
    heartbeat.Stop();
}

void test_context_already_cancelled() {
    auto context_and_cancel =
        go2cpp::Context::WithCancel(go2cpp::Context::Background());
    auto &context = context_and_cancel.first;
    const auto &cancel = context_and_cancel.second;
    cancel();

    std::atomic<int> sends{0};
    std::atomic<int> deaths{0};
    Heartbeat heartbeat(10ms);
    heartbeat.Start(
        context.get(), [&sends] { sends.fetch_add(1); return true; },
        [&deaths] { deaths.fetch_add(1); });

    SleepMs(100);
    GO2CPP_CHECK(sends.load() == 0);
    GO2CPP_CHECK(deaths.load() == 0);
    heartbeat.Stop();
}

void test_on_dead_can_stop_self() {
    std::atomic<int> deaths{0};
    Heartbeat heartbeat(10ms);
    heartbeat.Start(
        nullptr, [] { return false; }, [&deaths, &heartbeat] {
            deaths.fetch_add(1);
            // 在心跳线程的 on_dead 里调用 Stop()：实现必须 detach 而不是自 join。
            heartbeat.Stop();
        });

    GO2CPP_REQUIRE_EVENTUALLY(deaths.load() == 1, 5s);
    SleepMs(150);
    GO2CPP_CHECK(deaths.load() == 1);
}

void test_restart_after_stop() {
    std::atomic<int> first_sends{0};
    std::atomic<int> second_sends{0};
    Heartbeat heartbeat(10ms);

    heartbeat.Start(nullptr, [&first_sends] { first_sends.fetch_add(1); return true; },
                    nullptr);
    GO2CPP_REQUIRE_EVENTUALLY(first_sends.load() >= 2, 5s);
    heartbeat.Stop();
    const int first_after_stop = first_sends.load();
    SleepMs(100);
    GO2CPP_CHECK(first_sends.load() == first_after_stop);

    heartbeat.Start(nullptr,
                    [&second_sends] { second_sends.fetch_add(1); return true; }, nullptr);
    GO2CPP_REQUIRE_EVENTUALLY(second_sends.load() >= 2, 5s);
    heartbeat.Stop();
    // 重新 Start 不影响旧计数。
    GO2CPP_CHECK(first_sends.load() == first_after_stop);
}

void test_concurrent_stop_is_idempotent() {
    std::atomic<int> sends{0};
    Heartbeat heartbeat(5ms);
    heartbeat.Start(nullptr, [&sends] { sends.fetch_add(1); return true; }, nullptr);
    GO2CPP_REQUIRE_EVENTUALLY(sends.load() >= 2, 5s);

    std::vector<std::thread> stoppers;
    for (int index = 0; index < 4; ++index) {
        stoppers.emplace_back([&heartbeat] { heartbeat.Stop(); });
    }
    for (auto &thread : stoppers) {
        thread.join();
    }
    const int sends_after_stop = sends.load();
    SleepMs(150);
    GO2CPP_CHECK(sends.load() == sends_after_stop);
}

/** @brief 发送间隔不得突发：相邻两次发送的单调时间差都应 ≥ interval 的量级。 */
void test_interval_has_no_burst() {
    std::mutex mutex;
    std::vector<std::int64_t> send_times;
    Heartbeat heartbeat(25ms);
    heartbeat.Start(
        nullptr,
        [&mutex, &send_times] {
            const std::lock_guard<std::mutex> lock(mutex);
            send_times.push_back(SteadyNowMs());
            return true;
        },
        nullptr);

    const auto enough_ticks = [&mutex, &send_times] {
        const std::lock_guard<std::mutex> lock(mutex);
        return send_times.size() >= 6;
    };
    GO2CPP_REQUIRE_EVENTUALLY(enough_ticks(), 5s);
    heartbeat.Stop();

    const std::lock_guard<std::mutex> lock(mutex);
    GO2CPP_REQUIRE(send_times.size() >= 6);
    for (std::size_t index = 1; index < send_times.size(); ++index) {
        GO2CPP_CHECK(send_times[index] - send_times[index - 1] >= 15);
    }
    GO2CPP_CHECK(send_times.back() - send_times.front() >= 100);
}

} // namespace

int main() {
    go2cpp_tests::announce("relay.heartbeat");

    test_interval_and_stop();
    test_first_send_is_after_interval();
    test_dead_on_send_failure();
    test_send_exception_treated_as_dead();
    test_stop_before_first_send();
    test_context_cancel_stops_without_dead();
    test_context_already_cancelled();
    test_on_dead_can_stop_self();
    test_restart_after_stop();
    test_concurrent_stop_is_idempotent();
    test_interval_has_no_burst();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "relay_heartbeat_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("relay_heartbeat_test: all checks passed\n");
    return 0;
}
