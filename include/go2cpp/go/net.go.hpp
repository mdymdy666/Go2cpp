/**
 * @file net.go.hpp
 * @brief Go `net` 包的 C++ 对应物（Conn / Listener / Dial / Listen / Addr）。
 *
 * ## 定位
 * `go2cpp::net::Socket` 已经提供 TCP/UDP/Unix 的 RAII 封装（Fiber 感知的
 * 非阻塞 + IOManager 等待）。本文件按 Go 的接口形态封装：
 *
 * ```go
 * ln, err := net.Listen("tcp", "127.0.0.1:0")
 * conn, err := ln.Accept()
 * conn.SetReadDeadline(time.Now().Add(30 * time.Second))
 * n, err := conn.Read(buf)
 * _, err = conn.Write(buf[:n])
 * conn.Close()
 * ```
 *
 * 对应 C++：
 *
 * ```cpp
 * auto ln = go::net::Listen("tcp", "127.0.0.1:0");
 * auto conn = ln.listener->Accept();
 * conn.first->SetReadDeadline(go::Now().Add(30 * go::Second));
 * auto r = conn.first->Read(buf, sizeof(buf));
 * (void)conn.first->Write(buf, r.n);
 * conn.first->Close();
 * ```
 *
 * ## 与 Go 的差异
 * - Go 的 `net.Conn` 是隐式接口；C++ 用抽象基类 `Conn` + `shared_ptr`。
 * - `Read`/`Write` 返回 `IOResult{n, err}`，对应 `(n int, err error)`；
 *   EOF 用 `go::ErrEOF()`（`errors.Is` 可匹配）。
 * - `Listener` 的地址方法命名 `Addr()` 与类型 `Addr` 同名，C++ 需要返回类型别名
 *   `AddressType`，见类内声明。
 * - UDP 的 `Listen` 返回 `UDPListener`，`Accept` 阻塞到首个数据报并把它缓存进
 *   返回的 `UDPConn`；这是 JinShan relay 不需要的路径，仅提供最小可用语义。
 * - `SetDeadline` 使用 `go::Time` 的单调读数；零值 Time 表示清除 deadline。
 * - 错误文本不会包含 key/token/invitation（JinShan 的脱敏要求）。
 */

#pragma once

#include "go2cpp/go/error.go.hpp"
#include "go2cpp/go/time.go.hpp"
#include "go2cpp/network/address.hpp"
#include "go2cpp/network/socket.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::go::net {

using ::go2cpp::net::AddressPtr;
using ::go2cpp::net::Socket;

/** @brief 对应 Go 的 `net.Addr`。 */
class Addr final {
public:
    Addr() = default;
    Addr(std::string network, AddressPtr address)
        : m_network(std::move(network)), m_address(std::move(address)) {}

    /** @brief 对应 `Addr.Network()`，如 "tcp" / "udp"。 */
    const std::string& Network() const noexcept { return m_network; }

    /** @brief 对应 `Addr.String()`，如 "127.0.0.1:8080"。 */
    std::string String() const {
        return m_address ? m_address->to_string() : std::string{};
    }

    /** @brief 底层地址句柄；未连接时为空。 */
    AddressPtr Raw() const noexcept { return m_address; }
    bool Valid() const noexcept { return static_cast<bool>(m_address); }

private:
    std::string m_network;
    AddressPtr m_address;
};

/** @brief 对应 `(n int, err error)`；`err == nullptr` 表示成功。 */
struct IOResult {
    std::size_t n{0};
    error err;

    bool Ok() const noexcept { return !err; }
    explicit operator bool() const noexcept { return Ok(); }
};

/**
 * @brief 对应 Go 的 `net.Conn` 接口。
 * @details 读、写、关闭、地址与 deadline；实现分别为 `TCPConn` / `UDPConn`。
 */
class Conn {
public:
    virtual ~Conn() = default;

    /** @brief 对应 `conn.Read(p)`；EOF 时 `err == ErrEOF()`。 */
    virtual IOResult Read(void* buffer, std::size_t size) = 0;
    /** @brief 对应 `conn.Write(p)`；允许部分写。 */
    virtual IOResult Write(const void* buffer, std::size_t size) = 0;
    /** @brief 对应 `conn.Close()`；幂等，返回 nil 表示成功。 */
    virtual error Close() = 0;

