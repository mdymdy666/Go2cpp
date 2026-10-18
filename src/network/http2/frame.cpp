/**
 * @file frame.cpp
 * @brief HTTP/2 帧层实现（RFC 9113 §4 / §6）。
 *
 * ## 模块定位
 * 帧层实现：`Connection` 只通过 `DecodeFrame()` / `ParseXxx` / `MakeXxx` 与它打交道。
 * 本文件不持有状态、不分配长寿命对象，所有函数都是「输入校验 + 结构体映射」。
 *
 * ## 本文件定义什么
 * - 字节序小工具：`ReadUint24` / `ReadUint32` / `WriteUint24` / `WriteUint32`；
 * - 帧头：`DecodeFrameHeader()` / `EncodeFrameHeader()`；
 * - 通用帧：`DecodeFrame()`（增量解析，返回 `kNeedMore`）；
 * - 各语义帧的 `ParseXxx` 与 `MakeXxx`；
 * - `StripPadding()`：DATA/HEADERS 共用的 padding 剥离。
 *
 * ## 阅读建议
 * 先看 `DecodeFrame()`（先判长度、再判 stream id、最后判 payload 是否收全），
 * 再看任一 `ParseXxx` —— 它们的形状完全一致：校验类型/流 id/payload 长度，再取字段。
 *
 * ## 边界与陷阱
 * - 所有 `ParseXxx` 只做「帧内自洽性」校验，不查流状态（那是 `Connection` 的事）；
 * - `DecodeFrame()` 的 `kNeedMore` 优先于其它错误：帧可能只是还没收全；
 * - `Make*` 必须同步维护 `header.length`，否则线上字节数与声明不符。
 */
#include "go2cpp/network/http2/frame.hpp"

#include <cstring>

namespace go2cpp::net::http2 {

namespace {

std::uint32_t ReadUint24(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 16) |
           (static_cast<std::uint32_t>(in[1]) << 8) |
           static_cast<std::uint32_t>(in[2]);
}

std::uint32_t ReadUint32(const std::uint8_t* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) |
           static_cast<std::uint32_t>(in[3]);
}

void WriteUint24(std::uint32_t value, std::uint8_t* out) noexcept {
    out[0] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    out[2] = static_cast<std::uint8_t>(value & 0xFFU);
}

