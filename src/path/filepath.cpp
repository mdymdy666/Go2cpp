/**
 * @file filepath.cpp
 * @brief `go2cpp::path::Filepath`（Go `path/filepath` 对应实现）。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（移植自 Go 标准库，详见 filepath.hpp 的来源说明）
 *
 * ## 实现结构
 *
 * - 纯词法函数：Linux 直接转发 `go2cpp::path`（Go 在 Unix 上正是这样分派的）；
 *   Windows 分支用 `#ifdef _WIN32` 提供盘符/UNC/反斜杠版本；
 * - 文件系统函数：`Abs`/`Rel`/`EvalSymlinks`/`Walk`/`Glob`。Linux 用
 *   `lstat`/`readlink`/`getcwd`/`opendir`/`readdir`，逐行对照 Go 1.27.1；
 *   Windows 分支用 `std::filesystem` 近似实现，**未在真机验证**（见交付报告）。
 */

#include "go2cpp/path/filepath.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <filesystem>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace go2cpp::path::Filepath {

namespace {

// ---------------------------------------------------------------------------
// 平台词法辅助
// ---------------------------------------------------------------------------

#ifdef _WIN32

bool IsWinSep(char c) noexcept { return c == '\\' || c == '/'; }

/// 返回 Windows 卷名长度（`C:` 为 2，UNC `\\host\share` 到 share 末尾）。
std::size_t WinVolumeLen(std::string_view p) {
    if (p.size() >= 2 && p[1] == ':') {
        const char c = p[0];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            return 2;
        }
    }
    if (p.size() >= 2 && IsWinSep(p[0]) && IsWinSep(p[1])) {
        std::size_t i = 2;
        const std::size_t host_start = i;
        while (i < p.size() && !IsWinSep(p[i])) {
            ++i;
        }
        if (i == host_start || i >= p.size()) {
            return 0;
        }
        ++i;  // 跳过 host 与 share 之间的分隔符
        const std::size_t share_start = i;
        while (i < p.size() && !IsWinSep(p[i])) {
            ++i;
        }
        if (i == share_start) {
            return 0;
        }
        return i;
    }
    return 0;
}

/// Windows 版 Clean：保留盘符/UNC，统一分隔符为 `\`，语义对齐 Go `filepath.Clean`。
std::string CleanWindows(std::string_view original) {
    const std::size_t vol_len = WinVolumeLen(original);
    const std::string volume(original.substr(0, vol_len));
    std::string_view path = original.substr(vol_len);
    if (path.empty()) {
        if (vol_len > 1 && IsWinSep(original[0]) && IsWinSep(original[1])) {
            // UNC 根（\\host\share）：只做斜杠转换。
            std::string out(original);
            for (char& c : out) {
                if (c == '/') {
                    c = '\\';
                }
            }
            return out;
        }
        return std::string(original) + ".";
    }

    const bool rooted = IsWinSep(path.front());
    std::string out;
    if (rooted) {
        out.push_back('\\');
    }
    std::size_t r = 0;
    std::size_t dotdot = 0;
    if (rooted) {
        r = 1;
        dotdot = 1;
    }
    const std::size_t n = path.size();
    while (r < n) {
        if (IsWinSep(path[r])) {
            ++r;
        } else if (path[r] == '.' && (r + 1 == n || IsWinSep(path[r + 1]))) {
            ++r;
        } else if (path[r] == '.' && r + 1 < n && path[r + 1] == '.' &&
                   (r + 2 == n || IsWinSep(path[r + 2]))) {
            r += 2;
            if (out.size() > dotdot) {
                out.pop_back();
                while (out.size() > dotdot && !IsWinSep(out.back())) {
                    out.pop_back();
                }
            } else if (!rooted) {
                if (!out.empty()) {
                    out.push_back('\\');
                }
                out += "..";
                dotdot = out.size();
            }
        } else {
            if ((rooted && out.size() != 1) || (!rooted && !out.empty())) {
                out.push_back('\\');
            }
            while (r < n && !IsWinSep(path[r])) {
                out.push_back(path[r]);
                ++r;
            }
        }
    }
    if (out.empty()) {
        out = ".";
    }
    std::string result = volume + out;
    for (char& c : result) {
        if (c == '/') {
            c = '\\';
        }
    }
    return result;
}

