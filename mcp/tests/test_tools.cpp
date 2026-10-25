/**
 * @file test_tools.cpp
 * @brief 内置工具单元测试：数学工具组、通知工具、提示参数校验。
 */

#include <cstdlib>
#include <string>

#include "test_util.hpp"

#include "mcp/tools/builtin_tools.hpp"

namespace {

/// 调用工具并返回 (is_error, text)。
std::pair<bool, std::string> CallTool(const go2cpp::mcp::Registry& registry, const std::string& name,
                                      const go2cpp::mcp::json& arguments) {
    go2cpp::mcp::ToolContext context;
    context.arguments = arguments;
    const auto result = registry.InvokeTool(name, context);
    if (!result) {
        return {true, "__missing__"};
    }
    return {result->is_error, result->content.empty() ? "" : result->content[0].text};
}

}  // namespace

int main() {
    using namespace go2cpp::mcp;

    Registry registry;
    tools::RegisterCalculatorTools(registry);
    tools::RegisterSleepTool(registry);
    tools::RegisterNotificationTools(registry);
    tools::RegisterBuiltinPrompts(registry);

    // 表达式
    CHECK_EQ(CallTool(registry, "calculator", {{"expression", "(12+8)*3"}}).second, "60");
    CHECK_EQ(CallTool(registry, "calculator", {{"expression", " 2.5 * 4 "}}).second, "10");
    CHECK(CallTool(registry, "calculator", {{"expression", "1+"}}).first);
    CHECK(CallTool(registry, "calculator", {{"expression", "(1+2"}}).first);
    CHECK(CallTool(registry, "calculator", {{"expression", ""}}).first);
    CHECK(CallTool(registry, "calculator", json::object()).first);

    // 二元运算
    CHECK_EQ(CallTool(registry, "add", {{"a", 17}, {"b", 25}}).second, "42");
    CHECK_EQ(CallTool(registry, "subtract", {{"a", 50}, {"b", 8}}).second, "42");
    CHECK_EQ(CallTool(registry, "multiply", {{"a", 6}, {"b", 7}}).second, "42");
    CHECK_EQ(CallTool(registry, "divide", {{"a", 84}, {"b", 2}}).second, "42");
    CHECK(CallTool(registry, "divide", {{"a", 1}, {"b", 0}}).first);
    CHECK(CallTool(registry, "add", {{"a", "x"}, {"b", 1}}).first);

    // 幂与根
    CHECK_EQ(CallTool(registry, "power", {{"base", 2}, {"exponent", 10}}).second, "1024");
    CHECK_EQ(CallTool(registry, "sqrt", {{"number", 144}}).second, "12");
    CHECK(CallTool(registry, "sqrt", {{"number", -4}}).first);
    CHECK_EQ(CallTool(registry, "power", {{"base", 2}, {"exponent", 0.5}}).second, "1.414213562");
    CHECK(CallTool(registry, "power", {{"base", -1}, {"exponent", 0.5}}).first);

    // 阶乘
    CHECK_EQ(CallTool(registry, "factorial", {{"n", 0}}).second, "1");
    CHECK_EQ(CallTool(registry, "factorial", {{"n", 20}}).second, "2432902008176640000");
    CHECK(CallTool(registry, "factorial", {{"n", 21}}).first);
    CHECK(CallTool(registry, "factorial", {{"n", -1}}).first);
    CHECK(CallTool(registry, "factorial", {{"n", 1.5}}).first);

    // sleep 参数校验（不真睡）
    CHECK(CallTool(registry, "sleep", {{"milliseconds", -1}}).first);
    CHECK(CallTool(registry, "sleep", {{"milliseconds", 60001}}).first);
    CHECK(CallTool(registry, "sleep", json::object()).first);

    // 通知工具：没有 progressToken 时 progress_test 报错
    CHECK(CallTool(registry, "progress_test", json::object()).first);

    // progressToken 存在时应成功
    {
        int notifications = 0;
        ToolContext context;
        context.meta = json{{"progressToken", "tok"}};
        context.notify = [&notifications](const std::string& method, json) {
            if (method == "notifications/progress") {
                ++notifications;
            }
        };
        const auto result = registry.InvokeTool("progress_test", context);
        CHECK(result.has_value());
        CHECK(!result->is_error);
        CHECK(notifications == 4);
    }

    // 提示参数校验
    {
        const auto ok = registry.GetPrompt("code-review", {{"language", "C++"}});
        CHECK(ok.has_value());
        const std::string text =
            ok->at("messages").at(0).at("content").at("text").get<std::string>();
        CHECK(text.find("C++") != std::string::npos);
    }
    bool threw = false;
    try {
        (void)registry.GetPrompt("code-review", json::object());
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    return go2cpp::mcp::test::Summary("tools");
}
