/**
 * @file io_capability.hpp
 * @brief IO 能力描述与「是否交给 GMP 异步」的显式开关（R27-R30）。
 *
 * ## 背景：为什么需要能力描述
 *
 * POSIX 的 fd 在「能否非阻塞 / 能否注册 epoll / 该不该 park Fiber」上分成
 * 三类，而 C++ 类型系统不区分它们：
 *
 * | fd 类型 | `O_NONBLOCK` | epoll | 正确用法 |
 * |---|---|---|---|
 * | 普通文件 | 无意义（内核同步完成） | **不可注册** | 原生阻塞 |
 * | pipe / socketpair / socket | 有意义 | 可注册 | 默认阻塞；可显式异步 |
 * | TTY / 字符设备 / eventfd… | 视驱动 | 多数可注册 | 默认阻塞；按需异步 |
 *
 * 把普通文件塞进 `epoll` 会得到 `EPERM`；把普通文件设为非阻塞再 `read` 仍然
 * 同步返回 —— 所以「统一异步化」在这里是错误抽象。本文件提供**可查询的能力
 * 描述**，让调用方在运行前就能知道某条路径该走哪种模型，而不是靠 try/except
 * 或者「先设非阻塞再看有没有 EAGAIN」猜。
 *
 * ## 默认策略（安全第一）
 *
 * - **默认原生阻塞**：`File`/`FdChannel`/`Pipe`/`SocketPair` 构造后都是
 *   `native_blocking == true`，行为与 Go 的 `os.File` 一致；
 * - 用户显式调用 `EnableAsync(io_manager)` 才把等待交给 `IOManager`（GMP 的
 *   epoll 驱动），此时 Fiber 会 park、M 被释放；
 * - 能力不支持时 `EnableAsync` 返回 `kUnsupported` + 明确 `reason`，
 *   **不会静默降级**成阻塞（那会让调用方误以为协程友好）。
 *
 * ## 与 Go 的对应
 *
 * | Go | 本模块 |
 * |---|---|
 * | `os.File`（普通文件，同步） | `File`：`async_supported=false` |
 * | `net.Conn` / `netpoller` | `FdChannel::EnableAsync`（socket/pipe） |
 * | `internal/poll.FD.pollable` | `IoCapability::pollable` |
 * | `runtime_pollWait` | `AsyncIoBinding::WaitReadable/WaitWritable` |
 * | `syscall.EnterSyscall` | `native_blocking=true`（阻塞 M，不 park Fiber） |
 *
 * ## 与 hook 的关系
 *
 * `EnableAsync` 走 `IOManager::wait_for`（epoll）；`FdChannel` 在**同步**模式
 * 下的 `poll/read` 仍走裸 syscall（不经过 hook）。两条路径显式分开，不会出现
 * 「以为在异步模式、实际被 hook 接管」或相反的情况。
 */
#pragma once

#include "go2cpp/os/proc.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace go2cpp::io {
class IOManager;
}

