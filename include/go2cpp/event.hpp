#pragma once

#include "go2cpp/channel.hpp"
#include "go2cpp/sync.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp {

// EventBatch 是 Select 的新手友好包装。它只管理 channel SelectCase；任意
// 可能阻塞的业务回调仍应通过 go()/Scheduler::spawn() 启动，再用 channel
// 报告结果。这样等待会经过 ParkingCondition，Fiber 不会占住 M。
// 对象必须一直存活到所有 work/bind/add_event 调用返回；stop() 只改变状态，
// 不会强制销毁仍在等待的 Fiber 栈。
class EventBatch final {
public:
    using Duration = ContextDuration;
    using Handler = std::function<void(const SelectResult&)>;

    EventBatch() = default;
    EventBatch(const EventBatch&) = delete;
    EventBatch& operator=(const EventBatch&) = delete;

    // bind 会清空旧事件并装载一个新事件；add_event/load 用于追加事件。
    void bind(SelectCase event, Handler handler = {}) {
        clear();
        add_event(std::move(event), std::move(handler));
    }

    void add_event(SelectCase event, Handler handler = {}) {
        if (!m_mutex.Lock()) {
            return;
        }
        m_entries.push_back(Entry{std::move(event), std::move(handler)});
        m_mutex.Unlock();
    }

    void load(SelectCase event, Handler handler = {}) {
        add_event(std::move(event), std::move(handler));
    }

    void clear() {
        if (!m_mutex.Lock()) {
            return;
        }
        m_entries.clear();
        m_last = SelectResult{};
        m_has_result = false;
        m_stopped = false;
        m_rounds = 0;
        m_started_at.reset();
        m_mutex.Unlock();
    }

    // 设置单次 Select 的最长等待时间。超时后默认停止批次；可传 false
    // 继续下一轮，以便调用方自行决定降级策略。
    void make_loop_time(Duration timeout, bool stop_on_timeout = true) {
        if (!m_mutex.Lock()) {
            return;
        }
        m_loop_timeout = timeout;
        m_stop_on_timeout = stop_on_timeout;
        m_mutex.Unlock();
    }

    // 设置最多运行轮数和总工作时间；0 表示不限制对应条件。
    void make_stop_config(std::size_t max_rounds, Duration max_duration) {
        if (!m_mutex.Lock()) {
            return;
        }
        m_max_rounds = max_rounds;
        m_max_duration = max_duration;
        m_started_at.reset();
        m_mutex.Unlock();
    }

    void make_stop_config(std::size_t max_rounds,
                          std::uint64_t max_seconds) {
        make_stop_config(max_rounds,
                         std::chrono::seconds(max_seconds));
    }

    void stop() noexcept {
        try {
            if (!m_mutex.Lock()) {
                return;
            }
            m_stopped = true;
            m_mutex.Unlock();
        } catch (...) {
            // shutdown 期间不能再阻塞调用方；未能加锁时不触碰共享状态。
        }
    }
    void reset() {
        if (!m_mutex.Lock()) {
            return;
        }
        m_stopped = false;
        m_rounds = 0;
        m_started_at.reset();
        m_has_result = false;
        m_last = SelectResult{};
        m_mutex.Unlock();
    }

    bool runnable() const {
        if (!m_mutex.Lock()) {
            return false;
        }
        const bool result = !m_stopped && !m_entries.empty();
        m_mutex.Unlock();
        return result;
    }
    // 兼容用户习惯的拼写。
    bool runable() const { return runnable(); }
    bool happened() const {
        if (!m_mutex.Lock()) {
            return false;
        }
        const bool result = m_has_result && m_last.selected;
        m_mutex.Unlock();
        return result;
    }
    bool happend() const { return happened(); }

    static SelectResult stopped_result(const char* message) {
        return SelectResult{SelectResult::kNoSelection, false, false, {},
                            ChannelStatus::kCancelled, NewError(message)};
    }

