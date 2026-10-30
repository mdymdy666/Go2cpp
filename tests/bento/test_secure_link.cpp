/**
 * @file test_secure_link.cpp
 * @brief PSK 认证加密链路：握手、数据加密、错误 PSK、混合模式与统计。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

std::array<std::uint8_t, kPskBytes> MakePsk(std::uint8_t seed) {
    std::array<std::uint8_t, kPskBytes> psk{};
    for (std::size_t i = 0; i < psk.size(); ++i) {
        psk[i] = static_cast<std::uint8_t>((i * 37U + seed * 11U + 0x5A) & 0xFFU);
    }
    return psk;
}

LinkConfig ServerConfig() {
    LinkConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.heartbeat_interval_ms = 100;
    config.peer_timeout_ms = 5000;
    return config;
}

LinkConfig ClientConfig(std::uint16_t server_port) {
    LinkConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.peer_address = "127.0.0.1";
    config.peer_port = server_port;
    config.heartbeat_interval_ms = 100;
    config.peer_timeout_ms = 5000;
    config.handshake_timeout_ms = 3000;
    config.random_seed = 0x5EC0DE01U;
    return config;
}

std::vector<std::uint8_t> MakePayload(std::uint32_t index, std::size_t bytes) {
    std::vector<std::uint8_t> payload(bytes, 0);
    for (std::size_t i = 0; i < bytes; ++i) {
        payload[i] = static_cast<std::uint8_t>((index * 31U + i * 7U + 3U) & 0xFFU);
    }
    payload[0] = static_cast<std::uint8_t>(index);
    payload[1] = static_cast<std::uint8_t>(index >> 8);
    return payload;
}

void TestSecureHandshakeAndData() {
    const auto psk = MakePsk(0x11);
    LinkConfig server_config = ServerConfig();
    server_config.pre_shared_key.assign(psk.begin(), psk.end());
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));
    BENTO_CHECK(server.secure());

    LinkConfig client_config = ClientConfig(server.local_port());
    client_config.pre_shared_key.assign(psk.begin(), psk.end());
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));
    BENTO_CHECK(client.secure());

    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };

    std::vector<std::vector<std::uint8_t>> received;
    std::vector<std::vector<std::uint8_t>> echoes;
    server.on_stream = [&received](std::uint8_t channel, const std::uint8_t* data,
                                   std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 0U);
        received.emplace_back(data, data + bytes);
    };
    client.on_stream = [&echoes](std::uint8_t channel, const std::uint8_t* data,
                                 std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 1U);
        echoes.emplace_back(data, data + bytes);
    };

    client.Connect();
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 5000));
    BENTO_CHECK(server.session_established());
    BENTO_CHECK(client.session_established());
    BENTO_CHECK_EQ(server.secure_stats().handshakes, 1U);
    BENTO_CHECK_EQ(client.secure_stats().handshakes, 1U);

    constexpr std::uint32_t kMessages = 40;
    for (std::uint32_t i = 0; i < kMessages; ++i) {
        const std::vector<std::uint8_t> payload = MakePayload(i, 96);
        BENTO_CHECK(client.SendStream(0, payload.data(), payload.size()));
        BENTO_CHECK(server.SendStream(1, payload.data(), payload.size()));
    }

    const bool all_received = bento_test::PumpUntil(
        client, server,
        [&]() {
            return received.size() == kMessages && echoes.size() == kMessages &&
                   client.reliable_stats(0).inflight == 0 &&
                   server.reliable_stats(1).inflight == 0;
        },
        10000);
    BENTO_CHECK(all_received);
    BENTO_CHECK_EQ(received.size(), kMessages);
    BENTO_CHECK_EQ(echoes.size(), kMessages);
    for (std::uint32_t i = 0; i < received.size(); ++i) {
        BENTO_CHECK(received[i] == MakePayload(i, 96));
        BENTO_CHECK(echoes[i] == MakePayload(i, 96));
    }

    const auto server_secure = server.secure_stats();
    const auto client_secure = client.secure_stats();
    BENTO_CHECK(server_secure.packets_opened > 0);
    BENTO_CHECK(client_secure.packets_opened > 0);
    BENTO_CHECK(server_secure.packets_sealed > 0);
    BENTO_CHECK(client_secure.packets_sealed > 0);
    BENTO_CHECK(server_secure.encrypted_bytes_received > 0);
    BENTO_CHECK(client_secure.encrypted_bytes_sent > 0);
    // 正常链路不应出现认证失败/重放/明文帧。
    BENTO_CHECK_EQ(server_secure.auth_failures, 0U);
    BENTO_CHECK_EQ(server_secure.replay_drops, 0U);
    BENTO_CHECK_EQ(server_secure.plaintext_rejected, 0U);
    BENTO_CHECK_EQ(server.reliable_stats(0).retransmits, 0U);

    // 拥塞控制统计经 Link 导出。
    const CongestionStats congestion = client.congestion_stats(0);
    BENTO_CHECK(congestion.cwnd_packets >= congestion.min_cwnd_packets);
    BENTO_CHECK(congestion.packets_sent >= kMessages);
    BENTO_CHECK(congestion.pacing_rate_bps > 0);

    client.Close();
    server.Close();
}

void TestWrongPskNeverConnects() {
    const auto server_psk = MakePsk(0x21);
    const auto client_psk = MakePsk(0x22);
    LinkConfig server_config = ServerConfig();
    server_config.pre_shared_key.assign(server_psk.begin(), server_psk.end());
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    LinkConfig client_config = ClientConfig(server.local_port());
    client_config.pre_shared_key.assign(client_psk.begin(), client_psk.end());
    client_config.hello_initial_pto_ms = 50;
    client_config.hello_max_pto_ms = 100;
    client_config.hello_max_attempts = 3;
    client_config.handshake_timeout_ms = 400;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));

    bool client_connected = false;
    bool server_connected = false;
    client.on_connected = [&client_connected]() { client_connected = true; };
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.Connect();
    (void)bento_test::PumpUntil(client, server, []() { return false; }, 800);

    BENTO_CHECK(!client_connected);
    BENTO_CHECK(!server_connected);
    BENTO_CHECK(client.state() == LinkState::kTimedOut);
    BENTO_CHECK(!client.session_established());
    BENTO_CHECK(!server.session_established());
    BENTO_CHECK(server.secure_stats().auth_failures > 0);
    BENTO_CHECK_EQ(server.secure_stats().handshakes, 0U);

    client.Close();
    server.Close();
}

void TestPlaintextClientRejectedBySecureServer() {
    const auto psk = MakePsk(0x31);
    LinkConfig server_config = ServerConfig();
    server_config.pre_shared_key.assign(psk.begin(), psk.end());
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    // 客户端不配置 PSK：HELLO 是明文，安全监听方必须拒绝而不是降级。
    // 这是「故意混用明文端」的用例，必须显式关闭默认加密策略才能 Open。
    LinkConfig client_config = ClientConfig(server.local_port());
    client_config.require_encryption = false;
    client_config.hello_initial_pto_ms = 50;
    client_config.hello_max_pto_ms = 100;
    client_config.hello_max_attempts = 3;
    client_config.handshake_timeout_ms = 400;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));
    client.Connect();
    (void)bento_test::PumpUntil(client, server, []() { return false; }, 800);

    BENTO_CHECK(client.state() == LinkState::kTimedOut);
    BENTO_CHECK(server.secure_stats().malformed_frames > 0);
    BENTO_CHECK_EQ(server.secure_stats().handshakes, 0U);

    client.Close();
    server.Close();
}

void TestSecureClientRejectedByPlaintextServer() {
    const auto psk = MakePsk(0x41);
    // 明文监听端是显式不安全配置；默认策略下 Open 会直接拒绝。
    LinkConfig server_config = ServerConfig();
    server_config.require_encryption = false;
    Link server(server_config);
    std::string error;
    BENTO_CHECK(server.Open(&error));

    LinkConfig client_config = ClientConfig(server.local_port());
    client_config.pre_shared_key.assign(psk.begin(), psk.end());
    client_config.hello_initial_pto_ms = 50;
    client_config.hello_max_pto_ms = 100;
    client_config.hello_max_attempts = 3;
    client_config.handshake_timeout_ms = 400;
    Link client(client_config);
    BENTO_CHECK(client.Open(&error));
    client.Connect();
    (void)bento_test::PumpUntil(client, server, []() { return false; }, 800);

    BENTO_CHECK(client.state() == LinkState::kTimedOut);
    BENTO_CHECK(server.stats().invalid_frames > 0);
    BENTO_CHECK_EQ(server.stats().handshakes, 0U);
    BENTO_CHECK_EQ(server.secure_stats().handshakes, 0U);

    client.Close();
    server.Close();
}

}  // namespace

int main() {
    TestSecureHandshakeAndData();
    TestWrongPskNeverConnects();
    TestPlaintextClientRejectedBySecureServer();
    TestSecureClientRejectedByPlaintextServer();
    return bento_test::Finish("test_secure_link");
}
