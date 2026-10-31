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

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value >> 8U));
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

}  // namespace

std::vector<std::uint8_t> SerializeRelayMessage(const RelayMessage& message) {
    if (message.body.size() > kRelayMaxBody) {
        return {};
    }
    std::vector<std::uint8_t> wire;
    wire.reserve(kRelayHeaderSize + message.body.size());
    AppendU32(&wire, kRelayMagic);
    wire.push_back(kRelayVersion);
    wire.push_back(static_cast<std::uint8_t>(message.type));
    AppendU16(&wire, static_cast<std::uint16_t>(message.body.size()));
    AppendU32(&wire, message.request_id);
    AppendU32(&wire, 0);
    wire.insert(wire.end(), message.body.begin(), message.body.end());
    return wire;
}

bool ParseRelayMessage(const void* data, std::size_t length, RelayMessage* message,
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
    message->body.assign(bytes + kRelayHeaderSize, bytes + length);
    return true;
}

void AppendEndpointBody(std::vector<std::uint8_t>* body, const Endpoint& endpoint) {
    if (body == nullptr || !endpoint.valid()) {
        return;
    }
    const auto address = endpoint.AddressBytes();
    const std::size_t address_length = endpoint.IsIPv4() ? 4 : 16;
    body->push_back(endpoint.IsIPv4() ? 1 : 2);
    AppendU16(body, endpoint.port());
    body->insert(body->end(), address.begin(), address.begin() + address_length);
}

bool ReadEndpointBody(const std::vector<std::uint8_t>& body, std::size_t* offset,
                      Endpoint* endpoint) {
    if (offset == nullptr || endpoint == nullptr) {
        return false;
    }
    if (*offset + 3 > body.size()) {
        return false;
    }
    const std::uint8_t family = body[*offset];
    const std::uint16_t port = ReadU16(body.data() + *offset + 1);
    const std::size_t address_length = family == 1 ? 4 : (family == 2 ? 16 : 0);
    if (address_length == 0 || *offset + 3 + address_length > body.size()) {
        return false;
    }
    *endpoint = Endpoint::FromAddressBytes(body.data() + *offset + 3, address_length, port);
    *offset += 3 + address_length;
    return endpoint->valid();
}

void AppendU32Body(std::vector<std::uint8_t>* body, std::uint32_t value) {
    if (body != nullptr) {
        AppendU32(body, value);
    }
}

bool ReadU32Body(const std::vector<std::uint8_t>& body, std::size_t* offset,
                 std::uint32_t* value) {
    if (offset == nullptr || value == nullptr || *offset + 4 > body.size()) {
        return false;
    }
    *value = ReadU32(body.data() + *offset);
    *offset += 4;
    return true;
}

}  // namespace go2cpp::p2p::detail
