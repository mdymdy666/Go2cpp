#pragma once

/**
 * @file test_backup_support.hpp
 * @brief MOD-31 测试共用的临时目录、随机数据、目录树比较与命令执行辅助。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "go2cpp/os/proc.hpp"

namespace bento_test {

namespace fs = std::filesystem;

/// 构造即创建的临时目录；析构递归删除，保证测试不留垃圾。
class TempDir final {
public:
    explicit TempDir(const std::string& tag) {
        static std::atomic<int> counter{0};
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec);
        const fs::path root = ec ? fs::path("/tmp") : base;
        m_path = root / ("bento_mod31_" + tag + "_" + std::to_string(::getpid()) + "_" +
                         std::to_string(++counter));
        fs::remove_all(m_path, ec);
        fs::create_directories(m_path, ec);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const noexcept { return m_path; }
    fs::path operator/(const std::string& child) const { return m_path / child; }

private:
    fs::path m_path;
};

/// 写文件；父目录自动创建。
inline bool WriteFile(const fs::path& path, const std::vector<std::uint8_t>& data) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    out.flush();
    return static_cast<bool>(out);
}

/// 写文本文件。
inline bool WriteText(const fs::path& path, const std::string& text) {
    return WriteFile(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

/// 读整个文件；失败返回空 vector（空文件也返回空，调用方用 exists 区分）。
inline std::vector<std::uint8_t> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> data;
    if (!in) {
        return data;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size > 0) {
        data.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        if (!in) {
            data.clear();
        }
    }
    return data;
}

/// 确定性伪随机字节（xorshift64），用于大文件/随机树。
inline std::vector<std::uint8_t> RandomBytes(std::size_t size, std::uint64_t seed) {
    std::uint64_t state = seed == 0 ? 0x9e3779b97f4a7c15ull : seed;
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        data[i] = static_cast<std::uint8_t>(state >> 24);
    }
    return data;
}

/// 递归收集 root 下的相对路径（不含 root 自身）。
inline bool CollectTree(const fs::path& root, std::vector<std::string>* entries) {
    std::error_code ec;
    fs::recursive_directory_iterator it(root, ec);
    if (ec) {
        return false;
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            return false;
        }
        std::error_code relative_ec;
        const fs::path relative = fs::relative(it->path(), root, relative_ec);
        if (relative_ec) {
            return false;
        }
        entries->push_back(relative.generic_string());
    }
    return true;
}

/**
 * @brief 比较两棵目录树：相对路径集合、条目类型、文件大小与逐字节内容。
 * @return 相同返回 true；不同时写 `error`。
 */
inline bool CompareTrees(const fs::path& left, const fs::path& right, std::string* error) {
    std::vector<std::string> left_entries;
    std::vector<std::string> right_entries;
    if (!CollectTree(left, &left_entries) || !CollectTree(right, &right_entries)) {
        *error = "无法遍历目录树";
        return false;
    }
    std::sort(left_entries.begin(), left_entries.end());
    std::sort(right_entries.begin(), right_entries.end());
    if (left_entries != right_entries) {
        *error = "条目集合不同: " + std::to_string(left_entries.size()) + " vs " +
                 std::to_string(right_entries.size());
        return false;
    }
    for (const std::string& relative : left_entries) {
        std::error_code ec;
        const fs::path lpath = left / relative;
        const fs::path rpath = right / relative;
        const fs::file_status lstatus = fs::symlink_status(lpath, ec);
        const fs::file_status rstatus = fs::symlink_status(rpath, ec);
        if (ec || lstatus.type() != rstatus.type()) {
            *error = "条目类型不同: " + relative;
            return false;
        }
        if (lstatus.type() != fs::file_type::regular) {
            continue;
        }
        const std::uintmax_t lsize = fs::file_size(lpath, ec);
        const std::uintmax_t rsize = fs::file_size(rpath, ec);
        if (ec || lsize != rsize) {
            *error = "文件大小不同: " + relative;
            return false;
        }
        if (ReadFile(lpath) != ReadFile(rpath)) {
            *error = "文件内容不同: " + relative;
            return false;
        }
    }
    return true;
}

/// 命令执行结果。
struct CommandResult final {
    bool ran{false};      ///< 进程是否成功启动并回收。
    bool ok{false};       ///< 退出码 0。
    int exit_code{0};     ///< 退出码。
    std::string out;      ///< stdout。
    std::string err;      ///< stderr。
};

/// 直接 exec（不经过 shell）执行命令。
inline CommandResult RunCommand(const std::vector<std::string>& argv) {
    CommandResult result;
    go2cpp::os::ProcessOptions options;
    options.argv = argv;
    options.use_path_search = true;
    options.timeout = std::chrono::seconds(30);
    options.max_output_bytes = 4 * 1024 * 1024;
    const go2cpp::os::Result<go2cpp::os::RunResult> run = go2cpp::os::Run(options);
    if (!run) {
        result.err = "spawn failed";
        return result;
    }
    result.ran = true;
    result.ok = run->status.exited && run->status.exit_code == 0;
    result.exit_code = run->status.exit_code;
    result.out = run->standard_output;
    result.err = run->standard_error;
    return result;
}

/// PATH 中是否有 unzip（用于与系统工具交叉验证）。
inline bool HasUnzip() {
    const CommandResult result = RunCommand({"unzip", "-v"});
    return result.ran;
}

/// `unzip -t`：校验系统工具认可归档完整性。
inline bool UnzipTest(const fs::path& zip, std::string* error) {
    const CommandResult result = RunCommand({"unzip", "-t", zip.string()});
    if (!result.ok) {
        *error = result.err.empty() ? result.out : result.err;
        return false;
    }
    return true;
}

/// `unzip -p <zip> <name>`：用系统 unzip 读出单个条目。
inline std::vector<std::uint8_t> UnzipRead(const fs::path& zip, const std::string& name,
                                           bool* ok) {
    const CommandResult result = RunCommand({"unzip", "-p", zip.string(), name});
    if (ok != nullptr) {
        *ok = result.ok;
    }
    return std::vector<std::uint8_t>(result.out.begin(), result.out.end());
}

} // namespace bento_test
