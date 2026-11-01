/**
 * @file degradation.cpp
 * @brief `DegradationController` / `ReconnectSupervisor` / `Downscale2xBgra` 的实现。
 * @details 决策逻辑刻意保持纯整数/少量 double：`Update` 每 100~500ms 才调一次，
 *          不是性能敏感路径；真正热的是 `ShouldDeliverFrame`（每帧一次）与
 *          `ReconnectSupervisor::OnPacket`（每包一次），两者都只有几次比较与自增。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/degradation.hpp"

#include <algorithm>

#include "go2cpp/bento/net/bandwidth.hpp"

namespace go2cpp::media {
namespace {

/// @brief 档位与档位数必须同步，否则索引会越界。
static_assert(static_cast<std::size_t>(QualityLevel::kMinimal) + 1 == kQualityLevelCount,
              "QualityLevel 与 kQualityLevelCount 不同步");

/// @brief 档位转下标。
std::size_t Index(QualityLevel level) noexcept {
    return static_cast<std::size_t>(level);
}

/// @brief 64 位带宽截断到 32 位（饱和，不产生负数）。
std::uint32_t ClampToU32(std::uint64_t value) noexcept {
    constexpr std::uint64_t kMax = 0xFFFFFFFFULL;
    return value > kMax ? static_cast<std::uint32_t>(kMax) : static_cast<std::uint32_t>(value);
}

}  // namespace

const char* QualityLevelName(QualityLevel level) noexcept {
    switch (level) {
        case QualityLevel::kFull:
            return "full";
        case QualityLevel::kReduced:
            return "reduced";
        case QualityLevel::kLow:
            return "low";
        case QualityLevel::kMinimal:
            return "minimal";
    }
    return "unknown";
}

const char* DegradationReasonName(DegradationReason reason) noexcept {
    switch (reason) {
        case DegradationReason::kNone:
            return "none";
        case DegradationReason::kBandwidth:
            return "bandwidth";
        case DegradationReason::kLoss:
            return "loss";
        case DegradationReason::kRecovery:
            return "recovery";
        case DegradationReason::kReconnect:
            return "reconnect";
        case DegradationReason::kExternal:
            return "external";
    }
    return "unknown";
}

const char* LinkStateName(LinkState state) noexcept {
    switch (state) {
        case LinkState::kConnected:
            return "connected";
        case LinkState::kSuspect:
            return "suspect";
        case LinkState::kDisconnected:
            return "disconnected";
        case LinkState::kReconnecting:
            return "reconnecting";
        case LinkState::kFailed:
            return "failed";
    }
    return "unknown";
}

std::array<QualityParams, kQualityLevelCount> DefaultQualityLadder() noexcept {
    // 分辨率与帧率严格单调下降：320x240 → 320x240(15fps) → 160x120 → 160x120(10fps)。
    // 先降帧率再降分辨率：帧率对实时性的观感影响最小，且不改变编码器的尺寸假设。
    return {{
        {320, 240, 30, 800'000, 900'000},  // kFull
        {320, 240, 15, 450'000, 520'000},  // kReduced
        {160, 120, 15, 200'000, 240'000},  // kLow
        {160, 120, 10, 120'000, 140'000},  // kMinimal
    }};
}

NetworkSample NetworkSample::FromBbr(const go2cpp::bento::BandwidthStats& stats) noexcept {
    NetworkSample sample;
    sample.available_bps = stats.delivery_rate_bps;
    sample.loss_rate = stats.loss_rate;
    sample.rtt_ms = 0;  // BbrRttEstimator 的平滑 RTT 不在 BandwidthStats 里
    // 完全没有任何 ACK 样本时估计器未收敛，交给调用方跳过本次决策。
    sample.valid = stats.delivery_rate_bps != 0 || stats.acked_bytes != 0;
    return sample;
}

DegradationController::DegradationController(const DegradationConfig& config) noexcept
    : m_config(config) {
    // 规范化：非法参数不能让后期除零或产生 0 尺寸。
    for (std::size_t i = 0; i < kQualityLevelCount; ++i) {
        QualityParams& params = m_config.ladder[i];
        if (params.width <= 0) {
            params.width = 160;
        }
        if (params.height <= 0) {
            params.height = 120;
        }
        if (params.fps <= 0) {
            params.fps = 10;
        }
        if (params.bitrate_bps == 0) {
            params.bitrate_bps = 100'000;
        }
        if (params.min_bandwidth_bps == 0) {
            params.min_bandwidth_bps = params.bitrate_bps;
        }
    }
    if (m_config.downgrade_hold_ms == 0) {
        m_config.downgrade_hold_ms = 1;
    }
    if (m_config.upgrade_hold_ms == 0) {
        m_config.upgrade_hold_ms = 1;
    }
    if (m_config.max_downgrade_step == 0) {
        m_config.max_downgrade_step = 1;
    }
    if (m_config.bandwidth_ewma_shift > 8) {
        m_config.bandwidth_ewma_shift = 8;
    }
    if (m_config.safety_factor <= 0.0) {
        m_config.safety_factor = 0.9;
    }
    if (m_config.upgrade_margin < 1.0) {
        m_config.upgrade_margin = 1.0;
    }
}

bool DegradationController::Fits(QualityLevel level, std::uint64_t usable_bps) const noexcept {
    return usable_bps >= m_config.ladder[Index(level)].min_bandwidth_bps;
}

void DegradationController::ApplyLevel(QualityLevel level) noexcept {
    m_level = level;
    // 切档后帧率节拍器必须重新对齐，否则会把旧档位的相位带进新档。
    m_paced = false;
    m_next_due_ms = 0;
}

DegradationEvent DegradationController::Update(std::int64_t now_ms,
                                               const NetworkSample& sample) noexcept {
    ++m_stats.samples;
    m_stats.last_update_ms = now_ms;
    if (!sample.valid) {
        return DegradationEvent{};
    }

    const std::int64_t raw_bps = static_cast<std::int64_t>(sample.available_bps);
    if (!m_has_sample) {
        m_has_sample = true;
        m_smoothed_bps = raw_bps;
    } else {
        m_smoothed_bps += (raw_bps - m_smoothed_bps) >> m_config.bandwidth_ewma_shift;
    }
    if (m_smoothed_bps < 0) {
        m_smoothed_bps = 0;
    }

    m_stats.last_available_bps = sample.available_bps;
    m_stats.last_loss_rate = sample.loss_rate;

    const std::uint64_t usable =
        static_cast<std::uint64_t>(static_cast<double>(m_smoothed_bps) * m_config.safety_factor);

    // ---- 1) 决定期望档位 ----
    QualityLevel desired = m_level;
    DegradationReason reason = DegradationReason::kNone;

    // 中等丢包只降一级：持续丢包时若每个 hold 周期都再降一级，会在几秒内
    // 直接掉到保底档，而真实原因可能只是瞬时拥塞。严重丢包不受冷却限制。
    const bool loss_cooling = m_loss_downgraded &&
                              now_ms - m_last_loss_downgrade_ms <
                                  static_cast<std::int64_t>(m_config.loss_cooldown_ms);
    if (sample.loss_rate >= m_config.loss_severe) {
        desired = QualityLevel::kMinimal;
        reason = DegradationReason::kLoss;
    } else if (sample.loss_rate >= m_config.loss_downgrade && !loss_cooling) {
        const std::size_t next = std::min<std::size_t>(Index(m_level) + 1, kQualityLevelCount - 1);
        desired = static_cast<QualityLevel>(next);
        reason = DegradationReason::kLoss;
    } else if (m_level != QualityLevel::kFull && sample.loss_rate < m_config.loss_recover) {
        const std::uint64_t need = static_cast<std::uint64_t>(
            static_cast<double>(m_config.ladder[Index(m_level) - 1].min_bandwidth_bps) *
            m_config.upgrade_margin);
        if (usable >= need) {
            desired = static_cast<QualityLevel>(Index(m_level) - 1);
            reason = DegradationReason::kRecovery;
        }
    }

    if (desired == m_level && !Fits(m_level, usable)) {
        // 带宽不够维持当前档位：往下找第一个能维持的（级数越小画质越高）。
        std::size_t target = kQualityLevelCount - 1;
        for (std::size_t i = Index(m_level) + 1; i < kQualityLevelCount; ++i) {
            if (Fits(static_cast<QualityLevel>(i), usable)) {
                target = i;
                break;
            }
        }
        desired = static_cast<QualityLevel>(target);
        reason = DegradationReason::kBandwidth;
    }

    if (desired == m_level) {
        m_pending = Pending::kNone;
        return DegradationEvent{};
    }

    // ---- 2) 滞回：降级快、升级慢 ----
    const Pending direction = Index(desired) < Index(m_level) ? Pending::kUp : Pending::kDown;
    if (m_pending != direction) {
        m_pending = direction;
        m_pending_since_ms = now_ms;
    }
    if (direction == Pending::kUp && now_ms < m_upgrade_blocked_until_ms) {
        return DegradationEvent{};
    }
    const std::uint32_t hold =
        direction == Pending::kUp ? m_config.upgrade_hold_ms : m_config.downgrade_hold_ms;
    if (now_ms - m_pending_since_ms < static_cast<std::int64_t>(hold)) {
        return DegradationEvent{};
    }

    // 降级一次最多跨 `max_downgrade_step` 级（避免一刀切到底）。
    QualityLevel target = desired;
    if (direction == Pending::kDown) {
        const std::size_t limit =
            Index(m_level) + static_cast<std::size_t>(m_config.max_downgrade_step);
        target = static_cast<QualityLevel>(std::min(Index(desired), limit));
    }
    if (target == m_level) {
        m_pending = Pending::kNone;
        return DegradationEvent{};
    }

    DegradationEvent event;
    event.changed = true;
    event.from = m_level;
    event.to = target;
    event.reason = reason;
    ApplyLevel(target);
    if (reason == DegradationReason::kLoss) {
        m_last_loss_downgrade_ms = now_ms;
        m_loss_downgraded = true;
    }
    if (direction == Pending::kUp) {
        ++m_stats.upgrades;
    } else {
        ++m_stats.downgrades;
    }
    m_pending = Pending::kNone;
    return event;
}

DegradationEvent DegradationController::ForceLevel(std::int64_t now_ms, QualityLevel level,
                                                   DegradationReason reason) noexcept {
    ++m_stats.forced;
    m_pending = Pending::kNone;
    m_pending_since_ms = now_ms;
    if (level == m_level) {
        return DegradationEvent{};
    }
    DegradationEvent event;
    event.changed = true;
    event.from = m_level;
    event.to = level;
    event.reason = reason;
    ApplyLevel(level);
    return event;
}

DegradationEvent DegradationController::OnReconnected(std::int64_t now_ms) noexcept {
    ++m_stats.reconnects;
    m_keyframe_request = true;
    m_upgrade_blocked_until_ms = now_ms + static_cast<std::int64_t>(m_config.reconnect_guard_ms);
    m_pending = Pending::kNone;
    DegradationEvent event;
    event.from = m_level;
    event.to = m_level;
    event.reason = DegradationReason::kReconnect;
    return event;
}

std::uint32_t DegradationController::adaptive_bitrate_bps() const noexcept {
    const std::uint32_t nominal = current().bitrate_bps;
    if (!m_has_sample) {
        return nominal;
    }
    const std::uint64_t usable =
        static_cast<std::uint64_t>(static_cast<double>(m_smoothed_bps) * m_config.safety_factor);
    const std::uint32_t floor = nominal / 4 == 0 ? nominal : nominal / 4;
    const std::uint32_t cap = ClampToU32(usable);
    if (cap <= floor) {
        return floor;
    }
    return cap < nominal ? cap : nominal;
}

std::uint64_t DegradationController::smoothed_bandwidth_bps() const noexcept {
    return m_smoothed_bps < 0 ? 0 : static_cast<std::uint64_t>(m_smoothed_bps);
}

bool DegradationController::ShouldDeliverFrame(std::int64_t now_ms) noexcept {
    const int fps = current().fps;
    if (fps <= 0) {
        ++m_stats.frames_dropped;
        return false;
    }
    const std::int64_t interval = 1000 / static_cast<std::int64_t>(fps);
    if (!m_paced) {
        m_paced = true;
        m_next_due_ms = now_ms + interval;
        ++m_stats.frames_passed;
        return true;
    }
    if (now_ms >= m_next_due_ms) {
        m_next_due_ms += interval;
        if (m_next_due_ms <= now_ms) {
            // 落后超过一个周期：重新对齐，不补帧（补帧只会制造突发）。
            m_next_due_ms = now_ms + interval;
        }
        ++m_stats.frames_passed;
        return true;
    }
    ++m_stats.frames_dropped;
    return false;
}

bool DegradationController::ConsumeKeyframeRequest() noexcept {
    const bool requested = m_keyframe_request;
    m_keyframe_request = false;
    return requested;
}

DegradationStats DegradationController::Snapshot() const noexcept {
    DegradationStats snapshot = m_stats;
    snapshot.smoothed_bps = smoothed_bandwidth_bps();
    return snapshot;
}

ReconnectSupervisor::ReconnectSupervisor(const ReconnectConfig& config) noexcept : m_config(config) {
    if (m_config.suspect_after_ms == 0) {
        m_config.suspect_after_ms = 1;
    }
    if (m_config.disconnect_after_ms < m_config.suspect_after_ms) {
        m_config.disconnect_after_ms = m_config.suspect_after_ms;
    }
    if (m_config.initial_backoff_ms == 0) {
        m_config.initial_backoff_ms = 1;
    }
    if (m_config.max_backoff_ms < m_config.initial_backoff_ms) {
        m_config.max_backoff_ms = m_config.initial_backoff_ms;
    }
    if (m_config.max_attempts == 0) {
        m_config.max_attempts = 1;
    }
    if (m_config.jitter_percent > 100) {
        m_config.jitter_percent = 100;
    }
    m_rng = m_config.seed == 0 ? 0x9E3779B9u : m_config.seed;
}

void ReconnectSupervisor::OnPacket(std::int64_t now_ms) noexcept {
    ++m_stats.packets;
    m_last_packet_ms = now_ms;
    m_has_packet = true;
    if (m_state != LinkState::kConnected) {
        // 断线态下收到任何包 = 链路已恢复：立刻回到连接态并请求关键帧。
        m_state = LinkState::kConnected;
        m_attempts = 0;
        m_next_attempt_ms = 0;
        m_keyframe_request = true;
        ++m_stats.recoveries;
    }
}

ReconnectAction ReconnectSupervisor::Tick(std::int64_t now_ms) noexcept {
    switch (m_state) {
        case LinkState::kConnected: {
            if (m_has_packet &&
                now_ms - m_last_packet_ms >=
                    static_cast<std::int64_t>(m_config.suspect_after_ms)) {
                m_state = LinkState::kSuspect;
            }
            return ReconnectAction::kNone;
        }
        case LinkState::kSuspect: {
            if (now_ms - m_last_packet_ms >=
                static_cast<std::int64_t>(m_config.disconnect_after_ms)) {
                m_state = LinkState::kDisconnected;
                m_disconnected_at_ms = now_ms;
                m_attempts = 0;
                m_last_backoff_ms = ComputeBackoffMs(0);
                m_next_attempt_ms = now_ms + static_cast<std::int64_t>(m_last_backoff_ms);
                ++m_stats.disconnects;
            }
            return ReconnectAction::kNone;
        }
        case LinkState::kDisconnected:
        case LinkState::kReconnecting: {
            if (now_ms < m_next_attempt_ms) {
                return ReconnectAction::kNone;
            }
            if (m_attempts >= m_config.max_attempts) {
                m_state = LinkState::kFailed;
                m_next_attempt_ms = now_ms + static_cast<std::int64_t>(m_config.max_backoff_ms);
                ++m_stats.exhausted;
                return ReconnectAction::kExhausted;
            }
            ++m_attempts;
            m_last_backoff_ms = ComputeBackoffMs(m_attempts);
            m_next_attempt_ms = now_ms + static_cast<std::int64_t>(m_last_backoff_ms);
            m_state = LinkState::kReconnecting;
            ++m_stats.attempts;
            return ReconnectAction::kProbe;
        }
        case LinkState::kFailed: {
            if (now_ms >= m_next_attempt_ms) {
                m_next_attempt_ms = now_ms + static_cast<std::int64_t>(m_config.max_backoff_ms);
                return ReconnectAction::kExhausted;
            }
            return ReconnectAction::kNone;
        }
    }
    return ReconnectAction::kNone;
}

void ReconnectSupervisor::OnAttemptResult(std::int64_t now_ms, bool ok) noexcept {
    if (!ok) {
        if (m_state == LinkState::kReconnecting && m_attempts >= m_config.max_attempts) {
            m_state = LinkState::kFailed;
            m_next_attempt_ms = now_ms + static_cast<std::int64_t>(m_config.max_backoff_ms);
            ++m_stats.exhausted;
        }
        return;
    }
    m_attempts = 0;
    m_state = LinkState::kConnected;
    m_last_packet_ms = now_ms;
    m_has_packet = true;
    m_next_attempt_ms = 0;
    m_keyframe_request = true;
    ++m_stats.recoveries;
}

bool ReconnectSupervisor::ConsumeKeyframeRequest() noexcept {
    const bool requested = m_keyframe_request;
    m_keyframe_request = false;
    return requested;
}

void ReconnectSupervisor::Reset() noexcept {
    m_state = LinkState::kConnected;
    m_last_packet_ms = 0;
    m_next_attempt_ms = 0;
    m_disconnected_at_ms = 0;
    m_attempts = 0;
    m_last_backoff_ms = 0;
    m_has_packet = false;
    m_keyframe_request = false;
}

ReconnectStats ReconnectSupervisor::Snapshot() const noexcept {
    return m_stats;
}

std::uint32_t ReconnectSupervisor::ComputeBackoffMs(std::uint32_t attempt) noexcept {
    std::uint64_t base = m_config.initial_backoff_ms;
    const std::uint32_t shift = attempt < 20 ? attempt : 20;
    base <<= shift;
    if (base > m_config.max_backoff_ms) {
        base = m_config.max_backoff_ms;
    }
    const std::uint32_t base32 = static_cast<std::uint32_t>(base);
    const std::uint32_t span = base32 / 100 * m_config.jitter_percent;
    if (span == 0) {
        return base32;
    }
    // 确定性抖动：同一 attempt 序列可复现，测试能断言区间而不 flaky。
    const std::uint32_t offset = NextRandom() % (span * 2 + 1);
    return base32 - span + offset;
}

std::uint32_t ReconnectSupervisor::NextRandom() noexcept {
    std::uint32_t x = m_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_rng = x;
    return x;
}

void Downscale2xBgra(const std::uint8_t* src, int src_width, int src_height, int src_stride,
                     std::uint8_t* dst, int dst_stride) noexcept {
    if (src == nullptr || dst == nullptr || src_width <= 0 || src_height <= 0) {
        return;
    }
    if ((src_width & 1) != 0 || (src_height & 1) != 0) {
        return;  // 2:1 盒式下采样要求偶数尺寸
    }
    const int dst_width = src_width / 2;
    const int dst_height = src_height / 2;
    const int src_pitch = src_stride > 0 ? src_stride : src_width * 4;
    const int dst_pitch = dst_stride > 0 ? dst_stride : dst_width * 4;

    for (int y = 0; y < dst_height; ++y) {
        const std::uint8_t* row0 =
            src + static_cast<std::size_t>(2 * y) * static_cast<std::size_t>(src_pitch);
        const std::uint8_t* row1 = row0 + src_pitch;
        std::uint8_t* out = dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_pitch);
        for (int x = 0; x < dst_width; ++x) {
            const std::uint8_t* p00 = row0 + static_cast<std::size_t>(2 * x) * 4;
            const std::uint8_t* p01 = p00 + 4;
            const std::uint8_t* p10 = row1 + static_cast<std::size_t>(2 * x) * 4;
            const std::uint8_t* p11 = p10 + 4;
            for (int c = 0; c < 4; ++c) {
                const std::uint32_t sum = static_cast<std::uint32_t>(p00[c]) +
                                          static_cast<std::uint32_t>(p01[c]) +
                                          static_cast<std::uint32_t>(p10[c]) +
                                          static_cast<std::uint32_t>(p11[c]);
                out[static_cast<std::size_t>(x) * 4 + static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>((sum + 2) >> 2);
            }
        }
    }
}

}  // namespace go2cpp::media
