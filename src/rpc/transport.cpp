/**
 * @file transport.cpp
 * @brief TCP 传输/监听实现（POSIX：getaddrinfo + 非阻塞 connect + poll）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/rpc/transport.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>

namespace go2cpp::rpc {
namespace {

using Clock = std::chrono::steady_clock;

/// @brief 打开 close-on-exec（失败不致命，只是不继承到子进程）。
void SetCloseOnExec(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

/// @brief 设置阻塞/非阻塞。
bool SetNonBlocking(int fd, bool non_blocking) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int updated = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, updated) == 0;
}

/// @brief 开启 TCP_NODELAY（RPC 小消息延迟的关键）。
void SetNoDelay(int fd) noexcept {
    const int enable = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
}

/// @brief 计算剩余超时毫秒（至少 0）。
int RemainingMillis(Clock::time_point deadline) noexcept {
    const auto now = Clock::now();
    if (now >= deadline) {
        return 0;
    }
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

/// @brief 端口文本。
std::string PortText(std::uint16_t port) { return std::to_string(static_cast<unsigned>(port)); }

}  // namespace

bool Endpoint::Parse(std::string_view text, Endpoint* out) {
    if (out == nullptr || text.empty()) {
        return false;
    }
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    const std::string_view host = text.substr(0, colon);
    const std::string_view port_text = text.substr(colon + 1);
    if (host.find('[') != std::string_view::npos || host.find(']') != std::string_view::npos) {
        return false;  // IPv6 字面量暂不支持。
    }
    if (port_text.empty() || port_text.size() > 5) {
        return false;
    }
    unsigned value = 0;
    for (const char character : port_text) {
        if (character < '0' || character > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(character - '0');
    }
    if (value > 65535) {
        return false;
    }
    out->host.assign(host);
    out->port = static_cast<std::uint16_t>(value);
    return true;
}

std::string Endpoint::ToString() const {
    std::string text;
    text.reserve(host.size() + 6);
    text += host;
    text += ':';
    text += PortText(port);
    return text;
}

Status SocketErrorStatus(const char* action, int error_number) {
    std::string message(action != nullptr ? action : "socket");
    message += ": ";
    message += std::strerror(error_number);
    return Status(StatusCode::kUnavailable, std::move(message));
}

// ---------------------------------------------------------------------------
// TcpTransport
// ---------------------------------------------------------------------------

std::unique_ptr<TcpTransport> TcpTransport::Connect(const Endpoint& endpoint,
                                                    std::chrono::milliseconds timeout,
                                                    Status* status) {
    const std::string host = endpoint.host.empty() ? "127.0.0.1" : endpoint.host;
    const std::string port = PortText(endpoint.port);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const int lookup = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
    if (lookup != 0) {
        if (status != nullptr) {
            *status = Status(StatusCode::kInvalidArgument,
                             std::string("getaddrinfo: ") + ::gai_strerror(lookup));
        }
        return nullptr;
    }

    const auto deadline = Clock::now() + timeout;
    Status last_error = Status(StatusCode::kUnavailable, "no usable address");
    std::unique_ptr<TcpTransport> transport;
    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        const int fd = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) {
            last_error = SocketErrorStatus("socket", errno);
            continue;
        }
        SetCloseOnExec(fd);
        if (!SetNonBlocking(fd, true)) {
            last_error = SocketErrorStatus("fcntl(O_NONBLOCK)", errno);
            ::close(fd);
            continue;
        }
        bool connected = false;
        const int rc = ::connect(fd, entry->ai_addr, entry->ai_addrlen);
        if (rc == 0) {
            connected = true;
        } else if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
            pollfd poll_fd{};
            poll_fd.fd = fd;
            poll_fd.events = POLLOUT;
            int poll_rc = 0;
            do {
                poll_rc = ::poll(&poll_fd, 1, RemainingMillis(deadline));
            } while (poll_rc < 0 && errno == EINTR);
            if (poll_rc == 0) {
                last_error = Status(StatusCode::kDeadlineExceeded, "connect timed out");
            } else if (poll_rc < 0) {
                last_error = SocketErrorStatus("poll", errno);
            } else {
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 &&
                    socket_error == 0) {
                    connected = true;
                } else {
                    last_error = SocketErrorStatus("connect", socket_error != 0 ? socket_error
                                                                               : errno);
                }
            }
        } else {
            last_error = SocketErrorStatus("connect", errno);
        }
        if (!connected) {
            ::close(fd);
            if (Clock::now() >= deadline) {
                break;  // 超时后不再尝试其他地址。
            }
            continue;
        }
        if (!SetNonBlocking(fd, false)) {
            last_error = SocketErrorStatus("fcntl(blocking)", errno);
            ::close(fd);
            continue;
        }
        SetNoDelay(fd);
        transport = std::unique_ptr<TcpTransport>(new TcpTransport(fd));
        break;
    }
    ::freeaddrinfo(results);

    if (!transport && status != nullptr) {
        *status = std::move(last_error);
    } else if (status != nullptr) {
        *status = Status::Ok();
    }
    return transport;
}

TcpTransport::TcpTransport(int fd) noexcept : m_fd(fd) {
    if (m_fd >= 0) {
        SetNoDelay(m_fd);
    }
}

TcpTransport::~TcpTransport() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool TcpTransport::WriteAll(const void* data, std::size_t size, Status* status) {
    if (m_closed.load(std::memory_order_acquire)) {
        if (status != nullptr) {
            *status = Status(StatusCode::kUnavailable, "transport closed");
        }
        return false;
    }
    const auto* cursor = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const ssize_t written = ::send(m_fd, cursor, remaining, MSG_NOSIGNAL);
        if (written > 0) {
            cursor += written;
            remaining -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd poll_fd{};
            poll_fd.fd = m_fd;
            poll_fd.events = POLLOUT;
            int rc = 0;
            do {
                rc = ::poll(&poll_fd, 1, 1000);
            } while (rc < 0 && errno == EINTR);
            if (rc > 0) {
                continue;
            }
            if (rc == 0) {
                continue;  // 阻塞 socket 理论上不会走到这里；继续尝试。
            }
        }
        if (status != nullptr) {
            *status = m_closed.load(std::memory_order_acquire)
                          ? Status(StatusCode::kUnavailable, "transport closed")
                          : SocketErrorStatus("send", errno);
        }
        return false;
    }
    if (status != nullptr) {
        *status = Status::Ok();
    }
    return true;
}

std::ptrdiff_t TcpTransport::Read(void* data, std::size_t size, Status* status) {
    for (;;) {
        const ssize_t received = ::recv(m_fd, data, size, 0);
        if (received >= 0) {
            if (status != nullptr) {
                *status = Status::Ok();
            }
            return static_cast<std::ptrdiff_t>(received);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            pollfd poll_fd{};
            poll_fd.fd = m_fd;
            poll_fd.events = POLLIN;
            int rc = 0;
            do {
                rc = ::poll(&poll_fd, 1, 1000);
            } while (rc < 0 && errno == EINTR);
            if (rc >= 0) {
                continue;
            }
        }
        if (status != nullptr) {
            *status = m_closed.load(std::memory_order_acquire)
                          ? Status(StatusCode::kUnavailable, "transport closed")
                          : SocketErrorStatus("recv", errno);
        }
        return -1;
    }
}

void TcpTransport::Close() noexcept {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (m_fd >= 0) {
        (void)::shutdown(m_fd, SHUT_RDWR);
    }
}

// ---------------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------------

std::unique_ptr<TcpListener> TcpListener::Bind(const Endpoint& endpoint, int backlog,
                                               Status* status) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    const std::string port = PortText(endpoint.port);
    const char* host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
    addrinfo* results = nullptr;
    const int lookup = ::getaddrinfo(host, port.c_str(), &hints, &results);
    if (lookup != 0) {
        if (status != nullptr) {
            *status = Status(StatusCode::kInvalidArgument,
                             std::string("getaddrinfo: ") + ::gai_strerror(lookup));
        }
        return nullptr;
    }

    int listen_fd = -1;
    std::uint16_t actual_port = endpoint.port;
    Status last_error = Status(StatusCode::kUnavailable, "no usable address");
    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        const int fd = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) {
            last_error = SocketErrorStatus("socket", errno);
            continue;
        }
        SetCloseOnExec(fd);
        const int enable = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        if (::bind(fd, entry->ai_addr, entry->ai_addrlen) != 0) {
            last_error = SocketErrorStatus("bind", errno);
            ::close(fd);
            continue;
        }
        if (::listen(fd, backlog <= 0 ? 128 : backlog) != 0) {
            last_error = SocketErrorStatus("listen", errno);
            ::close(fd);
            continue;
        }
        sockaddr_in bound{};
        socklen_t bound_length = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
            actual_port = ntohs(bound.sin_port);
        }
        listen_fd = fd;
        break;
    }
    ::freeaddrinfo(results);

    if (listen_fd < 0) {
        if (status != nullptr) {
            *status = std::move(last_error);
        }
        return nullptr;
    }
    if (status != nullptr) {
        *status = Status::Ok();
    }
    return std::unique_ptr<TcpListener>(new TcpListener(listen_fd, actual_port));
}

TcpListener::~TcpListener() {
    Close();
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

std::unique_ptr<TcpTransport> TcpListener::Accept(int timeout_ms, Status* status) {
    if (m_fd < 0 || m_closed.load(std::memory_order_acquire)) {
        if (status != nullptr) {
            *status = Status(StatusCode::kUnavailable, "listener closed");
        }
        return nullptr;
    }
    if (timeout_ms >= 0) {
        pollfd poll_fd{};
        poll_fd.fd = m_fd;
        poll_fd.events = POLLIN;
        int rc = 0;
        do {
            rc = ::poll(&poll_fd, 1, timeout_ms);
        } while (rc < 0 && errno == EINTR);
        if (rc == 0) {
            if (status != nullptr) {
                *status = Status::Ok();
            }
            return nullptr;  // 超时不是错误。
        }
        if (rc < 0) {
            if (status != nullptr) {
                *status = SocketErrorStatus("poll", errno);
            }
            return nullptr;
        }
    }
    const int fd = ::accept(m_fd, nullptr, nullptr);
    if (fd < 0) {
        if (status != nullptr) {
            *status = (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                          ? Status::Ok()
                          : SocketErrorStatus("accept", errno);
        }
        return nullptr;
    }
    SetCloseOnExec(fd);
    SetNoDelay(fd);
    if (status != nullptr) {
        *status = Status::Ok();
    }
    return std::unique_ptr<TcpTransport>(new TcpTransport(fd));
}

void TcpListener::Close() noexcept {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (m_fd >= 0) {
        (void)::shutdown(m_fd, SHUT_RDWR);
    }
}

}  // namespace go2cpp::rpc
