/**
 * @file test_core_conversion.cpp
 * @brief `go2cpp::core` 转换表与 `Text` 的回归测试（迁移自 week1 `csvp::conversion`）。
 *
 * 覆盖：
 * - 内置 5 种转换器的成功/失败/边界（int 溢出、double 溢出与下溢、bool 大小写、
 *   Decimal 语法、String 借用语义）；
 * - `ItemValue` 的 7 个分支：`Convert` 后落在哪个 `index()`；
 * - 自定义 Converter 注册、幂等（保留首次注册）、内置 id 保护、null 与未知 id；
 * - `Convert` 失败时输出保持不变；无转换器时同样失败；
 * - `Text` 的 borrowed / owned 行为与零拷贝地址一致性；
 * - 随机：10 万条随机字节串喂给解析器，与 `strtoll` / `strtod` 对照，且失败不写输出；
 * - 真实场景（6 个）：CSV 行转换、JSON 键值、插件自定义类型、配置回退、
 *   批量统计、零拷贝字段传递。
 */

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <variant>

#include "go2cpp/json/core/conversion.hpp"
#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/json/core/text.hpp"
#include "go2cpp/tools/error.hpp"
#include "go2cpp/reflect/type.hpp"

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

using go2cpp::json::core::Convert;
using go2cpp::json::core::Converter;
using go2cpp::json::core::Decimal;
using go2cpp::json::core::ItemValue;
using go2cpp::json::core::StringToBool;
using go2cpp::json::core::StringToDecimal;
using go2cpp::json::core::StringToDouble;
using go2cpp::json::core::StringToInt;
using go2cpp::json::core::StringToString;
using go2cpp::json::core::Text;
using go2cpp::reflect::TypeId;

/// @brief 把整数包成 Decimal，供场景用例做定点复算。
Decimal MakeDecimal(std::int64_t value) {
  Decimal result;
  Decimal::Parse(std::to_string(value), result);
  return result;
}

/// @brief 插件自定义类型：解析 `"主版本.次版本"`。
struct Version {
  int major = 0;
  int minor = 0;
};

/// @brief 自定义转换器：必须是普通函数指针（可跨线程只读、无捕获）。
bool ConvertVersion(std::string_view from, ItemValue& to) {
  const std::size_t dot = from.find('.');
  if (dot == std::string_view::npos) {
    return false;
  }
  int major = 0;
  int minor = 0;
  if (!StringToInt(from.substr(0, dot), major) || !StringToInt(from.substr(dot + 1), minor)) {
    return false;
  }
  std::shared_ptr<const void> erased = std::make_shared<const Version>(Version{major, minor});
  to = std::move(erased);
  return true;
}

/// @brief 第二个版本转换器，用来验证「重复注册保留首次」：它总是失败。
bool ConvertVersionReject(std::string_view, ItemValue&) { return false; }

// ---------------------------------------------------------------------------
// 内置标量解析
// ---------------------------------------------------------------------------

void TestStringToInt() {
  int value = 0;
  CHECK(StringToInt("0", value));
  CHECK(value == 0);
  CHECK(StringToInt("42", value));
  CHECK(value == 42);
  CHECK(StringToInt("-42", value));
  CHECK(value == -42);
  CHECK(StringToInt("+7", value));
  CHECK(value == 7);
  CHECK(StringToInt("007", value));
  CHECK(value == 7);
  CHECK(StringToInt("2147483647", value));
  CHECK(value == std::numeric_limits<int>::max());
  CHECK(StringToInt("-2147483648", value));
  CHECK(value == std::numeric_limits<int>::min());

  // 失败时输出不变。
  value = 123;
  CHECK(!StringToInt("2147483648", value));   // 越过 int 上限 1
  CHECK(value == 123);
  CHECK(!StringToInt("-2147483649", value));  // 越过 int 下限 1
  CHECK(!StringToInt("", value));
  CHECK(!StringToInt("-", value));
  CHECK(!StringToInt("+", value));
  CHECK(!StringToInt("--1", value));
  CHECK(!StringToInt("12x", value));
  CHECK(!StringToInt(" 1", value));
  CHECK(!StringToInt("1 ", value));
  CHECK(!StringToInt("1.0", value));
  CHECK(!StringToInt("0x10", value));
  CHECK(!StringToInt("999999999999999999999999", value));
  CHECK(value == 123);
}

