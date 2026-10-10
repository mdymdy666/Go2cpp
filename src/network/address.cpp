/**
 * @file address.cpp
 * @brief 实现地址族构造、DNS 查询和 IPv4/IPv6 掩码运算。
 * @details 本实现只复制 sockaddr，不持有 Socket；所有长度和前缀都会先校验，
 *          避免 Unix 路径、IPv6 scope 或掩码计算越界。
 */
#include "go2cpp/network/address.hpp"
#include "go2cpp/network/endian.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <sstream>
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string_view>

namespace go2cpp::net {

namespace {
std::string service_host(const std::string& host) {
    if (!host.empty() && host.front() == '[') {
        const auto close = host.find(']');
        return close == std::string::npos ? host : host.substr(1, close - 1);
    }
    const auto colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) return host.substr(0, colon);
    return host;
}
std::string service_port(const std::string& host) {
    if (!host.empty() && host.front() == '[') {
        const auto close = host.find(']');
        if (close != std::string::npos && close + 1 < host.size() && host[close + 1] == ':')
            return host.substr(close + 2);
        return {};
    }
    const auto colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) return host.substr(colon + 1);
    return {};
}
}

Address::Ptr Address::Create(const sockaddr* address, socklen_t length) {
    if (!address || length < sizeof(sa_family_t)) return {};
    switch (address->sa_family) {
        case AF_INET:
            if (length >= sizeof(sockaddr_in)) {
                const auto* value = reinterpret_cast<const sockaddr_in*>(address);
                return std::make_shared<IPv4Address>(value->sin_addr.s_addr,
                                                      ntohs(value->sin_port));
            }
            break;
        case AF_INET6:
            if (length >= sizeof(sockaddr_in6)) {
                const auto* value = reinterpret_cast<const sockaddr_in6*>(address);
                return std::make_shared<IPv6Address>(value->sin6_addr,
                                                      ntohs(value->sin6_port), value->sin6_scope_id);
            }
            break;
        case AF_UNIX:
            // sockaddr_un 的有效长度可以小于 sizeof(sockaddr_un)，调用方
            // 传入的缓冲区也可能恰好只有 length 字节，不能直接按整结构引用。
            if (length < offsetof(sockaddr_un, sun_path) || length > sizeof(sockaddr_un))
                return {};
            {
                sockaddr_un copied{};
                std::memcpy(&copied, address, length);
                return std::make_shared<UnixAddress>(copied, length);
            }
        default:
            break;
    }
    return std::make_shared<UnknownAddress>(address, length);
}

bool Address::Lookup(std::vector<Ptr>& result, const std::string& host,
                     int family, int type, int protocol) {
    result.clear();
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = type;
    hints.ai_protocol = protocol;
    hints.ai_flags = AI_ADDRCONFIG;
    const std::string node = service_host(host);
    const std::string service = service_port(host);
    addrinfo* list = nullptr;
    const int error = ::getaddrinfo(node.empty() ? nullptr : node.c_str(),
                                    service.empty() ? nullptr : service.c_str(),
                                    &hints, &list);
    if (error != 0) return false;
    for (auto* item = list; item; item = item->ai_next) {
        auto address = Create(item->ai_addr, static_cast<socklen_t>(item->ai_addrlen));
        if (address) result.push_back(std::move(address));
    }
    ::freeaddrinfo(list);
    return !result.empty();
}

Address::Ptr Address::LookupAny(const std::string& host, int family, int type, int protocol) {
    std::vector<Ptr> result;
    return Lookup(result, host, family, type, protocol) ? result.front() : Ptr{};
}

std::shared_ptr<IPAddress> Address::LookupAnyIPAddress(const std::string& host,
                                                        int family, int type,
                                                        int protocol) {
    std::vector<Ptr> result;
    if (!Lookup(result, host, family, type, protocol)) return {};
    for (auto& address : result) {
        if (auto ip = std::dynamic_pointer_cast<IPAddress>(address)) return ip;
    }
    return {};
}

bool Address::operator<(const Address& other) const noexcept {
    if (family() != other.family()) return family() < other.family();
    const auto length = std::min(addr_len(), other.addr_len());
    const int compare = std::memcmp(addr(), other.addr(), length);
    return compare < 0 || (compare == 0 && addr_len() < other.addr_len());
}
bool Address::operator==(const Address& other) const noexcept {
    return family() == other.family() && addr_len() == other.addr_len() &&
           std::memcmp(addr(), other.addr(), addr_len()) == 0;
}

