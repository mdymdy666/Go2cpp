#include "go2cpp/os/monitor/aggregator.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/os/monitor/cpu_monitor.hpp"
#include "go2cpp/os/monitor/disk_monitor.hpp"
#include "go2cpp/os/monitor/fileio_monitor.hpp"
#include "go2cpp/os/monitor/futex_monitor.hpp"
#include "go2cpp/os/monitor/mem_monitor.hpp"
#include "go2cpp/os/monitor/net_ebpf_monitor.hpp"
#include "go2cpp/os/monitor/net_monitor.hpp"
#include "go2cpp/os/monitor/sched_monitor.hpp"
#include "go2cpp/os/monitor/syscall_monitor.hpp"
#include "go2cpp/os/monitor/tcp_monitor.hpp"
#include "go2cpp/runtime.hpp"

namespace go2cpp::os::monitor {
namespace {

std::int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

std::string Hostname() {
    char buffer[256] = {};
    if (::gethostname(buffer, sizeof(buffer) - 1) != 0) {
        return "unknown";
    }
    return buffer;
}

HostInfo ReadHostInfo() {
    HostInfo info;
    info.hostname = Hostname();
    struct utsname uts {};
    if (::uname(&uts) == 0) {
        info.kernel = uts.release;
        info.arch = uts.machine;
        info.os = std::string(uts.sysname) + " " + uts.version;
    }
    return info;
}

/// 采集器消息：只携带本次采集器负责的字段。
struct CollectorResult {
    CollectorKind kind{CollectorKind::kCpu};
    Status status{Status::kError};
    Snapshot partial;
};

}  // namespace

struct Aggregator::Impl {
    Impl(AggregatorConfig cfg, std::shared_ptr<SnapshotSink> snapshot_sink)
        : config(std::move(cfg)), sink(std::move(snapshot_sink)) {
        CreateCollectors();
        host_name = Hostname();
        host_info = ReadHostInfo();
    }

    /// 按 config 创建/重建全部采集器。
    ///
    /// Stop() 会关闭 eBPF 采集器（detach link、关 map fd），它们不会自己
    /// 恢复；Start() 复用同一对象时必须重建，否则第二个运行周期里五个
    /// 内核采集器会永久停在 reason="stopped" 的降级状态。
    void CreateCollectors() {
        if (config.enable_cpu) {
            cpu = std::make_unique<CpuMonitor>();
        }
        if (config.enable_mem) {
            mem = std::make_unique<MemMonitor>();
        }
        if (config.enable_disk) {
            disk = std::make_unique<DiskMonitor>();
        }
        if (config.enable_net) {
            net = std::make_unique<NetMonitor>();
        }
        if (config.enable_ebpf) {
            ebpf = std::make_unique<NetEbpfMonitor>();
        }
        if (config.enable_syscall) {
            syscall = std::make_unique<SyscallMonitor>();
        }
        if (config.enable_sched) {
            sched = std::make_unique<SchedMonitor>();
        }
        if (config.enable_futex) {
            futex = std::make_unique<FutexMonitor>();
        }
        if (config.enable_tcp) {
            tcp = std::make_unique<TcpMonitor>();
        }
        if (config.enable_fileio) {
            fileio = std::make_unique<FileIoMonitor>();
        }
    }

    /// 无锁版本的同步采集；调用方必须持有 lifecycle_mutex。
    Snapshot CollectNowLocked();

    ~Impl() { StopInternal(); }

    /// 同步采集某个采集器；status 返回其结果，partial 承载数据。
    ///
    /// 后台定时器与 CollectNow()/PublishNow() 可能同时采样同一个采集器，
    /// 而 Cpu/Disk/Net 采集器内部没有锁（差分基线是普通 std::map）。这里
    /// 按采集器种类分别加锁：不同种类仍可并行，同一种类严格串行。
    Status Collect(CollectorKind kind, Snapshot& partial) {
        const auto index = static_cast<std::size_t>(kind);
        if (index >= collect_mutexes.size()) {
            return Status::kUnsupported;
        }
        std::lock_guard<std::mutex> lock(collect_mutexes[index]);
        switch (kind) {
            case CollectorKind::kCpu:
                return cpu ? cpu->UpdateOnce(partial) : Status::kUnsupported;
            case CollectorKind::kMem:
                return mem ? mem->UpdateOnce(partial) : Status::kUnsupported;
            case CollectorKind::kDisk:
                return disk ? disk->UpdateOnce(partial) : Status::kUnsupported;
            case CollectorKind::kNet:
                return net ? net->UpdateOnce(partial) : Status::kUnsupported;
            case CollectorKind::kEbpf:
                return ebpf ? ebpf->UpdateOnce(partial) : Status::kUnsupported;
            case CollectorKind::kSyscall:
                return syscall ? syscall->UpdateOnce(partial)
                               : Status::kUnsupported;
            case CollectorKind::kSched:
                return sched ? sched->UpdateOnce(partial) : Status::kUnsupported;
            case CollectorKind::kFutex:
                return futex ? futex->UpdateOnce(partial) : Status::kUnsupported;
            case CollectorKind::kTcp:
                return tcp ? tcp->UpdateOnce(partial) : Status::kUnsupported;
            case CollectorKind::kFileIo:
                return fileio ? fileio->UpdateOnce(partial)
                              : Status::kUnsupported;
        }
        return Status::kUnsupported;
    }

