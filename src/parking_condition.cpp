#include "go2cpp/core/parking_condition.hpp"

#include "go2cpp/core/hybrid_mutex.hpp"

#include "go2cpp/fiber.hpp"
#include "go2cpp/core/timer.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <utility>

namespace go2cpp::core {

class ParkingCondition::WaitNode final {
public:
    enum class Result { kWaiting, kNotified, kTimedOut, kCancelled };

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
        : m_scheduler(scheduler), m_task(std::move(task)) {}

    /// 函数功能：执行 Finish，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] result 调用方传入的参数，具体约束以头文件声明为准。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    bool Finish(Result result) noexcept {
        auto expected = Result::kWaiting;
        return m_result.compare_exchange_strong(
            expected, result, std::memory_order_acq_rel);
    }

    /// 函数功能：执行 result，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    Result result() const noexcept {
        return m_result.load(std::memory_order_acquire);
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
        // Disarm waits for any in-flight callback to leave this gate, so no
        // raw Scheduler access survives the waiting G's stack cleanup.
        std::lock_guard<HybridMutex> lock(m_wake_mutex);
        if (!m_active) {
            return;
        }
        if (m_scheduler != nullptr && m_task) {
            try {
                (void)m_scheduler->wake_or_cancel(m_task);
            } catch (...) {
            }
        } else {
            m_native_cv.notify_one();
        }
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
        std::lock_guard<HybridMutex> lock(m_wake_mutex);
        m_active = false;
        m_scheduler = nullptr;
    }

    Result WaitNative(std::unique_lock<std::mutex>& outer,
                      std::optional<Clock::time_point> deadline) {
        std::unique_lock<HybridMutex> lock(m_wake_mutex);
        outer.unlock();
        if (deadline) {
            if (!m_native_cv.wait_until(lock, *deadline, [this] {
                    return result() != Result::kWaiting;
                })) {
                (void)Finish(Result::kTimedOut);
            }
        } else {
            m_native_cv.wait(lock, [this] {
                return result() != Result::kWaiting;
            });
        }
        return result();
    }

    /// 函数功能：执行 WaitManaged，完成本函数所属模块的单步操作。
    /// 执行流程：
    /// 1. 校验传入参数以及当前对象/线程状态；
    /// 2. 按状态机规则获取必要的锁并更新内部数据；
    /// 3. 发布结果、唤醒等待者并保持资源生命周期完整。
    /// @param[in] 无；该函数仅使用所属对象或线程局部状态。
    /// @return 通过返回值或对象状态报告执行结果；void/构造析构函数无返回值。
    /// @note 函数不改变公开接口；异常、取消和并发边界由实现中的保护路径处理。
    Result WaitManaged() {
        Scheduler* const scheduler = m_scheduler;
        const auto task = m_task;
        for (;;) {
            // Notify 可能在当前 G 进入 park 前完成。终态已经通过
            // WaitNode::Finish 发布时，直接返回可跳过 Scheduler admission
            // 锁和一次无意义的挂起/恢复，减少 Fiber/线程混合条件变量的
            // 短等待开销，同时不改变 notify-before-park 的语义。
            const Result published = result();
            if (published != Result::kWaiting) {
                return published;
            }
            if (task->cancellation_requested()) {
                (void)Finish(Result::kCancelled);
                return result();
            }
            // A notifier can run before park. Always consume its pending
            // wake permit, even when the terminal result is visible already.
            const bool suspended = scheduler->park_wait(task);
            if (result() != Result::kWaiting) {
                return result();
            }
            if (task->cancellation_requested() ||
                (!suspended && !scheduler->is_running()) ||
                Scheduler::current_scheduler() != scheduler ||
                Scheduler::current_task().get() != task.get()) {
                (void)Finish(Result::kCancelled);
                return result();
            }
            // Unrelated wake permits and unparks are spurious. Re-park the
            // same published node; never busy-yield to emulate waiting.
        }
    }

private:
    Scheduler* m_scheduler;
    std::shared_ptr<Task> m_task;
    std::atomic<Result> m_result{Result::kWaiting};
    HybridMutex m_wake_mutex;
    std::condition_variable_any m_native_cv;
    bool m_active{true};
};

ParkingCondition::ParkingCondition() = default;
ParkingCondition::~ParkingCondition() = default;