IPv4Address::IPv4Address(std::uint32_t address, std::uint16_t port) noexcept {
    m_addr.sin_family = AF_INET;
    m_addr.sin_addr.s_addr = address;
    m_addr.sin_port = htons(port);
}
std::shared_ptr<IPv4Address> IPv4Address::Create(const char* address, std::uint16_t port) {
    in_addr value{};
    if (!address || ::inet_pton(AF_INET, address, &value) != 1) return {};
    return std::make_shared<IPv4Address>(value.s_addr, port);
}
int IPv4Address::family() const noexcept { return AF_INET; }
const sockaddr* IPv4Address::addr() const noexcept { return reinterpret_cast<const sockaddr*>(&m_addr); }
sockaddr* IPv4Address::addr() noexcept { return reinterpret_cast<sockaddr*>(&m_addr); }
socklen_t IPv4Address::addr_len() const noexcept { return sizeof(m_addr); }
std::string IPv4Address::to_string() const {
    char buffer[INET_ADDRSTRLEN]{};
    ::inet_ntop(AF_INET, &m_addr.sin_addr, buffer, sizeof(buffer));
    return std::string(buffer) + ":" + std::to_string(port());
}
std::uint16_t IPv4Address::port() const noexcept { return ntohs(m_addr.sin_port); }
void IPv4Address::set_port(std::uint16_t value) noexcept { m_addr.sin_port = htons(value); }
IPAddress::Ptr IPv4Address::broadcast_address(std::uint32_t prefix_len) const {
    if (prefix_len > 32) return {};
    const std::uint32_t host = ntohl(m_addr.sin_addr.s_addr);
    const std::uint32_t mask = prefix_len == 0 ? 0U : 0xffffffffU << (32 - prefix_len);
    return std::make_shared<IPv4Address>(htonl(host | ~mask), port());
}
IPAddress::Ptr IPv4Address::network_address(std::uint32_t prefix_len) const {
    if (prefix_len > 32) return {};
    const std::uint32_t host = ntohl(m_addr.sin_addr.s_addr);
    const std::uint32_t mask = prefix_len == 0 ? 0U : 0xffffffffU << (32 - prefix_len);
    return std::make_shared<IPv4Address>(htonl(host & mask), port());
}
IPAddress::Ptr IPv4Address::subnet_mask(std::uint32_t prefix_len) const {
    if (prefix_len > 32) return {};
    const std::uint32_t mask = prefix_len == 0 ? 0U : 0xffffffffU << (32 - prefix_len);
    return std::make_shared<IPv4Address>(htonl(mask), port());
}

IPv6Address::IPv6Address(const in6_addr& address, std::uint16_t port, std::uint32_t scope) noexcept {
    m_addr.sin6_family = AF_INET6;
    m_addr.sin6_addr = address;
    m_addr.sin6_port = htons(port);
    m_addr.sin6_scope_id = scope;
}
std::shared_ptr<IPv6Address> IPv6Address::Create(const char* address, std::uint16_t port) {
    if (!address) return {};
    std::string value(address);
    std::uint32_t scope = 0;
    const auto percent = value.find('%');
    if (percent != std::string::npos) {
        const std::string_view scope_text(value.data() + percent + 1,
                                          value.size() - percent - 1);
        if (scope_text.empty()) return {};
        for (const unsigned char character : scope_text)
            if (character < '0' || character > '9') return {};
        const auto parsed = std::from_chars(scope_text.data(),
                                            scope_text.data() + scope_text.size(), scope);
        if (parsed.ec != std::errc{} || parsed.ptr != scope_text.data() + scope_text.size())
            return {};
        value.resize(percent);
    }
    in6_addr parsed{};
    if (::inet_pton(AF_INET6, value.c_str(), &parsed) != 1) return {};
    return std::make_shared<IPv6Address>(parsed, port, scope);
}
int IPv6Address::family() const noexcept { return AF_INET6; }
const sockaddr* IPv6Address::addr() const noexcept { return reinterpret_cast<const sockaddr*>(&m_addr); }
sockaddr* IPv6Address::addr() noexcept { return reinterpret_cast<sockaddr*>(&m_addr); }
socklen_t IPv6Address::addr_len() const noexcept { return sizeof(m_addr); }
std::string IPv6Address::to_string() const {
    char buffer[INET6_ADDRSTRLEN]{};
    ::inet_ntop(AF_INET6, &m_addr.sin6_addr, buffer, sizeof(buffer));
    std::string result = "[" + std::string(buffer);
    if (m_addr.sin6_scope_id) result += "%" + std::to_string(m_addr.sin6_scope_id);
    return result + "]:" + std::to_string(port());
}
std::uint16_t IPv6Address::port() const noexcept { return ntohs(m_addr.sin6_port); }
void IPv6Address::set_port(std::uint16_t value) noexcept { m_addr.sin6_port = htons(value); }
IPAddress::Ptr IPv6Address::broadcast_address(std::uint32_t prefix_len) const {
    if (prefix_len > 128) return {};
    auto result = std::make_shared<IPv6Address>(*this);
    for (std::uint32_t bit = prefix_len; bit < 128; ++bit)
        result->m_addr.sin6_addr.s6_addr[bit / 8] |= static_cast<unsigned char>(1U << (7 - bit % 8));
    return result;
}
IPAddress::Ptr IPv6Address::network_address(std::uint32_t prefix_len) const {
    if (prefix_len > 128) return {};
    auto result = std::make_shared<IPv6Address>(*this);
    for (std::uint32_t bit = prefix_len; bit < 128; ++bit)
        result->m_addr.sin6_addr.s6_addr[bit / 8] &= static_cast<unsigned char>(~(1U << (7 - bit % 8)));
    return result;
}
IPAddress::Ptr IPv6Address::subnet_mask(std::uint32_t prefix_len) const {
    if (prefix_len > 128) return {};
    in6_addr mask{};
    for (std::uint32_t bit = 0; bit < prefix_len; ++bit)
        mask.s6_addr[bit / 8] |= static_cast<unsigned char>(1U << (7 - bit % 8));
    return std::make_shared<IPv6Address>(mask, port(), m_addr.sin6_scope_id);
}

