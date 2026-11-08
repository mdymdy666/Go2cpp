/**
 * @file test_integration.cpp
 * @brief 跨模块端到端测试：解析器（P2/P6b）与读取层（P3）在同一份真实数据上的行为。
 *
 * @details
 * 注意：`Value` 是 `Document` 的 16 B 句柄（P6b），没有独立序列化；
 * 端到端验证的方式是「同一份原始文本」分别喂给解析器与 Reader，各自断言。
 */

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/json/document.hpp"
#include "go2cpp/json/parser.hpp"
#include "go2cpp/json/reader.hpp"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                             \
    }                                                                           \
  } while (false)

using go2cpp::json::core::Decimal;
using go2cpp::json::core::ItemValue;
using go2cpp::json::Document;
using go2cpp::json::FieldError;
using go2cpp::json::FieldRule;
using go2cpp::json::Parser;
using go2cpp::json::Reader;
using go2cpp::json::RowView;
using go2cpp::json::Value;
using go2cpp::reflect::kBoolType;
using go2cpp::json::core::kDecimalType;
using go2cpp::reflect::kInt32Type;
using go2cpp::reflect::kStringType;

bool ParseOk(std::string_view text, Document& out) {
  Parser parser;
  go2cpp::ErrorPtr error;
  return parser.Parse(text, out, error);
}

/// @brief 场景 1：服务配置（字符串/数字/布尔/嵌套）。
void TestConfig() {
  std::printf("\n[1] 服务配置...\n");
  constexpr std::string_view kJson =
      R"({"name":"server","port":8080,"debug":true,"limits":{"max":100}})";
  Document document;
  const Value& root = document.Root();
  CHECK(ParseOk(kJson, document));
  CHECK(root.IsObject());
  CHECK(root.Size() == 4u);

  std::string name;
  CHECK(root["name"].As(name));
  CHECK(name == "server");

  int port = 0;
  CHECK(root["port"].As(port));
  CHECK(port == 8080);

  bool debug = false;
  CHECK(root["debug"].As(debug));
  CHECK(debug);

  CHECK(root["limits"].IsObject());
  int limit = 0;
  CHECK(root["limits"]["max"].As(limit));
  CHECK(limit == 100);
}

/// @brief 场景 2：API 响应（对象数组）。
void TestApiResponse() {
  std::printf("\n[2] API 响应...\n");
  constexpr std::string_view kJson =
      R"({"users":[{"id":1,"name":"alice"},{"id":2,"name":"bob"}]})";
  Document document;
  const Value& root = document.Root();
  CHECK(ParseOk(kJson, document));
  const Value users = root["users"];
  CHECK(users.IsArray());
  CHECK(users.Size() == 2u);

  int id = 0;
  CHECK(users.At(0)["id"].As(id));
  CHECK(id == 1);
  std::string name;
  CHECK(users.At(1)["name"].As(name));
  CHECK(name == "bob");
}

/// @brief 场景 3：日志行（转义 + Unicode 解码）。
void TestLogLine() {
  std::printf("\n[3] 日志行（转义/Unicode）...\n");
  constexpr std::string_view kJson =
      R"({"level":"error","msg":"line\nbreak","path":"C:\\tmp","zh":"\u4f60\u597d"})";
  Document document;
  const Value& root = document.Root();
  CHECK(ParseOk(kJson, document));

  std::string msg;
  CHECK(root["msg"].As(msg));
  CHECK(msg == "line\nbreak");

  std::string path;
  CHECK(root["path"].As(path));
  CHECK(path == "C:\\tmp");

  std::string zh;
  CHECK(root["zh"].As(zh));
  CHECK(zh == "\xE4\xBD\xA0\xE5\xA5\xBD");  // 你好（UTF-8）
}

/// @brief 场景 4：价格表（double 与 Decimal 两条路径）。
void TestPrice() {
  std::printf("\n[4] 价格表...\n");
  constexpr std::string_view kJson = R"({"price":12.34,"qty":3})";
  Document document;
  const Value& root = document.Root();
  CHECK(ParseOk(kJson, document));

  double price = 0.0;
  CHECK(root["price"].As(price));
  CHECK(price > 12.33 && price < 12.35);

  int qty = 0;
  CHECK(root["qty"].As(qty));
  CHECK(qty == 3);

  // Reader 用同一份文本的字段，按 Decimal 规则转换。
  const std::string_view price_text = "12.34";
  Reader reader;
  reader.SetRules({{"price", kDecimalType, true, ""}, {"qty", kInt32Type, true, ""}});
  RowView<> row;
  row.Append(price_text);
  row.Append("3");
  std::vector<ItemValue> values;
  std::vector<FieldError> errors;
  CHECK(reader.Read(row, values, errors));
  CHECK(values.size() == 2u);
  CHECK(std::holds_alternative<Decimal>(values[0]));
  CHECK(std::get<std::int64_t>(values[1]) == 3);
}

