#pragma once

/**
 * @file retry.hpp
 * @brief 指数退避 + 抖动的重试执行器（header-only，无运行时依赖）。
 *
 * ## 为什么不是「全都重试」
 *
 * 重试只对**幂等且暂态**的失败有意义：
 *
 * - 可重试：`kTimeout`（超时）、`kClosed`（连接被对端关闭/复用失效）、
 *   `kUnavailable`（上游暂不可用）；
 * - 不可重试：`kClientError`（4xx 类调用方错误——重试只会放大错误）、
 *   `kPermanent`（明确不可恢复，如数据校验失败）。
 *
 * 把「能不能重试」做成显式分类，而不是让调用方随手 `while (true)`，
 * 是为了避免重试风暴与重复副作用。
 *
 * ## 退避公式
 *
 * 第 n 次重试（n 从 1 开始）的延迟：
 *
 *     delay(n) = min(base_delay * 2^(n-1), max_delay) * jitter_factor
 *     jitter_factor ∈ [1 - jitter, 1 + jitter]
 *
 * 抖动默认 ±20%，防止上游恢复瞬间所有客户端同时重试（惊群）。
 * 最终延迟仍被 `max_delay` 截断。
 *
 * ## 用法
 *
 * ```cpp
 * go2cpp::resilience::Retry retry({4, 20ms, 200ms, 0.2});
 * auto r = retry.Execute([&] {
 *     return CallUpstream() ? ErrorClass::kOk : ErrorClass::kTimeout;
 * });
 * if (!r.ok) { 看 r.attempts / r.total_delay / r.last_error }
 * ```
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

namespace go2cpp::resilience {

/**
 * @brief 失败分类：决定「要不要重试」，不是完整的错误码。
 *
 * @details 调用方把具体错误（ErrorPtr / errno / HTTP 状态码）映射到这
 *          六类之一。只有前三类暂态错误可重试。
 */
enum class ErrorClass : std::uint8_t {
    kOk = 0,      ///< 成功。
    kTimeout,     ///< 超时：请求可能已到达上游，调用方需保证幂等。
    kClosed,      ///< 连接关闭/失效：换连接重试通常有效。
    kUnavailable, ///< 上游暂不可用：拒绝连接、过载、leader 切换中。
    kClientError, ///< 调用方错误（4xx）：重试无意义，必须改请求。
    kPermanent,   ///< 永久失败：数据损坏、校验失败、确定性错误。
};

/// @brief 该失败类是否值得重试；只认 Timeout / Closed / Unavailable。
inline constexpr bool IsRetryable(ErrorClass error) noexcept {
    return error == ErrorClass::kTimeout || error == ErrorClass::kClosed ||
           error == ErrorClass::kUnavailable;
}

/// @brief 重试策略配置。
struct RetryConfig {
    /// 总尝试次数（含首次调用）；0 表示不执行、1 表示只试一次、默认 3。
    std::uint32_t max_attempts = 3;
    /// 首次退避基数；负数按 0 处理。
    std::chrono::milliseconds base_delay{10};
    /// 单次退避上限（抖动后仍不超过它）；负数按 0 处理。
    std::chrono::milliseconds max_delay{1000};
    /// 抖动比例 [0, 1]；0 = 无抖动，1 = 延迟在 [0, 2x] 之间。
    double jitter = 0.2;
};

/// @brief 一次重试执行的结果。
struct RetryResult {
    bool ok = false;                          ///< 最终是否成功。
    std::uint32_t attempts = 0;               ///< 实际调用次数（含首次）。
    std::chrono::milliseconds total_delay{0}; ///< 累计退避时长（不含业务耗时）。
    ErrorClass last_error = ErrorClass::kOk;  ///< 最后一次失败分类；成功时为 kOk。
};

/**
 * @brief 指数退避重试执行器。
 *
 * @details 对象持有配置与随机数发生器；`Execute` 的计数全部是局部变量，
 *          因此同一对象可以被多个线程并发调用（RNG 由内部互斥量保护）。
 *          注入的 `sleeper` 由调用方保证线程安全。
 */
class Retry {
public:
    using Sleeper = std::function<void(std::chrono::milliseconds)>;

    /**
     * @brief 构造重试执行器。
     *
     * @param config [in] 策略配置；内部会规范化非法值（负延迟→0、jitter 截断到 [0,1]）。
     * @param sleeper [in] 睡眠函数，默认 `std::this_thread::sleep_for`；
     *                     测试注入假睡眠即可零耗时验证退避序列。
     * @param seed [in] 抖动的随机种子；默认 `std::random_device`，测试可固定以复现。
     *
     * @return 无（构造函数）。
     *
     * ## 并发/失败语义
     * - 构造后配置不可变（`config()` 只读），可安全跨线程共享。
     * - 规范化是纯函数，不会抛异常。
     */
    explicit Retry(RetryConfig config = {}, Sleeper sleeper = {},
                   std::uint64_t seed = std::random_device{}())
        : config_(Normalize(config)), sleeper_(std::move(sleeper)), rng_(seed) {
        if (!sleeper_) {
            sleeper_ = [](std::chrono::milliseconds delay) {
                if (delay.count() > 0) {
                    std::this_thread::sleep_for(delay);
                }
            };
        }
    }

