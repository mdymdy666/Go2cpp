/**
 * @file bento_bridge.cpp
 * @brief App 帧 ↔ Bento 通用帧桥接实现。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/app/bento_bridge.hpp"

#include "go2cpp/bento/net/frame.hpp"

namespace go2cpp::app::bento_bridge {
namespace {

using go2cpp::bento::net::FrameDecoder;
using go2cpp::bento::net::FrameFormat;
using BentoFrame = go2cpp::bento::net::Frame;

/// Bento 通用帧的默认线上标识（"BENT"/1）；应用层帧靠 type 字段区分。
FrameFormat Format() { return go2cpp::bento::net::kDefaultFrameFormat; }

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

}  // namespace

const char* StatusName(Status status) noexcept {
    switch (status) {
        case Status::kOk:
            return "OK";
        case Status::kNeedMoreData:
            return "NEED_MORE_DATA";
        case Status::kBadFrame:
            return "BAD_FRAME";
        case Status::kBadFrameType:
            return "BAD_FRAME_TYPE";
        case Status::kBadPayload:
            return "BAD_PAYLOAD";
    }
    return "UNKNOWN";
}

std::vector<std::uint8_t> Encode(const Frame& frame, const Config& config) {
    // 单次分配 + 单次写：先按「20B Bento 头 + 9B App 头 + 载荷」算出总长，
    // 预分配后直接把 App 帧序列化到 20B 之后，再补 Bento 头与 CRC。
    // 旧实现先 EncodeFrame 分配一次、再 bento::Encode 分配并整体 memcpy 一次，
    // 对大帧等于多付一遍整帧内存带宽。
    const std::size_t app_bytes = kFrameHeaderBytes + frame.payload.size();
    if (app_bytes > go2cpp::bento::net::kFrameMaxPayload) {
        // 超过 Bento 单帧载荷上限：旧路径会在 bento::Encode 里返回空。
        return {};
    }
    std::vector<std::uint8_t> out;
    out.reserve(go2cpp::bento::net::kGenericFrameHeaderBytes + app_bytes);
    out.resize(go2cpp::bento::net::kGenericFrameHeaderBytes);
    // AppendEncodedFrame 只写 9B 头 + 载荷，且在容量足够时不会重新分配。
    if (!AppendEncodedFrame(frame, &out)) {
        return {};
    }
    std::uint8_t* header = out.data();
    WriteU32(header, Format().magic);
    WriteU16(header + 4, Format().version);
    WriteU16(header + 6, config.frame_type);
    WriteU16(header + 8, 0);   // flags
    WriteU16(header + 10, 0);  // reserved：当前版本必须为 0
    WriteU32(header + 12, static_cast<std::uint32_t>(app_bytes));
    // CRC 覆盖帧头前 16B + App 帧载荷，与 bento::net::Encode 完全一致。
    const std::uint32_t checksum = go2cpp::bento::net::ComputeFrameChecksum(
        header, out.data() + go2cpp::bento::net::kGenericFrameHeaderBytes, app_bytes);
    WriteU32(header + 16, checksum);
    return out;
}

Status Decode(const std::uint8_t* data, std::size_t size, Frame* out,
              std::size_t* consumed, const Config& config) {
    if (out == nullptr || (data == nullptr && size != 0)) {
        return Status::kBadPayload;
    }
    BentoFrame bento;
    std::size_t bento_consumed = 0;
    const auto status =
        go2cpp::bento::net::Decode(Format(), data, size, &bento, &bento_consumed);
    if (status == go2cpp::bento::net::FrameStatus::kNeedMoreData) {
        return Status::kNeedMoreData;
    }
    if (status != go2cpp::bento::net::FrameStatus::kOk) {
        return Status::kBadFrame;
    }
    if (bento.type != config.frame_type) {
        return Status::kBadFrameType;
    }
    std::size_t app_consumed = 0;
    const FrameStatus app_status = DecodeFrame(bento.payload.data(), bento.payload.size(),
                                               config.max_app_payload, out, &app_consumed);
    if (app_status != FrameStatus::kOk) {
        return Status::kBadPayload;
    }
    if (consumed != nullptr) {
        *consumed = bento_consumed;
    }
    return Status::kOk;
}

struct Decoder::Impl {
    explicit Impl(const Config& cfg)
        : config(cfg), decoder(go2cpp::bento::net::kFrameMaxPayload, Format()) {}

    Config config;         ///< 配置。
    FrameDecoder decoder;  ///< Bento 帧解码器。
};

Decoder::Decoder(const Config& config) : m_impl(std::make_unique<Impl>(config)) {}

Decoder::~Decoder() = default;

void Decoder::Append(const std::uint8_t* data, std::size_t size) {
    m_impl->decoder.Append(data, size);
}

Status Decoder::Next(Frame* out) {
    if (out == nullptr) {
        return Status::kBadPayload;
    }
    BentoFrame bento;
    const auto status = m_impl->decoder.Next(&bento);
    if (status == go2cpp::bento::net::FrameStatus::kNeedMoreData) {
        return Status::kNeedMoreData;
    }
    if (status != go2cpp::bento::net::FrameStatus::kOk) {
        return Status::kBadFrame;
    }
    if (bento.type != m_impl->config.frame_type) {
        return Status::kBadFrameType;
    }
    const FrameStatus app_status = DecodeFrame(bento.payload.data(), bento.payload.size(),
                                               m_impl->config.max_app_payload, out);
    if (app_status != FrameStatus::kOk) {
        return Status::kBadPayload;
    }
    return Status::kOk;
}

std::size_t Decoder::PendingBytes() const noexcept {
    return m_impl->decoder.PendingBytes();
}

void Decoder::Clear() noexcept { m_impl->decoder.Clear(); }

}  // namespace go2cpp::app::bento_bridge
