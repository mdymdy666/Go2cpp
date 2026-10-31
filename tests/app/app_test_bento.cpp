/**
 * @file app_test_bento.cpp
 * @brief Bento 对接测试：真实 UDP Link（STREAM 可靠通道）承载应用层流表。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app_test_support.hpp"
#include "go2cpp/app/bento_bridge.hpp"
#include "go2cpp/app/bento_link_transport.hpp"
#include "go2cpp/app/connection.hpp"
#include "go2cpp/bento/net/frame.hpp"
#include "go2cpp/bento/net/link.hpp"

namespace {

using namespace go2cpp::app;

/// 一对通过真实 UDP Link 相连的 Connection。
struct BentoSession {
    std::unique_ptr<go2cpp::bento::Link> server;
    std::unique_ptr<go2cpp::bento::Link> client;
    std::unique_ptr<BentoLinkTransport> server_transport;
    std::unique_ptr<BentoLinkTransport> client_transport;
    std::unique_ptr<Connection> server_conn;
    std::unique_ptr<Connection> client_conn;
    std::vector<Event> server_events;
    std::vector<Event> client_events;
    std::atomic<bool> stop{false};
    std::thread rx_thread;

    ~BentoSession() { Stop(); }

    static go2cpp::bento::LinkConfig BaseConfig() {
        go2cpp::bento::LinkConfig config;
        config.bind_address = "127.0.0.1";
        config.bind_port = 0;
        config.require_encryption = false;
        config.peer_timeout_ms = 10000;
        config.heartbeat_interval_ms = 200;
        config.hello_max_attempts = 32;
        return config;
    }

    bool Start(const ConnectionConfig& client_config = {},
               const ConnectionConfig& server_config = {}) {
        std::string error;
        server = std::make_unique<go2cpp::bento::Link>(BaseConfig());
        if (!server->Open(&error)) {
            app_test::Note("server Link.Open failed: " + error);
            return false;
        }
        go2cpp::bento::LinkConfig client_cfg = BaseConfig();
        client_cfg.remote = "127.0.0.1";
        client_cfg.peer_port = server->local_port();
        client = std::make_unique<go2cpp::bento::Link>(client_cfg);
        server_transport = std::make_unique<BentoLinkTransport>(server.get());
        client_transport = std::make_unique<BentoLinkTransport>(client.get());
        if (!server_transport->Install() || !client_transport->Install()) {
            return false;
        }
        rx_thread = std::thread([this] {
            while (!stop.load()) {
                server->RunOnce(1);
                client->RunOnce(1);
            }
        });
        if (!client->Open(&error)) {
            app_test::Note("client Link.Open failed: " + error);
            return false;
        }
        client->Connect();
        const std::uint64_t deadline = app_test::NowMs() + 5000;
        while (!client->connected() && app_test::NowMs() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!client->connected()) {
            app_test::Note("Bento Link handshake timed out");
            return false;
        }
        server_conn = std::make_unique<Connection>(std::move(server_transport),
                                                   Role::kServer, server_config);
        client_conn = std::make_unique<Connection>(std::move(client_transport),
                                                   Role::kClient, client_config);
        server_conn->SetEventHandler(
            [this](const Event& event) { server_events.push_back(event); });
        client_conn->SetEventHandler(
            [this](const Event& event) { client_events.push_back(event); });
        return client_conn->Start(&error) && server_conn->Start(&error);
    }

    bool PumpUntil(const std::function<bool()>& done, std::uint32_t timeout_ms) {
        const std::uint64_t deadline = app_test::NowMs() + timeout_ms;
        while (!done()) {
            if (app_test::NowMs() > deadline) {
                return done();
            }
            client_conn->Step(0);
            server_conn->Step(0);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return true;
    }

    void Stop() {
        if (client_conn) {
            client_conn->Close();
        }
        if (server_conn) {
            server_conn->Close();
        }
        stop.store(true);
        if (rx_thread.joinable()) {
            rx_thread.join();
        }
        if (client) {
            client->Close();
        }
        if (server) {
            server->Close();
        }
    }
};

void BentoKvOverLink() {
    APP_SCENARIO("bento: KV over real UDP Link (reliable STREAM)");
    BentoSession session;
    APP_REQUIRE(session.Start());
    APP_REQUIRE(session.PumpUntil(
        [&] { return session.client_conn->ready() && session.server_conn->ready(); },
        10000));
    KvTable table;
    table.Set("k1", "v1");
    table.Set("k2", "v2");
    table.Set("k3", "v3");
    table.Set("k4", "v4");
    table.Set("k5", "v5");
    APP_REQUIRE(session.client_conn->SendKv(table).ok());
    APP_REQUIRE(session.PumpUntil(
        [&] {
            return app_test::CountEvents(session.server_events,
                                         EventType::kKvComplete) >= 1;
        },
        20000));
    const Event* event = app_test::FindLastEvent(session.server_events,
                                                 EventType::kKvComplete);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->ok);
    APP_CHECK(event->headers == table);
}

void BentoResourceOverLink() {
    APP_SCENARIO("bento: 1 MiB resource over real UDP Link");
    BentoSession session;
    APP_REQUIRE(session.Start());
    APP_REQUIRE(session.PumpUntil(
        [&] { return session.client_conn->ready() && session.server_conn->ready(); },
        10000));
    const std::vector<std::uint8_t> data = app_test::MakePattern(1024 * 1024, 77);
    MemoryChunkSource source(data);
    const ResourceMeta meta = BuildResourceMeta("bento.bin", source, 64 * 1024);
    APP_REQUIRE(session.client_conn
                    ->SendResource(meta, std::make_shared<MemoryChunkSource>(data))
                    .ok());
    APP_REQUIRE(session.PumpUntil(
        [&] {
            return app_test::CountEvents(session.server_events,
                                         EventType::kResourceComplete) >= 1;
        },
        60000));
    const Event* event = app_test::FindLastEvent(session.server_events,
                                                 EventType::kResourceComplete);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->ok);
    APP_CHECK(event->resource_stats.verified);
    APP_CHECK(event->resource_stats.sha256_hex == meta.sha256_hex);
    app_test::Note("bento link resource throughput: " +
                   std::to_string(event->resource_stats.throughput_mbps) + " MiB/s");
}

void BentoBidirectional() {
    APP_SCENARIO("bento: server -> client KV over the same Link");
    BentoSession session;
    APP_REQUIRE(session.Start());
    APP_REQUIRE(session.PumpUntil(
        [&] { return session.client_conn->ready() && session.server_conn->ready(); },
        10000));
    KvTable reply;
    reply.Set("from", "server");
    APP_REQUIRE(session.server_conn->SendKv(reply).ok());
    APP_REQUIRE(session.PumpUntil(
        [&] {
            return app_test::CountEvents(session.client_events,
                                         EventType::kKvComplete) >= 1;
        },
        20000));
    const Event* event = app_test::FindLastEvent(session.client_events,
                                                 EventType::kKvComplete);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->headers.Get("from") == "server");
}

void BentoFrameBridgeOverUdp() {
    APP_SCENARIO("bento: app frame -> Bento 20B frame -> decode");
    Frame frame;
    frame.type = FrameType::kHeaders;
    frame.stream_id = 1;
    frame.payload = app_test::MakePattern(64, 1);
    const std::vector<std::uint8_t> bento_bytes = go2cpp::app::bento_bridge::Encode(frame);
    APP_REQUIRE(!bento_bytes.empty());
    go2cpp::bento::net::Frame decoded_bento;
    APP_CHECK(go2cpp::bento::net::Decode(bento_bytes.data(), bento_bytes.size(),
                                         &decoded_bento) ==
              go2cpp::bento::net::FrameStatus::kOk);
    Frame decoded;
    APP_CHECK(go2cpp::app::bento_bridge::Decode(bento_bytes.data(), bento_bytes.size(),
                                                &decoded) ==
              go2cpp::app::bento_bridge::Status::kOk);
    APP_CHECK(decoded.payload == frame.payload);
}

}  // namespace

int main() {
    BentoKvOverLink();
    BentoResourceOverLink();
    BentoBidirectional();
    BentoFrameBridgeOverUdp();
    return app_test::Finish("app_test_bento");
}