void WriteUint32(std::uint32_t value, std::uint8_t* out) noexcept {
    out[0] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    out[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

std::uint32_t MaskStreamId(std::uint32_t value) noexcept {
    // 最高位必须为 0；接收方忽略该位（RFC 9113 §4.1）
    return value & 0x7FFFFFFFU;
}

/// 该帧类型是否要求 stream id == 0。
bool RequiresStreamZero(FrameType type) noexcept {
    return type == FrameType::kSettings || type == FrameType::kPing ||
           type == FrameType::kGoAway;
}

/// 该帧类型是否要求 stream id != 0。
bool RequiresNonZeroStream(FrameType type) noexcept {
    return type == FrameType::kData || type == FrameType::kHeaders ||
           type == FrameType::kPriority || type == FrameType::kRstStream ||
           type == FrameType::kPushPromise || type == FrameType::kContinuation;
}

/// 从 payload 中剥掉 padding（DATA/HEADERS 共用）。
/**
 * @brief 从 payload 中剥掉 padding（DATA/HEADERS 共用）。
 *
 * ## 步骤
 * step1. 未置 PADDED 位则原样返回 `[0, size)`；
 * step2. PADDED 时首字节是 pad length，必须满足 `pad_length + 1 <= payload.size()`；
 * step3. 返回 `[1, size - pad_length)`，把校验过的区间交给调用方。
 *
 * ## 失败语义
 * payload 为空或 padding 越界都返回 `kProtocolError`（不是 `kNeedMore`：帧已完整）。
 */
FrameStatus StripPadding(const std::vector<std::uint8_t>& payload, bool padded,
                         std::size_t* begin, std::size_t* end,
                         std::string* error) {
    *begin = 0;
    *end = payload.size();
    if (!padded) {
        return FrameStatus::kOk;
    }
    // step1/2: PADDED 帧至少要有一个 pad length 字节。
    if (payload.empty()) {
        if (error != nullptr) {
            *error = "PADDED 帧的 payload 为空，缺少 pad length 字节";
        }
        return FrameStatus::kProtocolError;
    }
    const std::size_t pad_length = payload[0];
    if (pad_length + 1 > payload.size()) {
        if (error != nullptr) {
            *error = "PADDED 帧的 padding 长度超出 payload";
        }
        return FrameStatus::kProtocolError;
    }
    *begin = 1;
    *end = payload.size() - pad_length;
    return FrameStatus::kOk;
}

}  // namespace

const char* ToString(FrameStatus status) noexcept {
    switch (status) {
        case FrameStatus::kOk: return "ok";
        case FrameStatus::kNeedMore: return "need-more";
        case FrameStatus::kProtocolError: return "protocol-error";
        case FrameStatus::kFrameSizeError: return "frame-size-error";
        case FrameStatus::kUnknownType: return "unknown-type";
    }
    return "unknown";
}

const char* ToString(FrameType type) noexcept {
    switch (type) {
        case FrameType::kData: return "DATA";
        case FrameType::kHeaders: return "HEADERS";
        case FrameType::kPriority: return "PRIORITY";
        case FrameType::kRstStream: return "RST_STREAM";
        case FrameType::kSettings: return "SETTINGS";
        case FrameType::kPushPromise: return "PUSH_PROMISE";
        case FrameType::kPing: return "PING";
        case FrameType::kGoAway: return "GOAWAY";
        case FrameType::kWindowUpdate: return "WINDOW_UPDATE";
        case FrameType::kContinuation: return "CONTINUATION";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 帧头
// ---------------------------------------------------------------------------

/**
 * @brief 解析 9 字节帧头。
 *
 * ## 说明
 * `in` 不足 9 字节返回 `kNeedMore`（不是错误）；stream id 的最高位按 RFC 9113 §4.1
 * 被忽略（`MaskStreamId`）。
 */
FrameStatus DecodeFrameHeader(const std::uint8_t* in, std::size_t length,
                              FrameHeader* out) noexcept {
    if (out == nullptr) {
        return FrameStatus::kProtocolError;
    }
    if (in == nullptr || length < kFrameHeaderSize) {
        // 关键：帧头还没收全，属于 kNeedMore，不是错误
        return FrameStatus::kNeedMore;
    }
    out->length = ReadUint24(in);
    out->type = static_cast<FrameType>(in[3]);
    out->flags = in[4];
    out->stream_id = MaskStreamId(ReadUint32(in + 5));
    return FrameStatus::kOk;
}

void EncodeFrameHeader(const FrameHeader& header, std::uint8_t out[kFrameHeaderSize]) noexcept {
    WriteUint24(header.length, out);
    out[3] = static_cast<std::uint8_t>(header.type);
    out[4] = header.flags;
    WriteUint32(MaskStreamId(header.stream_id), out + 5);
}

/**
 * @brief 从字节流中解析一个完整帧（增量解析）。
 *
 * ## 步骤
 * step1. 先解析帧头；不足 9 字节 -> `kNeedMore`；
 * step2. 校验声明长度不超过对端 `max_frame_size`（超限立即报错，不等 payload）；
 * step3. 校验 stream id 规则（SETTINGS/PING/GOAWAY 必须为 0，其余必须非 0）；
 * step4. payload 未收全 -> `kNeedMore`，收全则拷贝并写 `consumed`。
 *
 * ## 边界
 * 校验顺序很关键：长度与 stream id 都能在拿到帧头后立即判定，不必等 payload。
 */
FrameStatus DecodeFrame(const std::uint8_t* in, std::size_t length, Frame* out,
                        std::size_t* consumed,
                        std::uint32_t max_frame_size) noexcept {
    if (out == nullptr || consumed == nullptr) {
        return FrameStatus::kProtocolError;
    }
    FrameHeader header;
    const auto header_status = DecodeFrameHeader(in, length, &header);
    if (header_status != FrameStatus::kOk) {
        return header_status;
    }
    // step2: 超限不必等 payload，直接报错。
    if (header.length > max_frame_size) {
        // 先判长度：超限说明对端违约，不必等 payload 收全
        return FrameStatus::kFrameSizeError;
    }
    // stream id 规则可以在拿到帧头后就校验，不必等 payload
    if (RequiresStreamZero(header.type) && header.stream_id != 0) {
        return FrameStatus::kProtocolError;
    }
    if (RequiresNonZeroStream(header.type) && header.stream_id == 0) {
        return FrameStatus::kProtocolError;
    }
    // step4: payload 未收全，等更多字节。
    if (length < kFrameHeaderSize + header.length) {
        return FrameStatus::kNeedMore;
    }
    out->header = header;
    out->payload.assign(in + kFrameHeaderSize, in + kFrameHeaderSize + header.length);
    *consumed = kFrameHeaderSize + header.length;
    return FrameStatus::kOk;
}

// ---------------------------------------------------------------------------
// SETTINGS
// ---------------------------------------------------------------------------

/**
 * @brief 解析 SETTINGS payload。
 *
 * ## 步骤
 * step1. 校验帧类型 SETTINGS 且 stream id 为 0；
 * step2. ACK 帧必须空 payload，非 ACK 帧长度必须是 6 的倍数；
 * step3. 每 6 字节拆成 (id, value)，按出现顺序保留（重复项由上层决定语义）。
 */
FrameStatus ParseSettings(const Frame& frame, SettingsFrame* out, std::string* error) {
    if (out == nullptr) {
        return FrameStatus::kProtocolError;
    }
    if (frame.header.type != FrameType::kSettings) {
        if (error != nullptr) *error = "帧类型不是 SETTINGS";
        return FrameStatus::kProtocolError;
    }
    if (frame.header.stream_id != 0) {
        if (error != nullptr) *error = "SETTINGS 的 stream id 必须为 0";
        return FrameStatus::kProtocolError;
    }
    out->ack = (frame.header.flags & FrameFlags::kAck) != 0;
    out->entries.clear();
    // step2: ACK 必须空 payload（RFC 9113 §6.5）。
    if (out->ack) {
        if (!frame.payload.empty()) {
            if (error != nullptr) *error = "SETTINGS ACK 的 payload 必须为空";
            return FrameStatus::kProtocolError;
        }
        return FrameStatus::kOk;
    }
    if (frame.payload.size() % 6 != 0) {
        if (error != nullptr) *error = "SETTINGS payload 长度必须是 6 的倍数";
        return FrameStatus::kProtocolError;
    }
    // step3: 每个条目是 2 字节 id + 4 字节 value。
    for (std::size_t offset = 0; offset + 6 <= frame.payload.size(); offset += 6) {
        SettingsEntry entry;
        entry.id = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(frame.payload[offset]) << 8) |
            static_cast<std::uint16_t>(frame.payload[offset + 1]));
        entry.value = ReadUint32(frame.payload.data() + offset + 2);
        out->entries.push_back(entry);
    }
    return FrameStatus::kOk;
}

Frame MakeSettings(const std::vector<SettingsEntry>& entries) {
    Frame frame;
    frame.header.type = FrameType::kSettings;
    frame.header.stream_id = 0;
    frame.header.flags = 0;
    frame.payload.resize(entries.size() * 6);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        frame.payload[i * 6] = static_cast<std::uint8_t>((entries[i].id >> 8) & 0xFFU);
        frame.payload[i * 6 + 1] = static_cast<std::uint8_t>(entries[i].id & 0xFFU);
        WriteUint32(entries[i].value, frame.payload.data() + i * 6 + 2);
    }
    frame.header.length = static_cast<std::uint32_t>(frame.payload.size());
    return frame;
}

