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

std::size_t StreamTable::LiveStreamCount() const noexcept { return m_live_streams; }

// ---------------------------------------------------------------------------
// ready 最小堆（按 (finish, id) 排序）+ 生产者索引
// ---------------------------------------------------------------------------

namespace {

/// @brief ready 堆比较：finish 小者优先，同 finish 按 id 升序（确定性）。
bool ReadyBefore(const Stream& a, const Stream& b) noexcept {
    if (a.finish != b.finish) {
        return a.finish < b.finish;
    }
    return a.id < b.id;
}

}  // namespace

void StreamTable::HeapSwap(std::size_t a, std::size_t b) noexcept {
    Stream* tmp = m_ready[a];
    m_ready[a] = m_ready[b];
    m_ready[b] = tmp;
    m_ready[a]->ready_slot = static_cast<std::uint32_t>(a);
    m_ready[b]->ready_slot = static_cast<std::uint32_t>(b);
}

void StreamTable::SiftUp(std::size_t i) noexcept {
    while (i > 0) {
        const std::size_t parent = (i - 1) / 2;
        if (!ReadyBefore(*m_ready[i], *m_ready[parent])) {
            return;
        }
        HeapSwap(i, parent);
        i = parent;
    }
}

void StreamTable::SiftDown(std::size_t i) noexcept {
    const std::size_t count = m_ready.size();
    while (true) {
        const std::size_t left = i * 2 + 1;
        if (left >= count) {
            return;
        }
        std::size_t best = left;
        const std::size_t right = left + 1;
        if (right < count && ReadyBefore(*m_ready[right], *m_ready[left])) {
            best = right;
        }
        if (!ReadyBefore(*m_ready[best], *m_ready[i])) {
            return;
        }
        HeapSwap(i, best);
        i = best;
    }
}

void StreamTable::PushReady(Stream& stream) noexcept {
    if (stream.ready_slot != Stream::kNoSlot) {
        return;  // 已在堆中（幂等）。
    }
    stream.ready_slot = static_cast<std::uint32_t>(m_ready.size());
    m_ready.push_back(&stream);
    SiftUp(m_ready.size() - 1);
}

void StreamTable::RemoveReady(Stream& stream) noexcept {
    if (stream.ready_slot == Stream::kNoSlot) {
        return;
    }
    const std::size_t index = stream.ready_slot;
    stream.ready_slot = Stream::kNoSlot;
    const std::size_t last = m_ready.size() - 1;
    if (index == last) {
        m_ready.pop_back();
        return;
    }
    m_ready[index] = m_ready[last];
    m_ready[index]->ready_slot = static_cast<std::uint32_t>(index);
    m_ready.pop_back();
    SiftUp(index);
    SiftDown(index);
}

void StreamTable::AddProducer(Stream& stream) noexcept {
    if (stream.producer_slot != Stream::kNoSlot) {
        return;
    }
    stream.producer_slot = static_cast<std::uint32_t>(m_producers.size());
    m_producers.push_back(&stream);
}

void StreamTable::RemoveProducerAt(std::size_t index) noexcept {
    if (index >= m_producers.size()) {
        return;
    }
    Stream* victim = m_producers[index];
    victim->producer_slot = Stream::kNoSlot;
    const std::size_t last = m_producers.size() - 1;
    if (index != last) {
        m_producers[index] = m_producers[last];
        m_producers[index]->producer_slot = static_cast<std::uint32_t>(index);
    }
    m_producers.pop_back();
}

void StreamTable::DetachIndexes(Stream& stream) noexcept {
    RemoveReady(stream);
    if (stream.idle_slot != Stream::kNoSlot) {
        RemoveIdleAt(stream.idle_slot);
    }
    if (stream.producer_slot != Stream::kNoSlot) {
        RemoveProducerAt(stream.producer_slot);
    }
}

void StreamTable::AddIdle(Stream& stream) noexcept {
    if (stream.idle_slot != Stream::kNoSlot) {
        return;  // 已在待补产列表里（幂等）。
    }
    stream.idle_slot = static_cast<std::uint32_t>(m_idle.size());
    m_idle.push_back(&stream);
}

void StreamTable::RemoveIdleAt(std::size_t index) noexcept {
    if (index >= m_idle.size()) {
        return;
    }
    Stream* victim = m_idle[index];
    victim->idle_slot = Stream::kNoSlot;
    const std::size_t last = m_idle.size() - 1;
    if (index != last) {
        m_idle[index] = m_idle[last];
        m_idle[index]->idle_slot = static_cast<std::uint32_t>(index);
    }
    m_idle.pop_back();
}

