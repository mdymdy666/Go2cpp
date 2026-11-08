/**
 * @file writer.cpp
 * @brief `writer.hpp` 的实现：迭代式 JSON 序列化（无递归）。
 *
 * 为什么用显式栈而不是递归：解析器支持 10 万层嵌套（`test_json_stream` 与
 * 场景测试里就有深嵌套），writer 若递归，第一次对深文档做 golden 输出就会
 * 爆栈。显式栈把深度成本放到堆上，栈帧只有 24 B。
 */

#include "go2cpp/json/writer.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/json/escape.hpp"
#include "go2cpp/json/value.hpp"

namespace go2cpp::json {

namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

/// @brief 写一个必须转义的字节（`"` `\` 或 `< 0x20` 控制字符）。
void AppendEscapedByte(unsigned char byte, std::string* out) {
  switch (byte) {
    case '"':
      out->append("\\\"", 2);
      return;
    case '\\':
      out->append("\\\\", 2);
      return;
    case '\b':
      out->append("\\b", 2);
      return;
    case '\f':
      out->append("\\f", 2);
      return;
    case '\n':
      out->append("\\n", 2);
      return;
    case '\r':
      out->append("\\r", 2);
      return;
    case '\t':
      out->append("\\t", 2);
      return;
    default:
      out->append("\\u00", 4);
      out->push_back(kHexDigits[byte >> 4]);
      out->push_back(kHexDigits[byte & 0x0Fu]);
      return;
  }
}

/**
 * @brief 解码一个 UTF-8 码点（严格：拒绝 overlong、代理区、> U+10FFFF）。
 *
 * @param data [in] 指向序列首字节；调用方保证 `size > 0`。
 * @param size [in] 剩余可读字节数。
 * @param code_point [out] 成功时写码点；失败时不改动。
 * @return 消耗字节数 `1..4`；非法序列返回 `0`（调用方按单字节原文处理）。
 */
std::size_t DecodeUtf8(const unsigned char* data, std::size_t size, std::uint32_t* code_point) {
  const unsigned char lead = data[0];
  if (lead < 0x80u) {
    *code_point = lead;
    return 1;
  }
  std::size_t length = 0;
  std::uint32_t code = 0;
  std::uint32_t minimum = 0;
  if ((lead & 0xE0u) == 0xC0u) {
    length = 2;
    code = lead & 0x1Fu;
    minimum = 0x80u;
  } else if ((lead & 0xF0u) == 0xE0u) {
    length = 3;
    code = lead & 0x0Fu;
    minimum = 0x800u;
  } else if ((lead & 0xF8u) == 0xF0u) {
    length = 4;
    code = lead & 0x07u;
    minimum = 0x10000u;
  } else {
    return 0;  // 0x80..0xBF 续字节或 0xF8..0xFF 非法首字节。
  }
  if (size < length) {
    return 0;
  }
  for (std::size_t index = 1; index < length; ++index) {
    if ((data[index] & 0xC0u) != 0x80u) {
      return 0;
    }
    code = (code << 6) | (data[index] & 0x3Fu);
  }
  if (code < minimum || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
    return 0;  // overlong / 越界 / 编码了代理项：不是合法 UTF-8。
  }
  *code_point = code;
  return length;
}

/// @brief 按选项写 JSON 字符串（含引号）；非 ASCII 默认原样 UTF-8。
void WriteString(std::string_view text, bool escape_non_ascii, std::string* out) {
  out->push_back('"');
  const auto* data = reinterpret_cast<const unsigned char*>(text.data());
  const std::size_t size = text.size();
  std::size_t index = 0;
  while (index < size) {
    const unsigned char byte = data[index];
    if (byte < 0x20u || byte == '"' || byte == '\\') {
      AppendEscapedByte(byte, out);
      ++index;
      continue;
    }
    if (byte < 0x80u || !escape_non_ascii) {
      out->push_back(static_cast<char>(byte));
      ++index;
      continue;
    }
    // 非 ASCII + 转义模式：合法 UTF-8 写成 \uXXXX（含代理对）；非法字节原样
    // 保留，避免 writer 成为数据丢失点。
    std::uint32_t code_point = 0;
    const std::size_t consumed = DecodeUtf8(data + index, size - index, &code_point);
    if (consumed == 0) {
      out->push_back(static_cast<char>(byte));
      ++index;
      continue;
    }
    out->append(EncodeUnicodeEscape(code_point));
    index += consumed;
  }
  out->push_back('"');
}

/// @brief 写一个数字节点；返回 false 表示遇到 JSON 无法表示的非有限浮点。
bool WriteNumber(const Value& value, std::string* out) {
  char buffer[32];
  if (value.IsInt()) {
    const std::int64_t number = value.As<std::int64_t>();
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), number);
    if (result.ec != std::errc{}) {
      return false;
    }
    out->append(buffer, static_cast<std::size_t>(result.ptr - buffer));
    return true;
  }
  if (value.IsDouble()) {
    const double number = value.As<double>();
    if (!std::isfinite(number)) {
      out->append("null");  // JSON 没有 NaN / Infinity，用 null 占位并报失败。
      return false;
    }
    const std::to_chars_result result = std::to_chars(
        buffer, buffer + sizeof(buffer), number, std::chars_format::general);
    if (result.ec != std::errc{}) {
      return false;
    }
    bool has_point_or_exponent = false;
    for (const char* cursor = buffer; cursor != result.ptr; ++cursor) {
      if (*cursor == '.' || *cursor == 'e' || *cursor == 'E') {
        has_point_or_exponent = true;
        break;
      }
    }
    out->append(buffer, static_cast<std::size_t>(result.ptr - buffer));
    // 最短往返把 1.0 写成 "1"；补 ".0" 保证重新解析仍是浮点节点。
    if (!has_point_or_exponent) {
      out->append(".0", 2);
    }
    return true;
  }
  if (value.IsDecimal()) {
    core::Decimal decimal;
    if (!value.As(decimal)) {
      return false;
    }
    out->append(decimal.ToString());
    return true;
  }
  return false;
}

