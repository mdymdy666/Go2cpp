/**
 * @file transport_quic.cpp
 * @brief RPC over QUIC 风格传输实现：Bento Link + 字节流切片 + 多流复用。
 *
 * @details
 * 设计取舍见 `include/go2cpp/rpc/transport_quic.hpp`。本文件只做三件事：
 *
 * 1. 把一个 `bento::Link`（UDP + 可靠通道 + 可选加密）包成「连接」，
 *    由独立接收线程驱动 `RunOnce`；
 * 2. 把每条可靠通道（channel）包成 `QuicStream`（`rpc::Transport` 的
 *    字节流语义），用 `max_chunk_bytes` 切片收发；
 * 3. 服务端在 channel 首帧到达时惰性创建 stream，并支持 Link 断链后的
 *    重建（Bento `Link::Fail` 会关闭 socket；监听方需要重开同一端口）。
 *
 * 本文件不复制任何 Bento 协议逻辑：重传、SACK、流控、分片、加密、0-RTT
 * 全部在 `go2cpp::bento` 内部。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/rpc/transport_quic.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/congestion.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/protocol.hpp"

namespace go2cpp::rpc {
namespace {

using Clock = std::chrono::steady_clock;

/// Bento 单链路可靠通道上限（见 link.hpp `kMaxChannels`）。
constexpr std::size_t kMaxStreams = 8;

/// 发送窗口满时的重试间隔：窗口由对端 ACK 推进，20µs 足够覆盖回环 RTT。
constexpr std::uint32_t kSendRetryMicros = 20;

/// 单条 STREAM 消息的硬上限（Bento `kMaxStreamMessageBytes`）。
constexpr std::size_t kMaxChunkHardLimit = bento::kMaxStreamMessageBytes;

/// 归一化 stream 数：1..8。
std::uint8_t NormalizeStreams(std::uint8_t streams) noexcept {
    if (streams == 0) {
        return 1;
    }
    return std::min<std::uint8_t>(streams, static_cast<std::uint8_t>(kMaxStreams));
}

/// 归一化切片大小：128B..Bento 单条硬上限。
std::size_t NormalizeChunkBytes(std::size_t bytes) noexcept {
    constexpr std::size_t kMinChunk = 128;
    if (bytes < kMinChunk) {
        return kMinChunk;
    }
    return std::min(bytes, kMaxChunkHardLimit);
}

/// 组装失败状态（调用方传 nullptr 时静默丢弃）。
void SetError(Status* status, StatusCode code, std::string message) {
    if (status != nullptr) {
        *status = Status(code, std::move(message));
    }
}

/// 组装成功状态。
void SetOk(Status* status) {
    if (status != nullptr) {
        *status = Status::Ok();
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// QuicStreamSlot：一条 stream 的接收队列与计数（连接与 transport 共享）
// ---------------------------------------------------------------------------

/// @brief stream 的共享状态：连接负责写入，QuicStream 负责读取。
/// @details 单独拆出来是为了让「连接」与「transport 对象」解耦：transport
///          交给 RPC Server 后可能被任意线程析构，而连接仍要能识别该 channel
///          已关闭并把后续数据丢弃，不需要持有 transport 对象。
struct QuicStreamSlot {
    mutable std::mutex mutex;              ///< 保护下面全部字段。
    std::condition_variable cv;            ///< 数据到达/关闭唤醒。
    std::deque<std::vector<std::uint8_t>> chunks;  ///< 按序到达的数据块。
    std::size_t front_offset = 0;          ///< 队首块已消费偏移。
    std::size_t buffered_bytes = 0;        ///< 未读字节数。
    bool closed = false;                   ///< 本地 Close / 连接关闭。
    bool created = false;                  ///< transport 已创建（服务端首帧/客户端 Open）。
    bool delivered = false;                ///< transport 已交给调用方。
    std::uint8_t channel = 0;              ///< 通道号。
    std::uint64_t bytes_sent = 0;          ///< 写入字节（切片后的总和）。
    std::uint64_t chunks_sent = 0;         ///< 发出的 STREAM 消息数。
    std::uint64_t bytes_received = 0;      ///< 读出字节。
    std::uint64_t chunks_received = 0;     ///< 收到的 STREAM 消息数。
    std::uint64_t write_blocks = 0;        ///< 发送窗口满次数。
};

// ---------------------------------------------------------------------------
// QuicConnection：Link 封装 + 接收线程 + stream 表
// ---------------------------------------------------------------------------

/// @brief 一条 QUIC 风格连接（客户端或监听方）。
/// @note 生命周期由 `shared_ptr` 管理；`QuicStream` 持有强引用保活。
class QuicConnection final : public std::enable_shared_from_this<QuicConnection> {
   public:
    QuicConnection(QuicOptions options, bool listener)
        : m_options(std::move(options)),
          m_listener(listener),
          m_max_chunk_bytes(NormalizeChunkBytes(m_options.max_chunk_bytes)),
          m_stream_limit(NormalizeStreams(m_options.streams)) {
        m_options.max_chunk_bytes = m_max_chunk_bytes;
        m_options.streams = m_stream_limit;
    }

    ~QuicConnection() { Close(); }

    QuicConnection(const QuicConnection&) = delete;
    QuicConnection& operator=(const QuicConnection&) = delete;

    /// @brief 打开客户端 socket 并解析远端（不握手）。
    bool OpenClient(const Endpoint& target, Status* status) {
        if (target.port == 0) {
            SetError(status, StatusCode::kInvalidArgument, "quic: target port is zero");
            return false;
        }
        if (!m_options.pre_shared_key.empty() && m_options.pre_shared_key.size() != 32) {
            SetError(status, StatusCode::kInvalidArgument,
                     "quic: pre_shared_key must be exactly 32 bytes");
            return false;
        }
        m_target = target;
        m_peer_text = target.ToString();
        auto link = std::make_shared<bento::Link>(MakeClientConfig(target));
        std::string error;
        if (!link->Open(&error)) {
            SetError(status, StatusCode::kUnavailable, "quic: open udp socket failed: " + error);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_link_mutex);
            m_link = link;
        }
        m_local_port.store(link->local_port(), std::memory_order_release);
        return true;
    }

    /// @brief 打开监听 socket（不启动接收线程）。
    bool OpenListener(const Endpoint& bind, Status* status) {
        if (!m_options.pre_shared_key.empty() && m_options.pre_shared_key.size() != 32) {
            SetError(status, StatusCode::kInvalidArgument,
                     "quic: pre_shared_key must be exactly 32 bytes");
            return false;
        }
        m_bind = bind;
        auto link = std::make_shared<bento::Link>(MakeListenerConfig(bind, bind.port));
        std::string error;
        if (!link->Open(&error)) {
            SetError(status, StatusCode::kUnavailable, "quic: bind udp socket failed: " + error);
            return false;
        }
        m_bind_port.store(link->local_port(), std::memory_order_release);
        m_local_port.store(link->local_port(), std::memory_order_release);
        InstallCallbacks(link);
        {
            std::lock_guard<std::mutex> lock(m_link_mutex);
            m_link = link;
        }
        return true;
    }

    /// @brief 客户端：0-RTT 恢复或 1-RTT 握手，并预创建 stream slot。
    bool StartClient(Status* status) {
        auto link = CurrentLink();
        if (!link) {
            SetError(status, StatusCode::kFailedPrecondition, "quic: link is not open");
            return false;
        }
        InstallCallbacks(link);
        // inline 接收只对客户端、单 stream 生效：RPC 读线程是唯一的消费者，
        // 由它直接驱动 RunOnce 省掉「接收线程 → 读线程」的一次调度。
        m_inline_receive = m_options.inline_receive && m_stream_limit == 1;
        if (!m_inline_receive && !StartReceiver()) {
            SetError(status, StatusCode::kUnavailable, "quic: failed to start receiver thread");
            return false;
        }

        bool resumed = false;
        if (m_options.enable_0rtt && !m_options.pre_shared_key.empty() &&
            !m_options.session_ticket.empty()) {
            if (link->Resume(m_options.session_ticket)) {
                resumed = true;
                m_resumed.store(true, std::memory_order_release);
                m_connected.store(true, std::memory_order_release);
            }
        }
        if (!resumed) {
            if (!link->Connect()) {
                SetError(status, StatusCode::kUnavailable,
                         "quic: handshake rejected (check PSK / encryption policy)");
                Close();
                return false;
            }
            const auto timeout = std::chrono::milliseconds(m_options.connect_timeout_ms);
            if (m_inline_receive) {
                // 没有后台接收线程：在调用线程里驱动握手收包。
                const auto deadline = Clock::now() + timeout;
                while (!m_connected.load(std::memory_order_acquire) &&
                       !m_interrupted.load(std::memory_order_acquire) &&
                       Clock::now() < deadline) {
                    link->RunOnce(1);
                }
            } else {
                std::unique_lock<std::mutex> lock(m_connect_mutex);
                m_connect_cv.wait_for(lock, timeout, [this]() {
                    return m_connected.load(std::memory_order_acquire) ||
                           m_interrupted.load(std::memory_order_acquire);
                });
            }
            if (!m_connected.load(std::memory_order_acquire)) {
                SetError(status, StatusCode::kDeadlineExceeded,
                         "quic: handshake timed out or was closed");
                Close();
                return false;
            }
        }

        // 预创建 stream slot：OpenStream 只是把 slot 与 transport 绑定，
        // 不产生额外网络往返（QUIC 的「stream 由首帧隐式打开」）。
        std::lock_guard<std::mutex> lock(m_streams_mutex);
        for (std::uint8_t index = 0; index < m_stream_limit; ++index) {
            auto slot = std::make_shared<QuicStreamSlot>();
            slot->channel = index;
            slot->created = true;
            m_slots[index] = std::move(slot);
        }
        return true;
    }

    /// @brief 启动接收线程（监听方在 `Listen` 后调用）。
    bool StartReceiver() {
        if (m_thread.joinable()) {
            return true;
        }
        if (m_running.exchange(true, std::memory_order_acq_rel)) {
            return true;
        }
        m_thread = std::thread([this]() { ReceiverLoop(); });
        return true;
    }

    /// @brief 是否已连接。
    bool connected() const noexcept {
        return m_connected.load(std::memory_order_acquire);
    }

    /// @brief 当前会话是否 0-RTT 恢复。
    bool resumed() const noexcept {
        return m_resumed.load(std::memory_order_acquire);
    }

    /// @brief 导出会话票据（无 PSK / X25519 会话返回空）。
    std::vector<std::uint8_t> SessionTicket() const {
        auto link = CurrentLink();
        if (!link) {
            return {};
        }
        return link->ExportSessionTicket();
    }

    /// @brief 打开客户端上的一条 stream（从预创建 slot 里分配）。
    std::unique_ptr<QuicStream> OpenStream(Status* status) {
        if (!connected()) {
            SetError(status, StatusCode::kUnavailable, "quic: connection is not established");
            return nullptr;
        }
        std::shared_ptr<QuicStreamSlot> slot;
        {
            std::lock_guard<std::mutex> lock(m_streams_mutex);
            for (std::uint8_t index = 0; index < m_stream_limit; ++index) {
                auto candidate = m_slots[index];
                if (candidate && !candidate->delivered) {
                    candidate->delivered = true;
                    slot = std::move(candidate);
                    break;
                }
            }
        }
        if (!slot) {
            SetError(status, StatusCode::kResourceExhausted,
                     "quic: no free stream (raise QuicOptions::streams)");
            return nullptr;
        }
        m_streams_opened.fetch_add(1, std::memory_order_relaxed);
        SetOk(status);
        // 必须先取 channel 再 move slot：函数实参的求值顺序未指定，
        // 直接在实参里写 `slot->channel` + `std::move(slot)` 会在移动后
        // 解引用空 shared_ptr（实测 -O3 直接段错误）。
        const std::uint8_t channel = slot->channel;
        return std::unique_ptr<QuicStream>(
            new QuicStream(shared_from_this(), channel, std::move(slot)));
    }

    /// @brief 服务端：新 stream 回调（在接收线程内执行）。
    void SetStreamHandler(std::function<void(std::unique_ptr<QuicStream>)> handler) {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        m_handler = std::move(handler);
    }

    /// @brief 发送一块字节（写满发送窗口时短睡眠重试）。
    bool SendChunk(std::uint8_t channel, const std::uint8_t* data, std::size_t bytes,
                   std::shared_ptr<QuicStreamSlot>* slot_out, Status* status) {
        if (m_interrupted.load(std::memory_order_acquire)) {
            SetError(status, StatusCode::kUnavailable, "quic: connection is closed");
            return false;
        }
        auto slot = SlotFor(channel);
        if (!slot) {
            SetError(status, StatusCode::kFailedPrecondition, "quic: stream is not open");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(slot->mutex);
            if (slot->closed) {
                SetError(status, StatusCode::kUnavailable, "quic: stream is closed");
                return false;
            }
        }
        if (bytes > m_max_chunk_bytes) {
            SetError(status, StatusCode::kInvalidArgument, "quic: chunk exceeds max_chunk_bytes");
            return false;
        }
        auto link = CurrentLink();
        if (!link) {
            SetError(status, StatusCode::kUnavailable, "quic: link is not available");
            return false;
        }

        const auto deadline =
            Clock::now() + std::chrono::milliseconds(m_options.send_timeout_ms);
        for (;;) {
            const bento::SendStatus result = link->TrySendStream(channel, data, bytes);
            if (result == bento::SendStatus::kOk) {
                std::lock_guard<std::mutex> lock(slot->mutex);
                slot->bytes_sent += bytes;
                ++slot->chunks_sent;
                if (slot_out != nullptr) {
                    *slot_out = std::move(slot);
                }
                SetOk(status);
                return true;
            }
            if (result == bento::SendStatus::kError) {
                SetError(status, StatusCode::kUnavailable,
                         "quic: reliable channel rejected the write");
                return false;
            }
            // WouldBlock：发送窗口/流控窗口满，等接收线程处理 ACK 后重试。
            {
                std::lock_guard<std::mutex> lock(slot->mutex);
                ++slot->write_blocks;
            }
            if (Clock::now() >= deadline) {
                SetError(status, StatusCode::kDeadlineExceeded,
                         "quic: send window full, write timed out");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(kSendRetryMicros));
        }
    }

    /// @brief 连接级统计快照。
    QuicStats Stats() const {
        QuicStats stats;
        stats.streams_opened = m_streams_opened.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_stats_mutex);
            stats.handshakes = m_handshakes;
        }
        {
            std::lock_guard<std::mutex> lock(m_streams_mutex);
            for (const auto& slot : m_slots) {
                if (!slot) {
                    continue;
                }
                std::lock_guard<std::mutex> slot_lock(slot->mutex);
                stats.bytes_sent += slot->bytes_sent;
                stats.bytes_received += slot->bytes_received;
                stats.chunks_sent += slot->chunks_sent;
                stats.chunks_received += slot->chunks_received;
                stats.write_blocks += slot->write_blocks;
            }
        }
        auto link = CurrentLink();
        if (link) {
            const bento::LinkStats link_stats = link->Stats();
            stats.frames_sent = link_stats.frames_sent;
            stats.frames_received = link_stats.frames_received;
            stats.retransmits = link_stats.retransmits;
            stats.srtt_ms = link_stats.srtt_ms;
            stats.pto_ms = link_stats.pto_ms;
            stats.handshakes = link_stats.handshakes;
            stats.resumptions = link->secure_stats().resumptions;
        }
        return stats;
    }

    /// @brief 关闭连接（幂等；停线程、关 Link、唤醒所有 stream 读者）。
    void Close() {
        if (m_interrupted.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        m_running.store(false, std::memory_order_release);
        auto link = CurrentLink();
        if (link) {
            link->Close();
        }
        CloseAllSlots("connection closed");
        m_connected.store(false, std::memory_order_release);
        m_connect_cv.notify_all();
        if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) {
            m_thread.join();
        } else if (m_thread.joinable()) {
            m_thread.detach();
        }
    }

    std::uint16_t local_port() const noexcept {
        return m_local_port.load(std::memory_order_acquire);
    }
    std::string peer_text() const { return m_peer_text; }
    std::size_t max_chunk_bytes() const noexcept { return m_max_chunk_bytes; }
    std::uint64_t stream_limit() const noexcept { return m_stream_limit; }
    std::uint32_t read_spin_us() const noexcept { return m_options.read_spin_us; }
    bool inline_receive() const noexcept { return m_inline_receive; }

    /// @brief inline 接收：由 Read 调用方驱动一次非阻塞 RunOnce。
    void PumpInline() {
        auto link = CurrentLink();
        if (link) {
            link->RunOnce(0);
        }
    }

   private:
    /// @brief 构造 Bento Link 配置（客户端）。
    bento::LinkConfig MakeClientConfig(const Endpoint& target) const {
        bento::LinkConfig config;
        config.bind_address = m_options.bind_address;
        config.bind_port = 0;
        config.remote = target.host.empty() ? "127.0.0.1" : target.host;
        config.peer_address = config.remote;
        config.peer_port = target.port;
        ApplyCommonConfig(&config);
        return config;
    }

    /// @brief 构造 Bento Link 配置（监听方）。
    bento::LinkConfig MakeListenerConfig(const Endpoint& bind, std::uint16_t port) const {
        bento::LinkConfig config;
        config.bind_address = bind.host.empty() ? "0.0.0.0" : bind.host;
        config.bind_port = port;
        ApplyCommonConfig(&config);
        return config;
    }

    /// @brief 两个方向共享的 Link 配置：超时、ACK、流控、拥塞档位、加密策略。
    void ApplyCommonConfig(bento::LinkConfig* config) const {
        config->handshake_timeout_ms = m_options.connect_timeout_ms;
        config->peer_timeout_ms = m_options.peer_timeout_ms;
        config->heartbeat_interval_ms = m_options.heartbeat_interval_ms;
        config->ack_interval_ms = m_options.ack_interval_ms;
        config->session_ticket_lifetime_ms = m_options.session_ticket_lifetime_ms;
        config->reliable.initial_max_data =
            static_cast<std::uint32_t>(std::min<std::size_t>(m_options.initial_max_data,
                                                             0xFFFFFFFFu));
        config->reliable.peer_initial_max_data = config->reliable.initial_max_data;
        config->reliable.max_data_refresh_ms = 20;
        if (m_options.mode == QuicMode::kLowLatency) {
            // 回环/同机房：BBR 的初始 pacing（64 包 × 1200B / 100ms ≈ 768KB/s）
            // 会把小消息串行化；关闭后由 256 条发送窗口与 PTO 兜底。
            config->reliable.congestion.algorithm = bento::CongestionAlgorithm::kNone;
            config->reliable.congestion.enabled = false;
            config->reliable.congestion.pacing = false;
            // 回环 RTT 只有几十微秒：PTO 下限从 50ms 降到 10ms 后，
            // 单帧丢失（同步 RPC 无法触发快速重传）的恢复时间从 50ms
            // 降到 10ms 量级；仍远大于 RTT，不会制造伪超时。
            config->reliable.rtt.min_pto_ms = 10;
            config->reliable.rtt.max_ack_delay_ms = 2;
        }
        if (!m_options.pre_shared_key.empty()) {
            config->pre_shared_key = m_options.pre_shared_key;
            config->require_encryption = true;
            config->enable_x25519 = m_options.enable_x25519;
            // 纯 PSK 是显式降级（无前向保密），Bento 要求显式确认。
            config->allow_legacy_peer = !m_options.enable_x25519;
        } else {
            // 无 PSK：明文模式（Bento 默认会拒绝明文握手）。
            config->require_encryption = false;
            config->allow_legacy_peer = true;
        }
    }

    /// @brief 取当前 Link 的强引用快照（服务端重建时可能替换）。
    std::shared_ptr<bento::Link> CurrentLink() const {
        std::lock_guard<std::mutex> lock(m_link_mutex);
        return m_link;
    }

    /// @brief 给 Link 挂回调；回调只在 Link 存活期间被调用。
    void InstallCallbacks(const std::shared_ptr<bento::Link>& link) {
        link->on_connected = [this]() { HandleConnected(); };
        link->on_disconnected = [this](const char* reason) { HandleDisconnected(reason); };
        link->on_stream = [this](std::uint8_t channel, const std::uint8_t* data,
                                 std::size_t bytes) { HandleStream(channel, data, bytes); };
    }

    void HandleConnected() {
        {
            std::lock_guard<std::mutex> lock(m_stats_mutex);
            ++m_handshakes;
        }
        m_connected.store(true, std::memory_order_release);
        m_connect_cv.notify_all();
    }

    void HandleDisconnected(const char* /*reason*/) {
        m_connected.store(false, std::memory_order_release);
        m_connect_cv.notify_all();
    }

    /// @brief 收到某一 channel 的完整 STREAM 消息。
    /// @details 服务端在没有对应 transport 时惰性创建并交付（1-RTT 与 0-RTT
    ///          恢复共用此路径）。
    void HandleStream(std::uint8_t channel, const std::uint8_t* data, std::size_t bytes) {
        if (channel >= kMaxStreams) {
            return;
        }
        std::shared_ptr<QuicStreamSlot> slot;
        std::unique_ptr<QuicStream> transport;
        {
            std::lock_guard<std::mutex> lock(m_streams_mutex);
            slot = m_slots[channel];
            if (!slot) {
                if (!m_listener) {
                    return;  // 客户端没打开该 channel：丢弃。
                }
                slot = std::make_shared<QuicStreamSlot>();
                slot->channel = channel;
                slot->created = true;
                slot->delivered = true;
                m_slots[channel] = slot;
                m_streams_opened.fetch_add(1, std::memory_order_relaxed);
                transport.reset(new QuicStream(shared_from_this(), channel, slot));
            }
        }
        if (transport) {
            std::function<void(std::unique_ptr<QuicStream>)> handler;
            {
                std::lock_guard<std::mutex> lock(m_handler_mutex);
                handler = m_handler;
            }
            if (handler) {
                try {
                    handler(std::move(transport));
                } catch (...) {
                    // 用户回调异常不能打断接收线程；该 stream 保持无主，
                    // 后续数据仍入 slot（无人读取，连接关闭时统一回收）。
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(slot->mutex);
            if (slot->closed) {
                return;
            }
            slot->chunks.emplace_back(data, data + bytes);
            slot->buffered_bytes += bytes;
            ++slot->chunks_received;
        }
        slot->cv.notify_one();
    }

    /// @brief 接收线程主循环：驱动 Link，监听方断链后重建.
    void ReceiverLoop() {
        while (m_running.load(std::memory_order_acquire)) {
            auto link = CurrentLink();
            if (!link) {
                break;
            }
            if (m_listener) {
                const bento::LinkState state = link->state();
                if (state == bento::LinkState::kTimedOut ||
                    state == bento::LinkState::kClosed) {
                    RebuildListenerLink();
                    continue;
                }
            }
            link->RunOnce(m_options.busy_poll ? 0 : static_cast<int>(m_options.poll_timeout_ms));
            if (m_options.busy_poll) {
                // 忙轮询：下一轮立刻检查 socket，避免 poll 睡眠/唤醒的调度延迟。
                std::this_thread::yield();
            }
            if (!m_listener) {
                const bento::LinkState state = link->state();
                if (state == bento::LinkState::kTimedOut ||
                    state == bento::LinkState::kClosed) {
                    m_connected.store(false, std::memory_order_release);
                    m_connect_cv.notify_all();
                    break;
                }
            }
        }
        m_running.store(false, std::memory_order_release);
    }

    /// @brief 监听方：Link 超时（Bento 会关闭 socket）后用同一端口重建。
    void RebuildListenerLink() {
        auto old = CurrentLink();
        if (old) {
            old->Close();
        }
        CloseAllSlots("link rebuild");
        const std::uint16_t port = m_bind_port.load(std::memory_order_acquire);
        auto link = std::make_shared<bento::Link>(MakeListenerConfig(m_bind, port));
        std::string error;
        if (!link->Open(&error)) {
            m_running.store(false, std::memory_order_release);
            return;
        }
        InstallCallbacks(link);
        {
            std::lock_guard<std::mutex> lock(m_link_mutex);
            m_link = link;
        }
        m_connected.store(false, std::memory_order_release);
    }

    /// @brief 取 channel 对应的 slot（存在才返回）。
    std::shared_ptr<QuicStreamSlot> SlotFor(std::uint8_t channel) const {
        std::lock_guard<std::mutex> lock(m_streams_mutex);
        if (channel >= kMaxStreams) {
            return nullptr;
        }
        return m_slots[channel];
    }

    /// @brief 关闭并清空所有 slot（旧 transport 的 Read 会立即返回 0）。
    void CloseAllSlots(const char* /*reason*/) {
        std::array<std::shared_ptr<QuicStreamSlot>, kMaxStreams> retired{};
        {
            std::lock_guard<std::mutex> lock(m_streams_mutex);
            retired.swap(m_slots);
            m_slots.fill(nullptr);
        }
        for (const auto& slot : retired) {
            if (!slot) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(slot->mutex);
                slot->closed = true;
            }
            slot->cv.notify_all();
        }
    }

    QuicOptions m_options;
    const bool m_listener;
    const std::size_t m_max_chunk_bytes;
    const std::uint8_t m_stream_limit;

    mutable std::mutex m_link_mutex;             ///< 保护 m_link 指针。
    std::shared_ptr<bento::Link> m_link;         ///< 当前 Link（服务端会重建）。
    mutable std::mutex m_streams_mutex;          ///< 保护 m_slots / m_streams_opened。
    std::array<std::shared_ptr<QuicStreamSlot>, kMaxStreams> m_slots{};
    mutable std::mutex m_handler_mutex;          ///< 保护 m_handler。
    std::function<void(std::unique_ptr<QuicStream>)> m_handler;

    mutable std::mutex m_stats_mutex;            ///< 保护 m_handshakes。
    std::uint64_t m_handshakes = 0;

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_resumed{false};
    std::atomic<bool> m_interrupted{false};
    std::atomic<bool> m_running{false};
    bool m_inline_receive = false;  ///< 客户端单 stream 由 Read 驱动收包。
    std::atomic<std::uint64_t> m_streams_opened{0};
    std::atomic<std::uint16_t> m_local_port{0};
    std::atomic<std::uint16_t> m_bind_port{0};
    std::mutex m_connect_mutex;
    std::condition_variable m_connect_cv;
    std::thread m_thread;
    Endpoint m_target;
    Endpoint m_bind;
    std::string m_peer_text;
};

