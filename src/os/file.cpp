/**
 * @file file.cpp
 * @brief `os::File` 与 `os` 模块便利函数实现。
 *
 * 所有文件 IO 走 **`syscall(SYS_*)` 而不是 libc 包装** —— 理由见 file.hpp 顶部
 * 的边界结论 1.2/1.3：绕过 Hook 的符号拦截，避免为普通文件 IO 白构造
 * `BlockingRegion`。代价是**自己处理 `EINTR` 与部分写**（hook 原本替我们做了）。
 */
/**
 * ## 模块定位（补充）
 *
 * `os::File` 的实现层：向上支撑 `tests/os/test_os_file.cpp`、examples 与
 * `ReadFile`/`WriteFile`；向下直接发 `SYS_openat/read/write/pread/pwrite/fsync/
 * ftruncate/fstat/lseek`。它**不经过** Hook、不注册 epoll，也不参与 Fiber 调度。
 *
 * ## 本文件定义什么
 *
 * - `RawOpen`/`RawClose`/`IsSyscallError`：裸 syscall 辅助；
 * - `File` 的生命周期、打开/接管、读写、定位、元信息与同步；
 * - `ReadFile`/`WriteFile`、环境变量、`SetArgs`/`Args`/`ProgramName`。
 *
 * ## 阅读建议
 *
 * 先看 `RawOpen` 为什么强制 `O_NONBLOCK`（否则打开 FIFO 会卡在 open），再看
 * `Open`/`Adopt` 的 `S_ISSOCK` → `S_ISREG` 白名单，最后看 `Read`/`Write` 的
 * `EINTR` 重试与 `WriteAll` 的部分写循环。
 *
 * ## 边界与陷阱
 *
 * - 所有 IO 自己处理 `EINTR`；Hook 原本替我们做的事现在必须显式做；
 * - `syscall` 成功值可能是 `-1`，所以 `IsSyscallError` 只认 `[-4095, -1]`；
 * - `ReadFile` 的 `st_size` 只是容量提示；`os.max_file_size` 由读取循环**累计**
 *   判断 —— procfs 的 `st_size==0` 与「读期间被并发追加」都不能绕过上限。
 */
#include "go2cpp/config/env.hpp"
#include "go2cpp/os/file.hpp"
#include "go2cpp/os/os.hpp"

#include "raw_syscall.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

extern char** environ;

