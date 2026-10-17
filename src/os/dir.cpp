/**
 * @file dir.cpp
 * @brief `os` 模块的目录 / 路径 / 符号链接 / 时间戳实现。
 *
 * 关于「这里为什么不像 file.cpp 那样走 `syscall(SYS_*)`」的完整论证见
 * `dir.hpp` 顶部 —— 一句话：`HookOp` 里没有 `opendir`/`readdir`/`getdents`，
 * 它们不会被 Hook 拦到等待路径上，所以直接用 libc 更简单也更快（能用 `d_type`）。
 */
#include "go2cpp/os/dir.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace go2cpp::os {

namespace {

/// syscall 系列返回 -1 表示失败（成功时返回的值可能恰好是 -1 的合法值，
/// 所以只在本文件用到的这几个 syscall 上用它 —— 它们的返回值都不是 -1）。
bool IsSyscallError(long result) noexcept { return result == -1; }

/// 把 errno 转成可读文本，拼进诊断信息。
std::string Describe(const std::string& what, int err) {
    std::string text = what;
    if (err != 0) {
        text += ": ";
        text += std::strerror(err);
        text += " (errno=";
        text += std::to_string(err);
        text += ")";
    }
    return text;
}

FileStatus Fail(OsError code, int err, std::string* error, const std::string& what) {
    FileStatus status;
    status.code = code;
    status.system_error = err;
    if (error != nullptr) {
        *error = Describe(what, err);
    }
    return status;
}

}  // namespace

// ---------------------------------------------------------------------------
// ReadDir
// ---------------------------------------------------------------------------

FileStatus ReadDir(const std::string& path, std::vector<DirEntry>* out,
                   std::string* error) {
    if (out == nullptr) {
        return Fail(OsError::kInvalidArgument, 0, error, "ReadDir: out 为空指针");
    }
    out->clear();
    if (path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "ReadDir: 路径为空");
    }

    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) {
        return Fail(OsError::kOpenFailed, errno, error, "ReadDir 打开目录失败: " + path);
    }

    // `readdir` 出错与"读完了"都返回 nullptr，靠 errno 区分。
    // 必须在循环前清零，否则会把上一次调用的残留 errno 当成错误。
    errno = 0;
    for (;;) {
        struct ::dirent* entry = ::readdir(dir);
        if (entry == nullptr) {
            break;
        }
        // 跳过 . 与 .. —— 与 Go 的 os.ReadDir 一致
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        DirEntry item;
        // `d_name` 指向 dirent 的内部缓冲，下一次 readdir 会覆盖它，**必须拷贝**。
        item.name = entry->d_name;
        switch (entry->d_type) {
            case DT_DIR:
                item.is_dir = true;
                break;
            case DT_REG:
                item.is_regular = true;
                break;
            case DT_LNK:
                item.is_symlink = true;
                break;
            case DT_UNKNOWN:
                // 某些文件系统（部分 NFS/FUSE）不填 d_type，需要 lstat 补。
                // 这里**不自动补** —— 补的话每个条目都多一次 syscall，
                // 而多数调用方只用 name。标记 type_unknown 让调用方自己决定。
                item.type_unknown = true;
                break;
            default:
                // 块设备/字符设备/FIFO/套接字：三个标记都留 false
                break;
        }
        out->push_back(std::move(item));
        errno = 0;
    }
    const int readdir_errno = errno;
    ::closedir(dir);
    if (readdir_errno != 0) {
        out->clear();
        return Fail(OsError::kIoFailed, readdir_errno, error,
                    "ReadDir 遍历失败: " + path);
    }
    return FileStatus{};
}

// ---------------------------------------------------------------------------
// MkdirAll
// ---------------------------------------------------------------------------

FileStatus MkdirAll(const std::string& path, mode_t mode, std::string* error) {
    if (path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "MkdirAll: 路径为空");
    }

    // 逐段创建：从第一个 '/' 开始，每往后一段就 mkdir 一次。
    // 这样 "/a/b/c" 会依次尝试 "/a"、"/a/b"、"/a/b/c"。
    std::string current;
    current.reserve(path.size());
    std::size_t position = 0;
    while (position <= path.size()) {
        const std::size_t slash = path.find('/', position);
        const std::size_t end = (slash == std::string::npos) ? path.size() : slash;
        current.assign(path, 0, end);
        position = end + 1;

        if (current.empty()) {
            // 绝对路径开头的 '/'：跳过，根目录一定存在
            if (end + 1 > path.size()) {
                break;
            }
            continue;
        }

        if (::mkdir(current.c_str(), mode) != 0) {
            const int mkdir_errno = errno;
            if (mkdir_errno == EEXIST) {
                // 已存在：必须是目录，否则是错误（对齐 mkdir -p 的行为）
                struct ::stat info {};
                if (::stat(current.c_str(), &info) != 0) {
                    return Fail(OsError::kIoFailed, errno, error,
                                "MkdirAll 检查已存在路径失败: " + current);
                }
                if (!S_ISDIR(info.st_mode)) {
                    return Fail(OsError::kIoFailed, EEXIST, error,
                                "MkdirAll: 路径已存在但不是目录: " + current);
                }
                // 是目录，继续下一段
            } else {
                return Fail(OsError::kIoFailed, mkdir_errno, error,
                            "MkdirAll 创建目录失败: " + current);
            }
        }
        if (end + 1 > path.size()) {
            break;
        }
    }
    return FileStatus{};
}

