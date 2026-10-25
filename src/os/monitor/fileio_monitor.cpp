#include "go2cpp/os/monitor/fileio_monitor.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "kernel_ebpf.hpp"

#if defined(GO2CPP_HAVE_EBPF)
// 由 CMake 的 embed_bpf.cmake 在构建期生成，运行时不再依赖 .bpf.o 文件。
extern "C" {
extern const unsigned char go2cpp_fileio_monitor_elf[];
extern const unsigned long go2cpp_fileio_monitor_elf_len;
}
#endif

namespace go2cpp::os::monitor {
namespace {

/// 与 fileio_monitor.bpf.c 的 struct fileio_task 逐字节一致。
struct FileIoTaskRaw {
    char comm[16];
    std::uint64_t read_count;
    std::uint64_t read_ns;
    std::uint64_t read_max_ns;
    std::uint64_t write_count;
    std::uint64_t write_ns;
    std::uint64_t write_max_ns;
    std::uint32_t ns_pid;  // 采集端当前 ns 的 tgid；0 = 未解析
    std::uint32_t pad;
};

/// 与 fileio_monitor.bpf.c 的 struct fileio_ns_info 逐字节一致。
struct FileIoNsInfoRaw {
    std::uint64_t dev;
    std::uint64_t ino;
};

/// 与 fileio_monitor.bpf.c 的 struct fileio_global 逐字节一致。
struct FileIoGlobalRaw {
    std::uint64_t read_count;
    std::uint64_t read_ns;
    std::uint64_t read_max_ns;
    std::uint64_t write_count;
    std::uint64_t write_ns;
    std::uint64_t write_max_ns;
    std::uint64_t read_hist[kKernelHistBuckets];
    std::uint64_t write_hist[kKernelHistBuckets];
    // readv/writev（vfs_iter_read/vfs_iter_write）。
    std::uint64_t iter_read_count;
    std::uint64_t iter_read_ns;
    std::uint64_t iter_read_max_ns;
    std::uint64_t iter_write_count;
    std::uint64_t iter_write_ns;
    std::uint64_t iter_write_max_ns;
    // io_uring（io_read/io_write）：count/ns/max 只含同步完成；pending 是
    // 异步排队或错误（延迟不可测）。
    std::uint64_t uring_read_count;
    std::uint64_t uring_read_ns;
    std::uint64_t uring_read_max_ns;
    std::uint64_t uring_write_count;
    std::uint64_t uring_write_ns;
    std::uint64_t uring_write_max_ns;
    std::uint64_t uring_read_pending;
    std::uint64_t uring_write_pending;
};

static_assert(sizeof(FileIoTaskRaw) == 72,
              "FileIoTaskRaw layout must match the eBPF map value");
static_assert(sizeof(FileIoNsInfoRaw) == 16,
              "FileIoNsInfoRaw layout must match the eBPF map value");
// 6 个基础计数 + 2 份 32 桶直方图 + 14 个 iter/uring 计数/延迟字段。
static_assert(sizeof(FileIoGlobalRaw) == sizeof(std::uint64_t) * 84,
              "FileIoGlobalRaw layout must match the eBPF map value");

/// 安全地拷贝内核 comm（可能没有结尾 NUL）。
std::string CommToString(const char comm[16]) {
    const void* terminator = std::memchr(comm, '\0', 16);
    const std::size_t length =
        terminator != nullptr ? static_cast<const char*>(terminator) - comm : 16;
    return std::string(comm, length);
}

/// 计算一次采样窗口的 IOPS（两次累计值之差 / 墙钟秒数）。
double ComputeIops(std::uint64_t now, std::uint64_t before, double seconds) {
    if (seconds <= 0.0 || now < before) {
        return 0.0;
    }
    return static_cast<double>(now - before) / seconds;
}

/// 从 /proc/<init_pid>/status 的 NSpid 行解析当前 namespace 的 pid。
///
/// NSpid 的列是从 init namespace 到读取者所在 namespace 的 pid 链：多列时
/// 首列是 init pid、末列是当前可读到的 pid；单列时说明 /proc 只暴露当前
/// namespace（常见于 WSL2 嵌套 pid ns），只有值恰好等于 init_pid 才能确认。
/// 无法确认时返回 0，绝不做「pid 对不上也硬映射」的猜测。
std::uint32_t ResolveNamespacePidFromProc(std::uint32_t init_pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%u/status", init_pid);
    std::FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return 0;
    }
    char line[256];
    std::uint32_t first_pid = 0;
    std::uint32_t last_pid = 0;
    std::uint32_t count = 0;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, "NSpid:", 6) != 0) {
            continue;
        }
        const char* cursor = line + 6;
        while (*cursor != '\0') {
            while (*cursor != '\0' &&
                   !std::isdigit(static_cast<unsigned char>(*cursor))) {
                ++cursor;
            }
            if (*cursor == '\0') {
                break;
            }
            char* end = nullptr;
            const unsigned long value = std::strtoul(cursor, &end, 10);
            if (end == cursor) {
                break;
            }
            if (count == 0) {
                first_pid = static_cast<std::uint32_t>(value);
            }
            last_pid = static_cast<std::uint32_t>(value);
            ++count;
            cursor = end;
        }
        break;
    }
    std::fclose(file);
    if (count == 0 || first_pid != init_pid) {
        return 0;
    }
    return last_pid;
}

