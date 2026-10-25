/**
 * @file prompts.cpp
 * @brief 内置提示：code-review。
 *
 * @details 参考项目的提示参数校验在插件里手写，返回的 messages 结构也是手工拼的
 * JSON；这里把内容写成值对象，方便在测试里断言「参数缺失必须报 invalidParams」。
 */

#include "mcp/tools/builtin_tools.hpp"

#include <string>

namespace go2cpp::mcp::tools {

void RegisterBuiltinPrompts(Registry& registry) {
    Prompt prompt;
    prompt.name = "code-review";
    prompt.description = "生成一段代码审查指令";
    prompt.arguments = {
        PromptArgument{"language", "被审查代码的语言（如 C++、Go）", true},
    };
    prompt.handler = [](const json& arguments) {
        // 参数缺失时交给 server 转成 -32602：抛出 RpcException 会绕过工具层，
        // 这里保持普通异常，由 server 的 prompts/get 分支统一处理。
        const auto language = arguments.find("language");
        if (language == arguments.end() || !language->is_string() ||
            language->get<std::string>().empty()) {
            throw std::runtime_error("missing required prompt argument: language (string)");
        }

        json message = json::object();
        message["role"] = "user";
        message["content"] = json{{"type", "text"},
                                  {"text", "Please analyze code quality and suggest improvements "
                                           "of this code written in " +
                                               language->get<std::string>()}};

        json result = json::object();
        result["description"] = "this is the code review prompt";
        result["messages"] = json::array({message});
        return result;
    };
    registry.RegisterPrompt(std::move(prompt));
}

}  // namespace go2cpp::mcp::tools
