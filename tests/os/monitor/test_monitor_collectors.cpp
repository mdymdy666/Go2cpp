// go2cpp::os::monitor 四个 /proc 采集器 + 聚合器 + JSON 的单元测试。
//
// 覆盖点：
//   1. 每个采集器都能从真实 /proc 读到非零数据；
//   2. 相邻两次采样能算出占比/速率，数值在合法区间；
//   3. Snapshot 的 JSON 往返不丢字段；
//   4. Aggregator 的同步采集与后台 Fiber 聚合都能产出快照；
//   5. Start/Stop 幂等、并发调用安全。

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/os/monitor/aggregator.hpp"
#include "go2cpp/os/monitor/cpu_monitor.hpp"
#include "go2cpp/os/monitor/disk_monitor.hpp"
#include "go2cpp/os/monitor/mem_monitor.hpp"
#include "go2cpp/os/monitor/net_monitor.hpp"
#include "test_support.hpp"

namespace {

using go2cpp::os::monitor::Aggregator;
using go2cpp::os::monitor::AggregatorConfig;
using go2cpp::os::monitor::CpuCore;
using go2cpp::os::monitor::CpuMonitor;
using go2cpp::os::monitor::DiskMonitor;
using go2cpp::os::monitor::DiskStat;
using go2cpp::os::monitor::HealthScore;
using go2cpp::os::monitor::MemMonitor;
using go2cpp::os::monitor::NetDevStat;
using go2cpp::os::monitor::NetMonitor;
using go2cpp::os::monitor::Snapshot;
using go2cpp::os::monitor::SnapshotFromJson;
using go2cpp::os::monitor::SnapshotToJson;
using go2cpp::os::monitor::Status;

/// 在指定时长内持续做 syscall/内存分配，制造可观测的 CPU 活动。
void BurnCpu(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    volatile std::uint64_t sink = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 4096; ++i) {
            sink = sink + static_cast<std::uint64_t>(i) * 3U;
        }
    }
}

/// 每个测试进程独立的临时目录，避免 ctest 并行时互相覆盖。
const std::string& TempDir() {
    static const std::string dir =
        "/tmp/go2cpp_monitor_proc_" + std::to_string(::getpid());
    return dir;
}

void EnsureTempDir() {
    if (::mkdir(TempDir().c_str(), 0700) != 0 && errno != EEXIST) {
        GO2CPP_CHECK(false);
    }
}

std::string TempFile(const std::string& name) {
    return TempDir() + "/" + name;
}

/// 写一个伪造的 /proc 文件，供采集器的路径注入使用。
void WriteTempFile(const std::string& name, const std::string& content) {
    EnsureTempDir();
    const std::string path = TempFile(name);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    GO2CPP_CHECK(file != nullptr);
    if (file == nullptr) {
        return;
    }
    GO2CPP_CHECK(std::fwrite(content.data(), 1, content.size(), file) ==
                 content.size());
    GO2CPP_CHECK(std::fclose(file) == 0);
}

const CpuCore* FindCore(const Snapshot& snapshot, const std::string& name) {
    for (const auto& core : snapshot.cpus) {
        if (core.name == name) {
            return &core;
        }
    }
    return nullptr;
}

const DiskStat* FindDisk(const Snapshot& snapshot, const std::string& name) {
    for (const auto& disk : snapshot.disks) {
        if (disk.name == name) {
            return &disk;
        }
    }
    return nullptr;
}

const NetDevStat* FindNet(const Snapshot& snapshot, const std::string& name) {
    for (const auto& net : snapshot.nets) {
        if (net.name == name) {
            return &net;
        }
    }
    return nullptr;
}

