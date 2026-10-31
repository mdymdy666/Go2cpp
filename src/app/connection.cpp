/**
 * @file connection.cpp
 * @brief 应用层连接实现：握手、帧分发、发送调度、资源/KV 业务组装。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/app/connection.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace go2cpp::app {
namespace {

constexpr std::uint8_t kPreface[8] = {'A', 'P', 'P', '2', '\r', '\n', '\r', '\n'};
constexpr std::size_t kPrefaceBytes = sizeof(kPreface);

std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

/**
 * @brief 裸字节流的生产者：HEADERS（可选元数据）+ DATA 分片 + END_STREAM。
 * @details 首帧一定是 HEADERS（即使 meta 为空），这样流表可以在不占用
 *          DATA 窗口的情况下推进流状态。
 */
class BytesProducer final : public FrameProducer {
   public:
    BytesProducer(std::vector<std::uint8_t> data, KvTable meta)
        : m_data(std::move(data)), m_meta(std::move(meta)) {}

    bool Produce(std::uint32_t max_payload, Frame* out) override {
        if (m_done || out == nullptr) {
            return false;
        }
        if (!m_headers_sent) {
            KvTable headers = m_meta;
            headers.Set(kAppKindKey, StreamKindKey(StreamKind::kBytes));
            std::vector<std::uint8_t> payload = headers.ToBinary();
            if (payload.size() > max_payload || payload.size() > kMaxFramePayloadLimit) {
                m_done = true;
                return false;
            }
            out->type = FrameType::kHeaders;
            out->flags = kFlagEndHeaders;
            out->payload = std::move(payload);
            m_headers_sent = true;
            if (m_data.empty()) {
                out->SetFlag(kFlagEndStream);
                m_done = true;
            }
            return true;
        }
        if (m_offset >= m_data.size()) {
            m_done = true;
            return false;
        }
        const std::size_t take = std::min<std::size_t>(max_payload, m_data.size() - m_offset);
        out->type = FrameType::kData;
        out->flags = 0;
        out->payload.assign(m_data.begin() + static_cast<std::ptrdiff_t>(m_offset),
                            m_data.begin() + static_cast<std::ptrdiff_t>(m_offset + take));
        m_offset += take;
        if (m_offset >= m_data.size()) {
            out->SetFlag(kFlagEndStream);
            m_done = true;
        }
        return true;
    }

    bool done() const override { return m_done; }
    std::uint64_t produced_bytes() const override { return m_offset; }

   private:
    std::vector<std::uint8_t> m_data;  ///< 数据。
    KvTable m_meta;                    ///< 元数据。
    std::size_t m_offset = 0;          ///< 已发送偏移。
    bool m_headers_sent = false;       ///< 是否已发 HEADERS。
    bool m_done = false;               ///< 是否完成。
};

}  // namespace

const char* RoleName(Role role) noexcept {
    return role == Role::kClient ? "client" : "server";
}

const char* EventTypeName(EventType type) noexcept {
    switch (type) {
        case EventType::kHeaders:
            return "HEADERS";
        case EventType::kData:
            return "DATA";
        case EventType::kEndStream:
            return "END_STREAM";
        case EventType::kReset:
            return "RESET";
        case EventType::kGoAway:
            return "GOAWAY";
        case EventType::kPing:
            return "PING";
        case EventType::kSettings:
            return "SETTINGS";
        case EventType::kKvComplete:
            return "KV_COMPLETE";
        case EventType::kResourceComplete:
            return "RESOURCE_COMPLETE";
        case EventType::kResourceIndex:
            return "RESOURCE_INDEX";
        case EventType::kClosed:
            return "CLOSED";
        case EventType::kError:
            return "ERROR";
        case EventType::kWindowUpdate:
            return "WINDOW_UPDATE";
    }
    return "UNKNOWN";
}

std::string Event::Describe() const {
    std::string out = EventTypeName(type);
    out += " stream=";
    out += std::to_string(stream_id);
    if (error_code != ErrorCode::kNoError) {
        out += " error=";
        out += ErrorCodeName(error_code);
    }
    if (!message.empty()) {
        out += " message=";
        out += message;
    }
    if (!headers.empty()) {
        out += " keys=";
        out += std::to_string(headers.size());
    }
    if (!data.empty()) {
        out += " bytes=";
        out += std::to_string(data.size());
    }
    return out;
}

Connection::Connection(TransportPtr transport, Role role, ConnectionConfig config)
    : m_transport(std::move(transport)),
      m_role(role),
      m_config(std::move(config)),
      m_table([this] {
          StreamTableConfig table = m_config.table;
          table.client = (m_role == Role::kClient);
          return table;
      }()),
      m_decoder(m_config.max_frame_payload + 64) {
    if (m_transport == nullptr) {
        m_closed = true;
        m_close_error = ErrorCode::kInternalError;
        m_close_message = "null transport";
    }
    if (m_config.max_frame_payload == 0) {
        m_config.max_frame_payload = 256u * 1024u;
    }
    if (m_config.read_chunk_bytes == 0) {
        m_config.read_chunk_bytes = 64u * 1024u;
    }
    m_read_buffer.resize(std::max<std::size_t>(4096, m_config.read_chunk_bytes));
    m_write_buffer.reserve(static_cast<std::size_t>(m_config.max_frame_payload) +
                           kFrameHeaderBytes);
    m_last_ping_ms = NowMs();
}

Connection::~Connection() {
    if (m_transport != nullptr) {
        CloseInternal(ErrorCode::kNoError, "connection destroyed", false);
    }
}

ConnectionStats Connection::stats() const {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    return m_stats;
}

const StreamTable& Connection::table() const { return m_table; }

bool Connection::closed() const {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    return m_closed;
}

bool Connection::ready() const {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    return m_preface_received && m_settings_received && !m_closed;
}

bool Connection::goaway_received() const {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    return m_goaway_received;
}

void Connection::SetResourceSinkFactory(
    std::function<std::unique_ptr<ResourceSink>(const ResourceMeta&)> factory) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    m_sink_factory = std::move(factory);
}

void Connection::SetEventHandler(std::function<void(const Event&)> handler) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    m_event_handler = std::move(handler);
}

void Connection::Publish(Event event) {
    if (event.type == EventType::kClosed || event.type == EventType::kError) {
        // 关闭/错误事件即使队列满也必须保留。
    }
    {
        std::lock_guard<std::mutex> lock(m_event_mutex);
        if (m_events.size() >= m_event_limit) {
            m_events.pop_front();
        }
        m_events.push_back(event);
    }
    m_event_cv.notify_all();
    if (m_event_handler) {
        m_event_handler(event);
    }
}

