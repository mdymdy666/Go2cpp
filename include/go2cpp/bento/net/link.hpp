#pragma once

/**
 * @file link.hpp
 * @brief Bento 的 UDP 链路层：socket、HELLO 握手、心跳、超时与事件循环。
 * @details Link 是单线程对象：所有公开方法与回调都必须在同一个线程调用，
 *          不做内部加锁。事件循环由使用方驱动（RunOnce/RunFor），
 *          采用「poll + 每次 tick 检查定时器」的阻塞模型，不额外起线程，
 *          便于在测试与 Fiber 调度器里复用。
 *
 *          状态机：
 *          Idle --Connect()--> Connecting --HELLO_ACK--> Connected
 *          Idle --收到 HELLO（监听方）--> Connected
 *          Connecting --超过 handshake_timeout_ms / 重试上限--> TimedOut
 *          Connected --超过 peer_timeout_ms 无任何有效帧--> TimedOut
 *          任意状态 --Close()--> Closed
 *
 *          HELLO 重传使用 200ms 起步、×2 退避、2s 上限（与可靠层 PTO 同构）。
 *         心跳是纯 ACK 帧：每 heartbeat_interval_ms 至少发一帧，既刷新
 *         对端活跃时间，也顺带补发尚未送达的 ACK。
 *
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/bento/net/relation.hpp"

namespace go2cpp::bento {

/// @brief Link 配置。peer_address 为空表示监听方（被动等 HELLO）。
struct LinkConfig {
    std::string bind_address = "127.0.0.1";  ///< IPv4 点分十进制；不支持域名。
    std::uint16_t bind_port = 0;             ///< 0 = 由内核分配。
    std::string peer_address;                ///< 空 = 监听方。
    std::uint16_t peer_port = 0;
    std::uint32_t handshake_timeout_ms = 5000;
    std::uint32_t peer_timeout_ms = 3000;  ///< 连接后多久收不到有效帧判定断链。
    std::uint32_t heartbeat_interval_ms = 500;
    std::uint32_t ack_interval_ms = 20;  ///< 独立 ACK 的最小发送间隔。
    std::uint32_t hello_initial_pto_ms = 200;
    std::uint32_t hello_max_pto_ms = 2000;
    std::uint32_t hello_max_attempts = 10;
    std::uint32_t random_seed = 0;  ///< 0 = 时钟+pid；测试可固定。
};

/// @brief 链路状态。
enum class LinkState : std::uint8_t {
    kIdle = 0,        ///< 已 bind，尚未开始握手。
    kConnecting = 1,  ///< 客户端已发出 HELLO，等待 HELLO_ACK。
    kConnected = 2,   ///< 握手完成，可收发数据帧。
    kClosed = 3,      ///< 主动关闭。
    kTimedOut = 4,    ///< 握手或心跳超时。
};

/// @brief 状态名（日志/测试用）。
const char* LinkStateName(LinkState state) noexcept;

/// @brief IPv4 UDP socket 的 RAII 封装。非阻塞，Linux only。
class UdpSocket final {
   public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// @brief 创建、绑定并设置为非阻塞。
    /// @param error 失败原因（可为 nullptr）。
    bool Open(const std::string& bind_address, std::uint16_t port, std::string* error);

    /// @brief 关闭 fd；幂等。
    void Close() noexcept;

    bool IsOpen() const noexcept {
        return m_fd >= 0;
    }
    int fd() const noexcept {
        return m_fd;
    }
    std::uint16_t local_port() const noexcept {
        return m_local_port;
    }

    /// @brief 发送一个数据报。返回 false 表示 sendto 失败（含 EAGAIN）。
    bool SendTo(const sockaddr_in& peer, const std::uint8_t* data, std::size_t bytes) noexcept;

    /// @brief poll 等待可读。返回 1 可读、0 超时、-1 错误。
    int WaitReadable(int timeout_ms) noexcept;

    /// @brief 非阻塞接收。返回 >0 字节数、0 表示当前无数据、-1 表示错误。
    int RecvFrom(std::uint8_t* buffer, std::size_t capacity, sockaddr_in* from) noexcept;

    /// @brief 解析 IPv4 点分十进制地址。
    static bool ResolveIPv4(const std::string& address, std::uint16_t port, sockaddr_in* out,
                            std::string* error);

    /// @brief 比较地址与端口是否相同（不含 scope id）。
    static bool SamePeer(const sockaddr_in& a, const sockaddr_in& b) noexcept;

   private:
    int m_fd = -1;
    std::uint16_t m_local_port = 0;
};

/// @brief 链路层计数器。
struct LinkStats {
    std::uint64_t frames_sent = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t invalid_frames = 0;       ///< 帧格式/长度非法。
    std::uint64_t unknown_peer_frames = 0;  ///< 来源不是已握手 peer，或状态不允许。
    std::uint64_t send_errors = 0;
    std::uint64_t recv_errors = 0;
    std::uint64_t poll_errors = 0;
    std::uint64_t handshakes = 0;
    std::uint64_t hello_retransmits = 0;
    std::uint64_t heartbeats_sent = 0;
    std::uint64_t acks_sent = 0;
    std::uint64_t barriers_received = 0;
    std::uint64_t timeouts = 0;
};

/// @brief 单条 UDP 链路上的 Bento 端点（客户端或监听方）。
class Link final {
   public:
    explicit Link(LinkConfig config);
    ~Link();

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    /// @brief 创建并绑定 socket。监听方 peer_address 为空。
    bool Open(std::string* error = nullptr);

    /// @brief 客户端：发送 HELLO 进入 Connecting。监听方调用无效。
    void Connect();

    /// @brief 主动关闭（不触发 on_disconnected）。
    void Close();

    /// @brief 处理一轮收发与定时器。返回本轮收到的帧数。
    int RunOnce(int timeout_ms);

    /// @brief 循环运行指定时长。
    int RunFor(int duration_ms);

    /// @brief 发送可靠消息。未连接、通道耗尽或载荷超长返回 false。
    bool SendStream(std::uint8_t channel, const void* data, std::size_t bytes,
                    std::uint32_t dep_seq = 0);

    /// @brief 发送不可靠消息。返回 true 仅表示已交给 UDP，不保证送达。
    bool SendDatagram(std::uint8_t channel, const void* data, std::size_t bytes,
                      std::uint32_t dep_seq = 0);

    /// @brief 发送视频帧（自动分片，固定 channel=kChannelVideo）。
    bool SendVideoFrame(std::uint32_t frame_id, bool key, const void* data, std::size_t bytes);

    /// @brief 发送因果屏障：接收端作废 seq 之前的不可靠序号。
    bool SendBarrier(std::uint32_t barrier_seq);

    LinkState state() const noexcept {
        return m_state;
    }
    bool connected() const noexcept {
        return m_state == LinkState::kConnected;
    }
    std::uint16_t local_port() const noexcept {
        return m_socket.local_port();
    }
    std::uint32_t session_id() const noexcept {
        return m_session_id;
    }
    const LinkStats& stats() const noexcept {
        return m_stats;
    }

    ReliableChannel::Stats reliable_stats(std::uint8_t channel) const;
    UnreliableChannel::Stats unreliable_stats(std::uint8_t channel) const;
    VideoChannel::Stats video_stats() const;

    /// 以下回调均在 RunOnce 内同步触发；回调里可以继续调用 Send*。
    std::function<void()> on_connected;
    std::function<void(const char* reason)> on_disconnected;
    std::function<void(std::uint8_t channel, const std::uint8_t* data, std::size_t bytes)>
        on_stream;
    std::function<void(std::uint8_t channel, const std::uint8_t* data, std::size_t bytes)>
        on_datagram;
    std::function<void(std::uint8_t channel, std::uint32_t frame_id, bool key,
                       const std::uint8_t* data, std::size_t bytes)>
        on_video;

   private:
    /// 同时存在的可靠 + 不可靠通道上限，防止对端用随机 chan 撑爆内存。
    static constexpr std::size_t kMaxChannels = 8;
    /// 单个 RunOnce 最多处理的帧数，避免持续流量饿死定时器。
    static constexpr int kMaxFramesPerRun = 256;

    ReliableChannel* GetOrCreateReliable(std::uint8_t channel);
    UnreliableChannel* GetOrCreateUnreliable(std::uint8_t channel);
    bool SendBuiltFrame(const std::uint8_t* data, std::size_t bytes);
    bool SendControl(const FrameHeader& header, const AckState* ack, const std::uint8_t* payload,
                     std::size_t bytes);
    void HandleFrame(const std::uint8_t* data, std::size_t bytes, const sockaddr_in& from);
    void HandleHello(const FrameView& view, const sockaddr_in& from);
    void HandleHelloAck(const FrameView& view, const sockaddr_in& from);
    void HandleAckFrame(const FrameView& view);
    void HandleBarrier(const FrameView& view);
    void SendHello();
    bool SendAckFrame(std::uint8_t channel, bool heartbeat);
    void Tick(std::uint64_t now_ms);
    void Fail(const char* reason);
    bool PeerMatches(const sockaddr_in& from) const noexcept;

    LinkConfig m_config;
    UdpSocket m_socket;
    LinkState m_state = LinkState::kIdle;
    bool m_is_listener = false;
    bool m_peer_configured = false;
    bool m_have_peer = false;
    sockaddr_in m_peer{};
    std::uint32_t m_session_id = 0;

    std::uint64_t m_handshake_start_ms = 0;
    std::uint64_t m_last_rx_ms = 0;
    std::uint64_t m_last_tx_ms = 0;
    std::uint64_t m_next_hello_ms = 0;
    std::uint64_t m_next_ack_tick_ms = 0;
    std::uint64_t m_next_heartbeat_ms = 0;
    std::uint32_t m_hello_attempts = 0;
    std::uint32_t m_hello_pto_ms = 0;

    std::map<std::uint8_t, std::unique_ptr<ReliableChannel>> m_reliable;
    std::map<std::uint8_t, std::unique_ptr<UnreliableChannel>> m_unreliable;
    std::unique_ptr<VideoChannel> m_video;
    Relation m_relation;

    LinkStats m_stats;
    std::vector<std::uint8_t> m_rx_buffer;
};

}  // namespace go2cpp::bento
