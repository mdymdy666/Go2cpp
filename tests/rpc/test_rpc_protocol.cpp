/**
 * @file test_rpc_protocol.cpp
 * @brief 协议层/注册表/方法绑定的验收测试（无网络）。
 *
 * @details 覆盖：
 * 1. 注册表：注册/查找/重名/非法名/冻结/枚举；
 * 2. 方法签名矩阵：值返回、out 参数、仅状态、void、多参数、流式标志；
 * 3. 协议往返：请求/响应/流数据/流结束/取消/心跳；
 * 4. 畸形载荷：截断、尾随、坏状态码、超长方法名全部拒绝；
 * 5. 随机 fuzz：10 万组随机字节不崩溃；
 * 6. 压测：1e6 次请求编码 → 解码，输出 ns/op。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "rpc_test_support.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "go2cpp/rpc/protocol.hpp"
#include "go2cpp/rpc/registry.hpp"
#include "go2cpp/rpc/status.hpp"

namespace {

using namespace go2cpp;
using namespace rpctest;

/// @brief 造一个只含 payload 的帧。
frame::Frame MakeFrame(rpc::FrameType type, std::vector<std::uint8_t> payload,
                       std::uint16_t flags = 0) {
    frame::Frame result;
    result.type = static_cast<std::uint8_t>(type);
    result.flags = flags;
    result.payload = std::move(payload);
    return result;
}

// ---------------------------------------------------------------------------
// 注册表
// ---------------------------------------------------------------------------

void TestRegistry() {
    Announce("registry: register/find/freeze");
    rpc::ServiceRegistry registry;
    UserService service;

    RPC_CHECK(registry.AddService("User", &service));
    RPC_CHECK_EQ(registry.size(), static_cast<std::size_t>(12));
    RPC_CHECK(!registry.AddService("User", &service));  // 重名拒绝

    const rpc::BoundMethod* get = registry.Find("User.Get");
    RPC_CHECK(get != nullptr);
    RPC_CHECK(get->service == &service);
    RPC_CHECK_EQ(get->binding.argc, static_cast<std::uint8_t>(1));
    RPC_CHECK(!get->binding.streaming);
    RPC_CHECK(registry.Find("User.Missing") == nullptr);
    RPC_CHECK(registry.Find("Missing.Get") == nullptr);
    RPC_CHECK(registry.Find("") == nullptr);

    const rpc::BoundMethod* watch = registry.Find("User.Watch");
    RPC_CHECK(watch != nullptr);
    RPC_CHECK(watch->binding.streaming);
    RPC_CHECK_EQ(watch->binding.argc, static_cast<std::uint8_t>(1));

    const rpc::BoundMethod* add = registry.Find("User.Add");
    RPC_CHECK(add != nullptr);
    RPC_CHECK_EQ(add->binding.argc, static_cast<std::uint8_t>(2));

    const auto names = registry.MethodNames();
    RPC_CHECK_EQ(names.size(), static_cast<std::size_t>(12));
    RPC_CHECK(!names.empty() && names.front() == "User.Add");
    RPC_CHECK(std::is_sorted(names.begin(), names.end()));

    // 非法名字/空方法表/空实现。
    rpc::MethodBinding invalid{};
    invalid.name = "X";
    invalid.invoke = nullptr;
    RPC_CHECK(!registry.AddBindings("Bad", &service, &invalid, 1));
    RPC_CHECK(!registry.AddBindings("Bad Name", &service, &invalid, 1));
    RPC_CHECK(!registry.AddBindings("Bad", nullptr, &invalid, 1));
    RPC_CHECK(!registry.AddBindings("Bad", &service, nullptr, 1));
    RPC_CHECK(!registry.AddBindings("Bad", &service, &invalid, 0));

    // 冻结后拒绝注册，查找仍然可用。
    registry.Freeze();
    RPC_CHECK(registry.frozen());
    RPC_CHECK(!registry.AddService("Other", &service));
    RPC_CHECK(registry.Find("User.Get") != nullptr);
}

/// @brief 按线上格式打包一个参数（varint 长度 + 序列化字节）。
template <typename T>
void PackArg(reflect::Buffer& out, const T& value) {
    reflect::Buffer scratch;
    RPC_CHECK(reflect::Serialize(value, scratch));
    RPC_CHECK(out.PutVarint(static_cast<std::uint64_t>(scratch.size())));
    RPC_CHECK(out.PutBytes(scratch.data().data(), scratch.size()));
}

// ---------------------------------------------------------------------------
// 方法绑定：直接按类型擦除签名调用
// ---------------------------------------------------------------------------

/// @brief 用给定参数调用一个已注册方法，返回状态并把结果写进 `result`。
rpc::Status Invoke(const rpc::ServiceRegistry& registry, const std::string& method,
                   const reflect::Buffer& args, reflect::Buffer* result, bool* streaming) {
    const rpc::BoundMethod* bound = registry.Find(method);
    if (bound == nullptr) {
        return rpc::Status(rpc::StatusCode::kNotFound, "not found");
    }
    if (streaming != nullptr) {
        *streaming = bound->binding.streaming;
    }
    std::atomic<bool> cancelled{false};
    rpc::CallContext context(7, &method, &cancelled);
    reflect::Buffer mutable_args;
    mutable_args.PutBytes(args.data().data(), args.size());
    return bound->binding.invoke(bound->service, context, mutable_args, *result, nullptr);
}

void TestBinder() {
    Announce("binder: return/out/status/void/multi-arg");
    rpc::ServiceRegistry registry;
    UserService service;
    RPC_CHECK(registry.AddService("User", &service));

    // 值返回：User Get(UserId)
    {
        reflect::Buffer args;
        UserId id;
        id.id = 42;
        PackArg(args, id);
        reflect::Buffer result;
        RPC_CHECK(Invoke(registry, "User.Get", args, &result, nullptr).ok());
        User user;
        RPC_CHECK(reflect::Deserialize(result, user));
        RPC_CHECK_EQ(user.id, static_cast<std::int64_t>(42));
        RPC_CHECK_EQ(user.name, std::string("user-42"));
        RPC_CHECK_EQ(result.Remaining(), static_cast<std::size_t>(0));
    }

    // out 参数：Status Create(CreateRequest, User*)
    {
        reflect::Buffer args;
        CreateRequest request;
        request.name = "alice";
        request.seed = 7;
        PackArg(args, request);
        reflect::Buffer result;
        RPC_CHECK(Invoke(registry, "User.Create", args, &result, nullptr).ok());
        User user;
        RPC_CHECK(reflect::Deserialize(result, user));
        RPC_CHECK_EQ(user.name, std::string("alice"));
        RPC_CHECK_EQ(user.level, static_cast<std::int32_t>(7));
        RPC_CHECK(user.id > 0);
    }

    // 业务错误：Status Fail(UserId)
    {
        reflect::Buffer args;
        UserId id;
        id.id = 1;
        PackArg(args, id);
        reflect::Buffer result;
        const rpc::Status status = Invoke(registry, "User.Fail", args, &result, nullptr);
        RPC_CHECK(!status.ok());
        RPC_CHECK(status.code == rpc::StatusCode::kPermissionDenied);
        RPC_CHECK_EQ(status.message, std::string("permission denied"));
    }

    // 仅状态：Status Remove(UserId)
    {
        reflect::Buffer args;
        UserId id;
        id.id = 0;
        PackArg(args, id);
        reflect::Buffer result;
        const rpc::Status status = Invoke(registry, "User.Remove", args, &result, nullptr);
        RPC_CHECK(status.code == rpc::StatusCode::kNotFound);
    }

    // void：Touch(UserId)
    {
        reflect::Buffer args;
        UserId id;
        id.id = 3;
        PackArg(args, id);
        reflect::Buffer result;
        RPC_CHECK(Invoke(registry, "User.Touch", args, &result, nullptr).ok());
        RPC_CHECK_EQ(result.size(), static_cast<std::size_t>(0));
    }

    // 多参数 + out：Status Add(int64, int64, int64*)
    {
        reflect::Buffer args;
        PackArg(args, static_cast<std::int64_t>(11));
        PackArg(args, static_cast<std::int64_t>(31));
        reflect::Buffer result;
        RPC_CHECK(Invoke(registry, "User.Add", args, &result, nullptr).ok());
        std::int64_t sum = 0;
        RPC_CHECK(reflect::Deserialize(result, sum));
        RPC_CHECK_EQ(sum, static_cast<std::int64_t>(42));
    }

    // 参数不足以解码：必须返回 INVALID_ARGUMENT，而不是崩溃。
    {
        reflect::Buffer empty;
        reflect::Buffer result;
        const rpc::Status status = Invoke(registry, "User.Get", empty, &result, nullptr);
        RPC_CHECK(status.code == rpc::StatusCode::kInvalidArgument);
    }

    // 尾随字节：拒绝。
    {
        reflect::Buffer args;
        UserId id;
        id.id = 5;
        PackArg(args, id);
        RPC_CHECK(args.PutU8(0x7f));
        reflect::Buffer result;
        const rpc::Status status = Invoke(registry, "User.Get", args, &result, nullptr);
        RPC_CHECK(status.code == rpc::StatusCode::kInvalidArgument);
    }

    // 流式标志：binder 报告 streaming；直接调用时因为 stream == nullptr 返回 INTERNAL。
    {
        reflect::Buffer args;
        WatchRequest request;
        request.count = 3;
        PackArg(args, request);
        reflect::Buffer result;
        bool streaming = false;
        const rpc::Status status = Invoke(registry, "User.Watch", args, &result, &streaming);
        RPC_CHECK(streaming);
        RPC_CHECK(status.code == rpc::StatusCode::kInternal);
    }
}

// ---------------------------------------------------------------------------
// 协议往返
// ---------------------------------------------------------------------------

void TestProtocolRoundTrip() {
    Announce("protocol: round-trip");
    // 请求
    {
        reflect::Buffer args;
        UserId id;
        id.id = 123456789;
        RPC_CHECK(reflect::Serialize(id, args));
        std::vector<std::uint8_t> payload;
        RPC_CHECK(rpc::EncodeRequest(payload, 99, "User.Get", args));
        rpc::RequestMessage message;
        std::string error;
        RPC_CHECK(rpc::DecodeRequest(MakeFrame(rpc::FrameType::kRequest, payload), &message,
                                     &error));
        RPC_CHECK_EQ(message.call_id, static_cast<std::uint64_t>(99));
        RPC_CHECK_EQ(message.method, std::string("User.Get"));
        RPC_CHECK(!message.streaming);
        UserId decoded;
        RPC_CHECK(reflect::Deserialize(message.args, decoded));
        RPC_CHECK_EQ(decoded.id, static_cast<std::int64_t>(123456789));

        // 流式标志在帧头。
        rpc::RequestMessage streaming_message;
        RPC_CHECK(rpc::DecodeRequest(
            MakeFrame(rpc::FrameType::kRequest, payload, rpc::kFlagStreaming),
            &streaming_message, &error));
        RPC_CHECK(streaming_message.streaming);
    }

    // 响应（成功 + 错误文本）
    {
        reflect::Buffer result;
        User user;
        user.id = 7;
        user.name = "bob";
        user.level = 2;
        RPC_CHECK(reflect::Serialize(user, result));
        std::vector<std::uint8_t> payload;
        RPC_CHECK(rpc::EncodeResponse(payload, 5, rpc::Status::Ok(), result));
        rpc::ResponseMessage message;
        std::string error;
        RPC_CHECK(rpc::DecodeResponse(MakeFrame(rpc::FrameType::kResponse, payload), &message,
                                      &error));
        RPC_CHECK_EQ(message.call_id, static_cast<std::uint64_t>(5));
        RPC_CHECK(message.status.ok());
        User decoded;
        RPC_CHECK(reflect::Deserialize(message.payload, decoded));
        RPC_CHECK_EQ(decoded.name, std::string("bob"));

        std::vector<std::uint8_t> error_payload;
        RPC_CHECK(rpc::EncodeResponse(
            error_payload, 6,
            rpc::Status(rpc::StatusCode::kPermissionDenied, "nope"), reflect::Buffer{}));
        rpc::ResponseMessage error_message;
        RPC_CHECK(rpc::DecodeResponse(MakeFrame(rpc::FrameType::kResponse, error_payload),
                                      &error_message, &error));
        RPC_CHECK(error_message.status.code == rpc::StatusCode::kPermissionDenied);
        RPC_CHECK_EQ(error_message.status.message, std::string("nope"));
    }

    // 流数据
    {
        const std::string text = "hello-stream";
        std::vector<std::uint8_t> payload;
        RPC_CHECK(rpc::EncodeStreamData(payload, 42, 7, text.data(), text.size()));
        rpc::StreamDataMessage message;
        std::string error;
        RPC_CHECK(rpc::DecodeStreamData(MakeFrame(rpc::FrameType::kStreamData, payload), &message,
                                        &error));
        RPC_CHECK_EQ(message.call_id, static_cast<std::uint64_t>(42));
        RPC_CHECK_EQ(message.sequence, static_cast<std::uint64_t>(7));
        RPC_CHECK_EQ(std::string(message.payload.begin(), message.payload.end()), text);
    }

    // 流结束
    {
        std::vector<std::uint8_t> payload;
        RPC_CHECK(rpc::EncodeStreamEnd(
            payload, 43, rpc::Status(rpc::StatusCode::kCancelled, "cancelled")));
        rpc::StreamEndMessage message;
        std::string error;
        RPC_CHECK(rpc::DecodeStreamEnd(MakeFrame(rpc::FrameType::kStreamEnd, payload), &message,
                                       &error));
        RPC_CHECK_EQ(message.call_id, static_cast<std::uint64_t>(43));
        RPC_CHECK(message.status.code == rpc::StatusCode::kCancelled);
    }

    // 取消/心跳
    {
        std::vector<std::uint8_t> payload;
        RPC_CHECK(rpc::EncodeCancel(payload, 44));
        std::uint64_t call_id = 0;
        std::string error;
        RPC_CHECK(rpc::DecodeCallId(MakeFrame(rpc::FrameType::kCancel, payload), &call_id, &error));
        RPC_CHECK_EQ(call_id, static_cast<std::uint64_t>(44));

        RPC_CHECK(rpc::EncodePing(payload, 45, false));
        RPC_CHECK(rpc::DecodeCallId(MakeFrame(rpc::FrameType::kPong, payload), &call_id, &error));
        RPC_CHECK_EQ(call_id, static_cast<std::uint64_t>(45));
    }
}

void TestProtocolRejects() {
    Announce("protocol: malformed inputs rejected");

    // 方法名超长。
    {
        std::vector<std::uint8_t> payload;
        RPC_CHECK(!rpc::EncodeRequest(payload, 1, std::string(rpc::kMaxMethodBytes + 1, 'm'),
                                      reflect::Buffer{}));
        RPC_CHECK(payload.empty());
    }

    // 状态码越界（手工构造 payload：call_id=1, status=99）。
    {
        std::vector<std::uint8_t> payload{1, 99, 0, 0};
        rpc::ResponseMessage message;
        std::string error;
        RPC_CHECK(!rpc::DecodeResponse(MakeFrame(rpc::FrameType::kResponse, payload), &message,
                                       &error));
        RPC_CHECK(!error.empty());
    }

    // 帧类型不符。
    {
        rpc::RequestMessage message;
        std::string error;
        RPC_CHECK(!rpc::DecodeRequest(MakeFrame(rpc::FrameType::kResponse, {1, 0, 0}), &message,
                                      &error));
    }

    // 截断/尾随：对每个合法 payload 逐字节截断，全部必须拒绝。
    {
        reflect::Buffer args;
        UserId id;
        id.id = 9;
        RPC_CHECK(reflect::Serialize(id, args));
        std::vector<std::uint8_t> payload;
        RPC_CHECK(rpc::EncodeRequest(payload, 3, "User.Get", args));
        for (std::size_t length = 0; length < payload.size(); ++length) {
            std::vector<std::uint8_t> truncated(payload.begin(),
                                                payload.begin() + static_cast<std::ptrdiff_t>(length));
            rpc::RequestMessage message;
            std::string error;
            RPC_CHECK(!rpc::DecodeRequest(
                MakeFrame(rpc::FrameType::kRequest, std::move(truncated)), &message, &error));
        }
        std::vector<std::uint8_t> trailing = payload;
        trailing.push_back(0);
        rpc::RequestMessage message;
        std::string error;
        RPC_CHECK(!rpc::DecodeRequest(MakeFrame(rpc::FrameType::kRequest, trailing), &message,
                                      &error));
    }
}

void TestProtocolFuzz() {
    Announce("protocol: 100k random frames (no crash)");
    std::mt19937_64 random(20261101);
    std::uniform_int_distribution<int> type_distribution(0, 9);
    std::uniform_int_distribution<std::size_t> length_distribution(0, 96);
    std::uniform_int_distribution<int> byte_distribution(0, 255);
    std::size_t accepted = 0;
    for (std::size_t iteration = 0; iteration < 100000; ++iteration) {
        const std::size_t length = length_distribution(random);
        std::vector<std::uint8_t> payload(length);
        for (std::size_t index = 0; index < length; ++index) {
            payload[index] = static_cast<std::uint8_t>(byte_distribution(random));
        }
        frame::Frame frame;
        frame.type = static_cast<std::uint8_t>(type_distribution(random));
        frame.flags = static_cast<std::uint16_t>(byte_distribution(random));
        frame.payload = std::move(payload);
        rpc::RequestMessage request;
        rpc::ResponseMessage response;
        rpc::StreamDataMessage stream_data;
        rpc::StreamEndMessage stream_end;
        std::uint64_t call_id = 0;
        std::string error;
        if (rpc::DecodeRequest(frame, &request, &error)) {
            ++accepted;
        }
        if (rpc::DecodeResponse(frame, &response, &error)) {
            ++accepted;
        }
        if (rpc::DecodeStreamData(frame, &stream_data, &error)) {
            ++accepted;
        }
        if (rpc::DecodeStreamEnd(frame, &stream_end, &error)) {
            ++accepted;
        }
        if (rpc::DecodeCallId(frame, &call_id, &error)) {
            ++accepted;
        }
    }
    RPC_CHECK(accepted < 500000);  // 随机字节几乎不可能全部合法。
}

// ---------------------------------------------------------------------------
// 压测：请求编码 → 解码
// ---------------------------------------------------------------------------

void BenchmarkProtocol(std::int64_t iterations) {
    Announce("bench: request encode/decode");
    const std::int64_t count = iterations > 0 ? iterations : 1000000;
    UserId id;
    id.id = 42;
    reflect::Buffer args;
    RPC_CHECK(reflect::Serialize(id, args));
    std::vector<std::uint8_t> payload;
    rpc::RequestMessage message;
    std::string error;
    const double start = NowMicros();
    for (std::int64_t iteration = 0; iteration < count; ++iteration) {
        if (!rpc::EncodeRequest(payload, static_cast<std::uint64_t>(iteration), "User.Get", args) ||
            !rpc::DecodeRequest(MakeFrame(rpc::FrameType::kRequest, payload), &message, &error)) {
            ++g_failures;
            break;
        }
    }
    const double elapsed = NowMicros() - start;
    const double ns_per_op = elapsed * 1000.0 / static_cast<double>(count);
    std::printf("  %lld ops in %.1f ms -> %.1f ns/op (encode+decode)\n",
                static_cast<long long>(count), elapsed / 1000.0, ns_per_op);
    std::fflush(stdout);
    RPC_CHECK(ns_per_op < 5000.0);
}

}  // namespace

int main() {
    TestRegistry();
    TestBinder();
    TestProtocolRoundTrip();
    TestProtocolRejects();
    TestProtocolFuzz();
    BenchmarkProtocol(EnvInt("RPC_PROTOCOL_ITERS", 1000000));
    return Summary("protocol");
}
