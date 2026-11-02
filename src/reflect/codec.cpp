/**
 * @file codec.cpp
 * @brief Reflect 消息外层编解码实现。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/reflect/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "go2cpp/reflect/compress.hpp"

namespace go2cpp::reflect {
namespace {

/// @brief 二进制消息魔数：ASCII "G2RF"（Go2Cpp Reflect Frame）。
constexpr std::uint8_t kBinaryMagic[4] = {'G', '2', 'R', 'F'};

/// @brief 二进制消息格式版本。
constexpr std::uint8_t kBinaryVersion = 1;

/// @brief flags 位定义：载荷已 RLE 压缩 / 载荷为紧凑格式（无字段表）。
constexpr std::uint8_t kFlagCompressed = kFlagCompressedPayload;
constexpr std::uint8_t kFlagCompact = kFlagCompactPayload;

/// @brief 文本消息协议版本。
constexpr std::int64_t kTextVersion = 1;

/// @brief 与 `Buffer::GetVarint` 相同规则的指针版 varint 读取（零拷贝解码用）。
bool ReadVarintRaw(const std::uint8_t*& cursor, const std::uint8_t* end,
                   std::uint64_t& value) noexcept {
    std::uint64_t result = 0;
    unsigned shift = 0;
    for (std::size_t index = 0; index < 10; ++index) {
        if (cursor >= end) {
            return false;
        }
        const std::uint8_t byte = *cursor++;
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

CodecStatus DecodeBinary(const std::uint8_t* data, std::size_t size, Envelope& out,
                         std::size_t* consumed) {
    // 零拷贝：旧实现先把整条消息复制进 Buffer、再把 payload 复制进 raw、
    // 最后复制进 out.payload（60 字节消息 = 3 次分配 + 3 次 memcpy）。
    // 现在直接在原指针上解析，只有 payload 落一次。
    const std::uint8_t* cursor = data;
    const std::uint8_t* const end = data + size;
    if (size < sizeof(kBinaryMagic)) {
        return CodecStatus::kTruncated;
    }
    if (std::memcmp(cursor, kBinaryMagic, sizeof(kBinaryMagic)) != 0) {
        return CodecStatus::kBadMagic;
    }
    cursor += sizeof(kBinaryMagic);
    if (cursor >= end) {
        return CodecStatus::kTruncated;
    }
    const std::uint8_t version = *cursor++;
    if (version != kBinaryVersion) {
        return CodecStatus::kBadVersion;
    }
    if (cursor >= end) {
        return CodecStatus::kTruncated;
    }
    const std::uint8_t flags = *cursor++;
    if ((flags & ~(kFlagCompressed | kFlagCompact)) != 0) {
        return CodecStatus::kBadVersion;
    }
    std::uint64_t type_id = 0;
    if (!ReadVarintRaw(cursor, end, type_id)) {
        return CodecStatus::kTruncated;
    }
    if (type_id > std::numeric_limits<TypeId>::max()) {
        return CodecStatus::kBadPayload;
    }
    std::uint64_t name_length = 0;
    if (!ReadVarintRaw(cursor, end, name_length)) {
        return CodecStatus::kTruncated;
    }
    if (name_length > kMaxStringBytes ||
        name_length > static_cast<std::uint64_t>(end - cursor)) {
        return CodecStatus::kTruncated;
    }
    if (name_length > kMaxTypeNameBytes) {
        return CodecStatus::kTooLarge;
    }
    out.type_name.assign(reinterpret_cast<const char*>(cursor),
                         static_cast<std::size_t>(name_length));
    cursor += name_length;
    std::uint64_t payload_length = 0;
    if (!ReadVarintRaw(cursor, end, payload_length)) {
        return CodecStatus::kTruncated;
    }
    if (payload_length > kMaxBufferBytes) {
        return CodecStatus::kTooLarge;
    }
    if (payload_length > static_cast<std::uint64_t>(end - cursor)) {
        return CodecStatus::kTruncated;
    }
    out.payload.Clear();
    const bool compressed = (flags & kFlagCompressed) != 0;
    if (compressed) {
        if (!RleDecompress(cursor, static_cast<std::size_t>(payload_length),
                           out.payload.data(), kMaxBufferBytes)) {
            return CodecStatus::kCompressionError;
        }
    } else if (!out.payload.PutBytes(cursor, static_cast<std::size_t>(payload_length))) {
        return CodecStatus::kTooLarge;
    }
    out.encoding = Encoding::kBinary;
    out.compressed = compressed;
    out.compact = (flags & kFlagCompact) != 0;
    out.type_id = static_cast<TypeId>(type_id);
    if (consumed != nullptr) {
        *consumed = static_cast<std::size_t>(cursor - data) +
                    static_cast<std::size_t>(payload_length);
    }
    return CodecStatus::kOk;
}

CodecStatus DecodeText(const std::uint8_t* data, std::size_t size, Envelope& out,
                       std::size_t* consumed) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    JsonValue root;
    std::size_t used = 0;
    if (!JsonParser::Parse(text, root, &used)) {
        return CodecStatus::kBadText;
    }
    if (root.kind != JsonValue::Kind::kObject) {
        return CodecStatus::kBadText;
    }
    const JsonValue* version = root.Find("v");
    std::int64_t version_value = 0;
    if (version == nullptr || !JsonNumberToInt64(*version, version_value)) {
        return CodecStatus::kBadText;
    }
    if (version_value != kTextVersion) {
        return CodecStatus::kBadVersion;
    }
    const JsonValue* id = root.Find("id");
    std::uint64_t id_value = 0;
    if (id == nullptr || !JsonNumberToUint64(*id, id_value)) {
        return CodecStatus::kBadText;
    }
    if (id_value > std::numeric_limits<TypeId>::max()) {
        return CodecStatus::kBadPayload;
    }
    const JsonValue* type = root.Find("type");
    if (type == nullptr || type->kind != JsonValue::Kind::kString) {
        return CodecStatus::kBadText;
    }
    if (type->text.size() > kMaxTypeNameBytes) {
        return CodecStatus::kTooLarge;
    }
    const JsonValue* payload = root.Find("payload");
    if (payload == nullptr || payload->kind != JsonValue::Kind::kObject) {
        return CodecStatus::kBadText;
    }
    std::string payload_text;
    JsonWriter writer(payload_text);
    if (!WriteJsonValue(*payload, writer) || !writer.ok()) {
        return CodecStatus::kBadText;
    }
    out.payload.Clear();
    if (!out.payload.PutBytes(payload_text.data(), payload_text.size())) {
        return CodecStatus::kTooLarge;
    }
    out.encoding = Encoding::kText;
    out.compressed = false;
    out.type_id = static_cast<TypeId>(id_value);
    out.type_name = type->text;
    if (consumed != nullptr) {
        *consumed = used;
    }
    return CodecStatus::kOk;
}

}  // namespace

const char* CodecStatusName(CodecStatus status) noexcept {
    switch (status) {
        case CodecStatus::kOk:
            return "ok";
        case CodecStatus::kInvalidArgument:
            return "invalid_argument";
        case CodecStatus::kTruncated:
            return "truncated";
        case CodecStatus::kBadMagic:
            return "bad_magic";
        case CodecStatus::kBadVersion:
            return "bad_version";
        case CodecStatus::kUnknownType:
            return "unknown_type";
        case CodecStatus::kTypeMismatch:
            return "type_mismatch";
        case CodecStatus::kUnknownField:
            return "unknown_field";
        case CodecStatus::kBadPayload:
            return "bad_payload";
        case CodecStatus::kBadText:
            return "bad_text";
        case CodecStatus::kUnsupportedEncoding:
            return "unsupported_encoding";
        case CodecStatus::kCompressionError:
            return "compression_error";
        case CodecStatus::kTooLarge:
            return "too_large";
        case CodecStatus::kFrameError:
            return "frame_error";
    }
    return "unknown";
}

namespace detail {

CodecStatus EncodeEnvelope(TypeId type_id, std::string_view type_name, Encoding encoding,
                           Compression compression, const Buffer& payload,
                           std::vector<std::uint8_t>& out, bool compact) {
    out.clear();
    if (type_name.size() > kMaxTypeNameBytes) {
        return CodecStatus::kTooLarge;
    }
    if (payload.size() > kMaxBufferBytes) {
        return CodecStatus::kTooLarge;
    }
    if (encoding == Encoding::kBinary) {
        std::vector<std::uint8_t> compressed;
        std::uint8_t flags = compact ? kFlagCompact : 0;
        const std::uint8_t* payload_data = payload.data().data();
        std::size_t payload_size = payload.size();
        if (compression == Compression::kRle && payload_size > 0) {
            if (!RleCompress(payload_data, payload_size, compressed)) {
                return CodecStatus::kTooLarge;
            }
            if (compressed.size() < payload_size) {
                flags |= kFlagCompressed;
                payload_data = compressed.data();
                payload_size = compressed.size();
            }
        }
        // 直接写 out，省掉中间 Buffer 的一次分配 + 整体搬移。
        try {
            out.clear();
            out.reserve(sizeof(kBinaryMagic) + 2U + 20U + type_name.size() + payload_size);
        } catch (const std::bad_alloc&) {
            return CodecStatus::kTooLarge;
        }
        const auto append_varint = [&out](std::uint64_t value) {
            while (value >= 0x80u) {
                out.push_back(static_cast<std::uint8_t>(value & 0x7fu) | 0x80u);
                value >>= 7;
            }
            out.push_back(static_cast<std::uint8_t>(value));
        };
        out.insert(out.end(), kBinaryMagic, kBinaryMagic + sizeof(kBinaryMagic));
        out.push_back(kBinaryVersion);
        out.push_back(flags);
        append_varint(type_id);
        append_varint(type_name.size());
        out.insert(out.end(), type_name.begin(), type_name.end());
        append_varint(payload_size);
        out.insert(out.end(), payload_data, payload_data + payload_size);
        if (out.size() > kMaxBufferBytes) {
            return CodecStatus::kTooLarge;
        }
        return CodecStatus::kOk;
    }

    // 文本模式是调试格式：压缩不生效（RLE 后的字节不再是 JSON），忽略
    // compression；换行/缩进也不做，保证一条消息就是一行。
    std::string text;
    text.reserve(payload.size() + type_name.size() + 64);
    text += "{\"v\":";
    text += std::to_string(kTextVersion);
    text += ",\"id\":";
    text += std::to_string(type_id);
    text += ",\"type\":\"";
    text += EscapeJsonString(type_name);
    text += "\",\"payload\":";
    if (payload.empty()) {
        text += "null";
    } else {
        text.append(reinterpret_cast<const char*>(payload.data().data()), payload.size());
    }
    text += '}';
    if (text.size() > kMaxBufferBytes) {
        return CodecStatus::kTooLarge;
    }
    out.assign(reinterpret_cast<const std::uint8_t*>(text.data()),
               reinterpret_cast<const std::uint8_t*>(text.data()) + text.size());
    return CodecStatus::kOk;
}

CodecStatus DecodeEnvelope(const std::uint8_t* data, std::size_t size, Envelope& out,
                           std::size_t* consumed) {
    if (data == nullptr) {
        return CodecStatus::kInvalidArgument;
    }
    out = Envelope{};
    if (size == 0) {
        return CodecStatus::kTruncated;
    }
    if (size >= sizeof(kBinaryMagic) &&
        std::memcmp(data, kBinaryMagic, sizeof(kBinaryMagic)) == 0) {
        return DecodeBinary(data, size, out, consumed);
    }
    std::size_t start = 0;
    while (start < size) {
        const char value = static_cast<char>(data[start]);
        if (value != ' ' && value != '\t' && value != '\n' && value != '\r') {
            break;
        }
        ++start;
    }
    if (start < size && data[start] == static_cast<std::uint8_t>('{')) {
        std::size_t text_consumed = 0;
        const CodecStatus status = DecodeText(data + start, size - start, out, &text_consumed);
        if (status == CodecStatus::kOk && consumed != nullptr) {
            *consumed = start + text_consumed;
        }
        return status;
    }
    return CodecStatus::kBadMagic;
}

}  // namespace detail

CodecStatus ReflectCodec::PeekType(const std::uint8_t* data, std::size_t size, TypeId& type_id,
                                   std::string& type_name) {
    Envelope envelope;
    const CodecStatus status = detail::DecodeEnvelope(data, size, envelope, nullptr);
    if (status != CodecStatus::kOk) {
        return status;
    }
    type_id = envelope.type_id;
    type_name = std::move(envelope.type_name);
    return CodecStatus::kOk;
}

}  // namespace go2cpp::reflect
