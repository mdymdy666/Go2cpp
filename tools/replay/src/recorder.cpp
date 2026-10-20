/**
 * @file recorder.cpp
 * @brief 录制端实现：流式编码 + 逐条 CRC + 整文件滚动 CRC + 故障注入。
 */

#include "replay/recorder.hpp"

#include <chrono>
#include <utility>

namespace go2cpp::replay {
namespace {

std::uint64_t UnixNowNs() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

}  // namespace

Event MakeOpEvent(EventType type, std::uint32_t gid, std::uint32_t channel,
                  std::uint32_t op_index, Decision decision, std::uint8_t blocked,
                  std::int64_t value, std::uint32_t xfer_id,
                  const std::string& data) {
    Event event;
    event.type = type;
    event.gid = gid;
    event.channel = channel;
    event.op_index = op_index;
    event.decision = decision;
    event.blocked = blocked;
    event.value = value;
    event.data = data;
    event.xfer_id = xfer_id;
    if (decision != Decision::kOk) {
        event.flags |= kFlagFailed;
    }
    return event;
}

Event MakeSelectEvent(std::uint32_t gid, std::uint32_t channel,
                      std::uint32_t op_index, Decision decision,
                      std::uint8_t blocked, std::uint16_t chosen,
                      std::int64_t value, const std::string& data,
                      std::vector<SelectCaseRecord> cases,
                      std::uint32_t xfer_id) {
    Event event;
    event.type = EventType::kSelect;
    event.gid = gid;
    event.channel = channel;
    event.op_index = op_index;
    event.decision = decision;
    event.blocked = blocked;
    event.chosen = chosen;
    event.value = value;
    event.data = data;
    event.cases = std::move(cases);
    event.xfer_id = xfer_id;
    if (decision != Decision::kOk) {
        event.flags |= kFlagFailed;
    }
    return event;
}

Event MakeParkEvent(EventType type, std::uint32_t gid, std::uint32_t channel,
                    ParkReason reason, std::uint32_t op_index,
                    std::uint32_t xfer_id) {
    Event event;
    event.type = type;
    event.gid = gid;
    event.channel = channel;
    event.reason = reason;
    event.op_index = op_index;
    event.xfer_id = xfer_id;
    return event;
}

Event MakeTimerEvent(std::uint32_t gid, TimerKind kind, std::uint64_t timer_ns,
                     std::uint32_t op_index, std::string name) {
    Event event;
    event.type = EventType::kTimer;
    event.gid = gid;
    event.channel = kNoChannel;
    event.timer_kind = static_cast<std::uint8_t>(kind);
    event.timer_ns = timer_ns;
    event.op_index = op_index;
    event.note = std::move(name);
    return event;
}

Event MakeCtxEvent(std::uint32_t ctx_id, ContextEventKind kind,
                   std::uint32_t op_index) {
    Event event;
    event.type = EventType::kCtx;
    event.gid = 0;
    event.channel = kNoChannel;
    event.ctx_id = ctx_id;
    event.ctx_kind = static_cast<std::uint8_t>(kind);
    event.op_index = op_index;
    return event;
}

Recorder::Recorder(RecordOptions options) : m_options(std::move(options)) {
    const std::size_t slots = m_options.gid_count == 0 ? 64U : m_options.gid_count;
    m_op_index = std::vector<std::atomic<std::uint32_t>>(slots);
    m_sleep_index = std::vector<std::atomic<std::uint32_t>>(slots);
    for (auto& counter : m_op_index) {
        counter.store(0, std::memory_order_relaxed);
    }
    for (auto& counter : m_sleep_index) {
        counter.store(0, std::memory_order_relaxed);
    }
    const std::size_t ctx_slots =
        m_options.ctx_count == 0 ? 16U : m_options.ctx_count;
    m_ctx_index = std::vector<std::atomic<std::uint32_t>>(ctx_slots);
    for (auto& counter : m_ctx_index) {
        counter.store(0, std::memory_order_relaxed);
    }
    m_start_ns = NowNs();

    m_file = std::fopen(m_options.path.c_str(), "wb");
    if (m_file == nullptr) {
        m_error = "cannot open '" + m_options.path + "' for writing";
        return;
    }
    FileHeader header;
    header.created_unix_ns = UnixNowNs();
    header.producer_id = 0x67326372706C79ULL;  // "g2crply"
    m_scratch.clear();
    EncodeFileHeader(header, m_scratch);
    if (!WriteRaw(m_scratch.data(), m_scratch.size())) {
        return;
    }
    m_scratch.clear();

    Event meta;
    meta.type = EventType::kMeta;
    meta.gid = m_options.gid_count;
    meta.channel = m_options.channel_count;
    meta.value = m_options.value_kind;
    meta.note = m_options.demo_name;
    meta.ext_flags = kMetaHasValueCodec;
    if (m_options.clock_mode == kClockVirtual) {
        meta.ext_flags |= kMetaVirtualClock;
    }
    meta.clock_mode = m_options.clock_mode;
    (void)Write(meta);
    if (m_options.drop_wake_at > 0) {
        Note(0, kNoChannel,
             "fault-injection: drop wake #" + std::to_string(m_options.drop_wake_at),
             kFlagInjected);
    }
}

Recorder::~Recorder() {
    Finalize("destructor");
    if (m_file != nullptr) {
        std::fclose(m_file);
        m_file = nullptr;
    }
}

std::string Recorder::error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

std::uint64_t Recorder::NowNs() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::uint64_t Recorder::VirtualNowNs() const noexcept {
    const std::uint64_t now = NowNs();
    return now >= m_start_ns ? now - m_start_ns : 0U;
}

bool Recorder::WriteRaw(const std::uint8_t* data, std::size_t size) {
    if (m_file == nullptr) {
        m_error = "file already closed";
        return false;
    }
    if (size == 0) {
        return true;
    }
    const std::size_t written = std::fwrite(data, 1, size, m_file);
    if (written != size) {
        m_error = "short write (" + std::to_string(written) + "/" +
                  std::to_string(size) + ")";
        return false;
    }
    return true;
}

bool Recorder::Write(Event event) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file == nullptr) {
        return false;
    }
    // seq 在文件锁内分配，保证「文件顺序 == seq 顺序」。调用方若已持有
    // channel/select 组锁，则同时保证「seq 顺序 == 线性化顺序」。
    event.seq = m_seq.fetch_add(1, std::memory_order_relaxed);
    if (event.ts_ns == 0) {
        event.ts_ns = NowNs();
    }
    if (event.vtime_ns == 0) {
        event.vtime_ns = VirtualNowNs();
    }
    m_scratch.clear();
    EncodeEvent(event, m_scratch);
    if (m_scratch.empty()) {
        m_error = "encode failed";
        return false;
    }
    m_file_crc = Crc32(m_scratch.data(), m_scratch.size(), m_file_crc);
    if (!WriteRaw(m_scratch.data(), m_scratch.size())) {
        return false;
    }
    m_records.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool Recorder::ShouldDropWake(std::uint32_t gid, std::uint32_t channel) {
    if (m_options.drop_wake_at == 0) {
        return false;
    }
    const std::uint32_t seen = m_wake_seen.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen != m_options.drop_wake_at) {
        return false;
    }
    if (m_wake_dropped.exchange(true, std::memory_order_relaxed)) {
        return false;
    }
    // 记录一条注入标记，但不写 kWake —— 等待者从此不再被唤醒。
    Note(gid, channel,
         "drop-wake: wake #" + std::to_string(seen) + " intentionally lost",
         kFlagInjected);
    return true;
}

