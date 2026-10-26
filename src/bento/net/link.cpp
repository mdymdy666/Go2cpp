/**
 * @file link.cpp
 * @brief Bento UDP 链路层实现：socket、握手、心跳、超时、事件循环。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include "go2cpp/bento/net/link.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

namespace go2cpp::bento {
namespace {

/// HELLO / HELLO_ACK 载荷 magic：'B' 'E' 'N' 'T'。
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

void MakeHelloPayload(std::uint32_t session, std::uint8_t out[8]) noexcept {
    out[0] = 'B';
    out[1] = 'E';
    out[2] = 'N';
    out[3] = 'T';
    WriteU32(out + 4, session);
}

}  // namespace

const char* LinkStateName(LinkState state) noexcept {
    switch (state) {
        case LinkState::kIdle:
            return "IDLE";
        case LinkState::kConnecting:
            return "CONNECTING";
        case LinkState::kConnected:
            return "CONNECTED";
        case LinkState::kClosed:
            return "CLOSED";
        case LinkState::kTimedOut:
            return "TIMED_OUT";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// UdpSocket
// ---------------------------------------------------------------------------

UdpSocket::~UdpSocket() {
    Close();
}

bool UdpSocket::ResolveIPv4(const std::string& address, std::uint16_t port, sockaddr_in* out,
                            std::string* error) {
    if (out == nullptr) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        if (error != nullptr) {
            *error = "invalid IPv4 address: " + address;
        }
        return false;
    }
    *out = addr;
    return true;
}

bool UdpSocket::Open(const std::string& bind_address, std::uint16_t port, std::string* error) {
    if (m_fd >= 0) {
        if (error != nullptr) {
            *error = "socket already open";
        }
        return false;
    }
    sockaddr_in addr{};
    if (!ResolveIPv4(bind_address, port, &addr, error)) {
        return false;
    }
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error != nullptr) {
            *error = std::string("socket: ") + std::strerror(errno);
        }
        return false;
    }
    const int reuse = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (error != nullptr) {
            *error = std::string("bind: ") + std::strerror(errno);
        }
        ::close(fd);
        return false;
    }
    sockaddr_in local{};
    socklen_t local_length = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length) < 0) {
        if (error != nullptr) {
            *error = std::string("getsockname: ") + std::strerror(errno);
        }
        ::close(fd);
        return false;
    }
    m_fd = fd;
    m_local_port = ntohs(local.sin_port);
    return true;
}

void UdpSocket::Close() noexcept {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_local_port = 0;
}

bool UdpSocket::SendTo(const sockaddr_in& peer, const std::uint8_t* data,
                       std::size_t bytes) noexcept {
    if (m_fd < 0 || data == nullptr) {
        return false;
    }
    for (;;) {
        const ssize_t sent =
            ::sendto(m_fd, data, bytes, 0, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
        if (sent == static_cast<ssize_t>(bytes)) {
            return true;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

int UdpSocket::WaitReadable(int timeout_ms) noexcept {
    if (m_fd < 0) {
        return -1;
    }
    pollfd descriptor{};
    descriptor.fd = m_fd;
    descriptor.events = POLLIN;
    for (;;) {
        const int result = ::poll(&descriptor, 1, timeout_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return result;
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            return -1;
        }
        return (descriptor.revents & POLLIN) != 0 ? 1 : 0;
    }
}

int UdpSocket::RecvFrom(std::uint8_t* buffer, std::size_t capacity, sockaddr_in* from) noexcept {
    if (m_fd < 0 || buffer == nullptr) {
        return -1;
    }
    for (;;) {
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        const ssize_t received =
            ::recvfrom(m_fd, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (received >= 0) {
            if (from != nullptr) {
                *from = peer;
            }
            return static_cast<int>(received);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
}

bool UdpSocket::SamePeer(const sockaddr_in& a, const sockaddr_in& b) noexcept {
    return a.sin_family == b.sin_family && a.sin_port == b.sin_port &&
           a.sin_addr.s_addr == b.sin_addr.s_addr;
}

// ---------------------------------------------------------------------------
// Link
// ---------------------------------------------------------------------------

Link::Link(LinkConfig config) : m_config(std::move(config)), m_rx_buffer(65536) {
    m_video = std::make_unique<VideoChannel>(
        [this](const std::uint8_t* data, std::size_t bytes) { return SendBuiltFrame(data, bytes); },
        [this](std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes) {
            if (on_video) {
                on_video(kChannelVideo, frame_id, key, data, bytes);
            }
        });
}

Link::~Link() {
    Close();
}

bool Link::Open(std::string* error) {
    std::string local_error;
    if (!m_socket.Open(m_config.bind_address, m_config.bind_port, &local_error)) {
        if (error != nullptr) {
            *error = local_error;
        }
        return false;
    }
    m_peer_configured = !m_config.peer_address.empty() && m_config.peer_port != 0;
    m_is_listener = !m_peer_configured;
    if (m_peer_configured) {
        if (!UdpSocket::ResolveIPv4(m_config.peer_address, m_config.peer_port, &m_peer,
                                    &local_error)) {
            m_socket.Close();
            if (error != nullptr) {
                *error = local_error;
            }
            return false;
        }
        m_have_peer = true;
    }
    const std::uint64_t now = NowMs();
    m_last_rx_ms = now;
    m_last_tx_ms = now;
    m_next_ack_tick_ms = now + m_config.ack_interval_ms;
    m_next_heartbeat_ms = now + m_config.heartbeat_interval_ms;
    m_state = LinkState::kIdle;
    return true;
}

void Link::Connect() {
    if (!m_socket.IsOpen() || m_state != LinkState::kIdle || !m_peer_configured) {
        return;
    }
    const std::uint64_t now = NowMs();
    m_state = LinkState::kConnecting;
    m_handshake_start_ms = now;
    m_next_hello_ms = now;
    m_hello_attempts = 0;
    m_hello_pto_ms = m_config.hello_initial_pto_ms;
    if (m_session_id == 0) {
        m_session_id =
            m_config.random_seed != 0
                ? m_config.random_seed
                : static_cast<std::uint32_t>(now ^ (static_cast<std::uint64_t>(::getpid()) << 16));
    }
    SendHello();
}

void Link::Close() {
    m_state = LinkState::kClosed;
    m_socket.Close();
}

int Link::RunOnce(int timeout_ms) {
    if (!m_socket.IsOpen()) {
        return 0;
    }
    int processed = 0;
    const int ready = m_socket.WaitReadable(timeout_ms);
    if (ready > 0) {
        while (processed < kMaxFramesPerRun) {
            sockaddr_in from{};
            const int received = m_socket.RecvFrom(m_rx_buffer.data(), m_rx_buffer.size(), &from);
            if (received == 0) {
                break;
            }
            if (received < 0) {
                ++m_stats.recv_errors;
                break;
            }
            ++m_stats.frames_received;
            m_stats.bytes_received += static_cast<std::uint64_t>(received);
            HandleFrame(m_rx_buffer.data(), static_cast<std::size_t>(received), from);
            ++processed;
            if (m_state == LinkState::kTimedOut || m_state == LinkState::kClosed) {
                break;
            }
        }
    } else if (ready < 0) {
        ++m_stats.poll_errors;
    }
    Tick(NowMs());
    return processed;
}

int Link::RunFor(int duration_ms) {
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

bool Link::PeerMatches(const sockaddr_in& from) const noexcept {
    return m_have_peer && UdpSocket::SamePeer(from, m_peer);
}

void Link::HandleHello(const FrameView& view, const sockaddr_in& from) {
    if (view.payload_bytes < 8 || !IsHelloMagic(view.payload)) {
        ++m_stats.invalid_frames;
        return;
    }
    const std::uint32_t session = ReadU32(view.payload + 4);
    if (session == 0) {
        ++m_stats.invalid_frames;
        return;
    }
    if (m_have_peer && !PeerMatches(from)) {
        ++m_stats.unknown_peer_frames;
        return;
    }
    if (!m_have_peer) {
        m_peer = from;
        m_have_peer = true;
        m_session_id = session;
    } else if (session != m_session_id) {
        // 同一地址的新 session：视为对端重启。重置可靠/不可靠通道状态，
        // 否则接收端 expected 会停在新对端永远不会产生的旧 seq 上。
        // 视频重组状态无法安全复用，旧 frame_id 会按 stale 丢弃；文档中标注。
        m_session_id = session;
        m_reliable.clear();
        m_unreliable.clear();
        m_relation.Reset();
    }

    // 重复 HELLO 也要回 ACK：HELLO_ACK 丢失时客户端会重发 HELLO。
    std::uint8_t payload[8];
    MakeHelloPayload(session, payload);
    FrameHeader header;
    header.type = FrameType::kHelloAck;
    header.channel = 0;
    header.seq = session;
    (void)SendControl(header, nullptr, payload, 8);
    m_last_rx_ms = NowMs();
    if (m_state != LinkState::kConnected) {
        m_state = LinkState::kConnected;
        ++m_stats.handshakes;
        if (on_connected) {
            on_connected();
        }
    }
}

void Link::HandleHelloAck(const FrameView& view, const sockaddr_in& from) {
    (void)from;
    if (view.payload_bytes < 8 || !IsHelloMagic(view.payload)) {
        ++m_stats.invalid_frames;
        return;
    }
    const std::uint32_t session = ReadU32(view.payload + 4);
    if (session != m_session_id || view.header.seq != m_session_id) {
        ++m_stats.invalid_frames;
        return;
    }
    m_last_rx_ms = NowMs();
    if (m_state == LinkState::kConnecting) {
        m_state = LinkState::kConnected;
        ++m_stats.handshakes;
        if (on_connected) {
            on_connected();
        }
    }
}

void Link::HandleAckFrame(const FrameView& view) {
    auto it = m_reliable.find(view.header.channel);
    if (it == m_reliable.end()) {
        // 对端 ACK 一个尚未建立的通道：合法但无意义（可能对端先发了数据）。
        return;
    }
    it->second->OnAck(view.ack);
}

void Link::HandleBarrier(const FrameView& view) {
    const std::uint32_t barrier_seq = view.header.seq;
    // 先抬各不可靠通道的 applied_seq，再释放 Relation 的 pending：
    // 这样 barrier 之前的分片/消息按 DropStale 丢弃，barrier 之后的
    // 依赖项可以立即交付。
    for (auto& entry : m_unreliable) {
        entry.second->OnBarrier(barrier_seq);
    }
    m_relation.OnBarrier(barrier_seq);
    ++m_stats.barriers_received;
}

void Link::HandleFrame(const std::uint8_t* data, std::size_t bytes, const sockaddr_in& from) {
    FrameView view;
    if (!ParseFrame(data, bytes, &view)) {
        ++m_stats.invalid_frames;
        return;
    }
    if (view.header.type == FrameType::kHello) {
        if (m_is_listener || (m_peer_configured && PeerMatches(from))) {
            HandleHello(view, from);
        } else {
            ++m_stats.unknown_peer_frames;
        }
        return;
    }
    if (view.header.type == FrameType::kHelloAck) {
        if (!m_is_listener && m_state == LinkState::kConnecting && PeerMatches(from)) {
            HandleHelloAck(view, from);
        } else {
            ++m_stats.unknown_peer_frames;
        }
        return;
    }
    if (m_state != LinkState::kConnected || !PeerMatches(from)) {
        ++m_stats.unknown_peer_frames;
        return;
    }
    m_last_rx_ms = NowMs();

    switch (view.header.type) {
        case FrameType::kStream: {
            ReliableChannel* channel = GetOrCreateReliable(view.header.channel);
            if (channel == nullptr) {
                ++m_stats.unknown_peer_frames;
                return;
            }
            channel->OnFrame(view.header, view.payload, view.payload_bytes,
                             view.has_piggyback_ack ? &view.ack : nullptr);
            break;
        }
        case FrameType::kDatagram: {
            UnreliableChannel* channel = GetOrCreateUnreliable(view.header.channel);
            if (channel == nullptr) {
                ++m_stats.unknown_peer_frames;
                return;
            }
            channel->OnFrame(view.header, view.payload, view.payload_bytes);
            break;
        }
        case FrameType::kVideo:
            m_video->OnFrame(view.header, view.payload, view.payload_bytes);
            break;
        case FrameType::kAck:
            HandleAckFrame(view);
            break;
        case FrameType::kBarrier:
            HandleBarrier(view);
            break;
        case FrameType::kHello:
        case FrameType::kHelloAck:
            break;  // 已在上方处理
    }
}

ReliableChannel* Link::GetOrCreateReliable(std::uint8_t channel) {
    auto it = m_reliable.find(channel);
    if (it != m_reliable.end()) {
        return it->second.get();
    }
    if (m_reliable.size() + m_unreliable.size() >= kMaxChannels) {
        return nullptr;
    }
    auto created = std::make_unique<ReliableChannel>(
        channel, &m_relation,
        [this, channel](std::uint32_t /*seq*/, const std::uint8_t* data, std::size_t bytes) {
            if (on_stream) {
                on_stream(channel, data, bytes);
            }
        });
    created->SetFrameSink([this](const std::uint8_t* data, std::size_t bytes) {
        return SendBuiltFrame(data, bytes);
    });
    ReliableChannel* raw = created.get();
    m_reliable.emplace(channel, std::move(created));
    return raw;
}

