#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace go2cpp::core {

class TimerService;

// Cancellation prevents a callback that has not started from being detached.
// A callback already detached by the timer thread may still finish; callbacks
// that reference a shorter-lived owner must use a weak pointer or wake gate.
class Timer final {
public:
    struct State;
    Timer() noexcept;
    ~Timer();
    Timer(Timer&&) noexcept;
    Timer& operator=(Timer&&) noexcept;
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    void Cancel() noexcept;
    bool Active() const noexcept;

private:
    explicit Timer(std::shared_ptr<State> state);
    std::shared_ptr<State> m_state;
    friend class TimerService;
};

// The explicitly named default service owns one process-wide timer thread.
// Embedders can instead construct a service with an independent lifetime.
class TimerService final {
public:
    using Clock = std::chrono::steady_clock;
    class Impl;

    TimerService();
    ~TimerService();
    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    static TimerService& Default();
    Timer Schedule(Clock::time_point deadline, std::function<void()> callback);

private:
    std::shared_ptr<Impl> m_impl;
    friend class Timer;
};

}  // namespace go2cpp::core
