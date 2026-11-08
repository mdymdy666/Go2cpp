/**
 * @file parser.cpp
 * @brief JSON 解析器的非模板部分：`detail::Scanner` 的重逻辑 + Policy 显式实例化。
 *
 * 为什么只有重逻辑在 `.cpp`：
 * - 字符串转义、UTF-8 编码、数字分类与 Policy 无关，放在这里能减少模板实例化
 *   的代码体积；
 * - 热路径原语（`AtEnd`/`Peek`/`Advance`/`SkipWhitespace`）已经内联在
 *   `parser.hpp`，跨 TU 调用开销从基线里消失；
 * - 显式实例化 Inline/Std/Compact/Auto 四套 Policy，保证所有组合都被编译检查，
 *   不会出现「测试只用了 InlinePolicy，StdPolicy 悄悄编不过」的情况。
 */

#include "go2cpp/json/parser.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "go2cpp/json/core/conversion.hpp"
#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/json/core/text.hpp"
#include "go2cpp/json/config.hpp"
#include "go2cpp/json/document.hpp"
#include "go2cpp/json/error.hpp"
#include "go2cpp/json/escape.hpp"
#include "go2cpp/json/value.hpp"

namespace go2cpp::json::detail {

void Scanner::AppendUtf8(std::uint32_t code) {
  // 编码内核统一放在 `escape.hpp`（writer 的 `\uXXXX` 反向逻辑与测试共用同一份
  // 实现）；这里只负责把 4 字节以内的结果追加到文档字符串池。
  char bytes[kMaxUtf8Length];
  const std::size_t length = EncodeUtf8(code, bytes);
  if (length != 0) {
    m_doc->AppendStringData(std::string_view(bytes, length));
  }
}

std::size_t Scanner::Line() const noexcept {
  std::size_t line = 1;
  std::size_t column = 1;
  ComputePosition(Offset(), line, column);
  return line;
}

std::size_t Scanner::Column() const noexcept {
  std::size_t line = 1;
  std::size_t column = 1;
  ComputePosition(Offset(), line, column);
  return column;
}

void Scanner::ComputePosition(std::size_t offset, std::size_t& line,
                              std::size_t& column) const noexcept {
  line = 1;
  column = 1;
  const std::size_t size = static_cast<std::size_t>(m_end - m_begin);
  const std::size_t limit = offset < size ? offset : size;
  bool last_cr = false;
  for (std::size_t index = 0; index < limit; ++index) {
    const char value = m_begin[index];
    if (value == '\r') {
      ++line;
      column = 1;
      last_cr = true;
    } else if (value == '\n') {
      // `\r\n` 算一个换行：`\n` 只在上一字节不是 `\r` 时递增行号。
      if (!last_cr) {
        ++line;
      }
      column = 1;
      last_cr = false;
    } else {
      // `\r` 自身已把列重置为 1，因此不占列宽。
      ++column;
      last_cr = false;
    }
  }
}

bool Scanner::Fail(JsonFailure failure, ErrorPtr& error) {
  std::size_t line = 1;
  std::size_t column = 1;
  ComputePosition(Offset(), line, column);
  error = NewJsonError(failure, Offset(), line, column);
  return false;
}

bool Scanner::FailAt(JsonFailure failure, std::size_t offset, std::size_t line,
                     std::size_t column, ErrorPtr& error) {
  error = NewJsonError(failure, offset, line, column);
  return false;
}

bool Scanner::FailAtOffset(JsonFailure failure, std::size_t offset, ErrorPtr& error) {
  std::size_t line = 1;
  std::size_t column = 1;
  ComputePosition(offset, line, column);
  error = NewJsonError(failure, offset, line, column);
  return false;
}

bool Scanner::ParseStringSlow(core::Text& out, ErrorPtr& error) {
  if (AtEnd() || *m_cursor != '"') {
    return Fail(JsonFailure::kUnexpectedChar, error);
  }
  ++m_cursor;  // 开引号

  const char* const string_start = m_cursor;
  bool has_escape = false;
  std::size_t mark = 0;

  // 读 4 位十六进制；残缺报「未闭合」，非法字符报「非法转义」。
  // 解码内核复用 `escape.hpp`：这里只负责定位（游标推进与错误偏移），
  // 不再维护第二份 hex 解析——两份实现漂移会让 `\u` 的接受集合不一致。
  auto read_hex4 = [this, &error](std::uint32_t& code) -> bool {
    const std::size_t remaining = static_cast<std::size_t>(m_end - m_cursor);
    if (remaining < 4) {
      m_cursor = m_end;
      return Fail(JsonFailure::kUnclosedString, error);
    }
    EscapeError escape_error;
    if (!DecodeUnicodeEscape(std::string_view(m_cursor, 4), &code, &escape_error)) {
      return FailAtOffset(JsonFailure::kInvalidEscape, Offset() + escape_error.offset, error);
    }
    m_cursor += 4;
    return true;
  };

  for (;;) {
    // 批量跳过普通字符：每字符一次比较 + 一次自增。
    const char* const plain_start = m_cursor;
    while (m_cursor != m_end) {
      const unsigned char plain = static_cast<unsigned char>(*m_cursor);
      if (plain == '"' || plain == '\\' || plain < 0x20u) {
        break;
      }
      ++m_cursor;
    }
    if (has_escape && m_cursor != plain_start) {
      m_doc->AppendStringData(
          std::string_view(plain_start, static_cast<std::size_t>(m_cursor - plain_start)));
    }
    if (AtEnd()) {
      return Fail(JsonFailure::kUnclosedString, error);
    }
    const unsigned char byte = static_cast<unsigned char>(*m_cursor);
    if (byte == '"') {
      const char* const string_end = m_cursor;
      ++m_cursor;
      if (!has_escape) {
        // 零拷贝：无转义的字符串直接指向输入缓冲。
        out = core::Text::Borrow(std::string_view(
            string_start, static_cast<std::size_t>(string_end - string_start)));
      } else {
        out = m_doc->CloseString(mark);
      }
      return true;
    }
    if (byte == '\\') {
      if (!has_escape) {
        has_escape = true;
        mark = m_doc->MarkString();
        // 把第一个转义之前的普通段先写进池（此时 plain_start == string_start）。
        m_doc->AppendStringData(
            std::string_view(plain_start, static_cast<std::size_t>(m_cursor - plain_start)));
      }
      ++m_cursor;  // 反斜杠
      if (AtEnd()) {
        return Fail(JsonFailure::kUnclosedString, error);
      }
      const char escape = *m_cursor;
      switch (escape) {
        case '"':
          m_doc->AppendStringChar('"');
          ++m_cursor;
          break;
        case '\\':
          m_doc->AppendStringChar('\\');
          ++m_cursor;
          break;
        case '/':
          m_doc->AppendStringChar('/');
          ++m_cursor;
          break;
        case 'b':
          m_doc->AppendStringChar('\b');
          ++m_cursor;
          break;
        case 'f':
          m_doc->AppendStringChar('\f');
          ++m_cursor;
          break;
        case 'n':
          m_doc->AppendStringChar('\n');
          ++m_cursor;
          break;
        case 'r':
          m_doc->AppendStringChar('\r');
          ++m_cursor;
          break;
        case 't':
          m_doc->AppendStringChar('\t');
          ++m_cursor;
          break;
        case 'u': {
          ++m_cursor;  // 'u'
          std::uint32_t code = 0;
          if (!read_hex4(code)) {
            return false;
          }
          if (IsHighSurrogate(code)) {
            // 高代理：必须紧跟 `\uDC00`..`\uDFFF` 才能合成一个码点。
            if (AtEnd()) {
              return Fail(JsonFailure::kUnclosedString, error);
            }
            if (*m_cursor != '\\') {
              return Fail(JsonFailure::kInvalidEscape, error);
            }
            ++m_cursor;
            if (AtEnd()) {
              return Fail(JsonFailure::kUnclosedString, error);
            }
            if (*m_cursor != 'u') {
              return Fail(JsonFailure::kInvalidEscape, error);
            }
            ++m_cursor;
            std::uint32_t low = 0;
            if (!read_hex4(low)) {
              return false;
            }
            EscapeError escape_error;
            if (!CombineSurrogates(code, low, &code, &escape_error)) {
              return Fail(JsonFailure::kInvalidEscape, error);
            }
          } else if (IsLowSurrogate(code)) {
            return Fail(JsonFailure::kInvalidEscape, error);  // 孤立低代理
          }
          AppendUtf8(code);
          break;
        }
        default:
          return Fail(JsonFailure::kInvalidEscape, error);
      }
      continue;
    }
    if (byte < 0x20u) {
      return Fail(JsonFailure::kUnescapedControl, error);
    }
    ++m_cursor;
  }
}

/// @brief 精确的 10 的幂（0..22）；Clinger 快速路径要求指数落在该区间。
constexpr double kPow10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

bool Scanner::ParseNumberSlow(Node& out, ErrorPtr& error) {
  const std::size_t start_offset = Offset();
  const char* const start = m_cursor;

  if (AtEnd()) {
    return Fail(JsonFailure::kUnexpectedEnd, error);
  }
  bool negative = false;
  if (*m_cursor == '-') {
    negative = true;
    ++m_cursor;
  }
  if (AtEnd()) {
    return Fail(JsonFailure::kUnexpectedEnd, error);
  }

  // 扫描的同时累积十进制尾数：命中「≤15 位 + 指数 ∈[-22,22]」时走 Clinger
  // 快速路径（一次乘/除，结果与正确舍入一致），否则回退 from_chars。
  std::uint64_t mantissa = 0;
  int digits = 0;
  int fraction_digits = 0;
  bool fast = true;

  if (*m_cursor == '0') {
    ++m_cursor;
    if (!AtEnd() && scanner_detail::IsDigit(*m_cursor)) {
      return Fail(JsonFailure::kInvalidNumber, error);
    }
  } else if (*m_cursor >= '1' && *m_cursor <= '9') {
    while (!AtEnd() && scanner_detail::IsDigit(*m_cursor)) {
      const unsigned digit = static_cast<unsigned>(*m_cursor - '0');
      if (fast) {
        // 用十进制位数代替除法做溢出检查：19 位以内不可能溢出 uint64，
        // 64 位整数除法（~20 周期/次）从热路径消失。
        if (digits < 19) {
          mantissa = mantissa * 10 + digit;
          ++digits;
        } else {
          fast = false;
        }
      }
      ++m_cursor;
    }
  } else {
    return Fail(JsonFailure::kInvalidNumber, error);
  }

  bool is_integer = true;
  if (!AtEnd() && *m_cursor == '.') {
    is_integer = false;
    ++m_cursor;
    if (AtEnd()) {
      return Fail(JsonFailure::kUnexpectedEnd, error);
    }
    if (!scanner_detail::IsDigit(*m_cursor)) {
      return Fail(JsonFailure::kInvalidNumber, error);
    }
    while (!AtEnd() && scanner_detail::IsDigit(*m_cursor)) {
      const unsigned digit = static_cast<unsigned>(*m_cursor - '0');
      if (fast) {
        if (digits < 19) {
          mantissa = mantissa * 10 + digit;
          ++digits;
          ++fraction_digits;
        } else {
          fast = false;
        }
      }
      ++m_cursor;
    }
  }

  int exponent = 0;
  if (!AtEnd() && (*m_cursor == 'e' || *m_cursor == 'E')) {
    is_integer = false;
    ++m_cursor;
    bool exponent_negative = false;
    if (!AtEnd() && (*m_cursor == '+' || *m_cursor == '-')) {
      exponent_negative = *m_cursor == '-';
      ++m_cursor;
    }
    if (AtEnd()) {
      return Fail(JsonFailure::kUnexpectedEnd, error);
    }
    if (!scanner_detail::IsDigit(*m_cursor)) {
      return Fail(JsonFailure::kInvalidNumber, error);
    }
    while (!AtEnd() && scanner_detail::IsDigit(*m_cursor)) {
      if (fast) {
        if (exponent > 100000) {
          fast = false;
        } else {
          exponent = exponent * 10 + (*m_cursor - '0');
        }
      }
      ++m_cursor;
    }
    if (exponent_negative) {
      exponent = -exponent;
    }
  }

  if (is_integer) {
    // 整数快速路径：尾数未溢出 uint64 时直接做范围检查。
    if (fast) {
      constexpr std::uint64_t kInt64Max = static_cast<std::uint64_t>(INT64_MAX);
      if (!negative && mantissa <= kInt64Max) {
        out.kind = NodeKind::kInt;
        out.payload.integer = static_cast<std::int64_t>(mantissa);
        return true;
      }
      if (negative && mantissa <= kInt64Max + 1u) {
        out.kind = NodeKind::kInt;
        out.payload.integer =
            mantissa == kInt64Max + 1u ? INT64_MIN : -static_cast<std::int64_t>(mantissa);
        return true;
      }
    }
    const std::string_view text(start, static_cast<std::size_t>(m_cursor - start));
    std::int64_t integer = 0;
    const std::from_chars_result result =
        std::from_chars(text.data(), text.data() + text.size(), integer);
    if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) {
      out.kind = NodeKind::kInt;
      out.payload.integer = integer;
      return true;
    }
    // 超出 int64：退化为 Decimal，保留全部十进制位。
    core::Decimal decimal;
    if (core::StringToDecimal(text, decimal)) {
      out.kind = NodeKind::kDecimal;
      out.payload.decimal_index = m_doc->PushDecimal(std::move(decimal));
      return true;
    }
    return FailAtOffset(JsonFailure::kInvalidNumber, start_offset, error);
  }

