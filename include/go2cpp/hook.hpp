#pragma once

#include "go2cpp/io.hpp"
#include "go2cpp/thread_policy.hpp"

#include <chrono>

namespace go2cpp::hook {

using ThreadHookMode = ::go2cpp::ThreadHookMode;
using ScopedThreadHookMode = ::go2cpp::ScopedThreadHookMode;
inline ThreadHookMode CurrentThreadMode() noexcept {
    return ::go2cpp::CurrentThreadHookMode();
}
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
void set_enabled(bool enabled) noexcept;
bool enabled() noexcept;
inline void SetEnabled(bool value) noexcept { set_enabled(value); }
inline bool IsEnabled() noexcept { return enabled(); }

// 绑定是可选的线程级后备。通常 IOManager::current() 会从当前运行的调度器
// Fiber 发现管理器。Fiber 迁移到其他 OS 线程时，不要继续保留显式绑定。
void bind_io_manager(IOManager* manager) noexcept;
IOManager* bound_io_manager() noexcept;
inline void BindIOManager(IOManager* manager) noexcept {
    bind_io_manager(manager);
}
inline IOManager* BoundIOManager() noexcept { return bound_io_manager(); }

void set_connect_timeout(std::chrono::milliseconds timeout) noexcept;
std::chrono::milliseconds connect_timeout() noexcept;

class ScopedEnable final {
public:
    ScopedEnable() noexcept;
    ~ScopedEnable();

    ScopedEnable(const ScopedEnable&) = delete;
    ScopedEnable& operator=(const ScopedEnable&) = delete;
    ScopedEnable(ScopedEnable&& other) noexcept;
    ScopedEnable& operator=(ScopedEnable&&) = delete;

private:
    bool m_active{true};
};

class ScopedIOManagerBinding final {
public:
    explicit ScopedIOManagerBinding(IOManager* manager) noexcept;
    explicit ScopedIOManagerBinding(IOManager& manager) noexcept
        : ScopedIOManagerBinding(&manager) {}
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
