#include "go2cpp/rd_net/socket.hpp"

/// @file socket.cpp
/// @brief REQ-006 的平台实现：非阻塞 POSIX socket + 四态错误映射。

#if !defined(_WIN32)

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace rd::net {
namespace {

/// @brief 解析 IP 字面量（快速路径，避免热路径反复 getaddrinfo）。
bool ParseIpLiteral(const Endpoint& ep, int sock_type, sockaddr_storage* out,
                    socklen_t* out_len) {
    if (ep.host.empty()) {
        return false;
    }
    sockaddr_in v4{};
    if (::inet_pton(AF_INET, ep.host.c_str(), &v4.sin_addr) == 1) {
        v4.sin_family = AF_INET;
        v4.sin_port = htons(ep.port);
        std::memcpy(out, &v4, sizeof(v4));
        *out_len = static_cast<socklen_t>(sizeof(v4));
        (void)sock_type;
        return true;
    }
    sockaddr_in6 v6{};
    if (::inet_pton(AF_INET6, ep.host.c_str(), &v6.sin6_addr) == 1) {
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(ep.port);
        std::memcpy(out, &v6, sizeof(v6));
        *out_len = static_cast<socklen_t>(sizeof(v6));
        return true;
    }
    return false;
}

/// @brief 把 Endpoint 解析成 sockaddr；passive=true 用于 bind。
bool ResolveEndpoint(const Endpoint& ep, int sock_type, bool passive,
                     sockaddr_storage* out, socklen_t* out_len, int* error) {
    if (ParseIpLiteral(ep, sock_type, out, out_len)) {
        return true;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = sock_type;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    char service[16];
    std::snprintf(service, sizeof(service), "%u", static_cast<unsigned>(ep.port));
    const char* node = ep.host.empty() ? nullptr : ep.host.c_str();
    addrinfo* list = nullptr;
    const int rc = ::getaddrinfo(node, service, &hints, &list);
    if (rc != 0) {
        *error = rc;
        return false;
    }
    bool copied = false;
    for (const addrinfo* it = list; it != nullptr; it = it->ai_next) {
        if (it->ai_addrlen <= sizeof(sockaddr_storage)) {
            std::memcpy(out, it->ai_addr, it->ai_addrlen);
            *out_len = static_cast<socklen_t>(it->ai_addrlen);
            copied = true;
            break;
        }
    }
    ::freeaddrinfo(list);
    if (!copied) {
        *error = EAI_FAMILY;
    }
    return copied;
}

Endpoint EndpointFromSockaddr(const sockaddr* sa, socklen_t len) {
    Endpoint ep;
    if (sa == nullptr) {
        return ep;
    }
    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};
    if (::getnameinfo(sa, len, host, sizeof(host), service, sizeof(service),
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return ep;
    }
    ep.host = host;
    ep.port = static_cast<std::uint16_t>(std::strtoul(service, nullptr, 10));
    return ep;
}

bool SetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool SetIntOption(int fd, int level, int name, int value) {
    return ::setsockopt(fd, level, name, &value, sizeof(value)) == 0;
}

/// @brief 收发 errno → IoStatus（EINTR 已在调用点重试）。
IoStatus MapSendErrno(int err) {
    switch (err) {
        case EAGAIN:
            return IoStatus::kWouldBlock;
        case EPIPE:
            return IoStatus::kClosed;
        case ECONNRESET:
        case ECONNREFUSED:
            return IoStatus::kReset;
        case ETIMEDOUT:
            return IoStatus::kTimeout;
        default:
            return IoStatus::kIoError;
    }
}

IoStatus MapRecvErrno(int err) {
    switch (err) {
        case EAGAIN:
            return IoStatus::kWouldBlock;
        case ECONNRESET:
        case ECONNREFUSED:
            return IoStatus::kReset;
        case ETIMEDOUT:
            return IoStatus::kTimeout;
        default:
            return IoStatus::kIoError;
    }
}

IoStatus MapConnectErrno(int err) {
    switch (err) {
        case ECONNREFUSED:
        case ECONNRESET:
            return IoStatus::kReset;
        case ETIMEDOUT:
            return IoStatus::kTimeout;
        default:
            return IoStatus::kIoError;
    }
}

}  // namespace

const char* IoStatusName(IoStatus status) noexcept {
    switch (status) {
        case IoStatus::kOk:
            return "kOk";
        case IoStatus::kWouldBlock:
            return "kWouldBlock";
        case IoStatus::kTimeout:
            return "kTimeout";
        case IoStatus::kClosed:
            return "kClosed";
        case IoStatus::kReset:
            return "kReset";
        case IoStatus::kTruncated:
            return "kTruncated";
        case IoStatus::kIoError:
            return "kIoError";
        case IoStatus::kInvalid:
            return "kInvalid";
    }
    return "Unknown";
}

// ---------------------------------------------------------------- DatagramSocket

DatagramSocket::~DatagramSocket() { Close(); }

DatagramSocket::DatagramSocket(DatagramSocket&& other) noexcept
    : fd_(other.fd_),
      local_(std::move(other.local_)),
      next_token_(other.next_token_),
      last_error_(other.last_error_) {
    other.fd_ = -1;
    other.next_token_ = 0;
    other.last_error_ = 0;
}

DatagramSocket& DatagramSocket::operator=(DatagramSocket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        local_ = std::move(other.local_);
        next_token_ = other.next_token_;
        last_error_ = other.last_error_;
        other.fd_ = -1;
        other.next_token_ = 0;
        other.last_error_ = 0;
    }
    return *this;
}

Result<DatagramSocket> DatagramSocket::Open(const Endpoint& local,
                                            const SocketOptions& options) {
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    int resolve_err = 0;
    if (!ResolveEndpoint(local, SOCK_DGRAM, true, &addr, &addr_len, &resolve_err)) {
        return Result<DatagramSocket>::Err(IoStatus::kInvalid, resolve_err);
    }
    const int fd = ::socket(addr.ss_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return Result<DatagramSocket>::Err(IoStatus::kIoError, errno);
    }
    if (options.reuse_address && !SetIntOption(fd, SOL_SOCKET, SO_REUSEADDR, 1)) {
        const int err = errno;
        ::close(fd);
        return Result<DatagramSocket>::Err(IoStatus::kIoError, err);
    }
    if (options.broadcast && !SetIntOption(fd, SOL_SOCKET, SO_BROADCAST, 1)) {
        const int err = errno;
        ::close(fd);
        return Result<DatagramSocket>::Err(IoStatus::kIoError, err);
    }
    if (options.recv_buffer_bytes != 0 &&
        !SetIntOption(fd, SOL_SOCKET, SO_RCVBUF,
                      static_cast<int>(options.recv_buffer_bytes))) {
        const int err = errno;
        ::close(fd);
        return Result<DatagramSocket>::Err(IoStatus::kIoError, err);
    }
    if (options.send_buffer_bytes != 0 &&
        !SetIntOption(fd, SOL_SOCKET, SO_SNDBUF,
                      static_cast<int>(options.send_buffer_bytes))) {
        const int err = errno;
        ::close(fd);
        return Result<DatagramSocket>::Err(IoStatus::kIoError, err);
    }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), addr_len) != 0) {
        const int err = errno;
        ::close(fd);
        return Result<DatagramSocket>::Err(IoStatus::kIoError, err);
    }
    if (!SetNonBlocking(fd)) {
        const int err = errno;
        ::close(fd);
        return Result<DatagramSocket>::Err(IoStatus::kIoError, err);
    }
    sockaddr_storage bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
        const int err = errno;
        ::close(fd);
        return Result<DatagramSocket>::Err(IoStatus::kIoError, err);
    }
    DatagramSocket sock;
    sock.fd_ = fd;
    sock.local_ = EndpointFromSockaddr(reinterpret_cast<const sockaddr*>(&bound),
                                       bound_len);
    return Result<DatagramSocket>::Ok(std::move(sock));
}

