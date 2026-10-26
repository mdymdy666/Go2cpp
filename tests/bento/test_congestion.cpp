/**
 * @file test_congestion.cpp
 * @brief 拥塞控制 + pacing：慢启动 / 拥塞避免 / 丢包减半 / 超时回慢启动 /
 *        令牌桶不超速 / ReliableChannel 集成与 Link 统计导出。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/congestion.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/relation.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

void TestSlowStartAndCongestionAvoidance() {
    CongestionController::Config config;
    config.initial_cwnd_packets = 10;
    config.initial_ssthresh_packets = 40;
    config.max_cwnd_packets = 256;
    config.pacing = false;
    CongestionController controller(config);
    BENTO_CHECK_EQ(controller.cwnd(), 10U);
    BENTO_CHECK(controller.slow_start());

    // 慢启动：每确认一个包 +1 MSS（一次 ACK 确认 10 个 = +10）。
    controller.OnAck(10, 100, 1000);
    BENTO_CHECK_EQ(controller.cwnd(), 20U);
    BENTO_CHECK(controller.slow_start());

    controller.OnAck(20, 100, 1100);
    BENTO_CHECK_EQ(controller.cwnd(), 40U);  // 到 ssthresh
    BENTO_CHECK(!controller.slow_start());

    // 拥塞避免：40 个确认（分 40 次）才 +1。
    for (int i = 0; i < 40; ++i) {
        controller.OnAck(1, 100, 1200 + static_cast<std::uint64_t>(i));
    }
    BENTO_CHECK_EQ(controller.cwnd(), 41U);
    BENTO_CHECK_EQ(controller.pacing_rate_bps(), 0U);  // pacing 关闭

    const CongestionStats stats = controller.Snapshot(256);
    BENTO_CHECK_EQ(stats.cwnd_packets, 41U);
    BENTO_CHECK_EQ(stats.ssthresh_packets, 40U);
    BENTO_CHECK_EQ(stats.effective_window, 41U);
    BENTO_CHECK_EQ(stats.packets_acked, 70U);
    BENTO_CHECK(!stats.pacing_enabled);
}

void TestLossHalvesWindow() {
    CongestionController::Config config;
    config.initial_cwnd_packets = 32;
    config.initial_ssthresh_packets = 64;
    config.max_cwnd_packets = 256;
    config.pacing = false;
    CongestionController controller(config);

    controller.OnLoss(5000, 200);
    BENTO_CHECK_EQ(controller.cwnd(), 16U);
    BENTO_CHECK_EQ(controller.ssthresh(), 16U);
    BENTO_CHECK(!controller.slow_start());

    // 同一 SRTT 内的第二次丢包不重复减半（防止连续空洞 ACK 打到最小）。
    controller.OnLoss(5100, 200);
    BENTO_CHECK_EQ(controller.cwnd(), 16U);
    BENTO_CHECK_EQ(controller.Snapshot(256).loss_events, 2U);

    // 超过 SRTT 后再次丢包：8 MSS。
    controller.OnLoss(5300, 200);
    BENTO_CHECK_EQ(controller.cwnd(), 8U);

    // 永不低于 2 MSS。
    for (int i = 0; i < 8; ++i) {
        controller.OnLoss(6000 + static_cast<std::uint64_t>(i) * 1000, 200);
    }
    BENTO_CHECK(controller.cwnd() >= 2U);
    BENTO_CHECK_EQ(controller.cwnd(), 2U);
}

void TestTimeoutReturnsToSlowStart() {
    CongestionController::Config config;
    config.initial_cwnd_packets = 32;
    config.initial_ssthresh_packets = 64;
    config.max_cwnd_packets = 256;
    config.pacing = false;
    CongestionController controller(config);

    controller.OnTimeout(7000);
    BENTO_CHECK_EQ(controller.cwnd(), 2U);
    BENTO_CHECK(controller.slow_start());
    BENTO_CHECK_EQ(controller.Snapshot(256).timeout_events, 1U);
}

void TestPacingBurstAndRate() {
    CongestionController::Config config;
    config.initial_cwnd_packets = 10;
    config.initial_ssthresh_packets = 256;
    config.max_cwnd_packets = 256;
    config.mss_bytes = 1000;
    config.initial_rtt_ms = 100;
    config.pacing = true;
    CongestionController controller(config);
    controller.Reset(0);

    // 初始桶 = cwnd × MSS = 10 包；发完 10 包后令牌耗尽。
    int sent = 0;
    while (controller.AllowSend(0, 0)) {
        controller.OnPacketSent(0, 1000);
        ++sent;
        if (sent > 100) {
            break;
        }
    }
    BENTO_CHECK_EQ(sent, 10);
    BENTO_CHECK_EQ(controller.pacing_rate_bps(), 100000U);  // 10*1000B / 0.1s

    // 50ms 后按 100B/ms 补充 5000B -> 只能再发 5 包。
    while (controller.AllowSend(50, 0)) {
        controller.OnPacketSent(50, 1000);
        ++sent;
        if (sent > 100) {
            break;
        }
    }
    BENTO_CHECK_EQ(sent, 15);

    // 再 10ms 只补充 1000B -> 恰好 1 包，不能更多。
    while (controller.AllowSend(60, 0)) {
        controller.OnPacketSent(60, 1000);
        ++sent;
        if (sent > 100) {
            break;
        }
    }
    BENTO_CHECK_EQ(sent, 16);
    BENTO_CHECK(controller.Snapshot(256).pacing_deferrals > 0);
}

/// @brief 假时钟 + 解析发送帧的 ReliableChannel harness。
struct ChannelHarness {
    Relation relation;
    std::uint64_t now = 1000;
    std::vector<std::vector<std::uint8_t>> sent_frames;
    std::unique_ptr<ReliableChannel> channel;

    explicit ChannelHarness(ReliableChannel::Config config) {
        channel = std::make_unique<ReliableChannel>(
            0, &relation, [](std::uint32_t, const std::uint8_t*, std::size_t) {}, config,
            [this]() { return now; });
        channel->SetFrameSink([this](const std::uint8_t* data, std::size_t bytes) {
            FrameView view;
            BENTO_CHECK(ParseFrame(data, bytes, &view));
            sent_frames.emplace_back(data, data + bytes);
            return true;
        });
    }

    void SendMessages(int count) {
        for (int i = 0; i < count; ++i) {
            const std::uint8_t payload[32] = {static_cast<std::uint8_t>(i), 1, 2, 3, 4, 5, 6, 7};
            BENTO_CHECK(channel->Send(payload, sizeof(payload)));
        }
    }

    void Ack(std::uint32_t ack_seq, std::uint32_t bits = 0) {
        AckState ack;
        ack.ack_seq = ack_seq;
        ack.ack_bits = bits;
        channel->OnAck(ack);
    }
};

void TestReliableChannelSlowStartAndLoss() {
    ReliableChannel::Config config;
    config.congestion.initial_cwnd_packets = 8;
    config.congestion.initial_ssthresh_packets = 64;
    config.congestion.pacing = false;
    ChannelHarness harness(config);

    harness.SendMessages(8);
    BENTO_CHECK_EQ(harness.sent_frames.size(), 8U);
    BENTO_CHECK_EQ(harness.channel->stats().cwnd_packets, 8U);
    BENTO_CHECK_EQ(harness.channel->stats().fragments_sent, 8U);

    // 逐个确认 8 条：慢启动 cwnd 8 -> 16。
    for (std::uint32_t seq = 2; seq <= 9; ++seq) {
        harness.Ack(seq);
    }
    BENTO_CHECK_EQ(harness.channel->stats().cwnd_packets, 16U);
    BENTO_CHECK_EQ(harness.channel->congestion_stats().packets_acked, 8U);

    // 再发 16 条并全部确认：16 -> 32。
    harness.SendMessages(16);
    for (std::uint32_t seq = 10; seq <= 25; ++seq) {
        harness.Ack(seq);
    }
    BENTO_CHECK_EQ(harness.channel->stats().cwnd_packets, 32U);
    BENTO_CHECK_EQ(harness.channel->stats().inflight, 0U);

    // 再发 16 条，制造 SACK 空洞（首包丢失，后续 15 包已收到）。
    const std::uint32_t base = harness.channel->next_tx_seq();
    harness.SendMessages(16);
    BENTO_CHECK_EQ(harness.sent_frames.size(), 40U);
    harness.Ack(base, 0xFFFFFFFEU);

    const auto stats = harness.channel->stats();
    BENTO_CHECK_EQ(stats.cwnd_packets, 16U);  // 32 -> 16
    BENTO_CHECK_EQ(stats.ssthresh_packets, 16U);
    BENTO_CHECK_EQ(stats.loss_events, 1U);
    BENTO_CHECK(stats.fast_retransmits >= 1U);
    BENTO_CHECK(!stats.slow_start);
    BENTO_CHECK(stats.pacing_rate_bps == 0U);
}

void TestReliableChannelPacingLimitsBurst() {
    ReliableChannel::Config config;
    config.congestion.initial_cwnd_packets = 4;
    config.congestion.initial_ssthresh_packets = 4;  // 不增长，固定 4 MSS
    config.congestion.pacing = true;
    config.congestion.mss_bytes = 1200;
    config.congestion.initial_rtt_ms = 100;
    ChannelHarness harness(config);

    harness.SendMessages(20);
    // 初始桶 4 包：立即最多发 4 条，其余排队等令牌。
    BENTO_CHECK_EQ(harness.channel->stats().fragments_sent, 4U);
    BENTO_CHECK_EQ(harness.channel->stats().unsent, 16U);

    // ACK 4 条腾出窗口，但同一毫秒没有令牌，不应补发。
    for (std::uint32_t seq = 2; seq <= 5; ++seq) {
        harness.Ack(seq);
    }
    BENTO_CHECK_EQ(harness.channel->stats().fragments_sent, 4U);

    // 100ms 后令牌补充 4×1200B，可再发 4 条。
    harness.now += 100;
    harness.channel->OnTimer(harness.now);
    BENTO_CHECK_EQ(harness.channel->stats().fragments_sent, 8U);

    // 再过 10ms 只补充 480B < 1 MSS，不允许发送。
    harness.now += 10;
    harness.channel->OnTimer(harness.now);
    BENTO_CHECK_EQ(harness.channel->stats().fragments_sent, 8U);
    BENTO_CHECK(harness.channel->stats().pacing_deferrals > 0);
}

void TestReliableChannelTimeout() {
    ReliableChannel::Config config;
    config.congestion.initial_cwnd_packets = 8;
    config.congestion.initial_ssthresh_packets = 64;
    config.congestion.pacing = false;
    ChannelHarness harness(config);

    harness.SendMessages(4);
    harness.now += 2000;
    harness.channel->OnTimer(harness.now);

    BENTO_CHECK_EQ(harness.channel->stats().cwnd_packets, 2U);
    BENTO_CHECK(harness.channel->stats().slow_start);
    BENTO_CHECK_EQ(harness.channel->stats().timeout_loss_events, 1U);
    BENTO_CHECK(harness.channel->stats().retransmits >= 1U);
}

void TestLinkCongestionStatsExport() {
    LinkConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.peer_timeout_ms = 5000;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    LinkConfig client_config;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.peer_address = "127.0.0.1";
    client_config.peer_port = server.local_port();
    client_config.peer_timeout_ms = 5000;
    client_config.random_seed = 0xC0DE1234U;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));

    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };
    std::uint32_t delivered = 0;
    server.on_stream = [&delivered](std::uint8_t, const std::uint8_t*, std::size_t) {
        ++delivered;
    };

    client.Connect();
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 5000));

    constexpr std::uint32_t kMessages = 30;
    for (std::uint32_t i = 0; i < kMessages; ++i) {
        const std::uint8_t payload[48] = {static_cast<std::uint8_t>(i)};
        BENTO_CHECK(client.SendStream(0, payload, sizeof(payload)));
    }
    BENTO_CHECK(bento_test::PumpUntil(
        client, server,
        [&]() {
            return delivered == kMessages && client.reliable_stats(0).inflight == 0;
        },
        10000));

    const CongestionStats stats = client.congestion_stats(0);
    BENTO_CHECK(stats.cwnd_packets >= stats.min_cwnd_packets);
    BENTO_CHECK(stats.effective_window > 0);
    BENTO_CHECK(stats.packets_sent >= kMessages);
    BENTO_CHECK(stats.pacing_rate_bps > 0);
    BENTO_CHECK(stats.srtt_ms > 0);
    std::printf(
        "[INFO] link congestion: cwnd=%u ssthresh=%u pacing=%lluB/s sent=%llu acked=%llu "
        "loss=%llu timeout=%llu deferrals=%llu\n",
        stats.cwnd_packets, stats.ssthresh_packets,
        static_cast<unsigned long long>(stats.pacing_rate_bps),
        static_cast<unsigned long long>(stats.packets_sent),
        static_cast<unsigned long long>(stats.packets_acked),
        static_cast<unsigned long long>(stats.loss_events),
        static_cast<unsigned long long>(stats.timeout_events),
        static_cast<unsigned long long>(stats.pacing_deferrals));

    client.Close();
    server.Close();
}

}  // namespace

int main() {
    TestSlowStartAndCongestionAvoidance();
    TestLossHalvesWindow();
    TestTimeoutReturnsToSlowStart();
    TestPacingBurstAndRate();
    TestReliableChannelSlowStartAndLoss();
    TestReliableChannelPacingLimitsBurst();
    TestReliableChannelTimeout();
    TestLinkCongestionStatsExport();
    return bento_test::Finish("test_congestion");
}
