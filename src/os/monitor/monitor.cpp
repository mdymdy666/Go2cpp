#include "go2cpp/os/monitor/monitor.hpp"

#include <algorithm>
#include <cstdio>
#include <type_traits>
#include <utility>

namespace go2cpp::os::monitor {
namespace {

/// 把标量写进当前对象/数组：类型分派集中在这里，36 个函数不再各写一遍。
template <typename T>
void Put(go2cpp::json::JsonBuilder& builder, const char* key, const T& value) {
    builder.Key(key);
    if constexpr (std::is_same_v<T, bool>) {
        builder.Bool(value);
    } else if constexpr (std::is_same_v<T, std::string>) {
        builder.String(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        builder.Double(value);
    } else if constexpr (std::is_signed_v<T>) {
        builder.Int(value);
    } else {
        builder.Uint(value);
    }
}

/// 取值：字段缺失或类型不符时保持 out 的现值（对应旧版 value 的
/// value("k", default) 的「缺失取默认」语义）。
template <typename T>
void Get(const go2cpp::json::Value& object, const char* key, T& out) {
    T parsed{};
    if (object[key].As(parsed)) {
        out = std::move(parsed);
    }
}

/// 读一个数组字段到 vector：缺字段/非数组按空处理，元素走调用方给的回调。
template <typename T, typename FromFn>
void ReadArray(const go2cpp::json::Value& object, const char* key,
               std::vector<T>& out, FromFn from) {
    out.clear();
    const go2cpp::json::Value field = object[key];
    if (!field.IsArray()) {
        return;
    }
    out.reserve(field.Size());
    for (std::size_t i = 0; i < field.Size(); ++i) {
        T item{};
        from(field.At(i), item);
        out.push_back(std::move(item));
    }
}

/// 直方图写出：32 个 uint64 计数数组。
void WriteHistogram(go2cpp::json::JsonBuilder& builder, const KernelHistogram& hist) {
    builder.BeginArray();
    for (const auto count : hist) {
        builder.Uint(count);
    }
    builder.EndArray();
}

/// 直方图读入：容忍短数组/缺字段/负数（按 0 处理），保证旧数据可解析。
void ReadHistogram(const go2cpp::json::Value& object, KernelHistogram& hist,
                   const char* key = "hist") {
    hist.fill(0);
    const go2cpp::json::Value field = object[key];
    if (!field.IsArray()) {
        return;
    }
    const std::size_t count = std::min<std::size_t>(hist.size(), field.Size());
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t parsed = 0;
        if (field.At(i).As(parsed)) {
            hist[i] = parsed;
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
// JSON 序列化（自研 go2cpp::json：JsonBuilder 写 / Value 读）
// ---------------------------------------------------------------------------
void ToJson(go2cpp::json::JsonBuilder& builder, const CpuCore& v) {
    builder.BeginObject();
    Put(builder, "guest", v.guest);
    Put(builder, "guest_nice", v.guest_nice);
    Put(builder, "idle", v.idle);
    Put(builder, "idle_percent", v.idle_percent);
    Put(builder, "iowait", v.iowait);
    Put(builder, "iowait_percent", v.iowait_percent);
    Put(builder, "irq", v.irq);
    Put(builder, "irq_percent", v.irq_percent);
    Put(builder, "name", v.name);
    Put(builder, "nice", v.nice);
    Put(builder, "nice_percent", v.nice_percent);
    Put(builder, "softirq", v.softirq);
    Put(builder, "softirq_percent", v.softirq_percent);
    Put(builder, "steal", v.steal);
    Put(builder, "system", v.system);
    Put(builder, "system_percent", v.system_percent);
    Put(builder, "usage_percent", v.usage_percent);
    Put(builder, "user", v.user);
    Put(builder, "user_percent", v.user_percent);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, CpuCore& v) {
    Get(j, "guest", v.guest);
    Get(j, "guest_nice", v.guest_nice);
    Get(j, "idle", v.idle);
    Get(j, "idle_percent", v.idle_percent);
    Get(j, "iowait", v.iowait);
    Get(j, "iowait_percent", v.iowait_percent);
    Get(j, "irq", v.irq);
    Get(j, "irq_percent", v.irq_percent);
    Get(j, "name", v.name);
    Get(j, "nice", v.nice);
    Get(j, "nice_percent", v.nice_percent);
    Get(j, "softirq", v.softirq);
    Get(j, "softirq_percent", v.softirq_percent);
    Get(j, "steal", v.steal);
    Get(j, "system", v.system);
    Get(j, "system_percent", v.system_percent);
    Get(j, "usage_percent", v.usage_percent);
    Get(j, "user", v.user);
    Get(j, "user_percent", v.user_percent);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const CpuLoad& v) {
    builder.BeginObject();
    Put(builder, "load1", v.load1);
    Put(builder, "load15", v.load15);
    Put(builder, "load5", v.load5);
    Put(builder, "runnable", v.runnable);
    Put(builder, "total_threads", v.total_threads);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, CpuLoad& v) {
    Get(j, "load1", v.load1);
    Get(j, "load15", v.load15);
    Get(j, "load5", v.load5);
    Get(j, "runnable", v.runnable);
    Get(j, "total_threads", v.total_threads);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const MemInfo& v) {
    builder.BeginObject();
    Put(builder, "active_kb", v.active_kb);
    Put(builder, "anon_pages_kb", v.anon_pages_kb);
    Put(builder, "available_kb", v.available_kb);
    Put(builder, "buffers_kb", v.buffers_kb);
    Put(builder, "cached_kb", v.cached_kb);
    Put(builder, "dirty_kb", v.dirty_kb);
    Put(builder, "free_kb", v.free_kb);
    Put(builder, "inactive_kb", v.inactive_kb);
    Put(builder, "mapped_kb", v.mapped_kb);
    Put(builder, "sreclaimable_kb", v.sreclaimable_kb);
    Put(builder, "sunreclaim_kb", v.sunreclaim_kb);
    Put(builder, "swap_free_kb", v.swap_free_kb);
    Put(builder, "swap_total_kb", v.swap_total_kb);
    Put(builder, "total_kb", v.total_kb);
    Put(builder, "used_percent", v.used_percent);
    Put(builder, "writeback_kb", v.writeback_kb);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, MemInfo& v) {
    Get(j, "active_kb", v.active_kb);
    Get(j, "anon_pages_kb", v.anon_pages_kb);
    Get(j, "available_kb", v.available_kb);
    Get(j, "buffers_kb", v.buffers_kb);
    Get(j, "cached_kb", v.cached_kb);
    Get(j, "dirty_kb", v.dirty_kb);
    Get(j, "free_kb", v.free_kb);
    Get(j, "inactive_kb", v.inactive_kb);
    Get(j, "mapped_kb", v.mapped_kb);
    Get(j, "sreclaimable_kb", v.sreclaimable_kb);
    Get(j, "sunreclaim_kb", v.sunreclaim_kb);
    Get(j, "swap_free_kb", v.swap_free_kb);
    Get(j, "swap_total_kb", v.swap_total_kb);
    Get(j, "total_kb", v.total_kb);
    Get(j, "used_percent", v.used_percent);
    Get(j, "writeback_kb", v.writeback_kb);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const DiskStat& v) {
    builder.BeginObject();
    Put(builder, "avg_read_latency_ms", v.avg_read_latency_ms);
    Put(builder, "avg_write_latency_ms", v.avg_write_latency_ms);
    Put(builder, "io_in_progress", v.io_in_progress);
    Put(builder, "io_time_ms", v.io_time_ms);
    Put(builder, "name", v.name);
    Put(builder, "read_bytes_per_sec", v.read_bytes_per_sec);
    Put(builder, "read_iops", v.read_iops);
    Put(builder, "read_time_ms", v.read_time_ms);
    Put(builder, "reads", v.reads);
    Put(builder, "sectors_read", v.sectors_read);
    Put(builder, "sectors_written", v.sectors_written);
    Put(builder, "util_percent", v.util_percent);
    Put(builder, "weighted_io_time_ms", v.weighted_io_time_ms);
    Put(builder, "write_bytes_per_sec", v.write_bytes_per_sec);
    Put(builder, "write_iops", v.write_iops);
    Put(builder, "write_time_ms", v.write_time_ms);
    Put(builder, "writes", v.writes);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, DiskStat& v) {
    Get(j, "avg_read_latency_ms", v.avg_read_latency_ms);
    Get(j, "avg_write_latency_ms", v.avg_write_latency_ms);
    Get(j, "io_in_progress", v.io_in_progress);
    Get(j, "io_time_ms", v.io_time_ms);
    Get(j, "name", v.name);
    Get(j, "read_bytes_per_sec", v.read_bytes_per_sec);
    Get(j, "read_iops", v.read_iops);
    Get(j, "read_time_ms", v.read_time_ms);
    Get(j, "reads", v.reads);
    Get(j, "sectors_read", v.sectors_read);
    Get(j, "sectors_written", v.sectors_written);
    Get(j, "util_percent", v.util_percent);
    Get(j, "weighted_io_time_ms", v.weighted_io_time_ms);
    Get(j, "write_bytes_per_sec", v.write_bytes_per_sec);
    Get(j, "write_iops", v.write_iops);
    Get(j, "write_time_ms", v.write_time_ms);
    Get(j, "writes", v.writes);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const NetDevStat& v) {
    builder.BeginObject();
    Put(builder, "name", v.name);
    Put(builder, "rcv_bytes", v.rcv_bytes);
    Put(builder, "rcv_drops", v.rcv_drops);
    Put(builder, "rcv_errs", v.rcv_errs);
    Put(builder, "rcv_packets", v.rcv_packets);
    Put(builder, "rcv_packets_rate", v.rcv_packets_rate);
    Put(builder, "rcv_rate", v.rcv_rate);
    Put(builder, "snd_bytes", v.snd_bytes);
    Put(builder, "snd_drops", v.snd_drops);
    Put(builder, "snd_errs", v.snd_errs);
    Put(builder, "snd_packets", v.snd_packets);
    Put(builder, "snd_packets_rate", v.snd_packets_rate);
    Put(builder, "snd_rate", v.snd_rate);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, NetDevStat& v) {
    Get(j, "name", v.name);
    Get(j, "rcv_bytes", v.rcv_bytes);
    Get(j, "rcv_drops", v.rcv_drops);
    Get(j, "rcv_errs", v.rcv_errs);
    Get(j, "rcv_packets", v.rcv_packets);
    Get(j, "rcv_packets_rate", v.rcv_packets_rate);
    Get(j, "rcv_rate", v.rcv_rate);
    Get(j, "snd_bytes", v.snd_bytes);
    Get(j, "snd_drops", v.snd_drops);
    Get(j, "snd_errs", v.snd_errs);
    Get(j, "snd_packets", v.snd_packets);
    Get(j, "snd_packets_rate", v.snd_packets_rate);
    Get(j, "snd_rate", v.snd_rate);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const NetEbpfStat& v) {
    builder.BeginObject();
    Put(builder, "name", v.name);
    Put(builder, "rcv_bytes", v.rcv_bytes);
    Put(builder, "rcv_packets", v.rcv_packets);
    Put(builder, "snd_bytes", v.snd_bytes);
    Put(builder, "snd_packets", v.snd_packets);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, NetEbpfStat& v) {
    Get(j, "name", v.name);
    Get(j, "rcv_bytes", v.rcv_bytes);
    Get(j, "rcv_packets", v.rcv_packets);
    Get(j, "snd_bytes", v.snd_bytes);
    Get(j, "snd_packets", v.snd_packets);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const SyscallLatency& v) {
    builder.BeginObject();
    Put(builder, "avg_us", v.avg_us);
    Put(builder, "count", v.count);
    Put(builder, "id", v.id);
    Put(builder, "max_us", v.max_us);
    Put(builder, "name", v.name);
    Put(builder, "p50_us", v.p50_us);
    Put(builder, "p99_us", v.p99_us);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, SyscallLatency& v) {
    Get(j, "avg_us", v.avg_us);
    Get(j, "count", v.count);
    Get(j, "id", v.id);
    Get(j, "max_us", v.max_us);
    Get(j, "name", v.name);
    Get(j, "p50_us", v.p50_us);
    Get(j, "p99_us", v.p99_us);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const SyscallStats& v) {
    builder.BeginObject();
    Put(builder, "active", v.active);
    builder.Key("hist");
    WriteHistogram(builder, v.hist);
    Put(builder, "reason", v.reason);
    builder.Key("top");
    builder.BeginArray();
    for (const auto& item : v.top) {
        ToJson(builder, item);
    }
    builder.EndArray();
    Put(builder, "total", v.total);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, SyscallStats& v) {
    Get(j, "active", v.active);
    Get(j, "reason", v.reason);
    Get(j, "total", v.total);
    ReadHistogram(j, v.hist);
    ReadArray(j, "top", v.top,
              [](const go2cpp::json::Value& element, SyscallLatency& item) {
                  FromJson(element, item);
              });
}

void ToJson(go2cpp::json::JsonBuilder& builder, const SchedTaskStat& v) {
    builder.BeginObject();
    Put(builder, "comm", v.comm);
    Put(builder, "on_cpu_ms", v.on_cpu_ms);
    Put(builder, "pid", v.pid);
    Put(builder, "switches", v.switches);
    Put(builder, "wait_avg_us", v.wait_avg_us);
    Put(builder, "wait_count", v.wait_count);
    Put(builder, "wait_max_us", v.wait_max_us);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, SchedTaskStat& v) {
    Get(j, "comm", v.comm);
    Get(j, "on_cpu_ms", v.on_cpu_ms);
    Get(j, "pid", v.pid);
    Get(j, "switches", v.switches);
    Get(j, "wait_avg_us", v.wait_avg_us);
    Get(j, "wait_count", v.wait_count);
    Get(j, "wait_max_us", v.wait_max_us);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const SchedStats& v) {
    builder.BeginObject();
    Put(builder, "active", v.active);
    builder.Key("hist");
    WriteHistogram(builder, v.hist);
    Put(builder, "reason", v.reason);
    Put(builder, "switches", v.switches);
    builder.Key("top");
    builder.BeginArray();
    for (const auto& item : v.top) {
        ToJson(builder, item);
    }
    builder.EndArray();
    Put(builder, "wait_avg_us", v.wait_avg_us);
    Put(builder, "wait_count", v.wait_count);
    Put(builder, "wait_max_us", v.wait_max_us);
    Put(builder, "wait_p50_us", v.wait_p50_us);
    Put(builder, "wait_p99_us", v.wait_p99_us);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, SchedStats& v) {
    Get(j, "active", v.active);
    Get(j, "reason", v.reason);
    Get(j, "switches", v.switches);
    Get(j, "wait_avg_us", v.wait_avg_us);
    Get(j, "wait_count", v.wait_count);
    Get(j, "wait_max_us", v.wait_max_us);
    Get(j, "wait_p50_us", v.wait_p50_us);
    Get(j, "wait_p99_us", v.wait_p99_us);
    ReadHistogram(j, v.hist);
    ReadArray(j, "top", v.top,
              [](const go2cpp::json::Value& element, SchedTaskStat& item) {
                  FromJson(element, item);
              });
}

void ToJson(go2cpp::json::JsonBuilder& builder, const FutexTaskStat& v) {
    builder.BeginObject();
    Put(builder, "comm", v.comm);
    Put(builder, "eagain_count", v.eagain_count);
    Put(builder, "error_count", v.error_count);
    Put(builder, "pid", v.pid);
    Put(builder, "timedout_count", v.timedout_count);
    Put(builder, "wait_avg_us", v.wait_avg_us);
    Put(builder, "wait_count", v.wait_count);
    Put(builder, "wait_max_us", v.wait_max_us);
    Put(builder, "wake_count", v.wake_count);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, FutexTaskStat& v) {
    Get(j, "comm", v.comm);
    Get(j, "eagain_count", v.eagain_count);
    Get(j, "error_count", v.error_count);
    Get(j, "pid", v.pid);
    Get(j, "timedout_count", v.timedout_count);
    Get(j, "wait_avg_us", v.wait_avg_us);
    Get(j, "wait_count", v.wait_count);
    Get(j, "wait_max_us", v.wait_max_us);
    Get(j, "wake_count", v.wake_count);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const FutexStats& v) {
    builder.BeginObject();
    Put(builder, "active", v.active);
    Put(builder, "eagain_count", v.eagain_count);
    Put(builder, "error_count", v.error_count);
    builder.Key("hist");
    WriteHistogram(builder, v.hist);
    Put(builder, "reason", v.reason);
    Put(builder, "timedout_count", v.timedout_count);
    builder.Key("top");
    builder.BeginArray();
    for (const auto& item : v.top) {
        ToJson(builder, item);
    }
    builder.EndArray();
    Put(builder, "wait_avg_us", v.wait_avg_us);
    Put(builder, "wait_count", v.wait_count);
    Put(builder, "wait_max_us", v.wait_max_us);
    Put(builder, "wait_p50_us", v.wait_p50_us);
    Put(builder, "wait_p99_us", v.wait_p99_us);
    Put(builder, "wait_wake_ratio", v.wait_wake_ratio);
    Put(builder, "wake_count", v.wake_count);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, FutexStats& v) {
    Get(j, "active", v.active);
    Get(j, "eagain_count", v.eagain_count);
    Get(j, "error_count", v.error_count);
    Get(j, "reason", v.reason);
    Get(j, "timedout_count", v.timedout_count);
    Get(j, "wait_avg_us", v.wait_avg_us);
    Get(j, "wait_count", v.wait_count);
    Get(j, "wait_max_us", v.wait_max_us);
    Get(j, "wait_p50_us", v.wait_p50_us);
    Get(j, "wait_p99_us", v.wait_p99_us);
    Get(j, "wait_wake_ratio", v.wait_wake_ratio);
    Get(j, "wake_count", v.wake_count);
    ReadHistogram(j, v.hist);
    ReadArray(j, "top", v.top,
              [](const go2cpp::json::Value& element, FutexTaskStat& item) {
                  FromJson(element, item);
              });
}

void ToJson(go2cpp::json::JsonBuilder& builder, const TcpConnStat& v) {
    builder.BeginObject();
    Put(builder, "family", v.family);
    Put(builder, "local", v.local);
    Put(builder, "remote", v.remote);
    Put(builder, "retransmits", v.retransmits);
    Put(builder, "rtt_avg_us", v.rtt_avg_us);
    Put(builder, "rtt_max_us", v.rtt_max_us);
    Put(builder, "rtt_p50_us", v.rtt_p50_us);
    Put(builder, "rtt_p99_us", v.rtt_p99_us);
    Put(builder, "rtt_samples", v.rtt_samples);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, TcpConnStat& v) {
    Get(j, "family", v.family);
    Get(j, "local", v.local);
    Get(j, "remote", v.remote);
    Get(j, "retransmits", v.retransmits);
    Get(j, "rtt_avg_us", v.rtt_avg_us);
    Get(j, "rtt_max_us", v.rtt_max_us);
    Get(j, "rtt_p50_us", v.rtt_p50_us);
    Get(j, "rtt_p99_us", v.rtt_p99_us);
    Get(j, "rtt_samples", v.rtt_samples);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const TcpStats& v) {
    builder.BeginObject();
    Put(builder, "active", v.active);
    builder.Key("conn_hist");
    WriteHistogram(builder, v.conn_rtt_hist);
    Put(builder, "conn_rtt_p50_us", v.conn_rtt_p50_us);
    Put(builder, "conn_rtt_p99_us", v.conn_rtt_p99_us);
    Put(builder, "conn_rtt_samples", v.conn_rtt_samples);
    builder.Key("hist");
    WriteHistogram(builder, v.rtt_hist);
    Put(builder, "reason", v.reason);
    Put(builder, "retransmits", v.retransmits);
    Put(builder, "retransmits_per_sec", v.retransmits_per_sec);
    Put(builder, "rtt_avg_us", v.rtt_avg_us);
    Put(builder, "rtt_max_us", v.rtt_max_us);
    Put(builder, "rtt_p50_us", v.rtt_p50_us);
    Put(builder, "rtt_p99_us", v.rtt_p99_us);
    Put(builder, "rtt_samples", v.rtt_samples);
    Put(builder, "rtt_source", v.rtt_source);
    Put(builder, "rtt_weighting", v.rtt_weighting);
    builder.Key("top");
    builder.BeginArray();
    for (const auto& item : v.top) {
        ToJson(builder, item);
    }
    builder.EndArray();
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, TcpStats& v) {
    Get(j, "active", v.active);
    Get(j, "reason", v.reason);
    Get(j, "rtt_source", v.rtt_source);
    Get(j, "retransmits", v.retransmits);
    Get(j, "retransmits_per_sec", v.retransmits_per_sec);
    Get(j, "rtt_samples", v.rtt_samples);
    ReadHistogram(j, v.rtt_hist);
    Get(j, "rtt_p50_us", v.rtt_p50_us);
    Get(j, "rtt_p99_us", v.rtt_p99_us);
    Get(j, "rtt_avg_us", v.rtt_avg_us);
    Get(j, "rtt_max_us", v.rtt_max_us);
    Get(j, "rtt_weighting", v.rtt_weighting);
    Get(j, "conn_rtt_samples", v.conn_rtt_samples);
    ReadHistogram(j, v.conn_rtt_hist, "conn_hist");
    Get(j, "conn_rtt_p50_us", v.conn_rtt_p50_us);
    Get(j, "conn_rtt_p99_us", v.conn_rtt_p99_us);
    ReadArray(j, "top", v.top,
              [](const go2cpp::json::Value& element, TcpConnStat& item) {
                  FromJson(element, item);
              });
}

void ToJson(go2cpp::json::JsonBuilder& builder, const FileIoTaskStat& v) {
    // pid_init_ns 是 pid 的显式别名：pid 的值来自 bpf_get_current_pid_tgid，
    // 是 init PID namespace 的 tgid；namespace_pid 才是 ps 可见的 pid。
    builder.BeginObject();
    Put(builder, "comm", v.comm);
    Put(builder, "namespace_pid", v.namespace_pid);
    Put(builder, "pid", v.pid);
    Put(builder, "pid_init_ns", v.pid);
    Put(builder, "read_avg_us", v.read_avg_us);
    Put(builder, "read_count", v.read_count);
    Put(builder, "read_max_us", v.read_max_us);
    Put(builder, "write_avg_us", v.write_avg_us);
    Put(builder, "write_count", v.write_count);
    Put(builder, "write_max_us", v.write_max_us);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, FileIoTaskStat& v) {
    // 新 JSON 同时带 pid_init_ns 与 pid，优先显式语义字段；旧 JSON 只有 pid。
    Get(j, "pid_init_ns", v.pid);
    if (v.pid == 0) {
        Get(j, "pid", v.pid);
    }
    Get(j, "namespace_pid", v.namespace_pid);
    Get(j, "comm", v.comm);
    Get(j, "read_count", v.read_count);
    Get(j, "write_count", v.write_count);
    Get(j, "read_avg_us", v.read_avg_us);
    Get(j, "read_max_us", v.read_max_us);
    Get(j, "write_avg_us", v.write_avg_us);
    Get(j, "write_max_us", v.write_max_us);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const FileIoStats& v) {
    builder.BeginObject();
    Put(builder, "active", v.active);
    Put(builder, "coverage", v.coverage);
    Put(builder, "coverage_gap", v.coverage_gap);
    Put(builder, "read_avg_us", v.read_avg_us);
    Put(builder, "read_count", v.read_count);
    builder.Key("read_hist");
    WriteHistogram(builder, v.read_hist);
    Put(builder, "read_iops", v.read_iops);
    Put(builder, "read_max_us", v.read_max_us);
    Put(builder, "read_p50_us", v.read_p50_us);
    Put(builder, "read_p99_us", v.read_p99_us);
    Put(builder, "readv_avg_us", v.readv_avg_us);
    Put(builder, "readv_count", v.readv_count);
    Put(builder, "readv_max_us", v.readv_max_us);
    Put(builder, "reason", v.reason);
    builder.Key("top");
    builder.BeginArray();
    for (const auto& item : v.top) {
        ToJson(builder, item);
    }
    builder.EndArray();
    Put(builder, "uring_read_avg_us", v.uring_read_avg_us);
    Put(builder, "uring_read_count", v.uring_read_count);
    Put(builder, "uring_read_max_us", v.uring_read_max_us);
    Put(builder, "uring_read_pending", v.uring_read_pending);
    Put(builder, "uring_write_avg_us", v.uring_write_avg_us);
    Put(builder, "uring_write_count", v.uring_write_count);
    Put(builder, "uring_write_max_us", v.uring_write_max_us);
    Put(builder, "uring_write_pending", v.uring_write_pending);
    Put(builder, "write_avg_us", v.write_avg_us);
    Put(builder, "write_count", v.write_count);
    builder.Key("write_hist");
    WriteHistogram(builder, v.write_hist);
    Put(builder, "write_iops", v.write_iops);
    Put(builder, "write_max_us", v.write_max_us);
    Put(builder, "write_p50_us", v.write_p50_us);
    Put(builder, "write_p99_us", v.write_p99_us);
    Put(builder, "writev_avg_us", v.writev_avg_us);
    Put(builder, "writev_count", v.writev_count);
    Put(builder, "writev_max_us", v.writev_max_us);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, FileIoStats& v) {
    Get(j, "active", v.active);
    Get(j, "reason", v.reason);
    Get(j, "read_count", v.read_count);
    Get(j, "write_count", v.write_count);
    ReadHistogram(j, v.read_hist, "read_hist");
    ReadHistogram(j, v.write_hist, "write_hist");
    Get(j, "read_p50_us", v.read_p50_us);
    Get(j, "read_p99_us", v.read_p99_us);
    Get(j, "read_avg_us", v.read_avg_us);
    Get(j, "read_max_us", v.read_max_us);
    Get(j, "write_p50_us", v.write_p50_us);
    Get(j, "write_p99_us", v.write_p99_us);
    Get(j, "write_avg_us", v.write_avg_us);
    Get(j, "write_max_us", v.write_max_us);
    Get(j, "read_iops", v.read_iops);
    Get(j, "write_iops", v.write_iops);
    Get(j, "readv_count", v.readv_count);
    Get(j, "writev_count", v.writev_count);
    Get(j, "readv_avg_us", v.readv_avg_us);
    Get(j, "readv_max_us", v.readv_max_us);
    Get(j, "writev_avg_us", v.writev_avg_us);
    Get(j, "writev_max_us", v.writev_max_us);
    Get(j, "uring_read_count", v.uring_read_count);
    Get(j, "uring_write_count", v.uring_write_count);
    Get(j, "uring_read_pending", v.uring_read_pending);
    Get(j, "uring_write_pending", v.uring_write_pending);
    Get(j, "uring_read_avg_us", v.uring_read_avg_us);
    Get(j, "uring_read_max_us", v.uring_read_max_us);
    Get(j, "uring_write_avg_us", v.uring_write_avg_us);
    Get(j, "uring_write_max_us", v.uring_write_max_us);
    Get(j, "coverage", v.coverage);
    Get(j, "coverage_gap", v.coverage_gap);
    ReadArray(j, "top", v.top,
              [](const go2cpp::json::Value& element, FileIoTaskStat& item) {
                  FromJson(element, item);
              });
}

void ToJson(go2cpp::json::JsonBuilder& builder, const HostInfo& v) {
    builder.BeginObject();
    Put(builder, "arch", v.arch);
    Put(builder, "hostname", v.hostname);
    Put(builder, "kernel", v.kernel);
    Put(builder, "os", v.os);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, HostInfo& v) {
    Get(j, "arch", v.arch);
    Get(j, "hostname", v.hostname);
    Get(j, "kernel", v.kernel);
    Get(j, "os", v.os);
}

void ToJson(go2cpp::json::JsonBuilder& builder, const Snapshot& v) {
    builder.BeginObject();
    builder.Key("cpus");
    builder.BeginArray();
    for (const auto& item : v.cpus) {
        ToJson(builder, item);
    }
    builder.EndArray();
    builder.Key("disks");
    builder.BeginArray();
    for (const auto& item : v.disks) {
        ToJson(builder, item);
    }
    builder.EndArray();
    Put(builder, "ebpf_active", v.ebpf_active);
    builder.Key("ebpf_nets");
    builder.BeginArray();
    for (const auto& item : v.ebpf_nets) {
        ToJson(builder, item);
    }
    builder.EndArray();
    Put(builder, "ebpf_reason", v.ebpf_reason);
    builder.Key("fileio");
    ToJson(builder, v.fileio);
    builder.Key("futex");
    ToJson(builder, v.futex);
    Put(builder, "host", v.host);
    builder.Key("host_info");
    ToJson(builder, v.host_info);
    builder.Key("load");
    ToJson(builder, v.load);
    builder.Key("mem");
    ToJson(builder, v.mem);
    builder.Key("nets");
    builder.BeginArray();
    for (const auto& item : v.nets) {
        ToJson(builder, item);
    }
    builder.EndArray();
    builder.Key("sched");
    ToJson(builder, v.sched);
    builder.Key("syscalls");
    ToJson(builder, v.syscalls);
    builder.Key("tcp");
    ToJson(builder, v.tcp);
    Put(builder, "timestamp_ms", v.timestamp_ms);
    builder.EndObject();
}

void FromJson(const go2cpp::json::Value& j, Snapshot& v) {
    Get(j, "host", v.host);
    Get(j, "timestamp_ms", v.timestamp_ms);
    FromJson(j["host_info"], v.host_info);
    FromJson(j["load"], v.load);
    ReadArray(j, "cpus", v.cpus,
              [](const go2cpp::json::Value& element, CpuCore& item) {
                  FromJson(element, item);
              });
    FromJson(j["mem"], v.mem);
    ReadArray(j, "disks", v.disks,
              [](const go2cpp::json::Value& element, DiskStat& item) {
                  FromJson(element, item);
              });
    ReadArray(j, "nets", v.nets,
              [](const go2cpp::json::Value& element, NetDevStat& item) {
                  FromJson(element, item);
              });
    ReadArray(j, "ebpf_nets", v.ebpf_nets,
              [](const go2cpp::json::Value& element, NetEbpfStat& item) {
                  FromJson(element, item);
              });
    Get(j, "ebpf_active", v.ebpf_active);
    Get(j, "ebpf_reason", v.ebpf_reason);
    FromJson(j["syscalls"], v.syscalls);
    FromJson(j["sched"], v.sched);
    FromJson(j["futex"], v.futex);
    FromJson(j["tcp"], v.tcp);
    FromJson(j["fileio"], v.fileio);
}

std::string SnapshotToJson(const Snapshot& snapshot) {
    go2cpp::json::JsonBuilder builder;
    ToJson(builder, snapshot);
    return builder.Take();
}

bool SnapshotFromJson(const std::string& text, Snapshot& snapshot,
                      std::string* error) {
    go2cpp::json::Document document;
    go2cpp::json::Parser parser;
    ErrorPtr parse_error;
    if (!parser.Parse(text, document, parse_error)) {
        if (error != nullptr) {
            *error = ErrorMessage(parse_error);
        }
        return false;
    }
    if (!document.Root().IsObject()) {
        if (error != nullptr) {
            *error = "JSON 根值不是对象";
        }
        return false;
    }
    snapshot = Snapshot{};
    FromJson(document.Root(), snapshot);
    return true;
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
