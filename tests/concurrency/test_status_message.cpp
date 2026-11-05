/**
 * @file test_status_message.cpp
 * @brief `WaitStatus` 细化与各 `StatusMessage` 诊断接口的回归测试。
 *
 * 覆盖三类契约：
 * 1. 每个状态枚举的每个值都能拿到「非空、非枚举名、非 unknown」的具体含义；
 * 2. `WaitStatus` 的历史值稳定，新增值只追加在末尾（向后兼容）；
 * 3. `ParkingCondition` 的真实结局：正常通知、超时、过去 deadline、通知
 *    不缓冲、手动 Fiber 未武装时 `wait()` 不原地忙等。
 */
#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/fast_channel.hpp"
#include "go2cpp/concurrency/future.hpp"
#include "go2cpp/fiber.hpp"

#include "test_support.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::core::ParkingCondition;

/**
 * @brief 断言一条 Message 是「具体含义」而不是空串/枚举名。
 * @param label [in] 所属枚举名，仅用于失败时定位。
 * @param message [in] 被检查的静态字符串，可为 nullptr（会被判失败）。
 */
void CheckMessage(const char* label, const char* message) {
    GO2CPP_REQUIRE(message != nullptr);
    GO2CPP_CHECK(message[0] != '\0');
    // 枚举名形如 "kTimedOut"；具体含义是一句中文处置建议，至少 20 字节。
    GO2CPP_CHECK(std::strlen(message) >= 20);
    GO2CPP_CHECK(message[0] != 'k');
    GO2CPP_CHECK(std::strstr(message, "unknown") == nullptr);
    if (message[0] == '\0' || message[0] == 'k') {
        std::cerr << "[message] " << label << " => " << message << '\n';
    }
}

/** @brief 遍历某个枚举的全部取值并检查各自的 Message。 */
template <typename Enum, std::size_t N, typename MessageFn>
void CheckAllMessages(const char* label, const std::array<Enum, N>& values,
                      MessageFn message_of) {
    for (const Enum value : values) {
        CheckMessage(label, message_of(value));
    }
}

/** @brief WaitStatus 的六个值都有语义化说明，且历史值不变。 */
void check_wait_status_messages() {
    using WaitStatus = ParkingCondition::WaitStatus;
    static_assert(static_cast<int>(WaitStatus::kNotified) == 0);
    static_assert(static_cast<int>(WaitStatus::kTimedOut) == 1);
    static_assert(static_cast<int>(WaitStatus::kCancelled) == 2);
    static_assert(static_cast<int>(WaitStatus::kSpurious) == 3);
    static_assert(static_cast<int>(WaitStatus::kNotArmed) == 4);
    static_assert(static_cast<int>(WaitStatus::kInvalid) == 5);
    const std::array<WaitStatus, 6> values{
        WaitStatus::kNotified, WaitStatus::kTimedOut, WaitStatus::kCancelled,
        WaitStatus::kSpurious, WaitStatus::kNotArmed, WaitStatus::kInvalid};
    CheckAllMessages("WaitStatus", values,
                     [](WaitStatus status) {
                         return go2cpp::core::WaitStatusMessage(status);
                     });
}

/** @brief ChannelStatus 十个值都有具体含义。 */
void check_channel_status_messages() {
    using go2cpp::ChannelStatus;
    const std::array<ChannelStatus, 10> values{
        ChannelStatus::kReady,       ChannelStatus::kWouldBlock,
        ChannelStatus::kClosed,      ChannelStatus::kAlreadyClosed,
        ChannelStatus::kCancelled,   ChannelStatus::kDeadlineExceeded,
        ChannelStatus::kTimedOut,    ChannelStatus::kNil,
        ChannelStatus::kInvalid,     ChannelStatus::kNoScheduler};
    CheckAllMessages("ChannelStatus", values,
                     [](ChannelStatus status) {
                         return go2cpp::ChannelStatusMessage(status);
                     });
    // Message 是语义，Name 是枚举名：两者对同一个值必须不同。
    for (const ChannelStatus status : values) {
        GO2CPP_CHECK(std::strcmp(go2cpp::ChannelStatusMessage(status),
                                 go2cpp::ChannelStatusName(status)) != 0);
    }
}

/** @brief FutureStatus 七个值都有具体含义。 */
void check_future_status_messages() {
    using go2cpp::FutureStatus;
    const std::array<FutureStatus, 7> values{
        FutureStatus::kReady,   FutureStatus::kError,
        FutureStatus::kException, FutureStatus::kCancelled,
        FutureStatus::kTimedOut, FutureStatus::kDeadlineExceeded,
        FutureStatus::kInvalid};
    CheckAllMessages("FutureStatus", values,
                     [](FutureStatus status) {
                         return go2cpp::FutureStatusMessage(status);
                     });
    for (const FutureStatus status : values) {
        GO2CPP_CHECK(std::strcmp(go2cpp::FutureStatusMessage(status),
                                 go2cpp::FutureStatusName(status)) != 0);
    }
}

