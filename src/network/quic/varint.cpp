/**
 * @file varint.cpp
 * @brief QUIC varint 实现（RFC 9000 §16）。
 */
#include "go2cpp/network/quic/varint.hpp"

namespace go2cpp::net::quic {

const char* ToString(VarIntStatus status) noexcept {
    switch (status) {
        case VarIntStatus::kOk: return "ok";
        case VarIntStatus::kNeedMore: return "need-more";
        case VarIntStatus::kTooLarge: return "too-large";
    }
    return "unknown";
}

std::size_t VarIntEncodedSize(std::uint64_t value) noexcept {
    if (value <= 63ULL) return 1;
    if (value <= 16383ULL) return 2;
    if (value <= 1073741823ULL) return 4;
    return 8;
}

VarIntEncoded EncodeVarInt(std::uint64_t value, std::uint8_t* out) noexcept {
    if (out == nullptr) {
        return VarIntEncoded{VarIntStatus::kTooLarge, 0};
    }
    if (value > kVarIntMax) {
        return VarIntEncoded{VarIntStatus::kTooLarge, 0};
    }
    switch (VarIntEncodedSize(value)) {
        case 1:
            out[0] = static_cast<std::uint8_t>(value);
            return VarIntEncoded{VarIntStatus::kOk, 1};
        case 2:
            out[0] = static_cast<std::uint8_t>(0x40U | static_cast<std::uint8_t>(value >> 8));
            out[1] = static_cast<std::uint8_t>(value & 0xFFU);
            return VarIntEncoded{VarIntStatus::kOk, 2};
        case 4:
            out[0] = static_cast<std::uint8_t>(0x80U | static_cast<std::uint8_t>(value >> 24));
            out[1] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
            out[2] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
            out[3] = static_cast<std::uint8_t>(value & 0xFFU);
            return VarIntEncoded{VarIntStatus::kOk, 4};
        default:
            out[0] = static_cast<std::uint8_t>(0xC0U | static_cast<std::uint8_t>(value >> 56));
            out[1] = static_cast<std::uint8_t>((value >> 48) & 0xFFU);
            out[2] = static_cast<std::uint8_t>((value >> 40) & 0xFFU);
            out[3] = static_cast<std::uint8_t>((value >> 32) & 0xFFU);
            out[4] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
            out[5] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
            out[6] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
            out[7] = static_cast<std::uint8_t>(value & 0xFFU);
            return VarIntEncoded{VarIntStatus::kOk, 8};
    }
}

VarIntDecoded DecodeVarInt(const std::uint8_t* in, std::size_t length) noexcept {
    if (in == nullptr || length == 0) {
        return VarIntDecoded{VarIntStatus::kNeedMore, 0, 0};
    }
    const std::uint8_t prefix = static_cast<std::uint8_t>(in[0] >> 6);
    const std::size_t size = (prefix == 0) ? 1 : (prefix == 1) ? 2 : (prefix == 2) ? 4 : 8;
    if (length < size) {
        // 关键：区分「数据还没到」与「数据非法」。增量解析时前者可以等。
        return VarIntDecoded{VarIntStatus::kNeedMore, 0, 0};
    }
    std::uint64_t value = static_cast<std::uint64_t>(in[0] & 0x3FU);
    for (std::size_t i = 1; i < size; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(in[i]);
    }
    return VarIntDecoded{VarIntStatus::kOk, value, size};
}

}  // namespace go2cpp::net::quic
