/**
 * @file test_flow_control.cpp
 * @brief RFC 9000 MAX_DATA 流控：窗口耗尽 -> WouldBlock、MAX_DATA -> 恢复。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/bento/net/channel.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/relation.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

struct ChannelHarness {
    Relation relation;
    std::uint64_t now = 1000;
    std::vector<std::vector<std::uint8_t>> sent;
    std::unique_ptr<ReliableChannel> channel;

    explicit ChannelHarness(ReliableChannel::Config config) {
        channel = std::make_unique<ReliableChannel>(
            0, &relation, [](std::uint32_t, const std::uint8_t*, std::size_t) {}, config,
            [this]() { return now; });
        channel->SetFrameSink([this](const std::uint8_t* data, std::size_t bytes) {
            sent.emplace_back(data, data + bytes);
            return true;
        });
    }
};

void WriteU32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

void TestSendBlocksAndResumes() {
    ReliableChannel::Config config;
    config.initial_max_data = 1000;
    config.peer_initial_max_data = 1000;
    config.max_data_refresh_ms = 0;
    ChannelHarness harness(config);
    const std::vector<std::uint8_t> payload(400, 0x42);

    BENTO_CHECK(harness.channel->TrySend(payload.data(), payload.size()) == SendStatus::kOk);
    BENTO_CHECK(harness.channel->TrySend(payload.data(), payload.size()) == SendStatus::kOk);
    BENTO_CHECK(harness.channel->TrySend(payload.data(), payload.size()) ==
                SendStatus::kWouldBlock);
    BENTO_CHECK_EQ(harness.channel->stats().flow_control_blocked, 1U);
    BENTO_CHECK_EQ(harness.channel->flow_control().peer_max_data, 1000U);
    BENTO_CHECK_EQ(harness.channel->flow_control().tx_bytes_total, 800U);

    // MAX_DATA 扩窗后同一消息重试必须成功。
    harness.channel->OnMaxData(2000);
    BENTO_CHECK_EQ(harness.channel->flow_control().peer_max_data, 2000U);
    BENTO_CHECK_EQ(harness.channel->flow_control().max_data_received, 1U);
    BENTO_CHECK(harness.channel->TrySend(payload.data(), payload.size()) == SendStatus::kOk);
    BENTO_CHECK_EQ(harness.channel->flow_control().tx_bytes_total, 1200U);
}

void TestReceiveWindowAdvances() {
    ReliableChannel::Config config;
    config.initial_max_data = 1000;
    config.max_data_refresh_ms = 200;
    Relation relation;
    std::uint64_t now = 1000;
    ReliableChannel receiver(
        0, &relation, [](std::uint32_t, const std::uint8_t*, std::size_t) {}, config,
        [&now]() { return now; });

    std::uint64_t limit = 0;
    BENTO_CHECK(!receiver.PendingMaxData(&limit, now));

    const std::vector<std::uint8_t> payload(600, 0x5A);
    FrameHeader header;
    header.type = FrameType::kStream;
    header.channel = 0;
    header.seq = 1;
    receiver.OnFrame(header, payload.data(), payload.size(), nullptr);
    BENTO_CHECK_EQ(receiver.stats().messages_delivered, 1U);
    BENTO_CHECK_EQ(receiver.flow_control().rx_bytes_total, 600U);

    // 剩余 400B < 窗口一半（500B）：推进到 600 + 1000。
    BENTO_CHECK(receiver.PendingMaxData(&limit, now));
    BENTO_CHECK_EQ(limit, 1600U);
    receiver.OnMaxDataSent(limit, now);
    BENTO_CHECK(!receiver.PendingMaxData(&limit, now));
    BENTO_CHECK_EQ(receiver.flow_control().max_data_sent, 1U);

    // 通告可能丢失：refresh 周期后幂等重发同一上限。
    now += 200;
    BENTO_CHECK(receiver.PendingMaxData(&limit, now));
    BENTO_CHECK_EQ(limit, 1600U);
}

void TestLinkFlowControlEndToEnd() {
    LinkConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 8000;
    server_config.reliable.initial_max_data = 8192;
    server_config.reliable.max_data_refresh_ms = 50;
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
    client_config.reliable.initial_max_data = 8192;
    client_config.reliable.max_data_refresh_ms = 50;
    client_config.random_seed = 0x51C0DE01U;
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

    constexpr std::uint32_t kCount = 200;
    constexpr std::size_t kPayloadBytes = 256;
    std::vector<std::vector<std::uint8_t>> payloads(kCount);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        payloads[i].assign(kPayloadBytes, static_cast<std::uint8_t>(i & 0xFFU));
        WriteU32(payloads[i].data(), i);
        WriteU32(payloads[i].data() + 4, i ^ 0x5A5A5A5AU);
    }

    std::vector<std::uint32_t> received;
    server.on_stream = [&received](std::uint8_t channel, const std::uint8_t* data,
                                   std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 0U);
        if (bytes < 8) {
            BENTO_CHECK(false);
            return;
        }
        const std::uint32_t seq = (static_cast<std::uint32_t>(data[0]) << 24) |
                                  (static_cast<std::uint32_t>(data[1]) << 16) |
                                  (static_cast<std::uint32_t>(data[2]) << 8) |
                                  static_cast<std::uint32_t>(data[3]);
        received.push_back(seq);
    };

    std::size_t next = 0;
    bool saw_would_block = false;
    const std::uint64_t deadline = NowMs() + 20000;
    while (NowMs() < deadline && next < kCount) {
        while (next < kCount) {
            const SendStatus status =
                client.TrySendStream(0, payloads[next].data(), payloads[next].size());
            if (status == SendStatus::kOk) {
                ++next;
                continue;
            }
            if (status == SendStatus::kWouldBlock) {
                saw_would_block = true;
                break;
            }
            BENTO_CHECK(false);
            return;
        }
        client.RunOnce(1);
        server.RunOnce(1);
    }
    BENTO_CHECK(saw_would_block);
    BENTO_CHECK_EQ(next, kCount);

    const bool delivered = bento_test::PumpUntil(
        client, server, [&]() { return received.size() == kCount; }, 20000);
    BENTO_CHECK(delivered);
    BENTO_CHECK_EQ(received.size(), kCount);
    for (std::size_t i = 0; i < received.size(); ++i) {
        BENTO_CHECK_EQ(received[i], static_cast<std::uint32_t>(i));
    }

    // 两端都观察到了窗口通告/接收：证明流控真正闭环，而不是窗口恰好够用。
    BENTO_CHECK(client.reliable_stats(0).flow_control_blocked > 0);
    BENTO_CHECK(server.reliable_stats(0).max_data_sent > 0);
    BENTO_CHECK(client.reliable_stats(0).max_data_received > 0);
    BENTO_CHECK_EQ(server.reliable_stats(0).messages_delivered, kCount);
    std::printf(
        "[INFO] flow_control: sent=%llu delivered=%llu blocked=%llu max_data_tx=%llu "
        "max_data_rx=%llu\n",
        static_cast<unsigned long long>(client.reliable_stats(0).messages_sent),
        static_cast<unsigned long long>(server.reliable_stats(0).messages_delivered),
        static_cast<unsigned long long>(client.reliable_stats(0).flow_control_blocked),
        static_cast<unsigned long long>(server.reliable_stats(0).max_data_sent),
        static_cast<unsigned long long>(client.reliable_stats(0).max_data_received));

    client.Close();
    server.Close();
}

}  // namespace

int main() {
    TestSendBlocksAndResumes();
    TestReceiveWindowAdvances();
    TestLinkFlowControlEndToEnd();
    return bento_test::Finish("test_flow_control");
}
