#pragma once

/**
 * @file circuit_breaker.hpp
 * @brief 三态熔断器（Closed / Open / HalfOpen），header-only、线程安全。
 *
 * ## 状态机
 *
 * ```
 *           失败数 >= failure_threshold
 *   Closed ───────────────────────────────► Open
 *      ▲                                     │ 冷却 cooldown 到期
 *      │ 连续成功 >= success_threshold       ▼
 *      └──────────────────────────────── HalfOpen
 *                          HalfOpen 中任一失败
 *                          └──────────────► Open（重新冷却）
 * ```
 *
 * - **Closed**：正常放行，统计连续失败；成功一次清零。
 * - **Open**：直接拒绝（`Allow() == false`），保护已过载的上游。
 * - **HalfOpen**：冷却到期后放行探测请求；成功累计到 `success_threshold`
 *   才回到 Closed，任一失败立刻重新 Open。
 *
 * 半开阶段**放行所有探测请求**而不是只放一个：实现简单、避免在途请求
 * 计数泄漏；代价是上游刚恢复时会有小簇探测流量，需配合 `success_threshold`
 * 使用。若上游恢复极慢，建议 success_threshold >= 2。
 */
#include <chrono>
#include <cstdint>
#include <mutex>

#include "go2cpp/resilience/clock.hpp"

namespace go2cpp::resilience {

/// @brief 熔断器三态。
enum class CircuitState : std::uint8_t {
    kClosed = 0, ///< 闭合：正常放行。
    kOpen,       ///< 打开：拒绝请求。
    kHalfOpen,   ///< 半开：放行探测，等待恢复确认。
};

/// @brief 熔断器配置。
struct CircuitBreakerConfig {
    /// 连续失败达到该值即打开；0 按 1 处理，默认 5。
    std::uint32_t failure_threshold = 5;
    /// 半开阶段连续成功达到该值即闭合；0 按 1 处理，默认 2。
    std::uint32_t success_threshold = 2;
    /// Open 冷却时长，到期转 HalfOpen；负数按 0 处理，默认 30s。
    std::chrono::milliseconds cooldown{30000};
};

/**
 * @brief 三态熔断器：用「拒绝」替代「继续打死上游」。
 *
 * @details 所有公开方法持同一把互斥量，可被多个线程/fiber 并发调用。
 *          锁内只做算术与状态转移，不调用用户回调，不会重入。
 */
class CircuitBreaker {
public:
    /**
     * @brief 构造熔断器。
     *
     * @param config [in] 阈值与冷却配置；非法值在内部规范化。
     * @param clock [in] 时间源；为空时用 `steady_clock::now`。
     *
     * @return 无（构造函数）。
     *
     * ## 并发/失败语义
     * - 构造后阈值不可变；状态由互斥量保护。
     */
    explicit CircuitBreaker(CircuitBreakerConfig config = {}, Clock clock = {})
        : config_(Normalize(config)), clock_(detail::ResolveClock(std::move(clock))) {}

    /**
     * @brief 询问当前是否放行一次调用；同时驱动 Open → HalfOpen 的冷却转移。
     *
     * @return
     * - `true`：调用方**必须**在结束后恰好调用一次 `RecordSuccess` 或
     *   `RecordFailure`（否则半开探测统计失真）；
     * - `false`：调用方**不要**调用上游，按快速失败处理（返回 cached 值/
     *   降级响应）。
     *
     * ## 定位
     * 全链路唯一放行闸门。若调用方绕过它直接请求，熔断打开也保护不了上游。
     *
     * ## 步骤
     * step1. 取锁与当前时间（时间源在锁内调用，保证状态转移基于同一时刻）；
     * step2. Closed 直接放行；
     * step3. Open 检查冷却：到期则转 HalfOpen 并放行首探；未到期拒绝；
     * step4. HalfOpen 继续放行探测请求。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程；不可在持有本类锁时递归调用（无递归锁）。
     * - 原子性：检查与状态转移在同一临界区内完成，不会出现两个线程同时
     *   把 Open 转成 HalfOpen 的重复计数。
     * - 失败语义：noexcept 级别（仅 std::mutex 可能抛 system_error）。
     */
    bool Allow();

    /**
     * @brief 记录一次成功，推进 HalfOpen → Closed。
     *
     * @return 无。
     *
     * ## 步骤
     * step1. Closed：清零连续失败计数；
     * step2. HalfOpen：累计探测成功，达到阈值则闭合；
     * step3. Open：忽略——在途请求的成功不能关闸，否则会掩盖持续故障。
     *
     * ## 并发/失败语义
     * - 必须与 `Allow() == true` 配对；钥匙不存在时调用会污染统计。
     * - 加锁修改状态；不调用回调。
     */
    void RecordSuccess();

