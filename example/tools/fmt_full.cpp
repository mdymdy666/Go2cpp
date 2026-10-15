/**
 * @file fmt_full.cpp
 * @brief `go2cpp/fmt.hpp` 全面版：六个场景覆盖格式化、转义、错误、对齐、指标与规模。
 *
 * @details 场景：
 *   1 日志行构造（宽度对齐）        4 表格对齐输出（左/右对齐与精度）
 *   2 SQL 拼接与 %q 转义            5 指标与百分号（%v 最短浮点、%% 转义）
 *   3 错误包装链（单/多 %w）        6 结构化展示（容器/pair/ErrorPtr）+ 10 万次规模（计时仅供参考）
 *
 * @note 计时数字只用于确认没有爆炸性劣化；性能结论必须用 Release 基准（build-bench）测量。
 * @return 全部场景通过返回 0，否则返回 1。
 */
#include "go2cpp/fmt.hpp"
#include "go2cpp/runtime.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

bool scenario_log_line() {
    std::clog << "[full][场景1] 日志行构造（宽度对齐）\n";
    using namespace go2cpp;
    const std::string line =
        Sprintf("%s | %-5s | %-20s | %d ms", "2026-10-15T18:40:00Z", "WARN", "disk usage 91%", 12);
    const bool ok = line == "2026-10-15T18:40:00Z | WARN  | disk usage 91%       | 12 ms" &&
                    Sprintf("[%5d][%-5d][%05d]", 42, 42, -42) == "[   42][42   ][-0042]";
    std::cout << "  " << line << "\n  [%5d][%-5d][%05d] → "
              << Sprintf("[%5d][%-5d][%05d]", 42, 42, -42) << "（预期 [   42][42   ][-0042]）\n";
    return ok;
}

bool scenario_sql_build() {
    std::clog << "[full][场景2] SQL 拼接与 %q 转义\n";
    using namespace go2cpp;
    const std::string statement =
        Sprintf("SELECT id, name FROM %s WHERE age > %d AND name != %q", "users", 30, "O'Brien");
    const bool ok = statement == "SELECT id, name FROM users WHERE age > 30 AND name != \"O'Brien\"" &&
                    Sprintf("%x", "AB") == "4142" &&
                    Sprintf("% X", "AB") == "41 42";
    std::cout << "  " << statement << "\n  逐字节 hex=" << Sprintf("% X", "AB")
              << "（预期 41 42）\n";
    return ok;
}

bool scenario_error_chain() {
    std::clog << "[full][场景3] 错误包装链（单/多 %w）\n";
    using namespace go2cpp;
    const ErrorPtr root = NewError("connection refused");
    const ErrorPtr timeout = NewError("timeout");
    const ErrorPtr wrapped = Errorf("dial %s: %w", "db:5432", root);
    const ErrorPtr both = Errorf("both failed: %w / %w", root, timeout);
    const ErrorPtr plain = Errorf("retry=%d after %s", 3, "2s");
    const bool ok = Is(wrapped, root) && ErrorMessage(wrapped) ==
                                            "dial db:5432: connection refused: connection refused" &&
                    Is(both, root) && Is(both, timeout) && !Unwrap(plain) &&
                    ErrorMessage(plain) == "retry=3 after 2s";
    std::cout << "  单 %w 可判定根因=" << Is(wrapped, root) << " 多 %w 同时可判定="
              << (Is(both, root) && Is(both, timeout)) << " 无 %w 时无包装=" << !Unwrap(plain)
              << "（预期 1 1 1）\n";
    return ok;
}

bool scenario_table_align() {
    std::clog << "[full][场景4] 表格对齐输出\n";
    using namespace go2cpp;
    const std::string header = Sprintf("%-10s%8s%12s", "name", "count", "ratio");
    const std::string row = Sprintf("%-10s%8d%12.2f", "requests", 12345, 0.9876);
    const bool ok = header == "name         count       ratio" &&
                    row == "requests     12345        0.99" && header.size() == row.size();
    std::cout << "  [" << header << "]\n  [" << row << "]\n  列宽一致="
              << (header.size() == row.size()) << "（预期 1）\n";
    return ok;
}

bool scenario_metrics() {
    std::clog << "[full][场景5] 指标与百分号\n";
    using namespace go2cpp;
    const bool ok = Sprintf("cpu=%.1f%% mem=%d%%", 91.25, 64) == "cpu=91.2% mem=64%" &&
                    Sprintf("%v", 3.14) == "3.14" &&
                    Sprintf("%v", 0.1 + 0.2) == "0.30000000000000004" &&
                    Sprintf("%v", 1000000.0) == "1e+06" && Sprintf("100%%") == "100%";
    std::cout << "  " << Sprintf("cpu=%.1f%% mem=%d%%", 91.25, 64) << "\n  %v 浮点最短表示：3.14→"
              << Sprintf("%v", 3.14) << "、0.1+0.2→" << Sprintf("%v", 0.1 + 0.2) << "、1e6→"
              << Sprintf("%v", 1000000.0) << "\n";
    return ok;
}

bool scenario_structured_and_scale() {
    std::clog << "[full][场景6] 结构化展示 + 10 万次规模\n";
    using namespace go2cpp;
    const ErrorPtr failure = NewError("boom");
    const bool structured_ok =
        Sprintf("%v", std::vector<int>{1, 2, 3}) == "[1 2 3]" &&
        Sprintf("%v", std::vector<std::string>{"a", "b"}) == "[a b]" &&
        Sprintf("%q", std::vector<std::string>{"a", "b"}) == "[\"a\" \"b\"]" &&
        Sprintf("%v", std::make_pair(7, std::string("x"))) == "(7, x)" &&
        Sprintf("%v", failure) == "boom" && Sprintf("%T", failure) == "error" &&
        Sprintf("%d", "abc") == "%!d(string=abc)";        // 类型不匹配的标记形式

    constexpr std::size_t kIterations = 100000;
    const auto started = Clock::now();
    std::size_t total = 0;
    for (std::size_t i = 0; i < kIterations; ++i) {
        total += Sprintf("%-8s %6d %8.3f", "req", static_cast<int>(i % 10000),
                         static_cast<double>(i % 1000) / 8.0).size();
    }
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();

    const bool ok = structured_ok && total > 0;
    std::cout << "  容器/pair/错误=" << structured_ok << " " << kIterations
              << " 次格式化总字节=" << total << " 耗时=" << elapsed_us
              << " µs（Debug 计时仅供参考）\n";
    return ok;
}

}  // namespace

/**
 * @brief fmt 全面版：六个场景。
 * @return 全部通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_log_line() && ok;
    ok = scenario_sql_build() && ok;
    ok = scenario_error_chain() && ok;
    ok = scenario_table_align() && ok;
    ok = scenario_metrics() && ok;
    ok = scenario_structured_and_scale() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
