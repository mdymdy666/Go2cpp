/**
 * @file config.hpp
 * @brief 框架配置层：把 MCP 的可调参数接到 `go2cpp::config` 配置中心。
 *
 * @details
 * 旧实现把模型名、端点、超时散落在 `agent_demo.cpp` 的局部变量和
 * `llm_client.hpp` 的默认成员里，换模型要改代码。这里统一用
 * `GO2CPP_CONFIG_REGISTER` 在静态初始化期注册 `mcp.*` 配置项：
 *
 * - `mcp.llm.model` / `mcp.llm.base_url` / `mcp.llm.api_key` / `mcp.llm.timeout_ms`
 * - `mcp.server.name` / `mcp.server.version` / `mcp.server.workers`
 *
 * 取值优先级（从高到低）：
 * 1. 命令行参数（`--model` / `--base-url` 等，由应用层显式覆盖）；
 * 2. INI 配置文件（`mcp.ini`，由 `LoadConfigFile()` 加载）；
 * 3. 环境变量（仅 API key：`DEEPSEEK_API_KEY` 等）；
 * 4. 代码内默认值（DeepSeek 端点与 `deepseek-chat`）。
 *
 * 为什么不把 key 直接写在代码默认值里：默认值会进二进制，泄漏面更大；
 * 配置文件 `mcp/mcp.ini` 才是「写死」key 的位置，且方便换环境。
 */

#pragma once

#include <string>

namespace go2cpp::mcp::config {

/// DeepSeek 默认端点（OpenAI 兼容）。
inline constexpr const char* kDefaultBaseUrl = "https://api.deepseek.com/v1";
/// DeepSeek 默认对话模型。
inline constexpr const char* kDefaultModel = "deepseek-chat";

/// LLM 相关配置的结构化快照。
struct LlmSettings {
    std::string model = kDefaultModel;      ///< 模型名，对应 `mcp.llm.model`
    std::string base_url = kDefaultBaseUrl; ///< OpenAI 兼容端点（不含 /chat/completions）
    std::string api_key;                    ///< 为空时尝试环境变量
    int timeout_ms = 60000;                 ///< 单次请求超时（毫秒）
};

/// 服务端配置的结构化快照。
struct ServerSettings {
    std::string name = "go2cpp-mcp";  ///< initialize 响应的 serverInfo.name
    std::string version = "0.1.0";    ///< serverInfo.version
    int workers = 1;                  ///< 0/1 = 顺序处理，>1 = Fiber 并发
};

/// 监控工具配置的结构化快照（Manager 查询端点）。
struct MonitorSettings {
    /// `mcp.monitor.base_url`：Manager 基地址，默认本地 9101 端口。
    std::string base_url = "http://127.0.0.1:9101";
    /// `mcp.monitor.timeout_ms`：单次查询超时（毫秒）。
    int timeout_ms = 3000;
};

/// 注册全部 `mcp.*` 配置项。静态注册已经完成，本函数供测试/显式初始化调用，幂等。
void RegisterConfigVars();

/**
 * @brief 加载 INI 配置文件，覆盖已注册项的默认值。
 * @param path 文件路径；不存在时返回 false 并写 error（调用方可当作「使用默认值」）。
 * @return 加载成功返回 true。解析失败不会留下半套配置（配置中心事务保证）。
 */
bool LoadConfigFile(const std::string& path, std::string* error = nullptr);

/// 查找配置文件：优先 `MCP_CONFIG_FILE` 环境变量，否则当前目录 `mcp.ini`，
/// 最后尝试项目默认相对路径 `mcp/mcp.ini`。都不存在时返回空串。
std::string ResolveConfigFile();

/// 读取当前的 LLM 配置快照（INI 值 > 环境变量 > 默认值）。
LlmSettings LoadLlmSettings();

/// 读取当前的服务端配置快照。
ServerSettings LoadServerSettings();

/// 读取监控工具配置快照（Manager 地址与查询超时）。
MonitorSettings LoadMonitorSettings();

/// 按优先级从环境变量读取 API key；都没有时返回空串。
std::string ApiKeyFromEnv();

}  // namespace go2cpp::mcp::config
