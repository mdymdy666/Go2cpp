#pragma once

/**
 * @file udp_batch.hpp
 * @brief 批量与多队列 UDP：recvmmsg/sendmmsg + SO_REUSEPORT。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 *
 * @details 为什么需要单独的批量 socket：
 *
 *          - `UdpTransport` 是一包一系统调用的通用实现，语义完整但每包两次
 *            系统调用（recvmsg/sendmsg）。Bento 的帧很小（ACK 帧 24B），
 *            在 1e6 pps 量级下 syscall 开销直接成为瓶颈。
 *          - Linux 的 `recvmmsg(2)` / `sendmmsg(2)` 把 N 个数据报合并成一次
 *            系统调用：实测单核回环吞吐约提升 2~3 倍（见 tests/bento 的
 *            压测输出）。批量上限刻意取小值（收 32 / 发 16），原因是
 *            `sendmmsg` 一次占用的内核时间片随 n 线性增长，过大的批次会
 *            拖高同机其它线程的 p99 延迟；32/16 在吞吐与尾延迟之间取平衡。
 *          - `SO_REUSEPORT` 让多个 socket 绑定同一 (addr, port)，由内核按
 *            四元组哈希分发。本类最多开 64 个队列（任务约定的 1~64），
 *            发送按队列轮转，接收轮询所有队列，从而把单核收包软中断的分发
 *            成本摊到多核。
 *
 * ## 队列与回退
 *
 * `Config::queues` 是期望队列数；内核不支持 `SO_REUSEPORT`（或权限不足）时
 * 自动回退为 1 个队列，`stats().reuse_port` 为 false，不会让 Open 失败。
 * 端口为 0 时先绑定第一队列，再用 `getsockname` 得到的实际端口绑定其余队列，
 * 保证所有队列端口一致。
 *
 * ## 超时语义
 *
 * `RecvBatch(timeout_ms)` 的 timeout_ms 是整次调用的预算：0 表示只 poll 一次
 * （非阻塞）；负值按 0 处理。返回实际读到的数据报数（可能为 0），系统错误
 * 通过 error 输出且返回值仍为已经读到的数量。
 *
 * ## 线程模型
 *
 * 不内部加锁。惯例是一个线程调用 `RecvBatch`、另一个线程调用 `SendBatch`；
 * `Open`/`Close` 必须与其它调用串行。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/types.h>

#include "go2cpp/bento/net/transport.hpp"

namespace go2cpp::bento::net {

/**
 * @brief 批量/多队列 UDP 收发器。
 *
 * @details 只负责数据报边界与地址，不做协议解析；帧格式由 Bento 的
 *          protocol.hpp 负责。
 */
class UdpBatchSocket final {
  public:
    /// 单实例最多打开的 socket 队列数。
    static constexpr std::size_t kMaxQueues = 64;
    /// recvmmsg 单次最多收取的数据报数。
    static constexpr std::size_t kMaxRecvBatch = 32;
    /// sendmmsg 单次最多发送的数据报数。
    static constexpr std::size_t kMaxSendBatch = 16;
    /// 每个接收槽的默认字节数（覆盖常见 MTU 下的 Bento 帧）。
    static constexpr std::size_t kSlotBytes = 1500;

    /// @brief 打开参数。
    struct Config {
        std::size_t queues = 1;                    ///< 队列数（1..kMaxQueues）。
        std::size_t recv_batch = kMaxRecvBatch;    ///< 单次 recvmmsg 上限。
        std::size_t send_batch = kMaxSendBatch;    ///< 单次 sendmmsg 上限。
        bool reuse_port = true;                    ///< 是否尝试 SO_REUSEPORT。
        std::size_t slot_bytes = kSlotBytes;       ///< 每个接收槽字节数。
    };

