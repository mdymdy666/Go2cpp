/**
 * @file raw_io.cpp
 * @brief 裸系统调用封装的实现。
 *
 * 这些函数只做「syscall + 错误返回」，不含重试/超时逻辑（由调用点统一处理），
 * 也不分配内存（`fork` 之后的子进程可以安全调用）。
 */
#include "raw_io.hpp"

#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace go2cpp::os::detail {

ssize_t RawRead(int fd, void* buffer, std::size_t count) noexcept {
    return static_cast<ssize_t>(::syscall(SYS_read, fd, buffer, count));
}

ssize_t RawWrite(int fd, const void* buffer, std::size_t count) noexcept {
    return static_cast<ssize_t>(::syscall(SYS_write, fd, buffer, count));
}

int RawClose(int fd) noexcept {
    return static_cast<int>(::syscall(SYS_close, fd));
}

int RawPoll(struct pollfd* descriptors, nfds_t count, int timeout_ms) noexcept {
    return static_cast<int>(::syscall(SYS_poll, descriptors, count, timeout_ms));
}

int RawPpoll(struct pollfd* descriptors, nfds_t count, const struct timespec* timeout,
             const void* sigmask) noexcept {
    return static_cast<int>(
        ::syscall(SYS_ppoll, descriptors, count, timeout, sigmask, sizeof(sigset_t)));
}

int RawFcntl(int fd, int command, long argument) noexcept {
    return static_cast<int>(::syscall(SYS_fcntl, fd, command, argument));
}

int RawDup(int fd) noexcept {
    return static_cast<int>(::syscall(SYS_dup, fd));
}

int RawDup2(int old_fd, int new_fd) noexcept {
    return static_cast<int>(::syscall(SYS_dup2, old_fd, new_fd));
}

int RawDup3(int old_fd, int new_fd, int flags) noexcept {
#ifdef SYS_dup3
    return static_cast<int>(::syscall(SYS_dup3, old_fd, new_fd, flags));
#else
    (void)old_fd;
    (void)new_fd;
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

int RawPipe2(int fds[2], int flags) noexcept {
#ifdef SYS_pipe2
    return static_cast<int>(::syscall(SYS_pipe2, fds, flags));
#else
    (void)fds;
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

int RawSocketPair(int address_family, int type, int protocol, int fds[2]) noexcept {
    return static_cast<int>(
        ::syscall(SYS_socketpair, address_family, type, protocol, fds));
}

int RawCloseRange(unsigned int first, unsigned int last, unsigned int flags) noexcept {
#ifdef SYS_close_range
    return static_cast<int>(::syscall(SYS_close_range, first, last, flags));
#else
    (void)first;
    (void)last;
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

int RawNanosleep(long seconds, long nanoseconds) noexcept {
    struct timespec request {};
    request.tv_sec = static_cast<time_t>(seconds);
    request.tv_nsec = nanoseconds;
    return static_cast<int>(::syscall(SYS_nanosleep, &request, nullptr));
}

bool SetNonBlocking(int fd) noexcept {
    const int flags = RawFcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return true;
    }
    return RawFcntl(fd, F_SETFL, static_cast<long>(flags | O_NONBLOCK)) == 0;
}

bool SetBlocking(int fd) noexcept {
    const int flags = RawFcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    if ((flags & O_NONBLOCK) == 0) {
        return true;
    }
    return RawFcntl(fd, F_SETFL, static_cast<long>(flags & ~O_NONBLOCK)) == 0;
}

bool SetCloseOnExec(int fd, bool enabled) noexcept {
    const int flags = RawFcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        return false;
    }
    const int next = enabled ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
    return RawFcntl(fd, F_SETFD, static_cast<long>(next)) == 0;
}

}  // namespace go2cpp::os::detail
