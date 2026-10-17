/**
 * @file timer.cpp
 * @brief 定时器服务（Timer / TimerService）的实现。
 *
 * ## 模块定位
 *
 * Timer 是运行时里**唯一自带后台线程**的并发设施：它不依赖 Scheduler，
 * 也不依赖 Fiber。定时回调默认在 Timer 自己的线程上执行（除非通过
 * `TimerOptions::executor` 交给调度器/Fiber 去跑）。
 *
 * 这样分层的原因：
 * - 调度器自己需要"超时"能力（`Timer::wait_for`），如果 Timer 反过来依赖
 *   调度器会形成环；
 * - 纯线程实现让 Timer 可以被**任何**线程安全地使用，包括还没有启动
 *   Scheduler 的初始化阶段。
 *
 * ## 这个文件里的三层结构
 *
 * 1. **`Timer`**：调用方持有的句柄，轻量、可移动；析构即取消
 *    （`~Timer()` 等价于 `Cancel()`，这一点在 `timer.hpp` 里有说明，
 *    写测试时若不持有句柄会观察不到回调）；
 * 2. **`Timer::State`**：句柄与服务端共享的可变状态（回调、状态机、
 *    下次到期时间、取消标记等），由 `m_mutex` 保护；
 * 3. **`TimerService::Impl`**：真正的服务实现，持有后台线程、
 *    按到期时间排序的 `multimap` 和用于 O(1) 删除的反向索引。
 *
 * ## 并发模型
 *
 * - 服务端用一个 `std::condition_variable` 驱动：插入/删除/重置都会
 *   `notify_all()` 并递增 `m_generation`，后台线程据此重新计算等待时长；
 * - 所有对 `m_entries` / `m_index` 的访问都在 `m_mutex` 下；
 * - 单个 `Timer::State` 的字段由 `State::m_mutex` 保护，**与**服务端
 *   `m_mutex` 是两把不同的锁（加锁顺序：先服务端锁、后状态锁，避免反转）。
 */
#include "go2cpp/core/timer.hpp"
#include "go2cpp/logging/log.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace go2cpp::core {

namespace {

/**
 * @brief 把负的时长钳到 0。
 * @details 定时器的剩余时长在时钟回拨或刚到期时可能算成负数；对外暴露
 *          「还剩多久」时统一钳到 0，避免调用方看到负值。
 */
std::chrono::steady_clock::duration nonnegative(
    std::chrono::steady_clock::duration value) noexcept {
    return value < std::chrono::steady_clock::duration::zero()
               ? std::chrono::steady_clock::duration::zero()
               : value;
}

/** @brief 判断时长是否严格大于 0（用于"是否还有等待时间"这类判断）。 */
bool positive(std::chrono::steady_clock::duration value) noexcept {
    return value > std::chrono::steady_clock::duration::zero();
}

/**
 * @brief 时间点加时长，溢出时饱和到 `time_point::max()`。
 * @details 直接用 `point + delta` 在极端截止时间下会溢出成负值，导致定时器
 *          立刻触发。这里显式检测加法溢出并饱和，保证"很久以后"仍然是
 *          "最早不早于现在"，不会变成"已经过期"。
 *          这也是把周期定时器的 `kAt` 决策换算成绝对时间时必须的安全性。
 */
std::chrono::steady_clock::time_point saturating_add(
    std::chrono::steady_clock::time_point point,
    std::chrono::steady_clock::duration delta) noexcept {
    using Clock = std::chrono::steady_clock;
    if (delta <= Clock::duration::zero()) return point;
    const auto point_count = point.time_since_epoch().count();
    const auto delta_count = delta.count();
    const auto maximum = Clock::duration::max().count();
    if (delta_count > 0 && point_count > maximum - delta_count) {
        return Clock::time_point::max();
    }
    return Clock::time_point(Clock::duration(point_count + delta_count));
}

/**
 * @brief 按 `TimerDurationLimits` 的分级阈值对定时任务时长告警。
 * @details 两级阈值：超过 `error_after` 记 ERROR，超过 `warn_after` 记 WARN。
 *          阈值为 0 表示关闭该级别。这个函数只观测、不干预 —— 一个跑得太久的
 *          回调不会被中断，只是留下日志线索。
 */
void log_long_duration(std::chrono::steady_clock::duration duration,
                       const TimerDurationLimits& limits,
                       const char* operation) noexcept {
    if (duration < std::chrono::steady_clock::duration::zero()) return;
    try {
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(duration).count();
        if (limits.error_after > std::chrono::steady_clock::duration::zero() &&
            duration >= limits.error_after) {
            auto logger = GO2CPP_LOG_NAME("Timer");
            GO2CPP_LOG_ERROR(logger) << operation
                                     << " 定时任务时长达到 ERROR 阈值，seconds="
                                     << seconds;
        } else if (limits.warn_after > std::chrono::steady_clock::duration::zero() &&
                   duration >= limits.warn_after) {
            auto logger = GO2CPP_LOG_NAME("Timer");
            GO2CPP_LOG_WARN(logger) << operation
                                    << " 定时任务时长达到 WARN 阈值，seconds="
                                    << seconds;
        }
    } catch (...) {
        // 日志异常不能影响定时任务的正常注册。
    }
}

}  // 匿名命名空间

