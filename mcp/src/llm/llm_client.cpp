/**
 * @file llm_client.cpp
 * @brief OpenAI 兼容客户端实现。
 */

#include "mcp/llm/llm_client.hpp"

#include <chrono>
#include <cstdlib>
#include <string>

#include "mcp/net/http_post.hpp"

namespace go2cpp::mcp::llm {

namespace {

/// 读取第一个非空环境变量。
std::string FirstEnv(const std::vector<const char*>& names) {
    for (const char* name : names) {
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0') {
            return value;
        }
    }
    return {};
}

}  // namespace

LlmClient::LlmClient(LlmConfig config) : m_config(std::move(config)) {
    if (m_config.api_key.empty()) {
        m_config.api_key = ApiKeyFromEnv();
    }
}

std::string LlmClient::ApiKeyFromEnv() {
    return FirstEnv({"DASHSCOPE_API_KEY", "QWEN_API_KEY", "DEEPSEEK_API_KEY", "OPENAI_API_KEY"});
}

bool LlmClient::HttpPost(const LlmConfig& config, const std::string& url, const std::string& body,
                         std::string& response_body, std::string& error) {
    // 传输细节统一在 go2cpp::mcp::net::HttpPostJson：https 走 curl，http 走 Go2Cpp。
    net::HttpPostOptions options;
    options.api_key = config.api_key;
    options.timeout = std::chrono::milliseconds(config.timeout_ms);
    return net::HttpPostJson(url, body, options, response_body, error);
}

ChatResponse LlmClient::Chat(const json& messages, const json& tools) const {
    ChatResponse response;
    if (m_config.api_key.empty()) {
        response.error = "缺少 API key（设置 DASHSCOPE_API_KEY / QWEN_API_KEY / "
                         "DEEPSEEK_API_KEY 之一）";
        return response;
    }

    json body = json::object();
    body["model"] = m_config.model;
    body["messages"] = messages;
    body["stream"] = false;
    if (!tools.is_null() && !tools.empty()) {
        body["tools"] = tools;
        body["tool_choice"] = "auto";
    }

    const std::string url = m_config.base_url + "/chat/completions";
    std::string raw;
    std::string error;
    if (!HttpPost(m_config, url, body.dump(), raw, error)) {
        response.error = std::move(error);
        return response;
    }

    json document;
    try {
        document = json::parse(raw);
    } catch (const json::exception& exception) {
        response.error = std::string("响应不是合法 JSON: ") + exception.what();
        return response;
    }

    const auto error_it = document.find("error");
    if (error_it != document.end() && !error_it->is_null()) {
        response.error = error_it->dump();
        return response;
    }

    const auto choices = document.find("choices");
    if (choices == document.end() || !choices->is_array() || choices->empty()) {
        response.error = "响应缺少 choices: " + raw.substr(0, 200);
        return response;
    }
    const auto message = choices->front().find("message");
    if (message == choices->front().end() || !message->is_object()) {
        response.error = "响应缺少 message";
        return response;
    }

    response.message = *message;
    if (message->contains("content") && (*message)["content"].is_string()) {
        response.content = (*message)["content"].get<std::string>();
    }

    const auto tool_calls = message->find("tool_calls");
    if (tool_calls != message->end() && tool_calls->is_array()) {
        for (const auto& call : *tool_calls) {
            const auto function = call.find("function");
            if (function == call.end()) {
                continue;
            }
            ToolCall parsed;
            parsed.id = call.value("id", "");
            parsed.name = function->value("name", "");
            const std::string arguments = function->value("arguments", "{}");
            try {
                parsed.arguments = json::parse(arguments);
            } catch (const json::exception&) {
                parsed.arguments = json{{"__raw_arguments", arguments}};
            }
            response.tool_calls.push_back(std::move(parsed));
        }
    }

    response.ok = true;
    return response;
}

}  // namespace go2cpp::mcp::llm
