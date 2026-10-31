#pragma once

/**
 * @file p2p_channel.hpp
 * @brief P2P 通信层：ICE 候选收集 + UDP 打洞 + 中继降级 + 数据面。
 * @details 这是本模块对外的唯一入口。典型用法：
 *
 *          ```cpp
 *          P2PConfig config;
 *          config.stun_servers = {*Endpoint::Parse("stun.example.com:3478")};
 *          config.turn_server = *Endpoint::Parse("turn.example.com:3478");
 *          config.signaling = my_im_signaling;   // 与对端交换候选的实现
 *          P2PChannel channel(config);
 *
 *          const auto local = channel.GatherCandidates();
 *          // （signaling 已配置时，Connect 会自动交换候选；否则手动 AddRemoteCandidate）
 *          if (channel.Connect(std::chrono::seconds(5))) {
 *              channel.Send("hello", 5);
 *              char buffer[1200];
 *              const std::size_t n = channel.Recv(buffer, sizeof(buffer), 1s);
 *          }
 *          ```
 *
 *          **连接流程**（`Connect`）：
 *          1. 若配置了 `signaling` 且没有手动添加远端候选，双方发送/接收候选串；
 *          2. 候选按优先级排序（host > srflx > relay），对所有 host/srflx 候选对
 *             并行发送 STUN Binding 检查（每 `check_interval` 一轮）；
 *          3. 收到对端的 Binding Request 或匹配事务 ID 的 Success Response
 *             即判定该候选对连通，锁定 peer 地址；
 *          4. 直连阶段耗尽时间后，若配置了 TURN 且双方都有 relay 候选，
 *             通过中继重新做一次 Binding 检查；成功则 `IsRelayed() == true`；
 *          5. 全部失败返回 false，`LastError()` 给出原因。
 *
 *          **数据面**：每个数据报带 12 字节帧头（magic/version/type/length/seq）。
 *          接收时识别并回应 keepalive，跳过控制帧；STUN 检查包也会被就地处理。
 *          单包上限 `kP2PMaxPayload`（1200 字节，避免 IP 分片）；更大的消息
 *          需要上层自行分片（例如复用 Bento 的可靠层）。
 *
 *          **弱网**：没有重传/拥塞控制/FEC，只提供 keepalive（保活 NAT 映射 +
 *          RTT 测量）、序列号缺口统计与对端静默超时。可靠传输应由上层负责。
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
#include <vector>

#include "go2cpp/p2p/candidate.hpp"
#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/signaling.hpp"
#include "go2cpp/p2p/transport.hpp"

namespace go2cpp::p2p {

/// 单个 P2P 数据报的最大业务载荷（避免 1500 MTU 下的 IP 分片）。
inline constexpr std::size_t kP2PMaxPayload = 1200;

/// 连接状态。
enum class P2PState : std::uint8_t {
    kIdle = 0,       ///< 尚未收集候选。
    kGathering = 1,  ///< 正在收集候选（同步调用期间）。
    kConnecting = 2, ///< 正在打洞/检查连通性。
    kConnected = 3,  ///< 直连成功。
    kRelayed = 4,    ///< 通过中继连接。
    kFailed = 5,     ///< 连接失败。
    kClosed = 6,     ///< 已关闭。
};

/// 状态名（日志/测试用）。
const char* P2PStateName(P2PState state) noexcept;

/// 累计统计（线程安全快照）。
struct P2PStats {
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t keepalives_sent = 0;
    std::uint64_t keepalives_received = 0;
    std::uint64_t connectivity_checks_sent = 0;
    std::uint64_t connectivity_checks_received = 0;
    std::uint64_t dropped_packets = 0;
    std::uint64_t relayed_packets_sent = 0;
    std::uint64_t relayed_packets_received = 0;
    std::uint32_t sequence_gaps = 0;  ///< 数据序列号缺口累计（近似丢包）。
    double rtt_ms = 0.0;              ///< 最近一次 keepalive 往返。
    double rtt_avg_ms = 0.0;          ///< keepalive RTT 滑动平均。
};

struct P2PConfig {
    /// 本地绑定端点；端口 0 由内核分配。绑定具体 IP 时只收集该地址的 host 候选。
    Endpoint bind_endpoint = Endpoint::Wildcard(0);
    /// STUN 服务器列表；为空时只收集 host 候选（局域网直连场景）。
    std::vector<Endpoint> stun_servers;
    /// 简化 TURN 服务器；无效表示不启用中继降级。
    Endpoint turn_server;
    /// 信令通道（非拥有）；为空时必须在调用 `Connect()` 前手动 `AddRemoteCandidate()`。
    Signaling* signaling = nullptr;
    /**
     * @brief 外部注入的数据报传输（测试/模拟 NAT 用）。
     * @details 为空时内部创建 `UdpTransport` 并绑定 `bind_endpoint`。
     *          注入时 `bind_endpoint` 被忽略，本地端点是 `transport->LocalEndpoint()`。
     */
    std::shared_ptr<DatagramTransport> transport;
    /// 单个 STUN 请求的超时（收集候选阶段）。
    std::chrono::milliseconds gather_timeout{800};
    /// Connect 的默认总超时（`Connect(0ms)` 时使用）。
    std::chrono::milliseconds connect_timeout{5000};
    /// 连通性检查的重发间隔。
    std::chrono::milliseconds check_interval{100};
    /// keepalive 间隔；0 表示关闭。
    std::chrono::milliseconds keepalive_interval{15000};
    /// 连接后多久收不到任何有效帧判定对端失联。
    std::chrono::milliseconds peer_timeout{30000};
    /// 是否收集 host 候选。
    bool gather_host_candidates = true;
    /// 是否把回环地址也算作 host 候选（本机测试/同机多进程需要）。
    bool gather_loopback_candidates = true;
    /// 是否启用 TURN 中继降级。
    bool enable_relay = true;
    /// 是否在 `Connect()` 里自动通过 signaling 交换候选。
    bool auto_exchange_candidates = true;
    /// 收发缓冲区大小（内部 UdpTransport 使用）。
    std::size_t receive_buffer_bytes = 4U << 20U;
    /// 最多保留的本地候选数。
    std::size_t max_local_candidates = 16;
};

