/**
 * @file channel.cpp
 * @brief Bento 可靠/不可靠/视频通道实现。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include "go2cpp/bento/net/channel.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace go2cpp::bento {
namespace {

/// 把 64 位毫秒差压到 32 位计数器；RTT 超过 4 秒没有记录意义。
std::uint32_t ClampRtt(std::uint64_t rtt_ms) noexcept {
    constexpr std::uint64_t kMaxRecordedRttMs = 0xFFFFFFFFULL;
    return static_cast<std::uint32_t>(std::min(rtt_ms, kMaxRecordedRttMs));
}

void WriteU16BE(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

std::uint16_t ReadU16BE(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                      static_cast<std::uint16_t>(in[1]));
}

}  // namespace

// ---------------------------------------------------------------------------
// RttEstimator
// ---------------------------------------------------------------------------

RttEstimator::RttEstimator() noexcept : RttEstimator(Config{}) {}

RttEstimator::RttEstimator(Config config) noexcept {
    SetConfig(config);
}

void RttEstimator::SetConfig(Config config) noexcept {
    if (config.min_pto_ms == 0) {
        config.min_pto_ms = 1;
    }
    if (config.max_pto_ms < config.min_pto_ms) {
        config.max_pto_ms = config.min_pto_ms;
    }
    if (config.initial_rtt_ms == 0) {
        config.initial_rtt_ms = 1;
    }
    m_config = config;
    if (!m_has_sample) {
        m_srtt_ms = m_config.initial_rtt_ms;
        m_rttvar_ms = m_config.initial_rtt_ms / 2;
    }
}

void RttEstimator::Reset() noexcept {
    m_has_sample = false;
    m_srtt_ms = m_config.initial_rtt_ms;
    m_rttvar_ms = m_config.initial_rtt_ms / 2;
}

void RttEstimator::OnSample(std::uint32_t rtt_ms) noexcept {
    if (!m_has_sample) {
        // 首样本：直接采用，方差取一半（RFC 9002 §5.3）。
        m_srtt_ms = rtt_ms;
        m_rttvar_ms = rtt_ms / 2;
        m_has_sample = true;
        return;
    }
    // 先用旧 srtt 算偏差，再更新 srtt；整数除法误差 < 1ms。
    const std::uint32_t delta =
        m_srtt_ms > rtt_ms ? m_srtt_ms - rtt_ms : rtt_ms - m_srtt_ms;
    m_rttvar_ms = static_cast<std::uint32_t>(
        (3ULL * static_cast<std::uint64_t>(m_rttvar_ms) + delta) / 4ULL);
    m_srtt_ms = static_cast<std::uint32_t>(
        (7ULL * static_cast<std::uint64_t>(m_srtt_ms) + rtt_ms) / 8ULL);
}

std::uint32_t RttEstimator::base_pto_ms() const noexcept {
    if (m_config.fixed_pto_ms != 0) {
        // 固定模式：完全复现旧实现的基础 PTO，用于弱网对比实验。
        return std::min(std::max(m_config.fixed_pto_ms, m_config.min_pto_ms),
                        m_config.max_pto_ms);
    }
    std::uint64_t pto = static_cast<std::uint64_t>(m_srtt_ms) +
                        4ULL * static_cast<std::uint64_t>(m_rttvar_ms) +
                        m_config.max_ack_delay_ms;
    if (pto < m_config.min_pto_ms) {
        pto = m_config.min_pto_ms;
    }
    if (pto > m_config.max_pto_ms) {
        pto = m_config.max_pto_ms;
    }
    return static_cast<std::uint32_t>(pto);
}

std::uint32_t RttEstimator::BackoffPtoMs(std::uint32_t retransmits) const noexcept {
    std::uint64_t pto = base_pto_ms();
    for (std::uint32_t i = 0; i < retransmits; ++i) {
        pto *= 2ULL;
        if (pto >= m_config.max_pto_ms) {
            return m_config.max_pto_ms;
        }
    }
    return static_cast<std::uint32_t>(pto);
}

// ---------------------------------------------------------------------------
// StreamReassembler
// ---------------------------------------------------------------------------

StreamReassembler::StreamReassembler() noexcept : StreamReassembler(Config{}) {}

StreamReassembler::StreamReassembler(Config config) noexcept {
    SetConfig(config);
}

void StreamReassembler::SetConfig(Config config) noexcept {
    if (config.timeout_ms == 0) {
        config.timeout_ms = 1;
    }
    if (config.max_active == 0) {
        config.max_active = 1;
    }
    m_config = config;
}

void StreamReassembler::Reset() noexcept {
    m_active.clear();
    m_stats = Stats{};
}

void StreamReassembler::EvictOne() {
    if (m_active.empty()) {
        return;
    }
    // 挤掉最久未更新的半成品，而不是固定 frame_id：可靠层乱序缓存时
    // 较老的消息可能仍在等待重传，按时间淘汰更接近真实占用。
    auto victim = m_active.begin();
    for (auto it = m_active.begin(); it != m_active.end(); ++it) {
        if (it->second.last_update_ms < victim->second.last_update_ms) {
            victim = it;
        }
    }
    m_active.erase(victim);
    ++m_stats.frames_evicted;
}

StreamReassembler::Result StreamReassembler::AddFragment(
    std::uint32_t frame_id, std::uint16_t frag_index, std::uint16_t frag_count,
    const std::uint8_t* data, std::size_t bytes, std::uint64_t now_ms) {
    if (frag_count == 0 || frag_count > kMaxStreamFragments || frag_index >= frag_count) {
        ++m_stats.fragments_invalid;
        return Result::kInvalid;
    }
    if (bytes > kStreamFragmentBytes || (bytes > 0 && data == nullptr)) {
        ++m_stats.fragments_invalid;
        return Result::kInvalid;
    }
    const bool is_last = static_cast<std::uint32_t>(frag_index) + 1U == frag_count;
    // 非末片必须满片，否则按固定偏移重组会错位；末片长度任意（含 0）。
    if (!is_last && bytes != kStreamFragmentBytes) {
        ++m_stats.fragments_invalid;
        return Result::kInvalid;
    }

    auto it = m_active.find(frame_id);
    if (it == m_active.end()) {
        if (m_active.size() >= m_config.max_active) {
            EvictOne();
        }
        Assembly assembly;
        assembly.frag_count = frag_count;
        assembly.last_update_ms = now_ms;
        assembly.data.assign(static_cast<std::size_t>(frag_count) * kStreamFragmentBytes, 0);
        assembly.received.assign(frag_count, 0);
        it = m_active.emplace(frame_id, std::move(assembly)).first;
        ++m_stats.frames_started;
    }

    Assembly& assembly = it->second;
    if (assembly.frag_count != frag_count) {
        // 同一 frame_id 的分片总数不一致：无法安全重组，整帧状态保留但不收。
        ++m_stats.fragments_invalid;
        return Result::kInvalid;
    }
    if (assembly.received[frag_index] != 0) {
        ++m_stats.fragments_duplicate;
        assembly.last_update_ms = now_ms;
        return Result::kDuplicate;
    }

    assembly.received[frag_index] = 1;
    ++assembly.received_count;
    ++m_stats.fragments_received;
    const std::size_t offset = static_cast<std::size_t>(frag_index) * kStreamFragmentBytes;
    if (bytes > 0) {
        std::memcpy(assembly.data.data() + offset, data, bytes);
    }
    if (is_last) {
        assembly.last_fragment_bytes = static_cast<std::uint16_t>(bytes);
    }
    assembly.last_update_ms = now_ms;

    if (assembly.received_count != assembly.frag_count) {
        return Result::kBuffered;
    }

    const std::size_t total_bytes =
        static_cast<std::size_t>(assembly.frag_count - 1) * kStreamFragmentBytes +
        assembly.last_fragment_bytes;
    std::vector<std::uint8_t> complete(assembly.data.begin(),
                                       assembly.data.begin() +
                                           static_cast<std::ptrdiff_t>(total_bytes));
    // 先移除状态再回调：上层可能在回调里继续注入/发送。
    m_active.erase(it);
    ++m_stats.frames_delivered;
    if (m_deliver) {
        m_deliver(frame_id, complete.data(), complete.size());
    }
    return Result::kDelivered;
}

std::size_t StreamReassembler::OnTimer(std::uint64_t now_ms) {
    std::size_t dropped = 0;
    auto it = m_active.begin();
    while (it != m_active.end()) {
        if (now_ms >= it->second.last_update_ms &&
            now_ms - it->second.last_update_ms >= m_config.timeout_ms) {
            it = m_active.erase(it);
            ++dropped;
            ++m_stats.frames_timed_out;
        } else {
            ++it;
        }
    }
    return dropped;
}

// ---------------------------------------------------------------------------
// ReliableChannel
// ---------------------------------------------------------------------------

ReliableChannel::ReliableChannel(std::uint8_t channel_id, Relation* relation, DeliverFn deliver)
    : ReliableChannel(channel_id, relation, std::move(deliver), Config{}, ClockFn{}) {}

ReliableChannel::ReliableChannel(std::uint8_t channel_id, Relation* relation, DeliverFn deliver,
                                 Config config)
    : ReliableChannel(channel_id, relation, std::move(deliver), std::move(config),
                      ClockFn{}) {}

ReliableChannel::ReliableChannel(std::uint8_t channel_id, Relation* relation, DeliverFn deliver,
                                 Config config, ClockFn clock)
    : m_channel_id(channel_id),
      m_relation(relation),
      m_deliver(std::move(deliver)),
      m_config(config),
      m_clock(std::move(clock)),
      m_rtt(m_config.rtt),
      m_cc(m_config.congestion),
      m_bandwidth(m_config.bandwidth) {
    if (m_config.fast_retransmit_threshold == 0) {
        m_config.fast_retransmit_threshold = 1;
    }
    if (m_config.reassembly_timeout_ms == 0) {
        m_config.reassembly_timeout_ms = 1;
    }
    if (m_config.max_reassembly == 0) {
        m_config.max_reassembly = 1;
    }
    if (m_config.initial_max_data == 0) {
        m_config.initial_max_data = 1U << 20;
    }
    if (m_config.peer_initial_max_data == 0) {
        m_config.peer_initial_max_data = m_config.initial_max_data;
    }
    m_local_max_data = m_config.initial_max_data;
    m_peer_max_data = m_config.peer_initial_max_data;
    m_last_advertised_max_data = m_config.initial_max_data;
    StreamReassembler::Config reassembly_config;
    reassembly_config.timeout_ms = m_config.reassembly_timeout_ms;
    reassembly_config.max_active = m_config.max_reassembly;
    m_reassembler.SetConfig(reassembly_config);
    m_reassembler.SetDeliver(
        [this](std::uint32_t /*frame_id*/, const std::uint8_t* data, std::size_t bytes) {
            ++m_stats.messages_reassembled;
            ++m_stats.messages_delivered;
            if (m_deliver) {
                m_deliver(m_delivery_first_seq, data, bytes);
            }
        });
    m_cc.Reset(Now());
    RefreshRttStats();
}

