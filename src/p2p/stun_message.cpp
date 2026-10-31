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

constexpr char kSoftwareName[] = "go2cpp-p2p/1.0";
constexpr std::size_t kSoftwareSize = sizeof(kSoftwareName) - 1;
constexpr std::size_t kSoftwarePadded = (kSoftwareSize + 3U) & ~std::size_t{3};

/// XOR-MAPPED-ADDRESS 的掩码（指针版，避免每属性构造 array）。
std::array<std::uint8_t, 16> XorMaskFrom(const std::uint8_t* transaction_id) noexcept {
    std::array<std::uint8_t, 16> mask{};
    mask[0] = static_cast<std::uint8_t>((kStunMagicCookie >> 24U) & 0xFFU);
    mask[1] = static_cast<std::uint8_t>((kStunMagicCookie >> 16U) & 0xFFU);
    mask[2] = static_cast<std::uint8_t>((kStunMagicCookie >> 8U) & 0xFFU);
    mask[3] = static_cast<std::uint8_t>(kStunMagicCookie & 0xFFU);
    std::memcpy(mask.data() + 4, transaction_id, kStunTransactionIdSize);
    return mask;
}

void StoreStunHeader(std::uint8_t* out, std::uint16_t type, std::uint16_t body_length,
                     const std::uint8_t* transaction_id) noexcept {
    out[0] = static_cast<std::uint8_t>(type >> 8U);
    out[1] = static_cast<std::uint8_t>(type & 0xFFU);
    out[2] = static_cast<std::uint8_t>(body_length >> 8U);
    out[3] = static_cast<std::uint8_t>(body_length & 0xFFU);
    out[4] = static_cast<std::uint8_t>((kStunMagicCookie >> 24U) & 0xFFU);
    out[5] = static_cast<std::uint8_t>((kStunMagicCookie >> 16U) & 0xFFU);
    out[6] = static_cast<std::uint8_t>((kStunMagicCookie >> 8U) & 0xFFU);
    out[7] = static_cast<std::uint8_t>(kStunMagicCookie & 0xFFU);
    std::memcpy(out + 8, transaction_id, kStunTransactionIdSize);
}

std::size_t WriteAttribute(std::uint8_t* out, std::size_t capacity, std::size_t offset,
                           std::uint16_t type, const std::uint8_t* value,
                           std::size_t length) noexcept {
    if (value == nullptr && length > 0) {
        return 0;
    }
    if (length > kStunMaxAttributeValue) {
        return 0;
    }
    const std::size_t padded = (length + 3U) & ~std::size_t{3};
    if (offset > capacity || capacity - offset < 4 + padded) {
        return 0;
    }
    out[offset] = static_cast<std::uint8_t>(type >> 8U);
    out[offset + 1] = static_cast<std::uint8_t>(type & 0xFFU);
    out[offset + 2] = static_cast<std::uint8_t>(length >> 8U);
    out[offset + 3] = static_cast<std::uint8_t>(length & 0xFFU);
    if (length > 0) {
        std::memcpy(out + offset + 4, value, length);
    }
    if (padded > length) {
        std::memset(out + offset + 4 + length, 0, padded - length);
    }
    return offset + 4 + padded;
}

