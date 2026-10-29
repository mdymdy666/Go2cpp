#pragma once

/**
 * @file heartbeat.hpp
 * @brief 心跳与超时检测（MOD-11）。
 *
 * @details 本组件只负责三件事：
 *   1. **心跳节拍**：`Start(interval)` 后由 `Poll()` 报告「该发心跳了」，
 *      实际发送由调用方完成（组件不碰 socket，便于单测与复用）；
 *   2. **存活判定**：`IsAlive(timeout)` 比较「最后一次收到对端活动」到现在
 *      的时长，单调时钟，不受系统时间回拨影响；
 *   3. **超时回调**：`Poll()` 发现超过超时阈值时调用一次 `OnTimeout`，
 *      并在 `OnReceive` / `Start` 之前不再重复触发。
 *
 * ## 默认值
 *
 *          心跳间隔 30s、超时 90s（3 个丢失心跳周期），与 MOD-11 的
 *          「30s/90s 可配」一致；两者都能在运行期通过 `SetInterval` /
 *          `SetTimeout` 调整。`Start(0)` / 构造传 0 表示「用默认值」，
 *          不是「关闭」。
 *
 * ## 时间源
 *
 *          默认用 `steady_clock`（单调）；测试通过构造函数注入假时钟，
 *          因此「30s/90s 之后会发生什么」不需要真的等待。
 *
 * ## 为什么没有后台线程
 *
 *          Bento 的网络循环本来就是「收包 → 处理 → 定时器」的单线程模型
 *          （见 Link::RunOnce），心跳挂在同一循环里即可，不需要额外线程与锁。
 *          `Poll()` 是**唯一**驱动点：不调用它，超时回调不会发生。
 *
 * ## 线程模型
 *
 *          非线程安全。所有方法（包括 `Poll()` 触发的回调）必须在同一线程
 *          串行调用；跨线程共享需要外部加锁。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstdint>
#include <functional>
#include <utility>

namespace go2cpp::bento {

/// @brief 单调时钟毫秒数（`std::chrono::steady_clock`），不受系统时间调整影响。
std::uint64_t MonotonicNowMs() noexcept;

/**
 * @brief 心跳发送节拍 + 对端存活检测。
 *
 * 典型用法：
 * ```cpp
 * Heartbeat hb(30'000, 90'000);
 * hb.SetOnTimeout([&] { conn.Close(); });
 * hb.Start(30'000);
 * // 事件循环里：
 * if (hb.Poll()) { SendHeartbeat(); }
 * // 收到对端任意消息时：
 * hb.OnReceive();
 * ```
 */
class Heartbeat {
 public:
    /// 时间源：返回单调毫秒数。空表示用 `MonotonicNowMs`。
    using NowFn = std::function<std::uint64_t()>;
    /// 超时回调：在 `Poll()` 内同步调用，只触发一次（直到再次有活动或重新 Start）。
    using TimeoutHandler = std::function<void()>;

    /// 默认心跳间隔：30 秒。
    static constexpr std::uint32_t kDefaultIntervalMs = 30000;
    /// 默认超时：90 秒（连续丢失 3 个心跳）。
    static constexpr std::uint32_t kDefaultTimeoutMs = 90000;

    /**
     * @brief 构造。
     * @param interval_ms 心跳间隔毫秒；0 表示用 kDefaultIntervalMs。
     * @param timeout_ms 存活超时毫秒；0 表示用 kDefaultTimeoutMs。
     * @param now 时间源；空表示用 MonotonicNowMs。测试注入假时钟。
     * @param on_timeout 超时回调；可为空，之后用 SetOnTimeout 设置。
     *
     * @note 构造时立即采样一次时间作为「最后活动时间」，因此 Start 之前调用
     *       IsAlive 也有确定语义。
     */
    explicit Heartbeat(std::uint32_t interval_ms = kDefaultIntervalMs,
                       std::uint32_t timeout_ms = kDefaultTimeoutMs,
                       NowFn now = nullptr,
                       TimeoutHandler on_timeout = nullptr);

    /**
     * @brief 启动（或重启）心跳节拍。
     * @param interval_ms 新的心跳间隔；0 表示沿用当前配置。
     *
     * @details 以当前时刻为基准：最后一次活动时间与下一次心跳时间都重置，
     *          超时状态与统计计数清零。「重启」语义用于重连后复用同一实例。
     */
    void Start(std::uint32_t interval_ms);

    /// 等同 `Start(IntervalMs())`，沿用当前间隔。
    void Start() { Start(0); }

    /// 停止心跳：之后 `Poll()` 恒返回 false，也不再触发超时回调。
    void Stop() noexcept;

    /// 是否处于运行状态（Start 与 Stop 之间）。
    bool IsRunning() const noexcept { return m_running; }

    /**
     * @brief 对端是否存活。
     * @param timeout_ms 超时阈值；0 表示「立即判死」，返回 false。
     * @return 距最后一次活动 < timeout_ms。
     *
     * @note 只看时间窗，与 IsRunning 无关；这样状态机可以在 Stop 之前先判活。
     *       注入时钟回退时按「刚有活动」处理（elapsed 夹到 0），不误杀连接。
     */
    bool IsAlive(std::uint32_t timeout_ms) const noexcept;

