/**
 * @file decimal.cpp
 * @brief `bgo2cpp::json::core::Decimal` 的定点数实现（迁移自 week1 `src/decimal.cpp`）。
 *
 * 算法与 week1 保持一致：十进制数字串 + 小数位，所有运算用十进制字符串完成
 * （加减乘、长除法），不经过 double/long double，因此没有二进制舍入。
 *
 * 与 week1 的差异集中在两处：
 * 1. 字符类型固定为 `char`（JSON 用 UTF-8 字节），`wchar_t` 的 `ParseImpl`
 *    模板实例化被删除，头文件里不再有模板声明；
 * 2. `ParseImpl` 现在返回 `DecimalFailure` 而不是 `bool`，让 `Parse` 能区分
 *    「格式非法」与「数量级超限」——week1 的 `kOutOfRange` 定义了却从未被写入。
 */

#include "go2cpp/json/core/decimal.hpp"

#include <algorithm>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace go2cpp::json::core {

namespace {

/// 输入长度上限：1 MiB。字段级十进制文本不可能合法地超过它，超了就是数据脏。
constexpr std::size_t kMaxInputBytes = 1U << 20;
/// 小数位 / 数量级上限（2^20）。同时用于 Parse 的 wide_scale 与 Divide 的 scale。
constexpr std::int64_t kMaxScale = 1 << 20;
/// 解析指数时的绝对值上限；超过直接判 kOutOfRange。
constexpr std::int64_t kMaxExponent = 1000000;

/// @brief 只认 ASCII 数字；不受 locale 影响。
bool IsDigit(char value) noexcept { return value >= '0' && value <= '9'; }

/// @brief 比较两个无前导零的数字串数值（长度优先，再逐字符）。
int CompareDigits(const std::string& lhs, const std::string& rhs) {
  if (lhs.size() != rhs.size()) {
    return lhs.size() < rhs.size() ? -1 : 1;
  }
  return lhs.compare(rhs);
}

/// @brief 绝对值相加（无符号十进制字符串加法）。
std::string AddAbs(const std::string& lhs, const std::string& rhs) {
  std::string out;
  out.reserve(std::max(lhs.size(), rhs.size()) + 1);
  int carry = 0;
  std::size_t i = lhs.size();
  std::size_t j = rhs.size();
  while (i > 0 || j > 0 || carry > 0) {
    const int a = i > 0 ? lhs[--i] - '0' : 0;
    const int b = j > 0 ? rhs[--j] - '0' : 0;
    const int sum = a + b + carry;
    out.push_back(static_cast<char>('0' + sum % 10));
    carry = sum / 10;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

/// @brief 无符号相减，调用方保证 `lhs >= rhs`；结果去前导零。
std::string SubAbs(const std::string& lhs, const std::string& rhs) {
  std::string out;
  out.reserve(lhs.size());
  int borrow = 0;
  std::size_t i = lhs.size();
  std::size_t j = rhs.size();
  while (i > 0) {
    int a = lhs[--i] - '0' - borrow;
    const int b = j > 0 ? rhs[--j] - '0' : 0;
    if (a < b) {
      a += 10;
      borrow = 1;
    } else {
      borrow = 0;
    }
    out.push_back(static_cast<char>('0' + (a - b)));
  }
  while (out.size() > 1 && out.back() == '0') {
    out.pop_back();
  }
  std::reverse(out.begin(), out.end());
  return out;
}

/// @brief 绝对值相乘（竖式乘法）；任一操作数为空都按 0 处理。
std::string MulAbs(const std::string& lhs, const std::string& rhs) {
  if (lhs.empty() || rhs.empty()) {
    return "0";
  }
  std::vector<int> acc(lhs.size() + rhs.size(), 0);
  for (std::size_t i = lhs.size(); i-- > 0;) {
    for (std::size_t j = rhs.size(); j-- > 0;) {
      acc[i + j + 1] += (lhs[i] - '0') * (rhs[j] - '0');
    }
  }
  for (std::size_t k = acc.size(); k-- > 1;) {
    acc[k - 1] += acc[k] / 10;
    acc[k] %= 10;
  }
  std::string out;
  out.reserve(acc.size());
  std::size_t start = 0;
  while (start + 1 < acc.size() && acc[start] == 0) {
    ++start;
  }
  for (std::size_t k = start; k < acc.size(); ++k) {
    out.push_back(static_cast<char>('0' + acc[k]));
  }
  return out;
}

/// @brief 长除法，返回商（无前导零），余数写入 `remainder`。
std::string DivAbs(const std::string& dividend, const std::string& divisor,
                   std::string& remainder) {
  std::string quotient;
  remainder.clear();
  for (const char ch : dividend) {
    remainder.push_back(ch);
    const std::size_t first = remainder.find_first_not_of('0');
    if (first != std::string::npos) {
      remainder.erase(0, first);
    } else {
      remainder.clear();
    }
    int digit = 0;
    while (CompareDigits(remainder.empty() ? std::string("0") : remainder, divisor) >= 0) {
      remainder = SubAbs(remainder.empty() ? std::string("0") : remainder, divisor);
      ++digit;
    }
    quotient.push_back(static_cast<char>('0' + digit));
  }
  const std::size_t first = quotient.find_first_not_of('0');
  return first == std::string::npos ? "0" : quotient.substr(first);
}

/// @brief 把原文转成错误消息里的引号片段：最多 32 字节，超出标注总长度。
std::string QuoteForMessage(const std::string& text) {
  constexpr std::size_t kQuoteBytes = 32;
  std::string out;
  out.push_back('"');
  out.append(text, 0, std::min(text.size(), kQuoteBytes));
  if (text.size() > kQuoteBytes) {
    out += "... (";
    out += std::to_string(text.size());
    out += " bytes)";
  }
  out.push_back('"');
  return out;
}

}  // namespace

DecimalError::DecimalError(DecimalFailure failure, std::string text)
    : m_failure(failure), m_text(std::move(text)) {}

std::string DecimalError::Message() const {
  switch (m_failure) {
    case DecimalFailure::kEmpty:
      return "decimal: empty input";
    case DecimalFailure::kTooLong:
      return "decimal: input too long (" + std::to_string(m_text.size()) + " bytes, limit " +
             std::to_string(kMaxInputBytes) + ")";
    case DecimalFailure::kOutOfRange:
      return "decimal: out of range: " + QuoteForMessage(m_text);
    case DecimalFailure::kInvalidChar:
      return "decimal: invalid character: " + QuoteForMessage(m_text);
    case DecimalFailure::kNone:
    default:
      return "decimal: invalid input: " + QuoteForMessage(m_text);
  }
}

DecimalFailure Decimal::ParseImpl(std::string_view text, Decimal& out) {
  if (text.empty()) {
    return DecimalFailure::kEmpty;
  }
  if (text.size() > kMaxInputBytes) {
    return DecimalFailure::kTooLong;
  }
  try {
    std::size_t pos = 0;
    bool negative = false;
    if (text[pos] == '+' || text[pos] == '-') {
      negative = text[pos] == '-';
      ++pos;
    }

    std::string digits;
    digits.reserve(text.size());
    std::int64_t scale = 0;
    bool has_digit = false;

    while (pos < text.size() && IsDigit(text[pos])) {
      digits.push_back(text[pos]);
      ++pos;
      has_digit = true;
    }
    if (pos < text.size() && text[pos] == '.') {
      ++pos;
      while (pos < text.size() && IsDigit(text[pos])) {
        digits.push_back(text[pos]);
        ++scale;
        ++pos;
        has_digit = true;
      }
    }
    if (!has_digit) {
      return DecimalFailure::kInvalidChar;
    }

    std::int64_t exponent = 0;
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
      ++pos;
      bool exponent_negative = false;
      if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        exponent_negative = text[pos] == '-';
        ++pos;
      }
      if (pos >= text.size() || !IsDigit(text[pos])) {
        return DecimalFailure::kInvalidChar;
      }
      std::int64_t value = 0;
      while (pos < text.size() && IsDigit(text[pos])) {
        const int digit = text[pos] - '0';
        // 先判断再乘，避免超长指数把 int64 乘溢出。
        if (value > (kMaxExponent - digit) / 10) {
          return DecimalFailure::kOutOfRange;
        }
        value = value * 10 + digit;
        ++pos;
      }
      exponent = exponent_negative ? -value : value;
    }
    if (pos != text.size()) {
      return DecimalFailure::kInvalidChar;
    }

    const std::size_t first = digits.find_first_not_of('0');
    if (first == std::string::npos) {
      out = Decimal{};
      return DecimalFailure::kNone;
    }
    digits.erase(0, first);

    const std::int64_t wide_scale = scale - exponent;
    // 上界防 scale 溢出 int32，下界防一次补出超大字符串。
    if (wide_scale > kMaxScale || wide_scale < -kMaxScale) {
      return DecimalFailure::kOutOfRange;
    }
    if (wide_scale < 0) {
      digits.append(static_cast<std::size_t>(-wide_scale), '0');
    }
    std::int64_t final_scale = wide_scale < 0 ? 0 : wide_scale;
    while (final_scale > 0 && !digits.empty() && digits.back() == '0') {
      digits.pop_back();
      --final_scale;
    }
    if (digits.empty()) {
      out = Decimal{};
      return DecimalFailure::kNone;
    }

    out.m_negative = negative;
    out.m_digits = std::move(digits);
    out.m_scale = static_cast<std::int32_t>(final_scale);
    return DecimalFailure::kNone;
  } catch (const std::bad_alloc&) {
    // 分配失败与数量级超限对调用方是同一类结果：这个数太大，无法表示。
    return DecimalFailure::kOutOfRange;
  }
}

