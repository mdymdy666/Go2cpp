/**
 * @file calculator.cpp
 * @brief 数学工具组：表达式求值 + 原项目的 add/subtract/multiply/divide/power/sqrt/factorial。
 *
 * @details
 * 参考项目里 8 个计算工具各自复制了一遍参数校验，且 `divide` 不检查除零、
 * `factorial` 用递归且不设上限。这里统一为：
 * - 一个表达式解析器（递归下降，支持 + - * / 与括号）；
 * - 一组二元/一元运算，共享同一套取参与报错逻辑；
 * - 明确的上限与定义域检查（除零、负数开方、阶乘溢出）。
 */

#include "mcp/tools/builtin_tools.hpp"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace go2cpp::mcp::tools {

namespace {

/// 递归下降表达式解析器。只接受十进制数字、四则运算与括号。
class ExpressionParser {
public:
    explicit ExpressionParser(const std::string& text) : m_text(text) {}

    double Parse() {
        const double value = ParseExpression();
        SkipSpaces();
        if (m_pos != m_text.size()) {
            throw std::runtime_error("unexpected character '" + std::string(1, m_text[m_pos]) +
                                     "' at offset " + std::to_string(m_pos));
        }
        if (!std::isfinite(value)) {
            throw std::runtime_error("result is not a finite number");
        }
        return value;
    }

private:
    double ParseExpression() {
        double value = ParseTerm();
        for (;;) {
            SkipSpaces();
            if (Match('+')) {
                value += ParseTerm();
            } else if (Match('-')) {
                value -= ParseTerm();
            } else {
                return value;
            }
        }
    }

    double ParseTerm() {
        double value = ParseFactor();
        for (;;) {
            SkipSpaces();
            if (Match('*')) {
                value *= ParseFactor();
            } else if (Match('/')) {
                const double divisor = ParseFactor();
                if (divisor == 0.0) {
                    throw std::runtime_error("division by zero at offset " + std::to_string(m_pos));
                }
                value /= divisor;
            } else {
                return value;
            }
        }
    }

    double ParseFactor() {
        SkipSpaces();
        if (Match('+')) {
            return ParseFactor();
        }
        if (Match('-')) {
            return -ParseFactor();
        }
        if (Match('(')) {
            const double value = ParseExpression();
            SkipSpaces();
            if (!Match(')')) {
                throw std::runtime_error("missing ')' at offset " + std::to_string(m_pos));
            }
            return value;
        }
        return ParseNumber();
    }

    double ParseNumber() {
        SkipSpaces();
        const std::size_t start = m_pos;
        bool has_digit = false;
        while (m_pos < m_text.size() &&
               (std::isdigit(static_cast<unsigned char>(m_text[m_pos])) || m_text[m_pos] == '.')) {
            has_digit = has_digit || std::isdigit(static_cast<unsigned char>(m_text[m_pos]));
            ++m_pos;
        }
        if (!has_digit) {
            throw std::runtime_error("expected a number at offset " + std::to_string(start));
        }
        try {
            return std::stod(m_text.substr(start, m_pos - start));
        } catch (const std::exception&) {
            throw std::runtime_error("invalid number at offset " + std::to_string(start));
        }
    }

    bool Match(char expected) {
        if (m_pos < m_text.size() && m_text[m_pos] == expected) {
            ++m_pos;
            return true;
        }
        return false;
    }

    void SkipSpaces() {
        while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos]))) {
            ++m_pos;
        }
    }

    const std::string& m_text;
    std::size_t m_pos = 0;
};

/// 取必需的数字参数；缺失或类型不对时抛出带参数名的异常。
double NumberArgument(const json& arguments, const char* name) {
    const auto it = arguments.find(name);
    if (it == arguments.end() || !it->is_number()) {
        throw std::runtime_error(std::string("缺少数字参数 ") + name);
    }
    return it->get<double>();
}

/// 数字转文本：整数不带小数点，其余保留 10 位有效数字。
std::string FormatNumber(double value) {
    if (std::fabs(value) < 1e-15) {
        value = 0.0;  // 消除 -0 与浮点残差
    }
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        return std::to_string(static_cast<long long>(value));
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    return buffer;
}

/// 二元运算工具的统一注册。
void RegisterBinaryTool(Registry& registry, const char* name, const char* description,
                        double (*operation)(double, double)) {
    Tool tool;
    tool.name = name;
    tool.description = description;
    tool.input_schema = json{{"type", "object"},
                             {"properties", {{"a", {{"type", "number"}}},
                                             {"b", {{"type", "number"}}}}},
                             {"required", json::array({"a", "b"})}};
    tool.handler = [operation](const ToolContext& context) {
        try {
            const double a = NumberArgument(context.arguments, "a");
            const double b = NumberArgument(context.arguments, "b");
            return ToolResult::Text(FormatNumber(operation(a, b)));
        } catch (const std::exception& error) {
            return ToolResult::Text(error.what(), true);
        }
    };
    registry.RegisterTool(std::move(tool));
}