    virtual Addr LocalAddr() const = 0;
    virtual Addr RemoteAddr() const = 0;

    /** @brief 对应 `SetDeadline(t)`；零值 Time 清除。 */
    virtual error SetDeadline(const Time& deadline) = 0;
    /** @brief 对应 `SetReadDeadline(t)`。 */
    virtual error SetReadDeadline(const Time& deadline) = 0;
    /** @brief 对应 `SetWriteDeadline(t)`。 */
    virtual error SetWriteDeadline(const Time& deadline) = 0;
};

using ConnPtr = std::shared_ptr<Conn>;

/**
 * @brief `Listener::Addr()` 的返回类型别名。
 * @details C++ 不允许类内成员函数与类型同名，因此地址类型 `Addr` 在
 *          `Listener` 接口里通过这个命名空间级别名引用，方法名仍保持 Go 的
 *          `Addr()`。
 */
using AddressType = Addr;

/**
 * @brief 对应 Go 的 `net.Listener` 接口。
 * @details `Addr()` 返回 `AddressType`（即 `Addr`）。
 */
class Listener {
public:
    virtual ~Listener() = default;

    /** @brief 对应 `ln.Accept()`；返回 `(Conn, error)`。 */
    virtual std::pair<ConnPtr, error> Accept() = 0;
    /** @brief 对应 `ln.Close()`。 */
    virtual error Close() = 0;
    /** @brief 对应 `ln.Addr()`。 */
    virtual AddressType Addr() const = 0;
};

using ListenerPtr = std::shared_ptr<Listener>;

namespace detail {

/** @brief `steady_clock` 的毫秒超时，与 `Socket::Duration` 一致。 */
using SocketDuration = ::go2cpp::net::Socket::Duration;

/** @brief 无超时的表示（`Socket::Duration::max()`）。 */
inline SocketDuration NoTimeout() noexcept {
    return SocketDuration::max();
}

/** @brief 把 go::Duration 转成 Socket 超时；<=0 表示无超时。 */
inline SocketDuration ToSocketDuration(Duration timeout) noexcept {
    if (timeout.Nanoseconds() <= 0) {
        return NoTimeout();
    }
    const auto milliseconds = timeout.Milliseconds();
    return SocketDuration(milliseconds <= 0 ? 1 : milliseconds);
}

/**
 * @brief 线程安全的 deadline 槽位。
 * @details Go 的 deadline 可以被其他 goroutine 并发修改（SetDeadline 是并发安全的），
 *          因此这里用 mutex 保护。
 */
class Deadline final {
public:
    void Set(const Time& deadline) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (deadline.IsZero()) {
            m_deadline.reset();
        } else {
            m_deadline = deadline;
        }
    }

    /** @brief 剩余毫秒；无 deadline 返回 max，已过期返回 0。 */
    SocketDuration Remaining() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_deadline.has_value()) {
            return NoTimeout();
        }
        const Duration left = m_deadline->Sub(Time::Now());
        if (left.Nanoseconds() <= 0) {
            return SocketDuration(0);
        }
        const auto milliseconds = left.Milliseconds();
        return SocketDuration(milliseconds <= 0 ? 1 : milliseconds);
    }

    bool Expired() const {
        return Remaining().count() == 0;
    }

    bool HasDeadline() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_deadline.has_value();
    }

private:
    mutable std::mutex m_mutex;
    std::optional<Time> m_deadline;
};

/** @brief errno → error；超时类错误统一映射到 `ErrDeadlineExceeded`。 */
inline error MapSocketError(int code, const char* operation) {
    if (code == 0) {
        return {};
    }
    if (code == ETIMEDOUT || code == EAGAIN || code == EWOULDBLOCK ||
        code == EINPROGRESS || code == EALREADY) {
        return ErrDeadlineExceeded();
    }
    if (code == EBADF || code == ENOTCONN || code == EPIPE) {
        return ErrClosed();
    }
    if (code == ECONNREFUSED) {
        return New(std::string(operation) + ": connection refused");
    }
    if (code == ECONNRESET) {
        return New(std::string(operation) + ": connection reset by peer");
    }
    return New(std::string(operation) + ": " + std::strerror(code));
}