  const int total_exponent = exponent - fraction_digits;
  if (fast && digits <= 15 && total_exponent >= -22 && total_exponent <= 22) {
    double value = static_cast<double>(mantissa);
    if (total_exponent > 0) {
      value *= kPow10[total_exponent];
    } else if (total_exponent < 0) {
      value /= kPow10[-total_exponent];
    }
    out.kind = NodeKind::kDouble;
    out.payload.real = negative ? -value : value;
    return true;
  }

  const std::string_view text(start, static_cast<std::size_t>(m_cursor - start));
  double value = 0.0;
  if (core::StringToDouble(text, value)) {
    out.kind = NodeKind::kDouble;
    out.payload.real = value;
    return true;
  }
  // 超出 double：退化为 Decimal（如 1e400、1e-400）。
  core::Decimal decimal;
  if (core::StringToDecimal(text, decimal)) {
    out.kind = NodeKind::kDecimal;
    out.payload.decimal_index = m_doc->PushDecimal(std::move(decimal));
    return true;
  }
  return FailAtOffset(JsonFailure::kInvalidNumber, start_offset, error);
}

bool Scanner::ParseLiteral(std::string_view literal, Node& out, ErrorPtr& error) {
  for (const char expected : literal) {
    if (AtEnd()) {
      return Fail(JsonFailure::kUnexpectedEnd, error);
    }
    if (*m_cursor != expected) {
      return Fail(JsonFailure::kUnexpectedChar, error);
    }
    ++m_cursor;
  }
  // 内部契约：调用方只传 true/false/null 三个字面量。
  if (literal == "true") {
    out.kind = NodeKind::kBool;
    out.payload.boolean = true;
  } else if (literal == "false") {
    out.kind = NodeKind::kBool;
    out.payload.boolean = false;
  } else {
    out.kind = NodeKind::kNull;
  }
  return true;
}

}  // namespace go2cpp::json::detail