UnreliableChannel* Link::GetOrCreateUnreliable(std::uint8_t channel) {
    auto it = m_unreliable.find(channel);
    if (it != m_unreliable.end()) {
        return it->second.get();
    }
    if (m_reliable.size() + m_unreliable.size() >= kMaxChannels) {
        return nullptr;
    }
    auto created = std::make_unique<UnreliableChannel>(
        channel, &m_relation,
        [this, channel](std::uint32_t /*seq*/, const std::uint8_t* data, std::size_t bytes) {
            if (on_datagram) {
                on_datagram(channel, data, bytes);
            }
        });
    created->SetFrameSink([this](const std::uint8_t* data, std::size_t bytes) {
        return SendBuiltFrame(data, bytes);
    });
    UnreliableChannel* raw = created.get();
    m_unreliable.emplace(channel, std::move(created));
    return raw;
}

bool Link::SendBuiltFrame(const std::uint8_t* data, std::size_t bytes) {
    if (!m_socket.IsOpen()) {
        return false;
    }
    if (!m_have_peer) {
        ++m_stats.send_errors;
        return false;
    }
    if (!m_socket.SendTo(m_peer, data, bytes)) {
        ++m_stats.send_errors;
        return false;
    }
    ++m_stats.frames_sent;
    m_stats.bytes_sent += bytes;
    m_last_tx_ms = NowMs();
    return true;
}

