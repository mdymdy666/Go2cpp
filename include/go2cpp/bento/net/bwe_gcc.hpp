#pragma once

/**
 * @file bwe_gcc.hpp
 * @brief 延迟梯度带宽估计（GCC / trendline 简化版）：纯计算、可裁剪、可被
 *        任意 pacer 消费。
 * @details 为什么需要这一层：bento 现有 `CongestionController` 的 BBR 与
 *          NewReno 都是**丢包驱动/带宽探测**——链路排队导致 RTT 上升但尚未
 *          丢包时，两者都不会提前降速，等丢包出现时队列已经排满。GCC 的
 *          价值是**在丢包之前**用「到达间隔 − 发送间隔」的累计延迟斜率
 *          （trend）判断「是否正在排队」，据此做 AIMD 调速率。
 *
 *          本模块把 WebRTC `trendline_estimator.cc` 与
 *          `aimd_rate_control.cc` 的最小可用子集抽成**零 IO、零 fiber、
 *          零全局状态**的确定性计算：
 *          - 输入：按发送顺序到达的逐包反馈 `(send_ms, arrival_ms, bytes)`；
 *          - 输出：`TargetBitrate()`（字节/秒），可直接交给
 *            `bento::Pacer::SetRate(bwe.TargetBitrate(), now_ms)`。
 *
 *          刻意省略的部分（详见 docs/modules/bwe_gcc.md「与 WebRTC 的差异」）：
 *          没有 transport-cc/REMB 反馈解析、没有吞吐量作为增长上限、
 *          没有 link capacity 的加性增切换、没有 ProbeRTT/ALR、没有丢包
 *          模型融合。这些省略让模块可以独立构建与单测，也避免把公网复杂度
 *          拖进内网路径。
 *
 * @note 线程模型：**非线程安全**。每个连接/方向一个实例，由调用方串行喂
 *       反馈；模块内部不读时钟，所有时间由调用方传入，便于确定性测试。
 * @author go2cpp contributors
 * @date 2026-11-08
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

namespace go2cpp::bento {

/// @brief trendline 的三态判定（与 WebRTC `BandwidthUsage` 同名语义）。
enum class GccTrendState : std::uint8_t {
    kNormal = 0,     ///< 无排队增长信号（可缓慢增速率）。
    kOverusing = 1,  ///< 延迟梯度持续为正：链路队列在增长（应降速）。
    kUnderusing = 2, ///< 延迟梯度持续为负：队列在排空（可正常增长）。
};

/// @brief 三态名称（日志/报告用）。
const char* GccTrendStateName(GccTrendState state) noexcept;

/// @brief 增长模式。
enum class GccIncreaseMode : std::uint8_t {
    /// WebRTC `aimd_rate_control.cc` 的默认语义：每秒 ×α（α=1.08，即 +8%/s）。
    kMultiplicative = 0,
    /// WebRTC 在有链路容量估计时使用的加性增：每秒 +N 字节（默认 4000 B/s）。
    kAdditive = 1,
};

/// @brief 延迟梯度 BWE 配置。时间单位毫秒、速率单位字节/秒。
struct BweGccConfig {
    // --- 包反馈分组 ---------------------------------------------------------
    /// 发送时间差小于该值的反馈合并为同一组（WebRTC `kSendTimeGroupLength`）。
    /// 分组避免「同一发送时刻的多个包」被到达抖动放大成多个 delta。
    double send_time_group_ms = 5.0;

    // --- trendline 回归 -----------------------------------------------------
    /// 延迟历史的滑动窗口样本数（WebRTC `TrendlineEstimator` 默认 20）。
    std::size_t trend_window = 20;
    /// 累计延迟的 EWMA 平滑系数（WebRTC `kSmoothingFactor = 0.9`）。
    double smoothing_factor = 0.9;
    /// trend 放大系数（WebRTC `threshold_gain_ = 4.0`）。
    double threshold_gain = 4.0;

    // --- 自适应阈值 ---------------------------------------------------------
    /// 初始阈值（WebRTC `kDefaultThreshold = 12.5`）。
    double initial_threshold = 12.5;
    /// 阈值下限（WebRTC `kMinThreshold = 6`）。
    double min_threshold = 6.0;
    /// 阈值上限（WebRTC `kMaxThreshold = 600`）。
    double max_threshold = 600.0;
    /// 阈值在正常态的上调速率（WebRTC `k_up = 0.0087`，每秒缩放）。
    double threshold_up_per_sec = 0.0087;
    /// 阈值在过载态的上调速率（WebRTC `k_down = 0.039`，每秒缩放）。
    double threshold_down_per_sec = 0.039;

    // --- 过度使用检测 -------------------------------------------------------
    /// 触发 overuse 所需的连续超阈值次数。WebRTC 用时间窗（10ms）判定，
    /// 本实现同时要求「连续 N 次」与「持续 ≥ overuse_time_threshold_ms」，
    /// 两者都满足才切 kOverusing，避免单个尖峰误判。
    std::uint32_t overuse_count_threshold = 2;
    /// overuse/underuse 的最短持续时间（WebRTC `overuse_time_threshold_ms=10`）。
    std::uint32_t overuse_time_threshold_ms = 10;

    // --- AIMD 速率控制 ------------------------------------------------------
    /// 初始目标速率（字节/秒）。200 KiB/s ≈ 1.6 Mbps，是公网 RPC 的温和起点。
    std::uint64_t initial_bitrate_bytes_per_sec = 200 * 1024;
    /// 目标速率下限（WebRTC GCC 下限约 10 kbps；这里取 12.5 KB/s = 100 kbps）。
    std::uint64_t min_bitrate_bytes_per_sec = 12500;
    /// 目标速率上限（默认 50 MiB/s，可按部署场景收紧）。
    std::uint64_t max_bitrate_bytes_per_sec = 50 * 1024 * 1024;
    /// 乘性增系数：每秒 ×alpha（WebRTC `kIncreaseFactor = 1.08`）。
    double alpha = 1.08;
    /// 乘性减系数：降速 ×beta（WebRTC `kDecreaseFactor = 0.85`，降 15%）。
    double beta = 0.85;
    /// 降速后的 hold 时长：hold 期内**禁止增长**（WebRTC 的 kRcHold 状态）。
    std::uint32_t hold_ms = 500;
    /// 两次降速之间的最小间隔。hold 结束后若仍 overuse，才允许再次降速；
    /// 避免持续排队时每个反馈样本都乘一次 0.85（指数雪崩）。
    std::uint32_t decrease_interval_ms = 500;
    /// 增长模式（默认乘性增，与任务给定的 α=1.08/s 对齐）。
    GccIncreaseMode increase_mode = GccIncreaseMode::kMultiplicative;
    /// 加性增速率（仅 kAdditive 模式使用；WebRTC 的 `kAdditiveIncreaseBps` 为
    /// `max(4000, avg_packet / (2 × (rtt + 100ms)))`，这里用固定下限值）。
    std::uint64_t additive_increase_bytes_per_sec = 4000;
};

/// @brief 一次反馈后的 trendline 结果。
struct GccTrendlineSample {
    bool valid = false;                        ///< false 表示首包/同组/乱序，未产生 delta。
    double trend = 0.0;                        ///< 最近一次回归斜率（ms/ms）。
    double modified_trend = 0.0;               ///< min(count,60) × trend × gain。
    double threshold = 12.5;                   ///< 当前自适应阈值。
    GccTrendState state = GccTrendState::kNormal;
    std::uint64_t now_ms = 0;                  ///< 本次反馈的到达时刻。
};

/// @brief 延迟梯度 + 自适应阈值 + 三态判定（WebRTC trendline 的最小实现）。
/// @details 输入是逐包 `(send_ms, arrival_ms)`；内部按发送时间 5ms 分组，
///          计算 `delta = (arrival_delta - send_delta)`，对累计延迟做 EWMA
///          后用滑动窗口线性回归求 trend。`modified_trend` 与自适应阈值
///          比较，连续超阈值且持续足够久才判定 kOverusing。
class GccTrendlineEstimator final {
   public:
    explicit GccTrendlineEstimator(const BweGccConfig& config = BweGccConfig());

    /// @brief 换配置并重置全部历史（配置会做 0 值/非法值归一化）。
    void SetConfig(const BweGccConfig& config);
    /// @brief 当前配置（只读）。
    const BweGccConfig& config() const noexcept {
        return m_config;
    }

    /// @brief 输入一个包反馈。
    /// @param send_ms [in] 包的发送时刻（毫秒，单调不减）；乱序/回退样本被丢弃。
    /// @param arrival_ms [in] 包的到达时刻（毫秒，必须 ≥ send_ms）。
    /// @return 本次是否产生有效 delta 及最新三态；首包/同组样本返回
    ///         `valid=false`，调用方不应据此驱动速率控制。
    GccTrendlineSample Update(std::uint64_t send_ms, std::uint64_t arrival_ms);

    /// @brief 累计延迟（ms，未平滑）。
    double accumulated_delay_ms() const noexcept {
        return m_accumulated_delay_ms;
    }
    /// @brief 平滑后的累计延迟（ms）。
    double smoothed_delay_ms() const noexcept {
        return m_smoothed_delay_ms;
    }
    /// @brief 最近一次回归斜率（ms/ms）。
    double trend() const noexcept {
        return m_trend;
    }
    /// @brief 最近一次 modified trend。
    double modified_trend() const noexcept {
        return m_modified_trend;
    }
    /// @brief 当前自适应阈值。
    double threshold() const noexcept {
        return m_threshold;
    }
    /// @brief 当前三态。
    GccTrendState state() const noexcept {
        return m_state;
    }
    /// @brief 已产生的有效 delta 数。
    std::uint64_t sample_count() const noexcept {
        return m_delta_count;
    }
    /// @brief 被丢弃的反馈数（乱序/到达早于发送）。
    std::uint64_t dropped_feedback() const noexcept {
        return m_dropped_feedback;
    }
    /// @brief 清空历史，阈值回到 initial_threshold。
    void Reset();

   private:
    /// @brief 用最近一次 modified_trend 按时间缩放更新阈值。
    void UpdateThreshold(double modified_trend, std::uint64_t now_ms);
    /// @brief 对 delay 历史做最小二乘线性回归，返回斜率（ms/ms）。
    double FitSlope() const noexcept;
    /// @brief 更新 overuse/underuse 状态机（连续次数 + 持续时间双条件）。
    void UpdateHypothesis(bool overuse, bool underuse, std::uint64_t now_ms);

    BweGccConfig m_config;

    bool m_has_prev = false;
    std::uint64_t m_prev_send_ms = 0;
    std::uint64_t m_prev_arrival_ms = 0;
    std::uint64_t m_last_send_ms = 0;
    bool m_has_last_send = false;

    bool m_has_delay_base = false;
    double m_accumulated_delay_ms = 0.0;
    double m_smoothed_delay_ms = 0.0;
    /// (arrival_ms, smoothed_delay_ms)，最多 trend_window 个。
    std::deque<std::pair<double, double>> m_delay_history;

    double m_threshold = 12.5;
    std::uint64_t m_last_threshold_update_ms = 0;
    bool m_has_threshold_update = false;

    double m_trend = 0.0;
    double m_modified_trend = 0.0;
    std::uint64_t m_delta_count = 0;
    std::uint64_t m_dropped_feedback = 0;

    GccTrendState m_state = GccTrendState::kNormal;
    bool m_overuse_active = false;
    std::uint64_t m_overuse_start_ms = 0;
    std::uint32_t m_overuse_count = 0;
    bool m_underuse_active = false;
    std::uint64_t m_underuse_start_ms = 0;
    std::uint32_t m_underuse_count = 0;
};

/// @brief AIMD 速率控制器（α=1.08/s、β=0.85、降后 hold）。
/// @details 只做「状态 + 时间 → 速率」的纯计算：
///          - kNormal/kUnderusing 且在 hold 之外：按 alpha 或加性步长增长；
///          - kOverusing 且距上次降速 ≥ decrease_interval：速率 ×beta 并 hold；
///          - hold 期内：目标速率保持不变，且不累计「欠账」，避免 hold 结束
///            后一次性补涨。
class GccAimdRateController final {
   public:
    explicit GccAimdRateController(const BweGccConfig& config = BweGccConfig());

    /// @brief 换配置并重置速率到 initial_bitrate。
    void SetConfig(const BweGccConfig& config);
    /// @brief 当前配置（只读）。
    const BweGccConfig& config() const noexcept {
        return m_config;
    }

    /// @brief 用最新三态驱动速率。
    /// @param state [in] trendline 三态；kOverusing 触发降速，其余允许增长。
    /// @param now_ms [in] 当前到达时刻（毫秒，单调不减）；回退时按上次时间处理。
    void OnState(GccTrendState state, std::uint64_t now_ms);

    /// @brief 当前目标速率（字节/秒）。
    std::uint64_t TargetBitrate() const noexcept;
    /// @brief hold 截止时刻（毫秒）；0 表示没有 hold。
    std::uint64_t hold_until_ms() const noexcept {
        return m_hold_until_ms;
    }
    /// @brief 当前是否处于 hold 期。
    bool IsHolding(std::uint64_t now_ms) const noexcept {
        return now_ms < m_hold_until_ms;
    }
    /// @brief 累计增长次数。
    std::uint64_t increase_count() const noexcept {
        return m_increase_count;
    }
    /// @brief 累计降速次数。
    std::uint64_t decrease_count() const noexcept {
        return m_decrease_count;
    }
    /// @brief 因 hold 被挡住（且本应增长）的次数。
    std::uint64_t hold_block_count() const noexcept {
        return m_hold_block_count;
    }
    /// @brief 清空状态，速率回到 initial_bitrate。
    void Reset();

   private:
    void Increase(std::uint64_t now_ms);
    void Decrease(std::uint64_t now_ms);
    std::uint64_t ClampRate(double rate) const noexcept;

    BweGccConfig m_config;
    double m_rate = 0.0;
    bool m_has_last_update = false;
    std::uint64_t m_last_update_ms = 0;
    std::uint64_t m_hold_until_ms = 0;
    std::uint64_t m_next_decrease_ms = 0;
    std::uint64_t m_increase_count = 0;
    std::uint64_t m_decrease_count = 0;
    std::uint64_t m_hold_block_count = 0;
};

/// @brief 延迟梯度 BWE 的可观测统计。
struct BweGccStats {
    std::uint64_t feedback_count = 0;   ///< 收到的反馈总数。
    std::uint64_t trend_samples = 0;    ///< 产生有效 delta 的反馈数。
    std::uint64_t dropped_feedback = 0; ///< 乱序/非法时间戳被丢弃的反馈数。
    std::uint64_t overuse_events = 0;   ///< 状态跃迁到 kOverusing 的次数。
    std::uint64_t underuse_events = 0;  ///< 状态跃迁到 kUnderusing 的次数。
    std::uint64_t decreases = 0;        ///< 真正执行降速的次数。
    std::uint64_t increases = 0;        ///< 真正执行增长的次数。
    std::uint64_t hold_blocks = 0;      ///< 因 hold 被挡住的增长次数。
    std::uint64_t bytes_reported = 0;   ///< 反馈携带的字节数累计。
    double last_trend = 0.0;
    double last_modified_trend = 0.0;
    double last_threshold = 0.0;
};

/// @brief 延迟梯度 BWE 的对外入口：反馈进、目标码率出。
/// @details 组合 `GccTrendlineEstimator` 与 `GccAimdRateController`，只做
///          转发与统计，不引入任何 IO/fiber/锁依赖。
class BweGcc final {
   public:
    explicit BweGcc(const BweGccConfig& config = BweGccConfig());

    /// @brief 换配置并重置（等价于新建实例）。
    void SetConfig(const BweGccConfig& config);
    /// @brief 当前配置（只读）。
    const BweGccConfig& config() const noexcept {
        return m_config;
    }

    /// @brief 输入一个包反馈并推进一次目标码率。
    /// @param send_ms [in] 发送时刻（毫秒，必须按发送顺序单调不减）。
    /// @param arrival_ms [in] 到达时刻（毫秒，必须 ≥ send_ms）。
    /// @param payload_bytes [in] 该包字节数；仅用于统计，0 合法。
    /// @return 无；最新速率通过 `TargetBitrate()` 读取。
    /// @note 首包、5ms 内的同组包、乱序包不会产生 delta，也不会改变速率。
    void OnPacketFeedback(std::uint64_t send_ms, std::uint64_t arrival_ms,
                          std::size_t payload_bytes = 1200);

    /// @brief 当前目标码率（字节/秒），可直接传给 `Pacer::SetRate`。
    std::uint64_t TargetBitrate() const noexcept;
    /// @brief 与 `TargetBitrate()` 同值的显式命名别名（字节/秒）。
    std::uint64_t target_bitrate_bytes_per_sec() const noexcept {
        return TargetBitrate();
    }
    /// @brief 当前三态。
    GccTrendState state() const noexcept {
        return m_trendline.state();
    }
    /// @brief 最近一次回归斜率（ms/ms）。
    double trend() const noexcept {
        return m_trendline.trend();
    }
    /// @brief 最近一次 modified trend。
    double modified_trend() const noexcept {
        return m_trendline.modified_trend();
    }
    /// @brief 当前自适应阈值。
    double threshold() const noexcept {
        return m_trendline.threshold();
    }
    /// @brief 累计延迟（ms）。
    double accumulated_delay_ms() const noexcept {
        return m_trendline.accumulated_delay_ms();
    }
    /// @brief 平滑累计延迟（ms）。
    double smoothed_delay_ms() const noexcept {
        return m_trendline.smoothed_delay_ms();
    }
    /// @brief hold 截止时刻（毫秒）。
    std::uint64_t hold_until_ms() const noexcept {
        return m_aimd.hold_until_ms();
    }
    /// @brief 当前是否处于降速后的 hold 期。
    bool IsHolding(std::uint64_t now_ms) const noexcept {
        return m_aimd.IsHolding(now_ms);
    }
    /// @brief 统计快照（只读引用，随反馈更新）。
    const BweGccStats& stats() const noexcept {
        return m_stats;
    }
    /// @brief 清空历史与速率（连接重建/方向切换时调用）。
    void Reset();

   private:
    BweGccConfig m_config;
    GccTrendlineEstimator m_trendline;
    GccAimdRateController m_aimd;
    BweGccStats m_stats;
    GccTrendState m_last_state = GccTrendState::kNormal;
};

}  // namespace go2cpp::bento
