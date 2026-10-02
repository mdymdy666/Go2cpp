#include "go2cpp/sync.hpp"

#include "go2cpp/core/hybrid_mutex.hpp"

#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace go2cpp::sync {
namespace {

/**
 * @brief Go2Cpp Scheduler 的默认同步等待适配器。
 * @details Scheduler 类型只在 .cpp 内出现，公共 sync 头因此保持与调度器
 *          解耦；其他 Fiber 后端只需实现 WaitBackend 协议即可接入。
 */
class SchedulerWaitBackend final : public WaitBackend {
public:
    bool Park(void* scheduler_ptr, const std::shared_ptr<void>& task_value,
              std::optional<ContextTimePoint>,
              const std::function<bool()>& predicate) override {
        auto* scheduler = static_cast<Scheduler*>(scheduler_ptr);
        auto task = std::static_pointer_cast<Task>(task_value);
        if (scheduler == nullptr || !task) {
            return false;
        }
        for (;;) {
            if (predicate()) {
                return true;
            }
            const bool suspended = scheduler->park_wait(task);
            if (predicate()) {
                return true;
            }
            if (task->cancellation_requested() ||
                Scheduler::current_scheduler() != scheduler ||
                Scheduler::current_task().get() != task.get() ||
                (!suspended && !scheduler->is_running())) {
                return false;
            }
        }
    }

    void Wake(void* scheduler_ptr,
              const std::shared_ptr<void>& task_value) noexcept override {
        try {
            auto* scheduler = static_cast<Scheduler*>(scheduler_ptr);
            auto task = std::static_pointer_cast<Task>(task_value);
            if (scheduler != nullptr && task) {
                (void)scheduler->wake_registered(task);
            }
        } catch (...) {
            // 唤醒回调位于取消/通知路径，异常必须被隔离。
        }
    }
};

std::mutex s_wait_backend_mutex;
WaitBackendFactory s_wait_backend_factory;

enum class WaitResult : std::uint8_t {
    kWaiting,
    kNotified,
    kCancelled,
};

class WaitNode final {
public:
    /// 函数功能：执行 WaitNode，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] scheduler 调用方传入的参数，具体约束以头文件声明为准。
    /// @param[in] task 调用方传入的参数，具体约束以头文件声明为准。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    WaitNode(Scheduler* scheduler, std::shared_ptr<Task> task)
        : m_scheduler(scheduler), m_task(std::move(task)),
          m_backend(CreateWaitBackend()) {}

    ~WaitNode() { Disarm(); }

    /// 函数功能：执行 Arm，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    bool Arm() noexcept {
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
        if (result() != WaitResult::kWaiting) {
            return false;
        }
        m_active = true;
        return true;
    }

    /// 函数功能：执行 Disarm，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    void Disarm() noexcept {
        // 回调可能已经离开 DoneSignal 的注册表。这里等待其 Wake 调用完成，
        // 防止回调生命周期超过 Scheduler 的关闭过程。
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
        m_active = false;
        m_scheduler = nullptr;
    }

    /// 函数功能：执行 result，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    WaitResult result() const noexcept {
        return m_result.load(std::memory_order_acquire);
    }

    /// 函数功能：执行 TryFinish，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] result 调用方传入的参数，具体约束以头文件声明为准。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    bool TryFinish(WaitResult result) noexcept {
        WaitResult expected = WaitResult::kWaiting;
        return m_result.compare_exchange_strong(
            expected, result, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    /// 函数功能：执行 Wake，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    void Wake() noexcept {
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
        WakeLocked();
    }

    /// 函数功能：执行 TryCancelAndWake，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] generation 调用方传入的参数，具体约束以头文件声明为准。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    // Context 的取消回调可能已经通过 weak_ptr 取得了本节点，随后
    // RemoveCallback 又与节点回收并发。取消、active 检查和唤醒必须在
    // 同一把门锁内完成；否则旧回调可能在节点放回 thread-local cache
    // 后，把下一次等待的 m_result 错误地改成 Cancelled。generation 用来
    // 区分同一个缓存节点的不同等待代次。
    /// 函数功能：完成 TryCancelAndWake 调用，读取或更新相关运行时状态。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
    /// 2. 按状态机规则获取必要的同步保护并执行核心操作；
    /// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
    /// @param[in] generation 调用方传入的参数，具体约束以头文件声明为准。
    /// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
    /// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
    bool TryCancelAndWake(std::uint64_t generation) noexcept {
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
        if (generation != m_generation ||
            !TryFinish(WaitResult::kCancelled)) {
            return false;
        }
        // 取消可能与 Arm() 交错。未 Arm 时只发布终态，发布方会在
        // 入队锁内观察到 Cancelled 并放弃挂起；已 Arm 时才需要实际唤醒。
        if (m_active) {
            WakeLocked();
        }
        return true;
    }

    /// 函数功能：执行 generation，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    std::uint64_t generation() noexcept {
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
        return m_generation;
    }

