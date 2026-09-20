#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace go2cpp::core {

// Predicate waits keep the external lock contract of condition_variable,
// but a managed G parks its Fiber instead of blocking its machine thread.
class ParkingCondition final {
public:
    using Clock = std::chrono::steady_clock;

    ParkingCondition();
    ~ParkingCondition();
    ParkingCondition(const ParkingCondition&) = delete;
    ParkingCondition& operator=(const ParkingCondition&) = delete;

    void notify_one() noexcept;
    void notify_all() noexcept;

    template <typename Predicate>
    bool wait(std::unique_lock<std::mutex>& lock, Predicate predicate) {
        while (!predicate()) {
            if (WaitOnce(lock, std::nullopt, predicate) ==
                WaitStatus::kCancelled) {
                return predicate();
            }
        }
        return true;
    }

    template <typename Rep, typename Period, typename Predicate>
    bool wait_for(std::unique_lock<std::mutex>& lock,
                  std::chrono::duration<Rep, Period> timeout,
                  Predicate predicate) {
        const auto duration =
            std::chrono::duration_cast<Clock::duration>(timeout);
        const auto now = Clock::now();
        const auto deadline = duration <= Clock::duration::zero()
                                  ? now
                                  : (duration >= Clock::time_point::max() - now
                                         ? Clock::time_point::max()
                                         : now + duration);
        return wait_until(lock, deadline, std::move(predicate));
    }

    template <typename Predicate>
    bool wait_until(std::unique_lock<std::mutex>& lock,
                    Clock::time_point deadline, Predicate predicate) {
        while (!predicate()) {
            const auto result = WaitOnce(lock, deadline, predicate);
            if (result != WaitStatus::kNotified) {
                return predicate();
            }
        }
        return true;
    }

    static bool CancellationRequested() noexcept;

private:
    enum class WaitStatus { kNotified, kTimedOut, kCancelled };
    class WaitNode;

    WaitStatus WaitOnce(std::unique_lock<std::mutex>& lock,
                        std::optional<Clock::time_point> deadline,
                        const std::function<bool()>& predicate);

    std::mutex m_mutex;
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

}  // namespace go2cpp::core
