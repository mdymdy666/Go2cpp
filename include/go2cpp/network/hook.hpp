#pragma once

#include "go2cpp/io.hpp"
#include "go2cpp/thread_policy.hpp"

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
/** @brief 设置某个操作的开关（线程安全，无锁）。 */
void SetOpEnabled(HookOp op, bool enabled) noexcept;
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
