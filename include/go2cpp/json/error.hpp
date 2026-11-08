#pragma once

/**
 * @file error.hpp
 * @brief JSON 解析失败的结构化错误：`bgo2cpp::json::JsonError`。
 *
 * 继承 `extened::Error`，因此可以直接放进 `ErrorPtr` 错误链，与
 * `Wrap`/`Join`/`ErrorMessage` 等错误工具互操作。除错误文本外还保留
 * `offset`（0 起字节偏移）与 `line`/`column`（均 1 起），供调用方定位坏数据。
 *
 * ## 定位口径
 * - `offset`：出错字节在输入缓冲中的下标（0 起）；输入提前结束时报 `input.size()`。
 * - `line`/`column`：均从 1 开始；**按字节计列**（UTF-8 多字节字符算多列），
 *   `\r\n` 只算一个换行，`\r` 单独出现也算一个换行。
 * - 错误位置指向「让解析无法继续的那个字节」；遇到输入结束时指向末尾。
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "go2cpp/tools/error.hpp"

namespace go2cpp::json {

/// @brief 解析失败的具体原因；与错误文本分离，供调用方程序化分支。
enum class JsonFailure : std::uint8_t {
  kUnexpectedChar = 0,  ///< 该位置出现了语法不允许的字符。
  kUnexpectedEnd,       ///< 输入在语法完成前结束（如 `tru`、缺右括号）。
  kUnclosedString,      ///< 字符串没有闭合的双引号。
  kUnescapedControl,    ///< 字符串内出现未转义的裸控制字符（< 0x20）。
  kInvalidEscape,       ///< 非法转义序列（含残缺/错误的 `\uXXXX` 与孤立代理项）。
  kInvalidNumber,       ///< 数字语法非法，或超出可表示范围。
  kTrailingComma,       ///< 不允许尾逗号时出现了尾逗号。
  kDepthExceeded,       ///< 容器嵌套深度达到 `ParserConfig::max_depth`。
  kTrailingGarbage,     ///< 根值解析完后还有非空白字节。
  kTimeout,             ///< 超出 `ParserConfig::timeout_ms` 时间预算。
  kTooLarge,            ///< 输入字节数超过 `ParserConfig::max_input_bytes`。
  kDuplicateKey,        ///< 对象内出现重复 key，且策略为 `DuplicateKeyPolicy::kError`。
  kStringTooLarge,      ///< 字符串/数字 token 超过 `ParserConfig::max_string_bytes`。
  kInvalidUtf8,         ///< 字符串内出现非法 UTF-8 序列（`validate_utf8=true` 时）。
};

/**
 * @brief 返回失败原因的英文短语；未知枚举值返回 `"unknown"`。
 * @param failure [in] 失败原因。
 * @return 静态存储的短语；调用方不得释放、不得改写。
 */
inline const char* JsonFailureName(JsonFailure failure) noexcept {
  switch (failure) {
    case JsonFailure::kUnexpectedChar:
      return "unexpected character";
    case JsonFailure::kUnexpectedEnd:
      return "unexpected end of input";
    case JsonFailure::kUnclosedString:
      return "unclosed string";
    case JsonFailure::kUnescapedControl:
      return "unescaped control character";
    case JsonFailure::kInvalidEscape:
      return "invalid escape sequence";
    case JsonFailure::kInvalidNumber:
      return "invalid number";
    case JsonFailure::kTrailingComma:
      return "trailing comma";
    case JsonFailure::kDepthExceeded:
      return "maximum depth exceeded";
    case JsonFailure::kTrailingGarbage:
      return "trailing garbage";
    case JsonFailure::kTimeout:
      return "timeout";
    case JsonFailure::kTooLarge:
      return "input too large";
    case JsonFailure::kDuplicateKey:
      return "duplicate key";
    case JsonFailure::kStringTooLarge:
      return "string too large";
    case JsonFailure::kInvalidUtf8:
      return "invalid utf-8";
  }
  return "unknown";
}

/**
 * @brief JSON 解析错误：原因 + 位置。
 *
 * @details 对象构造后不可变，可跨线程共享；`Message()` 形如
 *          `json: unexpected character at offset 7 (line 1, column 8)`。
 */
class JsonError final : public Error {
 public:
  /**
   * @brief 构造错误。
   * @param failure [in] 失败原因；决定 `Message()` 的前半段。
   * @param offset [in] 出错字节的 0 起偏移；调用方保证 <= 输入长度。
   * @param line [in] 出错位置的 1 起行号。
   * @param column [in] 出错位置的 1 起列号（按字节计）。
   */
  JsonError(JsonFailure failure, std::size_t offset, std::size_t line, std::size_t column)
      : m_failure(failure), m_offset(offset), m_line(line), m_column(column) {}

  /// @brief 返回带原因与位置的错误文本。
  std::string Message() const override {
    std::string out = "json: ";
    out += JsonFailureName(m_failure);
    out += " at offset ";
    out += std::to_string(m_offset);
    out += " (line ";
    out += std::to_string(m_line);
    out += ", column ";
    out += std::to_string(m_column);
    out += ')';
    return out;
  }

  /// @brief 结构化失败原因。
  JsonFailure Failure() const noexcept { return m_failure; }
  /// @brief 出错位置的 0 起字节偏移。
  std::size_t Offset() const noexcept { return m_offset; }
  /// @brief 出错位置的 1 起行号。
  std::size_t Line() const noexcept { return m_line; }
  /// @brief 出错位置的 1 起列号（按字节计）。
  std::size_t Column() const noexcept { return m_column; }

 private:
  JsonFailure m_failure;  ///< 失败原因。
  std::size_t m_offset;   ///< 0 起字节偏移。
  std::size_t m_line;     ///< 1 起行号。
  std::size_t m_column;   ///< 1 起列号。
};

/**
 * @brief 构造一个 `JsonError` 的 `ErrorPtr`。
 * @param failure [in] 失败原因。@param offset [in] 0 起字节偏移。
 * @param line [in] 1 起行号。@param column [in] 1 起列号。
 * @return 不可变错误对象；调用方按 `ErrorPtr` 语义共享。
 */
inline ErrorPtr NewJsonError(JsonFailure failure, std::size_t offset, std::size_t line,
                             std::size_t column) {
  return std::make_shared<JsonError>(failure, offset, line, column);
}

}  // namespace go2cpp::json
