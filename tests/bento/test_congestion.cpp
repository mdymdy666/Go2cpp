/**
 * @file test_congestion.cpp
 * @brief BBR 拥塞控制单测：SRTT/RTTVAR/RTO、带宽 max-filter、令牌桶 pacing、
 *        STARTUP/DRAIN/PROBE_BW/PROBE_RTT 状态机、inflight_hi、NewReno 对照，
 *        以及真实 UDP 回环 + 弱网代理下的通道集成（Pump 窗口/快速重传）。
 * @details 控制器用例全部使用显式时间戳，保证确定性；集成用例使用真实
 *          UDP 与 tests/bento/udp_proxy.hpp 的丢包代理。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/congestion.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "test_util.hpp"
#include "udp_proxy.hpp"

namespace {

using go2cpp::bento::AckSample;
using go2cpp::bento::BbrBandwidthEstimator;
using go2cpp::bento::BbrRttEstimator;
using go2cpp::bento::BbrState;
using go2cpp::bento::CongestionAlgorithm;
using go2cpp::bento::CongestionConfig;
using go2cpp::bento::CongestionController;
using go2cpp::bento::Link;
using go2cpp::bento::LinkConfig;
using go2cpp::bento::LossSample;
using go2cpp::bento::NowMs;
using go2cpp::bento::Pacer;

// ---------------------------------------------------------------------------
// RTT 估计与 RTO 公式
// ---------------------------------------------------------------------------

void TestRttEstimator() {
    BbrRttEstimator rtt(100);
    BENTO_CHECK(!rtt.has_sample());
    // 无样本：max(initial×6, min_rto) —— 必须大于首个 RTT，否则首个样本
    // 到达前包就被 PTO 重传，Karn 会把全部 RTT 样本过滤掉。
    BENTO_CHECK_EQ(rtt.RtoMs(100, 2000), 600U);
    BENTO_CHECK_EQ(rtt.RtoMs(100, 300), 300U);

    rtt.AddSample(100, 1000);
    BENTO_CHECK(rtt.has_sample());
    BENTO_CHECK_EQ(rtt.srtt_ms(), 100U);
    BENTO_CHECK_EQ(rtt.rttvar_ms(), 50U);
    BENTO_CHECK_EQ(rtt.min_rtt_ms(), 100U);
    // srtt + 4×rttvar = 100 + 200 = 300，大于 2×min_rtt = 200。
    BENTO_CHECK_EQ(rtt.RtoMs(100, 2000), 300U);

    // 采样稳定后 rttvar 收敛到下限 1，此时 RTO 由 2×min_rtt 兜底。
    for (int i = 0; i < 20; ++i) {
        rtt.AddSample(100, 1100 + static_cast<std::uint64_t>(i) * 100);
    }
    BENTO_CHECK_EQ(rtt.srtt_ms(), 100U);
    // 估计器层只给 srtt + 4×rttvar；2×min_rtt 的下限在控制器 RtoMs() 里。
    BENTO_CHECK_EQ(rtt.RtoMs(1, 2000), 104U);

    // min_rtt 是滑窗最小值：出现更小样本后立即下降。
    rtt.AddSample(40, 5000);
    BENTO_CHECK_EQ(rtt.min_rtt_ms(), 40U);
    // srtt=(7×100+40)/8=92，rttvar=(3×1+60)/4=15 → 92+60=152。
    BENTO_CHECK_EQ(rtt.RtoMs(1, 2000), 152U);

    // 窗口（10s）过期后旧样本被清理：只剩 40ms 的样本时 min_rtt 不变。
    rtt.OnTimer(60000);
    BENTO_CHECK_EQ(rtt.min_rtt_ms(), 40U);
}

// ---------------------------------------------------------------------------
// 带宽估计（滑窗最大值）
// ---------------------------------------------------------------------------

void TestBandwidthMaxFilter() {
    BbrBandwidthEstimator bw(10);  // 窗口 = 10 × 500ms = 5s
    BENTO_CHECK(!bw.has_sample());
    for (int i = 0; i < 5; ++i) {
        bw.AddSample(50000, 100, 1000 + static_cast<std::uint64_t>(i) * 100);  // 500kB/s
    }
    BENTO_CHECK_EQ(bw.max_bytes_per_sec(), 500000U);
    // 后续低速率样本不能压低 max-filter（这正是 BBR 抗 ACK 聚合的关键）。
    bw.AddSample(10000, 100, 2000);
    BENTO_CHECK_EQ(bw.max_bytes_per_sec(), 500000U);
    BENTO_CHECK_EQ(bw.latest_bytes_per_sec(), 100000U);
    // 超过时间窗口后峰值样本被淘汰，估计回落到最新样本。
    bw.AddSample(10000, 100, 9000);
    BENTO_CHECK(bw.max_bytes_per_sec() <= 100000U);
    bw.Reset();
    BENTO_CHECK(!bw.has_sample());
}

// ---------------------------------------------------------------------------
// 令牌桶 pacing
// ---------------------------------------------------------------------------

void TestPacerTokenBucket() {
    Pacer pacer;
    pacer.Configure(1200, 5, true);
    pacer.SetRate(120000, 0);  // 120kB/s → capacity = 2400B（>= 2×MSS）
    BENTO_CHECK_EQ(pacer.rate_bytes_per_sec(), 120000U);
    BENTO_CHECK(pacer.available_bytes() >= 2400U);
    BENTO_CHECK(pacer.Consume(1200));
    BENTO_CHECK(pacer.Consume(1200));
    BENTO_CHECK(!pacer.Consume(1));  // 令牌耗尽 → 调用方应延后发送
    pacer.OnTick(10);               // 10ms × 120kB/s = 1200B
    BENTO_CHECK_EQ(pacer.available_bytes(), 1200U);
    BENTO_CHECK(pacer.Consume(1200));
    // 长时间空闲不能一次性补满超过容量。
    pacer.OnTick(100000);
    BENTO_CHECK(pacer.available_bytes() <= 2400U);
    // 关闭 pacing 时 Consume 恒 true。
    Pacer disabled;
    disabled.Configure(1200, 5, false);
    disabled.SetRate(1, 0);
    BENTO_CHECK(disabled.Consume(1200));
}

// ---------------------------------------------------------------------------
// BBR 状态机
// ---------------------------------------------------------------------------

void FeedAcks(CongestionController* cc, std::uint64_t* now, int count, std::uint64_t delivered,
              std::uint32_t rtt_ms) {
    for (int i = 0; i < count; ++i) {
        *now += 10;
        AckSample sample;
        sample.now_ms = *now;
        sample.rtt_ms = rtt_ms;
        sample.delivered_bytes = delivered;
        sample.send_elapsed_ms = rtt_ms;
        cc->OnAck(sample);
    }
}

void TestBbrStartupToProbeBw() {
    CongestionConfig config;
    config.pacing_enabled = false;
    config.initial_cwnd_packets = 16;
    config.min_cwnd_packets = 4;
    config.max_cwnd_packets = 256;
    CongestionController cc(config);
    BENTO_CHECK(cc.algorithm() == CongestionAlgorithm::kBbr);
    BENTO_CHECK(cc.bbr_state() == BbrState::kStartup);

    std::uint64_t now = 1;
    for (int round = 0; round < 8; ++round) {
        FeedAcks(&cc, &now, 4, 48000, 100);  // 480kB/s
        now += 120;                          // 超过 min_rtt=100 → 推进轮次
        cc.OnTick(now);
    }
    const auto stats = cc.stats();
    BENTO_CHECK(stats.rounds >= 4U);
    BENTO_CHECK(stats.cwnd_packets > 16U);                 // startup 窗口增长
    BENTO_CHECK(stats.bandwidth_bps >= 400000U);           // 带宽估计接近 480kB/s
    BENTO_CHECK(stats.srtt_ms >= 100U);
    BENTO_CHECK(cc.bbr_state() != BbrState::kStartup);     // 带宽不再增长 → 退出
    BENTO_CHECK(stats.startup_exits >= 1U);
    BENTO_CHECK(stats.pacing_bps > 0U);
    // 控制器 RTO 必须同时覆盖 srtt+4×rttvar 与 2×min_rtt。
    BENTO_CHECK(cc.RtoMs() >= 2U * cc.min_rtt_ms());
}

void TestBbrRandomLossKeepsWindow() {
    CongestionConfig config;
    config.pacing_enabled = false;
    config.initial_cwnd_packets = 64;
    CongestionController cc(config);
    std::uint64_t now = 1;
    FeedAcks(&cc, &now, 4, 120000, 100);
    now += 120;
    cc.OnTick(now);

    const std::size_t cwnd_before = cc.cwnd_packets();
    const std::uint64_t pacing_before = cc.pacing_bytes_per_sec();
    for (int i = 0; i < 20; ++i) {
        now += 5;
        LossSample loss;
        loss.now_ms = now;
        loss.holes = 1;
        loss.inflight_bytes = cwnd_before * config.mss_bytes / 2;  // 未超窗
        loss.srtt_ms = 100;
        loss.min_rtt_ms = 100;
        loss.timeout = false;
        cc.OnLoss(loss);
    }
    // BBR 的核心性质：孤立随机丢包既不降窗也不降速。
    BENTO_CHECK_EQ(cc.cwnd_packets(), cwnd_before);
    BENTO_CHECK_EQ(cc.pacing_bytes_per_sec(), pacing_before);
    BENTO_CHECK_EQ(cc.stats().loss_events, 20U);
    // 真拥塞（在一次 probe_bw 轮次里 in-flight 超过窗口且多空洞）才收缩。
    std::uint64_t probe_now = now;
    for (int round = 0; round < 6; ++round) {
        FeedAcks(&cc, &probe_now, 4, 60000, 100);
        probe_now += 120;
        cc.OnTick(probe_now);
    }
    LossSample congestion;
    congestion.now_ms = probe_now;
    congestion.holes = 3;
    congestion.inflight_bytes = (cc.cwnd_packets() + 16) * config.mss_bytes;
    congestion.srtt_ms = 100;
    congestion.min_rtt_ms = 100;
    cc.OnLoss(congestion);
    if (cc.bbr_state() != BbrState::kStartup) {
        BENTO_CHECK(cc.stats().inflight_hi_packets > 0U);
    }
}

void TestBbrProbeRtt() {
    CongestionConfig config;
    config.pacing_enabled = false;
    config.initial_cwnd_packets = 64;
    config.min_cwnd_packets = 4;
    config.probe_rtt_interval_ms = 1000;
    config.probe_rtt_duration_ms = 100;
    CongestionController cc(config);
    cc.OnTick(1);  // 初始化 ProbeRTT 计时
    std::uint64_t now = 1;
    FeedAcks(&cc, &now, 4, 240000, 100);
    BENTO_CHECK(cc.bbr_state() == BbrState::kStartup);

    cc.OnTick(1002);
    BENTO_CHECK(cc.bbr_state() == BbrState::kProbeRtt);
    BENTO_CHECK_EQ(cc.stats().probe_rtt_entries, 1U);
    BENTO_CHECK_EQ(cc.cwnd_packets(), config.min_cwnd_packets);  // 压到最小值刷 min_rtt

    cc.OnTick(1103);  // 持续 100ms 后退出
    BENTO_CHECK(cc.bbr_state() != BbrState::kProbeRtt);
    BENTO_CHECK(cc.cwnd_packets() > config.min_cwnd_packets);
}

void TestBbrTimeoutDoesNotCollapse() {
    CongestionConfig config;
    config.pacing_enabled = false;
    config.initial_cwnd_packets = 48;
    CongestionController cc(config);
    std::uint64_t now = 1;
    FeedAcks(&cc, &now, 8, 96000, 100);
    const std::size_t cwnd_before = cc.cwnd_packets();
    LossSample timeout;
    timeout.now_ms = now;
    timeout.timeout = true;
    timeout.inflight_bytes = cwnd_before * config.mss_bytes;
    timeout.holes = 1;
    timeout.srtt_ms = 100;
    timeout.min_rtt_ms = 100;
    cc.OnTimeout(timeout);
    // PTO 超时只统计，不塌窗（对比 NewReno 的 cwnd=min）。
    BENTO_CHECK_EQ(cc.cwnd_packets(), cwnd_before);
    BENTO_CHECK_EQ(cc.stats().timeout_events, 1U);
}

// ---------------------------------------------------------------------------
// NewReno 对照
// ---------------------------------------------------------------------------

void TestNewRenoLossAndTimeout() {
    CongestionConfig config;
    config.algorithm = CongestionAlgorithm::kNewReno;
    config.pacing_enabled = false;
    config.initial_cwnd_packets = 16;
    config.min_cwnd_packets = 4;
    config.max_cwnd_packets = 256;
    CongestionController cc(config);
    std::uint64_t now = 1;
    FeedAcks(&cc, &now, 8, 24000, 100);  // 慢启动：每 ACK +20 包
    const std::size_t grown = cc.cwnd_packets();
    BENTO_CHECK(grown > 16U);

    LossSample loss;
    loss.now_ms = now;
    loss.holes = 1;
    loss.inflight_bytes = grown * config.mss_bytes;
    loss.srtt_ms = 100;
    loss.min_rtt_ms = 100;
    cc.OnLoss(loss);
    BENTO_CHECK(cc.cwnd_packets() < grown);       // 乘性减
    BENTO_CHECK(cc.stats().cwnd_reductions >= 1U);  // BBR 不会有这个计数

    LossSample timeout;
    timeout.now_ms = now + 200;  // 超过 srtt=100 的「同 RTT 只减一次」守卫
    timeout.timeout = true;
    timeout.inflight_bytes = cc.cwnd_packets() * config.mss_bytes;
    timeout.srtt_ms = 100;
    timeout.min_rtt_ms = 100;
    cc.OnTimeout(timeout);
    BENTO_CHECK_EQ(cc.cwnd_packets(), config.min_cwnd_packets);
    BENTO_CHECK(cc.ssthresh() >= cc.cwnd_packets());  // 超时后 ssthresh 不低于窗口
}

void TestNoneKeepsLegacyBehavior() {
    CongestionConfig config;
    config.enabled = false;  // 兼容旧开关：等价于 algorithm=kNone
    CongestionController cc(config);
    BENTO_CHECK(!cc.enabled());
    BENTO_CHECK(cc.CanSend(1U << 24));
    BENTO_CHECK(cc.ConsumePacing(1U << 24));
    LossSample loss;
    loss.now_ms = 1;
    loss.inflight_bytes = 1U << 20;
    cc.OnLoss(loss);
    BENTO_CHECK_EQ(cc.stats().loss_events, 1U);
}

// ---------------------------------------------------------------------------
// 真实 UDP 回环 + 弱网代理：通道集成
// ---------------------------------------------------------------------------

struct PairOptions {
    CongestionAlgorithm algorithm = CongestionAlgorithm::kBbr;
    int loss_percent = 0;
    int delay_ms = 0;
    std::uint32_t seed = 0xC0FFEE01U;
    bool fec = false;
};

struct PairResult {
    std::vector<std::uint32_t> received;
    go2cpp::bento::ReliableChannel::Stats sender_reliable;
    go2cpp::bento::CongestionStats sender_congestion;
    bool connected = false;
};

std::vector<std::uint8_t> MakePayload(std::uint32_t seq, std::size_t bytes) {
    std::vector<std::uint8_t> payload(bytes, 0);
    payload[0] = static_cast<std::uint8_t>(seq >> 24);
    payload[1] = static_cast<std::uint8_t>(seq >> 16);
    payload[2] = static_cast<std::uint8_t>(seq >> 8);
    payload[3] = static_cast<std::uint8_t>(seq);
    for (std::size_t i = 4; i < bytes; ++i) {
        payload[i] = static_cast<std::uint8_t>((seq * 31U + i * 7U) & 0xFFU);
    }
    return payload;
}

bool PairResultRun(const PairOptions& options, std::uint32_t messages, std::size_t payload_bytes,
                   PairResult* out) {
    LinkConfig server_config;
    server_config.require_encryption = false;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 8000;
    server_config.reliable.congestion.algorithm = options.algorithm;
    server_config.reliable.fec.enabled = options.fec;
    if (options.fec) {
        server_config.reliable.fec.kind = go2cpp::bento::FecKind::kXor;
        server_config.reliable.fec.data_shards = 8;
        server_config.reliable.fec.parity_shards = 1;
        server_config.reliable.fec.max_payload_bytes = 900;
    }
    Link server(server_config);
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "[FAIL] server.Open: %s\n", error.c_str());
        return false;
    }

    LinkConfig client_config;
    client_config.require_encryption = false;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.peer_address = "127.0.0.1";
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 8000;
    client_config.random_seed = options.seed;
    client_config.reliable.congestion.algorithm = options.algorithm;
    client_config.reliable.fec = server_config.reliable.fec;

    std::unique_ptr<bento_test::UdpProxy> proxy;
    if (options.loss_percent > 0 || options.delay_ms > 0) {
        sockaddr_in address{};
        if (!go2cpp::bento::UdpSocket::ResolveIPv4("127.0.0.1", server.local_port(), &address,
                                                   &error)) {
            return false;
        }
        proxy = std::make_unique<bento_test::UdpProxy>(address, options.loss_percent,
                                                       options.delay_ms, options.seed);
        if (!proxy->Start(&error)) {
            return false;
        }
        client_config.peer_port = proxy->port();
    } else {
        client_config.peer_port = server.local_port();
    }

    Link client(client_config);
    if (!client.Open(&error)) {
        std::fprintf(stderr, "[FAIL] client.Open: %s\n", error.c_str());
        return false;
    }
    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };
    server.on_stream = [&out](std::uint8_t channel, const std::uint8_t* data, std::size_t bytes) {
        if (channel != 0 || bytes < 4) {
            return;
        }
        const std::uint32_t seq = (static_cast<std::uint32_t>(data[0]) << 24) |
                                  (static_cast<std::uint32_t>(data[1]) << 16) |
                                  (static_cast<std::uint32_t>(data[2]) << 8) |
                                  static_cast<std::uint32_t>(data[3]);
        out->received.push_back(seq);
    };
    client.Connect();
    out->connected = bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 15000);
    if (!out->connected) {
        std::fprintf(stderr, "[DIAG] connect failed server=%d client=%d",
                     static_cast<int>(server_connected), static_cast<int>(client_connected));
        return false;
    }
    for (std::uint32_t i = 0; i < messages; ++i) {
        const std::vector<std::uint8_t> payload = MakePayload(i, payload_bytes);
        bool accepted = client.SendStream(0, payload.data(), payload.size());
        for (int retry = 0; !accepted && retry < 200; ++retry) {
            bento_test::PumpUntil(client, server, []() { return false; }, 5);
            accepted = client.SendStream(0, payload.data(), payload.size());
        }
        if (!accepted) {
            std::fprintf(stderr, "[DIAG] SendStream failed at %u", i);
            return false;
        }
    }
    const bool all = bento_test::PumpUntil(
        client, server,
        [&]() {
            return out->received.size() == messages &&
                   client.reliable_stats(0).inflight == 0 &&
                   server.reliable_stats(0).inflight == 0;
        },
        30000);
    out->sender_reliable = client.reliable_stats(0);
    out->sender_congestion = client.congestion_stats(0);
    std::fprintf(stderr, "[DIAG] all=%d recv=%zu inflight=%zu srtt=%u cwnd=%u",
                 static_cast<int>(all), out->received.size(), out->sender_reliable.inflight,
                 out->sender_reliable.srtt_ms, out->sender_congestion.cwnd_packets);
    if (proxy) {
        proxy->Stop();
    }
    client.Close();
    server.Close();
    return all;
}

void TestLinkPumpWindowAndDelivery() {
    PairOptions options;
    options.algorithm = CongestionAlgorithm::kBbr;
    PairResult result;
    constexpr std::uint32_t kMessages = 300;
    const bool ok = PairResultRun(options, kMessages, 200, &result);
    BENTO_CHECK(ok);
    BENTO_CHECK_EQ(result.received.size(), kMessages);
    if (result.received.size() == kMessages) {
        for (std::uint32_t i = 0; i < kMessages; ++i) {
            BENTO_CHECK_EQ(result.received[i], i);
        }
    }
    BENTO_CHECK(result.sender_congestion.cwnd_packets > 0U);
    BENTO_CHECK(result.sender_congestion.cwnd_packets <= 256U);
    BENTO_CHECK(result.sender_congestion.pacing_rate_bps > 0U);
    BENTO_CHECK(result.sender_congestion.rto_ms > 0U);
    BENTO_CHECK(result.sender_reliable.inflight == 0U);
    BENTO_CHECK(result.sender_reliable.srtt_ms > 0U);
    std::printf("[INFO] pump: cwnd=%u packets_acked=%llu pacing=%lluB/s srtt=%u rto=%u\n",
                static_cast<unsigned>(result.sender_congestion.cwnd_packets),
                static_cast<unsigned long long>(result.sender_congestion.packets_acked),
                static_cast<unsigned long long>(result.sender_congestion.pacing_rate_bps),
                result.sender_reliable.srtt_ms, result.sender_congestion.rto_ms);
}

void TestLinkFastRetransmitUnderLoss() {
    PairOptions options;
    options.algorithm = CongestionAlgorithm::kBbr;
    options.loss_percent = 5;
    options.seed = 0x5A17C0DEU;
    PairResult result;
    constexpr std::uint32_t kMessages = 200;
    const bool ok = PairResultRun(options, kMessages, 300, &result);
    BENTO_CHECK(ok);
    BENTO_CHECK_EQ(result.received.size(), kMessages);
    // 5% 丢包下必然出现空洞：快速重传应触发，且 BBR 不因随机丢包减窗。
    BENTO_CHECK(result.sender_reliable.fast_retransmits >= 1U);
    BENTO_CHECK_EQ(result.sender_congestion.cwnd_reductions, 0U);
    std::printf("[INFO] loss5: fast=%llu pto=%llu retx=%llu cwnd=%u\n",
                static_cast<unsigned long long>(result.sender_reliable.fast_retransmits),
                static_cast<unsigned long long>(result.sender_reliable.pto_retransmits),
                static_cast<unsigned long long>(result.sender_reliable.retransmits),
                static_cast<unsigned>(result.sender_congestion.cwnd_packets));
}

void TestBbrVsNewRenoDeliveryUnderLoss() {
    PairOptions bbr_options;
    bbr_options.algorithm = CongestionAlgorithm::kBbr;
    bbr_options.loss_percent = 10;
    bbr_options.seed = 0x1234ABCDU;
    PairResult bbr_result;
    BENTO_CHECK(PairResultRun(bbr_options, 120, 300, &bbr_result));
    BENTO_CHECK_EQ(bbr_result.received.size(), 120U);
    BENTO_CHECK_EQ(bbr_result.sender_congestion.cwnd_reductions, 0U);

    PairOptions reno_options = bbr_options;
    reno_options.algorithm = CongestionAlgorithm::kNewReno;
    PairResult reno_result;
    BENTO_CHECK(PairResultRun(reno_options, 120, 300, &reno_result));
    BENTO_CHECK_EQ(reno_result.received.size(), 120U);
    BENTO_CHECK(reno_result.sender_congestion.cwnd_reductions >= 1U);
    BENTO_CHECK(reno_result.sender_congestion.ssthresh_packets <=
                bbr_result.sender_congestion.ssthresh_packets);
    std::printf("[INFO] bbr/reno: cwnd=%u/%u ssthresh=%u/%u reno_red=%llu bbr_pacing=%llu\n",
                static_cast<unsigned>(bbr_result.sender_congestion.cwnd_packets),
                static_cast<unsigned>(reno_result.sender_congestion.cwnd_packets),
                static_cast<unsigned>(bbr_result.sender_congestion.ssthresh_packets),
                static_cast<unsigned>(reno_result.sender_congestion.ssthresh_packets),
                static_cast<unsigned long long>(reno_result.sender_congestion.cwnd_reductions),
                static_cast<unsigned long long>(bbr_result.sender_congestion.pacing_rate_bps));
}

}  // namespace

int main() {
    TestRttEstimator();
    TestBandwidthMaxFilter();
    TestPacerTokenBucket();
    TestBbrStartupToProbeBw();
    TestBbrRandomLossKeepsWindow();
    TestBbrProbeRtt();
    TestBbrTimeoutDoesNotCollapse();
    TestNewRenoLossAndTimeout();
    TestNoneKeepsLegacyBehavior();
    TestLinkPumpWindowAndDelivery();
    TestLinkFastRetransmitUnderLoss();
    TestBbrVsNewRenoDeliveryUnderLoss();
    return bento_test::Finish("test_congestion");
}
