#pragma once

/**
 * @file serializer.hpp
 * @brief 序列化钩子 `Serializer<T>` 与通用入口的声明。
 *
 * @details 用户可以为自定义类型显式特化 `Serializer<T>`：
 * @code
 * template <>
 * struct Serializer<Opaque> {
 *     static bool Serialize(const Opaque& value, Buffer& out);
 *     static bool Deserialize(Buffer& in, Opaque& value);
 * };
 * @endcode
 *
 * 没有特化的类型若提供了 `FieldList<T>` 字段表，会自动走字段反射序列化；
 * 两者都没有时会触发 `static_assert`，而不是静默编码成空。
 *
 * 通用入口 `Serialize` / `Deserialize` 负责：
 * - 归一化 cv/引用类型；
 * - 失败时把 Buffer 标记为失败，调用方可以在整条链末尾统一检查；
 * - 解码使用临时对象，失败不修改调用方的目标对象（强异常/错误保证）。
 *
 * 定义在 `serializer_impl.hpp`（需要字段描述完整定义），聚合头
 * `go2cpp/reflect/reflect.hpp` 已按顺序包含。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/reflect/buffer.hpp"

namespace go2cpp::reflect {

/// @brief 通用二进制序列化入口（定义见 serializer_impl.hpp）。
template <typename T>
bool Serialize(const T& value, Buffer& out);

/// @brief 通用二进制反序列化入口（定义见 serializer_impl.hpp）。
template <typename T>
bool Deserialize(Buffer& in, T& value);

/**
 * @brief 类型序列化器；可特化。
 * @tparam T 被序列化类型。
 * @tparam Enable 供偏特化使用的第二参数（内置类型/字段反射类型）。
 */
template <typename T, typename Enable = void>
struct Serializer;

}  // namespace go2cpp::reflect
