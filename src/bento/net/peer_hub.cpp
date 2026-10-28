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
#include <chrono>
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

/// 墙钟毫秒：只用于跨进程可比的握手时间戳，不参与超时计算。
std::uint64_t WallClockMs() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
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

/// @brief 标记「正在派发用户回调」；嵌套层数 > 0 时禁止销毁 Peer/通道。
class CallbackDepthGuard final {
   public:
    explicit CallbackDepthGuard(int& depth) noexcept : m_depth(depth) {
        ++m_depth;
    }
    ~CallbackDepthGuard() {
        --m_depth;
    }
    CallbackDepthGuard(const CallbackDepthGuard&) = delete;
    CallbackDepthGuard& operator=(const CallbackDepthGuard&) = delete;

   private:
    int& m_depth;
};

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

    // ---- PSK 认证加密（P1）：每 peer 独立 nonce/密钥/wire seq/重放窗口 ----
    bool secure_enabled = false;      ///< 该 peer 是否走加密握手。
    bool session_keys_ready = false;  ///< 会话密钥是否就绪。
    std::uint8_t psk[kPskBytes] = {};
    std::uint8_t client_nonce[kHandshakeNonceBytes] = {};
    std::uint8_t server_nonce[kHandshakeNonceBytes] = {};
    std::uint8_t tx_key[kChaCha20KeyBytes] = {};
    std::uint8_t rx_key[kChaCha20KeyBytes] = {};
    SecureDirection tx_direction = SecureDirection::kClientToServer;
    SecureDirection rx_direction = SecureDirection::kServerToClient;
    std::uint32_t tx_wire_seq = 0;       ///< 本方向已用的最大 wire seq。
    std::uint32_t handshake_tx_seq = 1;  ///< 握手帧 wire seq（每会话随机）。
    ReplayWindow data_rx_window;
    ReplayWindow handshake_rx_window;
    std::vector<std::uint8_t> cached_hello;      ///< 加密 HELLO（重传幂等）。
    std::vector<std::uint8_t> cached_hello_ack;  ///< 加密 HELLO_ACK（重传幂等）。

    // ---- 审查修复：延迟销毁与 HELLO 重放防护 ----
    /// 已进入延迟销毁队列；Connect 重连会清除（仅超时软回收可取消）。
    bool pending_cleanup = false;
    /// 已接受过的 client nonce（跨会话保留，拒绝旧 HELLO 重放）。
    std::vector<std::array<std::uint8_t, kHandshakeNonceBytes>> seen_client_nonces;
    /// 最近一次接受的新 HELLO 的墙钟时间戳（单调性检查）。
    std::uint64_t last_hello_timestamp_ms = 0;
    /// 每个 peer 保留的已见 nonce 数量（与 Link 的 kMaxRememberedClientNonces 一致）。
    static constexpr std::size_t kMaxRememberedClientNonces = 8;

    // ---- 日志钩子增量（仅注入 hook 时维护） ----
    std::map<std::uint8_t, std::uint64_t> log_retransmits;
    std::map<std::uint8_t, std::uint64_t> log_loss_events;
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
    m_log_hook = m_config.log_hook;
}

PeerHub::~PeerHub() {
    Close();
}

