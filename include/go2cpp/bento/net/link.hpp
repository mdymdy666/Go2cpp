#pragma once

/**
 * @file link.hpp
 * @brief Bento 的 UDP 链路层：socket、HELLO 握手、心跳、超时与事件循环。
 * @details Link 是单线程接收 + 多线程发送的混合模型：
 *
 *          - **接收**：RunOnce/RunFor 由使用方在单一接收线程驱动，采用
 *            「poll + 每次 tick 检查定时器」的阻塞模型，不额外起线程。
 *          - **发送**：SendStream/SendDatagram/SendVideoFrame/SendBarrier/
 *            ConcurrentSend 可以在任意线程并发调用；Link 内部用递归互斥量
 *            串行化对通道状态、socket 与统计的访问。因为持锁期间可能触发
 *            回调（回调里允许再次 Send*），所以使用 recursive_mutex。
 *          - **回调**：on_connected/on_disconnected/on_stream/on_datagram/
 *            on_video/on_bandwidth 都在 RunOnce 的接收线程内同步执行；
 *            回调里可以继续调用 Send*，但不要调用 RunOnce（会阻塞持锁的
 *            发送线程，且接收线程不可重入）。
 *
 *          设备地址支持 IPv4、IPv6 与主机名（getaddrinfo AF_UNSPEC）。
 *
 *          状态机：
 *          Idle --Connect()--> Connecting --HELLO_ACK--> Connected
 *          Idle --收到 HELLO（监听方）--> Connected
 *          Connecting --超过 handshake_timeout_ms / 重试上限--> TimedOut
 *          Connected --超过 peer_timeout_ms 无任何有效帧--> TimedOut
 *          任意状态 --Close()--> Closed
 *
 *          HELLO 重传使用 200ms 起步、×2 退避、2s 上限（与可靠层 PTO 同构）。
 *          心跳是纯 ACK 帧：每 heartbeat_interval_ms 至少发一帧，既刷新
 *          对端活跃时间，也顺带补发尚未送达的 ACK。
 *
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/crypto.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/bento/net/relation.hpp"

namespace go2cpp::bento {

/// @brief Link 配置。remote 为空表示监听方（被动等 HELLO）。
struct LinkConfig {
    /// 绑定地址：IPv4/IPv6 字面量或主机名；"0.0.0.0"/"::" 表示通配。
    std::string bind_address = "127.0.0.1";
    std::uint16_t bind_port = 0;  ///< 0 = 由内核分配。
    /// 对端地址（IPv4/IPv6/主机名）。为空 = 监听方。
    /// 保留旧字段名以兼容既有代码；新代码建议用 remote。
    std::string peer_address;
    /// 对端地址（P1）：非空时优先于 peer_address，支持 IPv6/域名。
    std::string remote;
    std::uint16_t peer_port = 0;
    std::uint32_t handshake_timeout_ms = 5000;
    std::uint32_t peer_timeout_ms = 3000;  ///< 连接后多久收不到有效帧判定断链。
    std::uint32_t heartbeat_interval_ms = 500;
    std::uint32_t ack_interval_ms = 20;  ///< 独立 ACK 的最小发送间隔。
    std::uint32_t hello_initial_pto_ms = 200;
    std::uint32_t hello_max_pto_ms = 2000;
    std::uint32_t hello_max_attempts = 10;
    std::uint32_t random_seed = 0;  ///< 0 = 时钟+pid；测试可固定。
    /// ABR 回调最小间隔（毫秒）。带宽变化超过 10% 时才回调。
    std::uint32_t abr_interval_ms = 200;
    /// PSK 认证加密：非空时启用（长度必须 32B）；空 = 明文模式，保持旧线格式。
    /// 双方必须配置相同 PSK；监听方未配置 PSK 时无法解密加密 HELLO。
    std::vector<std::uint8_t> pre_shared_key;
    /// 握手时间戳容忍窗口（毫秒）；0 = 不校验。依赖双方时钟大致同步。
    std::uint32_t handshake_replay_window_ms = 30000;
    /// STREAM 通道默认配置：动态 PTO（RFC 9002）、快速重传、分片重组、
    /// 拥塞控制、pacing、流控（MAX_DATA）与带宽估算。
    ReliableChannel::Config reliable;

    // ---- P2 ----
    /// VIDEO XOR FEC 分组大小：0=关闭（默认，保持既有线格式/测试），
    /// 2..8 启用（推荐 4 或 8）。仅影响 kChannelVideo 的发送端。
    std::uint16_t video_fec_group_size = 0;
    /// 是否启用路径 MTU 探测（DF + 1200→1500 递增探针）。
    /// 默认关闭以保持既有测试的时序/帧计数；生产建议开启。
    bool enable_pmtud = false;
    /// 探测上限（UDP 载荷字节，含加密封装开销）。
    std::uint32_t pmtud_max_mtu = 1500;
    /// 相邻两档探针的最小间隔（毫秒）。
    std::uint32_t pmtud_probe_interval_ms = 30;
    /// 单档探针等待 ACK 的超时（毫秒）。
    std::uint32_t pmtud_probe_timeout_ms = 300;
    /// 单档探针重试次数；全部失败视为黑洞，保持当前 MTU。
    std::uint32_t pmtud_probe_attempts = 3;
    /// 0-RTT 会话票据有效期（毫秒）；0 = 禁用票据导出。
    std::uint32_t session_ticket_lifetime_ms = 60000;
    /// 对端迁移地址后的旧地址静默期（毫秒）：期内旧地址的包直接丢弃。
    std::uint32_t migration_grace_ms = 3000;
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

/// @brief 通用（IPv4/IPv6）UDP socket 的 RAII 封装。非阻塞，Linux only。
class UdpSocket final {
   public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    /// @brief 创建、绑定并设置为非阻塞。地址由 getaddrinfo(AF_UNSPEC) 解析。
    /// @param error 失败原因（可为 nullptr）。
    bool Open(const std::string& bind_address, std::uint16_t port, std::string* error);

    /// @brief 配置 PMTUD 相关 socket 选项：DF 位（IP_MTU_DISCOVER）与
    ///        ICMP 错误队列（IP_RECVERR）。幂等；失败时静默降级。
    void ConfigurePathMtu(bool enable) noexcept;

    /// @brief 非阻塞读取 ICMP 错误队列中的 MTU 事件。
    /// @param reported_mtu 输出内核报告的路径 MTU（UDP 载荷字节）。
    /// @return 1 读到事件、0 无事件、-1 致命错误（应停止轮询）。
    int ReceiveMtuError(std::uint32_t* reported_mtu) noexcept;

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
    /// @brief 绑定的地址族：AF_INET 或 AF_INET6。
    int family() const noexcept {
        return m_family;
    }

    /// @brief 发送一个数据报到通用地址。返回 false 表示 sendto 失败。
    bool SendTo(const sockaddr_storage& peer, socklen_t peer_len, const std::uint8_t* data,
                std::size_t bytes) noexcept;

    /// @brief IPv4 兼容重载。
    bool SendTo(const sockaddr_in& peer, const std::uint8_t* data, std::size_t bytes) noexcept;

    /// @brief poll 等待可读。返回 1 可读、0 超时、-1 错误。
    int WaitReadable(int timeout_ms) noexcept;

    /// @brief 非阻塞接收通用地址。返回 >0 字节数、0 表示当前无数据、-1 表示错误。
    int RecvFrom(std::uint8_t* buffer, std::size_t capacity, sockaddr_storage* from,
                 socklen_t* from_len) noexcept;

    /// @brief IPv4 兼容重载；收到非 IPv4 包返回 -1。
    int RecvFrom(std::uint8_t* buffer, std::size_t capacity, sockaddr_in* from) noexcept;

    /// @brief 解析主机名/IPv4/IPv6 字面量（AF_UNSPEC，SOCK_DGRAM）。
    /// @param out_len 输出 sockaddr 实际长度。
    static bool Resolve(const std::string& address, std::uint16_t port, sockaddr_storage* out,
                        socklen_t* out_len, std::string* error);

    /// @brief 解析 IPv4 点分十进制（兼容旧接口）。
    static bool ResolveIPv4(const std::string& address, std::uint16_t port, sockaddr_in* out,
                            std::string* error);

    /// @brief 比较通用地址（family、端口、地址字节、IPv6 scope id）。
    static bool SamePeer(const sockaddr_storage& a, const sockaddr_storage& b) noexcept;

    /// @brief 比较 IPv4 地址（兼容旧接口）。
    static bool SamePeer(const sockaddr_in& a, const sockaddr_in& b) noexcept;

   private:
    int m_fd = -1;
    std::uint16_t m_local_port = 0;
    int m_family = AF_UNSPEC;
};

/// @brief 链路层计数器 + 可靠通道聚合统计（P1 扩展）。
struct LinkStats {
    // ---- 链路层（原有） ----
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

    // ---- P2：连接迁移 / PMTUD ----
    std::uint64_t conn_id_mismatch_frames = 0;  ///< conn_id 不匹配而丢弃的帧。
    std::uint64_t stale_peer_frames = 0;        ///< 迁移静默期内旧地址来的帧。
    std::uint64_t peer_migrations = 0;          ///< 通过 conn_id 识别出的对端换址次数。
    std::uint64_t pmtud_probes_sent = 0;
    std::uint64_t pmtud_probes_received = 0;
    std::uint64_t pmtud_acks_received = 0;
    std::uint64_t pmtud_icmp_errors = 0;  ///< ICMP/EMSGSIZE 触发的降 MTU 次数。
    std::uint64_t pmtud_blackholes = 0;   ///< 探针超时且无 ICMP 的次数（保持保守值）。

    // ---- 可靠通道聚合（P1，Stats() 刷新） ----
    std::uint32_t rtt_ms = 0;           ///< 最近一次未重传消息的 RTT 样本。
    std::uint32_t srtt_ms = 0;          ///< 平滑 RTT。
    std::uint32_t rttvar_ms = 0;        ///< RTT 方差。
    std::uint32_t pto_ms = 0;           ///< 当前基础 PTO。
    std::uint32_t cwnd_packets = 0;     ///< 拥塞窗口（多通道求和）。
    std::uint32_t ssthresh_packets = 0; ///< 慢启动阈值（多通道求和）。
    std::uint64_t inflight = 0;         ///< 未确认消息数（多通道求和）。
    std::uint64_t loss_events = 0;      ///< 丢包降窗事件数。
    std::uint64_t retransmits = 0;      ///< 重传次数。
    std::uint64_t bandwidth_bps = 0;    ///< 交付速率估计（bit/s）。
    double loss_rate = 0.0;             ///< 丢包率（0.0..1.0）。
    std::uint64_t flow_control_blocked = 0;  ///< 流控 WouldBlock 次数。
    std::uint64_t max_data_sent = 0;         ///< 发出的 MAX_DATA 帧数。
    std::uint64_t max_data_received = 0;     ///< 收到的 MAX_DATA 帧数。
};

/// @brief PSK 认证加密的可观测计数器。
struct SecureStats {
    std::uint64_t packets_sealed = 0;      ///< 发出的加密帧数。
    std::uint64_t packets_opened = 0;      ///< 成功解密的加密帧数。
    std::uint64_t auth_failures = 0;       ///< AEAD 校验失败（篡改/错误密钥）。
    std::uint64_t replay_drops = 0;        ///< 重放窗口拒绝的包数。
    std::uint64_t malformed_frames = 0;    ///< 结构/长度非法的加密包。
    std::uint64_t plaintext_rejected = 0;  ///< 加密模式收到的明文数据帧。
    std::uint64_t handshakes = 0;          ///< 完成的加密握手数。
    std::uint64_t handshake_retries = 0;   ///< 幂等重发的握手包数。
    std::uint64_t encrypted_bytes_sent = 0;
    std::uint64_t encrypted_bytes_received = 0;
    std::uint64_t resumptions = 0;       ///< 成功进入 0-RTT 恢复的会话数。
    std::uint64_t zero_rtt_frames = 0;   ///< 0-RTT 恢复后首个飞行内接受的帧数。
    std::uint64_t tickets_exported = 0;  ///< 成功导出的会话票据数。
    std::uint64_t tickets_rejected = 0;  ///< Resume 拒绝的票据数（过期/篡改/PSK 不符）。
};

/// @brief 把 LinkStats 导出为单行 JSON 对象（字段名见实现）。
std::string LinkStatsToJson(const LinkStats& stats);

/// @brief 单条 UDP 链路上的 Bento 端点（客户端或监听方）。
class Link final {
   public:
    explicit Link(LinkConfig config);
    ~Link();

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    /// @brief 创建并绑定 socket。监听方 remote/peer_address 为空。
    bool Open(std::string* error = nullptr);

    /// @brief 设置 32B PSK，启用认证加密。必须在 Open/Connect 之前调用。
    /// @return 长度不是 32B 或 key 为空返回 false（保持原模式）。
    bool SetPreSharedKey(const std::uint8_t* key, std::size_t bytes);

    /// @brief 是否配置了 PSK（加密模式）。
    bool secure() const noexcept {
        return m_secure_enabled;
    }
    /// @brief 会话密钥是否已就绪（加密握手完成）。
    bool session_established() const noexcept {
        return m_session_keys_ready;
    }
    const SecureStats& secure_stats() const noexcept {
        return m_secure_stats;
    }

    /// @brief 客户端：发送 HELLO 进入 Connecting。监听方调用无效。
    void Connect();

    /// @brief 主动关闭（不触发 on_disconnected）。
    void Close();

    /// @brief 处理一轮收发与定时器。返回本轮收到的帧数。
    /// @note 只在接收线程调用；持锁期间会阻塞并发发送。
    int RunOnce(int timeout_ms);

    /// @brief 循环运行指定时长。
    int RunFor(int duration_ms);

    /// @brief 发送可靠消息（线程安全）。未连接、窗口满或载荷超长返回 false。
    bool SendStream(std::uint8_t channel, const void* data, std::size_t bytes,
                    std::uint32_t dep_seq = 0);

    /// @brief 非阻塞可靠发送（线程安全），区分 WouldBlock 与永久错误。
    SendStatus TrySendStream(std::uint8_t channel, const void* data, std::size_t bytes,
                             std::uint32_t dep_seq = 0);

    /// @brief 多线程友好的显式发送入口；与 SendStream 等价（内部有锁）。
    /// @details 线程模型：接收由单个线程跑 RunOnce；任意线程可调用本方法
    ///          并发发送。消息的可靠有序性由通道 seq 保证，调用线程之间
    ///          的先后顺序不保证。
    bool ConcurrentSend(std::uint8_t channel, const void* data, std::size_t bytes,
                        std::uint32_t dep_seq = 0);

    /// @brief 发送不可靠消息（线程安全）。返回 true 仅表示已交给 UDP。
    bool SendDatagram(std::uint8_t channel, const void* data, std::size_t bytes,
                      std::uint32_t dep_seq = 0);

    /// @brief 发送视频帧（自动分片，固定 channel=kChannelVideo，线程安全）。
    bool SendVideoFrame(std::uint32_t frame_id, bool key, const void* data, std::size_t bytes);

    /// @brief 发送因果屏障（线程安全）。
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
    /// @brief 当前连接 id（HELLO 协商；0 表示尚未协商）。
    std::uint32_t conn_id() const;
    /// @brief 当前路径 MTU 估计（UDP 载荷字节）。无探测/无 ICMP 时保守 1200。
    std::uint32_t PathMtu() const;
    /// @brief 注入一条 ICMP“包太大/不可达”结果（平台无 IP_RECVERR 时由上层调用）。
    /// @details 只降不升；值被夹在 [576, pmtud_max_mtu]。测试也用它验证降档。
    void NotifyPathMtuError(std::uint32_t reported_mtu);
    /// @brief 本地地址迁移（NAT rebinding / 换网卡）：保留 conn_id 与会话状态，
    ///        关闭旧 socket 并在 new_address:new_port 上重建，下一包即从新地址发出。
    /// @param new_port 0 = 内核分配。
    /// @note 对端按 conn_id 识别后自动迁移；旧地址的包在 migration_grace_ms
    ///       内被丢弃。新地址族必须与对端一致（IPv4/IPv6 不混用）。
    bool Migrate(const std::string& new_address, std::uint16_t new_port = 0,
                 std::string* error = nullptr);
    /// @brief 导出 0-RTT 会话票据（仅 PSK 模式且会话密钥就绪时非空）。
    /// @details 票据由 PSK 派生 MAC，含 conn_id、双方 nonce 与两个方向的
    ///          下一个 wire seq；不包含密钥本身。
    std::vector<std::uint8_t> ExportSessionTicket();
    /// @brief 用票据恢复会话并直接进入 Connected（0-RTT），不发送 HELLO。
    /// @details 成功后首个 Send* 即用恢复出的会话密钥加密；接收端通过
    ///          conn_id + 会话缓存接受。票据过期/篡改/PSK 不符返回 false。
    bool Resume(const std::vector<std::uint8_t>& ticket);
    /// @brief 是否通过 Resume 进入当前会话。
    bool resumed() const noexcept {
        return m_resumed;
    }
    /// @brief 清空进程内 0-RTT 会话缓存（测试隔离用；生产无需调用）。
    static void ClearSessionCache();
    /// @brief 原始链路层计数器（不加锁；并发场景请用 Stats()）。
    const LinkStats& stats() const noexcept {
        return m_stats;
    }

    /// @brief 聚合快照：链路层计数 + 可靠通道 RTT/拥塞/流控/带宽。
    LinkStats Stats() const;

    /// @brief Stats() 的 JSON 字符串导出（线程安全）。
    std::string StatsJson() const;

    /// @brief 链路交付速率估计（bit/s），所有可靠通道求和。
    std::uint64_t BandwidthEstimate() const;

    /// @brief 链路丢包率估计（0.0..1.0），所有可靠通道聚合。
    double LossRate() const;

    ReliableChannel::Stats reliable_stats(std::uint8_t channel) const;
    UnreliableChannel::Stats unreliable_stats(std::uint8_t channel) const;
    VideoChannel::Stats video_stats() const;
    /// @brief 拥塞控制统计（cwnd/ssthresh/pacing/丢包/超时）。
    CongestionStats congestion_stats(std::uint8_t channel) const;

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
    /// ABR 回调：估算带宽（bit/s）变化超过 10% 时触发；应用据此调整视频码率。
    std::function<void(std::uint64_t bitrate_bps)> on_bandwidth;

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
    bool SendRawPacket(const std::uint8_t* data, std::size_t bytes);
    bool SendMaxDataFrame(std::uint8_t channel, std::uint64_t limit);
    bool SealAndSend(const std::uint8_t* frame, std::size_t bytes);
    void HandleFrame(const std::uint8_t* data, std::size_t bytes, const sockaddr_storage& from,
                     socklen_t from_len);
    void DispatchFrame(const FrameView& view);
    void HandleHelloPacket(const std::uint8_t* data, std::size_t bytes, const FrameHeader& outer,
                           const sockaddr_storage& from, socklen_t from_len);
    void HandleHelloAckPacket(const std::uint8_t* data, std::size_t bytes,
                              const FrameHeader& outer, const sockaddr_storage& from);
    void HandleHello(const FrameView& view, const sockaddr_storage& from, socklen_t from_len);
    void HandleHelloAck(const FrameView& view, const sockaddr_storage& from);
    bool HandleSecureHello(const std::uint8_t* data, std::size_t bytes, const FrameHeader& outer,
                           const sockaddr_storage& from, socklen_t from_len);
    bool HandleSecureHelloAck(const std::uint8_t* data, std::size_t bytes,
                              const FrameHeader& outer, const sockaddr_storage& from);
    bool SendSecureHello();
    bool SendSecureHelloAck();
    bool IsKnownClientNonce(const std::uint8_t* nonce) const noexcept;
    void RememberClientNonce(const std::uint8_t* nonce);
    bool TimestampAcceptable(std::uint64_t timestamp_ms) const noexcept;
    void ResetSecureSession();
    void HandleAckFrame(const FrameView& view);
    void HandleBarrier(const FrameView& view);
    void HandlePmtudProbe(const FrameView& view);
    void HandlePmtudAck(const FrameView& view);
    bool SendPmtudProbe();
    void PollPathMtuErrors();
    void ResetPmtud(std::uint64_t now_ms);
    void MaybeProbePmtud(std::uint64_t now_ms);
    void ResetApplicationChannels();
    void NoteResumeEpoch(std::uint8_t flags);
    bool ConnIdMatches(const FrameHeader& header) const noexcept;
    bool IsRetiredPeer(const sockaddr_storage& from, std::uint64_t now_ms) const noexcept;
    void UpdatePeerAddress(const sockaddr_storage& from, socklen_t from_len);
    bool TryAdoptCachedSession(const std::uint8_t* packet, std::size_t bytes,
                               const FrameHeader& outer, const sockaddr_storage& from,
                               socklen_t from_len, std::vector<std::uint8_t>* inner);
    void CacheServerSession();
    void SendHello();
    bool SendAckFrame(std::uint8_t channel, bool heartbeat);
    void Tick(std::uint64_t now_ms);
    void MaybeReportBandwidth(std::uint64_t now_ms);
    std::uint64_t BandwidthEstimateUnlocked() const;
    double LossRateUnlocked() const;
    LinkStats StatsUnlocked() const;
    void Fail(const char* reason);
    bool PeerMatches(const sockaddr_storage& from) const noexcept;

    LinkConfig m_config;
    UdpSocket m_socket;
    LinkState m_state = LinkState::kIdle;
    bool m_is_listener = false;
    bool m_peer_configured = false;
    bool m_have_peer = false;
    sockaddr_storage m_peer{};
    socklen_t m_peer_len = 0;
    std::uint32_t m_session_id = 0;
    std::uint32_t m_conn_id = 0;  ///< P2：HELLO 协商的连接 id。
    bool m_resumed = false;       ///< 当前会话是否来自 Resume。
    /// 0-RTT 后到对端首个有效帧之前，所有发出帧都带 kFlagResume。
    bool m_resume_epoch = false;
    /// 接收方：是否已处理过对端的恢复首帧（只重置一次通道状态）。
    bool m_peer_resume_epoch_seen = false;
    /// 监听方：在回复帧上置 kFlagResumeAck，直到会话结束。
    bool m_send_resume_ack = false;

    // ---- 连接迁移（P2） ----
    sockaddr_storage m_retired_peer{};
    socklen_t m_retired_peer_len = 0;
    bool m_have_retired_peer = false;
    std::uint64_t m_retired_until_ms = 0;

    // ---- PMTUD（P2） ----
    std::uint32_t m_path_mtu = 1200;
    bool m_pmtud_done = false;
    bool m_pmtud_awaiting_ack = false;
    std::uint32_t m_pmtud_target = 1200;
    std::uint32_t m_pmtud_token = 0;
    std::uint32_t m_pmtud_attempts = 0;
    std::uint64_t m_pmtud_next_ms = 0;
    std::uint64_t m_pmtud_deadline_ms = 0;

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

    // ---- ABR ----
    std::uint64_t m_last_abr_ms = 0;
    std::uint64_t m_last_reported_bps = 0;

    // ---- PSK 认证加密状态（仅 m_secure_enabled 时使用） ----
    static constexpr std::size_t kMaxRememberedClientNonces = 8;

    bool m_secure_enabled = false;
    bool m_session_keys_ready = false;
    std::uint8_t m_psk[kPskBytes] = {};
    std::uint8_t m_client_nonce[kHandshakeNonceBytes] = {};
    std::uint8_t m_server_nonce[kHandshakeNonceBytes] = {};
    std::uint8_t m_tx_key[kChaCha20KeyBytes] = {};
    std::uint8_t m_rx_key[kChaCha20KeyBytes] = {};
    SecureDirection m_tx_direction = SecureDirection::kClientToServer;
    SecureDirection m_rx_direction = SecureDirection::kServerToClient;
    std::uint32_t m_tx_wire_seq = 0;
    /// 握手帧的 wire seq：每次连接随机，保证握手 AEAD nonce 不复用。
    std::uint32_t m_handshake_tx_seq = 1;
    ReplayWindow m_data_rx_window;
    ReplayWindow m_handshake_rx_window;
    std::vector<std::uint8_t> m_cached_hello;
    std::vector<std::uint8_t> m_cached_hello_ack;
    std::vector<std::array<std::uint8_t, kHandshakeNonceBytes>> m_seen_client_nonces;
    SecureStats m_secure_stats;

    /// 保护通道表、socket 发送、统计与流控状态；recursive 以支持回调重入。
    mutable std::recursive_mutex m_mutex;
};

}  // namespace go2cpp::bento
