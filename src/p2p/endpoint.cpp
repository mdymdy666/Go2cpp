/**
 * @file endpoint.cpp
 * @brief P2P 端点解析、比较与哈希的实现。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/endpoint.hpp"

#include <arpa/inet.h>
#include <netdb.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace go2cpp::p2p {
namespace {

constexpr std::size_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::size_t kFnvPrime = 1099511628211ULL;

void HashBytes(std::size_t* hash, const std::uint8_t* data, std::size_t length) noexcept {
    for (std::size_t index = 0; index < length; ++index) {
        *hash ^= static_cast<std::size_t>(data[index]);
        *hash *= kFnvPrime;
    }
}

bool ParsePort(std::string_view text, std::uint16_t* port) {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10U + static_cast<std::uint32_t>(ch - '0');
        if (value > 65535U) {
            return false;
        }
    }
    *port = static_cast<std::uint16_t>(value);
    return true;
}

}  // namespace

std::optional<Endpoint> Endpoint::Parse(std::string_view text) {
    std::string_view host;
    std::string_view port_text;
    if (text.size() >= 4 && text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos || close + 2 > text.size() || text[close + 1] != ':') {
            return std::nullopt;
        }
        host = text.substr(1, close - 1);
        port_text = text.substr(close + 2);
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
        // 裸 IPv6（含多个冒号）无法区分地址与端口，必须加方括号。
        if (host.find(':') != std::string_view::npos) {
            return std::nullopt;
        }
    }
    std::uint16_t port = 0;
    if (!ParsePort(port_text, &port)) {
        return std::nullopt;
    }
    if (host.find(':') != std::string_view::npos) {
        return FromIPv6(host, port);
    }
    return FromIPv4(host, port);
}

Endpoint Endpoint::FromSockaddr(const sockaddr* address, socklen_t length) {
    Endpoint endpoint;
    if (address == nullptr || length <= 0) {
        return endpoint;
    }
    if (address->sa_family == AF_INET) {
        if (static_cast<std::size_t>(length) < sizeof(sockaddr_in)) {
            return endpoint;
        }
        std::memcpy(&endpoint.m_storage, address, sizeof(sockaddr_in));
        endpoint.m_length = static_cast<socklen_t>(sizeof(sockaddr_in));
        return endpoint;
    }
    if (address->sa_family == AF_INET6) {
        if (static_cast<std::size_t>(length) < sizeof(sockaddr_in6)) {
            return endpoint;
        }
        std::memcpy(&endpoint.m_storage, address, sizeof(sockaddr_in6));
        endpoint.m_length = static_cast<socklen_t>(sizeof(sockaddr_in6));
        return endpoint;
    }
    return endpoint;
}

Endpoint Endpoint::FromIPv4(std::string_view host, std::uint16_t port) {
    Endpoint endpoint;
    std::string text(host);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, text.c_str(), &address.sin_addr) != 1) {
        return endpoint;
    }
    std::memcpy(&endpoint.m_storage, &address, sizeof(address));
    endpoint.m_length = static_cast<socklen_t>(sizeof(address));
    return endpoint;
}

Endpoint Endpoint::FromIPv6(std::string_view host, std::uint16_t port) {
    Endpoint endpoint;
    // 允许 `%scope` 后缀（例如 `fe80::1%eth0`），atoi 的 scope 交给内核。
    std::string text(host);
    std::uint32_t scope = 0;
    const std::size_t percent = text.find('%');
    if (percent != std::string::npos) {
        const std::string scope_text = text.substr(percent + 1);
        text.resize(percent);
        if (scope_text.empty()) {
            return endpoint;
        }
        char* end = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(scope_text.c_str(), &end, 10);
        if (errno != 0 || end == scope_text.c_str() || *end != '\0') {
            return endpoint;
        }
        scope = static_cast<std::uint32_t>(parsed);
    }
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_scope_id = scope;
    if (::inet_pton(AF_INET6, text.c_str(), &address.sin6_addr) != 1) {
        return endpoint;
    }
    std::memcpy(&endpoint.m_storage, &address, sizeof(address));
    endpoint.m_length = static_cast<socklen_t>(sizeof(address));
    return endpoint;
}

Endpoint Endpoint::Wildcard(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    Endpoint endpoint;
    std::memcpy(&endpoint.m_storage, &address, sizeof(address));
    endpoint.m_length = static_cast<socklen_t>(sizeof(address));
    return endpoint;
}

Endpoint Endpoint::Loopback(std::uint16_t port) {
    return FromIPv4("127.0.0.1", port);
}

Endpoint Endpoint::WithPort(std::uint16_t port) const {
    if (!valid()) {
        return Endpoint();
    }
    if (IsIPv4()) {
        sockaddr_in address{};
        std::memcpy(&address, &m_storage, sizeof(address));
        address.sin_port = htons(port);
        Endpoint endpoint;
        std::memcpy(&endpoint.m_storage, &address, sizeof(address));
        endpoint.m_length = static_cast<socklen_t>(sizeof(address));
        return endpoint;
    }
    sockaddr_in6 address{};
    std::memcpy(&address, &m_storage, sizeof(address));
    address.sin6_port = htons(port);
    Endpoint endpoint;
    std::memcpy(&endpoint.m_storage, &address, sizeof(address));
    endpoint.m_length = static_cast<socklen_t>(sizeof(address));
    return endpoint;
}

Endpoint Endpoint::FromAddressBytes(const std::uint8_t* address, std::size_t length,
                                    std::uint16_t port) {
    if (address == nullptr || (length != 4 && length != 16)) {
        return Endpoint();
    }
    Endpoint endpoint;
    if (length == 4) {
        sockaddr_in value{};
        value.sin_family = AF_INET;
        value.sin_port = htons(port);
        std::memcpy(&value.sin_addr, address, 4);
        std::memcpy(&endpoint.m_storage, &value, sizeof(value));
        endpoint.m_length = static_cast<socklen_t>(sizeof(value));
    } else {
        sockaddr_in6 value{};
        value.sin6_family = AF_INET6;
        value.sin6_port = htons(port);
        std::memcpy(&value.sin6_addr, address, 16);
        std::memcpy(&endpoint.m_storage, &value, sizeof(value));
        endpoint.m_length = static_cast<socklen_t>(sizeof(value));
    }
    return endpoint;
}

bool Endpoint::IsIPv4() const noexcept {
    return m_length != 0 && m_storage.ss_family == AF_INET;
}

bool Endpoint::IsIPv6() const noexcept {
    return m_length != 0 && m_storage.ss_family == AF_INET6;
}

sa_family_t Endpoint::family() const noexcept {
    return m_length != 0 ? m_storage.ss_family : AF_UNSPEC;
}

std::uint16_t Endpoint::port() const noexcept {
    if (IsIPv4()) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&m_storage)->sin_port);
    }
    if (IsIPv6()) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&m_storage)->sin6_port);
    }
    return 0;
}

std::string Endpoint::AddressString() const {
    if (!valid()) {
        return "<invalid>";
    }
    char buffer[INET6_ADDRSTRLEN] = {};
    if (IsIPv4()) {
        const auto* address = reinterpret_cast<const sockaddr_in*>(&m_storage);
        if (::inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)) == nullptr) {
            return "<invalid>";
        }
        return buffer;
    }
    const auto* address = reinterpret_cast<const sockaddr_in6*>(&m_storage);
    if (::inet_ntop(AF_INET6, &address->sin6_addr, buffer, sizeof(buffer)) == nullptr) {
        return "<invalid>";
    }
    return buffer;
}

std::string Endpoint::ToString() const {
    if (!valid()) {
        return "<invalid>";
    }
    const std::string address = AddressString();
    if (IsIPv6()) {
        return "[" + address + "]:" + std::to_string(port());
    }
    return address + ":" + std::to_string(port());
}

const sockaddr* Endpoint::sockaddr_ptr() const noexcept {
    return reinterpret_cast<const sockaddr*>(&m_storage);
}

socklen_t Endpoint::sockaddr_length() const noexcept {
    return m_length;
}

std::array<std::uint8_t, 16> Endpoint::AddressBytes() const noexcept {
    std::array<std::uint8_t, 16> bytes{};
    if (IsIPv4()) {
        const auto* address = reinterpret_cast<const sockaddr_in*>(&m_storage);
        std::memcpy(bytes.data(), &address->sin_addr, 4);
    } else if (IsIPv6()) {
        const auto* address = reinterpret_cast<const sockaddr_in6*>(&m_storage);
        std::memcpy(bytes.data(), &address->sin6_addr, 16);
    }
    return bytes;
}

bool Endpoint::IsUnspecified() const noexcept {
    if (IsIPv4()) {
        return reinterpret_cast<const sockaddr_in*>(&m_storage)->sin_addr.s_addr == htonl(INADDR_ANY);
    }
    if (IsIPv6()) {
        const auto* address = reinterpret_cast<const sockaddr_in6*>(&m_storage);
        return IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr);
    }
    return false;
}

bool Endpoint::IsLoopback() const noexcept {
    if (IsIPv4()) {
        const std::uint32_t host_order =
            ntohl(reinterpret_cast<const sockaddr_in*>(&m_storage)->sin_addr.s_addr);
        return (host_order >> 24U) == 127U;
    }
    if (IsIPv6()) {
        const auto* address = reinterpret_cast<const sockaddr_in6*>(&m_storage);
        return IN6_IS_ADDR_LOOPBACK(&address->sin6_addr);
    }
    return false;
}

bool Endpoint::operator==(const Endpoint& other) const noexcept {
    // 数据面每包都会比较「数据报来源 == 已锁定的 peer」。旧实现各构造一次
    // 16 字节 AddressBytes()（memset + memcpy）再逐字节比较；这里直接比
    // sockaddr 字段：IPv4 两次 load + 两个 32 位比较，IPv6 一次 16 字节
    // SSE 比较，省掉两次临时数组构造。
    if (m_length != other.m_length) {
        return false;
    }
    if (m_length == 0) {
        return true;
    }
    if (m_storage.ss_family != other.m_storage.ss_family) {
        return false;
    }
    if (m_storage.ss_family == AF_INET) {
        const auto* left = reinterpret_cast<const sockaddr_in*>(&m_storage);
        const auto* right = reinterpret_cast<const sockaddr_in*>(&other.m_storage);
        return left->sin_port == right->sin_port &&
               left->sin_addr.s_addr == right->sin_addr.s_addr;
    }
    if (m_storage.ss_family == AF_INET6) {
        const auto* left = reinterpret_cast<const sockaddr_in6*>(&m_storage);
        const auto* right = reinterpret_cast<const sockaddr_in6*>(&other.m_storage);
        return left->sin6_port == right->sin6_port &&
               std::memcmp(&left->sin6_addr, &right->sin6_addr, sizeof(in6_addr)) == 0;
    }
    return false;
}

bool Endpoint::operator<(const Endpoint& other) const noexcept {
    if (family() != other.family()) {
        return family() < other.family();
    }
    const auto left = AddressBytes();
    const auto right = other.AddressBytes();
    if (left != right) {
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
    }
    return port() < other.port();
}

std::size_t Endpoint::Hash() const noexcept {
    std::size_t hash = kFnvOffsetBasis;
    const std::uint8_t family_byte = static_cast<std::uint8_t>(family() & 0xFF);
    HashBytes(&hash, &family_byte, 1);
    const auto bytes = AddressBytes();
    HashBytes(&hash, bytes.data(), bytes.size());
    const std::uint8_t port_bytes[2] = {static_cast<std::uint8_t>(port() >> 8U),
                                        static_cast<std::uint8_t>(port() & 0xFFU)};
    HashBytes(&hash, port_bytes, sizeof(port_bytes));
    return hash;
}

std::optional<Endpoint> ResolveEndpoint(std::string_view host, std::uint16_t port,
                                        std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* result = nullptr;
    const std::string host_text(host);
    const std::string service = std::to_string(port);
    const int rc = ::getaddrinfo(host_text.c_str(), service.c_str(), &hints, &result);
    if (rc != 0) {
        if (error != nullptr) {
            *error = "getaddrinfo(" + host_text + ") 失败: " + ::gai_strerror(rc);
        }
        return std::nullopt;
    }
    std::optional<Endpoint> endpoint;
    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
        if (current->ai_family != AF_INET && current->ai_family != AF_INET6) {
            continue;
        }
        endpoint = Endpoint::FromSockaddr(current->ai_addr,
                                          static_cast<socklen_t>(current->ai_addrlen));
        if (endpoint->valid()) {
            break;
        }
    }
    ::freeaddrinfo(result);
    if (!endpoint.has_value() && error != nullptr) {
        *error = "getaddrinfo(" + host_text + ") 没有返回可用的 IPv4/IPv6 地址";
    }
    return endpoint;
}

}  // namespace go2cpp::p2p
