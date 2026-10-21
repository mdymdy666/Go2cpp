/**
 * @file replay_engine.cpp
 * @brief 文件加载 + 静态重演 + 单线程完整重放 + 分片/检查点输出。
 */

#include "replay/replay_engine.hpp"

#include "replay/timer.hpp"
#include "replay/ucx.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <unordered_map>
#include <utility>

namespace go2cpp::replay {
namespace {

constexpr std::uint64_t kMaxFileSize = 256ULL * 1024ULL * 1024ULL;
/// 合法 gid 上限（含）。引擎按 gid 建稠密索引（11 个 vector + 每槽位固定
/// 开销），不设上限时一条 `gid=0x7FFFFFFF` 的 kSpawn 就会触发
/// std::bad_alloc 并 abort（P1-3）。65536 覆盖真实记录，索引内存 < 20MiB。
constexpr std::uint32_t kMaxGid = 1U << 16U;

/// 一个消息值（i64 或 bytes）；比较时两者都比，避免摘要碰撞。
struct RecordedValue {
    std::int64_t value{0};
    std::string data;

    bool operator==(const RecordedValue& other) const noexcept {
        return value == other.value && data == other.data;
    }
    bool operator!=(const RecordedValue& other) const noexcept {
        return !(*this == other);
    }
};

struct LoadedFile {
    FileHeader header;
    std::vector<Event> events;
    std::uint64_t file_size{0};
    std::uint32_t computed_crc{0};
    bool complete{false};
    /// 加载失败是否属于「文件内容/格式」问题（而不是 IO/权限）。
    bool format_error{false};
    std::string error;
};

std::uint64_t KeyOf(std::uint32_t gid, std::uint32_t op_index) noexcept {
    return (static_cast<std::uint64_t>(gid) << 32U) | op_index;
}

bool IsCheckpointNote(const Event& event) noexcept {
    return event.type == EventType::kNote &&
           event.note.rfind("checkpoint: part ", 0) == 0;
}

bool IsOperation(EventType type) noexcept {
    return type == EventType::kSend || type == EventType::kRecv ||
           type == EventType::kSelect || type == EventType::kClose;
}

bool SelectChosenKind(const Event& event, SelectCaseKind* kind) noexcept {
    if (event.type != EventType::kSelect || event.cases.empty()) {
        return false;
    }
    if (static_cast<std::size_t>(event.chosen) >= event.cases.size()) {
        return false;
    }
    *kind = event.cases[event.chosen].kind;
    return true;
}

bool IsSendLike(const Event& event) noexcept {
    if (event.type == EventType::kSend) {
        return true;
    }
    SelectCaseKind kind;
    return event.type == EventType::kSelect && SelectChosenKind(event, &kind) &&
           kind == SelectCaseKind::kSend;
}

bool IsRecvLike(const Event& event) noexcept {
    if (event.type == EventType::kRecv) {
        return true;
    }
    SelectCaseKind kind;
    return event.type == EventType::kSelect && SelectChosenKind(event, &kind) &&
           kind == SelectCaseKind::kRecv;
}

std::uint64_t FnvStep(std::uint64_t hash, std::uint64_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

bool LoadFile(const std::string& path, LoadedFile* out) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        out->error = "cannot open '" + path + "': " + std::strerror(errno);
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        out->error = "seek failed";
        return false;
    }
    const long length = std::ftell(file);
    if (length < 0) {
        std::fclose(file);
        out->error = "tell failed";
        return false;
    }
    std::rewind(file);
    out->file_size = static_cast<std::uint64_t>(length);
    if (out->file_size > kMaxFileSize) {
        std::fclose(file);
        out->error = "file too large (>256MiB)";
        return false;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    if (!data.empty()) {
        const std::size_t read = std::fread(data.data(), 1, data.size(), file);
        if (read != data.size()) {
            std::fclose(file);
            out->error = "short read";
            return false;
        }
    }
    std::fclose(file);

    if (!DecodeFileHeader(data.data(), data.size(), &out->header, &out->error)) {
        return false;
    }
    std::size_t offset = kFileHeaderSize;
    const std::size_t body_start = offset;
    std::size_t body_end = offset;
    while (offset + kRecordHeaderSize <= data.size()) {
        Event event;
        std::size_t consumed = 0;
        std::string error;
        if (!DecodeEvent(data.data() + offset, data.size() - offset, &event,
                         &consumed, &error)) {
            out->error = "record at offset " + std::to_string(offset) + ": " + error;
            return false;
        }
        // 资源事件的 gid 字段是 actor_gid（全局 Task 编号，可远超 65536），
        // 它们不进入 per-gid 稠密索引，因此不能按 kMaxGid 拒绝。
        const bool resource_event = IsResourceEventType(event.type);
        if (!resource_event && event.gid > kMaxGid) {
            // 超限 gid 必须在分配任何索引数组之前拒绝：per-gid 稠密数组
            // 会按 max_gid+1 分配，gid=0x7FFFFFFF 会直接 bad_alloc → abort。
            out->format_error = true;
            out->error = "record at offset " + std::to_string(offset) + ": gid " +
                         std::to_string(event.gid) +
                         " exceeds supported maximum " +
                         std::to_string(kMaxGid) + " (dense per-gid index)";
            return false;
        }
        if (event.type == EventType::kEnd) {
            out->complete = true;
            body_end = offset;
            out->computed_crc =
                Crc32(data.data() + body_start, body_end - body_start);
            // kEnd.sched_type 保存整文件滚动 CRC（不含 kEnd 自身）。
            if (static_cast<std::uint32_t>(event.sched_type) != out->computed_crc) {
                out->error = "file checksum mismatch (end=" +
                             std::to_string(event.sched_type) + ", computed=" +
                             std::to_string(out->computed_crc) + ")";
                return false;
            }
            offset += consumed;
            break;
        }
        out->events.push_back(std::move(event));
        offset += consumed;
    }
    if (!out->complete) {
        // 允许截断文件进入分析：`complete=false` 会作为诊断信息出现在报告里，
        // 而不是直接判成格式错误 —— 卡死进程被 kill 时正是这种形态。
        out->computed_crc =
            Crc32(data.data() + body_start, data.size() - body_start);
    }
    return true;
}

void FillBaseMetadata(const LoadedFile& loaded, const std::string& path,
                      ReplayReport* report) {
    report->path = path;
    report->file_size = loaded.file_size;
    report->file_crc = loaded.computed_crc;
    report->checksum_ok = loaded.complete;
    report->complete = loaded.complete;
    report->stats.file_events = loaded.events.size();
    for (const auto& event : loaded.events) {
        if (event.type == EventType::kMeta && !event.note.empty()) {
            report->demo_name = event.note;
            break;
        }
    }
}

void AccumulateTimeline(const Event& event, ReplayReport* report) {
    TimelineEntry entry;
    entry.seq = event.seq;
    entry.ts_ns = event.ts_ns;
    entry.vtime_ns = event.vtime_ns;
    entry.type = event.type;
    entry.flags = event.flags;
    entry.gid = event.gid;
    entry.channel = event.channel;
    entry.op_index = event.op_index;
    entry.value = event.value;
    entry.data = event.data;
    entry.decision = event.decision;
    entry.blocked = event.blocked;
    entry.reason = event.reason;
    entry.chosen = event.chosen;
    report->timeline.push_back(entry);
}

/// 检查点收集（静态分析与完整重放共用同一规则）。
class CheckpointCollector {
public:
    explicit CheckpointCollector(const ReplayOptions& options,
                                 std::vector<CheckpointInfo>* out)
        : m_options(options), m_out(out), m_next_ns(options.checkpoint_every_ns) {}

