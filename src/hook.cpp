#include "go2cpp/hook.hpp"

#ifndef __linux__
#error "go2cpp hook interposition currently requires Linux"
#endif

#include "go2cpp/fiber.hpp"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {


using go2cpp::IOEvent;
using go2cpp::IOManager;
using go2cpp::IOWaitStatus;
using Duration = IOManager::Duration;
using go2cpp::io::DescriptorGuard;
using go2cpp::io::DescriptorTokenPtr;

using SleepFn = unsigned int (*)(unsigned int);
using UsleepFn = int (*)(useconds_t);
using NanosleepFn = int (*)(const timespec*, timespec*);
using SocketFn = int (*)(int, int, int);
using SocketPairFn = int (*)(int, int, int, int*);
using ConnectFn = int (*)(int, const sockaddr*, socklen_t);
using AcceptFn = int (*)(int, sockaddr*, socklen_t*);
using Accept4Fn = int (*)(int, sockaddr*, socklen_t*, int);
using ReadFn = ssize_t (*)(int, void*, size_t);
using ReadvFn = ssize_t (*)(int, const iovec*, int);
using RecvFn = ssize_t (*)(int, void*, size_t, int);
using RecvFromFn = ssize_t (*)(int, void*, size_t, int, sockaddr*, socklen_t*);
using RecvMsgFn = ssize_t (*)(int, msghdr*, int);
using WriteFn = ssize_t (*)(int, const void*, size_t);
using WritevFn = ssize_t (*)(int, const iovec*, int);
using SendFn = ssize_t (*)(int, const void*, size_t, int);
using SendToFn = ssize_t (*)(int, const void*, size_t, int,
                             const sockaddr*, socklen_t);
using SendMsgFn = ssize_t (*)(int, const msghdr*, int);
using CloseFn = int (*)(int);
using DupFn = int (*)(int);
using Dup2Fn = int (*)(int, int);
using Dup3Fn = int (*)(int, int, int);
using FcntlFn = int (*)(int, int, ...);
using IoctlFn = int (*)(int, unsigned long, ...);
using GetSockOptFn = int (*)(int, int, int, void*, socklen_t*);
using SetSockOptFn = int (*)(int, int, int, const void*, socklen_t);
using PollFn = int (*)(struct pollfd*, nfds_t, int);
using PpollFn = int (*)(struct pollfd*, nfds_t, const timespec*,
                        const sigset_t*);
using SelectFn = int (*)(int, fd_set*, fd_set*, fd_set*, timeval*);
using PselectFn = int (*)(int, fd_set*, fd_set*, fd_set*, const timespec*,
                          const sigset_t*);
using EpollWaitFn = int (*)(int, epoll_event*, int, int);
using EpollPwaitFn = int (*)(int, epoll_event*, int, int, const sigset_t*);

struct Originals {
    SleepFn m_sleep{nullptr};
    UsleepFn m_usleep{nullptr};
    NanosleepFn m_nanosleep{nullptr};
    SocketFn m_socket{nullptr};
    SocketPairFn m_socketpair{nullptr};
    ConnectFn m_connect{nullptr};
    AcceptFn m_accept{nullptr};
    Accept4Fn m_accept4{nullptr};
    ReadFn m_read{nullptr};
    ReadvFn m_readv{nullptr};
    RecvFn m_recv{nullptr};
    RecvFromFn m_recvfrom{nullptr};
    RecvMsgFn m_recvmsg{nullptr};
    WriteFn m_write{nullptr};
    WritevFn m_writev{nullptr};
    SendFn m_send{nullptr};
    SendToFn m_sendto{nullptr};
    SendMsgFn m_sendmsg{nullptr};
    CloseFn m_close{nullptr};
    DupFn m_dup{nullptr};
    Dup2Fn m_dup2{nullptr};
    Dup3Fn m_dup3{nullptr};
    FcntlFn m_fcntl{nullptr};
    IoctlFn m_ioctl{nullptr};
    GetSockOptFn m_getsockopt{nullptr};
    SetSockOptFn m_setsockopt{nullptr};
    PollFn m_poll{nullptr};
    PpollFn m_ppoll{nullptr};
    SelectFn m_select{nullptr};
    PselectFn m_pselect{nullptr};
    EpollWaitFn m_epoll_wait{nullptr};
    EpollPwaitFn m_epoll_pwait{nullptr};
};

Originals s_originals;
std::once_flag s_originals_once;
thread_local unsigned int s_real_call_depth = 0;
thread_local bool s_resolving = false;

class RealCallGuard final {
public:
    RealCallGuard() noexcept { ++s_real_call_depth; }
    ~RealCallGuard() { --s_real_call_depth; }
};

template <typename Function>
Function load_symbol(const char* name) noexcept {
    void* symbol = ::dlsym(RTLD_NEXT, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(symbol),
                  "function and object pointers must have equal ABI size");
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
}

void initialize_originals() noexcept {
    if (s_resolving) {
        return;
    }
    std::call_once(s_originals_once, [] {
        s_resolving = true;
        s_originals.m_sleep = load_symbol<SleepFn>("sleep");
        s_originals.m_usleep = load_symbol<UsleepFn>("usleep");
        s_originals.m_nanosleep = load_symbol<NanosleepFn>("nanosleep");
        s_originals.m_socket = load_symbol<SocketFn>("socket");
        s_originals.m_socketpair = load_symbol<SocketPairFn>("socketpair");
        s_originals.m_connect = load_symbol<ConnectFn>("connect");
        s_originals.m_accept = load_symbol<AcceptFn>("accept");
        s_originals.m_accept4 = load_symbol<Accept4Fn>("accept4");
        s_originals.m_read = load_symbol<ReadFn>("read");
        s_originals.m_readv = load_symbol<ReadvFn>("readv");
        s_originals.m_recv = load_symbol<RecvFn>("recv");
        s_originals.m_recvfrom = load_symbol<RecvFromFn>("recvfrom");
        s_originals.m_recvmsg = load_symbol<RecvMsgFn>("recvmsg");
        s_originals.m_write = load_symbol<WriteFn>("write");
        s_originals.m_writev = load_symbol<WritevFn>("writev");
        s_originals.m_send = load_symbol<SendFn>("send");
        s_originals.m_sendto = load_symbol<SendToFn>("sendto");
        s_originals.m_sendmsg = load_symbol<SendMsgFn>("sendmsg");
        s_originals.m_close = load_symbol<CloseFn>("close");
        s_originals.m_dup = load_symbol<DupFn>("dup");
        s_originals.m_dup2 = load_symbol<Dup2Fn>("dup2");
        s_originals.m_dup3 = load_symbol<Dup3Fn>("dup3");
        s_originals.m_fcntl = load_symbol<FcntlFn>("fcntl");
        s_originals.m_ioctl = load_symbol<IoctlFn>("ioctl");
        s_originals.m_getsockopt =
            load_symbol<GetSockOptFn>("getsockopt");
        s_originals.m_setsockopt =
            load_symbol<SetSockOptFn>("setsockopt");
        s_originals.m_poll = load_symbol<PollFn>("poll");
        s_originals.m_ppoll = load_symbol<PpollFn>("ppoll");
        s_originals.m_select = load_symbol<SelectFn>("select");
        s_originals.m_pselect = load_symbol<PselectFn>("pselect");
        s_originals.m_epoll_wait = load_symbol<EpollWaitFn>("epoll_wait");
        s_originals.m_epoll_pwait =
            load_symbol<EpollPwaitFn>("epoll_pwait");
        s_resolving = false;
    });
}