    /// 把一次采集结果合并进累计快照（调用方持锁）。
    void MergeLocked(const CollectorResult& result) {
        switch (result.kind) {
            case CollectorKind::kCpu:
                latest.cpus = result.partial.cpus;
                latest.load = result.partial.load;
                break;
            case CollectorKind::kMem:
                latest.mem = result.partial.mem;
                break;
            case CollectorKind::kDisk:
                latest.disks = result.partial.disks;
                break;
            case CollectorKind::kNet:
                latest.nets = result.partial.nets;
                break;
            case CollectorKind::kEbpf:
                latest.ebpf_nets = result.partial.ebpf_nets;
                latest.ebpf_active = result.partial.ebpf_active;
                latest.ebpf_reason = result.partial.ebpf_reason;
                break;
            case CollectorKind::kSyscall:
                latest.syscalls = result.partial.syscalls;
                break;
            case CollectorKind::kSched:
                latest.sched = result.partial.sched;
                break;
            case CollectorKind::kFutex:
                latest.futex = result.partial.futex;
                break;
            case CollectorKind::kTcp:
                latest.tcp = result.partial.tcp;
                break;
            case CollectorKind::kFileIo:
                latest.fileio = result.partial.fileio;
                break;
        }
    }

    void AcceptResult(CollectorResult result) {
        std::lock_guard<std::mutex> lock(mutex);
        if (result.status == Status::kOk) {
            MergeLocked(result);
            latest.host = host_name;
            latest.host_info = host_info;
            latest.timestamp_ms = NowMs();
            ++stats.snapshots;
            if (result.kind == CollectorKind::kEbpf) {
                ++stats.ebpf_samples;
            }
        } else if (result.status == Status::kError) {
            ++stats.errors;
        }
    }

    void StartTimers() {
        const auto period = config.interval;
        const auto schedule = [this, period](CollectorKind kind) {
            return scheduler->schedule_every(
                period, [this, kind]() {
                    Snapshot partial;
                    CollectorResult result;
                    result.kind = kind;
                    result.status = Collect(kind, partial);
                    result.partial = std::move(partial);
                    // 聚合 Fiber 若已经退出（Stop 期间），TrySend 会失败，
                    // 这里直接丢弃即可，不能阻塞在 Channel 上。
                    (void)channel->TrySend(std::move(result));
                });
        };
        if (cpu) {
            timers.push_back(schedule(CollectorKind::kCpu));
        }
        if (mem) {
            timers.push_back(schedule(CollectorKind::kMem));
        }
        if (disk) {
            timers.push_back(schedule(CollectorKind::kDisk));
        }
        if (net) {
            timers.push_back(schedule(CollectorKind::kNet));
        }
        if (ebpf) {
            timers.push_back(schedule(CollectorKind::kEbpf));
        }
        if (syscall) {
            timers.push_back(schedule(CollectorKind::kSyscall));
        }
        if (sched) {
            timers.push_back(schedule(CollectorKind::kSched));
        }
        if (futex) {
            timers.push_back(schedule(CollectorKind::kFutex));
        }
        if (tcp) {
            timers.push_back(schedule(CollectorKind::kTcp));
        }
        if (fileio) {
            timers.push_back(schedule(CollectorKind::kFileIo));
        }
    }

    std::shared_ptr<SnapshotSink> Sink() const {
        std::lock_guard<std::mutex> lock(sink_mutex);
        return sink;
    }

    void SetSink(std::shared_ptr<SnapshotSink> next) {
        std::lock_guard<std::mutex> lock(sink_mutex);
        sink = std::move(next);
    }

