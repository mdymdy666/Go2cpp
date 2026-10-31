#pragma once

/**
 * @file connection.hpp
 * @brief 应用层连接：握手、多路复用、流控、优先级、事件与资源/KV 发送。
 *
 * @details `Connection` 把 `FrameDecoder`（线格式）、`StreamTable`（状态/窗口/
 *          调度）、`ResourceReceiver`/`KvAssembler`（业务重组）串成一个可直接
 *          使用的连接。**它不关心字节从哪里来**——传输通过 `ITransport` 注入，
 *          因此同一份逻辑可以跑在内存管道、Bento Link（UDP 可靠+不可靠）或
 *          HTTP 隧道（TCP）上。
 *
 * ## 握手（HTTP/2 风格）
 *
 *          双方各发 8 字节 preface（`APP2\r\n\r\n`）与一帧 SETTINGS；收到
 *          SETTINGS 后回 ACK。preface 的作用不是安全，而是**在一条字节流上
 *          明确协议归属**：把应用层帧流误接到别的端口时会在前 8 字节失败，
 *          而不是被当成垃圾帧继续解析。
 *
 * ## 并发模型
 *
 *          - `Step()` / `ReadEvent()` 是 IO 驱动入口，**必须由单一 IO 线程/
 *            Fiber 调用**（二者也会互斥）；
 *          - `SendKv*` / `SendResource*` / `SendBytes` / `ResetStream` 等发送
 *            API 可以在任意线程调用（内部用一把 IO 锁串行化）；
 *          - `ReadEvent()` 从事件队列取事件；事件队列有独立锁，等待时释放
 *            IO 锁，因此另一个线程的发送调用不会被阻塞。
 *
 *          这是「单驱动者接收 + 多线程发送」的折中：HTTP/2 库（如 Go 的
 *          `x/net/http2`）也采用同一模型。**不要**让两个线程同时 `Step`。
 *
 * ## 发送模型与背压
 *
 *          `SendKv*` / `SendResource*` 只**登记**流与生产者，不立刻写 socket；
 *          真正的发送在 `Step()` 的 `Flush` 阶段按优先级调度。每次 Step 的
 *          发送预算是 `max_bytes_per_flush`（默认 2 MiB），避免持续发送饿死
 *          接收与定时器。窗口不足时流保持 pending，等对端 WINDOW_UPDATE 后
 *          在下一次 Step 继续——不需要调用方轮询重试。
 *
 * ## 资源接收
 *
 *          资源流默认在内部消费（不投递逐块事件），完成时投递
 *          `kResourceComplete`；`SetResourceSinkFactory` 可把数据落到文件。
 *          即使不设工厂，接收侧仍会计算 SHA-256 并校验，绝不会静默丢弃。
 *
 * ## 事件
 *
 *          | 事件 | 触发 |
 *          |------|------|
 *          | `kKvComplete` | 一条 KV 流结束（表已合并完成） |
 *          | `kResourceComplete` | 一条资源流结束且校验通过/失败 |
 *          | `kResourceIndex` | 收到资源索引帧 |
 *          | `kData` / `kEndStream` | `kBytes` 流的裸数据/结束 |
 *          | `kHeaders` | `kBytes`/控制流的元数据 |
 *          | `kReset` / `kGoAway` / `kPing` / `kSettings` | 控制帧 |
 *          | `kClosed` | 连接关闭 |
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/app/frame.hpp"
#include "go2cpp/app/kv.hpp"
#include "go2cpp/app/kv_stream.hpp"
#include "go2cpp/app/resource.hpp"
#include "go2cpp/app/stream_table.hpp"
#include "go2cpp/app/transport.hpp"

namespace go2cpp::app {

/// @brief 连接角色。决定本端发起的流 id 奇偶（客户端奇数、服务端偶数）。
enum class Role : std::uint8_t {
    kClient = 0,  ///< 客户端。
    kServer = 1,  ///< 服务端。
};

/// @brief 角色名。
const char* RoleName(Role role) noexcept;

/// @brief 连接配置。
struct ConnectionConfig {
    StreamTableConfig table;                              ///< 流表配置。
    std::uint32_t max_frame_payload = 256u * 1024u;       ///< 单帧载荷上限（本端发送）。
    std::uint32_t small_kv_threshold = 4096;              ///< 小 KV 阈值。
    std::uint32_t small_value_limit = 4096;               ///< 单值进 HEADERS 上限。
    std::uint32_t resource_chunk_size = kDefaultResourceChunkSize;  ///< 资源块大小。
    std::uint32_t ping_interval_ms = 0;                   ///< 自动 PING 间隔（0=关闭）。
    std::uint32_t read_chunk_bytes = 64u * 1024u;         ///< 单次 Read 容量。
    std::uint32_t max_frames_per_step = 64;               ///< 单次 Step 最多处理帧数。
    std::uint32_t max_bytes_per_flush = 2u * 1024u * 1024u;  ///< 单次 Step 发送预算。
    bool auto_window_update = true;                       ///< 收到数据后自动归还窗口。
    bool auto_settings_ack = true;                        ///< 自动 ACK SETTINGS。
    bool auto_close_ack = true;                           ///< 接收完成时回 END_STREAM 确认。
};

/// @brief 事件类型。
enum class EventType : std::uint8_t {
    kHeaders = 0,          ///< 元数据（bytes/控制流）。
    kData = 1,             ///< 裸数据（bytes 流）。
    kEndStream = 2,        ///< 裸数据结束（bytes 流）。
    kReset = 3,            ///< 流被重置。
    kGoAway = 4,           ///< 对端 GOAWAY。
    kPing = 5,             ///< PING。
    kSettings = 6,         ///< SETTINGS。
    kKvComplete = 7,       ///< KV 流完成。
    kResourceComplete = 8, ///< 资源流完成（含校验结果）。
    kResourceIndex = 9,    ///< 资源索引。
    kClosed = 10,          ///< 连接关闭。
    kError = 11,           ///< 连接/协议错误。
    kWindowUpdate = 12,    ///< 收到 WINDOW_UPDATE。
};

/// @brief 事件类型名。
const char* EventTypeName(EventType type) noexcept;

/// @brief 一条连接事件。
struct Event {
    EventType type = EventType::kClosed;    ///< 类型。
    std::uint32_t stream_id = 0;            ///< 相关流。
    KvTable headers;                        ///< 元数据（kv 完成时为完整表）。
    std::vector<std::uint8_t> data;         ///< 裸数据。
    ErrorCode error_code = ErrorCode::kNoError;  ///< 错误码。
    std::string message;                    ///< 可读消息。
    std::array<std::uint8_t, 8> ping{};     ///< PING 载荷。
    ResourceMeta resource_meta;             ///< 资源元数据。
    ResourceStats resource_stats;           ///< 资源接收统计。
    ResourceIndex resource_index;           ///< 资源索引。
    std::uint64_t bytes = 0;                ///< 通用字节数（kData/kEndStream）。
    bool ok = true;                         ///< 业务结果（资源校验等）。

    /// @brief 事件名（含流 id 的简短形式）。
    std::string Describe() const;
};

/// @brief 发送 API 的结果。
struct SendResult {
    ErrorCode error = ErrorCode::kNoError;  ///< 错误码。
    std::uint32_t stream_id = 0;            ///< 分配的流 id（0=无）。
    std::string message;                    ///< 可读消息。

    /// @brief 是否成功。
    bool ok() const noexcept { return error == ErrorCode::kNoError; }
};

/// @brief 连接统计。
struct ConnectionStats {
    std::uint64_t frames_sent = 0;          ///< 发送帧数。
    std::uint64_t frames_received = 0;      ///< 接收帧数。
    std::uint64_t bytes_sent = 0;           ///< 发送载荷字节。
    std::uint64_t bytes_received = 0;       ///< 接收载荷字节。
    std::uint64_t streams_opened = 0;       ///< 本端打开的流。
    std::uint64_t streams_completed = 0;    ///< 完成的流。
    std::uint64_t resets = 0;               ///< RST_STREAM 次数。
    std::uint64_t flow_control_blocks = 0;  ///< 因窗口阻塞的调度次数。
    std::uint64_t window_updates_sent = 0;  ///< 发送的 WINDOW_UPDATE 数。
    std::uint64_t ping_sent = 0;            ///< 发送 PING 数。
    std::uint64_t ping_received = 0;        ///< 收到 PING 数。
    std::uint64_t kv_streams = 0;           ///< KV 流数量。
    std::uint64_t resource_streams = 0;     ///< 资源流数量。
    std::uint64_t bytes_streams = 0;        ///< 字节流数量。
    std::uint64_t flush_rounds = 0;         ///< Flush 轮次。
};

/**
 * @brief 应用层连接。
 *
 * @note 生命周期：构造 → `Start()` → 循环 `Step()`/`ReadEvent()` → `Close()`。
 *       析构会自动关闭。
 */