void Connection::CloseInternal(ErrorCode code, std::string message, bool notify) {
    if (m_closed) {
        return;
    }
    m_closed = true;
    m_close_error = code;
    m_close_message = std::move(message);
    if (m_transport != nullptr) {
        m_transport->Close();
    }
    if (notify) {
        Event event;
        event.type = EventType::kClosed;
        event.error_code = code;
        event.message = m_close_message;
        Publish(std::move(event));
    }
    m_event_cv.notify_all();
}

void Connection::FailConnection(ErrorCode code, std::string message) {
    if (m_closed) {
        return;
    }
    Event event;
    event.type = EventType::kError;
    event.error_code = code;
    event.message = message;
    Publish(std::move(event));
    CloseInternal(code, std::move(message), false);
}

bool Connection::WriteFrame(const Frame& frame) {
    if (m_closed || m_transport == nullptr) {
        return false;
    }
    // 复用成员缓冲：稳态下这里 0 分配（原来每帧一次 EncodeFrame 分配 + 拷贝）。
    m_write_buffer.clear();
    if (!AppendEncodedFrame(frame, &m_write_buffer)) {
        return false;
    }
    if (!m_transport->WriteAll(m_write_buffer.data(), m_write_buffer.size())) {
        FailConnection(ErrorCode::kInternalError, "transport write failed");
        return false;
    }
    ++m_stats.frames_sent;
    m_stats.bytes_sent += frame.Size();
    return true;
}

void Connection::ApplyLocalSettings() {
    m_control_queue.clear();
    std::vector<SettingsEntry> entries;
    entries.push_back({SettingId::kMaxConcurrentStreams,
                       m_config.table.max_concurrent_streams});
    entries.push_back({SettingId::kInitialWindowSize,
                       static_cast<std::uint32_t>(m_config.table.initial_stream_window)});
    entries.push_back({SettingId::kMaxFrameSize, m_config.max_frame_payload});
    entries.push_back({SettingId::kMaxHeaderListSize, m_config.max_frame_payload});
    entries.push_back({SettingId::kSmallKvThreshold, m_config.small_kv_threshold});
    entries.push_back({SettingId::kResourceChunkSize, m_config.resource_chunk_size});
    Frame frame;
    frame.type = FrameType::kSettings;
    frame.payload = EncodeSettings(entries);
    m_control_queue.push_back(std::move(frame));
}

bool Connection::Start(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_started) {
        return !m_closed;
    }
    if (m_closed || m_transport == nullptr) {
        SetError(error, m_close_message.empty() ? "connection is closed" : m_close_message);
        return false;
    }
    if (!m_transport->WriteAll(kPreface, kPrefaceBytes)) {
        SetError(error, "failed to write preface");
        CloseInternal(ErrorCode::kInternalError, "failed to write preface", false);
        return false;
    }
    m_started = true;
    ApplyLocalSettings();
    Frame settings = std::move(m_control_queue.front());
    m_control_queue.pop_front();
    if (!WriteFrame(settings)) {
        SetError(error, "failed to write settings");
        return false;
    }
    return true;
}

int Connection::Step(std::uint32_t timeout_ms) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_in_handle) {
        // 事件回调里重入 Step：此时当前帧的载荷是解码缓冲的借用视图，
        // 追加数据会让它失效，直接拒绝（回调返回后下一轮 Step 照常推进）。
        return 0;
    }
    if (m_closed) {
        return -1;
    }
    if (!m_started) {
        std::string error;
        if (!Start(&error)) {
            return -1;
        }
    }
    int processed = 0;
    const int max_frames = static_cast<int>(m_config.max_frames_per_step);
    auto drain = [this, &processed, max_frames] {
        while (processed < max_frames) {
            Frame frame;
            const FrameStatus status = m_decoder.NextView(&frame);
            if (status == FrameStatus::kNeedMoreData) {
                break;
            }
            if (status != FrameStatus::kOk) {
                FailConnection(ErrorCode::kProtocolError,
                               std::string("frame decode failed: ") +
                                   FrameStatusName(status));
                return false;
            }
            ++m_stats.frames_received;
            m_in_handle = true;
            HandleFrame(frame);
            m_in_handle = false;
            ++processed;
            if (m_closed) {
                return false;
            }
        }
        return true;
    };
    // 处理上一次 Step 留在解码器里的帧，再读新数据。
    if (!drain()) {
        return -1;
    }
    if (processed < max_frames && !m_closed) {
        const int n = m_transport->Read(m_read_buffer.data(), m_read_buffer.size(),
                                        timeout_ms);
        if (n < 0) {
            CloseInternal(ErrorCode::kNoError, "peer closed", true);
            return -1;
        }
        if (n > 0) {
            if (!m_preface_received) {
                m_rx_pending.insert(m_rx_pending.end(), m_read_buffer.begin(),
                                    m_read_buffer.begin() + n);
                if (m_rx_pending.size() < kPrefaceBytes) {
                    return processed;
                }
                if (std::memcmp(m_rx_pending.data(), kPreface, kPrefaceBytes) != 0) {
                    FailConnection(ErrorCode::kProtocolError, "invalid app preface");
                    return -1;
                }
                m_rx_pending.erase(m_rx_pending.begin(),
                                   m_rx_pending.begin() +
                                       static_cast<std::ptrdiff_t>(kPrefaceBytes));
                m_preface_received = true;
                if (!m_rx_pending.empty()) {
                    m_decoder.Append(m_rx_pending.data(), m_rx_pending.size());
                    m_rx_pending.clear();
                }
            } else {
                m_decoder.Append(m_read_buffer.data(), static_cast<std::size_t>(n));
            }
            if (!drain()) {
                return -1;
            }
        }
    }
    if (m_closed) {
        return -1;
    }
    FlushLocked();
    SendPendingPingLocked(NowMs());
    m_table.PruneClosedStreams();
    if (m_closed) {
        return -1;
    }
    return processed;
}

bool Connection::ReadEvent(Event* out, std::uint32_t timeout_ms) {
    if (out == nullptr) {
        return false;
    }
    const std::uint64_t start = NowMs();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_event_mutex);
            if (!m_events.empty()) {
                *out = std::move(m_events.front());
                m_events.pop_front();
                return true;
            }
        }
        if (closed()) {
            std::lock_guard<std::mutex> lock(m_event_mutex);
            if (!m_events.empty()) {
                *out = std::move(m_events.front());
                m_events.pop_front();
                return true;
            }
            return false;
        }
        const std::uint64_t elapsed = NowMs() - start;
        if (elapsed >= timeout_ms) {
            // 最后做一次非阻塞推进，避免刚到达的数据被漏掉。
            if (timeout_ms == 0) {
                Step(0);
                std::lock_guard<std::mutex> lock(m_event_mutex);
                if (!m_events.empty()) {
                    *out = std::move(m_events.front());
                    m_events.pop_front();
                    return true;
                }
            }
            return false;
        }
        const std::uint32_t remaining =
            static_cast<std::uint32_t>(timeout_ms - elapsed);
        const std::uint32_t wait = std::min<std::uint32_t>(remaining, 20);
        Step(wait);
        if (closed()) {
            std::lock_guard<std::mutex> lock(m_event_mutex);
            if (!m_events.empty()) {
                *out = std::move(m_events.front());
                m_events.pop_front();
                return true;
            }
            return false;
        }
    }
}