// ---------------------------------------------------------------------------
// QuicStream
// ---------------------------------------------------------------------------

QuicStream::QuicStream(std::shared_ptr<QuicConnection> connection, std::uint8_t channel,
                       std::shared_ptr<QuicStreamSlot> slot)
    : m_connection(std::move(connection)),
      m_slot(std::move(slot)),
      m_channel(channel),
      m_read_spin_us(m_connection ? m_connection->read_spin_us() : 0) {}

QuicStream::~QuicStream() { MarkClosed(); }

bool QuicStream::WriteAll(const void* data, std::size_t size, Status* status) {
    if (size == 0) {
        SetOk(status);
        return true;
    }
    if (data == nullptr) {
        SetError(status, StatusCode::kInvalidArgument, "quic: null write buffer");
        return false;
    }
    if (!m_connection) {
        SetError(status, StatusCode::kUnavailable, "quic: connection is gone");
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const std::size_t chunk = m_connection->max_chunk_bytes();
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t take = std::min(chunk, size - offset);
        if (!m_connection->SendChunk(m_channel, bytes + offset, take, nullptr, status)) {
            return false;
        }
        offset += take;
    }
    SetOk(status);
    return true;
}

std::ptrdiff_t QuicStream::Read(void* data, std::size_t size, Status* status) {
    if (data == nullptr && size != 0) {
        SetError(status, StatusCode::kInvalidArgument, "quic: null read buffer");
        return -1;
    }
    if (size == 0) {
        SetOk(status);
        return 0;
    }
    auto* out = static_cast<std::uint8_t*>(data);
    const bool inline_receive = m_connection != nullptr && m_connection->inline_receive();
    std::unique_lock<std::mutex> lock(m_slot->mutex);
    if (!inline_receive && m_slot->chunks.empty() && !m_slot->closed &&
        m_read_spin_us > 0) {
        // 短自旋：接收线程把数据推进队列后这里直接取走，不必经过
        // condition_variable 的 futex 唤醒（回环上 5~15µs）。
        const auto deadline =
            Clock::now() + std::chrono::microseconds(m_read_spin_us);
        while (m_slot->chunks.empty() && !m_slot->closed && Clock::now() < deadline) {
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
        }
    }
    while (m_slot->chunks.empty() && !m_slot->closed) {
        if (inline_receive) {
            // 读线程自己驱动收包：响应到达 socket 后直接在本线程处理，
            // 不经过「接收线程唤醒 → notify 读线程」的一跳。
            lock.unlock();
            m_connection->PumpInline();
            lock.lock();
        } else {
            m_slot->cv.wait(lock);
        }
    }
    if (m_slot->chunks.empty()) {
        // 连接关闭/本地 Close：返回 0 表示 EOF（RPC 读线程据此退出）。
        SetOk(status);
        return 0;
    }
    std::size_t copied = 0;
    while (copied < size && !m_slot->chunks.empty()) {
        std::vector<std::uint8_t>& front = m_slot->chunks.front();
        const std::size_t available = front.size() - m_slot->front_offset;
        const std::size_t take = std::min(available, size - copied);
        std::memcpy(out + copied, front.data() + m_slot->front_offset, take);
        m_slot->front_offset += take;
        copied += take;
        if (m_slot->front_offset == front.size()) {
            m_slot->chunks.pop_front();
            m_slot->front_offset = 0;
        }
    }
    m_slot->buffered_bytes -= copied;
    m_slot->bytes_received += copied;
    lock.unlock();
    SetOk(status);
    return static_cast<std::ptrdiff_t>(copied);
}

