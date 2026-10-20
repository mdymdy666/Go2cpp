/**
 * @file debug_event_ring.cpp
 * @brief 线程局部双段调度事件环的实现（GO2CPP_DEBUG_DUMP=ON 才记录）。
 *
 * ## 并发模型
 *
 * 每个线程独占自己的 `ThreadRing`，写入者只有本线程，读者是任意线程的
 * `Snapshot()`/`ReadSince()`。写入顺序是「先写槽位字段，最后 release 写 head」；
 * 读者先 acquire 读 head 再复制槽位，并用**全局唯一**的 seq 二次校验：
 * 若复制期间槽位被下一轮覆盖，`slot.seq != record.seq`，该条被丢弃而不是
 * 当成撕裂记录返回。
 *
 * ## 两段环形
 *
 * 普通区与关键区各自有独立的 head 与槽位数组，共享全局 seq 计数器。
 * 关键事件选择目标段由 `SchedEventCodeIsCritical()` 决定；两段互不覆盖。
 * 两段容量都是编译期常量，见 debug_event_ring.hpp。
 */

#include "go2cpp/debug/debug_event_ring.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <thread>

namespace go2cpp::debug {
namespace {

/// 普通区容量（与头文件中的编译期常量保持同一来源）。
constexpr std::size_t kNormalSlots = EventRing::kCapacity;
/// 关键区容量。
constexpr std::size_t kCriticalSlots = EventRing::kCriticalCapacity;

/// 普通区槽位数组类型：容量固定为 kNormalSlots。
struct NormalSegment {
    std::atomic<std::uint32_t> head{0};
    EventRecord slots[kNormalSlots];
};

/// 关键区槽位数组类型：容量固定为 kCriticalSlots。
struct CriticalSegment {
    std::atomic<std::uint32_t> head{0};
    EventRecord slots[kCriticalSlots];
};

struct ThreadRing {
    std::atomic<std::uint32_t> tid{0};
    NormalSegment normal;
    CriticalSegment critical;
};

struct Table {
    std::array<std::atomic<ThreadRing*>, EventRing::kMaxThreads> rings{};
    std::atomic<std::size_t> count{0};
    std::atomic<std::uint64_t> dropped_threads{0};
};

/// 堆上常驻：避免 thread_local 析构顺序与静态析构顺序互相踩。
Table& GetTable() {
    static Table* table = new Table();
    return *table;
}

#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP

thread_local ThreadRing t_ring;
/// 当前线程已发布的 P；未绑定为 kInvalidProcessorId。
thread_local std::uint32_t t_processor_id = kInvalidProcessorId;
/// 当前线程正在执行的 G（0=未绑定）。
thread_local std::uint64_t t_bound_task = 0;

/// 原语提示槽（park / wake 各一）：0 表示未设置。
struct HintSlot {
    std::uint16_t reason{0};
    std::uint64_t context{0};
};
thread_local HintSlot t_park_hint;
thread_local HintSlot t_wake_hint;
/// 采样计数器（按事件码低 8 位索引；只在本线程读写）。
thread_local std::uint32_t t_sample_counters[256] = {};

/// 进程级累计事件计数（按事件码）。堆上常驻，避免静态析构顺序问题。
struct EventTotals {
    std::atomic<std::uint64_t> codes[256];
    EventTotals() {
        for (auto& counter : codes) {
            counter.store(0, std::memory_order_relaxed);
        }
    }
};

EventTotals& GetTotals() {
    static EventTotals* totals = new EventTotals();
    return *totals;
}

/**
 * 全局单调 seq：本轮起跨线程唯一。
 * 只在 ON 构建存在；OFF 构建里 `PublishSeq()` 返回 0，不产生任何符号引用。
 */
std::atomic<std::uint64_t> g_publish_seq{0};

void AddRing(ThreadRing* ring) {
    Table& table = GetTable();
    for (auto& slot : table.rings) {
        ThreadRing* expected = nullptr;
        if (slot.compare_exchange_strong(expected, ring,
                                         std::memory_order_acq_rel)) {
            table.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    table.dropped_threads.fetch_add(1, std::memory_order_relaxed);
}

void RemoveRing(ThreadRing* ring) {
    Table& table = GetTable();
    for (auto& slot : table.rings) {
        ThreadRing* expected = ring;
        if (slot.compare_exchange_strong(expected, nullptr,
                                         std::memory_order_acq_rel)) {
            table.count.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
    }
}

struct Registration {
    Registration() {
        t_ring.tid.store(
            static_cast<std::uint32_t>(
                std::hash<std::thread::id>{}(std::this_thread::get_id())),
            std::memory_order_relaxed);
        AddRing(&t_ring);
    }
    ~Registration() { RemoveRing(&t_ring); }
};

thread_local Registration t_registration;

std::uint64_t NowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// 把一条记录写入指定段（调用方保证本线程独占该 ring）。
template <typename SegmentT>
void WriteSlot(SegmentT& segment, const EventRecord& record,
               std::size_t capacity) noexcept {
    const std::uint32_t write_index =
        segment.head.load(std::memory_order_relaxed);
    EventRecord& slot = segment.slots[write_index % capacity];
    slot.code = record.code;
    slot.gid = record.gid;
    slot.arg2 = record.arg2;
    slot.ts_ns = record.ts_ns;
    slot.tid = record.tid;
    slot.pid = record.pid;
    slot.reserved = 0;
    slot.arg3 = record.arg3;
    slot.arg4 = record.arg4;
    // seq 最后写：读者以 seq 作为「本条已完整发布」的判据。
    slot.seq = record.seq;
    segment.head.store(write_index + 1U, std::memory_order_release);
}

/// 采集单段事件（调用方保证 ring 指针有效）。
template <typename SegmentT>
void CollectSegment(const SegmentT& segment, std::size_t capacity,
                    std::vector<EventRecord>& out) {
    const std::uint32_t head = segment.head.load(std::memory_order_acquire);
    const std::uint32_t count = std::min<std::uint32_t>(
        head, static_cast<std::uint32_t>(capacity));
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t index = static_cast<std::uint32_t>(
            (head - count + i) % capacity);
        const EventRecord record = segment.slots[index];
        if (record.seq == 0U) {
            continue;
        }
        // 二次校验 seq：读字段期间被下一轮写入覆盖时丢弃该条，
        // 而不是把撕裂的记录当成真实事件。
        if (segment.slots[index].seq != record.seq) {
            continue;
        }
        out.push_back(record);
    }
}

#endif  // GO2CPP_DEBUG_DUMP

/// wake 事件码判定：ON/OFF 构建都需要（JSON 导出在 OFF 构建也编译）。
/// 只读枚举值、无副作用，放在 #if 外避免 OFF 构建未定义。
bool IsWakeCodeValue(std::uint32_t value) noexcept {
    return value == static_cast<std::uint32_t>(SchedEventCode::kWakeEnqueued) ||
           value == static_cast<std::uint32_t>(SchedEventCode::kWakePending) ||
           value == static_cast<std::uint32_t>(SchedEventCode::kWakeRejected) ||
           value == static_cast<std::uint32_t>(SchedEventCode::kWakeBatch);
}


std::string JsonString(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    out.push_back('"');
    return out;
}

/// 追加进程级累计计数（DumpText 用；键名稳定）。
void AppendTotalCountersText(std::string& out) {
    struct Entry {
        const char* key;
        SchedEventCode code;
    };
    static const Entry kEntries[] = {
        {"total_events", SchedEventCode::kUnknown},
        {"park_total", SchedEventCode::kParkEnter},
        {"park_return_total", SchedEventCode::kParkReturn},
        {"park_rejected_total", SchedEventCode::kParkRejected},
        {"wake_total", SchedEventCode::kWakeEnqueued},
        {"wake_pending_total", SchedEventCode::kWakePending},
        {"wake_rejected_total", SchedEventCode::kWakeRejected},
        {"enqueue_total", SchedEventCode::kEnqueue},
        {"enqueue_rejected_total", SchedEventCode::kEnqueueRejected},
        {"claim_ok_total", SchedEventCode::kRunQueueClaimOk},
        {"claim_fail_total", SchedEventCode::kRunQueueClaimFail},
        {"steal_ok_total", SchedEventCode::kStealOk},
        {"mutex_lock_total", SchedEventCode::kMutexLock},
        {"mutex_unlock_total", SchedEventCode::kMutexUnlock},
        {"mutex_fast_total", SchedEventCode::kMutexFastLock},
        {"channel_send_block_total", SchedEventCode::kChannelSend},
        {"channel_recv_block_total", SchedEventCode::kChannelRecv},
        {"channel_send_ok_total", SchedEventCode::kChannelSendOk},
        {"channel_recv_ok_total", SchedEventCode::kChannelRecvOk},
        {"channel_select_total", SchedEventCode::kChannelSelect},
        {"pc_wait_total", SchedEventCode::kPCWait},
        {"pc_notify_total", SchedEventCode::kPCNotify},
        {"timer_callback_total", SchedEventCode::kTimerCallback},
        {"timer_enqueue_total", SchedEventCode::kTimerEnqueue},
        {"g_created_total", SchedEventCode::kGCreated},
        {"g_started_total", SchedEventCode::kGStarted},
        {"g_completed_total", SchedEventCode::kGCompleted},
        {"g_failed_total", SchedEventCode::kGFailed},
        {"g_cancelled_total", SchedEventCode::kGCancelled},
    };
    for (const auto& entry : kEntries) {
        const std::uint64_t value =
            entry.code == SchedEventCode::kUnknown
                ? EventRing::TotalCountAll()
                : EventRing::TotalCount(entry.code);
        out += std::string(entry.key) + "=" + std::to_string(value) + "\n";
    }
}

/// 进程级累计计数（JSON 对象；键名与文本版一致）。
std::string TotalCountersJson() {
    struct Entry {
        const char* key;
        SchedEventCode code;
    };
    static const Entry kEntries[] = {
        {"total_events", SchedEventCode::kUnknown},
        {"park_total", SchedEventCode::kParkEnter},
        {"park_return_total", SchedEventCode::kParkReturn},
        {"park_rejected_total", SchedEventCode::kParkRejected},
        {"wake_total", SchedEventCode::kWakeEnqueued},
        {"wake_pending_total", SchedEventCode::kWakePending},
        {"wake_rejected_total", SchedEventCode::kWakeRejected},
        {"enqueue_total", SchedEventCode::kEnqueue},
        {"enqueue_rejected_total", SchedEventCode::kEnqueueRejected},
        {"claim_ok_total", SchedEventCode::kRunQueueClaimOk},
        {"claim_fail_total", SchedEventCode::kRunQueueClaimFail},
        {"steal_ok_total", SchedEventCode::kStealOk},
        {"mutex_lock_total", SchedEventCode::kMutexLock},
        {"mutex_unlock_total", SchedEventCode::kMutexUnlock},
        {"mutex_fast_total", SchedEventCode::kMutexFastLock},
        {"channel_send_block_total", SchedEventCode::kChannelSend},
        {"channel_recv_block_total", SchedEventCode::kChannelRecv},
        {"channel_send_ok_total", SchedEventCode::kChannelSendOk},
        {"channel_recv_ok_total", SchedEventCode::kChannelRecvOk},
        {"channel_select_total", SchedEventCode::kChannelSelect},
        {"pc_wait_total", SchedEventCode::kPCWait},
        {"pc_notify_total", SchedEventCode::kPCNotify},
        {"timer_callback_total", SchedEventCode::kTimerCallback},
        {"timer_enqueue_total", SchedEventCode::kTimerEnqueue},
        {"g_created_total", SchedEventCode::kGCreated},
        {"g_started_total", SchedEventCode::kGStarted},
        {"g_completed_total", SchedEventCode::kGCompleted},
        {"g_failed_total", SchedEventCode::kGFailed},
        {"g_cancelled_total", SchedEventCode::kGCancelled},
    };
    std::string out = "{";
    bool first = true;
    for (const auto& entry : kEntries) {
        if (!first) {
            out += ',';
        }
        first = false;
        const std::uint64_t value =
            entry.code == SchedEventCode::kUnknown
                ? EventRing::TotalCountAll()
                : EventRing::TotalCount(entry.code);
        out += std::string("\"") + entry.key + "\":" + std::to_string(value);
    }
    out += '}';
    return out;
}

/// 统计事件中的关键/普通条数（Dump 用）。
void CountBySegment(const std::vector<EventRecord>& events,
                    std::size_t* critical, std::size_t* normal) {
    std::size_t critical_count = 0;
    for (const auto& event : events) {
        if (SchedEventCodeIsCritical(static_cast<SchedEventCode>(event.code))) {
            ++critical_count;
        }
    }
    *critical = critical_count;
    *normal = events.size() - critical_count;
}

}  // namespace

const char* PrimitiveReasonName(PrimitiveReasonCode reason) noexcept {
    switch (reason) {
        case PrimitiveReasonCode::kUnknown: return "kUnknown";
        case PrimitiveReasonCode::kYield: return "kYield";
        case PrimitiveReasonCode::kChannelSend: return "kChannelSend";
        case PrimitiveReasonCode::kChannelRecv: return "kChannelRecv";
        case PrimitiveReasonCode::kSelect: return "kSelect";
        case PrimitiveReasonCode::kMutex: return "kMutex";
        case PrimitiveReasonCode::kParkingCondition: return "kParkingCondition";
        case PrimitiveReasonCode::kIo: return "kIo";
        case PrimitiveReasonCode::kTimer: return "kTimer";
        case PrimitiveReasonCode::kFuture: return "kFuture";
        case PrimitiveReasonCode::kScheduler: return "kScheduler";
        case PrimitiveReasonCode::kWait: return "kWait";
        case PrimitiveReasonCode::kShutdown: return "kShutdown";
    }
    return "kUnknown";
}

const char* SchedEventCodeName(SchedEventCode code) noexcept {
    switch (code) {
        case SchedEventCode::kUnknown: return "kUnknown";
        case SchedEventCode::kParkEnter: return "kParkEnter";
        case SchedEventCode::kParkReturn: return "kParkReturn";
        case SchedEventCode::kParkRejected: return "kParkRejected";
        case SchedEventCode::kWakeRequest: return "kWakeRequest";
        case SchedEventCode::kWakeEnqueued: return "kWakeEnqueued";
        case SchedEventCode::kWakePending: return "kWakePending";
        case SchedEventCode::kWakeRejected: return "kWakeRejected";
        case SchedEventCode::kWakeBatch: return "kWakeBatch";
        case SchedEventCode::kEnqueue: return "kEnqueue";
        case SchedEventCode::kEnqueueRejected: return "kEnqueueRejected";
        case SchedEventCode::kRequeueWorker: return "kRequeueWorker";
        case SchedEventCode::kRequeueProcessor: return "kRequeueProcessor";
        case SchedEventCode::kRunQueuePush: return "kRunQueuePush";
        case SchedEventCode::kRunQueuePop: return "kRunQueuePop";
        case SchedEventCode::kRunQueueClaimOk: return "kRunQueueClaimOk";
        case SchedEventCode::kRunQueueClaimFail: return "kRunQueueClaimFail";
        case SchedEventCode::kIoWait: return "kIoWait";
        case SchedEventCode::kIoWake: return "kIoWake";
        case SchedEventCode::kIoWakeBatch: return "kIoWakeBatch";
        case SchedEventCode::kSchedulerStart: return "kSchedulerStart";
        case SchedEventCode::kSchedulerStop: return "kSchedulerStop";
        case SchedEventCode::kThreadStart: return "kThreadStart";
        case SchedEventCode::kThreadStop: return "kThreadStop";
        case SchedEventCode::kDiagnosticSuspected:
            return "kDiagnosticSuspected";
        case SchedEventCode::kDiagnosticForceWake:
            return "kDiagnosticForceWake";
        case SchedEventCode::kDiagnosticMutexUnlock:
            return "kDiagnosticMutexUnlock";
        case SchedEventCode::kMutexLock: return "kMutexLock";
        case SchedEventCode::kMutexUnlock: return "kMutexUnlock";
        case SchedEventCode::kChannelSend: return "kChannelSend";
        case SchedEventCode::kChannelRecv: return "kChannelRecv";
        case SchedEventCode::kChannelWake: return "kChannelWake";
        case SchedEventCode::kTimerArm: return "kTimerArm";
        case SchedEventCode::kTimerFire: return "kTimerFire";
        case SchedEventCode::kTimerCancel: return "kTimerCancel";
        case SchedEventCode::kTimerReschedule: return "kTimerReschedule";
        case SchedEventCode::kGCreated: return "kGCreated";
        case SchedEventCode::kGStarted: return "kGStarted";
        case SchedEventCode::kGResumed: return "kGResumed";
        case SchedEventCode::kGCompleted: return "kGCompleted";
        case SchedEventCode::kGFailed: return "kGFailed";
        case SchedEventCode::kGCancelled: return "kGCancelled";
        case SchedEventCode::kStealOk: return "kStealOk";
        case SchedEventCode::kStealFail: return "kStealFail";
        case SchedEventCode::kWorkerPark: return "kWorkerPark";
        case SchedEventCode::kWorkerUnpark: return "kWorkerUnpark";
        case SchedEventCode::kSysmonPass: return "kSysmonPass";
        case SchedEventCode::kTimerEnqueue: return "kTimerEnqueue";
        case SchedEventCode::kMutexFastLock: return "kMutexFastLock";
        case SchedEventCode::kChannelSendOk: return "kChannelSendOk";
        case SchedEventCode::kChannelRecvOk: return "kChannelRecvOk";
        case SchedEventCode::kChannelSelectEnter:
            return "kChannelSelectEnter";
        case SchedEventCode::kChannelSelect: return "kChannelSelect";
        case SchedEventCode::kPCWait: return "kPCWait";
        case SchedEventCode::kPCNotify: return "kPCNotify";
        case SchedEventCode::kPCNotifyAll: return "kPCNotifyAll";
        case SchedEventCode::kTimerCallback: return "kTimerCallback";
    }
    return "kUnknown";
}

bool SchedEventCodeIsCritical(SchedEventCode code) noexcept {
    switch (code) {
        // wake 系列：唤醒丢失/重复唤醒是调度器最难查的一类缺陷。
        case SchedEventCode::kWakeRequest:
        case SchedEventCode::kWakeEnqueued:
        case SchedEventCode::kWakePending:
        case SchedEventCode::kWakeRejected:
        case SchedEventCode::kWakeBatch:
        // 入队/认领/ park 失败：出现即意味着状态机走了异常分支。
        case SchedEventCode::kEnqueueRejected:
        case SchedEventCode::kRunQueueClaimFail:
        case SchedEventCode::kParkRejected:
        // Doctor 的受控干预是低频且必须可追溯的事件。
        // G 终态：每个 G 只发生一次，是生命周期回溯的锚点。
        case SchedEventCode::kGCompleted:
        case SchedEventCode::kGFailed:
        case SchedEventCode::kGCancelled:
        // Doctor 的受控干预是低频且必须可追溯的事件。
        case SchedEventCode::kDiagnosticSuspected:
        case SchedEventCode::kDiagnosticForceWake:
        case SchedEventCode::kDiagnosticMutexUnlock:
            return true;
        default:
            return false;
    }
}

bool EventRing::Enabled() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    return true;
#else
    return false;
#endif
}

void EventRing::Record(SchedEventCode code, std::uint64_t gid,
                       std::uint64_t arg2) noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    // 强制 odr-use 注册对象：thread_local 的动态初始化只在首次 odr-use
    // 之前发生；若这里不引用它，编译器可以合法地永不构造它，ring 就
    // 不会被登记到全局表（实测：ON 构建 Dump 为空）。
    (void)t_registration;
    EventRecord record;
    record.seq =
        g_publish_seq.fetch_add(1, std::memory_order_relaxed) + 1U;
    record.ts_ns = NowNs();
    record.gid = gid;
    record.arg2 = arg2;
    record.code = static_cast<std::uint32_t>(code);
    record.tid = t_ring.tid.load(std::memory_order_relaxed);
    record.pid = t_processor_id;
    // 累计计数：环形窗口会被覆盖，覆盖率报告用进程级总量。
    const std::uint32_t code_value = static_cast<std::uint32_t>(code);
    GetTotals().codes[code_value & 0xFFU].fetch_add(
        1, std::memory_order_relaxed);
    // park/wake 事件带上当前线程的原语原因（RAII 作用域写入）。
    if (code == SchedEventCode::kParkEnter) {
        record.arg3 = t_park_hint.reason;
        record.arg4 = t_park_hint.context;
    } else if (IsWakeCodeValue(code_value)) {
        record.arg3 = t_wake_hint.reason;
    }
    if (SchedEventCodeIsCritical(code)) {
        WriteSlot(t_ring.critical, record, kCriticalSlots);
    } else {
        WriteSlot(t_ring.normal, record, kNormalSlots);
    }
#else
    (void)code;
    (void)gid;
    (void)arg2;
#endif
}

void EventRing::RecordTimer(SchedEventCode code, std::uint64_t timer_id,
                            std::uint64_t deadline_ns, std::uint8_t kind,
                            std::uint32_t source_gid) noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    // 与 Record 相同：先 odr-use 注册对象，保证线程 ring 已登记。
    (void)t_registration;
    EventRecord record;
    record.seq =
        g_publish_seq.fetch_add(1, std::memory_order_relaxed) + 1U;
    record.ts_ns = NowNs();
    record.gid = timer_id;
    record.arg2 = deadline_ns;
    record.code = static_cast<std::uint32_t>(code);
    record.tid = t_ring.tid.load(std::memory_order_relaxed);
    record.pid = t_processor_id;
    record.arg3 = kind;
    record.arg4 = source_gid;
    GetTotals().codes[static_cast<std::uint32_t>(code) & 0xFFU].fetch_add(
        1, std::memory_order_relaxed);
    // timer 事件走普通区：10k 级 arm 压测不应挤掉关键区里的 wake 线索。
    WriteSlot(t_ring.normal, record, kNormalSlots);
#else
    (void)code;
    (void)timer_id;
    (void)deadline_ns;
    (void)kind;
    (void)source_gid;
#endif
}

