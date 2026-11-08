/**
 * @file test_json_parser.cpp
 * @brief JSON 解析与值模型的回归测试（P2 / P6b：Document + 扁平池 API）。
 *
 * 覆盖：
 * - 基本类型：null/bool/int64/double/Decimal/string/array/object 与 `As<T>` 读取；
 * - 字符串：全部 8 种转义、`\uXXXX`、代理对 UTF-8、NUL、裸控制字符；
 * - 数字：int64 边界、int64 溢出 → Decimal、double 溢出/下溢 → Decimal；
 * - 嵌套：对象套数组、数组套对象、空容器、深层容器；
 * - 非法输入：未闭合、尾逗号、非法字符、深度超限、非法转义、裸控制字符、
 *   尾随垃圾、非法数字；逐项断言 `JsonFailure` 与 offset/line/column；
 * - CRLF 与 LF：同一 JSON 两种换行解析结果逐值相等；错误行列一致；
 * - 三 Policy：Inline/Std/Compact 对同一输入的解析结果与错误位置一致；
 * - 零拷贝：无转义字符串与对象键指向输入缓冲，有转义时指向文档字符串池；
 * - 深嵌套：10 万层数组解析后逐层下钻；默认深度拒绝；解析 → 清空 → 复用；
 * - 随机：2 万轮随机 JSON（生成器同时给出期望树），逐值比较；
 * - 真实场景（6 个）：配置、API 响应、日志事件、Decimal 价格、大整数 ID、多语言。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/json/core/text.hpp"
#include "go2cpp/tools/error.hpp"
#include "go2cpp/json/config.hpp"
#include "go2cpp/json/document.hpp"
#include "go2cpp/json/error.hpp"
#include "go2cpp/json/parser.hpp"
#include "go2cpp/json/value.hpp"

namespace {

std::size_t g_checks = 0;
std::size_t g_failures = 0;

/// @brief 断言；失败时打印位置并累计失败数，不中断后续用例。
#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,   \
                   #cond);                                                     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

using go2cpp::ErrorMessage;
using go2cpp::ErrorPtr;
using go2cpp::json::core::Decimal;
using go2cpp::json::core::Text;
using go2cpp::json::BasicParser;
using go2cpp::json::CompactPolicy;
using go2cpp::json::Document;
using go2cpp::json::DuplicateKeyPolicy;
using go2cpp::json::ErrorPolicy;
using go2cpp::json::InlinePolicy;
using go2cpp::json::JsonError;
using go2cpp::json::JsonFailure;
using go2cpp::json::Parser;
using go2cpp::json::ParserConfig;
using go2cpp::json::StdPolicy;
using go2cpp::json::Value;

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

/// @brief 解析；失败时打印文本与错误并计一次失败。
bool ParseText(std::string_view text, Document& out,
               const ParserConfig& config = ParserConfig{}) {
  Parser parser;
  ErrorPtr error;
  if (parser.Parse(text, out, error, config)) {
    return true;
  }
  ++g_failures;
  std::fprintf(stderr, "CHECK failed: 解析 \"%.120s\" 失败: %s\n", std::string(text).c_str(),
               ErrorMessage(error).c_str());
  return false;
}

/// @brief 断言解析失败，并精确检查失败原因与行列/偏移。
void CheckError(std::string_view text, JsonFailure expected_failure,
                std::size_t expected_offset, std::size_t expected_line,
                std::size_t expected_column, const ParserConfig& config = ParserConfig{}) {
  ++g_checks;
  Parser parser;
  Document document;
  ErrorPtr error;
  const bool ok = parser.Parse(text, document, error, config);
  if (ok) {
    std::fprintf(stderr, "CHECK failed: \"%.120s\" 期望失败却成功\n",
                 std::string(text).c_str());
    ++g_failures;
    return;
  }
  const auto* json_error = dynamic_cast<const JsonError*>(error.get());
  if (json_error == nullptr) {
    std::fprintf(stderr, "CHECK failed: \"%.120s\" 错误类型不是 JsonError: %s\n",
                 std::string(text).c_str(), ErrorMessage(error).c_str());
    ++g_failures;
    return;
  }
  if (json_error->Failure() != expected_failure || json_error->Offset() != expected_offset ||
      json_error->Line() != expected_line || json_error->Column() != expected_column) {
    std::fprintf(stderr,
                 "CHECK failed: \"%.120s\" 错误=%s(%zu,%zu,%zu)，期望 %s(%zu,%zu,%zu)\n",
                 std::string(text).c_str(), go2cpp::json::JsonFailureName(json_error->Failure()),
                 json_error->Offset(), json_error->Line(), json_error->Column(),
                 go2cpp::json::JsonFailureName(expected_failure), expected_offset,
                 expected_line, expected_column);
    ++g_failures;
    return;
  }
  // 失败时文档必须是可析构的确定状态（空文档）。
  if (!document.Root().IsNull()) {
    std::fprintf(stderr, "CHECK failed: \"%.120s\" 失败后根不是 null\n",
                 std::string(text).c_str());
    ++g_failures;
  }
}

/// @brief 逐值深比较（类型 + 内容 + 容器顺序）；测试树深度有限，可用递归。
bool DeepEqual(const Value& lhs, const Value& rhs) {
  if (lhs.IsNull() || rhs.IsNull()) {
    return lhs.IsNull() && rhs.IsNull();
  }
  if (lhs.IsBool()) {
    return rhs.IsBool() && lhs.As<bool>() == rhs.As<bool>();
  }
  if (lhs.IsInt()) {
    return rhs.IsInt() && lhs.As<std::int64_t>() == rhs.As<std::int64_t>();
  }
  if (lhs.IsDouble()) {
    return rhs.IsDouble() && lhs.As<double>() == rhs.As<double>();
  }
  if (lhs.IsDecimal()) {
    return rhs.IsDecimal() && lhs.As<Decimal>() == rhs.As<Decimal>();
  }
  if (lhs.IsString()) {
    return rhs.IsString() && lhs.AsText() == rhs.AsText();
  }
  if (lhs.IsArray()) {
    if (!rhs.IsArray() || lhs.Size() != rhs.Size()) {
      return false;
    }
    for (std::size_t index = 0; index < lhs.Size(); ++index) {
      if (!DeepEqual(lhs.At(index), rhs.At(index))) {
        return false;
      }
    }
    return true;
  }
  if (lhs.IsObject()) {
    if (!rhs.IsObject() || lhs.Size() != rhs.Size()) {
      return false;
    }
    for (std::size_t index = 0; index < lhs.Size(); ++index) {
      if (lhs.MemberName(index) != rhs.MemberName(index)) {
        return false;
      }
      if (!DeepEqual(lhs.At(index), rhs.At(index))) {
        return false;
      }
    }
    return true;
  }
  return false;
}

/// @brief 把同一个字符串里所有 `\n` 替换成 `\r\n`（用于 CRLF 对照）。
std::string ToCrlf(std::string_view text) {
  std::string out;
  out.reserve(text.size() + text.size() / 8 + 4);
  for (const char value : text) {
    if (value == '\n') {
      out += "\r\n";
    } else {
      out.push_back(value);
    }
  }
  return out;
}

/// @brief 用显式字节值构造字符串（避免 `\xNN` 十六进制转义贪心吞掉后续字符）。
std::string Bytes(std::initializer_list<unsigned int> bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (const unsigned int value : bytes) {
    out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
  }
  return out;
}

/// @brief 把内容原样包成 JSON 字符串字面量（调用方保证内容不含 `"`/`\`/控制字符）。
std::string Quote(std::string_view content) { return "\"" + std::string(content) + "\""; }

// ---------------------------------------------------------------------------
// 基本类型
// ---------------------------------------------------------------------------

void TestBasicTypes() {
  Document document;
  const Value& root = document.Root();
  ErrorPtr error;

  CHECK(ParseText("null", document) && root.IsNull());
  CHECK(ParseText("true", document) && root.IsBool() && root.As<bool>());
  CHECK(ParseText("false", document) && root.IsBool() && !root.As<bool>());
  CHECK(ParseText("0", document) && root.IsInt() && root.As<std::int64_t>() == 0);
  CHECK(ParseText("-42", document) && root.IsInt() && root.As<std::int64_t>() == -42);
  CHECK(ParseText("9223372036854775807", document) && root.IsInt() &&
        root.As<std::int64_t>() == 9223372036854775807LL);
  CHECK(ParseText("-9223372036854775808", document) && root.IsInt() &&
        root.As<std::int64_t>() == (-9223372036854775807LL - 1));
  CHECK(ParseText("3.5", document) && root.IsDouble() && root.As<double>() == 3.5);
  CHECK(ParseText("-1.25e2", document) && root.IsDouble() && root.As<double>() == -125.0);

  CHECK(ParseText("\"hello\"", document) && root.IsString() && root.AsText() == "hello");
  CHECK(ParseText("[]", document) && root.IsArray() && root.Size() == 0);
  CHECK(ParseText("{}", document) && root.IsObject() && root.Size() == 0);

  CHECK(ParseText(R"({"a":1,"b":[true,null,"x"]})", document) && root.IsObject() &&
        root.Size() == 2);
  CHECK(root["a"].IsInt() && root["a"].As<std::int64_t>() == 1);
  CHECK(root["b"].IsArray() && root["b"].Size() == 3);
  CHECK(root["b"].At(0).As<bool>());
  CHECK(root["b"].At(1).IsNull());
  CHECK(root["b"].At(2).AsText() == "x");
  CHECK(root["missing"].IsNull());
  CHECK(root["b"].At(99).IsNull());

  // As<T> 的类型不匹配必须失败且不写输出。
  std::int64_t integer = 12345;
  CHECK(!root["b"].At(2).As(integer));
  CHECK(integer == 12345);
  std::string text = "unchanged";
  CHECK(!root["b"].At(0).As(text));
  CHECK(text == "unchanged");

  // 数字的跨类型读取：int64 -> double、double -> int64（只接受整值）。
  CHECK(ParseText("7", document));
  CHECK(root.As<double>() == 7.0);
  CHECK(ParseText("7.0", document));
  std::int64_t converted = 0;
  CHECK(root.As(converted) && converted == 7);
  CHECK(ParseText("7.5", document));
  CHECK(!root.As(converted));

  // 字符串来源走 conversion：bool / 整数 / 浮点。
  CHECK(ParseText("\"true\"", document));
  bool boolean = false;
  CHECK(root.As(boolean) && boolean);
  CHECK(ParseText("\"-17\"", document));
  CHECK(root.As(converted) && converted == -17);
  double floating = 0.0;
  CHECK(ParseText("\"2.5\"", document));
  CHECK(root.As(floating) && floating == 2.5);
  CHECK(ParseText("\"0\"", document));
  CHECK(root.As(boolean) && !boolean);
}

void TestStrings() {
  Document document;
  const Value& root = document.Root();
  CHECK(ParseText(R"("a\"b\\c\/d\be\ff\ng\rh\ti")", document));
  CHECK(root.AsText() == "a\"b\\c/d\be\ff\ng\rh\ti");

  CHECK(ParseText(R"("\u0041\u00E9\u4E2D\u6587")", document));
  CHECK(root.AsText() == "A\xC3\xA9\xE4\xB8\xAD\xE6\x96\x87");
  CHECK(ParseText(R"("\uD83D\uDE00")", document));
  CHECK(root.AsText() == "\xF0\x9F\x98\x80");
  CHECK(ParseText(R"("\u0000")", document) && root.AsText().size() == 1 &&
        root.AsText()[0] == '\0');

  // 空字符串：借用，且内容为空。
  CHECK(ParseText("\"\"", document) && root.AsText().empty());
  CHECK(ParseText("\"\\u4E2D\"", document) && root.AsText() == "\xE4\xB8\xAD");

  // 未转义的 UTF-8 原样保留。
  CHECK(ParseText("\"h\xC3\xA9llo\"", document) && root.AsText() == "h\xC3\xA9llo");

  // 字符串里允许出现的空格、斜杠等。
  CHECK(ParseText("\" a / b \"", document) && root.AsText() == " a / b ");
}

/**
 * @brief 覆盖 `ScanPlainRun` 的 16 字节 SSE2 块边界与尾部回退。
 *
 * @details 扫描器一次并行判 16 字节的「引号 / 反斜杠 / 控制字符」，所以内容
 *          长度落在 `[0,15]`、`[16,31]`、`[32,47]` 时分别走 1/2/3 个块；
 *          字符串贴在输入末尾（`end - cursor < 16`）时还要进入
 *          `ScanPlainRunTail` 的 8 字节 SWAR 与逐字节回退。这里把这两条路径
 *          按长度逐个打穿，并额外钉住两个容易被 SIMD 写错的性质：
 *
 *          - `psubusb` 是**无符号**饱和减，`0x80`..`0xFF` 不会被误判成
 *            `< 0x20` 的控制字符（换成有符号 `pcmpgtb` 就会误判）；
 *          - `0x00`..`0x1F` 的裸控制字节必须触发回退并报 `kUnescapedControl`。
 *
 * @note 失败路径（裸控制字符）走的是「快路径放弃 → 通用状态机重放」，
 *       所以这里断言的是**通用状态机**给出的错误码，与快路径无关。
 */
