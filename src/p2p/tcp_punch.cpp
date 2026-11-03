/**
 * @file tcp_punch.cpp
 * @brief TCP 同时打开打洞实现。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/tcp_punch.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

namespace go2cpp::p2p {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kPollSliceMs = 50;

std::chrono::milliseconds Remaining(Clock::time_point deadline) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds(0);
    }
    return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                    std::chrono::milliseconds(1));
}

std::string ErrorText(const std::string& what, int error_number) {
    return what + " 失败: " + std::strerror(error_number);
}

bool SetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

std::unique_ptr<TcpPuncher> TcpPuncher::Bind(const Endpoint& local, std::string* error) {
    if (!local.valid() || local.family() != AF_INET) {
        if (error != nullptr) {
            *error = "TCP 打洞当前只支持 IPv4 端点";
        }
        return nullptr;
    }
    auto puncher = std::make_unique<TcpPuncher>();
    if (!puncher->RecreateSocket(local, error)) {
        return nullptr;
    }
    return puncher;
}

TcpPuncher::~TcpPuncher() {
    Close();
}

bool TcpPuncher::RecreateSocket(const Endpoint& local, std::string* error) {
    ResetSocket();
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error != nullptr) {
            *error = ErrorText("socket", errno);
        }
        return false;
    }
    const int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (!SetNonBlocking(fd)) {
        const int saved = errno;
        ::close(fd);
        if (error != nullptr) {
            *error = ErrorText("fcntl(O_NONBLOCK)", saved);
        }
        return false;
    }
    if (::bind(fd, local.sockaddr_ptr(), local.sockaddr_length()) != 0) {
        const int saved = errno;
        ::close(fd);
        if (error != nullptr) {
            *error = ErrorText("bind " + local.ToString(), saved);
        }
        return false;
    }
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        const int saved = errno;
        ::close(fd);
        if (error != nullptr) {
            *error = ErrorText("getsockname", saved);
        }
        return false;
    }
    m_fd = fd;
    m_local = Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&storage), length);
    m_remote = Endpoint();
    m_connected = false;
    return true;
}

void TcpPuncher::ResetSocket() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_connected = false;
}

/**
 * @brief TCP 同时打开打洞：反复 connect，直到成功或超时。
 *
 * @param remote [in] 对端 IPv4 端点；不可为 IPv6，否则返回 false。
 * @param timeout [in] 总预算毫秒；<=0 时循环体一次都不执行，直接超时。
 * @param error [out] 最后一次失败原因，可为 nullptr。
 *
 * ## 定位
 * TCP NAT 穿透的唯一发起者。对端未同时打开时每次 connect 都会被 RST，
 * 因此必须「重建 socket + 重试」，否则会永久卡在坏 socket 上。
 *
 * @return
 * - true：已连接——调用方**必须**开始收发，并注意连接随时可能被对端关闭；
 * - false：超时或参数非法——调用方**必须**放弃该候选，不要无限重试。
 *
 * ## 步骤
 * step1. 校验 fd 与对端（只支持 IPv4）；
 * step2. 记录 deadline 与本地地址；
 * step3. 循环直到 deadline：fd 失效则重建；
 * step4. 非阻塞 connect，返回 EINPROGRESS 时 poll(POLLOUT)；
 * step5. 可写后用 SO_ERROR/getpeername 判定是否真的连上；
 * step6. 失败则重置 socket 并 sleep 5ms，避免忙转。
 *
 * ## 并发/失败语义
 * - 调用约束：同一 TcpPuncher 不可并发 Connect/Close（m_fd 是共享状态）。
 * - 原子性：socket 重建与 connect 之间可能被对端 RST 打断，循环保证状态可恢复。
 * - 失败语义：不抛异常；失败时 socket 可能已被重置。
 * - 不变量：返回 true 后 m_connected 为 true 且 m_remote 有效。
 */
