/**
 * @file udp_transport.cpp
 * @brief `UdpTransport` 实现：非阻塞 UDP、poll deadline、EINTR/EAGAIN 循环与截断检测。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/udp_transport.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace go2cpp::bento::net {
namespace {

using Clock = std::chrono::steady_clock;

/// 一条解析结果：sockaddr_storage 足够容纳 IPv4/IPv6。
struct AddrEntry {
    sockaddr_storage storage{};
    socklen_t length = 0;
};

/// 单次调用的最大超时（24h）。`time_point + duration` 在 64 位纳秒表示下会先溢出：
/// 实测 `milliseconds::max()` 触发有符号整数溢出（UBSan 报错），deadline 变成
/// 过去时间，调用立即返回 `kTimeout`。钳制到 24h 既保留「几乎无限等待」的语义，
/// 又保证加法永不溢出。
constexpr std::int64_t kMaxTimeoutMs = 24LL * 60 * 60 * 1000;

/// 负超时按 0 处理；超过 `kMaxTimeoutMs` 按上限处理。
std::chrono::milliseconds NormalizeTimeout(std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() < 0) {
        return std::chrono::milliseconds(0);
    }
    if (timeout.count() > kMaxTimeoutMs) {
        return std::chrono::milliseconds(kMaxTimeoutMs);
    }
    return timeout;
}

/**
 * @brief 解析端点（SOCK_DGRAM）。
 * @param passive true 用于 Bind（允许空 host 通配、端口 0）。
 * @param[out] addresses 解析出的地址列表。
 * @param[out] error 失败原因（errno 风格）。
 * @return 至少拿到一个地址时 true。
 *
 * @note DNS 解析不受调用方 timeout 约束，这是 getaddrinfo 的固有限制；
 *       需要硬上界时传 IP 字面量。
 */
bool Resolve(const Endpoint& endpoint, bool passive, std::vector<AddrEntry>* addresses,
             int* error) {
    addresses->clear();
    if (!passive && (endpoint.host.empty() || endpoint.port == 0)) {
        *error = EINVAL;
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICSERV;
    if (passive) {
        hints.ai_flags |= AI_PASSIVE;
    }

    addrinfo* raw = nullptr;
    const std::string service = std::to_string(endpoint.port);
    const char* node = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
    const int rc = ::getaddrinfo(node, service.c_str(), &hints, &raw);
    if (rc != 0 || raw == nullptr) {
        if (raw != nullptr) {
            ::freeaddrinfo(raw);
        }
        *error = EHOSTUNREACH;
        return false;
    }

    for (const addrinfo* it = raw; it != nullptr; it = it->ai_next) {
        if (it->ai_addr == nullptr || it->ai_addrlen == 0 ||
            static_cast<std::size_t>(it->ai_addrlen) > sizeof(sockaddr_storage)) {
            continue;
        }
        AddrEntry entry;
        std::memcpy(&entry.storage, it->ai_addr, it->ai_addrlen);
        entry.length = static_cast<socklen_t>(it->ai_addrlen);
        addresses->push_back(entry);
    }
    ::freeaddrinfo(raw);

    if (addresses->empty()) {
        *error = EHOSTUNREACH;
        return false;
    }
    return true;
}

/// 设置 O_NONBLOCK + FD_CLOEXEC；失败返回 false，errno 保留。
bool SetNonBlocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    const int fd_flags = ::fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0) {
        (void)::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
    return true;
}