private:
    /// 函数功能：执行 WakeLocked，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    void WakeLocked() noexcept {
        if (!m_active) {
            return;
        }
        try {
            if (m_scheduler != nullptr && m_task) {
                // managed waiter 通过所属 Scheduler 恢复。
                // 已注册 G 走 wake_registered()；该入口仍保留 started G，
                // 即使 shutdown 或队列分配与这个回调并发。
                if (m_backend) {
                    m_backend->Wake(m_scheduler,
                                    std::static_pointer_cast<void>(m_task));
                }
            } else {
                // 普通线程调用者在节点私有条件变量上等待。结果字段就是谓词，
                // 因此 wait() 之前到达的通知不会丢失。
                m_native_condition.notify_one();
            }
        } catch (...) {
            // Scheduler 的可靠唤醒交接会保留所有已经启动的 G。
        }
    }

public:

    /// 函数功能：执行 managed，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    bool managed() const noexcept {
        return m_scheduler != nullptr && static_cast<bool>(m_task);
    }

    /// 函数功能：执行 WaitNative，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    WaitResult WaitNative() noexcept {
        try {
            std::unique_lock<std::mutex> lock(m_native_mutex);
            m_native_condition.wait(lock, [this] {
                return result() != WaitResult::kWaiting;
            });
        } catch (...) {
            // condition_variable 可能报告实现或系统错误。不要让错误越过运行时
            // 等待边界（该边界因调度器和取消回调只是通知而声明为 noexcept），
            // 而应将其转换为与取消相同的终态路径。
            (void)TryFinish(WaitResult::kCancelled);
        }
        return result();
    }

    const std::shared_ptr<Task>& task() const noexcept { return m_task; }
    Scheduler* scheduler() const noexcept { return m_scheduler; }
    const std::shared_ptr<WaitBackend>& backend() const noexcept {
        return m_backend;
    }

    /// 函数功能：执行 Reset，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] scheduler 调用方传入的参数，具体约束以头文件声明为准。
    /// @param[in] task 调用方传入的参数，具体约束以头文件声明为准。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    void Reset(Scheduler* scheduler, std::shared_ptr<Task> task) noexcept {
        std::lock_guard<core::HybridMutex> lock(m_wake_mutex);
        m_active = false;
        m_scheduler = scheduler;
        m_task = std::move(task);
        m_backend = CreateWaitBackend();
        m_result.store(WaitResult::kWaiting, std::memory_order_release);
        ++m_generation;
        if (m_generation == 0U) {
            m_generation = 1U;
        }
    }

private:
    Scheduler* m_scheduler{nullptr};
    std::shared_ptr<Task> m_task;
    std::atomic<WaitResult> m_result{WaitResult::kWaiting};
    core::HybridMutex m_wake_mutex;
    std::mutex m_native_mutex;
    std::condition_variable m_native_condition;
    std::shared_ptr<WaitBackend> m_backend;
    bool m_active{false};
    std::uint64_t m_generation{1U};
};

// WaitNode 只在等待期间被队列或取消回调引用。完成一次等待后将对象放回
// 当前 M 的小缓存，避免混合 Mutex 在短临界区竞争时反复分配控制块和
// condition_variable。缓存节点已清空 scheduler/task，不延长用户对象生命期。
thread_local std::vector<std::shared_ptr<WaitNode>> t_wait_node_cache;

std::shared_ptr<WaitNode> AcquireWaitNode(Scheduler* scheduler,
                                          std::shared_ptr<Task> task) {
    if (!t_wait_node_cache.empty()) {
        auto waiter = std::move(t_wait_node_cache.back());
        t_wait_node_cache.pop_back();
        waiter->Reset(scheduler, std::move(task));
        return waiter;
    }
    return std::make_shared<WaitNode>(scheduler, std::move(task));
}

/// 函数功能：完成 RelaxCpu 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
inline void RelaxCpu() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

