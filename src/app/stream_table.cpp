/**
 * @file stream_table.cpp
 * @brief 流表实现：状态机、两级窗口、依赖树 WFQ 调度。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/app/stream_table.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace go2cpp::app {
namespace {

constexpr std::int64_t kMaxWindow = 0x7fffffff;

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

}  // namespace

const char* StreamStateName(StreamState state) noexcept {
    switch (state) {
        case StreamState::kIdle:
            return "idle";
        case StreamState::kOpen:
            return "open";
        case StreamState::kHalfClosedLocal:
            return "half_closed_local";
        case StreamState::kHalfClosedRemote:
            return "half_closed_remote";
        case StreamState::kClosed:
            return "closed";
    }
    return "unknown";
}

const char* StreamTableStateName(StreamState state) noexcept {
    return StreamStateName(state);
}

const char* StreamKindName(StreamKind kind) noexcept {
    switch (kind) {
        case StreamKind::kControl:
            return "control";
        case StreamKind::kKv:
            return "kv";
        case StreamKind::kResource:
            return "resource";
        case StreamKind::kBytes:
            return "bytes";
    }
    return "unknown";
}

const char* StreamKindKey(StreamKind kind) noexcept {
    switch (kind) {
        case StreamKind::kControl:
            return "control";
        case StreamKind::kKv:
            return "kv";
        case StreamKind::kResource:
            return "resource";
        case StreamKind::kBytes:
            return "bytes";
    }
    return "bytes";
}

bool ParseStreamKind(std::string_view key, StreamKind* kind) noexcept {
    if (kind == nullptr) {
        return false;
    }
    if (key == "kv") {
        *kind = StreamKind::kKv;
        return true;
    }
    if (key == "resource") {
        *kind = StreamKind::kResource;
        return true;
    }
    if (key == "bytes") {
        *kind = StreamKind::kBytes;
        return true;
    }
    if (key == "control") {
        *kind = StreamKind::kControl;
        return true;
    }
    return false;
}

StreamTable::StreamTable(StreamTableConfig config) : m_config(std::move(config)) {
    if (m_config.max_concurrent_streams == 0) {
        m_config.max_concurrent_streams = 1;
    }
    if (m_config.initial_stream_window <= 0) {
        m_config.initial_stream_window = 65535;
    }
    if (m_config.connection_window <= 0) {
        m_config.connection_window = m_config.initial_stream_window;
    }
    if (m_config.max_frame_payload == 0) {
        m_config.max_frame_payload = 256u * 1024u;
    }
    if (m_config.default_weight == 0) {
        m_config.default_weight = 16;
    }
    m_config.max_stream_id = std::min(m_config.max_stream_id, kMaxStreamId);
    m_conn_send_window = m_config.connection_window;
    m_conn_recv_window = m_config.connection_window;
    m_initial_window = m_config.initial_stream_window;
    m_next_local_id = m_config.client ? 1u : 2u;
    m_last_local_id = 0;
}

std::uint32_t StreamTable::AllocateLocalId() {
    const std::uint32_t step = 2;
    if (m_next_local_id > m_config.max_stream_id || m_next_local_id > kMaxStreamId) {
        return 0;
    }
    // 跳过已经被对端占用（或本地已用）的 id。
    while (m_streams.count(m_next_local_id) != 0) {
        if (m_next_local_id > kMaxStreamId - step) {
            return 0;
        }
        m_next_local_id += step;
    }
    const std::uint32_t id = m_next_local_id;
    if (id > kMaxStreamId - step) {
        m_next_local_id = kMaxStreamId + 1;
    } else {
        m_next_local_id += step;
    }
    m_last_local_id = id;
    return id;
}

std::size_t StreamTable::LiveStreamCount() const noexcept {
    std::size_t count = 0;
    for (const auto& item : m_streams) {
        if (item.second->state != StreamState::kClosed) {
            ++count;
        }
    }
    return count;
}

Stream* StreamTable::CreateLocal(StreamKind kind, FrameProducerPtr producer,
                                 std::uint16_t weight, std::uint32_t parent,
                                 std::string* error) {
    if (m_goaway) {
        SetError(error, "connection is going away");
        return nullptr;
    }
    if (LiveStreamCount() >= m_config.max_concurrent_streams) {
        SetError(error, "max concurrent streams reached");
        return nullptr;
    }
    const std::uint32_t id = AllocateLocalId();
    if (id == 0) {
        SetError(error, "stream id space exhausted");
        return nullptr;
    }
    auto stream = std::make_unique<Stream>();
    stream->id = id;
    stream->kind = kind;
    stream->weight = weight == 0 ? m_config.default_weight : std::min<std::uint16_t>(weight, 256);
    stream->parent_id = parent;
    stream->local = true;
    stream->send_window = m_initial_window;
    stream->recv_window = m_initial_window;
    stream->opened_at_ms = NowMs();
    stream->producer = std::move(producer);
    Stream* raw = stream.get();
    m_streams.emplace(id, std::move(stream));
    RecordCreated();
    return raw;
}

Stream* StreamTable::CreateRemote(std::uint32_t id, StreamKind kind,
                                  std::string* error) {
    if (id == 0 || id > m_config.max_stream_id) {
        SetError(error, "invalid stream id");
        return nullptr;
    }
    if (m_streams.count(id) != 0) {
        SetError(error, "duplicate stream id");
        return nullptr;
    }
    if (LiveStreamCount() >= m_config.max_concurrent_streams) {
        SetError(error, "max concurrent streams reached");
        return nullptr;
    }
    auto stream = std::make_unique<Stream>();
    stream->id = id;
    stream->kind = kind;
    stream->weight = m_config.default_weight;
    stream->parent_id = 0;
    stream->local = false;
    stream->send_window = m_initial_window;
    stream->recv_window = m_initial_window;
    stream->opened_at_ms = NowMs();
    Stream* raw = stream.get();
    m_streams.emplace(id, std::move(stream));
    RecordCreated();
    return raw;
}

Stream* StreamTable::Find(std::uint32_t id) noexcept {
    const auto it = m_streams.find(id);
    return it == m_streams.end() ? nullptr : it->second.get();
}

const Stream* StreamTable::Find(std::uint32_t id) const noexcept {
    const auto it = m_streams.find(id);
    return it == m_streams.end() ? nullptr : it->second.get();
}

bool StreamTable::Erase(std::uint32_t id) noexcept {
    const auto it = m_streams.find(id);
    if (it == m_streams.end()) {
        return false;
    }
    m_streams.erase(it);
    return true;
}

void StreamTable::RecordCreated() { ++m_total_created; }

void StreamTable::RefreshState(Stream& stream) noexcept {
    if (!stream.opened) {
        stream.state = StreamState::kIdle;
        return;
    }
    if (stream.local_closed && stream.remote_closed) {
        if (stream.state != StreamState::kClosed) {
            stream.closed_at_ms = NowMs();
            ++m_total_closed;
        }
        stream.state = StreamState::kClosed;
        return;
    }
    if (stream.local_closed) {
        stream.state = StreamState::kHalfClosedLocal;
        return;
    }
    if (stream.remote_closed) {
        stream.state = StreamState::kHalfClosedRemote;
        return;
    }
    stream.state = StreamState::kOpen;
}

void StreamTable::MarkClosed(Stream& stream) noexcept {
    if (stream.state != StreamState::kClosed) {
        stream.state = StreamState::kClosed;
        stream.closed_at_ms = NowMs();
        ++m_total_closed;
    }
    stream.local_closed = true;
    stream.remote_closed = true;
}

bool StreamTable::SendHeaders(Stream& stream, bool end_stream) noexcept {
    if (!stream.opened) {
        if (stream.state != StreamState::kIdle || stream.closed()) {
            return false;
        }
        stream.opened = true;
        stream.local_closed = end_stream;
        RefreshState(stream);
        return true;
    }
    if (!stream.can_send()) {
        return false;
    }
    if (end_stream) {
        stream.local_closed = true;
    }
    RefreshState(stream);
    return true;
}

bool StreamTable::ReceiveHeaders(Stream& stream, bool end_stream) noexcept {
    if (!stream.opened) {
        if (stream.state != StreamState::kIdle) {
            return false;
        }
        stream.opened = true;
        stream.remote_closed = end_stream;
        RefreshState(stream);
        return true;
    }
    if (!stream.can_receive()) {
        return false;
    }
    if (end_stream) {
        stream.remote_closed = true;
    }
    RefreshState(stream);
    return true;
}

bool StreamTable::SendData(Stream& stream, std::size_t bytes, bool end_stream) noexcept {
    if (!stream.can_send()) {
        return false;
    }
    stream.bytes_sent += bytes;
    ++stream.frames_sent;
    m_total_bytes_sent += bytes;
    if (end_stream) {
        stream.local_closed = true;
    }
    RefreshState(stream);
    return true;
}

bool StreamTable::ReceiveData(Stream& stream, std::size_t bytes,
                              bool end_stream) noexcept {
    if (!stream.can_receive()) {
        return false;
    }
    if (bytes > 0) {
        if (static_cast<std::int64_t>(bytes) > stream.recv_window ||
            static_cast<std::int64_t>(bytes) > m_conn_recv_window) {
            return false;
        }
        stream.recv_window -= static_cast<std::int64_t>(bytes);
        m_conn_recv_window -= static_cast<std::int64_t>(bytes);
        stream.bytes_received += bytes;
        m_total_bytes_received += bytes;
    }
    ++stream.frames_received;
    if (end_stream) {
        stream.remote_closed = true;
    }
    RefreshState(stream);
    return true;
}

bool StreamTable::Reset(Stream& stream, ErrorCode error) noexcept {
    if (stream.state == StreamState::kClosed) {
        return false;
    }
    stream.reset = true;
    stream.reset_error = static_cast<std::uint32_t>(error);
    ++m_total_resets;
    MarkClosed(stream);
    return true;
}

bool StreamTable::Close(Stream& stream) noexcept {
    if (stream.state == StreamState::kClosed) {
        return false;
    }
    MarkClosed(stream);
    return true;
}

void StreamTable::AttachProducer(Stream& stream, FrameProducerPtr producer) noexcept {
    stream.producer = std::move(producer);
    stream.producer_done = false;
}

bool StreamTable::FillPending(Stream& stream, std::uint32_t max_payload) {
    if (stream.pending.has_value()) {
        return true;
    }
    if (!stream.producer || stream.producer_done) {
        return false;
    }
    // idle 流仍要允许生产首帧 HEADERS；已打开的流才检查发送方向是否关闭。
    if (stream.opened && !stream.can_send()) {
        return false;
    }
    const std::uint32_t limit = std::min(
        max_payload == 0 ? m_config.max_frame_payload : max_payload,
        m_config.max_frame_payload);
    std::uint32_t allowed = limit;
    if (stream.opened) {
        const std::int64_t window = SendWindow(stream);
        if (window <= 0) {
            return false;
        }
        allowed = static_cast<std::uint32_t>(
            std::min<std::int64_t>(static_cast<std::int64_t>(limit), window));
        // 小于一个最小有效载荷时不生产：等 WINDOW_UPDATE，避免碎片化。
        if (allowed < 16) {
            return false;
        }
    }
    Frame frame;
    if (!stream.producer->Produce(allowed, &frame)) {
        if (stream.producer->done()) {
            stream.producer_done = true;
        }
        return false;
    }
    if (frame.payload.size() > kMaxFramePayloadLimit ||
        frame.payload.size() > limit) {
        // 生产者违约：不把非法帧放进调度器。
        stream.producer_done = true;
        return false;
    }
    frame.stream_id = stream.id;
    stream.pending = std::move(frame);
    if (stream.producer->done()) {
        stream.producer_done = true;
    }
    return true;
}

bool StreamTable::IsReady(const Stream& stream) const noexcept {
    if (!stream.pending.has_value()) {
        return false;
    }
    const Frame& frame = *stream.pending;
    if (frame.type != FrameType::kData) {
        return true;
    }
    if (frame.payload.empty()) {
        return true;  // 空 DATA（结束确认）不占窗口。
    }
    const std::int64_t window = SendWindow(stream);
    return window >= static_cast<std::int64_t>(frame.payload.size());
}

bool StreamTable::HasReadyAncestor(const Stream& stream) const noexcept {
    std::uint32_t parent = stream.parent_id;
    std::uint32_t guard = 0;
    while (parent != 0 && guard < m_streams.size() + 1) {
        const auto it = m_streams.find(parent);
        if (it == m_streams.end()) {
            return false;
        }
        if (IsReady(*it->second)) {
            return true;
        }
        parent = it->second->parent_id;
        ++guard;
    }
    return false;
}

Stream* StreamTable::PickNext(std::uint32_t max_payload) {
    // 第一遍：给所有可发送流准备一帧（窗口不足时会跳过）。
    for (auto& item : m_streams) {
        Stream& stream = *item.second;
        if (!stream.pending.has_value()) {
            FillPending(stream, max_payload);
        }
    }
    // 第二遍：父优先 + WFQ。
    Stream* best = nullptr;
    for (auto& item : m_streams) {
        Stream& stream = *item.second;
        if (stream.opened && !stream.can_send()) {
            continue;
        }
        if (!IsReady(stream)) {
            continue;
        }
        if (HasReadyAncestor(stream)) {
            continue;
        }
        if (best == nullptr) {
            best = &stream;
            continue;
        }
        if (stream.finish < best->finish ||
            (stream.finish == best->finish && stream.id < best->id)) {
            best = &stream;
        }
    }
    return best;
}

bool StreamTable::HasWork() const noexcept {
    for (const auto& item : m_streams) {
        const Stream& stream = *item.second;
        if (stream.pending.has_value()) {
            return true;
        }
        if ((!stream.opened || stream.can_send()) && stream.producer &&
            !stream.producer_done) {
            return true;
        }
    }
    return false;
}

std::size_t StreamTable::ReadyCount() const noexcept {
    std::size_t count = 0;
    for (const auto& item : m_streams) {
        if (IsReady(*item.second)) {
            ++count;
        }
    }
    return count;
}

std::size_t StreamTable::BlockedCount() const noexcept {
    std::size_t count = 0;
    for (const auto& item : m_streams) {
        const Stream& stream = *item.second;
        if ((!stream.opened || stream.can_send()) && stream.producer &&
            !stream.producer_done && !stream.pending.has_value()) {
            ++count;
        }
    }
    return count;
}

bool StreamTable::ConsumeSend(Stream& stream, std::size_t bytes) noexcept {
    if (bytes > 0) {
        if (static_cast<std::int64_t>(bytes) > stream.send_window ||
            static_cast<std::int64_t>(bytes) > m_conn_send_window) {
            return false;
        }
        stream.send_window -= static_cast<std::int64_t>(bytes);
        m_conn_send_window -= static_cast<std::int64_t>(bytes);
    }
    return true;
}

void StreamTable::ChargeFinish(Stream& stream, std::size_t bytes) noexcept {
    if (bytes == 0) {
        return;
    }
    const std::uint32_t weight = std::max<std::uint16_t>(1, stream.weight);
    stream.finish += static_cast<std::int64_t>(bytes) * (1 << 20) / weight;
}

std::int64_t StreamTable::SendWindow(const Stream& stream) const noexcept {
    return std::min(m_conn_send_window, stream.send_window);
}

bool StreamTable::OnWindowUpdate(std::uint32_t stream_id, std::uint32_t increment,
                                 std::string* error) {
    if (increment == 0 || increment > kMaxStreamId) {
        SetError(error, "invalid window increment");
        return false;
    }
    if (stream_id == 0) {
        if (m_conn_send_window > kMaxWindow - static_cast<std::int64_t>(increment)) {
            SetError(error, "connection window overflow");
            return false;
        }
        m_conn_send_window += static_cast<std::int64_t>(increment);
        return true;
    }
    Stream* stream = Find(stream_id);
    if (stream == nullptr) {
        // 已关闭/未知流上的 WINDOW_UPDATE 按 RFC 允许忽略。
        return true;
    }
    if (stream->send_window > kMaxWindow - static_cast<std::int64_t>(increment)) {
        SetError(error, "stream window overflow");
        return false;
    }
    stream->send_window += static_cast<std::int64_t>(increment);
    return true;
}

bool StreamTable::SetInitialWindowSize(std::int64_t new_window, std::string* error) {
    if (new_window <= 0 || new_window > kMaxWindow) {
        SetError(error, "invalid initial window size");
        return false;
    }
    const std::int64_t delta = new_window - m_initial_window;
    for (auto& item : m_streams) {
        Stream& stream = *item.second;
        stream.send_window += delta;
        if (stream.send_window < 0) {
            SetError(error, "initial window change makes stream window negative");
            return false;
        }
        if (stream.send_window > kMaxWindow) {
            SetError(error, "initial window change overflows stream window");
            return false;
        }
    }
    m_initial_window = new_window;
    m_config.initial_stream_window = new_window;
    if (m_conn_send_window < 0) {
        m_conn_send_window = 0;
    }
    return true;
}

std::uint32_t StreamTable::AcknowledgeReceived(Stream& stream, std::size_t bytes) noexcept {
    stream.consumed_since_update += bytes;
    const std::uint64_t threshold = static_cast<std::uint64_t>(
        std::max<std::int64_t>(1, m_initial_window / 2));
    if (stream.consumed_since_update < threshold) {
        return 0;
    }
    const std::uint32_t delta =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(stream.consumed_since_update,
                                                           static_cast<std::uint64_t>(kMaxStreamId)));
    stream.consumed_since_update -= delta;
    stream.recv_window += delta;
    if (stream.recv_window > m_initial_window) {
        stream.recv_window = m_initial_window;
    }
    return delta;
}

std::uint32_t StreamTable::AcknowledgeConnection(std::size_t bytes) noexcept {
    m_conn_consumed += bytes;
    const std::uint64_t threshold = static_cast<std::uint64_t>(
        std::max<std::int64_t>(1, m_config.connection_window / 2));
    if (m_conn_consumed < threshold) {
        return 0;
    }
    const std::uint32_t delta =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(m_conn_consumed,
                                                           static_cast<std::uint64_t>(kMaxStreamId)));
    m_conn_consumed -= delta;
    m_conn_recv_window += delta;
    if (m_conn_recv_window > m_config.connection_window) {
        m_conn_recv_window = m_config.connection_window;
    }
    return delta;
}

bool StreamTable::IsAncestor(std::uint32_t ancestor, std::uint32_t id) const noexcept {
    if (ancestor == 0 || ancestor == id) {
        return false;
    }
    std::uint32_t current = id;
    std::size_t guard = 0;
    while (current != 0 && guard <= m_streams.size() + 1) {
        const auto it = m_streams.find(current);
        if (it == m_streams.end()) {
            return false;
        }
        current = it->second->parent_id;
        if (current == ancestor) {
            return true;
        }
        ++guard;
    }
    return false;
}

bool StreamTable::SetPriority(std::uint32_t id, std::uint32_t parent, std::uint16_t weight,
                              std::string* error) {
    Stream* stream = Find(id);
    if (stream == nullptr) {
        SetError(error, "unknown stream");
        return false;
    }
    if (parent != 0 && Find(parent) == nullptr) {
        SetError(error, "unknown parent stream");
        return false;
    }
    if (parent == id || IsAncestor(id, parent)) {
        SetError(error, "priority dependency cycle");
        return false;
    }
    stream->parent_id = parent;
    stream->weight = weight == 0 ? 1 : std::min<std::uint16_t>(weight, 256);
    return true;
}

std::uint16_t StreamTable::WeightOf(std::uint32_t id) const noexcept {
    const Stream* stream = Find(id);
    return stream == nullptr ? 0 : stream->weight;
}

void StreamTable::GoAway(std::uint32_t last_stream_id, ErrorCode error,
                         std::string debug) {
    m_goaway = true;
    m_goaway_last_stream_id = last_stream_id & kMaxStreamId;
    m_goaway_error = error;
    m_goaway_debug = std::move(debug);
}

std::size_t StreamTable::active_streams() const noexcept {
    std::size_t count = 0;
    for (const auto& item : m_streams) {
        const Stream& stream = *item.second;
        if (stream.active()) {
            ++count;
        }
    }
    return count;
}

std::size_t StreamTable::pending_streams() const noexcept {
    std::size_t count = 0;
    for (const auto& item : m_streams) {
        if (item.second->pending.has_value()) {
            ++count;
        }
    }
    return count;
}

void StreamTable::PruneClosedStreams() {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> closed_streams;
    closed_streams.reserve(m_streams.size());
    for (const auto& item : m_streams) {
        if (item.second->state == StreamState::kClosed) {
            closed_streams.emplace_back(item.second->closed_at_ms, item.first);
        }
    }
    const std::size_t keep = m_config.max_retired_streams;
    if (closed_streams.size() <= keep) {
        return;
    }
    // 先关闭的先淘汰：保留最近的 `keep` 条，避免刚关闭的流被立刻删除
    // （调用方可能还持有它的引用直到本轮处理结束）。
    std::sort(closed_streams.begin(), closed_streams.end());
    const std::size_t remove = closed_streams.size() - keep;
    for (std::size_t i = 0; i < remove; ++i) {
        m_streams.erase(closed_streams[i].second);
        ++m_retired_streams;
    }
}

void StreamTable::ResetStats() noexcept {
    m_total_created = 0;
    m_total_closed = 0;
    m_total_bytes_sent = 0;
    m_total_bytes_received = 0;
    m_total_resets = 0;
    m_retired_streams = 0;
}

}  // namespace go2cpp::app