/** @brief FastStatus 四个值都有具体含义。 */
void check_fast_status_messages() {
    using go2cpp::FastStatus;
    const std::array<FastStatus, 4> values{
        FastStatus::kOk, FastStatus::kWouldBlock, FastStatus::kClosed,
        FastStatus::kUnsupported};
    CheckAllMessages("FastStatus", values,
                     [](FastStatus status) {
                         return go2cpp::FastStatusMessage(status);
                     });
}

/** @brief ContextRollback::Status 五个值都有具体含义。 */
void check_context_rollback_status_messages() {
    using go2cpp::ContextRollback;
    using Status = ContextRollback::Status;
    const std::array<Status, 5> values{
        Status::kActive, Status::kRollingBack, Status::kCommitted,
        Status::kRolledBack, Status::kFailed};
    CheckAllMessages("ContextRollback::Status", values,
                     [](Status status) {
                         return ContextRollback::StatusMessage(status);
                     });
    for (const Status status : values) {
        GO2CPP_CHECK(std::strcmp(ContextRollback::StatusMessage(status),
                                 ContextRollback::StatusName(status)) != 0);
    }
}

/** @brief 谓词始终为假时按 deadline 超时返回（真实 kTimedOut 路径）。 */
void check_timeout_path() {
    ParkingCondition condition;
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    const auto started = std::chrono::steady_clock::now();
    const bool notified = condition.wait_for(lock, 20ms, [] { return false; });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    lock.unlock();
    GO2CPP_CHECK(!notified);
    GO2CPP_CHECK(elapsed >= 15ms);
}

/** @brief 过去的 deadline 必须立即返回，而不是一直阻塞（kTimedOut 提前返回）。 */
void check_past_deadline_path() {
    ParkingCondition condition;
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    const auto started = std::chrono::steady_clock::now();
    const bool notified = condition.wait_until(
        lock, std::chrono::steady_clock::now() - 1ms, [] { return false; });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    lock.unlock();
    GO2CPP_CHECK(!notified);
    GO2CPP_CHECK(elapsed < 100ms);
}

/** @brief notify_one 唤醒一个持锁等待者（真实 kNotified 路径）。 */
void check_notify_one_path() {
    ParkingCondition condition;
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
    {
        std::unique_lock<std::mutex> lock(mutex);
        GO2CPP_CHECK(condition.wait_for(lock, 5s, [&ready] { return ready; }));
    }
    GO2CPP_JOIN_WITH_WATCHDOG(producer, 6s);
}

/** @brief notify_all 唤醒全部等待者。 */
void check_notify_all_path() {
    ParkingCondition condition;
    std::mutex mutex;
    int ready = 0;
    constexpr int kWaiters = 4;
    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);
    for (int index = 0; index < kWaiters; ++index) {
        waiters.emplace_back([&] {
            std::unique_lock<std::mutex> lock(mutex);
            GO2CPP_CHECK(
                condition.wait_for(lock, 5s, [&ready] { return ready > 0; }));
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

/** @brief 通知不缓冲：没有等待者时 notify 不会让后续 wait 立即成功。 */
void check_notify_is_not_buffered() {
    ParkingCondition condition;
    condition.notify_one();
    condition.notify_all();
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    GO2CPP_CHECK(!condition.wait_for(lock, 10ms, [] { return false; }));
}

/**
 * @brief 手动 Fiber（无 Scheduler 归属）里 `wait()` 必须立即返回而不是忙等。
 * @details 该路径现在由 `WaitStatus::kNotArmed` 表示；若 `wait()` 仍只把
 *          `kCancelled` 当作退出条件，这里会以「每次立即返回 kNotArmed」
 *          形成无限忙等，测试会挂住（外部 20s 超时会失败）。
 */
void check_manual_fiber_wait_returns() {
    ParkingCondition condition;
    std::mutex mutex;
    bool ran = false;
    bool wait_returned_false = false;
    go2cpp::Fiber fiber([&] {
        ran = true;
        std::unique_lock<std::mutex> lock(mutex);
        wait_returned_false = !condition.wait(lock, [] { return false; });
    });
    GO2CPP_REQUIRE(fiber.resume());
    GO2CPP_CHECK(ran);
    GO2CPP_CHECK(wait_returned_false);
}

}  // namespace

/**
 * @brief 运行状态码细化与 StatusMessage 的全部检查。
 * @return 进程退出码：0 表示全部通过，1 表示有失败项。
 */
int main() {
    go2cpp_tests::announce("status message and WaitStatus refinement");
    check_wait_status_messages();
    check_channel_status_messages();
    check_future_status_messages();
    check_fast_status_messages();
    check_context_rollback_status_messages();
    check_timeout_path();
    check_past_deadline_path();
    check_notify_one_path();
    check_notify_all_path();
    check_notify_is_not_buffered();
    check_manual_fiber_wait_returns();
    return go2cpp_tests::g_failures.load(std::memory_order_relaxed) == 0 ? 0
                                                                        : 1;
}