/// 函数功能：完成 ReleaseWaitNode 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] waiter 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void ReleaseWaitNode(std::shared_ptr<WaitNode> waiter) noexcept {
    if (!waiter) {
        return;
    }
    waiter->Reset(nullptr, {});
    if (t_wait_node_cache.size() < 32U) {
        t_wait_node_cache.push_back(std::move(waiter));
    }
}

struct WaitTarget {
    Scheduler* scheduler{nullptr};
    std::shared_ptr<Task> task;
    bool unsupported_manual_fiber{false};

    /// 函数功能：执行 bool，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    explicit operator bool() const noexcept {
        return scheduler != nullptr && static_cast<bool>(task);
    }
};

/// 函数功能：完成 CurrentTarget 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
WaitTarget CurrentTarget() noexcept {
    Scheduler* const scheduler = Scheduler::current_scheduler();
    auto task = Scheduler::current_task();
    const bool manual_fiber = Fiber::Current() != nullptr &&
                              (scheduler == nullptr || !task);
    return {scheduler, std::move(task), manual_fiber};
}

class ContextSubscription final {
public:
    /// 函数功能：执行 ContextSubscription，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] context 调用方传入的参数，具体约束以头文件声明为准。
    /// @param[in] waiter 调用方传入的参数，具体约束以头文件声明为准。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    ContextSubscription(const ContextPtr& context,
                        const std::shared_ptr<WaitNode>& waiter)
        : m_context(context) {
        if (!m_context || !waiter) {
            return;
        }
        const std::weak_ptr<WaitNode> weak_waiter(waiter);
        const std::uint64_t generation = waiter->generation();
        m_id = m_context->Done().AddCallback([weak_waiter, generation] {
            const auto current = weak_waiter.lock();
            if (current) {
                (void)current->TryCancelAndWake(generation);
            }
        });
    }

    ContextSubscription(const ContextSubscription&) = delete;
    ContextSubscription& operator=(const ContextSubscription&) = delete;

    ~ContextSubscription() { Reset(); }

    /// 函数功能：执行 Reset，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    void Reset() noexcept {
        if (m_context && m_id != 0) {
            m_context->Done().RemoveCallback(m_id);
        }
        m_id = 0;
        m_context.reset();
    }

private:
    ContextPtr m_context;
    DoneSignal::CallbackId m_id{0};
};

class CancelGuard final {
public:
    /// 函数功能：执行 CancelGuard，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] cancel 调用方传入的参数，具体约束以头文件声明为准。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    explicit CancelGuard(CancelFunc cancel) : m_cancel(std::move(cancel)) {}
    ~CancelGuard() {
        if (m_cancel) {
            m_cancel();
        }
    }

    CancelGuard(const CancelGuard&) = delete;
    CancelGuard& operator=(const CancelGuard&) = delete;

private:
    CancelFunc m_cancel;
};

/// 函数功能：完成 Await 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] waiter 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
WaitResult Await(WaitNode* waiter) noexcept {
    if (!waiter) {
        return WaitResult::kCancelled;
    }

    const auto finish = [waiter](WaitResult result) {
        waiter->Disarm();
        return result;
    };
    if (!waiter->managed()) {
        // 普通线程调用者读取相同的结果状态，但在 condition_variable 上休眠；
        // 这样阻塞的 OS 线程不会占用 GMP worker，同时保持先通知后等待的语义。
        return finish(waiter->WaitNative());
    }

    // managed G 从不等待原生 condition_variable：park 会释放其 M，Scheduler
    // 的唤醒路径会重新排入同一个逻辑 G。
    Scheduler* const scheduler = waiter->scheduler();
    const auto& task = waiter->task();
    for (;;) {
        // Unlock() 可以在 waiter 刚发布、但尚未真正调用 park() 的窗口内
        // 完成交接。此时 Wake 已把结果设为 Notified；若仍然进入
        // park_with_reason()，会无谓获取 Scheduler admission 锁，短临界区
        // 的每次交接都会被放大成一次调度慢路径。先读取终态还能保留
        // notify-before-park 语义：结果已经线性化时无需再次挂起。
        const WaitResult published = waiter->result();
        if (published != WaitResult::kWaiting) {
            return finish(published);
        }
        if (task->cancellation_requested()) {
            if (waiter->result() == WaitResult::kWaiting) {
                (void)waiter->TryFinish(WaitResult::kCancelled);
            }
            return finish(waiter->result());
        }

        // 发布后始终尝试 park，即使通知者可能已经先一步完成。先通知后 park
        // 的唤醒会保存为 pending token；park 消耗该 token 后直接返回，不会挂起。
        const bool suspended = waiter->backend()->Park(
            scheduler, std::static_pointer_cast<void>(task), std::nullopt,
            [waiter, task] {
                return waiter->result() != WaitResult::kWaiting ||
                       task->cancellation_requested();
            });
        const WaitResult result = waiter->result();
        if (result != WaitResult::kWaiting) {
            return finish(result);
        }
        if (task->cancellation_requested() ||
            Scheduler::current_scheduler() != scheduler ||
            Scheduler::current_task().get() != task.get() ||
            (!suspended && !scheduler->is_running())) {
            (void)waiter->TryFinish(WaitResult::kCancelled);
            return finish(waiter->result());
        }
        // false park 可能消耗无关的 pending permit。该 permit 和无关的 unpark
        // 都属于伪唤醒，不代表取消。
    }
}