UnixAddress::UnixAddress(const std::string& path) : m_path(path) {
    m_addr.sun_family = AF_UNIX;
    const bool abstract = !path.empty() && path.front() == '\0';
    const std::size_t path_capacity = sizeof(m_addr.sun_path);
    if (abstract) {
        // 抽象命名空间中的首个 NUL 本身占用 sun_path 的一个字节。
        if (path.size() > path_capacity) throw std::length_error("unix abstract path too long");
    } else {
        if (path.find('\0') != std::string::npos)
            throw std::invalid_argument("unix filesystem path contains NUL");
        // 文件系统路径还需要一个结尾 NUL，因此最多可放 capacity - 1 字节。
        if (path.size() >= path_capacity) throw std::length_error("unix path too long");
    }
    if (!path.empty()) std::memcpy(m_addr.sun_path, path.data(), path.size());
    m_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + (abstract ? 0 : 1));
}
UnixAddress::UnixAddress(const sockaddr_un& address, socklen_t length) : m_addr(address) {
    if (address.sun_family != AF_UNIX ||
        length < offsetof(sockaddr_un, sun_path) || length > sizeof(m_addr))
        throw std::invalid_argument("invalid sockaddr_un length");
    m_length = length;
    const std::size_t size = m_length > offsetof(sockaddr_un, sun_path)
        ? m_length - offsetof(sockaddr_un, sun_path) : 0;
    const std::size_t bounded_size = std::min(size, sizeof(m_addr.sun_path));
    m_path.assign(m_addr.sun_path, m_addr.sun_path + bounded_size);
    if (!m_path.empty() && m_path.front() != '\0') {
        const auto nul = m_path.find('\0');
        if (nul != std::string::npos) {
            // 文件系统命名空间允许一个结尾 NUL，但 NUL 后不能夹带
            // 非填充字节，否则地址的二进制长度与可见路径不一致。
            for (std::size_t i = nul + 1; i < m_path.size(); ++i)
                if (m_path[i] != '\0') throw std::invalid_argument("invalid unix path padding");
            m_path.resize(nul);
        }
    }
}
int UnixAddress::family() const noexcept { return AF_UNIX; }
const sockaddr* UnixAddress::addr() const noexcept { return reinterpret_cast<const sockaddr*>(&m_addr); }
sockaddr* UnixAddress::addr() noexcept { return reinterpret_cast<sockaddr*>(&m_addr); }
socklen_t UnixAddress::addr_len() const noexcept { return m_length; }
std::string UnixAddress::to_string() const { return m_path; }

UnknownAddress::UnknownAddress(const sockaddr* address, socklen_t length) {
    m_length = std::min<socklen_t>(length, sizeof(m_addr));
    if (address && m_length) std::memcpy(&m_addr, address, m_length);
}
int UnknownAddress::family() const noexcept { return m_addr.ss_family; }
const sockaddr* UnknownAddress::addr() const noexcept { return reinterpret_cast<const sockaddr*>(&m_addr); }
sockaddr* UnknownAddress::addr() noexcept { return reinterpret_cast<sockaddr*>(&m_addr); }
socklen_t UnknownAddress::addr_len() const noexcept { return m_length; }
std::string UnknownAddress::to_string() const { return "UnknownAddress[family=" + std::to_string(family()) + "]"; }

}  // namespace go2cpp::net