void TestStringToDouble() {
  double value = 0.0;
  CHECK(StringToDouble("0", value));
  CHECK(value == 0.0);
  CHECK(StringToDouble("1.5", value));
  CHECK(value == 1.5);
  CHECK(StringToDouble("-1.5", value));
  CHECK(value == -1.5);
  CHECK(StringToDouble("+2.5", value));
  CHECK(value == 2.5);
  CHECK(StringToDouble(".5", value));
  CHECK(value == 0.5);
  CHECK(StringToDouble("5.", value));
  CHECK(value == 5.0);
  CHECK(StringToDouble("1e2", value));
  CHECK(value == 100.0);
  CHECK(StringToDouble("1E-2", value));
  CHECK(value == 0.01);
  CHECK(StringToDouble("-0", value));
  CHECK(value == 0.0);
  CHECK(StringToDouble("1e308", value));
  CHECK(value > 1e307);

  value = 2.5;
  CHECK(!StringToDouble("", value));
  CHECK(!StringToDouble("abc", value));
  CHECK(!StringToDouble("1.2.3", value));
  CHECK(!StringToDouble("1e", value));
  CHECK(!StringToDouble("1e+", value));
  CHECK(!StringToDouble("1e400", value));    // 溢出
  CHECK(!StringToDouble("1e-400", value));   // 非零输入下溢成 0
  CHECK(!StringToDouble("nan", value));
  CHECK(!StringToDouble("inf", value));
  CHECK(value == 2.5);

  // 十进制语义：double 的 0.1 + 0.2 不等于 0.3，这是 double 的固有问题，
  // 也是 Decimal 存在的理由（此断言用于固定参照事实）。
  double tenth = 0.0;
  double fifth = 0.0;
  CHECK(StringToDouble("0.1", tenth));
  CHECK(StringToDouble("0.2", fifth));
  CHECK(tenth + fifth != 0.3);
}

void TestStringToBool() {
  bool value = false;
  CHECK(StringToBool("true", value));
  CHECK(value);
  CHECK(StringToBool("TRUE", value));
  CHECK(value);
  CHECK(StringToBool("1", value));
  CHECK(value);
  CHECK(StringToBool("yes", value));
  CHECK(value);

  CHECK(StringToBool("false", value));
  CHECK(!value);
  CHECK(StringToBool("FALSE", value));
  CHECK(!value);
  CHECK(StringToBool("0", value));
  CHECK(!value);
  CHECK(StringToBool("no", value));
  CHECK(!value);

  value = true;
  CHECK(!StringToBool("True", value));   // 与 week1 一致：大小写敏感
  CHECK(!StringToBool("on", value));
  CHECK(!StringToBool("2", value));
  CHECK(!StringToBool("", value));
  CHECK(!StringToBool(" true", value));
  CHECK(value);
}

void TestStringToDecimal() {
  Decimal value;
  CHECK(StringToDecimal("1.25", value));
  CHECK(value.ToString() == "1.25");
  CHECK(StringToDecimal("-0.001", value));
  CHECK(value.ToString() == "-0.001");
  CHECK(StringToDecimal("1e3", value));
  CHECK(value.ToString() == "1000");

  Decimal unchanged;
  CHECK(Decimal::Parse("7.5", unchanged));
  CHECK(!StringToDecimal("1.2.3", unchanged));
  CHECK(unchanged.ToString() == "7.5");
  CHECK(!StringToDecimal("", unchanged));
  CHECK(unchanged.ToString() == "7.5");
}