PrimitiveHintScope::PrimitiveHintScope(bool wake_hint,
                                       PrimitiveReasonCode reason,
                                       std::uint64_t context) noexcept
    : m_wake_hint(wake_hint) {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    HintSlot& slot = m_wake_hint ? t_wake_hint : t_park_hint;
    // 外层优先：已有一个未退出的原因作用域时不覆盖。
    if (slot.reason == 0) {
        m_active = true;
        m_prior_reason = static_cast<PrimitiveReasonCode>(slot.reason);
        m_prior_context = slot.context;
        slot.reason = static_cast<std::uint16_t>(reason);
        slot.context = context;
    }
#else
    (void)reason;
    (void)context;
#endif
}

PrimitiveHintScope::~PrimitiveHintScope() {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    if (!m_active) {
        return;
    }
    HintSlot& slot = m_wake_hint ? t_wake_hint : t_park_hint;
    slot.reason = static_cast<std::uint16_t>(m_prior_reason);
    slot.context = m_prior_context;
#endif
}

void EventRing::RecordSampled(SchedEventCode code, std::uint64_t gid,
                              std::uint64_t arg2,
                              std::uint32_t every) noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    if (every <= 1U) {
        Record(code, gid, arg2);
        return;
    }
    std::uint32_t& counter =
        t_sample_counters[static_cast<std::uint32_t>(code) & 0xFFU];
    ++counter;
    if (counter % every != 0U) {
        return;
    }
    Record(code, gid, arg2);
