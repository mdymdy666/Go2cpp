/**
 * @file raw_io.hpp
 * @brief `os` 进程/通道子层的**裸系统调用**内部封装（不对外安装）。
 *
 * ## 为什么需要这一层
 *
 * `go2cpp_hook` 会拦截 `read`/`write`/`close`/`poll`/`fcntl`/`dup` 等 libc
 * 符号，并在 Fiber 中 park 当前 Fiber。`os` 的定位是「阻塞调用不放进 Fiber」，
 * 因此本模块内部**一律**走 `::syscall(SYS_*)`：
 *
 * - 等价于 hook 提供的 `read_f`/`write_f`/`close_f`/`poll_f`，但**不依赖**
 *   `go2cpp_hook` 共享库，Hook-off 构建同样可用（`src/os/file.cpp` 是同一先例）；
 * - `fork` 之后只能调用 async-signal-safe 函数，裸 syscall 不会碰 libc 锁。
 *
 * ## 边界
 *
 * - 只做「把 errno 变成可判断的返回值」这一件事，不分配内存、不打日志；
 * - `Poll` 在 `EINTR` 时返回 -1 并保留 errno，由调用方决定是否重试（本模块
 *   统一在调用点重试，避免把 EINTR 泄漏给上层）；
 * - 所有函数都在 `go2cpp::os::detail` 下，属于实现细节，不承诺 ABI。
 */
#pragma once

#include <poll.h>
#include <sys/types.h>

#include <cstddef>

namespace go2cpp::os::detail {

/// `SYS_read` 裸调用；返回 -1 时 `errno` 有效。
ssize_t RawRead(int fd, void* buffer, std::size_t count) noexcept;

/// `SYS_write` 裸调用。
ssize_t RawWrite(int fd, const void* buffer, std::size_t count) noexcept;

/// `SYS_close` 裸调用；不触发 hook 的 close 通知（进程 fd 不归 FdManager 管）。
int RawClose(int fd) noexcept;

/// `SYS_poll` 裸调用。
int RawPoll(struct pollfd* descriptors, nfds_t count, int timeout_ms) noexcept;

/// `SYS_ppoll` 裸调用；`sigmask` 传 nullptr 表示不修改信号掩码。
int RawPpoll(struct pollfd* descriptors, nfds_t count, const struct timespec* timeout,
             const void* sigmask) noexcept;

/// `SYS_fcntl` 裸调用（可变参数按 long 传递）。
int RawFcntl(int fd, int command, long argument) noexcept;

/// `SYS_dup` 裸调用。
int RawDup(int fd) noexcept;

/// `SYS_dup2` 裸调用。
int RawDup2(int old_fd, int new_fd) noexcept;

/// `SYS_dup3` 裸调用（支持 O_CLOEXEC）。
int RawDup3(int old_fd, int new_fd, int flags) noexcept;

/// `SYS_pipe2` 裸调用。
int RawPipe2(int fds[2], int flags) noexcept;

/// `SYS_socketpair` 裸调用；`address_family`/`type`/`protocol` 与 `socketpair(2)` 一致。
int RawSocketPair(int address_family, int type, int protocol, int fds[2]) noexcept;

/// `SYS_close_range` 裸调用；返回 -1 且 errno == ENOSYS 表示内核不支持。
int RawCloseRange(unsigned int first, unsigned int last, unsigned int flags) noexcept;

/// `SYS_nanosleep` 裸调用（用于超时轮询）。
int RawNanosleep(long seconds, long nanoseconds) noexcept;

/** @brief 把 fd 设为非阻塞（保留其他标志位）。 */
bool SetNonBlocking(int fd) noexcept;

/** @brief 把 fd 设为阻塞。 */
bool SetBlocking(int fd) noexcept;

/** @brief 设置或清除 `FD_CLOEXEC`。 */
bool SetCloseOnExec(int fd, bool enabled) noexcept;

}  // namespace go2cpp::os::detail