void QuicStream::Close() noexcept {
    if (!m_slot) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_slot->mutex);
        if (m_slot->closed) {
            return;
        }
        m_slot->closed = true;
    }
    m_slot->cv.notify_all();
}

bool QuicStream::closed() const {
    if (!m_slot) {
        return true;
    }
    std::lock_guard<std::mutex> lock(m_slot->mutex);
    return m_slot->closed;
}

QuicStats QuicStream::stats() const {
    QuicStats stats = m_connection ? m_connection->Stats() : QuicStats{};
    if (m_slot) {
        std::lock_guard<std::mutex> lock(m_slot->mutex);
        stats.bytes_sent = m_slot->bytes_sent;
        stats.bytes_received = m_slot->bytes_received;
        stats.chunks_sent = m_slot->chunks_sent;
        stats.chunks_received = m_slot->chunks_received;
        stats.write_blocks = m_slot->write_blocks;
    }
    return stats;
}

void QuicStream::PushData(const std::uint8_t* data, std::size_t bytes) {
    if (!m_slot) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_slot->mutex);
        if (m_slot->closed) {
            return;
        }
        m_slot->chunks.emplace_back(data, data + bytes);
        m_slot->buffered_bytes += bytes;
        ++m_slot->chunks_received;
    }
    m_slot->cv.notify_one();
}

