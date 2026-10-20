/**
 * @file dir.cpp
 * @brief `os` 模块的目录 / 路径 / 符号链接 / 时间戳实现。
 *
 * 关于「这里为什么不像 file.cpp 那样走 `syscall(SYS_*)`」的完整论证见
 * `dir.hpp` 顶部 —— 一句话：`HookOp` 里没有 `opendir`/`readdir`/`getdents`，
 * 它们不会被 Hook 拦到等待路径上，所以直接用 libc 更简单也更快（能用 `d_type`）。
 */
/**
 * ## 模块定位（补充）
 *
 * `os` 目录能力的实现层：被 `tests/os/test_os_dir.cpp`、examples 与后续的
 * 配置目录巡检调用；向下直接用 libc `opendir/readdir/mkdir/unlink/readlink/
 * symlink/stat/lstat`，只有 `SetTimes` 走 `SYS_utimensat`。
 *
 * ## 本文件定义什么
 *
 * - `ReadDir`：一次目录枚举，按 `d_type` 填 `DirEntry`，跳过 `.`/`..`；
 * - `MkdirAll`/`RemoveAll`：递归建/删，语义对齐 `mkdir -p`/`rm -rf`；
 * - `Readlink`（动态扩容）/`Symlink`/`SetTimes`/`StatPath`。
 *
 * ## 阅读建议
 *
 * 先看 `ReadDir` 的 errno 归零时机（`readdir` 用 nullptr 同时表示 EOF 和错误），
 * 再看 `RemoveAll` 的「先收集子项再递归删除」，最后看 `Readlink` 的翻倍重试。
 *
 * ## 边界与陷阱
 *
 * - `d_name` 指向 dirent 内部缓冲，必须立即拷贝；
 * - `RemoveAll` 用 `lstat`：符号链接只删链接本身，不递归进目标；
 * - `MkdirAll` 不清理中途已创建的目录（与 `mkdir -p` 一致）。
 */
#include "go2cpp/os/dir.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string_view>
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

/**
 * @brief 路径的**最后一段**是否是 `.` 或 `..`（先去掉尾随 `/`）。
 * @details `RemoveAll("a/.")` 与 `RemoveAll(".")` 的危害相同：都会先清空该目录
 *          的全部子项，再在 `rmdir("a/.")` / `rmdir(".")` 阶段失败 —— 没有任何
 *          成功可能，只有破坏性。`a/../b` 这种中间段不受影响，不在此列。
 */
bool EndsWithDotSegment(std::string_view path) noexcept {
    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    const auto slash = path.rfind('/');
    const std::string_view last =
        (slash == std::string_view::npos) ? path : path.substr(slash + 1);
    return last == "." || last == "..";
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

/**
 * @brief 列出一个目录下的条目（不递归、不排序、跳过 `.`/`..`）。
 *
 * ## 步骤
 * step1. 校验输出指针与路径，清空结果；
 * step2. `opendir`；失败返回 `kOpenFailed` 并保留 errno；
 * step3. 循环 `readdir`：名字立即拷贝，按 `d_type` 填类型标记；
 * step4. `DT_UNKNOWN` 只置 `type_unknown`，不自动补 `lstat`（避免每项多一次 syscall）；
 * step5. 结束后用 errno 区分 EOF 与读取错误；出错时清空结果再返回。
 */
FileStatus ReadDir(const std::string& path, std::vector<DirEntry>* out,
                   std::string* error) {
    if (out == nullptr) {
        return Fail(OsError::kInvalidArgument, EINVAL, error,
                    "ReadDir: out 为空指针");
    }
    out->clear();
    if (path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "ReadDir: 路径为空");
    }
    if (detail::ContainsNul(path)) {
        return Fail(OsError::kInvalidArgument, EINVAL, error,
                    "ReadDir: 路径含内嵌 NUL");
    }

    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) {
        return Fail(OsError::kOpenFailed, errno, error, "ReadDir 打开目录失败: " + path);
    }

    // step3: 必须在循环前清零 errno，否则会继承上一次调用的残留值
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
    // step5: 读取中途失败时不能把已收集的半份列表交给调用方
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

/**
 * @brief 逐段递归创建目录（等价 `mkdir -p`）。
 *
 * ## 步骤
 * step1. 空路径直接拒绝；
 * step2. 从第一个 `/` 起逐段取前缀，构造当前要创建的路径；
 * step3. `mkdir`：`EEXIST` 时用 `stat` 确认已是目录，否则报错；
 * step4. 已存在且是目录则继续下一段；中途失败立即返回，不回收已建目录。
 */
FileStatus MkdirAll(const std::string& path, mode_t mode, std::string* error) {
    if (path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "MkdirAll: 路径为空");
    }
    if (detail::ContainsNul(path)) {
        return Fail(OsError::kInvalidArgument, EINVAL, error,
                    "MkdirAll: 路径含内嵌 NUL");
    }

    // step2: current 是累积前缀，position 指向下一段的起点
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

        // step3: EEXIST 不算成功，必须再确认它是目录
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

/**
 * @brief 递归删除文件或目录（等价 `rm -rf`，不跟随符号链接）。
 *
 * ## 步骤
 * step0. 拒绝 `.` / `..` / `a/.` / `a/..`：它们会清空目录再在 rmdir 失败；
 * step1. `lstat` 判断类型；`ENOENT` 视为成功（幂等）；
 * step2. 非目录（含符号链接）直接 `unlink`，绝不递归进链接目标；
 * step3. 目录先收集全部子项名字，避免 readdir 期间删除导致条目被跳过；
 * step4. 逐个递归删除子项，任一失败立即返回；
 * step5. 最后 `rmdir` 空目录。
 */
