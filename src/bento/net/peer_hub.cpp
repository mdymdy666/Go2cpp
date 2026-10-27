/**
 * @file peer_hub.cpp
 * @brief 多 peer UDP 链路实现：单 socket 收发、conn_id 路由、每 peer 独立通道。
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include "go2cpp/bento/net/peer_hub.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

namespace go2cpp::bento {
namespace {

/// HELLO / HELLO_ACK 载荷字节数：4B magic + 4B session + 4B conn_id。
constexpr std::size_t kHelloPayloadBytes = 12;

/// HELLO / HELLO_ACK 载荷 magic：'B' 'E' 'N' 'T'（与 Link 同格式）。
bool IsHelloMagic(const std::uint8_t* payload) noexcept {
    return payload != nullptr && payload[0] == 'B' && payload[1] == 'E' && payload[2] == 'N' &&
           payload[3] == 'T';
}

void WriteU32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t ReadU32(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

void WriteU64(std::uint8_t* out, std::uint64_t value) noexcept {
    WriteU32(out, static_cast<std::uint32_t>(value >> 32));
    WriteU32(out + 4, static_cast<std::uint32_t>(value));
}

std::uint64_t ReadU64(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint64_t>(ReadU32(in)) << 32) | ReadU32(in + 4);
}

void MakeHelloPayload(std::uint32_t session, std::uint32_t conn_id,
                      std::uint8_t out[kHelloPayloadBytes]) noexcept {
    out[0] = 'B';
    out[1] = 'E';
    out[2] = 'N';
    out[3] = 'T';
    WriteU32(out + 4, session);
    WriteU32(out + 8, conn_id);
}

/// 生成非零随机 conn_id（32 位，碰撞概率可忽略；仍做冲突检查）。
std::uint32_t RandomNonZeroU32() {
    std::uint8_t raw[4] = {};
    SecureRandomBytes(raw, sizeof(raw));
    const std::uint32_t value = (static_cast<std::uint32_t>(raw[0]) << 24) |
                                (static_cast<std::uint32_t>(raw[1]) << 16) |
                                (static_cast<std::uint32_t>(raw[2]) << 8) |
                                static_cast<std::uint32_t>(raw[3]);
    return value == 0 ? 1U : value;
}

/// 地址转可读字符串（仅日志/快照用）。
std::string FormatAddress(const sockaddr_storage& addr) {
    char text[INET6_ADDRSTRLEN] = {};
    if (addr.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&addr);
        if (::inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text)) != nullptr) {
            return text;
        }
    } else if (addr.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        if (::inet_ntop(AF_INET6, &v6->sin6_addr, text, sizeof(text)) != nullptr) {
            return text;
        }
    }
    return "<invalid>";
}

/// 取地址端口（主机字节序）。
std::uint16_t PortOf(const sockaddr_storage& addr) noexcept {
    if (addr.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);
    }
    return 0;
}

}  // namespace

/// @brief 一个对端的全部会话状态。
/// @details 每个 peer 持有独立的 Relation（因果水位必须按对端隔离，
///          否则 A 的对端 seq 会推进 B 的水位）与独立的通道表。
struct PeerHub::Peer {
    std::uint32_t peer_id = 0;
    std::uint32_t conn_id = 0;
    std::uint32_t session_id = 0;
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    PeerState state = PeerState::kIdle;
    /// true = 本端主动 AddPeer 发起的；false = 监听端被动接受。
    bool outgoing = false;

    std::uint64_t last_rx_ms = 0;
    std::uint64_t last_tx_ms = 0;
    std::uint64_t handshake_start_ms = 0;
    std::uint64_t next_hello_ms = 0;
    std::uint32_t hello_attempts = 0;
    std::uint32_t hello_pto_ms = 200;
    std::uint64_t next_ack_ms = 0;
    std::uint64_t next_heartbeat_ms = 0;

    std::uint64_t frames_sent = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;

    Relation relation;
    std::map<std::uint8_t, std::unique_ptr<ReliableChannel>> reliable;
    std::map<std::uint8_t, std::unique_ptr<UnreliableChannel>> unreliable;
};

const char* PeerStateName(PeerState state) noexcept {
    switch (state) {
        case PeerState::kIdle:
            return "idle";
        case PeerState::kConnecting:
            return "connecting";
        case PeerState::kConnected:
            return "connected";
        case PeerState::kTimedOut:
            return "timed_out";
        case PeerState::kClosed:
            return "closed";
    }
    return "unknown";
}

PeerHub::PeerHub(PeerHubConfig config) : m_config(std::move(config)), m_rx_buffer(65536) {
    if (m_config.max_peers == 0) {
        m_config.max_peers = 1;
    }
    if (m_config.peer_timeout_ms == 0) {
        m_config.peer_timeout_ms = 3000;
    }
    if (m_config.heartbeat_interval_ms == 0) {
        m_config.heartbeat_interval_ms = 500;
    }
}

PeerHub::~PeerHub() {
    Close();
}

bool PeerHub::Open(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string local_error;
    if (!m_socket.Open(m_config.bind_address, m_config.bind_port, &local_error)) {
        if (error != nullptr) {
            *error = local_error;
        }
        return false;
    }
    const std::uint64_t now = NowMs();
    m_session_id = m_config.random_seed != 0
                       ? m_config.random_seed
                       : static_cast<std::uint32_t>(now ^
                                                    (static_cast<std::uint64_t>(::getpid()) << 16));
    if (m_session_id == 0) {
        m_session_id = 1;
    }
    return true;
}

void PeerHub::Close() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (auto& entry : m_peers) {
        entry.second->state = PeerState::kClosed;
        ResetChannels(*entry.second);
    }
    m_peers.clear();
    m_by_conn_id.clear();
    m_socket.Close();
}

std::uint32_t PeerHub::AllocatePeerId() {
    std::uint32_t candidate = m_next_peer_id == 0 ? 1 : m_next_peer_id;
    while (m_peers.find(candidate) != m_peers.end()) {
        ++candidate;
        if (candidate == 0) {
            candidate = 1;
        }
    }
    m_next_peer_id = candidate + 1;
    if (m_next_peer_id == 0) {
        m_next_peer_id = 1;
    }
    return candidate;
}

PeerHub::Peer* PeerHub::FindByConnId(std::uint32_t conn_id) {
    if (conn_id == 0) {
        return nullptr;
    }
    const auto route = m_by_conn_id.find(conn_id);
    if (route == m_by_conn_id.end()) {
        return nullptr;
    }
    const auto peer = m_peers.find(route->second);
    if (peer == m_peers.end()) {
        return nullptr;
    }
    return peer->second.get();
}

PeerHub::Peer* PeerHub::FindByAddress(const sockaddr_storage& addr, socklen_t len) {
    (void)len;
    for (auto& entry : m_peers) {
        if (UdpSocket::SamePeer(entry.second->addr, addr)) {
            return entry.second.get();
        }
    }
    return nullptr;
}

PeerHub::Peer* PeerHub::CreatePeer(std::uint32_t peer_id, const sockaddr_storage& addr,
                                   socklen_t len) {
    auto peer = std::make_unique<Peer>();
    peer->peer_id = peer_id;
    peer->addr = addr;
    peer->addr_len = len;
    peer->last_rx_ms = NowMs();
    peer->last_tx_ms = peer->last_rx_ms;
    Peer* raw = peer.get();
    m_peers.emplace(peer_id, std::move(peer));
    ++m_stats.peers_created;
    return raw;
}

void PeerHub::ResetChannels(Peer& peer) {
    peer.reliable.clear();
    peer.unreliable.clear();
    peer.relation.Reset();
    peer.next_ack_ms = 0;
    peer.next_heartbeat_ms = 0;
}

ReliableChannel* PeerHub::GetOrCreateReliable(Peer& peer, std::uint8_t channel) {
    const auto existing = peer.reliable.find(channel);
    if (existing != peer.reliable.end()) {
        return existing->second.get();
    }
    if (peer.reliable.size() + peer.unreliable.size() >= kMaxChannels) {
        return nullptr;
    }
    const std::uint32_t peer_id = peer.peer_id;
    auto created = std::make_unique<ReliableChannel>(
        channel, &peer.relation,
        [this, peer_id, channel](std::uint32_t /*seq*/, const std::uint8_t* data,
                                 std::size_t bytes) {
            if (on_stream) {
                on_stream(peer_id, channel, data, bytes);
            }
        },
        m_config.reliable);
    // FrameSink 不能捕获 Peer&：peer 表可能在通道生命周期内被 remove/rehash。
    created->SetFrameSink([this, peer_id](const std::uint8_t* data, std::size_t bytes) {
        const auto peer = m_peers.find(peer_id);
        if (peer == m_peers.end()) {
            return false;
        }
        return SendBuiltFrame(*peer->second, data, bytes);
    });
    ReliableChannel* raw = created.get();
    peer.reliable.emplace(channel, std::move(created));
    return raw;
}

