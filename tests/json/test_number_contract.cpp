/**
 * @file test_number_contract.cpp
 * @brief JSON 数值类型契约：`double` / `Decimal` 的边界、位级往返与失败语义。
 *
 * 覆盖（契约见 `docs/json-number-contract.md`）：
 * - `double` 边界：`0.0` / `-0.0` / `1.0` / `-1.0` / `100.0` / `0.1` / `1/3` /
 *   `DBL_MAX` / `DBL_MIN` / denormal min / `1e±300` / `1e20` / `1e-5`，
 *   逐项断言输出形态 + `parse(build(x))` **位级相等** + 节点类型仍是 double；
 * - 整值 double 必须补 `.0`（`-200..200` 全量）；
 * - 随机 **100 万** double 位模式（`mt19937_64`，跳过 NaN/Inf）位级往返；
 * - 前文干扰回归：先写含 `.` / `e` / `E` 的字符串或浮点，后续整值 double
 *   仍必须输出 `.0`（`JsonBuilder::Double` 曾因此漏 `.0`）；
 * - `NaN` / `±Inf` 必须报错（`ok() == false` + `error()` 非空 + 一个字节都不写），
 *   不能输出 `nan` / `inf` / `null`；
 * - `Decimal`：规范化、`Decimal → string → parse → Decimal` 值相等（含小数位）、
 *   随机 1000 组（小数位 0..30）、0 位 / 最大数量级边界（`1e±1000000`）；
 * - `writer.hpp`：`Write(Value)` 对 double / Decimal 的同样契约（含 1000 组
 *   位模式经 `to_chars → parse → Write → parse` 的位级往返）。
 */

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/json/document.hpp"
#include "go2cpp/json/json_build.hpp"
#include "go2cpp/json/parser.hpp"
#include "go2cpp/json/value.hpp"
#include "go2cpp/json/writer.hpp"

namespace {

int g_failures = 0;
std::size_t g_checks = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    ++g_checks;                                                                 \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                             \
    }                                                                           \
  } while (false)

using go2cpp::ErrorPtr;
using go2cpp::json::core::Decimal;
using go2cpp::json::core::DecimalFailure;
using go2cpp::json::Document;
using go2cpp::json::JsonBuilder;
using go2cpp::json::Parser;
using go2cpp::json::Value;
using go2cpp::json::Write;

