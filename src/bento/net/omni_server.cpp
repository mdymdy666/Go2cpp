/**
 * @file omni_server.cpp
 * @brief OmniNetServer M1 实现：配置校验、真实后端接入（LinkServer/HttpServer）、
 *        Memory 回环与 Raw 通道。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details 组合关系（与头文件注释一一对应）：
 *          - `transport == kMemory`：M0 的 MemoryTransport + Raw 通道 +
 *            `PollOnce()` 驱动，没有 socket/线程；
 *          - `transport == kUdp`：`OmniServerImpl` 持有一个
 *            `go2cpp::bento::LinkServer`，`Start()` 按 `config.bind` 真实绑定
 *            UDP 并启动内置接收线程；
 *          - `transport == kTcp`：持有一个 `go2cpp::bento::HttpServer`，
 *            `Start()` 真实绑定 TCP 并启动 accept/worker 线程。
 *
 *          协议实现全部复用已提交的后端，本文件不复制任何帧编解码或 HTTP
 *          解析；视图方法只做参数转发与生命周期检查。
 *
 *          `OmniServerImpl` / `ConnectionState` 只在本文件可见，头文件不泄漏
 *          实现细节；视图与连接句柄通过 `shared_ptr` 共享内部状态，因此可以
 *          晚于 `NetServer` 析构。
 */

// 先包含 HttpServer：它在 go2cpp::bento 命名空间内使用裸 `net::Socket`，
// 依赖 go2cpp::bento::net 尚未被 omni_server.hpp 定义。包含顺序反了会把
// `net::` 解析到 bento::net 并编译失败（http_server.hpp 是已提交后端，
// 本模块不改它，只保证自己的包含顺序正确）。
#include "go2cpp/bento/net/http_server.hpp"
#include "go2cpp/bento/net/omni_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/memory_transport.hpp"

namespace go2cpp::bento::omni {
namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

/// 单轮每连接最多搬运的出站帧数，防止一个话痨连接饿死其它连接。
constexpr std::size_t kMaxFramesPerPoll = 64;
/// 单轮每连接最多执行的 `Read()` 次数，理由同上。
constexpr std::size_t kMaxReadsPerPoll = 16;
/// 内存端点没有可等待 fd，`PollOnce(timeout)` 以 1ms 粒度轮询。
constexpr Milliseconds kPollSlice{1};
/// M0/M1 的 Memory 传输调用一律非阻塞（MemoryTransport 的 Write 本身也不阻塞）。
constexpr Milliseconds kNonBlockingIo{0};
/// Link 接收线程每次 `RunOnce` 的轮询粒度（毫秒）。
constexpr std::uint32_t kLinkPollTimeoutMs = 2;
/// HTTP 后端在 `worker_threads == 0` 时使用的默认 worker 数（与后端一致）。
constexpr std::size_t kDefaultHttpWorkers = 4;

void SetError(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
}

OmniError Fail(OmniError code, std::string text, std::string* error) {
    SetError(error, std::move(text));
    return code;
}

bool IsKnown(IoMode value) noexcept {
    switch (value) {
        case IoMode::kBlocking:
        case IoMode::kNonBlocking:
            return true;
    }
    return false;
}

bool IsKnown(TransportKind value) noexcept {
    switch (value) {
        case TransportKind::kTcp:
        case TransportKind::kUdp:
        case TransportKind::kTls:
        case TransportKind::kMemory:
            return true;
    }
    return false;
}

bool IsKnown(ProtocolKind value) noexcept {
    switch (value) {
        case ProtocolKind::kHttp:
        case ProtocolKind::kSse:
        case ProtocolKind::kWs:
        case ProtocolKind::kLink:
        case ProtocolKind::kRaw:
            return true;
    }
    return false;
}

bool IsKnown(ConcurrencyKind value) noexcept {
    switch (value) {
        case ConcurrencyKind::kGmp:
        case ConcurrencyKind::kThread:
        case ConcurrencyKind::kPoll:
            return true;
    }
    return false;
}

const char* TransportName(TransportKind value) noexcept {
    switch (value) {
        case TransportKind::kTcp:
            return "Tcp";
        case TransportKind::kUdp:
            return "Udp";
        case TransportKind::kTls:
            return "Tls";
        case TransportKind::kMemory:
            return "Memory";
    }
    return "Unknown";
}

const char* ProtocolName(ProtocolKind value) noexcept {
    switch (value) {
        case ProtocolKind::kHttp:
            return "Http";
        case ProtocolKind::kSse:
            return "Sse";
        case ProtocolKind::kWs:
            return "Ws";
        case ProtocolKind::kLink:
            return "Link";
        case ProtocolKind::kRaw:
            return "Raw";
    }
    return "Unknown";
}

/// 设计 §2.2 的 Transport × Protocol 兼容矩阵。
bool TransportSupportsProtocol(TransportKind transport, ProtocolKind protocol) noexcept {
    switch (transport) {
        case TransportKind::kTcp:
            return protocol == ProtocolKind::kHttp || protocol == ProtocolKind::kSse ||
                   protocol == ProtocolKind::kWs || protocol == ProtocolKind::kRaw;
        case TransportKind::kUdp:
            return protocol == ProtocolKind::kLink || protocol == ProtocolKind::kRaw;
        case TransportKind::kTls:
            return protocol == ProtocolKind::kHttp || protocol == ProtocolKind::kSse ||
                   protocol == ProtocolKind::kWs || protocol == ProtocolKind::kRaw;
        case TransportKind::kMemory:
            return true;
    }
    return false;
}

bool HasProtocol(const OmniNetConfig& config, ProtocolKind protocol) noexcept {
    return std::find(config.protocols.begin(), config.protocols.end(), protocol) !=
           config.protocols.end();
}

/// 设计 §2.2 的 V6：limits 关系校验。
OmniError ValidateLimits(const Limits& limits, std::string* error) {
    if (limits.max_connections == 0) {
        return Fail(OmniError::kInvalidConfig, "limits.max_connections 必须大于 0", error);
    }
    if (limits.inbound_capacity == 0) {
        return Fail(OmniError::kInvalidConfig, "limits.inbound_capacity 必须大于 0", error);
    }
    if (limits.outbound_capacity == 0) {
        return Fail(OmniError::kInvalidConfig, "limits.outbound_capacity 必须大于 0", error);
    }
    if (limits.max_message_bytes == 0) {
        return Fail(OmniError::kInvalidConfig, "limits.max_message_bytes 必须大于 0", error);
    }
    if (limits.read_buffer_bytes == 0) {
        return Fail(OmniError::kInvalidConfig, "limits.read_buffer_bytes 必须大于 0", error);
    }
    if (limits.max_accept_per_tick == 0) {
        return Fail(OmniError::kInvalidConfig, "limits.max_accept_per_tick 必须大于 0", error);
    }
    if (limits.per_connection_budget_bytes < limits.max_message_bytes) {
        return Fail(OmniError::kInvalidConfig,
                    "limits.per_connection_budget_bytes 必须不小于 max_message_bytes", error);
    }
    if (limits.read_buffer_bytes > limits.per_connection_budget_bytes) {
        return Fail(OmniError::kInvalidConfig,
                    "limits.read_buffer_bytes 必须不大于 per_connection_budget_bytes", error);
    }
    if (limits.accept_timeout.count() < 0 || limits.idle_timeout.count() < 0 ||
        limits.write_timeout.count() < 0 || limits.close_timeout.count() < 0) {
        return Fail(OmniError::kInvalidConfig, "limits 的超时字段不能为负", error);
    }
    if (limits.idle_timeout.count() != 0 && limits.close_timeout > limits.idle_timeout) {
        return Fail(OmniError::kInvalidConfig,
                    "limits.close_timeout 必须不大于 idle_timeout（idle_timeout 非 0 时）",
                    error);
    }
    return OmniError::kOk;
}

/// 把 `OmniNetConfig` 映射成 LinkServer 配置；只映射有明确语义的字段。
::go2cpp::bento::LinkServerConfig BuildLinkConfig(const OmniNetConfig& config) {
    ::go2cpp::bento::LinkServerConfig link;
    link.hub.bind_address = config.bind.address;
    link.hub.bind_port = config.bind.port;
    link.hub.max_peers = std::max<std::size_t>(config.limits.max_connections, 1);
    // Hub 的 peer_timeout_ms 为 uint32；0 表示"用后端默认"，避免把 0 解释成
    // "立刻超时"。上限钳制到 1 小时，防止把超时溢出成负数。
    if (config.limits.idle_timeout.count() > 0) {
        const auto timeout = std::min<std::int64_t>(config.limits.idle_timeout.count(), 3600000);
        link.hub.peer_timeout_ms = static_cast<std::uint32_t>(timeout);
    }
    // 空 PSK = 显式允许明文（仅限本机回环/测试）；非空必须 32B（Create 已校验）。
    link.hub.require_encryption = !config.link_psk.empty();
    link.hub.pre_shared_key = config.link_psk;
    link.poll_timeout_ms = kLinkPollTimeoutMs;
    link.start_receiver_thread = true;
    return link;
}

/// 把 `OmniNetConfig` 映射成 HttpServer 配置。
::go2cpp::bento::HttpServerConfig BuildHttpConfig(const OmniNetConfig& config) {
    ::go2cpp::bento::HttpServerConfig http;
    http.worker_threads =
        config.limits.worker_threads == 0 ? kDefaultHttpWorkers : config.limits.worker_threads;
    http.max_queued_connections = std::max<std::size_t>(config.limits.max_connections, 1);
    http.max_requests_per_connection = config.limits.max_requests_per_connection;
    http.max_header_bytes = config.limits.http_max_header_bytes;
    http.max_body_bytes = config.limits.max_message_bytes;
    http.accept_timeout = config.limits.accept_timeout;
    return http;
}

/// HttpServerResult → OmniError：只保留调用方需要区分的类别。
OmniError MapHttpListenError(::go2cpp::bento::HttpServerError error) {
    using ::go2cpp::bento::HttpServerError;
    switch (error) {
        case HttpServerError::None:
            return OmniError::kOk;
        case HttpServerError::InvalidAddress:
        case HttpServerError::BindFailed:
        case HttpServerError::ListenFailed:
            return OmniError::kBindFailed;
        case HttpServerError::AlreadyListening:
        case HttpServerError::AlreadyRunning:
            return OmniError::kAlreadyStarted;
        case HttpServerError::NotListening:
        case HttpServerError::UnsupportedModel:
        case HttpServerError::ThreadFailure:
            return OmniError::kInternal;
    }
    return OmniError::kInternal;
}

}  // namespace

