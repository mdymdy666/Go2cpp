#pragma once

/**
 * @file tcp_transport.hpp
 * @brief `ITransport` 的 TCP 实现（JinShan MOD-08）：非阻塞 + poll 超时、accept、IPv4/IPv6。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 一个 `TcpTransport` 实例在任一时刻只表示**一个 socket**：
 *           - 客户端：默认构造后 `Connect()`；
 *           - 服务端：`Listen()` 后循环 `Accept()`，每次产出一个已连接的
 *             `TcpTransport`；
 *           - `Accept()` 产出的对象与监听对象互不影响，监听对象可以继续 accept。
 *
 * ## 实现要点
 *
 * - **全非阻塞**：socket 创建后立即 `O_NONBLOCK | FD_CLOEXEC`。`connect` 走
 *   `EINPROGRESS` + `poll(POLLOUT)` + `getsockopt(SO_ERROR)`，超时用
 *   `steady_clock` deadline 控制；`accept`/`read`/`write` 都先 poll 再执行。
 * - **EINTR 重试**：所有系统调用（`poll`/`connect`/`accept`/`recv`/`send`）
 *   遇到 `EINTR` 都重新计算剩余时间后继续，不把信号中断当成错误。
 * - **EAGAIN 不是错误**：非阻塞 socket 的 `EAGAIN`/`EWOULDBLOCK` 表示「现在
 *   没有数据/缓冲满」，继续 poll 直到 deadline。
 * - **错误映射**：`ECONNRESET`/`EPIPE`/`ENOTCONN`/`ECONNABORTED`/`ESHUTDOWN`
 *   映射为 `kClosed`；`ETIMEDOUT` 映射为 `kTimeout`；其余保留 errno 归为
 *   `kError`。
 * - **SIGPIPE 规避**：`send` 使用 `MSG_NOSIGNAL`，对端关闭时返回 `EPIPE`
 *   而不是杀死进程。这是服务端必须处理的常见路径。
 * - **IPv4/IPv6**：`getaddrinfo(AF_UNSPEC)` 解析，按返回顺序逐个尝试；
 *   `Listen` 对 IPv6 关闭 `IPV6_V6ONLY`，通配监听可同时接受 IPv4 映射地址。
 * - **DNS 不占 deadline**：`getaddrinfo` 没有超时参数，解析阶段不受
 *   `timeout` 约束；连接阶段严格受控。需要硬上界时传 IP 字面量。
 *
 * ## 与项目已有 `TcpProxy` 的关系
 *
 * `TcpProxy` 是「监听 → 拨号 → 双向转发」的一体化工具；本类是**单连接**的
 * 可复用原语，`TcpProxy` 的事件循环逻辑更复杂，不复用其内部实现，但超时、
 * 半关闭和错误映射的语义保持一致（见 `src/bento/net/tcp_proxy.cpp`）。
 *
 * ## 线程模型
 *
 * 一个实例的 `Read` 与 `Write` 可以分别在不同线程调用（它们只操作 socket 的
 * 两个方向）；`Accept` 只在监听实例上调用；`Close` 与其它操作并发需要调用方
 * 协调。本类不加锁。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "go2cpp/bento/net/transport.hpp"

namespace go2cpp::bento::net {

/**
 * @brief TCP 字节流传输（非阻塞，deadline 超时）。
 *
 * @details 使用方式：
 * @code
 *   TcpTransport listener;
 *   listener.Listen({"127.0.0.1", 0});
 *   Endpoint local;
 *   listener.LocalEndpoint(&local);
 *
 *   TcpTransport client;
 *   client.Connect(local, std::chrono::milliseconds(1000));
 *
 *   TcpTransport server;
 *   listener.Accept(&server, std::chrono::milliseconds(1000));
 * @endcode
 *
 * 监听端口传 0 时由内核分配，测试与多实例场景必须这样用；实际端口通过
 * `LocalEndpoint()` 获取。
 */
class TcpTransport final : public ITransport {
public:
    TcpTransport() noexcept = default;
    ~TcpTransport() override;

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;
    TcpTransport(TcpTransport&&) = delete;
    TcpTransport& operator=(TcpTransport&&) = delete;

    /// @copydoc ITransport::Connect
    Result Connect(const Endpoint& endpoint, std::chrono::milliseconds timeout) override;

    /// @copydoc ITransport::Read
    Result Read(std::uint8_t* buffer, std::size_t capacity,
                std::chrono::milliseconds timeout) override;

    /// @copydoc ITransport::Write
    Result Write(const std::uint8_t* data, std::size_t size,
                 std::chrono::milliseconds timeout) override;

    /// @copydoc ITransport::ShutdownWrite
    Result ShutdownWrite() override;

    /// @copydoc ITransport::Close
    Result Close() override;

    /**
     * @brief 创建监听 socket（IPv4/IPv6，非阻塞）。
     * @param bind_endpoint 绑定地址；`host` 为空表示通配（`AI_PASSIVE`），
     *        端口 0 表示内核分配。
     * @param backlog `listen(2)` 的队列长度；小于等于 0 时按 64 处理。
     * @return `kOk` 成功；`kError` 解析/绑定/监听失败（`system_error` 为 errno
     *         或 `EINVAL`/`EHOSTUNREACH`）；已打开时返回 `kError` + `EALREADY`。
     *
     * @note 监听 socket 也设置为非阻塞，`Accept` 依靠 poll 等待，不阻塞线程。
     */
    Result Listen(const Endpoint& bind_endpoint, int backlog = 64);

    /**
     * @brief 从监听 socket 接受一条连接。
     * @param accepted 输出参数；必须处于未打开状态，否则返回 `kError` +
     *        `EALREADY`。成功后它持有一条独立、非阻塞的连接。
     * @param timeout accept 的总超时；0 表示只轮询一次。
     * @return `kOk` 成功；`kTimeout` 无连接到达；`kClosed` 监听 socket 已关闭；
     *         `kError` 参数非法或 accept 失败。
     */
    Result Accept(TcpTransport* accepted, std::chrono::milliseconds timeout);

    /**
     * @brief 获取本端实际地址（`getsockname`，数字形式）。
     * @param endpoint 输出参数（非空）。
     * @return `kOk` 成功；`kClosed` 未打开；`kError` 参数非法或系统调用失败。
     *
     * @details IPv6 地址以 `::1` 这类不带方括号的字面量返回，端口单独存放，
     *          便于直接传给 `Endpoint`。
     */
    Result LocalEndpoint(Endpoint* endpoint) const;

    /// socket 是否已打开（监听或已连接）。
    bool is_open() const noexcept {
        return m_fd >= 0;
    }

    /// 是否处于监听状态。
    bool is_listening() const noexcept {
        return m_listening;
    }

private:
    /// 接管一个已经设置好非阻塞标志的 fd。
    Result Adopt(int fd) noexcept;

    int m_fd = -1;                  ///< socket fd；-1 表示关闭。
    bool m_listening = false;       ///< 是否由 Listen 创建。
    bool m_write_shutdown = false;  ///< 是否已发送 FIN。
};

}  // namespace go2cpp::bento::net
