#pragma once

/**
 * @file omni_server.hpp
 * @brief OmniNetServer M1：配置契约 + 真实后端接入（UDP LinkServer / TCP
 *        HttpServer）+ 生命周期 + Raw Memory 回环。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details 设计稿见 `docs/research/omni-net-design.md`（§2 与 §5.2/§5.3）。
 *          M0 只有 Memory + Raw 骨架；M1 在不重写协议实现的前提下，把仓库里
 *          已经验证过的两个服务端接进统一入口：
 *
 *          - `transport == kMemory`：保持 M0 行为，`Raw` 数据面 + `PollOnce()`
 *            驱动，端口恒为 0（没有 socket）。
 *          - `transport == kUdp`：组合 `go2cpp::bento::LinkServer`（真实 UDP，
 *            1:N STREAM/DATAGRAM/广播），`AsLinkServer()` 返回可用的视图。
 *          - `transport == kTcp`：组合 `go2cpp::bento::HttpServer`（真实 TCP
 *            HTTP/SSE），`AsHttpServer()` 返回可用的视图。
 *
 *          M1 的 `Create()` 按 `config.transport` 选择后端；transport × protocol
 *          的非法组合、M1 未实现的协议（WS/Raw-over-TCP、Raw-over-UDP、TLS）
 *          与不支持的并发模型在 `Create()` 阶段就失败，不留下运行期"半支持"
 *          状态（设计 P3/P4）。
 *
 * ## 配置驱动的监听契约
 *
 * `config.bind` 描述真实监听端点（默认 `127.0.0.1:0`，0 表示内核分配）：
 *
 * - `NetServer::Start()` 会先按 `config.bind` 绑定，再启动后端线程；
 *   `Port()` 返回实际端口（绑定失败返回 0，`Start()` 返回 false 并写 error）。
 * - 视图的 `Listen()` 可在 `Start()` **之前**手动覆盖绑定地址/端口；此时
 *   `Start()` 复用已有监听、不再重复绑定（重复 `Listen()` 由后端拒绝）。
 * - `Stop()` 关闭监听并释放端口。M1 的 `Stop()` 仍是终态：同一实例不能再次
 *   `Start()`；换端口/重绑通过新实例完成（M0 的终态语义保持不变）。
 *
 * ## 并发契约（M1）
 *
 * | transport | 允许的 concurrency | 推进方式 |
 * |-----------|--------------------|----------|
 * | Memory    | 任意（M0 兼容）     | 调用方 `PollOnce()` |
 * | UDP       | `kGmp` / `kThread` | `LinkServer` 内置接收线程 |
 * | TCP       | `kGmp` / `kThread` | `HttpServer` accept + worker 线程 |
 *
 * UDP/TCP 的 `kPoll` 在 `Create()` 返回 `kUnsupportedCombination`：Poll 模式
 * 的驱动者是调用方，而两个后端各自持有线程；允许它只会得到"配置成功但
 * `PollOnce()` 不推进"的假象。`NonBlocking + Poll` 留给 M2 的 fd 事件循环。
 *
 * ## UDP 加密
 *
 * `go2cpp::bento::PeerHub` 默认 `require_encryption = true`，没有 PSK 时无法
 * 建立会话。M1 用 `config.link_psk` 表达：
 *
 * - 非空（必须 32 字节）：启用 PSK 认证加密（Hub 默认安全策略）；
 * - 空：显式允许明文，**仅限本机回环/测试**，不要用于生产。
 *
 * ## 线程模型与调用约束
 *
 * - `Start` / `Stop` / 视图 `Listen` 需要串行调用（通常只在启动/关闭阶段）。
 * - UDP 的 `Send` / `Broadcast` / 查询可从任意线程并发调用（后端保证）。
 * - TCP 的 `OnRequest` / `AsSse` 可在运行期从任意线程替换（后端有路由锁）；
 *   handler 在 worker 线程执行，同一 handler 可能被多个连接并发调用。
 * - `PollOnce()` 只对 Memory 有意义；UDP/TCP 调用返回 `kNotPollMode`。
 *
 * ## 所有权与生命周期
 *
 * `NetServer` 由 `Create()` 以 `std::unique_ptr` 交付；`AsRawServer()` /
 * `AsLinkServer()` / `AsHttpServer()` 返回的视图持有内部状态的共享指针，
 * 因此**可以安全地比 `NetServer` 活得更久**：服务器析构会停止后端、关闭
 * 通道与连接，之后视图只能读最终状态（`Port() == 0`、`Send()` 返回
 * `kNotConnected`、`peer_count() == 0`），不能再驱动 I/O。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/link_server.hpp"
#include "go2cpp/bento/net/transport.hpp"
#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/sse.hpp"

namespace go2cpp::bento {
/// 前置声明：Omni 的 HTTP 视图只转发到该后端，不需要把 http_server.hpp
/// 拖进所有包含 omni_server.hpp 的翻译单元（也避免 include 顺序陷阱）。
class HttpServer;
}  // namespace go2cpp::bento

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
 *          M1 没有视图级的阻塞收发，本字段只参与合法性与未来兼容。
 */
