#pragma once

/**
 * @file mock_nat.hpp
 * @brief 测试用用户态 NAT 模拟器：验证 STUN NAT 类型检测与 UDP 打洞。
 * @details 为什么需要它：真实的 NAT 行为无法在单机复现，而打洞的成功/失败
 *          恰恰取决于 NAT 的「映射行为」与「过滤行为」。本模拟器实现四种
 *          经典 NAT：
 *
 *          | 行为 | 映射 | 过滤 |
 *          |------|------|------|
 *          | FullCone | 所有目标共用一个外部端口 | 不限制来源 |
 *          | Restricted | 所有目标共用一个外部端口 | 只允许曾发过包的 IP |
 *          | PortRestricted | 所有目标共用一个外部端口 | 只允许曾发过包的 IP:port |
 *          | Symmetric | 每个目标一个外部端口 | 只允许曾发过包的 IP:port |
 *
 *          客户端通过 `NattedTransport` 把「目标端点 + 载荷」封装后发给
 *          模拟器的内部入口；模拟器按行为改写源端口后转发，并把外部收到的
 *          包按映射回送给客户端。这样 STUN/TURN/ICE 的代码完全不需要感知
 *          NAT 的存在。
 *
 *          限制：只支持 IPv4；不做端口预测（Port-Preserving/Predictable）；
 *          没有 allocation 超时与 ICMP 处理。这些足以覆盖本模块的测试目标。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/transport.hpp"

namespace go2cpp::p2p::test {

enum class NatBehavior {
    kFullCone,
    kRestricted,
    kPortRestricted,
    kSymmetric,
};

inline const char* NatBehaviorName(NatBehavior behavior) noexcept {
    switch (behavior) {
        case NatBehavior::kFullCone:
            return "FullCone";
        case NatBehavior::kRestricted:
            return "Restricted";
        case NatBehavior::kPortRestricted:
            return "PortRestricted";
        case NatBehavior::kSymmetric:
            return "Symmetric";
    }
    return "Unknown";
}

/// 目标端点编码：family(1) + port(2) + address(4/16)。
inline void AppendEndpoint(std::vector<std::uint8_t>* out, const Endpoint& endpoint) {
    if (out == nullptr || !endpoint.valid()) {
        return;
    }
    const bool ipv4 = endpoint.IsIPv4();
    out->push_back(ipv4 ? 1 : 2);
    const std::uint16_t port = endpoint.port();
    out->push_back(static_cast<std::uint8_t>(port >> 8U));
    out->push_back(static_cast<std::uint8_t>(port & 0xFFU));
    const auto bytes = endpoint.AddressBytes();
    out->insert(out->end(), bytes.begin(), bytes.begin() + (ipv4 ? 4 : 16));
}

inline bool ReadEndpoint(const std::uint8_t* data, std::size_t length, std::size_t* offset,
                         Endpoint* endpoint) {
    if (data == nullptr || offset == nullptr || endpoint == nullptr || *offset + 3 > length) {
        return false;
    }
    const std::uint8_t family = data[*offset];
    const std::uint16_t port =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[*offset + 1]) << 8U) |
                                   static_cast<std::uint16_t>(data[*offset + 2]));
    const std::size_t address_length = family == 1 ? 4 : (family == 2 ? 16 : 0);
    if (address_length == 0 || *offset + 3 + address_length > length) {
        return false;
    }
    *endpoint = Endpoint::FromAddressBytes(data + *offset + 3, address_length, port);
    *offset += 3 + address_length;
    return endpoint->valid();
}

/**
 * @brief 把「目标端点 + 载荷」封装后发给 NAT 内部入口的传输实现。
 *
 * @details 对上层（STUN/ICE/P2PChannel）来说，`SendTo(dst, payload)` 的语义
 *          就是「向 dst 发一个数据报」；`RecvFrom` 返回的源地址是 NAT 外部
 *          观察到的对端地址——与真实 NAT 后完全一致。
 */