void Connection::Close(ErrorCode code, std::string message) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    CloseInternal(code, std::move(message), true);
}

// ---------------------------------------------------------------------------
// 发送调度
// ---------------------------------------------------------------------------

void Connection::EmitWindowUpdates() {
    for (auto& item : m_pending_window_updates) {
        if (item.second == 0) {
            continue;
        }
        Frame frame;
        frame.type = FrameType::kWindowUpdate;
        frame.stream_id = item.first;
        frame.payload = EncodeWindowUpdate(item.second);
        if (!WriteFrame(frame)) {
            return;
        }
        ++m_stats.window_updates_sent;
    }
    m_pending_window_updates.clear();
    if (m_pending_conn_window_update != 0) {
        Frame frame;
        frame.type = FrameType::kWindowUpdate;
        frame.stream_id = 0;
        frame.payload = EncodeWindowUpdate(m_pending_conn_window_update);
        if (!WriteFrame(frame)) {
            return;
        }
        ++m_stats.window_updates_sent;
        m_pending_conn_window_update = 0;
    }
}

void Connection::FlushLocked() {
    if (m_closed) {
        return;
    }
    EmitWindowUpdates();
    if (m_closed) {
        return;
    }
    std::size_t budget = m_config.max_bytes_per_flush;
    std::size_t rounds = 0;
    while (budget > 0 && !m_closed) {
        Stream* stream = m_table.PickNext(m_config.max_frame_payload);
        if (stream == nullptr) {
            break;
        }
        // 零拷贝：直接用流上的 pending 缓冲（不再 move 出来再 reset）。
        Frame& frame = stream->pending;
        const std::size_t bytes = frame.Size();
        const FrameType frame_type = frame.type;
        const bool end_stream = frame.EndStream();
        if (frame_type == FrameType::kData && bytes > 0 &&
            !m_table.ConsumeSend(*stream, bytes)) {
            // 理论上 PickNext 已保证窗口足够；并发更新导致回退时重排。
            m_table.Requeue(*stream);
            ++m_stats.flow_control_blocks;
            break;
        }
        if (!WriteFrame(frame)) {
            break;
        }
        // 帧已写出：清空 pending 但保留 payload 容量，下一帧生产零分配。
        m_table.ConsumePending(*stream);
        bool ok = true;
        if (frame_type == FrameType::kData) {
            ok = m_table.SendData(*stream, bytes, end_stream);
        } else if (frame_type == FrameType::kHeaders) {
            ok = m_table.SendHeaders(*stream, end_stream);
        } else {
            m_table.ChargeFinish(*stream, bytes);
        }
        if (frame_type == FrameType::kData) {
            m_table.ChargeFinish(*stream, bytes);
        }
        ++m_stats.flush_rounds;
        if (!ok) {
            // 状态推进失败说明对端/本端状态不一致，重置这条流。
            const std::uint32_t id = stream->id;
            Frame rst;
            rst.type = FrameType::kRstStream;
            rst.stream_id = id;
            rst.payload = EncodeRstStream(ErrorCode::kProtocolError);
            WriteFrame(rst);
            m_table.Reset(*stream, ErrorCode::kProtocolError);
            ++m_stats.resets;
            CleanupStream(id);
            budget = 0;
            break;
        }
        if (bytes >= budget) {
            budget = 0;
        } else {
            budget -= bytes;
        }
        if (end_stream && stream->closed()) {
            const std::uint32_t id = stream->id;
            CleanupStream(id);
        }
        if (++rounds > 200000) {
            break;
        }
    }
}

void Connection::SendPendingPingLocked(std::uint64_t now_ms) {
    if (m_config.ping_interval_ms == 0 || m_closed) {
        return;
    }
    if (now_ms - m_last_ping_ms < m_config.ping_interval_ms) {
        return;
    }
    m_last_ping_ms = now_ms;
    Frame frame;
    frame.type = FrameType::kPing;
    frame.payload = EncodePing({});
    if (WriteFrame(frame)) {
        ++m_stats.ping_sent;
    }
}

// ---------------------------------------------------------------------------
// 接收分发
// ---------------------------------------------------------------------------

void Connection::HandleFrame(const Frame& frame) {
    m_stats.bytes_received += frame.Size();
    switch (frame.type) {
        case FrameType::kHeaders:
            HandleHeaders(frame);
            break;
        case FrameType::kData:
            HandleData(frame);
            break;
        case FrameType::kRstStream:
            HandleRstStream(frame);
            break;
        case FrameType::kSettings:
            HandleSettings(frame);
            break;
        case FrameType::kPing:
            HandlePing(frame);
            break;
        case FrameType::kGoAway:
            HandleGoAway(frame);
            break;
        case FrameType::kWindowUpdate:
            HandleWindowUpdate(frame);
            break;
        case FrameType::kPriority:
            HandlePriority(frame);
            break;
        case FrameType::kResourceIndex:
            HandleResourceIndex(frame);
            break;
    }
}

