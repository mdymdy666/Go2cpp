/**
 * @file test_secure_injection.cpp
 * @brief 链路层故障注入：篡改 / 截断 / 重放 / 明文注入必须被拒绝，
 *        同时可靠流仍 100% 按序送达。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "fault_proxy.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

std::array<std::uint8_t, kPskBytes> MakePsk() {
    std::array<std::uint8_t, kPskBytes> psk{};
    for (std::size_t i = 0; i < psk.size(); ++i) {
        psk[i] = static_cast<std::uint8_t>((i * 53U + 0x9B) & 0xFFU);
    }
    return psk;
}

std::vector<std::uint8_t> MakePayload(std::uint32_t index, std::size_t bytes) {
    std::vector<std::uint8_t> payload(bytes, 0);
    for (std::size_t i = 0; i < bytes; ++i) {
        payload[i] = static_cast<std::uint8_t>((index * 29U + i * 5U + 7U) & 0xFFU);
    }
    payload[0] = static_cast<std::uint8_t>(index);
    payload[1] = static_cast<std::uint8_t>(index >> 8);
    payload[2] = static_cast<std::uint8_t>(index >> 16);
    payload[3] = static_cast<std::uint8_t>(index >> 24);
    return payload;
}

}  // namespace

int main() {
    const auto psk = MakePsk();

    LinkConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 100;
    server_config.peer_timeout_ms = 5000;
    server_config.pre_shared_key.assign(psk.begin(), psk.end());
    Link server(server_config);
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "[FAIL] server.Open: %s\n", error.c_str());
        return 1;
    }

    sockaddr_in server_address{};
    if (!UdpSocket::ResolveIPv4("127.0.0.1", server.local_port(), &server_address, &error)) {
        std::fprintf(stderr, "[FAIL] resolve: %s\n", error.c_str());
        return 1;
    }

    bento_test::FaultProxy proxy(
        server_address,
        {bento_test::FaultProxy::Fault::kTamperTag,
         bento_test::FaultProxy::Fault::kTruncate,
         bento_test::FaultProxy::Fault::kReplay,
         bento_test::FaultProxy::Fault::kPlaintext});
    if (!proxy.Start(&error)) {
        std::fprintf(stderr, "[FAIL] proxy.Start: %s\n", error.c_str());
        return 1;
    }

    LinkConfig client_config;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.peer_address = "127.0.0.1";
    client_config.peer_port = proxy.port();
    client_config.heartbeat_interval_ms = 100;
    client_config.peer_timeout_ms = 5000;
    client_config.handshake_timeout_ms = 3000;
    client_config.random_seed = 0xBADF00D1U;
    client_config.pre_shared_key.assign(psk.begin(), psk.end());
    Link client(client_config);
    if (!client.Open(&error)) {
        std::fprintf(stderr, "[FAIL] client.Open: %s\n", error.c_str());
        return 1;
    }

    bool server_connected = false;
    bool client_connected = false;
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.on_connected = [&client_connected]() { client_connected = true; };

    std::vector<std::vector<std::uint8_t>> received;
    server.on_stream = [&received](std::uint8_t channel, const std::uint8_t* data,
                                   std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 0U);
        received.emplace_back(data, data + bytes);
    };

    client.Connect();
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 5000));

    constexpr std::uint32_t kMessages = 10;
    for (std::uint32_t i = 0; i < kMessages; ++i) {
        const std::vector<std::uint8_t> payload = MakePayload(i, 64);
        BENTO_CHECK(client.SendStream(0, payload.data(), payload.size()));
    }

    const bool all_received = bento_test::PumpUntil(
        client, server,
        [&]() {
            return received.size() == kMessages && client.reliable_stats(0).inflight == 0;
        },
        10000);
    // 再 pump 一段时间，确保 50ms 延迟的重放包已到达服务端。
    (void)bento_test::PumpUntil(client, server, []() { return false; }, 200);

    BENTO_CHECK(all_received);
    BENTO_CHECK_EQ(received.size(), kMessages);
    for (std::uint32_t i = 0; i < received.size(); ++i) {
        BENTO_CHECK(received[i] == MakePayload(i, 64));
    }

    const auto server_secure = server.secure_stats();
    const auto client_reliable = client.reliable_stats(0);
    BENTO_CHECK(server_secure.auth_failures >= 1);
    BENTO_CHECK(server_secure.malformed_frames >= 1);
    BENTO_CHECK(server_secure.replay_drops >= 1);
    BENTO_CHECK(server_secure.plaintext_rejected >= 1);
    BENTO_CHECK(client_reliable.retransmits >= 1);
    BENTO_CHECK(client.state() == LinkState::kConnected);
    BENTO_CHECK(server.state() == LinkState::kConnected);
    BENTO_CHECK(proxy.tampered() == 1U);
    BENTO_CHECK(proxy.truncated() == 1U);
    BENTO_CHECK(proxy.replayed() == 1U);
    BENTO_CHECK(proxy.injected() == 1U);

    std::printf(
        "[INFO] secure injection: delivered=%zu auth_failures=%llu malformed=%llu "
        "replay=%llu plaintext=%llu retransmits=%llu\n",
        received.size(),
        static_cast<unsigned long long>(server_secure.auth_failures),
        static_cast<unsigned long long>(server_secure.malformed_frames),
        static_cast<unsigned long long>(server_secure.replay_drops),
        static_cast<unsigned long long>(server_secure.plaintext_rejected),
        static_cast<unsigned long long>(client_reliable.retransmits));

    client.Close();
    server.Close();
    proxy.Stop();
    return bento_test::Finish("test_secure_injection");
}
