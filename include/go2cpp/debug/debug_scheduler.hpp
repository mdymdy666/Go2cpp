#pragma once

/**
 * @file debug_scheduler.hpp
 * @brief 调度器调试代理与序列化（P0/P1）。
 *
 * 设计依据：
 *   - DebugTooling/design/04-encoding-roadmap.md（五层架构：采集/快照/序列化）
 *   - DebugTooling/design/09-debug-class-conventions.md §2.4（代理方案）
 *
 * 为什么用**代理**而不是继承 `Scheduler`：
 *   `Scheduler` 虽无 `final`，但调试侧要包装的是**框架已创建的对象**
 *   （用户代码自己 new 的 `Scheduler`），继承无法替换它，且 `Scheduler`
 *   非虚析构（实测 `is_polymorphic = 0`），继承反而有误删风险。
 *   因此这里是：`DebugScheduler` 组合持有 `Scheduler&` + 只读查询。
 *
 * **零侵入**：本文件只使用 `Scheduler` 已有的 public 接口
 *   （`processor_count()`/`worker_count()`/`runnable_count()`/`global_runnable_count()`/
 *    `is_running()`/`sysmon_running()`/`sysmon_pass_count()`/`metrics()`/
 *    `processors()`/`machines()`），不修改框架任何一行。
 *
 * 双时间戳：使用者已定案 —— 记录时刻同时取 `steady_clock` 与 `system_clock`，
 *   并**明确声明不做跨机绝对对齐**（`steady` 跨机不可比）。
 *
 * ## 模块定位
 *
 * `debug` 的调度器快照/序列化层：通过组合持有 `Scheduler&` 读取公开计数与
 * 快照，不修改调度器热路径。上游是 `debug_cli` 与信号 dump，下游只依赖
 * `Scheduler` 的 public 查询接口。
 *
 * ## 本文件定义什么
 *
 * - `DumpFormat` / `DumpOptions`：输出格式与字段裁剪；
 * - `SchedulerDebugSnapshot`：一次采集的纯数据快照；
 * - `DebugScheduler`：采集 + text/ini/json 三种序列化。
 *
 * ## 阅读建议
 *
 * 先读 `SchedulerDebugSnapshot` 的双时间戳与 `per_call_consistent` 含义，
 * 再看 `DebugScheduler::Snapshot`/`DumpText` 如何如实标注「非全局原子」。
 *
 * ## 边界与陷阱
 *
 * 快照由多次独立读取拼成，**不是同一瞬间的全景**；`steady_at` 只能同机
 * 比较；`include_fibers` 需要遍历 16 个分片，成本明显更高，默认关闭。
 */

#include "go2cpp/scheduler.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace go2cpp::debug {

// 短别名：scheduler.hpp 在 go2cpp 顶层提供的别名未覆盖这三个类型
// （只有 Machine/Processor），因此这里显式引入，避免全名噪音。
using SchedulerMetrics = scheduler::SchedulerMetrics;
using ProcessorSnapshot = scheduler::ProcessorSnapshot;
using MachineSnapshot = scheduler::MachineSnapshot;
using TaskSnapshot = scheduler::TaskSnapshot;

/// 序列化格式。
enum class DumpFormat {
    kText,  ///< 人类可读。
    kIni,   ///< ini 节结构。
    kJson,  ///< JSON（字段名与 design/02 的 schema 一致）。
};

/**
 * @brief dump 选项（对应 design/02 的 CLI 选项，本轮支持子集）。
 *
 * ## 定位
 * `DebugScheduler` 与 `debug_cli` 共用的字段裁剪配置；只影响输出，不影响采集。
 */
struct DumpOptions {
    bool include_processors{true}; ///< 是否输出 P 列表
    bool include_machines{true};   ///< 是否输出 M 列表
    bool include_metrics{true};    ///< 是否输出 metrics 段
    bool include_fibers{false};  ///< 是否枚举全部 G；需“枚举全部 G”接口，成本较高。
    std::size_t event_limit{0};  ///< dump events 的条数上限；0 表示全部。
};