enum class IoMode : std::uint8_t {
    kBlocking = 0,     ///< 阻塞语义（M1 未实现视图，配置仍合法）。
    kNonBlocking = 1,  ///< 非阻塞 + 轮询语义。
};

/**
 * @brief 传输种类（设计 §2.2）。
 *
 * @details M1 实现 `kMemory` / `kUdp` / `kTcp`；`kTls` 在 M4 落地。
 */
enum class TransportKind : std::uint8_t {
    kTcp = 0,    ///< 流式：HTTP/SSE（M1）。
    kUdp = 1,    ///< 数据报：Link（M1）。
    kTls = 2,    ///< 流式 + TLS（M4；M1 返回 `kUnsupported`）。
    kMemory = 3, ///< 无 fd 的成对内存端点（M0 起）。
};

/// 协议种类（设计 §2.2）。M1 实现 kHttp/kSse（TCP）与 kLink（UDP）、kRaw（Memory）。
enum class ProtocolKind : std::uint8_t {
    kHttp = 0, ///< HTTP/1.1（M1，TCP）。
    kSse = 1,  ///< SSE（M1，TCP；与 HTTP 共用 HttpServer 后端）。
    kWs = 2,   ///< WebSocket（M3）。
    kLink = 3, ///< Bento 帧协议（M1，UDP）。
    kRaw = 4,  ///< 不解释协议，按读块交付（M0 起；M3 才支持 TCP/UDP 嗅探）。
};

/// 并发模型（设计 §2.2）。M1 的 UDP/TCP 后端自带线程；Memory 用 kPoll。
enum class ConcurrencyKind : std::uint8_t {
    kGmp = 0,    ///< io::IOManager Fiber（M1 映射为后端内置线程）。
    kThread = 1, ///< 固定线程池（M1 映射为后端内置线程）。
    kPoll = 2,   ///< 调用方线程驱动（M0 起；M1 仅 Memory 支持）。
};

