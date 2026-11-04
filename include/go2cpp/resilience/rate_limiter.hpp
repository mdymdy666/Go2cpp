#pragma once

/**
 * @file rate_limiter.hpp
 * @brief 令牌桶限流器：平滑速率 + 允许突发，header-only、线程安全。
 *
 * ## 模型
 *
 * 桶容量 `burst`，每秒补充 `rate` 个令牌，最多补到 `burst`。
 * `TryAcquire(n)` 需要桶里至少有 n 个令牌，取走 n 个返回 true；否则
 * 立即返回 false（**不排队**，由调用方决定拒绝/降级/排队）。
 *
 * 与固定窗口计数相比：令牌桶没有窗口边界突刺，且天然支持突发
 * （桶里攒下的令牌可以一次用掉），适合 RPC 客户端限流。
 *
 * ## 时间源
 *
 * 通过 `Clock` 注入；生产用单调时钟，测试用假时钟把「等 1 秒补 1 个」
 * 变成零耗时。补充按需计算（lazy），后台无定时器、无线程。
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

#include "go2cpp/resilience/clock.hpp"

namespace go2cpp::resilience {

/// @brief 令牌桶配置。
struct RateLimiterConfig {
    /// 每秒补充的令牌数；负数按 0 处理（不补充），默认 10。
    double rate_per_second = 10.0;
    /// 桶容量（最大突发量）；负数按 0 处理（永远拒绝），默认 10。
    double burst = 10.0;
    /// 初始令牌数；负数表示「满桶」，大于 burst 时截断到 burst。
    double initial_tokens = -1.0;
};

/**
 * @brief 令牌桶限流器。
 *
 * @details 所有状态由一把互斥量保护，可被多个线程并发调用。浮点比较
 *          使用 `1e-9` 容差，避免 `0.1 + 0.2 != 0.3` 这类误拒。
 */
class RateLimiter {
public:
    /**
     * @brief 以「速率 + 突发」构造满桶限流器。
     *
     * @param rate_per_second [in] 每秒补充令牌数；负数按 0。
     * @param burst [in] 桶容量；负数按 0。
     * @param clock [in] 时间源；空则用 `steady_clock::now`。
     *
     * @return 无（构造函数）。
     */
    explicit RateLimiter(double rate_per_second, double burst, Clock clock = {})
        : RateLimiter(RateLimiterConfig{rate_per_second, burst, -1.0}, std::move(clock)) {}

    /**
     * @brief 以完整配置构造。
     *
     * @param config [in] 速率/容量/初始令牌；内部规范化非法值。
     * @param clock [in] 时间源；空则用 `steady_clock::now`。
     *
     * ## 并发/失败语义
     * - 构造时读一次时钟作为补充起点，不会补发「构造前」的令牌。
     */
    explicit RateLimiter(RateLimiterConfig config = {}, Clock clock = {})
        : config_(Normalize(config)),
          clock_(detail::ResolveClock(std::move(clock))),
          tokens_(InitialTokens(config_)),
          last_(clock_()) {}

    /**
     * @brief 尝试取走 `cost` 个令牌；不足则立即失败，不阻塞、不排队。
     *
     * @param cost [in] 本次消耗的令牌数；<= 0 视为「空操作」并返回 true。
     *                  大于 `burst` 的请求永远无法满足，直接返回 false。
     *
     * @return
     * - `true`：已扣减令牌——调用方**可以**执行受保护的操作；
     * - `false`：桶不足——调用方**不要**重试等待，应按业务选择拒绝
     *   （429 / 降级 / 入队），否则会退化成忙等。
     *
     * ## 定位
     * 系统的唯一入口限流点。绕过它直接打上游，会把「保护」变成「装饰」；
     * 放在出口处则能保护下游，两者按调用位置选择，不要重复限流。
     *
     * ## 步骤
     * step1. 非法/零消耗直接返回 true（不改变桶）；
     * step2. 请求量超过桶容量直接拒绝（避免无意义的补充计算）；
     * step3. 按经过时间懒补充令牌，上限 burst；
     * step4. 令牌足够（含 1e-9 容差）则扣减并返回 true，否则 false。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程；不阻塞（除竞争互斥量的微秒级等待）。
     * - 原子性：补充、检查、扣减在同一临界区，不会超发。
     * - 失败语义：`false` 不改变桶状态（调用方可以过一会再用同一个桶）。
     * - 不变量：返回 true 后 `AvailableTokens()` 单调不增（在无补充的瞬间）。
     */
    bool TryAcquire(double cost = 1.0);

    /**
     * @brief 查询当前可用令牌数（会先按经过时间补充）。
     *
     * @return 当前令牌数，范围 `[0, burst]`——只用于观测/调试；**不要**用
     *         「先查再取」代替 `TryAcquire`（两次调用之间状态会变）。
     */
    double AvailableTokens();

    /// 当前速率（令牌/秒）。
    double rate_per_second() const noexcept { return config_.rate_per_second; }

    /// 桶容量。
    double burst() const noexcept { return config_.burst; }

    /// 清空并重置为满桶（用于测试或运维手工恢复）。
    void Reset();

private:
    static RateLimiterConfig Normalize(RateLimiterConfig config) {
        if (!(config.rate_per_second > 0.0)) {
            config.rate_per_second = 0.0; // 同时处理 NaN 与负数。
        }
        if (!(config.burst > 0.0)) {
            config.burst = 0.0;
        }
        return config;
    }

    static double InitialTokens(const RateLimiterConfig& config) {
        if (config.initial_tokens < 0.0) {
            return config.burst;
        }
        return std::min(config.initial_tokens, config.burst);
    }

    /// 调用方必须持锁；按 (now - last_) 补充，时钟回退时不补充也不回写 last_。
    void RefillLocked(TimePoint now) {
        if (now <= last_) {
            return;
        }
        const double elapsed =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - last_).count();
        tokens_ = std::min(config_.burst, tokens_ + elapsed * config_.rate_per_second);
        last_ = now;
    }

    RateLimiterConfig config_;
    Clock clock_;
    mutable std::mutex mutex_;
    double tokens_;
    TimePoint last_;
};

inline bool RateLimiter::TryAcquire(double cost) {
    // step1: 非正消耗没有语义，直接放行且不改变桶。
    if (cost <= 0.0) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // step2: 超过桶容量的请求永远无法满足，不参与补充计算。
    if (cost > config_.burst) {
        return false;
    }
    // step3: 懒补充；容差保证 0.5 + 0.5 能拿到 1.0 个令牌。
    RefillLocked(clock_());
    constexpr double kEpsilon = 1e-9;
    if (tokens_ + kEpsilon >= cost) {
        // step4: 扣减并放行。
        tokens_ -= cost;
        return true;
    }
    return false;
}

inline double RateLimiter::AvailableTokens() {
    std::lock_guard<std::mutex> lock(mutex_);
    RefillLocked(clock_());
    return tokens_;
}

inline void RateLimiter::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_ = config_.burst;
    last_ = clock_();
}

}  // namespace go2cpp::resilience
