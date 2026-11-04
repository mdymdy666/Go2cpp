/**
 * @file protocol.cpp
 * @brief RPC 帧载荷编解码实现（varint + 长度前缀，严格边界检查）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/rpc/protocol.hpp"

#include <cstring>
#include <string>
#include <utility>

namespace go2cpp::rpc {
namespace {

/// @brief 向 vector 追加 varint（LEB128）。
class Writer {
   public:
    explicit Writer(std::vector<std::uint8_t>& out) : m_out(out) { m_out.clear(); }

    void U8(std::uint8_t value) { m_out.push_back(value); }

    void Varint(std::uint64_t value) {
        while (value >= 0x80u) {
            m_out.push_back(static_cast<std::uint8_t>(value) | 0x80u);
            value >>= 7;
        }
        m_out.push_back(static_cast<std::uint8_t>(value));
    }

    void Bytes(const void* data, std::size_t size) {
        if (size == 0) {
            return;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        m_out.insert(m_out.end(), bytes, bytes + size);
    }

    void String(std::string_view text) {
        Varint(static_cast<std::uint64_t>(text.size()));
        Bytes(text.data(), text.size());
    }

   private:
    std::vector<std::uint8_t>& m_out;  ///< 输出缓冲。
};

/**
 * @brief 只读游标：所有读取都做剩余长度检查，绝不越界。
 */
class Reader {
   public:
    Reader(const std::uint8_t* data, std::size_t size) noexcept
        : m_data(data), m_size(size) {}

    bool U8(std::uint8_t& value) noexcept {
        if (m_offset + 1 > m_size) {
            return false;
        }
        value = m_data[m_offset++];
        return true;
    }

    bool Varint(std::uint64_t& value) noexcept {
        std::uint64_t result = 0;
        unsigned shift = 0;
        for (std::size_t index = 0; index < 10; ++index) {
            std::uint8_t byte = 0;
            if (!U8(byte)) {
                return false;
            }
            if (index == 9 && byte > 1) {
                return false;
            }
            result |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
            if ((byte & 0x80u) == 0) {
                value = result;
                return true;
            }
            shift += 7;
        }
        return false;
    }

    bool String(std::string& value, std::size_t max_bytes) {
        std::uint64_t length = 0;
        if (!Varint(length)) {
            return false;
        }
        if (length > max_bytes || length > Remaining()) {
            return false;
        }
        if (length == 0) {
            value.clear();
            return true;
        }
        value.assign(reinterpret_cast<const char*>(m_data + m_offset),
                     static_cast<std::size_t>(length));
        m_offset += static_cast<std::size_t>(length);
        return true;
    }

    bool Slice(std::uint64_t length, const std::uint8_t** data) noexcept {
        if (length > Remaining()) {
            return false;
        }
        *data = length == 0 ? nullptr : m_data + m_offset;
        m_offset += static_cast<std::size_t>(length);
        return true;
    }

    std::size_t Remaining() const noexcept { return m_size - m_offset; }
    bool AtEnd() const noexcept { return m_offset == m_size; }

   private:
    const std::uint8_t* m_data;  ///< 帧载荷首地址（可能为 nullptr）。
    std::size_t m_size;          ///< 载荷字节数。
    std::size_t m_offset = 0;    ///< 当前读位置。
};

/// @brief 写失败原因（`error` 可为空）。
bool Fail(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

/// @brief 状态码是否在协议允许范围（0..15）。
bool ValidStatusCode(std::uint8_t raw) noexcept {
    return raw <= static_cast<std::uint8_t>(StatusCode::kDataLoss);
}

}  // namespace

const char* FrameTypeName(FrameType type) noexcept {
    switch (type) {
        case FrameType::kRequest:
            return "REQUEST";
        case FrameType::kResponse:
            return "RESPONSE";
        case FrameType::kStreamData:
            return "STREAM_DATA";
        case FrameType::kStreamEnd:
            return "STREAM_END";
        case FrameType::kCancel:
            return "CANCEL";
        case FrameType::kPing:
            return "PING";
        case FrameType::kPong:
            return "PONG";
    }
    return "UNKNOWN";
}

/// @brief 写 8 字节大端整数（追踪上下文的线上表示）。
void WriteU64(Writer& writer, std::uint64_t value) {
    std::uint8_t bytes[8];
    for (int index = 7; index >= 0; --index) {
        bytes[index] = static_cast<std::uint8_t>(value & 0xFFu);
        value >>= 8;
    }
    writer.Bytes(bytes, sizeof(bytes));
}

/// @brief 读 8 字节大端整数；失败返回 false 且不写 out。
bool ReadU64(Reader& reader, std::uint64_t& out) noexcept {
    const std::uint8_t* bytes = nullptr;
    if (!reader.Slice(8, &bytes)) {
        return false;
    }
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | bytes[index];
    }
    out = value;
    return true;
}

