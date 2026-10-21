/**
 * @file debug_dump.cpp
 * @brief 统一 dump 序列化实现（text key=value + JSON）。
 *
 * 所有函数只读取已经采集好的快照；不调用任何会改变运行时状态的接口。
 */

#include "go2cpp/debug/debug_dump.hpp"
#include "go2cpp/debug/debug_resources.hpp"
#include "go2cpp/debug/debug_server.hpp"

#include "go2cpp/json.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace go2cpp::debug {
namespace {

std::uint64_t SteadyNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::uint64_t WallNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string HexValue(std::uint64_t value) {
    char buffer[2 + 16 + 1];
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

std::string BoolText(bool value) { return value ? "true" : "false"; }

/// steady_clock 时间点转纳秒（timer dump 的 deadline 字段）。
std::uint64_t ToSteadyNs(
    std::chrono::steady_clock::time_point point) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            point.time_since_epoch())
            .count());
}

}  // namespace

std::string DumpEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '=': out += "\\="; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string JsonEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
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
    return out;
}

DumpPayload DumpPayload::Unavailable(std::string reason_code) {
    DumpPayload payload;
    payload.available = false;
    payload.reason = std::move(reason_code);
    payload.text = "available=false\nreason=" + DumpEscape(payload.reason) + "\n";
    payload.json = "{\"available\":false,\"reason\":\"" +
                   JsonEscape(payload.reason) + "\"}";
    return payload;
}

TextDump& TextDump::KeyHex(std::string_view key, std::uint64_t value) {
    return Key(key, HexValue(value));
}

ConsistencyStamp BeginConsistency() noexcept {
    ConsistencyStamp stamp;
    stamp.supported = EventRing::Enabled();
    // v6：事件环已无全局 seq，用累计发布事件数作一致性探针
    // （仍单调；语义从「最后序号」变为「已发布总数」）。
    stamp.seq_start = EventRing::PublishedEvents();
    stamp.seq_end = stamp.seq_start;
    return stamp;
}

void EndConsistency(ConsistencyStamp& stamp) noexcept {
    stamp.seq_end = EventRing::PublishedEvents();
}

void AppendConsistency(TextDump& text, const ConsistencyStamp& stamp) {
    text.Key("seq_at_start", stamp.seq_start);
    text.Key("seq_at_end", stamp.seq_end);
    text.Key("changed_during_dump", stamp.Changed());
    text.Key("event_seq_supported", stamp.supported);
}

std::string JsonConsistency(const ConsistencyStamp& stamp) {
    std::string out;
    out += ",\"seq_at_start\":" + std::to_string(stamp.seq_start);
    out += ",\"seq_at_end\":" + std::to_string(stamp.seq_end);
    out += ",\"changed_during_dump\":";
    out += stamp.Changed() ? "true" : "false";
    out += ",\"event_seq_supported\":";
    out += stamp.supported ? "true" : "false";
    return out;
}

const char* GStateNameStable(GState state) noexcept {
    switch (state) {
        case GState::kNew: return "kNew";
        case GState::kRunnable: return "kRunnable";
        case GState::kRunning: return "kRunning";
        case GState::kWaiting: return "kWaiting";
        case GState::kDead: return "kDead";
        case GState::kCancelled: return "kCancelled";
        case GState::kFailed: return "kFailed";
    }
    return "kUnknown";
}

const char* MStateNameStable(MState state) noexcept {
    switch (state) {
        case MState::kIdle: return "kIdle";
        case MState::kRunning: return "kRunning";
        case MState::kBlocking: return "kBlocking";
        case MState::kParked: return "kParked";
        case MState::kStopping: return "kStopping";
        case MState::kDead: return "kDead";
    }
    return "kUnknown";
}

const char* PStateNameStable(PState state) noexcept {
    switch (state) {
        case PState::kIdle: return "kIdle";
        case PState::kRunning: return "kRunning";
        case PState::kStopping: return "kStopping";
        case PState::kDead: return "kDead";
    }
    return "kUnknown";
}

const char* FiberStateNameStable(FiberState state) noexcept {
    switch (state) {
        case FiberState::Ready: return "kReady";
        case FiberState::Running: return "kRunning";
        case FiberState::Suspended: return "kSuspended";
        case FiberState::Completed: return "kCompleted";
        case FiberState::Failed: return "kFailed";
    }
    return "kUnknown";
}

const char* SuspendReasonNameStable(SuspendReason reason) noexcept {
    switch (reason) {
        case SuspendReason::None: return "kNone";
        case SuspendReason::Yield: return "kYield";
        case SuspendReason::Park: return "kPark";
        case SuspendReason::Io: return "kIo";
        case SuspendReason::Timer: return "kTimer";
        case SuspendReason::Synchronization: return "kSynchronization";
    }
    return "kUnknown";
}

const char* WaitStateNameStable(int state) noexcept {
    switch (state) {
        case 0: return "kWaiting";
        case 1: return "kNotified";
        case 2: return "kTimedOut";
        case 3: return "kCancelled";
        default: return "kUnknown";
    }
}

DumpPayload DumpScheduler(Scheduler& scheduler, const DumpOptions& options) {
    ConsistencyStamp stamp = BeginConsistency();
    DebugScheduler proxy(scheduler);
    const SchedulerDebugSnapshot snapshot = proxy.SnapshotWith(options);
    EndConsistency(stamp);
    std::size_t local_total = 0;
    for (const auto& processor : snapshot.processors) {
        local_total += processor.queued;
    }

    TextDump text;
    text.Head("go2cpp.dump.scheduler/1");
    text.Key("consistent", "per_call");
    AppendConsistency(text, stamp);
    text.Key("steady_ns", SteadyNs());
    text.Key("wall_unix_ns", WallNs());
    text.Key("running", snapshot.running);
    text.Key("processor_count", snapshot.processor_count);
    text.Key("worker_count", snapshot.worker_count);
    text.Key("runnable_total", snapshot.runnable_total);
    text.Key("runnable_global", snapshot.runnable_global);
    text.Key("runqueue.local_total", local_total);
    text.Key("runqueue.global", snapshot.runnable_global);
    text.Key("sysmon_running", snapshot.sysmon_running);
    text.Key("sysmon_passes", snapshot.sysmon_passes);
    text.Key("metrics.task_runs", snapshot.metrics.task_runs);
    text.Key("metrics.task_completions", snapshot.metrics.task_completions);
    text.Key("metrics.fiber_resume_ns", snapshot.metrics.fiber_resume_ns);
    text.Key("metrics.local_queue_pops", snapshot.metrics.local_queue_pops);
    text.Key("metrics.steal_pops", snapshot.metrics.steal_pops);
    for (std::size_t i = 0; i < snapshot.processors.size(); ++i) {
        const auto& processor = snapshot.processors[i];
        const std::string prefix = "processor." + std::to_string(i) + ".";
        text.Key(prefix + "id", processor.id);
        text.Key(prefix + "state", PStateNameStable(processor.state));
        text.Key(prefix + "queued", processor.queued);
    }
    for (std::size_t i = 0; i < snapshot.machines.size(); ++i) {
        const auto& machine = snapshot.machines[i];
        const std::string prefix = "machine." + std::to_string(i) + ".";
        text.Key(prefix + "id", machine.id);
        text.Key(prefix + "state", MStateNameStable(machine.state));
        text.Key(prefix + "processor", machine.processor);
        text.Key(prefix + "processor_detached", machine.processor_detached);
        text.Key(prefix + "blocking_task", machine.blocking_task);
        text.Key(prefix + "long_syscall_count", machine.long_syscall_count);
        text.Key(prefix + "replacement", machine.replacement);
        text.Key(prefix + "affinity_hits", machine.affinity_hits);
        text.Key(prefix + "affinity_misses", machine.affinity_misses);
    }
    for (std::size_t i = 0; i < snapshot.tasks.size(); ++i) {
        const auto& task = snapshot.tasks[i];
        const std::string prefix = "task." + std::to_string(i) + ".";
        text.Key(prefix + "id", task.id);
        text.Key(prefix + "state", GStateNameStable(task.state));
        text.Key(prefix + "queued", task.queued);
        text.Key(prefix + "started", task.started);
        text.Key(prefix + "last_processor_id", task.last_processor_id);
        text.Key(prefix + "task_class", task.task_class);
        text.Key(prefix + "failed", task.failed);
        text.Key(prefix + "cancellation_requested", task.cancellation_requested);
    }

    std::string json =
        "{\"schema\":\"go2cpp.dump.scheduler/1\",\"version\":1";
    json += ",\"consistent\":\"per_call\"";
    json += JsonConsistency(stamp);
    json += ",\"steady_ns\":" + std::to_string(SteadyNs());
    json += ",\"wall_unix_ns\":" + std::to_string(WallNs());
    json += ",\"running\":" + BoolText(snapshot.running);
    json += ",\"processor_count\":" + std::to_string(snapshot.processor_count);
    json += ",\"worker_count\":" + std::to_string(snapshot.worker_count);
    json += ",\"runnable_total\":" + std::to_string(snapshot.runnable_total);
    json += ",\"runnable_global\":" + std::to_string(snapshot.runnable_global);
    json += ",\"runqueue\":{\"local_total\":" + std::to_string(local_total) +
            ",\"global\":" + std::to_string(snapshot.runnable_global) + "}";
    json += ",\"sysmon_running\":" + BoolText(snapshot.sysmon_running);
    json += ",\"sysmon_passes\":" + std::to_string(snapshot.sysmon_passes);
    json += ",\"metrics\":{\"task_runs\":" +
            std::to_string(snapshot.metrics.task_runs) +
            ",\"task_completions\":" +
            std::to_string(snapshot.metrics.task_completions) +
            ",\"fiber_resume_ns\":" +
            std::to_string(snapshot.metrics.fiber_resume_ns) +
            ",\"local_queue_pops\":" +
            std::to_string(snapshot.metrics.local_queue_pops) +
            ",\"steal_pops\":" +
            std::to_string(snapshot.metrics.steal_pops) + "}";
    json += ",\"processors\":[";
    for (std::size_t i = 0; i < snapshot.processors.size(); ++i) {
        if (i != 0) {
            json += ',';
        }
        const auto& processor = snapshot.processors[i];
        json += "{\"id\":" + std::to_string(processor.id);
        json += ",\"state\":\"";
        json += PStateNameStable(processor.state);
        json += "\",\"queued\":" + std::to_string(processor.queued) + "}";
    }
    json += "],\"machines\":[";
    for (std::size_t i = 0; i < snapshot.machines.size(); ++i) {
        if (i != 0) {
            json += ',';
        }
        const auto& machine = snapshot.machines[i];
        json += "{\"id\":" + std::to_string(machine.id);
        json += ",\"state\":\"";
        json += MStateNameStable(machine.state);
        json += "\",\"processor\":" + std::to_string(machine.processor);
        json += ",\"processor_detached\":" +
                BoolText(machine.processor_detached);
        json += ",\"blocking_task\":" + std::to_string(machine.blocking_task);
        json += ",\"long_syscall_count\":" +
                std::to_string(machine.long_syscall_count);
        json += ",\"replacement\":" + BoolText(machine.replacement);
        json += ",\"affinity_hits\":" +
                std::to_string(machine.affinity_hits);
        json += ",\"affinity_misses\":" +
                std::to_string(machine.affinity_misses) + "}";
    }
    json += "],\"tasks\":[";
    for (std::size_t i = 0; i < snapshot.tasks.size(); ++i) {
        if (i != 0) {
            json += ',';
        }
        const auto& task = snapshot.tasks[i];
        json += "{\"id\":" + std::to_string(task.id);
        json += ",\"state\":\"";
        json += GStateNameStable(task.state);
        json += "\",\"queued\":" + BoolText(task.queued);
        json += ",\"started\":" + BoolText(task.started);
        json += ",\"last_processor_id\":" +
                std::to_string(task.last_processor_id);
        json += ",\"task_class\":" + std::to_string(task.task_class);
        json += ",\"failed\":" + BoolText(task.failed);
        json += ",\"cancellation_requested\":" +
                BoolText(task.cancellation_requested) + "}";
    }
    json += "]}";

    DumpPayload payload;
    payload.text = text.Take();
    payload.json = std::move(json);
    return payload;
}

DumpPayload DumpFiber(const Fiber& fiber) {
    const FiberContextFrame frame = fiber.debug_info();
    const FiberStackDebugSnapshot stack = fiber.debug_stack();

    TextDump text;
    text.Head("go2cpp.dump.fiber/1");
    text.Key("id", stack.id);
    text.Key("state", FiberStateNameStable(stack.state));
    text.Key("suspend_reason", SuspendReasonNameStable(stack.suspend_reason));
    text.Key("parent_id", stack.parent_id);
    text.Key("active_parent_id", stack.active_parent_id);
    text.Key("nesting_depth", stack.nesting_depth);
    text.Key("managed", frame.execution.managed);
    text.Key("task_id", frame.execution.task_id);
    text.Key("machine_id", frame.execution.machine_id);
    text.Key("processor_id", frame.execution.processor_id);
    text.Key("has_stack", stack.has_stack);
    text.Key("growable", stack.growable);
    text.Key("requested_bytes", stack.requested_bytes);
    text.Key("reserve_bytes", stack.reserve_bytes);
    text.Key("usable_bytes", stack.usable_bytes);
    text.KeyHex("mapping_base", stack.mapping_base);
    text.KeyHex("mapping_top", stack.mapping_top);
    text.KeyHex("committed_begin", stack.committed_begin);
    text.KeyHex("current_sp", stack.current_sp);

    std::string json = "{\"schema\":\"go2cpp.dump.fiber/1\",\"version\":1";
    json += ",\"id\":" + std::to_string(stack.id);
    json += ",\"state\":\"";
    json += FiberStateNameStable(stack.state);
    json += "\",\"suspend_reason\":\"";
    json += SuspendReasonNameStable(stack.suspend_reason);
    json += "\",\"parent_id\":" + std::to_string(stack.parent_id);
    json += ",\"active_parent_id\":" +
            std::to_string(stack.active_parent_id);
    json += ",\"nesting_depth\":" + std::to_string(stack.nesting_depth);
    json += ",\"managed\":" + BoolText(frame.execution.managed);
    json += ",\"task_id\":" + std::to_string(frame.execution.task_id);
    json += ",\"machine_id\":" + std::to_string(frame.execution.machine_id);
    json += ",\"processor_id\":" +
            std::to_string(frame.execution.processor_id);
    json += ",\"has_stack\":" + BoolText(stack.has_stack);
    json += ",\"growable\":" + BoolText(stack.growable);
    json += ",\"requested_bytes\":" + std::to_string(stack.requested_bytes);
    json += ",\"reserve_bytes\":" + std::to_string(stack.reserve_bytes);
    json += ",\"usable_bytes\":" + std::to_string(stack.usable_bytes);
    json += ",\"mapping_base\":\"" + HexValue(stack.mapping_base) + "\"";
    json += ",\"mapping_top\":\"" + HexValue(stack.mapping_top) + "\"";
    json += ",\"committed_begin\":\"" + HexValue(stack.committed_begin) + "\"";
    json += ",\"current_sp\":\"" + HexValue(stack.current_sp) + "\"}";

    DumpPayload payload;
    payload.text = text.Take();
    payload.json = std::move(json);
    return payload;
}

