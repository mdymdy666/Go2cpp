#include "go2cpp/io_backend.hpp"

#include <cerrno>
#include <mutex>
#include <unordered_map>
#include <utility>

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace go2cpp::io {
namespace {
constexpr std::uint64_t kWakeToken = 0;
constexpr std::uint32_t kRead = 0x01U;
constexpr std::uint32_t kWrite = 0x02U;
constexpr std::uint32_t kError = 0x04U;
constexpr std::uint32_t kHangup = 0x08U;

/// 返回后端注册表的全局互斥量；只保护注册表，不保护具体后端实例。
std::mutex& backend_mutex() {
    static std::mutex mutex;
    return mutex;
}

/// 返回命名后端工厂表；工厂复制到锁外后再执行，避免插件重入死锁。
std::unordered_map<std::string, IOBackendFactory>& backend_factories() {
    static std::unordered_map<std::string, IOBackendFactory> factories;
    return factories;
}

#ifdef __linux__
/**
 * @brief Linux epoll/eventfd 默认后端。
 * @details 只处理平台句柄，不理解 Fiber 和等待节点，故可被其它 poller 替换。
 */
class EpollBackend final : public IOBackend {
public:
    /// 释放 epoll/eventfd 资源；重复析构安全。
    ~EpollBackend() override { shutdown(); }

    /// 创建 epoll 和跨线程唤醒 eventfd，并安装内部唤醒事件。
    bool initialize(int* error) noexcept override {
        if (m_epoll_fd >= 0) { if (error) *error = 0; return true; }
        m_epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (m_epoll_fd < 0) { if (error) *error = errno ? errno : EIO; return false; }
        m_wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_wake_fd < 0) { const int e = errno ? errno : EIO; close_fds(); if (error) *error = e; return false; }
        epoll_event event{}; event.events = EPOLLIN; event.data.u64 = kWakeToken;
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_wake_fd, &event) != 0) {
            const int e = errno ? errno : EIO; close_fds(); if (error) *error = e; return false;
        }
        if (error) *error = 0;
        return true;
    }

    /// 将统一读写兴趣转换为 epoll 事件并注册代际 token。
    int add_or_modify(int fd, std::uint32_t interest, std::uint64_t token,
                      bool modify) noexcept override {
        if (m_epoll_fd < 0 || fd < 0 || token == kWakeToken) return EINVAL;
        epoll_event event{}; event.events = EPOLLET | EPOLLERR | EPOLLHUP;
        if (interest & kRead) event.events |= EPOLLIN | EPOLLRDHUP;
        if (interest & kWrite) event.events |= EPOLLOUT;
        event.data.u64 = token;
        int operation = modify ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        if (::epoll_ctl(m_epoll_fd, operation, fd, &event) == 0) return 0;
        const int first = errno ? errno : EIO;
        if (!modify && first == EEXIST) operation = EPOLL_CTL_MOD;
        else if (modify && first == ENOENT) operation = EPOLL_CTL_ADD;
        else return first;
        if (::epoll_ctl(m_epoll_fd, operation, fd, &event) == 0) return 0;
        return errno ? errno : EIO;
    }

    /// 删除 fd 的 epoll 注册；fd 已关闭时按幂等删除处理。
    int remove(int fd) noexcept override {
        if (m_epoll_fd < 0 || fd < 0) return EINVAL;
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == 0) return 0;
        return errno == ENOENT || errno == EBADF ? 0 : (errno ? errno : EIO);
    }

    /// 阻塞等待 epoll 事件并转换为平台无关的事件位。
    int wait(BackendEvent* output, std::size_t capacity,
             int timeout_ms) noexcept override {
        if (m_epoll_fd < 0 || output == nullptr || capacity == 0) return -EINVAL;
        constexpr std::size_t kMaxEvents = 256;
        const int bounded = static_cast<int>(capacity > kMaxEvents ? kMaxEvents : capacity);
        epoll_event events[kMaxEvents]{};
        int count;
        do { count = ::epoll_wait(m_epoll_fd, events, bounded, timeout_ms); }
        while (count < 0 && errno == EINTR);
        if (count < 0) return -(errno ? errno : EIO);
        for (int index = 0; index < count; ++index) {
            std::uint32_t mask = 0;
            const std::uint32_t value = events[index].events;
            if (value & (EPOLLIN | EPOLLRDHUP)) mask |= kRead;
            if (value & EPOLLOUT) mask |= kWrite;
            if (value & EPOLLERR) mask |= kError;
            if (value & EPOLLHUP) mask |= kHangup;
            output[index] = BackendEvent{events[index].data.u64, mask};
        }
        return count;
    }

    /// 向 eventfd 写入唤醒计数，使 poller 重新计算超时和注册表。
    void wake() noexcept override {
        if (m_wake_fd < 0) return;
        const std::uint64_t value = 1;
        (void)::syscall(SYS_write, m_wake_fd, &value, sizeof(value));
    }
    /// 由 poller 线程排空 eventfd，合并多个待处理唤醒。
    void drain_wake() noexcept override {
        if (m_wake_fd < 0) return;
        std::uint64_t value = 0;
        while (::syscall(SYS_read, m_wake_fd, &value, sizeof(value)) ==
               static_cast<long>(sizeof(value))) {}
    }
    /// 关闭 eventfd 和 epoll；调用后其它操作只返回错误，不再创建资源。
    void shutdown() noexcept override { close_fds(); }