    void Maybe(const Event& event, std::uint64_t index, std::uint64_t virtual_ns) {
        bool due = false;
        if (m_options.checkpoint_every_events > 0 &&
            (index + 1U) % m_options.checkpoint_every_events == 0U) {
            due = true;
        }
        if (m_options.checkpoint_every_ns > 0 && virtual_ns >= m_next_ns) {
            due = true;
        }
        if (!due) {
            return;
        }
        m_out->push_back(CheckpointInfo{event.seq, index, virtual_ns, event.ts_ns});
        if (m_options.checkpoint_every_ns > 0) {
            m_next_ns = virtual_ns + m_options.checkpoint_every_ns;
        }
    }

private:
    const ReplayOptions& m_options;
    std::vector<CheckpointInfo>* m_out;
    std::uint64_t m_next_ns{0};
};


/// 资源事件判定（v4 的 6 种 + v5 的 13 种；谓词由 format.hpp 统一维护）。
bool IsResourceEvent(EventType type) noexcept {
    return IsResourceEventType(type);
}

/// 静态重演的资源状态：owner / 加锁时间 / lock 条目下标 / 等待者。
struct ResourceReplayState {
    std::map<std::uint64_t, std::uint64_t> owner;
    std::map<std::uint64_t, std::uint64_t> lock_vtime;
    std::map<std::uint64_t, std::size_t> lock_entry;
    std::map<std::uint64_t, std::vector<std::uint64_t>> waiters;
    /// v5：RWMutex 的读者数（读锁可多持有者，因此不用 owner）。
    std::map<std::uint64_t, std::uint32_t> rw_readers;
    /// v5：WaitGroup 的当前计数（用于校验 value 字段与事件序列自洽）。
    std::map<std::uint64_t, std::int64_t> wg_count;
};

/// v5：aux 里是否带某个标志位（低位）。
bool HasAux(const ResourceTimelineEntry& entry, ResourceAuxFlags flag) noexcept {
    return (entry.aux & static_cast<std::uint64_t>(flag)) != 0U;
}

void EraseWaiter(std::vector<std::uint64_t>* waiters, std::uint64_t gid) {
    if (waiters == nullptr || gid == 0U) {
        return;
    }
    waiters->erase(std::remove(waiters->begin(), waiters->end(), gid),
                   waiters->end());
}

/**
 * @brief 累计一条资源事件：统计、序列校验、时间线条目。
 *
 * 只读重演：不重新竞争，owner 由记录中的 lock/unlock 维护。校验规则：
 * - unlock 必须能对应到当前 owner（否则记录自相矛盾）；
 * - unlock 的 target（若非 0）从等待者集合移除；
 * - 成对 lock/unlock 的虚拟时间差累计为持锁时长。
 */
void AccumulateResourceEvent(const Event& event, ReplayReport* report,
                             ResourceReplayState* state) {
    ++report->stats.resource_events;
    const std::uint64_t key = event.resource_id;
    ResourceTimelineEntry entry;
    entry.seq = event.seq;
    entry.vtime_ns = event.vtime_ns;
    entry.type = event.type;
    entry.resource_id = key;
    // 旧录制（v1~v3）没有 actor_gid；回退到 gid 字段。
    entry.actor_gid = event.actor_gid != 0U ? event.actor_gid : event.gid;
    entry.target_gid = event.target_gid;
    entry.result = event.resource_result;
    entry.address = event.address;
    entry.waiters = event.waiters;
    entry.fd = event.res_fd;
    entry.bytes = event.res_bytes;
    entry.aux = event.res_aux;
    entry.value = event.value;

    switch (event.type) {
        case EventType::kResMutexLock:
            ++report->stats.res_mutex_locks;
            if (event.resource_result == 0U) {
                state->owner[key] = entry.actor_gid;
                state->lock_vtime[key] = event.vtime_ns;
                state->lock_entry[key] = report->resources.size();
            } else if (event.resource_result == 1U) {
                state->waiters[key].push_back(entry.actor_gid);
            }
            break;
        case EventType::kResMutexUnlock: {
            ++report->stats.res_mutex_unlocks;
            const auto owner_it = state->owner.find(key);
            // gid 0 是合法 actor（主线程），因此只按「是否存在」判断。
            //
            // 录制端的 lock/unlock 记录点在 CAS 之后（锁外），并发时
            // 记录顺序可能比线性化顺序晚一拍：A 的 unlock 已经真实
            // 释放、B 的 lock 已经成功，但 A 的 unlock 事件写入
            // recorder 更晚。这种窗口在只读重演里无法也不应判成
            // 记录损坏 —— 只计一次「顺序异常」，序列本身照原样输出。
            if (owner_it == state->owner.end()) {
                ++report->stats.resource_order_anomalies;
            } else if (owner_it->second != entry.actor_gid) {
                ++report->stats.resource_order_anomalies;
            } else {
                const auto lock_it = state->lock_vtime.find(key);
                if (lock_it != state->lock_vtime.end()) {
                    const std::uint64_t hold =
                        event.vtime_ns >= lock_it->second
                            ? event.vtime_ns - lock_it->second
                            : 0U;
                    report->stats.resource_lock_hold_ns += hold;
                    const auto entry_it = state->lock_entry.find(key);
                    if (entry_it != state->lock_entry.end() &&
                        entry_it->second < report->resources.size() &&
                        report->resources[entry_it->second].hold_ns == 0U) {
                        report->resources[entry_it->second].hold_ns = hold;
                    }
                }
                state->owner.erase(owner_it);
                state->lock_vtime.erase(key);
                state->lock_entry.erase(key);
            }
            EraseWaiter(&state->waiters[key], entry.target_gid);
            break;
        }
        case EventType::kResPCWait:
            ++report->stats.res_pc_waits;
            state->waiters[key].push_back(entry.actor_gid);
            break;
        case EventType::kResPCNotify:
            ++report->stats.res_pc_notifies;
            EraseWaiter(&state->waiters[key], entry.target_gid);
            break;
        case EventType::kResChannelSend:
            ++report->stats.res_channel_sends;
            break;
        case EventType::kResChannelRecv:
            ++report->stats.res_channel_recvs;
            break;
        // ---- v5：RWMutex ----
        case EventType::kResRwReadLock:
            if (event.resource_result == 0U) {
                ++report->stats.res_rw_reads;
                ++state->rw_readers[key];
                state->waiters[key].erase(
                    std::remove(state->waiters[key].begin(),
                                state->waiters[key].end(), entry.actor_gid),
                    state->waiters[key].end());
            } else {
                ++report->stats.res_rw_read_blocks;
                state->waiters[key].push_back(entry.actor_gid);
            }
            break;
        case EventType::kResRwWriteLock:
            if (event.resource_result == 0U) {
                ++report->stats.res_rw_writes;
                state->owner[key] = entry.actor_gid;
                state->waiters[key].erase(
                    std::remove(state->waiters[key].begin(),
                                state->waiters[key].end(), entry.actor_gid),
                    state->waiters[key].end());
            } else {
                ++report->stats.res_rw_write_blocks;
                state->waiters[key].push_back(entry.actor_gid);
            }
            break;
        case EventType::kResRwUnlock: {
            ++report->stats.res_rw_unlocks;
            if (HasAux(entry, ResourceAuxFlags::kRead)) {
                auto& readers = state->rw_readers[key];
                if (readers == 0U) {
                    ++report->stats.resource_order_anomalies;
                } else {
                    --readers;
                }
            } else {
                const auto owner_it = state->owner.find(key);
                if (owner_it == state->owner.end() ||
                    owner_it->second != entry.actor_gid) {
                    ++report->stats.resource_order_anomalies;
                } else {
                    state->owner.erase(owner_it);
                }
            }
            EraseWaiter(&state->waiters[key], entry.target_gid);
            break;
        }
        // ---- v5：ConditionVariable ----
        case EventType::kResCvWait:
            ++report->stats.res_cv_waits;
            if (event.resource_result == 1U) {
                state->waiters[key].push_back(entry.actor_gid);
            }
            break;
        case EventType::kResCvNotify:
            ++report->stats.res_cv_notifies;
            if (entry.target_gid != 0U) {
                ++report->stats.res_cv_woken;
                EraseWaiter(&state->waiters[key], entry.target_gid);
            }
            break;
        case EventType::kResCvNotifyAll:
            ++report->stats.res_cv_notify_alls;
            report->stats.res_cv_woken += static_cast<std::uint64_t>(
                entry.value > 0 ? entry.value : 0);
            // 广播把队列整体换出：等待者集合清空。
            state->waiters[key].clear();
            break;
        // ---- v5：WaitGroup ----
        case EventType::kResWgAdd:
            ++report->stats.res_wg_adds;
            state->wg_count[key] +=
                static_cast<std::int64_t>(entry.aux & 0xFFFFFFFFULL);
            break;
        case EventType::kResWgDone: {
            ++report->stats.res_wg_dones;
            const auto delta = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(entry.aux & 0xFFFFFFFFULL));
            state->wg_count[key] += delta;
            // Done 唤醒等待者时 target 就是被唤醒的 gid；等待者集合在此移除。
            EraseWaiter(&state->waiters[key], entry.target_gid);
            break;
        }
        case EventType::kResWgWait:
            ++report->stats.res_wg_waits;
            if (event.resource_result == 1U) {
                state->waiters[key].push_back(entry.actor_gid);
            }
            break;
        // ---- v5：IO ----
        case EventType::kResIoRead:
            ++report->stats.res_io_reads;
            report->stats.res_io_bytes += entry.bytes;
            break;
        case EventType::kResIoWrite:
            ++report->stats.res_io_writes;
            report->stats.res_io_bytes += entry.bytes;
            break;
        case EventType::kResIoWait:
            ++report->stats.res_io_waits;
            break;
        case EventType::kResIoWake:
            ++report->stats.res_io_wakes;
            report->stats.res_io_bytes += entry.bytes;
            break;
        default:
            break;
    }

    report->stats.resource_checksum = FnvStep(
        report->stats.resource_checksum,
        (static_cast<std::uint64_t>(event.type) << 48U) ^ (key & 0xFFFFFFFFFFFFULL));
    report->stats.resource_checksum =
        FnvStep(report->stats.resource_checksum, entry.actor_gid);
    report->stats.resource_checksum =
        FnvStep(report->stats.resource_checksum, entry.target_gid);
    report->stats.resource_checksum = FnvStep(
        report->stats.resource_checksum,
        (static_cast<std::uint64_t>(entry.result) << 32U) ^ entry.waiters);
    if (IsResourceEventV5(event.type)) {
        // v5 事件把扩展字段也纳入校验和：fd / bytes / aux / value 任一错位
        // 都会让 checksum 与录制端不一致。v4 事件不混入，保证旧文件 checksum
        // 与 v4 工具逐位一致。
        report->stats.res_v5_checksum =
            FnvStep(report->stats.res_v5_checksum,
                    static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(entry.fd)));
        report->stats.res_v5_checksum =
            FnvStep(report->stats.res_v5_checksum, entry.bytes);
        report->stats.res_v5_checksum =
            FnvStep(report->stats.res_v5_checksum, entry.aux);
        report->stats.res_v5_checksum = FnvStep(
            report->stats.res_v5_checksum,
            static_cast<std::uint64_t>(entry.value));
    }
    report->resources.push_back(entry);
}