DumpPayload DumpChannel(const ChannelDebugSnapshot& snapshot) {
    TextDump text;
    text.Head("go2cpp.dump.channel/1");
    text.Key("capacity", snapshot.capacity);
    text.Key("length", snapshot.length);
    text.Key("closed", snapshot.closed);
    text.Key("send_waiters", snapshot.send_waiters);
    text.Key("recv_waiters", snapshot.recv_waiters);
    text.Key("select_senders", snapshot.select_senders);
    text.Key("select_receivers", snapshot.select_receivers);
    text.Key("change_waiters", snapshot.change_waiters);
    text.Key("generation", snapshot.generation);

    std::string json = "{\"schema\":\"go2cpp.dump.channel/1\",\"version\":1";
    json += ",\"capacity\":" + std::to_string(snapshot.capacity);
    json += ",\"length\":" + std::to_string(snapshot.length);
    json += ",\"closed\":" + BoolText(snapshot.closed);
    json += ",\"send_waiters\":" + std::to_string(snapshot.send_waiters);
    json += ",\"recv_waiters\":" + std::to_string(snapshot.recv_waiters);
    json += ",\"select_senders\":" + std::to_string(snapshot.select_senders);
    json += ",\"select_receivers\":" +
            std::to_string(snapshot.select_receivers);
    json += ",\"change_waiters\":" + std::to_string(snapshot.change_waiters);
    json += ",\"generation\":" + std::to_string(snapshot.generation) + "}";

    DumpPayload payload;
    payload.text = text.Take();
    payload.json = std::move(json);
    return payload;
}

DumpPayload DumpParkingCondition(
    const core::ParkingCondition& condition) {
    const core::ParkingConditionDebugSnapshot snapshot =
        condition.DebugSnapshot();
    TextDump text;
    text.Head("go2cpp.dump.parking_condition/1");
    text.Key("waiters", snapshot.waiters);
    text.Key("native_waiters", snapshot.native_waiters);
    text.Key("fiber_waiters", snapshot.fiber_waiters);
    text.Key("waiting_nodes", snapshot.waiting_nodes);
    text.Key("notified_nodes", snapshot.notified_nodes);
    text.Key("broadcast_seq", snapshot.broadcast_seq);
    text.Key("broadcast_waiters", snapshot.broadcast_waiters);
    for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
        const auto& node = snapshot.nodes[i];
        const std::string prefix = "node." + std::to_string(i) + ".";
        text.Key(prefix + "index", node.index);
        text.Key(prefix + "native", node.native);
        text.Key(prefix + "state", WaitStateNameStable(node.state));
    }

    std::string json =
        "{\"schema\":\"go2cpp.dump.parking_condition/1\",\"version\":1";
    json += ",\"waiters\":" + std::to_string(snapshot.waiters);
    json += ",\"native_waiters\":" + std::to_string(snapshot.native_waiters);
    json += ",\"fiber_waiters\":" + std::to_string(snapshot.fiber_waiters);
    json += ",\"waiting_nodes\":" + std::to_string(snapshot.waiting_nodes);
    json += ",\"notified_nodes\":" + std::to_string(snapshot.notified_nodes);
    json += ",\"broadcast_seq\":" + std::to_string(snapshot.broadcast_seq);
    json += ",\"broadcast_waiters\":" +
            std::to_string(snapshot.broadcast_waiters);
    json += ",\"nodes\":[";
    for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
        if (i != 0) {
            json += ',';
        }
        const auto& node = snapshot.nodes[i];
        json += "{\"index\":" + std::to_string(node.index);
        json += ",\"native\":" + BoolText(node.native);
        json += ",\"state\":\"";
        json += WaitStateNameStable(node.state);
        json += "\"}";
    }
    json += "]}";

    DumpPayload payload;
    payload.text = text.Take();
    payload.json = std::move(json);
    return payload;
}

DumpPayload DumpMutex(const sync::Mutex& mutex) {
    const sync::MutexDebugSnapshot snapshot = mutex.DebugSnapshotInternal();
    TextDump text;
    text.Head("go2cpp.dump.mutex/1");
    text.Key("held", snapshot.held);
    text.Key("has_waiters", snapshot.has_waiters);
    text.Key("waiters", snapshot.waiters);
    text.Key("native_waiters", snapshot.native_waiters);
    text.Key("fiber_waiters", snapshot.fiber_waiters);
    text.KeyHex("state_bits", snapshot.state_bits);
    text.Key("locked", snapshot.locked);
    text.Key("waiter_flag", snapshot.waiter_flag);
    text.Key("starving", snapshot.starving);
    text.Key("woken", snapshot.woken);
    text.Key("owner_gid", snapshot.owner_gid);
    text.Key("owner_known", snapshot.owner_known);

    std::string json = "{\"schema\":\"go2cpp.dump.mutex/1\",\"version\":1";
    json += ",\"held\":" + BoolText(snapshot.held);
    json += ",\"has_waiters\":" + BoolText(snapshot.has_waiters);
    json += ",\"waiters\":" + std::to_string(snapshot.waiters);
    json += ",\"native_waiters\":" + std::to_string(snapshot.native_waiters);
    json += ",\"fiber_waiters\":" + std::to_string(snapshot.fiber_waiters);
    json += ",\"state_bits\":\"" + HexValue(snapshot.state_bits) + "\"";
    json += ",\"locked\":" + BoolText(snapshot.locked);
    json += ",\"waiter_flag\":" + BoolText(snapshot.waiter_flag);
    json += ",\"starving\":" + BoolText(snapshot.starving);
    json += ",\"woken\":" + BoolText(snapshot.woken);
    json += ",\"owner_gid\":" + std::to_string(snapshot.owner_gid);
    json += ",\"owner_known\":" + BoolText(snapshot.owner_known) + "}";

    DumpPayload payload;
    payload.text = text.Take();
    payload.json = std::move(json);
    return payload;
}

DumpPayload DumpConditionVariable(const sync::ConditionVariable& condition) {
    const sync::ConditionVariableDebugSnapshot snapshot =
        condition.DebugSnapshotInternal();
    TextDump text;
    text.Head("go2cpp.dump.condition_variable/1");
    text.Key("waiters", snapshot.waiters);

    std::string json =
        "{\"schema\":\"go2cpp.dump.condition_variable/1\",\"version\":1";
    json += ",\"waiters\":" + std::to_string(snapshot.waiters) + "}";

    DumpPayload payload;
    payload.text = text.Take();
    payload.json = std::move(json);
    return payload;
}

DumpPayload DumpIOManager(io::IOManager& manager) {
    ConsistencyStamp stamp = BeginConsistency();
    const io::IOManagerDebugSnapshot snapshot = manager.DebugSnapshot();
    EndConsistency(stamp);
    TextDump text;
    text.Head("go2cpp.dump.io/1");
    text.Key("consistent", "per_call");
    AppendConsistency(text, stamp);
    text.Key("running", snapshot.running);
    text.Key("accepting", snapshot.accepting);
    text.Key("worker_polling", snapshot.worker_polling);
    text.Key("fd_slots", snapshot.fd_slots);
    text.Key("slots_with_waiters", snapshot.slots_with_waiters);
    text.Key("epoll_registered", snapshot.epoll_registered);
    text.Key("reader_waiters", snapshot.reader_waiters);
    text.Key("writer_waiters", snapshot.writer_waiters);
    text.Key("pending_read", snapshot.pending_read);
    text.Key("pending_write", snapshot.pending_write);
    text.Key("timers", snapshot.timers);
    text.Key("scheduler_runnable", snapshot.scheduler_runnable);
    text.Key("scheduler_workers", snapshot.scheduler_workers);
    text.Key("event_ring_enabled", EventRing::Enabled());

    std::string json = "{\"schema\":\"go2cpp.dump.io/1\",\"version\":1";
    json += ",\"consistent\":\"per_call\"";
    json += JsonConsistency(stamp);
    json += ",\"running\":" + BoolText(snapshot.running);
    json += ",\"accepting\":" + BoolText(snapshot.accepting);
    json += ",\"worker_polling\":" + BoolText(snapshot.worker_polling);
    json += ",\"fd_slots\":" + std::to_string(snapshot.fd_slots);
    json += ",\"slots_with_waiters\":" +
            std::to_string(snapshot.slots_with_waiters);
    json += ",\"epoll_registered\":" +
            std::to_string(snapshot.epoll_registered);
    json += ",\"reader_waiters\":" + std::to_string(snapshot.reader_waiters);
    json += ",\"writer_waiters\":" + std::to_string(snapshot.writer_waiters);
    json += ",\"pending_read\":" + std::to_string(snapshot.pending_read);
    json += ",\"pending_write\":" + std::to_string(snapshot.pending_write);
    json += ",\"timers\":" + std::to_string(snapshot.timers);
    json += ",\"scheduler_runnable\":" +
            std::to_string(snapshot.scheduler_runnable);
    json += ",\"scheduler_workers\":" +
            std::to_string(snapshot.scheduler_workers);
    json += ",\"event_ring_enabled\":" + BoolText(EventRing::Enabled()) + "}";

    DumpPayload payload;
    payload.text = text.Take();
    payload.json = std::move(json);
    return payload;
}


