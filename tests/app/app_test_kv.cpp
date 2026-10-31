/**
 * @file app_test_kv.cpp
 * @brief KV 流测试：小表/大表/混合/每键一流/转义/大量键/流控。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "app_test_support.hpp"
#include "go2cpp/app/kv_stream.hpp"

namespace {

using namespace go2cpp::app;
using app_test::Pipe;

KvTable SmallTable() {
    KvTable table;
    table.Set("k1", "v1");
    table.Set("k2", "v2");
    table.Set("k3", "v3");
    table.Set("k4", "v4");
    table.Set("k5", "v5");
    return table;
}

const Event* FindKv(const std::vector<Event>& events, std::size_t index) {
    std::size_t seen = 0;
    for (const Event& event : events) {
        if (event.type != EventType::kKvComplete) {
            continue;
        }
        if (seen == index) {
            return &event;
        }
        ++seen;
    }
    return nullptr;
}

void SmallKv() {
    APP_SCENARIO("kv: 5 small pairs in a single HEADERS frame");
    Pipe pipe;
    const KvTable table = SmallTable();
    APP_REQUIRE(pipe.client().SendKv(table).ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, 1, 5000));
    const Event* event = FindKv(pipe.server_events(), 0);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->ok);
    APP_CHECK(event->headers == table);
    APP_CHECK(event->headers.ToText() == "k1=v1;k2=v2;k3=v3;k4=v4;k5=v5");
}

void TextApi() {
    APP_SCENARIO("kv: SendKvText parses k1=v1;...;k5=v5");
    Pipe pipe;
    const SendResult result = pipe.client().SendKvText("a=1;b=2;c=3;d=4;e=5");
    APP_REQUIRE(result.ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, 1, 5000));
    const Event* event = FindKv(pipe.server_events(), 0);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->headers.size() == 5);
    APP_CHECK(event->headers.Get("c") == "3");
    APP_CHECK(!pipe.client().SendKvText("=bad").ok());
}

void LargeKv() {
    APP_SCENARIO("kv: 1 MiB value uses streamed DATA frames");
    Pipe pipe;
    KvTable table;
    table.Set("name", "large");
    table.Set("payload", std::string(1024 * 1024, 'x'));
    table.Set("tail", "done");
    APP_REQUIRE(pipe.client().SendKv(table).ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, 1, 20000));
    const Event* event = FindKv(pipe.server_events(), 0);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->ok);
    APP_CHECK(event->headers.size() == 3);
    APP_CHECK(event->headers.Get("name") == "large");
    APP_CHECK(event->headers.Get("payload") == std::string(1024 * 1024, 'x'));
    APP_CHECK(event->headers.Get("tail") == "done");
}

void MixedKv() {
    APP_SCENARIO("kv: mixed small values + 100 KiB value");
    Pipe pipe;
    KvTable table;
    table.Set("small1", "a");
    table.Set("small2", "b");
    table.Set("big", std::string(100 * 1024, 'Z'));
    table.Set("small3", "c");
    APP_REQUIRE(pipe.client().SendKv(table).ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, 1, 10000));
    const Event* event = FindKv(pipe.server_events(), 0);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->ok);
    APP_CHECK(event->headers.Get("small1") == "a");
    APP_CHECK(event->headers.Get("small3") == "c");
    APP_CHECK(event->headers.Get("big").size() == 100 * 1024);
    APP_CHECK(event->headers.Get("big").find_first_not_of('Z') == std::string::npos);
}

void PerKeyStreams() {
    APP_SCENARIO("kv: one stream per key (5 streams) + batch collector");
    Pipe pipe;
    const KvTable table = SmallTable();
    std::vector<std::uint32_t> ids;
    APP_REQUIRE(pipe.client().SendKvPerKey(table, &ids).ok());
    APP_REQUIRE(ids.size() == 5);
    APP_CHECK(std::set<std::uint32_t>(ids.begin(), ids.end()).size() == 5);
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, 5, 5000));
    KvBatchCollector collector(5);
    for (const Event& event : pipe.server_events()) {
        if (event.type == EventType::kKvComplete) {
            APP_CHECK(event.ok);
            APP_CHECK(event.stream_id != 0);
            collector.Add(event.stream_id, event.headers);
        }
    }
    APP_CHECK(collector.complete());
    const KvTable merged = collector.Merge();
    APP_CHECK(merged == table);
}

void EscapingAndBinary() {
    APP_SCENARIO("kv: escaping / empty values / binary values");
    Pipe pipe;
    KvTable table;
    table.Set("a;b", "c=d\\e");
    table.Set("empty", "");
    table.Set("中文键", "中文值");
    table.Set("bin", std::string("\x00\xff\x01", 3));
    APP_REQUIRE(pipe.client().SendKv(table).ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, 1, 5000));
    const Event* event = FindKv(pipe.server_events(), 0);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->ok);
    APP_CHECK(event->headers == table);
}

void ManyKeysWithSmallFrames() {
    APP_SCENARIO("kv: 500 keys exceed small-frame threshold -> partitioned");
    ConnectionConfig config;
    config.max_frame_payload = 4 * 1024;
    config.table.max_frame_payload = 4 * 1024;
    config.small_kv_threshold = 8 * 1024;
    config.max_bytes_per_flush = 128 * 1024;
    Pipe pipe(config, config);
    KvTable table;
    for (int i = 0; i < 500; ++i) {
        table.Set("key" + std::to_string(i), "value" + std::to_string(i));
    }
    APP_REQUIRE(pipe.client().SendKv(table).ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, 1, 15000));
    const Event* event = FindKv(pipe.server_events(), 0);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->ok);
    APP_CHECK(event->headers.size() == 500);
    APP_CHECK(event->headers.Get("key0") == "value0");
    APP_CHECK(event->headers.Get("key499") == "value499");
}

void FlowControlledKv() {
    APP_SCENARIO("kv: 200 KiB value over 4 KiB stream window");
    ConnectionConfig config;
    config.table.initial_stream_window = 4 * 1024;
    config.table.connection_window = 16 * 1024;
    config.max_frame_payload = 4 * 1024;
    config.max_bytes_per_flush = 32 * 1024;
    Pipe pipe(config, config);
    KvTable table;
    table.Set("payload", std::string(200 * 1024, 'q'));
    APP_REQUIRE(pipe.client().SendKv(table).ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, 1, 20000));
    const Event* event = FindKv(pipe.server_events(), 0);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->ok);
    APP_CHECK(event->headers.Get("payload").size() == 200 * 1024);
    const auto stats = pipe.client().stats();
    APP_CHECK(stats.window_updates_sent == 0);  // 发送端只收 WINDOW_UPDATE
    APP_CHECK(pipe.client().table().total_bytes_sent() > 200 * 1024);
}

void ProducerAssemblerUnit() {
    APP_SCENARIO("kv: producer/assembler unit round trip");
    KvTable table = SmallTable();
    table.Set("big", std::string(200 * 1024, 'm'));
    KvProducer producer(table, 4096, 4096);
    std::vector<Frame> frames;
    Frame frame;
    while (producer.Produce(16 * 1024, &frame)) {
        frames.push_back(frame);
    }
    APP_REQUIRE(frames.size() >= 3);
    APP_CHECK(frames.front().type == FrameType::kHeaders);
    APP_CHECK(frames.back().EndStream());
    KvTable header_table;
    std::string error;
    APP_REQUIRE(KvTable::ParseBinary(frames.front().payload.data(),
                                     frames.front().payload.size(), &header_table,
                                     &error));
    APP_CHECK(header_table.Get(kKvModeKey) == "stream");
    APP_CHECK(header_table.Get(kKvPartialKey) == "1");
    KvAssembler assembler;
    APP_REQUIRE(assembler.OnHeaders(header_table, &error));
    for (std::size_t i = 1; i < frames.size(); ++i) {
        APP_REQUIRE(assembler.OnData(frames[i].payload.data(), frames[i].payload.size(),
                                     &error));
    }
    KvTable merged;
    APP_REQUIRE(assembler.Finish(&merged, &error));
    APP_CHECK(merged == table);
    APP_CHECK(merged.Get("big").size() == 200 * 1024);

    // perkey 模式：单键也标记为 perkey。
    KvTable one;
    one.Set("only", "value");
    KvProducer perkey(one, 4096, 4096, KvMode::kPerKey);
    Frame perkey_frame;
    APP_REQUIRE(perkey.Produce(16 * 1024, &perkey_frame));
    APP_CHECK(perkey.mode() == KvMode::kPerKey);
    KvTable perkey_headers;
    APP_REQUIRE(KvTable::ParseBinary(perkey_frame.payload.data(),
                                     perkey_frame.payload.size(), &perkey_headers,
                                     &error));
    APP_CHECK(perkey_headers.Get(kKvModeKey) == "perkey");
    APP_CHECK(perkey_frame.EndStream());
}

void BidirectionalKv() {
    APP_SCENARIO("kv: client and server both send KV concurrently");
    Pipe pipe;
    KvTable from_client;
    from_client.Set("dir", "client->server");
    from_client.Set("n", "1");
    KvTable from_server;
    from_server.Set("dir", "server->client");
    from_server.Set("n", "2");
    APP_REQUIRE(pipe.client().SendKv(from_client).ok());
    APP_REQUIRE(pipe.server().SendKv(from_server).ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, 1, 5000));
    APP_REQUIRE(pipe.WaitEvents(false, EventType::kKvComplete, 1, 5000));
    const Event* server_event = FindKv(pipe.server_events(), 0);
    const Event* client_event = FindKv(pipe.client_events(), 0);
    APP_REQUIRE(server_event != nullptr && client_event != nullptr);
    APP_CHECK(server_event->headers == from_client);
    APP_CHECK(client_event->headers == from_server);
}

}  // namespace

int main() {
    SmallKv();
    TextApi();
    LargeKv();
    MixedKv();
    PerKeyStreams();
    EscapingAndBinary();
    ManyKeysWithSmallFrames();
    FlowControlledKv();
    ProducerAssemblerUnit();
    BidirectionalKv();
    return app_test::Finish("app_test_kv");
}