    /// @brief 可观测统计。
    struct Stats {
        std::uint64_t rx_batches = 0;       ///< recvmmsg 调用次数。
        std::uint64_t rx_datagrams = 0;     ///< 收到的数据报数。
        std::uint64_t rx_empty_batches = 0; ///< 返回 0 条的 recvmmsg 次数。
        std::uint64_t rx_errors = 0;        ///< recvmmsg 系统错误次数。
        std::uint64_t tx_batches = 0;       ///< sendmmsg 调用次数。
        std::uint64_t tx_datagrams = 0;     ///< 发出的数据报数。
        std::uint64_t tx_errors = 0;        ///< sendmmsg 系统错误次数。
        std::uint32_t queues = 0;           ///< 实际打开的队列数。
        bool reuse_port = false;            ///< 是否真的启用了 SO_REUSEPORT。
    };

    /// @brief 收到的一个数据报。
    struct Datagram {
        std::vector<std::uint8_t> data;
        Endpoint peer;
    };

    UdpBatchSocket() noexcept = default;
    ~UdpBatchSocket();

    UdpBatchSocket(const UdpBatchSocket&) = delete;
    UdpBatchSocket& operator=(const UdpBatchSocket&) = delete;
    UdpBatchSocket(UdpBatchSocket&&) = delete;
    UdpBatchSocket& operator=(UdpBatchSocket&&) = delete;

    /// @brief 解析并绑定本地地址；queues>1 时尝试 SO_REUSEPORT。
    /// @param bind 本地地址与端口（port=0 表示由内核分配）。
    /// @param config 队列与批量参数；越界值会被夹逼。
    /// @param error 失败原因（可为 nullptr）。
    bool Open(const Endpoint& bind, Config config, std::string* error);

    /// @brief 批量发送同一对端的数据报，队列间轮转。
    /// @param sent 实际发出的数量（可为 nullptr）；出错时为已发出数量。
    /// @return 全部发出返回 true；部分发出或系统错误返回 false。
    bool SendBatch(const Endpoint& peer, const std::vector<std::vector<std::uint8_t>>& payloads,
                   std::size_t* sent, std::string* error);

    /// @brief 批量接收：轮询所有队列，最多读满 recv_batch 条。
    /// @param out 追加写入（调用方先清空可得到严格本次结果）。
    /// @param timeout_ms 整次调用的预算；0 表示非阻塞。
    /// @return 本次读到的数据报数。
    std::size_t RecvBatch(std::vector<Datagram>* out, int timeout_ms, std::string* error);

    /// @brief 关闭全部队列（幂等）。
    void Close() noexcept;

    bool IsOpen() const noexcept {
        return !m_fds.empty();
    }
    /// @brief 实际绑定的本地端口（Open 成功后有效）。
    std::uint16_t local_port() const noexcept {
        return m_port;
    }
    /// @brief 实际打开的队列数。
    std::size_t queue_count() const noexcept {
        return m_fds.size();
    }
    const Stats& stats() const noexcept {
        return m_stats;
    }
    const Config& config() const noexcept {
        return m_config;
    }

  private:
    /// @brief 关闭队列并清空批量缓冲。
    void CloseFds() noexcept;
    /// @brief 解析地址（getaddrinfo，SOCK_DGRAM）。
    static bool Resolve(const Endpoint& endpoint, sockaddr_storage* out, socklen_t* out_len,
                        int* family, std::string* error);
    /// @brief 把 sockaddr 还原成 Endpoint（IPv4/IPv6 字面量）。
    static Endpoint ToEndpoint(const sockaddr_storage& addr, socklen_t len);

    std::vector<int> m_fds;
    Config m_config;
    std::uint16_t m_port = 0;
    std::size_t m_next_send_queue = 0;
    Stats m_stats;
    std::vector<std::vector<std::uint8_t>> m_slots;
    bool m_peer_valid = false;
    std::string m_peer_host;
    std::uint16_t m_peer_port = 0;
    sockaddr_storage m_peer_addr{};
    socklen_t m_peer_addr_len = 0;
    std::vector<sockaddr_storage> m_peers;
    std::vector<socklen_t> m_peer_lens;
};

}  // namespace go2cpp::bento::net