#else
    (void)code;
    (void)gid;
    (void)arg2;
    (void)every;
#endif
}

std::uint64_t EventRing::TotalCount(SchedEventCode code) noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    return GetTotals()
        .codes[static_cast<std::uint32_t>(code) & 0xFFU]
        .load(std::memory_order_relaxed);
#else
    (void)code;
    return 0;
#endif
}

std::uint64_t EventRing::TotalCountAll() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    std::uint64_t total = 0;
    EventTotals& totals = GetTotals();
    for (auto& counter : totals.codes) {
        total += counter.load(std::memory_order_relaxed);
    }
    return total;
#else
    return 0;
#endif
}

void EventRing::ResetTotals() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    EventTotals& totals = GetTotals();
    for (auto& counter : totals.codes) {
        counter.store(0, std::memory_order_relaxed);
    }
#endif
}

void EventRing::BindProcessor(std::uint32_t pid) noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    t_processor_id = pid;
#else
    (void)pid;
#endif
}

void EventRing::UnbindProcessor() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    t_processor_id = kInvalidProcessorId;
#endif
}

std::uint32_t EventRing::BoundProcessor() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    return t_processor_id;
#else
    return kInvalidProcessorId;
#endif
}

void EventRing::BindTask(std::uint64_t gid) noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    t_bound_task = gid;
#else
    (void)gid;
