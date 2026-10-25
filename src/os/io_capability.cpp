/**
 * @file io_capability.cpp
 * @brief IO 能力探测与 `IOManager` 异步绑定的实现。
 */
#include "go2cpp/os/io_capability.hpp"

#include "raw_io.hpp"

#include "go2cpp/network/io.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace go2cpp::os {

namespace {

constexpr const char* kRegularReason =
    "regular file: 内核同步完成、不会 EAGAIN，epoll 不可注册（与 Go os.File 一致）";
constexpr const char* kDirectoryReason = "directory: 目录不能 read/write，也不可 poll";
constexpr const char* kBlockReason = "block device: 异步/非阻塞语义由驱动决定，本模块不接管";
constexpr const char* kCharReason =
    "character device: 可 poll 但异步语义取决于驱动，本模块只提供同步模式";
constexpr const char* kAnonReason = "anon inode: eventfd/timerfd/signalfd 等，可 poll";

}  // namespace

const char* ToString(IoKind kind) noexcept {
    switch (kind) {
        case IoKind::kRegularFile: return "regular_file";
        case IoKind::kDirectory: return "directory";
        case IoKind::kFifo: return "fifo";
        case IoKind::kSocket: return "socket";
        case IoKind::kCharacterDevice: return "character_device";
        case IoKind::kBlockDevice: return "block_device";
        case IoKind::kAnonInode: return "anon_inode";
        case IoKind::kUnknown: return "unknown";
    }
    return "unknown";
}

IoCapability DetectIoCapability(int fd) {
    IoCapability capability;
    if (fd < 0) {
        capability.kind = IoKind::kUnknown;
        capability.reason = "fd 无效";
        return capability;
    }
    struct ::stat info {};
    if (::fstat(fd, &info) != 0) {
        capability.kind = IoKind::kUnknown;
        capability.reason = std::string("fstat 失败: ") + std::strerror(errno);
        return capability;
    }
    const mode_t type = info.st_mode & S_IFMT;
    if (type == S_IFREG) {
        capability.kind = IoKind::kRegularFile;
        capability.is_regular_file = true;
        capability.pollable = false;
        capability.can_nonblocking = false;
        capability.native_blocking = true;
        capability.async_supported = false;
        capability.reason = kRegularReason;
        return capability;
    }
    if (type == S_IFDIR) {
        capability.kind = IoKind::kDirectory;
        capability.native_blocking = true;
        capability.reason = kDirectoryReason;
        return capability;
    }
    if (type == S_IFIFO) {
        capability.kind = IoKind::kFifo;
        capability.is_pipe = true;
        capability.pollable = true;
        capability.can_nonblocking = true;
        capability.native_blocking = true;
        capability.async_supported = true;
        return capability;
    }
    if (type == S_IFSOCK) {
        capability.kind = IoKind::kSocket;
        capability.is_socket = true;
        capability.pollable = true;
        capability.can_nonblocking = true;
        capability.native_blocking = true;
        capability.async_supported = true;
        return capability;
    }
    if (type == S_IFCHR) {
        capability.kind = IoKind::kCharacterDevice;
        capability.is_tty = ::isatty(fd) == 1;
        capability.pollable = true;
        capability.can_nonblocking = true;
        capability.native_blocking = true;
        // TTY/字符设备的可 poll 与异步语义依赖驱动；本模块只开放同步模式，
        // 需要异步的调用方可以自行把 fd 交给 IOManager（文档给出做法）。
        capability.async_supported = false;
        capability.reason = kCharReason;
        return capability;
    }
    if (type == S_IFBLK) {
        capability.kind = IoKind::kBlockDevice;
        capability.native_blocking = true;
        capability.reason = kBlockReason;
        return capability;
    }
    // anon_inodefs（eventfd/timerfd/signalfd）不给类型位：type == 0。
    // 这正是 file.hpp 1.4 节强调的「黑名单必漏」的情况：只能显式识别。
    capability.kind = IoKind::kAnonInode;
    capability.pollable = true;
    capability.can_nonblocking = true;
    capability.native_blocking = true;
    capability.async_supported = true;
    capability.reason = kAnonReason;
    return capability;
}

Result<void> SetFdNonBlocking(int fd, bool enable) {
    if (fd < 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EBADF, "fd 无效", "SetFdNonBlocking");
    }
    const int flags = static_cast<int>(::syscall(SYS_fcntl, fd, F_GETFL, 0L));
    if (flags < 0) {
        return MakeErrnoError(ProcErrorCode::kIoFailed, errno, "fcntl(F_GETFL)");
    }
    int next = flags;
    if (enable) {
        next |= O_NONBLOCK;
    } else {
        next &= ~O_NONBLOCK;
    }
    if (next == flags) {
        return OkResult();
    }
    if (::syscall(SYS_fcntl, fd, F_SETFL, static_cast<long>(next)) < 0) {
        return MakeErrnoError(ProcErrorCode::kIoFailed, errno, "fcntl(F_SETFL)");
    }
    return OkResult();
}

