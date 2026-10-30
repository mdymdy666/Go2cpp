#pragma once

/**
 * @file field.hpp
 * @brief 字段反射：字段描述表（名称/类型/偏移）与按名称查找。
 *
 * @details 字段表由业务结构体显式特化 `FieldList<T>` 提供，宏
 *          `GO2CPP_REFLECT_FIELD` 负责生成「名称 + TypeId + 偏移 + 类型擦除
 *          编解码函数」。这样做的取舍：
 *
 *          - **不要求侵入式继承/注册**：业务结构体保持 POD 风格，字段表是
 *            独立的编译期特化；
 *          - **偏移在运行期计算**：宏展开处创建一个临时 `Class` 对象，用
 *            `&(probe.*member) - &probe` 计算偏移。相比 `offsetof` 不要求
 *            standard-layout，也不会触发 `-Winvalid-offsetof`；代价是
 *            `Fields()` 首次调用时需要 `Class` 可默认构造（网络消息类型
 *            通常满足）。
 *          - **编解码函数指针按字段生成**：泛型 Serializer 无法从运行期
 *            TypeId 反推 C++ 类型，宏为每个字段生成无捕获 lambda，既支持
 *            嵌套结构体，也避免运行期注册表。
 *
 *          宏展开处需要能看到 `Serialize` / `Deserialize` / `WriteJson` /
 *          `ReadJson` 的模板定义（`go2cpp/reflect/reflect.hpp` 已按顺序包含）。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

#include "go2cpp/reflect/buffer.hpp"
#include "go2cpp/reflect/type.hpp"

namespace go2cpp::reflect {

class JsonWriter;
struct JsonValue;

// 字段编解码回调通过这四个模板把工作委托给具体类型；定义在
// serializer_impl.hpp / json_codec.hpp，这里只前置声明，避免头文件循环。
template <typename T>
bool Serialize(const T& value, Buffer& out);
template <typename T>
bool Deserialize(Buffer& in, T& value);
template <typename T>
bool WriteJson(const T& value, JsonWriter& out);
template <typename T>
bool ReadJson(const JsonValue& json, T& value);

/**
 * @brief 单字段的四类操作。
 *
 * @details 全部是无捕获函数指针：字段表可以放在只读静态存储里，不需要
 *          `std::function` 的堆分配，也不会因为捕获产生生命周期问题。
 */
struct FieldOps {
    /// 二进制编码：`object` 指向结构体起始地址。
    bool (*encode)(const void* object, Buffer& out);
    /// 二进制解码：写入 `object` 指向的结构体。
    bool (*decode)(Buffer& in, void* object);
    /// 文本编码（JSON）。
    bool (*write_text)(const void* object, JsonWriter& out);
    /// 文本解码（JSON）。
    bool (*read_text)(const JsonValue& json, void* object);
};

/**
 * @brief 字段描述。
 */
struct FieldDescriptor {
    const char* name;    ///< 字段名（字符串字面量，进程生命周期）。
    TypeId type_id;      ///< 字段类型的 TypeId。
    std::size_t offset;  ///< 相对结构体起始地址的字节偏移。
    std::size_t size;    ///< `sizeof(字段类型)`。
    FieldOps ops;        ///< 类型擦除的编解码回调。
};

/**
 * @brief 计算成员在结构体中的字节偏移。
 * @param member 成员指针（宏传 `&Class::member`）。
 * @return 偏移字节数。
 *
 * @note 要求 `Class` 可默认构造：这里创建一个临时探针对象，在其上取地址。
 *       临时对象的构造/析构只发生在字段表首次初始化时（静态存储初始化），
 *       不进入热路径。
 */
template <typename Class, typename Member>
std::size_t FieldOffset(Member Class::*member) {
    static_assert(std::is_default_constructible_v<Class>,
                  "GO2CPP_REFLECT_FIELD requires a default-constructible Class");
    Class probe{};
    const auto* base = reinterpret_cast<const unsigned char*>(&probe);
    const auto* field = reinterpret_cast<const unsigned char*>(&(probe.*member));
    return static_cast<std::size_t>(field - base);
}

