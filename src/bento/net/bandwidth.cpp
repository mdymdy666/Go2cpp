/**
 * @file bandwidth.cpp
 * @brief 滑动窗口交付速率/丢包率估计实现。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include "go2cpp/bento/net/bandwidth.hpp"

#include <algorithm>

namespace go2cpp::bento {

BandwidthEstimator::BandwidthEstimator() noexcept : BandwidthEstimator(Config{}) {}

BandwidthEstimator::BandwidthEstimator(Config config) noexcept {
    SetConfig(config);
}

void BandwidthEstimator::SetConfig(Config config) noexcept {
    if (config.window_ms == 0) {
        config.window_ms = 1;
    }
    if (config.min_window_ms == 0) {
        config.min_window_ms = 1;
    }
    if (config.min_window_ms > config.window_ms) {
        config.min_window_ms = config.window_ms;
    }
    if (config.max_samples == 0) {
        config.max_samples = 1;
    }
    m_config = config;
    // 收紧窗口后立即丢弃不再需要的旧样本，避免它们继续占用内存。
    while (m_samples.size() > m_config.max_samples) {
        m_samples.pop_front();
    }
}

void BandwidthEstimator::Reset() noexcept {
    m_samples.clear();
    m_acked_packets_total = 0;
    m_lost_packets_total = 0;
}

BandwidthEstimator::WindowSum BandwidthEstimator::ComputeWindow(
    std::uint64_t now_ms) const noexcept {
    WindowSum sum;
    for (const AckSample& sample : m_samples) {
        // 时钟回退产生的「未来样本」也按过期处理，避免负跨度。
        if (sample.time_ms > now_ms || now_ms - sample.time_ms > m_config.window_ms) {
            continue;
        }
        sum.bytes += sample.bytes;
        ++sum.samples;
        if (!sum.has_sample || sample.time_ms < sum.oldest_ms) {
            sum.oldest_ms = sample.time_ms;
        }
        sum.has_sample = true;
    }
    return sum;
}

void BandwidthEstimator::Prune(std::uint64_t now_ms) noexcept {
    while (!m_samples.empty()) {
        const AckSample& front = m_samples.front();
        if (front.time_ms <= now_ms && now_ms - front.time_ms <= m_config.window_ms) {
            break;
        }
        m_samples.pop_front();
    }
    while (m_samples.size() > m_config.max_samples) {
        m_samples.pop_front();
    }
}

void BandwidthEstimator::OnAck(std::uint64_t now_ms, std::size_t bytes) noexcept {
    m_samples.push_back(AckSample{now_ms, static_cast<std::uint64_t>(bytes)});
    ++m_acked_packets_total;
    Prune(now_ms);
}

void BandwidthEstimator::OnLoss(std::uint64_t now_ms, std::size_t packets) noexcept {
    // 丢包率按累计事件统计：窗口外的历史丢包仍应影响长期质量判断，
    // 因此 now_ms 只用于接口一致性（未来可扩展为滑动丢包窗口）。
    (void)now_ms;
    m_lost_packets_total += static_cast<std::uint64_t>(packets);
}

std::uint64_t BandwidthEstimator::delivery_rate_bps(std::uint64_t now_ms) const noexcept {
    const WindowSum sum = ComputeWindow(now_ms);
    if (!sum.has_sample) {
        return 0;
    }
    std::uint64_t span_ms = now_ms >= sum.oldest_ms ? now_ms - sum.oldest_ms : 0;
    if (span_ms < m_config.min_window_ms) {
        span_ms = m_config.min_window_ms;
    }
    if (span_ms == 0) {
        return 0;
    }
    return sum.bytes * 8ULL * 1000ULL / span_ms;
}

double BandwidthEstimator::loss_rate() const noexcept {
    const std::uint64_t total = m_acked_packets_total + m_lost_packets_total;
    if (total == 0) {
        return 0.0;
    }
    return static_cast<double>(m_lost_packets_total) / static_cast<double>(total);
}

BandwidthStats BandwidthEstimator::Snapshot(std::uint64_t now_ms) const noexcept {
    BandwidthStats stats;
    const WindowSum sum = ComputeWindow(now_ms);
    std::uint64_t span_ms = 0;
    if (sum.has_sample && now_ms >= sum.oldest_ms) {
        span_ms = now_ms - sum.oldest_ms;
    }
    const std::uint64_t bounded_span =
        std::max<std::uint64_t>(span_ms, m_config.min_window_ms);
    stats.acked_bytes = sum.bytes;
    stats.acked_packets = m_acked_packets_total;
    stats.lost_packets = m_lost_packets_total;
    stats.ack_samples = sum.samples;
    stats.window_ms = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(bounded_span, 0xFFFFFFFFULL));
    stats.delivery_rate_bps = delivery_rate_bps(now_ms);
    stats.loss_rate = loss_rate();
    return stats;
}

}  // namespace go2cpp::bento