/**
 * @brief 调度器调试快照（纯数据，可自由拷贝）。
 *
 * ## 定位
 * `DebugScheduler::Snapshot()` 的返回类型；供序列化与测试断言使用。
 *
 * ## 不变量
 * - 所有字段都是某一时刻的独立读取，不保证彼此严格一致；
 * - `tasks` 仅在 `include_fibers` 时填充，否则为空；
 * - 双时间戳同时存在，但 `steady_at` 不可跨机比较。
 */
struct SchedulerDebugSnapshot {
    std::size_t processor_count{0}; ///< P 数量；Snapshot() 读取
    std::size_t worker_count{0};    ///< 当前 worker 线程数
    std::size_t runnable_total{0};  ///< 全部可运行 G 数
    std::size_t runnable_global{0}; ///< 全局队列中的可运行 G 数
    bool running{false};            ///< 调度器是否运行中
    bool sysmon_running{false};     ///< sysmon 是否运行
    std::uint64_t sysmon_passes{0}; ///< sysmon 轮询次数
    SchedulerMetrics metrics{};     ///< 调度器指标快照
    std::vector<ProcessorSnapshot> processors; ///< P 快照；include_processors 时输出
    std::vector<MachineSnapshot> machines;     ///< M 快照；include_machines 时输出
    /// 全部仍在注册表里的 G（需 include_fibers 才填充，否则为空）。
    std::vector<TaskSnapshot> tasks;

    /// 双时间戳（使用者定案）。`steady` 仅用于**同机**排序与耗时，
    /// **不可跨机比较**；`wall` 用于跨机粗对齐。输出中必须标注此限制。
    std::chrono::steady_clock::time_point steady_at{}; ///< 单调时钟采集时刻；仅同机比较
    std::chrono::system_clock::time_point wall_at{};   ///< 墙上时钟采集时刻；用于跨机粗对齐

    /// 快照一致性声明：本快照由**多次独立读取**拼成，不是同一瞬间的全景。
    bool per_call_consistent{false};
};

/**
 * @brief 调度器调试代理。
 *
 * 依赖：调用方持有并保证 `Scheduler` 的生命周期长于本代理。
 * 对上层提供：一致读取的快照、以及 text/ini/json 三种序列化。
 *
 * ## 定位
 * `debug` 的调度器观测入口：组合而不是继承 `Scheduler`（后者无虚析构），
 * 供 `debug_cli`、信号 dump、测试读取调度器状态。
 *
 * ## 生命周期与线程模型
 * 只保存 `Scheduler*`，不拥有它；代理可在任意线程构造/调用，但快照读取
 * 的是调度器自身的并发安全接口。代理本身无状态、可拷贝。
 *
 * ## 不变量
 * - `m_scheduler == nullptr` 时所有采集返回默认快照；
 * - `Snapshot()` 绝不修改调度器，只调用 const/查询接口；
 * - 输出中始终带 `per_call_consistent=false` 与时钟不可比声明。
 */
class DebugScheduler final {
public:
    explicit DebugScheduler(Scheduler& scheduler) noexcept : m_scheduler(&scheduler) {}
    explicit DebugScheduler(Scheduler* scheduler) noexcept : m_scheduler(scheduler) {}

