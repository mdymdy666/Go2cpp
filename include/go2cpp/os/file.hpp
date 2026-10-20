/**
 * @file file.hpp
 * @brief `os::File` —— RAII 文件句柄（开荒版）。
 *
 * ============================================================================
 * ## 一、与 Hook / FdManager 的边界结论（本模块最重要的设计依据）
 *
 * 结论来自阅读 `src/network/hook.cpp` 的 `cooperative_io` 与 `inspect_socket`。
 *
 * ### 1.1 Hook 靠 `getsockopt(SO_TYPE)` 区分 socket 与普通文件
 *
 * `read()` 被 Hook 拦截后走 `cooperative_io`，其中：
 *
 * ```cpp
 * auto descriptor = descriptor_for(fd);           // 查已登记的受管描述符
 * if (!descriptor && managed_hook_admission) {
 *     descriptor = try_adopt_socket(fd);          // 内部用 getsockopt(SO_TYPE)
 * }
 * if (!descriptor) {
 *     blocking_region.emplace();                  // 标记「本 M 正在阻塞」
 *     return invoke_real(function, fd, args...);  // 直接走 libc
 * }
 * ```
 *
 * 而 `inspect_socket()` 实现是 `getsockopt(fd, SOL_SOCKET, SO_TYPE, ...)`。
 * **对普通文件这个调用必然失败（`ENOTSOCK`）**，所以：
 *
 * > **普通文件的 fd 永远不会被 Hook 接管，`read`/`write` 会直接落到 libc。**
 *
 * 也就是说 Hook **已经天然区分了** socket 与文件，`os::File` 不需要自己做类型
 * 判断来"避免被误拦" —— 它根本不会被拦到等待路径上。
 *
 * ### 1.2 但仍有一层该省掉的开销：`BlockingRegion`
 *
 * 即使直接走 libc，Hook 仍会为每次调用构造一个 `BlockingRegion`（作用域对象），
 * 目的是让 `sysmon` 知道「这个 M 正在执行可能很长的系统调用」，从而在
 * `long_syscall_threshold` 之后申请替代 M。
 *
 * 对 socket 这是必要的；**对普通文件读写则几乎总是多余的**：
 * 本地文件的 `read`/`write` 不会 `EAGAIN`、不会长时间等待（网络文件系统除外），
 * 每次调用都标一次「M 在阻塞」只会给 sysmon 制造噪声，且多一次构造/析构。
 *
 * **因此本模块的读写路径使用 `::syscall(SYS_read/SYS_write/...)` 直接进内核，
 * 完全绕过 Hook 的符号拦截。** 这不是"绕开安全机制"，因为文件 IO 本来就不该
 * 参与协程调度 —— 见 1.3。
 *
 * ### 1.3 为什么文件 IO 不该走 Fiber 挂起路径
 *
 * 普通文件的 `read` 在内核里**同步完成**：数据已经在 page cache 或要从磁盘读，
 * 内核会把调用线程投入 `TASK_UNINTERRUPTIBLE` 后返回，**不存在 `EAGAIN` 让出
 * 再去等 epoll 的语义**。如果强行把它接入 `IOManager`：
 * - 要么先 park 一次 Fiber 再立刻被唤醒 —— 白付一次调度往返（约 1µs 量级），
 *   而实际 `read` 可能只要 100ns；
 * - 要么把文件 fd 设为非阻塞 —— 对普通文件 `O_NONBLOCK` 无意义（`read` 照样同步），
 *   却会让后续所有调用方看到意外的 fd 状态。
 *
 * 所以本模块**刻意不接入协程调度**：`Read`/`Write` 是同步阻塞调用，
 * 会阻塞当前 M。这与 Go 的行为一致 —— Go 的 `os.File.Read` 对普通文件同样
 * 走同步路径（runtime 只对 pollable fd 做 netpoller 调度）。
 *
 * **代价（必须让使用者知道）**：如果在 Fiber 里读一个**慢速文件系统**
 * （NFS / FUSE / 大文件冷读），会阻塞整个 M。此时应显式包一层
 * `Scheduler::BlockingRegion`，让 sysmon 有机会申请替代 M。
 *
 * ### 1.4 因此 `os::File` **必须拒绝 socket fd**
 *
 * 既然读写绕过了 Hook，若把 socket fd 塞进 `os::File`：
 * - `read`/`write` 会走裸 `syscall`，**不会被 hook 接管**；
 * - 结果是**整个 M 线程被阻塞在 socket 上**，协程调度器被拖死。
 *
 * 这是本模块**唯一的高危误用**，所以：
 * - `Open`/`Create` 只接受普通文件路径；
 * - `Adopt`（接管已有 fd）会**显式检查** `S_ISREG`/`S_ISDIR`/`S_ISFIFO` 等，
 *   **拒绝 `S_ISSOCK`**，并返回 `kIsSocket` 错误。
 *
 * ### 1.5 与 `FdContext{m_user_nonblock, m_system_nonblock}` 的关系
 *
 * 那套标志是给**受管 socket** 用的：`m_user_nonblock` 记录用户是否原本要求非阻塞，
 * `m_system_nonblock` 记录 Hook 为了配合 `IOManager` 实际设置的值，关闭时需要还原。
 *
 * **`os::File` 不参与这套机制**：文件 fd 从不进入 `FdManager`，`O_NONBLOCK`
 * 对普通文件也无意义。本模块**不修改**调用方传入 fd 的任何标志位
 * （`Adopt` 时只读探测，不 `fcntl` 改写），避免与 `FdManager` 的还原逻辑打架。
 *
 * ============================================================================
 *
 * ## 二、未实现 / 只做声明的东西（开荒版明确边界）
 *
 * - **`Exit`**：只有声明 + 警告注释，**没有实现**。原因见 `os.hpp` 里的说明 ——
 *   裸 `exit()` 会跳过 Fiber 栈释放与挂起任务析构，属高危，需要调度器配合才能做对。
 * - **目录遍历**（`ReadDir` / `MkdirAll` / `RemoveAll`）：未做，属后续。
 * - **`File` 的异步/分片读**、`io/fs` 风格的抽象层：未做。
 * - **符号链接 / 权限 / 属主**：`Stat` 透传 `struct stat`，但没有 Lua 式的便捷封装。
 * - **Windows**：本模块假设 POSIX（`syscall`/`fstat`/`pread`），未做平台抽象。
 */