/// 创建非阻塞 UDP socket；失败返回 -1 并保留 errno。
int OpenSocket(int family) noexcept {
    const int fd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }
    if (!SetNonBlocking(fd)) {
        const int error = errno;
        (void)::close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

enum class WaitStatus : std::uint8_t {
    kReady = 0,
    kTimeout = 1,
    kError = 2,
};

/**
 * @brief 等待 fd 的指定事件就绪，严格受 deadline 约束。
 *
 * @details `timeout == 0` 时仍会执行一次 `poll(..., 0)`，允许非阻塞调用
 *          先消费内核里已经就绪的数据报。`EINTR` 不视为错误：重新计算
 *          剩余时间后继续等待。
 */
WaitStatus WaitFor(int fd, short events, Clock::time_point deadline, int* error) {
    for (;;) {
        int wait_ms = 0;
        const Clock::duration left = deadline - Clock::now();
        if (left > Clock::duration::zero()) {
            const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left);
            const std::int64_t clamped = std::min<std::int64_t>(
                static_cast<std::int64_t>(ms.count()),
                static_cast<std::int64_t>(std::numeric_limits<int>::max()));
            wait_ms = static_cast<int>(clamped);
        }

        pollfd entry{};
        entry.fd = fd;
        entry.events = events;
        entry.revents = 0;
        const int rc = ::poll(&entry, 1, wait_ms);
        if (rc > 0) {
            return WaitStatus::kReady;
        }
        if (rc == 0) {
            if (Clock::now() >= deadline) {
                *error = ETIMEDOUT;
                return WaitStatus::kTimeout;
            }
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        *error = errno;
        return WaitStatus::kError;
    }
}

/// errno -> 可移植分类。保留原始 errno 供排障。
Result MapIoError(int error, std::size_t bytes) noexcept {
    switch (error) {
        case ECONNRESET:
        case EPIPE:
        case ENOTCONN:
        case ECONNABORTED:
        case ESHUTDOWN:
            return Result{Code::kClosed, error, bytes};
        case ETIMEDOUT:
            return Result{Code::kTimeout, error, bytes};
        default:
            return Result{Code::kError, error, bytes};
    }
}

/// 把内核 sockaddr 转成数字形式 Endpoint。
bool EndpointFromSockAddr(const sockaddr_storage& storage, socklen_t length,
                          Endpoint* endpoint) {
    char host[NI_MAXHOST]{};
    char service[NI_MAXSERV]{};
    const int rc = ::getnameinfo(reinterpret_cast<const sockaddr*>(&storage), length, host,
                                 sizeof(host), service, sizeof(service),
                                 NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) {
        return false;
    }
    endpoint->host = host;
    endpoint->port = static_cast<std::uint16_t>(std::strtoul(service, nullptr, 10));
    return true;
}

}  // namespace

UdpTransport::~UdpTransport() {
    (void)Close();
}

Result UdpTransport::Bind(const Endpoint& local, bool reuse_address) {
    if (m_fd >= 0) {
        return Result{Code::kError, EALREADY, 0};
    }

    std::vector<AddrEntry> addresses;
    int resolve_error = 0;
    if (!Resolve(local, true, &addresses, &resolve_error)) {
        return Result{Code::kError, resolve_error, 0};
    }

    int last_error = EADDRINUSE;
    for (const AddrEntry& entry : addresses) {
        const int fd = OpenSocket(entry.storage.ss_family);
        if (fd < 0) {
            last_error = errno;
            continue;
        }

        if (reuse_address) {
            int reuse = 1;
            (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        }
        if (entry.storage.ss_family == AF_INET6) {
            // 通配绑定时同时接受 IPv4 映射地址；显式 ::1 不受影响。
            int v6_only = 0;
            (void)::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only));
        }

        if (::bind(fd, reinterpret_cast<const sockaddr*>(&entry.storage), entry.length) != 0) {
            last_error = errno;
            (void)::close(fd);
            continue;
        }

        m_fd = fd;
        m_family = entry.storage.ss_family;
        m_connected = false;
        m_write_shutdown = false;
        return Result{Code::kOk, 0, 0};
    }

    return Result{Code::kError, last_error, 0};
}

