#pragma once

/**
 * @file go.hpp
 * @brief 新手友好的 `go()` / `after()` / `every()` 入口与默认调度器管理。
 *
 * ## 模块定位
 *
 * `tools` 的调度器门面：把 `Scheduler` 的显式生命周期包装成「一行提交任务」
 * 的新手 API，同时保留显式调度器重载供库代码使用。上游是示例、测试与业务
 * 入口，下游是 `Scheduler`/`Task`/`core::Timer`。
 *
 * ## 本文件定义什么
 *
 * - 默认调度器：`default_scheduler()`/`default_scheduler_handle()`/
 *   `shutdown_default_scheduler()` 及内部 `DefaultSchedulerState`；
 * - `go()`：显式/隐式调度器、任意可调用对象三个重载；
 * - `after()`/`every()`：默认调度器上的定时任务；
 * - `fiber`：可先构造、后 `bind()` 的轻量任务句柄；
 * - 大写兼容别名（`Go`/`DefaultScheduler`/`ShutdownDefault`）。
 *
 * ## 阅读建议
 *
 * 先读 `ensure_default_scheduler()` 的「关闭后重建」逻辑，再看 `fiber::bind`
 * 的所有权检查；显式调度器重载只是薄转发。
 *
 * ## 边界与陷阱
 *
 * 默认调度器状态故意进程级泄漏（避免静态析构顺序问题）；`shutdown` 是终态，
 * 之后再次 `go()` 会创建新实例，旧引用不能继续提交任务。
 */

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
/**
 * @brief 默认调度器的进程级状态。
 *
 * ## 定位
 * `ensure_default_scheduler()` 的共享槽位；`shutdown_default_scheduler()`
 * 负责移出句柄。只被本文件的三个入口使用。
 *
 * ## 不变量
 * - `scheduler` 要么为空，要么指向一个已 `start()` 的实例；
 * - 所有读写都必须持有 `mutex`；
 * - 状态对象故意不析构（进程生命周期）。
 */
struct DefaultSchedulerState final {
    std::mutex mutex;                  ///< 保护 scheduler；ensure/shutdown 读写时持有
    std::shared_ptr<Scheduler> scheduler; ///< 当前默认调度器；关闭后置空，下次 go() 重建
};

inline DefaultSchedulerState& default_scheduler_state() {
    static auto* state = new DefaultSchedulerState();
    return *state;
}

/**
 * @brief 取得默认调度器；不存在或已停止时创建并启动新实例。
 *
 * ## 步骤
 * step1. 取进程级状态并加锁；
 * step2. 句柄为空或 `is_running()` 为假时新建 `Scheduler` 并 `start()`；
 * step3. 返回共享句柄（调用方共享所有权）。
 *
 * @note `Scheduler::shutdown()` 是终态，不能对同一实例再次 `start`，
 *       因此关闭后必须换新实例。
 */
inline std::shared_ptr<Scheduler> ensure_default_scheduler() {
    // step1: 取状态并加锁
    auto& state = default_scheduler_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    // step2: 空或已停止则重建
    if (!state.scheduler || !state.scheduler->is_running()) {
        // Scheduler::shutdown() 是终态，不能对同一个实例再次 start。关闭后
        // 创建新的句柄，旧句柄继续由持有者负责等待/释放。
        state.scheduler = std::make_shared<Scheduler>();
        state.scheduler->start();
    }
    // step3: 返回共享句柄
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
/**
 * @brief 停止默认调度器并等待工作线程退出。
 *
 * ## 步骤
 * step1. 持锁把句柄移出状态，保证下一次 `go()` 重建新实例；
 * step2. 在锁外调用 `shutdown()`，避免长时间持锁；
 * step3. 吞掉关闭异常，保持 `noexcept` 入口。
 */
inline void shutdown_default_scheduler() noexcept {
    // step1: 先移出句柄，避免新任务提交到 stopping 实例
    std::shared_ptr<Scheduler> scheduler;
    {
        auto& state = detail::default_scheduler_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        // 移出句柄后，下一次 go() 会创建新的 Scheduler；不会把任务提交到
        // 已经进入 stopping 状态的旧实例。
        scheduler = std::move(state.scheduler);
    }
    // step2/step3: 锁外关闭；异常不穿过 noexcept
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

/**
 * @brief 使用默认调度器在延时后提交一个 Fiber。
 * @param delay 相对当前 steady_clock 的等待时长；负值按立即执行处理。
 * @param function 到期后执行的 Fiber 入口。
 * @return 可取消、可查询状态的定时器句柄。
 * @note 该接口只负责注册，不会在定时器线程直接执行用户函数。
 */
inline core::Timer after(std::chrono::steady_clock::duration delay,
                         Task::Function function) {
    auto scheduler = detail::ensure_default_scheduler();
    return scheduler->schedule_after(delay, std::move(function));
}

/**
 * @brief 使用默认调度器按周期提交 Fiber。
 * @param period 周期，必须为正数。
 * @param function 每次到期后执行的 Fiber 入口。
 * @param options 固定延迟/固定频率、最大次数和等待策略。
 * @return 可取消、可查询状态的定时器句柄；参数无效时返回无效句柄。
 */
inline core::Timer every(std::chrono::steady_clock::duration period,
                         Task::Function function,
                         core::TimerOptions options = {}) {
    auto scheduler = detail::ensure_default_scheduler();
    return scheduler->schedule_every(period, std::move(function), options);
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
 *
 * ## 定位
 * 新手 API 的轻量句柄：先 `fiber f(fn)` 构造，再由 `bind()` 提交给某个
 * `Scheduler`；也可直接通过 `task()` 交给底层 API。
 *
 * ## 生命周期与线程模型
 * 构造时创建 `Task`，自身不启动线程；`bind()` 只允许绑定一个调度器。对象
 * 不可拷贝/移动，`m_mutex` 保护绑定检查。`wait()` 会阻塞当前执行流。
 *
 * ## 不变量
 * - 构造后 `m_task` 非空；
 * - `m_scheduler` 非空时必定已经成功 `add()`，且不会再绑定其他调度器；
 * - 同一对象的 `bind()` 由 `m_mutex` 串行化。
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
    /**
     * @brief 将 Fiber 提交到指定调度器。
     *
     * ## 步骤
     * step1. 持锁检查是否已绑定到别的调度器，是则拒绝；
     * step2. 调用 `Scheduler::add` 提交；
     * step3. 提交成功才记录绑定，失败返回空。
     */
    std::shared_ptr<Task> bind(Scheduler& scheduler) {
        // step1: 一个句柄只允许绑定一个调度器
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_scheduler && m_scheduler != &scheduler) {
            return {};
        }
        // step2: 提交
        auto admitted = scheduler.add(m_task);
        // step3: 成功后才记录绑定
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
    std::shared_ptr<Task> m_task;   ///< 内部任务；构造时创建，task()/wait()/cancel() 读取
    mutable std::mutex m_mutex;     ///< 保护 m_scheduler；bind() 持锁检查与更新
    Scheduler* m_scheduler{nullptr};///< 已绑定的调度器（不拥有）；bind 成功后设置
};

}  // namespace go2cpp
