#pragma once

// SyscallMonitor —— 基于 eBPF raw_syscalls tracepoint 的系统调用延迟统计。
//
// 内核侧（go2cpp/bpf/syscall_latency.bpf.c）在 sys_enter 记录起点、sys_exit
// 计算 delta，按 syscall id 累计 count/total_ns/max_ns 与 32 桶对数直方图；
// 用户态每次 UpdateOnce 读取整张 HASH map，计算 p50/p99/avg/max 并按调用
// 次数取 top-N。统计范围是**全系统所有进程**，不只是当前进程。
//
// 降级：非 root、内核缺 tracepoint 或构建期未启用 libbpf 时
// available()==false，reason() 给出原因，UpdateOnce 返回 kUnsupported 并写
// snapshot.syscalls.reason，不影响其它采集器。
//
// 开销：enter 路径 1 次 map update，exit 路径 1 次 lookup + 1 次 delete +
// 最多 4 次原子加。ProgramCost() 可读出内核记账的真实执行时间。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

class SyscallMonitor final : public Monitor {
public:
    /** @brief @param top_n 输出到 snapshot.syscalls.top 的热点 syscall 数量。 */
    explicit SyscallMonitor(std::size_t top_n = 10);
    ~SyscallMonitor() override;

    /** @brief 读取内核 map 并填充 snapshot.syscalls。 */
    Status UpdateOnce(Snapshot& snapshot) override;

    /** @brief 分离 tracepoint 并关闭 BPF 对象；可重复调用。 */
    void Stop() override;

    bool available() const override;
    std::string reason() const override;

    /** @brief 成功读数次数（诊断用）。 */
    std::uint64_t sample_count() const noexcept;

    /** @brief 已 attach 的 BPF 程序累计内核执行时间/次数（开销验证用）。 */
    void ProgramCost(std::uint64_t* run_time_ns, std::uint64_t* run_cnt) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace go2cpp::os::monitor