/// 静态分析：park/wake 配对、blocked 操作配对、xfer 配对、退出计数、select 自洽。
ReplayReport AnalyzeLoaded(const LoadedFile& loaded, const ReplayOptions& options,
                           const std::string& path) {
    ReplayReport report;
    FillBaseMetadata(loaded, path, &report);
    // FNV-1a 64 的 offset basis：demo 侧用同一算法直接比对。
    report.stats.message_checksum = 1469598103934665603ULL;
    report.stats.select_checksum = 1469598103934665603ULL;
    report.stats.resource_checksum = 1469598103934665603ULL;
    report.stats.res_v5_checksum = 1469598103934665603ULL;

    std::map<std::uint64_t, const Event*> parks;
    std::vector<const Event*> wakes;
    ResourceReplayState resource_state;
    std::map<std::uint32_t, const Event*> xfer_owners;
    std::uint32_t max_gid = 0;
    for (const auto& event : loaded.events) {
        if (IsResourceEvent(event.type)) {
            continue;  // 资源事件的 gid 是 actor_gid，不进稠密索引
        }
        max_gid = std::max(max_gid, event.gid);
    }
    if (max_gid > kMaxGid) {
        // LoadFile 已拒绝；这里再守一道，保证任何调用路径都不会按未校验
        // 的 gid 分配数组。
        report.status = ReplayStatus::kFormatError;
        report.error = "gid " + std::to_string(max_gid) +
                       " exceeds supported maximum " + std::to_string(kMaxGid);
        return report;
    }
    report.stats.per_gid.resize(static_cast<std::size_t>(max_gid) + 1U);
    report.stats.recv_by_gid.resize(static_cast<std::size_t>(max_gid) + 1U);
    for (std::uint32_t gid = 0; gid <= max_gid; ++gid) {
        report.stats.per_gid[gid].gid = gid;
    }

    CheckpointCollector checkpoints(options, &report.checkpoints);
    const bool has_stop = options.stop_at_seq != 0;
    std::uint64_t virtual_ns = 0;
    std::size_t index = 0;
    for (; index < loaded.events.size(); ++index) {
        const Event& event = loaded.events[index];
        if (has_stop && event.seq > options.stop_at_seq) {
            break;
        }
        virtual_ns = std::max(virtual_ns, event.vtime_ns);
        if (event.type == EventType::kTimer) {
            const auto kind = static_cast<TimerKind>(event.timer_kind);
            if (kind == TimerKind::kSleep) {
                virtual_ns = std::max(virtual_ns, event.vtime_ns + event.timer_ns);
            } else if (kind == TimerKind::kServiceFire) {
                virtual_ns = std::max(virtual_ns, event.fired_at_ns);
            } else if (kind == TimerKind::kServiceReschedule) {
                virtual_ns = std::max(virtual_ns, event.deadline_ns);
            }
        }
        checkpoints.Maybe(event, index, virtual_ns);
        if (options.keep_timeline) {
            AccumulateTimeline(event, &report);
        }
        if (((event.flags & kFlagSynthetic) != 0U) && event.type != EventType::kNote) {
            ++report.stats.synthetic_events;
        }
        GidStats* gid_stats = event.gid <= max_gid
                                  ? &report.stats.per_gid[event.gid]
                                  : nullptr;
        switch (event.type) {
            case EventType::kSpawn:
                if (gid_stats != nullptr) gid_stats->started = true;
                break;
            case EventType::kExit:
                if (gid_stats != nullptr) {
                    gid_stats->exited = true;
                    if (event.value != static_cast<std::int64_t>(gid_stats->sends) ||
                        static_cast<std::int64_t>(event.aux64) !=
                            static_cast<std::int64_t>(gid_stats->recvs)) {
                        report.status = ReplayStatus::kMismatch;
                        report.error = "gid " + std::to_string(event.gid) +
                                       " exit counters mismatch (recorded " +
                                       std::to_string(event.value) + "/" +
                                       std::to_string(event.aux64) + ", events " +
                                       std::to_string(gid_stats->sends) + "/" +
                                       std::to_string(gid_stats->recvs) + ")";
                    }
                }
                break;
            case EventType::kSend:
                ++report.stats.sends;
                if (gid_stats != nullptr) ++gid_stats->sends;
                break;
            case EventType::kRecv:
                ++report.stats.recvs;
                if (gid_stats != nullptr) ++gid_stats->recvs;
                if (event.decision == Decision::kOk) {
                    ++report.stats.messages;
                    if (!event.data.empty()) ++report.stats.bytes_messages;
                    report.stats.message_checksum =
                        FnvStep(report.stats.message_checksum,
                                static_cast<std::uint64_t>(event.value));
                    report.stats.global_recv_sequence.push_back(event.value);
                    report.stats.recv_by_gid[event.gid].push_back(event.value);
                }
                break;
            case EventType::kSelect: {
                ++report.stats.selects;
                if (gid_stats != nullptr) ++gid_stats->selects;
                report.stats.select_choices.push_back(event.chosen);
                report.stats.select_checksum = FnvStep(
                    report.stats.select_checksum, static_cast<std::uint64_t>(event.chosen));
                SelectCaseKind kind;
                const bool has_kind = SelectChosenKind(event, &kind);
                if (!event.cases.empty() &&
                    static_cast<std::size_t>(event.chosen) >= event.cases.size()) {
                    report.status = ReplayStatus::kMismatch;
                    report.error = "select chosen " + std::to_string(event.chosen) +
                                   " out of range (cases=" +
                                   std::to_string(event.cases.size()) + ")";
                } else if (has_kind && kind == SelectCaseKind::kRecv &&
                           event.decision == Decision::kOk) {
                    ++report.stats.recvs;
                    ++report.stats.messages;
                    if (gid_stats != nullptr) ++gid_stats->recvs;
                    if (!event.data.empty()) ++report.stats.bytes_messages;
                    report.stats.message_checksum =
                        FnvStep(report.stats.message_checksum,
                                static_cast<std::uint64_t>(event.value));
                    report.stats.global_recv_sequence.push_back(event.value);
                    report.stats.recv_by_gid[event.gid].push_back(event.value);
                } else if (has_kind && kind == SelectCaseKind::kSend &&
                           event.decision == Decision::kOk) {
                    ++report.stats.sends;
                    if (gid_stats != nullptr) ++gid_stats->sends;
                }
                break;
            }
            case EventType::kClose:
                ++report.stats.closes;
                break;
            case EventType::kTimer: {
                ++report.stats.timers;
                const auto timer_kind =
                    static_cast<TimerKind>(event.timer_kind);
                switch (timer_kind) {
                    case TimerKind::kServiceArm:
                        ++report.stats.timer_arms;
                        break;
                    case TimerKind::kServiceFire:
                        ++report.stats.timer_fires;
                        report.stats.timer_fire_ids.push_back(
                            event.timer_id);
                        report.stats.timer_checksum = FnvStep(
                            report.stats.timer_checksum,
                            (static_cast<std::uint64_t>(
                                 event.timer_id)
                             << 32U) ^
                                static_cast<std::uint32_t>(
                                    event.callback_result));
                        if (event.fired_at_ns > virtual_ns) {
                            virtual_ns = event.fired_at_ns;
                        }
                        break;
                    case TimerKind::kServiceCancel:
                        ++report.stats.timer_cancels;
                        break;
                    case TimerKind::kServiceReschedule:
                        ++report.stats.timer_reschedules;
                        if (event.deadline_ns > virtual_ns) {
                            virtual_ns = event.deadline_ns;
                        }
                        break;
                    default:
                        break;
                }
                break;
            }
            case EventType::kCtx:
                ++report.stats.ctx_events;
                break;
            case EventType::kPark:
                ++report.stats.parks;
                if (gid_stats != nullptr) ++gid_stats->parks;
                parks[KeyOf(event.gid, event.op_index)] = &event;
                break;
            case EventType::kWake:
                ++report.stats.wakes;
                if (gid_stats != nullptr) ++gid_stats->wakes;
                wakes.push_back(&event);
                break;
            case EventType::kResMutexLock:
            case EventType::kResMutexUnlock:
            case EventType::kResPCWait:
            case EventType::kResPCNotify:
            case EventType::kResChannelSend:
            case EventType::kResChannelRecv:
            case EventType::kResRwReadLock:
            case EventType::kResRwWriteLock:
            case EventType::kResRwUnlock:
            case EventType::kResCvWait:
            case EventType::kResCvNotify:
            case EventType::kResCvNotifyAll:
            case EventType::kResWgAdd:
            case EventType::kResWgDone:
            case EventType::kResWgWait:
            case EventType::kResIoRead:
            case EventType::kResIoWrite:
            case EventType::kResIoWait:
            case EventType::kResIoWake:
                AccumulateResourceEvent(event, &report, &resource_state);
                break;
            default:
                break;
        }
        if (IsOperation(event.type)) {
            if (event.blocked != 0U) {
                ++report.stats.blocked_ops;
            }
            if (event.xfer_id != 0U) {
                const auto [it, inserted] = xfer_owners.emplace(event.xfer_id, &event);
                if (!inserted) {
                    const Event* first = it->second;
                    const bool same_channel = first->channel == event.channel;
                    const bool paired =
                        same_channel &&
                        ((IsSendLike(*first) && IsRecvLike(event)) ||
                         (IsRecvLike(*first) && IsSendLike(event)));
                    if (!paired) {
                        report.status = ReplayStatus::kMismatch;
                        report.error = "xfer_id " + std::to_string(event.xfer_id) +
                                       " is not a send/recv pair on the same channel";
                    }
                }
            }
        }
    }
    report.stopped_at_seq = options.stop_at_seq;
    report.stopped_early = has_stop && index < loaded.events.size();
    report.virtual_ns = virtual_ns;
    // 静态重演同样是「只读重演」：资源事件序列完全来自记录，
    // 重放侧校验和与记录值一致（完整重放也会再写一次同值）。
    report.stats.replayed_resource_checksum =
        report.stats.resource_checksum;
    // 分片文件的最后一条是 `kNote("checkpoint: part ...")`：它标记「到此
    // 为止」而不是「记录损坏」，因此同样按 partial 处理。
    if (!report.stopped_early && !loaded.events.empty() &&
        IsCheckpointNote(loaded.events.back())) {
        report.stopped_early = true;
        report.stopped_at_seq = loaded.events.back().seq > 0
                                    ? loaded.events.back().seq - 1
                                    : 0;
    }

    if (report.status == ReplayStatus::kMismatch) {
        return report;
    }

    // 故障注入：丢弃第 N 个 kWake，然后按静态配对判定卡死。
    std::set<std::uint64_t> alive_wakes;
    for (const Event* wake : wakes) {
        const std::uint64_t key = KeyOf(wake->gid, wake->op_index);
        if (options.drop_wake_at != 0 && report.stats.dropped_wakes + 1U ==
                                                options.drop_wake_at) {
            ++report.stats.dropped_wakes;
            report.error = "injected: dropped wake #" +
                           std::to_string(options.drop_wake_at) + " for gid " +
                           std::to_string(wake->gid) + " op " +
                           std::to_string(wake->op_index);
            continue;
        }
        alive_wakes.insert(key);
    }

    // 只重放到出事点时，前缀里必然有「尚未被唤醒」的 park，不能判 stuck。
    if (!report.stopped_early) {
        for (const auto& [key, park] : parks) {
            if (alive_wakes.count(key) == 0U) {
                report.stuck = true;
                report.stuck_gid = park->gid;
                report.stuck_channel = park->channel;
                report.stuck_op_index = park->op_index;
                report.stuck_reason =
                    std::string("park at gid ") + std::to_string(park->gid) +
                    " op " + std::to_string(park->op_index) + " (" +
                    ParkReasonName(park->reason) + ") has no matching wake";
                break;
            }
        }
        if (!report.stuck) {
            for (const auto& event : loaded.events) {
                if (has_stop && event.seq > options.stop_at_seq) {
                    break;
                }
                if (!IsOperation(event.type) || event.blocked == 0U) {
                    continue;
                }
                if (parks.count(KeyOf(event.gid, event.op_index)) == 0U) {
                    report.stuck = true;
                    report.stuck_gid = event.gid;
                    report.stuck_channel = event.channel;
                    report.stuck_op_index = event.op_index;
                    report.stuck_reason =
                        "blocked operation on gid " + std::to_string(event.gid) +
                        " op " + std::to_string(event.op_index) +
                        " has no park event";
                    break;
                }
            }
        }
    }

    if (!loaded.complete) {
        report.error = report.error.empty()
                           ? "recording is truncated (no kEnd)"
                           : report.error;
        report.status = ReplayStatus::kStuck;
        report.stuck = true;
        return report;
    }
    if (report.stuck) {
        report.status = ReplayStatus::kStuck;
        return report;
    }
    report.status = report.stopped_early ? ReplayStatus::kPartial : ReplayStatus::kOk;
    return report;
}