/**
 * @brief `Timer` 句柄与服务端共享的可变状态。
 *
 * ## 作用
 *
 * `Timer` 本身只是一个 `shared_ptr<State>` 的包装：拷贝/移动 `Timer` 就是在
 * 拷贝/移动这个指针。真正的"定时器"生命周期由这里的字段决定。
 *
 * ## 字段分组
 *
 * - **回调**：`m_callback`（一次性）与 `m_repeat_callback`（可拿到
 *   `TimerContext` 并返回 `TimerDecision` 的周期回调），二者取其一；
 * - **执行器**：`m_executor` 非空时，回调被提交给外部执行器（通常是调度器），
 *   而不是在 Timer 线程上直接跑；
 * - **状态机**：`m_status`（`TimerState`：kPending/kRunning/kCancelled/
 *   kExpired/kFailed）与 `m_active` 共同决定是否还能触发。`m_active` 是
 *   取消/失效的快速标记，状态机值对外可见；
 * - **观测**：`m_run_count`、`m_next_deadline`、`m_failure`。
 *
 * ## 并发
 *
 * 所有字段由 `m_mutex` 保护。服务端在 `Execute` 时会短暂持有它只为把字段
 * **拷贝出来**，然后立刻释放 —— 回调本身在锁外执行，避免用户代码持锁。
 */
struct Timer::State final {
    std::weak_ptr<TimerService::Impl> m_owner;
    mutable std::mutex m_mutex;
    std::function<void()> m_callback;
    TimerRepeatCallback m_repeat_callback;
    TimerExecutorPtr m_executor;
    TimerOptions m_options;
    TimerState m_status{TimerState::kPending};
    bool m_active{true};
    std::size_t m_run_count{0};
    std::chrono::steady_clock::time_point m_next_deadline{};
    std::exception_ptr m_failure;
};

/**
 * @brief 定时器服务端：后台线程 + 到期时间索引。
 *
 * ## 模块定位
 *
 * `TimerService` 是对外的门面（单例 `Default()`），真正的调度全在这个 `Impl` 里。
 * 它由**一个**后台线程驱动：线程在 `m_cv` 上等待到最近一个截止时间，醒来后
 * 把已到期的条目依次取出执行。
 *
 * ## 数据结构
 *
 * - `m_entries`：`multimap<time_point, State::ptr>`，按到期时间有序，
 *   `begin()` 就是下一个要触发的定时器。用 multimap 而不是 map，因为同一
 *   时刻可以有多个定时器；
 * - `m_index`：`unordered_map<State*, Entries::iterator>`，从状态指针直接
 *   定位到 multimap 里的位置，让 `Remove`/`Reset` 是 O(1) 而不是线性扫描。
 *   两者必须**始终同步**维护。
 * - `m_generation`：每次条目变化都自增。后台线程醒来后比对它，判断"等待
 *   期间是否有新定时器插入"，从而决定是重新计算等待还是继续处理。
 *
 * ## 能力清单
 *
 * - 插入 / 删除 / 取消 / 重置定时器（O(log n) 插入、O(1) 定位）；
 * - 周期定时：由回调返回的 `TimerDecision` 决定下次时间（`kAfter` 相对、
 *   `kAt` 绝对、`kContinue` 沿用 period、`kStop` 停止）；
 * - 时长分级告警（WARN / ERROR 两级阈值）；
 * - 异常隔离：回调抛出的异常被捕获后交给 `m_exception_handler`，不会终止
 *   后台线程；
 * - 优雅停止：析构时置 `m_stopping`、唤醒线程、join，然后把遗留条目统一
 *   置为取消状态，避免调用方看到永远 pending 的定时器。
 *
 * ## 一个必须注意的约束
 *
 * 析构里检查"是否在 Timer 自己的线程上析构"并 `std::terminate()` ——
 * 因为在定时回调里销毁 `TimerService` 会导致线程 join 自身。这个检查是
 * 有意为之，不是防御性冗余。
 */
