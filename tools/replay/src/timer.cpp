/**
 * @file timer.cpp
 * @brief TimerService 录制 / 回放的实现（详见 timer.hpp 的定位与限制）。
 */

#include "replay/timer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace go2cpp::replay {
namespace {

/// live 端真实时钟（steady_clock 纳秒）。
std::uint64_t SteadyNowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string IdText(std::uint32_t timer_id) {
    return std::to_string(timer_id);
}

}  // namespace

const char* TimerSourceName(TimerSource source) noexcept {
    switch (source) {
        case TimerSource::kUnknown: return "kUnknown";
        case TimerSource::kContext: return "kContext";
        case TimerSource::kParkingCondition: return "kParkingCondition";
        case TimerSource::kChannel: return "kChannel";
        case TimerSource::kTimedMutex: return "kTimedMutex";
        case TimerSource::kScheduler: return "kScheduler";
        case TimerSource::kSleep: return "kSleep";
        case TimerSource::kUser: return "kUser";
    }
    return "kUnknown";
}

const char* TimerTraceKindName(TimerKind kind) noexcept {
    switch (kind) {
        case TimerKind::kServiceArm: return "arm";
        case TimerKind::kServiceFire: return "fire";
        case TimerKind::kServiceCancel: return "cancel";
        case TimerKind::kServiceReschedule: return "reschedule";
        default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

ReplayTimerService::ReplayTimerService(Recorder& recorder, ReplayClock& clock)
    : m_recorder(&recorder), m_clock(&clock) {
    m_thread = std::thread([this] { RunLive(); });
}

ReplayTimerService::ReplayTimerService(ReplaySource& source)
    : m_source(&source) {
    m_source->InstallTimerService(this);
    BuildReplayIndexes();
}

ReplayTimerService::~ReplayTimerService() {
    if (m_recorder != nullptr) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        return;
    }
    // replay：不再回头访问 source（它可能已经先于本对象销毁）。
}

void ReplayTimerService::Detach() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_source != nullptr && m_source->TimerService() == this) {
        m_source->InstallTimerService(nullptr);
    }
    m_source = nullptr;
}

// ---------------------------------------------------------------------------
// live：真实线程 + 真实等待
// ---------------------------------------------------------------------------

std::uint64_t ReplayTimerService::RealNowNs() const noexcept {
    return SteadyNowNs();
}

std::uint32_t ReplayTimerService::ArmLive(std::uint64_t deadline_ns,
                                          TimerCallback callback,
                                          std::uint64_t period_ns,
                                          TimerSource source,
                                          std::uint32_t source_gid) {
    if (!callback) {
        SetError("empty timer callback");
        return 0;
    }
    const std::uint32_t timer_id = m_recorder->NextTimerId();
    // 先写 arm 事件：即使随后立刻取消，记录里也有完整的生命周期起点。
    m_recorder->Write(MakeTimerServiceEvent(
        timer_id, TimerKind::kServiceArm, deadline_ns, 0, 0,
        static_cast<std::uint32_t>(source), source_gid));

    LiveTimer entry;
    entry.timer_id = timer_id;
    entry.deadline_ns = deadline_ns;
    entry.period_ns = period_ns;
    entry.callback = std::move(callback);
    entry.timer_source = static_cast<std::uint32_t>(source);
    entry.source_gid = source_gid;

    const std::uint64_t now = m_clock->NowNs();
    const std::uint64_t delay = deadline_ns > now ? deadline_ns - now : 0;
    const std::uint64_t real_deadline = RealNowNs() + delay;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_live_timers[timer_id] = entry;
        m_live_queue.emplace(std::make_pair(real_deadline, timer_id), entry);
    }
    m_cv.notify_all();

    TimerTraceEntry trace;
    trace.timer_id = timer_id;
    trace.kind = TimerKind::kServiceArm;
    trace.deadline_ns = deadline_ns;
    trace.timer_source = entry.timer_source;
    trace.source_gid = source_gid;
    RecordTrace(trace);
    ++m_armed;
    return timer_id;
}

