#pragma once

/**
 * @file debug_events.hpp
 * @brief 调度事件的采集、缓冲与序列化（P2）。
 *
 * 设计依据：
 *   - DebugTooling/design/04-encoding-roadmap.md（采集层）
 *   - DebugTooling/perf-observer-overhead.md（实测：单 worker 下观察者净开销 3~6%，
 *     但多 worker 时 `emit_event` 内的 `observer_mutex` 会争用 —— 因此本模块
 *     刻意采用**非阻塞有界环形缓冲 + 满则丢弃并计数**，绝不在回调里阻塞 worker）
 *
 * **能力边界（如实声明）**：
 *   本模块采集的是 `SchedulerObserver` 能看到的**已声明事件**；
 *   未声明阻塞、未产生事件的运行时行为**看不到** —— 即设计文档反复强调的
 *   「没显示阻塞 ≠ 没有阻塞」。
 *   `Drain()` 拿到的是**采集点已入队**的事件，不保证与调度器某一瞬间严格一致。
 */

#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace go2cpp::debug {

/// 事件类型名（用于序列化）。未知值返回 "Unknown"，不猜测。
inline const char* EventTypeName(SchedulerEventType type) noexcept {
    switch (type) {
        case SchedulerEventType::kStarted: return "kStarted";
        case SchedulerEventType::kStopping: return "kStopping";
        case SchedulerEventType::kWorkerStarted: return "kWorkerStarted";
        case SchedulerEventType::kWorkerStopped: return "kWorkerStopped";
        case SchedulerEventType::kTaskStarted: return "kTaskStarted";
        case SchedulerEventType::kTaskSuspended: return "kTaskSuspended";
        case SchedulerEventType::kTaskCompleted: return "kTaskCompleted";
        case SchedulerEventType::kTaskFailed: return "kTaskFailed";
        case SchedulerEventType::kTaskCancelled: return "kTaskCancelled";
    }
    return "Unknown";
}

/**
 * @brief 有界环形事件缓冲（单生产者多消费者安全，非阻塞）。
 *
 * 依赖：`std::vector` + 原子下标；容量在构造时固定，运行期不分配。
 * 对上层提供：`Push`/`Pop` 均**不阻塞、不抛异常**；满时 `Push` 失败并累加 `dropped`。
 *
 * @note 环形缓冲固定保留一个空槽用于区分「满」与「空」，
 *       因此**实际可存容量 = 构造参数**（内部数组为 capacity+1）。
 */
class EventBuffer {
public:
    explicit EventBuffer(std::size_t capacity = 8192)
        : m_slots(capacity + 1U), m_capacity(capacity) {}

    /// @return 入队成功为 true；缓冲满时为 false（调用方应记 dropped）。
    bool Push(const SchedulerEvent& event) noexcept {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1U) % m_slots.size();
        if (next == m_tail.load(std::memory_order_acquire)) {
            m_dropped.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        m_slots[head] = event;
        m_head.store(next, std::memory_order_release);
        m_pushed.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }

    /// @return 取到事件为 true；缓冲为空为 false。
    bool Pop(SchedulerEvent* out) noexcept {
        if (out == nullptr) {
            return false;
        }
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) {
            return false;
        }
        *out = m_slots[tail];
        m_tail.store((tail + 1U) % m_slots.size(), std::memory_order_release);
        return true;
    }

    /// 只读访问从队首数起第 `index` 个事件；越界返回 false。不改变缓冲内容。
    bool PeekAt(std::size_t index, SchedulerEvent* out) const noexcept {
        if (out == nullptr) {
            return false;
        }
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        const std::size_t head = m_head.load(std::memory_order_acquire);
        const std::size_t size = head >= tail ? head - tail : m_slots.size() - tail + head;
        if (index >= size) {
            return false;
        }
        *out = m_slots[(tail + index) % m_slots.size()];
        return true;
    }

    std::size_t Capacity() const noexcept { return m_capacity; }
    std::size_t Size() const noexcept {
        const std::size_t head = m_head.load(std::memory_order_acquire);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        return head >= tail ? head - tail : m_slots.size() - tail + head;
    }
    std::uint64_t Pushed() const noexcept { return m_pushed.load(std::memory_order_relaxed); }
    std::uint64_t Dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

    void Clear() noexcept {
        m_tail.store(m_head.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::vector<SchedulerEvent> m_slots;
    std::size_t m_capacity{0};
    std::atomic<std::size_t> m_head{0};
    std::atomic<std::size_t> m_tail{0};
    std::atomic<std::uint64_t> m_pushed{0};
    std::atomic<std::uint64_t> m_dropped{0};
};

/**
 * @brief 调度事件采集器：实现 `SchedulerObserver`，把事件写入有界环形缓冲。
 *
 * 依赖：`SchedulerObserver` 的插件边界；`OnEvent` 在 **worker 线程**上同步调用。
 * 对上层提供：`Drain()` 取走事件、`Pending()`/`Captured()`/`Dropped()` 计数、
 * 以及 text/ini/json 三种序列化。
 *
 * **性能取向**：`OnEvent` 只做一次非阻塞 `Push`（内部无锁）加上一次读取
 *   `m_head` 快照供序列化使用，**不加任何互斥、不分配内存、不做 I/O**，
 *   以满足「回调必须短小且无阻塞」的约束。
 *
 * @note 采集器**不是**线程局部的。按 `perf-observer-overhead.md` §4 的建议，
 *       进一步降低多 worker 争用的做法是"每 worker 线程局部暂存 + 批量发布"；
 *       那需要 per-instance/per-thread 状态管理，本轮**未实现**，
 *       这里如实标注为后续优化，而不是声称已经做到。
 */
class DebugEventCollector final : public SchedulerObserver {
public:
    explicit DebugEventCollector(std::size_t capacity = 8192) : m_buffer(capacity) {}

    void OnEvent(const SchedulerEvent& event) override {
        // 只入队：非阻塞、不分配、不做 I/O、不抛异常。
        (void)m_buffer.Push(event);
    }

    /// 取走最多 `max` 个事件（`max == 0` 表示全取）。
    std::vector<SchedulerEvent> Drain(std::size_t max = 0) {
        std::vector<SchedulerEvent> out;
        SchedulerEvent event;
        while ((max == 0 || out.size() < max) && m_buffer.Pop(&event)) {
            out.push_back(event);
        }
        return out;
    }

    std::size_t Pending() const noexcept { return m_buffer.Size(); }
    std::size_t Capacity() const noexcept { return m_buffer.Capacity(); }
    std::uint64_t Captured() const noexcept { return m_buffer.Pushed(); }
    std::uint64_t Dropped() const noexcept { return m_buffer.Dropped(); }
    void Clear() noexcept { m_buffer.Clear(); }

    /// 只读快照（不改变缓冲内容）。
    std::vector<SchedulerEvent> Peek(std::size_t max = 0) const {
        std::vector<SchedulerEvent> out;
        const std::size_t total = m_buffer.Size();
        const std::size_t limit = (max == 0 || max > total) ? total : max;
        out.reserve(limit);
        for (std::size_t index = 0; index < limit; ++index) {
            SchedulerEvent event;
            if (!m_buffer.PeekAt(index, &event)) {
                break;
            }
            out.push_back(event);
        }
        return out;
    }

    std::string DumpText(std::size_t max = 0) const {
        const auto events = Peek(max);
        std::string out;
        out += "[events]\n";
        out += "  capacity=" + std::to_string(m_buffer.Capacity()) +
               " pending=" + std::to_string(m_buffer.Size()) +
               " captured=" + std::to_string(m_buffer.Pushed()) +
               " dropped=" + std::to_string(m_buffer.Dropped()) + "\n";
        out += "  note: only *declared* scheduler events are visible; silence != no blocking\n";
        out += "  note: steady timestamps are NOT cross-machine comparable\n\n";
        out += "     #  type                 g      m      p  state  steady_ns\n";
        std::size_t seq = 0;
        for (const auto& event : events) {
            out += "  " + std::to_string(seq++) + "  " + EventTypeName(event.type);
            out += "\t" + std::to_string(event.task_id) + "\t" +
                   std::to_string(event.machine_id) + "\t" +
                   std::to_string(event.processor_id) + "\t" +
                   std::to_string(static_cast<int>(event.task_state)) + "\t" +
                   std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      event.timestamp.time_since_epoch())
                                      .count()) +
                   "\n";
        }
        if (events.empty()) {
            out += "  (no events captured)\n";
        }
        return out;
    }

    std::string DumpIni(std::size_t max = 0) const {
        const auto events = Peek(max);
        std::string out;
        out += "[events]\n";
        out += "capacity=" + std::to_string(m_buffer.Capacity()) + "\n";
        out += "pending=" + std::to_string(m_buffer.Size()) + "\n";
        out += "captured=" + std::to_string(m_buffer.Pushed()) + "\n";
        out += "dropped=" + std::to_string(m_buffer.Dropped()) + "\n";
        out += "snapshot_consistency=per_call\n\n";
        std::size_t seq = 0;
        for (const auto& event : events) {
            out += "[event." + std::to_string(seq++) + "]\n";
            out += "type=" + std::string(EventTypeName(event.type)) + "\n";
            out += "task_id=" + std::to_string(event.task_id) + "\n";
            out += "machine_id=" + std::to_string(event.machine_id) + "\n";
            out += "processor_id=" + std::to_string(event.processor_id) + "\n";
            out += "task_state=" + std::to_string(static_cast<int>(event.task_state)) + "\n";
            out += "steady_ns=" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   event.timestamp.time_since_epoch()).count()) + "\n\n";
        }
        return out;
    }

    std::string DumpJson(std::size_t max = 0) const {
        const auto events = Peek(max);
        std::string out;
        out += "{\n";
        out += "  \"schema\": \"go2cpp.dump.events/1\",\n";
        out += "  \"snapshot_consistency\": \"per_call\",\n";
        out += "  \"cross_machine_aligned\": false,\n";
        out += "  \"capacity\": " + std::to_string(m_buffer.Capacity()) + ",\n";
        out += "  \"pending\": " + std::to_string(m_buffer.Size()) + ",\n";
        out += "  \"captured\": " + std::to_string(m_buffer.Pushed()) + ",\n";
        out += "  \"dropped\": " + std::to_string(m_buffer.Dropped()) + ",\n";
        out += "  \"events\": [";
        for (std::size_t index = 0; index < events.size(); ++index) {
            const auto& event = events[index];
            if (index != 0) {
                out += ",";
            }
            out += "\n    {\"type\": \"" + std::string(EventTypeName(event.type)) +
                   "\", \"task_id\": " + std::to_string(event.task_id) +
                   ", \"machine_id\": " + std::to_string(event.machine_id) +
                   ", \"processor_id\": " + std::to_string(event.processor_id) +
                   ", \"task_state\": " + std::to_string(static_cast<int>(event.task_state)) +
                   ", \"steady_ns\": " +
                   std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      event.timestamp.time_since_epoch()).count()) +
                   "}";
        }
        out += events.empty() ? "]\n}\n" : "\n  ]\n}\n";
        return out;
    }

private:
    EventBuffer m_buffer;
};

}  // namespace go2cpp::debug
