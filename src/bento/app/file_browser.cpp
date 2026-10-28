/**
 * @file file_browser.cpp
 * @brief MOD-30：受限根目录的 List/Get/Put 实现。
 * @details 关键取舍：
 *
 *          - **拒绝而非修复**：`..` 段出现在任何位置都直接拒绝（即使规范化
 *            后仍在根目录内），这样审计只需看规则，不用重放路径解析；
 *          - **两次检查**：解析路径时逐段 `lstat` 拒绝符号链接（默认），
 *            打开文件时再带 `O_NOFOLLOW`，缩小「检查后替换」窗口；
 *          - **原子落盘**：Get/Put 都写同目录临时文件再发布；`overwrite=false`
 *            用 `link()` 占位（目标已存在时内核以 EEXIST 拒绝），消除
 *            「检查后目标被创建、rename 仍覆盖」的 TOCTOU；失败清理；
 *          - **复制前后核对大小**：源文件在复制期间变长/变短都按 IO 错误
 *            处理，宁可失败也不产出与 `st_size` 不符的副本。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/app/file_browser.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace go2cpp::bento::app {
namespace {

namespace fs = std::filesystem;

/// 临时文件序号；只在同一进程内保证唯一。
std::atomic<std::uint64_t> g_part_seq{0};

/// 是否 ASCII 控制字符。
bool IsControlByte(char ch) noexcept {
    const auto byte = static_cast<unsigned char>(ch);
    return byte < 0x20 || byte == 0x7f;
}

/// errno 到状态码的统一映射（权限与不存在单独分类）。
FileBrowserStatus MapErrno(int error) noexcept {
    switch (error) {
        case EACCES:
        case EPERM:
            return FileBrowserStatus::kPermissionDenied;
        case ENOENT:
        case ENOTDIR:
            return FileBrowserStatus::kNotFound;
        case ELOOP:
        case ENAMETOOLONG:
            return FileBrowserStatus::kPathRejected;
        case EISDIR:
            return FileBrowserStatus::kNotRegular;
        case EEXIST:
            // 不覆盖发布（link 占位）在目标已存在时返回 EEXIST。
            return FileBrowserStatus::kAlreadyExists;
        default:
            return FileBrowserStatus::kIoError;
    }
}

/// `std::error_code` 到状态码的统一映射。
FileBrowserStatus MapError(const std::error_code& ec) noexcept {
    if (!ec) {
        return FileBrowserStatus::kOk;
    }
    if (ec == std::errc::permission_denied) {
        return FileBrowserStatus::kPermissionDenied;
    }
    if (ec == std::errc::no_such_file_or_directory ||
        ec == std::errc::not_a_directory) {
        return FileBrowserStatus::kNotFound;
    }
    return FileBrowserStatus::kIoError;
}

/// `st_mode` 到类型。
FileEntryType TypeFromMode(mode_t mode) noexcept {
    if (S_ISREG(mode)) {
        return FileEntryType::kFile;
    }
    if (S_ISDIR(mode)) {
        return FileEntryType::kDirectory;
    }
    if (S_ISLNK(mode)) {
        return FileEntryType::kSymlink;
    }
    return FileEntryType::kOther;
}

/// `struct stat` 的 mtime 转 Unix epoch 毫秒。
std::int64_t MtimeMs(const struct stat& info) noexcept {
    const std::int64_t seconds = static_cast<std::int64_t>(info.st_mtim.tv_sec);
    const std::int64_t nanos = static_cast<std::int64_t>(info.st_mtim.tv_nsec);
    return seconds * 1000 + nanos / 1000000;
}

/**
 * @brief 校验并拆分相对路径。
 * @details 空路径返回 true 且 `segments` 为空；调用方用 `allow_root` 决定
 *          是否接受根目录。路径段不允许空、`.`、`..`、反斜杠、冒号和控制字符。
 */
