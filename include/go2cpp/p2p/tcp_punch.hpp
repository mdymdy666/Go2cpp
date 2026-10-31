#pragma once

/**
 * @file tcp_punch.hpp
 * @brief TCP 同时打开（TCP simultaneous open）打洞的最小实现。
 * @details 适用场景：双方都在 NAT 后、UDP 被完全封锁，但防火墙允许
 *          「由内向外」发起的 TCP 连接，且 NAT 对 TCP 端口做的是
 *          endpoint-independent mapping。双方各自 bind 一个本地端口，然后
 *          **同时** connect 对方的端口：两个 SYN 交叉后，双方都进入
 *          ESTABLISHED。
 *
 *          与 UDP 打洞相比限制更严格：
 *          - 绝大多数 NAT 对 TCP 做「地址/端口相关映射」，因此**对称 NAT
 *            下必然失败**；
 *          - 需要双方几乎同时发起（本实现靠重试 + 短轮询提高命中率，但没有
 *            精确时钟同步）；
 *          - Linux 内核支持同时打开，Windows/macOS 的支持程度不同。
 *
 *          因此本类被定位为「加分能力」：`P2PChannel` 默认不启用 TCP 打洞，
 *          由使用方显式调用 `TcpPuncher`。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/p2p/endpoint.hpp"

namespace go2cpp::p2p {

/**
 * @brief 一个已绑定本地端口、可执行同时打开的 TCP 端。
 *
 * @details 生命周期：`Bind()` 成功后套接字处于未连接状态；`Connect()` 成功
 *          后 `connected()` 为 true，可以 `Send()`/`Recv()`。所有方法都不是
 *          线程安全的，`Connect()` 会阻塞到成功或超时。
 */
class TcpPuncher {
public:
    /**
     * @brief 绑定本地端点（端口 0 由内核分配）。
     * @param local 本地地址；端口 0 表示自动分配。
     * @param error 失败原因。
     * @return 成功返回对象；失败返回 nullptr。
     */
    static std::unique_ptr<TcpPuncher> Bind(const Endpoint& local, std::string* error = nullptr);

    /// 构造一个未初始化对象（供 `std::make_unique` 使用；请优先用 `Bind()`）。
    TcpPuncher() = default;

    ~TcpPuncher();

    TcpPuncher(const TcpPuncher&) = delete;
    TcpPuncher& operator=(const TcpPuncher&) = delete;

    /// 本地端点（端口 0 已解析为实际端口）。
    Endpoint local_endpoint() const;

    /// 对端端点（`Connect` 成功后由 `getpeername` 得到）。
    Endpoint remote_endpoint() const;

    /**
     * @brief 与 `remote` 同时打开 TCP 连接。
     * @param timeout 总超时。
     * @return 是否建立连接。
     * @note 调用方需要在另一台主机/线程上同时对本地端口发起 `Connect`，
     *       否则本端会收到 RST 并不断重试直至超时。
     */
    bool Connect(const Endpoint& remote, std::chrono::milliseconds timeout,
                 std::string* error = nullptr);

    bool connected() const noexcept;

    /// 发送全部字节；失败返回 false。
    bool Send(const void* data, std::size_t length);

    /// 接收数据；`> 0` 字节数、`0` 超时、`< 0` 错误。
    int Recv(void* buffer, std::size_t capacity, std::chrono::milliseconds timeout);

    /// 关闭连接（幂等）。
    void Close();

private:
    bool RecreateSocket(const Endpoint& local, std::string* error);
    void ResetSocket();

    int m_fd = -1;
    Endpoint m_local;
    Endpoint m_remote;
    bool m_connected = false;
};

}  // namespace go2cpp::p2p