/// /proc/stat 差分：首帧、精确公式、回绕、CPU 热插拔、Stop 清基线。
void TestCpuSynthetic() {
    go2cpp_tests::announce("cpu_synthetic_diff");
    const std::string stat_path = TempFile("stat");
    const std::string loadavg_path = TempFile("loadavg");

    WriteTempFile("loadavg", "0.50 0.40 0.30 2/345 6789\n");
    WriteTempFile("stat",
                  "cpu  1000 0 1000 8000 0 0 0 0 0 0\n"
                  "cpu0 100 0 100 800 0 0 0 0 0 0\n"
                  "cpu1 200 0 200 600 0 0 0 0 0 0\n");

    CpuMonitor monitor(stat_path, loadavg_path);
    Snapshot first;
    GO2CPP_CHECK(monitor.UpdateOnce(first) == Status::kOk);
    GO2CPP_CHECK(first.cpus.size() == 3);
    GO2CPP_CHECK(std::fabs(first.load.load1 - 0.5) < 1e-12);
    GO2CPP_CHECK(std::fabs(first.load.load5 - 0.4) < 1e-12);
    GO2CPP_CHECK(std::fabs(first.load.load15 - 0.3) < 1e-12);
    GO2CPP_CHECK(first.load.runnable == 2);
    GO2CPP_CHECK(first.load.total_threads == 345);
    // 首帧没有基线：所有占比必须是 0，不能拿累计值当分母算出垃圾。
    for (const auto& core : first.cpus) {
        GO2CPP_CHECK(core.usage_percent == 0.0);
        GO2CPP_CHECK(core.user_percent == 0.0);
        GO2CPP_CHECK(core.idle_percent == 0.0);
    }

    // 第二帧 cpu0：Δuser=100 Δsystem=200 Δidle=600 Δiowait=100，总 Δ=1000，
    // busy=300 → usage 30%、user 10%、system 20%、idle 60%、iowait 10%。
    WriteTempFile("stat",
                  "cpu  2000 0 2000 16000 0 0 0 0 0 0\n"
                  "cpu0 200 0 300 1400 100 0 0 0 0 0\n"
                  "cpu1 400 0 400 1200 0 0 0 0 0 0\n");
    Snapshot second;
    GO2CPP_CHECK(monitor.UpdateOnce(second) == Status::kOk);
    const CpuCore* cpu0 = FindCore(second, "cpu0");
    GO2CPP_CHECK(cpu0 != nullptr);
    if (cpu0 != nullptr) {
        GO2CPP_CHECK(std::fabs(cpu0->usage_percent - 30.0) < 1e-9);
        GO2CPP_CHECK(std::fabs(cpu0->user_percent - 10.0) < 1e-9);
        GO2CPP_CHECK(std::fabs(cpu0->system_percent - 20.0) < 1e-9);
        GO2CPP_CHECK(std::fabs(cpu0->idle_percent - 60.0) < 1e-9);
        GO2CPP_CHECK(std::fabs(cpu0->iowait_percent - 10.0) < 1e-9);
        // iowait 既不算 idle 也不算 busy，三者之和为 100。
        GO2CPP_CHECK(std::fabs(cpu0->usage_percent + cpu0->idle_percent +
                              cpu0->iowait_percent - 100.0) < 1e-9);
    }
    const CpuCore* cpu1 = FindCore(second, "cpu1");
    GO2CPP_CHECK(cpu1 != nullptr);
    if (cpu1 != nullptr) {
        GO2CPP_CHECK(std::fabs(cpu1->usage_percent - 40.0) < 1e-9);
    }
    // 汇总行 "cpu" 是内核对 jiffies 的聚合，不是各核百分比的平均。
    const CpuCore* total = FindCore(second, "cpu");
    GO2CPP_CHECK(total != nullptr);
    if (total != nullptr) {
        GO2CPP_CHECK(std::fabs(total->usage_percent - 20.0) < 1e-9);
    }

    // 计数器回退（热插拔复用编号/容器迁移）：饱和为 0，不得出现负值或天文数字。
    WriteTempFile("stat",
                  "cpu  30 0 30 140 0 0 0 0 0 0\n"
                  "cpu0 10 0 10 80 0 0 0 0 0 0\n"
                  "cpu1 20 0 20 60 0 0 0 0 0 0\n");
    Snapshot wrapped;
    GO2CPP_CHECK(monitor.UpdateOnce(wrapped) == Status::kOk);
    for (const auto& core : wrapped.cpus) {
        GO2CPP_CHECK(core.usage_percent == 0.0);
        GO2CPP_CHECK(core.user_percent >= 0.0);
        GO2CPP_CHECK(core.user_percent <= 100.0);
    }

    // CPU 热插拔：cpu1 消失后只剩 cpu/cpu0，cpu0 继续正常差分。
    WriteTempFile("stat",
                  "cpu  40 0 40 220 0 0 0 0 0 0\n"
                  "cpu0 20 0 20 160 0 0 0 0 0 0\n");
    Snapshot shrunk;
    GO2CPP_CHECK(monitor.UpdateOnce(shrunk) == Status::kOk);
    GO2CPP_CHECK(shrunk.cpus.size() == 2);
    GO2CPP_CHECK(FindCore(shrunk, "cpu1") == nullptr);
    if (const CpuCore* core = FindCore(shrunk, "cpu0")) {
        GO2CPP_CHECK(std::fabs(core->usage_percent - 20.0) < 1e-9);
    }

    // cpu1 重新出现：没有基线，按首帧（占比 0）处理。
    WriteTempFile("stat",
                  "cpu  60 0 60 300 0 0 0 0 0 0\n"
                  "cpu0 30 0 30 240 0 0 0 0 0 0\n"
                  "cpu1 40 0 40 60 0 0 0 0 0 0\n");
    Snapshot regrown;
    GO2CPP_CHECK(monitor.UpdateOnce(regrown) == Status::kOk);
    const CpuCore* reborn = FindCore(regrown, "cpu1");
    GO2CPP_CHECK(reborn != nullptr);
    if (reborn != nullptr) {
        GO2CPP_CHECK(reborn->usage_percent == 0.0);
    }

    // Stop 后基线清空：下一帧同样是首帧。
    monitor.Stop();
    Snapshot after_stop;
    GO2CPP_CHECK(monitor.UpdateOnce(after_stop) == Status::kOk);
    GO2CPP_CHECK(after_stop.cpus.front().usage_percent == 0.0);

    // 文件不存在 → kError，不是崩溃或垃圾值。
    CpuMonitor missing(TempFile("stat_missing"), TempFile("loadavg_missing"));
    Snapshot broken;
    GO2CPP_CHECK(missing.UpdateOnce(broken) == Status::kError);
}