    SelectResult work(const ContextPtr& context = {},
                      std::optional<Duration> timeout = std::nullopt) {
        // 同一批次只允许一个 work 调用；sync::Mutex 会让竞争的 Fiber
        // 挂起而不是占住 M，普通线程仍可安全等待。
        if (!m_work_mutex.Lock()) {
            return stopped_result("事件批次无法获取工作锁");
        }
        std::vector<SelectCase> selected_cases;
        std::vector<Handler> selected_handlers;
        Handler handler;
        std::optional<Duration> effective_timeout;
        bool stop_on_timeout = true;
        {
            if (!m_mutex.Lock()) {
                m_work_mutex.Unlock();
                return stopped_result("事件批次无法获取状态锁");
            }
            if (!m_stopped && !m_entries.empty()) {
                if (!m_started_at.has_value()) {
                    m_started_at = std::chrono::steady_clock::now();
                }
                if (m_max_rounds != 0 && m_rounds >= m_max_rounds) {
                    m_stopped = true;
                } else if (m_max_duration > Duration::zero() &&
                           std::chrono::steady_clock::now() - *m_started_at >=
                               m_max_duration) {
                    m_stopped = true;
                }
            }
            if (!m_stopped && !m_entries.empty()) {
                selected_cases = cases();
                selected_handlers.reserve(m_entries.size());
                for (const auto& entry : m_entries) {
                    selected_handlers.push_back(entry.handler);
                }
                effective_timeout = timeout.has_value() ? timeout
                                                         : m_loop_timeout;
                stop_on_timeout = m_stop_on_timeout;
            } else {
                m_last = work_stopped();
                m_has_result = true;
                m_mutex.Unlock();
                m_work_mutex.Unlock();
                return m_last;
            }
            m_mutex.Unlock();
        }

        SelectResult result;
        try {
            result = Select(selected_cases, context, effective_timeout);
        } catch (const std::exception& exception) {
            result.index = SelectResult::kNoSelection;
            result.selected = false;
            result.status = ChannelStatus::kInvalid;
            result.error = NewError(std::string("Select 执行异常: ") +
                                    exception.what());
        } catch (...) {
            result.index = SelectResult::kNoSelection;
            result.selected = false;
            result.status = ChannelStatus::kInvalid;
            result.error = NewError("Select 执行发生未知异常");
        }

        {
            if (!m_mutex.Lock()) {
                m_work_mutex.Unlock();
                return stopped_result("事件批次无法更新状态");
            }
            m_last = result;
            m_has_result = true;
            ++m_rounds;
            if (result.status == ChannelStatus::kTimedOut &&
                stop_on_timeout) {
                m_stopped = true;
            }
            if (result.selected && result.index < selected_handlers.size()) {
                handler = selected_handlers[result.index];
            }
            result = m_last;
            m_mutex.Unlock();
        }
        m_work_mutex.Unlock();

        // 用户回调不在 EventBatch 锁内运行；异常转换成结果错误，不能
        // 穿过 scheduler worker 或 Fiber 边界。
        if (handler) {
            try {
                handler(result);
            } catch (const std::exception& exception) {
                result.status = ChannelStatus::kInvalid;
                result.error = NewError(std::string("事件处理回调异常: ") +
                                        exception.what());
                if (m_mutex.Lock()) {
                    m_last = result;
                    m_mutex.Unlock();
                }
            } catch (...) {
                result.status = ChannelStatus::kInvalid;
                result.error = NewError("事件处理回调发生未知异常");
                if (m_mutex.Lock()) {
                    m_last = result;
                    m_mutex.Unlock();
                }
            }
        }
        return result;
    }

    // 返回最近一次结果；没有结果时返回空值。调用方也可以直接使用 work()
    // 的返回值，避免额外保存状态。
    std::optional<SelectResult> handle() const {
        if (!m_mutex.Lock()) {
            return std::nullopt;
        }
        if (!m_has_result || !m_last.selected) {
            m_mutex.Unlock();
            return std::nullopt;
        }
        auto result = m_last;
        m_mutex.Unlock();
        return result;
    }

    // 返回快照，避免调用者在另一个 Fiber/thread 修改批次时持有内部引用。
    SelectResult last_result_copy() const {
        if (!m_mutex.Lock()) {
            return SelectResult{};
        }
        auto result = m_last;
        m_mutex.Unlock();
        return result;
    }

    // 返回独立快照；不把内部结果引用暴露给并发调用者。
    SelectResult last_result() const { return last_result_copy(); }
    std::size_t size() const {
        if (!m_mutex.Lock()) {
            return 0;
        }
        const auto result = m_entries.size();
        m_mutex.Unlock();
        return result;
    }

private:
    struct Entry {
        SelectCase event;
        Handler handler;
    };

    std::vector<SelectCase> cases() const {
        std::vector<SelectCase> result;
        result.reserve(m_entries.size());
        for (const auto& entry : m_entries) {
            result.push_back(entry.event);
        }
        return result;
    }

    SelectResult work_stopped() {
        m_last = SelectResult{SelectResult::kNoSelection, false, false, {},
                              ChannelStatus::kCancelled,
                              NewError("事件批次已停止")};
        m_has_result = true;
        return m_last;
    }

    mutable sync::Mutex m_mutex;
    sync::Mutex m_work_mutex;
    std::vector<Entry> m_entries;
    std::optional<Duration> m_loop_timeout;
    std::optional<Duration> m_max_duration;
    std::optional<std::chrono::steady_clock::time_point> m_started_at;
    SelectResult m_last;
    std::size_t m_rounds{0};
    std::size_t m_max_rounds{0};
    bool m_stop_on_timeout{true};
    bool m_stopped{false};
    bool m_has_result{false};
};

// SelectLoop 是更直观的别名，保留 EventBatch 的所有显式生命周期规则。
using SelectLoop = EventBatch;

}  // namespace go2cpp
