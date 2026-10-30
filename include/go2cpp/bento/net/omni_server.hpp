#pragma once

/**
 * @file omni_server.hpp
 * @brief OmniNetServer M0 骨架：配置契约、生命周期、连接表与 Memory 回环。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details 设计稿见 `docs/research/omni-net-design.md`（§2 与 §5.1）。本文件是
 *          **M0 阶段**的可编译落地：只做骨架，不承诺任何真实网络能力。
 *
 *          - 只接受 `transport == kMemory` 且 `protocols` 恰为 `{kRaw}`；
 *            其它组合在 `Create()` 阶段就被拒绝，不留下运行期
 *            "半支持"状态（设计 P3/P4）。
 *          - 数据面只有两条类型化通道：`Inbound()`（服务器 → 应用）与
 *            `Outbound()`（应用 → 服务器），元素类型为 `RawFrame`，底层复用
 *            `go2cpp::Channel<T>`（有界、可关闭、`TrySend`
 *            不阻塞驱动者）。
 *          - "连接"由调用方把一条**已建立**的 `net::ITransport` 交进来
 *            （`AttachConnection()` / `ConnectMemory()`）。M0 用
 *            `MemoryTransport::CreatePair()` 做回环，因此没有 socket、没有
 *            accept 线程、没有 Fiber——它们分别属于 M1/M2/M4。
 *          - 推进只有 `PollOnce()`：调用方线程驱动，M0 以 1ms 粒度轮询内存
 *            端点。M0 不做分帧，**一帧 = 一次 `Read()` 返回的字节数**；
 *            真正的 `FramingSpec` 在 M3 接入。
 *
 * ## M0 明确不做
 *
 * - TCP/UDP/TLS 端点、bind/listen；`Port()` 恒为 0；
 * - 协议嗅探与 HTTP/SSE/WS/Link 数据面（`AsLinkServer()` 只是占位视图，
 *   恒 `valid() == false`，`kLink` 配置由 `Create()` 以 `kUnsupported` 拒绝）；
 * - Fiber/线程池执行者、等待对端排空的优雅关闭、连接迁移、写超时；
 * - 重启：`Stop()` 之后实例进入终态，需要重新 `Create()`。
 *
 * ## 驱动与线程模型
 *
 * `Start` / `AttachConnection` / `PollOnce` / `Stop` 必须由**同一个驱动者**
 * 串行调用（设计 §4.7 的单驱动者不变量）。通道本身线程安全，业务可以在其它
 * 线程消费/生产；M0 不支持两个线程同时 `PollOnce`（会返回 `kReentrant`）。
 *
 * ## 所有权与生命周期
 *
 * `NetServer` 由 `Create()` 以 `std::unique_ptr` 交付；但 `AsRawServer()` /
 * `AsLinkServer()` 返回的视图与 `RawConnection` 句柄内部持有共享状态
 * （`shared_ptr`），因此**可以安全地比 `NetServer` 活得更久**：服务器析构会
 * 关闭全部通道与传输，之后视图只能读最终状态（`close_reason()`、已关闭的
 * 通道），不能再驱动 I/O。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/transport.hpp"
#include "go2cpp/concurrency/channel.hpp"

namespace go2cpp::bento::omni {

namespace detail {
struct ConnectionState;
struct OmniServerImpl;
}  // namespace detail

/**
 * @brief 调用方看到的 API 语义（设计 §2.2）。
 *
 * @details 只描述视图语义，不描述 fd：base 的 socket 恒为非阻塞，`kBlocking`
 *          指的是"视图的 `Recv/Send` 会等到有结果"，而不是去掉 `O_NONBLOCK`。
 *          M0 没有视图级的阻塞收发，本字段只参与合法性与未来兼容。
 */
enum class IoMode : std::uint8_t {
    kBlocking = 0,     ///< 阻塞语义（M0 未实现视图，配置仍合法）。
    kNonBlocking = 1,  ///< 非阻塞 + 轮询语义。
};

