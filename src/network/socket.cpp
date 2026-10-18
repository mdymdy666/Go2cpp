/**
 * @file socket.cpp
 * @brief 实现 Socket 的 RAII 生命周期、poll/IOManager 等待和 partial I/O。
 * @details 系统调用只在锁外执行；Fiber 线程使用 IOManager park，普通线程使用
 *          poll，close 会先通知等待者再删除元数据和关闭 fd。
 */
#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/fd_manager.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <algorithm>
#include <cstring>
#include <climits>

namespace go2cpp::net {

namespace {
std::chrono::milliseconds remaining(std::chrono::milliseconds timeout,
                                    std::chrono::steady_clock::time_point start) {
    if (timeout == std::chrono::milliseconds::max()) return timeout;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (elapsed >= timeout) return std::chrono::milliseconds(0);
    return timeout - elapsed;
}

int poll_wait(int fd, short events, std::chrono::milliseconds timeout) {
    pollfd descriptor{fd, events, 0};
    const int milliseconds = timeout == std::chrono::milliseconds::max()
        ? -1 : static_cast<int>(std::min<std::int64_t>(timeout.count(), INT_MAX));
    for (;;) {
        const int result = ::poll(&descriptor, 1, milliseconds);
        if (result >= 0) {
            if (result == 0) return ETIMEDOUT;
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) return descriptor.revents & POLLNVAL ? EBADF : 0;
            return 0;
        }
        if (errno != EINTR) return errno;
    }
}
}

Socket::Socket(int family, int type, int protocol, int fd, bool owner)
    : m_fd(fd), m_family(family), m_type(type), m_protocol(protocol), m_owner(owner) {
    if (m_fd >= 0) {
        const int saved_errno = errno;
        if (!init_nonblock()) {
            const int error = errno == 0 ? EIO : errno;
            // owner=false 时 fd 仍归调用方，不能替调用方关闭；owner=true
            // 时构造失败必须释放 fd，避免留下不可等待的阻塞句柄。
            if (m_owner) {
                (void)::close(m_fd);
            }
            m_fd = -1;
            errno = error;
            return;
        }
        errno = saved_errno;
        FdManager::instance().get(m_fd, true);
    }
}
Socket::~Socket() { close(); }
Socket::Ptr Socket::FromFd(int fd, int family, int type, int protocol, bool owner) {
    if (fd < 0) return {};
    auto socket = Ptr(new Socket(family, type, protocol, fd, owner));
    if (!socket->valid()) return {};
    return socket;
}
Socket::Ptr Socket::CreateTCP(int family) {
    const int fd = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    return fd < 0 ? Ptr{} : FromFd(fd, family, SOCK_STREAM, 0);
}
Socket::Ptr Socket::CreateUDP(int family) {
    const int fd = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    return fd < 0 ? Ptr{} : FromFd(fd, family, SOCK_DGRAM, 0);
}
Socket::Ptr Socket::CreateUnix(bool datagram) {
    const int type = datagram ? SOCK_DGRAM : SOCK_STREAM;
    const int fd = ::socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    return fd < 0 ? Ptr{} : FromFd(fd, AF_UNIX, type, 0);
}
/**
 * @brief 登记一次在途操作，并把当前 fd 交给调用方。
 *
 * ## 定位
 * `Socket` 的每个系统调用（recv/send/accept/connect/bind/listen/set_option…）
 * 都必须先走这里：它保证「关闭」不会在系统调用中途把 fd 抽走。上层的
 * `SocketStream`、`http2::FiberTransport`、benchmark 的服务端循环都依赖它。
 *
 * ## 步骤
 * step1. `shared_from_this()` 取一份自引用 —— 即使调用方随后释放最后一个
 *        `Socket::Ptr`，对象也会活到 `end_operation()`；
 * step2. 在 `m_mutex` 下检查 `m_closing`/`m_fd`，已关闭则 EBADF 返回；
 * step3. 首个在途操作把自引用存进 `m_inflight_keepalive`（只在 inflight==0
 *        时写，避免每次操作都改这个字段）；
 * step4. `++m_inflight` 后把 fd 拷给调用方。
 *
 * ## 配对
 * 必须与 `end_operation()` 成对；漏掉会让 `release()` 永远等不到 inflight 归零，
 * 关闭路径卡死。
 */