std::uint32_t ReplayTimerService::ScheduleAt(std::uint64_t deadline_ns,
                                             TimerCallback callback,
                                             TimerSource source,
                                             std::uint32_t source_gid) {
    if (m_recorder != nullptr) {
        return ArmLive(deadline_ns, std::move(callback), 0, source, source_gid);
    }

    // replay：消费记录中的下一个 arm，返回记录的 timer_id。
    if (m_arm_cursor >= m_arms.size()) {
        SetError("replay timer arm not found (program schedules more timers "
                 "than the recording)");
        return 0;
    }
    const Event* arm = m_arms[m_arm_cursor++];
    if (arm->timer_kind != static_cast<std::uint8_t>(TimerKind::kServiceArm)) {
        SetError("replay timer arm kind mismatch");
        return 0;
    }
    const std::uint32_t timer_id = arm->timer_id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callbacks[timer_id] = std::move(callback);
    }
    TimerTraceEntry trace;
    trace.timer_id = timer_id;
    trace.kind = TimerKind::kServiceArm;
    trace.deadline_ns = deadline_ns;
    trace.timer_source = static_cast<std::uint32_t>(source);
    trace.source_gid = source_gid;
    trace.seq = arm->seq;
    RecordTrace(trace);
    ++m_armed;
    return timer_id;
}

std::uint32_t ReplayTimerService::ScheduleAfter(std::uint64_t delay_ns,
                                                TimerCallback callback,
                                                TimerSource source,
                                                std::uint32_t source_gid) {
    const std::uint64_t now =
        m_recorder != nullptr ? m_clock->NowNs() : m_source->VirtualNowNs();
    return ScheduleAt(now + delay_ns, std::move(callback), source, source_gid);
}

std::uint32_t ReplayTimerService::ScheduleEvery(std::uint64_t period_ns,
                                                TimerCallback callback,
                                                TimerSource source,
                                                std::uint32_t source_gid) {
    if (period_ns == 0) {
        SetError("timer period must be positive");
        return 0;
    }
    if (m_recorder != nullptr) {
        const std::uint64_t deadline = m_clock->NowNs() + period_ns;
        return ArmLive(deadline, std::move(callback), period_ns, source,
                       source_gid);
    }
    const std::uint32_t timer_id =
        ScheduleAt(m_source->VirtualNowNs() + period_ns, std::move(callback),
                   source, source_gid);
    if (timer_id != 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_periods[timer_id] = period_ns;
    }
    return timer_id;
}

bool ReplayTimerService::Cancel(std::uint32_t timer_id) {
    if (timer_id == 0) {
        return false;
    }
    if (m_recorder == nullptr) {
        // replay：控制流以记录为准，这里只回答「记录里有没有 cancel」。
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& event : m_events) {
            if (event.timer_id == timer_id &&
                event.timer_kind ==
                    static_cast<std::uint8_t>(TimerKind::kServiceCancel)) {
                ++m_cancelled;
                return true;
            }
        }
        return false;
    }

    std::uint64_t deadline_ns = 0;
    std::uint32_t timer_source = 0;
    std::uint32_t source_gid = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (std::find(m_live_running.begin(), m_live_running.end(), timer_id) !=
            m_live_running.end()) {
            // 回调已经领取：不写 cancel，避免记录出现「cancel 在 fire 前」的
            // 假顺序（真实语义是 fire 已经发生）。
            return false;
        }
        const auto found = m_live_timers.find(timer_id);
        if (found == m_live_timers.end()) {
            return false;
        }
        deadline_ns = found->second.deadline_ns;
        timer_source = found->second.timer_source;
        source_gid = found->second.source_gid;
        m_live_timers.erase(found);
        for (auto it = m_live_queue.begin(); it != m_live_queue.end();) {
            if (it->first.second == timer_id) {
                it = m_live_queue.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_cv.notify_all();
    m_recorder->Write(MakeTimerServiceEvent(
        timer_id, TimerKind::kServiceCancel, deadline_ns, 0, 0, timer_source,
        source_gid));
    TimerTraceEntry trace;
    trace.timer_id = timer_id;
    trace.kind = TimerKind::kServiceCancel;
    trace.deadline_ns = deadline_ns;
    trace.timer_source = timer_source;
    trace.source_gid = source_gid;
    RecordTrace(trace);
    ++m_cancelled;
    return true;
}

void ReplayTimerService::RunLive() {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        if (m_stopping) {
            return;
        }
        if (m_live_queue.empty()) {
            m_cv.wait(lock, [this] {
                return m_stopping || !m_live_queue.empty();
            });
            continue;
        }
        const auto head = m_live_queue.begin();
        const std::uint64_t real_deadline = head->first.first;
        const std::uint32_t timer_id = head->first.second;
        const std::uint64_t now = RealNowNs();
        if (now < real_deadline) {
            m_cv.wait_until(
                lock,
                std::chrono::steady_clock::time_point(
                    std::chrono::nanoseconds(real_deadline)),
                [this, real_deadline] {
                    return m_stopping || m_live_queue.empty() ||
                           m_live_queue.begin()->first.first != real_deadline;
                });
            continue;
        }
        const LiveTimer entry = head->second;
        m_live_queue.erase(head);
        m_live_running.push_back(timer_id);
        lock.unlock();
        FireLive(entry);
        lock.lock();
        const auto running =
            std::find(m_live_running.begin(), m_live_running.end(), timer_id);
        if (running != m_live_running.end()) {
            m_live_running.erase(running);
        }
    }
}

