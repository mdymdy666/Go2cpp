#pragma once

/**
 * @file endpoint.hpp
 * @brief P2P 模块的传输端点：IPv4/IPv6 地址 + 端口的值类型。
 * @details 为什么不直接复用 `go2cpp::network::Address`：
 *
 *          - P2P 模块刻意**不依赖** network/runtime 目标，只依赖 libc 与
 *            pthread。这样它既能被主运行时链接，也能被独立工具/测试链接，
 *            且不会被 hook 的 syscall 拦截影响（hook 只在本进程加载后生效，
 *            独立测试不链接 hook）。
 *          - `Address` 是 DNS 解析 + 端口的重量级封装（含 getaddrinfo 缓存
 *            语义），而 P2P 的热路径只需要「字节地址 + 端口」的比较与哈希。
 *
 *          `Endpoint` 只接受**字面量** IP（`127.0.0.1:3478`、`[::1]:3478`）。
 *          需要域名时先调用方用 `ResolveEndpoint()` 做一次解析，避免在
 *          STUN/ICE 的候选比较路径里隐式触发阻塞 DNS。
 *
 *          裸 IPv6 地址必须写成 `[addr]:port`，与 URL/URI 的惯例一致：IPv6
 *          地址本身含冒号，不加方括号无法区分「地址」与「端口」。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace go2cpp::p2p {

/**
 * @brief 一个 IPv4/IPv6 传输端点（地址 + 端口）。
 *
 * @details 默认构造得到「无效端点」（`valid() == false`），用于表达
 *          「尚未收集到候选」「中继分配失败」等状态。有效端点一旦构造
 *          即不可变（没有 setter），因此可以安全地在多线程间按值拷贝。
 */
class Endpoint {
public:
    Endpoint() = default;

    /**
     * @brief 解析字面量端点文本。
     * @param text `a.b.c.d:port` 或 `[v6addr]:port`。
     * @return 成功时返回端点；格式非法、端口越界或 IP 字面量非法时返回
     *         `std::nullopt`。
     */
    static std::optional<Endpoint> Parse(std::string_view text);

    /// 从 `sockaddr`（`AF_INET`/`AF_INET6`）构造；其它 family 返回无效端点。
    static Endpoint FromSockaddr(const sockaddr* address, socklen_t length);

    /// 构造 IPv4 端点；`host` 必须是 `inet_pton` 可解析的字面量。
    static Endpoint FromIPv4(std::string_view host, std::uint16_t port);

    /// 构造 IPv6 端点；`host` 必须是 `inet_pton` 可解析的字面量（可带 `%scope`）。
    static Endpoint FromIPv6(std::string_view host, std::uint16_t port);

    /**
     * @brief 从 4/16 字节裸地址构造端点（STUN/TURN 属性解码用）。
     * @param address 4 字节（IPv4）或 16 字节（IPv6）。
     * @param length 必须是 4 或 16。
     * @param port 端口。
     * @return 长度非法时返回无效端点。
     */
    static Endpoint FromAddressBytes(const std::uint8_t* address, std::size_t length,
                                     std::uint16_t port);

    /// `0.0.0.0:port`（未指定地址，用作 bind 端点）。
    static Endpoint Wildcard(std::uint16_t port = 0);

    /// `127.0.0.1:port`。
    static Endpoint Loopback(std::uint16_t port = 0);

    /// 返回同地址、不同端口的端点（无效端点原样返回）。
    Endpoint WithPort(std::uint16_t port) const;

    bool valid() const noexcept { return m_length != 0; }

    bool IsIPv4() const noexcept;
    bool IsIPv6() const noexcept;
    sa_family_t family() const noexcept;

    /// 端口（主机字节序）。无效端点返回 0。
    std::uint16_t port() const noexcept;

    /// 地址文本（不含端口）；IPv6 不加方括号。
    std::string AddressString() const;

    /// `a.b.c.d:port` 或 `[v6]:port`；无效端点返回 `"<invalid>"`。
    std::string ToString() const;

    const sockaddr* sockaddr_ptr() const noexcept;
    socklen_t sockaddr_length() const noexcept;

    /**
     * @brief 地址的 16 字节表示（IPv4 占前 4 字节，其余补 0）。
     * @details STUN 的 XOR-MAPPED-ADDRESS 编解码需要这种定长表示：
     *          IPv4 的掩码是 magic cookie，IPv6 是 magic cookie + transaction id。
     */
    std::array<std::uint8_t, 16> AddressBytes() const noexcept;

    /// 是否为通配地址（`0.0.0.0` 或 `::`）。
    bool IsUnspecified() const noexcept;

    /// 是否为回环地址（`127.0.0.0/8` 或 `::1`）。
    bool IsLoopback() const noexcept;

    bool operator==(const Endpoint& other) const noexcept;
    bool operator!=(const Endpoint& other) const noexcept { return !(*this == other); }
    bool operator<(const Endpoint& other) const noexcept;

    /// FNV-1a 哈希，用于 `unordered_map`/`unordered_set`。
    std::size_t Hash() const noexcept;

private:
    sockaddr_storage m_storage{};
    socklen_t m_length = 0;
};

struct EndpointHash {
    std::size_t operator()(const Endpoint& endpoint) const noexcept { return endpoint.Hash(); }
};

/**
 * @brief 用 `getaddrinfo` 解析 `host:port`（支持域名与字面量）。
 * @param host 主机名或 IP 字面量（不含端口）。
 * @param port 端口。
 * @param error 失败时写入可读原因（可为空）。
 * @return 解析到的第一个 `AF_INET`/`AF_INET6` 端点；失败返回 `nullopt`。
 * @note 这个函数会阻塞（DNS），不要在热路径调用；STUN/TURN 服务器地址应
 *       在初始化阶段解析一次。
 */
std::optional<Endpoint> ResolveEndpoint(std::string_view host, std::uint16_t port,
                                        std::string* error = nullptr);

}  // namespace go2cpp::p2p
