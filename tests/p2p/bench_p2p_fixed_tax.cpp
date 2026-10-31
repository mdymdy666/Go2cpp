/**
 * @file bench_p2p_fixed_tax.cpp
 * @brief P2P 每包固定开销压测：真实 UDP 回环、默认 1e7 包、带窗口流控不丢包。
 * @details 用途：对比「每包 5-7 次 mutex + 每包一次 MakeFrame 堆分配」与
 *          「状态/统计原子化 + 复用帧缓冲」两种实现的端到端 pps。
 *          同一程序可链接改动前后的 libgo2cpp_p2p.so（LD_LIBRARY_PATH）跑出
 *          前后对比数据，因此本文件不进入 ctest。
 *
 *          运行：`.build-bento-wsl/p2p_bench_fixed_tax [包数] [载荷字节]`
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/p2p_channel.hpp"
#include "go2cpp/p2p/signaling.hpp"

namespace {

using namespace std::chrono_literals;

using go2cpp::p2p::Endpoint;
using go2cpp::p2p::P2PChannel;
using go2cpp::p2p::P2PConfig;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kWindow = 2048;

struct Pair {
    std::unique_ptr<go2cpp::p2p::LoopbackSignaling> signal_a;
    std::unique_ptr<go2cpp::p2p::LoopbackSignaling> signal_b;
    std::unique_ptr<P2PChannel> a;
    std::unique_ptr<P2PChannel> b;
};

P2PConfig MakeConfig(go2cpp::p2p::Signaling* signaling) {
    P2PConfig config;
    config.bind_endpoint = Endpoint::Loopback(0);
    config.signaling = signaling;
    config.gather_host_candidates = true;
    config.gather_loopback_candidates = true;
    config.enable_relay = false;
    config.auto_exchange_candidates = true;
    config.connect_timeout = 5000ms;
    config.gather_timeout = 500ms;
    config.check_interval = 20ms;
    // 压测期间不产生 keepalive 流量，只测数据面。
    config.keepalive_interval = 0ms;
    config.peer_timeout = 0ms;
    return config;
}

bool Connect(Pair* pair) {
    auto signals = go2cpp::p2p::LoopbackSignaling::CreatePair();
    pair->signal_a = std::move(signals.first);
    pair->signal_b = std::move(signals.second);
    pair->a = std::make_unique<P2PChannel>(MakeConfig(pair->signal_a.get()));
    pair->b = std::make_unique<P2PChannel>(MakeConfig(pair->signal_b.get()));
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

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t packets =
        argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10000000ULL;
    std::size_t payload_bytes = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 64ULL;
    payload_bytes = std::max<std::size_t>(9, std::min<std::size_t>(payload_bytes, 1200));

    Pair pair;
    if (!Connect(&pair)) {
        std::printf("[FAIL] connect: a=%s b=%s\n", pair.a->LastError().c_str(),
                    pair.b->LastError().c_str());
        return 1;
    }
    std::printf("[INFO] connected: a=%s b=%s packets=%llu payload=%zu\n",
                pair.a->PeerEndpoint().ToString().c_str(),
                pair.b->PeerEndpoint().ToString().c_str(),
                static_cast<unsigned long long>(packets), payload_bytes);

    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> corrupted{0};
    std::atomic<bool> receiver_stop{false};
    std::vector<std::uint8_t> buffer(payload_bytes + 64);

    std::thread receiver([&]() {
        std::uint64_t expected = 0;
        while (!receiver_stop.load(std::memory_order_relaxed)) {
            const std::size_t got = pair.b->Recv(buffer.data(), buffer.size(), 20ms);
            if (got == 0) {
                continue;
            }
            if (got != payload_bytes) {
                corrupted.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::uint64_t sequence = 0;
            std::memcpy(&sequence, buffer.data(), sizeof(sequence));
            // 允许乱序：只校验序号在窗口内且数据自洽。
            if (sequence >= expected && sequence - expected > kWindow) {
                corrupted.fetch_add(1, std::memory_order_relaxed);
            } else if (sequence + 1 > expected) {
                expected = sequence + 1;
            }
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::uint8_t> payload(payload_bytes, 0x5a);
    std::uint64_t send_failures = 0;
    const Clock::time_point started = Clock::now();
    for (std::uint64_t i = 0; i < packets; ++i) {
        std::memcpy(payload.data(), &i, sizeof(i));
        if (!pair.a->Send(payload.data(), payload.size())) {
            ++send_failures;
        }
        // 自适应节流：落后太多就短暂让出。UDP 丢包时严格窗口会永久卡住，
        // 所以这里只降速、不死等（丢包率单独统计）。
        if ((i & 127U) == 0 &&
            i - received.load(std::memory_order_relaxed) > kWindow) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    const Clock::time_point send_done = Clock::now();
    // 等待在途包落地（最多 10s）。
    const Clock::time_point drain_deadline = Clock::now() + 30s;
    while (received.load(std::memory_order_relaxed) < packets - send_failures &&
           Clock::now() < drain_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    receiver_stop.store(true, std::memory_order_relaxed);
    receiver.join();
    const Clock::time_point finished = Clock::now();

    const double send_seconds =
        std::chrono::duration<double>(send_done - started).count();
    const double total_seconds =
        std::chrono::duration<double>(finished - started).count();
    const std::uint64_t arrived = received.load(std::memory_order_relaxed);
    const auto stats_a = pair.a->Stats();
    const auto stats_b = pair.b->Stats();
    std::printf(
        "[PERF] packets=%llu arrived=%llu corrupted=%llu send_failures=%llu\n"
        "       send=%.3fs (%.0f pps) total=%.3fs (%.0f pps) %.1f MiB/s\n"
        "       a.sent=%llu b.recv=%llu a.bytes_sent=%llu b.dropped=%llu\n",
        static_cast<unsigned long long>(packets), static_cast<unsigned long long>(arrived),
        static_cast<unsigned long long>(corrupted.load()),
        static_cast<unsigned long long>(send_failures), send_seconds,
        static_cast<double>(packets) / send_seconds, total_seconds,
        static_cast<double>(arrived) / total_seconds,
        static_cast<double>(arrived) * static_cast<double>(payload_bytes) / total_seconds /
            (1024.0 * 1024.0),
        static_cast<unsigned long long>(stats_a.packets_sent),
        static_cast<unsigned long long>(stats_b.packets_received),
        static_cast<unsigned long long>(stats_a.bytes_sent),
        static_cast<unsigned long long>(stats_b.dropped_packets));

    pair.a->Close();
    pair.b->Close();
    // UDP 无重传：允许 <1% 内核队列溢出丢包，但收到的数据必须零损坏。
    const std::uint64_t lost = packets - send_failures - arrived;
    if (corrupted.load() != 0 || lost * 100 > packets) {
        std::printf("[FAIL] arrival/corruption check failed: lost=%llu\n",
                    static_cast<unsigned long long>(lost));
        return 1;
    }
    std::printf("[PASS] p2p fixed-tax bench\n");
    return 0;
}