bool Socket::begin_operation(int& fd) noexcept {
    std::shared_ptr<Socket> keep_alive;
    try {
        keep_alive = shared_from_this();
    } catch (...) {
        errno = EBADF;
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closing || m_fd < 0) { errno = EBADF; return false; }
    if (m_inflight == 0) {
        m_inflight_keepalive = std::move(keep_alive);
    }
    fd = m_fd;
    ++m_inflight;
    return true;
}
/**
 * @brief 结束一次在途操作；关闭正在等待时，这里是收尾点。
 *
 * ## 步骤
 * step1. `--m_inflight`，在锁内判断是否成为最后一个在途操作；
 * step2. 归零时把 `m_inflight_keepalive` 移到局部变量 —— 保证锁外收尾期间
 *        对象不被析构；
 * step3. 若 `m_closing` 且归零：**先** `notify_all` 唤醒 `wait_close_state()`
 *        的等待者，**再** `finalize_deferred_close()`（顺序不能反：关闭者需要
 *        先看到 inflight==0 的谓词成立才继续）。
 */
void Socket::end_operation() noexcept {
    bool notify = false;
    std::shared_ptr<Socket> keep_alive;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_inflight > 0) --m_inflight;
        notify = m_closing && m_inflight == 0;
        if (m_inflight == 0) {
            // 将 self-reference 移到局部变量，确保锁外收尾完成前对象仍存活。
            keep_alive = std::move(m_inflight_keepalive);
        }
    }
    if (notify) {
        // 等待者在同一把 m_mutex 下检查 m_inflight 并发布 ParkingCondition
        // 节点，因此更新谓词后再通知不会丢失唤醒。
        m_operation_condition.notify_all();
        finalize_deferred_close();
    }
}
bool Socket::wait_close_state(bool wait_fd_closed) noexcept {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_operation_condition.wait(lock, [this, wait_fd_closed] {
        return wait_fd_closed ? (m_fd < 0) : (m_inflight == 0);
    });
}
void Socket::finalize_deferred_close() noexcept {
    int fd = -1;
    bool owner = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_close_pending || m_inflight != 0 || m_fd < 0) return;
        // 在状态锁内一次性摘除 fd，保证只有一个最终关闭者。
        fd = m_fd;
        owner = m_owner;
        m_fd = -1;
        m_owner = false;
        m_connected = false;
        m_close_pending = false;
    }
    FdManager::instance().erase(fd);
    if (owner) (void)::close(fd);
    m_operation_condition.notify_all();
}
bool Socket::is_closing() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closing || m_fd < 0;
}
int Socket::fd() const noexcept { std::lock_guard<std::mutex> lock(m_mutex); return m_fd; }
bool Socket::valid() const noexcept { return fd() >= 0; }
bool Socket::connected() const noexcept { std::lock_guard<std::mutex> lock(m_mutex); return m_connected; }
Socket::Duration Socket::recv_timeout() const noexcept { std::lock_guard<std::mutex> lock(m_mutex); return m_recv_timeout; }
Socket::Duration Socket::send_timeout() const noexcept { std::lock_guard<std::mutex> lock(m_mutex); return m_send_timeout; }
bool Socket::init_nonblock() {
    // 先用**原始** syscall 读标志，避免进入 Hook 的 fcntl。
    //
    // Hook 版的 fcntl 会持有全局描述符注册表递归锁直到真实 syscall 返回
    // （close/fd 复用线性化需要它）。短连接场景下每个 accept 都要构造
    // Socket，两个 fcntl 就是两次全局锁 + 两次 syscall；WSL2 实测这一个
    // 环节占 accept 路径的 ~150µs，直接把短连接吞吐压到 5k/s。
    //
    // accept4 已带 SOCK_NONBLOCK，绝大多数情况下标志已经置位，因此这里
    // 只读不写；只有确实需要**改变**状态时才走 Hook 版 fcntl，让
    // m_system_nonblocking/m_user_nonblocking 元数据与内核保持一致。
#ifdef SYS_fcntl
    const long flags = ::syscall(SYS_fcntl, m_fd, F_GETFL, 0);
#else
    const long flags = ::fcntl(m_fd, F_GETFL, 0);
#endif
    if (flags < 0) {
        return false;
    }
    if ((flags & O_NONBLOCK) == 0 &&
        ::fcntl(m_fd, F_SETFL, static_cast<int>(flags) | O_NONBLOCK) != 0) {
        return false;
    }
    FdManager::instance().set_nonblock(m_fd, false, true);
    return true;
}
int Socket::last_error() const noexcept {
    int error = 0; socklen_t length = sizeof(error);
    if (!get_option(SOL_SOCKET, SO_ERROR, &error, &length)) return errno;
    return error;
}
int Socket::release() noexcept {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_fd < 0) { m_owner = false; return -1; }
    if (m_closing) {
        lock.unlock();
        (void)wait_close_state(true);
        lock.lock();
        m_owner = false;
        return -1;
    }
    const int fd = m_fd;
    m_closing = true;
    ++m_generation;
    lock.unlock();
    io::IOManager::NotifyCloseAll(fd);
    if (!wait_close_state(false)) {
        // 无法同步等待时不能把 fd 返回给调用方。返回 -1，并由最后一个
        // 在途操作或此处负责最终关闭，避免 fd 所有权无人承接。
        {
            std::lock_guard<std::mutex> pending_lock(m_mutex);
            m_close_pending = true;
        }
        finalize_deferred_close();
        return -1;
    }
    lock.lock();
    m_fd = -1; m_owner = false; m_connected = false;
    lock.unlock();
    if (fd >= 0) FdManager::instance().erase(fd);
    m_operation_condition.notify_all();
    return fd;
}
void Socket::set_waiter(std::shared_ptr<SocketWaiter> waiter) noexcept { std::lock_guard<std::mutex> lock(m_mutex); m_waiter = std::move(waiter); }