std::size_t WriteAddressAttribute(std::uint8_t* out, std::size_t capacity, std::size_t offset,
                                  std::uint16_t type, const Endpoint& endpoint, bool xored,
                                  const std::uint8_t* transaction_id) noexcept {
    if (out == nullptr || !endpoint.valid()) {
        return 0;
    }
    const bool ipv4 = endpoint.IsIPv4();
    if (!ipv4 && !endpoint.IsIPv6()) {
        return 0;
    }
    const std::size_t address_length = ipv4 ? 4 : 16;
    std::uint8_t value[20] = {};
    value[1] = ipv4 ? 0x01 : 0x02;
    const std::uint16_t port =
        static_cast<std::uint16_t>(endpoint.port() ^ (xored ? kStunXorPortMask : 0U));
    value[2] = static_cast<std::uint8_t>(port >> 8U);
    value[3] = static_cast<std::uint8_t>(port & 0xFFU);
    const auto address = endpoint.AddressBytes();
    const auto mask = XorMaskFrom(transaction_id);
    for (std::size_t index = 0; index < address_length; ++index) {
        value[4 + index] =
            static_cast<std::uint8_t>(address[index] ^ (xored ? mask[index] : 0U));
    }
    return WriteAttribute(out, capacity, offset, type, value, 4 + address_length);
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
    // 先算总长再一次性分配：旧实现先 push_back 拼 body（多次扩容 + 搬运），
    // 再 reserve + 拷贝到 output，最坏 3 次分配、3 次拷贝；现在恒为 1 + 1。
    std::size_t body_size = 0;
    for (const StunAttribute& attribute : message.attributes) {
        if (attribute.value.size() > kStunMaxAttributeValue) {
            if (error != nullptr) {
                *error = "STUN 属性值超过 65535 字节";
            }
            return {};
        }
        body_size += 4 + ((attribute.value.size() + 3U) & ~std::size_t{3});
    }
    if (body_size > kStunMaxMessageSize - kStunHeaderSize) {
        if (error != nullptr) {
            *error = "STUN 消息体超过 65515 字节";
        }
        return {};
    }
    std::vector<std::uint8_t> output(kStunHeaderSize + body_size);
    std::uint8_t* out = output.data();
    StoreStunHeader(out, EncodeStunType(message.method, message.klass),
                    static_cast<std::uint16_t>(body_size), message.transaction_id.data());
    std::size_t offset = kStunHeaderSize;
    for (const StunAttribute& attribute : message.attributes) {
        const std::size_t length = attribute.value.size();
        out[offset] = static_cast<std::uint8_t>(attribute.type >> 8U);
        out[offset + 1] = static_cast<std::uint8_t>(attribute.type & 0xFFU);
        out[offset + 2] = static_cast<std::uint8_t>(length >> 8U);
        out[offset + 3] = static_cast<std::uint8_t>(length & 0xFFU);
        offset += 4;
        if (length > 0) {
            std::memcpy(out + offset, attribute.value.data(), length);
        }
        const std::size_t padded = (length + 3U) & ~std::size_t{3};
        if (padded > length) {
            std::memset(out + offset + length, 0, padded - length);
        }
        offset += padded;
    }
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
    // 典型 STUN 响应 2~4 个属性：预留 4 个桶，省掉 vector 增长时的搬运。
    message.attributes.reserve(4);
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

std::size_t StunMessageView::attribute_count() const noexcept {
    std::size_t count = 0;
    std::size_t offset = kStunHeaderSize;
    while (offset + 4 <= m_length) {
        const std::uint16_t length = ReadU16(m_data + offset + 2);
        offset += 4 + ((static_cast<std::size_t>(length) + 3U) & ~std::size_t{3});
        if (offset > m_length) {
            return count;
        }
        ++count;
    }
    return count;
}

bool StunMessageView::attribute_at(std::size_t index, StunAttributeView* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    std::size_t offset = kStunHeaderSize;
    std::size_t current = 0;
    while (offset + 4 <= m_length) {
        const std::uint16_t type = ReadU16(m_data + offset);
        const std::uint16_t length = ReadU16(m_data + offset + 2);
        if (offset + 4 + length > m_length) {
            return false;
        }
        if (current == index) {
            out->type = type;
            out->value = m_data + offset + 4;
            out->length = length;
            return true;
        }
        offset += 4 + ((static_cast<std::size_t>(length) + 3U) & ~std::size_t{3});
        ++current;
    }
    return false;
}

bool StunMessageView::find(std::uint16_t type, StunAttributeView* out) const noexcept {
    if (out == nullptr) {
        return false;
    }
    std::size_t offset = kStunHeaderSize;
    while (offset + 4 <= m_length) {
        const std::uint16_t current_type = ReadU16(m_data + offset);
        const std::uint16_t length = ReadU16(m_data + offset + 2);
        if (offset + 4 + length > m_length) {
            return false;
        }
        if (current_type == type) {
            out->type = type;
            out->value = m_data + offset + 4;
            out->length = length;
            return true;
        }
        offset += 4 + ((static_cast<std::size_t>(length) + 3U) & ~std::size_t{3});
    }
    return false;
}

std::optional<std::uint32_t> StunMessageView::GetU32(std::uint16_t type) const noexcept {
    StunAttributeView attribute;
    if (!find(type, &attribute) || attribute.length != 4) {
        return std::nullopt;
    }
    return ReadU32(attribute.value);
}

std::optional<Endpoint> StunMessageView::GetAddress(std::uint16_t type, bool xored) const noexcept {
    StunAttributeView attribute;
    if (!find(type, &attribute) || attribute.length < 4) {
        return std::nullopt;
    }
    const std::uint8_t family = attribute.value[1];
    std::uint16_t port = ReadU16(attribute.value + 2);
    if (xored) {
        port = static_cast<std::uint16_t>(port ^ kStunXorPortMask);
    }
    if (family == 0x01) {
        if (attribute.length != 8) {
            return std::nullopt;
        }
        // IPv4 的 XOR 掩码就是 magic cookie 的 4 字节：不需要构造 16 字节
        // array（旧路径在每次解码时 memset 16B + memcpy 12B）。
        static constexpr std::uint8_t kCookieBytes[4] = {0x21, 0x12, 0xA4, 0x42};
        std::uint8_t address[4] = {};
        for (std::size_t index = 0; index < 4; ++index) {
            address[index] = static_cast<std::uint8_t>(
                attribute.value[4 + index] ^ (xored ? kCookieBytes[index] : 0U));
        }
        return Endpoint::FromAddressBytes(address, sizeof(address), port);
    }
    if (family == 0x02) {
        if (attribute.length != 20) {
            return std::nullopt;
        }
        const auto mask = XorMaskFrom(m_transaction_id);
        std::uint8_t address[16] = {};
        for (std::size_t index = 0; index < 16; ++index) {
            address[index] =
                static_cast<std::uint8_t>(attribute.value[4 + index] ^ (xored ? mask[index] : 0U));
        }
        return Endpoint::FromAddressBytes(address, sizeof(address), port);
    }
    return std::nullopt;
}

std::optional<Endpoint> StunMessageView::GetMappedAddress() const noexcept {
    const auto xored = GetAddress(kStunAttributeXorMappedAddress, true);
    if (xored.has_value()) {
        return xored;
    }
    return GetAddress(kStunAttributeMappedAddress, false);
}

bool StunMessageView::GetErrorCode(std::uint16_t* code, std::string* reason) const {
    StunAttributeView attribute;
    if (!find(kStunAttributeErrorCode, &attribute) || attribute.length < 4) {
        return false;
    }
    const std::uint8_t error_class = static_cast<std::uint8_t>(attribute.value[2] & 0x07U);
    const std::uint8_t error_number = attribute.value[3];
    if (code != nullptr) {
        *code = static_cast<std::uint16_t>(error_class * 100U + error_number);
    }
    if (reason != nullptr) {
        reason->assign(reinterpret_cast<const char*>(attribute.value) + 4,
                       static_cast<std::size_t>(attribute.length) - 4);
    }
    return true;
}

std::optional<StunMessageView> ParseStunMessageView(const void* data, std::size_t length,
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
            *error = "STUN 长度字段与实际不一致";
        }
        return std::nullopt;
    }
    if (ReadU32(bytes + 4) != kStunMagicCookie) {
        if (error != nullptr) {
            *error = "STUN magic cookie 不匹配";
        }
        return std::nullopt;
    }
    // 严格校验 TLV 结构与 4 字节对齐填充（保持与原解析器一致的拒绝语义）。
    std::size_t offset = kStunHeaderSize;
    while (offset < length) {
        if (offset + 4 > length) {
            if (error != nullptr) {
                *error = "STUN 属性头截断";
            }
            return std::nullopt;
        }
        const std::uint16_t attribute_length = ReadU16(bytes + offset + 2);
        offset += 4;
        if (attribute_length > length - offset) {
            if (error != nullptr) {
                *error = "STUN 属性值截断";
            }
            return std::nullopt;
        }
        offset += attribute_length;
        const std::size_t padding = (4U - (attribute_length % 4U)) % 4U;
        if (padding > length - offset) {
            if (error != nullptr) {
                *error = "STUN 属性填充截断";
            }
            return std::nullopt;
        }
        offset += padding;
    }
    StunMessageView view;
    view.m_data = bytes;
    view.m_length = length;
    view.m_method = DecodeStunMethod(type);
    view.m_klass = DecodeStunClass(type);
    view.m_transaction_id = bytes + 8;
    return view;
}

