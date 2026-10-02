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

// 非阻塞系统调用前捕获的代际句柄，并一直保留到等待结束。数字 fd 被复用
// 时不会重新激活旧句柄。
/**
 * fd 的代际令牌。
 *
 * 依赖：DescriptorRegistryState 的登记表；对上层/Hook 提供 fd、代际和
 * 有效性查询。令牌不拥有 fd，只防止 close/复用后旧等待错误地唤醒。
 */
/** @brief 记录文件描述符代际，防止关闭后旧等待节点误唤醒。 */
class DescriptorToken final {
public:
    ~DescriptorToken();
    /** @brief 返回关联 fd。 */
    int fd() const noexcept { return m_fd; }
    /** @brief 返回 fd 当前代际编号。 */
    std::uint64_t generation() const noexcept { return m_generation; }
    /** @brief 返回令牌是否仍有效。 */
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

// 进程级的短生命周期门。绝不能跨 park 或阻塞系统调用持有它。Hook 在
// 失效标记与 close/dup2 期间持有；IOManager 在验证 token 并发布 epoll
// interest 期间持有。
/**
 * fd 注册表的短生命周期保护。
 *
 * 依赖：DescriptorRegistryState 的递归互斥锁；对上层/Hook 提供 Capture
 * 和 Invalidate 的原子窗口。不能跨阻塞系统调用或 Fiber park 持有。
 */
/** @brief 持有描述符注册表锁并管理令牌生命周期的 RAII 守卫。 */
class DescriptorGuard final {
public:
    DescriptorGuard();
    ~DescriptorGuard();
    DescriptorGuard(const DescriptorGuard&) = delete;
    DescriptorGuard& operator=(const DescriptorGuard&) = delete;
    /** @brief 捕获 fd 当前代际令牌。 */
    static DescriptorTokenPtr Capture(int fd);
    /** @brief 使 fd 当前代际失效并唤醒相关等待。 */
    static void Invalidate(int fd) noexcept;

private:
    std::shared_ptr<DescriptorRegistryState> m_state;
    std::unique_lock<std::recursive_mutex> m_lock;
};

/** @brief IOManager 支持的可读、可写和错误事件。 */
enum class IOEvent : std::uint8_t {
    kRead = 1,
    kWrite = 2,

    Read = kRead,
    Write = kWrite,
};

/** @brief 单 fd 等待结果状态。 */
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

/** @brief 单 fd 等待的状态和系统错误码。 */
struct WaitResult {
    WaitStatus status{WaitStatus::kError};
    int system_error{0};

    bool ready() const noexcept { return status == WaitStatus::kReady; }
    explicit operator bool() const noexcept { return ready(); }
};

// 一个等待项描述一个 fd 上的读/写就绪条件。expected_descriptor 可选，
// 用于把数字 fd 与捕获时的代际绑定，避免 close+复用后误唤醒旧等待。
/** @brief wait_many 请求项。 */
struct WaitRequest {
    int fd{-1};
    IOEvent event{IOEvent::kRead};
    DescriptorTokenPtr expected_descriptor;
};

/** @brief wait_any 返回的第一个就绪 fd。 */
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
/** @brief wait_many 返回的全部就绪 fd 索引。 */
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
 * 面向调度器 Fiber 的 Linux fd 就绪管理器。
 *
 * 依赖：Scheduler 提供 G 的 park/wake 和 Timer/Context 的取消截止时间，
 * DescriptorRegistryState 为 fd 提供代际校验，Linux epoll 提供读写就绪。
 * 对上层提供单 fd、多 fd、超时、取消和关闭通知；不负责真正 read/write，
 * 也不拥有调用方的 fd。Wait 只允许在当前 IOManager 管理的 G 中调用，
 * 普通线程应使用系统 poll/select。就绪必须先注册再 park，竞态事件由
 * Scheduler 的 pending 唤醒令牌保存；同一 fd/方向的等待按 FIFO 完成。
 * Hook 层必须在真实 close 前调用 NotifyClose，避免数字 fd 复用后产生旧
 * 事件误唤醒。
 */
/**
 * @brief 基于 epoll 的 Fiber IO 等待管理器。
 * @details 将 fd 事件注册到 epoll，Fiber 阻塞时释放工作线程，事件触发后
 *          重新入队；普通线程也可通过 wait 接口安全使用。
 */
class IOManager final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    /** @brief 创建 IOManager。@param config 调度器配置。 */
    explicit IOManager(SchedulerConfig config = {});
    /** @brief 停止并释放内部资源。 */
    ~IOManager();

