#pragma once

/**
 * @file bandwidth.hpp
 * @brief 交付速率与丢包率估计（RFC 9002 delivery rate 的滑动窗口简化）。
 * @details 工业传输的 ABR 需要知道「链路实际能交付多少」而不是「发了多少」：
 *
 *          - **delivery_rate**：只统计被 ACK 确认的应用字节。维护一个
 *            固定长度的滑动窗口（默认 1s），速率 = 窗口内确认字节 × 8 /
 *            窗口实际跨度，单位 bit/s。分母下限 min_window_ms（默认
 *            100ms）避免启动/稀疏流量把速率放大到无穷。
 *          - **loss_rate**：累计推断丢失包数 / (累计确认包数 + 丢失包数)。
 *            丢包由可靠层在快速重传与 PTO 事件中上报；这是「事件级」
 *            近似，不依赖时间戳回显。
 *
 *          估算器不持有锁、不分配堆内存（deque 仅在 OnAck 时增长到
 *          max_samples）。由 ReliableChannel 在接收线程的 ACK 处理路径
 *          调用；Link 汇总所有可靠通道得到链路级带宽。
 *
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <deque>

namespace go2cpp::bento {

/// @brief 带宽/丢包率快照。
struct BandwidthStats {
    std::uint64_t delivery_rate_bps = 0;  ///< 交付速率（bit/s）。
    double loss_rate = 0.0;               ///< 丢包率（0.0..1.0）。
    std::uint64_t acked_bytes = 0;        ///< 窗口内确认的应用字节。
    std::uint64_t acked_packets = 0;      ///< 累计确认的包数。
    std::uint64_t lost_packets = 0;       ///< 累计推断丢失的包数。
    std::uint64_t ack_samples = 0;        ///< 窗口内的 ACK 样本数。
    std::uint32_t window_ms = 0;          ///< 速率计算实际使用的跨度（毫秒）。
};

/// @brief 滑动窗口交付速率 + 累计丢包率估计器。
class BandwidthEstimator final {
   public:
    struct Config {
        std::uint32_t window_ms = 1000;     ///< 滑动窗口长度。
        std::uint32_t min_window_ms = 100;  ///< 速率分母下限。
        std::size_t max_samples = 256;      ///< 窗口内样本上限（内存上界）。
    };

    BandwidthEstimator() noexcept;
    explicit BandwidthEstimator(Config config) noexcept;

    /// @brief 更新配置；已有样本保留，下一次 OnAck/Snapshot 按新窗口过滤。
    void SetConfig(Config config) noexcept;
    const Config& config() const noexcept {
        return m_config;
    }

    /// @brief 丢弃全部样本与累计计数。
    void Reset() noexcept;

    /// @brief 记录一次 ACK 交付的字节数（应用载荷字节）。
    void OnAck(std::uint64_t now_ms, std::size_t bytes) noexcept;

    /// @brief 记录一次推断丢包（快速重传/PTO）。
    void OnLoss(std::uint64_t now_ms, std::size_t packets = 1) noexcept;

    /// @brief 当前交付速率（bit/s）；窗口无样本返回 0。
    std::uint64_t delivery_rate_bps(std::uint64_t now_ms) const noexcept;

    /// @brief 累计丢包率（0.0..1.0）。
    double loss_rate() const noexcept;

    /// @brief 生成窗口统计快照。
    BandwidthStats Snapshot(std::uint64_t now_ms) const noexcept;

   private:
    struct AckSample {
        std::uint64_t time_ms = 0;
        std::uint64_t bytes = 0;
    };

    /// @brief 窗口聚合结果。
    struct WindowSum {
        std::uint64_t bytes = 0;
        std::uint64_t samples = 0;
        std::uint64_t oldest_ms = 0;
        bool has_sample = false;
    };

    WindowSum ComputeWindow(std::uint64_t now_ms) const noexcept;
    void Prune(std::uint64_t now_ms) noexcept;

    Config m_config;
    std::deque<AckSample> m_samples;
    std::uint64_t m_acked_packets_total = 0;
    std::uint64_t m_lost_packets_total = 0;
};

}  // namespace go2cpp::bento
