#pragma once

// CPU 采集器：解析 /proc/stat 与 /proc/loadavg。
//
// 参考实现依赖 /dev/cpu_stat_monitor 内核模块；这里直接用 /proc/stat 的
// 累计 jiffies，两个采样点之间的差分即使用率，无需内核模块。

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

class CpuMonitor final : public Monitor {
public:
    /**
     * @param stat_path    /proc/stat 路径。
     * @param loadavg_path /proc/loadavg 路径。
     *
     * 两个路径只用于测试注入伪造内容；生产代码用默认值。默认实参保证
     * 现有 `CpuMonitor monitor;` 调用不变。
     */
    explicit CpuMonitor(std::string stat_path = "/proc/stat",
                        std::string loadavg_path = "/proc/loadavg");
    ~CpuMonitor() override = default;

    /** @brief 读取 /proc/stat、/proc/loadavg，计算相对上次采样的占比。 */
    Status UpdateOnce(Snapshot& snapshot) override;

    void Stop() override;

    /** @brief 已成功采样的次数（用于测试与诊断）。 */
    std::uint64_t sample_count() const noexcept { return sample_count_; }

private:
    struct RawTimes {
        std::uint64_t user{0};
        std::uint64_t nice{0};
        std::uint64_t system{0};
        std::uint64_t idle{0};
        std::uint64_t iowait{0};
        std::uint64_t irq{0};
        std::uint64_t softirq{0};
        std::uint64_t steal{0};
        std::uint64_t guest{0};
        std::uint64_t guest_nice{0};
    };

    const std::string stat_path_;
    const std::string loadavg_path_;
    /// 上一次采样的每核累计值；key 为 "cpu" / "cpuN"。
    std::map<std::string, RawTimes> last_;
    std::uint64_t sample_count_{0};
};

}  // namespace go2cpp::os::monitor