void ReplayTimerService::FireLive(const LiveTimer& timer) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_live_timers.find(timer.timer_id) == m_live_timers.end()) {
            return;  // 排队期间被取消
        }
    }
    const std::uint64_t fired_at = m_clock->NowNs();
    std::int64_t result = 0;
    try {
        // 回调内如果操作 SyncChannel，记录归属应与注册该 timer 的 gid 一致；
        // 否则回放的 op 游标会与录制错位。
        ScopedGid gid_guard(timer.source_gid);
        if (timer.callback) {
            result = timer.callback();
        }
    } catch (...) {
        result = -1;  // 异常也记录结果，重放时按记录校验
    }
    m_recorder->Write(MakeTimerServiceEvent(
        timer.timer_id, TimerKind::kServiceFire, timer.deadline_ns, fired_at,
        result, timer.timer_source, timer.source_gid));
    TimerTraceEntry fire_trace;
    fire_trace.timer_id = timer.timer_id;
    fire_trace.kind = TimerKind::kServiceFire;
    fire_trace.deadline_ns = timer.deadline_ns;
    fire_trace.fired_at_ns = fired_at;
    fire_trace.callback_result = result;
    fire_trace.timer_source = timer.timer_source;
    fire_trace.source_gid = timer.source_gid;
    RecordTrace(fire_trace);
    ++m_fired;

    if (timer.period_ns == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_live_timers.erase(timer.timer_id);
        return;
    }

    // 周期：固定延迟（本次回调结束 + period），并记录一次 reschedule。
    const std::uint64_t next_deadline = fired_at + timer.period_ns;
    bool still_active = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_live_timers.find(timer.timer_id);
        if (found != m_live_timers.end()) {
            still_active = true;
            LiveTimer next = timer;
            next.deadline_ns = next_deadline;
            found->second = next;
            m_live_queue.emplace(
                std::make_pair(RealNowNs() + timer.period_ns, timer.timer_id),
                next);
        }
    }
    if (!still_active) {
        return;
    }
    m_recorder->Write(MakeTimerServiceEvent(
        timer.timer_id, TimerKind::kServiceReschedule, next_deadline, 0, 0,
        timer.timer_source, timer.source_gid));
    TimerTraceEntry reschedule_trace;
    reschedule_trace.timer_id = timer.timer_id;
    reschedule_trace.kind = TimerKind::kServiceReschedule;
    reschedule_trace.deadline_ns = next_deadline;
    reschedule_trace.timer_source = timer.timer_source;
    reschedule_trace.source_gid = timer.source_gid;
    RecordTrace(reschedule_trace);
    ++m_rescheduled;
    m_cv.notify_all();
}