void Connection::HandleHeaders(const Frame& frame) {
    if (frame.stream_id == 0) {
        FailConnection(ErrorCode::kProtocolError, "HEADERS on stream 0");
        return;
    }
    KvTable table;
    std::string error;
    if (!KvTable::ParseBinary(frame.Bytes(), frame.Size(), &table,
                              &error)) {
        Frame rst;
        rst.type = FrameType::kRstStream;
        rst.stream_id = frame.stream_id;
        rst.payload = EncodeRstStream(ErrorCode::kProtocolError);
        WriteFrame(rst);
        return;
    }
    Stream* stream = m_table.Find(frame.stream_id);
    if (stream == nullptr) {
        const bool odd = (frame.stream_id & 1u) != 0;
        const bool local_odd = (m_role == Role::kClient);
        if (odd == local_odd) {
            FailConnection(ErrorCode::kProtocolError,
                           "peer opened a stream with local parity");
            return;
        }
        StreamKind kind = StreamKind::kBytes;
        ParseStreamKind(table.Get(kAppKindKey), &kind);
        stream = m_table.CreateRemote(frame.stream_id, kind, &error);
        if (stream == nullptr) {
            Frame rst;
            rst.type = FrameType::kRstStream;
            rst.stream_id = frame.stream_id;
            rst.payload = EncodeRstStream(ErrorCode::kRefusedStream);
            WriteFrame(rst);
            return;
        }
        m_last_remote_stream_id = std::max(m_last_remote_stream_id, frame.stream_id);
    }
    if (!m_table.ReceiveHeaders(*stream, frame.EndStream())) {
        Frame rst;
        rst.type = FrameType::kRstStream;
        rst.stream_id = frame.stream_id;
        rst.payload = EncodeRstStream(ErrorCode::kStreamClosed);
        WriteFrame(rst);
        return;
    }
    const std::uint32_t id = stream->id;
    switch (stream->kind) {
        case StreamKind::kKv: {
            auto assembler = std::make_unique<KvAssembler>();
            if (!assembler->OnHeaders(table, &error)) {
                Frame rst;
                rst.type = FrameType::kRstStream;
                rst.stream_id = id;
                rst.payload = EncodeRstStream(ErrorCode::kProtocolError);
                WriteFrame(rst);
                m_table.Reset(*stream, ErrorCode::kProtocolError);
                CleanupStream(id);
                return;
            }
            if (frame.EndStream()) {
                KvTable out;
                const bool ok = assembler->Finish(&out, &error);
                Event event;
                event.type = EventType::kKvComplete;
                event.stream_id = id;
                event.headers = std::move(out);
                event.ok = ok;
                event.message = error;
                Publish(std::move(event));
                ++m_stats.streams_completed;
                CompleteReceive(*stream);
                return;
            }
            m_kv_assemblers[id] = std::move(assembler);
            return;
        }
        case StreamKind::kResource: {
            ResourceMeta meta;
            if (!ResourceMeta::FromKv(table, &meta, &error)) {
                Frame rst;
                rst.type = FrameType::kRstStream;
                rst.stream_id = id;
                rst.payload = EncodeRstStream(ErrorCode::kProtocolError);
                WriteFrame(rst);
                m_table.Reset(*stream, ErrorCode::kProtocolError);
                CleanupStream(id);
                return;
            }
            std::unique_ptr<ResourceSink> sink;
            if (m_sink_factory) {
                sink = m_sink_factory(meta);
            }
            const std::uint32_t resume_chunk = meta.resume_chunk;
            auto receiver = std::make_unique<ResourceReceiver>(std::move(meta),
                                                              std::move(sink),
                                                              resume_chunk);
            if (!receiver->error().empty()) {
                Event event;
                event.type = EventType::kResourceComplete;
                event.stream_id = id;
                event.resource_meta = receiver->meta();
                event.resource_stats = receiver->stats();
                event.ok = false;
                event.message = receiver->error();
                Publish(std::move(event));
                Frame rst;
                rst.type = FrameType::kRstStream;
                rst.stream_id = id;
                rst.payload = EncodeRstStream(ErrorCode::kInternalError);
                WriteFrame(rst);
                m_table.Reset(*stream, ErrorCode::kInternalError);
                CleanupStream(id);
                return;
            }
            m_receivers[id] = std::move(receiver);
            if (frame.EndStream()) {
                // 空资源：直接完成。
                auto it = m_receivers.find(id);
                ResourceReceiver& rx = *it->second;
                const bool ok = rx.Finish(&error);
                Event event;
                event.type = EventType::kResourceComplete;
                event.stream_id = id;
                event.resource_meta = rx.meta();
                event.resource_stats = rx.stats();
                event.ok = ok;
                event.message = error;
                Publish(std::move(event));
                if (ok) {
                    ++m_stats.streams_completed;
                    CompleteReceive(*stream);
                } else {
                    m_table.Reset(*stream, ErrorCode::kChecksumError);
                    CleanupStream(id);
                }
            }
            return;
        }
        case StreamKind::kControl:
        case StreamKind::kBytes: {
            Event event;
            event.type = EventType::kHeaders;
            event.stream_id = id;
            event.headers = table;
            event.bytes = 0;
            Publish(std::move(event));
            if (frame.EndStream()) {
                Event end;
                end.type = EventType::kEndStream;
                end.stream_id = id;
                Publish(std::move(end));
                ++m_stats.streams_completed;
                CompleteReceive(*stream);
            }
            return;
        }
    }
}