bool Socket::bind(const Address& address) {
    int fd = -1; if (!begin_operation(fd)) return false;
    const bool result = ::bind(fd, address.addr(), address.addr_len()) == 0;
    end_operation(); return result;
}
bool Socket::listen(int backlog) {
    int fd = -1; if (!begin_operation(fd)) return false;
    const bool result = ::listen(fd, backlog) == 0;
    end_operation(); return result;
}
Socket::Ptr Socket::accept(Duration timeout) {
    const auto start = std::chrono::steady_clock::now();
    const auto cancel_generation = m_read_cancel_generation.load(std::memory_order_acquire);
    int fd = -1; if (!begin_operation(fd)) return {};
    if (m_read_cancel_generation.load(std::memory_order_acquire) != cancel_generation) {
        errno = ECANCELED;
        end_operation();
        return {};
    }
    for (;;) {
        sockaddr_storage storage{}; socklen_t length = sizeof(storage);
        const int accepted = ::accept4(fd, reinterpret_cast<sockaddr*>(&storage), &length,
                                       SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (accepted >= 0) {
            auto result = FromFd(accepted, storage.ss_family, m_type, m_protocol);
            if (result) result->m_connected = true;
            end_operation(); return result;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) { end_operation(); return {}; }
        auto left = remaining(timeout, start);
        if (left.count() == 0 && timeout != Duration::max()) { errno = ETIMEDOUT; end_operation(); return {}; }
        if (wait_ready(fd, io::IOEvent::kRead, left, cancel_generation) != 0) {
            end_operation();
            return {};
        }
    }
}
bool Socket::connect(const Address& address, Duration timeout) {
    const auto cancel_generation = m_write_cancel_generation.load(std::memory_order_acquire);
    int fd = -1; if (!begin_operation(fd)) return false;
    if (m_write_cancel_generation.load(std::memory_order_acquire) != cancel_generation) {
        errno = ECANCELED;
        end_operation();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_remote = std::make_shared<UnknownAddress>(address.addr(), address.addr_len());
    }
    const int result = ::connect(fd, address.addr(), address.addr_len());
    if (result == 0) { { std::lock_guard<std::mutex> lock(m_mutex); m_connected = true; } end_operation(); return true; }
    if (errno != EINPROGRESS && errno != EALREADY) { end_operation(); return false; }
    const auto start = std::chrono::steady_clock::now();
    auto left = remaining(timeout, start);
    const int wait_error = wait_ready(fd, io::IOEvent::kWrite, left, cancel_generation);
    if (wait_error != 0) { errno = wait_error; end_operation(); return false; }
    int error = 0; socklen_t length = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
        if (error) errno = error;
        end_operation();
        return false;
    }
    { std::lock_guard<std::mutex> lock(m_mutex); m_connected = true; }
    end_operation(); return true;
}
bool Socket::reconnect(Duration timeout) {
    AddressPtr remote;
    { std::lock_guard<std::mutex> lock(m_mutex); remote = m_remote; m_connected = false; }
    if (!remote) return false;
    return connect(*remote, timeout);
}
bool Socket::close() noexcept {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_fd < 0) return true;
    if (m_closing) {
        lock.unlock();
        return wait_close_state(true);
    }
    const int fd = m_fd;
    m_closing = true;
    ++m_generation;
    const bool owner = m_owner;
    lock.unlock();
    io::IOManager::NotifyCloseAll(fd);
    if (!wait_close_state(false)) {
        // 调度器已停止或 Fiber 无法 park 时，不占用 carrier 等待；最终关闭
        // 责任交给最后一个在途操作。若它已结束，则当前线程会完成关闭。
        {
            std::lock_guard<std::mutex> pending_lock(m_mutex);
            m_close_pending = true;
        }
        finalize_deferred_close();
        return false;
    }
    lock.lock();
    m_fd = -1; m_connected = false;
    lock.unlock();
    FdManager::instance().erase(fd);
    m_operation_condition.notify_all();
    return !owner || ::close(fd) == 0;
}
bool Socket::shutdown(int how) noexcept {
    int fd = -1; if (!begin_operation(fd)) return false;
    const bool result = ::shutdown(fd, how) == 0;
    end_operation(); return result;
}

