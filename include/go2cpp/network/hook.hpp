#pragma once

#include "go2cpp/io.hpp"
#include "go2cpp/thread_policy.hpp"

#include <sys/socket.h>
#include <sys/uio.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <ctime>
#include <chrono>
#include <cstdint>
#include <string>

// 前置声明：避免 hook.hpp 把所有包含者都拖进 config.hpp。
namespace go2cpp::config { class Config; }

namespace go2cpp::hook {

/// 当前线程 Hook 模式的类型别名，便于在 go2cpp::hook 命名空间使用。
using ThreadHookMode = ::go2cpp::ThreadHookMode;
/// Hook 模式作用域守卫类型别名。
using ScopedThreadHookMode = ::go2cpp::ScopedThreadHookMode;
/** @brief 查询当前线程的 Hook 模式。 */
inline ThreadHookMode CurrentThreadMode() noexcept {
    return ::go2cpp::CurrentThreadHookMode();
}
/** @brief 设置当前线程 Hook 模式；线程退出或守卫销毁时可恢复。 */
inline void SetThreadMode(ThreadHookMode mode) noexcept {
    ::go2cpp::SetCurrentThreadHookMode(mode);
}

// Hook 默认在进程范围启用。IOManager 所有的 Fiber 使用 epoll；没有
// IOManager 的受管 Fiber 在延迟接管 socket 后使用有界的原生 poll 后备。
// 普通线程和非 socket 阻塞 IO 通常直接进入 libc；已经接管的 socket 因
// 内核标志保持非阻塞而使用原生 poll 后备。已跟踪 fd 以及未知的可变参数
// fcntl/ioctl 命令遵循下方明确的元数据/ENOTSUP 约定，不承诺无条件转发
// 到 libc。关闭 Hook 后不再接纳新的 Fiber/epoll 等待，socket 上已有的
// 元数据在 close 前仍可能使用原生后备。未知可变参数命令返回 ENOTSUP，
// 不会读取未知类型参数或转发无效 ABI 状态。
/** @brief 开启或关闭进程级 Hook 实现。 */
void set_enabled(bool enabled) noexcept;
/** @brief 返回当前进程级 Hook 是否启用。 */
bool enabled() noexcept;
inline void SetEnabled(bool value) noexcept { set_enabled(value); }
inline bool IsEnabled() noexcept { return enabled(); }

// 绑定是可选的线程级后备。通常 IOManager::current() 会从当前运行的调度器
// Fiber 发现管理器。Fiber 迁移到其他 OS 线程时，不要继续保留显式绑定。
/** @brief 将当前线程绑定到 IOManager，空指针表示解除绑定。 */
void bind_io_manager(IOManager* manager) noexcept;
/** @brief 返回当前线程绑定的 IOManager。 */
IOManager* bound_io_manager() noexcept;
inline void BindIOManager(IOManager* manager) noexcept {
    bind_io_manager(manager);
}
inline IOManager* BoundIOManager() noexcept { return bound_io_manager(); }

/** @brief 设置 Hook connect 调用使用的默认超时。 */
void set_connect_timeout(std::chrono::milliseconds timeout) noexcept;
/** @brief 读取当前 Hook connect 默认超时。 */
std::chrono::milliseconds connect_timeout() noexcept;

// ---------------------------------------------------------------------------
// 每函数动态开关
// ---------------------------------------------------------------------------

/**
 * @brief 被 Hook 的 libc 操作。**每个操作有独立的运行时开关**。
 *
 * ## 为什么要有它
 *
 * 全局开关（`set_enabled`）是"全开或全关"，但真实场景常常只需要关掉**某一个**：
 * 比如把日志文件写到普通文件时，`write` 走 Hook 毫无意义（普通文件几乎不会
 * EAGAIN，走 Hook 只是白 park 一次）；又比如接一个自带事件循环的第三方库时，
 * 只希望 `poll`/`epoll_wait` 不被接管，其它照旧。
 *
 * ## 存储与开销
 *
 * 所有开关压在一个 `std::atomic<uint32_t>` 位图里，检查是
 * **一次 relaxed load + 移位 + 与**（约 1~2 ns），无锁、无分支预测失败开销。
 * 开关检查在每次 syscall 之前执行，所以**绝不能用 mutex**。
 *
 * ## 危险点（务必先读再关）
 *
 * 1. **不对称风险**：只关 `read` 不关 `write`（或反之）会让同一个 fd 的
 *    等待/唤醒不对称 —— 关闭的那一侧会在 worker 线程上真实阻塞，
 *    而另一侧仍会 park Fiber。**在同一个连接上混用会导致吞吐骤降甚至卡死**；
 * 2. **在 Fiber 内关 `read`/`recv`/`write`/`send`**：这些调用将**阻塞整个
 *    worker 线程**（正是 Hook 要避免的事）。关掉它们只在你确定该 fd 是非阻塞
 *    的、或你就是要一次同步阻塞调用时才合理；
 * 3. **关 `sleep`**：`sleep_for`/`nanosleep` 会真实阻塞 worker 线程，期间该
 *    worker 上的其它 Fiber 全部停摆；
 * 4. **动态切换的时机**：如果某个 Fiber **已经 park 在** `read` 上，此刻把
 *    `read` 关掉**不会**唤醒它 —— 它仍在等 fd 可读，下一次该 fd 可读时照常
 *    被唤醒。反过来，关掉之后新发起的 `read` 直接走原生阻塞。**不存在
 *    "已 park 的 Fiber 永远醒不来"**，因为唤醒路径不查这个开关；
 * 5. **`close`/`shutdown`/`dup` 这类不阻塞的操作**：关掉它们只是让 fd 元数据
 *    不再更新，可能导致后续 `descriptor_for` 拿到过期信息。**不建议关**。
 */
enum class HookOp : std::uint8_t {
    kRead = 0,
    kReadv,
    kRecv,
    kRecvfrom,
    kWrite,
    kWritev,
    kSend,
    kSendto,
    kConnect,
    kAccept,
    kSleep,
    // 下面三个与 sleep 同源：都走 `cooperative_sleep_for`（eventfd + WaitFor 带超时），
    // 即「等到某个时间点」这条唤醒路径，不需要 timerfd 桥接。
    kNanosleep,
    kUsleep,
    kClockNanosleep,
    kPoll,
    kSelect,
    kEpollWait,
    kClose,
    kShutdown,
    kFcntl,
    kDup,
    kDup2,
    kCount,
};

/** @brief 操作名的稳定字符串（用于配置键与诊断）。 */
const char* ToString(HookOp op) noexcept;
/** @brief 由字符串解析操作；失败返回 false。 */
bool OpFromString(const std::string& name, HookOp* out) noexcept;

/** @brief 查询某个操作当前是否接受 Hook。 */
bool OpEnabled(HookOp op) noexcept;
/**
 * @brief 设置某个操作的开关（线程安全，无锁）。
 * @param op 目标操作。
 * @param enabled 是否接受 Hook。
 * @return `true` 表示设置已生效；`false` 表示**请求被拒绝**。
 *
 * @warning **有了 `*_f` 之后，这个开关是危险且不必要的。**
 *
 *          它是**进程级全局状态**：改一处会影响**所有线程**、所有已 park 的
 *          Fiber，而且**编译期不可见** —— 看代码不知道走哪条路，要查运行时
 *          配置；更糟的是容易**忘恢复**（`SetOpEnabled(false)` 之后忘了设回来，
 *          后续代码静默变慢，且很难定位）。
 *
 *          需要"这一次不 hook"时，**请用 `<函数名>_f`**（如 `read_f`）：
 *          它只影响你显式调用的那一处，看代码就知道，也不需要恢复。
 *
 *          这个开关保留下来**只为诊断便利**（临时全局关闭以对比行为），
 *          **不建议在任何生产路径上依赖它。**
 *
 * @details 有一组操作**不允许被关闭**（`close` / `shutdown` / `fcntl` /
 *          `dup` / `dup2`），因为关闭它们会让 fd 元数据与内核状态脱节 ——
 *          fd 号被复用时 Hook 会拿过期 descriptor 做判断（把普通文件当
 *          socket，或把新 socket 当已关闭）。对它们传 `false` 一律返回
 *          `false` 且**不改变位图**，保持永远启用。
 */
bool SetOpEnabled(HookOp op, bool enabled) noexcept;
/** @brief 一次性设置全部操作（等价于粗粒度总闸）。 */
void SetAllOpsEnabled(bool enabled) noexcept;
/** @brief 当前位图快照（诊断/测试用）。 */
std::uint32_t OpMask() noexcept;

/**
 * @brief 把 `hook.*` 配置项注册进配置中心，并双向绑定。
 * @details 每一项形如 `hook.enable_read`，类型 bool，默认 true。
 *          绑定后修改配置项会**立即**改变对应操作的开关（通过 ConfigVar 的
 *          监听器），无需重启。
 * @param config 配置中心实例。
 * @note 本函数只做注册与绑定，**不**负责加载文件或写 app.ini —— 调用方在
 *       注册完后自行 `LoadFromFile` / `SaveToFile` 即可（见示例）。
 */
void RegisterHookConfig(::go2cpp::config::Config& config);


/**
 * @brief 临时启用 Hook 的线程作用域守卫。
 * @details 构造时保存旧状态，析构时恢复；不可复制以避免恢复顺序错误。
 */
class ScopedEnable final {
public:
    /** @brief 保存旧状态并启用 Hook。 */
    ScopedEnable() noexcept;
    /** @brief 恢复构造前的 Hook 状态。 */
    ~ScopedEnable();