void StreamTable::DrainIdle(std::uint32_t max_payload) {
    // 为「没有待发帧」的流各补产一帧。稳态下这里只有刚发完的那一条，
    // 因此摊到每帧就是 O(1)；窗口不足的流会留在列表里下轮再试。
    std::size_t i = 0;
    while (i < m_idle.size()) {
        Stream* stream = m_idle[i];
        if (stream->closed() || stream->producer_done || !stream->producer) {
            RemoveIdleAt(i);
            continue;
        }
        if (!stream->has_pending) {
            FillPending(*stream, max_payload);  // 成功时内部入堆。
        }
        if (stream->has_pending || stream->producer_done) {
            RemoveIdleAt(i);
        } else {
            ++i;
        }
    }
}

void StreamTable::RefillReady(std::uint32_t max_payload) {
    m_refill_hint = false;
    DrainIdle(max_payload);
    for (std::size_t i = 0; i < m_producers.size();) {
        Stream* stream = m_producers[i];
        // 已有待发帧的先入堆：生产者结束（producer_done）的流也可能还剩最后
        // 一帧没发出去（窗口不足时「生产完就结束」），不能连带 pending 一起丢。
        if (stream->has_pending && IsReady(*stream)) {
            PushReady(*stream);  // 窗口恢复：把已有待发帧的流重新入堆。
        }
        // 只有「生产者结束且没有待发帧」才算真正做完；producer_done 但仍卡着
        // 最后一帧（窗口不足）的流必须留在候选里，等窗口恢复后重新入堆。
        if (stream->closed() || !stream->producer ||
            (stream->producer_done && !stream->has_pending)) {
            RemoveProducerAt(i);  // 已结束：从候选里摘除（O(1) swap-remove）。
            continue;
        }
        ++i;
    }
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
    ++m_live_streams;
    RecordCreated();
    if (raw->producer) {
        AddProducer(*raw);
        AddIdle(*raw);  // 首次调度前需要生产 HEADERS。
    }
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
    ++m_live_streams;
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
    Stream& stream = *it->second;
    if (stream.state != StreamState::kClosed && m_live_streams > 0) {
        --m_live_streams;
    }
    DetachIndexes(stream);
    m_streams.erase(it);
    return true;
}

void StreamTable::RecordCreated() { ++m_total_created; }

namespace {

/// @brief 把流标记为 closed 并记账（幂等；只在真正转变时计数）。
void MarkClosedLocked(Stream& stream, std::size_t* live, std::uint64_t* total_closed,
                      std::deque<std::uint32_t>* closed_order, std::uint64_t now_ms) {
    if (stream.state == StreamState::kClosed) {
        return;
    }
    stream.state = StreamState::kClosed;
    stream.closed_at_ms = now_ms;
    ++(*total_closed);
    if (*live > 0) {
        --(*live);
    }
    closed_order->push_back(stream.id);
}

}  // namespace