int Socket::wait_ready(int fd, io::IOEvent event, Duration timeout,
                       std::uint64_t cancel_generation) {
    const auto cancelled = [this, event, cancel_generation] {
        const auto current = event == io::IOEvent::kRead
                                 ? m_read_cancel_generation.load(std::memory_order_acquire)
                                 : m_write_cancel_generation.load(std::memory_order_acquire);
        return current != cancel_generation;
    };
    std::shared_ptr<SocketWaiter> waiter;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        waiter = m_waiter;
    }
    if (fd < 0) { errno = EBADF; return EBADF; }
    if (cancelled()) { errno = ECANCELED; return ECANCELED; }
    if (is_closing()) { errno = ECANCELED; return ECANCELED; }
    if (waiter) {
        const int result = waiter->wait(fd, event, timeout);
        if (cancelled()) { errno = ECANCELED; return ECANCELED; }
        return result;
    }
    if (auto* manager = io::IOManager::current()) {
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            auto result = manager->wait_for(
                fd, event, timeout == Duration::max() || timeout > Duration(100)
                        ? Duration(100)
                        : timeout);
            if (cancelled()) { errno = ECANCELED; return ECANCELED; }
            if (result.ready()) return 0;
            if (result.status != io::WaitStatus::kTimeout) {
                return result.system_error ? result.system_error : ECANCELED;
            }
            if (is_closing()) { errno = ECANCELED; return ECANCELED; }
            if (timeout != Duration::max() && remaining(timeout, started).count() == 0) {
                errno = ETIMEDOUT;
                return ETIMEDOUT;
            }
        }
    }
    // 普通线程没有 IOManager 的取消通知，按小片轮询检查关闭状态，避免
    // close() 永久等待一个无限期 poll。每片仍然由内核阻塞，不会忙等待。
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        Duration slice = timeout;
        if (slice == Duration::max() || slice > Duration(100)) slice = Duration(100);
        const int result = poll_wait(fd, event == io::IOEvent::kRead ? POLLIN : POLLOUT, slice);
        if (cancelled()) { errno = ECANCELED; return ECANCELED; }
        if (result != ETIMEDOUT) return result;
        if (is_closing()) { errno = ECANCELED; return ECANCELED; }
        if (timeout != Duration::max() && remaining(timeout, started).count() == 0) {
            errno = ETIMEDOUT;
            return ETIMEDOUT;
        }
    }
}
bool Socket::wait_or_retry(int fd, io::IOEvent event, Duration& left,
                           std::chrono::steady_clock::time_point started,
                           std::uint64_t cancel_generation) {
    left = remaining(left, started);
    if (left.count() == 0 && left != Duration::max()) { errno = ETIMEDOUT; return false; }
    const int error = wait_ready(fd, event, left, cancel_generation);
    if (error != 0) { errno = error; return false; }
    left = remaining(left, started); return true;
}
SocketResult Socket::send_impl(int fd, const msghdr* message, int flags,
                               Duration timeout, std::uint64_t cancel_generation) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (is_closing() || m_write_cancel_generation.load(std::memory_order_acquire) !=
                                cancel_generation) return {-1, ECANCELED};
        const ssize_t count = ::sendmsg(fd, message, flags | MSG_NOSIGNAL);
        if (count >= 0) return {count, 0};
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return {-1, errno};
        auto left = timeout; if (!wait_or_retry(fd, io::IOEvent::kWrite, left, start,
                                                 cancel_generation)) return {-1, errno};
    }
}

