#include "go2cpp/os/monitor/sched_monitor.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "kernel_ebpf.hpp"

#if defined(GO2CPP_HAVE_EBPF)
extern "C" {
extern const unsigned char go2cpp_sched_latency_elf[];
extern const unsigned long go2cpp_sched_latency_elf_len;
}
#endif

namespace go2cpp::os::monitor {
namespace {

/// 与 sched_latency.bpf.c 的 struct sched_task 逐字节一致。
struct SchedTaskRaw {
    char comm[16];
    std::uint64_t on_cpu_ns;
    std::uint64_t switches;
    std::uint64_t wait_ns;
    std::uint64_t wait_count;
    std::uint64_t max_wait_ns;
};

/// 与 sched_latency.bpf.c 的 struct sched_global 逐字节一致。
struct SchedGlobalRaw {
    std::uint64_t switches;
    std::uint64_t wait_count;
    std::uint64_t wait_ns;
    std::uint64_t max_wait_ns;
    std::uint64_t hist[kKernelHistBuckets];
};

static_assert(sizeof(SchedTaskRaw) == 56,
              "SchedTaskRaw layout must match the eBPF map value");
static_assert(sizeof(SchedGlobalRaw) ==
                  sizeof(std::uint64_t) * (4 + kKernelHistBuckets),
              "SchedGlobalRaw layout must match the eBPF map value");

/// tracepoint 的 comm 字段不保证以 NUL 结尾，按定长截断。
std::string ReadComm(const char (&raw)[16]) {
    return std::string(raw, ::strnlen(raw, sizeof(raw)));
}

}  // namespace

struct SchedMonitor::Impl {
    explicit Impl(std::size_t n) : top_n(n) {}
    ~Impl() { StopInternal(); }

    void Init() {
#if defined(GO2CPP_HAVE_EBPF)
        if (!detail::KernelBpfUsable()) {
            reason = detail::KernelBpfUnusableReason();
            return;
        }
        std::string error;
        if (!object.Load(go2cpp_sched_latency_elf, go2cpp_sched_latency_elf_len,
                         &error)) {
            reason = "eBPF load failed: " + error;
            return;
        }
        global_fd = object.MapFd("sched_global_map");
        task_fd = object.MapFd("sched_task_map");
        if (global_fd < 0 || task_fd < 0) {
            reason = "embedded BPF object lacks sched_global_map/sched_task_map";
            object.Close();
            return;
        }
        if (!object.AttachTracepoint("sched_switch", "sched", "sched_switch",
                                     &error) ||
            !object.AttachTracepoint("sched_wakeup", "sched", "sched_wakeup",
                                     &error)) {
            reason = "tracepoint attach failed: " + error;
            object.Close();
            global_fd = -1;
            task_fd = -1;
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
        global_fd = -1;
        task_fd = -1;
        available = false;
        reason = "stopped";
    }

    bool available{false};
    std::string reason;
    detail::KernelBpfObject object;
    int global_fd{-1};
    int task_fd{-1};
    std::size_t top_n{10};
    /// UpdateOnce 成功次数。sample_count() 不加锁读取，必须是原子量，
    /// 否则与 UpdateOnce 的 ++samples 构成数据竞争。
    std::atomic<std::uint64_t> samples{0};
    mutable std::mutex mutex;
};

SchedMonitor::SchedMonitor(std::size_t top_n)
    : impl_(std::make_unique<Impl>(top_n)) {
    impl_->Init();
}

SchedMonitor::~SchedMonitor() = default;

Status SchedMonitor::UpdateOnce(Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->available) {
        snapshot.sched = SchedStats{};
        snapshot.sched.reason = impl_->reason;
        return Status::kUnsupported;
    }

    SchedGlobalRaw global{};
    const bool global_ok =
        detail::ReadArrayMap(impl_->global_fd, 0, &global);

    SchedStats stats;
    stats.active = true;
    if (global_ok) {
        stats.switches = global.switches;
        stats.wait_count = global.wait_count;
        for (std::size_t i = 0; i < stats.hist.size(); ++i) {
            stats.hist[i] = global.hist[i];
        }
        if (global.wait_count > 0) {
            stats.wait_avg_us =
                static_cast<double>(global.wait_ns) / 1000.0 /
                static_cast<double>(global.wait_count);
        }
        stats.wait_max_us = static_cast<double>(global.max_wait_ns) / 1000.0;
        stats.wait_p50_us =
            KernelHistPercentile(stats.hist, stats.wait_count, 0.50);
        stats.wait_p99_us =
            KernelHistPercentile(stats.hist, stats.wait_count, 0.99);
    }

    std::vector<SchedTaskStat> tasks;
    for (const auto& row :
         detail::ReadHashMap<std::uint32_t, SchedTaskRaw>(impl_->task_fd)) {
        const SchedTaskRaw& raw = row.second;
        if (raw.switches == 0 && raw.wait_count == 0) {
            continue;
        }
        SchedTaskStat task;
        task.pid = row.first;
        task.comm = ReadComm(raw.comm);
        task.switches = raw.switches;
        task.on_cpu_ms = static_cast<double>(raw.on_cpu_ns) / 1e6;
        task.wait_count = raw.wait_count;
        if (raw.wait_count > 0) {
            task.wait_avg_us =
                static_cast<double>(raw.wait_ns) / 1000.0 /
                static_cast<double>(raw.wait_count);
        }
        task.wait_max_us = static_cast<double>(raw.max_wait_ns) / 1000.0;
        tasks.push_back(std::move(task));
    }
    // 热点任务按「等待样本数」降序，等待次数相同再看最大等待。
    std::sort(tasks.begin(), tasks.end(),
              [](const SchedTaskStat& left, const SchedTaskStat& right) {
                  if (left.wait_count != right.wait_count) {
                      return left.wait_count > right.wait_count;
                  }
                  return left.wait_max_us > right.wait_max_us;
              });
    if (tasks.size() > impl_->top_n) {
        tasks.resize(impl_->top_n);
    }
    stats.top = std::move(tasks);
    snapshot.sched = std::move(stats);
    ++impl_->samples;
    return Status::kOk;
}

void SchedMonitor::Stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->StopInternal();
}

bool SchedMonitor::available() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->available;
}

std::string SchedMonitor::reason() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->reason;
}

std::uint64_t SchedMonitor::sample_count() const noexcept {
    return impl_->samples.load(std::memory_order_relaxed);
}

void SchedMonitor::ProgramCost(std::uint64_t* run_time_ns,
                               std::uint64_t* run_cnt) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->object.ProgramCost(run_time_ns, run_cnt);
}

}  // namespace go2cpp::os::monitor