class TimerService::Impl final {
public:
    using Entries =
        std::multimap<Clock::time_point, std::shared_ptr<Timer::State>>;

    explicit Impl(TimerExceptionHandler handler,
                  TimerDurationLimits duration_limits)
        : m_exception_handler(std::move(handler)),
          m_duration_limits(duration_limits),
          m_thread([this] { Run(); }) {}

    bool SetDurationLimits(TimerDurationLimits limits) noexcept {
        if (limits.warn_after < Clock::duration::zero() ||
            limits.error_after < Clock::duration::zero()) return false;
        if (limits.warn_after > Clock::duration::zero() &&
            limits.error_after > Clock::duration::zero() &&
            limits.warn_after > limits.error_after) return false;
        std::lock_guard<std::mutex> lock(m_limits_mutex);
        m_duration_limits = limits;
        return true;
    }

    TimerDurationLimits DurationLimits() const noexcept {
        std::lock_guard<std::mutex> lock(m_limits_mutex);
        return m_duration_limits;
    }

    void LogLongDuration(Clock::duration duration,
                         const char* operation) const noexcept {
        log_long_duration(duration, DurationLimits(), operation);
    }

    ~Impl() {
        Entries abandoned;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_thread.joinable() &&
                m_thread.get_id() == std::this_thread::get_id()) {
                std::terminate();
            }
            m_stopping = true;
            ++m_generation;
            abandoned.swap(m_entries);
            m_index.clear();
        }
        m_cv.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        for (const auto& entry : abandoned) {
            CancelState(entry.second);
        }
    }

    /**
     * @brief 把一个定时器状态登记到服务端。
     * @details 同时维护 `m_entries`（按时间有序）与 `m_index`（O(1) 定位），
     *          递增 `m_generation` 并唤醒后台线程重算等待时长。
     * @throws std::runtime_error 服务已停止时抛出（不能在 shutdown 期间新增）。
     */
    void Add(const std::shared_ptr<Timer::State>& state,
             Clock::time_point deadline) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                throw std::runtime_error("timer service is stopped");
            }
            const auto it = m_entries.emplace(deadline, state);
            m_index[state.get()] = it;
            ++m_generation;
        }
        m_cv.notify_all();
    }

    /**
     * @brief 从服务端摘除一个定时器。
     * @return 是否真的摘除了（不存在时返回 false，不视为错误）。
     * @note 只动容器，不改 `State` 的状态机 —— 状态机的收尾由调用方或
     *       `CancelState` 负责。
     */
    bool Remove(const std::shared_ptr<Timer::State>& state) noexcept {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_index.find(state.get());
            if (found != m_index.end()) {
                m_entries.erase(found->second);
                m_index.erase(found);
                ++m_generation;
                removed = true;
            }
        }
        if (removed) {
            m_cv.notify_all();
        }
        return removed;
    }

    /**
     * @brief 取消一个定时器：置状态为 kCancelled 并摘除。
     * @details `~Timer()` 走的就是这条路径，所以"临时变量 Timer 立即析构"
     *          会取消定时器 —— 这是常见的踩坑点。
     */
    bool Cancel(const std::shared_ptr<Timer::State>& state) noexcept {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_index.find(state.get());
            if (found != m_index.end()) {
                m_entries.erase(found->second);
                m_index.erase(found);
                ++m_generation;
            }
            std::lock_guard<std::mutex> state_lock(state->m_mutex);
            if (!state->m_active ||
                state->m_status == TimerState::kCompleted ||
                state->m_status == TimerState::kCancelled ||
                state->m_status == TimerState::kFailed) {
                return false;
            }
            state->m_active = false;
            state->m_status = TimerState::kCancelled;
            state->m_callback = {};
            state->m_repeat_callback = {};
            state->m_executor.reset();
        }
        m_cv.notify_all();
        return true;
    }

    /**
     * @brief 把已有定时器改到新的截止时间。
     * @details 不是"删除再插入"，而是在原位置重排：先摘除旧条目，再以新时间
     *          插入，并同步更新 `m_index` 与 `m_generation`。这样 `Timer` 句柄
     *          与 `run_count` 等观测值都得以保留。
     * @return 定时器不存在或已失效时返回 false。
     */
    bool Reset(const std::shared_ptr<Timer::State>& state,
               Clock::time_point deadline) noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_index.find(state.get());
        if (found == m_index.end()) {
            return false;
        }
        std::lock_guard<std::mutex> state_lock(state->m_mutex);
        if (!state->m_active || state->m_status != TimerState::kPending) {
            return false;
        }
        m_entries.erase(found->second);
        const auto it = m_entries.emplace(deadline, state);
        found->second = it;
        state->m_next_deadline = deadline;
        ++m_generation;
        m_cv.notify_all();
        return true;
    }

    /**
     * @brief 执行一次到期的定时器，并决定它是否继续。
     *
     * 主要功能：在锁外跑用户回调，然后把结果交给 `Finish` 决定重排或收尾。
     *
     * step1: 在 `State::m_mutex` 下校验状态（必须 active 且 kPending），
     *        把它切成 kRunning、递增 run_count，并**拷贝**出回调/执行器/选项；
     *        随后立刻释放锁 —— 用户回调绝不能在持锁状态下执行。
     * step2: 按优先级选择执行方式：周期回调 > 外部执行器 > 普通回调；
     *        捕获所有异常交给 `NotifyException`，保证后台线程不被用户代码打死。
     * step3: 把执行结果（`TimerDecision` 或异常）交给 `Finish` 统一收尾，
     *        由它决定是重排下一次、标为已完成、还是标为失败。
     */
    void Execute(const std::shared_ptr<Timer::State>& state,
                 Clock::time_point scheduled_deadline) noexcept {
        TimerContext context;
        std::function<void()> callback;
        TimerRepeatCallback repeat_callback;
        TimerExecutorPtr executor;
        TimerOptions options;
        {
            std::lock_guard<std::mutex> lock(state->m_mutex);
            if (!state->m_active || state->m_status != TimerState::kPending) {
                return;
            }
            state->m_status = TimerState::kRunning;
            ++state->m_run_count;
            context.run_count = state->m_run_count;
            context.scheduled_deadline = scheduled_deadline;
            context.started_at = Clock::now();
            callback = state->m_callback;
            repeat_callback = state->m_repeat_callback;
            executor = state->m_executor;
            options = state->m_options;
        }

        TimerDecision decision = TimerDecision::Continue();
        std::exception_ptr failure;
        try {
            if (repeat_callback) {
                decision = repeat_callback(context);
            } else if (executor) {
                if (!executor->Submit(std::move(callback))) {
                    throw std::runtime_error("timer executor rejected callback");
                }
                decision = options.repeat == TimerRepeatMode::kOnce
                               ? TimerDecision::Stop()
                               : TimerDecision::Continue();
            } else if (callback) {
                callback();
                decision = options.repeat == TimerRepeatMode::kOnce
                               ? TimerDecision::Stop()
                               : TimerDecision::Continue();
            } else {
                decision = TimerDecision::Stop();
            }
        } catch (...) {
            failure = std::current_exception();
            NotifyException(failure);
        }
        Finish(state, context, decision, failure);
    }

    /**
     * @brief 定时器一次执行结束后的状态收尾与重排决策。
     *
     * 主要功能：根据 `TimerDecision` 把定时器移到下一次触发时间，或终结它。
     *
     * step1: 无异常时按决策类型做时长告警观测（kAfter/kAt/kContinue 分别对应
     *        相对延迟、绝对时间差、沿用 period），这一步只记日志不改变行为。
     * step2: 有异常时把状态机置为 kFailed 并记录 `m_failure`，同时从容器摘除，
     *        不再重排 —— 失败的定时器不会静默重试。
     * step3: 无异常时按 kStop / kAt / kAfter / kContinue 计算下一次截止时间；
     *        需要继续的重新插回 `m_entries` 并更新 `m_index`，否则置为 kExpired
     *        并摘除。所有分支都保证 `m_entries` 与 `m_index` 同步。
     */
    void Finish(const std::shared_ptr<Timer::State>& state,
                const TimerContext& context,
                const TimerDecision& decision,
                const std::exception_ptr& failure) noexcept {
        if (!failure) {
            switch (decision.kind()) {
                case TimerDecision::Kind::kAfter:
                    LogLongDuration(nonnegative(decision.delay()),
                                    "周期定时器");
                    break;
                case TimerDecision::Kind::kAt: {
                    const auto now = Clock::now();
                    if (decision.deadline() > now) {
                        LogLongDuration(decision.deadline() - now,
                                        "周期定时器");
                    }
                    break;
                }
                case TimerDecision::Kind::kContinue:
                    LogLongDuration(state->m_options.period, "周期定时器");
                    break;
                case TimerDecision::Kind::kStop:
                    break;
            }
        }
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::lock_guard<std::mutex> state_lock(state->m_mutex);
            // 服务停止后，正在执行的回调只能收敛为取消，禁止重新插入
            // 队列；析构线程已经接管并清空队列，继续插入会留下永远
            // 无法执行的重复任务。
            if (m_stopping || !state->m_active ||
                state->m_status == TimerState::kCancelled) {
                state->m_active = false;
                if (state->m_status != TimerState::kFailed) {
                    state->m_status = TimerState::kCancelled;
                }
                state->m_callback = {};
                state->m_repeat_callback = {};
                state->m_executor.reset();
                return;
            }
            if (failure) {
                state->m_active = false;
                state->m_status = TimerState::kFailed;
                state->m_failure = failure;
                state->m_callback = {};
                state->m_repeat_callback = {};
                state->m_executor.reset();
                return;
            }
            const TimerOptions options = state->m_options;
            const bool repeat = options.repeat != TimerRepeatMode::kOnce;
            if (!repeat || decision.kind() == TimerDecision::Kind::kStop ||
                (options.max_runs != 0 &&
                 state->m_run_count >= options.max_runs)) {
                state->m_active = false;
                state->m_status = TimerState::kCompleted;
                state->m_callback = {};
                state->m_repeat_callback = {};
                state->m_executor.reset();
                return;
            }

            const auto now = Clock::now();
            Clock::time_point next = now;
            switch (decision.kind()) {
                case TimerDecision::Kind::kAfter:
                    next = saturating_add(now, nonnegative(decision.delay()));
                    break;
                case TimerDecision::Kind::kAt:
                    next = decision.deadline();
                    break;
                case TimerDecision::Kind::kContinue:
                    if (options.repeat == TimerRepeatMode::kFixedRate) {
                        next = saturating_add(context.scheduled_deadline,
                                              options.period);
                        if (options.skip_missed && next <= now) {
                            next = saturating_add(now, options.period);
                        }
                    } else {
                        next = saturating_add(now, options.period);
                    }
                    break;
                case TimerDecision::Kind::kStop:
                    state->m_active = false;
                    state->m_status = TimerState::kCompleted;
                    state->m_callback = {};
                    state->m_repeat_callback = {};
                    state->m_executor.reset();
                    return;
            }
            if (next < now) {
                next = now;
            }
            if (!positive(options.period) &&
                decision.kind() == TimerDecision::Kind::kContinue) {
                state->m_active = false;
                state->m_status = TimerState::kFailed;
                state->m_failure = std::make_exception_ptr(
                    std::invalid_argument("timer period must be positive"));
                state->m_callback = {};
                state->m_repeat_callback = {};
                state->m_executor.reset();
                return;
            }
            state->m_next_deadline = next;
            state->m_status = TimerState::kPending;
            const auto it = m_entries.emplace(next, state);
            m_index[state.get()] = it;
            ++m_generation;
            notify = true;
        }
        if (notify) {
            m_cv.notify_all();
        }
    }

    void NotifyException(const std::exception_ptr& failure) noexcept {
        TimerExceptionHandler handler;
        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            handler = m_exception_handler;
        }
        if (!handler) {
            return;
        }
        try {
            handler(failure);
        } catch (...) {
        }
    }

    void SetExceptionHandler(TimerExceptionHandler handler) {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        m_exception_handler = std::move(handler);
    }

    bool Running() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_stopping;
    }

    std::size_t PendingCount() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    std::optional<Clock::time_point> NextDeadline() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_entries.empty()) {
            return std::nullopt;
        }
        return m_entries.begin()->first;
    }