// ---------------------------------------------------------------------------
// replay：记录索引 + 事件派发
// ---------------------------------------------------------------------------

void ReplayTimerService::BuildReplayIndexes() {
    const std::vector<Event>& events = m_source->Events();
    for (const auto& event : events) {
        if (event.type != EventType::kTimer) {
            continue;
        }
        const auto kind = static_cast<TimerKind>(event.timer_kind);
        if (kind != TimerKind::kServiceArm &&
            kind != TimerKind::kServiceFire &&
            kind != TimerKind::kServiceCancel &&
            kind != TimerKind::kServiceReschedule) {
            continue;
        }
        m_events.push_back(event);
    }
    // 指针要在 m_events 稳定之后再取（push_back 会让旧指针失效）。
    for (auto& event : m_events) {
        if (static_cast<TimerKind>(event.timer_kind) ==
            TimerKind::kServiceArm) {
            m_arms.push_back(&event);
        }
        TimerTraceEntry entry;
        entry.timer_id = event.timer_id;
        entry.kind = static_cast<TimerKind>(event.timer_kind);
        entry.deadline_ns = event.deadline_ns;
        entry.fired_at_ns = event.fired_at_ns;
        entry.callback_result = event.callback_result;
        entry.timer_source = event.timer_source;
        entry.source_gid = event.gid;
        entry.seq = event.seq;
        m_recorded_trace.push_back(entry);
    }
}

bool ReplayTimerService::DispatchFire(const Event& event) {
    TimerCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cancelled_by_record.count(event.timer_id) != 0 &&
            m_cancelled_by_record[event.timer_id]) {
            // 取消发生在 fire 之前：记录本身不应出现这条 fire；忽略。
            return true;
        }
        const auto found = m_callbacks.find(event.timer_id);
        if (found == m_callbacks.end()) {
            SetError("timer fire without registered callback: id=" +
                     IdText(event.timer_id));
            return false;
        }
        callback = found->second;
    }
    if (m_source != nullptr) {
        m_source->AdvanceVirtualTime(event.fired_at_ns);
    }
    std::int64_t result = 0;
    try {
        if (callback) {
            if (m_source != nullptr) {
                m_source->SetCurrentGidOverride(event.gid);
            }
            result = callback();
        }
    } catch (...) {
        result = -1;
    }
    if (m_source != nullptr) {
        m_source->SetCurrentGidOverride(kNoChannel);
    }
    if (result != event.callback_result) {
        SetError("timer callback result mismatch: id=" + IdText(event.timer_id) +
                 " recorded=" + std::to_string(event.callback_result) +
                 " replayed=" + std::to_string(result));
        return false;
    }
    ++m_fired;
    TimerTraceEntry trace;
    trace.timer_id = event.timer_id;
    trace.kind = TimerKind::kServiceFire;
    trace.deadline_ns = event.deadline_ns;
    trace.fired_at_ns = event.fired_at_ns;
    trace.callback_result = result;
    trace.timer_source = event.timer_source;
    trace.source_gid = event.gid;
    trace.seq = event.seq;
    RecordTrace(trace);
    return true;
}

bool ReplayTimerService::Dispatch(const Event& event) {
    if (event.type != EventType::kTimer) {
        return false;
    }
    const auto kind = static_cast<TimerKind>(event.timer_kind);
    switch (kind) {
        case TimerKind::kServiceArm:
            return true;  // arm 已由 Schedule* 消费
        case TimerKind::kServiceFire:
            return DispatchFire(event);
        case TimerKind::kServiceCancel: {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_cancelled_by_record[event.timer_id] = true;
            }
            ++m_cancelled;
            TimerTraceEntry trace;
            trace.timer_id = event.timer_id;
            trace.kind = TimerKind::kServiceCancel;
            trace.deadline_ns = event.deadline_ns;
            trace.timer_source = event.timer_source;
            trace.source_gid = event.gid;
            trace.seq = event.seq;
            RecordTrace(trace);
            return true;
        }
        case TimerKind::kServiceReschedule:
            if (m_source != nullptr) {
                m_source->AdvanceVirtualTime(event.deadline_ns);
            }
            ++m_rescheduled;
            {
                TimerTraceEntry trace;
                trace.timer_id = event.timer_id;
                trace.kind = TimerKind::kServiceReschedule;
                trace.deadline_ns = event.deadline_ns;
                trace.timer_source = event.timer_source;
                trace.source_gid = event.gid;
                trace.seq = event.seq;
                RecordTrace(trace);
            }
            return true;
        default:
            return false;
    }
}