void Connection::HandleData(const Frame& frame) {
    if (frame.stream_id == 0) {
        FailConnection(ErrorCode::kProtocolError, "DATA on stream 0");
        return;
    }
    Stream* stream = m_table.Find(frame.stream_id);
    if (stream == nullptr) {
        // 迟到帧（流已清理）：按关闭流忽略，避免误杀连接。
        return;
    }
    const std::uint32_t id = stream->id;
    const std::size_t bytes = frame.Size();
    if (!m_table.ReceiveData(*stream, bytes, frame.EndStream())) {
        FailConnection(ErrorCode::kFlowControlError,
                       "flow control violation on stream " + std::to_string(id));
        return;
    }
    if (stream->local && bytes == 0 && frame.EndStream()) {
        // 对端对单向流（KV/资源/bytes）回的关闭确认：只推进状态并清理，
        // 不把它当成普通数据交给业务，也绝不能因「没有 assembler」而 RST。
        if (stream->closed()) {
            CleanupStream(id);
        }
        return;
    }
    switch (stream->kind) {
        case StreamKind::kKv: {
            auto it = m_kv_assemblers.find(id);
            if (it == m_kv_assemblers.end()) {
                Frame rst;
                rst.type = FrameType::kRstStream;
                rst.stream_id = id;
                rst.payload = EncodeRstStream(ErrorCode::kProtocolError);
                WriteFrame(rst);
                m_table.Reset(*stream, ErrorCode::kProtocolError);
                CleanupStream(id);
                return;
            }
            std::string error;
            if (!it->second->OnData(frame.Bytes(), bytes, &error)) {
                Event event;
                event.type = EventType::kError;
                event.stream_id = id;
                event.error_code = ErrorCode::kProtocolError;
                event.message = error;
                Publish(std::move(event));
                Frame rst;
                rst.type = FrameType::kRstStream;
                rst.stream_id = id;
                rst.payload = EncodeRstStream(ErrorCode::kProtocolError);
                WriteFrame(rst);
                m_table.Reset(*stream, ErrorCode::kProtocolError);
                ++m_stats.resets;
                CleanupStream(id);
                return;
            }
            // 框架已消费，立即归还接收窗口。
            if (m_config.auto_window_update) {
                const std::uint32_t delta = m_table.AcknowledgeReceived(*stream, bytes);
                if (delta != 0) {
                    m_pending_window_updates[id] += delta;
                }
                const std::uint32_t conn_delta = m_table.AcknowledgeConnection(bytes);
                if (conn_delta != 0) {
                    m_pending_conn_window_update += conn_delta;
                }
            }
            if (frame.EndStream()) {
                KvTable out;
                const bool ok = it->second->Finish(&out, &error);
                Event event;
                event.type = EventType::kKvComplete;
                event.stream_id = id;
                event.headers = std::move(out);
                event.ok = ok;
                event.message = error;
                Publish(std::move(event));
                m_kv_assemblers.erase(it);
                if (ok) {
                    ++m_stats.streams_completed;
                    CompleteReceive(*stream);
                } else {
                    m_table.Reset(*stream, ErrorCode::kProtocolError);
                    CleanupStream(id);
                }
            }
            return;
        }
        case StreamKind::kResource: {
            auto it = m_receivers.find(id);
            if (it == m_receivers.end()) {
                Frame rst;
                rst.type = FrameType::kRstStream;
                rst.stream_id = id;
                rst.payload = EncodeRstStream(ErrorCode::kProtocolError);
                WriteFrame(rst);
                m_table.Reset(*stream, ErrorCode::kProtocolError);
                CleanupStream(id);
                return;
            }
            std::string error;
            if (!it->second->OnData(frame.Bytes(), bytes, &error)) {
                Event event;
                event.type = EventType::kResourceComplete;
                event.stream_id = id;
                event.resource_meta = it->second->meta();
                event.resource_stats = it->second->stats();
                event.ok = false;
                event.message = error;
                Publish(std::move(event));
                Frame rst;
                rst.type = FrameType::kRstStream;
                rst.stream_id = id;
                rst.payload = EncodeRstStream(ErrorCode::kChecksumError);
                WriteFrame(rst);
                m_table.Reset(*stream, ErrorCode::kChecksumError);
                ++m_stats.resets;
                CleanupStream(id);
                return;
            }
            if (m_config.auto_window_update) {
                const std::uint32_t delta = m_table.AcknowledgeReceived(*stream, bytes);
                if (delta != 0) {
                    m_pending_window_updates[id] += delta;
                }
                const std::uint32_t conn_delta = m_table.AcknowledgeConnection(bytes);
                if (conn_delta != 0) {
                    m_pending_conn_window_update += conn_delta;
                }
            }
            if (frame.EndStream()) {
                ResourceReceiver& rx = *it->second;
                const bool ok = rx.Finish(&error);
                Event event;
                event.type = EventType::kResourceComplete;
                event.stream_id = id;
                event.resource_meta = rx.meta();
                event.resource_stats = rx.stats();
                event.ok = ok;
                event.message = error;
                Publish(std::move(event));
                m_receivers.erase(it);
                if (ok) {
                    ++m_stats.streams_completed;
                    CompleteReceive(*stream);
                } else {
                    m_table.Reset(*stream, ErrorCode::kChecksumError);
                    CleanupStream(id);
                }
            }
            return;
        }
        case StreamKind::kControl:
        case StreamKind::kBytes: {
            Event event;
            event.type = EventType::kData;
            event.stream_id = id;
            event.data.assign(frame.Bytes(), frame.Bytes() + bytes);
            event.bytes = bytes;
            Publish(std::move(event));
            if (m_config.auto_window_update) {
                const std::uint32_t delta = m_table.AcknowledgeReceived(*stream, bytes);
                if (delta != 0) {
                    m_pending_window_updates[id] += delta;
                }
                const std::uint32_t conn_delta = m_table.AcknowledgeConnection(bytes);
                if (conn_delta != 0) {
                    m_pending_conn_window_update += conn_delta;
                }
            }
            if (frame.EndStream()) {
                Event end;
                end.type = EventType::kEndStream;
                end.stream_id = id;
                end.bytes = bytes;
                Publish(std::move(end));
                ++m_stats.streams_completed;
                CompleteReceive(*stream);
            }
            return;
        }
    }
}

void Connection::HandleRstStream(const Frame& frame) {
    if (frame.stream_id == 0) {
        FailConnection(ErrorCode::kProtocolError, "RST_STREAM on stream 0");
        return;
    }
    ErrorCode code = ErrorCode::kNoError;
    if (!DecodeRstStream(frame.Bytes(), frame.Size(), &code)) {
        FailConnection(ErrorCode::kFrameSizeError, "invalid RST_STREAM payload");
        return;
    }
    Stream* stream = m_table.Find(frame.stream_id);
    if (stream != nullptr) {
        m_table.Reset(*stream, code);
    }
    Event event;
    event.type = EventType::kReset;
    event.stream_id = frame.stream_id;
    event.error_code = code;
    Publish(std::move(event));
    ++m_stats.resets;
    CleanupStream(frame.stream_id);
}

void Connection::HandleSettings(const Frame& frame) {
    if (frame.stream_id != 0) {
        FailConnection(ErrorCode::kProtocolError, "SETTINGS on non-zero stream");
        return;
    }
    if (frame.HasFlag(kFlagAck)) {
        if (!frame.Empty()) {
            FailConnection(ErrorCode::kFrameSizeError, "SETTINGS ACK with payload");
            return;
        }
        Event event;
        event.type = EventType::kSettings;
        event.stream_id = 0;
        event.ok = true;
        Publish(std::move(event));
        return;
    }
    std::vector<SettingsEntry> entries;
    if (!DecodeSettings(frame.Bytes(), frame.Size(), &entries)) {
        FailConnection(ErrorCode::kFrameSizeError, "invalid SETTINGS payload");
        return;
    }
    std::string error;
    KvTable view;
    for (const SettingsEntry& entry : entries) {
        switch (entry.id) {
            case SettingId::kInitialWindowSize:
                if (!m_table.SetInitialWindowSize(entry.value, &error)) {
                    FailConnection(ErrorCode::kFlowControlError, error);
                    return;
                }
                break;
            case SettingId::kMaxConcurrentStreams:
                m_peer_max_concurrent_streams = entry.value;
                break;
            case SettingId::kMaxFrameSize:
                if (entry.value >= 16384) {
                    m_peer_max_frame_payload =
                        std::min<std::uint32_t>(entry.value, m_config.max_frame_payload);
                }
                break;
            case SettingId::kSmallKvThreshold:
                if (entry.value > 0) {
                    m_peer_small_kv_threshold = entry.value;
                }
                break;
            case SettingId::kResourceChunkSize:
                if (entry.value >= kMinResourceChunkSize &&
                    entry.value <= kMaxResourceChunkSize) {
                    m_peer_resource_chunk_size = entry.value;
                }
                break;
            case SettingId::kMaxHeaderListSize:
                break;
        }
        view.Set(std::to_string(static_cast<std::uint16_t>(entry.id)),
                 std::to_string(entry.value));
    }
    m_settings_received = true;
    Event event;
    event.type = EventType::kSettings;
    event.stream_id = 0;
    event.headers = std::move(view);
    event.ok = true;
    Publish(std::move(event));
    if (m_config.auto_settings_ack) {
        Frame ack;
        ack.type = FrameType::kSettings;
        ack.flags = kFlagAck;
        ack.stream_id = 0;
        WriteFrame(ack);
    }
}