#endif  // _WIN32

/// 平台版「是否含通配符元字符」。POSIX 上 `\` 也是元字符（转义符）。
bool HasMeta(std::string_view p) {
#ifdef _WIN32
    return p.find_first_of("*?[") != std::string_view::npos;
#else
    return p.find_first_of("*?[\\") != std::string_view::npos;
#endif
}

#ifndef _WIN32

/// 读取目录下的名字并排序；返回 false 时 `error_code` 为 errno。
bool ReadDirNames(const std::string& dir, std::vector<std::string>* names,
                  int* error_code) {
    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) {
        *error_code = errno;
        return false;
    }
    names->clear();
    errno = 0;
    while (const struct ::dirent* entry = ::readdir(handle)) {
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
            continue;
        }
        names->emplace_back(name);
    }
    const int read_error = errno;
    ::closedir(handle);
    if (read_error != 0) {
        *error_code = read_error;
        return false;
    }
    std::sort(names->begin(), names->end());
    return true;
}

/// 动态扩容读符号链接（最多 8 次尝试，上限 32 KiB）。
bool ReadLinkAll(const std::string& path, std::string* out, int* error_code) {
    std::size_t size = 256;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::string buffer(size, '\0');
        const ssize_t n =
            ::readlink(path.c_str(), buffer.data(), buffer.size());
        if (n < 0) {
            *error_code = errno;
            return false;
        }
        if (static_cast<std::size_t>(n) < buffer.size()) {
            out->assign(buffer.data(), static_cast<std::size_t>(n));
            return true;
        }
        size *= 2;
    }
    *error_code = ENAMETOOLONG;
    return false;
}

WalkInfo FromStat(const struct ::stat& st) {
    WalkInfo info;
    info.is_dir = S_ISDIR(st.st_mode);
    info.is_regular = S_ISREG(st.st_mode);
    info.is_symlink = S_ISLNK(st.st_mode);
    info.size = static_cast<std::uintmax_t>(st.st_size);
    return info;
}

std::string SysError(const char* what, const std::string& path, int code) {
    std::string message(what);
    message += " \"";
    message += path;
    message += "\": ";
    message += std::strerror(code);
    return message;
}

bool GetCwd(std::string* out, std::string* error) {
    std::size_t size = 256;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::vector<char> buffer(size);
        if (::getcwd(buffer.data(), buffer.size()) != nullptr) {
            out->assign(buffer.data());
            return true;
        }
        if (errno != ERANGE) {
            if (error != nullptr) {
                *error = SysError("getcwd 失败", "", errno);
            }
            return false;
        }
        size *= 2;
    }
    if (error != nullptr) {
        *error = "getcwd 失败: 工作目录路径过长";
    }
    return false;
}