UnreliableChannel* PeerHub::GetOrCreateUnreliable(Peer& peer, std::uint8_t channel) {
    const auto existing = peer.unreliable.find(channel);
    if (existing != peer.unreliable.end()) {
        return existing->second.get();
    }
    if (peer.reliable.size() + peer.unreliable.size() >= kMaxChannels) {
        return nullptr;
    }
    const std::uint32_t peer_id = peer.peer_id;
    auto created = std::make_unique<UnreliableChannel>(
        channel, &peer.relation,
        [this, peer_id, channel](std::uint32_t /*seq*/, const std::uint8_t* data,
                                 std::size_t bytes) {
            if (on_datagram) {
                on_datagram(peer_id, channel, data, bytes);
            }
        });
    created->SetFrameSink([this, peer_id](const std::uint8_t* data, std::size_t bytes) {
        const auto peer = m_peers.find(peer_id);
        if (peer == m_peers.end()) {
            return false;
        }
        return SendBuiltFrame(*peer->second, data, bytes);
    });
    UnreliableChannel* raw = created.get();
    peer.unreliable.emplace(channel, std::move(created));
    return raw;
}

bool PeerHub::SendBuiltFrame(Peer& peer, const std::uint8_t* data, std::size_t bytes) {
    if (!m_socket.IsOpen() || peer.state != PeerState::kConnected) {
        ++m_stats.send_errors;
        return false;
    }
    // 与 Link 一致：conn_id 由链路层统一戳在帧头上，通道只关心自己的载荷。
    std::vector<std::uint8_t> stamped;
    FrameHeader header;
    if (bytes >= kFrameHeaderBytes && DecodeHeader(data, bytes, &header) &&
        header.conn_id != peer.conn_id) {
        header.conn_id = peer.conn_id;
        stamped.assign(data, data + bytes);
        EncodeHeader(header, stamped.data());
        data = stamped.data();
        bytes = stamped.size();
    }
    if (!m_socket.SendTo(peer.addr, peer.addr_len, data, bytes)) {
        ++m_stats.send_errors;
        return false;
    }
    ++m_stats.frames_sent;
    m_stats.bytes_sent += static_cast<std::uint64_t>(bytes);
    ++peer.frames_sent;
    peer.bytes_sent += static_cast<std::uint64_t>(bytes);
    peer.last_tx_ms = NowMs();
    return true;
}