FileStatus RemoveAll(const std::string& path, std::string* error) {
    if (path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "RemoveAll: 路径为空");
    }
    if (detail::ContainsNul(path)) {
        return Fail(OsError::kInvalidArgument, EINVAL, error,
                    "RemoveAll: 路径含内嵌 NUL");
    }
    // 显式拒绝 `.` / `..`（含 `a/.`、`a/..` 这类点段结尾）：
    // 它们会先把当前目录/父目录的子项全部删掉，再在 rmdir 时必然失败，
    // 调用方拿到的只是一个错误，现场却已被清空 —— 一次误调用就能毁掉工作目录。
    // Go 的 os.RemoveAll(".") 是先删子项再报错；这里有意更严格（审计 D4）。
    if (EndsWithDotSegment(path)) {
        return Fail(OsError::kInvalidArgument, EINVAL, error,
                    "RemoveAll 拒绝删除 '.' 或 '..'（会清空当前/父目录）: " + path);
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

    // step2: lstat 已保证符号链接走到这里，删的是链接本身
    // 不是目录（含符号链接）：直接 unlink
    if (!S_ISDIR(info.st_mode)) {
        if (::unlink(path.c_str()) != 0) {
            return Fail(OsError::kIoFailed, errno, error, "RemoveAll 删除失败: " + path);
        }
        return FileStatus{};
    }

    // step3: readdir 迭代期间删除条目的行为在部分文件系统上未定义
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

    // step4: 子项删除失败时保留现场并返回，便于调用方定位
    for (const auto& child : children) {
        const auto status = RemoveAll(child, error);
        if (!status.ok()) {
            return status;
        }
    }

    // step5: 子项全部删除后目录应为空，rmdir 失败说明有并发写入或权限问题
    if (::rmdir(path.c_str()) != 0) {
        return Fail(OsError::kIoFailed, errno, error, "RemoveAll 删除目录失败: " + path);
    }
    return FileStatus{};
}

// ---------------------------------------------------------------------------
// Readlink / Symlink
// ---------------------------------------------------------------------------

/**
 * @brief 读取符号链接目标（不跟随），缓冲区不足时翻倍重试。
 *
 * ## 步骤
 * step1. 校验输出与路径；
 * step2. 从 256 字节开始 `readlink`；
 * step3. 返回值 < 缓冲区大小说明未截断，收缩到实际长度返回；
 * step4. 否则容量翻倍重试，最多 8 次（32 KiB），仍不足报 `ENAMETOOLONG`。
 *
 * @note `readlink` 在缓冲区不足时**静默截断**并返回缓冲区大小，所以不能用
 *       「返回值 < 0 才算失败」来判断。
 */
FileStatus Readlink(const std::string& path, std::string* out, std::string* error) {
    if (out == nullptr) {
        return Fail(OsError::kInvalidArgument, EINVAL, error,
                    "Readlink: out 为空指针");
    }
    out->clear();
    if (path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "Readlink: 路径为空");
    }
    if (detail::ContainsNul(path)) {
        return Fail(OsError::kInvalidArgument, EINVAL, error,
                    "Readlink: 路径含内嵌 NUL");
    }

    // 动态扩容：readlink 在缓冲区不够时会**静默截断**并返回缓冲区大小，
    // 所以"返回值 == 缓冲区大小"是"可能被截断"的信号，要翻倍重试。
    // step2: 256 字节覆盖绝大多数链接；不足时按 2 倍增长
    std::size_t capacity = 256;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::string buffer(capacity, '\0');
        const ssize_t written =
            ::readlink(path.c_str(), buffer.data(), buffer.size());
        // step3: 负值才是真错误；非负值可能仍被截断
        if (written < 0) {
            return Fail(OsError::kIoFailed, errno, error, "Readlink 失败: " + path);
        }
        const auto length = static_cast<std::size_t>(written);
        if (length < buffer.size()) {
            buffer.resize(length);
            *out = std::move(buffer);
            return FileStatus{};
        }
        // step4: 返回值等于缓冲区大小 = 可能截断，翻倍重试
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
    // target 也会原样交给 symlink(2)；含 NUL 同样会被截断成另一个链接目标。
    if (detail::ContainsNul(target) || detail::ContainsNul(link_path)) {
        return Fail(OsError::kInvalidArgument, EINVAL, error,
                    "Symlink: 路径含内嵌 NUL");
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
    if (detail::ContainsNul(path)) {
        return Fail(OsError::kInvalidArgument, EINVAL, error,
                    "SetTimes: 路径含内嵌 NUL");
    }
    struct timespec times[2];
    times[0] = atime;   // [0] = 访问时间
    times[1] = mtime;   // [1] = 修改时间
    // 走 syscall 而不是 utimensat(): 避开不同 glibc 版本对 timespec 的封装差异。
    // flags == 0 表示**跟随符号链接**（与 Go 的 os.Chtimes 一致）；若需要
    // 不跟随，得传 AT_SYMLINK_NOFOLLOW，本 API 暂未暴露该选项。
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
        return Fail(OsError::kInvalidArgument, EINVAL, error,
                    "StatPath: out 为空指针");
    }
    if (path.empty()) {
        return Fail(OsError::kInvalidArgument, EINVAL, error, "StatPath: 路径为空");
    }
    if (detail::ContainsNul(path)) {
        return Fail(OsError::kInvalidArgument, EINVAL, error,
                    "StatPath: 路径含内嵌 NUL");
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
