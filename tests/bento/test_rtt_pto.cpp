/**
 * @file test_rtt_pto.cpp
 * @brief RFC 9002 SRTT/RTTVAR/PTO 自适应、Karn 采样、固定/动态对比。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "go2cpp/bento/net/relation.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

/// @brief 假时钟 + 单通道，便于注入确定性的 ACK 时刻。
struct Harness {
    Relation relation;
    std::uint64_t now = 1000;
    std::vector<std::vector<std::uint8_t>> sent;
    std::unique_ptr<ReliableChannel> channel;

    explicit Harness(ReliableChannel::Config config) {
        channel = std::make_unique<ReliableChannel>(
            0, &relation, [](std::uint32_t, const std::uint8_t*, std::size_t) {}, config,
            [this]() { return now; });
        channel->SetFrameSink([this](const std::uint8_t* data, std::size_t bytes) {
            sent.emplace_back(data, data + bytes);
            return true;
        });
    }

    void SendOne() {
        const std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        BENTO_CHECK(channel->Send(payload, sizeof(payload)));
    }

    /// @brief 确认当前所有已分配 seq（消息均已发出，因此全部在途或已确认）。
    void AckAll() {
        AckState ack;
        ack.ack_seq = channel->next_tx_seq();
        ack.ack_bits = 0;
        channel->OnAck(ack);
    }
};

void TestEstimatorFormula() {
    RttEstimator estimator;
    BENTO_CHECK(!estimator.has_sample());
    // 无样本：initial_rtt=200、rttvar=100，PTO = 200+4*100 = 600。
    BENTO_CHECK_EQ(estimator.base_pto_ms(), 600U);

    estimator.OnSample(100);
    BENTO_CHECK_EQ(estimator.smoothed_rtt_ms(), 100U);
    BENTO_CHECK_EQ(estimator.rttvar_ms(), 50U);
    BENTO_CHECK_EQ(estimator.base_pto_ms(), 300U);  // 100 + 200（不含 ACK 补偿）

    estimator.OnSample(200);
    // rttvar = (3*50 + |100-200|)/4 = 62；srtt = (7*100 + 200)/8 = 112。
    BENTO_CHECK_EQ(estimator.rttvar_ms(), 62U);
    BENTO_CHECK_EQ(estimator.smoothed_rtt_ms(), 112U);
    BENTO_CHECK_EQ(estimator.base_pto_ms(), 360U);  // 112 + 248

    BENTO_CHECK_EQ(estimator.BackoffPtoMs(0), 360U);
    BENTO_CHECK_EQ(estimator.BackoffPtoMs(1), 720U);
    BENTO_CHECK_EQ(estimator.BackoffPtoMs(3), 2000U);  // 3080 -> max
}

void TestConfigBoundsAndFixedMode() {
    RttEstimator::Config config;
    config.min_pto_ms = 500;
    config.max_pto_ms = 2000;
    config.max_ack_delay_ms = 0;
    RttEstimator floor_estimator(config);
    floor_estimator.OnSample(10);  // 10 + 20 = 30，被下限抬到 500。
    BENTO_CHECK_EQ(floor_estimator.base_pto_ms(), 500U);

    config.min_pto_ms = 200;
    config.max_pto_ms = 300;
    RttEstimator ceiling_estimator(config);
    for (int i = 0; i < 20; ++i) {
        ceiling_estimator.OnSample(400);
    }
    BENTO_CHECK_EQ(ceiling_estimator.base_pto_ms(), 300U);

    config.min_pto_ms = 200;
    config.max_pto_ms = 2000;
    config.fixed_pto_ms = 200;
    RttEstimator fixed_estimator(config);
    fixed_estimator.OnSample(400);
    BENTO_CHECK_EQ(fixed_estimator.base_pto_ms(), 200U);
    BENTO_CHECK_EQ(fixed_estimator.BackoffPtoMs(1), 400U);
    BENTO_CHECK_EQ(fixed_estimator.BackoffPtoMs(4), 2000U);
}

struct DelayResult {
    std::uint32_t pto = 0;
    std::uint32_t srtt = 0;
    std::uint32_t rttvar = 0;
};

/// @brief 连续注入 12 个固定 RTT 样本，让 srtt/rttvar 收敛。
DelayResult RunDelay(std::uint32_t delay_ms) {
    ReliableChannel::Config config;
    Harness harness(config);
    for (int i = 0; i < 12; ++i) {
        harness.SendOne();
        harness.now += delay_ms;
        harness.AckAll();
    }
    DelayResult result;
    result.pto = harness.channel->base_pto_ms();
    result.srtt = harness.channel->rtt_estimator().smoothed_rtt_ms();
    result.rttvar = harness.channel->rtt_estimator().rttvar_ms();
    return result;
}

void TestAdaptation() {
    const DelayResult slow = RunDelay(50);
    const DelayResult normal = RunDelay(200);
    const DelayResult far = RunDelay(400);

    // 50ms RTT：srtt=50、rttvar 收敛到 0，被 srtt/8 抖动下限抬到
    // 50+4*6=74ms（旧实现被固定 200ms 下限抬到 200ms，在低延迟链路上
    // 每个迟到 ACK 都变成一次伪超时重传）。
    BENTO_CHECK_EQ(slow.pto, 74U);
    BENTO_CHECK(normal.pto >= 250U && normal.pto <= 350U);
    BENTO_CHECK(far.pto >= 500U && far.pto <= 700U);
    BENTO_CHECK(slow.pto <= normal.pto);
    BENTO_CHECK(normal.pto <= far.pto);

    // PTO 随 RTT 单调增长，且与 srtt 的偏差不超过 1 个 RTT（rttvar 收敛后）。
    BENTO_CHECK(slow.pto <= 2U * (slow.srtt + 1U));
    BENTO_CHECK(normal.pto <= 2U * (normal.srtt + 1U));
    BENTO_CHECK(far.pto <= 2U * (far.srtt + 1U));

    BENTO_CHECK(normal.srtt >= 190U && normal.srtt <= 210U);
    BENTO_CHECK(far.srtt >= 380U && far.srtt <= 420U);
    BENTO_CHECK(far.rttvar < far.srtt);

    // 每个结果都必须严格满足 srtt + 4*max(rttvar, srtt/8)，并夹在 [50ms, 2s]。
    const auto expected = [](const DelayResult& result) {
        const std::uint64_t floor = static_cast<std::uint64_t>(result.srtt) / 8ULL;
        const std::uint64_t rttvar = std::max<std::uint64_t>(result.rttvar, floor);
        const std::uint64_t formula =
            static_cast<std::uint64_t>(result.srtt) + 4ULL * rttvar;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(
            std::max<std::uint64_t>(formula, 50ULL), 2000ULL));
    };
    BENTO_CHECK_EQ(slow.pto, expected(slow));
    BENTO_CHECK_EQ(normal.pto, expected(normal));
    BENTO_CHECK_EQ(far.pto, expected(far));

    std::printf("[INFO] pto: rtt50=%ums rtt200=%ums rtt400=%ums srtt400=%ums rttvar400=%ums\n",
                slow.pto, normal.pto, far.pto, far.srtt, far.rttvar);
}

void TestFixedVersusDynamicTimeout() {
    // 400ms RTT 下，固定 200ms 必然在 ACK 到达前超时；动态 PTO 初始 1022ms
    // 不会。这里只触发一次 timer，比较是否发生冗余重传。
    const auto run = [](bool fixed) {
        ReliableChannel::Config config;
        if (fixed) {
            config.rtt.fixed_pto_ms = 200;
        }
        Harness harness(config);
        harness.SendOne();
        harness.now += 200;
        harness.channel->OnTimer(harness.now);
        return static_cast<std::uint32_t>(harness.channel->stats().retransmits);
    };
    BENTO_CHECK_EQ(run(false), 0U);
    BENTO_CHECK_EQ(run(true), 1U);
}

void TestKarnSampling() {
    ReliableChannel::Config config;
    config.rtt.fixed_pto_ms = 200;
    Harness harness(config);
    harness.SendOne();
    harness.now += 200;
    harness.channel->OnTimer(harness.now);
    BENTO_CHECK_EQ(harness.channel->stats().retransmits, 1U);
    BENTO_CHECK_EQ(harness.channel->stats().srtt_ms, 200U);  // 尚未采样

    harness.now += 300;  // 若采样会得到 500ms
    harness.AckAll();
    BENTO_CHECK_EQ(harness.channel->stats().acks_applied, 1U);
    BENTO_CHECK_EQ(harness.channel->stats().srtt_ms, 200U);  // Karn：重传消息不采样
    BENTO_CHECK_EQ(harness.channel->stats().last_rtt_ms, 0U);

    // 下一条未重传消息才产生首样本。
    harness.SendOne();
    harness.now += 120;
    harness.AckAll();
    BENTO_CHECK_EQ(harness.channel->stats().last_rtt_ms, 120U);
    BENTO_CHECK_EQ(harness.channel->stats().srtt_ms, 120U);
    BENTO_CHECK_EQ(harness.channel->rtt_estimator().rttvar_ms(), 60U);
}

}  // namespace

int main() {
    TestEstimatorFormula();
    TestConfigBoundsAndFixedMode();
    TestAdaptation();
    TestFixedVersusDynamicTimeout();
    TestKarnSampling();
    return bento_test::Finish("test_rtt_pto");
}
