/**
 * @file debug_dump.cpp
 * @brief 统一 dump 序列化实现（text key=value + JSON）。
 *
 * 所有函数只读取已经采集好的快照；不调用任何会改变运行时状态的接口。
 */

#include "go2cpp/debug/debug_dump.hpp"
#include "go2cpp/debug/debug_server.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
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
    stamp.seq_start = EventRing::PublishSeq();
    stamp.seq_end = stamp.seq_start;
    return stamp;
}

void EndConsistency(ConsistencyStamp& stamp) noexcept {
    stamp.seq_end = EventRing::PublishSeq();
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


bool RegisterTimer(const std::string& name, core::TimerService& service) {
    if (name.empty()) {
        return false;
    }
    return DebugServer::Instance().Register(
        name, "TimerService active/next-deadline/source stats/thread state",
        [&service]() { return DumpTimer(service); });
}

}  // namespace go2cpp::debug