template <typename Queue, typename Waiter>
void RemoveWaiter(Queue& waiters, const Waiter& waiter) noexcept {
    const auto position = std::find(waiters.begin(), waiters.end(), waiter);
    if (position != waiters.end()) {
        waiters.erase(position);
    }
}

ContextPtr TimeoutContext(ContextDuration timeout, const ContextPtr& parent,
                          CancelFunc* cancel) {
    auto pair = WithTimeout(parent ? parent : Background(), timeout);
    *cancel = std::move(pair.second);
    return std::move(pair.first);
}

// ConditionVariable::Wait 的前置条件与 std::condition_variable::wait 相同：
// 调用者必须持有 mutex。即使 Context 已取消或超时为零，也要完成看似原子的
// 解锁/重锁边界，让其他 waiter 有机会运行，并在看到 false 前重新获得锁。
// Scheduler 关闭期间 Mutex::Lock 可能拒绝再次 park G；此时按文档返回 false，
// mutex 保持未锁定。
/// 函数功能：完成 AbortConditionWait 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] mutex 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] preserve_lock 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool AbortConditionWait(Mutex& mutex, bool preserve_lock = false) {
    // 手动恢复的 Fiber 没有 Scheduler continuation。解锁后再阻塞重锁可能在
    // 其他 owner 获胜时挂起 carrier 线程，因此这个受限路径保留调用者持有的
    // 锁并返回 false。
    if (preserve_lock) {
        return false;
    }
    mutex.Unlock();
    (void)mutex.Lock();
    return false;
}

}  // namespace

/// 函数功能：注册同步等待后端工厂，隔离具体 Scheduler/Fiber 实现。
/// 执行流程：在短临界区内替换工厂快照；实际工厂调用始终发生在锁外。
/// @param[in] factory 新建等待节点使用的后端工厂。
/// @return 成功发布返回 true；复制 std::function 失败返回 false。
/// @note 已存在等待节点继续使用原后端，不会被强制迁移。
bool SetWaitBackendFactory(WaitBackendFactory factory) {
    try {
        std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
        s_wait_backend_factory = std::move(factory);
        return true;
    } catch (...) {
        return false;
    }
}

/// 函数功能：恢复默认 Scheduler 同步等待适配器。
/// @return 无返回值。
/// @note 只影响后续创建的等待节点。
void ResetWaitBackendFactory() noexcept {
    try {
        std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
        s_wait_backend_factory = {};
    } catch (...) {
    }
}

/// 函数功能：创建同步等待节点的后端实例。
/// @return 非空后端；用户工厂异常或返回空指针时回退 SchedulerWaitBackend。
/// @note 用户工厂在锁外执行，避免注册函数重入造成锁反转。
std::shared_ptr<WaitBackend> CreateWaitBackend() noexcept {
    WaitBackendFactory factory;
    try {
        {
            std::lock_guard<std::mutex> lock(s_wait_backend_mutex);
            factory = s_wait_backend_factory;
        }
        if (factory) {
            if (auto backend = factory()) {
                return backend;
            }
        }
    } catch (...) {
    }
    return std::make_shared<SchedulerWaitBackend>();
}

