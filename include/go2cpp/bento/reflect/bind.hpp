#pragma once

/**
 * @file bind.hpp
 * @brief 用 `TypeTraits` + `Caster` 把字符串 map 绑定到普通结构体。
 *
 * @details 使用方为结构体特化 `FieldList<T>` 并列出字段，`Bind` 负责查找、
 *          严格类型转换与“全有或全无”的提交：
 *
 * ```cpp
 * struct Config {
 *     std::string host = "127.0.0.1";
 *     int port = 8080;
 *     bool secure = false;
 * };
 *
 * namespace go2cpp::bento::reflect {
 * template <> struct FieldList<Config> {
 *     static std::vector<FieldBinder<Config>> Fields() {
 *         return {BENTO_BIND_FIELD(Config, host),
 *                 BENTO_BIND_FIELD(Config, port),
 *                 BENTO_BIND_FIELD(Config, secure)};
 *     }
 * };
 * } // namespace go2cpp::bento::reflect
 *
 * const std::map<std::string, std::string> values{{"port", "9090"}};
 * Config config;
 * if (!Bind(config, values)) { ... }  // 失败时 config 保持原值
 * ```
 *
 * ## 语义
 *
 * - 默认忽略未知键；`BindOptions::reject_unknown` 可改成失败。
 * - `BindOptions::require_all` 要求每个声明字段都出现在 map 里。
 * - 先在副本上应用全部字段，全部成功后才提交；任何转换失败都不会留下
 *   “改了一半”的对象（要求 T 可复制构造且可赋值）。
 * - 字段名区分大小写。
 * - `FieldList<T>::Fields()` 必须是纯函数且字段集合稳定：实现会在首次
 *   `Bind` 时缓存字段表，后续调用复用（避免每个字段一次 std::function
 *   分配）。依赖运行期状态的动态字段表不适合用本模块绑定。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "go2cpp/bento/reflect/reflect.hpp"

namespace go2cpp::bento::reflect {

/// @brief 绑定失败的类别。
enum class BindErrorKind : std::uint8_t {
    kNone = 0,       ///< 没有失败。
    kUnknownField,   ///< map 里出现未声明的字段（仅 reject_unknown 时）。
    kWrongType,      ///< 字段值无法按声明类型严格解析。
    kMissingField,   ///< require_all 下缺少声明字段。
};

/// @brief 绑定失败详情；由调用方传入指针接收。
struct BindError {
    BindErrorKind kind = BindErrorKind::kNone;
    std::string field;
    std::string value;
};

/// @brief 绑定行为开关。
struct BindOptions {
    bool reject_unknown = false; ///< true 时未声明字段导致失败。
    bool require_all = false;    ///< true 时缺少声明字段导致失败。
};

/// @brief 一个字段的绑定器：名字 + 把文本写入对象的类型擦除函数。
template <typename T>
struct FieldBinder {
    const char* name = nullptr;
    std::function<bool(T&, std::string_view)> apply;
};

/// @brief 字段列表特化点；未特化的类型无法 Bind（编译期报错）。
template <typename T>
struct FieldList;

namespace detail {

/// @brief 按 `TypeTraits<Field>::Caster` 解析文本并写入成员。
template <typename T, typename Field>
bool AssignField(T& object, Field T::*member, std::string_view text) {
    using Value = std::remove_cv_t<std::remove_reference_t<Field>>;
    static_assert(!std::is_const<Field>::value, "不能绑定 const 字段");
    using CasterType = typename TypeTraits<Value>::Caster;
    static_assert(!std::is_void<CasterType>::value, "字段类型缺少 TypeTraits/Caster 支持");
    typename CasterType::ToType parsed{};
    if (!CasterType::Cast(text, parsed)) {
        return false;
    }
    object.*member = std::move(parsed);
    return true;
}

/// @brief 按类型缓存字段表；`FieldList<T>::Fields()` 约定为纯函数。
/// @note 放在独立的函数模板里，保证同一 T 无论配哪种 Map 都只构造一次。
template <typename T>
const std::vector<FieldBinder<T>>& FieldsOf() {
    static const std::vector<FieldBinder<T>> fields = FieldList<T>::Fields();
    return fields;
}

} // namespace detail

/**
 * @brief 生成一个字段绑定器；通常经 `BENTO_BIND_FIELD` 使用。
 * @param name 字段名（宏传入 `#field`，生命周期为静态字符串）。
 * @param member 成员指针，字段类型从指针自动推导。
 */
