/**
 * @file tcp_transport.cpp
 * @brief `TcpTransport` 实现：非阻塞 connect/accept、poll deadline、EINTR/EAGAIN 循环。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/tcp_transport.hpp"

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

/// 负超时按 0 处理。
std::chrono::milliseconds NormalizeTimeout(std::chrono::milliseconds timeout) noexcept {
    return timeout.count() < 0 ? std::chrono::milliseconds(0) : timeout;
}

/**
 * @brief 解析端点。
 * @param passive true 用于监听（允许空 host 通配、端口 0）。
 * @param[out] addresses 解析出的地址列表（按 getaddrinfo 顺序）。
 * @param[out] error 失败原因（errno 风格）。
 * @return 至少拿到一个地址时 true。
 *
 * @note 只保留 `sockaddr_storage` 能容纳的地址；AF_UNIX 等类型被过滤。
 * @note DNS 解析不受调用方 timeout 约束，这是 getaddrinfo 的固有限制；
 *       需要硬上界时传 IP 字面量。
 */
bool Resolve(const Endpoint& endpoint, bool passive, std::vector<AddrEntry>* addresses,
             int* error) {
    addresses->clear();
    if (!passive && endpoint.host.empty()) {
        *error = EINVAL;
        return false;
    }
    if (!passive && endpoint.port == 0) {
        *error = EINVAL;
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
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

enum class WaitStatus : std::uint8_t {
    kReady = 0,
    kTimeout = 1,
    kError = 2,
};

/**
 * @brief 等待 fd 的指定事件就绪，严格受 deadline 约束。
 *
 * @details `timeout == 0` 时仍会执行一次 `poll(..., 0)`：这样非阻塞调用
 *          可以先消费内核里已经就绪的数据，而不是直接报超时。
 *          `EINTR` 不视为错误：重新计算剩余时间后继续等待。
 *
 * @param[out] error 超时写 `ETIMEDOUT`，poll 失败写 errno。
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

}  // namespace

TcpTransport::~TcpTransport() {
    (void)Close();
}

Result TcpTransport::Connect(const Endpoint& endpoint, std::chrono::milliseconds timeout) {
    if (m_fd >= 0) {
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
    for (const AddrEntry& entry : addresses) {
        const int fd = ::socket(entry.storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            last_error = errno;
            continue;
        }
        if (!SetNonBlocking(fd)) {
            last_error = errno;
            (void)::close(fd);
            continue;
        }

        const int rc =
            ::connect(fd, reinterpret_cast<const sockaddr*>(&entry.storage), entry.length);
        if (rc != 0) {
            const int connect_error = errno;
            // EINPROGRESS：连接异步进行；EINTR：被信号中断但连接仍在进行。
            // 两者都继续走 poll(POLLOUT) + SO_ERROR，不能重新 connect。
            if (connect_error != EINPROGRESS && connect_error != EWOULDBLOCK &&
                connect_error != EINTR) {
                last_error = connect_error;
                (void)::close(fd);
                continue;
            }

            int wait_error = 0;
            const WaitStatus status = WaitFor(fd, POLLOUT, deadline, &wait_error);
            if (status == WaitStatus::kTimeout) {
                (void)::close(fd);
                return Result{Code::kTimeout, ETIMEDOUT, 0};
            }
            if (status == WaitStatus::kError) {
                last_error = wait_error;
                (void)::close(fd);
                continue;
            }

            int so_error = 0;
            socklen_t so_length = sizeof(so_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_length) != 0) {
                last_error = errno;
                (void)::close(fd);
                continue;
            }
            if (so_error != 0) {
                last_error = so_error;
                (void)::close(fd);
                continue;
            }
        }

        m_fd = fd;
        m_listening = false;
        m_write_shutdown = false;
        return Result{Code::kOk, 0, 0};
    }

    return Result{Code::kError, last_error, 0};
}

Result TcpTransport::Listen(const Endpoint& bind_endpoint, int backlog) {
    if (m_fd >= 0) {
        return Result{Code::kError, EALREADY, 0};
    }
    if (backlog <= 0) {
        backlog = 64;
    }

    std::vector<AddrEntry> addresses;
    int resolve_error = 0;
    if (!Resolve(bind_endpoint, true, &addresses, &resolve_error)) {
        return Result{Code::kError, resolve_error, 0};
    }

    int last_error = EADDRINUSE;
    for (const AddrEntry& entry : addresses) {
        const int fd = ::socket(entry.storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            last_error = errno;
            continue;
        }
        if (!SetNonBlocking(fd)) {
            last_error = errno;
            (void)::close(fd);
            continue;
        }

        int reuse = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (entry.storage.ss_family == AF_INET6) {
            // 双栈监听：绑定 :: 时同时接受 IPv4 映射地址，部署更省事。
            int v6_only = 0;
            (void)::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only));
        }

        if (::bind(fd, reinterpret_cast<const sockaddr*>(&entry.storage), entry.length) != 0) {
            last_error = errno;
            (void)::close(fd);
            continue;
        }
        if (::listen(fd, backlog) != 0) {
            last_error = errno;
            (void)::close(fd);
            continue;
        }

        m_fd = fd;
        m_listening = true;
        m_write_shutdown = false;
        return Result{Code::kOk, 0, 0};
    }

    return Result{Code::kError, last_error, 0};
}

Result TcpTransport::Accept(TcpTransport* accepted, std::chrono::milliseconds timeout) {
    if (accepted == nullptr || accepted == this) {
        return Result{Code::kError, EINVAL, 0};
    }
    if (accepted->m_fd >= 0) {
        return Result{Code::kError, EALREADY, 0};
    }
    if (m_fd < 0 || !m_listening) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }

    const Clock::time_point deadline = Clock::now() + NormalizeTimeout(timeout);
    for (;;) {
        int wait_error = 0;
        const WaitStatus status = WaitFor(m_fd, POLLIN, deadline, &wait_error);
        if (status == WaitStatus::kTimeout) {
            return Result{Code::kTimeout, ETIMEDOUT, 0};
        }
        if (status == WaitStatus::kError) {
            return MapIoError(wait_error, 0);
        }

        const int fd = ::accept(m_fd, nullptr, nullptr);
        if (fd >= 0) {
            if (!SetNonBlocking(fd)) {
                const int error = errno;
                (void)::close(fd);
                return Result{Code::kError, error, 0};
            }
            return accepted->Adopt(fd);
        }

        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        return MapIoError(errno, 0);
    }
}

Result TcpTransport::Read(std::uint8_t* buffer, std::size_t capacity,
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

    const Clock::time_point deadline = Clock::now() + NormalizeTimeout(timeout);
    for (;;) {
        int wait_error = 0;
        const WaitStatus status = WaitFor(m_fd, POLLIN, deadline, &wait_error);
        if (status == WaitStatus::kTimeout) {
            return Result{Code::kTimeout, ETIMEDOUT, 0};
        }
        if (status == WaitStatus::kError) {
            if (m_fd < 0) {
                return Result{Code::kClosed, ENOTCONN, 0};
            }
            return MapIoError(wait_error, 0);
        }

        const ssize_t received = ::recv(m_fd, buffer, capacity, 0);
        if (received > 0) {
            return Result{Code::kOk, 0, static_cast<std::size_t>(received)};
        }
        if (received == 0) {
            return Result{Code::kClosed, 0, 0};
        }
        const int error = errno;
        if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) {
            continue;
        }
        return MapIoError(error, 0);
    }
}

Result TcpTransport::Write(const std::uint8_t* data, std::size_t size,
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

    const Clock::time_point deadline = Clock::now() + NormalizeTimeout(timeout);
    std::size_t written = 0;
    while (written < size) {
        const ssize_t sent =
            ::send(m_fd, data + written, size - written, MSG_NOSIGNAL);
        if (sent > 0) {
            written += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == 0) {
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
                return Result{Code::kTimeout, ETIMEDOUT, written};
            }
            if (status == WaitStatus::kError) {
                if (m_fd < 0) {
                    return Result{Code::kClosed, ENOTCONN, written};
                }
                return MapIoError(wait_error, written);
            }
            continue;
        }
        return MapIoError(error, written);
    }
    return Result{Code::kOk, 0, written};
}

Result TcpTransport::ShutdownWrite() {
    if (m_fd < 0) {
        return Result{Code::kClosed, ENOTCONN, 0};
    }
    if (m_listening) {
        return Result{Code::kError, ENOTCONN, 0};
    }
    if (m_write_shutdown) {
        return Result{Code::kOk, 0, 0};
    }

    if (::shutdown(m_fd, SHUT_WR) != 0) {
        // 对端已经关闭时 shutdown 可能返回 ENOTCONN/EPIPE；此时写方向本来
        // 就不可用，按「已经半关闭」处理，保证幂等。
        if (errno == ENOTCONN || errno == EPIPE) {
            m_write_shutdown = true;
            return Result{Code::kOk, 0, 0};
        }
        return MapIoError(errno, 0);
    }
    m_write_shutdown = true;
    return Result{Code::kOk, 0, 0};
}

Result TcpTransport::Close() {
    if (m_fd >= 0) {
        (void)::close(m_fd);
        m_fd = -1;
    }
    m_listening = false;
    m_write_shutdown = false;
    return Result{Code::kOk, 0, 0};
}

Result TcpTransport::LocalEndpoint(Endpoint* endpoint) const {
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

    char host[NI_MAXHOST]{};
    char service[NI_MAXSERV]{};
    const int rc = ::getnameinfo(reinterpret_cast<const sockaddr*>(&storage), length, host,
                                 sizeof(host), service, sizeof(service),
                                 NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) {
        return Result{Code::kError, EINVAL, 0};
    }

    endpoint->host = host;
    endpoint->port = static_cast<std::uint16_t>(std::strtoul(service, nullptr, 10));
    return Result{Code::kOk, 0, 0};
}

Result TcpTransport::Adopt(int fd) noexcept {
    if (m_fd >= 0) {
        (void)::close(fd);
        return Result{Code::kError, EALREADY, 0};
    }
    m_fd = fd;
    m_listening = false;
    m_write_shutdown = false;
    return Result{Code::kOk, 0, 0};
}

}  // namespace go2cpp::bento::net
