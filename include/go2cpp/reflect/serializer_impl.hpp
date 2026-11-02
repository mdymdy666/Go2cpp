#pragma once

/**
 * @file serializer_impl.hpp
 * @brief `Serializer<T>` 与通用序列化入口的模板实现（二进制 + 文本）。
 *
 * @details 本文件依赖字段描述（field.hpp）与 JSON（json.hpp），因此聚合头
 *          `reflect.hpp` 必须在 field.hpp 之后包含它。宏 `GO2CPP_REFLECT_FIELD`
 *          生成的 lambda 会实例化这里的 `Serialize` / `Deserialize` /
 *          `WriteJson` / `ReadJson`。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "go2cpp/reflect/field.hpp"
#include "go2cpp/reflect/json.hpp"
#include "go2cpp/reflect/serializer.hpp"
#include "go2cpp/reflect/status.hpp"

namespace go2cpp::reflect {

/// @brief 单个结构体允许的最大字段数（防止恶意字段表耗尽内存）。
inline constexpr std::size_t kMaxReflectFields = 4096;

namespace detail {

template <typename T>
inline constexpr bool kAlwaysFalse = false;

/// @brief 以 8 字节小端写入 double（与主机字节序无关）。
inline bool PutDouble(Buffer& out, double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(double) == sizeof(bits), "double must be 64-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    std::uint8_t bytes[8] = {};
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>((bits >> (8 * index)) & 0xffu);
    }
    return out.PutBytes(bytes, sizeof(bytes));
}

/// @brief 读取 8 字节小端 double。
inline bool GetDouble(Buffer& in, double& value) {
    std::uint8_t bytes[8] = {};
    if (!in.GetBytes(bytes, sizeof(bytes))) {
        return false;
    }
    std::uint64_t bits = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        bits |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
    }
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

/// @brief 写入字节数组：varint 长度 + 原始字节。
inline bool PutBlob(Buffer& out, const std::vector<std::uint8_t>& value) {
    if (value.size() > kMaxBlobBytes) {
        out.Fail();
        return false;
    }
    if (!out.PutVarint(static_cast<std::uint64_t>(value.size()))) {
        return false;
    }
    return out.PutBytes(value.data(), value.size());
}

/// @brief 读取字节数组；长度在分配前校验。
inline bool GetBlob(Buffer& in, std::vector<std::uint8_t>& value) {
    std::uint64_t length = 0;
    if (!in.GetVarint(length)) {
        return false;
    }
    if (length > kMaxBlobBytes || length > in.Remaining()) {
        in.Fail();
        return false;
    }
    try {
        value.resize(static_cast<std::size_t>(length));
    } catch (const std::bad_alloc&) {
        in.Fail();
        return false;
    }
    return in.GetBytes(value.data(), value.size());
}

/// @brief 字段表（count + 每字段 name/type_id）的线上字节，按类型缓存一次。
/// @details 字段表是编译期常量，旧实现每条消息都重新写一遍字符串与 varint；
///          对 60 B 的小消息，这部分约占序列化时间的三分之一。
template <typename T>
const std::vector<std::uint8_t>& WireFieldTable() {
    static const std::vector<std::uint8_t> table = [] {
        std::size_t count = 0;
        const FieldDescriptor* fields = FieldList<T>::Fields(count);
        Buffer buffer;
        if (!buffer.PutVarint(static_cast<std::uint64_t>(count))) {
            return std::vector<std::uint8_t>{};
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (!buffer.PutString(fields[index].name) ||
                !buffer.PutVarint(fields[index].type_id)) {
                return std::vector<std::uint8_t>{};
            }
        }
        return std::move(buffer.data());
    }();
    return table;
}

template <typename T>
bool SerializeReflect(const T& value, Buffer& out) {
    std::size_t count = 0;
    const FieldDescriptor* fields = FieldList<T>::Fields(count);
    if (count > kMaxReflectFields) {
        return false;
    }
    // 字段表：每个字段的名字与 TypeId 只出现一次；随后按声明顺序写值。
    // 这样单条消息里字段名不重复，跨字段更不会重复（名字表就是去重表）。
    const std::vector<std::uint8_t>& table = WireFieldTable<T>();
    if (table.empty() || !out.PutBytes(table.data(), table.size())) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!fields[index].ops.encode(&value, out)) {
            return false;
        }
    }
    return out.ok();
}

template <typename T>
CodecStatus DeserializeReflect(Buffer& in, T& value) {
    std::uint64_t count = 0;
    if (!in.GetVarint(count)) {
        return CodecStatus::kBadPayload;
    }
    if (count > kMaxReflectFields) {
        return CodecStatus::kTooLarge;
    }
    // 字段表很小（典型 4~10 个），用栈上数组保存匹配结果，避免每条消息
    // 一次 vector 分配；只有异常大的结构体才退回堆。
    std::array<const FieldDescriptor*, 32> inline_wire{};
    std::vector<const FieldDescriptor*> heap_wire;
    const FieldDescriptor** wire = inline_wire.data();
    if (count > inline_wire.size()) {
        try {
            heap_wire.resize(static_cast<std::size_t>(count));
        } catch (const std::bad_alloc&) {
            return CodecStatus::kTooLarge;
        }
        wire = heap_wire.data();
    }
    // 先完整读取字段表并逐项匹配本地字段，再读值：这样未知字段/类型不符
    // 能在写入任何成员之前拒绝，调用方的目标对象保持原样。
    for (std::uint64_t index = 0; index < count; ++index) {
        std::string_view name;
        std::uint64_t type_id = 0;
        if (!in.GetStringView(name) || !in.GetVarint(type_id)) {
            return CodecStatus::kBadPayload;
        }
        if (type_id > std::numeric_limits<TypeId>::max()) {
            return CodecStatus::kBadPayload;
        }
        const FieldDescriptor* local = FindField<T>(name);
        if (local == nullptr) {
            return CodecStatus::kUnknownField;
        }
        if (local->type_id != static_cast<TypeId>(type_id)) {
            return CodecStatus::kTypeMismatch;
        }
        wire[index] = local;
    }
    for (std::uint64_t index = 0; index < count; ++index) {
        if (!wire[index]->ops.decode(in, &value)) {
            return CodecStatus::kBadPayload;
        }
    }
    return CodecStatus::kOk;
}

/// @brief 指针版 varint 读取（紧凑格式零拷贝解码用，规则与 Buffer 一致）。
inline bool ReadVarintPtr(const std::uint8_t*& cursor, const std::uint8_t* end,
                          std::uint64_t& value) noexcept {
    std::uint64_t result = 0;
    unsigned shift = 0;
    for (std::size_t index = 0; index < kMaxVarintBytes; ++index) {
        if (cursor >= end) {
            return false;
        }
        const std::uint8_t byte = *cursor++;
        if (index == kMaxVarintBytes - 1 && byte > 1u) {
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

/// @brief 紧凑格式的内置类型解码：直接写指针游标，不经过 Buffer。
template <typename T>
bool DecodeCompactBuiltin(const std::uint8_t*& cursor, const std::uint8_t* end, T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        if (cursor >= end) {
            return false;
        }
        const std::uint8_t byte = *cursor++;
        if (byte > 1u) {
            return false;
        }
        value = byte != 0;
        return true;
    } else if constexpr (std::is_integral_v<T>) {
        std::uint64_t raw = 0;
        if (!ReadVarintPtr(cursor, end, raw)) {
            return false;
        }
        if constexpr (std::is_signed_v<T>) {
            const std::int64_t decoded = static_cast<std::int64_t>(raw >> 1) ^
                                         -static_cast<std::int64_t>(raw & 1u);
            if (decoded < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
                decoded > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
                return false;
            }
            value = static_cast<T>(decoded);
        } else {
            if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
                return false;
            }
            value = static_cast<T>(raw);
        }
        return true;
    } else if constexpr (std::is_same_v<T, double>) {
        if (end - cursor < 8) {
            return false;
        }
        std::uint64_t bits = 0;
        for (std::size_t index = 0; index < 8; ++index) {
            bits |= static_cast<std::uint64_t>(cursor[index]) << (8 * index);
        }
        cursor += 8;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    } else if constexpr (std::is_same_v<T, std::string>) {
        std::uint64_t length = 0;
        if (!ReadVarintPtr(cursor, end, length)) {
            return false;
        }
        if (length > kMaxStringBytes ||
            length > static_cast<std::uint64_t>(end - cursor)) {
            return false;
        }
        try {
            value.assign(reinterpret_cast<const char*>(cursor), static_cast<std::size_t>(length));
        } catch (const std::bad_alloc&) {
            return false;
        }
        cursor += static_cast<std::size_t>(length);
        return true;
    } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
        std::uint64_t length = 0;
        if (!ReadVarintPtr(cursor, end, length)) {
            return false;
        }
        if (length > kMaxBlobBytes || length > static_cast<std::uint64_t>(end - cursor)) {
            return false;
        }
        try {
            value.resize(static_cast<std::size_t>(length));
        } catch (const std::bad_alloc&) {
            return false;
        }
        if (length != 0) {
            std::memcpy(value.data(), cursor, static_cast<std::size_t>(length));
        }
        cursor += static_cast<std::size_t>(length);
        return true;
    } else {
        static_assert(kAlwaysFalse<T>, "unsupported builtin type for compact codec");
    }
}

/// @brief 紧凑格式前缀（count + 全部字段号）的线上字节，按类型缓存一次。
/// @details 字段号序列是编译期常量；缓存后编码只需一次 `PutBytes`，
///          不必为每个字段调用一次 `PutVarint`。
template <typename T>
const std::vector<std::uint8_t>& CompactFieldPrefix() {
    static const std::vector<std::uint8_t> prefix = [] {
        std::size_t count = 0;
        (void)FieldList<T>::Fields(count);
        Buffer buffer;
        if (!buffer.PutVarint(static_cast<std::uint64_t>(count))) {
            return std::vector<std::uint8_t>{};
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (!buffer.PutVarint(static_cast<std::uint64_t>(index))) {
                return std::vector<std::uint8_t>{};
            }
        }
        return std::move(buffer.data());
    }();
    return prefix;
}

/// @brief 紧凑格式：只写字段号 + 值；字段名/类型由本地 schema 还原。
/// @details 载荷布局为「varint 字段数 + 全部字段号 + 全部值」。字段号必须
///          严格递增；解码端按字段号直接索引本地字段表，不做字符串比较，
///          因此报文的 schema 必须与发送端一致。
template <typename T>
bool SerializeReflectCompact(const T& value, Buffer& out) {
    std::size_t count = 0;
    const FieldDescriptor* fields = FieldList<T>::Fields(count);
    if (count > kMaxReflectFields) {
        return false;
    }
    const std::vector<std::uint8_t>& prefix = CompactFieldPrefix<T>();
    if (prefix.empty()) {
        return false;
    }
    // 紧凑前缀只有 1~2 字节/字段；若不预留，vector 会从 5 字节开始反复扩容
    // （5→10→20→40…），单这一项就比自描述格式多 20 ns。按每字段 16 字节
    // 预留一个保守上界，一次性分配到位。
    out.Reserve(prefix.size() + count * 16 + 16);
    if (!out.PutBytes(prefix.data(), prefix.size())) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!fields[index].ops.encode_compact(&value, out)) {
            return false;
        }
    }
    return out.ok();
}

/// @brief 紧凑格式解码：按字段号直接写成员，全程零拷贝（字符串除外，需分配）。
template <typename T>
bool DeserializeCompactReflect(const std::uint8_t*& cursor, const std::uint8_t* end, T& value) {
    std::uint64_t count = 0;
    if (!ReadVarintPtr(cursor, end, count)) {
        return false;
    }
    std::size_t local_count = 0;
    const FieldDescriptor* fields = FieldList<T>::Fields(local_count);
    // 线上字段数多于本地：发送端 schema 与本地不一致，直接拒绝。
    if (count > kMaxReflectFields || count > local_count) {
        return false;
    }
    std::array<std::uint64_t, 32> inline_tags{};
    std::vector<std::uint64_t> heap_tags;
    std::uint64_t* tags = inline_tags.data();
    if (count > inline_tags.size()) {
        try {
            heap_tags.resize(static_cast<std::size_t>(count));
        } catch (const std::bad_alloc&) {
            return false;
        }
        tags = heap_tags.data();
    }
    std::uint64_t last = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        std::uint64_t tag = 0;
        if (!ReadVarintPtr(cursor, end, tag)) {
            return false;
        }
        if (tag >= local_count || (index > 0 && tag <= last)) {
            return false;
        }
        last = tag;
        tags[index] = tag;
    }
    for (std::uint64_t index = 0; index < count; ++index) {
        if (!fields[tags[index]].ops.decode_compact(cursor, end, &value)) {
            return false;
        }
    }
    return true;
}

template <typename T>
bool WriteJsonReflect(const T& value, JsonWriter& out) {
    std::size_t count = 0;
    const FieldDescriptor* fields = FieldList<T>::Fields(count);
    out.BeginObject();
    for (std::size_t index = 0; index < count; ++index) {
        out.Key(fields[index].name);
        if (!fields[index].ops.write_text(&value, out)) {
            return false;
        }
    }
    out.EndObject();
    return out.ok();
}

template <typename T>
bool ReadJsonReflect(const JsonValue& json, T& value) {
    if (json.kind != JsonValue::Kind::kObject) {
        return false;
    }
    for (const auto& member : json.members) {
        if (FindField<T>(member.first) == nullptr) {
            return false;
        }
    }
    std::size_t count = 0;
    const FieldDescriptor* fields = FieldList<T>::Fields(count);
    for (std::size_t index = 0; index < count; ++index) {
        const JsonValue* member = json.Find(fields[index].name);
        if (member == nullptr) {
            // 文本模式允许缺字段：保留结构体默认值，便于新旧版本互看日志。
            continue;
        }
        if (!fields[index].ops.read_text(*member, &value)) {
            return false;
        }
    }
    return true;
}

template <typename T>
bool WriteJsonBuiltin(const T& value, JsonWriter& out) {
    if constexpr (std::is_same_v<T, bool>) {
        out.Bool(value);
    } else if constexpr (std::is_integral_v<T>) {
        if constexpr (std::is_signed_v<T>) {
            out.Int(static_cast<std::int64_t>(value));
        } else {
            out.Uint(static_cast<std::uint64_t>(value));
        }
    } else if constexpr (std::is_same_v<T, double>) {
        out.Double(value);
    } else if constexpr (std::is_same_v<T, std::string>) {
        out.String(value);
    } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
        out.BeginArray();
        for (const std::uint8_t byte : value) {
            out.Uint(byte);
        }
        out.EndArray();
    } else {
        static_assert(kAlwaysFalse<T>, "unsupported builtin type for JSON");
    }
    return out.ok();
}

template <typename T>
bool ReadJsonBuiltin(const JsonValue& json, T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        if (json.kind != JsonValue::Kind::kBool) {
            return false;
        }
        value = json.boolean;
        return true;
    } else if constexpr (std::is_integral_v<T>) {
        if (json.kind != JsonValue::Kind::kNumber) {
            return false;
        }
        if constexpr (std::is_signed_v<T>) {
            std::int64_t raw = 0;
            if (!JsonNumberToInt64(json, raw)) {
                return false;
            }
            if (raw < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
                raw > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
                return false;
            }
            value = static_cast<T>(raw);
            return true;
        } else {
            std::uint64_t raw = 0;
            if (!JsonNumberToUint64(json, raw)) {
                return false;
            }
            if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
                return false;
            }
            value = static_cast<T>(raw);
            return true;
        }
    } else if constexpr (std::is_same_v<T, double>) {
        if (json.kind != JsonValue::Kind::kNumber) {
            return false;
        }
        double raw = 0.0;
        if (!JsonNumberToDouble(json, raw)) {
            return false;
        }
        value = raw;
        return true;
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (json.kind != JsonValue::Kind::kString) {
            return false;
        }
        value = json.text;
        return true;
    } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
        if (json.kind != JsonValue::Kind::kArray) {
            return false;
        }
        if (json.elements.size() > kMaxBlobBytes) {
            return false;
        }
        std::vector<std::uint8_t> bytes;
        bytes.reserve(json.elements.size());
        for (const auto& element : json.elements) {
            if (element.kind != JsonValue::Kind::kNumber) {
                return false;
            }
            std::uint64_t byte = 0;
            if (!JsonNumberToUint64(element, byte) || byte > 0xffu) {
                return false;
            }
            bytes.push_back(static_cast<std::uint8_t>(byte));
        }
        value = std::move(bytes);
        return true;
    } else {
        static_assert(kAlwaysFalse<T>, "unsupported builtin type for JSON");
    }
}

}  // namespace detail

/// @brief 类型是否支持 JSON 文本模式（内置类型或字段反射类型）。
template <typename T>
constexpr bool IsJsonWritable() noexcept {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    return HasFieldList<Value>::value || TypeTraits<Value>::kBuiltin;
}

// ---------------------------------------------------------------------------
// 公开钩子
// ---------------------------------------------------------------------------

/// @brief 字段反射类型的默认序列化器（二进制）。
template <typename T, typename Enable>
struct Serializer {
    static bool Serialize(const T& value, Buffer& out) {
        static_assert(HasFieldList<T>::value,
                      "go2cpp::reflect: no Serializer<T> specialization and no FieldList<T>");
        if constexpr (HasFieldList<T>::value) {
            return detail::SerializeReflect(value, out);
        } else {
            return false;
        }
    }

    static bool Deserialize(Buffer& in, T& value) {
        static_assert(HasFieldList<T>::value,
                      "go2cpp::reflect: no Serializer<T> specialization and no FieldList<T>");
        if constexpr (HasFieldList<T>::value) {
            return detail::DeserializeReflect(in, value) == CodecStatus::kOk;
        } else {
            return false;
        }
    }
};

/// @brief 内置类型序列化器。
template <typename T>
struct Serializer<T, std::enable_if_t<TypeTraits<T>::kBuiltin>> {
    static bool Serialize(const T& value, Buffer& out) {
        if constexpr (std::is_same_v<T, bool>) {
            return out.PutU8(value ? 1u : 0u);
        } else if constexpr (std::is_integral_v<T>) {
            if constexpr (std::is_signed_v<T>) {
                return out.PutSignedVarint(static_cast<std::int64_t>(value));
            } else {
                return out.PutVarint(static_cast<std::uint64_t>(value));
            }
        } else if constexpr (std::is_same_v<T, double>) {
            return detail::PutDouble(out, value);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return out.PutString(value);
        } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
            return detail::PutBlob(out, value);
        } else {
            static_assert(detail::kAlwaysFalse<T>, "unsupported builtin type");
        }
    }

    static bool Deserialize(Buffer& in, T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            std::uint8_t byte = 0;
            if (!in.GetU8(byte) || byte > 1u) {
                in.Fail();
                return false;
            }
            value = byte != 0;
            return true;
        } else if constexpr (std::is_integral_v<T>) {
            if constexpr (std::is_signed_v<T>) {
                std::int64_t raw = 0;
                if (!in.GetSignedVarint(raw)) {
                    return false;
                }
                if (raw < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
                    raw > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
                    in.Fail();
                    return false;
                }
                value = static_cast<T>(raw);
                return true;
            } else {
                std::uint64_t raw = 0;
                if (!in.GetVarint(raw)) {
                    return false;
                }
                if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
                    in.Fail();
                    return false;
                }
                value = static_cast<T>(raw);
                return true;
            }
        } else if constexpr (std::is_same_v<T, double>) {
            return detail::GetDouble(in, value);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return in.GetString(value);
        } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
            return detail::GetBlob(in, value);
        } else {
            static_assert(detail::kAlwaysFalse<T>, "unsupported builtin type");
        }
    }
};

/// @brief 通用二进制序列化：失败时把 Buffer 标记为失败。
template <typename T>
bool Serialize(const T& value, Buffer& out) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if (!out.ok()) {
        return false;
    }
    const bool ok = Serializer<Value>::Serialize(value, out);
    if (!ok) {
        out.Fail();
    }
    return ok && out.ok();
}

/// @brief 通用二进制反序列化：先解到临时对象，成功后才覆盖 `value`。
template <typename T>
bool Deserialize(Buffer& in, T& value) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if (!in.ok()) {
        return false;
    }
    Value temp{};
    if (!Serializer<Value>::Deserialize(in, temp)) {
        in.Fail();
        return false;
    }
    value = std::move(temp);
    return true;
}

/// @brief JSON 文本序列化；不支持的类型返回 false。
template <typename T>
bool WriteJson(const T& value, JsonWriter& out) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (HasFieldList<Value>::value) {
        return detail::WriteJsonReflect(value, out);
    } else if constexpr (TypeTraits<Value>::kBuiltin) {
        return detail::WriteJsonBuiltin(value, out);
    } else {
        return false;
    }
}

/// @brief JSON 文本反序列化；未知字段返回 false，缺字段保留默认值。
template <typename T>
bool ReadJson(const JsonValue& json, T& value) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (HasFieldList<Value>::value) {
        return detail::ReadJsonReflect(json, value);
    } else if constexpr (TypeTraits<Value>::kBuiltin) {
        return detail::ReadJsonBuiltin(json, value);
    } else {
        return false;
    }
}

/// @brief 紧凑二进制序列化：字段名/类型不落盘，靠 schema 还原。
/// @note 与 `Serialize` 的自描述格式互不兼容；`DeserializeCompact` 只认紧凑格式。
template <typename T>
bool SerializeCompact(const T& value, Buffer& out) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (HasFieldList<Value>::value) {
        return detail::SerializeReflectCompact(value, out);
    } else if constexpr (TypeTraits<Value>::kBuiltin) {
        // 内置类型的 Buffer 失败位由 Put* 自己维护，无额外的 ok/Fail 包装。
        return Serializer<Value>::Serialize(value, out);
    } else {
        return false;
    }
}

/// @brief 紧凑二进制反序列化：成功后才覆盖 `value`（失败时保持原值）。
template <typename T>
bool DeserializeCompact(const std::uint8_t*& cursor, const std::uint8_t* end, T& value) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    Value temp{};
    bool ok = false;
    if constexpr (HasFieldList<Value>::value) {
        ok = detail::DeserializeCompactReflect(cursor, end, temp);
    } else if constexpr (TypeTraits<Value>::kBuiltin) {
        ok = detail::DecodeCompactBuiltin(cursor, end, temp);
    }
    if (!ok) {
        return false;
    }
    value = std::move(temp);
    return true;
}

}  // namespace go2cpp::reflect

#include "go2cpp/reflect/json_direct.hpp"