    /// 把最新且未发布过的快照推给 sink；调用方可来自定时器 Fiber。
    bool PublishLatest() {
        auto target = Sink();
        if (!target) {
            return false;
        }
        Snapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (latest.timestamp_ms == 0 ||
                latest.timestamp_ms == last_published_ms) {
                return false;
            }
            snapshot = latest;
        }
        if (!target->Push(snapshot)) {
            // 发布失败时不推进 last_published_ms：下一个发布周期会重试同
            // 一快照。原实现先记账再 Push，sink 一次瞬断就会让该快照永久
            // 丢失（采集全部失败、快照不再更新时尤其致命）。
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (last_published_ms < snapshot.timestamp_ms) {
                last_published_ms = snapshot.timestamp_ms;
            }
        }
        return true;
    }

    /// 独立的发布定时器：与采集定时器同周期，但只推送 timestamp 变化的快照。
    void StartPublisher() {
        const auto period = config.interval;
        timers.push_back(scheduler->schedule_every(
            period, [this]() { (void)PublishLatest(); }));
    }

    /// 停止并释放资源；调用方必须持有 lifecycle_mutex。
    void StopInternal() {
        if (!running.load()) {
            return;
        }
        running = false;
        for (auto& timer : timers) {
            timer.Cancel();
        }
        timers.clear();
        if (channel) {
            (void)channel->Close();
        }
        // 顺序很重要：先等聚合 Fiber 因 Channel 关闭而退出，再销毁调度器。
        // 反过来会让 Task::wait() 等待一个已经被 shutdown 的调度器状态，
        // 并发 Stop 时表现为偶发卡死（ctest -j4 下实测 30s watchdog 超时）。
        if (aggregator_task) {
            aggregator_task->wait();
            aggregator_task.reset();
        }
        if (scheduler) {
            scheduler->shutdown();
            scheduler.reset();
        }
        channel.reset();
        if (cpu) {
            cpu->Stop();
        }
        if (mem) {
            mem->Stop();
        }
        if (disk) {
            disk->Stop();
        }
        if (net) {
            net->Stop();
        }
        if (ebpf) {
            ebpf->Stop();
        }
        if (syscall) {
            syscall->Stop();
        }
        if (sched) {
            sched->Stop();
        }
        if (futex) {
            futex->Stop();
        }
        if (tcp) {
            tcp->Stop();
        }
        if (fileio) {
            fileio->Stop();
        }
        // 五个 eBPF 采集器的 Stop 会销毁 BPF 对象；Start 时据此重建。
        collectors_stopped = true;
    }

    AggregatorConfig config;
    std::unique_ptr<CpuMonitor> cpu;
    std::unique_ptr<MemMonitor> mem;
    std::unique_ptr<DiskMonitor> disk;
    std::unique_ptr<NetMonitor> net;
    std::unique_ptr<NetEbpfMonitor> ebpf;
    std::unique_ptr<SyscallMonitor> syscall;
    std::unique_ptr<SchedMonitor> sched;
    std::unique_ptr<FutexMonitor> futex;
    std::unique_ptr<TcpMonitor> tcp;
    std::unique_ptr<FileIoMonitor> fileio;

    std::string host_name;
    HostInfo host_info;

    std::shared_ptr<SnapshotSink> sink;
    mutable std::mutex sink_mutex;
    /// 上次发布给 sink 的 timestamp_ms，避免同一快照重复推送。
    std::int64_t last_published_ms{0};

    std::unique_ptr<Scheduler> scheduler;
    std::shared_ptr<Channel<CollectorResult>> channel;
    std::shared_ptr<Task> aggregator_task;
    std::vector<core::Timer> timers;

    std::atomic<bool> running{false};
    std::atomic<bool> started{false};
    /// StopInternal() 是否已经关闭过 eBPF 采集器（Start 时据此重建）。
    bool collectors_stopped{false};
    /// 每种采集器一把锁，串行化后台定时器与同步 CollectNow 的同一对象。
    std::array<std::mutex, 10> collect_mutexes;
    /// 串行化 Start/Stop：两者都会创建/销毁 scheduler、timer 与 channel，
    /// 并发进入会看到半初始化的成员（这是并发启停用例卡死的根因）。
    std::mutex lifecycle_mutex;

    mutable std::mutex mutex;
    Snapshot latest;
    AggregatorStats stats;
};

Aggregator::Aggregator(AggregatorConfig config,
                       std::shared_ptr<SnapshotSink> sink)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(sink))),
      config_(impl_->config) {}

Aggregator::~Aggregator() = default;

