#include "go2cpp/os/monitor/tcp_monitor.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "kernel_ebpf.hpp"

#if defined(GO2CPP_HAVE_EBPF)
// 由 CMake 的 embed_bpf.cmake 在构建期生成，运行时不再依赖 .bpf.o 文件。
extern "C" {
extern const unsigned char go2cpp_tcp_monitor_elf[];
extern const unsigned long go2cpp_tcp_monitor_elf_len;
}
#endif

namespace go2cpp::os::monitor {
namespace {

/// 与 tcp_monitor.bpf.c 的 struct tcp_conn_key 逐字节一致。
struct TcpConnKeyRaw {
    std::uint32_t saddr[4];
    std::uint32_t daddr[4];
    std::uint16_t sport;
    std::uint16_t dport;
    std::uint16_t family;
    std::uint16_t pad;
};

/// 与 tcp_monitor.bpf.c 的 struct tcp_conn 逐字节一致。
struct TcpConnRaw {
    std::uint64_t retrans;
    std::uint64_t rtt_count;
    std::uint64_t rtt_us_sum;
    std::uint32_t rtt_max_us;
    std::uint32_t pad;
    std::uint64_t rtt_hist[kKernelHistBuckets];
};

/// 与 tcp_monitor.bpf.c 的 struct tcp_global 逐字节一致。
struct TcpGlobalRaw {
    std::uint64_t retrans;
    std::uint64_t rtt_count;
    std::uint64_t rtt_us_sum;
    std::uint32_t rtt_max_us;
    std::uint32_t pad;
    std::uint64_t rtt_hist[kKernelHistBuckets];
    std::uint64_t probe_seq;
    // 连接加权直方图：每个连接只在首次被采样时写一个样本。
    std::uint64_t conn_rtt_count;
    std::uint64_t conn_rtt_hist[kKernelHistBuckets];
};

static_assert(sizeof(TcpConnKeyRaw) == 40,
              "TcpConnKeyRaw layout must match the eBPF map key");
static_assert(sizeof(TcpConnRaw) == sizeof(std::uint64_t) * (4 + kKernelHistBuckets),
              "TcpConnRaw layout must match the eBPF map value");
// 5 个基础字段（retrans/rtt_count/rtt_us_sum/rtt_max+pad）+ 32 桶样本
// 直方图 + probe_seq + 连接加权 count + 32 桶连接直方图。
static_assert(sizeof(TcpGlobalRaw) ==
                  sizeof(std::uint64_t) * (5 + kKernelHistBuckets + 1 +
                                           kKernelHistBuckets),
              "TcpGlobalRaw layout must match the eBPF map value");

/// family 数字转可读字符串。
std::string FamilyName(std::uint16_t family) {
    switch (family) {
        case 2:  // AF_INET
            return "ipv4";
        case 10:  // AF_INET6
            return "ipv6";
        default:
            return "family_" + std::to_string(family);
    }
}

/// 把内核原样保存的地址字节格式化成可读字符串。
std::string FormatAddress(const std::uint32_t addr[4], std::uint16_t family) {
    char buffer[INET6_ADDRSTRLEN] = {};
    if (family == 2) {
        struct in_addr v4 {};
        std::memcpy(&v4, &addr[0], sizeof(v4));
        if (::inet_ntop(AF_INET, &v4, buffer, sizeof(buffer)) == nullptr) {
            return "0.0.0.0";
        }
    } else if (family == 10) {
        struct in6_addr v6 {};
        std::memcpy(&v6, addr, sizeof(v6));
        if (::inet_ntop(AF_INET6, &v6, buffer, sizeof(buffer)) == nullptr) {
            return "::";
        }
    } else {
        return "unknown";
    }
    return buffer;
}

}  // namespace

struct TcpMonitor::Impl {
    explicit Impl(std::size_t n) : top_n(n) {}
    ~Impl() { StopInternal(); }