/// 老内核/截断的 /proc/stat 行只有 4 个字段：tail 字段必须按 0 处理。
void TestCpuShortLineAndGarbage() {
    go2cpp_tests::announce("cpu_partial_fields");
    WriteTempFile("stat_short",
                  "cpu0 1 2 3 4\n"
                  "intr 12345 0 0\n"
                  "garbage line\n");
    CpuMonitor monitor(TempFile("stat_short"), TempFile("loadavg_missing"));
    Snapshot snapshot;
    GO2CPP_CHECK(monitor.UpdateOnce(snapshot) == Status::kOk);
    GO2CPP_CHECK(snapshot.cpus.size() == 1);
    if (!snapshot.cpus.empty()) {
        const auto& core = snapshot.cpus.front();
        GO2CPP_CHECK(core.user == 1);
        GO2CPP_CHECK(core.nice == 2);
        GO2CPP_CHECK(core.system == 3);
        GO2CPP_CHECK(core.idle == 4);
        GO2CPP_CHECK(core.iowait == 0);
        GO2CPP_CHECK(core.irq == 0);
        GO2CPP_CHECK(core.steal == 0);
        GO2CPP_CHECK(core.usage_percent == 0.0);
    }
    // loadavg 不存在不影响 CPU 采集，负载保持 0。
    GO2CPP_CHECK(snapshot.load.load1 == 0.0);
    GO2CPP_CHECK(snapshot.load.total_threads == 0);
}

/// /proc/meminfo 15 个字段、kB 单位、used_percent 公式。
void TestMemSynthetic() {
    go2cpp_tests::announce("mem_synthetic_fields");
    WriteTempFile("meminfo",
                  "MemTotal:       16384 kB\n"
                  "MemFree:         4096 kB\n"
                  "MemAvailable:   12288 kB\n"
                  "Buffers:          512 kB\n"
                  "Cached:          2048 kB\n"
                  "SwapTotal:       8192 kB\n"
                  "SwapFree:        4096 kB\n"
                  "Active:          3000 kB\n"
                  "Inactive:        2000 kB\n"
                  "Dirty:             10 kB\n"
                  "Writeback:          0 kB\n"
                  "AnonPages:       1500 kB\n"
                  "Mapped:           700 kB\n"
                  "SReclaimable:     300 kB\n"
                  "SUnreclaim:       100 kB\n");
    MemMonitor monitor(TempFile("meminfo"));
    Snapshot snapshot;
    GO2CPP_CHECK(monitor.UpdateOnce(snapshot) == Status::kOk);
    GO2CPP_CHECK(snapshot.mem.total_kb == 16384);
    GO2CPP_CHECK(snapshot.mem.free_kb == 4096);
    GO2CPP_CHECK(snapshot.mem.available_kb == 12288);
    GO2CPP_CHECK(snapshot.mem.buffers_kb == 512);
    GO2CPP_CHECK(snapshot.mem.cached_kb == 2048);
    GO2CPP_CHECK(snapshot.mem.swap_total_kb == 8192);
    GO2CPP_CHECK(snapshot.mem.swap_free_kb == 4096);
    GO2CPP_CHECK(snapshot.mem.active_kb == 3000);
    GO2CPP_CHECK(snapshot.mem.inactive_kb == 2000);
    GO2CPP_CHECK(snapshot.mem.dirty_kb == 10);
    GO2CPP_CHECK(snapshot.mem.writeback_kb == 0);
    GO2CPP_CHECK(snapshot.mem.anon_pages_kb == 1500);
    GO2CPP_CHECK(snapshot.mem.mapped_kb == 700);
    GO2CPP_CHECK(snapshot.mem.sreclaimable_kb == 300);
    GO2CPP_CHECK(snapshot.mem.sunreclaim_kb == 100);
    // (16384 - 12288) / 16384 = 25%。
    GO2CPP_CHECK(std::fabs(snapshot.mem.used_percent - 25.0) < 1e-9);
}

/// meminfo 边界：available > total 下溢、缺字段兜底、无 swap、空文件。
void TestMemEdgeCases() {
    go2cpp_tests::announce("mem_edge_cases");
    // available > total：u64 直接相减会下溢，必须夹到 0。
    WriteTempFile("meminfo_bad", "MemTotal: 1000 kB\nMemAvailable: 2000 kB\n");
    MemMonitor bad(TempFile("meminfo_bad"));
    Snapshot snapshot;
    GO2CPP_CHECK(bad.UpdateOnce(snapshot) == Status::kOk);
    GO2CPP_CHECK(snapshot.mem.used_percent == 0.0);
    GO2CPP_CHECK(snapshot.mem.used_percent <= 100.0);

    // 老内核缺 MemAvailable：退回 Free + Buffers + Cached = 300，used 70%。
    WriteTempFile("meminfo_old",
                  "MemTotal: 1000 kB\n"
                  "MemFree: 100 kB\n"
                  "Buffers: 50 kB\n"
                  "Cached: 150 kB\n");
    MemMonitor old_kernel(TempFile("meminfo_old"));
    Snapshot old_snapshot;
    GO2CPP_CHECK(old_kernel.UpdateOnce(old_snapshot) == Status::kOk);
    GO2CPP_CHECK(old_snapshot.mem.available_kb == 300);
    GO2CPP_CHECK(std::fabs(old_snapshot.mem.used_percent - 70.0) < 1e-9);

    // MemAvailable 存在且为 0 = 内存真的耗尽，不能触发兜底。
    WriteTempFile("meminfo_full",
                  "MemTotal: 1000 kB\nMemAvailable: 0 kB\nMemFree: 0 kB\n");
    MemMonitor full(TempFile("meminfo_full"));
    Snapshot full_snapshot;
    GO2CPP_CHECK(full.UpdateOnce(full_snapshot) == Status::kOk);
    GO2CPP_CHECK(full_snapshot.mem.available_kb == 0);
    GO2CPP_CHECK(std::fabs(full_snapshot.mem.used_percent - 100.0) < 1e-9);

    // SwapTotal = 0（无 swap）：不崩，也不参与 used_percent。
    WriteTempFile("meminfo_noswap",
                  "MemTotal: 1000 kB\nMemAvailable: 500 kB\n"
                  "SwapTotal: 0 kB\nSwapFree: 0 kB\n");
    MemMonitor no_swap(TempFile("meminfo_noswap"));
    Snapshot no_swap_snapshot;
    GO2CPP_CHECK(no_swap.UpdateOnce(no_swap_snapshot) == Status::kOk);
    GO2CPP_CHECK(no_swap_snapshot.mem.swap_total_kb == 0);
    GO2CPP_CHECK(std::fabs(no_swap_snapshot.mem.used_percent - 50.0) < 1e-9);

    // 空文件/文件不存在 → kError（没有分母，不能算出 0% 这种假数据）。
    WriteTempFile("meminfo_empty", "");
    MemMonitor empty(TempFile("meminfo_empty"));
    Snapshot empty_snapshot;
    GO2CPP_CHECK(empty.UpdateOnce(empty_snapshot) == Status::kError);
    MemMonitor missing(TempFile("meminfo_missing"));
    Snapshot missing_snapshot;
    GO2CPP_CHECK(missing.UpdateOnce(missing_snapshot) == Status::kError);
}