    /**
     * @brief 用当前配置的 timeout 判活。
     *
     * @details 比带参版本更严格：`Poll()` 上报过超时之后，在 `OnReceive` /
     *          `Start` 清除该状态之前恒为 false。否则「已判死」与「还活着」
     *          会同时成立——例如超时后把 `SetTimeout` 调大，无符号时间窗
     *          已经满足存活条件，但连接其实已经被上报判死了。
     */
    bool IsAlive() const noexcept { return !m_timeout_fired && IsAlive(m_timeout_ms); }

    /// 记录「刚收到对端一次活动」（任意消息，不限于心跳）。
    void OnReceive() noexcept { OnReceiveAt(Now()); }

    /**
     * @brief 用显式时刻记录对端活动。
     * @param now_ms 当前单调毫秒数（测试里直接传假时钟值，避免再取一次时间）。
     *
     * @details 除刷新最后活动时间外，还会清除「已超时」状态：下一次超过阈值时
     *          可以再次触发回调（例如对端恢复后又再次失联）。
     */
    void OnReceiveAt(std::uint64_t now_ms) noexcept;

    /**
     * @brief 推进一次定时器。
     * @return true 表示到了发送心跳的时刻（本次已推进下一次到期时间）。
     *
     * @details 超时判定优先于心跳：一旦判定超时，本组件进入 timed-out 状态，
     *          后续 Poll 返回 false（连接已该死，再发心跳没有意义），直到
     *          `OnReceive` 或 `Start` 清除该状态。同一时刻只产生一次心跳，
     *          即使时钟大步跳过多个周期也不补发（心跳只为刷新存活）。
     *
     * @warning 本函数是 noexcept。`TimeoutHandler` 若抛异常会直接
     *          `std::terminate`：回调里只做置标志、关连接这类不抛动作，
     *          复杂逻辑交给外层状态机。
     */
    bool Poll() noexcept;

    /// 设置超时回调（覆盖旧值）。
    void SetOnTimeout(TimeoutHandler handler) { m_on_timeout = std::move(handler); }

    /// 设置心跳间隔；0 表示恢复默认 30s。运行中调整会立即从当前时刻重排下一次
    /// 心跳；未运行时只改配置，由下一次 `Start` 重排。
    void SetInterval(std::uint32_t interval_ms) noexcept;

    /// 设置超时阈值；0 表示恢复默认 90s。调大**不会**撤销已经上报的超时状态：
    /// 连接是否复活由 `OnReceive` / `Start` 决定，避免「回调已关连接却又判活」。
    void SetTimeout(std::uint32_t timeout_ms) noexcept;

    /// 下一次应发心跳的单调时刻（毫秒）。
    std::uint64_t NextBeatMs() const noexcept { return m_next_beat_ms; }

    /// 最后一次对端活动的单调时刻（毫秒）。
    std::uint64_t LastActivityMs() const noexcept { return m_last_activity_ms; }

    /// 当前心跳间隔（毫秒）。
    std::uint32_t IntervalMs() const noexcept { return m_interval_ms; }

    /// 当前超时阈值（毫秒）。
    std::uint32_t TimeoutMs() const noexcept { return m_timeout_ms; }

    /// 自最近一次 Start 起 `Poll()` 报告的心跳次数。
    std::uint64_t Beats() const noexcept { return m_beats; }

    /// 自最近一次 Start 起超时回调触发次数（正常最多 1 次，恢复后可再触发）。
    std::uint64_t TimeoutsFired() const noexcept { return m_timeouts_fired; }

    /// 当前是否处于「已判超时、等待活动恢复」状态。
    bool IsTimedOut() const noexcept { return m_timeout_fired; }

 private:
    std::uint64_t Now() const noexcept { return m_now ? m_now() : MonotonicNowMs(); }

    NowFn m_now;                        ///< 时间源；空表示 MonotonicNowMs。
    TimeoutHandler m_on_timeout;        ///< 超时回调；Poll 内同步调用。
    std::uint32_t m_interval_ms;        ///< 心跳间隔；Start/SetInterval 修改。
    std::uint32_t m_timeout_ms;         ///< 超时阈值；SetTimeout 修改。
    std::uint64_t m_last_activity_ms;   ///< 最后活动时刻；构造/OnReceiveAt/Start 写。
    std::uint64_t m_next_beat_ms;       ///< 下次心跳时刻；Start/Poll/SetInterval 写。
    std::uint64_t m_beats = 0;          ///< 心跳计数；Start 清零、Poll 自增。
    std::uint64_t m_timeouts_fired = 0; ///< 超时触发计数；Start 清零、Poll 自增。
    bool m_running = false;             ///< 是否已 Start 且未 Stop。
    bool m_timeout_fired = false;       ///< 已触发超时、尚未恢复活动。
};

}  // namespace go2cpp::bento
