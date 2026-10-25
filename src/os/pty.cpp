/**
 * @file pty.cpp
 * @brief PTY 封装实现（R11；R31 增加显式异步等待）。
 */
#include "go2cpp/os/pty.hpp"

#include "raw_io.hpp"

#include "go2cpp/network/io.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace go2cpp::os {

namespace {

constexpr std::chrono::milliseconds kForever{-1};

/// 剩余超时；`timeout < 0` 表示永久（返回 -1ms）。
std::chrono::milliseconds Remaining(std::chrono::steady_clock::time_point start,
                                    std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        return kForever;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (elapsed >= timeout) {
        return std::chrono::milliseconds(0);
    }
    return timeout - elapsed;
}

/// 相对毫秒 -> `poll` 的 int 超时；上限 24h，避免 long long 溢出。
int PollTimeout(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        return -1;
    }
    constexpr long long kMaxPollTimeout = 24LL * 60 * 60 * 1000;
    return static_cast<int>(std::min<long long>(timeout.count(), kMaxPollTimeout));
}

/// `poll` 一个 fd，自动重试 EINTR；返回 1/0/-1（-1 时 errno 有效）。
int PollOnce(int fd, short events, std::chrono::milliseconds timeout) {
    struct pollfd descriptor {};
    descriptor.fd = fd;
    descriptor.events = events;
    for (;;) {
        const int ready = detail::RawPoll(&descriptor, 1, PollTimeout(timeout));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        return ready;
    }
}

}  // namespace

Pty::~Pty() {
    // 先解绑（恢复 O_NONBLOCK 需要 fd 仍有效），再关闭。
    (void)DisableAsync();
    CloseMaster();
    CloseSlave();
}

Pty::Pty(Pty&& other) noexcept
    : m_master(other.m_master), m_slave(other.m_slave),
      m_slave_name(std::move(other.m_slave_name)) {
    {
        std::lock_guard<std::mutex> lock(other.m_async_mutex);
        m_async = std::move(other.m_async);
    }
    other.m_master = -1;
    other.m_slave = -1;
    other.m_slave_name.clear();
}

Pty& Pty::operator=(Pty&& other) noexcept {
    if (this != &other) {
        (void)DisableAsync();
        CloseMaster();
        CloseSlave();
        m_master = other.m_master;
        m_slave = other.m_slave;
        m_slave_name = std::move(other.m_slave_name);
        {
            std::lock_guard<std::mutex> lock(other.m_async_mutex);
            m_async = std::move(other.m_async);
        }
        other.m_master = -1;
        other.m_slave = -1;
        other.m_slave_name.clear();
    }
    return *this;
}

Result<void> Pty::Open(int columns, int rows, bool echo, Pty* out) {
    if (out == nullptr) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "输出指针为空",
                         "Pty::Open");
    }
    if (columns <= 0 || rows <= 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "PTY 行列必须为正",
                         "Pty::Open");
    }
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        return MakeErrnoError(ProcErrorCode::kPtyFailed, errno, "posix_openpt");
    }
    if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        const int error = errno;
        (void)detail::RawClose(master);
        return MakeErrnoError(ProcErrorCode::kPtyFailed, error, "grantpt/unlockpt");
    }
    char name[256] = {};
    if (::ptsname_r(master, name, sizeof(name)) != 0) {
        const int error = errno;
        (void)detail::RawClose(master);
        return MakeErrnoError(ProcErrorCode::kPtyFailed, error, "ptsname_r");
    }
    const int slave = ::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        const int error = errno;
        (void)detail::RawClose(master);
        return MakeErrnoError(ProcErrorCode::kPtyFailed, error, "open(slave)");
    }
    Pty pty;
    pty.m_master = master;
    pty.m_slave = slave;
    pty.m_slave_name = name;
    auto resized = pty.Resize(columns, rows);
    if (!resized.ok()) {
        return resized.error();
    }
    auto echo_set = pty.SetEcho(echo);
    if (!echo_set.ok()) {
        return echo_set.error();
    }
    *out = std::move(pty);
    return OkResult();
}