/**
 * @brief 单线程完整重放：ucontext 承载 fiber，记录驱动 park/wake。
 *
 * 与静态重演的区别：这里**真正执行原程序的 fiber 函数**。channel 操作不
 * 再是「读一条记录」这么简单——它通过 `ReplaySource` 查 (gid, op_index) 的
 * 记录，阻塞时 `ParkCurrent` 把 CPU 交还引擎；引擎在 `kWake` 处 resume。
 * 因此最终消息序列是「程序逻辑 × 记录交错」共同的结果，可用于交叉验证。
 */
class Engine final : public ReplaySource {
public:
    Engine(std::vector<Event> events, const FiberEntryFactory& factory,
           const ReplayOptions& options)
        : m_events(std::move(events)), m_factory(factory), m_options(options) {
        m_stats.message_checksum = 1469598103934665603ULL;
        m_stats.select_checksum = 1469598103934665603ULL;
        m_stats.replayed_select_checksum = 1469598103934665603ULL;
        BuildIndexes();
    }

    ReplayReport Run(const ReplayReport& base);

    void InstallTimerService(ReplayTimerService* service) override {
        m_timers = service;
    }

    ReplayTimerService* TimerService() override { return m_timers; }

    void SetCurrentGidOverride(std::uint32_t gid) override {
        m_gid_override = gid;
    }

    const std::vector<Event>& Events() const override {
        return m_events;
    }

    const Event* NextOperation(std::uint32_t gid) override {
        if (gid >= m_ops.size()) {
            SetError("operation for gid " + std::to_string(gid) + " out of range");
            return nullptr;
        }
        auto& list = m_ops[gid];
        auto& cursor = m_cursor[gid];
        if (cursor >= list.size()) {
            if (m_options.stop_at_seq != 0) {
                m_stopped_early = true;
                m_abort = true;
                return nullptr;
            }
            m_stuck = true;
            m_stuck_gid = gid;
            m_stuck_reason = "recording ends while gid " + std::to_string(gid) +
                             " still has channel operations to perform";
            SetError(m_stuck_reason);
            return nullptr;
        }
        return list[cursor++];
    }

    const Event* NextSleep(std::uint32_t gid) override {
        if (gid >= m_sleeps.size()) {
            return nullptr;
        }
        auto& list = m_sleeps[gid];
        auto& cursor = m_sleep_cursor[gid];
        if (cursor >= list.size()) {
            if (m_options.stop_at_seq != 0) {
                m_stopped_early = true;
                m_abort = true;
                return nullptr;
            }
            SetError("recording ends while gid " + std::to_string(gid) +
                     " still has sleeps to perform");
            return nullptr;
        }
        return list[cursor++];
    }

    std::uint64_t VirtualNowNs() const noexcept override { return m_virtual_now; }

    void AdvanceVirtualTime(std::uint64_t virtual_ns) override {
        if (virtual_ns > m_virtual_now) {
            m_virtual_now = virtual_ns;
        }
    }

    Decision ContextDecision(std::uint32_t ctx_id) override {
        const auto found = m_ctx_events.find(ctx_id);
        if (found == m_ctx_events.end()) {
            return Decision::kOk;
        }
        std::size_t& cursor = m_ctx_cursor[ctx_id];
        const auto& list = found->second;
        while (cursor < list.size()) {
            const Event* event = list[cursor++];
            const auto kind = static_cast<ContextEventKind>(event->ctx_kind);
            if (kind == ContextEventKind::kCancelled) {
                return Decision::kCancelled;
            }
            if (kind == ContextEventKind::kDeadlineExceeded) {
                return Decision::kTimedOut;
            }
        }
        return Decision::kOk;
    }

    void ParkCurrent(std::uint32_t gid, const Event& operation) override {
        (void)operation;
        if (m_abort) {
            m_ucx.ExitCurrent();
            return;
        }
        if (gid >= m_parked.size()) {
            SetError("park for out-of-range gid");
            m_ucx.ExitCurrent();
            return;
        }
        m_parked[gid] = true;
        ++m_stats.parks;
        m_ucx.Park();
        m_parked[gid] = false;
        if (m_abort) {
            m_ucx.ExitCurrent();
        }
    }

    void RecordResult(std::uint32_t gid, const Event& operation,
                      std::int64_t value, const std::string& data) override {
        if (gid >= m_actual_recv.size()) {
            return;
        }
        const RecordedValue recorded{value, data};
        if (operation.type == EventType::kRecv) {
            ++m_stats.recvs;
            if (operation.decision == Decision::kOk) {
                m_actual_recv[gid].push_back(recorded);
                ++m_stats.messages;
                m_stats.message_checksum = FnvStep(
                    m_stats.message_checksum, static_cast<std::uint64_t>(value));
                m_stats.global_recv_sequence.push_back(value);
            }
            return;
        }
        if (operation.type == EventType::kSend) {
            ++m_stats.sends;
            if (operation.decision == Decision::kOk) {
                m_actual_send[gid].push_back(recorded);
            }
            return;
        }
        if (operation.type == EventType::kSelect) {
            m_actual_selects[gid].push_back(operation.chosen);
            m_stats.replayed_select_choices.push_back(operation.chosen);
            m_stats.replayed_select_checksum = FnvStep(
                m_stats.replayed_select_checksum,
                static_cast<std::uint64_t>(operation.chosen));
            SelectCaseKind kind;
            const bool has_kind = SelectChosenKind(operation, &kind);
            if (has_kind && kind == SelectCaseKind::kRecv) {
                ++m_stats.recvs;
                if (operation.decision == Decision::kOk) {
                    m_actual_recv[gid].push_back(recorded);
                    ++m_stats.messages;
                    m_stats.message_checksum = FnvStep(
                        m_stats.message_checksum, static_cast<std::uint64_t>(value));
                    m_stats.global_recv_sequence.push_back(value);
                }
            } else if (has_kind && kind == SelectCaseKind::kSend) {
                ++m_stats.sends;
                if (operation.decision == Decision::kOk) {
                    m_actual_send[gid].push_back(recorded);
                }
            }
            return;
        }
    }

    const char* LastError() const override { return m_error.c_str(); }

    std::uint32_t CurrentFiberGid() const noexcept override {
        if (m_gid_override != kNoChannel) {
            return m_gid_override;
        }
        const std::uint32_t ucx_id = m_ucx.current();
        return ucx_id < m_gid_by_ucx.size() ? m_gid_by_ucx[ucx_id] : kNoChannel;
    }

private:
    static TimelineEntry MakeTimeline(const Event& event) {
        TimelineEntry entry;
        entry.seq = event.seq;
        entry.ts_ns = event.ts_ns;
        entry.vtime_ns = event.vtime_ns;
        entry.type = event.type;
        entry.flags = event.flags;
        entry.gid = event.gid;
        entry.channel = event.channel;
        entry.op_index = event.op_index;
        entry.value = event.value;
        entry.data = event.data;
        entry.decision = event.decision;
        entry.blocked = event.blocked;
        entry.reason = event.reason;
        entry.chosen = event.chosen;
        return entry;
    }

    void BuildIndexes() {
        std::uint32_t max_gid = 0;
        for (const auto& event : m_events) {
            if (IsResourceEvent(event.type)) {
                continue;  // 资源事件不参与 per-gid 索引
            }
            max_gid = std::max(max_gid, event.gid);
        }
        if (max_gid > kMaxGid) {
            // 构造函数没有返回值：记下错误并让 Run() 直接返回
            // format-error，绝不按未校验的 gid 分配索引（P1-3）。
            SetError("gid " + std::to_string(max_gid) +
                     " exceeds supported maximum " +
                     std::to_string(kMaxGid));
            return;
        }
        const std::size_t slots = static_cast<std::size_t>(max_gid) + 1U;
        m_ops.resize(slots);
        m_sleeps.resize(slots);
        m_expected_recv.resize(slots);
        m_expected_send.resize(slots);
        m_expected_selects.resize(slots);
        m_actual_recv.resize(slots);
        m_actual_send.resize(slots);
        m_actual_selects.resize(slots);
        m_cursor.assign(slots, 0);
        m_sleep_cursor.assign(slots, 0);
        m_started.assign(slots, false);
        m_done.assign(slots, false);
        m_parked.assign(slots, false);
        m_ucx_id.assign(slots, UcontextScheduler::kNoFiber);
        for (const auto& event : m_events) {
            if (IsOperation(event.type)) {
                m_ops[event.gid].push_back(&event);
            }
            if (event.type == EventType::kTimer &&
                event.timer_kind == static_cast<std::uint8_t>(TimerKind::kSleep)) {
                m_sleeps[event.gid].push_back(&event);
            }
            if (event.type == EventType::kCtx) {
                m_ctx_events[event.ctx_id].push_back(&event);
            }
            const RecordedValue recorded{event.value, event.data};
            if (event.type == EventType::kRecv && event.decision == Decision::kOk) {
                m_expected_recv[event.gid].push_back(recorded);
            } else if (event.type == EventType::kSend &&
                       event.decision == Decision::kOk) {
                m_expected_send[event.gid].push_back(recorded);
            } else if (event.type == EventType::kSelect) {
                m_expected_selects[event.gid].push_back(event.chosen);
                SelectCaseKind kind;
                if (SelectChosenKind(event, &kind)) {
                    if (kind == SelectCaseKind::kRecv &&
                        event.decision == Decision::kOk) {
                        m_expected_recv[event.gid].push_back(recorded);
                    } else if (kind == SelectCaseKind::kSend &&
                               event.decision == Decision::kOk) {
                        m_expected_send[event.gid].push_back(recorded);
                    }
                }
            }
        }
    }