namespace detail {

/// 一条连接的内部状态；由 `NetServer` 的连接表与 `RawConnection` 句柄共享。
struct ConnectionState final {
    ConnectionId id;                                 ///< 连接代号（构造后不变）。
    std::unique_ptr<net::ITransport> transport;      ///< 已建立的传输（唯一所有者）。
    RawChannelPtr inbound;                           ///< 服务器 → 应用。
    RawChannelPtr outbound;                          ///< 应用 → 服务器。
    std::mutex mutex;                                ///< 保护 transport/paused/stats 与 I/O。
    std::atomic<bool> closed{false};                 ///< 是否已关闭（幂等标记）。
    std::atomic<CloseReason> reason{CloseReason::kNone};  ///< 终值关闭原因。
    bool paused{false};                              ///< Inbound 满导致暂停读（受 mutex 保护）。
    ConnectionStats stats;                           ///< 连接级计数（受 mutex 保护）。
    std::weak_ptr<OmniServerImpl> owner;             ///< 反向引用，打断与表格的循环。
};

/// 服务器级原子计数器；`SnapshotStats()` 一次性读出。
struct ServerCounters final {
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> active{0};
    std::atomic<std::uint64_t> closed_graceful{0};
    std::atomic<std::uint64_t> closed_abort{0};
    std::atomic<std::uint64_t> rejected_limit{0};
    std::atomic<std::uint64_t> rejected_per_peer{0};
    std::atomic<std::uint64_t> sniff_rejected{0};
    std::atomic<std::uint64_t> bytes_in{0};
    std::atomic<std::uint64_t> bytes_out{0};
    std::atomic<std::uint64_t> messages_in{0};
    std::atomic<std::uint64_t> messages_out{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> backpressure_events{0};
    std::atomic<std::uint64_t> http_requests{0};
    std::atomic<std::uint64_t> http_keepalive_reuse{0};
    std::atomic<std::uint64_t> sse_events{0};
    std::atomic<std::uint64_t> ws_messages{0};
    std::atomic<std::uint64_t> raw_frames{0};
    std::atomic<std::uint64_t> accept_wait_ms_total{0};
    std::atomic<std::uint64_t> sniff_timeout_total{0};
};

/// 一次出站排空的结果；`reason != kNone` 表示该连接需要关闭。
struct IoOutcome final {
    std::size_t events{0};
    CloseReason reason{CloseReason::kNone};
    CloseMode mode{CloseMode::kGraceful};
};

/// 服务器内部实现（头文件只前置声明）。
struct OmniServerImpl final : std::enable_shared_from_this<OmniServerImpl> {
    explicit OmniServerImpl(OmniNetConfig value) : config(std::move(value)) {}

    ~OmniServerImpl();

    OmniServerImpl(const OmniServerImpl&) = delete;
    OmniServerImpl& operator=(const OmniServerImpl&) = delete;

    bool Start(std::string* error);
    void Stop(CloseMode policy);
    ConnectionId Attach(std::unique_ptr<net::ITransport> transport);
    std::unique_ptr<net::ITransport> ConnectMemory(std::string* error);
    PollResult PollOnce(Milliseconds timeout);
    void CloseConnection(const std::shared_ptr<ConnectionState>& state, CloseReason reason,
                         CloseMode mode);

    /// 视图的手动 Listen：只在 Start 之前允许。
    bool ListenLink(const std::string& address, std::uint16_t port, std::string* error);
    bool ListenHttp(const std::string& endpoint, std::string* error);
    bool ListenHttp(const std::string& host, std::uint16_t port, std::string* error);
    /// HTTP 路由安装（包装计数）；空 handler 表示取消。
    void SetHttpHandler(::go2cpp::bento::HttpServer::RequestHandler handler);
    void SetSseHandler(std::string path, ::go2cpp::bento::HttpServer::SseHandler handler);

