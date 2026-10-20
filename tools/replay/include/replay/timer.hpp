#pragma once

/**
 * @file timer.hpp
 * @brief TimerService 的录制 / 回放：真实定时器触发 → 记录 → 虚拟时间重放。
 *
 * ## 定位
 *
 * `ReplayClock` 只覆盖「少量、低频」的 `SleepFor` 定时点；本文件补的是
 * **TimerService 语义**：一次性 / 周期定时器的注册、触发、取消与重排。
 * 它不改动 `core::TimerService` 的调度逻辑，而是在录制端起一个与它语义
 * 一致的最小定时器线程（`std::thread + condition_variable + multimap`），
 * 把每次生命周期事件写进同一条 `kTimer` 记录流。
 *
 * ## 两种模式
 *
 * - **live**（`Recorder&` + `ReplayClock&`）：真实等待、真实执行回调；每个
 *   决定点写 `kTimer(kServiceArm/Fire/Cancel/Reschedule)`。录制端用一把
 *   互斥锁保证「已领取的回调」不会被并发取消撕裂记录：取消一个**正在执行**
 *   的定时器返回 false 且不写 cancel（要么 fire、要么 cancel，记录里不会
 *   出现 cancel 在 fire 前面的假顺序）。
 * - **replay**（`ReplaySource&`）：不启动线程、不读真实时钟。`Schedule*`
 *   消费记录中的 `kServiceArm` 并把回调登记到记录的 `timer_id` 上；
 *   `ReplayAll()` / 引擎派发 `kTimerFire` 时按记录顺序调用用户代码。
 *   取消与到期的竞态完全按记录决定（记录中 cancel 在 fire 前才抑制该次
 *   fire）。
 *
 * ## 与 select / context 的统一虚拟时钟
 *
 * 所有 `deadline_ns` / `fired_at_ns` 都是「相对录制起点」的虚拟纳秒，
 * 与 `ReplayClock`、`SyncSelect::AddTimeout`、`ReplayContext::SetDeadline`
 * 共用同一个 `Recorder::VirtualNowNs()` / `ReplaySource::VirtualNowNs()`。
 * 因此 select 的 timeout 与 timer 的 fire 可以放在同一条时间轴上比较。
 *
 * ## 限制（如实声明）
 *
 * - live 端是「固定延迟」周期语义：每次 fire 结束后再等 period，慢回调不会
 *   追赶；`core::TimerService` 的 kFixedRate 在这里等价于 kFixedDelay。
 * - replay 的回调在**引擎线程**执行；回调里可以安全地做立即完成的 channel
 *   操作（记录里的 blocked=0），但**不支持**在回调里 park（阻塞 channel /
 *   select）——那需要把回调放到独立 fiber 上，目前未实现。
 * - `ReplaySource::Events()` 由 replay 引擎提供；没有事件来源的自定义
 *   `ReplaySource` 只能使用引擎派发模式（`Dispatch()`），`ReplayAll()`
 *   会因为没有事件而什么都不做。
 */

#include "replay/clock.hpp"
#include "replay/format.hpp"
#include "replay/recorder.hpp"
#include "replay/replay_source.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace go2cpp::replay {

/**
 * @brief 定时器来源分类（数值与 `go2cpp::core::TimerSource` 一致）。
 *
 * 独立定义是为了让 `tools/replay` 的格式层/回放层不必链接 runtime；
 * 两边数值必须保持同步（有静态断言在 timer.cpp 里）。
 */
enum class TimerSource : std::uint8_t {
    kUnknown = 0,          ///< 未标注。
    kContext = 1,          ///< Context deadline / cancel。
    kParkingCondition = 2, ///< ParkingCondition 超时。
    kChannel = 3,          ///< select / channel timeout。
    kTimedMutex = 4,       ///< timed_mutex。
    kScheduler = 5,        ///< Scheduler::ScheduleOn。
    kSleep = 6,            ///< Sleep / After。
    kUser = 7,             ///< 用户业务定时器。
};

/// 来源枚举的稳定名字。
const char* TimerSourceName(TimerSource source) noexcept;

/// 回调签名：返回 0 表示正常完成；非 0 原样记录并在重放时校验一致。
using TimerCallback = std::function<std::int64_t()>;

