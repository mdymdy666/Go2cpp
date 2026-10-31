/**
 * @file main.cpp
 * @brief web_server_full 入口：解析参数、加载配置、等待信号并优雅关闭。
 * @details 信号先屏蔽再启动 HttpServer：accept/worker 线程继承屏蔽字，
 *          只有主线程的 sigwait 能取到 SIGINT/SIGTERM，Stop 因此不会被
 *          信号打断，连接可以按 HttpServer 的关闭流程收尾。
 *
 *          运行（仓库根目录）：
 *            ./build/example/app/go2cpp_web_server_full
 *            ./go2cpp_web_server_full --config example/app/web_server_full/server.ini
 *          参数：
 *            --config <path>     INI 路径（默认 example/app/web_server_full/server.ini）
 *            --port <1..65535>   覆盖配置端口
 *            --web-root <path>   覆盖静态文件根目录
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "app_server.h"
#include "config.h"

#include <pthread.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace {

/// @brief 打印命令行用法。
void PrintUsage() {
    std::cout << "用法: go2cpp_web_server_full [--config <path>] [--port <1..65535>]\n"
                 "                                  [--web-root <path>]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "example/app/web_server_full/server.ini";
    std::string web_root_override;
    bool has_port_override = false;
    std::uint16_t port_override = 0;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return 0;
        }
        if (argument == "--config" && index + 1 < argc) {
            config_path = argv[++index];
            continue;
        }
        if (argument == "--web-root" && index + 1 < argc) {
            web_root_override = argv[++index];
            continue;
        }
        if (argument == "--port" && index + 1 < argc) {
            const int port = std::atoi(argv[++index]);
            if (port <= 0 || port > 65535) {
                std::cerr << "端口非法: " << argv[index] << "（期望 1..65535）\n";
                return 2;
            }
            port_override = static_cast<std::uint16_t>(port);
            has_port_override = true;
            continue;
        }
        std::cerr << "未知参数: " << argument << "\n";
        PrintUsage();
        return 2;
    }

    app::Config config = app::Config::Default();
    std::error_code file_error;
    if (std::filesystem::exists(config_path, file_error)) {
        std::string error;
        if (!config.LoadFromFile(config_path, &error)) {
            std::cerr << "[web_full] 配置加载失败: " << error << "\n";
            return 1;
        }
        std::cout << "[web_full] 已加载配置 " << config_path << "\n";
    } else {
        std::cout << "[web_full] 配置文件不存在，使用内置默认值: " << config_path << "\n";
    }
    if (has_port_override) {
        config.port = port_override;
    }
    if (!web_root_override.empty()) {
        config.web_root = web_root_override;
    }

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "[web_full] pthread_sigmask 失败\n";
        return 1;
    }

    app::Application application(config);
    std::string start_error;
    if (!application.Start(&start_error)) {
        std::cerr << "[web_full] 启动失败: " << start_error << "\n";
        return 1;
    }
    std::cout << "[web_full] 已启动 http://" << application.local_address() << "\n";
    std::cout << "[web_full] 健康检查 /health，SSE /events，路由 /api/routes\n";

    int signal_number = 0;
    if (sigwait(&signals, &signal_number) != 0) {
        std::cerr << "[web_full] sigwait 失败\n";
    }
    std::cout << "[web_full] 收到信号 " << signal_number << "，开始优雅关闭\n";
    application.Stop();
    std::cout << "[web_full] 已退出\n";
    return 0;
}
