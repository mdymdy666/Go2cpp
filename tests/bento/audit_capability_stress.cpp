/**
 * @file audit_capability_stress.cpp
 * @brief MOD-37 能力探测的压测、随机环境变体与复杂场景审计程序。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 本程序不注册 CTest（运行时间取决于参数），用于审计交付：
 *          - `--bench`：10 万次以上 `Probe()` 的吞吐、延迟分位与 RSS 变化；
 *          - `--random`：1000+ 个随机环境变体（/proc 条目缺失/权限拒绝/
 *            cgroup v1/v2/混合/畸形内容），用独立 oracle 断言精确降级状态；
 *          - `--scenarios`：正常、rlimit 受限、/proc 不可读、cgroup v1/v2、
 *            容器限制、畸形输入 6 个场景。
 *          默认全跑；`--quick` 缩小规模。
 *
 * 用法：
 *   g++ -std=c++17 -O2 -Iinclude tests/bento/audit_capability_stress.cpp \
 *       src/bento/app/capability.cpp -o /tmp/audit_capability_stress
 *   /tmp/audit_capability_stress --all
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "capability_fixture.hpp"
#include "go2cpp/bento/app/capability.hpp"

namespace {

using go2cpp::bento::app::CapabilityItem;
using go2cpp::bento::app::CapabilityMatrix;
using go2cpp::bento::app::CapabilityStatus;
using go2cpp::bento::app::CapabilityStatusName;
using go2cpp::bento::app::Probe;
using go2cpp::bento::app::ProbeOptions;

using capability_fixture::Join;
using capability_fixture::kMinMemBytes;
using capability_fixture::MakeDir;
using capability_fixture::PopulateCgroupV1;
using capability_fixture::PopulateCgroupV2;
using capability_fixture::PopulateProc;
using capability_fixture::PopulateStatus;
using capability_fixture::SetMode;
using capability_fixture::TempTree;
using capability_fixture::WriteFile;

constexpr std::uint64_t kUnlimitedThreshold = std::uint64_t{1} << 62;
constexpr long kTmpfsMagic = 0x01021994L;

// ASan 的 shadow memory 需要极大数据段虚拟地址空间，RLIMIT_AS 会直接让
// 进程无法 mmap；此时仍保留 RLIMIT_NOFILE 分支，仅跳过 AS 限制。
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAddressSanitizerBuild = true;
#else
constexpr bool kAddressSanitizerBuild = false;
#endif

int g_checks = 0;
int g_failures = 0;
int g_skips = 0;

std::string ToText(const std::string& value) { return value; }
std::string ToText(bool value) { return value ? "true" : "false"; }

template <typename T>
std::string ToText(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

void Fail(const std::string& tag, const std::string& message) {
    ++g_failures;
    if (g_failures <= 40) {
        std::fprintf(stderr, "[FAIL] %s: %s\n", tag.c_str(), message.c_str());
    }
}

#define CHECK_TRUE(condition, tag)                       \
    do {                                                 \
        ++g_checks;                                      \
        if (!(condition)) {                              \
            Fail((tag), #condition);                     \
        }                                                \
    } while (0)

#define CHECK_EQ(actual, expected, tag)                                             \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!((actual) == (expected))) {                                            \
            Fail((tag), std::string(#actual " == " #expected " (actual=") +         \
                            ToText(actual) + ", expected=" + ToText(expected) + ")"); \
        }                                                                           \
    } while (0)

// ---------------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------------

struct Expectation {
    std::string name;
    CapabilityStatus status = CapabilityStatus::kAvailable;
};

struct Fragment {
    std::string name;
    std::string needle;
};

std::string_view TrimView(std::string_view text) {
    const auto not_space = [](unsigned char character) { return std::isspace(character) == 0; };
    while (!text.empty() && !not_space(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && !not_space(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

bool ParseU64(std::string_view text, std::uint64_t* value) {
    std::size_t index = 0;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
        ++index;
    }
    if (index == text.size() || text[index] < '0' || text[index] > '9') {
        return false;
    }
    std::uint64_t result = 0;
    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (character < '0' || character > '9') {
            break;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (result > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    *value = result;
    return true;
}

bool FindLineValue(std::string_view text, std::string_view key, std::string_view* value) {
    std::size_t position = 0;
    while (position <= text.size()) {
        const std::size_t end = text.find('\n', position);
        const std::size_t length =
            end == std::string_view::npos ? text.size() - position : end - position;
        const std::string_view line = text.substr(position, length);
        if (line.size() >= key.size() && line.compare(0, key.size(), key) == 0) {
            *value = TrimView(line.substr(key.size()));
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        position = end + 1;
    }
    return false;
}

/// oracle 自己的文件读取（与实现独立，最多读 9 MiB）。
bool ReadWhole(const std::string& path, std::string* out) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    struct stat info {};
    if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(descriptor);
        return false;
    }
    std::string text;
    char buffer[4096];
    bool ok = true;
    for (;;) {
        const ssize_t count = ::read(descriptor, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        if (count == 0) {
            break;
        }
        text.append(buffer, static_cast<std::size_t>(count));
        if (text.size() > 9u * 1024u * 1024u) {
            ok = false;
            break;
        }
    }
    ::close(descriptor);
    if (!ok) {
        return false;
    }
    *out = std::move(text);
    return true;
}

bool IsRegular(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool IsDirectory(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::uint64_t ReadRssKb() {
    std::string text;
    if (!ReadWhole("/proc/self/status", &text)) {
        return 0;
    }
    std::string_view value;
    if (!FindLineValue(text, "VmRSS:", &value)) {
        return 0;
    }
    std::uint64_t kilobytes = 0;
    return ParseU64(value, &kilobytes) ? kilobytes : 0;
}

std::uint64_t ReadMaxRssKb() {
    struct rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(usage.ru_maxrss);
}

class Fixture {
public:
    Fixture() {
        if (!tree.valid()) {
            return;
        }
        options.proc_root = Join(tree.root(), "proc");
        options.cgroup_root = Join(tree.root(), "cgroup");
        options.tmp_path = Join(tree.root(), "tmpdir");
        options.root_path = tree.root();
        options.min_mem_available = kMinMemBytes;
        MakeDir(options.tmp_path);
        MakeDir(options.proc_root);
        MakeDir(options.cgroup_root);
    }

    TempTree tree;
    ProbeOptions options;
};

// ---------------------------------------------------------------------------
// 独立 oracle：根据夹具实际内容推导每一项的期望状态
// ---------------------------------------------------------------------------

void DeriveExpectations(const ProbeOptions& options, std::vector<Expectation>* expected,
                        std::vector<Fragment>* fragments) {
    const auto add = [expected](const char* name, CapabilityStatus status) {
        expected->push_back(Expectation{name, status});
    };
    const auto add_fragment = [fragments](const char* name, std::string needle) {
        fragments->push_back(Fragment{name, std::move(needle)});
    };

    // proc.meminfo
    {
        const std::string path = Join(options.proc_root, "meminfo");
        std::string text;
        CapabilityStatus status = CapabilityStatus::kUnavailable;
        if (ReadWhole(path, &text)) {
            std::string_view total_text;
            std::string_view available_text;
            std::uint64_t total_kb = 0;
            std::uint64_t available_kb = 0;
            if (!FindLineValue(text, "MemTotal:", &total_text) || !ParseU64(total_text, &total_kb) ||
                !FindLineValue(text, "MemAvailable:", &available_text) ||
                !ParseU64(available_text, &available_kb)) {
                status = CapabilityStatus::kDegraded;
            } else {
                const std::uint64_t available_bytes =
                    available_kb > (UINT64_MAX / 1024U) ? UINT64_MAX : available_kb * 1024U;
                status = available_bytes < options.min_mem_available
                             ? CapabilityStatus::kDegraded
                             : CapabilityStatus::kAvailable;
                if (status == CapabilityStatus::kAvailable) {
                    add_fragment("proc.meminfo", "MemAvailable=");
                }
            }
        }
        add("proc.meminfo", status);
    }

    // proc.cpuinfo
    {
        const std::string path = Join(options.proc_root, "cpuinfo");
        std::string text;
        CapabilityStatus status = CapabilityStatus::kUnavailable;
        if (ReadWhole(path, &text)) {
            std::size_t processors = 0;
            std::size_t position = 0;
            while (position < text.size()) {
                const std::size_t end = text.find('\n', position);
                const std::size_t length =
                    end == std::string::npos ? text.size() - position : end - position;
                const std::string_view line(text.data() + position, length);
                if (line.size() >= 9 && line.compare(0, 9, "processor") == 0) {
                    ++processors;
                }
                if (end == std::string::npos) {
                    break;
                }
                position = end + 1;
            }
            if (processors == 0) {
                status = CapabilityStatus::kDegraded;
            } else {
                status = CapabilityStatus::kAvailable;
                add_fragment("proc.cpuinfo", "processors=" + std::to_string(processors));
            }
        }
        add("proc.cpuinfo", status);
    }

    // proc.self.status
    {
        const std::string path = Join(options.proc_root, "self/status");
        std::string text;
        CapabilityStatus status = CapabilityStatus::kUnavailable;
        if (ReadWhole(path, &text)) {
            std::string_view uid_text;
            std::string_view cap_text;
            const bool has_uid = FindLineValue(text, "Uid:", &uid_text);
            const bool has_cap = FindLineValue(text, "CapEff:", &cap_text);
            status = (has_uid || has_cap) ? CapabilityStatus::kAvailable
                                          : CapabilityStatus::kDegraded;
        }
        add("proc.self.status", status);
    }

    // proc.self.fd
    {
        const std::string path = Join(options.proc_root, "self/fd");
        DIR* directory = ::opendir(path.c_str());
        CapabilityStatus status = CapabilityStatus::kUnavailable;
        if (directory != nullptr) {
            bool readable = true;
            errno = 0;
            while (::readdir(directory) != nullptr) {
            }
            if (errno != 0) {
                readable = false;
            }
            ::closedir(directory);
            status = readable ? CapabilityStatus::kAvailable : CapabilityStatus::kUnavailable;
        }
        add("proc.self.fd", status);
    }

    // cgroup.version / memory / cpu
    {
        CapabilityStatus version_status = CapabilityStatus::kDegraded;
        CapabilityStatus memory_status = CapabilityStatus::kDegraded;
        CapabilityStatus cpu_status = CapabilityStatus::kDegraded;
        if (options.check_cgroup) {
            const std::string controllers = Join(options.cgroup_root, "cgroup.controllers");
            const std::string memory_v1 = Join(options.cgroup_root, "memory");
            std::string version_detail;
            int version = 0;
            if (IsRegular(controllers)) {
                version = 2;
                version_detail = "v2 (";
            } else if (IsDirectory(memory_v1)) {
                version = 1;
                version_detail = "v1 (";
            }
            if (version != 0) {
                version_status = CapabilityStatus::kAvailable;
                add_fragment("cgroup.version", version_detail);
            }

            // memory
            {
                const std::string path =
                    version == 2 ? Join(options.cgroup_root, "memory.max")
                                 : Join(options.cgroup_root, "memory/memory.limit_in_bytes");
                if (version != 0) {
                    std::string text;
                    if (ReadWhole(path, &text)) {
                        text = std::string(TrimView(text));
                        if (text == "max") {
                            memory_status = CapabilityStatus::kAvailable;
                            add_fragment("cgroup.memory", "unlimited");
                        } else {
                            std::uint64_t bytes = 0;
                            if (ParseU64(text, &bytes)) {
                                if (bytes >= kUnlimitedThreshold) {
                                    memory_status = CapabilityStatus::kAvailable;
                                    add_fragment("cgroup.memory", "unlimited");
                                } else {
                                    memory_status =
                                        bytes < options.min_mem_available
                                            ? CapabilityStatus::kDegraded
                                            : CapabilityStatus::kAvailable;
                                }
                            }
                        }
                    }
                }
            }

            // cpu
            {
                if (version == 2) {
                    std::string text;
                    if (ReadWhole(Join(options.cgroup_root, "cpu.max"), &text)) {
                        std::istringstream stream(std::string(TrimView(text)));
                        std::string quota;
                        std::string period;
                        stream >> quota >> period;
                        if (!quota.empty() && period.empty()) {
                            cpu_status = CapabilityStatus::kDegraded;
                        } else if (!quota.empty()) {
                            if (quota == "max") {
                                cpu_status = CapabilityStatus::kAvailable;
                                add_fragment("cgroup.cpu", "unlimited");
                            } else {
                                std::uint64_t value = 0;
                                if (ParseU64(quota, &value)) {
                                    cpu_status = value == 0 ? CapabilityStatus::kDegraded
                                                            : CapabilityStatus::kAvailable;
                                }
                            }
                        }
                    }
                } else if (version == 1) {
                    std::string text;
                    if (ReadWhole(Join(options.cgroup_root, "cpu/cpu.cfs_quota_us"), &text)) {
                        text = std::string(TrimView(text));
                        if (text == "-1") {
                            cpu_status = CapabilityStatus::kAvailable;
                            add_fragment("cgroup.cpu", "unlimited");
                        } else {
                            std::uint64_t value = 0;
                            if (ParseU64(text, &value)) {
                                cpu_status = value == 0 ? CapabilityStatus::kDegraded
                                                        : CapabilityStatus::kAvailable;
                            }
                        }
                    }
                }
            }
        }
        add("cgroup.version", version_status);
        add("cgroup.memory", memory_status);
        add("cgroup.cpu", cpu_status);
    }

    // ipc.epoll
    add("ipc.epoll", options.check_epoll ? CapabilityStatus::kAvailable
                                         : CapabilityStatus::kDegraded);

    // net.interfaces：只断言开关语义，是否有非回环接口取决于宿主。
    if (!options.check_interfaces) {
        add("net.interfaces", CapabilityStatus::kDegraded);
    }

    // fs.tmpfs
    {
        struct statfs info {};
        CapabilityStatus status = CapabilityStatus::kUnavailable;
        if (::statfs(options.tmp_path.c_str(), &info) == 0) {
            status = info.f_type == kTmpfsMagic ? CapabilityStatus::kAvailable
                                                : CapabilityStatus::kDegraded;
        }
        add("fs.tmpfs", status);
    }

    // fs.root_readonly
    {
        struct statvfs info {};
        CapabilityStatus status = CapabilityStatus::kUnavailable;
        if (::statvfs(options.root_path.c_str(), &info) == 0) {
            status = (info.f_flag & ST_RDONLY) != 0 ? CapabilityStatus::kDegraded
                                                   : CapabilityStatus::kAvailable;
        }
        add("fs.root_readonly", status);
    }

    // rlimit.nofile / nproc
    {
        CapabilityStatus status = CapabilityStatus::kDegraded;
        if (options.check_rlimit) {
            struct rlimit limit {};
            if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
                status = CapabilityStatus::kUnavailable;
            } else if (limit.rlim_cur == RLIM_INFINITY) {
                status = CapabilityStatus::kAvailable;
            } else {
                const std::uint64_t soft = static_cast<std::uint64_t>(limit.rlim_cur);
                status = soft < options.min_nofile ? CapabilityStatus::kDegraded
                                                   : CapabilityStatus::kAvailable;
            }
        }
        add("rlimit.nofile", status);
    }
    {
        CapabilityStatus status = CapabilityStatus::kDegraded;
        if (options.check_rlimit) {
            struct rlimit limit {};
            if (::getrlimit(RLIMIT_NPROC, &limit) != 0) {
                status = CapabilityStatus::kUnavailable;
            } else if (limit.rlim_cur == RLIM_INFINITY) {
                status = CapabilityStatus::kAvailable;
            } else {
                const std::uint64_t soft = static_cast<std::uint64_t>(limit.rlim_cur);
                status = (options.min_nproc != 0 && soft < options.min_nproc)
                             ? CapabilityStatus::kDegraded
                             : CapabilityStatus::kAvailable;
            }
        }
        add("rlimit.nproc", status);
    }

    add("unix.privilege", CapabilityStatus::kAvailable);
    add("kernel.info", CapabilityStatus::kAvailable);
}

// ---------------------------------------------------------------------------
// 不变量与断言
// ---------------------------------------------------------------------------

const std::array<const char*, 15>& ExpectedNames() {
    static const std::array<const char*, 15> kNames = {
        "proc.meminfo",      "proc.cpuinfo",    "proc.self.status", "proc.self.fd",
        "unix.privilege",    "ipc.epoll",       "cgroup.version",   "cgroup.memory",
        "cgroup.cpu",        "net.interfaces",  "fs.tmpfs",         "fs.root_readonly",
        "rlimit.nofile",     "rlimit.nproc",    "kernel.info"};
    return kNames;
}

void CheckStatus(const CapabilityMatrix& matrix, const std::string& name,
                 CapabilityStatus expected, const std::string& tag) {
    ++g_checks;
    const CapabilityItem* item = matrix.Find(name);
    if (item == nullptr) {
        Fail(tag, "missing item " + name);
        return;
    }
    if (item->status != expected) {
        Fail(tag, name + " status=" + CapabilityStatusName(item->status) + " expected=" +
                       CapabilityStatusName(expected) + " reason=" + item->reason);
        return;
    }
    if (item->available() && !item->reason.empty()) {
        Fail(tag, name + " is available but reason is non-empty: " + item->reason);
    }
    if (!item->available() && item->reason.empty()) {
        Fail(tag, name + " is not available but reason is empty");
    }
}

void CheckFragment(const CapabilityMatrix& matrix, const Fragment& fragment, const std::string& tag) {
    ++g_checks;
    const CapabilityItem* item = matrix.Find(fragment.name);
    if (item == nullptr) {
        Fail(tag, "missing item " + fragment.name);
        return;
    }
    if (item->detail.find(fragment.needle) == std::string::npos) {
        Fail(tag, fragment.name + " detail missing \"" + fragment.needle + "\": " + item->detail);
    }
}

void CheckInvariants(const CapabilityMatrix& matrix, const std::string& tag) {
    CHECK_EQ(matrix.Size(), std::size_t{15}, tag);
    CHECK_TRUE(!matrix.Empty(), tag);
    const auto& names = ExpectedNames();
    std::size_t available = 0;
    std::size_t degraded = 0;
    std::size_t unavailable = 0;
    std::size_t observed = 0;
    for (std::size_t index = 0; index < matrix.items.size(); ++index) {
        const CapabilityItem& item = matrix.items[index];
        ++observed;
        CHECK_TRUE(!item.name.empty(), tag + " item#" + std::to_string(index));
        if (index < names.size()) {
            CHECK_EQ(item.name, std::string(names[index]), tag + " name order");
            CHECK_TRUE(matrix.Find(item.name) == &item, tag + " Find(" + item.name + ")");
        }
        switch (item.status) {
            case CapabilityStatus::kAvailable:
                ++available;
                CHECK_TRUE(item.reason.empty(), tag + " " + item.name + " available reason empty");
                CHECK_TRUE(!item.detail.empty(), tag + " " + item.name + " available detail");
                break;
            case CapabilityStatus::kDegraded:
                ++degraded;
                CHECK_TRUE(!item.reason.empty(), tag + " " + item.name + " degraded reason");
                break;
            case CapabilityStatus::kUnavailable:
                ++unavailable;
                CHECK_TRUE(!item.reason.empty(), tag + " " + item.name + " unavailable reason");
                break;
        }
    }
    CHECK_EQ(observed, matrix.Size(), tag + " iteration count");
    CHECK_EQ(available, matrix.Count(CapabilityStatus::kAvailable), tag + " Count(available)");
    CHECK_EQ(degraded, matrix.Count(CapabilityStatus::kDegraded), tag + " Count(degraded)");
    CHECK_EQ(unavailable, matrix.Count(CapabilityStatus::kUnavailable), tag + " Count(unavailable)");
    CHECK_EQ(available + degraded + unavailable, matrix.Size(), tag + " status partition");
    CHECK_EQ(matrix.IsDegraded(), available != matrix.Size(), tag + " IsDegraded");
    CHECK_EQ(matrix.Reasons().size(), matrix.Size() - available, tag + " Reasons size");
    const std::string expected_summary = "capabilities: total=15 available=" +
                                         std::to_string(available) + " degraded=" +
                                         std::to_string(degraded) + " unavailable=" +
                                         std::to_string(unavailable);
    CHECK_EQ(matrix.Summary(), expected_summary, tag + " Summary");
    CHECK_TRUE(matrix.Find("does.not.exist") == nullptr, tag + " Find(missing)");
}

void CheckMatrix(const CapabilityMatrix& matrix, const std::vector<Expectation>& expected,
                 const std::vector<Fragment>& fragments, const std::string& tag) {
    CheckInvariants(matrix, tag);
    for (const Expectation& expectation : expected) {
        CheckStatus(matrix, expectation.name, expectation.status, tag);
    }
    for (const Fragment& fragment : fragments) {
        CheckFragment(matrix, fragment, tag);
    }
}

// ---------------------------------------------------------------------------
// 压测
// ---------------------------------------------------------------------------

void RunBench(const char* label, const ProbeOptions& options, int iterations) {
    if (iterations <= 0) {
        return;
    }
    std::uint64_t sink = 0;
    for (int index = 0; index < 100; ++index) {
        const CapabilityMatrix matrix = Probe(options);
        sink += matrix.Size();
    }
    const std::uint64_t rss_before = ReadRssKb();
    const std::uint64_t max_rss_before = ReadMaxRssKb();
    std::vector<std::uint64_t> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < iterations; ++index) {
        const auto begin = std::chrono::steady_clock::now();
        const CapabilityMatrix matrix = Probe(options);
        const auto end = std::chrono::steady_clock::now();
        sink += matrix.Size() + matrix.Count(CapabilityStatus::kAvailable);
        samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
    }
    const auto finish = std::chrono::steady_clock::now();
    const double total_ms =
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count() / 1000.0;
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double ratio) {
        const double index = ratio * static_cast<double>(samples.size() - 1);
        return static_cast<double>(samples[static_cast<std::size_t>(index + 0.5)]) / 1000.0;
    };
    const double mean_us = (total_ms * 1000.0) / static_cast<double>(iterations);
    const std::uint64_t rss_after = ReadRssKb();
    const std::uint64_t max_rss_after = ReadMaxRssKb();
    std::printf(
        "[bench] %s iters=%d total_ms=%.3f ops_per_sec=%.0f mean_us=%.3f p50_us=%.3f "
        "p95_us=%.3f p99_us=%.3f rss_before_kb=%llu rss_after_kb=%llu rss_delta_kb=%lld "
        "maxrss_kb=%llu sink=%llu\n",
        label, iterations, total_ms, static_cast<double>(iterations) / (total_ms / 1000.0), mean_us,
        percentile(0.50), percentile(0.95), percentile(0.99),
        static_cast<unsigned long long>(rss_before), static_cast<unsigned long long>(rss_after),
        static_cast<long long>(rss_after) - static_cast<long long>(rss_before),
        static_cast<unsigned long long>(max_rss_after), static_cast<unsigned long long>(sink));
    (void)max_rss_before;
}

// ---------------------------------------------------------------------------
// 分解计时：定位瓶颈（Probe 各开关 + /proc 原语）
// ---------------------------------------------------------------------------

template <typename Function>
void TimePerOp(const char* label, int iterations, Function&& function) {
    for (int index = 0; index < 100; ++index) {
        function();
    }
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < iterations; ++index) {
        function();
    }
    const auto finish = std::chrono::steady_clock::now();
    const double nanoseconds = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
    std::printf("[profile] %-30s iters=%d ns_per_op=%.1f\n", label, iterations,
                nanoseconds / static_cast<double>(iterations));
}

void RunProfile() {
    constexpr int kProbeIterations = 5000;
    TimePerOp("probe-default", kProbeIterations, [] { (void)Probe(); });
    {
        ProbeOptions options;
        options.check_interfaces = false;
        TimePerOp("probe-no-interfaces", kProbeIterations, [&] { (void)Probe(options); });
    }
    {
        ProbeOptions options;
        options.check_epoll = false;
        TimePerOp("probe-no-epoll", kProbeIterations, [&] { (void)Probe(options); });
    }
    {
        ProbeOptions options;
        options.check_cgroup = false;
        TimePerOp("probe-no-cgroup", kProbeIterations, [&] { (void)Probe(options); });
    }
    {
        ProbeOptions options;
        options.check_rlimit = false;
        TimePerOp("probe-no-rlimit", kProbeIterations, [&] { (void)Probe(options); });
    }
    {
        ProbeOptions options;
        options.check_epoll = false;
        options.check_interfaces = false;
        options.check_cgroup = false;
        options.check_rlimit = false;
        TimePerOp("probe-checks-off", kProbeIterations, [&] { (void)Probe(options); });
    }
    {
        // 把真实 /proc 文本复制到 ext4 夹具，隔离「/proc 生成成本」与解析成本。
        Fixture fixture;
        for (const char* name : {"meminfo", "cpuinfo", "self/status"}) {
            std::string text;
            if (ReadWhole(std::string("/proc/") + name, &text)) {
                WriteFile(Join(fixture.options.proc_root, name), text);
            }
        }
        MakeDir(Join(fixture.options.proc_root, "self/fd"));
        TimePerOp("probe-copied-proc", kProbeIterations,
                  [&] { (void)Probe(fixture.options); });
        fixture.options.check_epoll = false;
        fixture.options.check_interfaces = false;
        fixture.options.check_cgroup = false;
        fixture.options.check_rlimit = false;
        TimePerOp("probe-copied-proc-off", kProbeIterations,
                  [&] { (void)Probe(fixture.options); });
    }

    TimePerOp("getifaddrs+free", 20000, [] {
        struct ifaddrs* addresses = nullptr;
        if (::getifaddrs(&addresses) == 0) {
            ::freeifaddrs(addresses);
        }
    });
    TimePerOp("epoll_create1+close", 100000, [] {
        const int descriptor = ::epoll_create1(EPOLL_CLOEXEC);
        if (descriptor >= 0) {
            ::close(descriptor);
        }
    });
    TimePerOp("statfs(/tmp)", 100000, [] {
        struct statfs info {};
        (void)::statfs("/tmp", &info);
    });
    TimePerOp("statvfs(/)", 100000, [] {
        struct statvfs info {};
        (void)::statvfs("/", &info);
    });
    TimePerOp("uname", 100000, [] {
        struct utsname info {};
        (void)::uname(&info);
    });
    TimePerOp("getrlimit(nofile)", 100000, [] {
        struct rlimit limit {};
        (void)::getrlimit(RLIMIT_NOFILE, &limit);
    });
    TimePerOp("stat cgroup.controllers", 100000, [] {
        struct stat info {};
        (void)::stat("/sys/fs/cgroup/cgroup.controllers", &info);
    });
    TimePerOp("read /proc/meminfo", 20000, [] {
        std::string text;
        (void)ReadWhole("/proc/meminfo", &text);
    });
    TimePerOp("read /proc/cpuinfo", 20000, [] {
        std::string text;
        (void)ReadWhole("/proc/cpuinfo", &text);
    });
    TimePerOp("read /proc/self/status", 20000, [] {
        std::string text;
        (void)ReadWhole("/proc/self/status", &text);
    });
    TimePerOp("opendir+readdir fd", 20000, [] {
        DIR* directory = ::opendir("/proc/self/fd");
        if (directory != nullptr) {
            while (::readdir(directory) != nullptr) {
            }
            ::closedir(directory);
        }
    });
    static const std::string kCpuinfoText = [] {
        std::string text;
        (void)ReadWhole("/proc/cpuinfo", &text);
        return text;
    }();
    TimePerOp("parse /proc/cpuinfo text", 20000, [] {
        std::size_t processors = 0;
        std::size_t position = 0;
        while (position < kCpuinfoText.size()) {
            const std::size_t end = kCpuinfoText.find('\n', position);
            const std::size_t length =
                end == std::string::npos ? kCpuinfoText.size() - position : end - position;
            if (length >= 9 && kCpuinfoText.compare(position, 9, "processor") == 0) {
                ++processors;
            }
            if (end == std::string::npos) {
                break;
            }
            position = end + 1;
        }
    });
}

// ---------------------------------------------------------------------------
// 场景 1：正常环境
// ---------------------------------------------------------------------------

void ScenarioNormal() {
    const CapabilityMatrix matrix = Probe();
    const std::string tag = "scenario:normal";
    CheckInvariants(matrix, tag);
    const std::size_t available = matrix.Count(CapabilityStatus::kAvailable);
    const std::size_t degraded = matrix.Count(CapabilityStatus::kDegraded);
    const std::size_t unavailable = matrix.Count(CapabilityStatus::kUnavailable);
    std::printf("[scenario] normal: %s\n", matrix.Summary().c_str());
    std::printf("[scenario] normal: available=%zu degraded=%zu unavailable=%zu\n", available,
                degraded, unavailable);
    CHECK_TRUE(available >= 8, tag + " at least 8 available");
    for (const CapabilityItem& item : matrix.items) {
        if (item.available()) {
            CHECK_TRUE(!item.detail.empty(), tag + " " + item.name + " detail");
        }
    }
    CHECK_TRUE(matrix.Find("does.not.exist") == nullptr, tag + " unknown item");
}

// ---------------------------------------------------------------------------
// 场景 2：受限环境（RLIMIT_AS / RLIMIT_NOFILE）
// ---------------------------------------------------------------------------

void ScenarioRestricted() {
    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child < 0) {
        Fail("scenario:restricted", "fork failed");
        return;
    }
    if (child == 0) {
        struct rlimit nofile {};
        if (::getrlimit(RLIMIT_NOFILE, &nofile) != 0) {
            _exit(2);
        }
        struct rlimit lowered = nofile;
        lowered.rlim_cur = 64;
        if (lowered.rlim_max != RLIM_INFINITY && lowered.rlim_cur > lowered.rlim_max) {
            lowered.rlim_cur = lowered.rlim_max;
        }
        if (::setrlimit(RLIMIT_NOFILE, &lowered) != 0) {
            _exit(3);
        }
        struct rlimit address {};
        if (!kAddressSanitizerBuild && ::getrlimit(RLIMIT_AS, &address) == 0) {
            struct rlimit address_limit = address;
            address_limit.rlim_cur = 128ull * 1024ull * 1024ull;
            if (address_limit.rlim_max != RLIM_INFINITY &&
                address_limit.rlim_cur > address_limit.rlim_max) {
                address_limit.rlim_cur = address_limit.rlim_max;
            }
            (void)::setrlimit(RLIMIT_AS, &address_limit);
        }
        ProbeOptions options;
        options.min_nofile = 128;
        const CapabilityMatrix matrix = Probe(options);
        const std::string tag = "scenario:restricted(child)";
        CheckInvariants(matrix, tag);
        CheckStatus(matrix, "rlimit.nofile", CapabilityStatus::kDegraded, tag);
        const CapabilityItem* item = matrix.Find("rlimit.nofile");
        if (item != nullptr) {
            CHECK_TRUE(item->reason.find("min_nofile=128") != std::string::npos,
                       tag + " nofile reason mentions threshold");
            CHECK_TRUE(item->reason.find("soft=64") != std::string::npos,
                       tag + " nofile reason mentions soft=64");
        }
        // 128 MiB 地址空间内继续探测 2000 次，验证不会因分配失败崩溃。
        for (int index = 0; index < 2000; ++index) {
            const CapabilityMatrix repeated = Probe();
            if (repeated.Size() != 15) {
                _exit(5);
            }
        }
        _exit(g_failures == 0 ? 0 : 1);
    }
    int status = 0;
    if (::waitpid(child, &status, 0) != child) {
        Fail("scenario:restricted", "waitpid failed");
        return;
    }
    const bool exited = WIFEXITED(status);
    const int code = exited ? WEXITSTATUS(status) : -1;
    CHECK_TRUE(exited && code == 0, "scenario:restricted child exit code");
    std::printf("[scenario] restricted: child exit=%d (RLIMIT_NOFILE soft=64, RLIMIT_AS=%s, "
                "2000 probes)\n",
                code, kAddressSanitizerBuild ? "skipped(ASan)" : "128MiB");
}

// ---------------------------------------------------------------------------
// 场景 3：/proc 不可读（权限拒绝）
// ---------------------------------------------------------------------------

void ScenarioUnreadableProc() {
    if (::geteuid() == 0) {
        ++g_skips;
        std::printf("[scenario] unreadable-proc: SKIP (uid 0 bypasses chmod 000)\n");
        return;
    }
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
    CHECK_TRUE(chmod_ok, "scenario:unreadable-proc chmod");
    const CapabilityMatrix matrix = Probe(fixture.options);
    // 先恢复权限，保证后续断言失败时临时目录也能被清理。
    SetMode(meminfo, 0644);
    SetMode(cpuinfo, 0644);
    SetMode(status, 0644);
    SetMode(fd_dir, 0755);
    const std::string tag = "scenario:unreadable-proc";
    CheckInvariants(matrix, tag);
    for (const char* name : {"proc.meminfo", "proc.cpuinfo", "proc.self.status", "proc.self.fd"}) {
        CheckStatus(matrix, name, CapabilityStatus::kUnavailable, tag);
        const CapabilityItem* item = matrix.Find(name);
        if (item != nullptr) {
            CHECK_TRUE(item->reason.find("proc") != std::string::npos,
                       tag + " " + name + " reason mentions path");
        }
    }
    CheckStatus(matrix, "ipc.epoll", CapabilityStatus::kAvailable, tag);
    CheckStatus(matrix, "fs.root_readonly", CapabilityStatus::kAvailable, tag);
    std::printf("[scenario] unreadable-proc: 4 proc items unavailable, epoll/fs still available\n");
}

// ---------------------------------------------------------------------------
// 场景 4：cgroup v1 / v2 / 混合格式
// ---------------------------------------------------------------------------

void ScenarioCgroupFormats() {
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        PopulateCgroupV2(fixture.options.cgroup_root, "268435456", "50000 100000");
        const CapabilityMatrix matrix = Probe(fixture.options);
        const std::string tag = "scenario:cgroup-v2";
        CheckInvariants(matrix, tag);
        CheckStatus(matrix, "cgroup.version", CapabilityStatus::kAvailable, tag);
        CheckStatus(matrix, "cgroup.memory", CapabilityStatus::kAvailable, tag);
        CheckStatus(matrix, "cgroup.cpu", CapabilityStatus::kAvailable, tag);
        CheckFragment(matrix, {"cgroup.version", "v2 ("}, tag);
        CheckFragment(matrix, {"cgroup.memory", "memory limit=268435456 B"}, tag);
        CheckFragment(matrix, {"cgroup.cpu", "cpu quota=50000us"}, tag);
        std::printf("[scenario] cgroup-v2: version/memory/cpu available\n");
    }
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        PopulateCgroupV1(fixture.options.cgroup_root, "9223372036854771712", "-1");
        const CapabilityMatrix matrix = Probe(fixture.options);
        const std::string tag = "scenario:cgroup-v1";
        CheckInvariants(matrix, tag);
        CheckStatus(matrix, "cgroup.version", CapabilityStatus::kAvailable, tag);
        CheckStatus(matrix, "cgroup.memory", CapabilityStatus::kAvailable, tag);
        CheckStatus(matrix, "cgroup.cpu", CapabilityStatus::kAvailable, tag);
        CheckFragment(matrix, {"cgroup.version", "v1 ("}, tag);
        CheckFragment(matrix, {"cgroup.memory", "unlimited"}, tag);
        CheckFragment(matrix, {"cgroup.cpu", "unlimited"}, tag);
        std::printf("[scenario] cgroup-v1: unlimited limit/quota recognized\n");
    }
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        PopulateCgroupV1(fixture.options.cgroup_root, "268435456", "25000");
        WriteFile(Join(fixture.options.cgroup_root, "cgroup.controllers"), "cpu memory\n");
        const CapabilityMatrix matrix = Probe(fixture.options);
        const std::string tag = "scenario:cgroup-mixed";
        CheckInvariants(matrix, tag);
        CheckStatus(matrix, "cgroup.version", CapabilityStatus::kAvailable, tag);
        CheckFragment(matrix, {"cgroup.version", "v2 ("}, tag);
        // v2 优先：memory.max/cpu.max 不存在，两项降级。
        CheckStatus(matrix, "cgroup.memory", CapabilityStatus::kDegraded, tag);
        CheckStatus(matrix, "cgroup.cpu", CapabilityStatus::kDegraded, tag);
        std::printf("[scenario] cgroup-mixed: v2 marker wins, v2 limit files missing -> degraded\n");
    }
}

// ---------------------------------------------------------------------------
// 场景 5：容器限制（memory.max / cpu.max / v1 quota）
// ---------------------------------------------------------------------------

void ScenarioContainerLimits() {
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        PopulateCgroupV2(fixture.options.cgroup_root, "33554432", "0 100000");
        const CapabilityMatrix matrix = Probe(fixture.options);
        const std::string tag = "scenario:container-limit";
        CheckInvariants(matrix, tag);
        CheckStatus(matrix, "cgroup.memory", CapabilityStatus::kDegraded, tag);
        CheckStatus(matrix, "cgroup.cpu", CapabilityStatus::kDegraded, tag);
        const CapabilityItem* memory = matrix.Find("cgroup.memory");
        if (memory != nullptr) {
            CHECK_TRUE(memory->reason.find("低于阈值") != std::string::npos,
                       tag + " memory reason mentions threshold");
        }
        const CapabilityItem* cpu = matrix.Find("cgroup.cpu");
        if (cpu != nullptr) {
            CHECK_TRUE(cpu->reason.find("quota=0") != std::string::npos ||
                           cpu->reason.find("配额为 0") != std::string::npos,
                       tag + " cpu reason mentions zero quota");
        }
    }
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        PopulateCgroupV2(fixture.options.cgroup_root, "max", "50000 100000");
        const CapabilityMatrix matrix = Probe(fixture.options);
        const std::string tag = "scenario:container-unlimited";
        CheckInvariants(matrix, tag);
        CheckStatus(matrix, "cgroup.memory", CapabilityStatus::kAvailable, tag);
        CheckStatus(matrix, "cgroup.cpu", CapabilityStatus::kAvailable, tag);
        CheckFragment(matrix, {"cgroup.memory", "unlimited"}, tag);
        CheckFragment(matrix, {"cgroup.cpu", "cpu quota=50000us"}, tag);
    }
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        PopulateCgroupV1(fixture.options.cgroup_root, "268435456", "25000");
        const CapabilityMatrix matrix = Probe(fixture.options);
        const std::string tag = "scenario:container-v1";
        CheckInvariants(matrix, tag);
        CheckStatus(matrix, "cgroup.memory", CapabilityStatus::kAvailable, tag);
        CheckStatus(matrix, "cgroup.cpu", CapabilityStatus::kAvailable, tag);
        CheckFragment(matrix, {"cgroup.cpu", "cpu quota=25000us"}, tag);
        std::printf("[scenario] container: v2 low-mem/zero-cpu degraded, max/v1 quota available\n");
    }
}

// ---------------------------------------------------------------------------
// 场景 6：畸形输入与文件类型错误
// ---------------------------------------------------------------------------

void ScenarioMalformed() {
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        const std::string meminfo = Join(fixture.options.proc_root, "meminfo");
        std::filesystem::remove(meminfo);
        MakeDir(meminfo);  // 目录冒充普通文件：必须判不可用，不能读成空内容。
        const CapabilityMatrix matrix = Probe(fixture.options);
        const std::string tag = "scenario:malformed-dir";
        CheckInvariants(matrix, tag);
        CheckStatus(matrix, "proc.meminfo", CapabilityStatus::kUnavailable, tag);
        std::printf("[scenario] malformed-dir: meminfo directory -> unavailable\n");
    }
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        const std::string huge(8u * 1024u * 1024u + 4096u, 'x');
        WriteFile(Join(fixture.options.proc_root, "meminfo"), huge);
        const CapabilityMatrix matrix = Probe(fixture.options);
        const std::string tag = "scenario:malformed-huge";
        CheckInvariants(matrix, tag);
        CheckStatus(matrix, "proc.meminfo", CapabilityStatus::kUnavailable, tag);
        std::printf("[scenario] malformed-huge: 8MiB+ meminfo -> bounded read unavailable\n");
    }
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        WriteFile(Join(fixture.options.proc_root, "meminfo"), "MemTotal: 1024 kB\n");
        WriteFile(Join(fixture.options.proc_root, "cpuinfo"), "model name: no processor lines\n");
        WriteFile(Join(fixture.options.proc_root, "self/status"),
                  "Name:\tcapability_fixture\nThreads:\t1\n");
        PopulateCgroupV2(fixture.options.cgroup_root, "not-a-number", "");
        const CapabilityMatrix matrix = Probe(fixture.options);
        const std::string tag = "scenario:malformed-content";
        CheckInvariants(matrix, tag);
        CheckStatus(matrix, "proc.meminfo", CapabilityStatus::kDegraded, tag);
        CheckStatus(matrix, "proc.cpuinfo", CapabilityStatus::kDegraded, tag);
        CheckStatus(matrix, "proc.self.status", CapabilityStatus::kDegraded, tag);
        CheckStatus(matrix, "cgroup.memory", CapabilityStatus::kDegraded, tag);
        CheckStatus(matrix, "cgroup.cpu", CapabilityStatus::kDegraded, tag);
        std::printf("[scenario] malformed-content: all unparsable fields degrade\n");
    }
    {
        Fixture fixture;
        PopulateProc(fixture.options.proc_root);
        PopulateCgroupV2(fixture.options.cgroup_root, "268435456", "50000 100000");
        fixture.options.check_cgroup = false;
        const CapabilityMatrix matrix = Probe(fixture.options);
        const std::string tag = "scenario:cgroup-disabled";
        CheckInvariants(matrix, tag);
        for (const char* name : {"cgroup.version", "cgroup.memory", "cgroup.cpu"}) {
            CheckStatus(matrix, name, CapabilityStatus::kDegraded, tag);
            const CapabilityItem* item = matrix.Find(name);
            if (item != nullptr) {
                CHECK_TRUE(item->reason.find("禁用") != std::string::npos,
                           tag + " " + name + " reason mentions disabled");
            }
        }
        std::printf("[scenario] cgroup-disabled: all three items degrade with disabled reason\n");
    }
}

// ---------------------------------------------------------------------------
// 场景 7：多线程并发探测（头文件声明 Probe 并发安全）
// ---------------------------------------------------------------------------

void ScenarioConcurrent() {
    const CapabilityMatrix reference = Probe();
    std::atomic<int> size_failures{0};
    std::atomic<int> status_mismatches{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&] {
            for (int index = 0; index < 500; ++index) {
                const CapabilityMatrix matrix = Probe();
                if (matrix.Size() != reference.Size()) {
                    ++size_failures;
                    continue;
                }
                for (std::size_t item = 0; item < matrix.items.size(); ++item) {
                    if (matrix.items[item].name != reference.items[item].name ||
                        matrix.items[item].status != reference.items[item].status) {
                        ++status_mismatches;
                    }
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    ++g_checks;
    if (size_failures != 0 || status_mismatches != 0) {
        Fail("scenario:concurrent", "size_failures=" + std::to_string(size_failures.load()) +
                                       " status_mismatches=" +
                                       std::to_string(status_mismatches.load()));
    }
    std::printf("[scenario] concurrent: 8 threads x 500 probes, size_failures=%d mismatches=%d\n",
                size_failures.load(), status_mismatches.load());
}

// ---------------------------------------------------------------------------
// 随机环境变体
// ---------------------------------------------------------------------------

class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}
    std::uint64_t Next() {
        std::uint64_t value = state_;
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
        state_ = value;
        return value;
    }
    std::uint32_t Below(std::uint32_t bound) {
        return bound == 0 ? 0 : static_cast<std::uint32_t>(Next() % bound);
    }
    bool Chance(std::uint32_t numerator, std::uint32_t denominator) {
        return Below(denominator) < numerator;
    }

private:
    std::uint64_t state_;
};

std::string PickValue(Rng* rng, const std::vector<std::string>& values) {
    return values[rng->Below(static_cast<std::uint32_t>(values.size()))];
}

std::string MakeMeminfo(std::uint64_t total_kb, std::uint64_t available_kb) {
    return "MemTotal:        " + std::to_string(total_kb) +
           " kB\nMemFree:          1024 kB\nMemAvailable:    " + std::to_string(available_kb) +
           " kB\n";
}

void BuildRandomProc(Rng* rng, const std::string& proc_root, bool can_deny) {
    switch (rng->Below(6)) {
        case 0:
            PopulateProc(proc_root, static_cast<int>(1 + rng->Below(4)));
            break;
        case 1: {
            PopulateProc(proc_root, 2);
            const std::vector<std::uint64_t> available_values = {0, 1024, 65536, 4194304};
            WriteFile(Join(proc_root, "meminfo"),
                      MakeMeminfo(8192000, available_values[rng->Below(4)]));
            break;
        }
        case 2: {
            PopulateProc(proc_root, 2);
            WriteFile(Join(proc_root, "meminfo"), "MemTotal:        8192000 kB\n");
            break;
        }
        case 3:
            // 缺 meminfo / fd，其余正常。
            PopulateProc(proc_root, 2);
            std::filesystem::remove(Join(proc_root, "meminfo"));
            std::filesystem::remove_all(Join(proc_root, "self/fd"));
            break;
        case 4:
            WriteFile(Join(proc_root, "meminfo"), "this is not meminfo\n");
            WriteFile(Join(proc_root, "cpuinfo"), "this is not cpuinfo\n");
            WriteFile(Join(proc_root, "self/status"), "this is not status\n");
            MakeDir(Join(proc_root, "self/fd"));
            break;
        default:
            PopulateProc(proc_root, 2);
            PopulateStatus(proc_root, false);
            break;
    }
    if (can_deny && rng->Chance(1, 6)) {
        const std::vector<std::string> victims = {Join(proc_root, "meminfo"),
                                                  Join(proc_root, "cpuinfo"),
                                                  Join(proc_root, "self/status")};
        const std::string& victim = victims[rng->Below(static_cast<std::uint32_t>(victims.size()))];
        if (IsRegular(victim)) {
            SetMode(victim, 0000);
        }
    }
}

void BuildRandomCgroup(Rng* rng, const std::string& cgroup_root, bool can_deny) {
    const std::vector<std::string> memory_values = {"max", "0", "33554432", "268435456",
                                                    "garbage", ""};
    const std::vector<std::string> cpu_values = {"max 100000", "50000 100000", "0 100000",
                                                 "garbage", "", "-1 100000"};
    const std::vector<std::string> v1_memory_values = {"9223372036854771712", "33554432",
                                                       "268435456", "garbage", ""};
    const std::vector<std::string> v1_cpu_values = {"-1", "0", "25000", "garbage", ""};
    const std::vector<std::string> memory_victims = {
        Join(cgroup_root, "memory.max"), Join(cgroup_root, "memory/memory.limit_in_bytes")};
    const std::vector<std::string> cpu_victims = {Join(cgroup_root, "cpu.max"),
                                                  Join(cgroup_root, "cpu/cpu.cfs_quota_us")};

    switch (rng->Below(10)) {
        case 0:
        case 1:
            return;  // 没有 cgroup 挂载
        case 2:
        case 3:
        case 4:
            PopulateCgroupV2(cgroup_root, PickValue(rng, memory_values), PickValue(rng, cpu_values));
            if (rng->Chance(1, 5)) {
                std::filesystem::remove(memory_victims[0]);
            }
            if (rng->Chance(1, 5)) {
                std::filesystem::remove(cpu_victims[0]);
            }
            break;
        case 5:
        case 6:
        case 7:
            PopulateCgroupV1(cgroup_root, PickValue(rng, v1_memory_values),
                             PickValue(rng, v1_cpu_values));
            if (rng->Chance(1, 5)) {
                std::filesystem::remove(memory_victims[1]);
            }
            if (rng->Chance(1, 5)) {
                std::filesystem::remove(cpu_victims[1]);
            }
            break;
        default:
            // 混合：v1 目录 + v2 标记（版本按 v2 识别，v2 限制文件缺失）。
            PopulateCgroupV1(cgroup_root, PickValue(rng, v1_memory_values),
                             PickValue(rng, v1_cpu_values));
            WriteFile(Join(cgroup_root, "cgroup.controllers"), "cpu memory\n");
            break;
    }
    if (can_deny && rng->Chance(1, 8)) {
        const std::string& victim =
            rng->Chance(1, 2) ? memory_victims[rng->Below(2)] : cpu_victims[rng->Below(2)];
        if (IsRegular(victim)) {
            SetMode(victim, 0000);
        }
    }
}

void RandomVariants(int count) {
    Rng rng(0x9e3779b97f4a7c15ULL);
    const bool can_deny = ::geteuid() != 0;
    for (int index = 0; index < count; ++index) {
        Fixture fixture;
        if (!fixture.tree.valid()) {
            Fail("random", "mkdtemp failed");
            break;
        }
        const std::string tag = "random#" + std::to_string(index);
        BuildRandomProc(&rng, fixture.options.proc_root, can_deny);
        BuildRandomCgroup(&rng, fixture.options.cgroup_root, can_deny);
        fixture.options.check_epoll = !rng.Chance(1, 7);
        fixture.options.check_interfaces = !rng.Chance(1, 7);
        fixture.options.check_cgroup = !rng.Chance(1, 5);
        fixture.options.check_rlimit = !rng.Chance(1, 5);
        fixture.options.min_mem_available =
            (std::uint64_t{1} << (20 + static_cast<unsigned>(rng.Below(13))));
        fixture.options.min_nofile = rng.Below(4) == 0 ? 0 : std::uint64_t{1} << (rng.Below(22));
        fixture.options.min_nproc = rng.Below(3) == 0 ? 0 : std::uint64_t{1} << (rng.Below(18));
        const CapabilityMatrix matrix = Probe(fixture.options);
        std::vector<Expectation> expected;
        std::vector<Fragment> fragments;
        DeriveExpectations(fixture.options, &expected, &fragments);
        CheckMatrix(matrix, expected, fragments, tag);
    }
    std::printf("[random] variants=%d checks=%d failures=%d\n", count, g_checks, g_failures);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

void PrintUsage(const char* program) {
    std::printf("usage: %s [--all|--bench-only|--random-only|--scenario-only|--profile-only] "
                "[--iters N] [--variants N] [--quick]\n",
                program);
}

}  // namespace

int main(int argc, char** argv) {
    int iterations = 100000;
    int variants = 1200;
    bool run_bench = true;
    bool run_random = true;
    bool run_scenarios = true;
    bool run_profile = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--all") {
            run_bench = true;
            run_random = true;
            run_scenarios = true;
        } else if (argument == "--bench-only") {
            run_bench = true;
            run_random = false;
            run_scenarios = false;
        } else if (argument == "--random-only") {
            run_bench = false;
            run_random = true;
            run_scenarios = false;
        } else if (argument == "--scenario-only") {
            run_bench = false;
            run_random = false;
            run_scenarios = true;
        } else if (argument == "--profile-only") {
            run_bench = false;
            run_random = false;
            run_scenarios = false;
            run_profile = true;
        } else if (argument == "--quick") {
            iterations = 10000;
            variants = 120;
        } else if (argument == "--iters" && index + 1 < argc) {
            iterations = std::atoi(argv[++index]);
        } else if (argument == "--variants" && index + 1 < argc) {
            variants = std::atoi(argv[++index]);
        } else if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            PrintUsage(argv[0]);
            return 2;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    if (run_scenarios) {
        ScenarioNormal();
        ScenarioRestricted();
        ScenarioUnreadableProc();
        ScenarioCgroupFormats();
        ScenarioContainerLimits();
        ScenarioMalformed();
        ScenarioConcurrent();
    }
    if (run_random) {
        RandomVariants(variants);
    }
    if (run_profile) {
        RunProfile();
    }
    if (run_bench) {
        RunBench("real-proc", ProbeOptions{}, iterations);
        {
            Fixture fixture;
            PopulateProc(fixture.options.proc_root);
            PopulateCgroupV2(fixture.options.cgroup_root, "268435456", "50000 100000");
            RunBench("fixture", fixture.options, std::max(iterations / 5, 10000));
        }
        {
            ProbeOptions options;
            options.check_epoll = false;
            options.check_interfaces = false;
            options.check_cgroup = false;
            options.check_rlimit = false;
            RunBench("checks-off", options, std::max(iterations / 5, 10000));
        }
    }
    const double elapsed_ms =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count() /
        1000.0;
    std::printf("[summary] checks=%d failures=%d skips=%d elapsed_ms=%.1f\n", g_checks,
                g_failures, g_skips, elapsed_ms);
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] audit_capability_stress: %d/%d checks failed\n", g_failures,
                     g_checks);
        return 1;
    }
    std::printf("[PASS] audit_capability_stress (%d checks)\n", g_checks);
    return 0;
}