    void SetError(std::string message) {
        if (!m_abort) {
            m_error = std::move(message);
        }
        m_abort = true;
    }

    void ResumeGid(std::uint32_t gid) {
        if (gid >= m_ucx_id.size() || m_ucx_id[gid] == UcontextScheduler::kNoFiber) {
            SetError("resume invalid gid " + std::to_string(gid));
            return;
        }
        m_ucx.Resume(m_ucx_id[gid]);
        if (m_ucx.done(m_ucx_id[gid])) {
            m_done[gid] = true;
        }
        if (!m_ucx.error().empty()) {
            SetError(m_ucx.error());
        }
    }

    void StartFiber(const Event& spawn) {
        const std::uint32_t gid = spawn.gid;
        if (gid >= m_started.size()) {
            SetError("spawn gid out of range");
            return;
        }
        if (m_started[gid]) {
            SetError("duplicate spawn for gid " + std::to_string(gid));
            return;
        }
        if (!m_factory) {
            SetError("no fiber entry factory provided");
            return;
        }
        m_started[gid] = true;
        // 逻辑 gid 与 ucontext fiber id 是两套编号：kSpawn 的到达顺序由真实
        // 调度决定（可能 1 先于 0），因此必须显式建立双向映射。
        const std::uint32_t ucx_id = m_ucx.Create(m_factory(gid, *this));
        if (ucx_id == UcontextScheduler::kNoFiber) {
            SetError("failed to create fiber for gid " + std::to_string(gid));
            return;
        }
        m_ucx_id[gid] = ucx_id;
        if (m_gid_by_ucx.size() <= ucx_id) {
            m_gid_by_ucx.resize(static_cast<std::size_t>(ucx_id) + 1U,
                                UcontextScheduler::kNoFiber);
        }
        m_gid_by_ucx[ucx_id] = gid;
        ResumeGid(gid);
    }

    void OnPark(const Event& park) {
        if (park.gid >= m_parked.size()) {
            SetError("park gid out of range");
            return;
        }
        if (m_done[park.gid]) {
            SetError("park after fiber done for gid " + std::to_string(park.gid));
            return;
        }
        if (!m_parked[park.gid]) {
            SetError("park event but fiber is not at a park point (gid " +
                     std::to_string(park.gid) + " op " +
                     std::to_string(park.op_index) + ")");
        }
    }

    void OnTimer(const Event& event) {
        const auto kind = static_cast<TimerKind>(event.timer_kind);
        if (kind == TimerKind::kServiceFire) {
            m_virtual_now = std::max(m_virtual_now, event.fired_at_ns);
        } else if (kind == TimerKind::kServiceReschedule) {
            m_virtual_now = std::max(m_virtual_now, event.deadline_ns);
        }
        if (m_timers == nullptr) {
            return;
        }
        // timer 回调在引擎线程执行；回调内 park 型操作不在支持范围
        // （见 replay/timer.hpp 的限制说明）。
        if (!m_timers->Dispatch(event) && !m_timers->error().empty()) {
            SetError(m_timers->error());
        }
    }

    void OnWake(const Event& wake) {
        ++m_stats.wakes;
        ++m_wake_seen;
        if (m_options.drop_wake_at != 0 && m_wake_seen == m_options.drop_wake_at &&
            !m_injected) {
            m_injected = true;
            ++m_stats.dropped_wakes;
            m_injected_gid = wake.gid;
            m_injected_channel = wake.channel;
            m_injected_op = wake.op_index;
            return;  // 故意不 resume：重演「唤醒丢失」
        }
        if (wake.gid >= m_parked.size() || !m_parked[wake.gid]) {
            SetError("wake without a parked fiber for gid " +
                     std::to_string(wake.gid));
            return;
        }
        ResumeGid(wake.gid);
    }

    void OnExit(const Event& exit) {
        if (exit.gid < m_done.size() && m_done[exit.gid]) {
            return;
        }
        if (exit.gid < m_parked.size() && m_parked[exit.gid]) {
            m_stuck = true;
            m_stuck_gid = exit.gid;
            m_stuck_reason = "gid " + std::to_string(exit.gid) +
                             " is still parked at exit event (lost wake)";
            SetError(m_stuck_reason);
            return;
        }
        SetError("exit event but fiber is not done for gid " +
                 std::to_string(exit.gid));
    }

    std::vector<Event> m_events;
    const FiberEntryFactory& m_factory;
    ReplayOptions m_options;
    UcontextScheduler m_ucx;
    std::vector<std::vector<const Event*>> m_ops;
    std::vector<std::vector<const Event*>> m_sleeps;
    std::vector<std::uint32_t> m_cursor;
    std::vector<std::uint32_t> m_sleep_cursor;
    std::vector<bool> m_started;
    std::vector<bool> m_done;
    std::vector<bool> m_parked;
    std::vector<std::uint32_t> m_ucx_id;   ///< 逻辑 gid → ucontext fiber id
    std::vector<std::uint32_t> m_gid_by_ucx;  ///< ucontext fiber id → 逻辑 gid
    std::vector<std::vector<RecordedValue>> m_expected_recv;
    std::vector<std::vector<RecordedValue>> m_expected_send;
    std::vector<std::vector<std::uint32_t>> m_expected_selects;
    std::vector<std::vector<RecordedValue>> m_actual_recv;
    std::vector<std::vector<RecordedValue>> m_actual_send;
    std::vector<std::vector<std::uint32_t>> m_actual_selects;
    std::unordered_map<std::uint32_t, std::vector<const Event*>> m_ctx_events;
    std::unordered_map<std::uint32_t, std::size_t> m_ctx_cursor;
    ReplayStats m_stats;
    ReplayTimerService* m_timers{nullptr}; ///< 由用户 fiber 构造并安装
    std::uint32_t m_gid_override{kNoChannel}; ///< timer 回调期间强制 gid
    std::string m_error;
    bool m_abort{false};
    bool m_stuck{false};
    std::uint32_t m_stuck_gid{kNoChannel};
    std::string m_stuck_reason;
    std::uint64_t m_wake_seen{0};
    bool m_injected{false};
    std::uint32_t m_injected_gid{kNoChannel};
    std::uint32_t m_injected_channel{kNoChannel};
    std::uint32_t m_injected_op{0};
    std::uint64_t m_virtual_now{0};
    bool m_stopped_early{false};
    std::uint64_t m_stopped_at_seq{0};
    std::vector<CheckpointInfo> m_checkpoints;
};

