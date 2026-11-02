/**
 * @file net.cpp
 * @brief 隧道模块的 TCP 层与 `relay::Stream` 适配实现。
 *
 * @details 关键取舍：
 *
 * 1. **非阻塞 fd + 显式 `poll`**：不依赖任何运行时调度器，超时可精确控制，
 *    `shutdown(2)` 能立即唤醒等待者。
 * 2. **`Shutdown()` 不关 fd**：fd 只在析构时 `close`，杜绝「读线程仍握着旧 fd、
 *    新连接复用了同一 fd」的 ABA 竞态。代价是调用方必须保证析构发生在所有
 *    使用线程退出之后——RelayPipe::Run 返回即满足。
 * 3. **超时不算错误**：`SocketStream::Read` 内部把 `ETIMEDOUT` 当作「暂无数据」
 *    继续等待，避免 HTTP keep-alive 的空闲期被 RelayPipe 的写 deadline 误判。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include "go2cpp/tunnel/net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace go2cpp {
namespace tunnel {
namespace {

constexpr int kMaxDialAttempts = 4;

/// 把点分 IPv4 / IPv6 字面量解析进 `sockaddr_storage`；成功返回 family。
int ParseLiteral(const std::string& host, std::uint16_t port,
                 sockaddr_storage* storage, socklen_t* length) {
    sockaddr_in v4{};
    if (::inet_pton(AF_INET, host.c_str(), &v4.sin_addr) == 1) {
        v4.sin_family = AF_INET;
        v4.sin_port = htons(port);
        std::memcpy(storage, &v4, sizeof(v4));
        *length = sizeof(v4);
        return AF_INET;
    }
    sockaddr_in6 v6{};
    if (::inet_pton(AF_INET6, host.c_str(), &v6.sin6_addr) == 1) {
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(port);
        std::memcpy(storage, &v6, sizeof(v6));
        *length = sizeof(v6);
        return AF_INET6;
    }
    return AF_UNSPEC;
}

/// 解析 `host:port`；返回 0 表示成功。
int Resolve(const std::string& host, std::uint16_t port, bool passive,
            int sock_type, sockaddr_storage* storage, socklen_t* length,
            std::string* err) {
    const int literal = ParseLiteral(host, port, storage, length);
    if (literal != AF_UNSPEC) {
        return 0;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = sock_type;
    if (passive) {
        hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
    } else {
        hints.ai_flags = AI_ADDRCONFIG;
    }
    const std::string service = std::to_string(port);
    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(passive && host.empty() ? nullptr : host.c_str(),
                                 service.c_str(), &hints, &result);
    if (rc != 0) {
        if (err != nullptr) {
            *err = "resolve " + host + ": " + ::gai_strerror(rc);
        }
        return -1;
    }
    int status = -1;
    for (addrinfo* current = result; current != nullptr;
         current = current->ai_next) {
        if (current->ai_addrlen > sizeof(sockaddr_storage)) {
            continue;
        }
        std::memcpy(storage, current->ai_addr, current->ai_addrlen);
        *length = static_cast<socklen_t>(current->ai_addrlen);
        status = 0;
        break;
    }
    ::freeaddrinfo(result);
    if (status != 0 && err != nullptr) {
        *err = "resolve " + host + ": no usable address";
    }
    return status;
}

/// 设置非阻塞、CLOEXEC、TCP_NODELAY、SO_KEEPALIVE。
bool ConfigureFd(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    const int fd_flags = ::fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0) {
        (void)::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
    int enable = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
    (void)::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
    return true;
}

/// 地址转 `ip:port` 文本。
std::string FormatSockaddr(const sockaddr* address, socklen_t length) {
    if (address == nullptr) {
        return {};
    }
    char host[INET6_ADDRSTRLEN] = {};
    std::uint16_t port = 0;
    if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        ::inet_ntop(AF_INET, &v4->sin_addr, host, sizeof(host));
        port = ntohs(v4->sin_port);
    } else if (address->sa_family == AF_INET6 &&
               length >= sizeof(sockaddr_in6)) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
        ::inet_ntop(AF_INET6, &v6->sin6_addr, host, sizeof(host));
        port = ntohs(v6->sin6_port);
    } else {
        return {};
    }
    return std::string(host) + ":" + std::to_string(port);
}

}  // namespace

// ============================================================================
// TcpConn
// ============================================================================

int TcpConn::PollFd(int fd, short events, int timeout_ms) {
    if (timeout_ms < 0) {
        for (;;) {
            pollfd entry{fd, events, 0};
            const int rc = ::poll(&entry, 1, -1);
            if (rc < 0 && errno == EINTR) {
                continue;
            }
            if (rc == 0) {
                return ETIMEDOUT;
            }
            if (rc < 0) {
                return errno;
            }
            return 0;
        }
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return ETIMEDOUT;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - now)
                                   .count();
        pollfd entry{fd, events, 0};
        const int rc = ::poll(&entry, 1, static_cast<int>(remaining));
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc == 0) {
            return ETIMEDOUT;
        }
        if (rc < 0) {
            return errno;
        }
        return 0;
    }
}

TcpConn::Ptr TcpConn::Dial(const std::string& host, std::uint16_t port,
                           int timeout_ms, std::string* err) {
    sockaddr_storage storage{};
    socklen_t storage_length = 0;
    if (Resolve(host, port, false, SOCK_STREAM, &storage, &storage_length,
                err) != 0) {
        return nullptr;
    }
    const int fd = ::socket(storage.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err != nullptr) {
            *err = "socket: " + std::string(std::strerror(errno));
        }
        return nullptr;
    }
    if (!ConfigureFd(fd)) {
        if (err != nullptr) {
            *err = "configure: " + std::string(std::strerror(errno));
        }
        ::close(fd);
        return nullptr;
    }
    const int connect_rc =
        ::connect(fd, reinterpret_cast<const sockaddr*>(&storage),
                  storage_length);
    if (connect_rc != 0 && errno != EINPROGRESS) {
        if (err != nullptr) {
            *err = "connect " + host + ":" + std::to_string(port) + ": " +
                   std::strerror(errno);
        }
        ::close(fd);
        return nullptr;
    }
    if (connect_rc != 0) {
        const int poll_error = PollFd(fd, POLLOUT, timeout_ms);
        if (poll_error != 0) {
            if (err != nullptr) {
                *err = "connect timeout " + host + ":" + std::to_string(port);
            }
            ::close(fd);
            return nullptr;
        }
        int pending = 0;
        socklen_t pending_length = sizeof(pending);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &pending, &pending_length) <
                0 ||
            pending != 0) {
            if (err != nullptr) {
                *err = "connect " + host + ":" + std::to_string(port) + ": " +
                       std::strerror(pending != 0 ? pending : errno);
            }
            ::close(fd);
            return nullptr;
        }
    }
    return Ptr(new TcpConn(fd));
}

TcpConn::Ptr TcpConn::FromFd(int fd) {
    if (fd < 0) {
        return nullptr;
    }
    if (!ConfigureFd(fd)) {
        ::close(fd);
        return nullptr;
    }
    return Ptr(new TcpConn(fd));
}

TcpConn::~TcpConn() {
    Shutdown();
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

IoResult TcpConn::Read(void* data, std::size_t length, int timeout_ms) {
    IoResult result;
    if (data == nullptr && length != 0) {
        result.error = EINVAL;
        return result;
    }
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t offset = 0;
    const auto deadline = timeout_ms < 0
                              ? std::chrono::steady_clock::time_point::max()
                              : std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (m_closed.load(std::memory_order_acquire)) {
            result.error = ECANCELED;
            result.count = offset;
            return result;
        }
        const int wait_ms =
            timeout_ms < 0 ? -1
                           : static_cast<int>(std::max<std::int64_t>(
                                 0, std::chrono::duration_cast<
                                        std::chrono::milliseconds>(
                                        deadline - std::chrono::steady_clock::now())
                                        .count()));
        const int poll_error = PollFd(m_fd, POLLIN, wait_ms);
        if (poll_error == ETIMEDOUT) {
            result.error = ETIMEDOUT;
            result.count = offset;
            return result;
        }
        if (poll_error != 0) {
            result.error = poll_error;
            result.count = offset;
            return result;
        }
        if (m_closed.load(std::memory_order_acquire)) {
            result.error = ECANCELED;
            result.count = offset;
            return result;
        }
        const ssize_t received =
            ::recv(m_fd, bytes + offset, length - offset, 0);
        if (received > 0) {
            result.count = offset + static_cast<std::size_t>(received);
            result.error = 0;
            return result;
        }
        if (received == 0) {
            result.count = offset;
            result.eof = true;
            return result;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        if (errno == ECONNRESET || errno == EPIPE) {
            // 对端重置按 EOF 处理：HTTP 场景下它等价于「连接没了」，
            // 让上层走正常收尾，而不是把整条隧道流标成故障。
            result.count = offset;
            result.eof = true;
            return result;
        }
        result.error = errno;
        result.count = offset;
        return result;
    }
}

bool TcpConn::WriteAll(const void* data, std::size_t length, int timeout_ms,
                       std::string* err) {
    if (data == nullptr && length != 0) {
        if (err != nullptr) {
            *err = "write: null data";
        }
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    std::lock_guard<std::mutex> lock(m_write_mutex);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(0, timeout_ms));
    while (offset < length) {
        if (m_closed.load(std::memory_order_acquire)) {
            if (err != nullptr) {
                *err = "write: connection closed";
            }
            return false;
        }
        const ssize_t sent =
            ::send(m_fd, bytes + offset, length - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (err != nullptr) {
                *err = "write: " + std::string(std::strerror(errno));
            }
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            if (err != nullptr) {
                *err = "write: timeout";
            }
            return false;
        }
        const int wait_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count());
        const int poll_error = PollFd(m_fd, POLLOUT, std::max(1, wait_ms));
        if (poll_error != 0) {
            if (err != nullptr) {
                *err = "write: " + std::string(std::strerror(poll_error));
            }
            return false;
        }
    }
    return true;
}

void TcpConn::Shutdown() noexcept {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (m_fd >= 0) {
        (void)::shutdown(m_fd, SHUT_RDWR);
    }
}

std::string TcpConn::PeerText() const {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getpeername(m_fd, reinterpret_cast<sockaddr*>(&storage), &length) !=
        0) {
        return {};
    }
    return FormatSockaddr(reinterpret_cast<const sockaddr*>(&storage), length);
}

std::string TcpConn::LocalText() const {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&storage), &length) !=
        0) {
        return {};
    }
    return FormatSockaddr(reinterpret_cast<const sockaddr*>(&storage), length);
}

// ============================================================================
// TcpListener
// ============================================================================

TcpListener::Ptr TcpListener::Listen(const std::string& host,
                                     std::uint16_t port, std::string* err,
                                     int backlog) {
    sockaddr_storage storage{};
    socklen_t storage_length = 0;
    if (Resolve(host, port, true, SOCK_STREAM, &storage, &storage_length, err) !=
        0) {
        return nullptr;
    }
    const int fd = ::socket(storage.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err != nullptr) {
            *err = "socket: " + std::string(std::strerror(errno));
        }
        return nullptr;
    }
    int enable = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    if (!ConfigureFd(fd) ||
        ::bind(fd, reinterpret_cast<const sockaddr*>(&storage), storage_length) !=
            0 ||
        ::listen(fd, backlog) != 0) {
        if (err != nullptr) {
            *err = "listen " + host + ":" + std::to_string(port) + ": " +
                   std::strerror(errno);
        }
        ::close(fd);
        return nullptr;
    }
    sockaddr_storage bound{};
    socklen_t bound_length = sizeof(bound);
    std::uint16_t bound_port = port;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_length) ==
        0) {
        if (bound.ss_family == AF_INET) {
            bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
        } else if (bound.ss_family == AF_INET6) {
            bound_port =
                ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
        }
    }
    return Ptr(new TcpListener(fd, bound_port));
}

TcpListener::~TcpListener() {
    Close();
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

TcpConn::Ptr TcpListener::Accept(int timeout_ms) {
    for (;;) {
        if (m_closed.load(std::memory_order_acquire)) {
            return nullptr;
        }
        const int poll_error =
            TcpConn::PollFd(m_fd, POLLIN, std::max(0, timeout_ms));
        if (poll_error != 0) {
            return nullptr;
        }
        if (m_closed.load(std::memory_order_acquire)) {
            return nullptr;
        }
        sockaddr_storage storage{};
        socklen_t length = sizeof(storage);
        const int accepted =
            ::accept4(m_fd, reinterpret_cast<sockaddr*>(&storage), &length,
                      SOCK_CLOEXEC);
        if (accepted < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return nullptr;
        }
        TcpConn::Ptr conn = TcpConn::FromFd(accepted);
        if (conn) {
            return conn;
        }
    }
}

void TcpListener::Close() noexcept {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (m_fd >= 0) {
        (void)::shutdown(m_fd, SHUT_RDWR);
    }
}

// ============================================================================
// SocketStream
// ============================================================================

SocketStream::SocketStream(TcpConn::Ptr conn, int read_timeout_ms)
    : m_conn(std::move(conn)), m_read_timeout_ms(read_timeout_ms) {}

streams::ReadResult SocketStream::Read(char* data, std::size_t size) {
    if (!m_conn) {
        return {0, NewError("tunnel: null connection")};
    }
    for (;;) {
        const IoResult result = m_conn->Read(data, size, m_read_timeout_ms);
        if (result.ok()) {
            return {result.count, nullptr};
        }
        if (result.eof) {
            return {0, streams::IoEof()};
        }
        if (result.error == ETIMEDOUT) {
            // 空闲不是失败：继续等，直到数据、EOF 或 Close()。
            continue;
        }
        if (result.error == ECANCELED) {
            return {0, NewError("tunnel: stream closed")};
        }
        return {0, NewError("tunnel: read: " +
                            std::string(std::strerror(result.error)))};
    }
}

streams::WriteResult SocketStream::Write(const char* data, std::size_t size) {
    if (!m_conn) {
        return {0, NewError("tunnel: null connection")};
    }
    std::string error;
    if (!m_conn->WriteAll(data, size, 30000, &error)) {
        return {0, NewError("tunnel: " + error)};
    }
    return {size, nullptr};
}

ErrorPtr SocketStream::Close() {
    if (m_conn) {
        m_conn->Shutdown();
    }
    return nullptr;
}

// ============================================================================
// PrefixedStream
// ============================================================================

PrefixedStream::PrefixedStream(relay::Stream* inner, std::string prefix)
    : m_inner(inner), m_prefix(std::move(prefix)) {}

streams::ReadResult PrefixedStream::Read(char* data, std::size_t size) {
    if (m_offset < m_prefix.size()) {
        const std::size_t take = std::min(size, m_prefix.size() - m_offset);
        std::memcpy(data, m_prefix.data() + m_offset, take);
        m_offset += take;
        if (m_offset == m_prefix.size()) {
            m_prefix.clear();
            m_prefix.shrink_to_fit();
            m_offset = 0;
        }
        return {take, nullptr};
    }
    return m_inner->Read(data, size);
}

streams::WriteResult PrefixedStream::Write(const char* data, std::size_t size) {
    return m_inner->Write(data, size);
}

ErrorPtr PrefixedStream::Close() { return m_inner->Close(); }

// ============================================================================
// EofClosingStream
// ============================================================================

EofClosingStream::EofClosingStream(relay::Stream* inner, relay::Stream* peer)
    : m_inner(inner), m_peer(peer) {}

streams::ReadResult EofClosingStream::Read(char* data, std::size_t size) {
    streams::ReadResult result = m_inner->Read(data, size);
    if (result.count == 0 && result.error && Is(result.error, streams::IoEof())) {
        // 本端读到 EOF：让对端也跟着结束，否则 RelayPipe 的另一方向永远等不到
        // 数据、Run() 不返回（HTTP 连接关闭时的常见泄漏）。
        if (m_peer != nullptr) {
            (void)m_peer->Close();
        }
    }
    return result;
}

streams::WriteResult EofClosingStream::Write(const char* data, std::size_t size) {
    return m_inner->Write(data, size);
}

ErrorPtr EofClosingStream::Close() { return m_inner->Close(); }

}  // namespace tunnel
}  // namespace go2cpp
