/**
 * @file llm_client.hpp
 * @brief OpenAI 兼容的 Chat Completions 客户端（Qwen / DeepSeek / 任意兼容端点）。
 *
 * @details
 * 参考项目把「调用大模型」全部放在 Python 脚本 `test_ai_mcp.py` 里，C++ 侧只有
 * MCP 协议。本框架把 LLM 调用做成 C++ 模块，使 MCP 服务端可以自带一个
 * agent 循环（给 GMP 调试提供一个真实的「模型 + 工具」负载）。
 *
 * ## 传输选择
 * - `http://` 端点：走 Go2Cpp 的 `http::Client`（纯 C++，零外部进程）；
 * - `https://` 端点：Go2Cpp 当前没有可用的 HTTPS 客户端实现（`http2/tls_transport`
 *   未接入构建），因此通过系统 `curl` 子进程完成 TLS。这是**已知取舍**：
 *   接口保持在 `HttpPost()` 一处，将来 Go2Cpp 提供 TLS 时替换该函数即可。
 *
 * ## 密钥
 * 不写入代码或配置文件：按 `DASHSCOPE_API_KEY` → `QWEN_API_KEY` →
 * `DEEPSEEK_API_KEY` → `OPENAI_API_KEY` 的顺序读环境变量。
 */

#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace go2cpp::mcp::llm {

using json = nlohmann::json;

/// 客户端配置。
struct LlmConfig {
    std::string base_url = "https://api.deepseek.com/v1";
    std::string api_key;           ///< 为空时由 ApiKeyFromEnv() 填充
    std::string model = "deepseek-chat";
    int timeout_ms = 60000;        ///< 单次请求超时
};

/// 模型请求的一次工具调用。
struct ToolCall {
    std::string id;        ///< 工具调用 id，回传 tool 消息时必须一致
    std::string name;      ///< 函数名（对应 MCP 工具名）
    json arguments;        ///< 参数对象（模型给的是字符串，这里已解析）
};

/// 一次 chat/completions 的结果。
struct ChatResponse {
    bool ok = false;                       ///< HTTP + 业务是否都成功
    std::string error;                     ///< ok=false 时的错误描述
    std::string content;                   ///< 文本回复
    std::vector<ToolCall> tool_calls;      ///< 模型要求调用的工具
    json message = nullptr;                ///< 原始 message 对象（用于回填 messages）
};

/**
 * @brief OpenAI 兼容客户端。
 *
 * 只支持非流式 `POST {base_url}/chat/completions`；流式响应不在当前范围内。
 */
class LlmClient {
public:
    explicit LlmClient(LlmConfig config);

    /**
     * @brief 发起一次对话。
     * @param messages 完整的 messages 数组（由调用方维护历史）。
     * @param tools OpenAI tools 数组；传空数组表示不启用工具。
     */
    ChatResponse Chat(const json& messages, const json& tools) const;

    const LlmConfig& config() const noexcept { return m_config; }

    /// 按优先级从环境变量读取 API key；都没有时返回空串。
    static std::string ApiKeyFromEnv();

    /// 执行一次 JSON POST；https 走 curl，http 走 Go2Cpp。
    static bool HttpPost(const LlmConfig& config, const std::string& url, const std::string& body,
                         std::string& response_body, std::string& error);

private:
    LlmConfig m_config;
};

}  // namespace go2cpp::mcp::llm