double Add(double a, double b) { return a + b; }
double Subtract(double a, double b) { return a - b; }
double Multiply(double a, double b) { return a * b; }

double Divide(double a, double b) {
    if (b == 0.0) {
        throw std::runtime_error("division by zero");
    }
    return a / b;
}

}  // namespace

void RegisterCalculatorTools(Registry& registry) {
    // 表达式求值
    {
        Tool tool;
        tool.name = "calculator";
        tool.description = "计算四则运算表达式（支持 + - * / 与括号）";
        tool.input_schema = json{{"type", "object"},
                                 {"properties", {{"expression", {{"type", "string"},
                                                                 {"description", "待计算的表达式，如 (1+2)*3.5"}}}}},
                                 {"required", json::array({"expression"})}};
        tool.handler = [](const ToolContext& context) {
            const auto expression = context.arguments.find("expression");
            if (expression == context.arguments.end() || !expression->is_string()) {
                return ToolResult::Text("缺少字符串参数 expression", true);
            }
            try {
                ExpressionParser parser(expression->get<std::string>());
                return ToolResult::Text(FormatNumber(parser.Parse()));
            } catch (const std::exception& error) {
                return ToolResult::Text(error.what(), true);
            }
        };
        registry.RegisterTool(std::move(tool));
    }

    RegisterBinaryTool(registry, "add", "两个数相加", &Add);
    RegisterBinaryTool(registry, "subtract", "两个数相减", &Subtract);
    RegisterBinaryTool(registry, "multiply", "两个数相乘", &Multiply);
    RegisterBinaryTool(registry, "divide", "两个数相除（除数为 0 时报错）", &Divide);

    // power：base ^ exponent
    {
        Tool tool;
        tool.name = "power";
        tool.description = "计算 base 的 exponent 次方";
        tool.input_schema = json{{"type", "object"},
                                 {"properties", {{"base", {{"type", "number"}}},
                                                 {"exponent", {{"type", "number"}}}}},
                                 {"required", json::array({"base", "exponent"})}};
        tool.handler = [](const ToolContext& context) {
            try {
                const double base = NumberArgument(context.arguments, "base");
                const double exponent = NumberArgument(context.arguments, "exponent");
                const double value = std::pow(base, exponent);
                if (!std::isfinite(value)) {
                    throw std::runtime_error("result is not a finite number");
                }
                return ToolResult::Text(FormatNumber(value));
            } catch (const std::exception& error) {
                return ToolResult::Text(error.what(), true);
            }
        };
        registry.RegisterTool(std::move(tool));
    }

    // sqrt：平方根，负数报错
    {
        Tool tool;
        tool.name = "sqrt";
        tool.description = "计算平方根（负数报错）";
        tool.input_schema = json{{"type", "object"},
                                 {"properties", {{"number", {{"type", "number"}}}}},
                                 {"required", json::array({"number"})}};
        tool.handler = [](const ToolContext& context) {
            try {
                const double number = NumberArgument(context.arguments, "number");
                if (number < 0.0) {
                    throw std::runtime_error("sqrt of negative number");
                }
                return ToolResult::Text(FormatNumber(std::sqrt(number)));
            } catch (const std::exception& error) {
                return ToolResult::Text(error.what(), true);
            }
        };
        registry.RegisterTool(std::move(tool));
    }

    // factorial：非负整数，n <= 20（64 位内可精确表示）
    {
        Tool tool;
        tool.name = "factorial";
        tool.description = "计算非负整数 n 的阶乘（n <= 20）";
        tool.input_schema = json{{"type", "object"},
                                 {"properties", {{"n", {{"type", "integer"},
                                                        {"minimum", 0},
                                                        {"maximum", 20}}}}},
                                 {"required", json::array({"n"})}};
        tool.handler = [](const ToolContext& context) {
            const auto field = context.arguments.find("n");
            if (field == context.arguments.end() || !field->is_number_integer()) {
                return ToolResult::Text("缺少整数参数 n", true);
            }
            const long long n = field->get<long long>();
            if (n < 0) {
                return ToolResult::Text("n 不能为负数", true);
            }
            if (n > 20) {
                return ToolResult::Text("n 超过上限 20（结果超出 64 位范围）", true);
            }
            std::uint64_t value = 1;
            for (long long index = 2; index <= n; ++index) {
                value *= static_cast<std::uint64_t>(index);
            }
            return ToolResult::Text(std::to_string(value));
        };
        registry.RegisterTool(std::move(tool));
    }
}

}  // namespace go2cpp::mcp::tools