/**
 * @brief 一条可比较的 timer 生命周期记录。
 * @details `seq` 只用于诊断排序，不参与 MATCH 比较（live 侧无法低成本拿到
 *          写入后的全局 seq）。
 */
struct TimerTraceEntry {
    std::uint32_t timer_id{0};   ///< 定时器标识（记录里分配，1 起）。
    TimerKind kind{TimerKind::kServiceArm}; ///< arm/fire/cancel/reschedule。
    std::uint64_t deadline_ns{0};///< 计划截止时间（绝对虚拟纳秒）。
    std::uint64_t fired_at_ns{0};///< 回调开始执行时的虚拟时间。
    std::int64_t callback_result{0}; ///< 回调返回值。
    std::uint32_t timer_source{0};   ///< `TimerSource` 数值。
    std::uint32_t source_gid{0};     ///< 发起注册/取消的逻辑 fiber id。
    std::uint64_t seq{0};            ///< 诊断用序号（不参与比较）。
};

/// 生命周期事件的名字（"arm"/"fire"/"cancel"/"reschedule"/"unknown"）。
const char* TimerTraceKindName(TimerKind kind) noexcept;

/**
 * @brief 定时器服务的录制 / 回放实现。
 *
 * 生命周期：对象必须比所有注册的回调活得久；live 模式析构会停止并 join
 * 后台线程。被 `ReplaySource` 安装后（replay 构造时自动），引擎在遍历事件
 * 流时会把 `kTimer` 事件派发进来。
 */
class ReplayTimerService final {
public:
    /**
     * @brief live 模式：启动真实定时器线程并写入 `recorder`。
     * @param recorder 录制器（必须活到本对象析构）。
     * @param clock 虚拟时钟（提供 `NowNs()`，必须活到本对象析构）。
     */
    ReplayTimerService(Recorder& recorder, ReplayClock& clock);

    /**
     * @brief replay 模式：只读记录，不启动线程。
     * @details 构造时自动 `source.InstallTimerService(this)`；析构时若仍是
     *          当前安装者则清除，避免引擎持有悬空指针。
     */
    explicit ReplayTimerService(ReplaySource& source);
    ~ReplayTimerService();

    ReplayTimerService(const ReplayTimerService&) = delete;
    ReplayTimerService& operator=(const ReplayTimerService&) = delete;

    /// 是否 live 模式。
    bool live() const noexcept { return m_recorder != nullptr; }

    /**
     * @brief 注册绝对截止时间（虚拟纳秒）的一次性定时器。
     * @return 定时器标识；失败返回 0（原因见 `error()`）。
     */
    std::uint32_t ScheduleAt(std::uint64_t deadline_ns, TimerCallback callback,
                             TimerSource source = TimerSource::kUser,
                             std::uint32_t source_gid = 0);

    /// 注册相对延时的一次性定时器（`NowNs() + delay_ns`）。
    std::uint32_t ScheduleAfter(std::uint64_t delay_ns, TimerCallback callback,
                                TimerSource source = TimerSource::kUser,
                                std::uint32_t source_gid = 0);

    /// 注册周期定时器（固定延迟；首次在 `NowNs() + period_ns` 触发）。
    std::uint32_t ScheduleEvery(std::uint64_t period_ns, TimerCallback callback,
                                TimerSource source = TimerSource::kUser,
                                std::uint32_t source_gid = 0);

    /**
     * @brief 取消一个尚未领取的定时器。
     * @details replay 模式只按记录判定：记录中存在该 timer 的 cancel 事件
     *          返回 true（控制流以记录为准），否则返回 false。
     *          live 模式成功取消时写 `kTimerCancel`。
     */
    bool Cancel(std::uint32_t timer_id);

    /**
     * @brief replay：把记录中剩余的 timer 事件全部跑完。
     * @details 按事件 `seq` 顺序触发 fire（同一虚拟时刻也严格按记录顺序），
     *          周期 timer 的每次 fire 都重新调用回调。返回执行的回调数。
     */
    std::size_t ReplayAll();

    /**
     * @brief replay：派发一条记录事件（供 replay 引擎的事件循环使用）。
     * @return 事件被消费返回 true；格式/一致性错误返回 false（见 error()）。
     */
    bool Dispatch(const Event& event);