/// @brief 迭代序列化的栈帧：当前容器 + 下一个待写成员/元素下标。
struct Frame {
  Value value;                ///< 容器句柄。
  std::size_t index = 0;      ///< 下一个成员/元素下标。
  bool is_object = false;     ///< true = 对象（写 key），false = 数组。
};

/// @brief 写 `depth` 层缩进。
void AppendIndent(int indent, std::size_t depth, std::string* out) {
  out->append(static_cast<std::size_t>(indent) * depth, ' ');
}

/// @brief 换行 + `depth` 层缩进。
void AppendNewlineIndent(int indent, std::size_t depth, std::string* out) {
  out->push_back('\n');
  AppendIndent(indent, depth, out);
}

/// @brief 写一个值；容器只写开括号并压帧，元素由主循环展开。
bool WriteValue(const Value& value, const WriterOptions& options, std::string* out,
                std::vector<Frame>* stack) {
  if (value.IsNull()) {
    out->append("null");
    return true;
  }
  if (value.IsBool()) {
    out->append(value.As<bool>() ? "true" : "false");
    return true;
  }
  if (value.IsNumber()) {
    return WriteNumber(value, out);
  }
  if (value.IsString()) {
    WriteString(value.AsText(), options.escape_non_ascii, out);
    return true;
  }
  if (value.IsArray()) {
    out->push_back('[');
    if (value.Size() == 0) {
      out->push_back(']');
      return true;
    }
    stack->push_back(Frame{value, 0, false});
    return true;
  }
  if (value.IsObject()) {
    out->push_back('{');
    if (value.Size() == 0) {
      out->push_back('}');
      return true;
    }
    stack->push_back(Frame{value, 0, true});
    return true;
  }
  out->append("null");  // 非法/失效句柄：按 null 处理，保持输出是合法 JSON。
  return true;
}

}  // namespace

bool Write(const Value& value, std::string* out, const WriterOptions& options) {
  out->clear();
  const int indent = options.indent > 0 ? options.indent : 0;
  bool ok = true;
  std::vector<Frame> stack;
  stack.reserve(16);
  ok = WriteValue(value, options, out, &stack) && ok;

  while (!stack.empty()) {
    Frame& frame = stack.back();
    if (frame.index >= frame.value.Size()) {
      if (indent > 0) {
        AppendNewlineIndent(indent, stack.size() - 1, out);
      }
      out->push_back(frame.is_object ? '}' : ']');
      stack.pop_back();
      continue;
    }
    if (frame.index > 0) {
      out->push_back(',');
    }
    if (indent > 0) {
      AppendNewlineIndent(indent, stack.size(), out);
    }
    if (frame.is_object) {
      WriteString(frame.value.MemberName(frame.index), options.escape_non_ascii, out);
      out->push_back(':');
      if (indent > 0) {
        out->push_back(' ');
      }
    }
    const Value child = frame.value.At(frame.index);
    ++frame.index;
    // 注意：`WriteValue` 可能让 `stack` 扩容，`frame` 从这里开始不再使用。
    ok = WriteValue(child, options, out, &stack) && ok;
  }
  return ok;
}

std::string Write(const Document& doc, const WriterOptions& options) {
  std::string text;
  (void)Write(doc.Root(), &text, options);
  return text;
}

}  // namespace go2cpp::json