/// @brief 按位比较 double（`==` 会把 `-0.0` 与 `0.0` 视为相等，不够严格）。
std::uint64_t BitsOf(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

bool SameBits(double lhs, double rhs) { return BitsOf(lhs) == BitsOf(rhs); }

/// @brief 把 double 经 `JsonBuilder` 写成 JSON 数字文本（顶层值）。
std::string BuildDouble(double value) {
  JsonBuilder builder;
  builder.Double(value);
  return builder.str();
}

/// @brief 解析单个 JSON 数字，要求根节点是 double。
bool ParseDouble(std::string_view text, double* out) {
  Parser parser;
  Document document;
  ErrorPtr error;
  if (!parser.Parse(text, document, error) || !document.Root().IsDouble()) {
    return false;
  }
  *out = document.Root().As<double>();
  return true;
}

/// @brief 解析单个 JSON 数字，要求根节点是 Decimal。
bool ParseDecimalNode(std::string_view text, Decimal* out) {
  Parser parser;
  Document document;
  ErrorPtr error;
  if (!parser.Parse(text, document, error) || !document.Root().IsDecimal()) {
    return false;
  }
  return document.Root().As(*out);
}

/// @brief 解析单个 JSON 数字，返回节点种类；用于验证 Decimal 文本是合法数字。
enum class NumberKind { kInvalid, kInt, kDouble, kDecimal };

NumberKind ParseNumberKind(std::string_view text) {
  Parser parser;
  Document document;
  ErrorPtr error;
  if (!parser.Parse(text, document, error)) {
    return NumberKind::kInvalid;
  }
  const Value root = document.Root();
  if (root.IsInt()) {
    return NumberKind::kInt;
  }
  if (root.IsDouble()) {
    return NumberKind::kDouble;
  }
  if (root.IsDecimal()) {
    return NumberKind::kDecimal;
  }
  return NumberKind::kInvalid;
}

// ---------------------------------------------------------------------------
// 1. double 边界：输出形态 + 位级往返
// ---------------------------------------------------------------------------

void TestDoubleBoundaries() {
  std::printf("\n[1] double 边界：输出形态、位级往返、类型保持...\n");

  struct Case {
    double value;
    const char* exact;  ///< 非空 = 精确文本断言；nullptr = 只做结构断言。
  };
  const Case cases[] = {
      {0.0, "0.0"},
      {-0.0, "-0.0"},
      {1.0, "1.0"},
      {-1.0, "-1.0"},
      {2.0, "2.0"},
      {100.0, "100.0"},
      {0.1, "0.1"},
      {1.0 / 3.0, "0.3333333333333333"},
      {1e-5, nullptr},
      {1e20, nullptr},
      {1e300, nullptr},
      {1e-300, nullptr},
      {std::numeric_limits<double>::max(), nullptr},
      {std::numeric_limits<double>::min(), nullptr},
      {std::numeric_limits<double>::denorm_min(), nullptr},
  };
  for (const Case& item : cases) {
    const std::string text = BuildDouble(item.value);
    CHECK(!text.empty());
    // 必须是「浮点形态」：含小数点或指数，否则重新解析会变成 int 节点。
    CHECK(text.find_first_of(".eE") != std::string::npos);
    if (item.exact != nullptr) {
      CHECK(text == item.exact);
    }
    double back = 0.0;
    CHECK(ParseDouble(text, &back));
    CHECK(SameBits(back, item.value));
  }

  // 整值 double 全量：-200..200 每个都必须以 ".0" 结尾且位级往返。
  for (int value = -200; value <= 200; ++value) {
    const double number = static_cast<double>(value);
    const std::string text = BuildDouble(number);
    CHECK(text.size() >= 2);
    CHECK(text.compare(text.size() - 2, 2, ".0") == 0);
    double back = 0.0;
    CHECK(ParseDouble(text, &back));
    CHECK(SameBits(back, number));
  }

  // 指数形式合法但不强求大小写/加号：只要求能往返。
  const std::string scientific = BuildDouble(1e300);
  CHECK(scientific.find_first_of("eE") != std::string::npos);
  double scientific_back = 0.0;
  CHECK(ParseDouble(scientific, &scientific_back));
  CHECK(SameBits(scientific_back, 1e300));
}

// ---------------------------------------------------------------------------
// 2. 随机 100 万 double 位模式：位级往返
// ---------------------------------------------------------------------------

void TestRandomBitPatterns() {
  std::printf("\n[2] 随机 100 万 double 位模式：位级往返...\n");

  constexpr std::size_t kIterations = 1000000;
  std::mt19937_64 engine(20261108ULL);
  Parser parser;
  Document document;
  ErrorPtr error;
  std::size_t checked = 0;
  std::size_t skipped = 0;
  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    const std::uint64_t bits = engine();
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) {
      ++skipped;
      continue;
    }
    JsonBuilder builder;
    builder.Double(value);
    const std::string text = builder.Take();
    if (!builder.ok()) {
      if (g_failures < 5) {
        std::fprintf(stderr, "FAIL builder 拒绝有限值: bits=0x%016llx\n",
                     static_cast<unsigned long long>(bits));
      }
      ++g_checks;
      ++g_failures;
      break;
    }
    error.reset();
    const bool parsed =
        parser.Parse(text, document, error) && document.Root().IsDouble();
    if (!parsed || !SameBits(document.Root().As<double>(), value)) {
      if (g_failures < 5) {
        std::fprintf(stderr, "FAIL 位级往返: bits=0x%016llx text=%s\n",
                     static_cast<unsigned long long>(bits), text.c_str());
      }
      ++g_checks;
      ++g_failures;
      break;
    }
    ++g_checks;
    ++checked;
  }
  CHECK(checked + skipped == kIterations);
  CHECK(skipped > 0);       // 生成器确实抽到过 NaN/Inf 并跳过
  CHECK(checked > 990000);  // 绝大多数位模式是有限值
  std::printf("  有限值 %zu，跳过 NaN/Inf %zu\n", checked, skipped);
}