Frame MakeSettingsAck() {
    Frame frame;
    frame.header.type = FrameType::kSettings;
    frame.header.stream_id = 0;
    frame.header.flags = FrameFlags::kAck;
    frame.header.length = 0;
    return frame;
}

// ---------------------------------------------------------------------------
// PING
// ---------------------------------------------------------------------------

FrameStatus ParsePing(const Frame& frame, PingFrame* out, std::string* error) {
    if (out == nullptr) {
        return FrameStatus::kProtocolError;
    }
    if (frame.header.type != FrameType::kPing) {
        if (error != nullptr) *error = "帧类型不是 PING";
        return FrameStatus::kProtocolError;
    }
    if (frame.header.stream_id != 0) {
        if (error != nullptr) *error = "PING 的 stream id 必须为 0";
        return FrameStatus::kProtocolError;
    }
    if (frame.payload.size() != 8) {
        if (error != nullptr) *error = "PING payload 必须恰好 8 字节";
        return FrameStatus::kProtocolError;
    }
    out->ack = (frame.header.flags & FrameFlags::kAck) != 0;
    out->opaque_data = (static_cast<std::uint64_t>(ReadUint32(frame.payload.data())) << 32) |
                       static_cast<std::uint64_t>(ReadUint32(frame.payload.data() + 4));
    return FrameStatus::kOk;
}