    /// 已经发生的生命周期序列（live：实际；replay：重放）。
    const std::vector<TimerTraceEntry>& trace() const noexcept {
        return m_trace;
    }
    /// replay：从记录中提取的期望序列（live 模式为空）。
    const std::vector<TimerTraceEntry>& recorded_trace() const noexcept {
        return m_recorded_trace;
    }

    std::uint64_t armed_count() const noexcept { return m_armed; }
    std::uint64_t fired_count() const noexcept { return m_fired; }
    std::uint64_t cancelled_count() const noexcept { return m_cancelled; }
    std::uint64_t rescheduled_count() const noexcept { return m_rescheduled; }
    std::uint64_t registered_count() const noexcept {
        return static_cast<std::uint64_t>(m_callbacks.size());
    }

    /**
     * @brief 解除与 `ReplaySource` 的绑定（引擎在 Run 结束时自动调用）。
     * @details 析构不再回头访问 source：调用方可以在 source 销毁后再销毁
     *          本对象。解绑后 `ReplayAll()` 不可用。
     */
    void Detach();

    /// 最近一次错误（空表示无错误）。
    const std::string& error() const noexcept { return m_error; }

    /**
     * @brief 比较录制与实际重放的生命周期序列。
     *
     * 比较规则（arm 的 deadline 允许因注册时刻的虚拟时间不同而不同）：
     * - arm：比较 timer_id / kind / timer_source / source_gid；
     * - cancel：比较 timer_id / kind；
     * - reschedule：比较 timer_id / kind / deadline_ns；
     * - fire：比较 timer_id / kind / deadline_ns / fired_at_ns / callback_result。
     *
     * @return 一致返回 true；不一致时把第一处差异写入 `error`。
     */
    static bool CompareTraces(const std::vector<TimerTraceEntry>& recorded,
                              const std::vector<TimerTraceEntry>& replayed,
                              std::string* error);

private:
    struct LiveTimer {
        std::uint32_t timer_id{0};
        std::uint64_t deadline_ns{0}; ///< 虚拟绝对截止时间
        std::uint64_t period_ns{0};   ///< 0=一次性
        TimerCallback callback;
        std::uint32_t timer_source{0};
        std::uint32_t source_gid{0};
    };

    // ---- live ----
    std::uint32_t ArmLive(std::uint64_t deadline_ns, TimerCallback callback,
                          std::uint64_t period_ns, TimerSource source,
                          std::uint32_t source_gid);
    void RunLive();
    void FireLive(const LiveTimer& timer);
    std::uint64_t RealNowNs() const noexcept;

    // ---- replay ----
    void BuildReplayIndexes();
    bool DispatchFire(const Event& event);
    void RecordTrace(const TimerTraceEntry& entry);
    void SetError(std::string message);

    // 两种模式共用（replay 回调表 / live 也叫这个名字会很乱，分开命名）
    Recorder* const m_recorder{nullptr};
    ReplayClock* const m_clock{nullptr};
    ReplaySource* m_source{nullptr};

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;

    // live 状态
    std::map<std::pair<std::uint64_t, std::uint32_t>, LiveTimer> m_live_queue;
    std::map<std::uint32_t, LiveTimer> m_live_timers;
    std::vector<std::uint32_t> m_live_running;
    std::thread m_thread;
    bool m_stopping{false};

    // replay 状态
    std::vector<Event> m_events;         ///< replay：全部 timer 事件（按 seq）
    std::vector<const Event*> m_arms;    ///< replay：arm 事件指针（按 seq）
    std::size_t m_arm_cursor{0};
    std::size_t m_event_cursor{0};
    std::map<std::uint32_t, TimerCallback> m_callbacks;
    std::map<std::uint32_t, std::uint64_t> m_periods;
    std::map<std::uint32_t, bool> m_cancelled_by_record;

    std::vector<TimerTraceEntry> m_recorded_trace;
    std::vector<TimerTraceEntry> m_trace;

    std::uint64_t m_armed{0};
    std::uint64_t m_fired{0};
    std::uint64_t m_cancelled{0};
    std::uint64_t m_rescheduled{0};
    std::string m_error;
};

}  // namespace go2cpp::replay