    /// 采集一份快照。
    /// @note 由多次独立读取拼成；`per_call_consistent` 为 false 正是要**如实标注**
    ///       它不是同一瞬间的全景（避免使用者误读）。
    /**
     * @brief 采集一份调度器快照。
     *
     * ## 步骤
     * step1. 空指针返回默认快照；
     * step2. 同时记录 steady/wall 两个时间戳；
     * step3. 逐项读取计数、metrics、P/M 列表（多次独立读取）；
     * step4. 标记 `per_call_consistent=false`，如实声明非全局原子。
     */
    SchedulerDebugSnapshot Snapshot() const {
        // step1: 未绑定调度器
        SchedulerDebugSnapshot snapshot;
        if (m_scheduler == nullptr) {
            return snapshot;
        }
        // step2: 双时间戳
        snapshot.steady_at = std::chrono::steady_clock::now();
        snapshot.wall_at = std::chrono::system_clock::now();
        snapshot.processor_count = m_scheduler->processor_count();
        snapshot.worker_count = m_scheduler->worker_count();
        snapshot.runnable_total = m_scheduler->runnable_count();
        snapshot.runnable_global = m_scheduler->global_runnable_count();
        snapshot.running = m_scheduler->is_running();
        snapshot.sysmon_running = m_scheduler->sysmon_running();
        snapshot.sysmon_passes = m_scheduler->sysmon_pass_count();
        // step3: 多次独立读取，拼成非原子快照
        snapshot.metrics = m_scheduler->metrics();
        snapshot.processors = m_scheduler->processors();
        snapshot.machines = m_scheduler->machines();
        // 枚举全部 G 需要遍历 16 个分片，成本比其它快照高，
        // 因此只在调用方明确要求时采集（见 DumpOptions::include_fibers）。
        // step4: 明确标注一致性边界
        snapshot.per_call_consistent = false;
        return snapshot;
    }

    /// 采集快照，并按 options 决定是否枚举全部 G。
    SchedulerDebugSnapshot SnapshotWith(const DumpOptions& options) const {
        SchedulerDebugSnapshot snapshot = Snapshot();
        if (options.include_fibers && m_scheduler != nullptr) {
            snapshot.tasks = m_scheduler->DebugSnapshotTasks();
        }
        return snapshot;
    }

    /// 按格式导出。
    std::string Dump(DumpFormat format, const DumpOptions& options = {}) const {
        switch (format) {
            case DumpFormat::kIni: return DumpIni(options);
            case DumpFormat::kJson: return DumpJson(options);
            case DumpFormat::kText: break;
        }
        return DumpText(options);
    }
    std::string Dump(const DumpOptions& options = {}) const { return DumpText(options); }