class Connection {
   public:
    /**
     * @brief 构造。
     * @param transport 字节流传输（所有权转移）。
     * @param role 角色。
     * @param config 配置。
     */
    Connection(TransportPtr transport, Role role, ConnectionConfig config = {});

    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /**
     * @brief 发送 preface + SETTINGS。
     * @return 成功返回 true；传输写失败返回 false。
     */
    bool Start(std::string* error = nullptr);

    /**
     * @brief 驱动一轮：读一帧（最多 `max_frames_per_step`）、处理、调度发送。
     * @param timeout_ms 读超时（0=非阻塞）。
     * @return 本轮处理的帧数；-1 表示连接已关闭。
     */
    int Step(std::uint32_t timeout_ms);

    /**
     * @brief 等待并取出一个事件（内部会调用 `Step`）。
     * @param out 输出事件。
     * @param timeout_ms 总等待预算。
     * @return 取到事件返回 true；超时/关闭返回 false。
     */
    bool ReadEvent(Event* out, std::uint32_t timeout_ms);

    /// @brief 设置事件回调（在 `Step` 线程同步调用；不需要队列时可只用它）。
    void SetEventHandler(std::function<void(const Event&)> handler);

    /// @brief 主动关闭连接（幂等）。
    void Close(ErrorCode code = ErrorCode::kNoError,
               std::string message = {});

