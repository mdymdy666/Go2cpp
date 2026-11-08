/**
 * @file conversion.cpp
 * @brief `extened::core` 转换表的实现（迁移自 week1 `src/conversion.cpp`）。
 *
 * 与 week1 的差异：
 * - `StringView`（`std::wstring_view`）→ `std::string_view`，手写解析全部改 ASCII 字节；
 * - `String` 分支（`std::wstring`）→ `Text`（借用视图，零拷贝）；
 * - 注册失败的错误从 `CSVP_ERROR_AT` 宏改为 `extened::NewError`；
 * - 内置 id 从 csvp 的 `kStringType/kIntType/...` 改为 `extened::reflect` 的常量，
 *   `kIntType`(2) 对应 `kInt32Type`(2)。
 *
 * 解析全部手写，不建临时 `std::string`、不受进程 locale 的小数点影响。
 */

#include "go2cpp/json/core/conversion.hpp"

#include <charconv>
#include <climits>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace go2cpp::json::core {

namespace {

/// @brief 只认 ASCII 数字；与 `Decimal` 的解析保持同一判定。
bool IsDigit(char value) noexcept { return value >= '0' && value <= '9'; }

/// @brief 32 位有符号整数解析：范围检查用无符号累加，避免 INT_MIN 先溢出。
bool ParseInt(std::string_view text, int& out) {
  if (text.empty()) {
    return false;
  }
  std::size_t pos = 0;
  bool negative = false;
  if (text[pos] == '+' || text[pos] == '-') {
    negative = text[pos] == '-';
    ++pos;
  }
  if (pos >= text.size()) {
    return false;
  }
  // 负号单独记，2147483648 这个边界值不能先转成 int 再取反。
  const unsigned long long limit = negative ? 2147483648ULL : 2147483647ULL;
  unsigned long long magnitude = 0;
  for (; pos < text.size(); ++pos) {
    if (!IsDigit(text[pos])) {
      return false;
    }
    const unsigned digit = static_cast<unsigned>(text[pos] - '0');
    if (magnitude > (limit - digit) / 10ULL) {
      return false;
    }
    magnitude = magnitude * 10ULL + digit;
  }
  if (negative) {
    out = magnitude == 2147483648ULL ? INT_MIN : -static_cast<int>(magnitude);
  } else {
    out = static_cast<int>(magnitude);
  }
  return true;
}

/// @brief 双精度解析：优先 `std::from_chars`（正确舍入、O(数字位数)），
///        编译器不支持浮点 `from_chars` 时回退到 long double 累积（O(指数)）。
bool ParseDouble(std::string_view text, double& out) {
  if (text.empty()) {
    return false;
  }
#if (defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11) || \
    (defined(_MSC_VER) && _MSC_VER >= 1924)
  {
    // JSON 不允许 inf/nan；部分 from_chars 实现会接受，先按首字符拒掉。
    std::size_t probe = 0;
    if (probe < text.size() && (text[probe] == '+' || text[probe] == '-')) ++probe;
    if (probe < text.size() && (IsDigit(text[probe]) || text[probe] == '.')) {
      double value = 0.0;
      const std::from_chars_result result =
          std::from_chars(text.data(), text.data() + text.size(), value);
      if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) {
        out = value;
        return true;
      }
    }
    // 语法错误或超出 double：交给下面的手写路径，保持原有失败语义。
  }
#endif
  std::size_t pos = 0;
  bool negative = false;
  if (text[pos] == '+' || text[pos] == '-') {
    negative = text[pos] == '-';
    ++pos;
  }

  long double mantissa = 0.0L;
  bool has_digit = false;
  bool after_point = false;
  int fraction_digits = 0;
  for (; pos < text.size(); ++pos) {
    const char value = text[pos];
    if (IsDigit(value)) {
      mantissa = mantissa * 10.0L + static_cast<long double>(value - '0');
      if (after_point) {
        ++fraction_digits;
      }
      has_digit = true;
      continue;
    }
    if (value == '.' && !after_point) {
      after_point = true;
      continue;
    }
    break;
  }
  if (!has_digit) {
    return false;
  }

  int exponent = 0;
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    ++pos;
    bool exponent_negative = false;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
      exponent_negative = text[pos] == '-';
      ++pos;
    }
    if (pos >= text.size() || !IsDigit(text[pos])) {
      return false;
    }
    for (; pos < text.size() && IsDigit(text[pos]); ++pos) {
      // 指数大到不可能落在 double 范围，截断后统一判超范围。
      if (exponent < 100000) {
        exponent = exponent * 10 + static_cast<int>(text[pos] - '0');
      }
    }
    if (exponent_negative) {
      exponent = -exponent;
    }
  }
  if (pos != text.size()) {
    return false;
  }

  exponent -= fraction_digits;
  // double 的十进制范围约 ±308；超过 400 直接判超范围，避免循环十万次。
  if (exponent > 400 || exponent < -400) {
    return false;
  }
  for (int index = 0; index < exponent; ++index) {
    mantissa *= 10.0L;
  }
  for (int index = 0; index < -exponent; ++index) {
    mantissa /= 10.0L;
  }
  if (!std::isfinite(mantissa)) {
    return false;
  }
  const double value = static_cast<double>(negative ? -mantissa : mantissa);
  if (!std::isfinite(value)) {
    return false;
  }
  // 非零输入完全下溢到 0 时按超范围处理，与 strtod 的 ERANGE 语义一致。
  if (value == 0.0 && mantissa != 0.0L) {
    return false;
  }
  out = value;
  return true;
}