    NetStats SnapshotStats() const;
    std::size_t ConnectionCount() const;
    std::shared_ptr<ConnectionState> Find(ConnectionId id) const;
    std::vector<std::shared_ptr<ConnectionState>> SnapshotConnections() const;

    OmniNetConfig config;  ///< 不可变配置。
    std::atomic<bool> running{false};
    std::atomic<bool> stopped{false};  ///< Stop 后进入终态，不可重启。
    std::atomic<bool> polling{false};  ///< PollOnce 重入检测。
    std::atomic<OmniError> last_error{OmniError::kOk};  ///< 最近一次 Start 的错误码。
    std::atomic<std::uint16_t> port{0};  ///< Start 成功后的真实监听端口。
    mutable std::mutex table_mutex;      ///< 保护 connections / next_id。
    std::unordered_map<std::uint64_t, std::shared_ptr<ConnectionState>> connections;
    std::uint64_t next_id{1};                       ///< 单调递增，永不复用。
    std::vector<std::uint8_t> read_scratch;         ///< PollOnce 专用读缓冲（单驱动者）。
    ChannelPtr<RawConnection> accept_channel;        ///< RawServer::Accept() 的唯一通道。
    ServerCounters counters;

    /// UDP 后端；`transport == kUdp` 时非空。
    std::unique_ptr<::go2cpp::bento::LinkServer> link_backend;
    ::go2cpp::bento::LinkServerConfig link_config;  ///< 后端配置快照（Create 写入）。
#if defined(GO2CPP_OMNI_WITH_HTTP)
    /// TCP 后端；`transport == kTcp` 时非空。
    std::unique_ptr<::go2cpp::bento::HttpServer> http_backend;
    ::go2cpp::bento::HttpServerConfig http_config;  ///< 后端配置快照（Create 写入）。
#endif

 private:
    /// 排空出站通道；调用方必须已持有 `state->mutex`。
    IoOutcome DrainOutboundLocked(const std::shared_ptr<ConnectionState>& state,
                                  std::size_t limit);
    /// 单连接单轮泵；调用方不得持有 `state->mutex`。
    std::size_t PumpConnection(const std::shared_ptr<ConnectionState>& state,
                               CloseReason* reason, CloseMode* mode);
    /// 遍历当前连接并泵一轮，返回本轮事件数。
    std::size_t PumpAll();
};

/// 关闭一条连接状态（幂等）；返回是否由本次调用完成关闭。
///
/// @details 关闭顺序固定（设计 C6）：先关闭 Inbound，再写终值 `close_reason`，
///          最后关闭 Outbound 与传输。这样应用不会读到 `kNone` 却已经 EOF。
bool CloseStateInternal(const std::shared_ptr<ConnectionState>& state, CloseReason reason,
                        CloseMode mode) {
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed.load(std::memory_order_acquire)) {
        return false;
    }
    if (state->inbound) {
        (void)state->inbound->Close();
    }
    state->reason.store(reason, std::memory_order_release);
    state->closed.store(true, std::memory_order_release);
    if (state->outbound) {
        (void)state->outbound->Close();
    }
    if (state->transport) {
        if (mode == CloseMode::kAbort) {
            (void)state->transport->Close();
        } else {
            (void)state->transport->ShutdownWrite();
        }
    }
    return true;
}

OmniServerImpl::~OmniServerImpl() {
    // NetServer 析构时已经 Stop 过；这里兜住"只留视图/句柄"的路径。
    Stop(CloseMode::kAbort);
}

bool OmniServerImpl::Start(std::string* error) {
    SetError(error, std::string{});
    last_error.store(OmniError::kOk, std::memory_order_release);
    if (stopped.load(std::memory_order_acquire)) {
        SetError(error, "NetServer 已 Stop，M1 不支持同一实例重启；请重新 Create()");
        last_error.store(OmniError::kAlreadyStarted, std::memory_order_release);
        return false;
    }
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        SetError(error, "Start() 重复调用");
        last_error.store(OmniError::kAlreadyStarted, std::memory_order_release);
        return false;
    }

    if (config.transport == TransportKind::kUdp) {
        auto* backend = link_backend.get();
        if (backend == nullptr) {
            running.store(false, std::memory_order_release);
            last_error.store(OmniError::kInternal, std::memory_order_release);
            SetError(error, "内部错误：UDP 后端未构造");
            return false;
        }
        // 视图可能已经在 Start 之前手动 Listen 过；此时复用，不重复绑定。
        if (!backend->is_open()) {
            std::string listen_error;
            if (!backend->Listen(config.bind.address, config.bind.port, link_config,
                                 &listen_error)) {
                running.store(false, std::memory_order_release);
                last_error.store(OmniError::kBindFailed, std::memory_order_release);
                SetError(error, "UDP 绑定 " + config.bind.address + ":" +
                                    std::to_string(config.bind.port) + " 失败：" + listen_error);
                return false;
            }
        }
        if (!backend->Start()) {
            running.store(false, std::memory_order_release);
            last_error.store(OmniError::kInternal, std::memory_order_release);
            SetError(error, "LinkServer 启动失败（socket 已关闭或未监听）");
            return false;
        }
        port.store(backend->local_port(), std::memory_order_release);
        return true;
    }

    if (config.transport == TransportKind::kTcp) {
#if defined(GO2CPP_OMNI_WITH_HTTP)
        auto* backend = http_backend.get();
        if (backend == nullptr) {
            running.store(false, std::memory_order_release);
            last_error.store(OmniError::kInternal, std::memory_order_release);
            SetError(error, "内部错误：HTTP 后端未构造");
            return false;
        }
        if (backend->Port() == 0) {
            const auto listened =
                backend->Listen(config.bind.address, config.bind.port, http_config);
            if (!listened.ok()) {
                running.store(false, std::memory_order_release);
                last_error.store(MapHttpListenError(listened.error), std::memory_order_release);
                SetError(error, "TCP 绑定 " + config.bind.address + ":" +
                                    std::to_string(config.bind.port) + " 失败：" +
                                    listened.message);
                return false;
            }
        }
        const auto started = backend->Start();
        if (!started.ok()) {
            running.store(false, std::memory_order_release);
            last_error.store(OmniError::kInternal, std::memory_order_release);
            SetError(error, "HttpServer 启动失败：" + started.message);
            return false;
        }
        port.store(backend->Port(), std::memory_order_release);
        return true;
#else
        running.store(false, std::memory_order_release);
        last_error.store(OmniError::kUnsupported, std::memory_order_release);
        SetError(error, "本构建未启用 HTTP 后端（go2cpp_network 不可用）");
        return false;
#endif
    }

    // Memory：没有监听/线程，Start 只做状态切换；Accept 通道在 Create 建好。
    return true;
}

void OmniServerImpl::Stop(CloseMode policy) {
    if (stopped.exchange(true, std::memory_order_acq_rel)) {
        return;  // 幂等：第二次 Stop 直接返回。
    }
    running.store(false, std::memory_order_release);
    // 先停真实后端（join 线程、关闭监听 socket），保证端口立即释放。
    if (link_backend) {
        link_backend->Stop();
        port.store(0, std::memory_order_release);
    }
#if defined(GO2CPP_OMNI_WITH_HTTP)
    if (http_backend) {
        http_backend->Stop();
        port.store(0, std::memory_order_release);
    }
#endif
    if (accept_channel) {
        (void)accept_channel->Close();
    }
    const auto snapshot = SnapshotConnections();
    for (const auto& state : snapshot) {
        if (policy == CloseMode::kGraceful) {
            // 优雅关闭只做一轮非阻塞排空，不等待对端（M0 边界）。
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->closed.load(std::memory_order_acquire)) {
                (void)DrainOutboundLocked(state, kMaxFramesPerPoll);
            }
        }
        CloseConnection(state, CloseReason::kShutdown, policy);
    }
    std::lock_guard<std::mutex> lock(table_mutex);
    connections.clear();
}

