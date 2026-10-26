/**
 * @file protocol.cpp
 * @brief Bento 协议帧编解码实现。
 * @details 所有多字节字段按网络字节序（大端）写入/读取，不依赖主机字节序，
 *          也不依赖 htonl/ntohl，便于在测试里手工构造异常帧。
 * @author go2cpp contributors
 * @date 2026-10-26
 * @copyright MIT License
 */

#include "go2cpp/bento/net/protocol.hpp"

#include <chrono>

namespace go2cpp::bento {
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
    return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

/// 分片标志只允许出现在 STREAM 上，且载荷必须够放 8B 子头。
bool ValidateFragmentFlag(const FrameHeader& header, std::size_t payload_bytes) noexcept {
    if ((header.flags & kFlagFragmented) == 0) {
        return true;
    }
    return header.type == FrameType::kStream &&
           payload_bytes >= kStreamFragmentHeaderBytes;
}

/// FEC 校验片只允许出现在 VIDEO 上。
bool ValidateFecFlag(const FrameHeader& header) noexcept {
    if ((header.flags & kFlagFec) == 0) {
        return true;
    }
    return header.type == FrameType::kVideo;
}

/// 帧类型对应的固定/最小载荷约束。
bool ValidatePayloadShape(FrameType type, std::size_t payload_bytes) noexcept {
    switch (type) {
        case FrameType::kHello:
        case FrameType::kHelloAck:
            // 4B magic + 4B session + 4B conn_id。
            return payload_bytes >= 12;
        case FrameType::kStream:
        case FrameType::kDatagram:
            return true;
        case FrameType::kVideo:
            // 视频分片子头：frame_id(4) + frag_idx(2) + frag_cnt(2)。
            return payload_bytes >= kVideoHeaderBytes;
        case FrameType::kAck:
            // 纯 ACK 的载荷就是 8B ACK 段。
            return payload_bytes == kAckBytes;
        case FrameType::kBarrier:
            // 屏障序号放在 header.seq，无载荷。
            return payload_bytes == 0;
        case FrameType::kMaxData:
            // 绝对上限：8B 大端 u64。
            return payload_bytes == kMaxDataPayloadBytes;
        case FrameType::kPmtudProbe:
        case FrameType::kPmtudAck:
            // token + target；探针其余字节是填充。
            return payload_bytes >= kPmtudPayloadBytes;
    }
    return false;
}

}  // namespace

std::uint64_t NowMs() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool IsKnownFrameType(std::uint8_t raw) noexcept {
    switch (static_cast<FrameType>(raw)) {
        case FrameType::kHello:
        case FrameType::kHelloAck:
        case FrameType::kStream:
        case FrameType::kDatagram:
        case FrameType::kVideo:
        case FrameType::kAck:
        case FrameType::kBarrier:
        case FrameType::kMaxData:
        case FrameType::kPmtudProbe:
        case FrameType::kPmtudAck:
            return true;
    }
    return false;
}

const char* FrameTypeName(FrameType type) noexcept {
    switch (type) {
        case FrameType::kHello:
            return "HELLO";
        case FrameType::kHelloAck:
            return "HELLO_ACK";
        case FrameType::kStream:
            return "STREAM";
        case FrameType::kDatagram:
            return "DATAGRAM";
        case FrameType::kVideo:
            return "VIDEO";
        case FrameType::kAck:
            return "ACK";
        case FrameType::kBarrier:
            return "BARRIER";
        case FrameType::kMaxData:
            return "MAX_DATA";
        case FrameType::kPmtudProbe:
            return "PMTUD_PROBE";
        case FrameType::kPmtudAck:
            return "PMTUD_ACK";
    }
    return "UNKNOWN";
}

void EncodeHeader(const FrameHeader& header, std::uint8_t* out) noexcept {
    out[0] = header.version;
    out[1] = static_cast<std::uint8_t>(header.type);
    out[2] = header.channel;
    out[3] = header.flags;
    WriteU32(out + 4, header.conn_id);
    WriteU32(out + 8, header.seq);
    WriteU32(out + 12, header.dep_seq);
}

bool DecodeHeader(const std::uint8_t* data, std::size_t size, FrameHeader* out) noexcept {
    if (out == nullptr || data == nullptr || size < kFrameHeaderBytes) {
        return false;
    }
    if (data[0] != kProtocolVersion || !IsKnownFrameType(data[1])) {
        return false;
    }
    FrameHeader header;
    header.version = data[0];
    header.type = static_cast<FrameType>(data[1]);
    header.channel = data[2];
    header.flags = data[3];
    header.conn_id = ReadU32(data + 4);
    header.seq = ReadU32(data + 8);
    header.dep_seq = ReadU32(data + 12);
    *out = header;
    return true;
}

void EncodeAck(const AckState& ack, std::uint8_t* out) noexcept {
    WriteU32(out, ack.ack_seq);
    WriteU32(out + 4, ack.ack_bits);
}

bool DecodeAck(const std::uint8_t* data, std::size_t size, AckState* out) noexcept {
    if (out == nullptr || data == nullptr || size < kAckBytes) {
        return false;
    }
    out->ack_seq = ReadU32(data);
    out->ack_bits = ReadU32(data + 4);
    return true;
}