    ScopedEnable(const ScopedEnable&) = delete;
    ScopedEnable& operator=(const ScopedEnable&) = delete;
    ScopedEnable(ScopedEnable&& other) noexcept;
    ScopedEnable& operator=(ScopedEnable&&) = delete;

private:
    bool m_active{true};
};

/**
 * @brief 临时绑定当前线程 IOManager 的作用域守卫。
 * @param manager 作用域内使用的 IOManager。
 */
class ScopedIOManagerBinding final {
public:
    /** @brief 保存旧绑定并绑定指定 IOManager。 */
    explicit ScopedIOManagerBinding(IOManager* manager) noexcept;
    explicit ScopedIOManagerBinding(IOManager& manager) noexcept
        : ScopedIOManagerBinding(&manager) {}
    /** @brief 恢复进入守卫前的线程绑定。 */
    ~ScopedIOManagerBinding();

    ScopedIOManagerBinding(const ScopedIOManagerBinding&) = delete;
    ScopedIOManagerBinding& operator=(const ScopedIOManagerBinding&) = delete;

private:
    IOManager* m_previous{nullptr};
};

}  // namespace go2cpp::hook

// 这些控制项刻意使用 C ABI，使 preload/interposer 宿主无需依赖 C++ 名字
// 修饰即可启用该层。manager 必须指向由调用方拥有且仍存活的 IOManager。
extern "C" {
void go2cpp_hook_set_enabled(int enabled) noexcept;
int go2cpp_hook_is_enabled() noexcept;
void go2cpp_hook_bind_io_manager(void* manager) noexcept;
}