Result UdpTransport::Connect(const Endpoint& endpoint, std::chrono::milliseconds timeout) {
    if (m_connected) {
        return Result{Code::kError, EALREADY, 0};
    }
    if (endpoint.host.empty() || endpoint.port == 0) {
        return Result{Code::kError, EINVAL, 0};
    }

    std::vector<AddrEntry> addresses;
    int resolve_error = 0;
    if (!Resolve(endpoint, false, &addresses, &resolve_error)) {
        return Result{Code::kError, resolve_error, 0};
    }

    const Clock::time_point deadline = Clock::now() + NormalizeTimeout(timeout);
    int last_error = ECONNREFUSED;
    bool family_mismatch = false;
    for (const AddrEntry& entry : addresses) {
        if (m_fd >= 0 && m_family >= 0 && entry.storage.ss_family != m_family) {
            family_mismatch = true;
            continue;
        }

        bool created = false;
        int fd = m_fd;
        if (fd < 0) {
            fd = OpenSocket(entry.storage.ss_family);
            if (fd < 0) {
                last_error = errno;
                continue;
            }
            created = true;
        }

        int rc = 0;
        do {
            rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&entry.storage), entry.length);
        } while (rc != 0 && errno == EINTR);

        if (rc != 0) {
            const int connect_error = errno;
            if (connect_error == EINPROGRESS || connect_error == EWOULDBLOCK) {
                int wait_error = 0;
                const WaitStatus status = WaitFor(fd, POLLOUT, deadline, &wait_error);
                if (status == WaitStatus::kTimeout) {
                    if (created) {
                        (void)::close(fd);
                    }
                    return Result{Code::kTimeout, ETIMEDOUT, 0};
                }
                if (status == WaitStatus::kError) {
                    last_error = wait_error;
                    if (created) {
                        (void)::close(fd);
                    }
                    continue;
                }

                int so_error = 0;
                socklen_t so_length = sizeof(so_error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_length) != 0) {
                    last_error = errno;
                    if (created) {
                        (void)::close(fd);
                    }
                    continue;
                }
                if (so_error != 0) {
                    last_error = so_error;
                    if (created) {
                        (void)::close(fd);
                    }
                    continue;
                }
            } else {
                last_error = connect_error;
                if (created) {
                    (void)::close(fd);
                }
                continue;
            }
        }

        m_fd = fd;
        m_family = entry.storage.ss_family;
        m_connected = true;
        m_write_shutdown = false;
        return Result{Code::kOk, 0, 0};
    }

    if (m_fd >= 0 && family_mismatch) {
        return Result{Code::kError, EAFNOSUPPORT, 0};
    }
    return Result{Code::kError, last_error, 0};
}

Result UdpTransport::Read(std::uint8_t* buffer, std::size_t capacity,
                          std::chrono::milliseconds timeout) {
    if (m_fd < 0) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    if (capacity == 0) {
        return Result{Code::kOk, 0, 0};
    }
    if (buffer == nullptr) {
        return Result{Code::kError, EINVAL, 0};
    }
    if (!m_connected) {
        return Result{Code::kError, ENOTCONN, 0};
    }

    const std::chrono::milliseconds budget = NormalizeTimeout(timeout);
    const Clock::time_point deadline = Clock::now() + budget;
    // 阻塞模式下先 recv 再 poll：数据通常已就绪，可省掉每包一次 poll 系统调用；
    // timeout==0 时保持 poll 优先，避免空闲轮询多一次必然失败的系统调用。
    const bool recv_first = budget.count() > 0;
    const auto poll_readable = [&](Result* failure) {
        int wait_error = 0;
        const WaitStatus status = WaitFor(m_fd, POLLIN, deadline, &wait_error);
        if (status == WaitStatus::kTimeout) {
            *failure = Result{Code::kTimeout, ETIMEDOUT, 0};
            return false;
        }
        if (status == WaitStatus::kError) {
            *failure =
                m_fd < 0 ? Result{Code::kClosed, ENOTCONN, 0} : MapIoError(wait_error, 0);
            return false;
        }
        return true;
    };
    for (;;) {
        if (!recv_first) {
            Result failure;
            if (!poll_readable(&failure)) {
                return failure;
            }
        }

        const ssize_t received = ::recv(m_fd, buffer, capacity, 0);
        if (received >= 0) {
            return Result{Code::kOk, 0, static_cast<std::size_t>(received)};
        }
        const int error = errno;
        if (error == EINTR) {
            continue;
        }
        if (error == EAGAIN || error == EWOULDBLOCK) {
            Result failure;
            if (!poll_readable(&failure)) {
                return failure;
            }
            continue;
        }
        return MapIoError(error, 0);
    }
}

