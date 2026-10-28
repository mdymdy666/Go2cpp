/**
 * @file heartbeat.cpp
 * @brief 心跳节拍与超时检测实现（MOD-11）。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/heartbeat.hpp"

#include <chrono>
#include <utility>

namespace go2cpp::bento {

std::uint64_t MonotonicNowMs() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

Heartbeat::Heartbeat(std::uint32_t interval_ms, std::uint32_t timeout_ms, NowFn now,
                     TimeoutHandler on_timeout)
    : m_now(std::move(now)),
      m_on_timeout(std::move(on_timeout)),
      m_interval_ms(interval_ms == 0 ? kDefaultIntervalMs : interval_ms),
      m_timeout_ms(timeout_ms == 0 ? kDefaultTimeoutMs : timeout_ms),
      m_last_activity_ms(Now()),
      m_next_beat_ms(m_last_activity_ms + m_interval_ms) {}

void Heartbeat::Start(std::uint32_t interval_ms) {
    if (interval_ms != 0) {
        m_interval_ms = interval_ms;
    }
    const std::uint64_t now = Now();
    m_last_activity_ms = now;
    m_next_beat_ms = now + m_interval_ms;
    m_beats = 0;
    m_timeouts_fired = 0;
    m_timeout_fired = false;
    m_running = true;
}

void Heartbeat::Stop() noexcept {
    m_running = false;
}

bool Heartbeat::IsAlive(std::uint32_t timeout_ms) const noexcept {
    // timeout 为 0 不是「永不过期」，而是「立即判死」：这两种解释都合理，
    // 但显式判死能避免配置漏填时把失效连接当成活连接。
    if (timeout_ms == 0) {
        return false;
    }
    return Now() - m_last_activity_ms < timeout_ms;
}

void Heartbeat::OnReceiveAt(std::uint64_t now_ms) noexcept {
    m_last_activity_ms = now_ms;
    m_timeout_fired = false;
}

bool Heartbeat::Poll() noexcept {
    if (!m_running || m_timeout_fired) {
        return false;
    }
    const std::uint64_t now = Now();
    if (now - m_last_activity_ms >= m_timeout_ms) {
        m_timeout_fired = true;
        ++m_timeouts_fired;
        // 先置状态再回调：回调里重复 Poll 或调用 OnReceive 都不会造成二次触发。
        if (m_on_timeout) {
            m_on_timeout();
        }
        return false;
    }
    if (now < m_next_beat_ms) {
        return false;
    }
    m_next_beat_ms = now + m_interval_ms;
    ++m_beats;
    return true;
}

void Heartbeat::SetInterval(std::uint32_t interval_ms) noexcept {
    m_interval_ms = interval_ms == 0 ? kDefaultIntervalMs : interval_ms;
    if (m_running) {
        // 运行中改间隔：从当前时刻重新排期，避免沿用旧的到期时刻造成突刺。
        m_next_beat_ms = Now() + m_interval_ms;
    }
}

void Heartbeat::SetTimeout(std::uint32_t timeout_ms) noexcept {
    m_timeout_ms = timeout_ms == 0 ? kDefaultTimeoutMs : timeout_ms;
}

}  // namespace go2cpp::bento