DumpPayload DumpTimer(const core::TimerService& service) {
    ConsistencyStamp stamp = BeginConsistency();
    const core::TimerDebugSnapshot snapshot = service.DebugSnapshot();
    EndConsistency(stamp);

    // 事件环里的 timer 事件（最近 8 条）：SUBSCRIBE 的文本行只有 7 列，
    // 这里给出 timer_id/kind/deadline_ns/source_gid 的语义视图，
    // 让 DUMP timer 一处就能核对事件字段。
    std::vector<EventRecord> recent_timer_events;
    if (EventRing::Enabled()) {
        for (const auto& event : EventRing::Snapshot(0)) {
            const auto code = static_cast<SchedEventCode>(event.code);
            if (code == SchedEventCode::kTimerArm ||
                code == SchedEventCode::kTimerFire ||
                code == SchedEventCode::kTimerCancel ||
                code == SchedEventCode::kTimerReschedule) {
                recent_timer_events.push_back(event);
            }
        }
        if (recent_timer_events.size() > 8U) {
            recent_timer_events.erase(recent_timer_events.begin(),
                                      recent_timer_events.end() - 8);
        }
    }

    const std::uint64_t captured_ns = ToSteadyNs(snapshot.captured_at);
    const std::uint64_t next_ns =
        snapshot.next_deadline ? ToSteadyNs(*snapshot.next_deadline) : 0;
    const std::uint64_t relative_ns =
        snapshot.next_deadline && next_ns > captured_ns
            ? next_ns - captured_ns
            : 0;

    TextDump text;
    text.Head("go2cpp.dump.timer/1");
    text.Key("consistent", "per_call");
    AppendConsistency(text, stamp);
    text.Key("steady_ns", SteadyNs());
    text.Key("captured_at_ns", captured_ns);
    text.Key("dump_seq", snapshot.seq);
    text.Key("active", static_cast<std::uint64_t>(snapshot.active));
    text.Key("next_deadline_ns", next_ns);
    text.Key("next_deadline_in_ns", relative_ns);
    text.Key("armed_total", snapshot.armed_total);
    text.Key("stats.fired", snapshot.fired);
    text.Key("stats.cancelled", snapshot.cancelled);
    text.Key("stats.rescheduled", snapshot.rescheduled);
    text.Key("stats.completed", snapshot.completed);
    text.Key("stats.failed", snapshot.failed);
    text.Key("source.count",
             static_cast<std::uint64_t>(core::kTimerSourceCount));
    for (std::size_t index = 0; index < snapshot.sources.size(); ++index) {
        const auto source = static_cast<core::TimerSource>(index);
        const std::string prefix =
            std::string("source.") + core::TimerSourceName(source) + ".";
        const auto& stats = snapshot.sources[index];
        text.Key(prefix + "active", stats.active);
        text.Key(prefix + "armed", stats.armed);
        text.Key(prefix + "fired", stats.fired);
        text.Key(prefix + "cancelled", stats.cancelled);
        text.Key(prefix + "rescheduled", stats.rescheduled);
        text.Key(prefix + "completed", stats.completed);
        text.Key(prefix + "failed", stats.failed);
    }
    text.Key("running", snapshot.running);
    text.Key("stopping", snapshot.stopping);
    text.Key("thread.joinable", snapshot.thread_joinable);
    text.KeyHex("thread.id", snapshot.thread_id);
    text.Key("thread.state",
             core::TimerThreadStateName(snapshot.thread_state));
    text.Key("thread.wait_wakeups", snapshot.background_wait_wakeups);
    text.Key("event_ring_supported", snapshot.event_ring_supported);
    text.Key("timer_events.count",
             static_cast<std::uint64_t>(recent_timer_events.size()));
    for (std::size_t index = 0; index < recent_timer_events.size(); ++index) {
        const auto& event = recent_timer_events[index];
        const std::string prefix =
            "timer_event." + std::to_string(index) + ".";
        text.Key(prefix + "code",
                 SchedEventCodeName(
                     static_cast<SchedEventCode>(event.code)));
        text.Key(prefix + "seq", event.seq);
        text.Key(prefix + "timer_id", event.gid);
        text.Key(prefix + "kind", event.arg3);
        text.Key(prefix + "deadline_ns", event.arg2);
        text.Key(prefix + "source_gid", event.arg4);
    }

    std::string json = "{\"schema\":\"go2cpp.dump.timer/1\",\"version\":1";
    json += ",\"consistent\":\"per_call\"";
    json += JsonConsistency(stamp);
    json += ",\"steady_ns\":" + std::to_string(SteadyNs());
    json += ",\"captured_at_ns\":" + std::to_string(captured_ns);
    json += ",\"dump_seq\":" + std::to_string(snapshot.seq);
    json += ",\"active\":" + std::to_string(snapshot.active);
    json += ",\"next_deadline_ns\":" + std::to_string(next_ns);
    json += ",\"next_deadline_in_ns\":" + std::to_string(relative_ns);
    json += ",\"armed_total\":" + std::to_string(snapshot.armed_total);
    json += ",\"stats\":{\"fired\":" + std::to_string(snapshot.fired) +
            ",\"cancelled\":" + std::to_string(snapshot.cancelled) +
            ",\"rescheduled\":" + std::to_string(snapshot.rescheduled) +
            ",\"completed\":" + std::to_string(snapshot.completed) +
            ",\"failed\":" + std::to_string(snapshot.failed) + "}";
    json += ",\"source_count\":" +
            std::to_string(static_cast<std::uint64_t>(
                core::kTimerSourceCount));
    json += ",\"sources\":{";
    for (std::size_t index = 0; index < snapshot.sources.size(); ++index) {
        if (index != 0) {
            json += ',';
        }
        const auto source = static_cast<core::TimerSource>(index);
        const auto& stats = snapshot.sources[index];
        json += "\"";
        json += core::TimerSourceName(source);
        json += "\":{\"active\":" + std::to_string(stats.active);
        json += ",\"armed\":" + std::to_string(stats.armed);
        json += ",\"fired\":" + std::to_string(stats.fired);
        json += ",\"cancelled\":" + std::to_string(stats.cancelled);
        json += ",\"rescheduled\":" + std::to_string(stats.rescheduled);
        json += ",\"completed\":" + std::to_string(stats.completed);
        json += ",\"failed\":" + std::to_string(stats.failed) + "}";
    }
    json += "}";
    json += ",\"running\":" + BoolText(snapshot.running);
    json += ",\"stopping\":" + BoolText(snapshot.stopping);
    json += ",\"thread\":{\"joinable\":" +
            BoolText(snapshot.thread_joinable);
    json += ",\"id\":\"" + HexValue(snapshot.thread_id) + "\"";
    json += ",\"state\":\"";
    json += core::TimerThreadStateName(snapshot.thread_state);
    json += "\",\"wait_wakeups\":" +
            std::to_string(snapshot.background_wait_wakeups) + "}";
    json += ",\"event_ring_supported\":" +
            BoolText(snapshot.event_ring_supported);
    json += ",\"timer_events\":{\"count\":" +
            std::to_string(recent_timer_events.size()) + ",\"recent\":[";
    for (std::size_t index = 0; index < recent_timer_events.size(); ++index) {
        if (index != 0) {
            json += ',';
        }
        const auto& event = recent_timer_events[index];
        json += "{\"code\":\"";
        json += SchedEventCodeName(
            static_cast<SchedEventCode>(event.code));
        json += "\",\"seq\":" + std::to_string(event.seq);
        json += ",\"timer_id\":" + std::to_string(event.gid);
        json += ",\"kind\":" + std::to_string(event.arg3);
        json += ",\"deadline_ns\":" + std::to_string(event.arg2);
        json += ",\"source_gid\":" + std::to_string(event.arg4);
        json += "}";
    }
    json += "]}}";

    DumpPayload payload;
    payload.text = text.Take();
    payload.json = std::move(json);
    return payload;
}

DumpPayload DumpEvents(std::size_t max) {
    DumpPayload payload;
    payload.text = EventRing::DumpText(max);
    payload.json = EventRing::DumpJson(max);
    return payload;
}

DumpPayload DumpRuntimeInfo() {
    ConsistencyStamp stamp = BeginConsistency();
    TextDump text;
    text.Head("go2cpp.dump.runtime/1");
    text.Key("consistent", "per_call");
    AppendConsistency(text, stamp);
    text.Key("pid", static_cast<std::int64_t>(::getpid()));
    text.Key("debug_dump_enabled", EventRing::Enabled());
    text.Key("event_ring_enabled", EventRing::Enabled());
    text.Key("event_ring_threads", EventRing::ThreadCount());
    text.Key("event_ring_dropped_threads", EventRing::DroppedThreads());

    std::string json = "{\"schema\":\"go2cpp.dump.runtime/1\",\"version\":1";
    json += ",\"consistent\":\"per_call\"";
    json += JsonConsistency(stamp);
    json += ",\"pid\":" + std::to_string(::getpid());
    json += ",\"debug_dump_enabled\":" + BoolText(EventRing::Enabled());
    json += ",\"event_ring_enabled\":" + BoolText(EventRing::Enabled());
    json += ",\"event_ring_threads\":" +
            std::to_string(EventRing::ThreadCount());
    json += ",\"event_ring_dropped_threads\":" +
            std::to_string(EventRing::DroppedThreads()) + "}";
    EndConsistency(stamp);

    DumpPayload payload;
    payload.text = text.Take();
    payload.json = std::move(json);
    return payload;
}

bool WriteDumpFile(const std::string& path, const DumpPayload& payload,
                   bool json, std::string* error) {
    const std::string& content = json ? payload.json : payload.text;
    const std::string temporary = path + ".tmp";
    {
        std::FILE* file = std::fopen(temporary.c_str(), "wb");
        if (file == nullptr) {
            if (error != nullptr) {
                *error = "cannot open " + temporary + ": " +
                         std::strerror(errno);
            }
            return false;
        }
        const std::size_t written =
            std::fwrite(content.data(), 1, content.size(), file);
        const bool ok = written == content.size() && std::fflush(file) == 0;
        std::fclose(file);
        if (!ok) {
            if (error != nullptr) {
                *error = "write failed for " + temporary + ": " +
                         std::strerror(errno);
            }
            (void)std::remove(temporary.c_str());
            return false;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        // 9p / 某些网络文件系统不支持 rename；退化为直接覆盖，并在
        // error 里如实记录（调用方仍能拿到完整文件）。
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) {
            if (error != nullptr) {
                *error = "rename failed and cannot open " + path + ": " +
                         std::strerror(errno);
            }
            (void)std::remove(temporary.c_str());
            return false;
        }
        const std::size_t written =
            std::fwrite(content.data(), 1, content.size(), file);
        std::fclose(file);
        (void)std::remove(temporary.c_str());
        if (written != content.size()) {
            if (error != nullptr) {
                *error = "fallback write failed for " + path;
            }
            return false;
        }
    }
    return true;
}



// ---------------------------------------------------------------------------
// Agent 可读视图（JSON 优先；字段稳定）
//
// 所有视图只消费 `EventRing::Snapshot()` 与调用方已经采集好的组件 JSON，
// 不调用会改变运行时状态的接口。窗口内的事件受环形容量限制，`window.gaps`
// 与 `window.total_published` 如实标注截断；进程级累计次数见 `totals`。
// ---------------------------------------------------------------------------

