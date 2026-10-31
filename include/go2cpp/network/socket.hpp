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
#include <sys/types.h>
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

/**
 * @brief 批量接收中的一个数据报槽位（recvmmsg 的每条消息）。
 * @details 调用方负责填 `data` / `capacity`，返回后读 `received` /
 *          `from` / `from_length`。每个槽位自带缓冲区，不复用同一块内存——
 *          批量收包不会因为共享缓冲区而互相覆盖。
 * @note 主要面向数据报套接字（UDP / Unix datagram）；流式套接字上一次
 *       recvmmsg 可能只填满前几个槽位，剩余数据仍留在内核。
 */
struct RecvBatchEntry {
    void* data{nullptr};      ///< 接收缓冲区首地址，必须非空。
    std::size_t capacity{0};  ///< 缓冲区容量，必须大于 0。
    std::size_t received{0};  ///< 实际收到的字节数（返回时写）。
    sockaddr_storage from{};  ///< 来源地址（返回时写）。
    socklen_t from_length{0}; ///< 来源地址实际长度（返回时写）。
};

/**
 * @brief 批量发送中的一个数据报（sendmmsg 的每条消息）。
 * @note `to` 为空表示使用已 connect 的对端地址；非空时用 `to`/`to_length`。
 */
struct SendBatchEntry {
    const void* data{nullptr};   ///< 数据首地址，必须非空。
    std::size_t length{0};       ///< 数据长度（UDP 允许 0，表示空数据报）。
    const sockaddr* to{nullptr}; ///< 目标地址；为空表示已连接的对端。
    socklen_t to_length{0};      ///< 目标地址长度。
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
    /// @brief 单次批量收发允许的最大条数（与内核 UIO_MAXIOV 对齐）。
    static constexpr std::size_t kMaxBatch = 1024;
    static Ptr CreateTCP(int family = AF_INET);
    static Ptr CreateUDP(int family = AF_INET);
    static Ptr CreateUnix(bool datagram = false);
    static Ptr FromFd(int fd, int family, int type, int protocol = 0,
                      bool owner = true);
    /**
     * @brief 给一个已建立的 TCP socket 开启 `TCP_NODELAY`（关闭 Nagle）。
     * @param fd 内核文件描述符；调用方负责保证它仍有效。
     * @return 成功返回 true；fd 无效、不是 TCP（UDP / Unix socket 返回
     *         `EOPNOTSUPP`）或 `setsockopt` 失败返回 false。
     * @note 只影响本端发送：小包不再等待前一个未确认小包的 ACK 才发。
     *       本库在 `accept()` 里自动调用（见 `TcpNoDelayDefault()`），
     *       直接用 `::accept` 的协议实现（如 `Socks5Server`）需要自行调用。
     */
    static bool SetTcpNoDelay(int fd) noexcept;
    /**
     * @brief 设置 `accept()` 返回的新 TCP 连接是否默认开启 `TCP_NODELAY`。
     * @details 默认开启。关闭只用于「修复前行为」的对比实验与回归定位；
     *          生产路径不应关闭——不关 Nagle 时小包往返会被延迟 ACK 压成
     *          约 40ms（见 `docs/reports/2026-10-31-http-perf.md` 第 6.1 节）。
     */
    static void SetTcpNoDelayDefault(bool enabled) noexcept;
    /// @brief 查询 `accept()` 是否自动开启 `TCP_NODELAY`（默认 true）。
    static bool TcpNoDelayDefault() noexcept;
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

    /**
     * @brief 一次 recvmmsg(2) 收取多个数据报（UDP 批量接收）。
     * @param entries 槽位数组，每项由调用方提供独立缓冲区。
     * @param count 槽位数量，范围 1..kMaxBatch。
     * @param flags 透传给 recvmmsg 的 flags（如 MSG_TRUNC）。
     * @param timeout 无数据时的最长等待；Duration::max() 表示采用 recv 超时。
     * @return `transferred` 为实际收包数（1..count）；完全失败时为 -1，
     *         `error` 给出 errno（超时 ETIMEDOUT、取消 ECANCELED、关闭 EBADF）。
     * @note 非阻塞语义：只取「当前内核队列里已到达」的包，不承诺凑满 count。
     *       这正是批量场景要的——一次系统调用带走所有已就绪数据报。
     *       返回后第 i 个槽位的 `received` / `from` 才有效（i < transferred）。
     */
    SocketResult recv_batch(RecvBatchEntry* entries, std::size_t count,
                            int flags = 0, Duration timeout = Duration::max());

    /**
     * @brief 一次 sendmmsg(2) 发送多个数据报（UDP 批量发送）。
     * @param entries 待发送的数据报数组，每项自带 data/length/目标地址。
     * @param count 条数，范围 1..kMaxBatch。
     * @param flags 透传给 sendmmsg 的 flags（内部始终附加 MSG_NOSIGNAL）。
     * @param timeout 缓冲区满时的最长等待；Duration::max() 表示采用 send 超时。
     * @return `transferred` 为**成功提交给内核的条数**，可能小于 count；
     *         调用方应把 [transferred, count) 作为下一批继续。只有一条都
     *         没发出时才返回 -1，`error` 给出 errno。
     * @note 部分成功不重试：重试会重复发送已经进入内核的数据报。
     */
    SocketResult send_batch(const SendBatchEntry* entries, std::size_t count,
                            int flags = 0, Duration timeout = Duration::max());

    /**
     * @brief 用 sendfile(2) 零拷贝把文件内容发送到本 socket（TCP 文件传输）。
     * @param in_fd 输入文件描述符（普通文件或 memfd）。
     * @param offset 非空时从 *offset 起发送并推进它；为空时用文件当前偏移并推进。
     * @param count 期望发送的字节数。
     * @param timeout 发送缓冲区满时的最长等待。
     * @return `transferred` 为已发送字节数（可能小于 count：对端关闭、
     *         超时或文件提前 EOF）；完全失败时 -1 并给出 errno。
     * @note 数据不经过用户态缓冲区，由内核页缓存直接 DMA 到网卡。
     *       中途出错时返回已发送字节数（error=0），调用方按
     *       `transferred < count` 判断是否继续，文件偏移已由内核推进。
     */
    SocketResult send_file(int in_fd, off_t* offset, std::size_t count,
                           Duration timeout = Duration::max());

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
    SocketResult recv_batch_impl(int fd, RecvBatchEntry* entries, std::size_t count,
                                 int flags, Duration timeout,
                                 std::uint64_t cancel_generation);
    SocketResult send_batch_impl(int fd, const SendBatchEntry* entries,
                                 std::size_t count, int flags, Duration timeout,
                                 std::uint64_t cancel_generation);
    SocketResult send_file_impl(int fd, int in_fd, off_t* offset, std::size_t count,
                                Duration timeout, std::uint64_t cancel_generation);
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