bool EvalSymlinksPosix(const std::string& input, std::string* out,
                       std::string* error) {
    std::string path = input;
    int links_walked = 0;

    // 与 Go walkSymlinks 一致：vol 是根前缀（绝对路径为 "/"，相对路径为空），
    // dest 从 vol 开始累积；没有这一步会把 "/tmp/x" 当成相对路径 "tmp/x"。
    std::size_t vol_len = 0;
    if (!path.empty() && path[0] == '/') {
        vol_len = 1;
    }
    const std::string vol = path.substr(0, vol_len);
    std::string dest = vol;

    // start/end 指向当前组件；循环末尾 start = end。
    for (std::size_t start = vol_len, end = vol_len; start < path.size();
         start = end) {
        while (start < path.size() && path[start] == '/') {
            ++start;
        }
        end = start;
        while (end < path.size() && path[end] != '/') {
            ++end;
        }
        if (end == start) {
            break;
        }
        const std::string component = path.substr(start, end - start);
        if (component == ".") {
            continue;
        }
        if (component == "..") {
            // 只在卷前缀之后找最后一个 `/`（与 Go 的 r >= volLen 一致）。
            std::size_t slash = std::string::npos;
            for (std::size_t i = dest.size(); i > vol_len; --i) {
                if (dest[i - 1] == '/') {
                    slash = i - 1;
                    break;
                }
            }
            const bool ends_with_dotdot =
                (slash == std::string::npos) ? (dest == "..")
                                            : (dest.substr(slash + 1) == "..");
            if (slash == std::string::npos || ends_with_dotdot) {
                // 无斜杠可回退，或上一个组件本身是保留下来的 `..`：保留。
                // 根路径的 "/.." 先留下，最后统一 Clean 成 "/"（Go 也如此）。
                if (dest.size() > vol_len) {
                    dest.push_back('/');
                }
                dest += "..";
            } else {
                dest.resize(slash);
            }
            continue;
        }

        // 普通组件：先拼进 dest 再看它是不是链接。
        if (!dest.empty() && dest.back() != '/') {
            dest.push_back('/');
        }
        dest += component;

        struct ::stat st {};
        if (::lstat(dest.c_str(), &st) != 0) {
            if (error != nullptr) {
                *error = SysError("EvalSymlinks: lstat 失败", dest, errno);
            }
            return false;
        }
        if (!S_ISLNK(st.st_mode)) {
            if (!S_ISDIR(st.st_mode) && end < path.size()) {
                if (error != nullptr) {
                    *error = SysError("EvalSymlinks: 中间组件不是目录", dest,
                                      ENOTDIR);
                }
                return false;
            }
            continue;
        }

        ++links_walked;
        if (links_walked > 255) {
            if (error != nullptr) {
                *error = "EvalSymlinks: 符号链接层数超过 255";
            }
            return false;
        }
        std::string link;
        int link_error = 0;
        if (!ReadLinkAll(dest, &link, &link_error)) {
            if (error != nullptr) {
                *error = SysError("EvalSymlinks: readlink 失败", dest,
                                  link_error);
            }
            return false;
        }

        path = link + path.substr(end);
        if (!link.empty() && link[0] == '/') {
            // 绝对链接：重置到根。
            dest = "/";
            end = 1;
        } else {
            // 相对链接：替换当前组件，并退回所属目录（可能是根）。
            std::size_t slash = std::string::npos;
            for (std::size_t i = dest.size(); i > vol_len; --i) {
                if (dest[i - 1] == '/') {
                    slash = i - 1;
                    break;
                }
            }
            if (slash == std::string::npos) {
                dest = vol;
            } else {
                dest.resize(slash);
            }
            end = 0;
        }
    }

    *out = Clean(dest);
    return true;
}

int WalkRecursive(const std::string& path, const WalkInfo& info,
                  const WalkFn& fn) {
    if (!info.is_dir) {
        return static_cast<int>(fn(path, info, 0));
    }

    std::vector<std::string> names;
    int read_error = 0;
    const bool read_ok = ReadDirNames(path, &names, &read_error);

    // Go 先读目录再回调父目录：这样目录读取失败也能带着 errno 报告。
    const int status = static_cast<int>(fn(path, info, read_ok ? 0 : read_error));
    if (!read_ok || status != 0) {
        return status;
    }

    for (const std::string& name : names) {
        const std::string filename = Join(path, name);
        struct ::stat st {};
        if (::lstat(filename.c_str(), &st) != 0) {
            const int entry_error = errno;
            const int entry_status =
                static_cast<int>(fn(filename, WalkInfo{}, entry_error));
            if (entry_status != 0 &&
                entry_status != static_cast<int>(WalkAction::SkipDir)) {
                return entry_status;
            }
            continue;
        }
        const WalkInfo child = FromStat(st);
        const int child_status = WalkRecursive(filename, child, fn);
        if (child_status != 0) {
            if (!child.is_dir ||
                child_status != static_cast<int>(WalkAction::SkipDir)) {
                return child_status;
            }
        }
    }
    return 0;
}

// ------------------------------- Glob --------------------------------------

