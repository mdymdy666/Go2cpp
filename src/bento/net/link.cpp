/**
 * @file link.cpp
 * @brief Bento UDP 链路层实现：socket、握手、心跳、超时、事件循环。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include "go2cpp/bento/net/link.hpp"

#include <arpa/inet.h>
#include <linux/errqueue.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace go2cpp::bento {
namespace {

/// 路径 MTU 的安全下限（RFC 791 最小重组缓冲 576B）。
constexpr std::uint32_t kMinPathMtu = 576;
/// PMTUD 每档递增步长（UDP 载荷字节）。
constexpr std::uint32_t kPmtudStepBytes = 100;

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

void WriteU64(std::uint8_t* out, std::uint64_t value) noexcept {
    WriteU32(out, static_cast<std::uint32_t>(value >> 32));
    WriteU32(out + 4, static_cast<std::uint32_t>(value));
}

std::uint64_t ReadU64(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint64_t>(ReadU32(in)) << 32) | ReadU32(in + 4);
}

/// 生成非零 32 位随机数（握手 wire seq，避免跨连接 nonce 复用）。
std::uint32_t RandomNonZeroU32() {
    std::uint8_t raw[4] = {};
    SecureRandomBytes(raw, sizeof(raw));
    const std::uint32_t value = (static_cast<std::uint32_t>(raw[0]) << 24) |
                                (static_cast<std::uint32_t>(raw[1]) << 16) |
                                (static_cast<std::uint32_t>(raw[2]) << 8) |
                                static_cast<std::uint32_t>(raw[3]);
    return value == 0 ? 1U : value;
}

/// 握手时间戳使用墙钟（跨进程可比）；steady_clock 的 epoch 不保证一致。
std::uint64_t WallClockMs() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void MakeHelloPayload(std::uint32_t session, std::uint32_t conn_id,
                      std::uint8_t out[12]) noexcept {
    out[0] = 'B';
    out[1] = 'E';
    out[2] = 'N';
    out[3] = 'T';
    WriteU32(out + 4, session);
    WriteU32(out + 8, conn_id);
}

/// 0-RTT 恢复时“下一个 wire seq”。窗口未初始化表示尚未收到任何帧。
std::uint32_t NextWireSeq(const ReplayWindow& window) noexcept {
    if (!window.initialized()) {
        return 1;
    }
    const std::uint32_t next = window.highest() + 1;
    return next == 0 ? 1 : next;
}

/// X25519 公钥是否全零：全零表示对端不提供公钥，应回退纯 PSK。
bool IsZeroX25519Key(const std::uint8_t key[kX25519KeyBytes]) noexcept {
    std::uint8_t accumulator = 0;
    for (std::size_t i = 0; i < kX25519KeyBytes; ++i) {
        accumulator = static_cast<std::uint8_t>(accumulator | key[i]);
    }
    return accumulator == 0;
}

// ---------------------------------------------------------------------------
// 进程内 0-RTT 会话缓存（仅监听方角色）
// ---------------------------------------------------------------------------

/// 监听方在握手中保存的服务端会话快照。客户端 Resume 只需票据；
/// 监听方凭 conn_id 查此缓存即可在“服务端对象重建”后接受 0-RTT 首包。
struct CachedSession {
    std::uint32_t conn_id = 0;
    std::uint32_t session_id = 0;
    std::uint16_t local_port = 0;  ///< 监听方本地端口：避免不同监听实例串会话。
    std::uint8_t psk_fingerprint[8] = {};
    std::uint8_t client_nonce[kHandshakeNonceBytes] = {};
    std::uint8_t server_nonce[kHandshakeNonceBytes] = {};
    std::uint32_t server_next_wire_seq = 1;
    ReplayWindow client_rx_window;
    std::uint64_t expires_ms = 0;
};

std::mutex g_session_cache_mutex;
std::map<std::uint32_t, CachedSession> g_session_cache;

void PurgeExpiredSessions(std::uint64_t now_ms) {
    for (auto it = g_session_cache.begin(); it != g_session_cache.end();) {
        if (it->second.expires_ms <= now_ms) {
            it = g_session_cache.erase(it);
        } else {
            ++it;
        }
    }
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

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : m_fd(other.m_fd), m_local_port(other.m_local_port), m_family(other.m_family) {
    other.m_fd = -1;
    other.m_local_port = 0;
    other.m_family = AF_UNSPEC;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        m_fd = other.m_fd;
        m_local_port = other.m_local_port;
        m_family = other.m_family;
        other.m_fd = -1;
        other.m_local_port = 0;
        other.m_family = AF_UNSPEC;
    }
    return *this;
}

bool UdpSocket::Resolve(const std::string& address, std::uint16_t port, sockaddr_storage* out,
                        socklen_t* out_len, std::string* error) {
    if (out == nullptr || out_len == nullptr) {
        if (error != nullptr) {
            *error = "null resolve output";
        }
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    const std::string service = std::to_string(port);
    addrinfo* result = nullptr;
    const int resolved = ::getaddrinfo(address.c_str(), service.c_str(), &hints, &result);
    if (resolved != 0 || result == nullptr) {
        if (error != nullptr) {
            *error = "resolve '" + address + "': " + ::gai_strerror(resolved);
        }
        if (result != nullptr) {
            ::freeaddrinfo(result);
        }
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    std::memcpy(out, result->ai_addr, result->ai_addrlen);
    *out_len = static_cast<socklen_t>(result->ai_addrlen);
    ::freeaddrinfo(result);
    return true;
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
    const std::string address = bind_address.empty() ? "0.0.0.0" : bind_address;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    const std::string service = std::to_string(port);
    addrinfo* result = nullptr;
    const int resolved = ::getaddrinfo(address.c_str(), service.c_str(), &hints, &result);
    if (resolved != 0 || result == nullptr) {
        if (error != nullptr) {
            *error = "resolve bind '" + address + "': " + ::gai_strerror(resolved);
        }
        if (result != nullptr) {
            ::freeaddrinfo(result);
        }
        return false;
    }
    std::string last_error;
    for (addrinfo* entry = result; entry != nullptr; entry = entry->ai_next) {
        const int fd =
            ::socket(entry->ai_family, entry->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                     entry->ai_protocol);
        if (fd < 0) {
            last_error = std::string("socket: ") + std::strerror(errno);
            continue;
        }
        if (entry->ai_family == AF_INET6) {
            // 双栈：绑定 "::" 时同时接受 IPv4-mapped 客户端。
            int v6_only = 0;
            (void)::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only));
        }
        const int reuse = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        // 突发流量下内核接收队列是 UDP 的第一道丢包点：默认 rmem ~212KB，
        // 多 peer 场景下 150 个 80B 小包叠加 skb 开销就可能溢出。统一抬到
        // 1MB（内核按 rmem_max 截断），显著降低回环与弱网突发丢包。
        const int socket_buffer_bytes = 1 << 20;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socket_buffer_bytes,
                           sizeof(socket_buffer_bytes));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &socket_buffer_bytes,
                           sizeof(socket_buffer_bytes));
        if (::bind(fd, entry->ai_addr, entry->ai_addrlen) < 0) {
            last_error = std::string("bind: ") + std::strerror(errno);
            ::close(fd);
            continue;
        }
        sockaddr_storage local{};
        socklen_t local_length = sizeof(local);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length) < 0) {
            last_error = std::string("getsockname: ") + std::strerror(errno);
            ::close(fd);
            continue;
        }
        m_fd = fd;
        m_family = entry->ai_family;
        if (local.ss_family == AF_INET) {
            sockaddr_in local_v4{};
            std::memcpy(&local_v4, &local, sizeof(local_v4));
            m_local_port = ntohs(local_v4.sin_port);
        } else if (local.ss_family == AF_INET6) {
            sockaddr_in6 local_v6{};
            std::memcpy(&local_v6, &local, sizeof(local_v6));
            m_local_port = ntohs(local_v6.sin6_port);
        }
        ::freeaddrinfo(result);
        return true;
    }
    if (error != nullptr) {
        *error = last_error.empty() ? "bind: no usable address" : last_error;
    }
    ::freeaddrinfo(result);
    return false;
}

void UdpSocket::Close() noexcept {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_local_port = 0;
}

void UdpSocket::ConfigurePathMtu(bool enable) noexcept {
    if (m_fd < 0) {
        return;
    }
    // DF：PMTUD 开启时禁止内核分片，让超 MTU 的包立即失败并产生 ICMP。
    const int discover = enable ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
    if (m_family == AF_INET6) {
        (void)::setsockopt(m_fd, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &discover, sizeof(discover));
    } else {
        (void)::setsockopt(m_fd, IPPROTO_IP, IP_MTU_DISCOVER, &discover, sizeof(discover));
    }
    // 错误队列：接收 ICMP“fragmentation needed / unreachable”的 MTU 通知。
    const int on = 1;
    (void)::setsockopt(m_fd, IPPROTO_IP, IP_RECVERR, &on, sizeof(on));
    (void)::setsockopt(m_fd, IPPROTO_IPV6, IPV6_RECVERR, &on, sizeof(on));
}

int UdpSocket::ReceiveMtuError(std::uint32_t* reported_mtu) noexcept {
    if (m_fd < 0 || reported_mtu == nullptr) {
        return -1;
    }
    alignas(cmsghdr) std::uint8_t control[512];
    std::uint8_t payload_byte = 0;
    iovec iov{};
    iov.iov_base = &payload_byte;
    iov.iov_len = sizeof(payload_byte);
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    const ssize_t received = ::recvmsg(m_fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        const bool ipv4_error =
            cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_RECVERR;
        const bool ipv6_error =
            cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_RECVERR;
        if (!ipv4_error && !ipv6_error) {
            continue;
        }
        if (cmsg->cmsg_len < CMSG_LEN(sizeof(sock_extended_err))) {
            continue;
        }
        const auto* error =
            reinterpret_cast<const sock_extended_err*>(CMSG_DATA(cmsg));
        if (error->ee_origin != SO_EE_ORIGIN_ICMP &&
            error->ee_origin != SO_EE_ORIGIN_ICMP6) {
            continue;
        }
        if (error->ee_errno == EMSGSIZE) {
            const std::uint32_t overhead = m_family == AF_INET6 ? 48U : 28U;
            const std::uint32_t ip_mtu = error->ee_info;
            *reported_mtu = ip_mtu > overhead ? ip_mtu - overhead : kMinPathMtu;
            return 1;
        }
        if (error->ee_errno == ENETUNREACH || error->ee_errno == EHOSTUNREACH ||
            error->ee_errno == ENOBUFS) {
            // 没有具体 MTU 的不可达：退回保守值。
            *reported_mtu = 1200;
            return 1;
        }
    }
    return 0;
}

bool UdpSocket::SendTo(const sockaddr_storage& peer, socklen_t peer_len,
                       const std::uint8_t* data, std::size_t bytes) noexcept {
    if (m_fd < 0 || data == nullptr || peer_len == 0 ||
        (peer.ss_family != AF_INET && peer.ss_family != AF_INET6)) {
        return false;
    }
    for (;;) {
        const ssize_t sent = ::sendto(m_fd, data, bytes, 0,
                                      reinterpret_cast<const sockaddr*>(&peer), peer_len);
        if (sent == static_cast<ssize_t>(bytes)) {
            return true;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

bool UdpSocket::SendTo(const sockaddr_in& peer, const std::uint8_t* data,
                       std::size_t bytes) noexcept {
    sockaddr_storage storage{};
    std::memcpy(&storage, &peer, sizeof(peer));
    return SendTo(storage, sizeof(peer), data, bytes);
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

int UdpSocket::RecvFrom(std::uint8_t* buffer, std::size_t capacity, sockaddr_storage* from,
                        socklen_t* from_len) noexcept {
    if (m_fd < 0 || buffer == nullptr) {
        return -1;
    }
    for (;;) {
        sockaddr_storage peer{};
        socklen_t peer_length = sizeof(peer);
        const ssize_t received = ::recvfrom(m_fd, buffer, capacity, 0,
                                            reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (received >= 0) {
            if (from != nullptr) {
                *from = peer;
            }
            if (from_len != nullptr) {
                *from_len = peer_length;
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

int UdpSocket::RecvFrom(std::uint8_t* buffer, std::size_t capacity, sockaddr_in* from) noexcept {
    sockaddr_storage peer{};
    socklen_t peer_length = sizeof(peer);
    const int received = RecvFrom(buffer, capacity, &peer, &peer_length);
    if (received <= 0) {
        return received;
    }
    if (peer.ss_family != AF_INET) {
        return -1;
    }
    if (from != nullptr) {
        sockaddr_in v4{};
        std::memcpy(&v4, &peer, sizeof(v4));
        *from = v4;
    }
    return received;
}

bool UdpSocket::SamePeer(const sockaddr_storage& a, const sockaddr_storage& b) noexcept {
    if (a.ss_family != b.ss_family) {
        return false;
    }
    if (a.ss_family == AF_INET) {
        sockaddr_in av{};
        sockaddr_in bv{};
        std::memcpy(&av, &a, sizeof(av));
        std::memcpy(&bv, &b, sizeof(bv));
        return av.sin_port == bv.sin_port && av.sin_addr.s_addr == bv.sin_addr.s_addr;
    }
    if (a.ss_family == AF_INET6) {
        sockaddr_in6 av{};
        sockaddr_in6 bv{};
        std::memcpy(&av, &a, sizeof(av));
        std::memcpy(&bv, &b, sizeof(bv));
        return av.sin6_port == bv.sin6_port && av.sin6_scope_id == bv.sin6_scope_id &&
               std::memcmp(&av.sin6_addr, &bv.sin6_addr, sizeof(av.sin6_addr)) == 0;
    }
    return false;
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
        },
        VideoChannel::Config{m_config.video_fec_group_size});
}

Link::~Link() {
    Close();
}

bool Link::Open(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string local_error;
    if (!m_socket.Open(m_config.bind_address, m_config.bind_port, &local_error)) {
        if (error != nullptr) {
            *error = local_error;
        }
        return false;
    }
    // remote（P1）优先于旧字段 peer_address；两者都支持 IPv4/IPv6/域名。
    const std::string& peer_address =
        m_config.remote.empty() ? m_config.peer_address : m_config.remote;
    m_peer_configured = !peer_address.empty() && m_config.peer_port != 0;
    m_is_listener = !m_peer_configured;
    if (m_peer_configured) {
        if (!UdpSocket::Resolve(peer_address, m_config.peer_port, &m_peer, &m_peer_len,
                                &local_error)) {
            m_socket.Close();
            if (error != nullptr) {
                *error = local_error;
            }
            return false;
        }
        m_have_peer = true;
    } else {
        m_peer = sockaddr_storage{};
        m_peer_len = 0;
    }
    const std::uint64_t now = NowMs();
    m_last_rx_ms = now;
    m_last_tx_ms = now;
    m_next_ack_tick_ms = now + m_config.ack_interval_ms;
    m_next_heartbeat_ms = now + m_config.heartbeat_interval_ms;
    m_conn_id = 0;
    m_resumed = false;
    m_resume_epoch = false;
    m_peer_resume_epoch_seen = false;
    m_send_resume_ack = false;
    m_have_retired_peer = false;
    m_retired_until_ms = 0;
    ResetPmtud(now);
    m_socket.ConfigurePathMtu(m_config.enable_pmtud);
    if (!m_config.pre_shared_key.empty()) {
        if (!SetPreSharedKey(m_config.pre_shared_key.data(), m_config.pre_shared_key.size())) {
            m_socket.Close();
            if (error != nullptr) {
                *error = "pre_shared_key must be exactly 32 bytes";
            }
            return false;
        }
    }
    m_state = LinkState::kIdle;
    return true;
}

void Link::Connect() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
    // conn_id 由客户端提议、监听方在 HELLO_ACK 中确认；后续所有帧按它路由。
    if (m_conn_id == 0) {
        m_conn_id = RandomNonZeroU32();
    }
    m_resumed = false;
    m_resume_epoch = false;
    m_have_retired_peer = false;
    if (m_secure_enabled) {
        // 每次连接使用新的 client nonce，避免会话密钥复用；旧 nonce 会被
        // 监听方记入重放缓存，重连不会被误判为重放。
        ResetSecureSession();
        SecureRandomBytes(m_client_nonce, kHandshakeNonceBytes);
        m_handshake_tx_seq = RandomNonZeroU32();
        m_tx_direction = SecureDirection::kClientToServer;
        m_rx_direction = SecureDirection::kServerToClient;
        if (m_config.enable_x25519) {
            // HELLO 前生成临时密钥对；私钥保留到 HELLO_ACK 到达后立即清零。
            // 生成失败时公钥保持全零，对端会按纯 PSK 回退（不会静默错误）。
            (void)GenerateX25519KeyPair();
        } else {
            // 显式纯 PSK：不发公钥，HELLO 退回旧长度。
            ClearX25519Ephemeral();
            std::memset(m_x25519_public, 0, sizeof(m_x25519_public));
        }
    }
    SendHello();
}

void Link::Close() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // 关闭前把服务端会话快照写入缓存，供对端下一次 0-RTT 恢复使用。
    CacheServerSession();
    m_state = LinkState::kClosed;
    m_socket.Close();
    if (m_secure_enabled) {
        ResetSecureSession();
    }
}

int Link::RunOnce(int timeout_ms) {
    // 接收循环单线程：持锁覆盖 poll+分发+Tick，避免与并发 Send 竞争。
    // 发送线程最多被阻塞一个 timeout_ms；测试通常传 1ms，生产可传 0。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_socket.IsOpen()) {
        return 0;
    }
    PollPathMtuErrors();
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
            HandleFrame(m_rx_buffer.data(), static_cast<std::size_t>(received), from, from_len);
            ++processed;
            if (m_state == LinkState::kTimedOut || m_state == LinkState::kClosed) {
                break;
            }
        }
    } else if (ready < 0) {
        ++m_stats.poll_errors;
    }
    PollPathMtuErrors();
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

bool Link::PeerMatches(const sockaddr_storage& from) const noexcept {
    return m_have_peer && UdpSocket::SamePeer(from, m_peer);
}

void Link::HandleHello(const FrameView& view, const sockaddr_storage& from,
                       socklen_t from_len) {
    if (view.payload_bytes < 12 || !IsHelloMagic(view.payload)) {
        ++m_stats.invalid_frames;
        return;
    }
    const std::uint32_t session = ReadU32(view.payload + 4);
    const std::uint32_t proposed_conn_id = ReadU32(view.payload + 8);
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
        m_peer_len = from_len;
        m_have_peer = true;
        m_session_id = session;
        m_conn_id = proposed_conn_id != 0 ? proposed_conn_id : RandomNonZeroU32();
    } else if (session != m_session_id) {
        // 同一地址的新 session：视为对端重启。重置可靠/不可靠/视频通道状态，
        // 否则接收端 expected 会停在新对端永远不会产生的旧 seq 上。
        m_session_id = session;
        m_conn_id = proposed_conn_id != 0 ? proposed_conn_id : RandomNonZeroU32();
        ResetApplicationChannels();
    } else if (m_conn_id == 0) {
        m_conn_id = proposed_conn_id != 0 ? proposed_conn_id : RandomNonZeroU32();
    }

    // 重复 HELLO 也要回 ACK：HELLO_ACK 丢失时客户端会重发 HELLO。
    std::uint8_t payload[12];
    MakeHelloPayload(session, m_conn_id, payload);
    FrameHeader header;
    header.type = FrameType::kHelloAck;
    header.channel = 0;
    header.conn_id = m_conn_id;
    header.seq = session;
    (void)SendControl(header, nullptr, payload, sizeof(payload));
    m_last_rx_ms = NowMs();
    if (m_state != LinkState::kConnected) {
        m_state = LinkState::kConnected;
        ++m_stats.handshakes;
        ResetPmtud(m_last_rx_ms);
        if (on_connected) {
            on_connected();
        }
    }
}

void Link::HandleHelloAck(const FrameView& view, const sockaddr_storage& from) {
    (void)from;
    if (view.payload_bytes < 12 || !IsHelloMagic(view.payload)) {
        ++m_stats.invalid_frames;
        return;
    }
    const std::uint32_t session = ReadU32(view.payload + 4);
    const std::uint32_t accepted_conn_id = ReadU32(view.payload + 8);
    if (session != m_session_id || view.header.seq != m_session_id ||
        accepted_conn_id == 0) {
        ++m_stats.invalid_frames;
        return;
    }
    m_conn_id = accepted_conn_id;
    m_last_rx_ms = NowMs();
    if (m_state == LinkState::kConnecting) {
        m_state = LinkState::kConnected;
        ++m_stats.handshakes;
        ResetPmtud(m_last_rx_ms);
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

void Link::HandleFrame(const std::uint8_t* data, std::size_t bytes,
                        const sockaddr_storage& from, socklen_t from_len) {
    // 加密帧的外层头明文可见（conn_id/wire seq 用于路由与重放窗口），
    // 载荷是内层完整帧的 AEAD 密文；因此先解 16B 头按类型分派。
    FrameHeader outer;
    if (!DecodeHeader(data, bytes, &outer)) {
        ++m_stats.invalid_frames;
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

    // 0-RTT：监听方对象可能已重建（原 session 不在内存里），凭 conn_id
    // 查进程内会话缓存并试解首包；解不开绝不改变现有状态。
    if (m_secure_enabled && m_is_listener && outer.conn_id != 0 &&
        (m_state != LinkState::kConnected || !m_session_keys_ready ||
         outer.conn_id != m_conn_id)) {
        std::vector<std::uint8_t> adopted_inner;
        if (TryAdoptCachedSession(data, bytes, outer, from, from_len, &adopted_inner)) {
            FrameView view;
            if (!ParseFrame(adopted_inner.data(), adopted_inner.size(), &view)) {
                ++m_secure_stats.malformed_frames;
                return;
            }
            if ((view.header.flags & kFlagResume) != 0) {
                NoteResumeEpoch(view.header.flags);
            }
            m_last_rx_ms = NowMs();
            DispatchFrame(view);
            return;
        }
    }

    if (m_state != LinkState::kConnected) {
        ++m_stats.unknown_peer_frames;
        return;
    }
    if (m_secure_enabled && (outer.flags & kFlagEncrypted) == 0) {
        // PSK 模式下所有数据必须加密；先于 conn_id 检查计数，
        // 让注入的明文帧有独立可观测指标。
        ++m_secure_stats.plaintext_rejected;
        return;
    }
    // P2：按 conn_id 路由，而不是源 IP:port。迁移后的新地址也会通过。
    if (!ConnIdMatches(outer)) {
        ++m_stats.conn_id_mismatch_frames;
        return;
    }
    const std::uint64_t now = NowMs();
    if (m_have_retired_peer && now >= m_retired_until_ms) {
        m_have_retired_peer = false;
    }
    if (IsRetiredPeer(from, now)) {
        // 迁移静默期：旧地址的包（即便 conn_id 正确）直接丢弃，
        // 防止旧路径的延迟包/重放包把对端地址拉回去。
        ++m_stats.stale_peer_frames;
        return;
    }
    if (m_secure_enabled) {
        if (!m_session_keys_ready) {
            ++m_secure_stats.plaintext_rejected;
            return;
        }
        if ((outer.flags & kFlagEncrypted) == 0) {
            ++m_secure_stats.plaintext_rejected;
            return;
        }
        std::vector<std::uint8_t> inner;
        OpenStatus status = OpenStatus::kMalformed;
        if (!OpenFrame(m_rx_key, m_rx_direction, &m_data_rx_window, data, bytes, &inner,
                       &status)) {
            switch (status) {
                case OpenStatus::kReplay:
                    ++m_secure_stats.replay_drops;
                    break;
                case OpenStatus::kAuthFailed:
                    ++m_secure_stats.auth_failures;
                    break;
                default:
                    ++m_secure_stats.malformed_frames;
                    break;
            }
            return;
        }
        ++m_secure_stats.packets_opened;
        m_secure_stats.encrypted_bytes_received += bytes;
        FrameView view;
        if (!ParseFrame(inner.data(), inner.size(), &view)) {
            ++m_secure_stats.malformed_frames;
            return;
        }
        // 只有通过 AEAD 认证的帧才允许改变对端地址。
        UpdatePeerAddress(from, from_len);
        NoteResumeEpoch(view.header.flags);
        if ((view.header.flags & kFlagResumeAck) != 0) {
            m_resume_epoch = false;
        }
        m_last_rx_ms = NowMs();
        DispatchFrame(view);
        return;
    }
    if ((outer.flags & kFlagEncrypted) != 0) {
        // 对端启用加密而本端没有 PSK：无法认证，按非法帧丢弃。
        ++m_stats.invalid_frames;
        return;
    }
    FrameView view;
    if (!ParseFrame(data, bytes, &view)) {
        ++m_stats.invalid_frames;
        return;
    }
    UpdatePeerAddress(from, from_len);
    NoteResumeEpoch(view.header.flags);
    if ((view.header.flags & kFlagResumeAck) != 0) {
        m_resume_epoch = false;
    }
    m_last_rx_ms = NowMs();
    DispatchFrame(view);
}

void Link::DispatchFrame(const FrameView& view) {
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
        case FrameType::kMaxData: {
            if (view.payload_bytes != kMaxDataPayloadBytes) {
                ++m_stats.invalid_frames;
                break;
            }
            auto it = m_reliable.find(view.header.channel);
            if (it == m_reliable.end()) {
                // 对端为尚未建立的通道通告窗口：合法但无意义。
                ++m_stats.unknown_peer_frames;
                break;
            }
            it->second->OnMaxData(ReadU64(view.payload));
            break;
        }
        case FrameType::kHello:
        case FrameType::kHelloAck:
            break;  // 握手帧在 HandleFrame 中单独处理
        case FrameType::kPmtudProbe:
            HandlePmtudProbe(view);
            break;
        case FrameType::kPmtudAck:
            HandlePmtudAck(view);
            break;
    }
}

void Link::HandleHelloPacket(const std::uint8_t* data, std::size_t bytes,
                             const FrameHeader& outer, const sockaddr_storage& from,
                             socklen_t from_len) {
    if (!m_is_listener && !(m_peer_configured && PeerMatches(from))) {
        ++m_stats.unknown_peer_frames;
        return;
    }
    if (m_secure_enabled) {
        (void)HandleSecureHello(data, bytes, outer, from, from_len);
        return;
    }
    if ((outer.flags & kFlagEncrypted) != 0) {
        ++m_stats.invalid_frames;
        return;
    }
    FrameView view;
    if (!ParseFrame(data, bytes, &view)) {
        ++m_stats.invalid_frames;
        return;
    }
    HandleHello(view, from, from_len);
}

void Link::HandleHelloAckPacket(const std::uint8_t* data, std::size_t bytes,
                                const FrameHeader& outer, const sockaddr_storage& from) {
    if (m_is_listener || m_state != LinkState::kConnecting || !PeerMatches(from)) {
        ++m_stats.unknown_peer_frames;
        return;
    }
    if (m_secure_enabled) {
        (void)HandleSecureHelloAck(data, bytes, outer, from);
        return;
    }
    if ((outer.flags & kFlagEncrypted) != 0) {
        ++m_stats.invalid_frames;
        return;
    }
    FrameView view;
    if (!ParseFrame(data, bytes, &view)) {
        ++m_stats.invalid_frames;
        return;
    }
    HandleHelloAck(view, from);
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
        },
        m_config.reliable);
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
    // 统一在这里写入 conn_id / kFlagResume：所有通道只负责自己的载荷，
    // 不需要知道链路层连接标识。帧很小（<=1.5KB），复制成本可忽略。
    std::vector<std::uint8_t> stamped;
    if ((m_conn_id != 0 || m_resume_epoch || m_send_resume_ack) && bytes >= kFrameHeaderBytes) {
        FrameHeader header;
        if (DecodeHeader(data, bytes, &header)) {
            bool changed = false;
            if (m_conn_id != 0 && header.conn_id != m_conn_id) {
                header.conn_id = m_conn_id;
                changed = true;
            }
            if (m_resume_epoch && (header.flags & kFlagResume) == 0) {
                header.flags = static_cast<std::uint8_t>(header.flags | kFlagResume);
                changed = true;
            }
            if (m_send_resume_ack && (header.flags & kFlagResumeAck) == 0) {
                header.flags = static_cast<std::uint8_t>(header.flags | kFlagResumeAck);
                changed = true;
            }
            if (changed) {
                stamped.assign(data, data + bytes);
                EncodeHeader(header, stamped.data());
                data = stamped.data();
                bytes = stamped.size();
            }
        }
    }
    if (m_secure_enabled) {
        return SealAndSend(data, bytes);
    }
    return SendRawPacket(data, bytes);
}

bool Link::SendRawPacket(const std::uint8_t* data, std::size_t bytes) {
    if (!m_socket.IsOpen() || !m_have_peer) {
        ++m_stats.send_errors;
        return false;
    }
    if (!m_socket.SendTo(m_peer, m_peer_len, data, bytes)) {
        ++m_stats.send_errors;
        if (m_config.enable_pmtud && errno == EMSGSIZE) {
            // DF 生效时内核会直接拒绝超路径 MTU 的包；按 UDP 载荷回退一档。
            const std::uint32_t overhead = m_peer.ss_family == AF_INET6 ? 48U : 28U;
            NotifyPathMtuError(bytes > overhead ? static_cast<std::uint32_t>(bytes) - overhead
                                               : kMinPathMtu);
        }
        return false;
    }
    ++m_stats.frames_sent;
    m_stats.bytes_sent += bytes;
    m_last_tx_ms = NowMs();
    return true;
}

bool Link::SendMaxDataFrame(std::uint8_t channel, std::uint64_t limit) {
    std::uint8_t payload[kMaxDataPayloadBytes];
    WriteU64(payload, limit);
    FrameHeader header;
    header.type = FrameType::kMaxData;
    header.channel = channel;
    header.seq = 0;
    return SendControl(header, nullptr, payload, sizeof(payload));
}

bool Link::SealAndSend(const std::uint8_t* frame, std::size_t bytes) {
    if (!m_session_keys_ready) {
        ++m_stats.send_errors;
        return false;
    }
    std::uint32_t seq = m_tx_wire_seq + 1;
    if (seq == 0) {
        seq = 1;  // wire seq 0 保留；2^32 回绕在实际连接中不会发生。
    }
    std::vector<std::uint8_t> sealed;
    if (!SealFrame(m_tx_key, m_tx_direction, seq, frame, bytes, &sealed)) {
        ++m_stats.send_errors;
        return false;
    }
    // 先推进 seq 再发送：即使 UDP 发送失败，也绝不能复用同一 (key, nonce)
    // 加密另一份明文（ChaCha20 的 nonce 重用会破坏机密性）。
    m_tx_wire_seq = seq;
    if (!SendRawPacket(sealed.data(), sealed.size())) {
        return false;
    }
    ++m_secure_stats.packets_sealed;
    m_secure_stats.encrypted_bytes_sent += sealed.size();
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
    if (m_secure_enabled) {
        // 重传复用同一份缓存密文（同一 wire seq）：监听方按「当前 nonce
        // 的重复 HELLO」幂等重发 HELLO_ACK，不会把重传误判为重放攻击。
        (void)SendSecureHello();
    } else {
        std::uint8_t payload[12];
        MakeHelloPayload(m_session_id, m_conn_id, payload);
        FrameHeader header;
        header.type = FrameType::kHello;
        header.channel = 0;
        header.conn_id = m_conn_id;
        header.seq = m_session_id;
        (void)SendControl(header, nullptr, payload, sizeof(payload));
    }
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
        // MAX_DATA：窗口推进后立即通告；丢失时按 refresh 周期幂等重发。
        std::uint64_t limit = 0;
        if (entry.second->PendingMaxData(&limit, now_ms) &&
            SendMaxDataFrame(entry.first, limit)) {
            entry.second->OnMaxDataSent(limit, now_ms);
        }
    }
    m_video->OnTimer(now_ms);
    MaybeReportBandwidth(now_ms);
    MaybeProbePmtud(now_ms);

    if (now_ms >= m_last_rx_ms && now_ms - m_last_rx_ms > m_config.peer_timeout_ms) {
        Fail("peer timeout");
    }
}

void Link::Fail(const char* reason) {
    if (m_state == LinkState::kTimedOut || m_state == LinkState::kClosed) {
        return;
    }
    // 监听方超时后仍保留会话快照，便于重启/重建后接受 0-RTT。
    CacheServerSession();
    // 握手超时后临时私钥不再有任何用途；不要留在对象内存里。
    ClearX25519Ephemeral();
    m_state = LinkState::kTimedOut;
    ++m_stats.timeouts;
    m_socket.Close();
    if (on_disconnected) {
        on_disconnected(reason);
    }
}

// ---------------------------------------------------------------------------
// P2：连接迁移 / 路径 MTU / 0-RTT
// ---------------------------------------------------------------------------

std::uint32_t Link::conn_id() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_conn_id;
}

bool Link::ConnIdMatches(const FrameHeader& header) const noexcept {
    return m_conn_id != 0 && header.conn_id == m_conn_id;
}

bool Link::IsRetiredPeer(const sockaddr_storage& from, std::uint64_t now_ms) const noexcept {
    return m_have_retired_peer && now_ms < m_retired_until_ms &&
           UdpSocket::SamePeer(from, m_retired_peer);
}

void Link::UpdatePeerAddress(const sockaddr_storage& from, socklen_t from_len) {
    if (!m_have_peer) {
        m_peer = from;
        m_peer_len = from_len;
        m_have_peer = true;
        return;
    }
    if (UdpSocket::SamePeer(from, m_peer)) {
        return;
    }
    // 对端换了本地地址：记录旧地址并进入静默期，随后按新地址发送。
    m_retired_peer = m_peer;
    m_retired_peer_len = m_peer_len;
    m_have_retired_peer = true;
    m_retired_until_ms = NowMs() + m_config.migration_grace_ms;
    m_peer = from;
    m_peer_len = from_len;
    ++m_stats.peer_migrations;
    // 新地址意味着可能是新一次 0-RTT 恢复：允许再次处理 kFlagResume。
    m_peer_resume_epoch_seen = false;
    m_send_resume_ack = false;
    // 新路径的 MTU 未知，重新从保守值探测。
    ResetPmtud(NowMs());
}

void Link::ResetApplicationChannels() {
    m_reliable.clear();
    m_unreliable.clear();
    m_relation.Reset();
    // 视频重组/FEC 状态绑定旧会话，不能跨恢复复用。
    m_video = std::make_unique<VideoChannel>(
        [this](const std::uint8_t* data, std::size_t bytes) { return SendBuiltFrame(data, bytes); },
        [this](std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes) {
            if (on_video) {
                on_video(kChannelVideo, frame_id, key, data, bytes);
            }
        },
        VideoChannel::Config{m_config.video_fec_group_size});
}

void Link::NoteResumeEpoch(std::uint8_t flags) {
    if ((flags & kFlagResume) == 0) {
        return;
    }
    if (!m_peer_resume_epoch_seen) {
        // 0-RTT 首帧：旧应用的 seq/重组状态全部作废，只保留 wire seq 与密钥。
        m_peer_resume_epoch_seen = true;
        m_send_resume_ack = true;
        ResetApplicationChannels();
        ++m_secure_stats.resumptions;
    }
    ++m_secure_stats.zero_rtt_frames;
}

std::uint32_t Link::PathMtu() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_path_mtu;
}

void Link::NotifyPathMtuError(std::uint32_t reported_mtu) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (reported_mtu == 0) {
        return;
    }
    const std::uint32_t bounded =
        std::min(std::max(reported_mtu, kMinPathMtu), m_config.pmtud_max_mtu);
    // ICMP 只会让 MTU 下降；更高的报告不推翻已探测到的值。
    if (bounded >= m_path_mtu) {
        return;
    }
    m_path_mtu = bounded;
    m_pmtud_awaiting_ack = false;
    m_pmtud_attempts = 0;
    m_pmtud_done = true;
    ++m_stats.pmtud_icmp_errors;
}

void Link::PollPathMtuErrors() {
    if (!m_config.enable_pmtud) {
        return;
    }
    for (int i = 0; i < 16; ++i) {
        std::uint32_t reported = 0;
        const int result = m_socket.ReceiveMtuError(&reported);
        if (result <= 0) {
            break;
        }
        NotifyPathMtuError(reported);
    }
}

void Link::ResetPmtud(std::uint64_t now_ms) {
    m_path_mtu = 1200;  // 无探测/无 ICMP 时的保守值。
    m_pmtud_done = !m_config.enable_pmtud;
    m_pmtud_awaiting_ack = false;
    m_pmtud_target = 1200;
    m_pmtud_attempts = 0;
    m_pmtud_token = RandomNonZeroU32();
    m_pmtud_next_ms = now_ms + m_config.pmtud_probe_interval_ms;
    m_pmtud_deadline_ms = 0;
}

bool Link::SendPmtudProbe() {
    if (!m_config.enable_pmtud || !m_socket.IsOpen() || !m_have_peer) {
        return false;
    }
    const std::size_t overhead = m_secure_enabled ? kSecureOverheadBytes : 0;
    if (m_pmtud_target <= overhead + kFrameHeaderBytes + kPmtudPayloadBytes) {
        return false;
    }
    const std::size_t frame_bytes = m_pmtud_target - overhead;
    std::vector<std::uint8_t> payload(frame_bytes - kFrameHeaderBytes, 0);
    m_pmtud_token = RandomNonZeroU32();
    WriteU32(payload.data(), m_pmtud_token);
    WriteU32(payload.data() + 4, m_pmtud_target);

    FrameHeader header;
    header.type = FrameType::kPmtudProbe;
    header.channel = 0;
    header.conn_id = m_conn_id;
    header.seq = m_pmtud_token;
    const std::vector<std::uint8_t> frame =
        BuildFrame(header, nullptr, payload.data(), payload.size());
    if (frame.empty()) {
        ++m_stats.send_errors;
        return false;
    }
    if (!SendBuiltFrame(frame.data(), frame.size())) {
        return false;
    }
    ++m_stats.pmtud_probes_sent;
    m_pmtud_awaiting_ack = true;
    m_pmtud_deadline_ms = NowMs() + m_config.pmtud_probe_timeout_ms;
    return true;
}

void Link::MaybeProbePmtud(std::uint64_t now_ms) {
    if (!m_config.enable_pmtud || m_pmtud_done || m_state != LinkState::kConnected) {
        return;
    }
    if (m_pmtud_awaiting_ack) {
        if (now_ms < m_pmtud_deadline_ms) {
            return;
        }
        m_pmtud_awaiting_ack = false;
        if (++m_pmtud_attempts >= m_config.pmtud_probe_attempts) {
            // 无 ICMP 且探针无响应：保持当前（保守）MTU，停止升档。
            ++m_stats.pmtud_blackholes;
            m_pmtud_done = true;
            return;
        }
        m_pmtud_next_ms = now_ms;
    }
    if (now_ms < m_pmtud_next_ms) {
        return;
    }
    if (m_pmtud_target > m_config.pmtud_max_mtu) {
        m_pmtud_done = true;
        return;
    }
    if (!SendPmtudProbe()) {
        m_pmtud_next_ms = now_ms + m_config.pmtud_probe_interval_ms;
    }
}

void Link::HandlePmtudProbe(const FrameView& view) {
    if (view.payload_bytes < kPmtudPayloadBytes) {
        ++m_stats.invalid_frames;
        return;
    }
    std::uint8_t payload[kPmtudPayloadBytes];
    std::memcpy(payload, view.payload, kPmtudPayloadBytes);
    FrameHeader header;
    header.type = FrameType::kPmtudAck;
    header.channel = 0;
    header.conn_id = m_conn_id;
    header.seq = 0;
    if (SendControl(header, nullptr, payload, sizeof(payload))) {
        ++m_stats.pmtud_probes_received;
    }
}

void Link::HandlePmtudAck(const FrameView& view) {
    if (view.payload_bytes < kPmtudPayloadBytes) {
        ++m_stats.invalid_frames;
        return;
    }
    const std::uint32_t token = ReadU32(view.payload);
    const std::uint32_t target = ReadU32(view.payload + 4);
    if (token != m_pmtud_token || target != m_pmtud_target) {
        return;  // 迟到的旧探针 ACK。
    }
    ++m_stats.pmtud_acks_received;
    m_pmtud_awaiting_ack = false;
    m_pmtud_attempts = 0;
    m_path_mtu = std::max(m_path_mtu, std::min(target, m_config.pmtud_max_mtu));
    if (m_pmtud_target >= m_config.pmtud_max_mtu) {
        m_pmtud_done = true;
        return;
    }
    m_pmtud_target =
        std::min(m_config.pmtud_max_mtu, m_pmtud_target + kPmtudStepBytes);
    m_pmtud_next_ms = NowMs() + m_config.pmtud_probe_interval_ms;
}

bool Link::Migrate(const std::string& new_address, std::uint16_t new_port, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_socket.IsOpen()) {
        if (error != nullptr) {
            *error = "socket not open";
        }
        return false;
    }
    UdpSocket replacement;
    if (!replacement.Open(new_address, new_port, error)) {
        return false;
    }
    if (m_have_peer && replacement.family() != m_peer.ss_family) {
        if (error != nullptr) {
            *error = "migrate requires the same address family as peer";
        }
        return false;
    }
    replacement.ConfigurePathMtu(m_config.enable_pmtud);
    // 先成功打开新 socket，再关闭旧 socket：失败时旧链路仍可用。
    m_socket.Close();
    m_socket = std::move(replacement);
    m_last_tx_ms = NowMs();
    ResetPmtud(m_last_tx_ms);
    return true;
}

std::vector<std::uint8_t> Link::ExportSessionTicket() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // 0-RTT 票据按静态 PSK 重派生会话密钥；X25519 会话的密钥混入了
    // 临时共享密钥，无法从票据安全恢复，因此直接拒绝导出（不静默降级）。
    if (!m_secure_enabled || !m_session_keys_ready || m_conn_id == 0 || m_session_id == 0 ||
        m_config.session_ticket_lifetime_ms == 0 || !IsZeroX25519Key(m_x25519_public)) {
        return {};
    }
    SessionTicket ticket;
    ticket.conn_id = m_conn_id;
    ticket.session_id = m_session_id;
    const std::uint64_t now = NowMs();
    ticket.issued_ms = now;
    ticket.expires_ms = now + m_config.session_ticket_lifetime_ms;
    std::memcpy(ticket.client_nonce, m_client_nonce, kHandshakeNonceBytes);
    std::memcpy(ticket.server_nonce, m_server_nonce, kHandshakeNonceBytes);
    std::uint32_t tx_next = m_tx_wire_seq + 1;
    if (tx_next == 0) {
        tx_next = 1;
    }
    if (m_is_listener) {
        ticket.client_next_wire_seq = NextWireSeq(m_data_rx_window);
        ticket.server_next_wire_seq = tx_next;
    } else {
        ticket.client_next_wire_seq = tx_next;
        ticket.server_next_wire_seq = NextWireSeq(m_data_rx_window);
    }
    std::vector<std::uint8_t> ticket_bytes;
    if (!EncodeSessionTicket(ticket, m_psk, &ticket_bytes)) {
        return {};
    }
    ++m_secure_stats.tickets_exported;
    if (m_is_listener) {
        CacheServerSession();
    }
    return ticket_bytes;
}

bool Link::Resume(const std::vector<std::uint8_t>& ticket_bytes) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_secure_enabled) {
        ++m_secure_stats.tickets_rejected;
        return false;
    }
    SessionTicket ticket;
    if (!DecodeSessionTicket(ticket_bytes.data(), ticket_bytes.size(), m_psk, &ticket)) {
        ++m_secure_stats.tickets_rejected;
        return false;
    }
    const std::uint64_t now = NowMs();
    if (ticket.expires_ms <= now) {
        ++m_secure_stats.tickets_rejected;
        return false;
    }
    if (!m_socket.IsOpen() || (!m_is_listener && !m_peer_configured)) {
        ++m_secure_stats.tickets_rejected;
        return false;
    }
    if (m_state == LinkState::kConnected || m_state == LinkState::kConnecting) {
        ++m_secure_stats.tickets_rejected;
        return false;
    }

    m_conn_id = ticket.conn_id;
    m_session_id = ticket.session_id;
    std::memcpy(m_client_nonce, ticket.client_nonce, kHandshakeNonceBytes);
    std::memcpy(m_server_nonce, ticket.server_nonce, kHandshakeNonceBytes);
    if (m_is_listener) {
        m_tx_direction = SecureDirection::kServerToClient;
        m_rx_direction = SecureDirection::kClientToServer;
        m_tx_wire_seq = ticket.server_next_wire_seq - 1;
        m_data_rx_window.InitializeBefore(ticket.client_next_wire_seq);
        m_have_peer = false;
        m_peer = sockaddr_storage{};
        m_peer_len = 0;
    } else {
        m_tx_direction = SecureDirection::kClientToServer;
        m_rx_direction = SecureDirection::kServerToClient;
        m_tx_wire_seq = ticket.client_next_wire_seq - 1;
        m_data_rx_window.InitializeBefore(ticket.server_next_wire_seq);
    }
    DeriveSessionKey(m_psk, m_client_nonce, m_server_nonce, m_tx_direction, m_tx_key);
    DeriveSessionKey(m_psk, m_client_nonce, m_server_nonce, m_rx_direction, m_rx_key);
    m_session_keys_ready = true;
    m_state = LinkState::kConnected;
    m_resumed = true;
    m_resume_epoch = !m_is_listener;
    m_peer_resume_epoch_seen = false;
    m_send_resume_ack = false;
    m_have_retired_peer = false;
    m_last_rx_ms = now;
    m_next_ack_tick_ms = now + m_config.ack_interval_ms;
    m_next_heartbeat_ms = now + m_config.heartbeat_interval_ms;
    ResetApplicationChannels();
    ResetPmtud(now);
    ++m_secure_stats.resumptions;
    if (on_connected) {
        on_connected();
    }
    return true;
}

void Link::CacheServerSession() {
    // 与 ExportSessionTicket 同一策略：只缓存纯 PSK 会话，避免
    // 0-RTT 恢复把 X25519 会话降级成静态 PSK。
    if (!m_is_listener || !m_secure_enabled || !m_session_keys_ready || m_conn_id == 0 ||
        m_session_id == 0 || m_config.session_ticket_lifetime_ms == 0 ||
        !IsZeroX25519Key(m_x25519_public)) {
        return;
    }
    CachedSession entry;
    entry.conn_id = m_conn_id;
    entry.session_id = m_session_id;
    entry.local_port = m_socket.local_port();
    DerivePskFingerprint(m_psk, entry.psk_fingerprint);
    std::memcpy(entry.client_nonce, m_client_nonce, kHandshakeNonceBytes);
    std::memcpy(entry.server_nonce, m_server_nonce, kHandshakeNonceBytes);
    std::uint32_t next = m_tx_wire_seq + 1;
    if (next == 0) {
        next = 1;
    }
    entry.server_next_wire_seq = next;
    entry.client_rx_window = m_data_rx_window;
    entry.expires_ms = NowMs() + m_config.session_ticket_lifetime_ms;
    std::lock_guard<std::mutex> cache_lock(g_session_cache_mutex);
    PurgeExpiredSessions(NowMs());
    g_session_cache[entry.conn_id] = entry;
}

bool Link::TryAdoptCachedSession(const std::uint8_t* packet, std::size_t bytes,
                                 const FrameHeader& outer, const sockaddr_storage& from,
                                 socklen_t from_len, std::vector<std::uint8_t>* inner) {
    if (!m_is_listener || !m_secure_enabled || outer.conn_id == 0 || inner == nullptr) {
        return false;
    }
    CachedSession entry;
    {
        std::lock_guard<std::mutex> cache_lock(g_session_cache_mutex);
        PurgeExpiredSessions(NowMs());
        auto it = g_session_cache.find(outer.conn_id);
        if (it == g_session_cache.end() || it->second.expires_ms <= NowMs() ||
            it->second.local_port != m_socket.local_port()) {
            return false;
        }
        entry = it->second;
    }
    std::uint8_t fingerprint[8];
    DerivePskFingerprint(m_psk, fingerprint);
    if (std::memcmp(fingerprint, entry.psk_fingerprint, sizeof(fingerprint)) != 0) {
        return false;
    }
    std::uint8_t rx_key[kChaCha20KeyBytes];
    std::uint8_t tx_key[kChaCha20KeyBytes];
    DeriveSessionKey(m_psk, entry.client_nonce, entry.server_nonce,
                     SecureDirection::kClientToServer, rx_key);
    DeriveSessionKey(m_psk, entry.client_nonce, entry.server_nonce,
                     SecureDirection::kServerToClient, tx_key);
    ReplayWindow trial_window = entry.client_rx_window;
    OpenStatus status = OpenStatus::kMalformed;
    if (!OpenFrame(rx_key, SecureDirection::kClientToServer, &trial_window, packet, bytes, inner,
                   &status)) {
        if (status == OpenStatus::kAuthFailed) {
            ++m_secure_stats.auth_failures;
        } else if (status == OpenStatus::kReplay) {
            ++m_secure_stats.replay_drops;
        } else {
            ++m_secure_stats.malformed_frames;
        }
        return false;
    }
    // 认证成功后才提交状态，避免伪造包劫持会话。
    m_conn_id = entry.conn_id;
    m_session_id = entry.session_id;
    std::memcpy(m_client_nonce, entry.client_nonce, kHandshakeNonceBytes);
    std::memcpy(m_server_nonce, entry.server_nonce, kHandshakeNonceBytes);
    std::memcpy(m_rx_key, rx_key, kChaCha20KeyBytes);
    std::memcpy(m_tx_key, tx_key, kChaCha20KeyBytes);
    m_tx_direction = SecureDirection::kServerToClient;
    m_rx_direction = SecureDirection::kClientToServer;
    m_tx_wire_seq = entry.server_next_wire_seq > 0 ? entry.server_next_wire_seq - 1 : 0;
    m_data_rx_window = trial_window;
    m_session_keys_ready = true;
    m_state = LinkState::kConnected;
    m_resumed = true;
    m_resume_epoch = false;
    m_peer_resume_epoch_seen = true;  // 本函数已重置过通道状态。
    m_send_resume_ack = true;
    m_have_retired_peer = false;
    m_last_rx_ms = NowMs();
    m_next_ack_tick_ms = m_last_rx_ms + m_config.ack_interval_ms;
    m_next_heartbeat_ms = m_last_rx_ms + m_config.heartbeat_interval_ms;
    UpdatePeerAddress(from, from_len);
    ResetApplicationChannels();
    ResetPmtud(m_last_rx_ms);
    ++m_secure_stats.resumptions;
    {
        std::lock_guard<std::mutex> cache_lock(g_session_cache_mutex);
        auto it = g_session_cache.find(entry.conn_id);
        if (it != g_session_cache.end()) {
            it->second.client_rx_window = trial_window;
        }
    }
    if (on_connected) {
        on_connected();
    }
    return true;
}

void Link::ClearSessionCache() {
    std::lock_guard<std::mutex> cache_lock(g_session_cache_mutex);
    g_session_cache.clear();
}

SendStatus Link::TrySendStream(std::uint8_t channel, const void* data, std::size_t bytes,
                               std::uint32_t dep_seq) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_state != LinkState::kConnected) {
        return SendStatus::kError;
    }
    ReliableChannel* target = GetOrCreateReliable(channel);
    if (target == nullptr) {
        return SendStatus::kError;
    }
    return target->TrySend(static_cast<const std::uint8_t*>(data), bytes, dep_seq);
}

bool Link::SendStream(std::uint8_t channel, const void* data, std::size_t bytes,
                      std::uint32_t dep_seq) {
    return TrySendStream(channel, data, bytes, dep_seq) == SendStatus::kOk;
}

bool Link::ConcurrentSend(std::uint8_t channel, const void* data, std::size_t bytes,
                          std::uint32_t dep_seq) {
    // 显式多线程入口：与 SendStream 共用同一把锁，语义完全一致。
    return SendStream(channel, data, bytes, dep_seq);
}

bool Link::SendDatagram(std::uint8_t channel, const void* data, std::size_t bytes,
                        std::uint32_t dep_seq) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_state != LinkState::kConnected) {
        return false;
    }
    UnreliableChannel* target = GetOrCreateUnreliable(channel);
    if (target == nullptr) {
        return false;
    }
    return target->Send(static_cast<const std::uint8_t*>(data), bytes, dep_seq);
}

bool Link::SendVideoFrame(std::uint32_t frame_id, bool key, const void* data,
                           std::size_t bytes) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_state != LinkState::kConnected) {
        return false;
    }
    return m_video->SendFrame(frame_id, key, static_cast<const std::uint8_t*>(data), bytes);
}

bool Link::SendBarrier(std::uint32_t barrier_seq) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_reliable.find(channel);
    if (it == m_reliable.end()) {
        return {};
    }
    return it->second->stats();
}

UnreliableChannel::Stats Link::unreliable_stats(std::uint8_t channel) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_unreliable.find(channel);
    if (it == m_unreliable.end()) {
        return {};
    }
    return it->second->stats();
}

VideoChannel::Stats Link::video_stats() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_video->stats();
}

CongestionStats Link::congestion_stats(std::uint8_t channel) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_reliable.find(channel);
    if (it == m_reliable.end()) {
        return {};
    }
    return it->second->congestion_stats();
}

LinkStats Link::StatsUnlocked() const {
    LinkStats snapshot = m_stats;
    for (const auto& entry : m_reliable) {
        const ReliableChannel& channel = *entry.second;
        const ReliableChannel::Stats reliable = channel.stats();
        const CongestionStats congestion = channel.congestion_stats();
        snapshot.rtt_ms = std::max(snapshot.rtt_ms, reliable.last_rtt_ms);
        snapshot.srtt_ms = std::max(snapshot.srtt_ms, reliable.srtt_ms);
        snapshot.rttvar_ms = std::max(snapshot.rttvar_ms, reliable.rttvar_ms);
        snapshot.pto_ms = std::max(snapshot.pto_ms, reliable.pto_ms);
        snapshot.cwnd_packets += congestion.cwnd_packets;
        snapshot.ssthresh_packets += congestion.ssthresh_packets;
        snapshot.inflight += reliable.inflight;
        snapshot.retransmits += reliable.retransmits;
        snapshot.loss_events += reliable.loss_events;
        snapshot.flow_control_blocked += reliable.flow_control_blocked;
        snapshot.max_data_sent += reliable.max_data_sent;
        snapshot.max_data_received += reliable.max_data_received;
    }
    snapshot.bandwidth_bps = BandwidthEstimateUnlocked();
    snapshot.loss_rate = LossRateUnlocked();
    return snapshot;
}

LinkStats Link::Stats() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return StatsUnlocked();
}

std::string Link::StatsJson() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return LinkStatsToJson(StatsUnlocked());
}

std::uint64_t Link::BandwidthEstimateUnlocked() const {
    std::uint64_t total_bps = 0;
    for (const auto& entry : m_reliable) {
        total_bps += entry.second->bandwidth_stats().delivery_rate_bps;
    }
    return total_bps;
}

std::uint64_t Link::BandwidthEstimate() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return BandwidthEstimateUnlocked();
}

double Link::LossRateUnlocked() const {
    std::uint64_t acked = 0;
    std::uint64_t lost = 0;
    for (const auto& entry : m_reliable) {
        const BandwidthStats stats = entry.second->bandwidth_stats();
        acked += stats.acked_packets;
        lost += stats.lost_packets;
    }
    const std::uint64_t total = acked + lost;
    if (total == 0) {
        return 0.0;
    }
    return static_cast<double>(lost) / static_cast<double>(total);
}

double Link::LossRate() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return LossRateUnlocked();
}

void Link::MaybeReportBandwidth(std::uint64_t now_ms) {
    if (!on_bandwidth) {
        return;
    }
    if (m_last_abr_ms != 0 && now_ms >= m_last_abr_ms &&
        now_ms - m_last_abr_ms < m_config.abr_interval_ms) {
        return;
    }
    m_last_abr_ms = now_ms;
    const std::uint64_t bitrate = BandwidthEstimateUnlocked();
    if (bitrate == 0) {
        return;
    }
    // 只有变化超过 10% 才回调，避免 ABR 在每个 tick 抖动。
    if (m_last_reported_bps != 0 && bitrate > m_last_reported_bps * 9 / 10 &&
        bitrate < m_last_reported_bps * 11 / 10) {
        return;
    }
    m_last_reported_bps = bitrate;
    on_bandwidth(bitrate);
}

// ---------------------------------------------------------------------------
// PSK 认证加密
// ---------------------------------------------------------------------------

bool Link::SetPreSharedKey(const std::uint8_t* key, std::size_t bytes) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (key == nullptr || bytes != kPskBytes) {
        return false;
    }
    std::memcpy(m_psk, key, kPskBytes);
    m_secure_enabled = true;
    ResetSecureSession();
    // 跨连接保留已见 client nonce：对端重启会用新 nonce，旧 HELLO 重放
    // 在时间戳窗口内也会被识别并拒绝。
    m_seen_client_nonces.clear();
    return true;
}

void Link::ResetSecureSession() {
    m_session_keys_ready = false;
    m_data_rx_window.Reset();
    m_handshake_rx_window.Reset();
    m_tx_wire_seq = 0;
    m_handshake_tx_seq = 1;
    m_cached_hello.clear();
    m_cached_hello_ack.clear();
    m_resume_epoch = false;
    m_resumed = false;
    m_peer_resume_epoch_seen = false;
    m_send_resume_ack = false;
    std::memset(m_client_nonce, 0, sizeof(m_client_nonce));
    std::memset(m_server_nonce, 0, sizeof(m_server_nonce));
    std::memset(m_tx_key, 0, sizeof(m_tx_key));
    std::memset(m_rx_key, 0, sizeof(m_rx_key));
    ClearX25519Ephemeral();
    std::memset(m_x25519_public, 0, sizeof(m_x25519_public));
}

bool Link::TimestampAcceptable(std::uint64_t timestamp_ms) const noexcept {
    if (m_config.handshake_replay_window_ms == 0) {
        return true;
    }
    const std::uint64_t now = WallClockMs();
    const std::uint64_t delta =
        now >= timestamp_ms ? now - timestamp_ms : timestamp_ms - now;
    return delta <= static_cast<std::uint64_t>(m_config.handshake_replay_window_ms);
}

bool Link::IsKnownClientNonce(const std::uint8_t* nonce) const noexcept {
    for (const auto& seen : m_seen_client_nonces) {
        if (std::memcmp(seen.data(), nonce, kHandshakeNonceBytes) == 0) {
            return true;
        }
    }
    return false;
}

void Link::RememberClientNonce(const std::uint8_t* nonce) {
    if (m_seen_client_nonces.size() >= kMaxRememberedClientNonces) {
        m_seen_client_nonces.erase(m_seen_client_nonces.begin());
    }
    std::array<std::uint8_t, kHandshakeNonceBytes> entry{};
    std::memcpy(entry.data(), nonce, kHandshakeNonceBytes);
    m_seen_client_nonces.push_back(entry);
}

bool Link::SendSecureHello() {
    if (m_cached_hello.empty()) {
        HelloBody body;
        body.session = m_session_id;
        std::memcpy(body.nonce, m_client_nonce, kHandshakeNonceBytes);
        std::memcpy(body.x25519_public, m_x25519_public, kX25519KeyBytes);
        body.timestamp_ms = WallClockMs();
        std::uint8_t raw[kHelloBodyBytes];
        const std::size_t raw_bytes =
            m_config.enable_x25519 ? kHelloBodyBytes : kLegacyHelloBodyBytes;
        if (!EncodeHelloBody(body, raw, raw_bytes)) {
            return false;
        }
        if (!SealHandshakeFrame(m_psk, SecureDirection::kClientToServer,
                                static_cast<std::uint8_t>(FrameType::kHello), m_session_id,
                                m_conn_id, m_handshake_tx_seq, raw, raw_bytes,
                                &m_cached_hello)) {
            return false;
        }
    }
    return SendRawPacket(m_cached_hello.data(), m_cached_hello.size());
}

bool Link::SendSecureHelloAck() {
    HelloAckBody body;
    body.session = m_session_id;
    std::memcpy(body.client_nonce, m_client_nonce, kHandshakeNonceBytes);
    std::memcpy(body.server_nonce, m_server_nonce, kHandshakeNonceBytes);
    std::memcpy(body.x25519_public, m_x25519_public, kX25519KeyBytes);
    body.timestamp_ms = WallClockMs();
    std::uint8_t raw[kHelloAckBodyBytes];
    // 回退纯 PSK 时用旧 52B 布局：既兼容旧对端，也让线格式与密钥模式一致。
    const std::size_t raw_bytes =
        IsZeroX25519Key(m_x25519_public) ? kLegacyHelloAckBodyBytes : kHelloAckBodyBytes;
    if (!EncodeHelloAckBody(body, raw, raw_bytes)) {
        return false;
    }
    if (!SealHandshakeFrame(m_psk, SecureDirection::kServerToClient,
                            static_cast<std::uint8_t>(FrameType::kHelloAck), m_session_id,
                            m_conn_id, m_handshake_tx_seq, raw, raw_bytes,
                            &m_cached_hello_ack)) {
        return false;
    }
    return SendRawPacket(m_cached_hello_ack.data(), m_cached_hello_ack.size());
}

bool Link::GenerateX25519KeyPair() {
    ClearX25519Ephemeral();
    X25519GeneratePrivateKey(m_x25519_private);
    if (!X25519PublicKeyFromPrivate(m_x25519_private, m_x25519_public)) {
        ClearX25519Ephemeral();
        std::memset(m_x25519_public, 0, sizeof(m_x25519_public));
        return false;
    }
    m_x25519_private_valid = true;
    return true;
}

void Link::ClearX25519Ephemeral() {
    SecureZero(m_x25519_private, sizeof(m_x25519_private));
    m_x25519_private_valid = false;
}

bool Link::DeriveX25519SessionKeys(const std::uint8_t* peer_public) {
    if (!m_x25519_private_valid || peer_public == nullptr) {
        return false;
    }
    std::uint8_t shared[kX25519KeyBytes];
    if (!X25519ComputeSharedSecret(m_x25519_private, peer_public, shared)) {
        // 低阶公钥：派生失败，不能退回 PSK（否则等于允许降级攻击）。
        SecureZero(shared, sizeof(shared));
        ClearX25519Ephemeral();
        return false;
    }
    DeriveSessionKeyWithX25519(m_psk, shared, m_client_nonce, m_server_nonce,
                               m_tx_direction, m_tx_key);
    DeriveSessionKeyWithX25519(m_psk, shared, m_client_nonce, m_server_nonce,
                               m_rx_direction, m_rx_key);
    SecureZero(shared, sizeof(shared));
    ClearX25519Ephemeral();
    return true;
}

bool Link::HandleSecureHello(const std::uint8_t* data, std::size_t bytes,
                             const FrameHeader& outer, const sockaddr_storage& from,
                             socklen_t from_len) {
    if (!m_is_listener && !(m_peer_configured && PeerMatches(from))) {
        ++m_stats.unknown_peer_frames;
        return false;
    }
    std::uint8_t inner_type = 0;
    std::uint32_t session = 0;
    std::uint32_t handshake_conn_id = 0;
    std::vector<std::uint8_t> body;
    OpenStatus status = OpenStatus::kMalformed;
    if (!OpenHandshakeFrame(m_psk, SecureDirection::kClientToServer, data, bytes, &inner_type,
                            &session, &handshake_conn_id, &body, &status)) {
        if (status == OpenStatus::kAuthFailed) {
            ++m_secure_stats.auth_failures;
        } else {
            ++m_secure_stats.malformed_frames;
        }
        return false;
    }
    if (inner_type != static_cast<std::uint8_t>(FrameType::kHello)) {
        ++m_secure_stats.malformed_frames;
        return false;
    }
    HelloBody hello;
    if (!DecodeHelloBody(body.data(), body.size(), &hello) || hello.session != session) {
        ++m_secure_stats.auth_failures;
        return false;
    }
    if (!TimestampAcceptable(hello.timestamp_ms)) {
        ++m_secure_stats.replay_drops;
        return false;
    }
    const bool current_session =
        m_session_keys_ready && hello.session == m_session_id &&
        std::memcmp(hello.nonce, m_client_nonce, kHandshakeNonceBytes) == 0;
    if (current_session) {
        // HELLO_ACK 丢失后的幂等重传：重发缓存 ACK，不重置会话。
        ++m_secure_stats.handshake_retries;
        m_last_rx_ms = NowMs();
        if (!m_cached_hello_ack.empty()) {
            (void)SendRawPacket(m_cached_hello_ack.data(), m_cached_hello_ack.size());
        }
        return true;
    }
    if (IsKnownClientNonce(hello.nonce)) {
        // 已处理过的 client nonce：旧 HELLO 重放，拒绝并保持当前会话。
        ++m_secure_stats.replay_drops;
        return false;
    }
    if (m_have_peer && !PeerMatches(from)) {
        ++m_stats.unknown_peer_frames;
        return false;
    }
    // 新会话：先完成可能失败的 X25519 协商，再提交会话状态。否则低阶
    // 公钥会把 peer/nonce 缓存污染成半初始化状态，影响后续合法重连。
    std::uint8_t server_nonce[kHandshakeNonceBytes];
    SecureRandomBytes(server_nonce, kHandshakeNonceBytes);
    std::uint8_t tx_key[kChaCha20KeyBytes];
    std::uint8_t rx_key[kChaCha20KeyBytes];
    std::uint8_t server_public[kX25519KeyBytes] = {};
    if (m_config.enable_x25519 && !IsZeroX25519Key(hello.x25519_public)) {
        std::uint8_t private_key[kX25519KeyBytes];
        std::uint8_t shared[kX25519KeyBytes];
        X25519GeneratePrivateKey(private_key);
        const bool derived = X25519PublicKeyFromPrivate(private_key, server_public) &&
                             X25519ComputeSharedSecret(private_key, hello.x25519_public, shared);
        if (!derived) {
            // 不发送 ACK：对端会按握手超时失败，而不是静默降级。
            SecureZero(private_key, sizeof(private_key));
            ++m_secure_stats.auth_failures;
            return false;
        }
        DeriveSessionKeyWithX25519(m_psk, shared, hello.nonce, server_nonce,
                                   SecureDirection::kClientToServer, rx_key);
        DeriveSessionKeyWithX25519(m_psk, shared, hello.nonce, server_nonce,
                                   SecureDirection::kServerToClient, tx_key);
        SecureZero(shared, sizeof(shared));
        // 监听方发完 ACK 不再需要私钥；这里本地派生完就清零。
        SecureZero(private_key, sizeof(private_key));
        ++m_secure_stats.x25519_handshakes;
    } else {
        // 对端没有临时公钥（旧版本或显式回退）：保持原有纯 PSK 派生。
        DeriveSessionKey(m_psk, hello.nonce, server_nonce, SecureDirection::kClientToServer,
                         rx_key);
        DeriveSessionKey(m_psk, hello.nonce, server_nonce, SecureDirection::kServerToClient,
                         tx_key);
        ++m_secure_stats.psk_fallbacks;
    }
    // 新 nonce 允许握手 seq 从 1 重来；低阶公钥路径已在上面提前返回。
    m_handshake_rx_window.Reset();
    m_handshake_rx_window.Accept(outer.seq);
    RememberClientNonce(hello.nonce);
    m_peer = from;
    m_peer_len = from_len;
    m_have_peer = true;
    m_session_id = hello.session;
    m_conn_id = handshake_conn_id != 0 ? handshake_conn_id : RandomNonZeroU32();
    std::memcpy(m_client_nonce, hello.nonce, kHandshakeNonceBytes);
    std::memcpy(m_server_nonce, server_nonce, kHandshakeNonceBytes);
    std::memcpy(m_tx_key, tx_key, kChaCha20KeyBytes);
    std::memcpy(m_rx_key, rx_key, kChaCha20KeyBytes);
    std::memcpy(m_x25519_public, server_public, kX25519KeyBytes);
    m_x25519_private_valid = false;
    SecureZero(m_x25519_private, sizeof(m_x25519_private));
    m_tx_direction = SecureDirection::kServerToClient;
    m_rx_direction = SecureDirection::kClientToServer;
    m_session_keys_ready = true;
    SecureZero(tx_key, sizeof(tx_key));
    SecureZero(rx_key, sizeof(rx_key));
    m_data_rx_window.Reset();
    m_tx_wire_seq = 0;
    m_resume_epoch = false;
    m_resumed = false;
    m_peer_resume_epoch_seen = false;
    m_send_resume_ack = false;
    m_have_retired_peer = false;
    // 每个新会话的 HELLO_ACK 使用独立随机 seq，避免固定 nonce 重用。
    m_handshake_tx_seq = RandomNonZeroU32();
    ResetApplicationChannels();
    if (!SendSecureHelloAck()) {
        return false;
    }
    CacheServerSession();
    m_last_rx_ms = NowMs();
    if (m_state != LinkState::kConnected) {
        m_state = LinkState::kConnected;
        ++m_stats.handshakes;
        ++m_secure_stats.handshakes;
        ResetPmtud(m_last_rx_ms);
        if (on_connected) {
            on_connected();
        }
    }
    return true;
}

bool Link::HandleSecureHelloAck(const std::uint8_t* data, std::size_t bytes,
                                const FrameHeader& outer, const sockaddr_storage& from) {
    if (m_is_listener || m_state != LinkState::kConnecting || !PeerMatches(from)) {
        ++m_stats.unknown_peer_frames;
        return false;
    }
    std::uint8_t inner_type = 0;
    std::uint32_t session = 0;
    std::uint32_t handshake_conn_id = 0;
    std::vector<std::uint8_t> body;
    OpenStatus status = OpenStatus::kMalformed;
    if (!OpenHandshakeFrame(m_psk, SecureDirection::kServerToClient, data, bytes, &inner_type,
                            &session, &handshake_conn_id, &body, &status)) {
        if (status == OpenStatus::kAuthFailed) {
            ++m_secure_stats.auth_failures;
        } else {
            ++m_secure_stats.malformed_frames;
        }
        return false;
    }
    if (inner_type != static_cast<std::uint8_t>(FrameType::kHelloAck)) {
        ++m_secure_stats.malformed_frames;
        return false;
    }
    HelloAckBody ack;
    if (!DecodeHelloAckBody(body.data(), body.size(), &ack) || ack.session != m_session_id ||
        std::memcmp(ack.client_nonce, m_client_nonce, kHandshakeNonceBytes) != 0) {
        // client nonce 不匹配：证书式绑定失败，可能是重放的旧 ACK 或错误 PSK。
        ++m_secure_stats.auth_failures;
        return false;
    }
    if (!TimestampAcceptable(ack.timestamp_ms)) {
        ++m_secure_stats.replay_drops;
        return false;
    }
    if (!m_handshake_rx_window.WouldAccept(outer.seq)) {
        ++m_secure_stats.replay_drops;
        return false;
    }
    m_handshake_rx_window.Accept(outer.seq);
    if (handshake_conn_id != 0) {
        m_conn_id = handshake_conn_id;
    }
    std::memcpy(m_server_nonce, ack.server_nonce, kHandshakeNonceBytes);
    m_tx_direction = SecureDirection::kClientToServer;
    m_rx_direction = SecureDirection::kServerToClient;
    if (m_config.enable_x25519 && !IsZeroX25519Key(ack.x25519_public)) {
        if (!DeriveX25519SessionKeys(ack.x25519_public)) {
            // 低阶公钥或临时私钥状态丢失：禁止降级回纯 PSK，直接失败。
            ++m_secure_stats.auth_failures;
            return false;
        }
        ++m_secure_stats.x25519_handshakes;
    } else {
        // 旧对端没有公钥：保持原有纯 PSK 派生（兼容旧服务端）。
        DeriveSessionKey(m_psk, m_client_nonce, m_server_nonce, m_tx_direction, m_tx_key);
        DeriveSessionKey(m_psk, m_client_nonce, m_server_nonce, m_rx_direction, m_rx_key);
        std::memset(m_x25519_public, 0, sizeof(m_x25519_public));
        ClearX25519Ephemeral();
        ++m_secure_stats.psk_fallbacks;
    }
    m_session_keys_ready = true;
    m_data_rx_window.Reset();
    m_tx_wire_seq = 0;
    m_resume_epoch = false;
    m_resumed = false;
    m_peer_resume_epoch_seen = false;
    m_send_resume_ack = false;
    m_have_retired_peer = false;
    m_last_rx_ms = NowMs();
    if (m_state != LinkState::kConnected) {
        m_state = LinkState::kConnected;
        ++m_stats.handshakes;
        ++m_secure_stats.handshakes;
        ResetPmtud(m_last_rx_ms);
        if (on_connected) {
            on_connected();
        }
    }
    return true;
}

std::string LinkStatsToJson(const LinkStats& stats) {
    std::ostringstream out;
    out << '{';
    bool first = true;
    const auto emit = [&out, &first](const char* name, auto value) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << '"' << name << '"' << ':' << value;
    };
    emit("frames_sent", stats.frames_sent);
    emit("frames_received", stats.frames_received);
    emit("bytes_sent", stats.bytes_sent);
    emit("bytes_received", stats.bytes_received);
    emit("invalid_frames", stats.invalid_frames);
    emit("unknown_peer_frames", stats.unknown_peer_frames);
    emit("send_errors", stats.send_errors);
    emit("recv_errors", stats.recv_errors);
    emit("poll_errors", stats.poll_errors);
    emit("handshakes", stats.handshakes);
    emit("hello_retransmits", stats.hello_retransmits);
    emit("heartbeats_sent", stats.heartbeats_sent);
    emit("acks_sent", stats.acks_sent);
    emit("barriers_received", stats.barriers_received);
    emit("timeouts", stats.timeouts);
    emit("conn_id_mismatch_frames", stats.conn_id_mismatch_frames);
    emit("stale_peer_frames", stats.stale_peer_frames);
    emit("peer_migrations", stats.peer_migrations);
    emit("pmtud_probes_sent", stats.pmtud_probes_sent);
    emit("pmtud_probes_received", stats.pmtud_probes_received);
    emit("pmtud_acks_received", stats.pmtud_acks_received);
    emit("pmtud_icmp_errors", stats.pmtud_icmp_errors);
    emit("pmtud_blackholes", stats.pmtud_blackholes);
    emit("rtt_ms", stats.rtt_ms);
    emit("srtt_ms", stats.srtt_ms);
    emit("rttvar_ms", stats.rttvar_ms);
    emit("pto_ms", stats.pto_ms);
    emit("cwnd_packets", stats.cwnd_packets);
    emit("ssthresh_packets", stats.ssthresh_packets);
    emit("inflight", stats.inflight);
    emit("loss", stats.loss_events);
    emit("retrans", stats.retransmits);
    emit("bandwidth_bps", stats.bandwidth_bps);
    emit("flow_control_blocked", stats.flow_control_blocked);
    emit("max_data_sent", stats.max_data_sent);
    emit("max_data_received", stats.max_data_received);
    out << ",\"loss_rate\":" << std::fixed << std::setprecision(4) << stats.loss_rate;
    out << '}';
    return out.str();
}

}  // namespace go2cpp::bento
