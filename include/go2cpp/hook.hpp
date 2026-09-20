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

// Hooking defaults to enabled process-wide. A Fiber owned by an IOManager
// uses epoll; a managed Fiber without an IOManager uses bounded native poll
// fallback after lazy socket adoption. Ordinary threads and non-socket
// blocking I/O generally fall through to libc; adopted sockets use a native
// poll fallback because their kernel flags remain nonblocking. Tracked
// descriptors and unknown variadic fcntl/ioctl commands follow the explicit
// metadata/ENOTSUP contract below rather than an unconditional libc promise.
// Disabling the hook stops new Fiber/epoll admission; metadata already
// installed on a socket may still use that native fallback until close.
// Unknown variadic fcntl/ioctl command contracts return ENOTSUP rather than
// reading an argument with an unknown type or forwarding invalid ABI state.
void set_enabled(bool enabled) noexcept;
bool enabled() noexcept;
inline void SetEnabled(bool value) noexcept { set_enabled(value); }
inline bool IsEnabled() noexcept { return enabled(); }

// A binding is an optional per-thread fallback. Normally IOManager::current()
// discovers the manager from the currently running scheduler Fiber. Do not
// keep an explicit binding across a Fiber migration to another OS thread.
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

// These controls intentionally use a C ABI so a preload/interposer host can
// enable the layer without depending on C++ name mangling. The manager value
// must point to a live go2cpp::IOManager owned by the caller.
extern "C" {
void go2cpp_hook_set_enabled(int enabled) noexcept;
int go2cpp_hook_is_enabled() noexcept;
void go2cpp_hook_bind_io_manager(void* manager) noexcept;
}
