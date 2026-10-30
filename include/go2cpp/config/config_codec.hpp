#pragma once

/**
 * @file include/go2cpp/config/config_codec.hpp
 * @brief 配置文本编解码：统一委托 Reflect 模块（字段反射 + JSON 文本 + ReflectCodec）。
 *
 * ## 为什么需要这一层
 * 配置系统需要「一个 C++ 值 ↔ 一行 INI 文本」的编解码，同时满足三个约束：
 *
 * 1. 旧配置文件（`port=8080`、`name=demo`、`ratio=0.25`）必须继续可读；
 * 2. 结构体配置（`GO2CPP_REFLECT_FIELD` 描述）必须自动解析/序列化，且旧配置
 *    中缺席的新字段保留结构体默认值；
 * 3. 类型与范围校验不能依赖手写的流式解析：`"1x"` 不能被读成 1，`300` 不能
 *    塞进 `uint8_t`，未知字段必须显式拒绝。
 *
 * 因此本层把编解码全部交给 `go2cpp::reflect`：
 * - 标量：`reflect::WriteJson` 生成 JSON 标量文本，`reflect::JsonParser` +
 *   `reflect::ReadJson` 解码（整型 min/max 检查由 Reflect 的 `ReadJsonBuiltin`
 *   完成，`"1x"` 这类尾随字符会被 JSON 解析器直接拒绝）；
 * - 结构体：`ReflectCodec::Encode`（文本消息）→ `Peek` 取出 JSON 载荷作为
 *   INI 值；解码先按 `ReflectCodec::Decode` 接受完整消息，再回退到裸 JSON
 *   对象（便于手写配置，也兼容既有的 `{"threads":4}` 写法）；
 * - 数组：`JsonWriter` 数组 + 逐元素 Reflect 编码，同时接受旧逗号分隔格式。
 *
 * ## 文本格式（与旧格式的差异）
 *
 * | 类型 | 文本 | 说明 |
 * |------|------|------|
 * | int/double | `8080` / `0.25` | JSON 数字，与旧格式逐字符一致 |
 * | bool | `true` / `false` | 与旧格式一致；解码仍接受 1/0/yes/no/on/off |
 * | string | `demo` | 裸文本（INI 值本身就是字符串）；首字符为 `"` 时按 JSON 字符串转义 |
 * | vector<T> | `[1,2,3]` | 新格式；解码仍接受 `1,2,3` |
 * | log::Level | `warn` | 委托 log 模块，保持既有文本 |
 * | 结构体 | `{"threads":4}` | 字段名来自 `FieldDescriptor`，旧字段缺失时保留默认值 |
 *
 * ## 不变量
 * - 解码失败时**不修改**输出对象（先解到临时对象，成功才 move）；
 * - 未知字段、类型不符、整型越界都返回失败并给出中文字段级诊断；
 * - `ConfigEncode` 对不受支持的类型在**编译期**报错（`static_assert`），
 *   而不是像旧的流式 `ValueCodec` 那样在运行期输出空串。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/log.hpp"
#include "go2cpp/reflect/codec.hpp"
#include "go2cpp/reflect/field.hpp"
#include "go2cpp/reflect/json.hpp"
#include "go2cpp/reflect/serializer_impl.hpp"
#include "go2cpp/reflect/type.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp::config {

/// @brief 一个字段的变更描述（来自 `FieldDescriptor` 的名称/偏移/大小）。
struct ConfigFieldChange final {
    std::string name;        ///< 字段名（`GO2CPP_REFLECT_FIELD` 的成员名）。
    std::size_t offset = 0;  ///< 字段相对结构体起始地址的字节偏移。
    std::size_t size = 0;    ///< `sizeof(字段类型)`。
};

/**
 * @brief 把值编码为 INI 文本。
 * @param value 待编码值。
 * @param out 输出文本；失败时清空。
 * @param error 失败时写入中文诊断，可为空。
 * @return 成功返回 true。
 */
template <typename T>
bool ConfigEncode(const T& value, std::string* out, std::string* error = nullptr);