namespace go2cpp::os {

/** @brief fd 的粗分类（来自 `fstat` 的 `S_IFMT` + `isatty`）。 */
enum class IoKind : std::uint8_t {
    kRegularFile,     ///< `S_ISREG`：普通文件
    kDirectory,       ///< `S_ISDIR`
    kFifo,            ///< `S_ISFIFO`：pipe/FIFO
    kSocket,          ///< `S_ISSOCK`
    kCharacterDevice, ///< `S_ISCHR`：TTY、/dev/null、/dev/urandom 等
    kBlockDevice,     ///< `S_ISBLK`
    kAnonInode,       ///< 无类型位：eventfd/timerfd/signalfd 等 anon_inode
    kUnknown,         ///< 探测失败或 fd 无效
};

/** @brief fd 分类转文本。 */
const char* ToString(IoKind kind) noexcept;

/**
 * @brief 一条 IO 路径的能力描述。
 *
 * @details 字段含义：
 * - `pollable`：能否注册到 epoll（`epoll_ctl` 不会返回 `EPERM`）；
 * - `can_nonblocking`：`O_NONBLOCK` 是否有实际语义；
 * - `native_blocking`：默认模型是否阻塞调用线程（普通文件恒为 true）；
 * - `async_supported`：能否交给 `IOManager`（`pollable && can_nonblocking`
 *   且本模块实现了该后端的异步等待）；
 * - `reason`：当上述某项为 false 时的**人话原因**（例如
 *   `"regular file: 内核同步完成，无 EAGAIN，epoll 不可注册"`），
 *   绝不留空让调用方猜。
 */
struct IoCapability final {
    bool pollable{false};
    bool can_nonblocking{false};
    bool native_blocking{true};
    bool async_supported{false};
    bool is_regular_file{false};
    bool is_tty{false};
    bool is_socket{false};
    bool is_pipe{false};
    IoKind kind{IoKind::kUnknown};
    std::string reason;  ///< 不支持异步/非阻塞的原因；支持时为空
};

/**
 * @brief 探测一个 fd 的能力。
 * @details 用 `fstat` 的 `S_IFMT` 分类，再叠加 `isatty`；`getsockopt(SO_TYPE)`
 *          只用于确认 socket（`S_ISSOCK` 已足够，保留它作为交叉校验）。
 * @return fd < 0 或 `fstat` 失败时返回 `kUnknown` + reason（不抛异常）。
 */
IoCapability DetectIoCapability(int fd);

/**
 * @brief 设置/清除 `O_NONBLOCK`（不校验能力，纯系统调用封装）。
 * @return 失败返回 `kIoFailed` + errno。
 * @note 能力不允许非阻塞时应先查 `can_nonblocking`；`File::SetNonBlocking`
 *       等对象方法会替调用方做这层校验并给出 `kUnsupported`。
 */
Result<void> SetFdNonBlocking(int fd, bool enable);

/**
 * @brief 把一个 fd 的等待显式绑定到 `IOManager`（R28）。
 *
 * @details 生命周期：
 * - `Enable` 记录原阻塞状态、把 fd 设为非阻塞、保存 manager 指针；
 * - `WaitReadable/WaitWritable` 只能在 **该 manager 管理的 Fiber** 中调用；
 *   普通线程调用会返回 `kUnsupported`（`IOManager::current()` 为空），
 *   不会静默退回阻塞 poll；
 * - `Disable` 恢复原阻塞状态并清空绑定（幂等）。
 *
 * `IOManager` 的生命周期必须覆盖本对象；本类不拥有它。
 */
class AsyncIoBinding final {
public:
    AsyncIoBinding() = default;

    /**
     * @brief 启用异步等待。
     * @param fd 目标 fd。
     * @param capability 该 fd 的能力（调用方通常来自 `DetectIoCapability`）。
     * @param manager IOManager；不能为空。
     * @return 不可 poll/不可非阻塞/manager 为空时返回 `kUnsupported`/`kInvalidArgument`，
     *         并在 message 里带上 `capability.reason`。
     */
    Result<void> Enable(int fd, const IoCapability& capability,
                        go2cpp::io::IOManager* manager);

    /// 恢复启用前的阻塞状态并解绑；未启用时为空操作。
    Result<void> Disable();

    bool enabled() const noexcept { return m_enabled.load(std::memory_order_acquire); }
    go2cpp::io::IOManager* manager() const noexcept {
        return m_manager.load(std::memory_order_acquire);
    }
    int fd() const noexcept { return m_fd.load(std::memory_order_acquire); }

    /// 等待可读；`timeout < 0` 表示永久（依赖 Context 取消）。
    Result<void> WaitReadable(std::chrono::milliseconds timeout);
    /// 等待可写。
    Result<void> WaitWritable(std::chrono::milliseconds timeout);

private:
    Result<void> Wait(bool read, std::chrono::milliseconds timeout);

    /// 保护 Enable/Disable 的复合状态（并发调用安全；Wait 只做快照）。
    mutable std::mutex m_mutex;
    std::atomic<go2cpp::io::IOManager*> m_manager{nullptr};
    std::atomic<int> m_fd{-1};
    std::atomic<bool> m_enabled{false};
    bool m_was_blocking{true};  ///< Enable 前的 O_NONBLOCK 状态，Disable 恢复
};

}  // namespace go2cpp::os