std::uint64_t ReliableChannel::Now() const noexcept {
    return m_clock ? m_clock() : NowMs();
}

void ReliableChannel::RefreshRttStats() noexcept {
    m_stats.srtt_ms = m_rtt.smoothed_rtt_ms();
    m_stats.rttvar_ms = m_rtt.rttvar_ms();
    m_stats.pto_ms = m_rtt.base_pto_ms();
    RefreshCongestionStats();
}

void ReliableChannel::RefreshCongestionStats() noexcept {
    const CongestionStats snapshot = m_cc.Snapshot(kSendWindow);
    m_stats.cwnd_packets = snapshot.cwnd_packets;
    m_stats.ssthresh_packets = snapshot.ssthresh_packets;
    m_stats.effective_cwnd = snapshot.effective_window;
    m_stats.pacing_rate_bps = snapshot.pacing_rate_bps;
    m_stats.pacing_deferrals = snapshot.pacing_deferrals;
    m_stats.loss_events = snapshot.loss_events;
    m_stats.timeout_loss_events = snapshot.timeout_events;
    m_stats.congestion_bytes_sent = snapshot.bytes_sent;
    m_stats.slow_start = snapshot.slow_start;
}

bool ReliableChannel::Send(const std::uint8_t* data, std::size_t bytes,
                           std::uint32_t dep_seq) {
    return TrySend(data, bytes, dep_seq) == SendStatus::kOk;
}

const char* SendStatusName(SendStatus status) noexcept {
    switch (status) {
        case SendStatus::kOk:
            return "OK";
        case SendStatus::kWouldBlock:
            return "WOULD_BLOCK";
        case SendStatus::kError:
            return "ERROR";
    }
    return "UNKNOWN";
}

