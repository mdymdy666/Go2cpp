/**
 * @file test_ipv6.cpp
 * @brief IPv6 回环：getaddrinfo(AF_UNSPEC)、::1 绑定/连接与双向 STREAM。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

constexpr std::uint32_t kMessageCount = 20;
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

std::vector<std::uint8_t> MakePayload(std::uint32_t seq) {
    std::vector<std::uint8_t> payload(kPayloadBytes, 0);
    WriteU32(payload.data(), seq);
    WriteU32(payload.data() + 4, seq ^ 0xA5A5A5A5U);
    for (std::size_t i = 8; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((seq * 31U + i) & 0xFFU);
    }
    return payload;
}

bool ValidatePayload(const std::uint8_t* data, std::size_t bytes, std::uint32_t expected) {
    if (bytes != kPayloadBytes) {
        return false;
    }
    const std::uint32_t seq = ReadU32(data);
    if (seq != expected || ReadU32(data + 4) != (seq ^ 0xA5A5A5A5U)) {
        return false;
    }
    for (std::size_t i = 8; i < bytes; ++i) {
        if (data[i] != static_cast<std::uint8_t>((seq * 31U + i) & 0xFFU)) {
            return false;
        }
    }
    return true;
}

void TestResolveIpv6() {
    sockaddr_storage storage{};
    socklen_t length = 0;
    std::string error;
    const bool resolved = UdpSocket::Resolve("::1", 12345, &storage, &length, &error);
    BENTO_CHECK(resolved);
    if (!resolved) {
        std::fprintf(stderr, "[FAIL] resolve ::1: %s\n", error.c_str());
        return;
    }
    BENTO_CHECK_EQ(storage.ss_family, AF_INET6);
    BENTO_CHECK_EQ(length, static_cast<socklen_t>(sizeof(sockaddr_in6)));

    sockaddr_storage second{};
    socklen_t second_length = 0;
    BENTO_CHECK(UdpSocket::Resolve("::1", 12345, &second, &second_length, &error));
    BENTO_CHECK(UdpSocket::SamePeer(storage, second));

    sockaddr_storage ipv4{};
    socklen_t ipv4_length = 0;
    BENTO_CHECK(UdpSocket::Resolve("127.0.0.1", 12345, &ipv4, &ipv4_length, &error));
    BENTO_CHECK_EQ(ipv4.ss_family, AF_INET);
    BENTO_CHECK(!UdpSocket::SamePeer(storage, ipv4));
}

void TestIpv6Loopback() {
    sockaddr_storage probe{};
    socklen_t probe_length = 0;
    std::string error;
    if (!UdpSocket::Resolve("::1", 0, &probe, &probe_length, &error)) {
        std::printf("[SKIP] IPv6 unavailable: %s\n", error.c_str());
        return;
    }

    LinkConfig server_config;
    server_config.require_encryption = false;
    server_config.bind_address = "::1";
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 5000;
    Link server(server_config);
    if (!server.Open(&error)) {
        std::fprintf(stderr, "[FAIL] server.Open(::1): %s\n", error.c_str());
        BENTO_CHECK(false);
        return;
    }
    BENTO_CHECK(server.local_port() > 0);

    LinkConfig client_config;
    client_config.require_encryption = false;
    client_config.bind_address = "::1";
    // remote 必须优先于旧字段 peer_address（这里故意给不可达 IPv4）。
    client_config.remote = "::1";
    client_config.peer_address = "192.0.2.1";
    client_config.peer_port = server.local_port();
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 5000;
    client_config.random_seed = 0x1F0B6U;
    Link client(client_config);
    if (!client.Open(&error)) {
        std::fprintf(stderr, "[FAIL] client.Open(::1): %s\n", error.c_str());
        BENTO_CHECK(false);
        return;
    }

    bool client_connected = false;
    bool server_connected = false;
    client.on_connected = [&client_connected]() { client_connected = true; };
    server.on_connected = [&server_connected]() { server_connected = true; };
    std::vector<std::uint32_t> server_received;
    std::vector<std::uint32_t> client_received;
    server.on_stream = [&server_received](std::uint8_t channel, const std::uint8_t* data,
                                          std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 0U);
        if (bytes >= 4) {
            const std::uint32_t seq = ReadU32(data);
            BENTO_CHECK(ValidatePayload(data, bytes, seq));
            server_received.push_back(seq);
        } else {
            BENTO_CHECK(false);
        }
    };
    client.on_stream = [&client_received](std::uint8_t channel, const std::uint8_t* data,
                                          std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 1U);
        if (bytes >= 4) {
            const std::uint32_t seq = ReadU32(data);
            BENTO_CHECK(ValidatePayload(data, bytes, seq));
            client_received.push_back(seq);
        } else {
            BENTO_CHECK(false);
        }
    };

    client.Connect();
    const bool connected = bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 5000);
    BENTO_CHECK(connected);
    BENTO_CHECK_EQ(client.state(), LinkState::kConnected);
    BENTO_CHECK_EQ(server.state(), LinkState::kConnected);

    for (std::uint32_t i = 0; i < kMessageCount; ++i) {
        const std::vector<std::uint8_t> payload = MakePayload(i);
        BENTO_CHECK(client.SendStream(0, payload.data(), payload.size()));
        BENTO_CHECK(server.SendStream(1, payload.data(), payload.size()));
    }

    const bool delivered = bento_test::PumpUntil(
        client, server,
        [&]() {
            return server_received.size() == kMessageCount &&
                   client_received.size() == kMessageCount;
        },
        10000);
    BENTO_CHECK(delivered);
    BENTO_CHECK_EQ(server_received.size(), kMessageCount);
    BENTO_CHECK_EQ(client_received.size(), kMessageCount);
    for (std::size_t i = 0; i < server_received.size(); ++i) {
        BENTO_CHECK_EQ(server_received[i], static_cast<std::uint32_t>(i));
    }
    for (std::size_t i = 0; i < client_received.size(); ++i) {
        BENTO_CHECK_EQ(client_received[i], static_cast<std::uint32_t>(i));
    }
    std::printf("[INFO] ipv6: port=%u stream_tx=%llu stream_rx=%llu\n",
                static_cast<unsigned>(server.local_port()),
                static_cast<unsigned long long>(server.reliable_stats(0).messages_delivered),
                static_cast<unsigned long long>(client.reliable_stats(1).messages_delivered));

    client.Close();
    server.Close();
}

}  // namespace

int main() {
    TestResolveIpv6();
    TestIpv6Loopback();
    return bento_test::Finish("test_ipv6");
}