template <typename Function, typename... Args>
auto invoke_real(Function function, Args&&... args)
    -> decltype(function(std::forward<Args>(args)...)) {
    RealCallGuard guard;
    return function(std::forward<Args>(args)...);
}

// 被禁用或不可用的 IOManager 仍需记录 managed G 发起的原生调用。普通线程
// 以及已经处于另一个声明区域内的 worker 会走空操作。该区域让 Scheduler 在
// 系统调用休眠前发布 M::Blocking，并接纳替代的 M。
template <typename Function, typename... Args>
auto invoke_native_blocking(Function function, Args&&... args)
    -> decltype(function(std::forward<Args>(args)...)) {
    std::optional<go2cpp::BlockingRegion> blocking_region;
    if (go2cpp::Scheduler::current_task() != nullptr) {
        blocking_region.emplace();
    }
    return invoke_real(function, std::forward<Args>(args)...);
}

int raw_close_fallback(int fd) noexcept {
    return static_cast<int>(::syscall(SYS_close, fd));
}

std::atomic<bool> s_enabled{true};
std::atomic<std::uint64_t> s_scoped_enable_count{0};
std::atomic<std::int64_t> s_connect_timeout_ms{5000};
thread_local IOManager* s_bound_manager = nullptr;

bool hooks_enabled() noexcept {
    const bool process_enabled =
        s_enabled.load(std::memory_order_acquire) ||
        s_scoped_enable_count.load(std::memory_order_acquire) != 0;
    return go2cpp::thread_policy::detail::HookAllowed(process_enabled);
}

IOManager* current_manager() noexcept {
    if (IOManager* manager = IOManager::Current()) {
        return manager;
    }
    return s_bound_manager;
}

IOManager* cooperative_manager() noexcept {
    if (!hooks_enabled() || s_real_call_depth != 0 ||
        go2cpp::Fiber::Current() == nullptr ||
        !go2cpp::Scheduler::current_task()) {
        return nullptr;
    }
    return current_manager();
}

struct OpenDescription {
    mutable std::mutex m_mutex;
    bool m_socket{true};
    bool m_user_nonblocking{false};
    bool m_system_nonblocking{false};
    std::optional<Duration> m_receive_timeout;
    std::optional<Duration> m_send_timeout;
};

struct Descriptor {
    Descriptor(std::shared_ptr<OpenDescription> description,
               DescriptorTokenPtr descriptor_token)
        : m_open(std::move(description)),
          m_token(std::move(descriptor_token)) {}

    std::shared_ptr<OpenDescription> m_open;
    DescriptorTokenPtr m_token;
    mutable std::mutex m_mutex;
    bool m_closed{false};
};

std::mutex s_descriptors_mutex;
std::unordered_map<int, std::shared_ptr<Descriptor>> s_descriptors;

std::optional<Duration> timeval_duration(const timeval& value) noexcept {
    if (value.tv_sec < 0 || value.tv_usec < 0 || value.tv_usec >= 1000000) {
        return std::nullopt;
    }
    if (value.tv_sec == 0 && value.tv_usec == 0) {
        return std::nullopt;
    }
    using Microseconds = std::chrono::microseconds;
    constexpr auto maximum =
        std::chrono::duration_cast<Microseconds>(Duration::max()).count();
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    const auto microseconds = static_cast<std::uint64_t>(value.tv_usec);
    const auto max_count = static_cast<std::uint64_t>(maximum);
    if (seconds > max_count / 1000000ULL ||
        seconds * 1000000ULL > max_count - microseconds) {
        return Duration::max();
    }
    return std::chrono::duration_cast<Duration>(
        Microseconds(seconds * 1000000ULL + microseconds));
}

std::optional<Duration> socket_timeout(int fd, int option) noexcept {
    if (!s_originals.m_getsockopt) {
        return std::nullopt;
    }
    timeval value{};
    socklen_t size = sizeof(value);
    if (invoke_real(s_originals.m_getsockopt, fd, SOL_SOCKET, option, &value,
                    &size) != 0 ||
        size < sizeof(value)) {
        return std::nullopt;
    }
    return timeval_duration(value);
}

std::shared_ptr<Descriptor> descriptor_for(int fd) {
    std::lock_guard<std::mutex> lock(s_descriptors_mutex);
    const auto found = s_descriptors.find(fd);
    return found == s_descriptors.end() ? nullptr : found->second;
}

bool inspect_socket(int fd) noexcept {
    if (!s_originals.m_getsockopt) {
        return false;
    }
    int type = 0;
    socklen_t size = sizeof(type);
    return invoke_real(s_originals.m_getsockopt, fd, SOL_SOCKET, SO_TYPE, &type,
                       &size) == 0;
}

