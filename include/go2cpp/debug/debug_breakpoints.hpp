#pragma once

/**
 * @file debug_breakpoints.hpp
 * @brief 协作式断点（P5）—— **零侵入、不碰调度器热路径**。
 *
 * ## 为什么这样设计（重要）
 *
 * 设计文档里 P5 原本设想"在调度器的安全点上下断点"。但核实后发现两条硬约束：
 *   1. 本项目**明确永不强制 detach 任意 C++ 调用栈**（cooperative-only 挂起），
 *      所以**不能在任意行**停下 G —— 只能在 G **自己愿意**停的地方停；
 *   2. `Scheduler` **没有安全点回调接口**，而 `SchedulerObserver` 是**只读**边界
 *      （`scheduler.hpp` 明确写明"不能通过事件回调控制当前 G 的执行权"）。
 *      要在调度器安全点挂钩子就必须改热路径上的调度器代码。
 *
 * 因此本模块采用**协作式**做法：由**使用者自己的代码**在它认为有意义的位置调用
 * `Checkpoint("name")`。这样：
 *   - **零侵入**：不修改 `Scheduler`/`Task`/`Fiber` 任何一行；
 *   - **不改变热路径开销**：没有调用 `Checkpoint` 的地方完全不受影响；
 *   - **符合项目原则**：G 只在**自己声明**的位置停，绝不被强行剥夺执行权。
 *
 * ## 能力边界（如实声明）
 *   - **不是**"任意行断点"，只能停在调用过 `Checkpoint` 的位置；
 *   - 未调用 `Checkpoint` 的代码路径**永远不会**命中断点；
 *   - `Checkpoint` **本身不阻塞**，它只返回命中的断点 id；是否等待由调用方决定
 *     （`WaitForResume` 才阻塞，且只应在受管 G 内使用）。
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace go2cpp::debug {

/// 断点条件。
struct BreakpointSpec {
    std::string name;             ///< 逻辑位置名（由使用者自己定义，例如 "before_commit"）。
    std::uint64_t task_id{0};     ///< 只对指定 G 生效；0 表示任意 G。
    std::size_t skip{0};          ///< 先跳过 N 次命中再触发。
    bool once{false};             ///< 触发一次后自动失效。
    bool enabled{true};           ///< 是否启用。

    bool operator==(const BreakpointSpec& other) const {
        return name == other.name && task_id == other.task_id && skip == other.skip &&
               once == other.once && enabled == other.enabled;
    }
};

/// 一次命中的记录。
struct BreakpointHit {
    std::uint64_t id{0};      ///< 断点 id（>0）。
    std::string name;         ///< 命中的位置名。
    std::uint64_t task_id{0}; ///< 触发时的 G id（0 表示调用方未提供）。
    std::uint64_t sequence{0};///< 全局递增的命中序号。
};

/**
 * @brief 协作式断点注册表。
 *
 * 依赖：仅标准库（`std::mutex` + 容器）—— 因为它可能被**任意线程**（含非受管线程）调用，
 *       所以刻意**不使用** `go2cpp::sync::Mutex`（后者需要调度器上下文）。
 * 对上层提供：注册/清除/命中判定/恢复/观测，全部线程安全。
 *
 * @note 命中判定是**幂等可重入**的：同一位置被多个 G 同时经过时各自独立计数。
 */
class DebugBreakpoints {
public:
    static DebugBreakpoints& Instance() noexcept {
        static DebugBreakpoints registry;
        return registry;
    }

