/**
 * @file strings_demo.cpp
 * @brief `go2cpp/strings.hpp` 简单版：三个最常见的使用场景。
 *
 * @details 场景：
 *          ① 日志行切分与字段清洗（Split + TrimSpace + HasPrefix）
 *          ② 拼接与替换（StringBuilder + Join + ReplaceAll）
 *          ③ 查找与判定的 Go 边界（Contains/Index/HasPrefix/Count 的空串语义）
 * @return 全部校验通过返回 0，否则返回 1。
 */
#include "go2cpp/runtime.hpp"
#include "go2cpp/strings.hpp"

#include <iostream>
#include <string>

namespace {

/// 场景 1：把一行日志切成字段并清洗。
bool scenario_split_log_line() {
    std::clog << "[strings][场景1] 日志行切分与字段清洗\n";
    using namespace go2cpp;

    const std::string line = "  2026-10-15T18:30:00Z | WARN  | disk usage 91%  ";
    const StringSlice fields = Split(line, "|");
    const std::string stamp = fields.size() > 0 ? TrimSpace(fields[0]) : std::string{};
    const std::string level = fields.size() > 1 ? TrimSpace(fields[1]) : std::string{};
    const std::string message = fields.size() > 2 ? TrimSpace(fields[2]) : std::string{};

    const bool ok = fields.size() == 3 && level == "WARN" &&
                    HasSuffix(stamp, "Z") && HasPrefix(message, "disk");
    std::cout << "  字段数=" << fields.size() << " level=\"" << level << "\" message=\""
              << message << "\"（预期 3 / WARN / disk usage 91%）\n";
    return ok;
}

/// 场景 2：拼接与替换。
bool scenario_build_and_replace() {
    std::clog << "[strings][场景2] 拼接与替换\n";
    using namespace go2cpp;

    StringBuilder sql;
    sql.WriteString("SELECT ");
    const StringSlice columns = {"id", "name", "email"};
    sql.WriteString(Join(columns, ", "));
    sql.WriteString(" FROM users WHERE status = ?");
    const std::string statement = sql.String();

    const std::string masked = ReplaceAll("token=abcdef&token=ghijkl", "token=", "token=***");
    const bool ok = statement == "SELECT id, name, email FROM users WHERE status = ?" &&
                    masked == "token=***abcdef&token=***ghijkl";
    std::cout << "  SQL=\"" << statement << "\"\n"
              << "  脱敏=\"" << masked << "\"（预期 token=***abcdef&token=***ghijkl）\n";
    return ok;
}

/// 场景 3：查找与判定的 Go 边界语义。
bool scenario_lookup_boundaries() {
    std::clog << "[strings][场景3] 查找与判定的空串边界\n";
    using namespace go2cpp;

    const bool empty_needle_ok =
        Contains("abc", "") && Index("abc", "") == 0 && !ContainsAny("abc", "") &&
        IndexAny("abc", "") == -1 && Count("abc", "") == 4;   // rune 数 + 1
    const bool normal_ok = Index("chicken", "ken") == 4 && LastIndex("go gopher", "go") == 3 &&
                           Count("cheese", "e") == 3 && HasSuffix("Gopher", "her");
    const bool n_zero_ok = SplitN("a,b,c", ",", 0).empty();   // n == 0 → 空切片
    const bool ok = empty_needle_ok && normal_ok && n_zero_ok;
    std::cout << "  空子串语义=" << empty_needle_ok << " 常规查找=" << normal_ok
              << " n==0 切分为空=" << n_zero_ok << "（预期 1 1 1）\n";
    return ok;
}

}  // namespace

/**
 * @brief strings 简单版：三个场景覆盖切分/拼接/查找。
 * @return 全部通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_split_log_line() && ok;
    ok = scenario_build_and_replace() && ok;
    ok = scenario_lookup_boundaries() && ok;

    std::clog << (ok ? "[strings] 全部通过\n" : "[strings] 有失败\n");
    return ok ? 0 : 1;
}