IoStatus DatagramSocket::SendTo(ByteView data, const Endpoint& to, SendResult* out) {
    SendResult result;
    result.token = ++next_token_;
    result.status = IoStatus::kInvalid;
    last_error_ = 0;
    if (out != nullptr) {
        *out = result;
    }
    if (!valid() || (data.data == nullptr && data.size != 0) ||
        data.size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        last_error_ = EINVAL;
        if (out != nullptr) {
            out->status = IoStatus::kInvalid;
        }
        return IoStatus::kInvalid;
    }
    sockaddr_storage dst{};
    socklen_t dst_len = 0;
    int resolve_err = 0;
    if (!ResolveEndpoint(to, SOCK_DGRAM, false, &dst, &dst_len, &resolve_err)) {
        last_error_ = resolve_err;
        if (out != nullptr) {
            out->status = IoStatus::kInvalid;
        }
        return IoStatus::kInvalid;
    }
    for (;;) {
        const ssize_t n =
            ::sendto(fd_, data.data, data.size, MSG_DONTWAIT | MSG_NOSIGNAL,
                     reinterpret_cast<const sockaddr*>(&dst), dst_len);
        if (n >= 0) {
            if (static_cast<std::size_t>(n) != data.size) {
                // UDP 不产生部分发送；若内核仍返回短写，按错误上报，避免静默丢包。
                last_error_ = 0;
                if (out != nullptr) {
                    out->status = IoStatus::kIoError;
                }
                return IoStatus::kIoError;
            }
            result.status = IoStatus::kOk;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kOk;
        }
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        last_error_ = err;
        result.status = MapSendErrno(err);
        if (out != nullptr) {
            *out = result;
        }
        return result.status;
    }
}