void TestStringToString() {
  const std::string source = "hello";
  Text text;
  CHECK(StringToString(source, text));
  CHECK(!text.empty());
  CHECK(text.borrowed());
  CHECK(text.view() == "hello");
  CHECK(text.size() == 5);
  // 零拷贝：视图直接指向输入缓冲。
  CHECK(text.view().data() == source.data());

  Text empty;
  CHECK(StringToString(std::string_view(), empty));
  CHECK(empty.empty());
  CHECK(empty.borrowed());

  // 拷贝借用态 Text：视图仍指向同一缓冲。
  Text copy = text;
  CHECK(copy.borrowed());
  CHECK(copy.view().data() == source.data());
}

void TestTextOwned() {
  const std::string pool = "chunk chunk";
  Text owned = Text::Owned(std::string_view(pool.data(), 5), 0, 5);
  CHECK(!owned.borrowed());
  CHECK(owned.view() == "chunk");
  CHECK(!owned.empty());
  CHECK(owned.size() == 5);
  CHECK(owned.owned_offset() == 0);
  CHECK(owned.owned_len() == 5);

  // 池化拥有态不复制字节：视图直接指向池。
  const std::string big(100, 'x');
  Text moved = Text::Owned(std::string_view(big), 0, 100);
  CHECK(!moved.borrowed());
  CHECK(moved.size() == 100);
  CHECK(moved.view().data() == big.data());
  CHECK(moved.view() == big);

  // 拷贝拥有态 Text：视图与池内区间都保持。
  Text moved_again = moved;
  CHECK(!moved_again.borrowed());
  CHECK(moved_again.size() == 100);
  CHECK(moved_again.owned_len() == 100);

  Text assigned;
  assigned = Text::Borrow("borrowed");
  CHECK(assigned.borrowed());
  CHECK(assigned.view() == "borrowed");
  assigned = Text::Owned(std::string_view(pool.data() + 6, 5), 6, 5);
  CHECK(!assigned.borrowed());
  CHECK(assigned.view() == "chunk");
  CHECK(assigned.owned_offset() == 6);
}

void TestItemValueBranches() {
  ItemValue value;
  CHECK(value.index() == 0);  // monostate
  CHECK(std::holds_alternative<std::monostate>(value));

  CHECK(Convert(go2cpp::reflect::kInt32Type, "42", value));
  CHECK(value.index() == 2);
  CHECK(std::holds_alternative<std::int64_t>(value));
  CHECK(std::get<std::int64_t>(value) == 42);

  CHECK(Convert(go2cpp::reflect::kDoubleType, "1.5", value));
  CHECK(value.index() == 3);
  CHECK(std::holds_alternative<double>(value));
  CHECK(std::get<double>(value) == 1.5);

  CHECK(Convert(go2cpp::reflect::kBoolType, "true", value));
  CHECK(value.index() == 4);
  CHECK(std::holds_alternative<bool>(value));
  CHECK(std::get<bool>(value));

  CHECK(Convert(go2cpp::reflect::kStringType, "hello", value));
  CHECK(value.index() == 5);
  CHECK(std::holds_alternative<Text>(value));
  CHECK(std::get<Text>(value).view() == "hello");
  CHECK(std::get<Text>(value).borrowed());

  CHECK(Convert(go2cpp::json::core::kDecimalType, "1.25", value));
  CHECK(value.index() == 1);
  CHECK(std::holds_alternative<Decimal>(value));
  CHECK(std::get<Decimal>(value).ToString() == "1.25");
}

