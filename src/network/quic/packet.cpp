/**
 * @file packet.cpp
 * @brief QUIC 包头解析与构造实现（RFC 9000 §17）。
 */
#include "go2cpp/network/quic/packet.hpp"
#include "go2cpp/network/quic/varint.hpp"

#include <algorithm>

namespace go2cpp::net::quic {

namespace {

/// Long header 首字节：Header Form(1)=1, Fixed Bit(1)=1, Type(2), TypeSpecific(4)
constexpr std::uint8_t kLongHeaderMask = 0x80U;
constexpr std::uint8_t kFixedBit = 0x40U;
constexpr std::uint8_t kLongTypeMask = 0x30U;

std::uint32_t ReadUint32(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) |
           static_cast<std::uint32_t>(in[3]);
}

void AppendUint32(std::uint32_t value, std::vector<std::uint8_t>* out) {
    out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

/// 从 long header 的 type 位映射到 PacketType。
PacketType LongTypeFromBits(std::uint8_t first_byte) noexcept {
    switch ((first_byte & kLongTypeMask) >> 4) {
        case 0x0: return PacketType::kInitial;
        case 0x1: return PacketType::kZeroRtt;
        case 0x2: return PacketType::kHandshake;
        case 0x3: return PacketType::kRetry;
        default: return PacketType::kInitial;
    }
}

std::uint8_t LongTypeBits(PacketType type) noexcept {
    switch (type) {
        case PacketType::kInitial: return 0x00U;
        case PacketType::kZeroRtt: return 0x10U;
        case PacketType::kHandshake: return 0x20U;
        case PacketType::kRetry: return 0x30U;
        default: return 0x00U;
    }
}

}  // namespace

const char* ToString(PacketType type) noexcept {
    switch (type) {
        case PacketType::kInitial: return "Initial";
        case PacketType::kZeroRtt: return "0-RTT";
        case PacketType::kHandshake: return "Handshake";
        case PacketType::kRetry: return "Retry";
        case PacketType::kVersionNegotiation: return "Version-Negotiation";
        case PacketType::kOneRtt: return "1-RTT";
    }
    return "unknown";
}

const char* ToString(HeaderStatus status) noexcept {
    switch (status) {
        case HeaderStatus::kOk: return "ok";
        case HeaderStatus::kNeedMore: return "need-more";
        case HeaderStatus::kInvalid: return "invalid";
        case HeaderStatus::kUnsupportedVersion: return "unsupported-version";
    }
    return "unknown";
}

HeaderDecoded DecodePacketHeader(const std::uint8_t* in, std::size_t length,
                                 std::size_t short_dcid_length) noexcept {
    HeaderDecoded result;
    if (in == nullptr || length == 0) {
        return result;   // kNeedMore
    }

    const std::uint8_t first = in[0];
    const bool is_long = (first & kLongHeaderMask) != 0;

    // ---------------------------------------------------------------------
    // Version Negotiation / Long header
    // ---------------------------------------------------------------------
    if (is_long) {
        // long header 至少要有 1(首字节) + 4(version) + 1(dcid len) + 1(scid len)
        if (length < 7) {
            return result;
        }
        const std::uint32_t version = ReadUint32(in + 1);

        // Version Negotiation：version == 0，且**不受 fixed bit 约束**
        // （RFC 9000 §17.2.1：VN 包的 fixed bit 可能为 0）
        if (version == kVersionNegotiation) {
            result.header.is_long = true;
            result.header.type = PacketType::kVersionNegotiation;
            result.header.version = version;
            result.header.first_byte = first;
            std::size_t offset = 5;
            const std::size_t dcid_len = in[offset++];
            if (dcid_len > kMaxConnectionIdLength) {
                result.status = HeaderStatus::kInvalid;
                return result;
            }
            if (offset + dcid_len > length) return result;
            result.header.dcid.assign(in + offset, in + offset + dcid_len);
            offset += dcid_len;
            if (offset >= length) return result;
            const std::size_t scid_len = in[offset++];
            if (scid_len > kMaxConnectionIdLength) {
                result.status = HeaderStatus::kInvalid;
                return result;
            }
            if (offset + scid_len > length) return result;
            result.header.scid.assign(in + offset, in + offset + scid_len);
            offset += scid_len;
            // 剩余全是 32 位版本号列表
            const std::size_t remaining = length - offset;
            if (remaining % 4 != 0) {
                result.status = HeaderStatus::kInvalid;
                return result;
            }
            for (std::size_t i = 0; i + 4 <= remaining; i += 4) {
                result.header.supported_versions.push_back(ReadUint32(in + offset + i));
            }
            offset += remaining;
            result.header.packet_number_offset = offset;
            result.consumed = offset;
            result.status = HeaderStatus::kOk;
            return result;
        }

        // 非 VN 的 long header：fixed bit 必须为 1
        if ((first & kFixedBit) == 0) {
            result.status = HeaderStatus::kInvalid;
            return result;
        }

        const PacketType type = LongTypeFromBits(first);
        result.header.is_long = true;
        result.header.type = type;
        result.header.version = version;
        result.header.first_byte = first;

        std::size_t offset = 5;
        const std::size_t dcid_len = in[offset++];
        if (dcid_len > kMaxConnectionIdLength) {
            result.status = HeaderStatus::kInvalid;
            return result;
        }
        if (offset + dcid_len > length) return result;
        result.header.dcid.assign(in + offset, in + offset + dcid_len);
        offset += dcid_len;

        if (offset >= length) return result;
        const std::size_t scid_len = in[offset++];
        if (scid_len > kMaxConnectionIdLength) {
            result.status = HeaderStatus::kInvalid;
            return result;
        }
        if (offset + scid_len > length) return result;
        result.header.scid.assign(in + offset, in + offset + scid_len);
        offset += scid_len;

        if (type == PacketType::kRetry) {
            // Retry 没有 Length 字段：剩余部分 = Retry Token + 16 字节完整性标签。
            // Token 长度不是显式字段（由包总长推得）。
            if (length < offset + kRetryIntegrityTagLength) {
                return result;
            }
            const std::size_t token_len = length - offset - kRetryIntegrityTagLength;
            result.header.token.assign(in + offset, in + offset + token_len);
            offset += token_len;
            result.header.retry_integrity_tag.assign(in + offset, in + offset +
                                                                  kRetryIntegrityTagLength);
            offset += kRetryIntegrityTagLength;
            result.header.packet_number_offset = offset;
            result.consumed = offset;
            result.status = HeaderStatus::kOk;
            return result;
        }

        if (type == PacketType::kInitial) {
            // Token Length(varint) + Token + Length(varint)
            const auto token_len = DecodeVarInt(in + offset, length - offset);
            if (token_len.status == VarIntStatus::kNeedMore) return result;
            if (token_len.status != VarIntStatus::kOk) {
                result.status = HeaderStatus::kInvalid;
                return result;
            }
            offset += token_len.consumed;
            if (token_len.value > length - offset) return result;
            result.header.token.assign(in + offset,
                                       in + offset + static_cast<std::size_t>(token_len.value));
            offset += static_cast<std::size_t>(token_len.value);
        }

        // Length(varint)：= packet number + payload 的字节数
        const auto payload_len = DecodeVarInt(in + offset, length - offset);
        if (payload_len.status == VarIntStatus::kNeedMore) return result;
        if (payload_len.status != VarIntStatus::kOk) {
            result.status = HeaderStatus::kInvalid;
            return result;
        }
        result.header.payload_length = payload_len.value;
        offset += payload_len.consumed;

        result.header.packet_number_offset = offset;
        result.consumed = offset;
        result.status = HeaderStatus::kOk;
        return result;
    }

    // ---------------------------------------------------------------------
    // Short header（1-RTT）
    // ---------------------------------------------------------------------
    // RFC 9000 §17.3 没有禁止 0 长度的 DCID；短包头只是"不含长度字段"，
    // 长度由连接状态给出。所以 0 是合法值（实践中罕见，但协议允许），
    // 只拒绝超过 20 的上限。
    if (short_dcid_length > kMaxConnectionIdLength) {
        result.status = HeaderStatus::kInvalid;
        return result;
    }
    if ((first & kFixedBit) == 0) {
        result.status = HeaderStatus::kInvalid;
        return result;
    }
    if (length < 1 + short_dcid_length) {
        return result;
    }
    result.header.is_long = false;
    result.header.type = PacketType::kOneRtt;
    result.header.version = 0;   // short header 不含版本
    result.header.first_byte = first;
    result.header.dcid.assign(in + 1, in + 1 + short_dcid_length);
    result.header.packet_number_offset = 1 + short_dcid_length;
    result.consumed = 1 + short_dcid_length;
    result.status = HeaderStatus::kOk;
    return result;
}

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> EncodeLongHeader(const LongHeaderSpec& spec) {
    if (spec.dcid.size() > kMaxConnectionIdLength ||
        spec.scid.size() > kMaxConnectionIdLength) {
        return {};
    }
    if (spec.type == PacketType::kVersionNegotiation) {
        return EncodeVersionNegotiation(spec.dcid, spec.scid, {kVersion1});
    }
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(kLongHeaderMask | kFixedBit |
                                            LongTypeBits(spec.type)));
    AppendUint32(spec.version, &out);
    out.push_back(static_cast<std::uint8_t>(spec.dcid.size()));
    out.insert(out.end(), spec.dcid.begin(), spec.dcid.end());
    out.push_back(static_cast<std::uint8_t>(spec.scid.size()));
    out.insert(out.end(), spec.scid.begin(), spec.scid.end());