    /// 注册一个断点。@return 分配的断点 id（>0）。
    std::uint64_t Add(BreakpointSpec spec) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::uint64_t id = ++m_next_id;
        Entry entry;
        entry.spec = std::move(spec);
        m_entries[id] = std::move(entry);
        return id;
    }

    /// 移除一个断点。@return 是否存在并被移除。
    bool Remove(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.erase(id) > 0;
    }

    /// 清空全部断点（**刻意不重置命中统计** —— 累计命中是跨批次的历史信息；
    /// 需要清零统计请显式调用 `ResetStats()`）。
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_waiting.clear();
    }

    /// 当前断点数量。
    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    /// 启用/停用某断点。@return 是否找到。
    bool SetEnabled(std::uint64_t id, bool enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_entries.find(id);
        if (it == m_entries.end()) {
            return false;
        }
        it->second.spec.enabled = enabled;
        return true;
    }

    /**
     * @brief 在 `Checkpoint` 中调用：判断该位置是否应命中。
     * @param name 位置名。
     * @param task_id 调用方所在 G 的 id（0 表示未知/非受管）。
     * @return 命中的断点 id（>0）；未命中返回 0。
     * @note 命中后：`skip` 递减；`once` 的断点会自动失效；命中的断点会登记到 waiting 表。
     */
    std::uint64_t ShouldStop(const std::string& name, std::uint64_t task_id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& pair : m_entries) {
            Entry& entry = pair.second;
            if (!entry.spec.enabled || entry.spec.name != name) {
                continue;
            }
            if (entry.spec.task_id != 0 && entry.spec.task_id != task_id) {
                continue;
            }
            if (entry.skipped < entry.spec.skip) {
                ++entry.skipped;
                continue;
            }
            const std::uint64_t id = pair.first;
            ++entry.hits;
            ++m_total_hits;
            if (entry.spec.once) {
                entry.spec.enabled = false;
            }
            HitRecord record;
            record.name = name;
            record.task_id = task_id;
            record.sequence = m_total_hits;
            m_waiting[id] = std::move(record);
            return id;
        }
        return 0;
    }

    /// 标记某断点已被恢复（从 waiting 表移除）。@return waiting 表里是否有它。
    bool Resume(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_waiting.erase(id) > 0;
    }

    /// 恢复全部等待中的断点。
    void ResumeAll() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_waiting.clear();
    }

    /// 当前处于"已命中未恢复"状态的断点。
    std::vector<BreakpointHit> Waiting() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<BreakpointHit> out;
        out.reserve(m_waiting.size());
        for (const auto& pair : m_waiting) {
            BreakpointHit hit;
            hit.id = pair.first;
            hit.name = pair.second.name;
            hit.task_id = pair.second.task_id;
            hit.sequence = pair.second.sequence;
            out.push_back(std::move(hit));
        }
        return out;
    }

    /**
     * @brief 重置命中统计（总命中数与每个断点的 hits/skipped）。
     * @note 只清统计，**不动**已注册的断点本身；与 `Clear()` 职责分开，
     *       这样"清空断点"与"清零计数器"两个动作不会互相牵连。
     */
    void ResetStats() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_total_hits = 0;
        for (auto& pair : m_entries) {
            pair.second.hits = 0;
            pair.second.skipped = 0;
        }
    }

    /// 累计命中次数（自上次 ResetStats 起；Clear 不会重置它）。
    std::uint64_t TotalHits() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_total_hits;
    }

    /// 某断点的命中次数。@return 未找到返回 0。
    std::uint64_t Hits(std::uint64_t id) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_entries.find(id);
        return it == m_entries.end() ? 0U : it->second.hits;
    }

    /// 观测：所有断点的 (id, spec)。
    std::vector<std::pair<std::uint64_t, BreakpointSpec>> Snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::pair<std::uint64_t, BreakpointSpec>> out;
        out.reserve(m_entries.size());
        for (const auto& pair : m_entries) {
            out.emplace_back(pair.first, pair.second.spec);
        }
        return out;
    }

    /// 序列化观测（text）。
    std::string DumpText() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string out;
        out += "[breakpoints]\n";
        out += "  count=" + std::to_string(m_entries.size()) +
               " total_hits=" + std::to_string(m_total_hits) +
               " waiting=" + std::to_string(m_waiting.size()) + "\n";
        out += "  note: cooperative only; a G stops ONLY where it calls Checkpoint()\n";
        out += "  note: arbitrary-line breakpoints are NOT supported by design\n\n";
        out += "  id  name                        task_id  skip  once  hits  enabled\n";
        for (const auto& pair : m_entries) {
            const auto& spec = pair.second.spec;
            out += "  " + std::to_string(pair.first) + "  " + spec.name +
                   "  " + std::to_string(spec.task_id) +
                   "  " + std::to_string(spec.skip) +
                   "  " + (spec.once ? "yes" : "no") +
                   "  " + std::to_string(pair.second.hits) +
                   "  " + (spec.enabled ? "yes" : "no") + "\n";
        }
        if (m_entries.empty()) {
            out += "  (no breakpoints)\n";
        }
        return out;
    }

private:
    DebugBreakpoints() = default;
    ~DebugBreakpoints() = default;
    DebugBreakpoints(const DebugBreakpoints&) = delete;
    DebugBreakpoints& operator=(const DebugBreakpoints&) = delete;

    struct Entry {
        BreakpointSpec spec;
        std::size_t skipped{0};
        std::uint64_t hits{0};
    };
    struct HitRecord {
        std::string name;
        std::uint64_t task_id{0};
        std::uint64_t sequence{0};
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::uint64_t, Entry> m_entries;
    std::unordered_map<std::uint64_t, HitRecord> m_waiting;
    std::uint64_t m_next_id{0};
    std::uint64_t m_total_hits{0};
};

/**
 * @brief 使用者在自己代码里插入的协作式检查点。
 *
 * @param name 逻辑位置名。
 * @param task_id 当前 G 的 id；不知道就传 0。
 * @return 命中的断点 id（>0）；未命中返回 0。
 *
 * @note **不阻塞**。要让 G 真的停下来等待恢复，需要调用方在拿到非 0 返回值后
 *       自行等待（例如用 `ParkingCondition`/`ConditionVariable`），
 *       或者调用 `Resume`/`ResumeAll` 让观测方知道可以继续。
 *       这样设计是为了不把"阻塞"这件事强加给调用方。
 */
inline std::uint64_t Checkpoint(const std::string& name, std::uint64_t task_id = 0) {
    return DebugBreakpoints::Instance().ShouldStop(name, task_id);
}

/// RAII 形式的检查点：析构时自动 `Resume`（异常/提前 return 也安全）。
class ScopedCheckpoint {
public:
    explicit ScopedCheckpoint(const std::string& name, std::uint64_t task_id = 0)
        : m_id(Checkpoint(name, task_id)) {}
    ~ScopedCheckpoint() {
        if (m_id != 0) {
            DebugBreakpoints::Instance().Resume(m_id);
        }
    }
    ScopedCheckpoint(const ScopedCheckpoint&) = delete;
    ScopedCheckpoint& operator=(const ScopedCheckpoint&) = delete;

    /// @return 命中的断点 id（0 表示未命中）。
    std::uint64_t id() const noexcept { return m_id; }
    bool hit() const noexcept { return m_id != 0; }

private:
    std::uint64_t m_id{0};
};

}  // namespace go2cpp::debug