#endif
}

void EventRing::UnbindTask() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    t_bound_task = 0;
#endif
}

std::uint64_t EventRing::BoundTask() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    return t_bound_task;
#else
    return 0;
#endif
}

std::size_t EventRing::ThreadCount() noexcept {
    return GetTable().count.load(std::memory_order_relaxed);
}

std::uint64_t EventRing::DroppedThreads() noexcept {
    return GetTable().dropped_threads.load(std::memory_order_relaxed);
}

std::uint64_t EventRing::PublishSeq() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    return g_publish_seq.load(std::memory_order_acquire);
#else
    return 0;
#endif
}

std::vector<EventRecord> EventRing::Snapshot(std::size_t max) {
    std::vector<EventRecord> events;
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    Table& table = GetTable();
    events.reserve(table.count.load(std::memory_order_relaxed) * 32U + 16U);
    for (auto& slot : table.rings) {
        ThreadRing* ring = slot.load(std::memory_order_acquire);
        if (ring == nullptr) {
            continue;
        }
        CollectSegment(ring->normal, kNormalSlots, events);
        CollectSegment(ring->critical, kCriticalSlots, events);
    }
    std::sort(events.begin(), events.end(),
              [](const EventRecord& lhs, const EventRecord& rhs) {
                  return lhs.seq < rhs.seq;
              });
    if (max != 0U && events.size() > max) {
        events.erase(events.begin(),
                     events.end() - static_cast<std::ptrdiff_t>(max));
    }
#endif
    (void)max;
    return events;
}