/** @brief 解析 `host:port` / `[::1]:port` / `:port`。 */
struct HostPort {
    std::string host;
    std::uint16_t port{0};
};

inline bool SplitHostPort(const std::string& address, HostPort& out, error& err) {
    if (address.empty()) {
        err = New("missing address");
        return false;
    }
    std::string host;
    std::string port_text;
    if (address.front() == '[') {
        const auto close = address.find(']');
        if (close == std::string::npos || close + 1 >= address.size() ||
            address[close + 1] != ':') {
            err = New("invalid address: missing port in address");
            return false;
        }
        host = address.substr(1, close - 1);
        port_text = address.substr(close + 2);
    } else {
        const auto colon = address.rfind(':');
        if (colon == std::string::npos) {
            err = New("missing port in address");
            return false;
        }
        host = address.substr(0, colon);
        port_text = address.substr(colon + 1);
    }
    if (port_text.empty()) {
        err = New("missing port in address");
        return false;
    }
    int port = 0;
    try {
        port = std::stoi(port_text);
    } catch (...) {
        err = New("invalid port in address: " + port_text);
        return false;
    }
    if (port < 0 || port > 65535) {
        err = New("invalid port in address: " + port_text);
        return false;
    }
    out.host = std::move(host);
    out.port = static_cast<std::uint16_t>(port);
    return true;
}

/** @brief 监听地址的 host 为空时绑定所有网卡。 */
inline std::string ListenHost(const std::string& host) {
    return host.empty() ? std::string("0.0.0.0") : host;
}

inline bool IsTCPNetwork(const std::string& network) {
    return network == "tcp" || network == "tcp4" || network == "tcp6";
}

inline bool IsUDPNetwork(const std::string& network) {
    return network == "udp" || network == "udp4" || network == "udp6";
}

}  // namespace detail

/** @brief 对应 Go 的 `*net.TCPConn`。 */
class TCPConn final : public Conn {
public:
    explicit TCPConn(Socket::Ptr socket, std::string network = "tcp")
        : m_socket(std::move(socket)), m_network(std::move(network)) {}

    IOResult Read(void* buffer, std::size_t size) override {
        if (!m_socket || !m_socket->valid()) {
            return {0, ErrClosed()};
        }
        const auto timeout = m_read_deadline.Remaining();
        if (timeout.count() == 0) {
            return {0, ErrDeadlineExceeded()};
        }
        const auto result = m_socket->recv(buffer, size, 0, timeout);
        if (result.transferred > 0) {
            return {static_cast<std::size_t>(result.transferred), {}};
        }
        if (result.transferred == 0 && result.error == 0) {
            return {0, ErrEOF()};
        }
        return {0, detail::MapSocketError(result.error, "read")};
    }

    IOResult Write(const void* buffer, std::size_t size) override {
        if (!m_socket || !m_socket->valid()) {
            return {0, ErrClosed()};
        }
        const auto timeout = m_write_deadline.Remaining();
        if (timeout.count() == 0) {
            return {0, ErrDeadlineExceeded()};
        }
        const auto result = m_socket->send(buffer, size, 0, timeout);
        if (result.transferred >= 0) {
            return {static_cast<std::size_t>(result.transferred), {}};
        }
        return {0, detail::MapSocketError(result.error, "write")};
    }

    error Close() override {
        if (!m_socket) {
            return {};
        }
        (void)m_socket->close();
        return {};
    }

    Addr LocalAddr() const override {
        return Addr(m_network, m_socket ? m_socket->local_address() : AddressPtr{});
    }
    Addr RemoteAddr() const override {
        return Addr(m_network, m_socket ? m_socket->remote_address() : AddressPtr{});
    }

    error SetDeadline(const Time& deadline) override {
        m_read_deadline.Set(deadline);
        m_write_deadline.Set(deadline);
        return {};
    }
    error SetReadDeadline(const Time& deadline) override {
        m_read_deadline.Set(deadline);
        return {};
    }
    error SetWriteDeadline(const Time& deadline) override {
        m_write_deadline.Set(deadline);
        return {};
    }

