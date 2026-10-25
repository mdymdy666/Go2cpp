#include "go2cpp/os/monitor/syscall_monitor.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "kernel_ebpf.hpp"

#if defined(GO2CPP_HAVE_EBPF)
// 由 CMake 的 embed_bpf.cmake 在构建期生成，运行时不再依赖 .bpf.o 文件。
extern "C" {
extern const unsigned char go2cpp_syscall_latency_elf[];
extern const unsigned long go2cpp_syscall_latency_elf_len;
}
#endif

namespace go2cpp::os::monitor {
namespace {

#include "syscall_names.inc"

/// 与 syscall_latency.bpf.c 的 GO2CPP_MAX_SYSCALL_ID 保持一致。
constexpr std::uint32_t kMaxSyscallId = 512;

/// syscall 名查询：先查内置 x86_64 表，未知编号回落为 syscall_<id>。
std::string SyscallName(std::uint32_t id) {
    if (id < kSyscallNameCount && kSyscallNames[id] != nullptr) {
        return kSyscallNames[id];
    }
    return "syscall_" + std::to_string(id);
}

/// 与 syscall_latency.bpf.c 的 struct syscall_stat 逐字节一致。
struct SyscallStatRaw {
    std::uint64_t count;
    std::uint64_t total_ns;
    std::uint64_t max_ns;
    std::uint64_t hist[kKernelHistBuckets];
};

static_assert(sizeof(SyscallStatRaw) ==
                  sizeof(std::uint64_t) * (3 + kKernelHistBuckets),
              "SyscallStatRaw layout must match the eBPF map value");

}  // namespace

struct SyscallMonitor::Impl {
    explicit Impl(std::size_t n) : top_n(n) {}
    ~Impl() { StopInternal(); }

    void Init() {
#if defined(GO2CPP_HAVE_EBPF)
        if (!detail::KernelBpfUsable()) {
            reason = detail::KernelBpfUnusableReason();
            return;
        }
        std::string error;
        if (!object.Load(go2cpp_syscall_latency_elf,
                         go2cpp_syscall_latency_elf_len, &error)) {
            reason = "eBPF load failed: " + error;
            return;
        }
        stat_fd = object.MapFd("syscall_stat_map");
        if (stat_fd < 0) {
            reason = "embedded BPF object lacks syscall_stat_map";
            object.Close();
            return;
        }
        if (!object.AttachTracepoint("syscall_enter", "raw_syscalls",
                                     "sys_enter", &error) ||
            !object.AttachTracepoint("syscall_exit", "raw_syscalls", "sys_exit",
                                     &error)) {
            reason = "tracepoint attach failed: " + error;
            object.Close();
            stat_fd = -1;
            return;
        }
        available = true;
        reason.clear();
#else
        reason = detail::KernelBpfUnusableReason();
#endif
    }

    void StopInternal() {
        object.Close();
        stat_fd = -1;
        available = false;
        reason = "stopped";
    }

    bool available{false};
    std::string reason;
    detail::KernelBpfObject object;
    int stat_fd{-1};
    std::size_t top_n{10};
    /// UpdateOnce 成功次数。sample_count() 不加锁读取，必须是原子量，
    /// 否则与 UpdateOnce 的 ++samples 构成数据竞争。
    std::atomic<std::uint64_t> samples{0};
    mutable std::mutex mutex;
};

SyscallMonitor::SyscallMonitor(std::size_t top_n)
    : impl_(std::make_unique<Impl>(top_n)) {
    impl_->Init();
}

SyscallMonitor::~SyscallMonitor() = default;

Status SyscallMonitor::UpdateOnce(Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->available) {
        snapshot.syscalls = SyscallStats{};
        snapshot.syscalls.reason = impl_->reason;
        return Status::kUnsupported;
    }
    SyscallStats stats;
    stats.active = true;
    std::vector<SyscallLatency> items;
    items.reserve(64);
    // syscall_stat_map 是 ARRAY[512]：按 id 直接索引，读取全部 512 项。
    // 每次 lookup 是一次 bpf syscall（约 1µs），5Hz 下总开销约 0.25% CPU；
    // 换来的是内核侧每次 syscall 少一次 HASH 查找与插入竞争。
    for (std::uint32_t id = 0; id < kMaxSyscallId; ++id) {
        SyscallStatRaw raw{};
        if (!detail::ReadArrayMap(impl_->stat_fd, id, &raw)) {
            continue;
        }
        if (raw.count == 0) {
            continue;
        }
        KernelHistogram hist{};
        for (std::size_t i = 0; i < hist.size(); ++i) {
            hist[i] = raw.hist[i];
            stats.hist[i] += raw.hist[i];
        }
        SyscallLatency item;
        item.id = id;
        item.name = SyscallName(item.id);
        item.count = raw.count;
        item.avg_us =
            static_cast<double>(raw.total_ns) / 1000.0 /
            static_cast<double>(raw.count);
        item.p50_us = KernelHistPercentile(hist, raw.count, 0.50);
        item.p99_us = KernelHistPercentile(hist, raw.count, 0.99);
        item.max_us = static_cast<double>(raw.max_ns) / 1000.0;
        stats.total += raw.count;
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(),
              [](const SyscallLatency& left, const SyscallLatency& right) {
                  if (left.count != right.count) {
                      return left.count > right.count;
                  }
                  return left.id < right.id;
              });
    if (items.size() > impl_->top_n) {
        items.resize(impl_->top_n);
    }
    stats.top = std::move(items);
    snapshot.syscalls = std::move(stats);
    ++impl_->samples;
    return Status::kOk;
}

void SyscallMonitor::Stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->StopInternal();
}

bool SyscallMonitor::available() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->available;
}

std::string SyscallMonitor::reason() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->reason;
}

std::uint64_t SyscallMonitor::sample_count() const noexcept {
    return impl_->samples.load(std::memory_order_relaxed);
}

void SyscallMonitor::ProgramCost(std::uint64_t* run_time_ns,
                                 std::uint64_t* run_cnt) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->object.ProgramCost(run_time_ns, run_cnt);
}

}  // namespace go2cpp::os::monitor