struct Mutex::Impl {
    std::mutex m_mutex;
    // 同一个原子字节同时发布“已持有”和“已有等待者/等待者正在入队”
    // 两个状态。Unlock 的 compare_exchange 因此不会读取到旧的等待者
    // 标记后错误地释放锁，避免丢失唤醒。
    static constexpr std::uint8_t kLocked = 1U;
    static constexpr std::uint8_t kWaiter = 2U;
    std::atomic<std::uint8_t> m_state{0U};
    // 只统计已经进入等待队列的普通线程。managed Fiber 在没有 native
    // waiter 时可以采用有界协作重试，避免每次短临界区都 park；一旦有
    // native waiter，所有后续 G 回到 FIFO 队列，保证线程不会饥饿。
    std::atomic<std::size_t> m_native_waiters{0U};
    // 队列必须持有 shared_ptr。Unlock 会先从队列取出等待者，再在释放
    // 队列锁后调用 Wake；如果队列只保存裸指针，等待方可能在这段窗口
    // 内返回并销毁栈上的 WaitNode，或把缓存节点重置给下一次等待，
    // 从而形成悬空访问。队列持有的引用把节点生命周期延长到 Wake
    // 完成，同时不改变锁外唤醒以避免锁反转的约束。
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

/// 函数功能：完成 Mutex 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
Mutex::Mutex() : m_impl(std::make_unique<Impl>()) {}
Mutex::~Mutex() = default;

/// 函数功能：完成 Lock 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] context 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool Mutex::Lock(const ContextPtr& context) {
    if (context && context->IsDone()) {
        return false;
    }

    std::uint8_t expected_state = 0U;
    if (m_impl->m_state.compare_exchange_strong(
            expected_state, Impl::kLocked, std::memory_order_acquire,
            std::memory_order_relaxed)) {
        return true;
    }

    const WaitTarget target = CurrentTarget();
    if (target.unsupported_manual_fiber) {
        // 手动恢复的 Fiber 没有可用于 park 的 Scheduler continuation。返回 false
        // 比阻塞 carrier 线程更安全。
        return false;
    }
    if (target && target.task->cancellation_requested()) {
        return false;
    }