void TestScanBlockBoundaries() {
  Parser parser;
  Document document;
  ErrorPtr error;

  // step1: 字符串值内容长度 0..47 —— 覆盖 1/2/3 个 16 字节块。
  for (std::size_t length = 0; length <= 47; ++length) {
    const std::string json = "\"" + std::string(length, 'a') + "\"";
    CHECK(parser.Parse(json, document, error));
    CHECK(document.Root().AsText().size() == length);
  }

  // step2: 字符串贴在输入末尾，强制 `ScanPlainRunTail` 的 8 字节 / 逐字节回退。
  for (std::size_t length = 0; length <= 40; ++length) {
    const std::string json = "{\"k\":\"" + std::string(length, 'b') + "\"}";
    CHECK(parser.Parse(json, document, error));
    CHECK(document.Root().Size() == 1);
    CHECK(document.Root().Size() == 1 && document.Root()["k"].AsText().size() == length);
  }

  // step3: 对象键走同一条扫描路径，键长 0..47。
  for (std::size_t length = 0; length <= 47; ++length) {
    const std::string json = "{\"" + std::string(length, 'c') + "\":1}";
    CHECK(parser.Parse(json, document, error));
    CHECK(document.Root().Size() == 1);
    CHECK(document.Root().Size() == 1 && document.Root().MemberName(0).size() == length);
  }

  // step4: 0x7F..0xFF 的单字节不得被当成控制字符（`psubusb` 无符号饱和减）。
  //        `"` 与 `\` 是事件字节，由转义用例单独覆盖，这里跳过。
  for (unsigned int byte = 0x7Fu; byte <= 0xFFu; ++byte) {
    if (byte == 0x22u || byte == 0x5Cu) {
      continue;
    }
    const std::string json = "\"" + Bytes({byte}) + "\"";
    CHECK(parser.Parse(json, document, error));
    const std::string_view text = document.Root().AsText();
    CHECK(text.size() == 1);
    CHECK(text.size() == 1 && static_cast<unsigned char>(text[0]) == byte);
  }

  // step5: 0x00..0x1F 的裸控制字符必须失败并报 kUnescapedControl。
  for (unsigned int byte = 0; byte < 0x20u; ++byte) {
    const std::string json = "\"a" + Bytes({byte}) + "b\"";
    CHECK(!parser.Parse(json, document, error));
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr && json_error->Failure() == JsonFailure::kUnescapedControl);
  }

  // step6: 转义序列落在 16 字节块边界前后，且转义之后还有跨块的长内容。
  for (std::size_t prefix = 0; prefix <= 32; ++prefix) {
    const std::string json =
        "\"" + std::string(prefix, 'x') + "\\n" + std::string(20, 'y') + "\"";
    CHECK(parser.Parse(json, document, error));
    const std::string_view text = document.Root().AsText();
    CHECK(text.size() == prefix + 1 + 20);
    CHECK(text.size() == prefix + 1 + 20 && text[prefix] == '\n');
  }
}

void TestNumbers() {
  Document document;
  const Value& root = document.Root();
  CHECK(ParseText("123456789012345678901234567890", document) && root.IsDecimal());
  CHECK(root.As<Decimal>().ToString() == "123456789012345678901234567890");
  CHECK(ParseText("18446744073709551616", document) && root.IsDecimal());
  CHECK(root.As<Decimal>().ToString() == "18446744073709551616");
  CHECK(ParseText("1e400", document) && root.IsDecimal());
  CHECK(ParseText("1e-400", document) && root.IsDecimal());
  CHECK(ParseText("9007199254740993", document) && root.IsInt() &&
        root.As<std::int64_t>() == 9007199254740993LL);

  CHECK(ParseText("0.0", document) && root.IsDouble());
  CHECK(ParseText("-0", document) && root.IsInt() && root.As<std::int64_t>() == 0);
  CHECK(ParseText("1E+3", document) && root.IsDouble() && root.As<double>() == 1000.0);
  CHECK(ParseText("12.5e-1", document) && root.As<double>() == 1.25);

  // Decimal 的字符串读取（金额场景）。
  CHECK(ParseText("\"19.99\"", document));
  Decimal price;
  CHECK(root.As(price));
  CHECK(price.ToString() == "19.99");
}

// ---------------------------------------------------------------------------
// 非法输入
// ---------------------------------------------------------------------------

void TestInvalidInput() {
  // 未闭合
  CheckError("", JsonFailure::kUnexpectedEnd, 0, 1, 1);
  CheckError("[", JsonFailure::kUnexpectedEnd, 1, 1, 2);
  CheckError("{\"a\":1", JsonFailure::kUnexpectedEnd, 6, 1, 7);
  CheckError("\"abc", JsonFailure::kUnclosedString, 4, 1, 5);
  CheckError("\"abc\\", JsonFailure::kUnclosedString, 5, 1, 6);
  CheckError("\"\\u12", JsonFailure::kUnclosedString, 5, 1, 6);

  // 尾逗号
  CheckError("[1,]", JsonFailure::kTrailingComma, 3, 1, 4);
  CheckError("{\"a\":1,}", JsonFailure::kTrailingComma, 7, 1, 8);
  CheckError("[\n1,\n]", JsonFailure::kTrailingComma, 5, 3, 1);

  // 非法字符 / 结构
  CheckError("[1 2]", JsonFailure::kUnexpectedChar, 3, 1, 4);
  CheckError("{\"a\" 1}", JsonFailure::kUnexpectedChar, 5, 1, 6);
  CheckError("{a:1}", JsonFailure::kUnexpectedChar, 1, 1, 2);
  CheckError("[1x]", JsonFailure::kUnexpectedChar, 2, 1, 3);
  CheckError("{\"a\":}", JsonFailure::kUnexpectedChar, 5, 1, 6);
  CheckError("[,]", JsonFailure::kUnexpectedChar, 1, 1, 2);
  CheckError("{,}", JsonFailure::kUnexpectedChar, 1, 1, 2);

  // 深度超限（显式 2 层）；自动模式按输入长度。
  ParserConfig shallow;
  shallow.max_depth = 2;
  CheckError("[[[]]]", JsonFailure::kDepthExceeded, 2, 1, 3, shallow);
  ParserConfig automatic;
  automatic.max_depth = 0;
  CheckError("[[[", JsonFailure::kDepthExceeded, 2, 1, 3, automatic);

  // 非法转义
  CheckError(R"("a\q")", JsonFailure::kInvalidEscape, 3, 1, 4);
  CheckError(R"("\u12G4")", JsonFailure::kInvalidEscape, 5, 1, 6);
  CheckError(R"("\uD800")", JsonFailure::kInvalidEscape, 7, 1, 8);
  CheckError(R"("\uDC00")", JsonFailure::kInvalidEscape, 7, 1, 8);
  CheckError(R"("\uD800\u0041")", JsonFailure::kInvalidEscape, 13, 1, 14);

  // 裸控制字符
  CheckError("\"a\x01" "b\"", JsonFailure::kUnescapedControl, 2, 1, 3);
  CheckError("\"\n\"", JsonFailure::kUnescapedControl, 1, 1, 2);

  // 尾随垃圾
  CheckError("null null", JsonFailure::kTrailingGarbage, 5, 1, 6);
  CheckError("{}x", JsonFailure::kTrailingGarbage, 2, 1, 3);

  // 非法数字
  CheckError("01", JsonFailure::kInvalidNumber, 1, 1, 2);
  CheckError("-", JsonFailure::kUnexpectedEnd, 1, 1, 2);
  CheckError("1.", JsonFailure::kUnexpectedEnd, 2, 1, 3);
  CheckError("[1.]", JsonFailure::kInvalidNumber, 3, 1, 4);
  CheckError("1e", JsonFailure::kUnexpectedEnd, 2, 1, 3);
  CheckError("1e+", JsonFailure::kUnexpectedEnd, 3, 1, 4);
  CheckError("+1", JsonFailure::kUnexpectedChar, 0, 1, 1);
  CheckError("NaN", JsonFailure::kUnexpectedChar, 0, 1, 1);
  CheckError("nul", JsonFailure::kUnexpectedEnd, 3, 1, 4);
  CheckError("nulx", JsonFailure::kUnexpectedChar, 3, 1, 4);
  CheckError("tru", JsonFailure::kUnexpectedEnd, 3, 1, 4);
  CheckError("fals", JsonFailure::kUnexpectedEnd, 4, 1, 5);

  // 允许尾逗号后这些输入应成功。
  ParserConfig trailing;
  trailing.allow_trailing_comma = true;
  Document document;
  const Value& root = document.Root();
  CHECK(ParseText("[1,]", document, trailing) && root.Size() == 1);
  CHECK(ParseText("{\"a\":1,}", document, trailing) && root.Size() == 1);
  CHECK(ParseText("[1,2,]", document, trailing) && root.Size() == 2);
  CHECK(ParseText("{\"a\":1,\"b\":2,}", document, trailing) && root.Size() == 2);
  // 允许尾逗号不能放过两个逗号。
  CheckError("[1,,]", JsonFailure::kUnexpectedChar, 3, 1, 4, trailing);
}

// ---------------------------------------------------------------------------
// CRLF 与 Policy
// ---------------------------------------------------------------------------

void TestCrlfEquivalence() {
  const std::string lf =
      "{\n"
      "  \"a\": 1,\n"
      "  \"b\": [true, null, \"x\\n\\u4E2D\"],\n"
      "  \"c\": {\"d\": 2.5, \"e\": [1, 2, 3]},\n"
      "  \"f\": \"\"\n"
      "}";
  const std::string crlf = ToCrlf(lf);

  Document lf_document;
  Document crlf_document;
  const Value& lf_root = lf_document.Root();
  const Value& crlf_root = crlf_document.Root();
  CHECK(ParseText(lf, lf_document));
  CHECK(ParseText(crlf, crlf_document));
  CHECK(DeepEqual(lf_root, crlf_root));
  CHECK(lf_root["b"].At(2).AsText() == "x\n\xE4\xB8\xAD");
  CHECK(crlf_root["b"].At(2).AsText() == "x\n\xE4\xB8\xAD");
  CHECK(crlf_root["c"]["d"].As<double>() == 2.5);

  // CRLF 与 LF 的错误行列必须一致（`\r\n` 只算一个换行）。
  CheckError("{\r\n\"a\":\r\n}", JsonFailure::kUnexpectedChar, 9, 3, 1);
  CheckError("{\n\"a\":\n}", JsonFailure::kUnexpectedChar, 7, 3, 1);
  CheckError("[\r\n1,\r\n]", JsonFailure::kTrailingComma, 7, 3, 1);
  CheckError("[\n1,\n]", JsonFailure::kTrailingComma, 5, 3, 1);

  // 独立的 `\r` 也算换行；`\r\n` 的列从 1 开始。
  CheckError("[\r1,\r]", JsonFailure::kTrailingComma, 5, 3, 1);
  CheckError("[\r\n\r\n1,]", JsonFailure::kTrailingComma, 7, 3, 3);
}

template <typename Policy>
bool ParseWithPolicy(std::string_view text, Document& out,
                     const ParserConfig& config = ParserConfig{}) {
  BasicParser<Policy> parser;
  ErrorPtr error;
  return parser.Parse(text, out, error, config);
}

void TestPolicyEquivalence() {
  const std::string inputs[] = {
      R"({"a":1,"b":[true,null,"x"],"c":{"d":[]}})",
      // 深度超过 InlinePolicy 的内联容量（8），强制切堆。
      "[[[[[[[[[[[[1]]]]]]]]]]]]",
      // 宽数组 + 转义字符串 + 数字。
      R"([0,1,-2,3.5,4e2,"a\nb","\u4E2D",true,false,null,{"k":"v"},[1,[2,[3]]]])",
  };

  for (const std::string& input : inputs) {
    Document inline_document;
    Document std_document;
    Document compact_document;
    CHECK(ParseWithPolicy<InlinePolicy>(input, inline_document));
    CHECK(ParseWithPolicy<StdPolicy>(input, std_document));
    CHECK(ParseWithPolicy<CompactPolicy>(input, compact_document));
    CHECK(DeepEqual(inline_document.Root(), std_document.Root()));
    CHECK(DeepEqual(inline_document.Root(), compact_document.Root()));
  }

  // 深度 12 的输入对 InlinePolicy（8）会切到堆，结果仍然一致。
  Document deep_inline;
  Document deep_std;
  CHECK(ParseWithPolicy<InlinePolicy>("[[[[[[[[[[[[]]]]]]]]]]]]", deep_inline));
  CHECK(ParseWithPolicy<StdPolicy>("[[[[[[[[[[[[]]]]]]]]]]]]", deep_std));
  CHECK(DeepEqual(deep_inline.Root(), deep_std.Root()));

  // 错误位置在三套 Policy 下必须完全一致。
  const std::string bad = "{\r\n\"a\": [1,\r\n2,] \r\n}";
  ParserConfig automatic;
  automatic.max_depth = 0;
  for (int policy = 0; policy < 3; ++policy) {
    ErrorPtr error;
    Document document;
    bool ok = false;
    if (policy == 0) {
      BasicParser<InlinePolicy> parser;
      ok = parser.Parse(bad, document, error, automatic);
    } else if (policy == 1) {
      BasicParser<StdPolicy> parser;
      ok = parser.Parse(bad, document, error, automatic);
    } else {
      BasicParser<CompactPolicy> parser;
      ok = parser.Parse(bad, document, error, automatic);
    }
    CHECK(!ok);
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr);
    if (json_error != nullptr) {
      CHECK(json_error->Failure() == JsonFailure::kTrailingComma);
      CHECK(json_error->Line() == 3);
      CHECK(json_error->Column() == 3);
    }
  }
}

