#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace go2cpp::core {

/**
 * @brief 定时器生命周期状态。
 * @details Pending 表示已注册，Running 表示回调或执行器已经领取，
 *          Completed/Cancelled/Failed 表示终态。取消只保证阻止尚未领取
 *          的回调；已经进入 Running 的回调需要业务自行协作停止。
 */
enum class TimerState : std::uint8_t {
    kInvalid,
    kPending,
    kRunning,
    kCompleted,
    kCancelled,
    kFailed,
};

/** @brief 定时器线程等待策略。 */
enum class TimerWaitMode : std::uint8_t {
    /** 使用条件变量休眠，适合大多数低 CPU 场景。 */
    kConditionVariable,
    /** 在截止时间附近短暂自旋，适合极低延迟且允许消耗一个 CPU 的场景。 */
    kBusyWait,
};

/** @brief 周期定时器的默认调度方式。 */
enum class TimerRepeatMode : std::uint8_t {
    /** 只执行一次。 */
    kOnce,
    /** 本次回调结束后再等待 period，慢回调不会堆积。 */
    kFixedDelay,
    /** 按计划时间推进，默认跳过已经错过的周期，避免追赶造成忙循环。 */
    kFixedRate,
};

/**
 * @brief 周期定时器行为参数。
 * @details period 必须为正；max_runs 为 0 表示不限制次数。busy wait 只在
 *          截止时间前 spin_window 内生效，避免长期占满 CPU。结构体可复制，
 *          创建后不再修改，避免热路径读取数据竞争。
 */
struct TimerOptions {
    TimerRepeatMode repeat{TimerRepeatMode::kOnce};
    std::chrono::steady_clock::duration period{};
    TimerWaitMode wait_mode{TimerWaitMode::kConditionVariable};
    std::chrono::steady_clock::duration spin_window{
        std::chrono::microseconds(50)};
    std::size_t max_runs{0};
    bool skip_missed{true};
};

/**
 * @brief 定时任务时长告警阈值。
 * @details 达到 warn_after 时记录 WARN，达到 error_after 时记录 ERROR，
 *          但两者都不拒绝任务；一年及更长任务仍可正常注册。阈值可通过
 *          [timer] 配置动态调整，0 表示关闭对应级别的告警。
 */
struct TimerDurationLimits {
    std::chrono::steady_clock::duration warn_after{
        std::chrono::hours(24 * 30)};
    std::chrono::steady_clock::duration error_after{
        std::chrono::hours(24 * 365)};
};

/**
 * @brief 本次定时回调的只读上下文。
 * @details run_count 从 1 开始；scheduled_deadline 是本次计划时间，
 *          started_at 是实际领取时间。回调不应保存该对象的引用。
 */
struct TimerContext {
    std::size_t run_count{0};
    std::chrono::steady_clock::time_point scheduled_deadline{};
    std::chrono::steady_clock::time_point started_at{};
};

/**
 * @brief 周期回调决定下一次行为。
 * @details Continue() 使用 TimerOptions 的周期策略；ContinueAfter/At 可
 *          动态修改下一次等待时间；Stop 结束任务。动态回调只能影响当前
 *          Timer，不会改变其他任务。
 */
class TimerDecision final {
public:
    enum class Kind : std::uint8_t { kStop, kContinue, kAfter, kAt };

    static TimerDecision Stop() noexcept {
        return TimerDecision(Kind::kStop);
    }
    static TimerDecision Continue() noexcept {
        return TimerDecision(Kind::kContinue);
    }
    static TimerDecision ContinueAfter(
        std::chrono::steady_clock::duration delay) noexcept {
        TimerDecision result(Kind::kAfter);
        result.m_delay = delay;
        return result;
    }
    static TimerDecision ContinueAt(
        std::chrono::steady_clock::time_point deadline) noexcept {
        TimerDecision result(Kind::kAt);
        result.m_deadline = deadline;
        return result;
    }

    Kind kind() const noexcept { return m_kind; }
    std::chrono::steady_clock::duration delay() const noexcept {
        return m_delay;
    }
    std::chrono::steady_clock::time_point deadline() const noexcept {
        return m_deadline;
    }

private:
    explicit TimerDecision(Kind kind) noexcept : m_kind(kind) {}