    void Init() {
#if defined(GO2CPP_HAVE_EBPF)
        if (!detail::KernelBpfUsable()) {
            reason = detail::KernelBpfUnusableReason();
            return;
        }
        std::string error;
        if (!object.Load(go2cpp_tcp_monitor_elf, go2cpp_tcp_monitor_elf_len,
                         &error)) {
            reason = "eBPF load failed: " + error;
            return;
        }
        global_fd = object.MapFd("tcp_global_map");
        conn_fd = object.MapFd("tcp_conn_map");
        if (global_fd < 0 || conn_fd < 0) {
            reason = "embedded BPF object lacks tcp_global_map/tcp_conn_map";
            object.Close();
            return;
        }
        if (!object.AttachTracepoint("tcp_retransmit", "tcp",
                                     "tcp_retransmit_skb", &error)) {
            reason = "tracepoint tcp_retransmit_skb attach failed: " + error;
            object.Close();
            global_fd = -1;
            conn_fd = -1;
            return;
        }
        available = true;
        // 部分裁剪内核没有 tcp_probe tracepoint（上游 5.15 移除的是同名
        // 的 tcp_probe 模块，tracepoint 仍保留）。此时只降级 RTT，重传
        // 统计继续工作（见头文件说明）。
        if (!object.AttachTracepoint("tcp_rtt_probe", "tcp", "tcp_probe",
                                     &error)) {
            rtt_source = "tcp_probe unavailable: " + error;
        } else {
            rtt_source = "tcp_probe";
        }
        reason.clear();
#else
        reason = detail::KernelBpfUnusableReason();
#endif
    }

    void StopInternal() {
        object.Close();
        global_fd = -1;
        conn_fd = -1;
        available = false;
        reason = "stopped";
    }

    bool available{false};
    std::string reason;
    std::string rtt_source;
    detail::KernelBpfObject object;
    int global_fd{-1};
    int conn_fd{-1};
    std::size_t top_n{10};
    /// UpdateOnce 成功次数。sample_count() 不加锁读取，必须是原子量，
    /// 否则与 UpdateOnce 的 ++samples 构成数据竞争。
    std::atomic<std::uint64_t> samples{0};
    /// 重传速率的差分基线。
    bool has_previous{false};
    std::chrono::steady_clock::time_point previous_time{};
    std::uint64_t previous_retransmits{0};
    mutable std::mutex mutex;
};

TcpMonitor::TcpMonitor(std::size_t top_n)
    : impl_(std::make_unique<Impl>(top_n)) {
    impl_->Init();
}

TcpMonitor::~TcpMonitor() = default;

Status TcpMonitor::UpdateOnce(Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->available) {
        snapshot.tcp = TcpStats{};
        snapshot.tcp.reason = impl_->reason;
        return Status::kUnsupported;
    }
    TcpStats stats;
    stats.active = true;
    stats.rtt_source = impl_->rtt_source;

    TcpGlobalRaw global {};
    const bool global_ok =
        detail::ReadArrayMap(impl_->global_fd, 0U, &global);
    if (global_ok) {
        stats.retransmits = global.retrans;
        stats.rtt_samples = global.rtt_count;
        for (std::size_t i = 0; i < stats.rtt_hist.size(); ++i) {
            stats.rtt_hist[i] = global.rtt_hist[i];
        }
        if (global.rtt_count > 0) {
            stats.rtt_avg_us = static_cast<double>(global.rtt_us_sum) /
                               static_cast<double>(global.rtt_count);
        }
        stats.rtt_max_us = static_cast<double>(global.rtt_max_us);
        stats.rtt_p50_us =
            KernelHistPercentile(stats.rtt_hist, stats.rtt_samples, 0.50);
        stats.rtt_p99_us =
            KernelHistPercentile(stats.rtt_hist, stats.rtt_samples, 0.99);

        stats.conn_rtt_samples = global.conn_rtt_count;
        for (std::size_t i = 0; i < stats.conn_rtt_hist.size(); ++i) {
            stats.conn_rtt_hist[i] = global.conn_rtt_hist[i];
        }
        stats.conn_rtt_p50_us = KernelHistPercentile(
            stats.conn_rtt_hist, stats.conn_rtt_samples, 0.50);
        stats.conn_rtt_p99_us = KernelHistPercentile(
            stats.conn_rtt_hist, stats.conn_rtt_samples, 0.99);
    }