class NattedTransport final : public DatagramTransport {
public:
    explicit NattedTransport(Endpoint nat_endpoint) : m_nat_endpoint(std::move(nat_endpoint)) {}

    bool Start(std::string* error = nullptr) {
        UdpTransportConfig config;
        config.bind_endpoint = Endpoint::Loopback(0);
        config.receive_buffer_bytes = 2U << 20U;
        m_socket = std::make_unique<UdpTransport>(config);
        return m_socket->Start(error);
    }

    bool SendTo(const Endpoint& destination, const void* data, std::size_t length) override {
        if (!m_socket || !destination.valid()) {
            return false;
        }
        std::vector<std::uint8_t> packet;
        packet.reserve(3 + 16 + length);
        AppendEndpoint(&packet, destination);
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        if (length > 0 && bytes != nullptr) {
            packet.insert(packet.end(), bytes, bytes + length);
        }
        return m_socket->SendTo(m_nat_endpoint, packet.data(), packet.size());
    }

    int RecvFrom(Endpoint* source, void* buffer, std::size_t capacity,
                 std::chrono::milliseconds timeout) override {
        if (!m_socket) {
            return -1;
        }
        std::vector<std::uint8_t> packet(65536);
        Endpoint nat_source;
        const int received =
            m_socket->RecvFrom(&nat_source, packet.data(), packet.size(), timeout);
        if (received <= 0) {
            return received;
        }
        std::size_t offset = 0;
        Endpoint original_source;
        if (!ReadEndpoint(packet.data(), static_cast<std::size_t>(received), &offset,
                          &original_source)) {
            return 0;
        }
        const std::size_t payload_length = static_cast<std::size_t>(received) - offset;
        const std::size_t copied = std::min(payload_length, capacity);
        if (copied > 0) {
            std::memcpy(buffer, packet.data() + offset, copied);
        }
        if (source != nullptr) {
            *source = original_source;
        }
        return static_cast<int>(copied);
    }

    Endpoint LocalEndpoint() const override {
        return m_socket ? m_socket->LocalEndpoint() : Endpoint();
    }

    bool SetReceiveBufferSize(std::size_t bytes) override {
        return m_socket && m_socket->SetReceiveBufferSize(bytes);
    }

private:
    Endpoint m_nat_endpoint;
    std::unique_ptr<UdpTransport> m_socket;
};

/**
 * @brief 用户态 NAT 模拟器。
 *
 * @details 生命周期：`Start()` 后 `endpoint()` 与 `CreateClientTransport()`
 *          可用；`Stop()` 会 join 线程并关闭全部外部 socket。所有客户端
 *          共用一个内部入口，按源端点区分；每个客户端一个 mapping。
 */
class MockNat {
public:
    explicit MockNat(NatBehavior behavior) : m_behavior(behavior) {}

    ~MockNat() { Stop(); }

    MockNat(const MockNat&) = delete;
    MockNat& operator=(const MockNat&) = delete;

    bool Start(std::string* error = nullptr) {
        int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            if (error != nullptr) {
                *error = std::string("socket: ") + std::strerror(errno);
            }
            return false;
        }
        const int buffer_size = 2 << 20;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
        const Endpoint local = Endpoint::Loopback(0);
        if (::bind(fd, local.sockaddr_ptr(), local.sockaddr_length()) != 0) {
            const int saved = errno;
            ::close(fd);
            if (error != nullptr) {
                *error = std::string("bind: ") + std::strerror(saved);
            }
            return false;
        }
        sockaddr_storage storage{};
        socklen_t length = sizeof(storage);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
            const int saved = errno;
            ::close(fd);
            if (error != nullptr) {
                *error = std::string("getsockname: ") + std::strerror(saved);
            }
            return false;
        }
        m_internal_fd = fd;
        m_internal_endpoint = Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&storage), length);
        m_stop.store(false);
        m_thread = std::thread([this]() { Run(); });
        return true;
    }

    void Stop() {
        if (m_internal_fd < 0) {
            return;
        }
        m_stop.store(true);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        for (auto& entry : m_mappings) {
            for (const auto& fd_entry : entry.second->external_by_fd) {
                ::close(fd_entry.first);
            }
        }
        m_mappings.clear();
        m_fd_client.clear();
        if (m_internal_fd >= 0) {
            ::close(m_internal_fd);
            m_internal_fd = -1;
        }
    }

    /// 客户端应把封装后的数据报发往这个内部入口。
    Endpoint endpoint() const { return m_internal_endpoint; }

    NatBehavior behavior() const { return m_behavior; }

    std::shared_ptr<NattedTransport> CreateClientTransport(std::string* error = nullptr) {
        auto transport = std::make_shared<NattedTransport>(m_internal_endpoint);
        if (!transport->Start(error)) {
            return nullptr;
        }
        return transport;
    }

    std::uint64_t forwarded_packets() const { return m_forwarded.load(); }
    std::uint64_t dropped_packets() const { return m_dropped.load(); }

