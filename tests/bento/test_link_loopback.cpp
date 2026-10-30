/**
 * @file test_link_loopback.cpp
 * @brief 本地 UDP 回环：HELLO 握手 + 双向 100 条 STREAM（0 丢、按序）+ 不可靠混跑。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "test_util.hpp"
#include "udp_proxy.hpp"

namespace {

using go2cpp::bento::Link;
using go2cpp::bento::LinkConfig;
using go2cpp::bento::LinkState;
using go2cpp::bento::NowMs;

constexpr std::uint32_t kMessageCount = 100;
constexpr std::uint32_t kDatagramCount = 10;
constexpr std::size_t kPayloadBytes = 64;

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

std::uint32_t CheckValue(std::uint32_t seq) {
    return seq * 2654435761U ^ 0xA5A5A5A5U;
}

std::vector<std::uint8_t> MakePayload(std::uint32_t seq, std::uint8_t tag) {
    std::vector<std::uint8_t> payload(kPayloadBytes, 0);
    WriteU32(payload.data(), seq);
    WriteU32(payload.data() + 4, CheckValue(seq));
    for (std::size_t i = 8; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((seq * 31U + i * 7U + tag) & 0xFFU);
    }
    return payload;
}

bool ValidatePayload(const std::uint8_t* data, std::size_t bytes, std::uint32_t* seq,
                     std::uint8_t tag) {
    if (bytes != kPayloadBytes) {
        return false;
    }
    *seq = ReadU32(data);
    if (ReadU32(data + 4) != CheckValue(*seq)) {
        return false;
    }
    for (std::size_t i = 8; i < bytes; ++i) {
        const std::uint8_t expected =
            static_cast<std::uint8_t>((*seq * 31U + i * 7U + tag) & 0xFFU);
        if (data[i] != expected) {
            return false;
        }
    }
    return true;
}


// ---------------------------------------------------------------------------
// SACK 多块扩展：HELLO 能力协商与旧端降级（真实 UDP 环回 + 15% 丢包代理）
// ---------------------------------------------------------------------------

/// 一对 Link 在弱网代理下的端到端用例。
/// @param client_sack / server_sack 两端是否开启 SACK 扩展；任一端关闭即
///        模拟旧端（12B HELLO、8B ACK），双方都必须降级且数据完整。
void RunSackNegotiationCase(bool client_sack, bool server_sack) {
    constexpr std::uint32_t kMessages = 100;

    LinkConfig server_config;
    server_config.require_encryption = false;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 50;
    server_config.peer_timeout_ms = 8000;
    server_config.enable_sack_ext = server_sack;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    // 15% 丢包保证接收端出现乱序，从而真实走到「扩展块」编码路径。
    sockaddr_in server_address{};
    const bool resolved = go2cpp::bento::UdpSocket::ResolveIPv4(
        "127.0.0.1", server.local_port(), &server_address, &error);
    BENTO_CHECK(resolved);
    bento_test::UdpProxy proxy(server_address, 15, 0, 0x5AC4U);
    BENTO_CHECK(proxy.Start(&error));

    LinkConfig client_config;
    client_config.require_encryption = false;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.peer_address = "127.0.0.1";
    client_config.peer_port = proxy.port();
    client_config.heartbeat_interval_ms = 50;
    client_config.peer_timeout_ms = 8000;
    client_config.handshake_timeout_ms = 15000;
    client_config.enable_sack_ext = client_sack;
    client_config.random_seed = 0xC0FFEE02U;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));

    bool server_connected = false;
    bool client_connected = false;
    std::vector<std::uint32_t> received;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };
    server.on_stream = [&](std::uint8_t channel, const std::uint8_t* data, std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 0U);
        std::uint32_t seq = 0;
        BENTO_CHECK(ValidatePayload(data, bytes, &seq, 0x44));
        received.push_back(seq);
    };

    client.Connect();
    const bool connected = bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 15000);
    BENTO_CHECK(connected);

    // 能力协商：只有双方都置位才启用；任一端关闭 → 双方一致降级。
    const bool expected = client_sack && server_sack;
    BENTO_CHECK_EQ(client.sack_ext_enabled(), expected);
    BENTO_CHECK_EQ(server.sack_ext_enabled(), expected);

    for (std::uint32_t i = 0; i < kMessages; ++i) {
        const std::vector<std::uint8_t> payload = MakePayload(i, 0x44);
        BENTO_CHECK(client.SendStream(0, payload.data(), payload.size()));
    }
    const bool all_received = bento_test::PumpUntil(
        client, server,
        [&]() {
            return received.size() == kMessages && client.reliable_stats(0).inflight == 0 &&
                   server.reliable_stats(0).inflight == 0;
        },
        20000);
    BENTO_CHECK(all_received);
    BENTO_CHECK_EQ(received.size(), kMessages);
    for (std::uint32_t i = 0; i < received.size(); ++i) {
        BENTO_CHECK_EQ(received[i], i);
    }

    if (expected) {
        // 15% 丢包下接收端必然出现乱序，扩展块应真的写进纯 ACK 帧。
        BENTO_CHECK(server.stats().sack_acks_sent > 0U);
        BENTO_CHECK(server.stats().sack_blocks_sent > 0U);
        // 接收端发出的是扩展 ACK；发送端（client）应确实采纳过扩展块。
        BENTO_CHECK(client.reliable_stats(0).sack_acks_with_blocks > 0U);
        BENTO_CHECK(client.reliable_stats(0).sack_seqs_covered > 0U);
    } else {
        // 旧端线格式：只发 8B 位图 ACK，一个扩展字节都不能出现。
        BENTO_CHECK_EQ(client.stats().sack_acks_sent, 0U);
        BENTO_CHECK_EQ(server.stats().sack_acks_sent, 0U);
        BENTO_CHECK_EQ(server.reliable_stats(0).sack_acks_with_blocks, 0U);
        BENTO_CHECK_EQ(client.reliable_stats(0).sack_seqs_covered, 0U);
    }

    proxy.Stop();
    client.Close();
    server.Close();
}

}  // namespace

int main() {
    LinkConfig server_config;
    server_config.require_encryption = false;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 5000;

    Link server(server_config);
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "[FAIL] server.Open: %s\n", error.c_str());
        return 1;
    }

    LinkConfig client_config;
    client_config.require_encryption = false;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.peer_address = "127.0.0.1";
    client_config.peer_port = server.local_port();
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 5000;
    client_config.random_seed = 0xC0FFEE01U;

    Link client(client_config);
    if (!client.Open(&error)) {
        std::fprintf(stderr, "[FAIL] client.Open: %s\n", error.c_str());
        return 1;
    }

    std::vector<std::uint32_t> server_received;
    std::vector<std::uint32_t> client_received;
    std::vector<std::uint32_t> server_datagrams;
    std::vector<std::uint64_t> client_send_ms(kMessageCount, 0);
    std::vector<std::uint64_t> server_latency_ms(kMessageCount, 0);
    bool server_connected = false;
    bool client_connected = false;

    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };

    server.on_stream = [&](std::uint8_t channel, const std::uint8_t* data, std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 0U);
        std::uint32_t seq = 0;
        BENTO_CHECK(ValidatePayload(data, bytes, &seq, 0x11));
        if (seq < kMessageCount) {
            server_received.push_back(seq);
            server_latency_ms[seq] = NowMs() - client_send_ms[seq];
        }
    };
    client.on_stream = [&](std::uint8_t channel, const std::uint8_t* data, std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 1U);
        std::uint32_t seq = 0;
        BENTO_CHECK(ValidatePayload(data, bytes, &seq, 0x22));
        client_received.push_back(seq);
    };
    server.on_datagram = [&](std::uint8_t channel, const std::uint8_t* data, std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 1U);
        BENTO_CHECK(bytes >= 4);
        if (bytes >= 4) {
            server_datagrams.push_back(ReadU32(data));
        }
    };

    client.Connect();
    const bool connected = bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 5000);
    BENTO_CHECK(connected);
    BENTO_CHECK_EQ(client.state(), LinkState::kConnected);
    BENTO_CHECK_EQ(server.state(), LinkState::kConnected);
    BENTO_CHECK_EQ(client.stats().handshakes, 1U);
    BENTO_CHECK_EQ(server.stats().handshakes, 1U);
    BENTO_CHECK_EQ(client.stats().hello_retransmits, 0U);

    for (std::uint32_t i = 0; i < kMessageCount; ++i) {
        const std::vector<std::uint8_t> payload = MakePayload(i, 0x11);
        client_send_ms[i] = NowMs();
        BENTO_CHECK(client.SendStream(0, payload.data(), payload.size()));
    }
    for (std::uint32_t i = 0; i < kMessageCount; ++i) {
        const std::vector<std::uint8_t> payload = MakePayload(i, 0x22);
        BENTO_CHECK(server.SendStream(1, payload.data(), payload.size()));
    }
    for (std::uint32_t i = 0; i < kDatagramCount; ++i) {
        const std::vector<std::uint8_t> payload = MakePayload(i, 0x33);
        BENTO_CHECK(client.SendDatagram(1, payload.data(), payload.size()));
    }

    const bool all_received = bento_test::PumpUntil(
        client, server,
        [&]() {
            return server_received.size() == kMessageCount &&
                   client_received.size() == kMessageCount &&
                   server_datagrams.size() == kDatagramCount &&
                   client.reliable_stats(0).inflight == 0 && server.reliable_stats(1).inflight == 0;
        },
        10000);
    BENTO_CHECK(all_received);

    BENTO_CHECK_EQ(server_received.size(), kMessageCount);
    BENTO_CHECK_EQ(client_received.size(), kMessageCount);
    BENTO_CHECK_EQ(server_datagrams.size(), kDatagramCount);
    for (std::uint32_t i = 0; i < server_received.size(); ++i) {
        BENTO_CHECK_EQ(server_received[i], i);
    }
    for (std::uint32_t i = 0; i < client_received.size(); ++i) {
        BENTO_CHECK_EQ(client_received[i], i);
    }
    for (std::uint32_t i = 0; i < server_datagrams.size(); ++i) {
        BENTO_CHECK_EQ(server_datagrams[i], i);
    }

    // 可靠层自检：全部确认、无重传、无重复/旧包。
    const auto client_stats = client.reliable_stats(0);
    const auto server_stats = server.reliable_stats(0);
    BENTO_CHECK_EQ(client_stats.messages_sent, kMessageCount);
    BENTO_CHECK_EQ(server_stats.messages_delivered, kMessageCount);
    BENTO_CHECK_EQ(client_stats.acks_applied, kMessageCount);
    BENTO_CHECK_EQ(client_stats.inflight, 0U);
    BENTO_CHECK_EQ(client_stats.unsent, 0U);
    BENTO_CHECK_EQ(client_stats.retransmits, 0U);
    BENTO_CHECK_EQ(server_stats.duplicates_dropped, 0U);
    BENTO_CHECK_EQ(server_stats.stale_dropped, 0U);
    BENTO_CHECK_EQ(client.reliable_stats(1).messages_delivered, kMessageCount);
    BENTO_CHECK_EQ(server.reliable_stats(1).acks_applied, kMessageCount);
    BENTO_CHECK_EQ(client.unreliable_stats(1).messages_sent, kDatagramCount);
    BENTO_CHECK_EQ(server.unreliable_stats(1).messages_delivered, kDatagramCount);
    BENTO_CHECK_EQ(server.unreliable_stats(1).stale_dropped, 0U);

    // 超过单条消息上限（分片数 × 1292B）必须被拒绝，不能静默截断；
    // 1301B 起走 STREAM 分片，不再拒绝（见 test_stream_fragment）。
    const std::vector<std::uint8_t> too_large(
        go2cpp::bento::kMaxStreamMessageBytes + 1, 0x5A);
    BENTO_CHECK(!client.SendStream(0, too_large.data(), too_large.size()));

    const std::uint64_t p50 = bento_test::Percentile(server_latency_ms, 0.50);
    const std::uint64_t p95 = bento_test::Percentile(server_latency_ms, 0.95);
    const std::uint64_t p99 = bento_test::Percentile(server_latency_ms, 0.99);
    std::printf(
        "[INFO] loopback: stream_tx=%llu stream_rx=%llu datagrams=%llu "
        "latency_p50=%llums p95=%llums p99=%llums\n",
        static_cast<unsigned long long>(client_stats.messages_sent),
        static_cast<unsigned long long>(server_received.size()),
        static_cast<unsigned long long>(server_datagrams.size()),
        static_cast<unsigned long long>(p50), static_cast<unsigned long long>(p95),
        static_cast<unsigned long long>(p99));

    client.Close();
    server.Close();
    BENTO_CHECK_EQ(client.state(), LinkState::kClosed);
    BENTO_CHECK_EQ(server.state(), LinkState::kClosed);

    // SACK 多块扩展：双方开启 / 单方开启（旧端）三种组合。
    RunSackNegotiationCase(true, true);
    RunSackNegotiationCase(true, false);
    RunSackNegotiationCase(false, true);
    return bento_test::Finish("test_link_loopback");
}