    Kind m_kind;
    std::chrono::steady_clock::duration m_delay{};
    std::chrono::steady_clock::time_point m_deadline{};
};

using TimerRepeatCallback =
    std::function<TimerDecision(const TimerContext&)>;
using TimerExceptionHandler = std::function<void(std::exception_ptr)>;

/**
 * @brief 定时器到期后的执行后端。
 * @details TimerService 只负责截止时间、取消和周期状态，不依赖 Scheduler。
 *          Native 后端可直接在线程中执行，Scheduler 适配器则把回调提交为
 *          Fiber。Submit 必须快速返回，返回 false 表示后端停止接收任务。
 */
class TimerExecutor {
public:
    virtual ~TimerExecutor() = default;

    /**
     * @brief 提交一个已经到期的回调。
     * @param callback 待执行任务；执行器取得该可调用对象的所有权。
     * @return true 表示已接收，false 表示后端已停止或拒绝。
     */
    virtual bool Submit(std::function<void()> callback) = 0;
};
using TimerExecutorPtr = std::shared_ptr<TimerExecutor>;

class TimerService;

/**
 * @brief 可取消、可查询状态的定时器句柄。
 * @details 句柄是轻量 shared state，不拥有 TimerService 线程。析构等价于
 *          Cancel；多个句柄不能复制，移动后源句柄失效。回调已经进入
 *          Running 后，Cancel 不会强行打断 C++ 调用栈。
 */
class Timer final {
public:
    struct State;

    Timer() noexcept;
    ~Timer();
    Timer(Timer&&) noexcept;
    Timer& operator=(Timer&&) noexcept;
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    /** @brief 取消尚未领取的回调；重复调用安全。 */
    void Cancel() noexcept;
    /** @brief 返回是否仍可能产生下一次回调。 */
    bool Active() const noexcept;
    /** @brief 返回当前生命周期状态。 */
    TimerState StateValue() const noexcept;
    /** @brief 返回已领取的回调次数。 */
    std::size_t RunCount() const noexcept;
    /** @brief 将尚未领取的一次任务改到绝对时间。 */
    bool Reset(std::chrono::steady_clock::time_point deadline) noexcept;
    /** @brief 按原周期从当前时刻重新开始。 */
    bool Refresh() noexcept;

    bool active() const noexcept { return Active(); }
    TimerState state() const noexcept { return StateValue(); }
    std::size_t run_count() const noexcept { return RunCount(); }
    bool reset(std::chrono::steady_clock::time_point deadline) noexcept {
        return Reset(deadline);
    }
    bool refresh() noexcept { return Refresh(); }

private:
    explicit Timer(std::shared_ptr<State> state);
    std::shared_ptr<State> m_state;
    friend class TimerService;
};

/**
 * @brief 独立的单调时钟定时任务服务。
 * @details 内部只有一个轻量 timer worker，队列按最早截止时间排序；回调在
 *          服务锁外执行，因此慢回调不会阻塞注册/取消，但会占用该服务线程。
 *          需要并行或 Fiber 执行时使用 TimerExecutor。服务不继承 Scheduler，
 *          Scheduler 也不拥有服务，双方通过执行器接口解耦。
 */
class TimerService final {
public:
    using Clock = std::chrono::steady_clock;
    class Impl;

    /**
     * @brief 创建并启动定时器服务。
     * @param exception_handler 回调异常处理器；为空时异常被隔离并丢弃。
     */
    explicit TimerService(TimerExceptionHandler exception_handler = {},
                          TimerDurationLimits duration_limits = {});
    ~TimerService();

    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    /** @brief 返回进程级默认服务；默认服务只在进程结束时回收。 */
    static TimerService& Default();

    /**
     * @brief 注册一次性绝对截止时间任务。
     * @param deadline 基于 steady_clock 的截止时间；过去时间会尽快执行。
     * @param callback 到期后执行的线程回调；空回调返回无效句柄。
     * @return 可取消、可查询状态的句柄。
     */
    Timer Schedule(Clock::time_point deadline,
                   std::function<void()> callback);

    /**
     * @brief 注册相对延时的一次性任务。
     * @param delay 相对当前时刻的延时；负值按零处理。
     * @param callback 到期后执行的线程回调。
     * @return 定时器句柄；服务已停止时抛出异常。
     */
    Timer ScheduleAfter(std::chrono::steady_clock::duration delay,
                        std::function<void()> callback);