bool PeerHub::SendControl(Peer& peer, const FrameHeader& header, const std::uint8_t* payload,
                          std::size_t bytes) {
    if (!m_socket.IsOpen()) {
        ++m_stats.send_errors;
        return false;
    }
    // 控制帧（ACK/心跳/MAX_DATA/BARRIER）也必须带 conn_id：接收端只能按
    // conn_id 路由，conn_id=0 的帧会被当作未知来源丢弃——这会让 ACK 永远
    // 回不去，发送端窗口卡死（实测：50 条只交付 32 条）。
    FrameHeader stamped = header;
    stamped.conn_id = peer.conn_id;
    const std::vector<std::uint8_t> frame = BuildFrame(stamped, nullptr, payload, bytes);
    if (frame.empty()) {
        ++m_stats.send_errors;
        return false;
    }
    if (!m_socket.SendTo(peer.addr, peer.addr_len, frame.data(), frame.size())) {
        ++m_stats.send_errors;
        return false;
    }
    ++m_stats.frames_sent;
    m_stats.bytes_sent += static_cast<std::uint64_t>(frame.size());
    ++peer.frames_sent;
    peer.bytes_sent += static_cast<std::uint64_t>(frame.size());
    peer.last_tx_ms = NowMs();
    return true;
}

void PeerHub::SendHello(Peer& peer) {
    if (peer.hello_attempts > 0) {
        ++m_stats.hello_retransmits;
    }
    ++peer.hello_attempts;
    std::uint8_t payload[kHelloPayloadBytes];
    MakeHelloPayload(peer.session_id, peer.conn_id, payload);
    FrameHeader header;
    header.type = FrameType::kHello;
    header.channel = 0;
    header.conn_id = peer.conn_id;
    header.seq = peer.session_id;
    (void)SendControl(peer, header, payload, sizeof(payload));
    const std::uint64_t now = NowMs();
    peer.next_hello_ms = now + peer.hello_pto_ms;
    peer.hello_pto_ms = std::min(peer.hello_pto_ms * 2, m_config.hello_max_pto_ms);
}