Result UdpTransport::Write(const std::uint8_t* data, std::size_t size,
                           std::chrono::milliseconds timeout) {
    if (m_fd < 0) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    if (m_write_shutdown) {
        return Result{Code::kClosed, EPIPE, 0};
    }
    if (size == 0) {
        return Result{Code::kOk, 0, 0};
    }
    if (data == nullptr) {
        return Result{Code::kError, EINVAL, 0};
    }
    if (!m_connected) {
        return Result{Code::kError, EDESTADDRREQ, 0};
    }

    const Clock::time_point deadline = Clock::now() + NormalizeTimeout(timeout);
    for (;;) {
        const ssize_t sent = ::send(m_fd, data, size, MSG_NOSIGNAL);
        if (sent >= 0) {
            const std::size_t written = static_cast<std::size_t>(sent);
            if (written == size) {
                return Result{Code::kOk, 0, written};
            }
            // UDP 是原子发送；短写属于异常实现行为，不能当作部分成功继续。
            return Result{Code::kError, EIO, written};
        }
        const int error = errno;
        if (error == EINTR) {
            continue;
        }
        if (error == EAGAIN || error == EWOULDBLOCK) {
            int wait_error = 0;
            const WaitStatus status = WaitFor(m_fd, POLLOUT, deadline, &wait_error);
            if (status == WaitStatus::kTimeout) {
                return Result{Code::kTimeout, ETIMEDOUT, 0};
            }
            if (status == WaitStatus::kError) {
                if (m_fd < 0) {
                    return Result{Code::kClosed, ENOTCONN, 0};
                }
                return MapIoError(wait_error, 0);
            }
            continue;
        }
        return MapIoError(error, 0);
    }
}

Result UdpTransport::SendTo(const Endpoint& peer, const std::uint8_t* data, std::size_t size,
                            std::chrono::milliseconds timeout) {
    if (m_fd < 0) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    if (m_write_shutdown) {
        return Result{Code::kClosed, EPIPE, 0};
    }
    if (peer.host.empty() || peer.port == 0) {
        return Result{Code::kError, EINVAL, 0};
    }
    if (size > 0 && data == nullptr) {
        return Result{Code::kError, EINVAL, 0};
    }

    std::vector<AddrEntry> addresses;
    int resolve_error = 0;
    if (!Resolve(peer, false, &addresses, &resolve_error)) {
        return Result{Code::kError, resolve_error, 0};
    }

    const Clock::time_point deadline = Clock::now() + NormalizeTimeout(timeout);
    int last_error = EAFNOSUPPORT;
    bool family_mismatch = false;
    for (const AddrEntry& entry : addresses) {
        if (m_family >= 0 && entry.storage.ss_family != m_family) {
            family_mismatch = true;
            continue;
        }

        for (;;) {
            const ssize_t sent =
                ::sendto(m_fd, data, size, MSG_NOSIGNAL,
                         reinterpret_cast<const sockaddr*>(&entry.storage), entry.length);
            if (sent >= 0) {
                const std::size_t written = static_cast<std::size_t>(sent);
                if (written == size) {
                    return Result{Code::kOk, 0, written};
                }
                return Result{Code::kError, EIO, written};
            }

            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            if (error == EAGAIN || error == EWOULDBLOCK) {
                int wait_error = 0;
                const WaitStatus status = WaitFor(m_fd, POLLOUT, deadline, &wait_error);
                if (status == WaitStatus::kTimeout) {
                    return Result{Code::kTimeout, ETIMEDOUT, 0};
                }
                if (status == WaitStatus::kError) {
                    if (m_fd < 0) {
                        return Result{Code::kClosed, ENOTCONN, 0};
                    }
                    last_error = wait_error;
                    break;
                }
                continue;
            }

            last_error = error;
            break;
        }
    }

    if (family_mismatch && last_error == EAFNOSUPPORT) {
        return Result{Code::kError, EAFNOSUPPORT, 0};
    }
    return Result{Code::kError, last_error, 0};
}