    if (spec.type == PacketType::kRetry) {
        out.insert(out.end(), spec.token.begin(), spec.token.end());
        if (spec.retry_integrity_tag.size() != kRetryIntegrityTagLength) {
            return {};
        }
        out.insert(out.end(), spec.retry_integrity_tag.begin(),
                   spec.retry_integrity_tag.end());
        return out;
    }
    if (spec.type == PacketType::kInitial) {
        std::uint8_t buffer[8] = {};
        const auto enc = EncodeVarInt(spec.token.size(), buffer);
        if (enc.status != VarIntStatus::kOk) {
            return {};
        }
        out.insert(out.end(), buffer, buffer + enc.size);
        out.insert(out.end(), spec.token.begin(), spec.token.end());
    }
    std::uint8_t buffer[8] = {};
    const auto enc = EncodeVarInt(spec.payload_length, buffer);
    if (enc.status != VarIntStatus::kOk) {
        return {};
    }
    out.insert(out.end(), buffer, buffer + enc.size);
    return out;
}

std::vector<std::uint8_t> EncodeVersionNegotiation(
    const std::vector<std::uint8_t>& dcid, const std::vector<std::uint8_t>& scid,
    const std::vector<std::uint32_t>& supported_versions) {
    if (dcid.size() > kMaxConnectionIdLength || scid.size() > kMaxConnectionIdLength) {
        return {};
    }
    std::vector<std::uint8_t> out;
    // VN 包的 fixed bit 可以随意；这里置 1 保持与多数实现一致。
    out.push_back(static_cast<std::uint8_t>(kLongHeaderMask | kFixedBit));
    AppendUint32(kVersionNegotiation, &out);
    out.push_back(static_cast<std::uint8_t>(dcid.size()));
    out.insert(out.end(), dcid.begin(), dcid.end());
    out.push_back(static_cast<std::uint8_t>(scid.size()));
    out.insert(out.end(), scid.begin(), scid.end());
    for (const auto version : supported_versions) {
        AppendUint32(version, &out);
    }
    return out;
}

std::vector<std::uint8_t> EncodeShortHeader(const std::vector<std::uint8_t>& dcid,
                                            std::uint64_t packet_number,
                                            std::uint8_t pn_length) {
    if (dcid.size() > kMaxConnectionIdLength || pn_length < 1 || pn_length > 4) {
        return {};
    }
    std::vector<std::uint8_t> out;
    // Header Form=0（short）、Fixed Bit=1、PN 长度低 2 位 = pn_length-1
    out.push_back(static_cast<std::uint8_t>(kFixedBit | ((pn_length - 1) & 0x3U)));
    out.insert(out.end(), dcid.begin(), dcid.end());
    for (std::uint8_t i = 0; i < pn_length; ++i) {
        out.push_back(static_cast<std::uint8_t>(
            (packet_number >> (8 * (pn_length - 1 - i))) & 0xFFU));
    }
    return out;
}

}  // namespace go2cpp::net::quic