bool EncodeRequest(std::vector<std::uint8_t>& out, std::uint64_t call_id,
                   std::string_view method, const reflect::Buffer& args) {
    return EncodeRequest(out, call_id, method, args, distributed::TraceContext{});
}

bool EncodeRequest(std::vector<std::uint8_t>& out, std::uint64_t call_id,
                   std::string_view method, const reflect::Buffer& args,
                   const distributed::TraceContext& trace) {
    if (method.empty() || method.size() > kMaxMethodBytes) {
        out.clear();
        return false;
    }
    Writer writer(out);
    writer.Varint(call_id);
    writer.String(method);
    writer.Varint(static_cast<std::uint64_t>(args.size()));
    writer.Bytes(args.data().data(), args.size());
    if (trace.IsValid()) {
        // 固定 32 字节：trace.hi | trace.lo | span | parent（大端）。
        WriteU64(writer, trace.trace.high());
        WriteU64(writer, trace.trace.low());
        WriteU64(writer, trace.span.value());
        WriteU64(writer, trace.parent.value());
    }
    return true;
}

bool EncodeResponse(std::vector<std::uint8_t>& out, std::uint64_t call_id,
                    const Status& status, const reflect::Buffer& payload) {
    const auto raw_code = static_cast<std::uint8_t>(status.code);
    if (!ValidStatusCode(raw_code) || status.message.size() > kMaxStatusMessageBytes) {
        out.clear();
        return false;
    }
    Writer writer(out);
    writer.Varint(call_id);
    writer.U8(raw_code);
    writer.String(status.message);
    writer.Varint(static_cast<std::uint64_t>(payload.size()));
    writer.Bytes(payload.data().data(), payload.size());
    return true;
}

bool EncodeStreamData(std::vector<std::uint8_t>& out, std::uint64_t call_id,
                      std::uint64_t sequence, const void* payload, std::size_t size) {
    Writer writer(out);
    writer.Varint(call_id);
    writer.Varint(sequence);
    writer.Varint(static_cast<std::uint64_t>(size));
    writer.Bytes(payload, size);
    return true;
}

bool EncodeStreamEnd(std::vector<std::uint8_t>& out, std::uint64_t call_id,
                     const Status& status) {
    const auto raw_code = static_cast<std::uint8_t>(status.code);
    if (!ValidStatusCode(raw_code) || status.message.size() > kMaxStatusMessageBytes) {
        out.clear();
        return false;
    }
    Writer writer(out);
    writer.Varint(call_id);
    writer.U8(raw_code);
    writer.String(status.message);
    return true;
}

bool EncodeCancel(std::vector<std::uint8_t>& out, std::uint64_t call_id) {
    Writer writer(out);
    writer.Varint(call_id);
    return true;
}

bool EncodePing(std::vector<std::uint8_t>& out, std::uint64_t call_id, bool pong) {
    (void)pong;  // 帧 type 已区分 Ping/Pong，载荷只带 call id。
    Writer writer(out);
    writer.Varint(call_id);
    return true;
}

bool DecodeRequest(const frame::Frame& input, RequestMessage* out, std::string* error) {
    if (out == nullptr) {
        return Fail(error, "null request output");
    }
    if (input.type != static_cast<std::uint8_t>(FrameType::kRequest)) {
        return Fail(error, "frame is not a request");
    }
    // 每次解码都从「无 trace」开始，避免复用 RequestMessage 时残留旧值。
    out->has_trace = false;
    out->trace = distributed::TraceContext{};
    Reader reader(input.payload.data(), input.payload.size());
    std::uint64_t call_id = 0;
    std::string method;
    std::uint64_t args_size = 0;
    if (!reader.Varint(call_id)) {
        return Fail(error, "request: bad call id");
    }
    if (!reader.String(method, kMaxMethodBytes)) {
        return Fail(error, "request: bad method name");
    }
    if (!reader.Varint(args_size)) {
        return Fail(error, "request: bad argument length");
    }
    const std::uint8_t* args = nullptr;
    if (!reader.Slice(args_size, &args)) {
        return Fail(error, "request: truncated arguments");
    }
    if ((input.flags & kFlagTrace) != 0) {
        std::uint64_t high = 0;
        std::uint64_t low = 0;
        std::uint64_t span = 0;
        std::uint64_t parent = 0;
        if (!ReadU64(reader, high) || !ReadU64(reader, low) ||
            !ReadU64(reader, span) || !ReadU64(reader, parent)) {
            return Fail(error, "request: truncated trace context");
        }
        out->trace.trace = distributed::TraceId::FromParts(high, low);
        out->trace.span = distributed::SpanId(span);
        out->trace.parent = distributed::SpanId(parent);
        out->has_trace = out->trace.IsValid();
    }
    if (!reader.AtEnd()) {
        return Fail(error, "request: trailing bytes");
    }
    out->call_id = call_id;
    out->method = std::move(method);
    out->streaming = (input.flags & kFlagStreaming) != 0;
    out->args.Clear();
    if (args_size > 0 && !out->args.PutBytes(args, static_cast<std::size_t>(args_size))) {
        return Fail(error, "request: argument buffer overflow");
    }
    return true;
}

