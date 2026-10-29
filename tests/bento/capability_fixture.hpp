#pragma once

/**
 * @file capability_fixture.hpp
 * @brief MOD-37 能力探测测试夹具：在临时目录里搭出可注入的 /proc 与
 *        cgroup v1/v2 视图，供 test_capability 与 audit_capability_stress 共用。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 夹具只写临时目录，不触碰真实 /proc、/sys；`TempTree` 析构时递归
 *          删除，权限被改成 000 的文件也能被拥有者 unlink。所有路径都由调用方
 *          拼到 `TempTree::root()` 下，测试之间互不影响。
 */

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace capability_fixture {

constexpr std::uint64_t kMinMemBytes = 64ull * 1024ull * 1024ull;

/// 拼接路径，保证只出现一个分隔符。
inline std::string Join(const std::string& base, std::string_view leaf) {
    std::string out = base;
    if (!out.empty() && out.back() != '/') {
        out.push_back('/');
    }
    out.append(leaf);
    return out;
}

/// 递归创建目录；已存在也算成功。
inline bool MakeDir(const std::string& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    std::error_code status_error;
    return std::filesystem::is_directory(path, status_error);
}

/// 写文件（自动建父目录），并显式设置 mode，便于制造权限拒绝。
inline bool WriteFile(const std::string& path, std::string_view content, mode_t mode = 0644) {
    const std::filesystem::path file_path(path);
    if (file_path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(file_path.parent_path(), error);
    }
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (descriptor < 0) {
        return false;
    }
    std::size_t written = 0;
    bool ok = true;
    while (written < content.size()) {
        const ssize_t count =
            ::write(descriptor, content.data() + written, content.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    if (::close(descriptor) != 0) {
        ok = false;
    }
    if (::chmod(path.c_str(), mode) != 0) {
        ok = false;
    }
    return ok;
}

inline bool SetMode(const std::string& path, mode_t mode) {
    return ::chmod(path.c_str(), mode) == 0;
}

/// 每个测试/变体一个临时根目录，析构时清理（含 000 权限文件）。
class TempTree {
public:
    TempTree() {
        char pattern[] = "/tmp/capability-fixture.XXXXXX";
        char* created = ::mkdtemp(pattern);
        if (created != nullptr) {
            root_ = created;
        }
    }

    ~TempTree() {
        if (!root_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    const std::string& root() const { return root_; }
    bool valid() const { return !root_.empty(); }

private:
    std::string root_;
};

/// 正常 /proc 视图：meminfo、cpuinfo（processors 个 processor 段）、
/// self/status（含 Uid/CapEff）、self/fd 目录。
inline void PopulateProc(const std::string& proc_root, int processors = 2) {
    WriteFile(Join(proc_root, "meminfo"),
              "MemTotal:        8192000 kB\n"
              "MemFree:          512000 kB\n"
              "MemAvailable:    4096000 kB\n"
              "Buffers:           10000 kB\n");
    std::string cpuinfo;
    for (int index = 0; index < processors; ++index) {
        cpuinfo += "processor\t: " + std::to_string(index) + "\n";
        cpuinfo += "model name\t: Capability Fixture CPU\n";
        cpuinfo += "cpu MHz\t\t: 2400.000\n\n";
    }
    WriteFile(Join(proc_root, "cpuinfo"), cpuinfo);
    WriteFile(Join(proc_root, "self/status"),
              "Name:\tcapability_fixture\n"
              "Uid:\t1000\t1000\t1000\t1000\n"
              "Gid:\t1000\t1000\t1000\t1000\n"
              "CapEff:\t0000000000000000\n"
              "Threads:\t1\n");
    MakeDir(Join(proc_root, "self/fd"));
}

/// 只写 self/status（用于「可读但缺字段」的降级分支）。
inline void PopulateStatus(const std::string& proc_root, bool include_cap_eff) {
    std::string text =
        "Name:\tcapability_fixture\n"
        "Uid:\t1000\t1000\t1000\t1000\n";
    if (include_cap_eff) {
        text += "CapEff:\t0000000000000000\n";
    }
    text += "Threads:\t1\n";
    WriteFile(Join(proc_root, "self/status"), text);
}

/// cgroup v2 视图：cgroup.controllers + memory.max + cpu.max。
inline void PopulateCgroupV2(const std::string& cgroup_root, std::string_view memory_max,
                             std::string_view cpu_max) {
    WriteFile(Join(cgroup_root, "cgroup.controllers"), "cpu memory pids\n");
    WriteFile(Join(cgroup_root, "memory.max"), memory_max);
    WriteFile(Join(cgroup_root, "cpu.max"), cpu_max);
}

/// cgroup v1 视图：memory/memory.limit_in_bytes + cpu/cpu.cfs_quota_us。
inline void PopulateCgroupV1(const std::string& cgroup_root, std::string_view memory_limit,
                             std::string_view cpu_quota) {
    WriteFile(Join(cgroup_root, "memory/memory.limit_in_bytes"), memory_limit);
    WriteFile(Join(cgroup_root, "cpu/cpu.cfs_quota_us"), cpu_quota);
}

}  // namespace capability_fixture