void Connection::HandlePing(const Frame& frame) {
    if (frame.stream_id != 0) {
        FailConnection(ErrorCode::kProtocolError, "PING on non-zero stream");
        return;
    }
    std::array<std::uint8_t, 8> opaque{};
    if (!DecodePing(frame.Bytes(), frame.Size(), &opaque)) {
        FailConnection(ErrorCode::kFrameSizeError, "invalid PING payload");
        return;
    }
    ++m_stats.ping_received;
    Event event;
    event.type = EventType::kPing;
    event.ping = opaque;
    event.ok = frame.HasFlag(kFlagAck);
    Publish(std::move(event));
    if (!frame.HasFlag(kFlagAck)) {
        Frame ack;
        ack.type = FrameType::kPing;
        ack.flags = kFlagAck;
        ack.payload = EncodePing(opaque);
        WriteFrame(ack);
    }
}

void Connection::HandleGoAway(const Frame& frame) {
    if (frame.stream_id != 0) {
        FailConnection(ErrorCode::kProtocolError, "GOAWAY on non-zero stream");
        return;
    }
    std::uint32_t last_stream_id = 0;
    ErrorCode code = ErrorCode::kNoError;
    std::string debug;
    if (!DecodeGoAway(frame.Bytes(), frame.Size(), &last_stream_id, &code,
                      &debug)) {
        FailConnection(ErrorCode::kFrameSizeError, "invalid GOAWAY payload");
        return;
    }
    m_goaway_received = true;
    Event event;
    event.type = EventType::kGoAway;
    event.error_code = code;
    event.message = std::move(debug);
    event.bytes = last_stream_id;
    Publish(std::move(event));
}

void Connection::HandleWindowUpdate(const Frame& frame) {
    std::uint32_t increment = 0;
    if (!DecodeWindowUpdate(frame.Bytes(), frame.Size(), &increment)) {
        FailConnection(ErrorCode::kFrameSizeError, "invalid WINDOW_UPDATE payload");
        return;
    }
    std::string error;
    if (!m_table.OnWindowUpdate(frame.stream_id, increment, &error)) {
        FailConnection(ErrorCode::kFlowControlError, error);
        return;
    }
    Event event;
    event.type = EventType::kWindowUpdate;
    event.stream_id = frame.stream_id;
    event.bytes = increment;
    Publish(std::move(event));
}

void Connection::HandlePriority(const Frame& frame) {
    if (frame.stream_id == 0) {
        FailConnection(ErrorCode::kProtocolError, "PRIORITY on stream 0");
        return;
    }
    std::uint32_t parent = 0;
    std::uint16_t weight = 16;
    if (!DecodePriority(frame.Bytes(), frame.Size(), &parent, &weight)) {
        FailConnection(ErrorCode::kFrameSizeError, "invalid PRIORITY payload");
        return;
    }
    std::string error;
    m_table.SetPriority(frame.stream_id, parent, weight, &error);
}

void Connection::HandleResourceIndex(const Frame& frame) {
    if (frame.stream_id != 0) {
        FailConnection(ErrorCode::kProtocolError, "RESOURCE_INDEX on non-zero stream");
        return;
    }
    ResourceIndex index;
    std::string error;
    if (!ResourceIndex::Decode(frame.Bytes(), frame.Size(), &index,
                               &error)) {
        Event event;
        event.type = EventType::kError;
        event.error_code = ErrorCode::kProtocolError;
        event.message = "invalid resource index: " + error;
        Publish(std::move(event));
        return;
    }
    Event event;
    event.type = EventType::kResourceIndex;
    event.resource_index = std::move(index);
    event.ok = true;
    Publish(std::move(event));
}

void Connection::CompleteReceive(Stream& stream) {
    const std::uint32_t id = stream.id;
    if (m_config.auto_close_ack && stream.can_send()) {
        Frame ack;
        ack.type = FrameType::kData;
        ack.stream_id = id;
        ack.flags = kFlagEndStream;
        if (WriteFrame(ack)) {
            m_table.SendData(stream, 0, true);
        }
    }
    if (stream.closed()) {
        CleanupStream(id);
    }
}

void Connection::CleanupStream(std::uint32_t stream_id) {
    m_kv_assemblers.erase(stream_id);
    m_receivers.erase(stream_id);
    m_pending_window_updates.erase(stream_id);
    if (m_table.Find(stream_id) != nullptr) {
        m_table.Erase(stream_id);
    }
}

// ---------------------------------------------------------------------------
// 发送 API
// ---------------------------------------------------------------------------

SendResult Connection::Fail(ErrorCode code, std::string message) const {
    SendResult result;
    result.error = code;
    result.message = std::move(message);
    return result;
}

SendResult Connection::OpenStream(StreamKind kind, std::uint16_t weight,
                                  std::uint32_t parent_id) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    if (m_goaway_received || m_goaway_sent) {
        return Fail(ErrorCode::kRefusedStream, "peer sent GOAWAY");
    }
    if (!m_started) {
        std::string error;
        if (!Start(&error)) {
            return Fail(ErrorCode::kInternalError, error);
        }
    }
    std::string error;
    Stream* stream = m_table.CreateLocal(kind, nullptr, weight, parent_id, &error);
    if (stream == nullptr) {
        return Fail(ErrorCode::kRefusedStream, error);
    }
    ++m_stats.streams_opened;
    SendResult result;
    result.stream_id = stream->id;
    return result;
}