    /**
     * @brief 记录一次失败，推进 Closed → Open 或 HalfOpen → Open。
     *
     * @return 无。
     *
     * ## 步骤
     * step1. Closed：连续失败 +1，达到阈值则打开并记录打开时刻；
     * step2. HalfOpen：一次失败立即重新打开（探测失败说明上游仍不可用）；
     * step3. Open：忽略重复失败，避免把冷却起点不断后移。
     *
     * ## 并发/失败语义
     * - 必须与 `Allow() == true` 配对。
     * - 打开瞬间清零计数器，保证下一次冷却是从「最近一次打开」起算。
     */
    void RecordFailure();

    /// 当前状态（只读快照；不会因冷却到期而自动转 HalfOpen，需先 Allow）。
    CircuitState state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    /// 连续失败计数（Closed 语义下有意义；Open/HalfOpen 下为 0）。
    std::uint32_t consecutive_failures() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return consecutive_failures_;
    }

    /**
     * @brief Open 状态还需等待多久才放行探测。
     *
     * @return 剩余冷却；非 Open 状态或冷却已到期返回 0——调用方**不要**用它
     *         代替 `Allow()` 做放行判断（存在检查与使用之间的时间窗）。
     */
    std::chrono::milliseconds retry_after() const;

    /// 生效配置（已规范化）。
    const CircuitBreakerConfig& config() const noexcept { return config_; }

private:
    static CircuitBreakerConfig Normalize(CircuitBreakerConfig config) {
        if (config.failure_threshold == 0) {
            config.failure_threshold = 1;
        }
        if (config.success_threshold == 0) {
            config.success_threshold = 1;
        }
        if (config.cooldown.count() < 0) {
            config.cooldown = std::chrono::milliseconds{0};
        }
        return config;
    }

    /// 调用方必须持锁；记录打开时刻并清零全部计数器。
    void OpenLocked(TimePoint now) {
        state_ = CircuitState::kOpen;
        opened_at_ = now;
        consecutive_failures_ = 0;
        halfopen_successes_ = 0;
    }

    CircuitBreakerConfig config_;
    Clock clock_;
    mutable std::mutex mutex_;
    CircuitState state_ = CircuitState::kClosed;
    TimePoint opened_at_{};
    std::uint32_t consecutive_failures_ = 0;
    std::uint32_t halfopen_successes_ = 0;
};

inline bool CircuitBreaker::Allow() {
    std::lock_guard<std::mutex> lock(mutex_);
    const TimePoint now = clock_();
    // step1/step2: 闭合态无条件放行。
    if (state_ == CircuitState::kClosed) {
        return true;
    }
    if (state_ == CircuitState::kOpen) {
        // step3: 冷却到期才允许探测；未到期直接拒绝。
        if (now - opened_at_ >= config_.cooldown) {
            state_ = CircuitState::kHalfOpen;
            halfopen_successes_ = 0;
            return true;
        }
        return false;
    }
    // step4: 半开态继续放行探测，由成功/失败计数决定去向。
    return true;
}

inline void CircuitBreaker::RecordSuccess() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == CircuitState::kClosed) {
        // step1: 成功打断「连续失败」，这是熔断不误开的依据。
        consecutive_failures_ = 0;
        return;
    }
    if (state_ == CircuitState::kHalfOpen) {
        // step2: 累计到 success_threshold 才闭合，避免一次侥幸成功就全量放行。
        ++halfopen_successes_;
        if (halfopen_successes_ >= config_.success_threshold) {
            state_ = CircuitState::kClosed;
            consecutive_failures_ = 0;
            halfopen_successes_ = 0;
        }
        return;
    }
    // step3: Open 期间在途成功不改变熔断决策。
}

inline void CircuitBreaker::RecordFailure() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == CircuitState::kClosed) {
        // step1: 连续失败累加；一次成功会清零（见 RecordSuccess）。
        ++consecutive_failures_;
        if (consecutive_failures_ >= config_.failure_threshold) {
            OpenLocked(clock_());
        }
        return;
    }
    if (state_ == CircuitState::kHalfOpen) {
        // step2: 探测失败立即重新打开，冷却从头算。
        OpenLocked(clock_());
        return;
    }
    // step3: 已打开时重复失败忽略，冷却起点不被推迟。
}

inline std::chrono::milliseconds CircuitBreaker::retry_after() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != CircuitState::kOpen) {
        return std::chrono::milliseconds{0};
    }
    const TimePoint now = clock_();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - opened_at_);
    if (elapsed >= config_.cooldown) {
        return std::chrono::milliseconds{0};
    }
    return config_.cooldown - elapsed;
}

}  // namespace go2cpp::resilience
