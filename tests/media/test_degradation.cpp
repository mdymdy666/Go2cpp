/**
 * @file test_degradation.cpp
 * @brief `DegradationController` / `ReconnectSupervisor` / `Downscale2xBgra` 的
 *        功能、随机与真实场景：带宽阶梯下降/恢复、丢包触发、帧率控制、
 *        断线重连退避、BBR 联动、滞回防震荡、2:1 下采样逐像素校验。
 * @details 时间全部是模拟毫秒，不 sleep；随机量固定种子，失败可复现。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "go2cpp/bento/net/bandwidth.hpp"
#include "go2cpp/media/degradation.hpp"
#include "go2cpp/media/frame.hpp"
#include "go2cpp/media/pixel.hpp"
#include "media_test_util.hpp"

// `media_test_util.hpp` 提供 CHECK / CHECK_EQ / CHECK_GE；这里补一个 LE（语义相同）。
#define MEDIA_CHECK_LE(actual, bound) MEDIA_CHECK((actual) <= (bound))

namespace {

using namespace go2cpp::media;

/// @brief 连续喂同一采样若干次，返回期间**最后一次真正的档位变化**事件。
/// @details 返回「最后一次变化」而不是「最后一次调用」：滞回逻辑意味着档位变化
///          通常发生在序列中段，只看末次调用会漏掉它。
DegradationEvent Feed(DegradationController& controller, std::int64_t* now_ms,
                      std::uint64_t available_bps, double loss_rate, int times,
                      std::int64_t step_ms = 100) {
    DegradationEvent changed;
    for (int i = 0; i < times; ++i) {
        NetworkSample sample;
        sample.available_bps = available_bps;
        sample.loss_rate = loss_rate;
        const DegradationEvent event = controller.Update(*now_ms, sample);
        if (event.changed) {
            changed = event;
        }
        *now_ms += step_ms;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// 场景 1：带宽阶梯下降（1.5M -> 700k -> 500k -> 200k -> 100k）
// ---------------------------------------------------------------------------
void TestBandwidthLadder() {
    DegradationController controller;
    std::int64_t now = 0;

    MEDIA_CHECK(controller.level() == QualityLevel::kFull);
    MEDIA_CHECK_EQ(controller.target_width(), 320);
    MEDIA_CHECK_EQ(controller.target_height(), 240);
    MEDIA_CHECK_EQ(controller.target_fps(), 30);
    MEDIA_CHECK_EQ(controller.target_bitrate_bps(), 800000U);

    // 1.5Mbps 充足：保持满档。
    const DegradationEvent keep = Feed(controller, &now, 1'500'000, 0.0, 4);
    MEDIA_CHECK(!keep.changed);
    MEDIA_CHECK(controller.level() == QualityLevel::kFull);

    // 700kbps：可用 630k < full 的 900k，但够 reduced 的 520k。
    const DegradationEvent down1 = Feed(controller, &now, 700'000, 0.0, 6);
    MEDIA_CHECK(down1.changed);
    MEDIA_CHECK(down1.to == QualityLevel::kReduced);
    MEDIA_CHECK(down1.reason == DegradationReason::kBandwidth);
    MEDIA_CHECK_EQ(controller.target_width(), 320);
    MEDIA_CHECK_EQ(controller.target_fps(), 15);

    // 500kbps：450k < reduced 的 520k，但够 low 的 240k。
    const DegradationEvent down2 = Feed(controller, &now, 500'000, 0.0, 6);
    MEDIA_CHECK(down2.changed);
    MEDIA_CHECK(down2.to == QualityLevel::kLow);
    MEDIA_CHECK_EQ(controller.target_width(), 160);
    MEDIA_CHECK_EQ(controller.target_height(), 120);
    MEDIA_CHECK_EQ(controller.target_fps(), 15);

    // 200kbps：180k < low 的 240k，但够 minimal 的 140k。
    const DegradationEvent down3 = Feed(controller, &now, 200'000, 0.0, 6);
    MEDIA_CHECK(down3.changed);
    MEDIA_CHECK(down3.to == QualityLevel::kMinimal);
    MEDIA_CHECK_EQ(controller.target_fps(), 10);

    // 100kbps：已经在保底档，不能继续降。
    const DegradationEvent floor_event = Feed(controller, &now, 100'000, 0.0, 6);
    MEDIA_CHECK(!floor_event.changed);
    MEDIA_CHECK(controller.level() == QualityLevel::kMinimal);

    const DegradationStats stats = controller.Snapshot();
    MEDIA_CHECK_EQ(stats.downgrades, 3U);
    MEDIA_CHECK_EQ(stats.upgrades, 0U);
    std::printf(
        "  [ladder] 1.5M->700k->500k->200k->100k: full->reduced->low->minimal "
        "downgrades=%llu\n",
        static_cast<unsigned long long>(stats.downgrades));
}

// ---------------------------------------------------------------------------
// 场景 2：丢包触发降级（6% -> 降一级；15% -> 保底）
// ---------------------------------------------------------------------------
void TestLossTrigger() {
    {
        DegradationController controller;
        std::int64_t now = 0;
        DegradationEvent event = Feed(controller, &now, 2'000'000, 0.06, 6);
        MEDIA_CHECK(event.changed);
        MEDIA_CHECK(event.reason == DegradationReason::kLoss);
        MEDIA_CHECK(controller.level() == QualityLevel::kReduced);
        std::printf("  [loss 6%%] -> %s (%s)\n", QualityLevelName(controller.level()),
                    DegradationReasonName(event.reason));
    }
    {
        DegradationController controller;
        std::int64_t now = 0;
        // 15% >= loss_severe(12%)：一次最多降 2 级，200ms 后再降到底。
        const DegradationEvent first = Feed(controller, &now, 2'000'000, 0.15, 4);
        MEDIA_CHECK(first.changed);
        MEDIA_CHECK(controller.level() == QualityLevel::kLow);
        const DegradationEvent second = Feed(controller, &now, 2'000'000, 0.15, 4);
        MEDIA_CHECK(second.changed);
        MEDIA_CHECK(controller.level() == QualityLevel::kMinimal);
        MEDIA_CHECK_EQ(controller.Snapshot().downgrades, 2U);
        std::printf("  [loss 15%%] -> %s (two steps, max_downgrade_step=2)\n",
                    QualityLevelName(controller.level()));
    }
}

// ---------------------------------------------------------------------------
// 场景 3：恢复（带宽回升 -> 3s hold -> 逐级升回满档）
// ---------------------------------------------------------------------------
void TestRecovery() {
    DegradationController controller;
    std::int64_t now = 0;
    // 先降到保底。
    Feed(controller, &now, 150'000, 0.0, 10);
    MEDIA_CHECK(controller.level() == QualityLevel::kMinimal);

    // 带宽回到 3Mbps：2.9s 内不升（upgrade_hold=3000ms）。
    const std::int64_t started = now;
    DegradationEvent event;
    int upgrades = 0;
    for (int i = 0; i < 40; ++i) {  // 40 x 100ms = 4s
        event = Feed(controller, &now, 3'000'000, 0.0, 1, 100);
        if (event.changed) {
            ++upgrades;
        }
        if (now - started < 2'900) {
            MEDIA_CHECK(!event.changed);
        }
    }
    MEDIA_CHECK(upgrades >= 1);
    MEDIA_CHECK(controller.level() != QualityLevel::kMinimal);
    const QualityLevel after_first = controller.level();
    MEDIA_CHECK(after_first == QualityLevel::kLow);

    // 继续喂：每 3s 升一级，直到满档。
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 40; ++i) {
            Feed(controller, &now, 3'000'000, 0.0, 1, 100);
        }
    }
    MEDIA_CHECK(controller.level() == QualityLevel::kFull);
    const DegradationStats stats = controller.Snapshot();
    MEDIA_CHECK_EQ(stats.upgrades, 3U);
    MEDIA_CHECK_GE(stats.downgrades, 2U);
    std::printf("  [recovery] minimal -> low -> reduced -> full, upgrades=%llu downgrades=%llu\n",
                static_cast<unsigned long long>(stats.upgrades),
                static_cast<unsigned long long>(stats.downgrades));
}

// ---------------------------------------------------------------------------
// 场景 4：低帧率落地（30fps 输入 -> 30/15/10fps 实际放行率）
// ---------------------------------------------------------------------------
void TestFramePacing() {
    DegradationController controller;
    const int expect_fps[] = {30, 15, 10};
    const QualityLevel levels[] = {QualityLevel::kFull, QualityLevel::kReduced,
                                   QualityLevel::kMinimal};
    std::uint64_t results[3] = {0, 0, 0};

    for (int round = 0; round < 3; ++round) {
        controller.ForceLevel(0, levels[round], DegradationReason::kExternal);
        std::int64_t now = 0;
        std::uint64_t passed = 0;
        std::uint64_t dropped = 0;
        // 30fps 输入，10 秒 = 300 帧。
        for (int tick = 0; tick < 300; ++tick) {
            if (controller.ShouldDeliverFrame(now)) {
                ++passed;
            } else {
                ++dropped;
            }
            now += 33;
        }
        results[round] = passed;
        MEDIA_CHECK_EQ(passed + dropped, 300U);
        const double fps = static_cast<double>(passed) / 10.0;
        const double target = static_cast<double>(expect_fps[round]);
        MEDIA_CHECK_GE(fps, target * 0.9);
        MEDIA_CHECK_LE(fps, target * 1.1);
    }
    std::printf("  [pacing] 30fps input -> passed full=%llu reduced=%llu minimal=%llu "
                "(expect 300/150/100)\n",
                static_cast<unsigned long long>(results[0]),
                static_cast<unsigned long long>(results[1]),
                static_cast<unsigned long long>(results[2]));
    MEDIA_CHECK_EQ(results[0], 300U);
}

// ---------------------------------------------------------------------------
// 场景 5：断线检测 + 指数退避重连（jitter=0 便于精确断言）
// ---------------------------------------------------------------------------
void TestReconnect() {
    ReconnectConfig config;
    config.suspect_after_ms = 1000;
    config.disconnect_after_ms = 3000;
    config.max_attempts = 8;
    config.jitter_percent = 0;  // 关抖动：退避序列必须精确
    ReconnectSupervisor supervisor(config);

    supervisor.OnPacket(0);
    MEDIA_CHECK(supervisor.state() == LinkState::kConnected);

    MEDIA_CHECK(supervisor.Tick(900) == ReconnectAction::kNone);
    MEDIA_CHECK(supervisor.state() == LinkState::kConnected);

    supervisor.Tick(1000);
    MEDIA_CHECK(supervisor.state() == LinkState::kSuspect);

    supervisor.Tick(2999);
    MEDIA_CHECK(supervisor.state() == LinkState::kSuspect);

    supervisor.Tick(3000);
    MEDIA_CHECK(supervisor.state() == LinkState::kDisconnected);
    MEDIA_CHECK_EQ(supervisor.Snapshot().disconnects, 1U);

    // 退避：第 n 次 Probe 之后安排的下一次间隔 = 200,400,800,1600,3200,5000,5000,5000
    const std::uint32_t expected_backoff[] = {200, 400, 800, 1600, 3200, 5000, 5000, 5000};
    for (std::size_t i = 0; i < 8; ++i) {
        const std::int64_t due = supervisor.next_attempt_at_ms();
        MEDIA_CHECK(supervisor.Tick(due - 1) == ReconnectAction::kNone);
        MEDIA_CHECK(supervisor.Tick(due) == ReconnectAction::kProbe);
        MEDIA_CHECK_EQ(supervisor.current_backoff_ms(), expected_backoff[i]);
        MEDIA_CHECK_EQ(supervisor.attempts(), static_cast<std::uint32_t>(i + 1));
    }
    // 第 9 次：尝试次数用尽。
    const ReconnectAction exhausted = supervisor.Tick(supervisor.next_attempt_at_ms());
    MEDIA_CHECK(exhausted == ReconnectAction::kExhausted);
    MEDIA_CHECK(supervisor.state() == LinkState::kFailed);
    MEDIA_CHECK_EQ(supervisor.Snapshot().attempts, 8U);
    MEDIA_CHECK_EQ(supervisor.Snapshot().exhausted, 1U);

    // 断线态下收到包：立刻恢复并请求关键帧。
    const std::int64_t clock = supervisor.next_attempt_at_ms() + 10;
    supervisor.OnPacket(clock);
    MEDIA_CHECK(supervisor.state() == LinkState::kConnected);
    MEDIA_CHECK_EQ(supervisor.attempts(), 0U);
    MEDIA_CHECK(supervisor.ConsumeKeyframeRequest());
    MEDIA_CHECK(!supervisor.ConsumeKeyframeRequest());
    MEDIA_CHECK_EQ(supervisor.Snapshot().recoveries, 1U);

    // 重连尝试成功也走同一条恢复路径。
    ReconnectSupervisor other(config);
    other.OnPacket(0);
    other.Tick(1000);
    other.Tick(3000);
    MEDIA_CHECK(other.Tick(3101) == ReconnectAction::kProbe);
    other.OnAttemptResult(3150, false);
    MEDIA_CHECK(other.state() == LinkState::kReconnecting);
    other.OnAttemptResult(3200, true);
    MEDIA_CHECK(other.state() == LinkState::kConnected);

    std::printf("  [reconnect] 8 backoff attempts 100..5000ms -> failed -> packet recovers, "
                "attempts=%llu recoveries=%llu\n",
                static_cast<unsigned long long>(supervisor.Snapshot().attempts),
                static_cast<unsigned long long>(supervisor.Snapshot().recoveries));
}

// ---------------------------------------------------------------------------
// 场景 6：抖动开启时退避仍落在 ±jitter% 区间内（确定性种子）
// ---------------------------------------------------------------------------
void TestReconnectJitter() {
    ReconnectConfig config;
    config.jitter_percent = 20;
    config.seed = 0xDEADBEEFU;
    ReconnectSupervisor supervisor(config);
    supervisor.OnPacket(0);
    supervisor.Tick(1000);
    supervisor.Tick(3000);

    std::int64_t clock = 3000;
    for (std::size_t i = 0; i < 8; ++i) {
        clock = supervisor.next_attempt_at_ms();
        MEDIA_CHECK(supervisor.Tick(clock) == ReconnectAction::kProbe);
        // 第 i 次 Probe 之后安排的间隔基准 = 200 << i（封顶 5000）。
        const std::uint32_t base = (i < 5) ? (200U << i) : 5000U;
        const std::uint32_t span = base / 100U * 20U;
        const std::uint32_t backoff = supervisor.current_backoff_ms();
        MEDIA_CHECK_GE(backoff, base - span);
        MEDIA_CHECK_LE(backoff, base + span);
        supervisor.OnAttemptResult(clock + 1, false);
    }
    std::printf("  [reconnect-jitter] 8 attempts stay within +-20%% of 100..5000ms\n");
}

// ---------------------------------------------------------------------------
// 场景 7：BBR 联动（真实 BandwidthEstimator -> FromBbr -> 控制器）
// ---------------------------------------------------------------------------
void TestBbrIntegration() {
    namespace bento = go2cpp::bento;

    DegradationController controller;
    std::int64_t now = 0;
    const std::uint64_t rates[] = {2'000'000, 600'000, 250'000};

    for (std::size_t round = 0; round < 3; ++round) {
        bento::BandwidthEstimator estimator;
        // 按目标速率产生 ACK：每 10ms 确认 (bps/8/100) 字节。
        const std::uint64_t bytes_per_ack = rates[round] / 8 / 100;
        for (int i = 0; i <= 100; ++i) {
            estimator.OnAck(static_cast<std::uint64_t>(i) * 10, bytes_per_ack);
        }
        const bento::BandwidthStats stats = estimator.Snapshot(1000);
        const NetworkSample sample = NetworkSample::FromBbr(stats);
        MEDIA_CHECK(sample.valid);
        MEDIA_CHECK_GE(sample.available_bps, rates[round] / 2);
        MEDIA_CHECK_LE(sample.available_bps, rates[round] * 2);
        for (int i = 0; i < 8; ++i) {
            controller.Update(now, sample);
            now += 100;
        }
    }
    // 2M -> full；600k -> reduced/low；250k -> minimal（逐级降）。
    MEDIA_CHECK(controller.level() != QualityLevel::kFull);
    const DegradationStats stats = controller.Snapshot();
    MEDIA_CHECK_GE(stats.downgrades, 1U);
    std::printf("  [bbr] delivery_rate 2M->600k->250k through BandwidthEstimator: "
                "level=%s smoothed=%.2fMbps downgrades=%llu\n",
                QualityLevelName(controller.level()),
                static_cast<double>(stats.smoothed_bps) / 1e6,
                static_cast<unsigned long long>(stats.downgrades));
}

// ---------------------------------------------------------------------------
// 场景 8：滞回防震荡（带宽短时抖动不触发升降）
// ---------------------------------------------------------------------------
void TestHysteresis() {
    DegradationController controller;
    std::int64_t now = 0;
    // 每 50ms 一个采样：150ms 低（700k）-> 150ms 高（2M），交替 20 个周期。
    for (int cycle = 0; cycle < 20; ++cycle) {
        for (int i = 0; i < 3; ++i) {
            NetworkSample low;
            low.available_bps = 700'000;
            controller.Update(now, low);
            now += 50;
        }
        for (int i = 0; i < 3; ++i) {
            NetworkSample high;
            high.available_bps = 2'000'000;
            controller.Update(now, high);
            now += 50;
        }
    }
    const DegradationStats stats = controller.Snapshot();
    // 低带宽每次只持续 150ms < downgrade_hold(200ms)，不应发生任何切档。
    MEDIA_CHECK_EQ(stats.downgrades, 0U);
    MEDIA_CHECK_EQ(stats.upgrades, 0U);
    MEDIA_CHECK(controller.level() == QualityLevel::kFull);
    std::printf("  [hysteresis] 150ms low / 150ms high x20 cycles -> level stays %s "
                "(downgrades=%llu)\n",
                QualityLevelName(controller.level()),
                static_cast<unsigned long long>(stats.downgrades));
}

// ---------------------------------------------------------------------------
// 场景 9：重连后的重建（关键帧请求 + 升级静默期）
// ---------------------------------------------------------------------------
void TestReconnectRebuild() {
    DegradationController controller;
    std::int64_t now = 0;
    controller.ForceLevel(now, QualityLevel::kMinimal, DegradationReason::kExternal);
    now += 100;

    const DegradationEvent event = controller.OnReconnected(now);
    MEDIA_CHECK(!event.changed);
    MEDIA_CHECK(event.reason == DegradationReason::kReconnect);
    MEDIA_CHECK(controller.ConsumeKeyframeRequest());
    MEDIA_CHECK(!controller.ConsumeKeyframeRequest());
    MEDIA_CHECK_EQ(controller.Snapshot().reconnects, 1U);

    // 重连后 500ms 静默期内即便带宽充足也不升档。
    const std::int64_t guard_until = now + 500;
    for (int i = 0; i < 4; ++i) {
        NetworkSample good;
        good.available_bps = 5'000'000;
        const DegradationEvent pending = controller.Update(now, good);
        MEDIA_CHECK(!pending.changed);
        now += 100;
    }
    MEDIA_CHECK(now <= guard_until);
    // 静默期过后持续 3s 才升一级。
    for (int i = 0; i < 40; ++i) {
        NetworkSample good;
        good.available_bps = 5'000'000;
        controller.Update(now, good);
        now += 100;
    }
    MEDIA_CHECK(controller.level() == QualityLevel::kLow);
    std::printf("  [reconnect-rebuild] keyframe requested, guard=%lldms, level -> %s\n",
                static_cast<long long>(controller.config().reconnect_guard_ms),
                QualityLevelName(controller.level()));
}

// ---------------------------------------------------------------------------
// 场景 10：2:1 盒式下采样（320x240 -> 160x120）逐像素校验
// ---------------------------------------------------------------------------
void TestDownscale() {
    constexpr int kSrcW = 320;
    constexpr int kSrcH = 240;
    constexpr int kDstW = kSrcW / 2;
    constexpr int kDstH = kSrcH / 2;
    const int src_stride = kSrcW * 4;
    const int dst_stride = kDstW * 4;

    std::vector<std::uint8_t> src(static_cast<std::size_t>(src_stride) * kSrcH);
    for (int y = 0; y < kSrcH; ++y) {
        for (int x = 0; x < kSrcW; ++x) {
            std::uint8_t* p = src.data() + static_cast<std::size_t>(y) * src_stride + x * 4;
            p[0] = static_cast<std::uint8_t>((x * 3 + y) & 0xFF);
            p[1] = static_cast<std::uint8_t>((y * 2) & 0xFF);
            p[2] = static_cast<std::uint8_t>((x ^ y) & 0xFF);
            p[3] = 255;
        }
    }
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(dst_stride) * kDstH, 0);
    Downscale2xBgra(src.data(), kSrcW, kSrcH, src_stride, dst.data(), dst_stride);

    std::uint64_t mismatches = 0;
    std::uint64_t max_delta = 0;
    for (int y = 0; y < kDstH; ++y) {
        for (int x = 0; x < kDstW; ++x) {
            for (int c = 0; c < 4; ++c) {
                std::uint32_t sum = 0;
                sum += src[static_cast<std::size_t>(2 * y) * src_stride + (2 * x) * 4 + c];
                sum += src[static_cast<std::size_t>(2 * y) * src_stride + (2 * x + 1) * 4 + c];
                sum += src[static_cast<std::size_t>(2 * y + 1) * src_stride + (2 * x) * 4 + c];
                sum += src[static_cast<std::size_t>(2 * y + 1) * src_stride + (2 * x + 1) * 4 + c];
                const std::uint8_t expected = static_cast<std::uint8_t>((sum + 2) >> 2);
                const std::uint8_t actual =
                    dst[static_cast<std::size_t>(y) * dst_stride + x * 4 + c];
                if (actual != expected) {
                    ++mismatches;
                    const std::uint32_t delta = actual > expected ? actual - expected
                                                                  : expected - actual;
                    if (delta > max_delta) {
                        max_delta = delta;
                    }
                }
            }
        }
    }
    MEDIA_CHECK_EQ(mismatches, 0U);

    // 降级后的帧字节数是 1/4（BGRA）——这是「低分辨率」真正的带宽收益。
    const std::size_t src_bytes = BgraFrameBytes(kSrcW, kSrcH);
    const std::size_t dst_bytes = BgraFrameBytes(kDstW, kDstH);
    MEDIA_CHECK_EQ(src_bytes, 4U * dst_bytes);
    MEDIA_CHECK_EQ(I420FrameBytes(kDstW, kDstH) * 4U, I420FrameBytes(kSrcW, kSrcH));
    std::printf("  [downscale] %dx%d -> %dx%d, pixel-exact box filter, frame %zuB -> %zuB "
                "(1/4)\n",
                kSrcW, kSrcH, kDstW, kDstH, src_bytes, dst_bytes);

    // 边界：奇数尺寸/空指针必须安全返回（不写越界、不崩）。
    std::vector<std::uint8_t> guard(64, 0xEE);
    Downscale2xBgra(src.data(), 321, 240, src_stride, guard.data(), 4);
    Downscale2xBgra(nullptr, kSrcW, kSrcH, src_stride, guard.data(), dst_stride);
    Downscale2xBgra(src.data(), kSrcW, kSrcH, src_stride, nullptr, dst_stride);
    std::uint64_t touched = 0;
    for (std::uint8_t byte : guard) {
        if (byte != 0xEE) {
            ++touched;
        }
    }
    MEDIA_CHECK_EQ(touched, 0U);
}

// ---------------------------------------------------------------------------
// 场景 11：随机带宽序列（1000 次采样，状态必须始终自洽）
// ---------------------------------------------------------------------------
void TestRandomBandwidth() {
    DegradationController controller;
    std::int64_t now = 0;
    std::uint32_t state = 0x2468ACE0U;
    std::uint64_t changes = 0;
    std::uint64_t invalid_levels = 0;
    const int fps_by_level[] = {30, 15, 15, 10};

    for (int i = 0; i < 1000; ++i) {
        state = state * 1664525U + 1013904223U;
        NetworkSample sample;
        sample.available_bps = 50'000 + (state % 3'000'000);
        sample.loss_rate = static_cast<double>(state >> 8 & 0xFFU) / 255.0 * 0.2;
        const DegradationEvent event = controller.Update(now, sample);
        if (event.changed) {
            ++changes;
            // 事件里的 from/to 必须与控制器当前状态一致。
            if (event.to != controller.level()) {
                ++invalid_levels;
            }
        }
        const int level = static_cast<int>(controller.level());
        if (level < 0 || level > 3) {
            ++invalid_levels;
        }
        if (controller.target_fps() != fps_by_level[level]) {
            ++invalid_levels;
        }
        now += 50;
    }
    MEDIA_CHECK_EQ(invalid_levels, 0U);
    MEDIA_CHECK_GE(changes, 1U);
    const DegradationStats stats = controller.Snapshot();
    MEDIA_CHECK_EQ(stats.samples, 1000U);
    std::printf("  [random] 1000 noisy samples: %llu level changes, downgrades=%llu "
                "upgrades=%llu, state consistent\n",
                static_cast<unsigned long long>(changes),
                static_cast<unsigned long long>(stats.downgrades),
                static_cast<unsigned long long>(stats.upgrades));
}

}  // namespace

int main() {
    std::printf("[degradation] bandwidth / loss / pacing / reconnect / bbr / downscale\n");
    TestBandwidthLadder();
    TestLossTrigger();
    TestRecovery();
    TestFramePacing();
    TestReconnect();
    TestReconnectJitter();
    TestBbrIntegration();
    TestHysteresis();
    TestReconnectRebuild();
    TestDownscale();
    TestRandomBandwidth();
    return media_test::Finish("degradation");
}
