// eBPF 非 root 降级契约测试（go2cpp_os_monitor_ebpf_nonroot）。
//
// 与 test_monitor_ebpf.cpp 的分工：
//   - 该文件的默认模式已经断言「当前用户不可用 → kUnsupported」；
//   - 本文件专门把**非 root**作为前置条件（root 下返回 77 让 ctest SKIP），
//     覆盖更完整的降级面：
//       1. NetEbpfMonitor：available/reason/UpdateOnce/Stop 幂等；
//       2. 五个内核 tracepoint 监控器（syscall/sched/futex/tcp/fileio）同样降级，
//          且 reason 明确指向 root 权限；
//       3. Aggregator：eBPF 降级不影响 CPU/内存/磁盘/网络四个 /proc 采集器；
//       4. SnapshotToJson 把 active=false + reason 透传给 Manager/工具层；
//       5. Manager.Summaries/KernelSummaries 里 eBPF 状态仍是 false + reason；
//       6. 连续 20 次采集稳定，不会因为反复尝试加载 BPF 而失败或崩溃。

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "go2cpp/os/monitor/aggregator.hpp"
#include "go2cpp/os/monitor/fileio_monitor.hpp"
#include "go2cpp/os/monitor/futex_monitor.hpp"
#include "go2cpp/os/monitor/interfaces.hpp"
#include "go2cpp/os/monitor/manager.hpp"
#include "go2cpp/os/monitor/net_ebpf_monitor.hpp"
#include "go2cpp/os/monitor/sched_monitor.hpp"
#include "go2cpp/os/monitor/syscall_monitor.hpp"
#include "go2cpp/os/monitor/tcp_monitor.hpp"
#include "test_support.hpp"

namespace {

int g_checks = 0;

void Expect(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        go2cpp_tests::g_failures.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "FAIL [ebpf_nonroot]: %s\n", what.c_str());
    }
}

bool HasRootReason(const std::string& reason) {
    return reason.find("root") != std::string::npos ||
           reason.find("权限") != std::string::npos;
}

/// 单个监控器的统一降级契约。
template <typename MonitorType>
void CheckKernelMonitor(const char* name, MonitorType& monitor) {
    Expect(!monitor.available(),
           std::string(name) + " 非 root 时 available() 必须为 false");
    const std::string reason = monitor.reason();
    Expect(!reason.empty(), std::string(name) + " 必须给出 reason");
    Expect(HasRootReason(reason),
           std::string(name) + " 的 reason 应指向 root 权限，实际: " + reason);
    std::printf("[ebpf_nonroot] %s: %s\n", name, reason.c_str());

    go2cpp::os::monitor::Snapshot snapshot;
    const auto status = monitor.UpdateOnce(snapshot);
    Expect(status == go2cpp::os::monitor::Status::kUnsupported,
           std::string(name) + " UpdateOnce 应返回 kUnsupported，实际 " +
               go2cpp::os::monitor::StatusName(status));
    monitor.Stop();
    monitor.Stop();  // 幂等
    go2cpp::os::monitor::Snapshot after_stop;
    Expect(monitor.UpdateOnce(after_stop) ==
               go2cpp::os::monitor::Status::kUnsupported,
           std::string(name) + " Stop 之后 UpdateOnce 仍应为 kUnsupported");
}

/// 1) eBPF 网络采集器。
void CheckNetEbpf() {
    go2cpp::os::monitor::NetEbpfMonitor monitor;
    Expect(!monitor.available(), "NetEbpfMonitor 非 root 时 available() 必须为 false");
    const std::string reason = monitor.reason();
    Expect(!reason.empty(), "NetEbpfMonitor 必须给出 reason");
    Expect(HasRootReason(reason),
           "NetEbpfMonitor reason 应指向 root 权限，实际: " + reason);
    std::printf("[ebpf_nonroot] net_ebpf: %s\n", reason.c_str());

    go2cpp::os::monitor::Snapshot snapshot;
    Expect(monitor.UpdateOnce(snapshot) ==
               go2cpp::os::monitor::Status::kUnsupported,
           "NetEbpfMonitor UpdateOnce 应返回 kUnsupported");
    Expect(!snapshot.ebpf_active, "降级时 snapshot.ebpf_active 必须为 false");
    Expect(!snapshot.ebpf_reason.empty(),
           "降级时 snapshot.ebpf_reason 必须非空");
    Expect(snapshot.ebpf_nets.empty(), "降级时 ebpf_nets 必须为空");

    monitor.Stop();
    monitor.Stop();
    go2cpp::os::monitor::Snapshot after_stop;
    Expect(monitor.UpdateOnce(after_stop) ==
               go2cpp::os::monitor::Status::kUnsupported,
           "NetEbpfMonitor Stop 之后仍应返回 kUnsupported");
    Expect(!monitor.available() && !monitor.reason().empty(),
           "Stop 之后 available=false 且 reason 仍非空");
}