bool Link::SendControl(const FrameHeader& header, const AckState* ack, const std::uint8_t* payload,
                       std::size_t bytes) {
    const std::vector<std::uint8_t> frame = BuildFrame(header, ack, payload, bytes);
    if (frame.empty()) {
        ++m_stats.send_errors;
        return false;
    }
    return SendBuiltFrame(frame.data(), frame.size());
}

void Link::SendHello() {
    if (!m_peer_configured || !m_have_peer) {
        return;
    }
    if (m_hello_attempts > 0) {
        ++m_stats.hello_retransmits;
    }
    ++m_hello_attempts;
    const std::uint64_t now = NowMs();
    std::uint8_t payload[8];
    MakeHelloPayload(m_session_id, payload);
    FrameHeader header;
    header.type = FrameType::kHello;
    header.channel = 0;
    header.seq = m_session_id;
    (void)SendControl(header, nullptr, payload, 8);
    m_next_hello_ms = now + m_hello_pto_ms;
    m_hello_pto_ms = std::min(m_hello_pto_ms * 2, m_config.hello_max_pto_ms);
}

bool Link::SendAckFrame(std::uint8_t channel, bool heartbeat) {
    AckState ack;
    ReliableChannel* reliable = nullptr;
    auto it = m_reliable.find(channel);
    if (it != m_reliable.end()) {
        reliable = it->second.get();
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
    if (!SendControl(header, nullptr, payload, kAckBytes)) {
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

void Link::Tick(std::uint64_t now_ms) {
    if (m_state == LinkState::kConnecting) {
        if (now_ms >= m_next_hello_ms) {
            if (m_hello_attempts >= m_config.hello_max_attempts ||
                now_ms - m_handshake_start_ms >= m_config.handshake_timeout_ms) {
                Fail("hello timeout");
                return;
            }
            SendHello();
        }
    }
    if (m_state != LinkState::kConnected) {
        return;
    }

    if (now_ms >= m_next_ack_tick_ms) {
        m_next_ack_tick_ms = now_ms + m_config.ack_interval_ms;
        for (auto& entry : m_reliable) {
            if (entry.second->HasPendingAck()) {
                (void)SendAckFrame(entry.first, false);
            }
        }
    }
    if (now_ms >= m_next_heartbeat_ms) {
        m_next_heartbeat_ms = now_ms + m_config.heartbeat_interval_ms;
        (void)SendAckFrame(kChannelMove, true);
    }
    for (auto& entry : m_reliable) {
        entry.second->OnTimer(now_ms);
    }
    m_video->OnTimer(now_ms);

    if (now_ms >= m_last_rx_ms && now_ms - m_last_rx_ms > m_config.peer_timeout_ms) {
        Fail("peer timeout");
    }
}

void Link::Fail(const char* reason) {
    if (m_state == LinkState::kTimedOut || m_state == LinkState::kClosed) {
        return;
    }
    m_state = LinkState::kTimedOut;
    ++m_stats.timeouts;
    m_socket.Close();
    if (on_disconnected) {
        on_disconnected(reason);
    }
}

bool Link::SendStream(std::uint8_t channel, const void* data, std::size_t bytes,
                      std::uint32_t dep_seq) {
    if (m_state != LinkState::kConnected) {
        return false;
    }
    ReliableChannel* target = GetOrCreateReliable(channel);
    if (target == nullptr) {
        return false;
    }
    return target->Send(static_cast<const std::uint8_t*>(data), bytes, dep_seq);
}

bool Link::SendDatagram(std::uint8_t channel, const void* data, std::size_t bytes,
                        std::uint32_t dep_seq) {
    if (m_state != LinkState::kConnected) {
        return false;
    }
    UnreliableChannel* target = GetOrCreateUnreliable(channel);
    if (target == nullptr) {
        return false;
    }
    return target->Send(static_cast<const std::uint8_t*>(data), bytes, dep_seq);
}

bool Link::SendVideoFrame(std::uint32_t frame_id, bool key, const void* data, std::size_t bytes) {
    if (m_state != LinkState::kConnected) {
        return false;
    }
    return m_video->SendFrame(frame_id, key, static_cast<const std::uint8_t*>(data), bytes);
}

bool Link::SendBarrier(std::uint32_t barrier_seq) {
    if (m_state != LinkState::kConnected) {
        return false;
    }
    FrameHeader header;
    header.type = FrameType::kBarrier;
    header.channel = 0;
    header.seq = barrier_seq;
    return SendControl(header, nullptr, nullptr, 0);
}

ReliableChannel::Stats Link::reliable_stats(std::uint8_t channel) const {
    auto it = m_reliable.find(channel);
    if (it == m_reliable.end()) {
        return {};
    }
    return it->second->stats();
}

UnreliableChannel::Stats Link::unreliable_stats(std::uint8_t channel) const {
    auto it = m_unreliable.find(channel);
    if (it == m_unreliable.end()) {
        return {};
    }
    return it->second->stats();
}

VideoChannel::Stats Link::video_stats() const {
    return m_video->stats();
}

}  // namespace go2cpp::bento