// ============================================================================
// 原始调用 API（`*_f`）—— 显式选择「不参与协程调度」
// ============================================================================
//
// ## 命名与语义
//
// 每个被 hook 的 libc 函数都有一个 `_f` 后缀的原始版本（`_f` = **raw /
// non-hooked**）。`read_f(...)` 与 `read(...)` 的区别：
//
// | 维度 | `read(...)` | `read_f(...)` |
// |---|---|---|
// | 参与协程调度 | 是（socket 上会 park Fiber）| **否** |
// | 受进程级 `hook.enable_*` 开关影响 | 是 | **否** |
// | 在普通线程可用 | 是（退化为阻塞）| **是** |
// | 行为 | 取决于 hook 启用状态与 fd 类型 | **永远是 libc 原始行为** |
//
// **`_f` 不代表更快。** 它只是"不参与协程调度"。在 Fiber 里调 `read_f`
// 会**真的阻塞整个 worker 线程** —— 该 P 上所有 Fiber 全部停摆。
//
// 只在这些场景用它：
// - 明确知道该 fd 不会阻塞（普通文件、已就绪的 socket）
// - 明确知道自己在普通线程上，不需要协程调度
// - 诊断 / 对照实验
//
// ## 为什么用显式 API 而不是只靠运行时开关
//
// 进程级 `hook.enable_*` 开关仍然保留（诊断用），但它有三个问题：
// 1. **编译期不可见**：看代码不知道走哪条路，要查运行时配置；
// 2. **影响全局**：改一处会影响所有线程与已 park 的 Fiber；
// 3. **容易忘恢复**：`SetOpEnabled(false)` 之后忘了设回来，后续代码静默变慢。
//
// `*_f` 解决了这三点：它只影响**你显式调用的那一处**，看代码就知道。
// **生产路径优先用 `*_f`，不要依赖全局开关。**
//
// ## 实现保证
//
// - 直接调 `dlsym(RTLD_NEXT)` 拿到的真实 libc 函数，**不经过** hook 的任何分支；
// - **不递归**：绝不调同名 libc 函数（那会再次进入 hook）；
// - 参数、返回值、`errno` 语义与 libc 完全一致；
// - **不依赖** Fiber / IOManager / `hooks_enabled()`，普通线程可直接调用。
//
// ## 关于 `shutdown_f`
//
// `shutdown` 在本层**从未被 hook**（`Originals` 里没有对应项；只有
// `hook.enable_shutdown` 这个配置项，是配置表的完整性遗留）。
// 所以 `shutdown_f` 与 `shutdown` 行为完全一致，提供它只是为了**命名统一**，
// 让调用方不必记"哪些函数有 `_f` 版本"。

