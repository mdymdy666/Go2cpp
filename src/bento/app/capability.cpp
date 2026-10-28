/**
 * @file capability.cpp
 * @brief Linux 运行环境能力探测实现（MOD-37）。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/app/capability.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace go2cpp::bento::app {
namespace {

/// tmpfs 的 `statfs.f_type`（Linux 上稳定；不 include <linux/magic.h>，
/// 避免把内核私有头带进公共构建）。
constexpr long kTmpfsMagic = 0x01021994L;

/// RLIM_INFINITY 的兼容判断：不同 C 库对 rlim_t 的宽度定义不同，
/// 统一按宏比较，避免把无限值截断成 64 位后误判。
bool IsInfinite(rlim_t value) noexcept {
    return value == RLIM_INFINITY;
}

/// 把 errno 转成可读文本（线程安全，不依赖 strerror 的静态缓冲）。
std::string ErrorText(int error) {
    return std::error_code(error, std::generic_category()).message();
}

/// 十六进制小写输出（CapEff 用）。
std::string ToHex(std::uint64_t value) {
    static const char kDigits[] = "0123456789abcdef";
    if (value == 0) {
        return "0";
    }
    std::string out;
    out.reserve(16);
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const unsigned digit = static_cast<unsigned>((value >> shift) & 0x0fU);
        if (!started && digit == 0) {
            continue;
        }
        started = true;
        out.push_back(kDigits[digit]);
    }
    return out;
}

/// 读取整个文本文件；不存在、无权限、是目录都返回 false。
bool ReadFile(const std::string& path, std::string* out) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        return false;
    }
    *out = buffer.str();
    return true;
}

std::string Trim(std::string text) {
    const auto not_space = [](unsigned char character) { return std::isspace(character) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

/// 在 `Key: value` 形式的多行文本里查找一行并返回值部分。
bool LookupLineValue(const std::string& text, const std::string& key, std::string* value) {
    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t end = text.find('\n', position);
        const std::size_t length = (end == std::string::npos) ? std::string::npos : end - position;
        const std::string line = text.substr(position, length);
        if (line.compare(0, key.size(), key) == 0) {
            *value = Trim(line.substr(key.size()));
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    return false;
}

/// 解析前导十进制整数；允许尾随单位（meminfo 的 "12345 kB"），
/// 但必须至少有一位数字。cgroup 的纯数值同样适用。
bool ParseU64(const std::string& text, std::uint64_t* value) {
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

void SetAvailable(CapabilityItem* item, std::string detail) {
    item->status = CapabilityStatus::kAvailable;
    item->reason.clear();
    item->detail = std::move(detail);
}

void SetDegraded(CapabilityItem* item, std::string reason, std::string detail = {}) {
    item->status = CapabilityStatus::kDegraded;
    item->reason = std::move(reason);
    item->detail = std::move(detail);
}

void SetUnavailable(CapabilityItem* item, std::string reason) {
    item->status = CapabilityStatus::kUnavailable;
    item->reason = std::move(reason);
    item->detail.clear();
}

/// /proc/meminfo：同时验证可读性与可用内存下限。
void ProbeMeminfo(const ProbeOptions& options, CapabilityItem* item) {
    const std::string path = options.proc_root + "/meminfo";
    std::string text;
    if (!ReadFile(path, &text)) {
        SetUnavailable(item, "无法读取 " + path);
        return;
    }
    std::string total_text;
    std::string available_text;
    std::uint64_t total_kb = 0;
    std::uint64_t available_kb = 0;
    if (!LookupLineValue(text, "MemTotal:", &total_text) ||
        !ParseU64(total_text, &total_kb) ||
        !LookupLineValue(text, "MemAvailable:", &available_text) ||
        !ParseU64(available_text, &available_kb)) {
        SetDegraded(item, "meminfo 缺少 MemTotal/MemAvailable 字段");
        return;
    }
    const std::uint64_t available_bytes = available_kb * 1024ULL;
    std::string detail = "MemTotal=" + std::to_string(total_kb) + " kB, MemAvailable=" +
                         std::to_string(available_kb) + " kB";
    if (available_bytes < options.min_mem_available) {
        SetDegraded(item, "可用内存 " + std::to_string(available_bytes) + " B 低于阈值 " +
                              std::to_string(options.min_mem_available) + " B",
                    std::move(detail));
        return;
    }
    SetAvailable(item, std::move(detail));
}

/// /proc/cpuinfo：统计 processor 条目；0 个说明 /proc 视图不完整。
void ProbeCpuinfo(const ProbeOptions& options, CapabilityItem* item) {
    const std::string path = options.proc_root + "/cpuinfo";
    std::string text;
    if (!ReadFile(path, &text)) {
        SetUnavailable(item, "无法读取 " + path);
        return;
    }
    std::size_t processors = 0;
    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t end = text.find('\n', position);
        const std::size_t length = (end == std::string::npos) ? std::string::npos : end - position;
        const std::string line = text.substr(position, length);
        if (line.compare(0, 9, "processor") == 0) {
            ++processors;
        }
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    if (processors == 0) {
        SetDegraded(item, "cpuinfo 中没有 processor 条目");
        return;
    }
    SetAvailable(item, "processors=" + std::to_string(processors));
}

/// /proc/self/status：验证进程视图可读，并提取 CapEff。
void ProbeSelfStatus(const ProbeOptions& options, CapabilityItem* item,
                     std::uint64_t* capability_effective) {
    const std::string path = options.proc_root + "/self/status";
    std::string text;
    if (!ReadFile(path, &text)) {
        SetUnavailable(item, "无法读取 " + path);
        return;
    }
    std::string uid_text;
    std::string cap_text;
    std::string detail;
    if (LookupLineValue(text, "Uid:", &uid_text)) {
        detail = "Uid=" + uid_text;
    }
    if (LookupLineValue(text, "CapEff:", &cap_text)) {
        if (!detail.empty()) {
            detail += " ";
        }
        detail += "CapEff=0x" + cap_text;
        // CapEff 是 16 位十六进制；按 64 位累积解析，超出部分忽略。
        std::uint64_t value = 0;
        bool valid = !cap_text.empty();
        for (char character : cap_text) {
            std::uint64_t digit = 0;
            if (character >= '0' && character <= '9') {
                digit = static_cast<std::uint64_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                digit = static_cast<std::uint64_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                digit = static_cast<std::uint64_t>(character - 'A' + 10);
            } else {
                valid = false;
                break;
            }
            value = (value << 4U) | digit;
        }
        if (valid) {
            *capability_effective = value;
        }
    }
    SetAvailable(item, std::move(detail));
}

/// /proc/self/fd：readdir 可枚举（容器隐藏 fd 目录时失败）。
void ProbeSelfFd(const ProbeOptions& options, CapabilityItem* item) {
    const std::string path = options.proc_root + "/self/fd";
    DIR* directory = ::opendir(path.c_str());
    if (directory == nullptr) {
        SetUnavailable(item, "无法打开 " + path + "：" + ErrorText(errno));
        return;
    }
    std::size_t entries = 0;
    while (::readdir(directory) != nullptr) {
        ++entries;
    }
    ::closedir(directory);
    // 固定存在 . 与 ..，其余为实际 fd；条目数只作为明细。
    SetAvailable(item, "entries=" + std::to_string(entries));
}

/// 身份与有效 capability：非 root 是正常状态，只有系统调用失败才不可用。
void ProbePrivilege(std::uint64_t capability_effective, bool capability_known,
                    CapabilityItem* item) {
    const uid_t uid = ::getuid();
    const uid_t euid = ::geteuid();
    const gid_t gid = ::getgid();
    std::string detail = "uid=" + std::to_string(uid) + ", euid=" + std::to_string(euid) +
                         ", gid=" + std::to_string(gid);
    if (capability_known) {
        detail += ", cap_eff=0x" + ToHex(capability_effective);
        if (uid != 0 && capability_effective == 0) {
            detail += " (unprivileged)";
        }
    }
    SetAvailable(item, std::move(detail));
}

/// epoll：实际创建一次并立即关闭，确认内核/seccomp 允许。
void ProbeEpoll(const ProbeOptions& options, CapabilityItem* item) {
    if (!options.check_epoll) {
        SetDegraded(item, "探测被配置禁用（check_epoll=false）");
        return;
    }
    const int descriptor = ::epoll_create1(EPOLL_CLOEXEC);
    if (descriptor < 0) {
        SetUnavailable(item, "epoll_create1 失败：" + ErrorText(errno));
        return;
    }
    ::close(descriptor);
    SetAvailable(item, "epoll_create1 可用");
}

/// cgroup 版本：返回 0=未知，1=v1，2=v2。
int DetectCgroupVersion(const std::string& root, std::string* detail) {
    struct stat info {};
    const std::string v2_marker = root + "/cgroup.controllers";
    if (::stat(v2_marker.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
        *detail = "v2 (" + v2_marker + ")";
        return 2;
    }
    const std::string v1_marker = root + "/memory";
    if (::stat(v1_marker.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
        *detail = "v1 (" + v1_marker + ")";
        return 1;
    }
    return 0;
}

void ProbeCgroupVersion(const ProbeOptions& options, CapabilityItem* item) {
    if (!options.check_cgroup) {
        SetDegraded(item, "探测被配置禁用（check_cgroup=false）");
        return;
    }
    std::string detail;
    const int version = DetectCgroupVersion(options.cgroup_root, &detail);
    if (version == 0) {
        SetDegraded(item, "无法识别 cgroup 版本（" + options.cgroup_root + " 不可用）");
        return;
    }
    SetAvailable(item, std::move(detail));
}

/// cgroup 内存限制：v2 的 memory.max 或 v1 的 memory.limit_in_bytes。
void ProbeCgroupMemory(const ProbeOptions& options, int version, CapabilityItem* item) {
    if (!options.check_cgroup || version == 0) {
        SetDegraded(item, "cgroup 不可用，无法读取内存限制");
        return;
    }
    std::string path;
    if (version == 2) {
        path = options.cgroup_root + "/memory.max";
    } else {
        path = options.cgroup_root + "/memory/memory.limit_in_bytes";
    }
    std::string text;
    if (!ReadFile(path, &text)) {
        SetDegraded(item, "无法读取 " + path);
        return;
    }
    text = Trim(text);
    if (text == "max") {
        SetAvailable(item, "memory limit=unlimited（" + path + "）");
        return;
    }
    std::uint64_t limit_bytes = 0;
    if (!ParseU64(text, &limit_bytes)) {
        SetDegraded(item, "无法解析内存限制值 \"" + text + "\"（" + path + "）");
        return;
    }
    // v1 用接近 ULONG_MAX 的值表示无限；统一按「极大值」展示。
    const bool unlimited = limit_bytes >= (UINT64_MAX / 2U);
    const std::string detail = "memory limit=" + (unlimited ? std::string("unlimited")
                                                             : std::to_string(limit_bytes) + " B") +
                               "（" + path + "）";
    if (!unlimited && limit_bytes < options.min_mem_available) {
        SetDegraded(item, "内存限制 " + std::to_string(limit_bytes) + " B 低于阈值 " +
                              std::to_string(options.min_mem_available) + " B",
                    detail);
        return;
    }
    SetAvailable(item, detail);
}

/// cgroup CPU 配额：v2 的 cpu.max 或 v1 的 cpu.cfs_quota_us。
void ProbeCgroupCpu(const ProbeOptions& options, int version, CapabilityItem* item) {
    if (!options.check_cgroup || version == 0) {
        SetDegraded(item, "cgroup 不可用，无法读取 CPU 配额");
        return;
    }
    if (version == 2) {
        const std::string path = options.cgroup_root + "/cpu.max";
        std::string text;
        if (!ReadFile(path, &text)) {
            SetDegraded(item, "无法读取 " + path);
            return;
        }
        std::istringstream stream(Trim(text));
        std::string quota;
        std::string period;
        stream >> quota >> period;
        if (quota.empty()) {
            SetDegraded(item, "cpu.max 内容为空");
            return;
        }
        if (quota == "max") {
            SetAvailable(item, "cpu quota=unlimited, period=" + period);
            return;
        }
        std::uint64_t quota_value = 0;
        if (!ParseU64(quota, &quota_value)) {
            SetDegraded(item, "无法解析 cpu.max 的 quota \"" + quota + "\"");
            return;
        }
        if (quota_value == 0) {
            SetDegraded(item, "cpu.max quota=0，进程无法获得 CPU 时间");
            return;
        }
        SetAvailable(item, "cpu quota=" + std::to_string(quota_value) + "us, period=" + period);
        return;
    }
    const std::string path = options.cgroup_root + "/cpu/cpu.cfs_quota_us";
    std::string text;
    if (!ReadFile(path, &text)) {
        SetDegraded(item, "无法读取 " + path);
        return;
    }
    text = Trim(text);
    if (text == "-1") {
        SetAvailable(item, "cpu quota=unlimited（" + path + "）");
        return;
    }
    std::uint64_t quota_value = 0;
    if (!ParseU64(text, &quota_value)) {
        SetDegraded(item, "无法解析 CPU 配额 \"" + text + "\"（" + path + "）");
        return;
    }
    if (quota_value == 0) {
        SetDegraded(item, "CPU 配额为 0，进程无法获得 CPU 时间");
        return;
    }
    SetAvailable(item, "cpu quota=" + std::to_string(quota_value) + "us");
}

/// 网络接口：统计 UP 且非回环的接口；只有 lo 时无法对外通信。
void ProbeInterfaces(const ProbeOptions& options, CapabilityItem* item) {
    if (!options.check_interfaces) {
        SetDegraded(item, "探测被配置禁用（check_interfaces=false）");
        return;
    }
    struct ifaddrs* addresses = nullptr;
    if (::getifaddrs(&addresses) != 0) {
        SetUnavailable(item, "getifaddrs 失败：" + ErrorText(errno));
        return;
    }
    std::set<std::string> names;
    std::size_t ipv4 = 0;
    std::size_t ipv6 = 0;
    for (const struct ifaddrs* entry = addresses; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_name == nullptr || entry->ifa_addr == nullptr) {
            continue;
        }
        if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        if (entry->ifa_addr->sa_family == AF_INET) {
            ++ipv4;
        } else if (entry->ifa_addr->sa_family == AF_INET6) {
            ++ipv6;
        } else {
            continue;
        }
        names.insert(entry->ifa_name);
    }
    ::freeifaddrs(addresses);
    if (names.empty()) {
        SetDegraded(item, "没有 UP 状态的非回环网络接口（只有 lo）");
        return;
    }
    std::string detail;
    for (const auto& name : names) {
        if (!detail.empty()) {
            detail += ",";
        }
        detail += name;
    }
    detail += " (ipv4=" + std::to_string(ipv4) + ", ipv6=" + std::to_string(ipv6) + ")";
    SetAvailable(item, std::move(detail));
}

/// 临时目录是否位于 tmpfs（原子写/匿名文件的常见前提）。
void ProbeTmpfs(const ProbeOptions& options, CapabilityItem* item) {
    struct statfs info {};
    if (::statfs(options.tmp_path.c_str(), &info) != 0) {
        SetUnavailable(item, "statfs(" + options.tmp_path + ") 失败：" + ErrorText(errno));
        return;
    }
    if (info.f_type != kTmpfsMagic) {
        SetDegraded(item, options.tmp_path + " 不是 tmpfs（f_type=0x" +
                              ToHex(static_cast<std::uint64_t>(info.f_type)) + "）");
        return;
    }
    SetAvailable(item, options.tmp_path + " 是 tmpfs");
}

/// 根文件系统是否只读（只读时无法落盘日志/证书/临时文件）。
void ProbeRootReadonly(const ProbeOptions& options, CapabilityItem* item) {
    struct statvfs info {};
    if (::statvfs(options.root_path.c_str(), &info) != 0) {
        SetUnavailable(item, "statvfs(" + options.root_path + ") 失败：" + ErrorText(errno));
        return;
    }
    if ((info.f_flag & ST_RDONLY) != 0) {
        SetDegraded(item, options.root_path + " 是只读文件系统");
        return;
    }
    SetAvailable(item, options.root_path + " 可写");
}

/// RLIMIT_NOFILE：软上限低于阈值时，高并发连接会以 EMFILE 失败。
void ProbeNofile(const ProbeOptions& options, CapabilityItem* item) {
    if (!options.check_rlimit) {
        SetDegraded(item, "探测被配置禁用（check_rlimit=false）");
        return;
    }
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        SetUnavailable(item, "getrlimit(RLIMIT_NOFILE) 失败：" + ErrorText(errno));
        return;
    }
    if (IsInfinite(limit.rlim_cur)) {
        SetAvailable(item, "RLIMIT_NOFILE=infinity");
        return;
    }
    const std::uint64_t soft = static_cast<std::uint64_t>(limit.rlim_cur);
    const std::string detail = "RLIMIT_NOFILE soft=" + std::to_string(soft) +
                               ", hard=" + std::to_string(static_cast<std::uint64_t>(limit.rlim_max));
    if (soft < options.min_nofile) {
        SetDegraded(item, "RLIMIT_NOFILE soft=" + std::to_string(soft) + " < min_nofile=" +
                              std::to_string(options.min_nofile),
                    detail);
        return;
    }
    SetAvailable(item, detail);
}

/// RLIMIT_NPROC：只记录与（可选）阈值判定，避免线程/进程创建失败。
void ProbeNproc(const ProbeOptions& options, CapabilityItem* item) {
    if (!options.check_rlimit) {
        SetDegraded(item, "探测被配置禁用（check_rlimit=false）");
        return;
    }
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_NPROC, &limit) != 0) {
        SetUnavailable(item, "getrlimit(RLIMIT_NPROC) 失败：" + ErrorText(errno));
        return;
    }
    if (IsInfinite(limit.rlim_cur)) {
        SetAvailable(item, "RLIMIT_NPROC=infinity");
        return;
    }
    const std::uint64_t soft = static_cast<std::uint64_t>(limit.rlim_cur);
    const std::string detail = "RLIMIT_NPROC soft=" + std::to_string(soft);
    if (options.min_nproc != 0 && soft < options.min_nproc) {
        SetDegraded(item, "RLIMIT_NPROC soft=" + std::to_string(soft) + " < min_nproc=" +
                              std::to_string(options.min_nproc),
                    detail);
        return;
    }
    SetAvailable(item, detail);
}

/// 内核信息：探测本身失败说明运行环境异常（极罕见）。
void ProbeKernel(CapabilityItem* item) {
    struct utsname info {};
    if (::uname(&info) != 0) {
        SetUnavailable(item, "uname 失败：" + ErrorText(errno));
        return;
    }
    SetAvailable(item, std::string(info.sysname) + " " + info.release + " " + info.machine);
}

}  // namespace

const char* CapabilityStatusName(CapabilityStatus status) noexcept {
    switch (status) {
        case CapabilityStatus::kAvailable:
            return "available";
        case CapabilityStatus::kDegraded:
            return "degraded";
        case CapabilityStatus::kUnavailable:
            return "unavailable";
    }
    return "unknown";
}

const CapabilityItem* CapabilityMatrix::Find(std::string_view name) const noexcept {
    for (const auto& item : items) {
        if (std::string_view(item.name) == name) {
            return &item;
        }
    }
    return nullptr;
}

std::size_t CapabilityMatrix::Count(CapabilityStatus status) const noexcept {
    std::size_t count = 0;
    for (const auto& item : items) {
        if (item.status == status) {
            ++count;
        }
    }
    return count;
}

bool CapabilityMatrix::IsDegraded() const noexcept {
    for (const auto& item : items) {
        if (!item.available()) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> CapabilityMatrix::Reasons() const {
    std::vector<std::string> reasons;
    reasons.reserve(items.size());
    for (const auto& item : items) {
        if (item.available()) {
            continue;
        }
        const std::string& reason =
            item.reason.empty() ? std::string(CapabilityStatusName(item.status)) : item.reason;
        reasons.push_back(item.name + ": " + reason);
    }
    return reasons;
}

std::string CapabilityMatrix::Summary() const {
    std::string summary = "capabilities: total=" + std::to_string(items.size());
    summary += " available=" + std::to_string(Count(CapabilityStatus::kAvailable));
    summary += " degraded=" + std::to_string(Count(CapabilityStatus::kDegraded));
    summary += " unavailable=" + std::to_string(Count(CapabilityStatus::kUnavailable));
    return summary;
}

CapabilityMatrix Probe(const ProbeOptions& options) {
    CapabilityMatrix matrix;
    matrix.items.reserve(15);

    matrix.items.emplace_back();
    matrix.items.back().name = "proc.meminfo";
    ProbeMeminfo(options, &matrix.items.back());

    matrix.items.emplace_back();
    matrix.items.back().name = "proc.cpuinfo";
    ProbeCpuinfo(options, &matrix.items.back());

    std::uint64_t capability_effective = 0;
    bool capability_known = false;

    matrix.items.emplace_back();
    matrix.items.back().name = "proc.self.status";
    ProbeSelfStatus(options, &matrix.items.back(), &capability_effective);
    capability_known = !matrix.items.back().unavailable();

    matrix.items.emplace_back();
    matrix.items.back().name = "proc.self.fd";
    ProbeSelfFd(options, &matrix.items.back());

    matrix.items.emplace_back();
    matrix.items.back().name = "unix.privilege";
    ProbePrivilege(capability_effective, capability_known, &matrix.items.back());

    matrix.items.emplace_back();
    matrix.items.back().name = "ipc.epoll";
    ProbeEpoll(options, &matrix.items.back());

    std::string cgroup_detail;
    const int cgroup_version = DetectCgroupVersion(options.cgroup_root, &cgroup_detail);

    matrix.items.emplace_back();
    matrix.items.back().name = "cgroup.version";
    ProbeCgroupVersion(options, &matrix.items.back());

    matrix.items.emplace_back();
    matrix.items.back().name = "cgroup.memory";
    ProbeCgroupMemory(options, cgroup_version, &matrix.items.back());

    matrix.items.emplace_back();
    matrix.items.back().name = "cgroup.cpu";
    ProbeCgroupCpu(options, cgroup_version, &matrix.items.back());

    matrix.items.emplace_back();
    matrix.items.back().name = "net.interfaces";
    ProbeInterfaces(options, &matrix.items.back());

    matrix.items.emplace_back();
    matrix.items.back().name = "fs.tmpfs";
    ProbeTmpfs(options, &matrix.items.back());

    matrix.items.emplace_back();
    matrix.items.back().name = "fs.root_readonly";
    ProbeRootReadonly(options, &matrix.items.back());

    matrix.items.emplace_back();
    matrix.items.back().name = "rlimit.nofile";
    ProbeNofile(options, &matrix.items.back());

    matrix.items.emplace_back();
    matrix.items.back().name = "rlimit.nproc";
    ProbeNproc(options, &matrix.items.back());

    matrix.items.emplace_back();
    matrix.items.back().name = "kernel.info";
    ProbeKernel(&matrix.items.back());

    return matrix;
}

}  // namespace go2cpp::bento::app