/**
 * @brief 传输种类（设计 §2.2）。
 *
 * @details M0 只实现 `kMemory`；其余枚举保留，保证配置矩阵与错误码稳定，
 *          由后续阶段逐个落地。
 */
enum class TransportKind : std::uint8_t {
    kTcp = 0,   ///< 流式（M2）。
    kUdp = 1,   ///< 数据报（M1）。
    kTls = 2,   ///< 流式 + TLS（M4；M0 返回 `kUnsupported`）。
    kMemory = 3, ///< 无 fd 的成对内存端点（M0 唯一实现）。
};

/// 协议种类（设计 §2.2）。M0 只实现 `kRaw`。
enum class ProtocolKind : std::uint8_t {
    kHttp = 0,  ///< HTTP/1.1（M2）。
    kSse = 1,   ///< SSE（M2）。
    kWs = 2,    ///< WebSocket（M3）。
    kLink = 3,  ///< Bento 帧协议（M1）。
    kRaw = 4,   ///< 不解释协议，按读块交付（M0 唯一实现）。
};

/// 并发模型（设计 §2.2）。M0 只有 `kPoll` 行为真实存在。
enum class ConcurrencyKind : std::uint8_t {
    kGmp = 0,     ///< io::IOManager Fiber（M1+）。
    kThread = 1,  ///< 固定线程池（M1+）。
    kPoll = 2,    ///< 调用方线程驱动，M0 唯一可 `PollOnce` 的模式。
};

/// 服务端操作错误码（设计 §2.3）。连接级错误不走这里，见 `CloseReason`。
enum class OmniError : std::uint8_t {
    kOk = 0,
    kInvalidConfig,           ///< 字段为空/越界/重复协议。
    kUnsupportedCombination,  ///< transport×protocol 或 mode×concurrency 非法。
    kUnsupported,             ///< 阶段未实现（非 Memory 传输、非 Raw 协议、TLS）。
    kBindFailed,              ///< 地址占用/权限（M0 不会产生）。
    kAlreadyStarted,          ///< `Start()` 重复调用。
    kNotRunning,              ///< 未 Start 就 `PollOnce()`。
    kNotPollMode,             ///< 非 `kPoll` 模式调用 `PollOnce()`。
    kReentrant,               ///< `PollOnce()` 重入。
    kProtocolMismatch,        ///< 快速路径下首包不符合唯一协议（M1+）。
    kSniffTimeout,            ///< 嗅探超时（M3）。
    kSniffTooLarge,           ///< 嗅探缓冲超限（M3）。
    kBackpressure,            ///< 发件通道或内部队列持续满。
    kLimitExceeded,           ///< 超硬上限（消息大小/连接数/预算）。
    kTimeout,                 ///< 操作超时。
    kClosed,                  ///< 连接/服务器已关闭。
    kInternal,                ///< 不应发生的内部错误。
};

/// 错误码的可读名称，用于日志与测试失败信息；未知值返回 "Unknown"。
const char* OmniErrorName(OmniError error) noexcept;

/**
 * @brief 硬上限集合（设计 §2.2 的 `Limits`）。
 *
 * @details 全部是硬上限：超限动作必须是"可观测的拒绝"（断开/拒绝/计数），
 *          不允许静默截断。M0 实际生效的是连接数、两条通道容量、
 *          `max_message_bytes` 与预算关系校验；其它字段先通过关系校验，
 *          等对应阶段实现后再生效。
 */