ConnectionId OmniServerImpl::Attach(std::unique_ptr<net::ITransport> transport) {
    if (!transport) {
        return ConnectionId{};
    }
    // M1：真实后端的连接由后端自己建立；外部注入仅保留给 Memory 回环。
    if (config.transport != TransportKind::kMemory) {
        return ConnectionId{};
    }
    if (!running.load(std::memory_order_acquire) || stopped.load(std::memory_order_acquire)) {
        return ConnectionId{};
    }
    std::lock_guard<std::mutex> lock(table_mutex);
    if (connections.size() >= config.limits.max_connections) {
        counters.rejected_limit.fetch_add(1, std::memory_order_relaxed);
        return ConnectionId{};
    }
    auto state = std::make_shared<ConnectionState>();
    state->id = ConnectionId{next_id++};
    state->transport = std::move(transport);
    state->inbound = RawChannel::Create(config.limits.inbound_capacity);
    state->outbound = RawChannel::Create(config.limits.outbound_capacity);
    state->owner = weak_from_this();
    // 先推入 Accept 再进连接表：应用拿到句柄时通道一定已经建好。
    if (HasProtocol(config, ProtocolKind::kRaw)) {
        if (!accept_channel || !accept_channel->TrySend(RawConnection(state)).Ok()) {
            counters.rejected_limit.fetch_add(1, std::memory_order_relaxed);
            return ConnectionId{};
        }
    }
    connections.emplace(state->id.value, state);
    counters.accepted.fetch_add(1, std::memory_order_relaxed);
    counters.active.fetch_add(1, std::memory_order_relaxed);
    return state->id;
}

std::unique_ptr<net::ITransport> OmniServerImpl::ConnectMemory(std::string* error) {
    if (config.transport != TransportKind::kMemory) {
        SetError(error, "ConnectMemory() 仅适用于 Memory 传输；UDP/TCP 请用对应后端");
        return nullptr;
    }
    if (!running.load(std::memory_order_acquire) || stopped.load(std::memory_order_acquire)) {
        SetError(error, "服务未启动：先调用 Start()");
        return nullptr;
    }
    auto pair = net::MemoryTransport::CreatePair();
    std::unique_ptr<net::ITransport> client = std::move(pair.first);
    const ConnectionId id = Attach(std::move(pair.second));
    if (!id.valid()) {
        SetError(error, "接入被拒绝（未启动/达到 max_connections/Accept 通道满）");
        return nullptr;
    }
    return client;
}

bool OmniServerImpl::ListenLink(const std::string& address, std::uint16_t listen_port,
                                std::string* error) {
    if (link_backend == nullptr) {
        SetError(error, "Link 后端不可用（transport 不是 UDP）");
        return false;
    }
    if (running.load(std::memory_order_acquire)) {
        SetError(error, "服务已启动，不能再 Listen()；请在 Start() 之前调用");
        return false;
    }
    if (stopped.load(std::memory_order_acquire)) {
        SetError(error, "实例已 Stop，不能再次 Listen()；请重新 Create()");
        return false;
    }
    return link_backend->Listen(address, listen_port, link_config, error);
}

bool OmniServerImpl::ListenHttp(const std::string& endpoint, std::string* error) {
#if defined(GO2CPP_OMNI_WITH_HTTP)
    if (http_backend == nullptr) {
        SetError(error, "HTTP 后端不可用（transport 不是 TCP）");
        return false;
    }
    if (running.load(std::memory_order_acquire)) {
        SetError(error, "服务已启动，不能再 Listen()；请在 Start() 之前调用");
        return false;
    }
    if (stopped.load(std::memory_order_acquire)) {
        SetError(error, "实例已 Stop，不能再次 Listen()；请重新 Create()");
        return false;
    }
    const auto result = http_backend->Listen(endpoint, http_config);
    if (!result.ok()) {
        SetError(error, result.message);
        return false;
    }
    return true;
#else
    (void)endpoint;
    SetError(error, "本构建未启用 HTTP 后端（go2cpp_network 不可用）");
    return false;
#endif
}

bool OmniServerImpl::ListenHttp(const std::string& host, std::uint16_t listen_port,
                                std::string* error) {
#if defined(GO2CPP_OMNI_WITH_HTTP)
    if (http_backend == nullptr) {
        SetError(error, "HTTP 后端不可用（transport 不是 TCP）");
        return false;
    }
    if (running.load(std::memory_order_acquire)) {
        SetError(error, "服务已启动，不能再 Listen()；请在 Start() 之前调用");
        return false;
    }
    if (stopped.load(std::memory_order_acquire)) {
        SetError(error, "实例已 Stop，不能再 Listen()；请重新 Create()");
        return false;
    }
    const auto result = http_backend->Listen(host, listen_port, http_config);
    if (!result.ok()) {
        SetError(error, result.message);
        return false;
    }
    return true;
#else
    (void)host;
    (void)listen_port;
    SetError(error, "本构建未启用 HTTP 后端（go2cpp_network 不可用）");
    return false;
#endif
}

void OmniServerImpl::SetHttpHandler(::go2cpp::bento::HttpServer::RequestHandler handler) {
#if defined(GO2CPP_OMNI_WITH_HTTP)
    if (http_backend == nullptr) {
        return;
    }
    if (!handler) {
        http_backend->OnRequest(nullptr);
        return;
    }
    std::weak_ptr<OmniServerImpl> weak = weak_from_this();
    auto wrapped = [weak, handler = std::move(handler)](const http::Request& request) {
        if (auto impl = weak.lock()) {
            impl->counters.http_requests.fetch_add(1, std::memory_order_relaxed);
        }
        return handler(request);
    };
    http_backend->OnRequest(std::move(wrapped));
#else
    (void)handler;
#endif
}

void OmniServerImpl::SetSseHandler(std::string path,
                                   ::go2cpp::bento::HttpServer::SseHandler handler) {
#if defined(GO2CPP_OMNI_WITH_HTTP)
    if (http_backend == nullptr) {
        return;
    }
    if (!handler) {
        http_backend->AsSse(std::move(path), nullptr);
        return;
    }
    std::weak_ptr<OmniServerImpl> weak = weak_from_this();
    auto wrapped = [weak, handler = std::move(handler)](const http::Request& request,
                                                       sse::Stream& stream) {
        if (auto impl = weak.lock()) {
            impl->counters.sse_events.fetch_add(1, std::memory_order_relaxed);
        }
        handler(request, stream);
    };
    http_backend->AsSse(std::move(path), std::move(wrapped));
#else
    (void)path;
    (void)handler;
#endif
}

