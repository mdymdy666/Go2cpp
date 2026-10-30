/**
 * @file test_conn_migration.cpp
 * @brief P2 连接迁移：同一 conn_id 换本地 socket 后会话/可靠流继续；
 *        旧地址与错误 conn_id 的包被忽略。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "go2cpp/bento/net/link.hpp"
#include "go2cpp/bento/net/protocol.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento;

bool SendRawDatagram(UdpSocket& socket, const sockaddr_in& destination, std::uint32_t conn_id,
                     std::uint32_t seq, const char* text) {
    FrameHeader header;
    header.type = FrameType::kDatagram;
    header.channel = 1;
    header.flags = 0;
    header.conn_id = conn_id;
    header.seq = seq;
    const auto* payload = reinterpret_cast<const std::uint8_t*>(text);
    const std::vector<std::uint8_t> frame =
        BuildFrame(header, nullptr, payload, std::strlen(text));
    if (frame.empty()) {
        return false;
    }
    return socket.SendTo(destination, frame.data(), frame.size());
}

}  // namespace

int main() {
    Link::ClearSessionCache();

    LinkConfig server_config;
    server_config.require_encryption = false;
    server_config.bind_address = "127.0.0.1";
    server_config.bind_port = 0;
    server_config.heartbeat_interval_ms = 50;
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
    client_config.heartbeat_interval_ms = 50;
    client_config.peer_timeout_ms = 5000;
    Link client(client_config);
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
    BENTO_CHECK_EQ(client.conn_id(), server.conn_id());
    const std::uint32_t conn_id = client.conn_id();
    const std::uint32_t session_id = client.session_id();
    const std::uint16_t old_port = client.local_port();
    BENTO_CHECK(conn_id != 0);
    BENTO_CHECK(old_port != 0);

    const char* before = "before-migration";
    BENTO_CHECK(client.SendDatagram(1, before, std::strlen(before)));
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return datagrams.size() == 1; }, 3000));

    // 换本地地址（新端口）：Link 内部重绑 socket，conn_id/会话状态不变。
    BENTO_CHECK(client.Migrate("127.0.0.1", 0, &error));
    BENTO_CHECK(client.local_port() != old_port);
    BENTO_CHECK_EQ(client.conn_id(), conn_id);
    BENTO_CHECK_EQ(client.session_id(), session_id);

    const char* after = "after-migration";
    BENTO_CHECK(client.SendDatagram(1, after, std::strlen(after)));
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return datagrams.size() == 2; }, 3000));
    BENTO_CHECK_EQ(datagrams[1], after);
    BENTO_CHECK(server.stats().peer_migrations >= 1);
    BENTO_CHECK(server.connected());

    // 可靠流跨迁移继续（通道状态未重置）。
    for (int i = 0; i < 3; ++i) {
        const std::string message = "stream-" + std::to_string(i);
        BENTO_CHECK(client.SendStream(0, message.data(), message.size()));
    }
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return streams.size() == 3; }, 5000));
    BENTO_CHECK_EQ(streams.size(), 3U);
    BENTO_CHECK_EQ(streams[0], "stream-0");
    BENTO_CHECK_EQ(streams[2], "stream-2");

    // 旧地址包必须被忽略：迁移后旧端口已释放，测试重新绑定它注入旧包。
    sockaddr_in server_address{};
    BENTO_CHECK(UdpSocket::ResolveIPv4("127.0.0.1", server.local_port(), &server_address, &error));
    UdpSocket stale_socket;
    BENTO_CHECK(stale_socket.Open("127.0.0.1", old_port, &error));
    BENTO_CHECK(SendRawDatagram(stale_socket, server_address, conn_id, 900, "stale-old-address"));
    (void)server.RunOnce(5);
    BENTO_CHECK_EQ(datagrams.size(), 2U);
    BENTO_CHECK(server.stats().stale_peer_frames >= 1);
    BENTO_CHECK(server.connected());

    // 错误 conn_id：从全新地址发送，按 conn_id 路由直接丢弃。
    UdpSocket bogus_socket;
    BENTO_CHECK(bogus_socket.Open("127.0.0.1", 0, &error));
    BENTO_CHECK(SendRawDatagram(bogus_socket, server_address, conn_id + 1, 901, "wrong-conn-id"));
    (void)server.RunOnce(5);
    BENTO_CHECK_EQ(datagrams.size(), 2U);
    BENTO_CHECK(server.stats().conn_id_mismatch_frames >= 1);

    // 迁移后的新地址仍然可用。
    const char* final_message = "final";
    BENTO_CHECK(client.SendDatagram(1, final_message, std::strlen(final_message)));
    BENTO_CHECK(bento_test::PumpUntil(
        client, server, [&]() { return datagrams.size() == 3; }, 3000));
    BENTO_CHECK_EQ(datagrams[2], "final");

    std::printf(
        "[INFO] migration: conn_id=%u old_port=%u new_port=%u migrations=%llu "
        "stale=%llu mismatch=%llu\n",
        conn_id, static_cast<unsigned>(old_port), static_cast<unsigned>(client.local_port()),
        static_cast<unsigned long long>(server.stats().peer_migrations),
        static_cast<unsigned long long>(server.stats().stale_peer_frames),
        static_cast<unsigned long long>(server.stats().conn_id_mismatch_frames));

    client.Close();
    server.Close();
    return bento_test::Finish("test_conn_migration");
}