bool Decimal::Parse(std::string_view text, Decimal& out) {
  return ParseImpl(text, out) == DecimalFailure::kNone;
}

bool Decimal::Parse(std::string_view text, Decimal& out, DecimalFailure& failure) {
  const DecimalFailure result = ParseImpl(text, out);
  failure = result;
  return result == DecimalFailure::kNone;
}

int Decimal::CompareAbs(const Decimal& lhs, const Decimal& rhs) noexcept {
  // 零值的 m_digits 为空，不能参与「对齐后位数」比较：0.01 对齐到 scale=2 后
  // 是 "1"（1 位），零是 "00"（2 位），按位数会误判 0.01 < 0。
  // week1 的 Compare 只在「两边都为零」时短路，所以这是迁移前就存在的缺陷，
  // 这里改成任何一边为零都直接按「零最小」返回。
  if (lhs.IsZero() || rhs.IsZero()) {
    if (lhs.IsZero() && rhs.IsZero()) {
      return 0;
    }
    return lhs.IsZero() ? -1 : 1;
  }
  const std::int32_t scale = std::max(lhs.m_scale, rhs.m_scale);
  const std::size_t lhs_len = lhs.m_digits.size() + static_cast<std::size_t>(scale - lhs.m_scale);
  const std::size_t rhs_len = rhs.m_digits.size() + static_cast<std::size_t>(scale - rhs.m_scale);
  if (lhs_len != rhs_len) {
    return lhs_len < rhs_len ? -1 : 1;
  }
  for (std::size_t i = 0; i < lhs_len; ++i) {
    const char lhs_digit = i < lhs.m_digits.size() ? lhs.m_digits[i] : '0';
    const char rhs_digit = i < rhs.m_digits.size() ? rhs.m_digits[i] : '0';
    if (lhs_digit != rhs_digit) {
      return lhs_digit < rhs_digit ? -1 : 1;
    }
  }
  return 0;
}