bool PeerHub::Open(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string local_error;
    if (!m_config.pre_shared_key.empty()) {
        if (!SetPreSharedKey(m_config.pre_shared_key.data(),
                             m_config.pre_shared_key.size())) {
            if (error != nullptr) {
                *error = "pre_shared_key must be exactly 32 bytes";
            }
            return false;
        }
    }
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
    m_socket.Close();
    m_by_conn_id.clear();
    if (m_callback_depth > 0) {
        // PEB-01：回调里 Close 时不能销毁正在执行的通道（on_stream 返回后
        // AdvanceDelivery 仍会写 m_next_rx_seq）。先标记终态，整表清空
        // 推迟到 RunOnce 末尾的安全点。
        for (auto& entry : m_peers) {
            entry.second->state = PeerState::kClosed;
            entry.second->session_keys_ready = false;
        }
        m_pending_cleanup.clear();
        m_close_deferred = true;
        return;
    }
    m_close_deferred = false;
    m_pending_cleanup.clear();
    m_peers.clear();
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
    if (m_secure_enabled) {
        peer->secure_enabled = true;
        std::memcpy(peer->psk, m_psk, kPskBytes);
    }
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

void PeerHub::ResetSecureSession(Peer& peer) {
    peer.session_keys_ready = false;
    std::memset(peer.client_nonce, 0, sizeof(peer.client_nonce));
    std::memset(peer.server_nonce, 0, sizeof(peer.server_nonce));
    std::memset(peer.tx_key, 0, sizeof(peer.tx_key));
    std::memset(peer.rx_key, 0, sizeof(peer.rx_key));
    peer.tx_wire_seq = 0;
    peer.handshake_tx_seq = 1;
    peer.data_rx_window.Reset();
    peer.handshake_rx_window.Reset();
    peer.cached_hello.clear();
    peer.cached_hello_ack.clear();
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
                // PEB-01：回调里允许 RemovePeer/Close，销毁延迟到安全点。
                CallbackDepthGuard guard(m_callback_depth);
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
                CallbackDepthGuard guard(m_callback_depth);
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
    return SendFrameBytes(peer, data, bytes);
}

bool PeerHub::SendFrameBytes(Peer& peer, const std::uint8_t* data, std::size_t bytes) {
    if (peer.session_keys_ready) {
        return SealAndSend(peer, data, bytes);
    }
    if (peer.secure_enabled) {
        // 加密 peer 在握手完成前不能发明文数据：对端会按明文拒绝，
        // 静默发送只会让可靠层误以为已入网。
        ++m_stats.send_errors;
        EmitLog(LogLevel::kError, LogEvent::kSendError, peer.peer_id,
                "secure peer not ready", bytes);
        return false;
    }
    return SendRawPacket(peer, data, bytes);
}

bool PeerHub::SendRawPacket(Peer& peer, const std::uint8_t* data, std::size_t bytes) {
    if (!m_socket.IsOpen()) {
        ++m_stats.send_errors;
        return false;
    }
    if (!m_socket.SendTo(peer.addr, peer.addr_len, data, bytes)) {
        ++m_stats.send_errors;
        EmitLog(LogLevel::kError, LogEvent::kSendError, peer.peer_id, "sendto failed", bytes);
        return false;
    }
    ++m_stats.frames_sent;
    m_stats.bytes_sent += static_cast<std::uint64_t>(bytes);
    ++peer.frames_sent;
    peer.bytes_sent += static_cast<std::uint64_t>(bytes);
    peer.last_tx_ms = NowMs();
    return true;
}

bool PeerHub::SealAndSend(Peer& peer, const std::uint8_t* frame, std::size_t bytes) {
    if (!peer.session_keys_ready) {
        ++m_stats.send_errors;
        return false;
    }
    std::uint32_t seq = peer.tx_wire_seq + 1;
    if (seq == 0) {
        seq = 1;  // wire seq 0 保留；2^32 回绕在实际连接中不会发生。
    }
    std::vector<std::uint8_t> sealed;
    if (!SealFrame(peer.tx_key, peer.tx_direction, seq, frame, bytes, &sealed)) {
        ++m_stats.send_errors;
        return false;
    }
    // 先推进 seq 再发送：发送失败也绝不复用同一 (key, nonce)。
    peer.tx_wire_seq = seq;
    if (!SendRawPacket(peer, sealed.data(), sealed.size())) {
        return false;
    }
    ++m_secure_stats.packets_sealed;
    m_secure_stats.encrypted_bytes_sent += static_cast<std::uint64_t>(sealed.size());
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
    return SendFrameBytes(peer, frame.data(), frame.size());
}

void PeerHub::SendHello(Peer& peer) {
    if (peer.hello_attempts > 0) {
        ++m_stats.hello_retransmits;
        EmitLog(LogLevel::kDebug, LogEvent::kHandshakeRetry, peer.peer_id,
                peer.secure_enabled ? "secure hello retransmit" : "hello retransmit",
                peer.hello_attempts);
    }
    ++peer.hello_attempts;
    if (peer.secure_enabled) {
        if (peer.cached_hello.empty()) {
            HelloBody body;
            body.session = peer.session_id;
            std::memcpy(body.nonce, peer.client_nonce, kHandshakeNonceBytes);
            body.timestamp_ms = WallClockMs();
            std::uint8_t raw[kHelloBodyBytes];
            if (!EncodeHelloBody(body, raw, sizeof(raw)) ||
                !SealHandshakeFrame(peer.psk, SecureDirection::kClientToServer,
                                    static_cast<std::uint8_t>(FrameType::kHello),
                                    peer.session_id, peer.conn_id, peer.handshake_tx_seq, raw,
                                    sizeof(raw), &peer.cached_hello)) {
                ++m_stats.send_errors;
                EmitLog(LogLevel::kError, LogEvent::kSendError, peer.peer_id,
                        "seal secure hello failed", 0);
                return;
            }
        }
        (void)SendRawPacket(peer, peer.cached_hello.data(), peer.cached_hello.size());
    } else {
        std::uint8_t payload[kHelloPayloadBytes];
        MakeHelloPayload(peer.session_id, peer.conn_id, payload);
        FrameHeader header;
        header.type = FrameType::kHello;
        header.channel = 0;
        header.conn_id = peer.conn_id;
        header.seq = peer.session_id;
        (void)SendControl(peer, header, payload, sizeof(payload));
    }
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
    // PEB-02：每一代连接用独立 session_id。对端据此识别「新会话」并重置
    // 通道/重放窗口；复用 Hub 级 session 会让重连后的新流被误判为旧包。
    peer.session_id = NextSessionId();
    if (peer.conn_id == 0) {
        peer.conn_id = RandomNonZeroU32();
    }
    m_by_conn_id[peer.conn_id] = peer.peer_id;
    ResetSecureSession(peer);
    if (peer.secure_enabled) {
        // 每次连接（含重连）都用新 client nonce：旧 HELLO 的重放不会
        // 被当作当前会话，AEAD nonce 也不会跨会话复用。
        SecureRandomBytes(peer.client_nonce, kHandshakeNonceBytes);
        peer.handshake_tx_seq = RandomNonZeroU32();
    }
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
    const std::uint32_t conn_id = AllocateConnId();
    if (conn_id == 0) {
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
    const auto pending = m_pending_cleanup.find(peer_id);
    if (pending != m_pending_cleanup.end() && pending->second) {
        // RemovePeer 已登记硬删除：不允许在销毁前复活。
        return false;
    }
    // PEB-04：取消超时软回收，重连复用该 peer 记录。
    peer.pending_cleanup = false;
    m_pending_cleanup.erase(peer_id);
    ResetChannels(peer);
    ResetSecureSession(peer);
    // PEB-02：新会话必须换 conn_id：旧的接收窗口/对端缓存仍指向上一代会话。
    if (peer.conn_id != 0) {
        m_by_conn_id.erase(peer.conn_id);
    }
    const std::uint32_t conn_id = AllocateConnId();
    if (conn_id == 0) {
        return false;
    }
    peer.conn_id = conn_id;
    StartHandshake(peer);
    return true;
}

bool PeerHub::RemovePeer(std::uint32_t peer_id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    if (found == m_peers.end()) {
        return false;
    }
    Peer& peer = *found->second;
    const auto pending = m_pending_cleanup.find(peer_id);
    if (peer.state == PeerState::kClosed && pending != m_pending_cleanup.end() &&
        pending->second) {
        // 已在待销毁队列：回调里重复 RemovePeer 是幂等的，不能二次 erase。
        return false;
    }
    if (peer.state != PeerState::kTimedOut && peer.state != PeerState::kClosed) {
        peer.state = PeerState::kClosed;
        ++m_stats.timeouts;
        if (m_callback_depth == 0) {
            ResetChannels(peer);
            ResetSecureSession(peer);
        } else {
            // 回调栈上可能正在执行该 peer 的通道，销毁推迟到安全点。
            peer.session_keys_ready = false;
        }
        EmitLog(LogLevel::kWarn, LogEvent::kPeerDisconnected, peer_id, "removed", 0);
        if (on_peer_disconnected) {
            CallbackDepthGuard guard(m_callback_depth);
            on_peer_disconnected(peer_id, "removed");
        }
    } else {
        peer.state = PeerState::kClosed;
        peer.session_keys_ready = false;
    }
    if (peer.conn_id != 0) {
        m_by_conn_id.erase(peer.conn_id);
    }
    ScheduleCleanup(peer, true);
    if (m_callback_depth == 0) {
        FlushDeferredCleanup();
    }
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
        info.secure = peer.secure_enabled;
        info.secure_ready = peer.session_keys_ready;
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

bool PeerHub::SetPreSharedKey(const std::uint8_t* key, std::size_t bytes) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (key == nullptr || bytes != kPskBytes) {
        return false;
    }
    std::memcpy(m_psk, key, kPskBytes);
    m_secure_enabled = true;
    for (auto& entry : m_peers) {
        Peer& peer = *entry.second;
        if (peer.state == PeerState::kConnected) {
            // 已建立的会话（明文或加密）不能热换模式：热换会让在途帧
            // 无法解密、或被对端按明文拒绝。需要 RemovePeer/Connect 重建。
            continue;
        }
        std::memcpy(peer.psk, m_psk, kPskBytes);
        peer.secure_enabled = true;
        ResetSecureSession(peer);
        if (peer.outgoing && peer.state != PeerState::kConnected) {
            StartHandshake(peer);
        }
    }
    return true;
}

bool PeerHub::SetPeerPreSharedKey(std::uint32_t peer_id, const std::uint8_t* key,
                                  std::size_t bytes) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (key == nullptr || bytes != kPskBytes) {
        return false;
    }
    const auto found = m_peers.find(peer_id);
    if (found == m_peers.end()) {
        return false;
    }
    Peer& peer = *found->second;
    if (peer.session_keys_ready || peer.state == PeerState::kConnected) {
        return false;
    }
    std::memcpy(peer.psk, key, kPskBytes);
    peer.secure_enabled = true;
    ResetSecureSession(peer);
    if (peer.outgoing) {
        StartHandshake(peer);
    }
    return true;
}

bool PeerHub::peer_secure(std::uint32_t peer_id) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    return found != m_peers.end() && found->second->secure_enabled;
}

