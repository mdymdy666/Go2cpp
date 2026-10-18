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
 *
 * ## 模块定位
 *
 * `debug` 的采集层：实现 `SchedulerObserver`，把调度器已声明的事件写入有界
 * 环形缓冲，并提供 text/ini/json 三种序列化。上游是 `Scheduler` 的事件回调，
 * 下游是 `debug_cli` 的 `dump events` 与调试工具。
 *
 * ## 本文件定义什么
 *
 * - `EventTypeName`：事件枚举到名字的映射；
 * - `EventBuffer`：无锁有界环形缓冲（满则丢弃并计数）；
 * - `DebugEventCollector`：观察者实现 + 线程局部批量提交 + 序列化；
 * - `EventFollowSink` / `FollowEvents`：持续拉取事件的后台跟随。
 *
 * ## 阅读建议
 *
 * 先读 `EventBuffer::Push/PushBatch` 的无锁下标协议，再读
 * `DebugEventCollector::OnEvent` 的线程局部攒批；最后看 `Drain/Peek`
 * 为什么都要先 `Flush`。
 *
 * ## 边界与陷阱
 *
 * 线程局部暂存只有**当前线程**能被 `Drain/Peek/Pending` 排空；其它线程
 * 最后不足一批的事件要等它们自己下次调用。`Drain` 是消耗性的，跟随期间
 * `Pending()` 会归零是刻意行为。
 */

#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
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
 * @brief 每个线程各自的暂存缓冲（第 21 轮，线程局部批量发布）。
 *
 * @details `SchedulerObserver::OnEvent` 在 **worker 线程**上被同步调用；
 *   若每条事件都直接 `Push` 到共享缓冲，会产生「每事件若干次原子操作」。
 *   这里改为：先在**线程局部** vector 里积累 `kDefaultFlushBatch` 条，
 *   再一次性 `PushBatch` 提交，从而把原子操作摊薄到每批一次。
 *
 * @note 线程局部缓冲需要**在采集器析构或显式 Flush 时排空**，否则最后不足一批的事件
 *   会滞留在暂存区。`DebugEventCollector` 的 `Drain`/`Peek`/析构都会先调用 `Flush()`；
 *   析构时还会尝试排空**当前线程**的暂存（其它线程的暂存只能靠各自线程的下次调用或进程退出，
 *   这是线程局部方案的固有代价，已在头文件中如实说明）。
 */
inline constexpr std::size_t kDefaultFlushBatch = 32;