private:
    static void CancelState(const std::shared_ptr<Timer::State>& state) noexcept {
        std::lock_guard<std::mutex> lock(state->m_mutex);
        if (state->m_status == TimerState::kPending ||
            state->m_status == TimerState::kRunning) {
            state->m_active = false;
            state->m_status = TimerState::kCancelled;
        }
        state->m_callback = {};
        state->m_repeat_callback = {};
        state->m_executor.reset();
    }

    /**
     * @brief 后台线程主循环：等最近的定时器到期，然后逐个执行。
     *
     * 主要功能：这是 Timer 服务唯一的工作线程，所有定时触发都从这里发出。
     *
     * step1: 在 `m_mutex` 下取当前 `m_generation` 和最近截止时间；空表时
     *        无限等待，否则等待到该时间点（同时记录 generation 以便识别
     *        "等待期间有新插入"）。
     * step2: 醒来后重新加锁校验 generation —— 如果期间条目变过，说明有新
     *        定时器插到了更早的位置，直接回到 step1 重算等待，避免漏触发。
     * step3: 把已经到期的条目**成批摘出**容器（摘出时同步维护 index），
     *        然后释放锁、逐个 `Execute`。摘出后再执行，是为了用户回调里
     *        可以安全地再次插入/取消定时器而不自锁。
     * step4: 若 `m_stopping` 置位则退出循环；否则回到 step1 继续等待。
     */
    void Run() noexcept {
        for (;;) {
            std::shared_ptr<Timer::State> expired;
            std::chrono::steady_clock::time_point deadline{};
            TimerWaitMode wait_mode = TimerWaitMode::kConditionVariable;
            std::chrono::steady_clock::duration spin_window{};
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_stopping) {
                    return;
                }
                if (m_entries.empty()) {
                    m_cv.wait(lock, [this] {
                        return m_stopping || !m_entries.empty();
                    });
                    continue;
                }
                const auto it = m_entries.begin();
                deadline = it->first;
                {
                    std::lock_guard<std::mutex> state_lock(it->second->m_mutex);
                    wait_mode = it->second->m_options.wait_mode;
                    spin_window = nonnegative(it->second->m_options.spin_window);
                }
                const auto now = Clock::now();
                if (now < deadline) {
                    const auto generation = m_generation.load();
                    const auto remaining = deadline - now;
                    if (wait_mode == TimerWaitMode::kBusyWait &&
                        remaining <= spin_window) {
                        lock.unlock();
                        for (;;) {
                            if (m_generation.load() != generation ||
                                Clock::now() >= deadline) {
                                break;
                            }
                            std::this_thread::yield();
                        }
                        continue;
                    }
                    m_cv.wait_until(lock, deadline, [this, generation] {
                        return m_stopping || m_generation.load() != generation;
                    });
                    continue;
                }
                expired = it->second;
                m_index.erase(expired.get());
                m_entries.erase(it);
            }
            Execute(expired, deadline);
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    Entries m_entries;
    std::unordered_map<Timer::State*, Entries::iterator> m_index;
    std::atomic<std::uint64_t> m_generation{0};
    bool m_stopping{false};
    mutable std::mutex m_handler_mutex;
    TimerExceptionHandler m_exception_handler;
    mutable std::mutex m_limits_mutex;
    TimerDurationLimits m_duration_limits;
    std::thread m_thread;
};