namespace go2cpp::os {

namespace {

/// 绕过 Hook 的 `openat`。失败返回 -1 且 errno 有效。
// 强制 O_NONBLOCK 只影响 open 本身：普通文件上 POSIX 保证它是 no-op；
// 对 FIFO/socket 则保证 open 不会在类型校验之前无限等待。
int RawOpen(const char* path, int flags, mode_t mode) noexcept {
    // O_NONBLOCK 的作用：**避免 open(2) 自己阻塞**。
    // 关键场景：`Open("/tmp/some_fifo")` 若不加 O_NONBLOCK，`open` 会一直等到
    // 有写端才返回 —— 根本到不了后面的 fstat 类型校验，worker 会卡在 open 里。
    // 对普通文件 O_NONBLOCK 是 no-op（POSIX 保证），所以加了无副作用。
    const long result = detail::Syscall4(SYS_openat, AT_FDCWD,
                                         static_cast<long>(reinterpret_cast<intptr_t>(path)),
                                         flags | O_CLOEXEC | O_NONBLOCK, mode);
    if (detail::SyscallFailed(result)) {
        // 本函数对外的契约是「失败返回 -1 且 errno 有效」，所以补写 errno。
        errno = detail::SyscallErrno(result);
        return -1;
    }
    return static_cast<int>(result);
}

/// 绕过 Hook 的 `close`。
int RawClose(int fd) noexcept {
    return static_cast<int>(detail::Syscall1(SYS_close, fd));
}

/**
 * @brief 把内核返回的长整型收窄成 ssize_t/off_t，并把 `errno` 暴露给调用方。
 * @details `syscall()` 成功时也可能返回值恰好是 -1（极端偏移），所以只在
 *          返回值落在 [-4095, -1] 区间时才认为是错误 —— 这是内核错误码的惯例。
 */
[[maybe_unused]] bool IsSyscallError(long result) noexcept {
    return result < 0 && result >= -4095;
}

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
    : m_fd(other.m_fd),
      m_owned(other.m_owned),
      m_append(other.m_append),
      m_last_error(other.m_last_error) {
    other.m_fd = -1;
    other.m_owned = false;
    other.m_append = false;
    other.m_last_error = 0;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        Close();
        m_fd = other.m_fd;
        m_owned = other.m_owned;
        m_append = other.m_append;
        m_last_error = other.m_last_error;
        other.m_fd = -1;
        other.m_owned = false;
        other.m_append = false;
        other.m_last_error = 0;
    }
    return *this;
}

bool File::Sync() noexcept {
    if (!valid()) {
        m_last_error = EBADF;
        return false;
    }
    const long result = detail::Syscall1(SYS_fsync, m_fd);
    if (detail::SyscallFailed(result)) {
        m_last_error = detail::SyscallErrno(result);
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
    const long result = detail::Syscall2(SYS_ftruncate, m_fd, size);
    if (detail::SyscallFailed(result)) {
        const int error = detail::SyscallErrno(result);
        status.code = OsError::kIoFailed;
        status.system_error = error;
        m_last_error = error;
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
    // 关闭后不再有 fd，O_APPEND 状态一并清掉（重新 Open 会重新设置）。
    m_append = false;
}

// ---------------------------------------------------------------------------
// 打开 / 接管
// ---------------------------------------------------------------------------

/**
 * @brief 打开一个普通文件；成功时把 fd 所有权移入 `*out`。
 *
 * ## 步骤
 * step1. 校验输出指针与路径（空路径直接 `EINVAL`）；
 * step2. `RawOpen`：追加 `O_CLOEXEC|O_NONBLOCK`，避免打开 FIFO 时卡在 open；
 * step3. `fstat` 做白名单校验：先判 socket 给出精确错误码，再要求 `S_ISREG`；
 *        **fstat 本身失败也按失败处理**（不能把未校验的 fd 装进 File）；
 * step4. 只有校验通过才把 fd/owned 写入 `*out`；失败路径先 `RawClose`，不漏 fd。
 *
 * ## 失败语义
 * `kOpenFailed`（open 失败，errno 有效）、`kIsSocket`、`kNotRegularFile`、
 * `kInvalidArgument`。失败时 `*out` 不会被写入新 fd。
 */
FileStatus File::Open(const std::string& path, OpenFlag flags, mode_t mode, File* out) {
    FileStatus status;
    if (out == nullptr) {
        // 与"路径为空"一样是参数错误：统一填 EINVAL，避免调用方按
        // system_error 分类时看到"无 errno"（审计 D13）。
        status.code = OsError::kInvalidArgument;
        status.system_error = EINVAL;
        return status;
    }
    if (path.empty()) {
        status.code = OsError::kInvalidArgument;
        status.system_error = EINVAL;
        return status;
    }
    // 路径可以带内嵌 '\0'，但 POSIX 只认到首个 NUL：不拦就会静默截断，
    // 实际打开的是另一个路径（路径注入），所以必须在此之前拒绝。
    if (detail::ContainsNul(path)) {
        status.code = OsError::kInvalidArgument;
        status.system_error = EINVAL;
        return status;
    }
    // step2: O_NONBLOCK 防止 open(FIFO) 在 fstat 之前阻塞整个 worker
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
    // step3: 白名单校验：anon_inodefs 的类型位为 0，黑名单必然漏掉
    struct ::stat info {};
    const long stat_result = detail::Syscall2(SYS_fstat, fd,
                                              static_cast<long>(reinterpret_cast<intptr_t>(&info)));
    if (detail::SyscallFailed(stat_result)) {
        // 与 Adopt 对齐：fstat 失败就无法确认 fd 类型，**绝不能**把未校验的
        // fd 交给只接受普通文件的 File —— 绕过 S_ISREG 白名单后，
        // FIFO/socket 的裸 syscall 读会把整个 worker 卡死（file.hpp 1.4）。
        const int stat_errno = detail::SyscallErrno(stat_result);
        (void)RawClose(fd);
        status.code = OsError::kOpenFailed;
        status.system_error = stat_errno;
        return status;
    }
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
    // step4: 校验全部通过后才转移所有权，失败路径不会泄漏 fd
    *out = File{};
    out->m_fd = fd;
    out->m_owned = true;
    // 记录 O_APPEND：WriteAt 依赖它拒绝"忽略 offset"的写入（见 file.hpp）。
    out->m_append = (static_cast<std::uint32_t>(flags) &
                     static_cast<std::uint32_t>(OpenFlag::kAppend)) != 0;
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

/**
 * @brief 接管一个已经打开的 fd，并拒绝 socket / 非普通文件。
 *
 * ## 步骤
 * step1. 校验输出指针与 fd 合法性；
 * step2. `fstat` 读取类型；失败按无效句柄处理（不关闭调用方的 fd）；
 * step3. `S_ISSOCK` → `kIsSocket`（比统一错误码更精确）；
 * step4. 非 `S_ISREG` → `kNotRegularFile`：FIFO/pipe/字符设备/eventfd/timerfd/
 *        signalfd 的 read 会真阻塞，而 `File::Read` 绕过 Hook；
 * step5. 校验通过后按 `take_ownership` 记录是否负责关闭。
 *
 * ## 不变量
 * `Adopt` **不修改** fd 的任何标志位（尤其不碰 `O_NONBLOCK`），避免与
 * `FdManager` 的 `m_user_nonblock`/`m_system_nonblock` 还原逻辑打架。
 */
FileStatus File::Adopt(int fd, bool take_ownership, File* out) {
    FileStatus status;
    if (out == nullptr) {
        // 与"路径为空"一样是参数错误：统一填 EINVAL，避免调用方按
        // system_error 分类时看到"无 errno"（审计 D13）。
        status.code = OsError::kInvalidArgument;
        status.system_error = EINVAL;
        return status;
    }
    if (fd < 0) {
        status.code = OsError::kInvalidHandle;
        status.system_error = EBADF;
        return status;
    }
    // step2: fstat 失败只报无效句柄，绝不能 close 调用方仍持有的 fd
    struct ::stat info {};
    const long fstat_result = detail::Syscall2(
        SYS_fstat, fd, static_cast<long>(reinterpret_cast<intptr_t>(&info)));
    if (detail::SyscallFailed(fstat_result)) {
        status.code = OsError::kInvalidHandle;
        status.system_error = detail::SyscallErrno(fstat_result);
        return status;
    }
    // step3: socket 先单独判，错误码比统一的 kNotRegularFile 更精确
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
    // step4: 白名单只留 S_ISREG；FIFO/eventfd 的 read 会真阻塞
    if (!S_ISREG(info.st_mode)) {
        status.code = OsError::kNotRegularFile;
        return status;
    }
    // step5: 所有权按调用方声明记录，不在此处 close 借用 fd。
    // 用 F_GETFL **只读探测** O_APPEND（不修改任何标志位，符合 1.5 的承诺），
    // 供 WriteAt 判断是否需要拒绝。
    const int fd_flags = ::fcntl(fd, F_GETFL);
    *out = File{};
    out->m_fd = fd;
    out->m_owned = take_ownership;
    out->m_append = fd_flags >= 0 && (fd_flags & O_APPEND) != 0;
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
        const long result = detail::Syscall3(
            SYS_read, m_fd,
            static_cast<long>(reinterpret_cast<intptr_t>(buffer)),
            static_cast<long>(length));
        if (result >= 0) {
            m_last_error = 0;
            return static_cast<ssize_t>(result);
        }
        const int error = detail::SyscallErrno(result);
        if (error == EINTR) {
            continue;   // 绕过 hook 后必须自己重试
        }
        m_last_error = error;
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
        const long result = detail::Syscall4(
            SYS_pread64, m_fd,
            static_cast<long>(reinterpret_cast<intptr_t>(buffer)),
            static_cast<long>(length), offset);
#else
        const long result = detail::Syscall4(
            SYS_pread, m_fd,
            static_cast<long>(reinterpret_cast<intptr_t>(buffer)),
            static_cast<long>(length), offset);
#endif
        if (result >= 0) {
            m_last_error = 0;
            return static_cast<ssize_t>(result);
        }
        const int error = detail::SyscallErrno(result);
        if (error == EINTR) {
            continue;
        }
        m_last_error = error;
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
        const long result = detail::Syscall3(
            SYS_write, m_fd,
            static_cast<long>(reinterpret_cast<intptr_t>(buffer)),
            static_cast<long>(length));
        if (result >= 0) {
            m_last_error = 0;
            return static_cast<ssize_t>(result);
        }
        const int error = detail::SyscallErrno(result);
        if (error == EINTR) {
            continue;
        }
        m_last_error = error;
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
    // Linux 的 pwrite(2) 在 O_APPEND 句柄上会忽略 offset、把数据追加到末尾
    // （POSIX 允许，Linux BUGS 有记载）。静默写错位置会破坏并发/随机写的数据，
    // 所以这里显式失败，让调用方改用不带 O_APPEND 的句柄（审计 D5）。
    if (m_append) {
        m_last_error = EINVAL;
        return -1;
    }
    for (;;) {
#ifdef SYS_pwrite64
        const long result = detail::Syscall4(
            SYS_pwrite64, m_fd,
            static_cast<long>(reinterpret_cast<intptr_t>(buffer)),
            static_cast<long>(length), offset);
#else
        const long result = detail::Syscall4(
            SYS_pwrite, m_fd,
            static_cast<long>(reinterpret_cast<intptr_t>(buffer)),
            static_cast<long>(length), offset);
#endif
        if (result >= 0) {
            m_last_error = 0;
            return static_cast<ssize_t>(result);
        }
        const int error = detail::SyscallErrno(result);
        if (error == EINTR) {
            continue;
        }
        m_last_error = error;
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
        const long result = detail::Syscall3(SYS_lseek, m_fd, offset, whence);
        if (result >= 0) {
            m_last_error = 0;
            return static_cast<off_t>(result);
        }
        const int error = detail::SyscallErrno(result);
        if (error == EINTR) {
            continue;
        }
        m_last_error = error;
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
    const long result = detail::Syscall2(SYS_fstat, m_fd,
                                         static_cast<long>(reinterpret_cast<intptr_t>(out)));
    if (detail::SyscallFailed(result)) {
        const_cast<File*>(this)->m_last_error = detail::SyscallErrno(result);
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
// 批量（scatter/gather）IO 与零拷贝
// ---------------------------------------------------------------------------
//
// 为什么要有这几个接口：`File::Read`/`Write` 的**固定成本**远大于 8 字节的
// 数据本身。本机（WSL2 / Ryzen 9 8945HX）实测 1e6 次 8B 顺序读写：
//
//   ::read  460 ns/次      File::Read  470 ns/次
//   ::write 820 ns/次      File::Write 840 ns/次
//
// 其中内核真正搬 8 字节的时间只占小头，剩下是 syscall 进入/退出 + 函数调用
// 边界。一次 `readv`/`writev` 处理 1024 条，就把这笔固定成本摊到 1/1024：
// 实测 writev 46 ns/条、readv 21 ns/条（**18~22x**）。
//
// 批量接口的难点不是发 syscall，而是**部分写/部分读的正确推进**：
// `writev` 允许只写前几个 iovec 的一部分就返回，下一次必须从「写到哪里」
// 继续，不能重发已经写过的段（否则数据重复）。

namespace {

/// Linux `IOV_MAX`：`readv`/`writev` 的 iovcnt 硬上限，超过内核返回 `EINVAL`。
constexpr int kMaxIov = 1024;

/// 所有 iovec 的字节数之和（可能为 0，调用方自行决定是否提前返回）。
std::size_t IovTotalBytes(const struct iovec* iov, int iovcnt) noexcept {
    std::size_t total = 0;
    for (int index = 0; index < iovcnt; ++index) {
        total += iov[index].iov_len;
    }
    return total;
}

/// 把 `consumed` 个已处理字节从 iovec 数组里推进掉（就地修改）。
/// @return 新的起始下标（相对传入数组）；返回值 == iovcnt 表示全部处理完。
int AdvanceIov(struct iovec* iov, int iovcnt, std::size_t consumed) noexcept {
    int index = 0;
    while (index < iovcnt && consumed >= iov[index].iov_len) {
        consumed -= iov[index].iov_len;
        ++index;
    }
    if (index < iovcnt && consumed > 0) {
        auto* base = static_cast<std::uint8_t*>(iov[index].iov_base);
        iov[index].iov_base = base + consumed;
        iov[index].iov_len -= consumed;
    }
    return index;
}

/// 粗筛：只有真的存在「可合并的相邻段」或零长段时才构造合并数组。
/// 全分散的 iovec 直接走原数组，避免每次都白写 16 KiB 栈数组（实测使分散
/// 负载多付约 1 us/批）。命中时通常在前两次迭代就退出。
bool HasMergeableIov(const struct iovec* iov, int iovcnt) noexcept {
    if (iov[0].iov_len == 0) {
        return true;
    }
    for (int index = 1; index < iovcnt; ++index) {
        if (iov[index].iov_len == 0) {
            return true;
        }
        auto* tail = static_cast<std::uint8_t*>(iov[index - 1].iov_base) +
                     iov[index - 1].iov_len;
        if (tail == static_cast<std::uint8_t*>(iov[index].iov_base)) {
            return true;
        }
    }
    return false;
}

/// 触发 iovec 合并的最小段数：段数少时内核走栈上快路径，合并反而多一次扫描。
constexpr int kMergeMinIov = 8;

/// 合并首尾相接的相邻 iovec：总字节与顺序不变，只减少内核要逐段处理的段数。
///
/// 为什么值得做：内核 readv/writev 的成本几乎全在「逐段」而不是「搬字节」。
/// 本机实测 1024 段 x 8B 的 readv 约 32.2 us/次，而同样 8192 字节的单段
/// readv 约 1.0 us/次（32 倍差距，数据量完全相同）。
///
/// @param out 输出数组，容量必须 >= iovcnt。
/// @param total 输出：所有段字节数之和。
/// @return 合并后的段数；零长段不搬运字节，直接丢弃。
///
/// ## 定位
/// 向量 I/O 的「固定成本摊销器」：内核按段收费，1024 段 8B 的 readv 约 32us，
/// 合并成 1 段同样字节数约 1us。合并不改变字节语义，只减少 iovcnt；
/// 一旦把不相邻的段误判为相邻，就会读到/写到错误地址，属于内存破坏级错误。
///
/// ## 步骤
/// step1. 逐段累加总字节数（含零长段，保证 total 与输入一致）；
/// step2. 零长段直接丢弃，不占输出名额；
/// step3. 用「上一段尾地址 == 本段首地址」判定相邻，命中则只加长上一段；
/// step4. 不相邻则原样拷贝到输出数组。
///
/// ## 并发/失败语义
/// - 调用约束：纯函数，任意线程可并发调用；out 必须由调用方保证容量 >= iovcnt。
/// - 原子性：无共享状态，不修改输入。
/// - 失败语义：noexcept，无分配；指针比较只在同一对象内有效，跨对象比较由调用方语义保证。
/// - 不变量：返回段数 <= iovcnt；*total 恒等于所有输入段长度之和。
int MergeAdjacentIov(const struct iovec* iov, int iovcnt, struct iovec* out,
                     std::size_t* total) noexcept {
    int count = 0;
    std::size_t sum = 0;
    for (int index = 0; index < iovcnt; ++index) {
        const std::size_t len = iov[index].iov_len;
        sum += len;
        if (len == 0) {
            continue;
        }
        if (count > 0) {
            auto* tail = static_cast<std::uint8_t*>(out[count - 1].iov_base) +
                         out[count - 1].iov_len;
            if (tail == static_cast<std::uint8_t*>(iov[index].iov_base)) {
                out[count - 1].iov_len += len;
                continue;
            }
        }
        out[count] = iov[index];
        ++count;
    }
    *total = sum;
    return count;
}

}  // namespace

ssize_t File::Readv(const struct iovec* iov, int iovcnt) noexcept {
    if (m_fd < 0) {
        m_last_error = EBADF;
        return -1;
    }
    if (iov == nullptr || iovcnt <= 0 || iovcnt > kMaxIov) {
        m_last_error = EINVAL;
        return -1;
    }
    for (;;) {
        const long result = detail::Syscall3(
            SYS_readv, m_fd, static_cast<long>(reinterpret_cast<intptr_t>(iov)), iovcnt);
        if (result >= 0) {
            m_last_error = 0;
            return static_cast<ssize_t>(result);
        }
        const int error = detail::SyscallErrno(result);
        if (error == EINTR) {
            continue;
        }
        m_last_error = error;
        return -1;
    }
}

ssize_t File::Writev(const struct iovec* iov, int iovcnt) noexcept {
    if (m_fd < 0) {
        m_last_error = EBADF;
        return -1;
    }
    if (iov == nullptr || iovcnt <= 0 || iovcnt > kMaxIov) {
        m_last_error = EINVAL;
        return -1;
    }
    for (;;) {
        const long result = detail::Syscall3(
            SYS_writev, m_fd, static_cast<long>(reinterpret_cast<intptr_t>(iov)), iovcnt);
        if (result >= 0) {
            m_last_error = 0;
            return static_cast<ssize_t>(result);
        }
        const int error = detail::SyscallErrno(result);
        if (error == EINTR) {
            continue;
        }
        m_last_error = error;
        return -1;
    }
}

/**
 * @brief 把 iov 全部写完，处理短写与 EINTR；必要时合并相邻段降低内核固定成本。
 *
 * @param iov [in] 待写段数组，不可为 nullptr——引用在本调用期间有效，函数不改其内容。
 * @param iovcnt [in] 段数，必须满足 1 <= iovcnt <= kMaxIov；超界返回 EINVAL。
 *
 * @return 实际写入的总字节数：== 期望值时表示写满——调用方**必须**按成功处理；
 *         小于期望值时表示短写后写不动（管道关闭/慢盘）——调用方**必须**从返回值处续写，
 *         不要把部分写入当失败重发整段。
 *
 * ## 定位
 * 数据面唯一的「写满」入口。若把短写当成功直接返回，会静默丢掉后半段数据；
 * 若失败时返回非零而不设 errno，调用方无法区分 EOF 与错误。
 *
 * ## 步骤
 * step1. 校验 fd 与 iov 参数；
 * step2. 段数够多且存在相邻段时才堆分配合并数组，分配失败退回原数组（语义不变）；
 * step3. 总长度为 0 直接成功返回；
 * step4. 快路径：一次 Writev 写满就返回，不复制 iovec；
 * step5. 慢路径：复制可推进的 iovec 副本，用 AdvanceIov 跳过已写段继续写；
 * step6. 写不动（<=0）时停止并返回已写字节数。
 *
 * ## 并发/失败语义
 * - 调用约束：同一 File 的写操作需调用方串行化（fd 偏移是共享状态）。
 * - 原子性：单次 writev 原子；多轮续写不是原子的，可能与其他写者交错。
 * - 失败语义：noexcept；失败通过返回 0/部分值与 m_last_error 表达，不抛异常。
 * - 不变量：返回值 <= 所有段长度之和；返回满值时 iov 内容已全部写出。
 */
std::size_t File::WritevAll(const struct iovec* iov, int iovcnt) noexcept {
    if (m_fd < 0) {
        m_last_error = EBADF;
        return 0;
    }
    if (iov == nullptr || iovcnt <= 0 || iovcnt > kMaxIov) {
        m_last_error = EINVAL;
        return 0;
    }
    // 相邻段合并：内核按段收费，1024 段小记录合成 1 段大拷贝能省掉绝大部分
    // 内核固定成本（语义不变，见 MergeAdjacentIov）。段数少时不扫描。
    // 合并数组**只在需要合并时才堆分配**：16 KiB 栈数组会让固定栈 Fiber 或
    // 极小线程栈爆栈（本函数是通用 API，不应把风险埋进来）。分配失败不报错，
    // 退回原数组走未合并路径，语义与不合并完全一致。
    std::unique_ptr<struct iovec[]> merged;
    const struct iovec* use = iov;
    int use_cnt = iovcnt;
    std::size_t want = 0;
    if (iovcnt > kMergeMinIov && HasMergeableIov(iov, iovcnt)) {
        merged.reset(new (std::nothrow) struct iovec[static_cast<std::size_t>(iovcnt)]);
        if (merged) {
            const int merged_cnt = MergeAdjacentIov(iov, iovcnt, merged.get(), &want);
            if (merged_cnt < iovcnt) {
                use = merged.get();
                use_cnt = merged_cnt;
            }
        } else {
            want = IovTotalBytes(iov, iovcnt);   // 分配失败：退回原数组，不报错
        }
    } else {
        want = IovTotalBytes(iov, iovcnt);
    }
    if (want == 0) {
        m_last_error = 0;
        return 0;
    }
    // 快路径：绝大多数情况一次写满，直接返回，**不分配、不复制 iovec**
    const ssize_t first = Writev(use, use_cnt);
    if (first < 0) {
        return 0;
    }
    auto total = static_cast<std::size_t>(first);
    if (total == want || total == 0) {
        return total;
    }
    // 慢路径：部分写。复制一份可推进的 iovec，从「写到哪里」继续。
    // 只有短写（管道/慢盘/信号打断）才走到这里，热路径不付这个代价。
    try {
        std::vector<struct iovec> rest(use, use + use_cnt);
        int index = AdvanceIov(rest.data(), use_cnt, total);
        while (index < use_cnt) {
            const ssize_t written = Writev(rest.data() + index, use_cnt - index);
            if (written <= 0) {
                break;   // 写不动了：0 在写路径上按失败处理
            }
            const auto count = static_cast<std::size_t>(written);
            total += count;
            index += AdvanceIov(rest.data() + index, use_cnt - index, count);
        }
    } catch (const std::bad_alloc&) {
        m_last_error = ENOMEM;
        return total;
    }
    return total;
}

/**
 * @brief 把 iov 读满（直到所有段填满或 EOF），必要时合并相邻段。
 *
 * @param iov [in,out] 接收缓冲区段数组，不可为 nullptr；函数只写缓冲区内容，不改 iov 本身——引用在本调用期间有效。
 * @param iovcnt [in] 段数，必须满足 1 <= iovcnt <= kMaxIov；超界返回 EINVAL。
 *
 * @return 读到的总字节数：等于期望总量表示读满——调用方**必须**按完整数据处理；
 *         小于期望值表示 EOF（含一开始就 EOF 的 0）——调用方**必须**当作流结束；
 *         -1 表示错误——调用方**必须**读 m_last_error 并停止。
 *
 * ## 定位
 * 数据面唯一的「读满」入口，用于定长协议头/帧体。少读一个字节就会让上层解析读到半截帧。
 *
 * ## 步骤
 * step1. 校验 fd 与 iov 参数；
 * step2. 段数够多且存在相邻段时才堆分配合并数组，分配失败退回原数组；
 * step3. 总长度为 0 直接返回 0；
 * step4. 首轮 Readv 读满或 EOF 时立即返回；
 * step5. 否则复制 iovec 副本，按已读字节推进后继续读；
 * step6. 读到 0 视为 EOF 并返回；读出错返回 -1。
 *
 * ## 并发/失败语义
 * - 调用约束：同一 File 的读操作需调用方串行化。
 * - 原子性：单次 readv 原子；多轮续读可能与其他读者交错。
 * - 失败语义：noexcept；错误经 m_last_error 返回，不抛异常。
 * - 不变量：返回正数时，前 n 个字节分布在 iov 覆盖的连续区域中。
 */
ssize_t File::ReadvAll(const struct iovec* iov, int iovcnt) noexcept {
    if (m_fd < 0) {
        m_last_error = EBADF;
        return -1;
    }
    if (iov == nullptr || iovcnt <= 0 || iovcnt > kMaxIov) {
        m_last_error = EINVAL;
        return -1;
    }
    // 同 WritevAll：先把首尾相接的相邻段合并，再下发 syscall。
    // 合并数组**只在需要合并时才堆分配**：16 KiB 栈数组会让固定栈 Fiber 或
    // 极小线程栈爆栈（本函数是通用 API，不应把风险埋进来）。分配失败不报错，
    // 退回原数组走未合并路径，语义与不合并完全一致。
    std::unique_ptr<struct iovec[]> merged;
    const struct iovec* use = iov;
    int use_cnt = iovcnt;
    std::size_t want = 0;
    if (iovcnt > kMergeMinIov && HasMergeableIov(iov, iovcnt)) {
        merged.reset(new (std::nothrow) struct iovec[static_cast<std::size_t>(iovcnt)]);
        if (merged) {
            const int merged_cnt = MergeAdjacentIov(iov, iovcnt, merged.get(), &want);
            if (merged_cnt < iovcnt) {
                use = merged.get();
                use_cnt = merged_cnt;
            }
        } else {
            want = IovTotalBytes(iov, iovcnt);   // 分配失败：退回原数组，不报错
        }
    } else {
        want = IovTotalBytes(iov, iovcnt);
    }
    if (want == 0) {
        m_last_error = 0;
        return 0;
    }
    const ssize_t first = Readv(use, use_cnt);
    if (first < 0) {
        return -1;
    }
    std::size_t total = static_cast<std::size_t>(first);
    if (total == want || total == 0) {
        return static_cast<ssize_t>(total);   // 读满，或一开始就是 EOF
    }
    try {
        std::vector<struct iovec> rest(use, use + use_cnt);
        int index = AdvanceIov(rest.data(), use_cnt, total);
        while (index < use_cnt) {
            const ssize_t got = Readv(rest.data() + index, use_cnt - index);
            if (got < 0) {
                return -1;
            }
            if (got == 0) {
                break;   // EOF
            }
            const auto count = static_cast<std::size_t>(got);
            total += count;
            index += AdvanceIov(rest.data() + index, use_cnt - index, count);
        }
    } catch (const std::bad_alloc&) {
        m_last_error = ENOMEM;
        return -1;
    }
    return static_cast<ssize_t>(total);
}

FileStatus File::CopyTo(File& dst, std::size_t* copied, std::size_t max_bytes) noexcept {
    FileStatus status;
    if (copied != nullptr) {
        *copied = 0;
    }
    if (m_fd < 0 || dst.m_fd < 0) {
        status.code = OsError::kInvalidHandle;
        status.system_error = EBADF;
        m_last_error = EBADF;
        return status;
    }
    constexpr std::size_t kUnlimited = static_cast<std::size_t>(-1);
    const std::size_t limit = max_bytes == 0 ? kUnlimited : max_bytes;
    if (limit == 0) {
        return status;   // 明确要求拷 0 字节：不是错误
    }
    off_t offset = Tell();
    const bool have_offset = offset >= 0;
    std::size_t total = 0;
    bool fallback = false;
    for (;;) {
        if (total >= limit) {
            break;
        }
        std::size_t chunk = limit - total;
        if (chunk > (1u << 20)) {
            chunk = 1u << 20;   // 单次 sendfile 控制在 1 MiB，避免一次卡太久
        }
#ifdef SYS_sendfile
        const long result = detail::Syscall4(
            SYS_sendfile, dst.m_fd, m_fd,
            have_offset ? static_cast<long>(reinterpret_cast<intptr_t>(&offset)) : 0L,
            static_cast<long>(chunk));
        if (result < 0) {
            const int error = detail::SyscallErrno(result);
            // 只有在**一字节都没拷**时才允许回退：混用两条路径会让
            // 「已拷贝了多少」变得难以推理（sendfile 的 offset 与 fd 偏移不一致）。
            if (total == 0 &&
                (error == EINVAL || error == ENOSYS || error == EXDEV || error == EOPNOTSUPP ||
                 error == ENOTSUP)) {
                fallback = true;
                break;
            }
            m_last_error = error;
            status.code = OsError::kIoFailed;
            status.system_error = error;
            if (copied != nullptr) {
                *copied = total;
            }
            return status;
        }
        if (result == 0) {
            break;   // EOF
        }
        total += static_cast<std::size_t>(result);
#else
        fallback = true;
        break;
#endif
    }
    if (!fallback) {
        // sendfile 用显式 offset 时**不移动** in_fd 的偏移，这里补上，
        // 让本对象的语义与连续 Read 一致（读了多少，偏移就前进多少）。
        if (have_offset && Seek(offset, SEEK_SET) < 0) {
            status.code = OsError::kIoFailed;
            status.system_error = m_last_error;
            if (copied != nullptr) {
                *copied = total;
            }
            return status;
        }
        if (copied != nullptr) {
            *copied = total;
        }
        return status;
    }
    // 回退路径：普通的读+写。语义一致，只是多一次用户态拷贝。
    std::vector<char> buffer(64 * 1024);
    while (total < limit) {
        std::size_t take = limit - total;
        if (take > buffer.size()) {
            take = buffer.size();
        }
        const ssize_t got = Read(buffer.data(), take);
        if (got < 0) {
            status.code = OsError::kIoFailed;
            status.system_error = m_last_error;
            if (copied != nullptr) {
                *copied = total;
            }
            return status;
        }
        if (got == 0) {
            break;
        }
        const auto count = static_cast<std::size_t>(got);
        const std::size_t wrote = dst.WriteAll(buffer.data(), count);
        total += wrote;
        if (wrote != count) {
            status.code = OsError::kIoFailed;
            status.system_error = dst.last_error();
            if (copied != nullptr) {
                *copied = total;
            }
            return status;
        }
    }
    if (copied != nullptr) {
        *copied = total;
    }
    return status;
}

// ---------------------------------------------------------------------------
// 便利函数
// ---------------------------------------------------------------------------

/**
 * @brief 把整个文件读进 `std::string`，带大小上限与可配置缓冲区。
 *
 * ## 步骤
 * step1. 校验输出并清空，打开只读句柄；
 * step2. 用 `st_size` 做容量提示，并按 `os.max_file_size` 提前拒绝超大文件；
 * step3. 以 `os.default_read_buffer_size` 为块循环 `read`，直到返回 0（EOF）；
 * step4. **累计**读取字节数在循环内检查上限：`st_size==0` 的 procfs 伪文件、
 *        读期间被并发追加的文件都不能绕过；超限立即清空并返回 `EFBIG`；
 * step5. 读失败或分配失败（`bad_alloc` / `length_error`）时清空输出并返回
 *        `FileStatus` —— 本模块的错误一律走返回值，不让异常穿透。
 *
 * ## 并发语义
 * 文件可能在读取期间增长：`st_size` 只用于 `reserve`，实际安全边界由累计
 * 检查保证（上限为 `os.max_file_size`，0 表示不限制）。
 */
FileStatus ReadFile(const std::string& path, std::string* out, std::string* error) {
    FileStatus status;
    if (out == nullptr) {
        // 与"路径为空"一样是参数错误：统一填 EINVAL，避免调用方按
        // system_error 分类时看到"无 errno"（审计 D13）。
        status.code = OsError::kInvalidArgument;
        status.system_error = EINVAL;
        return status;
    }
    out->clear();
    // step1: 打开失败直接返回；ReadFile 不创建文件，也不重试
    File file;
    status = File::Open(path, OpenFlag::kReadOnly, 0, &file);
    if (!status.ok()) {
        if (error != nullptr) {
            *error = "打开失败: " + path + " (errno=" + std::to_string(status.system_error) + ")";
        }
        return status;
    }
    // step2: 上限检查发生在分配/读取之前，防止单个大文件耗尽内存
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

    // step3: 读循环。st_size 只是容量提示 —— procfs 伪文件的 st_size 恒为 0，
    // 普通文件也可能在读期间被并发追加；所以上限必须在**读取过程中累计判断**，
    // 而不能只信打开前的 st_size。超限时清空输出并报 EFBIG，绝不交付半份/超限内容。
    std::size_t total = 0;
    try {
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
                    *error = "读失败: " + path + " (errno=" +
                             std::to_string(status.system_error) + ")";
                }
                return status;
            }
            if (got == 0) {
                break;
            }
            const auto count = static_cast<std::size_t>(got);
            // total 每轮都被下面的检查限制在 <= limit，所以 limit - total 不会下溢。
            if (limit > 0 && (total >= limit || count > limit - total)) {
                out->clear();
                status.code = OsError::kIoFailed;
                status.system_error = EFBIG;
                if (error != nullptr) {
                    *error = "文件超过 os.max_file_size 上限: " + path + " (> " +
                             std::to_string(limit) + " 字节)";
                }
                return status;
            }
            out->append(buffer.data(), count);
            total += count;
        }
    } catch (const std::bad_alloc&) {
        // 分配失败不能穿透成异常：模块约定用 FileStatus 报错（否则调用方
        // 没有 catch 时直接 std::terminate，DoS 面比"读失败"大得多）。
        out->clear();
        status.code = OsError::kIoFailed;
        status.system_error = ENOMEM;
        if (error != nullptr) {
            *error = "内存不足，无法读取: " + path;
        }
        return status;
    } catch (const std::length_error&) {
        // 文件大于 string::max_size()：同属容量不足，归一为 ENOMEM。
        out->clear();
        status.code = OsError::kIoFailed;
        status.system_error = ENOMEM;
        if (error != nullptr) {
            *error = "文件超出可表示大小: " + path;
        }
        return status;
    }
    // step5: 读到 EOF 才算成功；失败路径已清空 out 并写 errno
    return status;
}

/**
 * @brief 把 `data` 整体写入文件（创建或截断）。
 *
 * ## 步骤
 * step1. `Create` 打开（O_RDWR|O_CREAT|O_TRUNC），失败即返回；
 * step2. `WriteAll` 负责 EINTR 与部分写；写不满一律按 `kIoFailed` 上报。
 */
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
    // step2: WriteAll 已经循环处理部分写，这里的短写只可能是真失败
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
    // 空名与内嵌 NUL 都按「不存在」处理：getenv 会把 NUL 后的内容截掉，
    // 返回的会是另一个变量的值，比返回 nullopt 更危险。
    if (name.empty() || detail::ContainsNul(name)) {
        return std::nullopt;
    }
    // 统一走配置来源适配层：os 原语（与 Setenv 成对）不直接调 getenv，
    // 读取实现集中在一处，便于审计与测试隔离。返回的 std::string 是拷贝，
    // 不持有 libc 缓冲区（并发 setenv 会使其失效）。
    return go2cpp::config::GetEnv(name);
}

FileStatus Setenv(const std::string& name, const std::string& value, bool overwrite,
                   std::string* error) {
    FileStatus status;
    // setenv 的两个参数都是 C 字符串：名字或值里的内嵌 NUL 都会被静默截断，
    // 结果设置成另一个变量/另一个值，所以一并拒绝。
    if (name.empty() || name.find('=') != std::string::npos ||
        detail::ContainsNul(name) || detail::ContainsNul(value)) {
        status.code = OsError::kInvalidArgument;
        status.system_error = EINVAL;
        if (error != nullptr) {
            *error = "环境变量名/值非法（名不能为空或含 '='；两者都不能含内嵌 NUL）";
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
    // 同 Setenv：NUL 后的内容会被截断，实际删的是另一个变量。
    if (name.empty() || detail::ContainsNul(name)) {
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
