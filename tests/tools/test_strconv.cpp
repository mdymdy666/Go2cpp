/**
 * @file test_strconv.cpp
 * @brief `go2cpp/strconv.hpp` 的正式测试：Go 语义边界 + 大数据量 + 随机往返 + 真实场景。
 *
 * 测试设计（按项目规范：大数据量 / 随机 / ≥5 真实场景）：
 *   场景 1 配置值解析（Atoi + ParseBool + 错误**分类**）      场景 4 十六进制 ID 转换
 *   场景 2 指标/日志格式化（Itoa/FormatInt/FormatUint）       场景 5 Unix 权限位（八进制推断）
 *   场景 3 JSON 数值往返（FormatFloat('g',-1) → ParseFloat）  场景 6 范围与钳制值语义
 *   大数据量：20 万次 Itoa→Atoi 往返 + 20 万次 FormatUint→ParseUint（随机 base）
 *   随机：int64 往返、uint64×base(2..36) 往返、double 最短表示往返、
 *         随机字符串与 `std::from_chars` 的语法判定对拍
 *
 * ⚠️ 注释中不得出现连续的斜杠加星号（GCC -Wcomment）。
 */
#include "go2cpp/strconv.hpp"
#include "test_support.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>

namespace {

/// 独立参考实现：用 C++ 标准库判断一个十进制串能否被完整解析（用于语法判定对拍）。
bool ReferenceParsesSignedDecimal(std::string_view text) {
    if (text.empty()) return false;
    // 与 Go 的 Atoi 对齐：至多一个前导符号，其余必须全是十进制数字。
    // 不能把整串直接交给 std::from_chars —— 它既接受不了前导 '+'，
    // 又会在剥离 '+' 之后把 "+-5" 误判为合法负数。
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '+') {
        index = 1;
    } else if (text[0] == '-') {
        index = 1;
        negative = true;
    }
    if (index >= text.size()) return false;
    for (std::size_t i = index; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
    }
    const std::string_view digits = text.substr(index);
    std::uint64_t magnitude = 0;
    const auto parsed =
        std::from_chars(digits.data(), digits.data() + digits.size(), magnitude, 10);
    if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size()) return false;
    const std::uint64_t limit =
        negative ? 9223372036854775808ULL : 9223372036854775807ULL;
    return magnitude <= limit;
}

}  // namespace

/**
 * @brief 运行 strconv 模块的全部测试。
 */