void TestBuiltinTable() {
  CHECK(go2cpp::json::core::FindConverter(go2cpp::reflect::kBoolType) != nullptr);
  CHECK(go2cpp::json::core::FindConverter(go2cpp::reflect::kInt32Type) != nullptr);
  CHECK(go2cpp::json::core::FindConverter(go2cpp::reflect::kDoubleType) != nullptr);
  CHECK(go2cpp::json::core::FindConverter(go2cpp::reflect::kStringType) != nullptr);
  CHECK(go2cpp::json::core::FindConverter(go2cpp::json::core::kDecimalType) != nullptr);

  // 已知限制（与 week1 一致）：week1 只有 5 个内置转换器，其余内置类型没有。
  CHECK(go2cpp::json::core::FindConverter(go2cpp::reflect::kInt64Type) == nullptr);
  CHECK(go2cpp::json::core::FindConverter(go2cpp::reflect::kUint32Type) == nullptr);
  CHECK(go2cpp::json::core::FindConverter(go2cpp::reflect::kUint64Type) == nullptr);
  CHECK(go2cpp::json::core::FindConverter(go2cpp::reflect::kBytesType) == nullptr);
  CHECK(go2cpp::json::core::FindConverter(go2cpp::reflect::kUnknownType) == nullptr);
}

void TestConvertFailureKeepsOutput() {
  ItemValue value;
  CHECK(Convert(go2cpp::reflect::kInt32Type, "42", value));
  CHECK(std::get<std::int64_t>(value) == 42);

  CHECK(!Convert(go2cpp::reflect::kInt32Type, "abc", value));
  CHECK(value.index() == 2);
  CHECK(std::get<std::int64_t>(value) == 42);

  CHECK(!Convert(go2cpp::json::core::kDecimalType, "1.2.3", value));
  CHECK(std::get<std::int64_t>(value) == 42);

  CHECK(!Convert(go2cpp::reflect::kDoubleType, "1e400", value));
  CHECK(std::get<std::int64_t>(value) == 42);

  CHECK(!Convert(go2cpp::reflect::kBoolType, "maybe", value));
  CHECK(std::get<std::int64_t>(value) == 42);

  // 没有转换器的类型：同样失败且不改输出。
  CHECK(!Convert(go2cpp::reflect::kBytesType, "x", value));
  CHECK(std::get<std::int64_t>(value) == 42);
  CHECK(!Convert(go2cpp::reflect::kUnknownType, "x", value));
  CHECK(std::get<std::int64_t>(value) == 42);
}

// ---------------------------------------------------------------------------
// 自定义 Converter 注册
// ---------------------------------------------------------------------------

void TestRegisterConverter() {
  TypeId id = go2cpp::reflect::TypeRegistry::Instance().Register("test.Version", sizeof(Version));

  // 首次注册。
  go2cpp::ErrorPtr error = go2cpp::NewError("占位");
  CHECK(go2cpp::json::core::RegisterConverter(id, &ConvertVersion, error));
  CHECK(error == nullptr);
  CHECK(go2cpp::json::core::FindConverter(id) == &ConvertVersion);

  ItemValue value;
  CHECK(Convert(id, "1.2", value));
  CHECK(value.index() == 6);
  CHECK(std::holds_alternative<std::shared_ptr<const void>>(value));
  const auto& erased = std::get<std::shared_ptr<const void>>(value);
  CHECK(erased != nullptr);
  const Version* version = static_cast<const Version*>(erased.get());
  CHECK(version != nullptr);
  CHECK(version->major == 1);
  CHECK(version->minor == 2);

  // 解析失败不改输出。
  CHECK(!Convert(id, "1", value));
  CHECK(std::get<std::shared_ptr<const void>>(value) == erased);

  // 幂等：重复注册返回 true，但保留首次注册的函数。
  error = nullptr;
  CHECK(go2cpp::json::core::RegisterConverter(id, &ConvertVersionReject, error));
  CHECK(error == nullptr);
  CHECK(go2cpp::json::core::FindConverter(id) == &ConvertVersion);
  CHECK(Convert(id, "3.4", value));
  CHECK(static_cast<const Version*>(std::get<std::shared_ptr<const void>>(value).get())->major ==
        3);

  // 不带 error 的重载同样幂等。
  CHECK(go2cpp::json::core::RegisterConverter(id, &ConvertVersionReject));
}

