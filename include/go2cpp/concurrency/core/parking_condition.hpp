/**
 * @file parking_condition.hpp
 * @brief Fiber 与普通线程共用的条件等待适配器。
 *
 * ## 模块定位
 *
 * `ParkingCondition` 是运行时里**唯一的"等待原语"**：Scheduler 的 idle 等待、
 * `sync::Mutex` 的阻塞、Channel 的收发、Future 的等待，最终都落到它上面。
 *
 * 它解决的问题是：同一份代码既可能在**受管 Fiber** 里跑（此时等待必须
 * park 协程、让出 M 线程），也可能在**普通线程**里跑（此时退化为原生条件
 * 等待）。调用方不需要知道自己所在的上下文。
 *
 * ## 两条路径
 *
 * - **Fiber 路径**：Scheduler park。WaitNode 绑定 Task，唤醒方通过
 *   wake_or_cancel 把 G 重新放回调度队列，不经过内核 futex；
 * - **线程路径**：每个 WaitNode 自己的 32 位 futex 代际字。notify_one 只推进
 *   **选中节点**的字并只唤醒它（审计 D2：共享代际字会唤醒错节点，被选中的
 *   节点留在内核睡眠队列里丢唤醒）；notify_all 走一次共享广播字 + 一次
 *   FUTEX_WAKE(INT_MAX)（futex_waitv 让等待者同时监听两个字），把 N 次唤醒
 *   系统调用降为 1 次。不支持 futex_waitv 的环境自动退回逐节点唤醒。
 *
 * ## 为什么必须在 park 前自旋一小段
 *
 * 如果直接 FUTEX_WAIT，在"刚检查完条件、还没进入内核"这个窗口里到达的
 * 唤醒会丢失，等待者被迫睡到超时。加一小段自旋（kSpinBudget）让这个窗口
 * 在热路径上极大概率被跨过去 —— 实测 2 个等待者的场景下，无自旋比
 * std::condition_variable 慢 3.3 倍，加了自旋后所有场景都更快。预算大小与
 * 是否真正划算随机器（尤其 WSL2 的唤醒延迟）变化，见同文件 .cpp 实测注释。
 *
 * ## 能力清单
 *
 * - `wait / wait_for / wait_until`：带谓词等待，**调用方必须持有传入的外部
 *   互斥锁**；返回时锁仍被持有（与 `std::condition_variable` 契约一致）；
 * - `notify_one / notify_all`；
 * - `FiberWaitUnsupported()`：当前平台是否支持 Fiber 等待（移植性探测）。
 *
 * ## 明确不负责的事
 *
 * 它**不拥有业务状态**，也**不负责通知条件的产生** —— 谓词和状态由调用方
 * 管理。这样它才能被 Scheduler、Channel、Future 同时复用而互不耦合。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
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

    // ---- 广播代际字（futex_waitv 混合方案）----
    // notify_all 需要唤醒 N 个 native waiter。per-node futex 的地址彼此不同，
    // 逐个 FUTEX_WAKE 就是 N 次系统调用；因此让每个 native waiter 用
    // futex_waitv 同时监听「自己的节点字 + 这个共享广播字」：notify_one 只推进
    // 选中节点的字（保持 D2 的定向唤醒），notify_all 只推进一次广播字并做一次
    // FUTEX_WAKE(INT_MAX)。没有 futex_waitv 的内核/编译环境退回逐节点唤醒。
    struct BroadcastState {
        std::atomic<std::uint32_t> seq{0};
        // 处于 native 等待路径的 waiter 数量：为 0 时广播可跳过系统调用。
        std::atomic<std::uint32_t> waiters{0};
    };
    alignas(64) BroadcastState m_broadcast;
};

}  // namespace go2cpp::core