// ---------------------------------------------------------------------------
// 零拷贝
// ---------------------------------------------------------------------------

void TestZeroCopy() {
  const std::string input = R"({"key":"hello","escaped":"a\nb","n":1})";
  Document document;
  CHECK(ParseText(input, document));
  const Value& root = document.Root();

  const std::string_view key_text = root.MemberName(0);
  CHECK(key_text.data() >= input.data());
  CHECK(key_text.data() + key_text.size() <= input.data() + input.size());

  const Text plain = root["key"].As<Text>();
  CHECK(plain.borrowed());
  CHECK(plain.view() == "hello");
  CHECK(plain.view().data() >= input.data() &&
        plain.view().data() + plain.view().size() <= input.data() + input.size());

  const Text escaped = root["escaped"].As<Text>();
  CHECK(!escaped.borrowed());
  CHECK(escaped.view() == "a\nb");
}

// ---------------------------------------------------------------------------
// 深嵌套
// ---------------------------------------------------------------------------

void TestDeepNesting() {
  constexpr std::size_t kDepth = 100000;
  std::string text;
  text.reserve(kDepth * 2);
  text.append(kDepth, '[');
  text.append(kDepth, ']');

  // 1) 解析 10 万层：显式放宽 max_depth。
  {
    ParserConfig config;
    config.max_depth = kDepth;
    Parser parser;
    Document document;
    ErrorPtr error;
    ++g_checks;
    if (!parser.Parse(text, document, error, config)) {
      std::fprintf(stderr, "CHECK failed: 10 万层解析失败: %s\n", ErrorMessage(error).c_str());
      ++g_failures;
    } else {
      std::size_t depth = 0;
      Value cursor = document.Root();
      while (cursor.IsArray() && cursor.Size() == 1) {
        ++depth;
        cursor = cursor.At(0);
      }
      ++g_checks;
      if (depth != kDepth - 1 || !cursor.IsArray() || cursor.Size() != 0) {
        std::fprintf(stderr, "CHECK failed: 10 万层深度=%zu\n", depth);
        ++g_failures;
      }
    }
  }  // document 在此析构：池释放，不得爆栈。

  // 2) 默认配置（max_depth=1024）必须拒绝，且不崩溃。
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    const bool ok = parser.Parse(text, document, error);
    CHECK(!ok);
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr);
    if (json_error != nullptr) {
      CHECK(json_error->Failure() == JsonFailure::kDepthExceeded);
      CHECK(json_error->Line() == 1);
      CHECK(json_error->Column() == 1025);
    }
  }

  // 3) 解析 → 清空 → 复用：旧池释放后同一文档可以再次解析。
  {
    ParserConfig config;
    config.max_depth = kDepth;
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(text, document, error, config));
    document.Clear();
    CHECK(document.Empty() && document.ValueCount() == 0);
  }

  // 4) 深树被新解析覆盖：同样走池释放，不得爆栈。
  {
    ParserConfig config;
    config.max_depth = kDepth;
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(text, document, error, config));
    CHECK(parser.Parse("null", document, error, config) && document.Root().IsNull());
  }
}

// ---------------------------------------------------------------------------
// 随机
// ---------------------------------------------------------------------------

/// @brief 生成 UTF-8 编码（与解析器内部实现等价，测试侧独立实现）。
void AppendUtf8(std::uint32_t code, std::string& out) {
  if (code <= 0x7Fu) {
    out.push_back(static_cast<char>(code));
  } else if (code <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (code >> 6)));
    out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
  } else if (code <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (code >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (code >> 18)));
    out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
  }
}

/// @brief 期望树（测试侧独立类型）：不依赖 `Value`，避免「解析器自证」。
struct ExpectedValue {
  enum class Kind { kNull, kBool, kInt, kDouble, kString, kArray, kObject };

  Kind kind = Kind::kNull;
  bool boolean = false;
  std::int64_t integer = 0;
  double real = 0.0;
  std::string text;
  std::vector<ExpectedValue> elements;
  std::vector<std::pair<std::string, ExpectedValue>> members;
};

/// @brief 实际树 vs 期望树深比较（类型 + 内容 + 容器顺序）。
bool DeepEqual(const Value& actual, const ExpectedValue& expected) {
  switch (expected.kind) {
    case ExpectedValue::Kind::kNull:
      return actual.IsNull();
    case ExpectedValue::Kind::kBool:
      return actual.IsBool() && actual.As<bool>() == expected.boolean;
    case ExpectedValue::Kind::kInt:
      return actual.IsInt() && actual.As<std::int64_t>() == expected.integer;
    case ExpectedValue::Kind::kDouble:
      return actual.IsDouble() && actual.As<double>() == expected.real;
    case ExpectedValue::Kind::kString:
      return actual.IsString() && actual.AsText() == expected.text;
    case ExpectedValue::Kind::kArray: {
      if (!actual.IsArray() || actual.Size() != expected.elements.size()) {
        return false;
      }
      for (std::size_t index = 0; index < expected.elements.size(); ++index) {
        if (!DeepEqual(actual.At(index), expected.elements[index])) {
          return false;
        }
      }
      return true;
    }
    case ExpectedValue::Kind::kObject: {
      if (!actual.IsObject() || actual.Size() != expected.members.size()) {
        return false;
      }
      for (std::size_t index = 0; index < expected.members.size(); ++index) {
        if (actual.MemberName(index) != expected.members[index].first) {
          return false;
        }
        if (!DeepEqual(actual.At(index), expected.members[index].second)) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

/// @brief 随机 JSON 生成器：同时产出文本与期望树。
class JsonFuzzer {
 public:
  explicit JsonFuzzer(std::uint64_t seed) : m_random(seed) {}

  /// @brief 生成下一份 JSON；期望树通过 `expected()` 取。
  std::string Next() {
    m_expected = ExpectedValue{};
    std::string text;
    AppendValue(text, m_expected, 0);
    return text;
  }

  const ExpectedValue& expected() const { return m_expected; }

 private:
  static constexpr int kMaxDepth = 4;

  std::uint32_t NextU32() { return static_cast<std::uint32_t>(m_random()); }
  std::uint32_t Range(std::uint32_t bound) { return NextU32() % bound; }

  /// @brief 生成 double 文本；保证带 `.` 或 `e`，解析后一定落在 double 分支。
  static std::string FormatDouble(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    std::string text(buffer);
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos) {
      text += ".0";
    }
    return text;
  }

  void AppendString(std::string& literal, std::string& bytes) {
    literal.push_back('"');
    const std::uint32_t length = Range(7);
    for (std::uint32_t index = 0; index < length; ++index) {
      switch (Range(8)) {
        case 0: {
          const char value = static_cast<char>('a' + Range(26));
          literal.push_back(value);
          bytes.push_back(value);
          break;
        }
        case 1: {
          const char value = static_cast<char>('0' + Range(10));
          literal.push_back(value);
          bytes.push_back(value);
          break;
        }
        case 2: {
          const char choices[] = {'"', '\\', '/', '\b', '\f', '\n', '\r', '\t'};
          const char value = choices[Range(8)];
          bytes.push_back(value);
          switch (value) {
            case '"':
              literal += "\\\"";
              break;
            case '\\':
              literal += "\\\\";
              break;
            case '/':
              literal += "\\/";
              break;
            case '\b':
              literal += "\\b";
              break;
            case '\f':
              literal += "\\f";
              break;
            case '\n':
              literal += "\\n";
              break;
            case '\r':
              literal += "\\r";
              break;
            default:
              literal += "\\t";
              break;
          }
          break;
        }
        case 3: {
          // 随机 BMP 码点（避开代理区），写成 \uXXXX。
          const std::uint32_t code = 0x20u + Range(0xD7FFu - 0x20u + 1u);
          char escape[8];
          std::snprintf(escape, sizeof(escape), "\\u%04X", code);
          literal += escape;
          AppendUtf8(code, bytes);
          break;
        }
        case 4: {
          literal += "\xE4\xB8\xAD";
          bytes += "\xE4\xB8\xAD";
          break;
        }
        case 5: {
          literal.push_back(' ');
          bytes.push_back(' ');
          break;
        }
        case 6: {
          literal += "\\uD83D\\uDE00";
          bytes += "\xF0\x9F\x98\x80";
          break;
        }
        default: {
          const char value = static_cast<char>('A' + Range(26));
          literal.push_back(value);
          bytes.push_back(value);
          break;
        }
      }
    }
    literal.push_back('"');
  }

  void AppendValue(std::string& text, ExpectedValue& value, int depth) {
    const std::uint32_t kind = depth >= kMaxDepth ? Range(5) : Range(7);
    switch (kind) {
      case 0:
        text += "null";
        value.kind = ExpectedValue::Kind::kNull;
        break;
      case 1: {
        const bool boolean = Range(2) == 0;
        text += boolean ? "true" : "false";
        value.kind = ExpectedValue::Kind::kBool;
        value.boolean = boolean;
        break;
      }
      case 2: {
        const std::int64_t integer = static_cast<std::int64_t>(Range(2000001)) - 1000000;
        text += std::to_string(integer);
        value.kind = ExpectedValue::Kind::kInt;
        value.integer = integer;
        break;
      }
      case 3: {
        const std::int64_t numerator = static_cast<std::int64_t>(Range(20001)) - 10000;
        const double floating = static_cast<double>(numerator) / 8.0;
        text += FormatDouble(floating);
        value.kind = ExpectedValue::Kind::kDouble;
        value.real = floating;
        break;
      }
      case 4: {
        std::string literal;
        std::string bytes;
        AppendString(literal, bytes);
        text += literal;
        value.kind = ExpectedValue::Kind::kString;
        value.text = std::move(bytes);
        break;
      }
      case 5: {
        const std::uint32_t count = Range(4);
        text.push_back('[');
        value.kind = ExpectedValue::Kind::kArray;
        value.elements.resize(count);
        for (std::uint32_t index = 0; index < count; ++index) {
          if (index != 0) {
            text.push_back(',');
          }
          AppendValue(text, value.elements[index], depth + 1);
        }
        text.push_back(']');
        break;
      }
      default: {
        const std::uint32_t count = Range(4);
        text.push_back('{');
        value.kind = ExpectedValue::Kind::kObject;
        value.members.resize(count);
        for (std::uint32_t index = 0; index < count; ++index) {
          if (index != 0) {
            text.push_back(',');
          }
          std::string key_literal;
          std::string key_bytes;
          AppendString(key_literal, key_bytes);
          text += key_literal;
          text.push_back(':');
          value.members[index].first = std::move(key_bytes);
          AppendValue(text, value.members[index].second, depth + 1);
        }
        text.push_back('}');
        break;
      }
    }
  }

  std::mt19937_64 m_random;
  ExpectedValue m_expected;
};

void TestRandomJson() {
  constexpr std::size_t kRounds = 20000;
  JsonFuzzer fuzzer(0x5EED1234ULL);
  for (std::size_t round = 0; round < kRounds; ++round) {
    const std::string text = fuzzer.Next();
    Document document;
    if (!ParseText(text, document)) {
      std::fprintf(stderr, "随机第 %zu 轮输入: %.200s\n", round, text.c_str());
      return;
    }
    ++g_checks;
    if (!DeepEqual(document.Root(), fuzzer.expected())) {
      std::fprintf(stderr, "CHECK failed: 随机第 %zu 轮不匹配: %.200s\n", round, text.c_str());
      ++g_failures;
      if (g_failures > 5) {
        return;
      }
    }
  }
  std::printf("  随机 JSON: %zu 轮\n", kRounds);
}

// ---------------------------------------------------------------------------
// 真实场景
// ---------------------------------------------------------------------------

/// @brief 场景 1：服务配置。
void ScenarioConfig() {
  const char text[] = R"({
    "service": {"name": "gateway", "port": 8080, "timeout_ms": 1500},
    "features": ["http2", "tls", "metrics"],
    "logging": {"level": "info", "rotate": true}
  })";
  Document document;
  const Value& config = document.Root();
  CHECK(ParseText(text, document));
  CHECK(config.IsObject() && config.Size() == 3);
  CHECK(config["service"]["name"].AsText() == "gateway");
  CHECK(config["service"]["port"].As<std::int64_t>() == 8080);
  CHECK(config["service"]["timeout_ms"].As<std::int64_t>() == 1500);
  CHECK(config["features"].Size() == 3);
  CHECK(config["features"].At(0).AsText() == "http2");
  CHECK(config["features"].At(2).AsText() == "metrics");
  CHECK(config["logging"]["level"].AsText() == "info");
  CHECK(config["logging"]["rotate"].As<bool>());
  CHECK(config["logging"]["missing"].IsNull());
}

/// @brief 场景 2：API 响应，统计活跃用户与总分。
void ScenarioApiResponse() {
  const char text[] = R"({"code":0,"data":{"total":3,"users":[
    {"id":1,"name":"Ada","active":true,"score":98.5},
    {"id":2,"name":"Bob","active":false,"score":76.0},
    {"id":3,"name":"Cyd","active":true,"score":88.25}
  ]}})";
  Document document;
  const Value& response = document.Root();
  CHECK(ParseText(text, document));
  CHECK(response["code"].As<std::int64_t>() == 0);
  const Value users = response["data"]["users"];
  CHECK(users.IsArray() && users.Size() == 3);
  std::size_t active = 0;
  double total = 0.0;
  for (std::size_t index = 0; index < users.Size(); ++index) {
    if (users.At(index)["active"].As<bool>()) {
      ++active;
    }
    total += users.At(index)["score"].As<double>();
  }
  CHECK(active == 2);
  CHECK(total == 262.75);
  CHECK(users.At(1)["name"].AsText() == "Bob");
}