namespace {

using ViewJson = go2cpp::Json;

ViewJson ParseViewJson(const std::string& text) {
    if (text.empty()) {
        return ViewJson::object();
    }
    const auto parsed = go2cpp::ParseJson(text);
    if (!parsed.ok()) {
        return ViewJson::object();
    }
    return parsed.value;
}

bool IsTimerEventCode(SchedEventCode code) noexcept {
    switch (code) {
        case SchedEventCode::kTimerArm:
        case SchedEventCode::kTimerFire:
        case SchedEventCode::kTimerCancel:
        case SchedEventCode::kTimerReschedule:
            return true;
        default:
            return false;
    }
}

bool IsWakeEventCode(SchedEventCode code) noexcept {
    switch (code) {
        case SchedEventCode::kWakeEnqueued:
        case SchedEventCode::kWakePending:
        case SchedEventCode::kWakeRejected:
        case SchedEventCode::kWakeBatch:
            return true;
        default:
            return false;
    }
}

bool IsParkEventCode(SchedEventCode code) noexcept {
    switch (code) {
        case SchedEventCode::kParkEnter:
        case SchedEventCode::kParkReturn:
        case SchedEventCode::kParkRejected:
            return true;
        default:
            return false;
    }
}

bool IsGEventCode(SchedEventCode code) noexcept {
    switch (code) {
        case SchedEventCode::kGCreated:
        case SchedEventCode::kGStarted:
        case SchedEventCode::kGResumed:
        case SchedEventCode::kGCompleted:
        case SchedEventCode::kGFailed:
        case SchedEventCode::kGCancelled:
            return true;
        default:
            return false;
    }
}

bool IsSchedulerFlowCode(SchedEventCode code) noexcept {
    if (IsParkEventCode(code) || IsWakeEventCode(code) || IsGEventCode(code)) {
        return true;
    }
    switch (code) {
        case SchedEventCode::kEnqueue:
        case SchedEventCode::kEnqueueRejected:
        case SchedEventCode::kRequeueWorker:
        case SchedEventCode::kRequeueProcessor:
        case SchedEventCode::kRunQueuePush:
        case SchedEventCode::kRunQueuePop:
        case SchedEventCode::kRunQueueClaimOk:
        case SchedEventCode::kRunQueueClaimFail:
        case SchedEventCode::kStealOk:
        case SchedEventCode::kStealFail:
        case SchedEventCode::kWorkerPark:
        case SchedEventCode::kWorkerUnpark:
        case SchedEventCode::kSysmonPass:
        case SchedEventCode::kTimerEnqueue:
        case SchedEventCode::kThreadStart:
        case SchedEventCode::kThreadStop:
        case SchedEventCode::kSchedulerStart:
        case SchedEventCode::kSchedulerStop:
            return true;
        default:
            return false;
    }
}

bool IsPrimitiveCode(SchedEventCode code) noexcept {
    switch (code) {
        case SchedEventCode::kMutexLock:
        case SchedEventCode::kMutexUnlock:
        case SchedEventCode::kMutexFastLock:
        case SchedEventCode::kChannelSend:
        case SchedEventCode::kChannelRecv:
        case SchedEventCode::kChannelWake:
        case SchedEventCode::kChannelSendOk:
        case SchedEventCode::kChannelRecvOk:
        case SchedEventCode::kChannelSelectEnter:
        case SchedEventCode::kChannelSelect:
        case SchedEventCode::kPCWait:
        case SchedEventCode::kPCNotify:
        case SchedEventCode::kPCNotifyAll:
        case SchedEventCode::kTimerCallback:
        case SchedEventCode::kIoWait:
        case SchedEventCode::kIoWake:
        case SchedEventCode::kIoWakeBatch:
            return true;
        default:
            return false;
    }
}

/// 事件的可读详情（给 agent 直接读，不需要再查事件码语义）。
std::string EventDetail(SchedEventCode code, const EventRecord& event) {
    const auto primitive = PrimitiveReasonName(
        static_cast<PrimitiveReasonCode>(event.arg3));
    switch (code) {
        case SchedEventCode::kParkEnter:
            return std::string("suspend=") +
                   SuspendReasonNameStable(
                       static_cast<SuspendReason>(event.arg2)) +
                   " primitive=" + primitive;
        case SchedEventCode::kParkReturn:
            return event.arg2 == 0 ? "returned-cancelled" : "returned";
        case SchedEventCode::kParkRejected:
            return "rejected reason=" + std::to_string(event.arg2);
        case SchedEventCode::kWakeEnqueued:
            return std::string("wake-enqueued reason=") + primitive;
        case SchedEventCode::kWakePending:
            return std::string("wake-pending reason=") + primitive;
        case SchedEventCode::kWakeRejected:
            return std::string("wake-rejected reason=") + primitive;
        case SchedEventCode::kEnqueue:
            return "enqueue-request";
        case SchedEventCode::kEnqueueRejected:
            return "enqueue-rejected reason=" + std::to_string(event.arg2);
        case SchedEventCode::kRunQueuePush:
            return event.arg2 == kGlobalQueueTag
                       ? "push global"
                       : "push P=" + std::to_string(event.arg2);
        case SchedEventCode::kRunQueuePop:
            return event.arg2 == kGlobalQueueTag
                       ? "pop global"
                       : "pop P=" + std::to_string(event.arg2);
        case SchedEventCode::kRunQueueClaimOk:
            return "claim-ok";
        case SchedEventCode::kRunQueueClaimFail:
            return "claim-fail";
        case SchedEventCode::kStealOk:
            return "steal-ok victim-P=" + std::to_string(event.arg2);
        case SchedEventCode::kStealFail:
            return "steal-fail scanned=" + std::to_string(event.arg2);
        case SchedEventCode::kWorkerPark:
            return "M-park P=" + std::to_string(event.arg2);
        case SchedEventCode::kWorkerUnpark:
            return event.arg2 == 1 ? "M-unpark work"
                 : event.arg2 == 2 ? "M-unpark stopping"
                                   : "M-unpark timeout";
        case SchedEventCode::kSysmonPass:
            return "sysmon-pass #" + std::to_string(event.arg2);
        case SchedEventCode::kGCreated:
            return "created";
        case SchedEventCode::kGStarted:
            return "started M=" + std::to_string(event.arg2);
        case SchedEventCode::kGResumed:
            return "resumed M=" + std::to_string(event.arg2);
        case SchedEventCode::kGCompleted:
            return "completed";
        case SchedEventCode::kGFailed:
            return "failed";
        case SchedEventCode::kGCancelled:
            return "cancelled";
        case SchedEventCode::kMutexLock:
            return "mutex-contended";
        case SchedEventCode::kMutexUnlock:
            return "mutex-unlock+wake";
        case SchedEventCode::kMutexFastLock:
            return "mutex-fast-lock";
        case SchedEventCode::kChannelSend:
            return "channel-send-block cap=" + std::to_string(event.arg2);
        case SchedEventCode::kChannelRecv:
            return "channel-recv-block cap=" + std::to_string(event.arg2);
        case SchedEventCode::kChannelWake:
            return event.arg2 == 1 ? "channel-wake sender" : "channel-wake receiver";
        case SchedEventCode::kChannelSendOk:
            return "channel-send-immediate cap=" + std::to_string(event.arg2);
        case SchedEventCode::kChannelRecvOk:
            return "channel-recv-immediate cap=" + std::to_string(event.arg2);
        case SchedEventCode::kChannelSelectEnter:
            return "select-enter cases=" + std::to_string(event.arg2);
        case SchedEventCode::kChannelSelect:
            return event.arg2 == kNoSelectCase
                       ? "select-none"
                       : "select-case=" + std::to_string(event.arg2);
        case SchedEventCode::kPCWait:
            return "pc-wait";
        case SchedEventCode::kPCNotify:
            return "pc-notify";
        case SchedEventCode::kPCNotifyAll:
            return "pc-notify-all n=" + std::to_string(event.arg2);
        case SchedEventCode::kTimerArm:
            return "timer-arm deadline_ns=" + std::to_string(event.arg2);
        case SchedEventCode::kTimerFire:
            return "timer-fire deadline_ns=" + std::to_string(event.arg2);
        case SchedEventCode::kTimerCancel:
            return "timer-cancel";
        case SchedEventCode::kTimerReschedule:
            return "timer-reschedule deadline_ns=" + std::to_string(event.arg2);
        case SchedEventCode::kTimerCallback:
            return "timer-callback timer_id=" + std::to_string(event.arg2);
        case SchedEventCode::kTimerEnqueue:
            return "timer-G-enqueued";
        case SchedEventCode::kIoWait:
            return "io-wait fd=" + std::to_string(event.arg2);
        case SchedEventCode::kIoWake:
            return "io-wake fd=" + std::to_string(event.arg2);
        default:
            return SchedEventCodeName(code);
    }
}

ViewJson EventToJson(const EventRecord& event) {
    const auto code = static_cast<SchedEventCode>(event.code);
    ViewJson item;
    item["seq"] = event.seq;
    item["ts_ns"] = event.ts_ns;
    item["thread_id"] = event.thread_id;
    item["tid"] = event.tid;
    if (event.pid == kInvalidProcessorId) {
        item["pid"] = nullptr;
    } else {
        item["pid"] = event.pid;
    }
    item["code"] = SchedEventCodeName(code);
    item["code_value"] = event.code;
    item["gid"] = event.gid;
    item["arg2"] = event.arg2;
    item["arg3"] = event.arg3;
    item["arg4"] = event.arg4;
    const bool has_primitive =
        code == SchedEventCode::kParkEnter || IsWakeEventCode(code);
    item["primitive_reason"] =
        has_primitive
            ? PrimitiveReasonName(
                  static_cast<PrimitiveReasonCode>(event.arg3))
            : "";
    item["park_context"] =
        code == SchedEventCode::kParkEnter ? event.arg4 : 0;
    item["critical"] = SchedEventCodeIsCritical(code);
    item["resource_id"] = event.resource_id;
    item["target_gid"] = event.target_gid;
    item["result"] = event.result;
    item["result_name"] =
        ResourceResultName(static_cast<ResourceResult>(event.result));
    item["waiter_depth"] = event.waiters;
    item["value"] = event.value;
    item["value_known"] = (event.flags & kResourceFlagValueKnown) != 0U;
    item["stack_id"] = event.stack_id;
    item["chain_id"] = event.chain_id;
    item["flags"] = event.flags;
    item["detail"] = EventDetail(code, event);
    return item;
}

ViewJson WindowJson(const std::vector<EventRecord>& events,
                    std::uint64_t seq_start, std::uint64_t seq_end) {
    ViewJson window;
    window["event_count"] = events.size();
    // v6：seq 是 per-thread 序号，跨线程不可直接相减；
    // first_seq/last_seq 保留为首尾条目的本线程序号，另给出时间戳。
    window["first_seq"] = events.empty() ? 0 : events.front().seq;
    window["last_seq"] = events.empty() ? 0 : events.back().seq;
    window["first_ts_ns"] = events.empty() ? 0 : events.front().ts_ns;
    window["last_ts_ns"] = events.empty() ? 0 : events.back().ts_ns;
    // 按 thread_id 分组检测跳号（ring 覆盖/窗口截断）；
    // 首条不计，同线程的 normal/critical 两段共享同一计数器。
    std::map<std::uint32_t, std::uint64_t> last_seq_by_thread;
    std::uint64_t gaps = 0;
    for (const auto& event : events) {
        const auto found = last_seq_by_thread.find(event.thread_id);
        if (found != last_seq_by_thread.end() &&
            event.seq != found->second + 1U) {
            ++gaps;
        }
        last_seq_by_thread[event.thread_id] = event.seq;
    }
    const std::uint64_t total_published = EventRing::TotalCountAll();
    window["gaps"] = gaps;
    window["truncated"] =
        gaps != 0 || total_published > static_cast<std::uint64_t>(events.size());
    window["total_published"] = total_published;
    window["capacity_per_thread"] = EventRing::kCapacity;
    window["critical_capacity_per_thread"] = EventRing::kCriticalCapacity;
    window["threads"] = EventRing::ThreadCount();
    window["dropped_threads"] = EventRing::DroppedThreads();
    window["seq_at_start"] = seq_start;
    window["seq_at_end"] = seq_end;
    window["changed_during_snapshot"] = seq_start != seq_end;
    return window;
}

ViewJson TotalsJson() {
    struct Entry {
        const char* key;
        SchedEventCode code;
    };
    static const Entry kEntries[] = {
        {"park", SchedEventCode::kParkEnter},
        {"park_return", SchedEventCode::kParkReturn},
        {"park_rejected", SchedEventCode::kParkRejected},
        {"wake", SchedEventCode::kWakeEnqueued},
        {"wake_pending", SchedEventCode::kWakePending},
        {"wake_rejected", SchedEventCode::kWakeRejected},
        {"enqueue", SchedEventCode::kEnqueue},
        {"enqueue_rejected", SchedEventCode::kEnqueueRejected},
        {"claim_ok", SchedEventCode::kRunQueueClaimOk},
        {"claim_fail", SchedEventCode::kRunQueueClaimFail},
        {"runqueue_push", SchedEventCode::kRunQueuePush},
        {"runqueue_pop", SchedEventCode::kRunQueuePop},
        {"steal_ok", SchedEventCode::kStealOk},
        {"mutex_lock", SchedEventCode::kMutexLock},
        {"mutex_unlock", SchedEventCode::kMutexUnlock},
        {"mutex_fast_lock", SchedEventCode::kMutexFastLock},
        {"channel_send_block", SchedEventCode::kChannelSend},
        {"channel_recv_block", SchedEventCode::kChannelRecv},
        {"channel_send_ok", SchedEventCode::kChannelSendOk},
        {"channel_recv_ok", SchedEventCode::kChannelRecvOk},
        {"channel_select", SchedEventCode::kChannelSelect},
        {"pc_wait", SchedEventCode::kPCWait},
        {"pc_notify", SchedEventCode::kPCNotify},
        {"timer_arm", SchedEventCode::kTimerArm},
        {"timer_fire", SchedEventCode::kTimerFire},
        {"timer_cancel", SchedEventCode::kTimerCancel},
        {"timer_reschedule", SchedEventCode::kTimerReschedule},
        {"timer_callback", SchedEventCode::kTimerCallback},
        {"timer_enqueue", SchedEventCode::kTimerEnqueue},
        {"g_created", SchedEventCode::kGCreated},
        {"g_started", SchedEventCode::kGStarted},
        {"g_resumed", SchedEventCode::kGResumed},
        {"g_completed", SchedEventCode::kGCompleted},
        {"g_failed", SchedEventCode::kGFailed},
        {"g_cancelled", SchedEventCode::kGCancelled},
        {"worker_park", SchedEventCode::kWorkerPark},
        {"worker_unpark", SchedEventCode::kWorkerUnpark},
        {"sysmon_pass", SchedEventCode::kSysmonPass},
    };
    ViewJson totals = ViewJson::object();
    for (const auto& entry : kEntries) {
        totals[entry.key] = EventRing::TotalCount(entry.code);
    }
    return totals;
}

/// parking / wake 配对结果。
struct ParkPair {
    std::uint64_t gid{0};
    std::uint64_t park_seq{0};
    std::uint64_t park_ts{0};
    std::uint64_t suspend_reason{0};
    std::uint64_t primitive_reason{0};
    std::uint64_t wake_seq{0};
    std::uint64_t wake_ts{0};
    std::uint64_t wake_code{0};
    std::uint64_t wake_reason{0};
    std::uint64_t return_seq{0};
    std::uint64_t return_ts{0};
    bool returned{false};
};

std::vector<ParkPair> PairParks(const std::vector<EventRecord>& events) {
    std::vector<ParkPair> pairs;
    std::map<std::uint64_t, std::vector<std::size_t>> open_by_gid;
    for (const auto& event : events) {
        const auto code = static_cast<SchedEventCode>(event.code);
        if (code == SchedEventCode::kParkEnter && event.gid != 0) {
            ParkPair pair;
            pair.gid = event.gid;
            pair.park_seq = event.seq;
            pair.park_ts = event.ts_ns;
            pair.suspend_reason = event.arg2;
            pair.primitive_reason = event.arg3;
            pairs.push_back(pair);
            open_by_gid[event.gid].push_back(pairs.size() - 1);
        } else if (IsWakeEventCode(code) && event.gid != 0) {
            const auto found = open_by_gid.find(event.gid);
            if (found == open_by_gid.end()) {
                continue;
            }
            for (const auto index : found->second) {
                if (pairs[index].wake_seq == 0) {
                    pairs[index].wake_seq = event.seq;
                    pairs[index].wake_ts = event.ts_ns;
                    pairs[index].wake_code = event.code;
                    pairs[index].wake_reason = event.arg3;
                    break;
                }
            }
        } else if (code == SchedEventCode::kParkReturn && event.gid != 0) {
            const auto found = open_by_gid.find(event.gid);
            if (found == open_by_gid.end()) {
                continue;
            }
            for (auto it = found->second.rbegin(); it != found->second.rend();
                 ++it) {
                if (!pairs[*it].returned) {
                    pairs[*it].returned = true;
                    pairs[*it].return_seq = event.seq;
                    pairs[*it].return_ts = event.ts_ns;
                    break;
                }
            }
        }
    }
    return pairs;
}

std::string PairStatus(const ParkPair& pair) {
    const bool woke = pair.wake_seq != 0;
    if (woke && pair.returned) {
        return "paired";
    }
    if (woke && !pair.returned) {
        return "woken_pending_resume";
    }
    if (!woke && pair.returned) {
        return "returned_without_wake";
    }
    return "open";
}

ViewJson PairToJson(const ParkPair& pair) {
    ViewJson item;
    item["gid"] = pair.gid;
    item["park_seq"] = pair.park_seq;
    item["park_ts_ns"] = pair.park_ts;
    item["suspend_reason"] =
        SuspendReasonNameStable(static_cast<SuspendReason>(pair.suspend_reason));
    item["reason"] = PrimitiveReasonName(
        static_cast<PrimitiveReasonCode>(pair.primitive_reason));
    item["status"] = PairStatus(pair);
    if (pair.wake_seq != 0) {
        item["wake_seq"] = pair.wake_seq;
        item["wake_ts_ns"] = pair.wake_ts;
        item["wake_code"] = SchedEventCodeName(
            static_cast<SchedEventCode>(pair.wake_code));
        item["wake_reason"] = PrimitiveReasonName(
            static_cast<PrimitiveReasonCode>(pair.wake_reason));
        item["wake_latency_ns"] =
            pair.wake_ts > pair.park_ts ? pair.wake_ts - pair.park_ts : 0;
    } else {
        item["wake_seq"] = 0;
        item["wake_ts_ns"] = 0;
        item["wake_code"] = "";
        item["wake_reason"] = "";
        item["wake_latency_ns"] = 0;
    }
    if (pair.returned) {
        item["return_seq"] = pair.return_seq;
        item["resume_delay_ns"] =
            pair.return_ts > pair.park_ts ? pair.return_ts - pair.park_ts : 0;
    } else {
        item["return_seq"] = 0;
        item["resume_delay_ns"] = 0;
    }
    return item;
}

std::string TaskStateFromScheduler(const ViewJson& scheduler,
                                   std::uint64_t gid) {
    if (!scheduler.is_object() || !scheduler.contains("tasks") ||
        !scheduler["tasks"].is_array()) {
        return "";
    }
    for (const auto& task : scheduler["tasks"]) {
        if (task.value("id", std::uint64_t{0}) == gid) {
            return task.value("state", std::string{});
        }
    }
    return "";
}

bool SchedulerTaskStarted(const ViewJson& scheduler, std::uint64_t gid) {
    if (!scheduler.is_object() || !scheduler.contains("tasks") ||
        !scheduler["tasks"].is_array()) {
        return false;
    }
    for (const auto& task : scheduler["tasks"]) {
        if (task.value("id", std::uint64_t{0}) == gid) {
            return task.value("started", false);
        }
    }
    return false;
}

ViewJson SchedulerSummary(const ViewJson& scheduler) {
    ViewJson summary = ViewJson::object();
    if (!scheduler.is_object() || scheduler.empty()) {
        summary["available"] = false;
        return summary;
    }
    summary["available"] = true;
    summary["running"] = scheduler.value("running", false);
    summary["processor_count"] =
        scheduler.value("processor_count", std::uint64_t{0});
    summary["worker_count"] = scheduler.value("worker_count", std::uint64_t{0});
    summary["runnable_total"] =
        scheduler.value("runnable_total", std::uint64_t{0});
    summary["runnable_global"] =
        scheduler.value("runnable_global", std::uint64_t{0});
    summary["runqueue"] = scheduler.contains("runqueue")
                              ? scheduler["runqueue"]
                              : ViewJson::object();
    summary["sysmon_passes"] =
        scheduler.value("sysmon_passes", std::uint64_t{0});
    if (scheduler.contains("metrics")) {
        summary["metrics"] = scheduler["metrics"];
    }
    return summary;
}

ViewJson GmpView(const ViewJson& scheduler,
                 const std::vector<ParkPair>& pairs) {
    ViewJson gmp = ViewJson::object();
    gmp["summary"] = SchedulerSummary(scheduler);

    ViewJson processors = ViewJson::array();
    if (scheduler.contains("processors") && scheduler["processors"].is_array()) {
        for (const auto& processor : scheduler["processors"]) {
            ViewJson item;
            item["id"] = processor.value("id", std::uint64_t{0});
            item["state"] = processor.value("state", std::string{});
            item["queued"] = processor.value("queued", std::uint64_t{0});
            processors.push_back(item);
        }
    }
    gmp["processors"] = processors;

    ViewJson machines = ViewJson::array();
    if (scheduler.contains("machines") && scheduler["machines"].is_array()) {
        for (const auto& machine : scheduler["machines"]) {
            ViewJson item;
            item["id"] = machine.value("id", std::uint64_t{0});
            item["state"] = machine.value("state", std::string{});
            item["processor"] = machine.value("processor", std::uint64_t{0});
            item["blocking_task"] =
                machine.value("blocking_task", std::uint64_t{0});
            item["long_syscall_count"] =
                machine.value("long_syscall_count", std::uint64_t{0});
            item["replacement"] = machine.value("replacement", false);
            machines.push_back(item);
        }
    }
    gmp["machines"] = machines;

    ViewJson globals = ViewJson::array();
    ViewJson by_state = ViewJson::object();
    if (scheduler.contains("tasks") && scheduler["tasks"].is_array()) {
        for (const auto& task : scheduler["tasks"]) {
            ViewJson item;
            item["gid"] = task.value("id", std::uint64_t{0});
            item["state"] = task.value("state", std::string{});
            item["started"] = task.value("started", false);
            item["last_processor_id"] =
                task.value("last_processor_id", std::uint64_t{0});
            item["failed"] = task.value("failed", false);
            item["cancellation_requested"] =
                task.value("cancellation_requested", false);
            globals.push_back(item);
            const std::string state = item["state"];
            by_state[state] = by_state.value(state, std::uint64_t{0}) + 1;
        }
    }
    gmp["g"] = globals;
    gmp["g_by_state"] = by_state;

    // 阻塞图：窗口内未配对的 park + 当前仍处于 kWaiting 的 G。
    std::map<std::uint64_t, ViewJson> blocked;
    for (const auto& pair : pairs) {
        const std::string status = PairStatus(pair);
        if (status == "open" || status == "woken_pending_resume") {
            blocked[pair.gid] = PairToJson(pair);
        }
    }
    if (scheduler.contains("tasks") && scheduler["tasks"].is_array()) {
        for (const auto& task : scheduler["tasks"]) {
            const std::string state = task.value("state", std::string{});
            const std::uint64_t gid = task.value("id", std::uint64_t{0});
            if (state == "kWaiting" && blocked.find(gid) == blocked.end()) {
                ViewJson item;
                item["gid"] = gid;
                item["park_seq"] = 0;
                item["park_ts_ns"] = 0;
                item["suspend_reason"] = "kUnknown";
                item["reason"] = "kWindowMissing";
                item["status"] = "waiting_no_park_in_window";
                item["wake_seq"] = 0;
                item["wake_ts_ns"] = 0;
                item["wake_code"] = "";
                item["wake_reason"] = "";
                item["wake_latency_ns"] = 0;
                item["return_seq"] = 0;
                item["resume_delay_ns"] = 0;
                blocked[gid] = item;
            }
        }
    }
    ViewJson blocked_items = ViewJson::array();
    ViewJson blocked_by_reason = ViewJson::object();
    for (auto& entry : blocked) {
        blocked_items.push_back(entry.second);
        const std::string reason = entry.second.value("reason", std::string{});
        blocked_by_reason[reason] =
            blocked_by_reason.value(reason, std::uint64_t{0}) + 1;
    }
    ViewJson blocked_view;
    blocked_view["count"] = blocked_items.size();
    blocked_view["by_reason"] = blocked_by_reason;
    blocked_view["items"] = blocked_items;
    gmp["blocked"] = blocked_view;
    return gmp;
}

ViewJson TimerView(const ViewJson& timer_service,
                   const std::vector<EventRecord>& events) {
    ViewJson view = ViewJson::object();
    ViewJson summary = ViewJson::object();
    if (timer_service.is_object() && !timer_service.empty()) {
        summary["available"] = true;
        summary["active"] = timer_service.value("active", std::uint64_t{0});
        summary["next_deadline_in_ns"] =
            timer_service.value("next_deadline_in_ns", std::uint64_t{0});
        summary["armed_total"] =
            timer_service.value("armed_total", std::uint64_t{0});
        summary["fired"] =
            timer_service.value("stats", ViewJson::object())
                .value("fired", std::uint64_t{0});
        summary["cancelled"] =
            timer_service.value("stats", ViewJson::object())
                .value("cancelled", std::uint64_t{0});
        summary["rescheduled"] =
            timer_service.value("stats", ViewJson::object())
                .value("rescheduled", std::uint64_t{0});
        summary["running"] = timer_service.value("running", false);
        if (timer_service.contains("sources")) {
            summary["sources"] = timer_service["sources"];
        }
    } else {
        summary["available"] = false;
    }
    view["service"] = summary;

    struct TimerState {
        std::uint64_t timer_id{0};
        std::uint64_t arm_seq{0};
        std::uint64_t fire_count{0};
        std::uint64_t cancel_seq{0};
        std::uint64_t reschedule_count{0};
        std::uint64_t callback_count{0};
        std::uint64_t g_enqueue_count{0};
        std::uint64_t last_fire_ts{0};
        std::uint64_t last_callback_ts{0};
        std::uint64_t source_gid{0};
        std::uint64_t kind{0};
        std::uint64_t last_deadline_ns{0};
        bool saw_fire{false};
        bool saw_callback{false};
        bool saw_enqueue{false};
        std::vector<std::uint64_t> gids;
    };
    std::map<std::uint64_t, TimerState> timers;
    ViewJson timeline = ViewJson::array();
    std::uint64_t fire_with_callback = 0;
    std::uint64_t fire_total = 0;
    std::uint64_t fire_with_enqueue = 0;
    // 回调投递与 G 入队按时间邻近关联：执行器提交回调后立刻 spawn，
    // 两条事件在按时间排序的窗口里只隔少量条目。v6 的 seq 已是
    // per-thread，不能跨线程相减；改用排序后的条目距离。
    std::uint64_t last_callback_gid = 0;
    std::uint64_t callbacks_without_timer_id = 0;
    // fire → callback → G 入队的近邻关联（同一 timer 线程相邻记录）。
    std::size_t last_fire_index = 0;
    std::size_t last_callback_index = 0;
    bool has_last_fire = false;
    bool has_last_callback = false;
    std::size_t event_index = 0;
    std::uint64_t fire_events = 0;
    std::uint64_t fire_events_with_callback = 0;
    std::uint64_t fire_events_with_enqueue = 0;
    std::uint64_t g_enqueue_without_timer_id = 0;
    for (const auto& event : events) {
        const std::size_t current_index = event_index++;
        const auto code = static_cast<SchedEventCode>(event.code);
        if (!IsTimerEventCode(code) &&
            code != SchedEventCode::kTimerCallback &&
            code != SchedEventCode::kTimerEnqueue) {
            continue;
        }
        ViewJson item = EventToJson(event);
        item["timer_id"] = IsTimerEventCode(code) ? event.gid : event.arg2;
        item["kind"] = IsTimerEventCode(code) ? event.arg3 : 0;
        item["deadline_ns"] = IsTimerEventCode(code) ? event.arg2 : 0;
        item["source_gid"] = IsTimerEventCode(code) ? event.arg4 : event.gid;
        if (code == SchedEventCode::kTimerArm ||
            code == SchedEventCode::kTimerFire ||
            code == SchedEventCode::kTimerCancel ||
            code == SchedEventCode::kTimerReschedule) {
            TimerState& state = timers[event.gid];
            state.timer_id = event.gid;
            state.source_gid = event.arg4;
            state.kind = event.arg3;
            state.last_deadline_ns = event.arg2;
            if (code == SchedEventCode::kTimerArm) {
                state.arm_seq = event.seq;
            } else if (code == SchedEventCode::kTimerFire) {
                ++state.fire_count;
                state.last_fire_ts = event.ts_ns;
                state.saw_fire = true;
                ++fire_events;
                last_fire_index = current_index;
                has_last_fire = true;
            } else if (code == SchedEventCode::kTimerCancel) {
                state.cancel_seq = event.seq;
            } else {
                ++state.reschedule_count;
            }
        } else if (code == SchedEventCode::kTimerCallback) {
            if (event.arg2 == 0) {
                // 执行器路径的周期回调没有可用的 timer_id（ScheduleEveryOn
                // 的 repeat_callback 只能拿到回调本身）；单独计数，不能
                // 伪造成 timer_id=0 的定时器。
                ++callbacks_without_timer_id;
                item["timer_id_known"] = false;
            } else {
                TimerState& state = timers[event.arg2];
                state.timer_id = event.arg2;
                ++state.callback_count;
                state.last_callback_ts = event.ts_ns;
                state.saw_callback = true;
                item["timer_id_known"] = true;
            }
            if (has_last_fire &&
                current_index - last_fire_index <= 128) {
                ++fire_events_with_callback;
            }
            last_callback_index = current_index;
            has_last_callback = true;
            last_callback_gid = event.gid;
            item["source_gid"] = event.gid;
        } else {
            if (event.arg2 == 0) {
                // 执行器把回调交给 Scheduler 时只能拿到新 G，拿不到 timer_id；
                // 单独计数，不能伪造一个 timer_id=0 的定时器。
                ++g_enqueue_without_timer_id;
                item["timer_id_known"] = false;
            } else {
                TimerState& state = timers[event.arg2];
                state.timer_id = event.arg2;
                ++state.g_enqueue_count;
                state.gids.push_back(event.gid);
                state.saw_enqueue = true;
                item["timer_id_known"] = true;
            }
            item["source_gid"] = last_callback_gid;
            if (has_last_callback &&
                current_index - last_callback_index <= 128) {
                ++fire_events_with_enqueue;
            }
        }
        timeline.push_back(item);
    }

    fire_total = 0;
    fire_with_callback = 0;
    fire_with_enqueue = 0;
    for (const auto& entry : timers) {
        const auto& state = entry.second;
        if (state.saw_fire) {
            ++fire_total;
            if (state.saw_callback) {
                ++fire_with_callback;
            }
            if (state.saw_enqueue) {
                ++fire_with_enqueue;
            }
        }
    }
    ViewJson timer_map = ViewJson::object();
    for (const auto& entry : timers) {
        const auto& state = entry.second;
        ViewJson item;
        item["timer_id"] = state.timer_id;
        item["arm_seq"] = state.arm_seq;
        item["fire_count"] = state.fire_count;
        item["cancel_seq"] = state.cancel_seq;
        item["reschedule_count"] = state.reschedule_count;
        item["callback_count"] = state.callback_count;
        item["g_enqueue_count"] = state.g_enqueue_count;
        item["source_gid"] = state.source_gid;
        item["kind"] = state.kind;
        item["last_deadline_ns"] = state.last_deadline_ns;
        ViewJson gids = ViewJson::array();
        for (const auto gid : state.gids) {
            gids.push_back(gid);
        }
        item["g_enqueued"] = gids;
        timer_map[std::to_string(entry.first)] = item;
    }
    view["timers"] = timer_map;
    view["timeline"] = timeline;
    ViewJson pairing;
    pairing["fire_total"] = fire_total;
    pairing["fire_with_callback"] = fire_with_callback;
    pairing["fire_with_g_enqueue"] = fire_with_enqueue;
    const std::uint64_t fire_total_all =
        EventRing::TotalCount(SchedEventCode::kTimerFire);
    const std::uint64_t callback_total_all =
        EventRing::TotalCount(SchedEventCode::kTimerCallback);
    pairing["fire_total_all"] = fire_total_all;
    pairing["callback_total_all"] = callback_total_all;
    pairing["callback_without_fire_total"] =
        callback_total_all > fire_total_all
            ? callback_total_all - fire_total_all
            : 0;
    pairing["g_enqueue_total_all"] =
        EventRing::TotalCount(SchedEventCode::kTimerEnqueue);
    pairing["callbacks_without_timer_id"] = callbacks_without_timer_id;
    pairing["fire_events"] = fire_events;
    pairing["fire_events_with_callback"] = fire_events_with_callback;
    pairing["fire_events_with_enqueue"] = fire_events_with_enqueue;
    pairing["g_enqueues_without_timer_id"] = g_enqueue_without_timer_id;
    view["pairing"] = pairing;
    return view;
}

ViewJson ParkView(const std::vector<ParkPair>& pairs,
                  const ViewJson& scheduler) {
    ViewJson view = ViewJson::object();
    ViewJson by_reason = ViewJson::object();
    ViewJson items = ViewJson::array();
    std::uint64_t paired = 0;
    std::uint64_t woken_not_resumed = 0;
    std::uint64_t returned_without_wake = 0;
    std::uint64_t open = 0;
    for (const auto& pair : pairs) {
        items.push_back(PairToJson(pair));
        const std::string reason = PrimitiveReasonName(
            static_cast<PrimitiveReasonCode>(pair.primitive_reason));
        by_reason[reason] = by_reason.value(reason, std::uint64_t{0}) + 1;
        const std::string status = PairStatus(pair);
        if (status == "paired") {
            ++paired;
        } else if (status == "woken_pending_resume") {
            ++woken_not_resumed;
        } else if (status == "returned_without_wake") {
            ++returned_without_wake;
        } else {
            ++open;
        }
    }
    ViewJson pairing;
    pairing["checked"] = pairs.size();
    pairing["paired"] = paired;
    pairing["woken_pending_resume"] = woken_not_resumed;
    pairing["returned_without_wake"] = returned_without_wake;
    pairing["open"] = open;
    pairing["park_total"] = EventRing::TotalCount(SchedEventCode::kParkEnter);
    pairing["wake_total"] =
        EventRing::TotalCount(SchedEventCode::kWakeEnqueued);
    pairing["park_minus_wake_total"] =
        pairing["park_total"].get<std::uint64_t>() >=
                pairing["wake_total"].get<std::uint64_t>()
            ? pairing["park_total"].get<std::uint64_t>() -
                  pairing["wake_total"].get<std::uint64_t>()
            : 0;
    view["pairing"] = pairing;
    view["by_reason"] = by_reason;
    view["items"] = items;

    // 明确 stuck：窗口内 open 的 park 且当前 G 仍处于 kWaiting。
    ViewJson stuck = ViewJson::array();
    for (const auto& pair : pairs) {
        if (PairStatus(pair) != "open") {
            continue;
        }
        if (TaskStateFromScheduler(scheduler, pair.gid) == "kWaiting") {
            stuck.push_back(PairToJson(pair));
        }
    }
    view["stuck"] = stuck;
    return view;
}

ViewJson TimelineView(std::uint64_t gid, const std::vector<EventRecord>& events,
                      const ViewJson& scheduler) {
    ViewJson view = ViewJson::object();
    ViewJson timeline = ViewJson::array();
    ViewJson lifecycle = ViewJson::object();
    std::uint64_t park = 0;
    std::uint64_t wake = 0;
    std::uint64_t resume = 0;
    std::uint64_t push = 0;
    std::uint64_t pop = 0;
    std::uint64_t claim_ok = 0;
    std::uint64_t claim_fail = 0;
    for (const auto& event : events) {
        if (event.gid != gid) {
            continue;
        }
        const auto code = static_cast<SchedEventCode>(event.code);
        timeline.push_back(EventToJson(event));
        if (code == SchedEventCode::kParkEnter) {
            ++park;
        } else if (code == SchedEventCode::kWakeEnqueued) {
            ++wake;
        } else if (code == SchedEventCode::kGResumed) {
            ++resume;
        } else if (code == SchedEventCode::kRunQueuePush) {
            ++push;
        } else if (code == SchedEventCode::kRunQueuePop) {
            ++pop;
        } else if (code == SchedEventCode::kRunQueueClaimOk) {
            ++claim_ok;
        } else if (code == SchedEventCode::kRunQueueClaimFail) {
            ++claim_fail;
        } else if (code == SchedEventCode::kGCreated ||
                   code == SchedEventCode::kGStarted ||
                   code == SchedEventCode::kGCompleted ||
                   code == SchedEventCode::kGFailed ||
                   code == SchedEventCode::kGCancelled) {
            ViewJson marker;
            marker["seq"] = event.seq;
            marker["ts_ns"] = event.ts_ns;
            marker["code"] = SchedEventCodeName(code);
            switch (code) {
                case SchedEventCode::kGCreated: lifecycle["created"] = marker; break;
                case SchedEventCode::kGStarted: lifecycle["started"] = marker; break;
                case SchedEventCode::kGCompleted: lifecycle["completed"] = marker; break;
                case SchedEventCode::kGFailed: lifecycle["failed"] = marker; break;
                default: lifecycle["cancelled"] = marker; break;
            }
        }
    }
    ViewJson counts;
    counts["events"] = timeline.size();
    counts["park"] = park;
    counts["wake"] = wake;
    counts["resume"] = resume;
    counts["runqueue_push"] = push;
    counts["runqueue_pop"] = pop;
    counts["claim_ok"] = claim_ok;
    counts["claim_fail"] = claim_fail;
    view["gid"] = gid;
    view["found_in_window"] = !timeline.empty();
    std::string current_state = TaskStateFromScheduler(scheduler, gid);
    if (current_state.empty() && lifecycle.contains("completed")) {
        current_state = "kDead";
    } else if (current_state.empty() && lifecycle.contains("failed")) {
        current_state = "kFailed";
    } else if (current_state.empty() && lifecycle.contains("cancelled")) {
        current_state = "kCancelled";
    }
    view["current_state"] = current_state;
    view["started"] = SchedulerTaskStarted(scheduler, gid);
    view["lifecycle"] = lifecycle;
    view["counts"] = counts;
    view["events"] = timeline;
    return view;
}

ViewJson SchedView(const std::vector<EventRecord>& events,
                   const std::vector<ParkPair>& pairs,
                   const ViewJson& scheduler) {
    ViewJson view = ViewJson::object();
    ViewJson flow = ViewJson::array();
    ViewJson chains = ViewJson::array();
    ViewJson counts = ViewJson::object();
    std::uint64_t enqueue = 0;
    std::uint64_t enqueue_rejected = 0;
    std::uint64_t push = 0;
    std::uint64_t pop = 0;
    std::uint64_t claim_ok = 0;
    std::uint64_t claim_fail = 0;
    std::uint64_t steal_ok = 0;
    std::uint64_t worker_park = 0;
    std::uint64_t worker_unpark = 0;
    std::uint64_t sysmon = 0;

    struct Chain {
        std::uint64_t gid{0};
        std::uint64_t last_ts_ns{0};
        std::uint64_t enqueue_seq{0};
        std::uint64_t push_seq{0};
        std::uint64_t pop_seq{0};
        std::uint64_t claim_seq{0};
        std::uint64_t resume_seq{0};
        std::uint64_t park_seq{0};
        std::uint64_t wake_seq{0};
        std::uint64_t enqueue_count{0};
        std::uint64_t claim_count{0};
        std::uint64_t park_count{0};
        std::uint64_t wake_count{0};
    };
    std::map<std::uint64_t, Chain> chain_map;
    std::uint64_t max_ts_in_window = 0;
    for (const auto& event : events) {
        const auto code = static_cast<SchedEventCode>(event.code);
        if (!IsSchedulerFlowCode(code)) {
            continue;
        }
        if (event.ts_ns > max_ts_in_window) {
            max_ts_in_window = event.ts_ns;
        }
        flow.push_back(EventToJson(event));
        switch (code) {
            case SchedEventCode::kEnqueue: ++enqueue; break;
            case SchedEventCode::kEnqueueRejected: ++enqueue_rejected; break;
            case SchedEventCode::kRunQueuePush: ++push; break;
            case SchedEventCode::kRunQueuePop: ++pop; break;
            case SchedEventCode::kRunQueueClaimOk: ++claim_ok; break;
            case SchedEventCode::kRunQueueClaimFail: ++claim_fail; break;
            case SchedEventCode::kStealOk: ++steal_ok; break;
            case SchedEventCode::kWorkerPark: ++worker_park; break;
            case SchedEventCode::kWorkerUnpark: ++worker_unpark; break;
            case SchedEventCode::kSysmonPass: ++sysmon; break;
            default: break;
        }
        if (event.gid == 0) {
            continue;
        }
        Chain& chain = chain_map[event.gid];
        chain.gid = event.gid;
        // events 已按 (ts_ns, thread_id, seq) 升序，直接记最后一条的时间戳。
        chain.last_ts_ns = event.ts_ns;
        switch (code) {
            case SchedEventCode::kEnqueue:
                if (chain.enqueue_seq == 0) {
                    chain.enqueue_seq = event.seq;
                }
                ++chain.enqueue_count;
                break;
            case SchedEventCode::kRunQueuePush:
                if (chain.push_seq == 0) {
                    chain.push_seq = event.seq;
                }
                break;
            case SchedEventCode::kRunQueuePop:
                chain.pop_seq = event.seq;
                break;
            case SchedEventCode::kRunQueueClaimOk:
            case SchedEventCode::kRunQueueClaimFail:
                chain.claim_seq = event.seq;
                ++chain.claim_count;
                break;
            case SchedEventCode::kGResumed:
            case SchedEventCode::kGStarted:
                chain.resume_seq = event.seq;
                break;
            case SchedEventCode::kParkEnter:
                chain.park_seq = event.seq;
                ++chain.park_count;
                break;
            case SchedEventCode::kWakeEnqueued:
                chain.wake_seq = event.seq;
                ++chain.wake_count;
                break;
            default: break;
        }
    }

    // 只输出窗口内仍有活动、且链路不完整的 G（最多 64 条）。
    std::vector<const Chain*> candidates;
    for (const auto& entry : chain_map) {
        const Chain& chain = entry.second;
        const bool has_tail =
            chain.park_seq != 0 || chain.wake_seq != 0 ||
            chain.resume_seq != 0 || chain.claim_seq != 0;
        if (has_tail) {
            candidates.push_back(&chain);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Chain* lhs, const Chain* rhs) {
                  return lhs->last_ts_ns > rhs->last_ts_ns;
              });
    if (candidates.size() > 64) {
        candidates.resize(64);
    }
    for (const Chain* chain : candidates) {
        ViewJson item;
        item["gid"] = chain->gid;
        item["enqueue_count"] = chain->enqueue_count;
        item["claim_count"] = chain->claim_count;
        item["park_count"] = chain->park_count;
        item["wake_count"] = chain->wake_count;
        item["enqueue_seq"] = chain->enqueue_seq;
        item["push_seq"] = chain->push_seq;
        item["pop_seq"] = chain->pop_seq;
        item["claim_seq"] = chain->claim_seq;
        item["resume_seq"] = chain->resume_seq;
        item["park_seq"] = chain->park_seq;
        item["wake_seq"] = chain->wake_seq;
        item["state"] = TaskStateFromScheduler(scheduler, chain->gid);
        ViewJson missing = ViewJson::array();
        if (chain->enqueue_count > chain->claim_count) {
            missing.push_back("claim");
        }
        if (chain->claim_count != 0 && chain->pop_seq == 0) {
            missing.push_back("pop");
        }
        if (chain->park_count > chain->wake_count) {
            missing.push_back("wake");
        }
        item["missing"] = missing;
        item["status"] = missing.empty() ? "complete" : "incomplete";
        chains.push_back(item);
    }

    ViewJson pairing;
    pairing["enqueue_total"] =
        EventRing::TotalCount(SchedEventCode::kEnqueue);
    pairing["enqueue_rejected_total"] =
        EventRing::TotalCount(SchedEventCode::kEnqueueRejected);
    pairing["claim_ok_total"] =
        EventRing::TotalCount(SchedEventCode::kRunQueueClaimOk);
    pairing["claim_fail_total"] =
        EventRing::TotalCount(SchedEventCode::kRunQueueClaimFail);
    pairing["window_enqueue"] = enqueue;
    pairing["window_claim_ok"] = claim_ok;
    pairing["window_enqueue_pending"] =
        enqueue > enqueue_rejected + claim_ok + claim_fail
            ? enqueue - enqueue_rejected - claim_ok - claim_fail
            : 0;
    pairing["park_pairs"] = pairs.size();
    counts["enqueue"] = enqueue;
    counts["enqueue_rejected"] = enqueue_rejected;
    counts["runqueue_push"] = push;
    counts["runqueue_pop"] = pop;
    counts["claim_ok"] = claim_ok;
    counts["claim_fail"] = claim_fail;
    counts["steal_ok"] = steal_ok;
    counts["worker_park"] = worker_park;
    counts["worker_unpark"] = worker_unpark;
    counts["sysmon_pass"] = sysmon;
    counts["max_ts_ns"] = max_ts_in_window;
    // 兼容旧字段名：v6 下该值已无意义，固定为 0。
    counts["max_seq_in_window"] = 0;
    view["counts"] = counts;
    view["pairing"] = pairing;
    view["chains"] = chains;
    view["flow"] = flow;
    return view;
}

ViewJson PrimitivesView(const std::vector<EventRecord>& events,
                        const std::vector<ParkPair>& pairs) {
    ViewJson view = ViewJson::object();
    ViewJson flow = ViewJson::array();
    ViewJson counts = ViewJson::object();
    ViewJson selects = ViewJson::array();
    ViewJson mutex_items = ViewJson::array();
    ViewJson channel_items = ViewJson::array();
    ViewJson pc_items = ViewJson::array();
    std::uint64_t mutex_lock = 0;
    std::uint64_t mutex_unlock = 0;
    std::uint64_t mutex_fast = 0;
    std::uint64_t channel_send = 0;
    std::uint64_t channel_recv = 0;
    std::uint64_t channel_send_ok = 0;
    std::uint64_t channel_recv_ok = 0;
    std::uint64_t select_count = 0;
    std::uint64_t pc_wait = 0;
    std::uint64_t pc_notify = 0;
    for (const auto& event : events) {
        const auto code = static_cast<SchedEventCode>(event.code);
        if (!IsPrimitiveCode(code)) {
            continue;
        }
        ViewJson item = EventToJson(event);
        flow.push_back(item);
        switch (code) {
            case SchedEventCode::kMutexLock: {
                ++mutex_lock;
                mutex_items.push_back(item);
                break;
            }
            case SchedEventCode::kMutexUnlock: {
                ++mutex_unlock;
                mutex_items.push_back(item);
                break;
            }
            case SchedEventCode::kMutexFastLock:
                ++mutex_fast;
                mutex_items.push_back(item);
                break;
            case SchedEventCode::kChannelSend:
                ++channel_send;
                channel_items.push_back(item);
                break;
            case SchedEventCode::kChannelRecv:
                ++channel_recv;
                channel_items.push_back(item);
                break;
            case SchedEventCode::kChannelSendOk:
                ++channel_send_ok;
                channel_items.push_back(item);
                break;
            case SchedEventCode::kChannelRecvOk:
                ++channel_recv_ok;
                channel_items.push_back(item);
                break;
            case SchedEventCode::kChannelSelect: {
                ++select_count;
                ViewJson select_item;
                select_item["seq"] = event.seq;
                select_item["gid"] = event.gid;
                select_item["chosen"] =
                    event.arg2 == kNoSelectCase
                        ? ViewJson(nullptr)
                        : ViewJson(event.arg2);
                select_item["none"] = event.arg2 == kNoSelectCase;
                selects.push_back(select_item);
                break;
            }
            case SchedEventCode::kPCWait:
                ++pc_wait;
                pc_items.push_back(item);
                break;
            case SchedEventCode::kPCNotify:
            case SchedEventCode::kPCNotifyAll:
                ++pc_notify;
                pc_items.push_back(item);
                break;
            default:
                break;
        }
    }
    counts["mutex_lock"] = mutex_lock;
    counts["mutex_unlock"] = mutex_unlock;
    counts["mutex_fast_lock"] = mutex_fast;
    counts["channel_send_block"] = channel_send;
    counts["channel_recv_block"] = channel_recv;
    counts["channel_send_ok"] = channel_send_ok;
    counts["channel_recv_ok"] = channel_recv_ok;
    counts["channel_select"] = select_count;
    counts["pc_wait"] = pc_wait;
    counts["pc_notify"] = pc_notify;
    view["counts"] = counts;
    ViewJson park_by_reason = ViewJson::object();
    for (const auto& pair : pairs) {
        const std::string reason = PrimitiveReasonName(
            static_cast<PrimitiveReasonCode>(pair.primitive_reason));
        park_by_reason[reason] =
            park_by_reason.value(reason, std::uint64_t{0}) + 1;
    }
    view["park_reason_mix"] = park_by_reason;
    view["selects"] = selects;
    view["mutex"] = mutex_items;
    view["channel"] = channel_items;
    view["parking_condition"] = pc_items;
    view["flow"] = flow;
    return view;
}

void AppendCoverage(ViewJson& root, const std::vector<ParkPair>& pairs) {
    ViewJson coverage = ViewJson::object();
    const std::uint64_t park_total =
        EventRing::TotalCount(SchedEventCode::kParkEnter);
    const std::uint64_t wake_total =
        EventRing::TotalCount(SchedEventCode::kWakeEnqueued);
    coverage["park_total"] = park_total;
    coverage["wake_total"] = wake_total;
    coverage["park_minus_wake"] =
        park_total >= wake_total ? park_total - wake_total : 0;
    coverage["wake_pending_total"] =
        EventRing::TotalCount(SchedEventCode::kWakePending);
    coverage["wake_rejected_total"] =
        EventRing::TotalCount(SchedEventCode::kWakeRejected);
    coverage["park_return_total"] =
        EventRing::TotalCount(SchedEventCode::kParkReturn);
    coverage["park_rejected_total"] =
        EventRing::TotalCount(SchedEventCode::kParkRejected);
    coverage["enqueue_total"] =
        EventRing::TotalCount(SchedEventCode::kEnqueue);
    coverage["enqueue_rejected_total"] =
        EventRing::TotalCount(SchedEventCode::kEnqueueRejected);
    coverage["claim_ok_total"] =
        EventRing::TotalCount(SchedEventCode::kRunQueueClaimOk);
    coverage["claim_fail_total"] =
        EventRing::TotalCount(SchedEventCode::kRunQueueClaimFail);
    const std::uint64_t admitted = coverage["enqueue_total"].get<std::uint64_t>();
    const std::uint64_t rejected =
        coverage["enqueue_rejected_total"].get<std::uint64_t>();
    const std::uint64_t claimed =
        coverage["claim_ok_total"].get<std::uint64_t>();
    coverage["enqueue_pending_total"] =
        admitted > rejected + claimed ? admitted - rejected - claimed : 0;
    coverage["steal_ok_total"] =
        EventRing::TotalCount(SchedEventCode::kStealOk);
    coverage["g_created_total"] =
        EventRing::TotalCount(SchedEventCode::kGCreated);
    coverage["g_completed_total"] =
        EventRing::TotalCount(SchedEventCode::kGCompleted);
    coverage["g_failed_total"] =
        EventRing::TotalCount(SchedEventCode::kGFailed);
    coverage["g_cancelled_total"] =
        EventRing::TotalCount(SchedEventCode::kGCancelled);
    coverage["timer_fire_total"] =
        EventRing::TotalCount(SchedEventCode::kTimerFire);
    coverage["timer_callback_total"] =
        EventRing::TotalCount(SchedEventCode::kTimerCallback);
    coverage["timer_enqueue_total"] =
        EventRing::TotalCount(SchedEventCode::kTimerEnqueue);
    coverage["pc_wait_total"] =
        EventRing::TotalCount(SchedEventCode::kPCWait);
    coverage["pc_notify_total"] =
        EventRing::TotalCount(SchedEventCode::kPCNotify);
    coverage["window_pairs_checked"] = pairs.size();
    std::uint64_t paired = 0;
    std::uint64_t open = 0;
    for (const auto& pair : pairs) {
        if (PairStatus(pair) == "paired") {
            ++paired;
        } else if (PairStatus(pair) == "open") {
            ++open;
        }
    }
    coverage["window_pairs_paired"] = paired;
    coverage["window_pairs_open"] = open;
    root["coverage"] = coverage;
}

// ---------------------------------------------------------------------------
// 资源级历史视图（v3）：history / locks / channels / resources
//
// 数据来源：
//   - EventRing::Snapshot()：带 resource_id / target_gid / result / stack_id /
//     chain_id 的资源事件；
//   - 资源注册表（debug_resources.hpp）：全部 10 类并发结构的注册/注销记录；
//   - 线程栈样本表与 Fiber 链样本表：按 id 解析，视图输出时才做符号解析。
// ---------------------------------------------------------------------------

bool IsResourceEventCode(SchedEventCode code) noexcept {
    switch (code) {
        case SchedEventCode::kMutexLock:
        case SchedEventCode::kMutexUnlock:
        case SchedEventCode::kRwLockRead:
        case SchedEventCode::kRwUnlockRead:
        case SchedEventCode::kRwLockWrite:
        case SchedEventCode::kRwUnlockWrite:
        case SchedEventCode::kRecursiveLock:
        case SchedEventCode::kRecursiveUnlock:
        case SchedEventCode::kPCWait:
        case SchedEventCode::kPCNotify:
        case SchedEventCode::kPCNotifyAll:
        case SchedEventCode::kChannelSend:
        case SchedEventCode::kChannelRecv:
        case SchedEventCode::kChannelWake:
        case SchedEventCode::kChannelSendOk:
        case SchedEventCode::kChannelRecvOk:
        case SchedEventCode::kParkCaptured:
            return true;
        default:
            return false;
    }
}

bool IsChannelResourceCode(SchedEventCode code) noexcept {
    switch (code) {
        case SchedEventCode::kChannelSend:
        case SchedEventCode::kChannelRecv:
        case SchedEventCode::kChannelWake:
        case SchedEventCode::kChannelSendOk:
        case SchedEventCode::kChannelRecvOk:
        case SchedEventCode::kChannelSelect:
        case SchedEventCode::kChannelSelectEnter:
            return true;
        default:
            return false;
    }
}

bool IsLockEventCode(SchedEventCode code) noexcept {
    switch (code) {
        case SchedEventCode::kMutexLock:
        case SchedEventCode::kMutexUnlock:
        case SchedEventCode::kRwLockRead:
        case SchedEventCode::kRwUnlockRead:
        case SchedEventCode::kRwLockWrite:
        case SchedEventCode::kRwUnlockWrite:
        case SchedEventCode::kRecursiveLock:
        case SchedEventCode::kRecursiveUnlock:
            return true;
        default:
            return false;
    }
}

bool IsLockAcquireCode(SchedEventCode code) noexcept {
    return code == SchedEventCode::kMutexLock ||
           code == SchedEventCode::kRwLockRead ||
           code == SchedEventCode::kRwLockWrite ||
           code == SchedEventCode::kRecursiveLock;
}

bool IsLockReleaseCode(SchedEventCode code) noexcept {
    return code == SchedEventCode::kMutexUnlock ||
           code == SchedEventCode::kRwUnlockRead ||
           code == SchedEventCode::kRwUnlockWrite ||
           code == SchedEventCode::kRecursiveUnlock;
}

ViewJson StackSampleJson(StackSampleId id) {
    ViewJson out = ViewJson::object();
    StackSample sample;
    if (!LookupStackSample(id, &sample)) {
        out["id"] = id;
        out["available"] = false;
        return out;
    }
    out["id"] = id;
    out["available"] = true;
    out["thread_id"] = sample.thread_id;
    out["gid"] = sample.gid;
    out["ts_ns"] = sample.ts_ns;
    out["refs"] = sample.refs;
    ViewJson frames = ViewJson::array();
    for (auto& frame : ResolveStackSample(id)) {
        ViewJson item;
        item["address"] = static_cast<std::uint64_t>(frame.address);
        item["symbol"] = frame.symbol;
        item["module"] = frame.module;
        item["offset"] = static_cast<std::uint64_t>(frame.offset);
        item["resolved"] = frame.resolved;
        frames.push_back(item);
    }
    out["frames"] = frames;
    return out;
}

ViewJson FiberChainJson(FiberChainId id) {
    ViewJson out = ViewJson::object();
    FiberChainSample sample;
    if (!LookupFiberChain(id, &sample)) {
        out["id"] = id;
        out["available"] = false;
        return out;
    }
    out["id"] = id;
    out["available"] = true;
    out["thread_id"] = sample.thread_id;
    out["gid"] = sample.gid;
    out["ts_ns"] = sample.ts_ns;
    out["refs"] = sample.refs;
    ViewJson nodes = ViewJson::array();
    for (const auto& node : sample.nodes) {
        ViewJson item;
        item["fiber_id"] = node.fiber_id;
        item["gid"] = node.gid;
        item["state"] = node.state;
        item["state_name"] = FiberStateNameForDebug(node.state);
        item["suspend_reason"] = node.suspend_reason;
        item["depth"] = node.depth;
        item["alive"] = node.alive;
        item["main_fiber"] = node.main_fiber;
        item["function"] = node.function;
        item["stack_low"] = static_cast<std::uint64_t>(node.stack_low);
        item["stack_high"] = static_cast<std::uint64_t>(node.stack_high);
        item["stack_known"] = node.stack_known;
        nodes.push_back(item);
    }
    out["nodes"] = nodes;
    return out;
}

/// 单资源累计统计（视图中按资源 id 聚合）。
struct ResourceStats {
    std::uint64_t events{0};
    std::uint64_t holds{0};
    std::uint64_t waits{0};       ///< result=blocked 的次数
    std::uint64_t wakes{0};       ///< result=wake 的次数
    std::uint64_t timeouts{0};
    std::uint64_t cancels{0};
    std::uint64_t max_waiters{0};
    std::uint64_t hold_ns_total{0};
    std::uint64_t hold_ns_max{0};
    std::uint64_t wait_ns_total{0};
    std::uint64_t wait_ns_max{0};
    std::uint64_t sends{0};
    std::uint64_t recvs{0};
    std::uint64_t send_blocked{0};
    std::uint64_t recv_blocked{0};
    std::uint64_t value_known{0};
    std::uint64_t last_value{0};
    std::map<std::uint64_t, std::uint64_t> hold_start; ///< actor -> ts
    std::map<std::uint64_t, std::uint64_t> wait_start; ///< actor -> ts
};

void AccumulateResourceEvent(ResourceStats& stats, const EventRecord& event) {
    const auto code = static_cast<SchedEventCode>(event.code);
    const auto result = static_cast<ResourceResult>(event.result);
    ++stats.events;
    stats.max_waiters =
        std::max<std::uint64_t>(stats.max_waiters, event.waiters);
    if (result == ResourceResult::kBlocked) {
        ++stats.waits;
        stats.wait_start[event.gid] = event.ts_ns;
        if (code == SchedEventCode::kChannelSend) {
            ++stats.send_blocked;
        } else if (code == SchedEventCode::kChannelRecv) {
            ++stats.recv_blocked;
        }
    } else if (result == ResourceResult::kWake) {
        ++stats.wakes;
    } else if (result == ResourceResult::kTimeout) {
        ++stats.timeouts;
    } else if (result == ResourceResult::kCancelled) {
        ++stats.cancels;
    }
    if (code == SchedEventCode::kChannelSend ||
        code == SchedEventCode::kChannelSendOk) {
        ++stats.sends;
        if ((event.flags & kResourceFlagValueKnown) != 0U) {
            ++stats.value_known;
            stats.last_value = event.value;
        }
    } else if (code == SchedEventCode::kChannelRecv ||
               code == SchedEventCode::kChannelRecvOk) {
        ++stats.recvs;
        if ((event.flags & kResourceFlagValueKnown) != 0U) {
            ++stats.value_known;
            stats.last_value = event.value;
        }
    }
    if (IsLockAcquireCode(code)) {
        if (result == ResourceResult::kOk) {
            ++stats.holds;
            auto it = stats.hold_start.find(event.gid);
            if (it != stats.hold_start.end()) {
                // 同一 actor 重复 acquire（递归锁）：先按上一段结算。
                const std::uint64_t elapsed =
                    event.ts_ns >= it->second ? event.ts_ns - it->second : 0;
                stats.hold_ns_total += elapsed;
                stats.hold_ns_max = std::max(stats.hold_ns_max, elapsed);
            }
            stats.hold_start[event.gid] = event.ts_ns;
        }
        // 等待结束：同 actor 的 blocked -> ok / timeout / cancel。
        // 注意 blocked 事件本身就是 acquire code（kMutexLock 等），必须
        // 排除它自己，否则刚写入的起点会被同一事件立刻配对成 0。
        if (result != ResourceResult::kBlocked) {
            auto wait_it = stats.wait_start.find(event.gid);
            if (wait_it != stats.wait_start.end()) {
                if (event.ts_ns >= wait_it->second) {
                    const std::uint64_t elapsed = event.ts_ns - wait_it->second;
                    stats.wait_ns_total += elapsed;
                    stats.wait_ns_max = std::max(stats.wait_ns_max, elapsed);
                }
                stats.wait_start.erase(wait_it);
            }
        }
    }
    if (IsLockReleaseCode(code)) {
        auto it = stats.hold_start.find(event.gid);
        if (it != stats.hold_start.end()) {
            const std::uint64_t elapsed =
                event.ts_ns >= it->second ? event.ts_ns - it->second : 0;
            stats.hold_ns_total += elapsed;
            stats.hold_ns_max = std::max(stats.hold_ns_max, elapsed);
            stats.hold_start.erase(it);
        }
    }
}

ViewJson ResourceStatsJson(const ResourceStats& stats) {
    ViewJson out = ViewJson::object();
    out["events"] = stats.events;
    out["holds"] = stats.holds;
    out["hold_ns_total"] = stats.hold_ns_total;
    out["hold_ns_max"] = stats.hold_ns_max;
    out["waits"] = stats.waits;
    out["wait_ns_total"] = stats.wait_ns_total;
    out["wait_ns_max"] = stats.wait_ns_max;
    out["wakes"] = stats.wakes;
    out["timeouts"] = stats.timeouts;
    out["cancels"] = stats.cancels;
    out["max_waiters"] = stats.max_waiters;
    out["sends"] = stats.sends;
    out["recvs"] = stats.recvs;
    out["send_blocked"] = stats.send_blocked;
    out["recv_blocked"] = stats.recv_blocked;
    out["value_known"] = stats.value_known;
    out["last_value"] = stats.last_value;
    return out;
}

ViewJson ResourceInfoJson(const ResourceInfo& info) {
    ViewJson out = ViewJson::object();
    out["id"] = info.id;
    out["kind"] = ResourceKindName(info.kind);
    out["kind_code"] = static_cast<std::uint32_t>(info.kind);
    out["name"] = info.name;
    out["address"] = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(info.address));
    out["alive"] = info.alive;
    out["created_seq"] = info.created_seq;
    out["created_ts_ns"] = info.created_ts_ns;
    out["destroyed_seq"] = info.destroyed_seq;
    out["destroyed_ts_ns"] = info.destroyed_ts_ns;
    return out;
}

