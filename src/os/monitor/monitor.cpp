#include "go2cpp/os/monitor/monitor.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <sstream>

namespace go2cpp::os::monitor {
namespace {

/// 把可选字段安全写入 JSON：std::uint64_t / double / string 都有隐式转换。
template <typename T>
void Put(Json& j, const char* key, const T& value) {
    j[key] = value;
}

/// 直方图写出：std::array<uint64_t, 32> 直接转 JSON 数组。
void WriteHistogram(Json& j, const KernelHistogram& hist) {
    j = Json::array();
    for (const auto count : hist) {
        j.push_back(count);
    }
}

/// 直方图读入：容忍短数组/缺字段/负数（按 0 处理），保证旧数据可解析。
void ReadHistogram(const Json& j, KernelHistogram& hist,
                    const char* key = "hist") {
    hist.fill(0);
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) {
        return;
    }
    const std::size_t count =
        std::min<std::size_t>(hist.size(), it->size());
    for (std::size_t i = 0; i < count; ++i) {
        if ((*it)[i].is_number_unsigned()) {
            hist[i] = (*it)[i].get<std::uint64_t>();
        }
    }
}

}  // namespace

const char* StatusName(Status status) noexcept {
    switch (status) {
        case Status::kOk:
            return "ok";
        case Status::kUnsupported:
            return "unsupported";
        case Status::kError:
            return "error";
        case Status::kStopped:
            return "stopped";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 内核直方图工具
// ---------------------------------------------------------------------------
double KernelHistUpperUs(std::size_t bucket) noexcept {
    if (bucket == 0) {
        return 1.0;
    }
    // 桶 i 的上界是 2^i µs；溢出桶按 2^30 µs 计（下界）。
    if (bucket >= kKernelHistBuckets - 1) {
        return static_cast<double>(std::uint64_t{1} << 30);
    }
    return static_cast<double>(std::uint64_t{1} << bucket);
}

std::string KernelHistLabel(std::size_t bucket) {
    if (bucket == 0) {
        return "<1us";
    }
    char buffer[64] = {};
    if (bucket >= kKernelHistBuckets - 1) {
        std::snprintf(buffer, sizeof(buffer), ">=%.1fs",
                      static_cast<double>(std::uint64_t{1} << 30) / 1e6);
        return buffer;
    }
    const double lower =
        static_cast<double>(std::uint64_t{1} << (bucket - 1));
    const double upper = static_cast<double>(std::uint64_t{1} << bucket);
    if (upper < 1000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f-%.0fus", lower, upper);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f-%.1fms", lower / 1000.0,
                      upper / 1000.0);
    }
    return buffer;
}

double KernelHistPercentile(const KernelHistogram& hist, std::uint64_t total,
                            double percentile) {
    if (percentile <= 0.0) {
        return 0.0;
    }
    if (percentile > 1.0) {
        percentile = 1.0;
    }
    // 分位数只能基于直方图里真实存在的样本。内核侧 count 与各桶是分别
    // 原子累加的：用户态读取时可能看到 count 已增加、对应桶还没加（或
    // 反过来）。如果直接用 count 作分母，累计值可能永远达不到 target，
    // 函数会返回溢出桶，把 p99 放大几个数量级。用 min(total, 样本数)
    // 作分母：正常情况两者相等，结果不变。
    std::uint64_t samples = 0;
    for (const auto count : hist) {
        samples += count;
    }
    if (samples == 0 || total == 0) {
        return 0.0;
    }
    const std::uint64_t denominator = total < samples ? total : samples;
    const double target = static_cast<double>(denominator) * percentile;
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        cumulative += hist[i];
        if (static_cast<double>(cumulative) >= target) {
            return KernelHistUpperUs(i);
        }
    }
    // 理论上不可达（target <= samples），兜底返回最后一个非空桶。
    for (std::size_t i = hist.size(); i > 0; --i) {
        if (hist[i - 1] != 0) {
            return KernelHistUpperUs(i - 1);
        }
    }
    return 0.0;
}