private:
    struct Mapping {
        Endpoint client;
        int default_fd = -1;
        std::unordered_map<int, Endpoint> external_by_fd;
        std::unordered_map<std::string, int> fd_by_target;
        std::set<std::string> allowed_ips;
        std::set<Endpoint> allowed_peers;
    };

    int CreateExternalSocket(Endpoint* endpoint) {
        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }
        const int buffer_size = 2 << 20;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
        const Endpoint local = Endpoint::Loopback(0);
        if (::bind(fd, local.sockaddr_ptr(), local.sockaddr_length()) != 0) {
            ::close(fd);
            return -1;
        }
        sockaddr_storage storage{};
        socklen_t length = sizeof(storage);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
            ::close(fd);
            return -1;
        }
        *endpoint = Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&storage), length);
        return fd;
    }

    Mapping* GetOrCreateMapping(const Endpoint& client) {
        auto existing = m_mappings.find(client);
        if (existing != m_mappings.end()) {
            return existing->second.get();
        }
        auto mapping = std::make_unique<Mapping>();
        mapping->client = client;
        Endpoint external;
        const int fd = CreateExternalSocket(&external);
        if (fd < 0) {
            return nullptr;
        }
        mapping->default_fd = fd;
        mapping->external_by_fd.emplace(fd, external);
        m_fd_client.emplace(fd, client);
        Mapping* raw = mapping.get();
        m_mappings.emplace(client, std::move(mapping));
        return raw;
    }

    int ExternalFdFor(Mapping* mapping, const Endpoint& target, Endpoint* external) {
        if (m_behavior != NatBehavior::kSymmetric) {
            *external = mapping->external_by_fd.begin()->second;
            return mapping->default_fd;
        }
        const std::string key = target.ToString();
        auto existing = mapping->fd_by_target.find(key);
        if (existing != mapping->fd_by_target.end()) {
            *external = mapping->external_by_fd[existing->second];
            return existing->second;
        }
        Endpoint created;
        const int fd = CreateExternalSocket(&created);
        if (fd < 0) {
            return -1;
        }
        mapping->fd_by_target.emplace(key, fd);
        mapping->external_by_fd.emplace(fd, created);
        m_fd_client.emplace(fd, mapping->client);
        *external = created;
        return fd;
    }

    bool SourceAllowed(const Mapping& mapping, const Endpoint& source) const {
        switch (m_behavior) {
            case NatBehavior::kFullCone:
                return true;
            case NatBehavior::kRestricted:
                return mapping.allowed_ips.count(source.AddressString()) > 0;
            case NatBehavior::kPortRestricted:
            case NatBehavior::kSymmetric:
                return mapping.allowed_peers.count(source) > 0;
        }
        return false;
    }

    void Run() {
        while (!m_stop.load()) {
            std::vector<pollfd> descriptors;
            std::vector<int> kinds;
            descriptors.push_back(pollfd{m_internal_fd, POLLIN, 0});
            kinds.push_back(-1);
            for (const auto& entry : m_mappings) {
                for (const auto& fd_entry : entry.second->external_by_fd) {
                    descriptors.push_back(pollfd{fd_entry.first, POLLIN, 0});
                    kinds.push_back(fd_entry.first);
                }
            }
            const int ready = ::poll(descriptors.data(),
                                     static_cast<nfds_t>(descriptors.size()), 20);
            if (ready <= 0) {
                continue;
            }
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                if ((descriptors[index].revents & POLLIN) == 0) {
                    continue;
                }
                if (kinds[index] < 0) {
                    HandleFromClient();
                } else {
                    HandleFromPeer(descriptors[index].fd);
                }
            }
        }
    }

    void HandleFromClient() {
        std::vector<std::uint8_t> packet(65536);
        sockaddr_storage storage{};
        socklen_t length = sizeof(storage);
        const ssize_t received =
            ::recvfrom(m_internal_fd, packet.data(), packet.size(), 0,
                       reinterpret_cast<sockaddr*>(&storage), &length);
        if (received <= 0) {
            return;
        }
        const Endpoint client =
            Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&storage), length);
        if (!client.valid()) {
            return;
        }
        std::size_t offset = 0;
        Endpoint target;
        if (!ReadEndpoint(packet.data(), static_cast<std::size_t>(received), &offset, &target)) {
            ++m_dropped;
            return;
        }
        Mapping* mapping = GetOrCreateMapping(client);
        if (mapping == nullptr) {
            ++m_dropped;
            return;
        }
        Endpoint external;
        const int fd = ExternalFdFor(mapping, target, &external);
        if (fd < 0) {
            ++m_dropped;
            return;
        }
        if (m_behavior == NatBehavior::kRestricted) {
            mapping->allowed_ips.insert(target.AddressString());
        } else if (m_behavior == NatBehavior::kPortRestricted ||
                   m_behavior == NatBehavior::kSymmetric) {
            mapping->allowed_peers.insert(target);
        }
        const std::size_t payload_length = static_cast<std::size_t>(received) - offset;
        const ssize_t sent = ::sendto(fd, packet.data() + offset, payload_length, 0,
                                      target.sockaddr_ptr(), target.sockaddr_length());
        if (sent == static_cast<ssize_t>(payload_length)) {
            ++m_forwarded;
        } else {
            ++m_dropped;
        }
    }

    void HandleFromPeer(int fd) {
        auto owner = m_fd_client.find(fd);
        if (owner == m_fd_client.end()) {
            return;
        }
        const Endpoint client = owner->second;
        auto mapping_entry = m_mappings.find(client);
        if (mapping_entry == m_mappings.end()) {
            return;
        }
        Mapping& mapping = *mapping_entry->second;
        std::vector<std::uint8_t> payload(65536);
        sockaddr_storage storage{};
        socklen_t length = sizeof(storage);
        const ssize_t received =
            ::recvfrom(fd, payload.data(), payload.size(), 0,
                       reinterpret_cast<sockaddr*>(&storage), &length);
        if (received <= 0) {
            return;
        }
        const Endpoint source =
            Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&storage), length);
        if (!SourceAllowed(mapping, source)) {
            ++m_dropped;
            return;
        }
        std::vector<std::uint8_t> packet;
        packet.reserve(3 + 16 + static_cast<std::size_t>(received));
        AppendEndpoint(&packet, source);
        packet.insert(packet.end(), payload.begin(), payload.begin() + received);
        const ssize_t sent = ::sendto(m_internal_fd, packet.data(), packet.size(), 0,
                                      client.sockaddr_ptr(), client.sockaddr_length());
        if (sent == static_cast<ssize_t>(packet.size())) {
            ++m_forwarded;
        } else {
            ++m_dropped;
        }
    }

    NatBehavior m_behavior;
    int m_internal_fd = -1;
    Endpoint m_internal_endpoint;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::unordered_map<Endpoint, std::unique_ptr<Mapping>, EndpointHash> m_mappings;
    std::unordered_map<int, Endpoint> m_fd_client;
    std::atomic<std::uint64_t> m_forwarded{0};
    std::atomic<std::uint64_t> m_dropped{0};
};

}  // namespace go2cpp::p2p::test
