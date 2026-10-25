/**
 * @file pty.hpp
 * @brief 伪终端（`posix_openpt`/`grantpt`/`unlockpt`/`ptsname_r`）封装（R11，R31 异步）。
 *
 * ## 为什么需要 PTY
 *
 * 有些程序（`bash` 交互模式、`ros2` 的部分命令、`top`、`ssh`）会检测
 * `isatty(STDIN_FILENO)`：不是 TTY 就改变输出格式（无颜色、无进度条），甚至
 * 直接拒绝运行。管道无法伪装成 TTY，必须用 PTY。
 *
 * ## 语义与边界
 *
 * - PTY 是**双向**的：master 既可读（子进程输出）也可写（子进程输入）；
 *   `Process` 在 `use_pty = true` 时把 stdin/stdout 都指向同一个 master，
 *   stderr 也合并到 master（TTY 只有一个流）。
 * - **回显**：默认关闭（`echo = false`），否则父进程写进去的命令会被
 *   master 再读回来，造成「自己读到自己的输入」。需要交互式 shell 时再打开。
 * - **`EIO`**：从端全部关闭后，master 的 `read` 返回 `EIO`（不是 EOF）。
 *   `FdChannel::Pull` 把 `EIO` 映射为 EOF，因此 `Process` 的 stdout 通道在
 *   PTY 模式下会在子进程退出后正常返回 `kClosed`，不会把 `EIO` 当错误。
 *   本类新增的 `Read` 使用同一映射。
 * - 窗口大小通过 `TIOCSWINSZ` 设置；`Resize` 可在运行中调用（会向从端发送
 *   `SIGWINCH`）。
 * - 平台不支持时 `Open` 返回 `kPtyFailed`（`errno` 说明原因），调用方可以
 *   降级到管道；本模块不做编译期开关（POSIX 平台恒有该接口）。
 *
 * ## 异步等待（R31）
 *
 * master 是 `S_IFCHR` 字符设备：`poll`/`epoll` 可注册，`O_NONBLOCK` 下
 * `read/write` 返回 `EAGAIN`（对端关闭时 `read` 返回 `EIO`）。因此本类在
 * **显式** `EnableAsync(manager)` 之后把 master 的读/写等待交给 `IOManager`
 * （Fiber park，M 被释放）；默认仍是原生阻塞。
 *
 * `Pty::Capability()` 返回的是 **master** 的能力，并显式把
 * `async_supported` 置 true —— `DetectIoCapability()` 对通用字符设备保持
 * 保守（`false`，语义取决于驱动），这里能开放异步是因为 PTY 驱动的行为由
 * 内核保证、且本模块只用它做字节流读写。
 */
#pragma once

#include "go2cpp/os/channel.hpp"
#include "go2cpp/os/proc.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

namespace go2cpp::os {

/**
 * @brief PTY 主/从端 RAII 句柄。
 *
 * @details 从端 fd（`slave_fd`）通常在 `Spawn` 前交给子进程 `dup2`；父进程
 *          只用 master。对象析构关闭仍持有的 fd；`Release*` 放弃所有权。
 *
 *          默认所有读写都是原生阻塞；显式 `EnableAsync` 后才 park Fiber。
 */
class Pty final {
public:
    Pty() noexcept = default;
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;
    Pty(Pty&& other) noexcept;
    Pty& operator=(Pty&& other) noexcept;

    /**
     * @brief 创建一对 PTY。
     * @param columns 初始列数（>0）。
     * @param rows 初始行数（>0）。
     * @param echo 是否打开回显（默认 false，见文件头）。
     * @param out 成功时写入句柄。
     */
    static Result<void> Open(int columns, int rows, bool echo, Pty* out);

    int master_fd() const noexcept { return m_master; }
    int slave_fd() const noexcept { return m_slave; }
    const std::string& slave_name() const noexcept { return m_slave_name; }
    bool valid() const noexcept { return m_master >= 0 && m_slave >= 0; }

    /// 设置窗口大小（`TIOCSWINSZ`）。
    Result<void> Resize(int columns, int rows);

    /// 开关从端回显（`tcsetattr`）。
    Result<void> SetEcho(bool enabled);

    int ReleaseMaster() noexcept;
    int ReleaseSlave() noexcept;
    void CloseMaster() noexcept;
    void CloseSlave() noexcept;

    // ---- 读写与显式异步（R31）------------------------------------------

    /**
     * @brief master 端的能力描述（不是 `DetectIoCapability` 的通用 TTY 结论）。
     * @details master 恒为可 poll、可非阻塞、支持异步；未打开时返回
     *          `kUnknown` + reason。
     */
    IoCapability Capability() const;

    /// 设置/清除 master 的 `O_NONBLOCK`；失败返回 `kIoFailed` + errno。
    Result<void> SetNonBlocking(bool enable);

    /**
     * @brief 把 master 的读/写等待显式交给 `IOManager`。
     * @details 会记录原阻塞状态、设置 `O_NONBLOCK` 并保存 manager 指针。
     *          重复调用返回 `kInvalidArgument`；未打开返回 `kNotRunning`。
     * @note `IOManager` 的生命周期必须覆盖本对象与所有异步调用。
     */
    Result<void> EnableAsync(go2cpp::io::IOManager* manager);

    /// 恢复 Enable 前的阻塞状态并解绑（幂等）。
    Result<void> DisableAsync();

    bool async_enabled() const noexcept;

    /**
     * @brief 从 master 读一段字节。
     * @param timeout 负值表示永久等待；0 表示先做一次非阻塞尝试，无数据立即
     *        返回 `kTimeout`。
     * @return 对端（从端）全部关闭时返回 `kClosed`（`EIO` 已映射）。
     */
    Result<std::size_t> Read(MutableByteSpan buffer,
                             std::chrono::milliseconds timeout);

    /**
     * @brief 向 master 写一段字节（内部处理 EINTR/部分写/超时）。
     * @return 从端关闭时返回 `kChannelClosed`（`EPIPE`）。
     */
    Result<void> Write(ByteSpan data, std::chrono::milliseconds timeout);

private:
    /// 等待 master 可读/可写；按当前模式走 IOManager 或裸 poll。
    Result<void> WaitFor(bool readable, std::chrono::milliseconds timeout);

    int m_master{-1};
    int m_slave{-1};
    std::string m_slave_name;

    /// 取异步绑定快照；调用方持有返回的 `shared_ptr` 期间绑定不会被销毁。
    std::shared_ptr<AsyncIoBinding> async_binding() const;

    /// 保护 `m_async` 指针本身：`EnableAsync` 与 `Read`/`Write` 可能来自
    /// 不同线程/Fiber。绑定内部状态由 `AsyncIoBinding` 自己的原子量 +
    /// 互斥量保护，这里只保护指针读写。
    mutable std::mutex m_async_mutex;
    std::shared_ptr<AsyncIoBinding> m_async;
};

}  // namespace go2cpp::os