void TestRegisterConverterErrors() {
  go2cpp::ErrorPtr error;

  // 未知 id。
  CHECK(!go2cpp::json::core::RegisterConverter(go2cpp::reflect::kUnknownType, &ConvertVersion, error));
  CHECK(error != nullptr);
  CHECK(go2cpp::ErrorMessage(error).find("invalid") != std::string::npos);

  // 空转换器。
  error = nullptr;
  CHECK(!go2cpp::json::core::RegisterConverter(32, nullptr, error));
  CHECK(error != nullptr);

  // 内置 id 受保护，不可覆盖。
  error = nullptr;
  CHECK(!go2cpp::json::core::RegisterConverter(go2cpp::reflect::kInt32Type, &ConvertVersion, error));
  CHECK(error != nullptr);
  CHECK(go2cpp::ErrorMessage(error).find("builtin") != std::string::npos);
  // 内置转换器仍是原来的（int 解析而不是 Version 解析）。
  CHECK(go2cpp::json::core::FindConverter(go2cpp::reflect::kInt32Type) != &ConvertVersion);

  // 成功注册会清空调用方传入的旧错误。
  error = go2cpp::NewError("旧错误");
  const TypeId id = go2cpp::reflect::TypeRegistry::Instance().Register("test.Other", 8);
  CHECK(go2cpp::json::core::RegisterConverter(id, &ConvertVersion, error));
  CHECK(error == nullptr);
}

// ---------------------------------------------------------------------------
// 随机测试
// ---------------------------------------------------------------------------

/// @brief 10 万条随机字节串：解析器不能崩、失败不能写输出、结果与标准库自查一致。
void TestRandomParsers() {
  std::mt19937_64 rng(0xBADF00DULL);
  const char alphabet[] = {'0', '1', '2', '5', '9', '+', '-', '.', 'e', 'E', 'a', 'x'};
  constexpr std::size_t kAlphabetSize = sizeof(alphabet) / sizeof(alphabet[0]);

  for (int iteration = 0; iteration < 100000; ++iteration) {
    const std::size_t length = static_cast<std::size_t>(rng() % 12);
    std::string text;
    text.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      text.push_back(alphabet[rng() % kAlphabetSize]);
    }

    // int：以 strtoll 为参照，额外限定 int 范围。
    int actual_int = 777;
    const bool int_ok = StringToInt(text, actual_int);
    errno = 0;
    char* int_end = nullptr;
    const long long reference_int = std::strtoll(text.c_str(), &int_end, 10);
    const bool int_reference_ok =
        !text.empty() && int_end == text.c_str() + text.size() && errno == 0 &&
        reference_int >= std::numeric_limits<int>::min() &&
        reference_int <= std::numeric_limits<int>::max();
    if (int_ok != int_reference_ok || (int_ok && actual_int != static_cast<int>(reference_int))) {
      std::fprintf(stderr, "随机 int 对照失败 \"%s\": ok=%d ref=%d\n", text.c_str(),
                   static_cast<int>(int_ok), static_cast<int>(int_reference_ok));
      ++g_failures;
    }
    if (!int_ok && actual_int != 777) {
      std::fprintf(stderr, "随机 int：失败却修改了输出 \"%s\"\n", text.c_str());
      ++g_failures;
    }

    // double：以 strtod 为参照，要求有限值；相对误差 ≤ 1e-15。
    // 两类文本不参与「成功/失败」对照，因为本实现有意不支持：
    // 1. 十六进制浮点（"0x1p3" 是 C99 语法，week1 起就不接受）；
    // 2. 指数绝对值 > 400 的文本（本实现为防 10 万次循环直接判失败，
    //    而 strtod 对 "0e999" 这类会成功返回 0）。
    const bool has_exponent =
        text.find('e') != std::string::npos || text.find('E') != std::string::npos;
    const bool has_hex = text.find('x') != std::string::npos;
    const bool comparable = !has_exponent && !has_hex;
    double actual_double = 777.0;
    const bool double_ok = StringToDouble(text, actual_double);
    errno = 0;
    char* double_end = nullptr;
    const double reference_double = std::strtod(text.c_str(), &double_end);
    const bool double_reference_ok = !text.empty() && double_end == text.c_str() + text.size() &&
                                     errno == 0 && std::isfinite(reference_double);
    if (comparable && double_ok != double_reference_ok) {
      std::fprintf(stderr, "随机 double 对照失败 \"%s\": ok=%d ref=%d\n", text.c_str(),
                   static_cast<int>(double_ok), static_cast<int>(double_reference_ok));
      ++g_failures;
    } else if (comparable && double_ok) {
      const double scale = std::fabs(reference_double) > 1.0 ? std::fabs(reference_double) : 1.0;
      if (std::fabs(actual_double - reference_double) > 1e-15 * scale) {
        std::fprintf(stderr, "随机 double 精度失败 \"%s\": %.17g vs %.17g\n", text.c_str(),
                     actual_double, reference_double);
        ++g_failures;
      }
    }
    if (!double_ok && actual_double != 777.0) {
      std::fprintf(stderr, "随机 double：失败却修改了输出 \"%s\"\n", text.c_str());
      ++g_failures;
    }

    // bool：只接受 8 个字面量。
    bool actual_bool = true;
    const bool bool_ok = StringToBool(text, actual_bool);
    const bool bool_reference_ok =
        text == "true" || text == "TRUE" || text == "1" || text == "yes" || text == "false" ||
        text == "FALSE" || text == "0" || text == "no";
    if (bool_ok != bool_reference_ok) {
      std::fprintf(stderr, "随机 bool 对照失败 \"%s\"\n", text.c_str());
      ++g_failures;
    }

    // Decimal：失败必须保持输出；成功必须与直接调用 Decimal::Parse 一致。
    Decimal actual_decimal;
    CHECK(Decimal::Parse("5", actual_decimal));
    const bool decimal_ok = StringToDecimal(text, actual_decimal);
    Decimal reference_decimal;
    const bool decimal_reference_ok = Decimal::Parse(text, reference_decimal);
    if (decimal_ok != decimal_reference_ok ||
        (decimal_ok && actual_decimal != reference_decimal)) {
      std::fprintf(stderr, "随机 decimal 对照失败 \"%s\"\n", text.c_str());
      ++g_failures;
    }
    if (!decimal_ok && actual_decimal.ToString() != "5") {
      std::fprintf(stderr, "随机 decimal：失败却修改了输出 \"%s\"\n", text.c_str());
      ++g_failures;
    }

    // String：恒成功且零拷贝。
    Text actual_text;
    if (!StringToString(text, actual_text) || actual_text.view() != text ||
        actual_text.view().data() != text.data()) {
      std::fprintf(stderr, "随机 string 失败 \"%s\"\n", text.c_str());
      ++g_failures;
    }
  }
}