/// /proc/diskstats：分区去重、512B 扇区、await/util、回绕、空设备列表。
void TestDiskSynthetic() {
    go2cpp_tests::announce("disk_partition_and_wrap");
    // sysfs 指向空目录 → 分区判定退回名字启发式（与无 /sys 的容器一致）。
    EnsureTempDir();
    const std::string sys_root = TempDir() + "/sys_empty";
    (void)::mkdir(sys_root.c_str(), 0700);

    WriteTempFile("diskstats",
                  "   8       0 sda 100 0 800 40 50 0 400 20 0 100 60 1 0 0 0 0 0\n"
                  "   8       1 sda1 100 0 800 40 50 0 400 20 0 100 60 1 0 0 0 0 0\n"
                  "   7       0 loop0 10 0 80 4 5 0 40 2 0 10 6 1 0 0 0 0 0\n"
                  " 253       0 dm-0 10 0 80 4 5 0 40 2 0 10 6 1 0 0 0 0 0\n"
                  "   1       0 ram0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
                  "  11       0 sr0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
                  " 259       0 nvme0n1 20 0 160 8 10 0 80 4 0 20 12 1 0 0 0 0 0\n"
                  " 259       1 nvme0n1p1 20 0 160 8 10 0 80 4 0 20 12 1 0 0 0 0 0\n"
                  " 179       0 mmcblk0 30 0 240 12 15 0 120 6 0 30 18 1 0 0 0 0 0\n"
                  " 179       1 mmcblk0p1 30 0 240 12 15 0 120 6 0 30 18 1 0 0 0 0 0\n");
    DiskMonitor monitor(TempFile("diskstats"), sys_root);
    Snapshot first;
    GO2CPP_CHECK(monitor.UpdateOnce(first) == Status::kOk);
    // 只保留整盘 sda / nvme0n1 / mmcblk0；分区与虚拟设备全部过滤。
    GO2CPP_CHECK(first.disks.size() == 3);
    bool has_sda = false;
    bool has_nvme = false;
    bool has_mmc = false;
    for (const auto& disk : first.disks) {
        GO2CPP_CHECK(disk.name != "sda1");
        GO2CPP_CHECK(disk.name != "nvme0n1p1");
        GO2CPP_CHECK(disk.name != "mmcblk0p1");
        GO2CPP_CHECK(disk.read_bytes_per_sec == 0.0);
        GO2CPP_CHECK(disk.util_percent == 0.0);
        has_sda = has_sda || disk.name == "sda";
        has_nvme = has_nvme || disk.name == "nvme0n1";
        has_mmc = has_mmc || disk.name == "mmcblk0";
    }
    GO2CPP_CHECK(has_sda);
    GO2CPP_CHECK(has_nvme);
    GO2CPP_CHECK(has_mmc);

    // sda 增加：reads +10、sectors_read +16（8 KiB）、read_time +10ms、
    // writes +10、sectors_written +16、write_time +10ms、io_time +50ms。
    WriteTempFile("diskstats",
                  "   8       0 sda 110 0 816 50 60 0 416 30 0 150 60 1 0 0 0 0 0\n"
                  "   8       1 sda1 110 0 816 50 60 0 416 30 0 150 60 1 0 0 0 0 0\n"
                  " 259       0 nvme0n1 20 0 160 8 10 0 80 4 0 20 12 1 0 0 0 0 0\n"
                  " 179       0 mmcblk0 30 0 240 12 15 0 120 6 0 30 18 1 0 0 0 0 0\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Snapshot second;
    GO2CPP_CHECK(monitor.UpdateOnce(second) == Status::kOk);
    const DiskStat* sda = FindDisk(second, "sda");
    GO2CPP_CHECK(sda != nullptr);
    if (sda != nullptr) {
        GO2CPP_CHECK(sda->read_iops > 0.0);
        GO2CPP_CHECK(sda->write_iops > 0.0);
        GO2CPP_CHECK(sda->read_bytes_per_sec > 0.0);
        GO2CPP_CHECK(sda->write_bytes_per_sec > 0.0);
        // await = io_ms / ios：read_time 10ms / 10 次 = 1ms。
        GO2CPP_CHECK(std::fabs(sda->avg_read_latency_ms - 1.0) < 1e-9);
        GO2CPP_CHECK(std::fabs(sda->avg_write_latency_ms - 1.0) < 1e-9);
        GO2CPP_CHECK(sda->util_percent > 0.0);
        GO2CPP_CHECK(sda->util_percent <= 100.0);
    }

    // 计数器回退：按文档语义「本次值即增量」，不得出现负值。
    WriteTempFile("diskstats",
                  "   8       0 sda 5 0 8 1 2 0 8 1 0 3 2 1 0 0 0 0 0\n"
                  " 259       0 nvme0n1 20 0 160 8 10 0 80 4 0 20 12 1 0 0 0 0 0\n"
                  " 179       0 mmcblk0 30 0 240 12 15 0 120 6 0 30 18 1 0 0 0 0 0\n");
    Snapshot wrapped;
    GO2CPP_CHECK(monitor.UpdateOnce(wrapped) == Status::kOk);
    const DiskStat* wrapped_sda = FindDisk(wrapped, "sda");
    GO2CPP_CHECK(wrapped_sda != nullptr);
    if (wrapped_sda != nullptr) {
        GO2CPP_CHECK(wrapped_sda->read_bytes_per_sec >= 0.0);
        GO2CPP_CHECK(wrapped_sda->read_iops >= 0.0);
    }

    // 只有虚拟设备（容器常见）：不是错误，disks 为空。
    WriteTempFile("diskstats",
                  "   7       0 loop0 10 0 80 4 5 0 40 2 0 10 6 1 0 0 0 0 0\n"
                  " 253       0 dm-0 10 0 80 4 5 0 40 2 0 10 6 1 0 0 0 0 0\n");
    DiskMonitor only_virtual(TempFile("diskstats"), sys_root);
    Snapshot virtual_snapshot;
    GO2CPP_CHECK(only_virtual.UpdateOnce(virtual_snapshot) == Status::kOk);
    GO2CPP_CHECK(virtual_snapshot.disks.empty());

    // 空文件 → kError；老内核 7 字段行 → 正常解析、缺失字段按 0。
    WriteTempFile("diskstats", "");
    DiskMonitor empty(TempFile("diskstats"), sys_root);
    Snapshot empty_snapshot;
    GO2CPP_CHECK(empty.UpdateOnce(empty_snapshot) == Status::kError);
    WriteTempFile("diskstats", "   8       0 sda 1 0 8 4 2 0 8\n");
    DiskMonitor short_line(TempFile("diskstats"), sys_root);
    Snapshot short_snapshot;
    GO2CPP_CHECK(short_line.UpdateOnce(short_snapshot) == Status::kOk);
    GO2CPP_CHECK(short_snapshot.disks.size() == 1);
    if (!short_snapshot.disks.empty()) {
        const auto& disk = short_snapshot.disks.front();
        GO2CPP_CHECK(disk.reads == 1);
        GO2CPP_CHECK(disk.writes == 2);
        GO2CPP_CHECK(disk.sectors_written == 8);
        GO2CPP_CHECK(disk.write_time_ms == 0);
        GO2CPP_CHECK(disk.io_time_ms == 0);
    }

    (void)::rmdir(sys_root.c_str());
}

/// /proc/net/dev：速率单位、错误/丢包、接口热插拔、短行与空文件。
void TestNetSynthetic() {
    go2cpp_tests::announce("net_synthetic_diff");
    const std::string header =
        "Inter-|   Receive                                                |  "
        "Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed "
        "multicast|bytes    packets errs drop fifo colls carrier compressed\n";

    WriteTempFile("netdev", header +
                  "    lo:  1000 10 0 0 0 0 0 0 1000 10 0 0 0 0 0 0\n"
                  "  eth0: 2000 20 1 2 0 0 0 0 3000 30 3 4 0 0 0 0\n"
                  "docker0: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    NetMonitor monitor(TempFile("netdev"));
    Snapshot first;
    GO2CPP_CHECK(monitor.UpdateOnce(first) == Status::kOk);
    GO2CPP_CHECK(first.nets.size() == 3);
    const NetDevStat* eth0 = FindNet(first, "eth0");
    GO2CPP_CHECK(eth0 != nullptr);
    if (eth0 != nullptr) {
        GO2CPP_CHECK(eth0->rcv_bytes == 2000);
        GO2CPP_CHECK(eth0->rcv_packets == 20);
        GO2CPP_CHECK(eth0->rcv_errs == 1);
        GO2CPP_CHECK(eth0->rcv_drops == 2);
        GO2CPP_CHECK(eth0->snd_bytes == 3000);
        GO2CPP_CHECK(eth0->snd_errs == 3);
        GO2CPP_CHECK(eth0->snd_drops == 4);
        GO2CPP_CHECK(eth0->rcv_rate == 0.0);
        GO2CPP_CHECK(eth0->snd_rate == 0.0);
    }

    // 第二帧 eth0 增加 2000 字节 / 20 包；速率单位是 bytes/s 与 packets/s。
    WriteTempFile("netdev", header +
                  "    lo:  1000 10 0 0 0 0 0 0 1000 10 0 0 0 0 0 0\n"
                  "  eth0: 4000 40 1 2 0 0 0 0 5000 50 3 4 0 0 0 0\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Snapshot second;
    GO2CPP_CHECK(monitor.UpdateOnce(second) == Status::kOk);
    GO2CPP_CHECK(second.nets.size() == 2);  // docker0 消失
    const NetDevStat* eth0_second = FindNet(second, "eth0");
    GO2CPP_CHECK(eth0_second != nullptr);
    if (eth0_second != nullptr) {
        GO2CPP_CHECK(eth0_second->rcv_rate > 0.0);
        GO2CPP_CHECK(eth0_second->snd_rate > 0.0);
        GO2CPP_CHECK(eth0_second->rcv_packets_rate > 0.0);
        GO2CPP_CHECK(eth0_second->snd_packets_rate > 0.0);
        // 2000 字节 / 20 包 = 100 bytes/包，两个速率共用同一个 Δt。
        GO2CPP_CHECK(std::fabs(eth0_second->rcv_rate /
                                   eth0_second->rcv_packets_rate -
                               100.0) < 1e-6);
    }

    // docker0 重新出现：按首帧处理，速率 0，不能把累计值当本窗增量。
    WriteTempFile("netdev", header +
                  "    lo:  1000 10 0 0 0 0 0 0 1000 10 0 0 0 0 0 0\n"
                  "  eth0: 4000 40 1 2 0 0 0 0 5000 50 3 4 0 0 0 0\n"
                  "docker0: 500 5 0 0 0 0 0 0 600 6 0 0 0 0 0 0\n");
    Snapshot third;
    GO2CPP_CHECK(monitor.UpdateOnce(third) == Status::kOk);
    const NetDevStat* docker = FindNet(third, "docker0");
    GO2CPP_CHECK(docker != nullptr);
    if (docker != nullptr) {
        GO2CPP_CHECK(docker->rcv_rate == 0.0);
        GO2CPP_CHECK(docker->rcv_bytes == 500);
    }

    // 空文件/不存在 → kError。
    WriteTempFile("netdev", "");
    NetMonitor empty(TempFile("netdev"));
    Snapshot empty_snapshot;
    GO2CPP_CHECK(empty.UpdateOnce(empty_snapshot) == Status::kError);
    NetMonitor missing(TempFile("netdev_missing"));
    Snapshot missing_snapshot;
    GO2CPP_CHECK(missing.UpdateOnce(missing_snapshot) == Status::kError);
}

/// JSON 字段单位一致性与 HealthScore 的 CPU 核数回归。
void TestJsonUnitsAndHealthScore() {
    go2cpp_tests::announce("json_units_and_health");
    Snapshot snapshot;
    CpuCore cpu0;
    cpu0.name = "cpu0";
    cpu0.usage_percent = 12.5;
    snapshot.cpus.push_back(cpu0);
    DiskStat disk;
    disk.name = "sda";
    disk.read_bytes_per_sec = 1.5;
    disk.util_percent = 25.0;
    snapshot.disks.push_back(disk);
    NetDevStat net;
    net.name = "eth0";
    net.rcv_rate = 2.5;
    snapshot.nets.push_back(net);
    snapshot.mem.total_kb = 1024;
    snapshot.mem.used_percent = 12.5;

    const std::string text = SnapshotToJson(snapshot);
    // 字段名自带单位，避免 bytes/kB、速率/累计值混淆。
    GO2CPP_CHECK(text.find("\"total_kb\":1024") != std::string::npos);
    GO2CPP_CHECK(text.find("\"read_bytes_per_sec\":1.5") != std::string::npos);
    GO2CPP_CHECK(text.find("\"rcv_rate\":2.5") != std::string::npos);
    GO2CPP_CHECK(text.find("\"usage_percent\":12.5") != std::string::npos);

    // 汇总行 100% + 两个 0% 分核：修复后 CPU 项为 0%，总分 100；
    // 修复前会把汇总行当一核，平均成 33.3%，总分 86.7。
    Snapshot health;
    CpuCore total;
    total.name = "cpu";
    total.usage_percent = 100.0;
    CpuCore idle0;
    idle0.name = "cpu0";
    CpuCore idle1;
    idle1.name = "cpu1";
    health.cpus = {total, idle0, idle1};
    health.load.load1 = 0.0;
    GO2CPP_CHECK(std::fabs(HealthScore(health) - 100.0) < 1e-9);

    // 只有汇总行时退回用汇总行，不能变成「0 个核」按 0 计。
    Snapshot only_total;
    only_total.cpus = {total};
    only_total.load.load1 = 0.0;
    GO2CPP_CHECK(std::fabs(HealthScore(only_total) - 60.0) < 1e-9);
}

void TestCpuMonitor() {
    go2cpp_tests::announce("cpu_monitor");
    CpuMonitor monitor;
    Snapshot first;
    GO2CPP_CHECK(monitor.UpdateOnce(first) == Status::kOk);
    GO2CPP_CHECK(!first.cpus.empty());
    GO2CPP_CHECK(first.cpus.front().name == "cpu");
    // /proc/stat 的累计 jiffies 必须非零。
    GO2CPP_CHECK(first.cpus.front().idle > 0);
    GO2CPP_CHECK(first.load.total_threads > 0);

    BurnCpu(std::chrono::milliseconds(120));
    Snapshot second;
    GO2CPP_CHECK(monitor.UpdateOnce(second) == Status::kOk);
    GO2CPP_CHECK(second.cpus.size() == first.cpus.size());
    // 第二次采样后汇总核的使用率应在 [0, 100] 且大于 0（刚才烧了 CPU）。
    const double usage = second.cpus.front().usage_percent;
    GO2CPP_CHECK(usage >= 0.0);
    GO2CPP_CHECK(usage <= 100.5);
    GO2CPP_CHECK(usage > 0.0);
    // 各分项占比之和应接近 100%（允许浮点误差与 guest 字段不参与）。
    double sum = 0.0;
    for (const auto& core : second.cpus) {
        sum = core.user_percent + core.system_percent + core.nice_percent +
              core.idle_percent + core.iowait_percent + core.irq_percent +
              core.softirq_percent;
        GO2CPP_CHECK(sum > 99.0 && sum < 101.0);
    }
    // 每核也有独立条目（WSL2 至少 1 个 cpuN）。
    bool has_core = false;
    for (const auto& core : second.cpus) {
        if (core.name.rfind("cpu", 0) == 0 && core.name != "cpu") {
            has_core = true;
            break;
        }
    }
    GO2CPP_CHECK(has_core);
    monitor.Stop();
    monitor.Stop();  // 幂等
}

void TestMemMonitor() {
    go2cpp_tests::announce("mem_monitor");
    MemMonitor monitor;
    Snapshot snapshot;
    GO2CPP_CHECK(monitor.UpdateOnce(snapshot) == Status::kOk);
    GO2CPP_CHECK(snapshot.mem.total_kb > 0);
    GO2CPP_CHECK(snapshot.mem.free_kb <= snapshot.mem.total_kb);
    GO2CPP_CHECK(snapshot.mem.available_kb <= snapshot.mem.total_kb);
    GO2CPP_CHECK(snapshot.mem.available_kb > 0);
    GO2CPP_CHECK(snapshot.mem.used_percent >= 0.0);
    GO2CPP_CHECK(snapshot.mem.used_percent <= 100.0);
    GO2CPP_CHECK(snapshot.mem.buffers_kb + snapshot.mem.cached_kb > 0);
    GO2CPP_CHECK(monitor.sample_count() == 1);
    monitor.Stop();
}

void TestDiskMonitor() {
    go2cpp_tests::announce("disk_monitor");
    DiskMonitor monitor;
    Snapshot first;
    GO2CPP_CHECK(monitor.UpdateOnce(first) == Status::kOk);
    GO2CPP_CHECK(!first.disks.empty());
    for (const auto& disk : first.disks) {
        GO2CPP_CHECK(!disk.name.empty());
        // 首次采样没有差分，速率全 0。
        GO2CPP_CHECK(disk.read_bytes_per_sec == 0.0);
    }

    // 制造一次落盘：写临时文件并 fsync。
    const std::string path = "/tmp/go2cpp_monitor_disk_probe.bin";
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        GO2CPP_CHECK(file != nullptr);
        if (file != nullptr) {
            std::string payload(64 * 1024, 'x');
            (void)std::fwrite(payload.data(), 1, payload.size(), file);
            (void)std::fflush(file);
            (void)::fsync(::fileno(file));
            std::fclose(file);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Snapshot second;
    GO2CPP_CHECK(monitor.UpdateOnce(second) == Status::kOk);
    GO2CPP_CHECK(second.disks.size() == first.disks.size());
    bool any_activity = false;
    for (const auto& disk : second.disks) {
        GO2CPP_CHECK(disk.util_percent >= 0.0);
        GO2CPP_CHECK(disk.util_percent <= 100.5);
        if (disk.write_bytes_per_sec > 0.0 || disk.read_bytes_per_sec > 0.0) {
            any_activity = true;
        }
    }
    GO2CPP_CHECK(any_activity);
    (void)std::remove(path.c_str());
    monitor.Stop();
}

void TestNetMonitor() {
    go2cpp_tests::announce("net_monitor");
    NetMonitor monitor;
    Snapshot first;
    GO2CPP_CHECK(monitor.UpdateOnce(first) == Status::kOk);
    GO2CPP_CHECK(!first.nets.empty());
    bool has_eth0 = false;
    for (const auto& net : first.nets) {
        GO2CPP_CHECK(!net.name.empty());
        if (net.name == "eth0") {
            has_eth0 = true;
            GO2CPP_CHECK(net.rcv_bytes > 0);
        }
    }
    GO2CPP_CHECK(has_eth0);

    // 通过 loopback 走一次 TCP，至少让 eth0 上的计数继续增长。
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Snapshot second;
    GO2CPP_CHECK(monitor.UpdateOnce(second) == Status::kOk);
    GO2CPP_CHECK(second.nets.size() == first.nets.size());
    for (const auto& net : second.nets) {
        GO2CPP_CHECK(net.rcv_rate >= 0.0);
        GO2CPP_CHECK(net.snd_rate >= 0.0);
        GO2CPP_CHECK(net.rcv_packets_rate >= 0.0);
    }
    monitor.Stop();
}

void TestJsonRoundTrip() {
    go2cpp_tests::announce("json_round_trip");
    Aggregator aggregator;
    Snapshot snapshot = aggregator.CollectNow();
    snapshot.host = "unit-test-host";
    snapshot.timestamp_ms = 123456789;
    snapshot.ebpf_active = true;
    snapshot.ebpf_reason.clear();

    const std::string text = SnapshotToJson(snapshot);
    GO2CPP_CHECK(!text.empty());
    GO2CPP_CHECK(text.find("unit-test-host") != std::string::npos);

    Snapshot restored;
    std::string error;
    GO2CPP_CHECK(SnapshotFromJson(text, restored, &error));
    GO2CPP_CHECK(error.empty());
    GO2CPP_CHECK(restored.host == snapshot.host);
    GO2CPP_CHECK(restored.timestamp_ms == snapshot.timestamp_ms);
    GO2CPP_CHECK(restored.cpus.size() == snapshot.cpus.size());
    GO2CPP_CHECK(restored.mem.total_kb == snapshot.mem.total_kb);
    GO2CPP_CHECK(restored.disks.size() == snapshot.disks.size());
    GO2CPP_CHECK(restored.nets.size() == snapshot.nets.size());
    GO2CPP_CHECK(restored.ebpf_active);
    GO2CPP_CHECK(restored.cpus.front().name == snapshot.cpus.front().name);

    // 非法 JSON 必须返回 false 而不是抛异常。
    Snapshot broken;
    GO2CPP_CHECK(!SnapshotFromJson("{not json", broken, &error));
    GO2CPP_CHECK(!error.empty());

    const double score = HealthScore(snapshot);
    GO2CPP_CHECK(score >= 0.0);
    GO2CPP_CHECK(score <= 100.0);
}

void TestAggregatorSynchronous() {
    go2cpp_tests::announce("aggregator_collect_now");
    AggregatorConfig config;
    config.interval = std::chrono::milliseconds(50);
    config.enable_ebpf = false;  // 同步路径只验证 /proc 采集器
    Aggregator aggregator(config);

    Snapshot snapshot = aggregator.CollectNow();
    GO2CPP_CHECK(!snapshot.host.empty());
    GO2CPP_CHECK(snapshot.timestamp_ms > 0);
    GO2CPP_CHECK(!snapshot.cpus.empty());
    GO2CPP_CHECK(snapshot.mem.total_kb > 0);
    GO2CPP_CHECK(!snapshot.disks.empty());
    GO2CPP_CHECK(!snapshot.nets.empty());
    GO2CPP_CHECK(!snapshot.host_info.kernel.empty());
}

void TestAggregatorBackground() {
    go2cpp_tests::announce("aggregator_background_fibers");
    AggregatorConfig config;
    config.interval = std::chrono::milliseconds(50);
    config.enable_ebpf = false;
    config.workers = 4;
    Aggregator aggregator(config);

    GO2CPP_CHECK(aggregator.Start());
    GO2CPP_CHECK(aggregator.running());
    GO2CPP_CHECK(!aggregator.Start());  // 重复 Start 返回 false

    // 等后台 Fiber 至少产出两轮快照，并且 latest 已被各类采集器填满。
    // Latest() 是「最近已知状态」，各采集器结果按 Channel 到达顺序合并，
    // 刚启动时可能只填了 CPU/内存，因此要等到网卡/磁盘也写入后再断言。
    GO2CPP_REQUIRE_EVENTUALLY(aggregator.Stats().snapshots >= 2,
                              std::chrono::seconds(10));
    GO2CPP_REQUIRE_EVENTUALLY(
        !aggregator.Latest().cpus.empty() &&
            aggregator.Latest().mem.total_kb > 0 &&
            !aggregator.Latest().nets.empty() &&
            !aggregator.Latest().disks.empty(),
        std::chrono::seconds(10));

    const Snapshot latest = aggregator.Latest();
    GO2CPP_CHECK(!latest.cpus.empty());
    GO2CPP_CHECK(latest.mem.total_kb > 0);
    GO2CPP_CHECK(!latest.nets.empty());
    GO2CPP_CHECK(!latest.disks.empty());

    const auto stats = aggregator.Stats();
    GO2CPP_CHECK(stats.snapshots >= 2);
    GO2CPP_CHECK(stats.errors == 0);

    aggregator.Stop();
    GO2CPP_CHECK(!aggregator.running());
    aggregator.Stop();  // 幂等
}

void TestAggregatorConcurrentStartStop() {
    go2cpp_tests::announce("aggregator_concurrent_start_stop");
    AggregatorConfig config;
    config.interval = std::chrono::milliseconds(20);
    config.enable_ebpf = false;
    config.workers = 2;
    Aggregator aggregator(config);

    // 并发 Start/Stop：只允许一个 Start 成功，Stop 可重复。
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&aggregator]() {
            if (aggregator.Start()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                aggregator.Stop();
            }
        });
    }
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, std::chrono::seconds(30));
    }
    aggregator.Stop();
    GO2CPP_CHECK(!aggregator.running());
}

}  // namespace

int main() {
    TestCpuMonitor();
    TestMemMonitor();
    TestDiskMonitor();
    TestNetMonitor();
    TestJsonRoundTrip();
    TestAggregatorSynchronous();
    TestAggregatorBackground();
    TestAggregatorConcurrentStartStop();
    TestCpuSynthetic();
    TestCpuShortLineAndGarbage();
    TestMemSynthetic();
    TestMemEdgeCases();
    TestDiskSynthetic();
    TestNetSynthetic();
    TestJsonUnitsAndHealthScore();

    // 清理临时文件与目录，测试可重复执行、不留残留。
    for (const char* name :
         {"stat", "stat_short", "stat_missing", "loadavg", "loadavg_missing",
          "meminfo", "meminfo_bad", "meminfo_old", "meminfo_full",
          "meminfo_noswap", "meminfo_empty", "meminfo_missing", "diskstats",
          "netdev", "netdev_missing"}) {
        (void)std::remove(TempFile(name).c_str());
    }
    (void)::rmdir(TempDir().c_str());

    const int failures = go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::fprintf(stderr, "[test] failures=%d\n", failures);
        return 1;
    }
    std::printf("[test] all monitor collector checks passed\n");
    return 0;
}
