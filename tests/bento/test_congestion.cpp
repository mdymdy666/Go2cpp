/**
 * @file test_congestion.cpp
 * @brief 拥塞控制 + pacing：慢启动 / 拥塞避免 / 随机丢包归因 / 丢包减半 /
 *        超时降窗与超时保留窗口 / 令牌桶不超速 / 下限生效 / ReliableChannel
 *        集成与 Link 统计导出。
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

/// @brief 构造一个「关闭随机丢包归因」的配置：复现旧 NewReno 行为。
CongestionController::Config LegacyConfig() {
    CongestionController::Config config;
    config.initial_cwnd_packets = 32;
    config.initial_ssthresh_packets = 64;
    config.max_cwnd_packets = 256;
    config.min_cwnd_packets = 8;
    config.pacing = false;
    config.random_loss_aware = false;
    return config;
}

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
    BENTO_CHECK_EQ(stats.min_rtt_ms, 100U);  // 首个 RTT 样本即 min_rtt
}

void TestLossHalvesWindow() {
    CongestionController controller(LegacyConfig());

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

    // 下限生效：连续降窗不会低于 min_cwnd（弱网默认 8，旧实现为 2）。
    for (int i = 0; i < 8; ++i) {
        controller.OnLoss(6000 + static_cast<std::uint64_t>(i) * 1000, 200);
    }
    BENTO_CHECK_EQ(controller.cwnd(), 8U);
    BENTO_CHECK(controller.cwnd() >= controller.config().min_cwnd_packets);
}

void TestTimeoutReturnsToSlowStart() {
    CongestionController controller(LegacyConfig());

    controller.OnTimeout(7000);
    // 真拥塞型超时：窗口压到下限并回慢启动。
    BENTO_CHECK_EQ(controller.cwnd(), 8U);
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

void TestPacingFloorAndMinRttDenominator() {
    CongestionController::Config config;
    config.initial_cwnd_packets = 8;
    config.min_cwnd_packets = 8;
    config.initial_ssthresh_packets = 64;
    config.mss_bytes = 1200;
    config.initial_rtt_ms = 2000;
    config.min_rtt_ms = 20;
    config.pacing = true;
    config.min_pacing_bytes_per_sec = 12500;  // 100 kbps
    CongestionController controller(config);
    controller.Reset(0);

    // 无 RTT 样本：分母取 SRTT 初值 2000ms -> 8*1200*1000/2000 = 4800 B/s，
    // 被 100 kbps 下限抬到 12500 B/s（旧实现会一路掉到 4.8 kB/s）。
    BENTO_CHECK_EQ(controller.pacing_rate_bps(), 12500U);

    // 有 RTT 样本 100ms：8*1200*1000/100 = 96000 B/s。
    controller.OnAck(0, 100, 0);
    BENTO_CHECK_EQ(controller.pacing_rate_bps(), 96000U);

    // SRTT 被丢包推到 4000ms（10x min_rtt）：分母仍用 min_rtt，速率不衰减。
    controller.OnAck(0, 4000, 10);
    BENTO_CHECK_EQ(controller.srtt_ms(), 4000U);
    BENTO_CHECK_EQ(controller.Snapshot(256).min_rtt_ms, 100U);
    BENTO_CHECK_EQ(controller.pacing_rate_bps(), 96000U);
}

void TestRandomLossKeepsWindow() {
    CongestionController::Config config = LegacyConfig();
    config.random_loss_aware = true;
    CongestionController controller(config);
    // 建立 min_rtt=200ms 基线（acked_packets=0 只刷新 RTT，不增长窗口）。
    controller.OnAck(0, 200, 0);
    BENTO_CHECK_EQ(controller.cwnd(), 32U);

    CongestionController::LossContext isolated;
    isolated.now_ms = 1000;
    isolated.srtt_ms = 200;   // 等于 min_rtt：没有排队膨胀
    isolated.holes = 1;       // 孤立丢包
    isolated.inflight = 8;
    controller.OnLoss(isolated);

    // 随机丢包不降窗、不动 ssthresh，只退出「用丢包换来的指数增长」。
    BENTO_CHECK_EQ(controller.cwnd(), 32U);
    BENTO_CHECK_EQ(controller.ssthresh(), 64U);
    const CongestionStats stats = controller.Snapshot(256);
    BENTO_CHECK_EQ(stats.loss_events, 1U);
    BENTO_CHECK_EQ(stats.random_loss_events, 1U);
    BENTO_CHECK_EQ(stats.congestion_loss_events, 0U);
}

void TestConsecutiveLossHalvesWindow() {
    CongestionController::Config config = LegacyConfig();
    config.random_loss_aware = true;
    config.loss_sample_packets = 4;  // 便于在少量事件内得到丢包率
    CongestionController controller(config);
    controller.OnAck(0, 200, 0);

    // 4 次孤立丢包 -> 丢包率采样 1000‰；仍是孤立空洞，不降窗。
    for (int i = 0; i < 4; ++i) {
        CongestionController::LossContext ctx;
        ctx.now_ms = 1000 + static_cast<std::uint64_t>(i) * 1000;
        ctx.srtt_ms = 200;
        ctx.holes = 1;
        controller.OnLoss(ctx);
    }
    BENTO_CHECK_EQ(controller.cwnd(), 32U);
    BENTO_CHECK_EQ(controller.Snapshot(256).loss_rate_permille, 1000U);

    // 同一窗口多个空洞 + 极高丢包率：归因为拥塞，乘性减窗。
    CongestionController::LossContext burst;
    burst.now_ms = 9000;
    burst.srtt_ms = 200;
    burst.holes = 3;
    controller.OnLoss(burst);
    BENTO_CHECK_EQ(controller.cwnd(), 16U);
    BENTO_CHECK_EQ(controller.ssthresh(), 16U);
    BENTO_CHECK(!controller.slow_start());
}

void TestLossRateEstimate() {
    CongestionController::Config config = LegacyConfig();
    config.random_loss_aware = true;
    config.loss_sample_packets = 8;
    CongestionController controller(config);

    controller.OnAck(6, 200, 0);  // 6 个 ACK 包事件
    BENTO_CHECK_EQ(controller.Snapshot(256).loss_rate_permille, 0U);
    controller.OnLoss(1000, 200);
    controller.OnLoss(2000, 200);
    // 2 丢失 / 8 事件 = 250‰。
    BENTO_CHECK_EQ(controller.Snapshot(256).loss_rate_permille, 250U);
}

void TestTimeoutHalvesWindowOnCongestion() {
    CongestionController::Config config = LegacyConfig();
    config.random_loss_aware = true;
    CongestionController controller(config);
    controller.OnAck(0, 200, 0);    // min_rtt=200ms
    controller.OnAck(0, 600, 10);   // SRTT 3x min_rtt：排队膨胀 -> 拥塞信号

    CongestionController::LossContext ctx;
    ctx.now_ms = 1000;
    ctx.srtt_ms = 600;
    ctx.holes = 1;
    controller.OnTimeout(ctx);

    BENTO_CHECK_EQ(controller.cwnd(), 8U);  // 压到下限
    BENTO_CHECK(controller.slow_start());
    const CongestionStats stats = controller.Snapshot(256);
    BENTO_CHECK_EQ(stats.timeout_events, 1U);
    BENTO_CHECK_EQ(stats.congestion_loss_events, 1U);
}

void TestTimeoutKeepsWindowOnRandomLoss() {
    CongestionController::Config config = LegacyConfig();
    config.random_loss_aware = true;
    CongestionController controller(config);
    controller.OnAck(0, 200, 0);

    CongestionController::LossContext ctx;
    ctx.now_ms = 1000;
    ctx.srtt_ms = 200;  // 无膨胀
    ctx.holes = 1;
    controller.OnTimeout(ctx);

    // 随机丢包引发的 PTO：窗口保留，仍处于慢启动（32 < ssthresh 64）。
    BENTO_CHECK_EQ(controller.cwnd(), 32U);
    BENTO_CHECK(controller.slow_start());
    const CongestionStats stats = controller.Snapshot(256);
    BENTO_CHECK_EQ(stats.random_loss_events, 1U);
    BENTO_CHECK_EQ(stats.congestion_loss_events, 0U);
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

void TestReliableChannelCongestionLossHalvesWindow() {
    ReliableChannel::Config config;
    config.congestion = LegacyConfig();
    config.congestion.random_loss_aware = false;
    config.congestion.initial_cwnd_packets = 8;
    config.congestion.initial_ssthresh_packets = 64;
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

void TestReliableChannelRandomLossKeepsWindow() {
    ReliableChannel::Config config;
    config.congestion.initial_cwnd_packets = 8;
    config.congestion.min_cwnd_packets = 8;
    config.congestion.initial_ssthresh_packets = 64;
    config.congestion.pacing = false;  // 默认开启随机丢包归因
    ChannelHarness harness(config);

    harness.SendMessages(8);
    for (std::uint32_t seq = 2; seq <= 9; ++seq) {
        harness.Ack(seq);
    }
    BENTO_CHECK_EQ(harness.channel->stats().cwnd_packets, 16U);

    // 16 条里首包「丢失」、其余已 SACK：孤立空洞，应判为随机丢包。
    const std::uint32_t base = harness.channel->next_tx_seq();
    harness.SendMessages(16);
    harness.Ack(base, 0xFFFFFFFEU);

    BENTO_CHECK_EQ(harness.channel->stats().cwnd_packets, 16U);  // 不降窗
    BENTO_CHECK_EQ(harness.channel->stats().ssthresh_packets, 64U);
    BENTO_CHECK(harness.channel->stats().fast_retransmits >= 1U);
    const CongestionStats cc = harness.channel->congestion_stats();
    BENTO_CHECK_EQ(cc.loss_events, 1U);
    BENTO_CHECK_EQ(cc.random_loss_events, 1U);
    BENTO_CHECK_EQ(cc.congestion_loss_events, 0U);
}

void TestReliableChannelPacingLimitsBurst() {
    ReliableChannel::Config config;
    config.congestion.initial_cwnd_packets = 4;
    config.congestion.min_cwnd_packets = 4;         // 保持小窗口语义
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
    config.congestion.min_cwnd_packets = 8;
    config.congestion.initial_ssthresh_packets = 64;
    config.congestion.pacing = false;
    ChannelHarness harness(config);

    harness.SendMessages(4);
    harness.now += 2000;
    harness.channel->OnTimer(harness.now);

    // 没有 RTT 样本时无法判定排队膨胀：按随机丢包处理，保留窗口。
    BENTO_CHECK_EQ(harness.channel->stats().cwnd_packets, 8U);
    BENTO_CHECK(harness.channel->stats().slow_start);
    BENTO_CHECK_EQ(harness.channel->stats().timeout_loss_events, 1U);
    BENTO_CHECK(harness.channel->stats().retransmits >= 1U);
}

void TestLinkCongestionStatsExport() {
    LinkConfig server_config;
    server_config.require_encryption = false;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.peer_timeout_ms = 5000;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    LinkConfig client_config;
    client_config.require_encryption = false;
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
    const std::uint64_t pacing_floor =
        client_config.reliable.congestion.min_pacing_bytes_per_sec;
    BENTO_CHECK(stats.cwnd_packets >= stats.min_cwnd_packets);
    BENTO_CHECK(stats.effective_window > 0);
    BENTO_CHECK(stats.packets_sent >= kMessages);
    BENTO_CHECK(stats.pacing_rate_bps >= pacing_floor);
    BENTO_CHECK(stats.srtt_ms > 0);
    BENTO_CHECK(stats.min_rtt_ms > 0);
    BENTO_CHECK_EQ(stats.random_loss_events + stats.congestion_loss_events,
                   stats.loss_events);
    std::printf(
        "[INFO] link congestion: cwnd=%u ssthresh=%u pacing=%lluB/s sent=%llu acked=%llu "
        "loss=%llu timeout=%llu random_loss=%llu congestion_loss=%llu deferrals=%llu\n",
        stats.cwnd_packets, stats.ssthresh_packets,
        static_cast<unsigned long long>(stats.pacing_rate_bps),
        static_cast<unsigned long long>(stats.packets_sent),
        static_cast<unsigned long long>(stats.packets_acked),
        static_cast<unsigned long long>(stats.loss_events),
        static_cast<unsigned long long>(stats.timeout_events),
        static_cast<unsigned long long>(stats.random_loss_events),
        static_cast<unsigned long long>(stats.congestion_loss_events),
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
    TestPacingFloorAndMinRttDenominator();
    TestRandomLossKeepsWindow();
    TestConsecutiveLossHalvesWindow();
    TestLossRateEstimate();
    TestTimeoutHalvesWindowOnCongestion();
    TestTimeoutKeepsWindowOnRandomLoss();
    TestReliableChannelCongestionLossHalvesWindow();
    TestReliableChannelRandomLossKeepsWindow();
    TestReliableChannelPacingLimitsBurst();
    TestReliableChannelTimeout();
    TestLinkCongestionStatsExport();
    return bento_test::Finish("test_congestion");
}