int Decimal::Compare(const Decimal& other) const noexcept {
  if (IsZero() && other.IsZero()) {
    return 0;
  }
  if (m_negative != other.m_negative) {
    return m_negative ? -1 : 1;
  }
  const int absolute = CompareAbs(*this, other);
  return m_negative ? -absolute : absolute;
}

std::string Decimal::ToString() const {
  if (IsZero()) {
    return "0";
  }
  std::string result;
  if (m_negative) {
    result.push_back('-');
  }
  const std::size_t digits_size = m_digits.size();
  const std::size_t scale = static_cast<std::size_t>(m_scale);
  const std::size_t integer_len = digits_size > scale ? digits_size - scale : 0;
  if (integer_len == 0) {
    result.push_back('0');
  } else {
    result.append(m_digits, 0, integer_len);
  }
  if (m_scale > 0) {
    result.push_back('.');
    if (integer_len == 0) {
      result.append(scale - digits_size, '0');
      result.append(m_digits);
    } else {
      result.append(m_digits, integer_len, digits_size - integer_len);
    }
  }
  return result;
}

Decimal Decimal::Truncate() const {
  if (m_scale == 0) {
    return *this;
  }
  // 规范化表示保证 scale > 0 时 digits 非空且末位非零，即数值一定带小数部分。
  if (m_digits.size() <= static_cast<std::size_t>(m_scale)) {
    return Decimal{};
  }
  const std::size_t kept = m_digits.size() - static_cast<std::size_t>(m_scale);
  return Make(m_negative, m_digits.substr(0, kept), 0);
}