    // 混合并发下，短临界区通常只需要等待当前 Fiber 完成一小段工作。
    // 没有 native waiter 时，managed G 走有界协作重试，避免每次短竞争
    // 都进入 WaitNode + park。managed waiter 之间允许有限度地越过队列，
    // 以换取吞吐；一旦有普通线程 waiter，最多再重试 4 次后回到 FIFO
    // handoff，保证线程不会被 Fiber 长时间压住。预算耗尽后仍会进入
    // WaitNode + park，不会无界占用 M。
    if (target && target.scheduler->processor_count() > 1 &&
        m_impl->m_native_waiters.load(std::memory_order_acquire) ==
            0U) {
        // 竞争刚出现时，临界区通常仍在另一个 P 上运行。协作让出可以
        // 避免把短临界区升级为 Fiber 挂起/恢复和跨 M 唤醒；单 P 不走
        // 这条路径，因此不会把唯一 M 忙等住。预算耗尽后再进入 waiter。
        constexpr int kCooperativeAttempts = 64;
        for (int attempt = 0; attempt < kCooperativeAttempts; ++attempt) {
            // native waiter 出现后，重试预算被限制为 4 次，避免 Fiber
            // 无限越过普通线程；这次检查放在每轮 CAS 前。
            if (m_impl->m_native_waiters.load(std::memory_order_acquire) !=
                    0U &&
                attempt >= 4) {
                break;
            }
            expected_state = 0U;
            if (m_impl->m_state.compare_exchange_weak(
                    expected_state, Impl::kLocked,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return true;
            }
            if (target.task->cancellation_requested()) {
                break;
            }
            // 让出当前 G，使持锁者有机会在另一个 M 上完成短临界区。
            // 次数有界，耗尽后进入 FIFO 等待队列，确保 native waiter
            // 不会被无限期阻塞。
            if (!target.scheduler->yield_current()) {
                break;
            }
        }
    }
    // std::mutex 在 Linux 上也会先进行短暂自旋；混合锁的 native
    // 调用者如果立即进入条件变量慢路径，会把很短的临界区放大成一次
    // futex 唤醒。这里保留较小的有界自旋，并且一旦队列发布立即退出，
    // 因而不会越过 FIFO waiter。
    if (!target &&
        (m_impl->m_state.load(std::memory_order_acquire) & Impl::kWaiter) ==
            0U) {
        constexpr int kNativeSpinAttempts = 256;
        for (int attempt = 0; attempt < kNativeSpinAttempts; ++attempt) {
            if ((m_impl->m_state.load(std::memory_order_acquire) &
                 Impl::kWaiter) != 0U) {
                break;
            }
            expected_state = 0U;
            if (m_impl->m_state.compare_exchange_weak(
                    expected_state, Impl::kLocked,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return true;
            }
            RelaxCpu();
        }
    }
    // Mutex::Unlock 会在释放 m_mutex 后异步执行 Wake，因此即使没有
    // Context，也必须让队列持有一个 shared_ptr，不能使用栈上节点。
    std::shared_ptr<WaitNode> owned_waiter =
        AcquireWaitNode(target.scheduler, target.task);
    WaitNode* waiter = owned_waiter.get();
    ContextSubscription subscription(context, owned_waiter);
    const auto release_waiter = [&] {
        subscription.Reset();
        if (owned_waiter) {
            ReleaseWaitNode(std::move(owned_waiter));
        }
    };
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if ((context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting) {
            release_waiter();
            return false;
        }

        // 等待者位只在持有队列锁时发布。这样不会出现“等待者已经设置
        // 标志但尚未入队，Unlock 却覆盖标志”的窗口：Unlock 的无竞争
        // CAS 可以在本段之前把锁释放掉，当前调用随后重新观察到空闲锁
        // 并直接取得；如果 CAS 发生在发布之后，则必然看到 kWaiter 并
        // 进入同一把队列锁完成 FIFO 交接。
        for (;;) {
            expected_state = m_impl->m_state.load(std::memory_order_acquire);
            if ((expected_state & Impl::kLocked) == 0U &&
                m_impl->m_waiters.empty()) {
                if (m_impl->m_state.compare_exchange_weak(
                        expected_state, Impl::kLocked,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    release_waiter();
                    return true;
                }
                continue;
            }

            if ((expected_state & Impl::kWaiter) == 0U) {
                const std::uint8_t published = static_cast<std::uint8_t>(
                    expected_state | Impl::kWaiter);
                if (!m_impl->m_state.compare_exchange_weak(
                        expected_state, published,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    continue;
                }
            }
            break;
        }

        m_impl->m_waiters.push_back(owned_waiter);
        if (!target) {
            m_impl->m_native_waiters.fetch_add(1U, std::memory_order_release);
        }
        if (!waiter->Arm()) {
            m_impl->m_waiters.pop_back();
            if (!target) {
                m_impl->m_native_waiters.fetch_sub(1U,
                                                    std::memory_order_release);
            }
            if (m_impl->m_waiters.empty()) {
                m_impl->m_state.fetch_and(
                    static_cast<std::uint8_t>(~Impl::kWaiter),
                    std::memory_order_release);
            }
            release_waiter();
            return false;
        }
    }

    const WaitResult result = Await(waiter);
    subscription.Reset();
    if (result != WaitResult::kNotified) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        const auto position = std::find_if(
            m_impl->m_waiters.begin(), m_impl->m_waiters.end(),
            [waiter](const std::shared_ptr<WaitNode>& candidate) {
                return candidate.get() == waiter;
            });
        if (position != m_impl->m_waiters.end()) {
            m_impl->m_waiters.erase(position);
            if (!waiter->managed()) {
                m_impl->m_native_waiters.fetch_sub(
                    1U, std::memory_order_release);
            }
        }
        if (m_impl->m_waiters.empty()) {
            m_impl->m_state.fetch_and(
                static_cast<std::uint8_t>(~Impl::kWaiter),
                std::memory_order_release);
        }
        if (owned_waiter) {
            ReleaseWaitNode(std::move(owned_waiter));
        }
        return false;
    }
    if (owned_waiter) {
        ReleaseWaitNode(std::move(owned_waiter));
    }
    return true;
}

/// 函数功能：完成 LockFor 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] timeout 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] parent 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool Mutex::LockFor(ContextDuration timeout, const ContextPtr& parent) {
    if (parent && parent->IsDone()) {
        return false;
    }
    // 零或负超时的 timed lock 仍应先进行标准的立即 try-lock。如果把它转给
    // 已取消的 Context，即使 mutex 空闲也会错误失败。
    if (timeout <= ContextDuration::zero()) {
        return TryLock();
    }
    CancelFunc cancel;
    const ContextPtr context = TimeoutContext(timeout, parent, &cancel);
    CancelGuard cancel_guard(std::move(cancel));
    return Lock(context);
}

/// 函数功能：完成 TryLock 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool Mutex::TryLock() noexcept {
    std::uint8_t expected = 0U;
    const bool acquired = m_impl->m_state.compare_exchange_strong(
        expected, Impl::kLocked, std::memory_order_acquire,
        std::memory_order_relaxed);
    return acquired;
}

/// 函数功能：完成 Unlock 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void Mutex::Unlock() {
    // 无等待者时只需一次 CAS，不进入队列互斥量。若等待者位同时存在，
    // CAS 必定失败并转入下面的 FIFO 交接路径。
    std::uint8_t expected_state = Impl::kLocked;
    if (m_impl->m_state.compare_exchange_strong(
            expected_state, 0U, std::memory_order_release,
            std::memory_order_acquire)) {
        return;
    }