/// @brief 5 万次随机字段：`Convert` 与直接调用内置函数结果一致。
void TestRandomConvert() {
  std::mt19937_64 rng(0x12345678ULL);
  const TypeId ids[] = {go2cpp::reflect::kBoolType, go2cpp::reflect::kInt32Type,
                        go2cpp::reflect::kDoubleType, go2cpp::reflect::kStringType,
                        go2cpp::json::core::kDecimalType};
  const char alphabet[] = {'0', '1', '7', '9', '+', '-', '.', 'e', 't', 'r', 'u', 'a', 'l',
                           's', 'f', 'n', 'o', 'y'};
  constexpr std::size_t kAlphabetSize = sizeof(alphabet) / sizeof(alphabet[0]);

  for (int iteration = 0; iteration < 50000; ++iteration) {
    const TypeId id = ids[rng() % 5];
    const std::size_t length = static_cast<std::size_t>(rng() % 10);
    std::string text;
    for (std::size_t index = 0; index < length; ++index) {
      text.push_back(alphabet[rng() % kAlphabetSize]);
    }

    ItemValue value;
    const bool ok = Convert(id, text, value);
    switch (id) {
      case go2cpp::reflect::kBoolType: {
        bool expected = false;
        CHECK(ok == StringToBool(text, expected));
        break;
      }
      case go2cpp::reflect::kInt32Type: {
        int expected = 0;
        CHECK(ok == StringToInt(text, expected));
        if (ok) {
          CHECK(std::get<std::int64_t>(value) == expected);
        }
        break;
      }
      case go2cpp::reflect::kDoubleType: {
        double expected = 0.0;
        CHECK(ok == StringToDouble(text, expected));
        if (ok) {
          CHECK(std::get<double>(value) == expected);
        }
        break;
      }
      case go2cpp::reflect::kStringType: {
        CHECK(ok);
        CHECK(std::get<Text>(value).view() == text);
        break;
      }
      case go2cpp::json::core::kDecimalType: {
        Decimal expected;
        CHECK(ok == Decimal::Parse(text, expected));
        if (ok) {
          CHECK(std::get<Decimal>(value) == expected);
        }
        break;
      }
      default:
        CHECK(false);
        break;
    }
  }
}

