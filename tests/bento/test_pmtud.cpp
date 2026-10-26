/**
 * @file test_pmtud.cpp
 * @brief P2 路径 MTU：DF 探针 1200→1500、回环探测、ICMP 降档与下限夹逼。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <string>

#include "go2cpp/bento/net/link.hpp"
#include "test_util.hpp"

namespace {
using namespace go2cpp::bento;
}  // namespace

int main() {
    LinkConfig server_config;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 50;
    server_config.peer_timeout_ms = 5000;
    server_config.enable_pmtud = false;  // 只回应探针，不主动探测
    Link server(server_config);
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "[FAIL] server.Open: %s\n", error.c_str());
        return 1;
    }

    LinkConfig client_config;
    client_config.bind_address = "127.0.0.1";
    client_config.bind_port = 0;
    client_config.peer_address = "127.0.0.1";
    client_config.peer_port = server.local_port();
    client_config.heartbeat_interval_ms = 50;
    client_config.peer_timeout_ms = 5000;
    client_config.enable_pmtud = true;
    client_config.pmtud_probe_interval_ms = 10;
    client_config.pmtud_probe_timeout_ms = 200;
    client_config.pmtud_probe_attempts = 2;
    client_config.pmtud_max_mtu = 1500;
    Link client(client_config);
    if (!client.Open(&error)) {
        std::fprintf(stderr, "[FAIL] client.Open: %s\n", error.c_str());
        return 1;
    }

    // 未探测 / 无 ICMP 时必须是保守的 1200。
    BENTO_CHECK_EQ(client.PathMtu(), 1200U);

    bool client_connected = false;
    bool server_connected = false;
    client.on_connected = [&client_connected]() { client_connected = true; };
    server.on_connected = [&server_connected]() { server_connected = true; };
    client.Connect();
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 5000));

    // 回环 MTU 为 65536，应能通过 1200/1300/1400/1500 全部探针。
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return client.PathMtu() >= 1500; }, 3000));
    const std::uint32_t discovered = client.PathMtu();
    BENTO_CHECK_EQ(discovered, 1500U);
    BENTO_CHECK(client.stats().pmtud_probes_sent >= 4U);
    BENTO_CHECK(server.stats().pmtud_probes_received >= 4U);
    BENTO_CHECK(client.stats().pmtud_acks_received >= 4U);

    // 注入 ICMP “fragmentation needed, MTU=1100”：只降不升。
    const std::uint64_t probes_before = client.stats().pmtud_probes_sent;
    client.NotifyPathMtuError(1100);
    BENTO_CHECK_EQ(client.PathMtu(), 1100U);
    BENTO_CHECK_EQ(client.stats().pmtud_icmp_errors, 1U);
    (void)bento_test::PumpUntil(client, server, []() { return false; }, 150);
    BENTO_CHECK_EQ(client.PathMtu(), 1100U);
    BENTO_CHECK_EQ(client.stats().pmtud_probes_sent, probes_before);

    // 低于 RFC 791 下限的报告被夹到 576；之后更高的报告不会把 MTU 拉回。
    client.NotifyPathMtuError(100);
    BENTO_CHECK_EQ(client.PathMtu(), 576U);
    client.NotifyPathMtuError(2000);
    BENTO_CHECK_EQ(client.PathMtu(), 576U);

    // 降档不能影响数据面。
    const std::string message = "pmtud-still-works";
    BENTO_CHECK(client.SendStream(0, message.data(), message.size()));
    bool delivered = false;
    server.on_stream = [&delivered, message](std::uint8_t, const std::uint8_t* data,
                                             std::size_t bytes) {
        delivered = std::string(reinterpret_cast<const char*>(data), bytes) == message;
    };
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return delivered; }, 3000));

    std::printf(
        "[INFO] pmtud: discovered=%u final=%u probes=%llu acks=%llu rx_probes=%llu "
        "icmp=%llu\n",
        discovered, client.PathMtu(),
        static_cast<unsigned long long>(client.stats().pmtud_probes_sent),
        static_cast<unsigned long long>(client.stats().pmtud_acks_received),
        static_cast<unsigned long long>(server.stats().pmtud_probes_received),
        static_cast<unsigned long long>(client.stats().pmtud_icmp_errors));

    client.Close();
    server.Close();
    return bento_test::Finish("test_pmtud");
}
