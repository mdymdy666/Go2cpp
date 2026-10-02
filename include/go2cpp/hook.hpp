#pragma once

#include "go2cpp/io.hpp"
#include "go2cpp/thread_policy.hpp"

#include <chrono>

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