// ---------------------------------------------------------------------------
// 真实场景
// ---------------------------------------------------------------------------

/// @brief 场景 1：CSV 一行的 5 列按类型转换，结果与预期完全一致。
void ScenarioCsvRow() {
  // id,price,qty,active,name
  const std::string_view fields[] = {"1001", "12.50", "3", "true", "键盘"};
  ItemValue row[5];
  CHECK(Convert(go2cpp::reflect::kInt32Type, fields[0], row[0]));
  CHECK(Convert(go2cpp::json::core::kDecimalType, fields[1], row[1]));
  CHECK(Convert(go2cpp::reflect::kInt32Type, fields[2], row[2]));
  CHECK(Convert(go2cpp::reflect::kBoolType, fields[3], row[3]));
  CHECK(Convert(go2cpp::reflect::kStringType, fields[4], row[4]));

  CHECK(std::get<std::int64_t>(row[0]) == 1001);
  CHECK(std::get<Decimal>(row[1]).ToString() == "12.5");
  CHECK(std::get<std::int64_t>(row[2]) == 3);
  CHECK(std::get<bool>(row[3]));
  CHECK(std::get<Text>(row[4]).view() == "键盘");
  // 字符串列零拷贝：指向原始字段缓冲。
  CHECK(std::get<Text>(row[4]).view().data() == fields[4].data());

  // 小计 = 12.50 × 3。
  const Decimal subtotal = std::get<Decimal>(row[1]) * MakeDecimal(std::get<std::int64_t>(row[2]));
  CHECK(subtotal.ToString() == "37.5");
}

/// @brief 场景 2：JSON 键值对（字符串键 → 混合类型值）。
void ScenarioJsonObject() {
  struct Entry {
    const char* key;
    TypeId type;
    const char* text;
  };
  const Entry entries[] = {
      {"port", go2cpp::reflect::kInt32Type, "8080"},
      {"ratio", go2cpp::reflect::kDoubleType, "0.75"},
      {"enabled", go2cpp::reflect::kBoolType, "true"},
      {"limit", go2cpp::json::core::kDecimalType, "1.5e2"},
      {"mode", go2cpp::reflect::kStringType, "fast"},
  };
  int converted = 0;
  for (const Entry& entry : entries) {
    ItemValue value;
    CHECK(Convert(entry.type, entry.text, value));
    CHECK(value.index() != 0);
    ++converted;
  }
  CHECK(converted == 5);

  ItemValue limit;
  CHECK(Convert(go2cpp::json::core::kDecimalType, "1.5e2", limit));
  CHECK(std::get<Decimal>(limit).ToString() == "150");
}

