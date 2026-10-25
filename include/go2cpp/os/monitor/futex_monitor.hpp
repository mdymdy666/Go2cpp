#pragma once

// FutexMonitor —— 基于 eBPF futex tracepoint 的等待时长与争用统计。
//
// 内核侧（go2cpp/bpf/futex_latency.bpf.c）挂 sys_enter_futex/sys_exit_futex：
//   - wait 类 op（FUTEX_WAIT/WAIT_BITSET/LOCK_PI/WAIT_REQUEUE_PI）：累计等待
//     时长、最大等待与 32 桶直方图；
//   - wake 类 op（FUTEX_WAKE/WAKE_BITSET/UNLOCK_PI/CMP_REQUEUE/...）：只计数，
//     与 wait 数一起给出 wait/wake 争用比。
// 用户态按 pid/comm 聚合 top-N，并计算 p50/p99/avg/max。
//
// 注意：futex 的 syscall 时长包含进入/退出开销；值不匹配时 FUTEX_WAIT 立即
// 返回 -EAGAIN，根本没有睡。内核按 sys_exit 返回值分类：只有返回 0（睡着后
// 被成功唤醒）的样本进入等待直方图与 avg/max；-EAGAIN 计 eagain_count，
// -ETIMEDOUT 计 timedout_count，其余负值计 error_count，三者都不进直方图。
// 否则高 EAGAIN 比例会把 p50/p99 拉低到亚微秒级，等待分布失真。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

class FutexMonitor final : public Monitor {
public:
    /** @brief @param top_n 输出到 snapshot.futex.top 的进程数量。 */
    explicit FutexMonitor(std::size_t top_n = 10);
    ~FutexMonitor() override;

    /** @brief 读取内核 map 并填充 snapshot.futex。 */
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