SendStatus ReliableChannel::TrySend(const std::uint8_t* data, std::size_t bytes,
                                    std::uint32_t dep_seq) {
    if (bytes > kMaxStreamMessageBytes || (bytes > 0 && data == nullptr)) {
        ++m_stats.send_failures;
        return SendStatus::kError;
    }

    // <=1300B 保持 v0.1 单帧格式；更大的消息按 1292B 分片（8B 子头使
    // 每帧载荷仍 <=1300B）。分片总数为 0 不会出现。
    const bool fragmented = bytes > kMaxFramePayload;
    const std::size_t frag_count =
        fragmented ? (bytes + kStreamFragmentBytes - 1) / kStreamFragmentBytes : 1;
    if (frag_count > kMaxStreamFragments) {
        ++m_stats.send_failures;
        return SendStatus::kError;
    }

    // 流控（RFC 9000 MAX_DATA）：线上载荷（分片头也占用窗口）的累计
    // 偏移不得超过对端通告的上限。超窗是暂时状态，返回 WouldBlock，
    // 不能静默丢弃；调用方在收到 MAX_DATA 后重试同一消息。
    const std::uint64_t wire_payload =
        static_cast<std::uint64_t>(bytes) +
        (fragmented ? static_cast<std::uint64_t>(frag_count) * kStreamFragmentHeaderBytes : 0);
    if (m_tx_bytes_total > m_peer_max_data ||
        wire_payload > m_peer_max_data - m_tx_bytes_total) {
        ++m_stats.flow_control_blocked;
        return SendStatus::kWouldBlock;
    }

    // 窗口按「帧」计：一条大消息的所有分片必须在窗口内一次容纳，
    // 否则部分分片入队会破坏消息原子性。窗口不足时按背压返回 WouldBlock。
    if (m_unsent.size() + m_inflight.size() + frag_count > kSendWindow) {
        ++m_stats.send_window_blocked;
        return SendStatus::kWouldBlock;
    }

    std::uint32_t frame_id = m_next_frame_id++;
    if (m_next_frame_id == 0) {
        m_next_frame_id = 1;  // frame_id 0 保留为「未设置」。
    }

    for (std::size_t index = 0; index < frag_count; ++index) {
        Outgoing message;
        message.seq = m_next_tx_seq++;
        if (m_next_tx_seq == 0) {
            m_next_tx_seq = 1;  // seq 0 保留为「无效」。
        }
        message.dep_seq = dep_seq;
        message.fragmented = fragmented;
        if (fragmented) {
            const std::size_t offset = index * kStreamFragmentBytes;
            const std::size_t chunk = std::min(kStreamFragmentBytes, bytes - offset);
            message.payload.resize(kStreamFragmentHeaderBytes + chunk);
            StreamFragmentHeader fragment;
            fragment.frame_id = frame_id;
            fragment.frag_index = static_cast<std::uint16_t>(index);
            fragment.frag_count = static_cast<std::uint16_t>(frag_count);
            EncodeStreamFragmentHeader(fragment, message.payload.data());
            if (chunk > 0) {
                std::memcpy(message.payload.data() + kStreamFragmentHeaderBytes, data + offset,
                            chunk);
            }
        } else if (bytes > 0) {
            message.payload.assign(data, data + bytes);
        }
        m_unsent.push_back(std::move(message));
    }
    m_tx_bytes_total += wire_payload;
    ++m_stats.messages_sent;
    Pump(Now());
    m_stats.inflight = m_inflight.size();
    m_stats.unsent = m_unsent.size();
    return SendStatus::kOk;
}

void ReliableChannel::Pump(std::uint64_t now_ms) {
    while (!m_unsent.empty() && m_inflight.size() < kSendWindow) {
        // 拥塞窗口 + pacing 令牌双重门控：cwnd 满或令牌不足都不发新数据；
        // 已排队消息保留在 m_unsent，等 ACK/定时器继续 Pump。
        if (!m_cc.AllowSend(now_ms, m_inflight.size())) {
            break;
        }
        Outgoing message = std::move(m_unsent.front());
        const std::size_t wire_bytes =
            kFrameHeaderBytes + kAckBytes + message.payload.size();
        if (!SendNow(message, false, now_ms)) {
            // 放回队首，保留 seq，等下一次 timer 重试；不能丢弃，
            // 否则接收端的 expected 会永久卡住。
            m_unsent.push_front(std::move(message));
            break;
        }
        m_unsent.pop_front();
        m_inflight.push_back(std::move(message));
        m_cc.OnPacketSent(now_ms, wire_bytes);
        ++m_stats.fragments_sent;
    }
    RefreshCongestionStats();
}

bool ReliableChannel::SendNow(Outgoing& message, bool retransmit, std::uint64_t now_ms) {
    if (!m_sink) {
        ++m_stats.send_failures;
        return false;
    }
    AckState ack;
    FillAck(&ack);
    std::uint8_t flags = kFlagAck;
    if (retransmit) {
        flags = static_cast<std::uint8_t>(flags | kFlagRetransmit);
    }
    if (message.fragmented) {
        flags = static_cast<std::uint8_t>(flags | kFlagFragmented);
    }
    FrameHeader header;
    header.type = FrameType::kStream;
    header.channel = m_channel_id;
    header.flags = flags;
    header.seq = message.seq;
    header.dep_seq = message.dep_seq;
    const std::vector<std::uint8_t> frame =
        BuildFrame(header, &ack, message.payload.data(), message.payload.size());
    if (frame.empty()) {
        ++m_stats.send_failures;
        return false;
    }
    if (!m_sink(frame.data(), frame.size())) {
        ++m_stats.send_failures;
        return false;
    }
    if (message.first_send_ms == 0) {
        message.first_send_ms = now_ms;
    }
    message.last_send_ms = now_ms;
    // 数据帧已捎带当前接收状态，独立 ACK 不必重复发送。
    m_pending_ack = false;
    return true;
}

void ReliableChannel::OnFrame(const FrameHeader& header, const std::uint8_t* payload,
                              std::size_t bytes, const AckState* piggyback_ack) {
    if (header.channel != m_channel_id) {
        ++m_stats.invalid_frames;
        return;
    }
    if (piggyback_ack != nullptr) {
        OnAck(*piggyback_ack);
    }
    if (bytes > kMaxFramePayload || (bytes > 0 && payload == nullptr)) {
        ++m_stats.invalid_frames;
        return;
    }
    const bool fragmented = (header.flags & kFlagFragmented) != 0;
    if (fragmented && bytes < kStreamFragmentHeaderBytes) {
        ++m_stats.fragments_invalid;
        return;
    }
    const std::uint32_t seq = header.seq;
    if (seq == 0) {
        ++m_stats.invalid_frames;
        return;
    }

    // 因果等待：依赖未满足时拒绝该帧，且**不 ACK**。发送端会在 PTO 后
    // 重传；依赖满足后重传到达即可交付。这里不做乱序缓存，避免可靠通道
    // 在依赖满足后出现「越过较小 seq」的交付。
    if (header.dep_seq != 0 && m_relation != nullptr && !m_relation->IsReady(header.dep_seq)) {
        ++m_stats.dependency_deferred;
        return;
    }

    if (SeqGreater(m_next_rx_seq, seq)) {
        ++m_stats.stale_dropped;
        m_pending_ack = true;
        return;
    }
    if (static_cast<std::uint32_t>(seq - m_next_rx_seq) >=
        static_cast<std::uint32_t>(kReceiveWindow)) {
        ++m_stats.stale_dropped;
        return;
    }
    if (m_rx_seen.count(seq) != 0) {
        ++m_stats.duplicates_dropped;
        m_pending_ack = true;
        return;
    }

    // 流控：对端不得让累计偏移超过本端通告的 MAX_DATA。超窗帧不 ACK，
    // 由对端 PTO 重传；正常实现会在窗口耗尽前阻塞发送端。
    const std::uint64_t wire_bytes = static_cast<std::uint64_t>(bytes);
    if (m_rx_bytes_total > m_local_max_data ||
        wire_bytes > m_local_max_data - m_rx_bytes_total) {
        ++m_stats.flow_control_violations;
        return;
    }
    m_rx_bytes_total += wire_bytes;

    m_rx_seen.insert(seq);
    Received frame;
    frame.fragmented = fragmented;
    if (bytes > 0) {
        frame.payload.assign(payload, payload + bytes);
    }
    m_rx_buffer.emplace(seq, std::move(frame));
    m_pending_ack = true;
    AdvanceDelivery();

    // 接收窗口消耗过半即推进 MAX_DATA。这里按「已接收字节」而不是
    // 「已交付字节」前进：对端可以在应用消费前继续发送，内存上界仍由
    // kReceiveWindow（帧级）与初始窗口共同约束。
    const std::uint64_t window = m_config.initial_max_data;
    if (window > 0 && m_rx_bytes_total <= m_local_max_data &&
        m_local_max_data - m_rx_bytes_total < window / 2) {
        m_local_max_data = m_rx_bytes_total + window;
    }
    m_stats.inflight = m_inflight.size();
    m_stats.unsent = m_unsent.size();
}