struct Limits {
    std::size_t max_connections{1024};           ///< 总连接上限。
    std::size_t max_connections_per_peer{64};    ///< 单远端 IP 上限（M2 启用）。
    std::size_t inbound_capacity{256};           ///< 每连接收件通道容量（条数）。
    std::size_t outbound_capacity{256};          ///< 每连接发件通道容量（条数）。
    std::size_t max_message_bytes{1U << 20};     ///< 单条业务消息上限（1 MiB）。
    std::size_t read_buffer_bytes{64U << 10};    ///< 每连接读缓冲（M2 启用）。
    std::size_t per_connection_budget_bytes{2U << 20};  ///< 每连接内存预算。
    std::size_t max_accept_per_tick{64};         ///< 单轮 accept 上限。
    std::size_t max_requests_per_connection{100};  ///< HTTP keep-alive 上限（M2）。
    std::size_t http_max_header_bytes{64U << 10};  ///< HTTP 头上限（M2）。
    std::uint32_t worker_threads{0};             ///< 0 = hardware_concurrency。
    std::chrono::milliseconds accept_timeout{1000};
    std::chrono::milliseconds idle_timeout{60000};   ///< 0 = 不超时。
    std::chrono::milliseconds write_timeout{30000};
    std::chrono::milliseconds close_timeout{5000};
};

/**
 * @brief 服务器配置（设计 §2.2 的 M0 子集）。
 *
 * @details 只保留 M0 用得到的五个字段：`mode`、`transport`、`protocols`、
 *          `concurrency`、`limits`。`bind_address` / `sniff` / `tls` /
 *          `sse_path` 等在对应阶段随端点一起引入，避免 M0 出现"配置了但
 *          无人读"的字段。
 *
 * @note 默认值来自设计稿（TCP + HTTP），它在 M0 会返回 `kUnsupported`；
 *       这是有意的——默认配置直到 M2 才真正可用。
 */
struct OmniNetConfig {
    IoMode mode{IoMode::kBlocking};
    TransportKind transport{TransportKind::kTcp};
    std::vector<ProtocolKind> protocols{ProtocolKind::kHttp};
    ConcurrencyKind concurrency{ConcurrencyKind::kGmp};
    Limits limits;
};

/// 连接代号：进程内单调递增、永不复用；`value == 0` 表示无效。
struct ConnectionId {
    std::uint64_t value{0};

    bool valid() const noexcept { return value != 0; }

    friend bool operator==(ConnectionId lhs, ConnectionId rhs) noexcept {
        return lhs.value == rhs.value;
    }
    friend bool operator!=(ConnectionId lhs, ConnectionId rhs) noexcept {
        return !(lhs == rhs);
    }
};

/// 连接关闭原因（设计 §2.3）。连接关闭后仍可从句柄读取终值。
enum class CloseReason : std::uint8_t {
    kNone = 0,      ///< 仍打开。
    kPeerClosed,    ///< 对端关闭（EOF/RST）。
    kLocalClose,    ///< 应用调用 `Close()`。
    kIdleTimeout,   ///< 空闲超时（M2+）。
    kWriteTimeout,  ///< 写超时（M2+）。
    kBackpressure,  ///< 通道持续满，按策略断开。
    kProtocolError, ///< 协议/传输错误。
    kSniffRejected, ///< 嗅探拒绝（M3）。
    kLimitExceeded, ///< 超硬上限。
    kShutdown,      ///< 服务器 `Stop()`。
};

/// 关闭策略：优雅（半关闭、尽量排空） vs 立即中止。
enum class CloseMode : std::uint8_t {
    kGraceful = 0,
    kAbort = 1,
};

/// 与设计稿一致：服务器级关闭策略与连接级共用同一枚举。
using ShutdownPolicy = CloseMode;

/**
 * @brief Raw 连接的一帧。
 *
 * @details M0 没有 Framer：一帧就是一次 `Read()` 成功返回的字节数，因此
 *          **不保证与对端一次 `Write()` 的边界一致**（流式传输会粘包）。
 *          需要消息边界时，由上层协议或 M3 的 `FramingSpec` 负责。
 */
struct RawFrame {
    std::vector<std::uint8_t> bytes;
};