// ---------------------------------------------------------------------------
// 3. 前文干扰回归
// ---------------------------------------------------------------------------

void TestPrefixInterference() {
  std::printf("\n[3] 前文含 . / e / E 的干扰回归...\n");

  {
    JsonBuilder builder;
    builder.BeginArray();
    builder.String("a.b");
    builder.Double(1.0);
    builder.EndArray();
    CHECK(builder.ok());
    CHECK(builder.str() == "[\"a.b\",1.0]");
  }
  {
    JsonBuilder builder;
    builder.BeginObject();
    builder.Key("a.b");
    builder.Double(1.0);
    builder.EndObject();
    CHECK(builder.str() == "{\"a.b\":1.0}");
  }
  {
    JsonBuilder builder;
    builder.BeginArray();
    builder.String("1e5");
    builder.String("E");
    builder.Double(2.0);
    builder.EndArray();
    CHECK(builder.str() == "[\"1e5\",\"E\",2.0]");
  }
  {
    JsonBuilder builder;
    builder.BeginArray();
    builder.Double(1.5);
    builder.Double(2.0);
    builder.EndArray();
    CHECK(builder.str() == "[1.5,2.0]");
  }
  {
    JsonBuilder builder;
    builder.BeginArray();
    builder.Double(1e300);
    builder.Double(3.0);
    builder.EndArray();
    CHECK(builder.str().size() > 6);
    CHECK(builder.str().compare(builder.str().size() - 4, 4, "3.0]") == 0);
  }
  {
    JsonBuilder builder;
    builder.BeginArray();
    builder.Int(100);
    builder.Double(4.0);
    builder.EndArray();
    CHECK(builder.str() == "[100,4.0]");
  }
  {
    JsonBuilder builder;
    builder.BeginArray();
    builder.String("nan");
    builder.String("inf");
    builder.Double(5.0);
    builder.EndArray();
    CHECK(builder.str() == "[\"nan\",\"inf\",5.0]");
  }
  {
    JsonBuilder builder;
    builder.BeginArray();
    builder.BeginArray();
    builder.BeginArray();
    builder.String("a.b");
    builder.EndArray();
    builder.Double(6.0);
    builder.EndArray();
    builder.EndArray();
    CHECK(builder.str() == "[[[\"a.b\"],6.0]]");
  }

  // 解析回验证：干扰后的数值节点仍是 double，值 1.0。
  const std::string text = BuildDouble(1.0);
  double back = 0.0;
  CHECK(ParseDouble(text, &back));
  CHECK(SameBits(back, 1.0));
}

// ---------------------------------------------------------------------------
// 4. NaN / Inf 必须报错
// ---------------------------------------------------------------------------

