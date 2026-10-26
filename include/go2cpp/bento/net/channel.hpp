#pragma once

/**
 * @file channel.hpp
 * @brief Bento 的三类通道：可靠有序、不可靠 DropStale、视频帧分片。
 * @details 三个通道共享同一套 12B 帧头与可选 ACK 段，但可靠性语义不同：
 *
 *          - ReliableChannel（STREAM）：发送窗口 256，PTO 200ms 起步、
 *            每次重传 ×2、上限 2s（沿用 yojimbo reliable 的退避思路，
 *            但重新实现为 32 位 seq + 累积 ACK + 32 位 SACK）。接收端按
 *            seq 去重、乱序缓存、严格按序交付；dep_seq 未满足时**不 ACK**
 *            并丢弃该帧，等发送端重传，从而在不引入乱序的前提下实现因果等待。
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

#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/bento/net/relation.hpp"

namespace go2cpp::bento {

/// @brief 可靠有序通道（STREAM）。
class ReliableChannel final {
   public:
    /// 发送窗口：未确认消息上限，同时也是接收端乱序缓存窗口的上界来源。
    static constexpr std::size_t kSendWindow = 256;
    /// 接收窗口：距 expected 超过该值的 seq 视为过旧/不可信，直接丢弃。
    static constexpr std::size_t kReceiveWindow = 512;
    /// 初始 PTO（毫秒）。
    static constexpr std::uint32_t kInitialPtoMs = 200;
    /// PTO 指数退避上限（毫秒）。
    static constexpr std::uint32_t kMaxPtoMs = 2000;

    /// @brief 可观测计数器，测试与报告直接读取。
    struct Stats {
        std::uint64_t messages_sent = 0;        ///< 首次发出的消息数。
        std::uint64_t messages_delivered = 0;   ///< 按序交付给上层的消息数。
        std::uint64_t acks_applied = 0;         ///< 被确认并从 inflight 移除的消息数。
        std::uint64_t retransmits = 0;          ///< 重传次数。
        std::uint64_t duplicates_dropped = 0;   ///< 重复帧丢弃数。
        std::uint64_t stale_dropped = 0;        ///< 过旧/超窗帧丢弃数。
        std::uint64_t send_failures = 0;        ///< FrameSink 拒绝或编码失败次数。
        std::uint64_t dependency_deferred = 0;  ///< dep_seq 未满足而拒绝的帧数。
        std::uint64_t invalid_frames = 0;       ///< 通道/序号非法的帧数。
        std::uint32_t last_rtt_ms = 0;          ///< 最近一次未被重传的消息 RTT 样本。
        std::size_t inflight = 0;  ///< 当前未确认消息数（OnTimer 后刷新）。
        std::size_t unsent = 0;    ///< 尚未发出的排队消息数。
    };

    /// 交付回调：seq + 载荷。载荷只在回调期间有效。
    using DeliverFn =
        std::function<void(std::uint32_t seq, const std::uint8_t* data, std::size_t bytes)>;
    /// 发送回调：完整帧字节。返回 false 表示 UDP 发送失败，通道会重试。
    using FrameSink = std::function<bool(const std::uint8_t* data, std::size_t bytes)>;

    /// @param channel_id 通道 id，写入帧头。
    /// @param relation 因果水位，用于检查 dep_seq；可为 nullptr（关闭因果检查）。
    /// @param deliver 有序交付回调。
    ReliableChannel(std::uint8_t channel_id, Relation* relation, DeliverFn deliver);
    ~ReliableChannel() = default;

    ReliableChannel(const ReliableChannel&) = delete;
    ReliableChannel& operator=(const ReliableChannel&) = delete;

    /// @brief 设置发送函数（通常绑定到 Link::SendBuiltFrame）。
    void SetFrameSink(FrameSink sink) {
        m_sink = std::move(sink);
    }

    /// @brief 入队一条可靠消息并尽量立即发送。
    /// @param bytes 载荷长度，必须 <= kMaxFramePayload。
    /// @param dep_seq 依赖的不可靠 seq；0 表示无依赖。
    /// @return 入队成功返回 true；超长或发送窗口已满返回 false。
    bool Send(const std::uint8_t* data, std::size_t bytes, std::uint32_t dep_seq = 0);

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

   private:
    struct Outgoing {
        std::uint32_t seq = 0;
        std::uint32_t dep_seq = 0;
        std::vector<std::uint8_t> payload;
        std::uint64_t first_send_ms = 0;
        std::uint64_t last_send_ms = 0;
        std::uint32_t retransmits = 0;
        bool ever_retransmitted = false;
    };

    bool SendNow(Outgoing& message, bool retransmit, std::uint64_t now_ms);
    void Pump(std::uint64_t now_ms);
    void Acknowledge(const Outgoing& message, std::uint64_t now_ms);
    void AdvanceDelivery();
    static std::uint32_t PtoFor(std::uint32_t retransmits) noexcept;

    const std::uint8_t m_channel_id;
    Relation* const m_relation;
    const DeliverFn m_deliver;
    FrameSink m_sink;

    std::uint32_t m_next_tx_seq = 1;
    std::uint32_t m_next_rx_seq = 1;
    std::deque<Outgoing> m_unsent;    ///< 已分配 seq、尚未发出（按 seq 升序）。
    std::deque<Outgoing> m_inflight;  ///< 已发出未确认（按 seq 升序）。
    std::map<std::uint32_t, std::vector<std::uint8_t>> m_rx_buffer;
    std::set<std::uint32_t> m_rx_seen;
    bool m_pending_ack = false;
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

    /// @brief 可观测计数器。
    struct Stats {
        std::uint64_t frames_sent = 0;
        std::uint64_t fragments_sent = 0;
        std::uint64_t frames_delivered = 0;
        std::uint64_t fragments_duplicate = 0;
        std::uint64_t fragments_stale = 0;  ///< 旧 frame_id 或已交付帧的分片。
        std::uint64_t frames_incomplete_dropped = 0;
        std::uint64_t fragments_invalid = 0;
        std::uint64_t fragments_send_failed = 0;
    };

    using FrameSink = std::function<bool(const std::uint8_t* data, std::size_t bytes)>;
    using DeliverFn = std::function<void(std::uint32_t frame_id, bool key, const std::uint8_t* data,
                                         std::size_t bytes)>;

    VideoChannel(FrameSink sink, DeliverFn deliver);
    ~VideoChannel() = default;

    VideoChannel(const VideoChannel&) = delete;
    VideoChannel& operator=(const VideoChannel&) = delete;

    /// @brief 把一帧拆成 <=1200B 的分片并发送。
    /// @param key 关键帧标记，写入每片的帧头 flags。
    /// @return 参数合法且已尝试发送返回 true；超过 kMaxFragments 返回 false。
    bool SendFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes);

    /// @brief 处理收到的 VIDEO 分片。
    void OnFrame(const FrameHeader& header, const std::uint8_t* payload, std::size_t bytes);

    /// @brief 清理超时的半成品重组状态。
    void OnTimer(std::uint64_t now_ms);

    const Stats& stats() const noexcept {
        return m_stats;
    }
    std::uint32_t delivered_frame_id() const noexcept {
        return m_delivered_frame_id;
    }

   private:
    struct Reassembly {
        std::uint16_t frag_count = 0;
        std::uint16_t received_count = 0;
        std::uint16_t last_fragment_bytes = 0;
        std::uint64_t last_update_ms = 0;
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> received;
    };

    void DropReassembly(std::map<std::uint32_t, Reassembly>::iterator it);

    FrameSink m_sink;
    DeliverFn m_deliver;
    std::map<std::uint32_t, Reassembly> m_active;
    std::uint32_t m_delivered_frame_id = 0;
    std::uint32_t m_max_active_frame_id = 0;
    std::uint32_t m_next_tx_seq = 1;
    Stats m_stats;
};

}  // namespace go2cpp::bento