std::shared_ptr<Descriptor> adopt_socket(int fd) {
    DescriptorGuard lifecycle;
    if (fd < 0 || !s_originals.m_fcntl || !inspect_socket(fd)) {
        return {};
    }

    // 串行化第一次接管。否则一个接管者可能观察到另一个接管者设置的
    // O_NONBLOCK 位，并错误地将其识别为用户请求。
    std::lock_guard<std::mutex> lock(s_descriptors_mutex);
    const auto found = s_descriptors.find(fd);
    if (found != s_descriptors.end()) {
        return found->second;
    }

    const int flags = invoke_real(s_originals.m_fcntl, fd, F_GETFL);
    if (flags < 0) {
        return {};
    }

    const bool originally_nonblocking = (flags & O_NONBLOCK) != 0;
    bool changed_system_nonblocking = false;
    auto open = std::make_shared<OpenDescription>();
    open->m_user_nonblocking = originally_nonblocking;
    open->m_system_nonblocking = originally_nonblocking;
    open->m_receive_timeout = socket_timeout(fd, SO_RCVTIMEO);
    open->m_send_timeout = socket_timeout(fd, SO_SNDTIMEO);
    if (!open->m_system_nonblocking &&
        invoke_real(s_originals.m_fcntl, fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        open->m_system_nonblocking = true;
        changed_system_nonblocking = true;
    }
    try {
        auto descriptor = std::make_shared<Descriptor>(
            std::move(open), DescriptorGuard::Capture(fd));
        s_descriptors.emplace(fd, descriptor);
        return descriptor;
    } catch (...) {
        if (changed_system_nonblocking) {
            (void)invoke_real(s_originals.m_fcntl, fd, F_SETFL, flags);
        }
        throw;
    }
}

std::shared_ptr<Descriptor> try_adopt_socket(int fd) noexcept {
    try {
        return adopt_socket(fd);
    } catch (...) {
        // 元数据只是围绕真实系统调用的优化。分配失败时保持 fd 可用，让调用者
        // 回退到 libc。
        return {};
    }
}

void register_new_socket(int fd, bool user_nonblocking) {
    DescriptorGuard lifecycle;
    if (fd < 0 || !s_originals.m_fcntl) {
        return;
    }
    const int flags = invoke_real(s_originals.m_fcntl, fd, F_GETFL);
    if (flags < 0) {
        return;
    }
    const bool originally_nonblocking = (flags & O_NONBLOCK) != 0;
    bool changed_system_nonblocking = false;
    auto open = std::make_shared<OpenDescription>();
    open->m_user_nonblocking = user_nonblocking || originally_nonblocking;
    open->m_system_nonblocking = originally_nonblocking;
    if (!open->m_system_nonblocking &&
        invoke_real(s_originals.m_fcntl, fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        open->m_system_nonblocking = true;
        changed_system_nonblocking = true;
    }
    try {
        auto descriptor = std::make_shared<Descriptor>(
            std::move(open), DescriptorGuard::Capture(fd));
        std::lock_guard<std::mutex> lock(s_descriptors_mutex);
        s_descriptors[fd] = std::move(descriptor);
    } catch (...) {
        if (changed_system_nonblocking) {
            (void)invoke_real(s_originals.m_fcntl, fd, F_SETFL, flags);
        }
        throw;
    }
}

void try_register_new_socket(int fd, bool user_nonblocking) noexcept {
    try {
        register_new_socket(fd, user_nonblocking);
    } catch (...) {
        // socket 系统调用已经成功。未跟踪的描述符可以在后续 managed 操作中
        // 延迟接管。
    }
}

struct DescriptorSnapshot {
    DescriptorTokenPtr m_token;
    bool m_closed{true};
};

DescriptorSnapshot snapshot_descriptor(
    const std::shared_ptr<Descriptor>& descriptor) noexcept {
    DescriptorSnapshot snapshot;
    if (!descriptor) {
        return snapshot;
    }
    std::lock_guard<std::mutex> lock(descriptor->m_mutex);
    snapshot.m_token = descriptor->m_token;
    snapshot.m_closed = descriptor->m_closed || !snapshot.m_token ||
                        !snapshot.m_token->valid();
    return snapshot;
}

bool descriptor_closed(const std::shared_ptr<Descriptor>& descriptor) noexcept {
    return snapshot_descriptor(descriptor).m_closed;
}

struct ClosePlan {
    std::shared_ptr<Descriptor> m_descriptor;
};

ClosePlan prepare_close(int fd) {
    ClosePlan plan;
    {
        std::lock_guard<std::mutex> lock(s_descriptors_mutex);
        const auto found = s_descriptors.find(fd);
        if (found != s_descriptors.end()) {
            plan.m_descriptor = found->second;
        }
    }

    if (plan.m_descriptor) {
        std::lock_guard<std::mutex> lock(plan.m_descriptor->m_mutex);
        plan.m_descriptor->m_closed = true;
    }
    return plan;
}

void finish_close(int fd, const std::shared_ptr<Descriptor>& descriptor) {
    if (!descriptor) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_descriptors_mutex);
    const auto found = s_descriptors.find(fd);
    if (found != s_descriptors.end() && found->second == descriptor) {
        s_descriptors.erase(found);
    }
}

void notify_before_close(int fd) {
    IOManager::NotifyCloseAll(fd);
}

void clone_descriptor(int old_fd, int new_fd) {
    auto source = descriptor_for(old_fd);
    if (!source && cooperative_manager()) {
        source = adopt_socket(old_fd);
    }
    if (source && snapshot_descriptor(source).m_closed) {
        source.reset();
    }
    std::lock_guard<std::mutex> lock(s_descriptors_mutex);
    if (source) {
        s_descriptors[new_fd] =
            std::make_shared<Descriptor>(source->m_open,
                                         DescriptorGuard::Capture(new_fd));
    } else {
        s_descriptors.erase(new_fd);
    }
}

void try_clone_descriptor(int old_fd, int new_fd) noexcept {
    try {
        clone_descriptor(old_fd, new_fd);
    } catch (...) {
        // 不要让 C++ 分配失败越过 C ABI。移除过期的目标条目；下一次 managed
        // 调用可以重新接管该描述符。
        std::lock_guard<std::mutex> lock(s_descriptors_mutex);
        s_descriptors.erase(new_fd);
    }
}

// dup2/dup3 会把关闭目标作为内核操作的一部分。Hook 必须在进入系统调用前
// 发布关闭，防止旧 open-file description 的 epoll 事件赢得一次性等待。若
// Linux 内核拒绝本来有效的替换，目标仍保持打开；重新建立描述符 token，
// 让后续 managed 调用可以再次接管。预检关闭已经唤醒的 waiter 保留其终态结果。
void restore_failed_replacement(
    int fd, const std::shared_ptr<Descriptor>& descriptor) noexcept {
    if (!descriptor) {
        return;
    }
    DescriptorTokenPtr token;
    try {
        token = DescriptorGuard::Capture(fd);
    } catch (...) {
        // 如果恢复过程本身内存不足，丢弃过期条目。后续 managed 操作可以接管
        // 仍存活的描述符，而不会被错误的 closed 标志永久阻塞。
    }
    if (token) {
        std::lock_guard<std::mutex> lock(descriptor->m_mutex);
        descriptor->m_token = std::move(token);
        descriptor->m_closed = false;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(descriptor->m_mutex);
        descriptor->m_token.reset();
        descriptor->m_closed = true;
    }
    std::lock_guard<std::mutex> lock(s_descriptors_mutex);
    const auto found = s_descriptors.find(fd);
    if (found != s_descriptors.end() && found->second == descriptor) {
        s_descriptors.erase(found);
    }
}

bool source_fd_is_valid(int fd) noexcept {
    if (fd < 0 || !s_originals.m_fcntl) {
        return false;
    }
    return invoke_real(s_originals.m_fcntl, fd, F_GETFD) >= 0;
}

std::optional<Duration> operation_timeout(
    const std::shared_ptr<Descriptor>& descriptor, IOEvent event) {
    std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
    return event == IOEvent::kRead ? descriptor->m_open->m_receive_timeout
                                   : descriptor->m_open->m_send_timeout;
}

bool can_wait(const std::shared_ptr<Descriptor>& descriptor) noexcept {
    std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
    return descriptor->m_open->m_socket &&
           descriptor->m_open->m_system_nonblocking &&
           !descriptor->m_open->m_user_nonblocking;
}

int wait_error(const go2cpp::IOWaitResult& result) noexcept {
    switch (result.status) {
        case IOWaitStatus::kReady:
            return 0;
        case IOWaitStatus::kTimeout:
            return ETIMEDOUT;
        case IOWaitStatus::kCancelled:
            return ECANCELED;
        case IOWaitStatus::kClosed:
            return EBADF;
        case IOWaitStatus::kError:
            return result.system_error == 0 ? EIO : result.system_error;
    }
    return EIO;
}

go2cpp::IOWaitResult wait_for_io(
    IOManager& manager, int fd, IOEvent event,
    const std::optional<IOManager::TimePoint>& deadline,
    DescriptorTokenPtr token) {
    return manager.wait(fd, event, deadline, {}, std::move(token));
}

std::optional<IOManager::TimePoint> deadline_from_timeout(
    const std::optional<Duration>& timeout) {
    if (!timeout.has_value()) {
        return std::nullopt;
    }
    const auto now = IOManager::Clock::now();
    if (*timeout >= Duration::max() - now.time_since_epoch()) {
        return IOManager::TimePoint::max();
    }
    return now + *timeout;
}

int native_wait(int fd, IOEvent event,
                const std::optional<IOManager::TimePoint>& deadline,
                const std::shared_ptr<Descriptor>& descriptor) {
    // 另一个线程中的 poll 不一定会被 close 可靠中断。有限时长的 poll 切片让
    // 保留的代次检测 close，同时不持有 IOManager，也不会在复用的数字描述符
    // 上永久等待。
    for (;;) {
        {
            DescriptorGuard lifecycle;
            if (descriptor_closed(descriptor)) {
                return EBADF;
            }
        }
        int milliseconds = 10;
        if (deadline) {
            const auto now = IOManager::Clock::now();
            if (*deadline <= now) {
                // 原始阻塞 socket 契约报告 socket 超时，而不是内部非阻塞探测结果。
                return ETIMEDOUT;
            }
            const auto remaining = *deadline - now;
            // 用短切片轮询，及时观察 close 或 fd 复用。转换前先限制范围：
            // TimePoint::max() 可能超出毫秒 duration 的可表示范围。
            const auto slice = std::min(
                remaining,
                std::chrono::duration_cast<IOManager::Clock::duration>(
                    std::chrono::milliseconds(10)));
            auto rounded = std::chrono::duration_cast<std::chrono::milliseconds>(
                slice);
            if (rounded < slice) {
                rounded += std::chrono::milliseconds(1);
            }
            milliseconds = static_cast<int>(std::min<std::int64_t>(
                milliseconds, std::max<std::int64_t>(1, rounded.count())));
        }
        pollfd poll_descriptor{};
        poll_descriptor.fd = fd;
        poll_descriptor.events = event == IOEvent::kRead ? POLLIN : POLLOUT;
        const long result = ::syscall(SYS_poll, &poll_descriptor, 1,
                                      milliseconds);
        if (result < 0) {
            return errno;
        }
        if (result > 0) {
            // poll 返回就绪后再次通过生命周期门检查。这样 close/dup2
            // Hook 在就绪通知与真正读写之间不能把旧 generation 伪装成
            // 新的同号 fd；真正的 syscall 仍在 cooperative_io 的门内执行。
            DescriptorGuard lifecycle;
            if (descriptor_closed(descriptor)) {
                return EBADF;
            }
            return (poll_descriptor.revents & POLLNVAL) != 0 ? EBADF : 0;
        }
    }
}

template <typename Result, typename Function, typename... Args>
Result cooperative_io(int fd, Function function, IOEvent event,
                      bool per_call_nonblocking, Args... args) {
    if (!function) {
        errno = ENOSYS;
        return static_cast<Result>(-1);
    }
    IOManager* manager = cooperative_manager();
    const bool managed_hook_admission =
        manager != nullptr ||
        (hooks_enabled() && s_real_call_depth == 0 &&
         go2cpp::Scheduler::current_task() != nullptr &&
         go2cpp::Fiber::Current() != nullptr);
    // 启用 Hook 时，没有 IOManager 的 managed Fiber 使用相同的描述符元数据和
    // 原生 poll 回退。禁用 Hook 时保留调用者的原生阻塞选择，但仍在下方记录 M。
    std::optional<go2cpp::BlockingRegion> blocking_region;
    if (manager == nullptr && go2cpp::Scheduler::current_task() != nullptr) {
        blocking_region.emplace();
    }
    auto descriptor = descriptor_for(fd);
    if (!descriptor && managed_hook_admission) {
        descriptor = try_adopt_socket(fd);
    }
    if (!descriptor) {
        // IOManager 正常路径把 socket 设为 nonblocking 并 park Fiber；
        // 未跟踪/懒采用失败的 fd 会回到 libc，必须显式发布 M::Blocking，
        // 否则 sysmon 无法为这个长系统调用申请替代 M。
        if (!blocking_region && go2cpp::Scheduler::current_task() != nullptr) {
            blocking_region.emplace();
        }
        return invoke_real(function, fd, args...);
    }

    const bool blocking = !per_call_nonblocking && can_wait(descriptor);
    const auto deadline =
        deadline_from_timeout(operation_timeout(descriptor, event));
    for (;;) {
        Result result;
        {
            DescriptorGuard lifecycle;
            if (snapshot_descriptor(descriptor).m_closed) {
                errno = EBADF;
                return static_cast<Result>(-1);
            }
            result = invoke_real(function, fd, args...);
        }
        if (result == static_cast<Result>(-1) && errno == EINTR && manager) {
            continue;
        }
        if (!blocking) {
            return result;
        }
        if (result != static_cast<Result>(-1) ||
            (errno != EAGAIN && errno != EWOULDBLOCK)) {
            return result;
        }

        const auto token = snapshot_descriptor(descriptor).m_token;
        const int error = manager
                              ? wait_error(wait_for_io(*manager, fd, event,
                                                       deadline, token))
                              : native_wait(fd, event, deadline, descriptor);
        if (error != 0) {
            errno = error;
            return static_cast<Result>(-1);
        }
    }
}

bool has_dontwait(int flags) noexcept {
#ifdef MSG_DONTWAIT
    return (flags & MSG_DONTWAIT) != 0;
#else
    (void)flags;
    return false;
#endif
}

std::optional<Duration> connect_wait_timeout(
    const std::shared_ptr<Descriptor>& descriptor) {
    if (auto timeout = operation_timeout(descriptor, IOEvent::kWrite)) {
        return timeout;
    }
    const auto milliseconds =
        s_connect_timeout_ms.load(std::memory_order_acquire);
    if (milliseconds < 0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds(milliseconds);
}

int cooperative_connect(int fd, const sockaddr* address, socklen_t length) {
    if (!s_originals.m_connect) {
        errno = ENOSYS;
        return -1;
    }
    IOManager* manager = cooperative_manager();
    const bool managed_hook_admission =
        manager != nullptr ||
        (hooks_enabled() && s_real_call_depth == 0 &&
         go2cpp::Scheduler::current_task() != nullptr &&
         go2cpp::Fiber::Current() != nullptr);
    // 启用 Hook 时，没有 IOManager 的 managed Fiber 仍可使用有限时长的原生
    // poll 回退。禁用 Hook 时保留原生语义，只发布 M::Blocking。
    std::optional<go2cpp::BlockingRegion> blocking_region;
    if (manager == nullptr && go2cpp::Scheduler::current_task() != nullptr) {
        blocking_region.emplace();
    }
    auto descriptor = descriptor_for(fd);
    if (!descriptor && managed_hook_admission) {
        descriptor = try_adopt_socket(fd);
    }
    if (!descriptor) {
        // connect 的未跟踪 fd 同样会落回可能阻塞的 libc 调用；在
        // managed G 中补齐 BlockingRegion，普通线程保持原生语义。
        if (!blocking_region && go2cpp::Scheduler::current_task() != nullptr) {
            blocking_region.emplace();
        }
        return invoke_real(s_originals.m_connect, fd, address, length);
    }

    int result;
    do {
        DescriptorGuard lifecycle;
        if (snapshot_descriptor(descriptor).m_closed) {
            errno = EBADF;
            return -1;
        }
        result = invoke_real(s_originals.m_connect, fd, address, length);
    } while (result < 0 && errno == EINTR && manager);
    if (!can_wait(descriptor)) {
        return result;
    }
    if (result == 0) {
        return 0;
    }
    if (result != -1 ||
        (errno != EINPROGRESS && errno != EALREADY && errno != EAGAIN)) {
        return result;
    }

    // 配置的 connect 超时同样适用于 managed 延迟接管后的原生 poll 回退。若
    // 元数据分配失败或显式禁用 Hook，原始 libc 调用仍是文档规定的原生阻塞边界。
    const auto deadline =
        deadline_from_timeout(connect_wait_timeout(descriptor));
    const auto token = snapshot_descriptor(descriptor).m_token;
    const int wait_errno =
        manager ? wait_error(wait_for_io(*manager, fd, IOEvent::kWrite,
                                         deadline, token))
                : native_wait(fd, IOEvent::kWrite, deadline, descriptor);
    if (wait_errno != 0) {
        errno = wait_errno;
        return -1;
    }
    DescriptorGuard lifecycle;
    if (snapshot_descriptor(descriptor).m_closed) {
        errno = EBADF;
        return -1;
    }

    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (!s_originals.m_getsockopt ||
        invoke_real(s_originals.m_getsockopt, fd, SOL_SOCKET, SO_ERROR,
                    &socket_error, &error_size) != 0) {
        return -1;
    }
    if (socket_error != 0) {
        errno = socket_error;
        return -1;
    }
    return 0;
}

enum class SleepOutcome { Unavailable, Completed, Interrupted };

SleepOutcome cooperative_sleep_for(Duration duration) {
    IOManager* manager = cooperative_manager();
    if (duration <= Duration::zero()) {
        return SleepOutcome::Completed;
    }
    if (!manager) {
        return SleepOutcome::Unavailable;
    }
    const int fd = static_cast<int>(
        ::syscall(SYS_eventfd2, 0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (fd < 0) {
        return SleepOutcome::Unavailable;
    }
    const auto result = manager->WaitFor(fd, IOEvent::kRead, duration);
    (void)manager->NotifyClose(fd);
    (void)::syscall(SYS_close, fd);
    if (result.status == IOWaitStatus::kTimeout ||
        result.status == IOWaitStatus::kReady) {
        return SleepOutcome::Completed;
    }
    errno = wait_error(result);
    return SleepOutcome::Interrupted;
}

Duration saturating_duration(const timespec& value) noexcept {
    using Nanoseconds = std::chrono::nanoseconds;
    const auto max_ns =
        std::chrono::duration_cast<Nanoseconds>(Duration::max()).count();
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    const auto nanoseconds = static_cast<std::uint64_t>(value.tv_nsec);
    const auto maximum = static_cast<std::uint64_t>(max_ns);
    if (seconds > maximum / 1000000000ULL ||
        seconds * 1000000000ULL > maximum - nanoseconds) {
        return Duration::max();
    }
    return std::chrono::duration_cast<Duration>(
        Nanoseconds(seconds * 1000000000ULL + nanoseconds));
}

enum class FcntlArgument { None, Integer, Pointer, Unknown };

FcntlArgument fcntl_argument(int command) noexcept {
    switch (command) {
        case F_GETFD:
        case F_GETFL:
        case F_GETOWN:
#ifdef F_GETSIG
        case F_GETSIG:
#endif
#ifdef F_GETLEASE
        case F_GETLEASE:
#endif
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
#ifdef F_GET_SEALS
        case F_GET_SEALS:
#endif
            return FcntlArgument::None;

        case F_DUPFD:
#ifdef F_DUPFD_CLOEXEC
        case F_DUPFD_CLOEXEC:
#endif
        case F_SETFD:
        case F_SETFL:
        case F_SETOWN:
#ifdef F_SETSIG
        case F_SETSIG:
#endif
#ifdef F_SETLEASE
        case F_SETLEASE:
#endif
#ifdef F_NOTIFY
        case F_NOTIFY:
#endif
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
#ifdef F_ADD_SEALS
        case F_ADD_SEALS:
#endif
            return FcntlArgument::Integer;

        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
#ifdef F_OFD_GETLK
        case F_OFD_GETLK:
        case F_OFD_SETLK:
        case F_OFD_SETLKW:
#endif
#ifdef F_GETOWN_EX
        case F_GETOWN_EX:
        case F_SETOWN_EX:
#endif
#ifdef F_GET_RW_HINT
        case F_GET_RW_HINT:
        case F_SET_RW_HINT:
#endif
#ifdef F_GET_FILE_RW_HINT
        case F_GET_FILE_RW_HINT:
        case F_SET_FILE_RW_HINT:
#endif
            return FcntlArgument::Pointer;
        default:
            return FcntlArgument::Unknown;
    }
}

bool ioctl_has_pointer_argument(unsigned long request) noexcept {
    if (request == FIONBIO || request == FIONREAD) {
        return true;
    }
#ifdef TIOCOUTQ
    if (request == TIOCOUTQ) {
        return true;
    }
#endif
    return false;
}

bool ioctl_has_no_argument(unsigned long request) noexcept {
#ifdef FIOCLEX
    if (request == FIOCLEX) {
        return true;
    }
#endif
#ifdef FIONCLEX
    if (request == FIONCLEX) {
        return true;
    }
#endif
#ifdef TIOCEXCL
    if (request == TIOCEXCL || request == TIOCNXCL) {
        return true;
    }
#endif
    return false;
}

}  // namespace

namespace go2cpp::hook {

void set_enabled(bool enabled_value) noexcept {
    s_enabled.store(enabled_value, std::memory_order_release);
}

bool enabled() noexcept { return hooks_enabled(); }

void bind_io_manager(IOManager* manager) noexcept {
    s_bound_manager = manager;
}

IOManager* bound_io_manager() noexcept { return s_bound_manager; }

void set_connect_timeout(std::chrono::milliseconds timeout) noexcept {
    s_connect_timeout_ms.store(timeout == std::chrono::milliseconds::max()
                                   ? -1
                                   : std::max<std::int64_t>(0, timeout.count()),
                               std::memory_order_release);
}

std::chrono::milliseconds connect_timeout() noexcept {
    const auto value = s_connect_timeout_ms.load(std::memory_order_acquire);
    return value < 0 ? std::chrono::milliseconds::max()
                     : std::chrono::milliseconds(value);
}

ScopedEnable::ScopedEnable() noexcept {
    s_scoped_enable_count.fetch_add(1, std::memory_order_acq_rel);
}

ScopedEnable::~ScopedEnable() {
    if (m_active) {
        s_scoped_enable_count.fetch_sub(1, std::memory_order_acq_rel);
    }
}

ScopedEnable::ScopedEnable(ScopedEnable&& other) noexcept
    : m_active(other.m_active) {
    other.m_active = false;
}

ScopedIOManagerBinding::ScopedIOManagerBinding(IOManager* manager) noexcept
    : m_previous(s_bound_manager) {
    s_bound_manager = manager;
}

ScopedIOManagerBinding::~ScopedIOManagerBinding() {
    s_bound_manager = m_previous;
}

}  // namespace go2cpp::hook

extern "C" {

void go2cpp_hook_set_enabled(int enabled) noexcept {
    go2cpp::hook::set_enabled(enabled != 0);
}

int go2cpp_hook_is_enabled() noexcept {
    return go2cpp::hook::enabled() ? 1 : 0;
}

void go2cpp_hook_bind_io_manager(void* manager) noexcept {
    go2cpp::hook::bind_io_manager(static_cast<IOManager*>(manager));
}

unsigned int sleep(unsigned int seconds) {
    initialize_originals();
    const SleepOutcome outcome =
        cooperative_sleep_for(std::chrono::seconds(seconds));
    if (outcome == SleepOutcome::Completed) {
        return 0;
    }
    if (outcome == SleepOutcome::Interrupted) {
        return seconds;
    }
    if (!s_originals.m_sleep) {
        errno = ENOSYS;
        return seconds;
    }
    return invoke_native_blocking(s_originals.m_sleep, seconds);
}

int usleep(useconds_t microseconds) {
    initialize_originals();
    const SleepOutcome outcome =
        cooperative_sleep_for(std::chrono::microseconds(microseconds));
    if (outcome == SleepOutcome::Completed) {
        return 0;
    }
    if (outcome == SleepOutcome::Interrupted) {
        return -1;
    }
    if (!s_originals.m_usleep) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_usleep, microseconds);
}

int nanosleep(const timespec* request, timespec* remaining) {
    initialize_originals();
    if (!request || request->tv_sec < 0 || request->tv_nsec < 0 ||
        request->tv_nsec >= 1000000000L) {
        if (!s_originals.m_nanosleep) {
            errno = EINVAL;
            return -1;
        }
        return invoke_real(s_originals.m_nanosleep, request, remaining);
    }
    const SleepOutcome outcome =
        cooperative_sleep_for(saturating_duration(*request));
    if (outcome == SleepOutcome::Completed) {
        if (remaining) {
            *remaining = timespec{};
        }
        return 0;
    }
    if (outcome == SleepOutcome::Interrupted) {
        return -1;
    }
    if (!s_originals.m_nanosleep) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_nanosleep, request, remaining);
}

// poll/select/epoll_wait Hook 保持 libc ABI 和 timeout、EINTR、信号掩码、
// revents 语义，不把一次多路等待偷偷拆成多个顺序等待。需要在 Fiber 中
// 通过 epoll 注册多个 fd 并由任一事件唤醒时，显式调用 IOManager::WaitAny/
// WaitMany；普通线程继续使用原生 poll/select，长时间 native 调用仍由
// BlockingRegion/sysmon 记录其 M 状态。
int poll(struct pollfd* descriptors, nfds_t count, int timeout) {
    initialize_originals();
    if (!s_originals.m_poll) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_poll, descriptors, count,
                                  timeout);
}

int ppoll(struct pollfd* descriptors, nfds_t count, const timespec* timeout,
          const sigset_t* signal_mask) {
    initialize_originals();
    if (!s_originals.m_ppoll) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_ppoll, descriptors, count,
                                  timeout, signal_mask);
}

