#pragma once

/**
 * @file peer_hub.hpp
 * @brief 一个 UDP socket 承载 N 个对端的多 peer 链路（PeerHub）。
 * @details 背景：`Link` 是**单 peer** 链路（一个 socket 对应对端地址，
 *          监听方只接受第一个握手成功的来源）。JinShan 的「1 被控 : N 主控」
 *          场景（FR-06 / B2-2）要求被控端用**一个监听端口**同时服务多个
 *          主控，`Link` 无法表达。本组件补上这个缺口。
 *
 *          设计取向与取舍（为什么不是直接改 Link）：
 *          - Link 的会话状态（conn_id、加密密钥、迁移地址、PMTUD、通道表、
 *            统计）全部是「单 peer」的成员字段，改成多 peer 要把 2000+ 行
 *            实现整体 per-peer 化，回归面覆盖全部既有测试；因此这里新增
 *            **独立组件**，复用 Link 已验证的下层构件（UdpSocket、
 *            ReliableChannel、UnreliableChannel、Relation、帧编解码），
 *            不动 Link。
 *          - 路由键用协议帧头里的 **conn_id(u32)**，而不是源 IP:port：
 *            与 Bento v0.2 的既有语义一致，且天然支持对端换 NAT 端口后
 *            仍然路由到同一个 peer（与 Link 的迁移识别同源）。
 *          - **peer_id 是本地标识**（类似 fd），不上海缆：监听端按来源地址
 *            自动分配 1..N，主动端由应用指定。这样同一份线格式既能被
 *            监听端用于「1 被控 N 主控」，也不要求双方 id 对齐。
 *
 *          线格式：完全复用 Bento v0.2（16B 帧头 + 可选 ACK + 载荷），
 *          HELLO/HELLO_ACK 载荷为 4B magic "BENT" + 4B session + 4B conn_id。
 *
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/bento/net/relation.hpp"

namespace go2cpp::bento {

/// @brief 单个 peer 的状态。
enum class PeerState : std::uint8_t {
    kIdle = 0,       ///< 已注册，尚未发起握手（保留给显式 Connect）。
    kConnecting = 1, ///< 已发出 HELLO，等待 HELLO_ACK。
    kConnected = 2,  ///< 握手完成，可收发数据帧。
    kTimedOut = 3,   ///< 握手或心跳超时（可 Connect 重连）。
    kClosed = 4,     ///< 已显式关闭/移除。
};

/// @brief 状态名（日志/测试用）。
const char* PeerStateName(PeerState state) noexcept;

/// @brief PeerHub 配置。
struct PeerHubConfig {
    /// 绑定地址：IPv4/IPv6 字面量或主机名；"0.0.0.0"/"::" 表示通配。
    std::string bind_address = "0.0.0.0";
    std::uint16_t bind_port = 0;  ///< 0 = 内核分配（测试必需）。
    /// 是否自动接受来自未知地址的 HELLO（监听端模式）。
    /// false 时只接受预先 AddPeer 过的地址（更像「白名单」）。
    bool accept_unknown_peers = true;
    /// peer 数量上限。超限的新 HELLO 被丢弃并计入 peers_rejected。
    std::size_t max_peers = 16;
    std::uint32_t handshake_timeout_ms = 5000;
    std::uint32_t peer_timeout_ms = 5000;  ///< 连接后多久收不到帧判超时。
    std::uint32_t heartbeat_interval_ms = 500;
    std::uint32_t ack_interval_ms = 20;  ///< 独立 ACK 的最小发送间隔。
    std::uint32_t hello_initial_pto_ms = 200;
    std::uint32_t hello_max_pto_ms = 2000;
    std::uint32_t hello_max_attempts = 10;
    /// 0 = 时钟+pid 派生；测试可固定，便于复现。
    std::uint32_t random_seed = 0;
    /// STREAM 通道默认配置（与 Link 默认一致：动态 PTO/快速重传/分片/流控）。
    ReliableChannel::Config reliable;
};

/// @brief 单个 peer 的观测信息快照。
struct PeerInfo {
    std::uint32_t peer_id = 0;
    PeerState state = PeerState::kIdle;
    std::string address;     ///< 远端地址（点分/冒号形式）。
    std::uint16_t port = 0;  ///< 远端端口（主机字节序）。
    std::uint32_t conn_id = 0;
    bool outgoing = false;   ///< true = 本端主动发起（AddPeer）的 peer。
    std::uint32_t hello_attempts = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t frames_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t bytes_sent = 0;
};

/// @brief Hub 级计数器（peer 数、帧数、错误、握手）。
/// @note 每个 peer 的应用层计数请用 reliable_stats/unreliable_stats。
struct PeerHubStats {
    std::uint64_t peers_active = 0;   ///< 当前 kConnected 的 peer 数。
    std::uint64_t peers_created = 0;  ///< 累计创建的 peer 记录数。
    std::uint64_t peers_rejected = 0; ///< 超上限被拒的 HELLO 数。
    std::uint64_t frames_sent = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t invalid_frames = 0;        ///< 帧格式/版本非法。
    std::uint64_t unknown_peer_frames = 0;   ///< 找不到路由目标的帧。
    std::uint64_t conn_id_conflicts = 0;     ///< 提议的 conn_id 已被占用。
    std::uint64_t handshakes = 0;            ///< 完成的握手数。
    std::uint64_t hello_retransmits = 0;
    std::uint64_t heartbeats_sent = 0;
    std::uint64_t acks_sent = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t send_errors = 0;
    std::uint64_t recv_errors = 0;
    std::uint64_t poll_errors = 0;
};

/// @brief 多 peer UDP 链路：一个 socket 对 N 个对端，按 peer_id 路由。
/// @note 线程模型与 Link 一致：接收由单线程调用 RunOnce 驱动；Send* 可在
///       任意线程并发调用（内部 recursive_mutex）；回调在 RunOnce 线程内
///       同步执行，回调里可以继续 Send*，但不要重入 RunOnce。
class PeerHub final {
   public:
    explicit PeerHub(PeerHubConfig config);
    ~PeerHub();

    PeerHub(const PeerHub&) = delete;
    PeerHub& operator=(const PeerHub&) = delete;

    /// @brief 创建并绑定 UDP socket。
    bool Open(std::string* error = nullptr);

    /// @brief 关闭 socket 与所有 peer（幂等）。
    void Close();

    /// @brief 注册一个主动对端并发起握手（peer_id 必须非 0 且未被占用）。
    /// @param address IPv4/IPv6 字面量或主机名。
    /// @return 成功注册并发出首个 HELLO 返回 true。
    bool AddPeer(std::uint32_t peer_id, const std::string& address, std::uint16_t port,
                 std::string* error = nullptr);

    /// @brief 对已注册的 peer 重新发起握手（超时/断链后重连）。
    /// @return peer 存在且处于 kIdle/kTimedOut/kClosed 时返回 true。
    bool Connect(std::uint32_t peer_id);

    /// @brief 关闭并移除一个 peer。
    bool RemovePeer(std::uint32_t peer_id);

    bool HasPeer(std::uint32_t peer_id) const;
    /// @brief peer 不存在时返回 kClosed。
    PeerState peer_state(std::uint32_t peer_id) const;
    bool peer_connected(std::uint32_t peer_id) const;
    /// @brief 当前所有 peer 的快照（按 peer_id 升序）。
    std::vector<PeerInfo> Peers() const;
    /// @brief 当前所有 peer 的 id（按升序）。
    std::vector<std::uint32_t> PeerIds() const;

    /// @brief 处理一轮收发与定时器。返回本轮处理的帧数。
    int RunOnce(int timeout_ms);
    /// @brief 循环运行指定时长。
    int RunFor(int duration_ms);

    /// @brief 向指定 peer 发送可靠消息（线程安全）。
    bool SendStream(std::uint32_t peer_id, std::uint8_t channel, const void* data,
                    std::size_t bytes, std::uint32_t dep_seq = 0);
    /// @brief 非阻塞可靠发送（区分 WouldBlock 与永久错误）。
    SendStatus TrySendStream(std::uint32_t peer_id, std::uint8_t channel, const void* data,
                             std::size_t bytes, std::uint32_t dep_seq = 0);
    /// @brief 向指定 peer 发送不可靠消息（线程安全）。
    bool SendDatagram(std::uint32_t peer_id, std::uint8_t channel, const void* data,
                      std::size_t bytes, std::uint32_t dep_seq = 0);
    /// @brief 向指定 peer 发送因果屏障。
    bool SendBarrier(std::uint32_t peer_id, std::uint32_t barrier_seq);

    /// @brief 单个 peer 单个通道的可靠层统计（peer 不存在时返回默认值）。
    ReliableChannel::Stats reliable_stats(std::uint32_t peer_id, std::uint8_t channel) const;
    UnreliableChannel::Stats unreliable_stats(std::uint32_t peer_id, std::uint8_t channel) const;
    /// @brief Hub 级计数器快照（线程安全）。
    PeerHubStats stats() const;

    std::uint16_t local_port() const noexcept {
        return m_socket.local_port();
    }
    bool is_open() const noexcept {
        return m_socket.IsOpen();
    }

    // ---- 回调（RunOnce 内同步触发） ----
    std::function<void(std::uint32_t peer_id)> on_peer_connected;
    std::function<void(std::uint32_t peer_id, const char* reason)> on_peer_disconnected;
    std::function<void(std::uint32_t peer_id, std::uint8_t channel, const std::uint8_t* data,
                       std::size_t bytes)>
        on_stream;
    std::function<void(std::uint32_t peer_id, std::uint8_t channel, const std::uint8_t* data,
                       std::size_t bytes)>
        on_datagram;

   private:
    /// 同时存在的可靠 + 不可靠通道上限（与 Link 一致，防对端随机 chan 撑爆内存）。
    static constexpr std::size_t kMaxChannels = 8;
    /// 单个 RunOnce 最多处理的帧数，避免持续流量饿死定时器。
    static constexpr int kMaxFramesPerRun = 256;

    /// @brief 一个对端的全部会话状态。析构时释放其通道。
    struct Peer;

    // ---- 内部实现（全部在持锁下调用） ----
    Peer* FindByConnId(std::uint32_t conn_id);
    Peer* FindByAddress(const sockaddr_storage& addr, socklen_t len);
    std::uint32_t AllocatePeerId();
    Peer* CreatePeer(std::uint32_t peer_id, const sockaddr_storage& addr, socklen_t len);
    void ResetChannels(Peer& peer);
    ReliableChannel* GetOrCreateReliable(Peer& peer, std::uint8_t channel);
    UnreliableChannel* GetOrCreateUnreliable(Peer& peer, std::uint8_t channel);
    bool SendBuiltFrame(Peer& peer, const std::uint8_t* data, std::size_t bytes);
    bool SendControl(Peer& peer, const FrameHeader& header, const std::uint8_t* payload,
                     std::size_t bytes);
    void SendHello(Peer& peer);
    bool SendAckFrame(Peer& peer, std::uint8_t channel, bool heartbeat);
    bool SendMaxDataFrame(Peer& peer, std::uint8_t channel, std::uint64_t limit);
    void HandlePacket(const std::uint8_t* data, std::size_t bytes, const sockaddr_storage& from,
                      socklen_t from_len);
    void HandleHello(const FrameView& view, const sockaddr_storage& from, socklen_t from_len);
    void HandleHelloAck(const FrameView& view, const sockaddr_storage& from);
    void DispatchFrame(Peer& peer, const FrameView& view);
    void StartHandshake(Peer& peer);
    void MarkConnected(Peer& peer);
    void DropPeer(Peer& peer, const char* reason);
    void Tick(std::uint64_t now_ms);

    PeerHubConfig m_config;
    UdpSocket m_socket;
    std::map<std::uint32_t, std::unique_ptr<Peer>> m_peers;
    std::map<std::uint32_t, std::uint32_t> m_by_conn_id;  ///< conn_id → peer_id
    std::uint32_t m_next_peer_id = 1;
    std::uint32_t m_session_id = 0;
    PeerHubStats m_stats;
    std::vector<std::uint8_t> m_rx_buffer;
    /// 保护 peer 表、socket 发送与统计；recursive 以支持回调重入 Send*。
    mutable std::recursive_mutex m_mutex;
};

}  // namespace go2cpp::bento
