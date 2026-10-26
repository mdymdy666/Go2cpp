/**
 * @file congestion.cpp
 * @brief NewReno 拥塞控制 + 令牌桶 pacing 实现。
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
    if (config.min_cwnd_packets < 2) {
        config.min_cwnd_packets = 2;
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
    m_slow_start = true;
    m_ca_bytes = 0;
    m_last_refill_ms = now_ms;
    m_last_loss_ms = 0;
    m_loss_recorded = false;
    m_packets_sent = 0;
    m_packets_acked = 0;
    m_bytes_sent = 0;
    m_loss_events = 0;
    m_timeout_events = 0;
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

std::uint64_t CongestionController::pacing_rate_bps() const noexcept {
    if (!m_config.enabled || !m_config.pacing) {
        return 0;
    }
    const std::uint64_t srtt = std::max<std::uint64_t>(m_srtt_ms, 1);
    return static_cast<std::uint64_t>(m_cwnd_packets) * m_config.mss_bytes * 1000ULL / srtt;
}

void CongestionController::RefillTokens(std::uint64_t now_ms) noexcept {
    if (!m_config.enabled || !m_config.pacing) {
        return;
    }
    if (now_ms <= m_last_refill_ms) {
        return;
    }
    const std::uint64_t elapsed_ms = now_ms - m_last_refill_ms;
    const std::uint64_t srtt = std::max<std::uint64_t>(m_srtt_ms, 1);
    // 每毫秒补充的字节数，至少 1B/ms，避免极小窗口永远无法发送。
    const std::uint64_t bytes_per_ms =
        std::max<std::uint64_t>(1, static_cast<std::uint64_t>(m_cwnd_packets) *
                                       m_config.mss_bytes / srtt);
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

void CongestionController::OnAck(std::uint32_t acked_packets, std::uint32_t srtt_ms,
                                 std::uint64_t now_ms) noexcept {
    (void)now_ms;
    m_srtt_ms = std::max(srtt_ms, m_config.min_rtt_ms);
    if (!m_config.enabled || acked_packets == 0) {
        return;
    }
    m_packets_acked += acked_packets;

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

void CongestionController::OnLoss(std::uint64_t now_ms, std::uint32_t srtt_ms) noexcept {
    ++m_loss_events;
    if (!m_config.enabled) {
        return;
    }
    // 同一 SRTT 内最多降一次：连续空洞 ACK 不应把窗口瞬间打到最小。
    const std::uint64_t guard_ms =
        std::max<std::uint64_t>(srtt_ms, m_config.min_rtt_ms);
    if (m_loss_recorded && now_ms >= m_last_loss_ms &&
        now_ms - m_last_loss_ms < guard_ms) {
        return;
    }
    m_last_loss_ms = now_ms;
    m_loss_recorded = true;
    const std::uint32_t halved = std::max(m_cwnd_packets / 2, m_config.min_cwnd_packets);
    m_ssthresh_packets = SaturateCwnd(halved);
    m_cwnd_packets = m_ssthresh_packets;
    m_slow_start = false;
    m_ca_bytes = 0;
}

void CongestionController::OnTimeout(std::uint64_t now_ms) noexcept {
    ++m_timeout_events;
    if (!m_config.enabled) {
        return;
    }
    m_ssthresh_packets = SaturateCwnd(
        std::max(m_cwnd_packets / 2, m_config.min_cwnd_packets));
    m_cwnd_packets = m_config.min_cwnd_packets;
    m_slow_start = true;
    m_ca_bytes = 0;
    m_last_loss_ms = now_ms;
    m_loss_recorded = true;
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
    stats.slow_start = m_slow_start;
    stats.pacing_enabled = m_config.enabled && m_config.pacing;
    stats.pacing_rate_bps = pacing_rate_bps();
    stats.packets_sent = m_packets_sent;
    stats.packets_acked = m_packets_acked;
    stats.bytes_sent = m_bytes_sent;
    stats.loss_events = m_loss_events;
    stats.timeout_events = m_timeout_events;
    stats.pacing_deferrals = m_pacing_deferrals;
    stats.retransmits = m_retransmits;
    stats.effective_window = static_cast<std::uint32_t>(
        std::min<std::size_t>(stats.cwnd_packets, send_window));
    return stats;
}

}  // namespace go2cpp::bento
