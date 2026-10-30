#pragma once

/**
 * @file link_server.hpp
 * @brief Bento 的 UDP 服务端骨架：一个端口接 N 个 peer，STREAM（可靠）与
 *        DATAGRAM（不可靠）可在同一条链路上同时收发。
 * @details 本文件是 OmniNetServer 组合式架构里的 `AsLinkServer()` 落地
 *          （见 docs/modules/bento-roadmap.md §2.2）：它不发明新协议，而是把
 *          已经验证过的 Bento 传输原语组装成「可被应用直接使用的服务端」。
 *
 * ## 与 Link / PeerHub 的关系
 *
 * `Link` 是**单 peer** UDP 链路（一个 socket 只服务一个对端）；`PeerHub` 是
 * 同一套线格式与通道实现之上的**多 peer** 路由层（按 conn_id 路由，一个
 * socket 接 N 个对端）。服务端天然需要后者：
 *
 *   - `LinkServer` 内部持有且只持有一个 `PeerHub`（一个 UDP socket）；
 *   - peer 标识直接复用 `PeerHub` 的 peer_id（本地句柄，按来源地址自动
 *     分配，不上海缆）；
 *   - 帧编解码、HELLO 握手、心跳、PSK、可靠重传/分片、不可靠 seq 全部由
 *     `PeerHub` 及其下层 `ReliableChannel`/`UnreliableChannel` 提供，
 *     本类**不复制**任何协议实现。
 *
 * 因此 `LinkServer` 与 `Link`/`PeerHub` 线格式完全互通：用 `Link` 或
 * `PeerHub` 写的客户端可以直接连上来；反之亦然（测试里用 `PeerHub` 作
 * 客户端）。
 *
 * ## 可靠 + 不可靠同时传输（本类的核心价值）
 *
 * 两类流量共用一个 socket、一组 peer、一条加密会话，但走不同的通道语义：
 *
 *   | LinkReliability | 帧类型    | 语义                                     |
 *   |-----------------|-----------|------------------------------------------|
 *   | kReliable       | kStream   | 有序、不丢、自动分片（单条最大约 330KB） |
 *   | kUnreliable     | kDatagram | 不重传、可能丢失/乱序，单条 <= 1300B      |
 *
 * 调用方用同一个 `Send(peer, channel, data, reliability)` 选择语义；
 * `channel` 是应用层多路复用号（例如 0=控制、1=输入、2=遥测），两类通道
 * 各自独立编号、互不影响。接收侧分别由 `OnStream` / `OnDatagram` 交付。
 *
 * ## 线程模型（重要）
 *
 * 底层 `PeerHub` 的契约是「接收由单一线程调用 RunOnce 驱动，发送可任意
 * 线程并发」。`LinkServer` 有两种驱动方式：
 *
 *   1. **内置接收线程**（默认，`LinkServerConfig::start_receiver_thread`）：
 *      `Start()` 起一个线程循环 `hub.RunOnce(poll_timeout_ms)`，`Stop()` 停
 *      线程并 join。应用只需要注册回调、调用 `Send`/`Broadcast`。
 *   2. **外部驱动**（`start_receiver_thread = false`）：`Start()` 只置状态，
 *      由应用在自己的线程/事件循环里调用 `RunOnce`/`RunFor`。适合把
 *      LinkServer 嵌进已有的 poll/fiber 调度器。
 *
 * **所有** `On*` 回调都在「驱动 RunOnce 的那个线程」内同步执行：
 * 内置线程模式下就是接收线程，外部驱动模式下就是调用 RunOnce 的线程。
 * 回调里可以安全地调用 `Send`/`Broadcast`（底层是递归锁），但：
 *
 *   - 回调里**不要**调用 `Stop()`：会等待接收线程退出（自 join 是未定义
 *     行为）。`Stop()` 检测到同线程时只置停止标志、立即返回，真正的清理
 *     由接收线程退出前补做，因此不会崩溃，但 `Stop()` 返回时资源可能尚未
 *     全部释放。需要确定性关闭时从其他线程调用 `Stop()`。
 *   - 回调里不要销毁 `LinkServer` 自身。
 *   - `Send`/`Broadcast` 可从任意线程并发调用；peer 集合、统计与回调副本
 *     都有独立互斥量保护。
 *
 * ## 背压
 *
 * 可靠通道有发送窗口（256 条）与流控窗口（默认 1 MiB）。`Send` **不做
 * 内部无界排队**：窗口满时立即返回 `LinkSendResult::kWouldBlock`，由调用方
 * 决定等待/丢弃/降级。这是刻意设计——无界发送队列会把「对端慢」变成
 * 「本端 OOM」，而可靠通道本身要求同一通道上的消息保序，内部重试队列还得
 * 负责重排，复杂度远高于收益。需要「可靠 + 有界队列」时，应用可以自己用
 * 有界环形/`Channel<>` 包一层，把 `kWouldBlock` 作为生产端背压信号。
 *
 * 不可靠发送不排队：直接交给 UDP，`kOk` 只表示已写入 socket。
 *
 * ## 生命周期
 *
 *   LinkServer server;
 *   server.OnPeerConnected(...);          // 回调可随时替换
 *   server.OnStream(...);
 *   LinkServerConfig config;              // hub 超时/max_peers/PSK/reliable...
 *   if (!server.Listen("0.0.0.0", 9000, config, &error)) { ... }
 *   server.Start();                       // 起接收线程
 *   ...
 *   server.Send(peer, 1, data, size, LinkReliability::kReliable);
 *   server.Broadcast(2, frame, frame_bytes, LinkReliability::kUnreliable);
 *   ...
 *   server.Stop();                        // 停线程、断开所有 peer、关 socket
 *
 * 状态机：Idle --Listen()--> Listening --Start()--> Running
 *          --Stop()--> Stopped。`Stopped` 后 socket 已关闭，再次使用前必须
 *          重新 `Listen()`（换端口/换配置也走这条路）。`Stop()` 幂等，
 *          析构函数自动 `Stop()`。
 *
 * `Stop()` 会对每个仍连接的 peer 触发一次 `OnPeerDisconnected`（reason 为
 * `PeerHub` 的 "removed"），保证应用有机会清理 per-peer 资源——这比
 * 静默丢表更不容易泄漏。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/peer_hub.hpp"

namespace go2cpp::bento {

/// @brief 发送可靠性（STREAM / DATAGRAM 二选一）。
enum class LinkReliability : std::uint8_t {
    kReliable = 0,    ///< STREAM：有序、可靠、自动分片与重传。
    kUnreliable = 1,  ///< DATAGRAM：不重传，可能丢失/乱序；单条 <= kMaxFramePayload。
};

/// @brief 可靠性名称（日志/测试用）。
const char* LinkReliabilityName(LinkReliability reliability) noexcept;

/// @brief 一次单播/广播发送的结果。
/// @details 刻意把「暂时不可发」与「永久失败」分开：调用方对 `kWouldBlock`
///          重试，对 `kInvalid`/`kFailed` 放弃或告警，对 `kNotConnected`
///          判断是否要等重连。
enum class LinkSendResult : std::uint8_t {
    kOk = 0,           ///< 已交给通道：可靠=已入队，不可靠=已写入 socket。
    kWouldBlock = 1,   ///< 可靠通道发送窗口/流控窗口暂满，稍后重试即可。
    kNotConnected = 2, ///< peer 不在已连接集合中，或服务未运行（未 Listen/Start）。
    kInvalid = 3,      ///< 空指针、0 字节、超长或未知的 reliability。
    kFailed = 4,       ///< 底层通道拒绝（peer 刚断开、socket 写失败等）。
};

/// @brief 发送结果名（日志/测试用）。
const char* LinkSendResultName(LinkSendResult result) noexcept;

/// @brief peer 标识，复用 `PeerHub` 的 peer_id（本地句柄，不上海缆）。
using LinkPeerId = std::uint32_t;

/// @brief LinkServer 配置。
struct LinkServerConfig {
    /// 底层 Hub 配置：绑定地址/端口由 `Listen` 参数覆盖，其余（超时、心跳、
    /// max_peers、PSK、可靠通道参数、日志钩子）原样透传。
    PeerHubConfig hub;
    /// true（默认）= `Start()` 起内置接收线程；false = 由调用方驱动 `RunOnce`。
    bool start_receiver_thread = true;
    /// 内置接收线程每次 `RunOnce` 的 poll 超时（毫秒）。越小延迟越低、空转
    /// 越多；默认 5ms。会被夹在 [1, 1000]。
    std::uint32_t poll_timeout_ms = 5;
};

/// @brief LinkServer 自身计数器（底层链路统计请用 `hub_stats()`）。
struct LinkServerStats {
    std::uint64_t active_peers = 0;               ///< 当前已连接 peer 数。
    std::uint64_t peers_accepted = 0;             ///< 累计连接成功的 peer 数。
    std::uint64_t peers_disconnected = 0;         ///< 累计断开回调数。
    std::uint64_t stream_messages_received = 0;   ///< 收到并交付给应用的可靠消息数。
    std::uint64_t datagram_messages_received = 0; ///< 收到并交付给应用的不可靠消息数。
    std::uint64_t sends_ok = 0;                   ///< 返回 kOk 的单播数。
    std::uint64_t sends_would_block = 0;          ///< 返回 kWouldBlock 的单播数（背压）。
    std::uint64_t sends_not_connected = 0;        ///< 返回 kNotConnected 的单播数。
    std::uint64_t sends_invalid = 0;              ///< 返回 kInvalid 的单播数。
    std::uint64_t sends_failed = 0;               ///< 返回 kFailed 的单播数。
    std::uint64_t broadcasts = 0;                 ///< Broadcast 调用次数（含 0 peer）。
    std::uint64_t broadcast_deliveries = 0;       ///< Broadcast 成功投递的 peer 次数。
    std::uint64_t callback_exceptions = 0;        ///< 用户回调抛出异常的次数（已隔离）。
};

/// @brief 基于 `PeerHub` 的 Bento UDP 服务端。
///
/// @note 同一实例的 `Listen`/`Start`/`Stop` 需要串行调用（通常只在启动/关闭
///       阶段）；`Send`/`Broadcast`/统计/查询可在运行期任意线程并发调用。
class LinkServer final {
   public:
    /// @brief peer 标识（等价于 `LinkPeerId`）。
    using PeerId = LinkPeerId;

    /// @brief 收到一条应用消息。`data` 只在回调期间有效，需要保留请拷贝。
    using MessageHandler = std::function<void(PeerId peer_id, std::uint8_t channel,
                                              const std::uint8_t* data, std::size_t bytes)>;
    /// @brief peer 握手完成。
    using ConnectHandler = std::function<void(PeerId peer_id)>;
    /// @brief peer 断开（超时/主动移除/服务停止）。`reason` 为静态字符串。
    using DisconnectHandler = std::function<void(PeerId peer_id, const char* reason)>;

    LinkServer();
    ~LinkServer();

    LinkServer(const LinkServer&) = delete;
    LinkServer& operator=(const LinkServer&) = delete;

    // ---- 生命周期 ----

    /// @brief 绑定 UDP 地址并准备接收（不启动接收线程）。
    /// @param address IPv4/IPv6 字面量或主机名；"0.0.0.0"/"::" 为通配。
    /// @param port 0 = 内核分配（测试常用），实际端口用 `local_port()` 查询。
    /// @param config 配置；`config.hub.bind_address/bind_port` 会被本调用覆盖。
    /// @param error 失败原因（可为 nullptr）。
    /// @note 已处于监听状态时再次调用会失败，必须先 `Stop()`。这样「重复
    ///       Listen」不会被误认为成功而悄悄换端口。
    bool Listen(const std::string& address, std::uint16_t port,
                const LinkServerConfig& config = LinkServerConfig{},
                std::string* error = nullptr);

    /// @brief 以 "host:port" / "[v6]:port" / "host" 形式绑定。
    /// @details 没有端口（或端口为 0）时由内核分配；IPv6 字面量必须加 `[]`。
    ///          解析失败返回 false 并给出原因。
    bool Listen(const std::string& endpoint, const LinkServerConfig& config = LinkServerConfig{},
                std::string* error = nullptr);

    /// @brief 启动：内置线程模式下起接收线程，外部驱动模式下只置运行状态。
    /// @return 未成功 Listen、或 socket 已关闭时返回 false；已在运行返回 true。
    bool Start();

    /// @brief 停止：停接收线程（join）、断开所有 peer、关闭 socket。幂等。
    /// @note 从回调线程调用时不会自 join：只置停止标志并立即返回，清理由
    ///       接收线程退出前完成。
    void Stop();

    /// @brief 是否处于运行状态（`Start()` 成功且尚未 `Stop()`）。
    bool running() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }

    /// @brief 底层 socket 是否打开（`Listen` 成功且未 `Stop`）。
    /// @note 读的是本层缓存的原子状态，不会与 `Stop()` 的 socket 关闭竞争。
    bool is_open() const noexcept;

    /// @brief 实际绑定端口；未监听时返回 0。
    std::uint16_t local_port() const noexcept;

    /// @brief 最近一次 `Listen` 使用的配置（未 Listen 时返回默认值）。
    const LinkServerConfig& config() const noexcept {
        return m_config;
    }

    // ---- 驱动（外部驱动模式） ----

    /// @brief 处理一轮收发与定时器，返回本轮处理的帧数。
    /// @note 只在**外部驱动模式**下由单一线程调用；内置线程模式下会与接收
    ///       线程竞争（底层有锁，不会崩，但没有意义）。
    int RunOnce(int timeout_ms);

    /// @brief 循环驱动指定时长，返回处理帧数。
    int RunFor(int duration_ms);

    // ---- 发送 ----

    /// @brief 向指定 peer 发送一条消息。
    /// @param peer `OnPeerConnected` 给出的 peer_id。
    /// @param channel 应用层通道号（0..255）；STREAM/DATAGRAM 各自独立编号。
    /// @param data 载荷；可靠最大 `kMaxStreamMessageBytes`，不可靠最大
    ///        `kMaxFramePayload`。回调期间外由调用方保证生命周期。
    /// @param reliability 可靠 / 不可靠。
    /// @return 见 `LinkSendResult`；`kWouldBlock` 表示调用方应稍后重试同一条
    ///         消息（可靠通道保持顺序，重试不会破坏顺序）。
    LinkSendResult Send(PeerId peer, std::uint8_t channel, const void* data, std::size_t bytes,
                        LinkReliability reliability = LinkReliability::kReliable);

    /// @brief 向当前所有已连接 peer 发送同一条消息。
    /// @return 发送结果为 kOk 的 peer 数（0 表示无 peer 或全部背压/失败）。
    /// @note 单个 peer 失败不会中断广播；逐 peer 的结果计入
    ///       `LinkServerStats::sends_*`，另计 `broadcasts`/`broadcast_deliveries`。
    std::size_t Broadcast(std::uint8_t channel, const void* data, std::size_t bytes,
                          LinkReliability reliability = LinkReliability::kReliable);

    // ---- peer 查询 ----

    /// @brief 当前已连接 peer 的 id（升序）。
    std::vector<PeerId> Peers() const;

    /// @brief 该 peer 是否处于已连接集合中。
    bool HasPeer(PeerId peer) const;

    /// @brief 已连接 peer 数。
    std::size_t peer_count() const;

    /// @brief 转发 `PeerHub::peer_state`；peer 不存在时返回 `PeerState::kClosed`。
    PeerState peer_state(PeerId peer) const;

    // ---- 回调（可在运行中替换，下一个事件生效） ----

    void OnPeerConnected(ConnectHandler handler);
    void OnPeerDisconnected(DisconnectHandler handler);
    void OnStream(MessageHandler handler);
    void OnDatagram(MessageHandler handler);

    // ---- 统计 ----

    /// @brief LinkServer 层计数快照（线程安全）。
    LinkServerStats Stats() const;

    /// @brief 底层 Hub 计数快照（线程安全；含帧/字节/超时/握手等）。
    PeerHubStats hub_stats() const;

   private:
    /// @brief 内置接收线程主循环。
    void ReceiverLoop();
    /// @brief 幂等清理：对每个已连接 peer 触发断开回调并关闭 Hub。
    void Teardown();
    /// @brief 统一 Listen 实现（endpoint 解析后调用）。
    bool ListenImpl(const std::string& address, std::uint16_t port,
                    const LinkServerConfig& config, std::string* error);

    void HandlePeerConnected(std::uint32_t peer_id);
    void HandlePeerDisconnected(std::uint32_t peer_id, const char* reason);
    void HandleStream(std::uint32_t peer_id, std::uint8_t channel, const std::uint8_t* data,
                      std::size_t bytes);
    void HandleDatagram(std::uint32_t peer_id, std::uint8_t channel, const std::uint8_t* data,
                        std::size_t bytes);

    bool PeerKnown(std::uint32_t peer_id) const;
    std::vector<std::uint32_t> ConnectedSnapshot() const;
    /// @brief 取 Hub 的强引用快照；`Listen` 重建/`Stop` 不会让调用方悬空。
    std::shared_ptr<PeerHub> HubPointer() const;
    /// @brief 记录一次单播结果；调用方**不得**持 `m_state_mutex`。
    void RecordSend(LinkSendResult result);
    /// @brief 用户回调异常隔离；返回 false 表示抛了异常（调用方计数）。
    bool InvokeSafely(const ConnectHandler& handler, std::uint32_t peer_id) const noexcept;
    bool InvokeSafely(const DisconnectHandler& handler, std::uint32_t peer_id,
                      const char* reason) const noexcept;
    bool InvokeSafely(const MessageHandler& handler, std::uint32_t peer_id, std::uint8_t channel,
                      const std::uint8_t* data, std::size_t bytes) const noexcept;
    void IncrementCallbackExceptions();

    LinkServerConfig m_config;
    /// 底层多 peer 链路；`Listen` 成功后才非空，`Stop` 后保留（需重新 Listen）。
    /// 用 shared_ptr 让并发的 Send 在 `Listen` 重建时仍持有旧对象，避免 UAF。
    std::shared_ptr<PeerHub> m_hub;
    mutable std::mutex m_hub_mutex;  ///< 保护 m_hub 指针本身（不含其内部状态）。

    std::thread m_receiver;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop_requested{false};
    /// Teardown 只允许执行一次（外部 Stop 与接收线程退出可能竞争）。
    std::atomic<bool> m_teardown_started{false};
    /// socket 是否打开（避免与 Stop 的 Close 竞争读 fd）。
    std::atomic<bool> m_open{false};
    /// 实际绑定端口缓存（`Listen` 写入；避免 Stop 期间竞争读 socket 成员）。
    std::atomic<std::uint16_t> m_local_port{0};

    /// 保护 `m_connected` 与 `m_stats`。临界区内不得调用 Hub 或用户回调。
    mutable std::mutex m_state_mutex;
    std::set<std::uint32_t> m_connected;
    LinkServerStats m_stats;

    /// 保护用户回调副本；复制到局部后再调用，避免持锁执行用户代码。
    mutable std::mutex m_handler_mutex;
    ConnectHandler m_on_connected;
    DisconnectHandler m_on_disconnected;
    MessageHandler m_on_stream;
    MessageHandler m_on_datagram;
};

}  // namespace go2cpp::bento
