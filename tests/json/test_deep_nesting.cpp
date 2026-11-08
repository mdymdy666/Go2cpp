/**
 * @file test_deep_nesting.cpp
 * @brief J1 深嵌套优化：链式闭合、帧就地构造、节点槽复用（bare kind）的正确性与边界。
 *
 * ## 覆盖的新路径
 * - `BasicParser::CloseChain`：深栈（`stack.size() >= 8`）时连续闭合 + 就地消费 `,`；
 * - `detail::EmplaceFrame`：帧在容器内就地构造（不再经过栈上临时 Frame）；
 * - `Document::PushNodeSlotBare` + `Node::BareKindTag`：节点槽只写 `kind`，
 *   24 B payload 由写者随后整体覆盖。
 *
 * ## 边界
 * 内联/堆切换（深度 8/9）、`max_depth` 防线、每层带兄弟成员的深嵌套、
 * 空容器、转义键、错误恢复路径、Writer round-trip。
 */

#include <cstdint>
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
using go2cpp::json::JsonError;
using go2cpp::json::JsonFailure;
using go2cpp::json::Parser;
using go2cpp::json::ParserConfig;
using go2cpp::json::Value;

/// @brief `depth` 层 `{"k": ... }`，最内层是整数 `1`。
std::string MakeDeepObject(int depth) {
  std::string text;
  text.reserve(static_cast<std::size_t>(depth) * 6 + 1);
  for (int i = 0; i < depth; ++i) text += "{\"k\":";
  text += '1';
  for (int i = 0; i < depth; ++i) text += '}';
  return text;
}

/// @brief `depth` 层 `{"k": ... }`，最内层是空对象 `{}`。
std::string MakeDeepEmptyObject(int depth) {
  std::string text;
  text.reserve(static_cast<std::size_t>(depth) * 6 + 2);
  for (int i = 0; i < depth; ++i) text += "{\"k\":";
  text += "{}";
  for (int i = 0; i < depth; ++i) text += '}';
  return text;
}

/// @brief `depth` 层 `[ ... ]`，最内层是整数 `1`。
std::string MakeDeepArray(int depth) {
  std::string text;
  text.reserve(static_cast<std::size_t>(depth) * 2 + 1);
  for (int i = 0; i < depth; ++i) text += '[';
  text += '1';
  for (int i = 0; i < depth; ++i) text += ']';
  return text;
}

/// @brief 每层 `{"k": <child>, "s": i}`；最内层是空对象 `{}`。
std::string MakeDeepWithSiblings(int depth) {
  std::string text;
  for (int i = 0; i < depth; ++i) text += "{\"k\":";
  text += "{}";
  for (int i = depth - 1; i >= 0; --i) {
    text += ",\"s\":";
    text += std::to_string(i);
    text += '}';
  }
  return text;
}

/// @brief 逐层下钻 `["k"]` 并校验每层结构。
void DrillObject(const Value& root, int depth) {
  Value current = root;
  for (int i = 0; i < depth; ++i) {
    CHECK(current.IsObject());
    CHECK(current.Size() == 1);
    if (!current.IsObject() || current.Size() != 1) return;
    CHECK(current.MemberName(0) == "k");
    current = current["k"];
  }
  CHECK(current.IsInt());
  int value = 0;
  CHECK(current.As(value));
  CHECK(value == 1);
}

/// @brief 逐层下钻 `[0]` 并校验每层结构。
void DrillArray(const Value& root, int depth) {
  Value current = root;
  for (int i = 0; i < depth; ++i) {
    CHECK(current.IsArray());
    CHECK(current.Size() == 1);
    if (!current.IsArray() || current.Size() != 1) return;
    current = current.At(0);
  }
  CHECK(current.IsInt());
  int value = 0;
  CHECK(current.As(value));
  CHECK(value == 1);
}