/**
 * ## 模块定位（补充）
 *
 * `go2cpp::os::File` 是**普通文件**的 RAII 句柄，位于调度器可见范围之外：
 * `Read`/`Write`/`Sync` 走裸 `syscall`，不注册 epoll、不 park Fiber、也不经过
 * `FdManager`。上游调用方是 `tests/os/test_os_file.cpp`、examples 与
 * `os::ReadFile`/`WriteFile` 便利函数；`config` 的 `SaveToFile` 走的是
 * `std::ofstream + rename`，不经过本类。
 *
 * ## 本文件定义什么
 *
 * - `OsError`：稳定的错误码集合（尤其 `kIsSocket`/`kNotRegularFile`）；
 * - `FileStatus` + `ToString`：错误码 + errno 的轻量结果；
 * - `OpenFlag` 与 `operator|`：`open(2)` 标志的语义化别名；
 * - `File`：可移动不可复制的句柄，**只接受普通文件**；
 * - `ReadFile`/`WriteFile`、环境变量、命令行参数等便利函数。
 *
 * ## 阅读建议
 *
 * 必须先读文件头「一、与 Hook / FdManager 的边界结论」：它解释了为什么文件
 * IO 要绕过 Hook、为什么 `Adopt` 必须拒绝 socket/FIFO。然后看 `OsError::
 * kNotRegularFile` 的 fd 类型表，最后看 `File` 的状态机（`docs/state-machines/
 * File.md`）。
 *
 * ## 边界与陷阱
 *
 * - **只接受 `S_ISREG`**：FIFO/pipe/字符设备/eventfd/timerfd/signalfd 的 read
 *   会真阻塞，而本类绕过 Hook，会把整个 worker 线程卡死（本地 DoS）；
 * - `Open` 返回的 fd 一定带 `O_CLOEXEC | O_NONBLOCK`：后者防止 `open` 卡在
 *   FIFO 上、根本到不了类型校验；普通文件上 `O_NONBLOCK` 是 no-op，但调用方
 *   若把该 fd 传给期望阻塞语义的代码（如边缘触发 poll），需要知道这一点；
 *   `Adopt` 则**不修改**任何 fd 标志；
 * - `fsync`/`Truncate` 会真实等待磁盘/改变文件长度，热路径上需自行评估。
 */