    /// 文本格式（人类可读，缺信息如实标注）。
    /**
     * @brief 文本格式（人类可读，缺信息如实标注）。
     *
     * ## 步骤
     * step1. 采集快照（可能含 G 列表）；
     * step2. 输出调度器摘要、sysmon、双时钟与一致性声明；
     * step3. 按 options 依次输出 metrics、P、M、G 段；
     * step4. 对全零 metrics、long_syscalls 等易误读字段加显式 note。
     */
    std::string DumpText(const DumpOptions& options = {}) const {
        const auto s = SnapshotWith(options);
        std::string out;
        out += "[scheduler]\n";
        out += "  processors=" + std::to_string(s.processor_count) +
               " workers=" + std::to_string(s.worker_count) +
               " runnable=" + std::to_string(s.runnable_total) +
               " (global " + std::to_string(s.runnable_global) + ")" +
               " running=" + (s.running ? "true" : "false") + "\n";
        out += "  sysmon: running=" + std::string(s.sysmon_running ? "true" : "false") +
               " passes=" + std::to_string(s.sysmon_passes) + "\n";
        out += "  snapshot: per-call consistent, NOT globally atomic\n";
        out += "  clocks: steady_ns=" +
               std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  s.steady_at.time_since_epoch())
                                  .count()) +
               " wall_unix_ns=" +
               std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  s.wall_at.time_since_epoch())
                                  .count()) +
               "  (steady is NOT cross-machine comparable)\n";
        // step3: 可选段：metrics
        if (options.include_metrics) {
            out += "  metrics: runs=" + std::to_string(s.metrics.task_runs) +
                   " completions=" + std::to_string(s.metrics.task_completions) +
                   " resume_ns=" + std::to_string(s.metrics.fiber_resume_ns) +
                   " local=" + std::to_string(s.metrics.local_queue_pops) +
                   " steal=" + std::to_string(s.metrics.steal_pops);
            if (s.metrics.task_runs == 0) {
                out += "   [note: all-zero may mean collect_metrics=false, not 'no tasks']";
            }
            out += "\n";
        }
        // step3: 可选段：P 列表
        if (options.include_processors) {
            out += "\nprocessor  state      queued\n";
            for (const auto& p : s.processors) {
                out += "  " + Pad(std::to_string(p.id).c_str(), 9) +
                       Pad(ProcessorStateName(p.state), 11) +
                       std::to_string(p.queued) + "\n";
            }
        }
        // step3: 可选段：M 列表
        if (options.include_machines) {
            out += "\nmachine  state      P  detached  blocking  long_syscalls  affinity(h/m)\n";
            for (const auto& m : s.machines) {
                out += "  " + Pad(std::to_string(m.id).c_str(), 7) +
                       Pad(MachineStateName(m.state), 11) +
                       std::to_string(m.processor) + "  " +
                       std::string(m.processor_detached ? "true " : "false") + "     " +
                       std::to_string(m.blocking_task) + "         " +
                       std::to_string(m.long_syscall_count) + "              " +
                       std::to_string(m.affinity_hits) + "/" +
                       std::to_string(m.affinity_misses) + "\n";
            }
            out += "(long_syscalls is a COUNT of threshold-exceeding blocks, not a duration)\n";
        }
        if (options.include_fibers) {
            out += "\nfibers (all non-terminated G in registries): " +
                   std::to_string(s.tasks.size()) + "\n";
            out += "     g      state      queued started  class  last_p  flags\n";
            for (const auto& t : s.tasks) {
                out += "  " + Pad(std::to_string(t.id).c_str(), 7) +
                       Pad(GStateName(t.state), 11) +
                       Pad(t.queued ? "yes" : "no", 7) +
                       Pad(t.started ? "yes" : "no", 8) +
                       Pad(std::to_string(t.task_class).c_str(), 7) +
                       Pad(std::to_string(t.last_processor_id).c_str(), 8);
                if (t.failed) out += "failed ";
                if (t.cancellation_requested) out += "cancel-req ";
                out += "\n";
            }
            if (s.tasks.empty()) {
                out += "  (no live G)\n";
            }
        }
        return out;
    }

    /// ini 节结构（字段名与 design/02 的 schema 一致）。
    std::string DumpIni(const DumpOptions& options = {}) const {
        const auto s = SnapshotWith(options);
        std::string out;
        out += "[scheduler]\n";
        out += "processor_count=" + std::to_string(s.processor_count) + "\n";
        out += "worker_count=" + std::to_string(s.worker_count) + "\n";
        out += "runnable_total=" + std::to_string(s.runnable_total) + "\n";
        out += "runnable_global=" + std::to_string(s.runnable_global) + "\n";
        out += "is_running=" + std::string(s.running ? "true" : "false") + "\n";
        out += "sysmon_running=" + std::string(s.sysmon_running ? "true" : "false") + "\n";
        out += "sysmon_passes=" + std::to_string(s.sysmon_passes) + "\n";
        out += "steady_ns=" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               s.steady_at.time_since_epoch()).count()) + "\n";
        out += "wall_unix_ns=" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  s.wall_at.time_since_epoch()).count()) + "\n";
        out += "snapshot_consistency=per_call\n\n";
        if (options.include_metrics) {
            out += "[scheduler.metrics]\n";
            out += "task_runs=" + std::to_string(s.metrics.task_runs) + "\n";
            out += "task_completions=" + std::to_string(s.metrics.task_completions) + "\n";
            out += "fiber_resume_ns=" + std::to_string(s.metrics.fiber_resume_ns) + "\n";
            out += "local_queue_pops=" + std::to_string(s.metrics.local_queue_pops) + "\n";
            out += "steal_pops=" + std::to_string(s.metrics.steal_pops) + "\n\n";
        }
        if (options.include_processors) {
            for (const auto& p : s.processors) {
                out += "[processor." + std::to_string(p.id) + "]\n";
                out += "id=" + std::to_string(p.id) + "\n";
                out += std::string("state=") + ProcessorStateName(p.state) + "\n";
                out += "queued=" + std::to_string(p.queued) + "\n\n";
            }
        }
        if (options.include_machines) {
            for (const auto& m : s.machines) {
                out += "[machine." + std::to_string(m.id) + "]\n";
                out += "id=" + std::to_string(m.id) + "\n";
                out += std::string("state=") + MachineStateName(m.state) + "\n";
                out += "processor=" + std::to_string(m.processor) + "\n";
                out += "last_task_class=" + std::to_string(m.last_task_class) + "\n";
                out += "affinity_hits=" + std::to_string(m.affinity_hits) + "\n";
                out += "affinity_misses=" + std::to_string(m.affinity_misses) + "\n";
                out += "processor_detached=" + std::string(m.processor_detached ? "true" : "false") + "\n";
                out += "blocking_task=" + std::to_string(m.blocking_task) + "\n";
                out += "long_syscall_count=" + std::to_string(m.long_syscall_count) + "\n\n";
            }
        }
        if (options.include_fibers) {
            for (const auto& t : s.tasks) {
                out += "[g." + std::to_string(t.id) + "]\n";
                out += "state=" + std::string(GStateName(t.state)) + "\n";
                out += "queued=" + std::string(t.queued ? "true" : "false") + "\n";
                out += "started=" + std::string(t.started ? "true" : "false") + "\n";
                out += "task_class=" + std::to_string(t.task_class) + "\n";
                out += "last_processor_id=" + std::to_string(t.last_processor_id) + "\n";
                out += "failed=" + std::string(t.failed ? "true" : "false") + "\n";
                out += "cancellation_requested=" +
                       std::string(t.cancellation_requested ? "true" : "false") + "\n\n";
            }
        }
        return out;
    }

    /// JSON（字段名与 design/02 §3 的 schema 对齐）。
    std::string DumpJson(const DumpOptions& options = {}) const {
        const auto s = SnapshotWith(options);
        std::string out;
        out += "{\n";
        out += "  \"schema\": \"go2cpp.dump.scheduler/1\",\n";
        out += "  \"snapshot_consistency\": \"per_call\",\n";
        out += "  \"clocks\": {\"steady_ns\": " +
               std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  s.steady_at.time_since_epoch()).count()) +
               ", \"wall_unix_ns\": " +
               std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  s.wall_at.time_since_epoch()).count()) +
               ", \"cross_machine_aligned\": false},\n";
        out += "  \"scheduler\": {\n";
        out += "    \"processor_count\": " + std::to_string(s.processor_count) + ",\n";
        out += "    \"worker_count\": " + std::to_string(s.worker_count) + ",\n";
        out += "    \"runnable_total\": " + std::to_string(s.runnable_total) + ",\n";
        out += "    \"runnable_global\": " + std::to_string(s.runnable_global) + ",\n";
        out += "    \"is_running\": " + std::string(s.running ? "true" : "false") + ",\n";
        out += "    \"sysmon\": {\"running\": " + std::string(s.sysmon_running ? "true" : "false") +
               ", \"passes\": " + std::to_string(s.sysmon_passes) + "}";
        if (options.include_metrics) {
            out += ",\n    \"metrics\": {\"task_runs\": " + std::to_string(s.metrics.task_runs) +
                   ", \"task_completions\": " + std::to_string(s.metrics.task_completions) +
                   ", \"fiber_resume_ns\": " + std::to_string(s.metrics.fiber_resume_ns) +
                   ", \"local_queue_pops\": " + std::to_string(s.metrics.local_queue_pops) +
                   ", \"steal_pops\": " + std::to_string(s.metrics.steal_pops) + "}";
        }
        out += "\n  }";
        if (options.include_processors) {
            out += ",\n  \"processors\": [";
            for (std::size_t i = 0; i < s.processors.size(); ++i) {
                const auto& p = s.processors[i];
                if (i != 0) out += ", ";
                out += "{\"id\": " + std::to_string(p.id) + ", \"state\": \"" +
                       ProcessorStateName(p.state) + "\", \"queued\": " + std::to_string(p.queued) + "}";
            }
            out += "]";
        }
        if (options.include_machines) {
            out += ",\n  \"machines\": [";
            for (std::size_t i = 0; i < s.machines.size(); ++i) {
                const auto& m = s.machines[i];
                if (i != 0) out += ", ";
                out += "{\"id\": " + std::to_string(m.id) + ", \"state\": \"" +
                       MachineStateName(m.state) + "\", \"processor\": " + std::to_string(m.processor) +
                       ", \"last_task_class\": " + std::to_string(m.last_task_class) +
                       ", \"affinity_hits\": " + std::to_string(m.affinity_hits) +
                       ", \"affinity_misses\": " + std::to_string(m.affinity_misses) +
                       ", \"processor_detached\": " + (m.processor_detached ? "true" : "false") +
                       ", \"blocking_task\": " + std::to_string(m.blocking_task) +
                       ", \"long_syscall_count\": " + std::to_string(m.long_syscall_count) + "}";
            }
            out += "]";
        }
        if (options.include_fibers) {
            out += ",\n  \"fibers\": [";
            for (std::size_t i = 0; i < s.tasks.size(); ++i) {
                const auto& t = s.tasks[i];
                if (i != 0) out += ",";
                out += "\n    {\"g\": " + std::to_string(t.id) +
                       ", \"state\": \"" + std::string(GStateName(t.state)) +
                       "\", \"queued\": " + (t.queued ? "true" : "false") +
                       ", \"started\": " + (t.started ? "true" : "false") +
                       ", \"task_class\": " + std::to_string(t.task_class) +
                       ", \"last_processor_id\": " + std::to_string(t.last_processor_id) +
                       ", \"failed\": " + (t.failed ? "true" : "false") +
                       ", \"cancellation_requested\": " +
                       (t.cancellation_requested ? "true" : "false") + "}";
            }
            out += s.tasks.empty() ? "]\n}\n" : "\n  ]\n}\n";
        } else {
            out += ",\n  \"fibers\": null\n}\n";
        }
        return out;
        return out;
    }

