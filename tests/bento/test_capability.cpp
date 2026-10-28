/**
 * @file test_capability.cpp
 * @brief MOD-37 环境能力探测测试：正常环境、手工矩阵 API、缺失 /proc、
 *        低 rlimit、缺失 cgroup、关闭探测开关。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "go2cpp/bento/app/capability.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::app::CapabilityItem;
using go2cpp::bento::app::CapabilityMatrix;
using go2cpp::bento::app::CapabilityStatus;
using go2cpp::bento::app::CapabilityStatusName;
using go2cpp::bento::app::Probe;
using go2cpp::bento::app::ProbeOptions;

/// 场景 1：正常 Linux 环境应探测到全部 15 项，其中至少 8 项可用。
void TestNormalEnvironment() {
    const CapabilityMatrix matrix = Probe();
    BENTO_CHECK(matrix.Size() >= 8);
    BENTO_CHECK_EQ(matrix.Size(), std::size_t{15});
    BENTO_CHECK(!matrix.Empty());
    const std::size_t available = matrix.Count(CapabilityStatus::kAvailable);
    const std::size_t degraded = matrix.Count(CapabilityStatus::kDegraded);
    const std::size_t unavailable = matrix.Count(CapabilityStatus::kUnavailable);
    BENTO_CHECK(available >= 8);

    // Count 必须与逐项遍历的实际状态一致，而不是恒真的三态求和。
    std::size_t counted_available = 0;
    std::size_t counted_degraded = 0;
    std::size_t counted_unavailable = 0;
    for (const CapabilityItem& item : matrix.items) {
        switch (item.status) {
            case CapabilityStatus::kAvailable:
                ++counted_available;
                break;
            case CapabilityStatus::kDegraded:
                ++counted_degraded;
                break;
            case CapabilityStatus::kUnavailable:
                ++counted_unavailable;
                break;
        }
    }
    BENTO_CHECK_EQ(available, counted_available);
    BENTO_CHECK_EQ(degraded, counted_degraded);
    BENTO_CHECK_EQ(unavailable, counted_unavailable);

    // 每一项都必须有名字；非可用项必须有可操作的原因。
    for (const CapabilityItem& item : matrix.items) {
        BENTO_CHECK(!item.name.empty());
        BENTO_CHECK(std::string(CapabilityStatusName(item.status)) != "unknown");
        if (!item.available()) {
            BENTO_CHECK(!item.reason.empty());
        }
    }

    const CapabilityItem* meminfo = matrix.Find("proc.meminfo");
    BENTO_CHECK(meminfo != nullptr);
    if (meminfo != nullptr) {
        BENTO_CHECK(meminfo->available());
        BENTO_CHECK(!meminfo->detail.empty());
    }
    BENTO_CHECK(matrix.Find("proc.cpuinfo") != nullptr);
    BENTO_CHECK(matrix.Find("ipc.epoll") != nullptr);
    BENTO_CHECK(matrix.Find("net.interfaces") != nullptr);
    BENTO_CHECK(matrix.Find("fs.root_readonly") != nullptr);
    BENTO_CHECK(matrix.Find("rlimit.nofile") != nullptr);
    BENTO_CHECK(matrix.Find("kernel.info") != nullptr);
    BENTO_CHECK(matrix.Find("does.not.exist") == nullptr);

    const std::string summary = matrix.Summary();
    const std::string expected_summary =
        "capabilities: total=15 available=" + std::to_string(available) +
        " degraded=" + std::to_string(degraded) +
        " unavailable=" + std::to_string(unavailable);
    BENTO_CHECK_EQ(summary, expected_summary);

    // 全可用时 Reasons 为空；否则每条都带项名。
    const std::vector<std::string> reasons = matrix.Reasons();
    if (!matrix.IsDegraded()) {
        BENTO_CHECK(reasons.empty());
    } else {
        BENTO_CHECK(!reasons.empty());
        for (const std::string& reason : reasons) {
            BENTO_CHECK(reason.find(':') != std::string::npos);
        }
    }
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
        const CapabilityItem* item = matrix.Find(name);
        BENTO_CHECK(item != nullptr);
        if (item != nullptr) {
            BENTO_CHECK(item->unavailable());
            BENTO_CHECK(!item->reason.empty());
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
    const CapabilityItem* epoll = matrix.Find("ipc.epoll");
    BENTO_CHECK(epoll != nullptr);
    if (epoll != nullptr) {
        BENTO_CHECK(epoll->available());
    }
}

/// 场景 4：把 RLIMIT_NOFILE 软上限调低，探测必须报受限并说明阈值。
void TestLowNofile() {
    struct rlimit original {};
    if (::getrlimit(RLIMIT_NOFILE, &original) != 0) {
        BENTO_CHECK(false);
        return;
    }
    // 原软上限必须足够高，才能安全地降到 64；否则跳过本场景（不制造假失败）。
    if (original.rlim_cur != RLIM_INFINITY && original.rlim_cur <= 64) {
        bento_test::Skip("rlimit.nofile already <= 64");
        return;
    }
    struct rlimit lowered {};
    lowered.rlim_cur = 64;
    lowered.rlim_max = original.rlim_max;
    if (::setrlimit(RLIMIT_NOFILE, &lowered) != 0) {
        bento_test::Skip("setrlimit(RLIMIT_NOFILE) denied");
        return;
    }

    ProbeOptions options;
    options.min_nofile = 128;
    const CapabilityMatrix matrix = Probe(options);

    // 恢复原限制，避免影响同一进程内的后续测试。
    const int restore_result = ::setrlimit(RLIMIT_NOFILE, &original);
    BENTO_CHECK_EQ(restore_result, 0);

    const CapabilityItem* item = matrix.Find("rlimit.nofile");
    BENTO_CHECK(item != nullptr);
    if (item != nullptr) {
        BENTO_CHECK(item->degraded());
        BENTO_CHECK(item->reason.find("min_nofile") != std::string::npos);
        BENTO_CHECK(item->reason.find("soft=64") != std::string::npos);
    }
    BENTO_CHECK(matrix.IsDegraded());
}

/// 场景 5：cgroup 根目录缺失时，版本/内存/CPU 三项都必须降级并说明原因。
void TestMissingCgroup() {
    ProbeOptions options;
    options.cgroup_root = "/nonexistent-go2cpp-cgroup";
    const CapabilityMatrix matrix = Probe(options);
    for (const char* name : {"cgroup.version", "cgroup.memory", "cgroup.cpu"}) {
        const CapabilityItem* item = matrix.Find(name);
        BENTO_CHECK(item != nullptr);
        if (item != nullptr) {
            BENTO_CHECK(item->degraded());
            BENTO_CHECK(!item->reason.empty());
        }
    }
    const CapabilityItem* version = matrix.Find("cgroup.version");
    if (version != nullptr) {
        BENTO_CHECK(version->reason.find("nonexistent-go2cpp-cgroup") != std::string::npos);
    }
}

/// 场景 6：显式关闭各探测开关时，对应项应给出「禁用」原因而不是假可用。
void TestDisabledChecks() {
    ProbeOptions options;
    options.check_epoll = false;
    options.check_interfaces = false;
    options.check_cgroup = false;
    options.check_rlimit = false;
    const CapabilityMatrix matrix = Probe(options);

    for (const char* name : {"ipc.epoll", "net.interfaces", "cgroup.version", "rlimit.nofile",
                             "rlimit.nproc"}) {
        const CapabilityItem* item = matrix.Find(name);
        BENTO_CHECK(item != nullptr);
        if (item != nullptr) {
            BENTO_CHECK(item->degraded());
            BENTO_CHECK(item->reason.find("禁用") != std::string::npos);
        }
    }
    // 未被关闭的项仍应正常探测。
    const CapabilityItem* meminfo = matrix.Find("proc.meminfo");
    BENTO_CHECK(meminfo != nullptr);
    if (meminfo != nullptr) {
        BENTO_CHECK(meminfo->available());
    }
}

}  // namespace

int main() {
    TestNormalEnvironment();
    TestMatrixApi();
    TestMissingProc();
    TestLowNofile();
    TestMissingCgroup();
    TestDisabledChecks();
    return bento_test::Finish("bento_capability");
}
