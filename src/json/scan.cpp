/**
 * @file scan.cpp
 * @brief 结构扫描实现：AVX2 快路径 + 标量回退。
 */

#include "go2cpp/json/scan.hpp"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace go2cpp::json::detail {

namespace {

constexpr bool IsStructural(char value) noexcept {
  return value == '{' || value == '}' || value == '[' || value == ']' || value == ':' ||
         value == ',';
}

/// @brief 「没有待跳过的转义字符」哨兵。
constexpr std::size_t kNoPos = static_cast<std::size_t>(-1);

/// @brief 把字符串外的反斜杠视为错误（JSON 不允许）。
constexpr bool IsQuote(char value) noexcept { return value == '"'; }

}  // namespace

bool HasAvx2() noexcept {
#if defined(__AVX2__) && (defined(__GNUC__) || defined(__clang__))
  return __builtin_cpu_supports("avx2") != 0;
#elif defined(__AVX2__)
  return true;
#else
  return false;
#endif
}

void ScanScalar(std::string_view input, ScanResult& out) {
  bool in_string = false;
  bool skip_next = false;
  for (std::size_t index = 0; index < input.size(); ++index) {
    const char value = input[index];
    if (skip_next) {
      skip_next = false;
      continue;
    }
    if (in_string) {
      if (value == '\\') {
        skip_next = true;
      } else if (value == '"') {
        in_string = false;
        out.strings.push_back(static_cast<std::uint32_t>(index));
      } else if (static_cast<unsigned char>(value) < 0x20U) {
        out.ok = false;
        out.error_offset = index;
        return;
      }
      continue;
    }
    if (value == '"') {
      in_string = true;
    } else if (IsStructural(value)) {
      out.structural.push_back(static_cast<std::uint32_t>(index));
    }
  }
  if (in_string) {
    out.ok = false;
    out.error_offset = input.size();
  }
}

#if defined(__AVX2__)

bool ScanAvx2Impl(std::string_view input, ScanResult& out) {
  const char* const data = input.data();
  const std::size_t size = input.size();

  const __m256i quote_char = _mm256_set1_epi8('"');
  const __m256i backslash_char = _mm256_set1_epi8('\\');
  const __m256i lbrace = _mm256_set1_epi8('{');
  const __m256i rbrace = _mm256_set1_epi8('}');
  const __m256i lbracket = _mm256_set1_epi8('[');
  const __m256i rbracket = _mm256_set1_epi8(']');
  const __m256i colon = _mm256_set1_epi8(':');
  const __m256i comma = _mm256_set1_epi8(',');

  bool in_string = false;
  std::size_t escaped_at = kNoPos;
  std::size_t index = 0;

  for (; index + 32 <= size; index += 32) {
    const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + index));
    const __m256i structural = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(chunk, lbrace), _mm256_cmpeq_epi8(chunk, rbrace)),
        _mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(chunk, lbracket), _mm256_cmpeq_epi8(chunk, rbracket)),
            _mm256_or_si256(_mm256_cmpeq_epi8(chunk, colon), _mm256_cmpeq_epi8(chunk, comma))));

    const __m256i control =
        _mm256_cmpeq_epi8(_mm256_min_epu8(chunk, _mm256_set1_epi8(0x1F)), chunk);
    const std::uint32_t quote_mask = static_cast<std::uint32_t>(
        _mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, quote_char)));
    const std::uint32_t backslash_mask = static_cast<std::uint32_t>(
        _mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, backslash_char)));
    const std::uint32_t structural_mask =
        static_cast<std::uint32_t>(_mm256_movemask_epi8(structural));

    std::uint32_t special = quote_mask | backslash_mask | structural_mask |
                            static_cast<std::uint32_t>(_mm256_movemask_epi8(control));
    while (special != 0) {
      const int bit = __builtin_ctz(special);
      special &= special - 1;
      const std::size_t position = index + static_cast<std::size_t>(bit);
      const char value = data[position];

      if (escaped_at == position) {
        escaped_at = kNoPos;
        continue;
      }
      if (in_string) {
        if (value == '\\') {
          escaped_at = position + 1;
        } else if (value == '"') {
          in_string = false;
          out.strings.push_back(static_cast<std::uint32_t>(position));
        } else if (static_cast<unsigned char>(value) < 0x20U) {
          out.ok = false;
          out.error_offset = position;
          return true;
        }
        continue;
      }
      if (value == '"') {
        in_string = true;
      } else if (IsStructural(value)) {
        out.structural.push_back(static_cast<std::uint32_t>(position));
      }
    }
  }

  // 尾部不足 32 字节：标量收尾（保留 in_string/escaped_at 状态）。
  for (; index < size; ++index) {
    const char value = data[index];
    if (escaped_at == index) {
      escaped_at = kNoPos;
      continue;
    }
    if (in_string) {
      if (value == '\\') {
        escaped_at = index + 1;
      } else if (value == '"') {
        in_string = false;
        out.strings.push_back(static_cast<std::uint32_t>(index));
      } else if (static_cast<unsigned char>(value) < 0x20U) {
        out.ok = false;
        out.error_offset = index;
        return true;
      }
      continue;
    }
    if (value == '"') {
      in_string = true;
    } else if (IsStructural(value)) {
      out.structural.push_back(static_cast<std::uint32_t>(index));
    }
  }
  if (in_string) {
    out.ok = false;
    out.error_offset = size;
  }
  return true;
}

#endif  // __AVX2__

bool ScanAvx2(std::string_view input, ScanResult& out) {
#if defined(__AVX2__)
  if (!HasAvx2()) {
    return false;
  }
  return ScanAvx2Impl(input, out);
#else
  (void)input;
  (void)out;
  return false;
#endif
}

}  // namespace go2cpp::json::detail
