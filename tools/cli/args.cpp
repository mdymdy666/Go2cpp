/**
 * @file args.cpp
 * @brief `args.hpp` 的实现：线性扫描 + 严格错误。
 *
 * @details 解析顺序刻意保持「先语法、后语义」：扫描阶段只判断 token 形态
 *          （是不是选项、有没有值），扫描结束再检查 required 与位置参数数量。
 *          这样错误顺序稳定，同一份错误输入永远得到同一句提示。
 */

#include "args.hpp"

#include "cli.hpp"

#include <charconv>
#include <map>
#include <system_error>

namespace go2cpp::cli {
namespace {

/// 该 token 是否是「另一个选项」，用于避免把 `--pid --out f` 里的 `--out` 当成 pid 值。
bool LooksLikeOption(const std::string& token) {
    return token.size() >= 2 && token[0] == '-' && token[1] == '-';
}

[[noreturn]] void UsageError(const std::string& message) {
    throw CliError(ExitCode::kUsage, message);
}

}  // namespace

bool ParsedArgs::Has(std::string_view name) const {
    return values.find(name) != values.end();
}

std::string ParsedArgs::Get(std::string_view name) const {
    const auto it = values.find(name);
    return it == values.end() ? std::string() : it->second;
}

std::string ParsedArgs::GetOr(std::string_view name, std::string_view fallback) const {
    const auto it = values.find(name);
    return it == values.end() ? std::string(fallback) : it->second;
}

ParsedArgs ParseArgs(const std::vector<std::string>& args,
                     std::initializer_list<OptionSpec> specs,
                     std::size_t positional_min,
                     std::size_t positional_max) {
    ParsedArgs result;

    // 先把声明表建成「名字 → 声明」的索引；重名声明属于编程错误，直接暴露。
    std::map<std::string, const OptionSpec*, std::less<>> by_name;
    for (const OptionSpec& spec : specs) {
        const auto inserted = by_name.emplace(spec.name, &spec);
        if (!inserted.second) {
            throw CliError(ExitCode::kFailure,
                           "内部错误：选项 '" + spec.name + "' 在声明表中重复");
        }
    }

    bool after_double_dash = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& token = args[index];
        if (!after_double_dash) {
            if (token == "--") {
                after_double_dash = true;
                continue;
            }
            if (token == "-h" || token == "--help") {
                result.help_requested = true;
                return result;
            }
            if (StartsWith(token, "--")) {
                std::string name = token;
                std::string inline_value;
                bool has_inline_value = false;
                const auto equal = token.find('=');
                if (equal != std::string::npos) {
                    name = token.substr(0, equal);
                    inline_value = token.substr(equal + 1);
                    has_inline_value = true;
                }
                const auto spec_it = by_name.find(name);
                if (spec_it == by_name.end()) {
                    UsageError("未知选项 '" + name + "'；运行 `go2cpp-cli <command> --help` 查看可用选项");
                }
                if (result.values.find(name) != result.values.end()) {
                    UsageError("选项 '" + name + "' 重复出现");
                }
                const OptionSpec& spec = *spec_it->second;
                if (spec.metavar.empty()) {
                    if (has_inline_value) {
                        UsageError("选项 '" + name + "' 是开关，不接受值 '" + inline_value + "'");
                    }
                    result.values.emplace(name, std::string());
                    continue;
                }
                if (has_inline_value) {
                    result.values.emplace(name, inline_value);
                    continue;
                }
                if (index + 1 >= args.size()) {
                    UsageError("选项 '" + name + "' 缺少值（应为 <" + spec.metavar + ">）");
                }
                const std::string& next = args[index + 1];
                if (LooksLikeOption(next)) {
                    UsageError("选项 '" + name + "' 缺少值：下一个 token '" + next +
                               "' 看起来是另一个选项（应为 <" + spec.metavar + ">）");
                }
                result.values.emplace(name, next);
                ++index;
                continue;
            }
            if (token.size() > 1 && token[0] == '-') {
                UsageError("未知选项 '" + token + "'；本工具只支持长选项（形如 --pid）与 -h/--help");
            }
        }
        result.positional.push_back(token);
    }

    // 语法扫描完成后再检查 required，保证错误顺序可预测。
    for (const OptionSpec& spec : specs) {
        if (spec.required && result.values.find(spec.name) == result.values.end()) {
            UsageError("缺少必需选项 '" + spec.name + "'（<" + spec.metavar + ">）");
        }
    }

    if (result.positional.size() < positional_min) {
        UsageError("缺少位置参数：需要至少 " + std::to_string(positional_min) + " 个，实际 " +
                   std::to_string(result.positional.size()) + " 个");
    }
    if (positional_max != kUnlimitedPositional && result.positional.size() > positional_max) {
        UsageError("多余的参数 '" + result.positional[positional_max] + "'：该命令最多接受 " +
                   std::to_string(positional_max) + " 个位置参数");
    }
    return result;
}

int ParseIntOption(std::string_view text, std::string_view option, int min_value, int max_value) {
    if (text.empty()) {
        UsageError("选项 '" + std::string(option) + "' 需要整数值，实际为空");
    }
    int value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc() || parsed.ptr != end) {
        UsageError("选项 '" + std::string(option) + "' 的值 '" + std::string(text) +
                   "' 不是合法整数");
    }
    if (value < min_value || value > max_value) {
        UsageError("选项 '" + std::string(option) + "' 的值 " + std::to_string(value) +
                   " 超出范围 [" + std::to_string(min_value) + ", " + std::to_string(max_value) + "]");
    }
    return value;
}

}  // namespace go2cpp::cli
