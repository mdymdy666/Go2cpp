#pragma once

#include "go2cpp/context.hpp"
#include "go2cpp/scheduler.hpp"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace go2cpp::io {

struct DescriptorRegistryState;

// A generation handle is captured before a nonblocking syscall and retained
// until its wait completes. Numeric descriptor reuse never revives the handle.
class DescriptorToken final {
public:
    ~DescriptorToken();
    int fd() const noexcept { return m_fd; }
    std::uint64_t generation() const noexcept { return m_generation; }
    bool valid() const noexcept { return m_valid.load(std::memory_order_acquire); }

private:
    DescriptorToken(int fd, std::uint64_t generation,
                    std::weak_ptr<DescriptorRegistryState> registry);
    int m_fd;
    std::uint64_t m_generation;
    std::atomic<bool> m_valid{true};
    std::weak_ptr<DescriptorRegistryState> m_registry;
    friend class DescriptorGuard;
};

using DescriptorTokenPtr = std::shared_ptr<const DescriptorToken>;

// Short process-wide lifecycle gate. Never hold this guard across park or a
// blocking syscall. Hooks hold it across invalidation plus close/dup2, and
// IOManager holds it while validating a token and publishing epoll interest.
class DescriptorGuard final {
public:
    DescriptorGuard();
    ~DescriptorGuard();
    DescriptorGuard(const DescriptorGuard&) = delete;
    DescriptorGuard& operator=(const DescriptorGuard&) = delete;
    static DescriptorTokenPtr Capture(int fd);
    static void Invalidate(int fd) noexcept;

private:
    std::shared_ptr<DescriptorRegistryState> m_state;
    std::unique_lock<std::recursive_mutex> m_lock;
};

enum class IOEvent : std::uint8_t {
    kRead = 1,
    kWrite = 2,

    Read = kRead,
    Write = kWrite,
};

enum class WaitStatus : std::uint8_t {
    kReady = 1,
    kTimeout,
    kCancelled,
    kClosed,
    kError,

    Ready = kReady,
    Timeout = kTimeout,
    Cancelled = kCancelled,
    Closed = kClosed,
    Error = kError,
};

struct WaitResult {
    WaitStatus status{WaitStatus::kError};
    int system_error{0};

    bool ready() const noexcept { return status == WaitStatus::kReady; }
    explicit operator bool() const noexcept { return ready(); }
};

// 一个等待项描述一个 fd 上的读/写就绪条件。expected_descriptor 可选，
// 用于把数字 fd 与捕获时的代际绑定，避免 close+复用后误唤醒旧等待。
struct WaitRequest {
    int fd{-1};
    IOEvent event{IOEvent::kRead};
    DescriptorTokenPtr expected_descriptor;
};

struct WaitAnyResult {
    static constexpr std::size_t kNoIndex =
        static_cast<std::size_t>(-1);

    WaitStatus status{WaitStatus::kError};
    int system_error{0};
    std::size_t index{kNoIndex};
    int fd{-1};
    IOEvent event{IOEvent::kRead};

    bool ready() const noexcept {
        return status == WaitStatus::kReady && index != kNoIndex;
    }
    explicit operator bool() const noexcept { return ready(); }
};

// wait_many 在一次唤醒中返回已经完成的所有请求索引。它不是“等待所有
// 请求都完成”的屏障；需要屏障时应使用多个任务或 WaitGroup。ready_indices
// 按请求注册顺序排列，调用方可以据此稳定地分发后续 Fiber 工作。
// 同一集合内不得重复提交相同 fd/方向；补采样只完成该 fd/方向等待队列的
// 队头节点，以保持多个 Fiber 之间的 FIFO。
struct WaitManyResult {
    WaitStatus status{WaitStatus::kError};
    int system_error{0};
    std::vector<std::size_t> ready_indices;

    bool ready() const noexcept {
        return status == WaitStatus::kReady && !ready_indices.empty();
    }
    explicit operator bool() const noexcept { return ready(); }
};

/**
 * Linux readiness manager for scheduler fibers.
 *
 * IOManager owns its Scheduler. Wait() is valid only from a G currently
 * running on that scheduler. It registers readiness before parking the G, so
 * an event racing with park is retained by Scheduler's pending-wake token.
 *
 * Waiters for the same fd and direction are completed in FIFO order.
 * NotifyClose must be called before the actual close syscall; that is the hook
 * layer's contract and prevents a reused numeric fd from receiving a stale
 * readiness notification.
 */