// ---------------------------------------------------------------------------
// JSON 序列化
// ---------------------------------------------------------------------------
void to_json(Json& j, const CpuCore& v) {
    j = Json{{"name", v.name},
             {"user", v.user},
             {"nice", v.nice},
             {"system", v.system},
             {"idle", v.idle},
             {"iowait", v.iowait},
             {"irq", v.irq},
             {"softirq", v.softirq},
             {"steal", v.steal},
             {"guest", v.guest},
             {"guest_nice", v.guest_nice},
             {"usage_percent", v.usage_percent},
             {"user_percent", v.user_percent},
             {"system_percent", v.system_percent},
             {"nice_percent", v.nice_percent},
             {"idle_percent", v.idle_percent},
             {"iowait_percent", v.iowait_percent},
             {"irq_percent", v.irq_percent},
             {"softirq_percent", v.softirq_percent}};
}

void from_json(const Json& j, CpuCore& v) {
    v.name = j.value("name", std::string{});
    v.user = j.value("user", std::uint64_t{0});
    v.nice = j.value("nice", std::uint64_t{0});
    v.system = j.value("system", std::uint64_t{0});
    v.idle = j.value("idle", std::uint64_t{0});
    v.iowait = j.value("iowait", std::uint64_t{0});
    v.irq = j.value("irq", std::uint64_t{0});
    v.softirq = j.value("softirq", std::uint64_t{0});
    v.steal = j.value("steal", std::uint64_t{0});
    v.guest = j.value("guest", std::uint64_t{0});
    v.guest_nice = j.value("guest_nice", std::uint64_t{0});
    v.usage_percent = j.value("usage_percent", 0.0);
    v.user_percent = j.value("user_percent", 0.0);
    v.system_percent = j.value("system_percent", 0.0);
    v.nice_percent = j.value("nice_percent", 0.0);
    v.idle_percent = j.value("idle_percent", 0.0);
    v.iowait_percent = j.value("iowait_percent", 0.0);
    v.irq_percent = j.value("irq_percent", 0.0);
    v.softirq_percent = j.value("softirq_percent", 0.0);
}

void to_json(Json& j, const CpuLoad& v) {
    j = Json{{"load1", v.load1},
             {"load5", v.load5},
             {"load15", v.load15},
             {"runnable", v.runnable},
             {"total_threads", v.total_threads}};
}

void from_json(const Json& j, CpuLoad& v) {
    v.load1 = j.value("load1", 0.0);
    v.load5 = j.value("load5", 0.0);
    v.load15 = j.value("load15", 0.0);
    v.runnable = j.value("runnable", std::uint64_t{0});
    v.total_threads = j.value("total_threads", std::uint64_t{0});
}

void to_json(Json& j, const MemInfo& v) {
    j = Json{{"total_kb", v.total_kb},
             {"free_kb", v.free_kb},
             {"available_kb", v.available_kb},
             {"buffers_kb", v.buffers_kb},
             {"cached_kb", v.cached_kb},
             {"swap_total_kb", v.swap_total_kb},
             {"swap_free_kb", v.swap_free_kb},
             {"active_kb", v.active_kb},
             {"inactive_kb", v.inactive_kb},
             {"dirty_kb", v.dirty_kb},
             {"writeback_kb", v.writeback_kb},
             {"anon_pages_kb", v.anon_pages_kb},
             {"mapped_kb", v.mapped_kb},
             {"sreclaimable_kb", v.sreclaimable_kb},
             {"sunreclaim_kb", v.sunreclaim_kb},
             {"used_percent", v.used_percent}};
}

void from_json(const Json& j, MemInfo& v) {
    v.total_kb = j.value("total_kb", std::uint64_t{0});
    v.free_kb = j.value("free_kb", std::uint64_t{0});
    v.available_kb = j.value("available_kb", std::uint64_t{0});
    v.buffers_kb = j.value("buffers_kb", std::uint64_t{0});
    v.cached_kb = j.value("cached_kb", std::uint64_t{0});
    v.swap_total_kb = j.value("swap_total_kb", std::uint64_t{0});
    v.swap_free_kb = j.value("swap_free_kb", std::uint64_t{0});
    v.active_kb = j.value("active_kb", std::uint64_t{0});
    v.inactive_kb = j.value("inactive_kb", std::uint64_t{0});
    v.dirty_kb = j.value("dirty_kb", std::uint64_t{0});
    v.writeback_kb = j.value("writeback_kb", std::uint64_t{0});
    v.anon_pages_kb = j.value("anon_pages_kb", std::uint64_t{0});
    v.mapped_kb = j.value("mapped_kb", std::uint64_t{0});
    v.sreclaimable_kb = j.value("sreclaimable_kb", std::uint64_t{0});
    v.sunreclaim_kb = j.value("sunreclaim_kb", std::uint64_t{0});
    v.used_percent = j.value("used_percent", 0.0);
}