int select(int descriptor_count, fd_set* read_set, fd_set* write_set,
           fd_set* exception_set, timeval* timeout) {
    initialize_originals();
    if (!s_originals.m_select) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_select, descriptor_count,
                                  read_set, write_set, exception_set, timeout);
}

int pselect(int descriptor_count, fd_set* read_set, fd_set* write_set,
            fd_set* exception_set, const timespec* timeout,
            const sigset_t* signal_mask) {
    initialize_originals();
    if (!s_originals.m_pselect) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_pselect, descriptor_count,
                                  read_set, write_set, exception_set, timeout,
                                  signal_mask);
}

int epoll_wait(int epoll_descriptor, epoll_event* events, int max_events,
               int timeout) {
    initialize_originals();
    if (!s_originals.m_epoll_wait) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_epoll_wait, epoll_descriptor,
                                  events, max_events, timeout);
}

int epoll_pwait(int epoll_descriptor, epoll_event* events, int max_events,
                int timeout, const sigset_t* signal_mask) {
    initialize_originals();
    if (!s_originals.m_epoll_pwait) {
        errno = ENOSYS;
        return -1;
    }
    return invoke_native_blocking(s_originals.m_epoll_pwait, epoll_descriptor,
                                  events, max_events, timeout, signal_mask);
}

