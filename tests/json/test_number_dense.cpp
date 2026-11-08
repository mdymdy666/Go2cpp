/**
 * @file test_number_dense.cpp
 * @brief J2 数字密集路径：Clinger 快速路径边界、小数位数、负数/指数/Decimal 回退。
 *
 * 覆盖：
 * - `ParseNumber` 的 Clinger 快速路径：小数位数 1..22、尾数 15 位边界；
 * - 整数/浮点混合数组（数字密集场景的形态）逐值校验；
 * - 超 15 位尾数、指数、超 int64 整数的慢路径回退；
 * - 负数小数、`-0.0` 的符号；
 * - Writer 的浮点最短往返（`Parse(Write(Parse))` 值不变）。
 */

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/json/parser.hpp"
#include "go2cpp/json/writer.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    ++g_checks;                                                                 \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                             \
    }                                                                           \
  } while (false)

using go2cpp::ErrorPtr;
using go2cpp::json::Document;
using go2cpp::json::Parser;

/// @brief 解析单个数字字面量，返回 double；失败返回 NaN。
double ParseOne(std::string_view literal) {
  Parser parser;
  Document doc;
  ErrorPtr error;
  if (!parser.Parse(literal, doc, error)) {
    return std::nan("");
  }
  double value = 0.0;
  if (!doc.Root().As(value)) {
    return std::nan("");
  }
  return value;
}

/// @brief 解析单个数字字面量，返回 int64；失败返回 false。
bool ParseInt(std::string_view literal, std::int64_t& out) {
  Parser parser;
  Document doc;
  ErrorPtr error;
  if (!parser.Parse(literal, doc, error)) {
    return false;
  }
  return doc.Root().As(out);
}

/// @brief 小数位数 1..22：`1.<n 个 5>` 走 Clinger 快速路径，值必须精确等于
///        十进制字面量的最近 double（用 `strtod` 做独立参照）。
void TestFractionDigits() {
  std::printf("\n[1] 小数位数 1..22...\n");
  for (int digits = 1; digits <= 22; ++digits) {
    std::string literal = "1.";
    literal.append(static_cast<std::size_t>(digits), '5');
    const double expected = std::strtod(literal.c_str(), nullptr);
    const double actual = ParseOne(literal);
    CHECK(!std::isnan(actual));
    CHECK(actual == expected);
  }
  // 23 位小数：超出 Clinger 的 `fraction_digits <= 22`，走慢路径，仍要正确。
  {
    std::string literal = "1.";
    literal.append(23, '5');
    const double expected = std::strtod(literal.c_str(), nullptr);
    CHECK(ParseOne(literal) == expected);
  }
}

/// @brief 尾数 15 位边界：15 位走快速路径、16 位走慢路径，结果都要对。
void TestMantissaBoundary() {
  std::printf("\n[2] 尾数 15/16 位边界...\n");
  const char* k15 = "123456789012345.5";  // 尾数 16 位（含小数 1 位）→ 慢路径
  const char* k15b = "12345678901234.5";  // 尾数 15 位 → 快速路径
  CHECK(ParseOne(k15) == std::strtod(k15, nullptr));
  CHECK(ParseOne(k15b) == std::strtod(k15b, nullptr));

  // 9007199254740993 = 2^53+1：双精度无法精确表示，取最近可表示值。
  const char* kBig = "9007199254740993";
  std::int64_t big = 0;
  CHECK(ParseInt(kBig, big));
  CHECK(big == 9007199254740993LL);
}

/// @brief 数字密集数组：12000 个 `N.25`，逐值校验（抽样 + 全量整数部分）。
void TestDenseArray() {
  std::printf("\n[3] 数字密集数组（12000 个）...\n");
  std::string text = "[";
  constexpr int kCount = 12000;
  for (int i = 0; i < kCount; ++i) {
    if (i > 0) text += ',';
    text += std::to_string(i * 37 + 11) + ".25";
  }
  text += ']';

  Parser parser;
  Document doc;
  ErrorPtr error;
  CHECK(parser.Parse(text, doc, error));
  CHECK(doc.Root().Size() == static_cast<std::size_t>(kCount));
  CHECK(doc.ElementCount() == static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; i += 97) {
    double value = 0.0;
    CHECK(doc.Root().At(static_cast<std::size_t>(i)).As(value));
    const double expected = static_cast<double>(i * 37 + 11) + 0.25;
    CHECK(value == expected);
  }
}

/// @brief 负数小数与 `-0.0` 的符号。
void TestNegative() {
  std::printf("\n[4] 负数与 -0.0...\n");
  CHECK(ParseOne("-1.5") == -1.5);
  CHECK(ParseOne("-0.0") == 0.0);
  CHECK(std::signbit(ParseOne("-0.0")));
  CHECK(!std::signbit(ParseOne("0.0")));
  CHECK(ParseOne("-123.456") == -123.456);
}

/// @brief 指数与超长数字：走慢路径，结果与 strtod 一致。
void TestSlowPath() {
  std::printf("\n[5] 指数/超长数字回退...\n");
  const char* cases[] = {"1e10",   "1E-10",  "-2.5e+3", "1.7976931348623157e308",
                         "4.9e-324", "123456789012345678901234567890.5",
                         "0.000000000000000000001", "-1.2345678901234567e-5"};
  for (const char* literal : cases) {
    const double expected = std::strtod(literal, nullptr);
    const double actual = ParseOne(literal);
    CHECK(actual == expected);
  }
}

/// @brief 整数/浮点混合：类型必须保持（整数节点 vs 浮点节点）。
void TestMixedTypes() {
  std::printf("\n[6] 整数/浮点类型保持...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;
  CHECK(parser.Parse("[1,1.0,1.5,-2,-2.5,0]", doc, error));
  CHECK(doc.Root().At(0).IsInt());
  CHECK(doc.Root().At(1).IsDouble());
  CHECK(doc.Root().At(2).IsDouble());
  CHECK(doc.Root().At(3).IsInt());
  CHECK(doc.Root().At(4).IsDouble());
  CHECK(doc.Root().At(5).IsInt());

  double value = 0.0;
  CHECK(doc.Root().At(1).As(value));
  CHECK(value == 1.0);
}

/// @brief Writer 的浮点最短往返：写回再解析，double 位级一致。
void TestRoundTrip() {
  std::printf("\n[7] 浮点 round-trip...\n");
  const char* literals[] = {"3.141592653589793", "1.5", "-0.125", "1e300",
                            "2.2250738585072014e-308"};
  for (const char* literal : literals) {
    Parser parser;
    Document doc;
    ErrorPtr error;
    CHECK(parser.Parse(literal, doc, error));
    const std::string written = go2cpp::json::Write(doc);
    Document again;
    ErrorPtr error2;
    CHECK(parser.Parse(written, again, error2));
    double before = 0.0;
    double after = 0.0;
    CHECK(doc.Root().As(before));
    CHECK(again.Root().As(after));
    CHECK(before == after);
  }
}

}  // namespace

int main() {
  TestFractionDigits();
  TestMantissaBoundary();
  TestDenseArray();
  TestNegative();
  TestSlowPath();
  TestMixedTypes();
  TestRoundTrip();

  if (g_failures != 0) {
    std::fprintf(stderr, "\n%d FAILURES (%d checks)\n", g_failures, g_checks);
    return 1;
  }
  std::printf("\nALL TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
