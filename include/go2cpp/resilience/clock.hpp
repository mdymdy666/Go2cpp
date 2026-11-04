#pragma once

/**
 * @file clock.hpp
 * @brief 韧性组件的公共时间源：可注入的单调时钟。
 *
 * @details 三个组件（Retry / CircuitBreaker / RateLimiter）都需要读时间。
 *          生产环境用 `steady_clock`；测试注入假时钟，把「等 30 秒冷却」
 *          变成零耗时。这里统一别名，避免每个头各写一份导致漂移。
 *
 *          默认时钟**故意不内联在成员初始化里**：每个组件在构造时用
 *          `detail::ResolveClock` 把空 function 替换为真实时钟，这样
 *          测试可以只传 `{}` 表示「用真实时间」。
 */
#include <chrono>
#include <functional>
#include <utility>

namespace go2cpp::resilience {

/// 单调时间点：所有超时/冷却/补充都基于它，不受系统墙钟调整影响。
using TimePoint = std::chrono::steady_clock::time_point;

/// 时间源：返回当前单调时间点；可为空（组件内部回退到真实时钟）。
using Clock = std::function<TimePoint()>;

namespace detail {

/// @brief 把可能为空的 Clock 解析成可调用对象；空则回退 `steady_clock::now`。
inline Clock ResolveClock(Clock clock) {
    if (clock) {
        return clock;
    }
    return [] { return std::chrono::steady_clock::now(); };
}

}  // namespace detail
}  // namespace go2cpp::resilience