ReplayReport Engine::Run(const ReplayReport& base) {
    // timer 服务可能比引擎活得久（demo 里是全局对象）：Run 结束时解除绑定，
    // 避免它在 source 销毁后回头访问 this。
    struct TimerDetachGuard {
        ReplayTimerService* service;
        ~TimerDetachGuard() {
            if (service != nullptr) {
                service->Detach();
            }
        }
    } timer_guard{m_timers};
    ReplayReport report = base;
    if (m_abort) {
        // 索引构建阶段就拒绝了输入（gid 超限等）：这是文件格式问题，
        // 返回结构化错误而不是崩溃（P1-3）。
        report.status = ReplayStatus::kFormatError;
        report.replay_executed = false;
        report.error = m_error.empty() ? "index-build-failed" : m_error;
        return report;
    }
    report.replay_executed = true;
    report.timeline.clear();
    report.stuck = false;
    report.checkpoints.clear();

    CheckpointCollector checkpoints(m_options, &m_checkpoints);
    std::size_t index = 0;
    for (; index < m_events.size(); ++index) {
        if (m_abort) {
            break;
        }
        const Event& event = m_events[index];
        if (m_options.stop_at_seq != 0 && event.seq > m_options.stop_at_seq) {
            m_stopped_early = true;
            m_stopped_at_seq = m_options.stop_at_seq;
            break;
        }
        m_virtual_now = std::max(m_virtual_now, event.vtime_ns);
        if (event.type == EventType::kTimer &&
            event.timer_kind == static_cast<std::uint8_t>(TimerKind::kSleep)) {
            m_virtual_now = std::max(m_virtual_now, event.vtime_ns + event.timer_ns);
        }
        checkpoints.Maybe(event, index, m_virtual_now);
        if (m_options.keep_timeline) {
            report.timeline.push_back(MakeTimeline(event));
        }
        switch (event.type) {
            case EventType::kSpawn:
                StartFiber(event);
                break;
            case EventType::kPark:
                OnPark(event);
                break;
            case EventType::kWake:
                OnWake(event);
                break;
            case EventType::kExit:
                OnExit(event);
                break;
            case EventType::kTimer:
                OnTimer(event);
                break;
            default:
                break;
        }
    }
    if (!m_stopped_early && index >= m_events.size() && m_options.stop_at_seq != 0) {
        // stop_at_seq 大于最后一条事件的 seq：等价于完整重放。
        m_stopped_early = false;
    }

    report.stats.parks = m_stats.parks;
    report.stats.wakes = m_stats.wakes;
    report.stats.dropped_wakes = m_stats.dropped_wakes;
    report.stats.messages = m_stats.messages;
    // 保留 base（记录）的接收序列与校验和，重放结果单独放一份用于对照。
    report.stats.replayed_message_checksum = m_stats.message_checksum;
    report.stats.replayed_recv_sequence = m_stats.global_recv_sequence;
    report.stats.sends = m_stats.sends;
    report.stats.recvs = m_stats.recvs;
    report.stats.replayed_select_choices = m_stats.replayed_select_choices;
    report.stats.replayed_select_checksum = m_stats.replayed_select_checksum;
    // 资源事件只读重演：输出序列就是记录序列，校验和必然相同；
    // 显式写一份，便于调用方一次比较「录制 vs 重放」。
    report.stats.replayed_resource_checksum =
        base.stats.resource_checksum;
    report.stats.res_v5_checksum = base.stats.res_v5_checksum;
    if (m_timers != nullptr) {
        // 与静态分析的初值保持一致（ReplayStats 默认 0 起）——
        // 两边同一条 fire 序列必须得到同一个 checksum。
        report.stats.replayed_timer_checksum = 0;
        for (const auto& entry : m_timers->trace()) {
            if (entry.kind != TimerKind::kServiceFire) {
                continue;
            }
            report.stats.replayed_timer_fire_ids.push_back(
                entry.timer_id);
            report.stats.replayed_timer_checksum = FnvStep(
                report.stats.replayed_timer_checksum,
                (static_cast<std::uint64_t>(entry.timer_id) << 32U) ^
                    static_cast<std::uint32_t>(
                        entry.callback_result));
        }
    }
    report.checkpoints = m_checkpoints;
    report.virtual_ns = m_virtual_now;
    report.stopped_early = m_stopped_early;
    report.stopped_at_seq = m_stopped_at_seq;
    report.stats.per_gid.resize(m_started.size());
    report.stats.recv_by_gid.resize(m_started.size());
    for (std::size_t gid = 0; gid < m_started.size(); ++gid) {
        GidStats stats;
        stats.gid = static_cast<std::uint32_t>(gid);
        stats.started = m_started[gid];
        stats.exited = m_done[gid];
        stats.sends = m_actual_send[gid].size();
        stats.recvs = m_actual_recv[gid].size();
        stats.selects = m_actual_selects[gid].size();
        report.stats.per_gid[gid] = stats;
        report.stats.recv_by_gid[gid] = {};
        for (const auto& value : m_actual_recv[gid]) {
            report.stats.recv_by_gid[gid].push_back(value.value);
        }
    }

    if (m_stopped_early) {
        // 只重放到出事点：fiber 停在任意状态都是预期结果，不判 stuck；
        // 但已经执行的部分必须与记录逐项一致（前缀比较）。
        for (std::size_t gid = 0; gid < m_expected_recv.size(); ++gid) {
            const auto& actual = m_actual_recv[gid];
            const auto& expected = m_expected_recv[gid];
            for (std::size_t i = 0; i < actual.size() && i < expected.size(); ++i) {
                if (actual[i] != expected[i]) {
                    report.status = ReplayStatus::kMismatch;
                    report.error = "gid " + std::to_string(gid) +
                                   " receive sequence differs before stop point";
                    return report;
                }
            }
        }
        report.status = ReplayStatus::kPartial;
        return report;
    }

    if (m_stuck) {
        report.status = ReplayStatus::kStuck;
        report.stuck = true;
        report.stuck_gid = m_stuck_gid;
        report.stuck_reason = m_stuck_reason;
        report.error = m_error;
        return report;
    }
    if (m_abort) {
        report.status = ReplayStatus::kMismatch;
        report.error = m_error;
        return report;
    }

    for (std::size_t gid = 0; gid < m_started.size(); ++gid) {
        if (m_started[gid] && !m_done[gid]) {
            report.stuck = true;
            report.stuck_gid = static_cast<std::uint32_t>(gid);
            report.stuck_channel = m_injected_gid == gid ? m_injected_channel : kNoChannel;
            report.stuck_op_index = m_injected_gid == gid ? m_injected_op : 0;
            report.stuck_reason = "fiber gid " + std::to_string(gid) +
                                  " did not finish: blocked at end of recording";
            report.status = ReplayStatus::kStuck;
            if (!report.error.empty()) {
                report.error += "; ";
            }
            report.error += report.stuck_reason;
            return report;
        }
    }

    for (std::size_t gid = 0; gid < m_expected_recv.size(); ++gid) {
        if (m_actual_recv[gid] != m_expected_recv[gid]) {
            report.status = ReplayStatus::kMismatch;
            report.error = "gid " + std::to_string(gid) +
                           " receive sequence differs from recording";
            return report;
        }
        if (m_actual_send[gid] != m_expected_send[gid]) {
            report.status = ReplayStatus::kMismatch;
            report.error = "gid " + std::to_string(gid) +
                           " send sequence differs from recording";
            return report;
        }
        if (m_actual_selects[gid] != m_expected_selects[gid]) {
            report.status = ReplayStatus::kMismatch;
            report.error = "gid " + std::to_string(gid) +
                           " select decisions differ from recording";
            return report;
        }
    }

    if (m_timers != nullptr &&
        report.stats.timer_fires !=
            report.stats.replayed_timer_fire_ids.size()) {
        report.status = ReplayStatus::kMismatch;
        report.error = "timer fire count differs from recording (recorded=" +
                       std::to_string(report.stats.timer_fires) +
                       ", replayed=" +
                       std::to_string(
                           report.stats.replayed_timer_fire_ids.size()) +
                       ")";
        return report;
    }
    if (m_timers != nullptr &&
        report.stats.timer_checksum !=
            report.stats.replayed_timer_checksum) {
        report.status = ReplayStatus::kMismatch;
        report.error = "timer fire sequence differs from recording";
        return report;
    }
    report.status = ReplayStatus::kOk;
    return report;
}

}  // namespace

const char* ReplayStatusName(ReplayStatus status) noexcept {
    switch (status) {
        case ReplayStatus::kOk: return "ok";
        case ReplayStatus::kStuck: return "stuck";
        case ReplayStatus::kFormatError: return "format-error";
        case ReplayStatus::kMismatch: return "mismatch";
        case ReplayStatus::kIoError: return "io-error";
        case ReplayStatus::kNotRun: return "not-run";
        case ReplayStatus::kPartial: return "partial";
    }
    return "unknown";
}

const char* DecisionName(Decision decision) noexcept {
    switch (decision) {
        case Decision::kOk: return "ok";
        case Decision::kClosed: return "closed";
        case Decision::kAlreadyClosed: return "already-closed";
        case Decision::kWouldBlock: return "would-block";
        case Decision::kCancelled: return "cancelled";
        case Decision::kTimedOut: return "timed-out";
        case Decision::kStuck: return "stuck";
    }
    return "unknown";
}

const char* ParkReasonName(ParkReason reason) noexcept {
    switch (reason) {
        case ParkReason::kOther: return "other";
        case ParkReason::kSend: return "send";
        case ParkReason::kRecv: return "recv";
        case ParkReason::kSelect: return "select";
        case ParkReason::kClose: return "close";
        case ParkReason::kTimeout: return "timeout";
        case ParkReason::kContext: return "context";
    }
    return "unknown";
}

ReplayReport MakeFormatErrorReport(const std::string& path,
                                   const std::string& error) {
    ReplayReport report;
    report.path = path;
    report.status = ReplayStatus::kFormatError;
    report.error = error;
    return report;
}

bool LoadRecordingEvents(const std::string& path,
                         std::vector<Event>* events,
                         std::string* error) {
    if (events == nullptr) {
        if (error != nullptr) *error = "null output";
        return false;
    }
    LoadedFile loaded;
    if (!LoadFile(path, &loaded)) {
        if (error != nullptr) *error = loaded.error;
        return false;
    }
    *events = std::move(loaded.events);
    if (error != nullptr) error->clear();
    return true;
}

ReplayReport ReplayFile(const std::string& path, const ReplayOptions& options) {
    try {
        LoadedFile loaded;
        if (!LoadFile(path, &loaded)) {
            ReplayReport report;
            report.path = path;
            report.status =
                loaded.format_error ||
                        loaded.error.find("checksum") != std::string::npos ||
                        loaded.error.find("record") != std::string::npos ||
                        loaded.error.find("magic") != std::string::npos ||
                        loaded.error.find("version") != std::string::npos ||
                        loaded.error.find("header") != std::string::npos
                    ? ReplayStatus::kFormatError
                    : ReplayStatus::kIoError;
            report.error = loaded.error;
            return report;
        }
        return AnalyzeLoaded(loaded, options, path);
    } catch (const std::bad_alloc&) {
        // 顶层兜底：畸形记录绝不能让进程 abort（P1-3）。
        return MakeFormatErrorReport(
            path,
            "out of memory while loading recording: the file's gid/event "
            "counts exceed what this build can index");
    }
}

ReplayReport ReplayFileWith(const std::string& path, const FiberEntryFactory& factory,
                            const ReplayOptions& options) {
    try {
        LoadedFile loaded;
        if (!LoadFile(path, &loaded)) {
            ReplayReport report;
            report.path = path;
            report.status = loaded.format_error ? ReplayStatus::kFormatError
                                                : ReplayStatus::kIoError;
            report.error = loaded.error;
            return report;
        }
        // 分片文件末尾带 checkpoint note：自动把 stop 设在分片最后一条事件上，
        // 这样 `play part` 得到 partial 而不是「记录不完整」的 stuck。
        ReplayOptions effective = options;
        if (effective.stop_at_seq == 0 && !loaded.events.empty() &&
            IsCheckpointNote(loaded.events.back())) {
            effective.stop_at_seq = loaded.events.back().seq > 0
                                        ? loaded.events.back().seq - 1
                                        : 0;
        }
        // 先用静态分析拿到元信息与文件完整性；记录自洽性由完整重放重新判定。
        ReplayOptions meta_options;
        meta_options.keep_timeline = false;
        meta_options.stop_at_seq = effective.stop_at_seq;
        ReplayReport base = AnalyzeLoaded(loaded, meta_options, path);
        if (base.status == ReplayStatus::kFormatError ||
            base.status == ReplayStatus::kMismatch) {
            return base;
        }
        Engine engine(std::move(loaded.events), factory, effective);
        ReplayReport report = engine.Run(base);
        report.path = path;
        report.file_size = loaded.file_size;
        report.file_crc = loaded.computed_crc;
        return report;
    } catch (const std::bad_alloc&) {
        return MakeFormatErrorReport(
            path,
            "out of memory while replaying recording: the file's gid/event "
            "counts exceed what this build can index");
    }
}

