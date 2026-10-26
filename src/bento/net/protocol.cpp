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

/// 帧类型对应的固定/最小载荷约束。
bool ValidatePayloadShape(FrameType type, std::size_t payload_bytes) noexcept {
    switch (type) {
        case FrameType::kHello:
        case FrameType::kHelloAck:
            // 4B magic + 4B session：握手必须能区分随机探测与真实连接。
            return payload_bytes >= 8;
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
    }
    return "UNKNOWN";
}

void EncodeHeader(const FrameHeader& header, std::uint8_t* out) noexcept {
    out[0] = header.version;
    out[1] = static_cast<std::uint8_t>(header.type);
    out[2] = header.channel;
    out[3] = header.flags;
    WriteU32(out + 4, header.seq);
    WriteU32(out + 8, header.dep_seq);
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
    header.seq = ReadU32(data + 4);
    header.dep_seq = ReadU32(data + 8);
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
    if (payload_bytes > kMaxFramePayload) {
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
    if (header.version != kProtocolVersion) {
        return {};
    }
    // ACK/BARRIER 自身不捎带 ACK 段：ACK 帧的载荷已经是 ACK 状态，
    // BARRIER 只带 header.seq，避免出现两种来源的 ack 字段。
    if (ack != nullptr && (header.type == FrameType::kAck || header.type == FrameType::kBarrier)) {
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
    if (out == nullptr || data == nullptr || size > kMaxFrameBytes) {
        return false;
    }
    FrameView view;
    if (!DecodeHeader(data, size, &view.header)) {
        return false;
    }

    std::size_t offset = kFrameHeaderBytes;
    if ((view.header.flags & kFlagAck) != 0) {
        if (view.header.type == FrameType::kAck || view.header.type == FrameType::kBarrier) {
            // 这两类帧没有「包头后 ACK 段」的合法形态。
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
    if (view.payload_bytes > kMaxFramePayload) {
        return false;
    }
    if (!ValidatePayloadShape(view.header.type, view.payload_bytes)) {
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