/// @brief 场景 3：插件注册自定义类型（`shared_ptr<const void>` 类型擦除）。
void ScenarioPluginType() {
  const TypeId id = go2cpp::reflect::TypeRegistry::Instance().Register("plugin.Version", sizeof(Version));
  go2cpp::ErrorPtr error;
  CHECK(go2cpp::json::core::RegisterConverter(id, &ConvertVersion, error));
  CHECK(error == nullptr);

  ItemValue value;
  CHECK(Convert(id, "2.7", value));
  const Version* version = static_cast<const Version*>(std::get<std::shared_ptr<const void>>(value).get());
  CHECK(version != nullptr);
  CHECK(version->major == 2);
  CHECK(version->minor == 7);

  // 插件转换失败时上层看到 false，且能拿到明确的错误（这里用 Decimal 的例子）。
  Decimal bad;
  CHECK(!StringToDecimal("2.7.1", bad));
}

/// @brief 场景 4：配置读取失败时保留默认值（「错误是值」的典型用法）。
void ScenarioConfigFallback() {
  struct Config {
    int port = 8080;
    double ratio = 0.5;
    bool enabled = false;
  } config;

  ItemValue value;
  const char* port_text = "abc";  // 配置文件里写错了
  if (Convert(go2cpp::reflect::kInt32Type, port_text, value)) {
    config.port = static_cast<int>(std::get<std::int64_t>(value));
  }
  CHECK(config.port == 8080);  // 回退到默认值

  CHECK(Convert(go2cpp::reflect::kDoubleType, "0.75", value));
  config.ratio = std::get<double>(value);
  CHECK(config.ratio == 0.75);

  CHECK(Convert(go2cpp::reflect::kBoolType, "TRUE", value));
  config.enabled = std::get<bool>(value);
  CHECK(config.enabled);
}

/// @brief 场景 5：批量转换统计——成功/失败计数与失败兜底。
void ScenarioBatchStatistics() {
  const std::string_view cells[] = {"1", "2", "x", "4", "", "6", "7.5", "8"};
  int converted = 0;
  int failed = 0;
  std::int64_t total = 0;
  for (const std::string_view cell : cells) {
    ItemValue value;
    if (Convert(go2cpp::reflect::kInt32Type, cell, value)) {
      total += std::get<std::int64_t>(value);
      ++converted;
    } else {
      ++failed;
    }
  }
  CHECK(converted == 5);  // 1, 2, 4, 6, 8
  CHECK(failed == 3);     // "x"、"7.5"（小数点非法）、""
  CHECK(total == 1 + 2 + 4 + 6 + 8);
}

/// @brief 场景 6：零拷贝字段传递——同一块输入被多个 Text 借用，地址完全一致。
void ScenarioZeroCopyFields() {
  const std::string buffer = "alpha,beta,gamma";
  const std::string_view first(buffer.data(), 5);
  const std::string_view second(buffer.data() + 6, 4);
  const std::string_view third(buffer.data() + 11, 5);

  Text texts[3];
  CHECK(StringToString(first, texts[0]));
  CHECK(StringToString(second, texts[1]));
  CHECK(StringToString(third, texts[2]));
  CHECK(texts[0].view() == "alpha");
  CHECK(texts[1].view() == "beta");
  CHECK(texts[2].view() == "gamma");
  CHECK(texts[0].view().data() == buffer.data());
  CHECK(texts[1].view().data() == buffer.data() + 6);
  CHECK(texts[2].view().data() == buffer.data() + 11);
  // 三个视图共享同一缓冲：整个转换过程零次字符串拷贝。
  CHECK(texts[0].view().data() + 5 + 1 == texts[1].view().data());
}

}  // namespace

int main() {
  TestStringToInt();
  TestStringToDouble();
  TestStringToBool();
  TestStringToDecimal();
  TestStringToString();
  TestTextOwned();
  TestItemValueBranches();
  TestBuiltinTable();
  TestConvertFailureKeepsOutput();
  TestRegisterConverter();
  TestRegisterConverterErrors();
  TestRandomParsers();
  TestRandomConvert();
  ScenarioCsvRow();
  ScenarioJsonObject();
  ScenarioPluginType();
  ScenarioConfigFallback();
  ScenarioBatchStatistics();
  ScenarioZeroCopyFields();

  if (g_failures != 0) {
    std::fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
    return 1;
  }
  std::puts("ALL TESTS PASSED");
  return 0;
}