void to_json(Json& j, const DiskStat& v) {
    j = Json{{"name", v.name},
             {"reads", v.reads},
             {"writes", v.writes},
             {"sectors_read", v.sectors_read},
             {"sectors_written", v.sectors_written},
             {"read_time_ms", v.read_time_ms},
             {"write_time_ms", v.write_time_ms},
             {"io_in_progress", v.io_in_progress},
             {"io_time_ms", v.io_time_ms},
             {"weighted_io_time_ms", v.weighted_io_time_ms},
             {"read_bytes_per_sec", v.read_bytes_per_sec},
             {"write_bytes_per_sec", v.write_bytes_per_sec},
             {"read_iops", v.read_iops},
             {"write_iops", v.write_iops},
             {"avg_read_latency_ms", v.avg_read_latency_ms},
             {"avg_write_latency_ms", v.avg_write_latency_ms},
             {"util_percent", v.util_percent}};
}

void from_json(const Json& j, DiskStat& v) {
    v.name = j.value("name", std::string{});
    v.reads = j.value("reads", std::uint64_t{0});
    v.writes = j.value("writes", std::uint64_t{0});
    v.sectors_read = j.value("sectors_read", std::uint64_t{0});
    v.sectors_written = j.value("sectors_written", std::uint64_t{0});
    v.read_time_ms = j.value("read_time_ms", std::uint64_t{0});
    v.write_time_ms = j.value("write_time_ms", std::uint64_t{0});
    v.io_in_progress = j.value("io_in_progress", std::uint64_t{0});
    v.io_time_ms = j.value("io_time_ms", std::uint64_t{0});
    v.weighted_io_time_ms = j.value("weighted_io_time_ms", std::uint64_t{0});
    v.read_bytes_per_sec = j.value("read_bytes_per_sec", 0.0);
    v.write_bytes_per_sec = j.value("write_bytes_per_sec", 0.0);
    v.read_iops = j.value("read_iops", 0.0);
    v.write_iops = j.value("write_iops", 0.0);
    v.avg_read_latency_ms = j.value("avg_read_latency_ms", 0.0);
    v.avg_write_latency_ms = j.value("avg_write_latency_ms", 0.0);
    v.util_percent = j.value("util_percent", 0.0);
}

void to_json(Json& j, const NetDevStat& v) {
    j = Json{{"name", v.name},
             {"rcv_bytes", v.rcv_bytes},
             {"rcv_packets", v.rcv_packets},
             {"rcv_errs", v.rcv_errs},
             {"rcv_drops", v.rcv_drops},
             {"snd_bytes", v.snd_bytes},
             {"snd_packets", v.snd_packets},
             {"snd_errs", v.snd_errs},
             {"snd_drops", v.snd_drops},
             {"rcv_rate", v.rcv_rate},
             {"snd_rate", v.snd_rate},
             {"rcv_packets_rate", v.rcv_packets_rate},
             {"snd_packets_rate", v.snd_packets_rate}};
}

void from_json(const Json& j, NetDevStat& v) {
    v.name = j.value("name", std::string{});
    v.rcv_bytes = j.value("rcv_bytes", std::uint64_t{0});
    v.rcv_packets = j.value("rcv_packets", std::uint64_t{0});
    v.rcv_errs = j.value("rcv_errs", std::uint64_t{0});
    v.rcv_drops = j.value("rcv_drops", std::uint64_t{0});
    v.snd_bytes = j.value("snd_bytes", std::uint64_t{0});
    v.snd_packets = j.value("snd_packets", std::uint64_t{0});
    v.snd_errs = j.value("snd_errs", std::uint64_t{0});
    v.snd_drops = j.value("snd_drops", std::uint64_t{0});
    v.rcv_rate = j.value("rcv_rate", 0.0);
    v.snd_rate = j.value("snd_rate", 0.0);
    v.rcv_packets_rate = j.value("rcv_packets_rate", 0.0);
    v.snd_packets_rate = j.value("snd_packets_rate", 0.0);
}