/// @brief 场景 5：大整数（超出 int64 → Decimal）。
void TestBigInteger() {
  std::printf("\n[5] 大整数...\n");
  constexpr std::string_view kJson = R"({"id":123456789012345678901234567890})";
  Document document;
  const Value& root = document.Root();
  CHECK(ParseOk(kJson, document));

  Decimal id;
  CHECK(root["id"].As(id));
  CHECK(!id.IsZero());
  CHECK(id.ToString() == "123456789012345678901234567890");
}

/// @brief 场景 6：Reader 端到端（同一配置文本的字段）。
void TestReaderEndToEnd() {
  std::printf("\n[6] Reader 端到端...\n");
  Reader reader;
  reader.SetRules({
      {"name", kStringType, true, ""},
      {"port", kInt32Type, false, ""},
      {"debug", kBoolType, false, ""},
  });

  RowView<> row;
  row.Append("server");
  row.Append("8080");
  row.Append("true");
  std::vector<ItemValue> values;
  std::vector<FieldError> errors;
  CHECK(reader.Read(row, values, errors));
  CHECK(values.size() == 3u);
  CHECK(std::get<go2cpp::json::core::Text>(values[0]).view() == "server");
  CHECK(std::get<std::int64_t>(values[1]) == 8080);
  CHECK(std::get<bool>(values[2]) == true);
}

/// @brief 场景 7：错误链（解析失败带位置；Reader 失败带 cause）。
void TestErrorChain() {
  std::printf("\n[7] 错误链...\n");
  Document document;
  Parser parser;
  go2cpp::ErrorPtr error;
  CHECK(!parser.Parse(R"({"a":1,})", document, error));  // 尾逗号
  CHECK(error != nullptr);
  CHECK(!error->Message().empty());
  CHECK(document.Root().IsNull());

  Reader reader;
  reader.SetRules({{"age", kInt32Type, true, ""}});
  RowView<> row;
  row.Append("abc");
  std::vector<ItemValue> values;
  std::vector<FieldError> errors;
  CHECK(!reader.Read(row, values, errors));
  CHECK(errors.size() == 1u);
  CHECK(errors[0].cause != nullptr);
  CHECK(values.empty());
}

/// @brief 场景 8：随机端到端（生成 JSON → 解析 → 字段断言）。
void TestRandom() {
  std::printf("\n[8] 随机端到端...\n");
  constexpr int kRounds = 1000;
  unsigned seed = 20261106u;
  std::size_t mismatches = 0;

  for (int round = 0; round < kRounds; ++round) {
    seed = seed * 1103515245u + 12345u;
    const int port = static_cast<int>(seed % 60000) + 1000;
    const int number = static_cast<int>((seed >> 8) % 1000);
    const bool flag = ((seed >> 16) % 2) != 0;
    const std::string name = "user_" + std::to_string(round);

    std::string json = "{\"name\":\"" + name + "\",\"port\":" + std::to_string(port) +
                       ",\"n\":" + std::to_string(number) +
                       ",\"flag\":" + (flag ? "true" : "false") + "}";
    Document document;
    const Value& root = document.Root();
    if (!ParseOk(json, document)) {
      ++mismatches;
      continue;
    }
    std::string got_name;
    int got_port = 0;
    int got_n = 0;
    bool got_flag = false;
    if (!root["name"].As(got_name) || got_name != name) ++mismatches;
    if (!root["port"].As(got_port) || got_port != port) ++mismatches;
    if (!root["n"].As(got_n) || got_n != number) ++mismatches;
    if (!root["flag"].As(got_flag) || got_flag != flag) ++mismatches;
  }
  CHECK(mismatches == 0u);
  std::printf("  %d 轮随机 JSON，不一致 %zu\n", kRounds, mismatches);
}

}  // namespace

int main() {
  std::printf("=== 跨模块端到端测试 ===\n");
  TestConfig();
  TestApiResponse();
  TestLogLine();
  TestPrice();
  TestBigInteger();
  TestReaderEndToEnd();
  TestErrorChain();
  TestRandom();

  if (g_failures == 0) {
    std::printf("\nALL TESTS PASSED\n");
    return 0;
  }
  std::fprintf(stderr, "\n%d check(s) failed\n", g_failures);
  return 1;
}