/// 全部资源的累计统计（按 resource_id 聚合事件）。
std::map<ResourceId, ResourceStats> AggregateResourceStats(
    const std::vector<EventRecord>& events) {
    std::map<ResourceId, ResourceStats> stats;
    for (const auto& event : events) {
        const auto code = static_cast<SchedEventCode>(event.code);
        if (!IsResourceEventCode(code) || event.resource_id == 0) {
            continue;
        }
        AccumulateResourceEvent(stats[event.resource_id], event);
    }
    return stats;
}

ViewJson ResourceHistoryView(std::uint64_t resource_id,
                             const std::vector<EventRecord>& events) {
    ViewJson view = ViewJson::object();
    ViewJson timeline = ViewJson::array();
    ResourceStats stats;
    for (const auto& event : events) {
        const auto code = static_cast<SchedEventCode>(event.code);
        if (!IsResourceEventCode(code)) {
            continue;
        }
        if (resource_id == 0) {
            if (code != SchedEventCode::kParkCaptured) {
                continue;
            }
        } else if (event.resource_id != resource_id) {
            continue;
        }
        ViewJson item = EventToJson(event);
        item["actor_gid"] = event.gid;
        item["target_gid"] = event.target_gid;
        item["resource_id"] = event.resource_id;
        item["result"] = event.result;
        item["result_name"] =
            ResourceResultName(static_cast<ResourceResult>(event.result));
        item["waiters"] = event.waiters;
        item["value"] = event.value;
        item["value_known"] =
            (event.flags & kResourceFlagValueKnown) != 0U;
        if (event.stack_id != 0) {
            item["stack"] = StackSampleJson(event.stack_id);
        }
        if (event.chain_id != 0) {
            item["fiber_chain"] = FiberChainJson(event.chain_id);
        }
        timeline.push_back(item);
        AccumulateResourceEvent(stats, event);
    }

    ResourceInfo info;
    if (resource_id != 0 && LookupResource(resource_id, &info)) {
        view["resource"] = ResourceInfoJson(info);
    } else if (resource_id != 0) {
        ViewJson missing;
        missing["id"] = resource_id;
        missing["known"] = false;
        view["resource"] = missing;
    }
    view["resource_id"] = resource_id;
    view["events"] = timeline;
    view["count"] = timeline.size();
    view["summary"] = ResourceStatsJson(stats);
    view["stack_samples"] = StackSampleCount();
    view["fiber_chain_samples"] = FiberChainCount();
    return view;
}

