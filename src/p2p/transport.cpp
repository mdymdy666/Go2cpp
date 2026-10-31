/**
 * @file transport.cpp
 * @brief POSIX UDP 传输实现与本机地址枚举。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/transport.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>

namespace go2cpp::p2p {
namespace {

int ToPollTimeout(std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() < 0) {
        return -1;
    }
    if (timeout.count() > static_cast<long long>(INT_MAX)) {
        return INT_MAX;
    }
    return static_cast<int>(timeout.count());
}

std::string SocketErrorText(const std::string& what, int error_number) {
    return what + " 失败: " + std::strerror(error_number);
}

}  // namespace

UdpTransport::UdpTransport(UdpTransportConfig config) : m_config(std::move(config)) {}

UdpTransport::~UdpTransport() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool UdpTransport::Start(std::string* error) {
    if (m_fd >= 0) {
        return true;
    }
    Endpoint bind_endpoint = m_config.bind_endpoint;
    if (!bind_endpoint.valid()) {
        bind_endpoint = Endpoint::Wildcard(0);
    }
    int family = bind_endpoint.family();
    if (family != AF_INET && family != AF_INET6) {
        if (error != nullptr) {
            *error = "不支持的绑定地址族: " + std::to_string(static_cast<int>(family));
        }
        return false;
    }
    const int fd = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error != nullptr) {
            *error = SocketErrorText("socket", errno);
        }
        return false;
    }
    if (m_config.reuse_address) {
        const int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    if (family == AF_INET6) {
        // 显式绑定 IPv6 端点时只收 v6，避免 v4-mapped 地址混进候选。
        const int one = 1;
        (void)::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    }
    if (m_config.receive_buffer_bytes > 0) {
        const int bytes = static_cast<int>(std::min<std::size_t>(
            m_config.receive_buffer_bytes, static_cast<std::size_t>(INT_MAX)));
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    }
    if (m_config.send_buffer_bytes > 0) {
        const int bytes = static_cast<int>(std::min<std::size_t>(
            m_config.send_buffer_bytes, static_cast<std::size_t>(INT_MAX)));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
    }
    if (::bind(fd, bind_endpoint.sockaddr_ptr(), bind_endpoint.sockaddr_length()) != 0) {
        const int saved = errno;
        ::close(fd);
        if (error != nullptr) {
            *error = SocketErrorText("bind " + bind_endpoint.ToString(), saved);
        }
        return false;
    }
    sockaddr_storage local{};
    socklen_t local_length = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length) != 0) {
        const int saved = errno;
        ::close(fd);
        if (error != nullptr) {
            *error = SocketErrorText("getsockname", saved);
        }
        return false;
    }
    m_fd = fd;
    m_local = Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&local), local_length);
    return true;
}

bool UdpTransport::SendTo(const Endpoint& destination, const void* data, std::size_t length) {
    if (m_fd < 0 || !destination.valid()) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = length;
    // UDP 一次 sendto 就是一条数据报，不能拆包；这里只在 EINTR 上重试。
    for (;;) {
        const ssize_t sent = ::sendto(m_fd, bytes, remaining, 0, destination.sockaddr_ptr(),
                                     destination.sockaddr_length());
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        return static_cast<std::size_t>(sent) == length;
    }
}

int UdpTransport::RecvFrom(Endpoint* source, void* buffer, std::size_t capacity,
                           std::chrono::milliseconds timeout) {
    if (m_fd < 0) {
        return -1;
    }
    pollfd descriptor{};
    descriptor.fd = m_fd;
    descriptor.events = POLLIN;
    const int rc = ::poll(&descriptor, 1, ToPollTimeout(timeout));
    if (rc == 0) {
        return 0;
    }
    if (rc < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }
    sockaddr_storage address{};
    socklen_t address_length = sizeof(address);
    const ssize_t received = ::recvfrom(m_fd, buffer, capacity, 0,
                                        reinterpret_cast<sockaddr*>(&address), &address_length);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    if (source != nullptr) {
        *source = Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&address), address_length);
    }
    return static_cast<int>(received);
}

Endpoint UdpTransport::LocalEndpoint() const {
    std::lock_guard<std::mutex> lock(m_local_mutex);
    return m_local;
}

bool UdpTransport::SetReceiveBufferSize(std::size_t bytes) {
    if (m_fd < 0) {
        return false;
    }
    const int size = static_cast<int>(
        std::min<std::size_t>(bytes, static_cast<std::size_t>(INT_MAX)));
    return ::setsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0;
}

std::vector<Endpoint> EnumerateLocalAddresses(std::uint16_t port, bool include_loopback) {
    std::vector<Endpoint> ipv4;
    std::vector<Endpoint> ipv6;
    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) {
        return {};
    }
    for (const ifaddrs* current = interfaces; current != nullptr; current = current->ifa_next) {
        if (current->ifa_addr == nullptr || current->ifa_name == nullptr) {
            continue;
        }
        const unsigned int flags = current->ifa_flags;
        if ((flags & IFF_UP) == 0U) {
            continue;
        }
        const bool is_loopback = (flags & IFF_LOOPBACK) != 0U;
        if (is_loopback && !include_loopback) {
            continue;
        }
        const sa_family_t family = current->ifa_addr->sa_family;
        if (family == AF_INET) {
            const auto* address = reinterpret_cast<const sockaddr_in*>(current->ifa_addr);
            Endpoint endpoint = Endpoint::FromSockaddr(current->ifa_addr,
                                                       static_cast<socklen_t>(sizeof(sockaddr_in)));
            if (!endpoint.valid() || address->sin_addr.s_addr == htonl(INADDR_ANY)) {
                continue;
            }
            const Endpoint with_port = endpoint.WithPort(port);
            if (std::find(ipv4.begin(), ipv4.end(), with_port) == ipv4.end()) {
                ipv4.push_back(with_port);
            }
        } else if (family == AF_INET6) {
            const auto* address = reinterpret_cast<const sockaddr_in6*>(current->ifa_addr);
            if (IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr) ||
                IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr)) {
                continue;
            }
            Endpoint endpoint = Endpoint::FromSockaddr(current->ifa_addr,
                                                       static_cast<socklen_t>(sizeof(sockaddr_in6)));
            if (!endpoint.valid()) {
                continue;
            }
            const Endpoint with_port = endpoint.WithPort(port);
            if (std::find(ipv6.begin(), ipv6.end(), with_port) == ipv6.end()) {
                ipv6.push_back(with_port);
            }
        }
    }
    ::freeifaddrs(interfaces);
    std::vector<Endpoint> result;
    result.reserve(ipv4.size() + ipv6.size());
    result.insert(result.end(), ipv4.begin(), ipv4.end());
    result.insert(result.end(), ipv6.begin(), ipv6.end());
    return result;
}

}  // namespace go2cpp::p2p
