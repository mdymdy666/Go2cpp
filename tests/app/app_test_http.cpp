/**
 * @file app_test_http.cpp
 * @brief HttpServer（真实 TCP）承载应用层帧流：KV/资源/索引/坏 body/大 body。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_test_support.hpp"
#include "go2cpp/app/connection.hpp"
#include "go2cpp/app/http_tunnel.hpp"
#include "go2cpp/app/resource.hpp"
#include "go2cpp/bento/net/http_server.hpp"
#include "go2cpp/network/http.hpp"

namespace {

using namespace go2cpp::app;

struct EventCollector {
    std::mutex mutex;
    std::vector<Event> events;

    void Add(const Event& event) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(event);
    }

    std::size_t Count(EventType type) {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t count = 0;
        for (const Event& event : events) {
            if (event.type == type) {
                ++count;
            }
        }
        return count;
    }

    std::vector<Event> Snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return events;
    }
};

struct Harness {
    go2cpp::bento::HttpServer server;
    HttpTunnelServer tunnel;
    EventCollector collector;
    std::string url;
    std::unique_ptr<HttpTunnelClientTransport> client_transport;
    std::unique_ptr<Connection> client;

    explicit Harness(ConnectionConfig server_config = {})
        : tunnel(std::move(server_config)) {}

    bool Start(ConnectionConfig client_config = {}) {
        tunnel.SetEventHandler(
            [this](const Event& event) { collector.Add(event); });
        server.OnRequest([this](const go2cpp::http::Request& request) {
            const std::vector<std::uint8_t> body(request.body.begin(),
                                                 request.body.end());
            const std::vector<std::uint8_t> response = tunnel.HandleRequest(body);
            go2cpp::http::Response result;
            result.status = go2cpp::http::Status::OK;
            result.header.set("content-type", "application/octet-stream");
            result.body.assign(response.begin(), response.end());
            return result;
        });
        const auto listened = server.Listen("127.0.0.1", 0);
        if (!listened.ok()) {
            app_test::Note("HttpServer Listen failed: " + listened.message);
            return false;
        }
        const auto started = server.Start();
        if (!started.ok()) {
            app_test::Note("HttpServer Start failed: " + started.message);
            return false;
        }
        url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/app";
        std::string error;
        client_transport = std::make_unique<HttpTunnelClientTransport>(
            [this](const std::vector<std::uint8_t>& request,
                   std::vector<std::uint8_t>* response) {
                const std::string body(request.begin(), request.end());
                const go2cpp::http::Result result = go2cpp::http::Post(url, body);
                if (!result.ok()) {
                    return false;
                }
                response->assign(result.response->body.begin(),
                                 result.response->body.end());
                return true;
            });
        client = std::make_unique<Connection>(
            std::unique_ptr<ITransport>(client_transport.release()), Role::kClient,
            client_config);
        return client->Start(&error);
    }

    bool PumpUntil(const std::function<bool()>& done, std::uint32_t timeout_ms) {
        const std::uint64_t deadline = app_test::NowMs() + timeout_ms;
        while (!done()) {
            if (app_test::NowMs() > deadline) {
                return done();
            }
            client->Step(0);
            if (client->closed()) {
                return done();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return true;
    }

    ~Harness() {
        if (client) {
            client->Close();
        }
        server.Stop();
    }
};

void KvOverHttp() {
    APP_SCENARIO("http tunnel: KV over real TCP HttpServer");
    Harness harness;
    APP_REQUIRE(harness.Start());
    KvTable table;
    table.Set("k1", "v1");
    table.Set("k2", "v2");
    table.Set("k3", "v3");
    table.Set("k4", "v4");
    table.Set("k5", "v5");
    APP_REQUIRE(harness.client->SendKv(table).ok());
    APP_REQUIRE(harness.PumpUntil(
        [&] { return harness.collector.Count(EventType::kKvComplete) >= 1; }, 10000));
    const std::vector<Event> events = harness.collector.Snapshot();
    const Event* found = app_test::FindLastEvent(events, EventType::kKvComplete);
    APP_REQUIRE(found != nullptr);
    APP_CHECK(found->ok);
    APP_CHECK(found->headers == table);
    APP_CHECK(harness.tunnel.handled_requests() >= 1);
}

void ResourceOverHttp() {
    APP_SCENARIO("http tunnel: 256 KiB resource over real TCP HttpServer");
    Harness harness;
    APP_REQUIRE(harness.Start());
    const std::vector<std::uint8_t> data = app_test::MakePattern(256 * 1024, 41);
    MemoryChunkSource source(data);
    const ResourceMeta meta = BuildResourceMeta("http.bin", source, 64 * 1024);
    APP_REQUIRE(harness.client
                    ->SendResource(meta, std::make_shared<MemoryChunkSource>(data))
                    .ok());
    APP_REQUIRE(harness.PumpUntil(
        [&] {
            return harness.collector.Count(EventType::kResourceComplete) >= 1;
        },
        60000));
    const std::vector<Event> events = harness.collector.Snapshot();
    const Event* found = app_test::FindLastEvent(events, EventType::kResourceComplete);
    APP_REQUIRE(found != nullptr);
    APP_CHECK(found->ok);
    APP_CHECK(found->resource_stats.verified);
    APP_CHECK(found->resource_stats.sha256_hex == meta.sha256_hex);
    app_test::Note("http tunnel requests: " +
                   std::to_string(harness.tunnel.handled_requests()));
}

void IndexOverHttp() {
    APP_SCENARIO("http tunnel: resource index frame");
    Harness harness;
    APP_REQUIRE(harness.Start());
    ResourceIndex index;
    ResourceMeta meta;
    meta.name = "one.bin";
    meta.size = 100;
    meta.chunk_size = 64 * 1024;
    meta.chunk_count = 1;
    index.resources.push_back(meta);
    APP_REQUIRE(harness.client->SendResourceIndex(index).ok());
    APP_REQUIRE(harness.PumpUntil(
        [&] { return harness.collector.Count(EventType::kResourceIndex) >= 1; },
        10000));
    const std::vector<Event> events = harness.collector.Snapshot();
    const Event* found = app_test::FindLastEvent(events, EventType::kResourceIndex);
    APP_REQUIRE(found != nullptr);
    APP_CHECK(found->resource_index.resources.size() == 1);
    APP_CHECK(found->resource_index.resources[0].name == "one.bin");
}

void MalformedBodyIsRejected() {
    APP_SCENARIO("http tunnel: malformed body does not crash the session");
    HttpTunnelServer tunnel;
    std::vector<Event> events;
    tunnel.SetEventHandler([&events](const Event& event) { events.push_back(event); });
    std::vector<std::uint8_t> garbage(64, 0xAB);
    const std::vector<std::uint8_t> response = tunnel.HandleRequest(garbage);
    // 必须返回帧（至少包含服务端 preface/SETTINGS 或错误 GOAWAY），且不崩溃。
    APP_CHECK(!response.empty());
    APP_CHECK(tunnel.handled_requests() == 1);
    APP_CHECK(tunnel.closed() || !events.empty());
}

void LargeKvOverHttp() {
    APP_SCENARIO("http tunnel: 300 KiB KV value over real TCP");
    Harness harness;
    APP_REQUIRE(harness.Start());
    KvTable table;
    table.Set("payload", std::string(300 * 1024, 'h'));
    APP_REQUIRE(harness.client->SendKv(table).ok());
    APP_REQUIRE(harness.PumpUntil(
        [&] { return harness.collector.Count(EventType::kKvComplete) >= 1; }, 30000));
    const std::vector<Event> events = harness.collector.Snapshot();
    const Event* found = app_test::FindLastEvent(events, EventType::kKvComplete);
    APP_REQUIRE(found != nullptr);
    APP_CHECK(found->ok);
    APP_CHECK(found->headers.Get("payload").size() == 300 * 1024);
}

}  // namespace

int main() {
    KvOverHttp();
    ResourceOverHttp();
    IndexOverHttp();
    MalformedBodyIsRejected();
    LargeKvOverHttp();
    return app_test::Finish("app_test_http");
}