    /** @brief 半关闭写方向（对应 Go 的 `CloseWrite`，JinShan DATA 管道使用）。 */
    bool CloseWrite() noexcept {
        return m_socket ? m_socket->shutdown(SHUT_WR) : false;
    }
    /** @brief 半关闭读方向（对应 Go 的 `CloseRead`）。 */
    bool CloseRead() noexcept {
        return m_socket ? m_socket->shutdown(SHUT_RD) : false;
    }

    /** @brief 底层 Socket，便于与既有 API 互操作。 */
    const Socket::Ptr& Raw() const noexcept { return m_socket; }

private:
    Socket::Ptr m_socket;
    std::string m_network;
    detail::Deadline m_read_deadline;
    detail::Deadline m_write_deadline;
};

/** @brief 对应 Go 的 `*net.TCPListener`。 */
class TCPListener final : public Listener {
public:
    explicit TCPListener(Socket::Ptr socket, std::string network = "tcp")
        : m_socket(std::move(socket)), m_network(std::move(network)) {}

    std::pair<ConnPtr, error> Accept() override {
        if (!m_socket || !m_socket->valid()) {
            return {nullptr, ErrClosed()};
        }
        auto accepted = m_socket->accept(detail::NoTimeout());
        if (!accepted) {
            return {nullptr, detail::MapSocketError(errno, "accept")};
        }
        return {std::make_shared<TCPConn>(std::move(accepted), m_network), {}};
    }

    error Close() override {
        if (!m_socket) {
            return {};
        }
        (void)m_socket->close();
        return {};
    }

    AddressType Addr() const override {
        return AddressType(m_network, m_socket ? m_socket->local_address() : AddressPtr{});
    }

    const Socket::Ptr& Raw() const noexcept { return m_socket; }

private:
    Socket::Ptr m_socket;
    std::string m_network;
};

/** @brief 对应 Go 的 `*net.UDPConn`（已连接或按 peer 收发的数据报）。 */
class UDPConn final : public Conn {
public:
    UDPConn(Socket::Ptr socket, std::string network, AddressPtr peer, bool connected,
            bool owns_socket, std::vector<std::uint8_t> pending = {})
        : m_socket(std::move(socket)),
          m_network(std::move(network)),
          m_peer(std::move(peer)),
          m_connected(connected),
          m_owns_socket(owns_socket),
          m_pending(std::move(pending)) {}

    IOResult Read(void* buffer, std::size_t size) override {
        if (m_closed) {
            return {0, ErrClosed()};
        }
        if (!m_pending.empty()) {
            const std::size_t available = m_pending.size() - m_pending_offset;
            const std::size_t copied = available < size ? available : size;
            std::memcpy(buffer, m_pending.data() + m_pending_offset, copied);
            m_pending_offset += copied;
            if (m_pending_offset >= m_pending.size()) {
                m_pending.clear();
                m_pending_offset = 0;
            }
            return {copied, {}};
        }
        if (!m_socket || !m_socket->valid()) {
            return {0, ErrClosed()};
        }
        const auto timeout = m_read_deadline.Remaining();
        if (timeout.count() == 0) {
            return {0, ErrDeadlineExceeded()};
        }
        if (m_connected) {
            const auto result = m_socket->recv(buffer, size, 0, timeout);
            if (result.transferred >= 0) {
                return {static_cast<std::size_t>(result.transferred), {}};
            }
            return {0, detail::MapSocketError(result.error, "read")};
        }
        // 未连接：循环直到收到来自 peer 的数据报。
        for (;;) {
            AddressPtr from;
            const auto result = m_socket->recv_from(buffer, size, from, 0, timeout);
            if (result.transferred < 0) {
                return {0, detail::MapSocketError(result.error, "read")};
            }
            if (m_peer && from && *from == *m_peer) {
                return {static_cast<std::size_t>(result.transferred), {}};
            }
        }
    }

