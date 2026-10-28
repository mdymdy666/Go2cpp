/**
 * @file frame.cpp
 * @brief 通用帧编解码实现（MOD-10）。
 * @details 所有多字节字段按网络字节序（大端）写入/读取，不依赖主机字节序，
 *          也不依赖 htonl/ntohl，便于测试手工构造异常帧。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/frame.hpp"

#include <cstring>

#include "go2cpp/hash/crc32.hpp"

namespace go2cpp::bento::net {
namespace {

void WriteU16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}

void WriteU32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint16_t ReadU16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                      static_cast<std::uint16_t>(in[1]));
}

std::uint32_t ReadU32(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

/// @brief 从缓冲区开头解析一帧；不修改输入，不做任何缓冲管理。
/// @note 校验顺序固定为 magic → version → reserved → length → checksum，
///       与 FrameDecoder 的拒绝策略一致，避免两条路径给出不同错误码。
FrameStatus ParseOne(const FrameFormat& format, const std::uint8_t* data, std::size_t size,
                     std::uint32_t max_payload, Frame* out, std::size_t* consumed) noexcept {
    if (size < kGenericFrameHeaderBytes) {
        return FrameStatus::kNeedMoreData;
    }
    const std::uint32_t magic = ReadU32(data);
    if (magic != format.magic) {
        return FrameStatus::kBadMagic;
    }
    const std::uint16_t version = ReadU16(data + 4);
    if (version != format.version) {
        return FrameStatus::kBadVersion;
    }
    const std::uint16_t reserved = ReadU16(data + 10);
    if (reserved != 0) {
        return FrameStatus::kBadReserved;
    }
    const std::uint32_t length = ReadU32(data + 12);
    if (length > max_payload) {
        return FrameStatus::kLengthExceeded;
    }
    const std::size_t total = kGenericFrameHeaderBytes + static_cast<std::size_t>(length);
    if (size < total) {
        return FrameStatus::kNeedMoreData;
    }
    const std::uint32_t checksum = ReadU32(data + 16);
    if (checksum != ComputeFrameChecksum(data, data + kGenericFrameHeaderBytes, length)) {
        return FrameStatus::kChecksumMismatch;
    }
    out->magic = magic;
    out->version = version;
    out->type = ReadU16(data + 6);
    out->flags = ReadU16(data + 8);
    out->reserved = reserved;
    out->checksum = checksum;
    out->payload.assign(data + kGenericFrameHeaderBytes, data + total);
    if (consumed != nullptr) {
        *consumed = total;
    }
    return FrameStatus::kOk;
}

}  // namespace

const char* FrameStatusName(FrameStatus status) noexcept {
    switch (status) {
        case FrameStatus::kOk:
            return "ok";
        case FrameStatus::kNeedMoreData:
            return "need_more_data";
        case FrameStatus::kBadMagic:
            return "bad_magic";
        case FrameStatus::kBadVersion:
            return "bad_version";
        case FrameStatus::kBadReserved:
            return "bad_reserved";
        case FrameStatus::kLengthExceeded:
            return "length_exceeded";
        case FrameStatus::kChecksumMismatch:
            return "checksum_mismatch";
        case FrameStatus::kInvalidArgument:
            return "invalid_argument";
    }
    return "unknown";
}

std::uint32_t ComputeFrameChecksum(const std::uint8_t* header_prefix,
                                   const std::uint8_t* payload,
                                   std::size_t payload_bytes) noexcept {
    // 校验和本身不在覆盖范围内，所以这里只需要头的前 16 字节。
    const std::uint32_t crc = hash::Crc32::ChecksumIEEE(
        hash::AsBytes(header_prefix, kGenericFrameHeaderBytes - sizeof(std::uint32_t)));
    if (payload_bytes == 0) {
        return crc;
    }
    return hash::Crc32::Update(crc, hash::IEEETable,
                               hash::AsBytes(payload, payload_bytes));
}

std::vector<std::uint8_t> Encode(const Frame& frame) {
    return Encode(frame, kDefaultFrameFormat);
}

std::vector<std::uint8_t> Encode(const Frame& frame, const FrameFormat& format) {
    // reserved 是给未来扩展留的零值字段：当前版本出现非零值就不能保证
    // 对方能正确解析，宁可在发送端拒绝，也不要把隐患放到线上。
    if (frame.reserved != 0) {
        return {};
    }
    if (format.magic == 0) {
        // magic == 0 无法与「全零缓冲区」区分，接收端也没有任何协议标识可用。
        return {};
    }
    if (frame.payload.size() > kFrameMaxPayload) {
        return {};
    }
    const std::size_t total = kGenericFrameHeaderBytes + frame.payload.size();
    std::vector<std::uint8_t> out(total);
    WriteU32(out.data(), format.magic);
    WriteU16(out.data() + 4, format.version);
    WriteU16(out.data() + 6, frame.type);
    WriteU16(out.data() + 8, frame.flags);
    WriteU16(out.data() + 10, 0);
    WriteU32(out.data() + 12, static_cast<std::uint32_t>(frame.payload.size()));
    // 必须先把 payload 写入 out，再计算 checksum：ComputeFrameChecksum 读取的是
    // 输出缓冲中的 payload 区域；顺序反了会把全零当成 payload 计算。
    if (!frame.payload.empty()) {
        std::memcpy(out.data() + kGenericFrameHeaderBytes, frame.payload.data(),
                    frame.payload.size());
    }
    const std::uint32_t checksum = ComputeFrameChecksum(
        out.data(), out.data() + kGenericFrameHeaderBytes, frame.payload.size());
    WriteU32(out.data() + 16, checksum);
    return out;
}

FrameStatus Decode(const std::uint8_t* data, std::size_t size, Frame* out,
                   std::size_t* consumed) noexcept {
    if (out == nullptr) {
        return FrameStatus::kInvalidArgument;
    }
    if (data == nullptr) {
        return size == 0 ? FrameStatus::kNeedMoreData : FrameStatus::kInvalidArgument;
    }
    return ParseOne(kDefaultFrameFormat, data, size, kFrameMaxPayload, out, consumed);
}

FrameStatus Decode(const FrameFormat& format, const std::uint8_t* data, std::size_t size,
                   Frame* out, std::size_t* consumed) noexcept {
    if (out == nullptr) {
        return FrameStatus::kInvalidArgument;
    }
    if (format.magic == 0) {
        return FrameStatus::kInvalidArgument;
    }
    if (data == nullptr) {
        return size == 0 ? FrameStatus::kNeedMoreData : FrameStatus::kInvalidArgument;
    }
    return ParseOne(format, data, size, kFrameMaxPayload, out, consumed);
}

FrameDecoder::FrameDecoder(std::uint32_t max_payload, FrameFormat format) noexcept
    : m_max_payload(max_payload), m_format(format) {}

void FrameDecoder::Append(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    if (m_offset > 0) {
        // 只在有新数据时压缩一次，均摊 O(1)：解析阶段只推进 m_offset，
        // 不逐帧 erase，避免粘包场景下反复搬移。
        m_buffer.erase(m_buffer.begin(),
                       m_buffer.begin() + static_cast<std::ptrdiff_t>(m_offset));
        m_offset = 0;
    }
    m_buffer.insert(m_buffer.end(), data, data + size);
}

FrameStatus FrameDecoder::Next(Frame* out) {
    if (out == nullptr) {
        return FrameStatus::kInvalidArgument;
    }
    const std::size_t available = m_buffer.size() - m_offset;
    if (available == 0) {
        return FrameStatus::kNeedMoreData;
    }
    const FrameStatus status = ParseOne(m_format, m_buffer.data() + m_offset, available,
                                        m_max_payload, out, nullptr);
    if (status == FrameStatus::kOk) {
        m_offset += kGenericFrameHeaderBytes + out->payload.size();
        if (m_offset == m_buffer.size()) {
            m_buffer.clear();
            m_offset = 0;
        }
        return status;
    }
    if (status != FrameStatus::kNeedMoreData) {
        // 帧边界已不可信：清空缓冲，由调用方关闭/重建连接（fail-fast）。
        Clear();
    }
    return status;
}

void FrameDecoder::Clear() noexcept {
    m_buffer.clear();
    m_offset = 0;
}

}  // namespace go2cpp::bento::net
