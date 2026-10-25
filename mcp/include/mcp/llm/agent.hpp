/**
 * @file agent.hpp
 * @brief 最小 agent 循环：LLM 与 MCP 工具之间的「对话 → 调用 → 回填」。
 *
 * @details
 * 参考项目的 agent 循环在 Python 脚本里（`test_ai_mcp.py`）：它把 tools/list
 * 的结果转成 DashScope function calling 的 schema，再手工维护 messages。
 * 这里把同一套逻辑放进 C++，好处是 MCP 服务端可以独立跑「模型 + 工具」链路，
 * 用于 GMP 场景下的真实负载（模型调用会 park、工具调用会占线程）。
 *
 * 循环约束：
 * - 最多 `max_iterations` 轮，超过后抛异常（防止模型反复调用工具）；
 * - 工具错误（isError=true）也会回填给模型，由模型决定是否重试；
 * - 工具不存在时同样回填错误文本，不中断整个循环。
 */

#pragma once

#include <string>
#include <vector>

#include "mcp/llm/llm_client.hpp"
#include "mcp/server/registry.hpp"

namespace go2cpp::mcp::retrieval {
class ToolRetriever;
}

namespace go2cpp::mcp::agent {

/// agent 运行配置。
struct AgentConfig {
    std::string system_prompt =
        "你是一个严谨的助手。需要外部数据时调用提供的工具，不要凭空编造结果。"
        "工具返回 isError=true 时如实说明错误。";
    int max_iterations = 8;  ///< 最大模型轮次
    /// 同一轮返回多个 tool_calls 时是否用 Go2Cpp 的 go()+Future 并行执行。
    bool parallel_tool_calls = true;
};

/// 一步执行记录，便于日志与测试断言。
struct AgentStep {
    std::string kind;      ///< "assistant" 或 "tool"
    std::string name;      ///< 工具名（assistant 步为空）
    std::string content;   ///< 文本或工具结果 JSON
};

/**
 * @brief 把 MCP 注册表作为工具集暴露给 LLM 的 agent。
 *
 * 线程模型：`Run()` 在调用线程内串行完成；并发场景下每个 agent 实例互不共享
 * 可变状态（Registry 自身线程安全）。
 */
class Agent {
public:
    Agent(llm::LlmClient client, Registry& registry, AgentConfig config = {});

    /**
     * @brief 运行一次完整对话。
     * @param user_prompt 用户输入。
     * @param trace 可选输出：按顺序记录模型回复与工具调用。
     * @return 模型的最终文本回复。
     * @throws std::runtime_error 模型请求失败或超过迭代上限。
     */
    std::string Run(const std::string& user_prompt, std::vector<AgentStep>* trace = nullptr);

    /// 可选：接入检索层，只把 top-k 相关工具交给模型（RAG 选工具）。
    void SetToolRetriever(retrieval::ToolRetriever* retriever) { m_retriever = retriever; }

private:
    /// 把注册表里的工具转成 OpenAI tools 数组。
    json BuildTools() const;

    /// 检索子集：用 ToolRetriever 选 top-k 工具，并保证检索工具自身可见。
    json BuildToolsForPrompt(const std::string& prompt) const;

    /// 执行一次工具调用，返回可回填的 JSON 文本。
    std::string ExecuteTool(const llm::ToolCall& call);

    llm::LlmClient m_client;
    Registry& m_registry;
    AgentConfig m_config;
    retrieval::ToolRetriever* m_retriever = nullptr;
};

}  // namespace go2cpp::mcp::agent