std::string CleanGlobPath(std::string_view path) {
    if (path.empty()) {
        return ".";
    }
    if (path == "/") {
        return "/";
    }
    // Split 保证 dir 以分隔符结尾，这里砍掉它。
    return std::string(path.substr(0, path.size() - 1));
}

bool GlobDir(const std::string& dir, std::string_view pattern,
             std::vector<std::string>* matches, std::string* error) {
    struct ::stat st {};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return true;  // 忽略文件系统错误
    }
    std::vector<std::string> names;
    int read_error = 0;
    if (!ReadDirNames(dir, &names, &read_error)) {
        return true;  // 忽略文件系统错误
    }
    for (const std::string& name : names) {
        const MatchResult result = MatchChecked(pattern, name);
        if (result.bad_pattern) {
            if (error != nullptr) {
                *error = "Glob 失败: 模式非法 \"" + std::string(pattern) + "\"";
            }
            return false;
        }
        if (result.matched) {
            matches->push_back(Join(dir, name));
        }
    }
    return true;
}

bool GlobWithLimit(std::string_view pattern, int depth,
                   std::vector<std::string>* matches, std::string* error) {
    // 与 Go 相同的 10000 层上限（CVE-2022-30632 防护）。
    constexpr int kSeparatorLimit = 10000;
    if (depth == kSeparatorLimit) {
        if (error != nullptr) {
            *error = "Glob 失败: 模式递归层数超过上限（模式非法）";
        }
        return false;
    }

    const MatchResult sanity = MatchChecked(pattern, std::string_view{});
    if (sanity.bad_pattern) {
        if (error != nullptr) {
            *error = "Glob 失败: 模式非法 \"" + std::string(pattern) + "\"";
        }
        return false;
    }

    if (!HasMeta(pattern)) {
        struct ::stat st {};
        if (::lstat(std::string(pattern).c_str(), &st) != 0) {
            return true;  // 无匹配
        }
        matches->emplace_back(pattern);
        return true;
    }

    auto [dir, file] = Split(pattern);
    dir = CleanGlobPath(dir);
    if (!HasMeta(dir)) {
        return GlobDir(dir, file, matches, error);
    }
    if (std::string_view(dir) == pattern) {
        if (error != nullptr) {
            *error = "Glob 失败: 模式非法（目录部分未收敛）";
        }
        return false;
    }

    std::vector<std::string> dirs;
    if (!GlobWithLimit(dir, depth + 1, &dirs, error)) {
        return false;
    }
    for (const std::string& matched_dir : dirs) {
        if (!GlobDir(matched_dir, file, matches, error)) {
            return false;
        }
    }
    return true;
}

#endif  // !_WIN32

#ifdef _WIN32

// Windows 文件系统部分：用 std::filesystem 近似实现，未在真机验证。
int WalkRecursiveWin(const std::string& path, const WalkInfo& info,
                     const WalkFn& fn) {
    namespace fs = std::filesystem;
    if (!info.is_dir) {
        return static_cast<int>(fn(path, info, 0));
    }
    std::vector<std::string> names;
    int read_error = 0;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(path), ec), end; !ec && it != end;
         it.increment(ec)) {
        names.push_back(it->path().filename().u8string());
    }
    if (ec) {
        read_error = ec.value();
    } else {
        std::sort(names.begin(), names.end());
    }
    const int status = static_cast<int>(fn(path, info, read_error));
    if (read_error != 0 || status != 0) {
        return status;
    }
    for (const std::string& name : names) {
        const std::string filename = Join(path, name);
        std::error_code entry_ec;
        const fs::file_status st =
            fs::symlink_status(fs::u8path(filename), entry_ec);
        if (entry_ec) {
            const int entry_status =
                static_cast<int>(fn(filename, WalkInfo{}, entry_ec.value()));
            if (entry_status != 0 &&
                entry_status != static_cast<int>(WalkAction::SkipDir)) {
                return entry_status;
            }
            continue;
        }
        WalkInfo child;
        child.is_dir = fs::is_directory(st);
        child.is_regular = fs::is_regular_file(st);
        child.is_symlink = fs::is_symlink(st);
        std::error_code size_ec;
        const auto size = fs::file_size(fs::u8path(filename), size_ec);
        if (!size_ec) {
            child.size = static_cast<std::uintmax_t>(size);
        }
        const int child_status = WalkRecursiveWin(filename, child, fn);
        if (child_status != 0) {
            if (!child.is_dir ||
                child_status != static_cast<int>(WalkAction::SkipDir)) {
                return child_status;
            }
        }
    }
    return 0;
}

