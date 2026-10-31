#pragma once

/**
 * @file transport.hpp
 * @brief P2P 的数据报传输抽象（UDP 语义）与默认 POSIX 实现。
 * @details 把「收/发一个数据报」抽成接口，目的是让 NAT 模拟、故障注入与
 *          未来其它承载（例如 QUIC datagram、测试内存传输）都能复用
 *          STUN/TURN/ICE 的全部逻辑，而不需要改协议实现。
 *
 *          **线程模型**：`SendTo` 与 `RecvFrom` 允许从不同线程并发调用；
 *          实现必须自行保证这一点（`UdpTransport` 依赖内核 socket 的并发
 *          安全语义，模拟传输用互斥量）。同一个 `RecvFrom` 不应在两个线程
 *          同时调用（语义上是「唯一读端」）。
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

namespace go2cpp::p2p {

/**
 * @brief 数据报传输接口。
 *
 * @details `RecvFrom` 的返回值约定：
 *          - `> 0`：收到的字节数；
 *          - `= 0`：超时（或收到零长度数据报，二者在本项目中不区分）；
 *          - `< 0`：不可恢复错误（socket 关闭等）。
 */
class DatagramTransport {
public:
    virtual ~DatagramTransport() = default;

    /// 发送一个数据报；返回 false 表示内核拒绝（缓冲区满、网络不可达等）。
    virtual bool SendTo(const Endpoint& destination, const void* data, std::size_t length) = 0;

    /**
     * @brief 接收一个数据报。
     * @param source 非空时写入发送方端点。
     * @param buffer 接收缓冲区。
     * @param capacity 缓冲区容量。
     * @param timeout 最长等待时间；0 表示非阻塞轮询，负值表示一直等待。
     * @return 字节数 / 0 超时 / <0 错误。
     */
    virtual int RecvFrom(Endpoint* source, void* buffer, std::size_t capacity,
                         std::chrono::milliseconds timeout) = 0;

    /// 本端绑定的端点（`bind` 后由 `getsockname` 得到）。
    virtual Endpoint LocalEndpoint() const = 0;

    /// 调整接收缓冲区；返回是否成功（模拟传输可以返回 false）。
    virtual bool SetReceiveBufferSize(std::size_t bytes) {
        (void)bytes;
        return false;
    }
};

struct UdpTransportConfig {
    /// 绑定端点；端口 0 表示由内核分配。
    Endpoint bind_endpoint = Endpoint::Wildcard(0);
    /// 收发缓冲区大小（字节）。0 表示保持内核默认值。
    std::size_t receive_buffer_bytes = 4U << 20U;
    std::size_t send_buffer_bytes = 4U << 20U;
    /// 是否允许端口复用（重启/多实例测试时有用）。
    bool reuse_address = true;
};

/**
 * @brief 基于 POSIX UDP socket 的 `DatagramTransport` 实现。
 *
 * @details 接收使用 `poll` + `recvfrom`，因此超时精度不受 `SO_RCVTIMEO`
 *          的毫秒截断影响；发送直接 `sendto` 并重试 `EINTR`。
 *
 *          构造后必须调用 `Start()`；失败时 `Start()` 返回 false 并写入
 *          `error`。析构会关闭 fd。
 */
class UdpTransport final : public DatagramTransport {
public:
    explicit UdpTransport(UdpTransportConfig config = {});
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    /// 创建 socket、设置缓冲区并 bind；成功后可收发。
    bool Start(std::string* error = nullptr);

    bool SendTo(const Endpoint& destination, const void* data, std::size_t length) override;
    int RecvFrom(Endpoint* source, void* buffer, std::size_t capacity,
                 std::chrono::milliseconds timeout) override;
    Endpoint LocalEndpoint() const override;
    bool SetReceiveBufferSize(std::size_t bytes) override;

    int fd() const noexcept { return m_fd; }
    bool started() const noexcept { return m_fd >= 0; }

private:
    UdpTransportConfig m_config;
    int m_fd = -1;
    Endpoint m_local;
    mutable std::mutex m_local_mutex;
};

/**
 * @brief 枚举本机可用的单播地址（用于 ICE host 候选）。
 * @param port 附加到每个地址上的端口。
 * @param include_loopback 是否包含回环地址；局域网直连通常包含，
 *        纯公网部署可以排除。
 * @return 去重后的端点列表（IPv4 在前，IPv6 在后）。
 * @note 只返回 `IFF_UP` 且非 `IFF_LOOPBACK`（除非显式要求）的接口地址；
 *       不返回链路本地 IPv6（需要 scope id，跨主机无意义）。
 */
std::vector<Endpoint> EnumerateLocalAddresses(std::uint16_t port, bool include_loopback);

}  // namespace go2cpp::p2p