void run_strconv_tests() {
    go2cpp_tests::announce("strconv: Go 语义边界 + 大数据量 + 随机往返 + 真实场景");
    using namespace go2cpp;

    // ---------------- 场景 1：配置值解析与错误分类 ----------------
    {
        const StrconvIntResult retry = Atoi("3");
        GO2CPP_CHECK(retry.ok() && retry.value == 3);

        const StrconvIntResult broken = Atoi("3x");
        GO2CPP_CHECK(!broken.ok());
        GO2CPP_CHECK(Is(broken.error, StrconvErrSyntax()));      // 语法错误按身份判定

        const StrconvBoolResult enabled = ParseBool("TRUE");
        GO2CPP_CHECK(enabled.ok() && enabled.value);
        GO2CPP_CHECK(!ParseBool("yes").ok());
        GO2CPP_CHECK(Is(ParseBool("yes").error, StrconvErrSyntax()));

        // 配置里常见：数值 + 单位后缀 → 必须先 Trim 再解析（体现 Atoi 不做宽容解析）
        GO2CPP_CHECK(!Atoi("30s").ok());
    }

    // ---------------- 场景 2：指标与日志格式化 ----------------
    {
        std::string line = "requests=";
        line += Itoa(1234567);
        line += " errors=";
        line += FormatUint(42U, 10);
        line += " mask=0x";
        line += FormatUint(255U, 16);
        GO2CPP_CHECK(line == "requests=1234567 errors=42 mask=0xff");
        GO2CPP_CHECK(FormatInt(-255, 16) == "-ff");
        GO2CPP_CHECK(FormatInt(0, 2) == "0");
        GO2CPP_CHECK(FormatBool(false) == "false");
    }

    // ---------------- 场景 3：JSON 数值往返（最短表示） ----------------
    {
        const double values[] = {3.14, 0.1, 1.0 / 3.0, 1e-300, 1e300, -2.5e-10};
        std::int64_t mismatches = 0;
        for (const double value : values) {
            const std::string text = FormatFloat(value, 'g', -1, 64);
            const StrconvFloatResult back = ParseFloat(text, 64);
            if (!back.ok() || back.value != value) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
        GO2CPP_CHECK(FormatFloat(3.14, 'g', -1, 64) == "3.14");
        GO2CPP_CHECK(FormatFloat(0.1, 'f', 2, 64) == "0.10");
        GO2CPP_CHECK(FormatFloat(1e20, 'e', 2, 64) == "1.00e+20");
    }

    // ---------------- 场景 4：十六进制 ID 转换 ----------------
    {
        const std::uint64_t id = 0xDEADBEEFCAFEULL;
        const std::string hex = FormatUint(id, 16);
        GO2CPP_CHECK(hex == "deadbeefcafe");
        const StrconvUintResult back = ParseUint(hex, 16, 64);
        GO2CPP_CHECK(back.ok() && back.value == id);
        // 带 0x 前缀的自动推断
        const StrconvUintResult prefixed = ParseUint("0xDEADBEEF", 0, 64);
        GO2CPP_CHECK(prefixed.ok() && prefixed.value == 0xDEADBEEFULL);
    }

    // ---------------- 场景 5：Unix 权限位（前导 0 = 八进制） ----------------
    {
        const StrconvIntResult mode = ParseInt("0755", 0, 64);
        GO2CPP_CHECK(mode.ok() && mode.value == 493);
        GO2CPP_CHECK(FormatInt(mode.value, 8) == "755");
        const StrconvIntResult masked = ParseInt("0o644", 0, 64);
        GO2CPP_CHECK(masked.ok() && masked.value == 420);
    }

    // ---------------- 场景 6：范围与钳制值 ----------------
    {
        const StrconvIntResult overflow = ParseInt("999", 10, 8);
        GO2CPP_CHECK(overflow.value == 127);
        GO2CPP_CHECK(Is(overflow.error, StrconvErrRange()));     // 范围错误与语法错误必须可区分
        GO2CPP_CHECK(!Is(overflow.error, StrconvErrSyntax()));

        const StrconvUintResult underflow = ParseUint("-1", 10, 64);
        GO2CPP_CHECK(!underflow.ok() && Is(underflow.error, StrconvErrSyntax()));
        GO2CPP_CHECK(ParseInt("-128", 10, 8).ok());
        GO2CPP_CHECK(ParseInt("-129", 10, 8).value == -128);
        GO2CPP_CHECK(ParseUint("18446744073709551615", 10, 64).value ==
                     std::numeric_limits<std::uint64_t>::max());
    }

    // ---------------- 边界：下划线、特殊浮点、Quote ----------------
    {
        GO2CPP_CHECK(ParseInt("1_000", 0, 64).value == 1000);
        GO2CPP_CHECK(!ParseInt("1_0", 10, 64).ok());
        GO2CPP_CHECK(!ParseInt("1_", 0, 64).ok());
        GO2CPP_CHECK(std::isinf(ParseFloat("Infinity", 64).value));
        GO2CPP_CHECK(std::isnan(ParseFloat("NaN", 64).value));
        GO2CPP_CHECK(ParseFloat("0x1p4", 64).value == 16.0);
        GO2CPP_CHECK(!ParseFloat(" 1", 64).ok());
        GO2CPP_CHECK(Quote("a\"b\n") == "\"a\\\"b\\n\"");
        GO2CPP_CHECK(FormatInt(std::numeric_limits<std::int64_t>::min(), 10) ==
                     "-9223372036854775808");
    }

    // ---------------- 大数据量 1：20 万次 Itoa → Atoi 往返 ----------------
    {
        constexpr std::size_t kIterations = 200000;
        std::mt19937_64 rng(20261015ULL);
        std::uniform_int_distribution<std::int64_t> value_dist(
            std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max());
        std::int64_t mismatches = 0;
        for (std::size_t i = 0; i < kIterations; ++i) {
            const std::int64_t value = value_dist(rng);
            const std::string text = Itoa(value);
            const StrconvIntResult back = Atoi(text);
            if (!back.ok() || back.value != value) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
    }

    // ---------------- 大数据量 2：20 万次 FormatUint → ParseUint（随机 base） ----------------
    {
        constexpr std::size_t kIterations = 200000;
        std::mt19937_64 rng(777ULL);
        std::uniform_int_distribution<std::uint64_t> value_dist(0, std::numeric_limits<std::uint64_t>::max());
        std::uniform_int_distribution<int> base_dist(2, 36);
        std::int64_t mismatches = 0;
        for (std::size_t i = 0; i < kIterations; ++i) {
            const std::uint64_t value = value_dist(rng);
            const int base = base_dist(rng);
            const std::string text = FormatUint(value, base);
            const StrconvUintResult back = ParseUint(text, base, 64);
            if (!back.ok() || back.value != value) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
    }

    // ---------------- 随机 1：double 最短表示往返（位相等） ----------------
    {
        constexpr std::size_t kIterations = 100000;
        std::mt19937_64 rng(31337ULL);
        std::uniform_real_distribution<double> dist(-1e300, 1e300);
        std::int64_t mismatches = 0;
        for (std::size_t i = 0; i < kIterations; ++i) {
            const double value = dist(rng);
            if (!std::isfinite(value) || value == 0.0) continue;   // 跳过 0 与特殊值（另有边界用例）
            const std::string text = FormatFloat(value, 'g', -1, 64);
            const StrconvFloatResult back = ParseFloat(text, 64);
            if (!back.ok()) {
                ++mismatches;
                continue;
            }
            if (std::memcmp(&back.value, &value, sizeof(double)) != 0) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
    }

    // ---------------- 随机 2：随机字符串的语法判定与标准库对拍 ----------------
    {
        constexpr std::size_t kIterations = 100000;
        std::mt19937 rng(2026U);
        std::uniform_int_distribution<int> char_dist(0, 25);
        std::uniform_int_distribution<int> length_dist(0, 12);
        std::int64_t mismatches = 0;
        for (std::size_t i = 0; i < kIterations; ++i) {
            std::string text;
            const int length = length_dist(rng);
            for (int c = 0; c < length; ++c) {
                // 混合数字、字母与符号，确保既有合法也有非法输入
                const int bucket = char_dist(rng);
                if (bucket < 12) text.push_back(static_cast<char>('0' + (bucket % 10)));
                else if (bucket < 20) text.push_back(static_cast<char>('a' + bucket - 12));
                else text.push_back(bucket % 2 == 0 ? '+' : '-');
            }
            const bool ours = Atoi(text).ok();
            const bool reference = ReferenceParsesSignedDecimal(text);
            if (ours != reference) ++mismatches;
            // 范围错误不应被误判为语法错误
            const StrconvIntResult result = Atoi(text);
            if (!result.ok() && !Is(result.error, StrconvErrSyntax()) &&
                !Is(result.error, StrconvErrRange())) {
                ++mismatches;
            }
        }
        GO2CPP_CHECK(mismatches == 0);
    }
}