void PeerHub::StartHandshake(Peer& peer) {
    const std::uint64_t now = NowMs();
    peer.state = PeerState::kConnecting;
    peer.handshake_start_ms = now;
    peer.next_hello_ms = now;
    peer.hello_attempts = 0;
    peer.hello_pto_ms = m_config.hello_initial_pto_ms;
    peer.session_id = m_session_id;
    if (peer.conn_id == 0) {
        peer.conn_id = RandomNonZeroU32();
    }
    m_by_conn_id[peer.conn_id] = peer.peer_id;
    SendHello(peer);
}

bool PeerHub::AddPeer(std::uint32_t peer_id, const std::string& address, std::uint16_t port,
                      std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_socket.IsOpen()) {
        if (error != nullptr) {
            *error = "hub is not open";
        }
        return false;
    }
    if (peer_id == 0) {
        if (error != nullptr) {
            *error = "peer_id must be non-zero";
        }
        return false;
    }
    if (m_peers.find(peer_id) != m_peers.end()) {
        if (error != nullptr) {
            *error = "peer_id already registered";
        }
        return false;
    }
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    if (!UdpSocket::Resolve(address, port, &addr, &addr_len, error)) {
        return false;
    }
    // 同一地址只能对应一个 peer：否则回包无法按 conn_id 唯一区分。
    if (FindByAddress(addr, addr_len) != nullptr) {
        if (error != nullptr) {
            *error = "peer address already registered";
        }
        return false;
    }
    Peer* peer = CreatePeer(peer_id, addr, addr_len);
    peer->outgoing = true;
    // 随机 conn_id；极小概率与既有 peer 冲突，冲突时重抽。
    std::uint32_t conn_id = RandomNonZeroU32();
    for (int attempt = 0; attempt < 8 && m_by_conn_id.find(conn_id) != m_by_conn_id.end();
         ++attempt) {
        conn_id = RandomNonZeroU32();
    }
    if (m_by_conn_id.find(conn_id) != m_by_conn_id.end()) {
        m_peers.erase(peer_id);
        if (error != nullptr) {
            *error = "could not allocate a unique conn_id";
        }
        return false;
    }
    peer->conn_id = conn_id;
    StartHandshake(*peer);
    return true;
}

bool PeerHub::Connect(std::uint32_t peer_id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    if (found == m_peers.end() || !found->second->outgoing) {
        return false;
    }
    Peer& peer = *found->second;
    if (peer.state == PeerState::kConnecting || peer.state == PeerState::kConnected) {
        return false;
    }
    ResetChannels(peer);
    // 新会话必须换 conn_id：旧的接收窗口/对端缓存仍指向上一代会话。
    if (peer.conn_id != 0) {
        m_by_conn_id.erase(peer.conn_id);
    }
    peer.conn_id = RandomNonZeroU32();
    for (int attempt = 0; attempt < 8 && m_by_conn_id.find(peer.conn_id) != m_by_conn_id.end();
         ++attempt) {
        peer.conn_id = RandomNonZeroU32();
    }
    StartHandshake(peer);
    return true;
}

bool PeerHub::RemovePeer(std::uint32_t peer_id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    if (found == m_peers.end()) {
        return false;
    }
    const std::uint32_t conn_id = found->second->conn_id;
    DropPeer(*found->second, "removed");
    m_by_conn_id.erase(conn_id);
    m_peers.erase(found);
    return true;
}

bool PeerHub::HasPeer(std::uint32_t peer_id) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_peers.find(peer_id) != m_peers.end();
}

PeerState PeerHub::peer_state(std::uint32_t peer_id) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    return found == m_peers.end() ? PeerState::kClosed : found->second->state;
}

bool PeerHub::peer_connected(std::uint32_t peer_id) const {
    return peer_state(peer_id) == PeerState::kConnected;
}