Decimal Decimal::Floor() const {
  if (m_scale == 0) {
    return *this;
  }
  const Decimal truncated = Truncate();
  if (!m_negative) {
    return truncated;
  }
  // 负数带小数时向负无穷取整，需要让绝对值再加一。
  return Make(true, AddAbs(truncated.IsZero() ? std::string("0") : truncated.m_digits, "1"), 0);
}

Decimal Decimal::Ceil() const {
  if (m_scale == 0) {
    return *this;
  }
  const Decimal truncated = Truncate();
  if (m_negative) {
    return truncated;
  }
  // 正数带小数时向正无穷取整，需要让绝对值再加一。
  return Make(false, AddAbs(truncated.IsZero() ? std::string("0") : truncated.m_digits, "1"), 0);
}

Decimal Decimal::Round(std::int32_t scale) const {
  if (scale < 0) {
    // Decimal 的 scale 不允许为负，负参数按 0 位小数处理。
    scale = 0;
  }
  if (scale >= m_scale || IsZero()) {
    return *this;
  }
  const std::size_t drop = static_cast<std::size_t>(m_scale - scale);
  const std::size_t digits_size = m_digits.size();
  std::string kept;
  // 只看被丢弃部分的首位就够了：首位 >= 5 时被丢弃值已达到半个单位。
  char first_discarded = '0';
  if (digits_size > drop) {
    const std::size_t kept_size = digits_size - drop;
    kept = m_digits.substr(0, kept_size);
    first_discarded = m_digits[kept_size];
  } else if (digits_size == drop) {
    first_discarded = m_digits[0];
  }
  if (first_discarded >= '5') {
    kept = AddAbs(kept.empty() ? std::string("0") : kept, "1");
  }
  return Make(m_negative, std::move(kept), scale);
}

Decimal Decimal::Abs() const {
  Decimal result = *this;
  result.m_negative = false;
  return result;
}

Decimal Decimal::Make(bool negative, std::string digits, std::int32_t scale) {
  const std::size_t first = digits.find_first_not_of('0');
  if (first == std::string::npos) {
    return Decimal{};
  }
  digits.erase(0, first);
  while (scale > 0 && !digits.empty() && digits.back() == '0') {
    digits.pop_back();
    --scale;
  }
  if (digits.empty()) {
    return Decimal{};
  }
  Decimal result;
  result.m_negative = negative;
  result.m_digits = std::move(digits);
  result.m_scale = scale;
  return result;
}

