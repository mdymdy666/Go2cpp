/**
 * @file relay_protocol.cpp
 * @brief 简化 TURN 线格式编解码。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "relay_protocol.hpp"

#include <algorithm>
#include <cstring>

namespace go2cpp::p2p::detail {
namespace {

std::uint16_t ReadU16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) |
                                      static_cast<std::uint16_t>(data[1]));
}

std::uint32_t ReadU32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

void StoreU16(std::uint8_t* data, std::uint16_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 8U);
    data[1] = static_cast<std::uint8_t>(value & 0xFFU);
}

void StoreU32(std::uint8_t* data, std::uint32_t value) noexcept {
    data[0] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    data[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    data[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    data[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

}  // namespace

std::size_t BuildRelayHeader(std::uint8_t* out, std::size_t capacity, RelayMessageType type,
                             std::uint32_t request_id, std::size_t body_length) {
    if (out == nullptr || capacity < kRelayHeaderSize || body_length > kRelayMaxBody) {
        return 0;
    }
    StoreU32(out, kRelayMagic);
    out[4] = kRelayVersion;
    out[5] = static_cast<std::uint8_t>(type);
    StoreU16(out + 6, static_cast<std::uint16_t>(body_length));
    StoreU32(out + 8, request_id);
    StoreU32(out + 12, 0);
    return kRelayHeaderSize;
}

std::size_t BuildRelayMessage(std::uint8_t* out, std::size_t capacity, RelayMessageType type,
                              std::uint32_t request_id, const void* body,
                              std::size_t body_length) {
    if (body_length > 0 && body == nullptr) {
        return 0;
    }
    if (body_length > kRelayMaxBody) {
        return 0;
    }
    if (capacity < kRelayHeaderSize + body_length) {
        return 0;
    }
    if (BuildRelayHeader(out, capacity, type, request_id, body_length) == 0) {
        return 0;
    }
    if (body_length > 0) {
        std::memcpy(out + kRelayHeaderSize, body, body_length);
    }
    return kRelayHeaderSize + body_length;
}

std::vector<std::uint8_t> SerializeRelayMessage(const RelayMessage& message) {
    if (message.body.size() > kRelayMaxBody) {
        return {};
    }
    std::vector<std::uint8_t> wire(kRelayHeaderSize + message.body.size());
    const std::size_t written =
        BuildRelayMessage(wire.data(), wire.size(), message.type, message.request_id,
                          message.body.empty() ? nullptr : message.body.data(),
                          message.body.size());
    if (written == 0) {
        return {};
    }
    wire.resize(written);
    return wire;
}

bool ParseRelayMessageView(const void* data, std::size_t length, RelayMessageView* message,
                           std::string* error) {
    if (message == nullptr) {
        return false;
    }
    if (data == nullptr || length < kRelayHeaderSize) {
        if (error != nullptr) {
            *error = "中继消息短于 16 字节头";
        }
        return false;
    }
    if (length > kRelayHeaderSize + kRelayMaxBody) {
        if (error != nullptr) {
            *error = "中继消息体超长";
        }
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (ReadU32(bytes) != kRelayMagic) {
        if (error != nullptr) {
            *error = "中继消息 magic 不匹配";
        }
        return false;
    }
    if (bytes[4] != kRelayVersion) {
        if (error != nullptr) {
            *error = "不支持的简化 TURN 版本: " + std::to_string(bytes[4]);
        }
        return false;
    }
    const std::uint16_t body_length = ReadU16(bytes + 6);
    if (body_length != length - kRelayHeaderSize) {
        if (error != nullptr) {
            *error = "中继消息长度字段与实际不一致";
        }
        return false;
    }
    message->type = static_cast<RelayMessageType>(bytes[5]);
    message->request_id = ReadU32(bytes + 8);
    message->body = bytes + kRelayHeaderSize;
    message->body_length = body_length;
    return true;
}

bool ParseRelayMessage(const void* data, std::size_t length, RelayMessage* message,
                       std::string* error) {
    if (message == nullptr) {
        return false;
    }
    RelayMessageView view;
    if (!ParseRelayMessageView(data, length, &view, error)) {
        return false;
    }
    message->type = view.type;
    message->request_id = view.request_id;
    message->body.assign(view.body, view.body + view.body_length);
    return true;
}

std::size_t EncodeEndpointBody(std::uint8_t* out, std::size_t capacity,
                               const Endpoint& endpoint) {
    if (out == nullptr || !endpoint.valid()) {
        return 0;
    }
    const bool ipv4 = endpoint.IsIPv4();
    if (!ipv4 && !endpoint.IsIPv6()) {
        return 0;
    }
    const std::size_t address_length = ipv4 ? 4 : 16;
    if (capacity < 3 + address_length) {
        return 0;
    }
    out[0] = ipv4 ? 1 : 2;
    StoreU16(out + 1, endpoint.port());
    const auto address = endpoint.AddressBytes();
    std::memcpy(out + 3, address.data(), address_length);
    return 3 + address_length;
}

void AppendEndpointBody(std::vector<std::uint8_t>* body, const Endpoint& endpoint) {
    if (body == nullptr || !endpoint.valid()) {
        return;
    }
    std::uint8_t encoded[19] = {};
    const std::size_t written = EncodeEndpointBody(encoded, sizeof(encoded), endpoint);
    if (written == 0) {
        return;
    }
    body->insert(body->end(), encoded, encoded + written);
}

bool ReadEndpointBodyView(const std::uint8_t* body, std::size_t body_length, std::size_t* offset,
                          Endpoint* endpoint) {
    if (body == nullptr || offset == nullptr || endpoint == nullptr) {
        return false;
    }
    if (*offset + 3 > body_length) {
        return false;
    }
    const std::uint8_t family = body[*offset];
    const std::uint16_t port = ReadU16(body + *offset + 1);
    const std::size_t address_length = family == 1 ? 4 : (family == 2 ? 16 : 0);
    if (address_length == 0 || *offset + 3 + address_length > body_length) {
        return false;
    }
    *endpoint = Endpoint::FromAddressBytes(body + *offset + 3, address_length, port);
    *offset += 3 + address_length;
    return endpoint->valid();
}

bool ReadEndpointBody(const std::vector<std::uint8_t>& body, std::size_t* offset,
                      Endpoint* endpoint) {
    return ReadEndpointBodyView(body.data(), body.size(), offset, endpoint);
}

void AppendU32Body(std::vector<std::uint8_t>* body, std::uint32_t value) {
    if (body == nullptr) {
        return;
    }
    std::uint8_t encoded[4] = {};
    StoreU32(encoded, value);
    body->insert(body->end(), encoded, encoded + sizeof(encoded));
}

bool ReadU32BodyView(const std::uint8_t* body, std::size_t body_length, std::size_t* offset,
                     std::uint32_t* value) {
    if (body == nullptr || offset == nullptr || value == nullptr || *offset + 4 > body_length) {
        return false;
    }
    *value = ReadU32(body + *offset);
    *offset += 4;
    return true;
}

bool ReadU32Body(const std::vector<std::uint8_t>& body, std::size_t* offset,
                 std::uint32_t* value) {
    return ReadU32BodyView(body.data(), body.size(), offset, value);
}

}  // namespace go2cpp::p2p::detail
