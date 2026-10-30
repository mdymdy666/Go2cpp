#pragma once

/**
 * @file type.hpp
 * @brief 运行期类型身份（TypeId）、编译期类型特征（TypeTraits）与类型注册表。
 *
 * @details 设计参照 `csvp::reflect`（金山训练营 CSV 解析项目）的
 *          「内置类型固定 id + 自定义类型动态注册」模型，但为网络序列化场景
 *          做了两点调整：
 *
 *          1. **内置 id 只增不改**：新增内置类型只能追加在 9 之后，且第 16 号
 *             起留给动态类型。线上二进制消息会写 `TypeId`，重排常量会让旧数据
 *             被解码成错误类型。
 *          2. **名字与 id 同时上线**：服务端和客户端各自独立注册动态类型，分配到的
 *             id 可能不同；因此 Reflect 消息同时携带类型名，解码端按名字匹配，
 *             id 只做快速校验。这样「注册顺序不同」不会让协议失配。
 *
 *          `TypeTraits<T>` 是编译期身份：内置标量通过偏特化映射到固定 id，
 *          自定义类型可以显式特化它，也可以在 `FieldList<T>` 里写
 *          `kId` / `kName`（`TypeIdOf<T>()` 会回退到后者）。
 *
 * @note `FieldList` 的主模板定义在这里而不是 field.hpp，是为了让
 *       `TypeIdOf<T>()` 能在 field.hpp 之前安全地做 SFINAE 检测。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace go2cpp::reflect {

/// @brief 运行期类型身份；0 永久保留给「未知/未注册」。
using TypeId = std::uint32_t;

/// @brief 未知类型 id。
inline constexpr TypeId kUnknownType = 0;
/// @brief bool。
inline constexpr TypeId kBoolType = 1;
/// @brief 有符号 32 位整数（含 int/short/char 等 <= 4 字节有符号整型）。
inline constexpr TypeId kInt32Type = 2;
/// @brief 有符号 64 位整数。
inline constexpr TypeId kInt64Type = 3;
/// @brief 无符号 32 位整数。
inline constexpr TypeId kUint32Type = 4;
/// @brief 无符号 64 位整数。
inline constexpr TypeId kUint64Type = 5;
/// @brief IEEE-754 双精度。
inline constexpr TypeId kDoubleType = 6;
/// @brief UTF-8 字符串。
inline constexpr TypeId kStringType = 7;
/// @brief 字节数组（std::vector<std::uint8_t>）。
inline constexpr TypeId kBytesType = 8;

/// @brief 动态类型 id 的起点；1..15 预留给未来内置类型。
inline constexpr TypeId kFirstDynamicType = 16;

/// @brief 类型名最大字节数（线上字段，限制解码端单次分配）。
inline constexpr std::size_t kMaxTypeNameBytes = 64;

/// @brief 未命名类型的稳定名字。
inline constexpr const char* kUnknownTypeName = "unknown";

/**
 * @brief 配置结构体的字段表主模板。
 *
 * @details 用户对需要反射序列化的结构体做显式特化：
 * @code
 * template <>
 * struct FieldList<LoginRequest> {
 *     static constexpr TypeId kId = 100;
 *     static constexpr const char* kName = "LoginRequest";
 *     static const FieldDescriptor* Fields(std::size_t& count);
 * };
 * @endcode
 *
 * `kId` / `kName` 可选：不写时 `TypeIdOf<T>()` 返回 `kUnknownType`，消息仍可
 * 按纯结构编码，但解码端无法做类型路由。
 */
template <typename T>
struct FieldList {};

/// @brief 检测 `FieldList<T>` 是否提供 `kId` / `kName` 元信息。
template <typename T, typename = void>
struct ReflectMeta {
    static constexpr bool kHas = false;                     ///< 是否有元信息。
    static constexpr TypeId kId = kUnknownType;             ///< 类型 id。
    static constexpr const char* kName = kUnknownTypeName;  ///< 类型名。
};