ViewJson LocksView(const std::vector<EventRecord>& events) {
    const auto stats = AggregateResourceStats(events);
    ViewJson view = ViewJson::object();
    ViewJson items = ViewJson::array();
    std::uint64_t lock_resources = 0;
    std::uint64_t holds = 0;
    std::uint64_t waits = 0;
    std::uint64_t wakes = 0;
    for (const auto& info : SnapshotResources()) {
        switch (info.kind) {
            case ResourceKind::kMutex:
            case ResourceKind::kRwMutex:
            case ResourceKind::kRecursiveMutex:
            case ResourceKind::kTimedMutex:
                break;
            default:
                continue;
        }
        ++lock_resources;
        ViewJson item = ResourceInfoJson(info);
        const auto it = stats.find(info.id);
        if (it != stats.end()) {
            const ViewJson numbers = ResourceStatsJson(it->second);
            for (const auto& key : {"events", "holds", "hold_ns_total",
                                    "hold_ns_max", "waits", "wait_ns_total",
                                    "wait_ns_max", "wakes", "timeouts",
                                    "cancels", "max_waiters"}) {
                item[key] = numbers[key];
            }
            holds += it->second.holds;
            waits += it->second.waits;
            wakes += it->second.wakes;
        } else {
            item["events"] = 0;
            item["holds"] = 0;
            item["waits"] = 0;
            item["wakes"] = 0;
            item["max_waiters"] = 0;
        }
        items.push_back(item);
    }
    view["locks"] = items;
    ViewJson counts;
    counts["resources"] = lock_resources;
    counts["holds"] = holds;
    counts["waits"] = waits;
    counts["wakes"] = wakes;
    view["counts"] = counts;
    return view;
}