/// @brief 场景 3：日志事件，验证转义引号与换行。
void ScenarioLogEvent() {
  const char text[] =
      R"({"ts":1700000000,"level":"warn","message":"disk \"full\" at /var\nretry","tags":["disk","urgent"]})";
  Document document;
  const Value& event = document.Root();
  CHECK(ParseText(text, document));
  CHECK(event["ts"].As<std::int64_t>() == 1700000000LL);
  CHECK(event["level"].AsText() == "warn");
  CHECK(event["message"].AsText() == "disk \"full\" at /var\nretry");
  CHECK(event["tags"].IsArray() && event["tags"].Size() == 2);
  CHECK(event["tags"].At(1).AsText() == "urgent");
}

/// @brief 场景 4：Decimal 价格文本乘数量。
void ScenarioDecimalPrice() {
  const char text[] = R"({"sku":"X-1","price":"19.99","qty":3})";
  Document document;
  const Value& order = document.Root();
  CHECK(ParseText(text, document));
  Decimal price;
  CHECK(order["price"].As(price));
  CHECK(price.ToString() == "19.99");
  Decimal quantity;
  CHECK(Decimal::Parse("3", quantity));
  const Decimal total = price * quantity;
  CHECK(total.ToString() == "59.97");
  CHECK(order["qty"].As<std::int64_t>() == 3);
}

/// @brief 场景 5：超出 int64 / double 的 ID 仍然精确。
void ScenarioBigIdentifiers() {
  const char text[] = R"({"id":9007199254740993,"huge":18446744073709551616})";
  Document document;
  const Value& identifiers = document.Root();
  CHECK(ParseText(text, document));
  CHECK(identifiers["id"].IsInt());
  CHECK(identifiers["id"].As<std::int64_t>() == 9007199254740993LL);
  CHECK(identifiers["huge"].IsDecimal());
  CHECK(identifiers["huge"].As<Decimal>().ToString() == "18446744073709551616");
}

/// @brief 场景 6：多语言文本（\u 转义与代理对）。
void ScenarioUnicode() {
  const char text[] = R"({"zh":"\u4E2D\u6587","emoji":"\uD83D\uDE00","raw":"h\u00E9llo"})";
  Document document;
  const Value& localized = document.Root();
  CHECK(ParseText(text, document));
  CHECK(localized["zh"].AsText() == "\xE4\xB8\xAD\xE6\x96\x87");
  CHECK(localized["emoji"].AsText() == "\xF0\x9F\x98\x80");
  CHECK(localized["raw"].AsText() == "h\xC3\xA9llo");
}

/// @brief 场景 8：解析超时（时间预算）。
void TestTimeout() {
  std::printf("\n[9] 解析超时...\n");
  // 用数组而不是对象：平铺对象快路径（TryParseFlatObject）不检查时间预算，
  // 数组走通用状态机，每 256 轮检查一次 deadline。
  std::string big = "[";
  for (int i = 0; i < 200000; ++i) {
    if (i > 0) big += ',';
    big += '1';
  }
  big += "]";

  ParserConfig config;
  config.timeout_ms = 1;  // 1 ms 预算，10 MB 解析必然超时
  Document doc;
  go2cpp::ErrorPtr error;
  Parser parser;
  CHECK(!parser.Parse(big, doc, error, config));
  CHECK(error != nullptr);
  if (error != nullptr) {
    const auto* json_error = dynamic_cast<const go2cpp::json::JsonError*>(error.get());
    CHECK(json_error != nullptr);
    if (json_error != nullptr) {
      CHECK(json_error->Failure() == go2cpp::json::JsonFailure::kTimeout);
    }
  }

  // timeout_ms = 0：不限时，正常解析。
  ParserConfig unlimited;
  Document doc2;
  go2cpp::ErrorPtr error2;
  CHECK(parser.Parse(big, doc2, error2, unlimited));
}

// ---------------------------------------------------------------------------
// 场景 11：重复 key 策略
// ---------------------------------------------------------------------------

/// @brief 场景 11：`DuplicateKeyPolicy` 三种策略的语义与错误位置。
void TestDuplicateKeys() {
  std::printf("\n[10] 重复 key 策略...\n");

  // ---- kFirst（默认）：重复成员全部保留，首个值生效（历史行为）-------------
  {
    Document document;
    CHECK(ParseText(R"({"a":1,"b":0,"a":2})", document));
    const Value& root = document.Root();
    CHECK(root.Size() == 3);
    CHECK(root.MemberName(0) == "a");
    CHECK(root.MemberName(1) == "b");
    CHECK(root.MemberName(2) == "a");
    CHECK(root.At(0).As<std::int64_t>() == 1);
    CHECK(root.At(1).As<std::int64_t>() == 0);
    CHECK(root.At(2).As<std::int64_t>() == 2);
    CHECK(root["a"].As<std::int64_t>() == 1);  // 首个生效，不是最后
    CHECK(root.Find("a").As<std::int64_t>() == 1);
    // 默认配置（不写任何字段）必须与显式 kFirst 完全一致。
    ParserConfig defaults;
    CHECK(defaults.duplicate_key == DuplicateKeyPolicy::kFirst);
    Document default_document;
    CHECK(ParseText(R"({"a":1,"b":0,"a":2})", default_document, defaults));
    CHECK(DeepEqual(root, default_document.Root()));
  }
  // kFirst 对平铺对象（走快路径）同样保留全部重复项。
  {
    Document document;
    CHECK(ParseText(R"({"a":1,"a":2})", document));
    const Value& root = document.Root();
    CHECK(root.Size() == 2);
    CHECK(root["a"].As<std::int64_t>() == 1);
    CHECK(root.At(1).As<std::int64_t>() == 2);
  }
  // 转义 key 按解码后的字节比较：`"\u0061"` 与 `"a"` 是同一个 key。
  {
    Document document;
    CHECK(ParseText(R"({"\u0061":1,"a":2})", document));
    const Value& root = document.Root();
    CHECK(root.Size() == 2);
    CHECK(root["a"].As<std::int64_t>() == 1);
  }

  // ---- kLast：去重，位置取首次出现，值取最后一次 -----------------------------
  {
    ParserConfig config;
    config.duplicate_key = DuplicateKeyPolicy::kLast;
    Document document;
    CHECK(ParseText(R"({"a":1,"b":0,"a":2})", document, config));
    const Value& root = document.Root();
    CHECK(root.Size() == 2);
    CHECK(root.MemberName(0) == "a");
    CHECK(root.MemberName(1) == "b");
    CHECK(root["a"].As<std::int64_t>() == 2);  // 最后的值
    CHECK(root["b"].As<std::int64_t>() == 0);
  }
  // 顺序取首次出现：`b` 先出现，后面的重复不改变成员顺序。
  {
    ParserConfig config;
    config.duplicate_key = DuplicateKeyPolicy::kLast;
    Document document;
    CHECK(ParseText(R"({"b":0,"a":1,"b":2})", document, config));
    const Value& root = document.Root();
    CHECK(root.Size() == 2);
    CHECK(root.MemberName(0) == "b");
    CHECK(root.MemberName(1) == "a");
    CHECK(root["b"].As<std::int64_t>() == 2);
    CHECK(root["a"].As<std::int64_t>() == 1);
  }
  // 值是容器时同样被后面的值覆盖。
  {
    ParserConfig config;
    config.duplicate_key = DuplicateKeyPolicy::kLast;
    Document document;
    CHECK(ParseText(R"({"a":{"x":1},"a":[2,3]})", document, config));
    const Value& root = document.Root();
    CHECK(root.Size() == 1);
    CHECK(root["a"].IsArray() && root["a"].Size() == 2);
    CHECK(root["a"].At(1).As<std::int64_t>() == 3);
  }
  // 嵌套对象各自去重、互不影响；转义 key 也归一。
  {
    ParserConfig config;
    config.duplicate_key = DuplicateKeyPolicy::kLast;
    Document document;
    CHECK(ParseText(R"({"o":{"k":1,"k":2},"b":3,"\u0061":1,"a":2})", document, config));
    const Value& root = document.Root();
    CHECK(root.Size() == 3);
    CHECK(root["o"].Size() == 1);
    CHECK(root["o"]["k"].As<std::int64_t>() == 2);
    CHECK(root["b"].As<std::int64_t>() == 3);
    CHECK(root["a"].As<std::int64_t>() == 2);
  }
  // 三 Policy 下 kLast 结果一致（非默认策略绕过快路径，走通用状态机）。
  {
    ParserConfig config;
    config.duplicate_key = DuplicateKeyPolicy::kLast;
    Document inline_document;
    Document std_document;
    Document compact_document;
    CHECK(ParseWithPolicy<InlinePolicy>(R"({"a":1,"b":0,"a":2})", inline_document, config));
    CHECK(ParseWithPolicy<StdPolicy>(R"({"a":1,"b":0,"a":2})", std_document, config));
    CHECK(ParseWithPolicy<CompactPolicy>(R"({"a":1,"b":0,"a":2})", compact_document, config));
    CHECK(DeepEqual(inline_document.Root(), std_document.Root()));
    CHECK(DeepEqual(inline_document.Root(), compact_document.Root()));
  }

  // ---- kError：重复 key 报错，位置 = 重复出现的那个 key 的开引号 -------------
  {
    ParserConfig config;
    config.duplicate_key = DuplicateKeyPolicy::kError;
    CheckError(R"({"a":1,"a":2})", JsonFailure::kDuplicateKey, 7, 1, 8, config);
    CheckError(R"({"a":1,"b":2,"a":3})", JsonFailure::kDuplicateKey, 13, 1, 14, config);
    CheckError("{\n \"a\":1,\n \"a\":2\n}", JsonFailure::kDuplicateKey, 11, 3, 2, config);
  }
  // 嵌套对象里的重复同样报错，位置指向内层 key。
  {
    ParserConfig config;
    config.duplicate_key = DuplicateKeyPolicy::kError;
    CheckError(R"({"o":{"k":1,"k":2}})", JsonFailure::kDuplicateKey, 12, 1, 13, config);
  }
  // 没有重复时 kError 正常解析；多个重复只报第一个。
  {
    ParserConfig config;
    config.duplicate_key = DuplicateKeyPolicy::kError;
    Document document;
    CHECK(ParseText(R"({"a":1,"b":2,"c":3})", document, config));
    CHECK(document.Root().Size() == 3);
    CheckError(R"({"a":1,"a":2,"a":3})", JsonFailure::kDuplicateKey, 7, 1, 8, config);
  }
  // 错误原因名称、消息文本与失败后文档状态。
  {
    ParserConfig config;
    config.duplicate_key = DuplicateKeyPolicy::kError;
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(!parser.Parse(R"({"a":1,"a":2})", document, error, config));
    CHECK(JsonFailureName(JsonFailure::kDuplicateKey) == std::string("duplicate key"));
    CHECK(ErrorMessage(error).find("duplicate key") != std::string::npos);
    CHECK(document.Root().IsNull());
  }
  // 三 Policy 下 kError 的失败原因与位置一致。
  {
    ParserConfig config;
    config.duplicate_key = DuplicateKeyPolicy::kError;
    for (int policy = 0; policy < 3; ++policy) {
      ErrorPtr error;
      Document document;
      bool ok = false;
      if (policy == 0) {
        BasicParser<InlinePolicy> parser;
        ok = parser.Parse(R"({"a":1,"a":2})", document, error, config);
      } else if (policy == 1) {
        BasicParser<StdPolicy> parser;
        ok = parser.Parse(R"({"a":1,"a":2})", document, error, config);
      } else {
        BasicParser<CompactPolicy> parser;
        ok = parser.Parse(R"({"a":1,"a":2})", document, error, config);
      }
      CHECK(!ok);
      const auto* json_error = dynamic_cast<const JsonError*>(error.get());
      CHECK(json_error != nullptr);
      if (json_error != nullptr) {
        CHECK(json_error->Failure() == JsonFailure::kDuplicateKey);
        CHECK(json_error->Offset() == 7);
        CHECK(json_error->Line() == 1);
        CHECK(json_error->Column() == 8);
      }
    }
  }

  // ---- 默认策略不改变既有行为：无重复对象仍按原语义解析 ----------------------
  {
    Document document;
    CHECK(ParseText(R"({"a":1,"b":[true,null,"x"]})", document));
    CHECK(document.Root().Size() == 2);
    CHECK(document.Root()["b"].Size() == 3);
  }
}