SocketResult Socket::recv_impl(int fd, msghdr* message, int flags, Duration timeout,
                               std::uint64_t cancel_generation) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (is_closing() || m_read_cancel_generation.load(std::memory_order_acquire) !=
                                cancel_generation) return {-1, ECANCELED};
        const ssize_t count = ::recvmsg(fd, message, flags);
        if (count >= 0) return {count, 0};
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return {-1, errno};
        auto left = timeout; if (!wait_or_retry(fd, io::IOEvent::kRead, left, start,
                                                cancel_generation)) return {-1, errno};
    }
}
SocketResult Socket::send(const void* data, std::size_t length, int flags, Duration timeout) {
    iovec vector{const_cast<void*>(data), length}; return sendv(&vector, 1, flags, timeout);
}
SocketResult Socket::recv(void* data, std::size_t length, int flags, Duration timeout) {
    iovec vector{data, length}; return recvv(&vector, 1, flags, timeout);
}
SocketResult Socket::sendv(const iovec* buffers, std::size_t count, int flags, Duration timeout) {
    const auto cancel_generation = m_write_cancel_generation.load(std::memory_order_acquire);
    int fd = -1; if (!begin_operation(fd)) return {-1, errno};
    if (m_write_cancel_generation.load(std::memory_order_acquire) != cancel_generation) {
        end_operation();
        return {-1, ECANCELED};
    }
    msghdr message{}; message.msg_iov = const_cast<iovec*>(buffers); message.msg_iovlen = count;
    Duration effective = timeout;
    if (effective == Duration::max()) { std::lock_guard<std::mutex> lock(m_mutex); effective = m_send_timeout; }
    const auto result = send_impl(fd, &message, flags, effective, cancel_generation);
    end_operation();
    return result;
}
SocketResult Socket::recvv(iovec* buffers, std::size_t count, int flags, Duration timeout) {
    const auto cancel_generation = m_read_cancel_generation.load(std::memory_order_acquire);
    int fd = -1; if (!begin_operation(fd)) return {-1, errno};
    if (m_read_cancel_generation.load(std::memory_order_acquire) != cancel_generation) {
        end_operation();
        return {-1, ECANCELED};
    }
    msghdr message{}; message.msg_iov = buffers; message.msg_iovlen = count;
    Duration effective = timeout;
    if (effective == Duration::max()) { std::lock_guard<std::mutex> lock(m_mutex); effective = m_recv_timeout; }
    const auto result = recv_impl(fd, &message, flags, effective, cancel_generation);
    end_operation();
    return result;
}
SocketResult Socket::send_to(const void* data, std::size_t length, const Address& to, int flags, Duration timeout) {
    const auto cancel_generation = m_write_cancel_generation.load(std::memory_order_acquire);
    int fd = -1; if (!begin_operation(fd)) return {-1, errno};
    if (m_write_cancel_generation.load(std::memory_order_acquire) != cancel_generation) {
        end_operation();
        return {-1, ECANCELED};
    }
    iovec vector{const_cast<void*>(data), length}; msghdr message{}; message.msg_name = const_cast<sockaddr*>(to.addr());
    message.msg_namelen = to.addr_len(); message.msg_iov = &vector; message.msg_iovlen = 1;
    Duration effective = timeout;
    if (effective == Duration::max()) { std::lock_guard<std::mutex> lock(m_mutex); effective = m_send_timeout; }
    const auto result = send_impl(fd, &message, flags, effective, cancel_generation);
    end_operation();
    return result;
}
SocketResult Socket::recv_from(void* data, std::size_t length, AddressPtr& from, int flags, Duration timeout) {
    const auto cancel_generation = m_read_cancel_generation.load(std::memory_order_acquire);
    int fd = -1; if (!begin_operation(fd)) return {-1, errno};
    if (m_read_cancel_generation.load(std::memory_order_acquire) != cancel_generation) {
        end_operation();
        return {-1, ECANCELED};
    }
    iovec vector{data, length}; sockaddr_storage storage{}; msghdr message{}; message.msg_name = &storage;
    message.msg_namelen = sizeof(storage); message.msg_iov = &vector; message.msg_iovlen = 1;
    Duration effective = timeout;
    if (effective == Duration::max()) { std::lock_guard<std::mutex> lock(m_mutex); effective = m_recv_timeout; }
    auto result = recv_impl(fd, &message, flags, effective, cancel_generation);
    if (result.ok()) from = Address::Create(reinterpret_cast<sockaddr*>(&storage), message.msg_namelen);
    end_operation();
    return result;
}
AddressPtr Socket::local_address() const {
    int fd = -1;
    if (!const_cast<Socket*>(this)->begin_operation(fd)) return {};
    sockaddr_storage storage{}; socklen_t length = sizeof(storage);
    const bool ok = ::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) == 0;
    const auto address = ok ? Address::Create(reinterpret_cast<sockaddr*>(&storage), length) : AddressPtr{};
    const_cast<Socket*>(this)->end_operation();
    return address;
}
AddressPtr Socket::remote_address() const {
    int fd = -1;
    if (!const_cast<Socket*>(this)->begin_operation(fd)) return {};
    sockaddr_storage storage{}; socklen_t length = sizeof(storage);
    const bool ok = ::getpeername(fd, reinterpret_cast<sockaddr*>(&storage), &length) == 0;
    const auto address = ok ? Address::Create(reinterpret_cast<sockaddr*>(&storage), length) : AddressPtr{};
    const_cast<Socket*>(this)->end_operation();
    return address;
}
bool Socket::set_recv_timeout(Duration timeout) {
    if (timeout.count() < 0) return false;
    int fd = -1; if (!begin_operation(fd)) return false;
    { std::lock_guard<std::mutex> lock(m_mutex); m_recv_timeout = timeout; }
    FdManager::instance().set_timeout(fd, true, timeout);
    end_operation();
    return true;
}
bool Socket::set_send_timeout(Duration timeout) {
    if (timeout.count() < 0) return false;
    int fd = -1; if (!begin_operation(fd)) return false;
    { std::lock_guard<std::mutex> lock(m_mutex); m_send_timeout = timeout; }
    FdManager::instance().set_timeout(fd, false, timeout);
    end_operation();
    return true;
}
bool Socket::set_option(int level, int option, const void* value, socklen_t length) {
    int fd = -1; if (!begin_operation(fd)) return false;
    const bool result = ::setsockopt(fd, level, option, value, length) == 0;
    end_operation(); return result;
}
bool Socket::get_option(int level, int option, void* value, socklen_t* length) const {
    int fd = -1; if (!const_cast<Socket*>(this)->begin_operation(fd)) return false;
    const bool result = ::getsockopt(fd, level, option, value, length) == 0;
    const_cast<Socket*>(this)->end_operation(); return result;
}
// 发布读取消代际，并尽可能通知当前线程的 IOManager；代际检查覆盖跨线程调用。
bool Socket::cancel_read() noexcept {
    m_read_cancel_generation.fetch_add(1, std::memory_order_acq_rel);
    int fd = -1; if (!begin_operation(fd)) return false;
    auto* manager = io::IOManager::current();
    if (manager) {
        (void)manager->cancel(fd, io::IOEvent::kRead);
    }
    end_operation();
    return true;
}
// 发布写取消代际，并尽可能通知当前线程的 IOManager；没有 IOManager 时由分片等待退出。
bool Socket::cancel_write() noexcept {
    m_write_cancel_generation.fetch_add(1, std::memory_order_acq_rel);
    int fd = -1; if (!begin_operation(fd)) return false;
    auto* manager = io::IOManager::current();
    if (manager) {
        (void)manager->cancel(fd, io::IOEvent::kWrite);
    }
    end_operation();
    return true;
}

