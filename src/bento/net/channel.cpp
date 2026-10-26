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

}  // namespace

// ---------------------------------------------------------------------------
// ReliableChannel
// ---------------------------------------------------------------------------

ReliableChannel::ReliableChannel(std::uint8_t channel_id, Relation* relation, DeliverFn deliver)
    : m_channel_id(channel_id), m_relation(relation), m_deliver(std::move(deliver)) {}

std::uint32_t ReliableChannel::PtoFor(std::uint32_t retransmits) noexcept {
    std::uint32_t pto = kInitialPtoMs;
    for (std::uint32_t i = 0; i < retransmits && pto < kMaxPtoMs; ++i) {
        pto *= 2;
    }
    return std::min(pto, kMaxPtoMs);
}

bool ReliableChannel::Send(const std::uint8_t* data, std::size_t bytes, std::uint32_t dep_seq) {
    if (bytes > kMaxFramePayload || (bytes > 0 && data == nullptr)) {
        ++m_stats.send_failures;
        return false;
    }
    // 窗口满时返回 false 但不计入 send_failures：这是背压，不是链路故障。
    if (m_unsent.size() + m_inflight.size() >= kSendWindow) {
        return false;
    }

    Outgoing message;
    message.seq = m_next_tx_seq++;
    if (m_next_tx_seq == 0) {
        m_next_tx_seq = 1;  // seq 0 保留为「无效」
    }
    message.dep_seq = dep_seq;
    if (bytes > 0) {
        message.payload.assign(data, data + bytes);
    }
    m_unsent.push_back(std::move(message));
    Pump(NowMs());
    m_stats.inflight = m_inflight.size();
    m_stats.unsent = m_unsent.size();
    return true;
}

void ReliableChannel::Pump(std::uint64_t now_ms) {
    while (!m_unsent.empty() && m_inflight.size() < kSendWindow) {
        Outgoing message = std::move(m_unsent.front());
        if (!SendNow(message, false, now_ms)) {
            // 放回队首，保留 seq，等下一次 timer 重试；不能丢弃，
            // 否则接收端的 expected 会永久卡住。
            m_unsent.push_front(std::move(message));
            break;
        }
        m_unsent.pop_front();
        m_inflight.push_back(std::move(message));
        ++m_stats.messages_sent;
    }
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

    m_rx_seen.insert(seq);
    m_rx_buffer.emplace(seq, std::vector<std::uint8_t>(payload, payload + bytes));
    m_pending_ack = true;
    AdvanceDelivery();
    m_stats.inflight = m_inflight.size();
    m_stats.unsent = m_unsent.size();
}

void ReliableChannel::AdvanceDelivery() {
    while (m_rx_seen.erase(m_next_rx_seq) != 0) {
        auto it = m_rx_buffer.find(m_next_rx_seq);
        if (it != m_rx_buffer.end()) {
            // 先摘下再交付：回调里可能调用 Send()，不能持有 map 迭代器。
            std::vector<std::uint8_t> payload = std::move(it->second);
            m_rx_buffer.erase(it);
            ++m_stats.messages_delivered;
            if (m_deliver) {
                m_deliver(m_next_rx_seq, payload.data(), payload.size());
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
    const std::uint64_t now_ms = NowMs();

    // 累积确认：ack_seq 之前的所有消息都已收到。
    while (!m_inflight.empty() && SeqGreater(ack.ack_seq, m_inflight.front().seq)) {
        Acknowledge(m_inflight.front(), now_ms);
        m_inflight.pop_front();
    }

    // SACK：ack_bits 的 bit i 表示 ack_seq+i 已收到（可能跨过空洞）。
    for (auto it = m_inflight.begin(); it != m_inflight.end();) {
        const std::uint32_t delta = it->seq - ack.ack_seq;
        if (delta < 32 && (ack.ack_bits & (1U << delta)) != 0) {
            Acknowledge(*it, now_ms);
            it = m_inflight.erase(it);
        } else {
            ++it;
        }
    }

    // ACK 腾出了窗口，立即补发排队消息。
    Pump(now_ms);
    m_stats.inflight = m_inflight.size();
    m_stats.unsent = m_unsent.size();
}

void ReliableChannel::Acknowledge(const Outgoing& message, std::uint64_t now_ms) {
    ++m_stats.acks_applied;
    // Karn 算法：重传过的消息不采样 RTT，避免把重传等待算进 RTT。
    if (!message.ever_retransmitted && now_ms >= message.first_send_ms) {
        m_stats.last_rtt_ms = ClampRtt(now_ms - message.first_send_ms);
    }
}

void ReliableChannel::OnTimer(std::uint64_t now_ms) {
    Pump(now_ms);
    for (auto it = m_inflight.begin(); it != m_inflight.end(); ++it) {
        const std::uint32_t pto = PtoFor(it->retransmits);
        if (now_ms < it->last_send_ms || now_ms - it->last_send_ms < pto) {
            continue;
        }
        ++it->retransmits;
        it->ever_retransmitted = true;
        ++m_stats.retransmits;
        if (!SendNow(*it, true, now_ms)) {
            // 发送失败也推进时间戳，否则同一次 Tick 会反复重试。
            it->last_send_ms = now_ms;
        }
    }
    m_stats.inflight = m_inflight.size();
    m_stats.unsent = m_unsent.size();
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
    : m_sink(std::move(sink)), m_deliver(std::move(deliver)) {}

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
    return true;
}

void VideoChannel::OnFrame(const FrameHeader& header, const std::uint8_t* payload,
                           std::size_t bytes) {
    if (header.channel != kChannelVideo) {
        ++m_stats.fragments_invalid;
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

    if (reassembly.received_count != reassembly.frag_count) {
        return;
    }

    const std::size_t total_bytes =
        static_cast<std::size_t>(reassembly.frag_count - 1) * kFragmentBytes +
        reassembly.last_fragment_bytes;
    std::vector<std::uint8_t> complete(
        reassembly.data.begin(),
        reassembly.data.begin() + static_cast<std::ptrdiff_t>(total_bytes));
    const std::uint32_t frame_id = video_header.frame_id;
    const bool key = (header.flags & kFlagKey) != 0;
    m_delivered_frame_id = frame_id;
    ++m_stats.frames_delivered;

    // 交付后清理所有 <= delivered 的重组项；回调里可能继续发新帧，
    // 所以先清状态再交付。
    for (auto active_it = m_active.begin(); active_it != m_active.end();) {
        if (!SeqGreater(active_it->first, m_delivered_frame_id)) {
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
    m_active.erase(it);
    ++m_stats.frames_incomplete_dropped;
}

}  // namespace go2cpp::bento
