/**
 * @file test_bandwidth.cpp
 * @brief 交付速率/丢包率估计（偏差 <=30%）与 Link ABR 回调。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "go2cpp/bento/net/bandwidth.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

void TestEstimatorRates() {
    const std::uint64_t targets[] = {500000ULL, 2000000ULL, 8000000ULL};
    for (const std::uint64_t target : targets) {
        BandwidthEstimator estimator;
        std::uint64_t now = 1000;
        // 20ms 一个 ACK 样本，共 1s；样本字节数按目标速率换算。
        const std::uint64_t bytes_per_sample = target / 8ULL / 50ULL;
        for (int i = 0; i < 50; ++i) {
            estimator.OnAck(now, static_cast<std::size_t>(bytes_per_sample));
            now += 20;
        }
        const std::uint64_t measured = estimator.delivery_rate_bps(now);
        const double error = std::fabs(static_cast<double>(measured) -
                                       static_cast<double>(target)) /
                             static_cast<double>(target);
        BENTO_CHECK(error <= 0.30);
        const BandwidthStats snapshot = estimator.Snapshot(now);
        BENTO_CHECK_EQ(snapshot.acked_packets, 50U);
        BENTO_CHECK(snapshot.acked_bytes > 0);
        BENTO_CHECK_EQ(snapshot.ack_samples, 50U);
        std::printf("[INFO] bandwidth: target=%llubps measured=%llubps error=%.1f%%\n",
                    static_cast<unsigned long long>(target),
                    static_cast<unsigned long long>(measured), error * 100.0);
    }
}

void TestLossRate() {
    BandwidthEstimator estimator;
    std::uint64_t now = 1000;
    for (int i = 0; i < 80; ++i) {
        estimator.OnAck(now, 100);
        now += 10;
    }
    for (int i = 0; i < 20; ++i) {
        estimator.OnLoss(now, 1);
    }
    BENTO_CHECK(std::fabs(estimator.loss_rate() - 0.2) < 0.01);
    const BandwidthStats snapshot = estimator.Snapshot(now);
    BENTO_CHECK_EQ(snapshot.acked_packets, 80U);
    BENTO_CHECK_EQ(snapshot.lost_packets, 20U);
    BENTO_CHECK(std::fabs(snapshot.loss_rate - 0.2) < 0.01);
}

void TestLinkAbr() {
    LinkConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 8000;
    server_config.abr_interval_ms = 20;
    Link server(server_config);
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "[FAIL] server.Open: %s\n", error.c_str());
        BENTO_CHECK(false);
        return;
    }

    LinkConfig client_config;
    client_config.bind_address = "127.0.0.1";
    client_config.remote = "127.0.0.1";
    client_config.peer_port = server.local_port();
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 8000;
    client_config.abr_interval_ms = 20;
    client_config.random_seed = 0xAB12CD34U;
    Link client(client_config);
    if (!client.Open(&error)) {
        std::fprintf(stderr, "[FAIL] client.Open: %s\n", error.c_str());
        BENTO_CHECK(false);
        return;
    }

    bool client_connected = false;
    bool server_connected = false;
    client.on_connected = [&client_connected]() { client_connected = true; };
    server.on_connected = [&server_connected]() { server_connected = true; };
    std::vector<std::uint64_t> reports;
    client.on_bandwidth = [&reports](std::uint64_t bitrate_bps) {
        reports.push_back(bitrate_bps);
    };
    client.Connect();
    const bool connected = bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 5000);
    BENTO_CHECK(connected);

    constexpr std::uint32_t kCount = 300;
    const std::size_t payload_bytes = 1200;
    const std::vector<std::uint8_t> payload(payload_bytes, 0x3C);
    std::size_t next = 0;
    const std::uint64_t deadline = NowMs() + 20000;
    while (NowMs() < deadline && next < kCount) {
        while (next < kCount && client.SendStream(0, payload.data(), payload.size())) {
            ++next;
        }
        client.RunOnce(1);
        server.RunOnce(1);
    }
    BENTO_CHECK_EQ(next, kCount);

    const bool delivered = bento_test::PumpUntil(
        client, server,
        [&]() {
            return server.reliable_stats(0).messages_delivered == kCount &&
                   client.reliable_stats(0).inflight == 0;
        },
        20000);
    BENTO_CHECK(delivered);
    BENTO_CHECK(!reports.empty());
    const std::uint64_t estimate = client.BandwidthEstimate();
    BENTO_CHECK(estimate > 0);
    const double loss = client.LossRate();
    BENTO_CHECK(loss >= 0.0 && loss <= 1.0);
    for (const std::uint64_t report : reports) {
        BENTO_CHECK(report >= 100000ULL);
        BENTO_CHECK(report <= 1000000000ULL);
    }
    std::printf("[INFO] abr: callbacks=%llu estimate=%llubps loss=%.4f\n",
                static_cast<unsigned long long>(reports.size()),
                static_cast<unsigned long long>(estimate), loss);

    client.Close();
    server.Close();
}

}  // namespace

int main() {
    TestEstimatorRates();
    TestLossRate();
    TestLinkAbr();
    return bento_test::Finish("test_bandwidth");
}