void to_json(Json& j, const NetEbpfStat& v) {
    j = Json{{"name", v.name},
             {"rcv_bytes", v.rcv_bytes},
             {"rcv_packets", v.rcv_packets},
             {"snd_bytes", v.snd_bytes},
             {"snd_packets", v.snd_packets}};
}

void from_json(const Json& j, NetEbpfStat& v) {
    v.name = j.value("name", std::string{});
    v.rcv_bytes = j.value("rcv_bytes", std::uint64_t{0});
    v.rcv_packets = j.value("rcv_packets", std::uint64_t{0});
    v.snd_bytes = j.value("snd_bytes", std::uint64_t{0});
    v.snd_packets = j.value("snd_packets", std::uint64_t{0});
}

void to_json(Json& j, const SyscallLatency& v) {
    j = Json{{"id", v.id},       {"name", v.name},
             {"count", v.count}, {"avg_us", v.avg_us},
             {"p50_us", v.p50_us}, {"p99_us", v.p99_us},
             {"max_us", v.max_us}};
}

void from_json(const Json& j, SyscallLatency& v) {
    v.id = j.value("id", std::uint32_t{0});
    v.name = j.value("name", std::string{});
    v.count = j.value("count", std::uint64_t{0});
    v.avg_us = j.value("avg_us", 0.0);
    v.p50_us = j.value("p50_us", 0.0);
    v.p99_us = j.value("p99_us", 0.0);
    v.max_us = j.value("max_us", 0.0);
}

void to_json(Json& j, const SyscallStats& v) {
    Json hist;
    WriteHistogram(hist, v.hist);
    j = Json{{"active", v.active}, {"reason", v.reason},
             {"total", v.total},   {"hist", hist},
             {"top", v.top}};
}

void from_json(const Json& j, SyscallStats& v) {
    v.active = j.value("active", false);
    v.reason = j.value("reason", std::string{});
    v.total = j.value("total", std::uint64_t{0});
    ReadHistogram(j, v.hist);
    v.top = j.value("top", std::vector<SyscallLatency>{});
}

void to_json(Json& j, const SchedTaskStat& v) {
    j = Json{{"pid", v.pid},
             {"comm", v.comm},
             {"switches", v.switches},
             {"on_cpu_ms", v.on_cpu_ms},
             {"wait_count", v.wait_count},
             {"wait_avg_us", v.wait_avg_us},
             {"wait_max_us", v.wait_max_us}};
}

void from_json(const Json& j, SchedTaskStat& v) {
    v.pid = j.value("pid", std::uint32_t{0});
    v.comm = j.value("comm", std::string{});
    v.switches = j.value("switches", std::uint64_t{0});
    v.on_cpu_ms = j.value("on_cpu_ms", 0.0);
    v.wait_count = j.value("wait_count", std::uint64_t{0});
    v.wait_avg_us = j.value("wait_avg_us", 0.0);
    v.wait_max_us = j.value("wait_max_us", 0.0);
}

void to_json(Json& j, const SchedStats& v) {
    Json hist;
    WriteHistogram(hist, v.hist);
    j = Json{{"active", v.active},
             {"reason", v.reason},
             {"switches", v.switches},
             {"wait_count", v.wait_count},
             {"wait_avg_us", v.wait_avg_us},
             {"wait_p50_us", v.wait_p50_us},
             {"wait_p99_us", v.wait_p99_us},
             {"wait_max_us", v.wait_max_us},
             {"hist", hist},
             {"top", v.top}};
}

void from_json(const Json& j, SchedStats& v) {
    v.active = j.value("active", false);
    v.reason = j.value("reason", std::string{});
    v.switches = j.value("switches", std::uint64_t{0});
    v.wait_count = j.value("wait_count", std::uint64_t{0});
    v.wait_avg_us = j.value("wait_avg_us", 0.0);
    v.wait_p50_us = j.value("wait_p50_us", 0.0);
    v.wait_p99_us = j.value("wait_p99_us", 0.0);
    v.wait_max_us = j.value("wait_max_us", 0.0);
    ReadHistogram(j, v.hist);
    v.top = j.value("top", std::vector<SchedTaskStat>{});
}

