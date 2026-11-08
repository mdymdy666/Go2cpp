/**
 * @file test_bwe_gcc.cpp
 * @brief 延迟梯度 BWE 单测：延迟上升降速、稳定缓慢升速、降后 hold、加性增、
 *        乱序/Reset 边界，以及「延迟上升但无丢包」时 GCC 与 BBR 的对照。
 * @details 控制器用例全部使用显式时间戳，不读真实时钟，结果确定可复现；
 *          BBR 对照复用现有 `bento::CongestionController`（kBbr），喂同样的
 *          发送/确认节奏，只改变 RTT 样本，证明 BBR 对延迟上升零响应。
 * @author go2cpp contributors
 * @date 2026-11-08
 * @copyright MIT License
 */

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "go2cpp/bento/net/bwe_gcc.hpp"
#include "go2cpp/bento/net/congestion.hpp"

namespace {

using go2cpp::bento::AckSample;
using go2cpp::bento::BweGcc;
using go2cpp::bento::BweGccConfig;
using go2cpp::bento::CongestionAlgorithm;
using go2cpp::bento::CongestionConfig;
using go2cpp::bento::CongestionController;
using go2cpp::bento::GccIncreaseMode;
using go2cpp::bento::GccTrendState;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(condition)) {                                                             \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                               \
        }                                                                               \
    } while (0)

#define CHECK_EQ(actual, expected)                                                      \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!((actual) == (expected))) {                                                \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual, \
                         #expected);                                                        \
            ++g_failures;                                                               \
        }                                                                               \
    } while (0)

