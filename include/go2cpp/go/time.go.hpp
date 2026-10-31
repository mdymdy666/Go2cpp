/**
 * @file time.go.hpp
 * @brief Go `time` 包的 C++ 对应物（Timer / Ticker / After / Sleep / Now）。
 *
 * ## 定位
 * `go2cpp/tools/time.hpp` 已实现 `Duration`、`Time`、`Sleep`、`Since`、`Until`，
 * 但**明确未实现** `After`/`Tick`/`Ticker`/`Timer`（见该文件末尾的待办表），
 * 原因是它们需要与 `Channel<Time>`、`core::TimerService` 对接。本文件补上
 * 这一层，并保持 Go 的接口形态：
 *
 * ```go
 * select {
 * case <-time.After(30 * time.Second):
 * case <-done:
 * }
 *
 * ticker := time.NewTicker(time.Second)
 * defer ticker.Stop()
 * for now := range ticker.C { _ = now }
 * ```
 *
 * 对应 C++：
 *
 * ```cpp
 * go::Select({ch.RecvCase(), go::AfterCase(go::Second), go::DefaultCase()});
 *
 * auto ticker = go::NewTicker(go::Second);
 * while (auto now = ticker->C().Recv()) { ... }
 * ticker->Stop();
 * ```
 *
 * ## 与 Go 的差异（重要）
 * - Go 的 `time.After(d)` 返回 `<-chan Time`；C++ 的 `After(d)` 返回
 *   `std::shared_ptr<Timer>`（因为 C++ 没有 GC，定时器句柄必须有人持有），
 *   `After(d)->C()` 才是通道。纯通道形式用 `AfterChan(d)`。
 * - Go 的 `Timer.Stop`/`Reset` 有竞态细则；本封装基于 `core::TimerService`，
 *   `Stop()` 只保证「尚未执行的回调不会再执行」，已经进入回调的调用栈不会被打断。
 * - `AfterFunc` 的回调在 `TimerService` 的工作线程上执行，不是 goroutine。
 * - `Tick(d)` 在 Go 中无法停止、会泄漏；本封装返回 `shared_ptr<Ticker>`，可 Stop。
 */

#pragma once

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/core/timer.hpp"
#include "go2cpp/go/chan.go.hpp"
#include "go2cpp/tools/time.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace go2cpp::go {

using Duration = ::go2cpp::time::Duration;
using Time = ::go2cpp::time::Time;

// 单位常量直接透出，写法与 Go 一致：go::Second、go::Millisecond。
using ::go2cpp::time::Hour;
using ::go2cpp::time::Microsecond;
using ::go2cpp::time::Millisecond;
using ::go2cpp::time::Minute;
using ::go2cpp::time::Nanosecond;
using ::go2cpp::time::Second;

/** @brief 对应 `time.Now()`。 */
inline Time Now() {
    return Time::Now();
}

/** @brief 对应 `time.Since(t)`。 */
inline Duration Since(const Time& start) {
    return ::go2cpp::time::Since(start);
}

/** @brief 对应 `time.Until(deadline)`。 */
inline Duration Until(const Time& deadline) {
    return ::go2cpp::time::Until(deadline);
}

/** @brief 对应 `time.Sleep(d)`；在 Fiber 上下文中由 Hook 转成协作式等待。 */
inline void Sleep(Duration duration) {
    ::go2cpp::time::Sleep(duration);
}