NetStats OmniServerImpl::SnapshotStats() const {
    NetStats stats;
    stats.accepted = counters.accepted.load(std::memory_order_relaxed);
    stats.active = counters.active.load(std::memory_order_relaxed);
    stats.closed_graceful = counters.closed_graceful.load(std::memory_order_relaxed);
    stats.closed_abort = counters.closed_abort.load(std::memory_order_relaxed);
    stats.rejected_limit = counters.rejected_limit.load(std::memory_order_relaxed);
    stats.rejected_per_peer = counters.rejected_per_peer.load(std::memory_order_relaxed);
    stats.sniff_rejected = counters.sniff_rejected.load(std::memory_order_relaxed);
    stats.bytes_in = counters.bytes_in.load(std::memory_order_relaxed);
    stats.bytes_out = counters.bytes_out.load(std::memory_order_relaxed);
    stats.messages_in = counters.messages_in.load(std::memory_order_relaxed);
    stats.messages_out = counters.messages_out.load(std::memory_order_relaxed);
    stats.dropped = counters.dropped.load(std::memory_order_relaxed);
    stats.backpressure_events = counters.backpressure_events.load(std::memory_order_relaxed);
    stats.http_requests = counters.http_requests.load(std::memory_order_relaxed);
    stats.http_keepalive_reuse = counters.http_keepalive_reuse.load(std::memory_order_relaxed);
    stats.sse_events = counters.sse_events.load(std::memory_order_relaxed);
    stats.ws_messages = counters.ws_messages.load(std::memory_order_relaxed);
    stats.raw_frames = counters.raw_frames.load(std::memory_order_relaxed);
    stats.accept_wait_ms_total = counters.accept_wait_ms_total.load(std::memory_order_relaxed);
    stats.sniff_timeout_total = counters.sniff_timeout_total.load(std::memory_order_relaxed);

    if (config.transport == TransportKind::kUdp && link_backend) {
        const auto link = link_backend->Stats();
        const auto hub = link_backend->hub_stats();
        stats.accepted = link.peers_accepted;
        stats.active = link.active_peers;
        stats.closed_graceful = link.peers_disconnected;
        stats.messages_in = link.stream_messages_received + link.datagram_messages_received;
        stats.messages_out = link.sends_ok;
        stats.bytes_in = hub.bytes_received;
        stats.bytes_out = hub.bytes_sent;
        stats.dropped =
            link.sends_would_block + link.sends_failed + link.sends_invalid;
        stats.rejected_limit = hub.peers_rejected;
        stats.raw_frames = 0;
    } else if (config.transport == TransportKind::kTcp) {
        stats.messages_in = stats.http_requests;
#if defined(GO2CPP_OMNI_WITH_HTTP)
        stats.active = http_backend ? http_backend->ActiveConnections() : 0;
#endif
        // HttpServer 未暴露累计连接数；accepted 保持 0（M2 在连接层补计数），
        // 请求量用 http_requests/messages_in 表达，避免给出错误数字。
        stats.accepted = 0;
    }
    return stats;
}

std::size_t OmniServerImpl::ConnectionCount() const {
    if (config.transport == TransportKind::kUdp && link_backend) {
        return link_backend->peer_count();
    }
#if defined(GO2CPP_OMNI_WITH_HTTP)
    if (config.transport == TransportKind::kTcp && http_backend) {
        return http_backend->ActiveConnections();
    }
#endif
    std::lock_guard<std::mutex> lock(table_mutex);
    return connections.size();
}

