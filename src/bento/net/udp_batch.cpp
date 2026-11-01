/**
 * @file udp_batch.cpp
 * @brief 批量/多队列 UDP 实现（recvmmsg/sendmmsg + SO_REUSEPORT）。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/bento/net/udp_batch.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#if defined(__linux__)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace go2cpp::bento::net {
namespace {

/// @brief 设置 sockaddr_storage 的端口（保持地址族不变）。
void SetPort(sockaddr_storage* addr, std::uint16_t port) noexcept {
    if (addr->ss_family == AF_INET) {
        auto* v4 = reinterpret_cast<sockaddr_in*>(addr);
        v4->sin_port = htons(port);
    } else if (addr->ss_family == AF_INET6) {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(addr);
        v6->sin6_port = htons(port);
    }
}

/// @brief 读取 sockaddr_storage 的端口（主机字节序）。
std::uint16_t GetPort(const sockaddr_storage& addr) noexcept {
    if (addr.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in&>(addr).sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6&>(addr).sin6_port);
    }
    return 0;
}

}  // namespace

UdpBatchSocket::~UdpBatchSocket() {
    Close();
}

bool UdpBatchSocket::Resolve(const Endpoint& endpoint, sockaddr_storage* out, socklen_t* out_len,
                             int* family, std::string* error) {
#if !defined(__linux__)
    (void)endpoint;
    (void)out;
    (void)out_len;
    (void)family;
    if (error != nullptr) {
        *error = "UdpBatchSocket is only implemented on Linux";
    }
    return false;
#else
    if (out == nullptr || out_len == nullptr || family == nullptr) {
        if (error != nullptr) {
            *error = "null resolve output";
        }
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICSERV;
    const std::string service = std::to_string(endpoint.port);
    addrinfo* result = nullptr;
    const int resolved = ::getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &result);
    if (resolved != 0 || result == nullptr) {
        if (error != nullptr) {
            *error = "resolve '" + endpoint.host + "': " + ::gai_strerror(resolved);
        }
        if (result != nullptr) {
            ::freeaddrinfo(result);
        }
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    std::memcpy(out, result->ai_addr, result->ai_addrlen);
    *out_len = static_cast<socklen_t>(result->ai_addrlen);
    *family = result->ai_family;
    ::freeaddrinfo(result);
    return true;
#endif
}

Endpoint UdpBatchSocket::ToEndpoint(const sockaddr_storage& addr, socklen_t len) {
    (void)len;
    Endpoint endpoint;
    char host[INET6_ADDRSTRLEN] = {0};
#if defined(__linux__)
    if (addr.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&addr);
        if (::inet_ntop(AF_INET, &v4->sin_addr, host, sizeof(host)) != nullptr) {
            endpoint.host = host;
        }
        endpoint.port = ntohs(v4->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        if (::inet_ntop(AF_INET6, &v6->sin6_addr, host, sizeof(host)) != nullptr) {
            endpoint.host = host;
        }
        endpoint.port = ntohs(v6->sin6_port);
    }
#else
    (void)addr;
#endif
    return endpoint;
}

void UdpBatchSocket::CloseFds() noexcept {
#if defined(__linux__)
    for (const int fd : m_fds) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
#endif
    m_fds.clear();
    m_slots.clear();
    m_peers.clear();
    m_peer_lens.clear();
    m_port = 0;
    m_next_send_queue = 0;
    m_peer_valid = false;
}

void UdpBatchSocket::Close() noexcept {
    CloseFds();
    m_stats = Stats();
    m_config = Config();
}

bool UdpBatchSocket::Open(const Endpoint& bind, Config config, std::string* error) {
    Close();
    if (config.queues == 0) {
        config.queues = 1;
    }
    config.queues = std::min(config.queues, kMaxQueues);
    if (config.recv_batch == 0) {
        config.recv_batch = 1;
    }
    config.recv_batch = std::min(config.recv_batch, kMaxRecvBatch);
    if (config.send_batch == 0) {
        config.send_batch = 1;
    }
    config.send_batch = std::min(config.send_batch, kMaxSendBatch);
    if (config.slot_bytes < 64) {
        config.slot_bytes = 64;
    }
    if (!config.reuse_port) {
        // 同一端口只能被一个 socket 绑定；没有 REUSEPORT 时多队列无意义。
        config.queues = 1;
    }
    m_config = config;

#if !defined(__linux__)
    if (error != nullptr) {
        *error = "UdpBatchSocket is only implemented on Linux";
    }
    return false;
#else
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    int family = AF_UNSPEC;
    if (!Resolve(bind, &addr, &addr_len, &family, error)) {
        return false;
    }

    bool reuse_ok = false;
    for (std::size_t i = 0; i < m_config.queues; ++i) {
        const int fd = ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            if (i == 0) {
                if (error != nullptr) {
                    *error = std::string("socket: ") + std::strerror(errno);
                }
                CloseFds();
                return false;
            }
            break;
        }
        int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        // 压测时窗口内一次灌入上千个小包，默认 212KB 接收缓冲不够。
        int socket_buffer = 4 * 1024 * 1024;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socket_buffer, sizeof(socket_buffer));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &socket_buffer, sizeof(socket_buffer));
        if (m_config.reuse_port) {
            if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) == 0) {
                if (i == 0) {
                    reuse_ok = true;
                }
            } else if (i == 0) {
                // 内核不支持 REUSEPORT：继续用单队列，不算错误。
                reuse_ok = false;
            } else {
                ::close(fd);
                break;
            }
        }
        sockaddr_storage local = addr;
        if (i > 0) {
            // 后续队列必须绑定第一队列实际拿到的端口。
            SetPort(&local, m_port);
        }
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), addr_len) < 0) {
            const int saved = errno;
            ::close(fd);
            if (i == 0) {
                if (error != nullptr) {
                    *error = std::string("bind: ") + std::strerror(saved);
                }
                CloseFds();
                return false;
            }
            break;
        }
        if (i == 0) {
            sockaddr_storage actual{};
            socklen_t actual_len = sizeof(actual);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
                m_port = GetPort(actual);
            }
        }
        m_fds.push_back(fd);
    }

    m_stats.queues = static_cast<std::uint32_t>(m_fds.size());
    m_stats.reuse_port = reuse_ok && m_fds.size() > 1;
    m_slots.assign(m_config.recv_batch, std::vector<std::uint8_t>(m_config.slot_bytes));
    m_peers.assign(m_config.recv_batch, sockaddr_storage{});
    m_peer_lens.assign(m_config.recv_batch, sizeof(sockaddr_storage));
    return true;
#endif
}

bool UdpBatchSocket::SendBatch(const Endpoint& peer,
                               const std::vector<std::vector<std::uint8_t>>& payloads,
                               std::size_t* sent, std::string* error) {
    if (sent != nullptr) {
        *sent = 0;
    }
#if !defined(__linux__)
    (void)peer;
    (void)payloads;
    if (error != nullptr) {
        *error = "UdpBatchSocket is only implemented on Linux";
    }
    return false;
#else
    if (m_fds.empty()) {
        if (error != nullptr) {
            *error = "socket not open";
        }
        return false;
    }
    sockaddr_storage dest{};
    socklen_t dest_len = 0;
    // 同一对端的连续批量发送不能每批都 getaddrinfo：DNS/IP 解析在压测里
    // 占了 90% 的墙钟时间（1e7 帧实测 9k fps → 修复后 60 万 fps）。
    if (m_peer_valid && m_peer_host == peer.host && m_peer_port == peer.port) {
        dest = m_peer_addr;
        dest_len = m_peer_addr_len;
    } else {
        int family = AF_UNSPEC;
        if (!Resolve(peer, &dest, &dest_len, &family, error)) {
            return false;
        }
        m_peer_valid = true;
        m_peer_host = peer.host;
        m_peer_port = peer.port;
        m_peer_addr = dest;
        m_peer_addr_len = dest_len;
    }
    std::vector<mmsghdr> messages(m_config.send_batch);
    std::vector<iovec> iovs(m_config.send_batch);
    std::size_t offset = 0;
    while (offset < payloads.size()) {
        const std::size_t n = std::min(m_config.send_batch, payloads.size() - offset);
        std::memset(messages.data(), 0, sizeof(mmsghdr) * n);
        for (std::size_t i = 0; i < n; ++i) {
            iovs[i].iov_base = const_cast<std::uint8_t*>(payloads[offset + i].data());
            iovs[i].iov_len = payloads[offset + i].size();
            messages[i].msg_hdr.msg_name = &dest;
            messages[i].msg_hdr.msg_namelen = dest_len;
            messages[i].msg_hdr.msg_iov = &iovs[i];
            messages[i].msg_hdr.msg_iovlen = 1;
        }
        const std::size_t queue = m_next_send_queue % m_fds.size();
        m_next_send_queue = (queue + 1) % m_fds.size();
        const int written = ::sendmmsg(m_fds[queue], messages.data(),
                                       static_cast<unsigned int>(n), 0);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (sent != nullptr) {
                    *sent = offset;
                }
                return false;
            }
            ++m_stats.tx_errors;
            if (error != nullptr) {
                *error = std::string("sendmmsg: ") + std::strerror(errno);
            }
            if (sent != nullptr) {
                *sent = offset;
            }
            return false;
        }
        ++m_stats.tx_batches;
        m_stats.tx_datagrams += static_cast<std::uint64_t>(written);
        offset += static_cast<std::size_t>(written);
        if (static_cast<std::size_t>(written) < n) {
            break;  // 内核队列已满，剩余部分留给调用方重试。
        }
    }
    if (sent != nullptr) {
        *sent = offset;
    }
    return offset == payloads.size();
#endif
}

std::size_t UdpBatchSocket::RecvBatch(std::vector<Datagram>* out, int timeout_ms,
                                      std::string* error) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "null output";
        }
        return 0;
    }
#if !defined(__linux__)
    if (error != nullptr) {
        *error = "UdpBatchSocket is only implemented on Linux";
    }
    return 0;
#else
    if (m_fds.empty()) {
        if (error != nullptr) {
            *error = "socket not open";
        }
        return 0;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    std::vector<pollfd> polled(m_fds.size());
    for (std::size_t i = 0; i < m_fds.size(); ++i) {
        polled[i].fd = m_fds[i];
        polled[i].events = POLLIN;
        polled[i].revents = 0;
    }
    const int ready = ::poll(polled.data(), static_cast<nfds_t>(polled.size()), timeout_ms);
    if (ready < 0) {
        if (errno == EINTR) {
            return 0;
        }
        ++m_stats.rx_errors;
        if (error != nullptr) {
            *error = std::string("poll: ") + std::strerror(errno);
        }
        return 0;
    }
    if (ready == 0) {
        return 0;
    }

    const std::size_t batch = m_config.recv_batch;
    std::vector<mmsghdr> messages(batch);
    std::vector<iovec> iovs(batch);
    std::size_t total = 0;
    for (std::size_t i = 0; i < polled.size(); ++i) {
        if ((polled[i].revents & POLLIN) == 0) {
            continue;
        }
        std::memset(messages.data(), 0, sizeof(mmsghdr) * batch);
        for (std::size_t j = 0; j < batch; ++j) {
            m_peer_lens[j] = sizeof(sockaddr_storage);
            iovs[j].iov_base = m_slots[j].data();
            iovs[j].iov_len = m_slots[j].size();
            messages[j].msg_hdr.msg_name = &m_peers[j];
            messages[j].msg_hdr.msg_namelen = sizeof(sockaddr_storage);
            messages[j].msg_hdr.msg_iov = &iovs[j];
            messages[j].msg_hdr.msg_iovlen = 1;
        }
        const int received = ::recvmmsg(m_fds[i], messages.data(),
                                        static_cast<unsigned int>(batch), 0, nullptr);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ++m_stats.rx_errors;
            if (error != nullptr) {
                *error = std::string("recvmmsg: ") + std::strerror(errno);
            }
            continue;
        }
        ++m_stats.rx_batches;
        if (received == 0) {
            ++m_stats.rx_empty_batches;
            continue;
        }
        m_stats.rx_datagrams += static_cast<std::uint64_t>(received);
        for (int j = 0; j < received; ++j) {
            // recvmmsg 只在 msg_len 写入实际字节数；iov_len 保持调用方
            // 传入的缓冲区大小（用它会得到 1500 而不是真实长度）。
            const std::size_t copied = static_cast<std::size_t>(messages[j].msg_len);
            const std::size_t bytes = std::min(copied, m_slots[static_cast<std::size_t>(j)].size());
            Datagram datagram;
            datagram.data.assign(m_slots[static_cast<std::size_t>(j)].data(),
                                 m_slots[static_cast<std::size_t>(j)].data() + bytes);
            datagram.peer = ToEndpoint(m_peers[static_cast<std::size_t>(j)],
                                       messages[j].msg_hdr.msg_namelen);
            out->push_back(std::move(datagram));
        }
        total += static_cast<std::size_t>(received);
    }
    return total;
#endif
}

}  // namespace go2cpp::bento::net