/// @brief 场景 4：JSON 截断（部分解析 + consumed）。
void TestPartialParse() {
  std::printf("\n[11] 部分解析（截断）...\n");
  Parser parser;
  Document doc;
  go2cpp::ErrorPtr error;
  ParserConfig config;

  // 截断在字符串内：已解析成员保留在池里，consumed 指向截断处。
  constexpr std::string_view kTruncated = R"({"a":1,"b":"x)";
  const go2cpp::json::ParseOutcome out = parser.ParsePartial(kTruncated, doc, error, config);
  CHECK(!out.ok);
  CHECK(out.consumed > 0);
  CHECK(out.consumed <= kTruncated.size());
  CHECK(error != nullptr);
  CHECK(doc.ValueCount() > 0);  // "a":1 已进池

  // 完整输入：ok + consumed = 输入长度 + Root 可用。
  constexpr std::string_view kFull = R"({"a":1})";
  const go2cpp::json::ParseOutcome full = parser.ParsePartial(kFull, doc, error, config);
  CHECK(full.ok);
  CHECK(full.consumed == kFull.size());
  CHECK(!doc.Root().IsNull());
  CHECK(doc.Root().IsObject());
}

// ---------------------------------------------------------------------------
// 场景 7：单值上限（ParserConfig::max_string_bytes）
// ---------------------------------------------------------------------------

/// @brief 构造单值上限配置；`validate_utf8` 默认关闭（与生产默认一致）。
ParserConfig LimitConfig(std::size_t max_string_bytes, bool validate_utf8 = false) {
  ParserConfig config;
  config.max_string_bytes = max_string_bytes;
  config.validate_utf8 = validate_utf8;
  return config;
}

/// @brief 三 Policy 下同一输入必须给出完全相同的失败原因与位置。
void CheckAllPolicies(std::string_view text, JsonFailure failure, std::size_t offset,
                      std::size_t line, std::size_t column, const ParserConfig& config) {
  for (int policy = 0; policy < 3; ++policy) {
    ErrorPtr error;
    Document document;
    bool ok = false;
    if (policy == 0) {
      BasicParser<InlinePolicy> parser;
      ok = parser.Parse(text, document, error, config);
    } else if (policy == 1) {
      BasicParser<StdPolicy> parser;
      ok = parser.Parse(text, document, error, config);
    } else {
      BasicParser<CompactPolicy> parser;
      ok = parser.Parse(text, document, error, config);
    }
    CHECK(!ok);
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr);
    if (json_error != nullptr) {
      CHECK(json_error->Failure() == failure);
      CHECK(json_error->Offset() == offset);
      CHECK(json_error->Line() == line);
      CHECK(json_error->Column() == column);
    }
    CHECK(document.Root().IsNull());
  }
}

void TestStringTooLarge() {
  std::printf("\n[12] 单值上限（max_string_bytes）...\n");

  // ---- 默认 0 = 不限：1 MiB 字符串、10 万位整数照常解析（历史行为） ----------
  {
    Document document;
    const std::string big(1u << 20, 'a');  // 1 MiB
    // 输入缓冲必须活到 document 使用之后（Document 里借用该缓冲）。
    const std::string big_input = Quote(big);
    CHECK(ParseText(big_input, document));
    CHECK(document.Root().IsString());
    CHECK(document.Root().AsText().size() == big.size());
    CHECK(document.Root().AsText() == big);

    const std::string huge_integer = "1" + std::string(100000, '0');
    CHECK(ParseText(huge_integer, document));
    CHECK(document.Root().IsDecimal());
  }

  // ---- 字符串值：恰好等于上限通过，超 1 字节失败，位置 = 开引号 -------------
  {
    const ParserConfig config = LimitConfig(4);
    Document document;
    CHECK(ParseText("\"1234\"", document, config));
    CHECK(document.Root().AsText() == "1234");
    CHECK(ParseText("\"\"", document, config));  // 空串不受限
    CheckError("\"12345\"", JsonFailure::kStringTooLarge, 0, 1, 1, config);
    CheckError(" \"12345\"", JsonFailure::kStringTooLarge, 1, 1, 2, config);
    CheckError("\n\"12345\"", JsonFailure::kStringTooLarge, 1, 2, 1, config);
  }

  // ---- 对象 key / 数组元素 / 数字 token -------------------------------------
  {
    const ParserConfig config = LimitConfig(4);
    CheckError("{\"12345\":1}", JsonFailure::kStringTooLarge, 1, 1, 2, config);
    CheckError("{\"a\":\"12345\"}", JsonFailure::kStringTooLarge, 5, 1, 6, config);
    CheckError("[1,\"12345\"]", JsonFailure::kStringTooLarge, 3, 1, 4, config);
    CheckError("[12345]", JsonFailure::kStringTooLarge, 1, 1, 2, config);
    CheckError("12345", JsonFailure::kStringTooLarge, 0, 1, 1, config);
    CheckError("[1.5e+10]", JsonFailure::kStringTooLarge, 1, 1, 2, config);
    // 平铺快路径先命中失败、回退通用状态机后，最终错误与通用路径一致。
    CheckError(R"({"id":123456,"name":"abcdef"})", JsonFailure::kStringTooLarge, 6, 1, 7,
               config);
  }

  // ---- 数字长度口径：含负号、小数与指数 -------------------------------------
  {
    const ParserConfig config = LimitConfig(6);
    Document document;
    CHECK(ParseText("-12345", document, config));  // 6 字节
    CHECK(ParseText("123456", document, config));  // 6 字节
    CHECK(ParseText("1.2345", document, config));  // 6 字节
    CheckError("-123456", JsonFailure::kStringTooLarge, 0, 1, 1, config);  // 7 字节
    CheckError("1.23456", JsonFailure::kStringTooLarge, 0, 1, 1, config);
    CheckError("1e12345", JsonFailure::kStringTooLarge, 0, 1, 1, config);
  }

  // ---- 转义口径：按**原始**字节计（防转义膨胀），不按解码后长度 -------------
  {
    // `"\u4E2D"` 原始 6 字节、解码后 3 字节：上限 3 必须拒绝。
    CheckError(R"("\u4E2D")", JsonFailure::kStringTooLarge, 0, 1, 1, LimitConfig(3));
    Document document;
    CHECK(ParseText(R"("\u4E2D")", document, LimitConfig(6)));
    CHECK(document.Root().AsText() == "\xE4\xB8\xAD");
  }

  // ---- 嵌套容器与三 Policy ---------------------------------------------------
  {
    const ParserConfig config = LimitConfig(4);
    CheckError(R"({"o":{"a":"12345"}})", JsonFailure::kStringTooLarge, 10, 1, 11, config);
    CheckAllPolicies(R"({"o":{"a":"12345"}})", JsonFailure::kStringTooLarge, 10, 1, 11,
                     config);
    CheckAllPolicies("\"\xE4\xB8\xAD\xE4\xB8\xAD\"", JsonFailure::kStringTooLarge, 0, 1, 1,
                     config);
  }

  // ---- 失败后文档状态、错误名称与文本 ---------------------------------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(!parser.Parse(Quote(std::string(8, 'x')), document, error, LimitConfig(4)));
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr);
    CHECK(json_error != nullptr && json_error->Failure() == JsonFailure::kStringTooLarge);
    CHECK(JsonFailureName(JsonFailure::kStringTooLarge) == std::string("string too large"));
    CHECK(ErrorMessage(error).find("string too large") != std::string::npos);
    CHECK(document.Root().IsNull());
  }

  // ---- ParsePartial：consumed = 超限 token 的起点 ----------------------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    const auto outcome = parser.ParsePartial(R"([1,"12345"])", document, error, LimitConfig(4));
    CHECK(!outcome.ok);
    CHECK(outcome.consumed == 3);  // 开引号的偏移
  }

  // ---- 上限 1：单字节通过、双字节失败 ---------------------------------------
  {
    const ParserConfig config = LimitConfig(1);
    Document document;
    CHECK(ParseText("\"a\"", document, config));
    CheckError("\"ab\"", JsonFailure::kStringTooLarge, 0, 1, 1, config);
  }
}

// ---------------------------------------------------------------------------
// 场景 9：UTF-8 严格校验（ParserConfig::validate_utf8）
// ---------------------------------------------------------------------------