/** @brief `read` 的原始版本：不 park Fiber、不受 hook 开关影响。 */
ssize_t read_f(int fd, void* buffer, std::size_t count);
/** @brief `readv` 的原始版本。 */
ssize_t readv_f(int fd, const struct iovec* iov, int iovcount);
/** @brief `recv` 的原始版本。 */
ssize_t recv_f(int fd, void* buffer, std::size_t length, int flags);
/** @brief `recvfrom` 的原始版本。 */
ssize_t recvfrom_f(int fd, void* buffer, std::size_t length, int flags,
                   struct sockaddr* address, socklen_t* address_length);
/** @brief `write` 的原始版本。 */
ssize_t write_f(int fd, const void* buffer, std::size_t count);
/** @brief `writev` 的原始版本。 */
ssize_t writev_f(int fd, const struct iovec* iov, int iovcount);
/** @brief `send` 的原始版本。 */
ssize_t send_f(int fd, const void* buffer, std::size_t length, int flags);
/** @brief `sendto` 的原始版本。 */
ssize_t sendto_f(int fd, const void* buffer, std::size_t length, int flags,
                 const struct sockaddr* destination, socklen_t address_length);
/** @brief `connect` 的原始版本：不受 connect 超时配置影响。 */
int connect_f(int fd, const struct sockaddr* address, socklen_t address_length);
/** @brief `accept` 的原始版本：新 fd 不会被登记到 FdManager。 */
int accept_f(int fd, struct sockaddr* address, socklen_t* address_length);
/// @brief `sleep` 的原始版本：会真的阻塞整个 worker 线程。
unsigned int sleep_f(unsigned int seconds);
/// @brief `nanosleep` 的原始版本：会真的阻塞整个 worker 线程。
int nanosleep_f(const struct timespec* request, struct timespec* remaining);
/// @brief `usleep` 的原始版本：会真的阻塞整个 worker 线程。
int usleep_f(unsigned int microseconds);
/// @brief `clock_nanosleep` 的原始版本：会真的阻塞整个 worker 线程。
int clock_nanosleep_f(int clock_id, int flags, const struct timespec* request,
                      struct timespec* remaining);
/** @brief `poll` 的原始版本。 */
int poll_f(struct pollfd* descriptors, nfds_t count, int timeout);
/** @brief `select` 的原始版本。 */
int select_f(int descriptor_count, fd_set* read_set, fd_set* write_set,
             fd_set* except_set, struct timeval* timeout);
/** @brief `epoll_wait` 的原始版本。 */
int epoll_wait_f(int epoll_fd, struct epoll_event* events, int max_events,
                 int timeout);
/** @brief `close` 的原始版本：不清理 FdManager 元数据。 */
int close_f(int fd);
/** @brief `shutdown` 的原始版本（`shutdown` 本来就没被 hook，仅为命名统一）。 */
int shutdown_f(int fd, int how);
/**
 * @brief `fcntl` 的原始版本。
 * @note 变参命令按现有 hook 的同一套契约解析（`FcntlArgument`）；
 *       未知命令返回 `ENOTSUP`，不会读取未知类型参数。
 */
int fcntl_f(int fd, int command, ...);
/** @brief `dup` 的原始版本。 */
int dup_f(int fd);
/** @brief `dup2` 的原始版本。 */
int dup2_f(int fd, int new_fd);


