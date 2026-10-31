/**
 * @file bench_p2p_memory.cpp
 * @brief P2P 每包用户态开销微基准：内存 transport 回环，排除内核 syscall。
 * @details 真实 UDP 压测里每包 ~6.5us 的系统调用会把「5-7 次 mutex +
 *          每包一次堆分配」的用户态开销完全淹没。本程序用无内核的
 *          内存队列替代 socket，只测 P2PChannel 自身的固定税：
 *          Send 组帧 + 统计，Recv 解析 + 序号/统计更新。
 *          同一程序可链接改动前后的 libgo2cpp_p2p.so 做前后对比。
 *
 *          运行：`.build-bento-wsl/p2p_bench_memory [包数] [载荷字节]`
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/p2p/endpoint.hpp"
#include "go2cpp/p2p/p2p_channel.hpp"
#include "go2cpp/p2p/signaling.hpp"
#include "go2cpp/p2p/transport.hpp"

namespace {

using namespace std::chrono_literals;

using go2cpp::p2p::DatagramTransport;
using go2cpp::p2p::Endpoint;
using go2cpp::p2p::P2PChannel;
using go2cpp::p2p::P2PConfig;
using Clock = std::chrono::steady_clock;

/// 单生产者/单消费者的内存数据报队列（连接阶段跨线程，数据阶段同线程）。
class MemoryQueue {
   public:
    void Push(const void* data, std::size_t bytes) {
        const auto* begin = static_cast<const std::uint8_t*>(data);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_packets.emplace_back(begin, begin + bytes);
        m_cv.notify_one();
    }

    bool Pop(std::vector<std::uint8_t>* out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, timeout, [this] { return !m_packets.empty(); })) {
            return false;
        }
        *out = std::move(m_packets.front());
        m_packets.pop_front();
        return true;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_packets.clear();
    }

   private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::vector<std::uint8_t>> m_packets;
};

class MemoryTransport final : public DatagramTransport {
   public:
    MemoryTransport(Endpoint local, Endpoint peer, std::shared_ptr<MemoryQueue> inbox,
                    std::shared_ptr<MemoryQueue> outbox)
        : m_local(local), m_peer(peer), m_inbox(std::move(inbox)), m_outbox(std::move(outbox)) {}

    bool SendTo(const Endpoint& /*destination*/, const void* data, std::size_t length) override {
        m_outbox->Push(data, length);
        return true;
    }

    int RecvFrom(Endpoint* source, void* buffer, std::size_t capacity,
                 std::chrono::milliseconds timeout) override {
        if (!m_inbox->Pop(&m_scratch, timeout)) {
            return 0;
        }
        const std::size_t bytes = std::min(m_scratch.size(), capacity);
        std::memcpy(buffer, m_scratch.data(), bytes);
        if (source != nullptr) {
            *source = m_peer;
        }
        return static_cast<int>(bytes);
    }

    Endpoint LocalEndpoint() const override { return m_local; }
    bool SetReceiveBufferSize(std::size_t /*bytes*/) override { return false; }

   private:
    Endpoint m_local;
    Endpoint m_peer;
    std::shared_ptr<MemoryQueue> m_inbox;
    std::shared_ptr<MemoryQueue> m_outbox;
    std::vector<std::uint8_t> m_scratch;
};

P2PConfig MakeConfig(go2cpp::p2p::Signaling* signaling,
                     const std::shared_ptr<DatagramTransport>& transport) {
    P2PConfig config;
    config.transport = transport;
    config.signaling = signaling;
    config.gather_host_candidates = true;
    config.gather_loopback_candidates = true;
    config.enable_relay = false;
    config.auto_exchange_candidates = true;
    config.connect_timeout = 3000ms;
    config.gather_timeout = 200ms;
    config.check_interval = 5ms;
    config.keepalive_interval = 0ms;
    config.peer_timeout = 0ms;
    return config;
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t packets =
        argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10000000ULL;
    std::size_t payload_bytes = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 64ULL;
    payload_bytes = std::max<std::size_t>(9, std::min<std::size_t>(payload_bytes, 1200));

    auto a_to_b = std::make_shared<MemoryQueue>();
    auto b_to_a = std::make_shared<MemoryQueue>();
    const Endpoint endpoint_a = Endpoint::Loopback(47101);
    const Endpoint endpoint_b = Endpoint::Loopback(47102);
    auto transport_a = std::make_shared<MemoryTransport>(endpoint_a, endpoint_b, b_to_a, a_to_b);
    auto transport_b = std::make_shared<MemoryTransport>(endpoint_b, endpoint_a, a_to_b, b_to_a);

    auto signals = go2cpp::p2p::LoopbackSignaling::CreatePair();
    P2PChannel a(MakeConfig(signals.first.get(), transport_a));
    P2PChannel b(MakeConfig(signals.second.get(), transport_b));
    a.GatherCandidates();
    b.GatherCandidates();
    std::atomic<bool> ok_a{false};
    std::atomic<bool> ok_b{false};
    std::thread thread_a([&]() { ok_a.store(a.Connect(3000ms)); });
    std::thread thread_b([&]() { ok_b.store(b.Connect(3000ms)); });
    thread_a.join();
    thread_b.join();
    if (!ok_a.load() || !ok_b.load()) {
        std::printf("[FAIL] connect: a=%s b=%s\n", a.LastError().c_str(), b.LastError().c_str());
        return 1;
    }
    a_to_b->Clear();
    b_to_a->Clear();

    std::vector<std::uint8_t> payload(payload_bytes, 0x5a);
    std::vector<std::uint8_t> received(payload_bytes + 64);
    std::uint64_t failures = 0;
    std::uint64_t mismatched = 0;
    const Clock::time_point started = Clock::now();
    for (std::uint64_t i = 0; i < packets; ++i) {
        std::memcpy(payload.data(), &i, sizeof(i));
        if (!a.Send(payload.data(), payload.size())) {
            ++failures;
        }
        std::size_t got = 0;
        int retries = 0;
        do {
            got = b.Recv(received.data(), received.size(), 100ms);
        } while (got == 0 && ++retries < 4);
        if (got == 0) {
            ++failures;
        } else if (got != payload_bytes) {
            ++mismatched;
        }
    }
    const Clock::time_point finished = Clock::now();
    const double seconds = std::chrono::duration<double>(finished - started).count();
    const auto stats_a = a.Stats();
    const auto stats_b = b.Stats();
    std::printf("[PERF] packets=%llu payload=%zu elapsed=%.3fs ns/packet=%.1f "
                "pps=%.0f failures=%llu mismatched=%llu\n",
                static_cast<unsigned long long>(packets), payload_bytes, seconds,
                seconds * 1e9 / static_cast<double>(packets),
                static_cast<double>(packets) / seconds,
                static_cast<unsigned long long>(failures),
                static_cast<unsigned long long>(mismatched));
    std::printf("       a.sent=%llu a.bytes=%llu b.recv=%llu b.bytes=%llu\n",
                static_cast<unsigned long long>(stats_a.packets_sent),
                static_cast<unsigned long long>(stats_a.bytes_sent),
                static_cast<unsigned long long>(stats_b.packets_received),
                static_cast<unsigned long long>(stats_b.bytes_received));
    a.Close();
    b.Close();
    if (failures != 0 || mismatched != 0 || stats_a.packets_sent != packets ||
        stats_b.packets_received != packets) {
        std::printf("[FAIL] memory fixed-tax bench\n");
        return 1;
    }
    std::printf("[PASS] p2p memory fixed-tax bench\n");
    return 0;
}
