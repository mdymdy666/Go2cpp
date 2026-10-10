#include "go2cpp/core/parking_condition.hpp"

#include "go2cpp/fiber.hpp"

#include "test_support.hpp"

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/** @brief notify_one 唤醒一个持锁等待者。 */
void check_notify_one() {
    go2cpp::core::ParkingCondition condition;
    std::mutex mutex;
    bool ready = false;
    std::thread producer([&] {
        std::this_thread::sleep_for(10ms);
        {
            std::lock_guard<std::mutex> lock(mutex);
            ready = true;
        }
        condition.notify_one();
    });
    std::unique_lock<std::mutex> lock(mutex);
    const bool notified =
        condition.wait_for(lock, 5s, [&ready] { return ready; });
    lock.unlock();
    GO2CPP_JOIN_WITH_WATCHDOG(producer, 6s);
    GO2CPP_REQUIRE(notified);
}

/** @brief 谓词始终为假时按超时返回，不会永久阻塞普通线程。 */
void check_timeout() {
    go2cpp::core::ParkingCondition condition;
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    const auto started = std::chrono::steady_clock::now();
    const bool notified = condition.wait_for(lock, 20ms, [] { return false; });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    lock.unlock();
    GO2CPP_CHECK(!notified);
    GO2CPP_CHECK(elapsed >= 15ms);
}

/** @brief notify_all 唤醒全部等待线程。 */
void check_notify_all() {
    go2cpp::core::ParkingCondition condition;
    std::mutex mutex;
    int ready = 0;
    constexpr int kWaiters = 4;
    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);
    for (int index = 0; index < kWaiters; ++index) {
        waiters.emplace_back([&] {
            std::unique_lock<std::mutex> lock(mutex);
            (void)condition.wait(lock, [&ready] { return ready > 0; });
        });
    }
    std::this_thread::sleep_for(20ms);
    {
        std::lock_guard<std::mutex> lock(mutex);
        ready = 1;
    }
    condition.notify_all();
    for (auto& waiter : waiters) {
        GO2CPP_JOIN_WITH_WATCHDOG(waiter, 6s);
    }
    GO2CPP_REQUIRE(true);
}

/** @brief 谓词在进入等待前已成立时必须立即返回，不再等待。 */
void check_immediate_predicate() {
    go2cpp::core::ParkingCondition condition;
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    const auto started = std::chrono::steady_clock::now();
    const bool satisfied = condition.wait_for(lock, 5s, [] { return true; });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    lock.unlock();
    GO2CPP_CHECK(satisfied);
    GO2CPP_CHECK(elapsed < 1s);
}

/**
 * @brief 手动 Fiber（无 Scheduler 归属）中的等待必须立即取消而不是阻塞 carrier。
 * @details 源码 `src/concurrency/parking_condition.cpp:260-264` 对
 *          「是 Fiber 但没有 Scheduler/Task」的情况直接返回 kCancelled，
 *          并保持调用方的外部 mutex 锁定。若该分支退化为 native 阻塞，
 *          carrier OS 线程会被整条占住，这是最危险的回归之一。
 */
void check_manual_fiber_wait_is_cancelled() {
    go2cpp::core::ParkingCondition condition;
    std::mutex mutex;
    bool ready = false;
    bool wait_returned_true = true;
    bool lock_still_held = false;
    bool ran = false;

    go2cpp::Fiber fiber([&] {
        ran = true;
        std::unique_lock<std::mutex> lock(mutex);
        wait_returned_true =
            condition.wait_for(lock, 50ms, [&ready] { return ready; });
        lock_still_held = lock.owns_lock();
    });
    GO2CPP_REQUIRE(fiber.resume());
    GO2CPP_REQUIRE(ran);
    GO2CPP_CHECK(!wait_returned_true);
    GO2CPP_CHECK(lock_still_held);
}

/** @brief 普通线程上的静态查询入口不抛异常且语义稳定。 */
void check_static_queries() {
    GO2CPP_CHECK(!go2cpp::core::ParkingCondition::CancellationRequested());
    GO2CPP_CHECK(!go2cpp::core::ParkingCondition::FiberWaitUnsupported());
}

}  // namespace

/**
 * @brief 验证 core::ParkingCondition 的等待、超时与通知语义。
 * @details 该设施是 managed G 让出 M 的关键路径，此前仅被间接覆盖（台账 T-07）。
 * @return 无；失败项由测试支持代码累计。
 */
void run_parking_condition_tests() {
    go2cpp_tests::announce("parking condition wait and notify");
    check_notify_one();
    check_timeout();
    check_notify_all();
    check_immediate_predicate();
    check_manual_fiber_wait_is_cancelled();
    check_static_queries();
}