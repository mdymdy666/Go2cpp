/**
 * @file link_server.cpp
 * @brief LinkServer 实现：Hub 生命周期、peer 索引、统一 Send/Broadcast 与回调桥。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/bento/net/link_server.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace go2cpp::bento {
namespace {

/// @brief 执行用户回调并吞掉异常：接收线程不能因为应用代码抛异常而 terminate。
template <typename Handler, typename... Args>
bool SafeInvoke(const Handler& handler, Args&&... args) noexcept {
    if (!handler) {
        return true;
    }
    try {
        handler(std::forward<Args>(args)...);
        return true;
    } catch (...) {
        return false;
    }
}

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

/// @brief 解析十进制端口；空串、非数字、>65535 均视为非法。
bool ParsePortText(const std::string& text, std::uint16_t* port) {
    if (text.empty()) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10U + static_cast<std::uint32_t>(c - '0');
        if (value > 65535U) {
            return false;
        }
    }
    *port = static_cast<std::uint16_t>(value);
    return true;
}

/// @brief 解析 "host:port" / "[v6]:port" / "host"。
/// @details 多个冒号且没有 `[]` 时按裸 IPv6 字面量处理（无端口），避免把
///          `::1` 里的最后一个 `1` 误当端口。要指定端口必须写 `[::1]:9000`。
bool ParseEndpoint(const std::string& endpoint, std::string* host, std::uint16_t* port,
                   std::string* error) {
    host->clear();
    *port = 0;
    if (endpoint.empty()) {
        SetError(error, "endpoint is empty");
        return false;
    }
    if (endpoint.front() == '[') {
        const std::size_t close = endpoint.find(']');
        if (close == std::string::npos) {
            SetError(error, "missing ']' in IPv6 endpoint");
            return false;
        }
        *host = endpoint.substr(1, close - 1);
        if (close + 1 < endpoint.size()) {
            if (endpoint[close + 1] != ':') {
                SetError(error, "unexpected characters after ']'");
                return false;
            }
            const std::string text = endpoint.substr(close + 2);
            if (!ParsePortText(text, port)) {
                SetError(error, "invalid port: " + text);
                return false;
            }
        }
    } else {
        const std::size_t colon = endpoint.rfind(':');
        if (colon == std::string::npos) {
            *host = endpoint;
        } else if (endpoint.find(':') != colon) {
            *host = endpoint;  // 裸 IPv6：无端口。
        } else {
            const std::string text = endpoint.substr(colon + 1);
            if (!ParsePortText(text, port)) {
                SetError(error, "invalid port: " + text);
                return false;
            }
            *host = endpoint.substr(0, colon);
        }
    }
    if (host->empty()) {
        SetError(error, "host is empty");
        return false;
    }
    return true;
}

}  // namespace

const char* LinkReliabilityName(LinkReliability reliability) noexcept {
    switch (reliability) {
        case LinkReliability::kReliable:
            return "reliable";
        case LinkReliability::kUnreliable:
            return "unreliable";
    }
    return "unknown";
}

const char* LinkSendResultName(LinkSendResult result) noexcept {
    switch (result) {
        case LinkSendResult::kOk:
            return "ok";
        case LinkSendResult::kWouldBlock:
            return "would-block";
        case LinkSendResult::kNotConnected:
            return "not-connected";
        case LinkSendResult::kInvalid:
            return "invalid";
        case LinkSendResult::kFailed:
            return "failed";
    }
    return "unknown";
}

LinkServer::LinkServer() = default;

LinkServer::~LinkServer() {
    Stop();
}

bool LinkServer::Listen(const std::string& address, std::uint16_t port,
                        const LinkServerConfig& config, std::string* error) {
    return ListenImpl(address, port, config, error);
}

bool LinkServer::Listen(const std::string& endpoint, const LinkServerConfig& config,
                        std::string* error) {
    std::string host;
    std::uint16_t port = 0;
    if (!ParseEndpoint(endpoint, &host, &port, error)) {
        return false;
    }
    return ListenImpl(host, port, config, error);
}

bool LinkServer::ListenImpl(const std::string& address, std::uint16_t port,
                            const LinkServerConfig& config, std::string* error) {
    if (m_open.load(std::memory_order_acquire)) {
        SetError(error, "server is already listening; call Stop() first");
        return false;
    }
    if (address.empty()) {
        SetError(error, "bind address is empty");
        return false;
    }

    // 上一轮若已 Stop，接收线程应已 join；这里再防御一次，保证重建 Hub 时
    // 没有线程还在使用旧对象。
    if (m_receiver.joinable()) {
        m_receiver.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_hub_mutex);
        m_hub.reset();
    }

    LinkServerConfig effective = config;
    effective.hub.bind_address = address;
    effective.hub.bind_port = port;
    effective.poll_timeout_ms =
        std::clamp<std::uint32_t>(effective.poll_timeout_ms, 1U, 1000U);

    auto hub = std::make_shared<PeerHub>(effective.hub);
    std::string open_error;
    if (!hub->Open(&open_error)) {
        SetError(error, open_error.empty() ? "failed to open UDP socket" : open_error);
        return false;
    }

    // 回调桥捕获 this；Hub 的寿命受 m_hub（shared_ptr）与接收线程 join 保护。
    hub->on_peer_connected = [this](std::uint32_t peer_id) { HandlePeerConnected(peer_id); };
    hub->on_peer_disconnected = [this](std::uint32_t peer_id, const char* reason) {
        HandlePeerDisconnected(peer_id, reason);
    };
    hub->on_stream = [this](std::uint32_t peer_id, std::uint8_t channel, const std::uint8_t* data,
                            std::size_t bytes) {
        HandleStream(peer_id, channel, data, bytes);
    };
    hub->on_datagram = [this](std::uint32_t peer_id, std::uint8_t channel,
                              const std::uint8_t* data, std::size_t bytes) {
        HandleDatagram(peer_id, channel, data, bytes);
    };

    {
        std::lock_guard<std::mutex> lock(m_hub_mutex);
        m_hub = hub;
    }
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_connected.clear();
        m_stats = LinkServerStats{};
    }
    m_config = effective;
    m_stop_requested.store(false, std::memory_order_release);
    m_teardown_started.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    m_local_port.store(hub->local_port(), std::memory_order_release);
    m_open.store(true, std::memory_order_release);
    return true;
}

bool LinkServer::Start() {
    const std::shared_ptr<PeerHub> hub = HubPointer();
    if (!hub || !hub->is_open()) {
        return false;
    }
    // CAS 保证并发 Start 只有一个线程真正起接收线程；重复调用返回 true。
    m_stop_requested.store(false, std::memory_order_release);
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return true;
    }
    if (m_config.start_receiver_thread) {
        if (m_receiver.joinable()) {
            m_receiver.join();
        }
        m_receiver = std::thread(&LinkServer::ReceiverLoop, this);
    }
    return true;
}

void LinkServer::Stop() {
    m_stop_requested.store(true, std::memory_order_release);
    const bool on_receiver_thread =
        m_receiver.joinable() && m_receiver.get_id() == std::this_thread::get_id();
    if (on_receiver_thread) {
        // 回调内自停：自 join 是未定义行为。只置状态，清理由接收线程退出前补做。
        m_running.store(false, std::memory_order_release);
        return;
    }
    if (m_receiver.joinable()) {
        m_receiver.join();
    }
    m_running.store(false, std::memory_order_release);
    Teardown();
}

bool LinkServer::is_open() const noexcept {
    return m_open.load(std::memory_order_acquire);
}

std::uint16_t LinkServer::local_port() const noexcept {
    return m_local_port.load(std::memory_order_acquire);
}

int LinkServer::RunOnce(int timeout_ms) {
    const std::shared_ptr<PeerHub> hub = HubPointer();
    if (!hub || !m_running.load(std::memory_order_acquire)) {
        return 0;
    }
    return hub->RunOnce(timeout_ms);
}

int LinkServer::RunFor(int duration_ms) {
    const std::shared_ptr<PeerHub> hub = HubPointer();
    if (!hub || !m_running.load(std::memory_order_acquire)) {
        return 0;
    }
    return hub->RunFor(duration_ms);
}

LinkSendResult LinkServer::Send(PeerId peer, std::uint8_t channel, const void* data,
                                std::size_t bytes, LinkReliability reliability) {
    LinkSendResult result = LinkSendResult::kInvalid;
    const std::shared_ptr<PeerHub> hub = HubPointer();
    if (!m_running.load(std::memory_order_acquire) || !hub) {
        result = LinkSendResult::kNotConnected;
    } else if (data == nullptr || bytes == 0) {
        result = LinkSendResult::kInvalid;
    } else if (reliability == LinkReliability::kReliable) {
        if (bytes > kMaxStreamMessageBytes) {
            result = LinkSendResult::kInvalid;
        } else if (!PeerKnown(peer)) {
            result = LinkSendResult::kNotConnected;
        } else {
            switch (hub->TrySendStream(peer, channel, data, bytes)) {
                case SendStatus::kOk:
                    result = LinkSendResult::kOk;
                    break;
                case SendStatus::kWouldBlock:
                    result = LinkSendResult::kWouldBlock;
                    break;
                case SendStatus::kError:
                    result = LinkSendResult::kFailed;
                    break;
            }
        }
    } else if (reliability == LinkReliability::kUnreliable) {
        if (bytes > kMaxFramePayload) {
            result = LinkSendResult::kInvalid;
        } else if (!PeerKnown(peer)) {
            result = LinkSendResult::kNotConnected;
        } else {
            result = hub->SendDatagram(peer, channel, data, bytes) ? LinkSendResult::kOk
                                                                   : LinkSendResult::kFailed;
        }
    } else {
        result = LinkSendResult::kInvalid;
    }
    RecordSend(result);
    return result;
}

std::size_t LinkServer::Broadcast(std::uint8_t channel, const void* data, std::size_t bytes,
                                  LinkReliability reliability) {
    if (data == nullptr || bytes == 0) {
        return 0;
    }
    const std::size_t max_bytes =
        reliability == LinkReliability::kReliable ? kMaxStreamMessageBytes : kMaxFramePayload;
    if (reliability != LinkReliability::kReliable &&
        reliability != LinkReliability::kUnreliable) {
        return 0;
    }
    if (bytes > max_bytes) {
        return 0;
    }
    const std::shared_ptr<PeerHub> hub = HubPointer();
    if (!m_running.load(std::memory_order_acquire) || !hub) {
        return 0;
    }

    const std::vector<PeerId> peers = ConnectedSnapshot();
    std::size_t delivered = 0;
    for (const PeerId peer : peers) {
        if (Send(peer, channel, data, bytes, reliability) == LinkSendResult::kOk) {
            ++delivered;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        ++m_stats.broadcasts;
        m_stats.broadcast_deliveries += delivered;
    }
    return delivered;
}

std::vector<LinkPeerId> LinkServer::Peers() const {
    return ConnectedSnapshot();
}

bool LinkServer::HasPeer(PeerId peer) const {
    return PeerKnown(peer);
}

std::size_t LinkServer::peer_count() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_connected.size();
}

PeerState LinkServer::peer_state(PeerId peer) const {
    const std::shared_ptr<PeerHub> hub = HubPointer();
    if (!hub) {
        return PeerState::kClosed;
    }
    return hub->peer_state(peer);
}

void LinkServer::OnPeerConnected(ConnectHandler handler) {
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_on_connected = std::move(handler);
}

void LinkServer::OnPeerDisconnected(DisconnectHandler handler) {
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_on_disconnected = std::move(handler);
}

void LinkServer::OnStream(MessageHandler handler) {
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_on_stream = std::move(handler);
}

void LinkServer::OnDatagram(MessageHandler handler) {
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_on_datagram = std::move(handler);
}

LinkServerStats LinkServer::Stats() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    LinkServerStats snapshot = m_stats;
    snapshot.active_peers = m_connected.size();
    return snapshot;
}

PeerHubStats LinkServer::hub_stats() const {
    const std::shared_ptr<PeerHub> hub = HubPointer();
    if (!hub) {
        return PeerHubStats{};
    }
    return hub->stats();
}

std::shared_ptr<PeerHub> LinkServer::HubPointer() const {
    std::lock_guard<std::mutex> lock(m_hub_mutex);
    return m_hub;
}

void LinkServer::ReceiverLoop() {
    const int poll_ms = static_cast<int>(m_config.poll_timeout_ms);
    while (!m_stop_requested.load(std::memory_order_acquire)) {
        const std::shared_ptr<PeerHub> hub = HubPointer();
        if (!hub) {
            break;
        }
        hub->RunOnce(poll_ms);
    }
    // 外部 Stop 在 join 后会调用一次 Teardown；回调内自停时由这里补做。
    // exchange 保证两处只有一处真正执行。
    Teardown();
    m_running.store(false, std::memory_order_release);
}

void LinkServer::Teardown() {
    bool expected = false;
    if (!m_teardown_started.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel)) {
        return;
    }
    const std::shared_ptr<PeerHub> hub = HubPointer();
    if (!hub) {
        m_open.store(false, std::memory_order_release);
        m_local_port.store(0, std::memory_order_release);
        return;
    }
    // 先触发每个 peer 的断开回调，再关 socket：应用能在回调里拿到 peer_id
    // 做 per-peer 清理（reason 固定为 PeerHub 的 "removed"）。
    const std::vector<PeerId> peers = ConnectedSnapshot();
    for (const PeerId peer : peers) {
        hub->RemovePeer(peer);
    }
    hub->Close();
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_connected.clear();
    }
    m_open.store(false, std::memory_order_release);
    m_local_port.store(0, std::memory_order_release);
}

void LinkServer::HandlePeerConnected(std::uint32_t peer_id) {
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_connected.insert(peer_id);
        ++m_stats.peers_accepted;
        m_stats.active_peers = m_connected.size();
    }
    ConnectHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        handler = m_on_connected;
    }
    if (!InvokeSafely(handler, peer_id)) {
        IncrementCallbackExceptions();
    }
}

void LinkServer::HandlePeerDisconnected(std::uint32_t peer_id, const char* reason) {
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        known = m_connected.erase(peer_id) > 0;
        m_stats.active_peers = m_connected.size();
        if (known) {
            ++m_stats.peers_disconnected;
        }
    }
    if (!known) {
        // 握手未完成就失败的 peer 从未对应用可见，不制造「没连上先断线」回调。
        return;
    }
    DisconnectHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        handler = m_on_disconnected;
    }
    if (!InvokeSafely(handler, peer_id, reason)) {
        IncrementCallbackExceptions();
    }
}

void LinkServer::HandleStream(std::uint32_t peer_id, std::uint8_t channel,
                              const std::uint8_t* data, std::size_t bytes) {
    MessageHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        handler = m_on_stream;
    }
    if (!handler) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        ++m_stats.stream_messages_received;
    }
    if (!InvokeSafely(handler, peer_id, channel, data, bytes)) {
        IncrementCallbackExceptions();
    }
}

void LinkServer::HandleDatagram(std::uint32_t peer_id, std::uint8_t channel,
                                const std::uint8_t* data, std::size_t bytes) {
    MessageHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        handler = m_on_datagram;
    }
    if (!handler) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        ++m_stats.datagram_messages_received;
    }
    if (!InvokeSafely(handler, peer_id, channel, data, bytes)) {
        IncrementCallbackExceptions();
    }
}

bool LinkServer::PeerKnown(std::uint32_t peer_id) const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_connected.find(peer_id) != m_connected.end();
}

std::vector<std::uint32_t> LinkServer::ConnectedSnapshot() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return std::vector<std::uint32_t>(m_connected.begin(), m_connected.end());
}

void LinkServer::RecordSend(LinkSendResult result) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    switch (result) {
        case LinkSendResult::kOk:
            ++m_stats.sends_ok;
            break;
        case LinkSendResult::kWouldBlock:
            ++m_stats.sends_would_block;
            break;
        case LinkSendResult::kNotConnected:
            ++m_stats.sends_not_connected;
            break;
        case LinkSendResult::kInvalid:
            ++m_stats.sends_invalid;
            break;
        case LinkSendResult::kFailed:
            ++m_stats.sends_failed;
            break;
    }
}

bool LinkServer::InvokeSafely(const ConnectHandler& handler, std::uint32_t peer_id) const noexcept {
    return SafeInvoke(handler, peer_id);
}

bool LinkServer::InvokeSafely(const DisconnectHandler& handler, std::uint32_t peer_id,
                              const char* reason) const noexcept {
    return SafeInvoke(handler, peer_id, reason);
}

bool LinkServer::InvokeSafely(const MessageHandler& handler, std::uint32_t peer_id,
                              std::uint8_t channel, const std::uint8_t* data,
                              std::size_t bytes) const noexcept {
    return SafeInvoke(handler, peer_id, channel, data, bytes);
}

void LinkServer::IncrementCallbackExceptions() {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    ++m_stats.callback_exceptions;
}

}  // namespace go2cpp::bento