/// 2) 五个内核 tracepoint 监控器 + 对应 stats 字段。
void CheckKernelMonitors() {
    go2cpp::os::monitor::SyscallMonitor syscall;
    go2cpp::os::monitor::SchedMonitor sched;
    go2cpp::os::monitor::FutexMonitor futex;
    go2cpp::os::monitor::TcpMonitor tcp;
    go2cpp::os::monitor::FileIoMonitor fileio;

    CheckKernelMonitor("syscall", syscall);
    CheckKernelMonitor("sched", sched);
    CheckKernelMonitor("futex", futex);
    CheckKernelMonitor("tcp", tcp);
    CheckKernelMonitor("fileio", fileio);

    go2cpp::os::monitor::Snapshot snapshot;
    Expect(syscall.UpdateOnce(snapshot) ==
               go2cpp::os::monitor::Status::kUnsupported,
           "syscall 二次调用仍应 kUnsupported");
    Expect(sched.UpdateOnce(snapshot) ==
               go2cpp::os::monitor::Status::kUnsupported,
           "sched 二次调用仍应 kUnsupported");
    Expect(futex.UpdateOnce(snapshot) ==
               go2cpp::os::monitor::Status::kUnsupported,
           "futex 二次调用仍应 kUnsupported");
    Expect(tcp.UpdateOnce(snapshot) ==
               go2cpp::os::monitor::Status::kUnsupported,
           "tcp 二次调用仍应 kUnsupported");
    Expect(fileio.UpdateOnce(snapshot) ==
               go2cpp::os::monitor::Status::kUnsupported,
           "fileio 二次调用仍应 kUnsupported");
    Expect(!snapshot.syscalls.active && !snapshot.syscalls.reason.empty(),
           "snapshot.syscalls 必须 active=false + reason 非空");
    Expect(!snapshot.sched.active && !snapshot.sched.reason.empty(),
           "snapshot.sched 必须 active=false + reason 非空");
    Expect(!snapshot.futex.active && !snapshot.futex.reason.empty(),
           "snapshot.futex 必须 active=false + reason 非空");
    Expect(!snapshot.tcp.active && !snapshot.tcp.reason.empty(),
           "snapshot.tcp 必须 active=false + reason 非空");
    Expect(!snapshot.fileio.active && !snapshot.fileio.reason.empty(),
           "snapshot.fileio 必须 active=false + reason 非空");
}