std::vector<PeerInfo> PeerHub::Peers() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<PeerInfo> result;
    result.reserve(m_peers.size());
    for (const auto& entry : m_peers) {
        const Peer& peer = *entry.second;
        PeerInfo info;
        info.peer_id = peer.peer_id;
        info.state = peer.state;
        info.address = FormatAddress(peer.addr);
        info.port = PortOf(peer.addr);
        info.conn_id = peer.conn_id;
        info.outgoing = peer.outgoing;
        info.hello_attempts = peer.hello_attempts;
        info.frames_received = peer.frames_received;
        info.frames_sent = peer.frames_sent;
        info.bytes_received = peer.bytes_received;
        info.bytes_sent = peer.bytes_sent;
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<std::uint32_t> PeerHub::PeerIds() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<std::uint32_t> ids;
    ids.reserve(m_peers.size());
    for (const auto& entry : m_peers) {
        ids.push_back(entry.first);
    }
    return ids;
}

PeerHubStats PeerHub::stats() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    PeerHubStats snapshot = m_stats;
    snapshot.peers_active = 0;
    for (const auto& entry : m_peers) {
        if (entry.second->state == PeerState::kConnected) {
            ++snapshot.peers_active;
        }
    }
    return snapshot;
}

ReliableChannel::Stats PeerHub::reliable_stats(std::uint32_t peer_id,
                                               std::uint8_t channel) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    if (found == m_peers.end()) {
        return ReliableChannel::Stats{};
    }
    const auto channel_it = found->second->reliable.find(channel);
    if (channel_it == found->second->reliable.end()) {
        return ReliableChannel::Stats{};
    }
    return channel_it->second->stats();
}

UnreliableChannel::Stats PeerHub::unreliable_stats(std::uint32_t peer_id,
                                                   std::uint8_t channel) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    if (found == m_peers.end()) {
        return UnreliableChannel::Stats{};
    }
    const auto channel_it = found->second->unreliable.find(channel);
    if (channel_it == found->second->unreliable.end()) {
        return UnreliableChannel::Stats{};
    }
    return channel_it->second->stats();
}

bool PeerHub::SendStream(std::uint32_t peer_id, std::uint8_t channel, const void* data,
                         std::size_t bytes, std::uint32_t dep_seq) {
    if (data == nullptr || bytes == 0) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    if (found == m_peers.end() || found->second->state != PeerState::kConnected) {
        return false;
    }
    Peer& peer = *found->second;
    ReliableChannel* reliable = GetOrCreateReliable(peer, channel);
    if (reliable == nullptr) {
        return false;
    }
    return reliable->Send(static_cast<const std::uint8_t*>(data), bytes, dep_seq);
}

SendStatus PeerHub::TrySendStream(std::uint32_t peer_id, std::uint8_t channel, const void* data,
                                  std::size_t bytes, std::uint32_t dep_seq) {
    if (data == nullptr || bytes == 0) {
        return SendStatus::kError;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    if (found == m_peers.end() || found->second->state != PeerState::kConnected) {
        return SendStatus::kError;
    }
    Peer& peer = *found->second;
    ReliableChannel* reliable = GetOrCreateReliable(peer, channel);
    if (reliable == nullptr) {
        return SendStatus::kError;
    }
    return reliable->TrySend(static_cast<const std::uint8_t*>(data), bytes, dep_seq);
}

bool PeerHub::SendDatagram(std::uint32_t peer_id, std::uint8_t channel, const void* data,
                           std::size_t bytes, std::uint32_t dep_seq) {
    if (data == nullptr || bytes == 0) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    if (found == m_peers.end() || found->second->state != PeerState::kConnected) {
        return false;
    }
    Peer& peer = *found->second;
    UnreliableChannel* unreliable = GetOrCreateUnreliable(peer, channel);
    if (unreliable == nullptr) {
        return false;
    }
    return unreliable->Send(static_cast<const std::uint8_t*>(data), bytes, dep_seq);
}

bool PeerHub::SendBarrier(std::uint32_t peer_id, std::uint32_t barrier_seq) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    if (found == m_peers.end() || found->second->state != PeerState::kConnected) {
        return false;
    }
    FrameHeader header;
    header.type = FrameType::kBarrier;
    header.channel = 0;
    header.seq = barrier_seq;
    return SendControl(*found->second, header, nullptr, 0);
}

