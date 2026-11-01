/**
 * @file congestion.cpp
 * @brief Bento 拥塞控制实现：RTT/RTO、带宽估计、令牌桶 pacing、BBR 与 NewReno。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/bento/net/congestion.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace go2cpp::bento {
namespace {

/// 把 double 增益换算成包数/字节时统一做饱和转换，避免负值或极值溢出。
std::size_t SaturatePackets(double packets, std::size_t minimum, std::size_t maximum) {
    if (!(packets > 0.0)) {  // 同时挡住 NaN
        return minimum;
    }
    if (packets >= static_cast<double>(maximum)) {
        return maximum;
    }
    const std::size_t rounded = static_cast<std::size_t>(packets + 0.5);
    return std::max(rounded, minimum);
}

std::uint64_t SaturateU64(double value) {
    if (!(value > 0.0)) {
        return 0;
    }
    const double limit = static_cast<double>(std::numeric_limits<std::uint64_t>::max());
    if (value >= limit) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(value);
}

}  // namespace

const char* CongestionAlgorithmName(CongestionAlgorithm algorithm) noexcept {
    switch (algorithm) {
        case CongestionAlgorithm::kNone:
            return "none";
        case CongestionAlgorithm::kNewReno:
            return "newreno";
        case CongestionAlgorithm::kBbr:
            return "bbr";
    }
    return "unknown";
}

const char* BbrStateName(BbrState state) noexcept {
    switch (state) {
        case BbrState::kStartup:
            return "STARTUP";
        case BbrState::kDrain:
            return "DRAIN";
        case BbrState::kProbeBw:
            return "PROBE_BW";
        case BbrState::kProbeRtt:
            return "PROBE_RTT";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// BbrRttEstimator
// ---------------------------------------------------------------------------

BbrRttEstimator::BbrRttEstimator(std::uint32_t initial_rtt_ms)
    : m_initial_rtt_ms(initial_rtt_ms == 0 ? 100 : initial_rtt_ms),
      m_srtt_ms(m_initial_rtt_ms),
      m_rttvar_ms(m_initial_rtt_ms / 2),
      m_latest_rtt_ms(m_initial_rtt_ms),
      // min_rtt 必须从 0 开始：如果预填初始值（如 100ms），首个真实样本
      // 是 200ms 时 min_rtt 会永远停在 100ms，BDP 与 RTO 下限全部偏小。
      m_min_rtt_ms(0) {}

void BbrRttEstimator::AddSample(std::uint32_t rtt_ms, std::uint64_t now_ms) {
    if (rtt_ms == 0) {
        // 0ms 只可能来自同一毫秒内的测量，按 1ms 记录，避免 min_rtt 变成 0
        // 导致 BDP=0、pacing 除零。
        rtt_ms = 1;
    }
    if (!m_has_sample) {
        m_srtt_ms = rtt_ms;
        m_rttvar_ms = std::max<std::uint32_t>(rtt_ms / 2, 1);
        m_has_sample = true;
    } else {
        // RFC 6298：RTTVAR = 3/4×RTTVAR + 1/4×|SRTT - R|；SRTT = 7/8×SRTT + 1/8×R。
        const std::uint32_t deviation = m_srtt_ms > rtt_ms ? m_srtt_ms - rtt_ms : rtt_ms - m_srtt_ms;
        m_rttvar_ms = (3 * m_rttvar_ms + deviation) / 4;
        if (m_rttvar_ms == 0) {
            m_rttvar_ms = 1;
        }
        m_srtt_ms = (7 * m_srtt_ms + rtt_ms) / 8;
        if (m_srtt_ms == 0) {
            m_srtt_ms = 1;
        }
    }
    m_latest_rtt_ms = rtt_ms;
    if (m_min_rtt_ms == 0 || rtt_ms < m_min_rtt_ms) {
        m_min_rtt_ms = rtt_ms;
    }
    m_min_samples.emplace_back(now_ms, rtt_ms);
    if (m_min_samples.size() > kMaxSamples) {
        // 只有被淘汰的恰好是当前最小值时才需要 O(n) 重算；否则 min_rtt
        // 仍然有效。压测（千万级 ACK）下这一步决定了每包常数开销。
        const std::uint32_t removed_rtt = m_min_samples.front().second;
        m_min_samples.pop_front();
        if (removed_rtt == m_min_rtt_ms) {
            RecomputeMinRtt();
        }
    }
}

void BbrRttEstimator::OnTimer(std::uint64_t now_ms) {
    while (!m_min_samples.empty() &&
           now_ms >= m_min_samples.front().first &&
           now_ms - m_min_samples.front().first > kMinRttWindowMs) {
        const std::uint32_t removed_rtt = m_min_samples.front().second;
        m_min_samples.pop_front();
        if (removed_rtt == m_min_rtt_ms) {
            RecomputeMinRtt();
        }
    }
}

void BbrRttEstimator::RecomputeMinRtt() noexcept {
    if (m_min_samples.empty()) {
        return;
    }
    std::uint32_t minimum = m_min_samples.front().second;
    for (const auto& sample : m_min_samples) {
        minimum = std::min(minimum, sample.second);
    }
    m_min_rtt_ms = minimum;
}

std::uint32_t BbrRttEstimator::RtoMs(std::uint32_t min_rto_ms,
                                  std::uint32_t max_rto_ms) const noexcept {
    if (min_rto_ms == 0) {
        min_rto_ms = 1;
    }
    if (max_rto_ms < min_rto_ms) {
        max_rto_ms = min_rto_ms;
    }
    if (!m_has_sample) {
        // 初始 RTO 必须明显大于首个 RTT，否则第一个样本到达前包就被
        // 重传，Karn 过滤掉全部 RTT 样本，后续估计全部失准。
        const std::uint32_t fallback =
            std::max<std::uint32_t>(m_initial_rtt_ms * 6, min_rto_ms);
        return std::min(fallback, max_rto_ms);
    }
    const std::uint64_t rto = static_cast<std::uint64_t>(m_srtt_ms) + 4ULL * m_rttvar_ms;
    const std::uint64_t clamped =
        std::min<std::uint64_t>(std::max<std::uint64_t>(rto, min_rto_ms), max_rto_ms);
    return static_cast<std::uint32_t>(clamped);
}

// ---------------------------------------------------------------------------
// BbrBandwidthEstimator
// ---------------------------------------------------------------------------

BbrBandwidthEstimator::BbrBandwidthEstimator(std::size_t window)
    : m_window_ms(static_cast<std::uint64_t>(std::max<std::size_t>(window, 1)) * 500ULL) {}

void BbrBandwidthEstimator::AddSample(std::uint64_t delivered_bytes, std::uint64_t interval_ms,
                                   std::uint64_t now_ms) {
    if (delivered_bytes == 0 || interval_ms == 0) {
        return;
    }
    // 速率 = 字节×1000/毫秒；interval 很小时（同毫秒 ACK）会产生虚高样本，
    // 这里按 1ms 计，配合滑窗 max-filter 会略微高估，但不会失控。
    const std::uint64_t rate = delivered_bytes * 1000ULL / std::max<std::uint64_t>(interval_ms, 1);
    m_latest_bps = rate;
    DropOldSamples(now_ms);
    m_window.emplace_back(now_ms, rate);
    m_max_bps = std::max(m_max_bps, rate);
    while (m_window.size() > kMaxSamples) {
        if (m_window.front().second >= m_max_bps) {
            m_window.pop_front();
            RecomputeMax();
        } else {
            m_window.pop_front();
        }
    }
}

void BbrBandwidthEstimator::DropOldSamples(std::uint64_t now_ms) noexcept {
    while (!m_window.empty() && now_ms >= m_window.front().first &&
           now_ms - m_window.front().first > m_window_ms) {
        if (m_window.front().second >= m_max_bps) {
            m_window.pop_front();
            RecomputeMax();
        } else {
            m_window.pop_front();
        }
    }
}

void BbrBandwidthEstimator::RecomputeMax() noexcept {
    m_max_bps = 0;
    for (const auto& sample : m_window) {
        m_max_bps = std::max(m_max_bps, sample.second);
    }
}

void BbrBandwidthEstimator::Reset() noexcept {
    m_window.clear();
    m_latest_bps = 0;
    m_max_bps = 0;
}

// ---------------------------------------------------------------------------
// Pacer
// ---------------------------------------------------------------------------

void Pacer::Configure(std::uint32_t mss_bytes, std::uint32_t burst_ms, bool enabled) {
    m_mss_bytes = mss_bytes == 0 ? 1200 : mss_bytes;
    m_burst_ms = burst_ms == 0 ? 5 : burst_ms;
    m_enabled = enabled;
    m_capacity = static_cast<std::uint64_t>(m_mss_bytes) * 2ULL;
    m_tokens = std::min(m_tokens, m_capacity);
}

void Pacer::SetRate(std::uint64_t bytes_per_sec, std::uint64_t now_ms) {
    if (m_initialized) {
        Refill(now_ms);
    }
    m_rate_bps = bytes_per_sec;
    const std::uint64_t burst_capacity =
        m_rate_bps * static_cast<std::uint64_t>(m_burst_ms) / 1000ULL;
    m_capacity = std::max<std::uint64_t>(static_cast<std::uint64_t>(m_mss_bytes) * 2ULL,
                                         burst_capacity);
    m_tokens = std::min(m_tokens, m_capacity);
    if (!m_initialized) {
        m_initialized = true;
        m_last_refill_ms = now_ms;
        // 冷启动给一个突发额度，避免第一帧要等一个 burst 周期。
        m_tokens = m_capacity;
    }
}

void Pacer::OnTick(std::uint64_t now_ms) {
    Refill(now_ms);
}

void Pacer::Refill(std::uint64_t now_ms) noexcept {
    if (!m_initialized) {
        m_initialized = true;
        m_last_refill_ms = now_ms;
        m_tokens = m_capacity;
        return;
    }
    if (now_ms <= m_last_refill_ms) {
        m_last_refill_ms = now_ms;
        return;
    }
    const std::uint64_t elapsed = now_ms - m_last_refill_ms;
    m_last_refill_ms = now_ms;
    // 上限保护：elapsed 异常大（进程暂停）时不一次性补满。
    const std::uint64_t added = m_rate_bps * elapsed / 1000ULL;
    m_tokens = std::min<std::uint64_t>(m_capacity, m_tokens + added);
}

bool Pacer::Consume(std::size_t bytes) {
    if (!m_enabled) {
        return true;
    }
    if (m_tokens >= static_cast<std::uint64_t>(bytes)) {
        m_tokens -= static_cast<std::uint64_t>(bytes);
        return true;
    }
    return false;
}

std::size_t Pacer::available_bytes() const noexcept {
    return static_cast<std::size_t>(m_tokens);
}

// ---------------------------------------------------------------------------
// CongestionController
// ---------------------------------------------------------------------------

CongestionController::CongestionController(const CongestionConfig& config) : m_config(config) {
    NormalizeConfig();

    m_cwnd_packets = m_config.initial_cwnd_packets;
    m_startup_cwnd_target = m_cwnd_packets;
    m_ssthresh_packets = std::max<std::size_t>(m_config.initial_cwnd_packets * 2, 128);
    m_reno_ssthresh = m_ssthresh_packets;
    m_pacer.Configure(m_config.mss_bytes, m_config.pacing_burst_ms, m_config.pacing_enabled);
    m_pacer.SetRate(InitialPacingRate(), 0);
    m_pacing_bps = InitialPacingRate();
    m_round_start_ms = 0;
    m_last_probe_rtt_ms = 0;
    m_bbr_state = BbrState::kStartup;
}

std::uint64_t CongestionController::InitialPacingRate() const noexcept {
    const std::uint64_t rtt = std::max<std::uint32_t>(m_config.initial_rtt_ms, 1);
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(m_config.initial_cwnd_packets) * m_config.mss_bytes;
    return std::max<std::uint64_t>(bytes * 1000ULL / rtt,
                                   m_config.min_pacing_bytes_per_sec);
}

void CongestionController::Reset(std::uint64_t now_ms) {
    m_rtt = BbrRttEstimator(m_config.initial_rtt_ms);
    m_bandwidth.Reset();
    m_cwnd_packets = m_config.initial_cwnd_packets;
    m_startup_cwnd_target = m_cwnd_packets;
    m_ssthresh_packets = std::max<std::size_t>(m_config.initial_cwnd_packets * 2, 128);
    m_reno_ssthresh = m_ssthresh_packets;
    m_inflight_hi_packets = 0;
    m_has_inflight_hi = false;
    m_bbr_state = BbrState::kStartup;
    m_round_start_ms = now_ms;
    m_rounds = 0;
    m_bw_at_round_start = 0;
    m_full_bw_rounds = 0;
    m_cycle_index = 0;
    m_probe_rtt_active = false;
    m_last_probe_rtt_ms = now_ms;
    m_round_bytes_sent = 0;
    m_round_bytes_acked = 0;
    m_pacing_bps = InitialPacingRate();
    m_pacer = Pacer();
    m_pacer.Configure(m_config.mss_bytes, m_config.pacing_burst_ms, m_config.pacing_enabled);
    m_pacer.SetRate(m_pacing_bps, now_ms);
    m_stats = CongestionStats();
    m_stats_dirty = true;
}

void CongestionController::OnAck(const AckSample& sample) {
    ++m_stats.ack_events;
    m_stats_dirty = true;
    if (!sample.retransmitted && sample.rtt_ms > 0) {
        m_rtt.AddSample(sample.rtt_ms, sample.now_ms);
    }
    if (sample.delivered_bytes > 0) {
        m_bandwidth.AddSample(sample.delivered_bytes, sample.send_elapsed_ms, sample.now_ms);
        m_round_bytes_acked += sample.delivered_bytes;
    }
    if (m_config.algorithm == CongestionAlgorithm::kNone) {
        return;
    }
    if (m_config.algorithm == CongestionAlgorithm::kNewReno) {
        RenoOnAck(sample);
        return;
    }
    BbrOnAck(sample);
}

void CongestionController::OnLoss(const LossSample& sample) {
    m_stats_dirty = true;
    ++m_stats.loss_events;
    if (m_config.algorithm == CongestionAlgorithm::kNone) {
        return;
    }
    if (m_config.algorithm == CongestionAlgorithm::kNewReno) {
        RenoOnLoss(sample, false);
        return;
    }
    BbrOnLoss(sample, false);
}

void CongestionController::OnTimeout(const LossSample& sample) {
    m_stats_dirty = true;
    ++m_stats.loss_events;
    ++m_stats.timeout_events;
    if (m_config.algorithm == CongestionAlgorithm::kNone) {
        return;
    }
    if (m_config.algorithm == CongestionAlgorithm::kNewReno) {
        RenoOnLoss(sample, true);
        return;
    }
    BbrOnLoss(sample, true);
}

void CongestionController::OnSend(std::size_t bytes, std::uint64_t now_ms) {
    ++m_stats.packets_sent;
    m_stats.bytes_sent += bytes;
    m_round_bytes_sent += bytes;
    if (m_round_start_ms == 0) {
        m_round_start_ms = now_ms;
    }
    m_stats_dirty = true;
}

void CongestionController::OnTick(std::uint64_t now_ms) {
    if (m_config.algorithm == CongestionAlgorithm::kNone) {
        return;
    }
    // ProbeRTT 的进入/退出由 tick 驱动，避免链路上没有 ACK 时状态卡死。
    if (m_config.algorithm == CongestionAlgorithm::kBbr) {
        if (m_last_probe_rtt_ms == 0) {
            m_last_probe_rtt_ms = now_ms;
        }
        if (m_probe_rtt_active) {
            if (now_ms >= m_probe_rtt_entered_ms &&
                now_ms - m_probe_rtt_entered_ms >= m_config.probe_rtt_duration_ms) {
                BbrExitProbeRtt(now_ms);
            }
        } else if (m_last_probe_rtt_ms != 0 && now_ms >= m_last_probe_rtt_ms &&
                   now_ms - m_last_probe_rtt_ms >= m_config.probe_rtt_interval_ms) {
            BbrEnterProbeRtt(now_ms);
        }
    }
    m_pacer.OnTick(now_ms);
    m_rtt.OnTimer(now_ms);
}

bool CongestionController::CanSend(std::size_t inflight_bytes) const noexcept {
    if (m_config.algorithm == CongestionAlgorithm::kNone) {
        return true;
    }
    const std::size_t cwnd_bytes = m_cwnd_packets * m_config.mss_bytes;
    return inflight_bytes < cwnd_bytes;
}

bool CongestionController::ConsumePacing(std::size_t bytes) {
    if (m_config.algorithm == CongestionAlgorithm::kNone || !m_config.pacing_enabled) {
        return true;
    }
    if (m_pacer.Consume(bytes)) {
        return true;
    }
    ++m_stats.pacing_limited;
    m_stats_dirty = true;
    return false;
}

std::uint32_t CongestionController::RtoMs() const noexcept {
    std::uint32_t rto = m_rtt.RtoMs(m_config.min_rto_ms, m_config.max_rto_ms);
    const std::uint32_t min_rtt =
        m_rtt.min_rtt_ms() != 0 ? m_rtt.min_rtt_ms() : m_config.initial_rtt_ms;
    // RTO 至少覆盖 2×min_rtt：排队延迟会推高 SRTT，但 min_rtt 是链路
    // 的真实传播时延，用它兜底可以避免「一次排队 = 一次超时」。
    const std::uint64_t floor =
        std::min<std::uint64_t>(static_cast<std::uint64_t>(min_rtt) * 2ULL,
                                m_config.max_rto_ms);
    rto = std::max<std::uint32_t>(rto, static_cast<std::uint32_t>(floor));
    return rto;
}

// ---------------------------------------------------------------------------
// BBR
// ---------------------------------------------------------------------------

void CongestionController::BbrOnAck(const AckSample& sample) {
    // 轮次推进用时间近似（BBR 用「被 ACK 的包所属的包发送轮次」精确判定）。
    // 弱网 RTT 稳定时两者等价；时间近似在 ACK 聚合下略偏保守，可接受。
    BbrAdvanceRound(sample.now_ms);
    BbrUpdateModel(sample.now_ms);
}

void CongestionController::BbrAdvanceRound(std::uint64_t now_ms) {
    if (m_round_start_ms == 0) {
        m_round_start_ms = now_ms;
        return;
    }
    const std::uint32_t min_rtt =
        std::max<std::uint32_t>(m_rtt.min_rtt_ms(), 1);
    if (now_ms < m_round_start_ms || now_ms - m_round_start_ms < min_rtt) {
        return;
    }
    m_round_start_ms = now_ms;
    ++m_rounds;
    ++m_stats.rounds;

    const std::uint64_t bandwidth = m_bandwidth.max_bytes_per_sec();
    switch (m_bbr_state) {
        case BbrState::kStartup: {
            if (m_rounds >= m_config.startup_max_rounds ||
                m_full_bw_rounds >= m_config.startup_full_bw_rounds) {
                m_bbr_state = BbrState::kDrain;
                ++m_stats.startup_exits;
                break;
            }
            if (m_bw_at_round_start > 0 && bandwidth > 0 &&
                static_cast<double>(bandwidth) <=
                    static_cast<double>(m_bw_at_round_start) *
                        (1.0 + m_config.startup_growth_threshold)) {
                ++m_full_bw_rounds;
            } else {
                m_full_bw_rounds = 0;
            }
            if (bandwidth > 0) {
                m_bw_at_round_start = bandwidth;
            }
            // startup 的窗口每轮 ×1.25（约等于 2/ln2 增益的离散化），
            // 与带宽估计解耦，避免自锁。
            m_startup_cwnd_target = std::min<std::size_t>(
                m_config.max_cwnd_packets,
                m_startup_cwnd_target + std::max<std::size_t>(1, m_startup_cwnd_target / 4));
            break;
        }
        case BbrState::kDrain: {
            // 简化：drain 只持续一个轮次。BBR 要求 inflight 降到 BDP 以下，
            // 这里由 cwnd=DRAIN_GAIN×BDP 的窗口在下一轮自然做到。
            m_bbr_state = BbrState::kProbeBw;
            m_cycle_index = 0;
            break;
        }
        case BbrState::kProbeBw: {
            m_cycle_index =
                (m_cycle_index + 1) % std::max<std::uint32_t>(m_config.probe_bw_cycle_rounds, 1);
            ++m_stats.probe_bw_phases;
            // BBR 的 inflight_hi 不能只降不升：丢包时收缩后，在每个 probe
            // 轮次按 1/8 逐步探回去，否则一次早期丢包会让窗口永久停留。
            if (m_has_inflight_hi &&
                m_inflight_hi_packets < m_config.max_cwnd_packets) {
                const std::size_t grow =
                    std::max<std::size_t>(1, m_inflight_hi_packets / 4);
                m_inflight_hi_packets = std::min<std::size_t>(
                    m_inflight_hi_packets + grow, m_config.max_cwnd_packets);
            }
            break;
        }
        case BbrState::kProbeRtt:
            break;
    }
    m_bw_at_round_start = bandwidth;
    m_round_bytes_sent = 0;
    m_round_bytes_acked = 0;
}

std::uint64_t CongestionController::BdpBytes() const noexcept {
    const std::uint64_t bandwidth = m_bandwidth.max_bytes_per_sec();
    if (bandwidth == 0) {
        return static_cast<std::uint64_t>(m_config.initial_cwnd_packets) * m_config.mss_bytes;
    }
    std::uint64_t rtt = m_rtt.min_rtt_ms();
    if (rtt == 0) {
        rtt = std::max<std::uint32_t>(m_config.initial_rtt_ms, 1);
    }
    return std::max<std::uint64_t>(bandwidth * rtt / 1000ULL,
                                   static_cast<std::uint64_t>(m_config.min_cwnd_packets) *
                                       m_config.mss_bytes);
}

double CongestionController::BbrPacingGain() const noexcept {
    switch (m_bbr_state) {
        case BbrState::kStartup:
            return m_config.startup_pacing_gain;
        case BbrState::kDrain:
            return m_config.drain_pacing_gain;
        case BbrState::kProbeBw: {
            if (m_cycle_index == 0) {
                return m_config.probe_bw_pacing_gain;
            }
            if (m_cycle_index == 1) {
                return m_config.probe_bw_low_gain;
            }
            return 1.0;
        }
        case BbrState::kProbeRtt:
            return 1.0;
    }
    return 1.0;
}

double CongestionController::BbrCwndGain() const noexcept {
    switch (m_bbr_state) {
        case BbrState::kStartup:
            return m_config.startup_cwnd_gain;
        case BbrState::kDrain:
            return 1.0;
        case BbrState::kProbeBw:
            return m_config.probe_bw_cwnd_gain;
        case BbrState::kProbeRtt:
            return 1.0;
    }
    return 1.0;
}

void CongestionController::BbrUpdateModel(std::uint64_t now_ms) {
    const std::uint64_t bandwidth = m_bandwidth.max_bytes_per_sec();
    const std::uint64_t bdp = BdpBytes();
    const double cwnd_gain = BbrCwndGain();
    double target_packets =
        cwnd_gain * static_cast<double>(bdp) / static_cast<double>(m_config.mss_bytes);

    if (m_probe_rtt_active) {
        target_packets = static_cast<double>(m_config.min_cwnd_packets);
    }
    std::size_t cwnd = SaturatePackets(target_packets, m_config.min_cwnd_packets,
                                       m_config.max_cwnd_packets);
    if (m_bbr_state == BbrState::kStartup && !m_probe_rtt_active) {
        // startup 期间窗口取「BDP 目标」与「独立增长目标」的较大者。
        cwnd = std::max(cwnd, m_startup_cwnd_target);
        cwnd = std::min(cwnd, m_config.max_cwnd_packets);
    }
    if (m_has_inflight_hi && cwnd > m_inflight_hi_packets) {
        cwnd = std::max<std::size_t>(m_inflight_hi_packets, m_config.min_cwnd_packets);
    }
    m_cwnd_packets = cwnd;

    std::uint64_t pacing = SaturateU64(static_cast<double>(bandwidth) * BbrPacingGain());
    if (pacing == 0) {
        pacing = InitialPacingRate();
    }
    if (m_bbr_state == BbrState::kStartup) {
        // startup 的 pacing 不低于「初始窗口 / 当前 min_rtt」：否则第一个
        // ACK 的低带宽样本会把发送速率直接压到下限，探测还没开始就结束。
        // 用真实 min_rtt（而不是配置的 initial_rtt）换算，才不会被 100ms
        // 的假设值放大成 3 倍过载。
        const std::uint32_t rtt =
            m_rtt.min_rtt_ms() != 0 ? m_rtt.min_rtt_ms() : m_config.initial_rtt_ms;
        const std::uint64_t startup_floor =
            static_cast<std::uint64_t>(m_config.initial_cwnd_packets) * m_config.mss_bytes *
            1000ULL / std::max<std::uint32_t>(rtt, 1);
        pacing = std::max<std::uint64_t>(pacing, startup_floor);
    }
    pacing = std::max<std::uint64_t>(pacing, m_config.min_pacing_bytes_per_sec);
    if (pacing != m_pacing_bps) {
        m_pacing_bps = pacing;
        m_pacer.SetRate(pacing, now_ms);
    }
    m_pacer.OnTick(now_ms);
}

void CongestionController::BbrOnLoss(const LossSample& sample, bool timeout) {
    const std::uint64_t bdp = BdpBytes();
    // BBR 的关键性质：随机丢包不降 pacing、不把 cwnd 减半。丢包只说明
    // 「in-flight 可能超过了实际可用队列」，因此只收缩 inflight_hi——而且
    // 只有真的出现「在途超过当前窗口」或整窗超时时才收缩，否则一次孤立
    // 丢包就会把 inflight_hi 永久压低（之后每轮 probe 会逐步恢复）。
    const std::size_t cwnd_bytes = m_cwnd_packets * m_config.mss_bytes;
    // startup 阶段的丢包/超时几乎都是「探测性排队」的副产物，不是拥塞信号：
    // 此时收缩 inflight_hi 会把刚探到的窗口立刻压回去，导致「窗口小 → 发得
    // 慢 → 带宽估计低 → 窗口更小」的自锁（实测吞吐 30kB/s vs 链路 125kB/s）。
    // BBRv1 同样只在 inflight 超过模型时才收紧。
    const bool congestion_signal = !timeout && m_bbr_state != BbrState::kStartup &&
                                   (sample.inflight_bytes > cwnd_bytes || sample.holes >= 3);
    if (congestion_signal) {
        const std::uint64_t shrink = SaturateU64(
            static_cast<double>(sample.inflight_bytes) * m_config.inflight_hi_shrink);
        const std::uint64_t floor = std::max<std::uint64_t>(
            bdp, static_cast<std::uint64_t>(m_config.min_cwnd_packets) * m_config.mss_bytes);
        const std::uint64_t hi_bytes = std::max<std::uint64_t>(shrink, floor);
        const std::size_t hi_packets = SaturatePackets(
            static_cast<double>(hi_bytes) / static_cast<double>(m_config.mss_bytes),
            m_config.min_cwnd_packets, m_config.max_cwnd_packets);
        if (!m_has_inflight_hi || hi_packets < m_inflight_hi_packets) {
            // 收缩下限不低于当前窗口的一半：否则一次测得的低 BDP 会把
            // inflight_hi 压到 BDP 附近，而 cwnd 又被 inflight_hi 限制，
            // 形成「窗口小 → 带宽估计低 → 窗口更小」的正反馈。
            const std::size_t half_cwnd =
                std::max<std::size_t>(1, m_cwnd_packets / 2);
            m_inflight_hi_packets = std::max(hi_packets, half_cwnd);
            m_has_inflight_hi = true;
        }
    }
    // 超时只统计、不改变窗口与 pacing：1Mbps + 200ms 链路上，一个 BDP 的
    // 排队就能超过 1 秒，任何 RTO 都必然超时。BBR 的恢复靠 SACK 快速重传
    // 与带宽估计，而不是 loss-based 的塌窗。startup 的退出只由「连续 3 轮
    // 带宽不增长」决定（BbrAdvanceRound）。
    // 注意：这里刻意**不**在 startup 因「holes>=2」退出。5% 随机丢包下
    // 每个窗口几乎都会出现 2 个空洞，按空洞退出会把 startup 的
    // 2.885× 探测增益永久关掉，带宽估计只能停在低速率自稳定。startup
    // 的退出只由「连续 3 轮带宽不增长」或超时决定（BBRv1 语义）。
    BbrUpdateModel(sample.now_ms);
}
void CongestionController::BbrEnterProbeRtt(std::uint64_t now_ms) {
    m_probe_rtt_active = true;
    m_probe_rtt_entered_ms = now_ms;
    m_last_probe_rtt_ms = now_ms;
    m_bbr_state = BbrState::kProbeRtt;
    ++m_stats.probe_rtt_entries;
    BbrUpdateModel(now_ms);
}

void CongestionController::BbrExitProbeRtt(std::uint64_t now_ms) {
    m_probe_rtt_active = false;
    m_bbr_state = BbrState::kProbeBw;
    m_cycle_index = 0;
    m_last_probe_rtt_ms = now_ms;
    BbrUpdateModel(now_ms);
}

// ---------------------------------------------------------------------------
// NewReno
// ---------------------------------------------------------------------------

void CongestionController::RenoOnAck(const AckSample& sample) {
    const std::uint64_t acked_packets =
        std::max<std::uint64_t>(sample.delivered_bytes / m_config.mss_bytes, 1);
    if (m_cwnd_packets < m_ssthresh_packets) {
        // 慢启动：每确认一个包窗口 +1。
        m_cwnd_packets = std::min<std::size_t>(
            m_cwnd_packets + static_cast<std::size_t>(acked_packets),
            m_config.max_cwnd_packets);
    } else {
        // 拥塞避免：每 RTT 增加约一个 MSS（按 ACK 累积）。
        m_reno_ack_accum += static_cast<double>(acked_packets);
        if (m_reno_ack_accum >= static_cast<double>(m_cwnd_packets)) {
            m_reno_ack_accum -= static_cast<double>(m_cwnd_packets);
            m_cwnd_packets = std::min<std::size_t>(m_cwnd_packets + 1,
                                                   m_config.max_cwnd_packets);
        }
    }
    const std::uint32_t srtt = std::max<std::uint32_t>(m_rtt.srtt_ms(), 1);
    const std::uint64_t pacing = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(m_cwnd_packets) * m_config.mss_bytes * 1000ULL /
            srtt * m_config.reno_pacing_gain,
        m_config.min_pacing_bytes_per_sec);
    if (pacing != m_pacing_bps) {
        m_pacing_bps = pacing;
        m_pacer.SetRate(pacing, sample.now_ms);
    }
    m_pacer.OnTick(sample.now_ms);
    m_ssthresh_packets = m_reno_ssthresh;
}

void CongestionController::RenoOnLoss(const LossSample& sample, bool timeout) {
    // 同一个 SRTT 内只减一次，避免一个 ACK 位图里的多个空洞把窗口连砍。
    const std::uint32_t srtt = std::max<std::uint32_t>(sample.srtt_ms, 1);
    if (m_last_reduction_ms != 0 && sample.now_ms >= m_last_reduction_ms &&
        sample.now_ms - m_last_reduction_ms < srtt) {
        return;
    }
    m_last_reduction_ms = sample.now_ms;
    const std::size_t decreased = std::max<std::size_t>(
        static_cast<std::size_t>(static_cast<double>(m_cwnd_packets) * m_config.reno_decrease),
        m_config.min_cwnd_packets);
    m_reno_ssthresh = std::max<std::size_t>(decreased, m_config.min_cwnd_packets);
    m_ssthresh_packets = m_reno_ssthresh;
    m_cwnd_packets = timeout ? m_config.min_cwnd_packets : m_reno_ssthresh;
    m_reno_ack_accum = 0.0;
    ++m_stats.cwnd_reductions;

    const std::uint32_t denominator = timeout
                                          ? std::max<std::uint32_t>(sample.srtt_ms * 2, 1)
                                          : srtt;
    const std::uint64_t pacing = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(m_cwnd_packets) * m_config.mss_bytes * 1000ULL /
            denominator * m_config.reno_pacing_gain,
        m_config.min_pacing_bytes_per_sec);
    if (pacing != m_pacing_bps) {
        m_pacing_bps = pacing;
        m_pacer.SetRate(pacing, sample.now_ms);
    }
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------

const CongestionStats& CongestionController::stats() const {
    if (m_stats_dirty) {
        RefreshStats();
    }
    return m_stats;
}

void CongestionController::RefreshStats() const {
    m_stats.algorithm = m_config.algorithm;
    m_stats.bbr_state = m_bbr_state;
    m_stats.bandwidth_bps = m_bandwidth.max_bytes_per_sec();
    m_stats.pacing_bps = m_pacing_bps;
    m_stats.cwnd_packets = static_cast<std::uint32_t>(m_cwnd_packets);
    m_stats.ssthresh_packets = static_cast<std::uint32_t>(
        m_config.algorithm == CongestionAlgorithm::kNewReno ? m_reno_ssthresh : m_ssthresh_packets);
    m_stats.inflight_hi_packets =
        m_has_inflight_hi ? static_cast<std::uint32_t>(m_inflight_hi_packets) : 0U;
    m_stats.srtt_ms = m_rtt.srtt_ms();
    m_stats.min_rtt_ms = m_rtt.min_rtt_ms();
    m_stats.rto_ms = RtoMs();
    m_stats.rounds = m_rounds;
    m_stats.min_cwnd_packets = static_cast<std::uint32_t>(m_config.min_cwnd_packets);
    m_stats.pacing_enabled = m_config.pacing_enabled;
    const std::uint64_t acked_packets = m_stats.packets_acked != 0 ? m_stats.packets_acked : 0;
    const std::uint64_t events = m_stats.loss_events;
    if (acked_packets + events > 0) {
        m_stats.loss_rate_permille = static_cast<std::uint32_t>(
            events * 1000ULL / (acked_packets + events));
    }
    m_stats_dirty = false;
}


// ---------------------------------------------------------------------------
// 兼容层：旧控制器接口（包数口径）到 BBR 新接口（字节口径）的桥
// ---------------------------------------------------------------------------

void CongestionController::NormalizeConfig() {
    if (m_config.mss_bytes == 0) {
        m_config.mss_bytes = 1200;
    }
    if (!m_config.enabled) {
        m_config.algorithm = CongestionAlgorithm::kNone;
    }
    if (!m_config.pacing) {
        m_config.pacing_enabled = false;
    }
    m_config.min_cwnd_packets = std::max<std::size_t>(m_config.min_cwnd_packets, 1);
    m_config.max_cwnd_packets =
        std::max(m_config.max_cwnd_packets, m_config.min_cwnd_packets);
    m_config.initial_cwnd_packets =
        std::min(std::max(m_config.initial_cwnd_packets, m_config.min_cwnd_packets),
                 m_config.max_cwnd_packets);
}

void CongestionController::SetConfig(const CongestionConfig& config) {
    m_config = config;
    NormalizeConfig();
    m_pacer.Configure(m_config.mss_bytes, m_config.pacing_burst_ms, m_config.pacing_enabled);
    Reset(0);
}

bool CongestionController::enabled() const noexcept {
    return m_config.algorithm != CongestionAlgorithm::kNone;
}

std::size_t CongestionController::cwnd() const noexcept {
    return m_cwnd_packets;
}

std::size_t CongestionController::ssthresh() const noexcept {
    return m_config.algorithm == CongestionAlgorithm::kNewReno ? m_reno_ssthresh : m_ssthresh_packets;
}

bool CongestionController::slow_start() const noexcept {
    if (m_config.algorithm == CongestionAlgorithm::kBbr) {
        return m_bbr_state == BbrState::kStartup;
    }
    return m_cwnd_packets < m_reno_ssthresh;
}

std::uint32_t CongestionController::srtt_ms() const noexcept {
    return m_rtt.srtt_ms();
}

std::uint64_t CongestionController::pacing_rate_bps() const noexcept {
    return m_config.pacing_enabled ? m_pacing_bps : 0;
}

std::uint32_t CongestionController::min_rtt_ms() const noexcept {
    return m_rtt.min_rtt_ms();
}

bool CongestionController::AllowSend(std::uint64_t now_ms, std::size_t inflight_packets) {
    if (!enabled()) {
        return true;
    }
    // 先按 now 补令牌，避免调用方只在 Pump 时驱动而令牌永远不增长。
    m_pacer.OnTick(now_ms);
    if (!CanSend(inflight_packets * m_config.mss_bytes)) {
        return false;
    }
    if (!m_config.pacing_enabled) {
        return true;
    }
    // 只需「还有令牌」就放行，而不是要求攒够一个 MSS：Bento 的 STREAM/ACK
    // 帧远小于 MSS（152B vs 1200B），按 MSS 门槛会让突发末尾的小帧被饿死
    // （实测 20 条 152B 消息只发出 19 条，inflight 少 1）。实际发送量由
    // OnPacketSent 的 Consume 扣减，Pacer 的容量上限仍然约束突发。
    if (m_pacer.available_bytes() < 1) {
        ++m_stats.pacing_limited;
        m_stats_dirty = true;
        return false;
    }
    return true;
}

void CongestionController::OnPacketSent(std::uint64_t now_ms, std::size_t bytes) {
    OnSend(bytes, now_ms);
    if (enabled() && m_config.pacing_enabled && bytes > 0) {
        (void)m_pacer.Consume(bytes);
    }
}

void CongestionController::OnAck(std::uint32_t acked_packets, std::uint32_t rtt_ms,
                                 std::uint64_t now_ms) {
    if (acked_packets == 0) {
        return;
    }
    m_stats.packets_acked += acked_packets;
    AckSample sample;
    sample.now_ms = now_ms;
    sample.rtt_ms = rtt_ms;
    sample.delivered_bytes = static_cast<std::uint64_t>(acked_packets) * m_config.mss_bytes;
    sample.send_elapsed_ms = rtt_ms != 0 ? rtt_ms : m_config.initial_rtt_ms;
    sample.holes = 0;
    OnAck(sample);
}

void CongestionController::OnLoss(const LossContext& ctx) {
    LossSample sample;
    sample.now_ms = ctx.now_ms;
    sample.holes = ctx.holes;
    sample.inflight_bytes = ctx.inflight * m_config.mss_bytes;
    sample.srtt_ms = ctx.srtt_ms != 0 ? ctx.srtt_ms : m_rtt.srtt_ms();
    sample.min_rtt_ms = m_rtt.min_rtt_ms();
    sample.timeout = false;
    OnLoss(sample);
}

void CongestionController::OnTimeout(const LossContext& ctx) {
    LossSample sample;
    sample.now_ms = ctx.now_ms;
    sample.holes = ctx.holes;
    sample.inflight_bytes = ctx.inflight * m_config.mss_bytes;
    sample.srtt_ms = ctx.srtt_ms != 0 ? ctx.srtt_ms : m_rtt.srtt_ms();
    sample.min_rtt_ms = m_rtt.min_rtt_ms();
    sample.timeout = true;
    OnTimeout(sample);
}

void CongestionController::NoteRetransmit() noexcept {
    // 仅观测用途；BBR 的带宽/窗口模型不需要重传计数（随机丢包不降速）。
}

CongestionStats CongestionController::Snapshot(std::size_t send_window) const {
    const CongestionStats& base = stats();
    CongestionStats snapshot = base;
    const std::size_t effective =
        send_window == 0 ? m_cwnd_packets : std::min(m_cwnd_packets, send_window);
    snapshot.effective_window = static_cast<std::uint32_t>(effective);
    snapshot.pacing_rate_bps = m_config.pacing_enabled ? m_pacing_bps : 0;
    snapshot.pacing_deferrals = base.pacing_limited;
    if (m_config.algorithm == CongestionAlgorithm::kBbr) {
        snapshot.slow_start = m_bbr_state == BbrState::kStartup;
    } else {
        snapshot.slow_start = m_cwnd_packets < m_reno_ssthresh;
    }
    return snapshot;
}

}  // namespace go2cpp::bento
