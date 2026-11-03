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
    if (m_allocated.load(std::memory_order_relaxed)) {
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

void RelayClient::EnsureSendBuffer(std::size_t bytes) {
    // 只增不减：稳态下 SendTo 不再触发分配，也不会把已扩容的缓冲缩回去。
    if (m_send_buffer.size() < bytes) {
        m_send_buffer.resize(bytes);
    }
}

/**
 * @brief 发送一条中继协议请求，等待 request_id 匹配的响应并取回状态码与 body。
 *
 * @param type [in] 中继消息类型（Allocate/Refresh/Release 等），取值来自 RelayMessageType。
 * @param body [in] 请求体，可为空 vector；引用在本调用期间有效。
 * @param timeout [in] 接收上限毫秒；<=0 立即超时。
 * @param status [out] 响应首字节状态码，成功时写入；失败时未定义。
 * @param response_body [out] 响应体（含状态字节），可为 nullptr 表示不取；失败时不修改。
 * @param error [out] 失败原因，可为 nullptr。
 *
 * @return
 * - true：收到匹配响应——调用方**必须**先检查 status 再解析 body；
 * - false：超时/发送失败/格式非法——调用方**必须**读 error 并决定重试。
 *
 * ## 定位
 * 中继控制面唯一的请求/响应点。request_id 不匹配的响应必须丢弃，否则并发分配会串结果；
 * 响应体长度小于 1 时必须报错，否则调用方会读到未定义的 status。
 *
 * ## 步骤
 * step1. 校验 transport 与服务器地址；
 * step2. 生成请求 id 并序列化到复用缓冲；
 * step3. 发送；
 * step4. 循环接收直到 deadline；
 * step5. 丢弃解析失败或 request_id 不匹配的数据报；
 * step6. 校验 body 至少含状态字节，写回 status 与可选 body。
 *
 * ## 并发/失败语义
 * - 调用约束：共用 m_request_wire/m_request_buffer，不可并发调用。
 * - 原子性：请求-响应窗口内无锁，其他收发可能插入。
 * - 失败语义：返回 false 不抛异常；status 只在成功路径写入。
 * - 不变量：成功返回时 status 指向有效字节。
 */
bool RelayClient::Request(std::uint8_t type, const std::vector<std::uint8_t>& body,
                          std::chrono::milliseconds timeout, std::uint8_t* status,
                          std::vector<std::uint8_t>* response_body, std::string* error) {
    if (m_transport == nullptr || !m_config.server.valid()) {
        if (error != nullptr) {
            *error = "中继客户端未配置传输或服务器地址";
        }
        return false;
    }
    const std::uint32_t request_id = NextRequestId();
    m_request_wire.resize(detail::kRelayHeaderSize + body.size());
    if (detail::BuildRelayMessage(
            m_request_wire.data(), m_request_wire.size(),
            static_cast<detail::RelayMessageType>(type), request_id,
            body.empty() ? nullptr : body.data(), body.size()) == 0) {
        if (error != nullptr) {
            *error = "中继请求序列化失败";
        }
        return false;
    }
    if (!m_transport->SendTo(m_config.server, m_request_wire.data(), m_request_wire.size())) {
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
        detail::RelayMessageView response;
        if (!detail::ParseRelayMessageView(buffer.data(), static_cast<std::size_t>(received),
                                           &response)) {
            continue;
        }
        if (response.request_id != request_id) {
            continue;
        }
        if (response.body_length == 0) {
            if (error != nullptr) {
                *error = "中继响应缺少状态码";
            }
            return false;
        }
        *status = response.body[0];
        if (response_body != nullptr) {
            response_body->assign(response.body, response.body + response.body_length);
        }
        return true;
    }
    if (error != nullptr) {
        *error = "中继请求超时（" + m_config.server.ToString() + "）";
    }
    return false;
}

/**
 * @brief 申请中继端点；已分配时幂等成功，失败按配置次数重试。
 *
 * @param error [out] 最后一次失败原因，可为 nullptr；成功时不修改。
 *
 * @return
 * - true：已分配——调用方**必须**用 relayed_endpoint() 获取中继地址；
 * - false：所有尝试失败或响应格式非法——调用方**必须**降级到直连或报错。
 *
 * ## 定位
 * 中继数据面的前提。分配未成功就让 SendTo 走中继会静默丢包；
 * 不校验响应里的端点/生命周期字段会把垃圾数据当分配结果。
 *
 * ## 步骤
 * step1. 持锁并检查是否已分配（幂等快路径）；
 * step2. 按 allocate_attempts 循环；
 * step3. 每次发 Allocate 请求；
 * step4. 状态非 OK 或请求失败则记错误并重试；
 * step5. 解析中继端点与生命周期，格式非法同样重试；
 * step6. 成功后写 m_relayed/m_lifetime 并 release 置已分配。
 *
 * ## 并发/失败语义
 * - 调用约束：内部持 m_mutex，与 Refresh/Release 串行。
 * - 原子性：分配成功标志在字段写完之后 release 发布。
 * - 失败语义：不抛异常；失败后 allocated() 仍为 false，可重试。
 * - 不变量：allocated()==true 时 relayed_endpoint() 一定有效。
 */
bool RelayClient::Allocate(std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_allocated.load(std::memory_order_relaxed)) {
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
        m_lifetime_ms.store(lifetime, std::memory_order_relaxed);
        m_allocated.store(true, std::memory_order_release);
        return true;
    }
    if (error != nullptr) {
        *error = last_error.empty() ? "中继分配失败" : last_error;
    }
    return false;
}