bool SplitRelativePath(std::string_view rel, std::size_t max_bytes,
                       std::vector<std::string>* segments) {
    if (rel.size() > max_bytes) {
        return false;
    }
    if (rel.empty()) {
        return true;
    }
    if (rel.front() == '/') {
        return false;
    }
    std::size_t start = 0;
    while (start <= rel.size()) {
        const std::size_t slash = rel.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? rel.size() : slash;
        const std::string_view segment = rel.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        for (char ch : segment) {
            if (ch == '\\' || ch == ':' || IsControlByte(ch)) {
                return false;
            }
        }
        segments->emplace_back(segment);
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

/// 生成同目录、同前缀的临时文件路径（pid + 单调序号）。
std::string MakeSiblingTempPath(const std::string& target) {
    const std::uint64_t sequence = g_part_seq.fetch_add(1, std::memory_order_relaxed);
    return target + ".go2cpp-part." +
           std::to_string(static_cast<long long>(::getpid())) + "." +
           std::to_string(static_cast<unsigned long long>(sequence));
}

/// 以 O_EXCL|O_NOFOLLOW 创建临时文件；失败返回 -1（errno 有效）。
int OpenExclusiveTemp(const std::string& path) {
    return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
}

/// 把 `source_fd` 的 `expected_bytes` 字节复制到 `target_fd`。
bool CopyFileContent(int source_fd, int target_fd, std::uint64_t expected_bytes) {
    std::array<char, 128 * 1024> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        const ssize_t got = ::read(source_fd, buffer.data(), buffer.size());
        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        std::size_t written = 0;
        const auto size = static_cast<std::size_t>(got);
        while (written < size) {
            const ssize_t result = ::write(target_fd, buffer.data() + written, size - written);
            if (result > 0) {
                written += static_cast<std::size_t>(result);
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        total += size;
        if (total > expected_bytes) {
            return false;
        }
    }
    return total == expected_bytes;
}

/// 把临时文件发布为目标；`overwrite=false` 时保证「目标已存在则失败」。
/// @details 不覆盖路径用 `link()` 创建新目录项：目标已存在时内核以 EEXIST
///          拒绝，且发布动作本身是原子的；相比「先 lstat 再 rename」消除了
///          「检查后目标被创建、rename 仍然覆盖」的 TOCTOU 窗口（D-05）。
///          允许覆盖时仍用 `rename()` 原子替换。
/// @return kOk / kAlreadyExists / 其它 IO 状态；失败时调用方负责清理临时文件。
FileBrowserStatus PublishTempFile(const std::string& temp_path, const std::string& target,
                                  bool overwrite) {
    if (overwrite) {
        if (::rename(temp_path.c_str(), target.c_str()) == 0) {
            return FileBrowserStatus::kOk;
        }
        const int error = errno;
        ::unlink(temp_path.c_str());
        return MapErrno(error);
    }
    if (::link(temp_path.c_str(), target.c_str()) != 0) {
        const int error = errno;
        ::unlink(temp_path.c_str());
        return MapErrno(error);
    }
    // link 成功后目标已指向同一 inode；临时名只是多余硬链接，删除即可。
    ::unlink(temp_path.c_str());
    return FileBrowserStatus::kOk;
}

/// 复制文件到新临时文件并原子发布；返回最终状态。
FileBrowserStatus CopyAtomically(const std::string& source, const std::string& target,
                                 std::uint64_t expected_bytes, bool follow_source,
                                 bool overwrite) {
    const std::string temp_path = MakeSiblingTempPath(target);
    int target_fd = OpenExclusiveTemp(temp_path);
    if (target_fd < 0) {
        return MapErrno(errno);
    }
    const int flags = O_RDONLY | O_CLOEXEC | (follow_source ? 0 : O_NOFOLLOW);
    int source_fd = ::open(source.c_str(), flags);
    if (source_fd < 0) {
        const int error = errno;
        ::close(target_fd);
        ::unlink(temp_path.c_str());
        return MapErrno(error);
    }

    const bool copied = CopyFileContent(source_fd, target_fd, expected_bytes);
    const int close_source = ::close(source_fd);
    const int close_target = ::close(target_fd);
    if (!copied || close_source != 0 || close_target != 0) {
        ::unlink(temp_path.c_str());
        return FileBrowserStatus::kIoError;
    }
    return PublishTempFile(temp_path, target, overwrite);
}

}  // namespace

// ---------------------------------------------------------------------------
// 状态/类型转文本
// ---------------------------------------------------------------------------

const char* FileEntryTypeToString(FileEntryType type) noexcept {
    switch (type) {
        case FileEntryType::kFile:
            return "file";
        case FileEntryType::kDirectory:
            return "directory";
        case FileEntryType::kSymlink:
            return "symlink";
        case FileEntryType::kOther:
            return "other";
    }
    return "unknown";
}

const char* FileBrowserStatusToString(FileBrowserStatus status) noexcept {
    switch (status) {
        case FileBrowserStatus::kOk:
            return "ok";
        case FileBrowserStatus::kInvalidArgument:
            return "invalid_argument";
        case FileBrowserStatus::kPathRejected:
            return "path_rejected";
        case FileBrowserStatus::kNotFound:
            return "not_found";
        case FileBrowserStatus::kNotRegular:
            return "not_regular";
        case FileBrowserStatus::kTooLarge:
            return "too_large";
        case FileBrowserStatus::kIoError:
            return "io_error";
        case FileBrowserStatus::kPermissionDenied:
            return "permission_denied";
        case FileBrowserStatus::kAlreadyExists:
            return "already_exists";
        case FileBrowserStatus::kTooManyEntries:
            return "too_many_entries";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// FileBrowser
// ---------------------------------------------------------------------------

FileBrowser::FileBrowser(std::string root, FileBrowserOptions options)
    : m_options(std::move(options)) {
    const bool options_valid =
        m_options.max_file_bytes != 0 &&
        m_options.max_file_bytes <= kFileBrowserMaxBytesCeiling &&
        m_options.max_path_bytes != 0 &&
        m_options.max_path_bytes <= kFileBrowserMaxPathBytes &&
        m_options.max_entries != 0 &&
        m_options.max_entries <= kFileBrowserMaxEntriesCeiling;
    if (root.empty() || !options_valid) {
        return;
    }
    std::error_code ec;
    const fs::path path(root);
    if (!fs::is_directory(path, ec) || ec) {
        return;
    }
    const fs::path canonical = fs::weakly_canonical(path, ec);
    if (ec) {
        return;
    }
    m_root = canonical.string();
    m_valid = !m_root.empty();
}

FileBrowserStatus FileBrowser::Resolve(std::string_view rel, bool allow_root,
                                       std::string* resolved) const {
    if (resolved == nullptr) {
        return FileBrowserStatus::kInvalidArgument;
    }
    resolved->clear();
    if (!m_valid) {
        return FileBrowserStatus::kPathRejected;
    }
    std::vector<std::string> segments;
    if (!SplitRelativePath(rel, m_options.max_path_bytes, &segments)) {
        return FileBrowserStatus::kPathRejected;
    }
    if (segments.empty()) {
        if (!allow_root) {
            return FileBrowserStatus::kPathRejected;
        }
        *resolved = m_root;
        return FileBrowserStatus::kOk;
    }

    fs::path candidate(m_root);
    for (const std::string& segment : segments) {
        candidate /= segment;
    }

    // 逐段检查已存在前缀中的符号链接；遇到不存在的段就停止检查，但
    // candidate 必须保留完整路径（否则 Put 会把目标截断到最后一个已存在
    // 的目录）。
    fs::path current(m_root);
    for (const std::string& segment : segments) {
        current /= segment;
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) {
                break;
            }
            return MapError(ec);
        }
        if (status.type() == fs::file_type::not_found) {
            break;
        }
        if (fs::is_symlink(status) && !m_options.follow_symlinks) {
            return FileBrowserStatus::kPathRejected;
        }
    }

    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(candidate, ec);
    if (ec) {
        return MapError(ec);
    }
    const fs::path relative = canonical.lexically_relative(m_root);
    if (relative.empty()) {
        return FileBrowserStatus::kPathRejected;
    }
    const auto first = relative.begin();
    if (first == relative.end() || first->string() == "..") {
        return FileBrowserStatus::kPathRejected;
    }
    *resolved = canonical.string();
    return FileBrowserStatus::kOk;
}

FileBrowserStatus FileBrowser::List(std::string_view rel,
                                    std::vector<FileEntry>* entries) const {
    if (entries == nullptr) {
        return FileBrowserStatus::kInvalidArgument;
    }
    entries->clear();
    std::string directory;
    const FileBrowserStatus resolved = Resolve(rel, true, &directory);
    if (resolved != FileBrowserStatus::kOk) {
        return resolved;
    }

    struct stat info {};
    if (::stat(directory.c_str(), &info) != 0) {
        return MapErrno(errno);
    }
    if (!S_ISDIR(info.st_mode)) {
        return FileBrowserStatus::kNotRegular;
    }

    std::error_code ec;
    fs::directory_iterator iterator(fs::path(directory), ec);
    if (ec) {
        return MapError(ec);
    }
    const fs::directory_iterator end;
    while (iterator != end) {
        const fs::path entry_path = iterator->path();
        const std::string name = entry_path.filename().string();
        if (!name.empty() && (m_options.include_hidden || name.front() != '.')) {
            if (entries->size() >= m_options.max_entries) {
                return FileBrowserStatus::kTooManyEntries;
            }
            struct stat entry_info {};
            if (::lstat(entry_path.c_str(), &entry_info) != 0) {
                if (errno == ENOENT) {
                    // 遍历期间被删除：跳过，不把竞态当成错误。
                } else {
                    return MapErrno(errno);
                }
            } else {
                FileEntry entry;
                entry.name = name;
                entry.size = static_cast<std::uint64_t>(entry_info.st_size);
                entry.mtime_ms = MtimeMs(entry_info);
                entry.type = TypeFromMode(entry_info.st_mode);
                entries->push_back(std::move(entry));
            }
        }
        iterator.increment(ec);
        if (ec) {
            return MapError(ec);
        }
    }
    std::sort(entries->begin(), entries->end(),
              [](const FileEntry& left, const FileEntry& right) {
                  return left.name < right.name;
              });
    return FileBrowserStatus::kOk;
}

FileBrowserStatus FileBrowser::Stat(std::string_view rel, FileEntry* entry) const {
    if (entry == nullptr) {
        return FileBrowserStatus::kInvalidArgument;
    }
    *entry = FileEntry{};
    if (!m_valid) {
        return FileBrowserStatus::kPathRejected;
    }
    std::vector<std::string> segments;
    if (!SplitRelativePath(rel, m_options.max_path_bytes, &segments)) {
        return FileBrowserStatus::kPathRejected;
    }
    if (segments.empty()) {
        struct stat info {};
        if (::lstat(m_root.c_str(), &info) != 0) {
            return MapErrno(errno);
        }
        entry->name = fs::path(m_root).filename().string();
        entry->size = static_cast<std::uint64_t>(info.st_size);
        entry->mtime_ms = MtimeMs(info);
        entry->type = TypeFromMode(info.st_mode);
        return FileBrowserStatus::kOk;
    }

    // 只检查最后一段之前的目录：Stat 需要能报告符号链接本身。
    fs::path parent(m_root);
    for (std::size_t index = 0; index + 1 < segments.size(); ++index) {
        parent /= segments[index];
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(parent, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) {
                return FileBrowserStatus::kNotFound;
            }
            return MapError(ec);
        }
        if (status.type() == fs::file_type::not_found) {
            return FileBrowserStatus::kNotFound;
        }
        if (fs::is_symlink(status) && !m_options.follow_symlinks) {
            return FileBrowserStatus::kPathRejected;
        }
    }

    std::error_code ec;
    const fs::path parent_canonical = fs::weakly_canonical(parent, ec);
    if (ec) {
        return MapError(ec);
    }
    const fs::path parent_relative = parent_canonical.lexically_relative(m_root);
    if (parent_relative.empty()) {
        return FileBrowserStatus::kPathRejected;
    }
    if (parent_relative.begin()->string() == "..") {
        return FileBrowserStatus::kPathRejected;
    }

    const fs::path final_path = parent_canonical / segments.back();
    struct stat info {};
    if (::lstat(final_path.c_str(), &info) != 0) {
        return MapErrno(errno);
    }
    if (m_options.follow_symlinks && S_ISLNK(info.st_mode)) {
        const fs::path target = fs::weakly_canonical(final_path, ec);
        if (ec) {
            return MapError(ec);
        }
        const fs::path target_relative = target.lexically_relative(m_root);
        if (target_relative.empty() || target_relative.begin()->string() == "..") {
            return FileBrowserStatus::kPathRejected;
        }
        if (::stat(final_path.c_str(), &info) != 0) {
            return MapErrno(errno);
        }
    }
    entry->name = segments.back();
    entry->size = static_cast<std::uint64_t>(info.st_size);
    entry->mtime_ms = MtimeMs(info);
    entry->type = TypeFromMode(info.st_mode);
    return FileBrowserStatus::kOk;
}

FileBrowserStatus FileBrowser::Get(std::string_view rel,
                                   std::string_view local_path) const {
    if (local_path.empty()) {
        return FileBrowserStatus::kInvalidArgument;
    }
    std::string source;
    const FileBrowserStatus resolved = Resolve(rel, false, &source);
    if (resolved != FileBrowserStatus::kOk) {
        return resolved;
    }

    struct stat source_info {};
    if (::stat(source.c_str(), &source_info) != 0) {
        return MapErrno(errno);
    }
    if (!S_ISREG(source_info.st_mode)) {
        return FileBrowserStatus::kNotRegular;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(source_info.st_size);
    if (size > m_options.max_file_bytes) {
        return FileBrowserStatus::kTooLarge;
    }

    const std::string local(local_path);
    struct stat local_info {};
    if (::lstat(local.c_str(), &local_info) == 0) {
        if (!m_options.overwrite) {
            return FileBrowserStatus::kAlreadyExists;
        }
        if (S_ISDIR(local_info.st_mode)) {
            return FileBrowserStatus::kAlreadyExists;
        }
    } else if (errno != ENOENT) {
        return MapErrno(errno);
    }

    return CopyAtomically(source, local, size, m_options.follow_symlinks, m_options.overwrite);
}

FileBrowserStatus FileBrowser::Put(std::string_view rel,
                                   std::string_view local_path) const {
    if (local_path.empty()) {
        return FileBrowserStatus::kInvalidArgument;
    }
    const std::string local(local_path);
    struct stat source_info {};
    if (::stat(local.c_str(), &source_info) != 0) {
        return MapErrno(errno);
    }
    if (!S_ISREG(source_info.st_mode)) {
        return FileBrowserStatus::kNotRegular;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(source_info.st_size);
    if (size > m_options.max_file_bytes) {
        return FileBrowserStatus::kTooLarge;
    }

    std::string target;
    const FileBrowserStatus resolved = Resolve(rel, false, &target);
    if (resolved != FileBrowserStatus::kOk) {
        return resolved;
    }
    struct stat target_info {};
    if (::lstat(target.c_str(), &target_info) == 0) {
        if (!m_options.overwrite) {
            return FileBrowserStatus::kAlreadyExists;
        }
        if (S_ISDIR(target_info.st_mode)) {
            return FileBrowserStatus::kAlreadyExists;
        }
    } else if (errno != ENOENT) {
        return MapErrno(errno);
    }

    const fs::path parent = fs::path(target).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        const bool exists = fs::exists(parent, ec);
        if (ec) {
            return MapError(ec);
        }
        if (!exists) {
            fs::create_directories(parent, ec);
            if (ec) {
                return MapError(ec);
            }
        } else if (!fs::is_directory(parent, ec) || ec) {
            return FileBrowserStatus::kPathRejected;
        }
    }

    // local 是调用方给出的路径，允许它是符号链接（stat 已跟随并确认常规文件）。
    return CopyAtomically(local, target, size, true, m_options.overwrite);
}

// ---------------------------------------------------------------------------
// 便捷自由函数
// ---------------------------------------------------------------------------

FileBrowserStatus List(const std::string& root, std::string_view rel,
                       std::vector<FileEntry>* entries, const FileBrowserOptions& options) {
    const FileBrowser browser(root, options);
    return browser.List(rel, entries);
}

FileBrowserStatus Get(const std::string& root, std::string_view rel,
                      const std::string& local_path, const FileBrowserOptions& options) {
    const FileBrowser browser(root, options);
    return browser.Get(rel, local_path);
}

FileBrowserStatus Put(const std::string& root, std::string_view rel,
                      const std::string& local_path, const FileBrowserOptions& options) {
    FileBrowser browser(root, options);
    return browser.Put(rel, local_path);
}

}  // namespace go2cpp::bento::app