    std::shared_ptr<WaitNode> selected;
    {
        // Unlock 与 waiter 发布必须共享同一把队列锁。原子快路径无法
        // 同时覆盖 waiter 入队和另一个线程重新取得锁的交错，统一在此
        // 线性化可以避免丢唤醒和“误解锁后来 owner”。
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if ((m_impl->m_state.load(std::memory_order_acquire) &
             Impl::kLocked) == 0U) {
            throw std::logic_error("go2cpp::sync::Mutex unlock of unlocked mutex");
        }
        while (!m_impl->m_waiters.empty()) {
            std::shared_ptr<WaitNode> candidate =
                std::move(m_impl->m_waiters.front());
            m_impl->m_waiters.pop_front();
            if (candidate && !candidate->managed()) {
                m_impl->m_native_waiters.fetch_sub(
                    1U, std::memory_order_release);
            }
            if (candidate && candidate->TryFinish(WaitResult::kNotified)) {
                selected = std::move(candidate);
                break;
            }
        }
        if (!selected) {
            m_impl->m_state.store(0U, std::memory_order_release);
        } else {
            m_impl->m_state.store(
                static_cast<std::uint8_t>(Impl::kLocked |
                                           (m_impl->m_waiters.empty()
                                                ? 0U
                                                : Impl::kWaiter)),
                std::memory_order_release);
        }
        // 选定 waiter 后锁在逻辑上仍保持持有：所有权直接交给 FIFO 队首，
        // 因而 TryLock 调用者不能插队。
    }
    if (selected) {
        selected->Wake();
    }
}

/// 函数功能：完成 lock 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void Mutex::lock() {
    if (!Lock()) {
        throw std::logic_error(
            "go2cpp::sync::Mutex lock was cancelled or scheduler unavailable");
    }
}

struct ConditionVariable::Impl {
    std::mutex m_mutex;
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

/// 函数功能：完成 ConditionVariable 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ConditionVariable::ConditionVariable() : m_impl(std::make_unique<Impl>()) {}
ConditionVariable::~ConditionVariable() = default;

/// 函数功能：完成 Wait 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] mutex 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] context 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool ConditionVariable::Wait(Mutex& mutex, const ContextPtr& context) {
    // 必须先识别手动 Fiber。即使 Context 已取消，也不能走普通线程的
    // unlock/relock 路径，否则竞争中的重锁会阻塞 carrier 线程。
    const WaitTarget target = CurrentTarget();
    if (target.unsupported_manual_fiber) {
        return AbortConditionWait(mutex, true);
    }
    if (context && context->IsDone()) {
        return AbortConditionWait(mutex);
    }
    if (target && target.task->cancellation_requested()) {
        return AbortConditionWait(mutex);
    }

    const auto waiter =
        std::make_shared<WaitNode>(target.scheduler, target.task);
    ContextSubscription subscription(context, waiter);
    bool abort_before_publish = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        abort_before_publish =
            (context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting;
        if (!abort_before_publish) {
            m_impl->m_waiters.push_back(waiter);
            if (!waiter->Arm()) {
                m_impl->m_waiters.pop_back();
                abort_before_publish = true;
            }
        }
    }
    if (abort_before_publish) {
        // 释放 CV 注册表锁后再执行。Unlock 可能唤醒调用 NotifyOne 的 waiter，
        // 它必须能够检查注册表，避免锁顺序反转。
        return AbortConditionWait(mutex);
    }

    try {
        // 先发布再解锁。并发 Notify 可能在此时运行，但 Scheduler 的 pending-wake
        // 语义保证后续 park 不会丢失唤醒。
        mutex.Unlock();
    } catch (...) {
        (void)waiter->TryFinish(WaitResult::kCancelled);
        waiter->Disarm();
        subscription.Reset();
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
        throw;
    }

    const WaitResult result = Await(waiter.get());
    subscription.Reset();
    {
        // 已取消 Context 的节点由其 owner 移除。Notify 路径会先摘除获胜节点，
        // 因而通知后执行这里同样安全。
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
    }

    // Context 取消不会豁免条件变量的重锁规则。Scheduler 关闭不同：Mutex::Lock
    // 拒绝再次 park 已请求取消的 G，但仍可能立即获取锁。
    const bool relocked = mutex.Lock();
    return relocked && result == WaitResult::kNotified;
}