    /// @brief 是否已关闭。
    bool closed() const;

    /// @brief 是否已完成握手（收到对端 preface + SETTINGS）。
    bool ready() const;

    // ---- 发送 API（线程安全） ----

    /// @brief 打开一条空流（稍后用 SendHeaders/SendData）。
    SendResult OpenStream(StreamKind kind, std::uint16_t weight = 0,
                          std::uint32_t parent_id = 0);

    /// @brief 发送一张 KV 表（自动选择单帧/流式）。
    SendResult SendKv(const KvTable& table, std::uint16_t weight = 0);

    /// @brief 解析并发送 `k1=v1;...` 文本。
    SendResult SendKvText(std::string_view text, std::uint16_t weight = 0);

    /// @brief 每个键一条独立流（并发）。
    SendResult SendKvPerKey(const KvTable& table, std::vector<std::uint32_t>* stream_ids = nullptr,
                            std::uint16_t weight = 0);

    /**
     * @brief 发送资源。
     * @param meta 元数据（sha256 可为空；空时由本端补算）。
     * @param source 数据源（共享所有权）。
     * @param start_chunk 断点续传起始块。
     * @param weight 优先级权重。
     */
    SendResult SendResource(ResourceMeta meta, std::shared_ptr<ChunkSource> source,
                            std::uint32_t start_chunk = 0, std::uint16_t weight = 0);

    /// @brief 发送裸字节流（可选元数据）。
    SendResult SendBytes(std::vector<std::uint8_t> data, const KvTable& meta = {},
                         std::uint16_t weight = 0,
                         StreamKind kind = StreamKind::kBytes);

    /// @brief 在已打开的流上发送 HEADERS。
    SendResult SendHeaders(std::uint32_t stream_id, const KvTable& headers,
                           bool end_stream);

    /// @brief 在已打开的流上发送 DATA。
    SendResult SendData(std::uint32_t stream_id, const std::uint8_t* data,
                        std::size_t size, bool end_stream);

    /// @brief 取消一条流。
    SendResult ResetStream(std::uint32_t stream_id,
                           ErrorCode error = ErrorCode::kCancel);

    /// @brief 发送 PRIORITY。
    SendResult SendPriority(std::uint32_t stream_id, std::uint32_t parent_id,
                            std::uint16_t weight);

    /// @brief 发送资源索引。
    SendResult SendResourceIndex(const ResourceIndex& index);

    /// @brief 发送 PING。
    SendResult Ping(const std::array<std::uint8_t, 8>& opaque = {});

    /// @brief 发送 SETTINGS。
    SendResult SendSettings(const std::vector<SettingsEntry>& entries);

    /// @brief 发送 GOAWAY（之后本端不再打开新流）。
    SendResult SendGoAway(std::uint32_t last_stream_id,
                          ErrorCode error = ErrorCode::kNoError,
                          std::string debug = {});

    // ---- 接收侧配置 ----

    /**
     * @brief 资源落点工厂（按元数据创建 sink）。
     * @note 在 `Start()` 之前设置；工厂在 IO 线程调用。
     */
    void SetResourceSinkFactory(
        std::function<std::unique_ptr<ResourceSink>(const ResourceMeta&)> factory);

    /// @brief 手动归还接收窗口（`auto_window_update=false` 时用）。
    bool Acknowledge(std::uint32_t stream_id, std::size_t bytes);

    // ---- 访问器 ----

