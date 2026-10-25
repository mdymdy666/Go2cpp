/**
 * @file agent.cpp
 * @brief agent 循环实现。
 */

#include "mcp/llm/agent.hpp"

#include <stdexcept>
#include <string>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "go2cpp/concurrency/future.hpp"
#include "go2cpp/tools/go.hpp"
#include "mcp/retrieval/tool_retriever.hpp"

namespace go2cpp::mcp::agent {

Agent::Agent(llm::LlmClient client, Registry& registry, AgentConfig config)
    : m_client(std::move(client)), m_registry(registry), m_config(std::move(config)) {}

std::string Agent::ExecuteTool(const llm::ToolCall& call) {
    ToolContext context;
    context.arguments = call.arguments.is_object() ? call.arguments : json::object();
    context.notify = nullptr;

    const auto result = m_registry.InvokeTool(call.name, context);
    if (result) {
        return result->ToJson().dump();
    }
    return json{{"isError", true},
                {"content", json::array({json{{"type", "text"},
                                              {"text", "unknown tool: " + call.name}}})}}
        .dump();
}

json Agent::BuildTools() const {    const json listed = m_registry.ToolList();
    json tools = json::array();
    for (const auto& tool : listed["tools"]) {
        json function = json::object();
        function["name"] = tool["name"];
        function["description"] = tool.value("description", "");
        function["parameters"] = tool.value("inputSchema", json{{"type", "object"}});
        json entry = json::object();
        entry["type"] = "function";
        entry["function"] = std::move(function);
        tools.push_back(std::move(entry));
    }
    return tools;
}

json Agent::BuildToolsForPrompt(const std::string& prompt) const {
    if (m_retriever == nullptr) {
        return BuildTools();
    }
    const json listed = m_registry.ToolList();
    std::unordered_map<std::string, json> by_name;
    for (const auto& tool : listed["tools"]) {
        by_name.emplace(tool.value("name", ""), tool);
    }

    json tools = json::array();
    const auto append = [&tools](const json& tool) {
        json function = json::object();
        function["name"] = tool["name"];
        function["description"] = tool.value("description", "");
        function["parameters"] = tool.value("inputSchema", json{{"type", "object"}});
        json entry = json::object();
        entry["type"] = "function";
        entry["function"] = std::move(function);
        tools.push_back(std::move(entry));
    };

    for (const auto& item : m_retriever->ContextFor(prompt)) {
        const auto found = by_name.find(item.value("name", ""));
        if (found != by_name.end()) {
            append(found->second);
        }
    }
    // 检索工具自身必须常驻：否则模型无法在候选集不够时扩大范围。
    for (const char* name : {"search_tools", "knowledge_search"}) {
        const auto found = by_name.find(name);
        if (found == by_name.end()) {
            continue;
        }
        bool present = false;
        for (const auto& tool : tools) {
            if (tool["function"]["name"] == name) {
                present = true;
                break;
            }
        }
        if (!present) {
            append(found->second);
        }
    }
    if (tools.empty()) {
        return BuildTools();
    }
    return tools;
}

std::string Agent::Run(const std::string& user_prompt, std::vector<AgentStep>* trace) {
    const json tools = m_retriever != nullptr ? BuildToolsForPrompt(user_prompt) : BuildTools();

    json messages = json::array();
    messages.push_back(json{{"role", "system"}, {"content", m_config.system_prompt}});
    messages.push_back(json{{"role", "user"}, {"content", user_prompt}});

    std::string last_text;
    for (int iteration = 0; iteration < m_config.max_iterations; ++iteration) {
        const llm::ChatResponse response = m_client.Chat(messages, tools);
        if (!response.ok) {
            throw std::runtime_error("LLM 请求失败: " + response.error);
        }

        // 原样回填 assistant 消息（含 tool_calls），OpenAI 兼容端点要求如此。
        if (!response.message.is_null()) {
            messages.push_back(response.message);
        }

        if (trace != nullptr) {
            trace->push_back(AgentStep{"assistant", "", response.content});
        }
        last_text = response.content;

        if (response.tool_calls.empty()) {
            return response.content;
        }

        // 一轮里的多个 tool_call：默认并行执行（go() + Promise/Future），
        // 再按原顺序回填消息——模型要求 tool_call_id 与结果一一对应。
        if (m_config.parallel_tool_calls && response.tool_calls.size() > 1) {
            std::vector<go2cpp::Future<std::string>> futures;
            futures.reserve(response.tool_calls.size());
            for (const auto& call : response.tool_calls) {
                // std::function 要求目标可拷贝，Promise 只能移动——用 shared_ptr 包一层。
                auto promise = std::make_shared<go2cpp::Promise<std::string>>();
                futures.push_back(promise->GetFuture());
                go2cpp::go([this, call, promise]() mutable {
                    promise->SetValue(ExecuteTool(call));
                });
            }

            for (std::size_t index = 0; index < response.tool_calls.size(); ++index) {
                const auto& call = response.tool_calls[index];
                const auto result = futures[index].GetResult();
                const std::string content = result.Ok() ? *result.value : std::string("tool failed");
                if (trace != nullptr) {
                    trace->push_back(AgentStep{"tool", call.name, content});
                }
                messages.push_back(json{{"role", "tool"},
                                        {"tool_call_id", call.id},
                                        {"content", content}});
            }
            continue;
        }

        for (const auto& call : response.tool_calls) {
            const std::string content = ExecuteTool(call);
            if (trace != nullptr) {
                trace->push_back(AgentStep{"tool", call.name, content});
            }
            messages.push_back(json{{"role", "tool"},
                                    {"tool_call_id", call.id},
                                    {"content", std::move(content)}});
        }
    }

    throw std::runtime_error("agent 超过最大迭代次数（" +
                             std::to_string(m_config.max_iterations) + "）");
}

}  // namespace go2cpp::mcp::agent