IoStatus DatagramSocket::RecvFrom(MutableByteView buf, Endpoint* from,
                                  std::size_t* received) {
    if (received != nullptr) {
        *received = 0;
    }
    last_error_ = 0;
    if (!valid() || buf.data == nullptr || buf.size == 0) {
        last_error_ = EINVAL;
        return IoStatus::kInvalid;
    }
    iovec iov{};
    iov.iov_base = buf.data;
    iov.iov_len = buf.size;
    sockaddr_storage src{};
    msghdr msg{};
    msg.msg_name = &src;
    msg.msg_namelen = static_cast<socklen_t>(sizeof(src));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    for (;;) {
        // 传入 MSG_TRUNC：报文大于缓冲时返回值是**实际报文长度**，
        // 同时 msg_flags 置 MSG_TRUNC。这样「截断」与「成功」可区分。
        const ssize_t n = ::recvmsg(fd_, &msg, MSG_DONTWAIT | MSG_TRUNC);
        if (n >= 0) {
            const std::size_t actual = static_cast<std::size_t>(n);
            if (from != nullptr) {
                *from = EndpointFromSockaddr(reinterpret_cast<const sockaddr*>(&src),
                                             msg.msg_namelen);
            }
            if (received != nullptr) {
                *received = actual;
            }
            if ((msg.msg_flags & MSG_TRUNC) != 0 || actual > buf.size) {
                return IoStatus::kTruncated;
            }
            return IoStatus::kOk;
        }
        const int err = errno;
        if (err == EINTR) {
            msg.msg_namelen = static_cast<socklen_t>(sizeof(src));
            msg.msg_flags = 0;
            continue;
        }
        last_error_ = err;
        if (err == EAGAIN) {
            last_error_ = 0;
        }
        return MapRecvErrno(err);
    }
}

void DatagramSocket::Close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    next_token_ = 0;
}

// ---------------------------------------------------------------- StreamSocket

StreamSocket::~StreamSocket() { Close(); }

StreamSocket::StreamSocket(StreamSocket&& other) noexcept
    : fd_(other.fd_),
      peer_(std::move(other.peer_)),
      local_(std::move(other.local_)),
      next_token_(other.next_token_),
      last_transferred_(other.last_transferred_),
      last_error_(other.last_error_) {
    other.fd_ = -1;
    other.next_token_ = 0;
    other.last_transferred_ = 0;
    other.last_error_ = 0;
}

