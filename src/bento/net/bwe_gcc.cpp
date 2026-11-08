/**
 * @file bwe_gcc.cpp
 * @brief 延迟梯度带宽估计（GCC / trendline 简化版）实现。
 * @details 三个纯计算部件的组合：
 *          1) `GccTrendlineEstimator`：5ms 分组 → 累计延迟 → EWMA → 滑窗
 *             线性回归 → 自适应阈值 → overuse/underuse/normal 三态；
 *          2) `GccAimdRateController`：三态驱动的 α=1.08/s 增长、β=0.85
 *             降速与降后 hold；
 *          3) `BweGcc`：把反馈转发给前两者，输出 `TargetBitrate()`。
 *          没有 IO、没有时钟、没有锁、没有 fiber——时间全部由调用方提供，
 *          因此同样的反馈序列在任何平台上得到同样的比特率。
 * @author go2cpp contributors
 * @date 2026-11-08
 * @copyright MIT License
 */

#include "go2cpp/bento/net/bwe_gcc.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace go2cpp::bento {
namespace {

/// trendline 允许的最大样本数（防御性上限，避免配置成超大窗口后回归变慢）。
constexpr std::size_t kMaxTrendWindow = 1024;

/// 把任意 double 饱和到 [min_rate, max_rate] 的整数速率，NaN 取下限。
std::uint64_t SaturateRate(double rate, std::uint64_t min_rate, std::uint64_t max_rate) noexcept {
    if (!(rate > 0.0)) {  // 同时挡住 NaN/负值
        return min_rate;
    }
    const double upper = static_cast<double>(max_rate);
    if (rate >= upper) {
        return max_rate;
    }
    const double rounded = std::floor(rate + 0.5);
    const double lower = static_cast<double>(min_rate);
    if (rounded <= lower) {
        return min_rate;
    }
    return static_cast<std::uint64_t>(rounded);
}

/// 归一化配置：0/NaN/越界值替换为默认值，保证状态机不会除零或反向增长。
void NormalizeConfig(BweGccConfig* config) noexcept {
    if (!(config->send_time_group_ms > 0.0)) {
        config->send_time_group_ms = 5.0;
    }
    if (config->trend_window < 2) {
        config->trend_window = 2;
    }
    if (config->trend_window > kMaxTrendWindow) {
        config->trend_window = kMaxTrendWindow;
    }
    if (!(config->smoothing_factor >= 0.0) || !(config->smoothing_factor < 1.0)) {
        config->smoothing_factor = 0.9;
    }
    if (!(config->threshold_gain > 0.0)) {
        config->threshold_gain = 4.0;
    }
    if (!(config->initial_threshold > 0.0)) {
        config->initial_threshold = 12.5;
    }
    if (!(config->min_threshold > 0.0)) {
        config->min_threshold = 6.0;
    }
    if (config->max_threshold < config->min_threshold) {
        config->max_threshold = config->min_threshold;
    }
    // 初始阈值必须落在自适应区间内，否则第一次更新会立刻跳变。
    config->initial_threshold = std::min(
        std::max(config->initial_threshold, config->min_threshold), config->max_threshold);
    if (!(config->threshold_up_per_sec >= 0.0)) {
        config->threshold_up_per_sec = 0.0087;
    }
    if (!(config->threshold_down_per_sec >= 0.0)) {
        config->threshold_down_per_sec = 0.039;
    }
    if (config->overuse_count_threshold == 0) {
        config->overuse_count_threshold = 2;
    }
    if (config->overuse_time_threshold_ms == 0) {
        config->overuse_time_threshold_ms = 10;
    }
    if (config->min_bitrate_bytes_per_sec == 0) {
        config->min_bitrate_bytes_per_sec = 12500;
    }
    if (config->max_bitrate_bytes_per_sec < config->min_bitrate_bytes_per_sec) {
        config->max_bitrate_bytes_per_sec = config->min_bitrate_bytes_per_sec;
    }
    config->initial_bitrate_bytes_per_sec =
        std::min(std::max(config->initial_bitrate_bytes_per_sec,
                          config->min_bitrate_bytes_per_sec),
                 config->max_bitrate_bytes_per_sec);
    if (!(config->alpha > 1.0)) {
        config->alpha = 1.08;
    }
    if (!(config->beta > 0.0) || !(config->beta < 1.0)) {
        config->beta = 0.85;
    }
    if (config->hold_ms == 0) {
        config->hold_ms = 500;
    }
    // 降速间隔不得小于 hold：否则 hold 期内会再次降速，违背「降后 hold」。
    if (config->decrease_interval_ms < config->hold_ms) {
        config->decrease_interval_ms = config->hold_ms;
    }
    if (config->additive_increase_bytes_per_sec == 0) {
        config->additive_increase_bytes_per_sec = 4000;
    }
}

}  // namespace

