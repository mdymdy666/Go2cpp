/**
 * @file omni_server.cpp
 * @brief OmniNetServer M0 骨架实现：配置校验、连接表、Memory 回环与 Raw 通道。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details M0 的实现边界（与头文件注释一一对应）：
 *          - 只有 Memory 传输 + Raw 协议；没有 socket、线程、Fiber；
 *          - 驱动只有 `PollOnce()`，内存端点以 1ms 粒度轮询；
 *          - 不做分帧：一次成功 `Read()` 的字节数就是一帧；
 *          - Inbound 满时暂停读并计数，不丢帧；
 *          - `Stop()` 幂等且是终态。
 *
 *          `OmniServerImpl` / `ConnectionState` 只在本文件可见，头文件不泄漏
 *          任何实现细节；视图与连接句柄通过 `shared_ptr` 共享内部状态，
 *          因此可以晚于 `NetServer` 析构。
 */

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
/// M0 的传输调用一律非阻塞（MemoryTransport 的 Write 本身也不阻塞）。
constexpr Milliseconds kNonBlockingIo{0};

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

    NetStats SnapshotStats() const;
    std::size_t ConnectionCount() const;
    std::shared_ptr<ConnectionState> Find(ConnectionId id) const;
    std::vector<std::shared_ptr<ConnectionState>> SnapshotConnections() const;

    OmniNetConfig config;  ///< 不可变配置。
    std::atomic<bool> running{false};
    std::atomic<bool> stopped{false};  ///< Stop 后进入终态，不可重启。
    std::atomic<bool> polling{false};  ///< PollOnce 重入检测。
    mutable std::mutex table_mutex;    ///< 保护 connections / next_id。
    std::unordered_map<std::uint64_t, std::shared_ptr<ConnectionState>> connections;
    std::uint64_t next_id{1};                       ///< 单调递增，永不复用。
    std::vector<std::uint8_t> read_scratch;         ///< PollOnce 专用读缓冲（单驱动者）。
    ChannelPtr<RawConnection> accept_channel;        ///< RawServer::Accept() 的唯一通道。
    ServerCounters counters;

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
    if (stopped.load(std::memory_order_acquire)) {
        SetError(error, "NetServer 已 Stop，M0 不支持重启；请重新 Create()");
        return false;
    }
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        SetError(error, "Start() 重复调用");
        return false;
    }
    // M0：Memory 传输没有监听/线程，Start 只做状态切换；Accept 通道在 Create 建好。
    return true;
}

void OmniServerImpl::Stop(CloseMode policy) {
    if (stopped.exchange(true, std::memory_order_acq_rel)) {
        return;  // 幂等：第二次 Stop 直接返回。
    }
    running.store(false, std::memory_order_release);
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
    return stats;
}

std::size_t OmniServerImpl::ConnectionCount() const {
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
        // M0 不分帧：一次 Read 的字节数就是一帧。
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
    // M0 实现门：只落地 Memory 传输与 Raw 协议，其余如实返回 kUnsupported。
    if (config.transport != TransportKind::kMemory) {
        return Fail(OmniError::kUnsupported,
                    std::string("M0 只实现 Memory 传输，收到 ") +
                        TransportName(config.transport),
                    error);
    }
    for (const ProtocolKind protocol : config.protocols) {
        if (protocol != ProtocolKind::kRaw) {
            return Fail(OmniError::kUnsupported,
                        std::string("M0 只实现 Raw 协议，收到 ") + ProtocolName(protocol),
                        error);
        }
    }
    return OmniError::kOk;
}

std::unique_ptr<NetServer> NetServer::Create(const OmniNetConfig& config, std::string* error) {
    const OmniError error_code = ValidateConfig(config, error);
    if (error_code != OmniError::kOk) {
        return nullptr;
    }
    auto impl = std::make_shared<OmniServerImpl>(config);
    impl->accept_channel = Channel<RawConnection>::Create(config.limits.inbound_capacity);
    if (!HasProtocol(config, ProtocolKind::kRaw)) {
        // M0 的 Create 门保证 protocols 只含 kRaw；这条分支只为骨架对称性保留。
        (void)impl->accept_channel->Close();
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

bool NetServer::running() const noexcept {
    return m_impl != nullptr && m_impl->running.load(std::memory_order_acquire);
}

std::uint16_t NetServer::Port() const noexcept {
    return 0;  // M0：Memory 传输不绑定端口。
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

RawServer NetServer::AsRawServer() {
    return RawServer(m_impl, m_impl != nullptr && HasProtocol(m_impl->config, ProtocolKind::kRaw));
}

LinkServer NetServer::AsLinkServer() {
    return LinkServer(m_impl, m_impl != nullptr && HasProtocol(m_impl->config, ProtocolKind::kLink));
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

}  // namespace go2cpp::bento::omni
