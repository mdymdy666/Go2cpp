/**
 * @file bench_link_mq.cpp
 * @brief Link 多队列/批量收包压测：单 peer 吞吐、多实例多核扩展。
 * @details 不进 ctest（与 bench_batch3 一样是手工基准）：
 *
 *   g++ -O2 -std=c++17 -I include -DLINK_MULTIQUEUE \
 *       tests/bento/bench_link_mq.cpp -L.build -lgo2cpp_bento ...
 *
 *          场景 A `single`：1 对 Link（1 个 peer），K 个发送线程灌 N 个
 *          64B DATAGRAM，接收线程 RunOnce(0) 循环，测接收 pps。
 *          场景 B `multi`：P 对独立的 Link（各自端口），每对一个接收线程，
 *          测总 pps 随 P 的扩展 —— 单 peer 的四元组固定，一个 peer 的流量
 *          只会命中一个 REUSEPORT 队列，所以「多核扩展」在 Link 这一层只能
 *          由多实例/多 peer 容器（PeerHub）获得，本场景把这部分量化。
 *
 *          `-DLINK_MULTIQUEUE` 打开 rx_queues/rx_threads 字段（老版本没有）。
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/link.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double NowNs() {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

go2cpp::bento::LinkConfig MakeConfig() {
    go2cpp::bento::LinkConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.require_encryption = false;  // 压测数据面，不测加密
    config.heartbeat_interval_ms = 200;
    config.peer_timeout_ms = 10000;
    return config;
}

/// 建立一对已握手的 Link（server 监听，client 主动连）。
struct Pair {
    std::unique_ptr<go2cpp::bento::Link> server;
    std::unique_ptr<go2cpp::bento::Link> client;
    std::atomic<std::uint64_t> received{0};
    std::thread rx_thread;
    std::atomic<bool> stop{false};
};

bool ConnectPair(Pair* pair, go2cpp::bento::LinkConfig client_config) {
    std::string error;
    if (!pair->server->Open(&error)) {
        std::fprintf(stderr, "server open failed: %s\n", error.c_str());
        return false;
    }
    client_config.remote = "127.0.0.1";
    client_config.peer_port = pair->server->local_port();
    pair->client = std::make_unique<go2cpp::bento::Link>(std::move(client_config));
    if (!pair->client->Open(&error)) {
        std::fprintf(stderr, "client open failed: %s\n", error.c_str());
        return false;
    }
    if (!pair->client->Connect()) {
        std::fprintf(stderr, "connect failed\n");
        return false;
    }
    pair->server->on_datagram = [pair](std::uint8_t, const std::uint8_t*, std::size_t) {
        pair->received.fetch_add(1, std::memory_order_relaxed);
    };
    const double deadline = NowNs() + 10e9;
    while (NowNs() < deadline) {
        pair->server->RunOnce(1);
        pair->client->RunOnce(1);
        if (pair->server->connected() && pair->client->connected()) {
            return true;
        }
    }
    std::fprintf(stderr, "handshake timeout\n");
    return false;
}

struct Result {
    double tx_pps = 0;
    double rx_pps = 0;
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    double secs = 0;
};

/// 单对 Link：senders 个线程各发 total/senders 个包，接收线程死循环 RunOnce。
Result RunPair(const go2cpp::bento::LinkConfig& server_config,
               const go2cpp::bento::LinkConfig& client_config, std::uint64_t total,
               int senders, int timeout_ms) {
    Result result;
    Pair pair;
    pair.server = std::make_unique<go2cpp::bento::Link>(server_config);
    if (!ConnectPair(&pair, client_config)) {
        return result;
    }
    pair.received.store(0);
    pair.rx_thread = std::thread([&pair]() {
        while (!pair.stop.load(std::memory_order_relaxed)) {
            pair.server->RunOnce(0);
        }
    });

    std::uint8_t payload[64];
    std::memset(payload, 0x5A, sizeof(payload));
    std::atomic<std::uint64_t> sent{0};
    std::atomic<int> ready{0};
    const std::uint64_t per_sender = total / static_cast<std::uint64_t>(senders);
    std::vector<std::thread> pool;
    const double t0 = NowNs();
    for (int i = 0; i < senders; ++i) {
        pool.emplace_back([&]() {
            ready.fetch_add(1, std::memory_order_release);
            while (ready.load(std::memory_order_acquire) < senders) {
            }
            std::uint64_t ok = 0;
            for (std::uint64_t n = 0; n < per_sender; ++n) {
                if (pair.client->SendDatagram(1, payload, sizeof(payload))) {
                    ++ok;
                }
            }
            sent.fetch_add(ok, std::memory_order_relaxed);
        });
    }
    for (std::thread& worker : pool) {
        worker.join();
    }
    // 发送完成后再给接收端一点时间把内核队列抽干。
    const double deadline = NowNs() + static_cast<double>(timeout_ms) * 1e6;
    while (NowNs() < deadline && pair.received.load(std::memory_order_relaxed) < sent.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const double t1 = NowNs();
    pair.stop.store(true, std::memory_order_relaxed);
    pair.rx_thread.join();
    result.sent = sent.load();
    result.received = pair.received.load();
    result.secs = (t1 - t0) / 1e9;
    result.tx_pps = static_cast<double>(result.sent) / result.secs;
    result.rx_pps = static_cast<double>(result.received) / result.secs;
    pair.client->Close();
    pair.server->Close();
    return result;
}

/// P 对独立 Link 并行跑，测总 pps（多核扩展）。
Result RunMultiInstance(int pairs, std::uint64_t total_per_pair, int senders_per_pair,
                        int queues, int threads, int timeout_ms) {
    Result out;
    std::vector<std::unique_ptr<Pair>> links;
    for (int i = 0; i < pairs; ++i) {
        go2cpp::bento::LinkConfig server_config = MakeConfig();
        go2cpp::bento::LinkConfig client_config = MakeConfig();
#ifdef LINK_MULTIQUEUE
        server_config.rx_queues = static_cast<std::uint32_t>(queues);
        server_config.rx_threads = static_cast<std::uint32_t>(threads);
#endif
        auto pair = std::make_unique<Pair>();
        pair->server = std::make_unique<go2cpp::bento::Link>(server_config);
        if (!ConnectPair(pair.get(), client_config)) {
            return out;
        }
        links.push_back(std::move(pair));
    }
    for (auto& pair : links) {
        pair->stop.store(false, std::memory_order_relaxed);
        pair->rx_thread = std::thread([p = pair.get()]() {
            while (!p->stop.load(std::memory_order_relaxed)) {
                p->server->RunOnce(0);
            }
        });
    }
    std::uint8_t payload[64];
    std::memset(payload, 0x5A, sizeof(payload));
    std::atomic<std::uint64_t> sent{0};
    const double t0 = NowNs();
    std::vector<std::thread> pool;
    for (auto& pair : links) {
        for (int s = 0; s < senders_per_pair; ++s) {
            pool.emplace_back([&pair, &sent, &payload, total_per_pair, senders_per_pair]() {
                const std::uint64_t per =
                    total_per_pair / static_cast<std::uint64_t>(senders_per_pair);
                std::uint64_t ok = 0;
                for (std::uint64_t n = 0; n < per; ++n) {
                    if (pair->client->SendDatagram(1, payload, sizeof(payload))) {
                        ++ok;
                    }
                }
                sent.fetch_add(ok, std::memory_order_relaxed);
            });
        }
    }
    for (std::thread& worker : pool) {
        worker.join();
    }
    std::uint64_t received = 0;
    const double deadline = NowNs() + static_cast<double>(timeout_ms) * 1e6;
    while (NowNs() < deadline) {
        received = 0;
        for (auto& pair : links) {
            received += pair->received.load(std::memory_order_relaxed);
        }
        if (received >= sent.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const double t1 = NowNs();
    for (auto& pair : links) {
        pair->stop.store(true, std::memory_order_relaxed);
        pair->rx_thread.join();
    }
    out.sent = sent.load();
    out.received = received;
    out.secs = (t1 - t0) / 1e9;
    out.tx_pps = static_cast<double>(out.sent) / out.secs;
    out.rx_pps = static_cast<double>(out.received) / out.secs;
    for (auto& pair : links) {
        pair->client->Close();
        pair->server->Close();
    }
    return out;
}

void Report(const char* name, const Result& r) {
    std::printf(
        "BENCH %-34s sent=%-9llu rx=%-9llu %.3fs  tx_pps=%-10.0f rx_pps=%-10.0f "
        "rx_loss=%.2f%%\n",
        name, static_cast<unsigned long long>(r.sent),
        static_cast<unsigned long long>(r.received), r.secs, r.tx_pps, r.rx_pps,
        r.sent == 0 ? 0.0 : 100.0 * (1.0 - static_cast<double>(r.received) /
                                               static_cast<double>(r.sent)));
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "single";
    const std::uint64_t total =
        argc > 2 ? static_cast<std::uint64_t>(std::strtoull(argv[2], nullptr, 10)) : 10000000ULL;
    std::printf("=== bench_link_mq mode=%s total=%llu ===\n", mode.c_str(),
                static_cast<unsigned long long>(total));
    if (mode == "single") {
        // 不同发送线程数：1 线程时发送端可能是瓶颈，4 线程时接收端是瓶颈。
        for (int senders : {1, 4}) {
            go2cpp::bento::LinkConfig server_config = MakeConfig();
            go2cpp::bento::LinkConfig client_config = MakeConfig();
#ifdef LINK_MULTIQUEUE
            server_config.rx_queues = 1;
#endif
            char name[64];
            std::snprintf(name, sizeof(name), "single/queues=1/senders=%d", senders);
            Report(name, RunPair(server_config, client_config, total, senders, 5000));
        }
#ifdef LINK_MULTIQUEUE
        // 单 peer + 多队列：四元组固定，内核只会哈希到一个队列，用于验证
        // 「多队列不会拖慢也不会加速单 peer」这一架构事实。
        for (int queues : {4, 8}) {
            go2cpp::bento::LinkConfig server_config = MakeConfig();
            go2cpp::bento::LinkConfig client_config = MakeConfig();
            server_config.rx_queues = static_cast<std::uint32_t>(queues);
            server_config.rx_threads = static_cast<std::uint32_t>(queues);
            char name[64];
            std::snprintf(name, sizeof(name), "single/queues=%d/threads=%d/senders=4", queues,
                          queues);
            Report(name, RunPair(server_config, client_config, total, 4, 5000));
        }
#endif
    } else if (mode == "multi") {
        // 每对 2.5e6 包，P=1/2/4：总 pps 随实例数扩展（每实例一个接收线程）。
        for (int pairs : {1, 2, 4}) {
            char name[64];
            std::snprintf(name, sizeof(name), "multi/pairs=%d/queues=1", pairs);
            Report(name, RunMultiInstance(pairs, total / static_cast<std::uint64_t>(pairs), 1, 1,
                                          0, 5000));
        }
    } else if (mode == "threads") {
        // 收包线程模式（rx_threads>0）：单 peer，验证收包线程路径的吞吐。
#ifdef LINK_MULTIQUEUE
        for (int queues : {1, 4}) {
            go2cpp::bento::LinkConfig server_config = MakeConfig();
            go2cpp::bento::LinkConfig client_config = MakeConfig();
            server_config.rx_queues = static_cast<std::uint32_t>(queues);
            server_config.rx_threads = static_cast<std::uint32_t>(queues);
            char name[64];
            std::snprintf(name, sizeof(name), "threads/queues=%d/threads=%d", queues, queues);
            Report(name, RunPair(server_config, client_config, total, 4, 5000));
        }
#else
        std::printf("threads mode requires LINK_MULTIQUEUE\n");
#endif
    }
    return 0;
}