StreamSocket& StreamSocket::operator=(StreamSocket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        peer_ = std::move(other.peer_);
        local_ = std::move(other.local_);
        next_token_ = other.next_token_;
        last_transferred_ = other.last_transferred_;
        last_error_ = other.last_error_;
        other.fd_ = -1;
        other.next_token_ = 0;
        other.last_transferred_ = 0;
        other.last_error_ = 0;
    }
    return *this;
}

Result<StreamSocket> StreamSocket::Open(const Endpoint& remote,
                                        const DialOptions& options) {
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    int resolve_err = 0;
    if (!ResolveEndpoint(remote, SOCK_STREAM, false, &addr, &addr_len, &resolve_err)) {
        return Result<StreamSocket>::Err(IoStatus::kInvalid, resolve_err);
    }
    const int fd = ::socket(addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return Result<StreamSocket>::Err(IoStatus::kIoError, errno);
    }
    if (options.tcp_no_delay && !SetIntOption(fd, IPPROTO_TCP, TCP_NODELAY, 1)) {
        const int err = errno;
        ::close(fd);
        return Result<StreamSocket>::Err(IoStatus::kIoError, err);
    }
    if (options.recv_buffer_bytes != 0 &&
        !SetIntOption(fd, SOL_SOCKET, SO_RCVBUF,
                      static_cast<int>(options.recv_buffer_bytes))) {
        const int err = errno;
        ::close(fd);
        return Result<StreamSocket>::Err(IoStatus::kIoError, err);
    }
    if (options.send_buffer_bytes != 0 &&
        !SetIntOption(fd, SOL_SOCKET, SO_SNDBUF,
                      static_cast<int>(options.send_buffer_bytes))) {
        const int err = errno;
        ::close(fd);
        return Result<StreamSocket>::Err(IoStatus::kIoError, err);
    }
    if (!SetNonBlocking(fd)) {
        const int err = errno;
        ::close(fd);
        return Result<StreamSocket>::Err(IoStatus::kIoError, err);
    }

    int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), addr_len);
    if (rc != 0 && errno != EINPROGRESS) {
        const int err = errno;
        ::close(fd);
        return Result<StreamSocket>::Err(MapConnectErrno(err), err);
    }
    if (rc != 0) {
        // 非阻塞 connect 进行中：等 POLLOUT + 查 SO_ERROR，带 connect_timeout。
        std::chrono::milliseconds budget = options.connect_timeout;
        if (budget.count() <= 0) {
            budget = std::chrono::milliseconds(1);
        }
        const auto deadline = std::chrono::steady_clock::now() + budget;
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                ::close(fd);
                return Result<StreamSocket>::Err(IoStatus::kTimeout, ETIMEDOUT);
            }
            const auto remain =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const int timeout_ms =
                remain.count() > 0 ? static_cast<int>(remain.count()) : 1;
            const int pr = ::poll(&pfd, 1, timeout_ms);
            if (pr == 0) {
                ::close(fd);
                return Result<StreamSocket>::Err(IoStatus::kTimeout, ETIMEDOUT);
            }
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                const int err = errno;
                ::close(fd);
                return Result<StreamSocket>::Err(IoStatus::kIoError, err);
            }
            break;
        }
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0) {
            const int err = errno;
            ::close(fd);
            return Result<StreamSocket>::Err(IoStatus::kIoError, err);
        }
        if (so_error != 0) {
            ::close(fd);
            return Result<StreamSocket>::Err(MapConnectErrno(so_error), so_error);
        }
    }

    sockaddr_storage bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
        const int err = errno;
        ::close(fd);
        return Result<StreamSocket>::Err(IoStatus::kIoError, err);
    }
    StreamSocket sock;
    sock.fd_ = fd;
    sock.peer_ = EndpointFromSockaddr(reinterpret_cast<const sockaddr*>(&addr), addr_len);
    sock.local_ =
        EndpointFromSockaddr(reinterpret_cast<const sockaddr*>(&bound), bound_len);
    return Result<StreamSocket>::Ok(std::move(sock));
}

