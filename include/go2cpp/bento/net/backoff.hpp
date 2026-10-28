#pragma once

/**
 * @file backoff.hpp
 * @brief 指数退避 + 抖动（MOD-12）。
 *
 * @details 第 n 次重试的名义延迟为 `base * 2^n`，封顶 `max`（默认 30s），
 *          再叠加 `±jitter_ratio` 的均匀抖动：
 *
 *          | 次数 | 名义 | 30s 上限 + 20% 抖动后的实际范围 |
 *          |------|------|----------------------------------|
 *          | 0    | 1s   | [0.8s, 1.2s]                     |
 *          | 1    | 2s   | [1.6s, 2.4s]                     |
 *          | 2    | 4s   | [3.2s, 4.8s]                     |
 *          | 5    | 30s  | [24s, 30s]（向上被上限截断）     |
 *
 *          抖动的作用是打散多个客户端同时断线后的重连风暴；上限保证不会
 *          「越退越久」直到永远不重连。`Next()` 本身**不限制次数**：是否放弃
 *          由调用方根据 `Attempts()` 决定（MOD-12 的约束是「不无限重试」，
 *          这属于重连策略而不是退避计算）。
 *
 * ## 可测性
 *
 *          随机源是内置的 splitmix64，种子由 `Config::seed` 指定，不读系统熵。
 *          因此同一组配置 + 同一 seed 的序列完全可复现；测试既可以直接断言
 *          数值，也可以只断言区间。
 *
 * ## 线程模型
 *
 *          非线程安全：一个连接一个实例，由该连接所属线程独占。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstdint>

namespace go2cpp::bento {

/**
 * @brief 指数退避计算器。
 */
class Backoff {
 public:
    /// 默认初始延迟：1 秒。
    static constexpr std::uint32_t kDefaultBaseMs = 1000;
    /// 默认延迟上限：30 秒。
    static constexpr std::uint32_t kDefaultMaxMs = 30000;
    /// 默认抖动比例：±20%。
    static constexpr double kDefaultJitterRatio = 0.2;

    /// @brief 退避参数。
    struct Config {
        std::uint32_t base_ms = kDefaultBaseMs;      ///< 首次名义延迟；0 视为 1ms。
        std::uint32_t max_ms = kDefaultMaxMs;        ///< 名义延迟上限；小于 base 时按 base 处理。
        double jitter_ratio = kDefaultJitterRatio;   ///< 抖动比例，限定在 [0, 1]。
        std::uint64_t seed = 0x9e3779b97f4a7c15ull;  ///< 随机种子；相同种子 -> 相同序列。
    };

    /// 使用默认配置构造。
    Backoff();
    /// 按给定配置构造（构造时校验并规范化）。
    explicit Backoff(Config config);

    /**
     * @brief 返回下一次退避毫秒数，并推进尝试计数。
     * @return 实际延迟，范围 `[1, max_ms]`。
     *
     * @details 抖动后仍钳制在上限内；下限为 1ms，避免极端参数下返回 0
     *          导致调用方忙循环重试。
     */
    std::uint32_t Next() noexcept;

    /// 回到首次退避：尝试计数清零，随机源复位到构造时的种子。
    void Reset() noexcept;

    /// 已调用 `Next()` 的次数（即下一次的指数）。
    std::uint32_t Attempts() const noexcept { return m_attempts; }

    /// 下一次的名义延迟（不含抖动、已封顶），用于日志/观测。
    std::uint32_t PeekNominalMs() const noexcept;

    /// 配置的初始延迟。
    std::uint32_t BaseMs() const noexcept { return m_base_ms; }
    /// 配置的延迟上限。
    std::uint32_t MaxMs() const noexcept { return m_max_ms; }
    /// 配置的抖动比例（已规范化到 [0, 1]）。
    double JitterRatio() const noexcept { return m_jitter_ratio; }

 private:
    /// splitmix64：状态推进 + 位混合，输出 64 位伪随机数。
    std::uint64_t NextRandom() noexcept;
    /// [0, 1) 均匀分布的 double（取高 53 位，保证精度与平台无关）。
    double NextUnit() noexcept;
    /// 第 attempts 次的名义延迟（base * 2^attempts，封顶 max）。
    std::uint32_t NominalDelayMs(std::uint32_t attempts) const noexcept;

    std::uint32_t m_base_ms;        ///< 已规范化的首次延迟。
    std::uint32_t m_max_ms;         ///< 已规范化的上限。
    double m_jitter_ratio;          ///< 已规范化到 [0, 1]。
    std::uint64_t m_initial_seed;   ///< 构造时的种子；Reset 用来复位。
    std::uint64_t m_state;          ///< splitmix64 当前状态。
    std::uint32_t m_attempts = 0;   ///< 已产生的退避次数。
};

}  // namespace go2cpp::bento