template <typename T, typename Field>
FieldBinder<T> BindField(const char* name, Field T::*member) {
    return FieldBinder<T>{name, [member](T& object, std::string_view text) {
                              return detail::AssignField(object, member, text);
                          }};
}

// 绑定实现放在 detail，对外接口在下方统一转发；Map 只要求可遍历。
namespace detail {

/// @brief 绑定实现；Map 只需提供 begin/end，元素含 first（键）/second（值）。
template <typename T, typename Map>
bool BindMap(T& object, const Map& values, const BindOptions& options, BindError* error) {
    static_assert(std::is_copy_constructible<T>::value, "Bind 需要可复制构造的 T");
    static_assert(std::is_copy_assignable<T>::value, "Bind 需要可复制赋值的 T");
    if (error != nullptr) {
        *error = BindError{};
    }

    T candidate = object;
    // `Fields()` 约定为纯函数、字段集合稳定；按 T 缓存一次即可。每次 Bind
    // 重建 vector<FieldBinder> 会为每个字段分配一个 std::function，是配置
    // 绑定路径的主要分配来源（5 字段配置每次约 6 次分配）。
    const std::vector<FieldBinder<T>>& fields = FieldsOf<T>();
    std::vector<bool> seen(fields.size(), false);

    for (const auto& entry : values) {
        bool matched = false;
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const FieldBinder<T>& binder = fields[index];
            if (binder.name == nullptr || entry.first != binder.name) {
                continue;
            }
            matched = true;
            seen[index] = true;
            if (!binder.apply || !binder.apply(candidate, entry.second)) {
                if (error != nullptr) {
                    error->kind = BindErrorKind::kWrongType;
                    error->field = entry.first;
                    error->value = entry.second;
                }
                return false;
            }
            break;
        }
        if (!matched && options.reject_unknown) {
            if (error != nullptr) {
                error->kind = BindErrorKind::kUnknownField;
                error->field = entry.first;
                error->value = entry.second;
            }
            return false;
        }
    }

    if (options.require_all) {
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (seen[index]) {
                continue;
            }
            if (error != nullptr) {
                error->kind = BindErrorKind::kMissingField;
                error->field = fields[index].name != nullptr ? fields[index].name : "";
            }
            return false;
        }
    }

    object = std::move(candidate);
    return true;
}

} // namespace detail

/**
 * @brief 把字符串 map 绑定到结构体；成功后提交，失败时对象保持不变。
 * @param object 目标对象（按值复制出候选对象，成功才提交）。
 * @param values 字段名 -> 文本；`std::map`/`std::unordered_map` 等均可。
 * @param options 未知字段/缺失字段策略。
 * @param error 可选失败详情输出；成功时被重置为 kNone。
 * @return 全部转换成功返回 true。
 */
template <typename T, typename Map>
bool Bind(T& object, const Map& values, const BindOptions& options, BindError* error) {
    return detail::BindMap(object, values, options, error);
}

/// @brief 带策略、不带错误详情的重载。
template <typename T, typename Map>
bool Bind(T& object, const Map& values, const BindOptions& options) {
    return detail::BindMap(object, values, options, nullptr);
}

/// @brief 默认策略 + 失败详情输出。
template <typename T, typename Map>
bool Bind(T& object, const Map& values, BindError* error) {
    return detail::BindMap(object, values, BindOptions{}, error);
}

/// @brief 默认策略重载：忽略未知字段，不要求字段齐全。
template <typename T, typename Map>
bool Bind(T& object, const Map& values) {
    return detail::BindMap(object, values, BindOptions{}, nullptr);
}

/// @brief 构造新对象的便捷重载；失败返回 `std::nullopt`。
template <typename T, typename Map>
std::optional<T> Bind(const Map& values) {
    static_assert(std::is_default_constructible<T>::value, "此重载需要可默认构造的 T");
    T object{};
    if (!Bind(object, values)) {
        return std::nullopt;
    }
    return std::optional<T>(std::move(object));
}

} // namespace go2cpp::bento::reflect

/**
 * @brief 声明一个可绑定字段。
 * @param Type 结构体类型。
 * @param field 成员名（不带引号）。
 * @note 必须在 `FieldList<Type>::Fields()` 的初始化列表里使用。
 */
#define BENTO_BIND_FIELD(Type, field) \
    ::go2cpp::bento::reflect::BindField(#field, &Type::field)
