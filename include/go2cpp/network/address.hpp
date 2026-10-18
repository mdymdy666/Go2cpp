#pragma once

/**
 * @file address.hpp
 * @brief IPv4、IPv6、Unix 域地址和 DNS 查询抽象。
 * @details Address 只拥有地址值，不拥有 Socket。Lookup 返回多个地址以保留
 *          getaddrinfo 的顺序，调用方可自行选择 IPv4/IPv6 或尝试回退。
 *
 * ## 模块定位
 * 地址值对象：IPv4 / IPv6 / Unix / Unknown 的多态包装，加上基于 `getaddrinfo` 的
 * DNS 查询。被 `Socket`、服务端/客户端与示例使用；**Address 只拥有地址值，不拥有 Socket**。
 *
 * ## 本文件定义什么
 * - `Address`：sockaddr 多态基类 + `Create()` / `Lookup*()` 工厂；
 * - `IPAddress`：带端口的 IP 接口（含子网运算）；
 * - `IPv4Address` / `IPv6Address` / `UnixAddress` / `UnknownAddress` 四个实现。
 *
 * ## 阅读建议
 * 先看 `Address::Create()` 的 family 分派（长度校验是安全关键），再看 `Lookup()`
 * 如何保留 `getaddrinfo` 的顺序，最后看 IPv4/IPv6 的 prefix 掩码运算。
 *
 * ## 边界与陷阱
 * - `Lookup()` 用 `AI_ADDRCONFIG` 并保留**全部**候选（IPv4/IPv6 的选择与回退交给调用方）；
 * - `UnixAddress` 的长度规则：文件系统路径要留结尾 NUL，抽象命名空间的首 NUL 占一字节；
 * - `UnknownAddress` 会把超长输入截到 `sockaddr_storage` 大小；
 * - `Create()` 对长度不足的 sockaddr 返回空指针，而不是构造半个对象。
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>

namespace go2cpp::net {

/// 前置声明：定义在本文件下方（sockaddr 多态包装）。
class Address;
using AddressPtr = std::shared_ptr<Address>;

/** @brief sockaddr 的多态只读/可写包装。 */
/**
 * @brief sockaddr 的多态只读/可写包装。
 *
 * ## 定位
 * 被 `Socket` 的 bind/connect/accept 与 `Address::Lookup*()` 使用；四个子类分别
 * 对应 IPv4/IPv6/Unix/未知地址族。
 *
 * ## 不变量
 * - `addr()`/`addr_len()` 指向**本对象内部**的 sockaddr 副本，生命周期随对象；
 * - 基类不拥有 fd，也不做任何系统调用（除 `Lookup` 的 getaddrinfo）；
 * - `operator<` 先比 family 再比二进制内容，保证排序稳定。
 */
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
/**
 * @brief 带端口的 IP 地址通用接口。
 *
 * ## 定位
 * 被需要端口与子网运算的调用方使用（`Socket` 的端口设置、网段计算）；
 * `LookupAnyIPAddress()` 会把结果向下转型成它。
 *
 * ## 不变量
 * 子网运算不修改自身；`prefix_len` 越界返回空指针（IPv4 > 32、IPv6 > 128）。
 */
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
/**
 * @brief IPv4 地址。prefix_len 取值 0 到 32。
 *
 * ## 定位
 * `Address::Create()` 的 AF_INET 分支产物；`Socket` 的 IPv4 连接/监听使用。
 *
 * ## 不变量
 * `m_addr.sin_family` 恒为 AF_INET；端口始终以**主机序**进出（内部用 htons/ntohs）。
 */
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
    /// IPv4 sockaddr 副本；构造时填 family/addr/port，addr() 返回其指针。
    sockaddr_in m_addr{};
};

/** @brief IPv6 地址，保留 scope id 以支持链路本地地址。 */
/**
 * @brief IPv6 地址，保留 scope id 以支持链路本地地址。
 *
 * ## 定位
 * `Address::Create()` 的 AF_INET6 分支产物；链路本地地址靠 `scope_id` 区分接口。
 *
 * ## 不变量
 * `m_addr.sin6_family` 恒为 AF_INET6；`to_string()` 输出 `[addr%scope]:port`。
 */
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
    /// IPv6 sockaddr 副本；含 scope_id，addr() 返回其指针。
    sockaddr_in6 m_addr{};
};

/** @brief Unix 域套接字地址，支持抽象命名空间。 */
/**
 * @brief Unix 域套接字地址，支持抽象命名空间。
 *
 * ## 定位
 * 本地 IPC 与服务端测试使用；`m_length` 是 sockaddr_un 的**有效**长度（不是 sizeof）。
 *
 * ## 不变量
 * - 文件系统路径不以 NUL 开头且长度 < `sizeof(sun_path)`；
 * - 抽象命名空间以 NUL 开头，长度不超过 `sizeof(sun_path)`；
 * - `addr_len()` 返回 `offsetof(sun_path) + 路径长度 (+1)`。
 */
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
    /// Unix sockaddr 副本；sun_path 可能不含结尾 NUL（抽象命名空间）。
    sockaddr_un m_addr{};
    /// 有效长度（不是 sizeof）；构造时按路径计算，addr_len() 返回它。
    socklen_t m_length{sizeof(sockaddr_un)};
    /// 可见路径；抽象命名空间以 NUL 开头，to_string() 原样返回。
    std::string m_path;
};

/** @brief 无法识别的地址，用于保留系统扩展地址族的数据。 */
/**
 * @brief 无法识别的地址，用于保留系统扩展地址族的数据。
 *
 * ## 定位
 * `Address::Create()` 的兜底分支；用于保留系统扩展地址族，避免丢数据。
 *
 * ## 不变量
 * `m_length <= sizeof(sockaddr_storage)`；`m_addr` 是输入的**截断**副本。
 */
class UnknownAddress final : public Address {
public:
    UnknownAddress(const sockaddr* address, socklen_t length);
    int family() const noexcept override;
    const sockaddr* addr() const noexcept override;
    sockaddr* addr() noexcept override;
    socklen_t addr_len() const noexcept override;
    std::string to_string() const override;

private:
    /// 原始地址字节（截断到 storage 大小）；family()/addr() 读它。
    sockaddr_storage m_addr{};
    /// 有效长度；构造时 min(输入长度, sizeof(storage))。
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
