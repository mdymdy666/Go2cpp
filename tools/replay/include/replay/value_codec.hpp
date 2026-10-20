#pragma once

/**
 * @file value_codec.hpp
 * @brief `SyncChannel<T>` 的消息值序列化钩子。
 *
 * ## 设计
 *
 * 值在记录格式里有两个槽位：
 *   - `Event::value`（i64）：整型消息直接放这里，v1 兼容；
 *   - `Event::data`（bytes）：字符串/字节/结构体的序列化结果。
 *
 * `ValueCodec<T>` 决定用哪个槽位，以及如何双向转换。默认规则：
 *   1. 整型/枚举 → `kI64`，`value` 承载，`data` 为空；
 *   2. `std::string` / `std::vector<std::uint8_t>` → `kBytes`；
 *   3. 其它类型 → 要求提供 **`Serialize()` 成员**（返回 `std::string`）与
 *      **静态 `Deserialize(const std::string&)`**，否则编译期报错；
 *   4. 也可以用 `SyncChannel<T, MyCodec>` 显式传入自定义 codec。
 *
 * `ValueCodec<T>::ToInt()` 对 bytes 值返回其 FNV-1a 64 摘要，因此旧工具/
 * `message_checksum` 不需要知道值类型也能给出稳定的顺序指纹。
 */

#include "replay/format.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp::replay {

namespace detail {

template <typename T, typename = void>
struct HasSerialize : std::false_type {};

template <typename T>
struct HasSerialize<T, std::void_t<decltype(std::declval<const T&>().Serialize())>>
    : std::true_type {};

template <typename T, typename = void>
struct HasDeserialize : std::false_type {};

template <typename T>
struct HasDeserialize<
    T, std::void_t<decltype(T::Deserialize(std::declval<const std::string&>()))>>
    : std::true_type {};

/// 把一段 bytes 编码成 i64 摘要（FNV-1a 64）。
inline std::int64_t BytesDigest(const std::string& bytes) noexcept {
    return static_cast<std::int64_t>(
        Fnv1a64(bytes.data(), bytes.size(), 1469598103934665603ULL));
}

}  // namespace detail

/**
 * @brief 默认 codec：整型/枚举 → i64；有 Serialize/Deserialize 的类型 → bytes。
 *
 * 不满足条件的类型会在实例化时报 `static_assert`，提示用户特化
 * `ValueCodec<T>` 或提供序列化成员。
 */
template <typename T, typename Enable = void>
struct ValueCodec {
    static constexpr ValueKind kind = ValueKind::kBytes;
    static constexpr bool kSupported =
        detail::HasSerialize<T>::value && detail::HasDeserialize<T>::value;
    static_assert(kSupported,
                  "ValueCodec<T>: provide T::Serialize()/T::Deserialize() or "
                  "specialize go2cpp::replay::ValueCodec<T>");

    static std::int64_t ToInt(const T& value) {
        return detail::BytesDigest(value.Serialize());
    }
    static std::string ToBytes(const T& value) { return value.Serialize(); }
    static T FromValue(std::int64_t, const std::string& bytes) {
        return T::Deserialize(bytes);
    }
};

/// 整型特化：复用 v1 的 i64 槽位。
template <typename T>
struct ValueCodec<T, std::enable_if_t<std::is_integral<T>::value>> {
    static constexpr ValueKind kind = ValueKind::kI64;
    static std::int64_t ToInt(const T& value) {
        return static_cast<std::int64_t>(value);
    }
    static std::string ToBytes(const T&) { return {}; }
    static T FromValue(std::int64_t value, const std::string&) {
        return static_cast<T>(value);
    }
};

/// 枚举特化：按底层整型记录。
template <typename T>
struct ValueCodec<T, std::enable_if_t<std::is_enum<T>::value>> {
    using Underlying = std::underlying_type_t<T>;
    static constexpr ValueKind kind = ValueKind::kI64;
    static std::int64_t ToInt(const T& value) {
        return static_cast<std::int64_t>(static_cast<Underlying>(value));
    }
    static std::string ToBytes(const T&) { return {}; }
    static T FromValue(std::int64_t value, const std::string&) {
        return static_cast<T>(static_cast<Underlying>(value));
    }
};

/// `std::string` 特化：UTF-8 字节原样入 `data`。
template <>
struct ValueCodec<std::string, void> {
    static constexpr ValueKind kind = ValueKind::kBytes;
    static std::int64_t ToInt(const std::string& value) {
        return detail::BytesDigest(value);
    }
    static std::string ToBytes(const std::string& value) { return value; }
    static std::string FromValue(std::int64_t, const std::string& bytes) {
        return bytes;
    }
};

/// `std::vector<std::uint8_t>` 特化：二进制原样入 `data`。
template <>
struct ValueCodec<std::vector<std::uint8_t>, void> {
    static constexpr ValueKind kind = ValueKind::kBytes;
    static std::int64_t ToInt(const std::vector<std::uint8_t>& value) {
        return static_cast<std::int64_t>(
            Fnv1a64(value.data(), value.size(), 1469598103934665603ULL));
    }
    static std::string ToBytes(const std::vector<std::uint8_t>& value) {
        return std::string(reinterpret_cast<const char*>(value.data()), value.size());
    }
    static std::vector<std::uint8_t> FromValue(std::int64_t,
                                               const std::string& bytes) {
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }
};

}  // namespace go2cpp::replay
