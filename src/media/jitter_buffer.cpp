/**
 * @file jitter_buffer.cpp
 * @brief `JitterBuffer` 的实现：环形槽位寻址、RFC 3550 抖动估计、深度自适应。
 * @details 热路径（`Push` 命中 `kAccepted`、`Pop` 命中 `kOk`）不含堆分配、不含
 *          锁、不含原子操作、不含虚调用，只有数组寻址 + 一次 `memcpy`。
 *          所有可变量都在构造期定型（容量 / 槽宽），运行期不增长。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/media/jitter_buffer.hpp"

#include <cstring>

namespace go2cpp::media {
namespace {

/// @brief 最小槽数（低于它没有重排意义）。
constexpr std::uint32_t kMinCapacity = 8;

/// @brief 槽内 64B 对齐（`memcpy` 与缓存行友好）。
constexpr std::size_t kSlotAlign = 64;

/// @brief 环槽数规范化为 2 的幂并 clamp 到 `[kMinCapacity, kMaxJitterCapacity]`。
std::uint32_t NormalizeCapacity(std::uint32_t value) noexcept {
    std::uint32_t capacity = kMinCapacity;
    while (capacity < value && capacity < kMaxJitterCapacity) {
        capacity <<= 1;
    }
    return capacity;
}

/// @brief 每槽负载容量：clamp 到 `[64B, kMaxJitterPayloadBytes]` 后向上 64B 对齐。
std::size_t NormalizeSlotBytes(std::size_t value) noexcept {
    std::size_t clamped = value < kSlotAlign ? kSlotAlign : value;
    if (clamped > kMaxJitterPayloadBytes) {
        clamped = kMaxJitterPayloadBytes;
    }
    return (clamped + kSlotAlign - 1) & ~(kSlotAlign - 1);
}

}  // namespace

JitterBuffer::JitterBuffer(const JitterBufferConfig& config) : m_config(config) {
    m_capacity = NormalizeCapacity(m_config.capacity);
    m_mask = m_capacity - 1;
    m_slot_bytes = NormalizeSlotBytes(m_config.max_payload_bytes);

    // 规范化配置：调用方传 0/非法值时也不会产生除零或负深度。
    m_config.capacity = m_capacity;
    m_config.max_payload_bytes = m_slot_bytes;
    if (m_config.timestamp_hz == 0) {
        m_config.timestamp_hz = 90000;
    }
    if (m_config.min_delay_ns < 0) {
        m_config.min_delay_ns = 0;
    }
    if (m_config.max_delay_ns < m_config.min_delay_ns) {
        m_config.max_delay_ns = m_config.min_delay_ns;
    }
    if (m_config.base_delay_ns < m_config.min_delay_ns) {
        m_config.base_delay_ns = m_config.min_delay_ns;
    }
    if (m_config.base_delay_ns > m_config.max_delay_ns) {
        m_config.base_delay_ns = m_config.max_delay_ns;
    }
    if (m_config.jitter_factor_q8 == 0) {
        m_config.jitter_factor_q8 = 256;  // Q8：256 = 1.0x
    }
    // 时间戳 -> 纳秒的 Q16 定点比例：构造期算一次，热路径只做乘法+移位。
    m_ts_scale_q16 =
        (1'000'000'000LL << 16) / static_cast<std::int64_t>(m_config.timestamp_hz);

    // 构造期一次性分配：之后无论收多少包都不再申请内存。
    m_meta.reset(new SlotMeta[m_capacity]());
    m_pool.reset(new std::uint8_t[static_cast<std::size_t>(m_capacity) * m_slot_bytes]);
    if (m_config.deliver_late) {
        m_late_meta.reset(new LateMeta[kJitterLateSlots]());
        m_late_pool.reset(
            new std::uint8_t[static_cast<std::size_t>(kJitterLateSlots) * m_slot_bytes]);
    }
    m_target_delay_ns = m_config.base_delay_ns;
}

JitterBuffer::~JitterBuffer() = default;

PushResult JitterBuffer::Push(std::uint32_t seq, std::uint32_t timestamp,
                              const std::uint8_t* payload, std::size_t bytes,
                              std::int64_t arrival_ns) noexcept {
    if (payload == nullptr || bytes == 0 || bytes > m_slot_bytes) {
        ++m_stats.dropped_invalid;
        return PushResult::kDroppedInvalid;
    }

    if (!m_anchored) {
        m_anchored = true;
        m_next_seq = seq;
        m_highest_seq = seq;
        m_hole_deadline_ns = 0;
    }
    if (!m_has_first_arrival) {
        m_has_first_arrival = true;
        m_first_arrival_ns = arrival_ns;
    }
    ++m_stats.received;

    std::int32_t ahead = static_cast<std::int32_t>(seq - m_next_seq);
    if (ahead < 0) {
        // 序号已经滑过交付点：迟到。
        const std::uint32_t late_by = static_cast<std::uint32_t>(-ahead);
        if (!m_delivered_any && late_by <= m_config.late_tolerance) {
            // 首帧尚未交付：向前回锚，吸收「第一个包本身就乱序」的启动期问题。
            m_next_seq = seq;
            ahead = 0;
        } else if (m_config.deliver_late && late_by <= m_config.late_tolerance &&
                   m_late_count < kJitterLateSlots) {
            PushLate(seq, timestamp, payload, bytes, arrival_ns);
            return PushResult::kLateQueued;
        } else {
            // 视频默认路径：解码器按参考帧顺序工作，乱序补发会让它重置，故丢弃。
            ++m_stats.dropped_late;
            return PushResult::kDroppedLate;
        }
    }

    bool discontinuity = false;
    if (ahead > 0 && static_cast<std::uint32_t>(ahead) >= m_capacity) {
        // 跳变超过整个窗口：对端重启或长时间断流，重锚而不是永久追赶。
        ++m_stats.discontinuities;
        ResetTo(seq);
        ahead = 0;
        discontinuity = true;
    }

    const std::size_t idx = static_cast<std::size_t>(seq) & m_mask;
    SlotMeta* meta = m_meta.get() + idx;
    if (meta->occupied != 0) {
        if (meta->seq == seq) {
            ++m_stats.duplicates;
            return PushResult::kDuplicate;
        }
        // 同槽但序号不同 = 窗口已滑过一整圈，旧包被新包覆盖。
        ++m_stats.overwrites;
        --m_buffered;
    }

    UpdateJitter(timestamp, arrival_ns);

    std::memcpy(m_pool.get() + idx * m_slot_bytes, payload, bytes);
    meta->seq = seq;
    meta->timestamp = timestamp;
    meta->bytes = static_cast<std::uint32_t>(bytes);
    meta->occupied = 1;
    meta->arrival_ns = arrival_ns;
    ++m_buffered;
    ++m_stats.accepted;
    if (m_buffered > m_stats.max_buffered) {
        m_stats.max_buffered = m_buffered;
    }

    if (ahead <= 0) {
        // 当前空洞被填上，取消等待计时。
        m_hole_deadline_ns = 0;
    }
    if (static_cast<std::int32_t>(seq - m_highest_seq) > 0) {
        m_highest_seq = seq;
    } else if (seq != m_highest_seq) {
        // 乱序到达但落在窗口内：会被 Pop 按序吐出，这里只计数。
        ++m_stats.reordered;
    }
    return discontinuity ? PushResult::kDiscontinuity : PushResult::kAccepted;
}

PopResult JitterBuffer::Pop(JitterPacketView* out, std::int64_t now_ns) noexcept {
    return PopInternal(out, now_ns, false);
}

PopResult JitterBuffer::PopFlush(JitterPacketView* out, std::int64_t now_ns) noexcept {
    return PopInternal(out, now_ns, true);
}

PopResult JitterBuffer::PopInternal(JitterPacketView* out, std::int64_t now_ns,
                                    bool flush) noexcept {
    if (out == nullptr) {
        return PopResult::kEmpty;
    }

    // 迟到补发优先（仅 deliver_late 时可能非空；常规路径只多一次比较）。
    if (m_late_count != 0) {
        const std::size_t slot = static_cast<std::size_t>(m_late_head);
        const LateMeta* late = m_late_meta.get() + slot;
        out->seq = late->seq;
        out->timestamp = late->timestamp;
        out->data = m_late_pool.get() + slot * m_slot_bytes;
        out->bytes = late->bytes;
        out->arrival_ns = late->arrival_ns;
        out->held_ns = now_ns - late->arrival_ns;
        out->late = true;
        m_late_head = (m_late_head + 1) % kJitterLateSlots;
        --m_late_count;
        ++m_stats.late_delivered;
        ++m_stats.delivered;
        return PopResult::kLateDelivered;
    }

    if (m_buffered == 0) {
        return PopResult::kEmpty;
    }

    // 启动缓冲：首帧交付前先攒够 target_delay，让启动期的乱序有机会补上。
    if (!flush && m_config.startup_buffering && !m_delivered_any && m_has_first_arrival) {
        if (now_ns < m_first_arrival_ns + m_target_delay_ns) {
            return PopResult::kEmpty;
        }
    }

    bool skipped = false;
    for (std::uint32_t spins = 0; spins <= m_capacity; ++spins) {
        const std::size_t idx = static_cast<std::size_t>(m_next_seq) & m_mask;
        SlotMeta* meta = m_meta.get() + idx;
        if (meta->occupied != 0 && meta->seq == m_next_seq) {
            out->seq = meta->seq;
            out->timestamp = meta->timestamp;
            out->data = m_pool.get() + idx * m_slot_bytes;
            out->bytes = meta->bytes;
            out->arrival_ns = meta->arrival_ns;
            out->held_ns = now_ns - meta->arrival_ns;
            out->late = false;
            meta->occupied = 0;
            --m_buffered;
            ++m_next_seq;
            ++m_stats.delivered;
            m_delivered_any = true;
            m_hole_deadline_ns = 0;
            return PopResult::kOk;
        }

        // 当前序号缺失：只有「已经收到更晚的包」才说明这是空洞，否则只是还没到。
        if (static_cast<std::int32_t>(m_highest_seq - m_next_seq) <= 0) {
            return skipped ? PopResult::kGapSkipped : PopResult::kEmpty;
        }

        if (!flush && !skipped) {
            // 每个空洞单独计时：next_seq 变了就重新开始等。
            // 一旦本次调用已经判超时跳过（`skipped`），后续空洞属于同一次饥饿事件，
            // 直接连续跳过——否则「连丢 10 个包」要花 10 个渲染 tick 才能追上。
            if (m_hole_deadline_ns == 0 || m_hole_seq != m_next_seq) {
                m_hole_seq = m_next_seq;
                m_hole_deadline_ns = now_ns + m_target_delay_ns;
            }
            if (now_ns < m_hole_deadline_ns) {
                return PopResult::kEmpty;
            }
        }

        // 超过等待窗口（或显式排空）：判定该序号丢失并跳过。
        ++m_stats.gap_events;
        ++m_stats.lost_packets;
        ++m_next_seq;
        m_hole_deadline_ns = 0;
        skipped = true;
    }
    return skipped ? PopResult::kGapSkipped : PopResult::kEmpty;
}

void JitterBuffer::Reset() noexcept {
    std::memset(m_meta.get(), 0, sizeof(SlotMeta) * m_capacity);
    m_buffered = 0;
    m_anchored = false;
    m_has_first_arrival = false;
    m_has_timing = false;
    m_delivered_any = false;
    m_next_seq = 0;
    m_highest_seq = 0;
    m_hole_seq = 0;
    m_hole_deadline_ns = 0;
    m_jitter_ns = 0;
    m_prev_arrival_ns = 0;
    m_prev_timestamp = 0;
    m_first_arrival_ns = 0;
    m_late_head = 0;
    m_late_count = 0;
    m_target_delay_ns = m_config.base_delay_ns;
    m_stats = JitterStats{};
}

void JitterBuffer::ResetTo(std::uint32_t seq) noexcept {
    std::memset(m_meta.get(), 0, sizeof(SlotMeta) * m_capacity);
    m_buffered = 0;
    m_next_seq = seq;
    m_highest_seq = seq;
    m_hole_seq = 0;
    m_hole_deadline_ns = 0;
    m_delivered_any = false;
    m_has_first_arrival = false;
    m_first_arrival_ns = 0;
    m_late_head = 0;
    m_late_count = 0;
}

void JitterBuffer::UpdateJitter(std::uint32_t timestamp, std::int64_t arrival_ns) noexcept {
    if (!m_has_timing) {
        m_has_timing = true;
        m_prev_timestamp = timestamp;
        m_prev_arrival_ns = arrival_ns;
        return;
    }

    // RFC 3550 的 D：到达时间差减媒体时间戳差。`int32_t` 差值天然处理 32 位回绕。
    const std::int64_t arrival_delta = arrival_ns - m_prev_arrival_ns;
    const std::int32_t ts_delta = static_cast<std::int32_t>(timestamp - m_prev_timestamp);
    // Q16 定点换算（构造期已算好 1e9/hz），避免运行时 64 位除法。
    const std::int64_t media_delta =
        (static_cast<std::int64_t>(ts_delta) * m_ts_scale_q16) >> 16;
    std::int64_t deviation = arrival_delta - media_delta;
    if (deviation < 0) {
        deviation = -deviation;
    }

    // J += (|D| - J) / 16：整数除法（编译器展开为 3-4 条指令），无浮点、无实现定义行为。
    m_jitter_ns += (deviation - m_jitter_ns) / 16;
    if (m_jitter_ns < 0) {
        m_jitter_ns = 0;
    }
    if (m_jitter_ns > m_config.max_delay_ns) {
        m_jitter_ns = m_config.max_delay_ns;
    }

    m_prev_timestamp = timestamp;
    m_prev_arrival_ns = arrival_ns;
    UpdateDepth();
}

void JitterBuffer::UpdateDepth() noexcept {
    if (!m_config.adaptive) {
        m_target_delay_ns = m_config.base_delay_ns;
        return;
    }
    // Q8 定点：target = base + J * factor。
    const std::int64_t scaled =
        (m_jitter_ns * static_cast<std::int64_t>(m_config.jitter_factor_q8)) >> 8;
    std::int64_t target = m_config.base_delay_ns + scaled;
    if (target < m_config.min_delay_ns) {
        target = m_config.min_delay_ns;
    }
    if (target > m_config.max_delay_ns) {
        target = m_config.max_delay_ns;
    }
    m_target_delay_ns = target;
}

void JitterBuffer::PushLate(std::uint32_t seq, std::uint32_t timestamp,
                            const std::uint8_t* payload, std::size_t bytes,
                            std::int64_t arrival_ns) noexcept {
    const std::size_t slot =
        (static_cast<std::size_t>(m_late_head) + m_late_count) % kJitterLateSlots;
    LateMeta* late = m_late_meta.get() + slot;
    std::memcpy(m_late_pool.get() + slot * m_slot_bytes, payload, bytes);
    late->seq = seq;
    late->timestamp = timestamp;
    late->bytes = static_cast<std::uint32_t>(bytes);
    late->arrival_ns = arrival_ns;
    ++m_late_count;
}

std::size_t JitterBuffer::allocated_bytes() const noexcept {
    std::size_t total = sizeof(SlotMeta) * m_capacity;
    total += static_cast<std::size_t>(m_capacity) * m_slot_bytes;
    if (m_late_meta) {
        total += sizeof(LateMeta) * kJitterLateSlots;
        total += static_cast<std::size_t>(kJitterLateSlots) * m_slot_bytes;
    }
    return total;
}

JitterStats JitterBuffer::Snapshot() const noexcept {
    JitterStats snapshot = m_stats;
    snapshot.capacity = m_capacity;
    snapshot.buffered = m_buffered;
    snapshot.target_delay_ns = m_target_delay_ns;
    snapshot.jitter_ns = m_jitter_ns;
    return snapshot;
}

}  // namespace go2cpp::media
