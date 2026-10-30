/**
 * @file application.cpp
 * @brief 应用层实现：装配、运行 server / agent。
 */

#include "mcp/app/application.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "mcp/llm/agent.hpp"
#include "mcp/llm/llm_client.hpp"
#include "plugin/plugin_loader.hpp"
#include "mcp/tools/builtin_tools.hpp"
#include "mcp/tools/monitor_tools.hpp"
#include "mcp/tools/retrieval_tools.hpp"
#include "mcp/tools/socket_tools.hpp"
#include "transport/http_transport.hpp"
#include "transport/stdio_transport.hpp"

namespace go2cpp::mcp::app {

namespace {

/// 找一个存在的资源目录；都找不到返回空串。
std::string FindResourcesDirectory(const std::string& configured) {
    namespace fs = std::filesystem;
    if (!configured.empty()) {
        return fs::is_directory(configured) ? configured : std::string{};
    }
    for (const char* candidate : {"resources", "mcp/resources"}) {
        if (fs::is_directory(candidate)) {
            return candidate;
        }
    }
    return {};
}

}  // namespace

Application::Application(AppOptions options) : m_options(std::move(options)) {}

Application::~Application() = default;

std::unique_ptr<Application> Application::Create(AppOptions options, std::string* error) {
    auto application = std::unique_ptr<Application>(new Application(std::move(options)));

    // 向量层：默认用离线哈希编码器。换语义模型只需在这里替换实现，
    // 检索层与工具层完全不需要改动。
    application->m_embedding =
        std::make_unique<vector::HashEmbeddingProvider>(application->m_options.embedding_dimension);

    // 检索层先于工具层构造：ToolRetriever 在构造时订阅注册表的工具变更事件，
    // 之后每次 RegisterTool/UnregisterTool 都会增量更新索引，应用层不再需要
    // 在启动末尾做一次全量 Refresh()。retrieval=false 时不订阅，保持懒构建。
    retrieval::RetrieveOptions retrieve_options;
    retrieve_options.top_k = application->m_options.retrieval_top_k;
    application->m_retriever = std::make_unique<retrieval::ToolRetriever>(
        application->m_registry, *application->m_embedding, retrieve_options,
        application->m_options.retrieval);
    application->m_documents =
        std::make_unique<retrieval::DocumentStore>(*application->m_embedding);

    // 工具层：内置工具逐个注册，注册表事件把它们增量写入索引。
    tools::RegisterBuiltinTools(application->m_registry);

    const std::string resources = FindResourcesDirectory(application->m_options.resources_dir);
    if (!application->m_options.resources_dir.empty() && resources.empty()) {
        if (error != nullptr) {
            *error = "资源目录不存在: " + application->m_options.resources_dir;
        }
        return nullptr;
    }
    if (!resources.empty()) {
        std::string load_error;
        const std::size_t loaded = application->m_documents->AddDirectory(resources, load_error);
        if (!load_error.empty()) {
            // 文档索引失败不阻断服务：knowledge_search 会返回空结果并带 hint。
            std::fprintf(stderr, "文档索引失败: %s\n", load_error.c_str());
        } else {
            std::fprintf(stderr, "已索引 %zu 个文档文件（%zu 个片段）\n", loaded,
                         application->m_documents->size());
        }
    }

    tools::RegisterRetrievalTools(application->m_registry, *application->m_retriever,
                                  *application->m_documents);

    // 监控工具：构造注入 MonitorClient（默认 HttpMonitorClient），注册时不发起
    // 任何网络请求。Manager 没启动时服务照常可用，调用工具才会返回连接错误。
    tools::MonitorOptions monitor_options;
    monitor_options.base_url = application->m_options.monitor.base_url;
    monitor_options.timeout_ms = application->m_options.monitor.timeout_ms;
    tools::RegisterMonitorTools(
        application->m_registry,
        tools::MakeHttpMonitorClient(std::move(monitor_options)));

    // Socket 工具：数据源是进程内注册表（/proc/self/fd + TCP_INFO）；
    // 关闭仅限业务登记过的 fd，审计默认写 log/mcp-socket-audit.log。
    tools::RegisterSocketTools(application->m_registry);

    // 启动自检：订阅模式下索引应完整覆盖注册表。只有不一致（说明增量路径丢事件，
    // 例如 embedding 失败）时才回退到全量重建，正常启动不发生全量重算。
    if (application->m_options.retrieval) {
        const std::size_t indexed = application->m_retriever->IndexedTools();
        const std::size_t registered = application->m_registry.tool_count();
        if (indexed != registered && !application->m_retriever->Refresh()) {
            std::fprintf(stderr, "工具索引构建失败: %s\n",
                         application->m_retriever->LastError().c_str());
        }
    }
    return application;
}

void Application::RegisterTool(Tool tool) {
    // 不直接动索引：注册表事件监听器负责增量更新，保证「注册表变化 → 索引变化」
    // 只有一条通路，不会出现手工维护导致的双写不一致。
    m_registry.RegisterTool(std::move(tool));
}

bool Application::UnregisterTool(const std::string& name) {
    return m_registry.UnregisterTool(name);
}

int Application::RunServer() {
    ServerOptions server_options;
    server_options.name = m_options.server.name;
    server_options.version = m_options.server.version;
    server_options.workers = m_options.server.workers;
    server_options.verbose = m_options.verbose;

    // 先构造 Server、再构造插件加载器：局部对象逆序析构，加载器先析构，
    // 因此插件 Shutdown 里的通知最多被 Hub 丢弃，不会碰到已销毁的 Server；
    // 注册表由 Application 持有，比两者都活得久。
    Server server(server_options, m_registry);

    plugin::PluginLoader plugin_loader;
    const bool verbose = m_options.verbose;
    plugin_loader.SetNotificationSink(
        [&server, verbose](const std::string& plugin, const std::string& method,
                           const json& params) {
            if (verbose) {
                std::fprintf(stderr, "[plugin] %s -> %s\n", plugin.c_str(), method.c_str());
            }
            server.SendBroadcastNotification(method, params);
        });

    if (!m_options.plugins_dir.empty()) {
        std::string load_error;
        if (!plugin_loader.LoadDirectory(m_options.plugins_dir, m_registry, load_error)) {
            std::fprintf(stderr, "插件加载失败: %s\n", load_error.c_str());
            return 1;
        }
        std::fprintf(stderr, "已加载 %zu 个插件\n", plugin_loader.count());
    }

    if (m_options.http_port > 0) {
        HttpTransport::Options http_options;
        http_options.port = static_cast<std::uint16_t>(m_options.http_port);
        http_options.workers = m_options.server.workers > 1 ? m_options.server.workers : 4;
        HttpTransport http(server, http_options);
        std::uint16_t actual_port = 0;
        std::string start_error;
        if (!http.Start(actual_port, start_error)) {
            std::fprintf(stderr, "HTTP 传输启动失败: %s\n", start_error.c_str());
            return 1;
        }
        // HTTP 传输在 Start() 里注册定向/广播两个通知 sink，并在析构时置空，
        // 插件主动通知（SendBroadcastNotification）由它广播给所有活跃会话。
        std::fprintf(stderr, "HTTP 服务已启动: http://%s:%u (POST /mcp, GET /sse)\n",
                     http_options.host.c_str(), actual_port);
        http.Run();
        return 0;
    }

    StdioTransport transport;
    if (!transport.Start()) {
        std::fprintf(stderr, "传输启动失败\n");
        return 1;
    }
    return server.Serve(transport);
}

int Application::RunAgent(const std::string& prompt, int max_iterations, bool verbose,
                          std::string* output) {
    if (m_options.llm.api_key.empty()) {
        std::fprintf(stderr,
                     "缺少 API key：在 mcp.ini 的 mcp.llm.api_key 填写，或设置 "
                     "DEEPSEEK_API_KEY 环境变量\n");
        return 2;
    }

    llm::LlmConfig config;
    config.model = m_options.llm.model;
    config.base_url = m_options.llm.base_url;
    config.api_key = m_options.llm.api_key;
    config.timeout_ms = m_options.llm.timeout_ms;
    llm::LlmClient client(config);

    agent::AgentConfig agent_config;
    if (max_iterations > 0) {
        agent_config.max_iterations = max_iterations;
    }
    agent::Agent agent(std::move(client), m_registry, agent_config);
    if (m_options.retrieval && m_retriever) {
        agent.SetToolRetriever(m_retriever.get());
    }

    std::vector<agent::AgentStep> trace;
    try {
        const std::string answer = agent.Run(prompt, &trace);
        if (verbose) {
            for (const auto& step : trace) {
                std::fprintf(stderr, "[%s] %s%s\n", step.kind.c_str(),
                             step.name.empty() ? "" : (step.name + ": ").c_str(),
                             step.content.substr(0, 300).c_str());
            }
        }
        if (output != nullptr) {
            *output = answer;
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agent 失败: %s\n", error.what());
        return 1;
    }
}

std::string Application::Describe() const {
    std::string description;
    description += "model=" + m_options.llm.model;
    description += " base_url=" + m_options.llm.base_url;
    description += " api_key=" + std::string(m_options.llm.api_key.empty() ? "(未设置)" : "(已设置)");
    description += " workers=" + std::to_string(m_options.server.workers);
    description += " retrieval=" + std::string(m_options.retrieval ? "on" : "off");
    description += " tools=" + std::to_string(m_registry.tool_count());
    if (m_retriever) {
        description += " indexed=" + std::to_string(m_retriever->IndexedTools());
    }
    if (!m_options.config_file.empty()) {
        description += " config=" + m_options.config_file;
    }
    return description;
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

Builder& Builder::WithConfigFile(std::string path) {
    m_config_file = std::move(path);
    return *this;
}
Builder& Builder::WithModel(std::string model) {
    m_model = std::move(model);
    return *this;
}
Builder& Builder::WithBaseUrl(std::string url) {
    m_base_url = std::move(url);
    return *this;
}
Builder& Builder::WithApiKey(std::string api_key) {
    m_api_key = std::move(api_key);
    return *this;
}
Builder& Builder::WithServerName(std::string name) {
    m_server_name = std::move(name);
    return *this;
}
Builder& Builder::WithServerVersion(std::string version) {
    m_server_version = std::move(version);
    return *this;
}
Builder& Builder::WithWorkers(int workers) {
    m_workers = workers;
    return *this;
}
Builder& Builder::WithHttpPort(int port) {
    m_http_port = port;
    return *this;
}
Builder& Builder::WithPlugins(std::string directory) {
    m_plugins_dir = std::move(directory);
    return *this;
}
Builder& Builder::WithResources(std::string directory) {
    m_resources_dir = std::move(directory);
    return *this;
}
Builder& Builder::WithVerbose(bool verbose) {
    m_verbose = verbose;
    return *this;
}
Builder& Builder::WithRetrieval(bool enabled) {
    m_retrieval = enabled;
    return *this;
}
Builder& Builder::WithEmbeddingDimension(std::size_t dimension) {
    m_embedding_dimension = dimension;
    return *this;
}
Builder& Builder::WithRetrievalTopK(std::size_t top_k) {
    m_retrieval_top_k = top_k;
    return *this;
}

std::unique_ptr<Application> Builder::Build(std::string* error) {
    AppOptions options;

    std::string config_file = m_config_file;
    if (config_file.empty()) {
        config_file = config::ResolveConfigFile();
    }
    if (!config_file.empty()) {
        std::string load_error;
        if (!config::LoadConfigFile(config_file, &load_error)) {
            if (!m_config_file.empty()) {
                if (error != nullptr) {
                    *error = "加载配置 " + config_file + " 失败: " + load_error;
                }
                return nullptr;
            }
            // 自动发现的配置坏掉不致命：退回默认值并提示。
            std::fprintf(stderr, "配置 %s 加载失败: %s（继续用默认值）\n", config_file.c_str(),
                         load_error.c_str());
        } else {
            options.config_file = config_file;
        }
    }

    options.server = config::LoadServerSettings();
    if (!m_server_name.empty()) {
        options.server.name = m_server_name;
    }
    if (!m_server_version.empty()) {
        options.server.version = m_server_version;
    }
    if (m_workers > 0) {
        options.server.workers = m_workers;
    }

    options.llm = config::LoadLlmSettings();
    options.monitor = config::LoadMonitorSettings();
    if (!m_model.empty()) {
        options.llm.model = m_model;
    }
    if (!m_base_url.empty()) {
        options.llm.base_url = m_base_url;
    }
    if (!m_api_key.empty()) {
        options.llm.api_key = m_api_key;
    }

    options.plugins_dir = m_plugins_dir;
    options.resources_dir = m_resources_dir;
    options.http_port = m_http_port;
    options.verbose = m_verbose;
    options.retrieval = m_retrieval;
    options.embedding_dimension = m_embedding_dimension;
    options.retrieval_top_k = m_retrieval_top_k;

    return Application::Create(std::move(options), error);
}

}  // namespace go2cpp::mcp::app
