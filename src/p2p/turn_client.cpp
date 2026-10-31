/**
 * @file turn_client.cpp
 * @brief 简化 TURN 客户端实现。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/turn.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "go2cpp/p2p/random.hpp"
#include "relay_protocol.hpp"

namespace go2cpp::p2p {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxDatagramSize = 65535;

std::chrono::milliseconds Remaining(Clock::time_point deadline) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds(0);
    }
    return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                    std::chrono::milliseconds(1));
}

std::string StatusText(std::uint8_t status) {
    return "status=" + std::to_string(static_cast<int>(status));
}

}  // namespace

RelayClient::RelayClient(DatagramTransport* transport, RelayClientConfig config)
    : m_transport(transport),
      m_config(std::move(config)),
      m_recv_buffer(kMaxDatagramSize),
      m_request_buffer(kMaxDatagramSize) {
    m_next_request_id = RandomU32();
    if (m_next_request_id == 0) {
        m_next_request_id = 1;
    }
}

RelayClient::~RelayClient() {
    if (m_allocated) {
        Release();
    }
}

std::uint32_t RelayClient::NextRequestId() {
    ++m_next_request_id;
    if (m_next_request_id == 0) {
        m_next_request_id = 1;
    }
    return m_next_request_id;
}

bool RelayClient::Request(std::uint8_t type, const std::vector<std::uint8_t>& body,
                          std::chrono::milliseconds timeout, std::uint8_t* status,
                          std::vector<std::uint8_t>* response_body, std::string* error) {
    if (m_transport == nullptr || !m_config.server.valid()) {
        if (error != nullptr) {
            *error = "中继客户端未配置传输或服务器地址";
        }
        return false;
    }
    detail::RelayMessage request;
    request.type = static_cast<detail::RelayMessageType>(type);
    request.request_id = NextRequestId();
    request.body = body;
    const std::vector<std::uint8_t> wire = detail::SerializeRelayMessage(request);
    if (wire.empty()) {
        if (error != nullptr) {
            *error = "中继请求序列化失败";
        }
        return false;
    }
    if (!m_transport->SendTo(m_config.server, wire.data(), wire.size())) {
        if (error != nullptr) {
            *error = "中继请求发送失败";
        }
        return false;
    }
    const Clock::time_point deadline = Clock::now() + timeout;
    std::vector<std::uint8_t>& buffer = m_request_buffer;
    for (;;) {
        const std::chrono::milliseconds left = Remaining(deadline);
        if (left.count() <= 0) {
            break;
        }
        Endpoint source;
        const int received = m_transport->RecvFrom(&source, buffer.data(), buffer.size(), left);
        if (received == 0) {
            continue;
        }
        if (received < 0) {
            if (error != nullptr) {
                *error = "中继响应接收失败";
            }
            return false;
        }
        detail::RelayMessage response;
        if (!detail::ParseRelayMessage(buffer.data(), static_cast<std::size_t>(received),
                                       &response)) {
            continue;
        }
        if (response.request_id != request.request_id) {
            continue;
        }
        if (response.body.empty()) {
            if (error != nullptr) {
                *error = "中继响应缺少状态码";
            }
            return false;
        }
        *status = response.body[0];
        if (response_body != nullptr) {
            *response_body = std::move(response.body);
        }
        return true;
    }
    if (error != nullptr) {
        *error = "中继请求超时（" + m_config.server.ToString() + "）";
    }
    return false;
}

bool RelayClient::Allocate(std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_allocated) {
        return true;
    }
    const int attempts = std::max(1, m_config.allocate_attempts);
    std::string last_error;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        std::vector<std::uint8_t> body;
        detail::AppendU32Body(&body, m_config.requested_lifetime_ms);
        std::uint8_t status = 0;
        std::vector<std::uint8_t> response_body;
        if (!Request(static_cast<std::uint8_t>(detail::RelayMessageType::kAllocateRequest), body,
                     m_config.request_timeout, &status, &response_body, &last_error)) {
            continue;
        }
        if (status != static_cast<std::uint8_t>(detail::RelayStatus::kOk)) {
            last_error = "中继分配被拒绝（" + StatusText(status) + "）";
            continue;
        }
        std::size_t offset = 1;
        Endpoint relayed;
        std::uint32_t lifetime = 0;
        if (!detail::ReadEndpointBody(response_body, &offset, &relayed) ||
            !detail::ReadU32Body(response_body, &offset, &lifetime)) {
            last_error = "中继分配响应格式非法";
            continue;
        }
        m_relayed = relayed;
        m_lifetime_ms = lifetime;
        m_allocated = true;
        return true;
    }
    if (error != nullptr) {
        *error = last_error.empty() ? "中继分配失败" : last_error;
    }
    return false;
}

bool RelayClient::Refresh(std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_allocated) {
        if (error != nullptr) {
            *error = "尚未分配中继端点";
        }
        return false;
    }
    std::vector<std::uint8_t> body;
    detail::AppendU32Body(&body, m_config.requested_lifetime_ms);
    std::uint8_t status = 0;
    std::vector<std::uint8_t> response_body;
    if (!Request(static_cast<std::uint8_t>(detail::RelayMessageType::kRefreshRequest), body,
                 m_config.request_timeout, &status, &response_body, error)) {
        return false;
    }
    if (status != static_cast<std::uint8_t>(detail::RelayStatus::kOk)) {
        if (error != nullptr) {
            *error = "中继续租失败（" + StatusText(status) + "）";
        }
        return false;
    }
    std::size_t offset = 1;
    std::uint32_t lifetime = 0;
    if (detail::ReadU32Body(response_body, &offset, &lifetime)) {
        m_lifetime_ms = lifetime;
    }
    return true;
}

void RelayClient::Release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_allocated || m_transport == nullptr) {
        m_allocated = false;
        return;
    }
    std::uint8_t status = 0;
    std::string ignored;
    (void)Request(static_cast<std::uint8_t>(detail::RelayMessageType::kReleaseRequest), {},
                  std::chrono::milliseconds(200), &status, nullptr, &ignored);
    m_allocated = false;
    m_relayed = Endpoint();
}

bool RelayClient::allocated() const noexcept {
    return m_allocated;
}

Endpoint RelayClient::relayed_endpoint() const {
    return m_relayed;
}

std::uint32_t RelayClient::lifetime_ms() const noexcept {
    return m_lifetime_ms;
}

bool RelayClient::SendTo(const Endpoint& peer, const void* data, std::size_t length) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_allocated || m_transport == nullptr || !peer.valid()) {
        return false;
    }
    if (length > detail::kRelayMaxBody - 32) {
        return false;
    }
    std::vector<std::uint8_t> body;
    body.reserve(3 + 16 + length);
    detail::AppendEndpointBody(&body, peer);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (length > 0) {
        if (bytes == nullptr) {
            return false;
        }
        body.insert(body.end(), bytes, bytes + length);
    }
    detail::RelayMessage message;
    message.type = detail::RelayMessageType::kSendIndication;
    message.request_id = 0;
    message.body = std::move(body);
    const std::vector<std::uint8_t> wire = detail::SerializeRelayMessage(message);
    if (wire.empty() || !m_transport->SendTo(m_config.server, wire.data(), wire.size())) {
        return false;
    }
    ++m_packets_sent;
    m_bytes_sent += length;
    return true;
}

int RelayClient::RecvFrom(Endpoint* peer, void* buffer, std::size_t capacity,
                          std::chrono::milliseconds timeout) {
    // 只在校验状态时持锁：接收等待可能长达数秒，持锁会让并发的 SendTo 被
    // 卡住。数据报接收本身是「唯一读端」语义，由调用方保证串行。
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_allocated || m_transport == nullptr) {
            return -1;
        }
    }
    if (buffer == nullptr && capacity > 0) {
        return -1;
    }
    const Clock::time_point deadline = Clock::now() + timeout;
    for (;;) {
        const std::chrono::milliseconds left = Remaining(deadline);
        if (left.count() <= 0) {
            return 0;
        }
        Endpoint source;
        const int received =
            m_transport->RecvFrom(&source, m_recv_buffer.data(), m_recv_buffer.size(), left);
        if (received == 0) {
            continue;
        }
        if (received < 0) {
            return -1;
        }
        detail::RelayMessage message;
        if (!detail::ParseRelayMessage(m_recv_buffer.data(),
                                       static_cast<std::size_t>(received), &message)) {
            continue;
        }
        if (message.type != detail::RelayMessageType::kDataIndication) {
            continue;
        }
        std::size_t offset = 0;
        Endpoint sender;
        if (!detail::ReadEndpointBody(message.body, &offset, &sender)) {
            continue;
        }
        const std::size_t payload_length = message.body.size() - offset;
        const std::size_t copied = std::min(payload_length, capacity);
        if (copied > 0) {
            std::memcpy(buffer, message.body.data() + offset, copied);
        }
        if (peer != nullptr) {
            *peer = sender;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_packets_received;
            m_bytes_received += copied;
        }
        return static_cast<int>(copied);
    }
}

std::uint64_t RelayClient::bytes_sent() const noexcept {
    return m_bytes_sent;
}

std::uint64_t RelayClient::bytes_received() const noexcept {
    return m_bytes_received;
}

std::uint64_t RelayClient::packets_sent() const noexcept {
    return m_packets_sent;
}

std::uint64_t RelayClient::packets_received() const noexcept {
    return m_packets_received;
}

}  // namespace go2cpp::p2p