/// @brief 100 层对象：链式闭合的主要场景。
void TestDeepObject100() {
  std::printf("\n[1] 深嵌套对象 100 层...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;
  const std::string text = MakeDeepObject(100);
  CHECK(parser.Parse(text, doc, error));
  CHECK(doc.ValueCount() == 101);  // 100 个对象节点 + 1 个整数
  CHECK(doc.MemberCount() == 100);
  DrillObject(doc.Root(), 100);
}

/// @brief 100 层数组：数组帧走的链式闭合分支（`elements_` 提交路径不同）。
void TestDeepArray100() {
  std::printf("\n[2] 深嵌套数组 100 层...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;
  const std::string text = MakeDeepArray(100);
  CHECK(parser.Parse(text, doc, error));
  CHECK(doc.ValueCount() == 101);
  CHECK(doc.ElementCount() == 100);
  DrillArray(doc.Root(), 100);
}

/// @brief 内联/堆切换边界：8 层（全内联）与 9 层（首次迁移到堆）。
void TestInlineHeapBoundary() {
  std::printf("\n[3] 内联→堆切换边界（7/8/9/10/16/17/32/33 层）...\n");
  Parser parser;
  for (const int depth : {7, 8, 9, 10, 16, 17, 32, 33}) {
    Document doc;
    ErrorPtr error;
    const std::string text = MakeDeepObject(depth);
    CHECK(parser.Parse(text, doc, error));
    DrillObject(doc.Root(), depth);
  }
}

/// @brief 每层带兄弟成员：`{"k": <child>, "s": i}`，验证链式闭合与 `,` 交替。
void TestDeepWithSiblings() {
  std::printf("\n[4] 每层带兄弟成员（50 层）...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;
  constexpr int kDepth = 50;
  const std::string text = MakeDeepWithSiblings(kDepth);
  CHECK(parser.Parse(text, doc, error));
  CHECK(doc.Root().Size() == 2);

  Value current = doc.Root();
  for (int i = 0; i < kDepth; ++i) {
    CHECK(current.MemberName(0) == "k");
    CHECK(current.MemberName(1) == "s");
    int sibling = -1;
    CHECK(current["s"].As(sibling));
    CHECK(sibling == i);
    current = current["k"];
  }
  CHECK(current.IsObject());
  CHECK(current.Size() == 0);
}

/// @brief 连续闭合与逗号混合的边界形态（短文档，主状态机路径）。
void TestCloseChainEdges() {
  std::printf("\n[5] 闭合链边界形态...\n");
  Parser parser;

  // 对象里套空对象，再跟一个兄弟成员。
  {
    Document doc;
    ErrorPtr error;
    CHECK(parser.Parse("{\"a\":{\"b\":{}},\"c\":1}", doc, error));
    CHECK(doc.Root().Size() == 2);
    CHECK(doc.Root()["a"].IsObject());
    CHECK(doc.Root()["a"].Size() == 1);
    CHECK(doc.Root()["a"]["b"].IsObject());
    CHECK(doc.Root()["a"]["b"].Size() == 0);
    int c = 0;
    CHECK(doc.Root()["c"].As(c));
    CHECK(c == 1);
  }
  // 数组套数组，尾随兄弟元素。
  {
    Document doc;
    ErrorPtr error;
    CHECK(parser.Parse("[[[1],2],3]", doc, error));
    CHECK(doc.Root().Size() == 2);
    int last = 0;
    CHECK(doc.Root().At(1).As(last));
    CHECK(last == 3);
    CHECK(doc.Root().At(0).Size() == 2);
    CHECK(doc.Root().At(0).At(0).IsArray());
    CHECK(doc.Root().At(0).At(0).At(0).IsInt());
  }
  // 空容器并列：连续闭合后立刻是逗号。
  {
    Document doc;
    ErrorPtr error;
    CHECK(parser.Parse("[[],[],[]]", doc, error));
    CHECK(doc.Root().Size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
      CHECK(doc.Root().At(i).IsArray());
      CHECK(doc.Root().At(i).Size() == 0);
    }
  }
  // 深嵌套的最后一层是空容器：闭合链从最内层一路回到根。
  {
    Document doc;
    ErrorPtr error;
    const std::string text = MakeDeepEmptyObject(64);
    CHECK(parser.Parse(text, doc, error));
    Value current = doc.Root();
    for (int i = 0; i < 64; ++i) {
      CHECK(current.IsObject());
      CHECK(current.Size() == 1);
      if (!current.IsObject() || current.Size() != 1) break;
      current = current["k"];
    }
    CHECK(current.IsObject());
    CHECK(current.Size() == 0);
  }
  // 非法字节出现在闭合链上：错误位置必须指向该字节，而不是链尾。
  {
    Document doc;
    ErrorPtr error;
    CHECK(!parser.Parse("{\"a\":{\"b\":{}} x}", doc, error));
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr);
    if (json_error != nullptr) {
      CHECK(json_error->Failure() == JsonFailure::kUnexpectedChar);
      CHECK(json_error->Offset() == 14);  // 指向 'x'（空格在 13）
    }
  }
}

/// @brief 1 字节键、空键、转义键：键解析不依赖 payload 残留。
void TestKeyForms() {
  std::printf("\n[6] 键形态（空键/1 字节/转义）...\n");
  Parser parser;
  {
    Document doc;
    ErrorPtr error;
    CHECK(parser.Parse("{\"\":1}", doc, error));
    CHECK(doc.Root().Size() == 1);
    CHECK(doc.Root().MemberName(0).empty());
    int value = 0;
    CHECK(doc.Root()[""].As(value));
    CHECK(value == 1);
  }
  {
    Document doc;
    ErrorPtr error;
    CHECK(parser.Parse("{\"k\":{\"k\":{\"k\":1}}}", doc, error));
    DrillObject(doc.Root(), 3);
  }
  {
    Document doc;
    ErrorPtr error;
    CHECK(parser.Parse("{\"\\u006b\":{\"k\":1}}", doc, error));
    CHECK(doc.Root().MemberName(0) == "k");
    CHECK(doc.Root()["k"].IsObject());
  }
}

/// @brief 极深：1000 层（默认 max_depth=1024 以内），验证不会爆栈。
void TestVeryDeep() {
  std::printf("\n[7] 1000 层嵌套...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;
  const std::string text = MakeDeepObject(1000);
  CHECK(parser.Parse(text, doc, error));
  CHECK(doc.ValueCount() == 1001);  // 1000 个对象 + 1 个整数
  DrillObject(doc.Root(), 1000);
}

/// @brief 深度防线：`max_depth=100` 时 100 层通过、101 层报 `kDepthExceeded`。
void TestMaxDepth() {
  std::printf("\n[8] 深度防线...\n");
  Parser parser;
  ParserConfig config;
  config.max_depth = 100;
  {
    Document doc;
    ErrorPtr error;
    CHECK(parser.Parse(MakeDeepObject(100), doc, error, config));
  }
  {
    Document doc;
    ErrorPtr error;
    CHECK(!parser.Parse(MakeDeepObject(101), doc, error, config));
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr);
    if (json_error != nullptr) {
      CHECK(json_error->Failure() == JsonFailure::kDepthExceeded);
    }
    CHECK(doc.Empty());
  }
}

/// @brief 未闭合的深嵌套：失败路径必须回滚到空文档（`Parse` 的失败语义）。
void TestTruncated() {
  std::printf("\n[9] 截断的深嵌套...\n");
  Parser parser;
  const std::string full = MakeDeepObject(40);
  Document doc;
  ErrorPtr error;
  CHECK(!parser.Parse(std::string_view(full).substr(0, full.size() - 5), doc, error));
  CHECK(doc.Empty());
  CHECK(error != nullptr);
}

/// @brief 错误恢复路径（kSkipValue）不受链式闭合影响。
void TestRecovery() {
  std::printf("\n[10] 错误恢复 + 深嵌套...\n");
  Parser parser;
  ParserConfig config;
  config.error_policy = go2cpp::json::ErrorPolicy::kSkipValue;
  Document doc;
  ErrorPtr error;
  CHECK(parser.Parse("{\"a\":{\"b\":{\"c\":1}},\"d\":@,\"e\":2}", doc, error, config));
  CHECK(doc.Root().Size() == 3);
  CHECK(doc.Root()["a"]["b"]["c"].IsInt());
  CHECK(doc.Root()["d"].IsNull());
  int e = 0;
  CHECK(doc.Root()["e"].As(e));
  CHECK(e == 2);
  CHECK(doc.ErrorCount() == 1);
}

/// @brief Writer round-trip：100 层序列化后重新解析，结构逐层一致。
void TestRoundTrip() {
  std::printf("\n[11] Writer round-trip...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;
  const std::string text = MakeDeepObject(100);
  CHECK(parser.Parse(text, doc, error));
  const std::string written = go2cpp::json::Write(doc);
  CHECK(written == text);

  Document again;
  ErrorPtr error2;
  CHECK(parser.Parse(written, again, error2));
  DrillObject(again.Root(), 100);
}

/// @brief bare 节点槽不会漏写 payload：各类型分支逐值校验。
void TestAllKindPayloads() {
  std::printf("\n[12] 各类型 payload 完整性...\n");
  Parser parser;
  Document doc;
  ErrorPtr error;
  const char* text =
      "{\"n\":null,\"t\":true,\"f\":false,\"i\":-9223372036854775808,"
      "\"d\":3.14159265358979,\"s\":\"text\",\"a\":[1,2,3],\"o\":{\"x\":1}}";
  CHECK(parser.Parse(text, doc, error));
  CHECK(doc.Root().Size() == 8);
  CHECK(doc.Root()["n"].IsNull());
  bool boolean = false;
  CHECK(doc.Root()["t"].As(boolean));
  CHECK(boolean);
  boolean = true;
  CHECK(doc.Root()["f"].As(boolean));
  CHECK(!boolean);
  std::int64_t integer = 0;
  CHECK(doc.Root()["i"].As(integer));
  CHECK(integer == INT64_MIN);
  double real = 0.0;
  CHECK(doc.Root()["d"].As(real));
  CHECK(real == 3.14159265358979);
  std::string_view string_value;
  CHECK(doc.Root()["s"].As(string_value));
  CHECK(string_value == "text");
  CHECK(doc.Root()["a"].Size() == 3);
  CHECK(doc.Root()["a"].At(2).IsInt());
  CHECK(doc.Root()["o"]["x"].IsInt());
}

}  // namespace

int main() {
  TestDeepObject100();
  TestDeepArray100();
  TestInlineHeapBoundary();
  TestDeepWithSiblings();
  TestCloseChainEdges();
  TestKeyForms();
  TestVeryDeep();
  TestMaxDepth();
  TestTruncated();
  TestRecovery();
  TestRoundTrip();
  TestAllKindPayloads();

  if (g_failures != 0) {
    std::fprintf(stderr, "\n%d FAILURES (%d checks)\n", g_failures, g_checks);
    return 1;
  }
  std::printf("\nALL TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