/// 读取 /proc/sys/kernel/io_uring_disabled。
///
/// 语义（Documentation/admin-guide/sysctl/kernel.rst）：
///   0 = 允许所有进程使用；1 = 仅 CAP_SYS_ADMIN；2 = 完全禁用。
/// 文件不存在（5.10 之前的内核）按 0 处理。只有 2 才跳过 io_uring 探针；
/// 1 仍 attach —— 监控器只统计实际发生的 io_uring 调用，不阻止任何进程。
int ReadIoUringDisabled() {
    std::FILE* file = std::fopen("/proc/sys/kernel/io_uring_disabled", "r");
    if (file == nullptr) {
        return 0;
    }
    int value = 0;
    if (std::fscanf(file, "%d", &value) != 1) {
        value = 0;
    }
    std::fclose(file);
    return value;
}

/// 把字符串列表用逗号拼成一条诊断信息。
std::string JoinStrings(const std::vector<std::string>& items) {
    std::string result;
    for (const auto& item : items) {
        if (!result.empty()) {
            result += ",";
        }
        result += item;
    }
    return result;
}

}  // namespace

struct FileIoMonitor::Impl {
    explicit Impl(std::size_t n) : top_n(n) {}
    ~Impl() { StopInternal(); }

    void Init() {
#if defined(GO2CPP_HAVE_EBPF)
        if (!detail::KernelBpfUsable()) {
            reason = detail::KernelBpfUnusableReason();
            return;
        }
        std::string error;
        if (!object.Load(go2cpp_fileio_monitor_elf,
                         go2cpp_fileio_monitor_elf_len, &error)) {
            reason = "eBPF load failed: " + error;
            return;
        }
        global_fd = object.MapFd("fileio_global_map");
        task_fd = object.MapFd("fileio_task_map");
        ns_fd = object.MapFd("fileio_ns_map");
        if (global_fd < 0 || task_fd < 0 || ns_fd < 0) {
            reason =
                "embedded BPF object lacks fileio_global_map/fileio_task_map/"
                "fileio_ns_map";
            object.Close();
            return;
        }
        ConfigureNamespaceMap();
        // 四个探针都必须成功：只有 entry 没有 exit 会拿到永远不结算的
        // 起始时间，只有 exit 没有 entry 则什么都统计不到。
        const bool attached =
            object.AttachKprobe("fileio_read_enter", "vfs_read", false,
                                &error) &&
            object.AttachKprobe("fileio_read_exit", "vfs_read", true, &error) &&
            object.AttachKprobe("fileio_write_enter", "vfs_write", false,
                                &error) &&
            object.AttachKprobe("fileio_write_exit", "vfs_write", true,
                                &error);
        if (!attached) {
            reason = "kprobe attach failed: " + error;
            object.Close();
            global_fd = -1;
            task_fd = -1;
            return;
        }
        ConfigureOptionalCoverage();
        available = true;
        reason.clear();
#else
        reason = detail::KernelBpfUnusableReason();
#endif
    }