/// 单连接统计（设计 §2.8），字段与 base/Bento 现有计数器同名同义。
struct ConnectionStats {
    std::uint64_t bytes_in{0};
    std::uint64_t bytes_out{0};
    std::uint64_t messages_in{0};
    std::uint64_t messages_out{0};
    std::uint64_t dropped{0};
    std::uint64_t backpressure_events{0};
    std::uint64_t errors{0};
    int last_system_error{0};  ///< errno 风格；0 表示成功。
};

/// 服务器级聚合统计（设计 §2.8）。M0 只填连接/流量/raw 相关字段。
struct NetStats {
    // 连接
    std::uint64_t accepted{0};
    std::uint64_t active{0};
    std::uint64_t closed_graceful{0};
    std::uint64_t closed_abort{0};
    std::uint64_t rejected_limit{0};
    std::uint64_t rejected_per_peer{0};
    std::uint64_t sniff_rejected{0};
    // 流量
    std::uint64_t bytes_in{0};
    std::uint64_t bytes_out{0};
    std::uint64_t messages_in{0};
    std::uint64_t messages_out{0};
    std::uint64_t dropped{0};
    std::uint64_t backpressure_events{0};
    // 协议
    std::uint64_t http_requests{0};
    std::uint64_t http_keepalive_reuse{0};
    std::uint64_t sse_events{0};
    std::uint64_t ws_messages{0};
    std::uint64_t raw_frames{0};
    // 时间
    std::uint64_t accept_wait_ms_total{0};
    std::uint64_t sniff_timeout_total{0};
};

/// `PollOnce()` 的单轮结果；`timeout == true` 且 `events == 0` 表示本轮无事发生。
struct PollResult {
    OmniError error{OmniError::kOk};
    std::size_t events{0};
    bool timeout{false};

    bool ok() const noexcept { return error == OmniError::kOk; }
};

/// 复用 concurrency 模块的通道类型与别名，避免在 omni 里再造一套通道。
using go2cpp::ChannelPtr;
using RawChannel = go2cpp::Channel<RawFrame>;
using RawChannelPtr = std::shared_ptr<RawChannel>;

/**
 * @brief 一条 Raw 连接的应用侧句柄（设计 §2.5 的 `RawConnection`）。
 *
 * @details 句柄很薄：只持有共享的连接状态。它可以拷贝、可以比服务器活得久；
 *          连接结束（对端关闭/服务器关闭/应用 `Close`）后，`Inbound()` 返回的
 *          通道会被关闭，`close_reason()` 给出终值。
 *
 *          通道契约（与设计 §2.6 的 C1–C3 对应）：
 *          - `Inbound()` 在连接生命周期内始终返回**同一个** `shared_ptr`；
 *          - 服务器是 Inbound 的唯一生产者，连接结束后关闭它；
 *          - 应用是 Outbound 的生产者，服务器关闭它表示"不再接受发送",
 *            关闭后 `TrySend` 返回 `kClosed`。
 */
class RawConnection final {
public:
    /// 构造一个无效句柄（`valid() == false`，所有方法安全空转）。
    RawConnection() = default;

    ConnectionId id() const noexcept;
    bool valid() const noexcept;

    /// 服务器 → 应用；无效句柄返回空指针。
    ChannelPtr<RawFrame> Inbound() const;

    /// 应用 → 服务器；无效句柄返回空指针。
    ChannelPtr<RawFrame> Outbound() const;

    /// 关闭连接（幂等）。`kAbort` 立即关闭传输，`kGraceful` 只半关闭写方向。
    void Close(CloseMode mode = CloseMode::kGraceful) noexcept;

    /// 关闭原因终值；仍打开时为 `kNone`。
    CloseReason close_reason() const noexcept;

    /// 连接级统计快照。
    ConnectionStats stats() const;

private:
    friend class RawServer;
    friend struct detail::OmniServerImpl;

    explicit RawConnection(std::shared_ptr<detail::ConnectionState> state) noexcept;

