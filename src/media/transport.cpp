/**
 * @file transport.cpp
 * @brief UDP 端点 + VideoChannel 分片/重组 + 进程内直连链路实现。
 * @details 单线程 pump 事件循环：`Recv` 用 `poll` 做带超时的非阻塞读，不引入
 *          后台线程，测试可以逐包精确控制（丢包/乱序注入）。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/transport.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

#include "go2cpp/bento/net/protocol.hpp"

namespace go2cpp::media {
namespace {

/// @brief 单个 UDP 数据报缓冲上限（Bento 单帧最大 1524B，留余量）。
constexpr std::size_t kMaxDatagramBytes = 2048;

/// @brief 接收缓冲放大到 4MB：高码率视频在 pump 间隙会堆积，默认 208KB 太小。
constexpr int kSocketBufferBytes = 4 * 1024 * 1024;

}  // namespace

UdpEndpoint::~UdpEndpoint() {
    Close();
}

bool UdpEndpoint::Open(std::string* error) {
    Close();
    m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_fd < 0) {
        if (error != nullptr) {
            *error = "socket(AF_INET, SOCK_DGRAM) failed";
        }
        return false;
    }
    int buffer = kSocketBufferBytes;
    ::setsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    ::setsockopt(m_fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (error != nullptr) {
            *error = "bind(127.0.0.1:0) failed";
        }
        Close();
        return false;
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        if (error != nullptr) {
            *error = "getsockname failed";
        }
        Close();
        return false;
    }
    m_port = ntohs(bound.sin_port);
    return true;
}

bool UdpEndpoint::Send(const std::uint8_t* data, std::size_t bytes) noexcept {
    if (m_fd < 0 || m_peer_port == 0) {
        return false;
    }
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer.sin_port = htons(m_peer_port);
    const ssize_t sent = ::sendto(m_fd, data, bytes, 0, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    return sent == static_cast<ssize_t>(bytes);
}

int UdpEndpoint::Recv(std::uint8_t* buffer, std::size_t capacity, int timeout_ms) noexcept {
    if (m_fd < 0) {
        return -1;
    }
    pollfd entry{};
    entry.fd = m_fd;
    entry.events = POLLIN;
    const int ready = ::poll(&entry, 1, timeout_ms < 0 ? 0 : timeout_ms);
    if (ready == 0) {
        return 0;
    }
    if (ready < 0) {
        return -1;
    }
    const ssize_t received = ::recv(m_fd, buffer, capacity, 0);
    if (received < 0) {
        return -1;
    }
    return static_cast<int>(received);
}

void UdpEndpoint::Close() noexcept {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_port = 0;
}

struct UdpVideoSender::Impl {
    UdpEndpoint endpoint;
    std::uint64_t send_failures = 0;
    std::unique_ptr<go2cpp::bento::VideoChannel> channel;

    Impl() {
        channel = std::make_unique<go2cpp::bento::VideoChannel>(
            [this](const std::uint8_t* data, std::size_t bytes) {
                if (!endpoint.Send(data, bytes)) {
                    ++send_failures;
                    return false;
                }
                return true;
            },
            [](std::uint32_t, bool, const std::uint8_t*, std::size_t) {});
    }
};

UdpVideoSender::UdpVideoSender() : m_impl(std::make_unique<Impl>()) {}

UdpVideoSender::~UdpVideoSender() = default;

bool UdpVideoSender::Open(std::uint16_t peer_port, std::string* error) {
    if (!m_impl->endpoint.Open(error)) {
        return false;
    }
    m_impl->endpoint.SetPeerPort(peer_port);
    return true;
}

std::uint16_t UdpVideoSender::local_port() const noexcept {
    return m_impl->endpoint.port();
}

bool UdpVideoSender::SendFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data,
                               std::size_t bytes) {
    return m_impl->channel->SendFrame(frame_id, key, data, bytes);
}

const go2cpp::bento::VideoChannel::Stats& UdpVideoSender::stats() const noexcept {
    return m_impl->channel->stats();
}

std::uint64_t UdpVideoSender::send_failures() const noexcept {
    return m_impl->send_failures;
}

struct UdpVideoReceiver::Impl {
    UdpEndpoint endpoint;
    std::vector<std::uint8_t> buffer;
    std::unique_ptr<go2cpp::bento::VideoChannel> channel;

    explicit Impl(const FrameDeliverFn& deliver) {
        buffer.resize(kMaxDatagramBytes);
        channel = std::make_unique<go2cpp::bento::VideoChannel>(
            [](const std::uint8_t*, std::size_t) { return true; },
            [deliver](std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes) {
                if (deliver) {
                    deliver(frame_id, key, data, bytes);
                }
            });
    }
};

UdpVideoReceiver::UdpVideoReceiver(FrameDeliverFn deliver)
    : m_impl(std::make_unique<Impl>(deliver)), m_deliver(std::move(deliver)) {}

UdpVideoReceiver::~UdpVideoReceiver() = default;

bool UdpVideoReceiver::Open(std::string* error) {
    return m_impl->endpoint.Open(error);
}

std::uint16_t UdpVideoReceiver::local_port() const noexcept {
    return m_impl->endpoint.port();
}

int UdpVideoReceiver::Pump(int timeout_ms, int max_packets) {
    if (max_packets <= 0) {
        max_packets = 64;
    }
    int delivered = 0;
    for (int i = 0; i < max_packets; ++i) {
        const int received =
            m_impl->endpoint.Recv(m_impl->buffer.data(), m_impl->buffer.size(), i == 0 ? timeout_ms : 0);
        if (received <= 0) {
            break;
        }
        ++m_datagrams;
        go2cpp::bento::FrameView view;
        if (!go2cpp::bento::ParseFrame(m_impl->buffer.data(), static_cast<std::size_t>(received),
                                       &view) ||
            view.header.type != go2cpp::bento::FrameType::kVideo) {
            ++m_malformed;
            continue;
        }
        const std::uint64_t before = m_impl->channel->stats().frames_delivered;
        m_impl->channel->OnFrame(view.header, view.payload, view.payload_bytes);
        if (m_impl->channel->stats().frames_delivered > before) {
            ++delivered;
        }
    }
    return delivered;
}

void UdpVideoReceiver::OnTimer(std::uint64_t now_ms) {
    m_impl->channel->OnTimer(now_ms);
}

const go2cpp::bento::VideoChannel::Stats& UdpVideoReceiver::stats() const noexcept {
    return m_impl->channel->stats();
}

struct InProcessVideoLink::Impl {
    std::deque<std::vector<std::uint8_t>> queue;
    std::vector<std::vector<std::uint8_t>> sent;
    std::uint32_t drop_every = 0;
    std::uint32_t counter = 0;
    std::uint64_t delivered = 0;
    FrameDeliverFn deliver;
    std::unique_ptr<go2cpp::bento::VideoChannel> sender;
    std::unique_ptr<go2cpp::bento::VideoChannel> receiver;

    Impl() {
        sender = std::make_unique<go2cpp::bento::VideoChannel>(
            [this](const std::uint8_t* data, std::size_t bytes) {
                ++counter;
                if (drop_every >= 2 && (counter % drop_every) == 0) {
                    return true;  // 模拟丢包：吞掉这个分片但仍算发送成功。
                }
                sent.emplace_back(data, data + bytes);
                return true;
            },
            [](std::uint32_t, bool, const std::uint8_t*, std::size_t) {});
        receiver = std::make_unique<go2cpp::bento::VideoChannel>(
            [](const std::uint8_t*, std::size_t) { return true; },
            [this](std::uint32_t frame_id, bool key, const std::uint8_t* data, std::size_t bytes) {
                ++delivered;
                if (deliver) {
                    deliver(frame_id, key, data, bytes);
                }
            });
    }
};

InProcessVideoLink::InProcessVideoLink() : m_impl(std::make_unique<Impl>()) {}
InProcessVideoLink::~InProcessVideoLink() = default;

bool InProcessVideoLink::SendFrame(std::uint32_t frame_id, bool key, const std::uint8_t* data,
                                   std::size_t bytes) {
    return m_impl->sender->SendFrame(frame_id, key, data, bytes);
}

void InProcessVideoLink::Flush() {
    for (const auto& packet : m_impl->sent) {
        go2cpp::bento::FrameView view;
        if (go2cpp::bento::ParseFrame(packet.data(), packet.size(), &view)) {
            m_impl->receiver->OnFrame(view.header, view.payload, view.payload_bytes);
        }
    }
    m_impl->sent.clear();
    m_delivered = m_impl->delivered;
}

void InProcessVideoLink::SetDropEvery(std::uint32_t drop_every) noexcept {
    m_impl->drop_every = drop_every;
}

void InProcessVideoLink::SetDeliver(FrameDeliverFn deliver) {
    m_impl->deliver = std::move(deliver);
}

const go2cpp::bento::VideoChannel::Stats& InProcessVideoLink::send_stats() const noexcept {
    return m_impl->sender->stats();
}

const go2cpp::bento::VideoChannel::Stats& InProcessVideoLink::recv_stats() const noexcept {
    return m_impl->receiver->stats();
}

void InProcessVideoLink::OnTimer(std::uint64_t now_ms) {
    m_impl->receiver->OnTimer(now_ms);
}

}  // namespace go2cpp::media