std::shared_ptr<ConnectionState> OmniServerImpl::Find(ConnectionId id) const {
    std::lock_guard<std::mutex> lock(table_mutex);
    const auto it = connections.find(id.value);
    return it == connections.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<ConnectionState>> OmniServerImpl::SnapshotConnections() const {
    std::lock_guard<std::mutex> lock(table_mutex);
    std::vector<std::shared_ptr<ConnectionState>> snapshot;
    snapshot.reserve(connections.size());
    for (const auto& entry : connections) {
        snapshot.push_back(entry.second);
    }
    return snapshot;
}

IoOutcome OmniServerImpl::DrainOutboundLocked(const std::shared_ptr<ConnectionState>& state,
                                              std::size_t limit) {
    IoOutcome outcome;
    for (std::size_t index = 0; index < limit; ++index) {
        auto received = state->outbound->TryRecv();
        if (!received.ok) {
            break;
        }
        RawFrame frame = std::move(*received.value);
        if (frame.bytes.size() > config.limits.max_message_bytes) {
            // 超限帧已被取出通道，计一次 dropped 再断开；否则统计会把
            // "应用发了一条被拒绝的消息"伪装成什么也没发生。
            state->stats.dropped += 1;
            counters.dropped.fetch_add(1, std::memory_order_relaxed);
            outcome.reason = CloseReason::kLimitExceeded;
            outcome.mode = CloseMode::kAbort;
            return outcome;
        }
        const auto write = state->transport->WriteFull(frame.bytes.data(), frame.bytes.size(),
                                                       kNonBlockingIo);
        if (!write.ok() || write.bytes != frame.bytes.size()) {
            state->stats.errors += 1;
            state->stats.last_system_error = write.system_error;
            outcome.mode = CloseMode::kAbort;
            if (write.code == net::Code::kTimeout) {
                outcome.reason = CloseReason::kWriteTimeout;
            } else if (write.code == net::Code::kClosed) {
                outcome.reason = CloseReason::kPeerClosed;
            } else {
                outcome.reason = CloseReason::kProtocolError;
            }
            return outcome;
        }
        const auto bytes = static_cast<std::uint64_t>(frame.bytes.size());
        state->stats.bytes_out += bytes;
        state->stats.messages_out += 1;
        counters.bytes_out.fetch_add(bytes, std::memory_order_relaxed);
        counters.messages_out.fetch_add(1, std::memory_order_relaxed);
        outcome.events += 1;
    }
    return outcome;
}

std::size_t OmniServerImpl::PumpConnection(const std::shared_ptr<ConnectionState>& state,
                                           CloseReason* reason, CloseMode* mode) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed.load(std::memory_order_acquire)) {
        return 0;
    }

    std::size_t events = 0;

    // 1) 出站：应用 → 传输。写失败立即终止该连接。
    const IoOutcome outbound = DrainOutboundLocked(state, kMaxFramesPerPoll);
    events += outbound.events;
    if (outbound.reason != CloseReason::kNone) {
        *reason = outbound.reason;
        *mode = outbound.mode;
        return events;
    }

    // 2) 入站：Inbound 满时暂停读（反压），等消费者取走后再读，不丢帧。
    if (state->paused && state->inbound->Len() < state->inbound->Capacity()) {
        state->paused = false;
    }
    if (state->paused) {
        return events;
    }

    if (read_scratch.size() != config.limits.max_message_bytes) {
        read_scratch.assign(config.limits.max_message_bytes, std::uint8_t{0});
    }
    for (std::size_t reads = 0; reads < kMaxReadsPerPoll; ++reads) {
        const std::size_t capacity = state->inbound->Capacity();
        if (capacity == 0 || state->inbound->Len() >= capacity) {
            if (!state->paused) {
                state->paused = true;
                state->stats.backpressure_events += 1;
                counters.backpressure_events.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        const auto read = state->transport->Read(read_scratch.data(), read_scratch.size(),
                                                 kNonBlockingIo);
        if (read.code == net::Code::kTimeout) {
            break;
        }
        if (read.code == net::Code::kClosed) {
            *reason = CloseReason::kPeerClosed;
            *mode = CloseMode::kGraceful;
            break;
        }
        if (!read.ok()) {
            state->stats.errors += 1;
            state->stats.last_system_error = read.system_error;
            *reason = CloseReason::kProtocolError;
            *mode = CloseMode::kAbort;
            break;
        }
        if (read.bytes == 0) {
            break;
        }
        // M0/M1 Memory 不分帧：一次 Read 的字节数就是一帧。
        RawFrame frame;
        frame.bytes.assign(read_scratch.data(),
                           read_scratch.data() + static_cast<std::ptrdiff_t>(read.bytes));
        const auto sent = state->inbound->TrySend(std::move(frame));
        if (!sent.Ok()) {
            if (sent.Closed()) {
                *reason = CloseReason::kLocalClose;
                *mode = CloseMode::kAbort;
            } else {
                // 单生产者下不应发生；保守按反压处理，下一轮重试读取。
                state->paused = true;
                state->stats.backpressure_events += 1;
                counters.backpressure_events.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        const auto bytes = static_cast<std::uint64_t>(read.bytes);
        state->stats.bytes_in += bytes;
        state->stats.messages_in += 1;
        counters.bytes_in.fetch_add(bytes, std::memory_order_relaxed);
        counters.messages_in.fetch_add(1, std::memory_order_relaxed);
        counters.raw_frames.fetch_add(1, std::memory_order_relaxed);
        events += 1;
    }
    return events;
}

std::size_t OmniServerImpl::PumpAll() {
    const auto snapshot = SnapshotConnections();
    std::size_t events = 0;
    for (const auto& state : snapshot) {
        CloseReason reason = CloseReason::kNone;
        CloseMode mode = CloseMode::kGraceful;
        events += PumpConnection(state, &reason, &mode);
        if (reason != CloseReason::kNone) {
            CloseConnection(state, reason, mode);
            // 连接关闭本身也是一次可观测事件，避免 PollOnce 把"读到 EOF 并断开"
            // 误报成"本轮无事发生"。
            events += 1;
        }
    }
    return events;
}

PollResult OmniServerImpl::PollOnce(Milliseconds timeout) {
    PollResult result;
    if (!running.load(std::memory_order_acquire)) {
        result.error = OmniError::kNotRunning;
        return result;
    }
    if (config.concurrency != ConcurrencyKind::kPoll) {
        result.error = OmniError::kNotPollMode;
        return result;
    }
    bool expected = false;
    if (!polling.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        result.error = OmniError::kReentrant;
        return result;
    }
    struct PollGuard final {
        std::atomic<bool>* flag;
        ~PollGuard() { flag->store(false, std::memory_order_release); }
    } guard{&polling};

    if (timeout.count() < 0) {
        timeout = Milliseconds{0};
    }
    const Clock::time_point deadline = Clock::now() + timeout;
    for (;;) {
        result.events = PumpAll();
        if (result.events != 0) {
            result.timeout = false;
            break;
        }
        if (Clock::now() >= deadline) {
            // timeout == 0 时这里立即命中：单轮非阻塞检查。
            result.timeout = true;
            break;
        }
        std::this_thread::sleep_for(kPollSlice);
    }
    result.error = OmniError::kOk;
    return result;
}

void OmniServerImpl::CloseConnection(const std::shared_ptr<ConnectionState>& state,
                                     CloseReason reason, CloseMode mode) {
    if (!CloseStateInternal(state, reason, mode)) {
        return;
    }
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(table_mutex);
        const auto it = connections.find(state->id.value);
        if (it != connections.end() && it->second == state) {
            connections.erase(it);
            removed = true;
        }
    }
    if (removed) {
        counters.active.fetch_sub(1, std::memory_order_relaxed);
        if (mode == CloseMode::kAbort) {
            counters.closed_abort.fetch_add(1, std::memory_order_relaxed);
        } else {
            counters.closed_graceful.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace detail

using detail::OmniServerImpl;

const char* OmniErrorName(OmniError error) noexcept {
    switch (error) {
        case OmniError::kOk:
            return "Ok";
        case OmniError::kInvalidConfig:
            return "InvalidConfig";
        case OmniError::kUnsupportedCombination:
            return "UnsupportedCombination";
        case OmniError::kUnsupported:
            return "Unsupported";
        case OmniError::kBindFailed:
            return "BindFailed";
        case OmniError::kAlreadyStarted:
            return "AlreadyStarted";
        case OmniError::kNotRunning:
            return "NotRunning";
        case OmniError::kNotPollMode:
            return "NotPollMode";
        case OmniError::kReentrant:
            return "Reentrant";
        case OmniError::kProtocolMismatch:
            return "ProtocolMismatch";
        case OmniError::kSniffTimeout:
            return "SniffTimeout";
        case OmniError::kSniffTooLarge:
            return "SniffTooLarge";
        case OmniError::kBackpressure:
            return "Backpressure";
        case OmniError::kLimitExceeded:
            return "LimitExceeded";
        case OmniError::kTimeout:
            return "Timeout";
        case OmniError::kClosed:
            return "Closed";
        case OmniError::kInternal:
            return "Internal";
    }
    return "Unknown";
}

OmniError NetServer::ValidateConfig(const OmniNetConfig& config, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    // V1：枚举值在已知范围内（防新增枚举未处理）。
    if (!IsKnown(config.mode)) {
        return Fail(OmniError::kInvalidConfig, "mode 枚举值越界", error);
    }
    if (!IsKnown(config.transport)) {
        return Fail(OmniError::kInvalidConfig, "transport 枚举值越界", error);
    }
    if (!IsKnown(config.concurrency)) {
        return Fail(OmniError::kInvalidConfig, "concurrency 枚举值越界", error);
    }
    for (const ProtocolKind protocol : config.protocols) {
        if (!IsKnown(protocol)) {
            return Fail(OmniError::kInvalidConfig, "protocols 含越界枚举值", error);
        }
    }
    // V2：protocols 非空、无重复。
    if (config.protocols.empty()) {
        return Fail(OmniError::kInvalidConfig, "protocols 不能为空", error);
    }
    for (std::size_t i = 0; i < config.protocols.size(); ++i) {
        for (std::size_t j = i + 1; j < config.protocols.size(); ++j) {
            if (config.protocols[i] == config.protocols[j]) {
                return Fail(OmniError::kInvalidConfig,
                            std::string("protocols 重复：") + ProtocolName(config.protocols[i]),
                            error);
            }
        }
    }
    // V3：transport × protocol 兼容矩阵。
    for (const ProtocolKind protocol : config.protocols) {
        if (!TransportSupportsProtocol(config.transport, protocol)) {
            return Fail(OmniError::kUnsupportedCombination,
                        std::string("transport 与 protocol 不兼容：") +
                            TransportName(config.transport) + " × " + ProtocolName(protocol),
                        error);
        }
    }
    // V4：mode × concurrency 合法矩阵（Blocking + Poll 是设计层面的死锁）。
    if (config.mode == IoMode::kBlocking && config.concurrency == ConcurrencyKind::kPoll) {
        return Fail(OmniError::kUnsupportedCombination,
                    "Blocking + Poll 非法：同步等待会卡死调用方事件循环", error);
    }
    // V5：TLS 服务端在 M4 之前一律不实现。
    if (config.transport == TransportKind::kTls) {
        return Fail(OmniError::kUnsupported, "TLS 服务端在 M4 实现", error);
    }
    // V6：limits 关系。
    if (const OmniError limits = ValidateLimits(config.limits, error); limits != OmniError::kOk) {
        return limits;
    }
    // V7：M1 实现门。只接受已经接入真实后端的组合，其余如实返回 kUnsupported。
    switch (config.transport) {
        case TransportKind::kMemory:
            for (const ProtocolKind protocol : config.protocols) {
                if (protocol != ProtocolKind::kRaw) {
                    return Fail(OmniError::kUnsupported,
                                std::string("M1 Memory 只实现 Raw 协议，收到 ") +
                                    ProtocolName(protocol),
                                error);
                }
            }
            break;
        case TransportKind::kUdp:
            if (config.protocols.size() != 1 || config.protocols[0] != ProtocolKind::kLink) {
                return Fail(OmniError::kUnsupported,
                            "M1 UDP 只实现 Link 协议（单协议，暂不支持嗅探）", error);
            }
            break;
        case TransportKind::kTcp:
#if defined(GO2CPP_OMNI_WITH_HTTP)
            for (const ProtocolKind protocol : config.protocols) {
                if (protocol != ProtocolKind::kHttp && protocol != ProtocolKind::kSse) {
                    return Fail(OmniError::kUnsupported,
                                std::string("M1 TCP 只实现 HTTP/SSE，收到 ") +
                                    ProtocolName(protocol),
                                error);
                }
            }
            break;
#else
            return Fail(OmniError::kUnsupported,
                        "本构建未启用 HTTP 后端（go2cpp_network 不可用）", error);
#endif
        case TransportKind::kTls:
            return Fail(OmniError::kUnsupported, "TLS 服务端在 M4 实现", error);
    }
    // V8：UDP/TCP 的后端自带线程；Poll 模式由调用方驱动，会让 PollOnce 看起来
    // "配置成功但不推进"。M1 只允许 Memory 使用 Poll。
    if (config.transport != TransportKind::kMemory &&
        config.concurrency == ConcurrencyKind::kPoll) {
        return Fail(OmniError::kUnsupportedCombination,
                    "UDP/TCP 后端自带线程，M1 不接受 Poll 并发；请用 Gmp 或 Thread",
                    error);
    }
    // V9：bind 与 PSK（PSK 非空时必须 32 字节）。
    if (config.bind.address.empty()) {
        return Fail(OmniError::kInvalidConfig, "bind.address 不能为空", error);
    }
    if (!config.link_psk.empty() && config.link_psk.size() != 32) {
        return Fail(OmniError::kInvalidConfig, "link_psk 非空时必须为 32 字节", error);
    }
    return OmniError::kOk;
}

std::unique_ptr<NetServer> NetServer::Create(const OmniNetConfig& config, std::string* error) {
    const OmniError error_code = ValidateConfig(config, error);
    if (error_code != OmniError::kOk) {
        return nullptr;
    }
    auto impl = std::make_shared<OmniServerImpl>(config);
    switch (config.transport) {
        case TransportKind::kMemory:
            impl->accept_channel = Channel<RawConnection>::Create(config.limits.inbound_capacity);
            break;
        case TransportKind::kUdp:
            impl->link_config = BuildLinkConfig(config);
            impl->link_backend = std::make_unique<::go2cpp::bento::LinkServer>();
            break;
        case TransportKind::kTcp:
#if defined(GO2CPP_OMNI_WITH_HTTP)
            impl->http_config = BuildHttpConfig(config);
            impl->http_backend = std::make_unique<::go2cpp::bento::HttpServer>();
#endif
            break;
        case TransportKind::kTls:
            // ValidateConfig 已拒绝；这里只为 switch 完整性保留。
            break;
    }
    return std::unique_ptr<NetServer>(new NetServer(std::move(impl)));
}

NetServer::NetServer(std::shared_ptr<OmniServerImpl> impl) noexcept : m_impl(std::move(impl)) {}

NetServer::~NetServer() {
    if (m_impl) {
        m_impl->Stop(CloseMode::kGraceful);
    }
}

bool NetServer::Start(std::string* error) {
    return m_impl != nullptr && m_impl->Start(error);
}

void NetServer::Stop(ShutdownPolicy policy) noexcept {
    if (m_impl) {
        m_impl->Stop(policy);
    }
}

bool NetServer::IsRunning() const noexcept {
    return m_impl != nullptr && m_impl->running.load(std::memory_order_acquire);
}

bool NetServer::running() const noexcept {
    return IsRunning();
}

std::uint16_t NetServer::Port() const noexcept {
    return m_impl == nullptr ? 0 : m_impl->port.load(std::memory_order_acquire);
}

std::size_t NetServer::Connections() const noexcept {
    return m_impl == nullptr ? 0 : m_impl->ConnectionCount();
}

NetStats NetServer::Stats() const {
    return m_impl == nullptr ? NetStats{} : m_impl->SnapshotStats();
}

const OmniNetConfig& NetServer::config() const noexcept {
    return m_impl->config;
}

OmniError NetServer::last_error() const noexcept {
    return m_impl == nullptr ? OmniError::kInternal
                             : m_impl->last_error.load(std::memory_order_acquire);
}

RawServer NetServer::AsRawServer() {
    return RawServer(m_impl, m_impl != nullptr && HasProtocol(m_impl->config, ProtocolKind::kRaw));
}

LinkServer NetServer::AsLinkServer() {
    const bool valid = m_impl != nullptr && m_impl->link_backend != nullptr &&
                       HasProtocol(m_impl->config, ProtocolKind::kLink);
    return LinkServer(m_impl, valid);
}

HttpServer NetServer::AsHttpServer() {
    const bool valid =
        m_impl != nullptr && m_impl->config.transport == TransportKind::kTcp &&
        (HasProtocol(m_impl->config, ProtocolKind::kHttp) ||
         HasProtocol(m_impl->config, ProtocolKind::kSse));
    return HttpServer(m_impl, valid);
}

std::unique_ptr<net::ITransport> NetServer::ConnectMemory(std::string* error) {
    if (!m_impl) {
        SetError(error, "NetServer 没有内部状态");
        return nullptr;
    }
    return m_impl->ConnectMemory(error);
}

ConnectionId NetServer::AttachConnection(std::unique_ptr<net::ITransport> transport) {
    if (!m_impl) {
        return ConnectionId{};
    }
    return m_impl->Attach(std::move(transport));
}

PollResult NetServer::PollOnce(Milliseconds timeout) {
    if (!m_impl) {
        PollResult result;
        result.error = OmniError::kNotRunning;
        return result;
    }
    return m_impl->PollOnce(timeout);
}

RawConnection::RawConnection(std::shared_ptr<detail::ConnectionState> state) noexcept
    : m_state(std::move(state)) {}

ConnectionId RawConnection::id() const noexcept {
    return m_state == nullptr ? ConnectionId{} : m_state->id;
}

bool RawConnection::valid() const noexcept {
    return m_state != nullptr;
}

ChannelPtr<RawFrame> RawConnection::Inbound() const {
    return m_state == nullptr ? ChannelPtr<RawFrame>{} : m_state->inbound;
}

ChannelPtr<RawFrame> RawConnection::Outbound() const {
    return m_state == nullptr ? ChannelPtr<RawFrame>{} : m_state->outbound;
}

void RawConnection::Close(CloseMode mode) noexcept {
    if (m_state == nullptr) {
        return;
    }
    if (const auto impl = m_state->owner.lock()) {
        impl->CloseConnection(m_state, CloseReason::kLocalClose, mode);
        return;
    }
    // 服务器已析构：只关闭本地资源，语义保持一致。
    (void)detail::CloseStateInternal(m_state, CloseReason::kLocalClose, mode);
}

CloseReason RawConnection::close_reason() const noexcept {
    return m_state == nullptr ? CloseReason::kNone : m_state->reason.load(std::memory_order_acquire);
}

ConnectionStats RawConnection::stats() const {
    if (m_state == nullptr) {
        return ConnectionStats{};
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->stats;
}

RawServer::RawServer(std::shared_ptr<detail::OmniServerImpl> impl, bool valid) noexcept
    : m_impl(std::move(impl)), m_valid(valid) {}

bool RawServer::valid() const noexcept {
    return m_valid && m_impl != nullptr;
}

ProtocolKind RawServer::protocol() const noexcept {
    return ProtocolKind::kRaw;
}

ChannelPtr<RawConnection> RawServer::Accept() const {
    return m_impl == nullptr ? nullptr : m_impl->accept_channel;
}

RawConnection RawServer::Connection(ConnectionId id) const {
    if (!valid() || !id.valid()) {
        return RawConnection{};
    }
    const auto state = m_impl->Find(id);
    return state == nullptr ? RawConnection{} : RawConnection(state);
}

std::vector<RawConnection> RawServer::Connections() const {
    std::vector<RawConnection> connections;
    if (!valid()) {
        return connections;
    }
    for (const auto& state : m_impl->SnapshotConnections()) {
        connections.emplace_back(RawConnection(state));
    }
    return connections;
}

LinkServer::LinkServer(std::shared_ptr<detail::OmniServerImpl> impl, bool valid) noexcept
    : m_impl(std::move(impl)), m_valid(valid) {}

bool LinkServer::valid() const noexcept {
    return m_valid && m_impl != nullptr;
}

ProtocolKind LinkServer::protocol() const noexcept {
    return ProtocolKind::kLink;
}

bool LinkServer::Listen(const std::string& address, std::uint16_t listen_port,
                        std::string* error) {
    if (!valid()) {
        SetError(error, "Link 视图无效：配置不是 UDP + Link");
        return false;
    }
    return m_impl->ListenLink(address, listen_port, error);
}

::go2cpp::bento::LinkSendResult LinkServer::Send(
    ::go2cpp::bento::LinkPeerId peer, std::uint8_t channel, const void* data, std::size_t bytes,
    ::go2cpp::bento::LinkReliability reliability) {
    if (!valid()) {
        return ::go2cpp::bento::LinkSendResult::kNotConnected;
    }
    return m_impl->link_backend->Send(peer, channel, data, bytes, reliability);
}

std::size_t LinkServer::Broadcast(std::uint8_t channel, const void* data, std::size_t bytes,
                                  ::go2cpp::bento::LinkReliability reliability) {
    if (!valid()) {
        return 0;
    }
    return m_impl->link_backend->Broadcast(channel, data, bytes, reliability);
}

std::vector<::go2cpp::bento::LinkPeerId> LinkServer::Peers() const {
    if (!valid()) {
        return {};
    }
    return m_impl->link_backend->Peers();
}

bool LinkServer::HasPeer(::go2cpp::bento::LinkPeerId peer) const {
    return valid() && m_impl->link_backend->HasPeer(peer);
}

std::size_t LinkServer::peer_count() const {
    return valid() ? m_impl->link_backend->peer_count() : 0;
}

std::uint16_t LinkServer::local_port() const noexcept {
    return valid() ? m_impl->link_backend->local_port() : 0;
}

void LinkServer::OnPeerConnected(::go2cpp::bento::LinkServer::ConnectHandler handler) {
    if (valid()) {
        m_impl->link_backend->OnPeerConnected(std::move(handler));
    }
}

void LinkServer::OnPeerDisconnected(::go2cpp::bento::LinkServer::DisconnectHandler handler) {
    if (valid()) {
        m_impl->link_backend->OnPeerDisconnected(std::move(handler));
    }
}

void LinkServer::OnStream(::go2cpp::bento::LinkServer::MessageHandler handler) {
    if (valid()) {
        m_impl->link_backend->OnStream(std::move(handler));
    }
}

void LinkServer::OnDatagram(::go2cpp::bento::LinkServer::MessageHandler handler) {
    if (valid()) {
        m_impl->link_backend->OnDatagram(std::move(handler));
    }
}

::go2cpp::bento::LinkServer* LinkServer::backend() const noexcept {
    return valid() ? m_impl->link_backend.get() : nullptr;
}

HttpServer::HttpServer(std::shared_ptr<detail::OmniServerImpl> impl, bool valid) noexcept
    : m_impl(std::move(impl)), m_valid(valid) {}

bool HttpServer::valid() const noexcept {
    return m_valid && m_impl != nullptr;
}

ProtocolKind HttpServer::protocol() const noexcept {
    return ProtocolKind::kHttp;
}

bool HttpServer::Listen(const std::string& endpoint, std::string* error) {
    if (!valid()) {
        SetError(error, "HTTP 视图无效：配置不是 TCP + HTTP/SSE");
        return false;
    }
    return m_impl->ListenHttp(endpoint, error);
}

bool HttpServer::Listen(const std::string& host, std::uint16_t listen_port, std::string* error) {
    if (!valid()) {
        SetError(error, "HTTP 视图无效：配置不是 TCP + HTTP/SSE");
        return false;
    }
    return m_impl->ListenHttp(host, listen_port, error);
}

void HttpServer::OnRequest(RequestHandler handler) {
    if (valid()) {
        m_impl->SetHttpHandler(std::move(handler));
    }
}

void HttpServer::AsSse(std::string path, SseHandler handler) {
    if (valid()) {
        m_impl->SetSseHandler(std::move(path), std::move(handler));
    }
}

std::uint16_t HttpServer::Port() const noexcept {
    return valid() ? m_impl->port.load(std::memory_order_acquire) : 0;
}

std::string HttpServer::LocalAddress() const {
    if (!valid()) {
        return {};
    }
#if defined(GO2CPP_OMNI_WITH_HTTP)
    return m_impl->http_backend->LocalAddress();
#else
    return {};
#endif
}

std::size_t HttpServer::ActiveConnections() const noexcept {
    if (!valid()) {
        return 0;
    }
#if defined(GO2CPP_OMNI_WITH_HTTP)
    return m_impl->http_backend->ActiveConnections();
#else
    return 0;
#endif
}

::go2cpp::bento::HttpServer* HttpServer::backend() const noexcept {
#if defined(GO2CPP_OMNI_WITH_HTTP)
    return valid() ? m_impl->http_backend.get() : nullptr;
#else
    return nullptr;
#endif
}

}  // namespace go2cpp::bento::omni