bool PeerHub::SendAckFrame(Peer& peer, std::uint8_t channel, bool heartbeat) {
    AckState ack;
    ReliableChannel* reliable = nullptr;
    const auto found = peer.reliable.find(channel);
    if (found != peer.reliable.end()) {
        reliable = found->second.get();
        reliable->FillAck(&ack);
    } else {
        ack.ack_seq = 0;
        ack.ack_bits = 0;
    }
    std::uint8_t payload[kAckBytes];
    EncodeAck(ack, payload);
    FrameHeader header;
    header.type = FrameType::kAck;
    header.channel = channel;
    header.seq = 0;
    if (!SendControl(peer, header, payload, kAckBytes)) {
        return false;
    }
    ++m_stats.acks_sent;
    if (heartbeat) {
        ++m_stats.heartbeats_sent;
    }
    if (reliable != nullptr) {
        reliable->MarkAckSent();
    }
    return true;
}

bool PeerHub::SendMaxDataFrame(Peer& peer, std::uint8_t channel, std::uint64_t limit) {
    std::uint8_t payload[kMaxDataPayloadBytes];
    WriteU64(payload, limit);
    FrameHeader header;
    header.type = FrameType::kMaxData;
    header.channel = channel;
    header.seq = 0;
    return SendControl(peer, header, payload, sizeof(payload));
}

void PeerHub::HandleHello(const FrameView& view, const sockaddr_storage& from,
                          socklen_t from_len) {
    if (view.payload_bytes < kHelloPayloadBytes || !IsHelloMagic(view.payload)) {
        ++m_stats.invalid_frames;
        return;
    }
    const std::uint32_t session = ReadU32(view.payload + 4);
    const std::uint32_t proposed_conn_id = ReadU32(view.payload + 8);
    if (session == 0) {
        ++m_stats.invalid_frames;
        return;
    }
    Peer* peer = FindByAddress(from, from_len);
    if (peer == nullptr) {
        // 未知来源：只有显式允许时才接受（白名单模式直接丢弃）。
        if (!m_config.accept_unknown_peers) {
            ++m_stats.unknown_peer_frames;
            return;
        }
        if (m_peers.size() >= m_config.max_peers) {
            ++m_stats.peers_rejected;
            return;
        }
        peer = CreatePeer(AllocatePeerId(), from, from_len);
        peer->outgoing = false;
        peer->session_id = session;
        peer->conn_id = 0;
    } else if (peer->session_id != session) {
        // 同一地址的新会话（对端重启）：旧通道状态必须整体作废，
        // 否则接收端会停在旧 seq 上永远等不到数据。
        if (peer->conn_id != 0) {
            m_by_conn_id.erase(peer->conn_id);
        }
        ResetChannels(*peer);
        peer->session_id = session;
        peer->conn_id = 0;
    }

    if (peer->conn_id == 0) {
        // 采纳客户端提议的 conn_id；冲突时改用本端随机的（客户端以 ACK 为准）。
        std::uint32_t chosen = proposed_conn_id;
        if (chosen == 0 || m_by_conn_id.find(chosen) != m_by_conn_id.end()) {
            if (chosen != 0) {
                ++m_stats.conn_id_conflicts;
            }
            chosen = RandomNonZeroU32();
            for (int attempt = 0;
                 attempt < 8 && m_by_conn_id.find(chosen) != m_by_conn_id.end(); ++attempt) {
                chosen = RandomNonZeroU32();
            }
        }
        peer->conn_id = chosen;
        m_by_conn_id[chosen] = peer->peer_id;
    }

    // 重复 HELLO（ACK 丢失）要幂等回 ACK。
    std::uint8_t payload[kHelloPayloadBytes];
    MakeHelloPayload(peer->session_id, peer->conn_id, payload);
    FrameHeader header;
    header.type = FrameType::kHelloAck;
    header.channel = 0;
    header.conn_id = peer->conn_id;
    header.seq = peer->session_id;
    (void)SendControl(*peer, header, payload, sizeof(payload));
    peer->last_rx_ms = NowMs();
    if (peer->state != PeerState::kConnected) {
        MarkConnected(*peer);
    }
}

