/**
 * @file strconv_full.cpp
 * @brief `go2cpp/strconv.hpp` 全面版：六个场景覆盖解析、格式化、往返、进制、权限与边界。
 *
 * @details 场景：
 *   1 配置解析与错误分类（ErrSyntax 与 ErrRange 必须可区分）
 *   2 指标与日志格式化（Itoa/FormatInt/FormatUint/FormatFloat/FormatBool）
 *   3 JSON 数值的最短表示往返（FormatFloat('g',-1) → ParseFloat，位相等）
 *   4 十六进制 ID 转换（FormatUint ↔ ParseUint，含 0x 前缀自动推断）
 *   5 Unix 权限位（"0755" → 493 → "755"，base=0 推断）
 *   6 边界与规模（bitSize 钳制、下划线规则、10 万次往返；计时仅供参考）
 *
 * @note 计时数字只用于确认没有爆炸性劣化；性能结论必须用 Release 基准（build-bench）。
 * @return 全部场景通过返回 0，否则返回 1。
 */
#include "go2cpp/runtime.hpp"
#include "go2cpp/strconv.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

bool scenario_error_classification() {
    std::clog << "[full][场景1] 配置解析与错误分类\n";
    using namespace go2cpp;
    const StrconvIntResult overflow = ParseInt("999", 10, 8);       // 范围错误 + 钳制值
    const StrconvIntResult syntax = Atoi("12abc");                  // 语法错误
    const bool ok = !overflow.ok() && overflow.value == 127 &&
                    Is(overflow.error, StrconvErrRange()) &&
                    !Is(overflow.error, StrconvErrSyntax()) &&
                    !syntax.ok() && Is(syntax.error, StrconvErrSyntax());
    std::cout << "  越界值=" << overflow.value << " 判定为范围错误="
              << Is(overflow.error, StrconvErrRange()) << " 语法错误可区分="
              << (!Is(syntax.error, StrconvErrRange())) << "（预期 127 1 1）\n";
    return ok;
}

bool scenario_metrics_formatting() {
    std::clog << "[full][场景2] 指标与日志格式化\n";
    using namespace go2cpp;
    const bool ok = Itoa(-42) == "-42" && FormatInt(-255, 16) == "-ff" &&
                    FormatInt(255, 2) == "11111111" && FormatUint(255, 16) == "ff" &&
                    FormatBool(false) == "false" &&
                    FormatInt(std::numeric_limits<std::int64_t>::min(), 10) ==
                        "-9223372036854775808";
    std::cout << "  Itoa(-42)=" << Itoa(-42) << " hex(-255)=" << FormatInt(-255, 16)
              << " bin(255)=" << FormatInt(255, 2)
              << " INT64_MIN=" << FormatInt(std::numeric_limits<std::int64_t>::min(), 10)
              << "（预期 -42 / -ff / 11111111 / -9223372036854775808）\n";
    return ok;
}

bool scenario_json_round_trip() {
    std::clog << "[full][场景3] JSON 数值的最短表示往返\n";
    using namespace go2cpp;
    const double values[] = {3.14, 0.1, 1.0 / 3.0, 1e-300, 1e300, -2.5e-10};
    std::int64_t mismatches = 0;
    std::string sample;
    for (const double value : values) {
        const std::string text = FormatFloat(value, 'g', -1, 64);
        const StrconvFloatResult back = ParseFloat(text, 64);
        if (!back.ok() || std::memcmp(&back.value, &value, sizeof(double)) != 0) ++mismatches;
        if (sample.empty()) sample = text;
    }
    const bool ok = mismatches == 0 && sample == "3.14";
    std::cout << "  6 个值位相等往返不一致=" << mismatches << " FormatFloat(3.14)=" << sample
              << "（预期 0 / 3.14）\n";
    return ok;
}

bool scenario_hex_identifier() {
    std::clog << "[full][场景4] 十六进制 ID 转换\n";
    using namespace go2cpp;
    const std::uint64_t id = 0xDEADBEEFCAFEULL;
    const std::string hex = FormatUint(id, 16);
    const StrconvUintResult back = ParseUint(hex, 16, 64);
    const StrconvUintResult prefixed = ParseUint("0xDEADBEEF", 0, 64);
    const bool ok = hex == "deadbeefcafe" && back.ok() && back.value == id &&
                    prefixed.ok() && prefixed.value == 0xDEADBEEFULL;
    std::cout << "  hex=" << hex << " 回解一致=" << (back.value == id)
              << " 0xDEADBEEF=" << prefixed.value << "（预期 deadbeefcafe / 1 / 3735928559）\n";
    return ok;
}

bool scenario_unix_mode() {
    std::clog << "[full][场景5] Unix 权限位（base=0 八进制推断）\n";
    using namespace go2cpp;
    const StrconvIntResult mode = ParseInt("0755", 0, 64);
    const StrconvIntResult setgid = ParseInt("0o2755", 0, 64);
    const bool ok = mode.ok() && mode.value == 493 && FormatInt(mode.value, 8) == "755" &&
                    setgid.ok() && setgid.value == 1517 && FormatInt(setgid.value, 8) == "2755";
    std::cout << "  0755 → " << mode.value << "（八进制回写 " << FormatInt(mode.value, 8)
              << "）；0o2755 → " << setgid.value << "（八进制回写 "
              << FormatInt(setgid.value, 8) << "）（预期 493/755 与 1517/2755）\n";
    return ok;
}

bool scenario_boundaries_and_scale() {
    std::clog << "[full][场景6] 边界与规模\n";
    using namespace go2cpp;

    const bool underscore_ok = ParseInt("1_000", 0, 64).ok() &&
                               ParseInt("1_000", 0, 64).value == 1000 &&
                               !ParseInt("1_0", 10, 64).ok() && !ParseInt("1_", 0, 64).ok();
    const bool special_ok = std::isinf(ParseFloat("Infinity", 64).value) &&
                            std::isnan(ParseFloat("nan", 64).value) &&
                            ParseFloat("0x1p4", 64).value == 16.0 && !ParseFloat(" 1", 64).ok();
    const bool clamp_ok = ParseUint("18446744073709551616", 10, 64).value ==
                              std::numeric_limits<std::uint64_t>::max() &&
                          ParseInt("-129", 10, 8).value == -128;

    constexpr std::size_t kIterations = 100000;
    const auto started = Clock::now();
    std::int64_t mismatches = 0;
    for (std::size_t i = 0; i < kIterations; ++i) {
        const std::int64_t value = static_cast<std::int64_t>(i) * 7919 - 500000;
        if (Atoi(Itoa(value)).value != value) ++mismatches;
    }
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();

    const bool ok = underscore_ok && special_ok && clamp_ok && mismatches == 0;
    std::cout << "  下划线规则=" << underscore_ok << " 特殊浮点=" << special_ok
              << " 钳制值=" << clamp_ok << " " << kIterations << " 次往返不一致=" << mismatches
              << " 耗时=" << elapsed_us << " µs（Debug 计时仅供参考）\n";
    return ok;
}

}  // namespace

/**
 * @brief strconv 全面版：六个场景。
 * @return 全部通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_error_classification() && ok;
    ok = scenario_metrics_formatting() && ok;
    ok = scenario_json_round_trip() && ok;
    ok = scenario_hex_identifier() && ok;
    ok = scenario_unix_mode() && ok;
    ok = scenario_boundaries_and_scale() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
