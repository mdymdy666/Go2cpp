#pragma once

/**
 * @file stats.hpp
 * @brief 媒体管线统计：延迟分位数（p50/p99/p999）与帧率计。
 * @details 延迟采样用「排序后取分位」的精确算法（样本量 <= 10 万时代价可忽略），
 *          不引入 HDR 直方图的桶误差；单测里可直接断言 p50 <= p99 <= max。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp::media {

/// @brief 延迟采样与分位数统计（单位毫秒）。
class LatencyRecorder {
   public:
    /// @brief 记录一次延迟（毫秒，负数按 0 处理）。
    void Add(double ms) noexcept;

    /// @brief 样本数。
    std::size_t count() const noexcept {
        return m_samples.size();
    }

    /// @brief 样本是否为空。
    bool empty() const noexcept {
        return m_samples.empty();
    }

    /// @brief 第 `q` 分位（0..1），空样本返回 0。
    double Quantile(double q) const;

    /// @brief p50 / p90 / p99 / p999 / max / mean / min。
    double p50() const {
        return Quantile(0.50);
    }
    double p90() const {
        return Quantile(0.90);
    }
    double p99() const {
        return Quantile(0.99);
    }
    double p999() const {
        return Quantile(0.999);
    }
    double max() const;
    double min() const;
    double mean() const;

    /// @brief 清空样本。
    void Clear() noexcept {
        m_samples.clear();
    }

    /// @brief 一行摘要："n=100 p50=0.42 p99=1.03 max=2.10 mean=0.55 ms"。
    std::string Summary() const;

    /// @brief 原始样本（只读）。
    const std::vector<double>& samples() const noexcept {
        return m_samples;
    }

   private:
    std::vector<double> m_samples;
};

/// @brief 帧率/吞吐计数。
class RateMeter {
   public:
    /// @brief 记录 `frames` 帧、跨越 `elapsed_ms` 毫秒。
    void Observe(std::uint64_t frames, double elapsed_ms) noexcept {
        if (elapsed_ms > 0.0) {
            m_fps = static_cast<double>(frames) * 1000.0 / elapsed_ms;
        }
        m_frames += frames;
        m_elapsed_ms += elapsed_ms;
    }

    /// @brief 最近一次观测的帧率。
    double fps() const noexcept {
        return m_fps;
    }

    /// @brief 累计帧数。
    std::uint64_t frames() const noexcept {
        return m_frames;
    }

    /// @brief 累计耗时（毫秒）。
    double elapsed_ms() const noexcept {
        return m_elapsed_ms;
    }

   private:
    double m_fps = 0.0;
    std::uint64_t m_frames = 0;
    double m_elapsed_ms = 0.0;
};

}  // namespace go2cpp::media