bool ConditionVariable::WaitFor(Mutex& mutex, ContextDuration timeout,
                                const ContextPtr& parent) {
    CancelFunc cancel;
    const ContextPtr context = TimeoutContext(timeout, parent, &cancel);
    CancelGuard cancel_guard(std::move(cancel));
    return Wait(mutex, context);
}

/// 函数功能：完成 NotifyOne 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void ConditionVariable::NotifyOne() noexcept {
    std::shared_ptr<WaitNode> selected;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        while (!m_impl->m_waiters.empty()) {
            auto candidate = std::move(m_impl->m_waiters.front());
            m_impl->m_waiters.pop_front();
            if (candidate &&
                candidate->TryFinish(WaitResult::kNotified)) {
                selected = std::move(candidate);
                break;
            }
        }
    }
    if (selected) {
        selected->Wake();
    }
}

/// 函数功能：完成 NotifyAll 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void ConditionVariable::NotifyAll() noexcept {
    std::deque<std::shared_ptr<WaitNode>> waiters;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        waiters.swap(m_impl->m_waiters);
    }
    for (const auto& waiter : waiters) {
        if (waiter && waiter->TryFinish(WaitResult::kNotified)) {
            waiter->Wake();
        }
    }
}

struct WaitGroup::Impl {
    mutable std::mutex m_mutex;
    std::int64_t m_count{0};
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

/// 函数功能：完成 WaitGroup 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
WaitGroup::WaitGroup() : m_impl(std::make_unique<Impl>()) {}
WaitGroup::~WaitGroup() = default;

/// 函数功能：完成 Add 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] delta 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void WaitGroup::Add(std::int64_t delta) {
    std::deque<std::shared_ptr<WaitNode>> released;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (delta < 0 &&
            (delta == std::numeric_limits<std::int64_t>::min() ||
             m_impl->m_count < -delta)) {
            throw std::logic_error("go2cpp::sync::WaitGroup negative counter");
        }
        if (delta > 0 &&
            m_impl->m_count >
                std::numeric_limits<std::int64_t>::max() - delta) {
            throw std::logic_error("go2cpp::sync::WaitGroup counter overflow");
        }
        m_impl->m_count += delta;
        if (m_impl->m_count == 0) {
            for (const auto& waiter : m_impl->m_waiters) {
                if (waiter) {
                    (void)waiter->TryFinish(WaitResult::kNotified);
                }
            }
            released.swap(m_impl->m_waiters);
        }
    }

    // 在零值转换被锁保护时标记旧一轮结果。后续 Add 不能改变这些结果；
    // 任意 Scheduler 工作都放在锁外执行。
    for (const auto& waiter : released) {
        if (waiter && waiter->result() == WaitResult::kNotified) {
            waiter->Wake();
        }
    }
}

void WaitGroup::Done() { Add(-1); }

/// 函数功能：完成 Wait 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] context 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool WaitGroup::Wait(const ContextPtr& context) {
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_count == 0) {
            return true;
        }
    }
    if (context && context->IsDone()) {
        return false;
    }

    const WaitTarget target = CurrentTarget();
    if (target.unsupported_manual_fiber) {
        return false;
    }
    if (target && target.task->cancellation_requested()) {
        return false;
    }
    const auto waiter =
        std::make_shared<WaitNode>(target.scheduler, target.task);
    ContextSubscription subscription(context, waiter);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_count == 0) {
            return true;
        }
        if ((context && context->IsDone()) ||
            waiter->result() != WaitResult::kWaiting) {
            return false;
        }
        m_impl->m_waiters.push_back(waiter);
        if (!waiter->Arm()) {
            m_impl->m_waiters.pop_back();
            return false;
        }
    }

    const WaitResult result = Await(waiter.get());
    subscription.Reset();
    if (result != WaitResult::kNotified) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        RemoveWaiter(m_impl->m_waiters, waiter);
        return false;
    }
    return true;
}

/// 函数功能：完成 WaitFor 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] timeout 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] parent 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool WaitGroup::WaitFor(ContextDuration timeout, const ContextPtr& parent) {
    CancelFunc cancel;
    const ContextPtr context = TimeoutContext(timeout, parent, &cancel);
    CancelGuard cancel_guard(std::move(cancel));
    return Wait(context);
}

/// 函数功能：完成 Count 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
std::int64_t WaitGroup::Count() const noexcept {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_count;
}

}  // namespace go2cpp::sync
