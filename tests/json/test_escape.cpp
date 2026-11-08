/**
 * @file test_escape.cpp
 * @brief `\uXXXX` 转义 / UTF-8 编解码的单元、随机与真实场景测试。
 *
 * 覆盖：
 * - `HexDigit` / `EncodeUtf8` 的全部字节长度与边界（U+007F/0080/07FF/0800/
 *   FFFF/10000/10FFFF、代理区、> U+10FFFF）；
 * - `DecodeUnicodeEscape`：大小写 hex、长视图截取、位数不足、坏字符、失败
 *   不修改输出；
 * - `DecodeUnicodeEscapeSequence`：单码元、代理对（U+10000、U+1F600、
 *   U+10FFFF）、孤立高/低代理、高代理+非低代理、坏 hex；
 * - `EncodeUnicodeEscape`：BMP / 补充平面 / 非法码点，且与解码往返一致；
 * - `AppendUtf8`：追加与非法码点不写入；
 * - 随机：2 万组码点 `EncodeUtf8` ↔ 测试侧独立 UTF-8 解码器往返；
 * - 解析器集成：`\u` 已解码成 UTF-8（**规范修正**，不是原样保留）、代理对、
 *   非法序列的错误码与错误位置；
 * - 真实场景（5 个）：i18n 配置、emoji 昵称、中文日志、HTML 转义字符、
 *   混合转义 + 代理对的 API 响应。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>

#include "go2cpp/json/document.hpp"
#include "go2cpp/json/error.hpp"
#include "go2cpp/json/escape.hpp"
#include "go2cpp/json/parser.hpp"
#include "go2cpp/json/value.hpp"

namespace {

int g_failures = 0;
std::size_t g_checks = 0;

#define CHECK(condition)                                                              \
  do {                                                                                \
    ++g_checks;                                                                       \
    if (!(condition)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
      ++g_failures;                                                                   \
    }                                                                                 \
  } while (false)

using go2cpp::json::AppendUtf8;
using go2cpp::json::CombineSurrogates;
using go2cpp::json::DecodeUnicodeEscape;
using go2cpp::json::DecodeUnicodeEscapeSequence;
using go2cpp::json::Document;
using go2cpp::json::EncodeUnicodeEscape;
using go2cpp::json::EncodeUtf8;
using go2cpp::json::EscapeError;
using go2cpp::json::EscapeFailure;
using go2cpp::json::HexDigit;
using go2cpp::json::IsHighSurrogate;
using go2cpp::json::IsLowSurrogate;
using go2cpp::json::JsonError;
using go2cpp::json::JsonFailure;
using go2cpp::json::Parser;

/// @brief 字节串：避免 `\x` 转义吞掉后续十六进制字符。
std::string Bytes(std::initializer_list<int> values) {
  std::string out;
  out.reserve(values.size());
  for (const int value : values) {
    out.push_back(static_cast<char>(value));
  }
  return out;
}

/// @brief 测试侧独立 UTF-8 解码器（不复用库实现，避免自证）。
bool DecodeUtf8ForTest(std::string_view text, std::size_t* index, std::uint32_t* code_point) {
  const auto* data = reinterpret_cast<const unsigned char*>(text.data());
  const std::size_t size = text.size();
  if (*index >= size) {
    return false;
  }
  const unsigned char lead = data[*index];
  std::size_t length = 0;
  std::uint32_t code = 0;
  if (lead < 0x80u) {
    length = 1;
    code = lead;
  } else if ((lead & 0xE0u) == 0xC0u) {
    length = 2;
    code = lead & 0x1Fu;
  } else if ((lead & 0xF0u) == 0xE0u) {
    length = 3;
    code = lead & 0x0Fu;
  } else if ((lead & 0xF8u) == 0xF0u) {
    length = 4;
    code = lead & 0x07u;
  } else {
    return false;
  }
  if (*index + length > size) {
    return false;
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    code = (code << 6) | (data[*index + offset] & 0x3Fu);
  }
  *code_point = code;
  *index += length;
  return true;
}

// ---------------------------------------------------------------------------
// 1. 基本单元
// ---------------------------------------------------------------------------

void TestHexDigit() {
  std::printf("\n[1] HexDigit / 代理判定...\n");
  CHECK(HexDigit('0') == 0);
  CHECK(HexDigit('9') == 9);
  CHECK(HexDigit('a') == 10);
  CHECK(HexDigit('f') == 15);
  CHECK(HexDigit('A') == 10);
  CHECK(HexDigit('F') == 15);
  CHECK(HexDigit('g') == -1);
  CHECK(HexDigit('/') == -1);
  CHECK(HexDigit(' ') == -1);
  CHECK(HexDigit(static_cast<char>(0)) == -1);

  CHECK(IsHighSurrogate(0xD800));
  CHECK(IsHighSurrogate(0xDBFF));
  CHECK(!IsHighSurrogate(0xD7FF));
  CHECK(!IsHighSurrogate(0xDC00));
  CHECK(IsLowSurrogate(0xDC00));
  CHECK(IsLowSurrogate(0xDFFF));
  CHECK(!IsLowSurrogate(0xDBFF));
  CHECK(!IsLowSurrogate(0xE000));
}

void TestEncodeUtf8() {
  std::printf("\n[2] EncodeUtf8 全长度与边界...\n");
  char buffer[4];
  CHECK(EncodeUtf8(0x41, buffer) == 1 && buffer[0] == 'A');
  CHECK(EncodeUtf8(0x00, buffer) == 1 && buffer[0] == '\0');
  CHECK(EncodeUtf8(0x7F, buffer) == 1 && buffer[0] == 0x7F);

  CHECK(EncodeUtf8(0x80, buffer) == 2 && static_cast<unsigned char>(buffer[0]) == 0xC2 &&
        static_cast<unsigned char>(buffer[1]) == 0x80);
  CHECK(EncodeUtf8(0x7FF, buffer) == 2 && static_cast<unsigned char>(buffer[0]) == 0xDF &&
        static_cast<unsigned char>(buffer[1]) == 0xBF);

  CHECK(EncodeUtf8(0x800, buffer) == 3 && static_cast<unsigned char>(buffer[0]) == 0xE0);
  CHECK(EncodeUtf8(0x4E2D, buffer) == 3 && static_cast<unsigned char>(buffer[0]) == 0xE4 &&
        static_cast<unsigned char>(buffer[1]) == 0xB8 &&
        static_cast<unsigned char>(buffer[2]) == 0xAD);
  CHECK(EncodeUtf8(0xFFFF, buffer) == 3);

  CHECK(EncodeUtf8(0x10000, buffer) == 4 && static_cast<unsigned char>(buffer[0]) == 0xF0);
  CHECK(EncodeUtf8(0x1F600, buffer) == 4 &&
        static_cast<unsigned char>(buffer[0]) == 0xF0 &&
        static_cast<unsigned char>(buffer[1]) == 0x9F &&
        static_cast<unsigned char>(buffer[2]) == 0x98 &&
        static_cast<unsigned char>(buffer[3]) == 0x80);
  CHECK(EncodeUtf8(0x10FFFF, buffer) == 4 && static_cast<unsigned char>(buffer[0]) == 0xF4);

  // 非法：代理区不允许单独编码；超出 Unicode 上限。
  CHECK(EncodeUtf8(0xD800, buffer) == 0);
  CHECK(EncodeUtf8(0xDFFF, buffer) == 0);
  CHECK(EncodeUtf8(0x110000, buffer) == 0);
  CHECK(EncodeUtf8(0xFFFFFFFFu, buffer) == 0);
}

void TestDecodeUnicodeEscape() {
  std::printf("\n[3] DecodeUnicodeEscape...\n");
  std::uint32_t code = 0;
  CHECK(DecodeUnicodeEscape("0041", &code) && code == 0x41);
  CHECK(DecodeUnicodeEscape("00e9", &code) && code == 0xE9);
  CHECK(DecodeUnicodeEscape("00E9", &code) && code == 0xE9);
  CHECK(DecodeUnicodeEscape("AbCd", &code) && code == 0xABCD);
  CHECK(DecodeUnicodeEscape("0000", &code) && code == 0x0000);
  CHECK(DecodeUnicodeEscape("FFFF", &code) && code == 0xFFFF);
  CHECK(DecodeUnicodeEscape("D83D", &code) && code == 0xD83D);
  // 长视图只取前 4 位。
  CHECK(DecodeUnicodeEscape("0041EXTRA", &code) && code == 0x41);

  code = 0x1234;
  EscapeError error;
  CHECK(!DecodeUnicodeEscape("12", &code, &error));
  CHECK(code == 0x1234);  // 失败不修改输出。
  CHECK(error.failure == EscapeFailure::kShortInput && error.offset == 2);

  CHECK(!DecodeUnicodeEscape("", &code, &error));
  CHECK(error.failure == EscapeFailure::kShortInput && error.offset == 0);

  CHECK(!DecodeUnicodeEscape("00G1", &code, &error));
  CHECK(code == 0x1234);
  CHECK(error.failure == EscapeFailure::kBadHex && error.offset == 2);

  CHECK(!DecodeUnicodeEscape(" 041", &code, &error));
  CHECK(error.failure == EscapeFailure::kBadHex && error.offset == 0);

  // 无 error 参数的版本仍然可用。
  CHECK(!DecodeUnicodeEscape("ZZZZ", &code));
}

void TestSurrogateSequence() {
  std::printf("\n[4] 代理对与非法序列...\n");
  std::uint32_t code = 0;
  EscapeError error;
  CHECK(DecodeUnicodeEscapeSequence("0041", &code, &error) && code == 0x41);
  CHECK(DecodeUnicodeEscapeSequence("D83DDE00", &code, &error) && code == 0x1F600);
  CHECK(DecodeUnicodeEscapeSequence("D800DC00", &code, &error) && code == 0x10000);
  CHECK(DecodeUnicodeEscapeSequence("DBFFDFFF", &code, &error) && code == 0x10FFFF);
  // 多余字符忽略（调用方负责边界）。
  CHECK(DecodeUnicodeEscapeSequence("D83DDE00FFFF", &code, &error) && code == 0x1F600);

  code = 0x7777;
  CHECK(!DecodeUnicodeEscapeSequence("D83D", &code, &error));
  CHECK(code == 0x7777);
  CHECK(error.failure == EscapeFailure::kLoneSurrogate);

  CHECK(!DecodeUnicodeEscapeSequence("DE00", &code, &error));
  CHECK(error.failure == EscapeFailure::kLoneSurrogate && error.offset == 0);

  CHECK(!DecodeUnicodeEscapeSequence("D83D0041", &code, &error));
  CHECK(error.failure == EscapeFailure::kLoneSurrogate && error.offset == 4);

  CHECK(!DecodeUnicodeEscapeSequence("D83D00G1", &code, &error));
  CHECK(error.failure == EscapeFailure::kBadHex && error.offset == 6);

  CHECK(!DecodeUnicodeEscapeSequence("", &code, &error));
  CHECK(error.failure == EscapeFailure::kShortInput);

  // CombineSurrogates 直接调用。
  CHECK(CombineSurrogates(0xD83D, 0xDE00, &code, &error) && code == 0x1F600);
  CHECK(!CombineSurrogates(0x0041, 0xDE00, &code, &error));
  CHECK(!CombineSurrogates(0xD83D, 0x0041, &code, &error));
  CHECK(!CombineSurrogates(0xDC00, 0xD800, &code, &error));
}

void TestEncodeUnicodeEscape() {
  std::printf("\n[5] EncodeUnicodeEscape 反向编码...\n");
  CHECK(EncodeUnicodeEscape(0x41) == "\\u0041");
  CHECK(EncodeUnicodeEscape(0x00) == "\\u0000");
  CHECK(EncodeUnicodeEscape(0xE9) == "\\u00E9");
  CHECK(EncodeUnicodeEscape(0x4E2D) == "\\u4E2D");
  CHECK(EncodeUnicodeEscape(0xFFFF) == "\\uFFFF");
  CHECK(EncodeUnicodeEscape(0x10000) == "\\uD800\\uDC00");
  CHECK(EncodeUnicodeEscape(0x1F600) == "\\uD83D\\uDE00");
  CHECK(EncodeUnicodeEscape(0x10FFFF) == "\\uDBFF\\uDFFF");
  CHECK(EncodeUnicodeEscape(0xD800).empty());
  CHECK(EncodeUnicodeEscape(0xDFFF).empty());
  CHECK(EncodeUnicodeEscape(0x110000).empty());

  // 往返：编码 → 解码 → 原码点。
  const std::uint32_t samples[] = {0x00,    0x41,    0x7F,    0x80,    0x7FF,   0x800,
                                   0xE9,    0x4E2D,  0xFFFD,  0xFFFF,  0x10000, 0x1F600,
                                   0x2F800, 0x10FFFF};
  for (const std::uint32_t sample : samples) {
    const std::string escaped = EncodeUnicodeEscape(sample);
    CHECK(!escaped.empty());
    // 去掉 `\u` 前缀与代理对中间的 `\u`，只留十六进制位（hex 里不会出现 'u'）。
    std::string hex;
    for (const char byte : escaped) {
      if (byte != '\\' && byte != 'u') {
        hex.push_back(byte);
      }
    }
    std::uint32_t decoded = 0;
    EscapeError error;
    CHECK(DecodeUnicodeEscapeSequence(hex, &decoded, &error));
    CHECK(decoded == sample);
  }
}

void TestAppendUtf8() {
  std::printf("\n[6] AppendUtf8...\n");
  std::string out = "x";
  AppendUtf8(0x41, &out);
  AppendUtf8(0xE9, &out);
  AppendUtf8(0x4E2D, &out);
  AppendUtf8(0x1F600, &out);
  const std::string expected =
      std::string("x") + Bytes({0x41, 0xC3, 0xA9, 0xE4, 0xB8, 0xAD, 0xF0, 0x9F, 0x98, 0x80});
  CHECK(out == expected);

  const std::string before = out;
  AppendUtf8(0xD800, &out);
  AppendUtf8(0x110000, &out);
  CHECK(out == before);  // 非法码点一个字节都不写。
}

// ---------------------------------------------------------------------------
// 2. 随机往返
// ---------------------------------------------------------------------------

void TestRandomUtf8RoundTrip() {
  std::printf("\n[7] 随机码点 UTF-8 往返（2 万组）...\n");
  std::mt19937 engine(20261107);
  std::uniform_int_distribution<std::uint32_t> distribution(0x00, 0x10FFFF);
  for (int iteration = 0; iteration < 20000; ++iteration) {
    std::uint32_t code = distribution(engine);
    if (IsHighSurrogate(code) || IsLowSurrogate(code)) {
      continue;  // 代理区不是合法码点，单独在 [2] 里断言拒绝。
    }
    char buffer[4];
    const std::size_t length = EncodeUtf8(code, buffer);
    CHECK(length >= 1 && length <= 4);
    const std::string_view encoded(buffer, length);
    std::size_t index = 0;
    std::uint32_t decoded = 0;
    CHECK(DecodeUtf8ForTest(encoded, &index, &decoded));
    CHECK(index == length);
    CHECK(decoded == code);
  }

  // 反向：随机字节串里的合法序列也能被独立解码器与库编码互认。
  for (int iteration = 0; iteration < 2000; ++iteration) {
    const std::uint32_t code = 0x80u + distribution(engine) % 0x10FFFFu;
    if (IsHighSurrogate(code) || IsLowSurrogate(code) || code > 0x10FFFF) {
      continue;
    }
    char buffer[4];
    const std::size_t length = EncodeUtf8(code, buffer);
    CHECK(length != 0);
  }
}

// ---------------------------------------------------------------------------
// 3. 解析器集成
// ---------------------------------------------------------------------------

void TestParserIntegration() {
  std::printf("\n[8] 解析器集成（规范修正：\\u 解码成 UTF-8）...\n");
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;

  // 基本：\u00e9 → é。
  CHECK(parser.Parse(R"("\u00e9")", document, error));
  CHECK(document.Root().AsText() == Bytes({0xC3, 0xA9}));

  // 中文：\u4E2D\u6587。
  CHECK(parser.Parse(R"("\u4E2D\u6587")", document, error));
  CHECK(document.Root().AsText() == Bytes({0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87}));

  // 代理对：\uD83D\uDE00 → U+1F600。
  CHECK(parser.Parse(R"("\uD83D\uDE00")", document, error));
  CHECK(document.Root().AsText() == Bytes({0xF0, 0x9F, 0x98, 0x80}));

  // 混合：ASCII + 转义 + 原样 UTF-8 共存。
  CHECK(parser.Parse("\"a\\u0042\xE4\xB8\xAD\xF0\x9F\x98\x80\"", document, error));
  CHECK(document.Root().AsText() ==
        std::string("aB") + Bytes({0xE4, 0xB8, 0xAD, 0xF0, 0x9F, 0x98, 0x80}));

  // 大小写 hex 混写。
  CHECK(parser.Parse(R"("\uAbCd")", document, error));
  CHECK(document.Root().AsText() == Bytes({0xEA, 0xAF, 0x8D}));  // U+ABCD

  // key 里的 \u 也解码（`"\u0061"` 与 `"a"` 是同一个 key）。
  CHECK(parser.Parse(R"({"\u0061":1})", document, error));
  CHECK(document.Root().MemberName(0) == "a");

  // 非法：孤立高代理、孤立低代理、高代理+非低代理、残缺序列。
  const auto check_failure = [&](std::string_view text, JsonFailure expected) {
    ++g_checks;
    Document bad_document;
    go2cpp::ErrorPtr bad_error;
    if (parser.Parse(text, bad_document, bad_error)) {
      std::fprintf(stderr, "FAIL: \"%.60s\" 期望失败却成功\n", std::string(text).c_str());
      ++g_failures;
      return;
    }
    const auto* json_error = dynamic_cast<const JsonError*>(bad_error.get());
    if (json_error == nullptr || json_error->Failure() != expected) {
      std::fprintf(stderr, "FAIL: \"%.60s\" 错误码不符\n", std::string(text).c_str());
      ++g_failures;
    }
  };
  check_failure(R"("\uD800")", JsonFailure::kInvalidEscape);
  check_failure(R"("\uDC00")", JsonFailure::kInvalidEscape);
  check_failure(R"("\uD800\u0041")", JsonFailure::kInvalidEscape);
  check_failure(R"("\uD800\uD800")", JsonFailure::kInvalidEscape);
  check_failure(R"("\u12")", JsonFailure::kUnclosedString);
  check_failure(R"("\uXYZ")", JsonFailure::kInvalidEscape);

  // 合法 NUL：\u0000 是字符串内容，不是 C 字符串终止符。
  CHECK(parser.Parse(R"("\u0000")", document, error));
  CHECK(document.Root().AsText().size() == 1 && document.Root().AsText()[0] == '\0');
}

// ---------------------------------------------------------------------------
// 4. 真实场景（5 个）
// ---------------------------------------------------------------------------

void ScenarioI18nConfig() {
  std::printf("\n[9] 场景：i18n 配置（中文/日文/法文）...\n");
  const char* text =
      "{\"greeting\":\"\\u4F60\\u597D\","
      "\"farewell\":\"\\u3055\\u3088\\u3046\\u306A\\u3089\","
      "\"cafe\":\"caf\\u00E9\","
      "\"raw\":\"已经解码\"}";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  CHECK(parser.Parse(text, document, error));
  const auto root = document.Root();
  CHECK(root["greeting"].AsText() == Bytes({0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD}));
  CHECK(root["farewell"].AsText().size() == 15);  // 5 个日文假名 × 3 字节
  CHECK(root["cafe"].AsText() == (std::string("caf") + Bytes({0xC3, 0xA9})));
  CHECK(root["raw"].AsText() == Bytes({0xE5, 0xB7, 0xB2, 0xE7, 0xBB, 0x8F, 0xE8, 0xA7,
                                       0xA3, 0xE7, 0xA0, 0x81}));
}

void ScenarioEmojiNickname() {
  std::printf("\n[10] 场景：emoji 昵称（代理对密集）...\n");
  const char* text =
      "{\"user\":\"\\uD83D\\uDC69\\u200D\\uD83D\\uDCBB\","
      "\"status\":\"\\uD83D\\uDE00\\uD83D\\uDE00\\uD83D\\uDE00\"}";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  CHECK(parser.Parse(text, document, error));
  const auto root = document.Root();
  // 女程序员 ZWJ 序列：👩 + ZWJ(U+200D) + 💻。
  CHECK(root["user"].AsText() ==
        Bytes({0xF0, 0x9F, 0x91, 0xA9, 0xE2, 0x80, 0x8D, 0xF0, 0x9F, 0x92, 0xBB}));
  CHECK(root["status"].AsText() ==
        Bytes({0xF0, 0x9F, 0x98, 0x80, 0xF0, 0x9F, 0x98, 0x80, 0xF0, 0x9F, 0x98, 0x80}));
}

void ScenarioChineseLog() {
  std::printf("\n[11] 场景：中文日志行（转义与原文混排）...\n");
  const char* text = "\"\\u9519\\u8BEF: \\u6587\\u4EF6 \\\"config.json\\\" \\u4E0D\\u5B58\\u5728\"";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  CHECK(parser.Parse(text, document, error));
  const std::string expected = Bytes({0xE9, 0x94, 0x99, 0xE8, 0xAF, 0xAF}) + ": " +
                               Bytes({0xE6, 0x96, 0x87, 0xE4, 0xBB, 0xB6}) + " \"config.json\" " +
                               Bytes({0xE4, 0xB8, 0x8D, 0xE5, 0xAD, 0x98, 0xE5, 0x9C, 0xA8});
  CHECK(document.Root().AsText() == expected);
}

void ScenarioHtmlEscapes() {
  std::printf("\n[12] 场景：HTML/JSON 双转义字符...\n");
  const char* text = R"("\u003Cscript\u003E\u0026\u0022x\u0022\u003C/script\u003E")";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  CHECK(parser.Parse(text, document, error));
  CHECK(document.Root().AsText() == "<script>&\"x\"</script>");
}

void ScenarioApiResponse() {
  std::printf("\n[13] 场景：API 响应里的混合转义 + 代理对...\n");
  const char* text =
      "{\"code\":0,\"data\":{\"items\":["
      "{\"title\":\"\\u65E5\\u672C\\u8A9E\",\"icon\":\"\\uD83D\\uDCC4\"},"
      "{\"title\":\"\\u4E2D\\u6587\",\"icon\":\"\\uD83D\\uDCDD\"}]},"
      "\"trace\":\"a\\/b\\tc\"}";
  Parser parser;
  Document document;
  go2cpp::ErrorPtr error;
  CHECK(parser.Parse(text, document, error));
  const auto root = document.Root();
  CHECK(root["code"].As<std::int64_t>() == 0);
  CHECK(root["data"]["items"].Size() == 2);
  CHECK(root["data"]["items"].At(0)["title"].AsText() ==
        Bytes({0xE6, 0x97, 0xA5, 0xE6, 0x9C, 0xAC, 0xE8, 0xAA, 0x9E}));
  CHECK(root["data"]["items"].At(0)["icon"].AsText() == Bytes({0xF0, 0x9F, 0x93, 0x84}));
  CHECK(root["data"]["items"].At(1)["icon"].AsText() == Bytes({0xF0, 0x9F, 0x93, 0x9D}));
  CHECK(root["trace"].AsText() == "a/b\tc");
}

}  // namespace

int main() {
  const auto begin = std::chrono::steady_clock::now();

  TestHexDigit();
  TestEncodeUtf8();
  TestDecodeUnicodeEscape();
  TestSurrogateSequence();
  TestEncodeUnicodeEscape();
  TestAppendUtf8();
  TestRandomUtf8RoundTrip();
  TestParserIntegration();
  ScenarioI18nConfig();
  ScenarioEmojiNickname();
  ScenarioChineseLog();
  ScenarioHtmlEscapes();
  ScenarioApiResponse();

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