namespace {

std::string JsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8U);
    for (const char character : input) {
        switch (character) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20U) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned>(character) & 0xFFU);
                    out += buffer;
                } else {
                    out.push_back(character);
                }
                break;
        }
    }
    return out;
}

std::string HexEncode(const std::string& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2U);
    for (const unsigned char byte : bytes) {
        out.push_back(kHex[(byte >> 4U) & 0x0FU]);
        out.push_back(kHex[byte & 0x0FU]);
    }
    return out;
}

}  // namespace

std::string ReplayReport::Summary() const {
    std::string out;
    out += "[replay] path=" + path + "\n";
    out += "  status=" + std::string(ReplayStatusName(status)) +
           " complete=" + (complete ? "true" : "false") +
           " checksum_ok=" + (checksum_ok ? "true" : "false") +
           " file_size=" + std::to_string(file_size) +
           " events=" + std::to_string(stats.file_events) +
           " file_crc=" + std::to_string(file_crc) + "\n";
    if (!demo_name.empty()) {
        out += "  demo=\"" + demo_name + "\"";
        if (auto_entry) {
            out += " entry=\"" + entry_name + "\"";
        }
        out += "\n";
    }
    out += "  events: sends=" + std::to_string(stats.sends) +
           " recvs=" + std::to_string(stats.recvs) +
           " parks=" + std::to_string(stats.parks) +
           " wakes=" + std::to_string(stats.wakes) +
           " blocked=" + std::to_string(stats.blocked_ops) +
           " closes=" + std::to_string(stats.closes) +
           " selects=" + std::to_string(stats.selects) +
           " timers=" + std::to_string(stats.timers) +
           " timer_arms=" + std::to_string(stats.timer_arms) +
           " timer_fires=" + std::to_string(stats.timer_fires) +
           " timer_cancels=" + std::to_string(stats.timer_cancels) +
           " timer_reschedules=" + std::to_string(stats.timer_reschedules) +
           " ctx=" + std::to_string(stats.ctx_events) + "\n";
    out += "  messages=" + std::to_string(stats.messages) +
           " checksum=" + std::to_string(stats.message_checksum);
    if (stats.replayed_message_checksum != 0) {
        out += " replayed_checksum=" +
               std::to_string(stats.replayed_message_checksum);
    }
    if (stats.dropped_wakes > 0) {
        out += " dropped_wakes=" + std::to_string(stats.dropped_wakes);
    }
    out += "\n";
    if (stats.timer_fires > 0 || stats.timer_arms > 0) {
        out += "  timer: arms=" + std::to_string(stats.timer_arms) +
               " fires=" + std::to_string(stats.timer_fires) +
               " cancels=" + std::to_string(stats.timer_cancels) +
               " reschedules=" + std::to_string(stats.timer_reschedules) +
               " checksum=" + std::to_string(stats.timer_checksum);
        if (stats.replayed_timer_checksum != 0) {
            out += " replayed_checksum=" +
                   std::to_string(stats.replayed_timer_checksum);
        }
        out += "\n";
    }
    if (stats.selects > 0) {
        out += "  selects: choices=";
        for (std::size_t i = 0; i < stats.select_choices.size() && i < 32; ++i) {
            if (i != 0) out += ",";
            out += std::to_string(stats.select_choices[i]);
        }
        if (stats.select_choices.size() > 32) out += ",...";
        out += " checksum=" + std::to_string(stats.select_checksum);
        if (stats.replayed_select_checksum != 0) {
            out += " replayed_checksum=" +
                   std::to_string(stats.replayed_select_checksum);
        }
        out += "\n";
    }
    if (stats.resource_events > 0) {
        out += "  resources: events=" + std::to_string(stats.resource_events) +
               " mutex_locks=" + std::to_string(stats.res_mutex_locks) +
               " mutex_unlocks=" + std::to_string(stats.res_mutex_unlocks) +
               " pc_waits=" + std::to_string(stats.res_pc_waits) +
               " pc_notifies=" + std::to_string(stats.res_pc_notifies) +
               " channel_sends=" + std::to_string(stats.res_channel_sends) +
               " channel_recvs=" + std::to_string(stats.res_channel_recvs) +
               " checksum=" + std::to_string(stats.resource_checksum) + "\n";
        if (stats.res_rw_reads != 0 || stats.res_rw_writes != 0 ||
            stats.res_rw_unlocks != 0) {
            out += "  rwmutex: reads=" + std::to_string(stats.res_rw_reads) +
                   " writes=" + std::to_string(stats.res_rw_writes) +
                   " unlocks=" + std::to_string(stats.res_rw_unlocks) +
                   " read_blocks=" + std::to_string(stats.res_rw_read_blocks) +
                   " write_blocks=" +
                   std::to_string(stats.res_rw_write_blocks) + "\n";
        }
        if (stats.res_cv_waits != 0 || stats.res_cv_notifies != 0 ||
            stats.res_cv_notify_alls != 0) {
            out += "  condvar: waits=" + std::to_string(stats.res_cv_waits) +
                   " notifies=" + std::to_string(stats.res_cv_notifies) +
                   " notify_alls=" +
                   std::to_string(stats.res_cv_notify_alls) +
                   " woken=" + std::to_string(stats.res_cv_woken) + "\n";
        }
        if (stats.res_wg_adds != 0 || stats.res_wg_dones != 0 ||
            stats.res_wg_waits != 0) {
            out += "  waitgroup: adds=" + std::to_string(stats.res_wg_adds) +
                   " dones=" + std::to_string(stats.res_wg_dones) +
                   " waits=" + std::to_string(stats.res_wg_waits) + "\n";
        }
        if (stats.res_io_reads != 0 || stats.res_io_writes != 0 ||
            stats.res_io_waits != 0 || stats.res_io_wakes != 0) {
            out += "  io: reads=" + std::to_string(stats.res_io_reads) +
                   " writes=" + std::to_string(stats.res_io_writes) +
                   " waits=" + std::to_string(stats.res_io_waits) +
                   " wakes=" + std::to_string(stats.res_io_wakes) +
                   " bytes=" + std::to_string(stats.res_io_bytes) + "\n";
        }
        if (stats.res_v5_checksum != 0) {
            out += "  res_v5_checksum=" +
                   std::to_string(stats.res_v5_checksum) + "\n";
        }
    }
    if (virtual_ns > 0) {
        out += "  virtual_ns=" + std::to_string(virtual_ns);
        if (stopped_early) {
            out += " stopped_at_seq=" + std::to_string(stopped_at_seq);
        }
        out += "\n";
    }
    if (!checkpoints.empty()) {
        out += "  checkpoints=" + std::to_string(checkpoints.size()) +
               " last_seq=" + std::to_string(checkpoints.back().seq) + "\n";
    }
    if (replay_executed) {
        out += "  replay: executed=true";
        if (stuck) {
            out += " stuck=true " + stuck_reason;
        }
        out += "\n";
    }
    for (const auto& gid : stats.per_gid) {
        if (!gid.started && gid.sends == 0 && gid.recvs == 0) {
            continue;
        }
        out += "  gid " + std::to_string(gid.gid) +
               ": sends=" + std::to_string(gid.sends) +
               " recvs=" + std::to_string(gid.recvs) +
               " parks=" + std::to_string(gid.parks) +
               " wakes=" + std::to_string(gid.wakes) +
               " selects=" + std::to_string(gid.selects) +
               " exited=" + (gid.exited ? "true" : "false") + "\n";
    }
    if (!error.empty()) {
        out += "  error: " + error + "\n";
    }
    return out;
}