template <typename T>
struct ReflectMeta<T, std::void_t<decltype(FieldList<T>::kId), decltype(FieldList<T>::kName)>> {
    static constexpr bool kHas = true;
    static constexpr TypeId kId = FieldList<T>::kId;
    static constexpr const char* kName = FieldList<T>::kName;
};

/**
 * @brief 编译期类型特征：内置类型给出固定 TypeId 与稳定名字。
 *
 * @details 整型用偏特化按「符号 + 字节数」归类：1/2/4 字节有符号映射到
 *          `kInt32Type`，8 字节映射到 `kInt64Type`；无符号同理。序列化时按
 *          实际 C++ 类型选择 zigzag/普通 varint，因此 `int` 与 `long` 共用
 *          同一个 TypeId 也不会混淆取值范围——解码端会按目标类型做范围检查。
 */
template <typename T, typename Enable = void>
struct TypeTraits {
    static constexpr TypeId kId = kUnknownType;             ///< 类型 id。
    static constexpr const char* kName = kUnknownTypeName;  ///< 稳定类型名。
    static constexpr bool kBuiltin = false;                 ///< 是否内置类型。
};

/// @brief bool 的特征。
template <>
struct TypeTraits<bool> {
    static constexpr TypeId kId = kBoolType;
    static constexpr const char* kName = "bool";
    static constexpr bool kBuiltin = true;
};

/// @brief 有符号整型（除 bool）：<= 4 字节归 int32，8 字节归 int64。
template <typename T>
struct TypeTraits<
    T, std::enable_if_t<std::is_integral_v<T> && std::is_signed_v<T> && !std::is_same_v<T, bool>>> {
    static constexpr TypeId kId = sizeof(T) <= 4 ? kInt32Type : kInt64Type;
    static constexpr const char* kName = sizeof(T) <= 4 ? "int32" : "int64";
    static constexpr bool kBuiltin = true;
};

/// @brief 无符号整型：<= 4 字节归 uint32，8 字节归 uint64。
template <typename T>
struct TypeTraits<T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T> &&
                                      !std::is_same_v<T, bool>>> {
    static constexpr TypeId kId = sizeof(T) <= 4 ? kUint32Type : kUint64Type;
    static constexpr const char* kName = sizeof(T) <= 4 ? "uint32" : "uint64";
    static constexpr bool kBuiltin = true;
};

/// @brief double 的特征。
template <>
struct TypeTraits<double> {
    static constexpr TypeId kId = kDoubleType;
    static constexpr const char* kName = "double";
    static constexpr bool kBuiltin = true;
};

/// @brief std::string 的特征。
template <>
struct TypeTraits<std::string> {
    static constexpr TypeId kId = kStringType;
    static constexpr const char* kName = "string";
    static constexpr bool kBuiltin = true;
};

/// @brief std::vector<std::uint8_t> 的特征（任意字节串，不要求 UTF-8）。
template <>
struct TypeTraits<std::vector<std::uint8_t>> {
    static constexpr TypeId kId = kBytesType;
    static constexpr const char* kName = "bytes";
    static constexpr bool kBuiltin = true;
};

/**
 * @brief 编译期类型 -> 运行期 TypeId。
 *
 * @details 查找顺序：`TypeTraits<T>` 的固定 id → `FieldList<T>` 的 `kId` →
 *          `kUnknownType`。`const T` / `T&` 会先归一化。
 */
template <typename T>
constexpr TypeId TypeIdOf() noexcept {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (TypeTraits<Value>::kId != kUnknownType) {
        return TypeTraits<Value>::kId;
    } else if constexpr (ReflectMeta<Value>::kHas) {
        return ReflectMeta<Value>::kId;
    } else {
        return kUnknownType;
    }
}

/// @brief 编译期类型 -> 稳定类型名；未声明的类型返回 "unknown"。
template <typename T>
constexpr const char* TypeNameOf() noexcept {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (TypeTraits<Value>::kId != kUnknownType) {
        return TypeTraits<Value>::kName;
    } else if constexpr (ReflectMeta<Value>::kHas) {
        return ReflectMeta<Value>::kName;
    } else {
        return kUnknownTypeName;
    }
}