Timer::Timer() noexcept = default;

Timer::Timer(std::shared_ptr<State> state) : m_state(std::move(state)) {}

Timer::~Timer() {
    Cancel();
}

Timer::Timer(Timer&& other) noexcept = default;

Timer& Timer::operator=(Timer&& other) noexcept {
    if (this != &other) {
        Cancel();
        m_state = std::move(other.m_state);
    }
    return *this;
}

void Timer::Cancel() noexcept {
    const auto state = m_state;
    if (!state) {
        return;
    }
    if (const auto owner = state->m_owner.lock()) {
        owner->Cancel(state);
    } else {
        std::lock_guard<std::mutex> lock(state->m_mutex);
        if (state->m_active) {
            state->m_active = false;
            state->m_status = TimerState::kCancelled;
            state->m_callback = {};
            state->m_repeat_callback = {};
            state->m_executor.reset();
        }
    }
}

bool Timer::Active() const noexcept {
    const auto state = m_state;
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->m_mutex);
    return state->m_active;
}

TimerState Timer::StateValue() const noexcept {
    const auto state = m_state;
    if (!state) {
        return TimerState::kInvalid;
    }
    std::lock_guard<std::mutex> lock(state->m_mutex);
    return state->m_status;
}

std::size_t Timer::RunCount() const noexcept {
    const auto state = m_state;
    if (!state) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(state->m_mutex);
    return state->m_run_count;
}