    IOResult Write(const void* buffer, std::size_t size) override {
        if (m_closed) {
            return {0, ErrClosed()};
        }
        if (!m_socket || !m_socket->valid()) {
            return {0, ErrClosed()};
        }
        const auto timeout = m_write_deadline.Remaining();
        if (timeout.count() == 0) {
            return {0, ErrDeadlineExceeded()};
        }
        if (m_connected) {
            const auto result = m_socket->send(buffer, size, 0, timeout);
            if (result.transferred >= 0) {
                return {static_cast<std::size_t>(result.transferred), {}};
            }
            return {0, detail::MapSocketError(result.error, "write")};
        }
        if (!m_peer) {
            return {0, New("write udp: destination address required")};
        }
        const auto result = m_socket->send_to(buffer, size, *m_peer, 0, timeout);
        if (result.transferred >= 0) {
            return {static_cast<std::size_t>(result.transferred), {}};
        }
        return {0, detail::MapSocketError(result.error, "write")};
    }

    error Close() override {
        if (m_closed) {
            return {};
        }
        m_closed = true;
        if (m_owns_socket && m_socket) {
            (void)m_socket->close();
        }
        return {};
    }

    Addr LocalAddr() const override {
        return Addr(m_network, m_socket ? m_socket->local_address() : AddressPtr{});
    }
    Addr RemoteAddr() const override { return Addr(m_network, m_peer); }

    error SetDeadline(const Time& deadline) override {
        m_read_deadline.Set(deadline);
        m_write_deadline.Set(deadline);
        return {};
    }
    error SetReadDeadline(const Time& deadline) override {
        m_read_deadline.Set(deadline);
        return {};
    }
    error SetWriteDeadline(const Time& deadline) override {
        m_write_deadline.Set(deadline);
        return {};
    }

private:
    Socket::Ptr m_socket;
    std::string m_network;
    AddressPtr m_peer;
    bool m_connected{false};
    bool m_owns_socket{false};
    bool m_closed{false};
    std::vector<std::uint8_t> m_pending;
    std::size_t m_pending_offset{0};
    detail::Deadline m_read_deadline;
    detail::Deadline m_write_deadline;
};

/** @brief 对应 Go 的 `*net.UDPConn` 监听形态（`Accept` 等到首个数据报）。 */
class UDPListener final : public Listener {
public:
    explicit UDPListener(Socket::Ptr socket, std::string network = "udp")
        : m_socket(std::move(socket)), m_network(std::move(network)) {}

    std::pair<ConnPtr, error> Accept() override {
        if (!m_socket || !m_socket->valid()) {
            return {nullptr, ErrClosed()};
        }
        std::vector<std::uint8_t> first(65536);
        AddressPtr from;
        const auto result = m_socket->recv_from(first.data(), first.size(), from, 0,
                                                detail::NoTimeout());
        if (result.transferred < 0) {
            return {nullptr, detail::MapSocketError(result.error, "accept udp")};
        }
        first.resize(static_cast<std::size_t>(result.transferred));
        return {std::make_shared<UDPConn>(m_socket, m_network, from, false, false,
                                          std::move(first)),
                {}};
    }

    error Close() override {
        if (!m_socket) {
            return {};
        }
        (void)m_socket->close();
        return {};
    }

    AddressType Addr() const override {
        return AddressType(m_network, m_socket ? m_socket->local_address() : AddressPtr{});
    }

private:
    Socket::Ptr m_socket;
    std::string m_network;
};

// ============================================================================
// Dial / Listen
// ============================================================================

/** @brief `Dial` 的结果：`(Conn, error)`。 */
struct DialResult {
    ConnPtr conn;
    error err;

    bool Ok() const noexcept { return conn != nullptr; }
    explicit operator bool() const noexcept { return Ok(); }
};

/** @brief `Listen` 的结果：`(Listener, error)`。 */
struct ListenResult {
    ListenerPtr listener;
    error err;

    bool Ok() const noexcept { return listener != nullptr; }
    explicit operator bool() const noexcept { return Ok(); }
};

/**
 * @brief 对应 `net.Dial(network, address)` / `net.DialTimeout`。
 * @param network "tcp"/"tcp4"/"tcp6"/"udp"/"udp4"/"udp6"。
 * @param address "host:port"（IPv6 用 "[::1]:port"）。
 * @param timeout 相对超时；`Duration{}`（0）表示无超时。
 */