/// @brief 是否是内置类型（有固定 TypeId 且由本模块提供序列化）。
template <typename T>
constexpr bool IsBuiltinType() noexcept {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    return TypeTraits<Value>::kBuiltin;
}

/// @brief 类型描述（名字拷贝，避免调用方持有注册表内部视图）。
struct TypeInfo {
    std::string name;      ///< 稳定类型名。
    std::size_t size = 0;  ///< `sizeof(T)`。
    bool builtin = false;  ///< 是否内置类型。
};

/**
 * @brief 全局类型注册表：名字 ↔ id，内置 count 固定，自定义类型动态分配。
 *
 * @details 线程安全：所有公开操作共享一把互斥锁。注册是幂等的（同名返回首次
 *          分配的 id），因此多个模块各自 `Register` 同一个名字不会产生分歧。
 *
 *          `Name` 返回的 `string_view` 指向表中节点；`unordered_map` 的 rehash
 *          不移动节点，且本类不提供删除操作，所以视图在进程生命周期内有效。
 *
 * @note 动态 id 只在**本进程内**稳定；跨进程传输必须同时带上类型名。
 */
class TypeRegistry {
   public:
    /// @brief 全局唯一实例（首次调用时注册 8 个内置类型）。
    static TypeRegistry& Instance();

    /// @brief 注册（或查询）类型；返回类型 id，失败返回 `kUnknownType`。
    /// @param name 类型名，不能为空且不超过 `kMaxTypeNameBytes`。
    /// @param size 类型大小，必须非 0。
    TypeId Register(std::string_view name, std::size_t size);

    /// @brief 按协议约定的固定 id 注册类型（服务端/客户端共享同一张 id 表时用）。
    /// @param id 约定 id，必须 >= `kFirstDynamicType`。
    /// @param name 类型名，不能为空且不超过 `kMaxTypeNameBytes`。
    /// @param size 类型大小，必须非 0。
    /// @return 成功返回 `id`；同名已注册返回已有 id；id 已被其他名字占用返回
    ///         `kUnknownType`。
    /// @note 同名不同 id 属于调用方配置错误：本方法不会覆盖已有映射，返回的是
    ///       首次注册的 id，调用方应以返回值为准。
    TypeId RegisterWithId(TypeId id, std::string_view name, std::size_t size);

    /// @brief 按名字查 id；未注册返回 false。
    bool Find(std::string_view name, TypeId& out) const;

    /// @brief 按 id 查名字；未注册返回 false。
    bool Name(TypeId id, std::string_view& out) const;

    /// @brief 按 id 查完整描述；未注册返回 false。
    bool Describe(TypeId id, TypeInfo& out) const;

    /// @brief id 是否已注册（`kUnknownType` 永远返回 false）。
    bool Contains(TypeId id) const;

    /// @brief 已注册类型数量（含内置类型）。
    std::size_t Size() const;

    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;
    TypeRegistry(TypeRegistry&&) = delete;
    TypeRegistry& operator=(TypeRegistry&&) = delete;

   private:
    TypeRegistry();

    struct Entry {
        std::string name;      ///< 类型名。
        std::size_t size = 0;  ///< 类型大小。
        bool builtin = false;  ///< 是否内置。
    };

    void AddBuiltin(TypeId id, const char* name, std::size_t size);

    mutable std::mutex m_mutex;                         ///< 保护下面两张表。
    std::unordered_map<std::string, TypeId> m_by_name;  ///< 名字 -> id。
    std::unordered_map<TypeId, Entry> m_by_id;          ///< id -> 描述。
    TypeId m_next = kFirstDynamicType;                  ///< 下一个动态 id。
};

/// @brief 按类型注册并返回运行期 id；失败返回 `kUnknownType`。
template <typename T>
TypeId RegisterType(std::string_view name) {
    return TypeRegistry::Instance().Register(name, sizeof(T));
}

}  // namespace go2cpp::reflect
