/**
 * @file file.cpp
 * @brief `os::File` 与 `os` 模块便利函数实现。
 *
 * 所有文件 IO 走 **`syscall(SYS_*)` 而不是 libc 包装** —— 理由见 file.hpp 顶部
 * 的边界结论 1.2/1.3：绕过 Hook 的符号拦截，避免为普通文件 IO 白构造
 * `BlockingRegion`。代价是**自己处理 `EINTR` 与部分写**（hook 原本替我们做了）。
 */
#include "go2cpp/os/file.hpp"
#include "go2cpp/os/os.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

extern char** environ;

namespace go2cpp::os {

namespace {

/// 绕过 Hook 的 `openat`。失败返回 -1 且 errno 有效。
int RawOpen(const char* path, int flags, mode_t mode) noexcept {
    // O_NONBLOCK 的作用：**避免 open(2) 自己阻塞**。
    // 关键场景：`Open("/tmp/some_fifo")` 若不加 O_NONBLOCK，`open` 会一直等到
    // 有写端才返回 —— 根本到不了后面的 fstat 类型校验，worker 会卡在 open 里。
    // 对普通文件 O_NONBLOCK 是 no-op（POSIX 保证），所以加了无副作用。
    const long result = ::syscall(SYS_openat, AT_FDCWD, path,
                                  flags | O_CLOEXEC | O_NONBLOCK, mode);
    return static_cast<int>(result);
}

/// 绕过 Hook 的 `close`。
int RawClose(int fd) noexcept {
    return static_cast<int>(::syscall(SYS_close, fd));
}

/**
 * @brief 把内核返回的长整型收窄成 ssize_t/off_t，并把 `errno` 暴露给调用方。
 * @details `syscall()` 成功时也可能返回值恰好是 -1（极端偏移），所以只在
 *          返回值落在 [-4095, -1] 区间时才认为是错误 —— 这是内核错误码的惯例。
 */
bool IsSyscallError(long result) noexcept { return result < 0 && result >= -4095; }

}  // namespace

const char* ToString(OsError error) noexcept {
    switch (error) {
        case OsError::kOk: return "ok";
        case OsError::kOpenFailed: return "open-failed";
        case OsError::kIoFailed: return "io-failed";
        case OsError::kIsSocket: return "is-socket";
        case OsError::kNotRegularFile: return "not-a-regular-file";
        case OsError::kInvalidHandle: return "invalid-handle";
        case OsError::kInvalidArgument: return "invalid-argument";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// File 生命周期
// ---------------------------------------------------------------------------

File::~File() noexcept { Close(); }

File::File(File&& other) noexcept
    : m_fd(other.m_fd), m_owned(other.m_owned), m_last_error(other.m_last_error) {
    other.m_fd = -1;
    other.m_owned = false;
    other.m_last_error = 0;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        Close();
        m_fd = other.m_fd;
        m_owned = other.m_owned;
        m_last_error = other.m_last_error;
        other.m_fd = -1;
        other.m_owned = false;
        other.m_last_error = 0;
    }
    return *this;
}

bool File::Sync() noexcept {
    if (!valid()) {
        m_last_error = EBADF;
        return false;
    }
    const long result = ::syscall(SYS_fsync, m_fd);
    if (IsSyscallError(result)) {
        m_last_error = errno;
        return false;
    }
    m_last_error = 0;
    return true;
}

FileStatus File::Truncate(off_t size) noexcept {
    FileStatus status;
    if (!valid()) {
        status.code = OsError::kInvalidHandle;
        status.system_error = EBADF;
        m_last_error = EBADF;
        return status;
    }
    if (size < 0) {
        // 负大小在 ftruncate 里会返回 EINVAL，但先自己判掉能给出更准的错因。
        status.code = OsError::kInvalidArgument;
        status.system_error = EINVAL;
        m_last_error = EINVAL;
        return status;
    }
    const long result = ::syscall(SYS_ftruncate, m_fd, size);
    if (IsSyscallError(result)) {
        status.code = OsError::kIoFailed;
        status.system_error = errno;
        m_last_error = errno;
        return status;
    }
    m_last_error = 0;
    return status;
}

void File::Close() noexcept {
    if (m_fd >= 0) {
        if (m_owned) {
            // 忽略 close 的错误：析构路径不该抛，且 EINTR 下的 close 语义
            // 在 Linux 上属于「已关闭但不确定」，重试反而危险。
            (void)RawClose(m_fd);
        }
        m_fd = -1;
        m_owned = false;
    }
}

// ---------------------------------------------------------------------------
// 打开 / 接管
// ---------------------------------------------------------------------------

FileStatus File::Open(const std::string& path, OpenFlag flags, mode_t mode, File* out) {
    FileStatus status;
    if (out == nullptr) {
        status.code = OsError::kInvalidArgument;
        return status;
    }
    if (path.empty()) {
        status.code = OsError::kInvalidArgument;
        status.system_error = EINVAL;
        return status;
    }
    const int fd = RawOpen(path.c_str(), static_cast<int>(flags), mode);
    if (fd < 0) {
        status.code = OsError::kOpenFailed;
        status.system_error = errno;
        return status;
    }
    // 类型校验：只接受**普通文件**。理由见 OsError::kNotRegularFile 的文档 ——
    // 非普通文件（FIFO/pipe/字符设备/eventfd/timerfd/signalfd）的 read 会真阻塞，
    // 而本类走裸 syscall 绕过 Hook，会卡死整个 worker。
    // 先判 socket 是为了给出更精确的错误码。
    struct ::stat info {};
    if (static_cast<int>(::syscall(SYS_fstat, fd, &info)) == 0) {
        if (S_ISSOCK(info.st_mode)) {
            (void)RawClose(fd);
            status.code = OsError::kIsSocket;
            return status;
        }
        if (!S_ISREG(info.st_mode)) {
            (void)RawClose(fd);
            status.code = OsError::kNotRegularFile;
            return status;
        }
    }
    *out = File{};
    out->m_fd = fd;
    out->m_owned = true;
    return status;
}

FileStatus File::Create(const std::string& path, mode_t mode, File* out) {
    // **必须是 O_RDWR 而不是 O_WRONLY** —— 对齐 Go 的 `os.Create`。
    // 用 O_WRONLY 的话，创建后立刻 ReadAt/Read 会 EBADF，而调用方通常期望
    // "创建后既能写也能读回校验"（这也是 Go 的语义）。
    // 这个 bug 是被 `test_os_file.cpp` 的并发 pread 与随机 ReadAt 两个场景
    // 同时抓出来的 —— 单看写路径完全正常，只有读回时才暴露。
    const auto flags = OpenFlag::kReadWrite | OpenFlag::kCreate | OpenFlag::kTruncate;
    return Open(path, flags, mode, out);
}

FileStatus File::Adopt(int fd, bool take_ownership, File* out) {
    FileStatus status;
    if (out == nullptr) {
        status.code = OsError::kInvalidArgument;
        return status;
    }
    if (fd < 0) {
        status.code = OsError::kInvalidHandle;
        status.system_error = EBADF;
        return status;
    }
    struct ::stat info {};
    const long fstat_result = ::syscall(SYS_fstat, fd, &info);
    if (IsSyscallError(fstat_result)) {
        status.code = OsError::kInvalidHandle;
        status.system_error = errno;
        return status;
    }
    // ★ 高危误用拦截：`File::Read/Write` 走裸 syscall 绕过 Hook，前提是
    //   "普通文件的 IO 在内核里同步完成、不会 EAGAIN"。凡是**可能阻塞**的 fd
    //   都不满足这个前提，塞进来就会把整个 M 线程卡死。见 file.hpp 1.4。
    //
    //   先判 socket（错误码更精确），再判非普通文件（覆盖 FIFO/pipe/字符设备/
    //   eventfd/timerfd/signalfd —— 后三者的类型位是 0，黑名单必然漏掉）。
    if (S_ISSOCK(info.st_mode)) {
        status.code = OsError::kIsSocket;
        return status;
    }
    if (!S_ISREG(info.st_mode)) {
        status.code = OsError::kNotRegularFile;
        return status;
    }
    *out = File{};
    out->m_fd = fd;
    out->m_owned = take_ownership;
    return status;
}

// ---------------------------------------------------------------------------
// 读写（绕过 Hook，自己处理 EINTR / 部分写）
// ---------------------------------------------------------------------------

ssize_t File::Read(void* buffer, std::size_t length) noexcept {
    if (m_fd < 0) {
        m_last_error = EBADF;
        return -1;
    }
    if (buffer == nullptr && length > 0) {
        m_last_error = EINVAL;
        return -1;
    }
    for (;;) {
        const long result = ::syscall(SYS_read, m_fd, buffer, length);
        if (!IsSyscallError(result)) {
            m_last_error = 0;
            return static_cast<ssize_t>(result);
        }
        if (errno == EINTR) {
            continue;   // 绕过 hook 后必须自己重试
        }
        m_last_error = errno;
        return -1;
    }
}

ssize_t File::ReadAt(void* buffer, std::size_t length, off_t offset) noexcept {
    if (m_fd < 0) {
        m_last_error = EBADF;
        return -1;
    }
    if (buffer == nullptr && length > 0) {
        m_last_error = EINVAL;
        return -1;
    }
    for (;;) {
#ifdef SYS_pread64
        const long result = ::syscall(SYS_pread64, m_fd, buffer, length, offset);
#else
        const long result = ::syscall(SYS_pread, m_fd, buffer, length, offset);
#endif
        if (!IsSyscallError(result)) {
            m_last_error = 0;
            return static_cast<ssize_t>(result);
        }
        if (errno == EINTR) {
            continue;
        }
        m_last_error = errno;
        return -1;
    }
}

ssize_t File::Write(const void* buffer, std::size_t length) noexcept {
    if (m_fd < 0) {
        m_last_error = EBADF;
        return -1;
    }
    if (buffer == nullptr && length > 0) {
        m_last_error = EINVAL;
        return -1;
    }
    for (;;) {
        const long result = ::syscall(SYS_write, m_fd, buffer, length);
        if (!IsSyscallError(result)) {
            m_last_error = 0;
            return static_cast<ssize_t>(result);
        }
        if (errno == EINTR) {
            continue;
        }
        m_last_error = errno;
        return -1;
    }
}

ssize_t File::WriteAt(const void* buffer, std::size_t length, off_t offset) noexcept {
    if (m_fd < 0) {
        m_last_error = EBADF;
        return -1;
    }
    if (buffer == nullptr && length > 0) {
        m_last_error = EINVAL;
        return -1;
    }
    for (;;) {
#ifdef SYS_pwrite64
        const long result = ::syscall(SYS_pwrite64, m_fd, buffer, length, offset);
#else
        const long result = ::syscall(SYS_pwrite, m_fd, buffer, length, offset);
#endif
        if (!IsSyscallError(result)) {
            m_last_error = 0;
            return static_cast<ssize_t>(result);
        }
        if (errno == EINTR) {
            continue;
        }
        m_last_error = errno;
        return -1;
    }
}

std::size_t File::WriteAll(const void* buffer, std::size_t length) noexcept {
    if (buffer == nullptr && length > 0) {
        m_last_error = EINVAL;
        return 0;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    std::size_t total = 0;
    while (total < length) {
        const ssize_t written = Write(bytes + total, length - total);
        if (written <= 0) {
            break;   // 0 在写路径上意味着「写不动了」，按失败处理
        }
        total += static_cast<std::size_t>(written);
    }
    return total;
}

// ---------------------------------------------------------------------------
// 定位 / 元信息
// ---------------------------------------------------------------------------

off_t File::Seek(off_t offset, int whence) noexcept {
    if (m_fd < 0) {
        m_last_error = EBADF;
        return -1;
    }
    for (;;) {
        const long result = ::syscall(SYS_lseek, m_fd, offset, whence);
        if (!IsSyscallError(result)) {
            m_last_error = 0;
            return static_cast<off_t>(result);
        }
        if (errno == EINTR) {
            continue;
        }
        m_last_error = errno;
        return -1;
    }
}

off_t File::Tell() noexcept { return Seek(0, SEEK_CUR); }

bool File::Stat(struct ::stat* out) const noexcept {
    if (m_fd < 0) {
        const_cast<File*>(this)->m_last_error = EBADF;
        return false;
    }
    if (out == nullptr) {
        const_cast<File*>(this)->m_last_error = EINVAL;
        return false;
    }
    const long result = ::syscall(SYS_fstat, m_fd, out);
    if (IsSyscallError(result)) {
        const_cast<File*>(this)->m_last_error = errno;
        return false;
    }
    const_cast<File*>(this)->m_last_error = 0;
    return true;
}

off_t File::Size() const noexcept {
    struct ::stat info {};
    return Stat(&info) ? static_cast<off_t>(info.st_size) : static_cast<off_t>(-1);
}

bool File::IsRegular() const noexcept {
    struct ::stat info {};
    return Stat(&info) && S_ISREG(info.st_mode);
}

// ---------------------------------------------------------------------------
// 便利函数
// ---------------------------------------------------------------------------

FileStatus ReadFile(const std::string& path, std::string* out, std::string* error) {
    FileStatus status;
    if (out == nullptr) {
        status.code = OsError::kInvalidArgument;
        return status;
    }
    out->clear();
    File file;
    status = File::Open(path, OpenFlag::kReadOnly, 0, &file);
    if (!status.ok()) {
        if (error != nullptr) {
            *error = "打开失败: " + path + " (errno=" + std::to_string(status.system_error) + ")";
        }
        return status;
    }
    // 用 st_size 做容量提示（不当作精确长度：文件可能在被读期间变化）。
    const off_t size_hint = file.Size();
    // 大小上限（来自配置 os.max_file_size，0 表示不限制）。
    const std::size_t limit = MaxFileSize();
    if (limit > 0 && size_hint > 0 && static_cast<std::size_t>(size_hint) > limit) {
        status.code = OsError::kIoFailed;
        status.system_error = EFBIG;
        if (error != nullptr) {
            *error = "文件超过 os.max_file_size 上限: " + path + " (" +
                     std::to_string(size_hint) + " > " + std::to_string(limit) + ")";
        }
        return status;
    }
    if (size_hint > 0) {
        out->reserve(static_cast<std::size_t>(size_hint));
    }
    // 缓冲区大小来自配置（os.default_read_buffer_size，带钳制）。
    std::vector<char> buffer(ReadBufferSize());
    for (;;) {
        const ssize_t got = file.Read(buffer.data(), buffer.size());
        if (got < 0) {
            status.code = OsError::kIoFailed;
            status.system_error = file.last_error();
            out->clear();
            if (error != nullptr) {
                *error = "读失败: " + path + " (errno=" + std::to_string(status.system_error) + ")";
            }
            return status;
        }
        if (got == 0) {
            break;
        }
        out->append(buffer.data(), static_cast<std::size_t>(got));
    }
    return status;
}

FileStatus WriteFile(const std::string& path, std::string_view data, std::string* error) {
    FileStatus status;
    File file;
    status = File::Create(path, 0644, &file);
    if (!status.ok()) {
        if (error != nullptr) {
            *error = "创建失败: " + path + " (errno=" + std::to_string(status.system_error) + ")";
        }
        return status;
    }
    const std::size_t written = file.WriteAll(data.data(), data.size());
    if (written != data.size()) {
        status.code = OsError::kIoFailed;
        status.system_error = file.last_error();
        if (error != nullptr) {
            *error = "写入不完整: " + path + " (" + std::to_string(written) + "/" +
                     std::to_string(data.size()) + ")";
        }
        return status;
    }
    return status;
}

// ---------------------------------------------------------------------------
// 环境变量
// ---------------------------------------------------------------------------

std::optional<std::string> Getenv(const std::string& name) {
    if (name.empty()) {
        return std::nullopt;
    }
    // 必须拷贝：getenv 返回 libc 内部缓冲区指针，另一个线程 setenv 后即失效。
    const char* value = ::getenv(name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

FileStatus Setenv(const std::string& name, const std::string& value, bool overwrite,
                   std::string* error) {
    FileStatus status;
    if (name.empty() || name.find('=') != std::string::npos) {
        status.code = OsError::kInvalidArgument;
        status.system_error = EINVAL;
        if (error != nullptr) {
            *error = "环境变量名非法（不能为空或含 '='）: " + name;
        }
        return status;
    }
    if (::setenv(name.c_str(), value.c_str(), overwrite ? 1 : 0) != 0) {
        status.code = OsError::kIoFailed;
        status.system_error = errno;
        if (error != nullptr) {
            *error = "setenv 失败: " + name + " (errno=" + std::to_string(errno) + ")";
        }
        return status;
    }
    return status;
}

FileStatus Unsetenv(const std::string& name, std::string* error) {
    FileStatus status;
    if (name.empty()) {
        status.code = OsError::kInvalidArgument;
        status.system_error = EINVAL;
        return status;
    }
    if (::unsetenv(name.c_str()) != 0) {
        status.code = OsError::kIoFailed;
        status.system_error = errno;
        if (error != nullptr) {
            *error = "unsetenv 失败: " + name + " (errno=" + std::to_string(errno) + ")";
        }
        return status;
    }
    return status;
}

std::vector<std::pair<std::string, std::string>> Environ() {
    std::vector<std::pair<std::string, std::string>> out;
    if (environ == nullptr) {
        return out;
    }
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string_view line(*entry);
        const auto equal = line.find('=');
        if (equal == std::string_view::npos) {
            out.emplace_back(std::string(line), std::string{});
        } else {
            out.emplace_back(std::string(line.substr(0, equal)),
                             std::string(line.substr(equal + 1)));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 命令行参数
// ---------------------------------------------------------------------------

namespace {
std::vector<std::string>& ArgsStorage() {
    static std::vector<std::string> storage;
    return storage;
}
}  // namespace

void SetArgs(int argc, char** argv) {
    auto& storage = ArgsStorage();
    storage.clear();
    if (argc <= 0 || argv == nullptr) {
        return;
    }
    storage.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        // 拷贝而不是保存 argv 指针：argv 内存归宿主所有。
        storage.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
}

const std::vector<std::string>& Args() { return ArgsStorage(); }

const std::string& ProgramName() {
    static const std::string empty;
    const auto& storage = ArgsStorage();
    return storage.empty() ? empty : storage.front();
}

}  // namespace go2cpp::os