/// 3) Aggregator：只降级 eBPF，不影响四个 /proc 采集器。
void CheckAggregator() {
    go2cpp::os::monitor::AggregatorConfig config;
    config.interval = std::chrono::milliseconds(100);
    // 显式全开，确保降级来自权限而不是配置关闭。
    config.enable_ebpf = true;
    config.enable_syscall = true;
    config.enable_sched = true;
    config.enable_futex = true;
    config.enable_tcp = true;
    config.enable_fileio = true;
    go2cpp::os::monitor::Aggregator aggregator(config);

    const go2cpp::os::monitor::Snapshot snapshot = aggregator.CollectNow();
    Expect(!snapshot.cpus.empty(), "CPU 采集器不受 eBPF 降级影响");
    Expect(snapshot.mem.total_kb > 0, "内存采集器不受 eBPF 降级影响");
    Expect(!snapshot.disks.empty(), "磁盘采集器不受 eBPF 降级影响");
    Expect(!snapshot.nets.empty(), "网络采集器不受 eBPF 降级影响");
    Expect(snapshot.timestamp_ms > 0, "快照应带时间戳");
    Expect(!snapshot.ebpf_active && !snapshot.ebpf_reason.empty(),
           "Aggregator 快照的 eBPF 段必须降级且带 reason");
    Expect(!snapshot.syscalls.active && !snapshot.syscalls.reason.empty(),
           "Aggregator 快照的 syscalls 段必须降级且带 reason");
    Expect(!snapshot.sched.active, "Aggregator 快照的 sched 段必须降级");
    Expect(!snapshot.futex.active, "Aggregator 快照的 futex 段必须降级");
    Expect(!snapshot.tcp.active, "Aggregator 快照的 tcp 段必须降级");
    Expect(!snapshot.fileio.active, "Aggregator 快照的 fileio 段必须降级");

    // 连续 20 次采集：不因反复尝试 BPF 加载而失败/崩溃。
    int stable = 0;
    for (int round = 0; round < 20; ++round) {
        const go2cpp::os::monitor::Snapshot next = aggregator.CollectNow();
        if (!next.cpus.empty() && next.mem.total_kb > 0 && !next.ebpf_active) {
            ++stable;
        }
    }
    Expect(stable == 20,
           "连续 20 次采集应全部稳定，实际 " + std::to_string(stable));

    // 4) JSON 透传：Manager/工具层看到的是 active=false + 非空 reason。
    const nlohmann::json parsed =
        nlohmann::json::parse(go2cpp::os::monitor::SnapshotToJson(snapshot));
    Expect(parsed["ebpf_active"].get<bool>() == false,
           "Snapshot JSON 的 ebpf_active 应为 false");
    Expect(!parsed["ebpf_reason"].get<std::string>().empty(),
           "Snapshot JSON 的 ebpf_reason 应非空");
    for (const char* key : {"syscalls", "sched", "futex", "tcp", "fileio"}) {
        Expect(parsed.contains(key) && parsed[key].is_object(),
               std::string("Snapshot JSON 应包含 ") + key + " 对象");
        if (parsed.contains(key) && parsed[key].is_object()) {
            Expect(parsed[key]["active"].get<bool>() == false,
                   std::string(key) + ".active 应为 false");
            Expect(!parsed[key]["reason"].get<std::string>().empty(),
                   std::string(key) + ".reason 应非空");
        }
    }

    // 5) Manager 汇总：eBPF 状态不能被丢失或改写成 true。
    go2cpp::os::monitor::ManagerConfig manager_config;
    manager_config.listen_address = "127.0.0.1";
    manager_config.port = 0;
    go2cpp::os::monitor::Manager manager(manager_config);
    Expect(manager.Start(), "Manager 应能启动（eBPF 降级不影响 Manager）");
    Expect(manager.Ingest(snapshot), "Manager 应接受降级快照");
    const auto summaries = manager.Summaries();
    Expect(summaries.size() == 1, "Summaries 应有 1 台主机");
    if (summaries.size() == 1) {
        Expect(!summaries[0].ebpf_active,
               "HostSummary.ebpf_active 应为 false");
    }
    const auto kernels = manager.KernelSummaries();
    Expect(kernels.size() == 1, "KernelSummaries 应有 1 台主机");
    if (kernels.size() == 1) {
        Expect(!kernels[0].syscalls.active &&
                   !kernels[0].syscalls.reason.empty(),
               "KernelSummary.syscalls 应保留 false + reason");
        Expect(!kernels[0].tcp.active && !kernels[0].tcp.reason.empty(),
               "KernelSummary.tcp 应保留 false + reason");
        Expect(!kernels[0].fileio.active &&
                   !kernels[0].fileio.reason.empty(),
               "KernelSummary.fileio 应保留 false + reason");
    }
    manager.Stop();
}

}  // namespace

int main() {
    if (::geteuid() == 0) {
        std::printf(
            "[ebpf_nonroot] SKIP: running as root (euid=0)，非 root 契约由普通用户"
            "运行 ctest 覆盖\n");
        return 77;  // ctest SKIP_RETURN_CODE
    }

    go2cpp_tests::announce("ebpf_nonroot");
    std::printf("[ebpf_nonroot] euid=%d（非 root）\n", static_cast<int>(::geteuid()));
    CheckNetEbpf();
    CheckKernelMonitors();
    CheckAggregator();

    const int failures =
        go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    std::printf("[ebpf_nonroot] %d 项断言，失败 %d\n", g_checks, failures);
    if (failures != 0) {
        std::fprintf(stderr, "[test] failures=%d\n", failures);
        return 1;
    }
    return 0;
}
