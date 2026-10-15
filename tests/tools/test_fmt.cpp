/**
 * @file test_fmt.cpp
 * @brief `go2cpp/fmt.hpp` 的正式测试：Go 语义边界 + 大数据量 + 随机对拍 + 真实场景。
 *
 * 测试设计（按项目规范：大数据量 / 随机 / ≥5 真实场景）：
 *   场景 1 日志行构造（多字段 + 宽度对齐）      场景 4 表格对齐输出（左/右对齐 + 精度）
 *   场景 2 SQL 语句拼接（%s 表名 + %d 参数 + %q）场景 5 指标与百分号（%v 最短浮点 + %% 转义）
 *   场景 3 错误包装链（Errorf %w + Is + 多 %w）  场景 6 结构化展示（vector / pair 递归）
 *   大数据量：10 万次带格式化的日志构造 + 10 万次错误构造（并验证引用计数不泄漏）
 *   随机：随机值 × 6 种格式模板（输出不得含未预期标记）、
 *         随机 int64 的 `%d` → `strconv::Atoi` 往返、随机 double 的 `%v` → `ParseFloat` 位相等往返
 *
 * ⚠️ 注释中不得出现连续的斜杠加星号（GCC -Wcomment）。
 */
#include "go2cpp/fmt.hpp"
#include "go2cpp/strconv.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using go2cpp::ErrorPtr;
using go2cpp::NewError;

/// 输出里是否含 Go 的错误标记（`%!`）——用于随机对拍时判断"出现了未预期的类型不匹配"。
bool HasErrorMarker(const std::string& text) { return text.find("%!") != std::string::npos; }

}  // namespace

/**
 * @brief 运行 fmt 模块的全部测试。
 */