/// 服务端操作错误码（设计 §2.3）。连接级错误不走这里，见 `CloseReason`。
enum class OmniError : std::uint8_t {
    kOk = 0,
    kInvalidConfig,           ///< 字段为空/越界/重复协议。
    kUnsupportedCombination,  ///< transport×protocol 或 mode×concurrency 非法。
    kUnsupported,             ///< 阶段未实现（TLS、WS、TCP/UDP 的 Raw）。
    kBindFailed,              ///< 地址占用/权限/地址无法解析。
    kAlreadyStarted,          ///< `Start()` 重复调用（保留值，Start 用 bool 表达）。
    kNotRunning,              ///< 未 Start 就 `PollOnce()`。
    kNotPollMode,             ///< 非 `kPoll` 模式调用 `PollOnce()`。
    kReentrant,               ///< `PollOnce()` 重入。
    kProtocolMismatch,        ///< 快速路径下首包不符合唯一协议（M2+）。
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
 *          不允许静默截断。M1 实际生效的映射：
 *          - Memory：连接数、两条通道容量、`max_message_bytes`；
 *          - UDP：`max_connections` → Hub `max_peers`，`idle_timeout` →
 *            `peer_timeout_ms`（0 时保留 Hub 默认 5s）；
 *          - TCP：`worker_threads`（0 → 4）、`max_connections` → 待处理队列
 *            上限、`max_requests_per_connection`、`http_max_header_bytes`、
 *            `max_message_bytes` → body 上限、`accept_timeout`。
 */
struct Limits {
    std::size_t max_connections{1024};           ///< 总连接上限。
    std::size_t max_connections_per_peer{64};    ///< 单远端 IP 上限（M2 启用）。
    std::size_t inbound_capacity{256};           ///< 每连接收件通道容量（条数）。
    std::size_t outbound_capacity{256};          ///< 每连接发件通道容量（条数）。
    std::size_t max_message_bytes{1U << 20};     ///< 单条业务消息/HTTP body 上限（1 MiB）。
    std::size_t read_buffer_bytes{64U << 10};    ///< 每连接读缓冲（M2 启用）。
    std::size_t per_connection_budget_bytes{2U << 20};  ///< 每连接内存预算。
    std::size_t max_accept_per_tick{64};         ///< 单轮 accept 上限（M2 启用）。
    std::size_t max_requests_per_connection{100};  ///< HTTP keep-alive 单连接请求上限。
    std::size_t http_max_header_bytes{64U << 10};  ///< HTTP 头上限。
    std::uint32_t worker_threads{0};             ///< 0 = 后端默认（HTTP 用 4）。
    std::chrono::milliseconds accept_timeout{1000};  ///< HTTP accept 轮询超时。
    std::chrono::milliseconds idle_timeout{60000};   ///< UDP peer 超时；0 = 用后端默认。
    std::chrono::milliseconds write_timeout{30000};  ///< 单次写上限（M4 硬化）。
    std::chrono::milliseconds close_timeout{5000};   ///< 优雅关闭排空上限（M4）。
};

/**
 * @brief 真实监听端点（设计 §2.2 的 `bind_address` + `bind_port`）。
 *
 * @details `address` 为 IPv4/IPv6 字面量或主机名；`port == 0` 表示内核分配，
 *          实际端口用 `NetServer::Port()` 查询。地址无法解析时 `Start()`
 *          失败并返回 error（`last_error() == kBindFailed`）。
 */
struct BindEndpoint {
    std::string address{"127.0.0.1"};  ///< 监听地址（空串在 Create 被拒）。
    std::uint16_t port{0};             ///< 监听端口；0 = 内核分配。
};

/**
 * @brief 服务器配置（设计 §2.2 的 M1 子集）。
 *
 * @details 默认值来自设计稿（TCP + HTTP + Gmp + `127.0.0.1:0`）。M1 起默认
 *          配置可以 `Create()` 并真实监听；M0 阶段它会被 `kUnsupported` 拒绝。
 *
 * @note `link_psk` 是 M1 为接入 Hub 默认安全策略而增加的唯一安全字段：
 *       非空必须 32 字节；为空表示显式允许明文（仅限本机/测试）。
 */
struct OmniNetConfig {
    IoMode mode{IoMode::kBlocking};
    TransportKind transport{TransportKind::kTcp};
    std::vector<ProtocolKind> protocols{ProtocolKind::kHttp};
    ConcurrencyKind concurrency{ConcurrencyKind::kGmp};
    BindEndpoint bind;                       ///< 真实监听端点。
    std::vector<std::uint8_t> link_psk;      ///< UDP PSK；空 = 明文（不安全）。
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
 * @details Memory 传输没有 Framer：一帧就是一次 `Read()` 成功返回的字节数，
 *          因此**不保证与对端一次 `Write()` 的边界一致**。需要消息边界时，
 *          由上层协议或 M3 的 `FramingSpec` 负责。
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

/**
 * @brief 服务器级聚合统计（设计 §2.8）。
 *
 * @details M1 的映射口径：
 *          - Memory：完整填充连接/流量/raw 字段（与 M0 一致）；
 *          - UDP：`accepted`/`active` 来自 LinkServer peer 计数，`bytes_*` 与
 *            `messages_*` 来自 Hub/LinkServer 计数，`errors` 含发送失败；
 *          - TCP：`http_requests`/`sse_events` 由 OmniNetServer 在 handler
 *            包装层计数，`active` 为 HttpServer 正在处理的连接数；HTTP
 *            累计连接数后端未暴露，`accepted` 保持 0（M2 补）。
 */
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
 *          - 应用是 Outbound 的生产者，服务器关闭它表示"不再接受发送"，
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
 * @details `valid() == true` 表示配置里启用了 `kRaw`（M1 中只有 Memory 传输）。
 *          否则所有方法安全空转：`Accept()` 返回一条**已关闭**的通道，
 *          `Connection()` 返回无效句柄。视图不拥有服务器，但持有共享状态，
 *          可在服务器析构后安全读取。
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
    ///       否则接入会按 `rejected_limit` 被拒绝。服务器 `Stop()` 时关闭；
    ///       单个连接结束不会关闭它。
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
 * @brief LinkServer 视图（设计 §2.5；M1 落地）。
 *
 * @details 真正的数据面在 `go2cpp::bento::LinkServer`（一个 UDP socket 接
 *          N 个 peer，STREAM 可靠 / DATAGRAM 不可靠）。本视图不复制协议，
 *          只把配置驱动的 `Listen`、发送与回调查询转发给后端：
 *
 *          - `NetServer::Start()` 已按 `config.bind` 完成绑定并启动接收线程；
 *          - 视图的 `Listen()` 用于在 `Start()` 之前覆盖绑定端点；
 *          - `Send` / `Broadcast` / `OnStream` / `OnDatagram` 与后端同义。
 *
 *          配置未启用 UDP Link 时 `valid() == false`，所有方法安全空转：
 *          `Send` 返回 `kNotConnected`、`Broadcast` 返回 0、查询返回空。
 */
class LinkServer final {
public:
    LinkServer() = default;

