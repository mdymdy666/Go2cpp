/**
 * @file config.cpp
 * @brief 配置层实现：静态注册 + INI 加载 + 优先级合并。
 */

#include "mcp/config/config.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "go2cpp/config/config.hpp"

// 静态注册：main 之前完成。module 与 key 拼成 `mcp.llm.model` 这样的完整名；
// 对应 INI 的 section 是第一个点之前的部分，即 `[mcp.llm]` 段的 `model`。
// 默认值按 DeepSeek；API key 默认留空，由 mcp.ini 或环境变量提供。
GO2CPP_CONFIG_REGISTER("mcp.llm", std::string, model, std::string(go2cpp::mcp::config::kDefaultModel),
                       "LLM 模型名，默认 deepseek-chat");
GO2CPP_CONFIG_REGISTER("mcp.llm", std::string, base_url, std::string(go2cpp::mcp::config::kDefaultBaseUrl),
                       "OpenAI 兼容端点，默认 https://api.deepseek.com/v1");
GO2CPP_CONFIG_REGISTER("mcp.llm", std::string, api_key, std::string(""),
                       "API key；为空时读取 DEEPSEEK_API_KEY 等环境变量");
GO2CPP_CONFIG_REGISTER("mcp.llm", int, timeout_ms, 60000, "LLM 单次请求超时（毫秒）");
GO2CPP_CONFIG_REGISTER("mcp.server", std::string, name, std::string("go2cpp-mcp"),
                       "MCP 服务名，出现在 initialize 响应");
GO2CPP_CONFIG_REGISTER("mcp.server", std::string, version, std::string("0.1.0"),
                       "MCP 服务版本");
GO2CPP_CONFIG_REGISTER("mcp.server", int, workers, 1,
                       "并发 Fiber 数：1 表示顺序处理");

namespace {

/**
 * @brief `mcp.monitor.*` 的静态注册载体。
 *
 * 不复用 `GO2CPP_CONFIG_REGISTER`：宏用 `key + __LINE__` 生成静态变量名，而
 * `base_url` / `timeout_ms` 已在 `mcp.llm` 下注册过，同一翻译单元里同名 key
 * 会撞变量名。`Config::Lookup` 的注册语义与宏相同（首次 Lookup 时按默认值
 * 创建），因此直接调用即可，同时保留「main 之前完成注册」的时机。
 */
struct MonitorConfigRegistrar {
    MonitorConfigRegistrar() {
        auto& config = go2cpp::config::Config::Instance();
        (void)config.Lookup<std::string>("mcp.monitor.base_url",
                                         "http://127.0.0.1:9101",
                                         "监控 Manager 基地址（os_monitor_* 工具查询用）");
        (void)config.Lookup<int>("mcp.monitor.timeout_ms", 3000,
                                 "监控查询超时（毫秒）");
    }
};

const MonitorConfigRegistrar g_monitor_config_registrar;

}  // namespace

namespace go2cpp::mcp::config {

namespace {

/// 读取一个字符串配置项；缺失时返回 fallback（不打印日志，调用方负责诊断）。
std::string ReadString(const std::string& name, const std::string& fallback) {
    auto variable = go2cpp::config::Config::Instance().Lookup<std::string>(name, fallback, "");
    return variable ? variable->GetValue() : fallback;
}

int ReadInt(const std::string& name, int fallback) {
    auto variable = go2cpp::config::Config::Instance().Lookup<int>(name, fallback, "");
    return variable ? variable->GetValue() : fallback;
}

}  // namespace

void RegisterConfigVars() {
    // 静态注册已经完成；这里显式 Lookup 一次，保证在静态初始化顺序异常
    // （例如单元测试里单独链接）时配置项也存在，且类型冲突时能尽早暴露。
    (void)ReadString("mcp.llm.model", kDefaultModel);
    (void)ReadString("mcp.llm.base_url", kDefaultBaseUrl);
    (void)ReadString("mcp.llm.api_key", "");
    (void)ReadInt("mcp.llm.timeout_ms", 60000);
    (void)ReadString("mcp.server.name", "go2cpp-mcp");
    (void)ReadString("mcp.server.version", "0.1.0");
    (void)ReadInt("mcp.server.workers", 1);
    (void)ReadString("mcp.monitor.base_url", "http://127.0.0.1:9101");
    (void)ReadInt("mcp.monitor.timeout_ms", 3000);
}

bool LoadConfigFile(const std::string& path, std::string* error) {
    if (path.empty()) {
        if (error != nullptr) {
            *error = "配置文件路径为空";
        }
        return false;
    }
    return go2cpp::config::Config::Instance().LoadFromFile(path, error);
}

std::string ResolveConfigFile() {
    if (const char* from_env = std::getenv("MCP_CONFIG_FILE"); from_env != nullptr && *from_env != '\0') {
        return from_env;
    }
    // 当前目录优先，便于「在构建目录里跑」和「在源码目录里跑」两种习惯。
    for (const char* candidate : {"mcp.ini", "mcp/mcp.ini"}) {
        if (FILE* probe = std::fopen(candidate, "r"); probe != nullptr) {
            std::fclose(probe);
            return candidate;
        }
    }
    return {};
}

std::string ApiKeyFromEnv() {
    for (const char* name : {"DEEPSEEK_API_KEY", "MCP_API_KEY", "DASHSCOPE_API_KEY",
                             "QWEN_API_KEY", "OPENAI_API_KEY"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0') {
            return value;
        }
    }
    return {};
}

LlmSettings LoadLlmSettings() {
    LlmSettings settings;
    settings.model = ReadString("mcp.llm.model", kDefaultModel);
    settings.base_url = ReadString("mcp.llm.base_url", kDefaultBaseUrl);
    settings.timeout_ms = ReadInt("mcp.llm.timeout_ms", 60000);
    if (settings.timeout_ms <= 0) {
        settings.timeout_ms = 60000;
    }
    settings.api_key = ReadString("mcp.llm.api_key", "");
    if (settings.api_key.empty()) {
        settings.api_key = ApiKeyFromEnv();
    }
    return settings;
}

ServerSettings LoadServerSettings() {
    ServerSettings settings;
    settings.name = ReadString("mcp.server.name", "go2cpp-mcp");
    settings.version = ReadString("mcp.server.version", "0.1.0");
    settings.workers = ReadInt("mcp.server.workers", 1);
    if (settings.workers < 1) {
        settings.workers = 1;
    }
    return settings;
}

MonitorSettings LoadMonitorSettings() {
    MonitorSettings settings;
    settings.base_url = ReadString("mcp.monitor.base_url", settings.base_url);
    settings.timeout_ms = ReadInt("mcp.monitor.timeout_ms", settings.timeout_ms);
    // 与工具 schema 的 [100, 60000] 保持一致：坏配置不炸服务，钳到合法区间。
    if (settings.timeout_ms < 100) {
        settings.timeout_ms = 100;
    }
    if (settings.timeout_ms > 60000) {
        settings.timeout_ms = 60000;
    }
    return settings;
}

}  // namespace go2cpp::mcp::config
