/**
 * @file test_writer.cpp
 * @brief 序列化 Writer 的往返、转义、数字精度与 pretty 测试。
 *
 * 覆盖：
 * - 标量：null / bool / int64 边界 / double（0、-0、1.0、0.1、1e20、1e-7、
 *   DBL_MAX、denormal）/ Decimal（超 int64 的 30 位整数）；
 * - 字符串：`"` `\` `\b \f \n \r \t`、全部 `< 0x20` 控制字符的 `\u00XX`、
 *   `/` 不转义、非 ASCII 默认原样 UTF-8、`escape_non_ascii` 模式写 `\uXXXX`
 *   （含代理对）、非法 UTF-8 字节原样保留；
 * - 对象 key 保持插入序（不排序）；
 * - pretty：缩进、空容器紧凑、嵌套数组/对象；
 * - round-trip：固定用例 + 随机数字 + 深嵌套（5000 层）；
 * - 真实场景（6 个）：配置文件重写、API 响应、日志字段、emoji 昵称、
 *   金额精度、NDJSON 单行。
 */

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/json/document.hpp"
#include "go2cpp/json/parser.hpp"
#include "go2cpp/json/value.hpp"
#include "go2cpp/json/writer.hpp"

namespace {

int g_failures = 0;
std::size_t g_checks = 0;

#define CHECK(condition)                                                            \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(condition)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (false)

using go2cpp::json::core::Decimal;
using go2cpp::json::Document;
using go2cpp::json::Parser;
using go2cpp::json::Value;
using go2cpp::json::Write;
using go2cpp::json::WriterOptions;

/// @brief 解析成功并返回写入结果（输入与输出都借给 Document）。
bool RoundTrip(std::string_view text, std::string* written) {
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  if (!parser.Parse(text, document, error)) {
    return false;
  }
  *written = Write(document);
  return true;
}

/// @brief 递归比较两棵值树（类型 + 顺序 + 位级数值）。
bool SameValue(const Value& lhs, const Value& rhs) {
  if (lhs.IsNull() != rhs.IsNull()) {
    return false;
  }
  if (lhs.IsNull()) {
    return true;
  }
  if (lhs.IsBool() != rhs.IsBool()) {
    return false;
  }
  if (lhs.IsBool()) {
    return lhs.As<bool>() == rhs.As<bool>();
  }
  if (lhs.IsInt() != rhs.IsInt()) {
    return false;
  }
  if (lhs.IsInt()) {
    return lhs.As<std::int64_t>() == rhs.As<std::int64_t>();
  }
  if (lhs.IsDouble() != rhs.IsDouble()) {
    return false;
  }
  if (lhs.IsDouble()) {
    const double left = lhs.As<double>();
    const double right = rhs.As<double>();
    return left == right && std::signbit(left) == std::signbit(right);
  }
  if (lhs.IsDecimal() != rhs.IsDecimal()) {
    return false;
  }
  if (lhs.IsDecimal()) {
    Decimal left;
    Decimal right;
    return lhs.As(left) && rhs.As(right) && left == right;
  }
  if (lhs.IsString() != rhs.IsString()) {
    return false;
  }
  if (lhs.IsString()) {
    return lhs.AsText() == rhs.AsText();
  }
  if (lhs.IsArray() != rhs.IsArray()) {
    return false;
  }
  if (lhs.IsObject() != rhs.IsObject()) {
    return false;
  }
  if (!lhs.IsArray() && !lhs.IsObject()) {
    return false;  // 未知节点类型。
  }
  if (lhs.Size() != rhs.Size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.Size(); ++index) {
    if (lhs.IsObject() && lhs.MemberName(index) != rhs.MemberName(index)) {
      return false;
    }
    if (!SameValue(lhs.At(index), rhs.At(index))) {
      return false;
    }
  }
  return true;
}

/// @brief 解析 → 写 → 再解析，要求两棵树完全一致；返回写出的文本。
bool CheckRoundTrip(std::string_view text, std::string* written) {
  Parser parser;
  Document first;
  go2cpp::ErrorPtr error;
  if (!parser.Parse(text, first, error)) {
    std::fprintf(stderr, "round-trip: 原文本解析失败: %.120s\n", std::string(text).c_str());
    return false;
  }
  *written = Write(first);
  Document second;
  if (!parser.Parse(*written, second, error)) {
    std::fprintf(stderr, "round-trip: 写出文本无法解析: %.160s\n", written->c_str());
    return false;
  }
  if (!SameValue(first.Root(), second.Root())) {
    std::fprintf(stderr, "round-trip: 树不一致\n  in : %.120s\n  out: %.160s\n",
                 std::string(text).c_str(), written->c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 1. 标量
// ---------------------------------------------------------------------------

void TestScalars() {
  std::printf("\n[1] 标量输出...\n");
  struct Case {
    const char* input;
    const char* expected;
  };
  const Case cases[] = {
      {"null", "null"},
      {"true", "true"},
      {"false", "false"},
      {"0", "0"},
      {"-1", "-1"},
      {"123", "123"},
      {"-9223372036854775808", "-9223372036854775808"},
      {"9223372036854775807", "9223372036854775807"},
      {"0.0", "0.0"},
      {"-0.0", "-0.0"},
      {"1.0", "1.0"},
      {"1.5", "1.5"},
      {"0.1", "0.1"},
      {"3.141592653589793", "3.141592653589793"},
      {"1e20", "1e+20"},
      {"1e-7", "1e-07"},
      {"1.7976931348623157e308", "1.7976931348623157e+308"},
      {"5e-324", "5e-324"},
      {"\"\"", "\"\""},
      {"\"abc\"", "\"abc\""},
      {"[]", "[]"},
      {"{}", "{}"},
  };
  for (const Case& item : cases) {
    std::string written;
    CHECK(RoundTrip(item.input, &written));
    if (written != item.expected) {
      std::fprintf(stderr, "标量输出不符: %s → %s（期望 %s）\n", item.input, written.c_str(),
                   item.expected);
      ++g_failures;
    }
    ++g_checks;
  }

  // 超 int64 的整数：走 Decimal，保留全部十进制位。
  std::string written;
  CHECK(RoundTrip("123456789012345678901234567890", &written));
  CHECK(written == "123456789012345678901234567890");
  CHECK(RoundTrip("-123456789012345678901234567890", &written));
  CHECK(written == "-123456789012345678901234567890");

  // 超出 double 精度的整数：仍是 Decimal，不是近似 double。
  CHECK(RoundTrip("18446744073709551616", &written));
  CHECK(written == "18446744073709551616");
}

// ---------------------------------------------------------------------------
// 2. 字符串转义
// ---------------------------------------------------------------------------

void TestStringEscapes() {
  std::printf("\n[2] 字符串转义...\n");
  // 直接构造包含全部控制字符的 JSON 文本（用 \u 输入，避免裸控制字符）。
  std::string input = "\"";
  for (int code = 0; code < 0x20; ++code) {
    char escape[8];
    std::snprintf(escape, sizeof(escape), "\\u%04X", code);
    input += escape;
  }
  input += "\"";
  std::string written;
  CHECK(RoundTrip(input, &written));
  // 期望：\b \f \n \r \t 用短形式，其余 \u00XX。
  const std::string expected =
      "\"\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\b\\t\\n\\u000B\\f\\r"
      "\\u000E\\u000F\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017\\u0018"
      "\\u0019\\u001A\\u001B\\u001C\\u001D\\u001E\\u001F\"";
  CHECK(written == expected);

  // 引号、反斜杠、斜杠。
  CHECK(RoundTrip(R"("a\"b\\c\/d")", &written));
  CHECK(written == R"("a\"b\\c/d")");  // `/` 不需要转义。

  // 非 ASCII 默认原样 UTF-8。
  CHECK(RoundTrip("\"caf\xC3\xA9\"", &written));
  CHECK(written == "\"caf\xC3\xA9\"");
  CHECK(RoundTrip("\"\xF0\x9F\x98\x80\"", &written));
  CHECK(written == "\"\xF0\x9F\x98\x80\"");

  // escape_non_ascii：BMP 与代理对。
  {
    Parser parser;
    Document document;
    go2cpp::ErrorPtr error;
    CHECK(parser.Parse("\"caf\xC3\xA9 \xF0\x9F\x98\x80\"", document, error));
    const std::string escaped = Write(document, WriterOptions{0, true});
    CHECK(escaped == R"("caf\u00E9 \uD83D\uDE00")");
    // 转义结果必须能解析回同样的 UTF-8。
    Document reparsed;
    CHECK(parser.Parse(escaped, reparsed, error));
    CHECK(reparsed.Root().AsText() == document.Root().AsText());
  }

  // key 同样转义。
  {
    Parser parser;
    Document document;
    go2cpp::ErrorPtr error;
    CHECK(parser.Parse("{\"\\u4E2D\":1}", document, error));
    CHECK(document.Root().MemberName(0) == "\xE4\xB8\xAD");
    const std::string escaped = Write(document, WriterOptions{0, true});
    CHECK(escaped == R"({"\u4E2D":1})");
  }

  // 非法 UTF-8 字节：默认原样保留；转义模式也保留（不丢数据）。
  {
    Parser parser;
    Document document;
    go2cpp::ErrorPtr error;
    const std::string raw = "\"a\xFF" "b\"";
    CHECK(parser.Parse(raw, document, error));
    CHECK(Write(document) == raw);
    CHECK(Write(document, WriterOptions{0, true}) == raw);
  }

  // 0x7F（DEL）不是控制字符，原样输出。
  CHECK(RoundTrip("\"\x7F\"", &written));
  CHECK(written == "\"\x7F\"");
}

// ---------------------------------------------------------------------------
// 3. 插入序与 pretty
// ---------------------------------------------------------------------------

void TestOrderAndPretty() {
  std::printf("\n[3] 插入序与 pretty...\n");
  const char* text = R"({"z":1,"a":2,"m":3,"b":{"y":null,"x":true}})";
  std::string written;
  CHECK(RoundTrip(text, &written));
  // 不排序：z,a,m,b 原序；嵌套 y,x 原序。
  CHECK(written == R"({"z":1,"a":2,"m":3,"b":{"y":null,"x":true}})");

  // pretty：2 空格缩进。
  {
    Parser parser;
    Document document;
    go2cpp::ErrorPtr error;
    CHECK(parser.Parse(R"({"a":1,"b":[true,null],"c":{}})", document, error));
    const std::string pretty = Write(document, WriterOptions{2, false});
    const std::string expected =
        "{\n"
        "  \"a\": 1,\n"
        "  \"b\": [\n"
        "    true,\n"
        "    null\n"
        "  ],\n"
        "  \"c\": {}\n"
        "}";
    CHECK(pretty == expected);

    // pretty 后仍可解析，且值一致。
    Document reparsed;
    CHECK(parser.Parse(pretty, reparsed, error));
    CHECK(SameValue(document.Root(), reparsed.Root()));

    // 空数组/空对象紧凑。
    CHECK(parser.Parse(R"({"a":[],"b":{}})", document, error));
    CHECK(Write(document, WriterOptions{4, false}) ==
          "{\n    \"a\": [],\n    \"b\": {}\n}");
  }

  // indent=0 紧凑；负数按 0 处理。
  {
    Parser parser;
    Document document;
    go2cpp::ErrorPtr error;
    CHECK(parser.Parse("[1,[2,3]]", document, error));
    CHECK(Write(document, WriterOptions{0, false}) == "[1,[2,3]]");
    CHECK(Write(document, WriterOptions{-5, false}) == "[1,[2,3]]");
    CHECK(Write(document, WriterOptions{1, false}) == "[\n 1,\n [\n  2,\n  3\n ]\n]");
  }
}

// ---------------------------------------------------------------------------
// 4. 往返：固定 + 随机 + 深嵌套
// ---------------------------------------------------------------------------

void TestRoundTripFixed() {
  std::printf("\n[4] 固定用例 round-trip...\n");
  const char* cases[] = {
      "null",
      "[]",
      "{}",
      "[1,2,3]",
      R"({"a":1,"b":2.5,"c":"x","d":true,"e":null})",
      R"({"nested":{"arr":[1,{"k":"v"},[2,3]]},"empty":[],"nul":null})",
      R"("hello \u4E2D\u6587 \uD83D\uDE00")",
      R"({"long":123456789012345678901234567890})",
      R"({"float":1.2345678901234567})",
      R"({"ctrl":"\u0000\u001F\n\t"})",
      R"({"a":0.000001,"b":1e30,"c":-0.0,"d":1234567890123})",
  };
  for (const char* text : cases) {
    std::string written;
    CHECK(CheckRoundTrip(text, &written));
    // 二次往返必须稳定（写出的文本再过一次 writer 不变）。
    std::string again;
    CHECK(RoundTrip(written, &again));
    CHECK(again == written);
  }
}

void TestRoundTripRandomNumbers() {
  std::printf("\n[5] 随机 double round-trip（5000 组，位级一致）...\n");
  std::mt19937_64 engine(20261107);
  std::uniform_real_distribution<double> distribution(-1e300, 1e300);
  for (int iteration = 0; iteration < 5000; ++iteration) {
    double value = distribution(engine);
    if (!std::isfinite(value)) {
      continue;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.17g", value);
    std::string written;
    CHECK(RoundTrip(std::string(text), &written));
    Parser parser;
    Document parsed;
    go2cpp::ErrorPtr error;
    CHECK(parser.Parse(written, parsed, error));
    const double back = parsed.Root().As<double>();
    CHECK(back == value);
    CHECK(std::signbit(back) == std::signbit(value));
  }

  // 随机 int64。
  std::uniform_int_distribution<std::int64_t> ints(std::numeric_limits<std::int64_t>::min(),
                                                   std::numeric_limits<std::int64_t>::max());
  for (int iteration = 0; iteration < 2000; ++iteration) {
    const std::int64_t value = ints(engine);
    std::string written;
    CHECK(RoundTrip(std::to_string(value), &written));
    CHECK(written == std::to_string(value));
  }
}

void TestDeepNesting() {
  std::printf("\n[6] 深嵌套（5000 层，迭代 writer 不爆栈）...\n");
  constexpr int kDepth = 5000;
  std::string text;
  text.reserve(static_cast<std::size_t>(kDepth) * 6 + 8);
  for (int index = 0; index < kDepth; ++index) {
    text += "{\"k\":";
  }
  text += '1';
  for (int index = 0; index < kDepth; ++index) {
    text += '}';
  }
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  go2cpp::json::ParserConfig config;
  config.max_depth = kDepth + 10;
  CHECK(parser.Parse(text, document, error, config));
  const std::string written = Write(document);
  CHECK(written.size() == text.size());
  Document reparsed;
  CHECK(parser.Parse(written, reparsed, error, config));
  CHECK(SameValue(document.Root(), reparsed.Root()));
}

// ---------------------------------------------------------------------------
// 5. 真实场景（6 个）
// ---------------------------------------------------------------------------

void ScenarioConfigRewrite() {
  std::printf("\n[7] 场景：配置文件重写（顺序保持）...\n");
  const char* text = R"({
  "server": {"host": "127.0.0.1", "port": 8080, "tls": false},
  "limits": {"max_conn": 1024, "timeout_ms": 5000},
  "tags": ["prod", "cn-north"]
})";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  CHECK(parser.Parse(text, document, error));
  const std::string compact = Write(document);
  CHECK(compact ==
        R"({"server":{"host":"127.0.0.1","port":8080,"tls":false},)"
        R"("limits":{"max_conn":1024,"timeout_ms":5000},)"
        R"("tags":["prod","cn-north"]})");
  // 逐 key 对比：写出的顺序与原文件一致。
  const std::string_view expected_keys[] = {"server", "limits", "tags"};
  for (std::size_t index = 0; index < 3; ++index) {
    CHECK(document.Root().MemberName(index) == expected_keys[index]);
  }
}

void ScenarioApiResponse() {
  std::printf("\n[8] 场景：API 响应（嵌套 + 中文 + 转义）...\n");
  const char* text =
      R"({"code":0,"msg":"\u6210\u529F","data":{"items":[{"id":1,"name":"\u5F20\u4E09"},)"
      R"({"id":2,"name":"\u674E\u56DB"}],"total":2}})";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  CHECK(parser.Parse(text, document, error));
  const std::string written = Write(document);
  // 默认原样 UTF-8：中文以字节形式输出，不再有 \u。
  CHECK(written.find("\\u") == std::string::npos);
  CHECK(written.find("\xE6\x88\x90\xE5\x8A\x9F") != std::string::npos);
  Document reparsed;
  CHECK(parser.Parse(written, reparsed, error));
  CHECK(SameValue(document.Root(), reparsed.Root()));
  // 可读与可转义两种形态。
  const std::string escaped = Write(document, WriterOptions{0, true});
  CHECK(escaped.find(R"(\u6210\u529F)") != std::string::npos);
}

void ScenarioLogField() {
  std::printf("\n[9] 场景：日志字段（换行/制表/引号必须转义）...\n");
  const char* text = R"({"level":"error","message":"line1\nline2\t\"quoted\""})";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  CHECK(parser.Parse(text, document, error));
  const std::string written = Write(document);
  CHECK(written == R"({"level":"error","message":"line1\nline2\t\"quoted\""})");
  // 单行保证：写出文本里没有裸换行。
  CHECK(written.find('\n') == std::string::npos);
}

void ScenarioEmojiNickname() {
  std::printf("\n[10] 场景：emoji 昵称往返...\n");
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  CHECK(parser.Parse(R"({"nick":"\uD83D\uDE00\uD83D\uDC4D"})", document, error));
  const std::string plain = Write(document);
  CHECK(plain == "{\"nick\":\"\xF0\x9F\x98\x80\xF0\x9F\x91\x8D\"}");
  const std::string escaped = Write(document, WriterOptions{0, true});
  CHECK(escaped == R"({"nick":"\uD83D\uDE00\uD83D\uDC4D"})");
}

void ScenarioMoney() {
  std::printf("\n[11] 场景：金额与精度（不引入浮点噪声）...\n");
  const char* text = R"({"price":19.99,"qty":3,"total":"59.97","big":123456789012345678901})";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  CHECK(parser.Parse(text, document, error));
  const std::string written = Write(document);
  CHECK(written == R"({"price":19.99,"qty":3,"total":"59.97",)"
                    R"("big":123456789012345678901})");
  Document reparsed;
  CHECK(parser.Parse(written, reparsed, error));
  CHECK(reparsed.Root()["price"].As<double>() == 19.99);
  CHECK(reparsed.Root()["big"].IsDecimal());
}

void ScenarioNdjsonLine() {
  std::printf("\n[12] 场景：NDJSON 单行输出...\n");
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  CHECK(parser.Parse(R"({"id":7,"name":"user_7","ok":true})", document, error));
  const std::string line = Write(document) + "\n";
  CHECK(line == "{\"id\":7,\"name\":\"user_7\",\"ok\":true}\n");
  // 用 ParseMany 验证写出的行能被逐行解析（读写闭环）。
  std::size_t count = 0;
  CHECK(parser.ParseMany(line, [&count](Document&) {
    ++count;
    return true;
  }, error) == 1);
  CHECK(count == 1);
}

}  // namespace

int main() {
  const auto begin = std::chrono::steady_clock::now();

  TestScalars();
  TestStringEscapes();
  TestOrderAndPretty();
  TestRoundTripFixed();
  TestRoundTripRandomNumbers();
  TestDeepNesting();
  ScenarioConfigRewrite();
  ScenarioApiResponse();
  ScenarioLogField();
  ScenarioEmojiNickname();
  ScenarioMoney();
  ScenarioNdjsonLine();

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
