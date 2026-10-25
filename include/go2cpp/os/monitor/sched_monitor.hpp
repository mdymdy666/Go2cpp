#pragma once

// SchedMonitor —— 基于 eBPF sched tracepoint 的调度延迟统计。
//
// 内核侧（go2cpp/bpf/sched_latency.bpf.c）挂 sched_wakeup 与 sched_switch：
//   - 每个任务累加 on-cpu 时间与上下文切换次数；
//   - 用「sched_wakeup 时间戳 → 该任务真正被 switch-in」计算 runqueue wait，
//     写入 32 桶对数直方图与最大等待。
// 用户态计算 p50/p99/avg/max，并按等待次数取 top-N 任务。
//
// 口径限制（详见 docs/os-monitor.md）：
//   - 监控 attach 前就在运行的任务，第一次换出没有 on-cpu 起点；
//   - sched_wakeup_new 唤醒的新任务没有唤醒起点，不计入 wait；
//   - 同一任务被重复唤醒时后一次覆盖前一次，等待时间偏短。
//
// 降级与开销约定同 SyscallMonitor。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

class SchedMonitor final : public Monitor {
public:
    /** @brief @param top_n 输出到 snapshot.sched.top 的任务数量。 */
    explicit SchedMonitor(std::size_t top_n = 10);
    ~SchedMonitor() override;

    /** @brief 读取内核 map 并填充 snapshot.sched。 */
    Status UpdateOnce(Snapshot& snapshot) override;

    void Stop() override;

    bool available() const override;
    std::string reason() const override;

    std::uint64_t sample_count() const noexcept;
    void ProgramCost(std::uint64_t* run_time_ns, std::uint64_t* run_cnt) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace go2cpp::os::monitor
