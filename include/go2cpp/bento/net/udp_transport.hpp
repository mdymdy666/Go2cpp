#pragma once

/**
 * @file udp_transport.hpp
 * @brief `ITransport` 的 UDP 实现：面向无连接的数据报收发，以及连接后的 Read/Write。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details UDP 与 TCP 的语义差异很大，因此本类在 `ITransport` 的四个动作之外
 *          额外提供 `Bind` / `SendTo` / `RecvFrom`：
 *
 *           - **无连接模式**：`Bind({host, 0})` 绑定本地端口，然后循环
 *             `SendTo(peer, ...)` / `RecvFrom(..., &peer, ...)`。每个数据报
 *             独立保留边界，`RecvFrom` 一次只返回一个数据报。
 *           - **连接模式**：`Connect(peer)` 只在内核里设置默认对端（UDP 没有
 *             三次握手），随后可以直接用 `Read` / `Write`。`Connect` 返回
 *             成功只代表本地 socket 已接受该对端，**不代表对端在线**。
 *
 *           `SendTo` 与 `RecvFrom` 在连接模式下仍然可用；`SendTo` 可以发给
 *           另一个地址（Linux 允许 connected UDP socket 使用 sendto）。
 *
 * ## 数据报边界与截断
 *
 * `RecvFrom` 使用 `recvmsg(MSG_TRUNC)`：即使接收缓冲小于数据报，也会返回
 * 数据报的**原始长度**，并通过 `RecvInfo::truncated` 告知是否被截断。
 * 超出缓冲的部分被内核丢弃，不能通过后续读取找回——UDP 没有流式补读。
 * 需要完整数据报时，调用方必须提供足够大的缓冲（IPv4 上限 65507 字节，
 * IPv6 上限 65527 字节），或让上层协议在应用层分片。
 *
 * `ITransport::Read` 没有截断输出参数，因此连接模式下 `Read` 会静默截断；
 * 需要检测截断时请使用 `RecvFrom`。
 *
 * ## 超时语义
 *
 * 与 `TcpTransport` 一致：每个 `timeout` 是整次调用的 deadline，`EINTR`
 * 后重新计算剩余时间，`EAGAIN`/`EWOULDBLOCK` 转为 poll 等待，超时返回
 * `kTimeout`。`timeout == 0` 表示非阻塞尝试一次（仍会 poll 一次，消费内核
 * 中已经就绪的数据报）；负值按 0 处理，超过 24 小时按 24 小时处理——上限是
 * 必要的：`steady_clock::time_point + milliseconds::max()` 会触发有符号整数
 * 溢出，deadline 变成过去时间，调用会立即超时（UBSan 实测）。`timeout > 0`
 * 时先尝试一次 `recv`/`recvmsg`，只有内核无数据才 `poll`，省掉高负载下每包
 * 一次系统调用；`timeout == 0` 仍保持 poll 先行，空闲轮询成本不变。DNS 解析
 * 不受 deadline 约束。
 *
 * ## 错误映射
 *
 * `ECONNRESET`/`EPIPE`/`ENOTCONN`/`ECONNABORTED`/`ESHUTDOWN` 归为
 * `kClosed`；`ETIMEDOUT` 归为 `kTimeout`；其余（含 `ECONNREFUSED`、
 * `EMSGSIZE`、`EINVAL`）归为 `kError`，原始 errno 保存在
 * `Result::system_error`。connected UDP socket 收到 ICMP port unreachable
 * 后，下一次 `Read`/`Write` 可能返回 `ECONNREFUSED`，调用方应把它当作
 * 对端不可达，而不是传输层故障。
 *
 * ## 半关闭与关闭
 *
 * UDP 没有 FIN，`ShutdownWrite()` 只设置本端「不再发送」标志：后续
 * `Write`/`SendTo` 返回 `kClosed` + `EPIPE`，`Read`/`RecvFrom` 仍然可用，
 * 但**对端不会收到任何通知**。这是数据报语义与 TCP 半关闭的关键差异。
 * `Close()` 幂等，关闭后所有操作返回 `kClosed`。
 *
 * ## 线程模型
 *
 * 一个实例的 `Read`/`RecvFrom` 与 `Write`/`SendTo` 可以分别在不同线程调用
 * （它们只操作 socket 的读/写方向）；`Bind`/`Connect`/`Close` 与其它操作
 * 并发需要调用方协调。本类不加锁。
 *
 * ## 使用示例
 *
 * @code
 *   UdpTransport server;
 *   server.Bind({"127.0.0.1", 0});
 *   Endpoint local;
 *   server.LocalEndpoint(&local);
 *
 *   UdpTransport client;
 *   client.Connect(local, std::chrono::milliseconds(1000));
 *   client.Write(payload.data(), payload.size(), std::chrono::milliseconds(1000));
 *
 *   Endpoint peer;
 *   UdpTransport::RecvInfo info;
 *   server.RecvFrom(buffer.data(), buffer.size(), std::chrono::milliseconds(1000),
 *                   &peer, &info);
 * @endcode
 */

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "go2cpp/bento/net/transport.hpp"