int socket(int domain, int type, int protocol) {
    initialize_originals();
    if (!s_originals.m_socket) {
        errno = ENOSYS;
        return -1;
    }
    const int fd = invoke_real(s_originals.m_socket, domain, type, protocol);
    if (fd >= 0 && cooperative_manager()) {
        try_register_new_socket(fd, (type & SOCK_NONBLOCK) != 0);
    }
    return fd;
}

int socketpair(int domain, int type, int protocol, int descriptors[2]) {
    initialize_originals();
    if (!s_originals.m_socketpair) {
        errno = ENOSYS;
        return -1;
    }
    const int result = invoke_real(s_originals.m_socketpair, domain, type,
                                   protocol, descriptors);
    if (result == 0 && descriptors && cooperative_manager()) {
        const bool user_nonblocking = (type & SOCK_NONBLOCK) != 0;
        try_register_new_socket(descriptors[0], user_nonblocking);
        try_register_new_socket(descriptors[1], user_nonblocking);
    }
    return result;
}

int connect(int fd, const sockaddr* address, socklen_t length) {
    initialize_originals();
    return cooperative_connect(fd, address, length);
}

int accept(int fd, sockaddr* address, socklen_t* length) {
    initialize_originals();
    const int accepted = cooperative_io<int>(fd, s_originals.m_accept,
                                              IOEvent::kRead, false, address,
                                              length);
    if (accepted >= 0 && cooperative_manager()) {
        try_register_new_socket(accepted, false);
    }
    return accepted;
}

