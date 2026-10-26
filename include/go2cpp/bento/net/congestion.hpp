#pragma once

/**
 * @file congestion.hpp
 * @brief Bento 可靠通道的拥塞控制与 pacing：慢启动 / 拥塞避免 /
 *        乘性降窗 / 超时回慢启动 / 令牌桶 pacing。
 * @details 算法是 NewReno（RFC 5681）的整数简化版，满足工业传输的
 *          最小闭环：
 *
 *          - **慢启动**：每个被确认的包使 cwnd +1 MSS（等价于每轮翻倍），
 *            直到 cwnd >= ssthresh。
 *          - **拥塞避免**：每收到 cwnd 个确认才 +1 MSS（按字节累计，
 *            等价于每轮 +1 MSS）。
 *          - **快速重传/丢包**：ssthresh = max(cwnd/2, 2 MSS)，
 *            cwnd = ssthresh（乘性降）。同一 SRTT 内最多降一次，避免
 *            连续空洞 ACK 把窗口瞬间打到最小。
 *          - **PTO 超时**：ssthresh = max(cwnd/2, 2 MSS)，cwnd = 2 MSS，
 *            回到慢启动（加性增、乘性减的保守分支）。
 *          - **pacing**：速率 = cwnd × MSS / SRTT（bytes/ms），用容量为
 *            一个 cwnd 的令牌桶平滑发送；桶初始为满，允许一个窗口的
 *            突发，之后严格按速率补充。
 *
 *          cwnd 以「包」为单位、固定 MSS 估算（默认 1200B）。Bento 的
 *          STREAM 消息可能远小于 MSS，这里不按字节精细计算，保持可预测
 *          与可测试性；pacing 仍按包间隔限速。
 *
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>

namespace go2cpp::bento {

/// @brief 拥塞控制可导出统计（测试/诊断/报告直接读取）。
struct CongestionStats {
    std::uint32_t cwnd_packets = 0;      ///< 当前拥塞窗口（包）。
    std::uint32_t ssthresh_packets = 0;  ///< 慢启动阈值（包）。
    std::uint32_t min_cwnd_packets = 2;  ///< 下限。
    std::uint32_t mss_bytes = 1200;      ///< MSS 估算。
    std::uint32_t srtt_ms = 333;         ///< 用于 pacing 的平滑 RTT。
    bool slow_start = true;              ///< true=慢启动，false=拥塞避免。
    bool pacing_enabled = true;
    std::uint64_t pacing_rate_bps = 0;   ///< 当前 pacing 速率（bytes/s）。
    std::uint64_t packets_sent = 0;      ///< 经拥塞控制放行的新数据包数。
    std::uint64_t packets_acked = 0;     ///< 累计确认包数。
    std::uint64_t bytes_sent = 0;        ///< 放行字节数（含估算）。
    std::uint64_t loss_events = 0;       ///< 快速重传/丢包降窗次数。
    std::uint64_t timeout_events = 0;    ///< PTO 超时降窗次数。
    std::uint64_t pacing_deferrals = 0;  ///< 因令牌不足被延后的发送尝试。
    std::uint64_t retransmits = 0;       ///< 重传包数（仅观测）。
    std::uint32_t effective_window = 0;  ///< min(cwnd, 发送窗口)。
};

/// @brief NewReno 风格拥塞控制器 + 令牌桶 pacing。
class CongestionController final {
   public:
    struct Config {
        bool enabled = true;   ///< false 完全退回旧行为（窗口=发送窗口，无 pacing）。
        bool pacing = true;    ///< 是否启用令牌桶 pacing。
        std::uint32_t initial_cwnd_packets = 32;
        std::uint32_t min_cwnd_packets = 2;
        std::uint32_t max_cwnd_packets = 256;  ///< 与 ReliableChannel::kSendWindow 对齐。
        std::uint32_t initial_ssthresh_packets = 64;
        std::uint32_t mss_bytes = 1200;
        std::uint32_t initial_rtt_ms = 333;
        std::uint32_t min_rtt_ms = 20;  ///< pacing 分母下限，避免除零/过小。
    };

    CongestionController() noexcept;
    explicit CongestionController(Config config) noexcept;

    /// @brief 应用配置；会按新配置重置窗口与令牌桶。
    void SetConfig(Config config) noexcept;
    const Config& config() const noexcept {
        return m_config;
    }

    /// @brief 重置窗口、令牌桶与统计（新连接/新会话）。
    void Reset(std::uint64_t now_ms) noexcept;

    bool enabled() const noexcept {
        return m_config.enabled;
    }
    std::uint32_t cwnd() const noexcept {
        return m_cwnd_packets;
    }
    std::uint32_t ssthresh() const noexcept {
        return m_ssthresh_packets;
    }
    bool slow_start() const noexcept {
        return m_slow_start;
    }
    std::uint32_t srtt_ms() const noexcept {
        return m_srtt_ms;
    }

    /// @brief 当前 pacing 速率（bytes/s），未启用时为 0。
    std::uint64_t pacing_rate_bps() const noexcept;

    /// @brief 放行一个新数据包：检查 cwnd 与 pacing 令牌。
    /// @param inflight 当前未确认包数。
    /// @note 因 pacing 延后时递增 pacing_deferrals；因 cwnd 满时不计数。
    bool AllowSend(std::uint64_t now_ms, std::size_t inflight) noexcept;

    /// @brief 记录一个已发送包并消耗令牌。
    void OnPacketSent(std::uint64_t now_ms, std::size_t bytes) noexcept;

    /// @brief 收到 ACK：按慢启动/拥塞避免增长窗口。
    /// @param acked_packets 本次新确认的包数。
    /// @param srtt_ms 当前 SRTT 估计（用于 pacing）。
    void OnAck(std::uint32_t acked_packets, std::uint32_t srtt_ms, std::uint64_t now_ms) noexcept;

    /// @brief 丢包（快速重传）事件：乘性降窗。
    void OnLoss(std::uint64_t now_ms, std::uint32_t srtt_ms) noexcept;

    /// @brief PTO 超时事件：窗口回到 2 MSS 并进入慢启动。
    void OnTimeout(std::uint64_t now_ms) noexcept;

    /// @brief 观测到一次重传（不改变窗口）。
    void NoteRetransmit() noexcept;

    /// @brief 生成统计快照；effective_window 用发送窗口夹逼。
    CongestionStats Snapshot(std::size_t send_window) const noexcept;

   private:
    void RefillTokens(std::uint64_t now_ms) noexcept;
    std::uint32_t SaturateCwnd(std::uint32_t value) const noexcept;

    Config m_config;
    std::uint32_t m_cwnd_packets = 32;
    std::uint32_t m_ssthresh_packets = 64;
    std::uint32_t m_srtt_ms = 333;
    bool m_slow_start = true;
    std::uint64_t m_ca_bytes = 0;         ///< 拥塞避免的字节累计。
    std::uint64_t m_tokens_bytes = 0;     ///< pacing 令牌。
    std::uint64_t m_last_refill_ms = 0;
    std::uint64_t m_last_loss_ms = 0;
    bool m_loss_recorded = false;

    // 累计统计（Snapshot 时拼进返回值）。
    std::uint64_t m_packets_sent = 0;
    std::uint64_t m_packets_acked = 0;
    std::uint64_t m_bytes_sent = 0;
    std::uint64_t m_loss_events = 0;
    std::uint64_t m_timeout_events = 0;
    std::uint64_t m_pacing_deferrals = 0;
    std::uint64_t m_retransmits = 0;
};

}  // namespace go2cpp::bento
