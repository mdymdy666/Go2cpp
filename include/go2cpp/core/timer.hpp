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
    /** @brief 创建一个无效定时器句柄。 */
    Timer() noexcept;
    /** @brief 取消并释放定时器句柄。 */
    ~Timer();
    /** @brief 转移定时器所有权。 */
    Timer(Timer&&) noexcept;
    /** @brief 转移赋值定时器所有权。 */
    Timer& operator=(Timer&&) noexcept;
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    /** @brief 取消尚未触发的回调。 */
    void Cancel() noexcept;
    /** @brief 返回定时器是否仍处于活动状态。 */
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

    /** @brief 启动定时器服务线程。 */
    TimerService();
    /** @brief 停止服务并等待线程退出。 */
    ~TimerService();
    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    /** @brief 返回进程内默认定时器服务。 */
    static TimerService& Default();
    /**
     * @brief 注册一次性定时回调。
     * @param deadline steady_clock 截止时间。
     * @param callback 到期后执行的回调。
     * @return 可用于取消的 Timer 句柄。
     */
    Timer Schedule(Clock::time_point deadline, std::function<void()> callback);

private:
    std::shared_ptr<Impl> m_impl;
    friend class Timer;
};

}  // namespace go2cpp::core