SendResult Connection::EnqueueKv(const KvTable& table, KvMode mode, std::uint16_t weight,
                                 std::vector<std::uint32_t>* ids) {
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    if (m_goaway_received || m_goaway_sent) {
        return Fail(ErrorCode::kRefusedStream, "peer sent GOAWAY");
    }
    if (!m_started) {
        std::string error;
        if (!Start(&error)) {
            return Fail(ErrorCode::kInternalError, error);
        }
    }
    SendResult result;
    std::string error;
    if (mode == KvMode::kPerKey) {
        for (const auto& entry : table.entries()) {
            KvTable single;
            single.Set(entry.first, entry.second);
            Stream* stream = m_table.CreateLocal(StreamKind::kKv, nullptr, weight, 0,
                                                 &error);
            if (stream == nullptr) {
                result.error = ErrorCode::kRefusedStream;
                result.message = error;
                return result;
            }
            auto producer = std::make_unique<KvProducer>(
                std::move(single), m_config.small_kv_threshold,
                m_config.small_value_limit, KvMode::kPerKey);
            m_table.AttachProducer(*stream, std::move(producer));
            if (ids != nullptr) {
                ids->push_back(stream->id);
            }
            if (result.stream_id == 0) {
                result.stream_id = stream->id;
            }
            ++m_stats.streams_opened;
            ++m_stats.kv_streams;
        }
        return result;
    }
    Stream* stream = m_table.CreateLocal(StreamKind::kKv, nullptr, weight, 0, &error);
    if (stream == nullptr) {
        return Fail(ErrorCode::kRefusedStream, error);
    }
    auto producer = std::make_unique<KvProducer>(table, m_config.small_kv_threshold,
                                                 m_config.small_value_limit);
    m_table.AttachProducer(*stream, std::move(producer));
    ++m_stats.streams_opened;
    ++m_stats.kv_streams;
    result.stream_id = stream->id;
    if (ids != nullptr) {
        ids->push_back(stream->id);
    }
    return result;
}

SendResult Connection::SendKv(const KvTable& table, std::uint16_t weight) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    return EnqueueKv(table, KvMode::kSingle, weight, nullptr);
}

SendResult Connection::SendKvText(std::string_view text, std::uint16_t weight) {
    KvTable table;
    std::string error;
    if (!KvTable::ParseText(text, &table, &error)) {
        return Fail(ErrorCode::kProtocolError, "invalid kv text: " + error);
    }
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    return EnqueueKv(table, KvMode::kSingle, weight, nullptr);
}

SendResult Connection::SendKvPerKey(const KvTable& table,
                                    std::vector<std::uint32_t>* stream_ids,
                                    std::uint16_t weight) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    return EnqueueKv(table, KvMode::kPerKey, weight, stream_ids);
}

SendResult Connection::SendResource(ResourceMeta meta,
                                    std::shared_ptr<ChunkSource> source,
                                    std::uint32_t start_chunk, std::uint16_t weight) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    if (m_goaway_received || m_goaway_sent) {
        return Fail(ErrorCode::kRefusedStream, "peer sent GOAWAY");
    }
    if (source == nullptr) {
        return Fail(ErrorCode::kInternalError, "null resource source");
    }
    std::string error;
    if (!meta.Validate(&error)) {
        return Fail(ErrorCode::kProtocolError, "invalid resource meta: " + error);
    }
    if (meta.size != source->size()) {
        return Fail(ErrorCode::kProtocolError, "resource meta/source size mismatch");
    }
    if (meta.sha256_hex.empty()) {
        ResourceMeta built = BuildResourceMeta(meta.name, *source, meta.chunk_size,
                                               meta.extra, &error);
        if (built.name.empty()) {
            return Fail(ErrorCode::kInternalError,
                        "failed to build resource meta: " + error);
        }
        built.version = meta.version;
        meta = std::move(built);
    }
    if (meta.chunk_count == 0) {
        meta.chunk_count = static_cast<std::uint32_t>(
            (meta.size + meta.chunk_size - 1) / meta.chunk_size);
    }
    if (start_chunk > meta.chunk_count) {
        return Fail(ErrorCode::kResumeOutOfRange, "start chunk out of range");
    }
    if (!m_started) {
        if (!Start(&error)) {
            return Fail(ErrorCode::kInternalError, error);
        }
    }
    Stream* stream = m_table.CreateLocal(StreamKind::kResource, nullptr, weight, 0,
                                         &error);
    if (stream == nullptr) {
        return Fail(ErrorCode::kRefusedStream, error);
    }
    auto producer = std::make_unique<ResourceProducer>(meta, std::move(source), start_chunk);
    m_table.AttachProducer(*stream, std::move(producer));
    ++m_stats.streams_opened;
    ++m_stats.resource_streams;
    SendResult result;
    result.stream_id = stream->id;
    return result;
}

SendResult Connection::SendBytes(std::vector<std::uint8_t> data, const KvTable& meta,
                                 std::uint16_t weight, StreamKind kind) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    if (m_goaway_received || m_goaway_sent) {
        return Fail(ErrorCode::kRefusedStream, "peer sent GOAWAY");
    }
    if (!m_started) {
        std::string error;
        if (!Start(&error)) {
            return Fail(ErrorCode::kInternalError, error);
        }
    }
    std::string error;
    Stream* stream = m_table.CreateLocal(kind, nullptr, weight, 0, &error);
    if (stream == nullptr) {
        return Fail(ErrorCode::kRefusedStream, error);
    }
    m_table.AttachProducer(*stream,
                           std::make_unique<BytesProducer>(std::move(data), meta));
    ++m_stats.streams_opened;
    ++m_stats.bytes_streams;
    SendResult result;
    result.stream_id = stream->id;
    return result;
}

bool Connection::EnsureStartedLocked(std::string* error) {
    if (m_started) {
        return !m_closed;
    }
    return Start(error);
}

SendResult Connection::SendHeaders(std::uint32_t stream_id, const KvTable& headers,
                                   bool end_stream) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    std::string start_error;
    if (!EnsureStartedLocked(&start_error)) {
        return Fail(ErrorCode::kInternalError, start_error);
    }
    Stream* stream = m_table.Find(stream_id);
    if (stream == nullptr) {
        return Fail(ErrorCode::kStreamClosed, "unknown stream");
    }
    std::vector<std::uint8_t> payload = headers.ToBinary();
    if (payload.size() > m_config.max_frame_payload) {
        return Fail(ErrorCode::kFrameSizeError, "headers exceed max frame size");
    }
    Frame frame;
    frame.type = FrameType::kHeaders;
    frame.stream_id = stream_id;
    frame.flags = kFlagEndHeaders | (end_stream ? kFlagEndStream : 0);
    frame.payload = std::move(payload);
    if (!WriteFrame(frame)) {
        return Fail(ErrorCode::kInternalError, "write failed");
    }
    if (!m_table.SendHeaders(*stream, end_stream)) {
        return Fail(ErrorCode::kProtocolError, "invalid stream state");
    }
    if (stream->closed()) {
        CleanupStream(stream_id);
    }
    SendResult result;
    result.stream_id = stream_id;
    return result;
}