SocketStream::SocketStream(Socket::Ptr socket, bool owner) : m_socket(std::move(socket)), m_owner(owner) {}
SocketStream::~SocketStream() { if (m_owner) close(); }
ssize_t SocketStream::read(void* data, std::size_t length) { auto result = m_socket ? m_socket->recv(data, length) : SocketResult{-1, EBADF}; return result.ok() ? result.transferred : -result.error; }
ssize_t SocketStream::write(const void* data, std::size_t length) { auto result = m_socket ? m_socket->send(data, length) : SocketResult{-1, EBADF}; return result.ok() ? result.transferred : -result.error; }
ssize_t SocketStream::readv(iovec* buffers, std::size_t count) { auto result = m_socket ? m_socket->recvv(buffers, count) : SocketResult{-1, EBADF}; return result.ok() ? result.transferred : -result.error; }
ssize_t SocketStream::writev(const iovec* buffers, std::size_t count) { auto result = m_socket ? m_socket->sendv(buffers, count) : SocketResult{-1, EBADF}; return result.ok() ? result.transferred : -result.error; }
bool SocketStream::write_all(const void* data, std::size_t length) { const auto* bytes = static_cast<const std::uint8_t*>(data); while (length) { const auto result = write(bytes, length); if (result <= 0) return false; bytes += result; length -= static_cast<std::size_t>(result); } return true; }
bool SocketStream::close() noexcept { return m_socket ? m_socket->close() : true; }
bool SocketStream::connected() const noexcept { return m_socket && m_socket->connected(); }

