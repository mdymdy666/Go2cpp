#pragma once

// 内存采集器：解析 /proc/meminfo。

#include <string>

#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

class MemMonitor final : public Monitor {
public:
    /// @param meminfo_path /proc/meminfo 路径；仅测试注入，生产用默认值。
    explicit MemMonitor(std::string meminfo_path = "/proc/meminfo");
    ~MemMonitor() override = default;

    Status UpdateOnce(Snapshot& snapshot) override;
    void Stop() override;

    std::uint64_t sample_count() const noexcept { return sample_count_; }

private:
    const std::string meminfo_path_;
    std::uint64_t sample_count_{0};
};

}  // namespace go2cpp::os::monitor
