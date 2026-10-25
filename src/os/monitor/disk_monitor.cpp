#include "go2cpp/os/monitor/disk_monitor.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace go2cpp::os::monitor {
namespace {

/// 512 字节扇区是 /proc/diskstats 的固定语义。
constexpr double kSectorBytes = 512.0;

bool StartsWith(const std::string& name, const char* prefix) {
    return name.rfind(prefix, 0) == 0;
}

bool AllDigits(const std::string& text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) {
               return std::isdigit(c) != 0;
           });
}

/// 过滤 loop / ram / dm- / sr 等虚拟或只读设备，避免与底层物理盘重复统计。
bool IsVirtualDevice(const std::string& name) {
    return StartsWith(name, "loop") || StartsWith(name, "ram") ||
           StartsWith(name, "dm-") || StartsWith(name, "sr") ||
           StartsWith(name, "zram");
}

/// 没有 sysfs 可查时的设备名启发式（仅在 sysfs 查不到该设备时兜底）。
///
/// 分区名规则随驱动变化：sdX/vdX 是「整盘名 + 数字」，nvme/mmcblk/nbd 是
/// 「整盘名 + p + 数字」，而 nvme0n1、mmcblk0、nbd0 本身以数字结尾。
/// 因此对后三者只在最后一个 'p' 之后全为数字时才判为分区。
bool LooksLikePartition(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    if (StartsWith(name, "nvme") || StartsWith(name, "mmcblk") ||
        StartsWith(name, "nbd")) {
        const auto p = name.rfind('p');
        return p != std::string::npos && p + 1 < name.size() &&
               AllDigits(name.substr(p + 1));
    }
    return std::isdigit(static_cast<unsigned char>(name.back())) != 0;
}

/// 判断是否为分区。优先用 sysfs 的 partition 属性（权威）；sysfs 不可用
/// （容器未挂 /sys，或测试注入的虚构设备）时退回名字启发式。
bool IsPartition(const std::string& name, const std::string& sys_block_root) {
    if (!sys_block_root.empty()) {
        const std::string device_dir = sys_block_root + "/" + name;
        struct stat info {};
        if (::stat((device_dir + "/partition").c_str(), &info) == 0) {
            return true;
        }
        if (::stat(device_dir.c_str(), &info) == 0) {
            return false;  // 设备存在且没有 partition 属性 → 整盘
        }
    }
    return LooksLikePartition(name);
}

/// 差分用的时间戳：必须是单调时钟。system_clock 会被 NTP 校时、手动改
/// 时间或休眠恢复跳变，导致单窗速率被放大/归零；墙上时间只用于
/// Snapshot.timestamp_ms 展示。
std::int64_t NowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

DiskMonitor::DiskMonitor(std::string diskstats_path,
                         std::string sys_block_root)
    : diskstats_path_(std::move(diskstats_path)),
      sys_block_root_(std::move(sys_block_root)) {}

