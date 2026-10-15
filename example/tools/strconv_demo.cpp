/**
 * @file strconv_demo.cpp
 * @brief `go2cpp/strconv.hpp` 简单版：三个最常见的使用场景。
 *
 * @details 场景：
 *          ① 配置值解析与错误分类（Atoi/ParseBool + `Is(err, ErrSyntax())`）
 *          ② 数值格式化（Itoa/FormatInt/FormatUint/FormatBool）
 *          ③ 进制转换与 base=0 推断（"0755" → 493，Unix 权限位）
 * @return 全部校验通过返回 0，否则返回 1。
 */
#include "go2cpp/runtime.hpp"
#include "go2cpp/strconv.hpp"

#include <iostream>
#include <string>

namespace {

/// 场景 1：配置值解析与错误分类。
bool scenario_parse_config() {
    std::clog << "[strconv][场景1] 配置值解析与错误分类\n";
    using namespace go2cpp;

    const StrconvIntResult retry = Atoi("3");
    const StrconvBoolResult enabled = ParseBool("TRUE");
    const StrconvIntResult bad_number = Atoi("3x");
    const StrconvBoolResult bad_bool = ParseBool("yes");

    const bool ok = retry.ok() && retry.value == 3 && enabled.ok() && enabled.value &&
                    !bad_number.ok() && Is(bad_number.error, StrconvErrSyntax()) &&
                    !bad_bool.ok() && Is(bad_bool.error, StrconvErrSyntax());
    std::cout << "  retry=" << retry.value << " enabled=" << enabled.value
              << " 非法数字按语法错误判定=" << Is(bad_number.error, StrconvErrSyntax())
              << " 非法布尔=" << (!bad_bool.ok()) << "（预期 3 1 1 1）\n";
    return ok;
}

/// 场景 2：数值格式化。
bool scenario_format_values() {
    std::clog << "[strconv][场景2] 指标与日志的数值格式化\n";
    using namespace go2cpp;

    std::string line = "requests=";
    line += Itoa(1234567);
    line += " errors=";
    line += FormatUint(42U, 10);
    line += " mask=0x";
    line += FormatUint(255U, 16);
    line += " ratio=";
    line += FormatFloat(0.9876, 'f', 2, 64);
    line += " enabled=";
    line += FormatBool(true);

    const bool ok = line == "requests=1234567 errors=42 mask=0xff ratio=0.99 enabled=true";
    std::cout << "  " << line << "\n"
              << "  （预期 requests=1234567 errors=42 mask=0xff ratio=0.99 enabled=true）\n";
    return ok;
}

/// 场景 3：进制转换与 base=0 推断。
bool scenario_base_and_mode() {
    std::clog << "[strconv][场景3] 进制转换与 base=0 推断\n";
    using namespace go2cpp;

    const StrconvIntResult mode = ParseInt("0755", 0, 64);       // 前导 0 → 八进制
    const std::string mode_text = Itoa(mode.value);              // 493
    const std::string mode_octal = FormatInt(mode.value, 8);     // 755
    const StrconvUintResult hex = ParseUint("0xFF", 0, 64);
    const bool ok = mode.ok() && mode.value == 493 && mode_text == "493" &&
                    mode_octal == "755" && hex.ok() && hex.value == 255;
    std::cout << "  权限位 0755 十进制=" << mode_text << " 八进制回写=" << mode_octal
              << " 0xFF=" << hex.value << "（预期 493 / 755 / 255）\n";
    return ok;
}

}  // namespace

/**
 * @brief strconv 简单版：三个场景覆盖解析、格式化与进制。
 * @return 全部通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_parse_config() && ok;
    ok = scenario_format_values() && ok;
    ok = scenario_base_and_mode() && ok;

    std::clog << (ok ? "[strconv] 全部通过\n" : "[strconv] 有失败\n");
    return ok ? 0 : 1;
}
