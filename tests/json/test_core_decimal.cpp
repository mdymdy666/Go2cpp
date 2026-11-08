/**
 * @file test_core_decimal.cpp
 * @brief `go2cpp::json::core::Decimal` 的回归测试（迁移自 week1 `csvp::Decimal`）。
 *
 * 覆盖：
 * - 解析：正常 / 负 / 零 / 小数 / 前导零 / 指数 / 超长 / 非法字符 / 数量级超限 / 空串；
 * - 输出与比较：`ToString` 往返、六种比较运算符、跨 scale 比较；
 * - 算术：`+ - *`、`+= -= *=`、一元负号、进位与跨 scale；
 * - 取整：`Floor/Ceil/Truncate/Round`（含「远离零」舍入与负 scale）；
 * - 除法：除零、舍入、scale 边界、失败不修改输出、`error` 文本；
 * - 错误：`DecimalError` 的 failure 原因、`Message` 含原因与原文、可进错误链；
 * - 随机：5 万次随机定点数 `ToString` ↔ `Parse` 往返；2 万组随机整数四则对照；
 * - 真实场景（6 个）：CSV 价格求和、JSON 指数数字、分期对账、取整分箱、
 *   坏字段错误链、超大计数。
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/tools/error.hpp"

namespace {

int g_failures = 0;

/// @brief 断言；失败时打印位置并累计失败数，不中断后续用例。
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                       \
    }                                                                     \
  } while (false)

using go2cpp::json::core::Decimal;
using go2cpp::json::core::DecimalError;
using go2cpp::json::core::DecimalFailure;

/// @brief 解析文本；失败时记录一次失败并返回零，避免用例里到处写临时变量。
Decimal P(std::string_view text) {
  Decimal value;
  if (!Decimal::Parse(text, value)) {
    std::fprintf(stderr, "辅助解析失败: \"%s\"\n", std::string(text).c_str());
    ++g_failures;
  }
  return value;
}

/// @brief 断言 `value.ToString()` 等于 `expected`，失败时打印实际值。
void CheckText(const Decimal& value, const char* expected) {
  const std::string actual = value.ToString();
  if (actual != expected) {
    std::fprintf(stderr, "CHECK failed: ToString()= \"%s\", 期望 \"%s\"\n", actual.c_str(),
                 expected);
    ++g_failures;
  }
}

/// @brief 解析并断言失败原因。
void CheckFailure(std::string_view text, DecimalFailure expected) {
  Decimal value;
  DecimalFailure failure = DecimalFailure::kNone;
  const bool ok = Decimal::Parse(text, value, failure);
  if (ok || failure != expected) {
    std::fprintf(stderr, "CHECK failed: Parse(\"%.40s\") ok=%d failure=%d(期望 %d)\n",
                 std::string(text).c_str(), static_cast<int>(ok), static_cast<int>(failure),
                 static_cast<int>(expected));
    ++g_failures;
  }
}

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

void TestParseBasics() {
  CheckText(P("0"), "0");
  CheckText(P("-0"), "0");
  CheckText(P("+0"), "0");
  CheckText(P("0.000"), "0");
  CheckText(P("000123"), "123");
  CheckText(P("12.50"), "12.5");
  CheckText(P("-12.300"), "-12.3");
  CheckText(P(".5"), "0.5");
  CheckText(P("-.5"), "-0.5");
  CheckText(P("5."), "5");
  CheckText(P("+7"), "7");
  CheckText(P("1e3"), "1000");
  CheckText(P("1E+2"), "100");
  CheckText(P("1.5e-2"), "0.015");
  CheckText(P("-2.5E-3"), "-0.0025");
  CheckText(P("1e0"), "1");
  CheckText(P("0e999"), "0");
  CheckText(P("1.2300e2"), "123");

  const Decimal zero;
  CHECK(zero.IsZero());
  CHECK(!zero.IsNegative());
  CHECK(zero.IsInteger());
  CHECK(zero.ToString() == "0");

  const Decimal negative = P("-0.001");
  CHECK(negative.IsNegative());
  CHECK(!negative.IsZero());
  CHECK(!negative.IsInteger());
  CHECK(P("42").IsInteger());
  CHECK(P("42.0").IsInteger());
  CHECK(!P("42.0001").IsInteger());
}

void TestParseInvalid() {
  CheckFailure("", DecimalFailure::kEmpty);
  CheckFailure("-", DecimalFailure::kInvalidChar);
  CheckFailure("+", DecimalFailure::kInvalidChar);
  CheckFailure(".", DecimalFailure::kInvalidChar);
  CheckFailure("-.", DecimalFailure::kInvalidChar);
  CheckFailure("abc", DecimalFailure::kInvalidChar);
  CheckFailure("12x", DecimalFailure::kInvalidChar);
  CheckFailure("1.2.3", DecimalFailure::kInvalidChar);
  CheckFailure("1e", DecimalFailure::kInvalidChar);
  CheckFailure("1e+", DecimalFailure::kInvalidChar);
  CheckFailure("1e++2", DecimalFailure::kInvalidChar);
  CheckFailure("1e2x", DecimalFailure::kInvalidChar);
  CheckFailure(" 1", DecimalFailure::kInvalidChar);
  CheckFailure("1 ", DecimalFailure::kInvalidChar);
  CheckFailure("0x10", DecimalFailure::kInvalidChar);
  CheckFailure("nan", DecimalFailure::kInvalidChar);
  CheckFailure("inf", DecimalFailure::kInvalidChar);
  CheckFailure("1,000", DecimalFailure::kInvalidChar);
  CheckFailure("--1", DecimalFailure::kInvalidChar);

  // 失败时输出保持原值。
  Decimal unchanged = P("7.25");
  DecimalFailure failure = DecimalFailure::kNone;
  CHECK(!Decimal::Parse("oops", unchanged, failure));
  CHECK(unchanged == P("7.25"));
  CHECK(failure == DecimalFailure::kInvalidChar);
}

void TestParseLimits() {
  // 输入长度上限 1 MiB（1U << 20）：恰好 1 MiB 可以通过长度检查，
  // 多 1 字节即 kTooLong。
  const std::string max_length(1U << 20, '1');
  Decimal value;
  DecimalFailure failure = DecimalFailure::kNone;
  CHECK(Decimal::Parse(max_length, value, failure));
  CHECK(failure == DecimalFailure::kNone);
  CHECK(value.ToString().size() == max_length.size());

  const std::string too_long((1U << 20) + 1, '1');
  CHECK(!Decimal::Parse(too_long, value, failure));
  CHECK(failure == DecimalFailure::kTooLong);

  // 指数绝对值上限 10^6。
  CheckFailure("1e1000001", DecimalFailure::kOutOfRange);
  CheckFailure("1e-1000001", DecimalFailure::kOutOfRange);
  CheckFailure("1e9999999999999999999999", DecimalFailure::kOutOfRange);

  // 指数上限内的极值可以解析（1 后面跟 10^6 个 0 / 小数点后 10^6 位）。
  CHECK(Decimal::Parse("1e1000000", value, failure));
  CHECK(failure == DecimalFailure::kNone);
  CHECK(value.IsInteger());
  CHECK(value.ToString().size() == 1000001U);

  CHECK(Decimal::Parse("1e-1000000", value, failure));
  CHECK(failure == DecimalFailure::kNone);
  CHECK(!value.IsInteger());
  CHECK(value.ToString().size() == 1000002U);  // "0." + 999999 个 0 + "1"

  // 数量级上限 2^20 的触发点：需要很长的 scale 配负指数。
  // 「0.」+ 1 048 565 位数字 + "e-1000000" 总长恰好 1 MiB（不触发 kTooLong），
  // 而 wide_scale = 1048565 + 1000000 = 2048565 > 2^20 → kOutOfRange。
  std::string long_scale = "0.";
  long_scale.append(1048565, '1');
  long_scale += "e-1000000";
  CHECK(long_scale.size() == (1U << 20));
  DecimalFailure scale_failure = DecimalFailure::kNone;
  CHECK(!Decimal::Parse(long_scale, value, scale_failure));
  CHECK(scale_failure == DecimalFailure::kOutOfRange);
}

// ---------------------------------------------------------------------------
// 输出与比较
// ---------------------------------------------------------------------------

void TestToString() {
  CheckText(P("-123.456"), "-123.456");
  CheckText(P("0.0001"), "0.0001");
  CheckText(P("-0.5"), "-0.5");
  CheckText(P("100"), "100");
  CheckText(P("-100.00"), "-100");
  CheckText(P("123456789012345678901234567890"), "123456789012345678901234567890");
  CheckText(P("-0.000000000000000001"), "-0.000000000000000001");
  CHECK(P("0").ToString() == "0");
  CHECK(P("-0").ToString() == "0");
}

void TestCompare() {
  CHECK(P("1.5") == P("1.50"));
  CHECK(P("1.5") != P("1.51"));
  CHECK(P("1.5") < P("1.51"));
  CHECK(P("1.5") <= P("1.50"));
  CHECK(P("1.51") > P("1.5"));
  CHECK(P("1.50") >= P("1.5"));
  CHECK(P("-1.5") < P("-1.4"));
  CHECK(P("-1.5") < P("0"));
  CHECK(P("0") == P("-0"));
  CHECK(P("0.1") > P("0.09"));
  CHECK(P("0.10") == P("0.1"));
  CHECK(P("12345678901234567890") < P("12345678901234567891"));
  CHECK(P("123456789012345678901") > P("12345678901234567890"));
  CHECK(P("0.0000001") < P("0.0000002"));
  CHECK(P("2") == P("2.000"));

  // Compare 三路返回值。
  CHECK(P("1").Compare(P("2")) == -1);
  CHECK(P("2").Compare(P("2.0")) == 0);
  CHECK(P("3").Compare(P("-3")) == 1);
  CHECK(P("0").Compare(P("-0")) == 0);

  // 迁移时修正的缺陷：零值的 digits 为空，曾参与「对齐后位数」比较，
  // 导致绝对值小于 1 的数被判成小于零（0.01 < 0）。这里锁死正确语义。
  CHECK(P("0.01") > Decimal{});
  CHECK(P("0.001") > P("0"));
  CHECK(P("-0.01") < P("0"));
  CHECK(P("0.01") > P("0.001"));
  CHECK(P("0") < P("0.0001"));
  CHECK(P("-0.0001") < P("0"));
  CHECK(P("0.5") > P("0.05"));
}

// ---------------------------------------------------------------------------
// 算术
// ---------------------------------------------------------------------------

void TestArithmetic() {
  CheckText(P("1.5") + P("2.25"), "3.75");
  CheckText(P("0.9") + P("0.1"), "1");
  CheckText(P("1") - P("0.9"), "0.1");
  CheckText(P("-1.5") + P("1.5"), "0");
  CheckText(P("1.5") * P("2"), "3");
  CheckText(P("-1.5") * P("-2"), "3");
  CheckText(P("-1.5") * P("2"), "-3");
  CheckText(P("0.1") * P("0.1"), "0.01");
  CheckText(P("12345678901234567890") * P("10"), "123456789012345678900");
  CheckText(P("1.25") + P("1.25"), "2.5");

  // 加法产生新值，不修改操作数。
  const Decimal lhs = P("10.25");
  const Decimal rhs = P("0.75");
  const Decimal sum = lhs + rhs;
  CheckText(sum, "11");
  CheckText(lhs, "10.25");
  CheckText(rhs, "0.75");

  // 复合赋值。
  Decimal acc = P("1.5");
  acc += P("2.5");
  CheckText(acc, "4");
  acc -= P("0.5");
  CheckText(acc, "3.5");
  acc *= P("2");
  CheckText(acc, "7");

  // 一元负号：零值不产生 -0。
  CheckText(-P("1.5"), "-1.5");
  CheckText(-P("-1.5"), "1.5");
  CheckText(-P("0"), "0");
  CHECK(!(-P("0")).IsNegative());
}

// ---------------------------------------------------------------------------
// 取整
// ---------------------------------------------------------------------------

void TestRounding() {
  CheckText(P("1.5").Floor(), "1");
  CheckText(P("1.5").Ceil(), "2");
  CheckText(P("1.5").Truncate(), "1");
  CheckText(P("1.5").Round(0), "2");

  CheckText(P("-1.5").Floor(), "-2");
  CheckText(P("-1.5").Ceil(), "-1");
  CheckText(P("-1.5").Truncate(), "-1");
  CheckText(P("-1.5").Round(0), "-2");  // 远离零

  CheckText(P("0.5").Round(0), "1");
  CheckText(P("-0.5").Round(0), "-1");
  CheckText(P("2.5").Round(0), "3");
  CheckText(P("1.25").Round(1), "1.3");
  CheckText(P("-1.25").Round(1), "-1.3");
  CheckText(P("1.24").Round(1), "1.2");
  CheckText(P("1.999").Round(2), "2");
  CheckText(P("999.99").Round(1), "1000");
  CheckText(P("123.456").Round(2), "123.46");

  // scale 大于当前小数位：原样返回，不补零。
  CheckText(P("1.5").Round(5), "1.5");
  // 负 scale 按 0 处理。
  CheckText(P("1.5").Round(-3), "2");

  // 整数取整是空操作。
  CheckText(P("42").Floor(), "42");
  CheckText(P("42").Ceil(), "42");
  CheckText(P("42").Truncate(), "42");
  CheckText(P("0").Floor(), "0");
  CheckText(P("0").Ceil(), "0");

  // 远离零方向的近零值。
  CheckText(P("0.0001").Floor(), "0");
  CheckText(P("0.0001").Ceil(), "1");
  CheckText(P("-0.0001").Floor(), "-1");
  CheckText(P("-0.0001").Ceil(), "0");
  CheckText(P("-0.0001").Truncate(), "0");
}

// ---------------------------------------------------------------------------
// 除法
// ---------------------------------------------------------------------------

void TestDivide() {
  Decimal out;
  CHECK(Decimal::Divide(P("10"), P("4"), 2, out));
  CheckText(out, "2.5");
  CHECK(out.IsInteger() == false);

  CHECK(Decimal::Divide(P("1"), P("3"), 5, out));
  CheckText(out, "0.33333");

  CHECK(Decimal::Divide(P("2"), P("3"), 5, out));
  CheckText(out, "0.66667");

  CHECK(Decimal::Divide(P("1"), P("3"), 0, out));
  CheckText(out, "0");

  CHECK(Decimal::Divide(P("2"), P("3"), 0, out));
  CheckText(out, "1");

  // 0.125 保留两位 → 0.13（远离零）。
  CHECK(Decimal::Divide(P("1"), P("8"), 2, out));
  CheckText(out, "0.13");
  CHECK(Decimal::Divide(P("-1"), P("8"), 2, out));
  CheckText(out, "-0.13");

  // 1/7 = 0.142857142857... 保留 10 位 → 第 11 位是 5，进位。
  CHECK(Decimal::Divide(P("1"), P("7"), 10, out));
  CheckText(out, "0.1428571429");

  // 被除数为零。
  CHECK(Decimal::Divide(P("0"), P("5"), 3, out));
  CheckText(out, "0");

  // 除零：失败且输出不变。
  out = P("42.5");
  std::string error;
  CHECK(!Decimal::Divide(P("1"), P("0"), 2, out, error));
  CHECK(!error.empty());
  CheckText(out, "42.5");

  // 负 scale：宽松重载按 0 处理。
  CHECK(Decimal::Divide(P("7"), P("2"), -3, out));
  CheckText(out, "4");  // 3.5 → 远离零 → 4

  // 带 error 的重载把负 scale 视为非法。
  error.clear();
  CHECK(!Decimal::Divide(P("7"), P("2"), -3, out, error));
  CHECK(!error.empty());
  CheckText(out, "4");  // 仍保持上一次成功的结果

  // scale 超上限（2^20）。
  error.clear();
  CHECK(!Decimal::Divide(P("1"), P("3"), (1 << 20) + 1, out, error));
  CHECK(!error.empty());

  // 大数除法。
  CHECK(Decimal::Divide(P("12345678901234567890"), P("10"), 0, out));
  CheckText(out, "1234567890123456789");

  // 小数除小数。
  CHECK(Decimal::Divide(P("0.25"), P("0.5"), 4, out));
  CheckText(out, "0.5");
}

// ---------------------------------------------------------------------------
// 错误对象
// ---------------------------------------------------------------------------

void TestDecimalError() {
  Decimal value;
  DecimalFailure failure = DecimalFailure::kNone;

  CHECK(!Decimal::Parse("", value, failure));
  CHECK(failure == DecimalFailure::kEmpty);
  const auto empty_error = std::make_shared<DecimalError>(failure, "");
  CHECK(empty_error->Failure() == DecimalFailure::kEmpty);
  CHECK(empty_error->Message().find("empty") != std::string::npos);
  CHECK(empty_error->Text().empty());

  CHECK(!Decimal::Parse("12x3", value, failure));
  CHECK(failure == DecimalFailure::kInvalidChar);
  const auto invalid = std::make_shared<DecimalError>(failure, "12x3");
  CHECK(invalid->Failure() == DecimalFailure::kInvalidChar);
  CHECK(invalid->Message().find("invalid") != std::string::npos);
  CHECK(invalid->Message().find("12x3") != std::string::npos);
  CHECK(invalid->Text() == "12x3");

  CHECK(!Decimal::Parse("1e1000001", value, failure));
  CHECK(failure == DecimalFailure::kOutOfRange);
  const auto out_of_range = std::make_shared<DecimalError>(failure, "1e1000001");
  CHECK(out_of_range->Failure() == DecimalFailure::kOutOfRange);
  CHECK(out_of_range->Message().find("out of range") != std::string::npos);
  CHECK(out_of_range->Message().find("1e1000001") != std::string::npos);

  const std::string too_long((1U << 20) + 1, '7');
  CHECK(!Decimal::Parse(too_long, value, failure));
  CHECK(failure == DecimalFailure::kTooLong);
  const auto long_error = std::make_shared<DecimalError>(failure, too_long);
  CHECK(long_error->Failure() == DecimalFailure::kTooLong);
  CHECK(long_error->Message().find("too long") != std::string::npos);
  CHECK(long_error->Message().find("1048577") != std::string::npos);
  // 原文过长时必须截断，否则错误日志会被 1 MiB 的坏字段撑爆。
  CHECK(long_error->Message().size() < 200);
  CHECK(long_error->Text().size() == too_long.size());

  // 继承 go2cpp::Error：可以直接放进错误链。
  const go2cpp::ErrorPtr base = invalid;
  CHECK(go2cpp::ErrorMessage(base) == invalid->Message());
  CHECK(go2cpp::ErrorMessage(extened::Wrap(base, "字段 price")) ==
        "字段 price: " + invalid->Message());
  CHECK(extened::Unwrap(base) == nullptr);
  CHECK(extened::UnwrapAll(base).empty());

  // 错误链上仍能按地址找到这个节点。
  CHECK(extened::Is(base, base));
}

// ---------------------------------------------------------------------------
// 随机测试
// ---------------------------------------------------------------------------

/// @brief 5 万次随机定点数：ToString → Parse 必须得到同一个值与同一份文本。
void TestRandomRoundTrip() {
  std::mt19937_64 rng(20261106ULL);
  for (int iteration = 0; iteration < 50000; ++iteration) {
    const int digit_count = 1 + static_cast<int>(rng() % 18);
    std::string digits;
    digits.reserve(static_cast<std::size_t>(digit_count));
    for (int index = 0; index < digit_count; ++index) {
      digits.push_back(static_cast<char>('0' + rng() % 10));
    }
    digits[0] = static_cast<char>('1' + rng() % 9);  // 保证非零
    const int scale = static_cast<int>(rng() % 12);

    std::string text;
    if ((rng() & 1U) != 0U) {
      text.push_back('-');
    }
    if (scale == 0) {
      text += digits;
    } else if (digits.size() > static_cast<std::size_t>(scale)) {
      const std::size_t integer_len = digits.size() - static_cast<std::size_t>(scale);
      text.append(digits, 0, integer_len);
      text.push_back('.');
      text.append(digits, integer_len, static_cast<std::size_t>(scale));
    } else {
      text += "0.";
      text.append(static_cast<std::size_t>(scale) - digits.size(), '0');
      text += digits;
    }

    Decimal value;
    if (!Decimal::Parse(text, value)) {
      std::fprintf(stderr, "随机往返：解析失败 \"%s\"\n", text.c_str());
      ++g_failures;
      continue;
    }
    const std::string rendered = value.ToString();
    Decimal reparsed;
    if (!Decimal::Parse(rendered, reparsed)) {
      std::fprintf(stderr, "随机往返：回读失败 \"%s\"\n", rendered.c_str());
      ++g_failures;
      continue;
    }
    if (!(value == reparsed) || reparsed.ToString() != rendered) {
      std::fprintf(stderr, "随机往返：不一致 \"%s\" vs \"%s\"\n", rendered.c_str(),
                   reparsed.ToString().c_str());
      ++g_failures;
    }
  }
}

/// @brief 2 万组随机整数：`+ - *` 的结果必须与 int64 精确运算一致。
void TestRandomIntegerArithmetic() {
  std::mt19937_64 rng(0xC0FFEEULL);
  constexpr std::int64_t kRange = 1000000000;  // ±1e9，乘积分不超过 1e18
  for (int iteration = 0; iteration < 20000; ++iteration) {
    const std::int64_t a =
        static_cast<std::int64_t>(rng() % static_cast<unsigned long long>(2 * kRange + 1)) - kRange;
    const std::int64_t b =
        static_cast<std::int64_t>(rng() % static_cast<unsigned long long>(2 * kRange + 1)) - kRange;

    Decimal lhs;
    Decimal rhs;
    if (!Decimal::Parse(std::to_string(a), lhs) || !Decimal::Parse(std::to_string(b), rhs)) {
      std::fprintf(stderr, "随机算术：解析失败 a=%lld b=%lld\n", static_cast<long long>(a),
                   static_cast<long long>(b));
      ++g_failures;
      continue;
    }
    const std::string sum = (lhs + rhs).ToString();
    const std::string difference = (lhs - rhs).ToString();
    const std::string product = (lhs * rhs).ToString();
    if (sum != std::to_string(a + b)) {
      std::fprintf(stderr, "随机算术：加法不一致 %s vs %s\n", sum.c_str(),
                   std::to_string(a + b).c_str());
      ++g_failures;
    }
    if (difference != std::to_string(a - b)) {
      std::fprintf(stderr, "随机算术：减法不一致 %s vs %s\n", difference.c_str(),
                   std::to_string(a - b).c_str());
      ++g_failures;
    }
    if (product != std::to_string(a * b)) {
      std::fprintf(stderr, "随机算术：乘法不一致 %s vs %s\n", product.c_str(),
                   std::to_string(a * b).c_str());
      ++g_failures;
    }
    if ((lhs.Compare(rhs) < 0) != (a < b)) {
      std::fprintf(stderr, "随机算术：比较不一致 a=%lld b=%lld\n", static_cast<long long>(a),
                   static_cast<long long>(b));
      ++g_failures;
    }
  }
}

/// @brief 2 万次随机除法：结果必须等于「整数算术精确构造的四舍五入期望值」。
void TestRandomDivision() {
  std::mt19937_64 rng(0x5EEDULL);
  for (int iteration = 0; iteration < 20000; ++iteration) {
    const std::int64_t a = static_cast<std::int64_t>(rng() % 1000000);
    const std::int64_t b = static_cast<std::int64_t>(rng() % 1000000) + 1;  // 非零
    const int scale = static_cast<int>(rng() % 9);

    Decimal lhs;
    Decimal rhs;
    Decimal quotient;
    if (!Decimal::Parse(std::to_string(a), lhs) || !Decimal::Parse(std::to_string(b), rhs) ||
        !Decimal::Divide(lhs, rhs, scale, quotient)) {
      std::fprintf(stderr, "随机除法：失败 a=%lld b=%lld scale=%d\n", static_cast<long long>(a),
                   static_cast<long long>(b), scale);
      ++g_failures;
      continue;
    }

    // 期望值：round(a * 10^scale / b)，正数「远离零」即 half-up。
    // 用整数算，避免浮点在 1e14 量级上引入误差；a<1e6、scale<=8，乘积 < 1e14。
    std::int64_t unit = 1;
    for (int index = 0; index < scale; ++index) {
      unit *= 10;
    }
    const std::int64_t numerator = a * unit;
    const std::int64_t scaled = (2 * numerator + b) / (2 * b);

    std::string expected_text = std::to_string(scaled);
    if (scale > 0) {
      const auto digits_after_point = static_cast<std::size_t>(scale);
      if (expected_text.size() <= digits_after_point) {
        expected_text.insert(0, digits_after_point + 1 - expected_text.size(), '0');
      }
      expected_text.insert(expected_text.size() - digits_after_point, 1, '.');
    }
    Decimal expected;
    if (!Decimal::Parse(expected_text, expected)) {
      std::fprintf(stderr, "随机除法：期望值构造失败 \"%s\"\n", expected_text.c_str());
      ++g_failures;
      continue;
    }
    if (quotient != expected) {
      std::fprintf(stderr, "随机除法：结果不一致 a=%lld b=%lld scale=%d 得到 %s 期望 %s\n",
                   static_cast<long long>(a), static_cast<long long>(b), scale,
                   quotient.ToString().c_str(), expected.ToString().c_str());
      ++g_failures;
    }
  }
}

// ---------------------------------------------------------------------------
// 真实场景
// ---------------------------------------------------------------------------

/// @brief 场景 1：CSV 价格列求和——定点数必须精确，不能出现 110.43999999。
void ScenarioCsvPriceSum() {
  const char* prices[] = {"12.50", "0.99", "-3.05", "100", "0.01"};
  Decimal total;
  for (const char* price : prices) {
    Decimal value;
    CHECK(Decimal::Parse(price, value));
    total += value;
  }
  CheckText(total, "110.45");

  // 用 double 做同一件事会得到 110.45000000000002 之类的尾巴。
  double floating = 0.0;
  for (const char* price : prices) {
    floating += std::strtod(price, nullptr);
  }
  CHECK(floating > 110.0);  // 只是确认参照物存在，不比较精确值
}

/// @brief 场景 2：JSON 数字字段——指数形式与小数点混用。
void ScenarioJsonNumbers() {
  const std::pair<const char*, const char*> cases[] = {
      {"1.5e2", "150"},      {"-2.5E-3", "-0.0025"}, {"0", "0"},
      {"1e-7", "0.0000001"}, {"9.999e3", "9999"},
  };
  for (const auto& item : cases) {
    Decimal value;
    CHECK(Decimal::Parse(item.first, value));
    CheckText(value, item.second);
  }
}

/// @brief 场景 3：财务分期——总额三等分，余数差额必须可枚举。
void ScenarioInstallment() {
  Decimal total;
  CHECK(Decimal::Parse("100.00", total));
  Decimal each;
  CHECK(Decimal::Divide(total, P("3"), 2, each));
  CheckText(each, "33.33");
  Decimal rounded_total = each * P("3");
  CheckText(rounded_total, "99.99");
  Decimal remainder = total - rounded_total;
  CheckText(remainder, "0.01");
  CHECK(remainder > Decimal{});
}

/// @brief 场景 4：耗时/耗时统计的分箱——Floor/Ceil/Truncate 语义各不相同。
void ScenarioBucketing() {
  const Decimal elapsed = P("12.7");
  CheckText(elapsed.Floor(), "12");
  CheckText(elapsed.Ceil(), "13");
  CheckText(elapsed.Truncate(), "12");

  const Decimal negative = P("-12.7");
  CheckText(negative.Floor(), "-13");
  CheckText(negative.Ceil(), "-12");
  CheckText(negative.Truncate(), "-12");

  // 分箱结果：每 5 秒一箱。
  Decimal bucket;
  CHECK(Decimal::Divide(elapsed, P("5"), 0, bucket));
  CheckText(bucket, "3");  // 12.7/5 = 2.54 → 保留 0 位 → 3
}

/// @brief 场景 5：坏字段汇聚成错误链——每条都带失败原因与原文。
void ScenarioFieldErrorChain() {
  const char* fields[] = {"", "12x3", "1e1000001"};
  std::vector<go2cpp::ErrorPtr> causes;
  for (const char* text : fields) {
    Decimal value;
    DecimalFailure failure = DecimalFailure::kNone;
    CHECK(!Decimal::Parse(text, value, failure));
    causes.push_back(std::make_shared<DecimalError>(failure, text));
  }
  const go2cpp::ErrorPtr joined = extened::Join(causes);
  const std::string message = go2cpp::ErrorMessage(joined);
  CHECK(message.find("empty") != std::string::npos);
  CHECK(message.find("12x3") != std::string::npos);
  CHECK(message.find("out of range") != std::string::npos);
  CHECK(message.find('\n') != std::string::npos);

  // 包装后仍可判断「是不是 DecimalError」。
  const go2cpp::ErrorPtr wrapped = extened::Wrap(joined, "导入 CSV 第 3 行");
  CHECK(go2cpp::ErrorMessage(wrapped).find("导入 CSV 第 3 行") == 0);
}

/// @brief 场景 6：超出 int64 的计数（例如日志条数）仍然精确。
void ScenarioBigCounter() {
  Decimal counter;
  CHECK(Decimal::Parse("18446744073709551616", counter));  // 2^64，超 uint64 上限
  CheckText(counter, "18446744073709551616");
  counter += P("1");
  CheckText(counter, "18446744073709551617");
  counter *= P("1000");
  CheckText(counter, "18446744073709551617000");
  CHECK(counter > P("18446744073709551616"));
}

}  // namespace

int main() {
  const auto begin = std::chrono::steady_clock::now();

  TestParseBasics();
  TestParseInvalid();
  TestParseLimits();
  TestToString();
  TestCompare();
  TestArithmetic();
  TestRounding();
  TestDivide();
  TestDecimalError();
  TestRandomRoundTrip();
  TestRandomIntegerArithmetic();
  TestRandomDivision();
  ScenarioCsvPriceSum();
  ScenarioJsonNumbers();
  ScenarioInstallment();
  ScenarioBucketing();
  ScenarioFieldErrorChain();
  ScenarioBigCounter();

  const auto end = std::chrono::steady_clock::now();
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();

  if (g_failures != 0) {
    std::fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::printf("ALL TESTS PASSED (%lld ms)\n", static_cast<long long>(elapsed_ms));
  return 0;
}