/// 函数功能：完成 CancellationRequested 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool ParkingCondition::CancellationRequested() noexcept {
    const auto task = Scheduler::current_task();
    return task && task->cancellation_requested();
}

/// 函数功能：完成 FiberWaitUnsupported 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool ParkingCondition::FiberWaitUnsupported() noexcept {
    return Fiber::Current() != nullptr && !Scheduler::current_task();
}

/// 函数功能：完成 notify_one 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void ParkingCondition::notify_one() noexcept {
    std::shared_ptr<WaitNode> selected;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_waiters.empty()) {
            auto node = std::move(m_waiters.front());
            m_waiters.pop_front();
            if (node->Finish(WaitNode::Result::kNotified)) {
                selected = std::move(node);
                break;
            }
        }
    }
    if (selected) {
        selected->Wake();
    }
}

/// 函数功能：完成 notify_all 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void ParkingCondition::notify_all() noexcept {
    std::deque<std::shared_ptr<WaitNode>> waiters;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        waiters.swap(m_waiters);
        for (const auto& node : waiters) {
            (void)node->Finish(WaitNode::Result::kNotified);
        }
    }
    for (const auto& node : waiters) {
        if (node->result() == WaitNode::Result::kNotified) {
            node->Wake();
        }
    }
}

/// 函数功能：完成 WaitOnce 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] lock 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] deadline 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ParkingCondition::WaitStatus ParkingCondition::WaitOnce(
    std::unique_lock<std::mutex>& lock,
    std::optional<Clock::time_point> deadline,
    const std::function<bool()>& predicate) {
    Scheduler* const scheduler = Scheduler::current_scheduler();
    const auto task = Scheduler::current_task();
    const bool managed = scheduler != nullptr && static_cast<bool>(task);
    if (!managed && Fiber::Current() != nullptr) {
        // 没有 Scheduler 归属的手动 Fiber 无法安全 park；保持外部
        // mutex 锁定并返回取消状态，避免阻塞 carrier OS 线程。
        return WaitStatus::kCancelled;
    }
    if (managed && task->cancellation_requested()) {
        return WaitStatus::kCancelled;
    }
    if (deadline && Clock::now() >= *deadline) {
        return WaitStatus::kTimedOut;
    }

    const auto node = std::make_shared<WaitNode>(
        managed ? scheduler : nullptr, managed ? task : nullptr);
    {
        std::lock_guard<std::mutex> queue_lock(m_mutex);
        // Recheck after acquiring the notification lock. Context cancellation
        // does not hold the channel's external lock, so this second check is
        // required to close its notify-before-publication window.
        if (predicate()) {
            return WaitStatus::kNotified;
        }
        m_waiters.push_back(node);
    }

    Timer timer;
    const auto cleanup = [&] {
        (void)node->Finish(WaitNode::Result::kCancelled);
        node->Disarm();
        timer.Cancel();
        std::lock_guard<std::mutex> queue_lock(m_mutex);
        const auto position =
            std::find(m_waiters.begin(), m_waiters.end(), node);
        if (position != m_waiters.end()) {
            m_waiters.erase(position);
        }
    };

    WaitNode::Result result;
    try {
        if (managed && deadline) {
            const std::weak_ptr<WaitNode> weak(node);
            timer = TimerService::Default().Schedule(*deadline, [weak] {
                if (auto current = weak.lock()) {
                    if (current->Finish(WaitNode::Result::kTimedOut)) {
                        current->Wake();
                    }
                }
            });
        }
        if (managed) {
            lock.unlock();
            result = node->WaitManaged();
        } else {
            result = node->WaitNative(lock, deadline);
        }
        cleanup();
        lock.lock();
    } catch (...) {
        cleanup();
        if (!lock.owns_lock()) {
            lock.lock();
        }
        throw;
    }
    switch (result) {
    case WaitNode::Result::kNotified:
        return WaitStatus::kNotified;
    case WaitNode::Result::kTimedOut:
        return WaitStatus::kTimedOut;
    case WaitNode::Result::kWaiting:
    case WaitNode::Result::kCancelled:
        return WaitStatus::kCancelled;
    }
    return WaitStatus::kCancelled;
}

}  // namespace go2cpp::core