#endif  // _WIN32

}  // namespace

// ---------------------------------------------------------------------------
// 纯词法 API
// ---------------------------------------------------------------------------

std::string Clean(std::string_view path) {
#ifdef _WIN32
    return CleanWindows(path);
#else
    return go2cpp::path::Clean(path);
#endif
}

std::pair<std::string, std::string> Split(std::string_view path) {
#ifdef _WIN32
    std::size_t i = path.size();
    while (i > 0) {
        --i;
        if (path[i] == '\\' || path[i] == '/') {
            return {std::string(path.substr(0, i + 1)),
                    std::string(path.substr(i + 1))};
        }
    }
    return {std::string{}, std::string(path)};
#else
    return go2cpp::path::Split(path);
#endif
}

std::string Ext(std::string_view path) {
#ifdef _WIN32
    for (std::size_t i = path.size(); i > 0; --i) {
        const char c = path[i - 1];
        if (c == '\\' || c == '/') {
            break;
        }
        if (c == '.') {
            return std::string(path.substr(i - 1));
        }
    }
    return {};
#else
    return go2cpp::path::Ext(path);
#endif
}

std::string Base(std::string_view path) {
#ifdef _WIN32
    if (path.empty()) {
        return ".";
    }
    while (!path.empty() && (path.back() == '\\' || path.back() == '/')) {
        path.remove_suffix(1);
    }
    const std::size_t i = path.find_last_of("\\/");
    if (i != std::string_view::npos) {
        path.remove_prefix(i + 1);
    }
    if (path.empty()) {
        return "\\";
    }
    return std::string(path);
#else
    return go2cpp::path::Base(path);
#endif
}

std::string Dir(std::string_view path) {
#ifdef _WIN32
    return Clean(Split(path).first);
#else
    return go2cpp::path::Dir(path);
#endif
}

bool IsAbs(std::string_view path) {
#ifdef _WIN32
    const std::size_t volume = WinVolumeLen(path);
    if (volume == 0) {
        return false;
    }
    path.remove_prefix(volume);
    if (path.empty()) {
        return false;
    }
    return IsWinSep(path.front());
#else
    return go2cpp::path::IsAbs(path);
#endif
}

std::string JoinElements(std::initializer_list<std::string_view> elems) {
#ifdef _WIN32
    auto it = elems.begin();
    while (it != elems.end() && it->empty()) {
        ++it;
    }
    if (it == elems.end()) {
        return {};
    }
    std::string buffer;
    bool first_element = true;
    for (; it != elems.end(); ++it) {
        if (!first_element) {
            buffer.push_back('\\');
        }
        buffer.append(it->data(), it->size());
        first_element = false;
    }
    return Clean(buffer);
#else
    return go2cpp::path::JoinElements(elems);
#endif
}

std::string JoinElements(const std::vector<std::string>& elems) {
#ifdef _WIN32
    std::size_t first = elems.size();
    for (std::size_t i = 0; i < elems.size(); ++i) {
        if (!elems[i].empty()) {
            first = i;
            break;
        }
    }
    if (first == elems.size()) {
        return {};
    }
    std::string buffer = elems[first];
    for (std::size_t i = first + 1; i < elems.size(); ++i) {
        buffer.push_back('\\');
        buffer += elems[i];
    }
    return Clean(buffer);
#else
    return go2cpp::path::JoinElements(elems);
#endif
}

std::string Join(const std::vector<std::string>& elems) {
    return JoinElements(elems);
}

MatchResult MatchChecked(std::string_view pattern, std::string_view name) {
#ifdef _WIN32
    return detail::MatchWithSeparator(pattern, name, '\\', false);
#else
    return go2cpp::path::MatchChecked(pattern, name);
#endif
}

