/**
 * @file main.cpp
 * @brief mcp_server 入口：只做参数解析，装配全部交给应用层 Builder。
 *
 * @details 旧版本在这里直接构造 Registry / Transport / Server，`--plugins`
 * 与插件生命周期也散在 main 里。现在 main 的职责收敛为：
 * 1) 解析命令行 → 2) 覆盖配置 → 3) `app::Builder().Build()` → 4) `RunServer()`。
 * 新增传输或换装配方式只需要改应用层，不动入口。
 */

#include <cstdio>
#include <exception>
#include <memory>
#include <string>

#include "mcp/app/application.hpp"

namespace {

void PrintUsage(const char* program) {
    std::printf("用法: %s [选项]\n", program);
    std::printf("  --config <path>     配置文件（默认按 MCP_CONFIG_FILE / mcp.ini 查找）\n");
    std::printf("  --name <name>       服务名\n");
    std::printf("  --version <ver>     服务版本\n");
    std::printf("  --workers <n>       并发 Fiber 数（默认取配置，1 = 顺序）\n");
    std::printf("  --http <port>       以 HTTP/SSE 模式监听（POST /mcp, GET /sse）\n");
    std::printf("  --plugins <dir>     加载 mcp-extened 兼容的 .so 插件目录\n");
    std::printf("  --resources <dir>   文档资源目录（默认 resources/ 若存在）\n");
    std::printf("  --model <name>      覆盖模型名\n");
    std::printf("  --base-url <url>    覆盖 LLM 端点\n");
    std::printf("  --api-key <key>     覆盖 API key（不推荐写在命令行，会进 shell 历史）\n");
    std::printf("  --no-retrieval      关闭工具语义检索（全量工具交给模型）\n");
    std::printf("  --embedding-dim <n> 哈希向量维度（默认 512）\n");
    std::printf("  -v, --verbose       把协议帧打到 stderr\n");
    std::printf("  -h, --help          显示帮助\n");
}

/// 读取下一个参数；缺失时返回 false。
bool Next(int argc, char** argv, int& index, std::string& out) {
    if (index + 1 >= argc) {
        return false;
    }
    out = argv[++index];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    go2cpp::mcp::app::Builder builder;
    bool verbose = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        std::string value;
        if (argument == "--config" && Next(argc, argv, index, value)) {
            builder.WithConfigFile(value);
        } else if (argument == "--name" && Next(argc, argv, index, value)) {
            builder.WithServerName(value);
        } else if (argument == "--version" && Next(argc, argv, index, value)) {
            builder.WithServerVersion(value);
        } else if (argument == "--workers" && Next(argc, argv, index, value)) {
            try {
                builder.WithWorkers(std::stoi(value));
            } catch (const std::exception&) {
                std::fprintf(stderr, "--workers 需要正整数\n");
                return 2;
            }
        } else if (argument == "--http" && Next(argc, argv, index, value)) {
            try {
                builder.WithHttpPort(std::stoi(value));
            } catch (const std::exception&) {
                std::fprintf(stderr, "--http 需要端口号\n");
                return 2;
            }
        } else if (argument == "--plugins" && Next(argc, argv, index, value)) {
            builder.WithPlugins(value);
        } else if (argument == "--resources" && Next(argc, argv, index, value)) {
            builder.WithResources(value);
        } else if (argument == "--model" && Next(argc, argv, index, value)) {
            builder.WithModel(value);
        } else if (argument == "--base-url" && Next(argc, argv, index, value)) {
            builder.WithBaseUrl(value);
        } else if (argument == "--api-key" && Next(argc, argv, index, value)) {
            builder.WithApiKey(value);
        } else if (argument == "--no-retrieval") {
            builder.WithRetrieval(false);
        } else if (argument == "--embedding-dim" && Next(argc, argv, index, value)) {
            try {
                builder.WithEmbeddingDimension(static_cast<std::size_t>(std::stoi(value)));
            } catch (const std::exception&) {
                std::fprintf(stderr, "--embedding-dim 需要正整数\n");
                return 2;
            }
        } else if (argument == "-v" || argument == "--verbose") {
            verbose = true;
            builder.WithVerbose(true);
        } else if (argument == "-h" || argument == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", argument.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }

    std::string error;
    std::unique_ptr<go2cpp::mcp::app::Application> application = builder.Build(&error);
    if (!application) {
        std::fprintf(stderr, "启动失败: %s\n", error.c_str());
        return 1;
    }

    if (verbose) {
        std::fprintf(stderr, "[mcp] %s\n", application->Describe().c_str());
    }
    return application->RunServer();
}