    bool valid() const noexcept;

    /// 恒为 `kLink`。
    ProtocolKind protocol() const noexcept;

    /**
     * @brief 在 `NetServer::Start()` 之前手动绑定 UDP 地址。
     *
     * @param address IPv4/IPv6 字面量或主机名。
     * @param port 0 = 内核分配。
     * @param error 失败原因（可为 nullptr）。
     * @return 成功 true；已启动、已监听或无效视图返回 false。
     * @note 成功后 `Start()` 复用该监听；重复 `Listen()` 不会悄悄换端口。
     */
    bool Listen(const std::string& address, std::uint16_t port, std::string* error = nullptr);

    /// 向指定 peer 单播；语义同 `go2cpp::bento::LinkServer::Send`。
    ::go2cpp::bento::LinkSendResult Send(
        ::go2cpp::bento::LinkPeerId peer, std::uint8_t channel, const void* data,
        std::size_t bytes,
        ::go2cpp::bento::LinkReliability reliability =
            ::go2cpp::bento::LinkReliability::kReliable);

    /// 向当前所有已连接 peer 广播；返回成功投递的 peer 数。
    std::size_t Broadcast(
        std::uint8_t channel, const void* data, std::size_t bytes,
        ::go2cpp::bento::LinkReliability reliability =
            ::go2cpp::bento::LinkReliability::kReliable);

    /// 当前已连接 peer 的 id（升序）。
    std::vector<::go2cpp::bento::LinkPeerId> Peers() const;

    /// 该 peer 是否已连接。
    bool HasPeer(::go2cpp::bento::LinkPeerId peer) const;

    /// 已连接 peer 数。
    std::size_t peer_count() const;

    /// 后端实际绑定端口；未监听为 0（与 `NetServer::Port()` 在 Start 后一致）。
    std::uint16_t local_port() const noexcept;

    // ---- 回调转发（可在运行中替换，下一个事件生效） ----

    void OnPeerConnected(::go2cpp::bento::LinkServer::ConnectHandler handler);
    void OnPeerDisconnected(::go2cpp::bento::LinkServer::DisconnectHandler handler);
    void OnStream(::go2cpp::bento::LinkServer::MessageHandler handler);
    void OnDatagram(::go2cpp::bento::LinkServer::MessageHandler handler);

    /// 真实后端指针；无效视图返回 nullptr。生命周期由 `NetServer` 保证。
    ::go2cpp::bento::LinkServer* backend() const noexcept;

private:
    friend class NetServer;

    LinkServer(std::shared_ptr<detail::OmniServerImpl> impl, bool valid) noexcept;