    std::shared_ptr<detail::ConnectionState> m_state;
};

/**
 * @brief RawServer 视图（设计 §2.5）。
 *
 * @details `valid() == true` 表示配置里启用了 `kRaw`；否则所有方法安全空转：
 *          `Accept()` 返回一条**已关闭**的通道，`Connection()` 返回无效句柄。
 *          视图不拥有服务器，但持有共享状态，可在服务器析构后安全读取。
 */
class RawServer final {
public:
    RawServer() = default;

    bool valid() const noexcept;

    /// 恒为 `kRaw`。
    ProtocolKind protocol() const noexcept;

    /// 新连接通道：每接入一条 Raw 连接推送一个 `RawConnection`。
    ///
    /// @note 通道有界（容量 = `limits.inbound_capacity`）。应用必须持续消费，
    ///       否则接入会按 `rejected_limit` 被拒绝（M0 的可观测拒绝）。
    ///       服务器 `Stop()` 时关闭；单个连接结束不会关闭它。
    ChannelPtr<RawConnection> Accept() const;

    /// 按 id 取连接句柄；id 不存在时返回无效句柄。
    RawConnection Connection(ConnectionId id) const;

    /// 当前存活连接的句柄快照（创建顺序不保证）。
    std::vector<RawConnection> Connections() const;

private:
    friend class NetServer;

    RawServer(std::shared_ptr<detail::OmniServerImpl> impl, bool valid) noexcept;

    std::shared_ptr<detail::OmniServerImpl> m_impl;
    bool m_valid{false};
};

/**
 * @brief LinkServer 视图占位（M1 才实现）。
 *
 * @details M0 的 `Create()` 不接受 `kLink`（返回 `kUnsupported`），因此本视图
 *          在 M0 恒为 `valid() == false`；保留类型是为了让上层代码在 M1 落地时
 *          只多一次 `valid()` 判断，而不是改 API 形状。M0 **没有** `Session()`、
 *          `Outbound()` 等数据面方法——不提供会返回空/哨兵的假接口。
 */
class LinkServer final {
public:
    LinkServer() = default;

    bool valid() const noexcept;

    /// 恒为 `kLink`。
    ProtocolKind protocol() const noexcept;

private:
    friend class NetServer;

    LinkServer(std::shared_ptr<detail::OmniServerImpl> impl, bool valid) noexcept;

    std::shared_ptr<detail::OmniServerImpl> m_impl;
    bool m_valid{false};
};

/**
 * @brief 组合式服务器门面（设计 §2.4 的 M0 子集）。
 *
 * @details `Create()` 一次性完成配置校验与内部状态构造，不绑定端口、不建线程；
 *          `Start()` 只做状态切换；`Stop()` 幂等且是终态。M0 的唯一驱动入口是
 *          `PollOnce()`，它把 `Inbound` / `Outbound` 两条通道与内存传输对接。
 *
 * @warning `Create()` 返回 `unique_ptr`（设计稿写的是 `shared_ptr`，M0 按任务
 *          要求收窄所有权）。视图与连接句柄内部持有共享状态，因此仍然可以
 *          安全地晚于 `NetServer` 析构。
 */
class NetServer final {
public:
    /**
     * @brief 纯函数式配置校验（不碰 fd、不建线程）。
     *
     * @details 校验顺序固定（设计 §2.2 的 V1–V6 + M0 实现门），保证错误码稳定：
     *          V1 枚举范围 → V2 protocols 非空且无重复 → V3 transport×protocol →
     *          V4 mode×concurrency → V5 TLS 未实现 → V6 limits 关系 →
     *          M0 门（只允许 Memory + Raw）。
     *
     * @param config 待校验配置。
     * @param error  可选的中文诊断输出；失败时写入，成功时清空。
     * @return `kOk` 或具体错误码。
     */
    static OmniError ValidateConfig(const OmniNetConfig& config, std::string* error = nullptr);