void QuicStream::MarkClosed() {
    if (!m_slot) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_slot->mutex);
        m_slot->closed = true;
    }
    m_slot->cv.notify_all();
}

// ---------------------------------------------------------------------------
// QuicClient
// ---------------------------------------------------------------------------

QuicClient::QuicClient() = default;

QuicClient::~QuicClient() { Close(); }

std::unique_ptr<QuicClient> QuicClient::Dial(const Endpoint& target, const QuicOptions& options,
                                             Status* status) {
    auto connection = std::make_shared<QuicConnection>(options, false);
    if (!connection->OpenClient(target, status)) {
        return nullptr;
    }
    if (!connection->StartClient(status)) {
        return nullptr;
    }
    auto client = std::unique_ptr<QuicClient>(new QuicClient());
    client->m_connection = std::move(connection);
    SetOk(status);
    return client;
}

std::unique_ptr<QuicStream> QuicClient::OpenStream(Status* status) {
    if (!m_connection) {
        SetError(status, StatusCode::kUnavailable, "quic: client is not connected");
        return nullptr;
    }
    return m_connection->OpenStream(status);
}

bool QuicClient::connected() const {
    return m_connection && m_connection->connected();
}

bool QuicClient::resumed() const { return m_connection && m_connection->resumed(); }