std::vector<EventRecord> EventRing::ReadSince(std::uint64_t cursor,
                                              std::size_t max) {
    std::vector<EventRecord> events = Snapshot(0);
    events.erase(std::remove_if(events.begin(), events.end(),
                                [cursor](const EventRecord& event) {
                                    return event.seq <= cursor;
                                }),
                 events.end());
    if (max != 0U && events.size() > max) {
        // 保留最旧的 max 条：订阅端需要按 seq 连续推进游标，
        // 先交付新的会让中间事件永久丢失。
        events.resize(max);
    }
    return events;
}

void EventRing::Reset() noexcept {
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    Table& table = GetTable();
    for (auto& slot : table.rings) {
        ThreadRing* ring = slot.load(std::memory_order_acquire);
        if (ring == nullptr) {
            continue;
        }
        ring->normal.head.store(0, std::memory_order_release);
        ring->critical.head.store(0, std::memory_order_release);
        for (auto& record : ring->normal.slots) {
            record = EventRecord{};
        }
        for (auto& record : ring->critical.slots) {
            record = EventRecord{};
        }
    }
    // 注意：本函数只应在无并发写入时调用；并发时 seq 可能被回退，
    // 导致重复序号（与 v1 的 per-thread seq 行为一致，测试专用）。
    g_publish_seq.store(0, std::memory_order_release);
#endif
}