private:
    /// 以原始系统调用关闭描述符，避免被用户 hook 再次拦截。
    void close_fds() noexcept {
        if (m_wake_fd >= 0) { (void)::syscall(SYS_close, m_wake_fd); m_wake_fd = -1; }
        if (m_epoll_fd >= 0) { (void)::syscall(SYS_close, m_epoll_fd); m_epoll_fd = -1; }
    }
    int m_epoll_fd{-1};
    int m_wake_fd{-1};
};
#endif

/// 延迟注册平台默认后端，保证静态初始化顺序可控。
void ensure_builtin_backend() {
    static const bool registered = [] {
#ifdef __linux__
        std::lock_guard<std::mutex> lock(backend_mutex());
        backend_factories().emplace("epoll", [] { return std::make_shared<EpollBackend>(); });
#endif
        return true;
    }();
    (void)registered;
}
}  // namespace

/// 函数功能：发布一个命名 IO 后端工厂。
/// 执行流程：校验名称和工厂，初始化内置后端，再在注册表锁内完成唯一性检查。
/// @param[in] name 后端名称；空名称不允许注册。
/// @param[in] factory 创建后端实例的工厂；不得返回长期依赖注册表锁的对象。
/// @return 首次注册成功返回 true；名称冲突或参数无效返回 false。
/// @note 工厂本身不会在注册表锁内执行。
bool RegisterBackend(std::string name, IOBackendFactory factory) {
    if (name.empty() || !factory) return false;
    ensure_builtin_backend();
    std::lock_guard<std::mutex> lock(backend_mutex());
    return backend_factories().emplace(std::move(name), std::move(factory)).second;
}

/// 函数功能：移除一个命名 IO 后端工厂。
/// @param[in] name 要移除的后端名称。
/// @return 找到并移除返回 true；名称为空或不存在返回 false。
/// @note 已经创建的后端实例不受注销影响，只阻止后续创建。
bool UnregisterBackend(const std::string& name) noexcept {
    if (name.empty()) return false;
    ensure_builtin_backend();
    std::lock_guard<std::mutex> lock(backend_mutex());
    return backend_factories().erase(name) != 0;
}

/// 函数功能：按照名称创建独立的 IO 后端实例。
/// @param[in] name 后端名称；为空时使用当前平台默认名称。
/// @return 创建成功返回实例；名称不存在、工厂抛异常或返回空时返回空指针。
/// @note 工厂复制后在注册表锁外执行，允许工厂重入注册其它后端。
IOBackendPtr CreateBackend(const std::string& name) {
    ensure_builtin_backend();
    const std::string requested = name.empty() ? DefaultBackendName() : name;
    IOBackendFactory factory;
    {
        std::lock_guard<std::mutex> lock(backend_mutex());
        const auto found = backend_factories().find(requested);
        if (found == backend_factories().end()) return {};
        factory = found->second;
    }
    try { return factory ? factory() : IOBackendPtr{}; } catch (...) { return {}; }
}

/// 函数功能：返回当前平台的默认 IO 后端名称。
/// @return Linux 返回 epoll；没有内置平台实现时返回空字符串。
std::string DefaultBackendName() {
#ifdef __linux__
    return "epoll";
#else
    return {};
#endif
}
}  // namespace go2cpp::io