bool PeerHub::peer_secure_ready(std::uint32_t peer_id) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto found = m_peers.find(peer_id);
    return found != m_peers.end() && found->second->session_keys_ready;
}

PeerSecureStats PeerHub::secure_stats() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_secure_stats;
}

void PeerHub::SetLogHook(LogHook* hook) noexcept {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_log_hook = hook;
}

LogHook* PeerHub::log_hook() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_log_hook;
}

void PeerHub::EmitLog(LogLevel level, LogEvent event, std::uint32_t peer_id,
                      const char* message, std::uint64_t value) {
    if (m_log_hook != nullptr) {
        m_log_hook->Emit(level, event, peer_id, message, value);
    }
}

bool PeerHub::TimestampAcceptable(std::uint64_t timestamp_ms) const noexcept {
    if (m_config.handshake_replay_window_ms == 0) {
        return true;
    }
    const std::uint64_t now = WallClockMs();
    const std::uint64_t delta = now >= timestamp_ms ? now - timestamp_ms : timestamp_ms - now;
    return delta <= static_cast<std::uint64_t>(m_config.handshake_replay_window_ms);
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
            // PEB-04：先回收已超时的被动 peer，再判断容量。
            FlushDeferredCleanup();
        }
        if (m_peers.size() >= m_config.max_peers) {
            ++m_stats.peers_rejected;
            EmitLog(LogLevel::kWarn, LogEvent::kHandshakeFailed, 0,
                    "plaintext hello rejected: max peers", 0);
            return;
        }
        peer = CreatePeer(AllocatePeerId(), from, from_len);
        peer->outgoing = false;
        peer->session_id = session;
        peer->conn_id = 0;
    } else if (peer->session_id != session) {
        // 同一地址的新会话（对端重启/重连）：旧通道状态必须整体作废，
        // 否则接收端会停在旧 seq 上永远等不到数据。
        if (peer->conn_id != 0) {
            m_by_conn_id.erase(peer->conn_id);
        }
        ResetChannels(*peer);
        peer->session_id = session;
        peer->conn_id = 0;
    } else if (peer->conn_id != 0 && proposed_conn_id != 0 &&
               proposed_conn_id != peer->conn_id) {
        // 同一 session 但客户端换了 conn_id（兼容未换 session 的旧客户端）：
        // 接收侧通道状态必须重置，否则新流 seq=1 会被判 stale。
        m_by_conn_id.erase(peer->conn_id);
        ResetChannels(*peer);
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
    if (accepted_conn_id == 0) {
        ++m_stats.unknown_peer_frames;
        return;
    }
    Peer* peer = nullptr;
    const auto found = m_by_conn_id.find(accepted_conn_id);
    if (found != m_by_conn_id.end()) {
        const auto peer_it = m_peers.find(found->second);
        if (peer_it != m_peers.end()) {
            peer = peer_it->second.get();
        }
    }
    if (peer == nullptr) {
        // PEB-02：监听端可能因 conn_id 冲突改过 id；明文 ACK 也按地址回退，
        // 再用 session 校验兜底（与加密路径同等强度）。
        peer = FindByAddress(from, sizeof(sockaddr_storage));
    }
    if (peer == nullptr) {
        ++m_stats.unknown_peer_frames;
        return;
    }
    if (!peer->outgoing || peer->session_id != session || view.header.seq != session) {
        // 允许地址变化（NAT 重绑定），但 session 必须对得上，否则是注入/串台。
        ++m_stats.invalid_frames;
        return;
    }
    if (accepted_conn_id != peer->conn_id) {
        m_by_conn_id.erase(peer->conn_id);
        m_by_conn_id[accepted_conn_id] = peer->peer_id;
        peer->conn_id = accepted_conn_id;
    }
    if (!UdpSocket::SamePeer(from, peer->addr)) {
        peer->addr = from;
        peer->addr_len = sizeof(sockaddr_storage);
    }
    peer->last_rx_ms = NowMs();
    if (peer->state != PeerState::kConnected) {
        MarkConnected(*peer);
    }
}