bool Timer::Reset(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto state = m_state;
    if (!state) {
        return false;
    }
    const auto owner = state->m_owner.lock();
    return owner && owner->Reset(state, deadline);
}

bool Timer::Refresh() noexcept {
    const auto state = m_state;
    if (!state) {
        return false;
    }
    std::chrono::steady_clock::time_point deadline{};
    {
        std::lock_guard<std::mutex> lock(state->m_mutex);
        if (!state->m_active || !positive(state->m_options.period)) {
            return false;
        }
        deadline = saturating_add(std::chrono::steady_clock::now(),
                                  state->m_options.period);
    }
    return Reset(deadline);
}

TimerService::TimerService(TimerExceptionHandler exception_handler,
                           TimerDurationLimits duration_limits)
    : m_impl(std::make_shared<Impl>(std::move(exception_handler),
                                    duration_limits)) {}

TimerService::~TimerService() = default;

TimerService& TimerService::Default() {
    static TimerService service;
    return service;
}

Timer TimerService::Schedule(Clock::time_point deadline,
                             std::function<void()> callback) {
    if (!callback) {
        return {};
    }
    const auto now = Clock::now();
    if (deadline > now) {
        m_impl->LogLongDuration(deadline - now, "TimerService::Schedule");
    }
    const auto state = std::make_shared<Timer::State>();
    state->m_owner = m_impl;
    state->m_callback = std::move(callback);
    state->m_options.repeat = TimerRepeatMode::kOnce;
    state->m_next_deadline = deadline;
    m_impl->Add(state, deadline);
    return Timer(state);
}

