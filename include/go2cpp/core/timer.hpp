#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace go2cpp::core {

class TimerService;

// 取消会阻止尚未开始的回调被分离。已经由定时器线程分离的回调仍可能
// 执行完；引用短生命周期对象的回调必须使用弱指针或唤醒门。
/**
 * 可取消的单次定时器句柄。
 *
 * 依赖：TimerService 的共享状态；对上层提供 Cancel/Active。句柄析构会
 * 取消尚未分离的回调，但已经由定时器线程取出的回调仍可能执行完成。
 */
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

// 显式命名的默认服务拥有一个进程级定时器线程。嵌入方也可以自行构造
// 生命周期独立的服务实例。
/**
 * 基于 steady_clock 的定时器服务。
 *
 * 依赖：后台定时器线程和 Timer 状态；对上层提供默认进程级服务或独立
 * 生命周期实例，并按绝对截止时间调度回调。服务析构会等待自身线程退出。
 */
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