ViewJson ChannelsView(const std::vector<EventRecord>& events) {
    const auto stats = AggregateResourceStats(events);
    ViewJson view = ViewJson::object();
    ViewJson items = ViewJson::array();
    std::uint64_t channel_resources = 0;
    std::uint64_t sends = 0;
    std::uint64_t recvs = 0;
    std::uint64_t blocked = 0;
    std::uint64_t wakes = 0;
    for (const auto& info : SnapshotResources()) {
        if (info.kind != ResourceKind::kChannel) {
            continue;
        }
        ++channel_resources;
        ViewJson item = ResourceInfoJson(info);
        const auto it = stats.find(info.id);
        if (it != stats.end()) {
            const ViewJson numbers = ResourceStatsJson(it->second);
            for (const auto& key : {"events", "sends", "recvs",
                                    "send_blocked", "recv_blocked", "wakes",
                                    "max_waiters", "value_known",
                                    "last_value"}) {
                item[key] = numbers[key];
            }
            sends += it->second.sends;
            recvs += it->second.recvs;
            blocked += it->second.send_blocked + it->second.recv_blocked;
            wakes += it->second.wakes;
        } else {
            item["events"] = 0;
            item["sends"] = 0;
            item["recvs"] = 0;
            item["wakes"] = 0;
            item["max_waiters"] = 0;
        }
        items.push_back(item);
    }
    view["channels"] = items;
    ViewJson counts;
    counts["resources"] = channel_resources;
    counts["sends"] = sends;
    counts["recvs"] = recvs;
    counts["blocked"] = blocked;
    counts["wakes"] = wakes;
    view["counts"] = counts;
    return view;
}