Timer TimerService::ScheduleAfter(std::chrono::steady_clock::duration delay,
                                   std::function<void()> callback) {
    return Schedule(saturating_add(Clock::now(), nonnegative(delay)),
                    std::move(callback));
}

Timer TimerService::ScheduleEvery(std::chrono::steady_clock::duration period,
                                   std::function<void()> callback,
                                   TimerOptions options) {
    if (!callback || !positive(period)) {
        return {};
    }
    options.repeat = options.repeat == TimerRepeatMode::kOnce
                         ? TimerRepeatMode::kFixedDelay
                         : options.repeat;
    options.period = period;
    const auto repeat_callback =
        [callback = std::move(callback)](const TimerContext&) {
            callback();
            return TimerDecision::Continue();
        };
    return ScheduleRepeat(period, repeat_callback, options);
}

Timer TimerService::ScheduleRepeat(std::chrono::steady_clock::duration initial_delay,
                                   TimerRepeatCallback callback,
                                   TimerOptions options) {
    if (!callback) {
        return {};
    }
    if (options.repeat == TimerRepeatMode::kOnce) {
        options.repeat = TimerRepeatMode::kFixedDelay;
    }
    if (!positive(options.period)) {
        options.period = initial_delay;
    }
    if (!positive(options.period)) {
        return {};
    }
    m_impl->LogLongDuration(std::max(nonnegative(initial_delay), options.period),
                            "TimerService::ScheduleRepeat");
    const auto state = std::make_shared<Timer::State>();
    state->m_owner = m_impl;
    state->m_repeat_callback = std::move(callback);
    state->m_options = options;
    state->m_next_deadline = saturating_add(Clock::now(),
                                            nonnegative(initial_delay));
    m_impl->Add(state, state->m_next_deadline);
    return Timer(state);
}

