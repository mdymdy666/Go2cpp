/**
 * @file recorder.cpp
 * @brief 录制端实现：流式编码 + 逐条 CRC + 整文件滚动 CRC + 故障注入。
 */

#include "replay/recorder.hpp"

#if defined(GO2CPP_REPLAY_LINK_RUNTIME) && GO2CPP_REPLAY_LINK_RUNTIME
#include "go2cpp/debug/debug_resources.hpp"
#endif

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

namespace go2cpp::replay {
namespace {

#if defined(GO2CPP_REPLAY_LINK_RUNTIME) && GO2CPP_REPLAY_LINK_RUNTIME
/// 资源事件 → 事件类型；不支持的事件码返回 false（不录制）。
/// `aux` 输出格式层的固定标志位（方向/广播），与录制点提供的 aux 合并。
bool MapResourceEvent(go2cpp::debug::SchedEventCode code, EventType* type,
                      std::uint64_t* aux) {
    using go2cpp::debug::SchedEventCode;
    const auto flag = [aux](ResourceAuxFlags bit) {
        *aux |= static_cast<std::uint64_t>(bit);
    };
    switch (code) {
        case SchedEventCode::kMutexLock:
        case SchedEventCode::kMutexFastLock:
            *type = EventType::kResMutexLock;
            return true;
        case SchedEventCode::kMutexUnlock:
            *type = EventType::kResMutexUnlock;
            return true;
        case SchedEventCode::kPCWait:
            *type = EventType::kResPCWait;
            return true;
        case SchedEventCode::kPCNotify:
        case SchedEventCode::kPCNotifyAll:
            *type = EventType::kResPCNotify;
            return true;
        case SchedEventCode::kChannelSend:
        case SchedEventCode::kChannelSendOk:
            *type = EventType::kResChannelSend;
            return true;
        case SchedEventCode::kChannelRecv:
        case SchedEventCode::kChannelRecvOk:
            *type = EventType::kResChannelRecv;
            return true;
        // ---- v5：RWMutex ----
        case SchedEventCode::kRwLockRead:
            *type = EventType::kResRwReadLock;
            flag(ResourceAuxFlags::kRead);
            return true;
        case SchedEventCode::kRwUnlockRead:
            *type = EventType::kResRwUnlock;
            flag(ResourceAuxFlags::kRead);
            return true;
        case SchedEventCode::kRwLockWrite:
            *type = EventType::kResRwWriteLock;
            flag(ResourceAuxFlags::kWrite);
            return true;
        case SchedEventCode::kRwUnlockWrite:
            *type = EventType::kResRwUnlock;
            flag(ResourceAuxFlags::kWrite);
            return true;
        // ---- v5：ConditionVariable ----
        case SchedEventCode::kCvWait:
            *type = EventType::kResCvWait;
            return true;
        case SchedEventCode::kCvNotify:
            *type = EventType::kResCvNotify;
            return true;
        case SchedEventCode::kCvNotifyAll:
            *type = EventType::kResCvNotifyAll;
            flag(ResourceAuxFlags::kNotifyAll);
            return true;
        // ---- v5：WaitGroup ----
        case SchedEventCode::kWaitGroupAdd:
            *type = EventType::kResWgAdd;
            return true;
        case SchedEventCode::kWaitGroupDone:
            *type = EventType::kResWgDone;
            return true;
        case SchedEventCode::kWaitGroupWait:
            *type = EventType::kResWgWait;
            return true;
        // ---- v5：IO ----
        case SchedEventCode::kIoRead:
            *type = EventType::kResIoRead;
            flag(ResourceAuxFlags::kRead);
            return true;
        case SchedEventCode::kIoWrite:
            *type = EventType::kResIoWrite;
            flag(ResourceAuxFlags::kWrite);
            return true;
        case SchedEventCode::kIoWait:
            *type = EventType::kResIoWait;
            return true;
        case SchedEventCode::kIoWake:
            *type = EventType::kResIoWake;
            return true;
        default:
            return false;
    }
}

/// 安装在 go2cpp 资源事件记录点上的回调（同步、可重入到 Recorder::Write）。
void ResourceSinkThunk(void* user,
                       const go2cpp::debug::ResourceEventRecord& event) noexcept {
    auto* recorder = static_cast<Recorder*>(user);
    if (recorder == nullptr || !recorder->ok()) {
        return;
    }
    EventType type = EventType::kNote;
    std::uint64_t aux = event.aux;
    if (!MapResourceEvent(event.code, &type, &aux)) {
        return;
    }
    std::uint64_t address = 0;
    if (const void* pointer =
            go2cpp::debug::LookupResourceAddress(event.resource_id)) {
        address = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(pointer));
    }
    recorder->Write(MakeResourceEvent(
        type, event.resource_id, event.actor_gid, event.target_gid,
        static_cast<std::uint32_t>(event.result), address, event.waiters,
        event.fd, event.bytes, aux, static_cast<std::int64_t>(event.value)));
}
#endif  // GO2CPP_REPLAY_LINK_RUNTIME

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

Event MakeTimerServiceEvent(std::uint32_t timer_id, TimerKind kind,
                            std::uint64_t deadline_ns,
                            std::uint64_t fired_at_ns,
                            std::int64_t callback_result,
                            std::uint32_t timer_source,
                            std::uint32_t source_gid) {
    Event event;
    event.type = EventType::kTimer;
    event.gid = source_gid;
    event.channel = kNoChannel;
    event.timer_kind = static_cast<std::uint8_t>(kind);
    event.timer_ns = deadline_ns;
    event.timer_id = timer_id;
    event.timer_source = timer_source;
    event.deadline_ns = deadline_ns;
    event.fired_at_ns = fired_at_ns;
    event.callback_result = callback_result;
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

Event MakeResourceEvent(EventType type, std::uint64_t resource_id,
                        std::uint64_t actor_gid,
                        std::uint64_t target_gid,
                        std::uint32_t resource_result,
                        std::uint64_t address,
                        std::uint32_t waiters,
                        std::int32_t res_fd,
                        std::uint32_t res_bytes,
                        std::uint64_t res_aux,
                        std::int64_t value) {
    Event event;
    event.type = type;
    // gid 字段与 actor_gid 保持一致：旧工具只认识 gid，也能正确分组。
    event.gid = static_cast<std::uint32_t>(actor_gid);
    event.channel = kNoChannel;
    event.resource_id = resource_id;
    event.actor_gid = actor_gid;
    event.target_gid = target_gid;
    event.resource_result = resource_result;
    event.address = address;
    event.waiters = waiters;
    event.res_fd = res_fd;
    event.res_bytes = res_bytes;
    event.res_aux = res_aux;
    event.value = value;
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
#if defined(GO2CPP_REPLAY_LINK_RUNTIME) && GO2CPP_REPLAY_LINK_RUNTIME
    // 文件成功打开后才接管资源事件：资源事件与 channel/select/timer
    // 共用同一 seq 流（seq 由 Write 在文件锁内分配）。
    go2cpp::debug::SetResourceReplaySink(&ResourceSinkThunk, this);
#endif
}

Recorder::~Recorder() {
#if defined(GO2CPP_REPLAY_LINK_RUNTIME) && GO2CPP_REPLAY_LINK_RUNTIME
    // 只卸载本对象安装的汇，避免误清另一个录制器的注册。
    void* current_user = nullptr;
    if (go2cpp::debug::GetResourceReplaySink(&current_user) != nullptr &&
        current_user == this) {
        go2cpp::debug::SetResourceReplaySink(nullptr, nullptr);
    }
#endif
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
            if (!WriteRaw(m_scratch.data(), m_scratch.size())) {
                m_error = "无法写入收尾 note：" + m_error;
            }
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
    if (!m_scratch.empty() && !WriteRaw(m_scratch.data(), m_scratch.size())) {
        m_error = "无法写入 kEnd：" + m_error;
    }
    // fwrite 是缓冲写：/dev/full、磁盘满这类错误通常直到 flush 才暴露。
    // 必须检查 fflush 返回值，否则调用方会把「0 字节文件」当成成功（D3）。
    if (std::fflush(m_file) != 0 && m_error.empty()) {
        m_error = "刷新记录文件失败：" + std::string(std::strerror(errno));
    }
    m_finalized.store(true, std::memory_order_relaxed);
}

}  // namespace go2cpp::replay