bool Aggregator::Start() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    if (impl_->running.load()) {
        return false;
    }
    // Stop() 关闭过 eBPF 采集器：重建它们（含重新 attach），否则第二次
    // 启动周期里 tracepoint/kprobe/TC 监控器全部不可用。
    if (impl_->collectors_stopped) {
        impl_->CreateCollectors();
        impl_->collectors_stopped = false;
    }
    // 先置 running 再 spawn：聚合 Fiber 的循环条件依赖它，spawn 之后再置位
    // 会让 Fiber 首次调度就看到 false 并立即退出。
    impl_->running = true;
    const std::size_t workers =
        impl_->config.workers > 0
            ? static_cast<std::size_t>(impl_->config.workers)
            : std::max<std::size_t>(2, std::thread::hardware_concurrency());
    impl_->scheduler = std::make_unique<Scheduler>(workers);
    impl_->scheduler->start();
    impl_->channel = MakeChannel<CollectorResult>(16);
    impl_->started = true;

    // 聚合 Fiber：持续从 Channel 取采集结果并合并进 latest_。
    impl_->aggregator_task = impl_->scheduler->spawn([this]() {
        while (impl_->running.load()) {
            auto received = impl_->channel->Recv();
            if (!received) {
                break;  // Channel 已关闭
            }
            impl_->AcceptResult(std::move(*received.value));
        }
    });
    if (!impl_->aggregator_task) {
        impl_->running = false;
        impl_->channel.reset();
        impl_->scheduler->shutdown();
        impl_->scheduler.reset();
        return false;
    }

    impl_->StartTimers();
    impl_->StartPublisher();
    return true;
}

void Aggregator::Stop() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    impl_->StopInternal();
}

bool Aggregator::running() const noexcept {
    return impl_->running.load();
}

Snapshot Aggregator::Impl::CollectNowLocked() {
    Snapshot snapshot;
    snapshot.host = host_name;
    snapshot.host_info = host_info;
    snapshot.timestamp_ms = NowMs();
    if (cpu) {
        (void)cpu->UpdateOnce(snapshot);
    }
    if (mem) {
        (void)mem->UpdateOnce(snapshot);
    }
    if (disk) {
        (void)disk->UpdateOnce(snapshot);
    }
    if (net) {
        (void)net->UpdateOnce(snapshot);
    }
    if (ebpf) {
        const Status status = ebpf->UpdateOnce(snapshot);
        snapshot.ebpf_active = status == Status::kOk;
        if (!snapshot.ebpf_active) {
            snapshot.ebpf_reason = ebpf->reason();
        }
    }
    if (syscall) {
        (void)syscall->UpdateOnce(snapshot);
    }
    if (sched) {
        (void)sched->UpdateOnce(snapshot);
    }
    if (futex) {
        (void)futex->UpdateOnce(snapshot);
    }
    if (tcp) {
        (void)tcp->UpdateOnce(snapshot);
    }
    if (fileio) {
        (void)fileio->UpdateOnce(snapshot);
    }
    return snapshot;
}

Snapshot Aggregator::CollectNow() {
    // 与 Start/Stop 串行：Stop 会在 scheduler 停止后调用采集器的 Stop()，
    // 此时的并发 CollectNow 会访问已关闭的 BPF map fd。
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    return impl_->CollectNowLocked();
}

Snapshot Aggregator::Latest() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->latest;
}

void Aggregator::SetSink(std::shared_ptr<SnapshotSink> sink) {
    impl_->SetSink(std::move(sink));
}

std::shared_ptr<SnapshotSink> Aggregator::Sink() const {
    return impl_->Sink();
}

bool Aggregator::PublishLatest() {
    return impl_->PublishLatest();
}

bool Aggregator::PublishNow() {
    auto target = impl_->Sink();
    if (!target) {
        return false;
    }
    return target->Push(CollectNow());
}

std::int64_t Aggregator::LatestTimestampMs() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->latest.timestamp_ms;
}

AggregatorStats Aggregator::Stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    AggregatorStats copy = impl_->stats;
    copy.ebpf_active = impl_->ebpf != nullptr && impl_->ebpf->available();
    copy.ebpf_reason = impl_->ebpf != nullptr ? impl_->ebpf->reason() : "";
    return copy;
}

bool Aggregator::EbpfAvailable() const {
    return impl_->ebpf != nullptr && impl_->ebpf->available();
}

std::string Aggregator::EbpfReason() const {
    return impl_->ebpf != nullptr ? impl_->ebpf->reason() : "disabled";
}

}  // namespace go2cpp::os::monitor
