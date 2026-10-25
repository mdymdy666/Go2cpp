#pragma once

// FileIoMonitor —— 基于 kprobe/kretprobe 的文件 IO 延迟统计。
//
// 内核侧（go2cpp/bpf/fileio_monitor.bpf.c）在 vfs_read / vfs_write 的入口与
// 返回处打点，计算单次调用的墙钟延迟（含排队、缺页、等待块设备的时间）：
//   - 全局：读/写各自的 count、total_ns、max_ns、32 桶对数直方图；
//   - 每进程：comm + 读/写 count、total_ns、max_ns（不存直方图，原因见
//     内核程序注释：BPF 栈上限 512 字节，584 字节的 per-task value 无法在
//     栈上构造）。
//
// 用户态每次 UpdateOnce 读取全局与最多 1024 条进程记录，计算读/写
// p50/p99/avg/max、IOPS（相邻两次采样的 count 差分 / 墙钟秒数）与按 IO 次数
// 排序的进程 top-N。
//
// 只统计返回值 > 0 的调用：失败/返回 0 的 IO 没有数据面延迟，计入会把 p50
// 拉低。页缓存命中通常在 <1µs 桶，缺页回读与 fsync 落盘会落到毫秒桶，
// 因此同一份直方图能同时反映快慢两类路径。
//
// pid namespace（P2）：内核侧 bpf_get_current_pid_tgid() 返回 init PID
// namespace 的 tgid，FileIoTaskStat::pid 在嵌套 pid namespace（WSL2 容器、
// Docker、unshare -p）下与 getpid()/ps 的 pid 不同（本机实测偏移 391）。
// eBPF helper bpf_get_ns_current_pid_tgid() 按 /proc/self/ns/pid 的 dev/ino
// 把 init-ns tgid 换算成采集端当前 ns 的 tgid，写入 namespace_pid，可直接
// 对照 ps；内核不支持 helper 或 ns map 未配置时该字段为 0，用户态再用
// /proc/<pid>/status 的 NSpid 做 best-effort 回退（仅当 init pid 在当前 ns
// 的 /proc 中可见）。
//
// 降级：非 root / 构建期无 libbpf / 内核 kprobe 不可用（vfs_read 或
// vfs_write 无法 attach）时 available()==false，UpdateOnce 返回 kUnsupported
// 并写 snapshot.fileio.reason。vfs_read/vfs_write 在 6.x 仍是真实符号
// （kallsyms 中为 T），WSL2 6.6.87.2 实测可 attach。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

class FileIoMonitor final : public Monitor {
public:
    /** @brief @param top_n 输出到 snapshot.fileio.top 的进程数。 */
    explicit FileIoMonitor(std::size_t top_n = 10);
    ~FileIoMonitor() override;

    /** @brief 读取内核 map 并填充 snapshot.fileio。 */
    Status UpdateOnce(Snapshot& snapshot) override;

    /** @brief 分离 kprobe 并关闭 BPF 对象；可重复调用。 */
    void Stop() override;

    bool available() const override;
    std::string reason() const override;

    /** @brief 成功读数次数（诊断用）。 */
    std::uint64_t sample_count() const noexcept;

    /** @brief 已 attach 程序的累计内核执行时间/次数（开销验证用）。 */
    void ProgramCost(std::uint64_t* run_time_ns, std::uint64_t* run_cnt) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace go2cpp::os::monitor