std::string EventRing::DumpText(std::size_t max) {
    const std::uint64_t seq_start = PublishSeq();
    const std::vector<EventRecord> events = Snapshot(max);
    const std::uint64_t seq_end = PublishSeq();
    std::size_t critical_count = 0;
    std::size_t normal_count = 0;
    CountBySegment(events, &critical_count, &normal_count);
    std::size_t timer_event_count = 0;
    for (const auto& event : events) {
        const auto code = static_cast<SchedEventCode>(event.code);
        if (code == SchedEventCode::kTimerArm ||
            code == SchedEventCode::kTimerFire ||
            code == SchedEventCode::kTimerCancel ||
            code == SchedEventCode::kTimerReschedule) {
            ++timer_event_count;
        }
    }

    std::string out;
    out += "schema=go2cpp.dump.events/1\n";
    out += "version=1\n";
    out += std::string("enabled=") + (Enabled() ? "true" : "false") + "\n";
    out += "capacity=" + std::to_string(kCapacity) + "\n";
    out += "critical_capacity=" + std::to_string(kCriticalCapacity) + "\n";
    out += "total_capacity=" + std::to_string(kTotalCapacity) + "\n";
    out += "critical_policy=isolated\n";
    out += "consistency=per_call\n";
    out += "threads=" + std::to_string(ThreadCount()) + "\n";
    out += "dropped_threads=" + std::to_string(DroppedThreads()) + "\n";
    out += "seq_last=" + std::to_string(seq_end) + "\n";
    out += "seq_at_start=" + std::to_string(seq_start) + "\n";
    out += "seq_at_end=" + std::to_string(seq_end) + "\n";
    out += std::string("changed_during_dump=") +
           (seq_start != seq_end ? "true" : "false") + "\n";
    out += "event_count=" + std::to_string(events.size()) + "\n";
    out += "critical_event_count=" + std::to_string(critical_count) + "\n";
    out += "normal_event_count=" + std::to_string(normal_count) + "\n";
    out += "timer_event_count=" + std::to_string(timer_event_count) + "\n";
    AppendTotalCountersText(out);
    out += "# seq ts_ns tid code gid arg2 pid\n";
    for (const auto& event : events) {
        out += std::to_string(event.seq);
        out += ' ';
        out += std::to_string(event.ts_ns);
        out += ' ';
        out += std::to_string(event.tid);
        out += ' ';
        out += SchedEventCodeName(static_cast<SchedEventCode>(event.code));
        out += ' ';
        out += std::to_string(event.gid);
        out += ' ';
        out += std::to_string(event.arg2);
        out += ' ';
        out += std::to_string(event.pid);
        out += '\n';
    }
    return out;
}