/// @brief 检测 `FieldList<T>` 是否提供字段表。
template <typename T, typename = void>
struct HasFieldList : std::false_type {};

template <typename T>
struct HasFieldList<T, std::void_t<decltype(FieldList<T>::Fields(std::declval<std::size_t&>()))>>
    : std::true_type {};

/// @brief 类型是否有字段反射。
template <typename T>
constexpr bool HasFields() noexcept {
    return HasFieldList<std::remove_cv_t<std::remove_reference_t<T>>>::value;
}

/// @brief 字段数量；无字段表的类型返回 0。
template <typename T>
std::size_t FieldCount() {
    if constexpr (!HasFields<T>()) {
        return 0;
    } else {
        std::size_t count = 0;
        (void)FieldList<std::remove_cv_t<std::remove_reference_t<T>>>::Fields(count);
        return count;
    }
}

/**
 * @brief 按名称查找字段描述。
 * @param name 字段名。
 * @param index 非空时写入字段在表中的下标。
 * @return 找到返回描述指针，否则 nullptr。
 */
template <typename T>
const FieldDescriptor* FindField(std::string_view name, std::size_t* index = nullptr) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (!HasFields<Value>()) {
        (void)name;
        (void)index;
        return nullptr;
    } else {
        std::size_t count = 0;
        const FieldDescriptor* fields = FieldList<Value>::Fields(count);
        for (std::size_t position = 0; position < count; ++position) {
            if (name == fields[position].name) {
                if (index != nullptr) {
                    *index = position;
                }
                return &fields[position];
            }
        }
        return nullptr;
    }
}

}  // namespace go2cpp::reflect

/**
 * @brief 声明一个反射字段。
 *
 * 用法：
 * @code
 * template <>
 * struct FieldList<LoginRequest> {
 *     static constexpr TypeId kId = 100;
 *     static constexpr const char* kName = "LoginRequest";
 *     static const FieldDescriptor* Fields(std::size_t& count) {
 *         static const FieldDescriptor fields[] = {
 *             GO2CPP_REFLECT_FIELD(LoginRequest, user),
 *             GO2CPP_REFLECT_FIELD(LoginRequest, token),
 *         };
 *         count = sizeof(fields) / sizeof(fields[0]);
 *         return fields;
 *     }
 * };
 * @endcode
 *
 * @note `Fields()` 返回的数组必须是静态存储：`FindField` 与编解码都会返回/保存
 *       其中的指针，返回局部数组会悬空。
 */
#define GO2CPP_REFLECT_FIELD(Class, member)                                                        \
    ::go2cpp::reflect::FieldDescriptor {                                                           \
        #member, ::go2cpp::reflect::TypeIdOf<decltype(Class::member)>(),                           \
            ::go2cpp::reflect::FieldOffset(&Class::member), sizeof(decltype(Class::member)),       \
            ::go2cpp::reflect::FieldOps {                                                          \
            [](const void* object, ::go2cpp::reflect::Buffer& out) -> bool {                       \
                return ::go2cpp::reflect::Serialize(static_cast<const Class*>(object)->member,     \
                                                    out);                                          \
            },                                                                                     \
                [](::go2cpp::reflect::Buffer& in, void* object) -> bool {                          \
                    return ::go2cpp::reflect::Deserialize(in,                                      \
                                                          static_cast<Class*>(object)->member);    \
                },                                                                                 \
                [](const void* object, ::go2cpp::reflect::JsonWriter& out) -> bool {               \
                    return ::go2cpp::reflect::WriteJson(static_cast<const Class*>(object)->member, \
                                                        out);                                      \
                },                                                                                 \
                [](const ::go2cpp::reflect::JsonValue& json, void* object) -> bool {               \
                    return ::go2cpp::reflect::ReadJson(json, static_cast<Class*>(object)->member); \
                }                                                                                  \
        }                                                                                          \
    }