void Recorder::Note(std::uint32_t gid, std::uint32_t channel, std::string text,
                    std::uint16_t flags) {
    Event event;
    event.type = EventType::kNote;
    event.gid = gid;
    event.channel = channel;
    event.flags = flags;
    event.note = std::move(text);
    (void)Write(std::move(event));
}

void Recorder::Finalize(const std::string& reason) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file == nullptr) {
        return;
    }
    if (m_finalized.load(std::memory_order_relaxed)) {
        return;
    }
    WriteEndLocked(reason);
}

void Recorder::WriteEndLocked(const std::string& reason) {
    if (!reason.empty()) {
        Event note;
        note.type = EventType::kNote;
        note.flags = kFlagSynthetic;
        note.note = "finalize: " + reason;
        note.seq = m_seq.fetch_add(1, std::memory_order_relaxed);
        note.ts_ns = NowNs();
        note.vtime_ns = VirtualNowNs();
        note.gid = 0;
        note.channel = kNoChannel;
        m_scratch.clear();
        EncodeEvent(note, m_scratch);
        if (!m_scratch.empty()) {
            m_file_crc = Crc32(m_scratch.data(), m_scratch.size(), m_file_crc);
            (void)WriteRaw(m_scratch.data(), m_scratch.size());
            m_records.fetch_add(1, std::memory_order_relaxed);
        }
    }

    Event end;
    end.type = EventType::kEnd;
    end.flags = kFlagSynthetic;
    end.seq = m_seq.fetch_add(1, std::memory_order_relaxed);
    end.ts_ns = NowNs();
    end.vtime_ns = VirtualNowNs();
    end.gid = 0;
    end.channel = kNoChannel;
    end.value = static_cast<std::int64_t>(m_records.load(std::memory_order_relaxed));
    end.aux64 = m_wake_dropped.load(std::memory_order_relaxed) ? 1U : 0U;
    end.sched_type = m_file_crc;  // 整文件滚动 CRC（不含本条 kEnd）
    m_scratch.clear();
    EncodeEvent(end, m_scratch);
    if (!m_scratch.empty()) {
        (void)WriteRaw(m_scratch.data(), m_scratch.size());
    }
    std::fflush(m_file);
    m_finalized.store(true, std::memory_order_relaxed);
}

}  // namespace go2cpp::replay
