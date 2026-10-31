#pragma once

/**
 * @file turn.hpp
 * @brief 简化 TURN：中继服务器（Allocation + Send/Data）与客户端。
 * @details 用途：当双方处于对称 NAT / 严格防火墙下，UDP 打洞失败时，由
 *          双方各自连接同一个中继服务器，通过服务器分配的 relay 端点转发
 *          数据。语义对齐 TURN 的三段式：
 *
 *          1. **Allocation**：客户端向服务器申请一个 relay 端点（服务器上的
 *             一个 UDP socket 与端口）；
 *          2. **Send**：客户端把「目标地址 + 载荷」发给服务器，服务器从该
 *             relay 端点转发；
 *          3. **Data**：目标回包到达 relay 端点，服务器包装成 Data 消息回给
 *             客户端。
 *
 *          线格式与实现细节见 `src/p2p/relay_protocol.hpp`。服务器是单线程
 *          poll 循环，可同时服务多个 allocation；客户端不是线程安全的
 *          （`Allocate/Refresh/Release/SendTo/RecvFrom` 必须串行调用），
 *          因为所有请求共用同一个 `DatagramTransport`。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/transport.hpp"

namespace go2cpp::p2p {

struct RelayServerConfig {
    /// 控制面监听地址；relay 端点绑定在同一 IP 上、端口由内核分配。
    Endpoint bind_endpoint = Endpoint::Loopback(0);
    /**
     * @brief 对外通告的地址（只取 IP，端口用 relay 端点的实际端口）。
     * @details 当 `bind_endpoint` 是 `0.0.0.0` 时必须设置，否则客户端拿到的
     *          relay 地址不可达。为空时使用监听地址的 IP（要求监听地址具体）。
     */
    Endpoint advertised_address;
    /// allocation 生命周期；客户端需要在此之前 Refresh。
    std::chrono::milliseconds allocation_lifetime{60000};
    /// 最大并发 allocation 数；超过后返回 `kAllocationLimit`。
    std::size_t max_allocations = 256;
    /// 接收缓冲区大小。
    std::size_t receive_buffer_bytes = 2U << 20U;
};

struct RelayServerStats {
    std::uint64_t allocations_created = 0;
    std::uint64_t allocations_expired = 0;
    std::uint64_t requests_received = 0;
    std::uint64_t responses_sent = 0;
    std::uint64_t packets_client_to_peer = 0;
    std::uint64_t packets_peer_to_client = 0;
    std::uint64_t bytes_client_to_peer = 0;
    std::uint64_t bytes_peer_to_client = 0;
    std::uint64_t dropped_packets = 0;
};

/**
 * @brief 简化 TURN 中继服务器。
 *
 * @details 不校验来源权限：任何知道某个 relay 地址的人都能向它发包。这是
 *          有意的简化——部署时应把中继服务器放在可信内网，或用防火墙只允许
 *          已知网段访问。见文档「限制」一节。
 */
class RelayServer {
public:
    explicit RelayServer(RelayServerConfig config = {});
    ~RelayServer();

    RelayServer(const RelayServer&) = delete;
    RelayServer& operator=(const RelayServer&) = delete;

    /// 绑定监听 socket 并启动 poll 线程。
    bool Start(std::string* error = nullptr);

    /// 停止线程、关闭全部 allocation 与监听 socket；可重复调用。
    void Stop();

    bool started() const noexcept;

    /// 控制面监听端点（实际端口）。
    Endpoint listen_endpoint() const;

    /// 当前 allocation 数量。
    std::size_t allocation_count() const;

    /// 累计统计（线程安全）。
    RelayServerStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

struct RelayClientConfig {
    /// 中继服务器控制面地址。
    Endpoint server;
    /// 单次请求（Allocate/Refresh/Release）的等待超时。
    std::chrono::milliseconds request_timeout{500};
    /// Allocate 重试次数（第一次不算重试）。
    int allocate_attempts = 5;
    /// 请求的 allocation 生命周期。
    std::uint32_t requested_lifetime_ms = 60000;
};

/**
 * @brief 简化 TURN 客户端。
 *
 * @details 所有方法共用构造时传入的 `DatagramTransport`，因此**不支持并发
 *          调用**。析构时会尽力发送 Release（失败忽略）。
 */
class RelayClient {
public:
    RelayClient(DatagramTransport* transport, RelayClientConfig config);
    ~RelayClient();

    RelayClient(const RelayClient&) = delete;
    RelayClient& operator=(const RelayClient&) = delete;

    /// 申请 relay 端点（失败会按 `allocate_attempts` 重试）。
    bool Allocate(std::string* error = nullptr);

    /// 续租；未分配时返回 false。
    bool Refresh(std::string* error = nullptr);

    /// 释放 allocation（best effort，不等待响应）。
    void Release();

    bool allocated() const noexcept;
    Endpoint relayed_endpoint() const;
    std::uint32_t lifetime_ms() const noexcept;

    /**
     * @brief 通过中继向 peer 发送数据。
     * @return 是否成功交给传输层（不代表对端收到）。
     */
    bool SendTo(const Endpoint& peer, const void* data, std::size_t length);

    /**
     * @brief 接收一个经中继转发的数据报。
     * @return `> 0` 字节数、`0` 超时、`< 0` 错误。
     */
    int RecvFrom(Endpoint* peer, void* buffer, std::size_t capacity,
                 std::chrono::milliseconds timeout);

    std::uint64_t bytes_sent() const noexcept;
    std::uint64_t bytes_received() const noexcept;
    std::uint64_t packets_sent() const noexcept;
    std::uint64_t packets_received() const noexcept;

private:
    std::uint32_t NextRequestId();
    bool Request(const std::uint8_t type, const std::vector<std::uint8_t>& body,
                 std::chrono::milliseconds timeout, std::uint8_t* status,
                 std::vector<std::uint8_t>* response_body, std::string* error);

    DatagramTransport* m_transport = nullptr;
    RelayClientConfig m_config;
    Endpoint m_relayed;
    std::uint32_t m_lifetime_ms = 0;
    bool m_allocated = false;
    std::uint32_t m_next_request_id = 0;
    mutable std::mutex m_mutex;
    std::uint64_t m_bytes_sent = 0;
    std::uint64_t m_bytes_received = 0;
    std::uint64_t m_packets_sent = 0;
    std::uint64_t m_packets_received = 0;
    std::vector<std::uint8_t> m_recv_buffer;
    /// 请求-响应缓冲（成员而非栈数组：避免在 Fiber 的 4KB 栈上放 64KB）。
    std::vector<std::uint8_t> m_request_buffer;
};

}  // namespace go2cpp::p2p
