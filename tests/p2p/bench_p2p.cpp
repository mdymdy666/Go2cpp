/**
 * @file bench_p2p.cpp
 * @brief P2P 吞吐/延迟基准（不注册 ctest，手动运行）。
 * @details 测量三组数据：
 *          1. 局域网直连单向吞吐（1KB × 20000 包）；
 *          2. 直连请求-响应 RTT（1000 轮）；
 *          3. 简化 TURN 中继的单向吞吐（512B × 2000 包）。
 *
 *          输出同时写到 stdout 与 `/tmp/p2p_bench.txt`，便于归档。
 *
 *          运行：`.build-bento-wsl/p2p_bench`
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/p2p/candidate.hpp"
#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/p2p_channel.hpp"
#include "go2cpp/p2p/signaling.hpp"
#include "go2cpp/p2p/turn.hpp"
#include "go2cpp/p2p/transport.hpp"

namespace {

using namespace std::chrono_literals;
using go2cpp::p2p::Endpoint;
using go2cpp::p2p::LoopbackSignaling;
using go2cpp::p2p::P2PChannel;
using go2cpp::p2p::P2PConfig;
using go2cpp::p2p::P2PStats;
using go2cpp::p2p::RelayServer;
using go2cpp::p2p::RelayServerConfig;
using Clock = std::chrono::steady_clock;

struct Pair {
    std::unique_ptr<LoopbackSignaling> signaling_a;
    std::unique_ptr<LoopbackSignaling> signaling_b;
    std::unique_ptr<P2PChannel> a;
    std::unique_ptr<P2PChannel> b;
};

P2PConfig MakeConfig(LoopbackSignaling* signaling, bool relay_mode, Endpoint turn) {
    P2PConfig config;
    config.bind_endpoint = Endpoint::Loopback(0);
    config.signaling = signaling;
    config.gather_host_candidates = !relay_mode;
    config.enable_relay = relay_mode;
    config.turn_server = turn;
    config.connect_timeout = 5000ms;
    config.check_interval = 50ms;
    config.keepalive_interval = 0ms;
    config.peer_timeout = 0ms;
    return config;
}

bool ConnectPair(Pair* pair, bool relay_mode, Endpoint turn) {
    auto signals = LoopbackSignaling::CreatePair();
    pair->signaling_a = std::move(signals.first);
    pair->signaling_b = std::move(signals.second);
    pair->a = std::make_unique<P2PChannel>(MakeConfig(pair->signaling_a.get(), relay_mode, turn));
    pair->b = std::make_unique<P2PChannel>(MakeConfig(pair->signaling_b.get(), relay_mode, turn));
    pair->a->GatherCandidates();
    pair->b->GatherCandidates();
    std::atomic<bool> ok_a{false};
    std::atomic<bool> ok_b{false};
    std::thread thread_a([&]() { ok_a.store(pair->a->Connect(5000ms)); });
    std::thread thread_b([&]() { ok_b.store(pair->b->Connect(5000ms)); });
    thread_a.join();
    thread_b.join();
    return ok_a.load() && ok_b.load();
}

double Percentile(std::vector<double> samples, double percentile) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t index = static_cast<std::size_t>(
        std::min<double>(static_cast<double>(samples.size() - 1),
                         percentile * static_cast<double>(samples.size())));
    return samples[index];
}

struct Report {
    std::string text;

    void Line(const char* format, ...) {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        text += buffer;
        text += '\n';
        std::printf("%s", buffer);
        std::printf("\n");
    }
};

void BenchThroughput(Pair* pair, Report* report, int packets, std::size_t payload_size,
                     std::chrono::microseconds send_interval) {
    std::atomic<int> received{0};
    std::vector<double> latencies;
    std::thread receiver([&]() {
        std::vector<std::uint8_t> buffer(payload_size + 64);
        const Clock::time_point deadline = Clock::now() + 10s;
        while (received.load() < packets && Clock::now() < deadline) {
            const std::size_t n = pair->b->Recv(buffer.data(), buffer.size(), 50ms);
            if (n != payload_size) {
                continue;
            }
            std::uint64_t sent_micros = 0;
            std::memcpy(&sent_micros, buffer.data(), sizeof(sent_micros));
            const Clock::time_point sent_at(std::chrono::microseconds(
                static_cast<std::int64_t>(sent_micros)));
            latencies.push_back(
                std::chrono::duration<double, std::milli>(Clock::now() - sent_at).count());
            received.fetch_add(1);
        }
    });
    std::vector<std::uint8_t> payload(payload_size, 0x5A);
    const Clock::time_point started = Clock::now();
    for (int index = 0; index < packets; ++index) {
        const std::uint64_t now_micros = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
                .count());
        std::memcpy(payload.data(), &now_micros, sizeof(now_micros));
        (void)pair->a->Send(payload.data(), payload.size());
        if (send_interval.count() > 0) {
            std::this_thread::sleep_for(send_interval);
        }
    }
    receiver.join();
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const int arrived = received.load();
    const double mbps = static_cast<double>(arrived) * static_cast<double>(payload_size) /
                        seconds / 1e6;
    report->Line("[吞吐] %zuB × %d：到达 %d（%.1f%%），%.2f MB/s，P50=%.3fms P95=%.3fms P99=%.3fms",
                 payload_size, packets, arrived,
                 100.0 * static_cast<double>(arrived) / static_cast<double>(packets), mbps,
                 Percentile(latencies, 0.50), Percentile(latencies, 0.95),
                 Percentile(latencies, 0.99));
}

void BenchRtt(Pair* pair, Report* report, int rounds) {
    std::vector<double> latencies;
    latencies.reserve(rounds);
    std::vector<std::uint8_t> payload(64, 0x42);
    for (int index = 0; index < rounds; ++index) {
        const Clock::time_point begin = Clock::now();
        if (!pair->a->Send(payload.data(), payload.size())) {
            continue;
        }
        std::vector<std::uint8_t> buffer(256);
        const std::size_t received = pair->b->Recv(buffer.data(), buffer.size(), 1000ms);
        if (received != payload.size()) {
            continue;
        }
        if (!pair->b->Send(buffer.data(), received)) {
            continue;
        }
        const std::size_t reply = pair->a->Recv(buffer.data(), buffer.size(), 1000ms);
        if (reply == 0) {
            continue;
        }
        latencies.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
    }
    report->Line("[RTT] 请求-响应 %d 轮：完成 %zu，P50=%.3fms P95=%.3fms P99=%.3fms",
                 rounds, latencies.size(), Percentile(latencies, 0.50),
                 Percentile(latencies, 0.95), Percentile(latencies, 0.99));
}

}  // namespace

int main() {
    Report report;
    report.Line("=== P2P 基准（Loopback，Release）===");

    {
        Pair pair;
        if (!ConnectPair(&pair, false, Endpoint())) {
            std::fprintf(stderr, "直连建立失败\n");
            return 1;
        }
        const P2PStats stats = pair.a->Stats();
        report.Line("[直连] peer=%s relayed=%d checks_sent=%llu",
                    pair.a->PeerEndpoint().ToString().c_str(),
                    static_cast<int>(pair.a->IsRelayed()),
                    static_cast<unsigned long long>(stats.connectivity_checks_sent));
        BenchThroughput(&pair, &report, 20000, 1024, std::chrono::microseconds(0));
        BenchRtt(&pair, &report, 1000);
        pair.a.reset();
        pair.b.reset();
    }

    {
        RelayServerConfig config;
        config.bind_endpoint = Endpoint::Loopback(0);
        RelayServer relay(config);
        std::string error;
        if (!relay.Start(&error)) {
            std::fprintf(stderr, "中继启动失败: %s\n", error.c_str());
            return 1;
        }
        Pair pair;
        if (!ConnectPair(&pair, true, relay.listen_endpoint())) {
            std::fprintf(stderr, "中继连接失败\n");
            relay.Stop();
            return 1;
        }
        const auto stats = relay.stats();
        report.Line("[中继] allocations=%llu listen=%s",
                    static_cast<unsigned long long>(stats.allocations_created),
                    relay.listen_endpoint().ToString().c_str());
        // 中继数据面要经过两次用户态转发，单线程服务器的稳态处理能力
        // 远低于直连；这里按 10k pps（100µs 间隔）发送，测「可持续吞吐」
        // 而不是瞬时洪泛（后者只会打满受限的 UDP 接收缓冲而丢包）。
        BenchThroughput(&pair, &report, 5000, 512, std::chrono::microseconds(100));
        BenchRtt(&pair, &report, 200);
        pair.a.reset();
        pair.b.reset();
        relay.Stop();
    }

    std::ofstream output("/tmp/p2p_bench.txt", std::ios::trunc);
    if (output) {
        output << report.text;
    }
    report.Line("结果已写入 /tmp/p2p_bench.txt");
    return 0;
}
