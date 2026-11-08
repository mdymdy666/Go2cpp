/**
 * @file escape.cpp
 * @brief `escape.hpp` 的非内联实现：`\uXXXX` 解码、代理对组合与反向转义。
 *
 * 只有「解析/编码一个转义序列」这种一次性逻辑放在这里；UTF-8 编码
 * （`EncodeUtf8`）与十六进制判位（`HexDigit`）留在头文件内联，因为解析器的
 * `\u` 分支每次都要用到它们，跨 TU 调用会白丢一次函数调用。
 */

#include "go2cpp/json/escape.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::json {

namespace {

/// @brief Unicode 码点上限（RFC 8259 引用的 Unicode 标准）。
constexpr std::uint32_t kMaxCodePoint = 0x10FFFFu;

/// @brief 代理区起点 / 终点。
constexpr std::uint32_t kSurrogateFirst = 0xD800u;
constexpr std::uint32_t kSurrogateLast = 0xDFFFu;

/// @brief 高代理基数与低代理基数（代理对合成公式用）。
constexpr std::uint32_t kHighSurrogateBase = 0xD800u;
constexpr std::uint32_t kLowSurrogateBase = 0xDC00u;

/// @brief 写一个 4 位大写十六进制码元（`XXXX`，不含 `\u`）。
void AppendHex4(std::uint32_t value, std::string* out) {
  constexpr char kDigits[] = "0123456789ABCDEF";
  out->push_back('\\');
  out->push_back('u');
  out->push_back(kDigits[(value >> 12) & 0xFu]);
  out->push_back(kDigits[(value >> 8) & 0xFu]);
  out->push_back(kDigits[(value >> 4) & 0xFu]);
  out->push_back(kDigits[value & 0xFu]);
}

}  // namespace

bool DecodeUnicodeEscape(std::string_view hex4, std::uint32_t* code_point) {
  EscapeError error;
  return DecodeUnicodeEscape(hex4, code_point, &error);
}

bool DecodeUnicodeEscape(std::string_view hex4, std::uint32_t* code_point,
                         EscapeError* error) {
  if (error != nullptr) {
    *error = EscapeError{};
  }
  if (hex4.size() < 4) {
    if (error != nullptr) {
      error->failure = EscapeFailure::kShortInput;
      error->offset = hex4.size();
    }
    return false;
  }
  std::uint32_t code = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    const int digit = HexDigit(hex4[index]);
    if (digit < 0) {
      if (error != nullptr) {
        error->failure = EscapeFailure::kBadHex;
        error->offset = index;
      }
      return false;
    }
    code = (code << 4) | static_cast<std::uint32_t>(digit);
  }
  if (code_point != nullptr) {
    *code_point = code;
  }
  return true;
}

bool CombineSurrogates(std::uint32_t high, std::uint32_t low, std::uint32_t* code_point,
                       EscapeError* error) {
  if (error != nullptr) {
    *error = EscapeError{};
  }
  if (!IsHighSurrogate(high) || !IsLowSurrogate(low)) {
    if (error != nullptr) {
      error->failure = EscapeFailure::kLoneSurrogate;
    }
    return false;
  }
  const std::uint32_t combined =
      0x10000u + ((high - kHighSurrogateBase) << 10) + (low - kLowSurrogateBase);
  if (combined > kMaxCodePoint) {
    if (error != nullptr) {
      error->failure = EscapeFailure::kCodePointOutOfRange;
    }
    return false;
  }
  if (code_point != nullptr) {
    *code_point = combined;
  }
  return true;
}

bool DecodeUnicodeEscapeSequence(std::string_view hex, std::uint32_t* code_point,
                                 EscapeError* error) {
  if (error != nullptr) {
    *error = EscapeError{};
  }
  std::uint32_t first = 0;
  if (!DecodeUnicodeEscape(hex, &first, error)) {
    return false;
  }
  // step2: 非代理 → 普通 BMP 字符，直接返回。
  if (first < kSurrogateFirst || first > kSurrogateLast) {
    if (code_point != nullptr) {
      *code_point = first;
    }
    return true;
  }
  // step3: 低代理不可能是序列开头。
  if (IsLowSurrogate(first)) {
    if (error != nullptr) {
      error->failure = EscapeFailure::kLoneSurrogate;
      error->offset = 0;
    }
    return false;
  }
  // step4: 高代理必须紧跟 4 位低代理；结构错误与「低代理非法」区分开，便于上层
  // 生成更准确的错误位置。
  if (hex.size() < 8) {
    if (error != nullptr) {
      error->failure = EscapeFailure::kLoneSurrogate;
      error->offset = 4;
    }
    return false;
  }
  std::uint32_t second = 0;
  EscapeError second_error;
  if (!DecodeUnicodeEscape(hex.substr(4, 4), &second, &second_error)) {
    if (error != nullptr) {
      *error = second_error;
      error->offset = 4 + second_error.offset;
    }
    return false;
  }
  if (!IsLowSurrogate(second)) {
    if (error != nullptr) {
      error->failure = EscapeFailure::kLoneSurrogate;
      error->offset = 4;
    }
    return false;
  }
  // step5: 合成并做上限兜底（代理对公式保证结果 ≤ 0x10FFFF，这里只是契约检查）。
  return CombineSurrogates(first, second, code_point, error);
}

void AppendUtf8(std::uint32_t code_point, std::string* out) {
  char bytes[kMaxUtf8Length];
  const std::size_t length = EncodeUtf8(code_point, bytes);
  if (length != 0) {
    out->append(bytes, length);
  }
}

std::string EncodeUnicodeEscape(std::uint32_t code_point) {
  if (code_point > kMaxCodePoint ||
      (code_point >= kSurrogateFirst && code_point <= kSurrogateLast)) {
    return std::string{};  // 非法码点：调用方应先校验，空串即失败信号。
  }
  std::string result;
  result.reserve(12);
  if (code_point < 0x10000u) {
    AppendHex4(code_point, &result);
    return result;
  }
  const std::uint32_t offset = code_point - 0x10000u;
  AppendHex4(kHighSurrogateBase + (offset >> 10), &result);
  AppendHex4(kLowSurrogateBase + (offset & 0x3FFu), &result);
  return result;
}

}  // namespace go2cpp::json
