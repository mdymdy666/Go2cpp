/**
 * @file stats.cpp
 * @brief 延迟分位数与帧率统计实现。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace go2cpp::media {

void LatencyRecorder::Add(double ms) noexcept {
    if (!(ms >= 0.0)) {  // 同时挡住 NaN
        ms = 0.0;
    }
    m_samples.push_back(ms);
}

double LatencyRecorder::Quantile(double q) const {
    if (m_samples.empty()) {
        return 0.0;
    }
    if (q <= 0.0) {
        return min();
    }
    if (q >= 1.0) {
        return max();
    }
    std::vector<double> sorted = m_samples;
    std::sort(sorted.begin(), sorted.end());
    // 最近秩法：rank = ceil(q * n) - 1，对 p99 这类高位分位更保守。
    const double n = static_cast<double>(sorted.size());
    std::size_t rank = static_cast<std::size_t>(std::ceil(q * n));
    if (rank == 0) {
        rank = 1;
    }
    if (rank > sorted.size()) {
        rank = sorted.size();
    }
    return sorted[rank - 1];
}

double LatencyRecorder::max() const {
    if (m_samples.empty()) {
        return 0.0;
    }
    return *std::max_element(m_samples.begin(), m_samples.end());
}

double LatencyRecorder::min() const {
    if (m_samples.empty()) {
        return 0.0;
    }
    return *std::min_element(m_samples.begin(), m_samples.end());
}

double LatencyRecorder::mean() const {
    if (m_samples.empty()) {
        return 0.0;
    }
    double total = 0.0;
    for (double value : m_samples) {
        total += value;
    }
    return total / static_cast<double>(m_samples.size());
}

std::string LatencyRecorder::Summary() const {
    if (m_samples.empty()) {
        return "n=0 (no samples)";
    }
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "n=%zu p50=%.3f p90=%.3f p99=%.3f p999=%.3f max=%.3f mean=%.3f ms",
                  m_samples.size(), p50(), p90(), p99(), p999(), max(), mean());
    return std::string(buffer);
}

}  // namespace go2cpp::media