    /**
     * @brief 注册固定延迟或固定速率周期任务。
     * @param period 周期，必须为正数。
     * @param callback 每次到期执行的回调。
     * @param options 周期模式、最大次数、等待策略和错过周期策略。
     * @return 周期句柄；无效参数返回无效句柄。
     */
    Timer ScheduleEvery(std::chrono::steady_clock::duration period,
                        std::function<void()> callback,
                        TimerOptions options = {});

    /**
     * @brief 注册可动态决定下一次截止时间的周期任务。
     * @param initial_delay 首次触发的相对延时。
     * @param callback 接收本次上下文并返回 Stop/Continue/新时间。
     * @param options 默认周期模式、最大次数和等待策略。
     * @return 周期句柄；空回调或无有效周期返回无效句柄。
     */
    Timer ScheduleRepeat(std::chrono::steady_clock::duration initial_delay,
                         TimerRepeatCallback callback,
                         TimerOptions options = {});

    /**
     * @brief 注册到期后提交给外部执行器的一次性任务。
     * @param deadline 绝对截止时间。
     * @param callback 待提交的回调。
     * @param executor 执行后端；Submit 返回 false 会使任务失败。
     * @return 定时器句柄。
     */
    Timer ScheduleOn(Clock::time_point deadline,
                     std::function<void()> callback,
                     TimerExecutorPtr executor);

    /**
     * @brief 注册每次到期都提交给外部执行器的周期任务。
     * @param period 周期，必须为正数。
     * @param callback 每次提交的回调。
     * @param executor 执行后端；不得在 Submit 内恢复别的线程的 Fiber 栈。
     * @param options 周期和等待策略。
     * @return 定时器句柄；参数无效时返回无效句柄。
     */
    Timer ScheduleEveryOn(std::chrono::steady_clock::duration period,
                          std::function<void()> callback,
                          TimerExecutorPtr executor,
                          TimerOptions options = {});

    /**
     * @brief 注册一次性弱引用任务。
     * @param delay 首次触发延时。
     * @param owner 被观察对象的弱引用。
     * @param callback owner 仍存活时接收锁定后的 shared_ptr。
     * @return 定时器句柄；owner 已消失时到期后自动完成且不调用回调。
     */
    Timer ScheduleWeakAfter(std::chrono::steady_clock::duration delay,
                            std::weak_ptr<void> owner,
                            std::function<void(std::shared_ptr<void>)> callback);

    /**
     * @brief 注册弱引用周期任务。
     * @param period 周期，必须为正数。
     * @param owner 被观察对象的弱引用。
     * @param callback owner 存活时执行；对象消失后返回 Stop。
     * @param options 周期模式和最大次数。
     * @return 定时器句柄。
     */
    Timer ScheduleWeakEvery(
        std::chrono::steady_clock::duration period,
        std::weak_ptr<void> owner,
        std::function<void(std::shared_ptr<void>)> callback,
        TimerOptions options = {});

    /**
     * @brief 替换异常观察器。
     * @param handler 回调异常处理器；为空表示隔离异常但不回调用户。
     * @note 观察器在定时器锁外执行，必须短小且不能依赖正在销毁的服务。
     */
    void SetExceptionHandler(TimerExceptionHandler handler);

    /**
     * @brief 动态替换长时长告警阈值。
     * @param limits WARN/ERROR 阈值；负值非法，0 表示关闭该级别。
     * @return 参数合法并已发布返回 true，否则保持旧值。
     */
    bool SetDurationLimits(TimerDurationLimits limits) noexcept;
    /** @brief 返回当前长时长告警阈值快照。 */
    TimerDurationLimits DurationLimits() const noexcept;

    /** @brief 返回服务是否仍接受新任务。 */
    bool IsRunning() const noexcept;
    /** @brief 返回队列中尚未领取的任务数；Running 任务不计入。 */
    std::size_t PendingCount() const noexcept;
    /** @brief 返回最早待处理截止时间；无待处理任务时返回空值。 */
    std::optional<Clock::time_point> NextDeadline() const noexcept;

private:
    std::shared_ptr<Impl> m_impl;
    friend class Timer;
};

}  // go2cpp::core 命名空间