    /// readv/writev 与 io_uring 的可选探针。
    ///
    /// 这些路径不是可用性的前提：attach 失败只缩小覆盖范围，写进
    /// coverage_gap，vfs_read/vfs_write 的统计继续工作。每对探针成对
    /// attach：return 侧失败时回滚 entry 侧（否则留下永不结算的起始时间，
    /// 持续占用 start map 并产生无效开销）。
    void ConfigureOptionalCoverage() {
#if defined(GO2CPP_HAVE_EBPF)
        std::vector<std::string> covered{"vfs_read", "vfs_write"};
        std::vector<std::string> gaps;
        AttachPair("fileio_iter_read_enter", "fileio_iter_read_exit",
                   "vfs_readv", &covered, &gaps);
        AttachPair("fileio_iter_write_enter", "fileio_iter_write_exit",
                   "vfs_writev", &covered, &gaps);
        if (ReadIoUringDisabled() == 2) {
            gaps.push_back(
                "io_uring disabled by /proc/sys/kernel/io_uring_disabled=2");
        } else {
            AttachPair("fileio_uring_read_enter", "fileio_uring_read_exit",
                       "io_read", &covered, &gaps);
            AttachPair("fileio_uring_write_enter", "fileio_uring_write_exit",
                       "io_write", &covered, &gaps);
        }
        coverage = JoinStrings(covered);
        coverage_gap = JoinStrings(gaps);
#else
        coverage.clear();
        coverage_gap = detail::KernelBpfUnusableReason();
#endif
    }

    /// 成对 attach 一个内核符号的 entry/return 探针；失败时回滚并记录原因。
    void AttachPair(const char* enter, const char* exit, const char* symbol,
                    std::vector<std::string>* covered,
                    std::vector<std::string>* gaps) {
#if defined(GO2CPP_HAVE_EBPF)
        std::string error;
        if (!object.AttachKprobe(enter, symbol, false, &error)) {
            gaps->push_back(std::string(symbol) + ": " + error);
            return;
        }
        if (!object.AttachKprobe(exit, symbol, true, &error)) {
            object.DetachLast(1);
            gaps->push_back(std::string(symbol) + ": " + error);
            return;
        }
        covered->push_back(symbol);
#else
        (void)enter;
        (void)exit;
        (void)symbol;
        (void)covered;
        (void)gaps;
#endif
    }

    /// 把 /proc/self/ns/pid 的 nsfs dev/ino 写进内核 map，供
    /// bpf_get_ns_current_pid_tgid 解析采集端当前 namespace 的 pid。
    /// 失败不阻断采集：namespace_pid 退化为 0（用户态再用 /proc 兜底）。
    void ConfigureNamespaceMap() {
#if defined(GO2CPP_HAVE_EBPF)
        if (ns_fd < 0) {
            return;
        }
        struct stat ns_stat {};
        if (::stat("/proc/self/ns/pid", &ns_stat) != 0) {
            return;
        }
        FileIoNsInfoRaw info{};
        info.dev = static_cast<std::uint64_t>(ns_stat.st_dev);
        info.ino = static_cast<std::uint64_t>(ns_stat.st_ino);
        std::uint32_t zero = 0;
        (void)bpf_map_update_elem(ns_fd, &zero, &info, BPF_ANY);
#endif
    }

    void StopInternal() {
        object.Close();
        global_fd = -1;
        task_fd = -1;
        ns_fd = -1;
        available = false;
        reason = "stopped";
    }