void PeerHub::MarkConnected(Peer& peer) {
    peer.state = PeerState::kConnected;
    const std::uint64_t now = NowMs();
    peer.last_rx_ms = now;
    peer.next_ack_ms = now + m_config.ack_interval_ms;
    peer.next_heartbeat_ms = now + m_config.heartbeat_interval_ms;
    ++m_stats.handshakes;
    if (peer.secure_enabled) {
        ++m_secure_stats.handshakes;
    }
    EmitLog(LogLevel::kInfo, LogEvent::kPeerConnected, peer.peer_id,
            peer.secure_enabled ? "peer connected (psk)" : "peer connected", 0);
    if (on_peer_connected) {
        CallbackDepthGuard guard(m_callback_depth);
        on_peer_connected(peer.peer_id);
    }
}

std::uint32_t PeerHub::AllocateConnId() {
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint32_t candidate = RandomNonZeroU32();
        if (m_by_conn_id.find(candidate) == m_by_conn_id.end()) {
            return candidate;
        }
    }
    return 0;
}

std::uint32_t PeerHub::NextSessionId() {
    std::uint8_t raw[4] = {};
    SecureRandomBytes(raw, sizeof(raw));
    std::uint32_t value = (static_cast<std::uint32_t>(raw[0]) << 24) |
                          (static_cast<std::uint32_t>(raw[1]) << 16) |
                          (static_cast<std::uint32_t>(raw[2]) << 8) |
                          static_cast<std::uint32_t>(raw[3]);
    value ^= m_session_id;
    return value == 0 ? 1U : value;
}

bool PeerHub::IsKnownClientNonce(const Peer& peer, const std::uint8_t* nonce) const noexcept {
    for (const auto& seen : peer.seen_client_nonces) {
        if (std::memcmp(seen.data(), nonce, kHandshakeNonceBytes) == 0) {
            return true;
        }
    }
    return false;
}

void PeerHub::RememberClientNonce(Peer& peer, const std::uint8_t* nonce) {
    if (peer.seen_client_nonces.size() >= Peer::kMaxRememberedClientNonces) {
        peer.seen_client_nonces.erase(peer.seen_client_nonces.begin());
    }
    std::array<std::uint8_t, kHandshakeNonceBytes> entry{};
    std::memcpy(entry.data(), nonce, kHandshakeNonceBytes);
    peer.seen_client_nonces.push_back(entry);
}

void PeerHub::ScheduleCleanup(Peer& peer, bool hard_remove) {
    peer.pending_cleanup = true;
    const auto found = m_pending_cleanup.find(peer.peer_id);
    if (found == m_pending_cleanup.end()) {
        m_pending_cleanup.emplace(peer.peer_id, hard_remove);
    } else if (hard_remove) {
        // 同一个 peer 先软回收后又 RemovePeer：升级为硬删除。
        found->second = true;
    }
}