std::vector<std::uint8_t> QuicClient::session_ticket() const {
    return m_connection ? m_connection->SessionTicket() : std::vector<std::uint8_t>{};
}

QuicStats QuicClient::stats() const { return m_connection ? m_connection->Stats() : QuicStats{}; }

std::uint16_t QuicClient::local_port() const {
    return m_connection ? m_connection->local_port() : 0;
}

std::string QuicClient::peer_address() const {
    return m_connection ? m_connection->peer_text() : std::string{};
}

void QuicClient::Close() {
    if (m_connection) {
        m_connection->Close();
    }
}

// ---------------------------------------------------------------------------
// QuicListener
// ---------------------------------------------------------------------------

struct QuicListener::Impl {
    QuicOptions options;
    Endpoint bind;
    std::shared_ptr<QuicConnection> connection;
    QuicListener::StreamHandler handler;
    mutable std::mutex handler_mutex;
    std::atomic<bool> running{false};
    std::uint16_t port = 0;
};

QuicListener::QuicListener() : m_impl(std::make_unique<Impl>()) {}

QuicListener::~QuicListener() { Stop(); }

bool QuicListener::Listen(const Endpoint& bind, const QuicOptions& options, Status* status) {
    if (m_impl->running.load(std::memory_order_acquire)) {
        SetError(status, StatusCode::kFailedPrecondition, "quic: listener already running");
        return false;
    }
    auto connection = std::make_shared<QuicConnection>(options, true);
    if (!connection->OpenListener(bind, status)) {
        return false;
    }
    connection->SetStreamHandler([this](std::unique_ptr<QuicStream> stream) {
        StreamHandler handler;
        {
            std::lock_guard<std::mutex> lock(m_impl->handler_mutex);
            handler = m_impl->handler;
        }
        if (handler) {
            handler(std::move(stream));
        }
    });
    m_impl->connection = std::move(connection);
    m_impl->options = options;
    m_impl->bind = bind;
    m_impl->port = m_impl->connection->local_port();
    SetOk(status);
    return true;
}

void QuicListener::OnStream(StreamHandler handler) {
    std::lock_guard<std::mutex> lock(m_impl->handler_mutex);
    m_impl->handler = std::move(handler);
}

bool QuicListener::Start() {
    if (!m_impl->connection) {
        return false;
    }
    if (m_impl->running.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    if (!m_impl->connection->StartReceiver()) {
        m_impl->running.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void QuicListener::Stop() {
    if (!m_impl) {
        return;
    }
    m_impl->running.store(false, std::memory_order_release);
    if (m_impl->connection) {
        m_impl->connection->Close();
    }
}

bool QuicListener::running() const {
    return m_impl && m_impl->running.load(std::memory_order_acquire);
}

std::uint16_t QuicListener::port() const { return m_impl ? m_impl->port : 0; }

QuicStats QuicListener::stats() const {
    return m_impl && m_impl->connection ? m_impl->connection->Stats() : QuicStats{};
}

}  // namespace go2cpp::rpc