bool RelayClient::Refresh(std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_allocated.load(std::memory_order_relaxed)) {
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
        m_lifetime_ms.store(lifetime, std::memory_order_relaxed);
    }
    return true;
}

void RelayClient::Release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_allocated.load(std::memory_order_relaxed) || m_transport == nullptr) {
        m_allocated.store(false, std::memory_order_release);
        return;
    }
    std::uint8_t status = 0;
    std::string ignored;
    (void)Request(static_cast<std::uint8_t>(detail::RelayMessageType::kReleaseRequest), {},
                  std::chrono::milliseconds(200), &status, nullptr, &ignored);
    m_allocated.store(false, std::memory_order_release);
    m_relayed = Endpoint();
}

bool RelayClient::allocated() const noexcept {
    return m_allocated.load(std::memory_order_acquire);
}

Endpoint RelayClient::relayed_endpoint() const {
    return m_relayed;
}

std::uint32_t RelayClient::lifetime_ms() const noexcept {
    return m_lifetime_ms.load(std::memory_order_relaxed);
}

bool RelayClient::SendTo(const Endpoint& peer, const void* data, std::size_t length) {
    // 数据面只读 allocated 原子标志：不与控制面/接收路径抢同一把互斥量。
    if (!m_allocated.load(std::memory_order_acquire) || m_transport == nullptr || !peer.valid()) {
        return false;
    }
    if (length > detail::kRelayMaxBody - 32) {
        return false;
    }
    const bool ipv4 = peer.IsIPv4();
    if (!ipv4 && !peer.IsIPv6()) {
        return false;
    }
    if (length > 0 && data == nullptr) {
        return false;
    }
    const std::size_t body_length = 3 + (ipv4 ? 4U : 16U) + length;
    const std::size_t total = detail::kRelayHeaderSize + body_length;
    EnsureSendBuffer(total);
    std::uint8_t* out = m_send_buffer.data();
    (void)detail::BuildRelayHeader(out, total, detail::RelayMessageType::kSendIndication, 0,
                                   body_length);
    const std::size_t endpoint_bytes = detail::EncodeEndpointBody(
        out + detail::kRelayHeaderSize, total - detail::kRelayHeaderSize, peer);
    if (endpoint_bytes == 0) {
        return false;
    }
    if (length > 0) {
        std::memcpy(out + detail::kRelayHeaderSize + endpoint_bytes, data, length);
    }
    if (!m_transport->SendTo(m_config.server, out, total)) {
        return false;
    }
    m_packets_sent.fetch_add(1, std::memory_order_relaxed);
    m_bytes_sent.fetch_add(length, std::memory_order_relaxed);
    return true;
}

int RelayClient::RecvFrom(Endpoint* peer, void* buffer, std::size_t capacity,
                          std::chrono::milliseconds timeout) {
    // 只读取一次状态：接收等待可能长达数秒，不能持锁。
    if (!m_allocated.load(std::memory_order_acquire) || m_transport == nullptr) {
        return -1;
    }
    if (buffer == nullptr && capacity > 0) {
        return -1;
    }
    const Clock::time_point deadline = Clock::now() + timeout;
    // 稳态（每次都有数据报可读）时不读时钟：只有空包/EINTR/无效包才重算剩余
    // 预算。旧实现每个数据包都要 Clock::now()（vDSO 约 20ns）+ duration_cast。
    std::chrono::milliseconds left = timeout;
    for (;;) {
        if (left.count() <= 0) {
            return 0;
        }
        Endpoint source;
        const int received =
            m_transport->RecvFrom(&source, m_recv_buffer.data(), m_recv_buffer.size(), left);
        if (received == 0) {
            left = Remaining(deadline);
            continue;
        }
        if (received < 0) {
            return -1;
        }
        detail::RelayMessageView message;
        if (!detail::ParseRelayMessageView(m_recv_buffer.data(),
                                           static_cast<std::size_t>(received), &message)) {
            left = Remaining(deadline);
            continue;
        }
        if (message.type != detail::RelayMessageType::kDataIndication) {
            left = Remaining(deadline);
            continue;
        }
        std::size_t offset = 0;
        Endpoint sender;
        if (!detail::ReadEndpointBodyView(message.body, message.body_length, &offset, &sender)) {
            left = Remaining(deadline);
            continue;
        }
        const std::size_t payload_length = message.body_length - offset;
        const std::size_t copied = std::min(payload_length, capacity);
        if (copied > 0) {
            std::memcpy(buffer, message.body + offset, copied);
        }
        if (peer != nullptr) {
            *peer = sender;
        }
        m_packets_received.fetch_add(1, std::memory_order_relaxed);
        m_bytes_received.fetch_add(copied, std::memory_order_relaxed);
        return static_cast<int>(copied);
    }
}

std::uint64_t RelayClient::bytes_sent() const noexcept {
    return m_bytes_sent.load(std::memory_order_relaxed);
}

std::uint64_t RelayClient::bytes_received() const noexcept {
    return m_bytes_received.load(std::memory_order_relaxed);
}

std::uint64_t RelayClient::packets_sent() const noexcept {
    return m_packets_sent.load(std::memory_order_relaxed);
}

std::uint64_t RelayClient::packets_received() const noexcept {
    return m_packets_received.load(std::memory_order_relaxed);
}

}  // namespace go2cpp::p2p
