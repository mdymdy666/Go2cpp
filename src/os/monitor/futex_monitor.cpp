#include "go2cpp/os/monitor/futex_monitor.hpp"

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
extern const unsigned char go2cpp_futex_latency_elf[];
extern const unsigned long go2cpp_futex_latency_elf_len;
}
#endif

namespace go2cpp::os::monitor {
namespace {

/// 与 futex_latency.bpf.c 的 struct futex_task 逐字节一致。
struct FutexTaskRaw {
    char comm[16];
    std::uint64_t wait_count;  // 仅 ret==0 的睡眠样本
    std::uint64_t wait_ns;
    std::uint64_t max_wait_ns;
    std::uint64_t wake_count;
    std::uint64_t eagain_count;
    std::uint64_t timedout_count;
    std::uint64_t error_count;
};

/// 与 futex_latency.bpf.c 的 struct futex_global 逐字节一致。
struct FutexGlobalRaw {
    std::uint64_t wait_count;
    std::uint64_t wait_ns;
    std::uint64_t max_wait_ns;
    std::uint64_t wake_count;
    std::uint64_t eagain_count;
    std::uint64_t timedout_count;
    std::uint64_t error_count;
    std::uint64_t hist[kKernelHistBuckets];
};

static_assert(sizeof(FutexTaskRaw) == 72,
              "FutexTaskRaw layout must match the eBPF map value");
static_assert(sizeof(FutexGlobalRaw) ==
                  sizeof(std::uint64_t) * (7 + kKernelHistBuckets),
              "FutexGlobalRaw layout must match the eBPF map value");

std::string ReadComm(const char (&raw)[16]) {
    return std::string(raw, ::strnlen(raw, sizeof(raw)));
}

}  // namespace

struct FutexMonitor::Impl {
    explicit Impl(std::size_t n) : top_n(n) {}
    ~Impl() { StopInternal(); }

    void Init() {
#if defined(GO2CPP_HAVE_EBPF)
        if (!detail::KernelBpfUsable()) {
            reason = detail::KernelBpfUnusableReason();
            return;
        }
        std::string error;
        if (!object.Load(go2cpp_futex_latency_elf, go2cpp_futex_latency_elf_len,
                         &error)) {
            reason = "eBPF load failed: " + error;
            return;
        }
        global_fd = object.MapFd("futex_global_map");
        task_fd = object.MapFd("futex_task_map");
        if (global_fd < 0 || task_fd < 0) {
            reason = "embedded BPF object lacks futex_global_map/futex_task_map";
            object.Close();
            return;
        }
        if (!object.AttachTracepoint("futex_enter", "syscalls",
                                     "sys_enter_futex", &error) ||
            !object.AttachTracepoint("futex_exit", "syscalls", "sys_exit_futex",
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

FutexMonitor::FutexMonitor(std::size_t top_n)
    : impl_(std::make_unique<Impl>(top_n)) {
    impl_->Init();
}

FutexMonitor::~FutexMonitor() = default;

Status FutexMonitor::UpdateOnce(Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->available) {
        snapshot.futex = FutexStats{};
        snapshot.futex.reason = impl_->reason;
        return Status::kUnsupported;
    }

    FutexGlobalRaw global{};
    const bool global_ok =
        detail::ReadArrayMap(impl_->global_fd, 0, &global);

    FutexStats stats;
    stats.active = true;
    if (global_ok) {
        stats.wait_count = global.wait_count;
        stats.wake_count = global.wake_count;
        stats.eagain_count = global.eagain_count;
        stats.timedout_count = global.timedout_count;
        stats.error_count = global.error_count;
        for (std::size_t i = 0; i < stats.hist.size(); ++i) {
            stats.hist[i] = global.hist[i];
        }
        if (global.wait_count > 0) {
            stats.wait_avg_us =
                static_cast<double>(global.wait_ns) / 1000.0 /
                static_cast<double>(global.wait_count);
        }
        if (global.wake_count > 0) {
            stats.wait_wake_ratio =
                static_cast<double>(global.wait_count) /
                static_cast<double>(global.wake_count);
        }
        stats.wait_max_us = static_cast<double>(global.max_wait_ns) / 1000.0;
        stats.wait_p50_us =
            KernelHistPercentile(stats.hist, stats.wait_count, 0.50);
        stats.wait_p99_us =
            KernelHistPercentile(stats.hist, stats.wait_count, 0.99);
    }

    std::vector<FutexTaskStat> tasks;
    for (const auto& row :
         detail::ReadHashMap<std::uint32_t, FutexTaskRaw>(impl_->task_fd)) {
        const FutexTaskRaw& raw = row.second;
        // 只 EAGAIN 的进程也要保留：它正是「wait 调用很多但没有真正睡眠」
        // 的争用/自旋信号，过滤掉会让 top 丢失这一类负载。
        if (raw.wait_count + raw.wake_count + raw.eagain_count +
                raw.timedout_count + raw.error_count ==
            0) {
            continue;
        }
        FutexTaskStat task;
        task.pid = row.first;
        task.comm = ReadComm(raw.comm);
        task.wait_count = raw.wait_count;
        task.wake_count = raw.wake_count;
        task.eagain_count = raw.eagain_count;
        task.timedout_count = raw.timedout_count;
        task.error_count = raw.error_count;
        if (raw.wait_count > 0) {
            task.wait_avg_us =
                static_cast<double>(raw.wait_ns) / 1000.0 /
                static_cast<double>(raw.wait_count);
        }
        task.wait_max_us = static_cast<double>(raw.max_wait_ns) / 1000.0;
        tasks.push_back(std::move(task));
    }
    std::sort(tasks.begin(), tasks.end(),
              [](const FutexTaskStat& left, const FutexTaskStat& right) {
                  if (left.wait_count != right.wait_count) {
                      return left.wait_count > right.wait_count;
                  }
                  const std::uint64_t left_other =
                      left.eagain_count + left.timedout_count + left.error_count;
                  const std::uint64_t right_other =
                      right.eagain_count + right.timedout_count +
                      right.error_count;
                  if (left_other != right_other) {
                      return left_other > right_other;
                  }
                  return left.wait_max_us > right.wait_max_us;
              });
    if (tasks.size() > impl_->top_n) {
        tasks.resize(impl_->top_n);
    }
    stats.top = std::move(tasks);
    snapshot.futex = std::move(stats);
    ++impl_->samples;
    return Status::kOk;
}

void FutexMonitor::Stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->StopInternal();
}

bool FutexMonitor::available() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->available;
}

std::string FutexMonitor::reason() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->reason;
}

std::uint64_t FutexMonitor::sample_count() const noexcept {
    return impl_->samples.load(std::memory_order_relaxed);
}

void FutexMonitor::ProgramCost(std::uint64_t* run_time_ns,
                               std::uint64_t* run_cnt) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->object.ProgramCost(run_time_ns, run_cnt);
}

}  // namespace go2cpp::os::monitor