    std::shared_ptr<detail::OmniServerImpl> m_impl;
    bool m_valid{false};
};

/**
 * @brief HttpServer 视图（设计 §2.5；M1 落地）。
 *
 * @details 数据面是 `go2cpp::bento::HttpServer`（真实 TCP，GET/POST/keep-alive
 *          + SSE 推流）。本视图转发 `Listen` / `OnRequest` / `AsSse`：
 *
 *          - `NetServer::Start()` 已按 `config.bind` 绑定并启动 accept/worker；
 *          - 视图 `Listen()` 用于在 `Start()` 之前覆盖绑定端点；
 *          - `OnRequest` / `AsSse` 可在启动前或运行期注册（后端有路由锁）。
 *
 *          配置未启用 TCP HTTP/SSE 时 `valid() == false`，所有方法安全空转。
 */
class HttpServer final {
public:
    /// 普通请求处理器（签名与后端 `bento::HttpServer::RequestHandler` 一致）。
    using RequestHandler =
        std::function<::go2cpp::http::Response(const ::go2cpp::http::Request&)>;
    /// SSE 处理器（签名与后端 `bento::HttpServer::SseHandler` 一致）。
    using SseHandler =
        std::function<void(const ::go2cpp::http::Request&, ::go2cpp::sse::Stream&)>;

    HttpServer() = default;

    bool valid() const noexcept;

    /// 恒为 `kHttp`（SSE 与 HTTP 共用同一 TCP 后端，通过 `AsSse` 注册）。
    ProtocolKind protocol() const noexcept;

    /// 在 `Start()` 之前按 `"host:port"` 手动绑定（IPv6 字面量需加方括号）。
    bool Listen(const std::string& endpoint, std::string* error = nullptr);

    /// 在 `Start()` 之前按 host + port 手动绑定；port 0 = 内核分配。
    bool Listen(const std::string& host, std::uint16_t port, std::string* error = nullptr);

    /// 设置普通请求处理器；空函数对象表示取消（后端返回 404）。
    void OnRequest(RequestHandler handler);

    /// 注册/替换一个 SSE 路由（path 精确匹配）。
    void AsSse(std::string path, SseHandler handler);

    /// 实际监听端口；未启动/已停止为 0。
    std::uint16_t Port() const noexcept;

    /// 实际监听地址（`host:port`）；未 Listen 时为空。
    std::string LocalAddress() const;

    /// 当前正在被 worker 处理的连接数（不含 keep-alive 空闲连接）。
    std::size_t ActiveConnections() const noexcept;

    /// 真实后端指针；无效视图返回 nullptr。生命周期由 `NetServer` 保证。
    ::go2cpp::bento::HttpServer* backend() const noexcept;

private:
    friend class NetServer;

    HttpServer(std::shared_ptr<detail::OmniServerImpl> impl, bool valid) noexcept;

    std::shared_ptr<detail::OmniServerImpl> m_impl;
    bool m_valid{false};
};

/**
 * @brief 组合式服务器门面（设计 §2.4 的 M1 子集）。
 *
 * @details `Create()` 一次性完成配置校验与后端构造，不绑定端口、不建线程；
 *          `Start()` 按 `config.bind` 真实监听并启动后端；`Stop()` 幂等关闭
 *          并释放端口。M1 的驱动入口：
 *
 *          - Memory：`PollOnce()` + `ConnectMemory()`（M0 兼容）；
 *          - UDP：`AsLinkServer()` 的 `Send/Broadcast` + 回调；
 *          - TCP：`AsHttpServer()` 的 `OnRequest/AsSse`。
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
     * @details 校验顺序固定，保证错误码稳定：
     *          V1 枚举范围 → V2 protocols 非空且无重复 → V3 transport×protocol →
     *          V4 mode×concurrency → V5 TLS 未实现 → V6 limits 关系 →
     *          V7 M1 实现门（Memory×Raw、UDP×Link、TCP×HTTP/SSE）→
     *          V8 concurrency×transport（UDP/TCP 不接受 Poll）→
     *          V9 bind/PSK（地址非空、PSK 必须 32B）。
     *
     * @param config 待校验配置。
     * @param error  可选的中文诊断输出；失败时写入，成功时清空。
     * @return `kOk` 或具体错误码。
     */
    static OmniError ValidateConfig(const OmniNetConfig& config, std::string* error = nullptr);