bool Match(std::string_view pattern, std::string_view name) {
    return MatchChecked(pattern, name).matched;
}

// ---------------------------------------------------------------------------
// 文件系统 API
// ---------------------------------------------------------------------------

bool Abs(std::string_view path, std::string* out, std::string* error) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "Abs 失败: out 为空指针";
        }
        return false;
    }
    out->clear();
    if (IsAbs(path)) {
        *out = Clean(path);
        return true;
    }
#ifdef _WIN32
    std::vector<char> buffer(4096);
    if (::_getcwd(buffer.data(), static_cast<int>(buffer.size())) == nullptr) {
        if (error != nullptr) {
            *error = "Abs 失败: _getcwd 调用失败";
        }
        return false;
    }
    *out = Join(std::string_view(buffer.data()), path);
    return true;
#else
    std::string cwd;
    if (!GetCwd(&cwd, error)) {
        return false;
    }
    *out = Join(cwd, path);
    return true;
#endif
}

namespace {

#ifdef _WIN32

std::string VolumeName(std::string_view path) {
    return std::string(path.substr(0, WinVolumeLen(path)));
}

#else

std::string VolumeName(std::string_view /*path*/) { return {}; }

#endif

}  // namespace

bool Rel(std::string_view base_path, std::string_view target_path,
         std::string* out, std::string* error) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "Rel 失败: out 为空指针";
        }
        return false;
    }
    out->clear();

    const std::string base_volume = VolumeName(base_path);
    const std::string target_volume = VolumeName(target_path);
    std::string base = Clean(base_path);
    std::string target = Clean(target_path);

    if (base == target && base_volume == target_volume) {
        *out = ".";
        return true;
    }
    base.erase(0, base_volume.size());
    target.erase(0, target_volume.size());
    if (base == ".") {
        base.clear();
    }

    const bool base_slashed = !base.empty() && base.front() == Separator;
    const bool target_slashed =
        !target.empty() && target.front() == Separator;
    if (base_slashed != target_slashed || base_volume != target_volume) {
        if (error != nullptr) {
            *error = "Rel 失败: \"" + std::string(target_path) +
                     "\" 无法相对 \"" + std::string(base_path) + "\" 表示";
        }
        return false;
    }

    // 把 base/target 定位到第一处不同组件的边界。
    const std::size_t bl = base.size();
    const std::size_t tl = target.size();
    std::size_t b0 = 0;
    std::size_t bi = 0;
    std::size_t t0 = 0;
    std::size_t ti = 0;
    for (;;) {
        while (bi < bl && base[bi] != Separator) {
            ++bi;
        }
        while (ti < tl && target[ti] != Separator) {
            ++ti;
        }
        if (base.compare(b0, bi - b0, target, t0, ti - t0) != 0) {
            break;
        }
        if (bi >= bl && ti >= tl) {
            // 理论上 base != target 时不会同时走到末尾；防御性退出避免死循环。
            break;
        }
        if (bi < bl) {
            ++bi;
        }
        if (ti < tl) {
            ++ti;
        }
        b0 = bi;
        t0 = ti;
    }
    if (base.compare(b0, bi - b0, "..") == 0) {
        if (error != nullptr) {
            *error = "Rel 失败: \"" + std::string(base_path) +
                     "\" 含无法回退的前导 `..`";
        }
        return false;
    }
    if (b0 != bl) {
        // base 还有剩余组件：先逐级 `..`，再拼 target 的剩余部分。
        std::string buffer = "..";
        for (std::size_t i = b0; i < bl; ++i) {
            if (base[i] == Separator) {
                buffer += Separator;
                buffer += "..";
            }
        }
        if (t0 != tl) {
            buffer.push_back(Separator);
            buffer.append(target, t0, std::string::npos);
        }
        *out = Clean(buffer);
        return true;
    }
    *out = target.substr(t0);
    return true;
}