void ReliableChannel::AdvanceDelivery() {
    const std::uint64_t now_ms = Now();
    while (m_rx_seen.erase(m_next_rx_seq) != 0) {
        auto it = m_rx_buffer.find(m_next_rx_seq);
        if (it != m_rx_buffer.end()) {
            // 先摘下再交付：回调里可能调用 Send()，不能持有 map 迭代器。
            Received frame = std::move(it->second);
            m_rx_buffer.erase(it);
            const std::uint32_t seq = m_next_rx_seq;
            if (frame.fragmented) {
                StreamFragmentHeader fragment;
                if (frame.payload.size() < kStreamFragmentHeaderBytes ||
                    !DecodeStreamFragmentHeader(frame.payload.data(), frame.payload.size(),
                                                &fragment)) {
                    ++m_stats.fragments_invalid;
                } else {
                    ++m_stats.fragments_received;
                    // 同一消息的分片 seq 连续，首片 seq = 当前 seq - frag_index。
                    m_delivery_first_seq = seq - fragment.frag_index;
                    const StreamReassembler::Result result = m_reassembler.AddFragment(
                        fragment.frame_id, fragment.frag_index, fragment.frag_count,
                        frame.payload.data() + kStreamFragmentHeaderBytes,
                        frame.payload.size() - kStreamFragmentHeaderBytes, now_ms);
                    if (result == StreamReassembler::Result::kDuplicate) {
                        ++m_stats.fragments_duplicate;
                    } else if (result == StreamReassembler::Result::kInvalid) {
                        ++m_stats.fragments_invalid;
                    }
                }
            } else {
                ++m_stats.messages_delivered;
                if (m_deliver) {
                    m_deliver(seq, frame.payload.data(), frame.payload.size());
                }
            }
        }
        ++m_next_rx_seq;
        if (m_next_rx_seq == 0) {
            m_next_rx_seq = 1;
        }
    }
}

void ReliableChannel::OnAck(const AckState& ack) {
    if (ack.ack_seq == 0 || m_inflight.empty()) {
        return;
    }
    const std::uint64_t now_ms = Now();
    const bool progressed = SeqGreater(ack.ack_seq, m_highest_ack);
    std::uint32_t acked_now = 0;
    if (progressed) {
        m_highest_ack = ack.ack_seq;
        m_dup_acks = 0;
        m_sack_evidence = 0;
        m_in_fast_recovery = false;
        m_pto_recovery = false;
        // 旧 SACK 位图以旧 ack_seq 为基准，累积确认前进后全部失效。
        m_sacked.clear();
    }

    // 累积确认：ack_seq 之前的所有消息都已收到。
    while (!m_inflight.empty() && SeqGreater(ack.ack_seq, m_inflight.front().seq)) {
        m_sacked.erase(m_inflight.front().seq);
        Acknowledge(m_inflight.front(), now_ms);
        ++acked_now;
        m_inflight.pop_front();
    }

    // SACK：ack_bits 的 bit i 表示 ack_seq+i 已收到（可能跨过空洞）。
    std::size_t new_sack = 0;
    for (std::uint32_t bit = 1; bit < 32; ++bit) {
        if ((ack.ack_bits & (1U << bit)) == 0) {
            continue;
        }
        const std::uint32_t sacked_seq = ack.ack_seq + bit;
        if (m_sacked.insert(sacked_seq).second) {
            ++new_sack;
        }
    }
    m_sack_evidence += new_sack;
    // 无时间戳回显时，只要出现 SACK 空洞，空洞之后的消息（包括乱序到达、
    // 已被 SACK 的）其 ACK 时刻都可能被 ACK 调度/空洞恢复推迟；保守地
    // 全部跳过采样，只让「无空洞的累积 ACK」产生 RTT 样本。
    if (new_sack > 0 && !m_inflight.empty()) {
        const std::uint32_t first_unacked = m_inflight.front().seq;
        for (Outgoing& message : m_inflight) {
            if (SeqGreater(message.seq, first_unacked)) {
                message.hol_blocked = true;
            }
        }
    }

    for (auto it = m_inflight.begin(); it != m_inflight.end();) {
        const std::uint32_t delta = it->seq - ack.ack_seq;
        if (delta < 32 && (ack.ack_bits & (1U << delta)) != 0) {
            m_sacked.erase(it->seq);
            Acknowledge(*it, now_ms);
            // SACK 只是乱序到达的证据，不计入拥塞窗口增长：窗口增长只由
            // 累积 ACK 驱动（否则一次 SACK 风暴会在丢包减窗前虚增 cwnd）。
            it = m_inflight.erase(it);
        } else {
            ++it;
        }
    }

    // ACK 腾出了窗口：先增长拥塞窗口，再补发排队消息。
    m_cc.OnAck(acked_now, m_rtt.smoothed_rtt_ms(), now_ms);
    Pump(now_ms);
    (void)TryFastRetransmit(ack, progressed, new_sack, now_ms);
    RefreshRttStats();
    m_stats.inflight = m_inflight.size();
    m_stats.unsent = m_unsent.size();
}

bool ReliableChannel::TryFastRetransmit(const AckState& ack, bool ack_progressed,
                                        std::size_t new_sack, std::uint64_t now_ms) {
    if (!m_config.fast_retransmit || m_inflight.empty() || m_in_fast_recovery) {
        return false;
    }
    const std::uint32_t first_unacked = m_inflight.front().seq;
    std::size_t sacked_above = 0;
    for (const std::uint32_t sacked_seq : m_sacked) {
        if (SeqGreater(sacked_seq, first_unacked)) {
            ++sacked_above;
        }
    }

    const bool sack_gap = sacked_above >= m_config.fast_retransmit_threshold ||
                          m_sack_evidence >= m_config.fast_retransmit_threshold;
    bool trigger = sack_gap;
    if (!trigger && !ack_progressed && new_sack == 0) {
        // 只有「不推进累积确认、也没有新 SACK」的 ACK 才算重复 ACK。
        ++m_dup_acks;
        trigger = m_dup_acks >= m_config.fast_retransmit_threshold;
    }
    if (!trigger) {
        return false;
    }

    m_dup_acks = 0;
    m_sack_evidence = 0;
    m_in_fast_recovery = true;
    // 统计同一 SACK 窗口内的「空洞」数：ACK 位图覆盖范围（32 个 seq）内
    // 尚未被确认、也未被 SACK 的包就是可判定的空洞。窗口外的未确认包可能
    // 仍在网络中，不能计入（否则高抖动链路会把每个 ACK 都误判成批量丢包）。
    std::uint32_t holes = 1;
    if (sack_gap) {
        holes = 0;
        for (const Outgoing& message : m_inflight) {
            const std::uint32_t offset = message.seq - ack.ack_seq;
            if (offset >= 32) {
                break;  // inflight 按 seq 递增，之后的只会更远。
            }
            ++holes;
        }
        if (holes == 0) {
            holes = 1;
        }
    }
    // 每轮快速恢复只降一次窗；随机丢包时拥塞控制器不会降窗（见 congestion.cpp）。
    CongestionController::LossContext loss_context;
    loss_context.now_ms = now_ms;
    loss_context.srtt_ms = m_rtt.smoothed_rtt_ms();
    loss_context.holes = holes;
    loss_context.inflight = m_inflight.size();
    m_cc.OnLoss(loss_context);
    std::size_t retransmitted = 0;
    for (auto it = m_inflight.begin();
         it != m_inflight.end() && retransmitted < kFastRetransmitBatch; ++it) {
        if (sack_gap) {
            // 32 位 SACK 窗口内：SACK 已确认的消息已从 inflight 移除，
            // 仍在窗口内的就是空洞；窗口外的空洞不可知，留给后续 ACK/PTO。
            const std::uint32_t offset = it->seq - ack.ack_seq;
            if (offset >= 32) {
                break;
            }
        } else if (it != m_inflight.begin()) {
            // 纯重复 ACK（无 SACK 信息）只能确定最小未确认这一条。
            break;
        }
        ++it->retransmits;
        it->ever_retransmitted = true;
        ++m_stats.fast_retransmits;
        ++m_stats.retransmits;
        m_cc.NoteRetransmit();
        m_bandwidth.OnLoss(now_ms, 1);
        ++retransmitted;
        if (!SendNow(*it, true, now_ms)) {
            // 发送失败也推进时间戳，避免同一次 Tick 反复重试。
            it->last_send_ms = now_ms;
        }
    }
    return retransmitted != 0;
}