    /// 生效配置（已规范化）。
    const RetryConfig& config() const noexcept { return config_; }

    /**
     * @brief 按策略执行一个可重试操作，直到成功、遇到不可重试错误或次数耗尽。
     *
     * @param fn [in] 操作；必须返回 `ErrorClass`（`kOk` 表示成功）。
     *                调用方保证操作幂等——超时/关闭时它可能已被上游执行。
     *
     * @return
     * - `ok == true`：操作成功——调用方**必须**按成功处理，不要再看 `last_error`；
     * - `ok == false`：`last_error` 是最终失败分类——调用方**不要**自行再重试，
     *   按 `last_error`（4xx 直接上报 / 暂态错误上报断路）处理。
     *
     * ## 定位
     * 系统的唯一重试入口。若四处散落 `while (retry--)`，会绕过熔断与预算，
     * 造成重试风暴（一次用户请求放大成几十次上游调用），进而打死上游。
     *
     * ## 步骤
     * step1. `max_attempts == 0` 直接返回失败，一次都不调用；
     * step2. 调用操作并累计 attempts；成功立即返回；
     * step3. 不可重试错误立即返回，不做无意义退避；
     * step4. 若还有剩余次数，计算第 n 次重试延迟、累计并睡眠；
     * step5. 次数用尽返回最后一次失败，由调用方决定上报方式。
     *
     * ## 并发/失败语义
     * - 调用约束：可在任意线程调用；`fn` 与 `sleeper` 不得依赖调用线程的私有状态。
     * - 原子性：无共享状态写入（仅 RNG 加锁），无需调用方加锁。
     * - 失败语义：不抛异常（除非 `fn` 自身抛出）；`fn` 抛出会原样传播，重试停止。
     * - 不变量：返回 `ok == true` 当且仅当最后一次 `fn()` 返回 `kOk`。
     */
    template <typename Fn>
    RetryResult Execute(Fn&& fn) {
        RetryResult result;
        if (config_.max_attempts == 0) {
            return result;
        }
        for (std::uint32_t attempt = 1; attempt <= config_.max_attempts; ++attempt) {
            // step1: 先记账再调用，保证 attempts 反映真实调用次数。
            ++result.attempts;
            const ErrorClass error = fn();
            if (error == ErrorClass::kOk) {
                // step2: 成功即返回，last_error 保持 kOk。
                result.ok = true;
                result.last_error = ErrorClass::kOk;
                return result;
            }
            result.last_error = error;
            // step3: 4xx / 永久错误不重试，避免放大错误与重复副作用。
            if (!IsRetryable(error)) {
                return result;
            }
            if (attempt == config_.max_attempts) {
                break;
            }
            // step4: 第 attempt 次重试的延迟（attempt 从 1 开始）。
            const std::chrono::milliseconds delay = DelayForRetry(attempt);
            result.total_delay += delay;
            sleeper_(delay);
        }
        // step5: 次数耗尽，ok 保持 false。
        return result;
    }

    /**
     * @brief 计算第 `retry_index` 次重试的退避延迟（含抖动）。
     *
     * @param retry_index [in] 第几次重试，1 表示首次重试；0 返回 0。
     *
     * @return 延迟毫秒数，范围 `[0, max_delay]`——调用方**必须**按实际返回
     *         值睡眠，不要用理论值自行计算（抖动在内部）。
     *
     * ## 并发/失败语义
     * - 每次调用会消耗一次随机数；多线程并发的结果不保证可复现。
     * - 不修改配置，纯计算（除 RNG 状态）。
     */
    std::chrono::milliseconds DelayForRetry(std::uint32_t retry_index) {
        if (retry_index == 0) {
            return std::chrono::milliseconds{0};
        }
        const auto base = static_cast<double>(config_.base_delay.count());
        const auto cap = static_cast<double>(config_.max_delay.count());
        double raw = base;
        for (std::uint32_t i = 1; i < retry_index && raw < cap; ++i) {
            raw *= 2.0;
        }
        raw = std::min(raw, cap);
        const double factor = JitterFactor();
        double delay = raw * factor;
        delay = std::clamp(delay, 0.0, cap);
        return std::chrono::milliseconds{static_cast<std::int64_t>(delay + 0.5)};
    }

private:
    static RetryConfig Normalize(RetryConfig config) {
        if (config.base_delay.count() < 0) {
            config.base_delay = std::chrono::milliseconds{0};
        }
        if (config.max_delay.count() < 0) {
            config.max_delay = std::chrono::milliseconds{0};
        }
        if (config.jitter < 0.0) {
            config.jitter = 0.0;
        }
        if (config.jitter > 1.0) {
            config.jitter = 1.0;
        }
        return config;
    }

    /// 返回 [1 - jitter, 1 + jitter] 内的随机倍率；jitter == 0 时恒为 1。
    double JitterFactor() {
        if (config_.jitter <= 0.0) {
            return 1.0;
        }
        std::lock_guard<std::mutex> lock(rng_mutex_);
        std::uniform_real_distribution<double> dist(-config_.jitter, config_.jitter);
        return 1.0 + dist(rng_);
    }

    RetryConfig config_;
    Sleeper sleeper_;
    std::mutex rng_mutex_;
    std::mt19937_64 rng_;
};

}  // namespace go2cpp::resilience
