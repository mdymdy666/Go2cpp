/**
 * @file test_fast_retransmit.cpp
 * @brief 3 次重复 ACK / SACK 空洞触发快速重传，且严格早于 PTO。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

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

struct SentFrame {
    std::uint32_t seq = 0;
    std::uint8_t flags = 0;
};

struct Harness {
    Relation relation;
    std::uint64_t now = 1000;
    std::uint64_t start = 1000;
    std::vector<SentFrame> sent;
    std::unique_ptr<ReliableChannel> channel;

    explicit Harness(ReliableChannel::Config config) {
        channel = std::make_unique<ReliableChannel>(
            0, &relation, [](std::uint32_t, const std::uint8_t*, std::size_t) {}, config,
            [this]() { return now; });
        channel->SetFrameSink([this](const std::uint8_t* data, std::size_t bytes) {
            FrameView view;
            BENTO_CHECK(ParseFrame(data, bytes, &view));
            sent.push_back(SentFrame{view.header.seq, view.header.flags});
            return true;
        });
    }

    void SendMessages(int count) {
        for (int i = 0; i < count; ++i) {
            const std::uint8_t payload[8] = {static_cast<std::uint8_t>(i), 1, 2, 3, 4, 5, 6, 7};
            BENTO_CHECK(channel->Send(payload, sizeof(payload)));
        }
    }

    void Ack(std::uint32_t ack_seq, std::uint32_t bits) {
        AckState ack;
        ack.ack_seq = ack_seq;
        ack.ack_bits = bits;
        channel->OnAck(ack);
    }

    std::size_t RetransmitCount() const {
        std::size_t count = 0;
        for (const SentFrame& frame : sent) {
            if ((frame.flags & kFlagRetransmit) != 0) {
                ++count;
            }
        }
        return count;
    }
};

std::uint32_t LastRetransmitSeq(const Harness& harness) {
    std::uint32_t seq = 0;
    for (const SentFrame& frame : harness.sent) {
        if ((frame.flags & kFlagRetransmit) != 0) {
            seq = frame.seq;
        }
    }
    return seq;
}

void TestDuplicateAckThreshold() {
    ReliableChannel::Config config;
    Harness harness(config);
    harness.SendMessages(4);
    BENTO_CHECK_EQ(harness.sent.size(), 4U);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 0U);

    // 第一个 ACK 建立基准；随后 3 个「不推进累积确认、无新 SACK」的 ACK
    // 才是重复 ACK。第 2、3 个不够触发，第 4 个（累计 3 次重复）触发。
    harness.now = 1050;
    harness.Ack(1, 0);
    harness.now = 1060;
    harness.Ack(1, 0);
    harness.now = 1070;
    harness.Ack(1, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 0U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 0U);

    harness.now = 1080;
    harness.Ack(1, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 1U);
    BENTO_CHECK_EQ(LastRetransmitSeq(harness), 1U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 1U);
    BENTO_CHECK_EQ(harness.channel->stats().retransmits, 1U);
    // 快速路径必须早于 PTO（初始 1022ms），这里只过了 80ms。
    BENTO_CHECK(harness.now - harness.start < harness.channel->base_pto_ms());

    // 恢复期内再次重复 ACK 不重复重传（避免重传风暴）。
    harness.now = 1090;
    harness.Ack(1, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 1U);

    // 累积确认前进后恢复，下一轮 3 个重复 ACK 触发 seq=2。
    harness.now = 1200;
    harness.Ack(2, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 1U);
    harness.now = 1210;
    harness.Ack(2, 0);
    harness.now = 1220;
    harness.Ack(2, 0);
    harness.now = 1230;
    harness.Ack(2, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 2U);
    BENTO_CHECK_EQ(LastRetransmitSeq(harness), 2U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 2U);
}

void TestTwoDuplicatesNotEnough() {
    ReliableChannel::Config config;
    Harness harness(config);
    harness.SendMessages(3);
    harness.Ack(1, 0);
    harness.Ack(1, 0);
    harness.Ack(1, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 0U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 0U);
}

void TestSackGapTriggersImmediately() {
    ReliableChannel::Config config;
    Harness harness(config);
    harness.SendMessages(4);
    // ack_seq=1 表示 seq1 缺失；bit1..3 表示 2/3/4 已收到 -> SACK 空洞。
    harness.now = 1050;
    harness.Ack(1, 0x0000000EU);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 1U);
    BENTO_CHECK_EQ(LastRetransmitSeq(harness), 1U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 1U);
    BENTO_CHECK(harness.now - harness.start < harness.channel->base_pto_ms());
}

void TestSackMultipleHoles() {
    ReliableChannel::Config config;
    Harness harness(config);
    harness.SendMessages(6);
    // ack_seq=1；SACK 只确认 2/4/6（bit1/3/5），1/3/5 是三个已知空洞。
    // 一次 ACK 应并行补发三条，而不是等三个 PTO。
    harness.now = 1050;
    harness.Ack(1, 0x0000002AU);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 3U);
    BENTO_CHECK_EQ(harness.channel->stats().fast_retransmits, 3U);

    bool saw_one = false;
    bool saw_three = false;
    bool saw_five = false;
    for (const SentFrame& frame : harness.sent) {
        if ((frame.flags & kFlagRetransmit) == 0) {
            continue;
        }
        saw_one = saw_one || frame.seq == 1U;
        saw_three = saw_three || frame.seq == 3U;
        saw_five = saw_five || frame.seq == 5U;
    }
    BENTO_CHECK(saw_one);
    BENTO_CHECK(saw_three);
    BENTO_CHECK(saw_five);
}

void TestFastRetransmitDisabled() {
    ReliableChannel::Config config;
    config.fast_retransmit = false;
    Harness harness(config);
    harness.SendMessages(3);
    harness.Ack(1, 0);
    harness.Ack(1, 0);
    harness.Ack(1, 0);
    harness.Ack(1, 0);
    BENTO_CHECK_EQ(harness.RetransmitCount(), 0U);
    BENTO_CHECK_EQ(harness.channel->stats().retransmits, 0U);
}

}  // namespace

int main() {
    TestDuplicateAckThreshold();
    TestTwoDuplicatesNotEnough();
    TestSackGapTriggersImmediately();
    TestSackMultipleHoles();
    TestFastRetransmitDisabled();
    return bento_test::Finish("test_fast_retransmit");
}