void TestNonFiniteRejected() {
  std::printf("\n[4] NaN / Inf：必须报错，不能静默输出...\n");

  const double non_finite[] = {
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
  };
  for (const double value : non_finite) {
    JsonBuilder builder;
    builder.Double(value);
    CHECK(!builder.ok());
    CHECK(!builder.error().empty());
    CHECK(builder.str().empty());  // 一个字节都不写，更不能写 nan/inf/null
    CHECK(builder.str().find("nan") == std::string::npos);
    CHECK(builder.str().find("inf") == std::string::npos);
    CHECK(builder.str().find("null") == std::string::npos);
  }

  {
    JsonBuilder builder;
    builder.BeginObject();
    builder.Key("x");
    builder.Double(std::numeric_limits<double>::infinity());
    CHECK(!builder.ok());
    CHECK(builder.str() == "{\"x\":");
    // 进入失败态后，后续写入全部 no-op。
    builder.Key("y").Int(1);
    builder.EndObject();
    CHECK(builder.str() == "{\"x\":");
    CHECK(builder.error() == "JsonBuilder::Double: NaN/Infinity is not valid JSON");
  }

  {
    JsonBuilder builder;
    builder.BeginArray();
    builder.Double(std::numeric_limits<double>::quiet_NaN());
    builder.Int(7);
    builder.EndArray();
    CHECK(!builder.ok());
    CHECK(builder.str() == "[");
    const std::string partial = builder.Take();
    CHECK(partial.find("nan") == std::string::npos);
    CHECK(partial.find("NaN") == std::string::npos);
    CHECK(partial.find("inf") == std::string::npos);
    CHECK(partial.find("null") == std::string::npos);
  }

  // 正常路径：ok() 为 true、error() 为空。
  JsonBuilder healthy;
  healthy.BeginArray().Double(1.0).EndArray();
  CHECK(healthy.ok());
  CHECK(healthy.error().empty());
}

// ---------------------------------------------------------------------------
// 5. Decimal 契约
// ---------------------------------------------------------------------------

void TestDecimalContract() {
  std::printf("\n[5] Decimal：规范化、往返、边界...\n");

  struct Case {
    const char* input;
    const char* canonical;
  };
  const Case cases[] = {
      {"0", "0"},
      {"-0", "0"},
      {"0.000", "0"},
      {"1", "1"},
      {"-1", "-1"},
      {"1.5", "1.5"},
      {"-123.456", "-123.456"},
      {"0.0001", "0.0001"},
      {"123.4500", "123.45"},
      {"1e3", "1000"},
      {"1e-3", "0.001"},
      {"1.5e2", "150"},
      {"18446744073709551616", "18446744073709551616"},
      {"-123456789012345678901234567890", "-123456789012345678901234567890"},
  };
  for (const Case& item : cases) {
    Decimal value;
    CHECK(Decimal::Parse(item.input, value));
    CHECK(value.ToString() == item.canonical);
    Decimal reparsed;
    CHECK(Decimal::Parse(item.canonical, reparsed));
    CHECK(value == reparsed);
    CHECK(reparsed.ToString() == item.canonical);
  }

  // 小数位数为 0：整数形态。
  {
    Decimal integer;
    CHECK(Decimal::Parse("123", integer));
    CHECK(integer.IsInteger());
    CHECK(integer.ToString() == "123");
    CHECK(!integer.IsNegative());
  }

  // 最大数量级边界：指数上限 10^6（`1e-1000000` 成功，`1e-1000001` 失败）。
  {
    Decimal value;
    DecimalFailure failure = DecimalFailure::kNone;
    CHECK(Decimal::Parse("1e-1000000", value, failure));
    CHECK(failure == DecimalFailure::kNone);
    CHECK(!value.IsZero());
    const std::string rendered = value.ToString();
    CHECK(rendered.size() == 1000002);
    CHECK(rendered.compare(0, 2, "0.") == 0);
    CHECK(rendered.back() == '1');
    Decimal reparsed;
    CHECK(Decimal::Parse(rendered, reparsed));
    CHECK(value == reparsed);
    CHECK(reparsed.ToString() == rendered);
  }
  {
    Decimal value;
    DecimalFailure failure = DecimalFailure::kNone;
    CHECK(!Decimal::Parse("1e-1000001", value, failure));
    CHECK(failure == DecimalFailure::kOutOfRange);
  }
  {
    Decimal value;
    CHECK(Decimal::Parse("1e1000000", value));
    const std::string rendered = value.ToString();
    CHECK(rendered.size() == 1000001);
    CHECK(rendered.front() == '1');
    CHECK(rendered.back() == '0');
    Decimal reparsed;
    CHECK(Decimal::Parse(rendered, reparsed));
    CHECK(value == reparsed);
  }

  // 随机 1000 组：不同小数位数 0..30，不同整数位长度 1..40。
  std::mt19937_64 engine(20261109ULL);
  std::uniform_int_distribution<int> scale_distribution(0, 30);
  std::uniform_int_distribution<int> length_distribution(1, 40);
  std::uniform_int_distribution<int> digit_distribution(0, 9);
  std::size_t random_failures = 0;
  for (int iteration = 0; iteration < 1000; ++iteration) {
    std::string text;
    if ((engine() & 1ULL) != 0ULL) {
      text.push_back('-');
    }
    const int integer_digits = length_distribution(engine);
    for (int index = 0; index < integer_digits; ++index) {
      text.push_back(static_cast<char>('0' + digit_distribution(engine)));
    }
    const int scale = scale_distribution(engine);
    if (scale > 0) {
      text.push_back('.');
      for (int index = 0; index < scale; ++index) {
        text.push_back(static_cast<char>('0' + digit_distribution(engine)));
      }
    }
    Decimal value;
    if (!Decimal::Parse(text, value)) {
      ++random_failures;
      continue;
    }
    const std::string canonical = value.ToString();
    Decimal reparsed;
    if (!Decimal::Parse(canonical, reparsed) || value != reparsed ||
        reparsed.ToString() != canonical) {
      ++random_failures;
      if (g_failures < 5) {
        std::fprintf(stderr, "FAIL Decimal 往返: %s → %s\n", text.c_str(),
                     canonical.c_str());
      }
    }
    // 构建写入：`Decimal` 必须直接写成 JSON 数字，且文本等于规范化形式。
    JsonBuilder builder;
    builder.Decimal(value);
    CHECK(builder.ok());
    CHECK(builder.str() == canonical);
    // 写出的文本必须是合法 JSON 数字（int / double / Decimal 任一）。
    CHECK(ParseNumberKind(canonical) != NumberKind::kInvalid);
  }
  CHECK(random_failures == 0);
}