void TestUtf8Validation() {
  std::printf("\n[13] UTF-8 严格校验（validate_utf8）...\n");

  ParserConfig strict;
  strict.validate_utf8 = true;

  // ---- 合法序列：ASCII 与 2/3/4 字节的边界码点全部通过，字节不变 ------------
  const std::string legal[] = {
      "",
      "plain ascii 123",
      Bytes({0x7F}),                                // U+007F
      Bytes({0xC2, 0x80}),                          // U+0080（2 字节下界）
      Bytes({0xDF, 0xBF}),                          // U+07FF（2 字节上界）
      Bytes({0xE0, 0xA0, 0x80}),                    // U+0800（3 字节下界）
      Bytes({0xED, 0x9F, 0xBF}),                    // U+D7FF（代理区之前）
      Bytes({0xEE, 0x80, 0x80}),                    // U+E000（代理区之后）
      Bytes({0xEF, 0xBF, 0xBF}),                    // U+FFFF
      Bytes({0xF0, 0x90, 0x80, 0x80}),              // U+10000（4 字节下界）
      Bytes({0xF4, 0x8F, 0xBF, 0xBF}),              // U+10FFFF（Unicode 上界）
      Bytes({0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87}),  // 中文
      Bytes({0xF0, 0x9F, 0x98, 0x80}),              // 😀
      Bytes({0xEF, 0xBB, 0xBF}),                    // 字符串内的 BOM（U+FEFF）
  };
  for (const std::string& content : legal) {
    Document document;
    // 输入缓冲必须活到断言之后：Document 里的借用 Text 指向它。
    const std::string input = Quote(content);
    CHECK(ParseText(input, document, strict));
    CHECK(document.Root().IsString());
    CHECK(document.Root().AsText() == content);
  }

  // ---- 非法序列：每类逐项断言原因与偏移（内容起点在引号后，绝对偏移 1） -----
  CheckError(Quote(Bytes({0x80})), JsonFailure::kInvalidUtf8, 1, 1, 2, strict);  // 孤立续字节
  CheckError(Quote(Bytes({0xBF})), JsonFailure::kInvalidUtf8, 1, 1, 2, strict);  // 孤立续字节
  CheckError(Quote(Bytes({0xC0, 0xAF})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 超长编码 C0
  CheckError(Quote(Bytes({0xC1, 0xBF})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 超长编码 C1
  CheckError(Quote(Bytes({0xE0, 0x80, 0x80})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 超长编码 E0 80
  CheckError(Quote(Bytes({0xE0, 0x9F, 0xBF})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 超长编码 E0 9F
  CheckError(Quote(Bytes({0xED, 0xA0, 0x80})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 代理区 U+D800
  CheckError(Quote(Bytes({0xED, 0xBF, 0xBF})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 代理区 U+DFFF
  CheckError(Quote(Bytes({0xF0, 0x8F, 0xBF, 0xBF})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 超长编码 F0 8F
  CheckError(Quote(Bytes({0xF4, 0x90, 0x80, 0x80})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // > U+10FFFF
  CheckError(Quote(Bytes({0xF5, 0x80, 0x80, 0x80})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 首字节 F5
  CheckError(Quote(Bytes({0xFE})), JsonFailure::kInvalidUtf8, 1, 1, 2, strict);
  CheckError(Quote(Bytes({0xFF})), JsonFailure::kInvalidUtf8, 1, 1, 2, strict);
  CheckError(Quote(Bytes({0xE4, 0xB8})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 截断的 3 字节序列
  CheckError(Quote(Bytes({0xF0, 0x9F, 0x98})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 截断的 4 字节序列
  CheckError(Quote(Bytes({0xE4, 0x41})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 第二字节不是续字节
  CheckError(Quote(Bytes({0xF0, 0x9F, 0x28})), JsonFailure::kInvalidUtf8, 1, 1, 2,
             strict);  // 第三字节不是续字节

  // ---- 偏移定位：中间、多字节之后、对象 key、转义之后、跨行 -----------------
  {
    CheckError(Quote("a" + Bytes({0x80}) + "b"), JsonFailure::kInvalidUtf8, 2, 1, 3, strict);
    CheckError(Quote(Bytes({0xE4, 0xB8, 0xAD, 0x80})), JsonFailure::kInvalidUtf8, 4, 1, 5,
               strict);
    CheckError("{\"" + Bytes({0x80}) + "\":1}", JsonFailure::kInvalidUtf8, 2, 1, 3, strict);
    CheckError("\"\\u0041" + Bytes({0x80}) + "\"", JsonFailure::kInvalidUtf8, 7, 1, 8,
               strict);
    CheckError("[\n \"" + Bytes({0x80}) + "\"]", JsonFailure::kInvalidUtf8, 4, 2, 3, strict);
  }

  // ---- 默认关闭：同样的非法字节原样透传（历史行为） -------------------------
  {
    const std::string raw = Bytes({0xFF, 0x80, 0xC0, 0xAF});
    const std::string raw_input = Quote(raw);
    Document document;
    CHECK(ParseText(raw_input, document, ParserConfig{}));
    CHECK(document.Root().AsText() == raw);
    CheckError(raw_input, JsonFailure::kInvalidUtf8, 1, 1, 2, strict);
    // 合法 UTF-8 在关闭时同样原样保留。
    CHECK(ParseText("\"\xE4\xB8\xAD\"", document));
    CHECK(document.Root().AsText() == "\xE4\xB8\xAD");
  }

  // ---- 与 max_string_bytes 组合：长度闸门先于内容校验 -----------------------
  {
    ParserConfig combined;
    combined.max_string_bytes = 4;
    combined.validate_utf8 = true;
    CheckError("\"\xE4\xB8\xAD\xE4\xB8\xAD\x80\"", JsonFailure::kStringTooLarge, 0, 1, 1,
               combined);
    CheckError("\"\x80\"", JsonFailure::kInvalidUtf8, 1, 1, 2, combined);
  }

  // ---- 转义序列不受 UTF-8 校验影响（解码结果天然合法） ---------------------
  {
    Document document;
    CHECK(ParseText(R"("\u4E2D\uD83D\uDE00")", document, strict));
    CHECK(document.Root().AsText() == "\xE4\xB8\xAD\xF0\x9F\x98\x80");
    // 非法转义仍报 kInvalidEscape（与 validate_utf8 无关）。
    CheckError(R"("\uD800")", JsonFailure::kInvalidEscape, 7, 1, 8, strict);
  }

  // ---- 与 allow_bom 组合：输入开头的 BOM 归 allow_bom 管，与内容校验无关 ----
  {
    ParserConfig config;
    config.validate_utf8 = true;
    config.allow_bom = true;
    const std::string input = std::string("\xEF\xBB\xBF") + "{\"a\":\"\xE4\xB8\xAD\"}";
    Document document;
    CHECK(ParseText(input, document, config));
    CHECK(document.Root()["a"].AsText() == "\xE4\xB8\xAD");

    // allow_bom=false 时输入开头的 BOM 仍是非法字符（与 validate_utf8 无关）。
    ParserConfig no_bom;
    no_bom.validate_utf8 = true;
    CheckError(input, JsonFailure::kUnexpectedChar, 0, 1, 1, no_bom);
  }
}

// ---------------------------------------------------------------------------
// 随机 UTF-8 属性测试
// ---------------------------------------------------------------------------

void TestUtf8Random() {
  std::printf("\n[14] 随机 UTF-8（属性测试）...\n");
  std::mt19937_64 rng(0x5EED5EEDULL);
  ParserConfig strict;
  strict.validate_utf8 = true;

  // ---- 属性 1：随机合法 UTF-8 必须全部通过，且解码字节与输入逐字节相等 ------
  constexpr std::size_t kLegalRounds = 2000;
  for (std::size_t round = 0; round < kLegalRounds; ++round) {
    std::string content;
    const std::size_t count = static_cast<std::size_t>(rng() % 24);
    for (std::size_t index = 0; index < count; ++index) {
      std::uint32_t code = 0;
      for (;;) {
        code = static_cast<std::uint32_t>(rng() % 0x110000ULL);
        if (code >= 0xD800u && code <= 0xDFFFu) continue;              // 代理区不是码点
        if (code < 0x20u || code == 0x22u || code == 0x5Cu) continue;  // 需要 JSON 转义
        break;
      }
      if (code <= 0x7Fu) {
        content.push_back(static_cast<char>(code));
      } else if (code <= 0x7FFu) {
        content.push_back(static_cast<char>(0xC0u | (code >> 6)));
        content.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
      } else if (code <= 0xFFFFu) {
        content.push_back(static_cast<char>(0xE0u | (code >> 12)));
        content.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
        content.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
      } else {
        content.push_back(static_cast<char>(0xF0u | (code >> 18)));
        content.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
        content.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
        content.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
      }
    }
    Parser parser;
    Document document;
    ErrorPtr error;
    // 输入缓冲必须活到 AsText() 比较之后：Document 里的借用 Text 指向它。
    const std::string input = Quote(content);
    ++g_checks;
    if (!parser.Parse(input, document, error, strict)) {
      std::fprintf(stderr, "CHECK failed: 随机合法 UTF-8 第 %zu 轮被拒绝: %s\n", round,
                   ErrorMessage(error).c_str());
      ++g_failures;
      continue;
    }
    CHECK(document.Root().AsText() == content);
  }

  // ---- 属性 2：在 ASCII 串任意位置注入非法字节，必须在注入点失败 ------------
  const unsigned int kBadBytes[] = {0x80u, 0xBFu, 0xC0u, 0xC1u, 0xF5u, 0xFFu};
  constexpr std::size_t kInjectRounds = 500;
  for (std::size_t round = 0; round < kInjectRounds; ++round) {
    std::string content;
    const std::size_t length = static_cast<std::size_t>(rng() % 12);
    for (std::size_t index = 0; index < length; ++index) {
      content.push_back(static_cast<char>('a' + (rng() % 26)));
    }
    const std::size_t inject_at =
        content.empty() ? 0 : static_cast<std::size_t>(rng() % (content.size() + 1));
    const unsigned int bad_byte = kBadBytes[rng() % (sizeof(kBadBytes) / sizeof(kBadBytes[0]))];
    std::string broken = content;
    broken.insert(broken.begin() + static_cast<std::ptrdiff_t>(inject_at),
                  static_cast<char>(static_cast<unsigned char>(bad_byte)));

    Parser parser;
    Document document;
    ErrorPtr error;
    const std::string input = Quote(broken);
    ++g_checks;
    if (parser.Parse(input, document, error, strict)) {
      std::fprintf(stderr, "CHECK failed: 随机非法 UTF-8 第 %zu 轮被接受\n", round);
      ++g_failures;
      continue;
    }
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    ++g_checks;
    if (json_error == nullptr || json_error->Failure() != JsonFailure::kInvalidUtf8 ||
        json_error->Offset() != inject_at + 1) {
      std::fprintf(stderr, "CHECK failed: 随机非法 UTF-8 第 %zu 轮定位错误\n", round);
      ++g_failures;
    }
  }

  // ---- 属性 3：关闭校验时同样的非法串全部透传且字节不变 --------------------
  for (std::size_t round = 0; round < kInjectRounds; ++round) {
    std::string content;
    const std::size_t length = static_cast<std::size_t>(rng() % 12);
    for (std::size_t index = 0; index < length; ++index) {
      content.push_back(static_cast<char>('a' + (rng() % 26)));
    }
    const std::size_t inject_at =
        content.empty() ? 0 : static_cast<std::size_t>(rng() % (content.size() + 1));
    const unsigned int bad_byte = kBadBytes[rng() % (sizeof(kBadBytes) / sizeof(kBadBytes[0]))];
    std::string broken = content;
    broken.insert(broken.begin() + static_cast<std::ptrdiff_t>(inject_at),
                  static_cast<char>(static_cast<unsigned char>(bad_byte)));
    Document document;
    const std::string input = Quote(broken);
    CHECK(ParseText(input, document, ParserConfig{}));
    CHECK(document.Root().AsText() == broken);
  }
}

// ---------------------------------------------------------------------------
// 吞吐（量化，不做硬阈值断言）
// ---------------------------------------------------------------------------

void TestLimitsThroughput() {
  std::printf("\n[15] 上限/校验吞吐（量化）...\n");

  // 5 MiB 合法 UTF-8（ASCII + 中文 + emoji 混合），严格模式解析 20 轮。
  // 单轮 5 MiB、20 轮 100 MiB，总时长落在 100 ms 量级，避免 <100 ms 的计时噪声。
  std::string payload;
  payload.reserve(5u << 20);
  while (payload.size() < (5u << 20)) {
    payload += "hello 世界 \xF0\x9F\x98\x80 ";
  }
  ParserConfig strict;
  strict.validate_utf8 = true;
  Parser parser;
  Document document;
  ErrorPtr error;
  // 输入缓冲必须在所有轮次后仍然有效（Document 借用它，不拷贝）。
  const std::string input = Quote(payload);
  constexpr int kRounds = 20;
  bool all_ok = true;
  const auto begin = std::chrono::steady_clock::now();
  for (int round = 0; round < kRounds; ++round) {
    if (!parser.Parse(input, document, error, strict)) {
      std::fprintf(stderr, "CHECK failed: 吞吐用例解析失败: %s\n", ErrorMessage(error).c_str());
      ++g_failures;
      all_ok = false;
      break;
    }
  }
  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - begin).count();
  const double mebibytes =
      static_cast<double>(payload.size()) * static_cast<double>(kRounds) / (1024.0 * 1024.0);
  std::printf("  UTF-8 校验: %.0f MiB / %.3f s = %.0f MiB/s\n", mebibytes, seconds,
              mebibytes / seconds);
  ++g_checks;
  CHECK(all_ok);
  CHECK(document.Root().IsString());
}

// ---------------------------------------------------------------------------
// 场景 7/9 的真实用法模拟（7 个）
// ---------------------------------------------------------------------------

void ScenarioLimits() {
  std::printf("\n[16] 真实场景：单值上限 + UTF-8 严格校验...\n");

  // 场景 A：日志采集把 2 MiB 堆栈塞进 message；网关限制单字段 64 KiB。
  {
    ParserConfig config;
    config.max_string_bytes = 64 * 1024;
    const std::string payload(2u << 20, 'x');
    const std::string input = "{\"level\":\"error\",\"message\":\"" + payload + "\"}";
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(!parser.Parse(input, document, error, config));
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr && json_error->Failure() == JsonFailure::kStringTooLarge);
    CHECK(json_error != nullptr && json_error->Offset() == 27);  // message 值的开引号
    Document small_document;
    CHECK(ParseText("{\"level\":\"error\",\"message\":\"boom\"}", small_document, config));
  }

  // 场景 B：恶意 128 KiB 对象 key（key 进成员表，越长越吃内存）。
  {
    ParserConfig config;
    config.max_string_bytes = 1024;
    const std::string key(128 * 1024, 'k');
    CheckError("{\"" + key + "\":1}", JsonFailure::kStringTooLarge, 1, 1, 2, config);
    Document document;
    CHECK(ParseText("{\"user_id\":1}", document, config));
  }

  // 场景 C：19 位雪花 ID 正常；畸形 10 万位数字被拒（防 Decimal 膨胀）。
  {
    ParserConfig config;
    config.max_string_bytes = 32;
    Document document;
    CHECK(ParseText("{\"id\":1234567890123456789}", document, config));
    CHECK(document.Root()["id"].IsInt());
    const std::string bogus = "{\"id\":" + std::string(100000, '9') + "}";
    CheckError(bogus, JsonFailure::kStringTooLarge, 6, 1, 7, config);
  }

  // 场景 D：多语言 API 响应（中文 / emoji / 日文）在严格模式下正常解析。
  {
    ParserConfig config;
    config.validate_utf8 = true;
    Document document;
    const std::string input =
        "{\"user\":\"张三\",\"city\":\"北京\",\"bio\":\"emoji 😀 と 日本語\","
        "\"tags\":[\"a\",\"é\"]}";
    CHECK(ParseText(input, document, config));
    CHECK(document.Root()["user"].AsText() == "张三");
    CHECK(document.Root()["city"].AsText() == "北京");
    CHECK(document.Root()["tags"].Size() == 2);
    CHECK(document.Root()["tags"].At(1).AsText() == "é");
  }

  // 场景 E：日志采集截断 UTF-8（0xE4 0xB8 后直接闭引号）；严格模式拒绝，
  //         默认模式仍透传（不破坏历史数据管道）。
  {
    const std::string input = "{\"msg\":\"" + Bytes({0xE4, 0xB8}) + "\"}";
    ParserConfig strict;
    strict.validate_utf8 = true;
    CheckError(input, JsonFailure::kInvalidUtf8, 8, 1, 9, strict);
    Document document;
    CHECK(ParseText(input, document, ParserConfig{}));
    CHECK(document.Root()["msg"].AsText().size() == 2);
  }

  // 场景 F：网关同时开两个闸门，用 ParsePartial 定位第一条坏数据。
  {
    ParserConfig config;
    config.max_string_bytes = 16;
    config.validate_utf8 = true;
    const std::string input = "{\"ok\":\"hello\",\"bad\":\"" + Bytes({0x80}) + "\",\"huge\":\"" +
                              std::string(32, 'x') + "\"}";
    Parser parser;
    Document document;
    ErrorPtr error;
    const auto outcome = parser.ParsePartial(input, document, error, config);
    CHECK(!outcome.ok);
    CHECK(outcome.consumed == 21);  // "bad" 值的非法字节偏移
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr && json_error->Failure() == JsonFailure::kInvalidUtf8);
    CHECK(json_error != nullptr && json_error->Offset() == 21);
  }

  // 场景 G：NDJSON 流中第 3 行字段非法；ParseMany 返回已成功行数，错误相对该行。
  {
    ParserConfig config;
    config.validate_utf8 = true;
    const std::string input =
        "{\"n\":1}\n{\"n\":2}\n{\"m\":\"" + Bytes({0x80}) + "\"}\n{\"n\":4}\n";
    Parser parser;
    ErrorPtr error;
    std::size_t parsed = 0;
    const std::size_t count = parser.ParseMany(
        input, [&parsed](Document&) { ++parsed; return true; }, error, config);
    CHECK(count == 2);
    CHECK(parsed == 2);
    const auto* json_error = dynamic_cast<const JsonError*>(error.get());
    CHECK(json_error != nullptr && json_error->Failure() == JsonFailure::kInvalidUtf8);
    CHECK(json_error != nullptr && json_error->Offset() == 6);  // 相对该行
  }
}

// ---------------------------------------------------------------------------
// 场景 5：错误恢复（ErrorPolicy）
// ---------------------------------------------------------------------------

/// @brief 构造启用错误恢复的配置（默认上限 16，与 `ParserConfig` 一致）。
ParserConfig MakeRecoveryConfig(ErrorPolicy policy, std::uint32_t max_errors = 16) {
  ParserConfig config;
  config.error_policy = policy;
  config.max_errors = max_errors;
  return config;
}

/// @brief 取第 index 条恢复错误；越界或类型不符返回 nullptr。
const JsonError* RecoveryErrorAt(const Document& document, std::size_t index) {
  if (index >= document.Errors().size()) {
    return nullptr;
  }
  return dynamic_cast<const JsonError*>(document.Errors()[index].get());
}

/// @brief 场景 5a：kSkipValue——坏值填 null、兄弟成员/元素保留、只记首个错误。
void TestErrorRecoverySkipValue() {
  std::printf("\n[17] 错误恢复：kSkipValue...\n");

  // ---- 对象成员里一个坏值：坏值位置 = null，前后成员原样保留 ----------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(R"({"a":1,"b":@,"c":3})", document, error,
                       MakeRecoveryConfig(ErrorPolicy::kSkipValue)));
    CHECK(error == nullptr);  // 恢复成功即成功：error 必须为空（既有契约）
    CHECK(document.Root().IsObject());
    CHECK(document.Root().Size() == 3);  // 成员个数与顺序不变
    CHECK(document.Root()["a"].As<int>() == 1);
    CHECK(document.Root()["b"].IsNull());
    CHECK(document.Root()["c"].As<int>() == 3);
    CHECK(document.ErrorCount() == 1);
    const JsonError* failure = RecoveryErrorAt(document, 0);
    CHECK(failure != nullptr);
    if (failure != nullptr) {
      CHECK(failure->Failure() == JsonFailure::kUnexpectedChar);
      CHECK(failure->Offset() == 11);  // 指向 `@`
      CHECK(failure->Line() == 1);
      CHECK(failure->Column() == 12);
    }
  }

  // ---- 数组元素里一个坏值：元素个数不变，坏位置 = null ----------------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse("[1,@,3]", document, error,
                       MakeRecoveryConfig(ErrorPolicy::kSkipValue)));
    CHECK(document.Root().IsArray());
    CHECK(document.Root().Size() == 3);
    CHECK(document.Root().At(0).As<int>() == 1);
    CHECK(document.Root().At(1).IsNull());
    CHECK(document.Root().At(2).As<int>() == 3);
    CHECK(document.ErrorCount() == 1);
  }

  // ---- 嵌套容器里的坏值：只把坏值本身置 null，外层容器与兄弟成员保留 --------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(R"({"a":{"x":1,"y":@},"b":2})", document, error,
                       MakeRecoveryConfig(ErrorPolicy::kSkipValue)));
    CHECK(document.Root().Size() == 2);
    CHECK(document.Root()["a"].IsObject());       // 外层对象不受牵连
    CHECK(document.Root()["a"]["x"].As<int>() == 1);
    CHECK(document.Root()["a"]["y"].IsNull());    // 只有坏值本身变 null
    CHECK(document.Root()["b"].As<int>() == 2);
    CHECK(document.ErrorCount() == 1);
  }

  // ---- 坏值在最后一个成员 / 坏值位置本身就是闭合符 --------------------------
  {
    Parser parser;
    Document last_bad;
    ErrorPtr error1;
    CHECK(parser.Parse(R"({"a":@})", last_bad, error1,
                       MakeRecoveryConfig(ErrorPolicy::kSkipValue)));
    CHECK(last_bad.Root().Size() == 1);
    CHECK(last_bad.Root()["a"].IsNull());

    Document close_bad;
    ErrorPtr error2;
    CHECK(parser.Parse(R"({"a":})", close_bad, error2,
                       MakeRecoveryConfig(ErrorPolicy::kSkipValue)));
    CHECK(close_bad.Root().Size() == 1);
    CHECK(close_bad.Root()["a"].IsNull());
    CHECK(close_bad.ErrorCount() == 1);
  }

  // ---- 字符串内部的错误：同步扫描必须跨过整条字符串，不能吞掉后续成员 ------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(R"({"s":"a\qb","n":2})", document, error,
                       MakeRecoveryConfig(ErrorPolicy::kSkipValue)));
    CHECK(document.Root().Size() == 2);
    CHECK(document.Root()["s"].IsNull());
    CHECK(document.Root()["n"].As<int>() == 2);
    const JsonError* failure = RecoveryErrorAt(document, 0);
    CHECK(failure != nullptr);
    if (failure != nullptr) {
      CHECK(failure->Failure() == JsonFailure::kInvalidEscape);
    }
  }

  // ---- kSkipValue 只保留首个错误：后续坏值照常跳过但不记录 ------------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(R"({"a":@,"b":@,"c":@})", document, error,
                       MakeRecoveryConfig(ErrorPolicy::kSkipValue)));
    CHECK(document.Root().Size() == 3);
    CHECK(document.Root()["a"].IsNull());
    CHECK(document.Root()["b"].IsNull());
    CHECK(document.Root()["c"].IsNull());
    CHECK(document.ErrorCount() == 1);
  }
}

/// @brief 场景 5b：kCollect——收集全部错误（受 max_errors 限制），文档可继续使用。
void TestErrorRecoveryCollect() {
  std::printf("\n[18] 错误恢复：kCollect...\n");

  // ---- 三个坏值：全部收集，偏移严格递增 ------------------------------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(R"({"a":@,"b":@,"c":@})", document, error,
                       MakeRecoveryConfig(ErrorPolicy::kCollect)));
    CHECK(document.Root().Size() == 3);
    CHECK(document.ErrorCount() == 3);
    const std::size_t expected_offsets[] = {5, 11, 17};
    for (std::size_t index = 0; index < 3; ++index) {
      const JsonError* failure = RecoveryErrorAt(document, index);
      CHECK(failure != nullptr);
      if (failure != nullptr) {
        CHECK(failure->Failure() == JsonFailure::kUnexpectedChar);
        CHECK(failure->Offset() == expected_offsets[index]);
      }
    }
  }

  // ---- 不同失败原因都能恢复：非法数字 / 非法转义 / 残缺字面量 ---------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(R"({"n":01,"s":"\q","t":tru,"ok":1})", document, error,
                       MakeRecoveryConfig(ErrorPolicy::kCollect)));
    CHECK(document.Root().Size() == 4);
    CHECK(document.Root()["n"].IsNull());
    CHECK(document.Root()["s"].IsNull());
    CHECK(document.Root()["t"].IsNull());
    CHECK(document.Root()["ok"].As<int>() == 1);
    CHECK(document.ErrorCount() == 3);
    const JsonError* number = RecoveryErrorAt(document, 0);
    const JsonError* escape = RecoveryErrorAt(document, 1);
    const JsonError* literal = RecoveryErrorAt(document, 2);
    CHECK(number != nullptr && number->Failure() == JsonFailure::kInvalidNumber);
    CHECK(escape != nullptr && escape->Failure() == JsonFailure::kInvalidEscape);
    CHECK(literal != nullptr && literal->Failure() == JsonFailure::kUnexpectedChar);
  }

  // ---- 容器内部的**结构**错误（尾逗号）不是「坏值」，按不可恢复处理 --------
  {
    Parser parser;
    Document array_trailing;
    ErrorPtr error1;
    CHECK(!parser.Parse(R"({"a":[1,],"b":2})", array_trailing, error1,
                        MakeRecoveryConfig(ErrorPolicy::kCollect)));
    CHECK(array_trailing.Empty());
    const JsonError* failure = dynamic_cast<const JsonError*>(error1.get());
    CHECK(failure != nullptr && failure->Failure() == JsonFailure::kTrailingComma);

    Document object_trailing;
    ErrorPtr error2;
    CHECK(!parser.Parse(R"({"a":{"x":1,},"b":2})", object_trailing, error2,
                        MakeRecoveryConfig(ErrorPolicy::kCollect)));
    CHECK(object_trailing.Empty());
  }

  // ---- 嵌套数组里的坏值：只影响它所在的位置，兄弟元素照常保留 ---------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(R"({"a":[1,[2,@],4],"b":5})", document, error,
                       MakeRecoveryConfig(ErrorPolicy::kCollect)));
    CHECK(document.Root().Size() == 2);
    CHECK(document.Root()["a"].IsArray());
    CHECK(document.Root()["a"].Size() == 3);
    CHECK(document.Root()["a"].At(0).As<int>() == 1);
    CHECK(document.Root()["a"].At(1).IsArray());
    CHECK(document.Root()["a"].At(1).At(0).As<int>() == 2);
    CHECK(document.Root()["a"].At(1).At(1).IsNull());  // 坏值本身
    CHECK(document.Root()["a"].At(2).As<int>() == 4);
    CHECK(document.Root()["b"].As<int>() == 5);
    CHECK(document.ErrorCount() == 1);
  }

  // ---- max_errors 截断：文档照常恢复，错误清单只留前 N 条 ------------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(R"({"a":@,"b":@,"c":@,"d":@})", document, error,
                       MakeRecoveryConfig(ErrorPolicy::kCollect, 2)));
    CHECK(document.Root().Size() == 4);
    CHECK(document.Root()["a"].IsNull());
    CHECK(document.Root()["d"].IsNull());
    CHECK(document.ErrorCount() == 2);
    const JsonError* first = RecoveryErrorAt(document, 0);
    const JsonError* second = RecoveryErrorAt(document, 1);
    CHECK(first != nullptr && first->Offset() == 5);
    CHECK(second != nullptr && second->Offset() == 11);
  }

  // ---- max_errors = 0：恢复但一条也不记录 ----------------------------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(R"({"a":@,"b":@})", document, error,
                       MakeRecoveryConfig(ErrorPolicy::kCollect, 0)));
    CHECK(document.Root().Size() == 2);
    CHECK(document.Root()["a"].IsNull());
    CHECK(document.Root()["b"].IsNull());
    CHECK(document.Errors().empty());
  }

  // ---- 默认上限 16：第 17 个坏值仍填 null，但不再记录 ----------------------
  {
    std::string text = "{";
    for (int index = 0; index < 17; ++index) {
      if (index != 0) {
        text += ',';
      }
      text += "\"k" + std::to_string(index) + "\":@";
    }
    text += '}';
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(parser.Parse(text, document, error, MakeRecoveryConfig(ErrorPolicy::kCollect)));
    CHECK(document.Root().Size() == 17);
    CHECK(document.ErrorCount() == 16);
    for (std::size_t index = 0; index < document.Root().Size(); ++index) {
      CHECK(document.Root().At(index).IsNull());
    }
  }
}

