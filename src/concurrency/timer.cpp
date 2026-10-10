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

std::chrono::steady_clock::duration nonnegative(
    std::chrono::steady_clock::duration value) noexcept {
    return value < std::chrono::steady_clock::duration::zero()
               ? std::chrono::steady_clock::duration::zero()
               : value;
}

bool positive(std::chrono::steady_clock::duration value) noexcept {
    return value > std::chrono::steady_clock::duration::zero();
}

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