// ---------------------------------------------------------------------------
// 6. writer 的数字契约
// ---------------------------------------------------------------------------

void TestWriterNumbers() {
  std::printf("\n[6] writer：double / Decimal 的数字输出契约...\n");

  struct Case {
    const char* input;
    const char* expected;
  };
  const Case exact[] = {
      {"0.0", "0.0"},
      {"-0.0", "-0.0"},
      {"1.0", "1.0"},
      {"-1.0", "-1.0"},
      {"100.0", "100.0"},
      {"0.1", "0.1"},
      {"18446744073709551616", "18446744073709551616"},
      {"-123456789012345678901234567890", "-123456789012345678901234567890"},
  };
  for (const Case& item : exact) {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(item.input, document, error));
    const std::string written = Write(document);
    CHECK(written == item.expected);

    // 二次解析：节点类型与值都必须保持。
    Parser again;
    Document reparsed;
    ErrorPtr again_error;
    CHECK(again.Parse(written, reparsed, again_error));
    const Value lhs = document.Root();
    const Value rhs = reparsed.Root();
    CHECK(lhs.IsDouble() == rhs.IsDouble());
    CHECK(lhs.IsDecimal() == rhs.IsDecimal());
    if (lhs.IsDouble() && rhs.IsDouble()) {
      CHECK(SameBits(lhs.As<double>(), rhs.As<double>()));
    }
    if (lhs.IsDecimal() && rhs.IsDecimal()) {
      Decimal left;
      Decimal right;
      CHECK(lhs.As(left));
      CHECK(rhs.As(right));
      CHECK(left == right);
    }
  }

  // 极端 double：writer 必须写出浮点形态，且位级往返。
  const double extremes[] = {
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::min(),
      std::numeric_limits<double>::denorm_min(),
      1e300,
      1e-300,
      0.1,
      1.0 / 3.0,
  };
  for (const double value : extremes) {
    char text[64];
    const auto result =
        std::to_chars(text, text + sizeof(text), value, std::chars_format::general);
    if (result.ec != std::errc{}) {
      CHECK(false);
      continue;
    }
    const std::string_view input(text, static_cast<std::size_t>(result.ptr - text));
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(input, document, error));
    CHECK(document.Root().IsDouble());
    const std::string written = Write(document);
    CHECK(written.find_first_of(".eE") != std::string::npos);
    double back = 0.0;
    CHECK(ParseDouble(written, &back));
    CHECK(SameBits(back, value));
  }

  // 超出 double 的 Decimal：writer 保留全部十进制位，重新解析仍是 Decimal。
  const char* decimal_inputs[] = {"1e400", "1e-400", "18446744073709551616"};
  for (const char* input : decimal_inputs) {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(input, document, error));
    CHECK(document.Root().IsDecimal());
    const std::string written = Write(document);
    CHECK(written.find_first_of("eE") == std::string::npos);  // Decimal 走十进制展开
    Decimal before;
    Decimal after;
    CHECK(document.Root().As(before));
    CHECK(ParseDecimalNode(written, &after));
    CHECK(before == after);
  }

  // 随机 1000 个位模式：to_chars → parse → Write → parse，位级相等。
  std::mt19937_64 engine(20261110ULL);
  std::size_t checked = 0;
  for (int iteration = 0; iteration < 1000; ++iteration) {
    const std::uint64_t bits = engine();
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) {
      continue;
    }
    char text[64];
    const auto result =
        std::to_chars(text, text + sizeof(text), value, std::chars_format::general);
    if (result.ec != std::errc{}) {
      CHECK(false);
      continue;
    }
    const std::string_view input(text, static_cast<std::size_t>(result.ptr - text));
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(input, document, error));
    CHECK(document.Root().IsDouble());
    const std::string written = Write(document);
    double back = 0.0;
    CHECK(ParseDouble(written, &back));
    CHECK(SameBits(back, value));
    ++checked;
  }
  CHECK(checked > 900);
}