inline DialResult Dial(const std::string& network, const std::string& address,
                       Duration timeout = Duration{}) {
    detail::HostPort host_port;
    error parse_error;
    if (!detail::SplitHostPort(address, host_port, parse_error)) {
        return {nullptr, parse_error};
    }
    const auto socket_timeout = detail::ToSocketDuration(timeout);

    if (detail::IsTCPNetwork(network)) {
        auto resolved = ::go2cpp::net::ResolveTCPAddr(host_port.host, host_port.port);
        if (!resolved) {
            return {nullptr, New("dial tcp: lookup " + host_port.host + ": no such host")};
        }
        auto socket = Socket::CreateTCP(resolved->family());
        if (!socket) {
            return {nullptr, New("dial tcp: create socket failed")};
        }
        if (!socket->connect(*resolved, socket_timeout)) {
            return {nullptr, detail::MapSocketError(socket->last_error(), "dial tcp")};
        }
        return {std::make_shared<TCPConn>(std::move(socket), network), {}};
    }

    if (detail::IsUDPNetwork(network)) {
        auto resolved = ::go2cpp::net::ResolveUDPAddr(host_port.host, host_port.port);
        if (!resolved) {
            return {nullptr, New("dial udp: lookup " + host_port.host + ": no such host")};
        }
        auto socket = Socket::CreateUDP(resolved->family());
        if (!socket) {
            return {nullptr, New("dial udp: create socket failed")};
        }
        if (!socket->connect(*resolved, socket_timeout)) {
            return {nullptr, detail::MapSocketError(socket->last_error(), "dial udp")};
        }
        return {std::make_shared<UDPConn>(std::move(socket), network, resolved, true, true),
                {}};
    }

    return {nullptr, New("dial " + network + ": unknown network")};
}

/** @brief 对应 `net.Listen(network, address)`（tcp 为监听，udp 为首包 Accept）。 */
inline ListenResult Listen(const std::string& network, const std::string& address) {
    detail::HostPort host_port;
    error parse_error;
    if (!detail::SplitHostPort(address, host_port, parse_error)) {
        return {nullptr, parse_error};
    }
    const std::string host = detail::ListenHost(host_port.host);

    if (detail::IsTCPNetwork(network)) {
        auto resolved = ::go2cpp::net::ResolveTCPAddr(host, host_port.port);
        if (!resolved) {
            return {nullptr, New("listen tcp: lookup " + host + ": no such host")};
        }
        auto socket = Socket::CreateTCP(resolved->family());
        if (!socket) {
            return {nullptr, New("listen tcp: create socket failed")};
        }
        int reuse = 1;
        (void)socket->set_option(SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (!socket->bind(*resolved)) {
            return {nullptr, detail::MapSocketError(socket->last_error(), "bind")};
        }
        if (!socket->listen(SOMAXCONN)) {
            return {nullptr, detail::MapSocketError(socket->last_error(), "listen")};
        }
        return {std::make_shared<TCPListener>(std::move(socket), network), {}};
    }

    if (detail::IsUDPNetwork(network)) {
        auto resolved = ::go2cpp::net::ResolveUDPAddr(host, host_port.port);
        if (!resolved) {
            return {nullptr, New("listen udp: lookup " + host + ": no such host")};
        }
        auto socket = Socket::CreateUDP(resolved->family());
        if (!socket) {
            return {nullptr, New("listen udp: create socket failed")};
        }
        int reuse = 1;
        (void)socket->set_option(SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (!socket->bind(*resolved)) {
            return {nullptr, detail::MapSocketError(socket->last_error(), "bind udp")};
        }
        return {std::make_shared<UDPListener>(std::move(socket), network), {}};
    }

    return {nullptr, New("listen " + network + ": unknown network")};
}

/** @brief Go 风格的 `DialTimeout` 别名（Dial 已支持 timeout 参数）。 */
inline DialResult DialTimeout(const std::string& network, const std::string& address,
                              Duration timeout) {
    return Dial(network, address, timeout);
}

}  // namespace go2cpp::go::net