/// @brief 场景 5c：不可恢复的错误仍然按 kAbort 处理（返回 false、文档清空）。
void TestErrorRecoveryLimits() {
  std::printf("\n[19] 错误恢复：不可恢复场景...\n");

  struct FailureCase {
    const char* text;
    JsonFailure failure;
  };
  const FailureCase cases[] = {
      {"@abc", JsonFailure::kUnexpectedChar},              // 根值本身坏
      {R"({"a":1,"\q":2})", JsonFailure::kInvalidEscape},  // 键不是合法字符串
      {R"({"a":1 "b":2})", JsonFailure::kUnexpectedChar},  // 值后缺逗号
      {R"({"a":[1,2})", JsonFailure::kUnexpectedChar},     // 坏值延伸到输入末尾
      {R"({"a":"oops})", JsonFailure::kUnclosedString},    // 字符串未闭合
      {R"({"a":@}x)", JsonFailure::kTrailingGarbage},      // 恢复后又出现尾随垃圾
  };
  for (const FailureCase& test_case : cases) {
    Parser parser;
    Document document;
    ErrorPtr error;
    CHECK(!parser.Parse(test_case.text, document, error,
                        MakeRecoveryConfig(ErrorPolicy::kCollect)));
    CHECK(document.Empty());
    CHECK(document.Errors().empty());  // 失败即清空：错误清单不跨失败保留
    const JsonError* failure = dynamic_cast<const JsonError*>(error.get());
    CHECK(failure != nullptr);
    if (failure != nullptr) {
      CHECK(failure->Failure() == test_case.failure);
    }
  }

  // ---- 深度超限：防爆栈防线不能被「跳过坏值」绕过 --------------------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    ParserConfig config = MakeRecoveryConfig(ErrorPolicy::kCollect);
    config.max_depth = 2;
    CHECK(!parser.Parse(R"({"a":[[1]]})", document, error, config));
    CHECK(document.Empty());
    const JsonError* failure = dynamic_cast<const JsonError*>(error.get());
    CHECK(failure != nullptr);
    if (failure != nullptr) {
      CHECK(failure->Failure() == JsonFailure::kDepthExceeded);
    }
  }

  // ---- 超时：时间预算耗尽同样不可恢复 --------------------------------------
  {
    std::string big = "[";
    for (int index = 0; index < 200000; ++index) {
      if (index > 0) {
        big += ',';
      }
      big += '1';
    }
    big += "]";
    Parser parser;
    Document document;
    ErrorPtr error;
    ParserConfig config = MakeRecoveryConfig(ErrorPolicy::kCollect);
    config.timeout_ms = 1;
    CHECK(!parser.Parse(big, document, error, config));
    const JsonError* failure = dynamic_cast<const JsonError*>(error.get());
    CHECK(failure != nullptr);
    if (failure != nullptr) {
      CHECK(failure->Failure() == JsonFailure::kTimeout);
    }
  }
}