std::vector<std::uint8_t> BuildFrame(const FrameHeader& header, const AckState* ack,
                                     const std::uint8_t* payload, std::size_t payload_bytes) {
    // 探针是唯一允许超过 kMaxFramePayload 的帧类型：它的填充字节用于
    // 试探路径 MTU，仍然限制在 kMaxProbeFrameBytes 内。
    const std::size_t payload_limit = header.type == FrameType::kPmtudProbe
                                          ? kMaxProbeFrameBytes - kFrameHeaderBytes
                                          : kMaxFramePayload;
    if (payload_bytes > payload_limit) {
        return {};
    }
    if (payload_bytes > 0 && payload == nullptr) {
        return {};
    }
    if (!IsKnownFrameType(static_cast<std::uint8_t>(header.type))) {
        return {};
    }
    if (!ValidatePayloadShape(header.type, payload_bytes)) {
        return {};
    }
    if (!ValidateFragmentFlag(header, payload_bytes) || !ValidateFecFlag(header)) {
        return {};
    }
    if (header.version != kProtocolVersion) {
        return {};
    }
    // ACK/BARRIER 自身不捎带 ACK 段；PMTUD 探针也不捎带，保持尺寸可控。
    if (ack != nullptr &&
        (header.type == FrameType::kAck || header.type == FrameType::kBarrier ||
         header.type == FrameType::kPmtudProbe || header.type == FrameType::kPmtudAck)) {
        return {};
    }

    const bool with_ack = ack != nullptr;
    const std::size_t ack_bytes = with_ack ? kAckBytes : 0;
    std::vector<std::uint8_t> frame(kFrameHeaderBytes + ack_bytes + payload_bytes);

    FrameHeader wire_header = header;
    if (with_ack) {
        wire_header.flags = static_cast<std::uint8_t>(wire_header.flags | kFlagAck);
    } else {
        wire_header.flags =
            static_cast<std::uint8_t>(wire_header.flags & static_cast<std::uint8_t>(~kFlagAck));
    }
    EncodeHeader(wire_header, frame.data());
    std::size_t offset = kFrameHeaderBytes;
    if (with_ack) {
        EncodeAck(*ack, frame.data() + offset);
        offset += kAckBytes;
    }
    if (payload_bytes > 0) {
        for (std::size_t i = 0; i < payload_bytes; ++i) {
            frame[offset + i] = payload[i];
        }
    }
    return frame;
}

bool ParseFrame(const std::uint8_t* data, std::size_t size, FrameView* out) noexcept {
    if (out == nullptr || data == nullptr || size < kFrameHeaderBytes) {
        return false;
    }
    FrameView view;
    if (!DecodeHeader(data, size, &view.header)) {
        return false;
    }
    // 先拿到类型再决定尺寸上限：只有 PMTUD 探针允许 1500B。
    const std::size_t frame_limit =
        view.header.type == FrameType::kPmtudProbe ? kMaxProbeFrameBytes : kMaxFrameBytes;
    if (size > frame_limit) {
        return false;
    }

    std::size_t offset = kFrameHeaderBytes;
    if ((view.header.flags & kFlagAck) != 0) {
        if (view.header.type == FrameType::kAck || view.header.type == FrameType::kBarrier ||
            view.header.type == FrameType::kPmtudProbe ||
            view.header.type == FrameType::kPmtudAck) {
            // 这些帧没有「包头后 ACK 段」的合法形态。
            return false;
        }
        if (size < offset + kAckBytes) {
            return false;
        }
        if (!DecodeAck(data + offset, kAckBytes, &view.ack)) {
            return false;
        }
        view.has_piggyback_ack = true;
        offset += kAckBytes;
    }

    view.payload_bytes = size - offset;
    const std::size_t payload_limit = view.header.type == FrameType::kPmtudProbe
                                          ? kMaxProbeFrameBytes - offset
                                          : kMaxFramePayload;
    if (view.payload_bytes > payload_limit) {
        return false;
    }
    if (!ValidatePayloadShape(view.header.type, view.payload_bytes)) {
        return false;
    }
    if (!ValidateFragmentFlag(view.header, view.payload_bytes) || !ValidateFecFlag(view.header)) {
        return false;
    }
    view.payload = data + offset;

    if (view.header.type == FrameType::kAck) {
        // 纯 ACK 帧的 ACK 状态在载荷里；与捎带 ACK 不共用字段，避免歧义。
        AckState ack;
        if (!DecodeAck(view.payload, view.payload_bytes, &ack)) {
            return false;
        }
        view.ack = ack;
    }

    *out = view;
    return true;
}

void EncodeStreamFragmentHeader(const StreamFragmentHeader& header,
                                std::uint8_t* out) noexcept {
    WriteU32(out, header.frame_id);
    WriteU16(out + 4, header.frag_index);
    WriteU16(out + 6, header.frag_count);
}

bool DecodeStreamFragmentHeader(const std::uint8_t* data, std::size_t size,
                                StreamFragmentHeader* out) noexcept {
    if (out == nullptr || data == nullptr || size < kStreamFragmentHeaderBytes) {
        return false;
    }
    out->frame_id = ReadU32(data);
    out->frag_index = ReadU16(data + 4);
    out->frag_count = ReadU16(data + 6);
    return true;
}

void EncodeVideoHeader(const VideoFragmentHeader& header, std::uint8_t* out) noexcept {
    WriteU32(out, header.frame_id);
    WriteU16(out + 4, header.frag_index);
    WriteU16(out + 6, header.frag_count);
}

bool DecodeVideoHeader(const std::uint8_t* data, std::size_t size,
                       VideoFragmentHeader* out) noexcept {
    if (out == nullptr || data == nullptr || size < kVideoHeaderBytes) {
        return false;
    }
    out->frame_id = ReadU32(data);
    out->frag_index = ReadU16(data + 4);
    out->frag_count = ReadU16(data + 6);
    return true;
}

}  // namespace go2cpp::bento
