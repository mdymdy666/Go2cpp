/**
 * @file agent_demo.cpp
 * @brief 命令行 agent：用 DeepSeek（或任意 OpenAI 兼容端点）驱动 MCP 工具。
 *
 * 用法：
 *   ./mcp_agent --prompt "用 sqrt 算一下 144 的平方根，再用 add 加 6"
 *
 * 模型、端点、key 全部走配置层（`mcp.ini` > 环境变量 > 默认值），
 * 命令行只做覆盖。没有 key 时退出码为 2，不伪造模型输出。
 */

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

#include "mcp/app/application.hpp"

namespace {

void PrintUsage(const char* program) {
    std::printf("用法: %s --prompt <文本> [选项]\n", program);
    std::printf("  --prompt <text>        用户输入（必填）\n");
    std::printf("  --config <path>        配置文件（默认 mcp.ini）\n");
    std::printf("  --model <name>         覆盖模型名（默认 deepseek-chat）\n");
    std::printf("  --base-url <url>       覆盖 OpenAI 兼容端点\n");
    std::printf("  --max-iterations <n>   最大轮次（默认 8）\n");
    std::printf("  --no-retrieval         关闭工具检索，全量工具交给模型\n");
    std::printf("  -v, --verbose          打印每步工具调用\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string prompt;
    int max_iterations = 8;
    bool verbose = false;

    go2cpp::mcp::app::Builder builder;
    builder.WithVerbose(false);

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--prompt" && index + 1 < argc) {
            prompt = argv[++index];
        } else if (argument == "--config" && index + 1 < argc) {
            builder.WithConfigFile(argv[++index]);
        } else if (argument == "--model" && index + 1 < argc) {
            builder.WithModel(argv[++index]);
        } else if (argument == "--base-url" && index + 1 < argc) {
            builder.WithBaseUrl(argv[++index]);
        } else if (argument == "--max-iterations" && index + 1 < argc) {
            max_iterations = std::atoi(argv[++index]);
        } else if (argument == "--no-retrieval") {
            builder.WithRetrieval(false);
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

    if (prompt.empty()) {
        std::fprintf(stderr, "缺少 --prompt\n");
        PrintUsage(argv[0]);
        return 2;
    }

    std::string error;
    std::unique_ptr<go2cpp::mcp::app::Application> application = builder.Build(&error);
    if (!application) {
        std::fprintf(stderr, "初始化失败: %s\n", error.c_str());
        return 1;
    }
    if (verbose) {
        std::fprintf(stderr, "[agent] %s\n", application->Describe().c_str());
    }

    std::string answer;
    const int code = application->RunAgent(prompt, max_iterations, verbose, &answer);
    if (code == 0) {
        std::printf("%s\n", answer.c_str());
    }
    return code;
}