/// @brief 场景 5d：恢复路径与其它策略/入口的组合（重复 key、三 Policy、部分解析、NDJSON）。
void TestErrorRecoveryInterop() {
  std::printf("\n[20] 错误恢复：与其它策略/入口组合...\n");

  // ---- duplicate_key == kError：恢复后遇到重复 key 仍然中止 ----------------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    ParserConfig config = MakeRecoveryConfig(ErrorPolicy::kCollect);
    config.duplicate_key = DuplicateKeyPolicy::kError;
    CHECK(!parser.Parse(R"({"a":@,"a":2})", document, error, config));
    const JsonError* failure = dynamic_cast<const JsonError*>(error.get());
    CHECK(failure != nullptr);
    if (failure != nullptr) {
      CHECK(failure->Failure() == JsonFailure::kDuplicateKey);
    }
  }

  // ---- duplicate_key == kLast：坏值先填 null，后续同名成员再合并覆盖 --------
  {
    Parser parser;
    Document document;
    ErrorPtr error;
    ParserConfig config = MakeRecoveryConfig(ErrorPolicy::kCollect);
    config.duplicate_key = DuplicateKeyPolicy::kLast;
    CHECK(parser.Parse(R"({"a":1,"b":@,"a":2})", document, error, config));
    CHECK(document.Root().Size() == 2);
    CHECK(document.Root()["a"].As<int>() == 2);  // 最后生效
    CHECK(document.Root()["b"].IsNull());
    CHECK(document.ErrorCount() == 1);
  }

  // ---- 三 Policy 的恢复结果与错误清单一致 ----------------------------------
  {
    const std::string_view text = R"({"a":@,"b":[1,@],"c":3})";
    ParserConfig config = MakeRecoveryConfig(ErrorPolicy::kCollect);
    Document inline_document;
    Document std_document;
    Document compact_document;
    ErrorPtr inline_error;
    ErrorPtr std_error;
    ErrorPtr compact_error;
    BasicParser<InlinePolicy> inline_parser;
    BasicParser<StdPolicy> std_parser;
    BasicParser<CompactPolicy> compact_parser;
    CHECK(inline_parser.Parse(text, inline_document, inline_error, config));
    CHECK(std_parser.Parse(text, std_document, std_error, config));
    CHECK(compact_parser.Parse(text, compact_document, compact_error, config));
    CHECK(DeepEqual(inline_document.Root(), std_document.Root()));
    CHECK(DeepEqual(inline_document.Root(), compact_document.Root()));
    CHECK(inline_document.ErrorCount() == std_document.ErrorCount());
    CHECK(inline_document.ErrorCount() == compact_document.ErrorCount());
    CHECK(inline_document.ErrorCount() == 2);
  }

  // ---- ParsePartial：恢复成功时 ok = true、consumed = 输入长度 -------------
  {
    constexpr std::string_view kText = R"({"a":@,"b":2})";
    Parser parser;
    Document document;
    ErrorPtr error;
    const go2cpp::json::ParseOutcome outcome =
        parser.ParsePartial(kText, document, error, MakeRecoveryConfig(ErrorPolicy::kCollect));
    CHECK(outcome.ok);
    CHECK(outcome.consumed == kText.size());
    CHECK(document.Root().Size() == 2);
    CHECK(document.ErrorCount() == 1);
  }

  // ---- ParseMany：坏行被恢复，后续行继续解析 -------------------------------
  {
    Parser parser;
    ErrorPtr error;
    std::vector<std::size_t> sizes;
    const std::size_t count = parser.ParseMany(
        "{\"a\":1}\n{\"b\":@}\n{\"c\":3}\n",
        [&sizes](Document& document) {
          sizes.push_back(document.Root().Size());
          return true;
        },
        error, MakeRecoveryConfig(ErrorPolicy::kCollect));
    CHECK(count == 3);
    CHECK(sizes.size() == 3);
    if (sizes.size() == 3) {
      CHECK(sizes[0] == 1);
      CHECK(sizes[1] == 1);
      CHECK(sizes[2] == 1);
    }
    CHECK(error == nullptr);
  }
}

/// @brief 真实场景 1：线上配置部分损坏——坏字段置 null，其余配置项照常可读。
void ScenarioRecoverBrokenConfig() {
  std::printf("\n场景 5-1：配置部分损坏...\n");
  constexpr std::string_view kConfig = R"({
    "server": {"host": "0.0.0.0", "port": 8080},
    "timeout_ms": 30e,
    "retry": 3,
    "features": ["cache", @, "metrics"]
  })";
  Parser parser;
  Document document;
  ErrorPtr error;
  CHECK(parser.Parse(kConfig, document, error, MakeRecoveryConfig(ErrorPolicy::kCollect)));
  CHECK(document.Root().IsObject());
  CHECK(document.Root()["server"]["port"].As<int>() == 8080);
  CHECK(document.Root()["timeout_ms"].IsNull());  // 坏配置降级成 null
  CHECK(document.Root()["retry"].As<int>() == 3); // 后续字段仍然可用
  CHECK(document.Root()["features"].Size() == 3);
  CHECK(document.Root()["features"].At(1).IsNull());
  CHECK(document.ErrorCount() == 2);
  const JsonError* number = RecoveryErrorAt(document, 0);
  const JsonError* element = RecoveryErrorAt(document, 1);
  CHECK(number != nullptr && number->Failure() == JsonFailure::kInvalidNumber);
  CHECK(element != nullptr && element->Failure() == JsonFailure::kUnexpectedChar);
  std::printf("  配置项 server.port=%d retry=%d，坏字段 2 个已置 null\n",
              document.Root()["server"]["port"].As<int>(),
              document.Root()["retry"].As<int>());
}

/// @brief 真实场景 2：API 响应数组里个别元素损坏——其余元素仍可索引。
void ScenarioRecoverApiArray() {
  std::printf("\n场景 5-2：API 数组个别元素损坏...\n");
  constexpr std::string_view kResponse =
      R"([{"id":1,"name":"a"},{"id":2,"name":@},{"id":3,"name":"c"}])";
  Parser parser;
  Document document;
  ErrorPtr error;
  CHECK(parser.Parse(kResponse, document, error, MakeRecoveryConfig(ErrorPolicy::kSkipValue)));
  CHECK(document.Root().IsArray());
  CHECK(document.Root().Size() == 3);
  CHECK(document.Root().At(0)["id"].As<int>() == 1);
  CHECK(document.Root().At(1).IsObject());          // 坏元素的对象骨架保留
  CHECK(document.Root().At(1)["id"].As<int>() == 2);
  CHECK(document.Root().At(1)["name"].IsNull());    // 只有坏字段降级为 null
  CHECK(document.Root().At(2)["id"].As<int>() == 3);
  std::printf("  三个元素全部保留，第 2 个元素的 name 字段降级为 null\n");
}

/// @brief 真实场景 3：NDJSON 批量日志——坏行恢复后继续处理后续行。
void ScenarioRecoverNdjson() {
  std::printf("\n场景 5-3：NDJSON 批量日志...\n");
  constexpr std::string_view kLines =
      "{\"level\":\"info\",\"msg\":\"start\"}\n"
      "{\"level\":\"warn\",\"msg\":@}\n"
      "{\"level\":\"error\",\"msg\":\"boom\"}\n";
  Parser parser;
  ErrorPtr error;
  std::vector<std::string> levels;
  const std::size_t count = parser.ParseMany(
      kLines,
      [&levels](Document& document) {
        const Value level = document.Root()["level"];
        levels.push_back(std::string(level.AsText()));
        return true;
      },
      error, MakeRecoveryConfig(ErrorPolicy::kCollect));
  CHECK(count == 3);
  CHECK(levels.size() == 3);
  if (levels.size() == 3) {
    CHECK(levels[0] == "info");
    CHECK(levels[1] == "warn");  // 坏字段被跳过，行本身仍然有效
    CHECK(levels[2] == "error");
  }
  std::printf("  三行全部产出事件，第 2 行坏字段已置 null 并计数\n");
}

/// @brief 真实场景 4：错误清单用于排错——每条都能定位到行/列与原因。
void ScenarioRecoverErrorReport() {
  std::printf("\n场景 5-4：错误清单定位...\n");
  constexpr std::string_view kText =
      "{\n"
      "  \"a\": 01,\n"
      "  \"b\": @,\n"
      "  \"c\": 3\n"
      "}";
  Parser parser;
  Document document;
  ErrorPtr error;
  CHECK(parser.Parse(kText, document, error, MakeRecoveryConfig(ErrorPolicy::kCollect)));
  CHECK(document.ErrorCount() == 2);
  const JsonError* first = RecoveryErrorAt(document, 0);
  const JsonError* second = RecoveryErrorAt(document, 1);
  CHECK(first != nullptr);
  CHECK(second != nullptr);
  if (first != nullptr && second != nullptr) {
    CHECK(first->Failure() == JsonFailure::kInvalidNumber);
    CHECK(first->Line() == 2);
    CHECK(second->Failure() == JsonFailure::kUnexpectedChar);
    CHECK(second->Line() == 3);
    std::printf("  %s\n  %s\n", go2cpp::ErrorMessage(document.Errors()[0]).c_str(),
                go2cpp::ErrorMessage(document.Errors()[1]).c_str());
  }
}

/// @brief 真实场景 5：错误风暴——超过 max_errors 后文档仍完整恢复。
void ScenarioRecoverErrorStorm() {
  std::printf("\n场景 5-5：错误风暴（max_errors 截断）...\n");
  std::string text = "{\"good\":1";
  for (int index = 0; index < 40; ++index) {
    text += ",\"bad" + std::to_string(index) + "\":@";
  }
  text += ",\"tail\":42}";
  Parser parser;
  Document document;
  ErrorPtr error;
  CHECK(parser.Parse(text, document, error, MakeRecoveryConfig(ErrorPolicy::kCollect, 5)));
  CHECK(document.Root().Size() == 42);  // 1 good + 40 bad + 1 tail
  CHECK(document.Root()["good"].As<int>() == 1);
  CHECK(document.Root()["tail"].As<int>() == 42);
  CHECK(document.ErrorCount() == 5);  // 上限生效，但恢复不中断
  CHECK(document.Root()["bad39"].IsNull());
  std::printf("  42 个成员全部保留，错误清单按上限截断为 5 条\n");
}

/// @brief 错误名称：每个枚举值都必须返回固定短语（变异测试暴露的缺口）。
void TestFailureNames() {
  std::printf("\n[17] 错误名称...\n");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kUnexpectedChar)) ==
        "unexpected character");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kUnexpectedEnd)) ==
        "unexpected end of input");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kUnclosedString)) ==
        "unclosed string");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kUnescapedControl)) ==
        "unescaped control character");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kInvalidEscape)) ==
        "invalid escape sequence");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kInvalidNumber)) ==
        "invalid number");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kTrailingComma)) ==
        "trailing comma");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kDepthExceeded)) ==
        "maximum depth exceeded");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kTrailingGarbage)) ==
        "trailing garbage");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kTimeout)) == "timeout");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kTooLarge)) == "input too large");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kDuplicateKey)) == "duplicate key");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kStringTooLarge)) ==
        "string too large");
  CHECK(std::string(go2cpp::json::JsonFailureName(JsonFailure::kInvalidUtf8)) == "invalid utf-8");
}

}  // namespace

int main() {
  const auto begin = std::chrono::steady_clock::now();

  TestBasicTypes();
  TestStrings();
  TestScanBlockBoundaries();
  TestNumbers();
  TestInvalidInput();
  TestCrlfEquivalence();
  TestPolicyEquivalence();
  TestZeroCopy();
  TestDeepNesting();
  TestRandomJson();
  TestTimeout();
  TestPartialParse();
  TestDuplicateKeys();
  TestFailureNames();
  TestStringTooLarge();
  TestUtf8Validation();
  TestUtf8Random();
  TestLimitsThroughput();
  TestErrorRecoverySkipValue();
  TestErrorRecoveryCollect();
  TestErrorRecoveryLimits();
  TestErrorRecoveryInterop();

  ScenarioConfig();
  ScenarioApiResponse();
  ScenarioLogEvent();
  ScenarioDecimalPrice();
  ScenarioBigIdentifiers();
  ScenarioUnicode();
  ScenarioLimits();
  ScenarioRecoverBrokenConfig();
  ScenarioRecoverApiArray();
  ScenarioRecoverNdjson();
  ScenarioRecoverErrorReport();
  ScenarioRecoverErrorStorm();

  const auto end = std::chrono::steady_clock::now();
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();

  if (g_failures != 0) {
    std::fprintf(stderr, "FAILED: %zu check(s) failed out of %zu\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL TESTS PASSED (%zu checks, %lld ms)\n", g_checks,
              static_cast<long long>(elapsed_ms));
  return 0;
}