void run_fmt_tests() {
    go2cpp_tests::announce("fmt: Go 语义边界 + 大数据量 + 随机对拍 + 真实场景");
    using namespace go2cpp;

    // ---------------- 场景 1：日志行构造（多字段 + 宽度对齐） ----------------
    {
        const std::string line =
            Sprintf("%s | %-5s | %-20s | %d ms", "2026-10-15T18:40:00Z", "WARN", "disk usage 91%", 12);
        GO2CPP_CHECK(line == "2026-10-15T18:40:00Z | WARN  | disk usage 91%       | 12 ms");
        GO2CPP_CHECK(Sprintf("[%5d]", 42) == "[   42]");
        GO2CPP_CHECK(Sprintf("[%-5d]", 42) == "[42   ]");
        GO2CPP_CHECK(Sprintf("[%05d]", 42) == "[00042]");
        GO2CPP_CHECK(Sprintf("[%05d]", -42) == "[-0042]");     // 零填充在符号之后
    }

    // ---------------- 场景 2：SQL 语句拼接 ----------------
    {
        const std::string table = "users";
        const std::string statement =
            Sprintf("SELECT id, name FROM %s WHERE age > %d AND name != %q", table, 30, "O'Brien");
        GO2CPP_CHECK(statement ==
                     "SELECT id, name FROM users WHERE age > 30 AND name != \"O'Brien\"");
        // 危险输入被 %q 转义（引号与换行都变可见）
        const std::string escaped = Sprintf("%q", std::string("a\nb\"c"));
        GO2CPP_CHECK(escaped == "\"a\\nb\\\"c\"");
    }

    // ---------------- 场景 3：错误包装链 ----------------
    {
        const ErrorPtr root = NewError("connection refused");
        const ErrorPtr wrapped = Errorf("dial %s: %w", "db:5432", root);
        GO2CPP_CHECK(Is(wrapped, root));
        GO2CPP_CHECK(ErrorMessage(wrapped) == "dial db:5432: connection refused: connection refused");
        const ErrorPtr other = NewError("timeout");
        const ErrorPtr both = Errorf("both failed: %w / %w", root, other);
        GO2CPP_CHECK(Is(both, root) && Is(both, other));
        const ErrorPtr plain = Errorf("retry=%d after %s", 3, "2s");
        GO2CPP_CHECK(!Unwrap(plain) && ErrorMessage(plain) == "retry=3 after 2s");
    }

    // ---------------- 场景 4：表格对齐输出 ----------------
    {
        const std::string header = Sprintf("%-10s%8s%12s", "name", "count", "ratio");
        const std::string row = Sprintf("%-10s%8d%12.2f", "requests", 12345, 0.9876);
        GO2CPP_CHECK(header == "name         count       ratio");
        GO2CPP_CHECK(row == "requests     12345        0.99");
        GO2CPP_CHECK(header.size() == row.size());              // 列宽一致
    }

    // ---------------- 场景 5：指标与百分号 ----------------
    {
        GO2CPP_CHECK(Sprintf("cpu=%.1f%% mem=%d%%", 91.25, 64) == "cpu=91.2% mem=64%");
        GO2CPP_CHECK(Sprintf("%v", 3.14) == "3.14");            // %v 对浮点用最短表示
        GO2CPP_CHECK(Sprintf("%v", 0.1 + 0.2) == "0.30000000000000004");
        GO2CPP_CHECK(Sprintf("100%%") == "100%");
    }

    // ---------------- 场景 6：结构化展示（容器与 pair 递归） ----------------
    {
        GO2CPP_CHECK(Sprintf("%v", std::vector<int>{1, 2, 3}) == "[1 2 3]");
        GO2CPP_CHECK(Sprintf("%v", std::vector<std::string>{"a", "b"}) == "[a b]");
        GO2CPP_CHECK(Sprintf("%q", std::vector<std::string>{"a", "b"}) == "[\"a\" \"b\"]");
        GO2CPP_CHECK(Sprintf("%v", std::make_pair(7, std::string("x"))) == "(7, x)");
        const ErrorPtr failure = NewError("boom");
        GO2CPP_CHECK(Sprintf("%v", failure) == "boom");
        GO2CPP_CHECK(Sprintf("%T", failure) == "error");
    }

    // ---------------- 语义边界：错误标记与类型不匹配 ----------------
    {
        GO2CPP_CHECK(Sprintf("%d", "abc") == "%!d(string=abc)");
        GO2CPP_CHECK(Sprintf("%d %d", 1) == "1 %!d(MISSING)");
        GO2CPP_CHECK(Sprintf("%d", 1, 2) == "1%!(EXTRA int=2)");
        GO2CPP_CHECK(Sprintf("%t %v", true, false) == "true false");
        GO2CPP_CHECK(Sprintf("%c", 0x4E2D) == "中");
        GO2CPP_CHECK(Sprintf("%x %X %#x", 255, 255, 255) == "ff FF 0xff");
        GO2CPP_CHECK(Sprintf("%b %#b %o %#o", 5, 5, 8, 8) == "101 0b101 10 010");
        GO2CPP_CHECK(Sprintf("%.2s", "abcdef") == "ab");        // precision 按 rune 截断
        GO2CPP_CHECK(Sprintf("%.2s", "中文测试") == "中文");     // 多字节按 rune 而非字节
        GO2CPP_CHECK(Sprintf("%T", static_cast<std::int64_t>(1)) == "int64");
        GO2CPP_CHECK(Sprintf("%T", 1) == "int");
        GO2CPP_CHECK(Sprintf("%q", 65) == "'A'");
        GO2CPP_CHECK(Sprint(1, "a", 2.5) == "1a2.5");
        GO2CPP_CHECK(Sprintln(1, 2) == "1 2\n");
        bool threw = false;
        try {
            (void)Sprintf("%*d", 5, 42);                        // '*' 宽度明确拒绝
        } catch (const std::logic_error&) {
            threw = true;
        }
        GO2CPP_CHECK(threw);
    }

    // ---------------- 大数据量 1：10 万次日志构造 ----------------
    {
        constexpr std::size_t kIterations = 100000;
        std::size_t total = 0;
        for (std::size_t i = 0; i < kIterations; ++i) {
            const std::string line = Sprintf("%-8s %6d %8.3f", "req", static_cast<int>(i % 10000),
                                             static_cast<double>(i % 1000) / 8.0);
            total += line.size();
        }
        GO2CPP_CHECK(total > 0);
        GO2CPP_CHECK(Sprintf("%-8s %6d %8.3f", "req", 7, 0.125) == "req           7    0.125");
    }

    // ---------------- 大数据量 2：10 万次错误构造（不泄漏、身份可判定） ----------------
    {
        constexpr std::size_t kIterations = 100000;
        const ErrorPtr root = NewError("root cause");
        const long baseline = root.use_count();
        std::size_t matched = 0;
        for (std::size_t i = 0; i < kIterations; ++i) {
            const ErrorPtr wrapped = Errorf("op %d failed: %w", static_cast<int>(i), root);
            if (Is(wrapped, root)) ++matched;
        }
        GO2CPP_CHECK(matched == kIterations);
        GO2CPP_CHECK(root.use_count() == baseline);             // 循环内的临时错误已释放
    }

    // ---------------- 随机 1：随机值 × 6 种格式模板（不得出现未预期标记） ----------------
    {
        std::mt19937_64 rng(20261015ULL);
        std::uniform_int_distribution<std::int64_t> int_dist(-1000000, 1000000);
        std::uniform_real_distribution<double> real_dist(-1e6, 1e6);
        std::int64_t unexpected = 0;
        for (int iteration = 0; iteration < 20000; ++iteration) {
            const std::int64_t number = int_dist(rng);
            const double real = real_dist(rng);
            std::string text;
            switch (iteration % 6) {
                case 0: text = Sprintf("%d", number); break;
                case 1: text = Sprintf("%08d", number); break;
                case 2: text = Sprintf("%x", static_cast<std::uint64_t>(number)); break;
                case 3: text = Sprintf("%.4f", real); break;
                case 4: text = Sprintf("%v", real); break;
                default: text = Sprintf("%s=%d (%v)", "metric", number, real); break;
            }
            if (HasErrorMarker(text)) ++unexpected;
        }
        GO2CPP_CHECK(unexpected == 0);
    }

    // ---------------- 随机 2：随机 int64 的 %d → Atoi 往返 ----------------
    {
        std::mt19937_64 rng(4242ULL);
        std::uniform_int_distribution<std::int64_t> dist(
            std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max());
        std::int64_t mismatches = 0;
        for (int iteration = 0; iteration < 100000; ++iteration) {
            const std::int64_t value = dist(rng);
            if (Atoi(Sprintf("%d", value)).value != value) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
    }

    // ---------------- 随机 3：随机 double 的 %v → ParseFloat 位相等往返 ----------------
    {
        std::mt19937_64 rng(31337ULL);
        std::uniform_real_distribution<double> dist(-1e300, 1e300);
        std::int64_t mismatches = 0;
        for (int iteration = 0; iteration < 100000; ++iteration) {
            const double value = dist(rng);
            if (!std::isfinite(value) || value == 0.0) continue;
            const StrconvFloatResult back = ParseFloat(Sprintf("%v", value), 64);
            if (!back.ok()) {
                ++mismatches;
                continue;
            }
            if (std::memcmp(&back.value, &value, sizeof(double)) != 0) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
    }
}
