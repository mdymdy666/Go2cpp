/**
 * @file mqtt.cpp
 * @brief MQTT 3.1.1 最小协议骨架：VLQ、九种控制报文编解码、流式解码器、
 *        会话状态机与纯内存 broker。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/bento/net/mqtt.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::bento::net {

namespace {

/// 读取一段缓冲时遇到的三种结果：成功 / 越界（整包已收全，属于格式错误）/ 含 NUL。
enum class ReadResult : std::uint8_t { kOk = 0, kShort = 1, kBad = 2 };

/// 当前毫秒时间戳（steady_clock，仅用于 broker 的 keepalive 计时）。
std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// 有界只读游标：所有读取都检查剩余字节，绝不越界。
class BodyReader {
public:
    BodyReader(const std::uint8_t* data, std::size_t size) noexcept
        : m_data(data), m_size(size) {}

    std::size_t remaining() const noexcept { return m_size - m_offset; }
    bool empty() const noexcept { return m_offset == m_size; }
    const std::uint8_t* current() const noexcept { return m_data + m_offset; }

    ReadResult ReadU8(std::uint8_t* out) noexcept {
        if (out == nullptr) {
            return ReadResult::kBad;
        }
        if (remaining() < 1) {
            return ReadResult::kShort;
        }
        *out = m_data[m_offset];
        ++m_offset;
        return ReadResult::kOk;
    }

    ReadResult ReadU16(std::uint16_t* out) noexcept {
        if (out == nullptr || remaining() < 2) {
            return ReadResult::kShort;
        }
        *out = static_cast<std::uint16_t>((static_cast<std::uint16_t>(m_data[m_offset]) << 8) |
                                          static_cast<std::uint16_t>(m_data[m_offset + 1]));
        m_offset += 2;
        return ReadResult::kOk;
    }

    /// 读取 MQTT UTF-8 字符串（u16 长度前缀）。内嵌 NUL 返回 kBad。
    ReadResult ReadString(std::string* out) {
        if (out == nullptr) {
            return ReadResult::kBad;
        }
        std::uint16_t length = 0;
        const ReadResult head = ReadU16(&length);
        if (head != ReadResult::kOk) {
            return head;
        }
        if (remaining() < length) {
            return ReadResult::kShort;
        }
        const char* begin = reinterpret_cast<const char*>(m_data + m_offset);
        for (std::size_t i = 0; i < length; ++i) {
            if (begin[i] == '\0') {
                return ReadResult::kBad;
            }
        }
        out->assign(begin, static_cast<std::size_t>(length));
        m_offset += length;
        return ReadResult::kOk;
    }

    /// 读取二进制数据（u16 长度前缀，允许任意字节含 NUL）。
    ReadResult ReadBinary(std::vector<std::uint8_t>* out) {
        if (out == nullptr) {
            return ReadResult::kBad;
        }
        std::uint16_t length = 0;
        const ReadResult head = ReadU16(&length);
        if (head != ReadResult::kOk) {
            return head;
        }
        if (remaining() < length) {
            return ReadResult::kShort;
        }
        out->assign(m_data + m_offset, m_data + m_offset + length);
        m_offset += length;
        return ReadResult::kOk;
    }

    /// 读取剩余全部字节（PUBLISH 载荷）。
    ReadResult ReadRemaining(std::vector<std::uint8_t>* out) {
        if (out == nullptr) {
            return ReadResult::kBad;
        }
        out->assign(current(), current() + remaining());
        m_offset = m_size;
        return ReadResult::kOk;
    }

private:
    const std::uint8_t* m_data; ///< 输入起始地址（整包生命周期内有效）。
    std::size_t m_size;         ///< 总字节数。
    std::size_t m_offset = 0;   ///< 当前读取位置。
};

/// 追加 u16（大端）。
void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value >> 8));
    out->push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

/// 追加 MQTT UTF-8 字符串（u16 长度前缀）；超长或含 NUL 返回 false。
bool AppendString(std::vector<std::uint8_t>* out, const std::string& value) {
    if (value.size() > 0xFFFFu || value.find('\0') != std::string::npos) {
        return false;
    }
    AppendU16(out, static_cast<std::uint16_t>(value.size()));
    out->insert(out->end(), value.begin(), value.end());
    return true;
}

/// 追加二进制数据（u16 长度前缀）；超长返回 false。
bool AppendBinary(std::vector<std::uint8_t>* out, const std::vector<std::uint8_t>& value) {
    if (value.size() > 0xFFFFu) {
        return false;
    }
    AppendU16(out, static_cast<std::uint16_t>(value.size()));
    out->insert(out->end(), value.begin(), value.end());
    return true;
}

/// 拼接固定头 + 剩余长度；body_bytes 超限返回 false。
bool AppendHeader(std::vector<std::uint8_t>* out, MqttPacketType type, std::uint8_t flags,
                  std::size_t body_bytes) {
    if (body_bytes > kMqttMaxRemainingLength) {
        return false;
    }
    out->push_back(static_cast<std::uint8_t>((static_cast<std::uint8_t>(type) << 4) | flags));
    std::uint8_t encoded[kMqttMaxRemainingLengthBytes] = {0, 0, 0, 0};
    const std::size_t length = EncodeRemainingLength(static_cast<std::uint32_t>(body_bytes),
                                                     encoded);
    if (length == 0) {
        return false;
    }
    out->insert(out->end(), encoded, encoded + length);
    return true;
}

/// 固定头标志推导（PUBLISH 之外的报文标志是常量）。
std::uint8_t PublishFlags(const MqttPublish& publish) noexcept {
    std::uint8_t flags = 0;
    if (publish.dup) {
        flags |= 0x08u;
    }
    flags |= static_cast<std::uint8_t>((publish.qos & 0x03u) << 1);
    if (publish.retain) {
        flags |= 0x01u;
    }
    return flags;
}

/// 统计订阅表里命中某主题的最高授予 QoS；无命中返回 kMqttNoSubscription。
std::uint8_t BestSubscriptionQos(const MqttSession& session,
                                 const std::string& topic) noexcept {
    std::uint8_t best = kMqttNoSubscription;
    for (const auto& entry : session.subscriptions()) {
        // 注意：kMqttNoSubscription(0xFF) 是哨兵，不能直接与合法 QoS 比大小，
        // 否则 0/1/2 永远小于 255，路由会静默丢弃所有消息。
        if (TopicMatchesFilter(entry.first, topic) &&
            (best == kMqttNoSubscription || entry.second > best)) {
            best = entry.second;
        }
    }
    return best;
}

}  // namespace

const char* MqttStatusName(MqttStatus status) noexcept {
    switch (status) {
        case MqttStatus::kOk:
            return "Ok";
        case MqttStatus::kNeedMoreData:
            return "NeedMoreData";
        case MqttStatus::kMalformed:
            return "Malformed";
        case MqttStatus::kTooLarge:
            return "TooLarge";
        case MqttStatus::kUnsupported:
            return "Unsupported";
        case MqttStatus::kProtocolViolation:
            return "ProtocolViolation";
        case MqttStatus::kInvalidArgument:
            return "InvalidArgument";
    }
    return "Unknown";
}

const char* MqttPacketTypeName(MqttPacketType type) noexcept {
    switch (type) {
        case MqttPacketType::kConnect:
            return "CONNECT";
        case MqttPacketType::kConnack:
            return "CONNACK";
        case MqttPacketType::kPublish:
            return "PUBLISH";
        case MqttPacketType::kPuback:
            return "PUBACK";
        case MqttPacketType::kSubscribe:
            return "SUBSCRIBE";
        case MqttPacketType::kSuback:
            return "SUBACK";
        case MqttPacketType::kPingreq:
            return "PINGREQ";
        case MqttPacketType::kPingresp:
            return "PINGRESP";
        case MqttPacketType::kDisconnect:
            return "DISCONNECT";
    }
    return "Reserved";
}

std::size_t EncodeRemainingLength(std::uint32_t value, std::uint8_t out[4]) noexcept {
    if (out == nullptr || value > kMqttMaxRemainingLength) {
        return 0;
    }
    std::size_t count = 0;
    do {
        std::uint8_t encoded = static_cast<std::uint8_t>(value & 0x7Fu);
        value >>= 7;
        if (value != 0) {
            encoded = static_cast<std::uint8_t>(encoded | 0x80u);
        }
        out[count] = encoded;
        ++count;
    } while (value != 0);
    return count;
}

MqttStatus DecodeRemainingLength(const std::uint8_t* data, std::size_t size,
                                 std::uint32_t* value, std::size_t* consumed) noexcept {
    if (data == nullptr || size == 0) {
        return MqttStatus::kNeedMoreData;
    }
    std::uint32_t decoded = 0;
    std::uint32_t multiplier = 1;
    for (std::size_t i = 0; i < size && i < kMqttMaxRemainingLengthBytes; ++i) {
        const std::uint8_t byte = data[i];
        decoded += static_cast<std::uint32_t>(byte & 0x7Fu) * multiplier;
        if ((byte & 0x80u) == 0) {
            if (value != nullptr) {
                *value = decoded;
            }
            if (consumed != nullptr) {
                *consumed = i + 1;
            }
            return MqttStatus::kOk;
        }
        multiplier *= 128u;
    }
    if (size >= kMqttMaxRemainingLengthBytes) {
        // 4 字节之后仍有继续位：编码长度超过协议上限。
        return MqttStatus::kMalformed;
    }
    return MqttStatus::kNeedMoreData;
}

bool IsValidTopicName(const std::string& topic) noexcept {
    if (topic.empty() || topic.find('\0') != std::string::npos) {
        return false;
    }
    return topic.find('+') == std::string::npos && topic.find('#') == std::string::npos;
}

bool IsValidTopicFilter(const std::string& filter) noexcept {
    if (filter.empty() || filter.find('\0') != std::string::npos) {
        return false;
    }
    std::size_t layer_start = 0;
    for (;;) {
        const std::size_t slash = filter.find('/', layer_start);
        const std::size_t layer_end =
            (slash == std::string::npos) ? filter.size() : slash;
        const std::size_t layer_length = layer_end - layer_start;
        for (std::size_t i = layer_start; i < layer_end; ++i) {
            if (filter[i] == '#') {
                // '#' 必须独占一层，且必须是最后一层。
                if (layer_length != 1 || slash != std::string::npos) {
                    return false;
                }
            } else if (filter[i] == '+') {
                // '+' 必须独占一层（可以是最后一层）。
                if (layer_length != 1) {
                    return false;
                }
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        layer_start = slash + 1;
    }
    return true;
}

bool TopicMatchesFilter(const std::string& filter, const std::string& topic) noexcept {
    if (filter.empty() || topic.empty()) {
        return false;
    }
    // §4.7.2：以 '$' 开头的主题不匹配以通配符开头的过滤器。
    if (topic[0] == '$' && (filter[0] == '#' || filter[0] == '+')) {
        return false;
    }
    std::size_t filter_pos = 0;
    std::size_t topic_pos = 0;
    for (;;) {
        const std::size_t filter_slash = filter.find('/', filter_pos);
        const std::size_t filter_end =
            (filter_slash == std::string::npos) ? filter.size() : filter_slash;
        const std::size_t filter_length = filter_end - filter_pos;
        const std::size_t topic_slash = topic.find('/', topic_pos);
        const bool topic_done = (topic_slash == std::string::npos);
        const std::size_t topic_end = topic_done ? topic.size() : topic_slash;
        const std::size_t topic_length = topic_end - topic_pos;

        if (filter_length == 1 && filter[filter_pos] == '#') {
            return true;
        }
        bool matched = false;
        if (filter_length == 1 && filter[filter_pos] == '+') {
            matched = true;
        } else {
            matched = filter_length == topic_length &&
                      filter.compare(filter_pos, filter_length, topic, topic_pos,
                                     topic_length) == 0;
        }
        if (!matched) {
            return false;
        }
        const bool filter_done = (filter_slash == std::string::npos);
        if (filter_done) {
            return topic_done;
        }
        if (topic_done) {
            // "a/#" 也匹配父层 "a"。
            return filter_slash + 2 == filter.size() && filter[filter_slash + 1] == '#';
        }
        filter_pos = filter_slash + 1;
        topic_pos = topic_slash + 1;
    }
}

std::vector<std::uint8_t> EncodeConnect(const MqttConnectOptions& options) {
    if (options.protocol_name != kMqttProtocolName || options.will_qos > 2) {
        return {};
    }
    if (!options.has_will && (options.will_qos != 0 || options.will_retain ||
                              !options.will_topic.empty() ||
                              !options.will_message.empty())) {
        return {};
    }
    if (options.has_will && !IsValidTopicName(options.will_topic)) {
        return {};
    }
    if (options.has_password && !options.has_username) {
        return {};
    }

    std::uint8_t connect_flags = 0;
    if (options.has_username) {
        connect_flags |= 0x80u;
    }
    if (options.has_password) {
        connect_flags |= 0x40u;
    }
    if (options.has_will) {
        connect_flags |= 0x04u;
        connect_flags |= static_cast<std::uint8_t>((options.will_qos & 0x03u) << 3);
        if (options.will_retain) {
            connect_flags |= 0x20u;
        }
    }
    if (options.clean_session) {
        connect_flags |= 0x02u;
    }

    std::vector<std::uint8_t> body;
    body.reserve(64 + options.client_id.size() + options.will_topic.size() +
                 options.will_message.size() + options.username.size() +
                 options.password.size());
    if (!AppendString(&body, options.protocol_name)) {
        return {};
    }
    body.push_back(options.protocol_level);
    body.push_back(connect_flags);
    AppendU16(&body, options.keepalive_seconds);
    if (!AppendString(&body, options.client_id)) {
        return {};
    }
    if (options.has_will) {
        if (!AppendString(&body, options.will_topic) ||
            !AppendBinary(&body, options.will_message)) {
            return {};
        }
    }
    if (options.has_username && !AppendString(&body, options.username)) {
        return {};
    }
    if (options.has_password && !AppendBinary(&body, options.password)) {
        return {};
    }

    std::vector<std::uint8_t> packet;
    packet.reserve(4 + body.size());
    if (!AppendHeader(&packet, MqttPacketType::kConnect, 0, body.size())) {
        return {};
    }
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

std::vector<std::uint8_t> EncodeConnack(const MqttConnack& connack) {
    if (connack.return_code > 5) {
        return {};
    }
    // §3.2.2.2：return code 非 0 时 session present 必须为 0。
    if (connack.return_code != 0 && connack.session_present) {
        return {};
    }
    std::vector<std::uint8_t> packet = {0x20, 0x02,
                                       static_cast<std::uint8_t>(connack.session_present ? 1 : 0),
                                       connack.return_code};
    return packet;
}

std::vector<std::uint8_t> EncodePublish(const MqttPublish& publish) {
    if (!IsValidTopicName(publish.topic) || publish.qos > 2) {
        return {};
    }
    if (publish.qos == 0) {
        if (publish.dup || publish.packet_id != 0) {
            return {};
        }
    } else if (publish.packet_id == 0) {
        return {};
    }

    std::vector<std::uint8_t> body;
    body.reserve(2 + publish.topic.size() + 2 + publish.payload.size());
    if (!AppendString(&body, publish.topic)) {
        return {};
    }
    if (publish.qos > 0) {
        AppendU16(&body, publish.packet_id);
    }
    body.insert(body.end(), publish.payload.begin(), publish.payload.end());

    std::vector<std::uint8_t> packet;
    packet.reserve(4 + body.size());
    if (!AppendHeader(&packet, MqttPacketType::kPublish, PublishFlags(publish), body.size())) {
        return {};
    }
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

std::vector<std::uint8_t> EncodePuback(std::uint16_t packet_id) {
    if (packet_id == 0) {
        return {};
    }
    std::vector<std::uint8_t> packet = {0x40, 0x02, 0, 0};
    packet[2] = static_cast<std::uint8_t>(packet_id >> 8);
    packet[3] = static_cast<std::uint8_t>(packet_id & 0xFFu);
    return packet;
}

std::vector<std::uint8_t> EncodeSubscribe(
    std::uint16_t packet_id, const std::vector<MqttSubscription>& subscriptions) {
    if (packet_id == 0 || subscriptions.empty()) {
        return {};
    }
    std::vector<std::uint8_t> body;
    AppendU16(&body, packet_id);
    for (const auto& subscription : subscriptions) {
        if (!IsValidTopicFilter(subscription.filter) || subscription.qos > 2) {
            return {};
        }
        if (!AppendString(&body, subscription.filter)) {
            return {};
        }
        body.push_back(subscription.qos);
    }

    std::vector<std::uint8_t> packet;
    packet.reserve(4 + body.size());
    if (!AppendHeader(&packet, MqttPacketType::kSubscribe, 0x02u, body.size())) {
        return {};
    }
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

std::vector<std::uint8_t> EncodeSuback(std::uint16_t packet_id,
                                       const std::vector<std::uint8_t>& codes) {
    if (packet_id == 0 || codes.empty()) {
        return {};
    }
    for (const std::uint8_t code : codes) {
        if (code != 0 && code != 1 && code != 2 && code != kMqttSubackFailure) {
            return {};
        }
    }
    std::vector<std::uint8_t> body;
    body.reserve(2 + codes.size());
    AppendU16(&body, packet_id);
    body.insert(body.end(), codes.begin(), codes.end());

    std::vector<std::uint8_t> packet;
    packet.reserve(4 + body.size());
    if (!AppendHeader(&packet, MqttPacketType::kSuback, 0, body.size())) {
        return {};
    }
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

std::vector<std::uint8_t> EncodePingreq() {
    return {0xC0, 0x00};
}

std::vector<std::uint8_t> EncodePingresp() {
    return {0xD0, 0x00};
}

std::vector<std::uint8_t> EncodeDisconnect() {
    return {0xE0, 0x00};
}

namespace {

/// 解析 CONNECT 的 body（协议名之后的部分）。整包已完整，越界即格式错误。
MqttStatus DecodeConnectBody(BodyReader* reader, MqttConnectOptions* options) {
    std::string protocol_name;
    if (reader->ReadString(&protocol_name) != ReadResult::kOk) {
        return MqttStatus::kMalformed;
    }
    if (protocol_name != kMqttProtocolName) {
        return MqttStatus::kMalformed;
    }
    options->protocol_name = std::move(protocol_name);

    std::uint8_t protocol_level = 0;
    std::uint8_t connect_flags = 0;
    std::uint16_t keepalive = 0;
    if (reader->ReadU8(&protocol_level) != ReadResult::kOk ||
        reader->ReadU8(&connect_flags) != ReadResult::kOk ||
        reader->ReadU16(&keepalive) != ReadResult::kOk) {
        return MqttStatus::kMalformed;
    }
    if ((connect_flags & 0x01u) != 0) {
        return MqttStatus::kMalformed; // 保留位必须为 0。
    }
    const bool has_username = (connect_flags & 0x80u) != 0;
    const bool has_password = (connect_flags & 0x40u) != 0;
    const bool will_retain = (connect_flags & 0x20u) != 0;
    const std::uint8_t will_qos =
        static_cast<std::uint8_t>((connect_flags >> 3) & 0x03u);
    const bool has_will = (connect_flags & 0x04u) != 0;
    const bool clean_session = (connect_flags & 0x02u) != 0;

    if (!has_will) {
        if (will_qos != 0 || will_retain) {
            return MqttStatus::kMalformed;
        }
    } else if (will_qos == 3) {
        return MqttStatus::kMalformed;
    }
    if (has_password && !has_username) {
        return MqttStatus::kMalformed; // §3.1.2-22。
    }

    options->protocol_level = protocol_level;
    options->clean_session = clean_session;
    options->keepalive_seconds = keepalive;
    options->has_username = has_username;
    options->has_password = has_password;
    options->has_will = has_will;
    options->will_qos = will_qos;
    options->will_retain = will_retain;

    if (reader->ReadString(&options->client_id) != ReadResult::kOk) {
        return MqttStatus::kMalformed;
    }
    if (has_will) {
        if (reader->ReadString(&options->will_topic) != ReadResult::kOk ||
            reader->ReadBinary(&options->will_message) != ReadResult::kOk ||
            !IsValidTopicName(options->will_topic)) {
            return MqttStatus::kMalformed;
        }
    }
    if (has_username && reader->ReadString(&options->username) != ReadResult::kOk) {
        return MqttStatus::kMalformed;
    }
    if (has_password && reader->ReadBinary(&options->password) != ReadResult::kOk) {
        return MqttStatus::kMalformed;
    }
    if (!reader->empty()) {
        return MqttStatus::kMalformed; // CONNECT 允许的字段已全部解析完。
    }
    return MqttStatus::kOk;
}

/// 解析 CONNACK。
MqttStatus DecodeConnackBody(BodyReader* reader, MqttConnack* connack) {
    if (reader->remaining() != 2) {
        return MqttStatus::kMalformed;
    }
    std::uint8_t ack_flags = 0;
    std::uint8_t return_code = 0;
    if (reader->ReadU8(&ack_flags) != ReadResult::kOk ||
        reader->ReadU8(&return_code) != ReadResult::kOk) {
        return MqttStatus::kMalformed;
    }
    if ((ack_flags & 0xFEu) != 0 || return_code > 5) {
        return MqttStatus::kMalformed;
    }
    connack->session_present = (ack_flags & 0x01u) != 0;
    connack->return_code = return_code;
    if (return_code != 0 && connack->session_present) {
        return MqttStatus::kMalformed;
    }
    return MqttStatus::kOk;
}

/// 解析 PUBLISH 的 body，flags 已由调用方校验。
MqttStatus DecodePublishBody(BodyReader* reader, std::uint8_t flags, MqttPublish* publish) {
    publish->dup = (flags & 0x08u) != 0;
    publish->qos = static_cast<std::uint8_t>((flags >> 1) & 0x03u);
    publish->retain = (flags & 0x01u) != 0;
    if (publish->qos == 3 || (publish->qos == 0 && publish->dup)) {
        return MqttStatus::kMalformed;
    }
    if (reader->ReadString(&publish->topic) != ReadResult::kOk ||
        !IsValidTopicName(publish->topic)) {
        return MqttStatus::kMalformed;
    }
    if (publish->qos > 0) {
        if (reader->ReadU16(&publish->packet_id) != ReadResult::kOk ||
            publish->packet_id == 0) {
            return MqttStatus::kMalformed;
        }
    }
    if (reader->ReadRemaining(&publish->payload) != ReadResult::kOk) {
        return MqttStatus::kMalformed;
    }
    return MqttStatus::kOk;
}

/// 解析 SUBSCRIBE 的 body。
MqttStatus DecodeSubscribeBody(BodyReader* reader, std::uint16_t* packet_id,
                               std::vector<MqttSubscription>* subscriptions) {
    if (reader->ReadU16(packet_id) != ReadResult::kOk || *packet_id == 0) {
        return MqttStatus::kMalformed;
    }
    while (!reader->empty()) {
        MqttSubscription subscription;
        if (reader->ReadString(&subscription.filter) != ReadResult::kOk ||
            !IsValidTopicFilter(subscription.filter) ||
            reader->ReadU8(&subscription.qos) != ReadResult::kOk ||
            subscription.qos > 2) {
            return MqttStatus::kMalformed;
        }
        subscriptions->push_back(std::move(subscription));
    }
    if (subscriptions->empty()) {
        return MqttStatus::kMalformed;
    }
    return MqttStatus::kOk;
}

/// 解析 SUBACK 的 body。
MqttStatus DecodeSubackBody(BodyReader* reader, std::uint16_t* packet_id,
                            std::vector<std::uint8_t>* codes) {
    if (reader->ReadU16(packet_id) != ReadResult::kOk || *packet_id == 0) {
        return MqttStatus::kMalformed;
    }
    while (!reader->empty()) {
        std::uint8_t code = 0;
        if (reader->ReadU8(&code) != ReadResult::kOk) {
            return MqttStatus::kMalformed;
        }
        if (code != 0 && code != 1 && code != 2 && code != kMqttSubackFailure) {
            return MqttStatus::kMalformed;
        }
        codes->push_back(code);
    }
    if (codes->empty()) {
        return MqttStatus::kMalformed;
    }
    return MqttStatus::kOk;
}

}  // namespace

MqttStatus DecodePacket(const std::uint8_t* data, std::size_t size, MqttPacket* out,
                        std::size_t* consumed) {
    if (out == nullptr) {
        return MqttStatus::kInvalidArgument;
    }
    if (data == nullptr || size == 0) {
        return MqttStatus::kNeedMoreData;
    }

    const std::uint8_t first = data[0];
    const std::uint8_t type_nibble = static_cast<std::uint8_t>(first >> 4);
    const std::uint8_t flags = static_cast<std::uint8_t>(first & 0x0Fu);
    if (type_nibble == 0 || type_nibble == 15) {
        return MqttStatus::kMalformed;
    }
    std::uint32_t remaining = 0;
    std::size_t vlq_bytes = 0;
    const MqttStatus vlq_status =
        DecodeRemainingLength(data + 1, size - 1, &remaining, &vlq_bytes);
    if (vlq_status != MqttStatus::kOk) {
        return vlq_status;
    }
    const std::size_t header_bytes = 1 + vlq_bytes;
    if (remaining > size - header_bytes) {
        return MqttStatus::kNeedMoreData;
    }

    MqttPacket packet;
    packet.type = static_cast<MqttPacketType>(type_nibble);
    packet.flags = flags;
    BodyReader reader(data + header_bytes, remaining);
    MqttStatus status = MqttStatus::kMalformed;

    switch (packet.type) {
        case MqttPacketType::kConnect:
            if (flags != 0) {
                return MqttStatus::kMalformed;
            }
            status = DecodeConnectBody(&reader, &packet.connect);
            break;
        case MqttPacketType::kConnack:
            if (flags != 0) {
                return MqttStatus::kMalformed;
            }
            status = DecodeConnackBody(&reader, &packet.connack);
            break;
        case MqttPacketType::kPublish:
            status = DecodePublishBody(&reader, flags, &packet.publish);
            break;
        case MqttPacketType::kPuback: {
            if (flags != 0 || remaining != 2) {
                return MqttStatus::kMalformed;
            }
            std::uint16_t packet_id = 0;
            if (reader.ReadU16(&packet_id) != ReadResult::kOk || packet_id == 0) {
                return MqttStatus::kMalformed;
            }
            packet.packet_id = packet_id;
            status = MqttStatus::kOk;
            break;
        }
        case MqttPacketType::kSubscribe:
            if (flags != 0x02u) {
                return MqttStatus::kMalformed;
            }
            status = DecodeSubscribeBody(&reader, &packet.packet_id, &packet.subscriptions);
            break;
        case MqttPacketType::kSuback:
            if (flags != 0) {
                return MqttStatus::kMalformed;
            }
            status = DecodeSubackBody(&reader, &packet.packet_id, &packet.suback_codes);
            break;
        case MqttPacketType::kPingreq:
        case MqttPacketType::kPingresp:
        case MqttPacketType::kDisconnect:
            if (flags != 0 || remaining != 0) {
                return MqttStatus::kMalformed;
            }
            status = MqttStatus::kOk;
            break;
    }
    if (status != MqttStatus::kOk) {
        return status;
    }
    if (!reader.empty()) {
        return MqttStatus::kMalformed;
    }
    if (consumed != nullptr) {
        *consumed = header_bytes + remaining;
    }
    *out = std::move(packet);
    return MqttStatus::kOk;
}

MqttStatus DecodePacket(const std::vector<std::uint8_t>& data, MqttPacket* out,
                        std::size_t* consumed) {
    return DecodePacket(data.data(), data.size(), out, consumed);
}

MqttDecoder::MqttDecoder(std::uint32_t max_packet_bytes) noexcept
    : m_max_packet_bytes(max_packet_bytes) {}

void MqttDecoder::Append(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    if (m_offset > 0) {
        m_buffer.erase(m_buffer.begin(),
                       m_buffer.begin() + static_cast<std::ptrdiff_t>(m_offset));
        m_offset = 0;
    }
    m_buffer.insert(m_buffer.end(), data, data + size);
}

MqttStatus MqttDecoder::Next(MqttPacket* out) {
    if (out == nullptr) {
        return MqttStatus::kInvalidArgument;
    }
    if (PendingBytes() == 0) {
        return MqttStatus::kNeedMoreData;
    }
    const std::uint8_t* base = m_buffer.data() + m_offset;
    const std::size_t available = PendingBytes();
    const std::uint8_t type_nibble = static_cast<std::uint8_t>(base[0] >> 4);
    if (type_nibble == 0 || type_nibble == 15) {
        Clear();
        return MqttStatus::kMalformed;
    }
    std::uint32_t remaining = 0;
    std::size_t vlq_bytes = 0;
    const MqttStatus vlq_status =
        DecodeRemainingLength(base + 1, available - 1, &remaining, &vlq_bytes);
    if (vlq_status == MqttStatus::kNeedMoreData) {
        return MqttStatus::kNeedMoreData;
    }
    if (vlq_status != MqttStatus::kOk) {
        Clear();
        return vlq_status;
    }
    if (remaining > m_max_packet_bytes) {
        Clear();
        return MqttStatus::kTooLarge;
    }
    const std::size_t header_bytes = 1 + vlq_bytes;
    if (available - header_bytes < remaining) {
        return MqttStatus::kNeedMoreData;
    }
    std::size_t consumed = 0;
    const MqttStatus status = DecodePacket(base, available, out, &consumed);
    if (status != MqttStatus::kOk) {
        Clear();
        return status;
    }
    m_offset += consumed;
    if (m_offset == m_buffer.size()) {
        m_buffer.clear();
        m_offset = 0;
    }
    return MqttStatus::kOk;
}

void MqttDecoder::Clear() noexcept {
    m_buffer.clear();
    m_offset = 0;
}

MqttSession::MqttSession(MqttSessionConfig config) : m_config(std::move(config)) {}

MqttStatus MqttSession::HandleConnect(const MqttPacket& packet,
                                      std::vector<std::uint8_t>* response) {
    if (m_connected) {
        return MqttStatus::kProtocolViolation;
    }
    if (response != nullptr) {
        response->clear();
    }
    const MqttConnectOptions& options = packet.connect;
    std::uint8_t return_code = 0;
    if (options.protocol_level != kMqttProtocolLevel) {
        return_code = 1; // 不可接受的协议版本。
    } else if (options.client_id.empty() && !options.clean_session) {
        return_code = 2; // client id 被拒绝。
    } else if (options.has_will && options.will_qos > kMqttHighestSupportedQos) {
        // QoS 2 遗嘱不在骨架能力内：拒绝整条连接，而不是降级后悄悄投递。
        return MqttStatus::kUnsupported;
    }
    if (return_code != 0) {
        if (response != nullptr) {
            *response = EncodeConnack(MqttConnack{false, return_code});
        }
        return MqttStatus::kOk;
    }

    m_connect_options = options;
    if (!options.client_id.empty()) {
        m_config.client_id = options.client_id;
    }
    m_config.keepalive_seconds = options.keepalive_seconds;
    m_config.clean_session = options.clean_session;
    if (options.clean_session) {
        m_subscriptions.clear();
    }
    m_connected = true;
    m_disconnected = false;
    if (options.has_will) {
        m_has_pending_will = true;
        m_will = MqttPublish{};
        m_will.topic = options.will_topic;
        m_will.payload = options.will_message;
        m_will.qos = options.will_qos;
        m_will.retain = options.will_retain;
    } else {
        m_has_pending_will = false;
        m_will = MqttPublish{};
    }
    if (response != nullptr) {
        *response = EncodeConnack(MqttConnack{false, 0});
    }
    return MqttStatus::kOk;
}

MqttStatus MqttSession::HandleSubscribe(const MqttPacket& packet,
                                        std::vector<std::uint8_t>* response) {
    if (!m_connected) {
        return MqttStatus::kProtocolViolation;
    }
    if (packet.packet_id == 0 || packet.subscriptions.empty()) {
        return MqttStatus::kMalformed;
    }
    std::vector<std::uint8_t> codes;
    codes.reserve(packet.subscriptions.size());
    for (const auto& subscription : packet.subscriptions) {
        if (!IsValidTopicFilter(subscription.filter) || subscription.qos > 2) {
            return MqttStatus::kMalformed;
        }
        if (subscription.qos > kMqttHighestSupportedQos) {
            codes.push_back(kMqttSubackFailure); // 骨架不支持 QoS 2：明确拒绝。
            continue;
        }
        m_subscriptions[subscription.filter] = subscription.qos;
        codes.push_back(subscription.qos);
    }
    if (response != nullptr) {
        *response = EncodeSuback(packet.packet_id, codes);
    }
    return MqttStatus::kOk;
}

MqttStatus MqttSession::HandlePacket(const MqttPacket& packet,
                                     std::vector<std::uint8_t>* response) {
    if (response != nullptr) {
        response->clear();
    }
    switch (packet.type) {
        case MqttPacketType::kConnect:
            return HandleConnect(packet, response);
        case MqttPacketType::kSubscribe:
            return HandleSubscribe(packet, response);
        case MqttPacketType::kPublish:
            if (!m_connected) {
                return MqttStatus::kProtocolViolation;
            }
            if (packet.publish.qos > kMqttHighestSupportedQos) {
                return MqttStatus::kUnsupported;
            }
            if (packet.publish.qos == 1) {
                if (response == nullptr) {
                    return MqttStatus::kInvalidArgument;
                }
                *response = EncodePuback(packet.publish.packet_id);
            }
            return MqttStatus::kOk;
        case MqttPacketType::kPuback:
            if (!m_connected) {
                return MqttStatus::kProtocolViolation;
            }
            if (packet.packet_id == 0) {
                return MqttStatus::kMalformed;
            }
            return MqttStatus::kOk;
        case MqttPacketType::kPingreq:
            if (!m_connected) {
                return MqttStatus::kProtocolViolation;
            }
            if (response != nullptr) {
                *response = EncodePingresp();
            }
            return MqttStatus::kOk;
        case MqttPacketType::kPingresp:
            return m_connected ? MqttStatus::kOk : MqttStatus::kProtocolViolation;
        case MqttPacketType::kDisconnect:
            if (!m_connected) {
                return MqttStatus::kProtocolViolation;
            }
            m_connected = false;
            m_disconnected = true;
            m_has_pending_will = false; // 正常断开不投递遗嘱。
            return MqttStatus::kOk;
        case MqttPacketType::kConnack:
        case MqttPacketType::kSuback:
            // 服务端会话不应收到这两个方向相反的报文。
            return MqttStatus::kProtocolViolation;
    }
    return MqttStatus::kMalformed;
}

MqttStatus MqttSession::HandleBytes(const std::uint8_t* data, std::size_t size,
                                    std::vector<std::vector<std::uint8_t>>* responses) {
    m_decoder.Append(data, size);
    MqttPacket packet;
    for (;;) {
        const MqttStatus decode_status = m_decoder.Next(&packet);
        if (decode_status == MqttStatus::kNeedMoreData) {
            return MqttStatus::kOk;
        }
        if (decode_status != MqttStatus::kOk) {
            return decode_status;
        }
        std::vector<std::uint8_t> response;
        const MqttStatus handle_status = HandlePacket(packet, &response);
        if (handle_status != MqttStatus::kOk) {
            return handle_status;
        }
        if (responses != nullptr && !response.empty()) {
            responses->push_back(std::move(response));
        }
        if (packet.type == MqttPacketType::kDisconnect) {
            return MqttStatus::kOk;
        }
    }
}

bool MqttSession::HasSubscription(const std::string& filter) const noexcept {
    return m_subscriptions.find(filter) != m_subscriptions.end();
}

std::uint8_t MqttSession::SubscriptionQos(const std::string& filter) const noexcept {
    const auto it = m_subscriptions.find(filter);
    return it == m_subscriptions.end() ? kMqttNoSubscription : it->second;
}

std::uint64_t MqttSession::KeepaliveTimeoutMs() const noexcept {
    if (m_config.keepalive_seconds == 0) {
        return 0;
    }
    // 1.5 倍 keepalive：秒 -> 毫秒，避免浮点。
    const std::uint64_t base = static_cast<std::uint64_t>(m_config.keepalive_seconds) * 1000u;
    return base + base / 2u;
}

bool MqttSession::KeepaliveExpired(std::uint64_t now_ms) const noexcept {
    if (!m_connected || m_config.keepalive_seconds == 0) {
        return false;
    }
    const std::uint64_t timeout = KeepaliveTimeoutMs();
    if (now_ms < m_last_activity_ms) {
        return false; // 时钟回拨不误判。
    }
    return now_ms - m_last_activity_ms > timeout;
}

void MqttSession::Reset() noexcept {
    m_connected = false;
    m_disconnected = false;
    m_has_pending_will = false;
    m_subscriptions.clear();
    m_will = MqttPublish{};
    m_connect_options = MqttConnectOptions{};
    m_decoder.Clear();
    m_last_activity_ms = 0;
    m_config.keepalive_seconds = 0;
}

MqttBroker::MqttBroker(std::uint32_t max_packet_bytes) : m_max_packet_bytes(max_packet_bytes) {}

MqttStatus MqttBroker::OpenSession(const std::string& requested_client_id,
                                   std::string* assigned_id) {
    if (requested_client_id.find('\0') != std::string::npos) {
        return MqttStatus::kInvalidArgument;
    }
    std::string client_id = requested_client_id;
    if (client_id.empty()) {
        client_id = "auto-" + std::to_string(m_next_auto_id);
        ++m_next_auto_id;
    }
    // 同一 client_id 重连：顶替旧连接（不保留旧订阅/半包缓冲）。
    m_sessions.erase(client_id);
    ClientEntry entry{MqttSession{MqttSessionConfig{client_id, 0, true}},
                      MqttDecoder{m_max_packet_bytes}};
    m_sessions.emplace(client_id, std::move(entry));
    if (assigned_id != nullptr) {
        *assigned_id = client_id;
    }
    return MqttStatus::kOk;
}

MqttSession* MqttBroker::FindMutable(const std::string& client_id) noexcept {
    const auto it = m_sessions.find(client_id);
    return it == m_sessions.end() ? nullptr : &it->second.session;
}

const MqttSession* MqttBroker::FindSession(const std::string& client_id) const noexcept {
    const auto it = m_sessions.find(client_id);
    return it == m_sessions.end() ? nullptr : &it->second.session;
}

bool MqttBroker::HasSession(const std::string& client_id) const noexcept {
    return m_sessions.find(client_id) != m_sessions.end();
}

std::uint16_t MqttBroker::AllocatePacketId() noexcept {
    if (m_next_packet_id == 0) {
        m_next_packet_id = 1;
    }
    const std::uint16_t id = m_next_packet_id;
    ++m_next_packet_id;
    if (m_next_packet_id == 0) {
        m_next_packet_id = 1;
    }
    return id;
}

MqttStatus MqttBroker::HandleConnect(const std::string& client_id, const MqttPacket& packet,
                                     std::vector<MqttOutbound>* outbound) {
    MqttSession* session = FindMutable(client_id);
    if (session == nullptr) {
        return MqttStatus::kInvalidArgument;
    }
    // 空 client_id 由 broker 分配（OpenSession），允许报文留空；非空则必须一致。
    if (!packet.connect.client_id.empty() && packet.connect.client_id != client_id) {
        return MqttStatus::kProtocolViolation;
    }
    std::vector<std::uint8_t> response;
    const MqttStatus status = session->HandlePacket(packet, &response);
    if (status != MqttStatus::kOk) {
        return status;
    }
    session->Touch(NowMs());
    if (!response.empty() && outbound != nullptr) {
        outbound->push_back(MqttOutbound{client_id, std::move(response)});
    }
    return MqttStatus::kOk;
}

MqttStatus MqttBroker::HandleSubscribe(const std::string& client_id,
                                       const MqttPacket& packet,
                                       std::vector<MqttOutbound>* outbound) {
    MqttSession* session = FindMutable(client_id);
    if (session == nullptr) {
        return MqttStatus::kInvalidArgument;
    }
    std::vector<std::uint8_t> response;
    const MqttStatus status = session->HandlePacket(packet, &response);
    if (status != MqttStatus::kOk) {
        return status;
    }
    session->Touch(NowMs());
    if (!response.empty() && outbound != nullptr) {
        outbound->push_back(MqttOutbound{client_id, std::move(response)});
    }
    return MqttStatus::kOk;
}

void MqttBroker::RoutePublish(const MqttPublish& publish,
                              std::vector<MqttOutbound>* outbound) {
    if (outbound == nullptr || !IsValidTopicName(publish.topic)) {
        return;
    }
    for (const auto& entry : m_sessions) {
        const MqttSession& session = entry.second.session;
        if (!session.connected()) {
            continue;
        }
        const std::uint8_t best_qos = BestSubscriptionQos(session, publish.topic);
        if (best_qos == kMqttNoSubscription) {
            continue;
        }
        MqttPublish routed;
        routed.topic = publish.topic;
        routed.payload = publish.payload;
        routed.qos = std::min(publish.qos, best_qos);
        routed.retain = publish.retain;
        routed.dup = false;
        if (routed.qos > 0) {
            routed.packet_id = AllocatePacketId();
        }
        std::vector<std::uint8_t> bytes = EncodePublish(routed);
        if (bytes.empty()) {
            continue;
        }
        outbound->push_back(MqttOutbound{entry.first, std::move(bytes)});
    }
}

MqttStatus MqttBroker::HandlePublish(const std::string& client_id, const MqttPacket& packet,
                                     std::vector<MqttOutbound>* outbound) {
    MqttSession* session = FindMutable(client_id);
    if (session == nullptr) {
        return MqttStatus::kInvalidArgument;
    }
    if (!session->connected()) {
        return MqttStatus::kProtocolViolation;
    }
    if (!IsValidTopicName(packet.publish.topic)) {
        return MqttStatus::kMalformed;
    }
    if (packet.publish.qos > kMqttHighestSupportedQos) {
        return MqttStatus::kUnsupported;
    }
    std::vector<std::uint8_t> response;
    const MqttStatus status = session->HandlePacket(packet, &response);
    if (status != MqttStatus::kOk) {
        return status;
    }
    session->Touch(NowMs());
    if (!response.empty() && outbound != nullptr) {
        outbound->push_back(MqttOutbound{client_id, std::move(response)});
    }
    RoutePublish(packet.publish, outbound);
    return MqttStatus::kOk;
}

MqttStatus MqttBroker::HandlePacket(const std::string& client_id, const MqttPacket& packet,
                                    std::vector<MqttOutbound>* outbound) {
    switch (packet.type) {
        case MqttPacketType::kConnect:
            return HandleConnect(client_id, packet, outbound);
        case MqttPacketType::kSubscribe:
            return HandleSubscribe(client_id, packet, outbound);
        case MqttPacketType::kPublish:
            return HandlePublish(client_id, packet, outbound);
        case MqttPacketType::kPuback: {
            MqttSession* session = FindMutable(client_id);
            if (session == nullptr) {
                return MqttStatus::kInvalidArgument;
            }
            const MqttStatus status = session->HandlePacket(packet, nullptr);
            if (status == MqttStatus::kOk) {
                session->Touch(NowMs());
            }
            return status;
        }
        case MqttPacketType::kPingreq: {
            MqttSession* session = FindMutable(client_id);
            if (session == nullptr) {
                return MqttStatus::kInvalidArgument;
            }
            std::vector<std::uint8_t> response;
            const MqttStatus status = session->HandlePacket(packet, &response);
            if (status != MqttStatus::kOk) {
                return status;
            }
            session->Touch(NowMs());
            if (outbound != nullptr) {
                outbound->push_back(MqttOutbound{client_id, std::move(response)});
            }
            return MqttStatus::kOk;
        }
        case MqttPacketType::kPingresp: {
            MqttSession* session = FindMutable(client_id);
            if (session == nullptr) {
                return MqttStatus::kInvalidArgument;
            }
            const MqttStatus status = session->HandlePacket(packet, nullptr);
            if (status == MqttStatus::kOk) {
                session->Touch(NowMs());
            }
            return status;
        }
        case MqttPacketType::kDisconnect: {
            MqttSession* session = FindMutable(client_id);
            if (session == nullptr) {
                return MqttStatus::kInvalidArgument;
            }
            if (!session->connected()) {
                return MqttStatus::kProtocolViolation;
            }
            session->HandlePacket(packet, nullptr);
            m_sessions.erase(client_id);
            return MqttStatus::kOk;
        }
        case MqttPacketType::kConnack:
        case MqttPacketType::kSuback:
            return MqttStatus::kProtocolViolation;
    }
    return MqttStatus::kMalformed;
}

MqttStatus MqttBroker::HandleBytes(const std::string& client_id, const std::uint8_t* data,
                                   std::size_t size, std::vector<MqttOutbound>* outbound) {
    {
        const auto it = m_sessions.find(client_id);
        if (it == m_sessions.end()) {
            return MqttStatus::kInvalidArgument;
        }
        it->second.decoder.Append(data, size);
    }
    MqttPacket packet;
    for (;;) {
        const auto it = m_sessions.find(client_id);
        if (it == m_sessions.end()) {
            return MqttStatus::kInvalidArgument;
        }
        const MqttStatus decode_status = it->second.decoder.Next(&packet);
        if (decode_status == MqttStatus::kNeedMoreData) {
            return MqttStatus::kOk;
        }
        if (decode_status != MqttStatus::kOk) {
            return decode_status;
        }
        const MqttStatus handle_status = HandlePacket(client_id, packet, outbound);
        if (handle_status != MqttStatus::kOk) {
            return handle_status;
        }
        if (packet.type == MqttPacketType::kDisconnect) {
            return MqttStatus::kOk;
        }
    }
}

MqttStatus MqttBroker::CloseSession(const std::string& client_id, bool graceful,
                                    std::vector<MqttOutbound>* outbound) {
    const auto it = m_sessions.find(client_id);
    if (it == m_sessions.end()) {
        return MqttStatus::kInvalidArgument;
    }
    const bool has_will = it->second.session.HasPendingWill();
    const MqttPublish will = it->second.session.will();
    m_sessions.erase(it);
    if (!graceful && has_will) {
        RoutePublish(will, outbound);
    }
    return MqttStatus::kOk;
}

std::size_t MqttBroker::CloseExpiredSessions(std::uint64_t now_ms,
                                             std::vector<MqttOutbound>* outbound) {
    std::size_t closed = 0;
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (!it->second.session.KeepaliveExpired(now_ms)) {
            ++it;
            continue;
        }
        const bool has_will = it->second.session.HasPendingWill();
        const MqttPublish will = it->second.session.will();
        it = m_sessions.erase(it);
        if (has_will) {
            RoutePublish(will, outbound);
        }
        ++closed;
    }
    return closed;
}

}  // namespace go2cpp::bento::net
