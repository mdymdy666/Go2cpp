/**
 * @file fmt_demo.cpp
 * @brief `go2cpp/fmt.hpp` 简单版：三个最常见的使用场景。
 *
 * @details 场景：
 *          ① 日志行构造（宽度对齐）
 *          ② SQL 语句拼接与 `%q` 转义
 *          ③ 错误包装链（`Errorf` + `%w` + `Is`）
 * @return 全部校验通过返回 0，否则返回 1。
 */
#include "go2cpp/fmt.hpp"
#include "go2cpp/runtime.hpp"

#include <iostream>
#include <string>

namespace {

/// 场景 1：日志行构造。
bool scenario_log_line() {
    std::clog << "[fmt][场景1] 日志行构造（宽度对齐）\n";
    using namespace go2cpp;
    const std::string line =
        Sprintf("%s | %-5s | %-20s | %d ms", "2026-10-15T18:40:00Z", "WARN", "disk usage 91%", 12);
    const bool ok = line == "2026-10-15T18:40:00Z | WARN  | disk usage 91%       | 12 ms" &&
                    Sprintf("[%05d]", -42) == "[-0042]";       // 零填充在符号之后
    std::cout << "  " << line << "\n"
              << "  （预期 2026-10-15T18:40:00Z | WARN  | disk usage 91%       | 12 ms）\n";
    return ok;
}

/// 场景 2：SQL 语句拼接与转义。
bool scenario_sql_build() {
    std::clog << "[fmt][场景2] SQL 语句拼接与 %q 转义\n";
    using namespace go2cpp;
    const std::string statement =
        Sprintf("SELECT id FROM %s WHERE age > %d AND name != %q", "users", 30, "O'Brien");
    const std::string escaped = Sprintf("%q", std::string("a\nb\"c"));
    const bool ok = statement == "SELECT id FROM users WHERE age > 30 AND name != \"O'Brien\"" &&
                    escaped == "\"a\\nb\\\"c\"";
    std::cout << "  " << statement << "\n  转义=" << escaped
              << "（预期 \"a\\nb\\\"c\"）\n";
    return ok;
}

/// 场景 3：错误包装链。
bool scenario_error_chain() {
    std::clog << "[fmt][场景3] 错误包装链（%w）\n";
    using namespace go2cpp;
    const ErrorPtr root = NewError("connection refused");
    const ErrorPtr wrapped = Errorf("dial %s: %w", "db:5432", root);
    const bool ok = Is(wrapped, root) &&
                    ErrorMessage(wrapped) ==
                        "dial db:5432: connection refused: connection refused" &&
                    Is(wrapped, wrapped);
    std::cout << "  错误文本=" << ErrorMessage(wrapped) << "\n"
              << "  可判定为根因=" << Is(wrapped, root) << "（预期 1）\n";
    return ok;
}

}  // namespace

/**
 * @brief fmt 简单版：三个场景覆盖格式化、转义与错误包装。
 * @return 全部通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_log_line() && ok;
    ok = scenario_sql_build() && ok;
    ok = scenario_error_chain() && ok;

    std::clog << (ok ? "[fmt] 全部通过\n" : "[fmt] 有失败\n");
    return ok ? 0 : 1;
}
