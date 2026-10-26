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

bool WithinPercent50(std::uint32_t actual, std::uint32_t reference) {
    return static_cast<std::uint64_t>(actual) * 2ULL >= reference &&
           static_cast<std::uint64_t>(actual) * 2ULL <=
               static_cast<std::uint64_t>(reference) * 3ULL;
}

void TestEstimatorFormula() {
    RttEstimator estimator;
    BENTO_CHECK(!estimator.has_sample());
    // 初始 333ms、rttvar=166ms、max_ack_delay=25ms -> 333+664+25=1022。
    BENTO_CHECK_EQ(estimator.base_pto_ms(), 1022U);

    estimator.OnSample(100);
    BENTO_CHECK_EQ(estimator.smoothed_rtt_ms(), 100U);
    BENTO_CHECK_EQ(estimator.rttvar_ms(), 50U);
    BENTO_CHECK_EQ(estimator.base_pto_ms(), 325U);  // 100 + 200 + 25

    estimator.OnSample(200);
    // rttvar = (3*50 + |100-200|)/4 = 62；srtt = (7*100 + 200)/8 = 112。
    BENTO_CHECK_EQ(estimator.rttvar_ms(), 62U);
    BENTO_CHECK_EQ(estimator.smoothed_rtt_ms(), 112U);
    BENTO_CHECK_EQ(estimator.base_pto_ms(), 385U);  // 112 + 248 + 25

    BENTO_CHECK_EQ(estimator.BackoffPtoMs(0), 385U);
    BENTO_CHECK_EQ(estimator.BackoffPtoMs(1), 770U);
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

    // 50ms RTT：公式值 50+4*rttvar+25 低于 200ms 下限，应被夹到 200。
    BENTO_CHECK_EQ(slow.pto, 200U);
    BENTO_CHECK(normal.pto >= 200U && normal.pto <= 300U);
    BENTO_CHECK(far.pto >= 400U && far.pto <= 550U);
    BENTO_CHECK(slow.pto <= normal.pto);
    BENTO_CHECK(normal.pto <= far.pto);

    // 延迟越大 PTO 越大，且相对「max(RTT, 下限)」落在 ±50% 内。
    BENTO_CHECK(WithinPercent50(slow.pto, 200U));
    BENTO_CHECK(WithinPercent50(normal.pto, 200U));
    BENTO_CHECK(WithinPercent50(far.pto, 400U));

    BENTO_CHECK(normal.srtt >= 190U && normal.srtt <= 210U);
    BENTO_CHECK(far.srtt >= 380U && far.srtt <= 420U);
    BENTO_CHECK(far.rttvar < far.srtt);

    // 每个结果都必须严格满足 PTO 公式（含 25ms ACK 延迟补偿与上下限）。
    const auto expected = [](const DelayResult& result) {
        const std::uint64_t formula = static_cast<std::uint64_t>(result.srtt) +
                                     4ULL * result.rttvar + 25ULL;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(
            std::max<std::uint64_t>(formula, 200ULL), 2000ULL));
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
    BENTO_CHECK_EQ(harness.channel->stats().srtt_ms, 333U);  // 尚未采样

    harness.now += 300;  // 若采样会得到 500ms
    harness.AckAll();
    BENTO_CHECK_EQ(harness.channel->stats().acks_applied, 1U);
    BENTO_CHECK_EQ(harness.channel->stats().srtt_ms, 333U);  // Karn：重传消息不采样
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