Frame MakePing(std::uint64_t opaque_data, bool ack) {
    Frame frame;
    frame.header.type = FrameType::kPing;
    frame.header.stream_id = 0;
    frame.header.flags = ack ? FrameFlags::kAck : 0;
    frame.payload.resize(8);
    WriteUint32(static_cast<std::uint32_t>(opaque_data >> 32), frame.payload.data());
    WriteUint32(static_cast<std::uint32_t>(opaque_data & 0xFFFFFFFFULL),
                frame.payload.data() + 4);
    frame.header.length = 8;
    return frame;
}

// ---------------------------------------------------------------------------
// DATA
// ---------------------------------------------------------------------------

FrameStatus ParseData(const Frame& frame, DataFrame* out, std::string* error) {
    if (out == nullptr) {
        return FrameStatus::kProtocolError;
    }
    if (frame.header.type != FrameType::kData) {
        if (error != nullptr) *error = "帧类型不是 DATA";
        return FrameStatus::kProtocolError;
    }
    if (frame.header.stream_id == 0) {
        if (error != nullptr) *error = "DATA 的 stream id 不能为 0";
        return FrameStatus::kProtocolError;
    }
    const bool padded = (frame.header.flags & FrameFlags::kPadded) != 0;
    std::size_t begin = 0;
    std::size_t end = 0;
    const auto status = StripPadding(frame.payload, padded, &begin, &end, error);
    if (status != FrameStatus::kOk) {
        return status;
    }
    out->stream_id = frame.header.stream_id;
    out->end_stream = (frame.header.flags & FrameFlags::kEndStream) != 0;
    out->data.assign(frame.payload.begin() + static_cast<std::ptrdiff_t>(begin),
                     frame.payload.begin() + static_cast<std::ptrdiff_t>(end));
    return FrameStatus::kOk;
}

Frame MakeData(std::uint32_t stream_id, bool end_stream,
               const std::vector<std::uint8_t>& data) {
    Frame frame;
    frame.header.type = FrameType::kData;
    frame.header.stream_id = MaskStreamId(stream_id);
    frame.header.flags = end_stream ? FrameFlags::kEndStream : 0;
    frame.payload = data;
    frame.header.length = static_cast<std::uint32_t>(frame.payload.size());
    return frame;
}

// ---------------------------------------------------------------------------
// HEADERS / CONTINUATION
// ---------------------------------------------------------------------------

/**
 * @brief 解析 HEADERS（含可选的 PADDED 与 PRIORITY 字段）。
 *
 * ## 步骤
 * step1. 校验类型与 stream id，剥 padding；
 * step2. 若带 PRIORITY 标志，跳过固定 5 字节（1 字节独占+依赖 + 1 字节权重）；
 * step3. 剩余字节即 HPACK header block 片段（可能只是第一片）。
 *
 * ## 边界
 * `end_headers` 为假表示后面还有 CONTINUATION，由 `AssembleHeaderBlock()` 拼。
 */
FrameStatus ParseHeaders(const Frame& frame, HeadersFrame* out, std::string* error) {
    if (out == nullptr) {
        return FrameStatus::kProtocolError;
    }
    if (frame.header.type != FrameType::kHeaders) {
        if (error != nullptr) *error = "帧类型不是 HEADERS";
        return FrameStatus::kProtocolError;
    }
    if (frame.header.stream_id == 0) {
        if (error != nullptr) *error = "HEADERS 的 stream id 不能为 0";
        return FrameStatus::kProtocolError;
    }
    const bool padded = (frame.header.flags & FrameFlags::kPadded) != 0;
    const bool has_priority = (frame.header.flags & FrameFlags::kPriority) != 0;
    std::size_t begin = 0;
    std::size_t end = 0;
    // step1: 先剥 padding，再处理优先级字段。
    auto status = StripPadding(frame.payload, padded, &begin, &end, error);
    if (status != FrameStatus::kOk) {
        return status;
    }
    // step2: 优先级字段固定 5 字节，解析但不使用。
    if (has_priority) {
        // 优先级字段固定 5 字节（1 字节独占+依赖 + 1 字节权重）
        if (end < begin + 5) {
            if (error != nullptr) *error = "HEADERS 带 PRIORITY 但长度不足 5 字节";
            return FrameStatus::kProtocolError;
        }
        begin += 5;
    }
    out->stream_id = frame.header.stream_id;
    out->end_stream = (frame.header.flags & FrameFlags::kEndStream) != 0;
    out->end_headers = (frame.header.flags & FrameFlags::kEndHeaders) != 0;
    out->header_block.assign(frame.payload.begin() + static_cast<std::ptrdiff_t>(begin),
                             frame.payload.begin() + static_cast<std::ptrdiff_t>(end));
    return FrameStatus::kOk;
}