int accept4(int fd, sockaddr* address, socklen_t* length, int flags) {
    initialize_originals();
    const int accepted = cooperative_io<int>(
        fd, s_originals.m_accept4, IOEvent::kRead,
        (flags & SOCK_NONBLOCK) != 0, address, length,
        flags);
    if (accepted >= 0 && cooperative_manager()) {
        try_register_new_socket(accepted, (flags & SOCK_NONBLOCK) != 0);
    }
    return accepted;
}

ssize_t read(int fd, void* buffer, size_t count) {
    initialize_originals();
    if (s_resolving && !s_originals.m_read) {
        return static_cast<ssize_t>(::syscall(SYS_read, fd, buffer, count));
    }
    return cooperative_io<ssize_t>(fd, s_originals.m_read, IOEvent::kRead,
                                    false, buffer, count);
}

ssize_t readv(int fd, const iovec* vectors, int count) {
    initialize_originals();
    return cooperative_io<ssize_t>(fd, s_originals.m_readv, IOEvent::kRead,
                                    false, vectors, count);
}

ssize_t recv(int fd, void* buffer, size_t length, int flags) {
    initialize_originals();
    // MSG_WAITALL 需要跨多次读取累计字节数；有限 Hook 不模拟该契约。保留
    // libc 的确切行为，不要在一次唤醒后静默返回短读。
#ifdef MSG_WAITALL
    if ((flags & MSG_WAITALL) != 0 && s_originals.m_recv) {
        if (cooperative_manager() != nullptr) {
            errno = ENOTSUP;
            return static_cast<ssize_t>(-1);
        }
        return invoke_native_blocking(s_originals.m_recv, fd, buffer, length, flags);
    }
#endif
#ifdef MSG_OOB
    // EPOLLPRI 有意不属于当前有限的就绪能力。不要让 managed G 等待 poller
    // 无法报告的紧急数据事件；普通线程保留 libc 的原生语义。
    if ((flags & MSG_OOB) != 0 && cooperative_manager() != nullptr) {
        errno = ENOTSUP;
        return static_cast<ssize_t>(-1);
    }
#endif
    return cooperative_io<ssize_t>(fd, s_originals.m_recv, IOEvent::kRead,
                                    has_dontwait(flags), buffer, length,
                                    flags);
}