Result UdpTransport::RecvFrom(std::uint8_t* buffer, std::size_t capacity,
                              std::chrono::milliseconds timeout, Endpoint* peer,
                              RecvInfo* info) {
    if (m_fd < 0) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    if (buffer == nullptr && capacity > 0) {
        return Result{Code::kError, EINVAL, 0};
    }

    const std::chrono::milliseconds budget = NormalizeTimeout(timeout);
    const Clock::time_point deadline = Clock::now() + budget;
    // 同 Read：阻塞模式先 recvmsg 再 poll，省掉高负载下每包一次 poll；
    // timeout==0 保持 poll 优先，空闲轮询成本不变。
    const bool recv_first = budget.count() > 0;
    const auto poll_readable = [&](Result* failure) {
        int wait_error = 0;
        const WaitStatus status = WaitFor(m_fd, POLLIN, deadline, &wait_error);
        if (status == WaitStatus::kTimeout) {
            *failure = Result{Code::kTimeout, ETIMEDOUT, 0};
            return false;
        }
        if (status == WaitStatus::kError) {
            *failure =
                m_fd < 0 ? Result{Code::kClosed, ENOTCONN, 0} : MapIoError(wait_error, 0);
            return false;
        }
        return true;
    };
    for (;;) {
        if (!recv_first) {
            Result failure;
            if (!poll_readable(&failure)) {
                return failure;
            }
        }

        sockaddr_storage storage{};
        iovec iov{};
        iov.iov_base = buffer;
        iov.iov_len = capacity;
        msghdr message{};
        message.msg_name = &storage;
        message.msg_namelen = sizeof(storage);
        message.msg_iov = &iov;
        message.msg_iovlen = 1;

        // Linux 下 recvmsg 的 MSG_TRUNC 入参返回数据报真实长度；即使缓冲
        // 不足，调用方也能通过 RecvInfo 知道被截断了多少。
        const ssize_t received = ::recvmsg(m_fd, &message, MSG_TRUNC);
        if (received >= 0) {
            const std::size_t datagram_bytes = static_cast<std::size_t>(received);
            const std::size_t copied = std::min(datagram_bytes, capacity);
            if (info != nullptr) {
                info->bytes = copied;
                info->datagram_bytes = datagram_bytes;
                info->truncated = (message.msg_flags & MSG_TRUNC) != 0 ||
                                  datagram_bytes > capacity;
            }
            if (peer != nullptr) {
                if (message.msg_namelen > 0 &&
                    EndpointFromSockAddr(storage, message.msg_namelen, peer)) {
                    // 已填充来源地址。
                } else if (m_connected) {
                    Endpoint connected_peer;
                    if (PeerEndpoint(&connected_peer).ok()) {
                        *peer = connected_peer;
                    } else {
                        peer->host.clear();
                        peer->port = 0;
                    }
                } else {
                    peer->host.clear();
                    peer->port = 0;
                }
            }
            return Result{Code::kOk, 0, copied};
        }

        const int error = errno;
        if (error == EINTR) {
            continue;
        }
        if (error == EAGAIN || error == EWOULDBLOCK) {
            Result failure;
            if (!poll_readable(&failure)) {
                return failure;
            }
            continue;
        }
        return MapIoError(error, 0);
    }
}

Result UdpTransport::ShutdownWrite() {
    if (m_fd < 0) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    m_write_shutdown = true;
    return Result{Code::kOk, 0, 0};
}

Result UdpTransport::Close() {
    if (m_fd >= 0) {
        (void)::close(m_fd);
        m_fd = -1;
    }
    m_family = -1;
    m_connected = false;
    m_write_shutdown = false;
    return Result{Code::kOk, 0, 0};
}

Result UdpTransport::LocalEndpoint(Endpoint* endpoint) const {
    if (endpoint == nullptr) {
        return Result{Code::kError, EINVAL, 0};
    }
    if (m_fd < 0) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }

    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        return Result{Code::kError, errno, 0};
    }
    if (!EndpointFromSockAddr(storage, length, endpoint)) {
        return Result{Code::kError, EINVAL, 0};
    }
    return Result{Code::kOk, 0, 0};
}

Result UdpTransport::PeerEndpoint(Endpoint* endpoint) const {
    if (endpoint == nullptr) {
        return Result{Code::kError, EINVAL, 0};
    }
    if (m_fd < 0) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    if (!m_connected) {
        return Result{Code::kError, ENOTCONN, 0};
    }

    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getpeername(m_fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        return Result{Code::kError, errno, 0};
    }
    if (!EndpointFromSockAddr(storage, length, endpoint)) {
        return Result{Code::kError, EINVAL, 0};
    }
    return Result{Code::kOk, 0, 0};
}

}  // namespace go2cpp::bento::net