    /**
     * @brief 校验配置并构造服务器；不绑定端口。
     * @param config 配置。
     * @param error  可选错误输出。
     * @return 成功时非空；非法组合返回 `nullptr`（并写 `error`）。
     */
    static std::unique_ptr<NetServer> Create(const OmniNetConfig& config,
                                             std::string* error = nullptr);

    ~NetServer();

    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;
    NetServer(NetServer&&) = delete;
    NetServer& operator=(NetServer&&) = delete;

    /**
     * @brief 启动服务器（M0：状态切换 + 通道准备，无监听/线程）。
     * @return 成功 `true`；重复 `Start()` 或 `Stop()` 之后返回 `false`。
     */
    bool Start(std::string* error = nullptr);

    /**
     * @brief 幂等关闭：停止接入，按策略关闭全部连接，最后置 `running=false`。
     *
     * @details M0 的 `kGraceful` 只做一轮**非阻塞**出站排空，然后半关闭写方向；
     *          不等待对端（真正的排空与 `close_timeout` 是 M4 硬化）。
     *          `Stop()` 之后实例进入终态，`Start()` 会失败。
     */
    void Stop(ShutdownPolicy policy = ShutdownPolicy::kGraceful) noexcept;

    bool running() const noexcept;

    /// M0 恒为 0：Memory 传输不绑定端口。
    std::uint16_t Port() const noexcept;

    /// 当前存活连接数。
    std::size_t Connections() const noexcept;

    /// 服务器级统计快照。
    NetStats Stats() const;

    const OmniNetConfig& config() const noexcept;

    /// Raw 视图；配置未启用 `kRaw` 时 `valid() == false`。
    RawServer AsRawServer();

    /// Link 视图占位；M0 恒 `valid() == false`（见 `LinkServer` 注释）。
    LinkServer AsLinkServer();

    /**
     * @brief M0 回环便捷入口：创建一对内存端点，服务器侧立即接入，
     *        返回客户端侧传输。
     *
     * @details 等价于 `MemoryTransport::CreatePair()` + `AttachConnection()`。
     *          返回的传输由调用方负责读写与关闭；关闭它会驱动服务器侧读到
     *          EOF（`kPeerClosed`）。失败时返回 `nullptr`（未 Start / 达上限 /
     *          Accept 通道满）。
     */
    std::unique_ptr<net::ITransport> ConnectMemory(std::string* error = nullptr);

    /**
     * @brief 接入一条已建立的传输（M0 的 Memory 回环注入点）。
     *
     * @details 真实端点（TCP/UDP）会在 M1/M2 内部调用同一入口。服务器在其上
     *          建立连接状态与 Inbound/Outbound 通道，并把 `RawConnection`
     *          推入 `AsRawServer().Accept()`。
     *
     * @return 新连接 id；未 Start、传输为空、达到 `max_connections`、或 Accept
     *         通道满时返回无效 id（计入 `rejected_limit`）。
     */
    ConnectionId AttachConnection(std::unique_ptr<net::ITransport> transport);

    /**
     * @brief 推进一步 I/O（M0 的唯一位移驱动）。
     *
     * @details 每轮对每条连接：先排空 Outbound（应用 → 传输），再读取传输
     *          （传输 → Inbound）。Inbound 满时**暂停读**并计
     *          `backpressure_events`，不丢帧。`timeout > 0` 时以 1ms 粒度
     *          轮询到有事件或超时（内存端点没有可等待的 fd）；`timeout == 0`
     *          表示只做一轮非阻塞检查。
     *
     * @return 错误码 / 本轮事件数 / 是否无事发生。
     */
    PollResult PollOnce(std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

private:
    explicit NetServer(std::shared_ptr<detail::OmniServerImpl> impl) noexcept;

    std::shared_ptr<detail::OmniServerImpl> m_impl;
};

}  // namespace go2cpp::bento::omni
