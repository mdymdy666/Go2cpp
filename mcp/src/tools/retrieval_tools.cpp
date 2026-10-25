/**
 * @file retrieval_tools.cpp
 * @brief 检索类工具实现。
 */

#include "mcp/tools/retrieval_tools.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "mcp/validation/validators.hpp"

namespace go2cpp::mcp::tools {

namespace {

/// 从参数里取字符串；缺失时返回空串（校验层通常已经拦过，这里只做防御）。
std::string StringArgument(const json& arguments, const char* key) {
    const auto found = arguments.find(key);
    if (found == arguments.end() || !found->is_string()) {
        return {};
    }
    return found->get<std::string>();
}

/// 取 top_k 并钳到 [1, 20]；缺失或非法用 fallback。
std::size_t TopKArgument(const json& arguments, std::size_t fallback) {
    const auto found = arguments.find("top_k");
    if (found == arguments.end() || !found->is_number_integer()) {
        return fallback;
    }
    const long long requested = found->get<long long>();
    return static_cast<std::size_t>(std::clamp<long long>(requested, 1, 20));
}

json ToolSearchSchema() {
    json schema = json::object();
    schema["type"] = "object";
    schema["properties"] = json{{"query", json{{"type", "string"}, {"minLength", 1},
                                                {"description", "要查找的工具用途，例如「算平方根」"}}},
                                {"top_k", json{{"type", "integer"}, {"minimum", 1}, {"maximum", 20}}}};
    schema["required"] = json::array({"query"});
    schema["additionalProperties"] = false;
    return schema;
}

json KnowledgeSearchSchema() {
    json schema = json::object();
    schema["type"] = "object";
    schema["properties"] = json{{"query", json{{"type", "string"}, {"minLength", 1}}},
                                {"top_k", json{{"type", "integer"}, {"minimum", 1}, {"maximum", 20}}}};
    schema["required"] = json::array({"query"});
    schema["additionalProperties"] = false;
    return schema;
}

json ServerStatsSchema() {
    json schema = json::object();
    schema["type"] = "object";
    schema["properties"] = json::object();
    schema["additionalProperties"] = false;
    return schema;
}

}  // namespace

void RegisterRetrievalTools(Registry& registry, retrieval::ToolRetriever& retriever,
                            retrieval::DocumentStore& documents) {
    {
        Tool tool;
        tool.name = "search_tools";
        tool.description =
            "按用途检索可用的 MCP 工具，返回工具名、描述与相似度。"
            "在不确定该用哪个工具时先调用它，再调用检索到的工具。";
        tool.input_schema = ToolSearchSchema();
        tool.handler = [&retriever](const ToolContext& context) {
            const std::string query = StringArgument(context.arguments, "query");
            const std::size_t top_k = TopKArgument(context.arguments, 5);
            if (query.empty()) {
                return ToolResult::Text("query 不能为空", true);
            }
            const auto items = retriever.Retrieve(query, top_k);
            json payload = json::array();
            for (const auto& item : items) {
                payload.push_back(json{{"name", item.id}, {"score", item.score}});
            }
            json result = json::object();
            result["query"] = query;
            result["count"] = payload.size();
            result["tools"] = std::move(payload);
            result["hint"] = items.empty() ? "未检索到相关工具，可放宽 query 或直接查看 tools/list"
                                           : "调用 tools/call 时使用 tools[].name";
            return ToolResult::Text(result.dump(2));
        };
        registry.RegisterTool(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "knowledge_search";
        tool.description =
            "在已索引的本地文档/资源中做语义检索，返回最相关的原文片段与来源 URI。"
            "回答需要引用资料的问题前先调用它。";
        tool.input_schema = KnowledgeSearchSchema();
        tool.handler = [&documents](const ToolContext& context) {
            const std::string query = StringArgument(context.arguments, "query");
            const std::size_t top_k = TopKArgument(context.arguments, 5);
            if (query.empty()) {
                return ToolResult::Text("query 不能为空", true);
            }
            const auto hits = documents.Search(query, top_k);
            json payload = json::array();
            for (const auto& hit : hits) {
                const auto chunk = documents.Get(hit.id);
                if (!chunk) {
                    continue;
                }
                payload.push_back(json{{"id", hit.id},
                                       {"uri", chunk->uri},
                                       {"score", hit.score},
                                       {"text", chunk->text}});
            }
            json result = json::object();
            result["query"] = query;
            result["count"] = payload.size();
            result["chunks"] = std::move(payload);
            if (payload.empty()) {
                result["hint"] = "文档库为空或无匹配；检查 resources 目录是否已索引";
            }
            return ToolResult::Text(result.dump(2));
        };
        registry.RegisterTool(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "server_stats";
        tool.description = "返回服务端统计：工具/提示/资源数量、工具索引大小、文档块数。";
        tool.input_schema = ServerStatsSchema();
        tool.handler = [&registry, &retriever, &documents](const ToolContext&) {
            json stats = json::object();
            stats["tools"] = registry.tool_count();
            stats["prompts"] = registry.prompt_count();
            stats["resources"] = registry.resource_count();
            stats["indexed_tools"] = retriever.IndexedTools();
            stats["document_chunks"] = documents.size();
            stats["retriever_status"] = retriever.LastError();
            return ToolResult::Text(stats.dump(2));
        };
        registry.RegisterTool(std::move(tool));
    }
}

}  // namespace go2cpp::mcp::tools