    bool available{false};
    std::string reason;
    /// 已覆盖的路径；ConfigureOptionalCoverage 后固定。
    std::string coverage;
    /// 未覆盖路径及原因；空表示 vfs_read/vfs_write + readv + io_uring 全覆盖。
    std::string coverage_gap;
    detail::KernelBpfObject object;
    int global_fd{-1};
    int task_fd{-1};
    int ns_fd{-1};
    std::size_t top_n{10};
    /// UpdateOnce 成功次数。sample_count() 不加锁读取，必须是原子量，
    /// 否则与 UpdateOnce 的 ++samples 构成数据竞争。
    std::atomic<std::uint64_t> samples{0};
    /// IOPS 的差分基线。
    bool has_previous{false};
    std::chrono::steady_clock::time_point previous_time{};
    std::uint64_t previous_reads{0};
    std::uint64_t previous_writes{0};
    mutable std::mutex mutex;
};

FileIoMonitor::FileIoMonitor(std::size_t top_n)
    : impl_(std::make_unique<Impl>(top_n)) {
    impl_->Init();
}

FileIoMonitor::~FileIoMonitor() = default;

Status FileIoMonitor::UpdateOnce(Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->available) {
        snapshot.fileio = FileIoStats{};
        snapshot.fileio.reason = impl_->reason;
        return Status::kUnsupported;
    }
    FileIoStats stats;
    stats.active = true;

    FileIoGlobalRaw global {};
    const bool global_ok =
        detail::ReadArrayMap(impl_->global_fd, 0U, &global);
    if (global_ok) {
        stats.read_count = global.read_count;
        stats.write_count = global.write_count;
        for (std::size_t i = 0; i < stats.read_hist.size(); ++i) {
            stats.read_hist[i] = global.read_hist[i];
            stats.write_hist[i] = global.write_hist[i];
        }
        if (global.read_count > 0) {
            stats.read_avg_us = static_cast<double>(global.read_ns) /
                                static_cast<double>(global.read_count) / 1000.0;
        }
        if (global.write_count > 0) {
            stats.write_avg_us =
                static_cast<double>(global.write_ns) /
                static_cast<double>(global.write_count) / 1000.0;
        }
        stats.read_max_us = static_cast<double>(global.read_max_ns) / 1000.0;
        stats.write_max_us = static_cast<double>(global.write_max_ns) / 1000.0;
        stats.read_p50_us =
            KernelHistPercentile(stats.read_hist, stats.read_count, 0.50);
        stats.read_p99_us =
            KernelHistPercentile(stats.read_hist, stats.read_count, 0.99);
        stats.write_p50_us =
            KernelHistPercentile(stats.write_hist, stats.write_count, 0.50);
        stats.write_p99_us =
            KernelHistPercentile(stats.write_hist, stats.write_count, 0.99);

        stats.readv_count = global.iter_read_count;
        stats.writev_count = global.iter_write_count;
        if (global.iter_read_count > 0) {
            stats.readv_avg_us =
                static_cast<double>(global.iter_read_ns) /
                static_cast<double>(global.iter_read_count) / 1000.0;
        }
        if (global.iter_write_count > 0) {
            stats.writev_avg_us =
                static_cast<double>(global.iter_write_ns) /
                static_cast<double>(global.iter_write_count) / 1000.0;
        }
        stats.readv_max_us =
            static_cast<double>(global.iter_read_max_ns) / 1000.0;
        stats.writev_max_us =
            static_cast<double>(global.iter_write_max_ns) / 1000.0;

        stats.uring_read_count = global.uring_read_count;
        stats.uring_write_count = global.uring_write_count;
        stats.uring_read_pending = global.uring_read_pending;
        stats.uring_write_pending = global.uring_write_pending;
        if (global.uring_read_count > 0) {
            stats.uring_read_avg_us =
                static_cast<double>(global.uring_read_ns) /
                static_cast<double>(global.uring_read_count) / 1000.0;
        }
        if (global.uring_write_count > 0) {
            stats.uring_write_avg_us =
                static_cast<double>(global.uring_write_ns) /
                static_cast<double>(global.uring_write_count) / 1000.0;
        }
        stats.uring_read_max_us =
            static_cast<double>(global.uring_read_max_ns) / 1000.0;
        stats.uring_write_max_us =
            static_cast<double>(global.uring_write_max_ns) / 1000.0;
    }
    stats.coverage = impl_->coverage;
    stats.coverage_gap = impl_->coverage_gap;

    // 只有成功读到全局计数时才推进差分基线，否则一次 map 读取失败会把
    // 基线清零，恢复后得到「全部历史 IO / 一个窗口」的假 IOPS 尖峰。
    const auto now = std::chrono::steady_clock::now();
    if (global_ok) {
        if (impl_->has_previous) {
            const double elapsed =
                std::chrono::duration<double>(now - impl_->previous_time)
                    .count();
            stats.read_iops =
                ComputeIops(stats.read_count, impl_->previous_reads, elapsed);
            stats.write_iops = ComputeIops(stats.write_count,
                                           impl_->previous_writes, elapsed);
        }
        impl_->previous_time = now;
        impl_->previous_reads = stats.read_count;
        impl_->previous_writes = stats.write_count;
        impl_->has_previous = true;
    }

    std::vector<FileIoTaskStat> items;
    const auto rows =
        detail::ReadHashMap<std::uint32_t, FileIoTaskRaw>(impl_->task_fd);
    items.reserve(rows.size());
    for (const auto& row : rows) {
        const FileIoTaskRaw& raw = row.second;
        if (raw.read_count == 0 && raw.write_count == 0) {
            continue;
        }
        FileIoTaskStat item;
        item.pid = row.first;
        item.comm = CommToString(raw.comm);
        item.namespace_pid = raw.ns_pid;
        if (item.namespace_pid == 0) {
            // 内核 helper 不可用（旧内核/未配置 ns map）时用 /proc 兜底；
            // WSL2 嵌套 ns 下 /proc 看不到 init-ns pid，此时保持 0。
            item.namespace_pid = ResolveNamespacePidFromProc(item.pid);
        }
        item.read_count = raw.read_count;
        item.write_count = raw.write_count;
        if (raw.read_count > 0) {
            item.read_avg_us = static_cast<double>(raw.read_ns) /
                               static_cast<double>(raw.read_count) / 1000.0;
        }
        if (raw.write_count > 0) {
            item.write_avg_us = static_cast<double>(raw.write_ns) /
                                static_cast<double>(raw.write_count) / 1000.0;
        }
        item.read_max_us = static_cast<double>(raw.read_max_ns) / 1000.0;
        item.write_max_us = static_cast<double>(raw.write_max_ns) / 1000.0;
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(),
              [](const FileIoTaskStat& left, const FileIoTaskStat& right) {
                  const std::uint64_t left_total =
                      left.read_count + left.write_count;
                  const std::uint64_t right_total =
                      right.read_count + right.write_count;
                  if (left_total != right_total) {
                      return left_total > right_total;
                  }
                  return left.pid < right.pid;
              });
    if (items.size() > impl_->top_n) {
        items.resize(impl_->top_n);
    }
    stats.top = std::move(items);
    snapshot.fileio = std::move(stats);
    ++impl_->samples;
    return Status::kOk;
}

void FileIoMonitor::Stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->StopInternal();
}

bool FileIoMonitor::available() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->available;
}

std::string FileIoMonitor::reason() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->reason;
}

std::uint64_t FileIoMonitor::sample_count() const noexcept {
    return impl_->samples.load(std::memory_order_relaxed);
}

void FileIoMonitor::ProgramCost(std::uint64_t* run_time_ns,
                                std::uint64_t* run_cnt) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->object.ProgramCost(run_time_ns, run_cnt);
}

}  // namespace go2cpp::os::monitor
