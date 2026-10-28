#pragma once

/**
 * @file reflect.hpp
 * @brief 轻量类型反射：`TypeTraits<T>` 描述字段类型，供 CSV/配置绑定复用。
 *
 * @details 这里刻意不引入 RTTI/宏生成的结构体描述表：字段列表由使用方在
 *          `FieldList<T>` 里显式声明（见 bind.hpp），反射只负责“类型 -> 名字/
 *          大小/是否数值/字段种类/转换器”。这样编译期开销小，也不要求业务
 *          结构体侵入式注册。
 *
 *          目前覆盖 `int`/`double`/`bool`/`std::string` 四种标量；未特化的类型
 *          得到 `"unknown"` 且 `Caster = void`，`Bind` 会在编译期报错而不是
 *          静默跳过。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "go2cpp/bento/reflect/caster.hpp"

namespace go2cpp::bento::reflect {

/// @brief 字段类型身份；转换交给 caster，反射本身不做校验。
enum class FieldKind : std::uint8_t {
    kString = 0, ///< 文本字段。
    kInt = 1,    ///< 整数。
    kDouble = 2, ///< 浮点数。
    kBool = 3,   ///< 布尔。
};

/// @brief 类型的反射描述。
struct TypeDescriptor {
    const char* name;   ///< 稳定类型名（字符串字面量）。
    std::size_t size;   ///< `sizeof(T)`。
    bool is_numeric;    ///< 是否数值类型（int/double 为 true）。
    FieldKind kind;     ///< 字段种类。
};

/**
 * @brief 类型转换器描述：把 `From` 转成 `To`，实际逻辑在 `RealCaster`。
 * @tparam From 输入类型（本模块统一为 `std::string_view`）。
 * @tparam To 输出类型。
 * @tparam RealCaster 带 `bool operator()(const From&, To&) const` 的函子。
 */
template <typename From, typename To, typename RealCaster>
struct TypeCaster {
    using FromType = From;
    using ToType = To;
    using Caster = RealCaster;

    static bool Cast(const From& from, To& to) { return RealCaster{}(from, to); }
};

/// @brief 通用类型特征；未特化类型会得到 unknown + void Caster。
template <typename T>
struct TypeTraits {
    static constexpr const char* kName = "unknown";
    static constexpr bool kNumeric = false;
    static constexpr FieldKind kKind = FieldKind::kString;
    using Caster = void;
};

/// @brief `int` 的反射特征。
template <>
struct TypeTraits<int> {
    static constexpr const char* kName = "int";
    static constexpr bool kNumeric = true;
    static constexpr FieldKind kKind = FieldKind::kInt;
    using Caster = TypeCaster<std::string_view, int, caster::StringToInt>;
};

/// @brief `double` 的反射特征。
template <>
struct TypeTraits<double> {
    static constexpr const char* kName = "double";
    static constexpr bool kNumeric = true;
    static constexpr FieldKind kKind = FieldKind::kDouble;
    using Caster = TypeCaster<std::string_view, double, caster::StringToDouble>;
};

/// @brief `bool` 的反射特征。
template <>
struct TypeTraits<bool> {
    static constexpr const char* kName = "bool";
    static constexpr bool kNumeric = false;
    static constexpr FieldKind kKind = FieldKind::kBool;
    using Caster = TypeCaster<std::string_view, bool, caster::StringToBool>;
};

/// @brief `std::string` 的反射特征。
template <>
struct TypeTraits<std::string> {
    static constexpr const char* kName = "string";
    static constexpr bool kNumeric = false;
    static constexpr FieldKind kKind = FieldKind::kString;
    using Caster = TypeCaster<std::string_view, std::string, caster::StringToString>;
};

/// @brief 返回类型描述；`const T` / `T&` 会先归一化到去 cv/引用的类型。
template <typename T>
constexpr TypeDescriptor Describe() noexcept {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    return TypeDescriptor{TypeTraits<Value>::kName, sizeof(Value), TypeTraits<Value>::kNumeric,
                          TypeTraits<Value>::kKind};
}

/// @brief 类型名；`const T` / `T&` 归一化处理。
template <typename T>
constexpr const char* NameOf() noexcept {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    return TypeTraits<Value>::kName;
}

/// @brief `sizeof(T)`；`const T` / `T&` 归一化处理。
template <typename T>
constexpr std::size_t SizeOf() noexcept {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    return sizeof(Value);
}

} // namespace go2cpp::bento::reflect