namespace detail {

/** @brief `go2cpp::time::Duration` → `steady_clock::duration`。 */
inline std::chrono::steady_clock::duration ToSteady(Duration duration) noexcept {
    return std::chrono::nanoseconds(duration.Nanoseconds());
}

/** @brief `steady_clock::duration` → `go2cpp::time::Duration`。 */
inline Duration FromSteady(std::chrono::steady_clock::duration duration) noexcept {
    return Duration(
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

}  // namespace detail

/**
 * @brief 对应 `time.Timer`。
 * @details 一次性定时器，到期后向 `C()` 发送一个 `Time`。通道容量为 1，
 *          到期发送使用 `TrySend`，因此即使无人接收也不会阻塞定时器线程
 *          （与 Go 的 timer channel 缓冲 1 一致）。
 * @note 对象通过 `shared_ptr` 持有；`Stop()` 只取消尚未执行的回调。
 */
class Timer final {
public:
    using Ptr = std::shared_ptr<Timer>;

    /**
     * @brief 创建一次性定时器。
     * @param delay 相对当前时刻的延时；`<= 0` 表示尽快触发。
     * @param callback 到期回调（在 `core::TimerService` 工作线程执行）。
     */
    static Ptr New(Duration delay, std::function<void()> callback = {});

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    /** @brief 到期通道，容量 1。 */
    Chan<Time>& C() noexcept { return m_channel; }
    const Chan<Time>& C() const noexcept { return m_channel; }

    /** @brief 对应 `Timer.Stop()`；取消尚未执行的回调，重复调用安全。 */
    bool Stop() noexcept {
        m_timer.Cancel();
        return true;
    }

    /** @brief 对应 `Timer.Reset(d)`；返回是否成功重新排程。 */
    bool Reset(Duration delay);

    /** @brief 是否仍可能触发。 */
    bool Active() const noexcept { return m_timer.Active(); }

    /** @brief 已触发次数。 */
    std::size_t RunCount() const noexcept { return m_timer.RunCount(); }

private:
    Timer(Duration delay, std::function<void()> callback);

    Chan<Time> m_channel;
    ::go2cpp::core::Timer m_timer;
};

inline Timer::Timer(Duration delay, std::function<void()> callback) {
    m_channel = Chan<Time>::Make(1);
    auto channel = m_channel;
    m_timer = ::go2cpp::core::TimerService::Default().ScheduleAfter(
        detail::ToSteady(delay), [channel, callback = std::move(callback)]() mutable {
            if (callback) {
                callback();
            }
            // 与 Go 的 timer 通道一致：缓冲 1，无人接收时丢弃本次 tick。
            (void)channel.TrySend(Now());
        });
}

inline Timer::Ptr Timer::New(Duration delay, std::function<void()> callback) {
    return std::shared_ptr<Timer>(new Timer(delay, std::move(callback)));
}

inline bool Timer::Reset(Duration delay) {
    return m_timer.Reset(std::chrono::steady_clock::now() + detail::ToSteady(delay));
}

/**
 * @brief 对应 `time.NewTimer(d)`；返回可 Stop/Reset 的定时器。
 * @note Go 的 `time.NewTimer` 返回 `*Timer`，本封装返回 `shared_ptr<Timer>`。
 */
inline Timer::Ptr NewTimer(Duration delay) {
    return Timer::New(delay, {});
}

/**
 * @brief 对应 `time.After(d)`。
 * @note 与 Go 的差异：返回 `shared_ptr<Timer>` 而非裸通道，避免 C++ 中
 *       立即析构导致定时器被取消；通道用 `->C()` 取。只要有人持有返回值，
 *       定时器就不会被回收。纯通道写法用 `AfterChan`。
 */
inline Timer::Ptr After(Duration delay) {
    return Timer::New(delay, {});
}

/**
 * @brief `time.After(d)` 的纯通道形式（自持有定时器句柄）。
 * @details 定时器回调与通道句柄互相引用，回调执行后会取消自身，打破循环；
 *          因此没有外部持有者也不会泄漏。
 */
inline Chan<Time> AfterChan(Duration delay) {
    Chan<Time> channel = Chan<Time>::Make(1);
    auto holder = std::make_shared<::go2cpp::core::Timer>();
    *holder = ::go2cpp::core::TimerService::Default().ScheduleAfter(
        detail::ToSteady(delay), [channel, holder]() mutable {
            (void)channel.TrySend(Now());
            holder->Cancel();  // 打破 holder ↔ 回调 的循环引用
        });
    return channel;
}

/** @brief 对应 `time.AfterFunc(d, f)`；返回可 Stop 的定时器。 */
inline Timer::Ptr AfterFunc(Duration delay, std::function<void()> callback) {
    return Timer::New(delay, std::move(callback));
}

/**
 * @brief 对应 `time.Ticker`。
 * @details 周期触发，通道容量 1；接收方处理慢时**丢弃**旧 tick（Go 语义）。
 *          周期任务由 `core::TimerService` 驱动，回调在工作线程执行。
 */
class Ticker final {
public:
    using Ptr = std::shared_ptr<Ticker>;

    /** @brief 创建周期定时器；`period <= 0` 返回 nullptr（Go 会 panic）。 */
    static Ptr New(Duration period);

    Ticker(const Ticker&) = delete;
    Ticker& operator=(const Ticker&) = delete;

    /** @brief tick 通道，容量 1。 */
    Chan<Time>& C() noexcept { return m_channel; }
    const Chan<Time>& C() const noexcept { return m_channel; }

    /** @brief 对应 `Ticker.Stop()`；可重复调用。 */
    void Stop() noexcept { (void)m_timer.Cancel(); }

    /** @brief 是否仍在运行。 */
    bool Active() const noexcept { return m_timer.Active(); }

    /** @brief 对应 `Ticker.Reset(d)`；返回是否成功。 */
    bool Reset(Duration period);

private:
    explicit Ticker(Duration period);

    Chan<Time> m_channel;
    ::go2cpp::core::Timer m_timer;
};

inline Ticker::Ticker(Duration period) {
    m_channel = Chan<Time>::Make(1);
    auto channel = m_channel;
    m_timer = ::go2cpp::core::TimerService::Default().ScheduleEvery(
        detail::ToSteady(period), [channel]() mutable {
            // Go 的 ticker 缓冲 1：消费慢时丢弃旧 tick，而不是堆积。
            (void)channel.TrySend(Now());
        });
}

inline Ticker::Ptr Ticker::New(Duration period) {
    if (period.Nanoseconds() <= 0) {
        return nullptr;
    }
    return std::shared_ptr<Ticker>(new Ticker(period));
}

inline bool Ticker::Reset(Duration period) {
    if (period.Nanoseconds() <= 0) {
        return false;
    }
    return m_timer.Refresh();
}

/** @brief 对应 `time.NewTicker(d)`。 */
inline Ticker::Ptr NewTicker(Duration period) {
    return Ticker::New(period);
}

/**
 * @brief 对应 `time.Tick(d)`。
 * @note Go 的 `Tick` 无法停止、会泄漏；本封装返回 `shared_ptr<Ticker>`，
 *       调用方可以 Stop，未 Stop 时由持有者析构自动取消。
 */
inline Ticker::Ptr Tick(Duration period) {
    return Ticker::New(period);
}

// ============================================================================
// select 集成
// ============================================================================

/**
 * @brief 构造 `case <-time.After(d):` 的 select 分支。
 * @details 底层 `go2cpp::AfterCase` 会注册一个一次性定时器，并在分支未获胜时
 *          取消它；因此不需要调用方持有 Timer 句柄。
 */
inline SelectCase AfterCase(Duration delay) {
    return ::go2cpp::AfterCase(std::chrono::nanoseconds(delay.Nanoseconds()));
}

/** @brief 构造 `case <-time.After(deadline):` 的绝对截止时间版本。 */
inline SelectCase AfterCaseAt(::go2cpp::ContextTimePoint deadline) {
    return ::go2cpp::AfterCase(deadline);
}

}  // namespace go2cpp::go