void StreamTable::RefreshState(Stream& stream) noexcept {
    if (!stream.opened) {
        stream.state = StreamState::kIdle;
        return;
    }
    if (stream.local_closed && stream.remote_closed) {
        MarkClosedLocked(stream, &m_live_streams, &m_total_closed, &m_closed_order, NowMs());
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
    MarkClosedLocked(stream, &m_live_streams, &m_total_closed, &m_closed_order, NowMs());
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
    if (stream.producer) {
        AddProducer(stream);
        if (!stream.has_pending) {
            AddIdle(stream);
        }
    } else if (stream.producer_slot != Stream::kNoSlot) {
        RemoveProducerAt(stream.producer_slot);
    }
}

bool StreamTable::FillPending(Stream& stream, std::uint32_t max_payload) {
    if (stream.has_pending) {
        if (IsReady(stream)) {
            PushReady(stream);
        }
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
    // 复用 pending 的缓冲：稳态下这里的 payload 已有足够容量，不再分配。
    Frame& frame = stream.pending;
    frame.Clear();
    if (!stream.producer->Produce(allowed, &frame)) {
        if (stream.producer->done()) {
            stream.producer_done = true;
        }
        return false;
    }
    if (frame.Size() > kMaxFramePayloadLimit || frame.Size() > limit) {
        // 生产者违约：不把非法帧放进调度器。
        frame.Clear();
        stream.producer_done = true;
        return false;
    }
    frame.stream_id = stream.id;
    stream.has_pending = true;
    if (stream.producer->done()) {
        stream.producer_done = true;
    }
    if (IsReady(stream)) {
        PushReady(stream);
    }
    return true;
}

bool StreamTable::IsReady(const Stream& stream) const noexcept {
    if (!stream.has_pending) {
        return false;
    }
    const Frame& frame = stream.pending;
    if (frame.type != FrameType::kData) {
        return true;
    }
    if (frame.Empty()) {
        return true;  // 空 DATA（结束确认）不占窗口。
    }
    const std::int64_t window = SendWindow(stream);
    return window >= static_cast<std::int64_t>(frame.Size());
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
    // 1) 补产：给「上一帧刚发完」的流准备下一帧（稳态只有 1 条 → O(1)）。
    //    这一步让 ready 堆里始终包含全部「可发」的流，WFQ 才能按
    //    finish 选到真正最小的那条，而不是被迫选堆里唯一剩下的。
    // 2) 兜底：堆空或连接窗口刚恢复时做一次全量刷新（低频）。
    if (!m_idle.empty()) {
        DrainIdle(max_payload);
    }
    if (m_ready.empty() || m_refill_hint) {
        RefillReady(max_payload);
    }
    m_held.clear();
    Stream* best = nullptr;
    while (!m_ready.empty()) {
        Stream* stream = m_ready.front();
        RemoveReady(*stream);
        if (stream->opened && !stream->can_send()) {
            continue;  // 已关闭/被重置：不再参与调度。
        }
        if (!IsReady(*stream)) {
            // 窗口在入堆后被吃掉：等对端 WINDOW_UPDATE 后由 refill 重新入堆。
            continue;
        }
        if (HasReadyAncestor(*stream)) {
            m_held.push_back(stream);  // 父优先：本轮先让父流走。
            continue;
        }
        best = stream;
        break;
    }
    for (Stream* stream : m_held) {
        PushReady(*stream);
    }
    m_held.clear();
    if (best != nullptr) {
        // 返回后流仍持有 pending（调用方可能还没写出）：保持在就绪堆里，
        // 下一次 PickNext 仍能选中它——与原「每轮扫全表」的语义一致。
        // 调用方写出帧后必须调 ConsumePending，届时才真正出堆。
        PushReady(*best);
    }
    return best;
}

bool StreamTable::HasWork() const noexcept {
    for (const auto& item : m_streams) {
        const Stream& stream = *item.second;
        if (stream.has_pending) {
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
            !stream.producer_done && !stream.has_pending) {
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

void StreamTable::ConsumePending(Stream& stream) noexcept {
    RemoveReady(stream);
    stream.has_pending = false;
    stream.pending.Clear();  // 保留 payload 容量供下一帧复用。
    if (stream.producer && !stream.producer_done && !stream.closed()) {
        AddIdle(stream);  // 下一帧的生产候选（由 PickNext 补产，保持 O(1)/帧）。
    }
}

void StreamTable::Requeue(Stream& stream) noexcept {
    if (IsReady(stream)) {
        PushReady(stream);
    }
}

void StreamTable::ChargeFinish(Stream& stream, std::size_t bytes) noexcept {
    if (bytes == 0) {
        return;
    }
    const std::uint32_t weight = std::max<std::uint16_t>(1, stream.weight);
    stream.finish += static_cast<std::int64_t>(bytes) * (1 << 20) / weight;
    // 理论上发送后流已出堆；若调用方在其它时机推进信用，这里维持堆序。
    if (stream.ready_slot != Stream::kNoSlot) {
        SiftDown(stream.ready_slot);
    }
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
        // 连接窗口恢复：被阻塞的流要重新参与生产/调度。
        m_refill_hint = true;
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
    if (stream->has_pending && IsReady(*stream)) {
        PushReady(*stream);  // 只重排这一条，不惊动全表。
    }
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
    // 所有流窗口按同一 delta 变化：整体重新评估一次（低频事件）。
    m_refill_hint = true;
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
        if (item.second->has_pending) {
            ++count;
        }
    }
    return count;
}

void StreamTable::PruneClosedStreams() {
    const std::size_t keep = m_config.max_retired_streams;
    if (m_closed_order.size() <= keep) {
        return;  // 绝大多数 Step 走这里：O(1)，不再扫全表。
    }
    // 按关闭顺序 FIFO 淘汰（closed_at_ms 单调，等价于原「保留最近 keep 条」）。
    while (m_closed_order.size() > keep) {
        const std::uint32_t id = m_closed_order.front();
        m_closed_order.pop_front();
        const auto it = m_streams.find(id);
        if (it == m_streams.end()) {
            continue;  // 已被 Erase：懒删除，直接跳过。
        }
        Stream& stream = *it->second;
        if (stream.state != StreamState::kClosed) {
            continue;
        }
        DetachIndexes(stream);
        m_streams.erase(it);
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