Result<void> Pty::Resize(int columns, int rows) {
    if (m_master < 0) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "PTY 未打开", "Pty::Resize");
    }
    if (columns <= 0 || rows <= 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "PTY 行列必须为正",
                         "Pty::Resize");
    }
    struct winsize size {};
    size.ws_col = static_cast<unsigned short>(columns);
    size.ws_row = static_cast<unsigned short>(rows);
    if (::ioctl(m_master, TIOCSWINSZ, &size) != 0) {
        return MakeErrnoError(ProcErrorCode::kPtyFailed, errno, "ioctl(TIOCSWINSZ)");
    }
    return OkResult();
}

Result<void> Pty::SetEcho(bool enabled) {
    if (m_slave < 0) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "PTY 未打开", "Pty::SetEcho");
    }
    struct termios settings {};
    if (::tcgetattr(m_slave, &settings) != 0) {
        return MakeErrnoError(ProcErrorCode::kPtyFailed, errno, "tcgetattr");
    }
    if (enabled) {
        settings.c_lflag |= ECHO;
    } else {
        settings.c_lflag &= static_cast<tcflag_t>(~ECHO);
    }
    if (::tcsetattr(m_slave, TCSANOW, &settings) != 0) {
        return MakeErrnoError(ProcErrorCode::kPtyFailed, errno, "tcsetattr");
    }
    return OkResult();
}

int Pty::ReleaseMaster() noexcept {
    (void)DisableAsync();
    const int fd = m_master;
    m_master = -1;
    return fd;
}

int Pty::ReleaseSlave() noexcept {
    const int fd = m_slave;
    m_slave = -1;
    return fd;
}

void Pty::CloseMaster() noexcept {
    if (m_master >= 0) {
        (void)DisableAsync();
        (void)detail::RawClose(m_master);
        m_master = -1;
    }
}

void Pty::CloseSlave() noexcept {
    if (m_slave >= 0) {
        (void)detail::RawClose(m_slave);
        m_slave = -1;
    }
}

// ===========================================================================
// 能力与显式异步（R31）
// ===========================================================================

IoCapability Pty::Capability() const {
    IoCapability capability = DetectIoCapability(m_master);
    if (m_master < 0 || capability.kind != IoKind::kCharacterDevice) {
        // 未打开或类型异常：保留探测给出的 reason，不伪装成可异步。
        return capability;
    }
    // PTY master 由内核 pty 驱动保证：epoll 可注册、O_NONBLOCK 下返回 EAGAIN。
    // 通用字符设备（/dev/null、/dev/ttyS*）仍由 DetectIoCapability 保持保守。
    capability.pollable = true;
    capability.can_nonblocking = true;
    capability.native_blocking = true;
    capability.async_supported = true;
    capability.reason.clear();
    return capability;
}

Result<void> Pty::SetNonBlocking(bool enable) {
    if (m_master < 0) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "PTY master 未打开",
                         "Pty::SetNonBlocking");
    }
    return SetFdNonBlocking(m_master, enable);
}

std::shared_ptr<AsyncIoBinding> Pty::async_binding() const {
    std::lock_guard<std::mutex> lock(m_async_mutex);
    return m_async;
}

Result<void> Pty::EnableAsync(go2cpp::io::IOManager* manager) {
    if (m_master < 0) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "PTY master 未打开",
                         "Pty::EnableAsync");
    }
    std::shared_ptr<AsyncIoBinding> binding;
    {
        std::lock_guard<std::mutex> lock(m_async_mutex);
        if (!m_async) {
            m_async = std::make_shared<AsyncIoBinding>();
        }
        binding = m_async;
    }
    return binding->Enable(m_master, Capability(), manager);
}

Result<void> Pty::DisableAsync() {
    const std::shared_ptr<AsyncIoBinding> binding = async_binding();
    if (!binding) {
        return OkResult();
    }
    return binding->Disable();
}

bool Pty::async_enabled() const noexcept {
    std::lock_guard<std::mutex> lock(m_async_mutex);
    return m_async != nullptr && m_async->enabled();
}

Result<void> Pty::WaitFor(bool readable, std::chrono::milliseconds timeout) {
    if (m_master < 0) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "PTY master 未打开",
                         "Pty::WaitFor");
    }
    const std::shared_ptr<AsyncIoBinding> binding = async_binding();
    if (binding && binding->enabled()) {
        return readable ? binding->WaitReadable(timeout) : binding->WaitWritable(timeout);
    }
    const int ready = PollOnce(m_master, readable ? POLLIN : POLLOUT, timeout);
    if (ready < 0) {
        return MakeErrnoError(ProcErrorCode::kIoFailed, errno, "poll(pty master)");
    }
    if (ready == 0) {
        return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT,
                         readable ? "等待 PTY 可读超时" : "等待 PTY 可写超时",
                         "Pty::WaitFor");
    }
    return OkResult();
}