void PeerHub::HandleHelloAck(const FrameView& view, const sockaddr_storage& from) {
    if (view.payload_bytes < kHelloPayloadBytes || !IsHelloMagic(view.payload)) {
        ++m_stats.invalid_frames;
        return;
    }
    const std::uint32_t session = ReadU32(view.payload + 4);
    const std::uint32_t accepted_conn_id = ReadU32(view.payload + 8);
    const auto found = m_by_conn_id.find(accepted_conn_id);
    if (accepted_conn_id == 0 || found == m_by_conn_id.end()) {
        ++m_stats.unknown_peer_frames;
        return;
    }
    const auto peer_it = m_peers.find(found->second);
    if (peer_it == m_peers.end()) {
        ++m_stats.unknown_peer_frames;
        return;
    }
    Peer& peer = *peer_it->second;
    if (!peer.outgoing || peer.session_id != session || view.header.seq != session) {
        // 允许地址变化（NAT 重绑定），但 session 必须对得上，否则是注入/串台。
        ++m_stats.invalid_frames;
        return;
    }
    if (accepted_conn_id != peer.conn_id) {
        m_by_conn_id.erase(peer.conn_id);
        m_by_conn_id[accepted_conn_id] = peer.peer_id;
        peer.conn_id = accepted_conn_id;
    }
    if (!UdpSocket::SamePeer(from, peer.addr)) {
        peer.addr = from;
        peer.addr_len = sizeof(sockaddr_storage);
    }
    peer.last_rx_ms = NowMs();
    if (peer.state != PeerState::kConnected) {
        MarkConnected(peer);
    }
}

void PeerHub::MarkConnected(Peer& peer) {
    peer.state = PeerState::kConnected;
    const std::uint64_t now = NowMs();
    peer.last_rx_ms = now;
    peer.next_ack_ms = now + m_config.ack_interval_ms;
    peer.next_heartbeat_ms = now + m_config.heartbeat_interval_ms;
    ++m_stats.handshakes;
    if (on_peer_connected) {
        on_peer_connected(peer.peer_id);
    }
}

void PeerHub::DropPeer(Peer& peer, const char* reason) {
    if (peer.state == PeerState::kTimedOut || peer.state == PeerState::kClosed) {
        return;
    }
    peer.state = PeerState::kTimedOut;
    ++m_stats.timeouts;
    ResetChannels(peer);
    if (on_peer_disconnected) {
        on_peer_disconnected(peer.peer_id, reason);
    }
}

void PeerHub::DispatchFrame(Peer& peer, const FrameView& view) {
    switch (view.header.type) {
        case FrameType::kStream: {
            ReliableChannel* channel = GetOrCreateReliable(peer, view.header.channel);
            if (channel == nullptr) {
                ++m_stats.unknown_peer_frames;
                return;
            }
            channel->OnFrame(view.header, view.payload, view.payload_bytes,
                             view.has_piggyback_ack ? &view.ack : nullptr);
            break;
        }
        case FrameType::kDatagram: {
            UnreliableChannel* channel = GetOrCreateUnreliable(peer, view.header.channel);
            if (channel == nullptr) {
                ++m_stats.unknown_peer_frames;
                return;
            }
            channel->OnFrame(view.header, view.payload, view.payload_bytes);
            break;
        }
        case FrameType::kAck: {
            const auto found = peer.reliable.find(view.header.channel);
            if (found != peer.reliable.end()) {
                found->second->OnAck(view.ack);
            }
            break;
        }
        case FrameType::kBarrier: {
            for (auto& entry : peer.unreliable) {
                entry.second->OnBarrier(view.header.seq);
            }
            peer.relation.OnBarrier(view.header.seq);
            break;
        }
        case FrameType::kMaxData: {
            if (view.payload_bytes != kMaxDataPayloadBytes) {
                ++m_stats.invalid_frames;
                break;
            }
            const auto found = peer.reliable.find(view.header.channel);
            if (found != peer.reliable.end()) {
                found->second->OnMaxData(ReadU64(view.payload));
            }
            break;
        }
        case FrameType::kHello:
        case FrameType::kHelloAck:
            // 握手帧在 HandlePacket 中处理，不会到这里。
            break;
        case FrameType::kVideo:
        case FrameType::kPmtudProbe:
        case FrameType::kPmtudAck:
            // PeerHub 的 P0 范围只覆盖 STREAM/DATAGRAM/ACK/BARRIER/MAX_DATA；
            // 视频分片与路径 MTU 探测在 Link 上实现，见场景矩阵的缺口说明。
            ++m_stats.unknown_peer_frames;
            break;
    }
}