std::size_t ReplayTimerService::ReplayAll() {
    if (m_source == nullptr) {
        SetError("ReplayAll requires a replay source");
        return 0;
    }
    std::size_t fired = 0;
    while (m_event_cursor < m_events.size()) {
        const Event& event = m_events[m_event_cursor++];
        m_source->AdvanceVirtualTime(event.vtime_ns);
        const auto kind = static_cast<TimerKind>(event.timer_kind);
        if (kind == TimerKind::kServiceArm) {
            continue;
        }
        const bool ok = Dispatch(event);
        if (!ok && !m_error.empty()) {
            return fired;
        }
        if (kind == TimerKind::kServiceFire && ok) {
            ++fired;
        }
    }
    return fired;
}

// ---------------------------------------------------------------------------
// 公共辅助
// ---------------------------------------------------------------------------

void ReplayTimerService::RecordTrace(const TimerTraceEntry& entry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_trace.push_back(entry);
}

void ReplayTimerService::SetError(std::string message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_error.empty()) {
        m_error = std::move(message);
    }
}

bool ReplayTimerService::CompareTraces(
    const std::vector<TimerTraceEntry>& recorded,
    const std::vector<TimerTraceEntry>& replayed, std::string* error) {
    if (recorded.size() != replayed.size()) {
        if (error != nullptr) {
            *error = "timer trace size differs: recorded=" +
                     std::to_string(recorded.size()) + " replayed=" +
                     std::to_string(replayed.size());
        }
        return false;
    }

    // 事件顺序在两种模式下不同：live 是「边注册边取消」，replay 是「先按程序
    // 顺序注册全部、再按记录跑 fire/cancel」。因此分三层比较：
    //   1) arm 序列（程序注册顺序）；
    //   2) fire 序列（全局触发顺序，这是回放正确性的核心）；
    //   3) 每个 timer 组内的事件序列（arm/fire/cancel/reschedule 的相对顺序）。
    std::vector<const TimerTraceEntry*> recorded_arms;
    std::vector<const TimerTraceEntry*> replayed_arms;
    std::vector<const TimerTraceEntry*> recorded_fires;
    std::vector<const TimerTraceEntry*> replayed_fires;
    std::map<std::uint32_t, std::vector<const TimerTraceEntry*>> recorded_by_timer;
    std::map<std::uint32_t, std::vector<const TimerTraceEntry*>> replayed_by_timer;
    for (const auto& entry : recorded) {
        recorded_by_timer[entry.timer_id].push_back(&entry);
        if (entry.kind == TimerKind::kServiceArm) {
            recorded_arms.push_back(&entry);
        } else if (entry.kind == TimerKind::kServiceFire) {
            recorded_fires.push_back(&entry);
        }
    }
    for (const auto& entry : replayed) {
        replayed_by_timer[entry.timer_id].push_back(&entry);
        if (entry.kind == TimerKind::kServiceArm) {
            replayed_arms.push_back(&entry);
        } else if (entry.kind == TimerKind::kServiceFire) {
            replayed_fires.push_back(&entry);
        }
    }

    const auto field_mismatch = [error](const char* what, std::size_t index,
                                        std::uint32_t recorded_id,
                                        std::uint32_t replayed_id,
                                        const char* kind) {
        if (error != nullptr) {
            *error = std::string("timer trace ") + what + "[" +
                     std::to_string(index) + "] differs (" + kind +
                     "): recorded id=" + std::to_string(recorded_id) +
                     " replayed id=" + std::to_string(replayed_id);
        }
        return false;
    };

    if (recorded_arms.size() != replayed_arms.size()) {
        if (error != nullptr) {
            *error = "timer arm count differs: recorded=" +
                     std::to_string(recorded_arms.size()) + " replayed=" +
                     std::to_string(replayed_arms.size());
        }
        return false;
    }
    for (std::size_t index = 0; index < recorded_arms.size(); ++index) {
        const TimerTraceEntry& left = *recorded_arms[index];
        const TimerTraceEntry& right = *replayed_arms[index];
        if (left.timer_id != right.timer_id ||
            left.timer_source != right.timer_source ||
            left.source_gid != right.source_gid) {
            return field_mismatch("arm", index, left.timer_id, right.timer_id,
                                  "arm");
        }
    }

    if (recorded_fires.size() != replayed_fires.size()) {
        if (error != nullptr) {
            *error = "timer fire count differs: recorded=" +
                     std::to_string(recorded_fires.size()) + " replayed=" +
                     std::to_string(replayed_fires.size());
        }
        return false;
    }
    for (std::size_t index = 0; index < recorded_fires.size(); ++index) {
        const TimerTraceEntry& left = *recorded_fires[index];
        const TimerTraceEntry& right = *replayed_fires[index];
        if (left.timer_id != right.timer_id ||
            left.deadline_ns != right.deadline_ns ||
            left.fired_at_ns != right.fired_at_ns ||
            left.callback_result != right.callback_result) {
            return field_mismatch("fire", index, left.timer_id,
                                  right.timer_id, "fire");
        }
    }

    if (recorded_by_timer.size() != replayed_by_timer.size()) {
        if (error != nullptr) {
            *error = "timer id count differs: recorded=" +
                     std::to_string(recorded_by_timer.size()) + " replayed=" +
                     std::to_string(replayed_by_timer.size());
        }
        return false;
    }
    for (const auto& [timer_id, recorded_entries] : recorded_by_timer) {
        const auto found = replayed_by_timer.find(timer_id);
        if (found == replayed_by_timer.end()) {
            if (error != nullptr) {
                *error = "timer id " + std::to_string(timer_id) +
                         " exists only in the recording";
            }
            return false;
        }
        const auto& replayed_entries = found->second;
        if (recorded_entries.size() != replayed_entries.size()) {
            if (error != nullptr) {
                *error = "timer id " + std::to_string(timer_id) +
                         " entry count differs: recorded=" +
                         std::to_string(recorded_entries.size()) +
                         " replayed=" +
                         std::to_string(replayed_entries.size());
            }
            return false;
        }
        for (std::size_t index = 0; index < recorded_entries.size(); ++index) {
            const TimerTraceEntry& left = *recorded_entries[index];
            const TimerTraceEntry& right = *replayed_entries[index];
            if (left.kind != right.kind) {
                return field_mismatch("group", index, left.timer_id,
                                      right.timer_id, "kind");
            }
            switch (left.kind) {
                case TimerKind::kServiceArm:
                    if (left.timer_source != right.timer_source ||
                        left.source_gid != right.source_gid) {
                        return field_mismatch("group-arm", index,
                                              left.timer_id, right.timer_id,
                                              "arm");
                    }
                    break;
                case TimerKind::kServiceCancel:
                    break;
                case TimerKind::kServiceReschedule:
                    if (left.deadline_ns != right.deadline_ns) {
                        return field_mismatch("group-reschedule", index,
                                              left.timer_id, right.timer_id,
                                              "reschedule");
                    }
                    break;
                case TimerKind::kServiceFire:
                    if (left.deadline_ns != right.deadline_ns ||
                        left.fired_at_ns != right.fired_at_ns ||
                        left.callback_result != right.callback_result) {
                        return field_mismatch("group-fire", index,
                                              left.timer_id, right.timer_id,
                                              "fire");
                    }
                    break;
                default:
                    break;
            }
        }
    }
    return true;
}

}  // namespace go2cpp::replay