const char* GccTrendStateName(GccTrendState state) noexcept {
    switch (state) {
        case GccTrendState::kNormal:
            return "normal";
        case GccTrendState::kOverusing:
            return "overusing";
        case GccTrendState::kUnderusing:
            return "underusing";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// GccTrendlineEstimator
// ---------------------------------------------------------------------------

GccTrendlineEstimator::GccTrendlineEstimator(const BweGccConfig& config) {
    SetConfig(config);
}

void GccTrendlineEstimator::SetConfig(const BweGccConfig& config) {
    m_config = config;
    NormalizeConfig(&m_config);
    Reset();
}

void GccTrendlineEstimator::Reset() {
    m_has_prev = false;
    m_prev_send_ms = 0;
    m_prev_arrival_ms = 0;
    m_last_send_ms = 0;
    m_has_last_send = false;
    m_has_delay_base = false;
    m_accumulated_delay_ms = 0.0;
    m_smoothed_delay_ms = 0.0;
    m_delay_history.clear();
    m_threshold = m_config.initial_threshold;
    m_last_threshold_update_ms = 0;
    m_has_threshold_update = false;
    m_trend = 0.0;
    m_modified_trend = 0.0;
    m_delta_count = 0;
    m_dropped_feedback = 0;
    m_state = GccTrendState::kNormal;
    m_overuse_active = false;
    m_overuse_start_ms = 0;
    m_overuse_count = 0;
    m_underuse_active = false;
    m_underuse_start_ms = 0;
    m_underuse_count = 0;
}

GccTrendlineSample GccTrendlineEstimator::Update(std::uint64_t send_ms,
                                                 std::uint64_t arrival_ms) {
    GccTrendlineSample sample;
    sample.threshold = m_threshold;
    sample.state = m_state;
    sample.now_ms = arrival_ms;

    // step1: 拒绝非法/乱序时间戳。乱序反馈会让 delta 变负并伪造「延迟下降」，
    //        直接丢弃比修正更安全；到达早于发送在真实链路里只可能来自坏时钟。
    if (arrival_ms < send_ms) {
        ++m_dropped_feedback;
        return sample;
    }
    if (m_has_last_send && send_ms < m_last_send_ms) {
        ++m_dropped_feedback;
        return sample;
    }
    m_last_send_ms = send_ms;
    m_has_last_send = true;

    // step2: 首包只建立基准，不产生 delta（没有前一组可比较）。
    if (!m_has_prev) {
        m_prev_send_ms = send_ms;
        m_prev_arrival_ms = arrival_ms;
        m_has_prev = true;
        return sample;
    }

    // step3: 5ms 发送时间窗内的反馈并入同一组，只推进基准。这样同一发送
    //        时刻的多个包不会被各自的到达抖动放大成多个正 delta。
    const std::uint64_t send_gap_ms = send_ms - m_prev_send_ms;
    if (static_cast<double>(send_gap_ms) < m_config.send_time_group_ms) {
        m_prev_send_ms = send_ms;
        m_prev_arrival_ms = arrival_ms;
        return sample;
    }

    const std::uint64_t send_delta_ms = send_gap_ms;
    const std::uint64_t arrival_delta_ms =
        arrival_ms >= m_prev_arrival_ms ? arrival_ms - m_prev_arrival_ms : 0;
    m_prev_send_ms = send_ms;
    m_prev_arrival_ms = arrival_ms;
    if (arrival_delta_ms == 0) {
        return sample;
    }

    // step4: 累计延迟 = Σ((arrival_delta − send_delta))。它是「排队时间」的
    //        一阶积分：链路稳定时为常数，队列增长时线性上升。
    const double delta_ms = static_cast<double>(arrival_delta_ms) -
                            static_cast<double>(send_delta_ms);
    if (!m_has_delay_base) {
        m_accumulated_delay_ms = 0.0;
        m_smoothed_delay_ms = 0.0;
        m_has_delay_base = true;
    }
    m_accumulated_delay_ms += delta_ms;
    const double smoothing = m_config.smoothing_factor;
    m_smoothed_delay_ms =
        smoothing * m_smoothed_delay_ms + (1.0 - smoothing) * m_accumulated_delay_ms;
    ++m_delta_count;

    // step5: 滑窗保存 (arrival, smoothed_delay)；窗口未满时不回归，避免用
    //        两三个点拟合出巨大斜率造成启动误判。
    m_delay_history.emplace_back(static_cast<double>(arrival_ms), m_smoothed_delay_ms);
    while (m_delay_history.size() > m_config.trend_window) {
        m_delay_history.pop_front();
    }
    if (m_delay_history.size() < m_config.trend_window) {
        return sample;
    }

    // step6: 线性回归 + 放大 + 自适应阈值 + 三态判定。
    m_trend = FitSlope();
    const double count = static_cast<double>(std::min<std::uint64_t>(m_delta_count, 60));
    m_modified_trend = count * m_trend * m_config.threshold_gain;
    UpdateThreshold(m_modified_trend, arrival_ms);

    const bool overuse = m_modified_trend > m_threshold;
    const bool underuse = m_modified_trend < -m_threshold;
    UpdateHypothesis(overuse, underuse, arrival_ms);

    sample.valid = true;
    sample.trend = m_trend;
    sample.modified_trend = m_modified_trend;
    sample.threshold = m_threshold;
    sample.state = m_state;
    return sample;
}

void GccTrendlineEstimator::UpdateThreshold(double modified_trend, std::uint64_t now_ms) {
    if (!m_has_threshold_update) {
        m_last_threshold_update_ms = now_ms;
        m_has_threshold_update = true;
        return;
    }
    if (now_ms < m_last_threshold_update_ms) {
        now_ms = m_last_threshold_update_ms;
    }
    // 阈值按秒缩放（与 WebRTC 的 k_up/k_down 同量纲），单步最多 1 秒，
    // 防止长时间无反馈后一次跳变到上限。
    const double elapsed_s =
        std::min(static_cast<double>(now_ms - m_last_threshold_update_ms) / 1000.0, 1.0);
    m_last_threshold_update_ms = now_ms;
    const double gain = m_state == GccTrendState::kOverusing ? m_config.threshold_down_per_sec
                                                             : m_config.threshold_up_per_sec;
    m_threshold += gain * (std::fabs(modified_trend) - m_threshold) * elapsed_s;
    m_threshold =
        std::min(std::max(m_threshold, m_config.min_threshold), m_config.max_threshold);
}

double GccTrendlineEstimator::FitSlope() const noexcept {
    const std::size_t count = m_delay_history.size();
    if (count < 2) {
        return 0.0;
    }
    // x 用相对第一个样本的偏移，避免 arrival_ms 达到 1e9 时平方和丢精度。
    const double x_base = m_delay_history.front().first;
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    for (const auto& point : m_delay_history) {
        const double x = point.first - x_base;
        const double y = point.second;
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }
    const double n = static_cast<double>(count);
    const double denominator = n * sum_xx - sum_x * sum_x;
    if (std::fabs(denominator) < 1e-9) {
        return 0.0;
    }
    return (n * sum_xy - sum_x * sum_y) / denominator;
}

void GccTrendlineEstimator::UpdateHypothesis(bool overuse, bool underuse,
                                             std::uint64_t now_ms) {
    if (overuse) {
        // step1: 「连续 N 次 + 持续 ≥ 阈值时长」双条件，避免单个尖峰误判。
        if (!m_overuse_active) {
            m_overuse_active = true;
            m_overuse_start_ms = now_ms;
            m_overuse_count = 0;
        }
        if (now_ms >= m_overuse_start_ms) {
            ++m_overuse_count;
            if (m_overuse_count >= m_config.overuse_count_threshold &&
                now_ms - m_overuse_start_ms >= m_config.overuse_time_threshold_ms) {
                m_state = GccTrendState::kOverusing;
            }
        }
        m_underuse_active = false;
        m_underuse_start_ms = 0;
        m_underuse_count = 0;
        return;
    }
    if (underuse) {
        // step2: underuse 用同一套双条件；它不降速，只用于日志与「正常增长」。
        if (!m_underuse_active) {
            m_underuse_active = true;
            m_underuse_start_ms = now_ms;
            m_underuse_count = 0;
        }
        if (now_ms >= m_underuse_start_ms) {
            ++m_underuse_count;
            if (m_underuse_count >= m_config.overuse_count_threshold &&
                now_ms - m_underuse_start_ms >= m_config.overuse_time_threshold_ms) {
                m_state = GccTrendState::kUnderusing;
            }
        }
        m_overuse_active = false;
        m_overuse_start_ms = 0;
        m_overuse_count = 0;
        return;
    }
    // step3: 回到 normal 必须清空两个计数器，否则「断续 overuse」会被累计
    //        成持续过载，导致阈值永远不回落、速率长期不增。
    m_overuse_active = false;
    m_overuse_start_ms = 0;
    m_overuse_count = 0;
    m_underuse_active = false;
    m_underuse_start_ms = 0;
    m_underuse_count = 0;
    m_state = GccTrendState::kNormal;
}

// ---------------------------------------------------------------------------
// GccAimdRateController
// ---------------------------------------------------------------------------

GccAimdRateController::GccAimdRateController(const BweGccConfig& config) {
    SetConfig(config);
}

void GccAimdRateController::SetConfig(const BweGccConfig& config) {
    m_config = config;
    NormalizeConfig(&m_config);
    Reset();
}

void GccAimdRateController::Reset() {
    m_rate = static_cast<double>(m_config.initial_bitrate_bytes_per_sec);
    m_has_last_update = false;
    m_last_update_ms = 0;
    m_hold_until_ms = 0;
    m_next_decrease_ms = 0;
    m_increase_count = 0;
    m_decrease_count = 0;
    m_hold_block_count = 0;
}

void GccAimdRateController::OnState(GccTrendState state, std::uint64_t now_ms) {
    // step1: 首个状态样本只建立时间基准；时间回退（调用方时钟跳变）按上次
    //        时刻处理，避免 dt 为负导致 pow(alpha, 负值) 反而降速。
    if (!m_has_last_update) {
        m_last_update_ms = now_ms;
        m_has_last_update = true;
        return;
    }
    if (now_ms < m_last_update_ms) {
        now_ms = m_last_update_ms;
    }

    // step2: 过载：只有距上次降速 ≥ decrease_interval 才降一次。hold 期内
    //        既不增长也不重复降速；但要把 last_update 推到 now，避免 hold
    //        结束后一次性补涨「欠账」。
    if (state == GccTrendState::kOverusing) {
        if (now_ms < m_next_decrease_ms) {
            m_last_update_ms = now_ms;
            return;
        }
        Decrease(now_ms);
        return;
    }

    // step3: 非过载但仍在 hold 期：记录被挡住的增长，速率保持不变。
    if (now_ms < m_hold_until_ms) {
        ++m_hold_block_count;
        m_last_update_ms = now_ms;
        return;
    }

    // step4: 正常/欠载：按 α 的乘性增或固定步长的加性增恢复。
    Increase(now_ms);
}

void GccAimdRateController::Decrease(std::uint64_t now_ms) {
    m_rate *= m_config.beta;
    const double floor_rate = static_cast<double>(m_config.min_bitrate_bytes_per_sec);
    if (m_rate < floor_rate) {
        m_rate = floor_rate;
    }
    ++m_decrease_count;
    m_hold_until_ms = now_ms + m_config.hold_ms;
    m_next_decrease_ms = now_ms + m_config.decrease_interval_ms;
    m_last_update_ms = now_ms;
}

void GccAimdRateController::Increase(std::uint64_t now_ms) {
    if (now_ms <= m_last_update_ms) {
        return;
    }
    const double elapsed_s = static_cast<double>(now_ms - m_last_update_ms) / 1000.0;
    if (m_config.increase_mode == GccIncreaseMode::kAdditive) {
        m_rate += static_cast<double>(m_config.additive_increase_bytes_per_sec) * elapsed_s;
    } else {
        // 每秒 ×α：α=1.08 即「每秒 +8%」，与 WebRTC kIncreaseFactor 同义。
        m_rate *= std::pow(m_config.alpha, elapsed_s);
    }
    const double upper = static_cast<double>(m_config.max_bitrate_bytes_per_sec);
    if (m_rate > upper) {
        m_rate = upper;
    }
    ++m_increase_count;
    m_last_update_ms = now_ms;
}

std::uint64_t GccAimdRateController::ClampRate(double rate) const noexcept {
    return SaturateRate(rate, m_config.min_bitrate_bytes_per_sec,
                        m_config.max_bitrate_bytes_per_sec);
}

std::uint64_t GccAimdRateController::TargetBitrate() const noexcept {
    return ClampRate(m_rate);
}

// ---------------------------------------------------------------------------
// BweGcc
// ---------------------------------------------------------------------------

BweGcc::BweGcc(const BweGccConfig& config) {
    SetConfig(config);
}

void BweGcc::SetConfig(const BweGccConfig& config) {
    m_config = config;
    NormalizeConfig(&m_config);
    m_trendline.SetConfig(m_config);
    m_aimd.SetConfig(m_config);
    m_stats = BweGccStats();
    m_stats.last_threshold = m_config.initial_threshold;
    m_last_state = GccTrendState::kNormal;
}

void BweGcc::OnPacketFeedback(std::uint64_t send_ms, std::uint64_t arrival_ms,
                              std::size_t payload_bytes) {
    ++m_stats.feedback_count;
    m_stats.bytes_reported += payload_bytes;

    const GccTrendlineSample sample = m_trendline.Update(send_ms, arrival_ms);
    m_stats.dropped_feedback = m_trendline.dropped_feedback();
    if (!sample.valid) {
        return;
    }

    ++m_stats.trend_samples;
    if (sample.state != m_last_state) {
        if (sample.state == GccTrendState::kOverusing) {
            ++m_stats.overuse_events;
        } else if (sample.state == GccTrendState::kUnderusing) {
            ++m_stats.underuse_events;
        }
        m_last_state = sample.state;
    }

    m_aimd.OnState(sample.state, arrival_ms);
    m_stats.decreases = m_aimd.decrease_count();
    m_stats.increases = m_aimd.increase_count();
    m_stats.hold_blocks = m_aimd.hold_block_count();
    m_stats.last_trend = sample.trend;
    m_stats.last_modified_trend = sample.modified_trend;
    m_stats.last_threshold = sample.threshold;
}

std::uint64_t BweGcc::TargetBitrate() const noexcept {
    return m_aimd.TargetBitrate();
}

void BweGcc::Reset() {
    m_trendline.Reset();
    m_aimd.Reset();
    m_stats = BweGccStats();
    m_stats.last_threshold = m_trendline.threshold();
    m_last_state = GccTrendState::kNormal;
}

}  // namespace go2cpp::bento