ssize_t recvfrom(int fd, void* buffer, size_t length, int flags,
                 sockaddr* source, socklen_t* source_length) {
    initialize_originals();
#ifdef MSG_WAITALL
    if ((flags & MSG_WAITALL) != 0 && s_originals.m_recvfrom) {
        if (cooperative_manager() != nullptr) {
            errno = ENOTSUP;
            return static_cast<ssize_t>(-1);
        }
        return invoke_native_blocking(s_originals.m_recvfrom, fd, buffer, length, flags,
                                      source, source_length);
    }
#endif
#ifdef MSG_OOB
    if ((flags & MSG_OOB) != 0 && cooperative_manager() != nullptr) {
        errno = ENOTSUP;
        return static_cast<ssize_t>(-1);
    }
#endif
    return cooperative_io<ssize_t>(
        fd, s_originals.m_recvfrom, IOEvent::kRead, has_dontwait(flags), buffer,
        length, flags, source, source_length);
}

ssize_t recvmsg(int fd, msghdr* message, int flags) {
    initialize_originals();
#ifdef MSG_WAITALL
    if ((flags & MSG_WAITALL) != 0 && s_originals.m_recvmsg) {
        if (cooperative_manager() != nullptr) {
            errno = ENOTSUP;
            return static_cast<ssize_t>(-1);
        }
        return invoke_native_blocking(s_originals.m_recvmsg, fd, message, flags);
    }
#endif
#ifdef MSG_OOB
    if ((flags & MSG_OOB) != 0 && cooperative_manager() != nullptr) {
        errno = ENOTSUP;
        return static_cast<ssize_t>(-1);
    }
#endif
    return cooperative_io<ssize_t>(fd, s_originals.m_recvmsg, IOEvent::kRead,
                                    has_dontwait(flags), message, flags);
}

ssize_t write(int fd, const void* buffer, size_t count) {
    initialize_originals();
    if (s_resolving && !s_originals.m_write) {
        return static_cast<ssize_t>(::syscall(SYS_write, fd, buffer, count));
    }
    return cooperative_io<ssize_t>(fd, s_originals.m_write, IOEvent::kWrite,
                                    false, buffer, count);
}

ssize_t writev(int fd, const iovec* vectors, int count) {
    initialize_originals();
    return cooperative_io<ssize_t>(fd, s_originals.m_writev, IOEvent::kWrite,
                                    false, vectors, count);
}

ssize_t send(int fd, const void* buffer, size_t length, int flags) {
    initialize_originals();
    return cooperative_io<ssize_t>(fd, s_originals.m_send, IOEvent::kWrite,
                                    has_dontwait(flags), buffer, length,
                                    flags);
}

ssize_t sendto(int fd, const void* buffer, size_t length, int flags,
               const sockaddr* destination, socklen_t destination_length) {
    initialize_originals();
    return cooperative_io<ssize_t>(
        fd, s_originals.m_sendto, IOEvent::kWrite, has_dontwait(flags), buffer,
        length, flags, destination, destination_length);
}

ssize_t sendmsg(int fd, const msghdr* message, int flags) {
    initialize_originals();
    return cooperative_io<ssize_t>(fd, s_originals.m_sendmsg, IOEvent::kWrite,
                                    has_dontwait(flags), message, flags);
}

int close(int fd) {
    initialize_originals();
    ClosePlan plan;
    {
        DescriptorGuard lifecycle;
        plan = prepare_close(fd);
        notify_before_close(fd);
    }
    // 不要在真实 close 期间持有进程级描述符生命周期门；SO_LINGER 和文件系统
    // 描述符可能在此处阻塞。
    const int result = s_originals.m_close
                           ? invoke_native_blocking(s_originals.m_close, fd)
                           : invoke_native_blocking(&raw_close_fallback, fd);
    {
        DescriptorGuard lifecycle;
        finish_close(fd, plan.m_descriptor);
    }
    return result;
}

int dup(int old_fd) {
    initialize_originals();
    if (!s_originals.m_dup) {
        errno = ENOSYS;
        return -1;
    }
    DescriptorGuard lifecycle;
    const int new_fd = invoke_real(s_originals.m_dup, old_fd);
    if (new_fd >= 0) {
        try_clone_descriptor(old_fd, new_fd);
    }
    return new_fd;
}

int dup2(int old_fd, int new_fd) {
    initialize_originals();
    if (!s_originals.m_dup2) {
        errno = ENOSYS;
        return -1;
    }
    DescriptorGuard lifecycle;
    // dup2(old, old) 按文档是空操作。此时不要使描述符 token 失效，也不要
    // 唤醒无关的 waiter。
    if (old_fd == new_fd || new_fd < 0 || !source_fd_is_valid(old_fd)) {
        return invoke_real(s_originals.m_dup2, old_fd, new_fd);
    }

    // 目标由内核作为 dup2 的一部分关闭。在持有描述符生命周期门时发布关闭，
    // 之后替换系统调用才不会为旧描述产生过期就绪事件。
    const ClosePlan plan = prepare_close(new_fd);
    notify_before_close(new_fd);
    const int result = invoke_real(s_originals.m_dup2, old_fd, new_fd);
    if (result >= 0) {
        finish_close(new_fd, plan.m_descriptor);
        try_clone_descriptor(old_fd, result);
    } else {
        // 有效源和有效目标通常使 Linux 上的 dup2 不会失败。若内核仍拒绝操作，
        // 保留元数据。
        restore_failed_replacement(new_fd, plan.m_descriptor);
    }
    return result;
}