#pragma once

#include <cstddef>
#include <optional>
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace go2cpp::os {

/**
 * @brief 模块内部使用的校验辅助（**不是公开 API**，不保证跨版本稳定）。
 * @details `std::string` 可以携带内嵌 `'\0'`，而所有 POSIX 路径/名字入口都是
 *          `const char*` + 隐式长度：`c_str()` 会在首个 `'\0'` 处静默截断，
 *          导致「校验的路径」与「实际操作的路径」不一致（路径注入）。
 *          因此所有按路径/名字的公开入口都必须先过这里，返回 `EINVAL`
 *          （与 Go 的 `syscall.BytePtrFromString` 一致）。
 */
namespace detail {

/// @brief 文本中是否含内嵌 NUL（`std::string` 允许，C 字符串不允许）。
inline bool ContainsNul(std::string_view text) noexcept {
    return text.find('\0') != std::string_view::npos;
}

}  // namespace detail

/** @brief `os` 模块的错误码。 */
enum class OsError : std::uint8_t {
    kOk,
    /// 打开/创建失败（`errno` 保留在 `File::last_error()` 或返回值里）。
    kOpenFailed,
    /// 操作失败（读/写/seek/stat）。
    kIoFailed,
    /// 尝试接管一个 socket fd —— 见 file.hpp 1.4，这是高危误用，必须拒绝。
    kIsSocket,
    /**
     * @brief 尝试接管一个**非普通文件**（FIFO、pipe、字符设备、eventfd、
     *        timerfd、signalfd 等）。
     * @details 与 `kIsSocket` 同类风险，但覆盖范围更广。实测 `st_mode & S_IFMT`：
     *
     *          | fd | 类型位 |
     *          |---|---|
     *          | 普通文件 | `S_IFREG` |
     *          | FIFO / pipe | `S_IFIFO` |
     *          | 字符设备 | `S_IFCHR` |
     *          | **eventfd / timerfd / signalfd** | **`0`**（无类型位！）|
     *          | socket | `S_IFSOCK` |
     *
     *          这些 fd 上的 `read` **会真阻塞**（FIFO 无数据、eventfd 未 signal、
     *          timerfd 未到期、`/dev/tty` 无输入），而 `os::File::Read` 走裸
     *          `syscall(SYS_read)` 绕过 Hook —— 结果是**整个 worker 线程卡死**，
     *          等价于一个本地 DoS。
     *
     *          **为什么必须用白名单**：Linux 的 `anon_inodefs`（`eventfd`/
     *          `timerfd`/`signalfd`）**不给 `st_mode` 类型位**（`type_bits == 0`）。
     *          任何基于 `S_ISxxx` 的**黑名单**都会漏掉它们，而它们恰好都是会阻塞
     *          的 fd。所以只能反过来：**只接受 `S_ISREG`**。
     */
    kNotRegularFile,
    /// 无效的 fd / 已关闭的句柄。
    kInvalidHandle,
    /// 参数非法（路径为空、缓冲区为空指针等）。
    kInvalidArgument,
};

/** @brief 错误码转字符串。 */
const char* ToString(OsError error) noexcept;

/** @brief 带错误码的结果。 */
/**
 * ## 定位
 * 被 `File::Open`/`Create`/`Adopt`/`Truncate` 与 `ReadFile`/`WriteFile` 返回；
 * `system_error` 保留 errno，便于上层区分 ENOENT/EACCES/EFBIG 等具体原因。
 *
 * ## 不变量
 * - `code == kOk` 时 `system_error` 无意义（通常为 0）；
 * - `ok()` 与 `operator bool` 只看 `code`，不检查 `system_error`。
 */
struct FileStatus final {
    OsError code{OsError::kOk};  ///< 主错误码；所有返回 FileStatus 的函数写入，ok()/operator bool 只读取它
    /// 失败时的 `errno`（0 表示不适用）。
    int system_error{0};

    bool ok() const noexcept { return code == OsError::kOk; }
    explicit operator bool() const noexcept { return ok(); }
};

/** @brief 打开标志（与 `open(2)` 的 `O_*` 对齐的语义化别名）。 */
enum class OpenFlag : std::uint32_t {
    kReadOnly = 0,
    kWriteOnly = 1,
    kReadWrite = 2,
    kCreate = 64,      // O_CREAT
    kTruncate = 512,   // O_TRUNC
    kAppend = 1024,    // O_APPEND
    kExclusive = 128,  // O_EXCL
};

/** @brief 组合两个打开标志。 */
inline OpenFlag operator|(OpenFlag a, OpenFlag b) noexcept {
    return static_cast<OpenFlag>(static_cast<std::uint32_t>(a) |
                                 static_cast<std::uint32_t>(b));
}

/**
 * @brief RAII 文件句柄。**可移动，不可复制**。
 *
 * @details 析构时 `close(fd)`。读写走 `syscall(SYS_*)` 绕过 Hook ——
 *          理由见本文件顶部的边界结论 1.2 / 1.3。
 *
 * @warning 本类**只接受普通文件**。`Adopt` 一个 socket fd 会返回 `kIsSocket`，
 *          因为绕过 Hook 的读写会让 socket 阻塞整个 M 线程（见 1.4）。
 */
/**
 * ## 定位
 * 面向业务代码的**普通文件**句柄：`config` 之外的配置/日志/资源文件读写可直接
 * 使用；测试见 `tests/os/test_os_file.cpp`。它不参与协程调度，也不与
 * `FdManager` 的 socket 生命周期机制交互。
 *
 * ## 生命周期与线程模型
 * - 创建：`Open`/`Create`/`Adopt` 成功后才持有 fd；默认构造是关闭态；
 * - 销毁：析构调用 `Close()`；`m_owned == false` 时只放弃引用、不关闭 fd；
 * - 线程模型：**非线程安全**，同一个 `File` 不应被多线程并发读写；`ReadAt`/
 *   `WriteAt` 不改变文件偏移，可配合多个句柄做并发读，但需调用方自行同步。
 *
 * ## 状态机
 * 有 Closed / OpenOwned / OpenBorrowed 三个状态，状态转化表见
 * `docs/state-machines/File.md`（触发函数：`Open`/`Adopt`/移动构造/`Close`）。
 *
 * ## 不变量
 * - `m_fd >= 0` 时句柄可用；`m_fd == -1` 时 `m_owned` 必为 false；
 * - `m_owned == true` 时，析构或 `Close` 恰好关闭一次 fd；
 * - 持有有效 fd 的对象只可能是普通文件（`S_ISREG`）；
 * - `m_append == true` 时 `WriteAt` 一律返回 -1/EINVAL（见 `WriteAt` 的 note）。
 */
class File final {
public:
    File() noexcept = default;
    ~File() noexcept;

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

    /**
     * @brief 打开一个文件。
     * @param path 路径。空路径或含内嵌 NUL 的路径返回 `kInvalidArgument`（`EINVAL`）。
     * @param flags 打开标志（可用 `OpenFlag` 的 `|` 组合）。
     * @param mode `O_CREAT` 时使用的权限位。
     *
     * @note 返回的 fd 一定带 `O_CLOEXEC | O_NONBLOCK`：`O_NONBLOCK` 是为了让
     *       `open` 在 FIFO 上不阻塞（否则到不了 `fstat` 类型校验），普通文件上
     *       它是 no-op。调用方若把该 fd 传给期望阻塞语义的代码（如边缘触发的
     *       poll 注册），需要自行知道这一点；`Adopt` 则不修改任何 fd 标志。
     */
    static FileStatus Open(const std::string& path, OpenFlag flags, mode_t mode = 0644,
                           File* out = nullptr);

    /**
     * @brief 创建/截断一个文件（等价 `O_RDWR|O_CREAT|O_TRUNC`）。
     * @details 用 `O_RDWR` 而不是 `O_WRONLY`，与 Go 的 `os.Create` 一致 ——
     *          创建后可以立刻读回校验。若只需要写、并想避免「能读」带来的
     *          意外，用 `Open(path, kWriteOnly | kCreate | kTruncate, ...)`。
     */
    static FileStatus Create(const std::string& path, mode_t mode, File* out);

    /**
     * @brief 接管一个已经打开的 fd。
     * @param take_ownership 为 true 时析构会 `close` 它。
     * @details **会检查 fd 类型并拒绝 socket**（返回 `kIsSocket`）。
     *          不会修改 fd 的任何标志位（见 1.5）。
     */
    static FileStatus Adopt(int fd, bool take_ownership, File* out);

    /**
     * @brief 读最多 `length` 字节（同步阻塞，不参与协程调度）。
     * @return 实际读到的字节数；0 表示 EOF；负值表示失败（`errno` 有效）。
     */
    ssize_t Read(void* buffer, std::size_t length) noexcept;

    /** @brief 在指定偏移读（`pread`），**不改变文件偏移**，适合并发读。 */
    ssize_t ReadAt(void* buffer, std::size_t length, off_t offset) noexcept;

    /**
     * @brief 写最多 `length` 字节（同步阻塞）。
     * @details 这是**一次写**的语义，可能少写；需要写全请用 `WriteAll`。
     * @warning 写入超过 `RLIMIT_FSIZE` / 文件系统配额时，内核会向进程发
     *          `SIGXFSZ`，**默认动作是终止进程** —— 此时 `Write`/`WriteAll`
     *          还没机会返回 `EFBIG`。在限额环境里写文件前应先
     *          `signal(SIGXFSZ, SIG_IGN)`（或捕获），再用短写长度与
     *          `last_error() == EFBIG` 判断。
     */
    ssize_t Write(const void* buffer, std::size_t length) noexcept;

    /**
     * @brief 在指定偏移写（`pwrite`），不改变文件偏移。
     * @return 写入字节数；失败返回 -1（`last_error()` 有效）。
     * @note **`O_APPEND` 句柄上直接失败**（`-1` / `EINVAL`）：POSIX 允许
     *       `pwrite` 在 `O_APPEND` 上忽略 `offset` 并追加到末尾，Linux 至今
     *       如此（`pwrite(2)` BUGS 有记载）。本类选择显式拒绝而不是静默写到
     *       错误位置。需要随机写就打开一个**不带** `kAppend` 的句柄。
     */
    ssize_t WriteAt(const void* buffer, std::size_t length, off_t offset) noexcept;

    /**
     * @brief 循环写直到写完（处理 `EINTR` 与部分写）。
     * @return 写入了多少字节；小于 `length` 表示中途失败。
     * @warning 同 `Write`：超过 `RLIMIT_FSIZE` 会先收到 `SIGXFSZ`（默认终止
     *          进程）。需要在限额下容错时先忽略/捕获该信号。
     */
    std::size_t WriteAll(const void* buffer, std::size_t length) noexcept;

    /**
     * @brief 移动文件偏移（`lseek`）。
     * @return 新的偏移；失败返回 -1（`errno` 有效）。
     * @note `SEEK_END` + 正偏移是合法的（超过 EOF 后再写会形成稀疏文件）。
     */
    off_t Seek(off_t offset, int whence) noexcept;

    /** @brief 当前偏移（`Seek(0, SEEK_CUR)` 的便捷封装）。 */
    off_t Tell() noexcept;

    /**
     * @brief `fstat`。失败返回 false（`errno` 有效）。
     * @note 参数类型写作 `::stat` 是因为本函数在 `namespace go2cpp::os` 内，
     *       不加全局限定符时 `struct stat` 会被解析成 `go2cpp::os::stat`。
     */
    bool Stat(struct ::stat* out) const noexcept;

    /** @brief 文件大小（`fstat` 的 `st_size`）；失败返回 -1。 */
    off_t Size() const noexcept;

    /** @brief 是否是一个普通文件（`S_ISREG`）。 */
    bool IsRegular() const noexcept;

    /**
     * @brief 把该文件的所有已写数据与元数据刷到持久存储（`fsync`）。
     * @return 是否成功；失败原因在 `last_error()`。
     *
     * @details 走 `syscall(SYS_fsync)` —— 与读写路径一致，绕过 Hook。
     *
     * **`fsync` 与 `fdatasync` 的区别**：`fsync` 连元数据（mtime / size /
     * 块映射）一起刷，`fdatasync` 只刷数据。本方法用 `fsync`，因为
     * 「写日志/配置文件后崩溃，重启要能读到完整内容」通常也需要 size 已落盘
     * —— 只刷数据而 size 没落盘的话，崩溃后可能看到一个短文件。
     *
     * @warning `fsync` 会**真实等待磁盘**（可能几十毫秒到几秒）。在 Fiber 里
     *          调用它会阻塞整个 worker 线程 —— 这与本模块其他操作的性质不同
     *          （普通 `read`/`write` 通常几微秒）。如果要在热路径上写文件并
     *          `Sync`，应显式包一层 `Scheduler::BlockingRegion` 让 sysmon
     *          申请替代 M，或把它交给独立的普通线程。
     */
    bool Sync() noexcept;

    /**
     * @brief 把文件截断或扩展到 `size` 字节（`ftruncate`）。
     * @param size 目标大小，必须 `>= 0`；扩展时新增区域读出来是 0。
     * @return 状态。`size < 0` 返回 `kInvalidArgument`（`EINVAL`）；
     *         句柄无效返回 `kInvalidHandle`（`EBADF`）。
     *
     * @details 走 `syscall(SYS_ftruncate)`。注意它**不改变当前文件偏移** ——
     *          若之前偏移在 1 MiB、截断到 4 KiB，后续 `Write` 会在 1 MiB 处
     *          写（文件被重新"扩展"出空洞）。这是 POSIX 语义，不是 bug。
     */
    FileStatus Truncate(off_t size) noexcept;

    /**
     * @brief 手动关闭。可重复调用（幂等）。
     * @warning `close(2)` 阶段的错误（NFS 上的 `EIO`、`ENOSPC`、延迟写回失败等）
     *          **被丢弃**：Linux 上 `close` 遇到 `EINTR` 是"已关闭但状态不确定"，
     *          重试反而可能关掉别人的 fd，而本方法不返回状态。需要确认数据
     *          落盘时，先 `Sync()` 并检查返回值，再 `Close()`。
     *          把它改成返回 `FileStatus` 会破坏现有 API，属待决策项（审计 D7）。
     */
    void Close() noexcept;

    /** @brief 底层 fd；已关闭时返回 -1。 */
    int fd() const noexcept { return m_fd; }

    /** @brief 是否持有有效 fd。 */
    bool valid() const noexcept { return m_fd >= 0; }

    /** @brief 最近一次失败的 `errno`（0 表示没有失败）。 */
    int last_error() const noexcept { return m_last_error; }

private:
    int m_fd{-1};  ///< 持有的 fd，-1 表示关闭；Open/Adopt/移动赋值写入，Close 置 -1，fd()/valid() 读取
    bool m_owned{false};  ///< 是否拥有 fd（析构/Close 负责关闭）；Adopt 的 take_ownership 决定，移动时随 fd 转移
    /// fd 是否带 `O_APPEND`：`Open` 按调用方 flags 记录，`Adopt` 用 `F_GETFL`
    /// 只读探测一次（不修改标志）。`WriteAt` 据此拒绝写入 —— Linux `pwrite`
    /// 在 `O_APPEND` 句柄上会**忽略 offset**、追加到末尾，静默写错位置比报错危险。
    bool m_append{false};
    int m_last_error{0};  ///< 最近一次失败的 errno，0 表示成功；各 IO 函数写入，last_error() 读取
};

// ---------------------------------------------------------------------------
// 便利函数
// ---------------------------------------------------------------------------

/**
 * @brief 整个文件读进 `std::string`。
 * @details 先用 `fstat` 取大小并 reserve，读的过程仍按实际返回追加 ——
 *          因为文件可能在被读期间增长，`st_size` 只作为容量提示。
 *
 *          大小上限 `os.max_file_size`（0 表示不限制）在**读循环内累计检查**：
 *          `st_size==0` 的 procfs 伪文件、读期间被追加的文件都不能绕过；超限
 *          返回 `kIoFailed` + `EFBIG` 且清空 `out`。
 *
 * @note 分配失败（`std::bad_alloc` / `std::length_error`）不会抛出：返回
 *       `kIoFailed` + `ENOMEM`，`out` 为空，与本模块其它错误的报告方式一致。
 * @note 路径是目录时返回 `kNotRegularFile`（由 `File::Open` 的 `S_ISREG`
 *       白名单兜住），不会把目录当文件读。
 */
FileStatus ReadFile(const std::string& path, std::string* out, std::string* error = nullptr);

/**
 * @brief 把 `data` 整体写入文件（创建或截断）。
 * @details 内部用 `WriteAll` 保证写全；写不满返回 `kIoFailed`。
 */
FileStatus WriteFile(const std::string& path, std::string_view data,
                     std::string* error = nullptr);

// ---------------------------------------------------------------------------
// 环境变量
// ---------------------------------------------------------------------------

/**
 * @brief 读环境变量。
 * @return 存在则返回拷贝值的 `std::string`；不存在返回空 optional。
 * @note 名字为空或含内嵌 NUL 时返回空 optional（NUL 会被 `getenv` 静默截断，
 *       按「不存在」处理，避免读到另一个变量的值）。
 * @details **必须拷贝**：`getenv` 返回的是 libc 内部缓冲区的指针，
 *          另一个线程 `setenv` 后该指针可能失效 —— 直接返回 `const char*`
 *          会让调用方拿到悬空指针。本函数返回拷贝，避免该竞态。
 */
std::optional<std::string> Getenv(const std::string& name);

/**
 * @brief 设环境变量。
 * @param overwrite 为 false 时，已存在则不覆盖。
 * @return 名字/值含内嵌 NUL、名字为空或含 `'='` 时返回 `kInvalidArgument`（`EINVAL`）。
 * @note `setenv` 本身不是线程安全的（会动 `environ`）；本函数不加锁，
 *       调用方需自行保证不在多线程下并发修改环境变量。
 */
FileStatus Setenv(const std::string& name, const std::string& value, bool overwrite = true,
                  std::string* error = nullptr);

/**
 * @brief 删除环境变量。
 * @return 名字为空或含内嵌 NUL 时返回 `kInvalidArgument`（`EINVAL`）。
 */
FileStatus Unsetenv(const std::string& name, std::string* error = nullptr);

/**
 * @brief 返回当前全部环境变量（`key=value` 拆开后的拷贝）。
 * @details 遍历 `environ` 时同样有竞态风险；只适合在单线程初始化阶段调用。
 */
std::vector<std::pair<std::string, std::string>> Environ();

// ---------------------------------------------------------------------------
// 命令行参数
// ---------------------------------------------------------------------------

/**
 * @brief 在 `main` 开头调用一次，保存 `argc/argv` 的**拷贝**。
 * @details 拷贝而不是保存 `argv` 指针：`argv` 指向的内存归宿主所有，
 *          在某些嵌入场景下可能在 `main` 返回前被释放。
 *          可重复调用（覆盖旧值），但**不是线程安全的**。
 */
void SetArgs(int argc, char** argv);

/** @brief 返回保存的参数列表（含程序名）。未调用 `SetArgs` 时为空。 */
const std::vector<std::string>& Args();

/** @brief 返回程序名（`Args()[0]`）；为空时返回空字符串。 */
const std::string& ProgramName();

}  // namespace go2cpp::os
