/**
 * @file test_coap.cpp
 * @brief CoAP（RFC 7252）骨架测试：线格式 / 选项 delta-length / 边界 / 往返 /
 *        客户端-服务端交换 / 重传 / 去重 / 非法帧 / 随机 fuzz。
 * @details 覆盖范围：
 *          - 已知线格式向量（逐字节手工核对）与 180 组编解码往返矩阵；
 *          - 选项 delta/length 的全部编码区间（0..12 / 13..268 / 269..65804）
 *            以及编号上限 65535、单选项 65804 字节边界；
 *          - 选项稳定排序、Uri-Path/Content-Format 辅助、uint 最短编码；
 *          - 编码拒绝（TKL>8、选项超长、报文超 1 MiB、非法类型）；
 *          - 非法帧矩阵（版本 / TKL / 保留 nibble / 截断 / Payload Marker /
 *            编号累加溢出）；
 *          - 客户端-服务端同步回环：CON/NON、空 ACK + separate response、
 *            RST、Token 不匹配、重传时间线（指数退避、字节一致）、超时；
 *          - 服务端去重/丢弃/默认方法映射/handler 非法响应码兜底；
 *          - 随机往返 fuzz、变异帧 fuzz、截断前缀、2 万轮编解码压测。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/coap.hpp"
#include "test_util.hpp"

namespace {

using namespace go2cpp::bento::net;

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

/// 用 int 列表构造字节串，便于在期望值里直接写字符字面量（'s' 等）。
std::vector<std::uint8_t> Bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    out.reserve(values.size());
    for (const int value : values) {
        out.push_back(static_cast<std::uint8_t>(value));
    }
    return out;
}

std::vector<std::uint8_t> EncodeOk(const CoapMessage& message) {
    CoapStatus status = CoapStatus::kInvalidArgument;
    const std::vector<std::uint8_t> wire = EncodeCoapMessage(message, &status);
    BENTO_CHECK(status == CoapStatus::kOk);
    return wire;
}

CoapMessage DecodeOk(const std::vector<std::uint8_t>& wire) {
    CoapMessage message;
    BENTO_CHECK(DecodeCoapMessage(wire, &message) == CoapStatus::kOk);
    return message;
}

std::vector<std::uint8_t> MakeResponseWire(
    CoapType type, CoapCode code, std::uint16_t message_id,
    const std::vector<std::uint8_t>& token,
    const std::vector<std::uint8_t>& payload = {}) {
    CoapMessage message;
    message.type = type;
    message.code = code;
    message.message_id = message_id;
    message.token = token;
    message.payload = payload;
    return EncodeOk(message);
}

/// 确定性 xorshift64：随机测试可复现，不依赖系统熵。
struct Rng {
    std::uint64_t state = 0x243f6a8885a308d3ull;

    std::uint64_t Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    std::uint64_t Below(std::uint64_t bound) {
        return bound == 0 ? 0 : Next() % bound;
    }
};

/// 客户端 <-> 服务端同步内存回环：一端的发送回调立即投递给另一端。
struct Loopback {
    CoapServer server;
    CoapClient client;
    std::vector<std::vector<std::uint8_t>> from_client;
    std::vector<std::vector<std::uint8_t>> from_server;
    std::vector<CoapClientEvent> events;
    std::vector<CoapMessage> event_messages;

    Loopback()
        : server([this](const std::vector<std::uint8_t>& wire) {
              from_server.push_back(wire);
              return client.HandleDatagram(wire);
          }),
          client([this](const std::vector<std::uint8_t>& wire) {
              from_client.push_back(wire);
              return server.Handle(wire).has_value();
          }) {
        client.OnEvent([this](CoapClientEvent event, const CoapMessage& message) {
            events.push_back(event);
            event_messages.push_back(message);
        });
    }
};

// ---------------------------------------------------------------------------
// 编解码：已知向量 / 往返 / 选项编码
// ---------------------------------------------------------------------------

void TestKnownWireVector() {
    CoapMessage message;
    message.type = CoapType::kCon;
    message.code = CoapCode::kGet;
    message.message_id = 0x1234;
    message.token = {0xaa, 0xbb, 0xcc, 0xdd};
    message.AddUriPath("/sensors/temp");
    message.payload = {'o', 'k'};

    const std::vector<std::uint8_t> wire = EncodeOk(message);
    const std::vector<std::uint8_t> expected = Bytes({
        0x44, 0x01, 0x12, 0x34,                    // Ver=1, CON, TKL=4, 0.01, MID
        0xaa, 0xbb, 0xcc, 0xdd,                    // Token
        0xb7, 's', 'e', 'n', 's', 'o', 'r', 's',   // Uri-Path "sensors"
        0x04, 't', 'e', 'm', 'p',                  // Uri-Path "temp"
        0xff, 'o', 'k',                            // Payload Marker + "ok"
    });
    BENTO_CHECK_EQ(wire.size(), expected.size());
    BENTO_CHECK(wire == expected);

    const CoapMessage decoded = DecodeOk(wire);
    BENTO_CHECK(decoded.type == CoapType::kCon);
    BENTO_CHECK(decoded.code == CoapCode::kGet);
    BENTO_CHECK_EQ(decoded.message_id, static_cast<std::uint16_t>(0x1234));
    BENTO_CHECK(decoded.token == message.token);
    BENTO_CHECK(decoded.UriPath() == std::string("/sensors/temp"));
    BENTO_CHECK(decoded.payload == message.payload);

    // 空 ACK（0.00）：4 字节头，无 Token / 选项 / 载荷。
    CoapMessage ack;
    ack.type = CoapType::kAck;
    ack.code = CoapCode::kEmpty;
    ack.message_id = 0x002a;
    BENTO_CHECK(EncodeOk(ack) == Bytes({0x60, 0x00, 0x00, 0x2a}));
}

void TestRoundtripMatrix() {
    const CoapType types[] = {CoapType::kCon, CoapType::kNon, CoapType::kAck,
                              CoapType::kRst};
    const CoapCode codes[] = {CoapCode::kEmpty, CoapCode::kGet, CoapCode::kContent,
                              CoapCode::kBadRequest, CoapCode::kInternalServerError};
    std::size_t cases = 0;
    for (const CoapType type : types) {
        for (const CoapCode code : codes) {
            for (std::size_t token_bytes = 0; token_bytes <= kCoapMaxTokenBytes;
                 ++token_bytes) {
                CoapMessage message;
                message.type = type;
                message.code = code;
                message.message_id =
                    static_cast<std::uint16_t>(0x8000u + token_bytes * 7u);
                message.token.resize(token_bytes);
                for (std::size_t index = 0; index < token_bytes; ++index) {
                    message.token[index] = static_cast<std::uint8_t>(0x40u + index);
                }
                // 选项按编号升序追加：7 -> 11,11,11 -> 12 -> 15。
                message.AddUintOption(kCoapOptionUriPort, 5683);
                message.AddUriPath("/a/b/c");
                message.AddUintOption(kCoapOptionContentFormat,
                                      kCoapContentFormatJson);
                message.AddStringOption(kCoapOptionUriQuery, "x=1");
                message.payload = {'p', 'a', 'y'};

                const std::vector<std::uint8_t> wire = EncodeOk(message);
                const CoapMessage decoded = DecodeOk(wire);
                BENTO_CHECK(decoded.type == message.type);
                BENTO_CHECK(decoded.code == message.code);
                BENTO_CHECK_EQ(decoded.message_id, message.message_id);
                BENTO_CHECK(decoded.token == message.token);
                BENTO_CHECK(decoded.options == message.options);
                BENTO_CHECK(decoded.payload == message.payload);
                BENTO_CHECK(EncodeOk(decoded) == wire);  // 规范化后字节稳定
                ++cases;
            }
        }
    }
    BENTO_CHECK_EQ(cases, static_cast<std::size_t>(4 * 5 * 9));
}

void TestOptionDeltaLengthBoundaries() {
    struct OptionCase {
        std::uint16_t number;
        std::uint32_t length;
        std::vector<std::uint8_t> prefix;  // 4 字节头之后的前缀字节
    };
    const OptionCase cases[] = {
        {0, 0, Bytes({0x00})},                             // 直接值 0/0
        {12, 12, Bytes({0xcc})},                           // 直接值上限
        {13, 13, Bytes({0xdd, 0x00, 0x00})},               // 1 字节扩展起点
        {268, 268, Bytes({0xdd, 0xff, 0xff})},             // 1 字节扩展上限
        {269, 269, Bytes({0xee, 0x00, 0x00, 0x00, 0x00})},  // 2 字节扩展起点
        {65535, 65804, Bytes({0xee, 0xfe, 0xf2, 0xff, 0xff})},  // 双上限
    };
    for (const OptionCase& test : cases) {
        CoapMessage message;
        message.code = CoapCode::kPost;
        message.message_id = 0x0102;
        message.options.push_back(
            CoapOption{test.number, std::vector<std::uint8_t>(test.length, 0x5a)});

        const std::vector<std::uint8_t> wire = EncodeOk(message);
        BENTO_CHECK_EQ(wire.size(), 4 + test.prefix.size() + test.length);
        BENTO_CHECK(wire.size() >= 4 + test.prefix.size());
        if (wire.size() >= 4 + test.prefix.size()) {
            BENTO_CHECK(std::equal(test.prefix.begin(), test.prefix.end(),
                                   wire.begin() + 4));
        }

        const CoapMessage decoded = DecodeOk(wire);
        BENTO_CHECK_EQ(decoded.options.size(), static_cast<std::size_t>(1));
        if (!decoded.options.empty()) {
            BENTO_CHECK_EQ(decoded.options[0].number, test.number);
            BENTO_CHECK_EQ(decoded.options[0].value.size(),
                           static_cast<std::size_t>(test.length));
            BENTO_CHECK(std::all_of(decoded.options[0].value.begin(),
                                    decoded.options[0].value.end(),
                                    [](std::uint8_t byte) { return byte == 0x5a; }));
        }
    }
}

void TestOptionOrderingAndHelpers() {
    CoapMessage message;
    message.code = CoapCode::kPost;
    message.message_id = 0x0007;
    message.AddUriPath("/sensors/temp");                            // 11, 11
    message.AddUintOption(kCoapOptionContentFormat,
                          kCoapContentFormatJson);                  // 12
    message.AddStringOption(kCoapOptionUriHost, "example.com");     // 3
    message.AddUriPath("/c");                                       // 11

    const std::vector<std::uint8_t> wire = EncodeOk(message);
    const CoapMessage decoded = DecodeOk(wire);
    BENTO_CHECK_EQ(decoded.options.size(), static_cast<std::size_t>(5));
    const std::uint16_t expected_numbers[] = {
        kCoapOptionUriHost, kCoapOptionUriPath, kCoapOptionUriPath,
        kCoapOptionUriPath, kCoapOptionContentFormat};
    for (std::size_t index = 0; index < decoded.options.size() && index < 5; ++index) {
        BENTO_CHECK_EQ(decoded.options[index].number, expected_numbers[index]);
    }
    // 同编号选项保持调用方顺序（Uri-Path 段序不能乱）。
    BENTO_CHECK(decoded.UriPath() == std::string("/sensors/temp/c"));
    BENTO_CHECK(decoded.ContentFormat() ==
                std::optional<std::uint16_t>(kCoapContentFormatJson));
    const CoapOption* host = decoded.FindOption(kCoapOptionUriHost);
    BENTO_CHECK(host != nullptr);
    if (host != nullptr) {
        BENTO_CHECK(host->AsString() == std::string("example.com"));
    }
    BENTO_CHECK(decoded.FindOption(kCoapOptionUriPort) == nullptr);

    // Uri-Path 辅助：前导 '/'、重复 '/'、尾随 '/'（骨架简化语义）。
    CoapMessage paths;
    paths.AddUriPath("");
    BENTO_CHECK(paths.options.empty());
    BENTO_CHECK(paths.UriPath() == std::string("/"));
    paths.AddUriPath("sensors/temp");
    paths.AddUriPath("/");
    paths.AddUriPath("//a//b/");
    BENTO_CHECK_EQ(paths.options.size(), static_cast<std::size_t>(4));
    BENTO_CHECK(paths.UriPath() == std::string("/sensors/temp/a/b"));

    // Content-Format 超出 16 位，或值超过 4 字节 -> nullopt。
    CoapMessage wide;
    wide.AddUintOption(kCoapOptionContentFormat, 65536);
    BENTO_CHECK(!wide.ContentFormat().has_value());
    wide.options.clear();
    wide.AddOption(kCoapOptionContentFormat, std::vector<std::uint8_t>(5, 0x01));
    BENTO_CHECK(!wide.ContentFormat().has_value());
}

void TestUintAndStringOptions() {
    struct UintCase {
        std::uint32_t value;
        std::vector<std::uint8_t> bytes;
    };
    const UintCase cases[] = {
        {0, {}},
        {1, {0x01}},
        {255, {0xff}},
        {256, {0x01, 0x00}},
        {65535, {0xff, 0xff}},
        {65536, {0x01, 0x00, 0x00}},
        {0xffffffffu, {0xff, 0xff, 0xff, 0xff}},
    };
    for (const UintCase& test : cases) {
        const CoapOption option = CoapOption::FromUint(kCoapOptionSize1, test.value);
        BENTO_CHECK(option.number == kCoapOptionSize1);
        BENTO_CHECK_EQ(option.value.size(), test.bytes.size());
        BENTO_CHECK(option.value == test.bytes);
        BENTO_CHECK(option.AsUint() ==
                    std::optional<std::uint32_t>(test.value));
    }

    CoapOption too_long;
    too_long.number = kCoapOptionSize1;
    too_long.value.assign(5, 0x01);
    BENTO_CHECK(!too_long.AsUint().has_value());

    const CoapOption text = CoapOption::FromString(kCoapOptionUriPath, "a/b");
    BENTO_CHECK(text.AsString() == std::string("a/b"));
    CoapOption other = text;
    other.number = kCoapOptionUriQuery;
    BENTO_CHECK(text != other);
    BENTO_CHECK(!(text == other));
}

// ---------------------------------------------------------------------------
// 编码拒绝 / 非法帧 / 报文上限
// ---------------------------------------------------------------------------

void TestEncodeArgumentErrors() {
    CoapMessage message;
    message.code = CoapCode::kGet;

    CoapStatus status = CoapStatus::kOk;
    message.token.assign(kCoapMaxTokenBytes + 1, 0x01);
    BENTO_CHECK(EncodeCoapMessage(message, &status).empty());
    BENTO_CHECK(status == CoapStatus::kBadTokenLength);

    message.token.clear();
    message.options.push_back(
        CoapOption{kCoapOptionSize1,
                   std::vector<std::uint8_t>(kCoapMaxOptionValueBytes + 1, 0x00)});
    BENTO_CHECK(EncodeCoapMessage(message, &status).empty());
    BENTO_CHECK(status == CoapStatus::kInvalidArgument);

    message.options.clear();
    message.type = static_cast<CoapType>(4);
    BENTO_CHECK(EncodeCoapMessage(message, &status).empty());
    BENTO_CHECK(status == CoapStatus::kInvalidArgument);

    message.type = CoapType::kCon;
    message.payload.assign(kCoapMaxMessageBytes - 4, 0x42);  // + marker 超限
    BENTO_CHECK(EncodeCoapMessage(message, &status).empty());
    BENTO_CHECK(status == CoapStatus::kMessageTooLarge);

    message.payload.clear();
    BENTO_CHECK(!EncodeCoapMessage(message, nullptr).empty());
}

void TestDecodeMalformedFrames() {
    struct Case {
        std::vector<std::uint8_t> wire;
        CoapStatus expected;
    };
    const Case cases[] = {
        {Bytes({}), CoapStatus::kTruncated},
        {Bytes({0x40}), CoapStatus::kTruncated},
        {Bytes({0x40, 0x01}), CoapStatus::kTruncated},
        {Bytes({0x40, 0x01, 0x12}), CoapStatus::kTruncated},
        {Bytes({0x80, 0x01, 0x00, 0x01}), CoapStatus::kBadVersion},
        {Bytes({0xc0, 0x01, 0x00, 0x01}), CoapStatus::kBadVersion},
        {Bytes({0x49, 0x01, 0x00, 0x01}), CoapStatus::kBadTokenLength},
        {Bytes({0x4f, 0x01, 0x00, 0x01}), CoapStatus::kBadTokenLength},
        {Bytes({0x48, 0x01, 0x00, 0x01}), CoapStatus::kTruncated},
        {Bytes({0x40, 0x01, 0x00, 0x01, 0xf0}), CoapStatus::kBadOptionDelta},
        {Bytes({0x40, 0x01, 0x00, 0x01, 0x0f}), CoapStatus::kBadOptionLength},
        {Bytes({0x40, 0x01, 0x00, 0x01, 0xff}),
         CoapStatus::kPayloadMarkerWithoutPayload},
        {Bytes({0x40, 0x01, 0x00, 0x01, 0xd0}), CoapStatus::kTruncated},
        {Bytes({0x40, 0x01, 0x00, 0x01, 0xe0, 0x00}), CoapStatus::kTruncated},
        {Bytes({0x40, 0x01, 0x00, 0x01, 0x05, 'a'}), CoapStatus::kTruncated},
        {Bytes({0x40, 0x01, 0x00, 0x01, 0xe0, 0xfe, 0xf2, 0x10}),
         CoapStatus::kBadOptionDelta},
    };
    for (const Case& test : cases) {
        CoapMessage message;
        message.code = CoapCode::kContent;  // 哨兵：失败时输出不得被改写
        const CoapStatus status = DecodeCoapMessage(test.wire, &message);
        BENTO_CHECK(status == test.expected);
        BENTO_CHECK(message.code == CoapCode::kContent);
    }

    CoapMessage message;
    BENTO_CHECK(DecodeCoapMessage(nullptr, 0, &message) ==
                CoapStatus::kInvalidArgument);
    const std::uint8_t header[4] = {0x40, 0x01, 0x00, 0x01};
    BENTO_CHECK(DecodeCoapMessage(header, sizeof(header), nullptr) ==
                CoapStatus::kInvalidArgument);
}

void TestMessageByteLimitBoundary() {
    CoapMessage message;
    message.code = CoapCode::kPost;
    message.payload.assign(kCoapMaxMessageBytes - 5, 0x42);

    CoapStatus status = CoapStatus::kInvalidArgument;
    const std::vector<std::uint8_t> wire = EncodeCoapMessage(message, &status);
    BENTO_CHECK(status == CoapStatus::kOk);
    BENTO_CHECK_EQ(wire.size(), static_cast<std::size_t>(kCoapMaxMessageBytes));

    const CoapMessage decoded = DecodeOk(wire);
    BENTO_CHECK_EQ(decoded.payload.size(),
                   static_cast<std::size_t>(kCoapMaxMessageBytes - 5));

    std::vector<std::uint8_t> too_large(kCoapMaxMessageBytes + 1, 0x00);
    CoapMessage ignored;
    BENTO_CHECK(DecodeCoapMessage(too_large.data(), too_large.size(), &ignored) ==
                CoapStatus::kMessageTooLarge);
}

// ---------------------------------------------------------------------------
// 客户端-服务端交换
// ---------------------------------------------------------------------------

void TestClientServerExchange() {
    Loopback loop;
    loop.server.SetHandler([](const CoapMessage& request) {
        if (request.UriPath() == "/sensors/temp") {
            return CoapResponse::Content({'2', '3', '.', '5'},
                                         kCoapContentFormatTextPlain);
        }
        CoapResponse response;
        response.code = CoapCode::kNotFound;
        return response;
    });

    BENTO_CHECK(loop.client.SendRequest(CoapCode::kGet, "/sensors/temp"));
    BENTO_CHECK_EQ(loop.from_client.size(), static_cast<std::size_t>(1));
    BENTO_CHECK_EQ(loop.from_server.size(), static_cast<std::size_t>(1));
    BENTO_CHECK(!loop.client.HasPending());
    BENTO_CHECK_EQ(loop.server.HandlerCalls(), std::uint64_t{1});
    BENTO_CHECK_EQ(loop.events.size(), static_cast<std::size_t>(1));
    if (loop.events.size() == 1) {
        BENTO_CHECK(loop.events[0] == CoapClientEvent::kResponse);
    }

    const CoapMessage request = DecodeOk(loop.from_client[0]);
    BENTO_CHECK(request.type == CoapType::kCon);
    BENTO_CHECK(request.code == CoapCode::kGet);
    BENTO_CHECK(request.message_id == loop.client.LastMessageId());
    BENTO_CHECK(request.UriPath() == std::string("/sensors/temp"));
    BENTO_CHECK_EQ(request.token.size(), static_cast<std::size_t>(4));

    const CoapMessage reply = DecodeOk(loop.from_server[0]);
    BENTO_CHECK(reply.type == CoapType::kAck);
    BENTO_CHECK(reply.message_id == request.message_id);
    BENTO_CHECK(reply.token == request.token);
    BENTO_CHECK(reply.code == CoapCode::kContent);
    BENTO_CHECK(reply.payload == std::vector<std::uint8_t>({'2', '3', '.', '5'}));
    BENTO_CHECK(reply.ContentFormat() ==
                std::optional<std::uint16_t>(kCoapContentFormatTextPlain));

    // 第二个请求：未知路径 -> 4.04。
    BENTO_CHECK(loop.client.SendRequest(CoapCode::kGet, "/nope"));
    BENTO_CHECK_EQ(loop.events.size(), static_cast<std::size_t>(2));
    if (loop.events.size() == 2) {
        BENTO_CHECK(loop.events[1] == CoapClientEvent::kResponse);
        BENTO_CHECK(loop.event_messages[1].code == CoapCode::kNotFound);
    }
    BENTO_CHECK_EQ(loop.server.HandlerCalls(), std::uint64_t{2});
    BENTO_CHECK(loop.from_client.size() == 2);
    if (loop.from_client.size() == 2) {
        BENTO_CHECK(loop.from_client[0] != loop.from_client[1]);
    }
}

void TestServerDefaultResponses() {
    std::vector<std::vector<std::uint8_t>> outbox;
    CoapServer server([&outbox](const std::vector<std::uint8_t>& wire) {
        outbox.push_back(wire);
        return true;
    });
    struct Case {
        CoapCode method;
        CoapCode expected;
    };
    const Case cases[] = {{CoapCode::kGet, CoapCode::kContent},
                          {CoapCode::kPost, CoapCode::kCreated},
                          {CoapCode::kPut, CoapCode::kChanged},
                          {CoapCode::kDelete, CoapCode::kDeleted}};
    std::size_t index = 0;
    for (const Case& test : cases) {
        CoapMessage request;
        request.type = CoapType::kCon;
        request.code = test.method;
        request.message_id = static_cast<std::uint16_t>(0x2000u + index);
        request.token = {0x01, 0x02};
        request.AddUriPath("/r");
        const std::optional<CoapMessage> reply = server.Handle(EncodeOk(request));
        BENTO_CHECK(reply.has_value());
        if (!reply.has_value()) {
            continue;
        }
        BENTO_CHECK(reply->type == CoapType::kAck);
        BENTO_CHECK(reply->message_id == request.message_id);
        BENTO_CHECK(reply->token == request.token);
        BENTO_CHECK(reply->code == test.expected);
        ++index;
        BENTO_CHECK_EQ(server.HandlerCalls(), static_cast<std::uint64_t>(index));
        BENTO_CHECK_EQ(outbox.size(), index);
    }
    BENTO_CHECK_EQ(index, static_cast<std::size_t>(4));

    // DefaultResponse 的兜底映射（Handle 会先丢弃非请求码，这里直接测静态函数）。
    BENTO_CHECK(CoapServer::DefaultResponse(static_cast<CoapCode>(0x05)).code ==
                CoapCode::kMethodNotAllowed);
    BENTO_CHECK(CoapServer::DefaultResponse(CoapCode::kGet).code ==
                CoapCode::kContent);
}

void TestServerCustomHandler() {
    std::vector<std::vector<std::uint8_t>> outbox;
    CoapServer server([&outbox](const std::vector<std::uint8_t>& wire) {
        outbox.push_back(wire);
        return true;
    });
    std::vector<CoapMessage> seen;
    server.SetHandler([&seen](const CoapMessage& request) {
        seen.push_back(request);
        if (request.code == CoapCode::kPut &&
            request.UriPath() == "/actuators/led") {
            return CoapResponse::Changed();
        }
        if (request.code == CoapCode::kPost && request.UriPath() == "/devices") {
            return CoapResponse::Created();
        }
        CoapResponse response;
        response.code = CoapCode::kNotFound;
        return response;
    });

    CoapMessage put;
    put.type = CoapType::kCon;
    put.code = CoapCode::kPut;
    put.message_id = 0x3001;
    put.token = {0x07, 0x08};
    put.AddUriPath("/actuators/led");
    put.AddContentFormat(kCoapContentFormatJson);
    put.payload = {'o', 'n'};
    const std::optional<CoapMessage> put_reply = server.Handle(EncodeOk(put));
    BENTO_CHECK(put_reply.has_value());
    if (put_reply.has_value()) {
        BENTO_CHECK(put_reply->code == CoapCode::kChanged);
        BENTO_CHECK(put_reply->payload.empty());
    }
    BENTO_CHECK_EQ(seen.size(), static_cast<std::size_t>(1));
    if (!seen.empty()) {
        BENTO_CHECK(seen[0].payload == std::vector<std::uint8_t>({'o', 'n'}));
        BENTO_CHECK(seen[0].ContentFormat() ==
                    std::optional<std::uint16_t>(kCoapContentFormatJson));
    }

    CoapMessage post;
    post.type = CoapType::kCon;
    post.code = CoapCode::kPost;
    post.message_id = 0x3002;
    post.AddUriPath("/devices");
    const std::optional<CoapMessage> post_reply = server.Handle(EncodeOk(post));
    BENTO_CHECK(post_reply.has_value());
    if (post_reply.has_value()) {
        BENTO_CHECK(post_reply->code == CoapCode::kCreated);
    }

    CoapMessage missing;
    missing.type = CoapType::kCon;
    missing.code = CoapCode::kGet;
    missing.message_id = 0x3003;
    missing.AddUriPath("/missing");
    const std::optional<CoapMessage> missing_reply = server.Handle(EncodeOk(missing));
    BENTO_CHECK(missing_reply.has_value());
    if (missing_reply.has_value()) {
        BENTO_CHECK(missing_reply->code == CoapCode::kNotFound);
    }
    BENTO_CHECK_EQ(seen.size(), static_cast<std::size_t>(3));
}

void TestServerDedup() {
    std::vector<std::vector<std::uint8_t>> outbox;
    CoapServerConfig config;
    config.dedup_cache_size = 2;
    CoapServer server([&outbox](const std::vector<std::uint8_t>& wire) {
        outbox.push_back(wire);
        return true;
    }, config);
    std::size_t handler_calls = 0;
    server.SetHandler([&handler_calls](const CoapMessage&) {
        ++handler_calls;
        return CoapResponse::Content({'o', 'k'}, kCoapContentFormatTextPlain);
    });

    CoapMessage request;
    request.type = CoapType::kCon;
    request.code = CoapCode::kGet;
    request.message_id = 0x4001;
    request.token = {0x0a};
    request.AddUriPath("/x");
    const std::vector<std::uint8_t> wire = EncodeOk(request);

    BENTO_CHECK(server.Handle(wire).has_value());
    BENTO_CHECK(server.Handle(wire).has_value());  // 重传命中缓存
    BENTO_CHECK_EQ(handler_calls, static_cast<std::size_t>(1));
    BENTO_CHECK_EQ(server.HandlerCalls(), std::uint64_t{1});
    BENTO_CHECK_EQ(server.ResponseCount(), std::uint64_t{2});
    BENTO_CHECK_EQ(outbox.size(), static_cast<std::size_t>(2));
    if (outbox.size() == 2) {
        BENTO_CHECK(outbox[0] == outbox[1]);  // 重发字节完全一致
    }

    // 同 MID 不同 Token 不算重复。
    CoapMessage other = request;
    other.token = {0x0b};
    BENTO_CHECK(server.Handle(EncodeOk(other)).has_value());
    BENTO_CHECK_EQ(handler_calls, static_cast<std::size_t>(2));

    // 缓存容量 2：再写入一条后最早的条目被淘汰，重放会重新调用 handler。
    CoapMessage third = request;
    third.message_id = 0x4002;
    BENTO_CHECK(server.Handle(EncodeOk(third)).has_value());
    BENTO_CHECK_EQ(handler_calls, static_cast<std::size_t>(3));
    BENTO_CHECK(server.Handle(wire).has_value());
    BENTO_CHECK_EQ(handler_calls, static_cast<std::size_t>(4));
}

void TestServerDrops() {
    std::vector<std::vector<std::uint8_t>> outbox;
    CoapServer server([&outbox](const std::vector<std::uint8_t>& wire) {
        outbox.push_back(wire);
        return true;
    });

    BENTO_CHECK(!server.Handle(Bytes({0x40})).has_value());
    BENTO_CHECK(server.LastStatus() == CoapStatus::kTruncated);

    CoapMessage response_code;
    response_code.type = CoapType::kCon;
    response_code.code = CoapCode::kContent;
    response_code.message_id = 0x5001;
    BENTO_CHECK(!server.Handle(EncodeOk(response_code)).has_value());

    CoapMessage ack_request;
    ack_request.type = CoapType::kAck;
    ack_request.code = CoapCode::kGet;
    ack_request.message_id = 0x5002;
    BENTO_CHECK(!server.Handle(EncodeOk(ack_request)).has_value());

    CoapMessage rst_request;
    rst_request.type = CoapType::kRst;
    rst_request.code = CoapCode::kPut;
    rst_request.message_id = 0x5003;
    BENTO_CHECK(!server.Handle(EncodeOk(rst_request)).has_value());

    CoapMessage empty;
    empty.type = CoapType::kCon;
    empty.code = CoapCode::kEmpty;
    empty.message_id = 0x5004;
    BENTO_CHECK(!server.Handle(EncodeOk(empty)).has_value());

    BENTO_CHECK_EQ(server.DroppedCount(), std::uint64_t{5});
    BENTO_CHECK_EQ(server.HandlerCalls(), std::uint64_t{0});
    BENTO_CHECK_EQ(server.ResponseCount(), std::uint64_t{0});
    BENTO_CHECK(outbox.empty());
    // 最后一条能正常解码（只是不是请求码），所以状态是 ok。
    BENTO_CHECK(server.LastStatus() == CoapStatus::kOk);
}

void TestServerHandlerContract() {
    std::vector<std::vector<std::uint8_t>> outbox;
    CoapServer server([&outbox](const std::vector<std::uint8_t>& wire) {
        outbox.push_back(wire);
        return true;
    });
    server.SetHandler([](const CoapMessage&) {
        CoapResponse response;
        response.code = CoapCode::kGet;  // 请求码不是合法响应
        response.payload = {'x'};
        return response;
    });
    CoapMessage request;
    request.type = CoapType::kCon;
    request.code = CoapCode::kPut;
    request.message_id = 0x6001;
    const std::optional<CoapMessage> reply = server.Handle(EncodeOk(request));
    BENTO_CHECK(reply.has_value());
    if (reply.has_value()) {
        BENTO_CHECK(reply->code == CoapCode::kInternalServerError);
        BENTO_CHECK(reply->payload.empty());
    }
    BENTO_CHECK_EQ(outbox.size(), static_cast<std::size_t>(1));
    if (!outbox.empty()) {
        BENTO_CHECK(DecodeOk(outbox[0]).code == CoapCode::kInternalServerError);
    }

    // 保留类别 3.xx 同样不算响应码。
    server.SetHandler([](const CoapMessage&) {
        CoapResponse response;
        response.code = static_cast<CoapCode>(0x60);
        return response;
    });
    CoapMessage reserved;
    reserved.type = CoapType::kCon;
    reserved.code = CoapCode::kGet;
    reserved.message_id = 0x6002;
    BENTO_CHECK(!IsCoapResponseCode(static_cast<CoapCode>(0x60)));
    const std::optional<CoapMessage> reserved_reply =
        server.Handle(EncodeOk(reserved));
    BENTO_CHECK(reserved_reply.has_value());
    if (reserved_reply.has_value()) {
        BENTO_CHECK(reserved_reply->code == CoapCode::kInternalServerError);
    }
}

// ---------------------------------------------------------------------------
// 客户端状态机：重传 / 空 ACK / RST / 错误与夹取
// ---------------------------------------------------------------------------

void TestClientRetransmissionTimeline() {
    CoapClientConfig config;
    config.ack_timeout_ms = 1000;
    config.max_retransmit = 2;
    std::vector<std::vector<std::uint8_t>> sent;
    CoapClient client([&sent](const std::vector<std::uint8_t>& wire) {
        sent.push_back(wire);
        return true;
    }, config);
    std::vector<CoapClientEvent> events;
    client.OnEvent([&events](CoapClientEvent event, const CoapMessage&) {
        events.push_back(event);
    });

    CoapRequest request;
    request.method = CoapCode::kGet;
    request.uri_path = "/retry";
    BENTO_CHECK(client.Send(request, 1000));
    BENTO_CHECK(client.HasPending());
    BENTO_CHECK_EQ(sent.size(), static_cast<std::size_t>(1));

    client.Poll(1999);
    BENTO_CHECK_EQ(sent.size(), static_cast<std::size_t>(1));
    client.Poll(2000);  // 第 1 次重传，下一次 deadline = 2000 + 2000
    BENTO_CHECK_EQ(sent.size(), static_cast<std::size_t>(2));
    BENTO_CHECK_EQ(client.PendingRetransmits(), 1u);
    if (sent.size() == 2) {
        BENTO_CHECK(sent[0] == sent[1]);
    }
    client.Poll(3999);
    BENTO_CHECK_EQ(sent.size(), static_cast<std::size_t>(2));
    client.Poll(4000);  // 第 2 次重传，下一次 deadline = 4000 + 4000
    BENTO_CHECK_EQ(sent.size(), static_cast<std::size_t>(3));
    BENTO_CHECK_EQ(client.PendingRetransmits(), 2u);
    if (sent.size() == 3) {
        BENTO_CHECK(sent[0] == sent[2]);
    }
    client.Poll(7999);
    BENTO_CHECK(client.HasPending());
    client.Poll(8000);  // 重传耗尽 -> kTimeout
    BENTO_CHECK(!client.HasPending());
    BENTO_CHECK_EQ(events.size(), static_cast<std::size_t>(1));
    if (events.size() == 1) {
        BENTO_CHECK(events[0] == CoapClientEvent::kTimeout);
    }
    if (!sent.empty()) {
        BENTO_CHECK_EQ(DecodeOk(sent[0]).message_id, client.LastMessageId());
    }
    client.Poll(1000000);
    BENTO_CHECK_EQ(sent.size(), static_cast<std::size_t>(3));
}

void TestClientEmptyAckAndSeparateResponse() {
    CoapClientConfig config;
    config.ack_timeout_ms = 500;
    config.max_retransmit = 3;
    std::vector<std::vector<std::uint8_t>> sent;
    CoapClient client([&sent](const std::vector<std::uint8_t>& wire) {
        sent.push_back(wire);
        return true;
    }, config);
    std::vector<CoapClientEvent> events;
    client.OnEvent([&events](CoapClientEvent event, const CoapMessage&) {
        events.push_back(event);
    });

    CoapRequest request;
    request.method = CoapCode::kGet;
    request.uri_path = "/separate";
    BENTO_CHECK(client.Send(request, 5000));
    BENTO_CHECK_EQ(sent.size(), static_cast<std::size_t>(1));
    const CoapMessage sent_request = DecodeOk(sent[0]);

    // 空 ACK：只停止重传，pending 保留等待 separate response。
    BENTO_CHECK(client.HandleDatagram(MakeResponseWire(
        CoapType::kAck, CoapCode::kEmpty, sent_request.message_id, {})));
    BENTO_CHECK(client.LastStatus() == CoapClientStatus::kOk);
    BENTO_CHECK(client.HasPending());
    client.Poll(5000000);
    BENTO_CHECK_EQ(sent.size(), static_cast<std::size_t>(1));

    // separate CON 响应：客户端回空 ACK 并交付 kResponse。
    BENTO_CHECK(client.HandleDatagram(MakeResponseWire(
        CoapType::kCon, CoapCode::kContent, 0x7777, sent_request.token,
        {'h', 'i'})));
    BENTO_CHECK(!client.HasPending());
    BENTO_CHECK_EQ(events.size(), static_cast<std::size_t>(1));
    if (events.size() == 1) {
        BENTO_CHECK(events[0] == CoapClientEvent::kResponse);
    }
    BENTO_CHECK_EQ(sent.size(), static_cast<std::size_t>(2));
    if (sent.size() == 2) {
        const CoapMessage ack = DecodeOk(sent[1]);
        BENTO_CHECK(ack.type == CoapType::kAck);
        BENTO_CHECK(ack.code == CoapCode::kEmpty);
        BENTO_CHECK_EQ(ack.message_id, static_cast<std::uint16_t>(0x7777));
    }
}

void TestClientResetAndIgnored() {
    std::vector<std::vector<std::uint8_t>> sent;
    CoapClient client([&sent](const std::vector<std::uint8_t>& wire) {
        sent.push_back(wire);
        return true;
    });
    std::vector<CoapClientEvent> events;
    client.OnEvent([&events](CoapClientEvent event, const CoapMessage&) {
        events.push_back(event);
    });
    BENTO_CHECK(client.SendRequest(CoapCode::kGet, "/rst", {}, std::nullopt, 1000));
    if (sent.empty()) {
        BENTO_CHECK(false);
        return;
    }
    const CoapMessage request = DecodeOk(sent[0]);

    // MID 不匹配的空 ACK：忽略，pending 保持。
    BENTO_CHECK(!client.HandleDatagram(MakeResponseWire(
        CoapType::kAck, CoapCode::kEmpty,
        static_cast<std::uint16_t>(request.message_id + 1), {})));
    BENTO_CHECK(client.LastStatus() == CoapClientStatus::kIgnored);
    BENTO_CHECK(client.HasPending());

    // Token 不匹配的 piggyback 响应：忽略。
    BENTO_CHECK(!client.HandleDatagram(MakeResponseWire(
        CoapType::kAck, CoapCode::kContent, request.message_id, {0xff, 0xee})));
    BENTO_CHECK(client.LastStatus() == CoapClientStatus::kIgnored);
    BENTO_CHECK(client.HasPending());

    // 匹配 MID 的 RST：终止并触发 kReset。
    BENTO_CHECK(client.HandleDatagram(MakeResponseWire(
        CoapType::kRst, CoapCode::kEmpty, request.message_id, {})));
    BENTO_CHECK(!client.HasPending());
    BENTO_CHECK_EQ(events.size(), static_cast<std::size_t>(1));
    if (events.size() == 1) {
        BENTO_CHECK(events[0] == CoapClientEvent::kReset);
    }

    // 没有 pending 时的入站报文。
    BENTO_CHECK(!client.HandleDatagram(MakeResponseWire(
        CoapType::kCon, CoapCode::kContent, 0x1234, {})));
    BENTO_CHECK(client.LastStatus() == CoapClientStatus::kNoPending);

    // 解码失败。
    BENTO_CHECK(!client.HandleDatagram(Bytes({0x40, 0x01})));
    BENTO_CHECK(client.LastStatus() == CoapClientStatus::kDecodeError);
}

void TestClientErrorsAndClamps() {
    CoapClient no_send;
    BENTO_CHECK(!no_send.SendRequest(CoapCode::kGet, "/x"));
    BENTO_CHECK(no_send.LastStatus() == CoapClientStatus::kSendFailed);

    std::size_t sends = 0;
    CoapClient client([&sends](const std::vector<std::uint8_t>&) {
        ++sends;
        return true;
    });
    BENTO_CHECK(!client.SendRequest(CoapCode::kContent, "/x"));
    BENTO_CHECK(client.LastStatus() == CoapClientStatus::kEncodeError);

    CoapRequest ack_request;
    ack_request.method = CoapCode::kGet;
    ack_request.type = CoapType::kAck;
    BENTO_CHECK(!client.Send(ack_request, 1));
    BENTO_CHECK(client.LastStatus() == CoapClientStatus::kEncodeError);

    CoapRequest big_token;
    big_token.method = CoapCode::kGet;
    big_token.token.assign(kCoapMaxTokenBytes + 1, 0x01);
    BENTO_CHECK(!client.Send(big_token, 1));
    BENTO_CHECK(client.LastStatus() == CoapClientStatus::kEncodeError);
    BENTO_CHECK_EQ(sends, static_cast<std::size_t>(0));

    CoapClient failing([](const std::vector<std::uint8_t>&) { return false; });
    BENTO_CHECK(!failing.SendRequest(CoapCode::kGet, "/x", {}, std::nullopt, 1000));
    BENTO_CHECK(failing.LastStatus() == CoapClientStatus::kSendFailed);
    BENTO_CHECK(!failing.HasPending());

    std::size_t busy_sends = 0;
    CoapClient busy([&busy_sends](const std::vector<std::uint8_t>&) {
        ++busy_sends;
        return true;
    });
    BENTO_CHECK(busy.SendRequest(CoapCode::kGet, "/a", {}, std::nullopt, 1000));
    BENTO_CHECK(!busy.SendRequest(CoapCode::kGet, "/b", {}, std::nullopt, 1000));
    BENTO_CHECK(busy.LastStatus() == CoapClientStatus::kBusy);
    BENTO_CHECK_EQ(busy_sends, static_cast<std::size_t>(1));

    // 清除发送回调后 Poll 不得抛 std::bad_function_call。
    busy.SetSendFn({});
    busy.Poll(1000000);
    BENTO_CHECK(busy.HasPending());

    // token_bytes 夹取到 1..8。
    std::vector<std::vector<std::uint8_t>> narrow;
    CoapClientConfig narrow_config;
    narrow_config.token_bytes = 0;
    CoapClient narrow_client([&narrow](const std::vector<std::uint8_t>& wire) {
        narrow.push_back(wire);
        return true;
    }, narrow_config);
    BENTO_CHECK(narrow_client.SendRequest(CoapCode::kGet, "/t", {}, std::nullopt,
                                          1000));
    if (!narrow.empty()) {
        BENTO_CHECK_EQ(DecodeOk(narrow[0]).token.size(),
                       static_cast<std::size_t>(1));
    }

    std::vector<std::vector<std::uint8_t>> wide;
    CoapClientConfig wide_config;
    wide_config.token_bytes = 99;
    CoapClient wide_client([&wide](const std::vector<std::uint8_t>& wire) {
        wide.push_back(wire);
        return true;
    }, wide_config);
    BENTO_CHECK(wide_client.SendRequest(CoapCode::kGet, "/t", {}, std::nullopt,
                                        1000));
    if (!wide.empty()) {
        BENTO_CHECK_EQ(DecodeOk(wide[0]).token.size(),
                       static_cast<std::size_t>(8));
    }

    // ack_timeout_ms == 0 会被提升到 1ms。
    std::size_t retried = 0;
    CoapClientConfig fast_config;
    fast_config.ack_timeout_ms = 0;
    fast_config.max_retransmit = 1;
    CoapClient fast([&retried](const std::vector<std::uint8_t>&) {
        ++retried;
        return true;
    }, fast_config);
    BENTO_CHECK(fast.SendRequest(CoapCode::kGet, "/f", {}, std::nullopt, 1000));
    fast.Poll(1000);
    BENTO_CHECK_EQ(retried, static_cast<std::size_t>(1));
    fast.Poll(1001);
    BENTO_CHECK_EQ(retried, static_cast<std::size_t>(2));
}

void TestClientDeterminism() {
    std::vector<std::vector<std::uint8_t>> first_sent;
    std::vector<std::vector<std::uint8_t>> second_sent;
    CoapClient first([&first_sent](const std::vector<std::uint8_t>& wire) {
        first_sent.push_back(wire);
        return true;
    });
    CoapClient second([&second_sent](const std::vector<std::uint8_t>& wire) {
        second_sent.push_back(wire);
        return true;
    });
    BENTO_CHECK(first.SendRequest(CoapCode::kGet, "/d", {}, std::nullopt, 1000));
    BENTO_CHECK(second.SendRequest(CoapCode::kGet, "/d", {}, std::nullopt, 1000));
    BENTO_CHECK(first_sent.size() == 1);
    BENTO_CHECK(second_sent.size() == 1);
    if (!first_sent.empty() && !second_sent.empty()) {
        BENTO_CHECK(first_sent[0] == second_sent[0]);
    }
}

void TestNonConfirmableExchange() {
    Loopback loop;
    CoapRequest request;
    request.method = CoapCode::kGet;
    request.type = CoapType::kNon;
    request.uri_path = "/stats";
    BENTO_CHECK(loop.client.Send(request, 1000));
    BENTO_CHECK_EQ(loop.from_client.size(), static_cast<std::size_t>(1));
    BENTO_CHECK_EQ(loop.from_server.size(), static_cast<std::size_t>(1));
    BENTO_CHECK(!loop.client.HasPending());

    if (loop.from_client.empty() || loop.from_server.empty()) {
        BENTO_CHECK(false);
        return;
    }
    const CoapMessage sent = DecodeOk(loop.from_client[0]);
    BENTO_CHECK(sent.type == CoapType::kNon);
    const CoapMessage reply = DecodeOk(loop.from_server[0]);
    BENTO_CHECK(reply.type == CoapType::kNon);
    BENTO_CHECK(reply.code == CoapCode::kContent);
    BENTO_CHECK(reply.token == sent.token);

    // 第二条 NON：服务端必须分配新的 Message ID 与 Token。
    CoapRequest second = request;
    second.uri_path = "/stats2";
    BENTO_CHECK(loop.client.Send(second, 2000));
    BENTO_CHECK_EQ(loop.from_server.size(), static_cast<std::size_t>(2));
    if (loop.from_server.size() == 2) {
        const CoapMessage first_reply = DecodeOk(loop.from_server[0]);
        const CoapMessage second_reply = DecodeOk(loop.from_server[1]);
        BENTO_CHECK(first_reply.message_id != second_reply.message_id);
        BENTO_CHECK(first_reply.token != second_reply.token);
    }

    // RFC 7252 §4.3：NON 不重传；到点直接按 kTimeout 收尾。
    std::vector<std::vector<std::uint8_t>> sent_wire;
    CoapClientConfig config;
    config.ack_timeout_ms = 100;
    config.max_retransmit = 3;
    CoapClient client([&sent_wire](const std::vector<std::uint8_t>& wire) {
        sent_wire.push_back(wire);
        return true;
    }, config);
    std::vector<CoapClientEvent> events;
    client.OnEvent([&events](CoapClientEvent event, const CoapMessage&) {
        events.push_back(event);
    });
    CoapRequest non;
    non.method = CoapCode::kGet;
    non.type = CoapType::kNon;
    non.uri_path = "/non";
    BENTO_CHECK(client.Send(non, 1000));
    client.Poll(1100);
    BENTO_CHECK_EQ(sent_wire.size(), static_cast<std::size_t>(1));
    BENTO_CHECK(!client.HasPending());
    BENTO_CHECK_EQ(events.size(), static_cast<std::size_t>(1));
    if (events.size() == 1) {
        BENTO_CHECK(events[0] == CoapClientEvent::kTimeout);
    }
}

// ---------------------------------------------------------------------------
// 随机 / 对抗 / 压测
// ---------------------------------------------------------------------------

void TestRandomRoundtripFuzz() {
    Rng rng;
    const std::uint16_t interesting_numbers[] = {
        0,  1,  3,  4,  5,   6,   7,   8,   11,  12,  14,
        15, 17, 20, 23, 27,  28,  35,  39,  60,  268, 269,
        65535};
    const std::uint32_t interesting_lengths[] = {0, 1, 12, 13, 268, 269, 300};
    constexpr std::size_t kNumberCount =
        sizeof(interesting_numbers) / sizeof(interesting_numbers[0]);
    constexpr std::size_t kLengthCount =
        sizeof(interesting_lengths) / sizeof(interesting_lengths[0]);

    for (int iteration = 0; iteration < 2000; ++iteration) {
        CoapMessage message;
        message.type = static_cast<CoapType>(rng.Below(4));
        message.code = static_cast<CoapCode>(rng.Below(256));
        message.message_id = static_cast<std::uint16_t>(rng.Next());
        const std::size_t token_bytes = static_cast<std::size_t>(rng.Below(9));
        message.token.resize(token_bytes);
        for (std::uint8_t& byte : message.token) {
            byte = static_cast<std::uint8_t>(rng.Next());
        }
        const std::size_t option_count = static_cast<std::size_t>(rng.Below(6));
        for (std::size_t index = 0; index < option_count; ++index) {
            const std::uint16_t number = interesting_numbers
                [static_cast<std::size_t>(rng.Below(kNumberCount))];
            const std::uint32_t length = interesting_lengths
                [static_cast<std::size_t>(rng.Below(kLengthCount))];
            std::vector<std::uint8_t> value(length);
            for (std::uint8_t& byte : value) {
                byte = static_cast<std::uint8_t>(rng.Next());
            }
            message.options.push_back(CoapOption{number, std::move(value)});
        }
        if (rng.Below(2) == 0) {
            const std::size_t payload_bytes =
                static_cast<std::size_t>(rng.Below(200));
            message.payload.resize(payload_bytes);
            for (std::uint8_t& byte : message.payload) {
                byte = static_cast<std::uint8_t>(rng.Next());
            }
        }

        const std::vector<std::uint8_t> wire = EncodeOk(message);
        const CoapMessage decoded = DecodeOk(wire);
        BENTO_CHECK(decoded.type == message.type);
        BENTO_CHECK(decoded.code == message.code);
        BENTO_CHECK_EQ(decoded.message_id, message.message_id);
        BENTO_CHECK(decoded.token == message.token);
        BENTO_CHECK(decoded.payload == message.payload);
        BENTO_CHECK_EQ(decoded.options.size(), message.options.size());
        BENTO_CHECK(EncodeOk(decoded) == wire);
        for (std::size_t index = 1; index < decoded.options.size(); ++index) {
            BENTO_CHECK(decoded.options[index - 1].number <=
                        decoded.options[index].number);
        }
    }
}

void TestMutatedAndTruncatedFuzz() {
    CoapMessage base;
    base.type = CoapType::kCon;
    base.code = CoapCode::kPost;
    base.message_id = 0xbeef;
    base.token = {0xde, 0xad, 0xbe, 0xef};
    base.AddUriPath("/devices/42/state");
    base.AddUintOption(kCoapOptionContentFormat, kCoapContentFormatJson);
    base.AddStringOption(kCoapOptionUriQuery, "a=1&b=2");
    base.payload = {'{', '"', 'o', 'n', '"', ':', 't', 'r', 'u', 'e', '}'};
    const std::vector<std::uint8_t> wire = EncodeOk(base);
    BENTO_CHECK(wire.size() > 20);

    Rng rng;
    std::size_t accepted = 0;
    for (int iteration = 0; iteration < 4000; ++iteration) {
        std::vector<std::uint8_t> mutated = wire;
        const std::size_t flips = 1 + static_cast<std::size_t>(rng.Below(3));
        for (std::size_t flip = 0; flip < flips; ++flip) {
            const std::size_t index =
                static_cast<std::size_t>(rng.Below(mutated.size()));
            mutated[index] = static_cast<std::uint8_t>(rng.Next());
        }
        CoapMessage decoded;
        if (DecodeCoapMessage(mutated, &decoded) != CoapStatus::kOk) {
            continue;
        }
        ++accepted;
        // 任何能解出来的帧都必须能重新编码并稳定往返。
        CoapStatus reencode = CoapStatus::kInvalidArgument;
        const std::vector<std::uint8_t> again = EncodeCoapMessage(decoded, &reencode);
        BENTO_CHECK(reencode == CoapStatus::kOk);
        CoapMessage roundtrip;
        BENTO_CHECK(DecodeCoapMessage(again, &roundtrip) == CoapStatus::kOk);
        BENTO_CHECK(EncodeOk(roundtrip) == again);
    }
    BENTO_CHECK(accepted > 100);

    // 任意前缀只能是「截断」或「Payload Marker 后无载荷」，绝不能是 ok 的残缺帧。
    for (std::size_t length = 0; length < wire.size(); ++length) {
        CoapMessage decoded;
        const CoapStatus status = DecodeCoapMessage(wire.data(), length, &decoded);
        if (status != CoapStatus::kOk) {
            BENTO_CHECK(status == CoapStatus::kTruncated ||
                        status == CoapStatus::kPayloadMarkerWithoutPayload);
        }
    }
}

void TestCodecStress() {
    CoapMessage message;
    message.type = CoapType::kNon;
    message.code = CoapCode::kPost;
    message.message_id = 0x1234;
    message.token = {1, 2, 3, 4, 5, 6, 7, 8};
    message.AddUriPath("/v1/devices/42/state");
    message.AddUintOption(kCoapOptionUriPort, 5683);
    message.AddContentFormat(kCoapContentFormatJson);
    message.AddStringOption(kCoapOptionUriQuery, "pretty=1");
    message.payload.assign(64, 0x7b);
    const std::vector<std::uint8_t> wire = EncodeOk(message);

    constexpr int kIterations = 20000;
    const std::uint64_t start = CoapNowMs();
    std::size_t total_bytes = 0;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        CoapMessage decoded;
        if (DecodeCoapMessage(wire, &decoded) != CoapStatus::kOk) {
            BENTO_CHECK(false);
            break;
        }
        const std::vector<std::uint8_t> again = EncodeCoapMessage(decoded, nullptr);
        total_bytes += again.size();
    }
    const std::uint64_t elapsed = CoapNowMs() - start;
    std::printf("[INFO] 编解码压测：%d 轮, 累计 %zu 字节, 耗时 %llu ms\n", kIterations,
                total_bytes, static_cast<unsigned long long>(elapsed));
    BENTO_CHECK(total_bytes > 0);
    BENTO_CHECK(elapsed < 10000);
    BENTO_CHECK(CoapNowMs() >= start);
}

void TestNameAndCodeHelpers() {
    BENTO_CHECK(std::strcmp(CoapTypeName(CoapType::kCon), "CON") == 0);
    BENTO_CHECK(std::strcmp(CoapTypeName(CoapType::kNon), "NON") == 0);
    BENTO_CHECK(std::strcmp(CoapTypeName(CoapType::kAck), "ACK") == 0);
    BENTO_CHECK(std::strcmp(CoapTypeName(CoapType::kRst), "RST") == 0);
    BENTO_CHECK(std::strcmp(CoapTypeName(static_cast<CoapType>(9)), "unknown") == 0);

    BENTO_CHECK(std::strcmp(CoapCodeName(CoapCode::kContent), "2.05 Content") == 0);
    BENTO_CHECK(std::strcmp(CoapCodeName(CoapCode::kGet), "0.01 GET") == 0);
    BENTO_CHECK(std::strcmp(CoapCodeName(static_cast<CoapCode>(0x1f)), "unknown") ==
                0);

    BENTO_CHECK(std::strcmp(CoapStatusName(CoapStatus::kOk), "ok") == 0);
    BENTO_CHECK(std::strcmp(CoapStatusName(static_cast<CoapStatus>(99)), "unknown") ==
                0);

    BENTO_CHECK_EQ(CoapCodeClass(CoapCode::kContent), static_cast<std::uint8_t>(2));
    BENTO_CHECK_EQ(CoapCodeDetail(CoapCode::kContent),
                   static_cast<std::uint8_t>(5));
    BENTO_CHECK(IsCoapRequestCode(CoapCode::kGet));
    BENTO_CHECK(IsCoapRequestCode(CoapCode::kDelete));
    BENTO_CHECK(!IsCoapRequestCode(CoapCode::kContent));
    BENTO_CHECK(!IsCoapRequestCode(static_cast<CoapCode>(0x05)));
    BENTO_CHECK(IsCoapSuccessCode(CoapCode::kContent));
    BENTO_CHECK(!IsCoapSuccessCode(CoapCode::kNotFound));
    BENTO_CHECK(IsCoapResponseCode(CoapCode::kContent));
    BENTO_CHECK(IsCoapResponseCode(CoapCode::kNotFound));
    BENTO_CHECK(IsCoapResponseCode(CoapCode::kInternalServerError));
    BENTO_CHECK(!IsCoapResponseCode(static_cast<CoapCode>(0x60)));  // 3.xx 保留
    BENTO_CHECK(!IsCoapResponseCode(CoapCode::kGet));
}

}  // namespace

int main() {
    TestKnownWireVector();
    TestRoundtripMatrix();
    TestOptionDeltaLengthBoundaries();
    TestOptionOrderingAndHelpers();
    TestUintAndStringOptions();
    TestEncodeArgumentErrors();
    TestDecodeMalformedFrames();
    TestMessageByteLimitBoundary();
    TestClientServerExchange();
    TestServerDefaultResponses();
    TestServerCustomHandler();
    TestServerDedup();
    TestServerDrops();
    TestServerHandlerContract();
    TestClientRetransmissionTimeline();
    TestClientEmptyAckAndSeparateResponse();
    TestClientResetAndIgnored();
    TestClientErrorsAndClamps();
    TestClientDeterminism();
    TestNonConfirmableExchange();
    TestRandomRoundtripFuzz();
    TestMutatedAndTruncatedFuzz();
    TestCodecStress();
    TestNameAndCodeHelpers();
    return bento_test::Finish("test_coap");
}
