/**
 * @file event.hpp
 * @brief `EventBatch`（别名 `SelectLoop`）：把一组 `SelectCase` 与回调封装成
 *        可复用的事件循环。
 *
 * ## 模块定位
 *
 * `tools` 层的事件循环包装，建立在 `channel::Select` 与 `sync::Mutex` 之上。
 * 上游是「同时等多个 channel / 定时器 / 取消信号」的业务循环，下游是
 * `channel` 与 `sync`。它不拥有任何 Fiber，也不强制阻塞策略：等待经过
 * `ParkingCondition`，Fiber 不会占住 M。
 *
 * ## 本文件定义什么
 *
 * - `EventBatch`：事件集合、每轮 `work()`、停止/超时/轮数限制；
 * - `SelectLoop`：同一类型的更直观别名；
 * - 内部的 `StopControl`（Context + cancel）与 `Entry`（case + handler）。
 *
 * ## 阅读建议
 *
 * 先读 `work()` 的 6 个步骤与状态机文档 `docs/state-machines/EventBatch.md`，
 * 再看 `stop()`/`reset()`/`clear()` 三个生命周期函数，最后看各观测接口。
 *
 * ## 边界与陷阱
 *
 * **`m_mutex.Lock()` 返回 false 时，`add_event`/`clear`/`make_loop_time` 等
 * 写方法会静默 `return`，不会报错、也不会入队。** 因此不要用「方法返回
 * void」推断事件已入队；需要确认时用 `size()` 复查。锁失败通常发生在
 * 调度器关闭或 Fiber 被强制终止的窗口。
 */
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
 *
 * ## 定位
 * `tools` 的事件循环层：供需要「同时等多个 channel / 定时器 / 取消信号」的
 * 业务循环使用（例如服务主循环、协议多路复用）。它把 `SelectCase` 集合与
 * 每轮结果统一管理，避免调用方自己维护 case 数组与停止标志。
 *
 * ## 生命周期与线程模型
 * 构造后可 `bind`/`add_event` 装载事件，`work()` 每轮执行一次 Select，
 * `stop()`/`reset()` 控制生命周期。所有公开方法用 `sync::Mutex` 保护共享
 * 状态：Fiber 竞争时挂起而不是占住 M，普通线程也可安全等待。`work()` 由
 * `m_work_mutex` 串行化，同一批只允许一个执行者。用户 Handler 在锁外运行。
 *
 * ## 状态机
 * `Unbound -> Runnable -> Waiting -> Runnable/Stopped` 等转化见
 * `docs/state-machines/EventBatch.md`。
 *
 * ## 不变量
 * - `m_entries` 与 `m_last.index` 的一致性只在持 `m_mutex` 时成立；
 * - `m_rounds` 只在成功执行一轮 `work()` 后递增；
 * - `m_stopped` 为真后 `work()` 不再调用 `Select`，直接返回停止结果；
 * - `m_has_result` 为真时 `m_last` 是最近一轮结果的副本；
 * - `m_work_mutex` 保证同一时刻最多一个 `work()` 在跑。
 */
class EventBatch final {
    struct StopControl final {
        ContextPtr context; ///< 批次的停止 Context；stop() 通过 cancel 唤醒 Select
        CancelFunc cancel;  ///< 取消函数；stop() 调用，触发 DoneSignal
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
     *
     * ## 步骤
     * step1. 获取 `m_work_mutex`，保证同一批只有一个 `work()`；
     * step2. 持 `m_mutex` 检查停止/空事件，以及轮数与总时长上限；已停止则
     *        直接返回 `stopped_result`；
     * step3. 复制 case 与 handler 快照，计算本轮 timeout（显式 timeout >
     *        loop timeout > 批次剩余时间）；
     * step4. 在锁外调用 `Select`（可能挂起 Fiber）；异常转成 kInvalid 结果；
     * step5. 回到锁内更新 `m_last`/`m_has_result`/`m_rounds`，超时且
     *        `stop_on_timeout` 时置停止；
     * step6. 在锁外执行选中 case 的 Handler；回调异常只写回 `m_last`。
     */
    SelectResult work(const ContextPtr& context = {},
                      std::optional<Duration> timeout = std::nullopt) {
        // step1: 同一批次只允许一个 work；竞争 Fiber 会挂起而不是占住 M
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
            // step2: 检查停止条件与轮数/总时长上限
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
            // step3: 复制快照并计算本轮超时
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
            // step4: 锁外执行 Select；这里才可能挂起 Fiber
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
            // step5: 回写最近结果与轮数
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

        // step6: 用户回调在锁外运行；异常只更新 m_last，不穿过边界
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
        SelectCase event; ///< 一个等待中的 case；bind/add_event 写入
        Handler handler;  ///< 该 case 被选中时的回调，可为空
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

    mutable sync::Mutex m_mutex;          ///< 保护全部状态字段；所有公开方法先 Lock()，失败即静默返回
    sync::Mutex m_work_mutex;             ///< 串行化 work()；同一批只允许一个执行者
    std::shared_ptr<StopControl> m_stop_control; ///< 当前停止 Context；stop()/clear()/reset() 替换
    std::vector<Entry> m_entries;         ///< 已绑定事件；bind/add_event 追加，clear 清空
    std::optional<Duration> m_loop_timeout; ///< 每轮默认超时；make_loop_time() 设置
    std::optional<Duration> m_max_duration; ///< 批次总时长上限；make_stop_config() 设置
    std::optional<std::chrono::steady_clock::time_point> m_started_at; ///< 首轮开始时间；首次 work() 设置
    SelectResult m_last;                  ///< 最近一轮结果副本；work() 写，handle()/last_result_copy() 读
    std::size_t m_rounds{0};              ///< 已执行轮数；每轮 work() 递增，reset() 清零
    std::size_t m_max_rounds{0};          ///< 总轮数上限；0 表示不限
    bool m_stop_on_timeout{true};         ///< 本轮超时是否停止批次；make_loop_time() 设置
    bool m_stopped{false};                ///< 停止标志；stop()/超时/轮数上限置真，reset()/clear() 复位
    bool m_has_result{false};             ///< 是否已有最近结果；handle() 据此判断
};

// SelectLoop 是更直观的别名，保留 EventBatch 的所有显式生命周期规则。
using SelectLoop = EventBatch;

}  // namespace go2cpp