Result<std::size_t> Pty::Read(MutableByteSpan buffer,
                              std::chrono::milliseconds timeout) {
    if (m_master < 0) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "PTY master 未打开",
                         "Pty::Read");
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    const auto start = std::chrono::steady_clock::now();
    const bool async = async_enabled();
    for (;;) {
        if (!async) {
            // 同步模式：先 poll 等就绪（timeout=0 时相当于立即检查），再阻塞读。
            const auto remaining = Remaining(start, timeout);
            if (timeout.count() >= 0 && remaining.count() == 0) {
                return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待 PTY 可读超时",
                                 "Pty::Read");
            }
            auto waited = WaitFor(true, remaining);
            if (!waited.ok()) {
                return waited.error();
            }
        }
        // 异步模式下 fd 已被设为非阻塞：先尝试读，EAGAIN 才 park。
        const ssize_t count = detail::RawRead(m_master, buffer.data, buffer.size);
        if (count > 0) {
            return static_cast<std::size_t>(count);
        }
        if (count == 0) {
            return MakeError(ProcErrorCode::kClosed, 0, "PTY 对端已关闭", "Pty::Read");
        }
        const int error = errno;
        if (error == EINTR) {
            continue;
        }
        if (error == EIO) {
            // 从端全部关闭后 master 返回 EIO（与 FdChannel::Pull 的映射一致）。
            return MakeError(ProcErrorCode::kClosed, 0, "PTY 对端已关闭", "Pty::Read");
        }
        if (error != EAGAIN && error != EWOULDBLOCK) {
            return MakeErrnoError(ProcErrorCode::kIoFailed, error, "read(pty master)");
        }
        // EAGAIN：同步模式说明 poll 与 read 之间被抢走，重试；异步模式 park。
        if (!async) {
            continue;
        }
        const auto remaining = Remaining(start, timeout);
        if (timeout.count() >= 0 && remaining.count() == 0) {
            return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待 PTY 可读超时",
                             "Pty::Read");
        }
        auto waited = WaitFor(true, remaining);
        if (!waited.ok()) {
            if (waited.error().code == ProcErrorCode::kTimeout) {
                return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT,
                                 "等待 PTY 可读超时", "Pty::Read");
            }
            return waited.error();
        }
    }
}

Result<void> Pty::Write(ByteSpan data, std::chrono::milliseconds timeout) {
    if (m_master < 0) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "PTY master 未打开",
                         "Pty::Write");
    }
    if (data.size == 0) {
        return OkResult();
    }
    const auto start = std::chrono::steady_clock::now();
    const bool async = async_enabled();
    std::size_t offset = 0;
    while (offset < data.size) {
        if (!async) {
            const auto remaining = Remaining(start, timeout);
            if (timeout.count() >= 0 && remaining.count() == 0) {
                return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待 PTY 可写超时",
                                 "Pty::Write");
            }
            auto waited = WaitFor(false, remaining);
            if (!waited.ok()) {
                return waited.error();
            }
        }
        const ssize_t count = detail::RawWrite(
            m_master, reinterpret_cast<const char*>(data.data) + offset, data.size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        const int error = errno;
        if (count < 0 && error == EINTR) {
            continue;
        }
        if (count < 0 && error == EPIPE) {
            return MakeError(ProcErrorCode::kChannelClosed, EPIPE, "PTY 对端已关闭",
                             "Pty::Write");
        }
        if (count < 0 && (error == EAGAIN || error == EWOULDBLOCK)) {
            if (!async) {
                continue;
            }
            const auto remaining = Remaining(start, timeout);
            if (timeout.count() >= 0 && remaining.count() == 0) {
                return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT,
                                 "等待 PTY 可写超时", "Pty::Write");
            }
            auto waited = WaitFor(false, remaining);
            if (!waited.ok()) {
                if (waited.error().code == ProcErrorCode::kTimeout) {
                    return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT,
                                     "等待 PTY 可写超时", "Pty::Write");
                }
                return waited.error();
            }
            continue;
        }
        return MakeErrnoError(ProcErrorCode::kIoFailed, error, "write(pty master)");
    }
    return OkResult();
}

}  // namespace go2cpp::os