class IOManager final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    explicit IOManager(SchedulerConfig config = {});
    ~IOManager();

    IOManager(const IOManager&) = delete;
    IOManager& operator=(const IOManager&) = delete;

    bool start();
    void shutdown();
    bool is_running() const noexcept;

    bool Start() { return start(); }
    void Shutdown() { shutdown(); }
    bool IsRunning() const noexcept { return is_running(); }

    std::shared_ptr<Task> go(Task::Function function);
    std::shared_ptr<Task> Go(Task::Function function) {
        return go(std::move(function));
    }

    WaitResult wait(int fd, IOEvent event,
                    std::optional<TimePoint> deadline = std::nullopt,
                    ContextPtr context = {},
                    DescriptorTokenPtr expected_descriptor = {});
    WaitResult wait_for(int fd, IOEvent event, Duration timeout,
                        ContextPtr context = {});

    // 一次等待多个 fd；仅允许在当前 IOManager 的 managed Fiber 中调用。
    // 普通线程调用返回 kError/EPERM，应直接使用系统 poll/select。
    WaitAnyResult wait_any(const std::vector<WaitRequest>& requests,
                           std::optional<TimePoint> deadline = std::nullopt,
                           ContextPtr context = {});
    WaitAnyResult wait_any_for(const std::vector<WaitRequest>& requests,
                               Duration timeout, ContextPtr context = {});
    WaitManyResult wait_many(
        const std::vector<WaitRequest>& requests,
        std::optional<TimePoint> deadline = std::nullopt,
        ContextPtr context = {});
    WaitManyResult wait_many_for(const std::vector<WaitRequest>& requests,
                                 Duration timeout, ContextPtr context = {});

    WaitResult Wait(int fd, IOEvent event,
                    std::optional<TimePoint> deadline = std::nullopt,
                    ContextPtr context = {}) {
        return wait(fd, event, deadline, std::move(context));
    }
    WaitResult WaitFor(int fd, IOEvent event, Duration timeout,
                       ContextPtr context = {}) {
        return wait_for(fd, event, timeout, std::move(context));
    }
    WaitAnyResult WaitAny(const std::vector<WaitRequest>& requests,
                          std::optional<TimePoint> deadline = std::nullopt,
                          ContextPtr context = {}) {
        return wait_any(requests, deadline, std::move(context));
    }
    WaitAnyResult WaitAnyFor(const std::vector<WaitRequest>& requests,
                             Duration timeout, ContextPtr context = {}) {
        return wait_any_for(requests, timeout, std::move(context));
    }
    WaitManyResult WaitMany(
        const std::vector<WaitRequest>& requests,
        std::optional<TimePoint> deadline = std::nullopt,
        ContextPtr context = {}) {
        return wait_many(requests, deadline, std::move(context));
    }
    WaitManyResult WaitManyFor(const std::vector<WaitRequest>& requests,
                               Duration timeout, ContextPtr context = {}) {
        return wait_many_for(requests, timeout, std::move(context));
    }

    // Explicit cancellation reports kCancelled. It does not close fd.
    bool cancel(int fd, IOEvent event);
    bool cancel_all(int fd);
    bool Cancel(int fd, IOEvent event) { return cancel(fd, event); }
    bool CancelAll(int fd) { return cancel_all(fd); }

    // The hook layer calls this before the real close syscall. All current
    // waiters receive kClosed/EBADF and stale epoll payloads are invalidated.
    bool notify_close(int fd);
    bool NotifyClose(int fd) { return notify_close(fd); }
    // Safe cross-manager routing used by the hook; callbacks retain only weak
    // State handles and do not dereference an IOManager during destruction.
    static void NotifyCloseAll(int fd) noexcept;

    Scheduler& scheduler() noexcept { return m_scheduler; }
    const Scheduler& scheduler() const noexcept { return m_scheduler; }
    Scheduler& GetScheduler() noexcept { return m_scheduler; }
    const Scheduler& GetScheduler() const noexcept { return m_scheduler; }

    // Returns the manager which owns Scheduler::current_scheduler(), if any.
    static IOManager* current() noexcept;
    static IOManager* Current() noexcept { return current(); }

private:
    struct State;

    Scheduler m_scheduler;
    std::shared_ptr<State> m_state;
};

}  // namespace go2cpp::io

namespace go2cpp {
using IOManager = io::IOManager;
using IOEvent = io::IOEvent;
using IOWaitResult = io::WaitResult;
using IOWaitStatus = io::WaitStatus;
using IOWaitRequest = io::WaitRequest;
using IOWaitAnyResult = io::WaitAnyResult;
using IOWaitManyResult = io::WaitManyResult;
}  // namespace go2cpp