bool TcpPuncher::Connect(const Endpoint& remote, std::chrono::milliseconds timeout,
                         std::string* error) {
    if (m_fd < 0) {
        if (error != nullptr) {
            *error = "TCP 打洞套接字未初始化";
        }
        return false;
    }
    if (!remote.valid() || remote.family() != AF_INET) {
        if (error != nullptr) {
            *error = "TCP 打洞只支持 IPv4 对端";
        }
        return false;
    }
    const Clock::time_point deadline = Clock::now() + timeout;
    const Endpoint local = m_local;
    std::string last_error;
    while (Clock::now() < deadline) {
        if (m_fd < 0 && !RecreateSocket(local, &last_error)) {
            return false;
        }
        // connect 在非阻塞 socket 上返回 EINPROGRESS；如果上一次尝试已经
        // 收到 RST，则重建 socket 后重试。
        const int rc = ::connect(m_fd, remote.sockaddr_ptr(), remote.sockaddr_length());
        if (rc == 0) {
            m_remote = remote;
            m_connected = true;
            return true;
        }
        if (rc < 0 && errno != EINPROGRESS) {
            last_error = ErrorText("connect " + remote.ToString(), errno);
            ResetSocket();
            continue;
        }
        // 等待套接字可写（连接完成）或出错。
        pollfd descriptor{};
        descriptor.fd = m_fd;
        descriptor.events = POLLOUT;
        const std::chrono::milliseconds left = Remaining(deadline);
        const int ready = ::poll(&descriptor, 1,
                                 static_cast<int>(std::min<long long>(left.count(), kPollSliceMs)));
        if (ready == 0) {
            continue;  // SYN 仍在重传，保持同一连接继续等。
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            last_error = ErrorText("poll", errno);
            ResetSocket();
            continue;
        }
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0) {
            last_error = ErrorText("getsockopt(SO_ERROR)", errno);
            ResetSocket();
            continue;
        }
        if (socket_error == 0) {
            sockaddr_storage storage{};
            socklen_t length = sizeof(storage);
            if (::getpeername(m_fd, reinterpret_cast<sockaddr*>(&storage), &length) == 0) {
                m_remote = Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&storage), length);
                m_connected = true;
                return true;
            }
        }
        last_error = ErrorText("同时打开", socket_error);
        ResetSocket();
        // 对端尚未同时打开时每次 connect 都会被 RST，短暂让出 CPU，避免
        // 在超时窗口内忙转。
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (error != nullptr) {
        *error = last_error.empty() ? "TCP 同时打开超时" : last_error;
    }
    return false;
}

Endpoint TcpPuncher::local_endpoint() const {
    return m_local;
}

Endpoint TcpPuncher::remote_endpoint() const {
    return m_remote;
}

bool TcpPuncher::connected() const noexcept {
    return m_connected;
}

bool TcpPuncher::Send(const void* data, std::size_t length) {
    if (!m_connected || m_fd < 0) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t sent = ::send(m_fd, bytes + offset, length - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            pollfd descriptor{};
            descriptor.fd = m_fd;
            descriptor.events = POLLOUT;
            (void)::poll(&descriptor, 1, kPollSliceMs);
            continue;
        }
        return false;
    }
    return true;
}

int TcpPuncher::Recv(void* buffer, std::size_t capacity, std::chrono::milliseconds timeout) {
    if (!m_connected || m_fd < 0) {
        return -1;
    }
    pollfd descriptor{};
    descriptor.fd = m_fd;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1,
                             static_cast<int>(std::min<long long>(timeout.count(), 60000)));
    if (ready == 0) {
        return 0;
    }
    if (ready < 0) {
        return errno == EINTR ? 0 : -1;
    }
    const ssize_t received = ::recv(m_fd, buffer, capacity, 0);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    return static_cast<int>(received);
}

void TcpPuncher::Close() {
    ResetSocket();
    m_remote = Endpoint();
}

}  // namespace go2cpp::p2p
