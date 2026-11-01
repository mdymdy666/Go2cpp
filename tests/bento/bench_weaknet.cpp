/**
 * @file bench_weaknet.cpp
 * @brief UDP 弱网重做验收压测：
 *        1) 批量 socket 回环 1e7 帧吞吐（recvmmsg 32 / sendmmsg 16）；
 *        2) 真实 UDP + 弱网代理的 5 万帧矩阵（丢包/延迟/FEC/BBR 对照），
 *           统计交付率、重传、FEC 恢复与延迟分位。
 * @details 用法：bench_weaknet [--frames=10000000] [--weak-frames=10000]
 *          ctest 默认跑完整规模（1e7 + 5×1 万）。
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/udp_batch.hpp"
#include "test_util.hpp"
#include "udp_proxy.hpp"

namespace {

using go2cpp::bento::CongestionAlgorithm;
using go2cpp::bento::FecKind;
using go2cpp::bento::Link;
using go2cpp::bento::LinkConfig;
using go2cpp::bento::NowMs;
using go2cpp::bento::net::Endpoint;
using go2cpp::bento::net::UdpBatchSocket;

// ---------------------------------------------------------------------------
// 1) 批量回环吞吐
// ---------------------------------------------------------------------------

void RunBulkThroughput(std::uint64_t frames) {
    UdpBatchSocket socket;
    std::string error;
    UdpBatchSocket::Config config;
    config.queues = 1;
    config.recv_batch = UdpBatchSocket::kMaxRecvBatch;
    config.send_batch = UdpBatchSocket::kMaxSendBatch;
    if (!socket.Open(Endpoint{"127.0.0.1", 0}, config, &error)) {
        std::fprintf(stderr, "[FAIL] bulk open: %s\n", error.c_str());
        ++bento_test::g_failures;
        return;
    }
    const Endpoint peer{"127.0.0.1", socket.local_port()};
    std::vector<std::vector<std::uint8_t>> templates(
        UdpBatchSocket::kMaxSendBatch, std::vector<std::uint8_t>(64, 0x5AU));
    std::vector<UdpBatchSocket::Datagram> received;
    std::uint64_t sent = 0;
    std::uint64_t got = 0;
    const std::size_t window = 256;
    const std::uint64_t start = NowMs();
    while (sent < frames) {
        const std::size_t batch =
            static_cast<std::size_t>(std::min<std::uint64_t>(window, frames - sent));
        std::size_t offset = 0;
        while (offset < batch) {
            const std::size_t n = std::min<std::size_t>(UdpBatchSocket::kMaxSendBatch,
                                                        batch - offset);
            const std::vector<std::vector<std::uint8_t>> chunk(templates.begin(),
                                                               templates.begin() + n);
            std::size_t written = 0;
            socket.SendBatch(peer, chunk, &written, &error);
            if (written == 0) {
                break;
            }
            offset += written;
        }
        sent += offset;
        const std::size_t target = offset;
        int empty_rounds = 0;
        for (int attempt = 0; attempt < 400 && received.size() < target && empty_rounds < 3;
             ++attempt) {
            const std::size_t got_now = socket.RecvBatch(&received, 20, &error);
            empty_rounds = got_now == 0 ? empty_rounds + 1 : 0;
        }
        got += received.size();
        received.clear();
    }
    const std::uint64_t elapsed_ms = std::max<std::uint64_t>(NowMs() - start, 1);
    const double fps = static_cast<double>(got) * 1000.0 / static_cast<double>(elapsed_ms);
    std::printf(
        "[BULK] frames=%llu sent=%llu recv=%llu elapsed=%llums throughput=%.0ffps "
        "tx_batches=%llu rx_batches=%llu\n",
        static_cast<unsigned long long>(frames), static_cast<unsigned long long>(sent),
        static_cast<unsigned long long>(got), static_cast<unsigned long long>(elapsed_ms), fps,
        static_cast<unsigned long long>(socket.stats().tx_batches),
        static_cast<unsigned long long>(socket.stats().rx_batches));
    BENTO_CHECK(got >= frames / 1000U * 999U);  // 回环丢包率必须 < 0.1%
    BENTO_CHECK(fps >= 50000.0);                // 批量收发的基本吞吐门槛
    socket.Close();
}

// ---------------------------------------------------------------------------
// 2) 弱网矩阵（Link + UdpProxy）
// ---------------------------------------------------------------------------

struct Scenario {
    const char* name;
    int loss_percent;
    int delay_ms;
    bool fec;
};

struct ScenarioResult {
    bool connected = false;
    std::uint32_t frames = 0;
    std::size_t delivered = 0;
    std::uint64_t retransmits = 0;
    std::uint64_t fast_retransmits = 0;
    std::uint64_t pto_retransmits = 0;
    std::uint64_t fec_recovered = 0;
    std::uint64_t fec_groups = 0;
    std::uint64_t p50_ms = 0;
    std::uint64_t p99_ms = 0;
    std::uint32_t cwnd = 0;
    std::uint64_t pacing_bps = 0;
};

std::vector<std::uint8_t> MakePayload(std::uint32_t seq, std::size_t bytes) {
    std::vector<std::uint8_t> payload(bytes, 0);
    payload[0] = static_cast<std::uint8_t>(seq >> 24);
    payload[1] = static_cast<std::uint8_t>(seq >> 16);
    payload[2] = static_cast<std::uint8_t>(seq >> 8);
    payload[3] = static_cast<std::uint8_t>(seq);
    for (std::size_t i = 4; i < bytes; ++i) {
        payload[i] = static_cast<std::uint8_t>((seq * 131U + i * 17U) & 0xFFU);
    }
    return payload;
}

bool RunScenario(const Scenario& scenario, std::uint32_t frames, ScenarioResult* result) {
    LinkConfig server_config;
    server_config.require_encryption = false;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 10000;
    server_config.reliable.congestion.algorithm = CongestionAlgorithm::kBbr;
    server_config.reliable.fec.enabled = scenario.fec;
    if (scenario.fec) {
        server_config.reliable.fec.kind = FecKind::kXor;
        server_config.reliable.fec.data_shards = 8;
        server_config.reliable.fec.parity_shards = 1;
        server_config.reliable.fec.max_payload_bytes = 900;
    }
    Link server(server_config);
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "[FAIL] %s server.Open: %s\n", scenario.name, error.c_str());
        return false;
    }

    LinkConfig client_config;
    client_config.require_encryption = false;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.peer_address = "127.0.0.1";
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 10000;
    client_config.random_seed = 0xBEEF0001U;
    client_config.reliable.congestion.algorithm = CongestionAlgorithm::kBbr;
    client_config.reliable.fec = server_config.reliable.fec;

    std::unique_ptr<bento_test::UdpProxy> proxy;
    if (scenario.loss_percent > 0 || scenario.delay_ms > 0) {
        sockaddr_in address{};
        if (!go2cpp::bento::UdpSocket::ResolveIPv4("127.0.0.1", server.local_port(), &address,
                                                   &error)) {
            return false;
        }
        proxy = std::make_unique<bento_test::UdpProxy>(address, scenario.loss_percent,
                                                       scenario.delay_ms, 0xBEEF0001U);
        if (!proxy->Start(&error)) {
            return false;
        }
        client_config.peer_port = proxy->port();
    } else {
        client_config.peer_port = server.local_port();
    }

    Link client(client_config);
    if (!client.Open(&error)) {
        return false;
    }
    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };
    std::vector<std::uint64_t> send_ms(frames, 0);
    std::vector<std::uint64_t> latency_ms;
    latency_ms.reserve(frames);
    server.on_stream = [&](std::uint8_t channel, const std::uint8_t* data, std::size_t bytes) {
        if (channel != 0 || bytes < 4) {
            return;
        }
        const std::uint32_t seq = (static_cast<std::uint32_t>(data[0]) << 24) |
                                  (static_cast<std::uint32_t>(data[1]) << 16) |
                                  (static_cast<std::uint32_t>(data[2]) << 8) |
                                  static_cast<std::uint32_t>(data[3]);
        if (seq < frames) {
            ++result->delivered;
            latency_ms.push_back(std::max<std::uint64_t>(NowMs() - send_ms[seq], 1));
        }
    };
    client.Connect();
    result->connected = bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 15000);
    if (!result->connected) {
        return false;
    }
    result->frames = frames;
    const std::uint64_t start = NowMs();
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::vector<std::uint8_t> payload = MakePayload(i, 300);
        send_ms[i] = NowMs();
        bool accepted = client.SendStream(0, payload.data(), payload.size());
        for (int retry = 0; !accepted && retry < 200; ++retry) {
            bento_test::PumpUntil(client, server, []() { return false; }, 2);
            accepted = client.SendStream(0, payload.data(), payload.size());
        }
        if (!accepted) {
            std::fprintf(stderr, "[FAIL] %s SendStream at %u\n", scenario.name, i);
            return false;
        }
    }
    const bool all = bento_test::PumpUntil(
        client, server,
        [&]() {
            return result->delivered >= frames && client.reliable_stats(0).inflight == 0 &&
                   server.reliable_stats(0).inflight == 0;
        },
        60000);
    const auto sender = client.reliable_stats(0);
    const auto receiver = server.reliable_stats(0);
    result->retransmits = sender.retransmits;
    result->fast_retransmits = sender.fast_retransmits;
    result->pto_retransmits = sender.pto_retransmits;
    result->fec_recovered = receiver.fec_frames_recovered;
    result->fec_groups = sender.fec_groups_encoded;
    result->cwnd = client.congestion_stats(0).cwnd_packets;
    result->pacing_bps = client.congestion_stats(0).pacing_rate_bps;
    std::sort(latency_ms.begin(), latency_ms.end());
    result->p50_ms = bento_test::Percentile(latency_ms, 0.50);
    result->p99_ms = bento_test::Percentile(latency_ms, 0.99);
    if (proxy) {
        proxy->Stop();
    }
    client.Close();
    server.Close();
    const std::uint64_t elapsed = std::max<std::uint64_t>(NowMs() - start, 1);
    std::printf(
        "[WEAK] %-14s loss=%2d%% delay=%3dms fec=%d delivered=%zu/%u retx=%llu "
        "(fast=%llu pto=%llu) fec_rec=%llu groups=%llu cwnd=%u pacing=%lluB/s "
        "p50=%llums p99=%llums elapsed=%llums\n",
        scenario.name, scenario.loss_percent, scenario.delay_ms, static_cast<int>(scenario.fec),
        result->delivered, frames, static_cast<unsigned long long>(result->retransmits),
        static_cast<unsigned long long>(result->fast_retransmits),
        static_cast<unsigned long long>(result->pto_retransmits),
        static_cast<unsigned long long>(result->fec_recovered),
        static_cast<unsigned long long>(result->fec_groups), result->cwnd,
        static_cast<unsigned long long>(result->pacing_bps),
        static_cast<unsigned long long>(result->p50_ms),
        static_cast<unsigned long long>(result->p99_ms),
        static_cast<unsigned long long>(elapsed));
    return all;
}

void RunWeakMatrix(std::uint32_t frames_per_scenario) {
    const Scenario scenarios[] = {
        {"clean", 0, 0, false},        {"loss1", 1, 0, false},
        {"loss5", 5, 0, false},        {"loss10", 10, 0, false},
        {"loss5-delay20", 5, 20, false}, {"loss5-fec", 5, 0, true},
    };
    std::uint32_t total_frames = 0;
    std::uint32_t total_delivered = 0;
    for (const Scenario& scenario : scenarios) {
        ScenarioResult result;
        const bool ok = RunScenario(scenario, frames_per_scenario, &result);
        total_frames += frames_per_scenario;
        total_delivered += static_cast<std::uint32_t>(result.delivered);
        BENTO_CHECK(ok);
        BENTO_CHECK_EQ(result.delivered, frames_per_scenario);
        BENTO_CHECK(result.cwnd > 0U);
        BENTO_CHECK(result.pacing_bps > 0U);
        if (scenario.loss_percent > 0) {
            BENTO_CHECK(result.retransmits >= 1U);
        }
        if (scenario.fec) {
            BENTO_CHECK(result.fec_groups >= 1U);
            BENTO_CHECK(result.fec_recovered >= 1U);
        }
    }
    std::printf("[WEAK-TOTAL] scenarios=%zu frames=%u delivered=%u\n",
                sizeof(scenarios) / sizeof(scenarios[0]), total_frames, total_delivered);
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t bulk_frames = 10000000;   // 1e7
    std::uint32_t weak_frames = 10000;      // 5 个以上场景 × 1 万 = 5 万+
    bool skip_bulk = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--frames=", 0) == 0) {
            bulk_frames = std::strtoull(argument.c_str() + 9, nullptr, 10);
        } else if (argument.rfind("--weak-frames=", 0) == 0) {
            weak_frames = static_cast<std::uint32_t>(
                std::strtoul(argument.c_str() + 14, nullptr, 10));
        } else if (argument == "--skip-bulk") {
            skip_bulk = true;
        }
    }
    if (!skip_bulk) {
        RunBulkThroughput(bulk_frames);
    }
    RunWeakMatrix(weak_frames);
    return bento_test::Finish("bench_weaknet");
}