namespace go2cpp::bento::net {

/**
 * @brief UDP 数据报传输（非阻塞，deadline 超时）。
 *
 * @details 一个实例表示一个 UDP socket。默认构造后必须调用 `Bind` 或
 *          `Connect` 才能收发；`Connect` 在未绑定的 socket 上会由内核隐式
 *          绑定一个本地临时端口。
 */
class UdpTransport final : public ITransport {
public:
    /**
     * @brief `RecvFrom` 的数据报元信息。
     *
     * @details 失败时内容不保证被修改；调用方应只在返回 `kOk` 时读取。
     */
    struct RecvInfo {
        std::size_t bytes = 0;           ///< 实际复制到调用方缓冲的字节数。
        std::size_t datagram_bytes = 0;  ///< 数据报原始长度（可能大于 `bytes`）。
        bool truncated = false;          ///< 数据报是否大于接收缓冲。
    };

    UdpTransport() noexcept = default;
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;
    UdpTransport(UdpTransport&&) = delete;
    UdpTransport& operator=(UdpTransport&&) = delete;

    /**
     * @brief 绑定本地 UDP 地址。
     * @param local 本地地址；`host` 为空表示通配（`AI_PASSIVE`），端口 0
     *        表示由内核分配。IPv4/IPv6 字面量或主机名均可。
     * @param reuse_address 是否设置 `SO_REUSEADDR`；测试/多实例场景可打开。
     * @return `kOk` 成功；已打开时 `kError` + `EALREADY`；解析/绑定/创建
     *         socket 失败时 `kError` + errno。
     *
     * @note 绑定成功后可以通过 `LocalEndpoint` 取得内核分配的端口。
     */
    Result Bind(const Endpoint& local, bool reuse_address = false);

    /**
     * @brief 设置默认对端（UDP connect），之后可用 `Read`/`Write`。
     * @param endpoint 目标地址；`host` 非空且端口非 0。
     * @param timeout 连接总超时（UDP connect 通常立即完成，此参数只用于
     *        极端情况下等待异步错误）。
     * @return `kOk` 成功；已连接时 `kError` + `EALREADY`；参数非法时
     *         `kError` + `EINVAL`；解析/connect 失败时 `kError` + errno。
     *
     * @note 成功只表示本地 socket 接受了该对端，不表示对端可达。对端不存在
     *       时，错误通常在下一次 `Read`/`Write` 以 `ECONNREFUSED` 出现。
     * @note 已 `Bind` 到某一地址族后再 `Connect` 另一族地址会返回
     *       `kError` + `EAFNOSUPPORT`。
     */
    Result Connect(const Endpoint& endpoint, std::chrono::milliseconds timeout) override;

    /**
     * @brief 从已连接的对端读取一个数据报。
     * @param buffer 可写缓冲。
     * @param capacity 缓冲大小；0 是合法空操作（返回 `kOk` + 0）。
     * @param timeout 本次调用的总超时。
     * @return `kOk` 成功（`bytes` 为复制的字节数，可能因缓冲不足而截断）；
     *         `kTimeout` 超时；`kClosed` 本端已关闭；`kError` 未连接或系统错误。
     *
     * @note 零长度 UDP 数据报会返回 `kOk` 且 `bytes == 0`；这是 `Read`
     *       返回 0 字节的第二种情形（第一种是 `capacity == 0` 的合法空
     *       操作，已在 `ITransport::Read` 契约内）。
     * @note 未调用 `Connect` 时返回 `kError` + `ENOTCONN`；需要接收任意
     *       对端的数据报请使用 `RecvFrom`。
     */
    Result Read(std::uint8_t* buffer, std::size_t capacity,
                std::chrono::milliseconds timeout) override;