AddressPtr ResolveTCPAddr(const std::string& host, std::uint16_t port, int family) {
    return Address::LookupAny(host + ":" + std::to_string(port), family, SOCK_STREAM, IPPROTO_TCP);
}
AddressPtr ResolveUDPAddr(const std::string& host, std::uint16_t port, int family) {
    return Address::LookupAny(host + ":" + std::to_string(port), family, SOCK_DGRAM, IPPROTO_UDP);
}
Socket::Ptr DialTCP(const std::string& host, std::uint16_t port, Socket::Duration timeout) {
    auto address = ResolveTCPAddr(host, port); if (!address) return {};
    auto socket = Socket::CreateTCP(address->family()); return socket && socket->connect(*address, timeout) ? socket : Socket::Ptr{};
}
Socket::Ptr DialUDP(const std::string& host, std::uint16_t port, Socket::Duration timeout) {
    auto address = ResolveUDPAddr(host, port); if (!address) return {};
    auto socket = Socket::CreateUDP(address->family()); if (!socket || !socket->connect(*address, timeout)) return {}; return socket;
}
Socket::Ptr ListenTCP(const std::string& host, std::uint16_t port, int backlog) {
    auto address = ResolveTCPAddr(host, port); if (!address) return {};
    auto socket = Socket::CreateTCP(address->family()); if (!socket) return {};
    int reuse = 1; socket->set_option(SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    return socket->bind(*address) && socket->listen(backlog) ? socket : Socket::Ptr{};
}
Socket::Ptr ListenUDP(const std::string& host, std::uint16_t port) {
    auto address = ResolveUDPAddr(host, port); if (!address) return {};
    auto socket = Socket::CreateUDP(address->family()); return socket && socket->bind(*address) ? socket : Socket::Ptr{};
}

}  // namespace go2cpp::net