    /// @brief 流表（只读；修改请走发送 API）。
    const StreamTable& table() const;
    /// @brief 统计快照。
    ConnectionStats stats() const;
    /// @brief 角色。
    Role role() const noexcept { return m_role; }
    /// @brief 配置。
    const ConnectionConfig& config() const noexcept { return m_config; }
    /// @brief 已收到 GOAWAY。
    bool goaway_received() const;

   private:
    // ---- 内部实现 ----
    void Publish(Event event);
    void FailConnection(ErrorCode code, std::string message);
    bool WriteFrame(const Frame& frame);
    void HandleFrame(const Frame& frame);
    void HandleHeaders(const Frame& frame);
    void HandleData(const Frame& frame);
    void HandleRstStream(const Frame& frame);
    void HandleSettings(const Frame& frame);
    void HandlePing(const Frame& frame);
    void HandleGoAway(const Frame& frame);
    void HandleWindowUpdate(const Frame& frame);
    void HandlePriority(const Frame& frame);
    void HandleResourceIndex(const Frame& frame);
    void FlushLocked();
    void EmitWindowUpdates();
    void ApplyLocalSettings();
    void CompleteReceive(Stream& stream);
    void CleanupStream(std::uint32_t stream_id);
    void CloseInternal(ErrorCode code, std::string message, bool notify);
    SendResult EnqueueKv(const KvTable& table, KvMode mode, std::uint16_t weight,
                         std::vector<std::uint32_t>* ids);
    void SendPendingPingLocked(std::uint64_t now_ms);
    bool EnsureStartedLocked(std::string* error);
    void MaybeCompleteKv(Stream& stream, bool end_stream);
    void MaybeCompleteResource(Stream& stream, bool end_stream);
    SendResult Fail(ErrorCode code, std::string message) const;

    TransportPtr m_transport;                 ///< 传输。
    Role m_role;                              ///< 角色。
    ConnectionConfig m_config;                ///< 配置。
    StreamTable m_table;                      ///< 流表。
    FrameDecoder m_decoder;                   ///< 帧解码器。
    std::vector<std::uint8_t> m_read_buffer;  ///< 读缓冲。
    std::vector<std::uint8_t> m_rx_pending;   ///< preface 之前的暂存。
    bool m_preface_received = false;          ///< 是否收到对端 preface。
    bool m_settings_received = false;         ///< 是否收到对端 SETTINGS。
    bool m_started = false;                   ///< 是否已 Start。
    bool m_closed = false;                    ///< 是否已关闭。
    bool m_goaway_received = false;           ///< 是否收到 GOAWAY。
    bool m_goaway_sent = false;               ///< 是否发送 GOAWAY。
    ErrorCode m_close_error = ErrorCode::kNoError;  ///< 关闭错误码。
    std::string m_close_message;              ///< 关闭消息。
    std::uint32_t m_last_remote_stream_id = 0;///< 最近收到的对端流 id（GOAWAY 用）。
    std::uint32_t m_peer_max_concurrent_streams = 0;  ///< 对端通告的并发上限。
    std::uint32_t m_peer_max_frame_payload = 0;       ///< 对端通告的帧上限。
    std::uint32_t m_peer_small_kv_threshold = 0;      ///< 对端通告的小 KV 阈值。
    std::uint32_t m_peer_resource_chunk_size = 0;     ///< 对端通告的资源块大小。

    std::map<std::uint32_t, std::unique_ptr<KvAssembler>> m_kv_assemblers;      ///< KV 聚合。
    std::map<std::uint32_t, std::unique_ptr<ResourceReceiver>> m_receivers;     ///< 资源接收。
    std::map<std::uint32_t, std::uint32_t> m_pending_window_updates;            ///< 流级窗口更新。
    std::uint32_t m_pending_conn_window_update = 0;                             ///< 连接级窗口更新。
    std::deque<Frame> m_control_queue;        ///< 待发控制帧（ACK 等）。

    std::function<std::unique_ptr<ResourceSink>(const ResourceMeta&)> m_sink_factory;
    std::function<void(const Event&)> m_event_handler;

    mutable std::recursive_mutex m_io_mutex;  ///< 串行化流表/发送（允许事件回调重入）。
    mutable std::mutex m_event_mutex;         ///< 事件队列。
    std::condition_variable m_event_cv;       ///< 事件等待。
    std::deque<Event> m_events;               ///< 事件队列。
    std::size_t m_event_limit = 65536;        ///< 队列上限（防止无界增长）。

    ConnectionStats m_stats;                  ///< 统计。
    std::uint64_t m_last_ping_ms = 0;         ///< 上次 PING 时间。
};

}  // namespace go2cpp::app
