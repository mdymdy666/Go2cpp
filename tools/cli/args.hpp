#pragma once

/**
 * @file args.hpp
 * @brief 零依赖命令行参数解析：`--name value` / `--name=value` / `--` 透传。
 *
 * @details 为什么不引入 CLI11 或 cxxopts：本工具的参数面很小（5 个子命令、
 *          每个 3~6 个选项），而外部依赖会进入 CMake 依赖链，违背「CLI target
 *          默认 OFF、不影响库构建」的约束。这里用一个 `OptionSpec` 描述表 +
 *          一次线性扫描实现，行为可预期、错误文案可控。
 *
 * ## 支持的语法
 *
 * - `--pid 1234` 与 `--pid=1234` 等价；
 * - `-h` / `--help`：请求帮助（由各子命令决定打印哪一份）；
 * - `--`：其后所有 token 原样进入位置参数（供 `replay record` 透传给目标程序）；
 * - 位置参数可以出现在选项之间（例如 `migrate restore file.json`）；
 * - 未知选项、重复选项、缺少值、位置参数数量不符，一律 `kUsage` 错误并指出
 *   具体 token，绝不静默忽略。
 *
 * ## 边界与陷阱
 *
 * - 不接受短选项聚合（`-abc`），也不接受单字母短选项（除 `-h`）——本工具没有
 *   短选项，猜测性兼容只会让错误更难懂。
 * - 选项值允许以 `-` 开头（`--out -` 表示 stdout），但不允许以下一个 `--xxx`
 *   开头：这几乎总是「漏写值」，提前报错比把下一个选项当值更安全。
 * - `Required` 检查发生在扫描之后，所以 `--pid` 缺失会同时列出用法提示而不是
 *   在扫描中途抛错。
 */

#include <cstddef>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::cli {

/**
 * @brief 一个选项的声明。
 * @details `metavar` 为空表示布尔开关（不带值）；非空表示需要值，并作为错误
 *          文案里的占位符（例如 `PID`、`FILE`）。
 */
struct OptionSpec {
    std::string name;     ///< 完整选项名，必须带 `--` 前缀，例如 `--pid`。
    std::string metavar;  ///< 值占位符；空 = 布尔开关。
    std::string help;     ///< 帮助文本（仅用于渲染，不参与解析）。
    bool required{false}; ///< 是否必须出现。
};

/**
 * @brief 解析结果。
 * @details 只保存原始字符串，类型转换由调用方完成（`ParseIntOption`），
 *          这样同一个解析器可以复用于 pid / gid / timeout 等不同范围。
 */
struct ParsedArgs {
    std::map<std::string, std::string, std::less<>> values;  ///< 选项名 → 原始值。
    std::vector<std::string> positional;                    ///< 位置参数。
    bool help_requested{false};                             ///< 出现 -h/--help。

    /// 选项是否出现。
    bool Has(std::string_view name) const;

    /// 取选项值；未出现时返回空串。
    std::string Get(std::string_view name) const;

    /// 取选项值；未出现时返回 `fallback`。
    std::string GetOr(std::string_view name, std::string_view fallback) const;
};

/**
 * @brief 解析 `args`（不含 argv[0]）。
 *
 * @param args 参数列表。
 * @param specs 选项声明表。
 * @param positional_min 位置参数下限。
 * @param positional_max 位置参数上限；`kUnlimitedPositional` 表示不限。
 * @return 解析结果；`help_requested == true` 时其余字段可能为空。
 * @throws CliError 任何用法错误（退出码 `kUsage`）。
 */
inline constexpr std::size_t kUnlimitedPositional = static_cast<std::size_t>(-1);

ParsedArgs ParseArgs(const std::vector<std::string>& args,
                     std::initializer_list<OptionSpec> specs,
                     std::size_t positional_min = 0,
                     std::size_t positional_max = 0);

/**
 * @brief 把选项值解析成限定范围内的整数。
 *
 * @details 只接受十进制整数字面量（可带负号，随后被范围检查拒绝），
 *          拒绝空串、尾随字符（`12x`）与溢出。错误信息包含选项名与原值，
 *          避免使用者面对「参数无效」这种无从下手的提示。
 *
 * @param text 原始值。
 * @param option 选项名（用于错误文案，例如 `--pid`）。
 * @param min_value 允许的最小值（含）。
 * @param max_value 允许的最大值（含）。
 * @return 解析后的整数。
 * @throws CliError `kUsage`：非法整数或越界。
 */
int ParseIntOption(std::string_view text, std::string_view option, int min_value, int max_value);

}  // namespace go2cpp::cli