/// @brief 内置转换器：int 在 `ItemValue` 里统一存成 int64，避免两种整型分支。
bool ConvertInt(std::string_view from, ItemValue& to) {
  int value = 0;
  if (!ParseInt(from, value)) {
    return false;
  }
  to = static_cast<std::int64_t>(value);
  return true;
}

bool ConvertDouble(std::string_view from, ItemValue& to) {
  double value = 0.0;
  if (!ParseDouble(from, value)) {
    return false;
  }
  to = value;
  return true;
}

bool ConvertDecimal(std::string_view from, ItemValue& to) {
  Decimal value;
  if (!StringToDecimal(from, value)) {
    return false;
  }
  to = std::move(value);
  return true;
}

bool ConvertBool(std::string_view from, ItemValue& to) {
  bool value = false;
  if (!StringToBool(from, value)) {
    return false;
  }
  to = value;
  return true;
}

/// @brief 字符串转换器：借用输入缓冲，零拷贝。
bool ConvertString(std::string_view from, ItemValue& to) {
  to = Text::Borrow(from);
  return true;
}

/// @brief 转换表：自定义类型也写进这张表，读取时只需要查一次哈希。
std::unordered_map<reflect::TypeId, Converter>& ConverterTable() {
  static std::unordered_map<reflect::TypeId, Converter> table;
  return table;
}

std::mutex& ConverterMutex() {
  static std::mutex mutex;
  return mutex;
}

/// @brief 无递归插入：内置注册与业务注册都走这里，避免 `RegisterConverter`
///        在内部初始化时递归调用自己。
bool InsertConverter(reflect::TypeId id, Converter converter) {
  if (id == reflect::kUnknownType || converter == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(ConverterMutex());
  ConverterTable().emplace(id, converter);  // 同名幂等：保留首次注册
  return true;
}

void RegisterBuiltinConverters() {
  InsertConverter(reflect::kStringType, &ConvertString);
  InsertConverter(reflect::kInt32Type, &ConvertInt);
  InsertConverter(reflect::kDoubleType, &ConvertDouble);
  InsertConverter(kDecimalType, &ConvertDecimal);
  InsertConverter(reflect::kBoolType, &ConvertBool);
}

/// @brief 首次使用时注册内置转换器；函数内静态的初始化由 C++11 保证线程安全。
void EnsureBuiltinConverters() {
  static const bool initialized = []() {
    RegisterBuiltinConverters();
    return true;
  }();
  (void)initialized;
}

}  // namespace

bool StringToInt(std::string_view from, int& to) { return ParseInt(from, to); }

bool StringToDouble(std::string_view from, double& to) { return ParseDouble(from, to); }

bool StringToBool(std::string_view from, bool& to) {
  if (from == "true" || from == "TRUE" || from == "1" || from == "yes") {
    to = true;
    return true;
  }
  if (from == "false" || from == "FALSE" || from == "0" || from == "no") {
    to = false;
    return true;
  }
  return false;
}

bool StringToDecimal(std::string_view from, Decimal& to) { return Decimal::Parse(from, to); }

bool StringToString(std::string_view from, Text& to) {
  to = Text::Borrow(from);
  return true;
}

bool RegisterConverter(reflect::TypeId id, Converter converter) {
  ErrorPtr ignored;
  return RegisterConverter(id, converter, ignored);
}

bool RegisterConverter(reflect::TypeId id, Converter converter, ErrorPtr& error) {
  EnsureBuiltinConverters();
  error.reset();
  if (id == reflect::kUnknownType || converter == nullptr) {
    error = NewError("RegisterConverter: invalid type id or null converter");
    return false;
  }
  if (id < reflect::kFirstDynamicType) {
    // 内置转换器是类型系统的基准，禁止覆盖，否则同一 TypeId 的行为会静默改变。
    error = NewError("RegisterConverter: builtin type id " + std::to_string(id) +
                     " cannot be overridden");
    return false;
  }
  return InsertConverter(id, converter);
}

Converter FindConverter(reflect::TypeId id) {
  EnsureBuiltinConverters();
  std::lock_guard<std::mutex> lock(ConverterMutex());
  const auto found = ConverterTable().find(id);
  return found == ConverterTable().end() ? nullptr : found->second;
}

bool Convert(reflect::TypeId id, std::string_view from, ItemValue& to) {
  const Converter converter = FindConverter(id);
  if (converter == nullptr) {
    return false;
  }
  // 先写临时值：转换失败时 `to` 保持原状，避免调用方读到半成品。
  ItemValue parsed;
  if (!converter(from, parsed)) {
    return false;
  }
  to = std::move(parsed);
  return true;
}

}  // namespace go2cpp::json::core