/**
 * @brief P2P 通信通道。
 *
 * @details 线程模型：
 *          - `GatherCandidates()`、`Connect()`、`Send()`、`Recv()` 不应在
 *            多个线程同时调用（内部共享同一个 socket/候选状态）；
 *          - `Send()` 与 `Recv()` 可以在不同线程并发（一个发、一个收），
 *            统计与状态由互斥量保护；
 *          - `Stats()`、`IsRelayed()`、`LastError()` 可从任意线程调用。
 */
class P2PChannel {
public:
    explicit P2PChannel(P2PConfig config);
    ~P2PChannel();

    P2PChannel(const P2PChannel&) = delete;
    P2PChannel& operator=(const P2PChannel&) = delete;

    /**
     * @brief 收集本地候选（host + srflx + relay）。
     * @details 会阻塞在 STUN 请求与 TURN Allocate 上，最坏约
     *          `stun_servers.size() × gather_timeout + 中继超时`。
     *          重复调用会重新收集并清空已有候选。
     */
    std::vector<Candidate> GatherCandidates();

    /// 添加一个远端候选；重复添加同一地址会被忽略。
    void AddRemoteCandidate(const Candidate& candidate);

    /**
     * @brief 建立 P2P 连接。
     * @param timeout 总超时；0 表示使用 `P2PConfig::connect_timeout`。
     * @return 是否连接成功（直连或中继）。
     */
    bool Connect(std::chrono::milliseconds timeout);

    /// 发送一个数据报；未连接、长度为 0 或超过 `kP2PMaxPayload` 时返回 false。
    bool Send(const void* data, std::size_t length);

    /**
     * @brief 接收一个数据报。
     * @return 实际拷贝字节数；0 表示超时/未连接；大于容量时截断并返回容量。
     * @note 控制帧（keepalive/STUN）在内部处理，不会返回给调用方。
     */
    std::size_t Recv(void* buffer, std::size_t capacity, std::chrono::milliseconds timeout);

    /// 当前连接是否走中继。
    bool IsRelayed() const;

    /// 是否处于已连接/已中继状态。
    bool IsConnected() const;

    /// 当前状态。
    P2PState state() const;

    /// 本地传输端点。
    Endpoint LocalEndpoint() const;

    /// 对端地址（直连为对端候选，中继为对端 relay 地址）。
    Endpoint PeerEndpoint() const;

    /// 本地候选快照。
    std::vector<Candidate> LocalCandidates() const;

    /// 远端候选快照。
    std::vector<Candidate> RemoteCandidates() const;

    /// 统计快照。
    P2PStats Stats() const;

    /// 最近一次失败原因。
    std::string LastError() const;

    /// 关闭通道（释放中继 allocation）；可重复调用。
    void Close();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::p2p
