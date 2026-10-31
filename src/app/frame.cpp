/**
 * @file frame.cpp
 * @brief 应用层帧编解码实现（9B 头 + varint 工具 + SETTINGS/GOAWAY/... 载荷）。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/app/frame.hpp"

#include <algorithm>
#include <cstring>

namespace go2cpp::app {
namespace {

/// @brief 帧类型是否已知。
bool KnownType(FrameType type) noexcept {
    switch (type) {
        case FrameType::kData:
        case FrameType::kHeaders:
        case FrameType::kPriority:
        case FrameType::kRstStream:
        case FrameType::kSettings:
        case FrameType::kPing:
        case FrameType::kGoAway:
        case FrameType::kWindowUpdate:
        case FrameType::kResourceIndex:
            return true;
    }
    return false;
}

}  // namespace

const char* FrameTypeName(FrameType type) noexcept {
    switch (type) {
        case FrameType::kData:
            return "DATA";
        case FrameType::kHeaders:
            return "HEADERS";
        case FrameType::kPriority:
            return "PRIORITY";
        case FrameType::kRstStream:
            return "RST_STREAM";
        case FrameType::kSettings:
            return "SETTINGS";
        case FrameType::kPing:
            return "PING";
        case FrameType::kGoAway:
            return "GOAWAY";
        case FrameType::kWindowUpdate:
            return "WINDOW_UPDATE";
        case FrameType::kResourceIndex:
            return "RESOURCE_INDEX";
    }
    return "UNKNOWN";
}

const char* ErrorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kNoError:
            return "NO_ERROR";
        case ErrorCode::kProtocolError:
            return "PROTOCOL_ERROR";
        case ErrorCode::kInternalError:
            return "INTERNAL_ERROR";
        case ErrorCode::kFlowControlError:
            return "FLOW_CONTROL_ERROR";
        case ErrorCode::kStreamClosed:
            return "STREAM_CLOSED";
        case ErrorCode::kFrameSizeError:
            return "FRAME_SIZE_ERROR";
        case ErrorCode::kRefusedStream:
            return "REFUSED_STREAM";
        case ErrorCode::kCancel:
            return "CANCEL";
        case ErrorCode::kChecksumError:
            return "CHECKSUM_ERROR";
        case ErrorCode::kResourceNotFound:
            return "RESOURCE_NOT_FOUND";
        case ErrorCode::kResumeOutOfRange:
            return "RESUME_OUT_OF_RANGE";
        case ErrorCode::kInvalidChunk:
            return "INVALID_CHUNK";
    }
    return "UNKNOWN";
}

const char* FrameStatusName(FrameStatus status) noexcept {
    switch (status) {
        case FrameStatus::kOk:
            return "OK";
        case FrameStatus::kNeedMoreData:
            return "NEED_MORE_DATA";
        case FrameStatus::kLengthExceeded:
            return "LENGTH_EXCEEDED";
        case FrameStatus::kInvalidType:
            return "INVALID_TYPE";
        case FrameStatus::kInvalidStreamId:
            return "INVALID_STREAM_ID";
        case FrameStatus::kInvalidArgument:
            return "INVALID_ARGUMENT";
    }
    return "UNKNOWN";
}

FrameStatus DecodeFrame(const std::uint8_t* data, std::size_t size, Frame* out,
                        std::size_t* consumed) noexcept {
    return DecodeFrame(data, size, kDefaultMaxFramePayload, out, consumed);
}

FrameStatus DecodeFrame(const std::uint8_t* data, std::size_t size,
                        std::uint32_t max_payload, Frame* out,
                        std::size_t* consumed) noexcept {
    if (out == nullptr || (data == nullptr && size != 0)) {
        return FrameStatus::kInvalidArgument;
    }
    out->stream_id = 0;
    out->type = FrameType::kData;
    out->flags = 0;
    out->payload.clear();
    if (size < kFrameHeaderBytes) {
        return FrameStatus::kNeedMoreData;
    }
    const std::uint32_t length = (static_cast<std::uint32_t>(data[0]) << 16) |
                                 (static_cast<std::uint32_t>(data[1]) << 8) |
                                 static_cast<std::uint32_t>(data[2]);
    const FrameType type = static_cast<FrameType>(data[3]);
    if (!KnownType(type)) {
        return FrameStatus::kInvalidType;
    }
    const std::uint8_t flags = data[4];
    const std::uint32_t stream_field = GetU32(data + 5);
    if ((stream_field & 0x80000000u) != 0) {
        return FrameStatus::kInvalidStreamId;
    }
    if (length > max_payload || length > kMaxFramePayloadLimit) {
        return FrameStatus::kLengthExceeded;
    }
    const std::size_t total = kFrameHeaderBytes + static_cast<std::size_t>(length);
    if (size < total) {
        return FrameStatus::kNeedMoreData;
    }
    out->stream_id = stream_field & kMaxStreamId;
    out->type = type;
    out->flags = flags;
    out->payload.assign(data + kFrameHeaderBytes, data + total);
    if (consumed != nullptr) {
        *consumed = total;
    }
    return FrameStatus::kOk;
}

std::vector<std::uint8_t> EncodeFrame(const Frame& frame) {
    std::vector<std::uint8_t> out;
    out.reserve(kFrameHeaderBytes + frame.payload.size());
    if (!AppendEncodedFrame(frame, &out)) {
        return {};
    }
    return out;
}

bool AppendEncodedFrame(const Frame& frame, std::vector<std::uint8_t>* out) {
    if (out == nullptr) {
        return false;
    }
    if (frame.payload.size() > kMaxFramePayloadLimit ||
        frame.stream_id > kMaxStreamId || !KnownType(frame.type)) {
        return false;
    }
    const std::uint32_t length = static_cast<std::uint32_t>(frame.payload.size());
    out->push_back(static_cast<std::uint8_t>((length >> 16) & 0xFFu));
    out->push_back(static_cast<std::uint8_t>((length >> 8) & 0xFFu));
    out->push_back(static_cast<std::uint8_t>(length & 0xFFu));
    out->push_back(static_cast<std::uint8_t>(frame.type));
    out->push_back(frame.flags);
    PutU32(*out, frame.stream_id & kMaxStreamId);
    out->insert(out->end(), frame.payload.begin(), frame.payload.end());
    return true;
}

FrameDecoder::FrameDecoder(std::uint32_t max_payload) noexcept
    : m_max_payload(max_payload) {}

void FrameDecoder::Append(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    // 已消费前缀超过一半时整体压缩，避免长连接缓冲无限增长。
    if (m_offset > 0 && m_offset >= m_buffer.size() / 2) {
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
    std::size_t consumed = 0;
    const FrameStatus status = DecodeFrame(m_buffer.data() + m_offset, available,
                                           m_max_payload, out, &consumed);
    if (status == FrameStatus::kOk) {
        m_offset += consumed;
        if (m_offset == m_buffer.size()) {
            m_buffer.clear();
            m_offset = 0;
        }
        return FrameStatus::kOk;
    }
    if (status != FrameStatus::kNeedMoreData) {
        // 帧边界已不可信：清空缓冲，由调用方关闭连接。
        Clear();
    }
    return status;
}

void FrameDecoder::Clear() noexcept {
    m_buffer.clear();
    m_offset = 0;
}

// ---------------------------------------------------------------------------
// 整数 / 字符串 / varint
// ---------------------------------------------------------------------------

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void PutU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void PutVarint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80u) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7Fu) | 0x80u));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void PutString(std::vector<std::uint8_t>& out, std::string_view value) {
    PutVarint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

std::uint16_t GetU16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                      static_cast<std::uint16_t>(in[1]));
}

std::uint32_t GetU32(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) |
           static_cast<std::uint32_t>(in[3]);
}

std::uint64_t GetU64(const std::uint8_t* in) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(in[i]);
    }
    return value;
}

bool GetVarint(const std::uint8_t* in, std::size_t size, std::uint64_t* value,
               std::size_t* consumed) noexcept {
    if (in == nullptr || value == nullptr || consumed == nullptr) {
        return false;
    }
    std::uint64_t result = 0;
    std::size_t index = 0;
    int shift = 0;
    while (index < size && index < 10) {
        const std::uint8_t byte = in[index++];
        result |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            *value = result;
            *consumed = index;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool GetString(const std::uint8_t* in, std::size_t size, std::string* value,
               std::size_t* consumed, std::size_t max_bytes) noexcept {
    if (value == nullptr || consumed == nullptr) {
        return false;
    }
    std::uint64_t length = 0;
    std::size_t prefix = 0;
    if (!GetVarint(in, size, &length, &prefix)) {
        return false;
    }
    if (length > max_bytes || prefix + length > size) {
        return false;
    }
    value->assign(reinterpret_cast<const char*>(in + prefix),
                  static_cast<std::size_t>(length));
    *consumed = prefix + static_cast<std::size_t>(length);
    return true;
}

// ---------------------------------------------------------------------------
// 标准载荷
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> EncodeSettings(const std::vector<SettingsEntry>& entries) {
    std::vector<std::uint8_t> out;
    out.reserve(2 + entries.size() * 6);
    if (entries.size() > 0xFFFFu) {
        return {};
    }
    PutU16(out, static_cast<std::uint16_t>(entries.size()));
    for (const SettingsEntry& entry : entries) {
        PutU16(out, static_cast<std::uint16_t>(entry.id));
        PutU32(out, entry.value);
    }
    return out;
}

bool DecodeSettings(const std::uint8_t* data, std::size_t size,
                    std::vector<SettingsEntry>* entries) {
    if (entries == nullptr || size < 2 || (size - 2) % 6 != 0) {
        return false;
    }
    const std::uint16_t count = GetU16(data);
    if (static_cast<std::size_t>(count) * 6 + 2 != size) {
        return false;
    }
    entries->clear();
    entries->reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint8_t* item = data + 2 + static_cast<std::size_t>(i) * 6;
        SettingsEntry entry;
        entry.id = static_cast<SettingId>(GetU16(item));
        entry.value = GetU32(item + 2);
        entries->push_back(entry);
    }
    return true;
}

std::vector<std::uint8_t> EncodeGoAway(std::uint32_t last_stream_id, ErrorCode error,
                                       std::string_view debug) {
    std::vector<std::uint8_t> out;
    PutU32(out, last_stream_id & kMaxStreamId);
    PutU32(out, static_cast<std::uint32_t>(error));
    PutString(out, debug);
    return out;
}

bool DecodeGoAway(const std::uint8_t* data, std::size_t size,
                  std::uint32_t* last_stream_id, ErrorCode* error, std::string* debug) {
    if (data == nullptr || last_stream_id == nullptr || error == nullptr || size < 8) {
        return false;
    }
    const std::uint32_t raw_id = GetU32(data);
    if ((raw_id & 0x80000000u) != 0) {
        return false;
    }
    *last_stream_id = raw_id & kMaxStreamId;
    *error = static_cast<ErrorCode>(GetU32(data + 4));
    if (debug != nullptr) {
        debug->clear();
        std::size_t consumed = 0;
        std::string value;
        if (GetString(data + 8, size - 8, &value, &consumed, 4096)) {
            *debug = std::move(value);
        }
    }
    return true;
}

std::vector<std::uint8_t> EncodeRstStream(ErrorCode error) {
    std::vector<std::uint8_t> out;
    PutU32(out, static_cast<std::uint32_t>(error));
    return out;
}

bool DecodeRstStream(const std::uint8_t* data, std::size_t size, ErrorCode* error) {
    if (data == nullptr || error == nullptr || size < 4) {
        return false;
    }
    *error = static_cast<ErrorCode>(GetU32(data));
    return true;
}

std::vector<std::uint8_t> EncodeWindowUpdate(std::uint32_t increment) {
    std::vector<std::uint8_t> out;
    PutU32(out, increment & kMaxStreamId);
    return out;
}

bool DecodeWindowUpdate(const std::uint8_t* data, std::size_t size,
                        std::uint32_t* increment) {
    if (data == nullptr || increment == nullptr || size < 4) {
        return false;
    }
    const std::uint32_t value = GetU32(data);
    if ((value & 0x80000000u) != 0 || value == 0) {
        return false;
    }
    *increment = value & kMaxStreamId;
    return true;
}

std::vector<std::uint8_t> EncodePriority(std::uint32_t parent_id, std::uint16_t weight) {
    std::vector<std::uint8_t> out;
    PutU32(out, parent_id & kMaxStreamId);
    PutU16(out, weight);
    return out;
}

bool DecodePriority(const std::uint8_t* data, std::size_t size, std::uint32_t* parent_id,
                    std::uint16_t* weight) {
    if (data == nullptr || parent_id == nullptr || weight == nullptr || size < 6) {
        return false;
    }
    const std::uint32_t raw = GetU32(data);
    if ((raw & 0x80000000u) != 0) {
        return false;
    }
    *parent_id = raw & kMaxStreamId;
    *weight = GetU16(data + 4);
    return true;
}

std::vector<std::uint8_t> EncodePing(const std::array<std::uint8_t, 8>& opaque) {
    return std::vector<std::uint8_t>(opaque.begin(), opaque.end());
}

bool DecodePing(const std::uint8_t* data, std::size_t size,
                std::array<std::uint8_t, 8>* opaque) {
    if (data == nullptr || opaque == nullptr || size != opaque->size()) {
        return false;
    }
    std::copy(data, data + opaque->size(), opaque->begin());
    return true;
}

}  // namespace go2cpp::app
