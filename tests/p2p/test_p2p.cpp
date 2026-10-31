/**
 * @file test_p2p.cpp
 * @brief P2P 通道测试：候选排序、局域网直连、NAT 打洞、中继降级、超时、弱网。
 * @details 场景覆盖：
 *          1. 候选优先级（host > srflx > relay）与候选文本序列化；
 *          2. 局域网直连（host 候选，无 STUN/TURN）；
 *          3. 全锥形 NAT 打洞成功（双向数据、IsRelayed=false）；
 *          4. 端口受限 NAT 打洞成功（双方同时发包建立过滤表）；
 *          5. 对称 NAT 打洞失败 → 自动降级中继（IsRelayed=true）；
 *          6. 纯中继（只有 relay 候选）；
 *          7. 连接超时（黑洞候选）；
 *          8. keepalive / RTT / 序列号缺口统计；
 *          9. 多候选（黑洞候选 + 真实候选，并行尝试、第一个成功）；
 *          10. 500 条随机长度数据完整性；
 *          11. 2000 包吞吐/延迟压测（吞吐 + P50/P95/P99 + 到达率）。
 *
 *          运行：`.build-bento-wsl/p2p_test_p2p`
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/p2p/candidate.hpp"
#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/p2p_channel.hpp"
#include "go2cpp/p2p/random.hpp"
#include "go2cpp/p2p/signaling.hpp"
#include "go2cpp/p2p/stun_server.hpp"
#include "go2cpp/p2p/turn.hpp"
#include "mock_nat.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;

using go2cpp::p2p::Candidate;
using go2cpp::p2p::CandidateType;
using go2cpp::p2p::Endpoint;
using go2cpp::p2p::LoopbackSignaling;
using go2cpp::p2p::P2PChannel;
using go2cpp::p2p::P2PConfig;
using go2cpp::p2p::P2PState;
using go2cpp::p2p::P2PStateName;
using go2cpp::p2p::RelayServer;
using go2cpp::p2p::RelayServerConfig;
using go2cpp::p2p::StunServer;
using go2cpp::p2p::StunServerConfig;
using go2cpp::p2p::test::MockNat;
using go2cpp::p2p::test::NatBehavior;
using Clock = std::chrono::steady_clock;

struct SignalingPair {
    std::unique_ptr<LoopbackSignaling> a;
    std::unique_ptr<LoopbackSignaling> b;

    static SignalingPair Create() {
        auto pair = LoopbackSignaling::CreatePair();
        SignalingPair result;
        result.a = std::move(pair.first);
        result.b = std::move(pair.second);
        return result;
    }
};

struct ChannelPair {
    SignalingPair signaling;
    std::unique_ptr<P2PChannel> a;
    std::unique_ptr<P2PChannel> b;

    void Close() {
        a.reset();
        b.reset();
    }
};

struct PairOptions {
    std::shared_ptr<go2cpp::p2p::DatagramTransport> transport_a;
    std::shared_ptr<go2cpp::p2p::DatagramTransport> transport_b;
    bool gather_host = true;
    Endpoint stun;
    Endpoint turn;
    std::chrono::milliseconds timeout{3000};
    std::chrono::milliseconds keepalive{15000};
    std::chrono::milliseconds peer_timeout{30000};
};

P2PConfig MakeConfig(SignalingPair* signaling, bool side_a,
                     const std::shared_ptr<go2cpp::p2p::DatagramTransport>& transport,
                     const PairOptions& options) {
    P2PConfig config;
    config.bind_endpoint = Endpoint::Loopback(0);
    config.signaling = side_a ? signaling->a.get() : signaling->b.get();
    config.transport = transport;
    config.gather_host_candidates = options.gather_host;
    config.gather_loopback_candidates = true;
    config.enable_relay = options.turn.valid();
    config.turn_server = options.turn;
    if (options.stun.valid()) {
        config.stun_servers.push_back(options.stun);
    }
    config.connect_timeout = options.timeout;
    config.gather_timeout = 500ms;
    config.check_interval = 50ms;
    config.keepalive_interval = options.keepalive;
    config.peer_timeout = options.peer_timeout;
    config.auto_exchange_candidates = true;
    return config;
}

bool ConnectPair(ChannelPair* pair, const PairOptions& options, std::chrono::milliseconds timeout) {
    pair->signaling = SignalingPair::Create();
    pair->a = std::make_unique<P2PChannel>(
        MakeConfig(&pair->signaling, true, options.transport_a, options));
    pair->b = std::make_unique<P2PChannel>(
        MakeConfig(&pair->signaling, false, options.transport_b, options));
    pair->a->GatherCandidates();
    pair->b->GatherCandidates();

    std::atomic<bool> ok_a{false};
    std::atomic<bool> ok_b{false};
    std::thread thread_a([&]() { ok_a.store(pair->a->Connect(timeout)); });
    std::thread thread_b([&]() { ok_b.store(pair->b->Connect(timeout)); });
    thread_a.join();
    thread_b.join();
    return ok_a.load() && ok_b.load();
}

bool SendRecv(P2PChannel* from, P2PChannel* to, const std::string& message,
              std::chrono::milliseconds timeout) {
    if (!from->Send(message.data(), message.size())) {
        std::fprintf(stderr, "[INFO] Send 失败: %s\n", from->LastError().c_str());
        return false;
    }
    std::string buffer(message.size() + 1, '\0');
    const std::size_t received = to->Recv(buffer.data(), buffer.size(), timeout);
    if (received != message.size()) {
        std::fprintf(stderr, "[INFO] Recv 长度 %zu != %zu（%s）\n", received, message.size(),
                     to->LastError().c_str());
        return false;
    }
    return std::memcmp(buffer.data(), message.data(), message.size()) == 0;
}

void TestCandidateOrdering() {
    Candidate host;
    host.type = CandidateType::kHost;
    host.endpoint = *Endpoint::Parse("192.168.1.5:5000");
    host.base = host.endpoint;
    go2cpp::p2p::FinalizeCandidate(&host);

    Candidate srflx;
    srflx.type = CandidateType::kSrflx;
    srflx.endpoint = *Endpoint::Parse("203.0.113.7:62000");
    srflx.base = host.endpoint;
    go2cpp::p2p::FinalizeCandidate(&srflx);

    Candidate relay;
    relay.type = CandidateType::kRelay;
    relay.endpoint = *Endpoint::Parse("198.51.100.9:49152");
    relay.base = *Endpoint::Parse("198.51.100.9:3478");
    go2cpp::p2p::FinalizeCandidate(&relay);

    std::vector<Candidate> candidates = {relay, host, srflx};
    go2cpp::p2p::SortCandidatesByPriority(&candidates);
    P2P_CHECK(candidates.size() == 3);
    P2P_CHECK(candidates[0].type == CandidateType::kHost);
    P2P_CHECK(candidates[1].type == CandidateType::kSrflx);
    P2P_CHECK(candidates[2].type == CandidateType::kRelay);
    P2P_CHECK(host.priority > srflx.priority);
    P2P_CHECK(srflx.priority > relay.priority);

    const std::string text = go2cpp::p2p::SerializeCandidates(candidates);
    std::string error;
    const std::vector<Candidate> parsed = go2cpp::p2p::ParseCandidates(text, &error);
    P2P_CHECK_MSG(parsed.size() == 3, error.c_str());
    if (parsed.size() == 3) {
        P2P_CHECK(parsed[0] == candidates[0]);
        P2P_CHECK(parsed[1] == candidates[1]);
        P2P_CHECK(parsed[2] == candidates[2]);
    }

    // 非法输入必须整体失败，而不是返回「半套候选」。
    P2P_CHECK(go2cpp::p2p::ParseCandidates("g2p2-candidates/1\n", &error).empty());
    P2P_CHECK(go2cpp::p2p::ParseCandidates("g2p2-candidates/9\ncand host 1 1.2.3.4:5 1.2.3.4:5 a\n",
                                           &error)
                  .empty());
    P2P_CHECK(go2cpp::p2p::ParseCandidates("cand bogus 1 1.2.3.4:5 1.2.3.4:5 a\n", &error).empty());
    P2P_CHECK(go2cpp::p2p::ParseCandidates("cand host 1 not-an-ip\n", &error).empty());
}

void TestLanDirect() {
    ChannelPair pair;
    PairOptions options;
    options.gather_host = true;
    P2P_CHECK(ConnectPair(&pair, options, 3000ms));
    P2P_CHECK(pair.a->IsConnected());
    P2P_CHECK(pair.b->IsConnected());
    P2P_CHECK(!pair.a->IsRelayed());
    P2P_CHECK(!pair.b->IsRelayed());
    P2P_CHECK(pair.a->PeerEndpoint() == pair.b->LocalEndpoint());
    P2P_CHECK(pair.b->PeerEndpoint() == pair.a->LocalEndpoint());
    // 双方同时发包时，后发的一方可能先收到对方的检查包而直接成功，
    // 因此「发送数」和「接收数」只要有一个非零就说明检查确实发生过。
    P2P_CHECK(pair.a->Stats().connectivity_checks_sent +
                  pair.a->Stats().connectivity_checks_received >
              0);
    P2P_CHECK(SendRecv(pair.a.get(), pair.b.get(), "lan-direct-a2b", 1000ms));
    P2P_CHECK(SendRecv(pair.b.get(), pair.a.get(), "lan-direct-b2a", 1000ms));
    P2P_CHECK(pair.a->Stats().packets_sent >= 1);
    P2P_CHECK(pair.b->Stats().packets_received >= 1);
    pair.Close();
}

void TestHolePunching(NatBehavior behavior) {
    MockNat nat_a(behavior);
    MockNat nat_b(behavior);
    std::string error;
    P2P_CHECK(nat_a.Start(&error));
    P2P_CHECK(nat_b.Start(&error));
    auto transport_a = nat_a.CreateClientTransport(&error);
    auto transport_b = nat_b.CreateClientTransport(&error);
    P2P_CHECK(transport_a != nullptr);
    P2P_CHECK(transport_b != nullptr);
    if (!transport_a || !transport_b) {
        return;
    }
    StunServerConfig server_config;
    server_config.primary = Endpoint::Loopback(0);
    StunServer server(server_config);
    P2P_CHECK(server.Start(&error));

    ChannelPair pair;
    PairOptions options;
    options.transport_a = transport_a;
    options.transport_b = transport_b;
    options.gather_host = false;
    options.stun = server.primary_endpoint();
    options.timeout = 5000ms;
    const bool connected = ConnectPair(&pair, options, 5000ms);
    P2P_CHECK_MSG(connected, ("NAT " + std::string(behavior == NatBehavior::kFullCone ? "FullCone"
                                                                                        : "PortRestricted") +
                              " 打洞失败: A=" + pair.a->LastError() + " B=" + pair.b->LastError())
                                 .c_str());
    if (!connected) {
        pair.Close();
        server.Stop();
        nat_a.Stop();
        nat_b.Stop();
        return;
    }
    P2P_CHECK(!pair.a->IsRelayed());
    P2P_CHECK(!pair.b->IsRelayed());
    P2P_CHECK(SendRecv(pair.a.get(), pair.b.get(), "punch-a2b", 1000ms));
    P2P_CHECK(SendRecv(pair.b.get(), pair.a.get(), "punch-b2a", 1000ms));
    P2P_CHECK(pair.a->Stats().connectivity_checks_received > 0);
    std::printf("[INFO] NAT %-14s 打洞成功：A peer=%s B peer=%s\n",
                behavior == NatBehavior::kFullCone ? "FullCone" : "PortRestricted",
                pair.a->PeerEndpoint().ToString().c_str(),
                pair.b->PeerEndpoint().ToString().c_str());
    pair.Close();
    server.Stop();
    nat_a.Stop();
    nat_b.Stop();
}

void TestSymmetricFallbackToRelay() {
    MockNat nat_a(NatBehavior::kSymmetric);
    MockNat nat_b(NatBehavior::kSymmetric);
    std::string error;
    P2P_CHECK(nat_a.Start(&error));
    P2P_CHECK(nat_b.Start(&error));
    auto transport_a = nat_a.CreateClientTransport(&error);
    auto transport_b = nat_b.CreateClientTransport(&error);
    P2P_CHECK(transport_a != nullptr && transport_b != nullptr);
    if (!transport_a || !transport_b) {
        return;
    }
    StunServerConfig stun_config;
    stun_config.primary = Endpoint::Loopback(0);
    StunServer stun(stun_config);
    P2P_CHECK(stun.Start(&error));

    RelayServerConfig relay_config;
    relay_config.bind_endpoint = Endpoint::Loopback(0);
    RelayServer relay(relay_config);
    P2P_CHECK_MSG(relay.Start(&error), error.c_str());

    ChannelPair pair;
    PairOptions options;
    options.transport_a = transport_a;
    options.transport_b = transport_b;
    options.gather_host = false;
    options.stun = stun.primary_endpoint();
    options.turn = relay.listen_endpoint();
    options.timeout = 3000ms;
    const bool connected = ConnectPair(&pair, options, 3000ms);
    P2P_CHECK_MSG(connected,
                  ("对称 NAT 应降级中继但连接失败: A=" + pair.a->LastError() +
                   " B=" + pair.b->LastError())
                      .c_str());
    if (!connected) {
        pair.Close();
        relay.Stop();
        stun.Stop();
        nat_a.Stop();
        nat_b.Stop();
        return;
    }
    P2P_CHECK(pair.a->IsRelayed());
    P2P_CHECK(pair.b->IsRelayed());
    P2P_CHECK(SendRecv(pair.a.get(), pair.b.get(), "relay-a2b", 1500ms));
    P2P_CHECK(SendRecv(pair.b.get(), pair.a.get(), "relay-b2a", 1500ms));
    const auto relay_stats = relay.stats();
    P2P_CHECK(relay_stats.allocations_created >= 2);
    P2P_CHECK(relay_stats.packets_client_to_peer > 0);
    P2P_CHECK(relay_stats.packets_peer_to_client > 0);
    std::printf("[INFO] 对称 NAT 降级中继成功：allocations=%llu 转发 %llu/%llu 包\n",
                static_cast<unsigned long long>(relay_stats.allocations_created),
                static_cast<unsigned long long>(relay_stats.packets_client_to_peer),
                static_cast<unsigned long long>(relay_stats.packets_peer_to_client));
    pair.Close();
    relay.Stop();
    stun.Stop();
    nat_a.Stop();
    nat_b.Stop();
}

void TestRelayOnly() {
    std::string error;
    RelayServerConfig relay_config;
    relay_config.bind_endpoint = Endpoint::Loopback(0);
    RelayServer relay(relay_config);
    P2P_CHECK_MSG(relay.Start(&error), error.c_str());
    if (!relay.started()) {
        return;
    }
    ChannelPair pair;
    PairOptions options;
    options.gather_host = false;
    options.turn = relay.listen_endpoint();
    options.timeout = 2000ms;
    const bool connected = ConnectPair(&pair, options, 2000ms);
    P2P_CHECK(connected);
    if (connected) {
        P2P_CHECK(pair.a->IsRelayed());
        P2P_CHECK(pair.b->IsRelayed());
        P2P_CHECK(SendRecv(pair.a.get(), pair.b.get(), "relay-only-a2b", 1500ms));
        P2P_CHECK(SendRecv(pair.b.get(), pair.a.get(), "relay-only-b2a", 1500ms));
        P2P_CHECK(pair.a->Stats().relayed_packets_sent >= 1);
        P2P_CHECK(pair.b->Stats().relayed_packets_received >= 1);
    }
    pair.Close();
    relay.Stop();
}

void TestConnectTimeout() {
    P2PConfig config;
    config.bind_endpoint = Endpoint::Loopback(0);
    config.gather_host_candidates = false;
    config.enable_relay = false;
    config.connect_timeout = 300ms;
    config.check_interval = 50ms;
    P2PChannel channel(config);
    Candidate blackhole;
    blackhole.type = CandidateType::kHost;
    blackhole.endpoint = *Endpoint::Parse("127.0.0.1:9");
    blackhole.base = blackhole.endpoint;
    go2cpp::p2p::FinalizeCandidate(&blackhole);
    channel.AddRemoteCandidate(blackhole);
    const Clock::time_point started = Clock::now();
    const bool connected = channel.Connect(300ms);
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    P2P_CHECK(!connected);
    P2P_CHECK(channel.state() == P2PState::kFailed);
    P2P_CHECK(!channel.LastError().empty());
    P2P_CHECK(elapsed_ms >= 250.0);
    P2P_CHECK(elapsed_ms < 2000.0);
    std::printf("[INFO] 超时场景：%.1fms 后失败，错误=%s\n", elapsed_ms,
                channel.LastError().c_str());
}

void TestKeepaliveAndRtt() {
    ChannelPair pair;
    PairOptions options;
    options.gather_host = true;
    options.keepalive = 100ms;
    options.peer_timeout = 5000ms;
    P2P_CHECK(ConnectPair(&pair, options, 3000ms));
    if (!pair.a->IsConnected()) {
        return;
    }
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> a_received{0};
    std::atomic<std::size_t> b_received{0};
    std::thread receiver_a([&]() {
        char buffer[256];
        const Clock::time_point deadline = Clock::now() + 1200ms;
        while (Clock::now() < deadline) {
            const std::size_t n = pair.a->Recv(buffer, sizeof(buffer), 50ms);
            if (n > 0) {
                a_received.fetch_add(1);
            }
        }
    });
    std::thread receiver_b([&]() {
        char buffer[256];
        const Clock::time_point deadline = Clock::now() + 1200ms;
        while (Clock::now() < deadline) {
            const std::size_t n = pair.b->Recv(buffer, sizeof(buffer), 50ms);
            if (n > 0) {
                b_received.fetch_add(1);
            }
        }
    });
    receiver_a.join();
    receiver_b.join();
    stop.store(true);

    const auto stats_a = pair.a->Stats();
    const auto stats_b = pair.b->Stats();
    P2P_CHECK(stats_a.keepalives_sent > 0);
    P2P_CHECK(stats_b.keepalives_sent > 0);
    P2P_CHECK(stats_a.keepalives_received > 0);
    P2P_CHECK(stats_b.keepalives_received > 0);
    P2P_CHECK(stats_a.rtt_ms > 0.0);
    P2P_CHECK(stats_b.rtt_ms > 0.0);
    P2P_CHECK(stats_a.rtt_avg_ms > 0.0);
    std::printf("[INFO] keepalive: A sent=%llu recv=%llu rtt=%.3fms avg=%.3fms；"
                "B sent=%llu recv=%llu rtt=%.3fms\n",
                static_cast<unsigned long long>(stats_a.keepalives_sent),
                static_cast<unsigned long long>(stats_a.keepalives_received), stats_a.rtt_ms,
                stats_a.rtt_avg_ms, static_cast<unsigned long long>(stats_b.keepalives_sent),
                static_cast<unsigned long long>(stats_b.keepalives_received), stats_b.rtt_ms);
    pair.Close();
}

void TestMultipleCandidates() {
    ChannelPair pair;
    PairOptions options;
    options.gather_host = true;
    pair.signaling = SignalingPair::Create();
    pair.a = std::make_unique<P2PChannel>(
        MakeConfig(&pair.signaling, true, nullptr, options));
    pair.b = std::make_unique<P2PChannel>(
        MakeConfig(&pair.signaling, false, nullptr, options));
    pair.a->GatherCandidates();
    pair.b->GatherCandidates();

    // 给 A 额外塞一个不可达的 host 候选，验证并行尝试后仍能选到真实候选。
    Candidate blackhole;
    blackhole.type = CandidateType::kHost;
    blackhole.endpoint = *Endpoint::Parse("127.0.0.1:9");
    blackhole.base = blackhole.endpoint;
    go2cpp::p2p::FinalizeCandidate(&blackhole);
    pair.a->AddRemoteCandidate(blackhole);

    std::atomic<bool> ok_a{false};
    std::atomic<bool> ok_b{false};
    std::thread thread_a([&]() { ok_a.store(pair.a->Connect(3000ms)); });
    std::thread thread_b([&]() { ok_b.store(pair.b->Connect(3000ms)); });
    thread_a.join();
    thread_b.join();
    P2P_CHECK(ok_a.load());
    P2P_CHECK(ok_b.load());
    P2P_CHECK(pair.a->RemoteCandidates().size() >= 2);
    if (ok_a.load() && ok_b.load()) {
        P2P_CHECK(SendRecv(pair.a.get(), pair.b.get(), "multi-candidate", 1000ms));
        P2P_CHECK(pair.a->PeerEndpoint() == pair.b->LocalEndpoint());
    }
    pair.Close();
}

void TestRandomPayloads() {
    ChannelPair pair;
    PairOptions options;
    P2P_CHECK(ConnectPair(&pair, options, 3000ms));
    if (!pair.a->IsConnected()) {
        return;
    }
    int failures = 0;
    for (int round = 0; round < 500; ++round) {
        const std::size_t length = 1 + static_cast<std::size_t>(
                                           go2cpp::p2p::RandomU32() %
                                           static_cast<std::uint32_t>(go2cpp::p2p::kP2PMaxPayload));
        std::vector<std::uint8_t> payload(length);
        go2cpp::p2p::SecureRandomBytes(payload.data(), payload.size());
        if (!pair.a->Send(payload.data(), payload.size())) {
            ++failures;
            continue;
        }
        std::vector<std::uint8_t> received(length + 16, 0);
        const std::size_t got = pair.b->Recv(received.data(), received.size(), 1000ms);
        if (got != length ||
            std::memcmp(received.data(), payload.data(), length) != 0) {
            ++failures;
        }
    }
    P2P_CHECK(failures == 0);
    if (failures != 0) {
        std::printf("[INFO] 随机数据失败 %d/500\n", failures);
    }
    pair.Close();
}

void TestThroughput() {
    ChannelPair pair;
    PairOptions options;
    P2P_CHECK(ConnectPair(&pair, options, 3000ms));
    if (!pair.a->IsConnected()) {
        return;
    }
    constexpr int kPackets = 2000;
    constexpr std::size_t kPayload = 512;
    std::vector<double> latencies;
    latencies.reserve(kPackets);
    std::atomic<int> received{0};
    std::atomic<bool> receiver_done{false};
    std::thread receiver([&]() {
        std::vector<std::uint8_t> buffer(kPayload + 32);
        const Clock::time_point deadline = Clock::now() + 5s;
        while (received.load() < kPackets && Clock::now() < deadline) {
            const std::size_t n = pair.b->Recv(buffer.data(), buffer.size(), 100ms);
            if (n != kPayload) {
                continue;
            }
            std::uint64_t sent_micros = 0;
            std::memcpy(&sent_micros, buffer.data(), sizeof(sent_micros));
            const auto sent_at = Clock::time_point(std::chrono::microseconds(
                static_cast<std::int64_t>(sent_micros)));
            const double latency =
                std::chrono::duration<double, std::milli>(Clock::now() - sent_at).count();
            latencies.push_back(latency);
            received.fetch_add(1);
        }
        receiver_done.store(true);
    });

    std::vector<std::uint8_t> payload(kPayload, 0);
    const Clock::time_point started = Clock::now();
    int send_failures = 0;
    for (int index = 0; index < kPackets; ++index) {
        const std::uint64_t now_micros = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now().time_since_epoch())
                .count());
        std::memcpy(payload.data(), &now_micros, sizeof(now_micros));
        std::memcpy(payload.data() + sizeof(now_micros), &index, sizeof(index));
        if (!pair.a->Send(payload.data(), payload.size())) {
            ++send_failures;
        }
    }
    receiver.join();
    const double elapsed_seconds =
        std::chrono::duration<double>(Clock::now() - started).count();
    const int arrived = received.load();
    const double throughput_mbps =
        static_cast<double>(arrived) * static_cast<double>(kPayload) / elapsed_seconds / 1e6;
    const double p50 = p2p_test::Percentile(latencies, 0.50);
    const double p95 = p2p_test::Percentile(latencies, 0.95);
    const double p99 = p2p_test::Percentile(latencies, 0.99);
    std::printf("[PERF] P2P 直连: %d/%d 到达（发送失败 %d），%.2f MB/s，"
                "P50=%.3fms P95=%.3fms P99=%.3fms\n",
                arrived, kPackets, send_failures, throughput_mbps, p50, p95, p99);
    P2P_CHECK(send_failures == 0);
    P2P_CHECK(arrived >= static_cast<int>(kPackets * 0.98));
    const auto stats = pair.a->Stats();
    P2P_CHECK(stats.packets_sent >= static_cast<std::uint64_t>(kPackets));
    P2P_CHECK(stats.bytes_sent >= static_cast<std::uint64_t>(kPackets) * kPayload);
    pair.Close();
}

}  // namespace

int main() {
    std::printf("=== P2P 通道测试 ===\n");
    TestCandidateOrdering();
    TestLanDirect();
    TestHolePunching(NatBehavior::kFullCone);
    TestHolePunching(NatBehavior::kPortRestricted);
    TestSymmetricFallbackToRelay();
    TestRelayOnly();
    TestConnectTimeout();
    TestKeepaliveAndRtt();
    TestMultipleCandidates();
    TestRandomPayloads();
    TestThroughput();
    return p2p_test::Summary("test_p2p");
}
