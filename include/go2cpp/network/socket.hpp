#pragma once

/**
 * @file socket.hpp
 * @brief TCP、UDP、Unix Socket 的 RAII 封装。
 * @details Socket 不拥有调度器；在受管 Fiber 中遇到 EAGAIN 会通过当前
 *          IOManager 让出 Fiber，在普通线程中使用 poll。Socket 对象拥有 fd，
 *          accept 返回新的拥有对象，底层 fd 可通过 release 转移。
 */

#include "go2cpp/network/address.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/io.hpp"
#include "go2cpp/core/parking_condition.hpp"

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>

namespace go2cpp::net {

enum class SocketType { Tcp = SOCK_STREAM, Udp = SOCK_DGRAM, UnixStream = SOCK_STREAM,
                        UnixDatagram = SOCK_DGRAM };

/** @brief Socket 系统错误结果，error 为 errno 值，0 表示成功。 */
struct SocketResult {
    ssize_t transferred{-1};
    int error{0};
    bool ok() const noexcept { return transferred >= 0 && error == 0; }
};

/** @brief 可选的 Socket 等待后端，默认使用当前 IOManager 或 poll。 */
class SocketWaiter {
public:
    virtual ~SocketWaiter() = default;
    virtual int wait(int fd, io::IOEvent event,
                     std::chrono::milliseconds timeout) = 0;
};

/** @brief RAII Socket。支持线程与 Fiber 共同访问，但不允许同一方向多处消费协议。 */
class Socket final : public std::enable_shared_from_this<Socket> {
public:
    using Ptr = std::shared_ptr<Socket>;
    using Duration = std::chrono::milliseconds;
    static Ptr CreateTCP(int family = AF_INET);
    static Ptr CreateUDP(int family = AF_INET);
    static Ptr CreateUnix(bool datagram = false);
    static Ptr FromFd(int fd, int family, int type, int protocol = 0,
                      bool owner = true);
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    /**
     * @brief 返回当前底层文件描述符，供诊断或传给外部系统调用。
     * @note 返回的是**瞬时值**，不进入 `begin_operation` 的 in-flight 保护：
     *       取到之后若发生 `close()` 或 fd 复用，该值即失效（ABA）。库内部的
     *       读写、accept、connect、选项查询与地址查询都会自行固定 fd，
     *       **调用方不应跨操作持有本返回值**。
     */
    int fd() const noexcept;
    int family() const noexcept { return m_family; }
    int type() const noexcept { return m_type; }
    int protocol() const noexcept { return m_protocol; }
    bool valid() const noexcept;
    bool connected() const noexcept;
    int last_error() const noexcept;
    /**
     * @brief 移交底层文件描述符。
     * @details 若 Fiber 无法等待在途操作，则返回 -1 并转为延迟关闭；
     *          最后一个在途操作结束时关闭本对象拥有的 fd。
     * @return 成功移交的 fd；失败返回 -1。
     */
    int release() noexcept;
    void set_waiter(std::shared_ptr<SocketWaiter> waiter) noexcept;

    bool bind(const Address& address);
    bool listen(int backlog = SOMAXCONN);
    Ptr accept(Duration timeout = Duration::max());
    bool connect(const Address& address, Duration timeout = Duration::max());
    bool reconnect(Duration timeout = Duration::max());
    /**
     * @brief 关闭 Socket，并等待在途操作退出。
     * @details 当前上下文无法 park 时返回 false，最终关闭交由最后一个
     *          在途操作完成，避免 fd 泄漏。
     */
    bool close() noexcept;
    /** @brief 半关闭连接，how 取 SHUT_RD/SHUT_WR/SHUT_RDWR。 */
    bool shutdown(int how) noexcept;

    SocketResult send(const void* data, std::size_t length, int flags = 0,
                      Duration timeout = Duration::max());
    SocketResult recv(void* data, std::size_t length, int flags = 0,
                      Duration timeout = Duration::max());
    SocketResult sendv(const iovec* buffers, std::size_t count, int flags = 0,
                       Duration timeout = Duration::max());
    SocketResult recvv(iovec* buffers, std::size_t count, int flags = 0,
                       Duration timeout = Duration::max());
    SocketResult send_to(const void* data, std::size_t length, const Address& to,
                         int flags = 0, Duration timeout = Duration::max());
    SocketResult recv_from(void* data, std::size_t length, AddressPtr& from,
                           int flags = 0, Duration timeout = Duration::max());