void to_json(Json& j, const FutexTaskStat& v) {
    j = Json{{"pid", v.pid},
             {"comm", v.comm},
             {"wait_count", v.wait_count},
             {"wake_count", v.wake_count},
             {"eagain_count", v.eagain_count},
             {"timedout_count", v.timedout_count},
             {"error_count", v.error_count},
             {"wait_avg_us", v.wait_avg_us},
             {"wait_max_us", v.wait_max_us}};
}

void from_json(const Json& j, FutexTaskStat& v) {
    v.pid = j.value("pid", std::uint32_t{0});
    v.comm = j.value("comm", std::string{});
    v.wait_count = j.value("wait_count", std::uint64_t{0});
    v.wake_count = j.value("wake_count", std::uint64_t{0});
    v.eagain_count = j.value("eagain_count", std::uint64_t{0});
    v.timedout_count = j.value("timedout_count", std::uint64_t{0});
    v.error_count = j.value("error_count", std::uint64_t{0});
    v.wait_avg_us = j.value("wait_avg_us", 0.0);
    v.wait_max_us = j.value("wait_max_us", 0.0);
}

void to_json(Json& j, const FutexStats& v) {
    Json hist;
    WriteHistogram(hist, v.hist);
    j = Json{{"active", v.active},
             {"reason", v.reason},
             {"wait_count", v.wait_count},
             {"wake_count", v.wake_count},
             {"eagain_count", v.eagain_count},
             {"timedout_count", v.timedout_count},
             {"error_count", v.error_count},
             {"wait_wake_ratio", v.wait_wake_ratio},
             {"wait_avg_us", v.wait_avg_us},
             {"wait_p50_us", v.wait_p50_us},
             {"wait_p99_us", v.wait_p99_us},
             {"wait_max_us", v.wait_max_us},
             {"hist", hist},
             {"top", v.top}};
}

void from_json(const Json& j, FutexStats& v) {
    v.active = j.value("active", false);
    v.reason = j.value("reason", std::string{});
    v.wait_count = j.value("wait_count", std::uint64_t{0});
    v.wake_count = j.value("wake_count", std::uint64_t{0});
    v.eagain_count = j.value("eagain_count", std::uint64_t{0});
    v.timedout_count = j.value("timedout_count", std::uint64_t{0});
    v.error_count = j.value("error_count", std::uint64_t{0});
    v.wait_wake_ratio = j.value("wait_wake_ratio", 0.0);
    v.wait_avg_us = j.value("wait_avg_us", 0.0);
    v.wait_p50_us = j.value("wait_p50_us", 0.0);
    v.wait_p99_us = j.value("wait_p99_us", 0.0);
    v.wait_max_us = j.value("wait_max_us", 0.0);
    ReadHistogram(j, v.hist);
    v.top = j.value("top", std::vector<FutexTaskStat>{});
}

void to_json(Json& j, const TcpConnStat& v) {
    j = Json{{"local", v.local},
             {"remote", v.remote},
             {"family", v.family},
             {"retransmits", v.retransmits},
             {"rtt_samples", v.rtt_samples},
             {"rtt_avg_us", v.rtt_avg_us},
             {"rtt_p50_us", v.rtt_p50_us},
             {"rtt_p99_us", v.rtt_p99_us},
             {"rtt_max_us", v.rtt_max_us}};
}

void from_json(const Json& j, TcpConnStat& v) {
    v.local = j.value("local", std::string{});
    v.remote = j.value("remote", std::string{});
    v.family = j.value("family", std::string{});
    v.retransmits = j.value("retransmits", std::uint64_t{0});
    v.rtt_samples = j.value("rtt_samples", std::uint64_t{0});
    v.rtt_avg_us = j.value("rtt_avg_us", 0.0);
    v.rtt_p50_us = j.value("rtt_p50_us", 0.0);
    v.rtt_p99_us = j.value("rtt_p99_us", 0.0);
    v.rtt_max_us = j.value("rtt_max_us", 0.0);
}

