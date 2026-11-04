/**
 * @file strings_full.cpp
 * @brief `go2cpp/strings.hpp` 全面版：六个场景覆盖切分、清洗、解析、比较、构建与规模。
 *
 * @details 场景：
 *   1 日志行切分与字段清洗     4 HTTP 头名大小写无关比较与排序
 *   2 CSV 字段清洗与还原       5 Builder + UTF-8（RuneCount/Count 的 rune 语义）
 *   3 URL/路径解析（Cut 族）   6 配置项取值 + 10 万字段规模往返（打印耗时，Debug 计时仅供参考）
 *
 * @note 计时数字只用于确认"没有爆炸性劣化"；性能结论必须用 Release 基准（build-bench）测量。
 * @return 全部场景通过返回 0，否则返回 1。
 */
#include "go2cpp/runtime.hpp"
#include "go2cpp/strings.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

bool scenario_split_log_line() {
    std::clog << "[full][场景1] 日志行切分与字段清洗\n";
    using namespace go2cpp;
    const StringSlice fields = Split("  2026-10-15T18:30:00Z | WARN  | disk usage 91%  ", "|");
    const bool ok = fields.size() == 3 && TrimSpace(fields[1]) == "WARN" &&
                    TrimSuffix(TrimSpace(fields[0]), "Z") == "2026-10-15T18:30:00";
    std::cout << "  字段数=" << fields.size() << " level=\"" << TrimSpace(fields[1])
              << "\"（预期 3 / WARN）\n";
    return ok;
}

bool scenario_csv_round_trip() {
    std::clog << "[full][场景2] CSV 字段清洗与还原\n";
    using namespace go2cpp;
    const StringSlice raw = Split("alice, 30 , beijing", ",");
    StringSlice cleaned;
    for (const auto& cell : raw) cleaned.push_back(TrimSpace(cell));
    const std::string restored = Join(cleaned, ",");
    const bool ok = raw.size() == 3 && restored == "alice,30,beijing" && cleaned[1] == "30";
    std::cout << "  还原=\"" << restored << "\"（预期 alice,30,beijing）\n";
    return ok;
}

bool scenario_url_parsing() {
    std::clog << "[full][场景3] URL / 路径解析（Cut 族）\n";
    using namespace go2cpp;
    const auto scheme = Cut("https://example.com/a/b?x=1", "://");
    const auto query = Cut(scheme.after, "?");
    const auto trimmed = CutSuffix(query.before, "/b");
    const auto api = CutPrefix("/api/v1/users", "/api");
    const bool ok = scheme.found && scheme.before == "https" && query.found &&
                    query.before == "example.com/a/b" && query.after == "x=1" &&
                    trimmed.second && trimmed.first == "example.com/a" &&
                    api.second && api.first == "/v1/users";
    std::cout << "  scheme=" << scheme.before << " host+path=" << query.before
              << " query=" << query.after << " 去掉后缀=" << trimmed.first
              << "（预期 https / example.com/a/b / x=1 / example.com/a）\n";
    return ok;
}

bool scenario_header_compare() {
    std::clog << "[full][场景4] HTTP 头名大小写无关比较\n";
    using namespace go2cpp;
    const bool equal = EqualFold("Content-Type", "content-type") &&
                       !EqualFold("Content-Type", "content-encoding");
    const bool ordered = Compare("application/json", "text/plain") < 0 &&
                         Compare("a", "a") == 0 && Compare("b", "a") > 0;
    const bool any = ContainsAny("Content-Type", ":-/") && !ContainsAny("status", ":-/");
    const bool ok = equal && ordered && any;
    std::cout << "  大小写无关=" << equal << " 排序=" << ordered << " 字符集判定=" << any
              << "（预期 1 1 1）\n";
    return ok;
}

bool scenario_builder_utf8() {
    std::clog << "[full][场景5] Builder 与 UTF-8 rune 语义\n";
    using namespace go2cpp;
    StringBuilder builder;
    for (int i = 0; i < 1000; ++i) builder.WriteString("你");
    const std::size_t bytes = builder.Len();
    const std::string text = builder.String();
    const std::size_t runes = RuneCount(text);
    const bool ok = bytes == 3000 && runes == 1000 && Count(text, "") == 1001 &&
                    Contains(text, "你你你") && Split(text, "").size() == 1000;
    std::cout << "  字节=" << bytes << " rune=" << runes << " Count(\"\")=" << Count(text, "")
              << " Split(\"\")项数=" << Split(text, "").size()
              << "（预期 3000 / 1000 / 1001 / 1000）\n";
    return ok;
}

bool scenario_scale() {
    std::clog << "[full][场景6] 配置取值 + 10 万字段规模往返\n";
    using namespace go2cpp;
    const bool config_ok = TrimPrefix(TrimSpace("  retry=3  "), "retry=") == "3" &&
                           Trim("\"quoted\"", "\"") == "quoted" &&
                           Fields("  a=1\t b=2\n c=3  ").size() == 3;

    constexpr std::size_t kFields = 100000;
    StringSlice values;
    values.reserve(kFields);
    for (std::size_t i = 0; i < kFields; ++i) values.push_back(std::to_string(i % 1000));
    const auto started = Clock::now();
    const std::string joined = Join(values, ",");
    const StringSlice round_trip = Split(joined, ",");
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();

    std::int64_t mismatches = 0;
    for (std::size_t i = 0; i < kFields; i += 100) {
        if (round_trip[i] != std::to_string(i % 1000)) ++mismatches;
    }
    const bool scale_ok = round_trip.size() == kFields && mismatches == 0 &&
                          Count(joined, ",") == static_cast<std::int64_t>(kFields - 1);
    std::cout << "  配置取值=" << config_ok << " 往返字段数=" << round_trip.size()
              << " 抽查不一致=" << mismatches << " 耗时=" << elapsed_us
              << " µs（Debug 计时仅供参考）\n";
    return config_ok && scale_ok;
}

}  // namespace

/**
 * @brief strings 全面版：六个场景。
 * @return 全部通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_split_log_line() && ok;
    ok = scenario_csv_round_trip() && ok;
    ok = scenario_url_parsing() && ok;
    ok = scenario_header_compare() && ok;
    ok = scenario_builder_utf8() && ok;
    ok = scenario_scale() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