bool EvalSymlinks(std::string_view path, std::string* out,
                  std::string* error) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "EvalSymlinks 失败: out 为空指针";
        }
        return false;
    }
    out->clear();
#ifdef _WIN32
    // Windows：用 std::filesystem 的弱规范化近似（未在真机验证）。
    std::error_code ec;
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(
            std::filesystem::u8path(std::string(path)), ec);
    if (ec) {
        if (error != nullptr) {
            *error = std::string("EvalSymlinks 失败: ") + ec.message();
        }
        return false;
    }
    *out = Clean(resolved.u8string());
    return true;
#else
    return EvalSymlinksPosix(std::string(path), out, error);
#endif
}

bool Walk(std::string_view root, const WalkFn& fn, std::string* error) {
    if (!fn) {
        if (error != nullptr) {
            *error = "Walk 失败: 回调为空";
        }
        return false;
    }
    const std::string root_path(root);
#ifdef _WIN32
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(fs::u8path(root_path), ec);
    int status = 0;
    if (ec) {
        status = static_cast<int>(fn(root_path, WalkInfo{}, ec.value()));
    } else {
        WalkInfo info;
        info.is_dir = fs::is_directory(st);
        info.is_regular = fs::is_regular_file(st);
        info.is_symlink = fs::is_symlink(st);
        std::error_code size_ec;
        const auto size = fs::file_size(fs::u8path(root_path), size_ec);
        if (!size_ec) {
            info.size = static_cast<std::uintmax_t>(size);
        }
        status = WalkRecursiveWin(root_path, info, fn);
    }
#else
    struct ::stat st {};
    int lstat_error = 0;
    if (::lstat(root_path.c_str(), &st) != 0) {
        lstat_error = errno;
    }
    const int status =
        (lstat_error != 0)
            ? static_cast<int>(fn(root_path, WalkInfo{}, lstat_error))
            : WalkRecursive(root_path, FromStat(st), fn);
#endif
    if (status == static_cast<int>(WalkAction::SkipDir) ||
        status == static_cast<int>(WalkAction::SkipAll)) {
        return true;
    }
    if (status != 0) {
        if (error != nullptr) {
            *error = "Walk 失败: 回调返回了未知动作";
        }
        return false;
    }
    return true;
}

bool Glob(std::string_view pattern, std::vector<std::string>* matches,
          std::string* error) {
    if (matches == nullptr) {
        if (error != nullptr) {
            *error = "Glob 失败: matches 为空指针";
        }
        return false;
    }
    matches->clear();
#ifdef _WIN32
    // Windows：先确定不含元字符的目录前缀，再递归枚举并用 Match 过滤。
    const MatchResult sanity = MatchChecked(pattern, std::string_view{});
    if (sanity.bad_pattern) {
        if (error != nullptr) {
            *error = "Glob 失败: 模式非法 \"" + std::string(pattern) + "\"";
        }
        return false;
    }
    if (!HasMeta(pattern)) {
        std::error_code ec;
        if (std::filesystem::symlink_status(
                std::filesystem::u8path(std::string(pattern)), ec)
                .type() != std::filesystem::file_type::not_found) {
            matches->emplace_back(pattern);
        }
        return true;
    }
    // 取第一个元字符之前的最后一个分隔符作为枚举根。
    const std::size_t meta = pattern.find_first_of("*?[");
    const std::size_t sep = pattern.rfind(Separator, meta);
    std::string base =
        sep == std::string_view::npos ? "." : std::string(pattern.substr(0, sep));
    if (base.empty()) {
        base = "/";
    }
    std::vector<std::string> candidates;
    Walk(base, [&candidates](const std::string& p, const WalkInfo&,
                             int error_code) {
        if (error_code == 0) {
            candidates.push_back(p);
        }
        return WalkAction::Continue;
    });
    std::sort(candidates.begin(), candidates.end());
    for (const std::string& candidate : candidates) {
        if (MatchChecked(pattern, candidate).matched) {
            matches->push_back(candidate);
        }
    }
    return true;
#else
    return GlobWithLimit(pattern, 0, matches, error);
#endif
}

}  // namespace go2cpp::path::Filepath