Timer TimerService::ScheduleOn(Clock::time_point deadline,
                                std::function<void()> callback,
                                TimerExecutorPtr executor) {
    if (!callback || !executor) {
        return {};
    }
    const auto now = Clock::now();
    if (deadline > now) {
        m_impl->LogLongDuration(deadline - now, "TimerService::ScheduleOn");
    }
    const auto state = std::make_shared<Timer::State>();
    state->m_owner = m_impl;
    state->m_callback = std::move(callback);
    state->m_executor = std::move(executor);
    state->m_options.repeat = TimerRepeatMode::kOnce;
    state->m_next_deadline = deadline;
    m_impl->Add(state, deadline);
    return Timer(state);
}

Timer TimerService::ScheduleEveryOn(
    std::chrono::steady_clock::duration period,
    std::function<void()> callback,
    TimerExecutorPtr executor,
    TimerOptions options) {
    if (!callback || !executor || !positive(period)) {
        return {};
    }
    options.repeat = options.repeat == TimerRepeatMode::kOnce
                         ? TimerRepeatMode::kFixedDelay
                         : options.repeat;
    options.period = period;
    const auto repeat_callback =
        [callback = std::move(callback), executor = std::move(executor)](
            const TimerContext&) {
            if (!executor->Submit(callback)) {
                throw std::runtime_error("timer executor rejected callback");
            }
            return TimerDecision::Continue();
        };
    return ScheduleRepeat(period, repeat_callback, options);
}

Timer TimerService::ScheduleWeakAfter(
    std::chrono::steady_clock::duration delay, std::weak_ptr<void> owner,
    std::function<void(std::shared_ptr<void>)> callback) {
    if (!callback) {
        return {};
    }
    return ScheduleAfter(delay, [owner = std::move(owner),
                                 callback = std::move(callback)] {
        if (auto locked = owner.lock()) {
            callback(std::move(locked));
        }
    });
}

Timer TimerService::ScheduleWeakEvery(
    std::chrono::steady_clock::duration period, std::weak_ptr<void> owner,
    std::function<void(std::shared_ptr<void>)> callback, TimerOptions options) {
    if (!callback || !positive(period)) {
        return {};
    }
    options.repeat = options.repeat == TimerRepeatMode::kOnce
                         ? TimerRepeatMode::kFixedDelay
                         : options.repeat;
    options.period = period;
    auto repeat = [owner = std::move(owner), callback = std::move(callback)](
                      const TimerContext&) {
        if (auto locked = owner.lock()) {
            callback(std::move(locked));
            return TimerDecision::Continue();
        }
        return TimerDecision::Stop();
    };
    return ScheduleRepeat(period, std::move(repeat), options);
}

void TimerService::SetExceptionHandler(TimerExceptionHandler handler) {
    if (m_impl) {
        m_impl->SetExceptionHandler(std::move(handler));
    }
}

bool TimerService::SetDurationLimits(TimerDurationLimits limits) noexcept {
    return m_impl && m_impl->SetDurationLimits(limits);
}

TimerDurationLimits TimerService::DurationLimits() const noexcept {
    return m_impl ? m_impl->DurationLimits() : TimerDurationLimits{};
}

bool TimerService::IsRunning() const noexcept {
    return m_impl && m_impl->Running();
}

std::size_t TimerService::PendingCount() const noexcept {
    return m_impl ? m_impl->PendingCount() : 0;
}

std::optional<TimerService::Clock::time_point>
TimerService::NextDeadline() const noexcept {
    return m_impl ? m_impl->NextDeadline() : std::nullopt;
}

}  // go2cpp::core 命名空间
