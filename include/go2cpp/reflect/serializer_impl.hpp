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

template <typename T>
bool SerializeReflect(const T& value, Buffer& out) {
    std::size_t count = 0;
    const FieldDescriptor* fields = FieldList<T>::Fields(count);
    if (count > kMaxReflectFields) {
        return false;
    }
    if (!out.PutVarint(static_cast<std::uint64_t>(count))) {
        return false;
    }
    // 字段表：每个字段的名字与 TypeId 只出现一次；随后按声明顺序写值。
    // 这样单条消息里字段名不重复，跨字段更不会重复（名字表就是去重表）。
    for (std::size_t index = 0; index < count; ++index) {
        if (!out.PutString(fields[index].name) || !out.PutVarint(fields[index].type_id)) {
            return false;
        }
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
    std::vector<const FieldDescriptor*> wire;
    try {
        wire.reserve(static_cast<std::size_t>(count));
    } catch (const std::bad_alloc&) {
        return CodecStatus::kTooLarge;
    }
    // 先完整读取字段表并逐项匹配本地字段，再读值：这样未知字段/类型不符
    // 能在写入任何成员之前拒绝，调用方的目标对象保持原样。
    for (std::uint64_t index = 0; index < count; ++index) {
        std::string name;
        std::uint64_t type_id = 0;
        if (!in.GetString(name) || !in.GetVarint(type_id)) {
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
        wire.push_back(local);
    }
    for (const FieldDescriptor* field : wire) {
        if (!field->ops.decode(in, &value)) {
            return CodecStatus::kBadPayload;
        }
    }
    return CodecStatus::kOk;
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

}  // namespace go2cpp::reflect