void ReliableChannel::Acknowledge(Outgoing& message, std::uint64_t now_ms) {
    ++m_stats.acks_applied;
    // ABR 用应用层字节：分片消息的 8B 子头不计入有效载荷。
    const std::size_t app_bytes =
        message.fragmented && message.payload.size() >= kStreamFragmentHeaderBytes
            ? message.payload.size() - kStreamFragmentHeaderBytes
            : message.payload.size();
    m_bandwidth.OnAck(now_ms, app_bytes);
    // Karn 算法：重传过的消息不采样 RTT，避免把重传等待算进 RTT。
    if (!message.ever_retransmitted && !message.hol_blocked &&
        now_ms >= message.first_send_ms) {
        // 本地回环/同机场景实际 RTT 可能 <1ms：钳到 1ms，避免 0 RTT 把
        // srtt/rttvar 退化为 0 并污染统计与 pacing 分母。
        const std::uint32_t rtt_ms =
            std::max<std::uint32_t>(1, ClampRtt(now_ms - message.first_send_ms));
        m_stats.last_rtt_ms = rtt_ms;
        m_rtt.OnSample(rtt_ms);
    }
}

void ReliableChannel::OnTimer(std::uint64_t now_ms) {
    Pump(now_ms);
    // PTO 补发：每个 tick 最多重传 kPtoRetransmitBatch 条已超时消息，
    // 避免一次 RunOnce 内突发上百帧；后续 tick 继续按 seq 补。弱网下
    // ACK 位图只有 32 位，空洞之后的大量消息可能早已被对端收到但未被
    // SACK，因此保留退避、按 tick 限速能明显减少冗余重传。
    std::size_t retransmitted = 0;
    for (auto it = m_inflight.begin();
         it != m_inflight.end() && retransmitted < kPtoRetransmitBatch; ++it) {
        const std::uint32_t pto = m_rtt.BackoffPtoMs(it->retransmits);
        if (now_ms < it->last_send_ms || now_ms - it->last_send_ms < pto) {
            continue;
        }
        ++it->retransmits;
        it->ever_retransmitted = true;
        ++m_stats.retransmits;
        m_cc.NoteRetransmit();
        ++retransmitted;
        if (!SendNow(*it, true, now_ms)) {
            // 发送失败也推进时间戳，否则同一次 Tick 会反复重试。
            it->last_send_ms = now_ms;
        }
    }
    if (retransmitted > 0 && !m_pto_recovery) {
        // 一次 PTO 丢失事件只降窗一次；ACK 前进后 m_pto_recovery 清除。
        m_pto_recovery = true;
        // PTO 没有 ACK 位图可参考（对端 ACK 可能已经停了），按孤立丢包上报，
        // 由拥塞控制器结合 SRTT 膨胀与丢包率决定是否真的降窗。
        CongestionController::LossContext loss_context;
        loss_context.now_ms = now_ms;
        loss_context.srtt_ms = m_rtt.smoothed_rtt_ms();
        loss_context.holes = 1;
        loss_context.inflight = m_inflight.size();
        m_cc.OnTimeout(loss_context);
        m_bandwidth.OnLoss(now_ms, retransmitted);
    }
    const std::size_t dropped = m_reassembler.OnTimer(now_ms);
    m_stats.reassembly_timeouts += dropped;
    RefreshRttStats();
    m_stats.inflight = m_inflight.size();
    m_stats.unsent = m_unsent.size();
}

void ReliableChannel::OnMaxData(std::uint64_t limit) noexcept {
    ++m_stats.max_data_received;
    if (limit <= m_peer_max_data) {
        return;
    }
    m_peer_max_data = limit;
    // 窗口扩大后立刻补发：已入队但被拥塞/pacing 挡住的消息在这里前进。
    // 被 WouldBlock 拒绝的消息不会入队，需要调用方重试。
    Pump(Now());
}

bool ReliableChannel::PendingMaxData(std::uint64_t* limit, std::uint64_t now_ms) const noexcept {
    if (limit != nullptr) {
        *limit = m_local_max_data;
    }
    if (m_local_max_data <= m_config.initial_max_data) {
        return false;  // 初始窗口双方已知，无需通告。
    }
    if (m_local_max_data > m_last_advertised_max_data) {
        return true;
    }
    if (m_config.max_data_refresh_ms == 0 || m_last_max_data_sent_ms == 0) {
        return false;
    }
    // 幂等重发：MAX_DATA 可能丢失，对端在收到前会一直阻塞；按固定
    // 周期重发最新上限，直到窗口再次推进或连接关闭。
    return now_ms >= m_last_max_data_sent_ms &&
           now_ms - m_last_max_data_sent_ms >= m_config.max_data_refresh_ms;
}

void ReliableChannel::OnMaxDataSent(std::uint64_t limit, std::uint64_t now_ms) noexcept {
    if (limit > m_last_advertised_max_data) {
        m_last_advertised_max_data = limit;
    }
    m_last_max_data_sent_ms = now_ms;
    ++m_stats.max_data_sent;
}

FlowControlStats ReliableChannel::flow_control() const noexcept {
    FlowControlStats stats;
    stats.local_max_data = m_local_max_data;
    stats.peer_max_data = m_peer_max_data;
    stats.tx_bytes_total = m_tx_bytes_total;
    stats.rx_bytes_total = m_rx_bytes_total;
    stats.max_data_sent = m_stats.max_data_sent;
    stats.max_data_received = m_stats.max_data_received;
    stats.blocked = m_stats.flow_control_blocked;
    stats.violations = m_stats.flow_control_violations;
    return stats;
}

BandwidthStats ReliableChannel::bandwidth_stats() const noexcept {
    return m_bandwidth.Snapshot(Now());
}