Status DiskMonitor::UpdateOnce(Snapshot& snapshot) {
    std::ifstream in(diskstats_path_);
    if (!in) {
        return Status::kError;
    }

    const std::int64_t now_ns = NowNs();
    const double dt = has_prev_
                          ? static_cast<double>(now_ns - last_sample_ns_) / 1e9
                          : 0.0;

    std::vector<DiskStat> disks;
    std::map<std::string, RawCounters> current;
    std::string line;
    /// 是否解析到至少一个设备行。用来区分「文件格式异常」（错误）与
    /// 「只有被过滤的虚拟设备/分区」（正常，快照里 disks 为空）。
    bool saw_device_line = false;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::uint64_t major = 0;
        std::uint64_t minor = 0;
        std::string name;
        RawCounters raw;
        if (!(iss >> major >> minor >> name)) {
            continue;
        }
        saw_device_line = true;
        // 字段顺序见 Documentation/admin-guide/iostats.rst：
        // reads read_merged sectors_read read_time writes write_merged
        // sectors_written write_time io_in_progress io_time weighted_io_time
        std::uint64_t reads_merged = 0;
        std::uint64_t writes_merged = 0;
        if (!(iss >> raw.reads >> reads_merged >> raw.sectors_read >>
              raw.read_time_ms >> raw.writes >> writes_merged >>
              raw.sectors_written >> raw.write_time_ms >>
              raw.io_in_progress >> raw.io_time_ms >>
              raw.weighted_io_time_ms)) {
            // 老内核可能只有 7 个字段：保留前四项即可。
            raw.io_in_progress = 0;
            raw.io_time_ms = 0;
            raw.weighted_io_time_ms = 0;
        }
        // 分区与整盘统计的是同一批 I/O（分区计数是整盘的子集），两个都
        // 输出会让消费方求和时翻倍；这里只保留整盘。md/dm 等逻辑设备与
        // 底层盘的重复问题见审查报告 P2。
        if (IsVirtualDevice(name) || IsPartition(name, sys_block_root_)) {
            continue;
        }

        DiskStat stat;
        stat.name = name;
        stat.reads = raw.reads;
        stat.writes = raw.writes;
        stat.sectors_read = raw.sectors_read;
        stat.sectors_written = raw.sectors_written;
        stat.read_time_ms = raw.read_time_ms;
        stat.write_time_ms = raw.write_time_ms;
        stat.io_in_progress = raw.io_in_progress;
        stat.io_time_ms = raw.io_time_ms;
        stat.weighted_io_time_ms = raw.weighted_io_time_ms;

        const auto prev = last_.find(name);
        if (prev != last_.end() && dt > 0.0) {
            const RawCounters& old = prev->second;
            // 计数器只增不减；若回绕则按当前值处理，避免负速率。
            const auto delta = [](std::uint64_t now, std::uint64_t before) {
                return now >= before ? now - before : now;
            };
            const double read_ios = static_cast<double>(delta(raw.reads, old.reads));
            const double write_ios =
                static_cast<double>(delta(raw.writes, old.writes));
            const double read_bytes =
                static_cast<double>(delta(raw.sectors_read, old.sectors_read)) *
                kSectorBytes;
            const double write_bytes =
                static_cast<double>(delta(raw.sectors_written,
                                          old.sectors_written)) *
                kSectorBytes;
            const double read_time =
                static_cast<double>(delta(raw.read_time_ms, old.read_time_ms));
            const double write_time =
                static_cast<double>(delta(raw.write_time_ms, old.write_time_ms));
            const double io_time =
                static_cast<double>(delta(raw.io_time_ms, old.io_time_ms));

            stat.read_bytes_per_sec = read_bytes / dt;
            stat.write_bytes_per_sec = write_bytes / dt;
            stat.read_iops = read_ios / dt;
            stat.write_iops = write_ios / dt;
            stat.avg_read_latency_ms = read_ios > 0.0 ? read_time / read_ios : 0.0;
            stat.avg_write_latency_ms =
                write_ios > 0.0 ? write_time / write_ios : 0.0;
            // io_time 单位是 ms；除以 (dt*1000 ms) 得到占比。
            stat.util_percent = io_time / (dt * 1000.0) * 100.0;
            if (stat.util_percent > 100.0) {
                stat.util_percent = 100.0;
            }
        }
        disks.push_back(std::move(stat));
        current[name] = raw;
    }

    if (disks.empty() && !saw_device_line) {
        return Status::kError;
    }

    // 只有虚拟设备（容器里常见）不是错误：返回空 disks，仍推进采样时刻，
    // 使下一次出现真实设备时能正常差分。
    snapshot.disks = std::move(disks);
    last_ = std::move(current);
    last_sample_ns_ = now_ns;
    has_prev_ = true;
    ++sample_count_;
    return Status::kOk;
}

void DiskMonitor::Stop() {
    last_.clear();
    last_sample_ns_ = 0;
    has_prev_ = false;
}

}  // namespace go2cpp::os::monitor
