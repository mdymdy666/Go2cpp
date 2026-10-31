/**
 * @file app_test_load.cpp
 * @brief 负载测试：1000 并发流 + 1 GiB 资源传输（正确性 + 吞吐）。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app_test_support.hpp"
#include "go2cpp/app/connection.hpp"

namespace {

using namespace go2cpp::app;
using app_test::Pipe;

void ThousandConcurrentStreams() {
    APP_SCENARIO("load: 1000 concurrent KV streams");
    ConnectionConfig config;
    config.table.max_concurrent_streams = 2048;
    config.table.max_retired_streams = 512;
    config.max_bytes_per_flush = 1024 * 1024;
    Pipe pipe(config, config);
    APP_REQUIRE(pipe.Start());

    constexpr int kStreams = 1000;
    const std::uint64_t start = app_test::NowMs();
    std::vector<std::uint32_t> ids;
    ids.reserve(kStreams);
    for (int i = 0; i < kStreams; ++i) {
        KvTable table;
        table.Set("index", std::to_string(i));
        table.Set("kind", "bulk");
        const SendResult result = pipe.client().SendKv(table);
        APP_REQUIRE(result.ok());
        ids.push_back(result.stream_id);
    }
    APP_CHECK(std::set<std::uint32_t>(ids.begin(), ids.end()).size() == kStreams);
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, kStreams, 180000));
    const std::uint64_t elapsed = std::max<std::uint64_t>(1, app_test::NowMs() - start);
    std::set<int> seen;
    for (const Event& event : pipe.server_events()) {
        if (event.type != EventType::kKvComplete) {
            continue;
        }
        APP_CHECK(event.ok);
        seen.insert(std::stoi(event.headers.Get("index")));
    }
    APP_CHECK(seen.size() == kStreams);
    APP_CHECK(pipe.PumpUntil(
        [&] {
            return pipe.client().table().active_streams() == 0 &&
                   pipe.server().table().active_streams() == 0;
        },
        10000));
    const ConnectionStats client_stats = pipe.client().stats();
    const ConnectionStats server_stats = pipe.server().stats();
    app_test::Note("1000 streams: " + std::to_string(elapsed) + " ms, client frames=" +
                   std::to_string(client_stats.frames_sent) + ", server frames=" +
                   std::to_string(server_stats.frames_received));
    APP_CHECK(client_stats.streams_opened >= kStreams);
    APP_CHECK(server_stats.streams_completed >= kStreams);
    APP_CHECK(elapsed < 60000);
}

void OneGiBResource() {
    APP_SCENARIO("load: 1 GiB resource with SHA-256 verification");
    constexpr std::uint64_t kSize = 1024ull * 1024ull * 1024ull;
    auto source = std::make_shared<PatternChunkSource>(kSize, 0x123456789abcdefULL);
    const std::uint64_t meta_start = app_test::NowMs();
    const ResourceMeta meta = BuildResourceMeta("huge.bin", *source, 512 * 1024);
    const std::uint64_t meta_elapsed =
        std::max<std::uint64_t>(1, app_test::NowMs() - meta_start);
    APP_CHECK(meta.size == kSize);
    APP_CHECK(meta.chunk_count == 2048);
    APP_CHECK(meta.sha256_hex.size() == 64);

    ConnectionConfig config;
    config.max_frame_payload = 256 * 1024;
    config.table.initial_stream_window = 256 * 1024;
    config.table.connection_window = 1024 * 1024;
    config.max_bytes_per_flush = 1024 * 1024;
    Pipe pipe(config, config);
    APP_REQUIRE(pipe.Start());
    const std::uint64_t start = app_test::NowMs();
    const SendResult result = pipe.client().SendResource(meta, source);
    APP_REQUIRE(result.ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kResourceComplete, 1, 600000));
    const std::uint64_t elapsed = std::max<std::uint64_t>(1, app_test::NowMs() - start);
    const Event* event = app_test::FindLastEvent(pipe.server_events(),
                                                 EventType::kResourceComplete);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->ok);
    APP_CHECK(event->resource_stats.verified);
    APP_CHECK(event->resource_stats.bytes == kSize);
    APP_CHECK(event->resource_stats.chunks == meta.chunk_count);
    APP_CHECK(event->resource_stats.sha256_hex == meta.sha256_hex);
    const double mbps = 1024.0 / (static_cast<double>(elapsed) / 1000.0);
    app_test::Note("1 GiB transfer: " + std::to_string(elapsed) + " ms (" +
                   std::to_string(mbps) + " MiB/s), metadata scan " +
                   std::to_string(meta_elapsed) + " ms, sender bytes=" +
                   std::to_string(pipe.client().stats().bytes_sent));
    APP_CHECK(mbps > 10.0);
    // 接收端常驻内存不应随资源线性增长：这里用 Discard sink，收到的事件只带元数据。
    APP_CHECK(event->data.empty());
    APP_CHECK(pipe.server().table().active_streams() == 0);
}

void InterleavedResourcesAndKv() {
    APP_SCENARIO("load: 50 resources + 200 KV streams interleaved");
    ConnectionConfig config;
    config.table.max_concurrent_streams = 512;
    config.max_frame_payload = 64 * 1024;
    config.max_bytes_per_flush = 512 * 1024;
    Pipe pipe(config, config);
    APP_REQUIRE(pipe.Start());
    std::vector<std::vector<std::uint8_t>> blobs;
    std::vector<std::shared_ptr<MemoryChunkSource>> sources;
    std::vector<ResourceMeta> metas;
    for (int i = 0; i < 50; ++i) {
        blobs.push_back(app_test::MakePattern(64 * 1024, static_cast<std::uint32_t>(i + 1)));
        sources.push_back(std::make_shared<MemoryChunkSource>(blobs.back()));
        metas.push_back(BuildResourceMeta("r" + std::to_string(i), *sources.back(),
                                          64 * 1024));
    }
    for (int i = 0; i < 50; ++i) {
        APP_REQUIRE(pipe.client().SendResource(metas[static_cast<std::size_t>(i)],
                                               sources[static_cast<std::size_t>(i)])
                        .ok());
    }
    for (int i = 0; i < 200; ++i) {
        KvTable table;
        table.Set("i", std::to_string(i));
        APP_REQUIRE(pipe.client().SendKv(table).ok());
    }
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kResourceComplete, 50, 60000));
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, 200, 60000));
    std::size_t resources_ok = 0;
    for (const Event& event : pipe.server_events()) {
        if (event.type == EventType::kResourceComplete) {
            APP_CHECK(event.ok);
            ++resources_ok;
        }
    }
    APP_CHECK(resources_ok == 50);
    APP_CHECK(pipe.server().table().active_streams() == 0);
}

}  // namespace

int main() {
    ThousandConcurrentStreams();
    OneGiBResource();
    InterleavedResourcesAndKv();
    return app_test::Finish("app_test_load");
}