void PeerHub::FlushDeferredCleanup() {
    if (m_callback_depth > 0 || m_flushing_deferred) {
        return;
    }
    m_flushing_deferred = true;
    if (m_close_deferred) {
        m_close_deferred = false;
        m_pending_cleanup.clear();
        m_by_conn_id.clear();
        m_peers.clear();
        m_flushing_deferred = false;
        return;
    }
    while (!m_pending_cleanup.empty()) {
        std::map<std::uint32_t, bool> batch;
        batch.swap(m_pending_cleanup);
        for (const auto& entry : batch) {
            const auto found = m_peers.find(entry.first);
            if (found == m_peers.end()) {
                continue;
            }
            Peer& peer = *found->second;
            if (!entry.second && !peer.pending_cleanup) {
                continue;  // 软回收已被 Connect 取消：重连复用该记录。
            }
            if (peer.conn_id != 0) {
                const auto route = m_by_conn_id.find(peer.conn_id);
                if (route != m_by_conn_id.end() && route->second == peer.peer_id) {
                    m_by_conn_id.erase(route);
                }
            }
            // erase 会析构 Peer 及其通道；此处回调深度必为 0，调用栈上
            // 没有正在执行的 AdvanceDelivery/FlushPending。
            m_peers.erase(found);
        }
    }
    m_flushing_deferred = false;
}

