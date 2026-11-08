/**
 * @file test_scenarios.cpp
 * @brief 现实场景收尾：BOM、输入上限、NDJSON、深度超限、数字溢出边界。
 */

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/json/parser.hpp"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                             \
    }                                                                           \
  } while (false)

using go2cpp::ErrorPtr;
using go2cpp::json::Document;
using go2cpp::json::JsonError;
using go2cpp::json::JsonFailure;
using go2cpp::json::Parser;
using go2cpp::json::ParserConfig;
using go2cpp::json::ParseOutcome;

/// @brief 场景 12：BOM（UTF-8 EF BB BF）可配。
void TestBom() {
  std::printf("\n[12] BOM...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;
  constexpr std::string_view kWithBom = "\xEF\xBB\xBF{\"a\":1}";

  // 严格模式：BOM 被当作非法字符。
  CHECK(!parser.Parse(kWithBom, doc, error, ParserConfig{}));

  // 宽松模式：跳过 BOM 后正常解析。
  ParserConfig lax;
  lax.allow_bom = true;
  CHECK(parser.Parse(kWithBom, doc, error, lax));
  int value = 0;
  CHECK(doc.Root()["a"].As(value));
  CHECK(value == 1);
}

/// @brief 场景 1/3：输入字节上限（GB 级防护：超限报错，不进 DOM）。
void TestMaxInputBytes() {
  std::printf("\n[1] 输入上限...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;
  ParserConfig config;
  config.max_input_bytes = 8;

  CHECK(!parser.Parse("{\"aaaa\":1}", doc, error, config));  // 10 字节
  const auto* json_error = dynamic_cast<const JsonError*>(error.get());
  CHECK(json_error != nullptr);
  if (json_error != nullptr) {
    CHECK(json_error->Failure() == JsonFailure::kTooLarge);
  }
  CHECK(parser.Parse("{\"a\":1}", doc, error, config));  // 7 字节
}

/// @brief 场景 2：NDJSON（每行一个文档，空行忽略，CRLF 兼容）。
void TestNdjson() {
  std::printf("\n[2] NDJSON...\n");
  Parser parser;
  ErrorPtr error;
  constexpr std::string_view kNdjson = "{\"i\":1}\n{\"i\":2}\r\n\n{\"i\":3}\n";

  std::vector<int> values;
  const std::size_t count = parser.ParseMany(
      kNdjson,
      [&values](Document& document) {
        int value = 0;
        CHECK(document.Root()["i"].As(value));
        values.push_back(value);
        return true;
      },
      error, ParserConfig{});

  CHECK(count == 3u);
  CHECK(values.size() == 3u);
  CHECK(values == std::vector<int>({1, 2, 3}));
}

/// @brief 场景 6：深度超限（max_depth + 位置）。
void TestDepthLimit() {
  std::printf("\n[6] 深度超限...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;
  ParserConfig config;
  config.max_depth = 2;

  CHECK(!parser.Parse("{\"a\":{\"b\":{\"c\":1}}}", doc, error, config));  // 3 层
  const auto* json_error = dynamic_cast<const JsonError*>(error.get());
  CHECK(json_error != nullptr);
  if (json_error != nullptr) {
    CHECK(json_error->Failure() == JsonFailure::kDepthExceeded);
  }
  CHECK(parser.Parse("{\"a\":{\"b\":1}}", doc, error, config));  // 2 层
}

/// @brief 场景 10：数字溢出边界（超长整数 -> Decimal；1e400 行为记录）。
void TestNumberOverflow() {
  std::printf("\n[10] 数字溢出...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;

  // 30 位整数：超出 int64，走 Decimal 回退。
  CHECK(parser.Parse("123456789012345678901234567890", doc, error, ParserConfig{}));
  go2cpp::json::core::Decimal decimal;
  CHECK(doc.Root().As(decimal));
  CHECK(decimal.ToString() == "123456789012345678901234567890");

  // 1e400：超出 double 范围，记录实际行为（版本相关，不强行断言）。
  const bool ok = parser.Parse("1e400", doc, error, ParserConfig{});
  std::printf("  1e400 解析结果 ok=%d\n", ok ? 1 : 0);
}

/// @brief 场景 9：非法 UTF-8（当前实现按字节透传，不做校验——记录行为）。
void TestInvalidUtf8() {
  std::printf("\n[9] 非法 UTF-8（透传）...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;
  // 字符串里放非法字节 0xFF：当前实现不校验，按原始字节借用。
  constexpr std::string_view kInvalid = "{\"s\":\"\xFF\"}";
  CHECK(parser.Parse(kInvalid, doc, error, ParserConfig{}));
  const std::string_view text = doc.Root()["s"].AsText();
  CHECK(text.size() == 1u);
  CHECK(static_cast<unsigned char>(text[0]) == 0xFFu);
}

}  // namespace

int main() {
  std::printf("=== 现实场景收尾测试 ===\n");
  TestBom();
  TestMaxInputBytes();
  TestNdjson();
  TestDepthLimit();
  TestNumberOverflow();
  TestInvalidUtf8();

  if (g_failures == 0) {
    std::printf("\nALL TESTS PASSED\n");
    return 0;
  }
  std::fprintf(stderr, "\n%d check(s) failed\n", g_failures);
  return 1;
}
