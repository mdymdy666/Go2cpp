/**
 * @file test_rpc_stream.cpp
 * @brief 流式 RPC 验收测试：推流顺序、超时、取消、背压、多流、断连。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "rpc_test_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp;
using namespace rpctest;

/// @brief 等到服务端观察到的取消次数达到 `expected`（最多 `timeout_ms`）。
bool WaitForCancelled(const UserService& service, std::int64_t expected, int timeout_ms) {
    const double deadline = NowMicros() + static_cast<double>(timeout_ms) * 1000.0;
    while (NowMicros() < deadline) {
        if (service.cancelled_count() >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return service.cancelled_count() >= expected;
}

// ---------------------------------------------------------------------------
// 基本推流：顺序与内容
// ---------------------------------------------------------------------------

void TestStreamBasic(const std::string& address) {
    Announce("stream: 1000 messages in order");
    rpc::Client client(address);
    auto stream = client.OpenStream("User.Watch", WatchRequest{1000, 0});
    RPC_CHECK(stream.valid());

    std::vector<std::int32_t> sequences;
    sequences.reserve(1000);
    ChatMessage message;
    rpc::Status status;
    while (stream.Next(&message, &status, std::chrono::milliseconds(5000))) {
        sequences.push_back(message.seq);
        RPC_CHECK_EQ(message.text, std::string("chunk-") + std::to_string(message.seq));
    }
    RPC_CHECK(status.ok());
    RPC_CHECK_EQ(sequences.size(), static_cast<std::size_t>(1000));
    for (std::size_t index = 0; index < sequences.size(); ++index) {
        if (sequences[index] != static_cast<std::int32_t>(index)) {
            RPC_CHECK(false);
            break;
        }
    }
    RPC_CHECK(stream.done());
    RPC_CHECK(stream.Finish(std::chrono::milliseconds(1000)).ok());
}

// ---------------------------------------------------------------------------
// 客户端接收超时：不破坏流，之后仍可继续读
// ---------------------------------------------------------------------------

void TestStreamReceiveTimeout(const std::string& address) {
    Announce("stream: recv timeout then resume");
    rpc::Client client(address);
    auto stream = client.OpenStream("User.Watch", WatchRequest{3, 50000});
    RPC_CHECK(stream.valid());

    ChatMessage message;
    rpc::Status status;
    RPC_CHECK(stream.Next(&message, &status, std::chrono::milliseconds(1000)));
    RPC_CHECK_EQ(message.seq, static_cast<std::int32_t>(0));

    // 服务端每条之间睡 50ms；10ms 超时必须返回 DEADLINE_EXCEEDED。
    RPC_CHECK(!stream.Next(&message, &status, std::chrono::milliseconds(10)));
    RPC_CHECK(status.code == rpc::StatusCode::kDeadlineExceeded);
    RPC_CHECK(!stream.done());

    // 继续读：剩余两条仍然收得到。
    RPC_CHECK(stream.Next(&message, &status, std::chrono::milliseconds(2000)));
    RPC_CHECK_EQ(message.seq, static_cast<std::int32_t>(1));
    RPC_CHECK(stream.Next(&message, &status, std::chrono::milliseconds(2000)));
    RPC_CHECK_EQ(message.seq, static_cast<std::int32_t>(2));
    RPC_CHECK(!stream.Next(&message, &status, std::chrono::milliseconds(2000)));
    RPC_CHECK(status.ok());
}

// ---------------------------------------------------------------------------
// 取消：客户端立即结束，服务端观察到取消并提前返回
// ---------------------------------------------------------------------------

void TestStreamCancel(const std::string& address, UserService& service) {
    Announce("stream: cancel propagated to server");
    const std::int64_t before = service.cancelled_count();
    rpc::Client client(address);
    auto stream = client.OpenStream("User.Watch", WatchRequest{1000000, 500});
    RPC_CHECK(stream.valid());

    ChatMessage message;
    rpc::Status status;
    for (int index = 0; index < 10; ++index) {
        RPC_CHECK(stream.Next(&message, &status, std::chrono::milliseconds(2000)));
    }
    RPC_CHECK(stream.Cancel());
    RPC_CHECK(!stream.Next(&message, &status, std::chrono::milliseconds(1000)));
    RPC_CHECK(status.code == rpc::StatusCode::kCancelled);
    RPC_CHECK(stream.done());

    // 服务端下一次循环检查 `stream->cancelled()` 后返回 kCancelled。
    RPC_CHECK(WaitForCancelled(service, before + 1, 3000));

    // 取消后连接仍可发起新流。
    auto again = client.OpenStream("User.Watch", WatchRequest{2, 0});
    RPC_CHECK(again.valid());
    RPC_CHECK(again.Next(&message, &status, std::chrono::milliseconds(2000)));
    RPC_CHECK(again.Finish(std::chrono::milliseconds(2000)).ok());
}

// ---------------------------------------------------------------------------
// 背压：客户端缓冲超限后流以 RESOURCE_EXHAUSTED 结束
// ---------------------------------------------------------------------------

void TestStreamBackpressure(const std::string& address) {
    Announce("stream: backpressure limit");
    rpc::ClientOptions options;
    options.max_stream_frames = 64;
    options.max_stream_bytes = 64 * 1024;
    rpc::Client client(address, options);
    auto stream = client.OpenStream("User.Watch", WatchRequest{100000, 0});
    RPC_CHECK(stream.valid());

    // 不读，让读线程把 64 帧缓冲填满。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::size_t received = 0;
    ChatMessage message;
    rpc::Status status;
    while (stream.Next(&message, &status, std::chrono::milliseconds(2000))) {
        ++received;
    }
    RPC_CHECK(status.code == rpc::StatusCode::kResourceExhausted);
    RPC_CHECK(received <= 64);
}

// ---------------------------------------------------------------------------
// 多流：同一连接上 4 条流按 call_id 正确分派
// ---------------------------------------------------------------------------

void TestMultipleStreams(const std::string& address) {
    Announce("stream: 4 concurrent streams on one connection");
    rpc::Client client(address);
    std::vector<rpc::ClientStream> streams;
    streams.reserve(4);
    for (int index = 0; index < 4; ++index) {
        streams.push_back(client.OpenStream("User.Watch", WatchRequest{50, 0}));
        RPC_CHECK(streams.back().valid());
    }
    for (std::int32_t round = 0; round < 50; ++round) {
        for (auto& stream : streams) {
            ChatMessage message;
            rpc::Status status;
            RPC_CHECK(stream.Next(&message, &status, std::chrono::milliseconds(5000)));
            RPC_CHECK_EQ(message.seq, round);
        }
    }
    for (auto& stream : streams) {
        ChatMessage message;
        rpc::Status status;
        RPC_CHECK(!stream.Next(&message, &status, std::chrono::milliseconds(1000)));
        RPC_CHECK(status.ok());
    }
}

// ---------------------------------------------------------------------------
// 断连：流被连接关闭唤醒
// ---------------------------------------------------------------------------

void TestStreamDisconnect() {
    Announce("stream: connection close wakes receiver");
    rpc::Server server;
    UserService service;
    std::string address;
    RPC_CHECK(StartTestServer(server, service, &address));

    rpc::Client client(address);
    auto stream = client.OpenStream("User.Watch", WatchRequest{1000000, 1000});
    RPC_CHECK(stream.valid());
    ChatMessage message;
    rpc::Status status;
    RPC_CHECK(stream.Next(&message, &status, std::chrono::milliseconds(2000)));

    server.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    RPC_CHECK(!stream.Next(&message, &status, std::chrono::milliseconds(2000)));
    RPC_CHECK(status.code == rpc::StatusCode::kUnavailable ||
              status.code == rpc::StatusCode::kCancelled);
    RPC_CHECK(stream.Finish(std::chrono::milliseconds(1000)).code ==
              rpc::StatusCode::kUnavailable);
}

}  // namespace

int main() {
    rpc::Server server;
    UserService service;
    std::string address;
    if (!StartTestServer(server, service, &address)) {
        std::printf("[rpc] 服务端启动失败\n");
        return 1;
    }
    std::printf("[rpc] stream server listening on %s\n", address.c_str());

    TestStreamBasic(address);
    TestStreamReceiveTimeout(address);
    TestStreamCancel(address, service);
    TestStreamBackpressure(address);
    TestMultipleStreams(address);
    TestStreamDisconnect();
    server.Stop();
    return Summary("stream");
}
