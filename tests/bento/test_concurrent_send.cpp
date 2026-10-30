/**
 * @file test_concurrent_send.cpp
 * @brief 多线程 API：2 线程各并发 Send 100 条 -> 全部送达、0 重复。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

void WriteU32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t ReadU32(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

void TestConcurrentSend() {
    LinkConfig server_config;
    server_config.require_encryption = false;
    server_config.bind_address = "127.0.0.1";
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 8000;
    Link server(server_config);
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "[FAIL] server.Open: %s\n", error.c_str());
        BENTO_CHECK(false);
        return;
    }

    LinkConfig client_config;
    client_config.require_encryption = false;
    client_config.bind_address = "127.0.0.1";
    client_config.remote = "127.0.0.1";
    client_config.peer_port = server.local_port();
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 8000;
    client_config.random_seed = 0xC0C0A11U;
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
    client.Connect();
    const bool connected = bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 5000);
    BENTO_CHECK(connected);
    if (!connected) {
        return;
    }

    constexpr std::uint32_t kWorkers = 2;
    constexpr std::uint32_t kPerWorker = 100;
    constexpr std::uint32_t kTotal = kWorkers * kPerWorker;
    std::atomic<std::uint32_t> sent_ok{0};
    std::atomic<std::uint32_t> send_failed{0};
    std::atomic<bool> stop{false};

    std::set<std::uint64_t> received_keys;
    server.on_stream = [&received_keys](std::uint8_t channel, const std::uint8_t* data,
                                        std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 0U);
        if (bytes < 8) {
            BENTO_CHECK(false);
            return;
        }
        const std::uint64_t key = (static_cast<std::uint64_t>(ReadU32(data)) << 32) |
                                  static_cast<std::uint64_t>(ReadU32(data + 4));
        const bool inserted = received_keys.insert(key).second;
        BENTO_CHECK(inserted);  // 任何重复都立即失败
    };

    const auto worker = [&client, &sent_ok, &send_failed, &stop](std::uint32_t worker_id) {
        for (std::uint32_t i = 0; i < kPerWorker; ++i) {
            std::uint8_t payload[8];
            WriteU32(payload, worker_id);
            WriteU32(payload + 4, i);
            const std::uint64_t deadline = NowMs() + 15000;
            bool sent = false;
            while (!stop.load(std::memory_order_relaxed) && NowMs() < deadline) {
                if (client.ConcurrentSend(0, payload, sizeof(payload))) {
                    sent = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (sent) {
                sent_ok.fetch_add(1, std::memory_order_relaxed);
            } else {
                send_failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread first(worker, 1U);
    std::thread second(worker, 2U);

    // 接收线程（主线程）与两个发送线程并发：RunOnce 内部锁必须保证安全。
    const std::uint64_t deadline = NowMs() + 20000;
    while (NowMs() < deadline && received_keys.size() < kTotal) {
        client.RunOnce(1);
        server.RunOnce(1);
    }
    stop.store(true, std::memory_order_relaxed);
    first.join();
    second.join();

    const bool acked = bento_test::PumpUntil(
        client, server, [&]() { return client.reliable_stats(0).inflight == 0; }, 10000);
    BENTO_CHECK(acked);
    BENTO_CHECK_EQ(sent_ok.load(), kTotal);
    BENTO_CHECK_EQ(send_failed.load(), 0U);
    BENTO_CHECK_EQ(received_keys.size(), kTotal);
    BENTO_CHECK_EQ(server.reliable_stats(0).messages_delivered, kTotal);
    BENTO_CHECK_EQ(server.reliable_stats(0).duplicates_dropped, 0U);
    BENTO_CHECK_EQ(client.reliable_stats(0).messages_sent, kTotal);
    for (std::uint32_t worker_id = 1; worker_id <= kWorkers; ++worker_id) {
        for (std::uint32_t i = 0; i < kPerWorker; ++i) {
            const std::uint64_t key = (static_cast<std::uint64_t>(worker_id) << 32) |
                                      static_cast<std::uint64_t>(i);
            BENTO_CHECK(received_keys.count(key) == 1);
        }
    }
    std::printf("[INFO] concurrent: ok=%u failed=%u received=%llu delivered=%llu\n",
                sent_ok.load(), send_failed.load(),
                static_cast<unsigned long long>(received_keys.size()),
                static_cast<unsigned long long>(server.reliable_stats(0).messages_delivered));

    client.Close();
    server.Close();
}

}  // namespace

int main() {
    TestConcurrentSend();
    return bento_test::Finish("test_concurrent_send");
}