    AddressPtr local_address() const;
    AddressPtr remote_address() const;
    bool set_recv_timeout(Duration timeout);
    bool set_send_timeout(Duration timeout);
    Duration recv_timeout() const noexcept;
    Duration send_timeout() const noexcept;
    bool set_option(int level, int option, const void* value, socklen_t length);
    bool get_option(int level, int option, void* value, socklen_t* length) const;
    /** @brief 跨线程取消当前读等待；成功表示取消代际已发布。 */
    bool cancel_read() noexcept;
    /** @brief 跨线程取消当前写等待；成功表示取消代际已发布。 */
    bool cancel_write() noexcept;

private:
    /** @brief 为一次系统调用固定 fd 和代际，阻止 close 期间新操作进入。 */
    bool begin_operation(int& fd) noexcept;
    /** @brief 系统调用结束后减少 in-flight 计数并唤醒关闭者。 */
    void end_operation() noexcept;
    /**
     * @brief 等待关闭流程达到指定状态。
     * @param wait_fd_closed 为 true 时等待 fd 彻底失效，否则只等待在途操作归零。
     * @return 状态达到返回 true；调度器关闭导致 Fiber 无法继续等待时返回 false。
     * @note 使用混合条件变量，避免受管 Fiber 在等待关闭时阻塞 carrier 线程。
     */
    bool wait_close_state(bool wait_fd_closed) noexcept;
    /** @brief 唯一最终关闭者；仅在关闭待处理且在途计数归零时执行。 */
    void finalize_deferred_close() noexcept;
    /** @brief 查询 Socket 是否已经进入关闭/释放阶段。 */
    bool is_closing() const noexcept;
    Socket(int family, int type, int protocol, int fd, bool owner);
    bool init_nonblock();
    int wait_ready(int fd, io::IOEvent event, Duration timeout,
                   std::uint64_t cancel_generation);
    bool wait_or_retry(int fd, io::IOEvent event, Duration& remaining,
                       std::chrono::steady_clock::time_point started,
                       std::uint64_t cancel_generation);
    SocketResult send_impl(int fd, const msghdr* message, int flags,
                           Duration timeout, std::uint64_t cancel_generation);
    SocketResult recv_impl(int fd, msghdr* message, int flags, Duration timeout,
                           std::uint64_t cancel_generation);
    int m_fd{-1};
    int m_family{AF_UNSPEC};
    int m_type{0};
    int m_protocol{0};
    bool m_owner{true};
    bool m_connected{false};
    Duration m_recv_timeout{Duration::max()};
    Duration m_send_timeout{Duration::max()};
    AddressPtr m_remote;
    std::shared_ptr<SocketWaiter> m_waiter;
    mutable std::mutex m_mutex;
    // ParkingCondition 复用 m_mutex 保护的谓词，普通线程阻塞等待、受管
    // Fiber park；close/release 等待在途操作时不会占住 M，也不会丢通知。
    core::ParkingCondition m_operation_condition;
    std::size_t m_inflight{0};
    // 在途操作期间自持有对象，防止最后一个外部 shared_ptr 释放后提前析构。
    std::shared_ptr<Socket> m_inflight_keepalive;
    bool m_closing{false};
    // 当前执行上下文无法等待时，由最后一个操作接管 fd 的关闭责任。
    bool m_close_pending{false};
    std::uint64_t m_generation{1};
    std::atomic<std::uint64_t> m_read_cancel_generation{0};
    std::atomic<std::uint64_t> m_write_cancel_generation{0};
};

/** @brief 读写字节流接口，自动处理 partial I/O。 */
class SocketStream final {
public:
    using Ptr = std::shared_ptr<SocketStream>;
    explicit SocketStream(Socket::Ptr socket, bool owner = true);
    ~SocketStream();
    SocketStream(const SocketStream&) = delete;
    SocketStream& operator=(const SocketStream&) = delete;
    ssize_t read(void* data, std::size_t length);
    ssize_t write(const void* data, std::size_t length);
    ssize_t readv(iovec* buffers, std::size_t count);
    ssize_t writev(const iovec* buffers, std::size_t count);
    bool write_all(const void* data, std::size_t length);
    bool close() noexcept;
    bool connected() const noexcept;
    Socket::Ptr socket() const noexcept { return m_socket; }

private:
    Socket::Ptr m_socket;
    bool m_owner{true};
};

}  // namespace go2cpp::net

/** @brief Go 风格的地址解析和 Dial/Listen 便捷入口。 */
namespace go2cpp::net {
AddressPtr ResolveTCPAddr(const std::string& host, std::uint16_t port,
                          int family = AF_UNSPEC);
AddressPtr ResolveUDPAddr(const std::string& host, std::uint16_t port,
                          int family = AF_UNSPEC);
Socket::Ptr DialTCP(const std::string& host, std::uint16_t port,
                    Socket::Duration timeout = Socket::Duration::max());
Socket::Ptr DialUDP(const std::string& host, std::uint16_t port,
                    Socket::Duration timeout = Socket::Duration::max());
Socket::Ptr ListenTCP(const std::string& host, std::uint16_t port,
                      int backlog = SOMAXCONN);
Socket::Ptr ListenUDP(const std::string& host, std::uint16_t port);
}  // namespace go2cpp::net

namespace go2cpp {
using net::Socket;
using net::SocketResult;
using net::SocketStream;
using net::SocketType;
using net::DialTCP;
using net::DialUDP;
using net::ListenTCP;
using net::ListenUDP;
using net::ResolveTCPAddr;
using net::ResolveUDPAddr;
}  // namespace go2cpp