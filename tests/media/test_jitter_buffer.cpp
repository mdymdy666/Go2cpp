/**
 * @file test_jitter_buffer.cpp
 * @brief `JitterBuffer` 的功能、随机与压测：顺序/乱序/重复/丢包/迟到/断流/
 *        32 位回绕/抖动模拟（±50ms，1e5 帧）/1e6 帧吞吐/内存有界/零分配。
 * @details 全部用例都是确定性的（固定种子的 `mt19937` 或纯整数序列），
 *          因此失败可精确复现；时间用模拟纳秒时钟，不 sleep。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "go2cpp/media/jitter_buffer.hpp"
#include "go2cpp/media/stats.hpp"
#include "media_test_util.hpp"

#if defined(__GLIBC__)
#include <malloc.h>
#endif

// `media_test_util.hpp` 提供 CHECK / CHECK_EQ / CHECK_GE；这里补一个 LE（语义相同：
// 不中断、计一次检查），供「上界」断言使用。
#define MEDIA_CHECK_LE(actual, bound) MEDIA_CHECK((actual) <= (bound))

namespace {

using namespace go2cpp::media;

using Clock = std::chrono::steady_clock;

/// @brief 1 毫秒对应的纳秒数。
constexpr std::int64_t kMsNs = 1'000'000;

/// @brief 30fps 的帧间隔（纳秒）。
constexpr std::int64_t kFrameNs = 33'333'333;

/// @brief 从某个时间点起的纳秒耗时。
double NsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

/// @brief 当前进程已分配的堆字节数（glibc；其他平台返回 0）。
std::size_t HeapInUse() {
#if defined(__GLIBC__)
    return mallinfo2().uordblks;
#else
    return 0;
#endif
}

/// @brief 构造一段可校验负载：前 4 字节是小端 seq。
std::vector<std::uint8_t> MakePayload(std::uint32_t seq, std::size_t bytes) {
    std::vector<std::uint8_t> data(bytes, 0);
    if (bytes >= 4) {
        std::memcpy(data.data(), &seq, 4);
    }
    for (std::size_t i = 4; i < bytes; ++i) {
        data[i] = static_cast<std::uint8_t>((seq * 7U + i) & 0xFFU);
    }
    return data;
}

/// @brief 负载前 4 字节解出的 seq。
std::uint32_t PayloadSeq(const JitterPacketView& view) {
    std::uint32_t seq = 0;
    if (view.bytes >= 4 && view.data != nullptr) {
        std::memcpy(&seq, view.data, 4);
    }
    return seq;
}

// ---------------------------------------------------------------------------
// 场景 1：顺序流（基线）
// ---------------------------------------------------------------------------
void TestInOrder() {
    JitterBufferConfig config;
    config.startup_buffering = false;
    JitterBuffer buffer(config);
    MEDIA_CHECK_EQ(buffer.capacity(), 512U);

    constexpr std::uint32_t kFrames = 2000;
    const std::vector<std::uint8_t> payload = MakePayload(0, 1200);
    std::uint32_t expected = 0;
    std::uint64_t delivered = 0;

    for (std::uint32_t i = 0; i < kFrames; ++i) {
        const std::int64_t now = static_cast<std::int64_t>(i) * kFrameNs;
        MEDIA_CHECK(buffer.Push(i, i * 3000U, payload.data(), payload.size(), now) ==
                    PushResult::kAccepted);
        JitterPacketView view;
        while (buffer.Pop(&view, now) == PopResult::kOk) {
            MEDIA_CHECK_EQ(view.seq, expected);
            MEDIA_CHECK(!view.late);
            expected = view.seq + 1;
            ++delivered;
        }
    }
    MEDIA_CHECK_EQ(delivered, static_cast<std::uint64_t>(kFrames));
    MEDIA_CHECK_EQ(buffer.buffered(), 0U);

    const JitterStats stats = buffer.Snapshot();
    MEDIA_CHECK_EQ(stats.received, static_cast<std::uint64_t>(kFrames));
    MEDIA_CHECK_EQ(stats.delivered, static_cast<std::uint64_t>(kFrames));
    MEDIA_CHECK_EQ(stats.duplicates, 0U);
    MEDIA_CHECK_EQ(stats.dropped_late, 0U);
    MEDIA_CHECK_EQ(stats.lost_packets, 0U);
    MEDIA_CHECK_EQ(stats.gap_events, 0U);
    std::printf("  [in-order] %u frames, target=%.2fms jitter=%.2fms\n",
                static_cast<unsigned>(kFrames), static_cast<double>(stats.target_delay_ns) / 1e6,
                static_cast<double>(stats.jitter_ns) / 1e6);
}

// ---------------------------------------------------------------------------
// 场景 2：负载完整性（不同长度 93..1488B，逆序到达，零拷贝内容逐字节一致）
// ---------------------------------------------------------------------------
void TestPayloadIntegrity() {
    JitterBufferConfig config;
    JitterBuffer buffer(config);

    constexpr int kCount = 16;
    std::vector<std::vector<std::uint8_t>> payloads;
    payloads.reserve(kCount);
    std::size_t total = 0;
    for (int i = 0; i < kCount; ++i) {
        payloads.push_back(MakePayload(static_cast<std::uint32_t>(i),
                                       static_cast<std::size_t>(i + 1) * 93));
        total += payloads.back().size();
    }
    MEDIA_CHECK_GE(total, 7000U);

    const std::int64_t now = 0;
    // 完全逆序到达：15, 14, ... 0。
    for (int i = kCount - 1; i >= 0; --i) {
        MEDIA_CHECK(buffer.Push(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i) * 3000U,
                                payloads[static_cast<std::size_t>(i)].data(),
                                payloads[static_cast<std::size_t>(i)].size(),
                                0) == PushResult::kAccepted);
    }
    const std::int64_t later = now + 200 * kMsNs;
    for (int i = 0; i < kCount; ++i) {
        JitterPacketView view;
        MEDIA_CHECK(buffer.Pop(&view, later) == PopResult::kOk);
        MEDIA_CHECK_EQ(view.seq, static_cast<std::uint32_t>(i));
        MEDIA_CHECK_EQ(view.bytes, payloads[static_cast<std::size_t>(i)].size());
        MEDIA_CHECK(std::memcmp(view.data, payloads[static_cast<std::size_t>(i)].data(),
                                view.bytes) == 0);
        MEDIA_CHECK_EQ(PayloadSeq(view), static_cast<std::uint32_t>(i));
    }
    std::printf("  [payload] %d sizes 93..1488B, reverse arrival -> byte-identical\n", kCount);
}

// ---------------------------------------------------------------------------
// 场景 3：窗口内乱序（块内 32 帧 Fisher-Yates 打乱，2000 帧全部按序交付）
// ---------------------------------------------------------------------------
void TestReorder() {
    JitterBufferConfig config;
    config.startup_buffering = false;
    config.capacity = 256;
    JitterBuffer buffer(config);

    constexpr std::uint32_t kFrames = 2048;
    constexpr std::uint32_t kBlock = 32;
    static_assert(kFrames % kBlock == 0, "块必须整除帧数");

    const std::vector<std::uint8_t> payload = MakePayload(1, 1200);
    std::uint32_t expected = 0;
    std::uint64_t delivered = 0;
    std::int64_t now = 0;
    std::uint32_t state = 0x12345678U;

    for (std::uint32_t base = 0; base < kFrames; base += kBlock) {
        std::uint32_t order[kBlock];
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            order[i] = base + i;
        }
        // Fisher-Yates（LCG 代替 std::shuffle，保证跨标准库实现可复现）。
        for (std::uint32_t i = kBlock - 1; i > 0; --i) {
            state = state * 1664525U + 1013904223U;
            const std::uint32_t j = state % (i + 1);
            const std::uint32_t tmp = order[i];
            order[i] = order[j];
            order[j] = tmp;
        }
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            const std::uint32_t seq = order[i];
            MEDIA_CHECK(buffer.Push(seq, seq * 3000U, payload.data(), payload.size(), now) ==
                        PushResult::kAccepted);
        }
        now += 40 * kMsNs;
        JitterPacketView view;
        for (;;) {
            const PopResult result = buffer.Pop(&view, now);
            if (result != PopResult::kOk) {
                break;
            }
            MEDIA_CHECK_EQ(view.seq, expected);
            expected = view.seq + 1;
            ++delivered;
        }
    }

    MEDIA_CHECK_EQ(delivered, static_cast<std::uint64_t>(kFrames));
    const JitterStats stats = buffer.Snapshot();
    MEDIA_CHECK_EQ(stats.dropped_late, 0U);
    MEDIA_CHECK_EQ(stats.lost_packets, 0U);
    MEDIA_CHECK_EQ(stats.discontinuities, 0U);
    MEDIA_CHECK_GE(stats.reordered, 500U);
    MEDIA_CHECK_GE(stats.max_buffered, 8U);
    std::printf("  [reorder] %u frames shuffled in blocks of %u, delivered=%llu "
                "reordered=%llu max_buffered=%llu\n",
                static_cast<unsigned>(kFrames), static_cast<unsigned>(kBlock),
                static_cast<unsigned long long>(delivered),
                static_cast<unsigned long long>(stats.reordered),
                static_cast<unsigned long long>(stats.max_buffered));
}

// ---------------------------------------------------------------------------
// 场景 4：重复包（每包发 3 次，只交付一次）
// ---------------------------------------------------------------------------
void TestDuplicate() {
    JitterBufferConfig config;
    config.startup_buffering = false;
    JitterBuffer buffer(config);

    constexpr std::uint32_t kFrames = 500;
    constexpr std::uint32_t kCopies = 3;
    const std::vector<std::uint8_t> payload = MakePayload(2, 1200);
    std::uint32_t expected = 0;
    std::uint64_t delivered = 0;

    for (std::uint32_t i = 0; i < kFrames; ++i) {
        const std::int64_t now = static_cast<std::int64_t>(i) * kFrameNs;
        for (std::uint32_t copy = 0; copy < kCopies; ++copy) {
            const PushResult result =
                buffer.Push(i, i * 3000U, payload.data(), payload.size(), now + copy);
            if (copy == 0) {
                MEDIA_CHECK(result == PushResult::kAccepted);
            } else {
                MEDIA_CHECK(result == PushResult::kDuplicate);
            }
        }
        JitterPacketView view;
        while (buffer.Pop(&view, now) == PopResult::kOk) {
            MEDIA_CHECK_EQ(view.seq, expected);
            expected = view.seq + 1;
            ++delivered;
        }
    }
    MEDIA_CHECK_EQ(delivered, static_cast<std::uint64_t>(kFrames));
    const JitterStats stats = buffer.Snapshot();
    MEDIA_CHECK_EQ(stats.duplicates, static_cast<std::uint64_t>(kFrames) * (kCopies - 1));
    std::printf("  [duplicate] %u frames x%u copies -> delivered=%llu duplicates=%llu\n",
                static_cast<unsigned>(kFrames), kCopies,
                static_cast<unsigned long long>(delivered),
                static_cast<unsigned long long>(stats.duplicates));
}

// ---------------------------------------------------------------------------
// 场景 5：随机丢包 5%（丢的包被检测为空洞，其余全部按序交付）
// ---------------------------------------------------------------------------
void TestLoss() {
    JitterBufferConfig config;
    config.startup_buffering = false;
    JitterBuffer buffer(config);

    constexpr std::uint32_t kFrames = 2000;
    constexpr std::uint32_t kDropEvery = 20;  // 5%
    const std::vector<std::uint8_t> payload = MakePayload(3, 1200);
    std::uint32_t expected = 0;
    std::uint64_t delivered = 0;
    std::uint64_t dropped = 0;

    for (std::uint32_t i = 0; i < kFrames; ++i) {
        const std::int64_t now = static_cast<std::int64_t>(i) * kFrameNs;
        if ((i % kDropEvery) != (kDropEvery - 1)) {
            buffer.Push(i, i * 3000U, payload.data(), payload.size(), now);
        } else {
            ++dropped;
        }
        JitterPacketView view;
        for (;;) {
            const PopResult result = buffer.Pop(&view, now);
            if (result != PopResult::kOk) {
                break;
            }
            // 丢包会让交付序号跳跃：断言「单调且跳过的一定是被丢的」，
            // 不能断言 view.seq 与一个连续 expected 相等。
            MEDIA_CHECK_GE(view.seq, expected);
            MEDIA_CHECK((view.seq % kDropEvery) != (kDropEvery - 1));
            expected = view.seq + 1;
            ++delivered;
        }
    }
    MEDIA_CHECK_EQ(delivered + dropped, static_cast<std::uint64_t>(kFrames));
    const JitterStats stats = buffer.Snapshot();
    // 最后一个被丢的包后面没有更晚的包，永远不会被判定为空洞，因此允许差 1。
    MEDIA_CHECK_GE(stats.lost_packets, dropped > 0 ? dropped - 1 : 0);
    MEDIA_CHECK_LE(stats.lost_packets, dropped);
    MEDIA_CHECK_GE(stats.gap_events, dropped > 0 ? dropped - 1 : 0);
    std::printf("  [loss] %u frames, dropped=%llu delivered=%llu lost_packets=%llu gaps=%llu\n",
                static_cast<unsigned>(kFrames), static_cast<unsigned long long>(dropped),
                static_cast<unsigned long long>(delivered),
                static_cast<unsigned long long>(stats.lost_packets),
                static_cast<unsigned long long>(stats.gap_events));
}

// ---------------------------------------------------------------------------
// 场景 6：连续丢 10 个（一次跳过整段空洞，不需要 10 个 tick）
// ---------------------------------------------------------------------------
void TestBurstLoss() {
    JitterBufferConfig config;
    config.startup_buffering = false;
    JitterBuffer buffer(config);

    const std::vector<std::uint8_t> payload = MakePayload(4, 1200);
    // 先投 10 个正常包并取走。
    for (std::uint32_t i = 0; i < 10; ++i) {
        const std::int64_t now = static_cast<std::int64_t>(i) * kFrameNs;
        buffer.Push(i, i * 3000U, payload.data(), payload.size(), now);
        JitterPacketView view;
        while (buffer.Pop(&view, now) == PopResult::kOk) {
        }
    }
    // 序号 10..19 全丢，直接发 20（时间必须晚于前 10 个包，避免时钟倒退）。
    const std::int64_t now = 400 * kMsNs;
    MEDIA_CHECK(buffer.Push(20, 20 * 3000U, payload.data(), payload.size(), now) ==
                PushResult::kAccepted);
    // 第一次 Pop：空洞未超时 -> kEmpty。
    JitterPacketView view;
    MEDIA_CHECK(buffer.Pop(&view, now) == PopResult::kEmpty);
    // 超时后一次调用应连续跳过 10 个空洞并交付 seq=20。
    const std::int64_t later = now + 100 * kMsNs;
    const PopResult result = buffer.Pop(&view, later);
    MEDIA_CHECK(result == PopResult::kOk);
    MEDIA_CHECK_EQ(view.seq, 20U);
    const JitterStats stats = buffer.Snapshot();
    MEDIA_CHECK_EQ(stats.lost_packets, 10U);
    MEDIA_CHECK_EQ(stats.gap_events, 10U);
    std::printf("  [burst-loss] 10 consecutive gaps skipped in one Pop, lost=%llu\n",
                static_cast<unsigned long long>(stats.lost_packets));
}

// ---------------------------------------------------------------------------
// 场景 7：迟到帧（默认丢弃 / deliver_late 补发）
// ---------------------------------------------------------------------------
void TestLate() {
    const std::vector<std::uint8_t> payload = MakePayload(5, 1200);

    {
        JitterBufferConfig config;
        config.startup_buffering = false;
        config.capacity = 64;
        JitterBuffer buffer(config);
        // 先交付 seq 0..4。
        for (std::uint32_t i = 0; i < 5; ++i) {
            const std::int64_t now = static_cast<std::int64_t>(i) * kFrameNs;
            buffer.Push(i, i * 3000U, payload.data(), payload.size(), now);
            JitterPacketView view;
            while (buffer.Pop(&view, now) == PopResult::kOk) {
            }
        }
        // seq 2 迟到（已经交付过）。
        const PushResult result = buffer.Push(2, 2 * 3000U, payload.data(), payload.size(), 200);
        MEDIA_CHECK(result == PushResult::kDroppedLate);
        MEDIA_CHECK_EQ(buffer.Snapshot().dropped_late, 1U);
        // 超窗口的迟到（差值 > late_tolerance=64）同样丢弃。
        MEDIA_CHECK(buffer.Push(0xFFFFF000U, 0, payload.data(), payload.size(), 200) ==
                    PushResult::kDroppedLate);
        MEDIA_CHECK_EQ(buffer.Snapshot().dropped_late, 2U);
        std::printf("  [late] drop policy: dropped_late=%llu\n",
                    static_cast<unsigned long long>(buffer.Snapshot().dropped_late));
    }

    {
        JitterBufferConfig config;
        config.startup_buffering = false;
        config.capacity = 64;
        config.deliver_late = true;
        JitterBuffer buffer(config);
        for (std::uint32_t i = 0; i < 5; ++i) {
            const std::int64_t now = static_cast<std::int64_t>(i) * kFrameNs;
            buffer.Push(i, i * 3000U, payload.data(), payload.size(), now);
            JitterPacketView view;
            while (buffer.Pop(&view, now) == PopResult::kOk) {
            }
        }
        const std::int64_t now = 10 * kMsNs;
        MEDIA_CHECK(buffer.Push(3, 3 * 3000U, payload.data(), payload.size(), now) ==
                    PushResult::kLateQueued);
        JitterPacketView view;
        MEDIA_CHECK(buffer.Pop(&view, now) == PopResult::kLateDelivered);
        MEDIA_CHECK_EQ(view.seq, 3U);
        MEDIA_CHECK(view.late);
        MEDIA_CHECK_EQ(view.bytes, payload.size());
        MEDIA_CHECK_EQ(buffer.Snapshot().late_delivered, 1U);
        MEDIA_CHECK_GE(buffer.allocated_bytes(), 64U * 1536U + 8U * 1536U);
        std::printf("  [late] deliver policy: late_delivered=%llu bytes=%zu\n",
                    static_cast<unsigned long long>(buffer.Snapshot().late_delivered),
                    buffer.allocated_bytes());
    }
}

// ---------------------------------------------------------------------------
// 场景 8：断流重锚（序号跳变超过容量）
// ---------------------------------------------------------------------------
void TestDiscontinuity() {
    JitterBufferConfig config;
    config.startup_buffering = false;
    config.capacity = 128;
    JitterBuffer buffer(config);
    const std::vector<std::uint8_t> payload = MakePayload(6, 1200);

    for (std::uint32_t i = 0; i < 10; ++i) {
        const std::int64_t now = static_cast<std::int64_t>(i) * kFrameNs;
        buffer.Push(i, i * 3000U, payload.data(), payload.size(), now);
        JitterPacketView view;
        while (buffer.Pop(&view, now) == PopResult::kOk) {
        }
    }
    // 跳到 5000：差值 4990 >= capacity(128) -> 判定流重启。
    const PushResult jump = buffer.Push(5000, 5000 * 3000U, payload.data(), payload.size(),
                                        static_cast<std::int64_t>(20 * kFrameNs));
    MEDIA_CHECK(jump == PushResult::kDiscontinuity);
    MEDIA_CHECK_EQ(buffer.next_seq(), 5000U);
    MEDIA_CHECK_EQ(buffer.Snapshot().discontinuities, 1U);
    // 重置后继续按序交付。
    JitterPacketView view;
    MEDIA_CHECK(buffer.Pop(&view, 100 * kMsNs) == PopResult::kOk);
    MEDIA_CHECK_EQ(view.seq, 5000U);
    std::printf("  [discontinuity] jump 9 -> 5000 detected, next_seq=%u\n", buffer.next_seq());
}

// ---------------------------------------------------------------------------
// 场景 9：32 位序号回绕
// ---------------------------------------------------------------------------
void TestWrapAround() {
    JitterBufferConfig config;
    config.startup_buffering = false;
    JitterBuffer buffer(config);
    const std::vector<std::uint8_t> payload = MakePayload(7, 1200);

    constexpr std::uint32_t kStart = 0xFFFFFF00U;
    constexpr std::uint32_t kFrames = 600;  // 跨越 0xFFFFFFFF -> 0x00000000
    std::uint32_t expected = kStart;
    std::uint64_t delivered = 0;

    for (std::uint32_t i = 0; i < kFrames; ++i) {
        const std::uint32_t seq = kStart + i;  // 自然回绕
        const std::int64_t now = static_cast<std::int64_t>(i) * kFrameNs;
        MEDIA_CHECK(buffer.Push(seq, seq * 3000U, payload.data(), payload.size(), now) ==
                    PushResult::kAccepted);
        JitterPacketView view;
        while (buffer.Pop(&view, now) == PopResult::kOk) {
            MEDIA_CHECK_EQ(view.seq, expected);
            expected = view.seq + 1;
            ++delivered;
        }
    }
    MEDIA_CHECK_EQ(delivered, static_cast<std::uint64_t>(kFrames));
    MEDIA_CHECK_EQ(buffer.Snapshot().discontinuities, 0U);
    std::printf("  [wrap] %u frames from 0x%08X, delivered=%llu, no false discontinuity\n",
                static_cast<unsigned>(kFrames), kStart,
                static_cast<unsigned long long>(delivered));
}

// ---------------------------------------------------------------------------
// 场景 10：非法输入与配置规范化
// ---------------------------------------------------------------------------
void TestInvalid() {
    {
        JitterBufferConfig config;
        config.capacity = 0;           // -> 8
        config.max_payload_bytes = 0;  // -> 64
        config.timestamp_hz = 0;       // -> 90000
        config.min_delay_ns = 5 * kMsNs;
        config.max_delay_ns = 1 * kMsNs;  // < min -> 被抬到 min
        config.base_delay_ns = 0;         // -> min
        JitterBuffer buffer(config);
        MEDIA_CHECK_EQ(buffer.capacity(), 8U);
        MEDIA_CHECK_GE(buffer.target_delay_ns(), 5 * kMsNs);
        MEDIA_CHECK_GE(buffer.allocated_bytes(), 8U * 64U);
        std::printf("  [invalid] normalized capacity=%u target=%.2fms bytes=%zu\n",
                    buffer.capacity(), static_cast<double>(buffer.target_delay_ns()) / 1e6,
                    buffer.allocated_bytes());
    }
    {
        JitterBufferConfig config;
        JitterBuffer buffer(config);
        const std::uint8_t byte = 0;
        MEDIA_CHECK(buffer.Push(0, 0, nullptr, 10, 0) == PushResult::kDroppedInvalid);
        MEDIA_CHECK(buffer.Push(0, 0, &byte, 0, 0) == PushResult::kDroppedInvalid);
        std::vector<std::uint8_t> huge(70000, 0);
        MEDIA_CHECK(buffer.Push(0, 0, huge.data(), huge.size(), 0) == PushResult::kDroppedInvalid);
        MEDIA_CHECK_EQ(buffer.Snapshot().dropped_invalid, 3U);
        // 空缓冲 Pop 必须是 kEmpty（不能崩、不能返回脏 view）。
        JitterPacketView view;
        MEDIA_CHECK(buffer.Pop(&view, 0) == PopResult::kEmpty);
        MEDIA_CHECK(buffer.Pop(nullptr, 0) == PopResult::kEmpty);
        // 时间倒退不应触发异常交付。
        buffer.Push(1, 1, &byte, 1, 1000);
        MEDIA_CHECK(buffer.Pop(&view, 500) == PopResult::kEmpty);
        MEDIA_CHECK(buffer.PopFlush(&view, 500) == PopResult::kOk);
        std::printf("  [invalid] invalid payloads rejected, empty/regress safe\n");
    }
}

// ---------------------------------------------------------------------------
// 场景 11：抖动模拟 ±50ms（1e5 帧，乱序到达，自适应深度）
// ---------------------------------------------------------------------------
void TestJitter() {
    JitterBufferConfig config;
    config.capacity = 512;
    config.startup_buffering = true;
    JitterBuffer buffer(config);

    constexpr std::uint32_t kFrames = 100'000;
    constexpr std::int64_t kJitterNs = 50 * kMsNs;

    struct Arrival {
        std::int64_t arrive_ns;
        std::uint32_t seq;
        std::uint32_t timestamp;
    };

    std::mt19937 rng(20261101U);
    std::uniform_int_distribution<std::int64_t> jitter(-kJitterNs, kJitterNs);
    std::vector<Arrival> arrivals(kFrames);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        const std::int64_t send = static_cast<std::int64_t>(i) * kFrameNs;
        std::int64_t arrive = send + jitter(rng);
        if (arrive < 0) {
            arrive = 0;
        }
        arrivals[i] = Arrival{arrive, i, i * 3000U};
    }
    std::sort(arrivals.begin(), arrivals.end(), [](const Arrival& a, const Arrival& b) {
        return a.arrive_ns < b.arrive_ns;
    });

    std::vector<std::uint8_t> payload(1200, 0xA5);
    LatencyRecorder held;
    std::uint32_t cursor = 0;
    std::uint32_t expected = 0;
    std::uint64_t delivered = 0;
    std::uint64_t violations = 0;
    std::uint64_t late_drops = 0;
    const std::uint32_t ticks = kFrames + 400;

    for (std::uint32_t tick = 0; tick < ticks; ++tick) {
        const std::int64_t render = static_cast<std::int64_t>(tick) * kFrameNs;
        while (cursor < kFrames && arrivals[cursor].arrive_ns <= render) {
            const Arrival& arrival = arrivals[cursor];
            ++cursor;
            if (buffer.Push(arrival.seq, arrival.timestamp, payload.data(), payload.size(),
                            arrival.arrive_ns) == PushResult::kDroppedLate) {
                ++late_drops;
            }
        }
        JitterPacketView view;
        for (;;) {
            const PopResult result = buffer.Pop(&view, render);
            if (result != PopResult::kOk) {
                break;
            }
            if (view.seq != expected) {
                ++violations;
            }
            expected = view.seq + 1;
            ++delivered;
            held.Add(static_cast<double>(view.held_ns) / 1e6);
        }
    }

    const JitterStats stats = buffer.Snapshot();
    const double delivery_ratio = static_cast<double>(delivered) / static_cast<double>(kFrames);
    MEDIA_CHECK_EQ(violations, 0U);
    MEDIA_CHECK_GE(delivery_ratio, 0.995);
    MEDIA_CHECK_EQ(delivered, stats.delivered);
    // 深度自适应：target = base(20ms) + 2 x J，J 是 |D| 的 EWMA（本模拟约 35ms），
    // 所以 target 应在 80~200ms；同时必须显著大于零抖动时的 20ms。
    MEDIA_CHECK_GE(stats.jitter_ns, 20 * kMsNs);
    MEDIA_CHECK_LE(stats.jitter_ns, 80 * kMsNs);
    MEDIA_CHECK_GE(stats.target_delay_ns, 20 * kMsNs + stats.jitter_ns);
    MEDIA_CHECK_GE(stats.target_delay_ns, 60 * kMsNs);
    MEDIA_CHECK_LE(stats.target_delay_ns, 400 * kMsNs);
    MEDIA_CHECK_GE(stats.reordered, 1000U);

    std::printf(
        "  [jitter] %u frames +-50ms: delivered=%.3f%% violations=%llu late_drops=%llu "
        "reordered=%llu target=%.2fms jitter=%.2fms held p50=%.2f p99=%.2f max=%.2f ms\n",
        static_cast<unsigned>(kFrames), delivery_ratio * 100.0,
        static_cast<unsigned long long>(violations),
        static_cast<unsigned long long>(late_drops),
        static_cast<unsigned long long>(stats.reordered),
        static_cast<double>(stats.target_delay_ns) / 1e6,
        static_cast<double>(stats.jitter_ns) / 1e6, held.p50(), held.p99(), held.max());
    MEDIA_CHECK_LE(held.p99(), 400.0);
}

// ---------------------------------------------------------------------------
// 场景 12：压测 1e6 帧 + 内存有界 + 零分配 + ns 级吞吐
// ---------------------------------------------------------------------------
void BenchAndMemory() {
    JitterBufferConfig config;
    config.capacity = 512;
    config.startup_buffering = false;
    JitterBuffer buffer(config);

    const std::size_t fixed_bytes = buffer.allocated_bytes();
    const std::size_t heap_before = HeapInUse();

    constexpr std::size_t kBytes = 1200;
    std::vector<std::uint8_t> payload(kBytes, 0x5A);
    constexpr std::uint32_t kBatch = 480;      // < capacity，避免覆盖
    constexpr std::uint32_t kBatches = 2200;   // 1.056e6 次 push
    constexpr std::uint32_t kTotal = kBatch * kBatches;

    std::uint32_t seq = 0;
    std::int64_t now = 0;
    double push_ns = 0.0;
    double pop_ns = 0.0;
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    std::uint64_t push_rejects = 0;
    std::uint64_t order_violations = 0;
    std::uint32_t expected = 0;

    for (std::uint32_t batch = 0; batch < kBatches; ++batch) {
        const auto push_start = Clock::now();
        for (std::uint32_t i = 0; i < kBatch; ++i) {
            // 计时循环内不做 MEDIA_CHECK（宏会自增全局计数，污染 ns 级测量）。
            if (buffer.Push(seq, seq * 3000U, payload.data(), payload.size(), now) !=
                PushResult::kAccepted) {
                ++push_rejects;
            }
            ++seq;
            ++pushed;
            now += kFrameNs;
        }
        push_ns += NsSince(push_start);

        const auto pop_start = Clock::now();
        JitterPacketView view;
        for (;;) {
            const PopResult result = buffer.PopFlush(&view, now);
            if (result != PopResult::kOk) {
                break;
            }
            if (view.seq != expected) {
                ++order_violations;
            }
            expected = view.seq + 1;
            ++popped;
        }
        pop_ns += NsSince(pop_start);
    }

    const std::size_t heap_after = HeapInUse();
    const std::size_t fixed_after = buffer.allocated_bytes();

    MEDIA_CHECK_EQ(pushed, static_cast<std::uint64_t>(kTotal));
    MEDIA_CHECK_EQ(popped, static_cast<std::uint64_t>(kTotal));
    MEDIA_CHECK_EQ(push_rejects, 0U);
    MEDIA_CHECK_EQ(order_violations, 0U);
    MEDIA_CHECK_EQ(fixed_bytes, fixed_after);
    MEDIA_CHECK_EQ(buffer.buffered(), 0U);

    const double push_per = push_ns / static_cast<double>(pushed);
    const double pop_per = pop_ns / static_cast<double>(popped);
    std::printf(
        "  [stress] %u push / %u pop: push=%.2f ns pop=%.2f ns  (%.2f Mpps / %.2f Mpps)\n",
        static_cast<unsigned>(pushed), static_cast<unsigned>(popped), push_per, pop_per,
        1e3 / push_per, 1e3 / pop_per);
    std::printf("  [memory] fixed=%zu B (meta+payload), heap delta=%lld B\n", fixed_after,
                static_cast<long long>(heap_after) - static_cast<long long>(heap_before));

    // 内存有界：固定字节数不随流量变化；堆增长只允许是 printf/getenv 这类一次性噪声。
    MEDIA_CHECK_LE(heap_after > heap_before ? heap_after - heap_before : 0, 256U * 1024U);
    MEDIA_CHECK_LE(fixed_after, 2U * 1024U * 1024U);

    // ns 级：1.2KB memcpy 的下限约 12~25ns（L1/L2），索引开销应在个位数 ns。
    MEDIA_CHECK_LE(push_per, 120.0);
    MEDIA_CHECK_LE(pop_per, 60.0);
}

// ---------------------------------------------------------------------------
// 基线：memcpy 吞吐与缓存延迟（用于「离物理下限几倍」的换算）
// ---------------------------------------------------------------------------
void BenchBaselines() {
    {
        constexpr std::size_t kBytes = 1200;
        std::vector<std::uint8_t> src(kBytes, 1);
        std::vector<std::uint8_t> dst(kBytes, 0);
        constexpr std::uint32_t kIters = 1'000'000;
        const auto start = Clock::now();
        for (std::uint32_t i = 0; i < kIters; ++i) {
            std::memcpy(dst.data(), src.data(), kBytes);
            dst[i & 31U] = static_cast<std::uint8_t>(dst[i & 31U] + 1U);
        }
        const double ns = NsSince(start) / static_cast<double>(kIters);
        std::printf("  [baseline] memcpy(1200B) = %.2f ns (%.1f GB/s effective)\n", ns,
                    2.0 * static_cast<double>(kBytes) / ns);
    }
    {
        constexpr std::size_t kBytes = 1U << 20;  // 1MB
        std::vector<std::uint8_t> src(kBytes, 1);
        std::vector<std::uint8_t> dst(kBytes, 0);
        constexpr std::uint32_t kIters = 500;
        const auto start = Clock::now();
        for (std::uint32_t i = 0; i < kIters; ++i) {
            std::memcpy(dst.data(), src.data(), kBytes);
            dst[i] = static_cast<std::uint8_t>(dst[i] + 1U);
        }
        const double ns = NsSince(start);
        const double gbs = 2.0 * static_cast<double>(kBytes) * kIters / ns;
        std::printf("  [baseline] memcpy(1MB) = %.1f GB/s (read+write)\n", gbs);
    }
    {
        // 指针追逐：测三级缓存的实际延迟（数组大小决定工作集）。
        const std::uint32_t sizes[] = {4U * 1024U, 256U * 1024U, 8U * 1024U * 1024U};
        const char* names[] = {"L1(4KB)", "L2(256KB)", "L3(8MB)"};
        for (int index = 0; index < 3; ++index) {
            const std::size_t count = sizes[index] / sizeof(std::uint32_t);
            std::vector<std::uint32_t> next(count);
            for (std::size_t i = 0; i < count; ++i) {
                next[i] = static_cast<std::uint32_t>((i * 167U + 13U) % count);
            }
            constexpr std::uint32_t kSteps = 2'000'000;
            std::uint32_t cursor = 0;
            const auto start = Clock::now();
            for (std::uint32_t step = 0; step < kSteps; ++step) {
                cursor = next[cursor];
            }
            const double ns = NsSince(start) / static_cast<double>(kSteps);
            std::printf("  [baseline] pointer-chase %s = %.2f ns/load (cursor=%u)\n", names[index],
                        ns, cursor);
        }
    }
}

}  // namespace

int main() {
    std::printf("[jitter_buffer] functional + random + stress\n");
    TestInOrder();
    TestPayloadIntegrity();
    TestReorder();
    TestDuplicate();
    TestLoss();
    TestBurstLoss();
    TestLate();
    TestDiscontinuity();
    TestWrapAround();
    TestInvalid();
    TestJitter();
    BenchAndMemory();
    BenchBaselines();
    return media_test::Finish("jitter_buffer");
}