/**
 * @brief 从 INI 文本解码值；失败时不修改 `value`。
 * @param text INI 值文本（已由 `IniFile` 去掉注释/尾空白）。
 * @param value 输出对象；必须非空。
 * @param error 失败时写入中文诊断，可为空。
 * @return 成功返回 true。
 */
template <typename T>
bool ConfigDecode(std::string_view text, T* value, std::string* error = nullptr);

namespace detail {

/// @brief 依赖模板参数的错误分支标记（避免 static_assert 在无关实例化中触发）。
template <typename T>
inline constexpr bool kConfigCodecAlwaysFalse = false;

/// @brief 检测 `std::vector<T, Alloc>`。
template <typename T>
struct IsStdVector : std::false_type {};

template <typename T, typename Allocator>
struct IsStdVector<std::vector<T, Allocator>> : std::true_type {
    using value_type = T;
    using allocator_type = Allocator;
};

/// @brief 把 Reflect 状态码翻译成中文诊断。
std::string DescribeCodecStatus(reflect::CodecStatus status);

/// @brief 旧配置布尔词表（1/0/yes/no/on/off，大小写不敏感）。
bool DecodeLegacyBool(std::string_view text, bool* value);

/// @brief 旧配置浮点兼容解析（接受 `.5` 这类 JSON 不接受的写法）。
bool DecodeLegacyDouble(std::string_view text, double* value);

/// @brief 浮点编码：`to_chars` 最短往返表示；NaN/Inf 写成 `nan`/`inf`/`-inf`。
bool EncodeDouble(double value, std::string* out);

/// @brief 结构体字段表——按字段名与 `FieldDescriptor` 校验 JSON 对象。
template <typename T>
bool ValidateStructJson(const reflect::JsonValue& json, std::string* error) {
    std::size_t count = 0;
    const reflect::FieldDescriptor* fields = reflect::FieldList<T>::Fields(count);
    for (const auto& member : json.members) {
        if (reflect::FindField<T>(member.first) == nullptr) {
            if (error != nullptr) {
                *error = "未知配置字段: " + member.first;
            }
            return false;
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        const reflect::JsonValue* member = json.Find(fields[index].name);
        if (member == nullptr) {
            continue;  // 缺字段保留结构体默认值（新旧版本互看配置的关键）。
        }
        T probe{};
        if (!fields[index].ops.read_text(*member, &probe)) {
            if (error != nullptr) {
                *error = std::string("字段 ") + fields[index].name +
                         " 的类型或取值范围非法";
            }
            return false;
        }
    }
    return true;
}

/// @brief 结构体编码：ReflectCodec 文本消息 → 取 JSON 载荷作为 INI 值。
template <typename T>
bool EncodeStruct(const T& value, std::string* out, std::string* error) {
    const std::vector<std::uint8_t> wire =
        reflect::ReflectCodec::Encode<T>(value, reflect::Encoding::kText);
    if (wire.empty()) {
        if (error != nullptr) {
            *error = "结构体无法编码为 Reflect 文本消息（检查字段类型是否支持 JSON）";
        }
        return false;
    }
    reflect::Envelope envelope;
    const reflect::CodecStatus status =
        reflect::ReflectCodec::Peek(wire.data(), wire.size(), envelope);
    if (status != reflect::CodecStatus::kOk) {
        if (error != nullptr) {
            *error = "结构体文本消息解析失败: " + DescribeCodecStatus(status);
        }
        return false;
    }
    const auto& payload = envelope.payload.data();
    out->assign(payload.begin(), payload.end());
    return true;
}

/// @brief 结构体解码：先接受 Reflect 文本消息，再回退裸 JSON 对象。
template <typename T>
bool DecodeStruct(std::string_view text, T* value, std::string* error) {
    if (!text.empty() && text.front() == '{') {
        // 完整 Reflect 文本消息（带 v/id/type/payload）优先；失败再按裸 JSON
        // 解析。两步都不接受非对象输入，因此 INI 里写成标量会被明确拒绝。
        std::vector<std::uint8_t> wire(text.begin(), text.end());
        T message_value{};
        if (reflect::ReflectCodec::Decode<T>(wire, message_value) ==
            reflect::CodecStatus::kOk) {
            *value = std::move(message_value);
            return true;
        }
    }
    reflect::JsonValue json;
    std::string parse_error;
    if (!reflect::JsonParser::Parse(text, json, nullptr, &parse_error)) {
        if (error != nullptr) {
            *error = "不是合法的 JSON 对象（" + parse_error + "）";
        }
        return false;
    }
    if (json.kind != reflect::JsonValue::Kind::kObject) {
        if (error != nullptr) {
            *error = "结构体配置必须是 JSON 对象，例如 {\"threads\":4}";
        }
        return false;
    }
    if (!ValidateStructJson<T>(json, error)) {
        return false;
    }
    T parsed{};
    if (!reflect::ReadJson(json, parsed)) {
        if (error != nullptr) {
            *error = "结构体字段类型不匹配";
        }
        return false;
    }
    *value = std::move(parsed);
    return true;
}

/// @brief 数组编码：JSON 数组，元素走 Reflect 的 JSON 编码。
template <typename T>
bool EncodeVector(const T& value, std::string* out, std::string* error) {
    std::string text;
    reflect::JsonWriter writer(text);
    writer.BeginArray();
    for (const auto& item : value) {
        if (!reflect::WriteJson(item, writer)) {
            if (error != nullptr) {
                *error = "数组元素不支持 JSON 编码（只支持内置类型或带字段表的结构体）";
            }
            return false;
        }
    }
    writer.EndArray();
    if (!writer.ok()) {
        if (error != nullptr) {
            *error = "数组编码失败";
        }
        return false;
    }
    *out = std::move(text);
    return true;
}

/// @brief 数组解码：JSON 数组优先，兼容旧的逗号分隔写法。
template <typename T>
bool DecodeVector(std::string_view text, T* value, std::string* error) {
    using Element = typename IsStdVector<T>::value_type;
    T parsed;
    if (text.empty()) {
        *value = std::move(parsed);
        return true;
    }
    if (text.front() == '[') {
        reflect::JsonValue json;
        std::string parse_error;
        if (!reflect::JsonParser::Parse(text, json, nullptr, &parse_error) ||
            json.kind != reflect::JsonValue::Kind::kArray) {
            if (error != nullptr) {
                *error = "数组不是合法的 JSON 数组（" + parse_error + "）";
            }
            return false;
        }
        for (const auto& element : json.elements) {
            std::string element_text;
            reflect::JsonWriter writer(element_text);
            if (!reflect::WriteJsonValue(element, writer) || !writer.ok()) {
                if (error != nullptr) {
                    *error = "数组元素无法重新序列化";
                }
                return false;
            }
            Element item{};
            if (!ConfigDecode<Element>(element_text, &item, error)) {
                return false;
            }
            parsed.push_back(std::move(item));
        }
        *value = std::move(parsed);
        return true;
    }
    // 旧格式：`1,2,3`。空串表示空数组，与旧 ValueCodec 的行为一致。
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = text.find(',', start);
        const std::string_view piece =
            comma == std::string_view::npos ? text.substr(start)
                                            : text.substr(start, comma - start);
        Element item{};
        if (!ConfigDecode<Element>(piece, &item, error)) {
            return false;
        }
        parsed.push_back(std::move(item));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    *value = std::move(parsed);
    return true;
}

/// @brief 标量解码：JSON 标量文本 → Reflect 的 `ReadJson<T>`。
template <typename T>
bool DecodeJsonScalar(std::string_view text, T* value) {
    reflect::JsonValue json;
    if (!reflect::JsonParser::Parse(text, json)) {
        return false;
    }
    T parsed{};
    if (!reflect::ReadJson(json, parsed)) {
        return false;
    }
    *value = std::move(parsed);
    return true;
}

}  // namespace detail

/**
 * @brief 把值编码为 INI 文本（主模板按类型分派）。
 * @details 分派顺序与 Reflect 的能力一一对应：字段反射结构体 → 枚举 →
 *          内置标量 → 字符串 → 数组；其余类型在编译期拒绝。
 */
template <typename T>
bool ConfigEncode(const T& value, std::string* out, std::string* error) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "配置编码的输出指针为空";
        }
        return false;
    }
    out->clear();
    if constexpr (reflect::HasFields<Value>()) {
        return detail::EncodeStruct(value, out, error);
    } else if constexpr (std::is_same_v<Value, log::Level>) {
        *out = log::ToString(value);
        return true;
    } else if constexpr (std::is_enum_v<Value>) {
        // 普通枚举按底层整数编码；解码端按同一规则还原。
        using Underlying = std::underlying_type_t<Value>;
        return ConfigEncode<Underlying>(static_cast<Underlying>(value), out, error);
    } else if constexpr (std::is_same_v<Value, bool>) {
        *out = value ? "true" : "false";
        return true;
    } else if constexpr (std::is_integral_v<Value>) {
        std::string text;
        reflect::JsonWriter writer(text);
        if (!reflect::WriteJson(value, writer) || !writer.ok()) {
            if (error != nullptr) {
                *error = "整数编码失败";
            }
            return false;
        }
        *out = std::move(text);
        return true;
    } else if constexpr (std::is_floating_point_v<Value>) {
        return detail::EncodeDouble(static_cast<double>(value), out);
    } else if constexpr (std::is_same_v<Value, std::string>) {
        // INI 值本身就是字符串：裸文本最可读。值以 `"` 开头时必须走 JSON
        // 转义，否则 `"quoted"` 这类值读回会被误当成 JSON 字符串而丢引号。
        if (!value.empty() && value.front() == '"') {
            std::string text;
            reflect::JsonWriter writer(text);
            if (!reflect::WriteJson(value, writer) || !writer.ok()) {
                if (error != nullptr) {
                    *error = "字符串编码失败";
                }
                return false;
            }
            *out = std::move(text);
            return true;
        }
        *out = value;
        return true;
    } else if constexpr (detail::IsStdVector<Value>::value) {
        static_assert(
            std::is_same_v<typename detail::IsStdVector<Value>::allocator_type,
                           std::allocator<typename detail::IsStdVector<Value>::value_type>>,
            "配置数组只支持 std::vector<T> 的默认分配器");
        return detail::EncodeVector(value, out, error);
    } else {
        static_assert(detail::kConfigCodecAlwaysFalse<Value>,
                      "配置类型不支持文本编码：需要内置标量、std::vector<T>、"
                      "log::Level 或带 GO2CPP_REFLECT_FIELD 字段表的结构体");
    }
}

