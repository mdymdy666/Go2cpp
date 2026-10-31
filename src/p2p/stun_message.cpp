/**
 * @file stun_message.cpp
 * @brief RFC 5389 STUN 消息编解码实现。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/stun_message.hpp"

#include <algorithm>
#include <cstring>

#include "go2cpp/p2p/random.hpp"

namespace go2cpp::p2p {
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

/// XOR-MAPPED-ADDRESS 的掩码：IPv4 = cookie；IPv6 = cookie + transaction id。
std::array<std::uint8_t, 16> XorMask(
    const std::array<std::uint8_t, kStunTransactionIdSize>& transaction_id) noexcept {
    std::array<std::uint8_t, 16> mask{};
    mask[0] = static_cast<std::uint8_t>((kStunMagicCookie >> 24U) & 0xFFU);
    mask[1] = static_cast<std::uint8_t>((kStunMagicCookie >> 16U) & 0xFFU);
    mask[2] = static_cast<std::uint8_t>((kStunMagicCookie >> 8U) & 0xFFU);
    mask[3] = static_cast<std::uint8_t>(kStunMagicCookie & 0xFFU);
    std::copy(transaction_id.begin(), transaction_id.end(), mask.begin() + 4);
    return mask;
}

}  // namespace

std::uint16_t EncodeStunType(std::uint16_t method, StunClass klass) noexcept {
    const std::uint16_t value = method & 0x0FFFU;
    const std::uint16_t class_bits = static_cast<std::uint16_t>(klass) & 0x03U;
    return static_cast<std::uint16_t>((value & 0x000FU) | ((value & 0x0070U) << 1U) |
                                      ((value & 0x0F80U) << 2U) | ((class_bits & 0x02U) << 7U) |
                                      ((class_bits & 0x01U) << 4U));
}

std::uint16_t DecodeStunMethod(std::uint16_t type) noexcept {
    return static_cast<std::uint16_t>((type & 0x000FU) | ((type & 0x00E0U) >> 1U) |
                                      ((type & 0x3E00U) >> 2U));
}

StunClass DecodeStunClass(std::uint16_t type) noexcept {
    return static_cast<StunClass>(((type >> 7U) & 0x02U) | ((type >> 4U) & 0x01U));
}

StunMessage StunMessage::MakeBindingRequest() {
    StunMessage message;
    message.method = kStunMethodBinding;
    message.klass = StunClass::kRequest;
    SecureRandomBytes(message.transaction_id.data(), message.transaction_id.size());
    message.AddString(kStunAttributeSoftware, "go2cpp-p2p/1.0");
    return message;
}

StunMessage StunMessage::MakeBindingSuccess(
    const std::array<std::uint8_t, kStunTransactionIdSize>& transaction_id, const Endpoint& mapped) {
    StunMessage message;
    message.method = kStunMethodBinding;
    message.klass = StunClass::kSuccessResponse;
    message.transaction_id = transaction_id;
    message.AddAddress(kStunAttributeXorMappedAddress, mapped, true);
    message.AddAddress(kStunAttributeMappedAddress, mapped, false);
    return message;
}

StunMessage StunMessage::MakeBindingError(
    const std::array<std::uint8_t, kStunTransactionIdSize>& transaction_id, std::uint16_t code,
    std::string_view reason) {
    StunMessage message;
    message.method = kStunMethodBinding;
    message.klass = StunClass::kErrorResponse;
    message.transaction_id = transaction_id;
    // ERROR-CODE 的 value 布局：2 字节保留 + 1 字节 class（3 位）+ 1 字节 number + 原因短语。
    const std::uint8_t error_class = static_cast<std::uint8_t>((code / 100U) & 0x07U);
    const std::uint8_t error_number = static_cast<std::uint8_t>(code % 100U);
    std::vector<std::uint8_t> value = {0, 0, error_class, error_number};
    value.insert(value.end(), reason.begin(), reason.end());
    message.AddAttribute(kStunAttributeErrorCode, value.data(), value.size());
    return message;
}

const StunAttribute* StunMessage::Find(std::uint16_t type) const noexcept {
    for (const StunAttribute& attribute : attributes) {
        if (attribute.type == type) {
            return &attribute;
        }
    }
    return nullptr;
}

bool StunMessage::AddAttribute(std::uint16_t type, const void* data, std::size_t length) {
    if (length > kStunMaxAttributeValue) {
        return false;
    }
    if (length > 0 && data == nullptr) {
        return false;
    }
    StunAttribute attribute;
    attribute.type = type;
    if (length > 0) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        attribute.value.assign(bytes, bytes + length);
    }
    attributes.push_back(std::move(attribute));
    return true;
}

bool StunMessage::AddU32(std::uint16_t type, std::uint32_t value) {
    std::uint8_t bytes[4] = {static_cast<std::uint8_t>((value >> 24U) & 0xFFU),
                             static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
                             static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
                             static_cast<std::uint8_t>(value & 0xFFU)};
    return AddAttribute(type, bytes, sizeof(bytes));
}

bool StunMessage::AddString(std::uint16_t type, std::string_view text) {
    return AddAttribute(type, text.data(), text.size());
}

bool StunMessage::AddAddress(std::uint16_t type, const Endpoint& endpoint, bool xored) {
    if (!endpoint.valid()) {
        return false;
    }
    const bool ipv4 = endpoint.IsIPv4();
    if (!ipv4 && !endpoint.IsIPv6()) {
        return false;
    }
    const std::size_t address_length = ipv4 ? 4 : 16;
    std::uint8_t value[20] = {};
    value[1] = ipv4 ? 0x01 : 0x02;
    const std::uint16_t port =
        static_cast<std::uint16_t>(endpoint.port() ^ (xored ? kStunXorPortMask : 0U));
    value[2] = static_cast<std::uint8_t>(port >> 8U);
    value[3] = static_cast<std::uint8_t>(port & 0xFFU);
    const auto address = endpoint.AddressBytes();
    const auto mask = XorMask(transaction_id);
    for (std::size_t index = 0; index < address_length; ++index) {
        value[4 + index] =
            static_cast<std::uint8_t>(address[index] ^ (xored ? mask[index] : 0U));
    }
    return AddAttribute(type, value, 4 + address_length);
}

std::optional<std::uint32_t> StunMessage::GetU32(std::uint16_t type) const {
    const StunAttribute* attribute = Find(type);
    if (attribute == nullptr || attribute->value.size() != 4) {
        return std::nullopt;
    }
    return ReadU32(attribute->value.data());
}

std::optional<Endpoint> StunMessage::GetAddress(std::uint16_t type, bool xored) const {
    const StunAttribute* attribute = Find(type);
    if (attribute == nullptr || attribute->value.size() < 4) {
        return std::nullopt;
    }
    const auto& value = attribute->value;
    const std::uint8_t family = value[1];
    std::uint16_t port = ReadU16(&value[2]);
    if (xored) {
        port = static_cast<std::uint16_t>(port ^ kStunXorPortMask);
    }
    const auto mask = XorMask(transaction_id);
    if (family == 0x01) {
        if (value.size() != 8) {
            return std::nullopt;
        }
        std::uint8_t address[4] = {};
        for (std::size_t index = 0; index < 4; ++index) {
            address[index] =
                static_cast<std::uint8_t>(value[4 + index] ^ (xored ? mask[index] : 0U));
        }
        return Endpoint::FromAddressBytes(address, sizeof(address), port);
    }
    if (family == 0x02) {
        if (value.size() != 20) {
            return std::nullopt;
        }
        std::uint8_t address[16] = {};
        for (std::size_t index = 0; index < 16; ++index) {
            address[index] =
                static_cast<std::uint8_t>(value[4 + index] ^ (xored ? mask[index] : 0U));
        }
        return Endpoint::FromAddressBytes(address, sizeof(address), port);
    }
    return std::nullopt;
}

std::optional<Endpoint> StunMessage::GetMappedAddress() const {
    const auto xored = GetAddress(kStunAttributeXorMappedAddress, true);
    if (xored.has_value()) {
        return xored;
    }
    return GetAddress(kStunAttributeMappedAddress, false);
}

bool StunMessage::GetErrorCode(std::uint16_t* code, std::string* reason) const {
    const StunAttribute* attribute = Find(kStunAttributeErrorCode);
    if (attribute == nullptr || attribute->value.size() < 4) {
        return false;
    }
    const std::uint8_t error_class =
        static_cast<std::uint8_t>(attribute->value[2] & 0x07U);
    const std::uint8_t error_number = attribute->value[3];
    if (code != nullptr) {
        *code = static_cast<std::uint16_t>(error_class * 100U + error_number);
    }
    if (reason != nullptr) {
        reason->assign(attribute->value.begin() + 4, attribute->value.end());
    }
    return true;
}

std::vector<std::uint8_t> SerializeStunMessage(const StunMessage& message, std::string* error) {
    std::vector<std::uint8_t> body;
    for (const StunAttribute& attribute : message.attributes) {
        if (attribute.value.size() > kStunMaxAttributeValue) {
            if (error != nullptr) {
                *error = "STUN 属性值超过 65535 字节";
            }
            return {};
        }
        AppendU16(&body, attribute.type);
        AppendU16(&body, static_cast<std::uint16_t>(attribute.value.size()));
        body.insert(body.end(), attribute.value.begin(), attribute.value.end());
        const std::size_t padding = (4U - (attribute.value.size() % 4U)) % 4U;
        body.insert(body.end(), padding, 0);
    }
    if (body.size() > kStunMaxMessageSize - kStunHeaderSize) {
        if (error != nullptr) {
            *error = "STUN 消息体超过 65515 字节";
        }
        return {};
    }
    std::vector<std::uint8_t> output;
    output.reserve(kStunHeaderSize + body.size());
    AppendU16(&output, EncodeStunType(message.method, message.klass));
    AppendU16(&output, static_cast<std::uint16_t>(body.size()));
    AppendU32(&output, kStunMagicCookie);
    output.insert(output.end(), message.transaction_id.begin(), message.transaction_id.end());
    output.insert(output.end(), body.begin(), body.end());
    return output;
}

std::optional<StunMessage> ParseStunMessage(const void* data, std::size_t length,
                                            std::string* error) {
    if (data == nullptr || length < kStunHeaderSize) {
        if (error != nullptr) {
            *error = "STUN 消息短于 20 字节头部";
        }
        return std::nullopt;
    }
    if (length > kStunMaxMessageSize) {
        if (error != nullptr) {
            *error = "STUN 消息超过 65535 字节";
        }
        return std::nullopt;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const std::uint16_t type = ReadU16(bytes);
    const std::uint16_t body_length = ReadU16(bytes + 2);
    if (body_length != length - kStunHeaderSize) {
        if (error != nullptr) {
            *error = "STUN 长度字段(" + std::to_string(body_length) + ")与实际(" +
                     std::to_string(length - kStunHeaderSize) + ")不一致";
        }
        return std::nullopt;
    }
    if (ReadU32(bytes + 4) != kStunMagicCookie) {
        if (error != nullptr) {
            *error = "STUN magic cookie 不匹配";
        }
        return std::nullopt;
    }
    StunMessage message;
    message.method = DecodeStunMethod(type);
    message.klass = DecodeStunClass(type);
    std::copy(bytes + 8, bytes + 8 + kStunTransactionIdSize, message.transaction_id.begin());
    std::size_t offset = kStunHeaderSize;
    while (offset < length) {
        if (offset + 4 > length) {
            if (error != nullptr) {
                *error = "STUN 属性头截断";
            }
            return std::nullopt;
        }
        const std::uint16_t attribute_type = ReadU16(bytes + offset);
        const std::uint16_t attribute_length = ReadU16(bytes + offset + 2);
        offset += 4;
        if (offset + attribute_length > length) {
            if (error != nullptr) {
                *error = "STUN 属性值截断";
            }
            return std::nullopt;
        }
        StunAttribute attribute;
        attribute.type = attribute_type;
        attribute.value.assign(bytes + offset, bytes + offset + attribute_length);
        offset += attribute_length;
        const std::size_t padding = (4U - (attribute_length % 4U)) % 4U;
        if (offset + padding > length) {
            if (error != nullptr) {
                *error = "STUN 属性填充截断";
            }
            return std::nullopt;
        }
        offset += padding;
        message.attributes.push_back(std::move(attribute));
    }
    return message;
}

std::string StunClassName(StunClass klass) {
    switch (klass) {
        case StunClass::kRequest:
            return "request";
        case StunClass::kIndication:
            return "indication";
        case StunClass::kSuccessResponse:
            return "success";
        case StunClass::kErrorResponse:
            return "error";
    }
    return "unknown";
}

}  // namespace go2cpp::p2p
