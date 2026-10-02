#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace go2cpp::core {

// 带谓词等待保持 condition_variable 的外部锁契约，同时让受管 G 挂起
// Fiber，而不是阻塞承载它的 M 线程。
/**
 * 同时服务 Fiber 与普通线程的条件等待适配器。
 *
 * 依赖：Scheduler/Fiber 提供受管等待路径，std::condition_variable 提供
 * 普通线程路径。对上层提供带谓词的 wait/wait_for/wait_until，并要求调用
 * 方持有传入的外部互斥锁；它本身不拥有业务状态，也不负责通知条件的
 * 产生。
 */
class ParkingCondition final {
public:
    using Clock = std::chrono::steady_clock;

    ParkingCondition();
    ~ParkingCondition();
    ParkingCondition(const ParkingCondition&) = delete;
    ParkingCondition& operator=(const ParkingCondition&) = delete;

    /** @brief 唤醒一个等待者。 */
    void notify_one() noexcept;
    /** @brief 唤醒全部等待者。 */
    void notify_all() noexcept;

    /** @brief 持锁等待直到 predicate 成功或 Fiber 被取消。 */
    template <typename Predicate>
    bool wait(std::unique_lock<std::mutex>& lock, Predicate predicate) {
        while (!predicate()) {
            if (WaitOnce(lock, std::nullopt, predicate) ==
                WaitStatus::kCancelled) {
                return predicate();
            }
        }
        return true;
    }

    /** @brief 带相对超时的条件等待。 */
    template <typename Rep, typename Period, typename Predicate>
    bool wait_for(std::unique_lock<std::mutex>& lock,
                  std::chrono::duration<Rep, Period> timeout,
                  Predicate predicate) {
        const auto duration =
            std::chrono::duration_cast<Clock::duration>(timeout);
        const auto now = Clock::now();
        const auto deadline = duration <= Clock::duration::zero()
                                  ? now
                                  : (duration >= Clock::time_point::max() - now
                                         ? Clock::time_point::max()
                                         : now + duration);
        return wait_until(lock, deadline, std::move(predicate));
    }

    /** @brief 等待到绝对截止时间或 predicate 成功。 */
    template <typename Predicate>
    bool wait_until(std::unique_lock<std::mutex>& lock,
                    Clock::time_point deadline, Predicate predicate) {
        while (!predicate()) {
            const auto result = WaitOnce(lock, deadline, predicate);
            if (result != WaitStatus::kNotified) {
                return predicate();
            }
        }
        return true;
    }

    /** @brief 返回当前 Fiber 是否收到取消请求。 */
    static bool CancellationRequested() noexcept;

    // 手动 Fiber（没有 Scheduler/Task 绑定）无法把等待交还给 carrier
    // 线程，因此禁止退回原生 condition_variable 造成整条线程阻塞。
    // 调用方应先绑定 Scheduler，或使用非阻塞接口处理该情况。
    /** @brief 返回当前 Fiber 是否缺少可用 Scheduler 等待后端。 */
    static bool FiberWaitUnsupported() noexcept;

private:
    enum class WaitStatus { kNotified, kTimedOut, kCancelled };
    class WaitNode;

    WaitStatus WaitOnce(std::unique_lock<std::mutex>& lock,
                        std::optional<Clock::time_point> deadline,
                        const std::function<bool()>& predicate);

    std::mutex m_mutex;
    std::deque<std::shared_ptr<WaitNode>> m_waiters;
};

}  // namespace go2cpp::core
