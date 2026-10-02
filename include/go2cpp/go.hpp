#pragma once

#include "go2cpp/scheduler/scheduler.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace go2cpp {

namespace detail {

// 默认调度器故意采用进程生命周期句柄，避免静态析构顺序导致工作线程访问
// 已销毁对象。程序结束前可以显式调用 shutdown_default_scheduler()。
struct DefaultSchedulerState final {
    std::mutex mutex;
    std::shared_ptr<Scheduler> scheduler;
};

inline DefaultSchedulerState& default_scheduler_state() {
    static auto* state = new DefaultSchedulerState();
    return *state;
}

inline std::shared_ptr<Scheduler> ensure_default_scheduler() {
    auto& state = default_scheduler_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.scheduler || !state.scheduler->is_running()) {
        // Scheduler::shutdown() 是终态，不能对同一个实例再次 start。关闭后
        // 创建新的句柄，旧句柄继续由持有者负责等待/释放。
        state.scheduler = std::make_shared<Scheduler>();
        state.scheduler->start();
    }
    return state.scheduler;
}

}  // namespace detail

// 返回默认调度器的共享句柄。状态对象本身不会在进程退出前自动析构；显式
// shutdown 后旧 Scheduler 句柄仍可等待已提交任务，但不应再提交新任务。
/** @brief 获取默认调度器的共享句柄；首次调用时自动启动。 */
inline std::shared_ptr<Scheduler> default_scheduler_handle() {
    return detail::ensure_default_scheduler();
}

/** @brief 获取默认调度器引用；调用者不负责释放返回对象。 */
inline Scheduler& default_scheduler() {
    return *detail::ensure_default_scheduler();
}

// 停止默认调度器中的工作线程；已提交任务仍遵守 Scheduler 的排空规则。
// 之后再次调用 go() 会创建新的默认 Scheduler；不要继续使用旧引用提交任务。
/** @brief 停止默认调度器并等待其工作线程退出。 */
inline void shutdown_default_scheduler() noexcept {
    std::shared_ptr<Scheduler> scheduler;
    {
        auto& state = detail::default_scheduler_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        // 移出句柄后，下一次 go() 会创建新的 Scheduler；不会把任务提交到
        // 已经进入 stopping 状态的旧实例。
        scheduler = std::move(state.scheduler);
    }
    if (scheduler) {
        try {
            scheduler->shutdown();
        } catch (...) {
            // 关闭入口保持 noexcept，已发布的线程仍由 Scheduler 自己回收。
        }
    }
}

// 显式调度器版本，适合库代码和需要明确所有权的程序。
/**
 * @brief 向指定调度器提交一个 Fiber 任务。
 * @param scheduler 目标调度器；未运行时会自动启动。
 * @param function 任务入口函数。
 * @return 任务句柄；提交失败返回空指针。
 */
inline std::shared_ptr<Task> go(Scheduler& scheduler, Task::Function function) {
    if (!scheduler.is_running()) {
        scheduler.start();
    }
    return scheduler.spawn(std::move(function));
}

// 新手入口：先隐式取得默认调度器，再提交一个 Fiber 任务。
/**
 * @brief 使用默认调度器提交一个 Fiber 任务。
 * @param function 任务入口函数。
 * @return 任务句柄；提交失败返回空指针。
 */
inline std::shared_ptr<Task> go(Task::Function function) {
    auto scheduler = detail::ensure_default_scheduler();
    return scheduler->spawn(std::move(function));
}

// 让 lambda、函数指针等可调用对象无需手动构造 std::function。
template <typename Function,
          typename = std::enable_if_t<std::is_invocable_r_v<
              void, std::decay_t<Function>&>>>
/** @brief 接受任意无参数可调用对象的 go() 便捷重载。 */
inline std::shared_ptr<Task> go(Function&& function) {
    return go(Task::Function(std::forward<Function>(function)));
}

/** @brief default_scheduler() 的大写兼容接口。 */
inline Scheduler& DefaultScheduler() { return default_scheduler(); }
/** @brief shutdown_default_scheduler() 的大写兼容接口。 */
inline void ShutdownDefault() noexcept { shutdown_default_scheduler(); }

template <typename Function,
          typename = std::enable_if_t<std::is_invocable_r_v<
              void, std::decay_t<Function>&>>>
/** @brief go() 的大写兼容接口。 */
inline std::shared_ptr<Task> Go(Function&& function) {
    return go(std::forward<Function>(function));
}

// 轻量 Fiber 描述器。它只保存一个 Task，不持有线程、不创建隐式调度器，
// 因而可以先构造后由 Scheduler::add() 或 bind() 提交。
/**
 * @brief 面向新手的 Fiber 任务句柄。
 * @details 构造时保存入口函数，bind() 将其提交到 Scheduler；状态、等待和
 *          取消操作委托给内部 Task。对象本身不可复制，以避免重复提交。
 */
class fiber final {
public:
    using Function = Task::Function;

    /**
     * @brief 创建尚未提交的 Fiber。
     * @param function Fiber 入口函数。
     * @param options 调度选项；省略时使用默认值。
     */
    explicit fiber(Function function, TaskOptions options = {})
        : m_task(std::make_shared<Task>(std::move(function), options)) {}

    fiber(const fiber&) = delete;
    fiber& operator=(const fiber&) = delete;
    fiber(fiber&&) = delete;
    fiber& operator=(fiber&&) = delete;

    /** @brief 返回内部任务句柄，句柄可用于 Scheduler 的低层 API。 */
    std::shared_ptr<Task> task() const noexcept { return m_task; }

    /**
     * @brief 将 Fiber 提交到指定调度器。
     * @param scheduler 目标调度器。
     * @return 成功提交的任务句柄；已绑定其他调度器或提交失败返回空值。
     */
    std::shared_ptr<Task> bind(Scheduler& scheduler) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_scheduler && m_scheduler != &scheduler) {
            return {};
        }
        auto admitted = scheduler.add(m_task);
        if (admitted) {
            m_scheduler = &scheduler;
        }
        return admitted;
    }

    /** @brief 阻塞等待 Fiber 完成。@return 正常完成返回 true。 */
    bool wait() const { return m_task && m_task->wait(); }

    /**
     * @brief 在指定时长内等待 Fiber 完成。
     * @param timeout 最长等待时长。
     * @return 在超时前完成返回 true。
     */
    bool wait_for(std::chrono::steady_clock::duration timeout) const {
        return m_task && m_task->wait_for(timeout);
    }

    /** @brief 请求取消 Fiber。@return 请求被接受返回 true。 */
    bool cancel() { return m_task && m_task->cancel(); }
    /** @brief 返回 Fiber 当前状态。 */
    GState state() const noexcept {
        return m_task ? m_task->state() : GState::kCancelled;
    }
    /** @brief 返回 Fiber 是否已开始执行。 */
    bool started() const noexcept { return m_task && m_task->started(); }

private:
    std::shared_ptr<Task> m_task;
    mutable std::mutex m_mutex;
    Scheduler* m_scheduler{nullptr};
};

}  // namespace go2cpp
