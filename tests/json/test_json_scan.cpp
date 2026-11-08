/**
 * @file test_json_scan.cpp
 * @brief 结构扫描测试：AVX2 与标量实现逐字节对照 + 块边界 + 速度。
 */

#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/json/scan.hpp"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                             \
    }                                                                           \
  } while (false)

using go2cpp::json::detail::HasAvx2;
using go2cpp::json::detail::ScanAvx2;
using go2cpp::json::detail::ScanResult;
using go2cpp::json::detail::ScanScalar;

bool SameResult(const ScanResult& lhs, const ScanResult& rhs) {
  return lhs.ok == rhs.ok && lhs.error_offset == rhs.error_offset &&
         lhs.structural == rhs.structural && lhs.strings == rhs.strings;
}

/// @brief 逐字节对照：AVX2 与标量必须完全一致。
void Compare(std::string_view input) {
  ScanResult scalar;
  ScanScalar(input, scalar);
  ScanResult avx2;
  if (!ScanAvx2(input, avx2)) {
    return;  // 无 AVX2，跳过
  }
  if (!SameResult(scalar, avx2)) {
    ++g_failures;
    std::fprintf(stderr, "MISMATCH input=%.80s (size=%zu) scalar_ok=%d avx_ok=%d\n",
                 input.data(), input.size(), scalar.ok ? 1 : 0, avx2.ok ? 1 : 0);
  }
}

/// @brief 1. 固定用例（块边界 / 转义 / 未闭合 / 控制字符）。
void TestCases() {
  std::printf("\n[1] 固定用例...\n");
  Compare("");
  Compare("{}");
  Compare("[]");
  Compare(R"({"a":1})");
  Compare(R"({"a":"x","b":[1,2]})");
  Compare(R"({"a":"x\"y"})");          // 转义引号
  Compare(R"({"a":"x\\"})");           // 转义反斜杠
  Compare(R"({"a":"unclosed)");         // 未闭合
  Compare(std::string(31, ' '));       // 块边界前
  Compare(std::string(32, ' '));       // 块边界
  Compare(std::string(33, ' '));       // 块边界后
  Compare(std::string(31, ' ') + "\"x\"");   // 字符串跨块
  Compare(std::string(40, ' ') + R"({"a":1})");
  Compare(std::string(1, '\x01'));     // 字符串外控制字符：标量不报错（只处理字符串内）
  std::string with_control = R"({"a":"x)";
  with_control.push_back('\x01');
  with_control += R"("})";
  Compare(with_control);
}

/// @brief 2. 随机输入对照（含引号/反斜杠/结构字符/空白）。
void TestRandom() {
  std::printf("\n[2] 随机对照...\n");
  constexpr int kRounds = 20000;
  std::mt19937 rng(20261106u);
  const char alphabet[] = {'"', '\\', '{', '}', '[', ']', ':', ',', 'a', '1', ' ', '\n', '\t'};
  std::uniform_int_distribution<int> length_dist(0, 120);
  std::uniform_int_distribution<int> char_dist(0, static_cast<int>(sizeof(alphabet)) - 1);

  for (int round = 0; round < kRounds; ++round) {
    const int length = length_dist(rng);
    std::string input;
    input.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
      input.push_back(alphabet[char_dist(rng)]);
    }
    Compare(input);
  }
  std::printf("  %d 条随机输入\n", kRounds);
}

/// @brief 3. 速度：100 KB 文档，AVX2 vs 标量。
void TestSpeed() {
  std::printf("\n[3] 扫描速度（100 KB）...\n");
  std::string document;
  document.reserve(120000);
  for (int i = 0; i < 4000; ++i) {
    document += R"({"field_name_)" + std::to_string(i) + R"(":"value_text_)" +
                std::to_string(i) + R"(","n":123,"f":3.14},)";
  }

  const auto measure = [&](auto&& scan) {
    const auto start = std::chrono::steady_clock::now();
    ScanResult result;
    scan(document, result);
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(end - start).count();
  };

  const double scalar_us = measure(ScanScalar);
  ScanResult avx2_result;
  const double avx2_us = measure([&](std::string_view input, ScanResult& out) {
    ScanAvx2(input, out);
  });
  std::printf("  scalar=%.1f us  avx2=%.1f us  speedup=%.2fx  bytes=%zu\n", scalar_us, avx2_us,
              scalar_us / avx2_us, document.size());
  CHECK(avx2_us < scalar_us);
}

}  // namespace

int main() {
  std::printf("=== 结构扫描测试（AVX2=%d）===\n", HasAvx2() ? 1 : 0);
  TestCases();
  TestRandom();
  TestSpeed();

  if (g_failures == 0) {
    std::printf("\nALL TESTS PASSED\n");
    return 0;
  }
  std::fprintf(stderr, "\n%d check(s) failed\n", g_failures);
  return 1;
}
