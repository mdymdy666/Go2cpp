/**
 * @file test_zero_rtt.cpp
 * @brief P2 0-RTT：票据恢复后首个包即加密发送、对端无需 HELLO；
 *        过期/篡改/错误 PSK 的票据被拒绝。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/crypto.hpp"
#include "go2cpp/bento/net/link.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

std::vector<std::uint8_t> MakePsk(std::uint8_t seed) {
    std::vector<std::uint8_t> psk(kPskBytes, 0);
    for (std::size_t i = 0; i < psk.size(); ++i) {
        psk[i] = static_cast<std::uint8_t>((i * 53U + seed) & 0xFFU);
    }
    return psk;
}

LinkConfig ListenerConfig(const std::vector<std::uint8_t>& psk) {
    LinkConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.heartbeat_interval_ms = 50;
    config.peer_timeout_ms = 5000;
    config.pre_shared_key = psk;
    // 0-RTT 票据只按静态 PSK 恢复会话密钥；X25519 会话不允许导出票据
    // （避免静默降级），因此这里显式用纯 PSK 模式测 0-RTT。
    // 纯 PSK 是显式降级（无前向保密），默认策略要求 allow_legacy_peer=true。
    config.enable_x25519 = false;
    config.allow_legacy_peer = true;
    return config;
}

LinkConfig ClientConfig(const std::vector<std::uint8_t>& psk, std::uint16_t server_port) {
    LinkConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.peer_address = "127.0.0.1";
    config.peer_port = server_port;
    config.heartbeat_interval_ms = 50;
    config.peer_timeout_ms = 5000;
    config.pre_shared_key = psk;
    config.enable_x25519 = false;
    config.allow_legacy_peer = true;
    return config;
}

}  // namespace

int main() {
    Link::ClearSessionCache();
    const std::vector<std::uint8_t> psk = MakePsk(0x37);

    Link server(ListenerConfig(psk));
    std::string error;
    if (!server.Open(&error)) {
        std::fprintf(stderr, "[FAIL] server.Open: %s\n", error.c_str());
        return 1;
    }
    Link client(ClientConfig(psk, server.local_port()));
    if (!client.Open(&error)) {
        std::fprintf(stderr, "[FAIL] client.Open: %s\n", error.c_str());
        return 1;
    }

    bool client_connected = false;
    bool server_connected = false;
    client.on_connected = [&client_connected]() { client_connected = true; };
    server.on_connected = [&server_connected]() { server_connected = true; };
    std::vector<std::string> datagrams;
    server.on_datagram = [&datagrams](std::uint8_t channel, const std::uint8_t* data,
                                      std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 1U);
        datagrams.emplace_back(reinterpret_cast<const char*>(data), bytes);
    };
    std::vector<std::string> streams;
    server.on_stream = [&streams](std::uint8_t channel, const std::uint8_t* data,
                                  std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 0U);
        streams.emplace_back(reinterpret_cast<const char*>(data), bytes);
    };

    client.Connect();
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return client_connected && server_connected; }, 5000));
    BENTO_CHECK(client.session_established());

    const char* before = "before";
    BENTO_CHECK(client.SendDatagram(1, before, std::strlen(before)));
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return datagrams.size() == 1; }, 3000));

    const std::vector<std::uint8_t> ticket = client.ExportSessionTicket();
    BENTO_CHECK_EQ(ticket.size(), kSessionTicketBytes);
    const std::uint32_t conn_id = client.conn_id();
    const std::uint64_t server_handshakes = server.stats().handshakes;
    BENTO_CHECK_EQ(server_handshakes, 1U);

    // 断开旧连接，用票据在新 Link 上直接恢复；不调用 Connect、不发 HELLO。
    client.Close();
    Link client2(ClientConfig(psk, server.local_port()));
    if (!client2.Open(&error)) {
        std::fprintf(stderr, "[FAIL] client2.Open: %s\n", error.c_str());
        return 1;
    }
    bool resumed_connected = false;
    client2.on_connected = [&resumed_connected]() { resumed_connected = true; };
    BENTO_CHECK(client2.Resume(ticket));
    BENTO_CHECK(resumed_connected);
    BENTO_CHECK(client2.connected());
    BENTO_CHECK(client2.session_established());
    BENTO_CHECK(client2.resumed());
    BENTO_CHECK_EQ(client2.conn_id(), conn_id);
    BENTO_CHECK_EQ(client2.stats().handshakes, 0U);

    // 首个数据包立即加密发送（0-RTT）。

    const char* first = "first-0rtt";
    BENTO_CHECK(client2.SendDatagram(1, first, std::strlen(first)));
    BENTO_CHECK(bento_test::PumpUntil(
        client2, server, [&]() { return datagrams.size() == 2; }, 5000));
    BENTO_CHECK_EQ(datagrams[1], "first-0rtt");
    BENTO_CHECK_EQ(server.stats().handshakes, server_handshakes);
    BENTO_CHECK(server.secure_stats().resumptions >= 1U);
    BENTO_CHECK(server.secure_stats().zero_rtt_frames >= 1U);
    BENTO_CHECK(server.secure_stats().packets_opened >= 2U);

    // 恢复后的双向可靠流仍可用（服务端 ACK 使用恢复出的旧会话密钥）。
    const std::string stream_message = "after-resume";
    BENTO_CHECK(client2.SendStream(0, stream_message.data(), stream_message.size()));
    BENTO_CHECK(bento_test::PumpUntil(
        client2, server, [&]() { return streams.size() == 1; }, 5000));
    BENTO_CHECK_EQ(streams[0], "after-resume");

    // 篡改票据：MAC 校验失败。
    Link bad_client(ClientConfig(psk, server.local_port()));
    BENTO_CHECK(bad_client.Open(&error));
    std::vector<std::uint8_t> tampered = ticket;
    tampered.back() = static_cast<std::uint8_t>(tampered.back() ^ 0x80U);
    BENTO_CHECK(!bad_client.Resume(tampered));
    BENTO_CHECK(bad_client.secure_stats().tickets_rejected >= 1U);

    // 错误 PSK：票据 MAC 无法验证。
    Link wrong_psk_client(ClientConfig(MakePsk(0x99), server.local_port()));
    BENTO_CHECK(wrong_psk_client.Open(&error));
    BENTO_CHECK(!wrong_psk_client.Resume(ticket));

    // 过期票据：1ms 生命周期，等待 50ms 后拒绝。
    Link expiry_server(ListenerConfig(psk));
    BENTO_CHECK(expiry_server.Open(&error));
    LinkConfig short_lived = ClientConfig(psk, expiry_server.local_port());
    short_lived.session_ticket_lifetime_ms = 1;
    Link expiry_client(short_lived);
    BENTO_CHECK(expiry_client.Open(&error));
    bool expiry_connected = false;
    expiry_client.on_connected = [&expiry_connected]() { expiry_connected = true; };
    expiry_client.Connect();
    BENTO_CHECK(bento_test::PumpUntil(
        expiry_client, expiry_server, [&]() { return expiry_connected; }, 5000));
    const std::vector<std::uint8_t> short_ticket = expiry_client.ExportSessionTicket();
    BENTO_CHECK_EQ(short_ticket.size(), kSessionTicketBytes);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Link expired_resume(ClientConfig(psk, expiry_server.local_port()));
    BENTO_CHECK(expired_resume.Open(&error));
    BENTO_CHECK(!expired_resume.Resume(short_ticket));
    BENTO_CHECK(expired_resume.secure_stats().tickets_rejected >= 1U);

    // 服务端对象重建（同进程、同端口）：凭 conn_id + 进程内会话缓存接受
    // 0-RTT 首包，不要求服务端重新经历 HELLO。
    const std::vector<std::uint8_t> cold_ticket = client2.ExportSessionTicket();
    BENTO_CHECK_EQ(cold_ticket.size(), kSessionTicketBytes);
    const std::uint16_t server_port = server.local_port();
    client2.Close();
    server.Close();

    LinkConfig cold_server_config = ListenerConfig(psk);
    cold_server_config.bind_port = server_port;
    Link cold_server(cold_server_config);
    BENTO_CHECK(cold_server.Open(&error));
    Link cold_client(ClientConfig(psk, server_port));
    BENTO_CHECK(cold_client.Open(&error));
    std::vector<std::string> cold_datagrams;
    cold_server.on_datagram = [&cold_datagrams](std::uint8_t channel, const std::uint8_t* data,
                                                std::size_t bytes) {
        BENTO_CHECK_EQ(channel, 1U);
        cold_datagrams.emplace_back(reinterpret_cast<const char*>(data), bytes);
    };
    BENTO_CHECK(cold_client.Resume(cold_ticket));
    const char* cold_message = "cold-0rtt";
    BENTO_CHECK(cold_client.SendDatagram(1, cold_message, std::strlen(cold_message)));
    BENTO_CHECK(bento_test::PumpUntil(
        cold_client, cold_server, [&]() { return cold_datagrams.size() == 1; }, 5000));
    BENTO_CHECK_EQ(cold_datagrams[0], "cold-0rtt");
    BENTO_CHECK_EQ(cold_server.stats().handshakes, 0U);
    BENTO_CHECK(cold_server.secure_stats().resumptions >= 1U);
    BENTO_CHECK(cold_server.secure_stats().zero_rtt_frames >= 1U);

    // 服务端缓存条目过期（客户端票据仍有效）：重建监听方后，进程内缓存不能让
    // 过期会话被 0-RTT 恢复 —— 查询路径必须惰性删除并丢弃首包。
    LinkConfig short_cache_config = ListenerConfig(psk);
    short_cache_config.session_ticket_lifetime_ms = 20;
    Link short_cache_server(short_cache_config);
    BENTO_CHECK(short_cache_server.Open(&error));
    const std::uint16_t short_cache_port = short_cache_server.local_port();
    Link cache_client(ClientConfig(psk, short_cache_port));
    BENTO_CHECK(cache_client.Open(&error));
    bool cache_connected = false;
    cache_client.on_connected = [&cache_connected]() { cache_connected = true; };
    cache_client.Connect();
    BENTO_CHECK(bento_test::PumpUntil(
        cache_client, short_cache_server, [&]() { return cache_connected; }, 5000));
    // 监听方导出票据时会把会话写进进程内缓存（20ms 后过期）。
    BENTO_CHECK_EQ(short_cache_server.ExportSessionTicket().size(), kSessionTicketBytes);
    const std::vector<std::uint8_t> live_ticket = cache_client.ExportSessionTicket();
    BENTO_CHECK_EQ(live_ticket.size(), kSessionTicketBytes);
    cache_client.Close();
    short_cache_server.Close();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    LinkConfig rebuilt_config = short_cache_config;
    rebuilt_config.bind_port = short_cache_port;
    Link rebuilt(rebuilt_config);
    BENTO_CHECK(rebuilt.Open(&error));
    std::size_t delivered = 0;
    rebuilt.on_datagram = [&delivered](std::uint8_t, const std::uint8_t*, std::size_t) {
        ++delivered;
    };
    Link late_client(ClientConfig(psk, short_cache_port));
    BENTO_CHECK(late_client.Open(&error));
    BENTO_CHECK(late_client.Resume(live_ticket));  // 客户端票据 60s 有效
    const char* late_message = "stale-cache";
    BENTO_CHECK(late_client.SendDatagram(1, late_message, std::strlen(late_message)));
    (void)bento_test::PumpUntil(late_client, rebuilt, []() { return false; }, 100);
    BENTO_CHECK_EQ(delivered, 0U);
    BENTO_CHECK_EQ(rebuilt.secure_stats().resumptions, 0U);
    late_client.Close();
    rebuilt.Close();

    std::printf(
        "[INFO] 0rtt: ticket=%zuB handshakes=%llu resumptions=%llu zero_rtt=%llu "
        "opened=%llu\n",
        ticket.size(), static_cast<unsigned long long>(server.stats().handshakes),
        static_cast<unsigned long long>(server.secure_stats().resumptions),
        static_cast<unsigned long long>(server.secure_stats().zero_rtt_frames),
        static_cast<unsigned long long>(server.secure_stats().packets_opened));

    client2.Close();
    cold_client.Close();
    cold_server.Close();
    bad_client.Close();
    wrong_psk_client.Close();
    expired_resume.Close();
    expiry_client.Close();
    expiry_server.Close();
    server.Close();
    Link::ClearSessionCache();
    return bento_test::Finish("test_zero_rtt");
}