    /**
     * @brief 校验配置并构造服务器与后端；不绑定端口、不建线程。
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
     * @brief 启动服务器：按 `config.bind` 绑定并启动真实后端。
     *
     * @details Memory：只做状态切换（M0 行为）；UDP：`LinkServer::Listen` +
     *          `Start`（内置接收线程）；TCP：`HttpServer::Listen` + `Start`
     *          （accept + worker 线程）。若视图已经手动 `Listen` 过，这里
     *          直接复用，不再重复绑定。
     *
     * @param error 失败原因（绑定失败/已启动/已 Stop）。
     * @return 成功 `true`；重复 `Start()`、`Stop()` 之后或绑定失败返回 `false`。
     */
    bool Start(std::string* error = nullptr);

    /**
     * @brief 幂等关闭：停止后端、关闭监听与全部连接，最后置 `running=false`。
     *
     * @details UDP/TCP 调用后端 `Stop()`（已包含 join 线程与关闭 socket）；
     *          Memory 保持 M0 的一轮非阻塞出站排空 + 半关闭写方向。
     *          `Stop()` 之后实例进入终态，`Start()` 会失败；端口已释放，
     *          可以用新实例重新绑定。
     */
    void Stop(ShutdownPolicy policy = ShutdownPolicy::kGraceful) noexcept;

    /// 是否处于运行状态（`Start()` 成功且尚未 `Stop()`）。
    bool IsRunning() const noexcept;

    /// M0 兼容别名：等价于 `IsRunning()`。
    bool running() const noexcept;

    /// 实际监听端口；Memory/未启动/已停止返回 0。
    std::uint16_t Port() const noexcept;

    /**
     * @brief 当前存活连接/peer 数。
     * @note Memory 为连接表大小；UDP 为已连接 peer 数；TCP 为 HttpServer 正在
     *       处理的连接数（keep-alive 空闲连接不计，后端未暴露该计数）。
     */
    std::size_t Connections() const noexcept;

    /// 服务器级统计快照（各 transport 的口径见 `NetStats` 注释）。
    NetStats Stats() const;

    const OmniNetConfig& config() const noexcept;

    /// 最近一次 `Start()` 的错误码；成功后为 `kOk`。
    OmniError last_error() const noexcept;

    /// Raw 视图；配置未启用 `kRaw`（M1：仅 Memory）时 `valid() == false`。
    RawServer AsRawServer();

    /// Link 视图；配置不是 UDP + Link 时 `valid() == false`。
    LinkServer AsLinkServer();

    /// HTTP 视图；配置不是 TCP + HTTP/SSE 时 `valid() == false`。
    HttpServer AsHttpServer();

    /**
     * @brief M0 兼容的 Memory 回环入口：创建一对内存端点，服务器侧立即接入，
     *        返回客户端侧传输。
     *
     * @details 仅 `transport == kMemory` 可用；其它 transport 返回 nullptr 并
     *          写 error。返回的传输由调用方负责读写与关闭。
     */
    std::unique_ptr<net::ITransport> ConnectMemory(std::string* error = nullptr);

    /**
     * @brief 接入一条已建立的传输（M0 的 Memory 回环注入点）。
     *
     * @details M1 只为 Memory 传输保留该入口；UDP/TCP 的连接由真实后端建立，
     *          不接受外部注入（返回无效 id）。
     *
     * @return 新连接 id；未 Start、传输为空、达到 `max_connections`、或 Accept
     *         通道满时返回无效 id（计入 `rejected_limit`）。
     */
    ConnectionId AttachConnection(std::unique_ptr<net::ITransport> transport);

    /**
     * @brief 推进一步 Memory I/O（M0 兼容）。
     *
     * @details 每轮对每条连接：先排空 Outbound（应用 → 传输），再读取传输
     *          （传输 → Inbound）。Inbound 满时**暂停读**并计
     *          `backpressure_events`，不丢帧。非 `kPoll` 模式返回 `kNotPollMode`。
     *
     * @return 错误码 / 本轮事件数 / 是否无事发生。
     */
    PollResult PollOnce(std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

private:
    explicit NetServer(std::shared_ptr<detail::OmniServerImpl> impl) noexcept;

    std::shared_ptr<detail::OmniServerImpl> m_impl;
};

}  // namespace go2cpp::bento::omni
