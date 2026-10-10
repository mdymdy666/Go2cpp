#pragma once

/**
 * @file address.hpp
 * @brief IPv4、IPv6、Unix 域地址和 DNS 查询抽象。
 * @details Address 只拥有地址值，不拥有 Socket。Lookup 返回多个地址以保留
 *          getaddrinfo 的顺序，调用方可自行选择 IPv4/IPv6 或尝试回退。
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>

namespace go2cpp::net {

class Address;
using AddressPtr = std::shared_ptr<Address>;

/** @brief sockaddr 的多态只读/可写包装。 */
class Address {
public:
    using Ptr = std::shared_ptr<Address>;
    virtual ~Address() = default;

    /** @brief 从系统 sockaddr 复制出正确的地址子类。 */
    static Ptr Create(const sockaddr* address, socklen_t length);
    /** @brief 解析 host:service，保留 getaddrinfo 返回的全部候选。 */
    static bool Lookup(std::vector<Ptr>& result, const std::string& host,
                       int family = AF_UNSPEC, int type = 0, int protocol = 0);
    /** @brief 解析并返回第一个地址。 */
    static Ptr LookupAny(const std::string& host, int family = AF_UNSPEC,
                         int type = 0, int protocol = 0);
    /** @brief 解析出 IP 地址并过滤 Unix/未知地址。 */
    static std::shared_ptr<class IPAddress> LookupAnyIPAddress(
        const std::string& host, int family = AF_UNSPEC, int type = 0,
        int protocol = 0);

    virtual int family() const noexcept = 0;
    virtual const sockaddr* addr() const noexcept = 0;
    virtual sockaddr* addr() noexcept = 0;
    virtual socklen_t addr_len() const noexcept = 0;
    virtual std::string to_string() const = 0;

    /** @brief 比较地址二进制内容，便于排序和测试。 */
    bool operator<(const Address& other) const noexcept;
    bool operator==(const Address& other) const noexcept;
    bool operator!=(const Address& other) const noexcept { return !(*this == other); }
};

/** @brief 带端口的 IP 地址通用接口。 */
class IPAddress : public Address {
public:
    using Ptr = std::shared_ptr<IPAddress>;
    virtual std::uint16_t port() const noexcept = 0;
    virtual void set_port(std::uint16_t value) noexcept = 0;
    virtual Ptr broadcast_address(std::uint32_t prefix_len) const = 0;
    virtual Ptr network_address(std::uint32_t prefix_len) const = 0;
    virtual Ptr subnet_mask(std::uint32_t prefix_len) const = 0;
    static Ptr Create(const char* address, std::uint16_t port = 0);
};

/** @brief IPv4 地址。prefix_len 取值 0 到 32。 */
class IPv4Address final : public IPAddress {
public:
    explicit IPv4Address(std::uint32_t address = INADDR_ANY,
                         std::uint16_t port = 0) noexcept;
    static std::shared_ptr<IPv4Address> Create(const char* address,
                                               std::uint16_t port = 0);
    int family() const noexcept override;
    const sockaddr* addr() const noexcept override;
    sockaddr* addr() noexcept override;
    socklen_t addr_len() const noexcept override;
    std::string to_string() const override;
    std::uint16_t port() const noexcept override;
    void set_port(std::uint16_t value) noexcept override;
    Ptr broadcast_address(std::uint32_t prefix_len) const override;
    Ptr network_address(std::uint32_t prefix_len) const override;
    Ptr subnet_mask(std::uint32_t prefix_len) const override;

private:
    sockaddr_in m_addr{};
};

/** @brief IPv6 地址，保留 scope id 以支持链路本地地址。 */
class IPv6Address final : public IPAddress {
public:
    explicit IPv6Address(const in6_addr& address = in6addr_any,
                         std::uint16_t port = 0, std::uint32_t scope = 0) noexcept;
    static std::shared_ptr<IPv6Address> Create(const char* address,
                                               std::uint16_t port = 0);
    int family() const noexcept override;
    const sockaddr* addr() const noexcept override;
    sockaddr* addr() noexcept override;
    socklen_t addr_len() const noexcept override;
    std::string to_string() const override;
    std::uint16_t port() const noexcept override;
    void set_port(std::uint16_t value) noexcept override;
    Ptr broadcast_address(std::uint32_t prefix_len) const override;
    Ptr network_address(std::uint32_t prefix_len) const override;
    Ptr subnet_mask(std::uint32_t prefix_len) const override;

private:
    sockaddr_in6 m_addr{};
};

/** @brief Unix 域套接字地址，支持抽象命名空间。 */
class UnixAddress final : public Address {
public:
    explicit UnixAddress(const std::string& path = {});
    UnixAddress(const sockaddr_un& address, socklen_t length);
    int family() const noexcept override;
    const sockaddr* addr() const noexcept override;
    sockaddr* addr() noexcept override;
    socklen_t addr_len() const noexcept override;
    std::string to_string() const override;
    const std::string& path() const noexcept { return m_path; }

private:
    sockaddr_un m_addr{};
    socklen_t m_length{sizeof(sockaddr_un)};
    std::string m_path;
};

/** @brief 无法识别的地址，用于保留系统扩展地址族的数据。 */
class UnknownAddress final : public Address {
public:
    UnknownAddress(const sockaddr* address, socklen_t length);
    int family() const noexcept override;
    const sockaddr* addr() const noexcept override;
    sockaddr* addr() noexcept override;
    socklen_t addr_len() const noexcept override;
    std::string to_string() const override;

private:
    sockaddr_storage m_addr{};
    socklen_t m_length{0};
};

}  // namespace go2cpp::net

namespace go2cpp {
using net::Address;
using net::AddressPtr;
using net::IPAddress;
using net::IPv4Address;
using net::IPv6Address;
using net::UnixAddress;
using net::UnknownAddress;
}  // namespace go2cpp