bool ReliableChannel::FillAck(AckState* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    out->ack_seq = m_next_rx_seq;
    out->ack_bits = 0;
    for (std::uint32_t i = 0; i < 32; ++i) {
        if (m_rx_seen.count(m_next_rx_seq + i) != 0) {
            out->ack_bits |= (1U << i);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// UnreliableChannel
// ---------------------------------------------------------------------------

UnreliableChannel::UnreliableChannel(std::uint8_t channel_id, Relation* relation, DeliverFn deliver)
    : m_channel_id(channel_id), m_relation(relation), m_deliver(std::move(deliver)) {}

bool UnreliableChannel::Send(const std::uint8_t* data, std::size_t bytes, std::uint32_t dep_seq) {
    if (bytes > kMaxFramePayload || (bytes > 0 && data == nullptr)) {
        return false;
    }
    const std::uint32_t seq = m_next_tx_seq++;
    if (m_next_tx_seq == 0) {
        m_next_tx_seq = 1;
    }
    FrameHeader header;
    header.type = FrameType::kDatagram;
    header.channel = m_channel_id;
    header.seq = seq;
    header.dep_seq = dep_seq;
    const std::vector<std::uint8_t> frame = BuildFrame(header, nullptr, data, bytes);
    if (frame.empty() || !m_sink || !m_sink(frame.data(), frame.size())) {
        return false;
    }
    ++m_stats.messages_sent;
    return true;
}

void UnreliableChannel::OnFrame(const FrameHeader& header, const std::uint8_t* payload,
                                std::size_t bytes) {
    if (header.channel != m_channel_id) {
        ++m_stats.invalid_frames;
        return;
    }
    if (bytes > kMaxFramePayload || (bytes > 0 && payload == nullptr)) {
        ++m_stats.invalid_frames;
        return;
    }
    const std::uint32_t seq = header.seq;
    if (seq == 0) {
        ++m_stats.invalid_frames;
        return;
    }
    if (!SeqGreater(seq, m_applied_seq)) {
        // DropStale：到达的旧包/重复包永远不采纳。这正是「不可靠但 0 乱序」
        // 的代价——乱序晚到的包被丢弃，而不是缓存等待。
        ++m_stats.stale_dropped;
        return;
    }

    std::vector<std::uint8_t> owned;
    if (bytes > 0) {
        owned.assign(payload, payload + bytes);
    }
    if (header.dep_seq != 0 && m_relation != nullptr && !m_relation->IsReady(header.dep_seq)) {
        ++m_stats.dependency_deferred;
        const std::uint32_t pending_seq = seq;
        m_relation->Enqueue(header.dep_seq,
                            [this, pending_seq, owned = std::move(owned)]() mutable {
                                Accept(pending_seq, std::move(owned));
                            });
        // 依赖满足的时机只可能来自本帧之外（后续帧/BARRIER）；这里不 Flush。
        return;
    }

    Accept(seq, std::move(owned));
    FlushPending();
}

void UnreliableChannel::Accept(std::uint32_t seq, std::vector<std::uint8_t> payload) {
    // pending 释放时可能已被更晚的 seq 越过：仍然按 DropStale 丢弃，
    // 从而保证交付序列严格递增（0 乱序）。
    if (!SeqGreater(seq, m_applied_seq)) {
        ++m_stats.stale_dropped;
        return;
    }
    m_applied_seq = seq;
    m_stats.applied_seq = seq;
    if (m_relation != nullptr) {
        m_relation->AdvanceUnreliable(seq);
    }
    ++m_stats.messages_delivered;
    if (m_deliver) {
        m_deliver(seq, payload.data(), payload.size());
    }
}

void UnreliableChannel::FlushPending() {
    if (m_relation != nullptr) {
        m_relation->Flush();
    }
}

void UnreliableChannel::OnBarrier(std::uint32_t barrier_seq) noexcept {
    if (SeqGreater(barrier_seq, m_applied_seq)) {
        m_applied_seq = barrier_seq;
        m_stats.applied_seq = barrier_seq;
    }
}

// ---------------------------------------------------------------------------
// VideoChannel
// ---------------------------------------------------------------------------

VideoChannel::VideoChannel(FrameSink sink, DeliverFn deliver)
    : VideoChannel(std::move(sink), std::move(deliver), Config{}) {}

VideoChannel::VideoChannel(FrameSink sink, DeliverFn deliver, Config config)
    : m_sink(std::move(sink)), m_deliver(std::move(deliver)), m_config(config) {
    // FEC 只支持 2..8 的分组；其余值（含 1）一律视为关闭，避免
    // 生成“1 片数据 + 1 片校验”这种纯复制、无恢复价值的行为。
    if (m_config.fec_group_size < 2 ||
        m_config.fec_group_size > static_cast<std::uint16_t>(kMaxFecGroupSize)) {
        m_config.fec_group_size = 0;
    }
}

bool VideoChannel::SendFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data,
                             std::size_t bytes) {
    if (bytes > 0 && data == nullptr) {
        return false;
    }
    const std::size_t frag_count = bytes == 0 ? 1 : (bytes + kFragmentBytes - 1) / kFragmentBytes;
    if (frag_count > kMaxFragments) {
        return false;
    }
    ++m_stats.frames_sent;

    for (std::size_t index = 0; index < frag_count; ++index) {
        const std::size_t offset = index * kFragmentBytes;
        const std::size_t chunk = bytes == 0 ? 0 : std::min(kFragmentBytes, bytes - offset);
        VideoFragmentHeader video_header;
        video_header.frame_id = frame_id;
        video_header.frag_index = static_cast<std::uint16_t>(index);
        video_header.frag_count = static_cast<std::uint16_t>(frag_count);
        std::vector<std::uint8_t> payload(kVideoHeaderBytes + chunk);
        EncodeVideoHeader(video_header, payload.data());
        if (chunk > 0) {
            std::memcpy(payload.data() + kVideoHeaderBytes, data + offset, chunk);
        }

        FrameHeader header;
        header.type = FrameType::kVideo;
        header.channel = kChannelVideo;
        header.flags = key ? kFlagKey : 0;
        header.seq = m_next_tx_seq++;
        if (m_next_tx_seq == 0) {
            m_next_tx_seq = 1;
        }
        const std::vector<std::uint8_t> frame =
            BuildFrame(header, nullptr, payload.data(), payload.size());
        if (frame.empty() || !m_sink || !m_sink(frame.data(), frame.size())) {
            ++m_stats.fragments_send_failed;
        } else {
            ++m_stats.fragments_sent;
        }
    }

    if (m_config.fec_group_size >= 2) {
        for (std::size_t start = 0; start < frag_count; start += m_config.fec_group_size) {
            const std::size_t count =
                std::min<std::size_t>(m_config.fec_group_size, frag_count - start);
            if (count < 2) {
                // 单片的组没有恢复价值（校验片等于数据本身）。
                continue;
            }
            (void)SendFecParity(frame_id, key, m_config.fec_group_size,
                                static_cast<std::size_t>(frag_count), data, bytes,
                                static_cast<std::uint16_t>(start));
        }
    }
    return true;
}

bool VideoChannel::SendFecParity(std::uint32_t frame_id, bool key, std::uint16_t group_size,
                                 std::size_t frag_count, const std::uint8_t* data,
                                 std::size_t bytes, std::uint16_t group_start) {
    const std::size_t count =
        std::min<std::size_t>(group_size, frag_count - static_cast<std::size_t>(group_start));
    if (count < 2 || count > kMaxFecGroupSize) {
        return false;
    }
    std::vector<std::uint16_t> lengths(count, 0);
    std::uint16_t max_len = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t offset =
            (static_cast<std::size_t>(group_start) + i) * kFragmentBytes;
        const std::size_t length = bytes == 0 ? 0 : std::min(kFragmentBytes, bytes - offset);
        lengths[i] = static_cast<std::uint16_t>(length);
        max_len = std::max(max_len, lengths[i]);
    }

    std::vector<std::uint8_t> parity(max_len, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t offset =
            (static_cast<std::size_t>(group_start) + i) * kFragmentBytes;
        for (std::uint16_t j = 0; j < lengths[i]; ++j) {
            parity[j] = static_cast<std::uint8_t>(parity[j] ^ data[offset + j]);
        }
    }

    // 载荷：8B 视频子头 + 2B group_start + 2B group_count + 2B max_len
    //      + count×2B 原始长度 + max_len 字节 XOR 校验。
    // 校验片的 frag_index 固定为 group_size（题目约定 frag_idx=N）；
    // 多组时由 group_start 区分。frag_count 仍是数据片总数。
    const std::size_t payload_bytes =
        kVideoHeaderBytes + kFecExtensionBytes + count * 2 + max_len;
    std::vector<std::uint8_t> payload(payload_bytes, 0);
    VideoFragmentHeader video_header;
    video_header.frame_id = frame_id;
    video_header.frag_index = group_size;
    video_header.frag_count = static_cast<std::uint16_t>(frag_count);
    EncodeVideoHeader(video_header, payload.data());
    WriteU16BE(payload.data() + kVideoHeaderBytes, group_start);
    WriteU16BE(payload.data() + kVideoHeaderBytes + 2, static_cast<std::uint16_t>(count));
    WriteU16BE(payload.data() + kVideoHeaderBytes + 4, max_len);
    std::size_t cursor = kVideoHeaderBytes + kFecExtensionBytes;
    for (std::size_t i = 0; i < count; ++i) {
        WriteU16BE(payload.data() + cursor, lengths[i]);
        cursor += 2;
    }
    if (max_len > 0) {
        std::memcpy(payload.data() + cursor, parity.data(), max_len);
    }

    FrameHeader header;
    header.type = FrameType::kVideo;
    header.channel = kChannelVideo;
    header.flags = static_cast<std::uint8_t>(kFlagFec | (key ? kFlagKey : 0));
    header.seq = m_next_tx_seq++;
    if (m_next_tx_seq == 0) {
        m_next_tx_seq = 1;
    }
    const std::vector<std::uint8_t> frame =
        BuildFrame(header, nullptr, payload.data(), payload.size());
    if (frame.empty() || !m_sink || !m_sink(frame.data(), frame.size())) {
        ++m_stats.fec_parity_send_failed;
        return false;
    }
    ++m_stats.fec_parity_sent;
    return true;
}

void VideoChannel::OnFrame(const FrameHeader& header, const std::uint8_t* payload,
                           std::size_t bytes) {
    if (header.channel != kChannelVideo) {
        ++m_stats.fragments_invalid;
        return;
    }
    if ((header.flags & kFlagFec) != 0) {
        OnFecParity(header, payload, bytes);
        return;
    }
    VideoFragmentHeader video_header;
    if (!DecodeVideoHeader(payload, bytes, &video_header)) {
        ++m_stats.fragments_invalid;
        return;
    }
    if (video_header.frag_count == 0 || video_header.frag_count > kMaxFragments ||
        video_header.frag_index >= video_header.frag_count) {
        ++m_stats.fragments_invalid;
        return;
    }
    const std::size_t frag_bytes = bytes - kVideoHeaderBytes;
    const bool is_last = (video_header.frag_index + 1) == video_header.frag_count;
    if (frag_bytes > kFragmentBytes) {
        ++m_stats.fragments_invalid;
        return;
    }
    // 非末片必须是满片，否则重组偏移会错位；末片允许 0..1200。
    if (!is_last && frag_bytes != kFragmentBytes) {
        ++m_stats.fragments_invalid;
        return;
    }
    if (!SeqGreater(video_header.frame_id, m_delivered_frame_id)) {
        ++m_stats.fragments_stale;
        return;
    }

    auto it = m_active.find(video_header.frame_id);
    if (it == m_active.end()) {
        if (!SeqGreater(video_header.frame_id, m_max_active_frame_id)) {
            // 比当前最大重组帧还旧：丢弃，避免丢包后旧帧一直占着重组缓冲。
            ++m_stats.fragments_stale;
            return;
        }
        if (m_active.size() >= kMaxReassembly) {
            DropReassembly(m_active.begin());
        }
        Reassembly reassembly;
        reassembly.frag_count = video_header.frag_count;
        reassembly.last_update_ms = NowMs();
        reassembly.data.assign(static_cast<std::size_t>(video_header.frag_count) * kFragmentBytes,
                               0);
        reassembly.received.assign(video_header.frag_count, 0);
        it = m_active.emplace(video_header.frame_id, std::move(reassembly)).first;
        m_max_active_frame_id = video_header.frame_id;
    }

    Reassembly& reassembly = it->second;
    if (reassembly.frag_count != video_header.frag_count) {
        ++m_stats.fragments_invalid;
        return;
    }
    if ((header.flags & kFlagKey) != 0) {
        reassembly.key = true;
    }
    if (reassembly.received[video_header.frag_index] != 0) {
        ++m_stats.fragments_duplicate;
        reassembly.last_update_ms = NowMs();
        return;
    }
    reassembly.received[video_header.frag_index] = 1;
    ++reassembly.received_count;
    const std::size_t write_offset =
        static_cast<std::size_t>(video_header.frag_index) * kFragmentBytes;
    if (frag_bytes > 0) {
        std::memcpy(reassembly.data.data() + write_offset, payload + kVideoHeaderBytes, frag_bytes);
    }
    if (is_last) {
        reassembly.last_fragment_bytes = static_cast<std::uint16_t>(frag_bytes);
    }
    reassembly.last_update_ms = NowMs();

    // 数据片迟到时也要重试恢复：校验片可能先于缺失的数据片到达。
    (void)TryRecover(video_header.frame_id, reassembly);
    if (reassembly.received_count == reassembly.frag_count) {
        DeliverFrame(video_header.frame_id, reassembly);
    }
}

void VideoChannel::OnFecParity(const FrameHeader& header, const std::uint8_t* payload,
                               std::size_t bytes) {
    VideoFragmentHeader video_header;
    if (!DecodeVideoHeader(payload, bytes, &video_header) ||
        bytes < kVideoHeaderBytes + kFecExtensionBytes) {
        ++m_stats.fragments_invalid;
        return;
    }
    const std::uint16_t group_start =
        ReadU16BE(payload + kVideoHeaderBytes);
    const std::uint16_t group_count =
        ReadU16BE(payload + kVideoHeaderBytes + 2);
    const std::uint16_t max_len =
        ReadU16BE(payload + kVideoHeaderBytes + 4);
    if (video_header.frag_count == 0 || video_header.frag_count > kMaxFragments ||
        group_count < 2 || group_count > kMaxFecGroupSize || max_len > kFragmentBytes ||
        static_cast<std::size_t>(group_start) + group_count > video_header.frag_count) {
        ++m_stats.fragments_invalid;
        return;
    }
    const std::size_t lengths_bytes = static_cast<std::size_t>(group_count) * 2;
    if (bytes < kVideoHeaderBytes + kFecExtensionBytes + lengths_bytes + max_len) {
        ++m_stats.fragments_invalid;
        return;
    }
    if (!SeqGreater(video_header.frame_id, m_delivered_frame_id)) {
        ++m_stats.fragments_stale;
        return;
    }

    auto it = m_active.find(video_header.frame_id);
    if (it == m_active.end()) {
        if (!SeqGreater(video_header.frame_id, m_max_active_frame_id)) {
            ++m_stats.fragments_stale;
            return;
        }
        if (m_active.size() >= kMaxReassembly) {
            DropReassembly(m_active.begin());
        }
        Reassembly reassembly;
        reassembly.frag_count = video_header.frag_count;
        reassembly.last_update_ms = NowMs();
        reassembly.data.assign(static_cast<std::size_t>(video_header.frag_count) * kFragmentBytes,
                               0);
        reassembly.received.assign(video_header.frag_count, 0);
        it = m_active.emplace(video_header.frame_id, std::move(reassembly)).first;
        m_max_active_frame_id = video_header.frame_id;
    }
    Reassembly& reassembly = it->second;
    if (reassembly.frag_count != video_header.frag_count) {
        ++m_stats.fragments_invalid;
        return;
    }
    if ((header.flags & kFlagKey) != 0) {
        reassembly.key = true;
    }
    FecBlock block;
    block.group_start = group_start;
    block.group_count = group_count;
    block.max_len = max_len;
    block.lengths.resize(group_count);
    std::size_t cursor = kVideoHeaderBytes + kFecExtensionBytes;
    for (std::size_t i = 0; i < group_count; ++i) {
        const std::uint16_t length = ReadU16BE(payload + cursor);
        cursor += 2;
        if (length > kFragmentBytes) {
            ++m_stats.fragments_invalid;
            return;
        }
        // 非末片必须是满片；否则恢复出的重组偏移会错位。
        const std::size_t absolute = static_cast<std::size_t>(group_start) + i;
        if (absolute + 1 < video_header.frag_count && length != kFragmentBytes) {
            ++m_stats.fragments_invalid;
            return;
        }
        block.lengths[i] = length;
    }
    block.parity.assign(payload + cursor, payload + cursor + max_len);
    reassembly.last_update_ms = NowMs();
    m_fec[video_header.frame_id][group_start] = std::move(block);
    ++m_stats.fec_parity_received;

    (void)TryRecover(video_header.frame_id, reassembly);
    if (reassembly.received_count == reassembly.frag_count) {
        DeliverFrame(video_header.frame_id, reassembly);
    }
}

bool VideoChannel::TryRecover(std::uint32_t frame_id, Reassembly& reassembly) {
    auto fec_it = m_fec.find(frame_id);
    if (fec_it == m_fec.end()) {
        return false;
    }
    bool recovered_any = false;
    for (auto& entry : fec_it->second) {
        FecBlock& block = entry.second;
        if (block.group_count < 2 ||
            static_cast<std::size_t>(block.group_start) + block.group_count >
                reassembly.frag_count) {
            continue;
        }
        std::size_t missing = 0;
        std::size_t missing_index = 0;
        for (std::size_t i = 0; i < block.group_count; ++i) {
            const std::size_t index = static_cast<std::size_t>(block.group_start) + i;
            if (reassembly.received[index] == 0) {
                ++missing;
                missing_index = index;
            }
        }
        // 只有“恰好缺 1 片”才可解；缺 2 片及以上绝不能猜，直接保持
        // 未完成状态，最终由 OnTimer 丢弃整帧。
        if (missing != 1 || block.lengths.size() != block.group_count ||
            block.parity.size() != block.max_len) {
            continue;
        }
        const std::size_t relative = missing_index - block.group_start;
        const std::uint16_t missing_len = block.lengths[relative];
        // 从校验片开始，再异或其余数据片：结果就是缺失片的原始字节。
        std::vector<std::uint8_t> recovered = block.parity;
        recovered.resize(block.max_len, 0);
        for (std::size_t i = 0; i < block.group_count; ++i) {
            const std::size_t index = static_cast<std::size_t>(block.group_start) + i;
            if (index == missing_index) {
                continue;
            }
            const std::uint16_t length = block.lengths[i];
            if (length > block.max_len) {
                continue;
            }
            const std::uint8_t* fragment =
                reassembly.data.data() + index * kFragmentBytes;
            for (std::uint16_t j = 0; j < length; ++j) {
                recovered[j] = static_cast<std::uint8_t>(recovered[j] ^ fragment[j]);
            }
        }
        if (missing_len > block.max_len) {
            continue;
        }
        std::uint8_t* target = reassembly.data.data() + missing_index * kFragmentBytes;
        if (missing_len > 0) {
            std::memcpy(target, recovered.data(), missing_len);
        }
        reassembly.received[missing_index] = 1;
        ++reassembly.received_count;
        if (missing_index + 1 == reassembly.frag_count) {
            reassembly.last_fragment_bytes = missing_len;
        }
        ++m_stats.fec_recovered;
        recovered_any = true;
        if (reassembly.received_count == reassembly.frag_count) {
            break;
        }
    }
    return recovered_any;
}

void VideoChannel::DeliverFrame(std::uint32_t frame_id, Reassembly& reassembly) {
    const std::size_t total_bytes =
        static_cast<std::size_t>(reassembly.frag_count - 1) * kFragmentBytes +
        reassembly.last_fragment_bytes;
    std::vector<std::uint8_t> complete(
        reassembly.data.begin(),
        reassembly.data.begin() + static_cast<std::ptrdiff_t>(total_bytes));
    const bool key = reassembly.key;
    m_delivered_frame_id = frame_id;
    ++m_stats.frames_delivered;

    // 交付后清理所有 <= delivered 的重组项与 FEC 状态；回调里可能继续发
    // 新帧，所以先清状态再交付。
    for (auto active_it = m_active.begin(); active_it != m_active.end();) {
        if (!SeqGreater(active_it->first, m_delivered_frame_id)) {
            m_fec.erase(active_it->first);
            active_it = m_active.erase(active_it);
        } else {
            ++active_it;
        }
    }
    if (m_deliver) {
        m_deliver(frame_id, key, complete.data(), complete.size());
    }
}

void VideoChannel::OnTimer(std::uint64_t now_ms) {
    auto it = m_active.begin();
    while (it != m_active.end()) {
        if (now_ms >= it->second.last_update_ms &&
            now_ms - it->second.last_update_ms >= kReassemblyTimeoutMs) {
            m_fec.erase(it->first);
            it = m_active.erase(it);
            ++m_stats.frames_incomplete_dropped;
        } else {
            ++it;
        }
    }
}

void VideoChannel::DropReassembly(std::map<std::uint32_t, Reassembly>::iterator it) {
    if (it == m_active.end()) {
        return;
    }
    m_fec.erase(it->first);
    m_active.erase(it);
    ++m_stats.frames_incomplete_dropped;
}

}  // namespace go2cpp::bento