bool DecodeResponse(const frame::Frame& input, ResponseMessage* out, std::string* error) {
    if (out == nullptr) {
        return Fail(error, "null response output");
    }
    if (input.type != static_cast<std::uint8_t>(FrameType::kResponse)) {
        return Fail(error, "frame is not a response");
    }
    Reader reader(input.payload.data(), input.payload.size());
    std::uint64_t call_id = 0;
    std::uint8_t raw_status = 0;
    std::string message;
    std::uint64_t result_size = 0;
    if (!reader.Varint(call_id)) {
        return Fail(error, "response: bad call id");
    }
    if (!reader.U8(raw_status) || !ValidStatusCode(raw_status)) {
        return Fail(error, "response: bad status code");
    }
    if (!reader.String(message, kMaxStatusMessageBytes)) {
        return Fail(error, "response: bad status message");
    }
    if (!reader.Varint(result_size)) {
        return Fail(error, "response: bad result length");
    }
    const std::uint8_t* result = nullptr;
    if (!reader.Slice(result_size, &result)) {
        return Fail(error, "response: truncated result");
    }
    if (!reader.AtEnd()) {
        return Fail(error, "response: trailing bytes");
    }
    out->call_id = call_id;
    out->status.code = static_cast<StatusCode>(raw_status);
    out->status.message = std::move(message);
    out->payload.Clear();
    if (result_size > 0 &&
        !out->payload.PutBytes(result, static_cast<std::size_t>(result_size))) {
        return Fail(error, "response: result buffer overflow");
    }
    return true;
}

bool DecodeStreamData(const frame::Frame& input, StreamDataMessage* out, std::string* error) {
    if (out == nullptr) {
        return Fail(error, "null stream data output");
    }
    if (input.type != static_cast<std::uint8_t>(FrameType::kStreamData)) {
        return Fail(error, "frame is not stream data");
    }
    Reader reader(input.payload.data(), input.payload.size());
    std::uint64_t call_id = 0;
    std::uint64_t sequence = 0;
    std::uint64_t payload_size = 0;
    if (!reader.Varint(call_id) || !reader.Varint(sequence) || !reader.Varint(payload_size)) {
        return Fail(error, "stream data: bad header");
    }
    const std::uint8_t* payload = nullptr;
    if (!reader.Slice(payload_size, &payload)) {
        return Fail(error, "stream data: truncated payload");
    }
    if (!reader.AtEnd()) {
        return Fail(error, "stream data: trailing bytes");
    }
    out->call_id = call_id;
    out->sequence = sequence;
    out->payload.clear();
    if (payload_size > 0) {
        out->payload.assign(payload, payload + static_cast<std::size_t>(payload_size));
    }
    return true;
}

bool DecodeStreamEnd(const frame::Frame& input, StreamEndMessage* out, std::string* error) {
    if (out == nullptr) {
        return Fail(error, "null stream end output");
    }
    if (input.type != static_cast<std::uint8_t>(FrameType::kStreamEnd)) {
        return Fail(error, "frame is not stream end");
    }
    Reader reader(input.payload.data(), input.payload.size());
    std::uint64_t call_id = 0;
    std::uint8_t raw_status = 0;
    std::string message;
    if (!reader.Varint(call_id)) {
        return Fail(error, "stream end: bad call id");
    }
    if (!reader.U8(raw_status) || !ValidStatusCode(raw_status)) {
        return Fail(error, "stream end: bad status code");
    }
    if (!reader.String(message, kMaxStatusMessageBytes)) {
        return Fail(error, "stream end: bad status message");
    }
    if (!reader.AtEnd()) {
        return Fail(error, "stream end: trailing bytes");
    }
    out->call_id = call_id;
    out->status.code = static_cast<StatusCode>(raw_status);
    out->status.message = std::move(message);
    return true;
}

bool DecodeCallId(const frame::Frame& input, std::uint64_t* call_id, std::string* error) {
    if (call_id == nullptr) {
        return Fail(error, "null call id output");
    }
    Reader reader(input.payload.data(), input.payload.size());
    std::uint64_t value = 0;
    if (!reader.Varint(value)) {
        return Fail(error, "frame: bad call id");
    }
    if (!reader.AtEnd()) {
        return Fail(error, "frame: trailing bytes after call id");
    }
    *call_id = value;
    return true;
}

}  // namespace go2cpp::rpc