/**
 * @brief 从 INI 文本解码值（主模板按类型分派）。
 * @details 失败时 `*value` 保持不变；错误信息尽量指出具体字段与原因。
 */
template <typename T>
bool ConfigDecode(std::string_view text, T* value, std::string* error) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if (value == nullptr) {
        if (error != nullptr) {
            *error = "配置解码的输出指针为空";
        }
        return false;
    }
    if constexpr (reflect::HasFields<Value>()) {
        return detail::DecodeStruct(text, value, error);
    } else if constexpr (std::is_same_v<Value, log::Level>) {
        if (!log::ParseLevel(std::string(text), value)) {
            if (error != nullptr) {
                *error = "不是有效的日志级别";
            }
            return false;
        }
        return true;
    } else if constexpr (std::is_enum_v<Value>) {
        using Underlying = std::underlying_type_t<Value>;
        Underlying raw{};
        if (!ConfigDecode<Underlying>(text, &raw, error)) {
            return false;
        }
        *value = static_cast<Value>(raw);
        return true;
    } else if constexpr (std::is_same_v<Value, bool>) {
        if (detail::DecodeJsonScalar(text, value)) {
            return true;
        }
        // 旧配置兼容：1/0/yes/no/on/off。
        bool legacy = false;
        if (!detail::DecodeLegacyBool(text, &legacy)) {
            if (error != nullptr) {
                *error = "不是合法的布尔值（true/false/1/0/yes/no/on/off）";
            }
            return false;
        }
        *value = legacy;
        return true;
    } else if constexpr (std::is_integral_v<Value>) {
        if (!detail::DecodeJsonScalar(text, value)) {
            if (error != nullptr) {
                *error = "不是合法的整数或超出取值范围";
            }
            return false;
        }
        return true;
    } else if constexpr (std::is_floating_point_v<Value>) {
        double parsed = 0.0;
        if (detail::DecodeJsonScalar(text, &parsed)) {
            *value = static_cast<Value>(parsed);
            return true;
        }
        // 旧配置兼容：`.5`、`1e3` 等 JSON 不接受的写法由 from_chars 兜底。
        if (!detail::DecodeLegacyDouble(text, &parsed)) {
            if (error != nullptr) {
                *error = "不是合法的浮点数";
            }
            return false;
        }
        *value = static_cast<Value>(parsed);
        return true;
    } else if constexpr (std::is_same_v<Value, std::string>) {
        if (!text.empty() && text.front() == '"') {
            std::string parsed;
            if (!detail::DecodeJsonScalar(text, &parsed)) {
                if (error != nullptr) {
                    *error = "不是合法的 JSON 字符串";
                }
                return false;
            }
            *value = std::move(parsed);
            return true;
        }
        *value = std::string(text);
        return true;
    } else if constexpr (detail::IsStdVector<Value>::value) {
        return detail::DecodeVector(text, value, error);
    } else {
        static_assert(detail::kConfigCodecAlwaysFalse<Value>,
                      "配置类型不支持文本解码：需要内置标量、std::vector<T>、"
                      "log::Level 或带 GO2CPP_REFLECT_FIELD 字段表的结构体");
    }
}

