/**
 * @file application.hpp
 * @brief 应用层：把六层装配成可运行的 server / agent，并把 CLI 细节挡在外面。
 *
 * @details
 * 旧 `main.cpp` 直接 new Registry、new Transport、拼 ServerOptions，`agent_demo`
 * 又重复一遍「读环境变量 + 建 client + 建 agent」。这里把装配收进应用层：
 *
 * ```
 * Builder（链式配置 + 加载 ini + 命令行覆盖）
 *   └─ Application::Create()
 *        ├─ 向量层：HashEmbeddingProvider
 *        ├─ 检索层：ToolRetriever（订阅注册表事件）+ DocumentStore（加载 resources/）
 *        ├─ 工具层：RegisterBuiltinTools + RegisterRetrievalTools（每次注册增量入索引）
 *        └─ 校验层：由 Server 在 tools/call 前调用
 *   ├─ RunServer()：传输层 stdio / HTTP-SSE
 *   └─ RunAgent() ：LLM + 检索子集工具
 * ```
 *
 * 依赖方向：app → retrieval → vector；app → tools → validation；
 * app → transport。下层不反向依赖 app。
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "mcp/config/config.hpp"
#include "mcp/retrieval/document_store.hpp"
#include "mcp/retrieval/tool_retriever.hpp"
#include "mcp/server/registry.hpp"
#include "mcp/server/server.hpp"
#include "mcp/vector/embedding.hpp"

namespace go2cpp::mcp::app {

/// 应用配置（已合并命令行与 ini 的最终值）。
struct AppOptions {
    config::ServerSettings server;  ///< 服务名/版本/并发
    config::LlmSettings llm;        ///< 模型/端点/key/超时
    config::MonitorSettings monitor;  ///< Manager 地址/超时（os_monitor_* 工具）
    std::string config_file;        ///< 实际加载的 ini 路径（空 = 未加载）
    std::string plugins_dir;        ///< 插件目录（空 = 不加载）
    std::string resources_dir;      ///< 资源目录（空 = 不索引文档）
    int http_port = 0;              ///< >0 走 HTTP/SSE，否则 stdio
    bool verbose = false;           ///< 协议帧打到 stderr
    bool retrieval = true;          ///< agent 是否启用工具检索
    std::size_t embedding_dimension = 512;  ///< 向量维度
    std::size_t retrieval_top_k = 5;        ///< 检索返回条数
};

/**
 * @brief 装配好的应用实例。
 *
 * 生命周期：`Registry`、`ToolRetriever`、`DocumentStore` 都由本对象持有，
 * 且必须在 `Server` 之前构造、之后析构（工具回调持有它们的指针）。
 * 成员按「registry → embedding → retriever → documents」声明：逆序析构时
 * 检索器先退订监听器，注册表最后销毁，不会回调到已析构对象。
 */
class Application {
public:
    static std::unique_ptr<Application> Create(AppOptions options, std::string* error = nullptr);

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    ~Application();

    /// 运行 MCP 服务端（stdio 或 HTTP），阻塞到退出。
    int RunServer();

    /**
     * @brief 运行一次 agent 对话。
     * @param output 成功时写入最终回答
     * @return 0 成功；2 = 缺 API key；1 = 运行失败
     */
    int RunAgent(const std::string& prompt, int max_iterations, bool verbose, std::string* output);

    /**
     * @brief 运行时注册工具。
     * @details 只写注册表，索引由注册表事件监听器增量维护——与启动时注册走
     *          同一条通路，避免「注册表已变但索引没变」的双写不一致。
     */
    void RegisterTool(Tool tool);

    /// 运行时注销工具（索引条目由监听器同步删除）。
    bool UnregisterTool(const std::string& name);

    Registry& registry() { return m_registry; }
    retrieval::ToolRetriever* retriever() { return m_retriever.get(); }
    retrieval::DocumentStore* documents() { return m_documents.get(); }
    const AppOptions& options() const { return m_options; }

    /// 配置摘要（不含 API key），用于启动日志与 `server_stats` 工具。
    std::string Describe() const;

private:
    explicit Application(AppOptions options);

    AppOptions m_options;
    Registry m_registry;
    std::unique_ptr<vector::IEmbeddingProvider> m_embedding;
    std::unique_ptr<retrieval::ToolRetriever> m_retriever;
    std::unique_ptr<retrieval::DocumentStore> m_documents;
};

/**
 * @brief 链式装配器：`Builder().WithConfigFile("mcp.ini").WithHttpPort(8765).Build()`。
 *
 * @details 命令行覆盖优先于 ini；`Build()` 内部完成「查找/加载 ini → 合并设置 →
 * 创建 Application」，因此调用方不需要知道配置加载顺序。
 */
class Builder {
public:
    Builder& WithConfigFile(std::string path);
    Builder& WithModel(std::string model);
    Builder& WithBaseUrl(std::string url);
    Builder& WithApiKey(std::string api_key);
    Builder& WithServerName(std::string name);
    Builder& WithServerVersion(std::string version);
    Builder& WithWorkers(int workers);
    Builder& WithHttpPort(int port);
    Builder& WithPlugins(std::string directory);
    Builder& WithResources(std::string directory);
    Builder& WithVerbose(bool verbose);
    Builder& WithRetrieval(bool enabled);
    Builder& WithEmbeddingDimension(std::size_t dimension);
    Builder& WithRetrievalTopK(std::size_t top_k);

    /// 构建；失败返回 nullptr 并写 error。
    std::unique_ptr<Application> Build(std::string* error = nullptr);

private:
    std::string m_config_file;
    std::string m_model;
    std::string m_base_url;
    std::string m_api_key;
    std::string m_server_name;
    std::string m_server_version;
    std::string m_plugins_dir;
    std::string m_resources_dir;
    int m_workers = 0;
    int m_http_port = 0;
    bool m_verbose = false;
    bool m_retrieval = true;
    std::size_t m_embedding_dimension = 512;
    std::size_t m_retrieval_top_k = 5;
};

}  // namespace go2cpp::mcp::app
