/**
 * @file p2p_channel.cpp
 * @brief P2P 通道实现：候选收集、ICE 连通性检查、中继降级与数据面。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/p2p_channel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "go2cpp/p2p/random.hpp"
#include "go2cpp/p2p/stun_client.hpp"
#include "go2cpp/p2p/stun_message.hpp"
#include "go2cpp/p2p/turn.hpp"

namespace go2cpp::p2p {
namespace {

using Clock = std::chrono::steady_clock;

/// 数据帧 magic：ASCII "G2PD"。
constexpr std::uint32_t kDataMagic = 0x47325044U;
constexpr std::uint8_t kDataVersion = 1;
constexpr std::size_t kDataHeaderSize = 12;
/// 连接阶段接收轮询的最长单次等待（毫秒）。
constexpr int kConnectPollMs = 20;
/// 已连接后接收轮询的最长单次等待（毫秒）。
constexpr int kRecvPollMs = 50;
/// 记录待匹配 STUN 检查事务的上限。
constexpr std::size_t kMaxPendingChecks = 32;

enum class FrameType : std::uint8_t {
    kData = 1,
    kKeepalive = 2,
    kKeepaliveAck = 3,
};

std::uint16_t ReadU16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) |
                                      static_cast<std::uint16_t>(data[1]));
}

std::uint32_t ReadU32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value >> 8U));
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

std::vector<std::uint8_t> MakeFrame(FrameType type, std::uint32_t sequence,
                                    const void* payload, std::size_t length) {
    std::vector<std::uint8_t> frame;
    frame.reserve(kDataHeaderSize + length);
    AppendU32(&frame, kDataMagic);
    frame.push_back(kDataVersion);
    frame.push_back(static_cast<std::uint8_t>(type));
    AppendU16(&frame, static_cast<std::uint16_t>(length));
    AppendU32(&frame, sequence);
    const auto* bytes = static_cast<const std::uint8_t*>(payload);
    if (length > 0) {
        frame.insert(frame.end(), bytes, bytes + length);
    }
    return frame;
}

bool ParseFrame(const std::uint8_t* data, std::size_t length, FrameType* type,
                std::uint32_t* sequence, const std::uint8_t** payload,
                std::size_t* payload_length) {
    if (data == nullptr || length < kDataHeaderSize) {
        return false;
    }
    if (ReadU32(data) != kDataMagic || data[4] != kDataVersion) {
        return false;
    }
    const std::uint8_t raw_type = data[5];
    if (raw_type < static_cast<std::uint8_t>(FrameType::kData) ||
        raw_type > static_cast<std::uint8_t>(FrameType::kKeepaliveAck)) {
        return false;
    }
    const std::uint16_t declared = ReadU16(data + 6);
    if (kDataHeaderSize + declared != length) {
        return false;
    }
    *type = static_cast<FrameType>(raw_type);
    *sequence = ReadU32(data + 8);
    *payload = data + kDataHeaderSize;
    *payload_length = declared;
    return true;
}

bool LooksLikeStun(const std::uint8_t* data, std::size_t length) {
    return data != nullptr && length >= kStunHeaderSize && ReadU32(data + 4) == kStunMagicCookie;
}

std::chrono::milliseconds Remaining(Clock::time_point deadline) {
    const Clock::time_point now = Clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds(0);
    }
    return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                    std::chrono::milliseconds(1));
}

std::chrono::milliseconds ClampPoll(std::chrono::milliseconds remaining, int limit_ms) {
    if (remaining.count() > limit_ms) {
        return std::chrono::milliseconds(limit_ms);
    }
    return std::max(remaining, std::chrono::milliseconds(1));
}

}  // namespace

const char* P2PStateName(P2PState state) noexcept {
    switch (state) {
        case P2PState::kIdle:
            return "idle";
        case P2PState::kGathering:
            return "gathering";
        case P2PState::kConnecting:
            return "connecting";
        case P2PState::kConnected:
            return "connected";
        case P2PState::kRelayed:
            return "relayed";
        case P2PState::kFailed:
            return "failed";
        case P2PState::kClosed:
            return "closed";
    }
    return "unknown";
}

class P2PChannel::Impl {
public:
    explicit Impl(P2PConfig config) : m_config(std::move(config)) {
        if (m_config.transport) {
            m_transport = m_config.transport;
            return;
        }
        auto socket = std::make_unique<UdpTransport>(BuildUdpConfig());
        std::string error;
        if (!socket->Start(&error)) {
            m_error = "创建本地 UDP socket 失败: " + error;
            return;
        }
        m_transport = std::shared_ptr<DatagramTransport>(std::move(socket));
    }

    ~Impl() { Close(); }

    std::vector<Candidate> GatherCandidates() {
        if (!m_transport) {
            return {};
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state == P2PState::kClosed) {
            return {};
        }
        m_state = P2PState::kGathering;
        m_local_candidates.clear();
        m_error.clear();
        const Endpoint local = m_transport->LocalEndpoint();

        if (m_config.gather_host_candidates) {
            std::vector<Endpoint> addresses;
            if (!local.valid() || !local.IsUnspecified()) {
                if (local.valid()) {
                    addresses.push_back(local);
                }
            } else {
                addresses = EnumerateLocalAddresses(local.port(),
                                                    m_config.gather_loopback_candidates);
            }
            for (const Endpoint& address : addresses) {
                if (!address.valid()) {
                    continue;
                }
                if (!m_config.gather_loopback_candidates && address.IsLoopback()) {
                    continue;
                }
                Candidate candidate;
                candidate.type = CandidateType::kHost;
                candidate.endpoint = address;
                candidate.base = address;
                m_local_candidates.push_back(candidate);
                if (m_local_candidates.size() >= m_config.max_local_candidates) {
                    break;
                }
            }
        }

        for (const Endpoint& server : m_config.stun_servers) {
            if (!server.valid()) {
                continue;
            }
            StunClient client(m_transport.get(), server);
            std::string error;
            const auto mapped = client.Binding(m_config.gather_timeout, &error);
            if (!mapped.has_value()) {
                m_error = "STUN(" + server.ToString() + ") 失败: " + error;
                continue;
            }
            Candidate candidate;
            candidate.type = CandidateType::kSrflx;
            candidate.endpoint = *mapped;
            candidate.base = local.valid() && !local.IsUnspecified() ? local : *mapped;
            m_local_candidates.push_back(candidate);
            if (m_local_candidates.size() >= m_config.max_local_candidates) {
                break;
            }
        }

        if (m_config.enable_relay && m_config.turn_server.valid()) {
            if (!m_relay) {
                RelayClientConfig relay_config;
                relay_config.server = m_config.turn_server;
                relay_config.request_timeout = m_config.gather_timeout;
                m_relay = std::make_unique<RelayClient>(m_transport.get(), relay_config);
            }
            std::string error;
            if (m_relay->Allocate(&error)) {
                Candidate candidate;
                candidate.type = CandidateType::kRelay;
                candidate.endpoint = m_relay->relayed_endpoint();
                candidate.base = m_config.turn_server;
                m_local_candidates.push_back(candidate);
            } else {
                m_error = "TURN 分配失败: " + error;
            }
        }

        for (std::size_t index = 0; index < m_local_candidates.size(); ++index) {
            FinalizeCandidate(&m_local_candidates[index],
                              static_cast<std::uint32_t>(65535U - std::min<std::size_t>(index, 255U)));
        }
        SortCandidatesByPriority(&m_local_candidates);
        m_state = P2PState::kIdle;
        return m_local_candidates;
    }

    void AddRemoteCandidate(const Candidate& candidate) {
        if (!candidate.endpoint.valid()) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const Candidate& existing : m_remote_candidates) {
            if (existing.endpoint == candidate.endpoint && existing.type == candidate.type) {
                return;
            }
        }
        Candidate normalized = candidate;
        FinalizeCandidate(&normalized);
        m_remote_candidates.push_back(normalized);
        SortCandidatesByPriority(&m_remote_candidates);
    }

    bool Connect(std::chrono::milliseconds timeout) {
        if (!m_transport) {
            SetError("传输未初始化");
            return false;
        }
        if (timeout.count() <= 0) {
            timeout = m_config.connect_timeout;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_state == P2PState::kConnected || m_state == P2PState::kRelayed) {
                return true;
            }
            if (m_state == P2PState::kClosed) {
                return false;
            }
            m_state = P2PState::kConnecting;
            m_error.clear();
        }
        const Clock::time_point started = Clock::now();
        const Clock::time_point deadline = started + timeout;

        if (m_local_candidates.empty()) {
            GatherCandidates();
        }
        // 配置了信令且启用自动交换时，总是交换候选：调用方可能已经手动塞入
        // 了额外候选（例如测试里的黑洞候选），不能因为「远端列表非空」就
        // 跳过交换，否则永远拿不到对端的真实候选。
        if (m_config.signaling != nullptr && m_config.auto_exchange_candidates) {
            const auto exchange_deadline =
                std::min(deadline, started + std::chrono::milliseconds(timeout.count() / 2));
            if (!ExchangeCandidates(exchange_deadline)) {
                SetFailed("信令交换候选失败: " + LastError());
                return false;
            }
        }
        const std::vector<Candidate> remote = RemoteCandidateSnapshot();
        if (remote.empty()) {
            SetFailed("没有远端候选，无法连接");
            return false;
        }

        std::vector<Endpoint> direct_targets;
        std::vector<Endpoint> relay_targets;
        for (const Candidate& candidate : remote) {
            if (candidate.type == CandidateType::kRelay) {
                if (m_config.enable_relay) {
                    relay_targets.push_back(candidate.endpoint);
                }
            } else {
                direct_targets.push_back(candidate.endpoint);
            }
        }
        SortEndpointsByRemotePriority(&direct_targets, remote);

        // 没有可用的中继候选时，把整个预算都给直连；否则直连先用 70%，
        // 剩下 30% 留给中继降级。
        const bool can_use_relay = m_config.enable_relay && !relay_targets.empty();
        const auto direct_deadline =
            can_use_relay
                ? std::min(deadline,
                           started + std::chrono::milliseconds(timeout.count() * 7 / 10))
                : deadline;
        if (!direct_targets.empty() &&
            TryDirect(direct_targets, direct_deadline, started)) {
            return true;
        }
        if (!relay_targets.empty() && m_config.enable_relay) {
            if (TryRelay(relay_targets, deadline, started)) {
                return true;
            }
        }
        SetFailed("连接超时：直连与中继均未成功（远端候选 " +
                  std::to_string(remote.size()) + " 个）");
        return false;
    }

    bool Send(const void* data, std::size_t length) {
        if (data == nullptr || length == 0) {
            SetError("Send: 数据为空");
            return false;
        }
        if (length > kP2PMaxPayload) {
            SetError("Send: 载荷超过 " + std::to_string(kP2PMaxPayload) + " 字节");
            return false;
        }
        Endpoint peer;
        bool relayed = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_state != P2PState::kConnected && m_state != P2PState::kRelayed) {
                return false;
            }
            peer = m_peer;
            relayed = m_state == P2PState::kRelayed;
        }
        if (!peer.valid()) {
            return false;
        }
        const std::uint32_t sequence = ++m_send_sequence;
        const std::vector<std::uint8_t> frame =
            MakeFrame(FrameType::kData, sequence, data, length);
        const bool sent = relayed ? SendViaRelay(peer, frame) : SendDirect(peer, frame);
        if (sent) {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.packets_sent;
            m_stats.bytes_sent += length;
            if (relayed) {
                ++m_stats.relayed_packets_sent;
            }
        }
        return sent;
    }

    std::size_t Recv(void* buffer, std::size_t capacity, std::chrono::milliseconds timeout) {
        if (buffer == nullptr && capacity > 0) {
            return 0;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_state != P2PState::kConnected && m_state != P2PState::kRelayed) {
                return 0;
            }
        }
        const Clock::time_point deadline = Clock::now() + timeout;
        for (;;) {
            MaybeSendKeepalive();
            if (PeerTimedOut()) {
                SetFailed("对端静默超过 peer_timeout");
                return 0;
            }
            const std::chrono::milliseconds left = Remaining(deadline);
            if (left.count() <= 0) {
                return 0;
            }
            Endpoint source;
            const bool relayed = IsRelayed();
            const int received = relayed ? RecvViaRelay(&source, ClampPoll(left, kRecvPollMs))
                                         : RecvDirect(&source, ClampPoll(left, kRecvPollMs));
            if (received == 0) {
                continue;
            }
            if (received < 0) {
                SetError("Recv: 接收失败");
                return 0;
            }
            TouchPeer();
            const auto* bytes = m_recv_buffer.data();
            if (LooksLikeStun(bytes, static_cast<std::size_t>(received))) {
                HandleStun(bytes, static_cast<std::size_t>(received), source, relayed);
                continue;
            }
            FrameType type = FrameType::kData;
            std::uint32_t sequence = 0;
            const std::uint8_t* payload = nullptr;
            std::size_t payload_length = 0;
            if (!ParseFrame(bytes, static_cast<std::size_t>(received), &type, &sequence, &payload,
                            &payload_length)) {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_stats.dropped_packets;
                continue;
            }
            if (type == FrameType::kKeepalive) {
                const std::vector<std::uint8_t> ack =
                    MakeFrame(FrameType::kKeepaliveAck, sequence, nullptr, 0);
                (void)(relayed ? SendViaRelay(source, ack) : SendDirect(source, ack));
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_stats.keepalives_received;
                continue;
            }
            if (type == FrameType::kKeepaliveAck) {
                UpdateRtt();
                continue;
            }
            if (!SourceMatchesPeer(source)) {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_stats.dropped_packets;
                continue;
            }
            const std::size_t copied = std::min(payload_length, capacity);
            if (copied > 0) {
                std::memcpy(buffer, payload, copied);
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_stats.packets_received;
                m_stats.bytes_received += copied;
                if (relayed) {
                    ++m_stats.relayed_packets_received;
                }
                UpdateSequenceGapLocked(sequence);
                m_last_receive = Clock::now();
            }
            return copied;
        }
    }

    bool IsRelayed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state == P2PState::kRelayed;
    }

    bool IsConnected() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state == P2PState::kConnected || m_state == P2PState::kRelayed;
    }

    P2PState state() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }

    Endpoint LocalEndpoint() const {
        return m_transport ? m_transport->LocalEndpoint() : Endpoint();
    }

    Endpoint PeerEndpoint() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_peer;
    }

    std::vector<Candidate> LocalCandidates() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_local_candidates;
    }

    std::vector<Candidate> RemoteCandidates() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_remote_candidates;
    }

    P2PStats Stats() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats;
    }

    std::string LastError() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_error;
    }

    void Close() {
        std::unique_ptr<RelayClient> relay;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_state == P2PState::kClosed) {
                return;
            }
            m_state = P2PState::kClosed;
            m_peer = Endpoint();
            relay = std::move(m_relay);
        }
        if (relay) {
            relay->Release();
        }
    }

private:
    UdpTransportConfig BuildUdpConfig() const {
        UdpTransportConfig udp;
        udp.bind_endpoint = m_config.bind_endpoint;
        udp.receive_buffer_bytes = m_config.receive_buffer_bytes;
        udp.send_buffer_bytes = m_config.receive_buffer_bytes;
        return udp;
    }

    std::vector<Candidate> RemoteCandidateSnapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_remote_candidates;
    }

    void SetError(const std::string& error) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = error;
    }

    void SetFailed(const std::string& error) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != P2PState::kClosed) {
            m_state = P2PState::kFailed;
        }
        m_error = error;
    }

    void TouchPeer() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_receive = Clock::now();
    }

    bool PeerTimedOut() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != P2PState::kConnected && m_state != P2PState::kRelayed) {
            return false;
        }
        if (m_config.peer_timeout.count() <= 0) {
            return false;
        }
        return Clock::now() - m_last_receive > m_config.peer_timeout;
    }

    void UpdateSequenceGapLocked(std::uint32_t sequence) {
        if (!m_has_data_sequence) {
            m_has_data_sequence = true;
            m_last_data_sequence = sequence;
            return;
        }
        if (sequence > m_last_data_sequence) {
            const std::uint32_t gap = sequence - m_last_data_sequence - 1U;
            if (gap > 0 && gap < 100000U) {
                m_stats.sequence_gaps += gap;
            }
            m_last_data_sequence = sequence;
        }
    }

    void UpdateRtt() {
        const Clock::time_point now = Clock::now();
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_has_pending_keepalive) {
            return;
        }
        m_has_pending_keepalive = false;
        const double rtt =
            std::chrono::duration<double, std::milli>(now - m_last_keepalive_at).count();
        m_stats.rtt_ms = rtt;
        m_rtt_samples += 1;
        m_rtt_sum += rtt;
        m_stats.rtt_avg_ms = m_rtt_sum / static_cast<double>(m_rtt_samples);
    }

    void MaybeSendKeepalive() {
        if (m_config.keepalive_interval.count() <= 0) {
            return;
        }
        Endpoint peer;
        bool relayed = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_state != P2PState::kConnected && m_state != P2PState::kRelayed) {
                return;
            }
            const Clock::time_point now = Clock::now();
            if (now - m_last_keepalive_at < m_config.keepalive_interval) {
                return;
            }
            m_last_keepalive_at = now;
            m_has_pending_keepalive = true;
            peer = m_peer;
            relayed = m_state == P2PState::kRelayed;
        }
        if (!peer.valid()) {
            return;
        }
        const std::uint32_t sequence = ++m_keepalive_sequence;
        const std::vector<std::uint8_t> frame =
            MakeFrame(FrameType::kKeepalive, sequence, nullptr, 0);
        const bool sent = relayed ? SendViaRelay(peer, frame) : SendDirect(peer, frame);
        if (sent) {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.keepalives_sent;
        }
    }

    void UpdateLastReceiveLocked() { m_last_receive = Clock::now(); }

    bool SendDirect(const Endpoint& destination, const std::vector<std::uint8_t>& wire) {
        return m_transport != nullptr &&
               m_transport->SendTo(destination, wire.data(), wire.size());
    }

    bool SendViaRelay(const Endpoint& destination, const std::vector<std::uint8_t>& wire) {
        std::unique_ptr<RelayClient>* relay = &m_relay;
        if (*relay == nullptr) {
            return false;
        }
        return (*relay)->SendTo(destination, wire.data(), wire.size());
    }

    int RecvDirect(Endpoint* source, std::chrono::milliseconds timeout) {
        if (m_transport == nullptr) {
            return -1;
        }
        return m_transport->RecvFrom(source, m_recv_buffer.data(), m_recv_buffer.size(), timeout);
    }

    int RecvViaRelay(Endpoint* source, std::chrono::milliseconds timeout) {
        if (m_relay == nullptr) {
            return -1;
        }
        return m_relay->RecvFrom(source, m_recv_buffer.data(), m_recv_buffer.size(), timeout);
    }

    bool SourceMatchesPeer(const Endpoint& source) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return source == m_peer;
    }

    void HandleStun(const std::uint8_t* data, std::size_t length, const Endpoint& source,
                    bool relayed) {
        const auto message = ParseStunMessage(data, length);
        if (!message.has_value() || message->method != kStunMethodBinding) {
            return;
        }
        if (message->klass == StunClass::kRequest) {
            // 连接后收到对端重传的检查包：回一个 Success，避免对端超时重试。
            StunMessage response = StunMessage::MakeBindingSuccess(message->transaction_id, source);
            const std::vector<std::uint8_t> wire = SerializeStunMessage(response);
            if (!wire.empty()) {
                (void)(relayed ? SendViaRelay(source, wire) : SendDirect(source, wire));
            }
            return;
        }
        if (message->klass == StunClass::kSuccessResponse) {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.connectivity_checks_received;
        }
    }

    /**
     * @brief 通过 signaling 发送本地候选并等待远端候选。
     * @details 双方都先 Send 再 Recv，因此即使一端先调用也不会死锁：
     *          先 Send 的一方会在自己的 Recv 里等到后一方的候选。
     */
    bool ExchangeCandidates(Clock::time_point deadline) {
        const std::string payload = SerializeCandidates(LocalCandidates());
        if (!m_config.signaling->Send(payload)) {
            SetError("信令发送失败");
            return false;
        }
        while (Clock::now() < deadline) {
            std::string incoming;
            const std::chrono::milliseconds left = Remaining(deadline);
            const int received = m_config.signaling->Recv(
                &incoming, std::min(left, std::chrono::milliseconds(100)));
            if (received < 0) {
                SetError("信令通道已关闭");
                return false;
            }
            if (received == 0) {
                continue;
            }
            std::string error;
            const std::vector<Candidate> parsed = ParseCandidates(incoming, &error);
            if (parsed.empty()) {
                SetError("远端候选解析失败: " + error);
                continue;
            }
            for (const Candidate& candidate : parsed) {
                AddRemoteCandidate(candidate);
            }
            if (!RemoteCandidateSnapshot().empty()) {
                return true;
            }
        }
        SetError("等待远端候选超时");
        return false;
    }

    void SortEndpointsByRemotePriority(std::vector<Endpoint>* endpoints,
                                       const std::vector<Candidate>& remote) const {
        std::stable_sort(endpoints->begin(), endpoints->end(),
                         [&remote](const Endpoint& left, const Endpoint& right) {
                             const auto priority_of = [&remote](const Endpoint& endpoint) {
                                 for (const Candidate& candidate : remote) {
                                     if (candidate.endpoint == endpoint) {
                                         return candidate.priority;
                                     }
                                 }
                                 return 0U;
                             };
                             return priority_of(left) > priority_of(right);
                         });
    }

    bool TryDirect(const std::vector<Endpoint>& targets, Clock::time_point deadline,
                   Clock::time_point started) {
        std::vector<std::array<std::uint8_t, kStunTransactionIdSize>> pending;
        Clock::time_point next_check = started;
        const auto interval = std::max(m_config.check_interval, std::chrono::milliseconds(1));
        while (Clock::now() < deadline) {
            const Clock::time_point now = Clock::now();
            if (now >= next_check) {
                for (const Endpoint& target : targets) {
                    if (!SendCheckDirect(target, &pending)) {
                        continue;
                    }
                }
                next_check = now + interval;
            }
            Endpoint source;
            const int received = RecvDirect(&source, ClampPoll(Remaining(deadline), kConnectPollMs));
            if (received == 0) {
                continue;
            }
            if (received < 0) {
                SetFailed("直连检查接收失败");
                return false;
            }
            const auto* bytes = m_recv_buffer.data();
            if (!LooksLikeStun(bytes, static_cast<std::size_t>(received))) {
                continue;
            }
            const auto message = ParseStunMessage(bytes, static_cast<std::size_t>(received));
            if (!message.has_value() || message->method != kStunMethodBinding) {
                continue;
            }
            if (message->klass == StunClass::kRequest) {
                StunMessage response =
                    StunMessage::MakeBindingSuccess(message->transaction_id, source);
                const std::vector<std::uint8_t> wire = SerializeStunMessage(response);
                if (!wire.empty()) {
                    (void)SendDirect(source, wire);
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_stats.connectivity_checks_received;
                EstablishLocked(source, false);
                return true;
            }
            if (message->klass == StunClass::kSuccessResponse &&
                IsPendingTransaction(pending, message->transaction_id)) {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_stats.connectivity_checks_received;
                EstablishLocked(source, false);
                return true;
            }
        }
        return false;
    }

    bool TryRelay(const std::vector<Endpoint>& targets, Clock::time_point deadline,
                  Clock::time_point started) {
        if (m_relay == nullptr) {
            RelayClientConfig relay_config;
            relay_config.server = m_config.turn_server;
            relay_config.request_timeout = m_config.gather_timeout;
            m_relay = std::make_unique<RelayClient>(m_transport.get(), relay_config);
        }
        if (!m_relay->allocated()) {
            std::string error;
            if (!m_relay->Allocate(&error)) {
                SetError("中继分配失败: " + error);
                return false;
            }
        }
        std::vector<std::array<std::uint8_t, kStunTransactionIdSize>> pending;
        Clock::time_point next_check = started;
        const auto interval = std::max(m_config.check_interval, std::chrono::milliseconds(1));
        while (Clock::now() < deadline) {
            const Clock::time_point now = Clock::now();
            if (now >= next_check) {
                for (const Endpoint& target : targets) {
                    const StunMessage request = StunMessage::MakeBindingRequest();
                    const std::vector<std::uint8_t> wire = SerializeStunMessage(request);
                    if (wire.empty()) {
                        continue;
                    }
                    if (m_relay->SendTo(target, wire.data(), wire.size())) {
                        pending.push_back(request.transaction_id);
                        if (pending.size() > kMaxPendingChecks) {
                            pending.erase(pending.begin());
                        }
                        std::lock_guard<std::mutex> lock(m_mutex);
                        ++m_stats.connectivity_checks_sent;
                    }
                }
                next_check = now + interval;
            }
            Endpoint source;
            const int received =
                m_relay->RecvFrom(&source, m_recv_buffer.data(), m_recv_buffer.size(),
                                  ClampPoll(Remaining(deadline), kConnectPollMs));
            if (received == 0) {
                continue;
            }
            if (received < 0) {
                SetFailed("中继检查接收失败");
                return false;
            }
            const auto* bytes = m_recv_buffer.data();
            if (!LooksLikeStun(bytes, static_cast<std::size_t>(received))) {
                continue;
            }
            const auto message = ParseStunMessage(bytes, static_cast<std::size_t>(received));
            if (!message.has_value() || message->method != kStunMethodBinding) {
                continue;
            }
            if (message->klass == StunClass::kRequest) {
                StunMessage response =
                    StunMessage::MakeBindingSuccess(message->transaction_id, source);
                const std::vector<std::uint8_t> wire = SerializeStunMessage(response);
                if (!wire.empty()) {
                    (void)m_relay->SendTo(source, wire.data(), wire.size());
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_stats.connectivity_checks_received;
                EstablishLocked(source, true);
                return true;
            }
            if (message->klass == StunClass::kSuccessResponse &&
                IsPendingTransaction(pending, message->transaction_id)) {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_stats.connectivity_checks_received;
                EstablishLocked(source, true);
                return true;
            }
        }
        return false;
    }

    bool SendCheckDirect(
        const Endpoint& target,
        std::vector<std::array<std::uint8_t, kStunTransactionIdSize>>* pending) {
        const StunMessage request = StunMessage::MakeBindingRequest();
        const std::vector<std::uint8_t> wire = SerializeStunMessage(request);
        if (wire.empty() || !SendDirect(target, wire)) {
            return false;
        }
        pending->push_back(request.transaction_id);
        if (pending->size() > kMaxPendingChecks) {
            pending->erase(pending->begin());
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.connectivity_checks_sent;
        return true;
    }

    static bool IsPendingTransaction(
        const std::vector<std::array<std::uint8_t, kStunTransactionIdSize>>& pending,
        const std::array<std::uint8_t, kStunTransactionIdSize>& transaction_id) {
        for (const auto& item : pending) {
            if (item == transaction_id) {
                return true;
            }
        }
        return false;
    }

    void EstablishLocked(const Endpoint& peer, bool relayed) {
        m_peer = peer;
        m_state = relayed ? P2PState::kRelayed : P2PState::kConnected;
        m_last_receive = Clock::now();
        m_last_keepalive_at = Clock::now();
        m_has_pending_keepalive = false;
        m_has_data_sequence = false;
        m_error.clear();
    }

    P2PConfig m_config;
    std::shared_ptr<DatagramTransport> m_transport;
    mutable std::mutex m_mutex;
    std::vector<Candidate> m_local_candidates;
    std::vector<Candidate> m_remote_candidates;
    P2PState m_state = P2PState::kIdle;
    Endpoint m_peer;
    std::string m_error;
    P2PStats m_stats;
    Clock::time_point m_last_receive = Clock::now();
    Clock::time_point m_last_keepalive_at = Clock::now();
    bool m_has_pending_keepalive = false;
    bool m_has_data_sequence = false;
    std::uint32_t m_last_data_sequence = 0;
    // Send() 允许与 Recv() 在不同线程并发，序号必须是原子的。
    std::atomic<std::uint32_t> m_send_sequence{0};
    std::uint32_t m_keepalive_sequence = 0;
    std::uint64_t m_rtt_samples = 0;
    double m_rtt_sum = 0.0;
    std::unique_ptr<RelayClient> m_relay;
    std::vector<std::uint8_t> m_recv_buffer = std::vector<std::uint8_t>(65536);
};

P2PChannel::P2PChannel(P2PConfig config) : m_impl(std::make_unique<Impl>(std::move(config))) {}

P2PChannel::~P2PChannel() = default;

std::vector<Candidate> P2PChannel::GatherCandidates() {
    return m_impl->GatherCandidates();
}

void P2PChannel::AddRemoteCandidate(const Candidate& candidate) {
    m_impl->AddRemoteCandidate(candidate);
}

bool P2PChannel::Connect(std::chrono::milliseconds timeout) {
    return m_impl->Connect(timeout);
}

bool P2PChannel::Send(const void* data, std::size_t length) {
    return m_impl->Send(data, length);
}

std::size_t P2PChannel::Recv(void* buffer, std::size_t capacity,
                             std::chrono::milliseconds timeout) {
    return m_impl->Recv(buffer, capacity, timeout);
}

bool P2PChannel::IsRelayed() const {
    return m_impl->IsRelayed();
}

bool P2PChannel::IsConnected() const {
    return m_impl->IsConnected();
}

P2PState P2PChannel::state() const {
    return m_impl->state();
}

Endpoint P2PChannel::LocalEndpoint() const {
    return m_impl->LocalEndpoint();
}

Endpoint P2PChannel::PeerEndpoint() const {
    return m_impl->PeerEndpoint();
}

std::vector<Candidate> P2PChannel::LocalCandidates() const {
    return m_impl->LocalCandidates();
}

std::vector<Candidate> P2PChannel::RemoteCandidates() const {
    return m_impl->RemoteCandidates();
}

P2PStats P2PChannel::Stats() const {
    return m_impl->Stats();
}

std::string P2PChannel::LastError() const {
    return m_impl->LastError();
}

void P2PChannel::Close() {
    m_impl->Close();
}

}  // namespace go2cpp::p2p