// ---------------------------------------------------------------------------
// 7. 容器中的 -0.0 / 整值 double
// ---------------------------------------------------------------------------

void TestContainerSigns() {
  std::printf("\n[7] 容器中的 -0.0 与整值 double...\n");

  JsonBuilder builder;
  builder.BeginObject();
  builder.Key("zero").Double(-0.0);
  builder.Key("one").Double(1.0);
  builder.EndObject();
  CHECK(builder.ok());
  CHECK(builder.str() == "{\"zero\":-0.0,\"one\":1.0}");

  Parser parser;
  Document document;
  ErrorPtr error;
  CHECK(parser.Parse(builder.str(), document, error));
  const Value root = document.Root();
  CHECK(root.IsObject());
  CHECK(root["zero"].IsDouble());
  CHECK(std::signbit(root["zero"].As<double>()));
  CHECK(root["one"].IsDouble());
  CHECK(root["one"].As<double>() == 1.0);
}

}  // namespace

int main() {
  const auto begin = std::chrono::steady_clock::now();

  TestDoubleBoundaries();
  TestRandomBitPatterns();
  TestPrefixInterference();
  TestNonFiniteRejected();
  TestDecimalContract();
  TestWriterNumbers();
  TestContainerSigns();

  const auto end = std::chrono::steady_clock::now();
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();

  if (g_failures != 0) {
    std::fprintf(stderr, "FAILED: %d check(s) failed out of %zu\n", g_failures, g_checks);
    return 1;
  }
  std::printf("\nALL TESTS PASSED (%zu checks, %lld ms)\n", g_checks,
              static_cast<long long>(elapsed_ms));
  return 0;
}