Frame MakeHeaders(std::uint32_t stream_id, bool end_stream, bool end_headers,
                  const std::vector<std::uint8_t>& header_block) {
    Frame frame;
    frame.header.type = FrameType::kHeaders;
    frame.header.stream_id = MaskStreamId(stream_id);
    frame.header.flags = (end_stream ? FrameFlags::kEndStream : 0) |
                         (end_headers ? FrameFlags::kEndHeaders : 0);
    frame.payload = header_block;
    frame.header.length = static_cast<std::uint32_t>(frame.payload.size());
    return frame;
}

FrameStatus ParseContinuation(const Frame& frame, HeadersFrame* out, std::string* error) {
    if (out == nullptr) {
        return FrameStatus::kProtocolError;
    }
    if (frame.header.type != FrameType::kContinuation) {
        if (error != nullptr) *error = "帧类型不是 CONTINUATION";
        return FrameStatus::kProtocolError;
    }
    if (frame.header.stream_id == 0) {
        if (error != nullptr) *error = "CONTINUATION 的 stream id 不能为 0";
        return FrameStatus::kProtocolError;
    }
    out->stream_id = frame.header.stream_id;
    out->end_stream = false;
    out->end_headers = (frame.header.flags & FrameFlags::kEndHeaders) != 0;
    out->header_block = frame.payload;
    return FrameStatus::kOk;
}

Frame MakeContinuation(std::uint32_t stream_id, bool end_headers,
                       const std::vector<std::uint8_t>& header_block) {
    Frame frame;
    frame.header.type = FrameType::kContinuation;
    frame.header.stream_id = MaskStreamId(stream_id);
    frame.header.flags = end_headers ? FrameFlags::kEndHeaders : 0;
    frame.payload = header_block;
    frame.header.length = static_cast<std::uint32_t>(frame.payload.size());
    return frame;
}

// ---------------------------------------------------------------------------
// RST_STREAM / GOAWAY / WINDOW_UPDATE / PRIORITY
// ---------------------------------------------------------------------------

FrameStatus ParseRstStream(const Frame& frame, RstStreamFrame* out, std::string* error) {
    if (out == nullptr) {
        return FrameStatus::kProtocolError;
    }
    if (frame.header.type != FrameType::kRstStream) {
        if (error != nullptr) *error = "帧类型不是 RST_STREAM";
        return FrameStatus::kProtocolError;
    }
    if (frame.header.stream_id == 0) {
        if (error != nullptr) *error = "RST_STREAM 的 stream id 不能为 0";
        return FrameStatus::kProtocolError;
    }
    if (frame.payload.size() != 4) {
        if (error != nullptr) *error = "RST_STREAM payload 必须恰好 4 字节";
        return FrameStatus::kProtocolError;
    }
    out->stream_id = frame.header.stream_id;
    out->error_code = static_cast<ErrorCode>(ReadUint32(frame.payload.data()));
    return FrameStatus::kOk;
}

Frame MakeRstStream(std::uint32_t stream_id, ErrorCode error_code) {
    Frame frame;
    frame.header.type = FrameType::kRstStream;
    frame.header.stream_id = MaskStreamId(stream_id);
    frame.payload.resize(4);
    WriteUint32(static_cast<std::uint32_t>(error_code), frame.payload.data());
    frame.header.length = 4;
    return frame;
}

FrameStatus ParseGoAway(const Frame& frame, GoAwayFrame* out, std::string* error) {
    if (out == nullptr) {
        return FrameStatus::kProtocolError;
    }
    if (frame.header.type != FrameType::kGoAway) {
        if (error != nullptr) *error = "帧类型不是 GOAWAY";
        return FrameStatus::kProtocolError;
    }
    if (frame.header.stream_id != 0) {
        if (error != nullptr) *error = "GOAWAY 的 stream id 必须为 0";
        return FrameStatus::kProtocolError;
    }
    if (frame.payload.size() < 8) {
        if (error != nullptr) *error = "GOAWAY payload 至少 8 字节";
        return FrameStatus::kProtocolError;
    }
    out->last_stream_id = MaskStreamId(ReadUint32(frame.payload.data()));
    out->error_code = static_cast<ErrorCode>(ReadUint32(frame.payload.data() + 4));
    out->debug_data.assign(frame.payload.begin() + 8, frame.payload.end());
    return FrameStatus::kOk;
}