Decimal Decimal::operator-() const {
  Decimal result = *this;
  if (!result.IsZero()) {
    result.m_negative = !result.m_negative;
  }
  return result;
}

Decimal Decimal::operator+(const Decimal& other) const {
  if (IsZero()) {
    return other;
  }
  if (other.IsZero()) {
    return *this;
  }
  const std::int32_t scale = std::max(m_scale, other.m_scale);
  std::string lhs = m_digits;
  lhs.append(static_cast<std::size_t>(scale - m_scale), '0');
  std::string rhs = other.m_digits;
  rhs.append(static_cast<std::size_t>(scale - other.m_scale), '0');

  bool negative = false;
  std::string digits;
  if (m_negative == other.m_negative) {
    digits = AddAbs(lhs, rhs);
    negative = m_negative;
  } else {
    const int cmp = CompareAbs(*this, other);
    if (cmp == 0) {
      return Decimal{};
    }
    if (cmp > 0) {
      digits = SubAbs(lhs, rhs);
      negative = m_negative;
    } else {
      digits = SubAbs(rhs, lhs);
      negative = other.m_negative;
    }
  }
  return Make(negative, std::move(digits), scale);
}

Decimal Decimal::operator-(const Decimal& other) const { return *this + (-other); }

Decimal Decimal::operator*(const Decimal& other) const {
  if (IsZero() || other.IsZero()) {
    return Decimal{};
  }
  return Make(m_negative != other.m_negative, MulAbs(m_digits, other.m_digits),
              m_scale + other.m_scale);
}

Decimal& Decimal::operator+=(const Decimal& other) {
  *this = *this + other;
  return *this;
}

Decimal& Decimal::operator-=(const Decimal& other) {
  *this = *this - other;
  return *this;
}

Decimal& Decimal::operator*=(const Decimal& other) {
  *this = *this * other;
  return *this;
}

bool Decimal::Divide(const Decimal& lhs, const Decimal& rhs, std::int32_t scale, Decimal& out,
                     std::string& error) {
  error.clear();
  if (rhs.IsZero()) {
    error = "decimal division by zero";
    return false;
  }
  if (scale < 0) {
    error = "decimal division scale must be non-negative";
    return false;
  }
  if (scale > kMaxScale) {
    error = "decimal division scale exceeds limit";
    return false;
  }
  if (lhs.IsZero()) {
    out = Decimal{};
    return true;
  }
  const std::int64_t shift = static_cast<std::int64_t>(scale) + rhs.m_scale - lhs.m_scale;
  if (shift > kMaxScale || shift < -kMaxScale) {
    error = "decimal division result exceeds limit";
    return false;
  }
  std::string dividend = lhs.m_digits;
  std::string divisor = rhs.m_digits;
  if (shift >= 0) {
    dividend.append(static_cast<std::size_t>(shift), '0');
  } else {
    divisor.append(static_cast<std::size_t>(-shift), '0');
  }
  if (dividend.size() > kMaxInputBytes || divisor.size() > kMaxInputBytes) {
    error = "decimal division result exceeds limit";
    return false;
  }
  std::string remainder;
  std::string quotient = DivAbs(dividend, divisor, remainder);
  if (!remainder.empty() && CompareDigits(MulAbs(remainder, "2"), divisor) >= 0) {
    quotient = AddAbs(quotient, "1");
  }
  out = Make(lhs.m_negative != rhs.m_negative, std::move(quotient), scale);
  return true;
}

bool Decimal::Divide(const Decimal& lhs, const Decimal& rhs, std::int32_t scale, Decimal& out) {
  if (scale < 0) {
    // 保留 week1 的容错语义：负 scale 视为 0，可预期的失败交给带 error 的重载报告。
    scale = 0;
  }
  std::string error;
  return Divide(lhs, rhs, scale, out, error);
}

}  // namespace go2cpp::json::core
