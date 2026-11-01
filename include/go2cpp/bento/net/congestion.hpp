#pragma once

/**
 * @file congestion.hpp
 * @brief Bento 可靠通道的拥塞控制：RTT/RTO 估计、带宽估计、pacing 与
 *        BBR 风格控制器，并保留经典 NewReno 作为对照基线。
 * @details 为什么需要这一层（弱网实测结论）：
 *
 *          旧实现只有「固定发送窗口 256 + 每包独立 PTO 200ms×2 退避」。
 *          在 5% 随机丢包、100ms RTT 的链路上，固定窗口会一次性灌入上百个包，
 *          把无法排空的队列直接打爆；PTO 又与服务端 RTT 无关，导致大量
 *          「尚未丢失却被判定超时」的伪重传。两者叠加的结果是丢包率越高、
 *          有效吞吐越低，属于典型的 loss-based 失控。
 *
 *          BBR 的核心取舍是「丢包 ≠ 拥塞」：
 *          - 用「瓶颈带宽估计」决定发送速率（pacing），用「最小 RTT」估算 BDP；
 *          - cwnd 只用来限制 in-flight 不超过 BDP 的若干倍，不因随机丢包减半；
 *          - 随机丢包由重传/FEC 吸收，不触发降速，从而在 5% 丢包下仍能跑满带宽。
 *
 *          本实现是 BBRv1 的最小可用子集，刻意省略的部分在注释里标注：
 *          没有 ack aggregation / 没有带宽采样窗口的包级精确 round 判定
 *          （用时间轮次近似），也没有完全的 inflight_hi 探测（只做收缩）。
 *          这些取舍换来的是可单测的确定性状态机与零外部依赖。
 *
 *          NewReno 控制器保留「慢启动/拥塞避免/丢包减半/超时塌窗」的经典语义，
 *          仅作为 A/B 对照与回归基线（`CongestionAlgorithm::kNewReno`）。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace go2cpp::bento {

/// @brief 拥塞控制算法。kNone 保留旧行为（固定窗口 + PTO 退避）。
enum class CongestionAlgorithm : std::uint8_t {
    kNone = 0,     ///< 不启用拥塞控制（固定窗口，仅用于回归对照）。
    kNewReno = 1,  ///< RFC 6582 经典 NewReno（loss-based，对照组）。
    kBbr = 2,      ///< BBR 风格（带宽 + min_rtt + pacing，默认）。
};

/// @brief 算法名（日志/报告用）。
const char* CongestionAlgorithmName(CongestionAlgorithm algorithm) noexcept;

/// @brief BBR 状态机相位。
enum class BbrState : std::uint8_t {
    kStartup = 0,   ///< 指数探测带宽（pacing_gain > 1）。
    kDrain = 1,     ///< 排空 startup 造成的排队（pacing_gain < 1）。
    kProbeBw = 2,   ///< 稳态：周期性在 1.25/0.75/1.0 之间摆动探测带宽。
    kProbeRtt = 3,  ///< 周期性把 cwnd 压到最小以刷新 min_rtt。
};

/// @brief BBR 相位名（日志/报告用）。
const char* BbrStateName(BbrState state) noexcept;

/// @brief 拥塞控制配置。所有时间单位毫秒、速率单位字节/秒。
struct CongestionConfig {
    CongestionAlgorithm algorithm = CongestionAlgorithm::kBbr;

    /// 单个报文（MSS）的估算字节数；用于把 bps 换算成包数窗口。
    std::uint32_t mss_bytes = 1200;

    /// 初始窗口（包）。弱网 100ms RTT 下过小会让首轮 200 条消息要等 7+ 个 RTT，
    /// 过大则可能瞬间打爆瓶颈队列；64 包 ≈ 76KB，是 1Mbps×600ms 的 BDP 量级。
    std::size_t initial_cwnd_packets = 64;
    /// 最小窗口（包）。即使丢包严重也保留 4 包，避免「一次丢包永久停摆」。
    std::size_t min_cwnd_packets = 4;
    /// 最大窗口（包），与可靠通道的发送窗口上限一致。
    std::size_t max_cwnd_packets = 256;

    /// 还没有 RTT 样本时用于 pacing 下限的假设 RTT。
    std::uint32_t initial_rtt_ms = 100;
    /// RTO 下限（KCP 风格）。1Mbps 链路上一个 BDP 的排队就能超过 100ms，
    /// 下限过低会把正常排队误判成超时（实测会产生上百次伪 RTO）。
    std::uint32_t min_rto_ms = 200;
    /// RTO 上限。
    std::uint32_t max_rto_ms = 2000;

    /// 是否启用 pacing（关闭后只受 cwnd 限制，突发更明显）。
    bool pacing_enabled = true;

    /// 令牌桶突发窗口：允许在一个窗口内攒下的发送额度。
    std::uint32_t pacing_burst_ms = 5;
    /// pacing 速率下限，避免带宽估计为 0 时完全停发。
    std::uint64_t min_pacing_bytes_per_sec = 12500;

    // --- BBR 参数 -----------------------------------------------------------
    /// startup 的 cwnd/pacing 增益（2/ln2 ≈ 2.885）。
    double startup_cwnd_gain = 2.885;
    double startup_pacing_gain = 2.885;
    /// drain 的 pacing 增益（1/2.885 ≈ 0.346，这里取 0.75 更温和）。
    double drain_pacing_gain = 0.75;
    /// 稳态高增益相位。
    double probe_bw_pacing_gain = 1.25;
    /// 稳态 cwnd 增益（BBR 取 2.0，为 ACK 聚合留余量）。
    double probe_bw_cwnd_gain = 2.0;
    /// 稳态低增益相位。
    double probe_bw_low_gain = 0.75;
    /// startup 退出条件：连续若干轮带宽增长低于该比例。
    double startup_growth_threshold = 0.25;
    /// startup 判定「带宽不再增长」所需的连续轮数。
    std::uint32_t startup_full_bw_rounds = 3;
    /// startup 最长轮数（兜底，避免长时间指数增长）。
    std::uint32_t startup_max_rounds = 32;
    /// startup 在丢包率超过该比例时提前退出（BBRv1 的行为）。
    double startup_exit_loss_rate = 0.20;
    /// probe_bw 相位循环长度（BBRv1 为 8）。
    std::uint32_t probe_bw_cycle_rounds = 8;

    /// ProbeRTT 触发周期与持续时间（BBRv1：10s / 200ms）。
    std::uint32_t probe_rtt_interval_ms = 10000;
    std::uint32_t probe_rtt_duration_ms = 200;

    /// 带宽估计窗口（保留最近 N 个采样，取最大值）。
    std::size_t bandwidth_window = 10;
    /// 丢包时 inflight_hi 的收缩余量：inflight_hi = max(inflight×0.7, bdp)。
    double inflight_hi_shrink = 0.7;

    // --- NewReno 参数 -------------------------------------------------------
    /// 乘性减因子（RFC 2581 为 0.5）。
    double reno_decrease = 0.5;
    /// NewReno 的 pacing 增益（1.0 表示按 cwnd/SRTT 平滑发送）。
    double reno_pacing_gain = 1.0;

    // --- 兼容旧通道配置的字段（映射到上面的新参数）---------------------------
    /// false 等价于 algorithm=kNone（旧 Config 的开关）。
    bool enabled = true;
    /// 兼容旧名：false 等价于 pacing_enabled=false。
    bool pacing = true;
    /// 兼容旧名。BBR 天然不会因随机丢包降窗，该字段仅保留给调用方。
    bool random_loss_aware = true;
    /// 兼容旧名：NewReno 的初始 ssthresh（BBR 下按 2×初始窗口推导）。
    std::uint32_t initial_ssthresh_packets = 128;
    /// 兼容旧名：丢包率统计窗口（包事件数）。
    std::uint32_t loss_sample_packets = 32;
};

/// @brief RFC 6298 风格的 RTT 估计器（SRTT/RTTVAR + 滑窗 min_rtt）。
class BbrRttEstimator final {
   public:
    explicit BbrRttEstimator(std::uint32_t initial_rtt_ms = 100);

    /// @brief 加入一个 RTT 样本（必须是非重传包的样本，Karn 算法）。
    void AddSample(std::uint32_t rtt_ms, std::uint64_t now_ms);

    /// @brief 清理过期的 min_rtt 样本（窗口 10s）。
    void OnTimer(std::uint64_t now_ms);

    /// @brief 平滑 RTT（ms）；无样本时返回初始值。
    std::uint32_t srtt_ms() const noexcept {
        return m_srtt_ms;
    }
    /// @brief RTT 偏差（ms）。
    std::uint32_t rttvar_ms() const noexcept {
        return m_rttvar_ms;
    }
    /// @brief 最近一次样本（ms）。
    std::uint32_t latest_rtt_ms() const noexcept {
        return m_latest_rtt_ms;
    }
    /// @brief 滑窗内最小 RTT（ms）。
    std::uint32_t min_rtt_ms() const noexcept {
        return m_min_rtt_ms;
    }
    /// @brief 是否已有至少一个样本。
    bool has_sample() const noexcept {
        return m_has_sample;
    }

    /// @brief 计算 RTO：srtt + 4×rttvar，clamp 到 [min_rto, max_rto]。
    /// @note 无样本时返回 max(initial_rtt×6, min_rto)：初始 RTO 必须明显大于
    ///       首个 RTT（QUIC 的初始 RTO 同样是秒级）。若只给 2×initial_rtt，
    ///       第一个真实 RTT 样本到达前包就会被 PTO 重传，Karn 算法随即把
    ///       所有 RTT 样本过滤掉，min_rtt 永远为 0、BDP/RTO 全部失准。
    std::uint32_t RtoMs(std::uint32_t min_rto_ms, std::uint32_t max_rto_ms) const noexcept;

    /// @brief min_rtt 的采样窗口（毫秒）。
    static constexpr std::uint64_t kMinRttWindowMs = 10000;
    /// @brief 采样队列上限；超过后丢最旧样本并重算最小值。
    static constexpr std::size_t kMaxSamples = 2048;

   private:
    void RecomputeMinRtt() noexcept;

    std::uint32_t m_initial_rtt_ms;
    std::uint32_t m_srtt_ms = 0;
    std::uint32_t m_rttvar_ms = 0;
    std::uint32_t m_latest_rtt_ms = 0;
    std::uint32_t m_min_rtt_ms = 0;
    bool m_has_sample = false;
    std::deque<std::pair<std::uint64_t, std::uint32_t>> m_min_samples;
};

/// @brief 基于时间滑窗最大值的带宽估计器。
/// @details 每个样本形如「在 interval_ms 内发送了 delivered_bytes 字节」，
///          速率取滑窗内最大值（max-filter），这样能扛住 ACK 聚合与瞬时拥塞，
///          也符合 BBR「估计瓶颈可用带宽而非平均速率」的意图。
///
///          窗口按**时间**而不是样本数：重传率高时 ACK 样本会变得稀疏且
///          单样本速率偏低，按样本数滑动会把峰值样本过早挤出，导致带宽
///          估计单调下降（实测启动 214kB/s 两个 RTT 内掉到 56kB/s，随后
///          cwnd 被 BDP 拖到 9 包）。
class BbrBandwidthEstimator final {
   public:
    explicit BbrBandwidthEstimator(std::size_t window = 10);

    /// @brief 加入样本（字节 + 间隔 + 当前时刻）。间隔/字节为 0 时忽略。
    void AddSample(std::uint64_t delivered_bytes, std::uint64_t interval_ms,
                   std::uint64_t now_ms);

    /// @brief 滑窗内最大速率（字节/秒）。
    std::uint64_t max_bytes_per_sec() const noexcept {
        return m_max_bps;
    }
    /// @brief 最近一次样本速率（字节/秒）。
    std::uint64_t latest_bytes_per_sec() const noexcept {
        return m_latest_bps;
    }
    /// @brief 是否已有样本。
    bool has_sample() const noexcept {
        return !m_window.empty();
    }
    std::size_t window() const noexcept {
        return m_window_ms;
    }
    /// @brief 清空样本（近似于连接重建立）。
    void Reset() noexcept;

    /// @brief 窗口内样本数上限。压测的紧凑循环里时间戳几乎不推进，仅靠
    ///        时间窗口会让 deque 无限增长，max() 退化成 O(n²)（实测 1e7 帧
    ///        压测在 300 万帧时从 2.6 Mframes/s 掉到 0.01 Mframes/s）。
    static constexpr std::size_t kMaxSamples = 4096;

   private:
    void RecomputeMax() noexcept;
    void DropOldSamples(std::uint64_t now_ms) noexcept;

    std::uint64_t m_window_ms;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> m_window;  ///< (now_ms, rate)
    std::uint64_t m_latest_bps = 0;
    std::uint64_t m_max_bps = 0;
};

/// @brief 令牌桶 pacing 器。
/// @details 令牌上限取 max(2×MSS, rate×burst_ms/1000)，保证单帧一定能发、
///          又不会把长时间空闲攒出的额度一次性倾泻（那样 pacing 就失效了）。
class Pacer final {
   public:
    Pacer() = default;

    /// @brief 配置 MSS 与突发窗口。
    void Configure(std::uint32_t mss_bytes, std::uint32_t burst_ms, bool enabled);

    /// @brief 设置发送速率（字节/秒）。速率变化时立即按新速率填充一次。
    void SetRate(std::uint64_t bytes_per_sec, std::uint64_t now_ms);

    /// @brief 按当前速率填充令牌。
    void OnTick(std::uint64_t now_ms);

    /// @brief 尝试消费 tokens；不足返回 false（调用方应稍后重试）。
    bool Consume(std::size_t bytes);

    /// @brief 当前可用令牌（字节）。
    std::size_t available_bytes() const noexcept;

    /// @brief 当前速率（字节/秒）。
    std::uint64_t rate_bytes_per_sec() const noexcept {
        return m_rate_bps;
    }
    bool enabled() const noexcept {
        return m_enabled;
    }

   private:
    void Refill(std::uint64_t now_ms) noexcept;

    std::uint32_t m_mss_bytes = 1200;
    std::uint32_t m_burst_ms = 5;
    bool m_enabled = true;
    bool m_initialized = false;
    std::uint64_t m_rate_bps = 0;
    std::uint64_t m_tokens = 0;
    std::uint64_t m_capacity = 2400;
    std::uint64_t m_last_refill_ms = 0;
};

/// @brief 一次 ACK 带给拥塞控制器的样本。
struct AckSample {
    std::uint64_t now_ms = 0;           ///< 当前时刻。
    std::uint32_t rtt_ms = 0;           ///< 该消息的 RTT；非重传消息才有效。
    std::uint64_t delivered_bytes = 0;  ///< 自该消息发出以来新确认的字节数。
    std::uint64_t send_elapsed_ms = 0;  ///< now - 该消息首次发送时刻。
    std::uint32_t holes = 0;            ///< ACK 位图覆盖范围内的未确认空洞数。
    bool retransmitted = false;         ///< 该消息是否被重传过（Karn 算法）。
    bool fec_recovered = false;         ///< 该消息是否由 FEC 恢复（非重传）。
};

/// @brief 一次丢包/超时事件带给拥塞控制器的样本。
struct LossSample {
    std::uint64_t now_ms = 0;
    std::uint32_t holes = 0;         ///< 本次检测到的空洞数。
    std::size_t inflight_bytes = 0;  ///< 当前在途字节数。
    std::uint32_t srtt_ms = 0;
    std::uint32_t min_rtt_ms = 0;
    bool timeout = false;  ///< true = PTO 超时；false = 快速重传。
};

/// @brief 拥塞控制器的可观测统计（报告与单测直接读取）。
struct CongestionStats {
    CongestionAlgorithm algorithm = CongestionAlgorithm::kNone;
    BbrState bbr_state = BbrState::kStartup;
    std::uint64_t bandwidth_bps = 0;  ///< 带宽估计（字节/秒）。
    std::uint64_t pacing_bps = 0;     ///< 当前 pacing 速率（字节/秒）。
    std::uint32_t cwnd_packets = 0;
    std::uint32_t ssthresh_packets = 0;
    std::uint32_t inflight_hi_packets = 0;
    std::uint32_t srtt_ms = 0;
    std::uint32_t min_rtt_ms = 0;
    std::uint32_t rto_ms = 0;
    std::uint64_t rounds = 0;             ///< 已推进的轮次数。
    std::uint64_t ack_events = 0;         ///< ACK 样本数。
    std::uint64_t loss_events = 0;        ///< 丢弃/超时事件数。
    std::uint64_t timeout_events = 0;     ///< 其中 PTO 超时数。
    std::uint64_t startup_exits = 0;
    std::uint64_t probe_bw_phases = 0;
    std::uint64_t probe_rtt_entries = 0;
    std::uint64_t cwnd_reductions = 0;    ///< 真正发生减窗的次数（NewReno 用）。
    std::uint64_t pacing_limited = 0;     ///< 被 pacing 额度挡住的发送尝试数。
    std::uint64_t packets_sent = 0;
    std::uint64_t bytes_sent = 0;

    // --- 兼容旧统计字段（通道与回归测试直接读取）-----------------------------
    std::uint32_t effective_window = 0;      ///< min(cwnd, 发送窗口)。
    std::uint64_t pacing_rate_bps = 0;       ///< 与 pacing_bps 同值（旧名）。
    std::uint64_t pacing_deferrals = 0;      ///< 与 pacing_limited 同值（旧名）。
    bool slow_start = true;                  ///< BBR 下等价于 bbr_state==kStartup。
    std::uint64_t packets_acked = 0;         ///< 累计确认包数。
    std::uint64_t random_loss_events = 0;    ///< 随机丢包事件（BBR 不降窗）。
    std::uint64_t congestion_loss_events = 0;///< 归因为拥塞的丢包事件。
    std::uint32_t min_cwnd_packets = 4;      ///< 窗口下限（旧统计字段）。
    bool pacing_enabled = true;              ///< 是否启用 pacing（旧统计字段）。
    std::uint32_t loss_rate_permille = 0;    ///< 丢包率千分比（旧统计字段）。
};

/// @brief 拥塞控制器（BBR / NewReno / None 三合一）。
/// @details 对外只暴露「样本进、门控出」的接口：
///          - `OnAck/OnLoss/OnTimeout` 更新模型；
///          - `CanSend(inflight_bytes)` + `ConsumePacing(bytes)` 决定是否放行；
///          - `RtoMs()` 给可靠通道算 PTO。
///          所有时间来自调用方传入的 now_ms，控制器内部不读时钟，便于单测。
class CongestionController final {
   public:
    /// @brief 兼容旧代码里 CongestionController::Config 的写法。
    using Config = CongestionConfig;

    /// @brief 旧版丢包上下文（通道层沿用）；内部转换为 LossSample。
    struct LossContext {
        std::uint64_t now_ms = 0;
        std::uint32_t srtt_ms = 0;
        std::uint32_t holes = 1;
        std::size_t inflight = 0;
    };

    explicit CongestionController(const CongestionConfig& config = CongestionConfig());

    /// @brief 换配置并重置全部状态（旧接口）。
    void SetConfig(const CongestionConfig& config);

    // --- 兼容旧控制器接口（包数口径，通道层沿用）-----------------------------
    bool enabled() const noexcept;
    std::size_t cwnd() const noexcept;
    std::size_t ssthresh() const noexcept;
    bool slow_start() const noexcept;
    std::uint32_t srtt_ms() const noexcept;
    std::uint64_t pacing_rate_bps() const noexcept;
    std::uint32_t min_rtt_ms() const noexcept;
    /// @brief 包数口径的放行检查（cwnd + 令牌桶）。
    bool AllowSend(std::uint64_t now_ms, std::size_t inflight_packets);
    /// @brief 记录放行的一个包并消耗 pacing 令牌。
    void OnPacketSent(std::uint64_t now_ms, std::size_t bytes);
    /// @brief 包数口径的 ACK 汇总（rtt_ms 传本次真实 RTT，0 表示无样本）。
    void OnAck(std::uint32_t acked_packets, std::uint32_t rtt_ms, std::uint64_t now_ms);
    void OnLoss(const LossContext& ctx);
    void OnTimeout(const LossContext& ctx);
    void NoteRetransmit() noexcept;
    /// @brief 统计快照；effective_window 用发送窗口夹逼（旧接口）。
    CongestionStats Snapshot(std::size_t send_window) const;

    /// @brief 处理一个 ACK 样本。
    void OnAck(const AckSample& sample);

    /// @brief 处理一次快速重传（SACK 空洞）事件。
    void OnLoss(const LossSample& sample);

    /// @brief 处理一次 PTO 超时事件。
    void OnTimeout(const LossSample& sample);

    /// @brief 记录一次发送（用于 round 内字节统计与 pacing 记账）。
    void OnSend(std::size_t bytes, std::uint64_t now_ms);

    /// @brief 定时驱动：pacer 填充令牌与 ProbeRTT 状态推进。
    void OnTick(std::uint64_t now_ms);

    /// @brief 当前是否允许发送（in-flight 未超过 cwnd）。
    bool CanSend(std::size_t inflight_bytes) const noexcept;

    /// @brief 消费 pacing 额度；返回 false 表示应暂缓发送。
    bool ConsumePacing(std::size_t bytes);

    /// @brief 当前 RTO（毫秒）。
    std::uint32_t RtoMs() const noexcept;
    /// @brief RTT 估计器（只读）。
    const BbrRttEstimator& rtt() const noexcept {
        return m_rtt;
    }
    /// @brief 当前算法。
    CongestionAlgorithm algorithm() const noexcept {
        return m_config.algorithm;
    }
    /// @brief 配置（只读）。
    const CongestionConfig& config() const noexcept {
        return m_config;
    }
    /// @brief 可观测统计。
    const CongestionStats& stats() const;
    /// @brief cwnd（包）。
    std::size_t cwnd_packets() const noexcept {
        return m_cwnd_packets;
    }
    /// @brief ssthresh（包，仅 NewReno 有意义）。
    std::size_t ssthresh_packets() const noexcept {
        return m_ssthresh_packets;
    }
    /// @brief pacing 速率（字节/秒）。
    std::uint64_t pacing_bytes_per_sec() const noexcept {
        return m_pacing_bps;
    }
    /// @brief 当前 BBR 相位（非 BBR 算法时为 kStartup）。
    BbrState bbr_state() const noexcept {
        return m_bbr_state;
    }

    /// @brief 连接重建时重置（保留配置）。
    void Reset(std::uint64_t now_ms);

   private:
    /// @brief 归一化配置（0 值夹逼、兼容字段映射）。
    void NormalizeConfig();

    // --- BBR 内部 -----------------------------------------------------------
    void BbrOnAck(const AckSample& sample);
    void BbrOnLoss(const LossSample& sample, bool timeout);
    void BbrAdvanceRound(std::uint64_t now_ms);
    void BbrUpdateModel(std::uint64_t now_ms);
    void BbrEnterProbeRtt(std::uint64_t now_ms);
    void BbrExitProbeRtt(std::uint64_t now_ms);
    std::uint64_t BdpBytes() const noexcept;
    double BbrPacingGain() const noexcept;
    double BbrCwndGain() const noexcept;

    // --- NewReno 内部 -------------------------------------------------------
    void RenoOnAck(const AckSample& sample);
    void RenoOnLoss(const LossSample& sample, bool timeout);

    /// @brief 无带宽样本时的起速：初始窗口 / 初始 RTT。
    std::uint64_t InitialPacingRate() const noexcept;

    void RefreshStats() const;

    CongestionConfig m_config;
    BbrRttEstimator m_rtt;
    BbrBandwidthEstimator m_bandwidth;
    Pacer m_pacer;

    // 通用状态。
    std::size_t m_cwnd_packets = 64;
    std::size_t m_ssthresh_packets = 128;
    std::uint64_t m_pacing_bps = 0;
    std::size_t m_inflight_hi_packets = 0;
    bool m_has_inflight_hi = false;

    // BBR 状态。
    BbrState m_bbr_state = BbrState::kStartup;
    std::uint64_t m_round_start_ms = 0;
    std::uint64_t m_rounds = 0;
    std::uint64_t m_bw_at_round_start = 0;
    std::uint32_t m_full_bw_rounds = 0;
    std::uint32_t m_cycle_index = 0;
    /// startup 阶段的独立窗口目标：每轮 ×1.25 增长。为什么不能只用
    /// `gain×BDP`：带宽估计来自「确认速率」，而确认速率本身受 pacing 限制，
    /// 只按 BDP 定窗会形成「估计低 → 窗小 → 发得慢 → 估计更低」的自锁。
    std::size_t m_startup_cwnd_target = 64;
    std::uint64_t m_probe_rtt_entered_ms = 0;
    std::uint64_t m_last_probe_rtt_ms = 0;
    bool m_probe_rtt_active = false;
    std::uint64_t m_round_bytes_sent = 0;
    std::uint64_t m_round_bytes_acked = 0;
    double m_bw_growth = 0.0;

    // NewReno 状态。
    std::size_t m_reno_ssthresh = 128;
    double m_reno_ack_accum = 0.0;
    std::uint64_t m_last_reduction_ms = 0;

    // 统计。
    mutable CongestionStats m_stats;
    mutable bool m_stats_dirty = true;
};

}  // namespace go2cpp::bento