void PeerHub::HandlePacket(const std::uint8_t* data, std::size_t bytes,
                           const sockaddr_storage& from, socklen_t from_len) {
    FrameHeader header;
    if (!DecodeHeader(data, bytes, &header)) {
        ++m_stats.invalid_frames;
        return;
    }
    if (header.type == FrameType::kHello || header.type == FrameType::kHelloAck) {
        FrameView view;
        if (!ParseFrame(data, bytes, &view)) {
            ++m_stats.invalid_frames;
            return;
        }
        if (view.header.type == FrameType::kHello) {
            HandleHello(view, from, from_len);
        } else {
            HandleHelloAck(view, from);
        }
        return;
    }
    Peer* peer = FindByConnId(header.conn_id);
    if (peer == nullptr || peer->state != PeerState::kConnected) {
        ++m_stats.unknown_peer_frames;
        return;
    }
    FrameView view;
    if (!ParseFrame(data, bytes, &view)) {
        ++m_stats.invalid_frames;
        return;
    }
    // conn_id 已确认身份，源地址变化视为 NAT 重绑定，更新发送目的地址。
    if (!UdpSocket::SamePeer(from, peer->addr)) {
        peer->addr = from;
        peer->addr_len = from_len;
    }
    peer->last_rx_ms = NowMs();
    ++peer->frames_received;
    peer->bytes_received += static_cast<std::uint64_t>(bytes);
    DispatchFrame(*peer, view);
}

void PeerHub::Tick(std::uint64_t now_ms) {
    for (auto& entry : m_peers) {
        Peer& peer = *entry.second;
        if (peer.state == PeerState::kConnecting) {
            if (now_ms >= peer.next_hello_ms) {
                if (peer.hello_attempts >= m_config.hello_max_attempts ||
                    now_ms - peer.handshake_start_ms >= m_config.handshake_timeout_ms) {
                    DropPeer(peer, "hello timeout");
                    continue;
                }
                SendHello(peer);
            }
            continue;
        }
        if (peer.state != PeerState::kConnected) {
            continue;
        }
        if (now_ms >= peer.next_ack_ms) {
            peer.next_ack_ms = now_ms + m_config.ack_interval_ms;
            for (auto& channel : peer.reliable) {
                if (channel.second->HasPendingAck()) {
                    (void)SendAckFrame(peer, channel.first, false);
                }
            }
        }
        if (now_ms >= peer.next_heartbeat_ms) {
            peer.next_heartbeat_ms = now_ms + m_config.heartbeat_interval_ms;
            (void)SendAckFrame(peer, kChannelMove, true);
        }
        for (auto& channel : peer.reliable) {
            channel.second->OnTimer(now_ms);
            std::uint64_t limit = 0;
            if (channel.second->PendingMaxData(&limit, now_ms) &&
                SendMaxDataFrame(peer, channel.first, limit)) {
                channel.second->OnMaxDataSent(limit, now_ms);
            }
        }
        if (now_ms >= peer.last_rx_ms && now_ms - peer.last_rx_ms > m_config.peer_timeout_ms) {
            DropPeer(peer, "peer timeout");
        }
    }
}

int PeerHub::RunOnce(int timeout_ms) {
    // 与 Link 相同的线程模型：持锁覆盖 poll + 分发 + Tick，避免与并发 Send 竞争。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_socket.IsOpen()) {
        return 0;
    }
    int processed = 0;
    const int ready = m_socket.WaitReadable(timeout_ms);
    if (ready > 0) {
        while (processed < kMaxFramesPerRun) {
            sockaddr_storage from{};
            socklen_t from_len = sizeof(from);
            const int received =
                m_socket.RecvFrom(m_rx_buffer.data(), m_rx_buffer.size(), &from, &from_len);
            if (received == 0) {
                break;
            }
            if (received < 0) {
                ++m_stats.recv_errors;
                break;
            }
            ++m_stats.frames_received;
            m_stats.bytes_received += static_cast<std::uint64_t>(received);
            HandlePacket(m_rx_buffer.data(), static_cast<std::size_t>(received), from, from_len);
            ++processed;
        }
    } else if (ready < 0) {
        ++m_stats.poll_errors;
    }
    Tick(NowMs());
    return processed;
}

int PeerHub::RunFor(int duration_ms) {
    const int clamped = std::max(duration_ms, 0);
    const std::uint64_t start = NowMs();
    int total = 0;
    for (;;) {
        const std::uint64_t elapsed = NowMs() - start;
        if (elapsed >= static_cast<std::uint64_t>(clamped)) {
            break;
        }
        const int remaining = clamped - static_cast<int>(elapsed);
        total += RunOnce(std::min(remaining, 5));
    }
    return total;
}

}  // namespace go2cpp::bento