/**
 * @brief 逐字段比较两个结构体，返回发生变化的字段描述。
 * @details 用字段的二进制编码结果做比较，因此**不要求结构体定义
 *          `operator==`，也不受 padding 内容影响**——这正是热加载判断
 *          「这一代配置到底改了哪几个字段」的依据。非结构体类型没有字段
 *          概念，返回空列表。
 * @param before 旧值。
 * @param after 新值。
 * @return 变化字段的 `ConfigFieldChange` 列表（按字段表顺序）。
 */
template <typename T>
std::vector<ConfigFieldChange> ConfigFieldDiff(const T& before, const T& after) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    std::vector<ConfigFieldChange> changes;
    if constexpr (!reflect::HasFields<Value>()) {
        (void)before;
        (void)after;
        return changes;
    } else {
        std::size_t count = 0;
        const reflect::FieldDescriptor* fields =
            reflect::FieldList<Value>::Fields(count);
        for (std::size_t index = 0; index < count; ++index) {
            reflect::Buffer left;
            reflect::Buffer right;
            const bool encoded =
                fields[index].ops.encode(&before, left) &&
                fields[index].ops.encode(&after, right);
            // 编码失败时保守地视为「已变化」：宁可多通知一次，也不要漏掉
            // 一个监听器依赖的字段。
            if (!encoded || left.data() != right.data()) {
                changes.push_back(ConfigFieldChange{fields[index].name,
                                                    fields[index].offset,
                                                    fields[index].size});
            }
        }
        return changes;
    }
}

/**
 * @brief 值相等判断：结构体走字段级比较，其它类型用 `operator==`。
 * @details `ConfigVar<T>` 用它判断「新值是否与旧值相同」。对结构体而言，
 *          字段级比较比 `operator==` 更宽松——无需为配置结构体手写比较符。
 */
template <typename T>
bool ConfigValuesEqual(const T& before, const T& after) {
    using Value = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (reflect::HasFields<Value>()) {
        return ConfigFieldDiff(before, after).empty();
    } else {
        return before == after;
    }
}

}  // namespace go2cpp::config
