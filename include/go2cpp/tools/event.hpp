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
// 对象必须一直存活到所有 work/bind/add_event 调用返回；stop() 会唤醒等待，
// 但只走协作式返回路径，不会强制销毁仍在等待的 Fiber 栈。
// Handler 接收 const SelectResult&，并且内部保存的是并发安全快照；因此
// move-only 结果只能通过 SelectValue::Get<T>() 观察，不能在 Handler 中 Take.
/**
 * @brief 可复用的事件批处理器。
 * @details 将多个 SelectCase 与回调绑定为一轮一轮的等待任务，底层依赖
 *          Channel、Select 和 sync::Mutex，支持 Fiber 与普通线程调用。
 *          stop()/reset() 控制生命周期，超时和最大轮数用于限制长时间运行。
 */
class EventBatch final {
    struct StopControl final {
        ContextPtr context;
        CancelFunc cancel;
    };

    static std::shared_ptr<StopControl> make_stop_control() {
        auto control = std::make_shared<StopControl>();
        auto pair = Context::WithCancel(Context::Background());
        control->context = std::move(pair.first);
        control->cancel = std::move(pair.second);
        return control;
    }

public:
    using Duration = ContextDuration;
    /// 事件触发后的用户回调；参数包含被选中的 case 和结果值。
    using Handler = std::function<void(const SelectResult&)>;

    /** @brief 创建空事件批次并初始化内部停止 Context。 */
    EventBatch() : m_stop_control(make_stop_control()) {}
    EventBatch(const EventBatch&) = delete;
    EventBatch& operator=(const EventBatch&) = delete;

    // bind 会清空旧事件并装载一个新事件；add_event/load 用于追加事件。
    /** @brief 清空现有 case 后绑定一个初始事件。 */
    void bind(SelectCase event, Handler handler = {}) {
        clear();
        add_event(std::move(event), std::move(handler));
    }

    /** @brief 向批次尾部追加一个事件及其可选回调。 */
    void add_event(SelectCase event, Handler handler = {}) {
        if (!m_mutex.Lock()) {
            return;
        }
        try {
            m_entries.push_back(Entry{std::move(event), std::move(handler)});
        } catch (...) {
            m_mutex.Unlock();
            throw;
        }
        m_mutex.Unlock();
    }

    /** @brief 兼容 Go 风格命名的 add_event 别名。 */
    void load(SelectCase event, Handler handler = {}) {
        add_event(std::move(event), std::move(handler));
    }

    /** @brief 清空事件、结果和停止状态，准备重新绑定。 */
    void clear() {
        auto fresh_control = make_stop_control();
        if (!m_mutex.Lock()) {
            return;
        }
        m_entries.clear();
        m_last = SelectResult{};
        m_has_result = false;
        m_stopped = false;
        m_rounds = 0;
        m_started_at.reset();
        m_stop_control = std::move(fresh_control);
        m_mutex.Unlock();
    }

    // 设置单次 Select 的最长等待时间。超时后默认停止批次；可传 false
    // 继续下一轮，以便调用方自行决定降级策略。
    /** @brief 设置每轮 Select 的最长等待时间及超时后是否停止。 */
    void make_loop_time(Duration timeout, bool stop_on_timeout = true) {
        if (!m_mutex.Lock()) {
            return;
        }
        m_loop_timeout = timeout;
        m_stop_on_timeout = stop_on_timeout;
        m_mutex.Unlock();
    }

    // 设置最多运行轮数和总工作时间；0 表示不限制对应条件。
    /** @brief 设置批次总轮数和总运行时长上限，0/非正值表示不限制。 */
    void make_stop_config(std::size_t max_rounds, Duration max_duration) {
        if (!m_mutex.Lock()) {
            return;
        }
        m_max_rounds = max_rounds;
        m_max_duration = max_duration;
        m_started_at.reset();
        m_mutex.Unlock();
    }

    /** @brief 以秒为单位设置批次总运行限制。 */
    void make_stop_config(std::size_t max_rounds,
                          std::uint64_t max_seconds) {
        make_stop_config(max_rounds,
                         std::chrono::seconds(max_seconds));
    }