void to_json(Json& j, const TcpStats& v) {
    Json hist;
    Json conn_hist;
    WriteHistogram(hist, v.rtt_hist);
    WriteHistogram(conn_hist, v.conn_rtt_hist);
    j = Json{{"active", v.active},
             {"reason", v.reason},
             {"rtt_source", v.rtt_source},
             {"retransmits", v.retransmits},
             {"retransmits_per_sec", v.retransmits_per_sec},
             {"rtt_samples", v.rtt_samples},
             {"hist", hist},
             {"rtt_p50_us", v.rtt_p50_us},
             {"rtt_p99_us", v.rtt_p99_us},
             {"rtt_avg_us", v.rtt_avg_us},
             {"rtt_max_us", v.rtt_max_us},
             {"rtt_weighting", v.rtt_weighting},
             {"conn_rtt_samples", v.conn_rtt_samples},
             {"conn_hist", conn_hist},
             {"conn_rtt_p50_us", v.conn_rtt_p50_us},
             {"conn_rtt_p99_us", v.conn_rtt_p99_us},
             {"top", v.top}};
}

void from_json(const Json& j, TcpStats& v) {
    v.active = j.value("active", false);
    v.reason = j.value("reason", std::string{});
    v.rtt_source = j.value("rtt_source", std::string{});
    v.retransmits = j.value("retransmits", std::uint64_t{0});
    v.retransmits_per_sec = j.value("retransmits_per_sec", 0.0);
    v.rtt_samples = j.value("rtt_samples", std::uint64_t{0});
    ReadHistogram(j, v.rtt_hist);
    v.rtt_p50_us = j.value("rtt_p50_us", 0.0);
    v.rtt_p99_us = j.value("rtt_p99_us", 0.0);
    v.rtt_avg_us = j.value("rtt_avg_us", 0.0);
    v.rtt_max_us = j.value("rtt_max_us", 0.0);
    v.rtt_weighting = j.value(
        "rtt_weighting",
        std::string{
            "rtt_hist=1/8_ack_samples; "
            "conn_rtt_hist=one_sample_per_connection"});
    v.conn_rtt_samples = j.value("conn_rtt_samples", std::uint64_t{0});
    ReadHistogram(j, v.conn_rtt_hist, "conn_hist");
    v.conn_rtt_p50_us = j.value("conn_rtt_p50_us", 0.0);
    v.conn_rtt_p99_us = j.value("conn_rtt_p99_us", 0.0);
    v.top = j.value("top", std::vector<TcpConnStat>{});
}

void to_json(Json& j, const FileIoTaskStat& v) {
    // pid_init_ns 是 pid 的显式别名：pid 的值来自 bpf_get_current_pid_tgid，
    // 是 init PID namespace 的 tgid；namespace_pid 才是 ps 可见的 pid。
    j = Json{{"pid", v.pid},
             {"pid_init_ns", v.pid},
             {"namespace_pid", v.namespace_pid},
             {"comm", v.comm},
             {"read_count", v.read_count},
             {"write_count", v.write_count},
             {"read_avg_us", v.read_avg_us},
             {"read_max_us", v.read_max_us},
             {"write_avg_us", v.write_avg_us},
             {"write_max_us", v.write_max_us}};
}

void from_json(const Json& j, FileIoTaskStat& v) {
    // 新 JSON 同时带 pid_init_ns 与 pid，优先显式语义字段；旧 JSON 只有 pid。
    v.pid = j.value("pid_init_ns", std::uint32_t{0});
    if (v.pid == 0) {
        v.pid = j.value("pid", std::uint32_t{0});
    }
    v.namespace_pid = j.value("namespace_pid", std::uint32_t{0});
    v.comm = j.value("comm", std::string{});
    v.read_count = j.value("read_count", std::uint64_t{0});
    v.write_count = j.value("write_count", std::uint64_t{0});
    v.read_avg_us = j.value("read_avg_us", 0.0);
    v.read_max_us = j.value("read_max_us", 0.0);
    v.write_avg_us = j.value("write_avg_us", 0.0);
    v.write_max_us = j.value("write_max_us", 0.0);
}

