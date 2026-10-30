#pragma once

/**
 * @file json.hpp
 * @brief JSON-like 文本编解码：调试可读格式的写入器与解析器。
 *
 * @details 文本模式不是协议的一部分，而是排查问题时「人能看懂」的视图：
 *          字段名、布尔、字符串原样保留，二进制字段用数字数组表示。为了让
 *          `ReflectCodec` 的文本模式能真正往返，解析器按严格 JSON 语法实现
 *          （对象/数组/字符串/数字/布尔/null），并对深度、成员数、字符串长度
 *          设上限，避免调试接口变成内存放大器。
 *
 *          与标准 JSON 的差异：
 *          - 非有限浮点（NaN/Inf）不是合法 JSON，写入器会拒绝（`ok()` 变
 *            false），调用方应改用二进制模式；
 *          - 不输出缩进，保持单行（便于日志采集）。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::reflect {

/// @brief JSON 解析的最大嵌套深度。
inline constexpr std::size_t kMaxJsonDepth = 64;

/// @brief JSON 对象/数组的最大元素数。
inline constexpr std::size_t kMaxJsonElements = 4096;

/// @brief JSON 字符串的最大字节数。
inline constexpr std::size_t kMaxJsonStringBytes = 16u * 1024u * 1024u;

/// @brief 把字符串转义为 JSON 字符串内容（不含首尾引号）。
std::string EscapeJsonString(std::string_view text);

/// @brief JSON 值（解析结果或待写入的树）。
struct JsonValue {
    /// @brief 值类型。
    enum class Kind : std::uint8_t {
        kNull = 0,    ///< null。
        kBool = 1,    ///< 布尔。
        kNumber = 2,  ///< 数字字面量（原文保存在 `text`）。
        kString = 3,  ///< 字符串（解码后的内容）。
        kArray = 4,   ///< 数组。
        kObject = 5,  ///< 对象（保持插入顺序）。
    };

    Kind kind = Kind::kNull;                                 ///< 值类型。
    bool boolean = false;                                    ///< 布尔值。
    std::string text;                                        ///< 字符串内容或数字原文。
    std::vector<JsonValue> elements;                         ///< 数组元素。
    std::vector<std::pair<std::string, JsonValue>> members;  ///< 对象成员。

    /// @brief 按名字查对象成员；非对象或不存在返回 nullptr。
    const JsonValue* Find(std::string_view key) const noexcept;

    /// @brief 构造空对象。
    static JsonValue MakeObject();
    /// @brief 构造空数组。
    static JsonValue MakeArray();
};

/**
 * @brief JSON 写入器：按调用顺序生成单行 JSON。
 *
 * @details 调用方负责配对 `BeginObject/EndObject` 与 `BeginArray/EndArray`，
 *          并保证对象内先 `Key` 再写值。违反这些约束时写入器把 `ok()` 置为
 *          false，而不是生成非法 JSON；编码路径会因此返回错误。
 */
class JsonWriter {
   public:
    /// @param out 输出目标；写入器只追加，不清理已有内容。
    explicit JsonWriter(std::string& out) noexcept : m_out(out) {}

    /// @brief 开始一个对象。
    void BeginObject();
    /// @brief 结束当前对象。
    void EndObject();
    /// @brief 开始一个数组。
    void BeginArray();
    /// @brief 结束当前数组。
    void EndArray();
    /// @brief 写入对象键（后面必须紧跟一个值）。
    void Key(std::string_view key);
    /// @brief 写入字符串值。
    void String(std::string_view value);
    /// @brief 写入有符号整数。
    void Int(std::int64_t value);
    /// @brief 写入无符号整数。
    void Uint(std::uint64_t value);
    /// @brief 写入浮点数；非有限值时失败。
    void Double(double value);
    /// @brief 写入已经校验过的数字字面量（仅允许 `-+0123456789.eE`）。
    void NumberLiteral(std::string_view literal);
    /// @brief 写入布尔值。
    void Bool(bool value);
    /// @brief 写入 null。
    void Null();

    /// @brief 是否仍然合法。
    bool ok() const noexcept {
        return m_ok;
    }

    /// @brief 显式标记失败。
    void Fail() noexcept {
        m_ok = false;
    }

   private:
    struct Scope {
        bool first = true;    ///< 当前容器是否还没有元素。
        bool object = false;  ///< true=对象，false=数组。
    };

    void BeforeValue();
    void AppendEscaped(std::string_view text);

    std::string& m_out;           ///< 输出目标。
    std::vector<Scope> m_scopes;  ///< 当前容器栈。
    bool m_after_key = false;     ///< 刚写完对象键、等待值。
    bool m_ok = true;             ///< 是否合法。
};

/// @brief 把 `JsonValue` 树递归写入写入器。
bool WriteJsonValue(const JsonValue& value, JsonWriter& out);

/**
 * @brief 严格 JSON 解析器（无状态，全部方法为静态）。
 */
class JsonParser {
   public:
    /// @brief 解析一段 JSON 文本。
    /// @param text 输入文本。
    /// @param out 解析结果。
    /// @param consumed 非空时写入消费的字节数（解析结束位置）。
    /// @param error 非空时写入人类可读错误（可选）。
    /// @return 是否成功。
    static bool Parse(std::string_view text, JsonValue& out, std::size_t* consumed = nullptr,
                      std::string* error = nullptr);
};

/// @brief 按有符号 64 位整数解析数字成员；失败不修改 `out`。
bool JsonNumberToInt64(const JsonValue& value, std::int64_t& out) noexcept;

/// @brief 按无符号 64 位整数解析数字成员；失败不修改 `out`。
bool JsonNumberToUint64(const JsonValue& value, std::uint64_t& out) noexcept;

/// @brief 按 double 解析数字成员；失败不修改 `out`。
bool JsonNumberToDouble(const JsonValue& value, double& out) noexcept;

}  // namespace go2cpp::reflect
