/**
 * @file congestion.cpp
 * @brief NewReno 拥塞控制 + 随机丢包归因 + 令牌桶 pacing 实现。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include "go2cpp/bento/net/congestion.hpp"

#include <algorithm>

namespace go2cpp::bento {

CongestionController::CongestionController() noexcept : CongestionController(Config{}) {}

CongestionController::CongestionController(Config config) noexcept {
    SetConfig(config);
}

void CongestionController::SetConfig(Config config) noexcept {
    if (config.min_cwnd_packets < 1) {
        config.min_cwnd_packets = 1;
    }
    if (config.initial_cwnd_packets < config.min_cwnd_packets) {
        config.initial_cwnd_packets = config.min_cwnd_packets;
    }
    if (config.max_cwnd_packets < config.initial_cwnd_packets) {
        config.max_cwnd_packets = config.initial_cwnd_packets;
    }
    if (config.initial_ssthresh_packets < config.min_cwnd_packets) {
        config.initial_ssthresh_packets = config.min_cwnd_packets;
    }
    if (config.mss_bytes == 0) {
        config.mss_bytes = 1;
    }
    if (config.initial_rtt_ms < config.min_rtt_ms) {
        config.initial_rtt_ms = config.min_rtt_ms;
    }
    m_config = config;
    Reset(0);
}

void CongestionController::Reset(std::uint64_t now_ms) noexcept {
    m_cwnd_packets = m_config.initial_cwnd_packets;
    m_ssthresh_packets = m_config.initial_ssthresh_packets;
    m_srtt_ms = m_config.initial_rtt_ms;
    m_min_rtt_ms = 0;
    m_has_rtt_sample = false;
    m_slow_start = true;
    m_ca_bytes = 0;
    m_last_refill_ms = now_ms;
    m_last_loss_ms = 0;
    m_loss_recorded = false;
    m_last_loss_was_congestion = false;
    m_loss_rate_permille = 0;
    m_window_acked = 0;
    m_window_lost = 0;
    m_packets_sent = 0;
    m_packets_acked = 0;
    m_bytes_sent = 0;
    m_loss_events = 0;
    m_timeout_events = 0;
    m_random_loss_events = 0;
    m_congestion_loss_events = 0;
    m_pacing_deferrals = 0;
    m_retransmits = 0;
    // 初始桶为满：允许一个 cwnd 的突发，之后严格按 rate 补充。
    m_tokens_bytes = static_cast<std::uint64_t>(m_cwnd_packets) * m_config.mss_bytes;
}

std::uint32_t CongestionController::SaturateCwnd(std::uint32_t value) const noexcept {
    if (value < m_config.min_cwnd_packets) {
        value = m_config.min_cwnd_packets;
    }
    if (value > m_config.max_cwnd_packets) {
        value = m_config.max_cwnd_packets;
    }
    return value;
}

std::uint32_t CongestionController::PacingDenominatorMs() const noexcept {
    const std::uint32_t srtt = std::max<std::uint32_t>(m_srtt_ms, 1);
    if (!m_config.use_min_rtt_for_pacing || !m_has_rtt_sample) {
        return srtt;
    }
    // 丢包会让 Karn 算法丢掉重传样本、把 SRTT 推高；若继续拿 SRTT 当分母，
    // pacing 会随丢包自我衰减（实测 20% 丢包下 SRTT 451ms、pacing 13 kB/s）。
    // min_rtt 是「无排队」的基准，用它做分母切断这条正反馈。
    return std::max<std::uint32_t>(m_min_rtt_ms, m_config.min_rtt_ms);
}

std::uint64_t CongestionController::PacingRateBytesPerSec() const noexcept {
    const std::uint64_t denominator = std::max<std::uint64_t>(PacingDenominatorMs(), 1);
    std::uint64_t rate = static_cast<std::uint64_t>(m_cwnd_packets) * m_config.mss_bytes * 1000ULL /
                         denominator;
    if (rate < m_config.min_pacing_bytes_per_sec) {
        rate = m_config.min_pacing_bytes_per_sec;
    }
    return rate;
}

std::uint64_t CongestionController::pacing_rate_bps() const noexcept {
    if (!m_config.enabled || !m_config.pacing) {
        return 0;
    }
    return PacingRateBytesPerSec();
}

void CongestionController::RefillTokens(std::uint64_t now_ms) noexcept {
    if (!m_config.enabled || !m_config.pacing) {
        return;
    }
    if (now_ms <= m_last_refill_ms) {
        return;
    }
    const std::uint64_t elapsed_ms = now_ms - m_last_refill_ms;
    // 每毫秒补充的字节数，至少 1B/ms，避免极小窗口永远无法发送。
    const std::uint64_t bytes_per_ms =
        std::max<std::uint64_t>(1, PacingRateBytesPerSec() / 1000ULL);
    const std::uint64_t capacity =
        static_cast<std::uint64_t>(m_cwnd_packets) * m_config.mss_bytes;
    m_tokens_bytes = std::min<std::uint64_t>(capacity, m_tokens_bytes + elapsed_ms * bytes_per_ms);
    m_last_refill_ms = now_ms;
}

bool CongestionController::AllowSend(std::uint64_t now_ms, std::size_t inflight) noexcept {
    if (!m_config.enabled) {
        return true;
    }
    RefillTokens(now_ms);
    if (inflight >= m_cwnd_packets) {
        return false;
    }
    if (m_config.pacing && m_tokens_bytes < m_config.mss_bytes) {
        ++m_pacing_deferrals;
        return false;
    }
    return true;
}

void CongestionController::OnPacketSent(std::uint64_t now_ms, std::size_t bytes) noexcept {
    (void)now_ms;
    ++m_packets_sent;
    m_bytes_sent += bytes;
    if (!m_config.enabled) {
        return;
    }
    if (m_config.pacing) {
        // 按 MSS 计费（小于 MSS 的包也占一个包时隙），与 cwnd 的包单位
        // 一致；否则大量 32B 小包会绕过 pacing 的包间隔限制。
        const std::uint64_t cost =
            std::max<std::uint64_t>(bytes, m_config.mss_bytes);
        m_tokens_bytes = cost >= m_tokens_bytes ? 0 : m_tokens_bytes - cost;
    }
}

void CongestionController::NoteAckedForLossRate(std::uint32_t packets) noexcept {
    if (m_config.loss_sample_packets == 0) {
        return;
    }
    m_window_acked += packets;
    MaybeUpdateLossRate();
}

void CongestionController::NoteLossForLossRate(std::uint32_t packets) noexcept {
    if (m_config.loss_sample_packets == 0) {
        return;
    }
    m_window_lost += packets;
    MaybeUpdateLossRate();
}

void CongestionController::MaybeUpdateLossRate() noexcept {
    const std::uint64_t total = m_window_acked + m_window_lost;
    if (total < m_config.loss_sample_packets) {
        return;
    }
    // 包事件口径（ACK 数 + 丢包事件数）：每个窗口 32 个事件已足够平滑，
    // 直接替换而不是再做 EWMA，避免引入额外的时间常数。
    m_loss_rate_permille =
        static_cast<std::uint32_t>(m_window_lost * 1000ULL / std::max<std::uint64_t>(total, 1));
    m_window_acked = 0;
    m_window_lost = 0;
}

void CongestionController::OnAck(std::uint32_t acked_packets, std::uint32_t srtt_ms,
                                 std::uint64_t now_ms) noexcept {
    (void)now_ms;
    if (srtt_ms > 0) {
        m_srtt_ms = std::max(srtt_ms, m_config.min_rtt_ms);
        if (!m_has_rtt_sample || m_srtt_ms < m_min_rtt_ms) {
            m_min_rtt_ms = m_srtt_ms;
            m_has_rtt_sample = true;
        }
    }
    if (!m_config.enabled || acked_packets == 0) {
        return;
    }
    m_packets_acked += acked_packets;
    NoteAckedForLossRate(acked_packets);

    if (m_slow_start) {
        // 慢启动：每确认一个包 +1 MSS；到 ssthresh 后转拥塞避免。
        m_cwnd_packets = SaturateCwnd(m_cwnd_packets + acked_packets);
        if (m_cwnd_packets >= m_ssthresh_packets) {
            m_cwnd_packets = m_ssthresh_packets;
            m_slow_start = false;
            m_ca_bytes = 0;
        }
        return;
    }

    // 拥塞避免：累计 acked × MSS 字节，凑满一个 cwnd 才 +1 MSS。
    m_ca_bytes += static_cast<std::uint64_t>(acked_packets) * m_config.mss_bytes;
    while (m_ca_bytes >= static_cast<std::uint64_t>(m_cwnd_packets) * m_config.mss_bytes &&
           m_cwnd_packets < m_config.max_cwnd_packets) {
        m_ca_bytes -= static_cast<std::uint64_t>(m_cwnd_packets) * m_config.mss_bytes;
        ++m_cwnd_packets;
    }
    m_cwnd_packets = SaturateCwnd(m_cwnd_packets);
}

bool CongestionController::LooksLikeCongestion(const LossContext& ctx) const noexcept {
    if (!m_config.random_loss_aware) {
        return true;  // 旧行为：任何丢包都当作拥塞。
    }
    const std::uint32_t srtt = std::max(ctx.srtt_ms, m_config.min_rtt_ms);
    const std::uint32_t min_rtt = std::max(m_min_rtt_ms, m_config.min_rtt_ms);
    // 没有 RTT 样本时无法判断排队是否膨胀（min_rtt 还是配置默认值），
    // 此时不把「SRTT 比默认 min_rtt 大」当成拥塞证据——那会让握手后
    // 第一次丢包永远被判成拥塞。
    if (m_has_rtt_sample && m_config.rtt_inflate_permille > 0 &&
        static_cast<std::uint64_t>(srtt) * 1000ULL >
            static_cast<std::uint64_t>(min_rtt) * m_config.rtt_inflate_permille) {
        // RTT 相对 min_rtt 明显膨胀：瓶颈队列在累积，丢包是拥塞信号。
        return true;
    }
    if (ctx.holes >= 2 && m_loss_rate_permille >= m_config.extreme_loss_permille) {
        // 同一 SACK 窗口多个空洞 + 极高丢包率：瓶颈队列在批量丢弃。
        return true;
    }
    return false;
}

void CongestionController::OnLoss(const LossContext& ctx) noexcept {
    ++m_loss_events;
    if (!m_config.enabled) {
        return;
    }
    NoteLossForLossRate(1);
    // 同一 SRTT 内最多因「拥塞」降一次窗：连续空洞 ACK 不应把窗口瞬间打到最小。
    // 上一次归因是随机丢包时不做抑制，否则真拥塞事件会被误吞。
    const std::uint64_t guard_ms =
        std::max<std::uint64_t>(ctx.srtt_ms, m_config.min_rtt_ms);
    if (m_loss_recorded && ctx.now_ms >= m_last_loss_ms &&
        ctx.now_ms - m_last_loss_ms < guard_ms && m_last_loss_was_congestion) {
        return;
    }
    const bool congestion = LooksLikeCongestion(ctx);
    m_last_loss_ms = ctx.now_ms;
    m_loss_recorded = true;
    m_last_loss_was_congestion = congestion;

    if (!congestion) {
        // 随机丢包：不降窗（窗口塌到 2~5 是弱网吞吐崩塌的根因），
        // 只退出慢启动，避免用丢包换来的指数增长继续放大发送速率。
        ++m_random_loss_events;
        m_slow_start = m_cwnd_packets < m_ssthresh_packets;
        m_ca_bytes = 0;
        return;
    }

    ++m_congestion_loss_events;
    const std::uint32_t halved = std::max(m_cwnd_packets / 2, m_config.min_cwnd_packets);
    m_ssthresh_packets = SaturateCwnd(halved);
    m_cwnd_packets = m_ssthresh_packets;
    m_slow_start = false;
    m_ca_bytes = 0;
}

void CongestionController::OnLoss(std::uint64_t now_ms, std::uint32_t srtt_ms) noexcept {
    LossContext ctx;
    ctx.now_ms = now_ms;
    ctx.srtt_ms = srtt_ms;
    ctx.holes = 1;
    OnLoss(ctx);
}

void CongestionController::OnTimeout(const LossContext& ctx) noexcept {
    ++m_timeout_events;
    if (!m_config.enabled) {
        return;
    }
    NoteLossForLossRate(1);
    const bool congestion = LooksLikeCongestion(ctx);
    m_last_loss_ms = ctx.now_ms;
    m_loss_recorded = true;
    m_last_loss_was_congestion = congestion;

    if (!congestion) {
        // 随机丢包引发的 PTO：保留窗口。旧实现无条件 cwnd=2 并回慢启动，
        // 一次超时就把弱网吞吐打回 1~2 KB/s，且下一次丢包又会打断慢启动。
        ++m_random_loss_events;
        m_slow_start = m_cwnd_packets < m_ssthresh_packets;
        m_ca_bytes = 0;
        return;
    }

    ++m_congestion_loss_events;
    m_ssthresh_packets = SaturateCwnd(
        std::max(m_cwnd_packets / 2, m_config.min_cwnd_packets));
    m_cwnd_packets = m_config.min_cwnd_packets;
    m_slow_start = true;
    m_ca_bytes = 0;
}

void CongestionController::OnTimeout(std::uint64_t now_ms) noexcept {
    LossContext ctx;
    ctx.now_ms = now_ms;
    ctx.srtt_ms = m_srtt_ms;
    ctx.holes = 1;
    OnTimeout(ctx);
}

void CongestionController::NoteRetransmit() noexcept {
    ++m_retransmits;
}

CongestionStats CongestionController::Snapshot(std::size_t send_window) const noexcept {
    CongestionStats stats;
    stats.cwnd_packets = m_config.enabled
                             ? m_cwnd_packets
                             : static_cast<std::uint32_t>(std::min<std::size_t>(
                                   send_window, m_config.max_cwnd_packets));
    stats.ssthresh_packets = m_ssthresh_packets;
    stats.min_cwnd_packets = m_config.min_cwnd_packets;
    stats.mss_bytes = m_config.mss_bytes;
    stats.srtt_ms = m_srtt_ms;
    stats.min_rtt_ms = m_has_rtt_sample ? m_min_rtt_ms : 0;
    stats.loss_rate_permille = m_loss_rate_permille;
    stats.slow_start = m_slow_start;
    stats.pacing_enabled = m_config.enabled && m_config.pacing;
    stats.pacing_rate_bps = pacing_rate_bps();
    stats.packets_sent = m_packets_sent;
    stats.packets_acked = m_packets_acked;
    stats.bytes_sent = m_bytes_sent;
    stats.loss_events = m_loss_events;
    stats.timeout_events = m_timeout_events;
    stats.random_loss_events = m_random_loss_events;
    stats.congestion_loss_events = m_congestion_loss_events;
    stats.pacing_deferrals = m_pacing_deferrals;
    stats.retransmits = m_retransmits;
    stats.effective_window = static_cast<std::uint32_t>(
        std::min<std::size_t>(stats.cwnd_packets, send_window));
    return stats;
}

}  // namespace go2cpp::bento
