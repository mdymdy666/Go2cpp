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

    WaitNode(Scheduler* scheduler, std::shared_ptr<Task> task)
        : m_scheduler(scheduler), m_task(std::move(task)) {}

    bool Finish(Result result) noexcept {
        auto expected = Result::kWaiting;
        return m_result.compare_exchange_strong(
            expected, result, std::memory_order_acq_rel);
    }

    Result result() const noexcept {
        return m_result.load(std::memory_order_acquire);
    }

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

bool ParkingCondition::CancellationRequested() noexcept {
    const auto task = Scheduler::current_task();
    return task && task->cancellation_requested();
}

bool ParkingCondition::FiberWaitUnsupported() noexcept {
    return Fiber::Current() != nullptr && !Scheduler::current_task();
}

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
