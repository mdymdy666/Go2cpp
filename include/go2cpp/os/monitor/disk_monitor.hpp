#pragma once

// 磁盘采集器：解析 /proc/diskstats，按两次采样差分计算 IOPS / 吞吐 / 延迟。

#include <cstdint>
#include <map>
#include <string>

#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

class DiskMonitor final : public Monitor {
public:
    /**
     * @param diskstats_path /proc/diskstats 路径。
     * @param sys_block_root /sys/class/block 路径，用于区分整盘与分区。
     *
     * 两者仅测试注入；生产代码用默认值。sys_block_root 指向不存在的目录时
     * 自动退回按设备名判断分区（见 disk_monitor.cpp）。
     */
    explicit DiskMonitor(std::string diskstats_path = "/proc/diskstats",
                         std::string sys_block_root = "/sys/class/block");
    ~DiskMonitor() override = default;

    Status UpdateOnce(Snapshot& snapshot) override;
    void Stop() override;

    std::uint64_t sample_count() const noexcept { return sample_count_; }

private:
    struct RawCounters {
        std::uint64_t reads{0};
        std::uint64_t writes{0};
        std::uint64_t sectors_read{0};
        std::uint64_t sectors_written{0};
        std::uint64_t read_time_ms{0};
        std::uint64_t write_time_ms{0};
        std::uint64_t io_in_progress{0};
        std::uint64_t io_time_ms{0};
        std::uint64_t weighted_io_time_ms{0};
    };

    const std::string diskstats_path_;
    const std::string sys_block_root_;
    std::map<std::string, RawCounters> last_;
    /// 上一次采样时刻（CLOCK_MONOTONIC，纳秒），仅用于差分。
    std::int64_t last_sample_ns_{0};
    /// 是否已有可用的上一帧。用显式标志而不是 `last_sample_ns_ > 0`：
    /// 后者把「时间戳恰好为 0」误判成首帧，也依赖 steady_clock 的 epoch。
    bool has_prev_{false};
    std::uint64_t sample_count_{0};
};

}  // namespace go2cpp::os::monitor