int Finish(const char* name) {
    std::printf("[%s] %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

/// 稳定链路：发送与到达都每 10ms 推进，delta 恒为 0。
void FeedStable(BweGcc* bwe, std::uint64_t* send_ms, std::uint64_t* arrival_ms,
                int count) {
    for (int i = 0; i < count; ++i) {
        *send_ms += 10;
        *arrival_ms += 10;
        bwe->OnPacketFeedback(*send_ms, *arrival_ms);
    }
}

/// 排队增长链路：发送每 10ms 一个包，到达每 13ms 一个包（delta=3ms）。
/// 累计延迟以 3/13 ≈ 0.23 ms/ms 的斜率上升，足以越过 WebRTC 默认阈值。
void FeedQueueGrowth(BweGcc* bwe, std::uint64_t* send_ms, std::uint64_t* arrival_ms,
                     int count) {
    for (int i = 0; i < count; ++i) {
        *send_ms += 10;
        *arrival_ms += 13;
        bwe->OnPacketFeedback(*send_ms, *arrival_ms);
    }
}

// ---------------------------------------------------------------------------
// 场景 1：延迟上升（无丢包）必须降速
// ---------------------------------------------------------------------------

void TestDelayRiseDecreasesRate() {
    BweGcc bwe;
    const std::uint64_t initial = bwe.TargetBitrate();
    std::uint64_t send = 0;
    std::uint64_t arrival = 50;

    // 先给 40 包稳定链路，让 AIMD 有正常增长基准。
    FeedStable(&bwe, &send, &arrival, 40);
    const std::uint64_t stable_rate = bwe.TargetBitrate();
    CHECK_EQ(bwe.state(), GccTrendState::kNormal);
    CHECK_EQ(bwe.stats().decreases, 0U);

    // 再给 60 包「发送 10ms / 到达 13ms」的排队增长。
    FeedQueueGrowth(&bwe, &send, &arrival, 60);
    const std::uint64_t after_rate = bwe.TargetBitrate();

    std::printf("[INFO] delay-rise: initial=%llu stable=%llu after=%llu B/s "
                "trend=%.4f modified=%.2f threshold=%.2f state=%s decreases=%llu\n",
                static_cast<unsigned long long>(initial),
                static_cast<unsigned long long>(stable_rate),
                static_cast<unsigned long long>(after_rate), bwe.trend(),
                bwe.modified_trend(), bwe.threshold(),
                go2cpp::bento::GccTrendStateName(bwe.state()),
                static_cast<unsigned long long>(bwe.stats().decreases));

    CHECK_EQ(bwe.state(), GccTrendState::kOverusing);
    CHECK(bwe.stats().decreases >= 1U);
    CHECK(after_rate < initial);
    CHECK(after_rate < stable_rate);
    CHECK(bwe.accumulated_delay_ms() > 0.0);
}

// ---------------------------------------------------------------------------
// 场景 2：稳定链路必须缓慢升速（α=1.08/s）
// ---------------------------------------------------------------------------

void TestStableSlowIncrease() {
    BweGcc bwe;
    const std::uint64_t initial = bwe.TargetBitrate();
    std::uint64_t send = 0;
    std::uint64_t arrival = 50;

    // 200 包 × 10ms ≈ 2 秒；AIMD 从第 20 个有效样本开始计时。
    FeedStable(&bwe, &send, &arrival, 200);
    const std::uint64_t final_rate = bwe.TargetBitrate();
    const double ratio = static_cast<double>(final_rate) / static_cast<double>(initial);

    std::printf("[INFO] stable: initial=%llu final=%llu B/s ratio=%.4f "
                "increases=%llu decreases=%llu\n",
                static_cast<unsigned long long>(initial),
                static_cast<unsigned long long>(final_rate), ratio,
                static_cast<unsigned long long>(bwe.stats().increases),
                static_cast<unsigned long long>(bwe.stats().decreases));

    CHECK_EQ(bwe.state(), GccTrendState::kNormal);
    CHECK_EQ(bwe.stats().decreases, 0U);
    CHECK(bwe.stats().increases > 0U);
    // 约 1.79 秒的有效增长：1.08^1.79 ≈ 1.148，允许 ±8% 的测试窗口。
    CHECK(ratio > 1.10);
    CHECK(ratio < 1.25);
}

// ---------------------------------------------------------------------------
// 场景 3：降速后 hold 期内不增，hold 结束后恢复增长
// ---------------------------------------------------------------------------

void TestHoldAfterDecrease() {
    BweGcc bwe;
    std::uint64_t send = 0;
    std::uint64_t arrival = 50;

    FeedStable(&bwe, &send, &arrival, 40);
    // 持续排队增长，直到第一次降速；一旦发生就停，保证 hold 窗口完整。
    int guard = 0;
    while (bwe.stats().decreases == 0 && guard < 100) {
        send += 10;
        arrival += 13;
        bwe.OnPacketFeedback(send, arrival);
        ++guard;
    }
    CHECK(bwe.stats().decreases >= 1U);
    const std::uint64_t rate_after = bwe.TargetBitrate();
    const std::uint64_t hold_until = bwe.hold_until_ms();
    CHECK(hold_until > arrival);
    CHECK(bwe.IsHolding(arrival));

    // hold 内喂稳定包：状态会回到 normal，但速率必须纹丝不动。
    bool held_stable = true;
    while (arrival + 10 < hold_until) {
        send += 10;
        arrival += 10;
        bwe.OnPacketFeedback(send, arrival);
        if (bwe.TargetBitrate() != rate_after) {
            held_stable = false;
        }
    }
    CHECK(held_stable);
    CHECK_EQ(bwe.TargetBitrate(), rate_after);
    CHECK(bwe.stats().hold_blocks > 0U);

    // 越过 hold 后继续稳定链路，应恢复增长。
    FeedStable(&bwe, &send, &arrival, 30);
    const std::uint64_t after_hold = bwe.TargetBitrate();

    std::printf("[INFO] hold: rate_after=%llu hold_until=%llu after_hold=%llu B/s "
                "hold_blocks=%llu\n",
                static_cast<unsigned long long>(rate_after),
                static_cast<unsigned long long>(hold_until),
                static_cast<unsigned long long>(after_hold),
                static_cast<unsigned long long>(bwe.stats().hold_blocks));

    CHECK(after_hold > rate_after);
}

// ---------------------------------------------------------------------------
// 场景 4：加性增模式（WebRTC 有链路容量估计时的行为）
// ---------------------------------------------------------------------------

void TestAdditiveIncreaseMode() {
    BweGccConfig config;
    config.increase_mode = GccIncreaseMode::kAdditive;
    config.additive_increase_bytes_per_sec = 4000;
    BweGcc bwe(config);
    const std::uint64_t initial = bwe.TargetBitrate();
    std::uint64_t send = 0;
    std::uint64_t arrival = 50;

    FeedStable(&bwe, &send, &arrival, 200);
    const std::uint64_t final_rate = bwe.TargetBitrate();

    std::printf("[INFO] additive: initial=%llu final=%llu (+%llu B/s)\n",
                static_cast<unsigned long long>(initial),
                static_cast<unsigned long long>(final_rate),
                static_cast<unsigned long long>(final_rate - initial));

    CHECK_EQ(bwe.stats().decreases, 0U);
    // 1.79 秒 × 4000 B/s ≈ +7160；允许调度粒度造成的 ±2000。
    CHECK(final_rate > initial + 5000);
    CHECK(final_rate < initial + 9000);
}

// ---------------------------------------------------------------------------
// 场景 5：边界——首包/同组/乱序不改变速率，Reset 回到初始值
// ---------------------------------------------------------------------------

void TestEdgeCases() {
    BweGcc bwe;
    const std::uint64_t initial = bwe.TargetBitrate();

    // 首包只建立基准。
    bwe.OnPacketFeedback(0, 50);
    CHECK_EQ(bwe.stats().trend_samples, 0U);
    CHECK_EQ(bwe.TargetBitrate(), initial);

    // 发送间隔 < 5ms：并入同组，不产生 delta。
    bwe.OnPacketFeedback(2, 52);
    CHECK_EQ(bwe.stats().trend_samples, 0U);

    // 发送时间回退：丢弃并计数。
    bwe.OnPacketFeedback(1, 53);
    CHECK_EQ(bwe.stats().dropped_feedback, 1U);
    CHECK_EQ(bwe.stats().trend_samples, 0U);

    // 到达早于发送：丢弃。
    bwe.OnPacketFeedback(20, 10);
    CHECK_EQ(bwe.stats().dropped_feedback, 2U);

    bwe.Reset();
    CHECK_EQ(bwe.TargetBitrate(), initial);
    CHECK_EQ(bwe.state(), GccTrendState::kNormal);
    CHECK_EQ(bwe.stats().feedback_count, 0U);

    // 0/非法配置必须被归一化，不能出现除零或反向增长。
    BweGccConfig bad;
    bad.trend_window = 0;
    bad.smoothing_factor = 1.5;
    bad.alpha = 0.5;
    bad.beta = 2.0;
    bad.hold_ms = 0;
    BweGcc normalized(bad);
    CHECK(normalized.config().trend_window >= 2U);
    CHECK(normalized.config().smoothing_factor < 1.0);
    CHECK(normalized.config().alpha > 1.0);
    CHECK(normalized.config().beta < 1.0);
    CHECK(normalized.config().hold_ms > 0U);
}

// ---------------------------------------------------------------------------
// 场景 6：对照——延迟上升但无丢包时，GCC 降速而 BBR 完全无反应
// ---------------------------------------------------------------------------

void TestBbrIgnoresDelayRise() {
    constexpr int kPackets = 200;
    constexpr std::uint64_t kMss = 1200;

    // --- GCC：稳定链路 vs 排队增长链路 ---
    BweGcc gcc_stable;
    BweGcc gcc_rising;
    std::uint64_t stable_send = 0;
    std::uint64_t stable_arrival = 50;
    std::uint64_t rising_send = 0;
    std::uint64_t rising_arrival = 50;
    for (int i = 0; i < kPackets; ++i) {
        stable_send += 10;
        stable_arrival += 10;
        gcc_stable.OnPacketFeedback(stable_send, stable_arrival);

        rising_send += 10;
        // 前 40 包稳定，之后每包多 3ms 排队（无丢包，仅延迟上升）。
        rising_arrival += (i < 40 ? 10 : 13);
        gcc_rising.OnPacketFeedback(rising_send, rising_arrival);
    }
    const std::uint64_t gcc_stable_rate = gcc_stable.TargetBitrate();
    const std::uint64_t gcc_rising_rate = gcc_rising.TargetBitrate();

    // --- BBR：同样的发送/确认节奏，只把 RTT 样本从 50ms 抬到 150ms ---
    CongestionConfig bbr_config;
    bbr_config.algorithm = CongestionAlgorithm::kBbr;
    bbr_config.pacing_enabled = false;
    bbr_config.initial_rtt_ms = 100;
    bbr_config.initial_cwnd_packets = 64;
    CongestionController bbr_stable(bbr_config);
    CongestionController bbr_rising(bbr_config);
    bool bbr_trajectory_identical = true;
    std::uint64_t now = 10;
    for (int i = 0; i < kPackets; ++i) {
        AckSample ack;
        ack.now_ms = now;
        ack.delivered_bytes = kMss;
        ack.send_elapsed_ms = 10;
        ack.holes = 0;
        ack.retransmitted = false;

        ack.rtt_ms = 50;  // 稳定 RTT
        bbr_stable.OnAck(ack);
        ack.rtt_ms = 50 + static_cast<std::uint32_t>(i / 2);  // 50 → 149ms
        bbr_rising.OnAck(ack);
        // 每一步都比较：若 RTT 上升真的影响 BBR，轨迹会在某一步分叉。
        if (bbr_stable.pacing_bytes_per_sec() != bbr_rising.pacing_bytes_per_sec() ||
            bbr_stable.cwnd_packets() != bbr_rising.cwnd_packets()) {
            bbr_trajectory_identical = false;
        }
        now += 10;
    }
    const std::uint64_t bbr_stable_pacing = bbr_stable.pacing_bytes_per_sec();
    const std::uint64_t bbr_rising_pacing = bbr_rising.pacing_bytes_per_sec();

    const double gcc_change = 100.0 * (static_cast<double>(gcc_rising_rate) -
                                       static_cast<double>(gcc_stable_rate)) /
                              static_cast<double>(gcc_stable_rate);

    std::printf("[INFO] compare: gcc stable=%llu rising=%llu B/s (%+.1f%%), "
                "bbr stable=%llu rising=%llu B/s (%+.1f%%), bbr loss=%llu, "
                "bbr cwnd=%u/%u\n",
                static_cast<unsigned long long>(gcc_stable_rate),
                static_cast<unsigned long long>(gcc_rising_rate), gcc_change,
                static_cast<unsigned long long>(bbr_stable_pacing),
                static_cast<unsigned long long>(bbr_rising_pacing),
                bbr_stable_pacing == 0
                    ? 0.0
                    : 100.0 * (static_cast<double>(bbr_rising_pacing) -
                               static_cast<double>(bbr_stable_pacing)) /
                          static_cast<double>(bbr_stable_pacing),
                static_cast<unsigned long long>(bbr_rising.stats().loss_events),
                static_cast<unsigned>(bbr_stable.cwnd_packets()),
                static_cast<unsigned>(bbr_rising.cwnd_packets()));

    // GCC：延迟上升导致明确降速。
    CHECK_EQ(gcc_rising.state(), GccTrendState::kOverusing);
    CHECK(gcc_rising_rate < gcc_stable_rate);
    CHECK(gcc_change < -10.0);

    // BBR：无丢包时，RTT 从 50ms 抬到 150ms 对 pacing/cwnd 零影响。
    CHECK_EQ(bbr_rising.stats().loss_events, 0U);
    CHECK_EQ(bbr_stable_pacing, bbr_rising_pacing);
    CHECK_EQ(bbr_stable.cwnd_packets(), bbr_rising.cwnd_packets());
    CHECK(bbr_trajectory_identical);
}

}  // namespace

int main() {
    TestDelayRiseDecreasesRate();
    TestStableSlowIncrease();
    TestHoldAfterDecrease();
    TestAdditiveIncreaseMode();
    TestEdgeCases();
    TestBbrIgnoresDelayRise();
    return Finish("test_bwe_gcc");
}