std::string ReplayReportJson(const ReplayReport& report) {
    std::string out;
    out += "{\"schema\":\"go2cpp.replay.report/1\"";
    out += ",\"status\":\"" + std::string(ReplayStatusName(report.status)) + "\"";
    out += ",\"path\":\"" + JsonEscape(report.path) + "\"";
    out += ",\"demo\":\"" + JsonEscape(report.demo_name) + "\"";
    out += ",\"file_size\":" + std::to_string(report.file_size);
    out += ",\"file_crc\":" + std::to_string(report.file_crc);
    out += ",\"checksum_ok\":";
    out += report.checksum_ok ? "true" : "false";
    out += ",\"complete\":";
    out += report.complete ? "true" : "false";
    out += ",\"replay_executed\":";
    out += report.replay_executed ? "true" : "false";
    out += ",\"stuck\":";
    out += report.stuck ? "true" : "false";
    out += ",\"stuck_at\":{";
    out += "\"gid\":" + std::to_string(report.stuck_gid);
    out += ",\"channel\":" + std::to_string(report.stuck_channel);
    out += ",\"op_index\":" + std::to_string(report.stuck_op_index);
    out += ",\"reason\":\"" + JsonEscape(report.stuck_reason) + "\"}";
    out += ",\"error\":\"" + JsonEscape(report.error) + "\"";
    out += ",\"stopped_early\":";
    out += report.stopped_early ? "true" : "false";
    out += ",\"stopped_at_seq\":" + std::to_string(report.stopped_at_seq);
    out += ",\"auto_entry\":";
    out += report.auto_entry ? "true" : "false";
    out += ",\"entry_name\":\"" + JsonEscape(report.entry_name) + "\"";
    out += ",\"virtual_ns\":" + std::to_string(report.virtual_ns);

    out += ",\"stats\":{";
    out += "\"file_events\":" + std::to_string(report.stats.file_events);
    out += ",\"sends\":" + std::to_string(report.stats.sends);
    out += ",\"recvs\":" + std::to_string(report.stats.recvs);
    out += ",\"parks\":" + std::to_string(report.stats.parks);
    out += ",\"wakes\":" + std::to_string(report.stats.wakes);
    out += ",\"closes\":" + std::to_string(report.stats.closes);
    out += ",\"selects\":" + std::to_string(report.stats.selects);
    out += ",\"timers\":" + std::to_string(report.stats.timers);
    out += ",\"timer_arms\":" + std::to_string(report.stats.timer_arms);
    out += ",\"timer_fires\":" + std::to_string(report.stats.timer_fires);
    out += ",\"timer_cancels\":" +
           std::to_string(report.stats.timer_cancels);
    out += ",\"timer_reschedules\":" +
           std::to_string(report.stats.timer_reschedules);
    out += ",\"timer_checksum\":" +
           std::to_string(report.stats.timer_checksum);
    out += ",\"replayed_timer_checksum\":" +
           std::to_string(report.stats.replayed_timer_checksum);
    out += ",\"ctx_events\":" + std::to_string(report.stats.ctx_events);
    out += ",\"blocked_ops\":" + std::to_string(report.stats.blocked_ops);
    out += ",\"messages\":" + std::to_string(report.stats.messages);
    out += ",\"bytes_messages\":" + std::to_string(report.stats.bytes_messages);
    out += ",\"dropped_wakes\":" + std::to_string(report.stats.dropped_wakes);
    out += ",\"synthetic_events\":" + std::to_string(report.stats.synthetic_events);
    out += ",\"resource_events\":" +
           std::to_string(report.stats.resource_events);
    out += ",\"res_mutex_locks\":" +
           std::to_string(report.stats.res_mutex_locks);
    out += ",\"res_mutex_unlocks\":" +
           std::to_string(report.stats.res_mutex_unlocks);
    out += ",\"res_pc_waits\":" +
           std::to_string(report.stats.res_pc_waits);
    out += ",\"res_pc_notifies\":" +
           std::to_string(report.stats.res_pc_notifies);
    out += ",\"res_channel_sends\":" +
           std::to_string(report.stats.res_channel_sends);
    out += ",\"res_channel_recvs\":" +
           std::to_string(report.stats.res_channel_recvs);
    out += ",\"resource_checksum\":" +
           std::to_string(report.stats.resource_checksum);
    out += ",\"replayed_resource_checksum\":" +
           std::to_string(report.stats.replayed_resource_checksum);
    out += ",\"resource_lock_hold_ns\":" +
           std::to_string(report.stats.resource_lock_hold_ns);
    out += ",\"resource_order_anomalies\":" +
           std::to_string(report.stats.resource_order_anomalies);
    out += ",\"res_rw_reads\":" + std::to_string(report.stats.res_rw_reads);
    out += ",\"res_rw_writes\":" + std::to_string(report.stats.res_rw_writes);
    out += ",\"res_rw_unlocks\":" + std::to_string(report.stats.res_rw_unlocks);
    out += ",\"res_rw_read_blocks\":" +
           std::to_string(report.stats.res_rw_read_blocks);
    out += ",\"res_rw_write_blocks\":" +
           std::to_string(report.stats.res_rw_write_blocks);
    out += ",\"res_cv_waits\":" + std::to_string(report.stats.res_cv_waits);
    out += ",\"res_cv_notifies\":" +
           std::to_string(report.stats.res_cv_notifies);
    out += ",\"res_cv_notify_alls\":" +
           std::to_string(report.stats.res_cv_notify_alls);
    out += ",\"res_cv_woken\":" + std::to_string(report.stats.res_cv_woken);
    out += ",\"res_wg_adds\":" + std::to_string(report.stats.res_wg_adds);
    out += ",\"res_wg_dones\":" + std::to_string(report.stats.res_wg_dones);
    out += ",\"res_wg_waits\":" + std::to_string(report.stats.res_wg_waits);
    out += ",\"res_io_reads\":" + std::to_string(report.stats.res_io_reads);
    out += ",\"res_io_writes\":" + std::to_string(report.stats.res_io_writes);
    out += ",\"res_io_waits\":" + std::to_string(report.stats.res_io_waits);
    out += ",\"res_io_wakes\":" + std::to_string(report.stats.res_io_wakes);
    out += ",\"res_io_bytes\":" + std::to_string(report.stats.res_io_bytes);
    out += ",\"res_v5_checksum\":" +
           std::to_string(report.stats.res_v5_checksum);
    out += ",\"message_checksum\":" + std::to_string(report.stats.message_checksum);
    out += ",\"replayed_message_checksum\":" +
           std::to_string(report.stats.replayed_message_checksum);
    out += ",\"select_checksum\":" + std::to_string(report.stats.select_checksum);
    out += ",\"replayed_select_checksum\":" +
           std::to_string(report.stats.replayed_select_checksum);
    out += ",\"global_recv_sequence\":[";
    for (std::size_t index = 0; index < report.stats.global_recv_sequence.size();
         ++index) {
        if (index != 0) out += ",";
        out += std::to_string(report.stats.global_recv_sequence[index]);
    }
    out += "]";
    out += ",\"replayed_recv_sequence\":[";
    for (std::size_t index = 0; index < report.stats.replayed_recv_sequence.size();
         ++index) {
        if (index != 0) out += ",";
        out += std::to_string(report.stats.replayed_recv_sequence[index]);
    }
    out += "]";
    out += ",\"select_choices\":[";
    for (std::size_t index = 0; index < report.stats.select_choices.size(); ++index) {
        if (index != 0) out += ",";
        out += std::to_string(report.stats.select_choices[index]);
    }
    out += "]";
    out += ",\"timer_fire_ids\":[";
    for (std::size_t index = 0; index < report.stats.timer_fire_ids.size();
         ++index) {
        if (index != 0) out += ",";
        out += std::to_string(report.stats.timer_fire_ids[index]);
    }
    out += "]";
    out += ",\"replayed_timer_fire_ids\":[";
    for (std::size_t index = 0;
         index < report.stats.replayed_timer_fire_ids.size(); ++index) {
        if (index != 0) out += ",";
        out += std::to_string(report.stats.replayed_timer_fire_ids[index]);
    }
    out += "]";
    out += ",\"replayed_select_choices\":[";
    for (std::size_t index = 0;
         index < report.stats.replayed_select_choices.size(); ++index) {
        if (index != 0) out += ",";
        out += std::to_string(report.stats.replayed_select_choices[index]);
    }
    out += "]}";

    out += ",\"checkpoints\":[";
    for (std::size_t index = 0; index < report.checkpoints.size(); ++index) {
        const auto& checkpoint = report.checkpoints[index];
        if (index != 0) out += ",";
        out += "{\"seq\":" + std::to_string(checkpoint.seq);
        out += ",\"event_index\":" + std::to_string(checkpoint.event_index);
        out += ",\"vtime_ns\":" + std::to_string(checkpoint.vtime_ns);
        out += ",\"ts_ns\":" + std::to_string(checkpoint.ts_ns) + "}";
    }
    out += "]";

    out += ",\"per_gid\":[";
    for (std::size_t index = 0; index < report.stats.per_gid.size(); ++index) {
        const auto& gid = report.stats.per_gid[index];
        if (index != 0) out += ",";
        out += "{\"gid\":" + std::to_string(gid.gid);
        out += ",\"started\":";
        out += gid.started ? "true" : "false";
        out += ",\"exited\":";
        out += gid.exited ? "true" : "false";
        out += ",\"sends\":" + std::to_string(gid.sends);
        out += ",\"recvs\":" + std::to_string(gid.recvs);
        out += ",\"parks\":" + std::to_string(gid.parks);
        out += ",\"wakes\":" + std::to_string(gid.wakes);
        out += ",\"selects\":" + std::to_string(gid.selects) + "}";
    }
    out += "]";

    if (!report.resources.empty()) {
        out += ",\"resources\":[";
        for (std::size_t index = 0; index < report.resources.size();
             ++index) {
            const auto& entry = report.resources[index];
            if (index != 0) out += ",";
            out += "{\"seq\":" + std::to_string(entry.seq);
            out += ",\"vtime_ns\":" + std::to_string(entry.vtime_ns);
            out += ",\"type\":\"" +
                   std::string(EventTypeName(entry.type)) + "\"";
            out += ",\"resource_id\":" +
                   std::to_string(entry.resource_id);
            out += ",\"actor_gid\":" +
                   std::to_string(entry.actor_gid);
            out += ",\"target_gid\":" +
                   std::to_string(entry.target_gid);
            out += ",\"result\":" + std::to_string(entry.result);
            out += ",\"address\":" + std::to_string(entry.address);
            out += ",\"waiters\":" + std::to_string(entry.waiters);
            out += ",\"hold_ns\":" + std::to_string(entry.hold_ns);
            out += ",\"fd\":" + std::to_string(entry.fd);
            out += ",\"bytes\":" + std::to_string(entry.bytes);
            out += ",\"aux\":" + std::to_string(entry.aux);
            out += ",\"value\":" + std::to_string(entry.value);
            out += "}";
        }
        out += "]";
    }

    if (!report.timeline.empty()) {
        out += ",\"timeline\":[";
        for (std::size_t index = 0; index < report.timeline.size(); ++index) {
            const auto& entry = report.timeline[index];
            if (index != 0) out += ",";
            out += "{\"seq\":" + std::to_string(entry.seq);
            out += ",\"ts_ns\":" + std::to_string(entry.ts_ns);
            out += ",\"vtime_ns\":" + std::to_string(entry.vtime_ns);
            out += ",\"type\":\"" + std::string(EventTypeName(entry.type)) + "\"";
            out += ",\"flags\":" + std::to_string(entry.flags);
            out += ",\"gid\":" + std::to_string(entry.gid);
            out += ",\"channel\":" + std::to_string(entry.channel);
            out += ",\"op_index\":" + std::to_string(entry.op_index);
            out += ",\"value\":" + std::to_string(entry.value);
            out += ",\"data_hex\":\"" + HexEncode(entry.data) + "\"";
            out += ",\"decision\":\"" + std::string(DecisionName(entry.decision)) + "\"";
            out += ",\"blocked\":" + std::to_string(entry.blocked);
            out += ",\"chosen\":" + std::to_string(entry.chosen) + "}";
        }
        out += "]";
    }
    out += "}\n";
    return out;
}

}  // namespace go2cpp::replay