Result<void> AsyncIoBinding::Enable(int fd, const IoCapability& capability,
                                    go2cpp::io::IOManager* manager) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_enabled.load(std::memory_order_acquire)) {
        return MakeError(ProcErrorCode::kInvalidArgument, 0, "异步模式已启用", "AsyncIoBinding::Enable");
    }
    if (fd < 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EBADF, "fd 无效", "AsyncIoBinding::Enable");
    }
    if (manager == nullptr) {
        return MakeError(ProcErrorCode::kInvalidArgument, 0, "IOManager 为空",
                         "AsyncIoBinding::Enable");
    }
    if (!capability.pollable || !capability.async_supported) {
        return MakeError(ProcErrorCode::kUnsupported, 0,
                         "fd 不支持异步等待: " + capability.reason, "AsyncIoBinding::Enable");
    }
    if (!capability.can_nonblocking) {
        return MakeError(ProcErrorCode::kUnsupported, 0,
                         "fd 不支持非阻塞: " + capability.reason, "AsyncIoBinding::Enable");
    }
    const int flags = static_cast<int>(::syscall(SYS_fcntl, fd, F_GETFL, 0L));
    if (flags < 0) {
        return MakeErrnoError(ProcErrorCode::kIoFailed, errno, "fcntl(F_GETFL)");
    }
    m_was_blocking = (flags & O_NONBLOCK) == 0;
    Result<void> nonblocking = SetFdNonBlocking(fd, true);
    if (!nonblocking.ok()) {
        return nonblocking.error();
    }
    m_fd.store(fd, std::memory_order_release);
    m_manager.store(manager, std::memory_order_release);
    m_enabled.store(true, std::memory_order_release);
    return OkResult();
}

Result<void> AsyncIoBinding::Disable() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_enabled.load(std::memory_order_acquire)) {
        return OkResult();
    }
    Result<void> restored = SetFdNonBlocking(m_fd.load(std::memory_order_acquire),
                                             !m_was_blocking);
    m_enabled.store(false, std::memory_order_release);
    m_manager.store(nullptr, std::memory_order_release);
    m_fd.store(-1, std::memory_order_release);
    return restored;
}

Result<void> AsyncIoBinding::WaitReadable(std::chrono::milliseconds timeout) {
    return Wait(true, timeout);
}

Result<void> AsyncIoBinding::WaitWritable(std::chrono::milliseconds timeout) {
    return Wait(false, timeout);
}

Result<void> AsyncIoBinding::Wait(bool read, std::chrono::milliseconds timeout) {
    go2cpp::io::IOManager* manager = m_manager.load(std::memory_order_acquire);
    const int fd = m_fd.load(std::memory_order_acquire);
    if (!m_enabled.load(std::memory_order_acquire) || manager == nullptr) {
        return MakeError(ProcErrorCode::kUnsupported, 0, "异步模式未启用",
                         "AsyncIoBinding::Wait");
    }
    // IOManager::wait 只允许在它管理的 Fiber 中调用；普通线程里 current()
    // 为空，此时必须明确失败，不能悄悄退回阻塞 poll。
    if (go2cpp::io::IOManager::current() != manager) {
        return MakeError(ProcErrorCode::kUnsupported, EPERM,
                         "异步等待必须在绑定它的 IOManager 管理的 Fiber 中调用",
                         "AsyncIoBinding::Wait");
    }
    const go2cpp::io::IOEvent event =
        read ? go2cpp::io::IOEvent::kRead : go2cpp::io::IOEvent::kWrite;
    go2cpp::io::WaitResult result;
    if (timeout.count() < 0) {
        result = manager->wait(fd, event);
    } else {
        result = manager->wait_for(fd, event,
                                   std::chrono::duration_cast<go2cpp::io::IOManager::Duration>(
                                       timeout));
    }
    switch (result.status) {
        case go2cpp::io::WaitStatus::kReady:
            return OkResult();
        case go2cpp::io::WaitStatus::kTimeout:
            return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待 fd 就绪超时",
                             "AsyncIoBinding::Wait");
        case go2cpp::io::WaitStatus::kClosed:
            return MakeError(ProcErrorCode::kClosed, EBADF, "fd 已关闭", "AsyncIoBinding::Wait");
        case go2cpp::io::WaitStatus::kCancelled:
            return MakeError(ProcErrorCode::kChannelClosed, ECANCELED, "等待被取消",
                             "AsyncIoBinding::Wait");
        case go2cpp::io::WaitStatus::kError:
            return MakeError(ProcErrorCode::kIoFailed, result.system_error,
                             "IOManager 等待失败", "AsyncIoBinding::Wait");
    }
    return MakeError(ProcErrorCode::kInternal, 0, "未知等待状态", "AsyncIoBinding::Wait");
}

}  // namespace go2cpp::os