ViewJson ResourcesView(const std::vector<EventRecord>& events) {
    const auto stats = AggregateResourceStats(events);
    ViewJson view = ViewJson::object();
    ViewJson items = ViewJson::array();
    ViewJson by_kind = ViewJson::object();
    std::uint64_t alive = 0;
    for (const auto& info : SnapshotResources()) {
        ViewJson item = ResourceInfoJson(info);
        const auto it = stats.find(info.id);
        item["event_count"] = it == stats.end() ? 0 : it->second.events;
        if (it != stats.end()) {
            item["holds"] = it->second.holds;
            item["waits"] = it->second.waits;
            item["wakes"] = it->second.wakes;
            item["max_waiters"] = it->second.max_waiters;
        }
        items.push_back(item);
        const std::string kind = ResourceKindName(info.kind);
        by_kind[kind] = by_kind.value(kind, std::uint64_t{0}) + 1;
        if (info.alive) {
            ++alive;
        }
    }
    view["resources"] = items;
    view["count"] = items.size();
    view["alive"] = alive;
    view["by_kind"] = by_kind;
    view["stack_samples"] = StackSampleCount();
    view["stack_dropped"] = StackSampleDropped();
    view["fiber_chain_samples"] = FiberChainCount();
    view["fiber_chain_dropped"] = FiberChainDropped();

    // 项目内共有 10 类并发结构；明确列出覆盖状态，不静默跳过。
    ViewJson coverage = ViewJson::array();
    const auto add_kind = [&coverage](const char* kind, bool instrumented,
                                      const char* reason) {
        ViewJson item;
        item["kind"] = kind;
        item["instrumented"] = instrumented;
        item["reason"] = reason;
        coverage.push_back(item);
    };
    add_kind("sync::Mutex", true, "");
    add_kind("sync::RWMutex", true, "");
    add_kind("sync::RecursiveMutex", true, "");
    add_kind("core::ParkingCondition", true, "");
    add_kind("Channel<T>", true, "");
    add_kind("sync::ConditionVariable", true, "");
    add_kind("sync::WaitGroup", true, "");
    add_kind("sync::TimedMutex", true, "");
    add_kind("sync::SpinMutex", false,
             "纯自旋锁：无等待队列、无 park/唤醒事件；插桩会给无竞争路径"
             "增加原子操作，且无法提供等待者/唤醒字段");
    add_kind("core::FutexMutex", false,
             "最底层门闩：所有原语内部都经过它；逐次插桩会产生事件风暴并"
             "污染上层原语的资源归因，故只通过上层原语观测");
    view["coverage"] = coverage;
    return view;
}

}  // namespace

bool IsKnownViewName(std::string_view name) noexcept {
    return name == "gmp" || name == "timeline" || name == "sched" ||
           name == "park" || name == "timer" || name == "primitives" ||
           name == "history" || name == "locks" || name == "channels" ||
           name == "resources";
}

std::string BuildViewJson(std::string_view name, std::uint64_t gid,
                          const std::string& scheduler_json,
                          const std::string& timer_json,
                          const std::string& io_json) {
    ViewJson root = ViewJson::object();
    if (!IsKnownViewName(name)) {
        root["schema"] = "go2cpp.dump.view.error/1";
        root["version"] = 1;
        root["ok"] = false;
        root["error"] = "unknown-view";
        root["view"] = std::string(name);
        return root.dump() + "\n";
    }
    if (name == "timeline" && gid == 0) {
        root["schema"] = "go2cpp.dump.view.error/1";
        root["version"] = 1;
        root["ok"] = false;
        root["error"] = "missing-gid";
        root["view"] = std::string(name);
        return root.dump() + "\n";
    }

    const std::uint64_t seq_start = EventRing::PublishedEvents();
    const std::vector<EventRecord> events = EventRing::Snapshot(0);
    const std::uint64_t seq_end = EventRing::PublishedEvents();
    const ViewJson scheduler = ParseViewJson(scheduler_json);
    const ViewJson timer_service = ParseViewJson(timer_json);
    const ViewJson io_manager = ParseViewJson(io_json);
    const std::vector<ParkPair> pairs = PairParks(events);

    root["schema"] = "go2cpp.dump.view." + std::string(name) + "/1";
    root["version"] = 1;
    root["ok"] = true;
    root["view"] = std::string(name);
    root["gid"] = gid;
    root["generated_seq"] = seq_end;
    root["event_ring_enabled"] = EventRing::Enabled();
    root["window"] = WindowJson(events, seq_start, seq_end);
    root["totals"] = TotalsJson();
    AppendCoverage(root, pairs);

    if (name == "gmp") {
        root["gmp"] = GmpView(scheduler, pairs);
        ViewJson resources = ViewJson::object();
        resources["timer"] =
            TimerView(timer_service, events).value("service", ViewJson::object());
        resources["io"] = io_manager.is_object() && !io_manager.empty()
                              ? io_manager
                              : ViewJson(nullptr);
        root["resources"] = resources;
    } else if (name == "timeline") {
        root["timeline"] = TimelineView(gid, events, scheduler);
        ViewJson park_pairs = ViewJson::array();
        for (const auto& pair : pairs) {
            if (pair.gid == gid) {
                park_pairs.push_back(PairToJson(pair));
            }
        }
        root["parks"] = park_pairs;
    } else if (name == "sched") {
        ViewJson sched = SchedView(events, pairs, scheduler);
        root["sched"] = sched;
    } else if (name == "park") {
        root["park"] = ParkView(pairs, scheduler);
    } else if (name == "timer") {
        ViewJson timer_view = TimerView(timer_service, events);
        root["timer"] = timer_view;
    } else if (name == "primitives") {
        root["primitives"] = PrimitivesView(events, pairs);
    } else if (name == "history") {
        // history 复用 `VIEW <name> [n]` 的数值参数：对 history 它是
        // ResourceId，而不是 gid（服务端无需新增协议字段）。
        root["history"] = ResourceHistoryView(gid, events);
    } else if (name == "locks") {
        root["locks"] = LocksView(events);
    } else if (name == "channels") {
        root["channels"] = ChannelsView(events);
    } else if (name == "resources") {
        root["resources"] = ResourcesView(events);
    }
    return root.dump() + "\n";
}


bool RegisterTimer(const std::string& name, core::TimerService& service) {
    if (name.empty()) {
        return false;
    }
    return DebugServer::Instance().Register(
        name, "TimerService active/next-deadline/source stats/thread state",
        [&service]() { return DumpTimer(service); });
}

}  // namespace go2cpp::debug
