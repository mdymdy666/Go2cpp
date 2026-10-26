#pragma once

/**
 * @file channel.hpp
 * @brief Bento 的三类通道：可靠有序、不可靠 DropStale、视频帧分片。
 * @details 三个通道共享同一套 12B 帧头与可选 ACK 段，但可靠性语义不同：
 *
 *          - ReliableChannel（STREAM）：发送窗口 256，动态 PTO（RFC 9002
 *            的 SRTT/RTTVAR：srtt + 4*rttvar + max_ack_delay，默认夹在
 *            [200ms, 2s]，每次超时重传再 ×2 退避），3 个重复 ACK 或
 *            SACK 空洞触发快速重传。接收端按 seq 去重、乱序缓存、严格
 *            按序交付；dep_seq 未满足时**不 ACK** 并丢弃该帧，等发送端
 *            重传，从而在不引入乱序的前提下实现因果等待。单条消息超过
 *            1300B 时自动切分（kFlagFragmented + 8B 子头），接收端按
 *            frame_id 重组，缺片超时丢弃。
 *          - UnreliableChannel（DATAGRAM）：每通道独立单调 seq；接收端只接受
 *            seq > applied_seq，旧包一律 DropStale。带 dep_seq 的帧在依赖
 *            未满足时进入 Relation 的 pending 队列；依赖满足后若自身 seq
 *            已被更新的帧越过，则按 DropStale 丢弃（保证 0 乱序）。
 *          - VideoChannel（VIDEO）：帧拆成 <=1200B 的分片，每片带
 *            frame_id/frag_idx/frag_cnt；KEY 标志在帧头 flags。只保留最新
 *            若干帧的重组状态，缺片超时丢弃，旧 frame_id 直接丢弃。
 *
 *          所有通道都不拥有 socket；发送通过 FrameSink 把**已经编码好的
 *          完整帧**交给 Link，接收通过 DeliverFn 把载荷交给上层。这样
 *          编解码只在一处，测试也可以直接注入构造好的帧。
 *
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "go2cpp/bento/net/bandwidth.hpp"
#include "go2cpp/bento/net/congestion.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/bento/net/relation.hpp"

namespace go2cpp::bento {

/// @brief 非阻塞发送结果。区分「暂时不可发」与「永久错误」是工业 API
///        的基本要求：调用方按 WouldBlock 重试，按 Error 放弃/报错。
enum class SendStatus : std::uint8_t {
    kOk = 0,          ///< 已入队并尝试发送。
    kWouldBlock = 1, ///< 发送窗口/流控窗口暂满，稍后重试。
    kError = 2,       ///< 参数非法/超长/通道不可用。
};

/// @brief 发送结果名（日志/测试用）。
const char* SendStatusName(SendStatus status) noexcept;

/// @brief 可靠通道的流控计数器（RFC 9000 MAX_DATA 语义）。
struct FlowControlStats {
    std::uint64_t local_max_data = 0;  ///< 本端通告的接收上限。
    std::uint64_t peer_max_data = 0;   ///< 对端通告的发送上限。
    std::uint64_t tx_bytes_total = 0;  ///< 已入队的线上载荷字节。
    std::uint64_t rx_bytes_total = 0;  ///< 已接收的线上载荷字节。
    std::uint64_t max_data_sent = 0;
    std::uint64_t max_data_received = 0;
    std::uint64_t blocked = 0;     ///< 流控阻塞次数。
    std::uint64_t violations = 0;  ///< 对端超额发送被拒次数。
};

/// @brief RFC 9002 风格的 SRTT/RTTVAR 估计器与动态 PTO 计算。
/// @details 首样本取 srtt=rtt、rttvar=rtt/2；之后按
///          rttvar = 3/4*rttvar + 1/4*|srtt-rtt|、srtt = 7/8*srtt + 1/8*rtt
///          做整数近似（除法向下取整）。PTO 公式：
///          srtt + 4*rttvar + max_ack_delay，并夹在 [min_pto, max_pto]。
///          首样本之前使用 RFC 9002 的初始 RTT（333ms），使 400ms RTT
///          的链路在第一个 ACK 到达前不会因固定 200ms 而必然重传——
///          这正是旧实现拿不到干净 RTT 样本的根因。
class RttEstimator final {
   public:
    struct Config {
        std::uint32_t min_pto_ms = 200;      ///< PTO 下限（默认 200ms）。
        std::uint32_t max_pto_ms = 2000;     ///< PTO 上限（默认 2s）。
        /// 对端最大 ACK 延迟补偿。RFC 9002 建议 QUIC 默认 25ms；
        /// 本实现 ACK 间隔 20ms，留 25ms 可避免 PTO 与 ACK 到达竞争。
        std::uint32_t max_ack_delay_ms = 25;
        std::uint32_t initial_rtt_ms = 333;  ///< 首样本前的初始 RTT。
        /// >0 时禁用自适应，固定使用该基础 PTO；用于对比旧行为/回归实验。
        std::uint32_t fixed_pto_ms = 0;
    };

    RttEstimator() noexcept;
    explicit RttEstimator(Config config) noexcept;

    /// @brief 更新配置；尚未采样时同步重置 srtt/rttvar 为初始值。
    void SetConfig(Config config) noexcept;
    const Config& config() const noexcept {
        return m_config;
    }

    /// @brief 丢弃全部样本，回到初始状态。
    void Reset() noexcept;

    /// @brief 注入一个 RTT 样本（毫秒），更新 srtt/rttvar。
    void OnSample(std::uint32_t rtt_ms) noexcept;

    bool has_sample() const noexcept {
        return m_has_sample;
    }
    std::uint32_t smoothed_rtt_ms() const noexcept {
        return m_srtt_ms;
    }
    std::uint32_t rttvar_ms() const noexcept {
        return m_rttvar_ms;
    }
    /// @brief 未退避的基础 PTO（毫秒），已按配置夹逼。
    std::uint32_t base_pto_ms() const noexcept;
    /// @brief 第 n 次超时重传使用的 PTO：base * 2^n，夹在 max_pto。
    std::uint32_t BackoffPtoMs(std::uint32_t retransmits) const noexcept;

   private:
    Config m_config;
    std::uint32_t m_srtt_ms = 333;
    std::uint32_t m_rttvar_ms = 166;
    bool m_has_sample = false;
};

/// STREAM 单条消息的最大分片数（与发送窗口同量级，防止单条消息饿死窗口）。
inline constexpr std::size_t kMaxStreamFragments = 256;

/// @brief STREAM 单条应用消息上限：分片数 × 单分片数据量。
inline constexpr std::size_t kMaxStreamMessageBytes =
    kMaxStreamFragments * kStreamFragmentBytes;

/// @brief STREAM 分片重组器：按 frame_id 聚合，乱序容忍，缺片超时丢弃。
/// @details 与视频通道不同，这里不要求 frame_id 单调：可靠层已保证
///          分片按 seq 有序到达，但重组器仍允许任意顺序注入（便于单测
///          与未来多路复用）。非末片必须恰好 kStreamFragmentBytes 字节，
///          否则无法按固定偏移写入，直接判非法。
class StreamReassembler final {
   public:
    static constexpr std::uint64_t kDefaultTimeoutMs = 10000;
    static constexpr std::size_t kDefaultMaxActive = 16;

    struct Config {
        std::uint64_t timeout_ms = kDefaultTimeoutMs;  ///< 半成品超时。
        std::size_t max_active = kDefaultMaxActive;    ///< 并发重组帧上限。
    };

    /// @brief AddFragment 的结果，供调用方更新统计。
    enum class Result : std::uint8_t {
        kBuffered = 0,   ///< 已收下，消息尚未完整。
        kDelivered = 1,  ///< 收下并完成重组，已同步回调交付。
        kDuplicate = 2,  ///< 重复分片，忽略。
        kInvalid = 3,    ///< 子头/长度非法，忽略。
    };

    struct Stats {
        std::uint64_t frames_started = 0;     ///< 建立过重组状态的消息数。
        std::uint64_t frames_delivered = 0;   ///< 完整重组并交付的消息数。
        std::uint64_t fragments_received = 0; ///< 收下的分片数（不含非法）。
        std::uint64_t fragments_duplicate = 0;
        std::uint64_t fragments_invalid = 0;
        std::uint64_t frames_timed_out = 0;   ///< 缺片超时丢弃数。
        std::uint64_t frames_evicted = 0;     ///< 超过 max_active 被挤掉数。
    };

    using DeliverFn =
        std::function<void(std::uint32_t frame_id, const std::uint8_t* data,
                           std::size_t bytes)>;

    StreamReassembler() noexcept;
    explicit StreamReassembler(Config config) noexcept;

    void SetConfig(Config config) noexcept;
    void SetDeliver(DeliverFn deliver) {
        m_deliver = std::move(deliver);
    }

    /// @brief 注入一个 STREAM 分片。
    /// @param now_ms 调用方时钟，用于超时判定。
    Result AddFragment(std::uint32_t frame_id, std::uint16_t frag_index,
                       std::uint16_t frag_count, const std::uint8_t* data,
                       std::size_t bytes, std::uint64_t now_ms);

    /// @brief 丢弃超过 timeout_ms 未更新的半成品。返回丢弃数量。
    std::size_t OnTimer(std::uint64_t now_ms);

    const Stats& stats() const noexcept {
        return m_stats;
    }
    std::size_t active() const noexcept {
        return m_active.size();
    }
    void Reset() noexcept;

   private:
    struct Assembly {
        std::uint16_t frag_count = 0;
        std::uint16_t received_count = 0;
        std::uint16_t last_fragment_bytes = 0;
        std::uint64_t last_update_ms = 0;
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> received;
    };

    void EvictOne();

    Config m_config;
    DeliverFn m_deliver;
    std::map<std::uint32_t, Assembly> m_active;
    Stats m_stats;
};

/// @brief 可靠有序通道（STREAM）。
class ReliableChannel final {
   public:
    /// 发送窗口：未确认消息上限，同时也是接收端乱序缓存窗口的上界来源。
    static constexpr std::size_t kSendWindow = 256;
    /// 接收窗口：距 expected 超过该值的 seq 视为过旧/不可信，直接丢弃。
    static constexpr std::size_t kReceiveWindow = 512;
    /// 动态 PTO 的下限（毫秒），也是无样本时最小等待。
    static constexpr std::uint32_t kInitialPtoMs = 200;
    /// PTO 指数退避上限（毫秒）。
    static constexpr std::uint32_t kMaxPtoMs = 2000;
    /// 每个事件循环 tick 最多补发的超时消息数：避免一次 RunOnce 内突发
    /// 上百帧；不是每个 PTO 周期的硬上限（后续 tick 会按 seq 继续补）。
    static constexpr std::size_t kPtoRetransmitBatch = 4;
    /// 单次快速重传最多并行补几个 SACK 空洞（32 位 SACK 窗口内）。
    static constexpr std::size_t kFastRetransmitBatch = 8;

    /// @brief 可观测计数器，测试与报告直接读取。
    struct Stats {
        std::uint64_t messages_sent = 0;        ///< 接受入队的逻辑消息数。
        std::uint64_t messages_delivered = 0;   ///< 按序交付给上层的消息数。
        std::uint64_t acks_applied = 0;         ///< 被确认并从 inflight 移除的消息数。
        std::uint64_t retransmits = 0;          ///< 重传总次数（含快速重传）。
        std::uint64_t fast_retransmits = 0;     ///< 由重复 ACK/SACK 空洞触发。
        std::uint64_t fragments_sent = 0;       ///< 发出的 STREAM 分片帧数。
        std::uint64_t fragments_received = 0;   ///< 收到的 STREAM 分片帧数。
        std::uint64_t fragments_duplicate = 0;  ///< 重组层重复分片数。
        std::uint64_t fragments_invalid = 0;    ///< 重组层非法分片数。
        std::uint64_t messages_reassembled = 0; ///< 分片消息完整重组数。
        std::uint64_t reassembly_timeouts = 0;  ///< 缺片超时丢弃的消息数。
        std::uint64_t duplicates_dropped = 0;   ///< 重复帧丢弃数。
        std::uint64_t stale_dropped = 0;        ///< 过旧/超窗帧丢弃数。
        std::uint64_t send_failures = 0;        ///< FrameSink 拒绝或编码失败次数。
        std::uint64_t dependency_deferred = 0;  ///< dep_seq 未满足而拒绝的帧数。
        std::uint64_t invalid_frames = 0;       ///< 通道/序号非法的帧数。
        std::uint32_t last_rtt_ms = 0;          ///< 最近一次未被重传的消息 RTT 样本。
        std::uint32_t srtt_ms = 0;              ///< 平滑 RTT。
        std::uint32_t rttvar_ms = 0;            ///< RTT 方差。
        std::uint32_t pto_ms = 0;               ///< 当前基础 PTO（未退避）。
        std::size_t inflight = 0;  ///< 当前未确认消息数（OnTimer 后刷新）。
        std::size_t unsent = 0;    ///< 尚未发出的排队消息数。

        // 拥塞控制 / pacing 可观测字段（P2-5）。
        std::uint32_t cwnd_packets = 0;        ///< 当前拥塞窗口（包）。
        std::uint32_t ssthresh_packets = 0;    ///< 慢启动阈值（包）。
        std::uint32_t effective_cwnd = 0;      ///< min(cwnd, 发送窗口)。
        std::uint64_t pacing_rate_bps = 0;     ///< pacing 速率（bytes/s）。
        std::uint64_t pacing_deferrals = 0;    ///< 因令牌不足被延后的次数。
        std::uint64_t loss_events = 0;         ///< 快速重传/丢包降窗次数。
        std::uint64_t timeout_loss_events = 0; ///< PTO 超时降窗次数。
        std::uint64_t congestion_bytes_sent = 0;  ///< 拥塞层放行的字节数。
        bool slow_start = true;                ///< 当前是否处于慢启动。

        // 流控（P1）
        std::uint64_t flow_control_blocked = 0;     ///< 流控窗口满导致 WouldBlock。
        std::uint64_t send_window_blocked = 0;      ///< 发送窗口满导致 WouldBlock。
        std::uint64_t flow_control_violations = 0;  ///< 对端超窗发送被拒。
        std::uint64_t max_data_sent = 0;
        std::uint64_t max_data_received = 0;
    };

    /// 交付回调：seq + 载荷。载荷只在回调期间有效。
    using DeliverFn =
        std::function<void(std::uint32_t seq, const std::uint8_t* data, std::size_t bytes)>;
    /// 发送回调：完整帧字节。返回 false 表示 UDP 发送失败，通道会重试。
    using FrameSink = std::function<bool(const std::uint8_t* data, std::size_t bytes)>;

    /// @brief 可靠通道配置。
    struct Config {
        RttEstimator::Config rtt;                 ///< SRTT/RTTVAR/PTO 参数。
        bool fast_retransmit = true;              ///< 是否启用快速重传。
        std::uint32_t fast_retransmit_threshold = 3;  ///< 重复 ACK 门限。
        std::uint64_t reassembly_timeout_ms = StreamReassembler::kDefaultTimeoutMs;
        std::size_t max_reassembly = StreamReassembler::kDefaultMaxActive;
        /// 拥塞控制 + pacing（默认开启；enabled=false 可复现旧行为）。
        CongestionController::Config congestion;
        /// 流控（RFC 9000 MAX_DATA）：本端接收窗口，也是初始通告上限。
        /// 双方必须配置一致的窗口；默认 1 MiB 足以容纳单条最大消息。
        std::uint32_t initial_max_data = 1U << 20;
        /// 对端初始发送窗口；0 表示与 initial_max_data 相同。
        std::uint32_t peer_initial_max_data = 0;
        /// MAX_DATA 丢失后的重发周期（毫秒）；0 表示只在窗口推进时发送。
        std::uint32_t max_data_refresh_ms = 200;
        /// 交付速率/丢包率估计参数（ABR 输入）。
        BandwidthEstimator::Config bandwidth;
    };

    /// @brief 可注入时钟（毫秒），默认 NowMs()；测试用假时钟做确定性时序。
    using ClockFn = std::function<std::uint64_t()>;

    /// @param channel_id 通道 id，写入帧头。
    /// @param relation 因果水位，用于检查 dep_seq；可为 nullptr（关闭因果检查）。
    /// @param deliver 有序交付回调。
    /// @param config 可靠通道配置。
    /// @param clock 可选时钟；为空时使用 NowMs()。
    ReliableChannel(std::uint8_t channel_id, Relation* relation, DeliverFn deliver);
    ReliableChannel(std::uint8_t channel_id, Relation* relation, DeliverFn deliver,
                    Config config);
    ReliableChannel(std::uint8_t channel_id, Relation* relation, DeliverFn deliver,
                    Config config, ClockFn clock);
    ~ReliableChannel() = default;

    ReliableChannel(const ReliableChannel&) = delete;
    ReliableChannel& operator=(const ReliableChannel&) = delete;

    /// @brief 设置发送函数（通常绑定到 Link::SendBuiltFrame）。
    void SetFrameSink(FrameSink sink) {
        m_sink = std::move(sink);
    }

    /// @brief 入队一条可靠消息并尽量立即发送。
    /// @param bytes 载荷长度，必须 <= kMaxStreamMessageBytes；>1300B 自动分片。
    /// @param dep_seq 依赖的不可靠 seq；0 表示无依赖。
    /// @return 入队成功返回 true；超长、发送窗口或流控窗口已满返回 false。
    bool Send(const std::uint8_t* data, std::size_t bytes, std::uint32_t dep_seq = 0);

    /// @brief 非阻塞发送：返回 WouldBlock 时调用方应等待 MAX_DATA 后重试。
    SendStatus TrySend(const std::uint8_t* data, std::size_t bytes,
                       std::uint32_t dep_seq = 0);

    /// @brief 应用对端 MAX_DATA：更新发送上限并补发排队消息。
    /// @note 上限单调递增，较小的值只计数不生效。
    void OnMaxData(std::uint64_t limit) noexcept;

    /// @brief 是否应发送 MAX_DATA（窗口推进或重发周期到）。
    /// @param limit 输出当前通告上限（非空时写入）。
    bool PendingMaxData(std::uint64_t* limit, std::uint64_t now_ms) const noexcept;

    /// @brief 记录一次 MAX_DATA 已发出。
    void OnMaxDataSent(std::uint64_t limit, std::uint64_t now_ms) noexcept;

    /// @brief 流控计数器快照。
    FlowControlStats flow_control() const noexcept;

    /// @brief 交付速率/丢包率快照（ABR 输入）。
    BandwidthStats bandwidth_stats() const noexcept;

    /// @brief 处理收到的 STREAM 帧（内部先应用捎带的 ACK）。
    void OnFrame(const FrameHeader& header, const std::uint8_t* payload, std::size_t bytes,
                 const AckState* piggyback_ack);

    /// @brief 应用累积 ACK + SACK 位图。
    void OnAck(const AckState& ack);

    /// @brief 驱动超时重传与发送队列。Link 每次事件循环调用。
    void OnTimer(std::uint64_t now_ms);

    /// @brief 生成当前接收状态（ack_seq = 下一个期望 seq）。
    bool FillAck(AckState* out) const noexcept;

    /// @brief 是否有尚未发出的 ACK 状态（Link 据此发送独立 ACK 帧）。
    bool HasPendingAck() const noexcept {
        return m_pending_ack;
    }

    /// @brief 标记 ACK 已随帧发出（独立 ACK 或数据帧捎带）。
    void MarkAckSent() noexcept {
        m_pending_ack = false;
    }

    std::uint8_t channel_id() const noexcept {
        return m_channel_id;
    }
    std::uint32_t next_rx_seq() const noexcept {
        return m_next_rx_seq;
    }
    std::uint32_t next_tx_seq() const noexcept {
        return m_next_tx_seq;
    }
    std::size_t inflight() const noexcept {
        return m_inflight.size();
    }
    std::size_t unsent() const noexcept {
        return m_unsent.size();
    }
    const Stats& stats() const noexcept {
        return m_stats;
    }
    const RttEstimator& rtt_estimator() const noexcept {
        return m_rtt;
    }
    const BandwidthEstimator& bandwidth_estimator() const noexcept {
        return m_bandwidth;
    }
    const CongestionController& congestion() const noexcept {
        return m_cc;
    }
    /// @brief 拥塞控制统计快照（cwnd/ssthresh/pacing/丢包/重传）。
    CongestionStats congestion_stats() const noexcept {
        return m_cc.Snapshot(kSendWindow);
    }
    /// @brief 当前基础 PTO（未针对单条消息做退避）。
    std::uint32_t base_pto_ms() const noexcept {
        return m_rtt.base_pto_ms();
    }
    const StreamReassembler& reassembler() const noexcept {
        return m_reassembler;
    }

   private:
    struct Outgoing {
        std::uint32_t seq = 0;
        std::uint32_t dep_seq = 0;
        std::vector<std::uint8_t> payload;
        std::uint64_t first_send_ms = 0;
        std::uint64_t last_send_ms = 0;
        std::uint32_t retransmits = 0;
        bool ever_retransmitted = false;
        bool fragmented = false;  ///< 载荷是否带 8B STREAM 分片子头。
        /// 该消息曾被 SACK 空洞堵在有序交付之后：它的累积 ACK 延迟包含
        /// 对端等待空洞的时间，不能作为 RTT 样本（无时间戳回显时的折中）。
        bool hol_blocked = false;
    };
    struct Received {
        bool fragmented = false;
        std::vector<std::uint8_t> payload;
    };

    bool SendNow(Outgoing& message, bool retransmit, std::uint64_t now_ms);
    void Pump(std::uint64_t now_ms);
    void Acknowledge(Outgoing& message, std::uint64_t now_ms);
    void AdvanceDelivery();
    std::uint64_t Now() const noexcept;
    bool TryFastRetransmit(const AckState& ack, bool ack_progressed,
                           std::size_t new_sack, std::uint64_t now_ms);
    void RefreshRttStats() noexcept;
    void RefreshCongestionStats() noexcept;

    const std::uint8_t m_channel_id;
    Relation* const m_relation;
    const DeliverFn m_deliver;
    FrameSink m_sink;
    Config m_config;
    ClockFn m_clock;
    RttEstimator m_rtt;
    CongestionController m_cc;
    BandwidthEstimator m_bandwidth;
    StreamReassembler m_reassembler;

    // ---- 流控（RFC 9000 MAX_DATA） ----
    std::uint64_t m_peer_max_data = 0;   ///< 对端允许的累计字节偏移。
    std::uint64_t m_local_max_data = 0;  ///< 本端承诺的累计字节偏移。
    std::uint64_t m_tx_bytes_total = 0;  ///< 已入队的线上载荷字节。
    std::uint64_t m_rx_bytes_total = 0;  ///< 已接收的线上载荷字节。
    std::uint64_t m_last_advertised_max_data = 0;
    std::uint64_t m_last_max_data_sent_ms = 0;

    std::uint32_t m_next_tx_seq = 1;
    std::uint32_t m_next_rx_seq = 1;
    std::uint32_t m_next_frame_id = 1;
    std::uint32_t m_highest_ack = 0;   ///< 收到过的最大累积 ack_seq。
    std::uint32_t m_dup_acks = 0;      ///< 连续重复 ACK 计数。
    std::size_t m_sack_evidence = 0;   ///< 本轮累积确认前进后新增的 SACK 数。
    bool m_in_fast_recovery = false;   ///< 快速重传后等待 ACK 前进。
    std::set<std::uint32_t> m_sacked;  ///< SACK 位图已知收到的 seq。
    std::uint32_t m_delivery_first_seq = 0;  ///< 重组交付时的首分片 seq。
    std::deque<Outgoing> m_unsent;    ///< 已分配 seq、尚未发出（按 seq 升序）。
    std::deque<Outgoing> m_inflight;  ///< 已发出未确认（按 seq 升序）。
    std::map<std::uint32_t, Received> m_rx_buffer;
    std::set<std::uint32_t> m_rx_seen;
    bool m_pending_ack = false;
    bool m_pto_recovery = false;  ///< 已为当前 PTO 丢失事件降过窗，等 ACK 前进清除。
    Stats m_stats;
};

/// @brief 不可靠通道（DATAGRAM），接收端 DropStale。
class UnreliableChannel final {
   public:
    /// @brief 可观测计数器。
    struct Stats {
        std::uint64_t messages_sent = 0;
        std::uint64_t messages_delivered = 0;
        std::uint64_t stale_dropped = 0;  ///< seq <= applied_seq 的旧包/重复包。
        std::uint64_t invalid_frames = 0;
        std::uint64_t dependency_deferred = 0;  ///< 依赖未满足而挂起的帧数。
        std::uint64_t applied_seq = 0;          ///< 最近采纳的 seq。
    };

    using DeliverFn =
        std::function<void(std::uint32_t seq, const std::uint8_t* data, std::size_t bytes)>;
    using FrameSink = std::function<bool(const std::uint8_t* data, std::size_t bytes)>;

    /// @param relation 因果水位；可为 nullptr。
    UnreliableChannel(std::uint8_t channel_id, Relation* relation, DeliverFn deliver);
    ~UnreliableChannel() = default;

    UnreliableChannel(const UnreliableChannel&) = delete;
    UnreliableChannel& operator=(const UnreliableChannel&) = delete;

    void SetFrameSink(FrameSink sink) {
        m_sink = std::move(sink);
    }

    /// @brief 发送一条不可靠消息。seq 每通道独立单调递增（从 1 开始）。
    /// @return 入队并尝试发送的调用成功返回 true；超长/编码失败返回 false。
    /// @note 返回 true 不代表对端收到；UDP 丢包由上层决定是否重发。
    bool Send(const std::uint8_t* data, std::size_t bytes, std::uint32_t dep_seq = 0);

    /// @brief 处理收到的 DATAGRAM 帧。
    void OnFrame(const FrameHeader& header, const std::uint8_t* payload, std::size_t bytes);

    /// @brief 处理 BARRIER：把 applied_seq 抬到 barrier 以上。
    void OnBarrier(std::uint32_t barrier_seq) noexcept;

    std::uint32_t applied_seq() const noexcept {
        return m_applied_seq;
    }
    std::uint8_t channel_id() const noexcept {
        return m_channel_id;
    }
    const Stats& stats() const noexcept {
        return m_stats;
    }

   private:
    void Accept(std::uint32_t seq, std::vector<std::uint8_t> payload);
    void FlushPending();

    const std::uint8_t m_channel_id;
    Relation* const m_relation;
    const DeliverFn m_deliver;
    FrameSink m_sink;

    std::uint32_t m_next_tx_seq = 1;
    std::uint32_t m_applied_seq = 0;
    Stats m_stats;
};

/// @brief 视频通道（VIDEO）：帧分片与重组。
class VideoChannel final {
   public:
    /// 每片最大数据量（不含 8B 视频子头）。
    static constexpr std::size_t kFragmentBytes = 1200;
    /// 单帧最大分片数（同时把帧大小限制在 ~300KB）。
    static constexpr std::size_t kMaxFragments = 256;
    /// 同时保留的重组帧数上限。
    static constexpr std::size_t kMaxReassembly = 4;
    /// 半成品重组状态超时（毫秒）。
    static constexpr std::uint64_t kReassemblyTimeoutMs = 1000;
    /// XOR FEC 单组最大数据片数（group_size=4/8 是推荐值）。
    static constexpr std::size_t kMaxFecGroupSize = 8;
    /// FEC 扩展头：group_start u16 + group_count u16 + max_len u16。
    static constexpr std::size_t kFecExtensionBytes = 6;

    /// @brief 视频通道配置。FEC 默认关闭以保持与既有线格式/测试兼容。
    struct Config {
        /// 0 = 关闭；2..8 = 每 N 个数据片生成 1 个 XOR 校验片。
        std::uint16_t fec_group_size = 0;
    };

    /// @brief 可观测计数器。
    struct Stats {
        std::uint64_t frames_sent = 0;
        std::uint64_t fragments_sent = 0;  ///< 数据分片数（不含 FEC 校验片）。
        std::uint64_t frames_delivered = 0;
        std::uint64_t fragments_duplicate = 0;
        std::uint64_t fragments_stale = 0;  ///< 旧 frame_id 或已交付帧的分片。
        std::uint64_t frames_incomplete_dropped = 0;
        std::uint64_t fragments_invalid = 0;
        std::uint64_t fragments_send_failed = 0;
        std::uint64_t fec_parity_sent = 0;
        std::uint64_t fec_parity_send_failed = 0;
        std::uint64_t fec_parity_received = 0;
        std::uint64_t fec_recovered = 0;  ///< 用 1 个校验片恢复出的数据片数。
    };

    using FrameSink = std::function<bool(const std::uint8_t* data, std::size_t bytes)>;
    using DeliverFn = std::function<void(std::uint32_t frame_id, bool key, const std::uint8_t* data,
                                         std::size_t bytes)>;

    VideoChannel(FrameSink sink, DeliverFn deliver);
    VideoChannel(FrameSink sink, DeliverFn deliver, Config config);
    ~VideoChannel() = default;

    VideoChannel(const VideoChannel&) = delete;
    VideoChannel& operator=(const VideoChannel&) = delete;

    /// @brief 把一帧拆成 <=1200B 的分片并发送；配置了 FEC 时追加校验片。
    /// @param key 关键帧标记，写入每片的帧头 flags。
    /// @return 参数合法且已尝试发送返回 true；超过 kMaxFragments 返回 false。
    bool SendFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes);

    /// @brief 处理收到的 VIDEO 数据分片或 FEC 校验片。
    void OnFrame(const FrameHeader& header, const std::uint8_t* payload, std::size_t bytes);

    /// @brief 清理超时的半成品重组状态（含其 FEC 状态）。
    void OnTimer(std::uint64_t now_ms);

    const Stats& stats() const noexcept {
        return m_stats;
    }
    std::uint32_t delivered_frame_id() const noexcept {
        return m_delivered_frame_id;
    }
    std::uint16_t fec_group_size() const noexcept {
        return m_config.fec_group_size;
    }

   private:
    struct Reassembly {
        std::uint16_t frag_count = 0;
        std::uint16_t received_count = 0;
        std::uint16_t last_fragment_bytes = 0;
        bool key = false;
        std::uint64_t last_update_ms = 0;
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> received;
    };

    /// @brief 一组的 XOR 校验状态。parity 覆盖 [group_start, group_start+count)。
    struct FecBlock {
        std::uint16_t group_start = 0;
        std::uint16_t group_count = 0;
        std::uint16_t max_len = 0;
        std::vector<std::uint16_t> lengths;
        std::vector<std::uint8_t> parity;
    };

    bool SendFecParity(std::uint32_t frame_id, bool key, std::uint16_t group_size,
                       std::size_t frag_count, const std::uint8_t* data, std::size_t bytes,
                       std::uint16_t group_start);
    void OnFecParity(const FrameHeader& header, const std::uint8_t* payload, std::size_t bytes);
    bool TryRecover(std::uint32_t frame_id, Reassembly& reassembly);
    void DeliverFrame(std::uint32_t frame_id, Reassembly& reassembly);
    void DropReassembly(std::map<std::uint32_t, Reassembly>::iterator it);

    FrameSink m_sink;
    DeliverFn m_deliver;
    Config m_config;
    std::map<std::uint32_t, Reassembly> m_active;
    /// frame_id -> (group_start -> FEC block)；与 m_active 同生命周期。
    std::map<std::uint32_t, std::map<std::uint16_t, FecBlock>> m_fec;
    std::uint32_t m_delivered_frame_id = 0;
    std::uint32_t m_max_active_frame_id = 0;
    std::uint32_t m_next_tx_seq = 1;
    Stats m_stats;
};

}  // namespace go2cpp::bento