namespace go2cpp::json {

// ---------------------------------------------------------------------------
// 恢复路径的非模板入口（场景 5：默认 kAbort 零回退的关键）
// ---------------------------------------------------------------------------

// 只有默认 InlinePolicy 需要这个入口：其它 Policy 的恢复路径由调用方 TU 里的
// `ParseRecoverLimits<true>` 实例化完成（见 `Parse` 的 `if constexpr` 分派）。
// 显式特化保证默认 `Parser` 的每个使用方 TU 只会看到一个外部符号，不会各自编译
// 一份恢复状态机（旧版把恢复路径整段留在头文件，bench 的默认路径 I-cache/IPA-SRA
// 上下文被挤占，实测 +5~9%）。
// 固定 kChecks == true：恢复是冷路径，token 检查在 max_string_bytes == 0 &&
// !validate_utf8 时恒真，少一份实例化比省这几次冷函数调用更重要。
template <>
bool BasicParser<InlinePolicy>::ParseRecoverImpl(std::string_view input, Document& doc,
                                                 ErrorPtr& error,
                                                 const ParserConfig& config) const {
  return ParseInternalBody<true, true>(input, doc, error, config);
}

// 显式实例化四套 Policy：保证 parser.hpp 的模板代码对所有栈策略都被编译检查。
template class BasicParser<InlinePolicy>;
template class BasicParser<StdPolicy>;
template class BasicParser<CompactPolicy>;
template class BasicParser<AutoPolicy>;

}  // namespace go2cpp::json