    /** @brief 请求停止并唤醒当前等待中的 Fiber 或线程。 */
    void stop() noexcept {
        try {
            std::shared_ptr<StopControl> control;
            if (!m_mutex.Lock()) {
                return;
            }
            m_stopped = true;
            control = m_stop_control;
            m_mutex.Unlock();
            // 取消 DoneSignal 会唤醒正在 Select 的 Fiber；它仍通过正常
            // 返回路径退出，不会从这里强制销毁挂起栈。
            if (control && control->cancel) {
                control->cancel();
            }
        } catch (...) {
            // shutdown 期间不能再阻塞调用方；未能加锁时不触碰共享状态。
        }
    }
    /** @brief 清除停止、结果和轮数，重新激活事件批次。 */
    void reset() {
        auto fresh_control = make_stop_control();
        if (!m_mutex.Lock()) {
            return;
        }
        m_stopped = false;
        m_rounds = 0;
        m_started_at.reset();
        m_has_result = false;
        m_last = SelectResult{};
        m_stop_control = std::move(fresh_control);
        m_mutex.Unlock();
    }

    /** @brief 返回批次是否仍有事件且允许执行。 */
    bool runnable() const {
        if (!m_mutex.Lock()) {
            return false;
        }
        const bool result = !m_stopped && !m_entries.empty();
        m_mutex.Unlock();
        return result;
    }
    // 兼容用户习惯的拼写。
    /** @brief runnable() 的兼容拼写别名。 */
    bool runable() const { return runnable(); }
    /** @brief 返回最近一轮是否选中了事件。 */
    bool happened() const {
        if (!m_mutex.Lock()) {
            return false;
        }
        const bool result = m_has_result && m_last.selected;
        m_mutex.Unlock();
        return result;
    }
    /** @brief happened() 的兼容拼写别名。 */
    bool happend() const { return happened(); }

    /** @brief 构造一个表示批次停止的结果对象。 */
    static SelectResult stopped_result(const char* message) {
        return SelectResult{SelectResult::kNoSelection, false, false, {},
                            ChannelStatus::kCancelled, NewError(message)};
    }

    /**
     * @brief 执行一轮 Select 并保存结果。
     * @param context 外部取消 Context，可为空。
     * @param timeout 本轮覆盖默认 loop timeout 的时长。
     * @return 本轮选择结果；停止、取消和错误通过 status/error 返回。
     */
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
        ContextPtr stop_context;
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
                // max_duration 是整个批次的硬上限，即使没有设置 loop_time
                // 也必须把剩余时间传给 Select，不能无限等待。
                if (m_max_duration > Duration::zero()) {
                    const auto elapsed =
                        std::chrono::steady_clock::now() - *m_started_at;
                    const auto remaining = *m_max_duration - elapsed;
                    if (remaining <= Duration::zero()) {
                        effective_timeout = Duration::zero();
                    } else if (!effective_timeout.has_value() ||
                               remaining < *effective_timeout) {
                        effective_timeout = remaining;
                    }
                }
                stop_on_timeout = m_stop_on_timeout;
                stop_context = m_stop_control ? m_stop_control->context :
                                                   Context::Background();
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
        DoneSignal::CallbackId stop_bridge_id = 0;
        CancelFunc stop_bridge_cancel;
        ContextPtr effective_context = stop_context;
        try {
            if (context) {
                auto child = Context::WithCancel(context);
                effective_context = std::move(child.first);
                stop_bridge_cancel = std::move(child.second);
                if (stop_context) {
                    stop_bridge_id = stop_context->Done().AddCallback(
                        [stop_bridge_cancel] { stop_bridge_cancel(); });
                }
            }
            result = Select(selected_cases, effective_context,
                            effective_timeout);
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
        if (stop_context && stop_bridge_id != 0) {
            stop_context->Done().RemoveCallback(stop_bridge_id);
        }
        if (stop_bridge_cancel) {
            stop_bridge_cancel();
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
    /** @brief 读取最近一轮结果，没有成功选择时返回空值。 */
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
    /** @brief 返回最近一轮结果的线程安全副本。 */
    SelectResult last_result_copy() const {
        if (!m_mutex.Lock()) {
            return SelectResult{};
        }
        auto result = m_last;
        m_mutex.Unlock();
        return result;
    }

    // 返回独立快照；不把内部结果引用暴露给并发调用者。
    /** @brief last_result_copy() 的简写接口。 */
    SelectResult last_result() const { return last_result_copy(); }
    /** @brief 返回当前批次已绑定的事件数量。 */
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
    std::shared_ptr<StopControl> m_stop_control;
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