private:
    static std::string Pad(const char* text, std::size_t width) {
        std::string out = (text != nullptr) ? text : "";
        while (out.size() < width) {
            out.push_back(' ');
        }
        return out;
    }

    static const char* GStateName(GState state) noexcept {
        switch (state) {
            case GState::kNew: return "New";
            case GState::kRunnable: return "Runnable";
            case GState::kRunning: return "Running";
            case GState::kWaiting: return "Waiting";
            case GState::kDead: return "Dead";
            case GState::kCancelled: return "Cancelled";
            case GState::kFailed: return "Failed";
        }
        return "Unknown";
    }

    static const char* ProcessorStateName(PState state) noexcept {
        switch (state) {
            case PState::kIdle: return "Idle";
            case PState::kRunning: return "Running";
            case PState::kStopping: return "Stopping";
            case PState::kDead: return "Dead";
        }
        return "Unknown";
    }
    static const char* MachineStateName(MState state) noexcept {
        switch (state) {
            case MState::kIdle: return "Idle";
            case MState::kRunning: return "Running";
            case MState::kBlocking: return "Blocking";
            case MState::kParked: return "Parked";
            case MState::kStopping: return "Stopping";
            case MState::kDead: return "Dead";
        }
        return "Unknown";
    }

    Scheduler* m_scheduler{nullptr}; ///< 被观测的调度器（不拥有）；构造时绑定，所有查询只读
};

}  // namespace go2cpp::debug