    // 重传速率：两次采样之间的差分。首采样没有基线，速率保持 0。
    // 只有成功读到全局计数时才推进基线：若本次 lookup 失败就把基线清零，
    // 下一次成功会得到「全部历史重传 / 一个窗口」的假尖峰。
    const auto now = std::chrono::steady_clock::now();
    if (global_ok) {
        if (impl_->has_previous) {
            const double elapsed =
                std::chrono::duration<double>(now - impl_->previous_time)
                    .count();
            if (elapsed > 0.0 &&
                stats.retransmits >= impl_->previous_retransmits) {
                stats.retransmits_per_sec =
                    static_cast<double>(stats.retransmits -
                                        impl_->previous_retransmits) /
                    elapsed;
            }
        }
        impl_->previous_time = now;
        impl_->previous_retransmits = stats.retransmits;
        impl_->has_previous = true;
    }

    std::vector<TcpConnStat> items;
    const auto rows =
        detail::ReadHashMap<TcpConnKeyRaw, TcpConnRaw>(impl_->conn_fd);
    items.reserve(rows.size());
    for (const auto& row : rows) {
        const TcpConnKeyRaw& key = row.first;
        const TcpConnRaw& raw = row.second;
        if (raw.retrans == 0 && raw.rtt_count == 0) {
            continue;
        }
        TcpConnStat item;
        item.family = FamilyName(key.family);
        item.local = FormatAddress(key.saddr, key.family) + ":" +
                     std::to_string(key.sport);
        item.remote = FormatAddress(key.daddr, key.family) + ":" +
                      std::to_string(key.dport);
        item.retransmits = raw.retrans;
        item.rtt_samples = raw.rtt_count;
        if (raw.rtt_count > 0) {
            item.rtt_avg_us = static_cast<double>(raw.rtt_us_sum) /
                              static_cast<double>(raw.rtt_count);
        }
        item.rtt_max_us = static_cast<double>(raw.rtt_max_us);
        KernelHistogram hist {};
        for (std::size_t i = 0; i < hist.size(); ++i) {
            hist[i] = raw.rtt_hist[i];
        }
        item.rtt_p50_us = KernelHistPercentile(hist, raw.rtt_count, 0.50);
        item.rtt_p99_us = KernelHistPercentile(hist, raw.rtt_count, 0.99);
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(),
              [](const TcpConnStat& left, const TcpConnStat& right) {
                  if (left.retransmits != right.retransmits) {
                      return left.retransmits > right.retransmits;
                  }
                  if (left.rtt_samples != right.rtt_samples) {
                      return left.rtt_samples > right.rtt_samples;
                  }
                  return left.local < right.local;
              });
    if (items.size() > impl_->top_n) {
        items.resize(impl_->top_n);
    }
    stats.top = std::move(items);
    snapshot.tcp = std::move(stats);
    ++impl_->samples;
    return Status::kOk;
}

void TcpMonitor::Stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->StopInternal();
}

bool TcpMonitor::available() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->available;
}

std::string TcpMonitor::reason() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->reason;
}

std::uint64_t TcpMonitor::sample_count() const noexcept {
    return impl_->samples.load(std::memory_order_relaxed);
}

void TcpMonitor::ProgramCost(std::uint64_t* run_time_ns,
                             std::uint64_t* run_cnt) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->object.ProgramCost(run_time_ns, run_cnt);
}

}  // namespace go2cpp::os::monitor