void to_json(Json& j, const FileIoStats& v) {
    Json read_hist;
    Json write_hist;
    WriteHistogram(read_hist, v.read_hist);
    WriteHistogram(write_hist, v.write_hist);
    j = Json{{"active", v.active},
             {"reason", v.reason},
             {"read_count", v.read_count},
             {"write_count", v.write_count},
             {"read_hist", read_hist},
             {"write_hist", write_hist},
             {"read_p50_us", v.read_p50_us},
             {"read_p99_us", v.read_p99_us},
             {"read_avg_us", v.read_avg_us},
             {"read_max_us", v.read_max_us},
             {"write_p50_us", v.write_p50_us},
             {"write_p99_us", v.write_p99_us},
             {"write_avg_us", v.write_avg_us},
             {"write_max_us", v.write_max_us},
             {"read_iops", v.read_iops},
             {"write_iops", v.write_iops},
             {"readv_count", v.readv_count},
             {"writev_count", v.writev_count},
             {"readv_avg_us", v.readv_avg_us},
             {"readv_max_us", v.readv_max_us},
             {"writev_avg_us", v.writev_avg_us},
             {"writev_max_us", v.writev_max_us},
             {"uring_read_count", v.uring_read_count},
             {"uring_write_count", v.uring_write_count},
             {"uring_read_pending", v.uring_read_pending},
             {"uring_write_pending", v.uring_write_pending},
             {"uring_read_avg_us", v.uring_read_avg_us},
             {"uring_read_max_us", v.uring_read_max_us},
             {"uring_write_avg_us", v.uring_write_avg_us},
             {"uring_write_max_us", v.uring_write_max_us},
             {"coverage", v.coverage},
             {"coverage_gap", v.coverage_gap},
             {"top", v.top}};
}

void from_json(const Json& j, FileIoStats& v) {
    v.active = j.value("active", false);
    v.reason = j.value("reason", std::string{});
    v.read_count = j.value("read_count", std::uint64_t{0});
    v.write_count = j.value("write_count", std::uint64_t{0});
    ReadHistogram(j, v.read_hist, "read_hist");
    ReadHistogram(j, v.write_hist, "write_hist");
    v.read_p50_us = j.value("read_p50_us", 0.0);
    v.read_p99_us = j.value("read_p99_us", 0.0);
    v.read_avg_us = j.value("read_avg_us", 0.0);
    v.read_max_us = j.value("read_max_us", 0.0);
    v.write_p50_us = j.value("write_p50_us", 0.0);
    v.write_p99_us = j.value("write_p99_us", 0.0);
    v.write_avg_us = j.value("write_avg_us", 0.0);
    v.write_max_us = j.value("write_max_us", 0.0);
    v.read_iops = j.value("read_iops", 0.0);
    v.write_iops = j.value("write_iops", 0.0);
    v.readv_count = j.value("readv_count", std::uint64_t{0});
    v.writev_count = j.value("writev_count", std::uint64_t{0});
    v.readv_avg_us = j.value("readv_avg_us", 0.0);
    v.readv_max_us = j.value("readv_max_us", 0.0);
    v.writev_avg_us = j.value("writev_avg_us", 0.0);
    v.writev_max_us = j.value("writev_max_us", 0.0);
    v.uring_read_count = j.value("uring_read_count", std::uint64_t{0});
    v.uring_write_count = j.value("uring_write_count", std::uint64_t{0});
    v.uring_read_pending = j.value("uring_read_pending", std::uint64_t{0});
    v.uring_write_pending = j.value("uring_write_pending", std::uint64_t{0});
    v.uring_read_avg_us = j.value("uring_read_avg_us", 0.0);
    v.uring_read_max_us = j.value("uring_read_max_us", 0.0);
    v.uring_write_avg_us = j.value("uring_write_avg_us", 0.0);
    v.uring_write_max_us = j.value("uring_write_max_us", 0.0);
    v.coverage = j.value("coverage", std::string{});
    v.coverage_gap = j.value("coverage_gap", std::string{});
    v.top = j.value("top", std::vector<FileIoTaskStat>{});
}

void to_json(Json& j, const HostInfo& v) {
    j = Json{{"hostname", v.hostname},
             {"os", v.os},
             {"kernel", v.kernel},
             {"arch", v.arch}};
}

void from_json(const Json& j, HostInfo& v) {
    v.hostname = j.value("hostname", std::string{});
    v.os = j.value("os", std::string{});
    v.kernel = j.value("kernel", std::string{});
    v.arch = j.value("arch", std::string{});
}

void to_json(Json& j, const Snapshot& v) {
    j = Json{{"host", v.host},
             {"timestamp_ms", v.timestamp_ms},
             {"host_info", v.host_info},
             {"load", v.load},
             {"cpus", v.cpus},
             {"mem", v.mem},
             {"disks", v.disks},
             {"nets", v.nets},
             {"ebpf_nets", v.ebpf_nets},
             {"ebpf_active", v.ebpf_active},
             {"ebpf_reason", v.ebpf_reason},
             {"syscalls", v.syscalls},
             {"sched", v.sched},
             {"futex", v.futex},
             {"tcp", v.tcp},
             {"fileio", v.fileio}};
}

