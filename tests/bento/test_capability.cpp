/**
 * @file test_capability.cpp
 * @brief MOD-37 环境能力探测测试：正常环境、矩阵 API、/proc 缺失/不可读、
 *        rlimit 阈值、cgroup v1/v2/混合、容器限制、畸形输入、探测开关与
 *        随机环境变体。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include "capability_fixture.hpp"
#include "go2cpp/bento/app/capability.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::app::CapabilityItem;
using go2cpp::bento::app::CapabilityMatrix;
using go2cpp::bento::app::CapabilityStatus;
using go2cpp::bento::app::CapabilityStatusName;
using go2cpp::bento::app::Probe;
using go2cpp::bento::app::ProbeOptions;

using capability_fixture::Join;
using capability_fixture::MakeDir;
using capability_fixture::PopulateCgroupV1;
using capability_fixture::PopulateCgroupV2;
using capability_fixture::PopulateProc;
using capability_fixture::SetMode;
using capability_fixture::TempTree;
using capability_fixture::WriteFile;

/// 15 项的稳定顺序（与头文件表格一致）。
const std::array<const char*, 15>& ExpectedNames() {
    static const std::array<const char*, 15> kNames = {
        "proc.meminfo",   "proc.cpuinfo",   "proc.self.status", "proc.self.fd",
        "unix.privilege", "ipc.epoll",      "cgroup.version",   "cgroup.memory",
        "cgroup.cpu",     "net.interfaces", "fs.tmpfs",         "fs.root_readonly",
        "rlimit.nofile",  "rlimit.nproc",   "kernel.info"};
    return kNames;
}

/// 每个场景一个临时夹具根；析构清理。
class Fixture {
public:
    Fixture() {
        options.proc_root = Join(tree.root(), "proc");
        options.cgroup_root = Join(tree.root(), "cgroup");
        options.tmp_path = Join(tree.root(), "tmpdir");
        options.root_path = tree.root();
        options.min_mem_available = capability_fixture::kMinMemBytes;
        MakeDir(options.tmp_path);
        MakeDir(options.proc_root);
        MakeDir(options.cgroup_root);
    }

    bool valid() const { return tree.valid(); }

    TempTree tree;
    ProbeOptions options;
};

const CapabilityItem* Find(const CapabilityMatrix& matrix, const char* name) {
    return matrix.Find(name);
}

/// 不变量：项数、顺序、名称唯一、三态划分、Find/Count/Reasons/Summary 一致。
void CheckMatrixInvariants(const CapabilityMatrix& matrix) {
    BENTO_CHECK_EQ(matrix.Size(), std::size_t{15});
    BENTO_CHECK(!matrix.Empty());
    const auto& names = ExpectedNames();
    std::size_t available = 0;
    std::size_t degraded = 0;
    std::size_t unavailable = 0;
    for (std::size_t index = 0; index < matrix.items.size(); ++index) {
        const CapabilityItem& item = matrix.items[index];
        BENTO_CHECK(!item.name.empty());
        if (index < names.size()) {
            BENTO_CHECK(item.name == names[index]);
            BENTO_CHECK(matrix.Find(item.name) == &item);
        }
        switch (item.status) {
            case CapabilityStatus::kAvailable:
                ++available;
                BENTO_CHECK(item.reason.empty());
                BENTO_CHECK(!item.detail.empty());
                break;
            case CapabilityStatus::kDegraded:
                ++degraded;
                BENTO_CHECK(!item.reason.empty());
                break;
            case CapabilityStatus::kUnavailable:
                ++unavailable;
                BENTO_CHECK(!item.reason.empty());
                break;
        }
    }
    BENTO_CHECK_EQ(available, matrix.Count(CapabilityStatus::kAvailable));
    BENTO_CHECK_EQ(degraded, matrix.Count(CapabilityStatus::kDegraded));
    BENTO_CHECK_EQ(unavailable, matrix.Count(CapabilityStatus::kUnavailable));
    BENTO_CHECK_EQ(available + degraded + unavailable, matrix.Size());
    BENTO_CHECK_EQ(matrix.IsDegraded(), available != matrix.Size());
    BENTO_CHECK_EQ(matrix.Reasons().size(), matrix.Size() - available);
    BENTO_CHECK(matrix.Summary() == "capabilities: total=15 available=" +
                                        std::to_string(available) + " degraded=" +
                                        std::to_string(degraded) + " unavailable=" +
                                        std::to_string(unavailable));
    BENTO_CHECK(matrix.Find("does.not.exist") == nullptr);
}

void ExpectStatus(const CapabilityMatrix& matrix, const char* name, CapabilityStatus status) {
    const CapabilityItem* item = Find(matrix, name);
    BENTO_CHECK(item != nullptr);
    if (item != nullptr) {
        BENTO_CHECK(item->status == status);
        if (status != CapabilityStatus::kAvailable) {
            BENTO_CHECK(!item->reason.empty());
        }
    }
}

void ExpectReason(const CapabilityMatrix& matrix, const char* name, std::string_view needle) {
    const CapabilityItem* item = Find(matrix, name);
    BENTO_CHECK(item != nullptr);
    if (item != nullptr) {
        if (item->reason.find(needle) == std::string::npos) {
            std::fprintf(stderr, "[REASON] %s missing \"%s\" in \"%s\"\n", name,
                         std::string(needle).c_str(), item->reason.c_str());
        }
        BENTO_CHECK(item->reason.find(needle) != std::string::npos);
    }
}

void ExpectDetail(const CapabilityMatrix& matrix, const char* name, std::string_view needle) {
    const CapabilityItem* item = Find(matrix, name);
    BENTO_CHECK(item != nullptr);
    if (item != nullptr) {
        if (item->detail.find(needle) == std::string::npos) {
            std::fprintf(stderr, "[DETAIL] %s missing \"%s\" in \"%s\"\n", name,
                         std::string(needle).c_str(), item->detail.c_str());
        }
        BENTO_CHECK(item->detail.find(needle) != std::string::npos);
    }
}

/// 场景 1：正常 Linux 环境。
void TestNormalEnvironment() {
    const CapabilityMatrix matrix = Probe();
    CheckMatrixInvariants(matrix);

    const std::size_t available = matrix.Count(CapabilityStatus::kAvailable);
    BENTO_CHECK(available >= 8);
    // 基线机器为 12 available / 3 degraded（cgroup.memory、cgroup.cpu、
    // fs.tmpfs）；其他宿主允许不同，但三态必须合法。
    for (const CapabilityItem& item : matrix.items) {
        BENTO_CHECK(std::string(CapabilityStatusName(item.status)) != "unknown");
        if (item.available()) {
            BENTO_CHECK(!item.detail.empty());
        }
    }
    BENTO_CHECK(matrix.Find("proc.meminfo") != nullptr);
    BENTO_CHECK(matrix.Find("proc.cpuinfo") != nullptr);
    BENTO_CHECK(matrix.Find("proc.self.status") != nullptr);
    BENTO_CHECK(matrix.Find("proc.self.fd") != nullptr);
    BENTO_CHECK(matrix.Find("unix.privilege") != nullptr);
    BENTO_CHECK(matrix.Find("ipc.epoll") != nullptr);
    BENTO_CHECK(matrix.Find("cgroup.version") != nullptr);
    BENTO_CHECK(matrix.Find("cgroup.memory") != nullptr);
    BENTO_CHECK(matrix.Find("cgroup.cpu") != nullptr);
    BENTO_CHECK(matrix.Find("net.interfaces") != nullptr);
    BENTO_CHECK(matrix.Find("fs.tmpfs") != nullptr);
    BENTO_CHECK(matrix.Find("fs.root_readonly") != nullptr);
    BENTO_CHECK(matrix.Find("rlimit.nofile") != nullptr);
    BENTO_CHECK(matrix.Find("rlimit.nproc") != nullptr);
    BENTO_CHECK(matrix.Find("kernel.info") != nullptr);

    const CapabilityItem* meminfo = matrix.Find("proc.meminfo");
    BENTO_CHECK(meminfo != nullptr && meminfo->available());
    if (meminfo != nullptr) {
        BENTO_CHECK(meminfo->detail.find("MemAvailable=") != std::string::npos);
    }
    const CapabilityItem* privilege = matrix.Find("unix.privilege");
    BENTO_CHECK(privilege != nullptr && privilege->available());
    if (privilege != nullptr) {
        BENTO_CHECK(privilege->detail.find("uid=") != std::string::npos);
    }

    // 非法枚举值退化为 "unknown"，不能越界。
    BENTO_CHECK(std::string(CapabilityStatusName(static_cast<CapabilityStatus>(99))) == "unknown");
}

/// 场景 2：手工构造矩阵，验证 Count/Find/IsDegraded/Reasons 的纯逻辑。
void TestMatrixApi() {
    CapabilityMatrix matrix;
    BENTO_CHECK(matrix.Empty());
    BENTO_CHECK_EQ(matrix.Size(), std::size_t{0});
    BENTO_CHECK(!matrix.IsDegraded());
    BENTO_CHECK(matrix.Reasons().empty());
    BENTO_CHECK(matrix.Summary() == "capabilities: total=0 available=0 degraded=0 unavailable=0");

    CapabilityItem available;
    available.name = "alpha";
    available.status = CapabilityStatus::kAvailable;
    CapabilityItem degraded;
    degraded.name = "beta";
    degraded.status = CapabilityStatus::kDegraded;
    degraded.reason = "threshold not met";
    CapabilityItem unavailable;
    unavailable.name = "gamma";
    unavailable.status = CapabilityStatus::kUnavailable;

    matrix.items.push_back(available);
    matrix.items.push_back(degraded);
    matrix.items.push_back(unavailable);
    BENTO_CHECK_EQ(matrix.Size(), std::size_t{3});
    BENTO_CHECK(matrix.IsDegraded());
    BENTO_CHECK_EQ(matrix.Count(CapabilityStatus::kAvailable), std::size_t{1});
    BENTO_CHECK_EQ(matrix.Count(CapabilityStatus::kDegraded), std::size_t{1});
    BENTO_CHECK_EQ(matrix.Count(CapabilityStatus::kUnavailable), std::size_t{1});
    BENTO_CHECK(matrix.Find("alpha") != nullptr);
    BENTO_CHECK(matrix.Find("beta") != nullptr);
    BENTO_CHECK(matrix.Find("delta") == nullptr);

    const std::vector<std::string> reasons = matrix.Reasons();
    BENTO_CHECK_EQ(reasons.size(), std::size_t{2});
    BENTO_CHECK(reasons[0] == "beta: threshold not met");
    // 无 reason 的不可用项退化为状态名，保证日志可用。
    BENTO_CHECK(reasons[1] == "gamma: unavailable");
}

/// 场景 3：/proc 路径不可读时，相关项必须不可用并给出路径原因。
void TestMissingProc() {
    ProbeOptions options;
    options.proc_root = "/nonexistent-go2cpp-proc-root";
    options.cgroup_root = "/nonexistent-go2cpp-cgroup-root";
    const CapabilityMatrix matrix = Probe(options);

    for (const char* name : {"proc.meminfo", "proc.cpuinfo", "proc.self.status", "proc.self.fd"}) {
        ExpectStatus(matrix, name, CapabilityStatus::kUnavailable);
        const CapabilityItem* item = matrix.Find(name);
        if (item != nullptr) {
            BENTO_CHECK(item->reason.find("nonexistent-go2cpp-proc-root") != std::string::npos);
        }
    }
    BENTO_CHECK(matrix.IsDegraded());
    const std::vector<std::string> reasons = matrix.Reasons();
    BENTO_CHECK(reasons.size() >= 4);
    bool found_meminfo = false;
    for (const std::string& reason : reasons) {
        if (reason.find("proc.meminfo") == 0) {
            found_meminfo = true;
        }
    }
    BENTO_CHECK(found_meminfo);

    // 其余项不依赖 /proc，仍应可用（epoll/网络/文件系统）。
    ExpectStatus(matrix, "ipc.epoll", CapabilityStatus::kAvailable);
}

/// 场景 4：rlimit 阈值必须按软上限判定；不修改进程级限制，避免 SKIP 逃逸。
void TestRlimitThresholds() {
    struct rlimit nofile {};
    BENTO_CHECK_EQ(::getrlimit(RLIMIT_NOFILE, &nofile), 0);
    ProbeOptions options;
    if (nofile.rlim_cur == RLIM_INFINITY) {
        options.min_nofile = UINT64_MAX;
        const CapabilityMatrix matrix = Probe(options);
        ExpectStatus(matrix, "rlimit.nofile", CapabilityStatus::kAvailable);
        ExpectDetail(matrix, "rlimit.nofile", "infinity");
    } else {
        const std::uint64_t soft = static_cast<std::uint64_t>(nofile.rlim_cur);
        options.min_nofile = soft + 1;
        const CapabilityMatrix degraded_matrix = Probe(options);
        ExpectStatus(degraded_matrix, "rlimit.nofile", CapabilityStatus::kDegraded);
        ExpectReason(degraded_matrix, "rlimit.nofile",
                     "min_nofile=" + std::to_string(soft + 1));
        ExpectDetail(degraded_matrix, "rlimit.nofile", "soft=" + std::to_string(soft));

        options.min_nofile = soft;
        const CapabilityMatrix available_matrix = Probe(options);
        ExpectStatus(available_matrix, "rlimit.nofile", CapabilityStatus::kAvailable);
    }

    struct rlimit nproc {};
    BENTO_CHECK_EQ(::getrlimit(RLIMIT_NPROC, &nproc), 0);
    ProbeOptions nproc_options;
    if (nproc.rlim_cur == RLIM_INFINITY) {
        nproc_options.min_nproc = UINT64_MAX;
        const CapabilityMatrix matrix = Probe(nproc_options);
        ExpectStatus(matrix, "rlimit.nproc", CapabilityStatus::kAvailable);
    } else {
        const std::uint64_t soft = static_cast<std::uint64_t>(nproc.rlim_cur);
        nproc_options.min_nproc = soft + 1;
        const CapabilityMatrix degraded_matrix = Probe(nproc_options);
        ExpectStatus(degraded_matrix, "rlimit.nproc", CapabilityStatus::kDegraded);
        nproc_options.min_nproc = 0;
        const CapabilityMatrix available_matrix = Probe(nproc_options);
        ExpectStatus(available_matrix, "rlimit.nproc", CapabilityStatus::kAvailable);
    }
}

/// 场景 5：cgroup 根目录缺失时，版本/内存/CPU 三项都必须降级并说明原因。
void TestMissingCgroup() {
    ProbeOptions options;
    options.cgroup_root = "/nonexistent-go2cpp-cgroup";
    const CapabilityMatrix matrix = Probe(options);
    for (const char* name : {"cgroup.version", "cgroup.memory", "cgroup.cpu"}) {
        ExpectStatus(matrix, name, CapabilityStatus::kDegraded);
    }
    ExpectReason(matrix, "cgroup.version", "nonexistent-go2cpp-cgroup");
}

/// 场景 6：显式关闭各探测开关时，对应项应给出「禁用」原因而不是假可用。
void TestDisabledChecks() {
    ProbeOptions options;
    options.check_epoll = false;
    options.check_interfaces = false;
    options.check_cgroup = false;
    options.check_rlimit = false;
    const CapabilityMatrix matrix = Probe(options);

    for (const char* name : {"ipc.epoll", "net.interfaces", "cgroup.version", "cgroup.memory",
                             "cgroup.cpu", "rlimit.nofile", "rlimit.nproc"}) {
        ExpectStatus(matrix, name, CapabilityStatus::kDegraded);
        const CapabilityItem* item = matrix.Find(name);
        if (item != nullptr) {
            BENTO_CHECK(item->reason.find("禁用") != std::string::npos);
        }
    }
    // 未被关闭的项仍应正常探测。
    ExpectStatus(matrix, "proc.meminfo", CapabilityStatus::kAvailable);
}

/// 场景 7：cgroup v2 正常视图。
void TestCgroupV2() {
    Fixture fixture;
    PopulateProc(fixture.options.proc_root);
    PopulateCgroupV2(fixture.options.cgroup_root, "268435456", "50000 100000");
    const CapabilityMatrix matrix = Probe(fixture.options);
    CheckMatrixInvariants(matrix);
    ExpectStatus(matrix, "cgroup.version", CapabilityStatus::kAvailable);
    ExpectStatus(matrix, "cgroup.memory", CapabilityStatus::kAvailable);
    ExpectStatus(matrix, "cgroup.cpu", CapabilityStatus::kAvailable);
    ExpectDetail(matrix, "cgroup.version", "v2 (");
    ExpectDetail(matrix, "cgroup.memory", "memory limit=268435456 B");
    ExpectDetail(matrix, "cgroup.cpu", "cpu quota=50000us");
}

/// 场景 8：cgroup v1 的「无限」值必须识别为 unlimited，而不是具体 EB 限额。
void TestCgroupV1() {
    Fixture fixture;
    PopulateProc(fixture.options.proc_root);
    PopulateCgroupV1(fixture.options.cgroup_root, "9223372036854771712", "-1");
    const CapabilityMatrix matrix = Probe(fixture.options);
    CheckMatrixInvariants(matrix);
    ExpectStatus(matrix, "cgroup.version", CapabilityStatus::kAvailable);
    ExpectStatus(matrix, "cgroup.memory", CapabilityStatus::kAvailable);
    ExpectStatus(matrix, "cgroup.cpu", CapabilityStatus::kAvailable);
    ExpectDetail(matrix, "cgroup.version", "v1 (");
    ExpectDetail(matrix, "cgroup.memory", "unlimited");
    ExpectDetail(matrix, "cgroup.cpu", "unlimited");
}

/// 场景 9：v1/v2 混合时 v2 标记优先；v2 限制文件缺失则两项降级。
void TestCgroupMixed() {
    Fixture fixture;
    PopulateProc(fixture.options.proc_root);
    PopulateCgroupV1(fixture.options.cgroup_root, "268435456", "25000");
    WriteFile(Join(fixture.options.cgroup_root, "cgroup.controllers"), "cpu memory\n");
    const CapabilityMatrix matrix = Probe(fixture.options);
    CheckMatrixInvariants(matrix);
    ExpectStatus(matrix, "cgroup.version", CapabilityStatus::kAvailable);
    ExpectDetail(matrix, "cgroup.version", "v2 (");
    ExpectStatus(matrix, "cgroup.memory", CapabilityStatus::kDegraded);
    ExpectStatus(matrix, "cgroup.cpu", CapabilityStatus::kDegraded);
}

/// 场景 10：容器限制——低内存、零 CPU 配额降级；max 与有限配额可用。
void TestContainerLimits() {
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        PopulateCgroupV2(fixture.options.cgroup_root, "33554432", "0 100000");
        const CapabilityMatrix matrix = Probe(fixture.options);
        CheckMatrixInvariants(matrix);
        ExpectStatus(matrix, "cgroup.memory", CapabilityStatus::kDegraded);
        ExpectStatus(matrix, "cgroup.cpu", CapabilityStatus::kDegraded);
        ExpectReason(matrix, "cgroup.memory", "低于阈值");
        ExpectReason(matrix, "cgroup.cpu", "quota=0");
    }
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        PopulateCgroupV2(fixture.options.cgroup_root, "max", "max 100000");
        const CapabilityMatrix matrix = Probe(fixture.options);
        ExpectStatus(matrix, "cgroup.memory", CapabilityStatus::kAvailable);
        ExpectStatus(matrix, "cgroup.cpu", CapabilityStatus::kAvailable);
        ExpectDetail(matrix, "cgroup.memory", "unlimited");
        ExpectDetail(matrix, "cgroup.cpu", "unlimited");
    }
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        PopulateCgroupV1(fixture.options.cgroup_root, "268435456", "25000");
        const CapabilityMatrix matrix = Probe(fixture.options);
        ExpectStatus(matrix, "cgroup.memory", CapabilityStatus::kAvailable);
        ExpectStatus(matrix, "cgroup.cpu", CapabilityStatus::kAvailable);
        ExpectDetail(matrix, "cgroup.cpu", "cpu quota=25000us");
    }
}

/// 场景 11：畸形内容与错误文件类型。
void TestMalformedInputs() {
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        const std::string meminfo = Join(fixture.options.proc_root, "meminfo");
        std::filesystem::remove(meminfo);
        MakeDir(meminfo);  // 目录冒充文件：必须不可用。
        const CapabilityMatrix matrix = Probe(fixture.options);
        CheckMatrixInvariants(matrix);
        ExpectStatus(matrix, "proc.meminfo", CapabilityStatus::kUnavailable);
    }
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        WriteFile(Join(fixture.options.proc_root, "meminfo"),
                  std::string(8u * 1024u * 1024u + 4096u, 'x'));
        const CapabilityMatrix matrix = Probe(fixture.options);
        CheckMatrixInvariants(matrix);
        ExpectStatus(matrix, "proc.meminfo", CapabilityStatus::kUnavailable);
    }
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        WriteFile(Join(fixture.options.proc_root, "meminfo"), "MemTotal: 1024 kB\n");
        WriteFile(Join(fixture.options.proc_root, "cpuinfo"), "model name: no processors\n");
        WriteFile(Join(fixture.options.proc_root, "self/status"),
                  "Name:\tcapability_fixture\nThreads:\t1\n");
        PopulateCgroupV2(fixture.options.cgroup_root, "not-a-number", "");
        const CapabilityMatrix matrix = Probe(fixture.options);
        CheckMatrixInvariants(matrix);
        ExpectStatus(matrix, "proc.meminfo", CapabilityStatus::kDegraded);
        ExpectStatus(matrix, "proc.cpuinfo", CapabilityStatus::kDegraded);
        ExpectStatus(matrix, "proc.self.status", CapabilityStatus::kDegraded);
        ExpectStatus(matrix, "cgroup.memory", CapabilityStatus::kDegraded);
        ExpectStatus(matrix, "cgroup.cpu", CapabilityStatus::kDegraded);
    }
    {
        // cpu.max 缺少 period 字段：畸形内容必须降级，不能当成可用配额。
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        PopulateCgroupV2(fixture.options.cgroup_root, "268435456", "50000");
        const CapabilityMatrix matrix = Probe(fixture.options);
        CheckMatrixInvariants(matrix);
        ExpectStatus(matrix, "cgroup.cpu", CapabilityStatus::kDegraded);
        ExpectReason(matrix, "cgroup.cpu", "period");
    }
    {
        // status 可读但只有 Uid：不是不可用（进程视图仍可用），CapEff 缺失不伪造。
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        WriteFile(Join(fixture.options.proc_root, "self/status"),
                  "Name:\tcapability_fixture\nUid:\t1000\t1000\t1000\t1000\n");
        const CapabilityMatrix matrix = Probe(fixture.options);
        ExpectStatus(matrix, "proc.self.status", CapabilityStatus::kAvailable);
        ExpectDetail(matrix, "proc.self.status", "Uid=");
    }
}

/// 场景 12：/proc 文件权限拒绝（非 root 或 root 无 DAC override 时）。
void TestPermissionDenied() {
    Fixture fixture;
    PopulateProc(fixture.options.proc_root);
    const std::string proc = fixture.options.proc_root;
    const std::string meminfo = Join(proc, "meminfo");
    const std::string cpuinfo = Join(proc, "cpuinfo");
    const std::string status = Join(proc, "self/status");
    const std::string fd_dir = Join(proc, "self/fd");
    bool chmod_ok = SetMode(meminfo, 0000);
    chmod_ok = SetMode(cpuinfo, 0000) && chmod_ok;
    chmod_ok = SetMode(status, 0000) && chmod_ok;
    chmod_ok = SetMode(fd_dir, 0000) && chmod_ok;
    BENTO_CHECK(chmod_ok);

    // 权限位是否真的生效取决于 uid/capability；先自测一次，避免把 root
    // 绕过权限误判成探测缺陷，也避免静默 PASS。
    const int probe_descriptor = ::open(meminfo.c_str(), O_RDONLY | O_CLOEXEC);
    const bool permission_enforced = probe_descriptor < 0;
    if (probe_descriptor >= 0) {
        ::close(probe_descriptor);
    }
    if (!permission_enforced) {
        SetMode(meminfo, 0644);
        SetMode(cpuinfo, 0644);
        SetMode(status, 0644);
        SetMode(fd_dir, 0755);
        std::printf("[SKIP] permission-denied: uid bypasses mode 000\n");
        return;
    }

    const CapabilityMatrix matrix = Probe(fixture.options);
    SetMode(meminfo, 0644);
    SetMode(cpuinfo, 0644);
    SetMode(status, 0644);
    SetMode(fd_dir, 0755);
    CheckMatrixInvariants(matrix);
    for (const char* name : {"proc.meminfo", "proc.cpuinfo", "proc.self.status", "proc.self.fd"}) {
        ExpectStatus(matrix, name, CapabilityStatus::kUnavailable);
    }
    ExpectStatus(matrix, "ipc.epoll", CapabilityStatus::kAvailable);
}

/// 场景 13：cgroup 限制文件权限拒绝 → 两项降级而不是不可用。
void TestCgroupPermissionDenied() {
    Fixture fixture;
    PopulateProc(fixture.options.proc_root);
    PopulateCgroupV2(fixture.options.cgroup_root, "268435456", "50000 100000");
    const std::string memory = Join(fixture.options.cgroup_root, "memory.max");
    const std::string cpu = Join(fixture.options.cgroup_root, "cpu.max");
    bool chmod_ok = SetMode(memory, 0000);
    chmod_ok = SetMode(cpu, 0000) && chmod_ok;
    BENTO_CHECK(chmod_ok);
    const int probe_descriptor = ::open(memory.c_str(), O_RDONLY | O_CLOEXEC);
    const bool permission_enforced = probe_descriptor < 0;
    if (probe_descriptor >= 0) {
        ::close(probe_descriptor);
    }
    if (!permission_enforced) {
        SetMode(memory, 0644);
        SetMode(cpu, 0644);
        std::printf("[SKIP] cgroup-permission-denied: uid bypasses mode 000\n");
        return;
    }
    const CapabilityMatrix matrix = Probe(fixture.options);
    SetMode(memory, 0644);
    SetMode(cpu, 0644);
    ExpectStatus(matrix, "cgroup.version", CapabilityStatus::kAvailable);
    ExpectStatus(matrix, "cgroup.memory", CapabilityStatus::kDegraded);
    ExpectStatus(matrix, "cgroup.cpu", CapabilityStatus::kDegraded);
}

/// 场景 14：随机环境变体（96 组确定性伪随机；大规模 1200 组在
/// audit_capability_stress.cpp）。覆盖 proc 正常/缺失/畸形 × cgroup
/// v2/v1/缺失 × 内存/CPU 取值。
void TestRandomVariants() {
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    constexpr int kVariants = 96;
    for (int index = 0; index < kVariants; ++index) {
        Fixture fixture;
        BENTO_CHECK(fixture.valid());
        if (!fixture.valid()) {
            break;
        }
        const int proc_mode = static_cast<int>(next() % 3U);    // 0 正常 1 缺失 2 畸形
        const int cgroup_mode = static_cast<int>(next() % 3U);  // 0 v2 1 v1 2 缺失
        if (proc_mode == 0) {
            PopulateProc(fixture.options.proc_root);
        } else if (proc_mode == 2) {
            WriteFile(Join(fixture.options.proc_root, "meminfo"), "garbage\n");
            WriteFile(Join(fixture.options.proc_root, "cpuinfo"), "garbage\n");
            WriteFile(Join(fixture.options.proc_root, "self/status"), "garbage\n");
            MakeDir(Join(fixture.options.proc_root, "self/fd"));
        }
        std::string expected_memory = "available";
        std::string expected_cpu = "available";
        if (cgroup_mode == 0) {
            const std::array<const char*, 3> memory_values = {"max", "268435456", "33554432"};
            const std::array<const char*, 3> cpu_values = {"max 100000", "50000 100000",
                                                           "0 100000"};
            const std::size_t memory_index = next() % 3U;
            const std::size_t cpu_index = next() % 3U;
            PopulateCgroupV2(fixture.options.cgroup_root, memory_values[memory_index],
                             cpu_values[cpu_index]);
            if (memory_index == 2) {
                expected_memory = "degraded";
            }
            if (cpu_index == 2) {
                expected_cpu = "degraded";
            }
        } else if (cgroup_mode == 1) {
            const std::array<const char*, 3> memory_values = {"9223372036854771712", "268435456",
                                                              "33554432"};
            const std::array<const char*, 3> cpu_values = {"-1", "25000", "0"};
            const std::size_t memory_index = next() % 3U;
            const std::size_t cpu_index = next() % 3U;
            PopulateCgroupV1(fixture.options.cgroup_root, memory_values[memory_index],
                             cpu_values[cpu_index]);
            if (memory_index == 2) {
                expected_memory = "degraded";
            }
            if (cpu_index == 2) {
                expected_cpu = "degraded";
            }
        }
        const CapabilityMatrix matrix = Probe(fixture.options);
        BENTO_CHECK_EQ(matrix.Size(), std::size_t{15});
        CheckMatrixInvariants(matrix);
        if (proc_mode == 0) {
            for (const char* name : {"proc.meminfo", "proc.cpuinfo", "proc.self.status",
                                     "proc.self.fd"}) {
                ExpectStatus(matrix, name, CapabilityStatus::kAvailable);
            }
        } else {
            for (const char* name : {"proc.meminfo", "proc.cpuinfo", "proc.self.status",
                                     "proc.self.fd"}) {
                const CapabilityStatus expected =
                    proc_mode == 1 ? CapabilityStatus::kUnavailable
                                   : (std::string(name) == "proc.self.fd"
                                          ? CapabilityStatus::kAvailable
                                          : CapabilityStatus::kDegraded);
                ExpectStatus(matrix, name, expected);
            }
        }
        if (cgroup_mode == 2) {
            ExpectStatus(matrix, "cgroup.version", CapabilityStatus::kDegraded);
            ExpectStatus(matrix, "cgroup.memory", CapabilityStatus::kDegraded);
            ExpectStatus(matrix, "cgroup.cpu", CapabilityStatus::kDegraded);
        } else {
            ExpectStatus(matrix, "cgroup.version", CapabilityStatus::kAvailable);
            ExpectStatus(matrix, "cgroup.memory",
                         expected_memory == "available" ? CapabilityStatus::kAvailable
                                                        : CapabilityStatus::kDegraded);
            ExpectStatus(matrix, "cgroup.cpu",
                         expected_cpu == "available" ? CapabilityStatus::kAvailable
                                                     : CapabilityStatus::kDegraded);
        }
    }
}

}  // namespace

int main() {
    TestNormalEnvironment();
    TestMatrixApi();
    TestMissingProc();
    TestRlimitThresholds();
    TestMissingCgroup();
    TestDisabledChecks();
    TestCgroupV2();
    TestCgroupV1();
    TestCgroupMixed();
    TestContainerLimits();
    TestMalformedInputs();
    TestPermissionDenied();
    TestCgroupPermissionDenied();
    TestRandomVariants();
    return bento_test::Finish("bento_capability");
}
