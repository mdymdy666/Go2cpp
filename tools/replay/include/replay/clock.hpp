#pragma once

/**
 * @file clock.hpp
 * @brief 录制/重放共用的虚拟时钟与 sleep。
 *
 * - **live**：`SleepFor()` 真的 `sleep_for`，并把 `kTimer(kSleep)` 写进记录；
 *   时间取 steady_clock 相对录制起点的纳秒。
 * - **replay**：`SleepFor()` 只查 `(gid, sleep_index)` 对应的 `kTimer` 事件，
 *   把虚拟时钟推进到 `vtime + duration` 后**立即返回**。因此录制时 100ms
 *   的等待，重放只需要微秒级，且不读真实时钟。
 *
 * 限制（如实声明）：live 模式的 sleep 在 fiber 内会阻塞承载它的 M 线程；
 * 本工具定位是「少量、低频的定时点」，不是通用 timer 实现。
 */

#include "replay/gid.hpp"
#include "replay/recorder.hpp"
#include "replay/replay_source.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace go2cpp::replay {

namespace detail {
/// 解析 live 模式的 gid（声明在这里，定义在 gid.hpp，避免循环 include）。
std::uint32_t ResolveGid();
}  // namespace detail

class ReplayClock {
public:
    /// live 模式。
    explicit ReplayClock(Recorder& recorder) : m_recorder(&recorder) {}
    /// replay 模式。
    explicit ReplayClock(ReplaySource& source) : m_source(&source) {}

    /// 当前虚拟时间（相对录制起点）。
    std::uint64_t NowNs() const {
        if (m_source != nullptr) {
            return m_source->VirtualNowNs();
        }
        return m_recorder->VirtualNowNs();
    }

    /**
     * @brief 睡 `duration_ns`。
     *
     * live：写一条 `kTimer(kSleep)` 后真实等待；replay：按记录瞬间推进。
     */
    void SleepFor(std::uint64_t duration_ns) {
        if (duration_ns == 0) {
            return;
        }
        if (m_source != nullptr) {
            ReplaySleep(duration_ns);
            return;
        }
        const std::uint32_t gid = detail::ResolveGid();
        const std::uint32_t op_index = m_recorder->NextSleepIndex(gid);
        m_recorder->Write(MakeTimerEvent(gid, TimerKind::kSleep, duration_ns,
                                         op_index, "sleep"));
        std::this_thread::sleep_for(std::chrono::nanoseconds(duration_ns));
    }

    /// 睡到绝对虚拟时间 `deadline_ns`（已过期则立即返回）。
    void SleepUntil(std::uint64_t deadline_ns) {
        const std::uint64_t now = NowNs();
        if (deadline_ns <= now) {
            return;
        }
        SleepFor(deadline_ns - now);
    }

    /// 最近一次重放/时钟错误（空表示无）。
    const std::string& error() const noexcept { return m_error; }

private:
    void ReplaySleep(std::uint64_t duration_ns) {
        const std::uint32_t gid = m_source->CurrentFiberGid();
        const Event* event = m_source->NextSleep(gid);
        if (event == nullptr) {
            m_error = "no recorded sleep for gid " + std::to_string(gid);
            return;
        }
        if (event->type != EventType::kTimer ||
            event->timer_kind != static_cast<std::uint8_t>(TimerKind::kSleep)) {
            m_error = "recorded sleep type mismatch for gid " + std::to_string(gid);
            return;
        }
        if (event->timer_ns != duration_ns) {
            m_error = "recorded sleep duration mismatch for gid " + std::to_string(gid) +
                      " (recorded " + std::to_string(event->timer_ns) +
                      ", program " + std::to_string(duration_ns) + ")";
        }
        const std::uint64_t deadline = event->vtime_ns + event->timer_ns;
        if (deadline > m_source->VirtualNowNs()) {
            m_source->AdvanceVirtualTime(deadline);
        }
    }

    Recorder* const m_recorder{nullptr};
    ReplaySource* const m_source{nullptr};
    std::string m_error;
};

}  // namespace go2cpp::replay
