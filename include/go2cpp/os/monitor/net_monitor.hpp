#pragma once

// 网络采集器：解析 /proc/net/dev，差分出每网卡的收发包速率。

#include <cstdint>
#include <map>
#include <string>

#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

class NetMonitor final : public Monitor {
public:
    /// @param dev_path /proc/net/dev 路径；仅测试注入，生产用默认值。
    explicit NetMonitor(std::string dev_path = "/proc/net/dev");
    ~NetMonitor() override = default;

    Status UpdateOnce(Snapshot& snapshot) override;
    void Stop() override;

    std::uint64_t sample_count() const noexcept { return sample_count_; }

private:
    struct RawCounters {
        std::uint64_t rcv_bytes{0};
        std::uint64_t rcv_packets{0};
        std::uint64_t rcv_errs{0};
        std::uint64_t rcv_drops{0};
        std::uint64_t snd_bytes{0};
        std::uint64_t snd_packets{0};
        std::uint64_t snd_errs{0};
        std::uint64_t snd_drops{0};
    };

    const std::string dev_path_;
    std::map<std::string, RawCounters> last_;
    /// 上一次采样时刻（CLOCK_MONOTONIC，纳秒），仅用于差分。
    std::int64_t last_sample_ns_{0};
    /// 是否已有上一帧；理由同 DiskMonitor。
    bool has_prev_{false};
    std::uint64_t sample_count_{0};
};

}  // namespace go2cpp::os::monitor