IoStatus StreamSocket::Send(ByteView data, SendResult* out) {
    SendResult result;
    result.token = ++next_token_;
    result.status = IoStatus::kInvalid;
    last_transferred_ = 0;
    last_error_ = 0;
    if (out != nullptr) {
        *out = result;
    }
    if (!valid() || (data.data == nullptr && data.size != 0) ||
        data.size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        last_error_ = EINVAL;
        if (out != nullptr) {
            out->status = IoStatus::kInvalid;
        }
        return IoStatus::kInvalid;
    }
    for (;;) {
        const ssize_t n = ::send(fd_, data.data, data.size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n >= 0) {
            last_transferred_ = static_cast<std::size_t>(n);
            result.status = IoStatus::kOk;
            if (out != nullptr) {
                *out = result;
            }
            return IoStatus::kOk;
        }
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        last_error_ = err;
        result.status = MapSendErrno(err);
        if (out != nullptr) {
            *out = result;
        }
        return result.status;
    }
}

IoStatus StreamSocket::Recv(MutableByteView buf, std::size_t* received) {
    if (received != nullptr) {
        *received = 0;
    }
    last_transferred_ = 0;
    last_error_ = 0;
    if (!valid() || buf.data == nullptr || buf.size == 0) {
        last_error_ = EINVAL;
        return IoStatus::kInvalid;
    }
    for (;;) {
        const ssize_t n = ::recv(fd_, buf.data, buf.size, MSG_DONTWAIT);
        if (n > 0) {
            last_transferred_ = static_cast<std::size_t>(n);
            if (received != nullptr) {
                *received = static_cast<std::size_t>(n);
            }
            return IoStatus::kOk;
        }
        if (n == 0) {
            // 有序关闭：对端发了 FIN，之后不会再有数据。
            return IoStatus::kClosed;
        }
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        last_error_ = err;
        if (err == EAGAIN) {
            last_error_ = 0;
        }
        return MapRecvErrno(err);
    }
}

void StreamSocket::Close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    next_token_ = 0;
    last_transferred_ = 0;
}

}  // namespace rd::net

#else  // _WIN32：只保留接口占位，不引入 Windows 头，由 JinShan 侧适配 WSA。

namespace rd::net {

const char* IoStatusName(IoStatus) noexcept { return "kInvalid"; }

DatagramSocket::~DatagramSocket() = default;
DatagramSocket::DatagramSocket(DatagramSocket&&) noexcept = default;
DatagramSocket& DatagramSocket::operator=(DatagramSocket&&) noexcept = default;
Result<DatagramSocket> DatagramSocket::Open(const Endpoint&, const SocketOptions&) {
    return Result<DatagramSocket>::Err(IoStatus::kInvalid, 0);
}
IoStatus DatagramSocket::SendTo(ByteView, const Endpoint&, SendResult* out) {
    if (out != nullptr) {
        *out = SendResult{};
    }
    return IoStatus::kInvalid;
}
IoStatus DatagramSocket::RecvFrom(MutableByteView, Endpoint*, std::size_t* received) {
    if (received != nullptr) {
        *received = 0;
    }
    return IoStatus::kInvalid;
}
void DatagramSocket::Close() noexcept {}

StreamSocket::~StreamSocket() = default;
StreamSocket::StreamSocket(StreamSocket&&) noexcept = default;
StreamSocket& StreamSocket::operator=(StreamSocket&&) noexcept = default;
Result<StreamSocket> StreamSocket::Open(const Endpoint&, const DialOptions&) {
    return Result<StreamSocket>::Err(IoStatus::kInvalid, 0);
}
IoStatus StreamSocket::Send(ByteView, SendResult* out) {
    if (out != nullptr) {
        *out = SendResult{};
    }
    return IoStatus::kInvalid;
}
IoStatus StreamSocket::Recv(MutableByteView, std::size_t* received) {
    if (received != nullptr) {
        *received = 0;
    }
    return IoStatus::kInvalid;
}
void StreamSocket::Close() noexcept {}

}  // namespace rd::net

#endif  // _WIN32