std::string EventRing::DumpJson(std::size_t max) {
    const std::uint64_t seq_start = PublishSeq();
    const std::vector<EventRecord> events = Snapshot(max);
    const std::uint64_t seq_end = PublishSeq();
    std::size_t critical_count = 0;
    std::size_t normal_count = 0;
    CountBySegment(events, &critical_count, &normal_count);
    std::size_t timer_event_count = 0;
    for (const auto& event : events) {
        const auto code = static_cast<SchedEventCode>(event.code);
        if (code == SchedEventCode::kTimerArm ||
            code == SchedEventCode::kTimerFire ||
            code == SchedEventCode::kTimerCancel ||
            code == SchedEventCode::kTimerReschedule) {
            ++timer_event_count;
        }
    }

    std::string out = "{\"schema\":\"go2cpp.dump.events/1\",\"version\":1";
    out += ",\"enabled\":";
    out += Enabled() ? "true" : "false";
    out += ",\"capacity\":" + std::to_string(kCapacity);
    out += ",\"critical_capacity\":" + std::to_string(kCriticalCapacity);
    out += ",\"total_capacity\":" + std::to_string(kTotalCapacity);
    out += ",\"critical_policy\":\"isolated\"";
    out += ",\"consistency\":\"per_call\"";
    out += ",\"threads\":" + std::to_string(ThreadCount());
    out += ",\"dropped_threads\":" + std::to_string(DroppedThreads());
    out += ",\"seq_last\":" + std::to_string(seq_end);
    out += ",\"seq_at_start\":" + std::to_string(seq_start);
    out += ",\"seq_at_end\":" + std::to_string(seq_end);
    out += ",\"changed_during_dump\":";
    out += seq_start != seq_end ? "true" : "false";
    out += ",\"event_count\":" + std::to_string(events.size());
    out += ",\"critical_event_count\":" + std::to_string(critical_count);
    out += ",\"normal_event_count\":" + std::to_string(normal_count);
    out += ",\"timer_event_count\":" + std::to_string(timer_event_count);
    out += ",\"totals\":";
    out += TotalCountersJson();
    out += ",\"events\":[";
    bool first = true;
    for (const auto& event : events) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += "{\"seq\":" + std::to_string(event.seq);
        out += ",\"ts_ns\":" + std::to_string(event.ts_ns);
        out += ",\"tid\":" + std::to_string(event.tid);
        out += ",\"code\":" +
               JsonString(SchedEventCodeName(
                   static_cast<SchedEventCode>(event.code)));
        out += ",\"code_value\":" + std::to_string(event.code);
        out += ",\"gid\":" + std::to_string(event.gid);
        out += ",\"arg2\":" + std::to_string(event.arg2);
        // timer 事件用语义字段名暴露（非 timer 事件恒为 0，字段集合稳定）。
        const auto event_code = static_cast<SchedEventCode>(event.code);
        const bool is_timer_event =
            event_code == SchedEventCode::kTimerArm ||
            event_code == SchedEventCode::kTimerFire ||
            event_code == SchedEventCode::kTimerCancel ||
            event_code == SchedEventCode::kTimerReschedule;
        out += ",\"timer_id\":" +
               std::to_string(is_timer_event ? event.gid : 0);
        out += ",\"timer_kind\":" +
               std::to_string(is_timer_event ? event.arg3 : 0);
        out += ",\"deadline_ns\":" +
               std::to_string(is_timer_event ? event.arg2 : 0);
        out += ",\"source_gid\":" +
               std::to_string(is_timer_event ? event.arg4 : 0);
        out += ",\"pid\":" + std::to_string(event.pid);
        out += ",\"arg3\":" + std::to_string(event.arg3);
        out += ",\"arg4\":" + std::to_string(event.arg4);
        // park/wake 的语义原因（非 park/wake 事件为空串，字段集合稳定）。
        const bool is_park_enter = event_code == SchedEventCode::kParkEnter;
        const bool is_wake_event =
            IsWakeCodeValue(static_cast<std::uint32_t>(event.code));
        std::string primitive_name;
        std::uint64_t primitive_code = 0;
        if (is_park_enter || is_wake_event) {
            primitive_code = event.arg3;
            primitive_name = PrimitiveReasonName(
                static_cast<PrimitiveReasonCode>(event.arg3));
        }
        out += ",\"primitive_reason\":\"" + primitive_name + "\"";
        out += ",\"primitive_reason_code\":" +
               std::to_string(primitive_code);
        out += ",\"park_context\":" +
               std::to_string(is_park_enter ? event.arg4 : 0);
        out += ",\"critical\":";
        out += SchedEventCodeIsCritical(
                   static_cast<SchedEventCode>(event.code))
                   ? "true"
                   : "false";
        out += '}';
    }
    out += "]}";
    return out;
}

bool EventRing::WriteToFile(const std::string& path, bool json,
                            std::string* error, std::size_t max) {
    const std::string content = json ? DumpJson(max) : DumpText(max);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error != nullptr) {
            *error = "cannot open " + path + ": " + std::strerror(errno);
        }
        return false;
    }
    stream.write(content.data(),
                 static_cast<std::streamsize>(content.size()));
    stream.flush();
    if (!stream) {
        if (error != nullptr) {
            *error = "write failed for " + path + ": " + std::strerror(errno);
        }
        return false;
    }
    return true;
}

}  // namespace go2cpp::debug