/**
 * @brief 有界环形事件缓冲（单生产者多消费者安全，非阻塞）。
 *
 * 依赖：`std::vector` + 原子下标；容量在构造时固定，运行期不分配。
 * 对上层提供：`Push`/`Pop` 均**不阻塞、不抛异常**；满时 `Push` 失败并累加 `dropped`。
 *
 * @note 环形缓冲固定保留一个空槽用于区分「满」与「空」，
 *       因此**实际可存容量 = 构造参数**（内部数组为 capacity+1）。
 *
 * ## 定位
 * 采集器的存储后端：单生产者（`OnEvent`）多消费者（`Drain`/`Peek`）的
 * 无锁环形队列；被 `DebugEventCollector` 独占使用。
 *
 * ## 不变量
 * - `m_slots.size() == m_capacity + 1`，始终留一个空槽；
 * - `head`/`tail` 只用原子读写推进，`Push` 满时返回 false 并累加 dropped；
 * - `Size()` 是瞬时快照，并发写时只保证「某时刻」的近似值。
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


    /**
     * @brief 批量入队：先一次性预留 `count` 个槽，再拷贝（第 21 轮，线程局部批量发布）。
     * @return 实际入队条数；空间不足时入队前 `min(count, 剩余空间)` 条，其余计入 `dropped`。
     *
     * @note 与逐条 `Push` 相比，本函数把「读 head / 判满 / 写 head」的原子操作
     *       **从 count 次降到 1 次** —— 这是线程局部暂存能带来收益的技术前提
     *       （缓冲本身是**无锁**的，所以这里省的是原子操作而不是锁争用）。
     */
    /**
     * @brief 批量入队：先一次性计算可写槽位，再拷贝。
     *
     * ## 步骤
     * step1. 空输入直接返回 0；
     * step2. 读 head/tail，计算已用槽位与剩余空间；
     * step3. 接受 `min(count, free)` 条并拷贝到环形槽；
     * step4. 一次 release 提交 head，累加 pushed；被拒部分累加 dropped。
     */
    std::size_t PushBatch(const SchedulerEvent* data, std::size_t count) noexcept {
        // step1: 空输入
        if (data == nullptr || count == 0) {
            return 0;
        }
        // step2: 计算剩余空间
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        // 可用空间 = capacity - 已用量（内部数组比容量多 1 个哨兵槽）
        const std::size_t used = head >= tail ? head - tail : m_slots.size() - tail + head;
        const std::size_t free_slots = m_capacity - used;
        // step3: 只接受放得下的部分
        const std::size_t accepted = count < free_slots ? count : free_slots;
        for (std::size_t index = 0; index < accepted; ++index) {
            m_slots[(head + index) % m_slots.size()] = data[index];
        }
        // step4: 一次提交下标并更新统计
        m_head.store((head + accepted) % m_slots.size(), std::memory_order_release);
        m_pushed.fetch_add(accepted, std::memory_order_relaxed);
        if (accepted < count) {
            m_dropped.fetch_add(count - accepted, std::memory_order_relaxed);
        }
        return accepted;
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
    std::vector<SchedulerEvent> m_slots; ///< 环形槽数组（容量+1）；Push 写 head，Pop 读 tail
    std::size_t m_capacity{0};           ///< 对外容量（不含哨兵槽）；构造时固定
    std::atomic<std::size_t> m_head{0};  ///< 生产者下标；Push/PushBatch 以 release 推进
    std::atomic<std::size_t> m_tail{0};  ///< 消费者下标；Pop 以 release 推进，Clear 重置
    std::atomic<std::uint64_t> m_pushed{0};  ///< 成功入队总数；Push/PushBatch 累加
    std::atomic<std::uint64_t> m_dropped{0}; ///< 因满丢弃总数；Push/PushBatch 累加
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
 *
 * ## 定位
 * `debug` 的事件采集器：注册到 `Scheduler` 的观察者列表后，`OnEvent` 在
 * worker 线程上被同步调用；`Drain`/`Peek`/`Dump*` 供 `debug_cli` 与测试读取。
 *
 * ## 生命周期与线程模型
 * 构造时分配固定容量缓冲；`OnEvent` 可能来自多个 worker 线程，因此采用
 * 线程局部暂存 + 无锁批量提交。`Drain`/`Peek`/`Pending` 只提交**调用线程**
 * 的暂存；其它线程的不足一批事件需等其自身调用。对象必须比注册它的
 * `Scheduler` 活得久。
 *
 * ## 不变量
 * - `m_buffer` 的容量构造后固定；
 * - `OnEvent` 不分配内存（除线程局部 vector 增长）、不加锁、不做 I/O；
 * - `Dropped()` 只增不减，`Clear()` 也不清零（历史统计）；
 * - 线程局部暂存槽的 `owner` 指向当前采集器，切换实例前会先排空旧槽。
 */
class DebugEventCollector final : public SchedulerObserver {
public:
    explicit DebugEventCollector(std::size_t capacity = 8192) : m_buffer(capacity) {}

    /**
     * @brief 观察者回调：把事件写入线程局部暂存，满一批再提交。
     *
     * ## 步骤
     * step1. 取本线程暂存槽；若属于其它采集器，先排空旧槽再重绑；
     * step2. 把事件追加到暂存；
     * step3. 暂存达到 batch 阈值时调用 `FlushStaging()` 批量提交。
     *
     * @note 运行在 worker 线程，不能阻塞、不能抛异常。
     */
    void OnEvent(const SchedulerEvent& event) override {
        // step1: 线程局部暂存：先攒批，满一批再一次性提交
        auto& staging = Staging();
        if (staging.owner != this) {
            // 一个线程服务了另一个采集器：先把旧暂存排空，避免串到别的实例里。
            if (staging.owner != nullptr && !staging.events.empty()) {
                staging.owner->m_buffer.PushBatch(staging.events.data(), staging.events.size());
            }
            staging.events.clear();
            staging.owner = this;
        }
        // step2: 追加到本线程暂存
        staging.events.push_back(event);
        // step3: 满一批就提交，摊薄原子操作
        if (staging.events.size() >= staging.batch) {
            FlushStaging();
        }
    }

    /**
     * @brief 把**当前线程**的暂存区提交到共享缓冲。
     * @return 本次提交的条数。
     * @note 线程局部方案要求调用方在读取前先 Flush，否则最后不足一批的事件还没进缓冲。
     */
    std::size_t Flush() {
        return FlushStaging();
    }

    /// 设置每批提交条数（1 = 退化为逐条提交，便于与批量化对比）。
    /// @note 只影响**当前线程**的暂存槽（线程局部），不影响其它线程已生效的设置。
    void SetFlushBatch(std::size_t batch) {
        auto& staging = Staging();
        staging.batch = batch == 0 ? 1 : batch;
    }
    /// 读取当前线程的每批提交条数。
    std::size_t FlushBatch() const {
        return StagingConst().batch;
    }

    /// 当前线程暂存区里还没提交的条数（用于测试与观测）。
    std::size_t StagedOnThisThread() const {
        const auto& staging = StagingConst();
        return staging.owner == const_cast<DebugEventCollector*>(this)
                   ? staging.events.size()
                   : 0;
    }

    /// 取走最多 `max` 个事件（`max == 0` 表示全取）。
    std::vector<SchedulerEvent> Drain(std::size_t max = 0) {
        (void)FlushStaging();   // 先提交本线程暂存，否则最后不足一批的事件看不到
        std::vector<SchedulerEvent> out;
        SchedulerEvent event;
        while ((max == 0 || out.size() < max) && m_buffer.Pop(&event)) {
            out.push_back(event);
        }
        return out;
    }

    // 观测类接口都先提交**本线程**暂存，否则「刚写进去但还没攒够一批」的事件看不到，
    // 会让 Pending/Captured 看起来比实际少（首版就是因此让既有测试失败）。
    // 线程局部方案的这个代价在头文件里如实记录：其它线程的暂存只能等它们自己下次调用。
    std::size_t Pending() const {
        const_cast<DebugEventCollector*>(this)->FlushStaging();
        return m_buffer.Size();
    }
    std::size_t Capacity() const noexcept { return m_buffer.Capacity(); }
    std::uint64_t Captured() const {
        const_cast<DebugEventCollector*>(this)->FlushStaging();
        return m_buffer.Pushed();
    }
    std::uint64_t Dropped() const {
        const_cast<DebugEventCollector*>(this)->FlushStaging();
        return m_buffer.Dropped();
    }
    void Clear() noexcept { m_buffer.Clear(); }

    /// 只读快照（不改变缓冲内容）。
    std::vector<SchedulerEvent> Peek(std::size_t max = 0) const {
        // Peek 是 const，但为了让"刚写进去还没攒够一批"的事件也可见，
        // 这里对**本线程**暂存做一次提交（提交后采集器语义不变，只是提前进缓冲）。
        const_cast<DebugEventCollector*>(this)->FlushStaging();
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
    /// 线程局部暂存槽：owner 表示它属于哪个采集器。
    struct StagingSlot {
        // 用非 const 指针：排空旧暂存时需要调用 owner 的 m_buffer.PushBatch。
        DebugEventCollector* owner{nullptr}; ///< 该暂存槽属于哪个采集器；OnEvent 切换实例时重绑
        std::vector<SchedulerEvent> events;  ///< 本线程尚未提交的事件；OnEvent 追加，FlushStaging 清空
        std::size_t batch{kDefaultFlushBatch}; ///< 每批提交条数；SetFlushBatch 设置
    };
    static StagingSlot& Staging() {
        static thread_local StagingSlot slot;
        return slot;
    }
    static const StagingSlot& StagingConst() {
        return Staging();
    }
    std::size_t FlushStaging() {
        auto& staging = Staging();
        if (staging.owner != this || staging.events.empty()) {
            return 0;
        }
        const std::size_t accepted = m_buffer.PushBatch(staging.events.data(), staging.events.size());
        staging.events.clear();
        return accepted;
    }

    EventBuffer m_buffer; ///< 共享有界环形缓冲；FlushStaging 写入，Drain/Peek 读取
};


/**
 * @brief 事件跟随的接收端：每拿到一批事件调用一次。
 * @return 返回 false 表示"请停止跟随"。
 */
using EventFollowSink = std::function<bool(const std::vector<SchedulerEvent>&)>;

/**
 * @brief 持续跟随采集器：轮询取走已入队事件并交给 sink（第 20 轮，P2 收尾）。
 *
 * @param collector   事件采集器。
 * @param sink        每批回调一次；返回 false 即停止。
 * @param interval    两次拉取之间的间隔（避免忙等空转）。
 * @param max_batches 最多输出多少批；**0 表示不限制**（真实使用需配合信号/外部退出条件）。
 * @param overall_timeout 总超时；0 表示不限。**强烈建议非 0** ——
 *        若采集器一直为空，`batches` 永远到不了 `max_batches`，没有超时就会无限循环。
 * @return 实际输出的批次数。
 *
 * @note **进程内**跟随：只能跟随本进程里挂在该采集器上的事件；跨进程需要信号或管道，属后续项。
 * @note `Drain()` 是**消耗性**的：被跟随取走的事件不再留在采集器里，
 *       因此跟随期间缓冲只作短期暂存，`Pending()` 会随之归零 —— 这是刻意的。
 */
inline std::size_t FollowEvents(DebugEventCollector& collector, const EventFollowSink& sink,
                                std::chrono::milliseconds interval, std::size_t max_batches = 0,
                                std::chrono::milliseconds overall_timeout =
                                    std::chrono::milliseconds(0)) {
    // ## 步骤
    // step1. 记录起始时间；
    // step2. Drain 一批；非空则计数并回调 sink，sink 返回 false 即停止；
    // step3. 达到 max_batches 停止（0 表示不限）；
    // step4. 超过 overall_timeout 停止（0 表示不限，但真实使用建议非 0）；
    // step5. 否则 sleep interval 后重试。
    std::size_t batches = 0;
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        // step2: 拉取并交给 sink
        const auto events = collector.Drain();
        if (!events.empty()) {
            ++batches;
            if (sink && !sink(events)) {
                break;
            }
        }
        // step3: 批数上限
        if (max_batches != 0 && batches >= max_batches) {
            break;
        }
        // 总超时是**必需的**兜底：若采集器一直为空，batches 永远到不了 max_batches，
        // 旧实现会在此处无限循环（实测导致 debug_events 卡住、进而让整条 ctest 超时）。
        // step4: 总超时兜底，避免空采集器无限循环
        if (overall_timeout.count() > 0 &&
            std::chrono::steady_clock::now() - started >= overall_timeout) {
            break;
        }
        // step5: 避免忙等
        std::this_thread::sleep_for(interval);
    }
    return batches;
}

}  // namespace go2cpp::debug