Frame MakeGoAway(std::uint32_t last_stream_id, ErrorCode error_code,
                 const std::vector<std::uint8_t>& debug_data) {
    Frame frame;
    frame.header.type = FrameType::kGoAway;
    frame.header.stream_id = 0;
    frame.payload.resize(8 + debug_data.size());
    WriteUint32(MaskStreamId(last_stream_id), frame.payload.data());
    WriteUint32(static_cast<std::uint32_t>(error_code), frame.payload.data() + 4);
    if (!debug_data.empty()) {
        std::memcpy(frame.payload.data() + 8, debug_data.data(), debug_data.size());
    }
    frame.header.length = static_cast<std::uint32_t>(frame.payload.size());
    return frame;
}

/**
 * @brief 解析 WINDOW_UPDATE（stream id 为 0 表示连接级）。
 *
 * ## 说明
 * increment 是 31 位值，为 0 属于协议错误（RFC 9113 §6.9）；本文件只做形状校验，
 * 窗口是否溢出由 `Connection` 在扣/补时判断。
 */
FrameStatus ParseWindowUpdate(const Frame& frame, WindowUpdateFrame* out,
                              std::string* error) {
    if (out == nullptr) {
        return FrameStatus::kProtocolError;
    }
    if (frame.header.type != FrameType::kWindowUpdate) {
        if (error != nullptr) *error = "帧类型不是 WINDOW_UPDATE";
        return FrameStatus::kProtocolError;
    }
    if (frame.payload.size() != 4) {
        if (error != nullptr) *error = "WINDOW_UPDATE payload 必须恰好 4 字节";
        return FrameStatus::kProtocolError;
    }
    out->stream_id = frame.header.stream_id;   // 允许 0（连接级）
    out->increment = MaskStreamId(ReadUint32(frame.payload.data()));
    if (out->increment == 0) {
        if (error != nullptr) *error = "WINDOW_UPDATE 的增量为 0，属于协议错误";
        return FrameStatus::kProtocolError;
    }
    return FrameStatus::kOk;
}

Frame MakeWindowUpdate(std::uint32_t stream_id, std::uint32_t increment) {
    Frame frame;
    frame.header.type = FrameType::kWindowUpdate;
    frame.header.stream_id = MaskStreamId(stream_id);
    frame.payload.resize(4);
    WriteUint32(MaskStreamId(increment), frame.payload.data());
    frame.header.length = 4;
    return frame;
}

FrameStatus ParsePriority(const Frame& frame, PriorityFrame* out, std::string* error) {
    if (out == nullptr) {
        return FrameStatus::kProtocolError;
    }
    if (frame.header.type != FrameType::kPriority) {
        if (error != nullptr) *error = "帧类型不是 PRIORITY";
        return FrameStatus::kProtocolError;
    }
    if (frame.header.stream_id == 0) {
        if (error != nullptr) *error = "PRIORITY 的 stream id 不能为 0";
        return FrameStatus::kProtocolError;
    }
    if (frame.payload.size() != 5) {
        if (error != nullptr) *error = "PRIORITY payload 必须恰好 5 字节";
        return FrameStatus::kProtocolError;
    }
    const std::uint32_t raw = ReadUint32(frame.payload.data());
    out->stream_id = frame.header.stream_id;
    out->exclusive = (raw & 0x80000000U) != 0;
    out->depends_on = raw & 0x7FFFFFFFU;
    out->weight = frame.payload[4];
    return FrameStatus::kOk;
}

Frame MakePriority(std::uint32_t stream_id, std::uint32_t depends_on, bool exclusive,
                   std::uint8_t weight) {
    Frame frame;
    frame.header.type = FrameType::kPriority;
    frame.header.stream_id = MaskStreamId(stream_id);
    frame.payload.resize(5);
    const std::uint32_t raw = (exclusive ? 0x80000000U : 0U) | MaskStreamId(depends_on);
    WriteUint32(raw, frame.payload.data());
    frame.payload[4] = weight;
    frame.header.length = 5;
    return frame;
}

}  // namespace go2cpp::net::http2