SendResult Connection::SendData(std::uint32_t stream_id, const std::uint8_t* data,
                                std::size_t size, bool end_stream) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    std::string start_error;
    if (!EnsureStartedLocked(&start_error)) {
        return Fail(ErrorCode::kInternalError, start_error);
    }
    if (data == nullptr && size != 0) {
        return Fail(ErrorCode::kInternalError, "null data");
    }
    Stream* stream = m_table.Find(stream_id);
    if (stream == nullptr) {
        return Fail(ErrorCode::kStreamClosed, "unknown stream");
    }
    if (!stream->can_send()) {
        return Fail(ErrorCode::kStreamClosed, "stream is not writable");
    }
    if (size > m_config.max_frame_payload) {
        return Fail(ErrorCode::kFrameSizeError, "data exceeds max frame size");
    }
    const std::int64_t window = m_table.SendWindow(*stream);
    if (static_cast<std::int64_t>(size) > window) {
        return Fail(ErrorCode::kFlowControlError, "insufficient send window");
    }
    Frame frame;
    frame.type = FrameType::kData;
    frame.stream_id = stream_id;
    frame.flags = end_stream ? kFlagEndStream : 0;
    if (size != 0) {
        frame.payload.assign(data, data + size);
    }
    if (!WriteFrame(frame)) {
        return Fail(ErrorCode::kInternalError, "write failed");
    }
    if (!m_table.ConsumeSend(*stream, size)) {
        return Fail(ErrorCode::kFlowControlError, "window consume failed");
    }
    if (!m_table.SendData(*stream, size, end_stream)) {
        return Fail(ErrorCode::kProtocolError, "invalid stream state");
    }
    if (stream->closed()) {
        CleanupStream(stream_id);
    }
    SendResult result;
    result.stream_id = stream_id;
    return result;
}

SendResult Connection::ResetStream(std::uint32_t stream_id, ErrorCode error) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    std::string start_error;
    if (!EnsureStartedLocked(&start_error)) {
        return Fail(ErrorCode::kInternalError, start_error);
    }
    Stream* stream = m_table.Find(stream_id);
    if (stream == nullptr) {
        return Fail(ErrorCode::kStreamClosed, "unknown stream");
    }
    Frame frame;
    frame.type = FrameType::kRstStream;
    frame.stream_id = stream_id;
    frame.payload = EncodeRstStream(error);
    if (!WriteFrame(frame)) {
        return Fail(ErrorCode::kInternalError, "write failed");
    }
    m_table.Reset(*stream, error);
    ++m_stats.resets;
    CleanupStream(stream_id);
    SendResult result;
    result.stream_id = stream_id;
    return result;
}

SendResult Connection::SendPriority(std::uint32_t stream_id, std::uint32_t parent_id,
                                    std::uint16_t weight) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    std::string start_error;
    if (!EnsureStartedLocked(&start_error)) {
        return Fail(ErrorCode::kInternalError, start_error);
    }
    Stream* stream = m_table.Find(stream_id);
    if (stream == nullptr) {
        return Fail(ErrorCode::kStreamClosed, "unknown stream");
    }
    if (!m_table.SetPriority(stream_id, parent_id, weight)) {
        return Fail(ErrorCode::kProtocolError, "invalid priority dependency");
    }
    Frame frame;
    frame.type = FrameType::kPriority;
    frame.stream_id = stream_id;
    frame.payload = EncodePriority(parent_id, weight);
    if (!WriteFrame(frame)) {
        return Fail(ErrorCode::kInternalError, "write failed");
    }
    SendResult result;
    result.stream_id = stream_id;
    return result;
}

SendResult Connection::SendResourceIndex(const ResourceIndex& index) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    std::string start_error;
    if (!EnsureStartedLocked(&start_error)) {
        return Fail(ErrorCode::kInternalError, start_error);
    }
    std::vector<std::uint8_t> payload = index.Encode();
    if (payload.size() > m_config.max_frame_payload) {
        return Fail(ErrorCode::kFrameSizeError, "resource index exceeds max frame size");
    }
    Frame frame;
    frame.type = FrameType::kResourceIndex;
    frame.stream_id = 0;
    frame.payload = std::move(payload);
    if (!WriteFrame(frame)) {
        return Fail(ErrorCode::kInternalError, "write failed");
    }
    return SendResult{};
}

SendResult Connection::Ping(const std::array<std::uint8_t, 8>& opaque) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    std::string start_error;
    if (!EnsureStartedLocked(&start_error)) {
        return Fail(ErrorCode::kInternalError, start_error);
    }
    Frame frame;
    frame.type = FrameType::kPing;
    frame.payload = EncodePing(opaque);
    if (!WriteFrame(frame)) {
        return Fail(ErrorCode::kInternalError, "write failed");
    }
    ++m_stats.ping_sent;
    return SendResult{};
}

SendResult Connection::SendSettings(const std::vector<SettingsEntry>& entries) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    std::string start_error;
    if (!EnsureStartedLocked(&start_error)) {
        return Fail(ErrorCode::kInternalError, start_error);
    }
    Frame frame;
    frame.type = FrameType::kSettings;
    frame.payload = EncodeSettings(entries);
    if (!WriteFrame(frame)) {
        return Fail(ErrorCode::kInternalError, "write failed");
    }
    return SendResult{};
}

SendResult Connection::SendGoAway(std::uint32_t last_stream_id, ErrorCode error,
                                  std::string debug) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    if (m_closed) {
        return Fail(ErrorCode::kStreamClosed, "connection closed");
    }
    std::string start_error;
    if (!EnsureStartedLocked(&start_error)) {
        return Fail(ErrorCode::kInternalError, start_error);
    }
    Frame frame;
    frame.type = FrameType::kGoAway;
    frame.stream_id = 0;
    frame.payload = EncodeGoAway(last_stream_id, error, debug);
    if (!WriteFrame(frame)) {
        return Fail(ErrorCode::kInternalError, "write failed");
    }
    m_goaway_sent = true;
    m_table.GoAway(last_stream_id, error, std::move(debug));
    return SendResult{};
}

bool Connection::Acknowledge(std::uint32_t stream_id, std::size_t bytes) {
    std::lock_guard<std::recursive_mutex> lock(m_io_mutex);
    Stream* stream = m_table.Find(stream_id);
    if (stream == nullptr) {
        return false;
    }
    const std::uint32_t delta = m_table.AcknowledgeReceived(*stream, bytes);
    if (delta != 0) {
        m_pending_window_updates[stream_id] += delta;
    }
    const std::uint32_t conn_delta = m_table.AcknowledgeConnection(bytes);
    if (conn_delta != 0) {
        m_pending_conn_window_update += conn_delta;
    }
    return true;
}

}  // namespace go2cpp::app