void PeerHub::DropPeer(Peer& peer, const char* reason) {
    if (peer.state == PeerState::kTimedOut || peer.state == PeerState::kClosed) {
        return;
    }
    peer.state = PeerState::kTimedOut;
    ++m_stats.timeouts;
    // PEB-01：回调里可能正在执行该 peer 的通道（on_stream/on_datagram）；
    // 此时 ResetChannels 会销毁调用栈上的通道对象。销毁统一推迟到安全点。
    if (m_callback_depth == 0) {
        ResetChannels(peer);
        ResetSecureSession(peer);
    } else {
        peer.session_keys_ready = false;
    }
    const bool timeout = reason != nullptr && std::strcmp(reason, "peer timeout") == 0;
    EmitLog(LogLevel::kWarn,
            timeout ? LogEvent::kPeerTimeout : LogEvent::kPeerDisconnected, peer.peer_id,
            reason != nullptr ? reason : "dropped", 0);
    // PEB-04：被动 peer 超时后回收，释放 max_peers 配额；主动 peer 保留
    // 记录供应用 Connect 重连。软回收可被回调里的 Connect 取消。
    if (!peer.outgoing) {
        ScheduleCleanup(peer, false);
    }
    if (on_peer_disconnected) {
        CallbackDepthGuard guard(m_callback_depth);
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
    FrameHeader outer;
    if (!DecodeHeader(data, bytes, &outer)) {
        ++m_stats.invalid_frames;
        EmitLog(LogLevel::kWarn, LogEvent::kFrameRejected, 0, "invalid frame header", bytes);
        return;
    }
    if (outer.type == FrameType::kHello) {
        HandleHelloPacket(data, bytes, outer, from, from_len);
        return;
    }
    if (outer.type == FrameType::kHelloAck) {
        HandleHelloAckPacket(data, bytes, outer, from);
        return;
    }
    Peer* peer = FindByConnId(outer.conn_id);
    if (peer == nullptr || peer->state != PeerState::kConnected) {
        ++m_stats.unknown_peer_frames;
        return;
    }
    if (peer->secure_enabled) {
        if (!peer->session_keys_ready) {
            ++m_secure_stats.plaintext_rejected;
            return;
        }
        if ((outer.flags & kFlagEncrypted) == 0) {
            // 加密 peer 只接受加密数据帧：明文注入直接拒绝（降级攻击）。
            ++m_secure_stats.plaintext_rejected;
            EmitLog(LogLevel::kWarn, LogEvent::kPlaintextRejected, peer->peer_id,
                    "plaintext data frame on secure peer", bytes);
            return;
        }
        std::vector<std::uint8_t> inner;
        OpenStatus status = OpenStatus::kMalformed;
        if (!OpenFrame(peer->rx_key, peer->rx_direction, &peer->data_rx_window, data, bytes,
                       &inner, &status)) {
            switch (status) {
                case OpenStatus::kReplay:
                    ++m_secure_stats.replay_drops;
                    EmitLog(LogLevel::kWarn, LogEvent::kReplayDrop, peer->peer_id,
                            "secure frame replay", bytes);
                    break;
                case OpenStatus::kAuthFailed:
                    ++m_secure_stats.auth_failures;
                    EmitLog(LogLevel::kWarn, LogEvent::kAuthFailure, peer->peer_id,
                            "secure frame auth failed", bytes);
                    break;
                default:
                    ++m_secure_stats.malformed_frames;
                    break;
            }
            return;
        }
        ++m_secure_stats.packets_opened;
        m_secure_stats.encrypted_bytes_received += static_cast<std::uint64_t>(bytes);
        FrameView view;
        if (!ParseFrame(inner.data(), inner.size(), &view)) {
            ++m_secure_stats.malformed_frames;
            return;
        }
        // 只有通过 AEAD 认证的帧才允许更新地址（防注入迁移）。
        if (!UdpSocket::SamePeer(from, peer->addr)) {
            peer->addr = from;
            peer->addr_len = from_len;
        }
        peer->last_rx_ms = NowMs();
        ++peer->frames_received;
        peer->bytes_received += static_cast<std::uint64_t>(bytes);
        DispatchFrame(*peer, view);
        return;
    }
    if ((outer.flags & kFlagEncrypted) != 0) {
        // 明文 peer 收到加密帧：对端配置不一致（或注入），无法处理。
        ++m_stats.invalid_frames;
        EmitLog(LogLevel::kWarn, LogEvent::kFrameRejected, peer->peer_id,
                "encrypted frame on plaintext peer", bytes);
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

void PeerHub::HandleHelloPacket(const std::uint8_t* data, std::size_t bytes,
                                const FrameHeader& outer, const sockaddr_storage& from,
                                socklen_t from_len) {
    if ((outer.flags & kFlagEncrypted) != 0) {
        if (!m_secure_enabled) {
            // 对端要求加密但本端没有 PSK：无法认证，按非法帧计数。
            ++m_stats.invalid_frames;
            EmitLog(LogLevel::kWarn, LogEvent::kAuthFailure, 0,
                    "secure hello without local psk", bytes);
            return;
        }
        HandleSecureHello(data, bytes, outer, from, from_len);
        return;
    }
    if (m_secure_enabled) {
        // 加密模式下的明文 HELLO 是降级尝试：不创建 peer、不回 ACK。
        ++m_secure_stats.plaintext_rejected;
        EmitLog(LogLevel::kWarn, LogEvent::kPlaintextRejected, 0,
                "plaintext hello rejected in secure mode", bytes);
        return;
    }
    FrameView view;
    if (!ParseFrame(data, bytes, &view) || view.header.type != FrameType::kHello) {
        ++m_stats.invalid_frames;
        return;
    }
    HandleHello(view, from, from_len);
}

void PeerHub::HandleHelloAckPacket(const std::uint8_t* data, std::size_t bytes,
                                   const FrameHeader& outer, const sockaddr_storage& from) {
    if ((outer.flags & kFlagEncrypted) != 0) {
        // 加密 ACK 按 peer 自己的 PSK 处理，而不是 Hub 默认 PSK：
        // per-peer SetPeerPreSharedKey 场景下 Hub 本身可以没有默认 PSK。
        // conn_id 找不到时继续交给 HandleSecureHelloAck：服务端可能因
        // conn_id 冲突改过 id，内部会按地址回退，并用 AEAD 认证兜底。
        Peer* peer = FindByConnId(outer.conn_id);
        if (peer != nullptr && !peer->secure_enabled) {
            ++m_stats.invalid_frames;
            return;
        }
        HandleSecureHelloAck(data, bytes, outer, from);
        return;
    }
    Peer* peer = FindByConnId(outer.conn_id);
    if (peer != nullptr && peer->secure_enabled) {
        // 加密 peer 的明文 HELLO_ACK：拒绝，避免降级到无认证会话。
        ++m_secure_stats.plaintext_rejected;
        EmitLog(LogLevel::kWarn, LogEvent::kPlaintextRejected, peer->peer_id,
                "plaintext hello_ack rejected", bytes);
        return;
    }
    FrameView view;
    if (!ParseFrame(data, bytes, &view) || view.header.type != FrameType::kHelloAck) {
        ++m_stats.invalid_frames;
        return;
    }
    HandleHelloAck(view, from);
}

void PeerHub::HandleSecureHello(const std::uint8_t* data, std::size_t bytes,
                                const FrameHeader& outer, const sockaddr_storage& from,
                                socklen_t from_len) {
    std::uint8_t inner_type = 0;
    std::uint32_t session = 0;
    std::uint32_t handshake_conn_id = 0;
    std::vector<std::uint8_t> body;
    OpenStatus status = OpenStatus::kMalformed;
    if (!OpenHandshakeFrame(m_psk, SecureDirection::kClientToServer, data, bytes, &inner_type,
                            &session, &handshake_conn_id, &body, &status)) {
        if (status == OpenStatus::kAuthFailed) {
            ++m_secure_stats.auth_failures;
            EmitLog(LogLevel::kWarn, LogEvent::kAuthFailure, 0,
                    "secure hello auth failed (wrong psk?)", bytes);
        } else {
            ++m_secure_stats.malformed_frames;
        }
        return;
    }
    if (inner_type != static_cast<std::uint8_t>(FrameType::kHello)) {
        ++m_secure_stats.malformed_frames;
        return;
    }
    HelloBody hello;
    if (!DecodeHelloBody(body.data(), body.size(), &hello) || hello.session != session) {
        ++m_secure_stats.auth_failures;
        return;
    }
    if (!TimestampAcceptable(hello.timestamp_ms)) {
        ++m_secure_stats.replay_drops;
        return;
    }
    Peer* peer = FindByAddress(from, from_len);
    if (peer != nullptr && peer->secure_enabled && peer->session_keys_ready &&
        hello.session == peer->session_id &&
        std::memcmp(hello.nonce, peer->client_nonce, kHandshakeNonceBytes) == 0) {
        // HELLO_ACK 丢失后的幂等重传：重发缓存 ACK，不重置会话。
        ++m_secure_stats.handshake_retries;
        peer->last_rx_ms = NowMs();
        if (!peer->cached_hello_ack.empty()) {
            (void)SendRawPacket(*peer, peer->cached_hello_ack.data(),
                                peer->cached_hello_ack.size());
        }
        if (peer->state != PeerState::kConnected) {
            MarkConnected(*peer);
        }
        return;
    }
    if (peer != nullptr) {
        // PEB-03：已见 nonce / 更旧时间戳的 HELLO 属于重放。必须拒绝且保持
        // 当前会话不变，否则窗口内抓包重放可反复打断活动会话（会话抖动/DoS）。
        if (IsKnownClientNonce(*peer, hello.nonce)) {
            ++m_secure_stats.replay_drops;
            EmitLog(LogLevel::kWarn, LogEvent::kReplayDrop, peer->peer_id,
                    "secure hello replay (known nonce)", bytes);
            return;
        }
        if (peer->last_hello_timestamp_ms != 0 &&
            hello.timestamp_ms < peer->last_hello_timestamp_ms) {
            ++m_secure_stats.replay_drops;
            EmitLog(LogLevel::kWarn, LogEvent::kReplayDrop, peer->peer_id,
                    "secure hello replay (stale timestamp)", bytes);
            return;
        }
    }
    if (peer == nullptr) {
        if (!m_config.accept_unknown_peers) {
            ++m_stats.unknown_peer_frames;
            return;
        }
        if (m_peers.size() >= m_config.max_peers) {
            // PEB-04：先回收已超时的被动 peer，再判断容量。
            FlushDeferredCleanup();
        }
        if (m_peers.size() >= m_config.max_peers) {
            ++m_stats.peers_rejected;
            EmitLog(LogLevel::kWarn, LogEvent::kHandshakeFailed, 0,
                    "secure hello rejected: max peers", bytes);
            return;
        }
        peer = CreatePeer(AllocatePeerId(), from, from_len);
        peer->outgoing = false;
        peer->session_id = session;
        peer->conn_id = 0;
    } else if (peer->session_id != session) {
        // 同一地址的新会话（对端重启）：旧通道与旧密钥整体作废。
        if (peer->conn_id != 0) {
            m_by_conn_id.erase(peer->conn_id);
        }
        ResetChannels(*peer);
        ResetSecureSession(*peer);
        peer->session_id = session;
        peer->conn_id = 0;
    } else if (peer->conn_id != 0 && handshake_conn_id != 0 &&
               handshake_conn_id != peer->conn_id) {
        // PEB-02：同一 session 但 client 换了 conn_id（老客户端重连时未换
        // session）：视作新会话，重置接收侧通道并采纳新 conn_id；否则新流
        // seq=1 会被旧接收窗口判 stale。
        m_by_conn_id.erase(peer->conn_id);
        ResetChannels(*peer);
        peer->conn_id = 0;
    }
    if (peer->conn_id == 0) {
        std::uint32_t chosen = handshake_conn_id != 0 ? handshake_conn_id : outer.conn_id;
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
    // 新会话：安装加密状态并派生会话密钥（每 peer 独立 nonce -> 独立密钥）。
    peer->secure_enabled = true;
    std::memcpy(peer->psk, m_psk, kPskBytes);
    // PEB-03：记录本次已接受的 nonce 与时间戳（跨会话保留，供旧 HELLO 重放
    // 检测）。必须在安装密钥前更新，保证「已接受」与「已见」一致。
    RememberClientNonce(*peer, hello.nonce);
    peer->last_hello_timestamp_ms = hello.timestamp_ms;
    std::memcpy(peer->client_nonce, hello.nonce, kHandshakeNonceBytes);
    SecureRandomBytes(peer->server_nonce, kHandshakeNonceBytes);
    peer->tx_direction = SecureDirection::kServerToClient;
    peer->rx_direction = SecureDirection::kClientToServer;
    DeriveSessionKey(peer->psk, peer->client_nonce, peer->server_nonce, peer->rx_direction,
                     peer->rx_key);
    DeriveSessionKey(peer->psk, peer->client_nonce, peer->server_nonce, peer->tx_direction,
                     peer->tx_key);
    peer->session_keys_ready = true;
    peer->tx_wire_seq = 0;
    peer->handshake_rx_window.Reset();
    peer->data_rx_window.Reset();
    peer->handshake_tx_seq = RandomNonZeroU32();
    peer->cached_hello_ack.clear();
    peer->last_rx_ms = NowMs();
    SendSecureHelloAck(*peer);
    if (peer->state != PeerState::kConnected) {
        MarkConnected(*peer);
    }
}

void PeerHub::SendSecureHelloAck(Peer& peer) {
    if (peer.cached_hello_ack.empty()) {
        HelloAckBody body;
        body.session = peer.session_id;
        std::memcpy(body.client_nonce, peer.client_nonce, kHandshakeNonceBytes);
        std::memcpy(body.server_nonce, peer.server_nonce, kHandshakeNonceBytes);
        body.timestamp_ms = WallClockMs();
        std::uint8_t raw[kHelloAckBodyBytes];
        if (!EncodeHelloAckBody(body, raw, sizeof(raw)) ||
            !SealHandshakeFrame(peer.psk, SecureDirection::kServerToClient,
                                static_cast<std::uint8_t>(FrameType::kHelloAck),
                                peer.session_id, peer.conn_id, peer.handshake_tx_seq, raw,
                                sizeof(raw), &peer.cached_hello_ack)) {
            ++m_stats.send_errors;
            EmitLog(LogLevel::kError, LogEvent::kSendError, peer.peer_id,
                    "seal secure hello_ack failed", 0);
            return;
        }
    }
    (void)SendRawPacket(peer, peer.cached_hello_ack.data(), peer.cached_hello_ack.size());
}

void PeerHub::HandleSecureHelloAck(const std::uint8_t* data, std::size_t bytes,
                                   const FrameHeader& outer, const sockaddr_storage& from) {
    Peer* peer = FindByConnId(outer.conn_id);
    if (peer == nullptr) {
        // 监听端可能因冲突改过 conn_id：按地址回退，ACK 本身有 AEAD 认证。
        peer = FindByAddress(from, sizeof(sockaddr_storage));
        if (peer == nullptr || !peer->outgoing || !peer->secure_enabled) {
            ++m_stats.unknown_peer_frames;
            return;
        }
        if (outer.conn_id != 0 && outer.conn_id != peer->conn_id) {
            m_by_conn_id.erase(peer->conn_id);
            peer->conn_id = outer.conn_id;
            m_by_conn_id[peer->conn_id] = peer->peer_id;
        }
    }
    if (!peer->outgoing || !peer->secure_enabled) {
        ++m_stats.invalid_frames;
        return;
    }
    std::uint8_t inner_type = 0;
    std::uint32_t session = 0;
    std::uint32_t handshake_conn_id = 0;
    std::vector<std::uint8_t> body;
    OpenStatus status = OpenStatus::kMalformed;
    if (!OpenHandshakeFrame(peer->psk, SecureDirection::kServerToClient, data, bytes,
                            &inner_type, &session, &handshake_conn_id, &body, &status)) {
        if (status == OpenStatus::kAuthFailed) {
            ++m_secure_stats.auth_failures;
            EmitLog(LogLevel::kWarn, LogEvent::kAuthFailure, peer->peer_id,
                    "secure hello_ack auth failed (wrong psk?)", bytes);
        } else {
            ++m_secure_stats.malformed_frames;
        }
        return;
    }
    if (inner_type != static_cast<std::uint8_t>(FrameType::kHelloAck)) {
        ++m_secure_stats.malformed_frames;
        return;
    }
    HelloAckBody ack;
    if (!DecodeHelloAckBody(body.data(), body.size(), &ack) ||
        ack.session != peer->session_id ||
        std::memcmp(ack.client_nonce, peer->client_nonce, kHandshakeNonceBytes) != 0) {
        ++m_secure_stats.auth_failures;
        return;
    }
    if (!TimestampAcceptable(ack.timestamp_ms)) {
        ++m_secure_stats.replay_drops;
        return;
    }
    if (!peer->handshake_rx_window.WouldAccept(outer.seq)) {
        ++m_secure_stats.replay_drops;
        return;
    }
    peer->handshake_rx_window.Accept(outer.seq);
    if (handshake_conn_id != 0 && handshake_conn_id != peer->conn_id) {
        m_by_conn_id.erase(peer->conn_id);
        peer->conn_id = handshake_conn_id;
        m_by_conn_id[peer->conn_id] = peer->peer_id;
    }
    std::memcpy(peer->server_nonce, ack.server_nonce, kHandshakeNonceBytes);
    peer->tx_direction = SecureDirection::kClientToServer;
    peer->rx_direction = SecureDirection::kServerToClient;
    DeriveSessionKey(peer->psk, peer->client_nonce, peer->server_nonce, peer->tx_direction,
                     peer->tx_key);
    DeriveSessionKey(peer->psk, peer->client_nonce, peer->server_nonce, peer->rx_direction,
                     peer->rx_key);
    peer->session_keys_ready = true;
    peer->tx_wire_seq = 0;
    peer->data_rx_window.Reset();
    if (!UdpSocket::SamePeer(from, peer->addr)) {
        peer->addr = from;
        peer->addr_len = sizeof(sockaddr_storage);
    }
    peer->last_rx_ms = NowMs();
    if (peer->state != PeerState::kConnected) {
        MarkConnected(*peer);
    }
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
            if (m_log_hook != nullptr) {
                // 可靠层不直接依赖 LogHook：这里按统计增量转成事件，
                // 每 tick 每通道最多两条，避免逐包回调拖慢热路径。
                const ReliableChannel::Stats channel_stats = channel.second->stats();
                std::uint64_t& last_retransmit = peer.log_retransmits[channel.first];
                if (channel_stats.retransmits > last_retransmit) {
                    EmitLog(LogLevel::kDebug, LogEvent::kRetransmit, peer.peer_id,
                            "reliable retransmit",
                            channel_stats.retransmits - last_retransmit);
                    last_retransmit = channel_stats.retransmits;
                }
                // 丢包 = 快速重传降窗（loss_events）或 PTO 超时降窗
                // （timeout_loss_events）；两者都会触发重传。
                const std::uint64_t lost =
                    channel_stats.loss_events + channel_stats.timeout_loss_events;
                std::uint64_t& last_loss = peer.log_loss_events[channel.first];
                if (lost > last_loss) {
                    EmitLog(LogLevel::kWarn, LogEvent::kPacketLost, peer.peer_id,
                            "reliable packet loss", lost - last_loss);
                    last_loss = lost;
                }
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
    // 先清掉上一轮遗留的待销毁 peer（例如并发路径入队后尚未到安全点）。
    FlushDeferredCleanup();
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
                EmitLog(LogLevel::kError, LogEvent::kRecvError, 0, "recvfrom failed", 0);
                break;
            }
            ++m_stats.frames_received;
            m_stats.bytes_received += static_cast<std::uint64_t>(received);
            HandlePacket(m_rx_buffer.data(), static_cast<std::size_t>(received), from, from_len);
            ++processed;
            if (!m_socket.IsOpen()) {
                // 回调里 Close() 关闭了 socket：本轮不再继续收包。
                break;
            }
        }
    } else if (ready < 0) {
        ++m_stats.poll_errors;
    }
    Tick(NowMs());
    // PEB-01/04：回调栈已退出，销毁延迟队列里的 peer（通道）并释放容量。
    FlushDeferredCleanup();
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
