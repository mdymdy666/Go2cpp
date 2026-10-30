/**
 * @file test_mqtt.cpp
 * @brief MQTT 3.1.1 骨架测试：VLQ 边界、九种报文往返、粘包/半包、非法帧、
 *        QoS1 确认、keepalive、订阅路由、遗嘱、5 个真实场景、压测与随机 fuzz。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/mqtt.hpp"

namespace {

using namespace go2cpp::bento::net;

int g_failures = 0;
int g_checks = 0;
int g_scenarios = 0;

#define MQTT_CHECK(condition)                                                          \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

#define MQTT_CHECK_EQ(actual, expected)                                                     \
    do {                                                                                    \
        ++g_checks;                                                                         \
        if (!((actual) == (expected))) {                                                    \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual,  \
                         #expected);                                                        \
            ++g_failures;                                                                   \
        }                                                                                   \
    } while (0)

/// 确定性 xorshift：随机测试可复现。
std::uint64_t g_rng = 0x9e3779b97f4a7c15ull;

std::uint32_t Rand() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<std::uint32_t>(g_rng >> 32);
}

std::vector<std::uint8_t> Bytes(std::initializer_list<std::uint8_t> values) {
    return std::vector<std::uint8_t>(values);
}

std::vector<std::uint8_t> Concat(const std::vector<std::vector<std::uint8_t>>& parts) {
    std::vector<std::uint8_t> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

std::string PayloadString(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> ToBytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

MqttConnectOptions BaseConnect(const std::string& client_id, std::uint16_t keepalive) {
    MqttConnectOptions options;
    options.client_id = client_id;
    options.keepalive_seconds = keepalive;
    options.clean_session = true;
    return options;
}

MqttPublish BasePublish(const std::string& topic, const std::string& payload, std::uint8_t qos,
                        std::uint16_t packet_id) {
    MqttPublish publish;
    publish.topic = topic;
    publish.payload = ToBytes(payload);
    publish.qos = qos;
    publish.packet_id = packet_id;
    return publish;
}

// ---------------------------------------------------------------------------
// 1. VLQ 边界与非法编码
// ---------------------------------------------------------------------------

void TestVlqBoundaries() {
    const std::uint32_t values[] = {0,     1,      127,     128,      16383,
                                    16384, 2097151, 2097152, 268435455};
    const std::size_t expected_bytes[] = {1, 1, 1, 2, 2, 3, 3, 4, 4};
    for (std::size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        std::uint8_t encoded[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        const std::size_t count = EncodeRemainingLength(values[i], encoded);
        MQTT_CHECK_EQ(count, expected_bytes[i]);
        std::uint32_t decoded = 0;
        std::size_t consumed = 0;
        MQTT_CHECK_EQ(DecodeRemainingLength(encoded, count, &decoded, &consumed),
                      MqttStatus::kOk);
        MQTT_CHECK_EQ(decoded, values[i]);
        MQTT_CHECK_EQ(consumed, count);
        // 追加垃圾字节不应影响解码，也不能多消费。
        std::uint8_t padded[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        std::memcpy(padded, encoded, count);
        MQTT_CHECK_EQ(DecodeRemainingLength(padded, count + 2, &decoded, &consumed),
                      MqttStatus::kOk);
        MQTT_CHECK_EQ(decoded, values[i]);
        MQTT_CHECK_EQ(consumed, count);
    }
    // 超限值不可编码。
    std::uint8_t encoded[4] = {0, 0, 0, 0};
    MQTT_CHECK_EQ(EncodeRemainingLength(268435456u, encoded), 0U);
    MQTT_CHECK_EQ(EncodeRemainingLength(0xFFFFFFFFu, encoded), 0U);
    MQTT_CHECK_EQ(EncodeRemainingLength(0, nullptr), 0U);
}

void TestVlqDecodeFailure() {
    std::uint32_t value = 0;
    std::size_t consumed = 0;
    MQTT_CHECK_EQ(DecodeRemainingLength(nullptr, 0, &value, &consumed),
                  MqttStatus::kNeedMoreData);
    const std::uint8_t empty[1] = {0};
    MQTT_CHECK_EQ(DecodeRemainingLength(empty, 0, &value, &consumed),
                  MqttStatus::kNeedMoreData);
    // 只有继续位、没有终止字节：半包。
    const std::uint8_t partial[2] = {0x80, 0x80};
    MQTT_CHECK_EQ(DecodeRemainingLength(partial, 2, &value, &consumed),
                  MqttStatus::kNeedMoreData);
    // 第 4 字节仍带继续位：超过 4 字节，格式非法。
    const std::uint8_t overflow[5] = {0x80, 0x80, 0x80, 0x80, 0x01};
    MQTT_CHECK_EQ(DecodeRemainingLength(overflow, 5, &value, &consumed),
                  MqttStatus::kMalformed);
    MQTT_CHECK_EQ(DecodeRemainingLength(overflow, 4, &value, &consumed),
                  MqttStatus::kMalformed);
    // 允许 nullptr 输出参数：只做校验。
    MQTT_CHECK_EQ(DecodeRemainingLength(overflow, 3, nullptr, nullptr),
                  MqttStatus::kNeedMoreData);
}

// ---------------------------------------------------------------------------
// 2. CONNECT / CONNACK
// ---------------------------------------------------------------------------

void TestConnectRoundtrip() {
    // 最小 CONNECT：空 client_id、keepalive 0、clean session。
    MqttConnectOptions minimal;
    minimal.client_id.clear();
    const std::vector<std::uint8_t> minimal_bytes = EncodeConnect(minimal);
    MQTT_CHECK(!minimal_bytes.empty());
    MqttPacket decoded;
    std::size_t consumed = 0;
    MQTT_CHECK_EQ(DecodePacket(minimal_bytes, &decoded, &consumed), MqttStatus::kOk);
    MQTT_CHECK_EQ(consumed, minimal_bytes.size());
    MQTT_CHECK_EQ(decoded.type, MqttPacketType::kConnect);
    MQTT_CHECK_EQ(decoded.flags, 0);
    MQTT_CHECK_EQ(decoded.connect.protocol_name, std::string("MQTT"));
    MQTT_CHECK_EQ(decoded.connect.protocol_level, kMqttProtocolLevel);
    MQTT_CHECK_EQ(decoded.connect.client_id, std::string());
    MQTT_CHECK_EQ(decoded.connect.keepalive_seconds, 0);
    MQTT_CHECK(decoded.connect.clean_session);
    MQTT_CHECK(!decoded.connect.has_will);
    MQTT_CHECK(!decoded.connect.has_username);

    // 完整 CONNECT：遗嘱（QoS1 + retain）、用户名、二进制密码。
    MqttConnectOptions full = BaseConnect("device-42", 30);
    full.clean_session = false;
    full.has_will = true;
    full.will_topic = "clients/device-42/status";
    full.will_message = Bytes({'o', 'f', 'f', 'l', 'i', 'n', 'e', 0x00, 0xFF});
    full.will_qos = 1;
    full.will_retain = true;
    full.has_username = true;
    full.username = "user";
    full.has_password = true;
    full.password = Bytes({'p', 'w', 'd', 0x00});
    const std::vector<std::uint8_t> full_bytes = EncodeConnect(full);
    MQTT_CHECK(!full_bytes.empty());
    MqttPacket full_decoded;
    MQTT_CHECK_EQ(DecodePacket(full_bytes, &full_decoded, &consumed), MqttStatus::kOk);
    MQTT_CHECK_EQ(consumed, full_bytes.size());
    MQTT_CHECK_EQ(full_decoded.connect.client_id, std::string("device-42"));
    MQTT_CHECK_EQ(full_decoded.connect.keepalive_seconds, 30);
    MQTT_CHECK(!full_decoded.connect.clean_session);
    MQTT_CHECK(full_decoded.connect.has_will);
    MQTT_CHECK_EQ(full_decoded.connect.will_topic, std::string("clients/device-42/status"));
    MQTT_CHECK_EQ(PayloadString(full_decoded.connect.will_message),
                  PayloadString(full.will_message));
    MQTT_CHECK_EQ(full_decoded.connect.will_qos, 1);
    MQTT_CHECK(full_decoded.connect.will_retain);
    MQTT_CHECK(full_decoded.connect.has_username);
    MQTT_CHECK_EQ(full_decoded.connect.username, std::string("user"));
    MQTT_CHECK(full_decoded.connect.has_password);
    MQTT_CHECK_EQ(PayloadString(full_decoded.connect.password), PayloadString(full.password));
}

void TestConnectEncodingRejects() {
    MqttConnectOptions options = BaseConnect("id", 0);
    options.protocol_name = "MQIsdp";
    MQTT_CHECK(EncodeConnect(options).empty());

    options = BaseConnect("id", 0);
    options.has_password = true;
    options.password = ToBytes("secret");
    MQTT_CHECK(EncodeConnect(options).empty()); // 密码不能脱离用户名。

    options = BaseConnect("id", 0);
    options.will_topic = "will/topic"; // has_will 为 false 却给了遗嘱字段。
    MQTT_CHECK(EncodeConnect(options).empty());

    options = BaseConnect("id", 0);
    options.has_will = true;
    options.will_topic = "will/+";
    MQTT_CHECK(EncodeConnect(options).empty()); // 遗嘱主题不能含通配符。

    options = BaseConnect("id", 0);
    options.has_will = true;
    options.will_topic = "will/topic";
    options.will_qos = 3;
    MQTT_CHECK(EncodeConnect(options).empty());

    options = BaseConnect("id", 0);
    options.has_will = true;
    options.will_topic = "will/topic";
    options.will_message.resize(70000);
    MQTT_CHECK(EncodeConnect(options).empty()); // 超过 16 位长度。

    options = BaseConnect(std::string("a\0b", 3), 0);
    MQTT_CHECK(EncodeConnect(options).empty()); // MQTT 字符串禁止内嵌 NUL。

    // 非 3.1.1 的协议级别允许编码，便于测试服务端拒绝路径。
    options = BaseConnect("id", 0);
    options.protocol_level = 3;
    MQTT_CHECK(!EncodeConnect(options).empty());
}

void TestConnectDecodeRejects() {
    // 协议名必须是 "MQTT"。
    std::vector<std::uint8_t> wrong_name =
        Bytes({0x10, 0x0C, 0x00, 0x04, 'M', 'Q', 'I', 's', 0x04, 0x02, 0x00, 0x00, 0x00, 0x00});
    MqttPacket packet;
    MQTT_CHECK_EQ(DecodePacket(wrong_name, &packet), MqttStatus::kMalformed);

    // 保留位（bit0）非 0。
    std::vector<std::uint8_t> connect = EncodeConnect(BaseConnect("id", 0));
    const std::size_t flags_offset = 2 + 6 + 1; // 固定头 2 + 协议名(2+4) + 级别 1。
    MQTT_CHECK(connect.size() > flags_offset);
    connect[flags_offset] = static_cast<std::uint8_t>(connect[flags_offset] | 0x01u);
    MQTT_CHECK_EQ(DecodePacket(connect, &packet), MqttStatus::kMalformed);

    // 尾部多余字节。
    connect = EncodeConnect(BaseConnect("id", 0));
    MQTT_CHECK(connect.size() > 3);
    // 直接追加 0 字节不会进入该包（解码按剩余长度切分），因此改成手工加长。
    std::vector<std::uint8_t> trailing = connect;
    trailing.push_back(0x00);
    trailing[1] = static_cast<std::uint8_t>(connect[1] + 1); // 小包 remaining 仍是单字节。
    MQTT_CHECK_EQ(DecodePacket(trailing, &packet), MqttStatus::kMalformed);

    // 固定头 flags 非 0。
    connect = EncodeConnect(BaseConnect("id", 0));
    connect[0] = 0x11;
    MQTT_CHECK_EQ(DecodePacket(connect, &packet), MqttStatus::kMalformed);

    // 截断：只给一半。
    connect = EncodeConnect(BaseConnect("id", 0));
    MQTT_CHECK_EQ(DecodePacket(connect.data(), connect.size() / 2, &packet),
                  MqttStatus::kNeedMoreData);
    // 空指针 / 空输出。
    MQTT_CHECK_EQ(DecodePacket(nullptr, 0, &packet), MqttStatus::kNeedMoreData);
    MQTT_CHECK_EQ(DecodePacket(nullptr, 0, nullptr), MqttStatus::kInvalidArgument);
}

void TestConnackRoundtrip() {
    MqttPacket packet;
    std::size_t consumed = 0;
    const std::vector<std::uint8_t> accepted = EncodeConnack(MqttConnack{true, 0});
    MQTT_CHECK_EQ(accepted.size(), 4U);
    MQTT_CHECK_EQ(accepted[0], 0x20);
    MQTT_CHECK_EQ(accepted[1], 0x02);
    MQTT_CHECK_EQ(DecodePacket(accepted, &packet, &consumed), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.type, MqttPacketType::kConnack);
    MQTT_CHECK(packet.connack.session_present);
    MQTT_CHECK_EQ(packet.connack.return_code, 0);
    MQTT_CHECK_EQ(consumed, 4U);

    for (std::uint8_t code = 1; code <= 5; ++code) {
        const std::vector<std::uint8_t> rejected = EncodeConnack(MqttConnack{false, code});
        MQTT_CHECK(!rejected.empty());
        MQTT_CHECK_EQ(DecodePacket(rejected, &packet), MqttStatus::kOk);
        MQTT_CHECK_EQ(packet.connack.return_code, code);
        MQTT_CHECK(!packet.connack.session_present);
        // 拒绝码与 session present 同时出现是非法组合。
        MQTT_CHECK(EncodeConnack(MqttConnack{true, code}).empty());
    }
    MQTT_CHECK(EncodeConnack(MqttConnack{false, 6}).empty());

    // 解码非法 CONNACK：ack flags 保留位、返回码越界、长度不是 2。
    std::vector<std::uint8_t> bad = Bytes({0x20, 0x02, 0x02, 0x00});
    MQTT_CHECK_EQ(DecodePacket(bad, &packet), MqttStatus::kMalformed);
    bad = Bytes({0x20, 0x02, 0x00, 0x06});
    MQTT_CHECK_EQ(DecodePacket(bad, &packet), MqttStatus::kMalformed);
    bad = Bytes({0x20, 0x02, 0x00});
    MQTT_CHECK_EQ(DecodePacket(bad, &packet), MqttStatus::kNeedMoreData);
    bad = Bytes({0x21, 0x02, 0x00, 0x00});
    MQTT_CHECK_EQ(DecodePacket(bad, &packet), MqttStatus::kMalformed);
}

// ---------------------------------------------------------------------------
// 3. PUBLISH / PUBACK / SUBSCRIBE / SUBACK / PING / DISCONNECT
// ---------------------------------------------------------------------------

void TestPublishQos0() {
    MqttPublish publish = BasePublish("sensors/temp", std::string("23.5\0C", 5), 0, 0);
    publish.retain = true;
    const std::vector<std::uint8_t> bytes = EncodePublish(publish);
    MQTT_CHECK(!bytes.empty());
    MqttPacket packet;
    std::size_t consumed = 0;
    MQTT_CHECK_EQ(DecodePacket(bytes, &packet, &consumed), MqttStatus::kOk);
    MQTT_CHECK_EQ(consumed, bytes.size());
    MQTT_CHECK_EQ(packet.type, MqttPacketType::kPublish);
    MQTT_CHECK_EQ(packet.publish.topic, std::string("sensors/temp"));
    MQTT_CHECK_EQ(PayloadString(packet.publish.payload),
                  PayloadString(publish.payload));
    MQTT_CHECK_EQ(packet.publish.qos, 0);
    MQTT_CHECK(!packet.publish.dup);
    MQTT_CHECK(packet.publish.retain);
    MQTT_CHECK_EQ(packet.publish.packet_id, 0);

    // 空载荷。
    const MqttPublish empty = BasePublish("a", "", 0, 0);
    MQTT_CHECK(!EncodePublish(empty).empty());
    MQTT_CHECK_EQ(DecodePacket(EncodePublish(empty), &packet), MqttStatus::kOk);
    MQTT_CHECK(packet.publish.payload.empty());

    // 编码拒绝。
    MqttPublish bad = BasePublish("a/+", "x", 0, 0);
    MQTT_CHECK(EncodePublish(bad).empty());
    bad = BasePublish("", "x", 0, 0);
    MQTT_CHECK(EncodePublish(bad).empty());
    bad = BasePublish("a", "x", 0, 7);
    MQTT_CHECK(EncodePublish(bad).empty());
    bad = BasePublish("a", "x", 0, 0);
    bad.dup = true;
    MQTT_CHECK(EncodePublish(bad).empty());
    bad = BasePublish("a", "x", 3, 1);
    MQTT_CHECK(EncodePublish(bad).empty());
}

void TestPublishQos1And2Wire() {
    MqttPublish publish = BasePublish("cmd/device", "reboot", 1, 4660);
    publish.dup = true;
    const std::vector<std::uint8_t> bytes = EncodePublish(publish);
    MQTT_CHECK(!bytes.empty());
    MqttPacket packet;
    MQTT_CHECK_EQ(DecodePacket(bytes, &packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.publish.qos, 1);
    MQTT_CHECK(packet.publish.dup);
    MQTT_CHECK_EQ(packet.publish.packet_id, 4660);
    MQTT_CHECK_EQ(PayloadString(packet.publish.payload), std::string("reboot"));

    // packet id 边界。
    MQTT_CHECK(!EncodePublish(BasePublish("a", "x", 1, 1)).empty());
    MQTT_CHECK(!EncodePublish(BasePublish("a", "x", 1, 65535)).empty());
    MQTT_CHECK(EncodePublish(BasePublish("a", "x", 1, 0)).empty());

    // QoS 2 线格式可编解码，会话层会明确拒绝。
    const MqttPublish qos2 = BasePublish("a", "x", 2, 9);
    const std::vector<std::uint8_t> qos2_bytes = EncodePublish(qos2);
    MQTT_CHECK(!qos2_bytes.empty());
    MQTT_CHECK_EQ(DecodePacket(qos2_bytes, &packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.publish.qos, 2);

    // 手工构造 QoS1 缺 packet id 的畸形帧。
    const std::vector<std::uint8_t> no_packet_id =
        Bytes({0x32, 0x04, 0x00, 0x01, 'a', 'b'});
    MQTT_CHECK_EQ(DecodePacket(no_packet_id, &packet), MqttStatus::kMalformed);
    // QoS 3 非法。
    const std::vector<std::uint8_t> qos3 = Bytes({0x36, 0x04, 0x00, 0x01, 'a', 0x00});
    MQTT_CHECK_EQ(DecodePacket(qos3, &packet), MqttStatus::kMalformed);
}

void TestPuback() {
    for (const std::uint16_t packet_id : {1, 255, 256, 65535}) {
        const std::vector<std::uint8_t> bytes = EncodePuback(packet_id);
        MQTT_CHECK_EQ(bytes.size(), 4U);
        MqttPacket packet;
        MQTT_CHECK_EQ(DecodePacket(bytes, &packet), MqttStatus::kOk);
        MQTT_CHECK_EQ(packet.type, MqttPacketType::kPuback);
        MQTT_CHECK_EQ(packet.packet_id, packet_id);
    }
    MQTT_CHECK(EncodePuback(0).empty());
    MqttPacket packet;
    MQTT_CHECK_EQ(DecodePacket(Bytes({0x40, 0x02, 0x00, 0x00}), &packet),
                  MqttStatus::kMalformed);
    MQTT_CHECK_EQ(DecodePacket(Bytes({0x41, 0x02, 0x00, 0x01}), &packet),
                  MqttStatus::kMalformed);
    MQTT_CHECK_EQ(DecodePacket(Bytes({0x40, 0x02, 0x00}), &packet),
                  MqttStatus::kNeedMoreData);
}

void TestSubscribeSuback() {
    std::vector<MqttSubscription> subscriptions{
        {"sensors/+/temp", 0}, {"alerts/#", 1}, {"full/topic", 2}};
    const std::vector<std::uint8_t> bytes = EncodeSubscribe(1234, subscriptions);
    MQTT_CHECK(!bytes.empty());
    MQTT_CHECK_EQ(bytes[0], 0x82); // SUBSCRIBE 固定头必须 0x2。
    MqttPacket packet;
    std::size_t consumed = 0;
    MQTT_CHECK_EQ(DecodePacket(bytes, &packet, &consumed), MqttStatus::kOk);
    MQTT_CHECK_EQ(consumed, bytes.size());
    MQTT_CHECK_EQ(packet.type, MqttPacketType::kSubscribe);
    MQTT_CHECK_EQ(packet.packet_id, 1234);
    MQTT_CHECK_EQ(packet.subscriptions.size(), 3U);
    MQTT_CHECK_EQ(packet.subscriptions[0].filter, std::string("sensors/+/temp"));
    MQTT_CHECK_EQ(packet.subscriptions[0].qos, 0);
    MQTT_CHECK_EQ(packet.subscriptions[1].filter, std::string("alerts/#"));
    MQTT_CHECK_EQ(packet.subscriptions[1].qos, 1);
    MQTT_CHECK_EQ(packet.subscriptions[2].qos, 2);

    // 编码拒绝。
    MQTT_CHECK(EncodeSubscribe(0, subscriptions).empty());
    MQTT_CHECK(EncodeSubscribe(1, {}).empty());
    MQTT_CHECK(EncodeSubscribe(1, {{"bad/#/x", 0}}).empty());
    MQTT_CHECK(EncodeSubscribe(1, {{"a", 3}}).empty());

    // 解码：标志必须 0x2、至少一个过滤器、qos <= 2。
    MQTT_CHECK_EQ(DecodePacket(Bytes({0x80, 0x06, 0x00, 0x01, 0x00, 0x01, 'a', 0x00}),
                              &packet),
                  MqttStatus::kMalformed);
    MQTT_CHECK_EQ(DecodePacket(Bytes({0x82, 0x02, 0x00, 0x01}), &packet),
                  MqttStatus::kMalformed);
    MQTT_CHECK_EQ(DecodePacket(Bytes({0x82, 0x06, 0x00, 0x01, 0x00, 0x01, 'a', 0x03}),
                              &packet),
                  MqttStatus::kMalformed);

    // SUBACK 返回码含失败码。
    const std::vector<std::uint8_t> suback = EncodeSuback(1234, {0, 1, 2, 0x80});
    MQTT_CHECK(!suback.empty());
    MQTT_CHECK_EQ(DecodePacket(suback, &packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.packet_id, 1234);
    MQTT_CHECK_EQ(packet.suback_codes.size(), 4U);
    MQTT_CHECK_EQ(packet.suback_codes[3], 0x80);
    MQTT_CHECK(EncodeSuback(0, {0}).empty());
    MQTT_CHECK(EncodeSuback(1, {}).empty());
    MQTT_CHECK(EncodeSuback(1, {3}).empty());
    MQTT_CHECK_EQ(DecodePacket(Bytes({0x90, 0x03, 0x00, 0x01, 0x7F}), &packet),
                  MqttStatus::kMalformed);
}

void TestPingDisconnectAndReservedTypes() {
    MqttPacket packet;
    std::size_t consumed = 0;
    const struct {
        std::vector<std::uint8_t> bytes;
        MqttPacketType type;
    } cases[] = {{EncodePingreq(), MqttPacketType::kPingreq},
                 {EncodePingresp(), MqttPacketType::kPingresp},
                 {EncodeDisconnect(), MqttPacketType::kDisconnect}};
    for (const auto& item : cases) {
        MQTT_CHECK_EQ(item.bytes.size(), 2U);
        MQTT_CHECK_EQ(DecodePacket(item.bytes, &packet, &consumed), MqttStatus::kOk);
        MQTT_CHECK_EQ(packet.type, item.type);
        MQTT_CHECK_EQ(consumed, 2U);
    }
    // 空包不允许带剩余长度或非零标志。
    MQTT_CHECK_EQ(DecodePacket(Bytes({0xC0, 0x01, 0x00}), &packet), MqttStatus::kMalformed);
    MQTT_CHECK_EQ(DecodePacket(Bytes({0xC1, 0x00}), &packet), MqttStatus::kMalformed);
    MQTT_CHECK_EQ(DecodePacket(Bytes({0xE0, 0x01, 0x00}), &packet), MqttStatus::kMalformed);
    // 保留类型 0 / 15。
    MQTT_CHECK_EQ(DecodePacket(Bytes({0x00, 0x00}), &packet), MqttStatus::kMalformed);
    MQTT_CHECK_EQ(DecodePacket(Bytes({0xF0, 0x00}), &packet), MqttStatus::kMalformed);
    // 截断在固定头中间。
    MQTT_CHECK_EQ(DecodePacket(Bytes({0xC0}), &packet), MqttStatus::kNeedMoreData);
    MQTT_CHECK_EQ(DecodePacket(Bytes({0x30, 0x80}), &packet), MqttStatus::kNeedMoreData);
    MQTT_CHECK_EQ(DecodePacket(Bytes({0x30, 0x80, 0x80, 0x80, 0x80}), &packet),
                  MqttStatus::kMalformed);
}

// ---------------------------------------------------------------------------
// 4. 流式解码器：粘包 / 半包 / 上限 / 错误恢复
// ---------------------------------------------------------------------------

void TestDecoderStreaming() {
    const std::vector<std::uint8_t> ping = EncodePingreq();
    const std::vector<std::uint8_t> publish =
        EncodePublish(BasePublish("stream/topic", "payload", 1, 42));
    const std::vector<std::uint8_t> subscribe =
        EncodeSubscribe(7, {{"stream/#", 1}});
    const std::vector<std::vector<std::uint8_t>> parts{ping, publish, subscribe};
    const std::vector<std::uint8_t> stream = Concat(parts);

    // 一次喂入三包：依次取出。
    MqttDecoder decoder;
    decoder.Append(stream.data(), stream.size());
    MQTT_CHECK_EQ(decoder.PendingBytes(), stream.size());
    MqttPacket packet;
    MQTT_CHECK_EQ(decoder.Next(&packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.type, MqttPacketType::kPingreq);
    MQTT_CHECK_EQ(decoder.Next(&packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.type, MqttPacketType::kPublish);
    MQTT_CHECK_EQ(packet.publish.packet_id, 42);
    MQTT_CHECK_EQ(decoder.Next(&packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.type, MqttPacketType::kSubscribe);
    MQTT_CHECK_EQ(decoder.Next(&packet), MqttStatus::kNeedMoreData);
    MQTT_CHECK_EQ(decoder.PendingBytes(), 0U);

    // 逐字节喂入。
    MqttDecoder byte_decoder;
    std::size_t decoded = 0;
    for (const std::uint8_t byte : stream) {
        byte_decoder.Append(&byte, 1);
        MqttStatus status = MqttStatus::kOk;
        while ((status = byte_decoder.Next(&packet)) == MqttStatus::kOk) {
            ++decoded;
        }
        MQTT_CHECK(status == MqttStatus::kNeedMoreData);
    }
    MQTT_CHECK_EQ(decoded, 3U);
    MQTT_CHECK_EQ(byte_decoder.PendingBytes(), 0U);

    // 两包各拆一半交错喂入。
    MqttDecoder split_decoder;
    const std::size_t cut = publish.size() / 2;
    split_decoder.Append(publish.data(), cut);
    MQTT_CHECK_EQ(split_decoder.Next(&packet), MqttStatus::kNeedMoreData);
    split_decoder.Append(publish.data() + cut, publish.size() - cut);
    MQTT_CHECK_EQ(split_decoder.Next(&packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.publish.topic, std::string("stream/topic"));

    // 空 Append 无副作用。
    split_decoder.Append(nullptr, 0);
    MQTT_CHECK_EQ(split_decoder.PendingBytes(), 0U);
    MQTT_CHECK_EQ(split_decoder.Next(nullptr), MqttStatus::kInvalidArgument);
}

void TestDecoderErrorsAndLimits() {
    MqttDecoder decoder(4); // 只允许剩余长度 <= 4。
    const std::vector<std::uint8_t> ping = EncodePingreq();
    MqttPacket packet;
    decoder.Append(ping.data(), ping.size());
    MQTT_CHECK_EQ(decoder.Next(&packet), MqttStatus::kOk);
    const std::vector<std::uint8_t> publish =
        EncodePublish(BasePublish("long/topic", "0123456789", 0, 0));
    decoder.Append(publish.data(), publish.size());
    MQTT_CHECK_EQ(decoder.Next(&packet), MqttStatus::kTooLarge);
    MQTT_CHECK_EQ(decoder.PendingBytes(), 0U); // 错误后缓冲清空。

    // 非法类型清空缓冲，恢复后可继续解析新包。
    const std::vector<std::uint8_t> junk_bytes = Bytes({0xF0, 0x00});
    decoder.Append(junk_bytes.data(), junk_bytes.size());
    MQTT_CHECK_EQ(decoder.Next(&packet), MqttStatus::kMalformed);
    MQTT_CHECK_EQ(decoder.PendingBytes(), 0U);
    decoder.Append(ping.data(), ping.size());
    MQTT_CHECK_EQ(decoder.Next(&packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.type, MqttPacketType::kPingreq);

    // Clear 丢弃半包。
    decoder.Append(publish.data(), 3);
    MQTT_CHECK(decoder.PendingBytes() > 0);
    decoder.Clear();
    MQTT_CHECK_EQ(decoder.PendingBytes(), 0U);

    // max = 0 只接受空载荷包。
    MqttDecoder empty_only(0);
    empty_only.Append(ping.data(), ping.size());
    MQTT_CHECK_EQ(empty_only.Next(&packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(empty_only.max_packet_bytes(), 0U);
    const std::vector<std::uint8_t> connect = EncodeConnect(BaseConnect("x", 0));
    empty_only.Append(connect.data(), connect.size());
    MQTT_CHECK_EQ(empty_only.Next(&packet), MqttStatus::kTooLarge);
}

// ---------------------------------------------------------------------------
// 5. 主题过滤器与匹配
// ---------------------------------------------------------------------------

void TestTopicValidationAndMatching() {
    MQTT_CHECK(IsValidTopicName("a/b"));
    MQTT_CHECK(IsValidTopicName("/"));
    MQTT_CHECK(IsValidTopicName("a//b"));
    MQTT_CHECK(!IsValidTopicName(""));
    MQTT_CHECK(!IsValidTopicName("a/+"));
    MQTT_CHECK(!IsValidTopicName("a/#"));
    MQTT_CHECK(!IsValidTopicName(std::string("a\0b", 3)));

    MQTT_CHECK(IsValidTopicFilter("#"));
    MQTT_CHECK(IsValidTopicFilter("+"));
    MQTT_CHECK(IsValidTopicFilter("sport/#"));
    MQTT_CHECK(IsValidTopicFilter("sport/+/player1"));
    MQTT_CHECK(IsValidTopicFilter("/"));
    MQTT_CHECK(!IsValidTopicFilter(""));
    MQTT_CHECK(!IsValidTopicFilter("sport/#/ranking"));
    MQTT_CHECK(!IsValidTopicFilter("sport+"));
    MQTT_CHECK(!IsValidTopicFilter("sport/tennis#"));
    MQTT_CHECK(!IsValidTopicFilter("a/+b"));
    MQTT_CHECK(!IsValidTopicFilter(std::string("a\0b", 3)));

    struct MatchCase {
        const char* filter;
        const char* topic;
        bool expected;
    };
    const MatchCase cases[] = {
        {"#", "a", true},
        {"#", "a/b/c", true},
        {"#", "/", true},
        {"sport/#", "sport", true},
        {"sport/#", "sport/", true},
        {"sport/#", "sport/a/b", true},
        {"sport/+", "sport", false},
        {"sport/+", "sport/", true},
        {"sport/+", "sport/tennis", true},
        {"sport/+", "sport/tennis/player1", false},
        {"+/tennis/#", "sport/tennis/player1", true},
        {"+/tennis/#", "sport/badminton/player1", false},
        {"$SYS/#", "$SYS/broker/uptime", true},
        {"#", "$SYS/broker/uptime", false},
        {"+/broker/uptime", "$SYS/broker/uptime", false},
        {"a/b", "a/b", true},
        {"a/b", "a/b/c", false},
        {"a/b", "a", false},
        {"a/+", "a//", false},
        {"a/+", "a/", true},
    };
    for (const auto& item : cases) {
        const bool actual = TopicMatchesFilter(item.filter, item.topic);
        if (actual != item.expected) {
            std::fprintf(stderr, "[FAIL] match(%s, %s) = %d, expected %d\n", item.filter,
                         item.topic, actual ? 1 : 0, item.expected ? 1 : 0);
            ++g_failures;
        }
        ++g_checks;
    }
}

// ---------------------------------------------------------------------------
// 6. 会话状态机
// ---------------------------------------------------------------------------

void TestSessionStateMachine() {
    MqttSession session{MqttSessionConfig{"client-a", 0, true}};
    std::vector<std::uint8_t> response;
    MqttPacket packet;

    // 未 CONNECT 之前一切报文都是协议违规。
    packet.type = MqttPacketType::kPingreq;
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kProtocolViolation);
    packet = MqttPacket{};
    packet.type = MqttPacketType::kSubscribe;
    packet.packet_id = 1;
    packet.subscriptions = {{"a/#", 0}};
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kProtocolViolation);
    MQTT_CHECK(!session.connected());

    // CONNECT 成功。
    MqttPacket connect;
    connect.type = MqttPacketType::kConnect;
    connect.connect = BaseConnect("client-a", 5);
    MQTT_CHECK_EQ(session.HandlePacket(connect, &response), MqttStatus::kOk);
    MQTT_CHECK_EQ(response.size(), 4U);
    MQTT_CHECK_EQ(DecodePacket(response, &packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.connack.return_code, 0);
    MQTT_CHECK(session.connected());
    MQTT_CHECK_EQ(session.keepalive_seconds(), 5);
    MQTT_CHECK_EQ(session.client_id(), std::string("client-a"));

    // 重复 CONNECT。
    MQTT_CHECK_EQ(session.HandlePacket(connect, &response), MqttStatus::kProtocolViolation);

    // 订阅。
    MqttPacket subscribe;
    subscribe.type = MqttPacketType::kSubscribe;
    subscribe.packet_id = 11;
    subscribe.subscriptions = {{"sensors/+/temp", 1}, {"qos2/#", 2}};
    MQTT_CHECK_EQ(session.HandlePacket(subscribe, &response), MqttStatus::kOk);
    MQTT_CHECK_EQ(DecodePacket(response, &packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.type, MqttPacketType::kSuback);
    MQTT_CHECK_EQ(packet.packet_id, 11);
    MQTT_CHECK_EQ(packet.suback_codes.size(), 2U);
    MQTT_CHECK_EQ(packet.suback_codes[0], 1);
    MQTT_CHECK_EQ(packet.suback_codes[1], kMqttSubackFailure); // QoS2 不支持。
    MQTT_CHECK(session.HasSubscription("sensors/+/temp"));
    MQTT_CHECK(!session.HasSubscription("qos2/#"));
    MQTT_CHECK_EQ(session.SubscriptionQos("sensors/+/temp"), 1);
    MQTT_CHECK_EQ(session.SubscriptionQos("missing"), kMqttNoSubscription);

    // PINGREQ -> PINGRESP。
    packet = MqttPacket{};
    packet.type = MqttPacketType::kPingreq;
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kOk);
    MQTT_CHECK_EQ(response, EncodePingresp());
    // PINGRESP 在服务端会话里被忽略但不报错。
    packet = MqttPacket{};
    packet.type = MqttPacketType::kPingresp;
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kOk);

    // PUBACK 校验 packet id。
    packet = MqttPacket{};
    packet.type = MqttPacketType::kPuback;
    packet.packet_id = 0;
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kMalformed);
    packet.packet_id = 9;
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kOk);

    // 方向错误的 CONNACK / SUBACK。
    packet = MqttPacket{};
    packet.type = MqttPacketType::kConnack;
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kProtocolViolation);
    packet = MqttPacket{};
    packet.type = MqttPacketType::kSuback;
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kProtocolViolation);

    // PUBLISH：QoS0 无响应，QoS1 回 PUBACK，QoS2 明确不支持。
    packet = MqttPacket{};
    packet.type = MqttPacketType::kPublish;
    packet.publish = BasePublish("sensors/room/temp", "20", 0, 0);
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kOk);
    MQTT_CHECK(response.empty());
    packet.publish.qos = 1;
    packet.publish.packet_id = 77;
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kOk);
    MQTT_CHECK_EQ(DecodePacket(response, &packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.type, MqttPacketType::kPuback);
    MQTT_CHECK_EQ(packet.packet_id, 77);
    packet = MqttPacket{};
    packet.type = MqttPacketType::kPublish;
    packet.publish = BasePublish("sensors/room/temp", "20", 2, 78);
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kUnsupported);
    // QoS1 但调用方不给响应缓冲：拒绝，避免静默丢 PUBACK。
    packet.publish.qos = 1;
    MQTT_CHECK_EQ(session.HandlePacket(packet, nullptr), MqttStatus::kInvalidArgument);

    // DISCONNECT。
    packet = MqttPacket{};
    packet.type = MqttPacketType::kDisconnect;
    MQTT_CHECK_EQ(session.HandlePacket(packet, &response), MqttStatus::kOk);
    MQTT_CHECK(!session.connected());
    MQTT_CHECK(session.disconnected());
}

void TestSessionConnectRejections() {
    MqttSession session{MqttSessionConfig{"id", 0, true}};
    MqttPacket connect;
    connect.type = MqttPacketType::kConnect;
    std::vector<std::uint8_t> response;
    MqttPacket decoded;

    // 协议级别不对：CONNACK 0x01，保持未连接。
    connect.connect = BaseConnect("id", 0);
    connect.connect.protocol_level = 3;
    MQTT_CHECK_EQ(session.HandlePacket(connect, &response), MqttStatus::kOk);
    MQTT_CHECK_EQ(DecodePacket(response, &decoded), MqttStatus::kOk);
    MQTT_CHECK_EQ(decoded.connack.return_code, 1);
    MQTT_CHECK(!session.connected());

    // 空 client id 且非 clean session：CONNACK 0x02。
    connect.connect = BaseConnect("", 0);
    connect.connect.clean_session = false;
    MQTT_CHECK_EQ(session.HandlePacket(connect, &response), MqttStatus::kOk);
    MQTT_CHECK_EQ(DecodePacket(response, &decoded), MqttStatus::kOk);
    MQTT_CHECK_EQ(decoded.connack.return_code, 2);
    MQTT_CHECK(!session.connected());

    // QoS2 遗嘱：骨架不支持，拒绝连接且不产生 CONNACK。
    connect.connect = BaseConnect("id", 0);
    connect.connect.has_will = true;
    connect.connect.will_topic = "will/topic";
    connect.connect.will_qos = 2;
    MQTT_CHECK_EQ(session.HandlePacket(connect, &response), MqttStatus::kUnsupported);
    MQTT_CHECK(!session.connected());
}

void TestSessionHandleBytesAndCleanSession() {
    MqttSession session{MqttSessionConfig{"id", 0, true}};
    const std::vector<std::uint8_t> connect = EncodeConnect(BaseConnect("id", 0));
    const std::vector<std::uint8_t> subscribe = EncodeSubscribe(1, {{"a/#", 0}});
    const std::vector<std::uint8_t> stream = Concat({connect, subscribe});

    // 半包：第一次只喂 CONNECT 前 3 字节。
    std::vector<std::vector<std::uint8_t>> responses;
    MQTT_CHECK_EQ(session.HandleBytes(stream.data(), 3, &responses), MqttStatus::kOk);
    MQTT_CHECK(responses.empty());
    MQTT_CHECK(!session.connected());
    MQTT_CHECK_EQ(session.HandleBytes(stream.data() + 3, stream.size() - 3, &responses),
                  MqttStatus::kOk);
    MQTT_CHECK_EQ(responses.size(), 2U); // CONNACK + SUBACK
    MqttPacket packet;
    MQTT_CHECK_EQ(DecodePacket(responses[0], &packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.type, MqttPacketType::kConnack);
    MQTT_CHECK_EQ(DecodePacket(responses[1], &packet), MqttStatus::kOk);
    MQTT_CHECK_EQ(packet.type, MqttPacketType::kSuback);
    MQTT_CHECK(session.HasSubscription("a/#"));

    // 非法字节流：返回错误，之后会话状态不被破坏（由调用方关连接）。
    MQTT_CHECK_EQ(session.HandleBytes(Bytes({0xF0, 0x00}).data(), 2, &responses),
                  MqttStatus::kMalformed);

    // Reset 清空订阅与连接状态。
    session.Reset();
    MQTT_CHECK(!session.connected());
    MQTT_CHECK(!session.HasSubscription("a/#"));
    MQTT_CHECK_EQ(session.keepalive_seconds(), 0);

    // clean_session=false 重连保留旧订阅（同一 MqttSession 对象内）。
    MqttSession persistent{MqttSessionConfig{"p", 0, false}};
    MqttPacket connect_packet;
    connect_packet.type = MqttPacketType::kConnect;
    connect_packet.connect = BaseConnect("p", 0);
    connect_packet.connect.clean_session = false;
    std::vector<std::uint8_t> response;
    MQTT_CHECK_EQ(persistent.HandlePacket(connect_packet, &response), MqttStatus::kOk);
    MqttPacket sub_packet;
    sub_packet.type = MqttPacketType::kSubscribe;
    sub_packet.packet_id = 1;
    sub_packet.subscriptions = {{"keep/#", 1}};
    MQTT_CHECK_EQ(persistent.HandlePacket(sub_packet, &response), MqttStatus::kOk);
    MQTT_CHECK(persistent.HasSubscription("keep/#"));
    // 断开后重新 CONNECT（clean_session=true）会清空订阅。
    MqttPacket disconnect_packet;
    disconnect_packet.type = MqttPacketType::kDisconnect;
    MQTT_CHECK_EQ(persistent.HandlePacket(disconnect_packet, &response), MqttStatus::kOk);
    connect_packet.connect.clean_session = true;
    MQTT_CHECK_EQ(persistent.HandlePacket(connect_packet, &response), MqttStatus::kOk);
    MQTT_CHECK(!persistent.HasSubscription("keep/#"));
}

void TestKeepalive() {
    MqttSession session{MqttSessionConfig{"ka", 0, true}};
    MQTT_CHECK_EQ(session.KeepaliveTimeoutMs(), 0U);
    session.Touch(1000);
    MQTT_CHECK(!session.KeepaliveExpired(1000000)); // 未连接时永不判超时。

    MqttPacket connect;
    connect.type = MqttPacketType::kConnect;
    connect.connect = BaseConnect("ka", 2);
    std::vector<std::uint8_t> response;
    MQTT_CHECK_EQ(session.HandlePacket(connect, &response), MqttStatus::kOk);
    MQTT_CHECK_EQ(session.KeepaliveTimeoutMs(), 3000U);
    session.Touch(1000);
    MQTT_CHECK(!session.KeepaliveExpired(1000));
    MQTT_CHECK(!session.KeepaliveExpired(3999));
    MQTT_CHECK(!session.KeepaliveExpired(4000)); // 恰好 1.5 倍不算超时。
    MQTT_CHECK(session.KeepaliveExpired(4001));
    MQTT_CHECK(!session.KeepaliveExpired(500)); // 时钟回拨。
    MQTT_CHECK_EQ(session.last_activity_ms(), 1000U);

    // keepalive = 0：不启用超时。
    MqttSession no_timer{MqttSessionConfig{"nt", 0, true}};
    connect.connect = BaseConnect("nt", 0);
    MQTT_CHECK_EQ(no_timer.HandlePacket(connect, &response), MqttStatus::kOk);
    no_timer.Touch(0);
    MQTT_CHECK(!no_timer.KeepaliveExpired(0xFFFFFFFFFFFFFFFFull));
    MQTT_CHECK_EQ(no_timer.KeepaliveTimeoutMs(), 0U);

    // 最大 keepalive 的阈值不发生 uint32 溢出。
    connect.connect = BaseConnect("max", 65535);
    MqttSession max_timer{MqttSessionConfig{"max", 0, true}};
    MQTT_CHECK_EQ(max_timer.HandlePacket(connect, &response), MqttStatus::kOk);
    MQTT_CHECK_EQ(max_timer.KeepaliveTimeoutMs(), 65535ULL * 1000ULL + 65535ULL * 500ULL);
}

// ---------------------------------------------------------------------------
// 7. Broker：publish-subscribe 回环、QoS 降级、遗嘱、错误路径
// ---------------------------------------------------------------------------

/// 把 outbound 里发给 client_id 的所有字节串起来（同一客户端可能收到多包）。
std::vector<std::uint8_t> OutboundFor(const std::vector<MqttOutbound>& outbound,
                                      const std::string& client_id) {
    std::vector<std::uint8_t> joined;
    for (const auto& message : outbound) {
        if (message.client_id == client_id) {
            joined.insert(joined.end(), message.bytes.begin(), message.bytes.end());
        }
    }
    return joined;
}

std::size_t OutboundCount(const std::vector<MqttOutbound>& outbound,
                          const std::string& client_id) {
    std::size_t count = 0;
    for (const auto& message : outbound) {
        if (message.client_id == client_id) {
            ++count;
        }
    }
    return count;
}

/// 从一段字节流里解出全部报文（测试侧复用 MqttDecoder）。
std::vector<MqttPacket> DecodeAll(const std::vector<std::uint8_t>& bytes) {
    MqttDecoder decoder;
    decoder.Append(bytes.data(), bytes.size());
    std::vector<MqttPacket> packets;
    MqttPacket packet;
    while (decoder.Next(&packet) == MqttStatus::kOk) {
        packets.push_back(packet);
    }
    return packets;
}

MqttStatus ConnectClient(MqttBroker* broker, const std::string& client_id,
                         std::uint16_t keepalive, std::vector<MqttOutbound>* outbound) {
    const MqttStatus opened = broker->OpenSession(client_id, nullptr);
    if (opened != MqttStatus::kOk) {
        return opened;
    }
    const std::vector<std::uint8_t> bytes = EncodeConnect(BaseConnect(client_id, keepalive));
    return broker->HandleBytes(client_id, bytes.data(), bytes.size(), outbound);
}

MqttStatus SubscribeClient(MqttBroker* broker, const std::string& client_id,
                           std::uint16_t packet_id,
                           const std::vector<MqttSubscription>& subscriptions,
                           std::vector<MqttOutbound>* outbound) {
    const std::vector<std::uint8_t> bytes = EncodeSubscribe(packet_id, subscriptions);
    return broker->HandleBytes(client_id, bytes.data(), bytes.size(), outbound);
}

MqttStatus PublishClient(MqttBroker* broker, const std::string& client_id,
                         const MqttPublish& publish, std::vector<MqttOutbound>* outbound) {
    const std::vector<std::uint8_t> bytes = EncodePublish(publish);
    return broker->HandleBytes(client_id, bytes.data(), bytes.size(), outbound);
}

void TestBrokerPubSubLoop() {
    MqttBroker broker;
    std::vector<MqttOutbound> outbound;
    MQTT_CHECK_EQ(ConnectClient(&broker, "subscriber", 60, &outbound), MqttStatus::kOk);
    MQTT_CHECK_EQ(ConnectClient(&broker, "publisher", 60, &outbound), MqttStatus::kOk);
    MQTT_CHECK_EQ(broker.session_count(), 2U);
    // 两条 CONNACK 都应该在 outbound 里。
    MQTT_CHECK_EQ(OutboundCount(outbound, "subscriber"), 1U);
    MQTT_CHECK_EQ(OutboundCount(outbound, "publisher"), 1U);
    outbound.clear();

    MQTT_CHECK_EQ(SubscribeClient(&broker, "subscriber", 1, {{"sensors/+/temp", 1}}, &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK_EQ(OutboundCount(outbound, "subscriber"), 1U);
    MQTT_CHECK_EQ(DecodeAll(OutboundFor(outbound, "subscriber"))[0].type,
                  MqttPacketType::kSuback);
    outbound.clear();

    // QoS1 回环：发布者收 PUBACK，订阅者收 PUBLISH。
    MQTT_CHECK_EQ(PublishClient(&broker, "publisher",
                                BasePublish("sensors/room1/temp", "21.7", 1, 321), &outbound),
                  MqttStatus::kOk);
    const std::vector<MqttPacket> publisher_packets =
        DecodeAll(OutboundFor(outbound, "publisher"));
    MQTT_CHECK_EQ(publisher_packets.size(), 1U);
    MQTT_CHECK_EQ(publisher_packets[0].type, MqttPacketType::kPuback);
    MQTT_CHECK_EQ(publisher_packets[0].packet_id, 321);

    const std::vector<MqttPacket> subscriber_packets =
        DecodeAll(OutboundFor(outbound, "subscriber"));
    MQTT_CHECK_EQ(subscriber_packets.size(), 1U);
    MQTT_CHECK_EQ(subscriber_packets[0].type, MqttPacketType::kPublish);
    MQTT_CHECK_EQ(subscriber_packets[0].publish.topic, std::string("sensors/room1/temp"));
    MQTT_CHECK_EQ(PayloadString(subscriber_packets[0].publish.payload), std::string("21.7"));
    MQTT_CHECK_EQ(subscriber_packets[0].publish.qos, 1);
    MQTT_CHECK(subscriber_packets[0].publish.packet_id != 0);
    MQTT_CHECK_EQ(OutboundCount(outbound, "publisher"), 1U);

    // 未连接的客户端不能发布。
    outbound.clear();
    MQTT_CHECK_EQ(broker.OpenSession("idle", nullptr), MqttStatus::kOk);
    MQTT_CHECK_EQ(PublishClient(&broker, "idle", BasePublish("sensors/room1/temp", "x", 0, 0),
                                &outbound),
                  MqttStatus::kProtocolViolation);
    MQTT_CHECK(broker.FindSession("idle") != nullptr);

    // 不存在的客户端。
    MQTT_CHECK_EQ(PublishClient(&broker, "ghost", BasePublish("a", "b", 0, 0), &outbound),
                  MqttStatus::kInvalidArgument);
    MQTT_CHECK(!broker.HasSession("ghost"));
    MQTT_CHECK(broker.FindSession("ghost") == nullptr);
}

void TestBrokerWildcardsAndQosDowngrade() {
    MqttBroker broker;
    std::vector<MqttOutbound> outbound;
    MQTT_CHECK_EQ(ConnectClient(&broker, "all", 0, &outbound), MqttStatus::kOk);
    MQTT_CHECK_EQ(ConnectClient(&broker, "room", 0, &outbound), MqttStatus::kOk);
    MQTT_CHECK_EQ(ConnectClient(&broker, "qos0", 0, &outbound), MqttStatus::kOk);
    MQTT_CHECK_EQ(ConnectClient(&broker, "sender", 0, &outbound), MqttStatus::kOk);
    outbound.clear();

    MQTT_CHECK_EQ(SubscribeClient(&broker, "all", 1, {{"chat/#", 1}}, &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK_EQ(SubscribeClient(&broker, "room", 2, {{"chat/room/+", 1}}, &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK_EQ(SubscribeClient(&broker, "qos0", 3, {{"chat/room/1", 0}}, &outbound),
                  MqttStatus::kOk);
    outbound.clear();

    // chat/room/1 命中三个客户端，但 qos0 订阅要求降级为 QoS0。
    MQTT_CHECK_EQ(PublishClient(&broker, "sender",
                                BasePublish("chat/room/1", "hello", 1, 5), &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK_EQ(OutboundCount(outbound, "all"), 1U);
    MQTT_CHECK_EQ(OutboundCount(outbound, "room"), 1U);
    MQTT_CHECK_EQ(OutboundCount(outbound, "qos0"), 1U);
    MQTT_CHECK_EQ(OutboundCount(outbound, "sender"), 1U); // 只有 PUBACK。

    const std::vector<MqttPacket> all_packets = DecodeAll(OutboundFor(outbound, "all"));
    const std::vector<MqttPacket> room_packets = DecodeAll(OutboundFor(outbound, "room"));
    const std::vector<MqttPacket> qos0_packets = DecodeAll(OutboundFor(outbound, "qos0"));
    const std::vector<MqttPacket> sender_packets = DecodeAll(OutboundFor(outbound, "sender"));
    MQTT_CHECK_EQ(all_packets[0].publish.qos, 1);
    MQTT_CHECK_EQ(room_packets[0].publish.qos, 1);
    MQTT_CHECK_EQ(qos0_packets[0].publish.qos, 0);
    MQTT_CHECK_EQ(qos0_packets[0].publish.packet_id, 0);
    MQTT_CHECK_EQ(sender_packets[0].type, MqttPacketType::kPuback);

    // chat/other 只命中 chat/#。
    outbound.clear();
    MQTT_CHECK_EQ(PublishClient(&broker, "sender",
                                BasePublish("chat/other", "x", 0, 0), &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK_EQ(OutboundCount(outbound, "all"), 1U);
    MQTT_CHECK_EQ(OutboundCount(outbound, "room"), 0U);
    MQTT_CHECK_EQ(OutboundCount(outbound, "qos0"), 0U);
    MQTT_CHECK_EQ(OutboundCount(outbound, "sender"), 0U);

    // $SYS 主题不会被 # 吞掉。
    outbound.clear();
    MQTT_CHECK_EQ(SubscribeClient(&broker, "room", 4, {{"$SYS/#", 1}}, &outbound),
                  MqttStatus::kOk);
    outbound.clear();
    MQTT_CHECK_EQ(PublishClient(&broker, "sender",
                                BasePublish("$SYS/broker/uptime", "10", 0, 0), &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK_EQ(OutboundCount(outbound, "all"), 0U);
    MQTT_CHECK_EQ(OutboundCount(outbound, "room"), 1U);
}

void TestBrokerWill() {
    MqttBroker broker;
    std::vector<MqttOutbound> outbound;
    MQTT_CHECK_EQ(ConnectClient(&broker, "watcher", 0, &outbound), MqttStatus::kOk);
    outbound.clear();
    MQTT_CHECK_EQ(SubscribeClient(&broker, "watcher", 1, {{"clients/+/status", 1}}, &outbound),
                  MqttStatus::kOk);
    outbound.clear();

    // 带遗嘱的客户端接入。
    MQTT_CHECK_EQ(broker.OpenSession("device", nullptr), MqttStatus::kOk);
    MqttConnectOptions will_options = BaseConnect("device", 0);
    will_options.has_will = true;
    will_options.will_topic = "clients/device/status";
    will_options.will_message = ToBytes("offline");
    will_options.will_qos = 1;
    const std::vector<std::uint8_t> connect = EncodeConnect(will_options);
    MQTT_CHECK_EQ(broker.HandleBytes("device", connect.data(), connect.size(), &outbound),
                  MqttStatus::kOk);
    const MqttSession* device = broker.FindSession("device");
    MQTT_CHECK(device != nullptr);
    MQTT_CHECK(device != nullptr && device->HasPendingWill());
    outbound.clear();

    // 异常断开：遗嘱投递给 watcher。
    MQTT_CHECK_EQ(broker.CloseSession("device", false, &outbound), MqttStatus::kOk);
    MQTT_CHECK(!broker.HasSession("device"));
    const std::vector<MqttPacket> will_packets = DecodeAll(OutboundFor(outbound, "watcher"));
    MQTT_CHECK_EQ(will_packets.size(), 1U);
    MQTT_CHECK_EQ(will_packets[0].type, MqttPacketType::kPublish);
    MQTT_CHECK_EQ(will_packets[0].publish.topic, std::string("clients/device/status"));
    MQTT_CHECK_EQ(PayloadString(will_packets[0].publish.payload), std::string("offline"));
    MQTT_CHECK_EQ(will_packets[0].publish.qos, 1);

    // 正常 DISCONNECT：不投递遗嘱，会话被移除。
    outbound.clear();
    MQTT_CHECK_EQ(broker.OpenSession("polite", nullptr), MqttStatus::kOk);
    MqttConnectOptions polite_options = BaseConnect("polite", 0);
    polite_options.has_will = true;
    polite_options.will_topic = "clients/polite/status";
    polite_options.will_message = ToBytes("unexpected");
    polite_options.will_qos = 1;
    const std::vector<std::uint8_t> polite_connect = EncodeConnect(polite_options);
    MQTT_CHECK_EQ(
        broker.HandleBytes("polite", polite_connect.data(), polite_connect.size(), &outbound),
        MqttStatus::kOk);
    outbound.clear();
    const std::vector<std::uint8_t> disconnect = EncodeDisconnect();
    MQTT_CHECK_EQ(broker.HandleBytes("polite", disconnect.data(), disconnect.size(), &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK(!broker.HasSession("polite"));
    MQTT_CHECK(OutboundFor(outbound, "watcher").empty());

    // 重复关闭 / 关闭不存在会话。
    MQTT_CHECK_EQ(broker.CloseSession("polite", false, &outbound),
                  MqttStatus::kInvalidArgument);
    // keepalive=0 不超时。
    MQTT_CHECK_EQ(broker.CloseExpiredSessions(1000000, &outbound), 0U);
}

void TestBrokerErrorPaths() {
    MqttBroker broker;
    std::vector<MqttOutbound> outbound;

    // 空 client id：自动分配。
    std::string assigned;
    MQTT_CHECK_EQ(broker.OpenSession("", &assigned), MqttStatus::kOk);
    MQTT_CHECK_EQ(assigned, std::string("auto-1"));
    MQTT_CHECK(broker.HasSession("auto-1"));
    // CONNECT 报文里的 client id 留空：沿用 broker 分配值。
    const std::vector<std::uint8_t> empty_id_connect = EncodeConnect(BaseConnect("", 0));
    MQTT_CHECK_EQ(broker.HandleBytes("auto-1", empty_id_connect.data(),
                                     empty_id_connect.size(), &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK(broker.FindSession("auto-1") != nullptr &&
               broker.FindSession("auto-1")->connected());

    // client id 不一致。
    MQTT_CHECK_EQ(broker.OpenSession("alpha", nullptr), MqttStatus::kOk);
    const std::vector<std::uint8_t> mismatch = EncodeConnect(BaseConnect("beta", 0));
    MQTT_CHECK_EQ(broker.HandleBytes("alpha", mismatch.data(), mismatch.size(), &outbound),
                  MqttStatus::kProtocolViolation);

    // 未登记连接。
    MQTT_CHECK_EQ(broker.HandleBytes("nobody", nullptr, 0, &outbound),
                  MqttStatus::kInvalidArgument);

    // 畸形字节流：类型 15。
    const std::vector<std::uint8_t> junk = Bytes({0xF0, 0x00});
    MQTT_CHECK_EQ(broker.HandleBytes("alpha", junk.data(), junk.size(), &outbound),
                  MqttStatus::kMalformed);

    // 半包：CONNECT 拆两段，第二段才产生 CONNACK。
    const std::vector<std::uint8_t> connect = EncodeConnect(BaseConnect("beta", 0));
    MQTT_CHECK_EQ(broker.OpenSession("beta", nullptr), MqttStatus::kOk);
    outbound.clear();
    MQTT_CHECK_EQ(broker.HandleBytes("beta", connect.data(), 2, &outbound), MqttStatus::kOk);
    MQTT_CHECK(OutboundFor(outbound, "beta").empty());
    MQTT_CHECK_EQ(broker.HandleBytes("beta", connect.data() + 2, connect.size() - 2, &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK_EQ(OutboundCount(outbound, "beta"), 1U);

    // 重复 OpenSession 顶替旧会话：订阅丢失、连接状态重置。
    MQTT_CHECK_EQ(SubscribeClient(&broker, "beta", 1, {{"x/#", 1}}, &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK(broker.FindSession("beta")->HasSubscription("x/#"));
    MQTT_CHECK_EQ(broker.OpenSession("beta", nullptr), MqttStatus::kOk);
    MQTT_CHECK_EQ(broker.session_count(), 3U);
    MQTT_CHECK(broker.FindSession("beta") != nullptr &&
               !broker.FindSession("beta")->connected());
    MQTT_CHECK(!broker.FindSession("beta")->HasSubscription("x/#"));

    // 服务端不接受的报文方向。
    MQTT_CHECK_EQ(SubscribeClient(&broker, "beta", 2, {{"x/#", 1}}, &outbound),
                  MqttStatus::kProtocolViolation); // 尚未 CONNECT。

    // OpenSession 含 NUL 的 client id。
    MQTT_CHECK_EQ(broker.OpenSession(std::string("bad\0id", 6), nullptr),
                  MqttStatus::kInvalidArgument);
}

// ---------------------------------------------------------------------------
// 8. 真实场景（≥5，覆盖全部功能点）
// ---------------------------------------------------------------------------

void ScenarioTemperatureReport() {
    // 场景 1：传感器 QoS0 周期上报，手机通过单层通配符订阅。
    ++g_scenarios;
    std::printf("[SCENARIO] 温度传感器 QoS0 上报\n");
    MqttBroker broker;
    std::vector<MqttOutbound> outbound;
    MQTT_CHECK_EQ(ConnectClient(&broker, "sensor", 60, &outbound), MqttStatus::kOk);
    MQTT_CHECK_EQ(ConnectClient(&broker, "phone", 60, &outbound), MqttStatus::kOk);
    outbound.clear();
    MQTT_CHECK_EQ(SubscribeClient(&broker, "phone", 1, {{"home/+/temperature", 0}}, &outbound),
                  MqttStatus::kOk);
    outbound.clear();
    MQTT_CHECK_EQ(PublishClient(&broker, "sensor",
                                BasePublish("home/kitchen/temperature", "22.5", 0, 0), &outbound),
                  MqttStatus::kOk);
    const std::vector<MqttPacket> packets = DecodeAll(OutboundFor(outbound, "phone"));
    MQTT_CHECK_EQ(packets.size(), 1U);
    MQTT_CHECK_EQ(packets[0].publish.topic, std::string("home/kitchen/temperature"));
    MQTT_CHECK_EQ(PayloadString(packets[0].publish.payload), std::string("22.5"));
    MQTT_CHECK_EQ(packets[0].publish.qos, 0);
    MQTT_CHECK_EQ(OutboundFor(outbound, "sensor").size(), 0U);
}

void ScenarioQos1Command() {
    // 场景 2：控制端下发 QoS1 命令，设备回 PUBACK，双方 packet id 一致。
    ++g_scenarios;
    std::printf("[SCENARIO] QoS1 远程命令与 PUBACK\n");
    MqttBroker broker;
    std::vector<MqttOutbound> outbound;
    MQTT_CHECK_EQ(ConnectClient(&broker, "controller", 30, &outbound), MqttStatus::kOk);
    MQTT_CHECK_EQ(ConnectClient(&broker, "device", 30, &outbound), MqttStatus::kOk);
    outbound.clear();
    MQTT_CHECK_EQ(SubscribeClient(&broker, "device", 1, {{"cmd/device", 1}}, &outbound),
                  MqttStatus::kOk);
    outbound.clear();

    const MqttPublish command = BasePublish("cmd/device", "reboot", 1, 9001);
    MQTT_CHECK_EQ(PublishClient(&broker, "controller", command, &outbound), MqttStatus::kOk);
    const std::vector<MqttPacket> controller_packets =
        DecodeAll(OutboundFor(outbound, "controller"));
    MQTT_CHECK_EQ(controller_packets.size(), 1U);
    MQTT_CHECK_EQ(controller_packets[0].type, MqttPacketType::kPuback);
    MQTT_CHECK_EQ(controller_packets[0].packet_id, 9001);

    const std::vector<MqttPacket> device_packets = DecodeAll(OutboundFor(outbound, "device"));
    MQTT_CHECK_EQ(device_packets.size(), 1U);
    const std::uint16_t delivered_id = device_packets[0].publish.packet_id;
    MQTT_CHECK(delivered_id != 0);
    MQTT_CHECK_EQ(device_packets[0].publish.qos, 1);

    // 设备确认：把 PUBACK 发回 broker，会话接受。
    outbound.clear();
    const std::vector<std::uint8_t> puback = EncodePuback(delivered_id);
    MQTT_CHECK_EQ(broker.HandleBytes("device", puback.data(), puback.size(), &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK(outbound.empty());
}

void ScenarioChatRoom() {
    // 场景 3：聊天室三档订阅（#、/+/、精确）与不匹配隔离。
    ++g_scenarios;
    std::printf("[SCENARIO] 聊天室多订阅者与通配符\n");
    MqttBroker broker;
    std::vector<MqttOutbound> outbound;
    MQTT_CHECK_EQ(ConnectClient(&broker, "lobby", 0, &outbound), MqttStatus::kOk);
    MQTT_CHECK_EQ(ConnectClient(&broker, "room-1", 0, &outbound), MqttStatus::kOk);
    MQTT_CHECK_EQ(ConnectClient(&broker, "writer", 0, &outbound), MqttStatus::kOk);
    outbound.clear();
    MQTT_CHECK_EQ(SubscribeClient(&broker, "lobby", 1, {{"chat/#", 0}}, &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK_EQ(SubscribeClient(&broker, "room-1", 2, {{"chat/room/+", 0}}, &outbound),
                  MqttStatus::kOk);
    outbound.clear();

    MQTT_CHECK_EQ(PublishClient(&broker, "writer",
                                BasePublish("chat/room/1", "hi", 0, 0), &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK_EQ(OutboundCount(outbound, "lobby"), 1U);
    MQTT_CHECK_EQ(OutboundCount(outbound, "room-1"), 1U);
    outbound.clear();
    MQTT_CHECK_EQ(PublishClient(&broker, "writer",
                                BasePublish("chat/lobby/notice", "ping", 0, 0), &outbound),
                  MqttStatus::kOk);
    MQTT_CHECK_EQ(OutboundCount(outbound, "lobby"), 1U);
    MQTT_CHECK_EQ(OutboundCount(outbound, "room-1"), 0U);
}

void ScenarioOfflineWill() {
    // 场景 4：设备掉线（keepalive 超时/异常断开）触发遗嘱告警，正常断开不触发。
    ++g_scenarios;
    std::printf("[SCENARIO] 设备掉线遗嘱告警\n");
    MqttBroker broker;
    std::vector<MqttOutbound> outbound;
    MQTT_CHECK_EQ(ConnectClient(&broker, "monitor", 0, &outbound), MqttStatus::kOk);
    outbound.clear();
    MQTT_CHECK_EQ(SubscribeClient(&broker, "monitor", 1, {{"devices/+/state", 1}}, &outbound),
                  MqttStatus::kOk);
    outbound.clear();

    MQTT_CHECK_EQ(broker.OpenSession("camera", nullptr), MqttStatus::kOk);
    MqttConnectOptions options = BaseConnect("camera", 5);
    options.has_will = true;
    options.will_topic = "devices/camera/state";
    options.will_message = ToBytes("offline");
    options.will_qos = 1;
    const std::vector<std::uint8_t> connect = EncodeConnect(options);
    MQTT_CHECK_EQ(broker.HandleBytes("camera", connect.data(), connect.size(), &outbound),
                  MqttStatus::kOk);
    outbound.clear();

    MQTT_CHECK_EQ(broker.CloseSession("camera", false, &outbound), MqttStatus::kOk);
    const std::vector<MqttPacket> packets = DecodeAll(OutboundFor(outbound, "monitor"));
    MQTT_CHECK_EQ(packets.size(), 1U);
    MQTT_CHECK_EQ(packets[0].publish.topic, std::string("devices/camera/state"));
    MQTT_CHECK_EQ(PayloadString(packets[0].publish.payload), std::string("offline"));
}

void ScenarioHalfPacketAndMalformedIsolation() {
    // 场景 5：TCP 半包重组 + 恶意帧隔离，不影响其他连接。
    ++g_scenarios;
    std::printf("[SCENARIO] 半包重组与畸形帧隔离\n");
    MqttBroker broker;
    std::vector<MqttOutbound> outbound;
    MQTT_CHECK_EQ(ConnectClient(&broker, "good", 0, &outbound), MqttStatus::kOk);
    outbound.clear();
    MQTT_CHECK_EQ(SubscribeClient(&broker, "good", 1, {{"data/#", 0}}, &outbound),
                  MqttStatus::kOk);
    outbound.clear();

    // 恶意连接：先做半包重组完成 CONNECT，再喂一个保留类型 15 的非法帧。
    // 注意：半包未完成时追加非法字节不会立刻报错（解码器在等 CONNECT 剩余
    // 字节），所以必须先补齐 CONNECT 才能触发非法帧判定。
    MQTT_CHECK_EQ(broker.OpenSession("evil", nullptr), MqttStatus::kOk);
    const std::vector<std::uint8_t> evil = EncodeConnect(BaseConnect("evil", 0));
    MQTT_CHECK_EQ(broker.HandleBytes("evil", evil.data(), 3, &outbound), MqttStatus::kOk);
    MQTT_CHECK_EQ(broker.HandleBytes("evil", evil.data() + 3, evil.size() - 3, &outbound),
                  MqttStatus::kOk);
    const std::vector<std::uint8_t> junk = Bytes({0xF0, 0x00});
    MQTT_CHECK_EQ(broker.HandleBytes("evil", junk.data(), junk.size(), &outbound),
                  MqttStatus::kMalformed);
    // 非法连接被上层关闭，不影响正常连接。
    MQTT_CHECK_EQ(broker.CloseSession("evil", false, &outbound), MqttStatus::kOk);

    // 正常连接继续发布；用半包发送，验证流式重组。
    const std::vector<std::uint8_t> publish =
        EncodePublish(BasePublish("data/sensor/1", "42", 0, 0));
    const std::size_t cut = publish.size() / 2;
    MQTT_CHECK_EQ(broker.HandleBytes("good", publish.data(), cut, &outbound), MqttStatus::kOk);
    MQTT_CHECK_EQ(broker.HandleBytes("good", publish.data() + cut, publish.size() - cut,
                                     &outbound),
                  MqttStatus::kOk);
    const std::vector<MqttPacket> packets = DecodeAll(OutboundFor(outbound, "good"));
    MQTT_CHECK_EQ(packets.size(), 1U);
    MQTT_CHECK_EQ(PayloadString(packets[0].publish.payload), std::string("42"));
}

// ---------------------------------------------------------------------------
// 9. 压测：编码 + 解码吞吐与尾延迟
// ---------------------------------------------------------------------------

std::uint64_t Percentile(std::vector<std::uint64_t> samples, double ratio) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const double index = ratio * static_cast<double>(samples.size() - 1);
    const std::size_t offset = static_cast<std::size_t>(index + 0.5);
    return samples[std::min(offset, samples.size() - 1)];
}

void TestEncodeDecodeThroughput() {
    constexpr int kIterations = 20000;
    std::vector<std::uint64_t> samples;
    samples.reserve(kIterations);
    std::vector<std::uint8_t> payload(32, 'x');
    std::uint64_t mismatches = 0;
    const auto total_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        MqttPublish publish;
        publish.topic = "bench/topic/" + std::to_string(i % 100);
        publish.qos = static_cast<std::uint8_t>(i % 2);
        publish.packet_id = publish.qos == 1 ? static_cast<std::uint16_t>(i % 65535 + 1) : 0;
        publish.payload = payload;
        const auto start = std::chrono::steady_clock::now();
        const std::vector<std::uint8_t> bytes = EncodePublish(publish);
        MqttPacket decoded;
        const MqttStatus status = DecodePacket(bytes, &decoded);
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
        if (status != MqttStatus::kOk || decoded.publish.topic != publish.topic ||
            decoded.publish.payload != publish.payload ||
            decoded.publish.qos != publish.qos) {
            ++mismatches;
        }
    }
    const auto total_end = std::chrono::steady_clock::now();
    const auto total_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(total_end - total_start).count();
    const double total_seconds = static_cast<double>(total_ns) / 1e9;
    const double ops_per_second = total_seconds > 0 ? kIterations / total_seconds : 0.0;
    std::printf(
        "[BENCH] MQTT encode+decode: %d ops, %.2f s, %.0f ops/s, p50=%llu ns, "
        "p95=%llu ns, p99=%llu ns, max=%llu ns\n",
        kIterations, total_seconds, ops_per_second,
        static_cast<unsigned long long>(Percentile(samples, 0.50)),
        static_cast<unsigned long long>(Percentile(samples, 0.95)),
        static_cast<unsigned long long>(Percentile(samples, 0.99)),
        static_cast<unsigned long long>(Percentile(samples, 1.0)));
    MQTT_CHECK_EQ(mismatches, 0U);
    MQTT_CHECK(total_seconds < 5.0);
    MQTT_CHECK(ops_per_second > 20000.0);
    MQTT_CHECK(Percentile(samples, 0.99) < 100000U); // 100us 已是极宽松上界。
}

// ---------------------------------------------------------------------------
// 10. 随机 fuzz：任意字节不崩溃、合法包随机往返
// ---------------------------------------------------------------------------

void TestRandomByteFuzz() {
    std::size_t total_bytes = 0;
    for (int round = 0; round < 200; ++round) {
        std::vector<std::uint8_t> junk(Rand() % 512);
        for (auto& byte : junk) {
            byte = static_cast<std::uint8_t>(Rand() & 0xFFu);
        }
        total_bytes += junk.size();
        MqttDecoder decoder;
        std::size_t offset = 0;
        bool stopped = false;
        while (offset < junk.size() && !stopped) {
            const std::size_t chunk =
                std::min<std::size_t>(1 + Rand() % 32, junk.size() - offset);
            decoder.Append(junk.data() + offset, chunk);
            offset += chunk;
            MqttPacket packet;
            MqttStatus status = MqttStatus::kOk;
            while ((status = decoder.Next(&packet)) == MqttStatus::kOk) {
                MQTT_CHECK(packet.flags <= 0x0F);
            }
            MQTT_CHECK(status == MqttStatus::kNeedMoreData ||
                       status == MqttStatus::kMalformed ||
                       status == MqttStatus::kTooLarge);
            if (status != MqttStatus::kNeedMoreData) {
                stopped = true; // 错误后解码器已清空缓冲。
            }
        }
        MQTT_CHECK(decoder.PendingBytes() <= junk.size());
    }
    MQTT_CHECK(total_bytes > 0);
}

void TestRandomizedRoundtrip() {
    const char* const topics[] = {"a", "a/b", "sensors/room/temp", "/", "a//b",
                                  "devices/device-1/state"};
    for (int i = 0; i < 3000; ++i) {
        MqttPublish publish;
        publish.topic = topics[Rand() % (sizeof(topics) / sizeof(topics[0]))];
        publish.qos = static_cast<std::uint8_t>(Rand() % 2);
        publish.packet_id =
            publish.qos == 1 ? static_cast<std::uint16_t>(Rand() % 65535 + 1) : 0;
        publish.retain = (Rand() % 4) == 0;
        publish.dup = publish.qos == 1 && (Rand() % 4) == 0;
        publish.payload.resize(Rand() % 128);
        for (auto& byte : publish.payload) {
            byte = static_cast<std::uint8_t>(Rand() & 0xFFu);
        }
        const std::vector<std::uint8_t> bytes = EncodePublish(publish);
        MQTT_CHECK(!bytes.empty());
        MqttPacket decoded;
        std::size_t consumed = 0;
        MQTT_CHECK_EQ(DecodePacket(bytes, &decoded, &consumed), MqttStatus::kOk);
        MQTT_CHECK_EQ(consumed, bytes.size());
        MQTT_CHECK_EQ(decoded.publish.topic, publish.topic);
        MQTT_CHECK_EQ(decoded.publish.payload, publish.payload);
        MQTT_CHECK_EQ(decoded.publish.qos, publish.qos);
        MQTT_CHECK_EQ(decoded.publish.packet_id, publish.packet_id);
        MQTT_CHECK_EQ(decoded.publish.retain, publish.retain);
        MQTT_CHECK_EQ(decoded.publish.dup, publish.dup);
    }

    // 随机 CONNECT 往返（字段组合随机）。
    for (int i = 0; i < 500; ++i) {
        MqttConnectOptions options;
        options.client_id = "client-" + std::to_string(Rand() % 1000);
        options.keepalive_seconds = static_cast<std::uint16_t>(Rand() % 65536);
        options.clean_session = (Rand() % 2) == 0;
        options.has_will = (Rand() % 2) == 0;
        if (options.has_will) {
            options.will_topic = "will/" + std::to_string(Rand() % 50);
            options.will_qos = static_cast<std::uint8_t>(Rand() % 2);
            options.will_retain = (Rand() % 2) == 0;
            options.will_message.resize(Rand() % 32);
            for (auto& byte : options.will_message) {
                byte = static_cast<std::uint8_t>(Rand() & 0xFFu);
            }
        }
        options.has_username = (Rand() % 2) == 0;
        if (options.has_username) {
            options.username = "user" + std::to_string(Rand() % 10);
        }
        options.has_password = options.has_username && (Rand() % 2) == 0;
        if (options.has_password) {
            options.password.resize(Rand() % 16);
            for (auto& byte : options.password) {
                byte = static_cast<std::uint8_t>(Rand() & 0xFFu);
            }
        }
        const std::vector<std::uint8_t> bytes = EncodeConnect(options);
        MQTT_CHECK(!bytes.empty());
        MqttPacket decoded;
        MQTT_CHECK_EQ(DecodePacket(bytes, &decoded), MqttStatus::kOk);
        MQTT_CHECK_EQ(decoded.connect.client_id, options.client_id);
        MQTT_CHECK_EQ(decoded.connect.keepalive_seconds, options.keepalive_seconds);
        MQTT_CHECK_EQ(decoded.connect.has_will, options.has_will);
        MQTT_CHECK_EQ(decoded.connect.will_message, options.will_message);
        MQTT_CHECK_EQ(decoded.connect.has_username, options.has_username);
    }

    // 随机单字节篡改：不崩溃即可；成功解码时再走一遍编码也必须成功。
    const std::vector<std::uint8_t> base =
        EncodePublish(BasePublish("mutate/topic", "payload-bytes", 1, 123));
    for (int i = 0; i < 2000; ++i) {
        std::vector<std::uint8_t> mutated = base;
        const std::size_t index = Rand() % mutated.size();
        mutated[index] = static_cast<std::uint8_t>(mutated[index] ^ (1u << (Rand() % 8)));
        MqttPacket decoded;
        const MqttStatus status = DecodePacket(mutated, &decoded);
        MQTT_CHECK(status == MqttStatus::kOk || status == MqttStatus::kMalformed ||
                   status == MqttStatus::kNeedMoreData);
        if (status == MqttStatus::kOk) {
            MQTT_CHECK(decoded.type == MqttPacketType::kPublish ||
                       decoded.type == MqttPacketType::kSubscribe ||
                       decoded.type == MqttPacketType::kPuback);
        }
    }
}

int Finish() {
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] test_mqtt: %d/%d checks failed (%d scenarios)\n",
                     g_failures, g_checks, g_scenarios);
        return 1;
    }
    std::printf("[PASS] test_mqtt: %d checks, %d scenarios\n", g_checks, g_scenarios);
    return 0;
}

}  // namespace

int main() {
    TestVlqBoundaries();
    TestVlqDecodeFailure();
    TestConnectRoundtrip();
    TestConnectEncodingRejects();
    TestConnectDecodeRejects();
    TestConnackRoundtrip();
    TestPublishQos0();
    TestPublishQos1And2Wire();
    TestPuback();
    TestSubscribeSuback();
    TestPingDisconnectAndReservedTypes();
    TestDecoderStreaming();
    TestDecoderErrorsAndLimits();
    TestTopicValidationAndMatching();
    TestSessionStateMachine();
    TestSessionConnectRejections();
    TestSessionHandleBytesAndCleanSession();
    TestKeepalive();
    TestBrokerPubSubLoop();
    TestBrokerWildcardsAndQosDowngrade();
    TestBrokerWill();
    TestBrokerErrorPaths();
    ScenarioTemperatureReport();
    ScenarioQos1Command();
    ScenarioChatRoom();
    ScenarioOfflineWill();
    ScenarioHalfPacketAndMalformedIsolation();
    TestEncodeDecodeThroughput();
    TestRandomByteFuzz();
    TestRandomizedRoundtrip();
    return Finish();
}
