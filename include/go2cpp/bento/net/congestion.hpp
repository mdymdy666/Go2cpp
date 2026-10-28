#pragma once

/**
 * @file congestion.hpp
 * @brief Bento 可靠通道的拥塞控制与 pacing：慢启动 / 拥塞避免 /
 *        随机丢包感知 / 乘性降窗 / 超时恢复 / 令牌桶 pacing。
 * @details 算法是 NewReno（RFC 5681）的整数简化版，并针对「无队列随机丢包」
 *          （无线、跨洲链路、netem）补了一层丢包归因，满足工业传输的
 *          最小闭环：
 *
 *          - **慢启动**：每个被确认的包使 cwnd +1 MSS（等价于每轮翻倍），
 *            直到 cwnd >= ssthresh。
 *          - **拥塞避免**：每收到 cwnd 个确认才 +1 MSS（按字节累计，
 *            等价于每轮 +1 MSS）。
 *          - **随机丢包感知**（`random_loss_aware`，默认开）：丢包并不总是
 *            拥塞。判据是「RTT 是否膨胀」+「同一 SACK 窗口是否出现多个空洞
 *            且丢包率极高」：
 *              * RTT 膨胀（SRTT > min_rtt × 2.0）说明排队在累积，是拥塞信号；
 *                无 RTT 样本时无法判断排队，不做这条判定；
 *              * 多个空洞 + 丢包率 >= 60% 说明瓶颈队列在批量丢弃；
 *              * 其余情况（孤立空洞、低丢包率、无排队）归因为随机丢包，
 *                **不降窗**，只退出慢启动；
 *            这样 20%~40% 随机丢包的链路上窗口不会被打到地板（旧实现每次
 *            丢包减半、超时回 2 MSS，实测 cwnd 长期 2~5、pacing 6~18 kB/s）。
 *          - **乘性降窗（真拥塞）**：ssthresh = max(cwnd/2, min_cwnd)，
 *            cwnd = ssthresh。同一 SRTT 内同一归因最多降一次，避免连续
 *            空洞 ACK 把窗口瞬间打到最小。
 *          - **PTO 超时**：真拥塞 → ssthresh 减半、cwnd = min_cwnd、回慢启动；
 *            随机丢包导致 → **保留窗口**（只重传、不塌窗），同时保持慢启动
 *            让 ACK 恢复后能重新填满管道。旧实现无条件回到 2 MSS，是弱网
 *            下「超时一次就再也起不来」的主因。
 *          - **pacing**：速率 = cwnd × MSS / RTT，用容量为一个 cwnd 的
 *            令牌桶平滑发送。分母优先取 **min_rtt**（`use_min_rtt_for_pacing`）：
 *            丢包会推高 SRTT（Karn 算法丢弃重传样本），若继续用 SRTT 做分母，
 *            pacing 会随丢包自我衰减成死亡螺旋。速率另有硬下限
 *            `min_pacing_bytes_per_sec`（默认 12500 B/s = 100 kbps），
 *            保证「小窗口 + 大 RTT」时不至于每毫秒只补几字节。
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
    std::uint32_t min_cwnd_packets = 8;  ///< 下限（弱网默认 8，旧实现为 2）。
    std::uint32_t mss_bytes = 1200;      ///< MSS 估算。
    std::uint32_t srtt_ms = 333;         ///< 用于 pacing 的平滑 RTT。
    std::uint32_t min_rtt_ms = 0;        ///< 观测到的最小 SRTT（0=尚无样本）。
    std::uint32_t loss_rate_permille = 0;  ///< 最近采样窗口丢包率（千分比）。
    bool slow_start = true;              ///< true=慢启动，false=拥塞避免。
    bool pacing_enabled = true;
    std::uint64_t pacing_rate_bps = 0;   ///< 当前 pacing 速率（bytes/s）。
    std::uint64_t packets_sent = 0;      ///< 经拥塞控制放行的新数据包数。
    std::uint64_t packets_acked = 0;     ///< 累计确认包数。
    std::uint64_t bytes_sent = 0;        ///< 放行字节数（含估算）。
    std::uint64_t loss_events = 0;       ///< 全部丢包事件（快速重传 + PTO）。
    std::uint64_t timeout_events = 0;    ///< PTO 超时次数。
    std::uint64_t random_loss_events = 0;      ///< 归因为随机丢包、不降窗的次数。
    std::uint64_t congestion_loss_events = 0;  ///< 归因为拥塞、触发降窗的次数。
    std::uint64_t pacing_deferrals = 0;  ///< 因令牌不足被延后的发送尝试。
    std::uint64_t retransmits = 0;       ///< 重传包数（仅观测）。
    std::uint32_t effective_window = 0;  ///< min(cwnd, 发送窗口)。
};

/// @brief NewReno 风格拥塞控制器 + 随机丢包归因 + 令牌桶 pacing。
class CongestionController final {
   public:
    /// @brief 一次丢包事件的上下文；用于区分「随机丢包」与「拥塞丢包」。
    struct LossContext {
        std::uint64_t now_ms = 0;  ///< 事件时刻。
        std::uint32_t srtt_ms = 0; ///< 事件时的 SRTT 估计。
        /// 同一 SACK 窗口内可判定的空洞数（连续丢失的包数）。1 = 孤立丢包；
        /// >= 2 表示同一窗口内有多个包同时丢失。PTO 场景无法判定，调用方传 1。
        std::uint32_t holes = 1;
        std::size_t inflight = 0;  ///< 事件时的未确认包数（诊断用）。
    };

    struct Config {
        bool enabled = true;   ///< false 完全退回旧行为（窗口=发送窗口，无 pacing）。
        bool pacing = true;    ///< 是否启用令牌桶 pacing。
        std::uint32_t initial_cwnd_packets = 64;
        /// 窗口下限。弱网实测：旧值 2 会让随机丢包把吞吐压到 1~2 KB/s，
        /// 提到 8 后即使被误降窗也保留每 RTT 8 个包的基本盘。
        std::uint32_t min_cwnd_packets = 8;
        std::uint32_t max_cwnd_packets = 256;  ///< 与 ReliableChannel::kSendWindow 对齐。
        std::uint32_t initial_ssthresh_packets = 128;
        std::uint32_t mss_bytes = 1200;
        std::uint32_t initial_rtt_ms = 333;
        std::uint32_t min_rtt_ms = 20;  ///< pacing 分母下限，避免除零/过小。
        /// 随机丢包归因开关。false = 旧行为（任何丢包都乘性降窗）。
        bool random_loss_aware = true;
        /// SRTT / min_rtt 超过该千分比视为 RTT 膨胀（排队），默认 2000 = 2.0x。
        std::uint32_t rtt_inflate_permille = 2000;
        /// 丢包率达到该千分比且同一窗口出现多空洞时，归因为拥塞，默认 600 = 60%。
        std::uint32_t extreme_loss_permille = 600;
        /// 丢包率采样窗口的包事件数（acked + lost）；0 表示禁用丢包率估计。
        std::uint32_t loss_sample_packets = 32;
        /// pacing 速率硬下限（bytes/s）。默认 12500 B/s = 100 kbps。
        std::uint64_t min_pacing_bytes_per_sec = 12500;
        /// pacing 分母是否取 min_rtt（抑制「丢包 → SRTT 膨胀 → pacing 崩塌」）。
        bool use_min_rtt_for_pacing = true;
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

    /// @brief 收到 ACK：按慢启动/拥塞避免增长窗口，并刷新 RTT 估计。
    /// @param acked_packets 本次新确认的包数。
    /// @param srtt_ms 当前 SRTT 估计（用于 pacing）；0 表示无新样本。
    /// @param now_ms 当前时刻。
    void OnAck(std::uint32_t acked_packets, std::uint32_t srtt_ms, std::uint64_t now_ms) noexcept;

    /// @brief 丢包（快速重传）事件：按归因决定是否降窗。
    /// @param ctx 丢包上下文（空洞数 / SRTT / 时刻）。
    void OnLoss(const LossContext& ctx) noexcept;

    /// @brief 便捷重载：调用方无法判定空洞数时按孤立丢包（holes=1）处理。
    void OnLoss(std::uint64_t now_ms, std::uint32_t srtt_ms) noexcept;

    /// @brief PTO 超时事件：按归因决定「降窗回慢启动」还是「保留窗口」。
    void OnTimeout(const LossContext& ctx) noexcept;

    /// @brief 便捷重载：调用方无法判定空洞数时按孤立丢包（holes=1）处理。
    void OnTimeout(std::uint64_t now_ms) noexcept;

    /// @brief 观测到一次重传（不改变窗口）。
    void NoteRetransmit() noexcept;

    /// @brief 生成统计快照；effective_window 用发送窗口夹逼。
    CongestionStats Snapshot(std::size_t send_window) const noexcept;

   private:
    void RefillTokens(std::uint64_t now_ms) noexcept;
    std::uint32_t SaturateCwnd(std::uint32_t value) const noexcept;

    /// @brief pacing 分母（毫秒）：优先 min_rtt，抑制丢包导致的速率自我衰减。
    std::uint32_t PacingDenominatorMs() const noexcept;
    /// @brief 未应用 enabled/pacing 开关时的裸 pacing 速率（bytes/s）。
    std::uint64_t PacingRateBytesPerSec() const noexcept;

    /// @brief 本次丢包是否应归因为拥塞（而非随机丢包）。
    bool LooksLikeCongestion(const LossContext& ctx) const noexcept;

    /// @brief 丢包率采样：累计 ACK / 丢包包事件，凑满窗口后刷新千分比。
    void NoteAckedForLossRate(std::uint32_t packets) noexcept;
    void NoteLossForLossRate(std::uint32_t packets) noexcept;
    void MaybeUpdateLossRate() noexcept;

    Config m_config;
    std::uint32_t m_cwnd_packets = 32;
    std::uint32_t m_ssthresh_packets = 64;
    std::uint32_t m_srtt_ms = 333;
    std::uint32_t m_min_rtt_ms = 0;        ///< 观测到的最小 SRTT；0=尚无样本。
    bool m_has_rtt_sample = false;
    bool m_slow_start = true;
    std::uint64_t m_ca_bytes = 0;         ///< 拥塞避免的字节累计。
    std::uint64_t m_tokens_bytes = 0;     ///< pacing 令牌。
    std::uint64_t m_last_refill_ms = 0;
    std::uint64_t m_last_loss_ms = 0;
    bool m_loss_recorded = false;
    bool m_last_loss_was_congestion = false;  ///< 上次归因，用于「同 SRTT 不重复降窗」。

    // 丢包率估计（包事件口径，窗口 = loss_sample_packets）。
    std::uint32_t m_loss_rate_permille = 0;
    std::uint64_t m_window_acked = 0;
    std::uint64_t m_window_lost = 0;

    // 累计统计（Snapshot 时拼进返回值）。
    std::uint64_t m_packets_sent = 0;
    std::uint64_t m_packets_acked = 0;
    std::uint64_t m_bytes_sent = 0;
    std::uint64_t m_loss_events = 0;
    std::uint64_t m_timeout_events = 0;
    std::uint64_t m_random_loss_events = 0;
    std::uint64_t m_congestion_loss_events = 0;
    std::uint64_t m_pacing_deferrals = 0;
    std::uint64_t m_retransmits = 0;
};

}  // namespace go2cpp::bento
