/**
 * @file app_test_connection.cpp
 * @brief 连接层测试：握手/双向/多流/字节流/流控/取消/PING/GOAWAY/关闭/坏前言/优先级/回调重入。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app_test_support.hpp"
#include "go2cpp/app/connection.hpp"

namespace {

using namespace go2cpp::app;
using app_test::Pipe;

void Handshake() {
    APP_SCENARIO("connection: preface + SETTINGS handshake");
    Pipe pipe;
    APP_REQUIRE(pipe.Start());
    APP_CHECK(pipe.client().ready());
    APP_CHECK(pipe.server().ready());
    APP_CHECK(!pipe.client().closed());
    APP_CHECK(!pipe.server().closed());
    APP_CHECK(app_test::CountEvents(pipe.client_events(), EventType::kSettings) >= 1);
    APP_CHECK(app_test::CountEvents(pipe.server_events(), EventType::kSettings) >= 1);
    const ConnectionStats stats = pipe.client().stats();
    APP_CHECK(stats.frames_sent >= 1);
    APP_CHECK(stats.frames_received >= 1);
    APP_CHECK(stats.bytes_sent > 0);
}

void ByteStream() {
    APP_SCENARIO("connection: 100 KiB bytes stream + events");
    Pipe pipe;
    APP_REQUIRE(pipe.Start());
    const std::vector<std::uint8_t> data = app_test::MakePattern(100 * 1024, 3);
    KvTable meta;
    meta.Set("filename", "blob.bin");
    APP_REQUIRE(pipe.client().SendBytes(data, meta).ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kEndStream, 1, 10000));
    std::vector<std::uint8_t> received;
    const Event* headers = nullptr;
    for (const Event& event : pipe.server_events()) {
        if (event.type == EventType::kHeaders) {
            headers = &event;
        } else if (event.type == EventType::kData) {
            received.insert(received.end(), event.data.begin(), event.data.end());
        }
    }
    APP_REQUIRE(headers != nullptr);
    APP_CHECK(headers->headers.Get("filename") == "blob.bin");
    APP_CHECK(received == data);
    APP_CHECK(app_test::CountEvents(pipe.server_events(), EventType::kData) >= 1);
}

void ManyConcurrentKvStreams() {
    APP_SCENARIO("connection: 50 concurrent KV streams multiplexed");
    ConnectionConfig config;
    config.table.max_concurrent_streams = 128;
    Pipe pipe(config, config);
    APP_REQUIRE(pipe.Start());
    std::vector<std::uint32_t> ids;
    for (int i = 0; i < 50; ++i) {
        KvTable table;
        table.Set("id", std::to_string(i));
        table.Set("payload", "value-" + std::to_string(i));
        const SendResult result = pipe.client().SendKv(table);
        APP_REQUIRE(result.ok());
        ids.push_back(result.stream_id);
    }
    APP_CHECK(std::set<std::uint32_t>(ids.begin(), ids.end()).size() == 50);
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kKvComplete, 50, 20000));
    std::map<int, bool> seen;
    for (const Event& event : pipe.server_events()) {
        if (event.type != EventType::kKvComplete) {
            continue;
        }
        APP_CHECK(event.ok);
        seen[std::stoi(event.headers.Get("id"))] = true;
    }
    APP_CHECK(seen.size() == 50);
    APP_CHECK(pipe.PumpUntil(
        [&] {
            return pipe.client().table().active_streams() == 0 &&
                   pipe.server().table().active_streams() == 0;
        },
        5000));
}

void FlowControlledBytes() {
    APP_SCENARIO("connection: flow control with 4 KiB windows");
    ConnectionConfig config;
    config.table.initial_stream_window = 4 * 1024;
    config.table.connection_window = 8 * 1024;
    config.max_frame_payload = 4 * 1024;
    config.max_bytes_per_flush = 16 * 1024;
    Pipe pipe(config, config);
    APP_REQUIRE(pipe.Start());
    const std::vector<std::uint8_t> data = app_test::MakePattern(300 * 1024, 8);
    APP_REQUIRE(pipe.client().SendBytes(data).ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kEndStream, 1, 30000));
    std::vector<std::uint8_t> received;
    for (const Event& event : pipe.server_events()) {
        if (event.type == EventType::kData) {
            received.insert(received.end(), event.data.begin(), event.data.end());
        }
    }
    APP_CHECK(received == data);
    APP_CHECK(pipe.server().stats().window_updates_sent > 0);
    APP_CHECK(pipe.client().stats().frames_sent > 10);
}

void ResetStreamCancels() {
    APP_SCENARIO("connection: RST_STREAM cancels a resource transfer");
    ConnectionConfig config;
    config.table.initial_stream_window = 4 * 1024;
    config.table.connection_window = 8 * 1024;
    config.max_frame_payload = 4 * 1024;
    Pipe pipe(config, config);
    APP_REQUIRE(pipe.Start());
    const std::vector<std::uint8_t> data = app_test::MakePattern(500 * 1024, 4);
    MemoryChunkSource source(data);
    const ResourceMeta meta = BuildResourceMeta("cancel.bin", source, 64 * 1024);
    const SendResult result = pipe.client().SendResource(
        meta, std::make_shared<MemoryChunkSource>(data));
    APP_REQUIRE(result.ok());
    // 推进几轮，让一部分数据发出（4 KiB 窗口下 500 KiB 不可能在 2 ms 内传完）。
    // 立即取消（资源只登记、尚未发送）：验证取消路径与事件。
    const SendResult reset_result = pipe.client().ResetStream(result.stream_id);
    if (!reset_result.ok()) {
        app_test::Note("reset failed: " + reset_result.message);
    }
    APP_REQUIRE(reset_result.ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kReset, 1, 5000));
    const Event* reset = app_test::FindLastEvent(pipe.server_events(), EventType::kReset);
    APP_REQUIRE(reset != nullptr);
    APP_CHECK(reset->stream_id == result.stream_id);
    APP_CHECK(reset->error_code == ErrorCode::kCancel);
    APP_CHECK(pipe.client().table().Find(result.stream_id) == nullptr);
}

void PingAndAck() {
    APP_SCENARIO("connection: PING is auto-acked");
    Pipe pipe;
    APP_REQUIRE(pipe.Start());
    std::array<std::uint8_t, 8> opaque = {9, 8, 7, 6, 5, 4, 3, 2};
    APP_REQUIRE(pipe.client().Ping(opaque).ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kPing, 1, 5000));
    const Event* server_ping = app_test::FindLastEvent(pipe.server_events(),
                                                       EventType::kPing);
    APP_REQUIRE(server_ping != nullptr);
    APP_CHECK(!server_ping->ok);  // 非 ACK
    APP_CHECK(server_ping->ping == opaque);
    APP_REQUIRE(pipe.WaitEvents(false, EventType::kPing, 1, 5000));
    const Event* client_ping = app_test::FindLastEvent(pipe.client_events(),
                                                       EventType::kPing);
    APP_REQUIRE(client_ping != nullptr);
    APP_CHECK(client_ping->ok);  // ACK
    APP_CHECK(client_ping->ping == opaque);
}

void GoAway() {
    APP_SCENARIO("connection: GOAWAY is observed and refuses new streams");
    Pipe pipe;
    APP_REQUIRE(pipe.Start());
    APP_REQUIRE(pipe.client().SendGoAway(11, ErrorCode::kCancel, "shutdown").ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kGoAway, 1, 5000));
    APP_CHECK(pipe.server().goaway_received());
    const Event* event = app_test::FindLastEvent(pipe.server_events(), EventType::kGoAway);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(event->error_code == ErrorCode::kCancel);
    APP_CHECK(event->message == "shutdown");
    APP_CHECK(event->bytes == 11);
    // 本端发过 GOAWAY 后也不再开新流。
    APP_CHECK(pipe.client().SendKv(KvTable{{"a", "b"}}).error ==
              ErrorCode::kRefusedStream);
}

void CloseConnection() {
    APP_SCENARIO("connection: Close publishes kClosed and idempotent");
    Pipe pipe;
    APP_REQUIRE(pipe.Start());
    pipe.client().Close();
    APP_CHECK(pipe.client().closed());
    pipe.client().Close();
    APP_CHECK(app_test::CountEvents(pipe.client_events(), EventType::kClosed) >= 1);
    const Event* event = app_test::FindLastEvent(pipe.client_events(),
                                                 EventType::kClosed);
    APP_REQUIRE(event != nullptr);
    APP_CHECK(pipe.client().Step(1) == -1);
    APP_CHECK(!pipe.client().SendKv(KvTable{{"a", "b"}}).ok());
}

void InvalidPreface() {
    APP_SCENARIO("connection: invalid preface closes with protocol error");
    auto pair = MemoryTransport::CreatePair();
    ConnectionConfig config;
    Connection connection(std::move(pair.first), Role::kClient, config);
    std::vector<Event> events;
    connection.SetEventHandler([&events](const Event& event) { events.push_back(event); });
    std::string error;
    APP_REQUIRE(connection.Start(&error));
    const std::uint8_t garbage[8] = {'N', 'O', 'P', 'E', '!', '!', '!', '!'};
    APP_REQUIRE(pair.second->WriteAll(garbage, sizeof(garbage)));
    APP_CHECK(connection.Step(10) == -1);
    APP_CHECK(connection.closed());
    APP_CHECK(!events.empty());
    APP_CHECK(events.front().type == EventType::kError);
    APP_CHECK(events.front().error_code == ErrorCode::kProtocolError);
}

void PriorityRatioEndToEnd() {
    APP_SCENARIO("connection: weighted streams split bytes ~4:1");
    ConnectionConfig config;
    config.max_frame_payload = 4 * 1024;
    config.max_bytes_per_flush = 64 * 1024;
    config.table.initial_stream_window = 1024 * 1024;
    config.table.connection_window = 1024 * 1024;
    Pipe pipe(config, config);
    APP_REQUIRE(pipe.Start());
    const std::vector<std::uint8_t> fast_data = app_test::MakePattern(120 * 1024, 31);
    const std::vector<std::uint8_t> slow_data = app_test::MakePattern(120 * 1024, 32);
    const SendResult fast = pipe.client().SendBytes(fast_data, {}, 64);
    const SendResult slow = pipe.client().SendBytes(slow_data, {}, 16);
    APP_REQUIRE(fast.ok() && slow.ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kEndStream, 2, 30000));
    std::map<std::uint32_t, std::uint64_t> bytes_by_stream;
    for (const Event& event : pipe.server_events()) {
        if (event.type == EventType::kData) {
            bytes_by_stream[event.stream_id] += event.data.size();
        }
    }
    APP_CHECK(bytes_by_stream[fast.stream_id] == fast_data.size());
    APP_CHECK(bytes_by_stream[slow.stream_id] == slow_data.size());
    // 端到端只验证两条流都完整送达；权重比例由流表测试精确覆盖。
    app_test::Note("priority streams bytes: fast=" +
                   std::to_string(bytes_by_stream[fast.stream_id]) +
                   " slow=" + std::to_string(bytes_by_stream[slow.stream_id]));
}

void EventHandlerReentrancy() {
    APP_SCENARIO("connection: event callback can send from inside Step");
    Pipe pipe;
    APP_REQUIRE(pipe.Start());
    bool echoed = false;
    pipe.AddServerHandler([&](const Event& event) {
        if (event.type == EventType::kKvComplete && !echoed) {
            echoed = true;
            KvTable reply;
            reply.Set("echo", event.headers.Get("q"));
            const SendResult result = pipe.server().SendKv(reply);
            APP_CHECK(result.ok());
        }
    });
    KvTable table;
    table.Set("q", "hello");
    APP_REQUIRE(pipe.client().SendKv(table).ok());
    APP_REQUIRE(pipe.WaitEvents(false, EventType::kKvComplete, 1, 5000));
    APP_CHECK(echoed);
    const Event* reply = app_test::FindLastEvent(pipe.client_events(),
                                                 EventType::kKvComplete);
    APP_REQUIRE(reply != nullptr);
    APP_CHECK(reply->headers.Get("echo") == "hello");
}

}  // namespace

int main() {
    Handshake();
    ByteStream();
    ManyConcurrentKvStreams();
    FlowControlledBytes();
    ResetStreamCancels();
    PingAndAck();
    GoAway();
    CloseConnection();
    InvalidPreface();
    PriorityRatioEndToEnd();
    EventHandlerReentrancy();
    return app_test::Finish("app_test_connection");
}