std::size_t BuildBindingRequestWire(
    std::uint8_t* out, std::size_t capacity,
    std::array<std::uint8_t, kStunTransactionIdSize>* transaction_id) {
    if (out == nullptr) {
        return 0;
    }
    std::array<std::uint8_t, kStunTransactionIdSize> local{};
    SecureRandomBytes(local.data(), local.size());
    if (transaction_id != nullptr) {
        *transaction_id = local;
    }
    const std::size_t total = kStunHeaderSize + 4 + kSoftwarePadded;
    if (capacity < total) {
        return 0;
    }
    StoreStunHeader(out, EncodeStunType(kStunMethodBinding, StunClass::kRequest),
                    static_cast<std::uint16_t>(total - kStunHeaderSize), local.data());
    std::size_t offset = WriteAttribute(out, capacity, kStunHeaderSize, kStunAttributeSoftware,
                                        reinterpret_cast<const std::uint8_t*>(kSoftwareName),
                                        kSoftwareSize);
    return offset == kStunHeaderSize ? 0 : offset;
}

std::size_t BuildBindingSuccessWire(
    std::uint8_t* out, std::size_t capacity,
    const std::array<std::uint8_t, kStunTransactionIdSize>& transaction_id, const Endpoint& mapped,
    const Endpoint* response_origin, const Endpoint* other_address, std::string_view software) {
    if (out == nullptr) {
        return 0;
    }
    StoreStunHeader(out, EncodeStunType(kStunMethodBinding, StunClass::kSuccessResponse), 0,
                    transaction_id.data());
    std::size_t offset = WriteAddressAttribute(out, capacity, kStunHeaderSize,
                                               kStunAttributeXorMappedAddress, mapped, true,
                                               transaction_id.data());
    if (offset == 0) {
        return 0;
    }
    offset = WriteAddressAttribute(out, capacity, offset, kStunAttributeMappedAddress, mapped, false,
                                   transaction_id.data());
    if (offset == 0) {
        return 0;
    }
    if (response_origin != nullptr) {
        offset = WriteAddressAttribute(out, capacity, offset, kStunAttributeResponseOrigin,
                                       *response_origin, false, transaction_id.data());
        if (offset == 0) {
            return 0;
        }
    }
    if (other_address != nullptr) {
        offset = WriteAddressAttribute(out, capacity, offset, kStunAttributeOtherAddress,
                                       *other_address, false, transaction_id.data());
        if (offset == 0) {
            return 0;
        }
    }
    if (!software.empty()) {
        if (software.size() > kStunMaxAttributeValue) {
            return 0;
        }
        offset = WriteAttribute(out, capacity, offset, kStunAttributeSoftware,
                                reinterpret_cast<const std::uint8_t*>(software.data()),
                                software.size());
        if (offset == 0) {
            return 0;
        }
    }
    const std::size_t body_length = offset - kStunHeaderSize;
    if (body_length > kStunMaxMessageSize - kStunHeaderSize) {
        return 0;
    }
    out[2] = static_cast<std::uint8_t>(body_length >> 8U);
    out[3] = static_cast<std::uint8_t>(body_length & 0xFFU);
    return offset;
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
