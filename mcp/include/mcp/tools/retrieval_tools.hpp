/**
 * @file retrieval_tools.hpp
 * @brief 工具层：把检索层能力暴露成 MCP 工具（search_tools / knowledge_search / server_stats）。
 *
 * @details 这三个工具让模型可以「先找工具再调用」与「查资料再回答」，
 * 而不是一次性拿到全部工具描述与全部资源。它们依赖检索层实例，因此由应用层
 * 在装配阶段注册；内置工具（calculator 等）仍然通过 `RegisterBuiltinTools` 注册。
 */

#pragma once

#include "mcp/retrieval/document_store.hpp"
#include "mcp/retrieval/tool_retriever.hpp"
#include "mcp/server/registry.hpp"

namespace go2cpp::mcp::tools {

/**
 * @brief 注册检索相关工具。
 * @param retriever 工具检索器（生命周期必须长于注册表使用期）
 * @param documents 文档库
 */
void RegisterRetrievalTools(Registry& registry, retrieval::ToolRetriever& retriever,
                            retrieval::DocumentStore& documents);

}  // namespace go2cpp::mcp::tools