// ---------------------------------------------------------------------------
// RemoveAll
// ---------------------------------------------------------------------------

FileStatus RemoveAll(const std::string& path, std::string* error) {
    if (path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "RemoveAll: 路径为空");
    }

    // 用 lstat 而非 stat：符号链接要删链接本身，不跟随目标。
    struct ::stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        const int lstat_errno = errno;
        if (lstat_errno == ENOENT) {
            // 不存在视为成功 —— 与 Go 的 os.RemoveAll 一致
            return FileStatus{};
        }
        return Fail(OsError::kIoFailed, lstat_errno, error,
                    "RemoveAll 检查路径失败: " + path);
    }

    // 不是目录（含符号链接）：直接 unlink
    if (!S_ISDIR(info.st_mode)) {
        if (::unlink(path.c_str()) != 0) {
            return Fail(OsError::kIoFailed, errno, error, "RemoveAll 删除失败: " + path);
        }
        return FileStatus{};
    }

    // 是目录：两阶段 —— 先把子项名字全部收集出来，再逐个递归删除。
    // 理由：在 readdir 迭代过程中删除条目，某些文件系统上行为未定义
    // （可能跳过条目），所以不能边读边删。
    std::vector<std::string> children;
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) {
        return Fail(OsError::kOpenFailed, errno, error,
                    "RemoveAll 打开目录失败: " + path);
    }
    errno = 0;
    for (;;) {
        struct ::dirent* entry = ::readdir(dir);
        if (entry == nullptr) {
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        std::string child = path;
        if (!child.empty() && child.back() != '/') {
            child.push_back('/');
        }
        child += entry->d_name;
        children.push_back(std::move(child));
        errno = 0;
    }
    const int readdir_errno = errno;
    ::closedir(dir);
    if (readdir_errno != 0) {
        return Fail(OsError::kIoFailed, readdir_errno, error,
                    "RemoveAll 遍历失败: " + path);
    }

    for (const auto& child : children) {
        const auto status = RemoveAll(child, error);
        if (!status.ok()) {
            return status;
        }
    }

    if (::rmdir(path.c_str()) != 0) {
        return Fail(OsError::kIoFailed, errno, error, "RemoveAll 删除目录失败: " + path);
    }
    return FileStatus{};
}

// ---------------------------------------------------------------------------
// Readlink / Symlink
// ---------------------------------------------------------------------------

FileStatus Readlink(const std::string& path, std::string* out, std::string* error) {
    if (out == nullptr) {
        return Fail(OsError::kInvalidArgument, 0, error, "Readlink: out 为空指针");
    }
    out->clear();
    if (path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "Readlink: 路径为空");
    }

    // 动态扩容：readlink 在缓冲区不够时会**静默截断**并返回缓冲区大小，
    // 所以"返回值 == 缓冲区大小"是"可能被截断"的信号，要翻倍重试。
    std::size_t capacity = 256;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::string buffer(capacity, '\0');
        const ssize_t written =
            ::readlink(path.c_str(), buffer.data(), buffer.size());
        if (written < 0) {
            return Fail(OsError::kIoFailed, errno, error, "Readlink 失败: " + path);
        }
        const auto length = static_cast<std::size_t>(written);
        if (length < buffer.size()) {
            buffer.resize(length);
            *out = std::move(buffer);
            return FileStatus{};
        }
        capacity *= 2;
    }
    return Fail(OsError::kIoFailed, ENAMETOOLONG, error,
                "Readlink 目标过长（超过 32 KiB）: " + path);
}

FileStatus Symlink(const std::string& target, const std::string& link_path,
                   std::string* error) {
    if (target.empty() || link_path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "Symlink: 路径为空");
    }
    if (::symlink(target.c_str(), link_path.c_str()) != 0) {
        return Fail(OsError::kIoFailed, errno, error,
                    "Symlink 创建失败: " + link_path + " -> " + target);
    }
    return FileStatus{};
}

// ---------------------------------------------------------------------------
// SetTimes
// ---------------------------------------------------------------------------

FileStatus SetTimes(const std::string& path, const struct timespec& atime,
                    const struct timespec& mtime, std::string* error) {
    if (path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "SetTimes: 路径为空");
    }
    struct timespec times[2];
    times[0] = atime;   // [0] = 访问时间
    times[1] = mtime;   // [1] = 修改时间
    // 走 syscall 而不是 utimensat(): 避开不同 glibc 版本对 timespec 的封装差异。
    const long result =
        ::syscall(SYS_utimensat, AT_FDCWD, path.c_str(), times, 0);
    if (IsSyscallError(result)) {
        return Fail(OsError::kIoFailed, errno, error, "SetTimes 失败: " + path);
    }
    return FileStatus{};
}

// ---------------------------------------------------------------------------
// StatPath
// ---------------------------------------------------------------------------

FileStatus StatPath(const std::string& path, struct ::stat* out, bool follow_symlink,
                    std::string* error) {
    if (out == nullptr) {
        return Fail(OsError::kInvalidArgument, 0, error, "StatPath: out 为空指针");
    }
    if (path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "StatPath: 路径为空");
    }
    const int result = follow_symlink ? ::stat(path.c_str(), out)
                                      : ::lstat(path.c_str(), out);
    if (result != 0) {
        return Fail(OsError::kIoFailed, errno, error,
                    std::string(follow_symlink ? "stat" : "lstat") + " 失败: " + path);
    }
    return FileStatus{};
}

}  // namespace go2cpp::os