int dup3(int old_fd, int new_fd, int flags) {
    initialize_originals();
    if (!s_originals.m_dup3) {
        errno = ENOSYS;
        return -1;
    }
    DescriptorGuard lifecycle;
    // dup3 会在不触碰目标的情况下拒绝相同描述符和未知标志。对这些确定性失败
    // 不要发出伪关闭通知；源校验也让 dup3(-1, target, ...) 保持透明。
    if (old_fd == new_fd || new_fd < 0 ||
        (flags & ~O_CLOEXEC) != 0 || !source_fd_is_valid(old_fd)) {
        return invoke_real(s_originals.m_dup3, old_fd, new_fd, flags);
    }

    const ClosePlan plan = prepare_close(new_fd);
    notify_before_close(new_fd);
    const int result = invoke_real(s_originals.m_dup3, old_fd, new_fd, flags);
    if (result >= 0) {
        finish_close(new_fd, plan.m_descriptor);
        try_clone_descriptor(old_fd, result);
    } else {
        restore_failed_replacement(new_fd, plan.m_descriptor);
    }
    return result;
}

int fcntl(int fd, int command, ...) {
    initialize_originals();
    if (!s_originals.m_fcntl) {
        errno = ENOSYS;
        return -1;
    }

    const FcntlArgument kind = fcntl_argument(command);
    if (kind == FcntlArgument::Unknown) {
        // 读取未知的可变参数属于未定义行为。需要额外命令的宿主必须补充其
        // 确切的参数契约。
        errno = ENOTSUP;
        return -1;
    }
    va_list arguments;
    va_start(arguments, command);
    if (kind == FcntlArgument::Integer) {
        const int argument = va_arg(arguments, int);
        va_end(arguments);
        DescriptorGuard lifecycle;
        auto descriptor = descriptor_for(fd);
        if (command == F_SETFL && descriptor) {
            const bool user_nonblocking = (argument & O_NONBLOCK) != 0;
            const int result = invoke_real(s_originals.m_fcntl, fd, command,
                                           argument | O_NONBLOCK);
            if (result == 0) {
                std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
                descriptor->m_open->m_user_nonblocking = user_nonblocking;
                descriptor->m_open->m_system_nonblocking = true;
            }
            return result;
        }
        const int result =
            invoke_real(s_originals.m_fcntl, fd, command, argument);
        if (result >= 0 &&
            (command == F_DUPFD
#ifdef F_DUPFD_CLOEXEC
             || command == F_DUPFD_CLOEXEC
#endif
             )) {
            try_clone_descriptor(fd, result);
        }
        return result;
    }
    if (kind == FcntlArgument::Pointer) {
        void* argument = va_arg(arguments, void*);
        va_end(arguments);
        DescriptorGuard lifecycle;
        return invoke_real(s_originals.m_fcntl, fd, command, argument);
    }
    va_end(arguments);
    DescriptorGuard lifecycle;
    const int result = invoke_real(s_originals.m_fcntl, fd, command);
    if (command == F_GETFL && result >= 0) {
        if (auto descriptor = descriptor_for(fd)) {
            std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
            return descriptor->m_open->m_user_nonblocking
                       ? result | O_NONBLOCK
                       : result & ~O_NONBLOCK;
        }
    }
    return result;
}

int ioctl(int fd, unsigned long request, ...) {
    initialize_originals();
    if (!s_originals.m_ioctl) {
        errno = ENOSYS;
        return -1;
    }
    if (!ioctl_has_pointer_argument(request)) {
        DescriptorGuard lifecycle;
        if (ioctl_has_no_argument(request)) {
            return invoke_real(s_originals.m_ioctl, fd, request);
        }
        errno = ENOTSUP;
        return -1;
    }

    va_list arguments;
    va_start(arguments, request);
    void* argument = va_arg(arguments, void*);
    va_end(arguments);
    if (request == FIONBIO) {
        DescriptorGuard lifecycle;
        auto descriptor = descriptor_for(fd);
        if (descriptor && argument) {
            // 先让内核校验并读取调用者指针。Wrapper 侧解引用会把正常的 EFAULT
            // 契约变成无效用户地址导致的进程崩溃。
            const int result =
                invoke_real(s_originals.m_ioctl, fd, request, argument);
            if (result != 0) {
                return result;
            }

            // 通过内核可见标志恢复请求的状态，再恢复运行时的 O_NONBLOCK 位。
            // 这样既不触碰用户指针，又保留 cooperative_io() 使用的共享打开
            // 描述元数据。如果原始 fcntl 不可用或失败，最后尝试使用本地
            // FIONBIO 请求恢复；失败情况在下方按保守策略记录。
            bool user_nonblocking = false;
            bool system_nonblocking = false;
            if (s_originals.m_fcntl) {
                const int flags =
                    invoke_real(s_originals.m_fcntl, fd, F_GETFL);
                if (flags >= 0) {
                    user_nonblocking = (flags & O_NONBLOCK) != 0;
                    system_nonblocking = user_nonblocking;
                    if (!system_nonblocking &&
                        invoke_real(s_originals.m_fcntl, fd, F_SETFL,
                                    flags | O_NONBLOCK) == 0) {
                        system_nonblocking = true;
                    }
                }
            }
            if (!system_nonblocking) {
                int force_nonblocking = 1;
                system_nonblocking =
                    invoke_real(s_originals.m_ioctl, fd, FIONBIO,
                                &force_nonblocking) == 0;
            }
            {
                std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
                descriptor->m_open->m_user_nonblocking = user_nonblocking;
                descriptor->m_open->m_system_nonblocking =
                    system_nonblocking;
            }
            return result;
        }
    }
    DescriptorGuard lifecycle;
    return invoke_real(s_originals.m_ioctl, fd, request, argument);
}

int getsockopt(int fd, int level, int option, void* value,
               socklen_t* length) {
    initialize_originals();
    if (!s_originals.m_getsockopt) {
        errno = ENOSYS;
        return -1;
    }
    DescriptorGuard lifecycle;
    return invoke_real(s_originals.m_getsockopt, fd, level, option, value,
                       length);
}

int setsockopt(int fd, int level, int option, const void* value,
               socklen_t length) {
    initialize_originals();
    if (!s_originals.m_setsockopt) {
        errno = ENOSYS;
        return -1;
    }
    DescriptorGuard lifecycle;
    const int result = invoke_real(s_originals.m_setsockopt, fd, level, option,
                                   value, length);
    if (result != 0 || level != SOL_SOCKET ||
        (option != SO_RCVTIMEO && option != SO_SNDTIMEO) || !value ||
        length < sizeof(timeval)) {
        return result;
    }

    auto descriptor = descriptor_for(fd);
    if (!descriptor && cooperative_manager()) {
        descriptor = try_adopt_socket(fd);
    }
    if (descriptor) {
        const auto timeout = timeval_duration(*static_cast<const timeval*>(value));
        std::lock_guard<std::mutex> lock(descriptor->m_open->m_mutex);
        if (option == SO_RCVTIMEO) {
            descriptor->m_open->m_receive_timeout = timeout;
        } else {
            descriptor->m_open->m_send_timeout = timeout;
        }
    }
    return result;
}

}  // extern "C"