    IOManager(const IOManager&) = delete;
    IOManager& operator=(const IOManager&) = delete;

    // 启动内部 Scheduler 和 epoll 轮询线程；返回 true 表示本次完成启动。
    /** @brief 启动调度线程与 epoll。 */
    bool start();
    // 停止接收新任务，唤醒等待者并回收内部线程；可重复调用。
    /** @brief 停止 IOManager 并等待线程退出。 */
    void shutdown();
    /** @brief 返回 IOManager 是否运行。 */
    bool is_running() const noexcept;

    bool Start() { return start(); }
    void Shutdown() { shutdown(); }
    bool IsRunning() const noexcept { return is_running(); }

    /** @brief 提交一个由 IOManager 调度的 Fiber。 */
    std::shared_ptr<Task> go(Task::Function function);
    std::shared_ptr<Task> Go(Task::Function function) {
        return go(std::move(function));
    }

    // 等待一个 fd 的读/写就绪。参数 fd 为数字描述符，event 为读或写，
    // deadline 是可选的单调时钟截止点，context 可取消等待，token 用于
    // 校验 fd 代际。返回 WaitResult：ready/timeout/cancelled/closed/error。
    WaitResult wait(int fd, IOEvent event,
                    std::optional<TimePoint> deadline = std::nullopt,
                    ContextPtr context = {},
                    DescriptorTokenPtr expected_descriptor = {});
    // 以相对 timeout 等待 fd；timeout<=0 表示立即检查。其余参数和 wait
    // 相同，返回值携带最终状态及 errno。
    WaitResult wait_for(int fd, IOEvent event, Duration timeout,
                        ContextPtr context = {});

    // 一次等待多个 fd，返回第一个获胜项。requests 是 fd/方向请求列表，
    // deadline/context 与 wait 相同；仅允许当前 IOManager 的 managed Fiber
    // 调用，普通线程返回 kError/EPERM，应使用系统 poll/select。
    WaitAnyResult wait_any(const std::vector<WaitRequest>& requests,
                           std::optional<TimePoint> deadline = std::nullopt,
                           ContextPtr context = {});
    WaitAnyResult wait_any_for(const std::vector<WaitRequest>& requests,
                               Duration timeout, ContextPtr context = {});
    // 等待多个请求中已经就绪的所有队头项，返回 ready_indices；它不是
    // “所有请求都完成”的屏障。
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

    // 取消指定 fd/方向的等待并返回是否找到等待者；不会关闭 fd。
    /** @brief 取消指定 fd 事件并唤醒等待 Fiber。 */
    bool cancel(int fd, IOEvent event);
    /** @brief 取消 fd 的全部等待事件。 */
    bool cancel_all(int fd);
    bool Cancel(int fd, IOEvent event) { return cancel(fd, event); }
    bool CancelAll(int fd) { return cancel_all(fd); }

    // Hook 层在真实 close 前调用。当前等待者收到 kClosed/EBADF，旧 epoll
    // 载荷同时失效。
    /** @brief 通知 fd 即将关闭并清理全部等待节点。 */
    bool notify_close(int fd);
    bool NotifyClose(int fd) { return notify_close(fd); }
    // Hook 使用的跨 IOManager 安全路由；回调只保留 State 弱引用，析构中
    // 不会解引用已经销毁的 IOManager。
    /** @brief 通知全部 IOManager 某 fd 已关闭。 */
    static void NotifyCloseAll(int fd) noexcept;

    Scheduler& scheduler() noexcept { return m_scheduler; }
    const Scheduler& scheduler() const noexcept { return m_scheduler; }
    Scheduler& GetScheduler() noexcept { return m_scheduler; }
    const Scheduler& GetScheduler() const noexcept { return m_scheduler; }

    // 返回拥有 Scheduler::current_scheduler() 的 IOManager；没有时返回空。
    /** @brief 返回当前线程绑定的 IOManager。 */
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