    /**
     * @brief 向已连接的对端发送一个数据报。
     * @param data 数据；`size > 0` 时不能为空。
     * @param size 数据报长度；0 是合法空操作（返回 `kOk` + 0，不发送空包）。
     * @param timeout 本次调用的总超时。
     * @return 成功时 `kOk` 且 `bytes == size`（UDP 发送是原子的，不会部分写）；
     *         未连接返回 `kError` + `EDESTADDRREQ`；数据报过大返回 `kError` +
     *         `EMSGSIZE`；本端已关闭/已半关闭返回 `kClosed`。
     */
    Result Write(const std::uint8_t* data, std::size_t size,
                 std::chrono::milliseconds timeout) override;

    /**
     * @brief 向任意对端发送一个数据报（无需先 Connect）。
     * @param peer 目标地址；`host` 非空且端口非 0。
     * @param data 数据；`size > 0` 时不能为空。
     * @param size 数据报长度；允许 0（发送零长度数据报）。
     * @param timeout 本次调用的总超时；DNS 解析不占 deadline。
     * @return 成功时 `kOk` 且 `bytes == size`；未打开返回 `kClosed`；
     *         参数非法返回 `kError` + `EINVAL`；地址族与本地 socket 不匹配
     *         返回 `kError` + `EAFNOSUPPORT`；数据报过大返回 `EMSGSIZE`。
     */
    Result SendTo(const Endpoint& peer, const std::uint8_t* data, std::size_t size,
                  std::chrono::milliseconds timeout);

    /**
     * @brief 接收一个数据报，并返回发送方地址。
     * @param buffer 可写缓冲；`capacity == 0` 时可为空，用于探测/丢弃数据报。
     * @param capacity 缓冲大小。
     * @param timeout 本次调用的总超时。
     * @param peer 输出发送方地址；可为 nullptr（不关心来源时）。
     * @param info 输出数据报长度/截断信息；可为 nullptr。
     * @return `kOk` 成功；`kTimeout` 超时；`kClosed` 未打开/已关闭；
     *         `kError` 参数非法或系统错误。
     *
     * @note 即使 `info->truncated == true`，返回码仍是 `kOk`；调用方必须
     *       检查 `info` 再决定是否丢弃该数据报。
     * @note 在 connected socket 上内核只交付默认对端的数据报，`peer` 仍会
     *       被填充（若内核未提供来源地址，则回退到 `getpeername`）。
     */
    Result RecvFrom(std::uint8_t* buffer, std::size_t capacity,
                    std::chrono::milliseconds timeout, Endpoint* peer,
                    RecvInfo* info = nullptr);

    /**
     * @brief 关闭本端写方向（仅本地标志）。
     * @return 未打开时 `kClosed`；否则 `kOk`（幂等）。
     *
     * @details UDP 没有 FIN，对端不会收到通知；`Read` 仍可用。此动作只是
     *          防止本端继续 `Write`/`SendTo`，用于统一 `ITransport` 的生命
     *          周期。
     */
    Result ShutdownWrite() override;

    /// @brief 关闭 socket（幂等）。
    Result Close() override;

    /**
     * @brief 获取本端地址（`getsockname`，数字形式）。
     * @param endpoint 输出参数，非空。
     * @return `kOk` 成功；未打开 `kClosed`；参数为空或系统调用失败 `kError`。
     */
    Result LocalEndpoint(Endpoint* endpoint) const;

    /**
     * @brief 获取已连接的对端地址（`getpeername`，数字形式）。
     * @param endpoint 输出参数，非空。
     * @return `kOk` 成功；未打开 `kClosed`；未连接 `kError` + `ENOTCONN`。
     */
    Result PeerEndpoint(Endpoint* endpoint) const;

    /// socket 是否已打开（已 Bind 或已 Connect）。
    bool is_open() const noexcept {
        return m_fd >= 0;
    }

    /// 是否已设置默认对端。
    bool is_connected() const noexcept {
        return m_connected;
    }

private:
    int m_fd = -1;                  ///< socket fd；-1 表示关闭。
    int m_family = -1;              ///< 已绑定/已连接地址族；-1 表示未知。
    bool m_connected = false;       ///< 是否已 Connect。
    bool m_write_shutdown = false;  ///< 是否已本地半关闭。
};

}  // namespace go2cpp::bento::net