void from_json(const Json& j, Snapshot& v) {
    v.host = j.value("host", std::string{});
    v.timestamp_ms = j.value("timestamp_ms", std::int64_t{0});
    v.host_info = j.value("host_info", HostInfo{});
    v.load = j.value("load", CpuLoad{});
    v.cpus = j.value("cpus", std::vector<CpuCore>{});
    v.mem = j.value("mem", MemInfo{});
    v.disks = j.value("disks", std::vector<DiskStat>{});
    v.nets = j.value("nets", std::vector<NetDevStat>{});
    v.ebpf_nets = j.value("ebpf_nets", std::vector<NetEbpfStat>{});
    v.ebpf_active = j.value("ebpf_active", false);
    v.ebpf_reason = j.value("ebpf_reason", std::string{});
    v.syscalls = j.value("syscalls", SyscallStats{});
    v.sched = j.value("sched", SchedStats{});
    v.futex = j.value("futex", FutexStats{});
    v.tcp = j.value("tcp", TcpStats{});
    v.fileio = j.value("fileio", FileIoStats{});
}

std::string SnapshotToJson(const Snapshot& snapshot) {
    Json j = snapshot;
    return j.dump();
}

bool SnapshotFromJson(const std::string& text, Snapshot& snapshot,
                      std::string* error) {
    try {
        Json j = Json::parse(text);
        snapshot = j.get<Snapshot>();
        return true;
    } catch (const std::exception& e) {
        if (error != nullptr) {
            *error = e.what();
        }
        return false;
    }
}

double HealthScore(const Snapshot& snapshot) {
    // CPU：只平均分核；"cpu" 是 /proc/stat 的汇总行（各核 jiffies 之和），
    // 把它当成一核会重复计权，也会让下面的核数多 1。只有汇总行时退回用它。
    double cpu = 0.0;
    double cpu_sum = 0.0;
    double core_count = 0.0;
    for (const auto& core : snapshot.cpus) {
        if (core.name == "cpu") {
            continue;
        }
        cpu_sum += core.usage_percent;
        core_count += 1.0;
    }
    if (core_count > 0.0) {
        cpu = cpu_sum / core_count;
    } else if (!snapshot.cpus.empty()) {
        cpu = snapshot.cpus.front().usage_percent;
    }

    // 内存使用率。
    const double mem = snapshot.mem.used_percent;

    // 磁盘：最忙的一块盘。
    double disk = 0.0;
    for (const auto& d : snapshot.disks) {
        disk = std::max(disk, d.util_percent);
    }

    // 负载：按核数归一化；load1 等于核数时该项扣满 50 分。
    double load_penalty = 0.0;
    const double cores = core_count > 0.0
                             ? core_count
                             : static_cast<double>(snapshot.cpus.size());
    if (cores > 0.0) {
        load_penalty = std::min(50.0, snapshot.load.load1 / cores * 50.0);
    }

    // 网络错误惩罚：错误包占收发包总数的比例，最多扣 10 分。
    double net_penalty = 0.0;
    std::uint64_t net_packets = 0;
    std::uint64_t net_errors = 0;
    for (const auto& n : snapshot.nets) {
        net_packets += n.rcv_packets + n.snd_packets;
        net_errors += n.rcv_errs + n.snd_errs;
    }
    if (net_packets > 0) {
        net_penalty = std::min(10.0, 10.0 *
                                         static_cast<double>(net_errors) /
                                         static_cast<double>(net_packets));
    }

    // 四项加权：CPU 40%、内存 30%、磁盘 20%、负载 10%，再减网络惩罚。
    double score = 0.4 * (100.0 - cpu) + 0.3 * (100.0 - mem) +
                   0.2 * (100.0 - disk) + 0.1 * (100.0 - load_penalty * 2.0);
    score -= net_penalty;
    if (score < 0.0) {
        score = 0.0;
    }
    if (score > 100.0) {
        score = 100.0;
    }
    return score;
}

}  // namespace go2cpp::os::monitor
