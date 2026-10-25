#include "go2cpp/os/monitor/mem_monitor.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace go2cpp::os::monitor {
namespace {

/// 从 "MemTotal:  16384000 kB" 中取出数值部分（忽略单位）。
bool ParseKbLine(const std::string& line, std::string* key,
                 std::uint64_t* value_kb) {
    std::istringstream iss(line);
    std::string key_with_colon;
    std::uint64_t value = 0;
    if (!(iss >> key_with_colon >> value)) {
        return false;
    }
    std::string unit;
    (void)(iss >> unit);  // 可能是 "kB"，也可能缺省
    if (key_with_colon.empty() || key_with_colon.back() != ':') {
        return false;
    }
    key_with_colon.pop_back();
    *key = std::move(key_with_colon);
    *value_kb = value;
    return true;
}

}  // namespace

MemMonitor::MemMonitor(std::string meminfo_path)
    : meminfo_path_(std::move(meminfo_path)) {}

Status MemMonitor::UpdateOnce(Snapshot& snapshot) {
    std::ifstream in(meminfo_path_);
    if (!in) {
        return Status::kError;
    }

    MemInfo info;
    std::string line;
    // 区分「字段缺失」（老内核，需要兜底）与「字段存在但为 0」（内存耗尽，
    // used_percent 应为 100）。仅靠 available_kb == 0 无法区分。
    bool has_available = false;
    while (std::getline(in, line)) {
        std::string key;
        std::uint64_t value = 0;
        if (!ParseKbLine(line, &key, &value)) {
            continue;
        }
        if (key == "MemTotal") {
            info.total_kb = value;
        } else if (key == "MemFree") {
            info.free_kb = value;
        } else if (key == "MemAvailable") {
            info.available_kb = value;
            has_available = true;
        } else if (key == "Buffers") {
            info.buffers_kb = value;
        } else if (key == "Cached") {
            info.cached_kb = value;
        } else if (key == "SwapTotal") {
            info.swap_total_kb = value;
        } else if (key == "SwapFree") {
            info.swap_free_kb = value;
        } else if (key == "Active") {
            info.active_kb = value;
        } else if (key == "Inactive") {
            info.inactive_kb = value;
        } else if (key == "Dirty") {
            info.dirty_kb = value;
        } else if (key == "Writeback") {
            info.writeback_kb = value;
        } else if (key == "AnonPages") {
            info.anon_pages_kb = value;
        } else if (key == "Mapped") {
            info.mapped_kb = value;
        } else if (key == "SReclaimable") {
            info.sreclaimable_kb = value;
        } else if (key == "SUnreclaim") {
            info.sunreclaim_kb = value;
        } else {
            continue;
        }
    }

    // MemTotal 是任何 Linux 都有的字段；缺失说明文件格式异常。
    if (info.total_kb == 0) {
        return Status::kError;
    }
    // 只有字段缺失（老内核）才用 Free + Buffers + Cached 兜底；字段存在
    // 但值为 0 表示内存确实耗尽，此时 used_percent 应为 100。
    if (!has_available) {
        info.available_kb = info.free_kb + info.buffers_kb + info.cached_kb;
    }
    // total 已在上面保证非 0。这里先比较再相减：available > total 时
    // u64 相减会下溢成约 1.8e19，乘以 100 后变成天文数字；而无符号减法
    // 永远得不到负数，原来 `used_percent < 0` 的钳位根本不可能命中。
    // 兜底公式（free+buffers+cached）也可能略高于 total。
    double used_percent = 0.0;
    if (info.available_kb < info.total_kb) {
        used_percent = static_cast<double>(info.total_kb - info.available_kb) /
                       static_cast<double>(info.total_kb) * 100.0;
    }
    if (used_percent > 100.0) {
        used_percent = 100.0;
    }
    info.used_percent = used_percent;

    snapshot.mem = info;
    ++sample_count_;
    return Status::kOk;
}

void MemMonitor::Stop() {
    // 无持有资源。
}

}  // namespace go2cpp::os::monitor
